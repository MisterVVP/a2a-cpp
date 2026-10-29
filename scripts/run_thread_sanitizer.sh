#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${A2A_TSAN_BUILD_DIR:-build-tsan}"
CXX_COMPILER="${A2A_TSAN_CXX_COMPILER:-clang++}"
SANITIZER_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
TARGETS=(
  http_client_test
  http_json_streaming_integration_test
  http_json_transport_test
  json_rpc_transport_test
  server_dispatcher_task_store_integration_test
  server_dispatcher_test
  server_task_store_unit_test
  stream_cancellation_watcher_test
  stream_handle_lifecycle_test
  task_subscription_service_test
)

cmake -S . -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_CXX_COMPILER="${CXX_COMPILER}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="${SANITIZER_FLAGS}" \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread \
  -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread \
  -DA2A_ENABLE_ASIO="${A2A_TSAN_ASIO:-OFF}" \
  -DA2A_BUILD_EXAMPLES=OFF \
  -DA2A_ENABLE_POSTGRES_STORE=OFF \
  -DA2A_ENABLE_TESTING=ON

# Lifecycle and dispatcher tests are SDK-owned and require C++20 cancellation.
for optional_target in request_execution_test async_dispatcher_test async_sut_test coroutine_dispatcher_test; do
  if cmake --build "${BUILD_DIR}" --target help | grep -F "${optional_target}:" >/dev/null; then
    TARGETS+=("${optional_target}")
  fi
done

cmake --build "${BUILD_DIR}" --parallel --target "${TARGETS[@]}"

# No suppressions are applied: reports from this focused SDK-owned set must fail.
TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1}" \
  ctest --test-dir "${BUILD_DIR}" --output-on-failure -L tsan
