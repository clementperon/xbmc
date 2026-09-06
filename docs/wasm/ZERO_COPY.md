# Kodi WebAssembly — zero-copy video: WebCodecs frames as WebGL textures

**Status: implemented, TV validation pending** (September 2026). Steps 1 to
5 of §7.1 are in the tree; the sysmem copy path stays as the fallback until
the validation of §7.2 is done, then step 6 removes it. Verified so far in
desktop Chrome on macOS: H.264 at 640×360 (software `I420` frames), 1080p60
and 2160p30 (hardware `NV12` frames) play through the texture import at the
source frame rate with no dropped frames, pause, frame step and seek behave,
and the copy fallback, forced by hand, still plays through the YUV shader.
The TV numbers of §7.2 and the answers to §7.3 are still open. This is
the companion of [AVSYNC.md](AVSYNC.md), which describes the WebCodecs bridge
and the clocks, and of [RENDERING.md](RENDERING.md), which describes the GUI
presentation path and the threading rules the browser imposes. Their
vocabulary is used here without being re-explained. RENDERING.md §9.3
describes a video-plane design; this document replaces it as the next step and
§6 explains why.

**Scope.** §1 what the current path costs and why libswscale is in it, §2 the
observation the design rests on, §3 the architecture, §4 the frame protocol,
§5 the impact on CPU, memory, latency and features, §6 the alternatives, §7
the implementation plan and how to validate it on the TV.

---

## 1. The problem

The decoder is a black box on the far side of the browser main thread, and
with Kodi's generic sysmem YUV renderer a decoded frame is dragged through
the CPU to get from one to the other. That is the path this design replaced,
and the one the fallback of §4.7 still takes. For a 1080p frame from the
Samsung Tizen decoder, which outputs packed RGB (`RGBX`, 8.3 MB):

```
VideoDecoder output                 VideoFrame, GPU- or CPU-backed, RGBX 1920×1080
   │  VideoFrame.copyTo()           pass 1  GPU→CPU readback into the wasm heap, 8.3 MB   (browser, main thread)
   ▼
CVideoBufferSysMem, RGB landing zone
   │  sws_scale()                   pass 2  RGB→YUV420P, 8.3 MB in / 3.1 MB out          (VideoPlayer thread)
   ▼
CVideoBufferSysMem, YUV420P
   │  glTexSubImage2D ×3 planes     pass 3  Emscripten memdup()s each <256 KB band, 3.1 MB (render thread)
   │                                pass 4  ~12 texSubImage2D calls copy 3.1 MB again      (main thread)
   ▼
three GL textures
   │  YUV→RGB GLSL                  GPU, at output resolution
   ▼
Kodi framebuffer ─► commit blit ─► canvas ─► compositor
```

Four CPU passes over the pixels, about 23 MB of memory traffic per frame, on
a device whose browser is confined to one or two cores. In the TV profiles of
this branch, `sws_scale` was the largest single item on the VideoPlayer
thread at about 7 ms per frame for a 640×360 stream; the cost is linear in
pixels, so 1080p is on the order of 60 ms, longer than a 60 fps frame period,
and 4K is out of reach. It also quantises full-range RGB to 4:2:0 before the
GPU turns it back into RGB.

libswscale is there for one reason: `CLinuxRendererGLES` only takes planar or
packed YUV, and converting on the CPU was the shortest route to a picture on
screen. The decoder produced RGB and the GPU wanted RGB; the two passes in
between exist only because the pixels had to go through Kodi's memory at all.

---

## 2. The observation

Three facts, all already true in this branch, make the copy unnecessary:

1. **The WebGL context lives on the browser main thread.** RENDERING.md §2:
   the context is created with `proxyContextToMainThread = ALWAYS` and every
   GL call from the Kodi render pthread is marshalled to main by Emscripten's
   GL library. The `WebGLTexture` objects behind Kodi's texture ids, in
   Emscripten's `GL.textures` table, are main-thread objects.
2. **`VideoFrame`s live on the main thread too.** The `VideoDecoder` runs
   there (AVSYNC.md §3.2) and its output frames stay in a JS structure until
   the codec asks for them.
3. **WebGL accepts a `VideoFrame` as a texture source.** `texImage2D(target,
   level, internalformat, format, type, videoFrame)` is a `TexImageSource`
   overload. For a GPU-backed frame, which is what a hardware decoder
   produces, the browser copies GPU→GPU inside its GPU process; for a
   CPU-backed frame from a software decoder it uploads once. In both cases
   the browser also does whatever conversion the frame needs, YUV→RGB,
   10-bit→8-bit, limited→full range, using the frame's own `colorSpace`.
   No pixel enters the wasm heap.

So the frame never has to travel. The only thing that crosses threads is a
message from the render pthread saying *upload frame N into texture T*, and
that message is an asynchronous proxied call, exactly like every GL call the
render thread already makes.

