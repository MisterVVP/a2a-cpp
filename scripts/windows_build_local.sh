#!/usr/bin/env bash
set -Eeuo pipefail

# Windows Git Bash build+examples script for MisterVVP/a2a-cpp main branch.
#
# Local layout by default:
#   <repo>/.vcpkg              -> vcpkg tool checkout
#   <repo>/vcpkg_installed     -> installed packages/dependencies
#
# Usage:
#   ./scripts/windows_build_local.sh
#
# Useful env vars:
#   REPO_DIR=a2a-cpp                 # clone/update directory when not already in repo
#   VCPKG_ROOT=F:/tools/vcpkg        # optional custom vcpkg checkout location
#   BUILD_DIR=build-windows          # CMake build dir
#   CONFIG=RelWithDebInfo            # VS config: Debug, Release, RelWithDebInfo
#   TRIPLET=ci-x64-windows-release   # repo-provided Windows vcpkg triplet
#   CLEAN=1                          # delete build dir before configuring
#   RUN_TESTS=1                      # enable and run tests with ctest
#   UPDATE_REPO=0                    # do not git fetch/pull when already inside repo
#   RUN_EXAMPLES=0                   # build only; do not run examples
#   A2A_RUN_GRPC_EXAMPLE=1           # also run example_grpc_client; requires localhost:50051 server
#   RUN_PUSH_EXAMPLE=1               # also run example_push_notification_config_client

REPO_URL="https://github.com/MisterVVP/a2a-cpp.git"
REPO_DIR="${REPO_DIR:-a2a-cpp}"
REQUESTED_VCPKG_ROOT="${VCPKG_ROOT:-}"
BUILD_DIR="${BUILD_DIR:-build-windows}"
CONFIG="${CONFIG:-RelWithDebInfo}"
TRIPLET="${TRIPLET:-ci-x64-windows-release}"
HOST_TRIPLET="${HOST_TRIPLET:-$TRIPLET}"
CMAKE_GENERATOR="${CMAKE_GENERATOR:-Visual Studio 17 2022}"
UPDATE_REPO="${UPDATE_REPO:-1}"
RUN_EXAMPLES="${RUN_EXAMPLES:-1}"

log() {
  printf '\n[a2a-cpp] %s\n' "$*"
}

warn() {
  printf '\n[a2a-cpp] WARNING: %s\n' "$*" >&2
}

fail() {
  printf '\n[a2a-cpp] ERROR: %s\n' "$*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || fail "Missing required command: $1"
}

to_mixed_path() {
  # CMake and vcpkg accept C:/... paths well from Git Bash.
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -m "$1"
  else
    printf '%s' "$1"
  fi
}

path_basename_lower() {
  basename "$1" | tr '[:upper:]' '[:lower:]'
}

dir_has_entries() {
  [[ -d "$1" ]] && find "$1" -mindepth 1 -maxdepth 1 -print -quit | grep -q .
}

is_vcpkg_checkout() {
  local root="$1"

  [[ -f "$root/scripts/buildsystems/vcpkg.cmake" ]] && {
    [[ -f "$root/bootstrap-vcpkg.bat" || -f "$root/vcpkg.exe" ]]
  }
}

choose_vcpkg_root() {
  local repo_local_vcpkg
  repo_local_vcpkg="$(to_mixed_path "$repo_root/.vcpkg")"

  local requested=""
  if [[ -n "$REQUESTED_VCPKG_ROOT" ]]; then
    requested="$(to_mixed_path "$REQUESTED_VCPKG_ROOT")"
  fi

  if [[ -z "$requested" ]]; then
    printf '%s\n' "$repo_local_vcpkg"
    return 0
  fi

  if [[ "$(path_basename_lower "$requested")" == "vcpkg_installed" ]]; then
    warn "VCPKG_ROOT points to '$requested'."
    warn "'vcpkg_installed' is the package output directory, not the vcpkg tool checkout."
    warn "Keeping everything local and using '$repo_local_vcpkg' for the vcpkg tool checkout instead."
    printf '%s\n' "$repo_local_vcpkg"
    return 0
  fi

  printf '%s\n' "$requested"
}

bootstrap_vcpkg() {
  [[ -f "$VCPKG_ROOT/bootstrap-vcpkg.bat" ]] || fail "bootstrap-vcpkg.bat not found in $VCPKG_ROOT"

  log "Bootstrapping vcpkg"

  # Do NOT call this through `cmd.exe /C` from Git Bash.
  # Git Bash/MSYS quoting and path conversion around cmd.exe are easy to break.
  # Running the .bat directly from its own directory is the most stable option.
  (
    cd "$VCPKG_ROOT"
    MSYS2_ARG_CONV_EXCL="*" MSYS_NO_PATHCONV=1 ./bootstrap-vcpkg.bat -disableMetrics
  )
}

