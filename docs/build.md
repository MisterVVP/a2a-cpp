# Build instructions

## Prerequisites

- CMake 3.25+
- A C++20 toolchain (Clang or GCC)
- Protobuf (with `protoc`)
- gRPC C++ and `grpc_cpp_plugin`
- clang-format
- clang-tidy

## Configure

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

## Build

```bash
cmake --build build
```

## Optional libcurl support

The SDK can be configured without libcurl when default outbound HTTP is not needed:

```bash
cmake -S . -B build -DA2A_ENABLE_LIBCURL=OFF
```

With libcurl disabled or unavailable, the core SDK, injectable client transports, server request handling, and custom push-delivery implementations still build. The built-in buffered outbound HTTP implementation remains present as an API surface, but `a2a::http::Client::SendRequest` returns an internal error explaining that libcurl support is disabled. Use injected `HttpRequester`, `HttpFetcher`, `HttpStreamRequester`, or a custom `PushNotificationDeliveryClient` in libcurl-free builds.

When `A2A_ENABLE_LIBCURL=ON` (the default), CMake enables the shared libcurl-backed implementation only if `CURL::libcurl` is found; otherwise it continues with the limited libcurl-free feature set.


## Build examples

Curated examples are standalone consumer projects under `examples/`. Build them through the same CMake paths used by downstream applications instead of through the repository root:

```bash
cmake -S examples/fetch_content_consumer -B build-example -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-example --parallel
./build-example/a2a_example
```

After installing the SDK, the same app sources can be built with `find_package`:

```bash
cmake -S examples/installed_package_consumer -B build-installed-example \
  -DCMAKE_PREFIX_PATH=<install-prefix> \
  -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-installed-example --parallel
./build-installed-example/a2a_example
```

To run only proto generation:

```bash
cmake --build build --target a2a_proto_codegen
```

## Run tests

```bash
ctest --test-dir build --output-on-failure
```

## Run style checks

```bash
clang-format --dry-run --Werror $(git ls-files '*.h' '*.hpp' '*.c' '*.cpp')
```

## Run lints

```bash
./scripts/run_clang_tidy.sh build
```

## Notes on code generation

- Proto definitions are kept under `proto/a2a/v1/a2a.proto`.
- Generated outputs are written to `build/generated/a2a/v1/`.
- Generation is wired through the `a2a::proto_generated` target and runs automatically when needed.

## Generate API reference

```bash
./scripts/generate_api_reference.sh
```

This generates Doxygen documentation from public headers in `include/a2a/**` and publishes it locally at `book-build/api/cpp/index.html`.

## CI

- `.github/workflows/ci.yml` validates formatting, configure/build, clang-tidy, and tests.
- `.github/workflows/cmake-package.yml` validates that external CMake projects can consume `a2a-cpp` through both `FetchContent` and installed `find_package` flows.
- `.github/workflows/codeql.yml` runs CodeQL analysis for C/C++ on push, pull request, and a weekly schedule.

## Use With CMake FetchContent

For application projects, prefer `FetchContent` when you want CMake to fetch and build `a2a-cpp` as part of the consumer build:

```cmake
cmake_minimum_required(VERSION 3.25)

project(my_a2a_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(A2A_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(A2A_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(A2A_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(A2A_ENABLE_POSTGRES_STORE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  a2a_cpp
  GIT_REPOSITORY https://github.com/MisterVVP/a2a-cpp.git
  GIT_TAG main
)
FetchContent_MakeAvailable(a2a_cpp)

add_executable(my_a2a_agent main.cpp)
target_link_libraries(my_a2a_agent PRIVATE a2a::client a2a::server a2a::core)
```

Pin `GIT_TAG` to a release tag or commit for reproducible builds. See `examples/fetch_content_consumer/` for a minimal consumer project. The CI workflow overrides the repository and tag to validate the current PR head through a public GitHub clone URL.

## Install package

Use install mode when you want to package `a2a-cpp`, install it into a prefix, or consume it through package managers such as vcpkg:

```bash
cmake -S . -B build-install \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DA2A_ENABLE_TESTING=OFF \
  -DA2A_BUILD_EXAMPLES=OFF \
  -DA2A_BUILD_BENCHMARKS=OFF

cmake --build build-install --parallel
cmake --install build-install --prefix <install-prefix>
```

This installs headers, generated protobuf headers, static libraries, and exported CMake package files under `lib/cmake/a2a_cpp`.

## Use installed CMake package

A downstream CMake project can consume an installed SDK with `find_package`:

```cmake
cmake_minimum_required(VERSION 3.25)

project(my_a2a_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(a2a_cpp CONFIG REQUIRED)

add_executable(my_a2a_agent main.cpp)
target_link_libraries(my_a2a_agent PRIVATE a2a::client a2a::server a2a::core)
```

Configure the downstream project with the SDK install prefix in `CMAKE_PREFIX_PATH`. See `examples/installed_package_consumer/` for a minimal installed-package consumer project.

The exported targets are:

- `a2a::core`
- `a2a::http`
- `a2a::client`
- `a2a::server`
- `a2a::proto_generated`
- `a2a::store_postgres`, when built with `A2A_ENABLE_POSTGRES_STORE=ON`


## Run coverage with thresholds

```bash
python3 -m pip install --upgrade gcovr
./scripts/run_coverage.sh
```

This enforces:
- `src/core` line coverage >= 85%
- `src/client` line coverage >= 80%
- `src/server` line coverage >= 80%

## Run selected examples

```bash
./scripts/run_examples.sh hello_agent streaming_server push_notifications
```

With no arguments, the script runs a small deterministic default set through `examples/fetch_content_consumer/`.