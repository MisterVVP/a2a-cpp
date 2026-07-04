# Custom Executors

Implement `a2a::server::AgentExecutor` to define application behavior.

## Required methods

Executors implement task/message operations including:

- `SendMessage`
- `SendStreamingMessage`
- `GetTask`
- `ListTasks`
- `CancelTask`

Push-notification config methods have default `PushNotificationNotSupported` behavior and should be overridden only when the server actually supports them.

## Request context

Every executor method receives `RequestContext`. Use it for auth metadata, transport metadata, and request-scoped policy decisions. Treat metadata as untrusted until validated.

## Design guidance

- Keep executor methods small and deterministic.
- Validate inputs at the boundary.
- Return `a2a::core::Result<T>` with structured errors.
- Avoid shared mutable state unless synchronized and documented.
- Unit-test executor behavior separately from transport mapping.
