# Examples

This folder provides minimal, in-process examples for the A2A C++ SDK:

- `discovery_only_client.cpp`: fetches and parses an Agent Card.
- `rest_client.cpp`: sends a request to a REST transport backed by a custom executor.
- `json_rpc_client.cpp`: sends a request to a JSON-RPC transport backed by a custom executor.
- `minimal_server_custom_executor.cpp`: shows a minimal server-side setup.
- `streaming_client.cpp`: consumes SSE streaming events through `A2AClient`.

Build examples:

```bash
cmake -S . -B build -DA2A_BUILD_EXAMPLES=ON
cmake --build build --target example_rest_client
```

Run one example:

```bash
./build/examples/example_rest_client
```
