# Task 11 — Examples, interoperability tests, packaging, and CI

## Goal

Finish the first usable release by adding examples, end-to-end tests, packaging polish, and CI automation.

## Scope

- Add example programs:
  - discovery-only client
  - REST client
  - JSON-RPC client
  - minimal server with custom executor
  - streaming example
- Add end-to-end tests that run client against example server.
- Add interoperability-focused fixtures and golden tests.
- Add CI workflows for build, test, formatting, and generation checks.
- Add initial packaging/install support for downstream consumers.

## Constraints

- A2A **1.0 only**.
- Focus on a solid first release, not feature creep.
- Keep examples minimal and readable.

## Deliverables

- `examples/` programs with README
- end-to-end integration tests
- CI workflow files
- install/export rules in CMake
- `docs/` pages for:
  - build
  - quickstart
  - client usage
  - server usage
  - streaming usage
  - auth hooks

## Implementation notes

- Add at least one golden ProtoJSON fixture test for major message types.
- Ensure examples compile in CI.
- Add sanitizers in at least one CI job if practical.
- Add an install target and exported CMake package if reasonable at this stage.

## Acceptance criteria

- Fresh clone can build, run tests, and execute at least one example client/server flow.
- CI validates formatting, build, and tests.
- README and docs are sufficient for another engineer to start using the SDK.

## Out of scope

- 0.3 compatibility
- gRPC transport unless there is extra capacity after the v1 scope is stable
- advanced release automation beyond what is necessary for a first internal or OSS-quality release
