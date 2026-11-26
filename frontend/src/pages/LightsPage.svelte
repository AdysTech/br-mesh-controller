<!-- Landing page: control first (on/off · brightness · colour/CT), rename,
     delete, and the discovery/scan section (P2.4 folded in per the plan).
     P2.8 — the server now returns each bulb's *believed* state (last intent
     we broadcast, persisted across reboots). The card seeds its optimistic
     copy from it on load, so a refresh restores the real colour instead of
     a white box. (Note: no variable may be named `state` — Svelte 5 runes.) -->
<script>
  import { api } from '../lib/api.js';
  import { logStream } from '../lib/ws.js';
  import ColorPicker from '../lib/ColorPicker.svelte';

  let lights = $state([]);
  let local = $state({}); // n -> {on, brightness, rgb:[r,g,b], ct, mode}
  let err = $state('');

  // Only registered (numbered, n≥1) lights are addressable via /api/light?n=.
  // Detected-but-unkeyed bulbs (n=0) are not yet controllable — hide them from
  // the control list (they surface here once keying succeeds). Filtering them
  // also avoids the duplicate each-key: several unkeyed bulbs all carry n=0.
  // De-dupe by MAC too: a re-key can momentarily register the same bulb twice
  // (we hit #1 and #4 for the same 70:e7), and two entries sharing a key break
  // the each block (svelte.dev/e/each_key_duplicate → blank page).
  const controllable = $derived(
    (() => {
      const seen = new Set();
      return lights.filter((l) => {
        if (!l.n || seen.has(l.mac)) return false;
        seen.add(l.mac);
        return true;
      });
    })()
  );
  let note = $state('');
  let renaming = $state(0); // n being renamed, 0 = none
  let renameText = $state('');

  // scan section. The window is the board's CONFIGURED value (Config → Discovery)
  // — the SAME duration the HA "Start scan" button uses. This button just fires
  // the scan; there's no per-scan duration override here.
  let scanning = $state(false);
  let scanBusy = $state(false);
  let liveTail = $state([]);

  // Seed one card's optimistic copy from the server's believed state (P2.8).
  function seed(l) {
    const b = l.believed_state;
    if (!b) return { on: true, brightness: 127, rgb: [255, 255, 255], ct: 4000, mode: 'white' };
    return {
      on: !!b.on,
      brightness: b.brightness ?? 127,
      rgb: Array.isArray(b.rgb) ? b.rgb.slice() : [255, 255, 255],
      ct: b.ct ?? 4000,
      mode: b.mode || 'white',
    };
  }

  // Read-only: called from the template. Any local-state init happens in
  // load() — Svelte 5 throws on $state mutation during render.
  function st(l) {
    return local[l.n] || seed(l);
  }

  async function load() {
    try {
      lights = await api.lights();
      for (const l of lights)
        if (!local[l.n]) local[l.n] = seed(l);
      err = '';
    } catch (e) { err = String(e); }
  }

  async function toggle(l, on) {
    const s = st(l);
    const prev = s.on;
    s.on = on; // optimistic — the bulbs are broadcast-only, we are the truth
    err = note = '';
    try { await api.light(l.n, { on }); }
    catch (e) { s.on = prev; err = String(e); }
  }

  async function setBrightness(l, v) {
    st(l).brightness = v;
    err = note = '';
    try { await api.light(l.n, { brightness: v }); }
    catch (e) { err = String(e); }
  }

  async function setColor(l, rgb) {
    st(l).rgb = rgb; st(l).mode = 'rgb';
    err = note = '';
    try { await api.light(l.n, { rgb }); }
    catch (e) { err = String(e); }
  }

  async function setCt(l, ct) {
    st(l).ct = ct; st(l).mode = 'ct';
    err = note = '';
    try { await api.light(l.n, { ct }); }
    catch (e) { err = String(e); }
  }

  function beginRename(l) { renaming = l.n; renameText = l.name; }
  async function commitRename(l) {
    const name = renameText.trim();
    renaming = 0;
    if (!name || name === l.name) return;
    err = '';
    try { await api.rename(l.n, name); note = `renamed to “${name}”`; load(); }
    catch (e) { err = String(e); }
  }

  // Soft duplicate-name hint (WARN, never block): while renaming bulb l, if the
  // typed name already belongs to another controllable bulb, return that bulb's
  // number so the template can say "name already used by bulb #N". We surface
  // the collision but never block the commit — HA keeps the two distinct by the
  // slot-number unique_id (…_l<N>), the name is only the friendly label. 0 = none.
  function nameClash(l) {
    const t = renameText.trim().toLowerCase();
    if (!t) return 0;
    for (const o of controllable)
      if (o.n !== l.n && o.name.trim().toLowerCase() === t) return o.n;
    return 0;
  }

  async function remove(l) {
    if (!confirm(`Forget light #${l.n} “${l.name}”?\nIt is only removed from THIS controller's list; the bulb keeps its key and will be re-keyed if scanned again.`)) return;
    err = note = '';
    try { await api.del(l.n); delete local[l.n]; load(); }
    catch (e) { err = String(e); }
  }

  // ── scan ───────────────────────────────────────────────────────────────
  async function startScan() {
    scanBusy = true; err = note = '';
    try {
      await api.scan();
      note = 'scan queued — power-cycle any NEW bulb during the window to catch it';
      refreshScan();
    } catch (e) { err = String(e); }
    scanBusy = false;
  }
  async function stopScan() {
    scanBusy = true;
    try { await api.scanStop(); note = 'scan stopped'; }
    catch (e) { err = String(e); }
    scanBusy = false;
  }

  async function refreshScan() {
    try {
      const s = await api.status();
      const was = scanning;
      scanning = !!s.scanning;
      if (was && !scanning) { load(); note = 'scan finished — list refreshed'; }
      liveTail = logStream.ring.slice(-5);
    } catch { /* board gone — dot shows it */ }
  }

  // P2.7 idle-poll fix — no unconditional poll. The WS logStream already keeps
  // the live tail fresh; we only hit /api/status while a scan is actually
  // running, so we can catch it finishing and refresh the light list. Idle
  // (scanning=false) means zero /api/status traffic.
  $effect(() => {
    load();          // on mount: load the list once
    refreshScan();   // one-shot sync — catches a scan started from the console
  });
  $effect(() => {
    if (!scanning) return;               // idle → no timer at all
    const t = setInterval(refreshScan, 1000);   // scanning → track it to done
    return () => clearInterval(t);
  });
