// Environment of the page Kodi runs in: browser, cores, canvas, the refresh
// interval requestAnimationFrame delivers, VideoDecoder support per codec and
// hardware preference, AudioContext latencies.
(async () => {
  const out = {};
  out.ua = navigator.userAgent;
  out.cores = navigator.hardwareConcurrency;
  out.visibility = document.visibilityState;
  out.dpr = devicePixelRatio;
  out.screen = [screen.width, screen.height];
  out.canvas = Module.canvas ? [Module.canvas.width, Module.canvas.height, Module.canvas.clientWidth, Module.canvas.clientHeight] : null;
  out.uptimeS = Math.round(performance.now() / 1000);

  out.rafIntervalsMs = await new Promise((resolve) => {
    const ts = [];
    const tick = (t) => {
      ts.push(t);
      if (ts.length < 61) requestAnimationFrame(tick);
      else resolve(ts.slice(1).map((v, i) => +(v - ts[i]).toFixed(2)));
    };
    requestAnimationFrame(tick);
  });
  const sorted = [...out.rafIntervalsMs].sort((a, b) => a - b);
  out.rafMedianMs = sorted[Math.floor(sorted.length / 2)];
  out.rafMinMaxMs = [sorted[0], sorted[sorted.length - 1]];

  out.hasVideoDecoder = typeof VideoDecoder;
  out.decoders = {};
  if (typeof VideoDecoder !== 'undefined') {
    const probes = {
      'avc1.640028 (H.264 High 4.0)': 'avc1.640028',
      'hvc1.1.6.L120.B0 (HEVC Main 4.0)': 'hvc1.1.6.L120.B0',
      'hvc1.2.4.L153.B0 (HEVC Main10 5.1)': 'hvc1.2.4.L153.B0',
      'vp09.00.31.08 (VP9 profile 0)': 'vp09.00.31.08',
      'vp09.02.31.10 (VP9 profile 2 10-bit)': 'vp09.02.31.10',
      vp8: 'vp8',
      'av01.0.08M.08 (AV1 Main 4.0)': 'av01.0.08M.08',
      'av01.0.08M.10 (AV1 Main 10-bit)': 'av01.0.08M.10',
    };
    for (const [label, codec] of Object.entries(probes)) {
      const res = {};
      for (const hw of ['prefer-hardware', 'prefer-software', 'no-preference']) {
        try {
          const r = await VideoDecoder.isConfigSupported({ codec, hardwareAcceleration: hw, codedWidth: 1920, codedHeight: 1080 });
          res[hw] = r.supported;
        } catch (e) {
          res[hw] = 'err: ' + e.message;
        }
      }
      out.decoders[label] = res;
    }
  }
  try {
    const ctx = new AudioContext({ latencyHint: 'playback' });
    out.audio = {
      sampleRate: ctx.sampleRate,
      baseLatencyMs: +(ctx.baseLatency * 1000).toFixed(2),
      outputLatencyMs: +((ctx.outputLatency || 0) * 1000).toFixed(2),
      maxChannels: ctx.destination.maxChannelCount,
      state: ctx.state,
    };
    await ctx.close();
  } catch (e) {
    out.audio = 'err: ' + e.message;
  }
  return out;
})()
