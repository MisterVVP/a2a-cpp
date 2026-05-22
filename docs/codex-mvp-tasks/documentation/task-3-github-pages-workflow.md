# Task 3 — Add GitHub Pages Workflow

## Goal

Automate docs build and deployment to GitHub Pages using GitHub Actions.

## Scope

- Add `.github/workflows/docs.yml`.
- Configure triggers:
  - `push` on `main` for docs-relevant paths.
  - `pull_request` for docs validation.
  - `workflow_dispatch` for manual runs.
- Configure permissions:
  - `contents: read`
  - `pages: write`
  - `id-token: write`
- Add build job:
  - Checkout repository.
  - Install mdBook.
  - Run `mdbook build book`.
  - Upload static output with `actions/upload-pages-artifact`.
- Add deploy job:
  - Run only on `main`.
  - Deploy with `actions/deploy-pages`.

## Deliverables

- CI workflow that validates docs on PRs.
- Automatic deployment to Pages on `main`.

## Acceptance Criteria

- Pull requests run the docs build successfully.
- Merges to `main` publish updated documentation.
- Workflow follows GitHub’s artifact-based Pages deployment model.

## Out of Scope

- Content quality/SEO tuning.
