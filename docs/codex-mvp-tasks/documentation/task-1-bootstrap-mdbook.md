# Task 1 — Bootstrap mdBook Structure

> **Status:** Historical / completed. The mdBook skeleton exists; keep this file as setup history.

## Goal

Create the mdBook skeleton and navigation so the docs site builds with placeholder content.

## Scope

- Add `book/book.toml`.
- Add `book/src/SUMMARY.md`.
- Add `book/src/README.md`.
- Add the initial directory and page skeleton under `book/src/`:
  - `getting-started/installation.md`
  - `getting-started/quickstart.md`
  - `client/overview.md`
  - `client/sending-messages.md`
  - `client/discovery.md`
  - `server/overview.md`
  - `server/custom-executor.md`
  - `transports/rest.md`
  - `transports/json-rpc.md`
  - `transports/grpc.md`
  - `streaming/overview.md`
  - `auth/overview.md`
  - `build/cmake.md`
  - `build/vcpkg.md`
  - `api-reference.md`
- Keep root `README.md` concise and SEO-oriented.

## Deliverables

- A buildable mdBook skeleton with working sidebar navigation.
- Section hierarchy aligned with the planned documentation architecture.

## Acceptance Criteria

- `mdbook build book` succeeds locally.
- Sidebar structure follows the planned information architecture.
- No deployment workflow changes are included in this task.

## Out of Scope

- Deep content migration.
- GitHub Pages deployment.
