#!/usr/bin/env bash
set -euo pipefail

# Installs build/test/lint dependencies for a2a-cpp on Debian/Ubuntu.
# Usage:
#   ./scripts/install_build_deps.sh
#   ./scripts/install_build_deps.sh --dry-run

DRY_RUN=false
if [[ "${1:-}" == "--dry-run" ]]; then
  DRY_RUN=true
fi

if [[ ! -r /etc/os-release ]]; then
  echo "Unsupported environment: missing /etc/os-release" >&2
  exit 1
fi

# shellcheck disable=SC1091
source /etc/os-release
if [[ "${ID:-}" != "ubuntu" && "${ID_LIKE:-}" != *"debian"* ]]; then
  echo "This installer currently supports Debian/Ubuntu only." >&2
  exit 1
fi

PACKAGES=(
  build-essential
  cmake
  ninja-build
  pkg-config
  curl
  ca-certificates
  git
  protobuf-compiler
  libprotobuf-dev
  protobuf-compiler-grpc
  libgrpc++-dev
  libgtest-dev
  clang-format
  clang-tidy
  lcov
)

run_cmd() {
  if [[ "${DRY_RUN}" == "true" ]]; then
    printf '[dry-run] %s\n' "$*"
    return 0
  fi
  "$@"
}

SUDO=()
if [[ "${EUID}" -ne 0 ]]; then
  SUDO=(sudo)
fi

echo "Installing dependencies for ${PRETTY_NAME:-Linux}..."
run_cmd "${SUDO[@]}" apt-get update
run_cmd "${SUDO[@]}" apt-get install -y "${PACKAGES[@]}"

echo "Dependency installation complete."
