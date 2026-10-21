# Example applications

Run commands from the repository root. These small deterministic apps consume
public SDK targets through the [CMake consumer templates](../build/consumers.md).

## Build and run

```bash
cmake -S examples/fetch_content_consumer -B build-example \
  -DA2A_EXAMPLE_APP=hello_agent -DA2A_CPP_GIT_TAG=v0.5.2
cmake --build build-example --parallel
./build-example/a2a_example
```

Replace `hello_agent` with an app below. For an installed SDK, configure
`examples/installed_package_consumer` with `-DCMAKE_PREFIX_PATH=<install-prefix>`.
Multi-configuration Windows builds place the executable under `RelWithDebInfo/`.

Each app links `a2a::client`, `a2a::server` and `a2a::core` through the consumer
templates, prints concise task/status/event or delivery output, and exits
nonzero on an SDK error. Network streaming examples have their own
[client](streaming-client.md) and [server](streaming-server.md) guides.

## hello-agent

Smallest end-to-end in-process SDK example.

[Source](https://github.com/MisterVVP/a2a-cpp/blob/main/examples/apps/hello_agent/main.cpp).

## simple-client

Client-side request construction and task fetch flow.

[Source](https://github.com/MisterVVP/a2a-cpp/blob/main/examples/apps/simple_client/main.cpp).

## rest-server

Server-side REST transport setup and in-process handling.

[Source](https://github.com/MisterVVP/a2a-cpp/blob/main/examples/apps/rest_server/main.cpp).

## json-rpc-server

Server-side JSON-RPC transport setup.

[Source](https://github.com/MisterVVP/a2a-cpp/blob/main/examples/apps/json_rpc_server/main.cpp).

## grpc-server

gRPC service object setup and unary operation path.

[Source](https://github.com/MisterVVP/a2a-cpp/blob/main/examples/apps/grpc_server/main.cpp).

## push-notifications

Push notification configuration, store, and delivery abstraction.

[Source](https://github.com/MisterVVP/a2a-cpp/blob/main/examples/apps/push_notifications/main.cpp).

## auth-policy-server

Server-side auth metadata handling and policy shape.

[Source](https://github.com/MisterVVP/a2a-cpp/blob/main/examples/apps/auth_policy_server/main.cpp).
