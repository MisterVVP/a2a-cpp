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
BUILD_DIR=${BUILD_DIR:-"${ROOT_DIR}/build-tck"}
TCK_TRANSPORTS=${TCK_TRANSPORTS:-grpc,jsonrpc,http_json}
TCK_SOURCE_REPORT_DIR=${TCK_SOURCE_REPORT_DIR:-"${TCK_WORKDIR}/reports"}
TCK_REQUIRED_EXTENSIONS=${TCK_REQUIRED_EXTENSIONS:-${A2A_TCK_REQUIRED_EXTENSIONS:-}}
TCK_WORKDIR_ABS=$(cd "${ROOT_DIR}" && mkdir -p "${TCK_WORKDIR}" && cd "${TCK_WORKDIR}" && pwd)
TCK_REPORT_DIR_ABS=$(cd "${ROOT_DIR}" && mkdir -p "${TCK_REPORT_DIR}" && cd "${TCK_REPORT_DIR}" && pwd)
TCK_LOG_DIR_ABS=$(cd "${ROOT_DIR}" && mkdir -p "${TCK_LOG_DIR}" && cd "${TCK_LOG_DIR}" && pwd)
TCK_SOURCE_REPORT_DIR_ABS=$(cd "${ROOT_DIR}" && mkdir -p "${TCK_SOURCE_REPORT_DIR}" && cd "${TCK_SOURCE_REPORT_DIR}" && pwd)
TCK_LOG_FILE_ABS="${TCK_LOG_DIR_ABS}/$(basename "${TCK_LOG_FILE}")"
TCK_PYTHONPATH_DIR="${TCK_LOG_DIR_ABS}/pythonpath"

mkdir -p "${TCK_REPORT_DIR_ABS}" "${TCK_LOG_DIR_ABS}"

# Add executable SDK coverage where upstream only registers requirements.
# No upstream tests or assertions are replaced. Older/custom entrypoints must
# still produce the required scenario evidence or the report gate fails.
SDK_TEST_MODULE="${TCK_WORKDIR_ABS}/tests/compatibility/test_cpp_sdk_requirements.py"
if [[ ! -d "$(dirname "${SDK_TEST_MODULE}")" ]]; then
  echo "The selected TCK must expose tests/compatibility for SDK requirement coverage." >&2
  exit 1
fi
if [[ -e "${SDK_TEST_MODULE}" ]]; then
  echo "Refusing to overwrite existing TCK module: ${SDK_TEST_MODULE}" >&2
  exit 1
fi
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DA2A_ENABLE_TESTING=ON -DA2A_ENABLE_LIBCURL=ON
cmake --build "${BUILD_DIR}" --parallel --target shared_binding_test protocol_version_integration_test
cp "${ROOT_DIR}/tests/tck/test_cpp_sdk_requirements.py" "${SDK_TEST_MODULE}"
trap 'rm -f "${SDK_TEST_MODULE}"' EXIT
export A2A_CPP_SDK_ROOT="${ROOT_DIR}"
export A2A_CPP_SDK_BUILD
A2A_CPP_SDK_BUILD=$(realpath "${BUILD_DIR}")
export A2A_CPP_SDK_ARTIFACTS="${TCK_REPORT_DIR_ABS}/sdk-scenarios"
export A2A_TCK_STORE_BACKEND=${A2A_TCK_STORE_BACKEND:-inmemory}

# A custom command must not accidentally pass using a report from an older run.
for report in compatibility.json compatibility.html tck_report.html junitreport.xml; do
  rm -f "${TCK_SOURCE_REPORT_DIR_ABS}/${report}" "${TCK_REPORT_DIR_ABS}/${report}"
done

check_sdk_requirements() {
  python3 "${ROOT_DIR}/scripts/check_tck_sdk_requirements.py" "${TCK_REPORT_DIR_ABS}/compatibility.json"
}

install_tck_python_overrides() {
  if [[ -z "${TCK_REQUIRED_EXTENSIONS}" ]]; then
    return 0
  fi

  local override_source="${ROOT_DIR}/scripts/tck_python_overrides/sitecustomize.py"
  if [[ ! -f "${override_source}" ]]; then
    echo "Missing TCK Python override shim: ${override_source}" >&2
    return 1
  fi

  mkdir -p "${TCK_PYTHONPATH_DIR}"
  cp "${override_source}" "${TCK_PYTHONPATH_DIR}/sitecustomize.py"
  export A2A_TCK_CLIENT_REQUIRED_EXTENSIONS="${TCK_REQUIRED_EXTENSIONS}"
  export PYTHONPATH="${TCK_PYTHONPATH_DIR}:${PYTHONPATH:-}"
}

install_tck_python_overrides

copy_tck_reports() {
  local source_dir=$1
  if [[ ! -d "${source_dir}" ]]; then
    return 0
  fi
  local report
  for report in compatibility.json compatibility.html tck_report.html junitreport.xml; do
    local source_file="${source_dir}/${report}"
    local target_file="${TCK_REPORT_DIR_ABS}/${report}"
    if [[ -f "${source_file}" && "$(realpath "${source_file}")" != "$(realpath -m "${target_file}")" ]]; then
      cp "${source_file}" "${target_file}"
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
  if ! check_sdk_requirements; then
    return 1
  fi
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
  check_sdk_requirements
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
