# Installation and Build

This page migrates and expands the previous `docs/build.md` guide into mdBook format.

## Prerequisites

You need:

- **CMake 3.25+**
- **C++20 compiler** (Clang or GCC)
- **Protobuf** with `protoc`
- **gRPC C++** with `grpc_cpp_plugin`
- **clang-format**
- **clang-tidy**

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
cmake -S examples/fetch_content_consumer -B build-example -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-example --parallel
./build-example/a2a_example
```

## Generate protobuf code only

```bash
cmake --build build --target a2a_proto_codegen
```

## Run tests

```bash
ctest --test-dir build --output-on-failure
```

## Run formatting checks

Use the CI-compatible formatting check command:

```bash
mapfile -t CPP_FILES < <(git ls-files '*.h' '*.hpp' '*.c' '*.cpp')
if [ "${#CPP_FILES[@]}" -gt 0 ]; then
  clang-format --dry-run --Werror "${CPP_FILES[@]}"
fi
```

## Run clang-tidy

```bash
./scripts/run_clang_tidy.sh build
```

## Canonical local validation

Before opening or updating a PR, run:

```bash
./scripts/verify_changes.sh
```

This runs format, build, tests, and lint in sequence.

## Install package artifacts

```bash
cmake --install build --prefix /tmp/a2a-cpp-install
```

This installs headers, generated protobuf headers, static libraries, and exported CMake package files under `lib/cmake/a2a_cpp`.

## Coverage

```bash
python3 -m pip install --upgrade gcovr
./scripts/run_coverage.sh
```

Coverage thresholds:

- `src/core` line coverage >= 85%
- `src/client` line coverage >= 80%
- `src/server` line coverage >= 80%

## Run all examples

```bash
./scripts/run_examples.sh
```

## Notes on code generation and CI

- Proto definitions are in `proto/a2a/v1/a2a.proto`.
- Generated outputs are written to `build/generated/a2a/v1/`.
- Code generation is wired through `a2a::proto_generated` and runs automatically when required.
- `.github/workflows/ci.yml` validates formatting, configure/build, clang-tidy, and tests.
- `.github/workflows/codeql.yml` runs CodeQL analysis for C/C++ on push, pull requests, and a weekly schedule.
