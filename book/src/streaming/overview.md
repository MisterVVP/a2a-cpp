# Streaming Overview

Streaming is available through:

- `A2AClient::SendStreamingMessage`
- `A2AClient::SubscribeTask`

## Observer contract

Implement `a2a::client::StreamObserver`:

- `OnEvent` for each `StreamResponse`
- `OnError` for transport/protocol failures
- `OnCompleted` on normal stream completion

Use `StreamHandle` to cancel long-running streams.

## Operational guidance

- Handle event ordering explicitly in consumers.
- Make callback code thread-safe and non-blocking.
- Plan cancellation and shutdown semantics up front.

## Example and tests

- `examples/streaming_client.cpp`
- `tests/functional/examples_functional_test.cpp`
