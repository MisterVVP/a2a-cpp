# Python ↔ C++ interoperability validation

_Last updated: 2026-05-08._

This document explains exactly how interoperability validation currently works in this repository, what it proves, and what it does **not** prove yet.

## Current CI entrypoint

- Workflow job: `python-cross-sdk-interop` in `.github/workflows/ci.yml`.
- Script: `scripts/run_python_cross_sdk_interop.sh`.
- Python source input: `a2aproject/a2a-python` at `A2A_PYTHON_REF` (currently `main`).

## How `scripts/run_python_cross_sdk_interop.sh` works

The script is intentionally split into deterministic phases so failures are easy to triage:

1. **Fetch upstream SDK input**
   - Clones `a2aproject/a2a-python` at the exact `A2A_PYTHON_REF` provided by CI.
   - This keeps Python-side test input explicit and reproducible for each run.
2. **Prepare isolated Python environment**
   - Creates a fresh virtual environment under `build-python-interop/venv`.
   - Installs the checked-out Python SDK into that venv.
   - Runs an import/version smoke check to ensure the expected Python package is installed.
3. **Build C++ side from a clean interop tree**
   - Configures and builds `a2a-cpp` with CMake + Ninja under `build-python-interop/cpp`.
   - Isolates interop builds from default local build directories.
4. **Run protocol-focused gRPC integration tests**
   - Executes `ctest` filtered to `GrpcTransportIntegrationTest.*`.
   - Validates deterministic gRPC behavior in C++ transport/server contracts:
     - core lifecycle RPC + streaming round-trip,
     - subscription event flow + completion semantics,
     - unsupported-method error behavior,
     - unknown-task failure-path behavior.

## What interoperability this validates today

Current validation provides **two useful signals**:

1. **Environment-level compatibility signal**
   - The repository can pull, install, and import `a2a-python` in CI under pinned inputs.
2. **Protocol-contract compatibility signal (C++ side)**
   - C++ gRPC transport behavior is exercised with deterministic integration tests against C++ server transport.

## What this does *not* validate yet (important)

You are correct: true cross-SDK interop evidence should include **mixed-runtime execution**:

- Python server ↔ C++ client
- C++ server ↔ Python client

The CI workflow now runs both mixed-runtime exchanges:

- `python-server-cpp-client-interop`: Python gRPC fixture server with C++ interop client checks.
- `cpp-server-python-client-interop`: C++ gRPC fixture server with Python interop client checks.

## Mixed-runtime contract assertions

Both scenarios assert the same behavioral contract set with fixed ports and deterministic startup/teardown:

- core lifecycle: `SendMessage`, `GetTask`, `CancelTask`
- streaming semantics: `SubscribeTask` emits events and completes
- error mapping/failure path: unknown task lookup returns a transport error
- unsupported-method behavior for push-config in gRPC fixture path

The orchestration entrypoint is `scripts/run_mixed_runtime_interop.sh`, which uses explicit `SCENARIO` selection and process cleanup traps for deterministic CI behavior.

