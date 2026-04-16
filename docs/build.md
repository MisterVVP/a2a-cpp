# Build instructions

## Prerequisites

- CMake 3.25+
- A C++20 toolchain (Clang or GCC)
- Protobuf (with `protoc`)
- gRPC C++ and `grpc_cpp_plugin`

## Configure

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

## Build

```bash
cmake --build build
```

## Run tests

```bash
ctest --test-dir build --output-on-failure
```

## Notes on code generation

- Proto definitions are kept under `proto/a2a/v1/a2a.proto`.
- Generated outputs are written to `generated/a2a/v1/`.
- Generation is wired through the `a2a::proto_generated` target and runs automatically when needed.
