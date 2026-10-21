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
