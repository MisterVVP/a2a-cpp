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

## Task ID generation

- For new incoming messages where `message.taskId` is absent, the SDK service layer generates a server-side task id.
- The default generator is UUIDv7-based and emits ids like `task-0198f2d4-7c4a-7b21-9c02-dc6e7f2b8e91`.
- `message.messageId` is client-provided and is **not** used as a production task identifier.
- You can inject a custom task id strategy via `TaskLifecycleService` constructor if you require opaque/random ids or stricter privacy (UUIDv7 leaks approximate creation time).

## Example

See `examples/minimal_server_custom_executor.cpp` for a minimal in-process setup.
