# Conformance and validation

The SDK implements A2A protocol `1.0` across HTTP+JSON, JSON-RPC and gRPC.
[Binding compatibility](bindings.md), [version negotiation](protocol-versions.md)
and [security integration](../auth/security.md) describe the public contracts.

## Release validation snapshot

For v0.5.2 release preparation, unmodified upstream TCK
`5daf0144b4a4adbdefb62180739b13c5bd7d8196` ran on all three transports with each store:

| Evidence | In-memory | PostgreSQL |
|---|---|---|
| Executed upstream tests | 249 passed, 16 conditional skips | 249 passed, 16 conditional skips |
| Mandatory requirement coverage | 80.9% | 80.9% |
| Overall requirement coverage | 80.0% | 80.0% |

Registered requirements without executable upstream fixtures remain `NOT TESTED`.
Independent SDK C++ tests verify additional binding, version and security behavior;
they do not increase upstream TCK percentages. This snapshot is not a claim of
full TCK compliance. Review reports for the specific commit you deploy.

## Reproduce the upstream suite


Use the same deterministic workflow as `.github/workflows/tck.yml`:

```bash
# Check out the TCK and prepare Python dependencies.
TCK_REPO=${TCK_REPO:-a2aproject/a2a-tck}
TCK_REF=${TCK_REF:-main}
git clone --depth 1 --branch "${TCK_REF}" "https://github.com/${TCK_REPO}.git" tck-repo
python3 -m venv tck-repo/.venv
source tck-repo/.venv/bin/activate
python3 -m pip install --upgrade pip
python3 -m pip install -e ./tck-repo python-dotenv pyopenssl

# Run the repository entrypoint from the repository root.
export TCK_WORKDIR=tck-repo
export TCK_TRANSPORTS=grpc,jsonrpc,http_json
export A2A_TCK_REQUIRED_EXTENSIONS=urn:a2a:tck:required-extension
export A2A_TCK_STORE_BACKEND=inmemory
export TCK_REPORT_DIR=tck-artifacts/reports/inmemory
export TCK_LOG_DIR=tck-artifacts/logs/inmemory
./scripts/run_tck_sut.sh
trap './scripts/stop_tck_sut.sh' EXIT
./scripts/run_tck_mandatory.sh
./scripts/stop_tck_sut.sh

# Repeat using a running PostgreSQL instance.
export A2A_TCK_STORE_BACKEND=postgres
export A2A_TCK_POSTGRES_DSN=postgresql://a2a:a2a@127.0.0.1:5432/a2a
export A2A_TCK_POSTGRES_SCHEMA=tck_postgres
export TCK_REPORT_DIR=tck-artifacts/reports/postgres
export TCK_LOG_DIR=tck-artifacts/logs/postgres
./scripts/run_tck_sut.sh
./scripts/run_tck_mandatory.sh
./scripts/stop_tck_sut.sh
trap - EXIT
```

Expected artifacts:
- `build-tck/tck-sut.log`
- `tck-artifacts/reports/{inmemory,postgres}/compatibility.{json,html}`
- `tck-artifacts/logs/{inmemory,postgres}/tck-run.log`

The mandatory runner executes the upstream TCK entrypoint and preserves its
reports. It adds no repository-specific test module or requirement report gate.
Use `scripts/summarize_tck_report.py` to inspect failed, skipped, and untested
requirements. Independent C++ SDK tests run through CTest and are reported
separately from upstream TCK compliance.


## CI evidence

The [TCK workflow](https://github.com/MisterVVP/a2a-cpp/actions/workflows/tck.yml)
uploads separate backend reports and logs. The main
[CI workflow](https://github.com/MisterVVP/a2a-cpp/actions/workflows/ci.yml)
also exercises SDK integration, interoperability, sanitizers and examples.
See [contributor validation](../contributing/validation.md) for local SDK checks.
