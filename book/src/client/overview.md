# Client Overview

Use `a2a::client::A2AClient` with a concrete `ClientTransport` to call A2A servers.

## Client capabilities

- `SendMessage`
- `GetTask`
- `ListTasks`
- `CancelTask`
- `SendStreamingMessage`
- `SubscribeTask`
- `CreateTaskPushNotificationConfig`
- `GetTaskPushNotificationConfig`
- `ListTaskPushNotificationConfigs`
- `DeleteTaskPushNotificationConfig`
- Client interceptors via `ClientInterceptor`
- Per-call settings through `CallOptions`

## Typical flow

1. Fetch an Agent Card with `DiscoveryClient`.
2. Select an endpoint with `AgentCardResolver::SelectPreferredInterface`.
3. Construct `HttpJsonTransport`, `JsonRpcTransport`, or `GrpcTransport`.
4. Create `A2AClient` with the transport.
5. Invoke task, streaming, or push-notification APIs.
6. Call `Destroy()` during shutdown when you need transport cleanup.

## Default outbound HTTP

When libcurl is available and `A2A_ENABLE_LIBCURL=ON`, default constructors/factories can perform buffered outbound REST, JSON-RPC, and discovery calls. For tests, custom TLS policy, mTLS, retries, or embedded runtimes, inject your own requester/fetcher callbacks.

## Runnable examples

- `examples/apps/hello_agent/main.cpp`
- `examples/apps/simple_client/main.cpp`
- `examples/apps/streaming_client/main.cpp`
- `examples/apps/push_notifications/main.cpp`
