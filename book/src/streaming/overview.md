# Streaming

Streaming is available through client APIs and server stream sessions.

## Client APIs

- `A2AClient::SendStreamingMessage(request, observer, options)`
- `A2AClient::SubscribeTask(request, observer, options)`

Implement `a2a::client::StreamObserver`:

- `OnEvent(const StreamResponse&)` for each event.
- `OnError(const core::Error&)` for transport or protocol failures.
- `OnCompleted()` when the stream finishes normally.

Both calls return a `StreamHandle`. Call `Cancel()` to request cancellation and `IsActive()` to check handle state.

## Threading contract

Observer callbacks run on transport-managed background threads. Keep observers alive until stream completion, cancellation, or handle destruction. Callback code should be thread-safe, fast, and non-blocking.

## Server side

Executors return `std::unique_ptr<ServerStreamSession>` from `SendStreamingMessage` and, optionally, `SubscribeTask`. A stream session publishes `StreamResponse` values until it returns an empty optional or an error.

## Examples

- `examples/apps/streaming_client/main.cpp`
- `examples/apps/streaming_server/main.cpp`
