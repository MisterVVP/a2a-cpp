#!/usr/bin/env bash
set -euo pipefail

TCK_WORKDIR=${TCK_WORKDIR:-tck-repo}
TCK_REPORT_DIR=${TCK_REPORT_DIR:-tck-artifacts/reports}
TCK_LOG_DIR=${TCK_LOG_DIR:-tck-artifacts/logs}
TCK_LOG_FILE=${TCK_LOG_FILE:-"${TCK_LOG_DIR}/tck-run.log"}
SUT_HOST=${SUT_HOST:-127.0.0.1}
SUT_PORT=${SUT_PORT:-50061}
TCK_SUT_ENDPOINT=${TCK_SUT_ENDPOINT:-"${SUT_HOST}:${SUT_PORT}"}
TCK_SUT_URL=${TCK_SUT_URL:-"http://${SUT_HOST}:${SUT_PORT}"}
TCK_RUN_CMD=${TCK_RUN_CMD:-}
TCK_TRANSPORTS=${TCK_TRANSPORTS:-grpc,jsonrpc,http_json}

mkdir -p "${TCK_REPORT_DIR}" "${TCK_LOG_DIR}"

if [[ -n "${TCK_RUN_CMD}" ]]; then
  pushd "${TCK_WORKDIR}" >/dev/null
  bash -lc "${TCK_RUN_CMD}" | tee "../${TCK_LOG_FILE}"
  popd >/dev/null
  exit 0
fi

if [[ -x "${TCK_WORKDIR}/scripts/run_tck.sh" ]]; then
  "${TCK_WORKDIR}/scripts/run_tck.sh" \
    --category mandatory \
    --sut-endpoint "${TCK_SUT_ENDPOINT}" \
    --output-dir "${TCK_REPORT_DIR}" \
    | tee "${TCK_LOG_FILE}"
  exit 0
fi

if [[ -x "${TCK_WORKDIR}/scripts/run_mandatory.sh" ]]; then
  "${TCK_WORKDIR}/scripts/run_mandatory.sh" \
    --sut-endpoint "${TCK_SUT_ENDPOINT}" \
    --output-dir "${TCK_REPORT_DIR}" \
    | tee "${TCK_LOG_FILE}"
  exit 0
fi

if [[ -f "${TCK_WORKDIR}/run_tck.py" ]]; then
  pushd "${TCK_WORKDIR}" >/dev/null
  python3 -m pip install --upgrade pip
  if [[ -f "requirements.txt" ]]; then
    python3 -m pip install -r requirements.txt
  fi
  if [[ -f "pyproject.toml" ]]; then
    python3 -m pip install -e .
  fi
  python3 -m pip install python-dotenv pyopenssl
  if ! python3 -c "import pytest" >/dev/null 2>&1; then
    python3 -m pip install pytest pytest-html
  fi
  python3 run_tck.py \
    --sut-host "${TCK_SUT_URL}" \
    --transport "${TCK_TRANSPORTS}" \
    | tee "../${TCK_LOG_FILE}"
  popd >/dev/null
  exit 0
fi

if [[ -f "${TCK_WORKDIR}/package.json" ]]; then
  pushd "${TCK_WORKDIR}" >/dev/null
  npm ci
  if npm run | grep -q "test:mandatory"; then
    npm run test:mandatory -- --sut-endpoint "${TCK_SUT_ENDPOINT}" --report-dir "../${TCK_REPORT_DIR}" | tee "../${TCK_LOG_FILE}"
  elif npm run | grep -q "mandatory"; then
    npm run mandatory -- --sut-endpoint "${TCK_SUT_ENDPOINT}" --report-dir "../${TCK_REPORT_DIR}" | tee "../${TCK_LOG_FILE}"
  else
    npm test -- --category mandatory --sut-endpoint "${TCK_SUT_ENDPOINT}" --report-dir "../${TCK_REPORT_DIR}" | tee "../${TCK_LOG_FILE}"
  fi
  popd >/dev/null
  exit 0
fi

if [[ -f "${TCK_WORKDIR}/pyproject.toml" || -f "${TCK_WORKDIR}/requirements.txt" ]]; then
  python3 -m pip install --upgrade pip
  if [[ -f "${TCK_WORKDIR}/requirements.txt" ]]; then
    python3 -m pip install -r "${TCK_WORKDIR}/requirements.txt"
  fi
  if [[ -f "${TCK_WORKDIR}/pyproject.toml" ]]; then
    python3 -m pip install -e "${TCK_WORKDIR}"
  fi
  export PYTHONPATH="${PWD}/${TCK_WORKDIR}:${PYTHONPATH:-}"
  export SUT_ENDPOINT="${TCK_SUT_ENDPOINT}"
  export TCK_ENDPOINT="${TCK_SUT_ENDPOINT}"
  export A2A_TCK_SUT_ENDPOINT="${TCK_SUT_ENDPOINT}"
  export TCK_REPORT_DIR_ABS="${PWD}/${TCK_REPORT_DIR}"
  export A2A_TCK_REPORT_DIR="${PWD}/${TCK_REPORT_DIR}"
  pushd "${TCK_WORKDIR}" >/dev/null
  if [[ -d tests/mandatory ]]; then
    python3 -m pytest tests/mandatory --sut-host "http://${SUT_HOST}:${SUT_PORT}" --import-mode=prepend | tee "../${TCK_LOG_FILE}"
  else
    python3 -m pytest tests -m mandatory --sut-host "http://${SUT_HOST}:${SUT_PORT}" --import-mode=prepend --ignore=tests/unit --ignore=tests/unit/adapters | tee "../${TCK_LOG_FILE}"
  fi
  popd >/dev/null
  exit 0
fi

echo "Unable to locate a supported TCK entrypoint in ${TCK_WORKDIR}."
echo "Set repository variable TCK_RUN_CMD to the mandatory-suite command."
find "${TCK_WORKDIR}" -maxdepth 3 -type f | sort | tee "${TCK_LOG_DIR}/tck-workdir-files.txt"
exit 1
