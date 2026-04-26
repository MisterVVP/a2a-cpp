# A2A C++ vs A2A Go parity snapshot

_Last updated: 2026-04-25._

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

## Follow-up tasks created from gaps

- `docs/codex-mvp-tasks/12-parity-gap-analysis-vs-a2a-go.md`
- `docs/codex-mvp-tasks/13-grpc-transport-parity.md`
- `docs/codex-mvp-tasks/14-client-server-api-parity-extended-rpcs-and-interceptors.md`

## Notes

- This parity view is capability-oriented, not API-name parity.
- The next iteration should keep this file updated as tasks 12–14 land.