RENDERING.md §9.5 rejected `texImage2D(videoFrame)` because "a `VideoFrame`
can only reach the render pthread by `postMessage`". That was written for the
design of §4.9, where the context lived on the pthread. With the proxied
context the frame and the texture are on the same thread and the objection
no longer holds.

---

## 3. Architecture

```
┌──────────────────────────────────────────────────────────────────────────┐
│ Browser main thread                                                      │
│   VideoDecoder.decode(chunk) ──► output(frame): frames.set(seq, frame)   │
│                                   ring[seq % N] = {w, h, pts, colour…}   │
│                                   framesProduced = seq + 1  (Atomics)    │
│   upload(seq, tex):  gl.bindTexture(GL.textures[tex]);                   │
│                      gl.texImage2D(…, frame); frame.close()              │
│   release(seq):      frame.close()  (never-uploaded frames only)         │
│   Kodi's proxied GL calls and the commit blit, in queue order            │
└───────────────▲──────────────────────────▲─────────────────▲─────────────┘
   push (async) │ shared state + ring       │ upload (async)  │ release (async)
┌───────────────┴──────────────┐ ┌──────────┴────────────┐ ┌─┴───────────────┐
│ VideoPlayerVideo thread      │ │ Render pthread        │ │ any thread      │
│ AddData → push_packet        │ │ CRendererWebCodecs    │ │ CVideoBuffer    │
│ GetPicture:                  │ │  UploadTexture(idx):  │ │  ::Release()    │
│  framesTaken < framesProduced│ │   upload(seq, tex)    │ │  refcount → 0   │
│  read ring[framesTaken % N]  │ │  RenderHook: bind tex,│ │  pool.Return    │
│  pool.Get() → buffer{seq}    │ │   RGBA GUI shader,    │ │   → release(seq)│
│  framesTaken++  (no JS call) │ │   one quad            │ │                 │
└──────────────────────────────┘ └───────────────────────┘ └─────────────────┘
```

### 3.1 Bridge (`webcodecs_bridge.js`)

The decoder driver keeps its structure: one `VideoDecoder` per codec
instance, `push_packet` asynchronous, `WebCodecsSharedState` mirrored with
`Atomics` and a `signal` futex. What changes is what happens to an output
frame. Instead of being queued for a `copyTo`, it is stored in a `Map` keyed
by a per-decoder sequence number, and its metadata is written into a ring of
`WebCodecsFrameInfo` slots inside the shared state (§4.1). Two new calls
take over from `copy_next_frame` and `discard_next_frame`:

| Call | Proxy | Does |
|---|---|---|
| `webcodecs_upload_frame(handle, seq, glTexture)` | async | `texImage2D` the frame into Kodi's texture, then `close()` it |
| `webcodecs_release_frame(handle, seq)` | async | `close()` a frame that will never be uploaded (dropped, flushed, end of stream) |

Neither returns anything, so the video and render threads never wait for
the main thread. The synchronous calls stay what they are: create, reset,
destroy, stats, error text, plus the one-off capability probe of §4.7.
Until step 6 of §7.1 removes the sysmem fallback, the copy survives as
`webcodecs_copy_frame(handle, seq, copyId, dst, dstSize)`: it copies the
frame the codec has already taken from the ring, so `discard_next_frame` is
gone in both paths and the drop path is the same for both.

### 3.2 `CVideoBufferWebCodecs`

A `CVideoBuffer` that carries no pixels: the decoder handle, the sequence
number, dimensions and the colour metadata from the frame. Its pool grows
on demand like the sysmem pool, and its `Return(id)` sends
`webcodecs_release_frame`. Because Kodi's reference counting already
calls `Release()` on every path that stops using a picture, VideoPlayerVideo
dropping it, the render manager recycling its slot, a flush, a stream
close, the `VideoFrame` is closed on every one of those paths without any
special casing. `GetMemPtr()`, `GetPlanes()` and `GetStrides()` return
nothing; nothing reads them.

### 3.3 `CRendererWebCodecs`

A `CLinuxRendererGLES` subclass using the hooks the HwDecRender renderers
use, modelled on `CRendererMediaCodec`:

