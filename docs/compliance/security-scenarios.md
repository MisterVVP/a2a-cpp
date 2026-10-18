# SDK security scenarios

The TCK SUT's `security` profile shares SDK policy, storage and execution across
JSON-RPC, HTTP+JSON and gRPC. Its two identities, permission rules, keys and
credential trigger are application fixtures; enforcement resides in public SDK APIs.

```bash
./scripts/install_build_deps.sh
./scripts/install_security_test_deps.sh
cmake -S . -B build -DA2A_ENABLE_POSTGRES_STORE=ON -DA2A_ENABLE_SIGNING=ON
cmake --build build --parallel 2
A2A_TEST_POSTGRES_DSN=postgresql://a2a:a2a@127.0.0.1:5432/a2a \
  ctest --test-dir build -L security --output-on-failure
```

Signing is optional and disabled by default. Permissions, task authorization and
canonicalization also run without it. Ordinary builds need neither nginx nor
the OpenSSL CLI. Contributor validation uses `A2A_VERIFY_SIGNING=ON` with
`./scripts/verify_changes.sh`.

SDK unit/adapter tests run on supported platforms. Live scenarios require Unix,
Python, libcurl, nginx and the OpenSSL CLI; PostgreSQL requires
`A2A_TEST_POSTGRES_DSN`, otherwise that scenario explicitly skips. The launcher
isolates temporary files, loopback ports and database schemas and cleans up afterward.
Both stores run all nine ordered binding pairs for permissions and streaming,
including authorization with two simultaneously active streams. TLS checks run
once per target binding for plaintext/trusted/unrelated CA/expired/hostname-mismatch
profiles; each discovery check runs once per positive profile. All calls use public C++ SDK clients.

| SDK behavior | Independent test suite | Requirements |
| --- | --- | --- |
| Caller permissions | `SdkSecurityTest`, `SecurityTransportIntegrationTest` | `AUTH-SERVER-002`, `AUTH-SCOPE-001..003` |
| Additional task authorization | `TaskAuthorizationServiceTest` | `AUTH-INTASK-001..006` |
| TLS/server identity | `SecurityTransportIntegrationTest`, live scenarios | `AUTH-TLS-001/002`, `AUTH-SERVER-001`, `GRPC-SVC-003` |
| Signed cards | `JsonCanonicalizationTest`, `AgentCardSigningTest` | `CARD-SIGN-001..004` |

## Caller permissions

Configure the dispatcher with its executor's authoritative store:

```cpp
dispatcher.SetSecurityPolicy(policy, stores.task_store.get());
```

`Authenticate` returns an identity or SDK error, `AuthorizeOperation` controls
operations, and `CanAccessTask` evaluates authoritative tasks. The SDK clears and
reauthenticates `RequestContext::principal` each call, preserving it across
interceptors and executor callbacks. Applications own token validation, identity
lifetime, ownership and scope rules. Synchronize mutable
policy data: callbacks can run concurrently. Client-supplied context IDs are not identities.

Checks cover reads, cancellation, continuation, subscriptions and all push
configuration operations. Hidden and missing tasks return the same canonical
`TaskNotFoundError`, including message text. Missing/invalid credentials map to
HTTP 401 or gRPC `UNAUTHENTICATED`; operation denials to HTTP 403 or
`PERMISSION_DENIED`. Public card discovery stays public; extended cards use
authenticated dispatch. Returned tasks from sends, reads and cancellation are
checked before release; tasks without IDs are rejected before calling the policy.
Returned push configurations, including every list entry,
and associated messages are checked against the store; taskless messages are supported. Denials reach `AfterDispatch` in reverse order.
Secured `GetTask` and unary `SendMessage` calls request the full executor response for authorization, then
apply the caller's history limit before returning it or invoking audit hooks.

