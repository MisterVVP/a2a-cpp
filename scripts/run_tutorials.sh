#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="${A2A_TUTORIAL_BUILD_DIR:-${root}/build-tutorials}"
prefix="${work}/install"
pids=()
cleanup() { local pid; for pid in "${pids[@]:-}"; do kill -TERM "${pid}" 2>/dev/null || true; done; for pid in "${pids[@]:-}"; do wait "${pid}" 2>/dev/null || true; done; }
trap cleanup EXIT INT TERM
wait_ready() { local url="$1"; local pid="$2"; local attempts=100; while ((attempts-- > 0)); do if ! kill -0 "${pid}" 2>/dev/null; then echo "server exited before readiness: ${url}" >&2; return 1; fi; if curl --fail --silent --max-time 1 "${url}/.well-known/agent-card.json" >/dev/null; then return 0; fi; sleep 0.1; done; echo "readiness deadline exceeded: ${url}" >&2; return 1; }
cmake -S "${root}" -B "${work}/sdk" -DA2A_ENABLE_TESTING=OFF -DA2A_BUILD_EXAMPLES=OFF -DA2A_ENABLE_POSTGRES_STORE=OFF -DCMAKE_INSTALL_PREFIX="${prefix}"
cmake --build "${work}/sdk" --parallel
cmake --install "${work}/sdk"
run_one() { local name="$1" specialist="$2" coordinator="$3" sport="$4" cport="$5" client="$6"; shift 6; local source="${root}/examples/tutorials/${name}" build="${work}/${name}"; cmake -S "${source}" -B "${build}" -DCMAKE_PREFIX_PATH="${prefix}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON; cmake --build "${build}" --parallel; A2A_TUTORIAL_MODEL_PROVIDER=deterministic "${build}/${specialist}" "127.0.0.1:${sport}" >"${build}/specialist.log" 2>&1 & pids+=("$!"); wait_ready "http://127.0.0.1:${sport}" "${pids[-1]}"; A2A_TUTORIAL_MODEL_PROVIDER=deterministic A2A_TUTORIAL_SPECIALIST_URL="http://127.0.0.1:${sport}" "${build}/${coordinator}" "127.0.0.1:${cport}" >"${build}/coordinator.log" 2>&1 & pids+=("$!"); wait_ready "http://127.0.0.1:${cport}" "${pids[-1]}"; "${build}/${client}" --coordinator-url "http://127.0.0.1:${cport}" "$@" | tee "${build}/client.log"; }
run_one job_application_assistant profile_analyst application_coordinator 8081 8080 application_client --resume-file "${root}/examples/tutorials/job_application_assistant/samples/resume.txt" --job-file "${root}/examples/tutorials/job_application_assistant/samples/job_description.txt"
grep -Eq "Structured analysis|Application draft" "${work}/job_application_assistant/client.log"
run_one customer_support_copilot support_specialist support_coordinator 8181 8180 support_client --ticket-file "${root}/examples/tutorials/customer_support_copilot/samples/billing_currency_ticket.txt"
grep -Eq "Customer response|Internal support notes|billing" "${work}/customer_support_copilot/client.log"
"${work}/customer_support_copilot/support_client" --coordinator-url http://127.0.0.1:8180 --ticket-file "${root}/examples/tutorials/customer_support_copilot/samples/unknown_ticket.txt" >"${work}/customer_support_copilot/unknown.log"
grep -Fq "category: other" "${work}/customer_support_copilot/unknown.log"
grep -Fq "escalate: true" "${work}/customer_support_copilot/unknown.log"
if A2A_TUTORIAL_MODEL_PROVIDER=invalid "${work}/job_application_assistant/profile_analyst" 127.0.0.1:9081 >/dev/null 2>"${work}/invalid-provider.log"; then echo "invalid provider unexpectedly succeeded" >&2; exit 1; fi
grep -Fq "unsupported model provider" "${work}/invalid-provider.log"
if A2A_TUTORIAL_MODEL_PROVIDER=openai_compatible "${work}/job_application_assistant/profile_analyst" 127.0.0.1:9081 >/dev/null 2>"${work}/missing-model-config.log"; then echo "incomplete model config unexpectedly succeeded" >&2; exit 1; fi
grep -Fq "requires model base URL and model name" "${work}/missing-model-config.log"
if A2A_TUTORIAL_MODEL_PROVIDER=gemini "${work}/job_application_assistant/profile_analyst" 127.0.0.1:9081 >/dev/null 2>"${work}/missing-gemini-key.log"; then echo "gemini without API key unexpectedly succeeded" >&2; exit 1; fi
grep -Fq "gemini requires GEMINI_API_KEY or a model API key" "${work}/missing-gemini-key.log"
echo "Tutorial smoke tests passed"
