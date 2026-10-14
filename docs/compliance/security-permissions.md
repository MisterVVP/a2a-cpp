# Caller authentication and task permissions

Configure `Dispatcher::SetSecurityPolicy(policy, task_store)` to authenticate and
scope every dispatched operation, using the same authoritative task store as the
executor. `SecurityPolicy::Authenticate` returns an identity or a rich SDK error;
`AuthorizeOperation` controls operation permissions, and `CanAccessTask` evaluates
an authoritative task. The SDK sets `RequestContext::principal` from authentication
on each call and clears any prior value before authentication.

Applications own token validation, identity lifetime, scope rules and permission
changes. Policy methods may run concurrently; synchronize mutable identity and
permission data. Never treat client-supplied task metadata or context IDs as an identity.
The SUT's deterministic policy allows two configured identities to create tasks in
their respective fixture contexts. Its shared executor uses SDK-generated IDs
for new authenticated streams, so reusing a message ID cannot select another
caller's streaming task. Production applications should use their own
resource ownership model and persistent permission records.

```cpp
dispatcher.SetSecurityPolicy(policy, stores.task_store.get());
```

The SDK checks existing tasks before reads, cancellation, continuation,
subscription and all push configuration operations. Unauthorized tasks use the
same `TaskNotFoundError` as absent tasks. Missing or invalid credentials produce
HTTP 401 or gRPC `UNAUTHENTICATED`; an operation policy denial produces HTTP 403
or gRPC `PERMISSION_DENIED`. Public Agent Card discovery remains public; extended
card retrieval goes through authenticated dispatch.

Unary send responses also pass task ownership checks before they are released.
Returned tasks are checked directly; a returned message with a task ID is checked
against the authoritative store. Taskless messages remain supported. Authentication,
operation and task denials invoke `AfterDispatch` in reverse interceptor order,
so audit and metrics hooks receive the final error even when dispatch is blocked.

Task listing uses the configured authoritative store in bounded batches. It
evaluates permissions before counting or pagination, so totals and page tokens
reveal only permitted tasks. `SecurityPolicy::ScopeTaskList` may narrow the query
filters and projections before storage reads, or return `nullopt` for an empty
scope. A policy must retain every field its `CanAccessTask` check needs. The
default preserves full tasks for arbitrary application policies. The SUT's
context-based policy pushes the caller's context filter into both stores and
uses summaries; histories and artifacts are loaded only for selected results.

Exact visible counts still require inspecting every matching task. Stores may
repeat count/filter work for each batch, so backend costs can exceed a single
linear scan; caller filters reduce the matching set but do not eliminate this
limitation. The dispatcher retains at most one bounded batch and one page of task IDs;
stored protobuf blobs may still need decoding before projection in PostgreSQL.
This change preserves existing numeric offset tokens and ordering. Cursor-based
pagination, last-update ordering and stronger snapshot consistency remain a
separate [ListTasks conformance follow-up (#309)](https://github.com/MisterVVP/a2a-cpp/issues/309);
no full conformance claim is made here.

Streams retain the authenticated identity and check current task permissions
before reading and before releasing every event. Timed reads, live-stream
semantics and cancellation delegate to the underlying SDK session. Applications
must keep the policy and store alive until outstanding stream sessions finish.
A stream credential is authenticated at connection establishment; applications
can include identity revocation/expiry in `CanAccessTask` for each event.

`SdkSecurityTest` covers every operation's authentication, task visibility,
operation denials, permission revocation and filtered list pagination.
`SecurityTransportIntegrationTest` constructs SDK HTTP+JSON, JSON-RPC and gRPC
adapters without the SUT to verify transport error mapping and scoped results.
The [live SUT scenarios](security-scenarios.md) repeat caller isolation with both
storage backends and all nine ordered binding pairs.

Requirement mapping: `AUTH-SERVER-002`, `AUTH-SCOPE-001..003`. Server certificate
verification (`AUTH-SERVER-001`) is covered by [TLS deployment tests](security-tls.md).
