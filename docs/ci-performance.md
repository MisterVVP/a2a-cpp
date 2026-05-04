# CI performance: Windows dependency acceleration

## Baseline bottleneck

The Windows CI path spent most startup time in manifest dependency installation because each run performed a cold clone/bootstrap of vcpkg and rebuilt third-party ports from scratch.

## Changes implemented

1. **vcpkg tool cache (`C:/vcpkg`)**
   - Added `actions/cache@v4` for the vcpkg checkout/bootstrap directory.
   - Cache key is tied to Windows runner plus dependency manifests (`vcpkg.json`, `vcpkg-configuration.json`) to keep invalidation explicit.

2. **Binary package cache (`C:/vcpkg-binary-cache`)**
   - Enabled filesystem binary caching via:
     - `VCPKG_BINARY_SOURCES=clear;files,C:/vcpkg-binary-cache,readwrite`
   - Added `actions/cache@v4` for the binary cache path.
   - Key includes OS + triplet + manifest hashes.
   - Added `restore-keys` fallback so partially matching cache lines can still be used to avoid full cold reinstalls.

3. **Determinism and reproducibility safeguards**
   - Cache keys are derived from manifest files rather than source tree changes.
   - vcpkg still resolves/install from manifest definitions; caching only reuses previously-built artifacts.

4. **Timing observability**
   - Added a stopwatch around dependency installation and exported `dependency_install_seconds`.
   - Added a GitHub Step Summary entry with the measured dependency-install stage duration.

## Expected impact

- Significant reduction in median dependency-install time on non-cold runs (target: 50%+ reduction).
- Lower end-to-end Windows wall-clock CI time while keeping build determinism and required checks intact.

## Recommended follow-ups

- Add artifact upload for raw stage timing JSON for trend analysis over time.
- Add regression threshold checks in workflow (fail/warn on dependency stage drift).
- Consider splitting heavy integration tests into a separate Windows job if compile feedback remains slow.
