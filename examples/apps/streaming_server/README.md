# streaming_server

`streaming_server` is a runnable HTTP+JSON SSE server for the `streaming_client` example. It listens on `127.0.0.1:8080` by default and exposes the REST A2A endpoint at `/a2a`.

## Build

Linux or macOS:

```bash
./scripts/run_examples.sh build-example streaming_server
```

Windows Git Bash:

```bash
./scripts/install_build_deps.sh
rm -rf build-example-streaming_server
./scripts/run_examples.sh build-example streaming_server
```

The runner builds the server and invokes `--help` as a non-blocking smoke check. Start the resulting executable manually when using it with the client.

## Run with streaming_client

Build both examples:

```bash
./scripts/run_examples.sh build-example streaming_server streaming_client
```

Terminal 1 — start the server:

```bash
./build-example-streaming_server/a2a_example
```

Terminal 2 — send a streaming request:

```bash
./build-example-streaming_client/a2a_example \
  --transport http_json \
  --endpoint http://127.0.0.1:8080/a2a \
  --operation send \
  --timeout-ms 10000
```

On Windows Git Bash, use these executable paths instead:

```bash
./build-example-streaming_server/RelWithDebInfo/a2a_example.exe
```

```bash
./build-example-streaming_client/RelWithDebInfo/a2a_example.exe \
  --transport http_json \
  --endpoint http://127.0.0.1:8080/a2a \
  --operation send \
  --timeout-ms 10000
```

Stop the server with `Ctrl+C`.

## Custom address

Pass a different IPv4 host and port as the only argument:

```bash
./build-example-streaming_server/a2a_example 127.0.0.1:9090
```

Then point the client to `http://127.0.0.1:9090/a2a`.

## What it demonstrates

The server publishes one terminal `TaskStatusUpdateEvent` through the SDK's `RestServerTransport`, `HttpAdapter`, and `ServerStreamSession` APIs. It advertises `capabilities.streaming: true` in its Agent Card.
