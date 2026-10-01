# Customer Support Copilot

A standalone C++20 tutorial for A2A delegation backed by an OAuth-protected MCP resource.

```text
support_client -> A2A coordinator -> A2A specialist -> MCP -> ContextForge
                                                ^
                                                |
                                         Keycloak token
```

The tutorial is intentionally MCP-only: the client sends a `ticket_resource` URI, and the specialist always retrieves
the ticket through ContextForge. There is no file-input fallback.

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

See the [production tutorials guide](../README.md#optional-model-configuration) for optional model configuration.

## Troubleshooting Docker

To include infrastructure logs while debugging:

```text
docker compose -f examples/tutorials/customer_support_copilot/compose.yaml -f examples/tutorials/customer_support_copilot/compose.contextforge.yaml logs
```
