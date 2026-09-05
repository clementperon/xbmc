# Kodi WebAssembly — audio, video decoding and A/V sync

This document describes how decoded audio and video reach the browser when
Kodi is built for `wasm32-unknown-emscripten`, and how the two stay in
sync. It is the companion of [RENDERING.md](RENDERING.md), which covers the
GUI presentation path and the threading constraints the browser imposes;
those constraints are assumed here and not repeated.

**Scope.** Section 2 is the audio sink (Web Audio `AudioWorklet`), section 3
the video decoder (WebCodecs `VideoDecoder`), section 4 how Kodi's existing
clock and render-manager logic ties them together, section 5 the numbers to
look at when sync is off, and section 6 what is known to be missing.

---

## 1. Where the wasm build differs from a native one

Kodi's VideoPlayer is unchanged: the audio engine (ActiveAE) owns the master
clock, VideoPlayerVideo decodes into a small picture queue, and
`CRenderManager` picks which picture to show at each display refresh. The
wasm build plugs into it at three points:

| Kodi interface | Native example | wasm implementation |
|---|---|---|
| `IAESink` (audio output) | ALSA, PulseAudio, AudioTrack | `CAESinkWasmAudioWorklet` → `CWasmAudioWorkletManager` → Web Audio `AudioWorkletNode` |
| `CDVDVideoCodec` (video decode) | FFmpeg, MediaCodec | `CDVDVideoCodecWebCodecs` → `webcodecs_bridge.js` → WebCodecs `VideoDecoder` |
| `CBaseRenderer` (video display) | same | `CLinuxRendererGLES`, unchanged, on the proxied WebGL context of RENDERING.md §2 |

Both browser APIs are only reachable from the browser main thread, so every
call into them is a proxied call, and every piece of state Kodi needs to
*poll* is mirrored into wasm memory with `Atomics` so polling never crosses
threads. Where Kodi needs to *wait*, it waits on a futex (`Atomics.wait`)
that the JavaScript side notifies, never by spinning through the proxy.

---

## 2. Audio

### 2.1 Thread and buffer layout

```
┌───────────────────────────────────────────────────────────────────┐
│ ActiveAE sink thread (Kodi pthread)                               │
│   CActiveAESink::OutputSamples → IAESink::AddPackets (20 ms)      │
│     → CWasmAudioWorkletManager::WritePlanar                        │
│        copies planar float into the ring, blocks while full        │
│   IAESink::GetDelay → ring fill + baseLatency + outputLatency      │
└───────────────────────┬───────────────────────────────────────────┘
                        │ SPSC ring, planar float, 100 ms capacity
                        │ m_writeFrame (release) / m_readFrame (acquire)
┌───────────────────────▼───────────────────────────────────────────┐
│ AudioWorklet thread (Wasm Worker, real-time)                       │
│   ProcessAudio(): 128 frames per call (the render quantum)         │
│     memcpy one quantum per channel out of the ring, or silence     │
│     until 40 ms have been prebuffered                              │
└───────────────────────────────────────────────────────────────────┘
                        │
              AudioContext.destination → speakers / HDMI
```

The `AudioContext`, worklet thread and node are created from the sink
thread through `emscripten_sync_run_in_main_runtime_thread`; the
`AudioWorkletProcessor` itself is Emscripten's `-sAUDIO_WORKLET` glue, so
`ProcessAudio` is C++ compiled to wasm running on the audio rendering
thread. Nothing in that callback allocates, locks, logs or calls into JS.

### 2.2 Format negotiation

* The sink advertises `AE_FMT_FLOATP` (planar float) at 44.1 or 48 kHz;
  ActiveAE resamples and mixes into that, so the worklet does a plain
  `memcpy` per channel. Web Audio's own buffers are planar too.
* Channel count is clamped to what `destination.maxChannelCount` reports
  (`ResolveSinkLayout`): 2.0, 5.1 or 7.1. Web Audio only defines speaker
  mixing for 1/2/4/6 channels, so anything else would be treated as
  "discrete" and silently dropped. `destination.channelCount` is raised to
  match, otherwise the browser folds a 5.1 node down to stereo.
* Passthrough (`AE_FMT_RAW`) is refused; there is no bitstream path in
  Web Audio.
