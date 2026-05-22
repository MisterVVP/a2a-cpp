# JSON-RPC Server Transport

`JsonRpcServerTransport` maps JSON-RPC 2.0 method calls to dispatcher operations.

## Use cases

- Interop with systems already standardized on JSON-RPC.
- Environments where method-based routing is preferred to REST path routing.

## Operational guidance

- Validate `jsonrpc`, `id`, and `method` fields consistently.
- Return spec-consistent error objects for invalid requests and method failures.
- Preserve request metadata for auth/audit needs.

## Authentication behavior

Authentication metadata from inbound headers can be extracted and made available to server execution context.

## Related chapters

- [Server Overview](overview.md)
- [Authentication Overview](../auth/overview.md)
