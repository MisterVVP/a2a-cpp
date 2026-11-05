# Windows outbound HTTP reactor

The default libcurl HTTP client uses a shared socket-readiness reactor on Windows.
HTTP+JSON and JSON-RPC streaming retain their existing client and observer APIs.
Synchronous HTTP requests also use this reactor; asynchronous unary work can
share it without another scheduler. This backend handles outbound HTTP, while
inbound server sockets and gRPC have their own implementations.

## Decision and baseline

The previous Windows path called `curl_multi_poll()` followed by
`curl_multi_perform()`, with a one-second fallback when libcurl had no timer.
Commands used `curl_multi_wakeup()`. It already shared a bounded reactor pool;
the change makes Windows honor libcurl's multi-socket contract explicitly.

The backend uses `WSAPoll` over a growable vector of socket interests plus one
private loopback TCP wake connection. `WSAPoll` has no
`FD_SETSIZE`/`MAXIMUM_WAIT_OBJECTS` 64-socket restriction. Membership updates and
swap-removals use an index map in O(1) average time. Waiting and readiness
collection take O(number of registered sockets), without per-stream workers or
an imposed stream/handle ceiling. IOCP would require an overlapped-I/O adaptation
around libcurl's readiness contract; a wait set of WSA events would require
sharding around the handle limit. Neither is needed here.

`tests/curl_reactor_evidence.cpp` compares identical finite and idle-cancellation
workloads against the old fallback. The development-only
`A2A_TEST_CURL_PORTABLE_REACTOR` CMake option selects the unchanged previous path
on Windows and requires testing and libcurl. Production builds use the native
backend by default. There is no public client setting for this comparison.

## Ownership and wakeups

Each existing reactor shard owns one `CURLM`, its registrations, timer deadline,
easy-handle membership and one network thread. A process shares at most four
shards. The existing four dispatch workers serialize each stream's metadata,
chunks and terminal callback away from the network thread. Increasing the stream
count does not increase the SDK thread budget.

Add, Cancel, owner cancellation and Shutdown publish commands under the existing
queue mutex and coalesce a wake byte on the nonblocking wake connection. TCP_NODELAY
avoids delayed small writes. A full send buffer already represents a pending
wakeup. Other send failures shut down the writer, making the reader report
closure; wait/reader failures abort outstanding transfers and stop the shard.
The owner drains the reader and clears the pending-wake flag before swapping the
command queue, then handles commands before readiness events. A producer that
coalesces during draining is covered by that queue swap; a later producer sends
a new byte.
Wake bytes survive the interval between inspecting commands and entering a wait.

Only the owner changes socket registrations. Libcurl's socket and timer callbacks
update backend state and return; they never call another mutating `curl_multi_*`
operation. Allocation failures return callback failure for libcurl to observe
after unwinding. A generation accompanies every readiness snapshot, so commands,
timeout processing or another socket action can remove, change or reuse a socket
without delivering an obsolete event later in that batch.

An orderly Winsock `POLLHUP` is readable EOF, including any buffered final bytes.
It maps to `CURL_CSELECT_IN`; only `POLLERR` and `POLLNVAL` map to
`CURL_CSELECT_ERR`. Treating an ordinary FIN as an error can abort a successful
finite response before libcurl drains it.

The steady-clock deadline follows `CURLMOPT_TIMERFUNCTION`: negative cancels it,
zero schedules an immediate action after the callback returns, and positive
values replace the prior deadline. Wait durations round upward to milliseconds.
An unarmed deadline waits indefinitely. There is no recurring polling interval;
`curl_multi_socket_action(CURL_SOCKET_TIMEOUT)` runs when the current timer expires.
The consumed deadline is cleared before that action, allowing libcurl to arm its
successor safely.

Shutdown wakes and joins the owner before destroying the wake sockets. Libcurl
cleanup runs while the backend remains alive, since cleanup may invoke socket
callbacks. The backend balances its Winsock startup reference and closes its
private non-inheritable sockets; libcurl continues to own the network sockets. Existing transfer
lifetime and completion arbitration remain shared across platforms. Linux's
`epoll`/`eventfd`/`timerfd` implementation is unchanged.

