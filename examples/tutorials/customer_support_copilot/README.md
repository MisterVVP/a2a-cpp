# Customer Support Copilot

A standalone C++20 tutorial for A2A delegation from a support coordinator to a specialist.

```text
support_client -> A2A coordinator -> A2A specialist
```

The tutorial has two supported modes: local file input and Docker Compose with MCP + OAuth.

## Run locally on Linux

Local mode reads the ticket from a file and does not require MCP, OAuth, or external credentials.

From the repository root:

```bash
./scripts/run_tutorials.sh
```

This builds and installs the SDK, builds both standalone tutorials as downstream CMake projects, runs their deterministic smoke flows, and cleans up the local agent processes. The Customer Support run uses:

```text
examples/tutorials/customer_support_copilot/samples/billing_currency_ticket.txt
```

## Run with Docker Compose

Docker mode exercises the production-style resource flow:

```text
Keycloak -> access token -> A2A specialist -> ContextForge -> ticket resource
```

Use the repository smoke script:

```bash
./scripts/run_tutorial_docker_smoke.sh
```

This is the supported Docker Compose entry point. It runs both tutorials; for Customer Support it:

- builds the coordinator and specialist containers;
- generates ephemeral secrets and a local TLS certificate authority;
- starts Keycloak and ContextForge using `compose.yaml` plus `compose.contextforge.yaml`;
- bootstraps the ticket resource, virtual MCP server, and least-privilege service principal;
- verifies unauthenticated rejection and authenticated MCP `2025-11-25` `resources/read`;
- runs the A2A request and removes the temporary containers, secrets, and certificates.

Do not run `compose.contextforge.yaml` by itself: it expects the generated TLS files, secrets, MCP environment file, and bootstrap state created by the smoke script.

## Optional model configuration

Both local and Docker runs use the deterministic backend unless these variables are set:

```bash
export A2A_TUTORIAL_MODEL_PROVIDER=openai_compatible
export A2A_TUTORIAL_MODEL_BASE_URL=https://provider.example/v1
export A2A_TUTORIAL_MODEL_NAME=your-model
export A2A_TUTORIAL_MODEL_API_KEY=your-api-key
```

Do not commit API keys.

## Troubleshooting

Agent readiness is exposed at `/.well-known/agent-card.json`; ContextForge readiness is `/health`. On Docker failures, inspect the logs printed by `run_tutorial_docker_smoke.sh`.
