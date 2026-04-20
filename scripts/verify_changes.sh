#!/usr/bin/env bash
set -euo pipefail

echo "[verify_changes] Running format checks..."
mapfile -t CPP_FILES < <(git ls-files '*.h' '*.hpp' '*.c' '*.cpp')
if [ "${#CPP_FILES[@]}" -gt 0 ]; then
  clang-format --dry-run --Werror "${CPP_FILES[@]}"
fi

echo "[verify_changes] Building project..."
if [ ! -f build/CMakeCache.txt ] || [ ! -f build/Makefile ]; then
  echo "[verify_changes] Configuring CMake (build files missing)..."
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
fi
cmake --build build -j"$(nproc)"

echo "[verify_changes] Running tests..."
ctest --test-dir build --output-on-failure

echo "[verify_changes] Running clang-tidy..."
./scripts/run_clang_tidy.sh build

echo "[verify_changes] All validation checks passed."
