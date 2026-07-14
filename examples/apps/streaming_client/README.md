# streaming_client

`streaming_client` is a production-default streaming client example. It uses the SDK's libcurl-backed default HTTP transports instead of a synthetic requester and supports both HTTP+JSON SSE and JSON-RPC-over-SSE.

## Build

Configure the repository or example consumer with libcurl support enabled, then build examples:

```bash
cmake -S . -B build -DA2A_ENABLE_EXAMPLES=ON -DA2A_ENABLE_LIBCURL=ON
cmake --build build --target streaming_client
```

You can also use the repository example runner:

```bash
./scripts/run_examples.sh build-examples
```

## Usage

```text
streaming_client \
  --transport http_json|jsonrpc \
  --endpoint <url> \
  --operation send|subscribe \
  [--task-id <id>] \
  [--timeout-ms <milliseconds>] \
  [--cancel-after-first-event]
```

The target server must advertise `capabilities.streaming: true` and expose the selected transport endpoint.

### Send streaming message

HTTP+JSON:

```bash
./build/examples/apps/streaming_client/streaming_client \
  --transport http_json \
  --endpoint http://127.0.0.1:8080/a2a \
  --operation send \
  --timeout-ms 10000
```

JSON-RPC:

```bash
./build/examples/apps/streaming_client/streaming_client \
  --transport jsonrpc \
  --endpoint http://127.0.0.1:8080/rpc \
  --operation send \
  --timeout-ms 10000
```

### Subscribe to an existing task

```bash
./build/examples/apps/streaming_client/streaming_client \
  --transport http_json \
  --endpoint http://127.0.0.1:8080/a2a \
  --operation subscribe \
  --task-id task-123 \
  --timeout-ms 10000
```

For JSON-RPC, use the JSON-RPC endpoint and `--transport jsonrpc`.

### Cancellation

Add `--cancel-after-first-event` to demonstrate explicit cancellation after the first decoded event. Cancellation is requested by the main thread after the observer signals that the first event arrived; callbacks are not expected after cancellation settles.

## Output and lifecycle notes

The example prints every supported stream variant: `Task`, `TaskStatusUpdateEvent`, and `TaskArtifactUpdateEvent`. Observer callbacks run on the transport worker thread, so the observer is kept alive until completion, error, cancellation, or timeout. The wait is bounded by `--timeout-ms`; a timeout cancels the stream and returns non-zero.

## Running against the repository SUT

Start the local REST or JSON-RPC example server in another terminal, note its loopback port and endpoint, then run one of the commands above. Use the REST `/a2a` endpoint with `--transport http_json` and the JSON-RPC `/rpc` endpoint with `--transport jsonrpc`.
