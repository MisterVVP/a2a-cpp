# A2A C++ vs A2A Go/Python parity snapshot

_Last updated: 2026-05-08._

This matrix compares public `a2a-go` v2 and `a2a-python` SDK surfaces against current `a2a-cpp` repository state and backlog.

## Capability matrix

| Capability | a2a-go | a2a-python | a2a-cpp (current) | Status |
|---|---|---|---|---|
| REST client transport | Present | Present | Present | ✅ parity |
| JSON-RPC client transport | Present | Present | Present | ✅ parity |
| gRPC server/client transport path | Present | Present | Present (client + server transports with integration tests) | ✅ parity |
| Streaming send/subscription | Present | Present | Present | ✅ parity |
| Client call interceptors / middleware | Present (`WithCallInterceptors`) | Present (middleware/extensibility hooks) | Present (`A2AClient` interceptor hooks) | ✅ parity |
| Server interceptor chain | Present (`InterceptedHandler`) | Present (server middleware/extensibility) | Present (`Dispatcher` interceptor hooks) | ✅ parity |
| `ListTasks` API | Present | Present | Present on public `A2AClient` and server transports | ✅ parity |
| `GetExtendedAgentCard` API | Present | Present | Present as first-class API | ✅ parity |
| Push-config lifecycle APIs (`ListTaskPushConfigs`, etc.) | Present | Present | Present across client transports | ✅ parity |
| CLI workflow utility | Present (`cmd/a2a`) | Present (CLI package in repo) | Missing | ❌ gap |
| Standard package registry signal | Present (`go get`, pkg.go.dev indexed) | Present (`pip install a2a-sdk`, PyPI) | Partial (Conan + vcpkg workflow exists; public-registry proof still incomplete) | ⚠️ partial |
| Maintainer readiness signals (templates, governance cadence, support policy) | Present | Present | Partial | ⚠️ partial |

## Gap-to-task mitigation map

Every non-parity row is mapped to one or more concrete tasks:

| Gap / partial area | Mitigation task(s) |
|---|---|
| gRPC server/client transport parity completion | `docs/codex-mvp-tasks/13-grpc-transport-parity.md` |
| Missing CLI utility workflows | `docs/codex-mvp-tasks/16-examples-expansion-and-local-runner-guide.md`, `docs/codex-mvp-tasks/20-cross-sdk-parity-vs-a2a-python.md` |
| Registry publication evidence and release automation | `docs/codex-mvp-tasks/17-google-sdk-readiness-checklist.md`, `docs/codex-mvp-tasks/18-spec-conformance-and-sdk-application-readiness.md` |
| Maintenance/governance readiness signals | `docs/codex-mvp-tasks/17-google-sdk-readiness-checklist.md`, `docs/codex-mvp-tasks/18-spec-conformance-and-sdk-application-readiness.md` |
| Python-SDK-specific parity deltas and validation loop | `docs/codex-mvp-tasks/20-cross-sdk-parity-vs-a2a-python.md` |
| Deterministic C++ ↔ Python interop CI path | `docs/codex-mvp-tasks/21-python-cross-sdk-interop-ci.md` |
| Python CLI workflow parity closure | `docs/codex-mvp-tasks/22-python-cli-workflow-parity.md` |
| Windows dependency-install bottleneck affecting CI confidence/velocity | `docs/codex-mvp-tasks/19-windows-build-dependency-acceleration.md` |

## Python-focused parity expansion (task 20 execution view)

The following Python-facing deltas are tracked as explicit, ordered implementation tasks to ensure no gap remains unowned:

1. Baseline parity and backlog synchronization: `20-cross-sdk-parity-vs-a2a-python.md`.
2. Deterministic CI interop path (C++ ↔ Python): `21-python-cross-sdk-interop-ci.md`.
3. CLI workflow parity closure decision + implementation: `22-python-cli-workflow-parity.md`.

## Notes

- This parity view is capability-oriented, not API-name parity.
- Cross-language parity should optimize for behavior-level interoperability over naming-level equivalence.
- Tasks 18–20 are intended to close submission-readiness and cross-SDK parity confidence gaps after core protocol parity work.

## Package publishing workload (release automation)

The package publishing workload is implemented in `.github/workflows/release-packages.yml` and is designed to be deterministic for release evidence collection and parity-readiness tracking.

### Triggers

- Automated release: push a tag matching `v*` (for example, `v0.2.0`).

### Required repository secrets

- `GITHUB_TOKEN`: provided by GitHub Actions runtime and used to publish release assets on tag-triggered runs.

### What the workload does

1. Builds source release archives (`.tar.gz`, `.zip`) and `SHA256SUMS.txt`.
2. For tag pushes (`v*`), publishes those assets to the GitHub Release for that tag.
3. Verifies `vcpkg.json` exists.
4. Publishes `vcpkg-submission-notes` artifact containing the public vcpkg registry submission checklist.

### How to run it

1. Ensure secrets are configured in the GitHub repository settings.
2. Go to **Actions → Release Packages**.
3. Create/push a release tag:
   - `git tag vX.Y.Z`
   - `git push origin vX.Y.Z`
4. Validate success by checking:
   - `github-release-artifacts` job passed.
   - On tag runs, the release contains `.tar.gz`, `.zip`, and `SHA256SUMS.txt` assets.
   - `vcpkg-metadata-check` job passed.
   - `vcpkg-submission-notes` artifact is attached to the run.


## Parity evidence

- CI workflow job `python-cross-sdk-interop` in `.github/workflows/ci.yml` runs `scripts/run_python_cross_sdk_interop.sh` with `A2A_PYTHON_REF` (currently set to `main`) to validate deterministic cross-SDK compatibility signals.
- gRPC parity is evidenced by both client and server transports in `include/a2a/client/grpc_transport.h` and `include/a2a/server/grpc_server_transport.h`, with integration coverage in `tests/integration/grpc_transport_integration_test.cpp`.
- `GetExtendedAgentCard` is now implemented as a first-class SDK API surface; parity status is updated accordingly.

- Mixed-runtime interoperability is validated in CI via `python-server-cpp-client-interop` and `cpp-server-python-client-interop`; workflow and contract details are documented in `docs/python-cpp-interop.md`.
