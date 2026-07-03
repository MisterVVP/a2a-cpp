# Packaging and publication

## Targets

- vcpkg overlay: repository-local custom overlay under `vcpkg-overlay-ports/`
- vcpkg: **public vcpkg registry** via PR to `microsoft/vcpkg`

## Release metadata policy

- Package name: `a2a-cpp`
- Version source: Git tag `v<major>.<minor>.<patch>`
- License: Apache-2.0
- Changelog source: GitHub Releases notes
- Support window: latest release line + `main`

## Repository-local vcpkg overlay flow

1. Add or update the overlay port under `vcpkg-overlay-ports/a2a-cpp`.
2. Pin the overlay source to the current stable release tag.
3. Keep package builds focused on installable artifacts:
   - `A2A_ENABLE_TESTING=OFF`
   - `A2A_BUILD_EXAMPLES=OFF`
   - `A2A_BUILD_BENCHMARKS=OFF`
4. Validate manifest-mode consumption through `examples/installed_package_consumer`.
5. For direct classic-mode smoke tests, run the package install command from a directory that does not contain a `vcpkg.json` manifest.

## vcpkg public registry flow

1. Fork `microsoft/vcpkg`.
2. Copy the validated overlay port files into `ports/a2a-cpp`.
3. Replace `vcpkg_from_git` with `vcpkg_from_github` so the registry port downloads the tagged release archive:

   ```cmake
   vcpkg_from_github(
       OUT_SOURCE_PATH SOURCE_PATH
       REPO MisterVVP/a2a-cpp
       REF "v${VERSION}"
       SHA512 REPLACE_WITH_RELEASE_ARCHIVE_SHA512
       HEAD_REF main
   )
   ```

4. Compute and add the real `SHA512` for the release archive.
5. Build-test the registry port on the intended triplet, for example `x64-windows` and at least one Linux triplet when available.
6. Run `vcpkg x-add-version a2a-cpp` in the `microsoft/vcpkg` checkout to update the version database.
7. Open the vcpkg PR and pass vcpkg CI.

## Best-practice recommendations

- Prefer short-lived tokens or OIDC where possible.
- Use signed tags/releases.
- Keep SBOM and provenance artifacts with release assets.
