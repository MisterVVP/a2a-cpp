#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"${REPO_ROOT}/build-python-interop"}
PYTHON_BIN=${PYTHON_BIN:-python3}
A2A_PYTHON_REF=${A2A_PYTHON_REF:-"main"}
A2A_PYTHON_REPO=${A2A_PYTHON_REPO:-"https://github.com/a2aproject/a2a-python.git"}
CHECKOUT_DIR="${BUILD_DIR}/a2a-python"

mkdir -p "${BUILD_DIR}"
rm -rf "${CHECKOUT_DIR}"

echo "[python-interop] cloning ${A2A_PYTHON_REPO} @ ${A2A_PYTHON_REF}"
git clone --depth 1 --branch "${A2A_PYTHON_REF}" "${A2A_PYTHON_REPO}" "${CHECKOUT_DIR}"

"${PYTHON_BIN}" -m venv "${BUILD_DIR}/venv"
# shellcheck source=/dev/null
source "${BUILD_DIR}/venv/bin/activate"
pip install --upgrade pip
pip install "${CHECKOUT_DIR}"

"${PYTHON_BIN}" - <<'PY'
import a2a
print("[python-interop] installed a2a-python version:", getattr(a2a, "__version__", "unknown"))
PY

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}/cpp" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "${BUILD_DIR}/cpp" --parallel
ctest --test-dir "${BUILD_DIR}/cpp" --output-on-failure -R 'GrpcTransportIntegrationTest\..*'

echo "[python-interop] completed"
