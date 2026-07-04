# a2a-cpp C++20 Agent2Agent (A2A) SDK Documentation

**a2a-cpp** is a C++20 SDK for building Agent2Agent (A2A) protocol clients and servers. The current documented release focuses on production-oriented protocol coverage: REST, JSON-RPC, and gRPC transports; Agent Card discovery; task lifecycle APIs; streaming; push notification configuration APIs; authentication metadata propagation; interceptors; CMake package exports; and vcpkg-oriented packaging.

## What is included

- Client API: `SendMessage`, `GetTask`, `ListTasks`, `CancelTask`, streaming send/subscribe, and task push-notification config lifecycle calls.
- Server API: executor-driven dispatch for REST, JSON-RPC, and gRPC transports.
- Discovery: public and extended Agent Card fetch plus preferred-interface resolution.
- Streaming: client observers, cancellable stream handles, and server stream sessions.
- Authentication hooks: client credential providers and server request metadata extraction.
- Operational extensions: client/server interceptors, required-extension validation, task stores, task history ordering, UUIDv7 task IDs, and optional PostgreSQL stores.
- Build integration: CMake 3.25+, C++20, installable CMake package exports, generated protobuf headers, and vcpkg overlay/public-registry preparation.

## Recommended reading path

1. [Installation and Build](getting-started/installation.md) for toolchain requirements and CMake options.
2. [Quickstart](getting-started/quickstart.md) for a copy/paste example flow.
3. [Client Overview](client/overview.md) or [Server Overview](server/overview.md), depending on your integration role.
4. [Transports](transports/rest.md), [Streaming](streaming/overview.md), and [Authentication](auth/overview.md) for runtime design decisions.
5. [API Reference](api-reference.md) when you need generated public-header details.

## Version and support notes

- See [Releases and Versions](releases.md) for current release details and versioning guidance.
- The SDK is C++20-only and exports CMake targets under the `a2a::` namespace.
- Package documentation describes vcpkg workflows only.
- Examples are deterministic and are intended to run without external services unless the example README says otherwise.
