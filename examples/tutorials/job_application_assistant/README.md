# Job Application Assistant

This standalone C++20 downstream application demonstrates real HTTP+JSON A2A delegation.

```text
application_client --A2A--> application_coordinator --Agent Card discovery + A2A--> profile_analyst
```

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

### 3. Start the profile analyst

In the first terminal, from the repository root:

```bash
A2A_TUTORIAL_MODEL_PROVIDER=deterministic \
./build-tutorials/job_application_assistant/profile_analyst 127.0.0.1:8081
```

Verify that its Agent Card is available:

```bash
curl --fail http://127.0.0.1:8081/.well-known/agent-card.json
```

### 4. Start the coordinator

In a second terminal, from the repository root:

```bash
A2A_TUTORIAL_MODEL_PROVIDER=deterministic \
A2A_TUTORIAL_SPECIALIST_URL=http://127.0.0.1:8081 \
./build-tutorials/job_application_assistant/application_coordinator 127.0.0.1:8080
```

Verify that its Agent Card is available:

```bash
curl --fail http://127.0.0.1:8080/.well-known/agent-card.json
```

### 5. Run the client

In a third terminal, from the repository root:

```bash
./build-tutorials/job_application_assistant/application_client \
  --coordinator-url http://127.0.0.1:8080 \
  --resume-file examples/tutorials/job_application_assistant/samples/resume.txt \
  --job-file examples/tutorials/job_application_assistant/samples/job_description.txt
```

Stop the coordinator and profile analyst with `Ctrl+C` when finished.

The repository-level `scripts/run_tutorials.sh` builds and runs both tutorials with bounded Agent Card readiness checks and automatic cleanup. The commands above are intended for running only this tutorial manually on a Linux host.

## Model backends

### Without AI (default)

No model configuration is required. Native runs and Docker Compose fall back to the deterministic backend, which makes no model HTTP requests. To override inherited model settings explicitly, set:

```bash
export A2A_TUTORIAL_MODEL_PROVIDER=deterministic
```

### With an OpenAI-compatible model

Export the shared settings before starting the native processes or running the Docker Compose commands below. Compose forwards them to both tutorial agents and uses the displayed defaults only when a setting is absent.

```bash
export A2A_TUTORIAL_MODEL_PROVIDER=openai_compatible
export A2A_TUTORIAL_MODEL_BASE_URL=https://provider.example/v1
export A2A_TUTORIAL_MODEL_NAME=your-model
export A2A_TUTORIAL_MODEL_API_KEY=your-api-key # omit only for endpoints that do not require one
export A2A_TUTORIAL_MODEL_TIMEOUT_MS=30000
```

Prefix these settings with `A2A_TUTORIAL_COORDINATOR_` or `A2A_TUTORIAL_SPECIALIST_` for role-specific native configuration. Credentials are sent only as an Authorization header and are never logged. Do not commit keys; a ChatGPT subscription or product login is not an API credential.

For Ollama use `http://127.0.0.1:11434/v1` on the host. From Linux Compose use `http://host.docker.internal:11434/v1`; the Compose file provides the explicit `host-gateway` mapping. No model is downloaded automatically.

#### Gemini free tier docker compose example
For the Gemini free tier, create a Google AI Studio API key, use `https://generativelanguage.googleapis.com/v1beta/openai` as the base URL, set `A2A_TUTORIAL_MODEL_NAME` to a model available on the free tier, and put the key in `A2A_TUTORIAL_MODEL_API_KEY`. A model name is required; free-tier availability and limits may change.

1. Start agents.  
```bash
export A2A_TUTORIAL_MODEL_PROVIDER=openai_compatible && \
export A2A_TUTORIAL_MODEL_BASE_URL=https://generativelanguage.googleapis.com/v1beta/openai && \
export A2A_TUTORIAL_MODEL_API_KEY=<YOUR_API_KEY> && \
export A2A_TUTORIAL_MODEL_NAME=gemini-3.8-flash && \
docker compose -f examples/tutorials/job_application_assistant/compose.yaml up --build -d
```
2. Send a new request.  
```bash
docker compose -f examples/tutorials/job_application_assistant/compose.yaml exec application-coordinator ./application_client \
  --coordinator-url http://application-coordinator:8080 \
  --resume-file samples/resume.txt \
  --job-file samples/job_description.txt
```
3. Stop agents  
```bash
docker compose -f examples/tutorials/job_application_assistant/compose.yaml down --remove-orphans
```

## Docker Compose

From the repository root:

```bash
docker compose -f examples/tutorials/job_application_assistant/compose.yaml up --build -d
docker compose -f examples/tutorials/job_application_assistant/compose.yaml exec application-coordinator ./application_client \
  --coordinator-url http://application-coordinator:8080 \
  --resume-file samples/resume.txt \
  --job-file samples/job_description.txt
docker compose -f examples/tutorials/job_application_assistant/compose.yaml down --remove-orphans
```

Only the coordinator port is published. Service DNS is used for delegation, containers run as a non-root user, and deterministic mode is the default.

## Troubleshooting

Readiness is the Agent Card URL `/.well-known/agent-card.json`, not an arbitrary delay. Check `docker compose logs` if it does not become available. Bind endpoints and advertised public URLs are independently configurable. Requests and provider calls have bounded timeouts; invalid provider configuration fails at startup.
