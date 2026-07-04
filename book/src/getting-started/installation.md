# Installation and Build

This page reflects the current build surface for the latest documented release.

## Prerequisites

Install these tools before configuring the repository:

- CMake 3.25 or newer.
- A C++20 compiler, tested primarily with GCC and Clang.
- Protobuf with `protoc`.
- gRPC C++ with `grpc_cpp_plugin`.
- `clang-format` and `clang-tidy` for contributor validation.
- Optional: Doxygen for API reference generation.
- Optional: libcurl for built-in outbound HTTP support.
- Optional: PostgreSQL client libraries when building PostgreSQL-backed stores.

## Configure from source

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

Useful options:

| Option | Default | Purpose |
|---|---:|---|
| `A2A_ENABLE_TESTING` | `ON` | Builds unit and integration tests. |
| `A2A_BUILD_EXAMPLES` | `ON` | Keeps the root project compatible with example-related CI messaging; curated examples are built as standalone consumers. |
| `A2A_BUILD_BENCHMARKS` | `OFF` | Builds the optional Google Benchmark suite. |
| `A2A_ENABLE_LIBCURL` | `ON` | Enables default buffered outbound HTTP when `CURL::libcurl` is found. |
| `A2A_ENABLE_POSTGRES_STORE` | `OFF` | Builds PostgreSQL task and push-notification stores. |

## Build and test

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Generate only protobuf outputs when needed:

```bash
cmake --build build --target a2a_proto_codegen
```

Generated headers are written under `build/generated/a2a/v1/` and are installed with the SDK.

## Install CMake package artifacts

```bash
cmake --install build --prefix /tmp/a2a-cpp-install
```

The install tree includes public headers, generated protobuf headers, libraries, and CMake package files under `lib/cmake/a2a_cpp`.

## Contributor validation

For code changes, run the canonical validation script before opening or updating a PR:

```bash
./scripts/verify_changes.sh
```

The script runs the same major local gates as CI: clang-format dry run, configure/build, tests, and clang-tidy.

For documentation-only mdBook changes, build the book:

```bash
mdbook build book
```
