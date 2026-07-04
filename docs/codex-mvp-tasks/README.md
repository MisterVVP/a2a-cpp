# A2A C++ SDK — Ordered Codex Task Pack

This folder contains an ordered set of implementation tasks for building an **A2A SDK in C++** targeting **A2A protocol 1.0 only**.

## Current use and authority

This folder is an internal planning/archive task pack for agents and maintainers. It is useful for understanding implementation intent, task sequencing, and historical acceptance criteria, but it is **not** the canonical source for current user-facing behavior.

When a task file conflicts with current repository state, prefer these sources in order:

1. Source code, tests, and CI workflows.
2. Current mdBook content under `book/src/**`.
3. Focused current docs under `docs/**` outside this task-pack folder.
4. These task files as historical planning context.

Each task now carries status metadata near the title. Status values mean:

- **Historical / completed**: the task described earlier implementation work and may be superseded by current source or docs.
- **Active follow-up**: the task still describes useful follow-up work or evidence collection.
- **Planned / decision needed**: the task is a future decision or parity item that should be revalidated before implementation.
- **Superseded**: the task contains assumptions known to be obsolete; keep it only for project history unless it is rewritten.

Before using any task as implementation guidance, verify current state with source, tests, workflows, and mdBook docs.

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

1. `development/01-repo-bootstrap-and-codegen.md`
2. `development/02-core-versioning-errors-protojson.md`
3. `development/03-agent-card-discovery.md`
4. `development/04-http-json-client-core-rpcs.md`
5. `development/05-sse-streaming-client.md`
6. `development/06-json-rpc-client.md`
7. `development/07-server-core-executor-dispatcher-task-store.md`
8. `development/08-rest-server-transport.md`
9. `development/09-json-rpc-server-transport.md`
10. `development/10-auth-security-hooks.md`
11. `development/11-examples-interop-and-ci.md`
12. `development/12-parity-gap-analysis-vs-a2a-go.md`
13. `development/13-grpc-transport-parity.md`
14. `development/14-client-server-api-parity-extended-rpcs-and-interceptors.md`
15. `development/15-test-coverage-and-quality-gates.md`
16. `development/16-examples-expansion-and-local-runner-guide.md`
17. `development/17-google-sdk-readiness-checklist.md`
18. `development/18-spec-conformance-and-sdk-application-readiness.md`
19. `development/19-windows-build-dependency-acceleration.md`
20. `development/20-cross-sdk-parity-vs-a2a-python.md`
21. `development/21-python-cross-sdk-interop-ci.md`
22. `development/22-python-cli-workflow-parity.md`

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

## Additional task tracks

- Development tasks: `development/`
- Documentation setup tasks (mdBook + GitHub Pages): `documentation/`
