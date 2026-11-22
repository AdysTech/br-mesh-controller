<script>
  import { page, NAV } from './lib/router.js';
  import { api } from './lib/api.js';
  import { logStream } from './lib/ws.js';
  import LightsPage from './pages/LightsPage.svelte';
  import ConfigPage from './pages/ConfigPage.svelte';
  import ConsolePage from './pages/ConsolePage.svelte';
  import InfoPage from './pages/InfoPage.svelte';

  const PAGES = {
    lights: LightsPage,
    config: ConfigPage,
    console: ConsolePage,
    info: InfoPage,
  };

  let dev = $state('brmesh');
  let staleSpa = $state(false);

  // WS open/close dot (all pages share the single /ws/log connection; the
  // __open callback is how ws.js reports it).
  let wsOpen = $state(false);
  const wsState = () => {};
  wsState.__open = (o) => { wsOpen = o; };
  logStream.subscribe(wsState);

  $effect(() => {
    let alive = true;
    async function refreshMeta() {
      try {
        const s = await api.status();
        if (!alive) return;
        dev = s.name || 'brmesh';
        staleSpa =
          typeof __SPA_VERSION__ !== 'undefined' &&
          !!s.spa && s.spa !== __SPA_VERSION__;
      } catch { /* board offline — the dot / pages will show it */ }
    }
    refreshMeta();
    const t = setInterval(refreshMeta, 15000);
    return () => { alive = false; clearInterval(t); };
  });

  // Hash routing: a hashchange listener flips reactive state so Svelte
  // re-renders the active page (a bare location.hash read is not tracked).
  let route = $state(page());
  $effect(() => {
    const onHash = () => { route = page(); };
    window.addEventListener('hashchange', onHash);
    return () => window.removeEventListener('hashchange', onHash);
  });

  const current = $derived(PAGES[route] || LightsPage);
</script>

<div class="topbar">
  <h1>BRMesh</h1>
  <span class="dev">{dev}</span>
  {#if staleSpa}<span class="dev">⚠ SPA build ≠ board build</span>{/if}
  <span class="dotlabel">live</span>
  <span class="dot" class:off={!wsOpen}></span>
</div>

<nav>
  {#each NAV as [key, label] (key)}
    <a href="#/{key === 'lights' ? '' : key}" class:active={route === key}>
      {label}
    </a>
  {/each}
</nav>

<svelte:component this={current} />