ensure_vcpkg() {
  VCPKG_ROOT="$(choose_vcpkg_root)"
  export VCPKG_ROOT

  log "Using VCPKG_ROOT=$VCPKG_ROOT"

  if is_vcpkg_checkout "$VCPKG_ROOT"; then
    :
  else
    if [[ -e "$VCPKG_ROOT" ]] && { [[ ! -d "$VCPKG_ROOT" ]] || dir_has_entries "$VCPKG_ROOT"; }; then
      fail "Refusing to clone vcpkg into non-empty non-vcpkg path: $VCPKG_ROOT"
    fi

    log "Cloning vcpkg into: $VCPKG_ROOT"
    git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
  fi

  if [[ ! -f "$VCPKG_ROOT/vcpkg.exe" ]]; then
    bootstrap_vcpkg
  fi

  toolchain="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

  [[ -f "$VCPKG_ROOT/vcpkg.exe" ]] || fail "vcpkg.exe not found after bootstrap: $VCPKG_ROOT/vcpkg.exe"
  [[ -f "$toolchain" ]] || fail "vcpkg toolchain not found: $toolchain"
}

need_cmd git
need_cmd cmake

ensure_main_branch_when_updating() {
  local branch
  branch="$(git branch --show-current)"
  if [[ -z "$branch" ]]; then
    fail "Could not determine current git branch. Set UPDATE_REPO=0 to skip branch update steps."
  fi

  if [[ "$branch" != "main" ]]; then
    fail "Refusing to update from branch '$branch'. Re-run with UPDATE_REPO=0 on PR branches, or switch to main explicitly."
  fi
}

if [[ -f "CMakeLists.txt" && -f "vcpkg.json" && -d ".git" ]]; then
  log "Using existing repository: $(pwd)"
  if [[ "$UPDATE_REPO" == "1" ]]; then
    ensure_main_branch_when_updating
    git fetch origin main
    git pull --ff-only origin main
  else
    log "Skipping git fetch/pull because UPDATE_REPO=0"
  fi
else
  if [[ -d "$REPO_DIR/.git" ]]; then
    log "Using existing clone: $REPO_DIR"
    cd "$REPO_DIR"
    if [[ "$UPDATE_REPO" == "1" ]]; then
      ensure_main_branch_when_updating
      git fetch origin main
      git pull --ff-only origin main
    else
      log "Skipping git fetch/pull because UPDATE_REPO=0"
    fi
  else
    log "Cloning main branch into: $REPO_DIR"
    git clone --branch main "$REPO_URL" "$REPO_DIR"
    cd "$REPO_DIR"
  fi
fi

repo_root="$(pwd)"
overlay_triplets="$(to_mixed_path "$repo_root/triplets")"

ensure_vcpkg

[[ -f "$repo_root/triplets/$TRIPLET.cmake" ]] || fail "Triplet file not found: triplets/$TRIPLET.cmake"

export VCPKG_DEFAULT_TRIPLET="$TRIPLET"
export VCPKG_HOST_TRIPLET="$HOST_TRIPLET"
export VCPKG_OVERLAY_TRIPLETS="$overlay_triplets"

log "Installing vcpkg manifest dependencies"
"$VCPKG_ROOT/vcpkg.exe" install \
  --triplet "$TRIPLET" \
  --host-triplet "$HOST_TRIPLET" \
  --overlay-triplets "$overlay_triplets"

if [[ "${CLEAN:-0}" == "1" ]]; then
  log "Cleaning build directory: $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

enable_testing="OFF"
if [[ "${RUN_TESTS:-0}" == "1" ]]; then
  enable_testing="ON"
fi

log "Configuring with CMake"
cmake -S . -B "$BUILD_DIR" \
  -G "$CMAKE_GENERATOR" \
  -A x64 \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
  -DVCPKG_TARGET_TRIPLET="$TRIPLET" \
  -DVCPKG_HOST_TRIPLET="$HOST_TRIPLET" \
  -DVCPKG_OVERLAY_TRIPLETS="$overlay_triplets" \
  -DA2A_BUILD_EXAMPLES=ON \
  -DA2A_ENABLE_TESTING="$enable_testing"

log "Building SDK and examples"
cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel

if [[ "${RUN_TESTS:-0}" == "1" ]]; then
  log "Running tests"
  ctest --test-dir "$BUILD_DIR" -C "$CONFIG" --output-on-failure
fi

if [[ "$RUN_EXAMPLES" != "1" ]]; then
  log "Skipping examples because RUN_EXAMPLES=0"
  log "Done"
  exit 0
fi

example_path() {
  local name="$1"
  local candidates=(
    "$BUILD_DIR/examples/$CONFIG/$name.exe"
    "$BUILD_DIR/examples/$name.exe"
    "$BUILD_DIR/$CONFIG/examples/$name.exe"
  )

  for candidate in "${candidates[@]}"; do
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  return 1
}

examples=(
  example_discovery_only_client
  example_rest_client
  example_json_rpc_client
  example_streaming_client
  example_minimal_server_custom_executor
  example_list_tasks_client
  example_cancel_task_client
  example_interceptor_client
  example_auth_policy_server
  example_push_notifications
)

if [[ "${RUN_PUSH_EXAMPLE:-0}" == "1" ]]; then
  examples+=(example_push_notification_config_client)
fi

if [[ "${A2A_RUN_GRPC_EXAMPLE:-0}" == "1" ]]; then
  examples+=(example_grpc_client)
fi

log "Running examples"
for example in "${examples[@]}"; do
  exe="$(example_path "$example")" || fail "Could not find executable for $example under $BUILD_DIR/examples"
  log "Running $example"
  "$exe"
done

log "Done"