| Hook | Behaviour |
|---|---|
| `Create(CVideoBuffer*)` / `Register()` | returns a renderer only for a `CVideoBufferWebCodecs`; registered under `"webcodecs"` from `InitWindowSystem` next to the codec; `CLinuxRendererGLES` stays the `"default"` for FFmpeg pictures |
| `LoadShadersHook()` | `m_textureTarget = GL_TEXTURE_2D`, `m_renderMethod = RENDER_CUSTOM`; no YUV shaders are compiled |
| `CreateTexture(index)` | `glGenTextures` one texture, filter and wrap parameters, no storage; `texwidth/texheight` = source size, `pixpertex` 1 |
| `UploadTexture(index)` | `webcodecs_upload_frame(handle, seq, plane.id)`, `CalculateTextureSourceRects(index, 1)`; `loaded` then keeps it from repeating |
| `RenderHook(index)` | bind the texture, `EnableGUIShader(SM_TEXTURE_RGBA)` with the brightness/contrast uniforms, draw the quad on `m_rotatedDestCoords`, `DisableGUIShader` |
| `DeleteTexture(index)` | `glDeleteTextures` (synchronous under proxying, like `glGenTextures`; both happen per `Configure`/`Flush`, not per frame) and `ReleaseBuffer(index)` |
| `GetRenderInfo()` | `max_buffer_size = 4` |
| `Supports(ESCALINGMETHOD)` | `LINEAR`, `NEAREST`, `AUTO`; a change of method mid-stream is applied to the textures from `RenderHook`, since the base class only refilters `RENDER_GLSL` renderers |
| `Supports(ERENDERFEATURE)` | the base set minus `TONEMAP`, which the browser owns here |

Everything the base class does with geometry, aspect, zoom, view modes,
vertical shift, pixel ratio, rotation, stereo source rects, is inherited,
and because the frame is drawn into Kodi's framebuffer, the OSD, subtitles,
GUI blending and the capture path (`VideoBypassesFramebuffer()` is false)
keep working unchanged.

`CLinuxRendererGLES::Configure` refuses a picture whose
`videoBuffer->GetFormat()` maps to `SHADER_NONE`, because the base class has
no host planes to upload for it. A `RENDER_CUSTOM` subclass brings its own
upload, so the gate becomes `m_renderMethod != RENDER_CUSTOM &&
GetShaderFormat() == SHADER_NONE`, and `CRendererWebCodecs` sets
`m_renderMethod = RENDER_CUSTOM` in its constructor (`LoadShadersHook` sets
it again later, as `CRendererMediaCodec` does). That is the whole hook: no
new virtual, and `GetShaderFormat()`, which logs an error for unknown
formats, is not consulted for custom renderers. The buffer still reports the
decoder's real format where FFmpeg has a name for it (`RGB0`, `NV12`,
`YUV420P10`, …) so `ConfigChanged` reconfigures if the decoder switches
output format mid-stream.

One base-class cost to know about: `ValidateRenderTarget` runs
`UpdateVideoFilter`, whose `SetTextureFilter` asks `glIsTexture` for every
field, plane and buffer (36 synchronous round trips with four buffers) once
per `Configure` or `Flush`. `CRendererMediaCodec` pays the same; it is not
in the per-frame path.

### 3.4 Codec (`CDVDVideoCodecWebCodecs`)

`AddData`, keyframe classification, codec strings, backpressure, drain and
the error path are unchanged. `GetPicture` no longer allocates sysmem
buffers, waits for a copy, or runs libswscale: it reads the next ring slot,
takes a `CVideoBufferWebCodecs` from the pool and fills the `VideoPicture`.
It makes no call into JS at all. `DiscardNextFrame`'s synchronous call goes
away now; the `SwsContext`, the RGB landing pool, the YUV pool and
`WaitForCopy` stay behind the fallback flag until step 6 of §7.1. The colour
fields of the picture come from the frame's `colorSpace` (§5.5), falling
back to the stream hints for the fields the browser leaves `null` so a
decoder that reports metadata on some frames only does not make
`CRenderManager::Configure` reconfigure on every change, and the
process-info pixel format becomes the `VideoFrame.format` string (`RGBX`,
`NV12`, `I420P10`, …). The ring carries that format as a
`WebCodecsPixelFormat` value whose enumerators are named exactly like the
WebCodecs strings, so the JS side maps `frame.format` to it by name through
the Embind table and the codec maps it back to the string, and to the
`AVPixelFormat` the buffer reports.

### 3.5 Build

`RendererWebCodecs.{h,cpp}` in `VideoRenderers/HwDecRender/` behind
`CORE_SYSTEM_NAME STREQUAL wasm`; `CVideoBufferWebCodecs` in the codec
header, where the other platform codecs keep their buffer types; the bridge
JS library and the ABI header are the existing files.

---

## 4. Frame protocol

### 4.1 Shared state and the frame ring

```c
struct WebCodecsFrameInfo            // one per output frame, 80 bytes
{
  int32_t width, height;             // visibleRect
  int32_t displayWidth, displayHeight;
  int32_t pixelFormat;               // WebCodecsPixelFormat, named after VideoFrame.format
  int32_t keyFrame;
  int32_t colorMatrix, colorPrimaries, colorTransfer, fullRange;  // from frame.colorSpace
  int32_t payloadSize;               // sysmem fallback only: tightly packed copy size
  int32_t yStride, uStride, vStride, uOffset, vOffset;            // sysmem fallback only
  double ptsSeconds, durationSeconds;
};

struct WebCodecsSharedState
{
  int32_t signal;                    // bumped + Atomics.notify on every change
  int32_t framesProduced;            // JS → C++: sequence number of the next output
  int32_t framesTaken;               // C++ → JS: sequence number GetPicture will read next
  int32_t inflight;                  // decodeQueueSize (+1 while a fallback copy runs)
  int32_t failed;
  int32_t pushesProcessed;
  int32_t copyDone, copyResult;      // sysmem fallback only
  struct WebCodecsFrameInfo ring[WEBCODECS_FRAME_RING];   // slot = seq % WEBCODECS_FRAME_RING
};
```

