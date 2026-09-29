#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
contextforge_url="${A2A_TUTORIAL_CONTEXTFORGE_URL:-http://127.0.0.1:4444}"
keycloak_url="${A2A_TUTORIAL_KEYCLOAK_URL:-https://keycloak:8443}"
keycloak_issuer="https://keycloak:8443/realms/a2a-tutorial"
service_principal_email="svc-mcp-agent@example.com"
service_role_name="a2a_tutorial_mcp_reader"
resource_uri="ticket://northstar/billing-currency"
server_name="a2a-cpp-support-tutorial"
env_file="${A2A_TUTORIAL_MCP_ENV_FILE:?A2A_TUTORIAL_MCP_ENV_FILE is required}"
: "${A2A_TUTORIAL_CA_CERT:?tutorial CA certificate is required}"
: "${A2A_TUTORIAL_CONTEXTFORGE_ADMIN_PASSWORD:?admin password is required}"
: "${A2A_TUTORIAL_MCP_AGENT_SECRET:?agent secret is required}"
request_json() {
  local method="$1" url="$2" body="$3" token="${4:-}" response
  local -a authorization=()
  [[ -n "${token}" ]] && authorization=(-H "Authorization: Bearer ${token}")
  if ! response="$(curl --fail-with-body --silent --show-error --request "${method}" \
    -H 'Content-Type: application/json' "${authorization[@]}" --data "${body}" "${url}")"; then
    [[ -n "${response}" ]] && printf '%s\n' "${response}" >&2
    return 1
  fi
  printf '%s' "${response}"
}
request_get_json() {
  local url="$1" token="$2" response
  if ! response="$(curl --fail-with-body --silent --show-error \
    -H "Authorization: Bearer ${token}" "${url}")"; then
    [[ -n "${response}" ]] && printf '%s\n' "${response}" >&2
    return 1
  fi
  printf '%s' "${response}"
}
login_payload="$(jq -cn --arg password "${A2A_TUTORIAL_CONTEXTFORGE_ADMIN_PASSWORD}" \
  '{email:"admin@example.com",password:$password}')"
admin_token="$(request_json POST "${contextforge_url}/v1/auth/login" "${login_payload}" | jq -er '.access_token')" || {
  echo 'ContextForge admin login failed' >&2; exit 1;
}
request_json PUT "${contextforge_url}/v1/auth/sso/admin/providers/keycloak" \
  '{"trusted_for_api_auth":true,"api_audience":"mcp-gateway"}' "${admin_token}" >/dev/null || {
  echo 'ContextForge Keycloak provider configuration failed' >&2; exit 1;
}

users_response="$(request_get_json "${contextforge_url}/v1/auth/email/admin/users" "${admin_token}")" || {
  echo 'ContextForge user lookup failed' >&2; exit 1;
}
if ! jq -e --arg email "${service_principal_email}" 'any(.[]; .email == $email)' <<<"${users_response}" >/dev/null; then
  service_principal_password="Aa1!$(openssl rand -hex 32 | sed 's/../&!/g')"
  service_principal_payload="$(jq -cn --arg email "${service_principal_email}" --arg password "${service_principal_password}" \
    '{email:$email,password:$password,full_name:"A2A tutorial MCP agent",is_admin:false,is_active:true,password_change_required:false}')"
  request_json POST "${contextforge_url}/v1/auth/email/admin/users" \
    "${service_principal_payload}" "${admin_token}" >/dev/null || {
    echo 'ContextForge service-principal user creation failed' >&2; exit 1;
  }
  unset service_principal_password
fi

roles_response="$(request_get_json "${contextforge_url}/v1/rbac/roles?scope=global" "${admin_token}")" || {
  echo 'ContextForge role lookup failed' >&2; exit 1;
}
service_role_id="$(jq -r --arg name "${service_role_name}" \
  'first(.[] | select(.name == $name and .scope == "global") | .id) // empty' <<<"${roles_response}")"
if [[ -z "${service_role_id}" ]]; then
  service_role_payload="$(jq -cn --arg name "${service_role_name}" \
    '{name:$name,description:"Least-privilege role for the Customer Support Copilot MCP client",scope:"global",permissions:["servers.use","resources.read"],is_system_role:false}')"
  service_role_response="$(request_json POST "${contextforge_url}/v1/rbac/roles" "${service_role_payload}" "${admin_token}")" || {
    echo 'ContextForge service-principal role creation failed' >&2; exit 1;
  }
  service_role_id="$(jq -er '.id' <<<"${service_role_response}")" || {
    echo 'ContextForge service-principal role response is missing an id' >&2; exit 1;
  }
fi

