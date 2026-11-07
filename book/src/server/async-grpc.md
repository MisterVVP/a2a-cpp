# Native async gRPC service

`a2a::server::AsyncGrpcServerTransport` connects the
[async dispatcher](async-dispatcher.md) to the generated native gRPC callback
service. It supports all nine unary RPCs, including extended agent cards, and
[coroutine streaming](async-streaming.md) for both streaming RPCs. Include
`a2a/server/async_grpc_server_transport.h` when `A2A_HAS_ASYNC_SERVER` is `1`.

Choose the service when registering an A2A endpoint:

- `GrpcServerTransport` uses the existing synchronous `Dispatcher` / `AgentExecutor`
  interfaces and retains unary and streaming behavior.
- `AsyncGrpcServerTransport` uses `engine::AsyncDispatcher` /
  `engine::CoroutineAgentExecutor`. Streaming methods await owned session writes;
  unimplemented application methods return unsupported-operation errors.

Register one A2A service implementation per endpoint. Existing applications can
keep their current registration. Coroutine executors suspend for asynchronous
backends and return owned typed results with `co_return`. The gRPC callback API is only the transport boundary.

```cpp
#include <boost/asio/thread_pool.hpp>
#include <grpcpp/server_builder.h>
#include "a2a/server/async_grpc_server_transport.h"

// executor implements engine::CoroutineAgentExecutor.
a2a::server::engine::AsyncDispatcherOptions execution;
boost::asio::thread_pool runtime(2);
execution.max_in_flight = 1024;
auto dispatcher = std::make_shared<a2a::server::engine::AsyncDispatcher>(
    executor, runtime.get_executor(), execution);
a2a::server::AsyncGrpcServerTransport service(dispatcher);

grpc::ServerBuilder builder;
builder.AddListeningPort("127.0.0.1:50051", grpc::InsecureServerCredentials());
builder.RegisterService(&service);
auto server = builder.BuildAndStart();

// At host shutdown, while service and dispatcher are still alive:
service.Shutdown();
server->Shutdown();
server->Wait();
dispatcher->Shutdown();
runtime.join();
```

The complete [installed-consumer example](../examples/apps.md) is
`examples/apps/async_grpc_server/main.cpp`. It implements a minimal read-only
GetTask application and a `--smoke` mode that starts a real callback server,
sends a request, and shuts it down. Other application operations explicitly
return `UnsupportedOperation`. This example's immediate in-memory response does
not demonstrate asynchronous network or database I/O.

## Metadata, validation, and error mapping

The callback service shares metadata extraction, version and required-extension
validation, gRPC status/ErrorInfo serialization, and ListTasks conversion with
the synchronous transport. All incoming metadata is copied into the owned
`RequestContext`; authentication metadata includes the same bearer/API-key
extraction. Interceptors receive that metadata on Asio workers. Ingress
metadata is untrusted until the configured security policy authenticates it.

Set `GrpcServerTransportOptions::required_extensions` as for the synchronous
service. Successful responses and application errors carry the same activated
extension trailers. Authentication, authorization, agent-card providers, task
history projection, and interceptor sequencing use the shared dispatcher pipeline.

The gRPC deadline is converted at ingress to a steady-clock deadline. Native
`OnCancel` reports a reached server deadline as `kDeadlineExceeded`, otherwise
`kClientCancelled`. The callback only queues the signal; application stop
observers execute on Asio workers. There is no SDK per-request polling
thread or timer. As with the standalone dispatcher, completion also checks the
deadline before selecting a terminal outcome.

Cancellation maps to gRPC `CANCELLED`, deadline expiry to `DEADLINE_EXCEEDED`, and
service shutdown to `UNAVAILABLE`. Ordinary application errors retain the
synchronous transport's status, protocol-code trailer, and binary ErrorInfo.
Admission errors return `UNAVAILABLE`. The client may observe cancellation or
expiry during response delivery even after the server lifecycle completed; the
server's first terminal transition remains unchanged. The server can only
classify the cancellation signal and deadline it observes locally.

## Threading and shutdown

The native gRPC callback method copies and validates the protocol envelope, then
submits to the bounded coroutine dispatcher. Policies/providers, interceptors,
application coroutines, and response finalization run on the caller's Asio runtime.
Pending application I/O suspends without occupying an event-loop worker. gRPC owns its own
callback/event-engine threads; the SDK does not create a worker for each RPC.

Stop callbacks must still be short and non-throwing. Application code or a hook that blocks delays other work on the Asio pool. Synchronous security
stores and card providers retain their existing contracts on those workers;
this is not asynchronous database or outbound-client I/O.

Call `service.Shutdown()` **before** `grpc::Server::Shutdown()`. It closes
admission and queues `kServerShutdown` for accepted pending calls. Without that
host notification, gRPC exposes cancellation but does not identify host shutdown
separately from client cancellation. Keep the service alive through server
`Wait()`; destroy the dispatcher afterwards and drain/join the Asio runtime.
Late coroutine results are ignored, and retained execution contexts stay valid. Cancellation is cooperative and does
not forcefully stop or join application-owned I/O.

## Availability and installed consumers

Build with `A2A_ENABLE_ASIO=ON`, link `a2a::asio`, and provide Boost 1.92+.
The API requires a standard library implementing C++20 `stop_token`, `stop_source`,
and `stop_callback`, plus the generated gRPC callback service API. The SDK exports
`A2A_HAS_ASYNC_SERVER=1` from `a2a::asio`; the standalone lifecycle remains available
through `A2A_HAS_REQUEST_EXECUTION` on `a2a::server`. Guard native async header
inclusion when the optional target is unavailable.

