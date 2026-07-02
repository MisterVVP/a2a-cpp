# Packaging and publication

## Targets

- vcpkg: **public vcpkg registry** (via PR to `microsoft/vcpkg`)

## Release metadata policy

- Package name: `a2a-cpp`
- Version source: Git tag `v<major>.<minor>.<patch>`
- License: Apache-2.0
- Changelog source: GitHub Releases notes
- Support window: latest release line + `main`

## vcpkg public registry flow

1. Create vcpkg port files in a fork of `microsoft/vcpkg`.
2. Set source archive URL to tagged release.
3. Compute and include SHA512.
4. Open vcpkg PR and pass vcpkg CI.

## Best-practice recommendations

- Prefer short-lived tokens or OIDC where possible.
- Use signed tags/releases.
- Keep SBOM and provenance artifacts with release assets.
