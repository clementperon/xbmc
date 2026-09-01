# Kodi WebAssembly — rendering architecture

This document describes how Kodi renders its GUI to a web page when built
for the `wasm32-unknown-emscripten` target. It also records the other
designs that were considered, and why we didn't pick them.

**Scope.** GUI presentation only (the path from an OpenGL ES 2/3 draw
call inside Kodi to a pixel on the user's screen). Audio output, video
decoding, and input plumbing are described elsewhere; this document only
refers to them when they influence the rendering design.

---

## 1. Environment and constraints

A Kodi process built for the web is subject to constraints that do not
apply to any native target. The architecture is a direct consequence of
these constraints, so they are worth stating up front.

### 1.1 Everything JavaScript-visible runs in a JS event loop

There is no "OS thread" in the Unix sense. Every WebAssembly thread runs
inside a browser Worker or the browser's main thread, and a Worker only
progresses between JS tasks when its script returns to the event loop.
A thread that never returns is a thread that never sees timers, resize
notifications, `postMessage` deliveries, or any browser-side
housekeeping.

### 1.2 WebGL must be created on the thread that will use it

WebGL contexts created on thread A cannot be used on thread B. They are
not shareable, cloneable, or movable after creation (except via an
`OffscreenCanvas` transfer, which is one-way and destructive).

### 1.3 DOM is only reachable from the main thread

`document`, `window`, `HTMLCanvasElement`, and any element retrieved
from the DOM can only be touched from the browser's main thread.
Workers cannot access them directly.

### 1.4 The main thread also drives compositing

The browser composites the page — and therefore presents pixels to the
screen — on its main thread. Anything that blocks the main thread
blocks compositing, producing stutters, input lag, and eventually the
browser's "unresponsive page" dialog.

### 1.5 Kodi has blocking, nested render loops

Kodi's GUI code is written for native targets and assumes a blocking
main loop. Modal dialogs in particular enter nested render loops:

- `CGUIDialog::Open_Internal` — `while (m_active)
  ProcessRenderLoop();`
- `CGUIWindowManager::CloseWindowSync` — `while (IsAnimating(...))
  ProcessRenderLoop();`

These loops are synchronous: they call `Render()` themselves and do
**not** return control to the caller until the dialog closes or the
animation finishes. Any web architecture that relies on "return to the
JS event loop between frames" breaks modal dialogs completely.

### 1.6 WebGL on `OffscreenCanvas` does not implicitly present

`OffscreenCanvas.transferControlToOffscreen()` returns an object the
spec used to pair with an implicit "update the rendering" step that
copied the worker's drawing buffer onto the placeholder `<canvas>` at
JS task boundaries. Modern Chromium and Firefox do not run that step
reliably when the worker stays busy — and they specifically do **not**
run it while a Worker is inside a blocking loop (see 1.5). The related
WebGL method `WebGLRenderingContext.commit()` was removed from the spec
years ago; Emscripten's `emscripten_webgl_commit_frame()` is a no-op on
current browsers unless `OFFSCREEN_FRAMEBUFFER` is enabled, in which
case it performs a blit-shader copy into a framebuffer we own but does
**not** make the result appear on screen.

---

## 2. Architecture

### 2.1 Thread and canvas layout

```
┌──────────────────────────────────────────────────────────────────┐
│ Browser main thread                                              │
│ ──────────────────────────────────────────────────────────────── │
│   DOM <canvas id="canvas">                                       │
│     └─► ImageBitmapRenderingContext (set up in kodi_pre.js)      │
│   requestAnimationFrame pump                                     │
│     └─► Atomics.add(sharedVsyncCounter, 1) + Atomics.notify      │
│   Module.onKodiFrame(bitmap)                                     │
│     └─► bitmapCtx.transferFromImageBitmap(bitmap)                │
│                                                                  │
│   Input event listeners (keyboard/mouse/resize) registered by    │
│   Emscripten on behalf of the pthread.  Callbacks are proxied    │
│   to the pthread via a shared memory queue.                      │
└──────────────────────────────────────────────────────────────────┘
                        ▲                          │
                        │  ImageBitmap (transfer)  │  vsync tick
                        │  via worker.postMessage  │  via Atomics.notify
                        │                          │
                        │                          ▼
┌──────────────────────────────────────────────────────────────────┐
│ Kodi render pthread (Emscripten Worker)                          │
│ Started by crt1_proxy_main.c -> runs main()                      │
│ ──────────────────────────────────────────────────────────────── │
│   Standalone OffscreenCanvas (created via                        │
│     `new OffscreenCanvas(w, h)`, NOT transferControlToOffscreen) │
│     └─► WebGL2 context                                           │
│         └─► Kodi GUI/RenderSystemGLES                            │
│                                                                  │
│   PresentRenderImpl(rendered):                                   │
│     1. glFlush()                                                 │
│     2. const bm = offCanvas.transferToImageBitmap()              │
│     3. postMessage({cmd:CMD_CALL_HANDLER,                        │
│                     handler:'onKodiFrame',                       │
│                     args:[bm]}, [bm])                            │
│     4. emscripten_futex_wait(&vsyncCounter, last, 100ms)         │
└──────────────────────────────────────────────────────────────────┘
```

### 2.2 Startup

1. The page ships a `<canvas id="canvas">` with no rendering context.
   `kodi_pre.js` runs on the main thread before Emscripten's runtime
   boots and attaches an `ImageBitmapRenderingContext` to that canvas.
   It also installs `Module.onKodiFrame(bitmap)`, the handler that
   will receive frames from the render pthread, and starts the
   `requestAnimationFrame` pump that increments the shared vsync
   counter.
2. `-sPROXY_TO_PTHREAD` causes Emscripten's `crt1_proxy_main.c` to
   spawn a pthread for `main()` instead of running it on the browser's
   main thread. This is the Kodi render thread.
3. `CWinSystemWasmGLESContext::InitWindowSystem` runs on the render
   pthread and, from JS, executes `new OffscreenCanvas(w, h)` and
   `getContext('webgl2', ...)`. The resulting GL context is registered
   with Emscripten's GL layer via `GL.registerContext` so that the
   rest of Kodi can continue to call `gl*` functions and
   `emscripten_webgl_*` APIs unchanged.
4. `CWinSystemWasmGLESContext` also installs the vsync pump (once per
   process) via `MAIN_THREAD_EM_ASM`, giving the render pthread a
   shared `uint32_t` that ticks at display rate.

The DOM canvas and the OffscreenCanvas are two independent canvases.
Kodi renders into the OffscreenCanvas; the DOM canvas displays a
recent frame.

### 2.3 Per-frame path

Inside Kodi's usual "prepare frame → `Render()` → `PresentRenderImpl()`"
cycle, `PresentRenderImpl` does four things:

1. **`glFlush()`.** Guarantees the commands are in the WebGL queue
   before we try to read back the drawing buffer.
2. **`transferToImageBitmap()`.** Returns an `ImageBitmap` that
   references the current drawing buffer contents. The
   OffscreenCanvas's drawing buffer is reset to transparent black by
   this call (standard WebGL "`preserveDrawingBuffer: false`" rules);
   that's fine because Kodi redraws every pass.
3. **`postMessage({cmd: CMD_CALL_HANDLER, handler: 'onKodiFrame',
   args: [bitmap]}, [bitmap])`.** Transfers the bitmap to the main
   thread. Emscripten's libpthread main-thread `onmessage` handler
   dispatches `CMD_CALL_HANDLER` to `Module['onKodiFrame'](bitmap)`.
   The id is numeric (`CMD_CALL_HANDLER`, currently `9`, defined in
   emscripten's `src/lib/libpthread.js`); it was the string
   `'callHandler'` before Emscripten 6. We post the message by hand
   rather than through emscripten's auto-proxied handler stub because
   that stub cannot pass a transfer list. The bitmap is a transferable
   object, so ownership moves from the worker to the main thread with
   no copy.
4. **`emscripten_futex_wait(&vsyncCounter, lastSeen, 100 ms)`.** If
   the vsync counter hasn't advanced since the previous frame, the
   render thread blocks until the next `requestAnimationFrame` fires.
   The 100 ms timeout protects forward progress when rAF is throttled
   (backgrounded tab, "Reduce animation" setting) so Kodi keeps
   running at a slower rate instead of deadlocking.

On the main thread, `Module.onKodiFrame(bitmap)` stores the bitmap as
"pending" and schedules a single `requestAnimationFrame` callback that
calls `bitmapCtx.transferFromImageBitmap(bitmap)`. If a new bitmap
arrives before the rAF fires, the older one is `close()`d and replaced:
we never display a stale frame.

### 2.4 Input

Keyboard, mouse, and resize events are registered against the DOM
(`EMSCRIPTEN_EVENT_TARGET_WINDOW` and `#canvas`) via Emscripten's
`emscripten_set_*_callback` functions. These are sync-proxied from the
pthread to the main thread at registration, and the resulting browser
events are delivered to the pthread via a shared-memory queue. The
render pthread pulls them out with
`emscripten_current_thread_process_queued_calls()` at the top of
`CWinEventsWasm::MessagePump`, so input still flows while a modal
dialog has control of the inner render loop.

Because Kodi renders at the viewport's CSS pixel size (see
`CWinSystemWasmGLESContext::CreateNewWindow`), mouse coordinates are
pixel-identical between the browser event and Kodi's GUI coordinate
space. `WinEventsWasm::TranslateMousePosition` just clamps and forwards
`EmscriptenMouseEvent::targetX / targetY` to Kodi; there is no runtime
rescaling.

### 2.5 Resize

Browser window resize → `EmscriptenUiEvent` → posted to Kodi's
`XBMC_VIDEORESIZE` queue → eventually calls
`CWinSystemWasmGLESContext::ResizeWindow(newW, newH)` → sets the
OffscreenCanvas's `width`/`height` attributes (WebGL's drawing buffer
auto-resizes). The visible DOM canvas's drawing buffer is re-sized
implicitly on the next frame because `transferFromImageBitmap` matches
its canvas to the bitmap's dimensions.

