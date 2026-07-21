# Installation and Build

This page reflects the current build surface for the latest documented release.

## Choose an integration method

Use the workflow that matches your application:

- Build the SDK directly from source as described below.
- Use CMake [`FetchContent`](../build/cmake.md#fetchcontent-consumer) and pin a release tag or reviewed commit.
- Install the SDK as a CMake package and consume it with [`find_package`](../build/cmake.md#install-as-a-cmake-package).
- Consume the repository-local [`vcpkg` overlay port](../build/vcpkg.md#consume-a2a-cpp-through-the-repository-overlay-port).

## Prerequisites

Install these tools before configuring the repository:

- CMake 3.25 or newer.
- A C++20 compiler: GCC, Clang, AppleClang, or Visual Studio 2022.
- Protobuf with `protoc`.
- gRPC C++ with `grpc_cpp_plugin`.
- libcurl when using the built-in HTTP, JSON-RPC, or SSE clients.
- `clang-format` and `clang-tidy` for contributor validation.
- Optional: Doxygen for API reference generation.
- Optional: PostgreSQL client libraries when building PostgreSQL-backed stores.

### Debian or Ubuntu

```bash
./scripts/install_build_deps.sh
cmake --version
```

The installer supports Debian and Ubuntu. Confirm that the installed CMake version is 3.25 or newer. Some older distributions, including Ubuntu 22.04, provide an older CMake package and require a newer CMake installation from another source.

The same script also supports Windows when run from Git Bash.

### macOS

```bash
brew install cmake ninja protobuf grpc re2 abseil curl
```

When enabling PostgreSQL-backed stores, also install the PostgreSQL client package:

```bash
brew install libpq
```

### Windows Git Bash

Install Visual Studio 2022 with the **Desktop development with C++** workload and Git for Windows. Open Git Bash in the repository root and run:

```bash
./scripts/install_build_deps.sh
```

The script first checks `VCPKG_ROOT`, then searches for `vcpkg.exe` or `vcpkg` on `PATH`, and then checks `$HOME/vcpkg`. It clones and bootstraps vcpkg under `${VCPKG_ROOT:-$HOME/vcpkg}` only when an existing installation is not found. It installs the dependencies declared by the root `vcpkg.json` and prints the resolved `VCPKG_ROOT`, target triplet, and host triplet when it finishes.

Override `VCPKG_ROOT`, `VCPKG_TARGET_TRIPLET`, or `VCPKG_HOST_TRIPLET` before running the script when needed. Keep the resolved `VCPKG_ROOT` value for the CMake configuration step below; do not reset it to `$HOME/vcpkg` when the script found vcpkg elsewhere.

The root manifest does not install PostgreSQL client libraries. For PostgreSQL-enabled Windows builds, use a vcpkg manifest that includes `libpq`, or consume the SDK through the repository overlay port with its `postgres-store` feature. See [Build with vcpkg](../build/vcpkg.md#enable-postgresql-store-support).

## Configure from source

On Linux:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

On macOS, use the same Homebrew prefixes validated by CI:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_PREFIX_PATH="$(brew --prefix);$(brew --prefix curl)"
```

When `A2A_ENABLE_POSTGRES_STORE=ON`, append `$(brew --prefix libpq)` to `CMAKE_PREFIX_PATH`.

On Windows Git Bash, set `VCPKG_ROOT` to the value printed by `install_build_deps.sh` or to an existing vcpkg checkout:

```bash
export VCPKG_ROOT="/path/to/vcpkg"
export VCPKG_TARGET_TRIPLET="${VCPKG_TARGET_TRIPLET:-x64-windows}"
export VCPKG_HOST_TRIPLET="${VCPKG_HOST_TRIPLET:-$VCPKG_TARGET_TRIPLET}"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET="$VCPKG_TARGET_TRIPLET" \
  -DVCPKG_HOST_TRIPLET="$VCPKG_HOST_TRIPLET"
```

If a build directory was first configured without the vcpkg toolchain, delete it before reconfiguring. CMake caches the toolchain during the first configure.

Useful options:

| Option | Default | Purpose |
|---|---:|---|
| `A2A_ENABLE_TESTING` | `ON` | Builds unit and integration tests. |
| `A2A_BUILD_EXAMPLES` | `ON` | Keeps the root project compatible with example-related CI messaging; curated examples are built as standalone consumers. |
| `A2A_BUILD_BENCHMARKS` | `OFF` | Builds the optional Google Benchmark suite. |
| `A2A_ENABLE_LIBCURL` | `ON` | Enables default outbound HTTP and SSE support when `CURL::libcurl` is found. |
| `A2A_ENABLE_POSTGRES_STORE` | `OFF` | Builds PostgreSQL task and push-notification stores. |

## Build and test

Linux or macOS:

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows Visual Studio generators are multi-configuration; run these commands from Git Bash:

```bash
cmake --build build --config RelWithDebInfo --parallel
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

Generate only protobuf outputs when needed:

```bash
cmake --build build --target a2a_proto_codegen
```

Generated A2A headers are written under `build/generated/a2a/v1/`. Generated Google API annotation headers are written under `build/generated/google/api/`. Both sets are installed with the SDK.

## Install CMake package artifacts

```bash
cmake --install build --prefix /tmp/a2a-cpp-install
```

On a Visual Studio build, also pass `--config RelWithDebInfo`.

The install tree includes public headers, generated protobuf headers, libraries, and CMake package files under `${CMAKE_INSTALL_LIBDIR}/cmake/a2a_cpp`, commonly `lib/cmake/a2a_cpp`.

## Contributor validation

For code changes in a Linux CI-compatible environment, run the canonical validation script before opening or updating a PR:

```bash
./scripts/verify_changes.sh
```

The script runs the same main validation categories as CI: formatting, configure/build, tests, and clang-tidy. It runs `clang-format -i` first and can modify tracked C and C++ files, so review `git diff` after it completes.

The current script assumes a Unix environment with `nproc` and a single-configuration build. On macOS or Windows, use the platform-specific configure, build, and test commands above and run the required formatting and clang-tidy checks separately.

For documentation-only mdBook changes, build the book:

```bash
mdbook build book
```
