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
    - Native CMake bootstrap now uses CMake's bundled curl instead of requiring host libcurl development headers.

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

- `macos-superbuild-with-addons`
  - Status: **new staged lane (non-blocking)**
  - What is validated:
    - `kodi-e2e` path for macOS superbuild
    - Initial add-on selection flow (`KODI_ADDONS_TO_BUILD=pvr.iptvsimple`)

- `windows-build`
  - Status: **required end-to-end lane exists (native Windows buildsteps)**
  - What is validated:
    - Desktop Windows x64 configure + build via Visual Studio generator
    - BuildDependencies bootstrap through `tools/buildsteps/windows`
    - Required representative add-on build (`pvr.iptvsimple`)
    - Add-on build diagnostics publication (`.success`, `.failure`, `make-addons.error`)
  - Notes:
    - This lane currently follows the established native Windows script path, not `KODI_SUPERBUILD_DEPENDS`.

- `windows-uwp-build`
  - Status: **required end-to-end lane exists (native WindowsStore buildsteps)**
  - What is validated:
    - WindowsStore (UWP) x64 configure + build path
    - UWP dependency bootstrap through `tools/buildsteps/windows/x64-uwp`
    - Required representative UWP add-on build (`pvr.iptvsimple`)
    - UWP/add-on artifact publication for CI diagnostics
  - Notes:
    - Uses the `x64-uwp` wrappers that call `vswhere.bat ... store` to initialize the UWP toolchain environment.

- `macos-tvos-cross-build`
  - Status: **full end-to-end target configured (`kodi-core`)**
  - What is validated:
    - tvOS toolchain/dependency bootstrap config path
    - Full `kodi-core` compile for tvOS
  - Not yet validated:
    - Packaging/signing behavior.

- `macos-tvos-superbuild-with-addons`
  - Status: **new staged lane (non-blocking)**
  - What is validated:
    - `kodi-e2e` path for tvOS superbuild
    - Initial add-on selection flow (`KODI_ADDONS_TO_BUILD=pvr.iptvsimple`)

- `wasm-cross-build`
  - Status: **staged full core lane (`kodi-core`)**
  - What is validated:
    - Emscripten setup
    - Depends bootstrap + Kodi app target (`kodi-core`)
  - Not yet validated:
    - WASM-specific packaging artifact expectations.

- `wasm-superbuild-with-addons`
  - Status: **new staged lane (non-blocking)**
  - What is validated:
    - `kodi-e2e` path for wasm superbuild
    - Initial add-on selection flow (`KODI_ADDONS_TO_BUILD=pvr.iptvsimple`)
  - Notes:
    - Binary add-ons are still disabled at startup in WASM runtime (`ADDONS_CONFIGURE_AT_STARTUP=OFF`), so this lane focuses on build-path validation.

- `android-cross-build`
  - Status: **staged full core lane (`kodi-core`)**
  - What is validated:
    - Android SDK/NDK setup
    - Depends bootstrap + Kodi app target (`kodi-core`)
  - Not yet validated:
    - Packaging artifact publication policy for app outputs.

- `android-superbuild-with-addons`
  - Status: **new staged lane (non-blocking)**
  - What is validated:
    - `kodi-e2e` path for Android superbuild
    - Initial add-on selection flow (`KODI_ADDONS_TO_BUILD=pvr.iptvsimple`)
  - Notes:
    - Lane is intentionally non-blocking while runtime/package integration stabilizes.

## What Is Still Needed For Full End-to-End

For each platform, "full end-to-end" means:
- CMake configure succeeds
- Depends graph builds
- `kodi-core` builds
- Platform packaging artifacts are produced (where applicable)
- (Optional but recommended) smoke runtime validation

Remaining work:

- Cross lanes (`android`, `wasm`, `tvos`)
  - Android/WASM/tvOS now stage `kodi-core` and non-blocking `KODI_SUPERBUILD_ADDONS=ON` lanes.
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

- Windows lanes
  - Decide whether to keep CI on native Windows buildstep scripts or add parity with top-level superbuild orchestration.
  - Expand add-on coverage from representative builds (`pvr.iptvsimple`) to broader supported sets after runtime/perf validation.
  - Evaluate whether desktop/UWP package artifact checks should become strict release gates or remain diagnostic CI outputs.

- General
  - Add per-lane timeout/retry strategy to reduce flaky failures.
  - Track CI runtime budget after enabling more `kodi-core` cross builds.
  - Define graduation criteria for each platform (from smoke to required end-to-end gate).

## Suggested Rollout To Full E2E

- Phase 1 (now):
  - Keep smoke checks on cross lanes for fast feedback.
  - Keep Linux/macOS full app superbuild lanes active.
  - Run staged Linux/macOS/tvOS add-on lanes (`kodi-e2e`) as non-blocking.

- Phase 2:
  - Expand Linux add-on lane from representative add-ons to `all`.
  - Enable cross lanes to build `kodi-core` (Android, WASM and tvOS staged).
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
