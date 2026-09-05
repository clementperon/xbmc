# Kodi WebAssembly — rendering architecture

This document describes how Kodi renders its GUI to a web page when built
for the `wasm32-unknown-emscripten` target. It also records the other
designs that were considered, and why we didn't pick them.

**Scope.** Sections 1-8 cover GUI presentation (the path from an OpenGL
ES 2/3 draw call inside Kodi to a pixel on the user's screen). Section 9
covers how decoded video reaches the screen today and the video-plane
design it should move to. Audio output, WebCodecs decoding and A/V sync
are documented in [AVSYNC.md](AVSYNC.md); input is covered only where it
touches rendering.

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

### 1.6 A Worker that never yields cannot release browser resources

Per-frame objects a Worker creates on its own canvas (`transferToImageBitmap`
results, the drawing buffers behind them) are reclaimed by tasks the browser
posts back to that Worker. A pthread that sits inside a Kodi modal loop (1.5)
never runs those tasks, so whatever it produces per frame is never freed for
as long as the dialog is open. This is the constraint that shaped the current
design; §4.9 records how it was found.

The implicit "propagate the OffscreenCanvas to the placeholder canvas" step
that older WebGL designs relied on is affected the same way: it only runs at
task boundaries, which a modal loop never reaches. `emscripten_webgl_commit_frame()`
is only meaningful with `OFFSCREEN_FRAMEBUFFER`, where it blits Emscripten's
emulated back buffer to the canvas.

---

## 2. Architecture

### 2.1 Thread and canvas layout

```
┌──────────────────────────────────────────────────────────────────┐
│ Browser main thread                                              │
│ ──────────────────────────────────────────────────────────────── │
│   DOM <canvas id="canvas">                                       │
│     └─► WebGL2 context, created by Emscripten on behalf of       │
│         the Kodi pthread (proxyContextToMainThread = ALWAYS)     │
│         renders into Emscripten's offscreen framebuffer          │
│   Proxied GL queue: executes the gl* calls the Kodi pthread      │
│     enqueued; emscripten_webgl_commit_frame() blits the          │
│     offscreen framebuffer to the canvas                          │
│   requestAnimationFrame pump                                     │
│     └─► Atomics.add(vsyncTick, 1) + Atomics.notify               │
│   Input event listeners registered by Emscripten for the         │
│     pthread; callbacks proxied to it via a shared-memory queue   │
└──────────────────────────────────────────────────────────────────┘
                        ▲                          │
          gl* calls, commit_frame (proxied)        │ vsync tick (futex)
                        │                          ▼
┌──────────────────────────────────────────────────────────────────┐
│ Kodi render pthread (Emscripten Worker)                          │
│ Started by crt1_proxy_main.c -> runs main()                      │
│ ──────────────────────────────────────────────────────────────── │
│   Kodi GUI / RenderSystemGLES issue gl* calls as usual           │
│   PresentRenderImpl(rendered):                                   │
│     1. emscripten_futex_wait(&vsyncTick, last, 100 ms)           │
│     2. emscripten_webgl_commit_frame()   (synchronous)           │
└──────────────────────────────────────────────────────────────────┘
```

The Kodi pthread owns no canvas and no per-frame browser object. Everything
it produces is a GL command in a shared-memory queue; the thread that owns the
context and the canvas — the browser main thread — is the one that yields
every frame, so the browser can reclaim whatever it allocates.

### 2.2 Startup

1. The page ships a `<canvas id="canvas">` with no rendering context.
   `kodi_pre.js` runs before the Emscripten runtime boots; on the main
   thread it makes the canvas focusable, installs the clipboard-paste
   shim and `Module.onKodiWorkerLog` (§7); in each Worker it forwards
   diagnostic console lines to that handler. It must not take a
   rendering context on the canvas.
2. `-sPROXY_TO_PTHREAD` makes `crt1_proxy_main.c` spawn a pthread for
   `main()`. This is the Kodi render thread.
3. `CWinSystemWasmGLESContext::InitWindowSystem` runs on that pthread. It
   sizes the canvas with `emscripten_set_canvas_element_size("#canvas")`
   (proxied to main), then creates the context with
   `proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_ALWAYS`,
   `explicitSwapControl = 1` and `renderViaOffscreenBackBuffer = 1`.
   Emscripten creates the WebGL2 context on the main thread and hands the
   pthread a token; `emscripten_webgl_make_context_current` binds that
   token to the pthread once. It is never rebound: for a proxied context
   `emscripten_webgl_get_current_context()` returns 0, and a per-frame
   `make_context_current` costs a synchronous main-thread round trip
   (during bring-up it took the frame rate from 120 to 10).
4. `VSYNC::InstallPump` (`WasmVsync.cpp`) installs the `requestAnimationFrame`
   pump on the main thread. Every display frame it stores the frame's
   timestamp in `CurrentHostCounter()` units and an EMA of the refresh rate
   in shared memory (the rate is mirrored to `globalThis.__kodiRefreshRate`
   for debugging), increments a shared `uint32_t` and `Atomics.notify`s
   every waiter: the Kodi render thread (§2.3) and, when *Sync playback to
   display* is on, the video reference clock (AVSYNC.md §4).

### 2.3 Per-frame path

Kodi's GLES code is unchanged. Each `gl*` call lands in Emscripten's GL
library (`system/lib/gl/webgl1.c`, `webgl2.c`), which — because the
calling thread does not own the context — dispatches it to the main
thread:

* Calls without a return value and without pointer arguments
  (`glBindTexture`, `glUseProgram`, `glEnable`, `glScissor`,
  `glDrawArrays`, `glDrawElements`, `glVertexAttribPointer`, …) are
  **asynchronous**: enqueued and returned immediately.
* Calls carrying a pointer to less than 256 KB of data
  (`glUniformMatrix4fv`, `glBufferData`, `glBufferSubData`,
  `glTexImage2D`, `glTexSubImage2D`) copy the data and are asynchronous.
* Calls that return a value or reference memory the caller may reuse are
  **synchronous** round trips: `glGen*`, `glDelete*`, `glGet*`,
  `glCheckFramebufferStatus`, `glFlush`, `glFinish`, `glReadPixels`,
  shader compile/link queries, and uploads of 256 KB or more.

Kodi's per-frame GUI path is entirely in the first two groups; the
synchronous calls happen at init (shader locations, `glGetString`), when a
texture or buffer is first created, and for large texture uploads.
`VerifyGLState()` (a `glGetError` per draw) is compiled out unless
`GL_DEBUGGING` is defined. Without `-sFULL_ES2`/`-sFULL_ES3`,
`glDrawElements` and `glVertexAttribPointer` stay asynchronous; with them
they become synchronous, which is one more reason those flags are off.

`PresentRenderImpl(rendered)`:

1. Returns immediately if Kodi drew nothing (`rendered == false`). That is
   the steady state of a static screen: the offscreen framebuffer keeps the
   previous frame, because Emscripten forces `preserveDrawingBuffer`
   together with `renderViaOffscreenBackBuffer`, so `GetBufferAge()`
   reports 1 and `CGUIWindowManager::Render` repaints dirty regions only.
   With none, `CRenderSystemGLES::PresentRender` sleeps 40 ms, as on
   native targets.
2. **`emscripten_futex_wait(&vsyncTick, lastSeen, 100 ms)`.** Paces the
   loop to the display. If no tick arrives in 100 ms the frame is
   dropped and the function returns, which is what happens while the
   tab is hidden: the browser stops `requestAnimationFrame`, Kodi keeps
   running at ~10 iterations/s and presents nothing.
3. **`emscripten_webgl_commit_frame()`.** Synchronous: the main thread
   executes every GL call still queued, then blits the offscreen
   framebuffer to the canvas. The compositor presents the canvas like
   any other; there is no hand-off object. The synchronous return is
   also the backpressure: the Kodi thread cannot get more than one frame
   ahead of main.
4. The first successful commit calls `Module.onKodiFirstFramePresented`
   on the main thread so the HTML shell can drop its loading overlay.

Measured on an Apple-silicon Mac, Chrome 152, 120 Hz display, Estuary at
1512×862: 120 frames/s with `commit_frame` at 0.2–0.3 ms, both on the home
screen and inside a modal dialog's nested loop.

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
`CWinSystemWasmGLESContext::ResizeWindow(newW, newH)` →
`emscripten_set_canvas_element_size("#canvas", w, h)`, which sets the
canvas backing size and, under `OFFSCREEN_FRAMEBUFFER`, resizes the
offscreen framebuffer with it.

---

## 3. Why this design

**No per-frame browser resources on a thread that cannot yield.** The
Kodi pthread produces GL commands and nothing else. The canvas, the
offscreen framebuffer and the WebGL context belong to the main thread,
which returns to its event loop every frame regardless of what Kodi is
doing, so modal dialogs (1.5) can spin for as long as they like without
anything accumulating. This is the property the previous design lacked
(§4.9).

**Kodi's nested render loops keep working unchanged.** The futex wait in
`PresentRenderImpl` paces the nested loop to the compositor, and the
synchronous `commit_frame` presents from inside it. No changes to shared
Kodi code were required.

**Presentation is the compositor's own.** The canvas is composited
directly. No `ImageBitmap`, no `postMessage`, no second canvas, and the
GUI canvas can be given an alpha channel with a context attribute — which
the video-plane design (§9) needs.

**Idle frames are free.** The offscreen framebuffer is a single buffer that
`commit_frame` only reads, so Kodi's dirty-region tracking works as on a
platform with buffer age 1: a static screen issues no GL calls and no
commit.

**The proxying cost is bounded and measured.** The hot path is
asynchronous (2.3); the main thread does the WebGL work, which on a
two-core TV is where the compositing work already happens.

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
is invisible.

### 4.2 `OFFSCREEN_FRAMEBUFFER` + `emscripten_webgl_commit_frame()` on the pthread

**Design.** Keep the context on the pthread, render into Emscripten's
offscreen FBO and commit it to a placeholder canvas.

**Rejected because** the placeholder still needs (4.1) to propagate onto
the visible canvas, so the nested-loop problem is unchanged. The same
two flags are what make the *main-thread* variant (§2) work; the
difference is which thread owns the context.

### 4.3 Run Kodi on the browser main thread

**Design.** Drop pthreads, run `main()` on the browser main thread,
drive everything from an `emscripten_set_main_loop` callback.

**Rejected because** Kodi's startup is synchronous and takes
seconds (skin parsing, addon scanning, database migration, etc.).
Running any of that on the browser main thread hangs the page for
the entire startup duration — no loading indicator, no input, no
paint. And the nested render loops in modal dialogs (1.5) would
still freeze the tab. `PROXY_TO_PTHREAD` solves both.

### 4.4 Asyncify + `emscripten_sleep(0)`

**Design.** Compile with `-sASYNCIFY=1` and insert
`emscripten_sleep(0)` once per frame, unwinding the C stack to
JavaScript so the pthread returns to its event loop even inside a
modal loop. This would have let the pthread keep owning the canvas.

**Rejected because** Asyncify instruments every function that could
be on the stack between the sleep and the JS boundary, saving and
restoring frames to linear memory on every call. For Kodi's render
path (virtual calls everywhere) the instrumented set is large; the
wasm size penalty is typically 30-80 % and the per-call cost is
measurable in release builds. JSPI, the zero-overhead successor,
shipped in Chromium 137; the Tizen TVs this port targets run Chromium
M120 (Tizen 9.0) and M130 (Tizen 10.0), so it is not available there.

### 4.5 `readPixels` + `drawImage` every frame

**Design.** After rendering, read the back buffer with `glReadPixels`,
transfer the resulting typed array to the main thread, and
`drawImage` it into a 2D canvas.

**Rejected because** `glReadPixels` is a GPU → CPU download, one of
the slowest things WebGL can do (synchronises the GPU, can cost
5-15 ms for a 1080p frame on modest hardware). This would run every
frame, adding enough CPU work to blow past our frame budget before
Kodi has drawn anything.

### 4.6 `WebGLRenderingContext.commit()`

**Design.** Use WebGL's old `commit()` method to tell the browser
"this frame is done, please present".

**Rejected because** `commit()` was removed from the WebGL spec and
no modern browser implements it.

### 4.7 `SharedArrayBuffer`-backed framebuffer

**Design.** Render into a CPU-side pixel buffer backed by SAB, hand
it to the main thread, `ImageData` + `putImageData` onto a 2D
canvas.

**Rejected because** it requires either software rendering (no WebGL
at all — not tenable for a GUI of Kodi's complexity) or a
`readPixels` step per frame (see 4.5). Also locks us out of hardware
video compositing in the future.

### 4.8 Making modal dialogs asynchronous

**Design.** Remove the nested loop instead of working around it.

**Rejected because** it is a Kodi-wide change, not a platform one:
`dialog->Open()` is synchronous by contract at ~140 call sites plus
~36 `ShowAndGet*` helpers, `CGUIDialogBusy::Wait`, `CGUIDialogProgress`,
`CGUIMediaWindow` and `CGUIWindowManager::CloseWindowSync` all spin the
render loop, and the Python and binary add-on dialog APIs are
synchronous.

### 4.9 Standalone `OffscreenCanvas` on the pthread + `transferToImageBitmap` (previous design)

**Design.** The pthread creates its own `OffscreenCanvas` and WebGL2
context (registered with `GL.registerContext`), renders into it, and
per frame calls `transferToImageBitmap()` and `postMessage`s the bitmap
to the main thread, which shows it with an `ImageBitmapRenderingContext`
on the DOM canvas. Explicit presentation, zero-copy hand-off, and it
works inside nested loops. This shipped first.

**Replaced because** it leaked exactly when a modal dialog was open.
Reproduced on an Apple-silicon Mac with Chrome 152, three runs out of
three: with a dialog open, renderer RSS was flat for two to three
minutes and then grew by ~12 GB in 40 s — one full frame buffer per
frame at the display rate — after which `requestAnimationFrame` stopped
and Chrome killed the renderer. The controls that isolated the cause:

| Condition (all at 120 fps) | Renderer RSS |
|---|---|
| dialog open, full hand-off | flat, then +12 GB in 40 s, renderer killed |
| **no dialog**, same 120 fps, same hand-off | flat for 3+ min |
| dialog open, `transferToImageBitmap` skipped | flat for 5.5 min |
| dialog open, main closes each bitmap unshown | slow creep (~0.7 MB/s) |

Wasm memory stayed at 512 MB and the main-thread JS heap flat; the
growth was renderer-side native memory behind the bitmaps. The only
structural difference between the fatal and the healthy rows is that the
modal loop never returns the pthread to its Worker event loop (1.6),
while `emscripten_set_main_loop` does so every frame. On the Samsung TV
the same freeze took seconds: its browser is a 32-bit `armv7` process
with ~3 GB of address space, 512 MB of which is Kodi's wasm heap.

Bounding frames in flight between the two threads (added along the
way, then removed with the rest of the design) was correct but
orthogonal; the frames were consumed, the resources behind them were
not released.

---

## 5. Build configuration

Set in `cmake/scripts/wasm/ArchSetup.cmake`:

| Flag | Why |
|---|---|
| `-pthread`, `-sPTHREAD_POOL_SIZE=4` | Kodi is multi-threaded. The pool only pre-spawns Workers; threads created past it are started by the main thread on demand. |
| `-sPROXY_TO_PTHREAD` | `main()` must not run on the browser main thread (1.4, 1.5, 4.3). |
| `-sOFFSCREEN_FRAMEBUFFER=1` | Enables proxied WebGL contexts and the emulated back buffer that `emscripten_webgl_commit_frame()` blits (§2). |
| `-sGL_SUPPORT_EXPLICIT_SWAP_CONTROL=1` | Allows `explicitSwapControl`; without it Emscripten only commits at main-loop iterations, which a modal loop never reaches. |
| `-sMIN_WEBGL_VERSION=2`, `-sMAX_WEBGL_VERSION=2` | Kodi's GLES renderer targets GLES 3 on other platforms. |
| `--pre-js xbmc/platform/wasm/kodi_pre.js` | Main-thread canvas focus, clipboard paste, worker log forwarding, HTTP-proxy shim. |

Flags set for other subsystems, listed so nobody removes them while
touching rendering: `-sAUDIO_WORKLET` + `-sWASM_WORKERS` (audio sink),
`-lembind` + `--js-library .../webcodecs_bridge.js` (video decoding,
§9), `-sEXPORTED_RUNTIME_METHODS=ccall,cwrap` (paste), `-lidbfs.js`
(persistent userdata), `-sALLOW_TABLE_GROWTH` (CPython).

These flags are deliberately **not** set:

| Flag | Why it's off |
|---|---|
| `-sOFFSCREENCANVAS_SUPPORT=1` / `-sOFFSCREENCANVASES_TO_PTHREAD` | Would transfer `#canvas` to the pthread at startup (4.1); the canvas must stay on the main thread. |
| `-sASYNCIFY=1` | Same as 4.4. |
| `-sFULL_ES2=1` / `-sFULL_ES3=1` | Make `glDrawElements` and `glVertexAttribPointer` synchronous under proxying (2.3), and every GLES path built for wasm uploads through real GPU buffers anyway. |

---

## 6. Code map

| Concern | File |
|---|---|
| HTML shell | `tools/wasm/kodi.html` |
| Main-thread glue: canvas focus, paste, worker log forwarding | `xbmc/platform/wasm/kodi_pre.js` |
| Proxied context creation, `PresentRenderImpl`, display latency | `xbmc/windowing/wasm/WinSystemWasmGLESContext.cpp` |
| Vsync pump: tick count, tick time, refresh rate in shared memory | `xbmc/windowing/wasm/WasmVsync.cpp` |
| Reference-clock video sync fed by the pump | `xbmc/windowing/wasm/VideoSyncWasm.cpp` |
| Input events (keyboard / mouse / resize) | `xbmc/windowing/wasm/WinEventsWasm.cpp` |
| WASM `main()` + `WasmRunIteration` | `xbmc/platform/wasm/ApplicationWasm.cpp`, `xbmc/application/Application.cpp` |
| Link flags | `cmake/scripts/wasm/ArchSetup.cmake` |
| WebCodecs video decoder + JS bridge (§9) | `xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecWebCodecs.cpp`, `webcodecs_bridge.js`, `DVDVideoCodecWebCodecsBridge.h` |
| Audio sink + AudioWorklet ring | `xbmc/cores/AudioEngine/Sinks/AESinkWasmAudioWorklet.cpp`, `xbmc/platform/wasm/WasmAudioWorkletManager.cpp` |

Relevant Emscripten source, for reference:

| Concern | File |
|---|---|
| GL entry points and which are proxied sync vs. async | `system/lib/gl/webgl1.c`, `webgl2.c`, `webgl_internal.h` |
| Proxied context creation, `make_context_current`, `commit_frame` | `src/lib/libhtml5_webgl.js` |
| Offscreen framebuffer, canvas resize | `src/lib/libwebgl.js`, `src/lib/libhtml5.js` |
| `crt1_proxy_main` — spawns the pthread that runs `main()` | `system/lib/libc/crt1_proxy_main.c` |

---

## 7. Debugging

- **Worker console output.** Kodi's log and the `[KODI_DBG]`
  instrumentation are written from Workers. `kodi_pre.js` forwards lines
  matching `KODI_DBG|[kodi]|WASM|WebGL|GL_|lost|ERROR|error` to the main
  thread, where they appear in the console prefixed `[worker]` and are
  kept in `Module.kodi.workerLog` (last 1000 entries). This is what makes
  them reachable from tooling that only sees the main-thread console.

- **Present stats.** Once a second `PresentRenderImpl` logs
  `[KODI_DBG] ... present stats | {"frames","timeouts","avgWaitMs",
  "avgCommitMs","maxCommitMs","tick"}`. Healthy: `frames` at the display
  rate, `timeouts` 0, `avgCommitMs` well under a millisecond. `timeouts`
  at ~10/s with `avgWaitMs` ≈ 100 means no rAF ticks — check
  `document.visibilityState` first; a hidden tab is the normal cause.

- **Frame rate collapses to ~10 fps while the tab is visible.** Either
  the pump is not installed (`globalThis.__kodiRefreshRate` undefined on
  main) or something on the Kodi thread is doing a synchronous
  main-thread round trip per frame; `emscripten_webgl_make_context_current`
  inside the frame loop was the first offender.

- **Only the control that changed repaints; the rest is black.** The back
  buffer is not being preserved. `GetBufferAge()` returning 1 relies on the
  offscreen framebuffer surviving `commit_frame`; check that
  `renderViaOffscreenBackBuffer` is still set in `CreateProxiedGLContext`
  and that nothing takes another context on, or clears, `#canvas`.

- **Nothing renders.** `Module.ctx` must be set on main and Kodi's log
  must show `WASM: using main-thread WebGL2 context proxied to the Kodi
  thread`. A `bitmaprenderer` or `2d` context taken on `#canvas` before
  Emscripten runs makes `getContext('webgl2')` fail.

- **Mouse coordinates are wrong.** The render resolution must equal
  the canvas's CSS pixel size for coordinates to be identity. If
  `CWinSystemWasmGLESContext::CreateNewWindow` or `ResizeWindow` is
  changed to render at a different size, either restore the 1:1
  relationship or reintroduce scaling in
  `WinEventsWasm::TranslateMousePosition`.

---

## 8. Planned improvements

- **HiDPI.** Render at `cssSize * devicePixelRatio`; rescale mouse
  input by `devicePixelRatio` in `TranslateMousePosition`.

- **Video.** See §9.

---

## 9. Video: current path and the video-plane design

### 9.1 Target constraints

The primary target is Samsung Tizen TVs, where the browser process is
typically confined to one or two CPU cores. Every design choice below is
judged by one number: how many times a decoded frame's pixels cross the
CPU. A 1080p YUV frame is ~3 MB; at 60 fps each CPU pass over it costs
~180 MB/s of memory bandwidth plus the cycles to drive it, and 4K is
four times that.

### 9.2 Current path (texture upload)

`CDVDVideoCodecWebCodecs` drives a `VideoDecoder` that lives on the
browser main thread through an Emscripten `--js-library`
(`webcodecs_bridge.js`); every bridge call is `__proxy: 'sync'`. State
the codec needs to poll — queued frames, in-flight work, backpressure,
failure, the next frame's size — is mirrored by the JS side into a
`WebCodecsSharedState` struct in wasm memory with `Atomics`, and a
`signal` word is bumped and `Atomics.notify`'d on every change so the
VideoPlayer thread can `emscripten_futex_wait` instead of polling
through the proxy. Decoded `VideoFrame`s stay open in a JS queue until
the codec asks for one; `copyTo()` then writes straight into the
`CVideoBuffer`'s memory (the wasm heap is a `SharedArrayBuffer` under
pthreads). The frame reaches the screen through `CLinuxRendererGLES` as
a sysmem YUV420P/NV12 picture: `glTexImage2D` on the render pthread and
the YUV→RGB shader.

Per frame that is two CPU passes — the `copyTo` readback (GPU→CPU for a
hardware decoder, memcpy for a software one) and the WebGL texture
upload — plus one GPU pass for colour conversion. It is the cheapest a
sysmem pipeline can be, and it is still the wrong pipeline for a
two-core TV at 1080p60.

Drain semantics follow the other Kodi codecs: `DVD_CODEC_CTRL_DRAIN`
triggers `VideoDecoder.flush()`, which releases the reorder buffer, and
the codec then skips delta packets until the next keyframe.

### 9.3 Design: video plane under the GUI

The design Kodi already uses on comparable hardware is a **video
plane**: the decoder never hands pixels to Kodi, the renderer only
positions and times an opaque frame handle, and the platform compositor
stacks the video under a GUI drawn with a transparent hole. This is
`CRendererMediaCodecSurface` on Android and the DRM-PRIME plane renderer
on GBM. In the browser it maps onto:

```
┌──────────────────────────────────────────────────────────────────┐
│ Browser main thread                                              │
│   VideoDecoder ──► Map<id, VideoFrame>   (frames never leave)    │
│   <canvas id="video">   ◄── present(id, rect): draw / transfer   │
│   <canvas id="canvas">  ◄── GUI WebGL canvas (existing path, §2) │
│         GUI canvas is alpha-enabled; video area cleared to 0     │
└──────────────────────────────────────────────────────────────────┘
          ▲ shared state (Atomics) + postMessage           │ ids
          │                                                ▼
┌──────────────────────────────────────────────────────────────────┐
│ Kodi threads                                                     │
│   VideoPlayerVideo: GetPicture() → CVideoBufferWebCodecs{id,pts} │
│   RenderManager / render pthread: Render() → present(id, rect)   │
│   CVideoBuffer release → release(id) → VideoFrame.close()        │
└──────────────────────────────────────────────────────────────────┘
```

**Codec.** Unchanged in structure. `GetPicture` returns a
`CVideoBufferWebCodecs` carrying only an id, pts and dimensions; the JS
side keeps the `VideoFrame` in a map keyed by id. No `copyTo`, no
`WebCodecsFrameInfo` payload.

**Renderer.** A new `CRendererWebCodecsPlane` registered for that
buffer type. `Configure` records geometry; `RenderUpdate` computes the
destination rect exactly as the GLES renderer does today (aspect,
zoom, stereo, view mode) and sends `present(id, rect)`; `ManageRenderArea`
and `RenderCapture` behave like the MediaCodec surface renderer (capture
unsupported or via `drawImage` on request). It draws nothing into the GL
context. The GUI canvas must be created with `alpha: true` — a context
attribute in `CreateProxiedGLContext`, since the canvas is composited
directly (§2) — and the video window cleared to transparent, which is
what the Android and GBM windowing code already does for their planes.

**Presentation.** The main thread keeps one pending `(id, rect)` per
video canvas and applies it from the rAF pump (§2.2), so the video plane
and the GUI canvas update in the same compositor frame. Presenting is `bitmaprenderer.transferFromImageBitmap(await
createImageBitmap(frame))` or, where a `VideoFrame` can be drawn
directly, `2d.drawImage(frame, rect)`. Both are compositor operations:
no CPU pass over the pixels. Timing stays with `CRenderManager`; the
plane only shows what it was last told to show.

**Buffer lifetime.** Hardware decoders have a fixed output pool and
stall when too many frames are held open, so `VideoFrame.close()` must
follow `CVideoBuffer::Release()` promptly. The render buffer queue
(3-4 pictures) plus the codec's in-flight cap is well inside typical
pool sizes; the existing `busy` backpressure already counts open
frames.

**Protocol.** Two new bridge calls, `webcodecs_present(handle, id,
x, y, w, h)` and `webcodecs_release(handle, id)`, both cheap and
`__proxy: 'async'` — nothing needs to wait for them. The rest of the
`WebCodecsSharedState` protocol carries over unchanged.

**Resize / HiDPI.** The video canvas is sized with the GUI canvas
(§2.5). Because the plane is composited, rendering the GUI at CSS size
while the video shows at native decode resolution is free, which is the
right split on a TV.

### 9.4 Backends

The renderer/codec split above is deliberately backend-neutral:

- **WebCodecs + canvas** — any Chromium or Firefox with WebCodecs.
- **Tizen Elementary Media Stream Source (EMSS)** — Samsung's
  WASM-specific media API feeds compressed packets straight to the TV's
  hardware decoder and video plane (audio too). Same shape: the codec
  appends packets, the renderer positions a plane. On Samsung TVs this
  is expected to be the efficient path and avoids depending on whether
  the TV's browser exposes hardware-accelerated WebCodecs at all.

Which backend is built first depends on what the target TV actually
offers, so the first step is to probe `VideoDecoder.isConfigSupported`
with `hardwareAcceleration: 'prefer-hardware'` and EMSS availability on
real hardware.

### 9.5 Alternatives considered

- **`gl.texImage2D(target, …, videoFrame)` on the render pthread.**
  Zero-copy on paper, but a `VideoFrame` is a JS object that can only
  reach the render pthread by `postMessage`, and that pthread never
  returns to its Worker event loop (it blocks in `Atomics.wait`, §1.5),
  so the message is never delivered. It would need the GL context on a
  thread that yields — i.e. the video-plane design anyway.
- **A dedicated decode Worker owning the `VideoDecoder`.** Removes work
  from the main thread but removes no CPU work; on a two-core TV it is
  a scheduling shuffle plus one more thread. Not pursued.
- **Software YUV→RGB in wasm + `putImageData`.** Adds a CPU pass rather
  than removing one.
