# Job Application Assistant

A standalone C++20 tutorial for HTTP+JSON A2A delegation.

```text
application_client -> A2A coordinator -> A2A profile analyst
```

The client sends a resume and job description to the coordinator, which delegates analysis to the specialist and returns structured analysis plus an application draft.

## Run locally on Linux

From the repository root:

```bash
./scripts/run_tutorials.sh
```

This builds and installs the SDK, builds both standalone tutorials as downstream CMake projects, runs their deterministic smoke flows, and cleans up the local agent processes.

## Run with Docker Compose

To run only this tutorial in Docker:

```bash
docker compose -f examples/tutorials/job_application_assistant/compose.yaml up --build -d

docker compose -f examples/tutorials/job_application_assistant/compose.yaml exec application-coordinator ./application_client \
  --coordinator-url http://application-coordinator:8080 \
  --resume-file samples/resume.txt \
  --job-file samples/job_description.txt

docker compose -f examples/tutorials/job_application_assistant/compose.yaml down --remove-orphans
```

Only the coordinator port is published; delegation uses Docker service DNS.

To run the repository Docker smoke for both tutorials instead:

```bash
./scripts/run_tutorial_docker_smoke.sh
```

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

Agent readiness is exposed at `/.well-known/agent-card.json`. For Docker runs, use:

```bash
docker compose -f examples/tutorials/job_application_assistant/compose.yaml logs
```
