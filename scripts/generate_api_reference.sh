#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="${1:-${REPO_ROOT}/book-build/api/cpp}"
DOXYFILE_PATH="${REPO_ROOT}/Doxyfile"

if ! command -v doxygen >/dev/null 2>&1; then
  echo "doxygen is required but was not found in PATH" >&2
  exit 1
fi

rm -rf "${REPO_ROOT}/build/doxygen"
mkdir -p "${OUTPUT_DIR}"

(
  cd "${REPO_ROOT}"
  doxygen "${DOXYFILE_PATH}"
)

if [ ! -d "${REPO_ROOT}/build/doxygen/html" ]; then
  echo "Doxygen HTML output was not produced at build/doxygen/html" >&2
  exit 1
fi

rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"
cp -R "${REPO_ROOT}/build/doxygen/html/." "${OUTPUT_DIR}/"

echo "Generated API reference at ${OUTPUT_DIR}"
