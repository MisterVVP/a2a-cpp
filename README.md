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

See the platform-specific [installation guide](https://mistervvp.github.io/a2a-cpp/getting-started/installation.html).

## Quick start

On Debian, Ubuntu, or Windows Git Bash:

```bash
git clone https://github.com/MisterVVP/a2a-cpp.git
cd a2a-cpp
./scripts/install_build_deps.sh
./scripts/run_examples.sh build-example hello_agent
```

`hello_agent` runs a deterministic in-process client/server flow. The
[quickstart](https://mistervvp.github.io/a2a-cpp/getting-started/quickstart.html) covers macOS, Windows,
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

See the [CMake integration](https://mistervvp.github.io/a2a-cpp/build/cmake.html) and
[vcpkg](https://mistervvp.github.io/a2a-cpp/build/vcpkg.html) guides for additional workflows.

## Examples

The [`examples/`](examples/) directory includes client/server, transport,
streaming, push-notification, and authentication examples.

```bash
./scripts/run_examples.sh build-example   hello_agent streaming_client streaming_server push_notifications
```

## Documentation

- [Documentation website](https://mistervvp.github.io/a2a-cpp/)
- [Installation and quickstart](https://mistervvp.github.io/a2a-cpp/getting-started/installation.html)
- [Client and server APIs](https://mistervvp.github.io/a2a-cpp/client/overview.html)
- [Transports and streaming](https://mistervvp.github.io/a2a-cpp/transports/rest.html)
- [Authentication](https://mistervvp.github.io/a2a-cpp/auth/overview.html)
- [Storage](https://mistervvp.github.io/a2a-cpp/server/storage.html)
- [Performance testing](https://mistervvp.github.io/a2a-cpp/performance/workloads.html)
- [API reference](https://mistervvp.github.io/a2a-cpp/api-reference.html)
- [Releases and versioning](https://mistervvp.github.io/a2a-cpp/releases.html)

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

See [microbenchmark guide](https://mistervvp.github.io/a2a-cpp/performance/microbenchmarks.html) for microbenchmarks and
[workload testing guide](https://mistervvp.github.io/a2a-cpp/performance/workloads.html) for workload tests.

## Production tutorials

The realistic multi-process tutorials under `examples/tutorials/` demonstrate Agent Card discovery and HTTP+JSON A2A delegation. They support native Linux runs and cross-platform Docker Compose, including Docker Desktop on Windows without WSL.

See the [production tutorials guide](https://mistervvp.github.io/a2a-cpp/examples/tutorials.html) to run both tutorials or choose an individual tutorial.

## Repository policies

See [CONTRIBUTING.md](CONTRIBUTING.md) for contribution requirements,
[AGENTS.md](AGENTS.md) and [C++ conventions](cpp_conventions.md) for repository
engineering rules, [SECURITY.md](SECURITY.md) for vulnerability reporting, and
[SUPPORT.md](SUPPORT.md) for supported versions and release cadence.

## License

Licensed under the [Apache License 2.0](LICENSE).
