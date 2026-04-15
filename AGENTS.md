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
3. Unit tests pass.
4. Functional/integration tests pass.
5. Vulnerability/dependency checks pass.
6. Documentation is updated when behavior or interfaces change.
