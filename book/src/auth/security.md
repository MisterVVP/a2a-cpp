# Security integration

Configure authentication, permissions, task authorization and TLS through public
SDK APIs. Applications supply identity validation, credential lifetime, trusted
keys and authorization policy; test fixtures are not a production identity system.

Signing is optional and disabled by default. Permissions and task authorization
also work without it. Ordinary builds need neither nginx nor the OpenSSL CLI.

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
needed by `CanAccessTask`; defaults preserve full tasks. The dispatcher retains
one bounded batch and one page of IDs. Exact counts still inspect all matching
tasks and may decode protobuf blobs before projection; bounded internal scans
skip redundant backend counts. In v0.6.0 development, listings use versioned
cursors and descending update-time ordering. See the
[ListTasks contract](../server/list-tasks-pagination.md) for live traversal
consistency, legacy-token compatibility and migration requirements.

Streams check current task permissions before reading and releasing events,
reject task-bearing events without IDs, and allow taskless messages. On a stream
associated with a task, even taskless messages recheck that task after reading,
so revocation during a blocking read is enforced. Streams preserve timed reads,
live semantics and cancellation. Keep policies/stores alive
until sessions finish. Credentials are authenticated on connection; include
revocation/expiry in `CanAccessTask` for subsequent events.

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
target-name overrides. HTTP adapters use application-owned byte transports; a reverse
proxy can terminate TLS and forward to loopback upstreams with SSE buffering disabled.
Protect proxy-to-adapter traffic with same-host loopback or another secured connection.

## Signed Agent Cards

Enable `A2A_ENABLE_SIGNING=ON` and link `a2a::signing` for OpenSSL 3.0+
Ed25519 and [simdjson](https://github.com/simdjson/simdjson) 3.0+ parsing
(Apache-2.0/MIT). Both are optional signing dependencies. With vcpkg select
`VCPKG_MANIFEST_FEATURES=signing`; installed packages expose
`A2A_CPP_HAS_AGENT_CARD_SIGNING` and the enabled target. A pinned vcpkg registry
selects maintained OpenSSL and CMake helpers, with simdjson pinned to 3.12.3;
other packages retain the existing baseline. Canonicalization is
private to signing; no general JSON parser APIs or other SDK parsing paths change.

```cpp
auto signed_card = a2a::core::SignAgentCard(card, key_id, private_key_pem);
auto verified = a2a::core::VerifyAgentCardJson(raw_json, key_resolver, now);
discovery.SetAgentCardVerifier([&](std::string_view json) {
  return a2a::core::VerifyAgentCardJson(json, key_resolver, current_time());
});
```

Protected JWS headers contain `alg: EdDSA`, nonempty `kid`, and `typ: JOSE`.
RFC 8785 JCS canonicalizes the payload after removing `signatures`.
The private adapter rejects duplicate decoded keys, invalid Unicode and nonfinite
numbers, sorts members by UTF-16 code units, uses ECMAScript number formatting
and retains array order. Limits are 1 MiB and 128 nesting levels. Simdjson's
On-Demand API parses all values as binary64 where JCS requires it, including
integer tokens beyond 64 bits. Simdjson is not a JCS implementation: the adapter
supplies these serialization rules and validates excluded signatures too.

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
