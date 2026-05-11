# Task 1 — Add TCK CI Workflow

## Objective

Create a dedicated GitHub Actions workflow that executes protocol conformance tests via TCK and enforces mandatory-category pass criteria.

## Scope

- Add `.github/workflows/tck.yml`.
- Add script support under `scripts/` to start/stop a deterministic C++ SUT server for TCK execution.
- Add local reproducibility instructions to documentation.

## Required implementation steps

1. **Create workflow skeleton**
   - Trigger on `pull_request`, `push` to `main`, and `workflow_dispatch`.
   - Use pinned action versions.
   - Use Ubuntu runner matching primary CI baseline.

2. **Prepare deterministic environment**
   - Install exact build/runtime dependencies for C++, Python (if needed by harness), and networking tools.
   - Pin TCK source ref via env var (default to stable branch/tag used by the community guidance).

3. **Start SDK under test (SUT)**
   - Add `scripts/run_tck_sut.sh` that:
     - builds required target(s),
     - starts server with explicit host/port,
     - performs readiness polling with bounded timeout,
     - emits clear logs and non-zero exit code on startup failures.

4. **Run TCK**
   - Clone TCK repository at pinned ref.
   - Execute mandatory category suite first.
   - Fail workflow on any mandatory test failure.

5. **Collect artifacts**
   - Upload TCK report files, server logs, and runner diagnostics.
   - Ensure artifacts are available on failure and success.

6. **Document local execution**
   - Add commands to a docs page so maintainers can reproduce CI runs locally.

## Acceptance criteria

- `tck.yml` exists and runs automatically on PRs.
- Mandatory TCK category pass is enforced.
- Artifacts include enough diagnostics to debug failures.
- Local reproducibility instructions are complete and validated.

## Non-goals

- Running all optional/recommended categories on every PR if runtime is excessive (can be scheduled separately).
