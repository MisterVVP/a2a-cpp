#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
contextforge_url="${A2A_TUTORIAL_CONTEXTFORGE_URL:-http://127.0.0.1:4444}"
keycloak_url="${A2A_TUTORIAL_KEYCLOAK_URL:-http://127.0.0.1:8280}"
env_file="${A2A_TUTORIAL_MCP_ENV_FILE:?A2A_TUTORIAL_MCP_ENV_FILE is required}"
: "${A2A_TUTORIAL_CONTEXTFORGE_ADMIN_PASSWORD:?admin password is required}"
: "${A2A_TUTORIAL_MCP_AGENT_SECRET:?agent secret is required}"
request_json() {
  local method="$1" url="$2" body="$3" token="${4:-}" response
  local -a authorization=()
  [[ -n "${token}" ]] && authorization=(-H "Authorization: Bearer ${token}")
  response="$(curl --fail-with-body --silent --show-error --request "${method}" \
    -H 'Content-Type: application/json' "${authorization[@]}" --data "${body}" "${url}")" || return 1
  printf '%s' "${response}"
}
login_payload="$(jq -cn --arg password "${A2A_TUTORIAL_CONTEXTFORGE_ADMIN_PASSWORD}" \
  '{email:"admin@example.com",password:$password}')"
admin_token="$(request_json POST "${contextforge_url}/auth/login" "${login_payload}" | jq -er '.access_token')" || {
  echo 'ContextForge admin login failed' >&2; exit 1;
}
request_json PUT "${contextforge_url}/auth/sso/admin/providers/keycloak" \
  '{"trusted_for_api_auth":true,"api_audience":"mcp-gateway"}' "${admin_token}" >/dev/null || {
  echo 'ContextForge Keycloak provider configuration failed' >&2; exit 1;
}
ticket="$(cat "${root}/examples/tutorials/customer_support_copilot/samples/billing_currency_ticket.txt")"
resource_payload="$(jq -cn --arg content "${ticket}" '{resource:{name:"Billing currency ticket",uri:"ticket://northstar/billing-currency",description:"Deterministic Customer Support Copilot fixture",mime_type:"text/plain",content:$content},visibility:"public"}')"
resource_response=""
for path in /v1/resources /resources; do
  resource_response="$(request_json POST "${contextforge_url}${path}" "${resource_payload}" "${admin_token}" 2>/dev/null)" && break
  resource_response="$(request_json POST "${contextforge_url}${path}" "$(jq -c 'del(.visibility)' <<<"${resource_payload}")" "${admin_token}" 2>/dev/null)" && break
done
resource_id="$(jq -er '.id // .resource.id' <<<"${resource_response}")" || { echo 'ContextForge resource registration failed' >&2; exit 1; }
server_payload="$(jq -cn --arg id "${resource_id}" '{server:{name:"a2a-cpp-support-tutorial",description:"Customer Support Copilot MCP resources",associated_resources:[$id]},visibility:"public"}')"
server_response=""
for path in /v1/servers /servers; do
  server_response="$(request_json POST "${contextforge_url}${path}" "${server_payload}" "${admin_token}" 2>/dev/null)" && break
done
server_id="$(jq -er '.id // .server.id' <<<"${server_response}")" || { echo 'ContextForge virtual-server creation failed' >&2; exit 1; }
access_token="$(curl --fail-with-body --silent --show-error --request POST \
  --data-urlencode grant_type=client_credentials --data-urlencode client_id=mcp-agent \
  --data-urlencode "client_secret=${A2A_TUTORIAL_MCP_AGENT_SECRET}" \
  "${keycloak_url}/realms/a2a-tutorial/protocol/openid-connect/token" | jq -er '.access_token')" || {
  echo 'Keycloak token request failed' >&2; exit 1;
}
umask 077
{
  printf 'A2A_TUTORIAL_MCP_URL=http://contextforge:4444/servers/%s/mcp/\n' "${server_id}"
  printf 'A2A_TUTORIAL_MCP_TOKEN=%s\n' "${access_token}"
  printf 'A2A_TUTORIAL_MCP_SERVER_ID=%s\n' "${server_id}"
} >"${env_file}"
echo 'ContextForge tutorial resource and OAuth access were configured'
