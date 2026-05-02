#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-build-coverage}"
REPORT_DIR="${2:-${BUILD_DIR}/coverage}"

cmake -S . -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DA2A_BUILD_EXAMPLES=ON \
  -DCMAKE_CXX_FLAGS='--coverage -O0 -g' \
  -DCMAKE_EXE_LINKER_FLAGS='--coverage' \
  -DCMAKE_SHARED_LINKER_FLAGS='--coverage'

cmake --build "${BUILD_DIR}" --parallel
ctest --test-dir "${BUILD_DIR}" --output-on-failure

mkdir -p "${REPORT_DIR}"

gcovr --root . --object-directory "${BUILD_DIR}" \
  --filter 'src/' \
  --xml-pretty --output "${REPORT_DIR}/coverage.xml" \
  --html-details "${REPORT_DIR}/index.html" \
  --print-summary \
  --fail-under-line 80

gcovr --root . --object-directory "${BUILD_DIR}" \
  --filter 'src/core/' --print-summary --fail-under-line 85

gcovr --root . --object-directory "${BUILD_DIR}" \
  --filter 'src/client/' --print-summary --fail-under-line 80

gcovr --root . --object-directory "${BUILD_DIR}" \
  --filter 'src/server/' --print-summary --fail-under-line 80
