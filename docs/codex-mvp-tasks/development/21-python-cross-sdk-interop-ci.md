# Task 21 — Deterministic C++ ↔ Python interop CI scenario

> **Status:** Planned / decision needed. Treat as parity follow-up requiring fresh validation before implementation.

## Goal

Add an automated, deterministic CI scenario that validates A2A protocol interoperability between `a2a-cpp` and public `a2aproject/a2a-python` sample agent flows.

## Scope

- Add a dedicated CI workflow job that executes a C++↔Python interop path.
- Pin Python dependency inputs (SDK version/tag/commit and Python toolchain version) to keep runs deterministic.
- Validate task lifecycle and at least one streaming path across SDK boundaries.
- Publish CI evidence references in `docs/parity-a2a.md`.

## Deliverables

- `.github/workflows/ci.yml` contains a `python-cross-sdk-interop` (or equivalent) job.
- A reproducible script under `scripts/` that runs the interop scenario locally and in CI.
- Documentation that explains prerequisites, pass criteria, and artifact/log locations.

## Constraints

- Use only public upstream references (`a2aproject/a2a-python`).
- No flaky timing dependencies; timeouts must be explicit and conservative.
- Keep execution time compatible with standard PR CI budgets.

## Acceptance criteria

- CI fails when interop contract breaks and passes when behavior is correct.
- Same script succeeds locally and in CI with equivalent inputs.
- `docs/parity-a2a.md` links to the CI job as parity evidence.
