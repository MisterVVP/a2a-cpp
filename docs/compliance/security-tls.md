# TLS and server identity

HTTP+JSON and JSON-RPC clients accept `CallOptions::tls`. The shared HTTP client
accepts `Request::tls`, and discovery accepts TLS options through
`MakeDefaultHttpFetcher`. These options configure a local CA bundle without
changing system trust or disabling peer/hostname verification.

```cpp
a2a::client::CallOptions options;
options.tls = a2a::http::TlsOptions{.ca_bundle_path = ca_bundle_path,
                                     .require_tls13 = true};
auto task = client.GetTask(request, options);
```

TLS options propagate through unary and streaming requests in both HTTP
bindings. libcurl verifies certificates and hostnames. The ordinary default
requires TLS 1.2 or later; an explicit TLS configuration recommends TLS 1.3 by
default. Set `require_tls13=false` for TLS 1.2 deployments. HTTP URLs are still
available for local fixtures; production applications must configure HTTPS
endpoints and enforce their deployment's encrypted ingress.

gRPC uses the established `grpc::SslCredentials` and
`grpc::SslServerCredentials` APIs. Supply trusted PEM roots when constructing
the channel and server certificate/key pairs when configuring the
`grpc::ServerBuilder`. The SDK takes that configured channel; it does not
substitute insecure credentials. Avoid target-name overrides in production.

The SDK HTTP server adapters implement protocol dispatch and operate on an
application-owned byte transport. The supported deployment path demonstrated
here uses **nginx TLS termination** with TLS 1.3, a loopback upstream, and response
buffering disabled for SSE. The SUT contains no TLS server implementation.
Production deployments must protect the path from the proxy to the adapter:
loopback on the same host, or an independently secured connection/network.

The SUT accepts `A2A_TCK_TLS_CERT_FILE` and `A2A_TCK_TLS_KEY_FILE` for native gRPC
TLS and `A2A_TCK_PUBLIC_HTTPS_BASE` for the externally terminated HTTP URLs in its
Agent Card. Configure both certificate/key variables together. The HTTP fixture
listener remains loopback in secure scenarios and cannot receive remote
plaintext traffic.

The [SDK security launcher](../../tests/scripts/run_security_scenarios.py)
generates an isolated CA, an unrelated CA and localhost server certificates with
fixed validity windows. nginx provides ordinary TLS termination; gRPC uses its
native server credentials. C++ SDK clients verify trusted certificates succeed
and untrusted, expired and hostname-mismatched certificates fail on every
binding. The same launcher also runs a gRPC TLS server constructed directly in
`SecurityTransportIntegrationTest`, independently of the SUT. Tests never disable certificate validation or use external services.

Requirement mapping: `AUTH-TLS-001/002`, `AUTH-SERVER-001`, `GRPC-SVC-003`.
