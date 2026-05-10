# JSON-RPC Transport

JSON-RPC transport supports method-based request dispatch aligned with JSON-RPC 2.0.

## Client side

Use `JsonRpcTransport` with `A2AClient` for JSON-RPC integrations.

## Server side

Use `JsonRpcServerTransport` to decode method calls and route them to dispatcher operations.

## Operational considerations

- Validate protocol envelope fields (`jsonrpc`, `id`, `method`).
- Return consistent JSON-RPC error structures.
- Preserve metadata needed for auth and auditing.
