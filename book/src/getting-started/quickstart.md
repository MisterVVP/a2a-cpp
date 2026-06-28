# Quickstart: Build and Run an A2A REST Client End-to-End

This quickstart gives a practical, copy/paste flow that goes from build to a runnable client example.

Looking for the full docs map first? Start at the [Documentation Home](../README.md).

## 1) Configure and build with examples

```bash
cmake -S . -B build -DA2A_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

## 2) Run a minimal example

```bash
./build-example/a2a_example
```

Expected behavior: the client performs a minimal REST workflow and exits successfully.

## 3) Try additional examples

```bash
./build-example/a2a_example
./build-example/a2a_example
./build-example/a2a_example
./build-example/a2a_example
```

## 4) Run quality gates locally

```bash
./scripts/verify_changes.sh
```

This command is the canonical contributor validation entrypoint and should pass before PR updates.

## 5) Optional: run focused checks during iteration

```bash
ctest --test-dir build --output-on-failure
./scripts/run_clang_tidy.sh build
```

## Troubleshooting

- If configure fails, confirm the required tools from [Installation](installation.md) are installed.
- If example binaries are missing, re-run CMake with `-DA2A_BUILD_EXAMPLES=ON`.
- If `clang-tidy` reports issues, fix touched code and re-run `./scripts/verify_changes.sh`.

## Next steps

- Continue with [Send Messages with A2AClient](../client/sending-messages.md).
- Review [REST Transport](../transports/rest.md) for endpoint and runtime guidance.
- Add auth headers safely using [Authentication Overview](../auth/overview.md).
