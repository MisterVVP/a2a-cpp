# Task 15 — Test coverage baselines and quality gate hardening

## Goal

Establish measurable coverage baselines and enforce them in CI so regressions are blocked before release.

## Why now

The repository has broad unit/integration/functional test suites, but lacks an explicit line/branch coverage target and automated enforcement for release readiness.

## Scope

- Add coverage build profile (gcc/clang `--coverage` or `llvm-cov`) for Linux CI.
- Publish per-target and aggregate coverage reports.
- Set minimum thresholds (initially pragmatic, then ratchet upward):
  - `a2a_core`: line >= 85%
  - `a2a_client`: line >= 80%
  - `a2a_server`: line >= 80%
- Add a CI failure gate when thresholds are not met.
- Document local coverage workflow in `docs/build.md`.
- Add targeted missing-path tests where current coverage is weak (error paths, auth hooks, streaming cancellation races, push-notification RPC failures).

## Deliverables

- Coverage CMake preset or script.
- CI job producing downloadable HTML/XML report artifact.
- Threshold enforcement (e.g., `gcovr --fail-under-line` or equivalent).
- Documentation update with local commands.

## Acceptance criteria

- Coverage report is generated in CI on every PR.
- Threshold violations fail CI.
- Coverage numbers are visible in PR artifacts/logs.

## Out of scope

- Fuzzing and chaos testing (tracked separately).
