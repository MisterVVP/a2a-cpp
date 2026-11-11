# a2a-cpp: C++20 Agent2Agent (A2A) SDK

[![CI](https://img.shields.io/github/actions/workflow/status/MisterVVP/a2a-cpp/ci.yml?branch=main&label=CI)](https://github.com/MisterVVP/a2a-cpp/actions/workflows/ci.yml?query=branch%3Amain)
[![TCK conformance](https://img.shields.io/github/actions/workflow/status/MisterVVP/a2a-cpp/tck.yml?branch=main&label=TCK%20conformance)](https://github.com/MisterVVP/a2a-cpp/actions/workflows/tck.yml?query=branch%3Amain)
[![Documentation](https://img.shields.io/github/actions/workflow/status/MisterVVP/a2a-cpp/docs.yml?branch=main&label=docs)](https://github.com/MisterVVP/a2a-cpp/actions/workflows/docs.yml?query=branch%3Amain)
[![License](https://img.shields.io/github/license/MisterVVP/a2a-cpp)](LICENSE)

**a2a-cpp SDK v0.6.0** is a C++20 SDK for building Agent2Agent (A2A) clients and servers.
It implements **A2A protocol 1.0 (v1.0.1 specification)**; protocol and SDK
versions are independent. v0.6.0 is a release candidate until tagged. See the
[conformance scope](https://mistervvp.github.io/a2a-cpp/compatibility/conformance.html)
for measured coverage and release-gate limitations.

It provides Agent Card discovery, task lifecycle operations, streaming, push
notifications, authentication hooks, and HTTP+JSON, JSON-RPC, and gRPC transports.

## Highlights

- Client APIs for messaging, task management, streaming, subscriptions, and push
  notification configuration.
- Server-side executor and transport abstractions for HTTP+JSON, JSON-RPC, and gRPC.
- Agent Card discovery, preferred-interface resolution, and extended-card support.
- Cancellable streaming with built-in SSE support for HTTP transports.
- Authentication metadata, interceptors, and required-extension validation.
- Opt-in async unary clients, coroutine dispatch, native async gRPC and an
  event-driven HTTP/SSE server; existing synchronous APIs remain available.
- In-memory stores and optional PostgreSQL-backed stores. PostgreSQL store I/O
  remains synchronous, including when called from coroutine applications.
- Installable CMake targets under the `a2a::` namespace.
- Continuous Linux, macOS, Windows, interoperability, benchmark, and
  [A2A TCK](https://github.com/MisterVVP/a2a-cpp/actions/workflows/tck.yml) validation.

## Requirements

- CMake 3.25 or newer.
- A C++20 compiler: GCC, Clang, AppleClang, or Visual Studio 2022.
- Protobuf and gRPC C++.
- libcurl for built-in HTTP+JSON, JSON-RPC, and SSE clients.
- simdjson 3.0+ for SDK JSON parsing.
- Boost 1.92.0+ only for `A2A_ENABLE_ASIO=ON` / the overlay `asio` feature.
- OpenSSL 3.0+ only for optional Agent Card signing.
- PostgreSQL client libraries only when PostgreSQL stores are enabled.

See the platform-specific [installation guide](https://mistervvp.github.io/a2a-cpp/getting-started/installation.html).

## Quick start

On Debian, Ubuntu, or Windows Git Bash:

```bash
git clone --branch release/v0.6.0 https://github.com/MisterVVP/a2a-cpp.git
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
  GIT_TAG release/v0.6.0
)
FetchContent_MakeAvailable(a2a_cpp)

target_link_libraries(my_agent PRIVATE a2a::client a2a::server a2a::core)
```

The untagged candidate uses `release/v0.6.0`. Pin `GIT_TAG` to a
[release](https://github.com/MisterVVP/a2a-cpp/releases) or
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

## Opt in to async APIs

Async unary clients are available through `a2a::client` (built-in HTTP needs
libcurl). Coroutine server execution, native async gRPC and the event-driven
HTTP server require `-DA2A_ENABLE_ASIO=ON` when building the SDK and linking
`a2a::asio`. Set that option before `FetchContent_MakeAvailable` for source
consumers. Select the reviewed v0.6.0 candidate commit, or set
`FETCHCONTENT_SOURCE_DIR_A2A_CPP` to your candidate checkout while its release
tag is unpublished. The [consumer guide](https://mistervvp.github.io/a2a-cpp/build/consumers.html)
shows that local path. For installed packages it is a build-time choice; setting it only in
a downstream application cannot add the target to a synchronous SDK.

```bash
cmake -S . -B build-asio -DCMAKE_BUILD_TYPE=Release -DA2A_ENABLE_ASIO=ON
cmake --build build-asio --parallel
cmake --install build-asio --prefix /tmp/a2a-cpp-asio
cmake -S examples/installed_package_consumer -B build-event-example \
  -DCMAKE_PREFIX_PATH=/tmp/a2a-cpp-asio -DA2A_EXAMPLE_APP=event_http_server
cmake --build build-event-example --parallel
./build-event-example/a2a_example --smoke
```

Make Boost discoverable via `CMAKE_PREFIX_PATH`; see the
[build guide](https://mistervvp.github.io/a2a-cpp/build/cmake.html).
The repository vcpkg overlay accepts `a2a-cpp[asio]` and
`a2a-cpp[asio,postgres-store]`. Async execution is opt-in; this release does not
make every transport or database operation asynchronous by default.

Follow the [async client](https://mistervvp.github.io/a2a-cpp/client/async-unary.html),
[coroutine runtime](https://mistervvp.github.io/a2a-cpp/server/asio-coroutines.html),
[native gRPC](https://mistervvp.github.io/a2a-cpp/server/async-grpc.html),
[streaming](https://mistervvp.github.io/a2a-cpp/server/async-streaming.html), and
[event HTTP](https://mistervvp.github.io/a2a-cpp/server/event-http.html) guides.
Review the [v0.6.0 migration notes](https://mistervvp.github.io/a2a-cpp/releases.html)
for opaque pagination cursors, JSON parsing changes and dependency requirements.

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