---

## 3. Why this design

The architecture is built around three properties.

**Explicit presentation.** Every frame is delivered by a
`transferToImageBitmap + postMessage` sequence that is fully specified
by WHATWG. There is no "the browser might decide to propagate the
drawing buffer" step anywhere in the pipeline. The visible canvas
updates exactly once per frame Kodi renders, regardless of what the
render thread is doing in between.

**Kodi's nested render loops keep working unchanged.** The render
thread can spin inside `CGUIDialog::Open_Internal` for as long as it
likes. The futex wait in `PresentRenderImpl` paces the loop to the
browser's compositor, so modal dialogs animate at 60 Hz instead of
either (a) running at CPU speed and starving the main thread, or
(b) not rendering at all because they never return to the JS event
loop. No changes to shared Kodi code were required.

**Zero copy on the hot path.** `transferToImageBitmap` is a GPU-side
handle transfer; `postMessage` with a transfer list moves ownership
without structured-cloning the pixels; `transferFromImageBitmap`
hands the bitmap straight to the compositor. On Chrome and Firefox
this is a single GPU texture binding from render-thread to compositor;
the CPU never reads the pixels.

---

## 4. Alternatives considered

### 4.1 `transferControlToOffscreen` from `#canvas` with implicit propagation

**Design.** Transfer the DOM canvas to the render pthread with
`OFFSCREENCANVASES_TO_PTHREAD=#canvas`, create the GL context on the
transferred canvas, and rely on the browser's implicit
"placeholder canvas update" step to copy frames to the visible
canvas.

