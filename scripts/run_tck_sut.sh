#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"${ROOT_DIR}/build-tck"}
SUT_HOST=${SUT_HOST:-127.0.0.1}
SUT_PORT=${SUT_PORT:-50061}
SUT_ENDPOINT="${SUT_HOST}:${SUT_PORT}"
SUT_TARGET=${SUT_TARGET:-tck_sut}
SUT_STORE_BACKEND=${A2A_TCK_STORE_BACKEND:-inmemory}
SUT_PID_FILE=${SUT_PID_FILE:-"${BUILD_DIR}/tck-sut.pid"}
SUT_LOG_FILE=${SUT_LOG_FILE:-"${BUILD_DIR}/tck-sut.log"}
READY_TIMEOUT_SECS=${READY_TIMEOUT_SECS:-30}

mkdir -p "${BUILD_DIR}"

if [[ -f "${SUT_PID_FILE}" ]] && kill -0 "$(cat "${SUT_PID_FILE}")" 2>/dev/null; then
  echo "[tck-sut] Existing SUT detected with PID $(cat "${SUT_PID_FILE}"); refusing to start a second instance."
  exit 1
fi

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
)

case "${SUT_STORE_BACKEND}" in
  inmemory)
    CMAKE_ARGS+=(
      -DA2A_ENABLE_POSTGRES_STORE=OFF
    )
    ;;
  postgres)
    if [[ -z "${A2A_TCK_POSTGRES_DSN:-}" ]]; then
      echo "[tck-sut] A2A_TCK_POSTGRES_DSN must be set when A2A_TCK_STORE_BACKEND=postgres." >&2
      exit 1
    fi
    CMAKE_ARGS+=(
      -DA2A_ENABLE_POSTGRES_STORE=ON
    )
    ;;
  *)
    echo "[tck-sut] Unsupported A2A_TCK_STORE_BACKEND: ${SUT_STORE_BACKEND}" >&2
    exit 1
    ;;
esac

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja "${CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --parallel --target "${SUT_TARGET}"

SUT_BIN="${BUILD_DIR}/tests/${SUT_TARGET}"
if [[ ! -x "${SUT_BIN}" ]]; then
  echo "[tck-sut] Expected SUT binary not found: ${SUT_BIN}"
  exit 1
fi

echo "[tck-sut] Starting ${SUT_TARGET} with ${SUT_STORE_BACKEND} store on ${SUT_ENDPOINT}"
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
