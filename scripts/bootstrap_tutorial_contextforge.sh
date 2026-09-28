#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
contextforge_url="${A2A_TUTORIAL_CONTEXTFORGE_URL:-http://127.0.0.1:4444}"
keycloak_url="${A2A_TUTORIAL_KEYCLOAK_URL:-https://keycloak:8443}"
keycloak_host_port="${A2A_TUTORIAL_KEYCLOAK_HOST_PORT:-8443}"
env_file="${A2A_TUTORIAL_MCP_ENV_FILE:?A2A_TUTORIAL_MCP_ENV_FILE is required}"
: "${A2A_TUTORIAL_CA_CERT:?tutorial CA certificate is required}"
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
  --cacert "${A2A_TUTORIAL_CA_CERT}" --resolve "keycloak:${keycloak_host_port}:127.0.0.1" \
  --noproxy keycloak \
  --data-urlencode grant_type=client_credentials --data-urlencode client_id=mcp-agent \
  --data-urlencode "client_secret=${A2A_TUTORIAL_MCP_AGENT_SECRET}" \
  "${keycloak_url}/realms/a2a-tutorial/protocol/openid-connect/token" | jq -er '.access_token')" || {
  echo 'Keycloak token request failed' >&2; exit 1;
}
token_claims="$(printf '%s' "${access_token}" | python3 -c '
import base64
import json
import sys

token = sys.stdin.read()
payload = token.split(".")[1]
payload += "=" * (-len(payload) % 4)
claims = json.loads(base64.urlsafe_b64decode(payload))
safe_names = ("iss", "aud", "azp", "exp", "preferred_username")
print(json.dumps({name: claims.get(name) for name in safe_names}, separators=(",", ":")))
')" || { echo 'Keycloak token payload is malformed' >&2; exit 1; }
expected_issuer='https://keycloak:8443/realms/a2a-tutorial'
jq -e --arg expected "${expected_issuer}" '.iss == $expected' <<<"${token_claims}" >/dev/null || {
  echo 'Keycloak token issuer does not match the configured HTTPS issuer' >&2; exit 1;
}
jq -e '([.aud] | flatten | index("mcp-gateway")) != null' <<<"${token_claims}" >/dev/null || {
  echo 'Keycloak token audience does not contain mcp-gateway' >&2; exit 1;
}
jq -e '.azp == "mcp-agent"' <<<"${token_claims}" >/dev/null || {
  echo 'Keycloak token authorized party is not mcp-agent' >&2; exit 1;
}
jq -e --argjson now "$(date +%s)" '(.exp | type == "number") and .exp > $now' \
  <<<"${token_claims}" >/dev/null || { echo 'Keycloak token is expired or has no expiry' >&2; exit 1; }
jq -e '.preferred_username | type == "string" and length > 0' <<<"${token_claims}" >/dev/null || {
  echo 'Keycloak token has no preferred_username' >&2; exit 1;
}
umask 077
{
  printf 'A2A_TUTORIAL_MCP_URL=http://contextforge:4444/servers/%s/mcp/\n' "${server_id}"
  printf 'A2A_TUTORIAL_MCP_TOKEN=%s\n' "${access_token}"
  printf 'A2A_TUTORIAL_MCP_SERVER_ID=%s\n' "${server_id}"
} >"${env_file}"
printf '%s\n' "${token_claims}" >"${env_file}.claims"
echo 'ContextForge tutorial resource and OAuth access were configured'
