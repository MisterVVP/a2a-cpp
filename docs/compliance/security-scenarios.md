# SDK security scenarios

The TCK SUT's `security` profile shares one SDK dispatcher, security policy, task
store and executor across JSON-RPC, HTTP+JSON and gRPC. It supplies two identities,
a fixture permission policy, credentials, keys and an application trigger. The
security enforcement, authorization lifecycle, streams, TLS clients and card
signing/verification reside in public SDK APIs.

```bash
./scripts/install_build_deps.sh
./scripts/install_security_test_deps.sh
cmake -S . -B build -DA2A_ENABLE_POSTGRES_STORE=ON -DA2A_ENABLE_SIGNING=ON
cmake --build build --parallel 2
A2A_TEST_POSTGRES_DSN=postgresql://a2a:a2a@127.0.0.1:5432/a2a \
  ctest --test-dir build -L security --output-on-failure
```

Signing is disabled by default. Ordinary builds do not require nginx or the
OpenSSL CLI. Canonicalization, permissions and task-authorization tests also run
with signing disabled. For canonical contributor validation use
`A2A_VERIFY_SIGNING=ON` with `./scripts/verify_changes.sh`.

SDK unit and adapter integration tests run through CTest on supported platforms.
Live SUT scenarios require Unix, Python, libcurl, nginx and the OpenSSL CLI and
are registered when those tools are present. PostgreSQL requires
`A2A_TEST_POSTGRES_DSN`; its absence produces an explicit skip. Each launcher run
uses temporary credential/CA files, free loopback ports and an isolated database
schema which it removes afterward.

The live matrix covers both stores, all nine ordered binding pairs, and
plaintext local, trusted TLS, unrelated CA, expired certificate and hostname-
mismatch profiles. All tests use public C++ SDK clients. Authorization scenarios
retain both the send stream and a separate subscription while receiving a
pending message, then resume the same task after a credential file is written.

## Caller permissions

Configure the dispatcher with the same authoritative task store as the executor:

```cpp
dispatcher.SetSecurityPolicy(policy, stores.task_store.get());
```

`SecurityPolicy::Authenticate` returns an identity or a rich SDK error;
`AuthorizeOperation` controls operation permissions, and `CanAccessTask` evaluates
an authoritative task. The SDK clears and reauthenticates `RequestContext::principal`
on each call. Applications own token validation, identity lifetime, ownership
records, scope rules and permission changes. Policy methods can run concurrently;
synchronize mutable data. Client-supplied context IDs are not authenticated identities.

Existing tasks are checked before reads, cancellation, continuation, subscription
and every push configuration operation. Unauthorized tasks use the same
`TaskNotFoundError` as absent tasks. Missing/invalid credentials produce HTTP 401
or gRPC `UNAUTHENTICATED`; an operation denial produces HTTP 403 or gRPC
`PERMISSION_DENIED`. Public Agent Card discovery remains public; extended cards
use authenticated dispatch. Unary send responses pass ownership checks before
release: returned tasks are checked directly, associated messages against the
store, and taskless messages remain supported. Denials invoke `AfterDispatch`
in reverse interceptor order so audit/metrics hooks receive the final error.

