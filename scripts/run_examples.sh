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
DEFAULT_BUILD_CONFIG="RelWithDebInfo"
DEFAULT_WINDOWS_GENERATOR="Visual Studio 17 2022"
DEFAULT_WINDOWS_ARCHITECTURE="x64"
EXAMPLES_APPS_DIR="examples/apps"

build_prefix="${A2A_EXAMPLE_BUILD_PREFIX:-$DEFAULT_BUILD_PREFIX}"
build_config="${A2A_EXAMPLE_BUILD_CONFIG:-$DEFAULT_BUILD_CONFIG}"
platform="${A2A_EXAMPLE_PLATFORM:-$(uname -s 2>/dev/null || true)}"
args=("$@")

is_windows() {
  [[ "${OS:-}" == "Windows_NT" || "${platform}" == MINGW* || "${platform}" == MSYS* || "${platform}" == CYGWIN* ]]
}

to_cmake_path() {
  local path="$1"
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -m "${path}"
    return
  fi
  printf '%s\n' "${path}"
}

find_example_binary() {
  local build_dir="$1"
  local config="$2"
  local candidates=(
    "${build_dir}/${config}/a2a_example.exe"
    "${build_dir}/${config}/a2a_example"
    "${build_dir}/a2a_example.exe"
    "${build_dir}/a2a_example"
  )
  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -f "${candidate}" ]]; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done
  return 1
}

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

cmake_dependency_args=()
cmake_generator_args=()
if is_windows; then
  vcpkg_root="${VCPKG_ROOT:-${HOME}/vcpkg}"
  target_triplet="${VCPKG_TARGET_TRIPLET:-${VCPKG_DEFAULT_TRIPLET:-x64-windows}}"
  host_triplet="${VCPKG_HOST_TRIPLET:-${target_triplet}}"

  if [[ -n "${CMAKE_TOOLCHAIN_FILE:-}" ]]; then
    toolchain_file="${CMAKE_TOOLCHAIN_FILE}"
  else
    toolchain_file="${vcpkg_root}/scripts/buildsystems/vcpkg.cmake"
  fi
  if [[ ! -f "${toolchain_file}" ]]; then
    echo "[run_examples] vcpkg toolchain not found: ${toolchain_file}" >&2
    echo "[run_examples] run ./scripts/install_build_deps.sh from Git Bash, then retry." >&2
    exit 1
  fi

  cmake_dependency_args+=(
    "-DCMAKE_TOOLCHAIN_FILE=$(to_cmake_path "${toolchain_file}")"
    "-DVCPKG_TARGET_TRIPLET=${target_triplet}"
    "-DVCPKG_HOST_TRIPLET=${host_triplet}"
  )

  generator="${A2A_EXAMPLE_CMAKE_GENERATOR:-$DEFAULT_WINDOWS_GENERATOR}"
  cmake_generator_args+=(-G "${generator}")
  if [[ "${generator}" == Visual\ Studio* ]]; then
    cmake_generator_args+=(-A "${A2A_EXAMPLE_CMAKE_ARCHITECTURE:-$DEFAULT_WINDOWS_ARCHITECTURE}")
  fi
elif [[ -n "${CMAKE_TOOLCHAIN_FILE:-}" ]]; then
  cmake_dependency_args+=("-DCMAKE_TOOLCHAIN_FILE=$(to_cmake_path "${CMAKE_TOOLCHAIN_FILE}")")
fi

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
    "${cmake_generator_args[@]}" \
    -DA2A_EXAMPLE_APP="${app}" \
    -DA2A_CPP_GIT_REPOSITORY="${A2A_CPP_GIT_REPOSITORY}" \
    -DA2A_CPP_GIT_TAG="${A2A_CPP_GIT_TAG}" \
    "${cmake_dependency_args[@]}"

  build_args=(--build "${build_dir}" --parallel)
  if is_windows; then
    build_args+=(--config "${build_config}")
  fi
  cmake "${build_args[@]}"

  echo "[run_examples] running ${app}"
  if ! example_binary="$(find_example_binary "${build_dir}" "${build_config}")"; then
    echo "[run_examples] built executable was not found under ${build_dir}" >&2
    exit 1
  fi
  if [[ "${app}" == "streaming_client" || "${app}" == "streaming_server" ]]; then
    "${example_binary}" --help
  else
    "${example_binary}"
  fi
done
