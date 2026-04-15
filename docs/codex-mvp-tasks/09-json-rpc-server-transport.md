# Task 09 — JSON-RPC server transport

## Goal

Expose the same server core over JSON-RPC so a single executor implementation can serve both REST and JSON-RPC clients.

## Scope

- Implement JSON-RPC request parsing and validation.
- Map JSON-RPC method names to dispatcher operations.
- Serialize results and errors into JSON-RPC responses.
- Support the same core A2A operations already implemented on the server side.
- Add integration tests with the JSON-RPC client from Task 06.

## Constraints

- A2A **1.0 only**.
- Reuse the transport-agnostic dispatcher from Task 07.
- Keep JSON-RPC specifics confined to the adapter layer.

## Deliverables

- JSON-RPC server adapter
- envelope parsing / serialization helpers if not already shared
- integration tests with client/server round-trip
- example JSON-RPC server wiring

## Implementation notes

- Validate method presence, ID shape, params shape, and result/error exclusivity.
- Preserve request ID through response generation.
- Centralize method string constants to avoid drift with client transport.
- Reuse ProtoJSON helpers for params/result payloads where possible.

## Test expectations

Cover:
- happy-path send
- invalid JSON-RPC envelope
- missing method
- invalid params
- unknown method
- executor failure mapped to JSON-RPC error

## Acceptance criteria

- A single executor implementation can serve JSON-RPC clients.
- Existing JSON-RPC client can talk to the server in integration tests.
- JSON-RPC errors are standards-compliant and still actionable for SDK users.

## Out of scope

- gRPC transport
- version compatibility with 0.3
