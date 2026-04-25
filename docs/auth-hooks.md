# Authentication hooks

Client-side hooks (`include/a2a/client/auth.h`):

- API key headers (`ApiKeyCredentialProvider`)
- Bearer token headers (`BearerTokenCredentialProvider`)
- Custom header hooks (`CustomHeaderCredentialProvider`)
- OAuth2 extension point (`OAuth2BearerCredentialProvider`)

Server-side metadata extraction (`RequestContext::auth_metadata`) is populated from inbound auth headers in REST and JSON-RPC transports.

See `README.md` and `tests/integration/rest_server_transport_integration_test.cpp` for end-to-end auth propagation examples.
