# a2a-cpp

C++ SDK for the Agent2Agent (A2A) Protocol.

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
