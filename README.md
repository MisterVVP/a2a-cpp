# a2a-cpp: C++20 Agent2Agent (A2A) SDK

[![grpc TCK (main)](https://img.shields.io/github/actions/workflow/status/mistervvp/a2a-cpp/tck.yml?branch=main&job=mandatory-conformance%20%28grpc%29&label=grpc%20TCK%20%28main%29)](https://github.com/mistervvp/a2a-cpp/actions/workflows/tck.yml?query=branch%3Amain+job%3A%22mandatory-conformance+%28grpc%29%22)
[![http-json TCK (main)](https://img.shields.io/github/actions/workflow/status/mistervvp/a2a-cpp/tck.yml?branch=main&job=mandatory-conformance%20%28http-json%29&label=http-json%20TCK%20%28main%29)](https://github.com/mistervvp/a2a-cpp/actions/workflows/tck.yml?query=branch%3Amain+job%3A%22mandatory-conformance+%28http-json%29%22)
[![json-rpc TCK (main)](https://img.shields.io/github/actions/workflow/status/mistervvp/a2a-cpp/tck.yml?branch=main&job=mandatory-conformance%20%28jsonrpc%29&label=json-rpc%20TCK%20%28main%29)](https://github.com/mistervvp/a2a-cpp/actions/workflows/tck.yml?query=branch%3Amain+job%3A%22mandatory-conformance+%28jsonrpc%29%22)

**a2a-cpp** is a modern C++ SDK for building Agent2Agent protocol clients and servers.

It supports core A2A workflows including client/server APIs, discovery, REST/JSON-RPC/gRPC transports, streaming, authentication hooks, and CMake/vcpkg/Conan build integration.

## TCK Compliance Level

[MUST](https://github.com/a2aproject/a2a-tck/blob/1.0-dev/README.md#compatibility-levels)

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
