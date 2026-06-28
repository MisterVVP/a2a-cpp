# auth_policy_server

## What it demonstrates

Server-side auth metadata handling and policy shape.

## SDK targets

This app links the public SDK targets `a2a::client`, `a2a::server`, and `a2a::core` through the consumer CMake templates.

## Build with FetchContent

```bash
cmake -S examples/fetch_content_consumer -B build-example -DA2A_EXAMPLE_APP=auth_policy_server
cmake --build build-example --parallel
./build-example/a2a_example
```

## Build with an installed package

```bash
cmake -S examples/installed_package_consumer -B build-installed-example \
  -DCMAKE_PREFIX_PATH=<install-prefix> \
  -DA2A_EXAMPLE_APP=auth_policy_server
cmake --build build-installed-example --parallel
./build-installed-example/a2a_example
```

## Expected output

The program prints concise success lines containing the app name plus a task id, status, event, or delivery detail. It exits non-zero if an SDK call fails.
