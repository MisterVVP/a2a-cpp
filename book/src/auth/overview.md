# Authentication Overview

Client-side authentication hooks are provided in `include/a2a/client/auth.h`.

## Supported client auth patterns

- **API key auth** via `ApiKeyCredentialProvider`
- **Bearer token auth** via `BearerTokenCredentialProvider`
- **Custom header auth** via `CustomHeaderCredentialProvider`
- **OAuth2 extension point** via `OAuth2BearerCredentialProvider`

## Server-side metadata extraction

Server request context metadata (`RequestContext::auth_metadata`) is populated from inbound auth headers in REST and JSON-RPC transports. Executor logic can use this metadata for authorization and auditing.

## mTLS notes

mTLS is transport/runtime-termination dependent. If your deployment terminates TLS before the SDK boundary, propagate verified client identity metadata safely into request headers/context. If transport-native mTLS is enabled in your stack, ensure certificate validation, trust store rotation, and least-privilege identity mapping are documented and tested.

## Operational guidance

- Do not hardcode secrets in source or test fixtures.
- Rotate keys/tokens and keep credential lifetimes bounded.
- Treat all inbound metadata as untrusted until validated against policy.
- Test auth success and failure paths end-to-end through transport integration tests.

## References

- `tests/integration/rest_server_transport_integration_test.cpp`
- `README.md`
