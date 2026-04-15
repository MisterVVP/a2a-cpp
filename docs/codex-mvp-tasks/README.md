# A2A C++ SDK — Ordered Codex Task Pack

This folder contains an ordered set of implementation tasks for building an **A2A SDK in C++** targeting **A2A protocol 1.0 only**.

## Ground rules

- Protocol target: **A2A 1.0**
- Do **not** add 0.3 compatibility in this workstream.
- Prefer **generated protobuf / gRPC types as the canonical model**.
- Avoid creating a parallel handwritten model for protocol messages.
- Keep transport adapters thin and keep business logic transport-agnostic.
- Use **C++20** and **CMake**.
- Keep public APIs stable and minimize exceptions across library boundaries where practical.
- Favor deterministic tests and small examples.

## Suggested execution order

1. `01-repo-bootstrap-and-codegen.md`
2. `02-core-versioning-errors-protojson.md`
3. `03-agent-card-discovery.md`
4. `04-http-json-client-core-rpcs.md`
5. `05-sse-streaming-client.md`
6. `06-json-rpc-client.md`
7. `07-server-core-executor-dispatcher-task-store.md`
8. `08-rest-server-transport.md`
9. `09-json-rpc-server-transport.md`
10. `10-auth-security-hooks.md`
11. `11-examples-interop-and-ci.md`

## Recommended repository layout

```text
a2a-cpp/
  CMakeLists.txt
  cmake/
  third_party/
  proto/
  generated/
  include/
    a2a/
  src/
    core/
    discovery/
    client/
      http_json/
      jsonrpc/
      sse/
    server/
      rest/
      jsonrpc/
    auth/
    util/
  tests/
    unit/
    integration/
    fixtures/
  examples/
```

## Definition of done for the whole project

- Discovery works against a valid A2A 1.0 Agent Card.
- A client can send messages over REST and JSON-RPC.
- Streaming works via SSE for `SendStreamingMessage` and task subscription.
- A server runtime can expose REST and JSON-RPC endpoints using a shared executor.
- Examples build and run.
- CI builds, runs tests, and validates formatting / linting.

## Notes for Codex

Each task file includes:
- goal
- scope
- deliverables
- constraints
- implementation notes
- acceptance criteria
- explicit out-of-scope items

Work in order unless a task clearly states it can be parallelized.