* The sink period (`format.m_frames`) is 20 ms rounded up to a whole
  number of render quanta, so each `AddPackets` call carries a few quanta.

### 2.3 Timing and the master clock

ActiveAE derives the audio clock from what the sink reports in
`GetDelay()`: the seconds of audio that will play before a sample written
now is heard. The wasm sink reports

```
delay = (m_writeFrame - m_readFrame) / sampleRate      ring fill
      + AudioContext.baseLatency + AudioContext.outputLatency
```

The `AudioContext` is created with `latencyHint: 'playback'`, so the browser
uses its largest output buffer; that latency lands in `outputLatency`, hence
in `delay`, and does not move lip-sync. `m_readFrame` advances once per
render quantum, so the estimate has a ±2.7 ms (128 / 48000) sawtooth on top
of it. `AEDelayStatus::tick` is the host counter at the time of the read and
ActiveAE extrapolates from it, which is the same treatment every other sink
gets. `IAESink::GetLatency()` is left at 0 on purpose: the pipeline latency
is already inside `delay`, adding it again would shift lip-sync by the same
amount.

`GetCacheTotal()` returns the ring capacity (100 ms). ActiveAE keeps its own
0.4 s of stream cache plus 0.2 s of post-stage water level ahead of that,
so total audio buffering is ~0.7 s, like on other platforms.

Backpressure is the ring. `WritePlanar` copies what fits and sleeps one
quantum (`emscripten_thread_sleep`) while the ring is full, giving up after
500 ms so a suspended `AudioContext` (autoplay policy, tab in the
background) cannot hang the sink thread forever; ActiveAE then sees a short
write, retries, and eventually reopens the sink.

### 2.4 Underruns and prebuffer

The worklet emits silence and counts the missing frames when the ring is
empty. The sink thread drains that counter after every write and logs once
at least 10 ms of silence accumulated
(`CAESinkWasmAudioWorklet: worklet underrun, N frames (x ms)`). After
`ResetBuffer()` (start, seek) the worklet plays silence until 40 ms are
buffered so the first quanta do not underrun while ActiveAE is still
filling.

### 2.5 Autoplay

Browsers refuse to start an `AudioContext` before a user gesture.
`InstallResumeHooks` registers one-shot `pointerdown` / `keydown` /
`touchstart` / `visibilitychange` listeners on the main thread that call
`ctx.resume()`. Until then the worklet never runs, `m_readFrame` never
advances, and the sink stalls as described in 2.3. On a Tizen TV the app
is launched by the user so this resolves at the first remote key.

---

## 3. Video

### 3.1 Codec selection

`CDVDVideoCodecWebCodecs` is registered as a hardware codec (`webcodecs_dec`)
from `CWinSystemWasmGLESContext::InitWindowSystem`, so `CDVDFactoryCodec`
tries it before FFmpeg. `Open()` fails cleanly — and the player falls back
to the software FFmpeg decoder — when the codec is not H.264, VP8 or VP9,
when the page has no `VideoDecoder`, or when `configure()` throws. A
configuration that `isConfigSupported()` later rejects is reported through
the shared `failed` flag and surfaces as `VC_ERROR` on the next
`GetPicture()`.

Codec strings: `avc1.PPCCLL` from the AVCDecoderConfigurationRecord (with
the record passed as `description`, packets in AVCC length-prefixed form),
or `avc1.42E01E` + `avc: {format: 'annexb'}` when the extradata is not an
AVCC record (MPEG-TS, raw H.264); `vp09.PP.LL.DD` from the stream hints;
`vp8`. The decoder is configured with `optimizeForLatency: true` and
`hardwareAcceleration: 'prefer-hardware'`.

### 3.2 Thread and queue layout

