# Task 14 — Extended RPC and interceptor parity with a2a-go

## Goal

Close major API-surface gaps with a2a-go by adding missing protocol methods and interceptor/lifecycle extension points.

## Scope

- Add missing/partial client/server operations where supported by protocol and go SDK surface, prioritizing:
  - `ListTasks`
  - `GetExtendedAgentCard` (if retained in current protocol profile)
  - push config methods parity checks
- Add transport-agnostic call interceptors/hooks:
  - client pre/post call hooks
  - server request/response hooks
  - no-op passthrough helpers for extension ergonomics
- Add lifecycle surfaces where appropriate:
  - explicit `Destroy`/shutdown semantics for transports/clients

## Deliverables

- public interceptor APIs in `include/a2a/client/` and `include/a2a/server/`
- implementation wiring in transports
- tests validating ordering, context propagation, and error handling
- docs pages describing hook contracts and thread-safety guarantees

## Constraints

- Keep default path zero-config and backward-compatible.
- Avoid introducing global mutable state in interceptor plumbing.
- Preserve deterministic test behavior.

## Implementation notes

- Prefer composition around existing `ClientTransport`/dispatcher instead of deep inheritance trees.
- Make contracts explicit (callback thread, cancellation, ownership, exception policy).
- Add metrics/logging hook examples without hardwiring to a specific logging backend.

## Acceptance criteria

- A user can attach interceptors and observe request lifecycle events on both client and server paths.
- Added RPCs are available across supported transports where protocol-compatible.
- Functional and integration tests cover happy-path and failure-path interceptor behavior.

## Out of scope

- Opinionated tracing vendor SDK integrations.
- Full-blown policy engines beyond lightweight interceptor hooks.
