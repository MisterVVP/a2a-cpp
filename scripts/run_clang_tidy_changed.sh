#!/usr/bin/env bash
set -euo pipefail

BASE_REF="${1:-origin/main}"
BUILD_DIR="${2:-build}"
SOURCE_CONFIG_FILE="${CLANG_TIDY_SOURCE_CONFIG_FILE:-.clang-tidy.src}"
TEST_CONFIG_FILE="${CLANG_TIDY_TEST_CONFIG_FILE:-.clang-tidy.tests}"

usage() {
  cat >&2 <<'USAGE'
Usage: scripts/run_clang_tidy_changed.sh [BASE_REF] [BUILD_DIR]

Runs the fast clang-tidy profile only on changed C++ source files that are
present in the compile database. Falls back to the full source profile when a
safe changed-file-only run cannot be determined.
USAGE
}

fallback_to_full_source_lint() {
  local reason="$1"
  echo "[run_clang_tidy_changed] ${reason}" >&2
  echo "[run_clang_tidy_changed] Falling back to full source clang-tidy profile." >&2
  CLANG_TIDY_PROFILE=src "$(dirname "$0")/run_clang_tidy.sh" --profile src "${BUILD_DIR}"
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if ! command -v clang-tidy >/dev/null 2>&1; then
  echo "clang-tidy is required but not installed." >&2
  exit 1
fi

if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
  fallback_to_full_source_lint "Missing ${BUILD_DIR}/compile_commands.json; changed-file analysis requires the compile database."
  exit $?
fi

if ! git rev-parse --verify --quiet "${BASE_REF}^{commit}" >/dev/null; then
  fallback_to_full_source_lint "Base ref '${BASE_REF}' cannot be resolved."
  exit $?
fi

if ! MERGE_BASE="$(git merge-base "${BASE_REF}" HEAD)"; then
  fallback_to_full_source_lint "Unable to find merge base between '${BASE_REF}' and HEAD."
  exit $?
fi

mapfile -t changed_cpp_files < <(
  git diff --name-only --diff-filter=ACMR "${MERGE_BASE}...HEAD" -- \
    '*.cpp' '*.cc' '*.cxx' '*.h' '*.hpp'
)

if [[ ${#changed_cpp_files[@]} -eq 0 ]]; then
  fallback_to_full_source_lint "No changed C++ files found relative to '${BASE_REF}'."
  exit $?
fi

is_generated_file() {
  local file_path="$1"
  case "${file_path}" in
    generated/*|*/generated/*|*.pb.cc|*.pb.h|*.grpc.pb.cc|*.grpc.pb.h)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

is_header_file() {
  local file_path="$1"
  case "${file_path}" in
    *.h|*.hpp)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

mapfile -t existing_changed_cpp_files < <(
  for file_path in "${changed_cpp_files[@]}"; do
    if [[ -f "${file_path}" ]]; then
      printf '%s\n' "${file_path}"
    else
      echo "[run_clang_tidy_changed] Skipping deleted or missing file: ${file_path}" >&2
    fi
  done
)

mapfile -t lintable_changed_cpp_files < <(
  for file_path in "${existing_changed_cpp_files[@]}"; do
    if is_generated_file "${file_path}"; then
      echo "[run_clang_tidy_changed] Skipping generated file: ${file_path}" >&2
      continue
    fi
    printf '%s\n' "${file_path}"
  done
)

if [[ ${#lintable_changed_cpp_files[@]} -eq 0 ]]; then
  fallback_to_full_source_lint "Changed C++ files are deleted, missing, or generated; no direct clang-tidy targets remain."
  exit $?
fi

mapfile -t compile_db_files < <(
  python3 - "${BUILD_DIR}/compile_commands.json" "${lintable_changed_cpp_files[@]}" <<'PYTHON'
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
)

mapfile -t changed_source_files < <(
  for file_path in "${lintable_changed_cpp_files[@]}"; do
    if ! is_header_file "${file_path}"; then
      printf '%s\n' "${file_path}"
    fi
  done
)

if [[ ${#changed_source_files[@]} -eq 0 ]]; then
  fallback_to_full_source_lint "Only headers changed; falling back because headers usually do not have direct compile database entries."
  exit $?
fi

if [[ ${#compile_db_files[@]} -eq 0 ]]; then
  fallback_to_full_source_lint "No changed C++ source files are present in ${BUILD_DIR}/compile_commands.json."
  exit $?
fi

mapfile -t missing_compile_db_files < <(
  python3 - "${BUILD_DIR}/compile_commands.json" "${changed_source_files[@]}" <<'PYTHON'
import json
import os
import sys

compile_commands_path = sys.argv[1]
candidates = sys.argv[2:]
with open(compile_commands_path, encoding="utf-8") as compile_commands_file:
    compile_commands = json.load(compile_commands_file)

compiled_files = {os.path.realpath(entry["file"]) for entry in compile_commands}
for candidate in candidates:
    if os.path.realpath(candidate) not in compiled_files:
        print(candidate)
PYTHON
)

if [[ ${#missing_compile_db_files[@]} -gt 0 ]]; then
  echo "[run_clang_tidy_changed] Changed source files missing from compile database:" >&2
  printf '  %s\n' "${missing_compile_db_files[@]}" >&2
  fallback_to_full_source_lint "Cannot safely lint only changed source files."
  exit $?
fi

run_changed_profile() {
  local profile_name="$1"
  local config_file="$2"
  shift 2
  local target_files=("$@")

  if [[ ${#target_files[@]} -eq 0 ]]; then
    return
  fi

  if [[ ! -f "${config_file}" ]]; then
    echo "Missing clang-tidy config file: ${config_file}" >&2
    exit 1
  fi

  echo "::group::clang-tidy changed ${profile_name}"
  echo "[run_clang_tidy_changed] Running changed-file ${profile_name} profile with ${config_file} on ${#target_files[@]} file(s):"
  printf '  %s\n' "${target_files[@]}"
  clang-tidy -p "${BUILD_DIR}" --config-file="${config_file}" "${target_files[@]}"
  if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    printf '%s\n' "- clang-tidy changed ${profile_name}: ${#target_files[@]} file(s), ${config_file}" >>"${GITHUB_STEP_SUMMARY}"
  fi
  echo "::endgroup::"
}

source_targets=()
test_targets=()
for file_path in "${compile_db_files[@]}"; do
  case "${file_path}" in
    tests/*|examples/*|benchmarks/*)
      test_targets+=("${file_path}")
      ;;
    *)
      source_targets+=("${file_path}")
      ;;
  esac
done

mapfile -t skipped_headers < <(
  for file_path in "${lintable_changed_cpp_files[@]}"; do
    if is_header_file "${file_path}"; then
      printf '%s\n' "${file_path}"
    fi
  done
)

if [[ ${#skipped_headers[@]} -gt 0 ]]; then
  echo "[run_clang_tidy_changed] Header changes detected; clang-tidy will check changed compiled sources directly and their included headers indirectly:" >&2
  printf '  %s\n' "${skipped_headers[@]}" >&2
fi

run_changed_profile source "${SOURCE_CONFIG_FILE}" "${source_targets[@]}"
run_changed_profile tests "${TEST_CONFIG_FILE}" "${test_targets[@]}"