```
┌────────────────────────────────────────────────────────────────────┐
│ VideoPlayerVideo thread (Kodi pthread)                             │
│   AddData(packet):                                                 │
│     PacketIsKeyFrame() parses the bitstream (IDR / VP8 / VP9 hdr)  │
│     skip deltas until the first key                                 │
│     shared.busy? → return false (VideoPlayer re-queues the packet)  │
│     webcodecs_push_packet()               ── sync proxy ──►  main   │
│   GetPicture():                                                     │
│     shared.queuedFrames == 0 → VC_BUFFER (futex wait ≤20 ms if busy)│
│     DROP flag set? → webcodecs_discard_next_frame() ── sync ──► main │
│     pool.Get() → CVideoBufferSysMem sized from shared.nextPayloadSize│
│     webcodecs_copy_next_frame(dst)        ── sync proxy ──►  main   │
│     futex wait on shared.signal until shared.copyResult != 0        │
│     packed RGB? → sws_scale → YUV420P (see 3.5)                     │
│     fill VideoPicture{pts, duration, colour metadata} → VC_PICTURE  │
└─────────────────────────────┬──────────────────────────────────────┘
                              │ WebCodecsSharedState (Atomics) + futex
┌─────────────────────────────▼──────────────────────────────────────┐
│ Browser main thread                                                │
│   VideoDecoder.decode(EncodedVideoChunk{type, timestamp µs, dur}) │
│   output(frame) → state.frames.push({frame, layout, pts, ...})     │
│   copy_next_frame: frames.shift(); frame.copyTo(HEAPU8 view) …     │
│     .finally → frame.close(); publishState()                       │
│   publishState(): store every field, signal++, Atomics.notify      │
└────────────────────────────────────────────────────────────────────┘
```

`WebCodecsSharedState` (32 bytes, layout asserted in
`DVDVideoCodecWebCodecsBridge.h`) carries `signal`, `queuedFrames`,
`inflight`, `busy`, `failed`, `nextPayloadSize`, `nextPixelFormat` and
`copyResult`. The JS side rewrites all of them and bumps `signal` on every
state change, so the C++ side reads them with acquire loads and only ever
blocks in `emscripten_futex_wait(&signal, seen, timeout)`.

### 3.3 Backpressure

Two limits, both enforced on the main thread:

| Limit | Value | Effect |
|---|---|---|
| `MAX_INFLIGHT` | 12 | `busy` is set when `decodeQueueSize + copying + queuedFrames` reaches it; `push_packet` applies the same rule. `AddData` then returns `false` and VideoPlayerVideo re-queues the packet as a priority message; `GetPicture` waits up to 20 ms on the futex for the next output instead of returning `VC_BUFFER` immediately. Hardware decoders have a fixed output pool and stall when too many `VideoFrame`s stay open. |
| `FRAME_QUEUE_HIGH_WATER` | 24 | Safety valve in the output callback: frames beyond it are closed and counted as dropped. Not reached while `busy` works. |

Decoded frames stay open on the main thread until the codec asks for one,
so the memory cost of a queued frame is the decoder's, not the wasm heap's.

### 3.4 Timestamps

Kodi's `DVD_TIME_BASE` is microseconds as a `double`; WebCodecs timestamps
are microseconds as a 64-bit integer. `AddData` converts `packet.pts` to
seconds and the bridge back to integer microseconds, so nothing is lost
below the microsecond. Negative timestamps are legal for
`EncodedVideoChunk` and are kept, which matters after a seek into a stream
whose first packets have negative pts. `VideoDecoder` outputs frames in
presentation order, so `GetPicture()` returns them in display order and
sets `dts = DVD_NOPTS_VALUE`; VideoPlayerVideo takes `pts` as-is and
replaces `iDuration` with its own frame-rate estimate, as it does for every
codec.

### 3.5 Pixel formats and the copy

`describeFrame()` maps the `VideoFrame.format` to a tightly packed layout
and `copyTo()` writes straight into the `CVideoBufferSysMem` memory (the
wasm heap is a `SharedArrayBuffer` under pthreads, and Chromium's `copyTo`
accepts a view on it; a browser that rejects shared views falls back to a
scratch buffer plus one `memcpy`).

| `VideoFrame.format` | Handed to the renderer as | Extra work |
|---|---|---|
| `I420` | `AV_PIX_FMT_YUV420P` | none |
| `NV12` | `AV_PIX_FMT_NV12` | none |
| `RGBA` / `RGBX` / `BGRA` / `BGRX` | `AV_PIX_FMT_YUV420P` | `sws_scale` RGB → YUV420P on the VideoPlayer thread, `SWS_POINT`, colour matrix taken from the stream hints so the renderer's YUV→RGB inverts it exactly |

