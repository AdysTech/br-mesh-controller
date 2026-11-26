// Tiny typed wrapper over the board's REST API (BRIDGE_PLAN.md §6).
//
// esp_http_server has no path params, so per-light ops use ?n=<number>.
// Masked-credential convention (GET/POST /api/config): the board returns
// "****" for a stored secret; send "****" back to leave it unchanged, "" to
// clear it.

async function j(url, opts = {}) {
  const r = await fetch(url, { cache: 'no-store', ...opts });
  let body = null;
  try { body = await r.json(); } catch { /* empty body (e.g. on reboot) */ }
  if (!r.ok) throw new Error((body && body.error) || `${r.status} ${r.statusText}`);
  return body;
}

const json = (o) => ({
  method: 'POST',
  headers: { 'content-type': 'application/json' },
  body: JSON.stringify(o),
});

export const api = {
  status: () => j('/api/status'),
  lights: () => j('/api/lights'),

  scan: () => j('/api/scan', json({})),
  scanStop: () => j('/api/scan/stop'),
  raw: (sec, mode = 'active') => j('/api/raw', json({ sec, mode })),

  light: (n, op) => j(`/api/light?n=${n}`, json(op)),
  rename: (n, name) =>
    j(`/api/light?n=${n}`, {
      method: 'PATCH',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ name }),
    }),
  del: (n) => j(`/api/light?n=${n}`, { method: 'DELETE' }),

  config: () => j('/api/config'),
  saveConfig: (cfg) => j('/api/config', json(cfg)),
  provision: () => j('/api/provision', { method: 'POST' }),
  factoryReset: () => j('/api/factory-reset', { method: 'POST' }),

  // Portable backup/restore: the board's full state (settings + raw mesh key +
  // light list) as a flat JSON snapshot. backup() GETs it (for download/e2e);
  // restore() POSTs a snapshot back (the board reboots to apply it).
  backup: () => j('/api/backup'),
  restore: (snapshot) => j('/api/restore', json(snapshot)),
};