| Platform/toolchain | Availability and validation |
| --- | --- |
| Linux GCC / Clang with C++20 cancellation | Native unary, cancellation/shutdown, concurrency and installed-consumer tests run. |
| Windows MSVC with C++20 cancellation | The same C++ network tests and installed-consumer smoke run in Windows CI. |
| macOS with a capable libc++ | The same tests run when CMake's cancellation probe succeeds. |
| Older Apple libc++ without C++20 cancellation | Synchronous SDK remains available; native async targets are omitted and installed async smoke reports a skip. |

Build a downstream app using the installed package:

```sh
cmake --install build --prefix "$PWD/work/sdk"
cmake -S examples/installed_package_consumer -B work/async-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/work/sdk" -DA2A_EXAMPLE_APP=async_grpc_server
cmake --build work/async-consumer
./work/async-consumer/a2a_example --smoke
```

Supply your dependency prefix/toolchain as for any installed SDK consumer. For
multi-config generators select the SDK's configuration at install/build and use
that configuration's executable directory. The CTest
`installed_async_grpc_consumer_test` performs this flow in an isolated directory,
forwards the parent compiler/vcpkg settings, and links only exported SDK targets.
It runs with the normal Linux/macOS/Windows test jobs. An unsupported C++20
cancellation library is reported as a CTest skip, not an async validation success.

## Reproducing thread and latency evidence

Build `async_grpc_execution_evidence` in an optimized configuration and run:

```sh
./build/tests/async_grpc_execution_evidence 32 0
./build/tests/async_grpc_execution_evidence 32 10
./build/tests/async_grpc_execution_evidence 128 10
```

The first argument selects simultaneous requests (1–128), and the second selects
a controller-held backend readiness delay in milliseconds (0–10). Both services
use the same generated completion-queue client, payload, metadata, warmed channel, and
backend fixture. The pending workload waits until all operations have initiated,
then releases their replies together. Only the controller sleeps; Asio
workers remain free while replies are pending. This models externally completed
work and does not measure a particular network or database backend.

The JSON records application execution threads, client-observed p50/p95 latency,
and Linux process thread counts before/during the workload. Process counts on
other systems are `null`; application thread counts are portable. gRPC's own
shared pools can change process counts independently of application Asio workers. The CTest
smoke checks successful completion and the configured async worker bound without
asserting host-dependent latency thresholds.

Each fixture uses one shared client completion queue with one polling thread.
This client infrastructure stays constant across both services and supports older
gRPC releases without relying on their client callback queue implementation.

### Coroutine execution evidence

Run the optimized evidence executable with the commands above to compare the
current coroutine engine with synchronous execution on the same host. The
controlled backend uses suspendable coroutine results; the same fixed Asio pool
handles all async requests. Historical callback-engine timings do not describe
this implementation. Portable acceptance checks cover successful completion and
the configured worker bound; host-dependent latency is reported rather than used
as a pass/fail threshold.

A Linux working-tree run with GCC 15, gRPC 1.51.1, Boost 1.92.0 and
RelWithDebInfo produced the following results with a 10 ms backend hold:

| Requests | Service | Application threads | Process threads before → pending | Client p50 / p95 |
| ---: | --- | ---: | --- | --- |
| 32 | Synchronous | 32 | 11 → 42 | 14.24 / 14.61 ms |
| 32 | Coroutine | 2 | 13 → 13 | 11.27 / 11.36 ms |
| 128 | Synchronous | 128 | 11 → 138 | 31.57 / 34.31 ms |
| 128 | Coroutine | 2 | 13 → 13 | 14.05 / 14.58 ms |

These measurements demonstrate bounded application-worker use for this controlled
pending workload; they do not establish general database or network throughput.

## TCK profiles

The existing `tck_sut` retains synchronous execution, streaming, and all three
protocol bindings. A separate `tck_async_sut` provides one shared async setup for
gRPC, JSON-RPC, and HTTP+JSON integration. Currently only gRPC is implemented;
requesting an unavailable async binding fails at startup. The setup enables
coroutine streaming and shared task subscriptions over gRPC.

```sh
SUT_TARGET=tck_async_sut A2A_TCK_TRANSPORTS=grpc ./scripts/run_tck_sut.sh
TCK_TRANSPORTS=grpc ./scripts/run_tck_mandatory.sh
./scripts/stop_tck_sut.sh
```

The async agent uses the SDK `AsyncDispatcher` and `AsyncGrpcServerTransport`
directly. Both setups share application behavior, stores, policies, and agent-card
providers. The async application executor queues blocking fixture operations on
fixed backend workers instead of calling the synchronous dispatcher. Live stream
subscriptions suspend on shared publication notifications. Existing store and
push-delivery APIs are synchronous; this facility keeps that fixture work off Asio
workers and does not provide asynchronous database or outbound network
I/O. The separate network evidence above uses externally completed pending
operations.

The async card advertises only enabled bindings. HTTP serves SDK agent-card
discovery and caching, without registering JSON-RPC or HTTP+JSON operation routes.
CI runs the synchronous and async setups against both stores.

Upstream registered requirements without tests remain a reported coverage gap;
passing executable TCK tests is not a claim of complete specification coverage.
The gRPC-only report also leaves requirements tested exclusively through other
bindings untested. Upstream console summaries group `NOT TESTED` under “Failed”;
use the JSON requirement statuses and executable test results to distinguish
coverage gaps from failures.
