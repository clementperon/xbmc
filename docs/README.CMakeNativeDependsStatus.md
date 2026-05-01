# Top-Level CMake Native Depends: Platform Status

This document summarizes the current state of the top-level CMake native-depends migration and what remains to reach full end-to-end coverage per platform.

Scope:
- Top-level entrypoint (`cmake -S . -B ...`) with `KODI_SUPERBUILD_DEPENDS=ON`
- Dependency bootstrap through `tools/depends` orchestrated by CMake
- Kodi app build through `kodi-core`
- Optional binary add-on build/package through `KODI_SUPERBUILD_ADDONS=ON`
- CI lanes in `.github/workflows/build-and-cross-build.yml`

## New Superbuild Targets

- `kodi-core`
  - Existing target: builds dependency graph and Kodi app only.
- `kodi-addons`
  - New staged target: configures/builds the `cmake/addons` project from the top-level superbuild.
- `kodi-addons-package`
  - New staged target: packages selected add-ons and writes `.success` / `.failure` into the add-ons superbuild binary dir.
- `kodi-e2e`
  - New aggregate target: `kodi-core` + `kodi-addons` + `kodi-addons-package`.

## New Superbuild Add-On Options

All options are passed to top-level CMake when `KODI_SUPERBUILD_DEPENDS=ON`:

- `KODI_SUPERBUILD_ADDONS` (default: `OFF`)
  - Enables add-on integration in the superbuild.
- `KODI_ADDONS_TO_BUILD` (default: `all`)
  - Add-on selector forwarded to `cmake/addons` (`ADDONS_TO_BUILD`).
- `KODI_ADDONS_PACKAGE_ZIP` (default: `AUTO`)
  - `ON`/`OFF`/`AUTO` (`AUTO` resolves to `OFF` on Android, `ON` elsewhere).
- `KODI_ADDONS_BUILD_TARGET` (default: `all`)
  - Build target for `cmake/addons`.
- `KODI_ADDONS_PACKAGE_TARGET` (default: `package-addons`)
  - Package target for `cmake/addons`.
- `KODI_ADDONS_INSTALL_PREFIX`
  - Optional add-on install prefix override.
- `KODI_ADDONS_DEFINITION_DIR`
  - Optional override for add-on definition checkout/bootstrap location.
- `KODI_ADDONS_SOURCE_PREFIX`
  - Optional local source checkout prefix for add-ons.
- `KODI_ADDONS_AUTOCONF_FILES`
  - Optional explicit autoconf helpers forwarding.
- `KODI_ADDONS_EXTRA_CMAKE_ARGS`
  - Extra CMake arguments forwarded to `cmake/addons`.

## Current Platform Status

- `linux-build`
  - Status: **end-to-end build lane exists**
  - What is validated:
    - Native Linux configure + build through regular top-level CMake (non-superbuild lane)
    - Internal FFmpeg path in CI config
  - Notes:
    - Uses system packages and ccache.

- `linux-full-build-no-system-deps`
  - Status: **full end-to-end target configured (`kodi-core`)**
  - What is validated:
    - Superbuild orchestration
    - Depends bootstrap + package graph + Kodi build target
  - Recent fix applied:
    - Depends package execution now forces `make/gmake` (not top-level Ninja) for `tools/depends/*` package directories.

- `linux-superbuild-with-addons`
  - Status: **new staged lane (non-blocking)**
  - What is validated:
    - Top-level `kodi-addons` integration
    - `kodi-e2e` target generation and execution path
    - Add-on artifact publication (`addons` output, `.success`, `.failure`)
  - Notes:
    - Starts with a constrained add-on set (`pvr.iptvsimple`) before expanding to `all`.

- `macos-build`
  - Status: **full end-to-end target configured (`kodi-core`)**
  - What is validated:
    - Superbuild orchestration on macOS runner
    - Native macOS host triplet path
  - Notes:
    - Still CI-focused; no signing/notarization verification in this lane.

- `macos-tvos-cross-build`
  - Status: **smoke-level cross lane**
  - What is validated:
    - tvOS toolchain/dependency bootstrap config path
    - `kodi-depends-configure` target
  - Not yet validated:
    - Full `kodi-core` compile for tvOS
    - Packaging/signing behavior.

- `wasm-cross-build`
  - Status: **smoke-level cross lane**
  - What is validated:
    - Emscripten setup
    - Depends configure path (`kodi-depends-configure`)
  - Recent fix applied:
    - Emscripten toolchain path derivation adjusted to emsdk root expected by depends configure.
  - Not yet validated:
    - Full `kodi-core` compile with wasm toolchain in CI.

