# CMake consumer templates

## FetchContent consumer

Use this template when your application should build a2a-cpp from a Git repository as part of your CMake configure step.

```bash
cmake -S examples/fetch_content_consumer -B build-example -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-example --parallel
./build-example/a2a_example
```

Select another app with `-DA2A_EXAMPLE_APP=<name>`. For reproducible builds, pin `A2A_CPP_GIT_TAG` to a release tag or commit instead of relying on the default branch:

```bash
cmake -S examples/fetch_content_consumer -B build-example \
  -DA2A_EXAMPLE_APP=rest_server \
  -DA2A_CPP_GIT_TAG=<release-tag-or-commit>
```

## Installed package consumer

Use this template when a2a-cpp has already been installed and your application should consume it with `find_package(a2a_cpp CONFIG REQUIRED)`.

### With an existing install prefix

```bash
cmake -S examples/installed_package_consumer -B build-installed-example \
  -DCMAKE_PREFIX_PATH=<install-prefix> \
  -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-installed-example --parallel
./build-installed-example/a2a_example
```

### With the local vcpkg overlay

```bash
cd examples/installed_package_consumer
vcpkg install
cmake -S . -B ../../build-installed-example \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DA2A_EXAMPLE_APP=hello_agent
cmake --build ../../build-installed-example --parallel
../../build-installed-example/a2a_example
```

Select another app with `-DA2A_EXAMPLE_APP=<name>`.

Run commands from the repository root unless the example explicitly changes directory.

## Async consumers (SDK v0.6.0)

Set `-DA2A_ENABLE_ASIO=ON` and supply Boost 1.92.0+ via `CMAKE_PREFIX_PATH`
when configuring the SDK or FetchContent template. Select
`-DA2A_EXAMPLE_APP=event_http_server`, `asio_executor`, `blocking_executor` or
`async_grpc_server`. For an installed SDK, build/install with Asio first, then
configure the installed consumer against that prefix; the package exports
`a2a::asio` and finds its Boost dependency automatically.

```bash
cmake -S examples/fetch_content_consumer -B build-event-fetch \
  -DFETCHCONTENT_SOURCE_DIR_A2A_CPP="$PWD" \
  -DA2A_ENABLE_ASIO=ON -DA2A_EXAMPLE_APP=event_http_server
cmake --build build-event-fetch --parallel
./build-event-fetch/a2a_example --smoke
```

`FETCHCONTENT_SOURCE_DIR_A2A_CPP` selects the local reviewed candidate without
requiring a release tag. For remote consumption set `A2A_CPP_GIT_TAG` to the
full reviewed commit. The installed consumer can select the same app with
`-DCMAKE_PREFIX_PATH=<asio-sdk-prefix>`. Visual Studio builds use
`--config Release` and the `Release/` executable directory.

`async_unary_client` uses `a2a::client` in either package. See
[async clients](../client/async-unary.md), [native gRPC](../server/async-grpc.md),
and [event HTTP](../server/event-http.md) for runtime contracts.
