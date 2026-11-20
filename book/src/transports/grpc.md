# gRPC Transport

gRPC support is first-class for client and server integrations.

## Client side

Use `a2a::client::GrpcTransport` with a resolved interface and either:

- a `std::shared_ptr<grpc::Channel>` for real gRPC calls, or
- a custom `GrpcTransport::RpcClient` for tests and embedded adapters.

The transport supports unary task operations, streaming send, task subscription, and push-notification config lifecycle calls.

Channel-backed clients use native gRPC read reactors for both streaming methods.
Idle streams share gRPC's runtime threads instead of owning a blocking reader
thread. Observer callbacks run serially per stream on gRPC callback threads;
keep them brief. The next read starts after `OnEvent` returns, preserving order
and applying backpressure without an SDK frame queue. Injected `RpcClient`
implementations retain their blocking-reader compatibility path.

Cancellation suppresses subsequent observer calls. External cancellation waits
for an executing callback to return; cancellation from a stream callback does
not wait on other callbacks. Client destruction or transport shutdown cancels
native streams and drains native cleanup when called outside SDK callbacks.
Shutdown from a callback signals cancellation and lets owned native state finish
cleanup asynchronously.

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

## Opt-in native async unary server

Register `AsyncGrpcServerTransport` with an `engine::AsyncDispatcher` to use gRPC
unary callbacks. The existing `GrpcServerTransport` registration remains
synchronous. See [Native async gRPC](../server/async-grpc.md) for application
contracts, metadata/deadline mapping, graceful shutdown, installed consumers,
supported standard libraries, and thread/latency evidence.
