# a2a-cpp: C++20 Agent2Agent (A2A) SDK

[![CI](https://img.shields.io/github/actions/workflow/status/MisterVVP/a2a-cpp/ci.yml?branch=main&label=CI)](https://github.com/MisterVVP/a2a-cpp/actions/workflows/ci.yml?query=branch%3Amain)
[![TCK conformance](https://img.shields.io/github/actions/workflow/status/MisterVVP/a2a-cpp/tck.yml?branch=main&label=TCK%20conformance)](https://github.com/MisterVVP/a2a-cpp/actions/workflows/tck.yml?query=branch%3Amain)
[![Documentation](https://img.shields.io/github/actions/workflow/status/MisterVVP/a2a-cpp/docs.yml?branch=main&label=docs)](https://github.com/MisterVVP/a2a-cpp/actions/workflows/docs.yml?query=branch%3Amain)
[![License](https://img.shields.io/github/license/MisterVVP/a2a-cpp)](LICENSE)

**a2a-cpp** is a C++20 SDK for building Agent2Agent (A2A) clients and servers.

It provides Agent Card discovery, task lifecycle operations, streaming, push
notifications, authentication hooks, and HTTP+JSON, JSON-RPC, and gRPC transports.

## Highlights

- Client APIs for messaging, task management, streaming, subscriptions, and push
  notification configuration.
- Server-side executor and transport abstractions for HTTP+JSON, JSON-RPC, and gRPC.
- Agent Card discovery, preferred-interface resolution, and extended-card support.
- Cancellable streaming with built-in SSE support for HTTP transports.
- Authentication metadata, interceptors, and required-extension validation.
- In-memory stores and optional PostgreSQL-backed stores.
- Installable CMake targets under the `a2a::` namespace.
- Continuous Linux, macOS, Windows, interoperability, benchmark, and
  [A2A TCK](https://github.com/MisterVVP/a2a-cpp/actions/workflows/tck.yml) validation.

## Requirements

- CMake 3.25 or newer.
- A C++20 compiler: GCC, Clang, AppleClang, or Visual Studio 2022.
- Protobuf and gRPC C++.
- libcurl for built-in HTTP+JSON, JSON-RPC, and SSE clients.
- PostgreSQL client libraries only when PostgreSQL stores are enabled.

See the platform-specific [installation guide](book/src/getting-started/installation.md).

## Quick start

On Debian, Ubuntu, or Windows Git Bash:

```bash
git clone https://github.com/MisterVVP/a2a-cpp.git
cd a2a-cpp
./scripts/install_build_deps.sh
./scripts/run_examples.sh build-example hello_agent
```

`hello_agent` runs a deterministic in-process client/server flow. The
[quickstart](book/src/getting-started/quickstart.md) covers macOS, Windows,
transport, streaming, push-notification, and authentication examples.

## Use with CMake FetchContent

```cmake
include(FetchContent)

set(A2A_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(A2A_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(A2A_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  a2a_cpp
  GIT_REPOSITORY https://github.com/MisterVVP/a2a-cpp.git
  GIT_TAG main
)
FetchContent_MakeAvailable(a2a_cpp)

target_link_libraries(my_agent PRIVATE a2a::client a2a::server a2a::core)
```

Pin `GIT_TAG` to a [release](https://github.com/MisterVVP/a2a-cpp/releases) or
reviewed commit for reproducible builds. See the complete
[`FetchContent` consumer](examples/fetch_content_consumer/).

## Use an installed CMake package

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
cmake --install build --prefix /tmp/a2a-cpp-install
```

```cmake
find_package(a2a_cpp CONFIG REQUIRED)

target_link_libraries(my_agent PRIVATE a2a::client a2a::server a2a::core)
```

See the [CMake integration](book/src/build/cmake.md) and
[vcpkg](book/src/build/vcpkg.md) guides for additional workflows.

## Examples

The [`examples/`](examples/) directory includes client/server, transport,
streaming, push-notification, and authentication examples.

```bash
./scripts/run_examples.sh build-example   hello_agent streaming_client streaming_server push_notifications
```

## Documentation

- [Documentation website](https://mistervvp.github.io/a2a-cpp/)
- [Installation and quickstart](book/src/getting-started/installation.md)
- [Client and server APIs](book/src/client/overview.md)
- [Transports and streaming](book/src/transports/rest.md)
- [Authentication](book/src/auth/overview.md)
- [Storage](docs/storage.md)
- [Performance testing](docs/performance-testing.md)
- [API reference](book/src/api-reference.md)
- [Releases and versioning](book/src/releases.md)

## Development

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Contributors should read [`CONTRIBUTING.md`](CONTRIBUTING.md) and
[`AGENTS.md`](AGENTS.md), then run:

```bash
./scripts/verify_changes.sh
```

See [`benchmarks/README.md`](benchmarks/README.md) for microbenchmarks and
[`docs/performance-testing.md`](docs/performance-testing.md) for workload tests.

## License

Licensed under the [Apache License 2.0](LICENSE).
