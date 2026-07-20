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
REST_ENDPOINT="http://${HOST}:${HTTP_JSON_PORT}/a2a"
"${CLIENT}" http_json send "${REST_ENDPOINT}"
"${CLIENT}" http_json subscribe "${REST_ENDPOINT}"
"${CLIENT}" http_json cancel "${REST_ENDPOINT}"

start_fixture "${JSON_RPC_PORT}"
JSON_RPC_ENDPOINT="http://${HOST}:${JSON_RPC_PORT}/rpc"
"${CLIENT}" jsonrpc send "${JSON_RPC_ENDPOINT}"
"${CLIENT}" jsonrpc subscribe "${JSON_RPC_ENDPOINT}"
"${CLIENT}" jsonrpc cancel "${JSON_RPC_ENDPOINT}"

expect_json_rpc_rejected() {
  local name=$1
  local payload=$2
  local status
  status=$("${PYTHON}" - "${JSON_RPC_ENDPOINT}" "${payload}" <<'PY'
import sys
import urllib.error
import urllib.request

endpoint = sys.argv[1]
payload = sys.argv[2].encode("utf-8")
request = urllib.request.Request(
    endpoint,
    data=payload,
    headers={
        "Accept": "text/event-stream",
        "A2A-Version": "1.0",
        "Content-Type": "application/json",
    },
    method="POST",
)
try:
    with urllib.request.urlopen(request, timeout=5) as response:
        print(response.status)
except urllib.error.HTTPError as error:
    print(error.code)
PY
)
  if [[ "${status}" != "400" ]]; then
    echo "[http-sse-interop] expected ${name} to be rejected with HTTP 400, got ${status}" >&2
    return 1
  fi
}

expect_json_rpc_rejected "unexpected method" \
  '{"jsonrpc":"2.0","id":"bad-method","method":"a2a.wrong","params":{"message":{"messageId":"m-1"}}}'
expect_json_rpc_rejected "send missing messageId" \
  '{"jsonrpc":"2.0","id":"bad-send","method":"a2a.sendStreamingMessage","params":{"message":{}}}'
expect_json_rpc_rejected "subscribe missing id" \
  '{"jsonrpc":"2.0","id":"bad-subscribe","method":"a2a.subscribeToTask","params":{}}'

echo "[http-sse-interop] focused regression tests and six HTTP fixture scenarios passed"
