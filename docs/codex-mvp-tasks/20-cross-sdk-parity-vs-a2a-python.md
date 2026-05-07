# Task 20 — Cross-SDK parity expansion vs a2a-python

## Goal

Close remaining capability and ecosystem parity gaps between `a2a-cpp` and the public `a2aproject/a2a-python` SDK, while preserving C++-idiomatic APIs.

## Scope

- Maintain a Python-focused parity section in `docs/parity-a2a.md`.
- Validate feature parity for:
  - protocol coverage (REST, JSON-RPC, gRPC)
  - optional capabilities (CLI workflows, telemetry hooks, extension points)
  - interoperability behavior for task lifecycle and streaming
- Add missing parity items as implementation tasks with clear ordering and dependencies.
- Add an interop CI scenario that exercises C++ client/server flows against Python sample agents.

## Deliverables

- Updated parity matrix entries and links in `docs/parity-a2a.md`.
- New/updated implementation tasks for any missing Python parity features.
- CI/docs evidence that Python cross-SDK interop runs deterministically.

## Constraints

- Use public upstream references only (`a2aproject/a2a-python` docs and code).
- Focus on behavior parity, not exact naming parity.
- Keep tests deterministic and runnable in standard CI environments.

## Acceptance criteria

- Reviewer can trace every identified Python parity gap to a concrete task.
- At least one automated cross-SDK (C++ ↔ Python) interop path is documented and CI-runnable.
- No untracked Python parity gap remains in the parity matrix.

## Out of scope

- Full duplication of every Python-specific optional integration inside the C++ SDK.
