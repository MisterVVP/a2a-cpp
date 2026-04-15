# A2A C++ SDK 1.0 — Backlog overview

## Objective

Build a production-usable **A2A 1.0** SDK in **C++20** with:

- discovery via Agent Card
- REST client
- JSON-RPC client
- SSE streaming client
- transport-agnostic server runtime
- REST server
- JSON-RPC server
- auth hooks
- examples and CI

## Excluded for this workstream

- A2A 0.3 compatibility
- speculative multi-version negotiation beyond 1.0
- full OAuth2 product flows
- advanced persistence layer
- gRPC transport unless explicitly added later

## Backlog order

| Order | Task | Depends on |
|---|---|---|
| 01 | Repo bootstrap and proto codegen | none |
| 02 | Core versioning, errors, ProtoJSON | 01 |
| 03 | Agent Card discovery | 02 |
| 04 | HTTP+JSON client | 02, 03 |
| 05 | SSE streaming client | 04 |
| 06 | JSON-RPC client | 02, 03 |
| 07 | Server core and task store | 02 |
| 08 | REST server transport | 03, 04, 07 |
| 09 | JSON-RPC server transport | 06, 07 |
| 10 | Auth and security hooks | 03, 04, 06, 07 |
| 11 | Examples, interop, packaging, CI | all major tasks |

## Definition of done per task

Each task should produce:
- implementation
- tests
- docs or usage notes
- no unrelated refactors
- clear follow-up notes if blockers remain
