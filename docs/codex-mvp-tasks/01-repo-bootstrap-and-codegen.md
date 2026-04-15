# Task 01 — Repository bootstrap, build system, and proto codegen

## Goal

Create the initial repository skeleton for the A2A C++ SDK and establish a repeatable code-generation pipeline using the official A2A 1.0 protobuf definitions.

## Why this comes first

Everything else depends on stable build, dependency, and generated-type foundations.

## Scope

- Create the base repository layout.
- Add top-level and subdirectory `CMakeLists.txt`.
- Vendor or fetch the official A2A 1.0 proto definitions into `proto/`.
- Add protobuf and gRPC generation steps.
- Generate code into a predictable directory such as `generated/`.
- Add library targets for:
  - `a2a_core`
  - `a2a_client`
  - `a2a_server`
- Add minimal compile options for Linux/macOS builds if possible, but Linux is the main target.
- Add basic formatting and lint configuration.
- Add a small smoke test that verifies generated headers are usable from C++.

## Constraints

- Protocol version target is **A2A 1.0 only**.
- Do not implement runtime protocol logic yet.
- Do not introduce a second handwritten protocol model.
- Keep dependencies minimal and explicit.
- Use **C++20**.
- Prefer an approach that can work cleanly in CI.

## Recommended dependencies

- protobuf
- gRPC
- a small HTTP client/server stack later, but do not lock transport here unless needed
- test framework such as GoogleTest
- optional: Abseil only if required by gRPC / protobuf toolchain

## Deliverables

- repository skeleton
- CMake build that configures and compiles
- proto generation script or CMake target
- generated code committed or reproducibly generated in CI
- `docs/build.md` with exact build instructions
- one smoke test for generated types

## Implementation notes

- Put all generated code behind a dedicated target.
- Expose generated include directories cleanly to downstream targets.
- Decide whether codegen happens:
  - at configure/build time, or
  - via a checked-in generation step
- Whichever path you choose, document it clearly and keep it deterministic.
- Namespaces should be mapped cleanly and consistently.

## Acceptance criteria

- Fresh clone can be configured and built with documented steps.
- Generated A2A proto code compiles without manual fixes.
- A test can include generated headers and instantiate at least one generated message.
- CI can reproduce the generation/build flow.

## Out of scope

- discovery
- HTTP
- JSON-RPC
- SSE
- auth
- server runtime
