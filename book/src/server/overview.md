# Server Overview

Server integration starts by implementing executor logic and attaching it to a transport.

## Core components

- `a2a::server::AgentExecutor`: your business logic entrypoint.
- `a2a::server::Dispatcher`: routes protocol operations to executor methods.
- Transport adapter:
  - `RestServerTransport` for HTTP+JSON REST paths.
  - `JsonRpcServerTransport` for JSON-RPC 2.0 method dispatch.

## Happy path

1. Implement a custom executor.
2. Build a `Dispatcher` with that executor.
3. Create REST or JSON-RPC transport.
4. Forward inbound requests to transport `Handle(...)`.

## Example

See `examples/minimal_server_custom_executor.cpp` for a minimal in-process setup.
