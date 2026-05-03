# Task 18 — Spec conformance evidence and SDK application-readiness closure

## Goal

Produce objective, auditable evidence that the C++ SDK satisfies community SDK requirements before submitting for official listing/review.

## Scope

- Spec compliance hardening
  - Build a section-by-section A2A v1.0 conformance matrix mapping spec requirements to source files and tests.
  - Add negative protocol tests (unsupported method, malformed params, version mismatch, auth requirement mismatch).
  - Add golden cross-SDK interop fixtures (a2a-go ↔ a2a-cpp) for core RPCs and streaming events.
- Standard registry publishing readiness
  - Finalize package metadata and publication automation for at least one mainstream C++ registry (vcpkg and/or ConanCenter).
  - Add reproducible source archive + signature/provenance generation in release workflow.
  - Validate install + smoke-test flow from published artifacts in CI.
- Documentation completeness
  - Publish API docs and quickstart navigation from README to examples and protocol coverage table.
  - Add compatibility/support matrix: compiler versions, supported OSes, transport support, and known limitations.
- Tests with CI evidence
  - Add required status checks and branch protections for build/test/lint jobs.
  - Add interoperability CI job that runs sample client/server exchange against another SDK implementation.
- Apache 2.0 + governance compliance
  - Verify LICENSE, NOTICE, SPDX header policy, SECURITY.md, and clear vulnerability disclosure path.
  - Add MAINTAINERS/CODEOWNERS and documented release cadence/SLA for issue triage.

## Deliverables

- `docs/sdk-readiness.md` updated with pass/fail checklist and links to verifiable evidence.
- `docs/spec-conformance-matrix.md` with requirement-to-test mapping.
- CI/release workflow updates for registry publish and artifact validation.
- Governance docs (`SECURITY.md`, maintainer policy, support/version policy) if missing.

## Constraints

- Use only publicly verifiable repository artifacts as proof.
- Keep conformance tests deterministic and runnable in CI without proprietary infrastructure.
- Do not mark checklist items as complete without linked evidence.

## Acceptance criteria

- An external reviewer can verify each requirement (spec compliance, registry publishing, docs, tests+CI, Apache-2.0, active maintenance) directly from repository links.
- Release workflow can produce installable artifacts and validate a clean-install smoke test.
- Interop/conformance failures are reported with actionable diagnostics.

## Out of scope

- Guaranteeing approval timing from external maintainers.