- `android-cross-build`
  - Status: **smoke-level cross lane**
  - What is validated:
    - Android SDK/NDK setup
    - Depends configure path (`kodi-depends-configure`)
  - Recent fix applied:
    - Removed unsupported platform override from superbuild configure arguments.
  - Not yet validated:
    - Full `kodi-core` compile/package in CI.

## What Is Still Needed For Full End-to-End

For each platform, "full end-to-end" means:
- CMake configure succeeds
- Depends graph builds
- `kodi-core` builds
- Platform packaging artifacts are produced (where applicable)
- (Optional but recommended) smoke runtime validation

Remaining work:

- Cross lanes (`android`, `wasm`, `tvos`)
  - Promote from `kodi-depends-configure` to `kodi-core` (possibly staged with non-blocking jobs first).
  - After `kodi-core` is stable, stage `KODI_SUPERBUILD_ADDONS=ON` lane per platform.
  - Add resource/time controls (parallelism, cache sizing, selective artifact retention).
  - Add packaging step where supported:
    - Android: APK/AAB generation and artifact upload.
    - tvOS: project/package generation expectations and signing strategy for CI.
    - WASM: clarify target artifact expectations and add upload.

- macOS full lane
  - Add packaging validation target(s) if required for release confidence.
  - Decide whether signing/notarization checks belong in CI or in dedicated release pipelines.

- Linux full lane
  - Keep hardening cache reuse and failure diagnostics.
  - Expand add-on lane from representative add-ons to `KODI_ADDONS_TO_BUILD=all`.
  - Promote add-on lane from non-blocking to required once stable.

- General
  - Add per-lane timeout/retry strategy to reduce flaky failures.
  - Track CI runtime budget after enabling more `kodi-core` cross builds.
  - Define graduation criteria for each platform (from smoke to required end-to-end gate).

## Suggested Rollout To Full E2E

- Phase 1 (now):
  - Keep smoke checks on cross lanes for fast feedback.
  - Keep Linux/macOS full app superbuild lanes active.
  - Run staged Linux add-on lane (`kodi-e2e`) as non-blocking.

- Phase 2:
  - Expand Linux add-on lane from representative add-ons to `all`.
  - Enable one cross lane at a time to build `kodi-core` (start with Android).
  - Mark new full cross lanes as non-blocking until stable.

- Phase 3:
  - Add packaging/artifact checks per platform.
  - Enable `KODI_SUPERBUILD_ADDONS=ON` per platform after `kodi-core` stability.
  - Promote stable lanes to required checks.

## Example Commands

Build Kodi + one representative add-on:

```sh
cmake -S . -B build-linux-superbuild -G Ninja \
  -DKODI_SUPERBUILD_DEPENDS=ON \
  -DKODI_SUPERBUILD_ADDONS=ON \
  -DKODI_DEPENDS_HOST=x86_64-linux-gnu \
  -DKODI_DEPENDS_TOOLCHAIN=/usr \
  -DKODI_DEPENDS_RENDER_SYSTEM=gl \
  -DKODI_ADDONS_TO_BUILD=pvr.iptvsimple
cmake --build build-linux-superbuild --target kodi-e2e --parallel 2
```

Build Kodi + all supported add-ons:

```sh
cmake -S . -B build-linux-superbuild-all -G Ninja \
  -DKODI_SUPERBUILD_DEPENDS=ON \
  -DKODI_SUPERBUILD_ADDONS=ON \
  -DKODI_DEPENDS_HOST=x86_64-linux-gnu \
  -DKODI_DEPENDS_TOOLCHAIN=/usr \
  -DKODI_DEPENDS_RENDER_SYSTEM=gl \
  -DKODI_ADDONS_TO_BUILD=all
cmake --build build-linux-superbuild-all --target kodi-e2e --parallel 2
```

## Troubleshooting

- Missing add-on definitions
  - Ensure add-on bootstrap ran, or pass `KODI_ADDONS_DEFINITION_DIR`.
- Unsupported platform add-ons
  - Use `supported_addons` in the add-on build tree to inspect resolved set.
- Add-on source download failures
  - Check add-on configure/build logs and tarball fetch output from `cmake/addons`.
- Add-on dependency failures
  - Inspect dependency build output from `HandleDepends.cmake` inside add-on project logs.
- Packaging failures
  - Read `.failure` generated in `build-<superbuild>/addons-build/.failure`.