assignments_response="$(request_get_json "${contextforge_url}/v1/rbac/users/${service_principal_email}/roles?scope=global" "${admin_token}")" || {
  echo 'ContextForge service-principal role lookup failed' >&2; exit 1;
}
if ! jq -e --arg role_id "${service_role_id}" \
  'any(.[]; .role_id == $role_id and .scope == "global" and .is_active == true)' <<<"${assignments_response}" >/dev/null; then
  service_role_assignment="$(jq -cn --arg role_id "${service_role_id}" '{role_id:$role_id,scope:"global",scope_id:null}')"
  request_json POST "${contextforge_url}/v1/rbac/users/${service_principal_email}/roles" \
    "${service_role_assignment}" "${admin_token}" >/dev/null || {
    echo 'ContextForge service-principal role assignment failed' >&2; exit 1;
  }
fi

resources_response="$(request_get_json "${contextforge_url}/v1/resources?include_inactive=true" "${admin_token}")" || {
  echo 'ContextForge resource lookup failed' >&2; exit 1;
}
resource_id="$(jq -r --arg uri "${resource_uri}" 'first(.[] | select(.uri == $uri) | .id) // empty' <<<"${resources_response}")"
if [[ -z "${resource_id}" ]]; then
  ticket="$(cat "${root}/examples/tutorials/customer_support_copilot/samples/billing_currency_ticket.txt")"
  resource_payload="$(jq -cn --arg content "${ticket}" --arg uri "${resource_uri}" \
    '{resource:{name:"Billing currency ticket",uri:$uri,description:"Deterministic Customer Support Copilot fixture",mime_type:"text/plain",content:$content},visibility:"public"}')"
  resource_response=""
  for path in /v1/resources /resources; do
    resource_response="$(request_json POST "${contextforge_url}${path}" "${resource_payload}" "${admin_token}" 2>/dev/null)" && break
    resource_response="$(request_json POST "${contextforge_url}${path}" "$(jq -c 'del(.visibility)' <<<"${resource_payload}")" "${admin_token}" 2>/dev/null)" && break
  done
  resource_id="$(jq -er '.id // .resource.id' <<<"${resource_response}")" || {
    echo 'ContextForge resource registration failed' >&2; exit 1;
  }
fi

servers_response="$(request_get_json "${contextforge_url}/v1/servers?include_inactive=true" "${admin_token}")" || {
  echo 'ContextForge virtual-server lookup failed' >&2; exit 1;
}
server_id="$(jq -r --arg name "${server_name}" 'first(.[] | select(.name == $name) | .id) // empty' <<<"${servers_response}")"
if [[ -z "${server_id}" ]]; then
  server_payload="$(jq -cn --arg id "${resource_id}" --arg issuer "${keycloak_issuer}" --arg name "${server_name}" \
    '{server:{name:$name,description:"Customer Support Copilot MCP resources",associated_resources:[$id],oauth_enabled:true,oauth_config:{authorization_servers:[$issuer],client_id:"mcp-gateway"}},visibility:"public"}')"
  server_response=""
  for path in /v1/servers /servers; do
    server_response="$(request_json POST "${contextforge_url}${path}" "${server_payload}" "${admin_token}" 2>/dev/null)" && break
  done
  server_id="$(jq -er '.id // .server.id' <<<"${server_response}")" || {
    echo 'ContextForge virtual-server creation failed' >&2; exit 1;
  }
fi

access_token="$(curl --fail-with-body --silent --show-error --request POST \
  --cacert "${A2A_TUTORIAL_CA_CERT}" \
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
safe_names = ("iss", "aud", "azp", "exp", "preferred_username", "clientId", "email")
print(json.dumps({name: claims.get(name) for name in safe_names}, separators=(",", ":")))
')" || { echo 'Keycloak token payload is malformed' >&2; exit 1; }
jq -e --arg expected "${keycloak_issuer}" '.iss == $expected' <<<"${token_claims}" >/dev/null || {
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
jq -e --arg expected "${service_principal_email}" '.preferred_username == $expected' <<<"${token_claims}" >/dev/null || {
  echo 'Keycloak service-account username does not match the ContextForge principal' >&2; exit 1;
}
jq -e '.clientId == "mcp-agent" and .email == null' <<<"${token_claims}" >/dev/null || {
  echo 'Keycloak token is not an email-less mcp-agent service token' >&2; exit 1;
}
umask 077
{
  printf 'A2A_TUTORIAL_MCP_URL=http://contextforge:4444/servers/%s/mcp/\n' "${server_id}"
  printf 'A2A_TUTORIAL_MCP_TOKEN=%s\n' "${access_token}"
  printf 'A2A_TUTORIAL_MCP_SERVER_ID=%s\n' "${server_id}"
} >"${env_file}"
printf '%s\n' "${token_claims}" >"${env_file}.claims"
chmod 0644 "${env_file}" "${env_file}.claims"
echo 'ContextForge tutorial resource and OAuth access were configured'
