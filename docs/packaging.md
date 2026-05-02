# Packaging and publication

## Targets

- Conan registry: **GitHub Packages**
- vcpkg: **public vcpkg registry** (via PR to `microsoft/vcpkg`)

## Required repository secrets (placeholders)

Set these in GitHub Actions secrets before publishing:

- `CONAN_REMOTE_URL` = `https://npm.pkg.github.com/<owner>`
- `CONAN_LOGIN_USERNAME` = `<github-username>`
- `CONAN_PASSWORD` = `<token-with-packages-write>`

Optional (recommended for provenance/signing):

- `COSIGN_PRIVATE_KEY` = `<pem-content>`
- `COSIGN_PASSWORD` = `<password>`

## Release metadata policy

- Package name: `a2a-cpp`
- Version source: Git tag `v<major>.<minor>.<patch>`
- License: Apache-2.0
- Changelog source: GitHub Releases notes
- Support window: latest release line + `main`

## Conan publish flow

1. Tag release (`vX.Y.Z`).
2. GitHub Action `Release Packages` runs automatically.
3. Workflow builds and uploads package to configured GitHub Packages Conan remote.

## vcpkg public registry flow

1. Create vcpkg port files in a fork of `microsoft/vcpkg`.
2. Set source archive URL to tagged release.
3. Compute and include SHA512.
4. Open vcpkg PR and pass vcpkg CI.

## Best-practice recommendations

- Prefer short-lived tokens or OIDC where possible.
- Use signed tags/releases.
- Keep SBOM and provenance artifacts with release assets.