Listing checks permissions before counting/pagination. `ScopeTaskList` can narrow
store filters/projections or return `nullopt` for an empty scope. Retain fields
needed by `CanAccessTask`; defaults preserve full tasks. The SUT pushes its
context filter into both stores, loading histories/artifacts only for selected
results. The dispatcher retains one bounded batch and one page of IDs. Exact
counts still inspect all matching tasks; backends may repeat filtering/counting
or decode protobuf blobs before projection. Numeric offsets/ordering remain;
cursor pagination, update-time ordering and snapshot consistency are tracked in
[ListTasks #309](https://github.com/MisterVVP/a2a-cpp/issues/309).

Streams check current task permissions before reading and releasing events,
reject task-bearing events without IDs, and allow taskless messages. On a stream
associated with a task, even taskless messages recheck that task after reading,
so revocation during a blocking read is enforced. Streams preserve timed reads,
live semantics and cancellation. Keep policies/stores alive
until sessions finish. Credentials are authenticated on connection; include
revocation/expiry in `CanAccessTask` for subsequent events. New SUT streams use
SDK-generated IDs so reused message IDs cannot select another caller's task.

## Task authorization

`TaskAuthorizationService` combines storage, lifecycle, history and subscriptions:

```cpp
a2a::server::TaskAuthorizationService authorization(
    stores.task_store.get(), validate_credential);
auto pending = authorization.RequestAuthorization(request, context, explanation);
auto resumed = authorization.Authorize(task_id, out_of_band_credential);
auto completed = authorization.Complete(task_id);
```

`RequestAuthorization` requires an agent explanation with a message ID and part;
it persists `TASK_STATE_AUTH_REQUIRED` and `TaskStatus::message`. `ReceiveMessage`
validates task/context association and deduplicates history by message ID without
granting authorization. Invalid credentials leave state unchanged. `Authorize`
validates credentials and moves to `WORKING`; `Complete` rejects pending tasks.
Cancellation remains available while pending.
Transitions reject tasks without the service's authorization marker, so cancellation
cannot mutate an unrelated ordinary task.

Route mutations/subscriptions through this service and keep its store alive.
Send streams and subscriptions remain live during `auth_required` and receive
working/completed events afterward. A mutex serializes mutations;
`SendStreamingMessage` registers its stream before continuation can run.
Separate creation/subscription calls can miss completion between them.

Both stores support the required conditional writes. Each transition clears the
previous explanation and commits the complete task in one revision-checked write;
only success publishes events. Failures preserve prior state; conflicts report
errors instead of overwriting cancellation/history. Validation is not retried
automatically, so validators should be safe to repeat. All writers must respect
revisions: unconditional writes can replace committed state afterward.

## TLS and server identity

HTTP bindings accept `CallOptions::tls`, the shared client `Request::tls`, and
discovery `MakeDefaultHttpFetcher` TLS options, for unary and streaming requests:

```cpp
a2a::client::CallOptions options;
options.tls = a2a::http::TlsOptions{.ca_bundle_path = ca_bundle_path,
                                     .require_tls13 = true};
auto task = client.GetTask(request, options);
```

libcurl verifies certificates/hostnames. Absent options or empty CA paths preserve
compiled default trust even after pooled custom-CA use; transfer errors retain
detailed diagnostics. Ordinary defaults require TLS 1.2+, explicit options default
to TLS 1.3; set `require_tls13=false` for TLS 1.2. HTTP supports local fixtures;
applications must enforce HTTPS ingress in production.

gRPC uses native `grpc::SslCredentials`/`grpc::SslServerCredentials`: supply PEM
roots on channels and certificate/key pairs on the server builder. Avoid production
target-name overrides. HTTP adapters use application-owned byte transports; the
fixture uses nginx TLS 1.3 termination, loopback upstreams and SSE buffering disabled.
Protect proxy-to-adapter traffic with same-host loopback or another secured connection.
The [launcher](../../tests/scripts/run_security_scenarios.py) generates isolated CAs
and fixed-validity certificates and checks trust, expiry and hostname failures
without disabling verification or contacting external services.

## Signed Agent Cards

Enable `A2A_ENABLE_SIGNING=ON` and link `a2a::signing` for OpenSSL 3.0+ Ed25519.
Only signing adds this direct dependency. With vcpkg select
`VCPKG_MANIFEST_FEATURES=signing`; installed packages expose
`A2A_CPP_HAS_AGENT_CARD_SIGNING` and the enabled target. Canonicalization stays in
`a2a::core` via `a2a/core/json_canonicalization.h`.

```cpp
auto signed_card = a2a::core::SignAgentCard(card, key_id, private_key_pem);
auto verified = a2a::core::VerifyAgentCardJson(raw_json, key_resolver, now);
discovery.SetAgentCardVerifier([&](std::string_view json) {
  return a2a::core::VerifyAgentCardJson(json, key_resolver, current_time());
});
```

Protected JWS headers contain `alg: EdDSA`, nonempty `kid`, and `typ: JOSE`.
RFC 8785 JCS canonicalizes the payload after removing `signatures`.
`CanonicalizeJson` rejects duplicate keys, invalid Unicode and nonfinite numbers,
sorts members by UTF-16 code units, uses ECMAScript number thresholds and retains
array order. Limits are 1 MiB and 128 nesting levels. Strict syntax validation
and existing ProtoJSON/protobuf `Value` parsing require no new JSON library.

Verification preserves original JSON, including unknown fields. At least one
signature must verify with an application-trusted key; unknown/invalid/expired/
revoked signatures do not defeat another valid signature. Signing and verification
allow at most 16 signatures (`kMaxAgentCardSignatures`); oversized arrays are
rejected before resolver calls or cryptographic verification. Resolvers provide
keys, validity and revocation; callers supply time. Noncritical protected headers
(`typ`, `jku`, etc.) and unprotected annotations are accepted; overlapping names,
`crit` and unencoded payloads are rejected. ES256/RS256 are not implemented.
`jku` never fetches keys. Signing uses protobuf field-presence serialization;
empty/default-field differences await [A2A #2122](https://github.com/a2aproject/A2A/issues/2122).

Discovery verifies public and extended cards before parsing. Public cache hits
recheck signatures against current key policy. `AgentCardVerifier` names the
owning callback type; replacing or clearing it invalidates the cache. Invalid
cached cards are evicted and refreshed; refresh failure never returns the invalid
card. Without a verifier, unsigned behavior remains unchanged. Applications
choose signature requirements and provision trusted keys.

The [JCS corpus](../../tests/support/security/jcs_vectors.json) is Python SDK
`a2a-jcs-v01` at `0ee47d7baa47a706e33934a4e4c3d46bad16298f`, with independent
oracles/provenance retained. Two rejection cases check signature-field exclusion
separately from generic JSON syntax. The [Ed25519 vector](../../tests/support/security/sdk_signing_vector.json)
is Go SDK `a2acrypto/testdata/golden.json` at
`e85760e87ce007bfbd02e38374a4ce2fc2037fe3`, generated by its Python-SDK
`gen_golden.py` and reproduced by Go tests. Both SDKs use Apache-2.0.
Additional header cases use OpenSSL CLI and the published test key; Python sorted,
compact ASCII JSON matches the golden payload. All fixture keys are public test
material, never deployment credentials.

## SUT configuration and upstream reporting

```bash
unset A2A_TCK_AUTH_TOKEN
export A2A_TCK_PROFILE=security
export A2A_TCK_ALICE_TOKEN='<caller fixture A>'
export A2A_TCK_BOB_TOKEN='<caller fixture B>'
export A2A_TCK_CREDENTIAL_DIRECTORY='<private fixture directory>'
export A2A_TCK_TASK_CREDENTIAL='<out-of-band credential fixture>'
./scripts/run_tck_sut.sh
```

Tokens must be distinct/nonempty; messages use `contextId=alice`/`bob` with the
matching identity. A `request-authorization` text part creates a pending task.
The fixture reader authorizes/completes it when a file named after its generated
ID exactly matches the configured credential. Keep files/directories owner-readable
only. This local fixture is not an A2A credential endpoint or production identity system.

Set `A2A_TCK_TLS_CERT_FILE` and `A2A_TCK_TLS_KEY_FILE` together for native gRPC
TLS, and `A2A_TCK_PUBLIC_HTTPS_BASE` for externally terminated HTTP card URLs;
secure HTTP listeners remain loopback. `A2A_TCK_PUBLIC_GRPC_TARGET` sets the
certificate hostname and gRPC port advertised to discovery clients.
`A2A_TCK_SIGNING_KEY_FILE` and `A2A_TCK_SIGNING_KEY_ID` enable card signing,
including re-signing extended cards.

CI runs SDK evidence separately from the unmodified upstream mandatory runner.
At `263b9cfaf16a554bdfb166a7ba5b67716e946349`, upstream security groups lack
executable assertions and remain **NOT TESTED**. SDK tests do not raise that
percentage; no local TCK assertions/report gates are added. Follow-ups:
[a2a-cpp #305](https://github.com/MisterVVP/a2a-cpp/issues/305),
[TCK task 27](https://github.com/a2aproject/a2a-tck/blob/main/backlog/tasks/task-27%20-%20Add-auth-security-conformance-tests-AUTH.md),
[TCK task 29](https://github.com/a2aproject/a2a-tck/blob/main/backlog/tasks/task-29%20-%20Add-agent-card-signing-conformance-tests-CARD-SIGN.md)
and `GRPC-SVC-003`.
