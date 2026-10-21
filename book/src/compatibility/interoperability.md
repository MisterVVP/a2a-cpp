# Cross-SDK interoperability

The SDK's CI uses `a2aproject/a2a-python` at the configured `A2A_PYTHON_REF`.
Record that ref and the C++ commit when comparing results.

Two mixed-runtime gRPC fixture flows run through `scripts/run_mixed_runtime_interop.sh`:

- `python-server-cpp-client-interop`: a Python server with a C++ client.
- `cpp-server-python-client-interop`: a C++ server with a Python client.

These fixtures check messaging, task reads and cancellation, subscription events,
unknown-task errors and unsupported push operations. Their intentionally limited
capabilities do not represent every SDK operation or binding.

The separate `python-cross-sdk-interop` job installs/imports the Python SDK and
runs C++ gRPC integration tests through `scripts/run_python_cross_sdk_interop.sh`.
That environment check alone is not evidence of mixed-runtime exchange.

[Cross-binding SDK tests](bindings.md) and [upstream TCK](conformance.md) provide
different evidence. Inspect workflows and test fixtures for the version and
operation you depend on rather than assuming complete Go/Python API parity.
