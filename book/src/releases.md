# Releases and Versions

This documentation is intended to stay version-aware without embedding a release number in every page title.

## Current documented release

The current documented release is **v0.4.1** and is recommended for new consumers.

### Highlights since `v0.4.0`

- Optimized PostgreSQL push-configuration create, get, list, and cleanup paths,
  reducing redundant task lookups, commands, and connection acquisitions while
  preserving authoritative task validation and concurrent-deletion safety.
- Added optimistic revision-aware task persistence to the built-in stores so
  the example/TCK `SendMessage` path can persist complete task mutations with
  fewer PostgreSQL round trips while preserving concurrent history updates.
- Added HTTP/1.1 persistent connections and connection-scoped request buffering,
  eliminating per-request connection churn for ordinary unary HTTP+JSON and
  JSON-RPC operations.
- Optimized REST query parsing and reorganized transport implementation sources
  under dedicated client/server transport directories without changing public
  include paths.
- Optimized HTTP+JSON and JSON-RPC `ListTasks` parsing and serialization through
  typed protobuf JSON paths, with focused parser/scanner benchmarks and CI
  thresholds.
- Hardened typed `ListTasks` compatibility validation for null values, duplicate
  fields, protobuf-name aliases, nested duplicate message members, escaped
  JSON-RPC result keys, HTTP-status classification, and protobuf integer ranges.
- Added component-level transport benchmarks for JSON-RPC envelopes, ProtoJSON,
  REST query parsing, response construction, and `ListTasks` client parsing.
- Isolated performance fixtures from measured operations and restructured
  reports around concrete workload coordinates rather than mixed scenario
  averages.
- Expanded PostgreSQL command-level diagnostics and documented storage,
  performance, and transport behavior.
- Added repository-wide Conventional Commit validation in CI.

### Compatibility notes

There are no intentional public API removals in `v0.4.1`.

The transport source-tree reorganization is internal; installed public include
paths and exported CMake targets remain unchanged.

HTTP+JSON and JSON-RPC unary traffic can now reuse HTTP/1.1 connections. Explicit
`Connection: close` remains supported, and the existing streaming/SSE behavior
is preserved.

Valid `ListTasks` responses remain compatible across HTTP+JSON and JSON-RPC.
Malformed or ambiguous JSON that protobuf 3.21 could otherwise accept with
last-value-wins or null-as-unset behavior is now rejected consistently.

PostgreSQL users should allow SDK-managed schemas to migrate and validate the
task-aware push-configuration helpers. Externally managed schemas should follow
the storage documentation for the required schema objects and privileges.

## Previous release: `v0.4.0`

`v0.4.0` focused on configurable PostgreSQL pool sizing, bounded protocol-facing
`ListTasks` pagination, optimized in-memory task listing, interruptible gRPC
stream cancellation, expanded performance validation, and centralized HTTP
server response construction.

Its important compatibility changes remain in effect:

- omitted protocol-facing `ListTasks.page_size` defaults to `50`;
- explicit page sizes must be between `1` and `100`;
- the PostgreSQL connection pool defaults to `4` unless configured otherwise.

## Earlier release: `v0.3.0`

`v0.3.0` introduced production-ready SSE streaming for HTTP+JSON and JSON-RPC,
end-to-end streaming and subscriptions across all supported transports,
report-only performance testing, repository-local vcpkg overlay packaging, and
expanded build and platform documentation.

Its TCK validation snapshot was:

- 249 tests passed
- 16 tests skipped
- 0 executed test failures
- 100.0% compatibility for tested, non-skipped requirements

## Versioning guidance

- Pin CMake `FetchContent` integrations to a release tag such as `v0.4.1` or to
  a reviewed commit.
- Prefer `find_package(a2a_cpp CONFIG REQUIRED)` for installed SDK packages.
- Keep generated protobuf headers and linked SDK libraries from the same
  installed package or build tree.
- Review release notes before upgrading between versions.

## Documentation policy

Page titles and navigation should remain mostly version agnostic. Release-specific notes belong on this page or in clearly marked compatibility sections.
