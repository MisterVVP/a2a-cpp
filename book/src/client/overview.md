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

- `examples/apps/simple_client/main.cpp`
- `examples/apps/rest_server/main.cpp`
- `examples/apps/json_rpc_server/main.cpp`
- `examples/apps/streaming_client/main.cpp`

## Related chapters

- [Sending Messages](sending-messages.md)
- [Discovery](discovery.md)
- [Get Task](get-task.md)
- [Cancel Task](cancel-task.md)