`queuedFrames` is `framesProduced - framesTaken`, computed by whichever side
needs it. `nextPayloadSize` and `nextPixelFormat` disappear, and with them
the half-published metadata case AVSYNC.md §3.2 describes: the JS side
writes the ring slot *before* it publishes `framesProduced`, so a frame
count is never visible without its metadata. The slot fields marked
"sysmem fallback only", and `copyDone`/`copyResult`, leave with step 6 of
§7.1. The colour fields use FFmpeg's `AVCOL_*` values, exported through the
same Embind table as the other enums, with the `*_UNSPECIFIED` value for a
`null` field; `AVCOL_SPC_RGB` is 0, so 0 cannot mean "unknown" there.

The JS side writes the ring through `HEAP32`/`HEAPF64` directly. Under
pthreads with memory growth Emscripten's link step rewrites every such
access into `(growMemViews(), HEAP32)[…]`, so the view is current at each
access; what must not happen is caching a view in a local across an `await`,
which the existing bridge already avoids (its `copyTo` destination is a
`subarray` of shared memory, which never detaches).

The ring holds `WEBCODECS_FRAME_RING = 32` slots. A slot is rewritten only
`WEBCODECS_FRAME_RING` outputs later, and the output callback refuses (closes
and counts as dropped) a frame while `framesProduced - framesTaken ==
WEBCODECS_FRAME_RING`, so a slot the codec has not read yet is never
overwritten. This replaces `FRAME_QUEUE_HIGH_WATER`; the codec's in-flight
cap keeps the queue far below it in practice.

### 4.2 Sequence of one frame

1. **Output.** `output(frame)`: `seq = framesProduced`; store the frame in
   the map, write `ring[seq % N]`, `Atomics.store(framesProduced, seq + 1)`,
   bump `signal`, notify.
2. **Take.** `GetPicture()`: if `framesTaken == framesProduced`, `VC_BUFFER`
   (with the existing futex wait when the decoder is busy). Otherwise copy
   `ring[framesTaken % N]` out, get a buffer from the pool, record `(handle,
   seq)` in it, and only then publish `framesTaken + 1`: once it is
   published the output callback may reuse the slot. Fill the picture from
   the copy, return `VC_PICTURE`. No proxied call.
3. **Upload.** The render manager picks the picture for a display frame,
   `CLinuxRendererGLES::Render` calls `UploadTexture(index)`, which queues
   `upload(seq, tex)`. On main: look the frame up, bind Kodi's texture,
   `texImage2D`, `close()`, drop it from the map. A frame that is no longer
   in the map (closed by a reset that overtook it) leaves the texture as it
   was; the render manager is discarding that buffer anyway.
4. **Draw.** `RenderHook` binds the same texture and draws. The commit blit
   at `PresentRenderImpl` is synchronous, so by the time the render thread
   continues, upload and draw have executed on main.
5. **Release.** When the last reference to the `CVideoBufferWebCodecs`
   goes, `Return(id)` queues `release(seq)`; on main it closes the frame if
   the upload has not already done so (it normally has), and is a no-op
   otherwise.

### 4.3 Ordering

Every message above goes through one FIFO. Emscripten's proxied GL calls
(`system/lib/gl/webgl1.c`) dispatch through `proxying_legacy.c`, whose
`do_dispatch_to_thread` calls
`emscripten_proxy_async(emscripten_proxy_get_system_queue(), main, …)`; a JS
library function with `__proxy: 'async'` or `'sync'` goes through
`proxying.c` to `emscripten_proxy_async`/`emscripten_proxy_sync` on the
*same* system queue. That queue keeps one task list per target thread, so
every task addressed to the main thread executes in the order it was
enqueued, whichever thread enqueued it. Hence:

- `upload(seq → T)` is enqueued by the render thread before its own
  `glBindTexture(T)` and draw, so it runs before them.
- `release(seq)` can only be enqueued after the buffer's last user let go.
  For a presented picture that user is `CRenderManager::FrameMove`, which
  runs on the render thread at the start of the *next* GUI frame: it moves
  the previous present source to `m_discard` and releases it in the same
  call, before `Render` uploads the new one. The previous frame's commit was
  synchronous, so the release is enqueued after its upload has *executed*,
  not merely after it was enqueued. `Flush` and `UnInit` run on the same
  thread and start with `glFinish`, a synchronous call, before they delete
  textures and release buffers.
