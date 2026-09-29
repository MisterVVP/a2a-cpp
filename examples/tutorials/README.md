# Production tutorials

These standalone C++20 applications demonstrate real multi-process A2A delegation:

- [Job Application Assistant](job_application_assistant/README.md): a coordinator delegates resume and job analysis to a profile specialist.
- [Customer Support Copilot](customer_support_copilot/README.md): a coordinator delegates a support ticket to a specialist; the Docker flow adds OAuth-protected MCP resource access through Keycloak and ContextForge.

Both use the deterministic backend by default, so no external credentials are required.

## Run both with Docker Compose

Docker Compose 2.20.3 or newer is required. The same command works on Linux, macOS, PowerShell, CMD, and Git Bash with Docker Desktop on Windows; WSL is not required.

For an interactive run with Compose's service-prefixed, colored logs from the applications and smoke clients:

```text
docker compose -f examples/tutorials/compose.yaml up --build --attach profile-analyst --attach application-coordinator --attach application-smoke --attach support-specialist --attach support-coordinator --attach support-smoke --attach tutorials-smoke tutorials-smoke
```

Wait for `Tutorial Docker smoke tests passed`, then press `Ctrl+C` and clean up:

```text
docker compose -f examples/tutorials/compose.yaml down --volumes --remove-orphans
```

For an automated one-shot smoke run:

```text
docker compose -f examples/tutorials/compose.yaml run --build --rm tutorials-smoke
docker compose -f examples/tutorials/compose.yaml down --volumes --remove-orphans
```

On Linux/macOS, `./scripts/run_tutorial_docker_smoke.sh` is a convenience wrapper around the automated flow.

## Run both locally on Linux

From the repository root:

```bash
./scripts/run_tutorials.sh
```

This builds and installs the SDK, builds both tutorials as downstream projects, runs their deterministic smoke flows, and cleans up the agent processes.

## Build the SDK for an individual local run

Individual native runs use the installed CMake package:

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

Then follow the individual tutorial guide:

- [Job Application Assistant](job_application_assistant/README.md)
- [Customer Support Copilot](customer_support_copilot/README.md)

## Optional model configuration

Set these variables before starting the agents or Docker Compose to use an OpenAI-compatible endpoint:

```bash
export A2A_TUTORIAL_MODEL_PROVIDER=openai_compatible
export A2A_TUTORIAL_MODEL_BASE_URL=https://provider.example/v1
export A2A_TUTORIAL_MODEL_NAME=your-model
export A2A_TUTORIAL_MODEL_API_KEY=your-api-key
```

### Gemini free tier example

Gemini exposes an OpenAI-compatible API. For the Gemini API free tier:

```bash
export A2A_TUTORIAL_MODEL_PROVIDER=openai_compatible
export A2A_TUTORIAL_MODEL_BASE_URL=https://generativelanguage.googleapis.com/v1beta/openai/
export A2A_TUTORIAL_MODEL_NAME=gemini-3.8-flash
export A2A_TUTORIAL_MODEL_API_KEY=your-gemini-api-key
```

Use the equivalent environment-variable syntax for your shell on Windows. Do not commit API keys.
