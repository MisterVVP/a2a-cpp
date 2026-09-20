# Customer Support Copilot

This standalone C++20 downstream application demonstrates real HTTP+JSON A2A delegation.

```text
support_client --A2A--> support_coordinator --Agent Card discovery + A2A--> support_specialist
```

The client discovers the coordinator; the coordinator is both an A2A server and a client that discovers the separately running specialist. Input is support ticket; output is customer response and internal notes. Business contracts use structured `DataPart` values and structured artifacts.

## Host-native build and run

First install `a2a-cpp`, then configure this directory independently:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/a2a-install
cmake --build build --parallel
```

Start `support_specialist` on `8181`, then `support_coordinator` on `8180`, and invoke the client using a file under `samples/`. The repository-level `scripts/run_tutorials.sh` automates this with bounded Agent Card readiness checks and cleanup.

## Model backends

Deterministic mode is the default and performs no model HTTP request:

```bash
export A2A_TUTORIAL_MODEL_PROVIDER=deterministic
```

### OpenAI-compatible endpoints

For any OpenAI-compatible chat-completions endpoint:

```bash
export A2A_TUTORIAL_MODEL_PROVIDER=openai_compatible
export A2A_TUTORIAL_MODEL_BASE_URL=https://provider.example/v1
export A2A_TUTORIAL_MODEL_NAME=your-model
export A2A_TUTORIAL_MODEL_API_KEY=... # optional (for example, Ollama)
export A2A_TUTORIAL_MODEL_TIMEOUT_MS=30000
```

Use the same variables for every compatible service rather than provider-specific keys or configuration files. For the Gemini free tier, create a Google AI Studio API key, use `https://generativelanguage.googleapis.com/v1beta/openai` as the base URL, select a model available on the free tier, and assign the key to `A2A_TUTORIAL_MODEL_API_KEY`; free-tier availability and limits may change.

Prefix these settings with `A2A_TUTORIAL_COORDINATOR_` or `A2A_TUTORIAL_SPECIALIST_` to configure roles independently. Credentials are sent only as an Authorization header and are never logged. Do not commit keys; a ChatGPT subscription or product login is not an API credential.

For Ollama use `http://127.0.0.1:11434/v1` on the host. From Linux Compose use `http://host.docker.internal:11434/v1`; the Compose file provides the explicit `host-gateway` mapping. No model is downloaded automatically.

## Docker Compose

From the repository root:

```bash
docker compose -f examples/tutorials/customer_support_copilot/compose.yaml up --build -d
docker compose -f examples/tutorials/customer_support_copilot/compose.yaml exec support-coordinator ./support_client \
  --coordinator-url http://support-coordinator:8180 \
  --ticket-file samples/billing_currency_ticket.txt
docker compose -f examples/tutorials/customer_support_copilot/compose.yaml down --remove-orphans
```

Only the coordinator port is published. Service DNS is used for delegation, containers run as a non-root user, and deterministic mode is the default.

## Troubleshooting

Readiness is the Agent Card URL `/.well-known/agent-card.json`, not an arbitrary delay. Check `docker compose logs` if it does not become available. Bind endpoints and advertised public URLs are independently configurable. Requests and provider calls have bounded timeouts; invalid provider configuration fails at startup.
