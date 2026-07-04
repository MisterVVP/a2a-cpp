# Server Overview

Server integrations implement `a2a::server::AgentExecutor` and route protocol requests through `Dispatcher` plus one or more transports.

## Server capabilities

- REST server transport.
- JSON-RPC server transport.
- gRPC service transport.
- Public and extended Agent Card dispatch when a provider is installed.
- Required extension validation.
- Server interceptors.
- Streaming through `ServerStreamSession`.
- Task lifecycle helpers, in-memory task store, and optional PostgreSQL stores.
- Push-notification config CRUD and delivery service abstractions.

## Core flow

1. Implement an executor for task and message behavior.
2. Create a `Dispatcher` with the executor and optional Agent Card provider/interceptors.
3. Attach REST, JSON-RPC, or gRPC transport adapters.
4. Convert inbound framework requests into transport calls.
5. Return structured protocol errors instead of transport-specific ad hoc errors.

## Task IDs

When an incoming message does not carry `message.task_id`, the SDK service layer can generate server-side task IDs using UUIDv7. UUIDv7 is sortable and operationally useful, but it leaks approximate creation time. Inject a custom task ID generator if your deployment needs opaque identifiers.
