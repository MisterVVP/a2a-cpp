# REST Transport for A2A Clients and Servers

The REST transport uses HTTP+JSON endpoints for A2A operations.

Need a runnable baseline first? Start with the [Quickstart](../getting-started/quickstart.md).

## Client side

Use `HttpJsonTransport` with `A2AClient` when your deployment or gateway standardizes on REST.

## Server side

Use `RestServerTransport` to map inbound HTTP requests to dispatcher/executor operations.

## Operational considerations

- Ensure content-type and method validation is strict.
- Configure request deadlines/timeouts.
- Propagate auth metadata safely for policy checks.
- Log stable request/task identifiers.

## See also

- [Send Messages with A2AClient](../client/sending-messages.md)
- [Custom Executor Design and Implementation](../server/custom-executor.md)
- [JSON-RPC Transport](json-rpc.md)
- [Authentication Overview](../auth/overview.md)
