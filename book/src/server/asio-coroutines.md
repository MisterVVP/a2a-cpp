# Boost.Asio coroutine executors

The optional `a2a::asio` target lets applications implement async unary operations
with C++20 `co_await` and `co_return`. It connects a `CoroutineAgentExecutor` to the
existing `AsyncDispatcher` and native `AsyncGrpcServerTransport`. The dispatcher
retains ownership of admission, security, interceptors, response projection, and
terminal request outcomes.

## Build and consume

Enable `A2A_ENABLE_ASIO=ON` when building the SDK. Boost 1.92 or newer, including
its CMake configuration package, and C++20 standard-library cancellation support
are required. Default builds do not find or link Boost.

```bash
cmake -S . -B build -DA2A_ENABLE_ASIO=ON
cmake --build build --parallel
```

The optional dependency installer downloads the pinned Boost 1.92.0 release,
verifies its SHA-256, and builds/installs Asio and its dependency closure:

```bash
cmake -DA2A_BOOST_INSTALL_PREFIX="$PWD/boost-prefix" -P cmake/InstallBoost.cmake
cmake -S . -B build -DA2A_ENABLE_ASIO=ON -DCMAKE_PREFIX_PATH="$PWD/boost-prefix"
```

For Homebrew use `brew install boost`. Older AppleClang/libc++
installations without `stop_token` require a newer compiler **and standard library**;
changing only the C++ language flag is insufficient. CI retains the default Apple
build and additionally exercises Asio with Homebrew LLVM/libc++.

With vcpkg, enable the
`asio` manifest feature as well as the CMake option:

```bash
cmake -S . -B build -DA2A_ENABLE_ASIO=ON -DVCPKG_MANIFEST_FEATURES=asio
```

Installed consumers link the exported optional target:

```cmake
find_package(a2a_cpp CONFIG REQUIRED)
target_link_libraries(my_agent PRIVATE a2a::asio)
```

The SDK remains Apache-2.0. Boost retains its compatible Boost Software License
1.0; the enabled SDK installation includes that license under
`share/licenses/a2a_cpp/BSL-1.0.txt` (subject to `CMAKE_INSTALL_DATADIR`).

## Implement operations

Include `a2a/server/engine/asio_agent_executor.h`. The coroutine interface supports
all eight executor unary operations, including `Result<void>` for deleting push
configuration. Unimplemented operations return the existing protocol
`UnsupportedOperation` error. Extended agent cards continue to use the dispatcher's
agent-card provider.

```cpp
class MyExecutor final : public a2a::server::engine::CoroutineAgentExecutor {
 public:
  boost::asio::awaitable<a2a::core::Result<lf::a2a::v1::Task>> GetTask(
      lf::a2a::v1::GetTaskRequest request,
      a2a::server::engine::RequestExecutionContext context) override {
    // An application backend returning a cancellation-aware Asio awaitable.
    co_return co_await backend_.GetTask(request.id(), context.StopToken());
  }

 private:
  MyAsyncBackend backend_;
};
```

`MyAsyncBackend` above is application-provided. The SDK's existing synchronous
stores do not become awaitable through this adapter. See the runnable
`examples/apps/asio_executor/main.cpp` for a complete example using an Asio timer.

Supply an event-loop executor when constructing the adapter:

```cpp
boost::asio::io_context loop;
auto application = std::make_shared<MyExecutor>();
auto executor = std::make_shared<a2a::server::engine::AsioAgentExecutor>(
    application, loop.get_executor());
auto dispatcher = std::make_shared<a2a::server::engine::AsyncDispatcher>(executor);
a2a::server::AsyncGrpcServerTransport transport(dispatcher);
```

Run the event loop on a fixed number of application-owned threads. Keep an Asio
work guard while accepting requests so `run()` does not exit before the dispatcher
posts its first operation. Each request gets a strand: its coroutine and
cancellation emission are serialized, while different requests may execute
concurrently. Shared application backends must remain thread-safe.

Asio manages coroutine frames; the adapter never resumes raw coroutine handles.
Request and context parameters are owned by value. The application executor stays
alive until the underlying coroutine finishes, even after the dispatcher or
transport releases the request. Prefer member coroutine functions or named
functions with owned parameters over temporary capturing coroutine lambdas.

## Cancellation, capacity, and shutdown

The adapter forwards the request's stop token to Asio terminal cancellation on the
request's strand. Standard cancellation-aware Asio operations, such as timer waits,
therefore wake when a client cancels, a native transport deadline expires, or the
dispatcher shuts down. A request cancelled before coroutine initiation does not
invoke application code. The existing `RequestExecution` remains authoritative:
a late coroutine result cannot replace an earlier terminal outcome.

Application `core::Result` errors retain their structure. Unhandled exceptions
become internal errors; Asio `operation_aborted` becomes a network cancellation
error. The dispatcher maps the request's winning cancellation reason to transport
status. Completion exceptions are contained to preserve the event loop.

The adapter's `max_pending` constructor argument defaults to 1024 and must be
nonzero. It bounds queued and suspended coroutine operations, including operations
whose requests already completed through cancellation but whose backend cleanup
is still pending. Exhaustion returns a network capacity error through the normal
dispatcher completion path. This limit complements `AsyncDispatcher::max_in_flight`.

Shut down the transport/dispatcher first, then allow the event loop to process
cancellation and backend cleanup. Release the work guard and join the event-loop
threads after producers can no longer post new work. Destroy application-owned
Asio I/O objects and executor adapters before destroying their execution context.
Do not call `io_context::stop()`
or destroy the execution context as a substitute for draining operations.
Cancellation is cooperative: application code that disables cancellation must
provide its own bounded cleanup. The adapter does not force destruction of a frame
that may still be referenced by an outstanding operation.

The adapter adds coroutine-frame allocation and an event-loop scheduling hop.
Immediate in-memory operations can be cheaper through the callback API; benchmark
the application's actual I/O workload before making throughput claims.

Blocking database, security, interceptor, and outbound operations still require
bounded backend workers or genuinely asynchronous implementations. Native async
JSON-RPC/HTTP+JSON listeners, asynchronous PostgreSQL stores, and coroutine
streaming are separate follow-up work.

For the focused ThreadSanitizer profile, enable the optional target explicitly and
supply the installed Boost prefix through `CMAKE_PREFIX_PATH`:

```bash
A2A_TSAN_ASIO=ON CMAKE_PREFIX_PATH="$PWD/boost-prefix" ./scripts/run_thread_sanitizer.sh
```
