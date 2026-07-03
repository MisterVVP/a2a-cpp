# Installed package consumer

Use this template when a2a-cpp has already been installed and your application should consume it with `find_package(a2a_cpp CONFIG REQUIRED)`.

## With an existing install prefix

```bash
cmake -S examples/installed_package_consumer -B build-installed-example \
  -DCMAKE_PREFIX_PATH=<install-prefix> \
  -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-installed-example --parallel
./build-installed-example/a2a_example
```

## With the local vcpkg overlay

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
