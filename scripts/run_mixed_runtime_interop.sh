#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"${ROOT}/build-mixed-interop"}
PY_BIN=${PY_BIN:-python3}
PORT1=50171
PORT2=50172
SCENARIO=${SCENARIO:-both}

cleanup() {
  [[ -n "${PY_SERVER_PID:-}" ]] && kill "${PY_SERVER_PID}" 2>/dev/null || true
  [[ -n "${CPP_SERVER_PID:-}" ]] && kill "${CPP_SERVER_PID}" 2>/dev/null || true
}
trap cleanup EXIT

cmake -S "${ROOT}" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "${BUILD_DIR}" --parallel --target cpp_grpc_interop_client cpp_grpc_interop_server

"${PY_BIN}" -m grpc_tools.protoc -I "${ROOT}/proto" --python_out="${BUILD_DIR}" --grpc_python_out="${BUILD_DIR}" "${ROOT}/proto/a2a/v1/a2a.proto"
export PYTHONPATH="${BUILD_DIR}:${PYTHONPATH:-}"

if [[ "${SCENARIO}" == "python-server-cpp-client" || "${SCENARIO}" == "both" ]]; then
  "${PY_BIN}" "${ROOT}/scripts/interop/python_grpc_fixture_server.py" --port "${PORT1}" &
  PY_SERVER_PID=$!
  sleep 1
  "${BUILD_DIR}/tests/cpp_grpc_interop_client" "127.0.0.1:${PORT1}"
  kill "${PY_SERVER_PID}" || true
  unset PY_SERVER_PID
fi

if [[ "${SCENARIO}" == "cpp-server-python-client" || "${SCENARIO}" == "both" ]]; then
  "${BUILD_DIR}/tests/cpp_grpc_interop_server" "127.0.0.1:${PORT2}" &
  CPP_SERVER_PID=$!
  sleep 1
  "${PY_BIN}" "${ROOT}/scripts/interop/python_grpc_client_check.py" --endpoint "127.0.0.1:${PORT2}"
  kill "${CPP_SERVER_PID}" || true
  unset CPP_SERVER_PID
fi

echo "[mixed-interop] scenario ${SCENARIO} passed"
