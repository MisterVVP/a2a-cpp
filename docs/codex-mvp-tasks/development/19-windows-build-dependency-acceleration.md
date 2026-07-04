# Task 19 — Windows dependency-install acceleration and CI latency reduction

> **Status:** Active follow-up. Use as readiness or operational planning context, but verify each evidence link against current repository state before claiming completion.

## Goal

Reduce Windows CI build startup time (currently dominated by manifest dependency installation) to improve developer feedback loops and release velocity.

## Scope

- Baseline and profiling
  - Measure current Windows job timings broken down by checkout, tool bootstrap, dependency resolution, configure, build, and test stages.
  - Capture dependency graph and identify the highest-latency packages.
- Caching and artifact reuse
  - Add robust cache keys for package manager artifacts (vcpkg cache, build tree, compiler cache where applicable).
  - Separate cache keys for lockfile/manifests vs source changes to maximize hit rate.
  - Add fallback behavior that avoids full cold reinstalls when partial cache is valid.
- Build graph optimization
  - Audit and disable unneeded optional dependencies/features for default CI profile.
  - Split heavyweight integration tests into separate jobs so compile verification returns faster.
  - Enable parallel package builds and compiler parallelism with controlled limits.
- Toolchain and workflow optimization
  - Pin faster mirrors/registries where policy allows and verify integrity.
  - Reuse prebuilt binary artifacts for third-party dependencies when reproducibility and trust constraints are met.
  - Evaluate self-hosted Windows runners or larger hosted runners if cost/perf tradeoff is favorable.
- Observability and regression prevention
  - Publish per-stage timing summary as CI artifact/comment.
  - Add alert threshold when dependency install stage regresses beyond target.

## Deliverables

- Updated Windows workflow(s) under `.github/workflows/` with optimized caching and job structure.
- `docs/ci-performance.md` documenting baseline, changes, and measured improvements.
- Optional helper scripts for timing collection and cache diagnostics.

## Constraints

- Preserve deterministic, secure dependency resolution and reproducible builds.
- Do not trade correctness for speed; all required checks must still pass.
- Keep cache invalidation explicit and easy to reason about.

## Acceptance criteria

- Median Windows dependency-install stage time is reduced by at least 50% from baseline.
- End-to-end Windows CI wall-clock time is materially reduced with no increase in flaky failures.
- Timing artifacts clearly show where improvements came from.

## Out of scope

- Removing Windows CI coverage.
