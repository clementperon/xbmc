// Compare where the browser main thread spends its time in two captures, by
// category. A sample is charged to the first category matching any frame on
// its stack, so categories are ordered from most to least specific.
//
//   node compare_main.mjs <before/main.cpuprofile> <after/main.cpuprofile>
import { readFileSync } from 'node:fs';

const BUCKETS = [
  ['commit blit (getParameter, blitOffscreenFramebuffer, wasm_webgl_commit_frame)', /^getParameter$|^blitOffscreenFramebuffer$|^_emscripten_webgl_do_commit_frame$|^wasm_webgl_commit_frame$|^blitFramebuffer$/],
  ['socket polling (recvmsg, ___syscall_recvfrom, ___syscall_poll, ErrnoError)', /^recvmsg$|^___syscall_recvfrom$|^___syscall_poll$|^ErrnoError$|^strError$/],
  ['VideoDecoder.decode', /^decode$/],
  ['VideoFrame.copyTo', /^copyTo$/],
  ['texImage2D (VideoFrame import)', /^texImage2D$/],
  ['texSubImage2D (plane upload)', /^texSubImage2D$/],
  ['other WebGL calls', /^(bufferData|uniformMatrix4fv|bindBuffer|vertexAttribPointer|uniform1f|bindTexture|useProgram|drawElements|drawArrays|enableVertexAttribArray|disableVertexAttribArray|enable|disable|scissor|viewport|blendFunc|blendFuncSeparate|activeTexture|clear|clearColor|texParameteri|bindFramebuffer|uniform4f|uniform1i|uniform2f|uniform3f|uniform4fv|uniform1fv|uniform2fv|uniform3fv|deleteTextures|createTexture|getUniformLocation|webglGetProgramUniformLocation)$/],
  ['idle', /^\(idle\)$/],
];

const load = (file) => {
  const p = JSON.parse(readFileSync(file, 'utf8'));
  const byId = new Map(p.nodes.map((n) => [n.id, n]));
  const parent = new Map();
  for (const n of p.nodes) for (const c of n.children || []) parent.set(c, n.id);
  const totals = new Map(BUCKETS.map(([k]) => [k, 0]));
  let total = 0;
  for (let i = 0; i < p.samples.length; i++) {
    const d = p.timeDeltas[i] || 0;
    total += d;
    const names = [];
    for (let id = p.samples[i]; id !== undefined; id = parent.get(id))
      names.push((byId.get(id).callFrame.functionName || '').replace(/\(.*$/, ''));
    for (const [k, re] of BUCKETS) {
      if (names.some((n) => re.test(n))) {
        totals.set(k, totals.get(k) + d);
        break;
      }
    }
  }
  return { total, totals };
};

const [, , before, after] = process.argv;
const a = load(before);
const b = load(after);
const pct = (v, total) => ((100 * v) / total).toFixed(1).padStart(5);
console.log(`${'category'.padEnd(80)} before   after`);
for (const [k] of BUCKETS) console.log(`${k.padEnd(80)} ${pct(a.totals.get(k), a.total)}%  ${pct(b.totals.get(k), b.total)}%`);
console.log(`${'sampled seconds'.padEnd(80)} ${(a.total / 1e6).toFixed(1).padStart(5)}   ${(b.total / 1e6).toFixed(1).padStart(5)}`);
