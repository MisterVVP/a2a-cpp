# Task 06 — JSON-RPC client transport

## Goal

Implement JSON-RPC support for the same high-level A2A client API already exposed by the REST transport.

## Scope

- Add JSON-RPC envelope serialization / parsing.
- Map A2A client methods to JSON-RPC method calls.
- Support at least:
  - `SendMessage`
  - `GetTask`
  - `ListTasks`
  - `CancelTask`
  - optional push config methods if present in chosen 1.0 surface
- Reuse generated protobuf request/response types.
- Reuse shared error handling and version header helpers.
- Add tests for JSON-RPC protocol failures.

## Constraints

- A2A **1.0 only**.
- Do not duplicate business logic already present in the high-level client.
- Keep request ID handling strict and testable.

## Deliverables

- `JsonRpcTransport`
- envelope types / helpers
- mapping from JSON-RPC error objects to SDK errors
- unit and integration tests

## Implementation notes

- Ensure request IDs are unique and validated on response.
- Support HTTP transport carrying JSON-RPC payloads if that is the selected binding.
- Keep headers such as `A2A-Version` consistent with the rest of the SDK.
- Centralize method-name constants.
- Support batched JSON-RPC only if required by the protocol surface; otherwise skip it for now.

## Test expectations

Cover:
- happy-path send
- request/response ID mismatch
- remote JSON-RPC error object
- malformed envelope
- invalid result payload
- timeout / network failure
- unsupported method

## Acceptance criteria

- Existing high-level client can switch between REST and JSON-RPC transport without changing request/response types.
- JSON-RPC errors map cleanly into SDK errors.
- Response ID mismatches are detected and surfaced.

## Out of scope

- server-side JSON-RPC dispatcher
- gRPC transport
