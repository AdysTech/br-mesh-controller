// /ws/log client — one app-wide connection (all pages share it).
//
// Message schema (BRIDGE_PLAN §6): typed JSON; the only type the Phase-2
// board emits is {"type":"log","text":...}. The server greets a new client
// with the log tail (one frame per line), so a reconnect loses nothing.
//
// Reconnect: exponential backoff 1/2/4/8 s, cap 30 s (BRIDGE_PLAN §6).

const MAX_RING = 500;

const ring = [];
const listeners = new Set();
let ws = null;
let backoff = 1000;
let open = false;

function push(text) {
  ring.push(text);
  if (ring.length > MAX_RING) ring.splice(0, ring.length - MAX_RING);
  for (const l of listeners) l(text);
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws/log`);
  ws.onopen = () => {
    open = true;
    backoff = 1000;
    for (const l of listeners) l.__open && l.__open(true);
    // The board's WS handler dispatches on a CLIENT-SENT frame (lora-relay
    // model): the hello triggers the log-tail greeting + client registration.
    try { ws.send('{"type":"hello"}'); } catch { /* closing already */ }
  };
  ws.onmessage = (e) => {
    let text = e.data;
    try {
      const m = JSON.parse(e.data);
      if (m.type === 'log' && typeof m.text === 'string') text = m.text;
      else if (m.type !== 'log') return; // other frame types: not used by the UI yet
    } catch { /* plain text line — show as-is */ }
    push(text);
  };
  ws.onclose = () => {
    open = false;
    for (const l of listeners) l.__open && l.__open(false);
    const d = backoff;
    backoff = Math.min(backoff * 2, 30000);
    setTimeout(connect, d);
  };
  ws.onerror = () => { try { ws.close(); } catch { /* already closed */ } };
}

connect();

export const logStream = {
  ring,
  isOpen: () => open,
  /** Subscribe to live lines. Returns an unsubscribe function. */
  subscribe: (cb) => {
    listeners.add(cb);
    return () => listeners.delete(cb);
  },
};
