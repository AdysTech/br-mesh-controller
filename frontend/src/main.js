import { mount } from 'svelte';
import App from './App.svelte';
import './style.css';

if (typeof __SPA_VERSION__ !== 'undefined') {
  console.info(`[brmesh] SPA build ${__SPA_VERSION__}`);
  document
    .querySelector('meta[name="spa-version"]')
    ?.setAttribute('content', __SPA_VERSION__);
}

export default mount(App, { target: document.getElementById('app') });
