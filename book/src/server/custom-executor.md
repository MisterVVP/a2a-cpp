# Custom Executor

Implement `a2a::server::AgentExecutor` to provide server-side task and message behavior.

## Design guidance

- Keep executor methods focused and deterministic.
- Validate request inputs at API boundaries.
- Return rich failures (structured status/errors according to project conventions).
- Avoid shared mutable state unless synchronization and threading expectations are explicit.

## Threading and lifecycle

- Define whether your executor is single-threaded or concurrent.
- Ensure referenced resources outlive in-flight requests.
- Use RAII to manage external resources (files, sockets, handles).

## Testing recommendations

- Unit-test executor logic in isolation.
- Add integration tests through REST/JSON-RPC transport paths.
- Cover happy path, validation failures, and cancellation semantics.
