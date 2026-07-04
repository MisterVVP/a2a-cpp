# JSON-RPC Server Transport

`JsonRpcServerTransport` maps JSON-RPC 2.0 method calls to dispatcher operations.

## Responsibilities

- Validate the `jsonrpc`, `id`, `method`, and `params` envelope fields.
- Route supported A2A methods to the dispatcher.
- Return JSON-RPC error objects for invalid requests, unknown methods, and executor failures.
- Preserve request metadata for auth and audit policy.

## Use cases

Use JSON-RPC when method-based routing is easier to integrate than resource-oriented REST paths or when another system already standardizes on JSON-RPC 2.0.

## Example

See `examples/apps/json_rpc_server/main.cpp`.
