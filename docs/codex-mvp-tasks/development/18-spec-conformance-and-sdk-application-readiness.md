# Task 18 — Spec conformance evidence and SDK application-readiness closure

> **Status:** Active follow-up. Use as readiness or operational planning context, but verify each evidence link against current repository state before claiming completion.

## Goal

Produce objective, auditable evidence that the C++ SDK satisfies community SDK requirements before submitting for official listing/review.

## Community requirement verification status (as of 2026-05-07)

Based on `https://a2a-protocol.org/latest/community/#the-future-is-interoperable` requirements and current repo artifacts:

- ✅ **Spec compliance**: substantial evidence exists (`docs/spec-conformance.md`, protocol tests), but section-by-section requirement traceability still needs hardening.
- ⚠️ **Published package on standard registry**: release workflow and manifests exist, but public registry publication proof for C++ consumers remains incomplete.
- ✅ **Documentation**: core docs/readmes/build docs exist; linking/navigation and explicit protocol coverage summary should be tightened.
- ✅ **Tests with CI**: CI pipeline, coverage, sanitizer workflows are present.
- ✅ **Apache 2.0 license**: repository licensing is in place.
- ⚠️ **Active maintenance**: strong signals exist (templates/CODEOWNERS/security/support), but measurable cadence/SLA evidence should be made explicit.

## Scope

- Spec compliance hardening
  - Build a section-by-section A2A v1.0 conformance matrix mapping spec requirements to source files and tests.
  - Add/maintain negative protocol tests (unsupported method, malformed params, version mismatch, auth requirement mismatch).
  - Add golden cross-SDK interop fixtures (a2a-go ↔ a2a-cpp and a2a-python ↔ a2a-cpp) for core RPCs and streaming events.
- Standard registry publishing readiness
  - Finalize package metadata and publication automation for at least one mainstream C++ registry (vcpkg).
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
