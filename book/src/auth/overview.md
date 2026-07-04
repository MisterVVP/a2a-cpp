# Authentication and Authorization Hooks

The SDK provides hooks for carrying credentials and auth metadata. Your application remains responsible for credential storage, verification, authorization decisions, and audit policy.

## Client credential providers

`include/a2a/client/auth.h` includes providers for:

- API key headers.
- Bearer token headers.
- Custom header credentials.
- OAuth2 bearer token extension points.

Use these with transport call options or request construction paths that support metadata injection.

## Server metadata

REST, JSON-RPC, and gRPC server paths populate `RequestContext` with transport metadata. Executor or interceptor code can read this metadata to make authorization decisions.

## Policy guidance

- Never hardcode secrets in source, examples, or tests.
- Validate credentials before trusting request metadata.
- Normalize auth failures so callers receive consistent protocol errors.
- Avoid logging raw credentials.
- Prefer short-lived tokens and managed secret storage.
- Document how TLS or mTLS identity is terminated and propagated into the SDK boundary.

## Example

See `examples/apps/auth_policy_server/main.cpp` for an auth-policy shape.
