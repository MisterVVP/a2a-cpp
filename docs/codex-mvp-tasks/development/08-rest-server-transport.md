# Task 08 — REST server transport and Agent Card publishing

## Goal

Expose the server core over HTTP+JSON/REST, including discovery via the A2A 1.0 Agent Card.

## Scope

- Publish `/.well-known/agent-card.json`.
- Implement REST endpoints that map to the server dispatcher.
- Serialize and deserialize using shared ProtoJSON helpers.
- Support non-streaming endpoints first, then add SSE endpoints backed by the executor / task store where appropriate.
- Add example server wiring.

## Constraints

- A2A **1.0 only**.
- Reuse the server dispatcher from Task 07.
- Avoid embedding business logic into HTTP handlers.
- Keep HTTP framework details behind a small adapter layer.

## Deliverables

- REST route adapter
- Agent Card publisher
- request/response mapping
- example REST server
- integration tests against the public `A2AClient`

## Implementation notes

- Always emit `A2A-Version` correctly.
- Keep endpoint path mapping centralized and testable.
- Return sensible HTTP status codes while preserving protocol detail in response bodies where applicable.
- For streaming endpoints, use the SSE implementation strategy from Task 05 where reusable.

## Test expectations

Cover:
- agent card fetch
- send message
- get task
- list tasks
- cancel task
- malformed input
- missing version header if relevant
- streaming happy path if implemented in this task

## Acceptance criteria

- A sample executor can be exposed over REST.
- The existing client can discover and call the example server.
- Agent Card publishing works and advertises the REST interface correctly.

## Out of scope

- JSON-RPC server
- gRPC server
- advanced auth policy
