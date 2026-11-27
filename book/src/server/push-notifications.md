# Push Notifications

The SDK includes task push-notification configuration APIs and server-side delivery abstractions.

## Client API surface

`A2AClient` exposes:

- `CreateTaskPushNotificationConfig`
- `GetTaskPushNotificationConfig`
- `ListTaskPushNotificationConfigs`
- `DeleteTaskPushNotificationConfig`

Use these only when the target Agent Card and server policy indicate push-notification support.

## Server components

- `PushNotificationService` coordinates stored configs and delivery.
- `PushNotificationStore` persists webhook configs.
- `PushNotificationDeliveryClient` abstracts outbound delivery.
- `HttpPushNotificationDeliveryClient` provides a simple libcurl-backed synchronous delivery path when libcurl support is enabled.

## Recommended executor flow

1. Persist task state in your `TaskStore` or `TaskLifecycleService`.
2. Register inline configs from `SendMessageRequest` when present.
3. Call `NotifyTaskUpdated(task)` after status changes.
4. Propagate delivery failures instead of silently dropping them.
5. Override push-config CRUD executor methods only when push support is fully configured.

## Production guidance

For production, prefer a durable queued delivery client with retries, backoff, webhook URL validation, SSRF controls, credential protection, and delivery telemetry. Keep TLS policy at least as strong as the built-in delivery client, which requires TLS 1.2 or newer for HTTPS URLs.

## Example

See `examples/apps/push_notifications/main.cpp`.


Use `a2a::server::PushNotificationService` when an executor supports task push-notification configs. Wire it with the same `TaskStore` that owns task state, a `PushNotificationStore` for webhook configs, and a `PushNotificationDeliveryClient` for outbound delivery.

Recommended executor flow:

1. Persist the task state with `TaskLifecycleService` or your `TaskStore`.
2. Call `RegisterInlineConfigIfPresent(request, resolved_task_id)` for `SendMessageRequest` values that include an inline push config.
3. Call `NotifyTaskUpdated(task)` after each task status update and propagate the returned `Result<void>` to the caller. The service now fails the request path when delivery returns an error, reports an error message in `PushDeliveryResult`, or reports a non-2xx HTTP status.
4. Forward create/get/list/delete push-config RPCs to `CreateConfig`, `GetConfig`, `ListConfigs`, and `DeleteConfig`.
5. Advertise push support in the Agent Card only after the store and delivery client are configured.

The built-in `HttpPushNotificationDeliveryClient` is synchronous and intended for local examples, tests, and simple deployments. Production servers should usually inject a durable queued sender with retries, backoff, webhook URL validation, credential protection, SSRF controls, and delivery telemetry. See `examples/apps/push_notifications/main.cpp` for a focused service-level example and `tests/support/example_support/example_support.h` for executor wiring.

### Built-in HTTP delivery requirements

When libcurl support is enabled, `HttpPushNotificationDeliveryClient` uses the SDK shared libcurl-backed outbound HTTP client for webhook delivery. SDK users that enable the built-in client must provide a libcurl build with HTTPS support and a platform trust store (or equivalent libcurl TLS backend configuration) so production `https://` webhook URLs can be verified.

For security and interoperability, the built-in client:

- accepts `http://` and `https://` webhook URLs, with TLS used for `https://` URLs;
- sets libcurl's minimum TLS version to TLS 1.2, allowing TLS 1.2 or newer only;
- supports HTTP/1.1, HTTP/2.0, and HTTP/3.0 request configuration; and
- rejects HTTP/1.0 configuration instead of downgrading to it.

The default push-delivery preference is HTTP/2.0 with HTTP/1.1 fallback. Keep custom delivery clients at the same or stronger TLS baseline.

The shared outbound HTTP implementation lives outside the server namespace and is reused by client REST, JSON-RPC, discovery, and push-notification delivery paths. Custom `PushNotificationDeliveryClient` implementations can still be injected for queues, retries, policy enforcement, or alternate HTTP stacks.

Builds configured with `-DA2A_ENABLE_LIBCURL=OFF` keep server transports and custom delivery clients available, but the built-in `HttpPushNotificationDeliveryClient` cannot perform outbound webhook calls and reports that libcurl support is disabled.

## Asynchronous HTTP delivery

Include `a2a/server/async_push_notification_delivery.h` and link `a2a::server`.
`HttpAsyncPushNotificationDeliveryClient` is opt-in; the existing `Deliver`
methods and synchronous executors keep their behavior. Native async delivery
requires `A2A_ENABLE_LIBCURL=ON`. It uses the same libcurl multi reactors,
connection pool and completion runtime as async HTTP unary clients.

```cpp
using namespace a2a::server;
HttpAsyncPushNotificationDeliveryClient delivery;
AsyncPushDeliveryOptions options;
options.timeout = std::chrono::seconds(2);
options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
auto started = delivery.DeliverAsync(request, ProcessDeliveryResult, options);
if (started.ok()) {
  auto handle = started.value();
  handle.Wait();
}
delivery.Shutdown();
```

