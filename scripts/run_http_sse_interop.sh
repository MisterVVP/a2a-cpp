#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"${ROOT}/build-http-sse-interop"}
PYTHON=${PYTHON:-python3}
HOST=${HOST:-127.0.0.1}
HTTP_JSON_PORT=${HTTP_JSON_PORT:-50281}
JSON_RPC_PORT=${JSON_RPC_PORT:-50282}
FIXTURE="${ROOT}/scripts/interop/python_http_sse_fixture.py"
CLIENT="${BUILD_DIR}/http_sse_interop_client"
GAP_TEST="${BUILD_DIR}/http_streaming_gap_test"
CONSUMER_SOURCE="${ROOT}/tests/interop/http_sse_consumer"

PIDS=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do
    kill "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
  done
}
trap cleanup EXIT

cmake -S "${CONSUMER_SOURCE}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DA2A_SOURCE_DIR="${ROOT}"
cmake --build "${BUILD_DIR}" --parallel --target http_sse_interop_client http_streaming_gap_test
"${GAP_TEST}"

start_fixture() {
  local port=$1
  "${PYTHON}" "${FIXTURE}" --host "${HOST}" --port "${port}" >"${BUILD_DIR}/fixture-${port}.log" 2>&1 &
  local pid=$!
  PIDS+=("${pid}")
  local deadline=$((SECONDS + 15))
  until nc -z "${HOST}" "${port}" >/dev/null 2>&1; do
    if ! kill -0 "${pid}" 2>/dev/null; then
      cat "${BUILD_DIR}/fixture-${port}.log" >&2 || true
      return 1
    fi
    if ((SECONDS >= deadline)); then
      cat "${BUILD_DIR}/fixture-${port}.log" >&2 || true
      return 1
    fi
    sleep 0.1
  done
}

start_fixture "${HTTP_JSON_PORT}"
"${CLIENT}" http_json "http://${HOST}:${HTTP_JSON_PORT}/a2a"

start_fixture "${JSON_RPC_PORT}"
"${CLIENT}" jsonrpc "http://${HOST}:${JSON_RPC_PORT}/rpc"

echo "[http-sse-interop] focused regression tests and HTTP fixtures passed"
