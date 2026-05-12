# Community SDK Compliance Task Pack

This folder contains implementation-ready tasks for AI agents to bring this repository into alignment with community SDK verification expectations.

## Task Index

1. [Task 1: Add TCK CI Workflow](./task-1-tck-ci-workflow.md)
2. [Task 2: Add ITK Interop CI Workflow](./task-2-itk-ci-workflow.md)
3. [Task 3: Publish Compliance Evidence in Documentation](./task-3-compliance-evidence-docs.md)

## Execution order

Run tasks in the listed order. Task 1 establishes protocol conformance gating, Task 2 establishes interop verification, and Task 3 publishes auditable evidence.

## Definition of done

- TCK workflow exists, passes mandatory categories, and uploads artifacts.
- ITK workflow exists, runs deterministic interop scenarios, and uploads artifacts.
- Documentation clearly maps requirements to CI jobs and evidence links.

## Local reproduction for Task 1 (TCK CI)

Use the same deterministic workflow as `.github/workflows/tck.yml`:

```bash
# 1) Start the C++ SUT used by TCK.
./scripts/run_tck_sut.sh

# 2) Clone the pinned TCK ref (override TCK_REF as needed).
TCK_REPO=${TCK_REPO:-a2aproject/a2a-tck}
TCK_REF=${TCK_REF:-main}
git clone --depth 1 --branch "${TCK_REF}" "https://github.com/${TCK_REPO}.git" tck-repo
python3 -m venv tck-repo/.venv
source tck-repo/.venv/bin/activate
python3 -m pip install --upgrade pip
if [[ -f tck-repo/requirements.txt ]]; then
  python3 -m pip install -r tck-repo/requirements.txt
fi
python3 -m pip install python-dotenv pyopenssl

# 3) Run mandatory category against the local SUT.
mkdir -p tck-artifacts/reports tck-artifacts/logs
if [[ -x tck-repo/scripts/run_tck.sh ]]; then
  tck-repo/scripts/run_tck.sh \
    --category mandatory \
    --sut-endpoint "127.0.0.1:50061" \
    --output-dir tck-artifacts/reports \
    | tee tck-artifacts/logs/tck-run.log
elif [[ -f tck-repo/run_tck.py ]]; then
  python3 tck-repo/run_tck.py \
    --sut-url "http://127.0.0.1:50061" \
    --category mandatory \
    --transports grpc \
    --transport-strategy prefer_grpc \
    | tee tck-artifacts/logs/tck-run.log
else
  echo "Adjust this command to the pinned TCK entrypoint for your selected ref."
fi

# 4) Stop the SUT and preserve logs.
./scripts/stop_tck_sut.sh
```

Expected artifacts:
- `build-tck/tck-sut.log`
- `tck-artifacts/reports/*`
- `tck-artifacts/logs/tck-run.log`
