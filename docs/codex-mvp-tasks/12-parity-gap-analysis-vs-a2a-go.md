# Task 12 — A2A Go parity gap analysis and roadmap alignment

## Goal

Document feature gaps between the current C++ SDK MVP and the public `a2aproject/a2a-go` SDK surface, then convert those gaps into concrete, ordered implementation tasks.

## Scope

- Create and maintain a parity matrix for major SDK capabilities:
  - transport support (REST, JSON-RPC, gRPC)
  - client API surface (core RPCs + optional protocol methods)
  - server API surface and middleware/interceptors
  - observability and lifecycle hooks
  - examples/e2e coverage
- Mark each capability as one of:
  - parity achieved
  - partial parity
  - missing
- For partial/missing areas, link to follow-up task files in `docs/codex-mvp-tasks/`.

## Deliverables

- `docs/parity-a2a-go.md` (single source of truth parity matrix)
- updates to task ordering in `docs/codex-mvp-tasks/README.md`
- cross-links from parity matrix rows to actionable task files

## Constraints

- Compare against current public `a2aproject/a2a-go` mainline API/docs (not private branches).
- Keep parity statements evidence-based (pkg.go.dev/GitHub docs links).
- Do not block ongoing bug fixes while parity plan is prepared.

## Implementation notes

- Treat parity as **relative** to language/runtime idioms; exact API naming parity is not required.
- Focus first on behavior parity and capability parity.
- Track protocol-level methods absent in C++ but present in Go as high-priority backlog items.

## Acceptance criteria

- A reviewer can identify exactly which a2a-go capabilities are missing in C++.
- Every missing/partial item is linked to a follow-up implementation task.
- Task ordering reflects dependencies and avoids circular planning.

## Out of scope

- Immediate implementation of all parity tasks in one change.
- Non-A2A product concerns unrelated to SDK capability parity.
