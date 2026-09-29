# Customer Support Copilot

A standalone C++20 tutorial for A2A delegation from a support coordinator to a specialist.

```text
support_client -> A2A coordinator -> A2A specialist
```

## Run locally on Linux

Local mode reads the ticket from a file and does not require MCP or OAuth:

```bash
./scripts/run_tutorials.sh
```

The script builds and installs the SDK, builds both tutorials as downstream CMake projects, runs their deterministic smoke flows, and cleans up the agent processes.

## Run with Docker Compose

Docker mode exercises the production-style resource flow:

```text
Keycloak -> access token -> A2A specialist -> ContextForge -> ticket resource
```

Docker owns all generated secrets, TLS material, and MCP runtime state through named volumes, so no host Bash, OpenSSL, jq, or WSL is required. Docker Compose 2.20.3 or newer is required.

From the repository root, run this on Linux, macOS, PowerShell, CMD, or Git Bash:

```text
docker compose -f examples/tutorials/customer_support_copilot/compose.yaml -f examples/tutorials/customer_support_copilot/compose.contextforge.yaml run --build --rm support-smoke
docker compose -f examples/tutorials/customer_support_copilot/compose.yaml -f examples/tutorials/customer_support_copilot/compose.contextforge.yaml down --volumes --remove-orphans
```

The Compose flow generates ephemeral secrets and certificates, starts Keycloak and ContextForge, bootstraps a least-privilege MCP principal and ticket resource, verifies OAuth-protected MCP `2025-11-25` `resources/read`, and runs the A2A client.

To run both production tutorials in Docker:

```text
docker compose -f examples/tutorials/compose.yaml run --build --rm tutorials-smoke
docker compose -f examples/tutorials/compose.yaml down --volumes --remove-orphans
```

## Optional model configuration

The deterministic backend is used unless these variables are set:

```bash
export A2A_TUTORIAL_MODEL_PROVIDER=openai_compatible
export A2A_TUTORIAL_MODEL_BASE_URL=https://provider.example/v1
export A2A_TUTORIAL_MODEL_NAME=your-model
export A2A_TUTORIAL_MODEL_API_KEY=your-api-key
```

Use the equivalent environment-variable syntax for your shell on Windows. Do not commit API keys.

## Troubleshooting

Agent readiness is `/.well-known/agent-card.json`; ContextForge readiness is `/health`. Inspect the Compose logs on failure:

```text
docker compose -f examples/tutorials/customer_support_copilot/compose.yaml -f examples/tutorials/customer_support_copilot/compose.contextforge.yaml logs
```
