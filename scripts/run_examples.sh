#!/usr/bin/env bash
set -euo pipefail

DEFAULT_APPS=(hello_agent streaming_server push_notifications)
if [[ "$#" -gt 0 ]]; then
  APPS=("$@")
else
  APPS=("${DEFAULT_APPS[@]}")
fi
A2A_CPP_GIT_REPOSITORY="${A2A_CPP_GIT_REPOSITORY:-file://${PWD}}"
A2A_CPP_GIT_TAG="${A2A_CPP_GIT_TAG:-HEAD}"

for app in "${APPS[@]}"; do
  build_dir="build-example-${app}"
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
