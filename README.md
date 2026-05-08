# a2a-cpp — C++20 SDK for the Agent2Agent (A2A) Protocol

**a2a-cpp** is a modern C++ SDK for building **Agent2Agent (A2A) protocol** clients and servers.

It provides C++20 APIs for:

- A2A client implementations
- A2A server implementations
- Agent discovery with Agent Cards
- REST transport
- JSON-RPC transport
- gRPC/protobuf integration
- streaming/SSE-style A2A flows
- authentication hooks for API keys, bearer tokens, custom headers, and mTLS integration
- CMake, vcpkg, and Conan-based builds

Use this repository if you are looking for:

- C++ A2A SDK
- Agent2Agent C++ SDK
- A2A protocol C++ implementation
- C++ client for Agent2Agent protocol
- C++ server for Agent2Agent protocol
- A2A JSON-RPC C++ library
- A2A REST C++ library

## Repository layout

- `include/` — public headers.
- `src/` — library implementations (`a2a_core`, `a2a_client`, `a2a_server`).
- `proto/` — A2A protocol buffer definitions.
- `build/generated/` — generated protobuf and gRPC C++ sources (build output).
- `tests/` — smoke and unit tests.
- `scripts/` — local automation helpers (for example, clang-tidy runner).
- `docs/` — contributor documentation.

## Quality gates

- CI workflow: `.github/workflows/ci.yml` (format, build, clang-tidy, tests).
- Security scanning: `.github/workflows/codeql.yml`.

See [`docs/build.md`](docs/build.md) for build, lint, and test instructions.
See [`docs/quickstart.md`](docs/quickstart.md) for a first local client/server run.
Client and server API overviews are documented in:

- [`docs/client-usage.md`](docs/client-usage.md)
- [`docs/server-usage.md`](docs/server-usage.md)
- [`docs/streaming-usage.md`](docs/streaming-usage.md)
- [`docs/auth-hooks.md`](docs/auth-hooks.md)

## Authentication hooks

The SDK exposes transport-agnostic auth hooks through `a2a/client/auth.h`:

- `ApiKeyCredentialProvider` injects API keys (default header `X-API-Key`).
- `BearerTokenCredentialProvider` injects `Authorization: Bearer <token>`.
- `CustomHeaderCredentialProvider` injects arbitrary custom auth headers.
- `OAuth2TokenProvider` + `OAuth2BearerCredentialProvider` provide extension points for future OAuth2 helpers without implementing interactive flows in the SDK core.

Per-call auth can be supplied using `CallOptions::credential_provider` and `CallOptions::auth_context`.
For compatibility with existing code, `CallOptions::auth_hook` remains available.

mTLS transport knobs are exposed via `CallOptions::mtls` and plumbed to HTTP request adapters for integration with TLS-capable HTTP backends.

On the server side, REST and JSON-RPC transports populate `RequestContext::auth_metadata` from inbound auth-related headers (including `Authorization`, `X-API-Key`, and forwarded client certificate headers).
