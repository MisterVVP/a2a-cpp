# streaming_client

`streaming_client` is a production-default streaming client example. It uses the SDK's libcurl-backed default HTTP transports instead of a synthetic requester and supports both HTTP+JSON SSE and JSON-RPC-over-SSE.

## Prerequisites

The SDK always requires gRPC and Protobuf. This example also requires libcurl for the default HTTP+JSON and JSON-RPC streaming transports.

- Debian/Ubuntu: run `./scripts/install_build_deps.sh`.
- macOS: run `brew install cmake ninja protobuf grpc re2 abseil curl`.
- Windows: use Visual Studio 2022 with the Desktop development with C++ workload and Git for Windows. Run `./scripts/install_build_deps.sh` from Git Bash.

## Build the client and example server

Linux or macOS:

```bash
./scripts/run_examples.sh build-example streaming_server streaming_client
```

The runner builds both programs and invokes their `--help` modes as non-blocking smoke checks. It does not leave the server running.

Windows Git Bash:

```bash
./scripts/install_build_deps.sh
rm -rf build-example-streaming_server build-example-streaming_client
./scripts/run_examples.sh build-example streaming_server streaming_client
```

The dependency script bootstraps vcpkg under `${VCPKG_ROOT:-$HOME/vcpkg}`, installs the root manifest's gRPC, Protobuf, and curl dependencies, and defaults both triplets to `x64-windows`. The example runner uses the same defaults, passes the vcpkg CMake toolchain, builds `RelWithDebInfo`, and locates the Visual Studio multi-configuration output automatically.

Delete an example build directory before retrying if it was previously configured without `CMAKE_TOOLCHAIN_FILE`; CMake caches the toolchain during the first configure.

## Run the paired examples

Terminal 1 — start the HTTP+JSON SSE server:

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

On Windows Git Bash, use:

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

Stop the server with `Ctrl+C`. See [`../streaming_server/README.md`](../streaming_server/README.md) for custom listen addresses and server details.

## Usage

Linux and macOS:

```text
./build-example-streaming_client/a2a_example \
  --transport http_json|jsonrpc \
  --endpoint <url> \
  --operation send|subscribe \
  [--task-id <id>] \
  [--timeout-ms <milliseconds>] \
  [--cancel-after-first-event]
```

Windows Git Bash:

```text
./build-example-streaming_client/RelWithDebInfo/a2a_example.exe \
  --transport http_json|jsonrpc \
  --endpoint <url> \
  --operation send|subscribe \
  [--task-id <id>] \
  [--timeout-ms <milliseconds>] \
  [--cancel-after-first-event]
```

The target server must advertise `capabilities.streaming: true` and expose the selected transport endpoint. Use `--task-id` with `--operation send` only when updating an existing task. Use `--task-id` with `--operation subscribe` to identify the existing task subscription.

### Send streaming message

HTTP+JSON:

```bash
./build-example-streaming_client/a2a_example \
  --transport http_json \
  --endpoint http://127.0.0.1:8080/a2a \
  --operation send \
  --timeout-ms 10000
```

JSON-RPC:

```bash
./build-example-streaming_client/a2a_example \
  --transport jsonrpc \
  --endpoint http://127.0.0.1:8080/rpc \
  --operation send \
  --timeout-ms 10000
```

The repository's `streaming_server` example currently exposes HTTP+JSON SSE. Use a JSON-RPC-capable server for the JSON-RPC command.

### Subscribe to an existing task

```bash
./build-example-streaming_client/a2a_example \
  --transport http_json \
  --endpoint http://127.0.0.1:8080/a2a \
  --operation subscribe \
  --task-id task-123 \
  --timeout-ms 10000
```

For JSON-RPC, use the JSON-RPC endpoint and `--transport jsonrpc`. On Windows, use the multi-configuration executable path shown above.

### Cancellation

Add `--cancel-after-first-event` to demonstrate explicit cancellation after the first decoded event. Cancellation is requested by the main thread after the observer signals that the first event arrived; callbacks are not expected after cancellation settles.

## Output and lifecycle notes

The example prints every supported stream variant: `Task`, `TaskStatusUpdateEvent`, and `TaskArtifactUpdateEvent`. Observer callbacks run on the transport worker thread, so the observer is kept alive until completion, error, cancellation, or timeout. The wait is bounded by `--timeout-ms`; a timeout cancels the stream and returns non-zero.
