#!/usr/bin/env bash
set -euo pipefail
contextforge_url="${A2A_TUTORIAL_CONTEXTFORGE_URL:-http://contextforge:4444}"
env_file="${A2A_TUTORIAL_MCP_ENV_FILE:?A2A_TUTORIAL_MCP_ENV_FILE is required}"
protocol_version="2025-11-25"
resource_uri="ticket://northstar/billing-currency"

server_id="$(sed -n 's/^A2A_TUTORIAL_MCP_SERVER_ID=//p' "${env_file}")"
token="$(sed -n 's/^A2A_TUTORIAL_MCP_TOKEN=//p' "${env_file}")"
[[ -n "${server_id}" && -n "${token}" ]] || {
  echo 'ContextForge MCP bootstrap output is incomplete' >&2
  exit 1
}

temporary_directory="$(mktemp -d)"
trap 'rm -rf "${temporary_directory}"' EXIT
headers="${temporary_directory}/unauthenticated.headers"
body="${temporary_directory}/authenticated.body"

unauthenticated_status="$(curl --silent --dump-header "${headers}" --output /dev/null --write-out '%{http_code}' \
  --request POST -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
  -H "MCP-Protocol-Version: ${protocol_version}" -H 'Mcp-Method: resources/read' -H "Mcp-Name: ${resource_uri}" \
  --data '{"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"ticket://northstar/billing-currency"}}' \
  "${contextforge_url}/servers/${server_id}/mcp/")"
[[ "${unauthenticated_status}" == 401 ]] || {
  echo "Unauthenticated MCP request returned HTTP ${unauthenticated_status}, expected 401" >&2
  exit 1
}
grep -Eiq '^www-authenticate:.*resource_metadata=' "${headers}" || {
  echo 'Unauthenticated MCP response did not advertise RFC 9728 resource metadata' >&2
  exit 1
}

authenticated_status="$(curl --silent --show-error --output "${body}" --write-out '%{http_code}' \
  --request POST -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
  -H "MCP-Protocol-Version: ${protocol_version}" -H 'Mcp-Method: resources/read' -H "Mcp-Name: ${resource_uri}" \
  -H "Authorization: Bearer ${token}" \
  --data '{"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"ticket://northstar/billing-currency","_meta":{"io.modelcontextprotocol/protocolVersion":"2025-11-25","io.modelcontextprotocol/clientInfo":{"name":"a2a-cpp-tutorial","version":"1.0.0"},"io.modelcontextprotocol/clientCapabilities":{}}}}' \
  "${contextforge_url}/servers/${server_id}/mcp/")"
if [[ ! "${authenticated_status}" =~ ^2[0-9][0-9]$ ]] || ! grep -Fq billing "${body}"; then
  echo "Authenticated MCP preflight failed with HTTP ${authenticated_status}" >&2
  cat "${body}" >&2
  exit 1
fi

echo 'ContextForge OAuth-protected MCP access was verified'
