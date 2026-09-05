/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

//
// Emscripten JS library implementing the WebCodecs bridge declared in
// DVDVideoCodecWebCodecsBridge.h. Linked via --js-library (see
// cmake/scripts/wasm/ArchSetup.cmake).
//
// Every exported function carries a __proxy attribute so calls from any pthread
// are marshalled to the main browser thread, where the VideoDecoder lives. The
// per-packet calls are 'async' and report through WebCodecsSharedState, which
// the C++ side polls with Atomics instead of a round trip (see publishState).
//
// Function signatures must match the C prototypes; mismatched __sig will
// silently corrupt arguments on threaded builds.
//

mergeInto(LibraryManager.library, {

  // ---------------------------------------------------------------------------
  // Internal module: shared state + helpers, attached only if any bridge
  // function is used (Emscripten dead-strips unreferenced $-symbols).
  // ---------------------------------------------------------------------------
  $WebCodecsBridge: {
    MICROSECONDS_PER_SECOND: 1000000.0,
    FRAME_QUEUE_HIGH_WATER: 24,

    // Enum values below are pulled from the C++-owned Embind registrations
    // (Module.WebCodecsPixelFormat / Module.WebCodecsCopyResult) on first use
    // by syncEnumsFromEmbind(). The C++ header (DVDVideoCodecWebCodecsBridge.h)
    // is the single source of truth; do not hardcode these here.
    PIXFMT_UNKNOWN: 0,
    PIXFMT_YUV420P: 0,
    PIXFMT_NV12: 0,
    PIXFMT_RGBA: 0,
    PIXFMT_RGBX: 0,
    PIXFMT_BGRA: 0,
    PIXFMT_BGRX: 0,
    COPY_OK: 0,
    COPY_FAILED: 0,
    COPY_DST_TOO_SMALL: 0,
    COPY_NO_FRAME: 0,
    _enumsReady: false,

    // Byte offsets inside struct WebCodecsFrameInfo (kept in sync with the
    // static_asserts in DVDVideoCodecWebCodecsBridge.h).
    FI_PIXFMT: 0,
    FI_WIDTH: 4,
    FI_HEIGHT: 8,
    FI_Y_STRIDE: 12,
    FI_U_STRIDE: 16,
    FI_V_STRIDE: 20,
    FI_U_OFFSET: 24,
    FI_V_OFFSET: 28,
    FI_KEYFRAME: 32,
    FI_PAYLOAD_SIZE: 36,
    FI_PTS: 40,
    FI_DURATION: 48,

    // Int32 indices inside struct WebCodecsSharedState (offsets / 4).
    SS_SIGNAL: 0,
    SS_QUEUED_FRAMES: 1,
    SS_INFLIGHT: 2,
    SS_FAILED: 3,
    SS_NEXT_PAYLOAD_SIZE: 4,
    SS_NEXT_PIXFMT: 5,
    SS_PUSHES_PROCESSED: 6,
    SS_COPY_DONE: 7,
    SS_COPY_RESULT: 8,

    // Registry is lazily created on first decoder creation; this library file
    // is merged into both the main thread and every pthread module, but only
    // the main thread ever touches the maps (__proxy:'sync' on every entry).
    registry: null,
    nextId: 1,

    ensureRegistry: function() {
      if (!this.registry)
        this.registry = new Map();
      return this.registry;
    },

    // Pull the canonical enum numeric values out of the Embind bindings
    // registered by EMSCRIPTEN_BINDINGS(kodi_webcodecs_bridge) in
    // DVDVideoCodecWebCodecs.cpp. Embind exposes each C++ enum value as an
    // object with a .value property holding the underlying integer.
    syncEnumsFromEmbind: function() {
      if (this._enumsReady)
        return;
      const pf = Module['WebCodecsPixelFormat'];
      const cr = Module['WebCodecsCopyResult'];
      if (!pf || !cr)
        throw new Error('WebCodecs bridge enums missing from Module (Embind not linked?)');
      this.PIXFMT_UNKNOWN = pf.UNKNOWN.value;
      this.PIXFMT_YUV420P = pf.YUV420P.value;
      this.PIXFMT_NV12 = pf.NV12.value;
      this.PIXFMT_RGBA = pf.RGBA.value;
      this.PIXFMT_RGBX = pf.RGBX.value;
      this.PIXFMT_BGRA = pf.BGRA.value;
      this.PIXFMT_BGRX = pf.BGRX.value;
      this.COPY_OK = cr.OK.value;
      this.COPY_FAILED = cr.FAILED.value;
      this.COPY_DST_TOO_SMALL = cr.DST_TOO_SMALL.value;
      this.COPY_NO_FRAME = cr.NO_FRAME.value;
      this._enumsReady = true;
    },

    getState: function(handle) {
      return this.registry ? this.registry.get(handle) : null;
    },

    inflight: function(state) {
      const decoder = state.decoder;
      const queueSize = decoder && decoder.state === 'configured' ? decoder.decodeQueueSize : 0;
      return queueSize + (state.copying ? 1 : 0);
    },

    publishState: function(state) {
      if (!state.sharedPtr)
        return;
      const base = state.sharedPtr >> 2;
      const next = state.frames.length > 0 ? state.frames[0] : null;
      Atomics.store(HEAP32, base + this.SS_QUEUED_FRAMES, state.frames.length);
      Atomics.store(HEAP32, base + this.SS_INFLIGHT, this.inflight(state));
      Atomics.store(HEAP32, base + this.SS_FAILED, state.failed ? 1 : 0);
      Atomics.store(HEAP32, base + this.SS_NEXT_PAYLOAD_SIZE, next ? next.payloadSize : 0);
      Atomics.store(HEAP32, base + this.SS_NEXT_PIXFMT, next ? next.pixelFormat : this.PIXFMT_UNKNOWN);
      Atomics.store(HEAP32, base + this.SS_PUSHES_PROCESSED, state.pushesProcessed);
      Atomics.store(HEAP32, base + this.SS_COPY_DONE, state.copyDone);
      Atomics.store(HEAP32, base + this.SS_COPY_RESULT, state.copyResult);
      Atomics.add(HEAP32, base + this.SS_SIGNAL, 1);
      Atomics.notify(HEAP32, base + this.SS_SIGNAL);
    },

    // Plane layout for the formats we accept, with tightly packed strides.
    // Some hardware decoders (Samsung Tizen) only hand out packed RGB frames.
    describeFrame: function(frame, width, height) {
      const format = frame.format || 'I420';

      const rgbFormats = {
        RGBA: this.PIXFMT_RGBA, RGBX: this.PIXFMT_RGBX,
        BGRA: this.PIXFMT_BGRA, BGRX: this.PIXFMT_BGRX,
      };
      if (format in rgbFormats) {
        const stride = width * 4;
        return {
          pixelFormat: rgbFormats[format],
          payloadSize: stride * height,
          yStride: stride, uStride: 0, vStride: 0,
          uOffset: 0, vOffset: 0,
          layout: [{ offset: 0, stride }],
        };
      }

      const yStride = width;
      const uvHeight = (height + 1) >> 1;
      const ySize = yStride * height;

      if (format === 'NV12') {
        const uvSize = yStride * uvHeight;
        return {
          pixelFormat: this.PIXFMT_NV12,
          payloadSize: ySize + uvSize,
          yStride, uStride: yStride, vStride: 0,
          uOffset: ySize, vOffset: 0,
          layout: [{ offset: 0, stride: yStride }, { offset: ySize, stride: yStride }],
        };
      }

      if (format === 'I420') {
        const uvStride = (width + 1) >> 1;
        const uvSize = uvStride * uvHeight;
        return {
          pixelFormat: this.PIXFMT_YUV420P,
          payloadSize: ySize + uvSize * 2,
          yStride, uStride: uvStride, vStride: uvStride,
          uOffset: ySize, vOffset: ySize + uvSize,
          layout: [
            { offset: 0, stride: yStride },
            { offset: ySize, stride: uvStride },
            { offset: ySize + uvSize, stride: uvStride },
          ],
        };
      }

      return null;
    },

    // Copies straight into wasm memory: under pthreads the heap is a
    // SharedArrayBuffer and existing views stay valid across growth. Browsers
    // whose copyTo() still rejects shared views fall back to a scratch buffer
    // plus one memcpy.
    copyFrame: async function(state, entry, dstPtr) {
      const options = { layout: entry.layout };
      if (entry.visibleRect)
        options.rect = entry.visibleRect;

      if (state.directCopy) {
        try {
          await entry.frame.copyTo(HEAPU8.subarray(dstPtr, dstPtr + entry.payloadSize), options);
          return;
        } catch (e) {
          if (!(e instanceof TypeError))
            throw e;
          state.directCopy = false;
        }
      }

      if (!state.scratch || state.scratch.byteLength < entry.payloadSize)
        state.scratch = new Uint8Array(entry.payloadSize);
      const scratch = state.scratch.subarray(0, entry.payloadSize);
      await entry.frame.copyTo(scratch, options);
      HEAPU8.set(scratch, dstPtr);
    },

    // Build the configure() dictionary from the stored codec parameters.
    buildConfig: function(state, width, height) {
      const config = {
        codec: state.codec,
        optimizeForLatency: true,
        hardwareAcceleration: 'prefer-hardware',
      };
      if (width > 0) config.codedWidth = width;
      if (height > 0) config.codedHeight = height;
      if (state.codec.startsWith('avc1'))
        config.avc = { format: state.annexB ? 'annexb' : 'avc' };
      else if (state.codec.startsWith('hvc1') || state.codec.startsWith('hev1'))
        config.hevc = { format: state.annexB ? 'annexb' : 'hevc' };
      if (state.description)
        config.description = state.description;
      return config;
    },

    writeFrameInfo: function(infoPtr, frame) {
      HEAP32[(infoPtr + this.FI_PIXFMT) >> 2] = frame.pixelFormat | 0;
      HEAP32[(infoPtr + this.FI_WIDTH) >> 2] = frame.width | 0;
      HEAP32[(infoPtr + this.FI_HEIGHT) >> 2] = frame.height | 0;
      HEAP32[(infoPtr + this.FI_Y_STRIDE) >> 2] = frame.yStride | 0;
      HEAP32[(infoPtr + this.FI_U_STRIDE) >> 2] = frame.uStride | 0;
      HEAP32[(infoPtr + this.FI_V_STRIDE) >> 2] = frame.vStride | 0;
      HEAP32[(infoPtr + this.FI_U_OFFSET) >> 2] = frame.uOffset | 0;
      HEAP32[(infoPtr + this.FI_V_OFFSET) >> 2] = frame.vOffset | 0;
      HEAP32[(infoPtr + this.FI_KEYFRAME) >> 2] = frame.keyFrame ? 1 : 0;
      HEAP32[(infoPtr + this.FI_PAYLOAD_SIZE) >> 2] = frame.payloadSize | 0;
      HEAPF64[(infoPtr + this.FI_PTS) >> 3] = frame.ptsSeconds;
      HEAPF64[(infoPtr + this.FI_DURATION) >> 3] = frame.durationSeconds;
    },
  },

  // ---------------------------------------------------------------------------
  // webcodecs_create_decoder: build + configure a VideoDecoder.
  // sig: i (ret) | string*, i, i, u8*, i, i, WebCodecsSharedState*
  // ---------------------------------------------------------------------------
  webcodecs_create_decoder__deps: ['$WebCodecsBridge'],
  webcodecs_create_decoder__proxy: 'sync',
  webcodecs_create_decoder__sig: 'iiiiiiii',
  webcodecs_create_decoder: function(codecPtr, width, height, extraPtr, extraSize, annexB, sharedPtr) {
    if (typeof VideoDecoder === 'undefined' || typeof EncodedVideoChunk === 'undefined') {
      console.warn('WASM WebCodecs: VideoDecoder / EncodedVideoChunk not available');
      return 0;
    }

    try {
      WebCodecsBridge.syncEnumsFromEmbind();
    } catch (e) {
      console.warn('WASM WebCodecs:', e);
      return 0;
    }

    const registry = WebCodecsBridge.ensureRegistry();
    const id = WebCodecsBridge.nextId++;
    const codec = UTF8ToString(codecPtr);

    const state = {
      id,
      codec,
      sharedPtr,
      annexB: !!annexB,
      failed: false,
      errorMessage: '',
      lastTimestamp: 0,
      frames: [],
      copying: false,
      copyDone: 0,
      copyResult: 0,
      pushesProcessed: 0,
      scratch: null,
      directCopy: true,
      generation: 0,
      droppedFrames: 0,
      highWaterMark: 0,
      decoder: null,
      description: null,
      configured: false,
    };

    const errorCallback = (error) => {
      state.failed = true;
      state.errorMessage = 'VideoDecoder error: ' + (error && error.message ? error.message : error);
      console.warn('WASM WebCodecs:', state.errorMessage);
      WebCodecsBridge.publishState(state);
    };

    const outputCallback = (frame) => {
      const visibleRect = frame.visibleRect || null;
      const width = visibleRect ? visibleRect.width : frame.codedWidth;
      const height = visibleRect ? visibleRect.height : frame.codedHeight;
      const timestampMicros = Number.isFinite(frame.timestamp) ? Number(frame.timestamp)
                                                                : state.lastTimestamp;
      const durationMicros = Number.isFinite(frame.duration) ? Number(frame.duration) : 0;
      state.lastTimestamp = timestampMicros;

      const described = WebCodecsBridge.describeFrame(frame, width, height);
      if (!described) {
        state.failed = true;
        state.errorMessage = 'unsupported frame format: ' + frame.format;
        frame.close();
        WebCodecsBridge.publishState(state);
        return;
      }

      // Safety valve: the codec's in-flight cap keeps the queue far below this.
      if (state.frames.length >= WebCodecsBridge.FRAME_QUEUE_HIGH_WATER) {
        state.droppedFrames += 1;
        frame.close();
        WebCodecsBridge.publishState(state);
        return;
      }

      state.frames.push(Object.assign({
        frame,
        visibleRect,
        width,
        height,
        ptsSeconds: timestampMicros / WebCodecsBridge.MICROSECONDS_PER_SECOND,
        durationSeconds: durationMicros / WebCodecsBridge.MICROSECONDS_PER_SECOND,
        keyFrame: frame.type === 'key',
      }, described));
      if (state.frames.length > state.highWaterMark)
        state.highWaterMark = state.frames.length;
      WebCodecsBridge.publishState(state);
    };

    try {
      if (extraSize > 0)
        state.description = HEAPU8.slice(extraPtr, extraPtr + extraSize);

      state.decoder = new VideoDecoder({ output: outputCallback, error: errorCallback });
      state.decoder.addEventListener('dequeue', () => WebCodecsBridge.publishState(state));

      const config = WebCodecsBridge.buildConfig(state, width, height);

      // isConfigSupported is advisory: we log but don't block on it because
      // it's async and we need a synchronous return here. A failed config
      // will surface via the decoder's error callback.
      try {
        VideoDecoder.isConfigSupported(config).then((support) => {
          if (!support || !support.supported) {
            state.failed = true;
            state.errorMessage = 'isConfigSupported rejected config for ' + codec;
            console.warn('WASM WebCodecs: isConfigSupported rejected', codec, support);
            WebCodecsBridge.publishState(state);
          }
        }).catch((error) => {
          state.failed = true;
          state.errorMessage = 'isConfigSupported threw: ' + String(error);
          WebCodecsBridge.publishState(state);
        });
      } catch (probeError) {
        console.warn('WASM WebCodecs: isConfigSupported threw synchronously', probeError);
      }

      state.decoder.configure(config);
      state.configured = true;
      registry.set(id, state);
      WebCodecsBridge.publishState(state);

      console.info('WASM WebCodecs: configured VideoDecoder', {
        codec, annexB: !!annexB, descriptionBytes: extraSize, width, height,
      });
      return id;
    } catch (e) {
      console.warn('WASM WebCodecs: create/configure decoder failed', e);
      // The state never reached the registry, so webcodecs_destroy_decoder cannot
      // reach the decoder to close it.
      try {
        if (state.decoder) state.decoder.close();
      } catch (closeError) {
        console.warn('WASM WebCodecs: decoder close failed', closeError);
      }
      return 0;
    }
  },

  // ---------------------------------------------------------------------------
  webcodecs_destroy_decoder__deps: ['$WebCodecsBridge'],
  webcodecs_destroy_decoder__proxy: 'sync',
  webcodecs_destroy_decoder__sig: 'vi',
  webcodecs_destroy_decoder: function(handle) {
    const state = WebCodecsBridge.getState(handle);
    if (!state) return;
    state.generation += 1;
    for (const entry of state.frames) entry.frame.close();
    state.frames.length = 0;
    try {
      if (state.decoder) state.decoder.close();
    } catch (e) {
      console.warn('WASM WebCodecs: decoder close failed', e);
    }
    // A copy still in flight finishes on its own; it must not publish into
    // memory the codec may have freed by then.
    state.sharedPtr = 0;
    WebCodecsBridge.registry.delete(handle);
  },

  // ---------------------------------------------------------------------------
  webcodecs_reset_decoder__deps: ['$WebCodecsBridge'],
  webcodecs_reset_decoder__proxy: 'sync',
  webcodecs_reset_decoder__sig: 'ii',
  webcodecs_reset_decoder: function(handle) {
    const state = WebCodecsBridge.getState(handle);
    if (!state || !state.decoder) return 0;
    try {
      state.decoder.reset();
      state.generation += 1;
      for (const entry of state.frames) entry.frame.close();
      state.frames.length = 0;
      state.droppedFrames = 0;
      state.highWaterMark = 0;
      state.failed = false;
      state.errorMessage = '';
      // reset() returns the decoder to 'unconfigured'; we must configure again.
      state.decoder.configure(WebCodecsBridge.buildConfig(state, 0, 0));
      WebCodecsBridge.publishState(state);
      return 1;
    } catch (e) {
      state.failed = true;
      state.errorMessage = 'reset failed: ' + String(e);
      WebCodecsBridge.publishState(state);
      return 0;
    }
  },

  // ---------------------------------------------------------------------------
  // Asynchronous: the video thread never waits for the main thread here. The
  // caller malloc'd the packet and this side frees it.
  webcodecs_push_packet__deps: ['$WebCodecsBridge', 'free'],
  webcodecs_push_packet__proxy: 'async',
  webcodecs_push_packet__sig: 'viiiidd',
  webcodecs_push_packet: function(handle, dataPtr, dataSize, keyFrame, ptsSeconds, durationSeconds) {
    const B = WebCodecsBridge;
    const payload = HEAPU8.slice(dataPtr, dataPtr + dataSize);
    _free(dataPtr);
    const state = B.getState(handle);
    if (!state) return;

    state.pushesProcessed += 1;
    if (!state.failed) {
      if (!state.decoder || state.decoder.state !== 'configured') {
        state.failed = true;
        state.errorMessage = 'decoder not configured (state=' +
          (state.decoder ? state.decoder.state : 'null') + ')';
      } else {
        const tsMicros = Math.round(ptsSeconds * B.MICROSECONDS_PER_SECOND);
        const durMicros = Math.max(0, Math.round(durationSeconds * B.MICROSECONDS_PER_SECOND));
        try {
          state.decoder.decode(new EncodedVideoChunk({
            type: keyFrame ? 'key' : 'delta',
            timestamp: tsMicros,
            duration: durMicros > 0 ? durMicros : undefined,
            data: payload,
          }));
        } catch (e) {
          state.failed = true;
          state.errorMessage = 'decode threw: ' + String(e);
        }
      }
    }
    B.publishState(state);
  },

  // ---------------------------------------------------------------------------
  // Asynchronous; the outcome lands in copyDone/copyResult. The head frame is
  // the one the caller sized its buffer for: only its own calls remove frames,
  // and they run in order.
  webcodecs_copy_next_frame__deps: ['$WebCodecsBridge'],
  webcodecs_copy_next_frame__proxy: 'async',
  webcodecs_copy_next_frame__sig: 'viiiii',
  webcodecs_copy_next_frame: function(handle, copyId, dstPtr, dstSize, infoPtr) {
    const B = WebCodecsBridge;
    const state = B.getState(handle);
    if (!state) return;

    const finish = (result) => {
      state.copyDone = copyId;
      state.copyResult = result;
      B.publishState(state);
    };
    if (state.failed || state.copying) return finish(B.COPY_FAILED);
    if (state.frames.length === 0) return finish(B.COPY_NO_FRAME);

    const entry = state.frames[0];
    if (entry.payloadSize > dstSize) {
      B.writeFrameInfo(infoPtr, entry);
      return finish(B.COPY_DST_TOO_SMALL);
    }

    state.frames.shift();
    state.copying = true;
    const generation = state.generation;
    let result = B.COPY_FAILED;
    B.copyFrame(state, entry, dstPtr).then(() => {
      // The codec, and with it infoPtr, may be gone after a destroy.
      if (state.sharedPtr)
        B.writeFrameInfo(infoPtr, entry);
      result = B.COPY_OK;
    }, (e) => {
      if (state.generation === generation) {
        state.failed = true;
        state.errorMessage = 'frame copy failed: ' + String(e);
      }
    }).finally(() => {
      entry.frame.close();
      state.copying = false;
      finish(result);
    });
  },

  // ---------------------------------------------------------------------------
  webcodecs_discard_next_frame__deps: ['$WebCodecsBridge'],
  webcodecs_discard_next_frame__proxy: 'sync',
  webcodecs_discard_next_frame__sig: 'iii',
  webcodecs_discard_next_frame: function(handle, infoPtr) {
    const B = WebCodecsBridge;
    const state = B.getState(handle);
    if (!state || state.frames.length === 0) return 0;

    const entry = state.frames.shift();
    B.writeFrameInfo(infoPtr, entry);
    entry.frame.close();
    B.publishState(state);
    return 1;
  },

  // ---------------------------------------------------------------------------
  webcodecs_read_stats__deps: ['$WebCodecsBridge'],
  webcodecs_read_stats__proxy: 'sync',
  webcodecs_read_stats__sig: 'iiii',
  webcodecs_read_stats: function(handle, droppedPtr, highWaterPtr) {
    const state = WebCodecsBridge.getState(handle);
    if (!state) return 0;
    HEAP32[droppedPtr >> 2] = state.droppedFrames | 0;
    HEAP32[highWaterPtr >> 2] = state.highWaterMark | 0;
    return 1;
  },

  // ---------------------------------------------------------------------------
  webcodecs_take_error__deps: ['$WebCodecsBridge'],
  webcodecs_take_error__proxy: 'sync',
  webcodecs_take_error__sig: 'iiii',
  webcodecs_take_error: function(handle, dstPtr, dstSize) {
    if (dstSize <= 1) return 0;
    const state = WebCodecsBridge.getState(handle);
    if (!state) return 0;
    const decoderState = state.decoder ? state.decoder.state : 'none';
    const message = state.errorMessage || '';
    if (!message && decoderState === 'configured')
      return 0;

    state.errorMessage = '';
    const text = decoderState + '|' + message;
    const encoded = new TextEncoder().encode(text);
    const maxCopy = Math.min(encoded.length, dstSize - 1);
    HEAPU8.set(encoded.subarray(0, maxCopy), dstPtr);
    HEAPU8[dstPtr + maxCopy] = 0;
    return maxCopy;
  },

});
