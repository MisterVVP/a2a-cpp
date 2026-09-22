# Customer Support Copilot

This standalone C++20 downstream application demonstrates real HTTP+JSON A2A delegation.

```text
support_client --A2A--> support_coordinator --Agent Card discovery + A2A--> support_specialist
```

The client discovers the coordinator; the coordinator is both an A2A server and a client that discovers the separately running specialist. Input is support ticket; output is customer response and internal notes. Business contracts use structured `DataPart` values and structured artifacts.

## Host-native build and run on Linux

The tutorial is a standalone downstream CMake project. It uses `find_package(a2a_cpp CONFIG REQUIRED)`, so the SDK must first be installed to a local prefix. Pointing `CMAKE_PREFIX_PATH` at the repository or tutorial source directory is not sufficient.

Run the following commands from the `a2a-cpp` repository root.

### 1. Build and install the SDK locally

```bash
export A2A_INSTALL_DIR="$PWD/build-tutorials/install"

cmake -S . -B build-tutorials/sdk \
  -DA2A_ENABLE_TESTING=OFF \
  -DA2A_BUILD_EXAMPLES=OFF \
  -DA2A_ENABLE_POSTGRES_STORE=OFF \
  -DCMAKE_INSTALL_PREFIX="$A2A_INSTALL_DIR"

cmake --build build-tutorials/sdk --parallel
cmake --install build-tutorials/sdk
```

If the SDK source changes, rebuild and reinstall it before rebuilding the tutorial.

### 2. Build this tutorial

```bash
cmake -S examples/tutorials/customer_support_copilot \
  -B build-tutorials/customer_support_copilot \
  -DCMAKE_PREFIX_PATH="$A2A_INSTALL_DIR"

cmake --build build-tutorials/customer_support_copilot --parallel
```

This builds `support_specialist`, `support_coordinator`, and `support_client`.

### 3. Start the specialist

In the first terminal, from the repository root:

```bash
./build-tutorials/customer_support_copilot/support_specialist 127.0.0.1:8181
```

Verify that its Agent Card is available:

```bash
curl --fail http://127.0.0.1:8181/.well-known/agent-card.json
```

### 4. Start the coordinator

In a second terminal, from the repository root:

```bash
A2A_TUTORIAL_SPECIALIST_URL=http://127.0.0.1:8181 \
./build-tutorials/customer_support_copilot/support_coordinator 127.0.0.1:8180
```

Verify that its Agent Card is available:

```bash
curl --fail http://127.0.0.1:8180/.well-known/agent-card.json
```

### 5. Run the client

In a third terminal, from the repository root:

```bash
./build-tutorials/customer_support_copilot/support_client \
  --coordinator-url http://127.0.0.1:8180 \
  --ticket-file examples/tutorials/customer_support_copilot/samples/billing_currency_ticket.txt
```

Stop the coordinator and specialist with `Ctrl+C` when finished.

Without the AI model configuration below, both agents use the deterministic fallback backend and print a warning at startup. The repository-level `scripts/run_tutorials.sh` builds and runs both tutorials with bounded Agent Card readiness checks and automatic cleanup.

## AI model configuration

Configure an AI model to run the tutorial in agentic mode. Set these four environment variables before starting the host-native agents or Docker Compose:

```bash
export A2A_TUTORIAL_MODEL_PROVIDER=openai_compatible
export A2A_TUTORIAL_MODEL_BASE_URL=https://provider.example/v1
export A2A_TUTORIAL_MODEL_NAME=your-model
export A2A_TUTORIAL_MODEL_API_KEY=your-api-key
```

If they are not configured, the tutorial falls back to the deterministic backend. Deterministic mode is intended for offline smoke testing and does not make AI model requests. The model request timeout defaults to 30 seconds.

Credentials are sent only as an Authorization header and are never logged. Do not commit API keys.

### Gemini free tier example

Create an API key in Google AI Studio, then export:

```bash
export A2A_TUTORIAL_MODEL_PROVIDER=openai_compatible
export A2A_TUTORIAL_MODEL_BASE_URL=https://generativelanguage.googleapis.com/v1beta/openai
export A2A_TUTORIAL_MODEL_NAME=gemini-3.8-flash
export A2A_TUTORIAL_MODEL_API_KEY=<YOUR_API_KEY>
```

With these variables exported, use the same host-native commands above or the Docker Compose commands below. Free-tier availability and rate limits may change; see the [Gemini API pricing](https://ai.google.dev/gemini-api/docs/pricing) and [OpenAI compatibility](https://ai.google.dev/gemini-api/docs/openai) documentation.

## Docker Compose

From the repository root:

```bash
docker compose -f examples/tutorials/customer_support_copilot/compose.yaml up --build -d
docker compose -f examples/tutorials/customer_support_copilot/compose.yaml exec support-coordinator ./support_client \
  --coordinator-url http://support-coordinator:8180 \
  --ticket-file samples/billing_currency_ticket.txt
docker compose -f examples/tutorials/customer_support_copilot/compose.yaml down --remove-orphans
```

Only the coordinator port is published. Service DNS is used for delegation and containers run as a non-root user. If the four AI model variables are not set, Docker Compose uses the deterministic fallback.

## Troubleshooting

Readiness is the Agent Card URL `/.well-known/agent-card.json`, not an arbitrary delay. Check `docker compose logs` if it does not become available. Bind endpoints and advertised public URLs are independently configurable. Requests and provider calls have bounded timeouts; invalid provider configuration fails at startup.
