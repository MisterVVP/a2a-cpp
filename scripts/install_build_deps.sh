#!/usr/bin/env bash
set -euo pipefail

# Installs build/test/lint dependencies for a2a-cpp on Debian/Ubuntu or
# Windows through Git Bash and vcpkg.
# Usage:
#   ./scripts/install_build_deps.sh
#   ./scripts/install_build_deps.sh --dry-run

DRY_RUN=false
if [[ "${1:-}" == "--dry-run" ]]; then
  DRY_RUN=true
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
PLATFORM="${A2A_DEPS_PLATFORM:-$(uname -s 2>/dev/null || true)}"

run_cmd() {
  if [[ "${DRY_RUN}" == "true" ]]; then
    printf '[dry-run]'
    printf ' %q' "$@"
    printf '\n'
    return 0
  fi
  "$@"
}

is_windows_git_bash() {
  [[ "${OS:-}" == "Windows_NT" || "${PLATFORM}" == MINGW* || "${PLATFORM}" == MSYS* || "${PLATFORM}" == CYGWIN* ]]
}

install_debian_dependencies() {
  # shellcheck disable=SC1091
  source /etc/os-release
  if [[ "${ID:-}" != "ubuntu" && "${ID_LIKE:-}" != *"debian"* ]]; then
    echo "This installer supports Debian/Ubuntu and Windows Git Bash." >&2
    exit 1
  fi

  local packages=(
    build-essential
    cmake
    ninja-build
    pkg-config
    curl
    netcat-openbsd
    ca-certificates
    git
    protobuf-compiler
    libprotobuf-dev
    protobuf-compiler-grpc
    libgrpc++-dev
    libsimdjson-dev
    libcurl4-openssl-dev
    libpq-dev
    postgresql-client
    libgtest-dev
    clang-format
    clang-tidy
    lcov
  )
  local sudo_command=()
  if [[ "${EUID}" -ne 0 ]]; then
    sudo_command=(sudo)
  fi

  echo "Installing dependencies for ${PRETTY_NAME:-Linux}..."
  run_cmd "${sudo_command[@]}" apt-get update
  run_cmd "${sudo_command[@]}" apt-get install -y "${packages[@]}"
}

normalize_git_bash_path() {
  local path="$1"

  if command -v cygpath >/dev/null 2>&1 && [[ "${path}" =~ ^[A-Za-z]:[\\/] ]]; then
    path="$(cygpath -u "${path}")"
  fi

  if command -v readlink >/dev/null 2>&1; then
    local resolved_path
    resolved_path="$(readlink -f "${path}" 2>/dev/null || true)"
    if [[ -n "${resolved_path}" ]]; then
      path="${resolved_path}"
    fi
  fi

  printf '%s\n' "${path}"
}

find_vcpkg_executable() {
  local candidate=""

  if [[ -n "${VCPKG_ROOT:-}" ]]; then
    for candidate in "${VCPKG_ROOT}/vcpkg.exe" "${VCPKG_ROOT}/vcpkg"; do
      if [[ -f "${candidate}" ]]; then
        normalize_git_bash_path "${candidate}"
        return 0
      fi
    done
    return 1
  fi

  candidate="$(type -P vcpkg.exe 2>/dev/null || true)"
  if [[ -z "${candidate}" ]]; then
    candidate="$(type -P vcpkg 2>/dev/null || true)"
  fi
  if [[ -n "${candidate}" ]]; then
    normalize_git_bash_path "${candidate}"
    return 0
  fi

  for candidate in "${HOME}/vcpkg/vcpkg.exe" "${HOME}/vcpkg/vcpkg"; do
    if [[ -f "${candidate}" ]]; then
      normalize_git_bash_path "${candidate}"
      return 0
    fi
  done

  return 1
}

