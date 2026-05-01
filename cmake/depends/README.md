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

## Important Notes

- This layer intentionally mirrors `tools/depends/native/Makefile` and
  `tools/depends/target/Makefile` package selection and dependency edges.
- It is transitional: package recipes are still Makefile-driven.
- Future phases should replace package build recipes with native CMake helpers.
