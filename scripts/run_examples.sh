#!/usr/bin/env bash
set -euo pipefail

DEFAULT_APPS=(
  hello_agent
  simple_client
  rest_server
  json_rpc_server
  grpc_server
  streaming_client
  streaming_server
  push_notifications
  auth_policy_server
)
DEFAULT_BUILD_PREFIX="build-example"
EXAMPLES_APPS_DIR="examples/apps"

build_prefix="${A2A_EXAMPLE_BUILD_PREFIX:-$DEFAULT_BUILD_PREFIX}"
args=("$@")

# Backward compatibility: the legacy runner accepted an optional build directory
# as its first positional argument. Treat a first argument that is not an app
# name as the build prefix and use any remaining arguments as apps.
if [[ "${#args[@]}" -gt 0 && ! -d "${EXAMPLES_APPS_DIR}/${args[0]}" ]]; then
  build_prefix="${args[0]}"
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

  build_dir="${build_prefix}-${app}"
  echo "[run_examples] configuring ${app}"
  cmake -S examples/fetch_content_consumer \
    -B "${build_dir}" \
    -DA2A_EXAMPLE_APP="${app}" \
    -DA2A_CPP_GIT_REPOSITORY="${A2A_CPP_GIT_REPOSITORY}" \
    -DA2A_CPP_GIT_TAG="${A2A_CPP_GIT_TAG}"
  cmake --build "${build_dir}" --parallel
  echo "[run_examples] running ${app}"
  if [[ "${app}" == "streaming_client" ]]; then
    "./${build_dir}/a2a_example" --help
  else
    "./${build_dir}/a2a_example"
  fi
done