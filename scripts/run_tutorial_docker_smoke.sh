#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
compose_file="${root}/examples/tutorials/compose.yaml"
cleanup() {
  docker compose -f "${compose_file}" down --volumes --remove-orphans || true
}
trap cleanup EXIT INT TERM
docker compose -f "${compose_file}" run --build --rm tutorials-smoke
