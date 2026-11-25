<!-- Device config (NVS 'brmesh' namespace), BLE re-provisioning, and the
     factory reset (wipes the brmesh NVS namespace only — WiFi creds stay,
     so the board is still reachable). -->
<script>
  import { api } from '../lib/api.js';

  let cfg = $state({
    name: 'brmesh', ssid: '', pass: '',
    mhost: '', mport: 1883, muser: '', mpass: '',
    mclient: '', mname: '',
  });
  let mclientNow = $state('');
  let ctrlId = $state('');
  let passMasked = $state(false);
  let mpassMasked = $state(false);
  let busy = $state(false);
  let backupBusy = $state(false);
  let restoring = $state(false);
  let note = $state('');
  let err = $state('');

  async function load() {
    try {
      const c = await api.config();
      cfg = {
        name: c.name ?? 'brmesh',
        ssid: c.ssid ?? '',
        pass: c.pass ?? '',
        mhost: c.mhost ?? '',
        mport: c.mport ?? 1883,
        muser: c.muser ?? '',
        mpass: c.mpass ?? '',
        mclient: c.mclient ?? '',
        mname: c.mname ?? '',
      };
      mclientNow = c.mqtt_client ?? '';
      ctrlId = c.ctrl_id ?? '';
      passMasked = cfg.pass === '****';
      mpassMasked = cfg.mpass === '****';
      err = '';
    } catch (e) { err = String(e); }
  }
  $effect(() => { load(); });

  // focusing a masked secret field clears the mask so a real value can go in
  function unmask(field) {
    if (field === 'pass' && passMasked) { cfg.pass = ''; passMasked = false; }
    if (field === 'mpass' && mpassMasked) { cfg.mpass = ''; mpassMasked = false; }
  }

  async function save() {
    busy = true; err = note = '';
    try {
      const out = { ...cfg };
      // masked secrets sent back unchanged would clear them — restore the
      // mask unless the operator explicitly edited (mask gone) or cleared it
      if (passMasked) out.pass = '****';
      if (mpassMasked) out.mpass = '****';
      const r = await api.saveConfig(out);
      note = r.reboot_needed
        ? 'saved — reboot the board to apply the new WiFi / MQTT broker credentials'
        : 'saved';
      if (r.reboot_needed) await api.status().catch(() => {});
    } catch (e) { err = String(e); }
    busy = false;
  }

  async function provision() {
    if (!confirm('Reboot the board into Improv-BLE provisioning?\nClears saved WiFi creds; you then connect the BLE app (Improv) and enter a network. The board is unreachable over WiFi during this.')) return;
    busy = true; err = note = '';
    try {
      await api.provision();
      note = 'board is rebooting into Improv BLE provisioning — pair it with your phone now';
    } catch (e) { err = String(e); }
    busy = false;
  }

  async function factoryReset() {
    if (!confirm('FACTORY RESET: deletes the saved key + light list (brmesh NVS namespace).\nWiFi credentials are kept so the board stays reachable.\n\nThe bulbs keep their key — they will be re-keyed on the next scan.')) return;
    busy = true; err = note = '';
    try {
      await api.factoryReset();
      note = 'reset done — board is rebooting with a fresh light list';
    } catch (e) { err = String(e); }
    busy = false;
  }

  // ---- backup / restore ----------------------------------------------------
  let fileInput;   // bind:this target for the hidden restore file picker

  async function downloadBackup() {
    if (backupBusy || restoring) return;
    backupBusy = true; err = '';
    try {
      const snap = await api.backup();
      const blob = new Blob([JSON.stringify(snap, null, 2)], { type: 'application/json' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `brmesh-backup-${cfg.name || 'board'}.json`;
      document.body.appendChild(a);
      a.click();
      a.remove();
      URL.revokeObjectURL(url);
      note = `backup downloaded — ${snap.ln ?? 0} light(s) + mesh key + settings`;
    } catch (e) { err = String(e); }
    backupBusy = false;
  }

  async function onRestoreFile(ev) {
    const file = ev.target.files?.[0];
    ev.target.value = '';   // let the same file be re-selected
    if (!file) return;
    let snap;
    try { snap = JSON.parse(await file.text()); }
    catch (e) { err = 'backup file is not valid JSON: ' + e; return; }
    if (!snap || !/^[0-9a-fA-F]{8}$/.test(snap.key || '') || typeof snap.ln !== 'number') {
      err = 'that is not a brmesh backup (missing key / ln)';
      return;
    }
    if (!confirm(`RESTORE from backup:\noverwrite settings + mesh key + ${snap.ln} light(s) and reboot?\n\nIf the backup's WiFi differs, the board comes back on THAT network, not this one.`)) return;
    restoring = true; err = note = '';
    try {
      await api.restore(snap);
      note = 'restore sent — board is rebooting with the restored state';
    } catch (e) { err = String(e); }
    restoring = false;
  }
</script>

<div class="panel">
  <h2>Device</h2>
  <div class="row" style="align-items:flex-start">
    <label class="f" style="flex:1">name (DHCP hostname)
      <input bind:value={cfg.name} maxlength="15" placeholder="brmesh" />
    </label>
    <label class="f" style="flex:1">WiFi SSID
      <input bind:value={cfg.ssid} placeholder="network" />
    </label>
  </div>
  <div class="row" style="align-items:flex-start">
    <label class="f" style="flex:1">WiFi password {passMasked ? '(set — unchanged on save)' : ''}
      <input type="password" bind:value={cfg.pass} onfocus={() => unmask('pass')}
             placeholder={passMasked ? '••••' : 'password'} />
    </label>
  </div>
  <div class="note info" style="margin:0 0 10px">
    changing the network credentials reboots the board — it will reappear on the
    new network, not this one.
  </div>
  <button class="primary" disabled={busy} onclick={save}>save device config</button>
</div>

<div class="panel">
  <h2>MQTT (Home Assistant, Phase 3)</h2>
  <div class="row" style="align-items:flex-start">
    <label class="f" style="flex:2">host
      <input bind:value={cfg.mhost} placeholder="192.168.21.1" />
    </label>
    <label class="f" style="flex:0 0 6rem">port
      <input type="number" bind:value={cfg.mport} />
    </label>
  </div>
  <div class="row" style="align-items:flex-start">
    <label class="f" style="flex:1">user
      <input bind:value={cfg.muser} placeholder="(none)" />
    </label>
    <label class="f" style="flex:1">password {mpassMasked ? '(set — unchanged on save)' : ''}
      <input type="password" bind:value={cfg.mpass} onfocus={() => unmask('mpass')}
             placeholder={mpassMasked ? '••••' : '(none)'} />
    </label>
  </div>
  <div class="row" style="align-items:flex-start">
    <label class="f" style="flex:1">client id (what the broker sees)
      <input bind:value={cfg.mclient} placeholder={mclientNow || 'brmesh-<mac>'} />
    </label>
  </div>
  <div class="row" style="align-items:flex-start">
    <label class="f" style="flex:1">HA device name (what HA shows)
      <input bind:value={cfg.mname} placeholder="(empty → uses device name above)" />
    </label>
  </div>
  <div class="note info" style="margin:0 0 10px">
    client id = what the MQTT broker logs (applies on next reboot). the HA device
    name = what Home Assistant shows. they may be the same value, but are separate.
  </div>
  {#if ctrlId}
    <div class="note info" style="margin:0 0 10px">
      in Home Assistant this controller appears as
      <span class="mono">{cfg.mname || cfg.name}</span>
      (device id <span class="mono">{ctrlId}</span>); each bulb is
      <span class="mono">{ctrlId}_l&lt;n&gt;</span>. topics live under
      <span class="mono">brmesh/{ctrlId}/…</span>.
    </div>
  {/if}
  <button disabled={busy} onclick={save}>save MQTT</button>
</div>

<div class="panel">
  <h2>Maintenance</h2>
  <div class="row">
    <button class="primary" disabled={busy} onclick={provision}>
      provision via BLE (Improv)
    </button>
    <span class="dim">forgets WiFi and reboots into BLE pairing</span>
  </div>
  <div class="row" style="margin-top:10px">
    <button class="danger" disabled={busy} onclick={factoryReset}>
      factory reset (key + light list)
    </button>
    <span class="dim">keeps WiFi so the board stays reachable</span>
  </div>
</div>

<div class="panel">
  <h2>Backup &amp; restore</h2>
  <div class="note info" style="margin:0 0 10px">
    a full snapshot of this board — device config, WiFi + MQTT credentials, the
    mesh key, and every registered light with its last state. restore overwrites
    all of it and reboots. keep the file somewhere safe.
  </div>
  <div class="row">
    <button class="primary" disabled={backupBusy || restoring} onclick={downloadBackup}>
      download backup
    </button>
    <span class="dim">settings + mesh key + lights → a .json file</span>
  </div>
  <div class="row" style="margin-top:10px">
    <button class="danger" disabled={backupBusy || restoring}
            onclick={() => fileInput?.click()}>
      restore from file…
    </button>
    <input type="file" accept="application/json,.json" bind:this={fileInput}
           style="display:none" onchange={onRestoreFile} />
    <span class="dim">applies a backup and reboots</span>
  </div>
</div>

{#if note}<div class="note ok">{note}</div>{/if}
{#if err}<div class="note err">{err}</div>{/if}
