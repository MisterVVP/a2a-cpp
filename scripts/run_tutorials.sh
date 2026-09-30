#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="${A2A_TUTORIAL_BUILD_DIR:-${root}/build-tutorials}"
prefix="${work}/install"
pids=()

cleanup() {
  local pid
  for pid in "${pids[@]:-}"; do kill -TERM "${pid}" 2>/dev/null || true; done
  for pid in "${pids[@]:-}"; do wait "${pid}" 2>/dev/null || true; done
}
trap cleanup EXIT INT TERM

wait_ready() {
  local url="$1" pid="$2" attempts=100
  while ((attempts-- > 0)); do
    if ! kill -0 "${pid}" 2>/dev/null; then
      echo "server exited before readiness: ${url}" >&2
      return 1
    fi
    if curl --fail --silent --max-time 1 "${url}/.well-known/agent-card.json" >/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "readiness deadline exceeded: ${url}" >&2
  return 1
}

build_tutorial() {
  local name="$1"
  local source="${root}/examples/tutorials/${name}"
  local build="${work}/${name}"
  cmake -S "${source}" -B "${build}" -DCMAKE_PREFIX_PATH="${prefix}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  cmake --build "${build}" --parallel
}

cmake -S "${root}" -B "${work}/sdk" -DA2A_ENABLE_TESTING=OFF -DA2A_BUILD_EXAMPLES=OFF \
  -DA2A_ENABLE_POSTGRES_STORE=OFF -DCMAKE_INSTALL_PREFIX="${prefix}"
cmake --build "${work}/sdk" --parallel
cmake --install "${work}/sdk"

build_tutorial job_application_assistant
build_tutorial customer_support_copilot

job_build="${work}/job_application_assistant"
A2A_TUTORIAL_MODEL_PROVIDER=deterministic "${job_build}/profile_analyst" 127.0.0.1:8081 \
  >"${job_build}/specialist.log" 2>&1 &
pids+=("$!")
wait_ready http://127.0.0.1:8081 "${pids[-1]}"

A2A_TUTORIAL_MODEL_PROVIDER=deterministic A2A_TUTORIAL_SPECIALIST_URL=http://127.0.0.1:8081 \
  "${job_build}/application_coordinator" 127.0.0.1:8080 >"${job_build}/coordinator.log" 2>&1 &
pids+=("$!")
wait_ready http://127.0.0.1:8080 "${pids[-1]}"

"${job_build}/application_client" --coordinator-url http://127.0.0.1:8080 \
  --resume-file "${root}/examples/tutorials/job_application_assistant/samples/resume.txt" \
  --job-file "${root}/examples/tutorials/job_application_assistant/samples/job_description.txt" \
  | tee "${job_build}/client.log"

grep -Fq "deterministic backend" "${job_build}/specialist.log"
grep -Fq "deterministic backend" "${job_build}/coordinator.log"
grep -Eq "Structured analysis|Application draft" "${job_build}/client.log"

if A2A_TUTORIAL_MODEL_PROVIDER=invalid "${job_build}/profile_analyst" 127.0.0.1:9081 \
  >/dev/null 2>"${work}/invalid-provider.log"; then
  echo "invalid provider unexpectedly succeeded" >&2
  exit 1
fi
grep -Fq "unsupported model provider" "${work}/invalid-provider.log"

if A2A_TUTORIAL_MODEL_PROVIDER=openai_compatible "${job_build}/profile_analyst" 127.0.0.1:9081 \
  >/dev/null 2>"${work}/missing-model-config.log"; then
  echo "incomplete model config unexpectedly succeeded" >&2
  exit 1
fi
grep -Fq "requires model base URL and model name" "${work}/missing-model-config.log"

echo "Host tutorial smoke tests passed"
