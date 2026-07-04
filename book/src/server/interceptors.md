# Server Interceptors

Server interceptors let deployments attach cross-cutting behavior around dispatcher operations.

## Common uses

- Authorization and policy checks.
- Request/response telemetry.
- Audit logging.
- Metrics and latency measurement.
- Required extension enforcement in combination with transport validators.

## Design guidance

- Keep interceptors deterministic and low-latency.
- Avoid logging secrets or raw credential values.
- Use structured errors so transports can map failures consistently.
- Prefer composition: keep business behavior in `AgentExecutor` and cross-cutting behavior in interceptors.

## Related APIs

- `a2a::server::ServerInterceptor`
- `a2a::server::Dispatcher::AddInterceptor`
- `a2a::client::ClientInterceptor` for client-side call hooks
