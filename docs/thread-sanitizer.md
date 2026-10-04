# ThreadSanitizer validation

The ThreadSanitizer (TSan) profile is a focused race-detection check for SDK-owned
concurrency. It uses Clang with `-fsanitize=thread -fno-omit-frame-pointer`, builds
only the selected test executables, and runs tests carrying the `tsan` CTest label.

Run the same profile as CI from the repository root:

```bash
./scripts/run_thread_sanitizer.sh
```

Set `A2A_TSAN_BUILD_DIR` to use a build directory other than `build-tsan`.
Set `A2A_TSAN_CXX_COMPILER` to select a particular Clang executable.
`TSAN_OPTIONS` may also be set by a developer; CI uses the script default,
`halt_on_error=1`.

## Test selection

The profile covers:

- stream-handle lifecycle, cancellation, and cancellation-watcher coordination;
- the shared HTTP client reactor and dispatch executor lifecycle;
- HTTP+JSON and JSON-RPC streaming, including cancellation;
- subscription publication, cancellation, and shutdown;
- in-memory task-store behavior; and
- dispatcher and task-store integration.

The concrete executables are listed in `scripts/run_thread_sanitizer.sh`. Their
discovered tests carry the `tsan` label in `tests/CMakeLists.txt`. To extend the
profile, add the executable to the script's `TARGETS` array and label its
`gtest_discover_tests` call with `LABELS "tsan"`.

PostgreSQL and gRPC transport integration tests are intentionally outside this
focused profile. They add third-party runtime activity without improving coverage
of the synchronization primitives targeted here. The selected HTTP tests retain
libcurl coverage because the shared reactor is one of the principal race-detection
targets.

No TSan suppression file is used. A report in the selected tests is therefore a
failure. If verified third-party noise appears in the future, prefer narrowing the
test selection; any unavoidable suppression must identify the specific external
symbol and document the supporting investigation here.
