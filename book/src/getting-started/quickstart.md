# Quickstart: Build and Run Examples

The fastest way to exercise the SDK is through the curated consumer examples. They build the example app sources the same way downstream applications consume the SDK.

## 1. Run the smallest end-to-end example

```bash
cmake -S examples/fetch_content_consumer -B build-example \
  -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-example --parallel
./build-example/a2a_example
```

`hello_agent` creates a minimal in-process client/server flow and exits deterministically.

## 2. Try transport-specific examples

```bash
./scripts/run_examples.sh rest_server json_rpc_server grpc_server
```

These examples cover server transport setup and deterministic request handling across REST, JSON-RPC, and gRPC.

## 3. Try streaming, push, and auth examples

```bash
./scripts/run_examples.sh streaming_client streaming_server push_notifications auth_policy_server
```

Use these when validating event streams, webhook configuration flows, or server-side auth metadata policy shapes.

## 4. Consume an installed SDK package

After installing the SDK into a prefix, build the same app source with `find_package(a2a_cpp CONFIG REQUIRED)`:

```bash
cmake -S examples/installed_package_consumer -B build-installed-example \
  -DCMAKE_PREFIX_PATH=/tmp/a2a-cpp-install \
  -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-installed-example --parallel
./build-installed-example/a2a_example
```

## 5. Validate your local checkout

```bash
./scripts/verify_changes.sh
```

For documentation-only edits, use `mdbook build book` instead of the full code validation flow.
