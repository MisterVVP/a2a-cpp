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
- Documented package-publishing workload runbook (manual + tag-triggered) so parity verification artifacts can be released consistently.

## Constraints

- Use public upstream references only (`a2aproject/a2a-python` docs and code).
- Focus on behavior parity, not exact naming parity.
- Keep tests deterministic and runnable in standard CI environments.

## Acceptance criteria

- Reviewer can trace every identified Python parity gap to a concrete task.
- At least one automated cross-SDK (C++ ↔ Python) interop path is documented and CI-runnable.
- No untracked Python parity gap remains in the parity matrix.
- Package publishing workflow invocation steps are documented with prerequisite secrets and expected artifacts.

## Implementation checklist

1. Refresh Python parity matrix rows in `docs/parity-a2a.md` and ensure each non-parity row maps to a task.
2. Add/verify an interop CI path that runs deterministic C++ client/server exchanges against Python sample agents.
3. Capture deterministic evidence links (CI job name, workflow file, and pass criteria) in docs.
4. Keep package publishing instructions current with `.github/workflows/release-packages.yml`:
   - manual run via `workflow_dispatch`
   - release run via `v*` git tags
   - required secrets and expected outputs (GitHub release source archives/checksums + vcpkg submission notes artifact)
5. Open/maintain explicit follow-up tasks for uncovered items:
   - deterministic interop CI execution path: `docs/codex-mvp-tasks/21-python-cross-sdk-interop-ci.md`
   - CLI workflow parity closure: `docs/codex-mvp-tasks/22-python-cli-workflow-parity.md`

## Out of scope

- Full duplication of every Python-specific optional integration inside the C++ SDK.