</script>

{#each controllable as l (l.mac + ':' + l.n)}
  {@const s = st(l)}
  <div class="light">
    <div class="head">
      {#if renaming === l.n}
        {@const clash = nameClash(l)}
        <span class="name">
          <input value={renameText} oninput={(e) => (renameText = e.target.value)}
                 onkeyup={(e) => e.key === 'Enter' && commitRename(l)}
                 onblur={() => commitRename(l)} autofocus />
          <button class="tiny" onclick={() => (renaming = 0)}>cancel</button>
        </span>
        {#if clash}<span class="nameclash">name already used by bulb #{clash}</span>{/if}
      {:else}
        <span class="name">#{l.n} · {l.name}</span>
      {/if}
      <button class="switch" class:on={s.on} role="switch" aria-checked={s.on}
              title={s.on ? 'turn off' : 'turn on'}
              onclick={() => toggle(l, !s.on)}></button>
      <span class="meta" style="margin-left:auto">
        {l.mac} · type {l.type}
      </span>
      <button class="tiny" onclick={() => beginRename(l)}>rename</button>
      <button class="tiny danger" onclick={() => remove(l)}>forget</button>
    </div>

    <div class="slider-row">
      <span class="dim">bright</span>
      <input type="range" min="0" max="127"
             onchange={(e) => setBrightness(l, +e.target.value)}
             bind:value={s.brightness} />
      <span class="val">{s.brightness}</span>
    </div>

    <div class="color-row">
      <span class="dim">color</span>
      <ColorPicker rgb={s.rgb} ct={s.ct}
                   minKelvin={l.min_kelvin ?? 2700} maxKelvin={l.max_kelvin ?? 6000}
                   mode={s.mode === 'ct' ? 'ct' : 'rgb'}
                   on:color={(e) => setColor(l, e.detail.rgb)}
                   on:ct={(e) => setCt(l, e.detail.ct)} />
    </div>
  </div>
{/each}

{#if controllable.length === 0 && !err}
  <div class="note info">
    No lights yet — start a scan below and power-cycle a fresh bulb.
    {#if lights.length > controllable.length}
      <span class="dim">({lights.length - controllable.length} detected, still keying…)</span>
    {/if}
  </div>
{/if}

<div class="panel">
  <h2>Discovery — key a new bulb</h2>
  <div class="row">
    <button class="primary" disabled={scanBusy || scanning} onclick={startScan}>
      {scanning ? 'scanning…' : 'Scan now'}
    </button>
    {#if scanning}<button disabled={scanBusy} onclick={stopScan}>stop</button>{/if}
    <span class="dim">
      uses the configured window (Config → Discovery) — the same duration the HA
      "Start scan" button uses. Fresh bulbs only advertise after power-up —
      cycle the bulb while scanning.
    </span>
  </div>
  {#if scanning}
    <div class="note info">
      scanning… tail of the board log:
      <pre class="console" style="height:auto;margin:6px 0 0;max-height:9rem">
        {liveTail.join('\n') || '(quiet)'}
      </pre>
    </div>
  {/if}
</div>

{#if note}<div class="note ok">{note}</div>{/if}
{#if err}<div class="note err">{err}</div>{/if}
