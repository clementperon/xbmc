// A/V health of the last file played, from Kodi's log: sync errors, resyncs,
// caching, underruns, audio lines, errors, and the histogram of intervals
// between presented frames (needs LOGAVTIMING, see debug_logging_on.js).
(() => {
  const lines = FS.readFile('/home/web_user/.kodi/temp/kodi.log', { encoding: 'utf8' }).split('\n');
  let start = 0;
  for (let i = 0; i < lines.length; i++) if (/VideoPlayer::OpenFile/.test(lines[i])) start = i;
  const tail = lines.slice(start);
  const out = { totalLines: lines.length, playbackLines: tail.length, lastTimestamp: tail[tail.length - 2]?.slice(11, 24) };
  const pick = (re, max) => tail.filter((l) => re.test(l)).map((l) => l.slice(11, 250)).slice(-max);
  out.syncErrorCount = tail.filter((l) => /large audio sync error/.test(l)).length;
  out.syncErrors = pick(/large audio sync error/, 6);
  out.syncStream = pick(/SyncStream|start sync|Discontinuity|SetSpeedAdjust - adjusted:(?!0\.000000)/, 20);
  out.caching = pick(/SetCaching|caching state|CVideoPlayer::SetCaching/, 12);
  out.underruns = pick(/underrun/, 8);
  out.audio = pick(/<audio>/, 20);
  out.errors = pick(/ error </, 8);

  const stamps = [];
  for (const l of tail) {
    const m = /^\S+ (\d\d):(\d\d):(\d\d)\.(\d\d\d) .*render: true/.exec(l);
    if (m) stamps.push(((+m[1] * 60 + +m[2]) * 60 + +m[3]) * 1000 + +m[4]);
  }
  const hist = {};
  for (let i = 1; i < stamps.length; i++) {
    const k = (Math.round((stamps[i] - stamps[i - 1]) / 8.33) * 8.33).toFixed(0);
    hist[k] = (hist[k] || 0) + 1;
  }
  out.presented = stamps.length;
  out.presentGapHistogramMs = hist;
  out.seconds = stamps.length ? (stamps[stamps.length - 1] - stamps[0]) / 1000 : 0;
  return out;
})()
