#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="${A2A_TUTORIAL_BUILD_DIR:-${root}/build-tutorials}"
prefix="${work}/install"
mcp_venv="${work}/mcp-venv"
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
    if curl --fail --silent --max-time 1 "${url}" >/dev/null; then return 0; fi
    sleep 0.1
  done
  echo "readiness deadline exceeded: ${url}" >&2
  return 1
}
start_mcp() {
  local fixture_set="$1" fixture_root="$2" port="$3" log="$4"
  "${mcp_venv}/bin/python" "${root}/examples/tutorials/mcp_server/server.py" \
    --fixture-set "${fixture_set}" --fixture-root "${fixture_root}" --host 127.0.0.1 --port "${port}" \
    >"${log}" 2>&1 &
  pids+=("$!")
  wait_ready "http://127.0.0.1:${port}/health" "${pids[-1]}"
}

cmake -S "${root}" -B "${work}/sdk" -DA2A_ENABLE_TESTING=OFF -DA2A_BUILD_EXAMPLES=OFF \
  -DA2A_ENABLE_POSTGRES_STORE=OFF -DCMAKE_INSTALL_PREFIX="${prefix}"
cmake --build "${work}/sdk" --parallel
cmake --install "${work}/sdk"
python3 -m venv "${mcp_venv}"
"${mcp_venv}/bin/python" -m pip install --disable-pip-version-check \
  --requirement "${root}/examples/tutorials/mcp_server/requirements.txt"

run_one() {
  local name="$1" fixture_set="$2" mcp_port="$3" fixture_root="$4" specialist="$5" coordinator="$6"
  local specialist_port="$7" coordinator_port="$8" client="$9"
  shift 9
  local source="${root}/examples/tutorials/${name}" build="${work}/${name}"
  cmake -S "${source}" -B "${build}" -DCMAKE_PREFIX_PATH="${prefix}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  cmake --build "${build}" --parallel
  start_mcp "${fixture_set}" "${fixture_root}" "${mcp_port}" "${build}/mcp.log"
  A2A_TUTORIAL_MODEL_PROVIDER=deterministic A2A_TUTORIAL_MCP_URL="http://127.0.0.1:${mcp_port}/mcp" \
    "${build}/${specialist}" "127.0.0.1:${specialist_port}" >"${build}/specialist.log" 2>&1 &
  pids+=("$!")
  wait_ready "http://127.0.0.1:${specialist_port}/.well-known/agent-card.json" "${pids[-1]}"
  A2A_TUTORIAL_MODEL_PROVIDER=deterministic \
    A2A_TUTORIAL_SPECIALIST_URL="http://127.0.0.1:${specialist_port}" \
    "${build}/${coordinator}" "127.0.0.1:${coordinator_port}" >"${build}/coordinator.log" 2>&1 &
  pids+=("$!")
  wait_ready "http://127.0.0.1:${coordinator_port}/.well-known/agent-card.json" "${pids[-1]}"
  "${build}/${client}" --coordinator-url "http://127.0.0.1:${coordinator_port}" "$@" | tee "${build}/client.log"
}

run_one job_application_assistant job_application 8090 \
  "${root}/examples/tutorials/job_application_assistant/samples" profile_analyst application_coordinator \
  8081 8080 application_client --resume-resource resume://candidate/alex \
  --job-file "${root}/examples/tutorials/job_application_assistant/samples/job_description.txt"
grep -Fq "deterministic backend" "${work}/job_application_assistant/specialist.log"
grep -Fq "deterministic backend" "${work}/job_application_assistant/coordinator.log"
grep -Eq "Structured analysis|Application draft" "${work}/job_application_assistant/client.log"
if "${work}/job_application_assistant/application_client" --coordinator-url http://127.0.0.1:8080 \
  --resume-resource resume://candidate/missing \
  --job-file "${root}/examples/tutorials/job_application_assistant/samples/job_description.txt" \
  >"${work}/job_application_assistant/missing.log" 2>&1; then
  echo "missing MCP resource unexpectedly succeeded" >&2
  exit 1
fi
grep -Eq "resource not found|Unknown resource" "${work}/job_application_assistant/missing.log"
kill -TERM "${pids[0]}"
wait "${pids[0]}" 2>/dev/null || true
if "${work}/job_application_assistant/application_client" --coordinator-url http://127.0.0.1:8080 \
  --resume-resource resume://candidate/alex \
  --job-file "${root}/examples/tutorials/job_application_assistant/samples/job_description.txt" \
  >"${work}/job_application_assistant/unavailable.log" 2>&1; then
  echo "unavailable MCP service unexpectedly succeeded" >&2
  exit 1
fi
grep -Fq "MCP service unavailable" "${work}/job_application_assistant/unavailable.log"

run_one customer_support_copilot customer_support 8190 \
  "${root}/examples/tutorials/customer_support_copilot/samples" support_specialist support_coordinator \
  8181 8180 support_client --ticket-resource ticket://northstar/billing-currency
grep -Fq "deterministic backend" "${work}/customer_support_copilot/specialist.log"
grep -Fq "deterministic backend" "${work}/customer_support_copilot/coordinator.log"
grep -Eq "Customer response|Internal support notes|billing" "${work}/customer_support_copilot/client.log"
"${work}/customer_support_copilot/support_client" --coordinator-url http://127.0.0.1:8180 \
  --ticket-file "${root}/examples/tutorials/customer_support_copilot/samples/unknown_ticket.txt" \
  >"${work}/customer_support_copilot/unknown.log"
grep -Fq "category: other" "${work}/customer_support_copilot/unknown.log"
grep -Fq "escalate: true" "${work}/customer_support_copilot/unknown.log"
if A2A_TUTORIAL_MODEL_PROVIDER=invalid "${work}/job_application_assistant/profile_analyst" \
  127.0.0.1:9081 >/dev/null 2>"${work}/invalid-provider.log"; then
  echo "invalid provider unexpectedly succeeded" >&2
  exit 1
fi
grep -Fq "unsupported model provider" "${work}/invalid-provider.log"
if A2A_TUTORIAL_MODEL_PROVIDER=openai_compatible "${work}/job_application_assistant/profile_analyst" \
  127.0.0.1:9081 >/dev/null 2>"${work}/missing-model-config.log"; then
  echo "incomplete model config unexpectedly succeeded" >&2
  exit 1
fi
grep -Fq "requires model base URL and model name" "${work}/missing-model-config.log"
echo "Tutorial smoke tests passed"
