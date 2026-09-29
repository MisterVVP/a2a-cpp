#!/usr/bin/env bash
set -euo pipefail
runtime_dir="${A2A_TUTORIAL_RUNTIME_DIR:-/runtime}"
tls_dir="${runtime_dir}/tls"
secrets_file="${runtime_dir}/secrets.env"
mkdir -p "${tls_dir}"

if [[ -s "${secrets_file}" && -s "${tls_dir}/ca.crt" && -s "${tls_dir}/keycloak.crt" && -s "${tls_dir}/keycloak.key" ]]; then
  echo 'Reusing existing tutorial runtime secrets and TLS material'
  exit 0
fi

rm -f "${secrets_file}" "${tls_dir}/ca.crt" "${tls_dir}/keycloak.crt" "${tls_dir}/keycloak.key"

random_secret() {
  openssl rand -hex 32
}

random_encryption_key() {
  openssl rand -base64 32 | tr '+/' '-_' | tr -d '\n'
}

mcp_agent_secret="$(random_secret)"
oidc_secret="$(random_secret)"
admin_password="$(random_secret)"
jwt_secret="$(random_secret)"
auth_encryption_secret="$(random_encryption_key)"

umask 077
cat >"${secrets_file}" <<EOF
export A2A_TUTORIAL_MCP_AGENT_SECRET=${mcp_agent_secret}
export A2A_TUTORIAL_CONTEXTFORGE_OIDC_SECRET=${oidc_secret}
export A2A_TUTORIAL_CONTEXTFORGE_ADMIN_PASSWORD=${admin_password}
export A2A_TUTORIAL_CONTEXTFORGE_JWT_SECRET=${jwt_secret}
export A2A_TUTORIAL_CONTEXTFORGE_AUTH_ENCRYPTION_SECRET=${auth_encryption_secret}
EOF

ca_key="$(mktemp)"
trap 'rm -f "${ca_key}"' EXIT
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 2 \
  -subj '/CN=a2a-cpp tutorial CA' -keyout "${ca_key}" -out "${tls_dir}/ca.crt" >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -sha256 -subj '/CN=keycloak' \
  -keyout "${tls_dir}/keycloak.key" -out "${tls_dir}/keycloak.csr" >/dev/null 2>&1
printf 'subjectAltName=DNS:keycloak\nextendedKeyUsage=serverAuth\n' >"${tls_dir}/keycloak.ext"
openssl x509 -req -sha256 -days 1 -in "${tls_dir}/keycloak.csr" \
  -CA "${tls_dir}/ca.crt" -CAkey "${ca_key}" -CAcreateserial \
  -extfile "${tls_dir}/keycloak.ext" -out "${tls_dir}/keycloak.crt" >/dev/null 2>&1

rm -f "${tls_dir}/keycloak.csr" "${tls_dir}/keycloak.ext" "${tls_dir}/ca.srl"
chmod 0644 "${secrets_file}" "${tls_dir}/ca.crt" "${tls_dir}/keycloak.crt" "${tls_dir}/keycloak.key"
echo 'Tutorial runtime secrets and TLS material were generated'
