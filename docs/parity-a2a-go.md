# A2A C++ vs A2A Go parity snapshot

_Last updated: 2026-05-02._

This matrix is a lightweight capability comparison against public a2a-go docs (GitHub README and pkg.go.dev API pages).

## Capability matrix

| Capability | a2a-go | a2a-cpp (current) | Status |
|---|---|---|---|
| REST client transport | Present | Present | ✅ parity |
| JSON-RPC client transport | Present | Present | ✅ parity |
| gRPC server/client transport path | Present (`a2agrpc`, gRPC handler in README flow) | Not implemented in SDK runtime | ❌ gap |
| Streaming send/subscription | Present | Present | ✅ parity |
| Client interceptors (`Before`/call interceptor pipeline) | Present | Not available as first-class API | ❌ gap |
| Server call interceptors/middleware hooks | Present | Partial (auth context extraction only, no generic interceptor chain) | ⚠️ partial |
| `ListTasks` style client API surface | Present in a2a-go transport docs | Missing on public `A2AClient` | ❌ gap |
| Extended card retrieval (`GetExtendedAgentCard`) | Present in a2a-go docs | Missing | ❌ gap |
| CLI utility for discovery/send/serve workflows | Present (`cmd/a2a`) | Missing | ❌ gap |
| Standard package registry release signal | Present (`go get .../v2`, pkg.go.dev indexed) | Not yet published via mainstream C++ registries | ❌ gap |
| Maintainer readiness signals (templates/governance cadence docs) | Present in repo metadata/workflows | Partial in current repo | ⚠️ partial |

## Follow-up tasks created from gaps

- `docs/codex-mvp-tasks/12-parity-gap-analysis-vs-a2a-go.md`
- `docs/codex-mvp-tasks/13-grpc-transport-parity.md`
- `docs/codex-mvp-tasks/14-client-server-api-parity-extended-rpcs-and-interceptors.md`
- `docs/codex-mvp-tasks/15-test-coverage-and-quality-gates.md`
- `docs/codex-mvp-tasks/16-examples-expansion-and-local-runner-guide.md`
- `docs/codex-mvp-tasks/17-google-sdk-readiness-checklist.md`

## Notes

- This parity view is capability-oriented, not API-name parity.
- The next iteration should keep this file updated as tasks 12–14 land.
