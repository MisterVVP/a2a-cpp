#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-build}"

cmake -S . -B "${BUILD_DIR}" -DA2A_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "${BUILD_DIR}" --parallel

examples=(
  example_discovery_only_client
  example_rest_client
  example_json_rpc_client
  example_streaming_client
  example_minimal_server_custom_executor
  example_grpc_client
  example_list_tasks_client
  example_cancel_task_client
  example_push_notification_config_client
  example_interceptor_client
  example_auth_policy_server
)

for target in "${examples[@]}"; do
  echo "[run_examples] running ${target}"
  "./${BUILD_DIR}/examples/${target}"
done
