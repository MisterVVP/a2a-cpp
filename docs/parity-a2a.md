# A2A C++ vs A2A Go/Python parity snapshot

_Last updated: 2026-05-07._

This matrix compares public `a2a-go` v2 and `a2a-python` SDK surfaces against current `a2a-cpp` repository state and backlog.

## Capability matrix

| Capability | a2a-go | a2a-python | a2a-cpp (current) | Status |
|---|---|---|---|---|
| REST client transport | Present | Present | Present | ✅ parity |
| JSON-RPC client transport | Present | Present | Present | ✅ parity |
| gRPC server/client transport path | Present | Present | Client transport present; server runtime parity still incomplete | ⚠️ partial |
| Streaming send/subscription | Present | Present | Present | ✅ parity |
| Client call interceptors / middleware | Present (`WithCallInterceptors`) | Present (middleware/extensibility hooks) | Present (`A2AClient` interceptor hooks) | ✅ parity |
| Server interceptor chain | Present (`InterceptedHandler`) | Present (server middleware/extensibility) | Present (`Dispatcher` interceptor hooks) | ✅ parity |
| `ListTasks` API | Present | Present | Present on public `A2AClient` and server transports | ✅ parity |
| `GetExtendedAgentCard` API | Present | Present | Missing as first-class API | ❌ gap |
| Push-config lifecycle APIs (`ListTaskPushConfigs`, etc.) | Present | Present | Present across client transports | ✅ parity |
| CLI workflow utility | Present (`cmd/a2a`) | Present (CLI package in repo) | Missing | ❌ gap |
| Standard package registry signal | Present (`go get`, pkg.go.dev indexed) | Present (`pip install a2a-sdk`, PyPI) | Partial (Conan + vcpkg workflow exists; public-registry proof still incomplete) | ⚠️ partial |
| Maintainer readiness signals (templates, governance cadence, support policy) | Present | Present | Partial | ⚠️ partial |

## Gap-to-task mitigation map

Every non-parity row is mapped to one or more concrete tasks:

| Gap / partial area | Mitigation task(s) |
|---|---|
| gRPC server/client transport parity completion | `docs/codex-mvp-tasks/13-grpc-transport-parity.md` |
| `GetExtendedAgentCard` missing API | `docs/codex-mvp-tasks/14-client-server-api-parity-extended-rpcs-and-interceptors.md` |
| Missing CLI utility workflows | `docs/codex-mvp-tasks/16-examples-expansion-and-local-runner-guide.md`, `docs/codex-mvp-tasks/20-cross-sdk-parity-vs-a2a-python.md` |
| Registry publication evidence and release automation | `docs/codex-mvp-tasks/17-google-sdk-readiness-checklist.md`, `docs/codex-mvp-tasks/18-spec-conformance-and-sdk-application-readiness.md` |
| Maintenance/governance readiness signals | `docs/codex-mvp-tasks/17-google-sdk-readiness-checklist.md`, `docs/codex-mvp-tasks/18-spec-conformance-and-sdk-application-readiness.md` |
| Python-SDK-specific parity deltas and validation loop | `docs/codex-mvp-tasks/20-cross-sdk-parity-vs-a2a-python.md` |
| Windows dependency-install bottleneck affecting CI confidence/velocity | `docs/codex-mvp-tasks/19-windows-build-dependency-acceleration.md` |

## Notes

- This parity view is capability-oriented, not API-name parity.
- Cross-language parity should optimize for behavior-level interoperability over naming-level equivalence.
- Tasks 18–20 are intended to close submission-readiness and cross-SDK parity confidence gaps after core protocol parity work.
