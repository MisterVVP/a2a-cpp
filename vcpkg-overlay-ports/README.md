# a2a-cpp vcpkg overlay ports

This directory contains custom vcpkg overlay ports for packages that are not yet published in a vcpkg registry.

## Install a2a-cpp from the overlay

From a checkout of this repository, run:

```bash
vcpkg install a2a-cpp --overlay-ports=./vcpkg-overlay-ports
```

For a consumer project using manifest mode, add the overlay to the consumer project's `vcpkg-configuration.json`:

```json
{
  "overlay-ports": [
    "../path/to/a2a-cpp/vcpkg-overlay-ports"
  ]
}
```

Then add `a2a-cpp` to the consumer project's `vcpkg.json` dependencies.

## Consume from CMake

```cmake
find_package(a2a_cpp CONFIG REQUIRED)
target_link_libraries(my_agent PRIVATE a2a::client a2a::server a2a::core)
```

The overlay currently pins the upstream source to a commit SHA. When this port is moved to a registry, replace the Git source with a release tag and an archive `SHA512`.
