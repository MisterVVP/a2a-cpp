# Client and server interceptor hooks

This repository exposes transport-agnostic interceptors at two layers:

- `a2a::client::A2AClient` interceptors in `include/a2a/client/client.h`
- `a2a::server::Dispatcher` interceptors in `include/a2a/server/server.h`

## Client hook contract

Use `ClientInterceptor` to observe calls made through `A2AClient`.

- `BeforeCall` runs in registration order.
- `AfterCall` runs in reverse registration order.
- Hooks execute on the caller thread of each `A2AClient` method.
- Hooks must be non-blocking and should not throw exceptions.
- Interceptors observe call metadata through `ClientCallContext`:
  - operation name (for example `GetTask` or `ListTasks`)
  - the `CallOptions` object used for the invocation
- Result state is provided through `ClientCallResult`:
  - `ok == true` for success
  - `ok == false` and `error` populated for failures

`A2AClient::Destroy()` provides explicit lifecycle shutdown. It invokes transport shutdown and
clears the transport pointer to prevent further calls.

## Server hook contract

Use `ServerInterceptor` to observe dispatcher request handling.

- `BeforeDispatch` runs in registration order before invoking `AgentExecutor`.
- `AfterDispatch` runs in reverse registration order after request completion.
- If a `BeforeDispatch` hook returns an error, executor dispatch is skipped and
  `AfterDispatch` hooks are still invoked with the failure result.
- Hooks execute on the thread that calls `Dispatcher::Dispatch`.
- Hooks may read and mutate `RequestContext`; changes are visible to downstream interceptors and
  executor logic.

## Thread-safety guarantees

- Interceptor registration is synchronized:
  - client: internal mutex in `A2AClient`
  - server: shared mutex in `Dispatcher`
- Dispatch/call execution reads a stable interceptor snapshot order within a call.
- No global mutable state is used in interceptor plumbing.
