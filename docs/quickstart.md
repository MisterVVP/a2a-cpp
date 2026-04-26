# Quickstart

1. Build with examples enabled:

```bash
cmake -S . -B build -DA2A_BUILD_EXAMPLES=ON
cmake --build build
```

2. Run a minimal end-to-end example:

```bash
./build/examples/example_rest_client
```

3. Validate local quality gates before opening a PR:

```bash
./scripts/verify_changes.sh
```
