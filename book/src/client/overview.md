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


When libcurl support is enabled at configure time, the SDK provides a shared libcurl-backed outbound HTTP implementation for buffered REST, JSON-RPC, and Agent Card discovery calls. Use `a2a::client::MakeDefaultHttpRequester()`, `a2a::client::MakeDefaultHttpFetcher()`, `HttpJsonTransport::CreateDefault(...)`, `JsonRpcTransport::CreateDefault(...)`, or `DiscoveryClient::CreateDefault(...)` when you do not need to provide custom HTTP plumbing.

The callback-based constructors remain available for tests, embedded runtimes, custom TLS/mTLS policy, observability, retry logic, and other environments that need to control outbound HTTP execution. The default requester captures response headers, including `A2A-Version`, so client-side protocol-version validation continues to apply.

SSE streaming uses the same default libcurl outbound HTTP infrastructure as buffered calls. `HttpJsonTransport::CreateDefault(...)` and `JsonRpcTransport::CreateDefault(...)` configure production SSE requesters for `SendStreamingMessage` and `SubscribeTask`; injected requesters are still available for tests or custom HTTP stacks. With `-DA2A_ENABLE_LIBCURL=OFF`, injected requesters/fetchers continue to work while default outbound HTTP and SSE calls return a clear runtime error. gRPC and Protobuf remain mandatory build dependencies. On Windows, run `./scripts/install_build_deps.sh` from Git Bash; the example runner then configures CMake through the vcpkg toolchain.


See [task history and retries](../server/idempotency.md) for duplicate handling and telemetry.
