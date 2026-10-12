# Shared-binding SUT scenarios

The TCK SUT exposes JSON-RPC at `/rpc`, HTTP+JSON at `/a2a`, and gRPC
on the HTTP port plus one. `SutAgent` owns one SDK executor, dispatcher,
card provider, task store, push store, and authorization policy. All three
SDK transport adapters share that dispatcher. SUT HTTP handlers only manage
connections; protocol parsing, serialization, streaming, and error mapping
are SDK behavior.

`shared_binding_test` uses public C++ SDK clients against the running SUT.
The Python launcher starts isolated SUT processes for each profile. Every
scenario runs for all nine ordered binding pairs, including a task created
through one binding and read, continued, canceled, or subscribed to through
another. Failures include the operation, binding pair, backend, and profile.

| Requirement | Scenarios |
|---|---|
| `BIND-EQUIV-001` | Shared capabilities, task/history/artifact lifecycle, direct messages, list pages, and push CRUD |
| `BIND-EQUIV-002` | Exact cross-binding task identity and content; streaming events and live subscription snapshots |
| `BIND-EQUIV-003` | Task-not-found, terminal subscription, and capability-negative errors with binding-specific codes |
| `BIND-EQUIV-004` | Authenticated task creation and access; missing/wrong credentials rejected across bindings |

Stored task comparisons use protobuf semantic equality without removing fields.
Two independent direct-message sends generate different task/message IDs; only
those generated IDs are excluded, and both returned identities are checked.
Subscription initial snapshots omit history and artifacts by the SDK's documented
snapshot contract; their remaining fields are compared exactly. Streaming sends
reuse a single task identity, so their events need no normalization.

## Profiles

- `A2A_TCK_PROFILE=default`: streaming and push notifications enabled.
- `A2A_TCK_PROFILE=capability_negative`: both disabled, as requested by #110.
  Shared dispatch rejects streaming/subscription with `UnsupportedOperationError`
  and push operations with `PushNotificationNotSupportedError`.
- Set `A2A_TCK_AUTH_TOKEN` to require a bearer credential for every dispatched
  operation. This deterministic test profile grants access to the shared agent
  to that credential; it is not a multi-tenant authorization implementation.
- Existing `A2A_TCK_EXTENDED_AGENT_CARD_MODE=configured|declared_only|disabled`
  profiles continue to use the shared card provider. `sut_agent_test` covers
  their declared capabilities and configured/not-configured behavior.

Authentication and capability failures come from an SDK `ServerInterceptor`.
SDK HTTP adapters preserve HTTP 401/403 for authentication; the SDK gRPC adapter
maps these failures to `UNAUTHENTICATED`/`PERMISSION_DENIED`. Other JSON-RPC
protocol failures retain HTTP 200 with their prescribed JSON-RPC error codes.

## Reproduction

```bash
cmake -S . -B build -DA2A_ENABLE_TESTING=ON -DA2A_ENABLE_POSTGRES_STORE=ON
cmake --build build --parallel
A2A_TEST_POSTGRES_DSN=postgresql://a2a:a2a@127.0.0.1:5432/a2a \
  ctest --test-dir build -L binding_equivalence --output-on-failure
```

The standalone CTest scenarios require Python and libcurl. PostgreSQL developers
must supply the DSN above to run that backend. The mandatory TCK entrypoint uses
strict mode: missing binaries, missing PostgreSQL credentials, empty scenario
selection, disabled tests, skipped tests, and failed assertions are failures.
Each live scenario group uses an isolated SUT process and PostgreSQL schema.

## TCK integration and required evidence

`scripts/run_tck_mandatory.sh` builds the SDK scenario binaries and temporarily
adds `tests/tck/test_cpp_sdk_requirements.py` to the upstream compatibility test
directory. Upstream tests and assertions stay intact. The supplemental module
executes the C++ assertions and records their actual outcomes through the
upstream `CompatibilityCollector`. It is removed from the TCK checkout on exit.

`compatibility.json` and `compatibility.html` include all four `BIND-EQUIV`
requirements with per-binding results and executable test IDs. GTest XML and
process logs are retained under `sdk-scenarios/<requirement>/<binding>/` in the
backend's report directory. The final report gate requires **PASS on all three
bindings**, including SDK scenario evidence for each binding; `SKIPPED`,
`NOT TESTED`, missing evidence and failures cannot satisfy the gate. The same
entrypoint runs in CI for both in-memory and PostgreSQL stores.

The capability-negative profile also executes `CORE-CAP-001/002` assertions on
all bindings. Conditional upstream tests may skip when their precondition is
absent in the default profile; those skips are retained in the report rather
than hidden. The nine binding/version requirements themselves must have complete
passing coverage. See [reproduction and evidence](README.md).

These tests validate this SDK's bindings. Cross-SDK interoperability remains
scoped to #111, and full TCK compliance requires the other registered requirements
to pass as well.
