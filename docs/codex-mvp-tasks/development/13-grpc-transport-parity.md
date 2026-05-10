# Task 13 — gRPC transport parity (client + server)

## Goal

Add first-class gRPC transport support so the C++ SDK reaches transport-level parity with a2a-go's gRPC path.

## Scope

- Client-side gRPC transport:
  - implement core RPCs (`SendMessage`, `GetTask`, `CancelTask`)
  - implement streaming (`SendStreamingMessage`, task subscription)
  - reuse existing auth and timeout hooks where feasible
- Server-side gRPC handler:
  - adapt existing dispatcher/executor contracts to generated gRPC service
  - map errors to canonical gRPC status + protocol payloads
- Discovery + resolver integration:
  - allow selecting gRPC endpoint when available/preferred

## Deliverables

- `include/a2a/client/grpc_transport.h`
- `src/client/grpc_transport.cpp`
- `include/a2a/server/grpc_server_transport.h`
- `src/server/grpc_server_transport.cpp`
- unit + integration tests for gRPC client/server interoperability
- one minimal `examples/grpc_client.cpp` and optional `examples/grpc_server.cpp`

## Constraints

- A2A 1.0 only.
- Prefer generated protobuf/gRPC code and avoid handwritten wire models.
- Keep transport adapter thin; business logic remains dispatcher/executor based.

## Implementation notes

- Reuse existing `CallOptions` semantics (headers/metadata, auth providers, mTLS intent).
- Verify cancellation behavior and stream shutdown semantics under load.
- Add CI job matrix entry that executes at least one gRPC interop integration test.

## Acceptance criteria

- gRPC client can call a gRPC server implementation backed by the existing executor.
- Streaming behavior is validated with deterministic tests.
- Discovery can choose gRPC when requested and correctly fall back when unavailable.

## Out of scope

- Custom gRPC load balancing strategies.
- Multi-cluster networking features not required for baseline SDK parity.
