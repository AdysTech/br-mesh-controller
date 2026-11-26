// E2E smoke — the SPA in a real (headless) browser, against the board or the
// vite dev server (E2E_APP_URL). Same setup as lora-relay's e2e: puppeteer +
// the cached chrome-headless-shell (findChrome in helpers.mjs).
//
//   node e2e/smoke.mjs                      # default: http://brmesh (the board)
//   E2E_APP_URL=http://localhost:5173 \     # …or the vite dev server
//   node e2e/smoke.mjs
//
// Checks:
//   1. page loads with NO pageerror/console error (the blank-page regression)
//   2. #app actually rendered (nav + a panel + the light card)
//   3. hash routing switches all four pages (Lights/Config/Console/Info)
//   4. one light toggle round-trip (off then on) — POST /api/light?n=1
//   5. colour + white-LED (CT) controls (P2.10) drive the board's believed
//      state end-to-end, and a refresh RESTORES it (P2.8 — no white box)
//   6. screenshots land in e2e/shots/ for the record
//
// Every board-bound action is exactly ONE request, no retries (the bulbs are
// broadcast-only; the toggle is the only off/on pair, one request each).

import puppeteer from 'puppeteer';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  APP_URL, test, section, assert, ok, req, summary, exitWith, findChrome,
  waitForBoard,
} from './helpers.mjs';

const SHOT_DIR = path.join(path.dirname(fileURLToPath(import.meta.url)), 'shots');
fs.mkdirSync(SHOT_DIR, { recursive: true });

let browser;
let page;

const pageErrors = [];
const consoleErrors = [];
const shot = (name) => page.screenshot({ path: path.join(SHOT_DIR, name) });

