# a2a-cpp examples

This directory contains user-facing CMake consumer examples for the a2a-cpp SDK. The examples are small applications under `apps/`, and the two consumer templates build the same app sources in the two common downstream integration modes.

- `fetch_content_consumer/` shows how to consume a2a-cpp directly from a Git repository with CMake `FetchContent`.
- `installed_package_consumer/` shows how to consume an installed SDK package with `find_package(a2a_cpp CONFIG REQUIRED)`.

## Applications

- `hello_agent`: smallest end-to-end in-process client/server example and the CI smoke-test default.
- `simple_client`: deterministic client-side request construction and task fetch flow.
- `rest_server`: REST server transport setup and in-process request handling.
- `json_rpc_server`: JSON-RPC server transport setup with a client request.
- `grpc_server`: gRPC service object setup with a unary call path.
- `streaming_client`: client-side streaming consumption with deterministic server-sent events.
- `streaming_server`: server-side stream session publication and consumption.
- `push_notifications`: push notification store and delivery abstraction.
- `auth_policy_server`: auth metadata handling shape for server-side policy examples.

## FetchContent consumer

```bash
cmake -S examples/fetch_content_consumer -B build-example -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-example --parallel
./build-example/a2a_example
```

## Installed package consumer

```bash
cmake -S examples/installed_package_consumer -B build-installed-example -DCMAKE_PREFIX_PATH=<install-prefix> -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-installed-example --parallel
./build-installed-example/a2a_example
```
