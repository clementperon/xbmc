// Print the console output of the socket test app running on the TV.
//
//   node tools/wasm/tizen/socktest/capture.mjs [devtools-port] [timeout-s]
//
// Connects to the page target on the forwarded DevTools port, replays the
// console history and streams new messages until "[socktest] done" or the
// timeout.
const port = process.argv[2] || '7012';
const timeoutMs = 1000 * (parseInt(process.argv[3] || '90', 10));

const targets = await fetch(`http://localhost:${port}/json`).then((r) => r.json());
const page = targets.find((t) => t.type === 'page');
if (!page) {
  console.error('no page target on port', port);
  process.exit(1);
}
const ws = new WebSocket(page.webSocketDebuggerUrl);
let id = 0;
const send = (method, params = {}) => ws.send(JSON.stringify({ id: ++id, method, params }));

const done = new Promise((resolve) => {
  const timer = setTimeout(() => resolve('timeout'), timeoutMs);
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    let text = null;
    if (m.method === 'Runtime.consoleAPICalled') {
      text = m.params.args.map((a) => a.value ?? a.description ?? '').join(' ');
    } else if (m.method === 'Runtime.exceptionThrown') {
      text = 'EXCEPTION ' + (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
    } else if (m.method === 'Log.entryAdded') {
      text = `[${m.params.entry.level}] ${m.params.entry.text}`;
    }
    if (text !== null) {
      console.log(text);
      if (text.includes('[socktest] done')) {
        clearTimeout(timer);
        resolve('done');
      }
    }
  };
});

await new Promise((r) => (ws.onopen = r));
send('Runtime.enable');
send('Log.enable');
const outcome = await done;
console.log(`-- capture ${outcome}`);
ws.close();
