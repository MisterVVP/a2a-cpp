#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="${1:-${REPO_ROOT}/book-build/api/cpp}"
DOXYFILE_PATH="${REPO_ROOT}/Doxyfile"
DOXYGEN_CONFIG_PATH="${DOXYFILE_PATH}"

if ! command -v doxygen >/dev/null 2>&1; then
  echo "doxygen is required but was not found in PATH" >&2
  exit 1
fi

if ! command -v dot >/dev/null 2>&1; then
  echo "dot (Graphviz) was not found; disabling graph generation for this run"
  DOXYGEN_CONFIG_PATH="$(mktemp)"
  cat "${DOXYFILE_PATH}" > "${DOXYGEN_CONFIG_PATH}"
  {
    echo "HAVE_DOT = NO"
    echo "CLASS_GRAPH = NO"
    echo "COLLABORATION_GRAPH = NO"
    echo "INCLUDE_GRAPH = NO"
    echo "INCLUDED_BY_GRAPH = NO"
    echo "CALL_GRAPH = NO"
    echo "CALLER_GRAPH = NO"
    echo "GRAPHICAL_HIERARCHY = NO"
    echo "DIRECTORY_GRAPH = NO"
  } >> "${DOXYGEN_CONFIG_PATH}"
fi

rm -rf "${REPO_ROOT}/build/doxygen"
mkdir -p "${REPO_ROOT}/build/doxygen"
mkdir -p "${OUTPUT_DIR}"

(
  cd "${REPO_ROOT}"
  doxygen "${DOXYGEN_CONFIG_PATH}"
)

if [ "${DOXYGEN_CONFIG_PATH}" != "${DOXYFILE_PATH}" ]; then
  rm -f "${DOXYGEN_CONFIG_PATH}"
fi

if [ ! -d "${REPO_ROOT}/build/doxygen/html" ]; then
  echo "Doxygen HTML output was not produced at build/doxygen/html" >&2
  exit 1
fi

rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"
cp -R "${REPO_ROOT}/build/doxygen/html/." "${OUTPUT_DIR}/"

echo "Generated API reference at ${OUTPUT_DIR}"
