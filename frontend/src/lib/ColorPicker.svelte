<!-- P2.10 — BRMesh-app-style colour control for the RGBW bulbs.
     Two modes, matching the wire:
       · Color  → a hue/saturation wheel (drives the RGB frame, 0x72 B R G)
       · White  → a warm→cool temperature slider, shown in KELVIN (drives the
                  separate white LED / CT frame, 0x72 t). Kelvin is the display
                  label; the wire carries WW/CW crossfade bytes (protocol.md
                  §2.5b). Range defaults 2700 K (warm) ↔ 6000 K (cool); the
                  parent may pass per-light min/max Kelvin.
     Emits `color` ({rgb:[r,g,b]}) and `ct` ({ct:kelvin}); the parent sends them.
     NOTE: no local variable may be named `state` (Svelte 5 runes). -->
<script>
  import { createEventDispatcher } from 'svelte';
  const dispatch = createEventDispatcher();

  // Props (runes mode — `$props()`, not `export let`). The destructured values
  // stay in sync with the parent's, but reassigning them here is local-only:
  // changes are reported back through the dispatched `color`/`ct` events.
  let { rgb = [255, 255, 255], ct = 4000, minKelvin = 2700, maxKelvin = 6000,
        mode = 'rgb' } = $props();

  const R = 46;            // wheel radius (px)
  const PAD = 6;           // handle overscan so it clears the wheel edge
  const C = R + PAD;       // wheel-box centre offset
  // Kelvin range the bulbs support (parent may pass per-light min/max). Warm
  // end = WW-led (minKelvin), cool end = CW-led (maxKelvin). Kelvin is a display
  // label over the WW/CW crossfade bytes on the wire (protocol.md §2.5b).
  const kelvinAt = (v) => Math.round(minKelvin + (v / 100) * (maxKelvin - minKelvin));
  const sliderAt = (k) => {
    const span = maxKelvin - minKelvin || 1;
    return Math.max(0, Math.min(100, Math.round(((k - minKelvin) / span) * 100)));
  };

  // ── colour math (HSL wheel, lightness fixed at 0.5 → white centre) ──────
  function hslToRgb(h, s, l) {
    const c = (1 - Math.abs(2 * l - 1)) * s;
    const hp = (((h % 360) + 360) % 360) / 60;
    const x = c * (1 - Math.abs((hp % 2) - 1));
    let r = 0, g = 0, b = 0;
    if (hp < 1) [r, g, b] = [c, x, 0];
    else if (hp < 2) [r, g, b] = [x, c, 0];
    else if (hp < 3) [r, g, b] = [0, c, x];
    else if (hp < 4) [r, g, b] = [0, x, c];
    else if (hp < 5) [r, g, b] = [x, 0, c];
    else [r, g, b] = [c, 0, x];
    const m = l - c / 2;
    return [
      Math.round((r + m) * 255),
      Math.round((g + m) * 255),
      Math.round((b + m) * 255),
    ];
  }
  function rgbToHsl(r, g, b) {
    r /= 255; g /= 255; b /= 255;
    const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
    let h = 0, s = 0;
    const l = (max + min) / 2;
    if (d) {
      s = d / (1 - Math.abs(2 * l - 1));
      if (max === r) h = ((g - b) / d + 6) % 6 * 60;
      else if (max === g) h = ((b - r) / d + 2) * 60;
      else h = ((r - g) / d + 4) * 60;
    }
    return [h, s];
  }

  // ── wheel handle ─────────────────────────────────────────────────────────
  let wheelEl = $state(null);
  let hx = $state(C);
  let hy = $state(C);
  let dragging = $state(false);

  $effect(() => {
    if (mode !== 'rgb' || dragging) return;
    const [h, s] = rgbToHsl(rgb[0], rgb[1], rgb[2]);
    // hue 0 = top, increasing clockwise (matches the conic-gradient), so the
    // handle lands ON the colour it represents.
    hx = C + R * s * Math.sin((h * Math.PI) / 180);
    hy = C - R * s * Math.cos((h * Math.PI) / 180);
  });

  // commit = send to the board. While dragging we only move the handle +
  // swatch (the live preview); the actual broadcast happens once, on release.
  function fromPointer(clientX, clientY, commit) {
    const rect = wheelEl.getBoundingClientRect();
    const dx = clientX - rect.left - C;
    const dy = clientY - rect.top - C;
    const dist = Math.hypot(dx, dy) || 1;
    const s = Math.min(1, dist / R);
    // hue = angle clockwise from the top — the same convention as the wheel's
    // conic-gradient (#f00 at top, increasing clockwise), so the dot sits on
    // exactly the colour it selects.
    const h = ((Math.atan2(dx, -dy) * 180) / Math.PI + 360) % 360;
    hx = C + s * R * Math.sin((h * Math.PI) / 180);
    hy = C - s * R * Math.cos((h * Math.PI) / 180);
    const [r, g, b] = hslToRgb(h, s, 0.5);
    rgb = [r, g, b]; // live preview (swatch + handle) — nothing transmitted yet
    // Each dispatch = one broadcast-only BLE command on the board AND one NVS
    // flash write ("Saved … lights to NVS"). So commit only on release, never
    // per-mousemove — dragging must not burn the flash write budget.
    if (commit) { mode = 'rgb'; dispatch('color', { rgb }); }
  }

  function onDown(e) {
    dragging = true;
    try { wheelEl.setPointerCapture?.(e.pointerId); } catch { /* capture is optional — some pointer paths throw */ }
    fromPointer(e.clientX, e.clientY, false); // preview only — no TX
  }
  function onMove(e) {
    if (dragging) fromPointer(e.clientX, e.clientY, false); // preview only — no TX
  }
  function onUp(e) {
    if (dragging) fromPointer(e.clientX, e.clientY, true); // release → exactly one TX
    dragging = false;
  }

  // ── CT (white) slider, in KELVIN ─────────────────────────────────────────
  // Warm (minKelvin, WW-led) → cool (maxKelvin, CW-led). Kelvin is the label;
  // the board maps it onto the WW/CW crossfade bytes (protocol.md §2.5b).
  let ctSlider = $state(sliderAt(ct));
  $effect(() => {
    if (mode !== 'ct') return;
    ctSlider = sliderAt(ct);
  });
  // Preview is live (bind:value updates ctSlider → the preview re-renders);
  // commit only on release, so a slider drag is one TX + one NVS write — not one
  // per input event. (Same rule as the colour wheel + the brightness slider.)
  function onCtCommit(e) {
    mode = 'ct';
    dispatch('ct', { ct: kelvinAt(+e.target.value) }); // release → exactly one TX
  }

  // A little preview of what's currently aimed at the bulb — Kelvin for white.
  let preview = $derived(
    mode === 'ct'
      ? `${kelvinAt(ctSlider)} K`
      : `rgb(${rgb[0]},${rgb[1]},${rgb[2]})`
  );
