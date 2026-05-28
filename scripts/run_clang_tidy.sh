#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="build"
BUILD_DIR_SET=0
CONFIG_FILE="${CLANG_TIDY_CONFIG_FILE:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --config." >&2
        exit 1
      fi
      CONFIG_FILE="$2"
      shift 2
      ;;
    --config-file)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --config-file." >&2
        exit 1
      fi
      CONFIG_FILE="$2"
      shift 2
      ;;
    --config-file=*)
      CONFIG_FILE="${1#--config-file=}"
      shift
      ;;
    --config=*)
      CONFIG_FILE="${1#--config=}"
      shift
      ;;
    -*)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
    *)
      if [[ "${BUILD_DIR_SET}" == "1" ]]; then
        echo "Unexpected extra positional argument: $1" >&2
        exit 1
      fi
      BUILD_DIR="$1"
      BUILD_DIR_SET=1
      shift
      ;;
  esac
done

CHECK_PROFILE_FILE="${CLANG_TIDY_CHECK_PROFILE_FILE:-${BUILD_DIR}/clang-tidy-check-profile.txt}"
ENABLE_CHECK_PROFILE="${CLANG_TIDY_ENABLE_CHECK_PROFILE:-0}"

if ! command -v clang-tidy >/dev/null 2>&1; then
  echo "clang-tidy is required but not installed." >&2
  exit 1
fi

if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
  echo "Missing ${BUILD_DIR}/compile_commands.json. Configure first:" >&2
  echo "  cmake -S . -B ${BUILD_DIR} -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
  exit 1
fi

mapfile -t TARGET_FILES < <(git ls-files 'src/**/*.cpp' 'tests/**/*.cpp')

if [[ ${#TARGET_FILES[@]} -eq 0 ]]; then
  echo "No C++ files found for clang-tidy."
  exit 0
fi

CLANG_TIDY_ARGS=(
  -p "${BUILD_DIR}"
)

if [[ -n "${CONFIG_FILE}" ]]; then
  if [[ ! -f "${CONFIG_FILE}" ]]; then
    echo "Missing clang-tidy config file: ${CONFIG_FILE}" >&2
    exit 1
  fi
  echo "[run_clang_tidy] Using config file: ${CONFIG_FILE}" >&2
  CLANG_TIDY_ARGS+=(--config-file="${CONFIG_FILE}")
else
  echo "[run_clang_tidy] Using default clang-tidy config discovery." >&2
fi

if [[ "${ENABLE_CHECK_PROFILE}" == "1" || "${ENABLE_CHECK_PROFILE}" == "true" ]]; then
  mkdir -p "$(dirname "${CHECK_PROFILE_FILE}")"
  echo "[run_clang_tidy] Check profile enabled: ${CHECK_PROFILE_FILE}" >&2
  CLANG_TIDY_ARGS+=(
    --enable-check-profile
    --store-check-profile="${CHECK_PROFILE_FILE}"
  )
else
  echo "[run_clang_tidy] Check profile disabled (set CLANG_TIDY_ENABLE_CHECK_PROFILE=1 to enable)." >&2
fi

clang-tidy "${CLANG_TIDY_ARGS[@]}" "${TARGET_FILES[@]}"
