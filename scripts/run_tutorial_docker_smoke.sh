#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cleanup() { docker compose -f "${root}/examples/tutorials/job_application_assistant/compose.yaml" down --remove-orphans || true; docker compose -f "${root}/examples/tutorials/customer_support_copilot/compose.yaml" down --remove-orphans || true; }
trap cleanup EXIT INT TERM
for tutorial in job_application_assistant customer_support_copilot; do docker compose -f "${root}/examples/tutorials/${tutorial}/compose.yaml" up --build -d; done
for attempt in $(seq 1 100); do curl -fsS http://127.0.0.1:8080/.well-known/agent-card.json >/dev/null && curl -fsS http://127.0.0.1:8180/.well-known/agent-card.json >/dev/null && break; [[ "${attempt}" == 100 ]] && exit 1; sleep 0.1; done
docker compose -f "${root}/examples/tutorials/job_application_assistant/compose.yaml" exec -T application-coordinator ./application_client --coordinator-url http://application-coordinator:8080 --resume-file samples/resume.txt --job-file samples/job_description.txt | grep -F "Application draft"
docker compose -f "${root}/examples/tutorials/customer_support_copilot/compose.yaml" exec -T support-coordinator ./support_client --coordinator-url http://support-coordinator:8180 --ticket-file samples/billing_currency_ticket.txt | grep -F "billing"