Packed RGB is what the Samsung Tizen hardware decoder currently returns.
That path costs an extra CPU pass over every frame and quantises full-range
RGB to 4:2:0 before the renderer turns it back into RGB; see §6.

### 3.6 Seek, flush and end of stream

* **Seek / `Reset()`** → `webcodecs_reset_decoder`: `VideoDecoder.reset()`
  drops everything, the JS state closes its queued frames, the decoder is
  `configure()`d again (reset returns it to `unconfigured`), and the codec
  goes back to skipping deltas until the next key packet. A copy still in
  flight is waited for first (`ReleaseCopyBuffer`) so the browser never
  writes into a buffer Kodi has recycled.
* **Drain** (`DVD_CODEC_CTRL_DRAIN`, sent at end of stream, on stream change
  and when VideoPlayerVideo detects a still frame) does not call
  `VideoDecoder.flush()`. A flushed decoder demands a key chunk, and
  VideoPlayerVideo drains whenever no packet arrives for ten frame times, so
  every stall on a long-GOP stream would freeze the picture until the next
  IDR. Instead `GetPicture()` hands out whatever is queued, waits for
  `inflight` to reach 0 and for the decoder to stay silent for 100 ms, then
  returns `VC_EOF`; the decoder keeps its reference state and delta packets
  decode normally when data resumes. The wait is bounded by 1 s, after which
  the remaining in-flight count is logged. The cost is at a real end of
  stream: frames a decoder holds back until it sees more input are never
  emitted.
* **Drop** (`DVD_CODEC_CTRL_DROP`, `DVD_CODEC_CTRL_DROP_ANY`) → the next
  queued frame is closed without `copyTo()` through
  `webcodecs_discard_next_frame`, and `GetPicture()` returns a picture flagged
  `DVP_FLAG_DROPPED`. VideoPlayerVideo still configures the renderer from such
  a picture, so it carries an uncopied buffer of the displayed format; it is
  never rendered. Skipping the copy saves the most exactly when the CPU is
  already behind.
* **Destroy** → `webcodecs_destroy_decoder` closes frames and decoder and
  zeroes the state pointer so a late `copyTo` completion cannot publish
  into memory the codec has freed.

---

## 4. How they meet: clock, render manager, display

Nothing in this section is wasm-specific code; it is what Kodi does with
the numbers the two previous sections provide.

1. **Master clock.** ActiveAE compares each stream's pts with
   `clock + delay` (delay from §2.3) and nudges `CDVDClock`; large errors
   are logged as `ActiveAE - large audio sync error`, which the wasm build
   extends with the sink delay and buffer level for diagnosis.
2. **Video start.** VideoPlayerVideo reports the first picture's pts plus
   `CRenderManager::GetDelay()` to the player, which synchronises the
   audio and video start points as on any platform.
3. **Picture selection.** `CRenderManager::PrepareNextRender` runs once per
   GUI frame on the Kodi render pthread, paced by the `requestAnimationFrame`
   futex of RENDERING.md §2.3, and shows the queued picture whose pts is
   closest to `clock + displayLatency`. `GetFPS()` is the refresh rate the
   rAF pump measured at window creation.
4. **Display latency.** `CWinSystemWasmGLESContext` does not override
   `GetDisplayLatency()`, so Kodi uses the generic fallback
   `(videoscreen.noofbuffers + 1) / fps`, 66.7 ms at 60 Hz with the default
   of 3. The real figure is the proxied GL queue plus `commit_frame` plus
   the compositor, roughly one to two refresh intervals; the user-facing
   audio offset setting absorbs the difference for now.
5. **Upload and draw.** `CLinuxRendererGLES` uploads the planes with
   `glTexSubImage2D` from the render pthread. Under Emscripten's proxying,
   uploads of 256 KB or more are synchronous round trips to the main
   thread (`system/lib/gl/webgl1.c`), and a 1080p luma plane is 2 MB, so
   the video upload is the one place per frame where the render thread
   blocks on main.
6. **Display-as-clock.** The wasm window system provides no `CVideoSync`,
   so `videoplayer.usedisplayasclock` falls back to the system clock. The
   audio-master path above does not need it.

