# SDK readiness checklist (Google/community aligned)

## Current status

- ✅ Spec compliance: parity matrix + conformance tests tracked and partially implemented.
- ⚠️ Standard registry packaging: Conan (GitHub Packages) workflow and vcpkg public-registry submission workflow are added; repository secrets and port PR still required.
- ✅ Documentation: build, quickstart, usage guides exist; API doc generation workflow added.
- ✅ Tests with CI: Linux CI plus sanitizer/valgrind/coverage and examples smoke.
- ✅ Apache 2.0 license: repository carries Apache-2.0 license.
- ✅ Active maintenance signals: templates, CODEOWNERS, SECURITY/support policy, roadmap labels automation.

## Evidence map

- Conformance tests: `tests/integration/*` and `tests/fixtures/*`.
- Coverage gate: `.github/workflows/ci.yml` + `scripts/run_coverage.sh`.
- Examples runnable matrix: `examples/README.md` + `scripts/run_examples.sh`.
- Packaging manifests: `conanfile.py`, `vcpkg.json`.
- Packaging workflow docs: `docs/packaging.md`, `.github/workflows/release-packages.yml`.
- Governance docs: `SECURITY.md`, `SUPPORT.md`, `.github/ISSUE_TEMPLATE/*`, `.github/pull_request_template.md`, `.github/CODEOWNERS`.

## Remaining actions before external SDK request

1. Enable release secrets and signing for package publication.
2. Publish generated API docs to GitHub Pages on tags.
3. Add formal protocol-section conformance mapping in docs/spec-conformance.md.
