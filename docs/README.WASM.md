# Build Kodi for WASM (Emscripten) - WIP

This document tracks the current WebAssembly platform bring-up status.

The WASM target is not fully working yet. This guide documents what is already integrated and what remains to be done.

## Current Status

- `tools/depends` now has a WASM host path for `wasm32-unknown-emscripten`.
- Generated toolchain files are produced under the WASM depends prefix (`Toolchain.cmake`, `config.site`, Meson cross file).
- Main CMake was aligned to the depends-based flow:
  - removed the temporary WASM-only `EmscriptenWasmDeps.cmake` path
  - kept Emscripten built-in ports through `cmake/scripts/wasm/EmscriptenImportedTargets.cmake`
- WASM options remain in `cmake/scripts/wasm/WasmOptions.cmake` (Python/UPnP/testing/etc. disabled for now).

## Build Flow (Current Intended Flow)

```bash
source "$EMSDK/emsdk_env.sh"

cd "$HOME/kodi/tools/depends"
sh bootstrap
autoreconf -fi
./configure \
  --host=wasm32-unknown-emscripten \
  --with-platform=wasm \
  --prefix=/tmp/kodi-wasm-depends \
  --disable-debug
make -j4

cd "$HOME/kodi"
cmake -S . -B build-wasm \
  -GNinja \
  -DCMAKE_TOOLCHAIN_FILE=/tmp/kodi-wasm-depends/wasm32-unknown-emscripten-release/share/Toolchain.cmake
cmake --build build-wasm -j4
```

If you configure depends in debug mode, use the `.../wasm32-unknown-emscripten-debug/...` toolchain path instead.

## Known Incomplete Areas

- `tools/depends make` does not complete end-to-end yet for the WASM flow.
- Main Kodi CMake configure currently blocks on missing dependencies from the depends prefix (for example `FriBidi`) until depends build fully succeeds.
- Some dependency recipes still need WASM validation/tuning in `tools/depends/target/*`.

## Remaining Tasks

1. Stabilize `tools/depends` full build for WASM:
   - ensure native toolchain packages finish cleanly
   - ensure WASM target libs required by Kodi are installed to prefix
2. Confirm required target deps are present in prefix after make:
   - `fribidi`, `fstrcmp`, `tinyxml`, `openssl`, and other required packages
3. Re-run Kodi CMake configure with generated toolchain and resolve remaining missing dependency checks.
4. Build Kodi (`cmake --build`) and record first runtime milestone in browser shell.
5. After first successful build, document exact pinned command sequence and known caveats.

## Notes

- Emscripten built-in ports (`-sUSE_ZLIB`, `-sUSE_FREETYPE`, `-sUSE_HARFBUZZ`, `-sUSE_SQLITE3`) are still the chosen approach for those libraries.
- Fontconfig is intentionally not part of the current WASM bring-up scope.
