# Server usage

Implement `a2a::server::AgentExecutor` and route requests through a transport:

- `RestServerTransport` for HTTP+JSON REST paths.
- `JsonRpcServerTransport` for JSON-RPC 2.0 method dispatch.

Minimal in-process setup:

1. Implement custom executor logic.
2. Create `a2a::server::Dispatcher` with the executor.
3. Create a transport and forward inbound HTTP requests to `Handle(...)`.

See `examples/minimal_server_custom_executor.cpp` for a minimal setup.
