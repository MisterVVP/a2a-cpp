## Summary
- Describe what changed.
- Describe why the change is needed.

## Validation
- [ ] `./scripts/verify_changes.sh` passed locally
- [ ] `clang-format --dry-run --Werror` passed
- [ ] `cmake --build build` passed
- [ ] `ctest --test-dir build --output-on-failure` passed
- [ ] `./scripts/run_clang_tidy.sh build` passed

## Testing Notes
- List unit tests added/updated.
- List integration/functional tests added/updated.
- Note any risk areas or follow-up checks.
