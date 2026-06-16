#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
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
TCK_SOURCE_REPORT_DIR=${TCK_SOURCE_REPORT_DIR:-"${TCK_WORKDIR}/reports"}
TCK_WORKDIR_ABS=$(cd "${ROOT_DIR}" && mkdir -p "${TCK_WORKDIR}" && cd "${TCK_WORKDIR}" && pwd)
TCK_REPORT_DIR_ABS=$(cd "${ROOT_DIR}" && mkdir -p "${TCK_REPORT_DIR}" && cd "${TCK_REPORT_DIR}" && pwd)
TCK_LOG_DIR_ABS=$(cd "${ROOT_DIR}" && mkdir -p "${TCK_LOG_DIR}" && cd "${TCK_LOG_DIR}" && pwd)
TCK_SOURCE_REPORT_DIR_ABS=$(cd "${ROOT_DIR}" && mkdir -p "${TCK_SOURCE_REPORT_DIR}" && cd "${TCK_SOURCE_REPORT_DIR}" && pwd)
TCK_LOG_FILE_ABS="${TCK_LOG_DIR_ABS}/$(basename "${TCK_LOG_FILE}")"

mkdir -p "${TCK_REPORT_DIR_ABS}" "${TCK_LOG_DIR_ABS}"

copy_tck_reports() {
  local source_dir=$1
  if [[ ! -d "${source_dir}" ]]; then
    return 0
  fi
  local report
  for report in compatibility.json compatibility.html tck_report.html junitreport.xml; do
    if [[ -f "${source_dir}/${report}" ]]; then
      cp "${source_dir}/${report}" "${TCK_REPORT_DIR_ABS}/${report}"
    fi
  done
}

run_and_collect_reports() {
  set +e
  "$@"
  local status=$?
  set -e
  copy_tck_reports "${TCK_SOURCE_REPORT_DIR_ABS}"
  copy_tck_reports "${TCK_REPORT_DIR_ABS}"
  return "${status}"
}

if [[ -n "${TCK_RUN_CMD}" ]]; then
  pushd "${TCK_WORKDIR_ABS}" >/dev/null
  set +e
  bash -lc "${TCK_RUN_CMD}" | tee "${TCK_LOG_FILE_ABS}"
  status=${PIPESTATUS[0]}
  set -e
  popd >/dev/null
  copy_tck_reports "${TCK_SOURCE_REPORT_DIR_ABS}"
  exit "${status}"
fi

if [[ -x "${TCK_WORKDIR_ABS}/scripts/run_tck.sh" ]]; then
  run_and_collect_reports "${TCK_WORKDIR_ABS}/scripts/run_tck.sh" \
    --category mandatory \
    --sut-endpoint "${TCK_SUT_ENDPOINT}" \
    --output-dir "${TCK_REPORT_DIR_ABS}" \
    | tee "${TCK_LOG_FILE_ABS}"
  exit "${PIPESTATUS[0]}"
fi

if [[ -x "${TCK_WORKDIR_ABS}/scripts/run_mandatory.sh" ]]; then
  run_and_collect_reports "${TCK_WORKDIR_ABS}/scripts/run_mandatory.sh" \
    --sut-endpoint "${TCK_SUT_ENDPOINT}" \
    --output-dir "${TCK_REPORT_DIR_ABS}" \
    | tee "${TCK_LOG_FILE_ABS}"
  exit "${PIPESTATUS[0]}"
fi

if [[ -f "${TCK_WORKDIR_ABS}/run_tck.py" ]]; then
  pushd "${TCK_WORKDIR_ABS}" >/dev/null
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
  run_and_collect_reports python3 run_tck.py \
    --sut-host "${TCK_SUT_URL}" \
    --transport "${TCK_TRANSPORTS}" \
    | tee "${TCK_LOG_FILE_ABS}"
  status=${PIPESTATUS[0]}
  popd >/dev/null
  exit "${status}"
fi

if [[ -f "${TCK_WORKDIR_ABS}/package.json" ]]; then
  pushd "${TCK_WORKDIR_ABS}" >/dev/null
  npm ci
  if npm run | grep -q "test:mandatory"; then
    run_and_collect_reports npm run test:mandatory -- --sut-endpoint "${TCK_SUT_ENDPOINT}" \
      --report-dir "${TCK_REPORT_DIR_ABS}" |
      tee "${TCK_LOG_FILE_ABS}"
    status=${PIPESTATUS[0]}
  elif npm run | grep -q "mandatory"; then
    run_and_collect_reports npm run mandatory -- --sut-endpoint "${TCK_SUT_ENDPOINT}" --report-dir "${TCK_REPORT_DIR_ABS}" |
      tee "${TCK_LOG_FILE_ABS}"
    status=${PIPESTATUS[0]}
  else
    run_and_collect_reports npm test -- --category mandatory --sut-endpoint "${TCK_SUT_ENDPOINT}" \
      --report-dir "${TCK_REPORT_DIR_ABS}" |
      tee "${TCK_LOG_FILE_ABS}"
    status=${PIPESTATUS[0]}
  fi
  popd >/dev/null
  exit "${status}"
fi

if [[ -f "${TCK_WORKDIR_ABS}/pyproject.toml" || -f "${TCK_WORKDIR_ABS}/requirements.txt" ]]; then
  python3 -m pip install --upgrade pip
  if [[ -f "${TCK_WORKDIR_ABS}/requirements.txt" ]]; then
    python3 -m pip install -r "${TCK_WORKDIR_ABS}/requirements.txt"
  fi
  if [[ -f "${TCK_WORKDIR_ABS}/pyproject.toml" ]]; then
    python3 -m pip install -e "${TCK_WORKDIR_ABS}"
  fi
  export PYTHONPATH="${TCK_WORKDIR_ABS}:${PYTHONPATH:-}"
  export SUT_ENDPOINT="${TCK_SUT_ENDPOINT}"
  export TCK_ENDPOINT="${TCK_SUT_ENDPOINT}"
  export A2A_TCK_SUT_ENDPOINT="${TCK_SUT_ENDPOINT}"
  export TCK_REPORT_DIR_ABS
  export A2A_TCK_REPORT_DIR="${TCK_REPORT_DIR_ABS}"
  pushd "${TCK_WORKDIR_ABS}" >/dev/null
  if [[ -d tests/mandatory ]]; then
    run_and_collect_reports python3 -m pytest tests/mandatory --sut-host "http://${SUT_HOST}:${SUT_PORT}" --import-mode=prepend |
      tee "${TCK_LOG_FILE_ABS}"
    status=${PIPESTATUS[0]}
  else
    run_and_collect_reports python3 -m pytest tests -m mandatory --sut-host "http://${SUT_HOST}:${SUT_PORT}" \
      --import-mode=prepend --ignore=tests/unit --ignore=tests/unit/adapters |
      tee "${TCK_LOG_FILE_ABS}"
    status=${PIPESTATUS[0]}
  fi
  popd >/dev/null
  exit "${status}"
fi

echo "Unable to locate a supported TCK entrypoint in ${TCK_WORKDIR}."
echo "Set repository variable TCK_RUN_CMD to the mandatory-suite command."
find "${TCK_WORKDIR}" -maxdepth 3 -type f | sort | tee "${TCK_LOG_DIR}/tck-workdir-files.txt"
exit 1
