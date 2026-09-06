// Chrome DevTools Protocol client for Kodi running under the Tizen Web
// Inspector (tools/wasm/tizen/inspect.sh forwards it to localhost:7011).
//
//   node kodiprof.mjs probe                         handshake, browser version, target list
//   node kodiprof.mjs targets                       page and worker targets with their attach state
//   node kodiprof.mjs console <seconds> [regex]     console output of the page and its workers
//   node kodiprof.mjs eval <file.js>                evaluate a JS expression on the page, print its value
//   node kodiprof.mjs screenshot <file.png>
//   node kodiprof.mjs cpu <seconds> [outdir]        CPU profile of the main thread and every worker
//   node kodiprof.mjs cpu-onplay <seconds> [outdir] attach now, profile once playback has started
//   node kodiprof.mjs close                         close the page so inspect.sh can relaunch Kodi
//
// KODI_CDP_HTTP overrides the DevTools HTTP endpoint (default http://localhost:7011).
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import http from 'node:http';

const BASE = process.env.KODI_CDP_HTTP || 'http://localhost:7011';
const SAMPLING_INTERVAL_US = 500;
const PLAYBACK_STARTED = /Using WebCodecs for video decoding/;
const PLAYBACK_SETTLE_MS = 12000;
const PLAYBACK_WAIT_MS = 8 * 60 * 1000;

const [, , mode, arg1, arg2] = process.argv;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const fmtArg = (a) => (a.value !== undefined ? String(a.value) : a.description || a.type);

const getJson = (path) =>
  new Promise((resolve, reject) => {
    http
      .get(BASE + path, (res) => {
        let body = '';
        res.on('data', (d) => (body += d));
        res.on('end', () => {
          try {
            resolve(JSON.parse(body));
          } catch {
            reject(new Error(`bad json from ${path}: ${body.slice(0, 200)}`));
          }
        });
      })
      .on('error', reject);
  });

// Minimal RFC 6455 client over node:http. Node's WebSocket sends an Origin
// header that the TV's DevTools server refuses, and this way the server's HTTP
// answer is visible when it does refuse.
function connect(wsUrl) {
  return new Promise((resolve, reject) => {
    const url = new URL(wsUrl);
    const key = Buffer.from(Array.from({ length: 16 }, () => Math.floor(Math.random() * 256))).toString('base64');
    const req = http.request({
      host: url.hostname,
      port: url.port,
      path: url.pathname,
      headers: { Connection: 'Upgrade', Upgrade: 'websocket', 'Sec-WebSocket-Version': '13', 'Sec-WebSocket-Key': key },
    });
    req.on('response', (res) => {
      let body = '';
      res.on('data', (d) => (body += d));
      res.on('end', () => reject(new Error(`HTTP ${res.statusCode}: ${body.slice(0, 300)}`)));
    });
    req.on('error', reject);
    req.on('upgrade', (res, socket) => {
      socket.setNoDelay(true);
      const listeners = [];
      const send = (text) => {
        const payload = Buffer.from(text, 'utf8');
        let header;
        if (payload.length < 126) {
          header = Buffer.from([0x81, 0x80 | payload.length]);
        } else if (payload.length < 65536) {
          header = Buffer.alloc(4);
          header[0] = 0x81;
          header[1] = 0x80 | 126;
          header.writeUInt16BE(payload.length, 2);
        } else {
          header = Buffer.alloc(10);
          header[0] = 0x81;
          header[1] = 0x80 | 127;
          header.writeBigUInt64BE(BigInt(payload.length), 2);
        }
        socket.write(Buffer.concat([header, Buffer.alloc(4), payload]));
      };
      let buf = Buffer.alloc(0);
      let fragments = [];
      socket.on('data', (chunk) => {
        buf = Buffer.concat([buf, chunk]);
        for (;;) {
          if (buf.length < 2) return;
          const fin = (buf[0] & 0x80) !== 0;
          const opcode = buf[0] & 0x0f;
          let len = buf[1] & 0x7f;
          let off = 2;
          if (len === 126) {
            if (buf.length < 4) return;
            len = buf.readUInt16BE(2);
            off = 4;
          } else if (len === 127) {
            if (buf.length < 10) return;
            len = Number(buf.readBigUInt64BE(2));
            off = 10;
          }
          if (buf.length < off + len) return;
          const payload = buf.subarray(off, off + len);
          buf = buf.subarray(off + len);
          if (opcode === 0x8) {
            socket.end();
            return;
          }
          if (opcode === 0x9) {
            socket.write(Buffer.concat([Buffer.from([0x8a, 0x80]), Buffer.alloc(4)]));
            continue;
          }
          if (opcode === 0x1 || opcode === 0x0) {
            fragments.push(Buffer.from(payload));
            if (fin) {
              const text = Buffer.concat(fragments).toString('utf8');
              fragments = [];
              for (const listener of listeners) listener(text);
            }
          }
        }
      });
      resolve({ send, onMessage: (l) => listeners.push(l), close: () => socket.end() });
    });
    req.setTimeout(8000, () => {
      reject(new Error('handshake timeout'));
      req.destroy();
    });
    req.end();
  });
}

