# Task 22 — Python CLI workflow parity decisions for C++

> **Status:** Planned / decision needed. Treat as parity follow-up requiring fresh validation before implementation.

## Goal

Close the documented CLI-workflow parity gap versus `a2a-python` by deciding and implementing the C++ equivalent strategy (native CLI, helper scripts, or explicit non-goal with rationale).

## Scope

- Inventory Python CLI workflows that are required for interoperability and onboarding.
- Classify each CLI workflow as:
  - must-have parity in C++
  - optional parity
  - intentional non-parity (with rationale)
- Implement must-have workflows or document why they are deferred.

## Deliverables

- Updated parity rows in `docs/parity-a2a.md` for CLI capability status.
- Implementation references (task links, scripts, or binaries) for accepted workflows.
- Short usage docs for local and CI automation.

## Acceptance criteria

- CLI parity row in `docs/parity-a2a.md` is evidence-backed.
- Each required workflow has a concrete implementation task or completion reference.
- Reviewers can run at least one end-to-end workflow using documented commands.
