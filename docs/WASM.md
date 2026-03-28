# Kodi WebAssembly (Emscripten) port

Experimental build target for running Kodi in the browser via WebAssembly.

## Requirements

- [Emscripten](https://emscripten.org/) SDK (matching the CMake toolchain)
- Dependencies cross-compiled for the `wasm32-unknown-emscripten` target (typically via `tools/depends` with `DEPENDS_DIR` set), including FFmpeg, SQLite, libcurl, etc.

## Configure

From the project root, using the Emscripten wrapper:

```bash
mkdir -p build-wasm && cd build-wasm
emcmake cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCORE_SYSTEM_NAME=wasm \
  -DAPP_RENDER_SYSTEM=gles \
  -DDEPENDS_DIR=/path/to/wasm/prefix
emcmake cmake --build . -j$(nproc)
```

`CMAKE_SYSTEM_NAME` is set to `Emscripten` by `emcmake`; `Platform.cmake` maps this to `CORE_SYSTEM_NAME=wasm`.

## Runtime (browser)

- **Pthreads**: Serve the built `.html` / `.js` with `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` so `SharedArrayBuffer` is available. See [tools/wasm/kodi.html](../tools/wasm/kodi.html).
- **IDBFS**: Profile data can persist under `/kodi_profile` when IndexedDB is mounted (see `xbmc/filesystem/wasm/WasmFilesystem.cpp`).
- **Web Audio**: Optional `Module.webaudioInit` / `Module.webaudioWrite` hooks in the HTML shell for PCM output.

## Implementation map

| Area | Location |
|------|----------|
| Platform / entry | `xbmc/platform/wasm/` |
| Windowing + GLES → WebGL2 | `xbmc/windowing/wasm/` |
| Audio sink | `xbmc/cores/AudioEngine/Sinks/webaudio/` |
| Virtual FS | `xbmc/filesystem/wasm/` |
| Main loop | `CApplication::Run` / `WasmRunIteration` in `xbmc/application/Application.cpp` |
| Process info | `xbmc/cores/VideoPlayer/Process/wasm/` |
| JSON-RPC bridge | `xbmc/interfaces/json-rpc/wasm/` |
| PWA (Phase 9) | `web/manifest.json`, `web/service-worker.js` |

## Limitations

Full parity with desktop Kodi requires cross-compiling all mandatory dependencies and addressing binary addons (no `dlopen` of `.so` in typical WASM deployments). Python and many network services are disabled by default via `cmake/scripts/wasm/WasmOptions.cmake`.
