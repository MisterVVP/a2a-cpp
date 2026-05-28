#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="build"
BUILD_DIR_SET=0
PROFILE="${CLANG_TIDY_PROFILE:-split}"
CONFIG_FILE="${CLANG_TIDY_CONFIG_FILE:-}"

usage() {
  cat >&2 <<'USAGE'
Usage: scripts/run_clang_tidy.sh [--profile src|tests|split|strict] [--config-file FILE] [BUILD_DIR]

Profiles:
  src     Run the SDK source/public-header clang-tidy profile.
  tests   Run the lighter tests/examples/interop clang-tidy profile.
  split   Run src and tests profiles separately (default).
  strict  Run the full strict profile across source, tests, examples, and interop.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --profile." >&2
        exit 1
      fi
      PROFILE="$2"
      shift 2
      ;;
    --profile=*)
      PROFILE="${1#--profile=}"
      shift
      ;;
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
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage
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

case "${PROFILE}" in
  src|tests|split|strict) ;;
  *)
    echo "Unknown clang-tidy profile: ${PROFILE}" >&2
    usage
    exit 1
    ;;
esac

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

resolve_config_file() {
  local profile_name="$1"
  if [[ -n "${CONFIG_FILE}" ]]; then
    printf '%s\n' "${CONFIG_FILE}"
    return
  fi

  case "${profile_name}" in
    src) printf '%s\n' ".clang-tidy.src" ;;
    tests) printf '%s\n' ".clang-tidy.tests" ;;
    strict) printf '%s\n' ".clang-tidy" ;;
    *)
      echo "Unsupported profile for config resolution: ${profile_name}" >&2
      exit 1
      ;;
  esac
}

filter_compile_commands_files() {
  local compile_commands_file="${BUILD_DIR}/compile_commands.json"
  python3 - "${compile_commands_file}" "$@" <<'PYTHON'
import json
import os
import sys

compile_commands_path = sys.argv[1]
candidates = sys.argv[2:]
with open(compile_commands_path, encoding="utf-8") as compile_commands_file:
    compile_commands = json.load(compile_commands_file)

compiled_files = {os.path.realpath(entry["file"]) for entry in compile_commands}
for candidate in candidates:
    if os.path.realpath(candidate) in compiled_files:
        print(candidate)
PYTHON
}

collect_files() {
  local profile_name="$1"
  local files
  case "${profile_name}" in
    src)
      mapfile -t files < <(git ls-files 'src/**/*.cpp')
      filter_compile_commands_files "${files[@]}"
      ;;
    tests)
      mapfile -t files < <(git ls-files 'tests/**/*.cpp' 'examples/**/*.cpp')
      filter_compile_commands_files "${files[@]}"
      ;;
    strict)
      mapfile -t files < <(git ls-files 'src/**/*.cpp' 'tests/**/*.cpp' 'examples/**/*.cpp')
      filter_compile_commands_files "${files[@]}"
      ;;
    *)
      echo "Unsupported profile for file collection: ${profile_name}" >&2
      exit 1
      ;;
  esac
}

run_profile() {
  local profile_name="$1"
  local display_name="$2"
  local config_file
  local started_at
  local finished_at
  local elapsed_seconds
  config_file="$(resolve_config_file "${profile_name}")"

  if [[ ! -f "${config_file}" ]]; then
    echo "Missing clang-tidy config file: ${config_file}" >&2
    exit 1
  fi

  mapfile -t target_files < <(collect_files "${profile_name}")
  if [[ ${#target_files[@]} -eq 0 ]]; then
    echo "[run_clang_tidy] No files found for ${display_name}."
    return
  fi

  local clang_tidy_args=(
    -p "${BUILD_DIR}"
    --config-file="${config_file}"
  )

  if [[ "${ENABLE_CHECK_PROFILE}" == "1" || "${ENABLE_CHECK_PROFILE}" == "true" ]]; then
    local profile_output="${CHECK_PROFILE_FILE}"
    if [[ "${PROFILE}" == "split" ]]; then
      profile_output="${CHECK_PROFILE_FILE%.txt}-${profile_name}.txt"
    fi
    mkdir -p "$(dirname "${profile_output}")"
    echo "[run_clang_tidy] Check profile enabled for ${display_name}: ${profile_output}" >&2
    clang_tidy_args+=(
      --enable-check-profile
      --store-check-profile="${profile_output}"
    )
  else
    echo "[run_clang_tidy] Check profile disabled for ${display_name} (set CLANG_TIDY_ENABLE_CHECK_PROFILE=1 to enable)." >&2
  fi

  echo "::group::clang-tidy ${display_name}"
  echo "[run_clang_tidy] Running ${display_name} with ${config_file} on ${#target_files[@]} file(s)."
  started_at="$(date +%s)"
  clang-tidy "${clang_tidy_args[@]}" "${target_files[@]}"
  finished_at="$(date +%s)"
  elapsed_seconds=$((finished_at - started_at))
  echo "[run_clang_tidy] clang-tidy ${display_name} completed in ${elapsed_seconds}s."
  if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    printf '%s\n' "- clang-tidy ${display_name}: ${elapsed_seconds}s (${#target_files[@]} file(s), ${config_file})" >>"${GITHUB_STEP_SUMMARY}"
  fi
  echo "::endgroup::"
}

case "${PROFILE}" in
  src)
    run_profile src "source"
    ;;
  tests)
    run_profile tests "tests"
    ;;
  split)
    run_profile src "source"
    run_profile tests "tests"
    ;;
  strict)
    run_profile strict "strict"
    ;;
esac
