# Job Application Assistant

A standalone C++20 tutorial for HTTP+JSON A2A delegation.

```text
application_client -> A2A coordinator -> A2A profile analyst
```

The client sends a resume and job description to the coordinator, which delegates analysis to the specialist and returns structured analysis plus an application draft.

## Run individually with Docker Compose

From the repository root:

```text
docker compose -f examples/tutorials/job_application_assistant/compose.yaml up --build application-smoke
```

Compose streams the profile analyst, coordinator, and smoke-client logs with service prefixes and colors. After the smoke client prints the application draft, press `Ctrl+C` and clean up:

```text
docker compose -f examples/tutorials/job_application_assistant/compose.yaml down --remove-orphans
```

For a one-shot automated run:

```text
docker compose -f examples/tutorials/job_application_assistant/compose.yaml run --build --rm application-smoke
```

## Run individually on Linux

First [install the SDK for an individual local run](../README.md#build-the-sdk-for-an-individual-local-run), then build this tutorial:

```bash
cmake -S examples/tutorials/job_application_assistant \
  -B build-tutorials/job_application_assistant \
  -DCMAKE_PREFIX_PATH="$A2A_INSTALL_DIR"
cmake --build build-tutorials/job_application_assistant --parallel
```

Start the three processes in separate terminals from the repository root.

Profile analyst:

```bash
./build-tutorials/job_application_assistant/profile_analyst 127.0.0.1:8081
```

Coordinator:

```bash
A2A_TUTORIAL_SPECIALIST_URL=http://127.0.0.1:8081 \
./build-tutorials/job_application_assistant/application_coordinator 127.0.0.1:8080
```

Client:

```bash
./build-tutorials/job_application_assistant/application_client \
  --coordinator-url http://127.0.0.1:8080 \
  --resume-file examples/tutorials/job_application_assistant/samples/resume.txt \
  --job-file examples/tutorials/job_application_assistant/samples/job_description.txt
```

See the [production tutorials guide](../README.md#optional-model-configuration) for optional model configuration.
