# Runtime and transport helpers

Applications can keep their own Asio event loop or use the SDK's optional runtime
owner. These helpers compose the existing dispatcher and native transports; they
preserve the request lifecycle described in [request execution](request-execution.md).

## A coroutine application

Link `a2a::asio` from an SDK configured with `A2A_ENABLE_ASIO=ON`. Implement
`CoroutineAgentExecutor` as before, then construct:

```cpp
a2a::server::engine::AsyncRuntime runtime(application, execution_options);
a2a::server::ServerBindings bindings(runtime.dispatcher(), card, binding_options);
```

`application` is a `shared_ptr<CoroutineAgentExecutor>`. `execution_options` is
`AsyncDispatcherOptions`, including interceptors, the extended-card provider,
security policy, stores, and admission limits. `AsyncRuntimeOptions` is an optional
third argument; its `threads` field defaults to two and must be positive.

Register `bindings.grpc_service()` with your `grpc::ServerBuilder`. Pass incoming
HTTP requests to `bindings.http_mux().RouteRequest(request)` using your existing
HTTP host. The bindings own their transports and route references. They do not
create a socket listener, choose credentials, install signal handlers, or run an
HTTP event loop.

This constructor enables native async gRPC unary calls and public agent-card
HTTP discovery, including the legacy discovery path. It does not register REST
or JSON-RPC operation routes. Those async transports remain separate work.
`ServerBindingsOptions::jsonrpc` is unused for async construction. The caller
supplies a card advertising the implemented interfaces and capabilities; the
helper does not rewrite or invalidate signed cards.

See the runnable [async gRPC application](https://github.com/MisterVVP/a2a-cpp/blob/main/examples/apps/async_grpc_server/main.cpp)
and [timer-based coroutine application](https://github.com/MisterVVP/a2a-cpp/blob/main/examples/apps/asio_executor/main.cpp).
Both are built against the installed SDK during testing. The original
`AsyncDispatcher(application, asio_executor, options)` constructor remains
available for applications that already own an Asio runtime.

## An existing blocking application

A thread-safe synchronous `AgentExecutor` can opt into bounded worker adaptation:

```cpp
using namespace a2a::server::engine;
BlockingExecutorOptions limits;
limits.workers = backend_workers;
limits.max_pending = backend_capacity;
auto adapted = std::make_shared<BlockingExecutorAdapter>(application, limits);
AsyncRuntime runtime(adapted, execution_options);
```

The adapter implements all eight typed unary application operations, including
list and void results. Extended-card retrieval continues through the dispatcher's
provider. Validation, authorization, and interceptor processing run once in the
normal dispatcher pipeline. The adapter calls the executor directly; it does not
nest a synchronous dispatcher or restore the removed callback executor API.

`workers` defaults to two. `max_pending` defaults to 1,024 and is a hard bound on
**queued plus running** backend operations. Both must be positive. Closed or full
admission returns a network error. Backend errors are preserved; thrown backend
exceptions become internal errors. Requests, metadata, and application ownership
remain valid until backend completion, which returns to the awaiting Asio executor.

A cancelled queued request skips backend execution when its turn arrives. Running
synchronous calls cannot be forcibly interrupted. Transport cancellation still
completes logically through the existing first-terminal-transition contract;
physical cleanup and its pending capacity remain until the blocking call returns.
Choose backend timeouts accordingly. This facility does not provide asynchronous
network or database I/O, and the backend must support concurrent calls.

The [blocking executor example](https://github.com/MisterVVP/a2a-cpp/blob/main/examples/apps/blocking_executor/main.cpp)
is also compiled and run against the installed SDK.

## Synchronous applications

Link `a2a::server`; Boost is unnecessary:

```cpp
a2a::server::ServerBindings bindings(dispatcher, card, binding_options);
```

Here `dispatcher` is a synchronous `Dispatcher&` that outlives the bindings. The
bundle registers gRPC, REST, and JSON-RPC using their existing transports, including
streaming support. REST and JSON-RPC paths default to `/a2a` and `/rpc` and are configurable;
the HTTP root aliases the configured JSON-RPC path. The common
`ServerBindingsOptions::required_extensions`, when nonempty, applies to every
transport and supersedes their individual extension lists. An empty shared list
preserves individual requirements. Other transport options retain their existing
meaning.

For individually managed registrations, `TransportMux::RegisterAgentCardRoute`
adds only public discovery routes for an existing `RestServerTransport`. Like
other mux registrations, that transport must outlive the mux.

## Shutdown and ownership

Stop transport ingress before draining the runtime:

1. Call `bindings.Shutdown()` to close async service admission and signal existing
   calls. It is idempotent and does nothing for synchronous bindings.
2. Call `runtime.Shutdown()` to close dispatcher admission and request cancellation
   for all accepted calls, including calls submitted outside the bindings.
3. Stop and wait for your HTTP and gRPC hosts while their bindings remain alive.
4. Call `runtime.Join()` to drain coroutine cleanup. Release blocking backends so
   their operations can finish. `BlockingExecutorAdapter::Join()` closes its
   admission and drains all accepted backend work.

Runtime and adapter destruction perform the corresponding shutdown and join.
`Shutdown()` may be called from an application callback; `Join()` and destruction
must run on the owning control thread, outside runtime callbacks and backend
operations. Explicit `Join()` calls from their own workers throw `std::logic_error`
instead of deadlocking. Destroying an owner from its own worker is a programming
error and terminates. They can wait indefinitely for a backend that never returns. Repeated
joins are supported. Neither owner restarts after joining.

Keep transport registrations, dispatcher handles, and application-owned Asio
objects within the runtime's lifetime. Application operations can obtain the
current executor with `this_coro::executor`; local timers then belong to their
coroutine frames. Cancellation remains cooperative, and exactly-once completion,
deadlines, metadata, interceptors, and separate active/physical-cleanup limits
remain the engine's existing responsibilities.