try {
  const chrome = findChrome();
  browser = await puppeteer.launch({
    executablePath: chrome,
    headless: 'new',
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-dev-shm-usage'],
  });
  page = await browser.newPage();
  page.on('pageerror', (e) => pageErrors.push(String(e)));
  page.on('console', (m) => { if (m.type() === 'error') consoleErrors.push(m.text()); });

  // ── board reachable? ──────────────────────────────────────────────────
  let status;
  try {
    const r = await req('GET', APP_URL + '/api/status');
    status = await ok(r);
  } catch (e) {
    console.error(`FATAL: board unreachable at ${APP_URL} — ${e.message}`);
    process.exit(2);
  }
  console.log(`▸ target ${APP_URL} — fw=${status.fw} spa=${status.spa} lights=${status.lights}`);

  // ── 1+2: load, no errors, rendered ───────────────────────────────────
  await test('page loads with no page/console errors', async () => {
    const res = await page.goto(APP_URL, { waitUntil: 'networkidle2', timeout: 20000 });
    assert(res.ok(), `HTTP ${res.status()}`);
    // give effects (fetch/paint) a beat before asserting
    await new Promise((r) => setTimeout(r, 500));
    assert(pageErrors.length === 0, `pageerror: ${pageErrors[0]}`);
    const real = consoleErrors.filter((t) => !/favicon|404/.test(t));
    assert(real.length === 0, `console error: ${real[0]}`);
  });

  await test('#app rendered (nav + light card)', async () => {
    const app = await page.$eval('#app', (el) => el.textContent || '');
    assert(app.replace(/\s+/g, '').length > 40, `#app nearly empty: "${app.slice(0, 80)}"`);
    const navs = await page.$$eval('nav a', (a) => a.map((x) => x.textContent));
    assert(navs.length === 4, `nav links: ${JSON.stringify(navs)}`);
    if (status.lights > 0) {
      await page.waitForSelector('.light', { timeout: 5000 });
      const head = await page.$eval('.light .name', (el) => el.textContent);
      assert(head.length > 0, 'light card name empty');
    }
  });

  // ── 3: hash routing — every page renders ─────────────────────────────
  const PAGES = [
    ['lights', 'Discovery'],
    ['config', 'Device'],
    ['console', 'board log'],
    ['info', 'Build'],
  ];
  for (const [key, marker] of PAGES) {
    await test(`page #/${key} renders`, async () => {
      if (key !== 'lights') await page.goto(`${APP_URL}/#/${key}`, { waitUntil: 'networkidle2' });
      else await page.goto(APP_URL, { waitUntil: 'networkidle2' });
      await new Promise((r) => setTimeout(r, 400));
      const txt = await page.$eval('#app', (el) => el.textContent || '');
      assert(txt.toLowerCase().includes(marker.toLowerCase()),
             `marker "${marker}" not found — blank page? (${txt.slice(0, 60).trim()})`);
      assert(pageErrors.length === 0, `pageerror on ${key}: ${pageErrors[pageErrors.length - 1]}`);
      await shot(`0${PAGES.findIndex(([k]) => k === key) + 1}-${key}.png`);
    });
  }

  // ── 4: one light toggle round-trip (off → on) ────────────────────────
  if (status.lights > 0) {
    await section('light toggle (exactly two state changes, one request each)');
    await test('POST /api/light?n=1 off+on round-trip via the UI', async () => {
      await page.goto(APP_URL, { waitUntil: 'networkidle2' });
      const sw = await page.waitForSelector('.light .switch', { timeout: 5000 });
      await sw.click();                       // off — one request
      await new Promise((r) => setTimeout(r, 800));
      assert(pageErrors.length === 0, `pageerror after off: ${pageErrors[pageErrors.length - 1]}`);
      const errText = await page.$eval('#app', (el) => el.textContent || '');
      assert(!/missing light number|no such light/i.test(errText), 'board rejected the ?n= op');
      await page.waitForSelector('.light .switch', { timeout: 5000 });
      await (await page.$('.light .switch')).click(); // on — back to the original state
      await new Promise((r) => setTimeout(r, 800));
    });
  }

  // ── 5: colour + white-LED (CT) controls, and believed-state restore ─────
  // Exercises the P2.10 controls against the board (not a stub): one pointer
  // action on each control, then assert the board's OWN believed_state moved —
  // the full round trip UI → POST /api/light?n → controller → GET /api/lights.
  // These need a registered (n≥1) bulb, so they run only in that case.
  if (status.lights > 0) {
    await section('colour + white-LED (CT) controls → believed_state (P2.10 + P2.8)');

    await test('color wheel: one drag → board believes the new RGB', async () => {
      await page.goto(APP_URL, { waitUntil: 'networkidle2' });
      // Force the FIRST light into Color mode (mirrors the CT test forcing White)
      // so the first .wheel in the DOM belongs to the first light — otherwise, if
      // the first light happens to be in CT mode, the first wheel is a DIFFERENT
      // light's and the assert below (on the first light) would read its stale CT
      // state. The click on "Color" only reveals the wheel; it sends no command.
      const cbtns = await page.$$('.picker .modes button');
      let cbtn = null;
      for (const b of cbtns)
        if ((await page.evaluate((el) => el.textContent.trim(), b)) === 'Color') { cbtn = b; break; }
      if (cbtn) { await cbtn.click(); await new Promise((r) => setTimeout(r, 300)); }
      const wheel = await page.waitForSelector('.light .wheel', { timeout: 5000 });
      const box = await wheel.boundingBox();
      assert(box, 'colour wheel not visible');
      // Top-centre at the rim → hue 0 (red) at near-full saturation → red-ish.
      // The wheel is a clockwise hue ring with RED at 12 o'clock (right edge is
      // green, hue 90). A single mouse.click fires the pointerdown/pointerup
      // the wheel listens for, so this commits (one TX) and reads the result.
      await page.mouse.click(box.x + box.width / 2, box.y + 8);
      await new Promise((r) => setTimeout(r, 800));
      assert(pageErrors.length === 0, `pageerror after wheel: ${pageErrors[pageErrors.length - 1]}`);
      const l1 = (await ok(await req('GET', APP_URL + '/api/lights'))).find((l) => l.n >= 1);
      const bs = l1 && l1.believed_state;
      assert(bs && bs.mode === 'rgb', `believed mode should be rgb, got ${bs && bs.mode}`);
      assert(bs.rgb && bs.rgb[0] > 200 && bs.rgb[1] < 80 && bs.rgb[2] < 80,
             `expected a red-ish rgb, got [${bs && bs.rgb}]`);
    });

    await test('white/CT slider: release → board believes CT (white LED)', async () => {
      const btns = await page.$$('.picker .modes button');
      let target = null;
      for (const b of btns)
        if ((await page.evaluate((el) => el.textContent.trim(), b)) === 'White') { target = b; break; }
      assert(target, 'White (CT) mode button not found');
      await target.click();
      await new Promise((r) => setTimeout(r, 300));
      await page.$eval('.light .ct', (el) => {
        el.value = 50;                                   // mid slider
        // `input` is live preview only (no TX); the single commit fires on
        // `change` (release) — drive a real release so exactly one POST goes out.
        el.dispatchEvent(new Event('input', { bubbles: true }));
        el.dispatchEvent(new Event('change', { bubbles: true }));
      });
      await new Promise((r) => setTimeout(r, 800));
      assert(pageErrors.length === 0, `pageerror after ct: ${pageErrors[pageErrors.length - 1]}`);
      const l1 = (await ok(await req('GET', APP_URL + '/api/lights'))).find((l) => l.n >= 1);
      const bs = l1 && l1.believed_state;
      assert(bs && bs.mode === 'ct', `believed mode should be ct, got ${bs && bs.mode}`);
      // UI slider at 50% → kelvinAt(50) = 2700 + 0.5·(6000−2700) = 4350 K. The
      // board must round-trip a Kelvin value near the mid of the bulbs' [2700,
      // 6000] range 
      assert(Number.isFinite(bs.ct) && bs.ct >= 3800 && bs.ct <= 4900,
             `mid-slider ct should be ~4350 K, got ${bs && bs.ct}`);
    });

    await test('refresh RESTORES believed state (White/CT active, no white box)', async () => {
      await page.goto(APP_URL, { waitUntil: 'networkidle2' });
      await new Promise((r) => setTimeout(r, 500));
      // The card seeds from believed_state → the White/CT control is active.
      const onMode = await page.$eval('.picker .modes button.on',
                                      (el) => el.textContent.trim()).catch(() => '');
      assert(/white/i.test(onMode),
             `after refresh the White/CT control should be active, got "${onMode}"`);
    });
  }

  // ── 6: backup → mutate → restore round-trip ────────────────────────────
  // GET /api/backup, sanity-check the snapshot, mutate the board (rename the
  // FIRST light — a live NVS write, no air), then POST /api/restore (reboots)
  // and assert the mutation is REVERTED. Guards the whole portable
  // backup/restore path end-to-end, incl. the reboot + NVS reload + HA
  // re-publish. Uses the first light's ACTUAL number (not a literal 1 — a
  // forgotten bulb can leave the list starting at 2).
  //
  // Pure-API test (no browser needed). Release the SPA's app-global /ws/log
  // WebSocket + its HTTP keep-alives first: the board caps at
  // max_open_sockets=12, and holding the browser's sockets open while Node
  // fires its own requests intermittently exhausts the pool → "fetch failed".
  await shot('99-final.png');   // final SPA screenshot while the page is live
  await page.close();

  if (status.lights > 0) {
    await section('backup → mutate → restore round-trip (reboots the board)');

    // Find a backup entry's name by light NUMBER (not vector index — a forgotten
    // light can make l0_num != 0).
    const backupNameFor = (bk, n) => {
      for (let i = 0; i < (bk.ln || 0); i++)
        if (bk[`l${i}_num`] === n) return bk[`l${i}_nm`];
      return null;
    };

    let backup = null;
    let targetN = null;

    await test('GET /api/backup returns a valid snapshot', async () => {
      const b = await ok(await req('GET', APP_URL + '/api/backup'));
      assert(/^[0-9a-fA-F]{8}$/.test(b.key || ''), `bad mesh key: "${b.key}"`);
      assert(typeof b.ln === 'number' && b.ln >= 1, `backup ln should be ≥1, got ${b.ln}`);
      assert(typeof b.s_ssid === 'string' && b.s_ssid.length > 0, 'backup missing s_ssid');
      backup = b;
    });

    await test('backup entry for the first light is well-formed (name + mac)', async () => {
      const lights = await ok(await req('GET', APP_URL + '/api/lights'));
      const first = lights.find((l) => l.n >= 1);
      assert(first, 'no live light found');
      targetN = first.n;
      const i = Object.keys(backup).find((k) => k.endsWith('_num') && backup[k] === targetN);
      assert(i, `backup has no entry with number ${targetN}`);
      // keep the "l0_" prefix (strip only the "num" field) so l0_num -> l0_nm / l0_mac
      const prefix = i.slice(0, -'num'.length);
      const name = backup[`${prefix}nm`];
      const mac = backup[`${prefix}mac`];
      assert(typeof name === 'string' && name.length > 0, `light name empty: "${name}"`);
      assert(/^[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}$/.test(mac || ''), `light mac malformed: "${mac}"`);
    });

    await test('mutate: rename the first light (to be reverted by restore)', async () => {
      const marker = 'e2e-restore-check';
      await ok(await req('PATCH', `${APP_URL}/api/light?n=${targetN}`, { name: marker }));
      const lights = await ok(await req('GET', APP_URL + '/api/lights'));
      const l = lights.find((x) => x.n === targetN);
      assert(l.name === marker, `rename did not take: "${l.name}"`);
    });

    await test('POST /api/restore reboots + reverts the rename', async () => {
      const before = backupNameFor(backup, targetN);
      await ok(await req('POST', APP_URL + '/api/restore', backup));
      await waitForBoard();
      const lights = await ok(await req('GET', APP_URL + '/api/lights'));
      const l = lights.find((x) => x.n === targetN);
      assert(l.name === before, `restore did not revert rename: "${l.name}" != "${before}"`);
    });
  }
} finally {
  if (browser) await browser.close().catch(() => {});
}

exitWith(summary('brmesh e2e'));
