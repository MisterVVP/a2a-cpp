# Build with CMake

`a2a-cpp` is a C++20 SDK built and packaged with CMake. The project can be used directly from source with `FetchContent`, installed into a CMake package prefix, or built with dependencies supplied by vcpkg.

## Requirements

- CMake 3.25 or newer.
- A C++20 compiler.
- Protobuf and gRPC development packages.
- libcurl for the default buffered outbound HTTP implementation.
- Optional: PostgreSQL client libraries when `A2A_ENABLE_POSTGRES_STORE=ON`.

On Ubuntu-like systems, the repository helper installs the dependencies used by CI:

```bash
./scripts/install_build_deps.sh
```

On macOS, install equivalent packages with Homebrew:

```bash
brew install cmake ninja protobuf grpc re2 abseil curl
```

## Configure from source

The default source build enables tests, keeps the curated example apps out of the top-level build, and enables libcurl-backed HTTP support when CMake can find `CURL::libcurl`.

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DA2A_ENABLE_TESTING=ON
```

When dependencies are installed outside standard search paths, pass a CMake prefix path:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="/opt/homebrew;/opt/homebrew/opt/curl"
```

## Build and test

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For the repository's full local code validation flow, run:

```bash
./scripts/verify_changes.sh
```

That script runs the same main gates expected before a code PR: formatting, configure/build, tests, and clang-tidy.

## CMake options

| Option | Default | Description |
| --- | --- | --- |
| `A2A_ENABLE_TESTING` | `ON` | Builds unit and integration tests and enables CTest. |
| `A2A_BUILD_EXAMPLES` | `ON` | Prints guidance for the curated consumer examples. The examples are built from `examples/fetch_content_consumer` or `examples/installed_package_consumer`. |
| `A2A_BUILD_BENCHMARKS` | `OFF` | Builds benchmark targets under `benchmarks/`. |
| `A2A_ENABLE_LIBCURL` | `ON` | Enables the default libcurl-backed outbound HTTP implementation when libcurl is found. Disable it to require injected requesters/fetchers. |
| `A2A_ENABLE_POSTGRES_STORE` | `OFF` | Builds PostgreSQL-backed store targets when PostgreSQL dependencies are available. |

## Generated protobuf headers

The SDK generates A2A protocol C++ sources during the build. Generated headers are written under:

```text
build/generated/a2a/v1/
```

They are installed with the SDK, so downstream projects should include headers from the installed package rather than copying build-tree generated files.

## Install as a CMake package

Install the SDK to a prefix:

```bash
cmake --install build --prefix /tmp/a2a-cpp-install
```

The install tree includes public headers, generated protobuf headers, libraries, and package configuration files under `lib/cmake/a2a_cpp`.

A downstream project can then consume the installed package:

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_a2a_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(a2a_cpp CONFIG REQUIRED)

add_executable(my_a2a_app main.cpp)
target_link_libraries(my_a2a_app PRIVATE a2a::client a2a::server a2a::core)
```

Configure that downstream project with `CMAKE_PREFIX_PATH` pointing at the install prefix:

```bash
cmake -S path/to/app -B build-app \
  -DCMAKE_PREFIX_PATH=/tmp/a2a-cpp-install
cmake --build build-app --parallel
```

## FetchContent consumer

For application projects that prefer source integration, use CMake `FetchContent` and pin `GIT_TAG` to a release tag or reviewed commit:

```cmake
include(FetchContent)

set(A2A_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(A2A_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(A2A_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(A2A_ENABLE_POSTGRES_STORE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  a2a_cpp
  GIT_REPOSITORY https://github.com/MisterVVP/a2a-cpp.git
  GIT_TAG v0.2.0
)
FetchContent_MakeAvailable(a2a_cpp)

target_link_libraries(my_a2a_app PRIVATE a2a::client a2a::server a2a::core)
```

See `examples/fetch_content_consumer/` for a minimal runnable consumer.

## Exported targets

Common exported targets include:

- `a2a::core` for shared core types and utilities.
- `a2a::client` for client APIs.
- `a2a::server` for server APIs.
- `a2a::http` for HTTP support internals used by higher-level targets.
- `a2a::proto_generated` for generated protobuf bindings.
- `a2a::store_postgres` when PostgreSQL store support is enabled.

Most applications should link the smallest set they use. The examples link `a2a::client`, `a2a::server`, and `a2a::core` for a combined client/server sample.

## Build the curated examples

Use the FetchContent example when testing source consumption:

```bash
cmake -S examples/fetch_content_consumer -B build-example \
  -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-example --parallel
./build-example/a2a_example
```

Use the installed-package example when testing package consumption:

```bash
cmake -S examples/installed_package_consumer -B build-installed-example \
  -DCMAKE_PREFIX_PATH=/tmp/a2a-cpp-install \
  -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-installed-example --parallel
./build-installed-example/a2a_example
```

## Platform notes

- Linux CI configures with CMake and validates build, tests, examples, clang-format, clang-tidy, coverage, and selected sanitizer/interop flows.
- macOS CI builds with Homebrew-provided dependencies and Ninja.
- Windows CI uses vcpkg manifest dependencies and the Visual Studio 2022 generator. See [vcpkg](vcpkg.md) for manifest, triplet, and overlay details.
