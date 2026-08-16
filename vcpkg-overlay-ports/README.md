# a2a-cpp vcpkg overlay ports

This directory contains custom vcpkg overlay ports for packages that are not yet published in a vcpkg registry.

## Install a2a-cpp from the overlay

The repository root has its own `vcpkg.json`, so `vcpkg install <package>` from the root runs in manifest mode and does not accept package arguments.

To smoke-test the overlay port directly, run the classic-mode command from a directory without a `vcpkg.json` manifest:

```bash
cd ..
mkdir -p a2a-cpp-vcpkg-smoke
cd a2a-cpp-vcpkg-smoke
vcpkg install a2a-cpp --overlay-ports=../a2a-cpp/vcpkg-overlay-ports
```

For a consumer project using manifest mode, add the overlay to the consumer project's `vcpkg-configuration.json`:

```json
{
  "overlay-ports": [
    "../path/to/a2a-cpp/vcpkg-overlay-ports"
  ]
}
```

Then add `a2a-cpp` to the consumer project's `vcpkg.json` dependencies and run `vcpkg install` without package arguments.

## Consume from CMake

```cmake
find_package(a2a_cpp CONFIG REQUIRED)
target_link_libraries(my_agent PRIVATE a2a::client a2a::server a2a::core)
```

## Registry publishing preparation

The overlay pins the upstream source to the stable `v0.4.1` release tag. For an official `microsoft/vcpkg` registry PR, copy the port files into `ports/a2a-cpp` in a `microsoft/vcpkg` fork and replace `vcpkg_from_git` with a release archive source:

```cmake
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO MisterVVP/a2a-cpp
    REF "v${VERSION}"
    SHA512 <release-archive-sha512>
    HEAD_REF main
)
```

After updating the registry port, run `vcpkg x-add-version a2a-cpp` in the `microsoft/vcpkg` checkout so the version database is updated before opening the registry PR.