## Validation and reproduction

### Build and test on Windows

Install Visual Studio 2022 with **Desktop development with C++**, CMake, Git,
and Python 3. Run these commands from the repository in Developer PowerShell for
VS 2022. PowerShell 7 is recommended. Fetch the PR branch and select your vcpkg
installation; the first CMake configure installs its pinned manifest dependencies.

```powershell
git fetch origin
git switch perf/windows-curl-readiness-232
git pull --ff-only

$env:VCPKG_ROOT = 'C:/vcpkg' # Adjust if vcpkg is installed elsewhere.
if (-not (Test-Path "$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake")) {
    git clone https://github.com/microsoft/vcpkg.git $env:VCPKG_ROOT
}
& "$env:VCPKG_ROOT/bootstrap-vcpkg.bat"

cmake -S . -B build-windows -G 'Visual Studio 17 2022' -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
    "-DVCPKG_OVERLAY_TRIPLETS=$PWD/triplets" `
    -DVCPKG_TARGET_TRIPLET=ci-x64-windows-release `
    -DVCPKG_HOST_TRIPLET=ci-x64-windows-release `
    '-DVCPKG_MANIFEST_FEATURES=signing;asio' `
    -DA2A_ENABLE_TESTING=ON -DA2A_ENABLE_LIBCURL=ON `
    -DA2A_ENABLE_SIGNING=ON -DA2A_ENABLE_ASIO=ON

cmake --build build-windows --config RelWithDebInfo `
    --target curl_reactor_test curl_reactor_evidence --parallel
ctest --test-dir build-windows -C RelWithDebInfo --output-on-failure `
    --no-tests=error -R 'CurlReactor|CurlWindowsReadiness|CurlStreamingTransport'
```

This focused run includes the EOF/hangup mapping, timer, cancellation, shutdown,
scale, synchronous unary, and real HTTP+JSON/JSON-RPC streaming checks. Winsock
can return from a timed wait before the steady-clock deadline; the timer test
waits for the actual deadline, as the reactor does, without assuming one system
call has millisecond-exact timing.

To run the complete SDK suite with the same platform exclusions as Windows CI:

```powershell
cmake --build build-windows --config RelWithDebInfo --parallel
ctest --test-dir build-windows -C RelWithDebInfo --output-on-failure --no-tests=error `
    -E '^(sut_entrypoints_test|performance_runner_script_test|run_examples_script_test)$'
```

The excluded script fixtures require POSIX tooling or `compile_commands.json`,
which the Visual Studio generator does not produce. TCK compliance is separate.

### Compare the native and fallback reactors

Keep the machine otherwise idle during latency measurements. From the configured
build above, run this in Developer PowerShell (the VS environment supplies
`dumpbin` for the symbol tests):

```powershell
python scripts/investigate_curl_reactor.py --build-dir build-windows `
    --configuration RelWithDebInfo --output-dir reactor-investigation
if ($LASTEXITCODE -ne 0) { throw 'Evidence collection failed' }
Get-Content reactor-investigation/summary.md
```

The runner builds and snapshots **both** executables before measuring either,
restores the native build even if baseline preparation fails, and measures both
native-first and fallback-first. It retains the commands, CPU/OS, common CMake
options, executable/DLL hashes, compiler/libcurl versions, timer period, and
loopback TIME_WAIT counts. Each order has three matched scenarios:

- `fresh`: the unchanged acceptance workload, twenty repetitions, all stream
  counts and both completion/cancellation modes, without diagnostic timestamps.
- `traced-fresh`: five repetitions of finite workloads with per-request timing.
- `traced-reused`: the same diagnostic workload with one client reused at each
  stream count and one excluded warmup repetition retained in the raw trace.

`summary.md` includes every strict failure and diagnostic latency distributions.
A zero runner exit code means collection and data validation succeeded; it does
**not** mean strict performance acceptance passed. Check each `strict.md`, or
run the comparator directly (a failing strict comparison returns nonzero):

```powershell
python scripts/compare_curl_reactor_evidence.py `
    reactor-investigation/native-first/fresh/fallback.json `
    reactor-investigation/native-first/fresh/native.json --output strict.md
