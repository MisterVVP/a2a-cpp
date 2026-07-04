# Sending Messages

`A2AClient::SendMessage` starts or continues a task by sending a protobuf `lf::a2a::v1::SendMessageRequest` through the configured transport.

## Request construction guidance

- Set a stable `message.message_id` for idempotency and diagnostics.
- Let the server generate a task ID for new work unless you are intentionally continuing an existing task.
- Attach push-notification config only when the target Agent Card advertises support and your server policy allows it.
- Use `CallOptions` for per-call deadlines, metadata, or auth settings where supported by the transport.

## Response handling

The response can contain immediate task state and output data. Persist task identifiers in logs/telemetry so later `GetTask`, `CancelTask`, streaming subscription, or push-notification calls can be correlated.

## Failure paths to test

- Network or HTTP/gRPC transport failure.
- Serialization or protocol validation failure.
- Auth policy rejection.
- Unsupported operation or required extension mismatch.
- Duplicate/retry behavior for repeated message IDs.

## Related pages

- [Discovery](discovery.md)
- [Get Task](get-task.md)
- [Cancel Task](cancel-task.md)
- [Authentication](../auth/overview.md)
