# Async unary dispatch

When `A2A_HAS_REQUEST_EXECUTION` is `1`, include
`a2a/server/engine/async_dispatcher.h` and implement
`a2a::server::engine::AsyncAgentExecutor`. This opt-in API supports `SendMessage`,
`GetTask`, `ListTasks`, `CancelTask`, and create/get/list/delete push-notification
configuration. Extended agent cards use `AsyncDispatcherOptions::agent_card_provider`,
with the same provider contract as synchronous dispatch.

The existing `AgentExecutor`, `Dispatcher`, and transports keep their synchronous
interfaces. Native transport registration is a subsequent integration step; this
API can already be driven by an application or fake transport. Streaming operations
are rejected by the async dispatcher.

## Application execution

Each executor method receives its request and `RequestExecutionContext` by value,
and a typed `Completion<T>` accepting `core::Result<T>`. Move arguments into the
application's asynchronous operation when needed after the method returns. The
metadata view includes changes made by `BeforeDispatch`; it shares the original
request's cancellation state and deadline. `WithMetadata` creates such an immutable
view without changing the source's ingress snapshot.

Initiate work and return promptly. Complete inline or later from any thread. The
SDK never holds a worker while waiting for that completion. Duplicate or late
completions have no effect. Application I/O and cleanup remain the application's
responsibility; this API does not make blocking network or database operations
non-blocking.

The dispatcher owns a shared executor while it is alive and retains it through
each initiating method. Application work continuing after dispatcher destruction
must own its own resources; a saved completion does not keep the executor or
dispatcher alive. Retained lifecycle contexts remain valid after shutdown.

## Transport admission and completion

Call `Dispatch` with an owned `DispatchRequest`, metadata, completion, and an
optional steady-clock deadline. It returns `Result<shared_ptr<RequestExecution>>`.
An admission error (closed dispatcher, capacity exhausted, or missing completion)
is returned directly and does not invoke the completion. Every accepted request
receives exactly one completion after its `AfterDispatch` hooks. The callback takes
`(core::Result<DispatchResponse>, RequestExecutionContext)`. Use that owned context
to map the terminal outcome: a worker may deliver completion before `Dispatch`
returns its cancellation handle. The completion context retains the ingress
metadata snapshot.

Retain the returned source to report client cancellation, deadline expiry, or
shutdown. Do not call `TryComplete` on it: the dispatcher owns application
completion arbitration. Check the completion context's terminal `Outcome()` when mapping
cancellation to a transport status; cancellation errors use `ErrorCode::kNetwork`,
while normal executor/provider errors retain their original structured fields.

The transport must schedule `ExpireDeadline()` or report a native deadline signal
for prompt expiry while application work is pending. The dispatcher also checks
the deadline before invoking the executor and when finalizing its response. It
does not create a timer per request. A deadline cannot replace an earlier terminal
outcome. Cancellation competes with completion after response security/projection
and before audit hooks; completion delivery can follow the winning transition.

## Scheduling and limits

`worker_count` fixes the number of SDK workers. `max_in_flight` bounds all admitted
requests, including queued and pending application operations. Both must be nonzero;
invalid options or a null executor throw `std::invalid_argument`. Defaults are two
workers and 1024 in-flight requests. Capacity exhaustion returns a network error;
the transport chooses its overload response.

Executor initiation, security policies, agent-card providers, interceptors, and
transport completion run on these workers. No application dispatch/completion
callback runs inline in `Dispatch`, `Cancel`, or `Shutdown`. A call's hooks and
finalization are serialized; separate calls may run concurrently. Interceptors,
providers, and executors shared across calls must therefore be thread-safe. Keep
hooks and transport completions short. Cancellation observers registered through
`std::stop_callback` retain the [inline, non-blocking contract](request-execution.md)
and must enqueue any substantial cleanup themselves.

The dispatcher and synchronous path share one typed operation router and the same
security, agent-card, interceptor, and history projection pipeline. An async
request snapshots its configured interceptors, as does synchronous dispatch. Before hooks run in registration
order; after hooks run in reverse order, once each, including validation,
application failure, and cancellation. Cancellation during a before hook waits for
that hook to return before finalization; it never starts a concurrent after hook.
In the async path, a throwing executor, provider, policy, or before hook becomes an internal error.
After-hook exceptions are isolated so remaining hooks still run; completion
exceptions are contained after terminal delivery. Callbacks should not throw.

`required_extensions` uses the same `RequiredExtensionsValidator` as synchronous
transports. Rejection precedes before hooks and execution; after hooks still run
for an accepted request. Activated response headers remain the transport's concern.

If a `security_policy` is configured, `task_store` must outlive all completions.
Authorization and authorized list filtering use the existing synchronous store
facility on workers; this is blocking compatibility, not async database I/O. No
legacy synchronous executor adapter is provided. Policies see complete task
history before the caller's requested projection is applied.

## Shutdown

`Shutdown()` closes admission and requests `kServerShutdown` for pending calls. It
is idempotent and does not wait for application I/O. Destruction drains accepted
SDK callbacks and joins workers. If a completion destroys its own dispatcher, that
worker drains its remaining work and exits after the callback returns; other
workers are joined. Cancellation does not forcefully stop application code.

Keep the dispatcher alive during calls to its public methods, as with ordinary
C++ objects. Destruction from a worker callback is supported. A transport must keep
its reply resources alive until completion, even if the client has disconnected.
