/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// Web Audio output for CWasmAudioWorkletManager. The AudioContext lives on the
// browser main thread. The processor reads the manager's ring and counters
// straight from the wasm memory SharedArrayBuffer, so the AudioWorklet scope
// never instantiates the wasm module.
mergeInto(LibraryManager.library, {
  $WasmAudio: {
    ctx: null,
    node: null,
    moduleUrl: null,
    resumeHandler: null,

    // Views are created once over fixed byte ranges: the SharedArrayBuffer
    // keeps the length it had when it was posted, and the ring and counters
    // were allocated before that and do not move while the node exists.
    PROCESSOR_SOURCE: `
class KodiAudioProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions;
    const b = o.buffer;
    this.ready = new Uint8Array(b, o.ready, 1);
    this.prebufferComplete = new Uint8Array(b, o.prebufferComplete, 1);
    this.active = new Int32Array(b, o.activeCallbacks, 1);
    this.readFrame = new BigUint64Array(b, o.readFrame, 1);
    this.writeFrame = new BigUint64Array(b, o.writeFrame, 1);
    this.underrunFrames = new BigUint64Array(b, o.underrunFrames, 1);
    this.capacity = o.capacity;
    this.prebuffer = o.prebuffer;
    this.planes = [];
    for (let ch = 0; ch < o.channels; ++ch)
      this.planes.push(new Float32Array(b, o.ring + ch * o.capacity * 4, o.capacity));
  }

  process(inputs, outputs) {
    const out = outputs[0];
    Atomics.add(this.active, 0, 1);
    if (!this.render(out))
      for (let ch = 0; ch < out.length; ++ch)
        out[ch].fill(0);
    Atomics.sub(this.active, 0, 1);
    return true;
  }

  render(out) {
    if (out.length === 0 || !Atomics.load(this.ready, 0))
      return false;

    const frames = out[0].length;
    const read = Number(Atomics.load(this.readFrame, 0));
    const available = Number(Atomics.load(this.writeFrame, 0)) - read;
    if (!Atomics.load(this.prebufferComplete, 0)) {
      if (available < this.prebuffer)
        return false;
      Atomics.store(this.prebufferComplete, 0, 1);
    }

    const toRead = Math.min(available, frames);
    if (toRead < frames)
      Atomics.add(this.underrunFrames, 0, BigInt(frames - toRead));
    if (toRead === 0)
      return false;

    const start = read % this.capacity;
    const first = Math.min(toRead, this.capacity - start);
    for (let ch = 0; ch < out.length; ++ch) {
      const dst = out[ch];
      if (ch >= this.planes.length) {
        dst.fill(0);
        continue;
      }
      const src = this.planes[ch];
      for (let i = 0; i < first; ++i)
        dst[i] = src[start + i];
      for (let i = first; i < toRead; ++i)
        dst[i] = src[i - first];
      dst.fill(0, toRead);
    }

    Atomics.store(this.readFrame, 0, BigInt(read + toRead));
    return true;
  }
}
registerProcessor('kodi-audio', KodiAudioProcessor);
`,

    setResumeHooks: function(handler) {
      const method = handler ? 'addEventListener' : 'removeEventListener';
      const target = handler || this.resumeHandler;
      if (!target)
        return;
      window[method]('pointerdown', target, true);
      window[method]('keydown', target, true);
      window[method]('touchstart', target, true);
      document[method]('visibilitychange', target, true);
      this.resumeHandler = handler;
    },

    // Autoplay policy keeps a context suspended until a user gesture.
    armResume: function(ctx) {
      if (ctx.state === 'running' || this.resumeHandler)
        return;
      const tryResume = () => {
        if (ctx.state !== 'running')
          ctx.resume().catch(() => {});
        if (ctx.state === 'running')
          this.setResumeHooks(null);
      };
      this.setResumeHooks(tryResume);
    },
  },

  wasm_audio_open__deps: ['$WasmAudio'],
  wasm_audio_open__proxy: 'sync',
  wasm_audio_open__sig: 'iiiii',
  wasm_audio_open: function(sampleRate, runningFlag, sampleRateOut, quantumSizeOut) {
    let ctx;
    try {
      // The largest output buffer the browser offers; the added latency is part
      // of outputLatency and so of the delay the sink reports.
      ctx = new AudioContext({latencyHint: 'playback', sampleRate: sampleRate || undefined});
    } catch (e) {
      console.error('WASM AudioWorklet: cannot create AudioContext', e);
      return 0;
    }
    WasmAudio.ctx = ctx;
    HEAP32[sampleRateOut >> 2] = ctx.sampleRate;
    HEAP32[quantumSizeOut >> 2] = ctx.renderQuantumSize || 128;

    ctx.onstatechange = () => {
      Atomics.store(HEAPU8, runningFlag, ctx.state === 'running' ? 1 : 0);
      if (ctx.state !== 'closed')
        WasmAudio.armResume(ctx);
    };
    ctx.onstatechange();
    return 1;
  },

  wasm_audio_start__deps: ['$WasmAudio'],
  wasm_audio_start__proxy: 'sync',
  wasm_audio_start__sig: 'viiiiiiiiiii',
  wasm_audio_start: function(channels, ring, capacity, prebuffer, ready, prebufferComplete,
                             activeCallbacks, readFrame, writeFrame, underrunFrames, state) {
    const ctx = WasmAudio.ctx;
    const finish = (ok) => {
      Atomics.store(HEAP32, state >> 2, ok ? 1 : 2);
      Atomics.notify(HEAP32, state >> 2);
    };
    if (!ctx) {
      finish(false);
      return;
    }
    if (!WasmAudio.moduleUrl)
      WasmAudio.moduleUrl = URL.createObjectURL(
          new Blob([WasmAudio.PROCESSOR_SOURCE], {type: 'text/javascript'}));

    ctx.audioWorklet.addModule(WasmAudio.moduleUrl)
        .then(() => {
          // Closed while the module loaded; the caller has stopped waiting.
          if (WasmAudio.ctx !== ctx)
            return;
          const node = new AudioWorkletNode(ctx, 'kodi-audio', {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [channels],
            channelCount: channels,
            channelCountMode: 'explicit',
            channelInterpretation: 'speakers',
            processorOptions: {
              buffer: wasmMemory.buffer,
              channels: channels,
              ring: ring,
              capacity: capacity,
              prebuffer: prebuffer,
              ready: ready,
              prebufferComplete: prebufferComplete,
              activeCallbacks: activeCallbacks,
              readFrame: readFrame,
              writeFrame: writeFrame,
              underrunFrames: underrunFrames,
            },
          });
          // AudioDestinationNode.channelCount defaults to 2 regardless of the
          // hardware, which would fold a 5.1 node to stereo.
          try {
            ctx.destination.channelCount = Math.min(channels, ctx.destination.maxChannelCount);
          } catch (e) {
            console.warn('WASM AudioWorklet: cannot set destination channelCount', e);
          }
          node.connect(ctx.destination);
          WasmAudio.node = node;
          finish(true);
        })
        .catch((e) => {
          console.error('WASM AudioWorklet: cannot start the processor', e);
          if (WasmAudio.ctx === ctx)
            finish(false);
        });
  },

  wasm_audio_close__deps: ['$WasmAudio'],
  wasm_audio_close__proxy: 'sync',
  wasm_audio_close__sig: 'v',
  wasm_audio_close: function() {
    const ctx = WasmAudio.ctx;
    if (!ctx)
      return;
    WasmAudio.ctx = null;
    WasmAudio.setResumeHooks(null);
    ctx.onstatechange = null;
    if (WasmAudio.node) {
      WasmAudio.node.disconnect();
      WasmAudio.node = null;
    }
    ctx.close().catch(() => {});
  },

  wasm_audio_latency_us__deps: ['$WasmAudio'],
  wasm_audio_latency_us__proxy: 'sync',
  wasm_audio_latency_us__sig: 'i',
  wasm_audio_latency_us: function() {
    const ctx = WasmAudio.ctx;
    if (!ctx)
      return 0;
    let total = 0;
    if (typeof ctx.baseLatency === 'number' && isFinite(ctx.baseLatency))
      total += ctx.baseLatency;
    if (typeof ctx.outputLatency === 'number' && isFinite(ctx.outputLatency))
      total += ctx.outputLatency;
    return total > 0 ? Math.min(Math.floor(total * 1e6), 0x7fffffff) : 0;
  },

  wasm_audio_max_output_channels__deps: ['$WasmAudio'],
  wasm_audio_max_output_channels__proxy: 'sync',
  wasm_audio_max_output_channels__sig: 'i',
  wasm_audio_max_output_channels: function() {
    let ctx = WasmAudio.ctx;
    const temporary = !ctx;
    try {
      if (!ctx)
        ctx = new AudioContext();
      return ctx.destination.maxChannelCount | 0;
    } catch (e) {
      return 0;
    } finally {
      if (temporary && ctx)
        ctx.close().catch(() => {});
    }
  },
});
