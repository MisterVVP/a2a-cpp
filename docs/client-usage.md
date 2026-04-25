# Client usage

Use `a2a::client::A2AClient` with either `HttpJsonTransport` (REST) or `JsonRpcTransport`.

Typical flow:

1. Discover an `AgentCard` with `DiscoveryClient`.
2. Resolve an endpoint with `AgentCardResolver`.
3. Build a transport and pass it to `A2AClient`.
4. Call `SendMessage`, `GetTask`, `CancelTask`, or streaming APIs.

See runnable examples:

- `examples/discovery_only_client.cpp`
- `examples/rest_client.cpp`
- `examples/json_rpc_client.cpp`
- `examples/streaming_client.cpp`
