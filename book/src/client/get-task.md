# Get Task

`GetTask` retrieves task state and associated outputs after a send operation.

## When to use

- Polling task progress/state after `SendMessage`.
- Fetching final task payloads when streaming is not enabled.

## Happy path

1. Persist the returned task ID from `SendMessage`.
2. Call `GetTask` with that task ID.
3. Interpret status and outputs.

## Operational context

- Retry transient failures with bounded backoff.
- Enforce request deadlines to avoid hanging calls.
- Record task IDs in logs/telemetry for traceability.

## Failure scenarios

- Unknown task ID.
- Task exists but has not produced outputs yet.
- Authorization mismatch for task visibility.
