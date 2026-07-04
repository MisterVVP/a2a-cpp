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
