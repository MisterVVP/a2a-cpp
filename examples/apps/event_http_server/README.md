# Event-driven HTTP server

Build the SDK with `A2A_ENABLE_ASIO=ON`, install it, then build this consumer:

```sh
cmake -S examples/installed_package_consumer -B build-http-example \
  -DA2A_EXAMPLE_APP=event_http_server -DCMAKE_PREFIX_PATH=/path/to/sdk
cmake --build build-http-example
./build-http-example/a2a_example --smoke
```

Without `--smoke`, the example prints its ephemeral loopback port and waits for
Enter. Request `GET /a2a/tasks/example` with `A2A-Version: 1.0`. See the
[event runtime guide](../../../book/src/server/event-http.md) for limits,
streaming, cancellation, platform design, and shutdown ownership.
