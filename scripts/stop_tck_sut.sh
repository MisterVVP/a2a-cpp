#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"${ROOT_DIR}/build-tck"}
SUT_PID_FILE=${SUT_PID_FILE:-"${BUILD_DIR}/tck-sut.pid"}

if [[ ! -f "${SUT_PID_FILE}" ]]; then
  echo "[tck-sut] PID file not found: ${SUT_PID_FILE}. Nothing to stop."
  exit 0
fi

SUT_PID=$(cat "${SUT_PID_FILE}")
if [[ -n "${SUT_PID}" ]] && kill -0 "${SUT_PID}" 2>/dev/null; then
  kill "${SUT_PID}"
  for _ in {1..10}; do
    if ! kill -0 "${SUT_PID}" 2>/dev/null; then
      break
    fi
    sleep 1
  done
  if kill -0 "${SUT_PID}" 2>/dev/null; then
    kill -9 "${SUT_PID}"
  fi
  echo "[tck-sut] Stopped PID ${SUT_PID}."
else
  echo "[tck-sut] PID ${SUT_PID} not running; cleaning stale PID file."
fi

rm -f "${SUT_PID_FILE}"
