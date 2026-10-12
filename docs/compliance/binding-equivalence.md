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

Live client scenarios require the SDK's libcurl-backed default HTTP support;
they are registered when that support and Python are available. The PostgreSQL
scenario is explicitly skipped if its DSN is absent. To validate
both backends, enable PostgreSQL at build time and provide the DSN. Each run
uses its own PostgreSQL schema. The normal CI PostgreSQL test job supplies the
DSN, so these scenarios run through its existing CTest entrypoint.

These are requirement-mapped SDK/SUT checks, separate from the generic upstream
TCK. The TCK currently lacks generic cross-binding/client-header assertions;
its existing operation assertions remain useful, and the local scenarios cover
those gaps without modifying or overriding the upstream assertions. Cross-SDK
interoperability remains scoped to #111.
