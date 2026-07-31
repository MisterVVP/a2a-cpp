# Build with vcpkg

`a2a-cpp` provides vcpkg metadata for two related workflows:

1. **Manifest dependency mode** for building this repository with vcpkg-supplied third-party dependencies.
2. **Overlay port mode** for consuming `a2a-cpp` itself as a vcpkg package before it is available from a public registry.

The repository root `vcpkg.json` pins the dependency baseline and declares the SDK's third-party dependencies: protobuf, gRPC, and curl.

## Prerequisites

Install or clone vcpkg and bootstrap it:

```bash
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh"
```

On Windows PowerShell:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
```

Set `VCPKG_ROOT` for convenience:

```bash
export VCPKG_ROOT="$HOME/vcpkg"
```

```powershell
$env:VCPKG_ROOT = 'C:\vcpkg'
```

### Repository helper on Windows Git Bash

From the repository root, the Bash helper clones, bootstraps, and installs the vcpkg manifest dependencies:

```bash
./scripts/install_build_deps.sh
```

The default vcpkg checkout is `$HOME/vcpkg`; no fixed `C:/vcpkg` location is required. Override the root or triplets with environment variables:

```bash
export VCPKG_ROOT=/d/tools/vcpkg
export VCPKG_TARGET_TRIPLET=x64-windows-static
export VCPKG_HOST_TRIPLET=x64-windows
./scripts/install_build_deps.sh
```

## Build this repository with manifest dependencies

From the repository root, let vcpkg install the manifest dependencies and then configure CMake with the vcpkg toolchain file:

```bash
"$VCPKG_ROOT/vcpkg" install
cmake -S . -B build-vcpkg \
  -DVCPKG_MANIFEST_MODE=ON \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DA2A_ENABLE_TESTING=ON
cmake --build build-vcpkg --parallel
ctest --test-dir build-vcpkg --output-on-failure
```

On multi-config generators such as Visual Studio, pass the configuration during build and test:

```powershell
& "$env:VCPKG_ROOT\vcpkg.exe" install
cmake -S . -B build-vcpkg -G "Visual Studio 17 2022" -A x64 `
  -DVCPKG_MANIFEST_MODE=ON `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build build-vcpkg --config RelWithDebInfo --parallel
ctest --test-dir build-vcpkg -C RelWithDebInfo --output-on-failure
```

## Use a specific triplet

Pass the same target triplet to vcpkg and CMake. For native builds, use the same value for the host triplet so host tools such as `protoc` and `grpc_cpp_plugin` are resolved consistently:

```bash
"$VCPKG_ROOT/vcpkg" install --triplet x64-linux --host-triplet x64-linux
cmake -S . -B build-vcpkg \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DVCPKG_HOST_TRIPLET=x64-linux
```

Windows CI uses the repository triplet `triplets/ci-x64-windows-release.cmake` to build release-only dependencies and reduce dependency build time:

```powershell
$env:VCPKG_OVERLAY_TRIPLETS = "$PWD\triplets"
& "$env:VCPKG_ROOT\vcpkg.exe" install --triplet ci-x64-windows-release --host-triplet ci-x64-windows-release
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=ci-x64-windows-release `
  -DVCPKG_HOST_TRIPLET=ci-x64-windows-release
```

## Build standalone FetchContent examples on Windows

The example runner uses the already-installed root manifest dependencies. It automatically passes the vcpkg toolchain and triplets when `VCPKG_ROOT` is set:

```bash
./scripts/install_build_deps.sh
rm -rf build-example-streaming_client
./scripts/run_examples.sh build-example streaming_client
./build-example-streaming_client/RelWithDebInfo/a2a_example.exe --help
```

Delete a build directory that was configured before the toolchain was supplied; changing `CMAKE_TOOLCHAIN_FILE` in an existing CMake cache is not reliable.

## Consume `a2a-cpp` through the repository overlay port

The repository includes an overlay port at `vcpkg-overlay-ports/a2a-cpp`. A downstream manifest can depend on `a2a-cpp` and point vcpkg at that overlay.

`vcpkg.json`:

```json
{
  "name": "my-a2a-app",
  "version-string": "0.4.0",
  "dependencies": [
    "a2a-cpp"
  ]
}
```

`vcpkg-configuration.json`:

```json
{
  "default-registry": {
    "kind": "builtin",
    "baseline": "3426db05b996481ca31e95fff3734cf23e0f51bc"
  },
  "overlay-ports": [
    "path/to/a2a-cpp/vcpkg-overlay-ports"
  ]
}
```

Then configure the application with the vcpkg toolchain file on the first CMake configure and use the installed CMake package:

```cmake
find_package(a2a_cpp CONFIG REQUIRED)
target_link_libraries(my_a2a_app PRIVATE a2a::client a2a::server a2a::core)
```

A complete example is available in `examples/installed_package_consumer/`.

## Enable PostgreSQL store support

The overlay port exposes a `postgres-store` feature. Enable it in manifest mode when your application needs PostgreSQL-backed stores:

```json
{
  "name": "my-a2a-app",
  "version-string": "0.4.0",
  "dependencies": [
    {
      "name": "a2a-cpp",
      "features": ["postgres-store"]
    }
  ]
}
```

When the feature is enabled, link the additional target where needed:

```cmake
target_link_libraries(my_a2a_app PRIVATE a2a::store_postgres)
```

## Classic mode smoke install

For a direct overlay smoke test, run classic mode from a directory that does not contain a `vcpkg.json` manifest:

```bash
mkdir -p /tmp/a2a-vcpkg-smoke
cd /tmp/a2a-vcpkg-smoke
"$VCPKG_ROOT/vcpkg" install a2a-cpp --overlay-ports=/path/to/a2a-cpp/vcpkg-overlay-ports
```

Add a triplet if needed:

```bash
"$VCPKG_ROOT/vcpkg" install a2a-cpp:x64-linux --overlay-ports=/path/to/a2a-cpp/vcpkg-overlay-ports
```

## Binary caching

Large dependencies such as gRPC and protobuf can take time to build. Enable binary caching for local and CI runs:

```bash
export VCPKG_BINARY_SOURCES="clear;files,$HOME/.cache/vcpkg-binary-cache,readwrite"
mkdir -p "$HOME/.cache/vcpkg-binary-cache"
```

On Windows PowerShell:

```powershell
$env:VCPKG_BINARY_SOURCES = 'clear;files,C:\vcpkg-binary-cache,readwrite'
New-Item -ItemType Directory -Force C:\vcpkg-binary-cache | Out-Null
```

## Troubleshooting

- **CMake cannot find gRPC or Protobuf**: confirm `CMAKE_TOOLCHAIN_FILE` points to `scripts/buildsystems/vcpkg.cmake` before the first configure. If you configured without it, delete the build directory and configure again.
- **Unexpected manifest behavior in classic mode**: classic `vcpkg install a2a-cpp` should be run outside directories containing `vcpkg.json`, otherwise vcpkg switches to manifest mode.
- **Different host and target triplets**: pass both `VCPKG_TARGET_TRIPLET` and `VCPKG_HOST_TRIPLET` when cross-compiling or when CI uses a custom host triplet.
- **Slow clean builds**: enable binary caching and prefer release-only dependency triplets for CI jobs that only link release configurations.
- **Example executable is not in the build root on Windows**: Visual Studio is multi-configuration; use `build-example-<app>/RelWithDebInfo/a2a_example.exe`, or let `scripts/run_examples.sh` locate and run it.
- **The dependency helper reports an unsupported shell on Windows**: run `scripts/install_build_deps.sh` from Git Bash rather than another Windows shell.