- A synchronous call (`glGenTextures` in `CreateTexture`,
  `wasm_webgl_commit_frame`) waits for everything queued before it, so a
  frame's upload is on the GPU before its commit.

The only ordering the design does *not* rely on is between the VideoPlayer
thread and the main thread's output callback, and that is covered by the
ring's publish order (§4.1).

### 4.4 Lifetime and backpressure

A `VideoFrame` holds a decoder output buffer, and hardware decoders have a
small fixed pool of them; Chromium's `VideoDecoder` stops producing output
while too many frames stay open. The design closes a frame at the earliest
moment its pixels are safe, the upload, rather than when Kodi recycles the
render buffer some frames later. Kodi never re-uploads a buffer: `loaded`
stays set until `ReleaseBuffer`, and `DeleteTexture` releases the buffer
along with the texture, so a closed frame is never needed again. Open frames
are therefore bounded by pushed-but-not-run + decoding + queued (the
existing `WEBCODECS_MAX_INFLIGHT = 12` rule, unchanged) plus the frames
taken but not yet uploaded: the render manager queues up to
`max_buffer_size - 1 = 3` pictures behind the one on screen and
VideoPlayerVideo can hold one more while it waits for a free slot, so up to
16 decoder outputs can be open at once, and the on-screen one is already
closed. The render queue depth no longer adds a second copy of each frame.

Frames that never reach an upload, because the player dropped the picture,
a seek flushed the queue or the stream ended, are closed by `release` from
the pool's `Return`. Frames the codec has not taken when the decoder is
reset are closed by the reset itself.

### 4.5 Drop, seek, drain, teardown

- **Drop** (`DVD_CODEC_CTRL_DROP`): take the frame as in §4.2 step 2, queue
  `release(seq)` at once, return the picture flagged `DVP_FLAG_DROPPED`.
  VideoPlayerVideo counts it and never sends it to the render manager; its
  buffer is released like any other. The synchronous
  `webcodecs_discard_next_frame` round trip disappears.
- **Seek / `Reset()`** (synchronous, VideoPlayer thread): `VideoDecoder.reset()`
  and reconfigure as today; the JS side reads `framesTaken` with
  `Atomics.load` and closes every frame with `seq >= framesTaken`; when the
  call returns the codec sets `framesTaken = framesProduced` and stores it.
  `framesTaken` is written by the VideoPlayer thread only and read by the
  main thread in the output callback (ring-full check) and in the reset;
  the reset is a synchronous proxied call from the writing thread, so it
  sees the latest value, and a stale value in the output callback only
  makes the ring look fuller than it is. Atomics are all it needs. Sequence
  numbers keep counting across resets so a stale `upload`/`release` can
  never match a new frame. Frames already taken belong to Kodi buffers and
  are closed by their `Return`.
- **Drain / end of stream**: unchanged (settle-based, AVSYNC.md §3.6),
  reading `framesProduced - framesTaken` and `inflight`.
- **Destroy**: closes every frame of that decoder, taken or not, so a
  stream switch gives the hardware pool back immediately. A `release` or
  `upload` for a dead handle is a no-op; the pool may outlive the codec
  because the render manager releases its buffers a little later.

### 4.6 GL details

- Kodi owns the textures: one `GL_TEXTURE_2D` per render buffer, created
  with `glGenTextures` (synchronous, once per `Configure`), min/mag filter
  `GL_LINEAR` or `GL_NEAREST` from the scaling method, `CLAMP_TO_EDGE`.
  No `glTexImage2D` storage call: the upload's `texImage2D` specifies level
  0 at the frame's size each time. This is the form browsers optimise for
  video and it absorbs a mid-stream size change for free.
- The upload uses `GL.currentContext.GLctx` and `GL.textures[id]`, the same
  Emscripten internals `webgl_commit.js` already relies on.
- No `getParameter` anywhere in the per-frame path: each one is a
  synchronous round trip to the GPU process and cost a quarter of the TV's
  main thread before the commit fix (RENDERING.md §2.3). The upload leaves
  its texture bound on the active unit; `RenderHook` rebinds, and Kodi's
  GUI code binds before every draw.
- `UNPACK_FLIP_Y_WEBGL` stays false: row 0 lands at t = 0, the same
  orientation as Kodi's own `glTexSubImage2D` uploads, so the texture rects
  are those of `RenderSinglePass`. MediaCodec's y-flip does not apply.
- `UNPACK_COLORSPACE_CONVERSION_WEBGL` stays at `BROWSER_DEFAULT_WEBGL`: the
  browser converts the frame to the context's unpack colour space (sRGB)
  using `frame.colorSpace` for matrix, range, primaries and transfer.
- The canvas is `alpha: false` and video is opaque, so premultiplication is
  moot.

### 4.7 Capability probe and fallback

