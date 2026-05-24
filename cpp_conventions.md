# C++ Conventions and Tips

This document defines repository-wide C++ implementation conventions for maintainability and performance.

## Core conventions
- Prefer C++20 standard library facilities over third-party wrappers when equivalent.
- Favor value semantics and RAII for lifetime/resource ownership.
- Keep interfaces explicit (`explicit`, `override`, const-correctness, `enum class`).
- Keep functions focused and side-effect minimal.
- Validate external inputs at boundaries and return structured errors.

## Performance-focused guidelines
- Avoid unnecessary allocations/copies in hot paths.
- Prefer pre-sizing (`reserve`) for strings/vectors when estimated size is known.
- Prefer linear-time single-pass transforms over repeated scans.

## String building rule (mandatory)
- **Do not compose multi-part strings with repeated `operator+` chains.**
- Use one of these approaches instead:
  1. `std::string` + `reserve` + `append`/`push_back`
  2. `std::ostringstream` for complex formatting
  3. `absl::StrAppend` (only where Abseil is already part of the target)
- This applies to both production and test code.

## Practical tips
- Use `std::string_view` for read-only string parameters where ownership is not needed.
- Keep headers self-contained and include only what you use.
- Prefer named `constexpr` constants over magic literals.
- Define shared `constexpr` constants in headers (or namespace scope), not inside function bodies, to improve readability and discoverability.
