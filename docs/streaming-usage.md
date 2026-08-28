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

## Default HTTP network reactor

Streams issued through one default libcurl-backed HTTP client share a single
`CURLM` multi handle and reactor thread. The reactor is the only thread that
adds or removes easy handles. Start, cancellation, and shutdown operations are
serialized through a synchronized command queue.

The reactor is initialized lazily by the first streaming request. Constructing
or using a client exclusively for unary requests does not start a streaming
reactor thread; concurrent first-stream calls synchronize initialization and
reuse the same reactor.

On Linux, libcurl socket and timer callbacks maintain an `epoll` readiness set
and a `timerfd`. An `eventfd` wakes the reactor immediately for queued commands,
including cancellation and shutdown, with no fixed polling interval. Socket and
timer readiness is forwarded to `curl_multi_socket_action()`. Other platforms
use libcurl's event-driven multi wakeup fallback without a fixed wake interval.

The synchronous `a2a::http::Client::StreamRequest()` API waits for its transfer
while network I/O progresses on the shared reactor. Injected custom stream
requesters retain their existing synchronous compatibility path.

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

## Build and run the paired HTTP+JSON examples

The SDK build requires gRPC and Protobuf even when the examples use an HTTP transport. The default HTTP streaming client additionally requires libcurl.

On Debian, Ubuntu, or macOS, build both examples with:

```bash
./scripts/install_build_deps.sh
./scripts/run_examples.sh build-example streaming_server streaming_client
```

On macOS, `brew install cmake ninja protobuf grpc re2 abseil curl` may be used instead of the dependency script.

On Windows, install Visual Studio 2022 with the C++ workload and use Git Bash:

```bash
./scripts/install_build_deps.sh
rm -rf build-example-streaming_server build-example-streaming_client
./scripts/run_examples.sh build-example streaming_server streaming_client
```

The runner only invokes `--help` for these long-running/network-dependent examples. Start the server manually in the first terminal.

Linux or macOS, terminal 1:

```bash
./build-example-streaming_server/a2a_example
```

Linux or macOS, terminal 2:

```bash
./build-example-streaming_client/a2a_example \
  --transport http_json \
  --endpoint http://127.0.0.1:8080/a2a \
  --operation send \
  --timeout-ms 10000
```

Windows Git Bash, terminal 1:

```bash
./build-example-streaming_server/RelWithDebInfo/a2a_example.exe
```

Windows Git Bash, terminal 2:

```bash
./build-example-streaming_client/RelWithDebInfo/a2a_example.exe \
  --transport http_json \
  --endpoint http://127.0.0.1:8080/a2a \
  --operation send \
  --timeout-ms 10000
```

Stop the server with `Ctrl+C`. The client prints the terminal status update and exits after the server closes the SSE stream.

## Other servers and operations

Point the generated client at any server whose Agent Card advertises `capabilities.streaming: true`:

```bash
./build-example-streaming_client/a2a_example --transport jsonrpc --endpoint http://127.0.0.1:8080/rpc --operation send --timeout-ms 10000
./build-example-streaming_client/a2a_example --transport http_json --endpoint http://127.0.0.1:8080/a2a --operation subscribe --task-id task-123 --timeout-ms 10000
```

The repository's `streaming_server` example exposes HTTP+JSON SSE. Use a JSON-RPC-capable server for the JSON-RPC command. Add `--cancel-after-first-event` to request cancellation after the first event.
