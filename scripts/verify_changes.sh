#!/usr/bin/env bash
set -euo pipefail

echo "[verify_changes] Running format checks..."
mapfile -t CPP_FILES < <(git ls-files '*.h' '*.hpp' '*.c' '*.cpp')
if [ "${#CPP_FILES[@]}" -gt 0 ]; then
  clang-format -i "${CPP_FILES[@]}"
  clang-format --dry-run --Werror "${CPP_FILES[@]}"
fi

echo "[verify_changes] Selecting build profile..."

BUILD_DIR="${A2A_VERIFY_BUILD_DIR:-build}"
CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Debug
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  -DA2A_ENABLE_TESTING=ON
)

if [ "${A2A_VERIFY_POSTGRES:-0}" = "1" ]; then
  echo "[verify_changes] PostgreSQL store verification enabled."
  BUILD_DIR="${A2A_VERIFY_BUILD_DIR:-build-postgres}"
  CMAKE_ARGS+=(-DA2A_ENABLE_POSTGRES_STORE=ON)

  if [ -z "${A2A_TEST_POSTGRES_DSN:-}" ]; then
    echo "[verify_changes] ERROR: A2A_TEST_POSTGRES_DSN must be set when A2A_VERIFY_POSTGRES=1." >&2
    exit 1
  fi
else
  echo "[verify_changes] PostgreSQL store verification disabled."
fi

echo "[verify_changes] Configuring CMake in ${BUILD_DIR}..."
cmake -S . -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"

echo "[verify_changes] Building project..."
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "[verify_changes] Running tests..."
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "[verify_changes] Running clang-tidy..."
./scripts/run_clang_tidy.sh --profile split "${BUILD_DIR}"
echo "[verify_changes] All validation checks passed."