---

## 5. Diagnostics

| Symptom | Where to look |
|---|---|
| Audio stutters, `worklet underrun` warnings | The sink thread is late filling the ring: main thread saturated (video copies, GL) or the pthread starved. Check `[KODI_DBG]` present stats and whether `AudioContext.state` is `running`. |
| `large audio sync error` with a stable `sinkDelay` | Clock drift between `AudioContext` and the host counter; expected to be small. If `sinkDelay` jumps, `outputLatency` changed (device switch, HDMI re-negotiation): the pipeline latency is only sampled at sink initialisation. |
| `dropped N queued WebCodecs frames` | The output queue hit `FRAME_QUEUE_HIGH_WATER`; VideoPlayer is not pulling pictures — usually the render side is stalled. |
| `frame copy did not complete within 500 ms` | `copyTo()` never resolved: main thread blocked or the frame was closed by a `reset()` racing the copy. |
| Lip-sync off by a constant | Compare `GetDisplayLatency()` (§4.4) with the measured present latency; adjust with the audio offset setting until a `CVideoSync`/latency override exists. |

Enable `LOGAVTIMING` in the component logging settings to see
`frameOnScreen / renderPts / nextFramePts` from `PrepareNextRender`.

---

## 6. Known limitations and planned improvements

Ordered by expected impact on a two-core Tizen TV.

1. **Packed-RGB frames go through a software RGB→YUV pass.** The renderer
   only takes planar YUV, so a decoder that outputs RGBA costs a 1080p
   `sws_scale` per frame and a 4:2:0 quantisation of the picture. Teaching
   `CLinuxRendererGLES` (or a small dedicated renderer) to upload packed
   RGBA and draw it without a colour conversion removes the pass entirely;
   it is the cheapest sysmem path possible short of the video-plane design
   in RENDERING.md §9.3.
2. **`push_packet` is a synchronous proxy call per packet.** Only the
   return status needs the round trip, and `busy` / `failed` are already in
   shared memory. An asynchronous variant has to copy the packet before
   dispatching (the DemuxPacket does not outlive the call), which is a
   `memdup` per packet — cheaper than blocking the video thread behind a
   busy main thread.
3. **Display latency is the generic fallback.** A `GetDisplayLatency()`
   override measured from the rAF pump, or a `CVideoSync` implementation
   fed by the same `g_vsyncTick` counter, would give `CRenderManager`
   both the real latency and the clock-sync mode.
4. **The last frames of a stream can be lost.** Drain does not flush
   (§3.6), so a decoder that withholds output until it sees more input
   never emits its final frames at end of stream. Flushing only when the
   player knows the stream has really ended would need a new codec-control
   flag.
5. **Codec coverage** is H.264, VP8 and VP9. Tizen TVs decode HEVC and AV1
   in hardware; both map onto WebCodecs (`hvc1.`/`hev1.` from `hvcC`,
   `av01.` from `av1C`) with the same bridge.

---

## 7. Code map

| Concern | File |
|---|---|
| Audio sink (`IAESink`) | `xbmc/cores/AudioEngine/Sinks/AESinkWasmAudioWorklet.cpp` |
| AudioContext / worklet / ring buffer | `xbmc/platform/wasm/WasmAudioWorkletManager.cpp` |
| Video codec (`CDVDVideoCodec`) | `xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecWebCodecs.cpp` |
| Shared ABI: enums, `WebCodecsSharedState`, `WebCodecsFrameInfo` | `xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecWebCodecsBridge.h` |
| Main-thread `VideoDecoder` driver | `xbmc/cores/VideoPlayer/DVDCodecs/Video/webcodecs_bridge.js` |
| Keyframe flag from the demuxer (fallback only) | `xbmc/cores/VideoPlayer/Interface/DemuxPacket.h`, `DVDDemuxers/DVDDemuxFFmpeg.cpp` |
| Sync-error diagnostics | `xbmc/cores/AudioEngine/Engines/ActiveAE/ActiveAE.cpp` |
| Link flags (`-sAUDIO_WORKLET`, `-sWASM_WORKERS`, `--js-library`) | `cmake/scripts/wasm/ArchSetup.cmake` |