Whether a browser's WebGL accepts a `VideoFrame` source is probed once:
construct a 2×2 `RGBA` `VideoFrame` from an `ArrayBuffer`, `texImage2D` it
into a scratch texture, check `gl.getError()`, delete and close both. The
probe runs from `CWinSystemWasmGLESContext::InitWindowSystem`, on the render
thread right after the context is made current
(`webcodecs_probe_texture_upload`, synchronous). Run there it is ordered
inside the render thread's own GL stream while that stream is still empty,
so it needs no `getParameter` to save and restore the texture binding. Run
from the codec's `Open` on the VideoPlayer thread instead, it would land at
an arbitrary point between two of the render thread's queued GL calls, the
same interleaving the upload avoids by being enqueued by the render thread
itself. The JS side caches the answer; the codec asks for it at `Open` and
chooses between `CVideoBufferWebCodecs` and the sysmem copy path. The copy
path stays in the tree for that fallback until the TV and the desktop
browsers have validated the new one; removing it afterwards is a separate
decision.

---

## 5. Impact

### 5.1 CPU and memory, 1080p frame

| | Today | Zero copy |
|---|---|---|
| CPU passes over the pixels | 4 (readback, swscale, memdup, upload) | 0 |
| CPU memory traffic per frame | ~23 MB | ~0 |
| VideoPlayer thread per frame | `sws_scale` ≈ 7 ms at 640×360, ≈ 60 ms at 1080p, plus the copy wait | ring read + pool get, microseconds |
| Render thread per frame | ~12 band `memdup`s of 3.1 MB total, then draw | one queued call, then draw |
| Main thread per frame | `copyTo` (readback + 8.3 MB copy), ~12 `texSubImage2D` | one `texImage2D` |
| GPU per frame | 3-texture YUV→RGB pass at output resolution | one copy/convert at source resolution, one textured quad |
| wasm heap for pictures | RGB landing pool (≥ 8.3 MB) + up to 7 × 3.1 MB YUV buffers + 3.1 MB transient ≈ 30–35 MB | none; frames stay in decoder/GPU memory, closed at upload |
| GL textures per buffer | 3 (Y, U, V) | 1 (RGBA) |

The 4K numbers are four times the 1080p ones on the left and unchanged on
the right, which is the point: the design's per-frame CPU cost does not
depend on the resolution.

### 5.2 What each thread does after the change

- **VideoPlayer thread**: parse the packet for the keyframe flag, one
  asynchronous push, and per output frame a ring read and pool bookkeeping.
- **Render pthread**: one asynchronous upload and one quad per video frame,
  plus the GUI. No synchronous GL call in the per-frame path at all except
  the commit, which is the same for GUI-only frames.
- **Main thread**: `decode()`, the output callback (a 64-byte ring write),
  `texImage2D`, forwarding Kodi's GL commands, the commit blit. The two
  largest items in today's TV profiles of this thread, `copyTo` and the
  banded `texSubImage2D`, are gone.
- **GPU**: decoder output → one copy/convert → quad → blit → composite.

### 5.3 Latency and A/V sync

Pipeline depth is unchanged: the render manager still holds the same queue,
`PrepareNextRender` still picks by `clock + displayLatency` at the rAF tick,
and the upload is enqueued in the same display frame as the draw and
executes before the commit, so no frame of latency is added. Jitter should
improve: the main thread has less to do between the render thread's draw
and the commit, and the VideoPlayer thread no longer spends tens of
milliseconds per frame in libswscale, which today delays `GetPicture` and
with it the pictures available to the render manager. Nothing about the
audio path, the clocks or `CVideoSyncWasm` changes.

### 5.4 Features

| Feature | Today (`CLinuxRendererGLES`, sysmem YUV) | Zero copy (`CRendererWebCodecs`) |
|---|---|---|
| Aspect, zoom, view modes, vertical shift, pixel ratio | yes | yes, base-class geometry |
| Rotation from the stream | yes | yes, `m_rotatedDestCoords` |
| Brightness, contrast | yes, YUV shader | yes, `SM_TEXTURE_RGBA` uniforms |
| Scaling | bilinear, nearest, HQ scalers when enabled | bilinear, nearest |
| Dithering | yes | no (GUI shader) |
| Colour matrix and range | Kodi's shader from stream hints, inferred when unspecified | the browser, from the frame's own `colorSpace` |
| 10-bit sources (HEVC Main 10, VP9 p2, AV1 10-bit) | stream fails (AVSYNC.md §6, item 3) | any format the browser decodes |
| HDR (PQ, HLG) | Kodi tone mapping when the stream is flagged | the browser's conversion to the canvas colour space |
| Deinterlacing | Kodi's methods on YUV planes | none; bob via `SM_TEXTURE_RGBA_BOB` is possible if fields are ever reported |
| OSD, subtitles, GUI over video | yes | yes, video is drawn into Kodi's framebuffer |
| Screenshots, captures | yes, framebuffer read | yes, same path |
| Pause, frame step, repeated frames | yes | yes, the texture keeps the pixels |
| Stereo 3D source layouts | yes | yes, base-class source rects |
| FFmpeg fallback for other codecs | `CLinuxRendererGLES` | unchanged, still the `"default"` renderer |

