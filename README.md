# a2a-cpp: C++20 Agent2Agent (A2A) SDK

[![TCK conformance (main)](https://img.shields.io/github/actions/workflow/status/mistervvp/a2a-cpp/tck.yml?branch=main&label=TCK%20conformance%20%28main%29)](https://github.com/mistervvp/a2a-cpp/actions/workflows/tck.yml?query=branch%3Amain+job%3Amandatory-conformance)

**a2a-cpp** is a modern C++ SDK for building Agent2Agent protocol clients and servers.

It supports core A2A workflows including client/server APIs, discovery, REST/JSON-RPC/gRPC transports, streaming, authentication hooks, and CMake/vcpkg build integration.

## Use With CMake FetchContent

For application projects, the simplest integration path is to fetch `a2a-cpp` from GitHub and link the exported CMake targets:

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

Pin `GIT_TAG` to a release tag or commit for reproducible builds. See [`examples/fetch_content_consumer/`](examples/fetch_content_consumer/) for a minimal consumer project.

## Use Installed CMake Package

When `a2a-cpp` is installed by CMake or a package manager, downstream projects can consume it with `find_package`:

```cmake
find_package(a2a_cpp CONFIG REQUIRED)

target_link_libraries(my_agent PRIVATE a2a::client a2a::server a2a::core)
```

## TCK Compliance Level

[MUST](https://github.com/a2aproject/a2a-tck/blob/main/README.md#compatibility-levels) (validated in CI via `--level must --transport grpc,jsonrpc,http_json` in `.github/workflows/tck.yml`).

## Documentation

- Documentation website (GitHub Pages): `https://mistervvp.github.io/a2a-cpp/`
- Documentation home source: [`book/src/README.md`](book/src/README.md)
- Project docs and engineering notes: [`docs/`](docs/)
- Build and validation guide: [`docs/build.md`](docs/build.md)

## Repository layout

- `include/` public headers
- `src/` library implementation
- `tests/` unit and integration tests
- `proto/` protocol definitions
- `scripts/` local tooling and CI helpers
- `benchmarks/` optional Google Benchmark performance suite
- `tools/bench_runner/` Go benchmark threshold/report utility
- `examples/` curated consumer apps for FetchContent and installed-package CMake flows

## Performance benchmarks

The SDK includes C++ microbenchmarks for core hot paths. Benchmarks are run in CI with threshold checks to catch performance regressions.

Google Benchmark measures SDK internals directly, including proto/JSON serialization, task store operations, task lifecycle logic, transport routing, server transport handling, UUIDv7 task ID generation, Agent Card generation, and HTTP adapter parsing/serialization. A Go-based benchmark runner parses Google Benchmark JSON output, compares fixed thresholds from `benchmarks/thresholds.json`, and generates CI summaries.

Benchmarks are optional and should be run in Release mode:

```bash
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DA2A_BUILD_BENCHMARKS=ON
cmake --build build-bench --target a2a_benchmarks -j"$(nproc)"
./build-bench/benchmarks/a2a_benchmarks
```

To run the CI-style threshold check locally:

```bash
./build-bench/benchmarks/a2a_benchmarks \
  --benchmark_out=benchmark-results.json \
  --benchmark_out_format=json \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true

go run ./tools/bench_runner/cmd/a2a-bench-runner \
  --results benchmark-results.json \
  --thresholds benchmarks/thresholds.json \
  --summary benchmark-summary.md
```

See [`benchmarks/README.md`](benchmarks/README.md) for threshold strategy, Release-mode guidance, and instructions for adding benchmarks.

## Examples

The [`examples/`](examples/) directory contains curated SDK consumer apps. Build `hello_agent` with `FetchContent`:

```bash
cmake -S examples/fetch_content_consumer -B build-example -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-example --parallel
./build-example/a2a_example
```

Or build the same app source against an installed package with `find_package(a2a_cpp CONFIG REQUIRED)`:

```bash
cmake -S examples/installed_package_consumer -B build-installed-example \
  -DCMAKE_PREFIX_PATH=<install-prefix> \
  -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-installed-example --parallel
./build-installed-example/a2a_example
```