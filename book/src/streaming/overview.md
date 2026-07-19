# Streaming

Streaming is available through all production client transports:

- gRPC native server streaming;
- HTTP+JSON SSE;
- JSON-RPC response envelopes carried in SSE `data:` fields.

Servers should advertise `capabilities.streaming: true` in their Agent Card.

## Client APIs

- `A2AClient::SendStreamingMessage(request, observer, options)`
- `A2AClient::SubscribeTask(request, observer, options)`

Implement `a2a::client::StreamObserver`:

- `OnEvent(const StreamResponse&)` for each decoded event.
- `OnError(const core::Error&)` once for transport or protocol failure.
- `OnCompleted()` once after a clean remote close.

Both calls return a `StreamHandle`. Call `Cancel()` to request cancellation and `IsActive()` to check handle state. Destruction also cancels the request and joins its worker.

## Threading contract

Observer callbacks run on transport-managed background threads. Keep observers alive until stream completion, cancellation, or handle destruction. Callback code should be thread-safe, fast, and non-blocking.

## Build the streaming client example

The SDK requires gRPC and Protobuf at configure time. The default HTTP streaming transports also require libcurl.

Linux or macOS:

```bash
./scripts/run_examples.sh build-example streaming_client
```

Windows Git Bash:

```bash
./scripts/install_build_deps.sh
rm -rf build-example-streaming_client
./scripts/run_examples.sh build-example streaming_client
./build-example-streaming_client/RelWithDebInfo/a2a_example.exe --help
```

See [Installation and Build](../getting-started/installation.md) and [vcpkg](../build/vcpkg.md) for dependency and toolchain details.

## Server side

Executors return `std::unique_ptr<ServerStreamSession>` from `SendStreamingMessage` and, optionally, `SubscribeTask`. A stream session publishes `StreamResponse` values until it returns an empty optional or an error.

## Examples

- `examples/apps/streaming_client/main.cpp`
- `examples/apps/streaming_server/main.cpp`
