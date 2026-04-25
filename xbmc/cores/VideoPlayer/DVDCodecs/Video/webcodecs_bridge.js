/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

//
// Emscripten JS library implementing the WebCodecs bridge declared in
// DVDVideoCodecWebCodecsBridge.h. Linked via --js-library (see
// cmake/scripts/wasm/ArchSetup.cmake).
//
// Every exported function has __proxy:'sync' so calls from any pthread are
// automatically marshalled to the main browser thread -- WebCodecs objects
// (VideoDecoder, VideoFrame, EncodedVideoChunk) are only usable there.
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
    BUFFER_POOL_MAX: 32,
    // Cap decoder-side work. Queued frames are deliberately excluded so
    // VideoPlayer can continue draining output after a successful AddData().
    // Beyond this, push_packet reports BUSY so VideoPlayer re-queues the demux
    // packet instead of the decoder silently buffering up frames we would
    // then have to drop.
    MAX_INFLIGHT: 12,

    // Enum values below are pulled from the C++-owned Embind registrations
    // (Module.WebCodecsPixelFormat / Module.WebCodecsPushStatus) on first use
    // by syncEnumsFromEmbind(). The C++ header (DVDVideoCodecWebCodecsBridge.h)
    // is the single source of truth; do not hardcode these here.
    PIXFMT_UNKNOWN: 0,
    PIXFMT_YUV420P: 0,
    PIXFMT_NV12: 0,
    PUSH_QUEUED: 0,
    PUSH_EMPTY: 0,
    PUSH_HANDLE_NOT_FOUND: 0,
    PUSH_DECODER_FAILED: 0,
    PUSH_NOT_CONFIGURED: 0,
    PUSH_DECODE_THREW: 0,
    PUSH_BUSY: 0,
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
      const ps = Module['WebCodecsPushStatus'];
      if (!pf || !ps)
        throw new Error('WebCodecs bridge enums missing from Module (Embind not linked?)');
      this.PIXFMT_UNKNOWN = pf.UNKNOWN.value;
      this.PIXFMT_YUV420P = pf.YUV420P.value;
      this.PIXFMT_NV12 = pf.NV12.value;
      this.PUSH_QUEUED = ps.QUEUED.value;
      this.PUSH_EMPTY = ps.EMPTY.value;
      this.PUSH_HANDLE_NOT_FOUND = ps.HANDLE_NOT_FOUND.value;
      this.PUSH_DECODER_FAILED = ps.DECODER_FAILED.value;
      this.PUSH_NOT_CONFIGURED = ps.NOT_CONFIGURED.value;
      this.PUSH_DECODE_THREW = ps.DECODE_THREW.value;
      this.PUSH_BUSY = ps.BUSY.value;
      this._enumsReady = true;
    },

    getState: function(handle) {
      return this.registry ? this.registry.get(handle) : null;
    },

    // Reuse previously-released payload buffers so the hot path does not
    // allocate 1-3 MB per decoded frame (that rate of GC is what causes the
    // visible stutters we observed before pooling).
    acquirePayloadBuffer: function(state, size) {
      const pool = state.bufferPool;
      for (let i = 0; i < pool.length; ++i) {
        if (pool[i].byteLength >= size)
          return pool.splice(i, 1)[0].subarray(0, size);
      }
      return new Uint8Array(size);
    },

    releasePayloadBuffer: function(state, payload) {
      if (!payload || state.bufferPool.length >= this.BUFFER_POOL_MAX)
        return;
      state.bufferPool.push(new Uint8Array(payload.buffer));
    },

    // Copy a VideoFrame in its native planar layout into a single Uint8Array
    // sourced from the pool. Returns strides/offsets and the pixel-format id.
    // Hardware decoders usually output NV12; forcing I420 via copyTo throws
    // NotSupportedError on Chrome.
    copyFrameNative: async function(state, frame, width, height, visibleRect) {
      const frameFormat = frame.format || 'I420';
      const copyOptions = {};
      if (visibleRect)
        copyOptions.rect = visibleRect;
      const yStride = width;
      const uvHeight = (height + 1) >> 1;
      const ySize = yStride * height;

      if (frameFormat === 'NV12') {
        const uvStride = yStride;
        const uvSize = uvStride * uvHeight;
        const payload = this.acquirePayloadBuffer(state, ySize + uvSize);
        await frame.copyTo(payload, {
          ...copyOptions,
          layout: [
            { offset: 0, stride: yStride },
            { offset: ySize, stride: uvStride },
          ],
        });
        return {
          payload,
          pixelFormat: this.PIXFMT_NV12,
          yStride,
          uStride: uvStride,
          vStride: 0,
          uOffset: ySize,
          vOffset: 0,
        };
      }

      if (frameFormat === 'I420' || frameFormat === 'I420A') {
        const uvStride = (width + 1) >> 1;
        const uvSize = uvStride * uvHeight;
        const payload = this.acquirePayloadBuffer(state, ySize + uvSize * 2);
        await frame.copyTo(payload, {
          ...copyOptions,
          layout: [
            { offset: 0, stride: yStride },
            { offset: ySize, stride: uvStride },
            { offset: ySize + uvSize, stride: uvStride },
          ],
        });
        return {
          payload,
          pixelFormat: this.PIXFMT_YUV420P,
          yStride,
          uStride: uvStride,
          vStride: uvStride,
          uOffset: ySize,
          vOffset: ySize + uvSize,
        };
      }

      throw new Error('unsupported frame format: ' + frameFormat);
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
      HEAP32[(infoPtr + this.FI_PAYLOAD_SIZE) >> 2] = frame.payload.byteLength | 0;
      HEAPF64[(infoPtr + this.FI_PTS) >> 3] = frame.ptsSeconds;
      HEAPF64[(infoPtr + this.FI_DURATION) >> 3] = frame.durationSeconds;
    },
  },

  // ---------------------------------------------------------------------------
  // webcodecs_create_decoder: build + configure a VideoDecoder.
  // sig: i (ret) | string*, i, i, u8*, i, i
  // ---------------------------------------------------------------------------
  webcodecs_create_decoder__deps: ['$WebCodecsBridge'],
  webcodecs_create_decoder__proxy: 'sync',
  webcodecs_create_decoder__sig: 'iiiiiii',
  webcodecs_create_decoder: function(codecPtr, width, height, extraPtr, extraSize, annexB) {
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
      annexB: !!annexB,
      failed: false,
      errorMessage: '',
      lastTimestamp: 0,
      frames: [],
      bufferPool: [],
      pendingCopies: 0,
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
    };

    const outputCallback = (frame) => {
      const visibleRect = frame.visibleRect || null;
      const width = visibleRect ? visibleRect.width : frame.codedWidth;
      const height = visibleRect ? visibleRect.height : frame.codedHeight;
      const timestampMicros = Number.isFinite(frame.timestamp) ? Number(frame.timestamp)
                                                                : state.lastTimestamp;
      const durationMicros = Number.isFinite(frame.duration) ? Number(frame.duration) : 0;
      state.lastTimestamp = timestampMicros;
      const generation = state.generation;

      state.pendingCopies += 1;
      (async () => {
        let copied = null;
        try {
          // Safety valve: drop if the queue is already full. In steady state
          // push_packet returns BUSY long before we reach this point.
          if (state.frames.length >= WebCodecsBridge.FRAME_QUEUE_HIGH_WATER) {
            state.droppedFrames += 1;
            return;
          }
          copied = await WebCodecsBridge.copyFrameNative(state, frame, width, height, visibleRect);
          if (state.generation !== generation) {
            WebCodecsBridge.releasePayloadBuffer(state, copied.payload);
            return;
          }

          state.frames.push({
            payload: copied.payload,
            pixelFormat: copied.pixelFormat,
            width,
            height,
            yStride: copied.yStride,
            uStride: copied.uStride,
            vStride: copied.vStride,
            uOffset: copied.uOffset,
            vOffset: copied.vOffset,
            ptsSeconds: timestampMicros / WebCodecsBridge.MICROSECONDS_PER_SECOND,
            durationSeconds: durationMicros / WebCodecsBridge.MICROSECONDS_PER_SECOND,
            keyFrame: frame.type === 'key',
          });
          if (state.frames.length > state.highWaterMark)
            state.highWaterMark = state.frames.length;
        } catch (e) {
          if (state.generation === generation) {
            state.failed = true;
            state.errorMessage = 'frame copy failed: ' + String(e);
          }
        } finally {
          frame.close();
          if (state.generation === generation && state.pendingCopies > 0)
            state.pendingCopies -= 1;
        }
      })();
    };

    try {
      if (extraSize > 0)
        state.description = HEAPU8.slice(extraPtr, extraPtr + extraSize);

      state.decoder = new VideoDecoder({ output: outputCallback, error: errorCallback });

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
          }
        }).catch((error) => {
          state.failed = true;
          state.errorMessage = 'isConfigSupported threw: ' + String(error);
        });
      } catch (probeError) {
        console.warn('WASM WebCodecs: isConfigSupported threw synchronously', probeError);
      }

      state.decoder.configure(config);
      state.configured = true;
      registry.set(id, state);

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
    state.pendingCopies = 0;
    state.frames.length = 0;
    try {
      if (state.decoder) state.decoder.close();
    } catch (e) {
      console.warn('WASM WebCodecs: decoder close failed', e);
    }
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
      state.frames.length = 0;
      state.pendingCopies = 0;
      state.droppedFrames = 0;
      state.highWaterMark = 0;
      state.failed = false;
      state.errorMessage = '';
      // reset() returns the decoder to 'unconfigured'; we must configure again.
      state.decoder.configure(WebCodecsBridge.buildConfig(state, 0, 0));
      return 1;
    } catch (e) {
      state.failed = true;
      state.errorMessage = 'reset failed: ' + String(e);
      return 0;
    }
  },

  // ---------------------------------------------------------------------------
  webcodecs_push_packet__deps: ['$WebCodecsBridge'],
  webcodecs_push_packet__proxy: 'sync',
  webcodecs_push_packet__sig: 'iiiiidd',
  webcodecs_push_packet: function(handle, dataPtr, dataSize, keyFrame, ptsSeconds, durationSeconds) {
    const B = WebCodecsBridge;
    const state = B.getState(handle);
    if (!state) return B.PUSH_HANDLE_NOT_FOUND;
    if (state.failed) return B.PUSH_DECODER_FAILED;
    if (!state.decoder || state.decoder.state !== 'configured') {
      if (!state.errorMessage)
        state.errorMessage = 'decoder not configured (state=' +
          (state.decoder ? state.decoder.state : 'null') + ')';
      return B.PUSH_NOT_CONFIGURED;
    }

    if (dataSize <= 0)
      return B.PUSH_EMPTY;

    if (state.decoder.decodeQueueSize + state.pendingCopies >= B.MAX_INFLIGHT)
      return B.PUSH_BUSY;

    const tsMicros = Math.max(0, Math.round(ptsSeconds * B.MICROSECONDS_PER_SECOND));
    const durMicros = Math.max(0, Math.round(durationSeconds * B.MICROSECONDS_PER_SECOND));
    const payload = HEAPU8.slice(dataPtr, dataPtr + dataSize);

    try {
      state.decoder.decode(new EncodedVideoChunk({
        type: keyFrame ? 'key' : 'delta',
        timestamp: tsMicros,
        duration: durMicros > 0 ? durMicros : undefined,
        data: payload,
      }));
      return B.PUSH_QUEUED;
    } catch (e) {
      state.failed = true;
      state.errorMessage = 'decode threw: ' + String(e);
      return B.PUSH_DECODE_THREW;
    }
  },

  // ---------------------------------------------------------------------------
  webcodecs_drain_decoder__deps: ['$WebCodecsBridge'],
  webcodecs_drain_decoder__proxy: 'sync',
  webcodecs_drain_decoder__sig: 'ii',
  webcodecs_drain_decoder: function(handle) {
    const state = WebCodecsBridge.getState(handle);
    if (!state || !state.decoder) return 0;
    return (state.frames.length + state.decoder.decodeQueueSize + state.pendingCopies) | 0;
  },

  // ---------------------------------------------------------------------------
  webcodecs_copy_next_frame__deps: ['$WebCodecsBridge'],
  webcodecs_copy_next_frame__proxy: 'sync',
  webcodecs_copy_next_frame__sig: 'iiiii',
  webcodecs_copy_next_frame: function(handle, dstPtr, dstSize, infoPtr) {
    const B = WebCodecsBridge;
    const state = B.getState(handle);
    if (!state) return 0;
    if (state.failed) return -1;
    if (state.frames.length === 0) return 0;

    const frame = state.frames[0];
    if (!frame) return 0;
    if (frame.payload.byteLength > dstSize) {
      B.writeFrameInfo(infoPtr, frame);
      return -2;
    }

    HEAPU8.set(frame.payload, dstPtr);
    B.writeFrameInfo(infoPtr, frame);

    state.frames.shift();
    B.releasePayloadBuffer(state, frame.payload);
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
