<!-- Live board log (WS /ws/log — same tail the serial console prints) plus
     the raw frame-capture trigger (P2.6). The board's console command set
     stays serial; this page is the read-only mirror + air-capture tools. -->
<script>
  import { api } from '../lib/api.js';
  import { logStream } from '../lib/ws.js';

  let lines = $state([]);
  let open = $state(false);
  let pinned = $state(false); // auto-scroll unless the user scrolled up
  let box; // bound via bind:this — the live log container

  let rawSec = $state(5);
  // "active" | "passive" | "wake" — default active (plain capture), matching the
  // API default and the old checkbox-off behaviour. Wake is an opt-in.
  let rawMode = $state('active');
  let busy = $state(false);
  let note = $state('');
  let err = $state('');

  const MODE_HINT = {
    active:  'plain capture — every frame on the air',
    passive: 'listen-only scan (no probe requests)',
    wake:    'WAKE broadcast + time-sliced capture (identity frames only appear while waking)',
  };
  let modeHint = $derived(MODE_HINT[rawMode] || '');

  const cb = (text) => {
    lines = [...lines.slice(-499), text];
    if (pinned && box.current) requestAnimationFrame(() => {
      box.current.scrollTop = box.current.scrollHeight;
    });
  };
  const stateCb = () => {};
  stateCb.__open = (o) => { open = o; };
  const unsub = logStream.subscribe(cb);
  logStream.subscribe(stateCb);
  lines = [...logStream.ring]; // seed with whatever the ring already holds

  $effect(() => {
    const cleanup = () => unsub();
    return cleanup;
  });

  function onScroll() {
    const el = box.current;
    if (!el) return;
    pinned = el.scrollHeight - el.scrollTop - el.clientHeight < 40;
  }

  async function startRaw() {
    busy = true; err = note = '';
    try {
      const res = await api.raw(rawSec, rawMode);
      const sec = (res && res.sec) || rawSec;
      const cap = (res && res.capture) || rawMode;
      note = `capture queued: ${sec}s (${cap}) — watch the log below; hit stop to end early`;
    } catch (e) { err = String(e); }
    busy = false;
  }

  async function stop() {
    busy = true;
    try { await api.scanStop(); note = 'stopped'; }
    catch (e) { err = String(e); }
    busy = false;
  }

  function clear() {
    lines = [];
  }
</script>

<div class="panel">
  <h2>Raw air capture {open ? '(live)' : '(WS reconnecting…)'}</h2>
  <div class="row">
    <label class="f" style="width:auto">seconds
      <input type="number" min="1" max="120" style="width:6rem" bind:value={rawSec} />
    </label>
    <label class="f" style="width:auto; min-width:10rem">mode
      <select bind:value={rawMode}>
        <option value="active">active</option>
        <option value="passive">passive</option>
        <option value="wake">wake</option>
      </select>
    </label>
    <button class="primary" disabled={busy} onclick={startRaw}>start capture</button>
    <button disabled={busy} onclick={stop}>stop</button>
  </div>
  <div class="dim" style="margin-top:6px">{modeHint}</div>
</div>

<div class="panel">
  <div class="row spread" style="margin-bottom:8px">
    <h2 style="margin:0">board log — live</h2>
    <button class="tiny" onclick={clear}>clear</button>
  </div>
  <div class="console" bind:this={box} onscroll={onScroll}>
    {#each lines as ln, i (i + ln)}
      <div class="ln" class:error={/error|fail/i.test(ln)} class:warn={/warn/i.test(ln)}>{ln}</div>
    {:else}
      <div class="ln dim">waiting for lines…</div>
    {/each}
  </div>
</div>

{#if note}<div class="note ok">{note}</div>{/if}
{#if err}<div class="note err">{err}</div>{/if}
