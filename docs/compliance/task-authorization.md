# Additional authorization during a task

`TaskAuthorizationService` combines SDK task storage, lifecycle transitions,
history retention and subscriptions. An executor can request authorization with
an agent status message, accept messages while authorization is pending, and
resume the same task after an application validates an out-of-band credential.

```cpp
a2a::server::TaskAuthorizationService authorization(
    stores.task_store.get(), validate_credential);
auto pending = authorization.RequestAuthorization(request, context, explanation);
auto resumed = authorization.Authorize(task_id, out_of_band_credential);
auto completed = authorization.Complete(task_id);
```

`RequestAuthorization` requires an agent explanation with a message ID and at
least one part. It persists `TASK_STATE_AUTH_REQUIRED` and the explanatory
`TaskStatus::message`. `ReceiveMessage` validates the task/context association,
appends history with SDK message-ID deduplication, and leaves authorization
pending. Receiving a message never grants authorization, even if its text happens to
contain the fixture credential. Invalid credentials
leave the task unchanged. `Authorize` invokes the application validator and moves
the task to `WORKING`; `Complete` rejects an authorization-pending task.
Cancellation remains available while pending.

The service owns a subscription service and publishes state changes. Active send
streams and subscriptions remain live during `auth_required`, then receive the
working and completed events after authorization. Applications should route all
mutations and subscriptions for these tasks through this service and keep the
store alive until streams finish. Its mutex serializes its own mutations and
registers send streams before
out-of-band continuation can run. Use `SendStreamingMessage` to combine creation
or pending-message receipt with stream registration; calling the separate
creation and subscription methods can miss completion between those calls.

Authorization mutations require the store's conditional-write APIs (supported
by the in-memory and PostgreSQL backends). Each state transition clears the previous
status explanation and commits the complete task in one revision-checked write.
Only successful writes publish events. A write failure leaves the prior state
available for retry; a revision conflict reports an error instead of overwriting
another writer's cancellation or history update. This service does not retry
credential validation automatically, so validators should be safe to call again.
All task writers must respect revisions; unconditional writes can still replace
a successfully committed state afterward.

The SUT's security executor uses this public SDK service. A background fixture
reader obtains credentials from a local directory configured with
`A2A_TCK_CREDENTIAL_DIRECTORY`. The file name is the server-generated task ID;
its content must exactly match `A2A_TCK_TASK_CREDENTIAL`. This local fixture is
an application credential source, not an A2A authentication endpoint. Files and
the directory are local test fixtures and should remain readable only by their
owner. The executor's trigger is a text part containing `request-authorization`.

`TaskAuthorizationServiceTest` verifies state persistence, explanatory messages,
invalid credentials, incoming pending messages, active streams and continuation
independently of the SUT. The [live scenarios](security-scenarios.md) repeat the
flow for every ordered binding pair with both storage backends, including two
simultaneously active streams.

Requirement mapping: `AUTH-INTASK-001..006`. Upstream reports remain separate
from these SDK tests.