Listing evaluates permissions before counting and pagination. `ScopeTaskList`
can narrow store filters/projections, or return `nullopt` for an empty scope.
Retain every field needed by `CanAccessTask`; the default preserves full tasks.
The SUT pushes the caller's context filter into both stores and uses summaries,
loading histories/artifacts only for selected results. Exact visible counts
still inspect every matching task; repeated backend filtering/counting can cost
more than one linear scan. The dispatcher retains one bounded batch and one
page of IDs; PostgreSQL may still decode stored protobuf blobs before projection.
Numeric offset tokens and ordering remain unchanged. Cursor pagination,
last-update ordering and stronger snapshot consistency are tracked in
[ListTasks follow-up #309](https://github.com/MisterVVP/a2a-cpp/issues/309).

Streams retain the authenticated identity and check current task permissions
before reading and releasing events. Task-bearing events without IDs are
rejected; taskless messages remain supported. Timed reads, live semantics and
cancellation delegate to the underlying session. Keep the policy/store alive
until streams finish. Credentials are authenticated when connecting; applications
can include revocation/expiry in `CanAccessTask` for subsequent events. New
SUT streams use SDK-generated IDs so a reused message ID cannot select another
caller's task.

`SdkSecurityTest` covers operations, visibility, revocation, malformed events and
filtered pagination. `SecurityTransportIntegrationTest` independently checks
adapter error mapping and scoped results. Requirement mapping:
`AUTH-SERVER-002`, `AUTH-SCOPE-001..003`.

## Task authorization

`TaskAuthorizationService` combines storage, lifecycle transitions, history and
subscriptions for additional authorization during a task:

```cpp
a2a::server::TaskAuthorizationService authorization(
    stores.task_store.get(), validate_credential);
auto pending = authorization.RequestAuthorization(request, context, explanation);
auto resumed = authorization.Authorize(task_id, out_of_band_credential);
auto completed = authorization.Complete(task_id);
```

`RequestAuthorization` requires an agent explanation with a message ID and a
part. It persists `TASK_STATE_AUTH_REQUIRED` and `TaskStatus::message`.
`ReceiveMessage` validates task/context association and deduplicates history by
message ID while leaving authorization pending; message text never grants
authorization. Invalid credentials leave the task unchanged. `Authorize` calls
the application validator and moves the task to `WORKING`; `Complete` rejects a
pending task. Cancellation remains available while pending.

Send streams and subscriptions remain live during `auth_required` and receive
working/completed events after authorization. Route mutations/subscriptions
through this service and keep its store alive until streams finish. Its mutex
serializes mutations. `SendStreamingMessage` registers the stream before
out-of-band continuation can run; separate creation/subscription calls can miss
completion between them.

Mutations require conditional writes, supported by both stores. Transitions
clear the previous explanation and commit the complete task in one revision-
checked write. Only successful writes publish events. Failures preserve prior
state; conflicts report an error rather than overwrite concurrent cancellation
or history. Validation is not retried automatically; validators should be safe
to call again. All writers must respect revisions: unconditional writes can
replace a committed state afterward.

`TaskAuthorizationServiceTest` independently covers persistence, explanations,
credentials, pending messages, concurrent writes, active streams and continuation.
Requirement mapping: `AUTH-INTASK-001..006`.

## TLS and server identity

HTTP bindings accept `CallOptions::tls`, the shared HTTP client accepts
`Request::tls`, and discovery accepts TLS options through `MakeDefaultHttpFetcher`:

```cpp
a2a::client::CallOptions options;
options.tls = a2a::http::TlsOptions{.ca_bundle_path = ca_bundle_path,
                                     .require_tls13 = true};
auto task = client.GetTask(request, options);
```

Options propagate to unary/streaming requests. libcurl verifies certificates and
hostnames. An absent configuration or empty CA path uses compiled default trust,
including after pooled-handle reuse with a custom CA. Ordinary defaults require
TLS 1.2 or later; an explicit TLS configuration defaults to TLS 1.3. Set
`require_tls13=false` for TLS 1.2 deployments. HTTP remains available for local
fixtures; applications must enforce HTTPS ingress in production.

gRPC uses `grpc::SslCredentials` and `grpc::SslServerCredentials`. Supply trusted
PEM roots when constructing channels and certificate/key pairs on the server
builder. The SDK uses that channel; avoid production target-name overrides.
HTTP server adapters operate on an application-owned byte transport. The tested
deployment uses nginx TLS 1.3 termination, loopback upstreams and SSE buffering
disabled. Protect the proxy-to-adapter path with same-host loopback or an
independently secured connection/network.

The [launcher](../../tests/scripts/run_security_scenarios.py) generates isolated
CAs and localhost certificates with fixed validity windows. Public clients
accept trusted certificates and reject unrelated CAs, expired certificates and
hostname mismatches on every binding. It also runs a standalone gRPC TLS adapter
test. Tests never disable verification or depend on external services.
Requirement mapping: `AUTH-TLS-001/002`, `AUTH-SERVER-001`, `GRPC-SVC-003`.

## Signed Agent Cards

Enable `A2A_ENABLE_SIGNING=ON` and link `a2a::signing` for OpenSSL 3.0+ Ed25519
signing. Core/client/server acquire no new direct dependency. With vcpkg select
`VCPKG_MANIFEST_FEATURES=signing`. Installed packages expose
`A2A_CPP_HAS_AGENT_CARD_SIGNING` and the signing target when enabled.
Canonicalization remains in `a2a::core` via `a2a/core/json_canonicalization.h`.

```cpp
auto signed_card = a2a::core::SignAgentCard(card, key_id, private_key_pem);
auto verified = a2a::core::VerifyAgentCardJson(raw_json, key_resolver, now);
discovery.SetAgentCardVerifier([&](std::string_view json) {
  return a2a::core::VerifyAgentCardJson(json, key_resolver, current_time());
});
```

The protected JWS header contains `alg: EdDSA`, nonempty `kid` and `typ: JOSE`;
the payload excludes the entire `signatures` field and uses RFC 8785 JCS.
`CanonicalizeJson` validates strict JSON, rejects duplicate keys, invalid Unicode
and nonfinite numbers, sorts members by UTF-16 code units and formats IEEE-754
numbers using ECMAScript thresholds. Arrays retain order. Limits are 1 MiB and
128 nesting levels. Existing ProtoJSON helpers and generic protobuf `Value`
retain unknown fields; no additional JSON library is required.

Verification uses original discovery JSON including unknown fields. At least one
signature must verify with an application-trusted EdDSA key; unknown, invalid,
expired or revoked signatures do not defeat another trusted signature. The
resolver supplies a public key, validity interval and revocation status; callers
supply time. Noncritical headers (`typ`, `jku`, etc.) and unprotected annotations
are accepted, but overlapping protected/unprotected names are rejected. This
profile rejects `crit` and unencoded payloads and does not yet support ES256 or
RS256. `jku` never triggers a network fetch. Applications provision trusted keys.

Signing uses protobuf field-presence serialization; verification preserves all
received members except `signatures`. Cross-SDK empty/default-field treatment
remains an [upstream clarification (A2A #2122)](https://github.com/a2aproject/A2A/issues/2122).
Discovery verifies before parsing/caching and again on every cache hit. Failed
cached verification evicts the entry and fetches a replacement under the current
verifier; refresh failure never returns the invalid card. Extended discovery
uses the same verifier. Without a configured verifier, unsigned-card behavior
remains unchanged. Applications decide whether signatures are required.

`JsonCanonicalizationTest` uses the Python SDK's independent RFC 8785 corpus.
`AgentCardSigningTest` uses the Python/Go shared Ed25519 card and independently
signed header cases, with provenance retained, plus tampering, key validity and
cached policy-change tests. Requirement mapping: `CARD-SIGN-001..004`.

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

Tokens must be nonempty and distinct. New messages use `contextId=alice` or
`contextId=bob` with the matching identity. A `request-authorization` text part
creates a pending task. The background fixture reader authorizes/completes it
when a file named after its server-generated ID contains exactly the configured
credential. Keep files/directories owner-readable only. These are deterministic
application fixtures, not a production identity system or an A2A credential endpoint.

Configure `A2A_TCK_TLS_CERT_FILE` and `A2A_TCK_TLS_KEY_FILE` together for native
gRPC TLS, and `A2A_TCK_PUBLIC_HTTPS_BASE` for externally terminated HTTP card URLs.
The secure HTTP listener remains loopback. Configure `A2A_TCK_SIGNING_KEY_FILE`
and `A2A_TCK_SIGNING_KEY_ID` to sign public/extended cards; the extended card is
re-signed after modification.

TCK CI runs SDK security CTest evidence separately from the unmodified upstream
mandatory runner. No local Python TCK assertions or report gates are added.
At upstream revision `263b9cfaf16a554bdfb166a7ba5b67716e946349`, security groups
are registered without executable assertions and remain **NOT TESTED** in
upstream reports. The SDK evidence does not raise that percentage. Upstream
integration is tracked by [a2a-cpp #305](https://github.com/MisterVVP/a2a-cpp/issues/305),
[a2a-tck task 27](https://github.com/a2aproject/a2a-tck/blob/main/backlog/tasks/task-27%20-%20Add-auth-security-conformance-tests-AUTH.md),
[a2a-tck task 29](https://github.com/a2aproject/a2a-tck/blob/main/backlog/tasks/task-29%20-%20Add-agent-card-signing-conformance-tests-CARD-SIGN.md)
and the `GRPC-SVC-003` coverage backlog.
