# Client Overview

Use `a2a::client::A2AClient` to interact with A2A servers over multiple transports.

## Typical client flow

1. Discover an `AgentCard` with `DiscoveryClient`.
2. Resolve endpoint details with `AgentCardResolver`.
3. Construct a transport (`HttpJsonTransport` for REST, or `JsonRpcTransport` for JSON-RPC).
4. Create `A2AClient` with that transport.
5. Invoke operations such as:
   - `SendMessage`
   - `GetTask`
   - `CancelTask`
   - `SendStreamingMessage`
   - `SubscribeTask`

## Runnable examples

- `examples/discovery_only_client.cpp`
- `examples/rest_client.cpp`
- `examples/json_rpc_client.cpp`
- `examples/streaming_client.cpp`

## Related chapters

- [Sending Messages](sending-messages.md)
- [Discovery](discovery.md)
- [Get Task](get-task.md)
- [Cancel Task](cancel-task.md)
