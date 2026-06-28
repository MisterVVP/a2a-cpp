# Client usage

Use `a2a::client::A2AClient` with either `HttpJsonTransport` (REST) or `JsonRpcTransport`.

Typical flow:

1. Discover an `AgentCard` with `DiscoveryClient`.
2. Resolve an endpoint with `AgentCardResolver`.
3. Build a transport and pass it to `A2AClient`.
4. Call `SendMessage`, `GetTask`, `CancelTask`, or streaming APIs.

For server-side task history updates, SDK-managed executors now route history appends through
`TaskStore::AppendTaskHistory(task_id, message, policy)`, which centralizes chronological ordering
and deduplication policy handling (`NoDedup`, `DedupByMessageId`, `DedupByIdOrFingerprint`).


## Default outbound HTTP

When libcurl support is enabled at configure time, the SDK provides a shared libcurl-backed outbound HTTP implementation for buffered REST, JSON-RPC, and Agent Card discovery calls. Use `a2a::client::MakeDefaultHttpRequester()`, `a2a::client::MakeDefaultHttpFetcher()`, `HttpJsonTransport::CreateDefault(...)`, `JsonRpcTransport::CreateDefault(...)`, or `DiscoveryClient::CreateDefault(...)` when you do not need to provide custom HTTP plumbing.

The callback-based constructors remain available for tests, embedded runtimes, custom TLS/mTLS policy, observability, retry logic, and other environments that need to control outbound HTTP execution. The default requester captures response headers, including `A2A-Version`, so client-side protocol-version validation continues to apply.

SSE streaming still uses the existing `HttpStreamRequester` path. Provide a stream requester when using streaming APIs until shared libcurl streaming support is added. libcurl is the default outbound HTTP dependency for non-streaming SDK client calls, but `-DA2A_ENABLE_LIBCURL=OFF` builds the SDK with limited features: injected requesters/fetchers continue to work, while default buffered outbound HTTP returns a clear runtime error.

See runnable examples:

- `examples/apps/simple_client/main.cpp`
- `examples/apps/rest_server/main.cpp`
- `examples/apps/json_rpc_server/main.cpp`
- `examples/apps/streaming_client/main.cpp`

See `docs/sdk-idempotency.md` for the normative duplicate/retry behavior and telemetry model.
