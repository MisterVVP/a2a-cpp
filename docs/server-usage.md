# Server usage

Implement `a2a::server::AgentExecutor` and route requests through a transport:

- `RestServerTransport` for HTTP+JSON REST paths.
- `JsonRpcServerTransport` for JSON-RPC 2.0 method dispatch.

Minimal in-process setup:

1. Implement custom executor logic.
2. Create `a2a::server::Dispatcher` with the executor.
3. Create a transport and forward inbound HTTP requests to `Handle(...)`.

See `examples/minimal_server_custom_executor.cpp` for a minimal setup.

## Push notifications

Use `a2a::server::PushNotificationService` when an executor supports task push-notification configs. Wire it with the same `TaskStore` that owns task state, a `PushNotificationStore` for webhook configs, and a `PushNotificationDeliveryClient` for outbound delivery.

Recommended executor flow:

1. Persist the task state with `TaskLifecycleService` or your `TaskStore`.
2. Call `RegisterInlineConfigIfPresent(request, resolved_task_id)` for `SendMessageRequest` values that include an inline push config.
3. Call `NotifyTaskUpdated(task)` after each task status update and propagate the returned `Result<void>` to the caller. The service now fails the request path when delivery returns an error, reports an error message in `PushDeliveryResult`, or reports a non-2xx HTTP status.
4. Forward create/get/list/delete push-config RPCs to `CreateConfig`, `GetConfig`, `ListConfigs`, and `DeleteConfig`.
5. Advertise push support in the Agent Card only after the store and delivery client are configured.

The built-in `HttpPushNotificationDeliveryClient` is synchronous and intended for local examples, tests, and simple deployments. Production servers should usually inject a durable queued sender with retries, backoff, webhook URL validation, credential protection, SSRF controls, and delivery telemetry. See `examples/push_notifications.cpp` for a focused service-level example and `examples/example_support.h` for executor wiring.

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