async function session(wsUrl) {
  const ws = await connect(wsUrl);
  let nextId = 0;
  const pending = new Map();
  const eventListeners = [];
  ws.onMessage((text) => {
    const m = JSON.parse(text);
    if (m.id && pending.has(m.id)) {
      pending.get(m.id)(m);
      pending.delete(m.id);
      return;
    }
    for (const listener of eventListeners) listener(m);
  });
  const send = (method, params = {}, sessionId) =>
    new Promise((resolve) => {
      const id = ++nextId;
      pending.set(id, resolve);
      ws.send(JSON.stringify(sessionId ? { id, method, params, sessionId } : { id, method, params }));
    });
  return { send, on: (l) => eventListeners.push(l), close: ws.close };
}

// Returns sessionId -> targetInfo for every worker. Workers a previous client
// left attached are not auto-attached again, so they are attached explicitly.
async function attachWorkers(cdp, onAttached) {
  const sessions = new Map();
  cdp.on(async (m) => {
    if (m.method === 'Target.attachedToTarget') {
      const { sessionId, targetInfo } = m.params;
      sessions.set(sessionId, targetInfo);
      await onAttached(sessionId, targetInfo);
      if (m.params.waitingForDebugger) await cdp.send('Runtime.runIfWaitingForDebugger', {}, sessionId);
    }
    if (m.method === 'Target.detachedFromTarget') sessions.delete(m.params.sessionId);
  });
  const auto = await cdp.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true });
  if (auto.error) console.log('setAutoAttach:', JSON.stringify(auto.error));
  await sleep(500);
  const all = await cdp.send('Target.getTargets');
  for (const t of (all.result || {}).targetInfos || []) {
    if (t.type !== 'worker') continue;
    if ([...sessions.values()].some((info) => info.targetId === t.targetId)) continue;
    const a = await cdp.send('Target.attachToTarget', { targetId: t.targetId, flatten: true });
    if (a.error) console.log(`attach ${t.targetId.slice(0, 6)}: ${JSON.stringify(a.error)}`);
  }
  await sleep(500);
  return sessions;
}

const workerLabel = (info) => `worker-${info.targetId.slice(0, 6)}`;

async function startProfiler(cdp, started, sid, name) {
  const e = await cdp.send('Profiler.enable', {}, sid);
  if (e.error) {
    console.log(`${name}: Profiler.enable failed: ${JSON.stringify(e.error)}`);
    return;
  }
  await cdp.send('Profiler.setSamplingInterval', { interval: SAMPLING_INTERVAL_US }, sid);
  const s = await cdp.send('Profiler.start', {}, sid);
  if (s.error) {
    console.log(`${name}: Profiler.start failed: ${JSON.stringify(s.error)}`);
    return;
  }
  started.set(sid || '', name);
}

async function stopProfilers(cdp, started, outdir) {
  mkdirSync(outdir, { recursive: true });
  for (const [sid, name] of started) {
    const r = await cdp.send('Profiler.stop', {}, sid || undefined);
    if (r.error || !r.result) {
      console.log(`${name}: stop failed ${JSON.stringify(r.error)}`);
      continue;
    }
    const file = `${outdir}/${name}.cpuprofile`;
    writeFileSync(file, JSON.stringify(r.result.profile));
    console.log(`saved ${file} (${r.result.profile.samples.length} samples)`);
    printSelfTime(name, r.result.profile);
  }
}

function printSelfTime(name, profile, top = 25) {
  const byId = new Map(profile.nodes.map((n) => [n.id, n]));
  const self = new Map();
  let total = 0;
  for (let i = 0; i < profile.samples.length; i++) {
    const f = byId.get(profile.samples[i]).callFrame;
    const d = profile.timeDeltas[i] || 0;
    total += d;
    const src = (f.url || '').split('/').pop();
    const key = `${f.functionName || '(anonymous)'}  ${src}${f.lineNumber >= 0 ? ':' + f.lineNumber : ''}`;
    self.set(key, (self.get(key) || 0) + d);
  }
  console.log(`\n== ${name}: ${(total / 1e6).toFixed(1)}s sampled, ${profile.samples.length} samples`);
  for (const [k, v] of [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, top))
    console.log(`${((100 * v) / total).toFixed(1).padStart(5)}%  ${k}`);
}

async function profileAll(cdp, seconds, outdir, sessions) {
  const started = new Map();
  for (const [sid, info] of sessions) await startProfiler(cdp, started, sid, workerLabel(info));
  await startProfiler(cdp, started, undefined, 'main');
  console.log(`profiling ${started.size} thread(s) for ${seconds}s: ${[...started.values()].join(', ')}`);
  await sleep(seconds * 1000);
  await stopProfilers(cdp, started, outdir);
}

