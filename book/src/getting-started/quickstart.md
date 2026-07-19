# Quickstart: Build and Run Examples

The curated examples build app sources the same way downstream applications consume the SDK.

## 1. Install dependencies

Follow the platform instructions in [Installation and Build](installation.md). On Windows, run the repository helper from Git Bash before the example runner:

```bash
./scripts/install_build_deps.sh
```

## 2. Run the smallest end-to-end example

Linux or macOS:

```bash
./scripts/run_examples.sh build-example hello_agent
```

Windows Git Bash cleanup and rebuild:

```bash
rm -rf build-example-hello_agent
./scripts/run_examples.sh build-example hello_agent
```

The executable is written to `build-example-hello_agent/a2a_example` on single-configuration generators and to `build-example-hello_agent/RelWithDebInfo/a2a_example.exe` with Visual Studio.

`hello_agent` creates a minimal in-process client/server flow and exits deterministically.

## 3. Try transport-specific examples

```bash
./scripts/run_examples.sh build-example rest_server json_rpc_server grpc_server
```

These examples cover server transport setup and deterministic request handling across REST, JSON-RPC, and gRPC.

## 4. Try streaming, push, and auth examples

```bash
./scripts/run_examples.sh build-example streaming_client streaming_server push_notifications auth_policy_server
```

Use these when validating event streams, webhook configuration flows, or server-side auth metadata policy shapes. On Windows, the runner automatically forwards the vcpkg toolchain and locates the Visual Studio configuration output when `VCPKG_ROOT` is set.

## 5. Consume an installed SDK package

After installing the SDK into a prefix, build the same app source with `find_package(a2a_cpp CONFIG REQUIRED)`:

```bash
cmake -S examples/installed_package_consumer -B build-installed-example \
  -DCMAKE_PREFIX_PATH=/tmp/a2a-cpp-install \
  -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-installed-example --parallel
./build-installed-example/a2a_example
```

For Visual Studio, add `--config RelWithDebInfo` to the build and run `build-installed-example/RelWithDebInfo/a2a_example.exe`.

## 6. Validate your local checkout

```bash
./scripts/verify_changes.sh
```

For documentation-only edits, use `mdbook build book` instead of the full code validation flow.
