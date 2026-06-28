# REST Server Transport

`RestServerTransport` adapts inbound HTTP+JSON requests into server dispatcher calls.

## Use cases

- Service deployments standardized on REST.
- Environments requiring straightforward HTTP middleware integration.

## Operational guidance

- Validate method/path/content-type boundaries strictly.
- Propagate request metadata needed by executors (including auth headers).
- Define timeout and payload size policies explicitly.
- Log stable identifiers for correlation.

## Authentication behavior

Inbound auth headers can be mapped into request context metadata and consumed by executor logic.

## Related files

- `tests/integration/rest_server_transport_integration_test.cpp`
- `examples/apps/rest_server/main.cpp`