The losses are the ones every hardware-surface renderer in Kodi accepts
(`CRendererMediaCodec`, the VAAPI and DRM-PRIME EGL renderers): scaling and
colour conversion are done by the platform, at the quality the platform
provides. On a TV whose decoder is a black box that is the right trade, and
it is the trade the current packed-RGB path already makes badly, by
converting RGB to 4:2:0 and back.

### 5.5 Formats, colour and HDR

The bridge stops caring about `VideoFrame.format`: `I420`, `NV12`, `RGBX`,
`I420P10`, `P010` and anything else the decoder emits all go through the
same `texImage2D`. That removes AVSYNC.md §6 items 1 (the swscale pass) and
3 (10-bit rejected) at once. Colour metadata becomes authoritative instead of
inferred: `frame.colorSpace` carries matrix, primaries, transfer and range as
the decoder saw them, and the picture's fields are filled from it for the
info dialogs. `VideoColorSpace` uses the WebCodecs names; each field may be
`null`, in which case the stream hint is used as today:

| `colorSpace.matrix` | `color_space` | `colorSpace.primaries` | `color_primaries` | `colorSpace.transfer` | `color_transfer` |
|---|---|---|---|---|---|
| `rgb` | `AVCOL_SPC_RGB` | `bt709` | `AVCOL_PRI_BT709` | `bt709` | `AVCOL_TRC_BT709` |
| `bt709` | `AVCOL_SPC_BT709` | `bt470bg` | `AVCOL_PRI_BT470BG` | `smpte170m` | `AVCOL_TRC_SMPTE170M` |
| `bt470bg` | `AVCOL_SPC_BT470BG` | `smpte170m` | `AVCOL_PRI_SMPTE170M` | `iec61966-2-1` | `AVCOL_TRC_IEC61966_2_1` |
| `smpte170m` | `AVCOL_SPC_SMPTE170M` | `bt2020` | `AVCOL_PRI_BT2020` | `linear` | `AVCOL_TRC_LINEAR` |
| `bt2020-ncl` | `AVCOL_SPC_BT2020_NCL` | `smpte432` | `AVCOL_PRI_SMPTE432` | `pq` | `AVCOL_TRC_SMPTE2084` |
| | | | | `hlg` | `AVCOL_TRC_ARIB_STD_B67` |

`fullRange` maps to `color_range`, and `colorBits` comes from the format
name (`P10` → 10, `P12` → 12, else 8). For the TV's `RGBX` frames the
matrix is `rgb`, which no Kodi consumer on this path interprets: the
renderer does not convert, `GetColorimetry` falls back to its size rule for
the log line, and `IsSameParams` compares primaries and transfer only. HDR
frames are tone-mapped by the browser to the sRGB canvas; how that looks on
the TV is one of the things to verify (§7.3).

### 5.6 Browser support

Chromium has accepted a `VideoFrame` as a `texImage2D` source since WebCodecs
shipped; the Samsung S95F's Chrome 120 is well past that. Firefox ships
WebCodecs since 130 and Safari since 16.4; whether their WebGL takes a
`VideoFrame` source is what the probe in §4.7 is for, and the sysmem path
covers a negative answer.

### 5.7 Risks

- **The TV's `texImage2D(VideoFrame)` might not be a GPU path.** If the
  browser readbacks the frame internally, the CPU work moves from wasm to the
  browser rather than disappearing. It would still be one pass instead of
  four and off Kodi's threads, and the profiler shows it directly as
  `texImage2D` self time on main (§7.2).
- **Decoder pool size.** Unknown on Tizen. Closing at upload keeps open
  frames at the in-flight cap plus the taken-but-not-uploaded ones, 16 at
  most (§4.4); if the decoder still stalls with all outputs open,
  `WEBCODECS_MAX_INFLIGHT` is the knob.
- **HDR appearance** differs from Kodi's tone mapping and is not
  user-tunable. Acceptable for a first version; the video-plane design has
  the same property.
- **Reliance on Emscripten internals** (`GL.currentContext`, `GL.textures`).
  Already the case for `webgl_commit.js`; pinned by the Emscripten version
  the build uses.
- **Stale texture on a flush that overtakes an upload.** Benign: the buffer
  is being discarded, and the next picture overwrites the texture.
- **Texture re-specification each frame.** Browsers take a fast path for
  video sources; if profiling shows otherwise, `texSubImage2D` into a
  pre-sized texture is a one-line change in the upload.

---

## 6. Alternatives

