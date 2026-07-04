# API Reference

The generated C++ API reference is built from public headers under `include/a2a/**`.

## Generated reference

When published with the documentation site, open:

- [C++ API Reference](api/cpp/index.html)

## Generate locally

```bash
./scripts/generate_api_reference.sh
```

The script writes generated pages to `book-build/api/cpp`.

## Public API areas

- `a2a::core`: results, errors, protocol constants, JSON/protobuf helpers, Agent Card support, task state helpers, and versioning.
- `a2a::client`: `A2AClient`, transports, discovery, auth hooks, call options, interceptors, and streaming observers.
- `a2a::server`: executor, dispatcher, transports, interceptors, task stores, task lifecycle helpers, push notifications, and Agent Card serialization.
- `a2a::http`: shared outbound HTTP client abstraction used by default REST/JSON-RPC/discovery/push paths when libcurl is enabled.
