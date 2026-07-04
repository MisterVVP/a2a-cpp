# Releases and Versions

This documentation is intended to stay version-aware without embedding a release number in every page title.

## Current documented release

The current documented release is **v0.2.0**. It includes:

- REST, JSON-RPC, and gRPC client/server transport coverage.
- Agent Card discovery, including extended Agent Card support.
- Task lifecycle APIs including send, get, list, cancel, streaming send, and task subscription.
- Task push-notification configuration APIs and server-side delivery abstractions.
- Client and server interceptors.
- CMake package exports and vcpkg-oriented packaging support.

## Versioning guidance

- Pin CMake `FetchContent` integrations to a release tag such as `v0.2.0` or to a reviewed commit.
- Prefer `find_package(a2a_cpp CONFIG REQUIRED)` for installed SDK packages.
- Keep generated protobuf headers and linked SDK libraries from the same installed package or build tree.
- Review release notes before upgrading between minor versions.

## Documentation policy

Page titles and navigation should remain mostly version agnostic. Release-specific notes belong on this page or in clearly marked compatibility sections.