**Rejected because** it cannot support Kodi's nested modal render
loops (1.5 + 1.6). The implicit update only fires when the worker
returns to its JS event loop; while a modal dialog is open, the
worker is inside a C++ `while (m_active)` loop and never returns.
Frames rendered during the modal are never propagated, so the dialog
is invisible. Sprinkling `emscripten_sleep(0)` in the presentation
path to force returns to the event loop requires Asyncify (see 4.5)
and still did not reliably trigger the propagation on modern
Chromium in our testing.

### 4.2 `OFFSCREEN_FRAMEBUFFER` + `emscripten_webgl_commit_frame()`

**Design.** Let Emscripten render into an offscreen FBO that it
controls, then call `emscripten_webgl_commit_frame()` every frame to
blit-shader the FBO onto a placeholder canvas on main.

**Rejected because** the placeholder still needs (4.1) to propagate
onto the visible canvas, so the nested-loop problem is unchanged.
`emscripten_webgl_commit_frame` is additionally a no-op on modern
browsers unless `OFFSCREEN_FRAMEBUFFER` is set — and even then, all
it does is a CPU-visible blit into an FBO we then have to ship
somewhere else. It also imposes a shader-based blit on every frame
and a driver-level framebuffer allocation.

### 4.3 Run the GL context on the browser main thread

