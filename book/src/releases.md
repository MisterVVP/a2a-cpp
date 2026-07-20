# Releases and Versions

This documentation is intended to stay version-aware without embedding a release number in every page title.

## Current documented release

The current documented release is **v0.3.0**. It is the recommended release for new consumers and is the target for the future `v0.3.0` Git tag.

### Highlights since `v0.2.0`

- Production-ready Server-Sent Events (SSE) streaming for HTTP+JSON and JSON-RPC clients.
- End-to-end `SendStreamingMessage` and `SubscribeTask` support across HTTP+JSON, JSON-RPC, and gRPC transports.
- Shared libcurl-backed streaming HTTP infrastructure for built-in HTTP streaming clients.
- Cancellation, stream lifecycle, terminal-callback, and SSE protocol validation improvements.
- Expanded HTTP SSE interoperability and functional coverage.
- Report-only performance testing for SDK and wire-level scenarios.
- In-memory and PostgreSQL performance configurations.
- JSON, CSV, and Markdown performance artifacts.
- Repository-local vcpkg overlay packaging.
- Expanded CMake, vcpkg, Windows, examples, transports, streaming, authentication, and server documentation.
- Supported GitHub Actions updates.
- PostgreSQL parameter-array cleanup.
- AI-contribution labeling guidance.

### Compatibility notes

There are no intentional breaking API changes in `v0.3.0`. Consumers pinned to `v0.2.0` should update to `v0.3.0` when they are ready to consume the current SDK release.

Built-in HTTP+JSON and JSON-RPC SSE clients require libcurl support. Custom HTTP requester and streaming requester implementations remain supported for consumers that need alternative transport stacks.

TCK validation snapshot:

- 249 tests passed
- 16 tests skipped
- 0 executed test failures
- 100.0% compatibility for tested, non-skipped requirements

Existing capabilities from earlier releases, such as extended Agent Card support, required-extension support, PostgreSQL-backed stores, push-notification support, and CMake package exports, remain available in this release.

## Versioning guidance

- Pin CMake `FetchContent` integrations to a release tag such as `v0.3.0` or to a reviewed commit.
- Prefer `find_package(a2a_cpp CONFIG REQUIRED)` for installed SDK packages.
- Keep generated protobuf headers and linked SDK libraries from the same installed package or build tree.
- Review release notes before upgrading between minor versions.

## Documentation policy

Page titles and navigation should remain mostly version agnostic. Release-specific notes belong on this page or in clearly marked compatibility sections.
