# Packages and release artifacts

The SDK exports installed CMake targets under the `a2a::` namespace. Use
[CMake integration](cmake.md) or the [repository-local vcpkg overlay](vcpkg.md)
to consume a reviewed release.

Release tags use `v<major>.<minor>.<patch>`. The release packaging workflow produces
source `.tar.gz` and `.zip` archives and `SHA256SUMS.txt` on
[GitHub Releases](https://github.com/MisterVVP/a2a-cpp/releases).
Verify the checksum after downloading. Pin source dependencies to a release tag
or reviewed commit and keep generated protobuf headers and linked libraries
from the same package or build tree.

The repository-local overlay is distinct from publication in the public
`microsoft/vcpkg` registry. A local overlay does not prove registry availability.
See [release compatibility notes](../releases.md) and
[supported versions](https://github.com/MisterVVP/a2a-cpp/blob/main/SUPPORT.md) before upgrading.