**Design.** Keep the GL context on the main thread and run Kodi on a
pthread that proxies every `gl*` call across the thread boundary
(Emscripten's `GL_PROXY=1` mode or the default
`proxyContextToMainThread` context attribute).

**Rejected because** (a) every `gl*` call becomes a sync round-trip,
which is catastrophic for a GUI that issues thousands of draw calls
per frame, and (b) the render thread constantly blocks the main
thread, which we wanted to keep free to composite and handle input.
The WebGL-on-worker model exists precisely to avoid this.

### 4.4 Run Kodi on the browser main thread

**Design.** Drop pthreads, run `main()` on the browser main thread,
drive everything from an `emscripten_set_main_loop` callback.

**Rejected because** Kodi's startup is synchronous and takes
seconds (skin parsing, addon scanning, database migration, etc.).
Running any of that on the browser main thread hangs the page for
the entire startup duration — no loading indicator, no input, no
paint. And the nested render loops in modal dialogs (1.5) would
still freeze the tab. `PROXY_TO_PTHREAD` solves both.

### 4.5 Asyncify + `emscripten_sleep(0)`

**Design.** Compile with `-sASYNCIFY=1` and insert
`emscripten_sleep(0)` after each commit, unwinding the C stack to
JavaScript so the browser can process one task (notionally including
the OffscreenCanvas → placeholder propagation in 4.1).

**Rejected because** Asyncify instruments every function that could
reach `emscripten_sleep`, saving and restoring its stack frame to
and from linear memory on every call. The wasm size penalty is
typically 30-80 % for a codebase this large, and the per-call cost
is measurable in release builds. Worse, in practice the forced
yield did not reliably cause modern Chromium to run the
placeholder-canvas propagation step inside a `while (m_active)
loop`, so the problem it was introduced to solve was not actually
solved.

### 4.6 `readPixels` + `drawImage` every frame

**Design.** After rendering, read the back buffer with `glReadPixels`,
transfer the resulting typed array to the main thread, and
`drawImage` it into a 2D canvas.

**Rejected because** `glReadPixels` is a GPU → CPU download, one of
the slowest things WebGL can do (synchronises the GPU, can cost
5-15 ms for a 1080p frame on modest hardware). This would run every
frame, adding enough CPU work to blow past our frame budget before
Kodi has drawn anything.

### 4.7 `WebGLRenderingContext.commit()` / explicit swap control

**Design.** Use WebGL's old `commit()` method (paired with
`explicitSwapControl: true` context attribute) to tell the browser
"this frame is done, please present".

**Rejected because** `commit()` was removed from the WebGL spec and
no modern browser implements it. `emscripten_webgl_commit_frame()`
still exists as an Emscripten API but, as noted, it is a no-op on
modern Chromium (see the source of
`emscripten_webgl_do_commit_frame` in
`emscripten/src/lib/libhtml5_webgl.js`).

### 4.8 `SharedArrayBuffer`-backed framebuffer

**Design.** Render into a CPU-side pixel buffer backed by SAB, hand
it to the main thread, `ImageData` + `putImageData` onto a 2D
canvas.

