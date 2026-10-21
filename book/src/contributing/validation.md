# Contributor validation

Run commands from the repository root. Contribution requirements and repository
engineering rules remain in
[CONTRIBUTING.md](https://github.com/MisterVVP/a2a-cpp/blob/main/CONTRIBUTING.md),
[AGENTS.md](https://github.com/MisterVVP/a2a-cpp/blob/main/AGENTS.md), and
[cpp_conventions.md](https://github.com/MisterVVP/a2a-cpp/blob/main/cpp_conventions.md).
Those files define the required checks and scoped exceptions. This chapter
explains how to run the SDK validation tools.

## SDK checks

```bash
./scripts/install_build_deps.sh
./scripts/verify_changes.sh
```

The verifier runs formatting, build, CTest and split clang-tidy profiles.
For PostgreSQL, start a [local database](local-development.md) and set:

```bash
A2A_TEST_POSTGRES_DSN=postgresql://a2a:a2a@127.0.0.1:5432/a2a \
A2A_VERIFY_POSTGRES=1 ./scripts/verify_changes.sh
```

For optional signing, also set `A2A_VERIFY_SIGNING=ON` and install OpenSSL 3.0+
and simdjson 3.0+. Live TLS fixtures additionally require Python, libcurl, nginx
and the OpenSSL CLI; use `./scripts/install_security_test_deps.sh`.
Tests that require an unavailable backend explicitly skip.

## Coverage


```bash
python3 -m pip install --upgrade gcovr
./scripts/run_coverage.sh
```

This enforces:

- `src/core` line coverage >= 85%
- `src/client` line coverage >= 80%
- `src/server` line coverage >= 80%


## Additional profiles

- [ThreadSanitizer](thread-sanitizer.md) targets SDK-owned synchronization.
- [Upstream conformance](../compatibility/conformance.md) is separate from SDK
  tests and preserves skipped/untested requirements.
- [Microbenchmarks](../performance/microbenchmarks.md),
  [workload tests](../performance/workloads.md) and
  [benchmark reports](../performance/reports.md) explain performance evidence.

Documentation-only changes use the scoped rules in `AGENTS.md`. Build the book
with `mdbook build book` and check moved links before publishing.
