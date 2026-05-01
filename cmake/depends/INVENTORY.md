# CMake Native Depends Inventory

This file records the migration baseline from the legacy `tools/depends` graph.
It intentionally mirrors current behavior before recipe-by-recipe CMake rewrites.

## Source Of Truth (Current)

- Native dependency graph: `tools/depends/native/Makefile`
- Target dependency graph: `tools/depends/target/Makefile`
- Per-package recipes: `tools/depends/native/*/Makefile`, `tools/depends/target/*/Makefile`
- Toolchain/config-site generation: `tools/depends/configure.ac`
- CMake consumers of depends metadata: `cmake/scripts/common/ModuleHelpers.cmake`
- ExternalProject patch helper: `cmake/modules/PatchHelpers.cmake`

## Platform Selection Inputs

- `OS`
- `TARGET_PLATFORM`
- `CPU`
- `RENDER_SYSTEM`
- `ENABLE_GPLV3`
- `HAS_ZLIB`
- `NEED_LIBICONV`

## CI/Buildstep Callers

`tools/buildsteps/*/configure-depends` and `tools/buildsteps/*/make-depends` currently
own depends configuration/build. `tools/buildsteps/*/configure-xbmc` configures Kodi.

This migration adds a CMake-managed depends layer under `cmake/depends/` so top-level
CMake can own dependency orchestration while preserving legacy recipes during transition.
