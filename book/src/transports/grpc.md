# gRPC Transport

gRPC support is first-class for client and server integrations.

## Client side

Use `a2a::client::GrpcTransport` with a resolved interface and either:

- a `std::shared_ptr<grpc::Channel>` for real gRPC calls, or
- a custom `GrpcTransport::RpcClient` for tests and embedded adapters.

The transport supports unary task operations, streaming send, task subscription, and push-notification config lifecycle calls.

## Server side

`a2a::server::GrpcServerTransport` implements `lf::a2a::v1::A2AService::Service` and routes RPCs to the dispatcher.

It supports:

- `SendMessage`
- `SendStreamingMessage`
- `GetTask`
- `ListTasks`
- `CancelTask`
- `SubscribeToTask`
- Push-notification config RPCs
- `GetExtendedAgentCard`

## Metadata and extensions

The server transport recognizes A2A metadata such as `a2a-version` and can validate required extensions through `GrpcServerTransportOptions::required_extensions`.

## Example

See `examples/apps/grpc_server/main.cpp`.
