#!/usr/bin/env bash
set -euo pipefail

DEFAULT_APPS=(hello_agent streaming_server push_notifications)
DEFAULT_BUILD_PREFIX="build-example"
EXAMPLES_APPS_DIR="examples/apps"

build_prefix="${A2A_EXAMPLE_BUILD_PREFIX:-$DEFAULT_BUILD_PREFIX}"
reuse_build_dir=0
args=("$@")

# Backward compatibility: the legacy runner accepted an optional build
# directory as its first positional argument. CI still invokes
# `./scripts/run_examples.sh build-examples`, so treat a first argument that is
# not an app name as the build prefix and use any remaining arguments as apps.
if [[ "${#args[@]}" -gt 0 && ! -d "${EXAMPLES_APPS_DIR}/${args[0]}" ]]; then
  build_prefix="${args[0]}"
  reuse_build_dir=1
  args=("${args[@]:1}")
fi

if [[ "${#args[@]}" -gt 0 ]]; then
  APPS=("${args[@]}")
else
  APPS=("${DEFAULT_APPS[@]}")
fi

A2A_CPP_GIT_REPOSITORY="${A2A_CPP_GIT_REPOSITORY:-file://${PWD}}"
A2A_CPP_GIT_TAG="${A2A_CPP_GIT_TAG:-HEAD}"

for app in "${APPS[@]}"; do
  if [[ ! -f "${EXAMPLES_APPS_DIR}/${app}/main.cpp" ]]; then
    echo "[run_examples] unknown example app: ${app}" >&2
    echo "[run_examples] expected ${EXAMPLES_APPS_DIR}/${app}/main.cpp to exist" >&2
    exit 1
  fi

  if [[ "${reuse_build_dir}" == "1" ]]; then
    build_dir="${build_prefix}"
  else
    build_dir="${build_prefix}-${app}"
  fi
  echo "[run_examples] configuring ${app}"
  cmake -S examples/fetch_content_consumer \
    -B "${build_dir}" \
    -DA2A_EXAMPLE_APP="${app}" \
    -DA2A_CPP_GIT_REPOSITORY="${A2A_CPP_GIT_REPOSITORY}" \
    -DA2A_CPP_GIT_TAG="${A2A_CPP_GIT_TAG}"
  cmake --build "${build_dir}" --parallel
  echo "[run_examples] running ${app}"
  "./${build_dir}/a2a_example"
done
