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

## Debug Build

Pass `-DCMAKE_BUILD_TYPE=Debug` to CMake to get an instrumented build suited for troubleshooting:

```bash
cmake -S . -B build-wasm-debug \
  -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=/tmp/kodi-wasm-depends/wasm32-unknown-emscripten-release/share/Toolchain.cmake
cmake --build build-wasm-debug -j4
```

What the debug build enables compared to release:

| Flag | Effect |
|---|---|
| `-g -O0` | Full DWARF symbols, no optimisation — sources map 1:1 in DevTools |
| `ASSERTIONS=2` | Extra Emscripten runtime checks (e.g. pointer alignment, table bounds) |
| `SAFE_HEAP=1` | Every heap load/store is bounds-checked; null-pointer dereferences and bad casts surface as a clear abort message instead of an opaque `TypeError` |
| `STACK_OVERFLOW_CHECK=2` | Stack canary on every function call; catches unbounded recursion or oversized stack frames |
| `DISABLE_EXCEPTION_CATCHING=0` | C++ exceptions propagate with a readable message rather than being silently swallowed |
| `--source-map-base` | Browser DevTools fetches the `.wasm.map` source map from `http://localhost:8080/` |

> **Note**: `SAFE_HEAP=1` instruments every memory access and makes the build noticeably slower. Use it to track down memory errors; switch back to release for performance work.

After building, copy `kodi.html` and serve from `build-wasm-debug/` the same way as a release build.

## Running in the Browser

Pthreads require [`SharedArrayBuffer`](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer), which browsers only enable when the page is served with two specific HTTP headers (`COOP`/`COEP`). A plain `python3 -m http.server` will not work — use the provided helper instead.

```bash
cd build-wasm
cp ../tools/wasm/kodi.html .
python3 ../tools/wasm/serve.py          # http://localhost:8080
python3 ../tools/wasm/serve.py 9000     # custom port
```

Then open <http://localhost:8080/kodi.html> in a Chromium-based or Firefox browser.

The server sets:
- `Cross-Origin-Opener-Policy: same-origin`
- `Cross-Origin-Embedder-Policy: require-corp`
- Correct MIME types for `.wasm` and `.js`

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
