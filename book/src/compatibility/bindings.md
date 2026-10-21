# Binding compatibility

HTTP+JSON, JSON-RPC and gRPC expose the same SDK operations through shared
dispatcher and executor interfaces. Applications can attach the adapters to
one dispatcher and shared stores when resources must be visible across bindings.

Independent SDK integration tests exercise all nine ordered binding pairs with
in-memory and PostgreSQL stores: task identity, history/artifacts, messages,
continuation, cancellation, list pages, push configuration, streaming, subscriptions,
capability errors and authenticated access. Generated IDs from independent sends
can differ. Subscription snapshots omit history and artifacts by contract.

Transport error envelopes differ: HTTP authentication failures use 401/403,
gRPC uses `UNAUTHENTICATED`/`PERMISSION_DENIED`, and JSON-RPC protocol errors retain
their prescribed error codes. A shared application security policy determines
caller access on every binding.

SDK integration coverage is separate from upstream TCK coverage and from
[cross-SDK interoperability](interoperability.md). See
[protocol version handling](protocol-versions.md) and [conformance evidence](conformance.md).

To reproduce the shared-binding tests from the repository root:

```bash
cmake -S . -B build -DA2A_ENABLE_TESTING=ON -DA2A_ENABLE_POSTGRES_STORE=ON
cmake --build build --parallel
A2A_TEST_POSTGRES_DSN=postgresql://a2a:a2a@127.0.0.1:5432/a2a \
  ctest --test-dir build -L binding_equivalence --output-on-failure
```

Live scenarios require Python and libcurl. PostgreSQL scenarios explicitly skip
without `A2A_TEST_POSTGRES_DSN`. Each backend uses isolated fixture processes
and PostgreSQL schemas; test identities are not a deployment authorization model.
