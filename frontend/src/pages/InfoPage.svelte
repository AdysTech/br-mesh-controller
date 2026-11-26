<!-- Board + build info, the stale-SPA check (baked SPA version vs the
     board's served build tag from /api/status "spa"), and WS health. -->
<script>
  import { api } from '../lib/api.js';
  import { logStream } from '../lib/ws.js';

  let s = $state(null);
  let open = $state(false);
  let err = $state('');

  const stateCb = () => {};
  stateCb.__open = (o) => { open = o; };
  logStream.subscribe(stateCb);

  const baked = typeof __SPA_VERSION__ !== 'undefined' ? __SPA_VERSION__ : '(dev)';
  const matches = $derived(!!s && !!s.spa && s.spa === baked);

  $effect(() => {
    let alive = true;
    async function load() {
      try { if (alive) { s = await api.status(); err = ''; } }
      catch (e) { if (alive) err = String(e); }
    }
    load();
    const t = setInterval(load, 10000);
    return () => { alive = false; clearInterval(t); };
  });

  function uptime(sec) {
    if (!sec && sec !== 0) return '—';
    const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), ss = sec % 60;
    return (h ? h + 'h ' : '') + m + 'm ' + ss + 's';
  }
  function kb(v) {
    return (v || v === 0) ? v + ' KB' : '—';
  }
</script>

<div class="panel">
  <h2>Build</h2>
  {#if s}
    <table class="kv">
      <tbody>
      <tr><td>firmware</td><td class="mono">{s.fw}</td></tr>
      <tr><td>SPA on board</td><td class="mono">{s.spa || '—'}</td></tr>
      <tr>
        <td>SPA in this browser</td>
        <td class="mono">{baked}</td>
      </tr>
      <tr>
        <td>build match</td>
        <td>
          {#if matches}<span class="badge ok">match</span>
          {:else}<span class="badge bad">mismatch — reflash the FS ({`pio run -t uploadfs`}) or the app</span>{/if}
        </td>
      </tr>
    </tbody>
    </table>
  {:else}
    <div class="dim">{err || 'loading…'}</div>
  {/if}
</div>

{#if s}
  <div class="panel">
    <h2>Board</h2>
    <table class="kv">
      <tbody>
      <tr><td>board</td><td>{s.board}</td></tr>
      <tr><td>name</td><td class="mono">{s.name}</td></tr>
      <tr><td>IP</td><td class="mono">{s.ip}</td></tr>
      <tr><td>network</td><td>{s.ssid} · {s.rssi} dBm</td></tr>
      <tr><td>lights registered</td><td>{s.lights}</td></tr>
      {#if s.ctrl_id}
        <tr>
          <td>MQTT bridge</td>
          <td>
            {#if s.mqtt}<span class="badge ok">online</span>
            {:else if s.mqtt_enabled}<span class="badge mid">connecting…</span>
            {:else}<span class="badge bad">disabled</span>{/if}
            <span class="mono dim">{s.ctrl_id}</span>
          </td>
        </tr>
      {/if}
      <tr>
        <td>scanning</td>
        <td>{#if s.scanning}<span class="badge mid">active</span>{:else}idle{/if}</td>
      </tr>
      <tr><td>uptime</td><td>{uptime(s.uptime_s)}</td></tr>
      <tr><td>free heap</td><td>{kb(s.free_heap_kb)} <span class="dim">(min-seen {kb(s.min_free_heap_kb)})</span></td></tr>
      <tr><td>largest free block</td><td>{kb(s.largest_free_internal_block_kb)} <span class="dim">(internal)</span></td></tr>
      <tr><td>PSRAM</td><td>{#if s.psram_size_kb}{kb(s.psram_free_kb)} free / {kb(s.psram_size_kb)} total{:else}none{/if}</td></tr>
    </tbody>
    </table>
  </div>

  <div class="panel">
    <h2>Live log link</h2>
    <table class="kv">
      <tbody>
      <tr>
        <td>websocket /ws/log</td>
        <td>
          {#if open}<span class="badge ok">open</span>
          {:else}<span class="badge mid">reconnecting…</span>{/if}
        </td>
      </tr>
      <tr><td>ring buffer</td><td>{logStream.ring.length} lines kept client-side</td></tr>
    </tbody>
    </table>
  </div>
{/if}

<div class="panel">
  <h2>Verification</h2>
  <div class="dim">
    from the host: <span class="mono">curl http://brmesh/api/status</span> — the
    "spa" field is the build tag this page compares against. DevTools shows the
    running tag in <span class="mono">&lt;meta name="spa-version"&gt;</span>.
  </div>
</div>