const targets = await getJson('/json');
const page = targets.find((t) => t.type === 'page');
if (!page) {
  console.log('no page target:', JSON.stringify(targets));
  process.exit(1);
}
const cdp = await session(page.webSocketDebuggerUrl);

switch (mode) {
  case 'probe': {
    console.log(JSON.stringify(targets, null, 1));
    const v = await cdp.send('Browser.getVersion');
    console.log(JSON.stringify(v.result || v.error));
    break;
  }
  case 'targets': {
    const all = await cdp.send('Target.getTargets');
    for (const t of (all.result || {}).targetInfos || [])
      console.log(t.type, t.targetId.slice(0, 6), t.attached ? 'attached' : 'free', (t.url || '').split('/').pop());
    break;
  }
  case 'close': {
    const r = await cdp.send('Page.close');
    console.log(r.error ? 'Page.close: ' + JSON.stringify(r.error) : 'page close requested');
    break;
  }
  case 'eval': {
    const r = await cdp.send('Runtime.evaluate', {
      expression: readFileSync(arg1, 'utf8'),
      awaitPromise: true,
      returnByValue: true,
      timeout: 20000,
    });
    const out = r.error || (r.result.exceptionDetails ? { exception: r.result.exceptionDetails } : r.result.result.value);
    console.log(JSON.stringify(out, null, 1));
    break;
  }
  case 'screenshot': {
    const r = await cdp.send('Page.captureScreenshot', { format: 'png' });
    if (r.error) console.log(JSON.stringify(r.error));
    else {
      writeFileSync(arg1, Buffer.from(r.result.data, 'base64'));
      console.log('wrote', arg1);
    }
    break;
  }
  case 'console': {
    const seconds = Number(arg1 || 10);
    const filter = arg2 ? new RegExp(arg2) : null;
    const lines = [];
    const sessions = await attachWorkers(cdp, (sid) => cdp.send('Runtime.enable', {}, sid));
    const label = (m) => (m.sessionId ? workerLabel(sessions.get(m.sessionId) || { targetId: m.sessionId }) : 'main');
    cdp.on((m) => {
      if (m.method === 'Runtime.consoleAPICalled') {
        const text = m.params.args.map(fmtArg).join(' ');
        if (!filter || filter.test(text)) lines.push(`[${label(m)}] ${m.params.type}: ${text}`);
      } else if (m.method === 'Runtime.exceptionThrown') {
        lines.push(`[${label(m)}] EXCEPTION: ${m.params.exceptionDetails.text}`);
      }
    });
    await cdp.send('Runtime.enable');
    await sleep(seconds * 1000);
    console.log(`workers attached: ${sessions.size}`);
    console.log(lines.join('\n'));
    break;
  }
  case 'cpu': {
    const seconds = Number(arg1 || 10);
    const started = new Map();
    await attachWorkers(cdp, (sid, info) => startProfiler(cdp, started, sid, workerLabel(info)));
    await startProfiler(cdp, started, undefined, 'main');
    await sleep(1000);
    console.log(`profiling ${started.size} thread(s) for ${seconds}s: ${[...started.values()].join(', ')}`);
    await sleep(seconds * 1000);
    await stopProfilers(cdp, started, arg2 || 'profiles');
    break;
  }
  case 'cpu-onplay': {
    const sessions = await attachWorkers(cdp, (sid) => cdp.send('Runtime.enable', {}, sid));
    await cdp.send('Runtime.enable');
    console.log(`attached ${sessions.size} workers, waiting for Kodi to log ${PLAYBACK_STARTED}`);
    let playing = false;
    cdp.on((m) => {
      if (m.method !== 'Runtime.consoleAPICalled') return;
      const text = m.params.args.map(fmtArg).join(' ');
      if (/CVideoPlayer::OpenFile|OpenDemuxStream|Opening:/.test(text) || PLAYBACK_STARTED.test(text))
        console.log('seen:', text.trim().slice(0, 200));
      if (PLAYBACK_STARTED.test(text)) playing = true;
    });
    const deadline = Date.now() + PLAYBACK_WAIT_MS;
    while (!playing && Date.now() < deadline) await sleep(500);
    if (!playing) {
      console.log('no playback started in time');
      cdp.close();
      process.exit(1);
    }
    console.log(`playback started, settling ${PLAYBACK_SETTLE_MS / 1000}s`);
    await sleep(PLAYBACK_SETTLE_MS);
    await profileAll(cdp, Number(arg1 || 15), arg2 || 'profiles', sessions);
    break;
  }
  default: {
    const usage = [];
    for (const line of readFileSync(new URL(import.meta.url), 'utf8').split('\n')) {
      if (!line.startsWith('//')) break;
      usage.push(line);
    }
    console.log(usage.join('\n'));
  }
}

cdp.close();
process.exit(0);