</script>

<div class="picker">
  <div class="modes">
    <button class={mode === 'rgb' ? 'on' : ''} onclick={() => (mode = 'rgb')}>Color</button>
    <button class={mode === 'ct' ? 'on' : ''} onclick={() => (mode = 'ct')}>White</button>
  </div>

  {#if mode === 'rgb'}
    <div class="wheelbox">
      <div class="wheel" bind:this={wheelEl}
           onpointerdown={onDown} onpointermove={onMove}
           onpointerup={onUp} onpointercancel={onUp}>
        <span class="handle" style="left:{hx}px; top:{hy}px"></span>
      </div>
      <span class="swatch" style="background:rgb({rgb[0]},{rgb[1]},{rgb[2]})"
            title="selected colour"></span>
    </div>
  {:else}
    <div class="ctbox">
      <input class="ct" type="range" min="0" max="100" bind:value={ctSlider}
             onchange={onCtCommit} onpointerdown={() => (mode = 'ct')} />
      <div class="ctlabels"><span>{minKelvin} K · warm</span><span>{maxKelvin} K · cool</span></div>
    </div>
  {/if}

  <span class="dim">{mode === 'rgb' ? 'drag to pick a colour' : 'drag for colour temperature'} · {preview}</span>
</div>

<style>
  .picker { display: flex; flex-direction: column; gap: 0.5rem; margin: 0.5rem 0; }
  .modes { display: flex; gap: 0.4rem; }
  .modes button {
    font: inherit; padding: 0.2rem 0.7rem; cursor: pointer;
    background: #1c2026; color: #cfd6df; border: 1px solid #333b45;
    border-radius: 6px;
  }
  .modes button.on { background: #2b6cb0; color: #fff; border-color: #2b6cb0; }

  .wheelbox { position: relative; width: calc(var(--w, 104px)); height: 104px; }
  .wheel {
    position: absolute; inset: 0; border-radius: 50%; touch-action: none;
    /* hue ring + saturation fade to white in the centre */
    background:
      radial-gradient(circle, #fff, #fff0 70%),
      conic-gradient(#f00, #ff0, #0f0, #0ff, #00f, #f0f, #f00);
    /* A conic-gradient with the default background-origin:padding-box +
       background-repeat:repeat tiles into the 2px border gap and WRAPS — the
       opposite rim (red↔cyan, purple↔green) bleeds a 2-3px sliver of the
       wrong hue through the semi-transparent border. Pin the origin to the
       border-box and disable repeat so it spans the full box without wrapping. */
    background-origin: border-box;
    background-clip: padding-box;
    background-repeat: no-repeat;
    border: 2px solid #00000033; box-shadow: 0 1px 4px #0007;
  }
  .handle {
    position: absolute; width: 16px; height: 16px; margin: -8px 0 0 -8px;
    border-radius: 50%; border: 2px solid #fff; box-shadow: 0 0 2px #000, 0 0 3px #000a;
    pointer-events: none; background: #fff;
  }
  .swatch {
    position: absolute; right: -4px; bottom: -4px; width: 20px; height: 20px;
    border-radius: 4px; border: 2px solid #0008; box-shadow: 0 0 3px #0008;
  }

  .ctbox { width: 100%; max-width: 220px; }
  .ct {
    /* padding:0 overrides the global `input,select{padding:7px 10px}` (style.css) —
       that 10px horizontal padding insets the thumb's travel and leaves a visible
       gap before each end even at value 0/100. */
    -webkit-appearance: none; appearance: none; width: 100%; height: 14px;
    padding: 0;
    border-radius: 7px; cursor: pointer; border: 1px solid #0006;
    background: linear-gradient(90deg, #ff9b3c, #fff3df 45%, #ffffff 70%, #7ab8ff);
  }
  .ct::-webkit-slider-thumb, .ct::-moz-range-thumb {
    -webkit-appearance: none; appearance: none; width: 18px; height: 18px;
    border-radius: 50%; background: #fff; border: 2px solid #333; box-shadow: 0 0 2px #0009;
  }
  .ctlabels { display: flex; justify-content: space-between; font-size: 0.7rem; color: #8892a0; margin-top: 2px; }
</style>
