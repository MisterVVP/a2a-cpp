# Job Application Assistant

This standalone C++20 downstream application demonstrates real HTTP+JSON A2A delegation.

```text
User / application_client
    | A2A
    v
application_coordinator
    | Agent Card discovery + A2A
    v
profile_analyst
    | MCP resources/read
    v
official Python SDK MCP server (seeded local resource)
```

**A2A connects independent agents. MCP connects an individual agent to its tools and resources.** MCP is deliberately confined to the specialist and does not alter A2A discovery, transport, or messages between agents.

The client discovers the coordinator; the coordinator is both an A2A server and a client that discovers the separately running specialist. Input is resume and job description; output is structured fit analysis and a cover note. Business contracts use structured `DataPart` values and structured artifacts.

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
cmake -S examples/tutorials/job_application_assistant \
  -B build-tutorials/job_application_assistant \
  -DCMAKE_PREFIX_PATH="$A2A_INSTALL_DIR"

cmake --build build-tutorials/job_application_assistant --parallel
```

This builds `profile_analyst`, `application_coordinator`, and `application_client`.

### 3. Start the local MCP resource server

#### Terminal 1 — MCP server

From the repository root, create the environment and start the server:

```bash
python3 -m venv build-tutorials/mcp-venv
build-tutorials/mcp-venv/bin/python -m pip install --requirement examples/tutorials/mcp_server/requirements.txt
build-tutorials/mcp-venv/bin/python examples/tutorials/mcp_server/server.py \
  --fixture-set job_application \
  --fixture-root examples/tutorials/job_application_assistant/samples \
  --host 127.0.0.1 --port 8090
```

Leave this process running. The server uses the official MCP Python SDK and exposes deterministic fixtures without credentials.

#### Terminal 2 — Verify readiness and start the specialist

From the repository root, verify that the MCP server is ready:

```bash
curl --fail http://127.0.0.1:8090/health
```

After the readiness check succeeds, start the specialist with its bounded-timeout MCP endpoint in the same terminal:

```bash
A2A_TUTORIAL_MCP_URL=http://127.0.0.1:8090/mcp \
./build-tutorials/job_application_assistant/profile_analyst 127.0.0.1:8081
```

Verify that its Agent Card is available:

```bash
curl --fail http://127.0.0.1:8081/.well-known/agent-card.json
```

### 4. Start the coordinator

In another terminal, from the repository root:

```bash
A2A_TUTORIAL_SPECIALIST_URL=http://127.0.0.1:8081 \
./build-tutorials/job_application_assistant/application_coordinator 127.0.0.1:8080
```

Verify that its Agent Card is available:

```bash
curl --fail http://127.0.0.1:8080/.well-known/agent-card.json
```

### 5. Run the client

In another terminal, from the repository root:

```bash
./build-tutorials/job_application_assistant/application_client \
  --coordinator-url http://127.0.0.1:8080 \
  --resume-resource resume://candidate/alex \
  --job-file examples/tutorials/job_application_assistant/samples/job_description.txt
```

The identifier `resume://candidate/alex` is sent over A2A; only `profile_analyst` resolves it through MCP. The legacy file option remains available for migration and offline comparison.

Stop the MCP server, coordinator, and profile analyst in their respective terminals with `Ctrl+C` when finished.

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
docker compose -f examples/tutorials/job_application_assistant/compose.yaml up --build -d
docker compose -f examples/tutorials/job_application_assistant/compose.yaml exec application-coordinator ./application_client \
  --coordinator-url http://application-coordinator:8080 \
  --resume-resource resume://candidate/alex \
  --job-file samples/job_description.txt
docker compose -f examples/tutorials/job_application_assistant/compose.yaml down --remove-orphans
```

Only the coordinator port is published. Service DNS is used for delegation and containers run as a non-root user. If the four AI model variables are not set, Docker Compose uses the deterministic fallback.

## Replacing the local resource server

Set `A2A_TUTORIAL_MCP_URL` on `profile_analyst` to a trusted MCP Streamable HTTP endpoint that exposes the same resource URI contract. A production adapter can front Google Drive, Linear, or another system; keep authentication in runtime secret configuration, enforce TLS and access controls, and never pass provider credentials through A2A. The official-SDK local server remains the default CI path. The tutorial C++ client supports textual MCP resources; provider-specific tools and OAuth require an adapter.

The deliberately small tutorial client supports MCP protocol version `2025-06-18`, JSON responses, optional session IDs, and one `resources/read` result whose first item contains non-empty text. It does not implement SSE response parsing, OAuth, MCP tools, subscriptions, binary resources, or provider-specific discovery. The resource-mode coordinator receives the specialist's normalized analysis rather than the original resume; an external integration should extend that structured result when its drafting model needs more source evidence.

## Troubleshooting

Readiness is the Agent Card URL `/.well-known/agent-card.json`, not an arbitrary delay. Check `docker compose logs` if it does not become available. Bind endpoints and advertised public URLs are independently configurable. Requests and provider calls have bounded timeouts; invalid provider configuration fails at startup.
