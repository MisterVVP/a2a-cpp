# JSON-RPC Transport for A2A Clients and Servers

JSON-RPC transport supports method-based request dispatch aligned with JSON-RPC 2.0.

If you are new to the SDK runtime flow, begin with the [Quickstart](../getting-started/quickstart.md).

## Client side

Use `JsonRpcTransport` with `A2AClient` for JSON-RPC integrations.

## Server side

Use `JsonRpcServerTransport` to decode method calls and route them to dispatcher operations.

## Operational considerations

- Validate protocol envelope fields (`jsonrpc`, `id`, `method`).
- Return consistent JSON-RPC error structures.
- Preserve metadata needed for auth and auditing.

## See also

- [Send Messages with A2AClient](../client/sending-messages.md)
- [Custom Executor Design and Implementation](../server/custom-executor.md)
- [REST Transport](rest.md)
- [Authentication Overview](../auth/overview.md)
