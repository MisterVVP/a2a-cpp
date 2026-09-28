# Customer Support Copilot

This standalone C++20 tutorial demonstrates a coordinator delegating a structured support request to a specialist over A2A. It has two input modes.

## Simple file mode

```text
ticket file -> support_client -> A2A coordinator -> A2A specialist
```

This mode is deterministic, requires no credentials, and is used by the host-native tutorial smoke test. Build and run all host tutorials from the repository root:

```bash
./scripts/run_tutorials.sh
```

The individual client accepts a local ticket with `--ticket-file`:

```bash
support_client --coordinator-url http://127.0.0.1:8180 \
  --ticket-file samples/billing_currency_ticket.txt
```

## Production-style MCP and OAuth mode

```text
Keycloak
   |
OAuth access token
   v
support_client -> A2A coordinator -> A2A specialist -> ContextForge -> ticket resource
```

A2A is used only for agent-to-agent communication. MCP is used only by the specialist to read the ticket resource, and OAuth protects that MCP boundary. ContextForge is the third-party MCP implementation; Keycloak supplies a short-lived machine-to-machine token through `client_credentials`. The C++ tutorial consumes an already-issued access token and does not implement OAuth.

The Docker smoke test generates ephemeral secrets and a short-lived local certificate authority, runs Keycloak over verified HTTPS, imports the local realm, configures ContextForge external-token validation, registers the deterministic ticket as a public resource, creates a virtual MCP server, verifies both unauthenticated rejection and authenticated access, and then runs the resource flow:

```bash
./scripts/run_tutorial_docker_smoke.sh
```

The resource request is:

```bash
support_client --coordinator-url http://support-coordinator:8180 \
  --ticket-resource ticket://northstar/billing-currency
```

The specialist alone receives `A2A_TUTORIAL_MCP_URL` and `A2A_TUTORIAL_MCP_TOKEN`; neither value is forwarded in A2A messages or Agent Cards. The client sends a stateless MCP `2025-11-25` `resources/read` request with its bearer token. ContextForge validates the Keycloak signature, issuer, expiry, token type, and `mcp-gateway` audience.

The local Docker environment uses its ephemeral CA solely for deterministic development and CI; the CA and Keycloak server key are deleted during cleanup. TLS verification remains enabled. Production deployments must use certificates and secrets from a real secret-management system rather than process environment files. No SaaS account or checked-in secret is required.

## Model configuration

Without model configuration, both agents use the deterministic backend. Optional model settings use the existing `A2A_TUTORIAL_MODEL_*` environment variables documented by the other tutorials.

## Troubleshooting

Agent readiness is exposed at `/.well-known/agent-card.json`; ContextForge readiness is exposed at `/health`. All bootstrap polling and C++ requests are bounded. Check `docker compose logs` when startup fails. Resource mode reports missing MCP configuration, authorization failures, unavailable services, and invalid MCP responses without printing the bearer token.
