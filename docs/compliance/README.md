# Community SDK Compliance Task Pack

This folder contains implementation-ready tasks for AI agents to bring this repository into alignment with community SDK verification expectations.

## Task Index

1. [Task 1: Add TCK CI Workflow](./task-1-tck-ci-workflow.md)
2. [Task 2: Add ITK Interop CI Workflow](./task-2-itk-ci-workflow.md)
3. [Task 3: Publish Compliance Evidence in Documentation](./task-3-compliance-evidence-docs.md)

## Execution order

Run tasks in the listed order. Task 1 establishes protocol conformance gating, Task 2 establishes interop verification, and Task 3 publishes auditable evidence.

## Definition of done

- TCK workflow exists, passes mandatory categories, and uploads artifacts.
- ITK workflow exists, runs deterministic interop scenarios, and uploads artifacts.
- Documentation clearly maps requirements to CI jobs and evidence links.

## Local reproduction for Task 1 (TCK CI)

Use the same deterministic workflow as `.github/workflows/tck.yml`:

```bash
# 1) Check out the TCK and prepare Python dependencies.
TCK_REPO=${TCK_REPO:-a2aproject/a2a-tck}
TCK_REF=${TCK_REF:-main}
git clone --depth 1 --branch "${TCK_REF}" "https://github.com/${TCK_REPO}.git" tck-repo
python3 -m venv tck-repo/.venv
source tck-repo/.venv/bin/activate
python3 -m pip install --upgrade pip
python3 -m pip install -e ./tck-repo python-dotenv pyopenssl

# 2) Run the repository entrypoint from the repository root.
export TCK_WORKDIR=tck-repo
export TCK_TRANSPORTS=grpc,jsonrpc,http_json
export A2A_TCK_REQUIRED_EXTENSIONS=urn:a2a:tck:required-extension
export A2A_TCK_STORE_BACKEND=inmemory
export TCK_REPORT_DIR=tck-artifacts/reports/inmemory
export TCK_LOG_DIR=tck-artifacts/logs/inmemory
./scripts/run_tck_sut.sh
trap './scripts/stop_tck_sut.sh' EXIT
./scripts/run_tck_mandatory.sh
./scripts/stop_tck_sut.sh

# 3) Repeat using a running PostgreSQL instance.
export A2A_TCK_STORE_BACKEND=postgres
export A2A_TCK_POSTGRES_DSN=postgresql://a2a:a2a@127.0.0.1:5432/a2a
export A2A_TCK_POSTGRES_SCHEMA=tck_postgres
export TCK_REPORT_DIR=tck-artifacts/reports/postgres
export TCK_LOG_DIR=tck-artifacts/logs/postgres
./scripts/run_tck_sut.sh
./scripts/run_tck_mandatory.sh
./scripts/stop_tck_sut.sh
trap - EXIT
```

Expected artifacts:
- `build-tck/tck-sut.log`
- `tck-artifacts/reports/{inmemory,postgres}/compatibility.{json,html}`
- `tck-artifacts/reports/{inmemory,postgres}/sdk-scenarios/**`
- `tck-artifacts/logs/{inmemory,postgres}/tck-run.log`

The mandatory runner adds executable C++ SDK coverage for requirements that
upstream registers without implementing dedicated tests. It leaves upstream
assertions intact and reports supplemental results through the upstream
collector. The runner fails unless `BIND-EQUIV-001..004`, `VER-CLIENT-001/002`,
and `VER-SERVER-001..003` pass on **all three bindings**, with SDK scenario
evidence for each. A custom `TCK_RUN_CMD` must execute the staged supplemental
module and produce the same evidence; it cannot bypass this check.

GTest XML and SUT/scenario logs are included under each backend report's
`sdk-scenarios` directory. Conditional upstream skips and other untested registry
requirements remain visible; none of the nine required binding/version checks
may be skipped or untested. Use `scripts/check_tck_sdk_requirements.py` to check
a saved report and `scripts/summarize_tck_report.py` to inspect remaining gaps.

## Verified requirement coverage

Local validation on 2026-10-12 used upstream TCK commit
[`263b9cfaf16a554bdfb166a7ba5b67716e946349`](https://github.com/a2aproject/a2a-tck/commit/263b9cfaf16a554bdfb166a7ba5b67716e946349)
with this repository's executable SDK requirement module:

| Evidence | In-memory | PostgreSQL |
|---|---|---|
| Nine binding/version requirements on all three bindings | PASS; no skips or untested coverage | PASS; no skips or untested coverage |
| Supplemental C++ scenarios | 252 passed, 0 skipped | 252 passed, 0 skipped |
| Full pytest suite, including SDK module | 282 passed, 16 conditional upstream skips | 282 passed, 16 conditional upstream skips |
| Mandatory requirement compliance | 87.5% | 87.5% |
| Overall requirement compliance | 85.8% | 85.8% |

The earlier upstream-only mandatory baseline was 80.9%. Additional executed
requirements improve that result; other registered requirements still lack
coverage. This evidence does not claim full TCK compliance. CI uploads the
backend reports, scenario XML, and logs through
[the TCK workflow](../../.github/workflows/tck.yml).

## Conformance SUT binary

The conformance scripts build and run the conformance-only `tck_sut` binary.
Wire-level performance tests use a separate `performance_sut` entry point with
performance diagnostics; both binaries delegate protocol and lifecycle behavior
to the same internal runtime. The exposed endpoints are REST at `/a2a`,
JSON-RPC at `/rpc`, and gRPC on the configured port plus one. Store selection
continues to use
`A2A_TCK_STORE_BACKEND=inmemory|postgres`, `A2A_TCK_POSTGRES_DSN`, and
`A2A_TCK_POSTGRES_SCHEMA`.

## Shared-binding and protocol-version evidence

- [Shared-binding SUT scenarios](binding-equivalence.md)
- [Protocol versions and SDK client/server checks](protocol-versions.md)
