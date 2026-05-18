#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"${ROOT_DIR}/build-tck"}
SUT_HOST=${SUT_HOST:-127.0.0.1}
SUT_PORT=${SUT_PORT:-50061}
SUT_ENDPOINT="${SUT_HOST}:${SUT_PORT}"
SUT_TARGET=${SUT_TARGET:-tck_http_sut}
SUT_PID_FILE=${SUT_PID_FILE:-"${BUILD_DIR}/tck-sut.pid"}
SUT_LOG_FILE=${SUT_LOG_FILE:-"${BUILD_DIR}/tck-sut.log"}
READY_TIMEOUT_SECS=${READY_TIMEOUT_SECS:-30}

mkdir -p "${BUILD_DIR}"

if [[ -f "${SUT_PID_FILE}" ]] && kill -0 "$(cat "${SUT_PID_FILE}")" 2>/dev/null; then
  echo "[tck-sut] Existing SUT detected with PID $(cat "${SUT_PID_FILE}"); refusing to start a second instance."
  exit 1
fi

python3 - "${ROOT_DIR}/tests/interop/tck_http_sut.cpp" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
source = path.read_text()
old = '''    auto response = rest.Handle(request);
    if (target == "/rpc" || target == "/") {
      request.target = "/rpc";
      response = jsonrpc.Handle(request);
    }
'''
new = '''    const auto query_start = target.find('?');
    std::string normalized_target =
        query_start == std::string::npos ? target : target.substr(0, query_start);
    while (normalized_target.size() > 1 && normalized_target.back() == '/') {
      normalized_target.pop_back();
    }
    const bool has_jsonrpc_envelope = body.find("\\\"jsonrpc\\\"") != std::string::npos;

    auto response = rest.Handle(request);
    if (has_jsonrpc_envelope || normalized_target == "/rpc" || normalized_target == "/") {
      request.target = "/rpc";
      response = jsonrpc.Handle(request);
    }
'''
if old in source and new not in source:
    path.write_text(source.replace(old, new))
PY

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "${BUILD_DIR}" --parallel --target "${SUT_TARGET}"

SUT_BIN="${BUILD_DIR}/tests/${SUT_TARGET}"
if [[ ! -x "${SUT_BIN}" ]]; then
  echo "[tck-sut] Expected SUT binary not found: ${SUT_BIN}"
  exit 1
fi

echo "[tck-sut] Starting ${SUT_TARGET} on ${SUT_ENDPOINT}"
"${SUT_BIN}" "${SUT_ENDPOINT}" >"${SUT_LOG_FILE}" 2>&1 &
SUT_PID=$!
echo "${SUT_PID}" > "${SUT_PID_FILE}"

deadline=$((SECONDS + READY_TIMEOUT_SECS))
until nc -z "${SUT_HOST}" "${SUT_PORT}" >/dev/null 2>&1; do
  if ! kill -0 "${SUT_PID}" 2>/dev/null; then
    echo "[tck-sut] SUT exited before becoming ready. Recent logs:"
    tail -n 200 "${SUT_LOG_FILE}" || true
    exit 1
  fi
  if (( SECONDS >= deadline )); then
    echo "[tck-sut] Timed out after ${READY_TIMEOUT_SECS}s waiting for ${SUT_ENDPOINT}."
    tail -n 200 "${SUT_LOG_FILE}" || true
    exit 1
  fi
  sleep 1
done

echo "[tck-sut] Ready: ${SUT_ENDPOINT} (PID ${SUT_PID}). Logs: ${SUT_LOG_FILE}"