**Rejected because** it requires either software rendering (no WebGL
at all — not tenable for a GUI of Kodi's complexity) or a
`readPixels` step per frame (see 4.6). Also locks us out of hardware
video compositing in the future.

---

## 5. Build configuration

These flags are required for the design described above and are set in
`cmake/scripts/wasm/ArchSetup.cmake`:

| Flag | Why |
|---|---|
| `-sUSE_PTHREADS=1`, `-sPTHREAD_POOL_SIZE=16` | Kodi is multi-threaded. |
| `-sPROXY_TO_PTHREAD` | `main()` must not run on the browser main thread (1.4, 1.5, 4.4). |
| `-sMIN_WEBGL_VERSION=2`, `-sMAX_WEBGL_VERSION=2`, `-sFULL_ES3=1` | Kodi's GLES renderer targets GLES 3 on other platforms. |
| `--pre-js xbmc/platform/wasm/kodi_pre.js` | Main-thread setup of `ImageBitmapRenderingContext` and `Module.onKodiFrame` before the Emscripten runtime boots. |

These flags are deliberately **not** set:

| Flag | Why it's off |
|---|---|
| `-sOFFSCREENCANVAS_SUPPORT=1` | Activates `crt1_proxy_main.c`'s unconditional `transferControlToOffscreen(#canvas)` on pthread startup, which fails because `kodi_pre.js` has already attached a rendering context to `#canvas`. The JS `new OffscreenCanvas(...)` constructor is a browser-native API that does not require this flag. |
| `-sOFFSCREENCANVASES_TO_PTHREAD=#canvas` | Same as 4.1. |
| `-sOFFSCREEN_FRAMEBUFFER=1` | Same as 4.2. |
| `-sASYNCIFY=1` | Same as 4.5. |

---

## 6. Code map

| Concern | File |
|---|---|
| HTML shell | `tools/wasm/kodi.html` |
| Main-thread presentation glue (bitmaprenderer, `Module.onKodiFrame`) | `xbmc/platform/wasm/kodi_pre.js` |
| OffscreenCanvas + WebGL2 creation, `PresentRenderImpl`, vsync pump | `xbmc/windowing/wasm/WinSystemWasmGLESContext.cpp` |
| Input events (keyboard / mouse / resize) | `xbmc/windowing/wasm/WinEventsWasm.cpp` |
| WASM `main()` + `WasmRunIteration` | `xbmc/platform/wasm/ApplicationWasm.cpp`, `xbmc/application/Application.cpp` |
| Link flags | `cmake/scripts/wasm/ArchSetup.cmake` |

Relevant Emscripten source, for reference:

| Concern | File |
|---|---|
| pthread main-thread `onmessage` (handles `CMD_CALL_HANDLER`) | `src/lib/libpthread.js` |
| `crt1_proxy_main` — spawns the pthread that runs `main()` | `system/lib/libc/crt1_proxy_main.c` |
| `emscripten_webgl_commit_frame` — no-op on modern browsers | `src/lib/libhtml5_webgl.js` |

---

## 7. Debugging

- **No frames appear on the visible canvas.** From DevTools on main:
  ```js
  const c = document.getElementById('canvas');
  typeof Module.onKodiFrame;       // 'function'
  c.width + 'x' + c.height;        // non-zero after first frame
  ```
  `kodi_pre.js` emits `console.error` on any failed
  `transferFromImageBitmap`. `PresentRenderImpl` (via its inline
  `EM_ASM` wrapper) emits `console.error` on any failed
  `transferToImageBitmap` / `postMessage`.

- **rAF never advances the vsync counter.** Temporarily add a
  `CLog::Log` in `PresentRenderImpl` printing the vsync counter
  value before and after the futex wait, or read
  `HEAPU32[vsyncCounterAddr >> 2]` from DevTools.

- **Modal dialogs animate choppily.** Confirm the futex wait is
  actually returning on each rAF; if the 100 ms timeout is firing
  every frame, the main thread is not running its rAF pump — check
  for main-thread stalls, and that `kodi_pre.js` actually ran
  (`typeof Module.kodi === 'object'`).

- **Mouse coordinates are wrong.** The render resolution must equal
  the canvas's CSS pixel size for coordinates to be identity. If
  `CWinSystemWasmGLESContext::CreateNewWindow` or `ResizeWindow` is
  changed to render at a different size, either restore the 1:1
  relationship or reintroduce scaling in
  `WinEventsWasm::TranslateMousePosition`.

---

## 8. Planned improvements

- **HiDPI.** Render at `cssSize * devicePixelRatio`; rescale mouse
  input by `devicePixelRatio` in `TranslateMousePosition`. The DOM
  canvas's CSS size stays at 100vw/100vh and the bitmaprenderer
  will display the higher-resolution bitmap scaled to the CSS box.

- **Hardware video.** `VideoFrame` objects from WebCodecs can be
  uploaded zero-copy via `gl.texImage2D(target, level, format,
  videoFrame)` on Chromium; this is compatible with the current
  pipeline because the GL context lives on the render pthread.

- **Tear-free presentation.** Currently a frame is committed every
  time Kodi issues a present; if we wanted stricter pacing we could
  have `PresentRenderImpl` skip the `postMessage` when no dirty
  regions changed and let the main thread keep displaying the
  previous bitmap.
