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

Add commands queue handles for activation without running socket actions. The
owner finishes queued commands, including cancellation, then activates one new
transfer per event-loop turn. Libcurl's zero-timeout callback drives its start;
ready sockets run before the timer. This gives readiness and cancellation a turn
between new connection starts, without a polling interval or another worker.
Native Windows traces found 10–11 ms waits in libcurl's `SO_ERROR` and
`getpeername()` connection checks;
running those checks while draining commands delayed queued cancellation and
already-readable responses. Connection setup can still depend on the installed
libcurl and Winsock versions. First-event measurements include new TCP connects,
server admission and dispatch, in addition to readiness processing.

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

The CI comparison opens 28,320 loopback streams across four processes. Windows
retains TCP `TIME_WAIT` entries after a process exits, so CI expands its disposable
runner's IPv4 dynamic TCP port range to 1025–65535 before measuring both orders.
The artifacts include the configured range and TCP state counts. This provisions
the fixture without changing latency budgets or retrying failed measurements.

Keep the machine otherwise idle during latency measurements. Use explicit UTF-8
output so the reports also work in Windows PowerShell 5.1:

```powershell
./build-windows/tests/RelWithDebInfo/curl_reactor_evidence.exe | Out-File -Encoding utf8 native.json
cmake -S . -B build-windows -DA2A_TEST_CURL_PORTABLE_REACTOR=ON
cmake --build build-windows --config RelWithDebInfo --target curl_reactor_evidence --parallel
./build-windows/tests/RelWithDebInfo/curl_reactor_evidence.exe | Out-File -Encoding utf8 fallback.json
cmake -S . -B build-windows -DA2A_TEST_CURL_PORTABLE_REACTOR=OFF
cmake --build build-windows --config RelWithDebInfo --parallel
python scripts/compare_curl_reactor_evidence.py fallback.json native.json --output reactor-summary.md
```

Windows CI runs these equivalent workloads and the mandatory gRPC and HTTP+JSON
performance smoke. It publishes raw JSON and a Markdown comparison in
`windows-curl-reactor-evidence`. The report records first-event p95,
finite-completion/cancellation p95 and p99, samples, errors and added process
threads for 1/16/64/96 streams over twenty repetitions. One server worker serves
all sockets. Cancellation timing starts after the first event establishes each
stream and ends at dispatch completion, while peers remain idle. Finite timing
starts before releasing all established server connections. First-event timing
includes submission and cold shard startup. Record compiler, libcurl version,
OS and build configuration when comparing results outside CI.
The fixture sends the headers and first SSE chunk in one write so TCP
Nagle/delayed-ACK behavior cannot hold the tiny chunk behind the headers.
The evidence executable requests a 1 ms Windows process timer period with
`timeBeginPeriod`, balances it with `timeEndPeriod` on exit, and records
`timer_period_ms` in each row. Both backends use this same setting; the
comparison rejects mismatched periods. The SDK itself does not request a timer
resolution change. Default Windows timer granularity can dominate startup
measurements (about 16 ms at one stream on both backends in a native CI run).
Twenty repetitions reduce dependence on a single cold or descheduled sample.

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

### Native Windows startup results

Two native Windows job attempts for `221bfe6` measured both execution orders
using prebuilt binaries, 20 repetitions and a requested 1 ms timer period.
The runners used Windows Server 2022 build 20348, Intel Xeon Platinum 8573C
with four exposed logical CPUs, MSVC 19.44.35229 and RelWithDebInfo. Runtime
libcurl was 7.74.0-DEV with Schannel and zlib 1.2.11; dependency DLL hashes
matched between the two backends.

At 96 streams, first-event p95 values were milliseconds, fallback/native:

| Attempt | Order | Complete startup | Cancellation-workload startup |
| ---: | --- | ---: | ---: |
| 1 | native-first | 160.305/110.278 | 151.488/118.112 |
| 1 | fallback-first | 149.108/138.259 | 170.325/125.165 |
| 2 | native-first | 159.102/126.864 | 138.240/147.240 |
| 2 | fallback-first | 151.103/99.605 | 162.961/118.419 |

All four 96-stream comparisons passed the unchanged strict budgets. Every
completion and actual cancellation check passed at 1/16/64/96 streams, with
zero errors, equal complete samples and at most five added SDK threads.
Cancellation-workload startup above is separate from actual cancellation time.

Strict startup parity across every workload is not established. At 16 streams,
native first-event p95 was sometimes 8–9 ms slower than fallback; four startup
checks exceeded their strict budgets by 0.190–0.857 ms. Connection setup still
depends on libcurl and Winsock. The scheduling correction removes repeated
connection starts ahead of ready work, but does not eliminate platform connect
waits. The cloud gate and strict thresholds remain unchanged.

The [first attempt's raw reports](https://github.com/MisterVVP/a2a-cpp/actions/runs/42108738541/artifacts/13453307322)
and [independent attempt's raw reports](https://github.com/MisterVVP/a2a-cpp/actions/runs/42108738541/artifacts/13457933707)
include both execution orders, DLL/executable hashes and runner metadata.

### Local comparison limits

The Linux-hosted Windows check used MinGW GCC 14, libcurl 8.17.0 with
Schannel, and Wine 10.0. All 27 applicable deadline, readiness and transfer tests
passed in the initial implementation, with zero workload errors and bounded
thread growth. Wine did not reproduce native Winsock's EOF flag combination;
the regression tests now cover those flags directly. With competing lint
and build processes paused, the native backend's cancellation p95/p99 were 0.507/0.507 ms
at one stream, 0.524/1.225 ms at 16, 0.567/0.744 ms at 64, and 0.527/0.712 ms at
96. Under concurrent compiler/analyzer load, Wine exhibited cancellation tails
above 20 ms for both implementations.

The unloaded Wine comparison still flagged some first-event and finite-completion
regressions, including cold one-stream startup (fallback/native first-event p95
4.010/9.076 ms) and 16-stream completion p99 (5.455/9.851 ms). Removing analyzer
contention reduced cancellation tails but did not establish full latency parity.
These measurements include Wine's Winsock translation and the fixture's sequential
accept/release work. They are diagnostic evidence, not a passing native Windows
performance claim. The Windows CI comparison publishes native-runner evidence
with the documented cloud budgets; local comparisons retain the stricter
default thresholds.
