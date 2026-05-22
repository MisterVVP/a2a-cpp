#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-build}"

if ! command -v clang-tidy >/dev/null 2>&1; then
  echo "clang-tidy is required but not installed." >&2
  exit 1
fi

if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
  echo "Missing ${BUILD_DIR}/compile_commands.json. Configure first:" >&2
  echo "  cmake -S . -B ${BUILD_DIR} -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
  exit 1
fi

mapfile -t TARGET_FILES < <(git ls-files 'src/**/*.cpp' 'tests/**/*.cpp')

if [[ ${#TARGET_FILES[@]} -eq 0 ]]; then
  echo "No C++ files found for clang-tidy."
  exit 0
fi

clang-tidy -p "${BUILD_DIR}" "${TARGET_FILES[@]}"
