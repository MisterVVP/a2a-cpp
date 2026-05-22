# a2a-cpp: C++20 Agent2Agent (A2A) SDK

[![TCK conformance (main)](https://img.shields.io/github/actions/workflow/status/mistervvp/a2a-cpp/tck.yml?branch=main&label=TCK%20conformance%20%28main%29)](https://github.com/mistervvp/a2a-cpp/actions/workflows/tck.yml?query=branch%3Amain+job%3Amandatory-conformance)

**a2a-cpp** is a modern C++ SDK for building Agent2Agent protocol clients and servers.

It supports core A2A workflows including client/server APIs, discovery, REST/JSON-RPC/gRPC transports, streaming, authentication hooks, and CMake/vcpkg/Conan build integration.

## TCK Compliance Level

[MUST](https://github.com/a2aproject/a2a-tck/blob/1.0-dev/README.md#compatibility-levels) (validated in CI via `--level must --transport grpc,jsonrpc,http_json` in `.github/workflows/tck.yml`).

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
