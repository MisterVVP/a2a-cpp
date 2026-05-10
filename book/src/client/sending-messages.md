# Sending Messages

This chapter covers the common happy path for sending a message request with `A2AClient`.

## Happy path

1. Build or resolve the server endpoint.
2. Initialize a transport (REST or JSON-RPC).
3. Create an `A2AClient`.
4. Build the outgoing message payload.
5. Call `SendMessage`.
6. Inspect task/message response data.

## Operational guidance

- Validate endpoint configuration and protocol choice at startup.
- Prefer explicit timeouts at transport boundaries.
- Log request IDs/task IDs for diagnostics.
- Keep request construction deterministic in tests.

## Failure paths to handle

- Network transport errors.
- Protocol/serialization errors.
- Server-side execution errors.
- Auth failures (401/403 equivalents depending on transport mapping).

## See also

- [Client Overview](overview.md)
- [Get Task](get-task.md)
- [Cancel Task](cancel-task.md)
- [Authentication Overview](../auth/overview.md)
