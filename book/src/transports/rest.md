# REST Transport

REST is the HTTP+JSON transport path for clients and servers.

## Client side

Use `HttpJsonTransport` with `A2AClient`. When libcurl is enabled and found, default outbound buffered HTTP can be used. Otherwise inject an `HttpRequester` implementation.

## Server side

Use `RestServerTransport` to translate inbound framework requests into dispatcher calls.

## Operational considerations

- Enforce TLS and auth policy at your edge or hosting layer.
- Set explicit request deadlines and payload limits.
- Log stable task IDs and request IDs.
- Validate protocol version headers where required.
- Prefer injected requesters for custom retry, proxy, mTLS, or observability policy.
