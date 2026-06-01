# Examples

This folder provides runnable, in-process examples for the A2A C++ SDK.

## Example index

| Example | Target | Demonstrates | Run command | Expected output snippet |
|---|---|---|---|---|
| discovery_only_client.cpp | `example_discovery_only_client` | Agent-card discovery | `./build/examples/example_discovery_only_client` | `discovered agent:` |
| rest_client.cpp | `example_rest_client` | REST `SendMessage` | `./build/examples/example_rest_client` | `created task:` |
| json_rpc_client.cpp | `example_json_rpc_client` | JSON-RPC `SendMessage` | `./build/examples/example_json_rpc_client` | `json-rpc created task:` |
| streaming_client.cpp | `example_streaming_client` | SSE `SendStreamingMessage` | `./build/examples/example_streaming_client` | `status event:` |
| grpc_client.cpp | `example_grpc_client` | gRPC `GetTask` | `./build/examples/example_grpc_client` | `Task id:` |
| list_tasks_client.cpp | `example_list_tasks_client` | REST `ListTasks` | `./build/examples/example_list_tasks_client` | `listed tasks:` |
| cancel_task_client.cpp | `example_cancel_task_client` | REST `CancelTask` | `./build/examples/example_cancel_task_client` | `task state after cancel:` |
| push_notification_config_client.cpp | `example_push_notification_config_client` | Push-config CRUD/list APIs (transport support still evolving) | `./build/examples/example_push_notification_config_client` | may return route/validation error on REST server |
| push_notifications.cpp | `example_push_notifications` | End-to-end `PushNotificationService` config registration and delivery with a recording delivery client | `./build/examples/example_push_notifications` | `delivered push notifications:` |
| interceptor_client.cpp | `example_interceptor_client` | Client interceptor before/after hooks | `./build/examples/example_interceptor_client` | `before GetTask` |
| auth_policy_server.cpp | `example_auth_policy_server` | Server auth metadata extraction + policy point | `./build/examples/example_auth_policy_server` | `missing auth status:` |
| minimal_server_custom_executor.cpp | `example_minimal_server_custom_executor` | Minimal server setup | `./build/examples/example_minimal_server_custom_executor` | `agent-card status:` |

## Build all examples

```bash
cmake -S . -B build -DA2A_BUILD_EXAMPLES=ON
cmake --build build --parallel
```

## Run all examples sequentially

```bash
./scripts/run_examples.sh
```

## Windows (Git Bash) note

- Use `./scripts/windows_build_local.sh` from Git Bash for local Windows builds.
- When working on a PR branch, set `UPDATE_REPO=0` to prevent branch update logic from running:

```bash
UPDATE_REPO=0 ./scripts/windows_build_local.sh
```

- If `UPDATE_REPO=1`, the script now requires the current branch to already be `main` and fails fast otherwise.

See also: `docs/quickstart.md` for first-run setup and `docs/client-usage.md`/`docs/server-usage.md` for API details.

## Push notifications

`push_notifications.cpp` is the focused push-notification walkthrough: it stores a task, registers a webhook config, calls `NotifyTaskUpdated`, and verifies the delivery client receives one `StreamResponse` status-update payload. Examples that use `ExampleExecutor` enable SDK-backed push notifications in their Agent Card and route CRUD calls through `PushNotificationService`.

To enable push in an SDK server:

1. Store tasks in a `TaskStore` implementation.
2. Create a `PushNotificationStore` such as `InMemoryPushNotificationStore`, or inject your own durable implementation.
3. Create a `PushNotificationDeliveryClient` such as `HttpPushNotificationDeliveryClient`, or inject your own queued/retrying sender.
4. Construct `PushNotificationService` with those dependencies and forward push CRUD methods from your executor.
5. After creating or updating a task, call `NotifyTaskUpdated(task)` and return its `Result<void>` to the request path; delivery failures are surfaced to the caller instead of being ignored.
6. Advertise support with `AgentCardBuilder::WithPushNotifications(true)` only after the store and delivery client are wired.

`HttpPushNotificationDeliveryClient` sends webhook payloads as JSON `StreamResponse` HTTP POST requests with `Content-Type: application/json`. `PushNotificationService::NotifyTaskUpdated` treats a delivery-client error, an error message in `PushDeliveryResult`, or a non-2xx delivery status as a failed notification and returns that failure to the caller. When `AuthenticationInfo.scheme` is set, it adds `Authorization: <scheme> <credentials>`; for example, scheme `Bearer` and credentials `abc` produce `Authorization: Bearer abc`.

The in-memory store and synchronous HTTP delivery client are intended for local development, tests, examples, and single-process deployments. Production deployments should provide durable config storage, retry queues, backoff, observability, and outbound webhook security controls. Validate webhook URLs, prefer HTTPS, protect credentials, consider host allowlists to reduce SSRF risk, treat task IDs as opaque identifiers, and validate received task IDs on clients.
