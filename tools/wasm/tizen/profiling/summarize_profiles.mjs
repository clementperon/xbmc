// Summarise every .cpuprofile in a directory, one block per thread: wall and
// active time (samples outside idle and futex frames), the top self and
// inclusive functions of the active part, and what the thread was blocked on
// while in emscripten_futex_wait. Threads idle throughout are listed in one line.
//
//   node summarize_profiles.mjs <dir> [topSelf=12] [topInclusive=14]
import { readdirSync, readFileSync } from 'node:fs';

const [, , dir, topSelfArg, topInclArg] = process.argv;
const topSelf = Number(topSelfArg || 12);
const topIncl = Number(topInclArg || 14);

const IDLE =
  /^emscripten_futex_wait$|^\(program\)$|^\(idle\)$|^\(garbage collector\)$|^__timedwait|^__pthread_cond_timedwait|^emscripten_thread_sleep|^__clock_nanosleep|^nanosleep/;
// Frames of Emscripten's dispatch machinery, hidden so the caller shows instead.
const BORING =
  /__thread_proxy|js-to-wasm|invokeEntryPoint|handleMessage|^callUserCallback|^runIter|^MainLoop_runner|^checkMailbox|^em_task_queue_execute|^receive_notification|^_emscripten_check_mailbox|^wasm-to-js$|^\s*$/;
const WAIT_MACHINERY =
  /futex|__timedwait|pthread_cond|proxy|_do_call|call_with_ctx|run_js_func|em_task|emscripten_sync_run|^__syscall|^wasm-to-js|^\(/;

const name = (n) => {
  const f = n.callFrame;
  const fn = (f.functionName || '(anonymous)').replace(/^([^(]+)\(.*$/, '$1').replace(/^non-virtual thunk to /, '');
  return fn.length > 90 ? fn.slice(0, 87) + '...' : fn;
};

const pct = (v, total) => ((100 * v) / total).toFixed(1).padStart(5);
const ms = (v) => (v / 1e3).toFixed(0).padStart(5);
const top = (map, n) => [...map.entries()].sort((a, b) => b[1] - a[1]).slice(0, n);

const idle = [];
for (const file of readdirSync(dir).filter((f) => f.endsWith('.cpuprofile')).sort()) {
  const profile = JSON.parse(readFileSync(`${dir}/${file}`, 'utf8'));
  const byId = new Map(profile.nodes.map((n) => [n.id, n]));
  const parent = new Map();
  for (const n of profile.nodes) for (const c of n.children || []) parent.set(c, n.id);

  const self = new Map();
  const incl = new Map();
  const blocked = new Map();
  let total = 0;
  let active = 0;
  let blockedTotal = 0;
  for (let i = 0; i < profile.samples.length; i++) {
    const d = profile.timeDeltas[i] || 0;
    total += d;
    const leaf = byId.get(profile.samples[i]);
    if (name(leaf) === 'emscripten_futex_wait') {
      blockedTotal += d;
      const seen = new Set();
      for (let id = leaf.id; id !== undefined; id = parent.get(id)) {
        const nm = name(byId.get(id));
        if (seen.has(nm) || BORING.test(nm) || WAIT_MACHINERY.test(nm)) continue;
        seen.add(nm);
        blocked.set(nm, (blocked.get(nm) || 0) + d);
      }
    }
    if (IDLE.test(name(leaf))) continue;
    active += d;
    self.set(name(leaf), (self.get(name(leaf)) || 0) + d);
    const seen = new Set();
    for (let id = leaf.id; id !== undefined; id = parent.get(id)) {
      const nm = name(byId.get(id));
      if (seen.has(nm) || BORING.test(nm)) continue;
      seen.add(nm);
      incl.set(nm, (incl.get(nm) || 0) + d);
    }
  }

  const label = file.replace('.cpuprofile', '');
  if (active < 0.01 * total) {
    idle.push(`${label} (${(active / 1e3).toFixed(0)} ms active)`);
    continue;
  }
  console.log(`\n=== ${label}: ${(total / 1e6).toFixed(1)} s wall, ${(active / 1e6).toFixed(2)} s active (${pct(active, total).trim()}%)`);
  console.log('  self:');
  for (const [k, v] of top(self, topSelf)) console.log(`  ${pct(v, active)}%  ${ms(v)} ms  ${k}`);
  console.log('  inclusive:');
  for (const [k, v] of top(incl, topIncl)) console.log(`  ${pct(v, active)}%  ${ms(v)} ms  ${k}`);
  if (blockedTotal > 0.01 * total) {
    console.log(`  blocked in futex_wait ${(blockedTotal / 1e6).toFixed(1)} s, from:`);
    for (const [k, v] of top(blocked, 8)) console.log(`  ${pct(v, blockedTotal)}%  ${ms(v)} ms  ${k}`);
  }
}
if (idle.length) console.log(`\nidle threads: ${idle.join(', ')}`);
