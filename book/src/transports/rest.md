# REST Transport

The REST transport uses HTTP+JSON endpoints for A2A operations.

## Client side

Use `HttpJsonTransport` with `A2AClient` when your deployment or gateway standardizes on REST.

## Server side

Use `RestServerTransport` to map inbound HTTP requests to dispatcher/executor operations.

## Operational considerations

- Ensure content-type and method validation is strict.
- Configure request deadlines/timeouts.
- Propagate auth metadata safely for policy checks.
- Log stable request/task identifiers.
