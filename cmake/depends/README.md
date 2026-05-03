# CMake Native Depends

This directory contains the first migration layer for managing `tools/depends`
from top-level CMake.

## Goals

- Keep `tools/depends` package recipes as the source of truth during migration.
- Move dependency graph orchestration (native + target) into CMake targets.
- Enable a single top-level configure entrypoint via `KODI_SUPERBUILD_DEPENDS`.

## Usage

```sh
cmake -S . -B build \
  -DKODI_SUPERBUILD_DEPENDS=ON \
  -DKODI_DEPENDS_HOST=<host-triplet> \
  [other KODI_DEPENDS_* options]
cmake --build build --target kodi-core
```

Initial presets are also available for common native superbuilds:

```sh
cmake --preset macos-arm64-superbuild
cmake --build --preset macos-arm64-kodi-core
```

Use `cmake --list-presets` to see the platform presets supported by the
current branch.

Key options are defined in `DependsSetup.cmake`, including:

- `KODI_DEPENDS_HOST`
- `KODI_DEPENDS_TARGET_PLATFORM`
- `KODI_DEPENDS_PREFIX`
- `KODI_DEPENDS_TARBALLS`
- `KODI_DEPENDS_TOOLCHAIN`
- `KODI_DEPENDS_SDK_PATH`
- `KODI_DEPENDS_NDK_PATH`
- `KODI_DEPENDS_NDK_API`
- `KODI_DEPENDS_SDK_VERSION`
- `KODI_DEPENDS_VERIFY_LEGACY_GRAPH`
- `KODI_DEPENDS_USE_GENERIC_TOOLCHAIN`

## Migration Helpers

`DependsRecipeHelpers.cmake` now provides transitional package helpers:

- `kodi_depends_add_make_target()` keeps the current Makefile recipe path.
- `kodi_dep_cmake()` is the target helper for CMake-native package recipes.
- `kodi_dep_autotools()` wraps upstream `configure`/`make` packages through
  CMake `ExternalProject`.
- `kodi_dep_meson()` wraps Meson/Ninja packages using generated cross files.
- `kodi_dep_script()` is for rare package-specific script flows.

The Makefile helper remains the default while recipes are migrated one package
family at a time.

## Important Notes

- This layer intentionally mirrors `tools/depends/native/Makefile` and
  `tools/depends/target/Makefile` package selection and dependency edges.
- By default, configure verifies the expanded Make package lists against the
  CMake inventory. Disable with `-DKODI_DEPENDS_VERIFY_LEGACY_GRAPH=OFF` only
  when debugging graph generation itself.
- It is transitional: package recipes are still Makefile-driven.
- Future phases should replace package build recipes with native CMake helpers.
