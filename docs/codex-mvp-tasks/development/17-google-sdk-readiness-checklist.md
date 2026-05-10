# Task 17 — Google ecosystem SDK-readiness checklist closure

## Goal

Close remaining gaps against community expectations for production-quality SDKs before requesting inclusion/endorsement.

## Reference criteria

From A2A community guidance and common SDK expectations:
- spec compliance
- package published on standard registry
- documentation
- tests with CI
- Apache 2.0 license
- active maintenance

## Scope

- Spec compliance
  - Add explicit protocol conformance matrix against current A2A specification sections.
  - Add automated conformance tests (golden request/response and negative protocol cases).
- Published package on standard registry
  - Define and implement package publication path (vcpkg + Conan recommended for C++).
  - Add versioning/release policy and signed release artifacts.
- Documentation quality
  - Add API reference generation (Doxygen or Sphinx+Breathe) and publish to GitHub Pages.
  - Add migration/version compatibility page.
- Tests + CI
  - Add multi-platform CI matrix (Linux, macOS, Windows).
  - Add sanitizer jobs (ASan/UBSan) and optional TSan nightly.
  - Add coverage gate integration (depends on Task 15).
- License and governance
  - Verify SPDX headers where required and Apache-2.0 consistency.
  - Add SECURITY.md, support policy, and release cadence notes.
- Active maintenance signals
  - Add issue/PR templates, CODEOWNERS, and labeled roadmap.
  - Add automated stale issue policy with maintainer opt-out labels.

## Deliverables

- `docs/sdk-readiness.md` with checklist and live status.
- CI/release workflow updates.
- Packaging manifests for selected registry targets.
- Governance/repository policy docs.

## Acceptance criteria

- Checklist items are objectively verifiable from the repository.
- Release process can publish installable package artifacts.
- External reviewer can assess compliance and maintenance posture quickly.

## Out of scope

- Guarantee of third-party approval decision.
