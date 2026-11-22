import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { viteSingleFile } from 'vite-plugin-singlefile';

// ESP32 backend IP for the dev proxy (the board on VLAN 40).
// Override: ESP32_IP=192.168.x.x npm run dev
const ESP32_IP = process.env.ESP32_IP || '192.168.40.51';

// SPA build tag — tools/build_spa.py exports SPA_VERSION (from
// src/FirmwareVersion.h) when it runs `npm run build`; baked in below as
// __SPA_VERSION__ (stale-SPA check against the board's /api/status "spa").
export default defineConfig({
  define: {
    __SPA_VERSION__: JSON.stringify(process.env.SPA_VERSION || 'dev')
  },
  plugins: [
    svelte({
      onwarn(warning, handler) {
        if (warning.code && (
          warning.code.startsWith('a11y') ||
          warning.code === 'css_unused_selector'
        )) return;
        if (!handler) return;
        handler(warning);
      }
    }),
    // Inline JS/CSS into the single HTML blob — the device serves exactly one
    // file (index.html.gz) from LittleFS, no other assets.
    viteSingleFile()
  ],
  build: {
    outDir: '../html',
    emptyOutDir: true
  },
  // Dev server: proxy /api + /ws to the board so the SPA can be debugged locally.
  server: {
    port: 5173,
    proxy: {
      '/api': { target: `http://${ESP32_IP}`, changeOrigin: true },
      '/ws': { target: `ws://${ESP32_IP}`, ws: true }
    }
  }
});
