# Cancel Task

`CancelTask` requests cancellation of in-flight work.

## When to use

- User explicitly aborts an operation.
- Upstream deadline expires and work should stop.
- Supervising process needs to reclaim resources.

## Happy path

1. Keep the task ID returned by `SendMessage`.
2. Issue `CancelTask` for that task ID.
3. Confirm task moves to a terminal canceled state via `GetTask` or stream events.

## Operational context

- Cancellation is best-effort; completion may race with cancellation.
- Make cancellation idempotent in higher-level workflows.
- Document user-visible semantics (for example, whether partial outputs are retained).

## Failure scenarios

- Unknown task ID.
- Task already completed.
- Task cannot be canceled due to policy/executor constraints.
