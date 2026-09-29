# Job Application Assistant

A standalone C++20 tutorial for HTTP+JSON A2A delegation.

```text
application_client -> A2A coordinator -> A2A profile analyst
```

The client sends a resume and job description to the coordinator, which delegates analysis to the specialist and returns structured analysis plus an application draft.

## Run locally on Linux

```bash
./scripts/run_tutorials.sh
```

The script builds and installs the SDK, builds both tutorials as downstream CMake projects, runs their deterministic smoke flows, and cleans up the agent processes.

## Run with Docker Compose

Docker Compose runs the specialist, coordinator, and client entirely in Linux containers. Docker Desktop on Windows is supported without WSL.

From the repository root:

```text
docker compose -f examples/tutorials/job_application_assistant/compose.yaml run --build --rm application-smoke
docker compose -f examples/tutorials/job_application_assistant/compose.yaml down --remove-orphans
```

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

Agent readiness is `/.well-known/agent-card.json`. Inspect logs with:

```text
docker compose -f examples/tutorials/job_application_assistant/compose.yaml logs
```
