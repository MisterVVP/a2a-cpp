# Task 2 — Add ITK Interop CI Workflow

## Objective

Create a dedicated CI workflow that validates multi-agent interoperability through ITK-style scenarios with deterministic execution.

## Scope

- Add `.github/workflows/itk.yml`.
- Add script support under `scripts/` for end-to-end ITK execution.
- Add/adjust interop test assets required by the scenario.

## Required implementation steps

1. **Create workflow skeleton**
   - Trigger on `pull_request`, `push` to `main`, and `workflow_dispatch`.
   - Use pinned versions of external actions and repositories.

2. **Build and launch C++ interop agent**
   - Add deterministic launch script (for example `scripts/run_itk_cpp_agent.sh`).
   - Use explicit ports, fixed startup timeouts, and health checks.
   - Define clean teardown behavior to avoid orphaned processes.

3. **Run ITK harness scenario**
   - Clone interop harness repository using pinned commit SHA.
   - Launch dependent agents/services required by the scenario.
   - Execute the hop-chain interop run and fail on mismatch.

4. **Harden determinism**
   - Remove implicit sleeps where possible; use readiness probes.
   - Standardize timeouts and retries with named constants.
   - Capture environment metadata (refs, versions, ports) in logs.

5. **Collect artifacts**
   - Upload harness reports, per-agent logs, and consolidated summary.
   - Ensure retained artifacts are enough for post-failure triage.

6. **Document local execution**
   - Add a section describing prerequisites and exact run commands.

## Acceptance criteria

- `itk.yml` exists and runs on PRs.
- ITK scenario is deterministic across repeated runs.
- Logs and reports are published as artifacts.
- Local reproduction steps are documented and verified.

## Non-goals

- Exhaustive matrix across every optional language/runtime in a single PR job.
