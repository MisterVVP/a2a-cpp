#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SUT_BUILD_DIR=${SUT_BUILD_DIR:-"${ROOT}/build-http-sse-sut"}
CLIENT_BUILD_DIR=${CLIENT_BUILD_DIR:-"${ROOT}/build-http-sse-interop"}
SUT_HOST=${SUT_HOST:-127.0.0.1}
SUT_PORT=${SUT_PORT:-50381}
CLIENT="${CLIENT_BUILD_DIR}/http_sse_interop_client"

cleanup() {
  BUILD_DIR="${SUT_BUILD_DIR}" "${ROOT}/scripts/stop_tck_sut.sh" || true
}
trap cleanup EXIT

if [[ ! -x "${CLIENT}" ]]; then
  echo "Expected interop client not found: ${CLIENT}" >&2
  exit 1
fi

BUILD_DIR="${SUT_BUILD_DIR}" SUT_HOST="${SUT_HOST}" SUT_PORT="${SUT_PORT}" \
  "${ROOT}/scripts/run_tck_sut.sh"

"${CLIENT}" http_json "http://${SUT_HOST}:${SUT_PORT}/a2a"
"${CLIENT}" jsonrpc "http://${SUT_HOST}:${SUT_PORT}/rpc"

echo "[cpp-sut-streaming] production HTTP clients passed against tck_sut"
