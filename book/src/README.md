# a2a-cpp C++20 Agent2Agent (A2A) SDK Documentation

Welcome to the **a2a-cpp** documentation for the C++20 Agent2Agent (A2A) SDK.

Use this guide to:

- install and build the SDK,
- build A2A clients and servers,
- select a transport (REST, JSON-RPC, or gRPC), and
- integrate streaming and authentication.

## Start here

- New to the SDK? Begin with the [Quickstart: Build and Run a REST Client](getting-started/quickstart.md).
- Building a client workflow? Continue to [Send Messages with A2AClient](client/sending-messages.md).
- Implementing server-side execution? Read [Custom Executor Design and Implementation](server/custom-executor.md).
- Choosing a transport? Compare [REST Transport](transports/rest.md), [JSON-RPC Transport](transports/json-rpc.md), and [gRPC Transport](transports/grpc.md).
- Securing requests? Review [Authentication Overview](auth/overview.md).

## SDK scope and capabilities

The SDK supports production-focused A2A capabilities including:

- Agent card discovery and task lifecycle APIs,
- REST, JSON-RPC, and gRPC transport integrations,
- Streaming event consumption,
- Client credential providers and server auth metadata propagation,
- Build integration with CMake and vcpkg.

## Examples

Curated CMake consumer examples live in the repository [`examples/`](../../examples/) directory and cover FetchContent plus installed-package usage.
