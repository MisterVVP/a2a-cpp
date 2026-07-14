# Streaming usage

Streaming is exposed through `A2AClient::SendStreamingMessage` and `A2AClient::SubscribeTask` for all production client transports:

- gRPC uses native server streaming;
- HTTP+JSON uses `text/event-stream` responses from the REST streaming and task-subscription endpoints;
- JSON-RPC over HTTP posts a JSON-RPC request envelope and receives JSON-RPC response envelopes inside SSE `data:` fields.

Servers are expected to advertise `capabilities.streaming: true` in their Agent Card. Transport instances validate the server response shape (`HTTP 2xx`, `Content-Type: text/event-stream`, valid SSE frames, valid protocol payloads), while capability discovery and policy decisions remain the caller or client-factory responsibility.

## Operations

`SendStreamingMessage` starts work and subscribes to updates in a single call. `SubscribeTask` reconnects to an existing active task and is the preferred recovery path after a broken stream. Servers close successful streams after a terminal or interrupted task state.

Implement `a2a::client::StreamObserver`:

- `OnEvent` is called for each decoded `StreamResponse`, including `Task`, `TaskStatusUpdateEvent`, and `TaskArtifactUpdateEvent` variants.
- `OnError` is called once for transport, HTTP, SSE, JSON/protobuf, or JSON-RPC envelope failures.
- `OnCompleted` is called once after a clean server close. It is mutually exclusive with `OnError`.

Observer callbacks run on the transport worker thread that owns the stream request. Keep callbacks fast, avoid blocking indefinitely, and hand work to an application executor if expensive processing is needed.

## Handles, cancellation, and timeouts

The returned `StreamHandle` remains active while the request is running. `Cancel()` is idempotent and requests prompt cancellation; destroying the handle also cancels and joins the worker before returning. Cancellation is not reported as normal completion. Configure bounded operation timeouts with `CallOptions::timeout` or the transport default timeout.

## HTTP transport examples

```cpp
auto rest = a2a::client::HttpJsonTransport::CreateDefault(resolved_rest_interface);
a2a::client::A2AClient rest_client(std::move(rest));
auto rest_stream = rest_client.SendStreamingMessage(send_request, observer, call_options);
```

```cpp
auto jsonrpc = a2a::client::JsonRpcTransport::CreateDefault(resolved_jsonrpc_interface);
a2a::client::A2AClient jsonrpc_client(std::move(jsonrpc));
auto subscription = jsonrpc_client.SubscribeTask(get_task_request, observer, call_options);
```

Both default HTTP transports use the shared libcurl-backed SSE requester. Custom `HttpStreamRequester` injection remains available for tests and custom HTTP stacks.

See `examples/apps/streaming_client/main.cpp` and `tests/functional/examples_functional_test.cpp`.

## Production streaming client example

`examples/apps/streaming_client/main.cpp` demonstrates production default transports for both HTTP+JSON and JSON-RPC. Build it with libcurl-enabled examples and point it at a server whose Agent Card advertises `capabilities.streaming: true`:

```bash
./build/examples/apps/streaming_client/streaming_client --transport http_json --endpoint http://127.0.0.1:8080/a2a --operation send --timeout-ms 10000
./build/examples/apps/streaming_client/streaming_client --transport jsonrpc --endpoint http://127.0.0.1:8080/rpc --operation send --timeout-ms 10000
./build/examples/apps/streaming_client/streaming_client --transport http_json --endpoint http://127.0.0.1:8080/a2a --operation subscribe --task-id task-123 --timeout-ms 10000
```

The example prints `Task`, status-update, and artifact-update variants, keeps the observer alive for the full stream lifetime, uses a bounded condition-variable wait, and returns non-zero on timeout or `OnError`. Add `--cancel-after-first-event` to request cancellation after the first event.
