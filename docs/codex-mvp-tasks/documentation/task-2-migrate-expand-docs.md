# Task 2 — Migrate and Expand Existing Docs

> **Status:** Historical / partially completed. Current mdBook pages have been expanded, but future doc work should use current `book/src/**` content as source of truth.

## Goal

Move and expand current documentation from `docs/` into `book/src/`, preserving useful content and improving depth.

## Scope

- Migrate/refactor content from:
  - `docs/build.md`
  - `docs/quickstart.md`
  - `docs/client-usage.md`
  - `docs/server-usage.md`
  - `docs/streaming-usage.md`
  - `docs/auth-hooks.md`
- Expand key pages into dedicated chapters:
  - Client:
    - `client/overview.md`
    - `client/sending-messages.md`
    - `client/discovery.md`
    - `client/get-task.md`
    - `client/cancel-task.md`
  - Server:
    - `server/overview.md`
    - `server/custom-executor.md`
    - `server/rest-server.md`
    - `server/json-rpc-server.md`
  - Authentication:
    - API key auth
    - Bearer token auth
    - Custom header auth
    - OAuth2 extension point
    - Server-side metadata extraction
    - mTLS notes (if supported by transports)
- Expand quickstart into practical, copy-pasteable steps.

## Deliverables

- Reorganized documentation under `book/src` with clearer topic boundaries.
- Expanded guides aligned with real SDK usage flows.

## Acceptance Criteria

- Existing user-facing knowledge is preserved after migration.
- Quickstart is runnable end-to-end with explicit commands.
- Client/server/auth sections include happy-path and common operational context.

## Out of Scope

- GitHub Pages workflow.
- SEO final polish.