run_windows_batch_file() {
  local batch_file="$1"
  local windows_file="${batch_file}"
  if command -v cygpath >/dev/null 2>&1; then
    windows_file="$(cygpath -w "${batch_file}")"
  fi

  if [[ "${DRY_RUN}" == "true" ]]; then
    printf '[dry-run] cmd.exe /D /C "%s"\n' "${windows_file}"
    return 0
  fi

  # Pass the quoted batch path as the command string after /C. Avoid `call`
  # here because MSYS quoting can otherwise leave literal backslashes before
  # the quotes and make cmd.exe treat them as part of the filename.
  MSYS2_ARG_CONV_EXCL="*" MSYS_NO_PATHCONV=1 \
    cmd.exe /D /C "\"${windows_file}\""
}

install_windows_dependencies() {
  local target_triplet="${VCPKG_TARGET_TRIPLET:-${VCPKG_DEFAULT_TRIPLET:-x64-windows}}"
  local host_triplet="${VCPKG_HOST_TRIPLET:-${target_triplet}}"
  local vcpkg_executable=""
  local vcpkg_root=""

  vcpkg_executable="$(find_vcpkg_executable || true)"
  if [[ -n "${vcpkg_executable}" ]]; then
    vcpkg_root="$(cd -- "$(dirname -- "${vcpkg_executable}")" && pwd)"
    echo "Using existing vcpkg: ${vcpkg_executable}"
  else
    vcpkg_root="${VCPKG_ROOT:-${HOME}/vcpkg}"
    local bootstrap_file="${vcpkg_root}/bootstrap-vcpkg.bat"
    vcpkg_executable="${vcpkg_root}/vcpkg.exe"

    if [[ ! -d "${vcpkg_root}/.git" ]]; then
      if [[ "${DRY_RUN}" != "true" ]] && ! command -v git >/dev/null 2>&1; then
        echo "git is required. Install Git for Windows and run this script from Git Bash." >&2
        exit 1
      fi
      echo "Cloning vcpkg into ${vcpkg_root}..."
      run_cmd git clone https://github.com/microsoft/vcpkg.git "${vcpkg_root}"
    fi

    if [[ "${DRY_RUN}" != "true" && ! -f "${bootstrap_file}" ]]; then
      echo "vcpkg bootstrap script was not found at ${bootstrap_file}" >&2
      exit 1
    fi
    if [[ "${DRY_RUN}" != "true" ]] && ! command -v cmd.exe >/dev/null 2>&1; then
      echo "cmd.exe is unavailable. Run this script from Git Bash on Windows." >&2
      exit 1
    fi

    echo "Bootstrapping vcpkg..."
    run_windows_batch_file "${bootstrap_file}"

    if [[ "${DRY_RUN}" != "true" && ! -f "${vcpkg_executable}" ]]; then
      echo "vcpkg executable was not created at ${vcpkg_executable}" >&2
      exit 1
    fi
  fi

  echo "Installing a2a-cpp manifest dependencies for ${target_triplet}..."
  (
    cd "${REPOSITORY_ROOT}"
    run_cmd env \
      VCPKG_ROOT="${vcpkg_root}" \
      VCPKG_TARGET_TRIPLET="${target_triplet}" \
      VCPKG_HOST_TRIPLET="${host_triplet}" \
      "${vcpkg_executable}" install \
      --triplet "${target_triplet}" \
      --host-triplet "${host_triplet}"
  )

  echo "Dependency installation complete. The example runner uses the same defaults automatically."
  printf 'VCPKG_ROOT=%s\n' "${vcpkg_root}"
  printf 'VCPKG_TARGET_TRIPLET=%s\n' "${target_triplet}"
  printf 'VCPKG_HOST_TRIPLET=%s\n' "${host_triplet}"
}

if is_windows_git_bash; then
  install_windows_dependencies
elif [[ -r /etc/os-release ]]; then
  install_debian_dependencies
  echo "Dependency installation complete."
else
  echo "Unsupported environment. Use Debian/Ubuntu or Git Bash on Windows." >&2
  exit 1
fi