`ProcessDeliveryResult` is an application callback. `request` is a `PushDeliveryRequest`, with the same config, authentication and
StreamResponse payload as synchronous delivery. The SDK copies it before
returning. Protobuf JSON, POST headers, successful status and non-2xx/network
error mapping are shared with synchronous delivery. The configured HTTP version
fallback is attempted once after an error. Cancellation and deadline expiry
stop the delivery without fallback. As with synchronous fallback, an ambiguous
primary failure can produce a duplicate webhook POST; receivers should tolerate
that possibility. There is no retry scheduler or durable queue.

An absent timeout uses `HttpPushNotificationDeliveryOptions::timeout` (five
seconds by default). Zero and negative timeouts expire immediately. The earlier
of timeout and absolute steady-clock deadline applies to the **whole delivery**,
including both version attempts and time waiting for transport admission. A
fallback receives only the remaining time. Deadline errors carry
`DEADLINE_EXCEEDED`; cancellation and shutdown carry `CANCELLED`.

A rejected call returns an error and invokes no callback. An accepted call
returns an `AsyncHandle` and completes exactly once, including validation errors,
timeout, cancellation and shutdown. Cancellation races with successful completion;
the first claimed terminal result wins. Dropping the handle does not cancel.
Callbacks execute on the shared SDK completion workers, never inline on the
calling thread or on a libcurl reactor. Capture application state with sufficient
lifetime and keep callbacks short. One slow callback leaves other completion
workers and the network reactors available; blocking every worker delays callback
execution, including cancellation notifications.

No SDK thread is created for each delivery. The shared runtime bounds all
accepted async operations (including callbacks still running or queued) at 4096,
and each native HTTP client caps in-flight requests at 4096. Each push delivery
uses an outer operation and an underlying HTTP operation, so its effective limit
is lower and shares capacity with other SDK clients. Saturation returns a network
capacity error, either on admission or through an accepted delivery's callback.
The native client's existing response header/body bounds also apply.

`Shutdown()` closes admission, cancels accepted deliveries, drains their
callbacks and stops this client's native transfers; it is idempotent. Destruction
calls it automatically. It must run while the callback's captured application
state still exists. Inside an SDK callback, shutdown requests cancellation but
skips waiting to avoid deadlock; `Wait()` also skips waiting there. Handles remain
safe to observe after client destruction. Concurrent calls and shutdown are safe
while the client object remains alive.

The requester-injection constructor accepts `PushHttpAsyncRequester` for tests or
an existing non-blocking HTTP facility. It must follow
`http::Client::SendRequestAsync`'s accepted/rejected completion and cancellation
contract. Requests own their data. The wrapper owns its callbacks and cancellation
handles, while the injected requester owns transport resources and must arrange
its own final transport shutdown. An empty injected requester yields a validation
completion. Blocking requesters defeat asynchronous delivery.

## Coroutine executors

With `A2A_ENABLE_ASIO=ON`, link `a2a::asio` and include
`a2a/server/engine/async_push_notification_service.h`. After persisting task state
and registering any inline config, a `CoroutineAgentExecutor` can await:

```cpp
auto notified = co_await a2a::server::engine::AsyncPushNotificationService::NotifyTaskUpdated(
    push_store, delivery_client, task, context);
if (!notified.ok()) {
  co_return notified.error();
}
```

Here `delivery_client` is a `shared_ptr<AsyncPushNotificationDeliveryClient>` and
`context` is the executor's `RequestExecutionContext`. The helper retains the
client across suspension and resumes on the calling Asio executor. Context stop,
request deadline and Asio cancellation reach the outbound handle. The helper
uses the same status payload, ordered config snapshot and first-error behavior
as `PushNotificationService::NotifyTaskUpdated`, including validation of custom
clients' result status/error message. Config listing remains synchronous on the
calling worker; provide an appropriate store implementation. The task must
already exist, and the store must outlive the coroutine. Use the existing
`PushNotificationService` for config CRUD and inline registration.

`AsyncPushNotificationService::Deliver` bridges a single arbitrary
`PushDeliveryRequest` when config fanout is managed by the application. Both
helpers suspend for HTTP delivery without occupying an Asio worker; they require
no changes to the dispatcher or existing synchronous executors. Close server
admission and signal cancellation, drain coroutine work, then shut down the
shared delivery client before destroying application state.

## Resource evidence

`async_push_delivery_evidence` compares synchronous and asynchronous delivery
against the same controllable localhost webhook at 1, 16 and 64 simultaneous
requests. Each run holds all responses until every request arrives, samples
process threads while requests are pending, then releases them and reports total
elapsed microseconds and errors. The synchronous baseline uses one application
caller thread per in-flight request. The async workload uses the shared reactors
and completion workers. Run it in a fresh process for reproducible thread counts:

```bash
cmake --build build --target async_push_delivery_evidence
./build/tests/async_push_delivery_evidence
```

The evidence executable fails on delivery errors or growth beyond the shared
async thread bound. Timings describe this controlled workload, not production
webhook latency. `AsyncPushNetworkTest` covers cancellation, timeout, shutdown,
callback isolation and HTTP result parity; `AsyncPushServiceTest` checks suspension
and cancellation on a single Asio worker.
