# AGENTS.md

## Scope and intent
These instructions apply to the entire repository. Follow them for all changes unless a more specific nested `AGENTS.md` overrides them.
AI agents must follow the [AI-assisted contributions](CONTRIBUTING.md#ai-assisted-contributions) requirements in `CONTRIBUTING.md`.

## C++ conventions reference
- Contributors and AI agents must follow `cpp_conventions.md` for additional C++ conventions and performance tips.
- In case of overlap, apply the stricter rule between this file and `cpp_conventions.md`.

## C++ engineering principles
- Prefer modern C++ (C++20 or newer where available) and standard library facilities before third-party abstractions.
- Keep headers minimal and self-contained; include only what is used.
- Favor clear ownership semantics:
  - Use RAII for resource management.
  - Prefer value semantics by default.
  - Use `std::unique_ptr` for exclusive ownership and `std::shared_ptr` only when shared lifetime is required.
- Make interfaces explicit and hard to misuse:
  - Mark single-argument constructors `explicit`.
  - Use `override` on overridden virtual methods.
  - Use `const` correctness consistently.
  - Prefer `enum class` over unscoped enums.
- Prefer compile-time guarantees:
  - Use `constexpr`, `noexcept`, and strong types where meaningful.
  - Minimize implicit conversions and avoid narrowing.
- Error handling:
  - Validate inputs at boundaries.
  - Return rich error information (status/result types or well-structured exceptions, depending on project convention).
  - Fail fast on programming errors with assertions in debug builds.
- Concurrency:
  - Avoid shared mutable state when possible.
  - Use standard synchronization primitives and document threading contracts.
- API and design:
  - Keep functions focused and small.
  - Prefer composition over inheritance.
  - Separate interface from implementation to reduce coupling.
- Readability and maintainability:
  - Choose descriptive names.
  - Avoid surprising side effects.
  - Leave code cleaner than found.

## General coding guidelines
- Reuse shared protocol helpers across transports when behavior overlaps, for example HTTP header lookup, media-type parsing, version handling, and protocol envelope validation.

## File naming and extensions
- Use `.cpp` for C++ implementation files.
- Use `.h` for headers unless a nested scope explicitly defines `.hpp`.
- Do not introduce `.cc` files for hand-written source; when touching legacy code, prefer migrating `.cc` to `.cpp` (generated protobuf files are exempt).

## Style and quality
- Use project formatter and linter in CI and locally before merging.
- Treat warnings as actionable; keep warning count at zero for touched code.
- Keep diffs small and reviewable.
- Do not suppress linter findings with `NOLINT` unless there is a documented, reviewer-approved justification in code comments and the PR description. Prefer structural refactors that eliminate the warning.
- Document non-obvious decisions with short comments near the code.
- **Magic strings are forbidden** in production and test code. Use named `constexpr` constants (or equivalent strongly typed constants) and centralize protocol literals in shared headers where possible.
- Code must be both readable and efficient: prefer straightforward, maintainable constructs first, then use measured/pre-allocated optimizations for hot paths without obscuring intent.
- Prioritize performance and algorithmic efficiency:
  - Analyze asymptotic complexity for hot paths and prefer lower-complexity algorithms/data structures (`O(1)`/`O(log n)` over repeated linear scans where feasible).
  - Avoid avoidable allocations, copies, and parsing overhead in request/response critical paths.
  - Benchmark or profile non-trivial changes that may impact latency/throughput.
- Plain C-style code is allowed when cross-platform, safe, and measurably more efficient than a higher-level abstraction. Focus on runtime performance over stylistic patterns/idioms when there is a trade-off.
- Test code must satisfy `clang-tidy` readability checks in this repository:
  - Keep each test body and helper function below the cognitive complexity threshold (currently 25).
  - Prefer extracting fixture helpers/builders over large inline lambdas inside `TEST(...)`.
  - Avoid magic numbers in tests; define named `constexpr` constants for status codes, timeouts, IDs, etc.
  - Prefer raw string literals for JSON payload templates instead of heavily escaped JSON strings.

## Testing requirements
- Every behavior change must include tests.
- Add or update **unit tests** for isolated logic.
- Add or update **functional/integration tests** for end-to-end behavior and component interactions.
- Tests must be deterministic and independent.
- Cover happy paths, edge cases, and failure paths.
- Prefer fixing flaky tests over retrying.

## Security and vulnerability hygiene
- Run dependency and vulnerability scanning as part of validation.
- Avoid unsafe APIs and undefined behavior patterns.
- Validate and sanitize all untrusted input.
- Use least privilege for credentials, tokens, and runtime permissions.
- Do not hardcode secrets; use secure configuration and secret management.
- Keep dependencies current and remove unused dependencies.

## Validation checklist for contributors
Before submitting changes:
1. Format the code by running `./scripts/run_clang_format.sh`
2. Build succeeds in a clean environment.
3. Formatter and linter pass.
   - AI agents must run the exact CI formatting mechanism locally before pushing changes:
     ```bash
     mapfile -t CPP_FILES < <(git ls-files '*.h' '*.hpp' '*.c' '*.cpp')
     if [ "${#CPP_FILES[@]}" -gt 0 ]; then
       clang-format --dry-run --Werror "${CPP_FILES[@]}"
     fi
     ```
   - After a successful build/configure, run `./scripts/run_clang_tidy.sh build` and verify it exits with code `0`.
   - Code must not be pushed to any branch unless this linter command succeeds (exit code `0`).
   - Contributors should run `./scripts/verify_changes.sh` as the canonical local validation entrypoint.
   - AI agents must run `./scripts/verify_changes.sh` and must not claim success unless it exits with code `0`.
   - `clang-tidy` passing is not a substitute for `clang-format --dry-run --Werror`; both are required.
4. Unit tests pass.
5. Functional/integration tests pass.
6. Vulnerability/dependency checks pass.
7. Documentation is updated when behavior or interfaces change.
8. Documentation changes that affect mdBook content or structure must verify that `mdbook build book` succeeds locally.

### Documentation-only and README-only exception for AI agents
When a change set is limited to documentation and diagrams (for example files under `docs/**`, `book/**`, `README*`, and other `*.md` content) and does not modify production code, tests, build scripts, or dependency manifests, AI agents may skip checklist items **1 through 5** above.

For these documentation-only/README-only changes, AI agents must validate only:
- 6. Vulnerability/dependency checks pass.
- 7. Documentation is updated when behavior or interfaces change.
- 8. Documentation changes that affect mdBook content or structure verify `mdbook build book` succeeds locally.

## Mandatory performance smoke validation
Run a smoke-sized performance check before opening or updating a PR that touches performance drivers, runner scripts, CI performance workflows, transport clients, streaming/subscription code, or push-notification paths:

```bash
A2A_PERF_TRANSPORTS=grpc,http_json \
A2A_PERF_STORE_BACKENDS=inmemory \
A2A_PERF_REQUESTS=2 \
A2A_PERF_CONCURRENCY=1 \
A2A_PERF_WARMUP_SECONDS=0 \
A2A_PERF_DURATION_SECONDS=0 \
A2A_PERF_REPORT_DIR=perf-artifacts-smoke \
./scripts/run_performance_tests.sh
python3 - <<'PY'
import json
with open('perf-artifacts-smoke/results.json', encoding='utf-8') as results_file:
    errors = sum(int(row.get('errors', 0)) for row in json.load(results_file)['results'])
if errors:
    raise SystemExit(f'performance smoke reported {errors} errors')
PY
```

## Mandatory contributor validation command
Run this command before opening or updating a PR:

```bash
./scripts/verify_changes.sh

Exception: for documentation-only/README-only changes covered by the rule above, AI agents are not required to run `./scripts/verify_changes.sh` and should run only the scoped documentation validation steps.
```

## Mandatory TCK conformance gate
- Contributors and AI agents must run the same TCK flow used in CI locally before committing:
  1. Start SUT: `./scripts/run_tck_sut.sh`
  2. Run TCK mandatory suite via `.github/workflows/tck.yml` equivalent entrypoint (for example the detected script in checked out TCK repo, such as `scripts/run_tck.sh` / `scripts/run_mandatory.sh`).
  3. Stop SUT: `./scripts/stop_tck_sut.sh`
- Do not commit or push code unless TCK mandatory compliance is **100%** (all mandatory requirements passing).
- If TCK tooling or fixtures are unavailable locally, treat that as a blocking issue and resolve environment parity before committing.

## AI agent pre-commit hygiene
AI agents must proactively tidy up touched code before every commit:
- Run clang-format using the repository's required CI-compatible command.
- Run `./scripts/run_clang_tidy.sh build` and fix all reported issues in touched code.
- Re-run `./scripts/verify_changes.sh` after fixes and only commit when it exits with code `0`.

This script enforces the repository quality gates in order:
- format (`clang-format --dry-run --Werror`)
- build (`cmake --build`)
- tests (`ctest --output-on-failure`)
- lint (`./scripts/run_clang_tidy.sh build`)
