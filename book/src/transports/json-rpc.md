# JSON-RPC Transport

JSON-RPC transport supports A2A method dispatch over JSON-RPC 2.0 envelopes.

## Client side

Use `JsonRpcTransport` with `A2AClient`. Default buffered outbound HTTP is available when libcurl is enabled and found; otherwise inject a requester.

## Server side

Use `JsonRpcServerTransport` to decode envelopes, dispatch operations, and encode JSON-RPC responses.

## Operational considerations

- Validate envelope fields strictly.
- Preserve client-provided request IDs in responses.
- Map structured SDK errors to consistent JSON-RPC error responses.
- Include auth metadata in `RequestContext` only after policy validation.