python scripts/compare_curl_reactor_evidence.py `
    reactor-investigation/fallback-first/fresh/fallback.json `
    reactor-investigation/fallback-first/fresh/native.json --output strict-reversed.md
```

The portable flag is the only backend build difference. Both snapshots use the
same configured dependencies and compiler options. Diagnostic runs also verify
matching compiler/libcurl, timer period, hardware thread count and client lifetime.
Do not compare traced results with untraced ones as acceptance evidence.

Windows CI runs these equivalent workloads and the mandatory gRPC and HTTP+JSON
performance smoke. It publishes raw JSON and a Markdown comparison in
`windows-curl-reactor-evidence`. The report records first-event p95,
finite-completion/cancellation p95 and p99, samples, errors and added process
threads for 1/16/64/96 streams over twenty repetitions. One server worker serves
all sockets. Cancellation timing starts after the first event establishes each
stream and ends at dispatch completion, while peers remain idle. Finite timing
starts before releasing all established server connections. First-event timing
includes submission, shard startup, TCP setup, server scheduling and callback
dispatch. A fresh client does not guarantee a uniformly cold pool: asynchronous
cleanup and idle-slot retention can overlap the next repetition. Record compiler, libcurl version,
OS and build configuration when comparing results outside CI.
The fixture sends the headers and first SSE chunk in one write, avoiding a
separate tiny body write behind the headers. This does not isolate TCP from
other startup or scheduling effects.
The evidence executable requests a 1 ms Windows process timer period with
`timeBeginPeriod`, balances it with `timeEndPeriod` on exit, and records
`timer_period_ms` in each row. Both backends use this same setting; the
comparison rejects mismatched periods. The SDK itself does not request a timer
resolution change. An earlier native CI run observed about 16 ms at one stream on both backends
with the default timer period; that observation alone does not establish the
cause of higher-concurrency tails. Requesting 1 ms did not establish strict
latency parity. Twenty repetitions reduce dependence on a single sample.

Both comparison profiles require zero errors, complete/equal sample counts,
matching timer periods, and at most eight added SDK threads. The default
`--profile strict` is intended for local measurements: cancellation p95 below
10 ms and p99 below 20 ms, with a regression budget of 25% or 2 ms above the
equivalent fallback, whichever is larger.

Windows CI uses `--profile cloud` to accommodate shared-runner scheduling:
cancellation p95 below 50 ms and p99 below 100 ms, with a relative budget of
2.5 times the fallback or 20 ms above it, whichever is larger. The report names
the selected profile and its thresholds. To reproduce CI's acceptance locally,
append `--profile cloud` to the comparison command above. Large regressions,
errors, incomplete workloads and thread-budget violations still fail CI.
A failure retains the raw evidence. Cloud budgets are a coarse regression
guard; deployment-hardware measurements establish local performance parity.

Deterministic tests cover disarm/rearm/immediate timers, retained and concurrent
wakeups, readiness-interest changes, stale registrations, closure, idle Add and
Cancel, repeated/concurrent cancellation, completion/cancellation races,
start/shutdown races, peer isolation, callback order, and real HTTP+JSON and
JSON-RPC SSE. One test establishes 96 transfers on a single reactor, independent
of the pool's sharding. Existing HTTP SSE interoperability scripts also apply.
TCK compliance is a separate validation step; this backend does not establish
protocol conformance by itself.

### Diagnostic boundaries and limits

The measured first event is the first raw HTTP body chunk delivered to
`StartStreamRequest`'s callback, **not** a parsed A2A observer event. The serial
server accepts a connection, reads its complete request, then sends the initial
response before accepting another connection. A slow request can therefore
hold later connections behind it. Finite completion also includes the test
thread's sequential release of server connections.

Opt-in `--diagnostics FILE` adds explicit request IDs, server accept/read/send
timestamps and client API-return/metadata/body timestamps in the **test harness**.
IDs join server and client samples independently of socket arrival order. The
sidecar retains every repetition and reports p50/p90/p95/p99/max, process CPU
through first delivery, and reactor creation/destruction counters. Pre-send
(submission to server send) plus post-send (server send to body callback) equals
first latency per sample; their percentiles cannot be added. The server-read
interval can expose request head-of-line blocking. Process CPU includes the
fixture server and client; it cannot identify an individual busy SDK thread.

The pre-send interval aggregates client setup, command enqueue, coalesced wake,
owner scheduling, `curl_multi_add_handle`, registration, readiness processing,
connection establishment and server admission. Post-send aggregates Winsock
readiness, `curl_multi_socket_action` and ordered dispatch. These timestamps do
not separately measure queue residence, time inside `WSAPoll`, wake-to-owner
latency or individual socket actions. A phase difference alone cannot identify
which of those stages caused it. No production hot-path logging is added.

The source audit found four reactor shards (24 new transfers per shard at 96),
an O(1) average registration map, generation checks for stale snapshots, and an
O(socket count) `WSAPoll` scan without a 64-socket ceiling. The client retains
up to 64 idle stream slots; teardown beyond that boundary is another reason to
compare fresh and reused clients, not evidence of a proven bottleneck. Linux's
epoll backend is unchanged.

If native raw traces leave a material post-send regression, use deeper timing
and Windows CPU/scheduling evidence before changing the reactor.

The temporary `scripts/trace_curl_reactor.py` investigation tool adds a deeper
source overlay while building the evidence executable. It restores the original
source bytes on exit, including failures, and refuses to overwrite concurrent
edits. Its per-thread buffers record waits, commands, socket actions, libcurl
receipt and dispatch with request IDs and Windows thread cycle counts. CI uploads
these files as `windows-reactor-stage-trace`. These instrumented runs are
diagnostic controls, not acceptance measurements.

The connection probe records libcurl's `SleepEx` and `SO_ERROR` checks. Its earlier
skipped-yield control did not remove the pauses: they occur in `SO_ERROR`.
The current control uses writable-without-exception readiness to identify a
successful connect, preserving the original error query on failures. It compares
both checks using the same binaries and reverses backend and control order. The
focused suite runs under both checks, including refused connections and invalid
socket errors. Only the diagnostic processes' libcurl import entries are
temporarily redirected; they are restored on exit, and the DLL on disk is
unchanged. This is an experiment, not an SDK fix. Paths and the manifest identify
each control explicitly.

After running the tool locally, rebuild the ordinary targets before running
untraced acceptance benchmarks:

```powershell
python scripts/trace_curl_reactor.py --build-dir build-windows `
    --configuration RelWithDebInfo --output-dir reactor-stage-trace
cmake --build build-windows --config RelWithDebInfo --parallel
```

For a separate Windows CPU/scheduling trace:

```powershell
# Elevated Developer PowerShell; inspect locally before sharing the ETL.
wpr -start GeneralProfile -filemode
python scripts/investigate_curl_reactor.py --build-dir build-windows `
    --configuration RelWithDebInfo --output-dir reactor-investigation-wpr
wpr -stop reactor-investigation.etl
```

Building under WPR perturbs the capture. For targeted follow-up, use the runner's
`--native PATH --fallback PATH --configuration RelWithDebInfo` form with the
prebuilt snapshots in `build-windows/reactor-investigation-binaries/`, and an
optional `--cmake-cache build-windows/CMakeCache.txt`. Inspect network-owner and
dispatch stacks and scheduling in Windows Performance Analyzer. This trace is
separate diagnostic evidence and must not replace an untraced acceptance run.

Linux/MinGW/Wine checks validate portability and correctness but cannot establish
native Windows latency parity. Native Windows CI artifacts and measurements on
the affected Windows machine are the relevant performance evidence.
