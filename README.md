# a2a-cpp

C++ SDK for the Agent2Agent (A2A) Protocol.

## Repository layout

- `include/` — public headers.
- `src/` — library implementations (`a2a_core`, `a2a_client`, `a2a_server`).
- `proto/` — A2A protocol buffer definitions.
- `generated/` — generated protobuf and gRPC C++ sources.
- `tests/` — smoke and unit tests.
- `scripts/` — local automation helpers (for example, clang-tidy runner).
- `docs/` — contributor documentation.

## Quality gates

- CI workflow: `.github/workflows/ci.yml` (format, build, clang-tidy, tests).
- Security scanning: `.github/workflows/codeql.yml`.

See [`docs/build.md`](docs/build.md) for build, lint, and test instructions.
