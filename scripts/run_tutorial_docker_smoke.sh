#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
support_compose="${root}/examples/tutorials/customer_support_copilot/compose.yaml"
contextforge_compose="${root}/examples/tutorials/customer_support_copilot/compose.contextforge.yaml"
job_compose="${root}/examples/tutorials/job_application_assistant/compose.yaml"
temporary_directory="$(mktemp -d)"
export A2A_TUTORIAL_TLS_DIR="${temporary_directory}/tls"
export A2A_TUTORIAL_CA_CERT="${A2A_TUTORIAL_TLS_DIR}/ca.crt"
export A2A_TUTORIAL_MCP_ENV_FILE="${temporary_directory}/mcp.env"
mkdir -p "${A2A_TUTORIAL_TLS_DIR}"
: >"${A2A_TUTORIAL_MCP_ENV_FILE}"
random_secret() { openssl rand -hex 32; }
random_encryption_key() { openssl rand -base64 32 | tr '+/' '-_'; }
export A2A_TUTORIAL_MCP_AGENT_SECRET="$(random_secret)"
export A2A_TUTORIAL_CONTEXTFORGE_OIDC_SECRET="$(random_secret)"
export A2A_TUTORIAL_CONTEXTFORGE_ADMIN_PASSWORD="$(random_secret)"
export A2A_TUTORIAL_CONTEXTFORGE_JWT_SECRET="$(random_secret)"
export A2A_TUTORIAL_CONTEXTFORGE_AUTH_ENCRYPTION_SECRET="$(random_encryption_key)"
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 2 \
  -subj '/CN=a2a-cpp tutorial CA' -keyout "${A2A_TUTORIAL_TLS_DIR}/ca.key" \
  -out "${A2A_TUTORIAL_CA_CERT}" >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -sha256 -subj '/CN=keycloak' \
  -keyout "${A2A_TUTORIAL_TLS_DIR}/keycloak.key" \
  -out "${A2A_TUTORIAL_TLS_DIR}/keycloak.csr" >/dev/null 2>&1
printf 'subjectAltName=DNS:keycloak\nextendedKeyUsage=serverAuth\n' >"${A2A_TUTORIAL_TLS_DIR}/keycloak.ext"
openssl x509 -req -sha256 -days 1 -in "${A2A_TUTORIAL_TLS_DIR}/keycloak.csr" \
  -CA "${A2A_TUTORIAL_CA_CERT}" -CAkey "${A2A_TUTORIAL_TLS_DIR}/ca.key" -CAcreateserial \
  -extfile "${A2A_TUTORIAL_TLS_DIR}/keycloak.ext" \
  -out "${A2A_TUTORIAL_TLS_DIR}/keycloak.crt" >/dev/null 2>&1
chmod 0600 "${A2A_TUTORIAL_TLS_DIR}/ca.key"
chmod 0644 "${A2A_TUTORIAL_TLS_DIR}/keycloak.key"
compose_support=(docker compose -f "${support_compose}" -f "${contextforge_compose}")
cleanup() {
  docker compose -f "${job_compose}" down --remove-orphans || true
  "${compose_support[@]}" down --remove-orphans --volumes || true
  rm -rf "${temporary_directory}"
}
trap cleanup EXIT INT TERM
wait_for_url() {
  local url="$1"
  for attempt in $(seq 1 120); do
    curl --fail --silent "${url}" >/dev/null 2>&1 && return
    [[ "${attempt}" == 120 ]] && { echo "Timed out waiting for ${url}" >&2; return 1; }
    sleep 1
  done
}
wait_for_keycloak() {
  for attempt in $(seq 1 120); do
    curl --fail --silent --cacert "${A2A_TUTORIAL_CA_CERT}" \
      --resolve keycloak:8443:127.0.0.1 --noproxy keycloak \
      https://keycloak:8443/realms/a2a-tutorial/.well-known/openid-configuration \
      >/dev/null 2>&1 && return
    [[ "${attempt}" == 120 ]] && { echo 'Timed out waiting for HTTPS Keycloak' >&2; return 1; }
    sleep 1
  done
}
docker compose -f "${job_compose}" up --build -d
wait_for_url http://127.0.0.1:8080/.well-known/agent-card.json
docker compose -f "${job_compose}" exec -T application-coordinator ./application_client \
  --coordinator-url http://application-coordinator:8080 --resume-file samples/resume.txt \
  --job-file samples/job_description.txt | grep -F "Application draft"
# Build the support agents before issuing the five-minute Keycloak access token.
# Otherwise a cold CI build can consume the token's lifetime before the MCP call.
"${compose_support[@]}" build support-specialist support-coordinator
"${compose_support[@]}" up -d keycloak
wait_for_keycloak
"${compose_support[@]}" up -d contextforge
if ! wait_for_url http://127.0.0.1:4444/health; then
  "${compose_support[@]}" ps contextforge >&2
  "${compose_support[@]}" logs --no-color contextforge >&2
  exit 1
fi
"${root}/scripts/bootstrap_tutorial_contextforge.sh"
server_id="$(sed -n 's/^A2A_TUTORIAL_MCP_SERVER_ID=//p' "${A2A_TUTORIAL_MCP_ENV_FILE}")"
unauthenticated_status="$(curl --silent --output /dev/null --write-out '%{http_code}' --request POST \
  -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: resources/read' \
  -H 'Mcp-Name: ticket://northstar/billing-currency' \
  --data '{"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"ticket://northstar/billing-currency"}}' \
  "http://127.0.0.1:4444/servers/${server_id}/mcp/")"
[[ "${unauthenticated_status}" == 401 || "${unauthenticated_status}" == 403 ]] || {
  echo "Unauthenticated MCP request unexpectedly returned HTTP ${unauthenticated_status}" >&2; exit 1;
}
token="$(sed -n 's/^A2A_TUTORIAL_MCP_TOKEN=//p' "${A2A_TUTORIAL_MCP_ENV_FILE}")"
preflight_body="${temporary_directory}/mcp-preflight.json"
preflight_status="$(curl --silent --show-error --output "${preflight_body}" --write-out '%{http_code}' \
  --request POST -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: resources/read' \
  -H 'Mcp-Name: ticket://northstar/billing-currency' -H "Authorization: Bearer ${token}" \
  --data '{"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"ticket://northstar/billing-currency","_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{"name":"a2a-cpp-tutorial","version":"1.0.0"},"io.modelcontextprotocol/clientCapabilities":{}}}}' \
  "http://127.0.0.1:4444/servers/${server_id}/mcp/")"
if [[ ! "${preflight_status}" =~ ^2[0-9][0-9]$ ]] || ! grep -Fq billing "${preflight_body}"; then
  echo "Authenticated MCP preflight failed with HTTP ${preflight_status}" >&2
  cat "${preflight_body}" >&2
  echo 'Safe Keycloak token claims:' >&2
  cat "${A2A_TUTORIAL_MCP_ENV_FILE}.claims" >&2
  "${compose_support[@]}" logs --no-color contextforge >&2
  exit 1
fi
unset token
"${compose_support[@]}" up -d support-specialist support-coordinator
wait_for_url http://127.0.0.1:8180/.well-known/agent-card.json
"${compose_support[@]}" exec -T support-coordinator ./support_client \
  --coordinator-url http://support-coordinator:8180 \
  --ticket-resource ticket://northstar/billing-currency | grep -F billing
