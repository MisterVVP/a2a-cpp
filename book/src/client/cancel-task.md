# Cancel Task

`A2AClient::CancelTask` asks the server to cancel in-flight work.

## When to use cancellation

- A user aborts an operation.
- A deadline expires upstream.
- Supervising logic needs to reclaim compute or queue capacity.

## Semantics

Cancellation is best-effort. A task can complete before the cancellation request is processed, and servers can reject cancellation for completed, unknown, or policy-protected tasks.

## Recommended behavior

- Make higher-level cancellation idempotent.
- Return or display the latest task state after cancellation attempts.
- Preserve enough audit data to explain whether the task was canceled, completed, or rejected.
