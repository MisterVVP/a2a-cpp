# Customer Support Copilot

A standalone C++20 tutorial for A2A delegation from a support coordinator to a specialist.

```text
support_client -> A2A coordinator -> A2A specialist
```

Local mode reads the ticket from a file. Docker mode exercises the production-style MCP/OAuth boundary:

```text
Keycloak -> access token -> A2A specialist -> ContextForge -> ticket resource
```

## Run individually with Docker Compose

Docker Compose 2.20.3 or newer is required. From the repository root:

```text
docker compose -f examples/tutorials/customer_support_copilot/compose.yaml -f examples/tutorials/customer_support_copilot/compose.contextforge.yaml up --build --attach support-specialist --attach support-coordinator --attach support-smoke support-smoke
```

Compose streams only the specialist, coordinator, and smoke-client logs; Keycloak and ContextForge remain available without flooding the tutorial output. The flow generates ephemeral secrets and certificates, bootstraps a least-privilege MCP principal and ticket resource, verifies OAuth-protected MCP `2025-11-25` `resources/read`, and runs the A2A client.

After the smoke client completes, press `Ctrl+C` and clean up:

```text
docker compose -f examples/tutorials/customer_support_copilot/compose.yaml -f examples/tutorials/customer_support_copilot/compose.contextforge.yaml down --volumes --remove-orphans
```

For a one-shot automated run:

```text
docker compose -f examples/tutorials/customer_support_copilot/compose.yaml -f examples/tutorials/customer_support_copilot/compose.contextforge.yaml run --build --rm support-smoke
```

## Run individually on Linux

First [install the SDK for an individual local run](../README.md#build-the-sdk-for-an-individual-local-run), then build this tutorial:

```bash
cmake -S examples/tutorials/customer_support_copilot \
  -B build-tutorials/customer_support_copilot \
  -DCMAKE_PREFIX_PATH="$A2A_INSTALL_DIR"
cmake --build build-tutorials/customer_support_copilot --parallel
```

Start the three processes in separate terminals from the repository root.

Support specialist:

```bash
./build-tutorials/customer_support_copilot/support_specialist 127.0.0.1:8181
```

Coordinator:

```bash
A2A_TUTORIAL_SPECIALIST_URL=http://127.0.0.1:8181 \
./build-tutorials/customer_support_copilot/support_coordinator 127.0.0.1:8180
```

Client:

```bash
./build-tutorials/customer_support_copilot/support_client \
  --coordinator-url http://127.0.0.1:8180 \
  --ticket-file examples/tutorials/customer_support_copilot/samples/billing_currency_ticket.txt
```

See the [production tutorials guide](../README.md#optional-model-configuration) for optional model configuration.

## Troubleshooting Docker

To include infrastructure logs while debugging:

```text
docker compose -f examples/tutorials/customer_support_copilot/compose.yaml -f examples/tutorials/customer_support_copilot/compose.contextforge.yaml logs
```
