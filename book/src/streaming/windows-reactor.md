# Windows outbound HTTP reactor

The default libcurl HTTP client uses a shared socket-readiness reactor on Windows
for synchronous requests and HTTP+JSON/JSON-RPC streaming. Client and observer APIs
are unchanged. Inbound server sockets and gRPC use separate implementations.

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
an imposed stream/handle ceiling. IOCP would require adapting libcurl's readiness
contract to overlapped I/O; WSA event wait sets would require handle-limit sharding.

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
Libcurl's Winsock connection checks can delay ready work when run while draining
commands. Connection setup still depends on libcurl and Winsock; first-event
measurements include new TCP connects, server admission and dispatch.

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
VS 2022. PowerShell 7 is recommended. Select your vcpkg installation; the first
CMake configure installs its pinned manifest dependencies.

```powershell
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

The focused suite covers EOF/hangup mapping, timers, wakeups, cancellation and
shutdown races, callback order, synchronous unary requests, and HTTP+JSON/JSON-RPC
streaming. It also establishes 96 transfers on a single reactor to check scale
independently of pool sharding.

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
This provides capacity for both execution orders without changing latency budgets
or retrying failed measurements.

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

Windows CI builds both executables before measuring native-first and fallback-first
orders. It uploads raw JSON, executable/DLL hashes and runner metadata in
`windows-reactor-comparison`, with the mandatory gRPC and HTTP+JSON smoke results
in `windows-performance-smoke`.

Each backend measures 1/16/64/96 streams over twenty repetitions, recording
first-event p95, completion/cancellation p95 and p99, samples, errors and thread
growth. Cancellation starts after the first event establishes each stream and
includes dispatch completion while peers remain idle. Finite completion starts
before releasing all established server connections. First-event timing includes
cold connection setup and shard startup. The fixture sends headers and the first
SSE chunk together to avoid Nagle/delayed-ACK artifacts.

The executable requests and records a 1 ms Windows process timer period for both
backends, balancing `timeBeginPeriod` with `timeEndPeriod`. The SDK does not change
timer resolution. Keep the machine idle and record compiler, libcurl version, OS
and build configuration when comparing results outside CI.

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

First-event timing remains sensitive to libcurl/Winsock connection setup and
runner scheduling. Earlier native measurements occasionally exceeded the strict
16-stream startup budget; passing a shared-runner cloud gate does not establish
strict parity on deployment hardware. TCK protocol conformance is a separate
validation step.
