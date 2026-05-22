# Task 3 — Publish Compliance Evidence in Documentation

## Objective

Make compliance status auditable by mapping community SDK requirements to concrete CI jobs, pass criteria, and evidence artifacts.

## Scope

- Update `docs/sdk-readiness.md`.
- Optionally add a dedicated evidence page in `docs/` and cross-link from README.
- Add workflow badges/links for TCK and ITK.

## Required implementation steps

1. **Add evidence matrix**
   - Create a table with columns:
     - Requirement
     - CI workflow/job
     - Trigger
     - Pass criteria
     - Artifact/report link

2. **Define requirement mappings**
   - Include at minimum:
     - TCK mandatory category conformance
     - Interoperability validation (ITK scenario)
     - Existing quality gates (build/test/lint)

3. **Expose discoverability**
   - Add links from `README.md` to the compliance section/page.
   - Add workflow status badges for TCK and ITK.

4. **Document maintenance policy**
   - Clarify when full suites run (PR vs main vs schedule).
   - Describe ownership and expected response for red workflows.

5. **Ensure consistency**
   - Validate names in docs exactly match workflow/job names.
   - Verify links resolve to existing paths and CI pages.

## Acceptance criteria

- Evidence mapping is explicit and easy to audit.
- README links users to the compliance evidence quickly.
- Workflow names and links are accurate.
- Maintenance expectations are documented.

## Non-goals

- Replacing CI implementation details already defined in workflow files.
