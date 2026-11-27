# Coroutine unary dispatch

Build with `A2A_ENABLE_ASIO=ON` and link `a2a::asio`. The async execution engine uses
Boost.Asio C++20 coroutines throughout. Implement
`a2a::server::engine::CoroutineAgentExecutor`; its eight unary methods return
`boost::asio::awaitable<core::Result<T>>`. Extended agent cards continue to use the
agent-card provider. [Streaming methods](async-streaming.md) use the same engine
through `DispatchStream` and an owned, demand-driven session.

```cpp
boost::asio::thread_pool runtime(2);
auto application = std::make_shared<MyCoroutineExecutor>();
auto dispatcher = std::make_shared<a2a::server::engine::AsyncDispatcher>(
    application, runtime.get_executor());
```

The callback-based executor and its Asio adapter have been removed. There is one
async application API and one coroutine dispatcher. The synchronous `AgentExecutor`,
`Dispatcher`, and synchronous transports remain available independently of Boost.

## Execution and ownership

Each executor method owns its request and `RequestExecutionContext` parameters.
Use `co_await` for asynchronous backends and `co_return` for the typed result.
Unimplemented methods return the existing protocol unsupported-operation error.
The dispatcher directly awaits the application, with no callback executor bridge
or separate SDK worker queue.

`AsyncRuntime` assigns each request to one of its worker executors. Each worker
serializes required-extension validation, authorization, interceptors, coroutine
execution, cancellation and completion for its assigned requests. Suspended
coroutines release the worker so other requests can progress. Dispatchers
constructed with a caller-supplied executor use a strand per request. Workers
can run concurrently, so shared application services must be thread-safe.
Keep hooks and coroutine execution nonblocking. Synchronous security and store
facilities block the worker that invokes them.

Before hooks run in registration order and after hooks in reverse order. An
accepted request has one terminal result and one after-hook sequence, including
validation failures and cancellation. Hooks for one call never overlap.
Interceptor-updated metadata is passed to the executor through `WithMetadata`;
the transport result retains the original ingress snapshot.

The synchronous and coroutine dispatchers share pipeline configuration and routing.
Each invocation creates its own internal session for authentication and interceptor
state. The session restores the authenticated principal between hooks. Synchronous
after-hook exceptions retain their existing propagation behavior; the coroutine
path contains them so remaining hooks and transport completion can still run.

The coroutine frame retains its application and pipeline through physical backend
cleanup. Request cancellation can finish the protocol response earlier. A late
coroutine return cannot replace the winning outcome or invoke transport completion
again. Never destroy an application-owned Asio I/O object after its execution
context has already been destroyed.

## Immediate reads

A coroutine executor can also implement `ImmediateReadExecutor` to supply
`TryGetTask` or `TryListTasks`. The dispatcher calls these methods on its
application worker after validation, authorization, and before-dispatch hooks.
Return a result when the read can complete without blocking. Returning
`std::nullopt` leaves the request to the coroutine API and must have no side
effects. A returned error completes the request. Response projection,
cancellation, deadlines, and completion hooks apply to both paths.

## Transport boundary

`Dispatch(request, metadata, transport_completion, deadline)` admits work and
returns `Result<shared_ptr<RequestExecution>>` immediately. It never executes user
code inline. The completion sink is the boundary to native transport APIs such as
gRPC reactors; application executor methods do not receive completion callbacks.
Admission failure returns directly and invokes no transport completion. Accepted
requests complete on their serialized executor after the final interceptor hooks.

Retain the returned lifecycle source to report cancellation. A network reactor
calls `dispatcher.Cancel(source, reason)`, which posts cancellation to the assigned
runtime worker or the supplied Asio executor. Calling `source->Cancel()` directly retains the standalone
lifecycle's inline stop-observer semantics. A transport must not call `TryComplete`;
terminal arbitration belongs to the dispatcher.

The transport drives deadlines using its native timer or `ExpireDeadline()`.
Dispatch also checks the deadline before application execution and during
finalization. First-terminal-transition precedence remains unchanged.

## Limits, errors, and shutdown

`AsyncDispatcherOptions::max_in_flight` bounds logically active requests.
`max_pending` additionally bounds coroutine frames, including requests already
cancelled at the protocol boundary but still cleaning up backend work. Both default
to 1024, must be nonzero, and cause immediate admission rejection when exhausted.
Thread count is controlled by the caller's Asio runtime; there is no dispatcher
`worker_count` or second application-adapter capacity setting.

Admission reservations release the active slot before transport completion and
retain the pending slot until physical cleanup. Construction failures and failures while
scheduling the initial work release their reservations automatically.

Application errors retain their structured fields. Exceptions become internal
errors; Asio operation cancellation becomes a network error, while the lifecycle's
winning outcome determines transport cancellation status. After-hook and transport
completion exceptions are contained.

`Shutdown()` closes admission and schedules server cancellation without waiting
for application I/O. Destruction also requests shutdown and does not own or join
runtime threads. Keep the runtime alive, allow cancellation and cleanup to drain,
and then join it. A completion can destroy its own dispatcher safely. Cancellation
remains cooperative; a backend that disables it must supply its own bounded cleanup.

Configured task stores must outlive their pipeline operations. Applications must
own any backend resources used after logical request completion. See the
[coroutine guide](asio-coroutines.md) and [native gRPC integration](async-grpc.md).
