# Task 02 — Core utilities: versioning, error model, and ProtoJSON helpers

## Goal

Build the core shared layer used by both client and server:
- protocol version helpers
- extension header helpers
- canonical SDK error model
- protobuf JSON encode/decode utilities

## Scope

- Add `a2a::Version` or equivalent helpers for A2A 1.0.
- Add helpers to build/read:
  - `A2A-Version`
  - `A2A-Extensions`
- Define a canonical SDK error hierarchy or error-code system.
- Map transport-neutral protocol failures into SDK errors.
- Add ProtoJSON helper functions:
  - protobuf message -> JSON string
  - JSON string -> protobuf message
- Ensure enum handling follows ProtoJSON expectations.
- Add unit tests for all helpers.

## Constraints

- This task is for **A2A 1.0 only**.
- Use generated protobuf messages as the canonical model.
- Avoid ad hoc JSON parsing for protocol messages beyond what is necessary for headers / envelopes.
- Keep the public error surface transport-agnostic.

## Deliverables

- core version helpers
- extension negotiation helpers
- error types / status codes
- JSON encode/decode helpers for generated messages
- unit tests

## Suggested file layout

```text
include/a2a/core/version.h
include/a2a/core/extensions.h
include/a2a/core/error.h
include/a2a/core/result.h
include/a2a/core/protojson.h
src/core/version.cpp
src/core/extensions.cpp
src/core/error.cpp
src/core/protojson.cpp
tests/unit/core_*.cpp
```

## Implementation notes

- Prefer a `Result<T>` style API or similar predictable status-return convention.
- Preserve useful context in errors:
  - category
  - transport
  - protocol code if available
  - HTTP status if relevant
  - human-readable message
- ProtoJSON helpers should be reusable by REST and JSON-RPC transport code later.
- Add strict tests for enum serialization and missing/unknown field behavior where supported by the chosen protobuf JSON APIs.

## Acceptance criteria

- Can serialize and deserialize a representative A2A message via ProtoJSON.
- `A2A-Version` helper always emits `1.0`.
- Extension header parsing and formatting is deterministic.
- Errors can represent:
  - validation failure
  - unsupported version
  - network failure
  - remote protocol error
  - serialization error

## Out of scope

- Agent Card fetching
- transport clients
- server request handling
