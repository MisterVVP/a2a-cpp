# Streaming usage

Streaming is exposed through `A2AClient::SendStreamingMessage` and `A2AClient::SubscribeTask`.

Implement `a2a::client::StreamObserver`:

- `OnEvent` for each `StreamResponse` event.
- `OnError` for transport/protocol failures.
- `OnCompleted` when stream finishes.

Use the returned `StreamHandle` to cancel long-running streams.

See `examples/apps/streaming_client/main.cpp` and `tests/functional/examples_functional_test.cpp`.
