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

## Build examples

```bash
cmake -S . -B build -DA2A_BUILD_EXAMPLES=ON
cmake --build build --target example_rest_client
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

## CI

- `.github/workflows/ci.yml` validates formatting, configure/build, clang-tidy, and tests.
- `.github/workflows/codeql.yml` runs CodeQL analysis for C/C++ on push, pull request, and a weekly schedule.

## Install package

```bash
cmake --install build --prefix /tmp/a2a-cpp-install
```

This installs headers, generated protobuf headers, static libraries, and exported CMake package files under `lib/cmake/a2a_cpp`.


## Run coverage with thresholds

```bash
python3 -m pip install --upgrade gcovr
./scripts/run_coverage.sh
```

This enforces:
- `src/core` line coverage >= 85%
- `src/client` line coverage >= 80%
- `src/server` line coverage >= 80%

## Run all examples

```bash
./scripts/run_examples.sh
```
