# AGENTS.md

## Scope and intent
These instructions apply to the entire repository. Follow them for all changes unless a more specific nested `AGENTS.md` overrides them.

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

## File naming and extensions
- Use `.cpp` for C++ implementation files.
- Use `.h` for headers unless a nested scope explicitly defines `.hpp`.
- Do not introduce `.cc` files for hand-written source; when touching legacy code, prefer migrating `.cc` to `.cpp` (generated protobuf files are exempt).

## Style and quality
- Use project formatter and linter in CI and locally before merging.
- Treat warnings as actionable; keep warning count at zero for touched code.
- Keep diffs small and reviewable.
- Document non-obvious decisions with short comments near the code.

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
1. Build succeeds in a clean environment.
2. Formatter and linter pass.
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
3. Unit tests pass.
4. Functional/integration tests pass.
5. Vulnerability/dependency checks pass.
6. Documentation is updated when behavior or interfaces change.

## Mandatory contributor validation command
Run this command before opening or updating a PR:

```bash
./scripts/verify_changes.sh
```

This script enforces the repository quality gates in order:
- format (`clang-format --dry-run --Werror`)
- build (`cmake --build`)
- tests (`ctest --output-on-failure`)
- lint (`./scripts/run_clang_tidy.sh build`)