- **Video plane under the GUI** (RENDERING.md §9.3). Saves the GPU copy and
  the quad; costs an alpha-enabled GUI canvas with a transparent hole,
  compositor-aligned presentation of a second canvas, the loss of the
  framebuffer capture path and untested interaction with Kodi's GUI
  blending. Nothing in today's profiles points at GPU bandwidth, and a 1080p
  RGBA copy is a few megabytes of GPU traffic per frame. The texture import
  gets the entire CPU win without any of that. The plane remains the next
  step if a target turns out GPU-bound, and the natural shape for a Tizen
  EMSS backend (RENDERING.md §9.4).
- **WebGPU `importExternalTexture`.** The one browser API that samples the
  decoder's YUV planes directly with no copy. Kodi has no WebGPU render
  system, and Chrome 120 on Tizen does not expose WebGPU.
- **`WEBGL_webcodecs_video_frame`.** A proposed WebGL extension for
  importing `VideoFrame` planes; never shipped.
- **Upload packed RGBA through sysmem without libswscale** (AVSYNC.md §6
  item 1). Removes one pass and keeps the readback plus an 8.3 MB upload in
  33 bands. Strictly dominated by this design.
- **`drawImage(frame)` on a 2D canvas, then `texImage2D(canvas)`.** Two GPU
  copies for the price of one; no gain.
- **A dedicated decode Worker.** Moves work between threads; removes none.

---

## 7. Implementation plan and validation

### 7.1 Steps

Each step is a separate commit with its documentation update, in the order
the code depends on it:

1. **ABI and bridge**: new `WebCodecsSharedState` and `WebCodecsFrameInfo`,
   `webcodecs_upload_frame`, `webcodecs_release_frame`, the frame map and
   ring in `webcodecs_bridge.js`, the capability probe and its call from
   `InitWindowSystem`. The copy stays for the fallback, keyed by sequence
   number.
2. **Buffer and pool**: `CVideoBufferWebCodecs` in
   `DVDVideoCodecWebCodecs.h`, pool `Return` → release.
3. **Renderer**: `HwDecRender/RendererWebCodecs.{h,cpp}`, the
   `RENDER_CUSTOM` condition on the `Configure` format gate in
   `CLinuxRendererGLES`, CMake for `wasm`, registration in
   `WinSystemWasmGLESContext::InitWindowSystem`.
4. **Codec**: `GetPicture` on the ring, drop path, reset bookkeeping,
   colour metadata from the frame, process-info format string; the sysmem
   path selected only when the probe fails.
5. **Docs**: AVSYNC.md §3.2, §3.3, §3.5, §6 and §7; RENDERING.md §9; this
   document's status line; README.WASM.md.
6. **Cleanup**, once validated: remove the sysmem copy path, libswscale
   from the codec, and the RGB conversion table in AVSYNC.md.

Roughly the size of the HEVC/AV1 change: a few hundred lines of C++ and
JavaScript, most of them replacing code that exists today.

### 7.2 Validation on the TV

The same tooling as the profiling session that motivated this document:
build with `ENABLE_WASM_PROFILING`, install with
`cmake --build build-wasm --target install_tizen_wgt`, launch under
`tools/wasm/tizen/inspect.sh`, attach the CDP profiler to the page and every
worker at launch, play Big Buck Bunny (H.264, 720p and 1080p), then an HEVC
Main 10 and an AV1 sample.

Pass criteria, compared with the profiles taken before the change:

- VideoPlayer thread: `sws_scale` and `copyTo` waits gone; active time per
  frame under 0.5 ms.
- Main thread: `copyTo` and `texSubImage2D` gone; `texImage2D` self time is
  the number that says whether the TV took the GPU path (a few hundred
  microseconds per frame) or a readback path (milliseconds).
- Render thread: no `memdup`/`emscripten_sync_run_in_main_runtime_thread`
  in the video path.
- `debug.setextraloglevel` with LOGAVTIMING: the 3:2 cadence at 24p on
  60 Hz stays clean, no `large audio sync error`, no skipped frames.
- Visual: colour range and matrix against the swscale build's screenshot;
  a 10-bit HEVC sample plays; pause, seek, frame step, OSD, subtitles,
  screenshots, aspect and zoom changes, resize on a desktop browser, and a
  VP9 profile 2 sample still falls back to FFmpeg with the GLES renderer.
- Desktop Chrome with a software decoder (CPU-backed frames) and, for the
  probe, Firefox and Safari.

### 7.3 Open questions to settle on hardware

1. Is `texImage2D(VideoFrame)` from the TV's `RGBX` frames a GPU copy?
   (`texImage2D` self time on main.)
2. How many output frames does the Tizen hardware decoder allow open before
   `decode()` stalls? (`inflight` growth with `WEBCODECS_MAX_INFLIGHT` open
   frames.)
3. Does the TV populate `VideoFrame.colorSpace`, and what does an HDR
   sample look like after the browser's conversion?
4. Does the frame's `visibleRect` match the coded size on this decoder
   (cropping is handled by `texImage2D`, but the picture dimensions Kodi
   reports come from it)?
