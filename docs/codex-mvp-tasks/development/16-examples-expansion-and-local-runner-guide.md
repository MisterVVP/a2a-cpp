# Task 16 — Examples expansion and local runnable scenarios

## Goal

Expand SDK examples from minimal snippets into a practical runnable matrix that users can execute locally to validate end-to-end behavior.

## Scope

- Keep current minimal examples and add focused runnable examples for:
  - `ListTasks` client flow
  - `CancelTask` client flow
  - push-notification config CRUD flow
  - `SubscribeTask` streaming flow
  - client interceptor usage (`BeforeCall` / `AfterCall` logging)
  - server-side auth metadata inspection and policy decision example
- Provide a single examples index with:
  - what each example demonstrates
  - build target name
  - run command
  - expected output snippet
- Add a convenience runner script (e.g., `scripts/run_examples.sh`) to build and run all examples sequentially.
- Add CI smoke job that builds all examples and runs a selected deterministic subset.

## Deliverables

- Additional `examples/*.cpp` programs.
- Expanded `examples/README.md` and quickstart cross-links.
- Example runner script.
- Example smoke tests or CI step.

## Acceptance criteria

- A user can copy/paste commands and run each example on a local machine.
- Every listed example compiles in CI.
- At least one example per major transport and advanced SDK capability exists.

## Out of scope

- Production deployment scaffolding.
