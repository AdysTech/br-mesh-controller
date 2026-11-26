// Shared helpers for the E2E suite (smoke.mjs).
// Modeled on the proven lora-relay e2e harness (same shape, self-contained):
// tiny test runner + chrome discovery for the cached headless shell.
// Node 18+, no external deps (Node has global fetch).

import { execSync } from 'node:child_process';
import path from 'node:path';
import os from 'node:os';

// ── Config (all overridable via env) ────────────────────────────────────────
// E2E_APP_URL — where the SPA is served: the board itself (default) or the
// vite dev server (http://localhost:5173) while iterating on pages.
export const APP_URL = process.env.E2E_APP_URL || 'http://brmesh';

// ── Tiny test harness (lora-relay shape) ────────────────────────────────────
let _pass = 0, _fail = 0, _failures = [];

export async function test(name, fn) {
  try {
    await fn();
    _pass++;
    console.log(`  ✓ ${name}`);
  } catch (e) {
    _fail++;
    _failures.push({ name, error: e && (e.stack || e.message) || String(e) });
    console.log(`  ✗ ${name}`);
    console.log(`      ${e && e.message ? e.message : e}`);
  }
}

export function section(title) { console.log(`\n▸ ${title}`); }

export function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}

/** Expect a request to succeed (2xx) and return its parsed body. */
export async function ok(res) {
  if (res.status < 200 || res.status >= 300) {
    throw new Error(`expected 2xx, got ${res.status}: ${(res.body && res.body.error) || res.text.slice(0, 120)}`);
  }
  return res.body;
}

/** fetch wrapper that returns {status, body, text} without throwing. */
export async function req(method, url, body) {
  const res = await fetch(url, {
    method,
    headers: body !== undefined ? { 'Content-Type': 'application/json' } : undefined,
    body: body !== undefined ? JSON.stringify(body) : undefined,
    cache: 'no-store',
    signal: AbortSignal.timeout(10000),
  });
  const text = await res.text();
  let json = null;
  try { json = JSON.parse(text); } catch { /* not JSON */ }
  return { status: res.status, body: json, text };
}

export function summary(title = 'E2E') {
  console.log(`\n▸ ${title}: ${_pass} passed, ${_fail} failed`);
  return _fail === 0;
}

// Wait for the board to come back after a reboot (restore reboots it). Polls
// /api/status until it 2xx's or the deadline passes. Local device only — this
// is a boot-wait, not a request retry.
export async function waitForBoard(timeoutMs = 45000) {
  const deadline = Date.now() + timeoutMs;
  let last = 'no attempt yet';
  while (Date.now() < deadline) {
    try {
      const r = await fetch(APP_URL + '/api/status', { cache: 'no-store', signal: AbortSignal.timeout(4000) });
      if (r.status >= 200 && r.status < 300) {
        return await r.json();
      }
      last = `HTTP ${r.status}`;
    } catch (e) { last = e.message; }
    await new Promise((r) => setTimeout(r, 1000));
  }
  throw new Error(`board did not come back within ${timeoutMs}ms (last: ${last})`);
}

export function exitWith(okAll) { process.exit(okAll ? 0 : 1); }

// ── Chrome discovery (lora-relay shape: CHROME_PATH or the puppeteer cache) ──
export function findChrome() {
  if (process.env.CHROME_PATH) return process.env.CHROME_PATH;
  const base = path.join(os.homedir(), '.cache', 'puppeteer', 'chrome-headless-shell');
  try {
    const p = execSync(`find ${JSON.stringify(base)} -name chrome-headless-shell -type f 2>/dev/null | head -1`, { encoding: 'utf8' })
      .trim();
    return p || undefined;
  } catch {
    return undefined;
  }
}
