# Native async gRPC unary service

`a2a::server::AsyncGrpcServerTransport` connects the
[async dispatcher](async-dispatcher.md) to the generated native gRPC callback
service. It supports all nine unary RPCs, including extended agent cards. Include
`a2a/server/async_grpc_server_transport.h` when `A2A_HAS_REQUEST_EXECUTION` is `1`.

Choose the service when registering an A2A endpoint:

- `GrpcServerTransport` uses the existing synchronous `Dispatcher` / `AgentExecutor`
  interfaces and retains unary and streaming behavior.
- `AsyncGrpcServerTransport` uses `engine::AsyncDispatcher` /
  `engine::AsyncAgentExecutor`. Streaming RPCs return gRPC `UNIMPLEMENTED`;
  async stream sessions are a separate feature.

Register one A2A service implementation per endpoint. Existing applications can
keep their current registration. An async executor initiates work and returns;
it may complete inline for fast in-memory work, or retain its owned request,
context, and typed completion for a genuinely asynchronous backend.

```cpp
#include <grpcpp/server_builder.h>
#include "a2a/server/async_grpc_server_transport.h"

// executor implements engine::AsyncAgentExecutor.
a2a::server::engine::AsyncDispatcherOptions execution;
execution.worker_count = 2;
execution.max_in_flight = 1024;
auto dispatcher = std::make_shared<a2a::server::engine::AsyncDispatcher>(
    executor, execution);
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
extraction. Interceptors receive that metadata on dispatcher workers. Ingress
metadata is untrusted until the configured security policy authenticates it.

Set `GrpcServerTransportOptions::required_extensions` as for the synchronous
service. Successful responses and application errors carry the same activated
extension trailers. Authentication, authorization, agent-card providers, task
history projection, and interceptor sequencing use the shared dispatcher pipeline.

The gRPC deadline is converted at ingress to a steady-clock deadline. Native
`OnCancel` reports a reached server deadline as `kDeadlineExceeded`, otherwise
`kClientCancelled`. The callback only queues the signal; application stop
observers execute on dispatcher workers. There is no SDK per-request polling
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
submits to the existing bounded dispatcher. Executor initiation, policies/providers,
interceptors, application stop observers, and response finalization use its fixed
worker pool. Pending application operations hold no SDK worker. gRPC owns its own
callback/event-engine threads; the SDK does not create a worker for each RPC.

Stop callbacks must still be short and non-throwing. An application callback or
hook that blocks delays other work on that fixed pool. Synchronous security
stores and card providers retain their existing contracts on those workers;
this is not asynchronous database or outbound-client I/O.

Call `service.Shutdown()` **before** `grpc::Server::Shutdown()`. It closes
admission and queues `kServerShutdown` for accepted pending calls. Without that
host notification, gRPC exposes cancellation but does not identify host shutdown
separately from client cancellation. Keep the service alive through server
`Wait()`; destroy the dispatcher afterwards. Late executor replies are ignored,
and retained execution contexts stay valid. Cancellation is cooperative and does
not forcefully stop or join application-owned I/O.

## Availability and installed consumers

The API needs a standard library implementing C++20 `stop_token`, `stop_source`,
and `stop_callback`, plus the generated gRPC callback service API. The SDK exports
`A2A_HAS_REQUEST_EXECUTION` from `a2a::server`. Guard the header include itself on
platforms where that definition is `0`.

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
then releases their replies together. Only the controller sleeps; async SDK
workers remain free while replies are pending. This models externally completed
work and does not measure a particular network or database backend.

The JSON records application execution threads, client-observed p50/p95 latency,
and Linux process thread counts before/during the workload. Process counts on
other systems are `null`; application thread counts are portable. gRPC's own
shared pools can change process counts independently of SDK workers. The CTest
smoke checks successful completion and the configured async worker bound without
asserting host-dependent latency thresholds.

Each fixture uses one shared client completion queue with one polling thread.
This client infrastructure stays constant across both services and supports older
gRPC releases without relying on their client callback queue implementation.

### Measured comparison

A representative Linux run with GCC 15, gRPC 1.51.1, and RelWithDebInfo produced
these results on loopback. The dispatcher used two workers; both paths used the
same controller-held backend fixture and warmed generated gRPC client.

| Requests | Backend hold | Service | Application threads | Process threads before → pending | Client p50 / p95 |
| ---: | ---: | --- | ---: | --- | --- |
| 32 | 10 ms | Synchronous | 32 | 11 → 42 | 27.00 / 27.40 ms |
| 32 | 10 ms | Async callback | 2 | 13 → 13 | 12.53 / 12.58 ms |
| 128 | 10 ms | Synchronous | 128 | 11 → 138 | 30.72 / 44.60 ms |
| 128 | 10 ms | Async callback | 2 | 13 → 13 | 14.63 / 14.95 ms |

Application worker counts establish the bounded scheduling property. The process
counts include gRPC and client infrastructure, and latency samples depend on the
host and controlled workload; these values are not throughput or latency
promises for a real backend. The zero-delay, 32-request sample measured p50/p95
1.28/1.35 ms for sync and 0.93/1.02 ms for async, with two application threads
in each service. Immediate work can pay scheduling overhead; pending work
is where the bounded worker model avoids holding one server thread per call.

The earlier isolated dispatcher benchmark recorded a sync cost of 0.408 µs
before the shared pipeline and 0.631 µs afterwards (about +0.22 µs). That
microbenchmark tradeoff remains documented; the native wire comparison here uses
the current shared sync pipeline for both application semantics and baseline.

## TCK profiles

The default SUT remains synchronous with its existing capabilities and full
three-transport suite. `A2A_TCK_GRPC_ASYNC=1` selects the callback gRPC service and
advertises streaming as unavailable. CI additionally runs that unary-only profile
against both stores using `TCK_TRANSPORTS=grpc`.

The async TCK SUT intentionally adapts the existing synchronous SUT dispatcher
on the bounded SDK workers so protocol fixtures and store behavior stay identical.
It validates wire semantics, not non-blocking store/network I/O or performance.
The separate network evidence above uses externally completed pending operations.
Upstream registered requirements without tests remain a reported coverage gap;
passing executable TCK tests is not a claim of complete specification coverage.
