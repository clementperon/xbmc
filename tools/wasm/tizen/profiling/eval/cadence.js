// Frame cadence of the last file played, from the LOGAVTIMING lines: frames
// presented, fps, histogram of wall-clock gaps between presented frames (bins
// of one 120 Hz interval), histogram of pts steps (bins of one 24p frame),
// and frames whose pts step says a source frame was skipped.
(() => {
  const lines = FS.readFile('/home/web_user/.kodi/temp/kodi.log', { encoding: 'utf8' }).split('\n');
  let start = 0;
  for (let i = 0; i < lines.length; i++) if (/VideoPlayer::OpenFile/.test(lines[i])) start = i;
  // A new nextFramePts means the previous frame was presented.
  let lastPts = null;
  let lastT = null;
  let first = null;
  let last = null;
  let frames = 0;
  const gaps = [];
  const ptsSteps = [];
  for (const l of lines.slice(start)) {
    const m = /^\S+ (\d\d):(\d\d):(\d\d)\.(\d\d\d) .*frameOnScreen: (-?[\d.]+) renderPts: (-?[\d.]+) nextFramePts: (-?[\d.]+)/.exec(l);
    if (!m) continue;
    const t = ((+m[1] * 60 + +m[2]) * 60 + +m[3]) * 1000 + +m[4];
    const pts = +m[7];
    if (lastPts !== null && pts !== lastPts) {
      frames++;
      gaps.push(t - lastT);
      ptsSteps.push((pts - lastPts) / 1000);
      if (first === null) first = t;
      last = t;
      lastT = t;
    }
    if (lastPts === null) lastT = t;
    lastPts = pts;
  }
  const hist = (arr, q) => {
    const h = {};
    for (const v of arr) {
      const k = (Math.round(v / q) * q).toFixed(1);
      h[k] = (h[k] || 0) + 1;
    }
    return h;
  };
  const seconds = (last - first) / 1000;
  return {
    framesPresented: frames,
    seconds,
    fps: frames / seconds,
    wallGapHistogramMs: hist(gaps, 8.33),
    ptsStepHistogramMs: hist(ptsSteps, 41.67),
    skippedFrames: ptsSteps.filter((s) => s > 41.67 * 1.5).length,
  };
})()
