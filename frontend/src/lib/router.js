// Tiny hash router (BRIDGE_PLAN §11): works from any static path, no
// server-side rewrite, safe for the LittleFS single-HTML build.
//
// Routes: #/ (Lights, the landing page) #/lights #/config #/console #/info

export function page() {
  const h = location.hash.replace(/^#\/?/, '').split('/')[0];
  return h || 'lights';
}

export function go(key) {
  location.hash = `#/${key === 'lights' ? '' : key}`;
}

// Nav order is the task order: control first, then setup, then debugging.
export const NAV = [
  ['lights', 'Lights'],
  ['config', 'Config'],
  ['console', 'Console'],
  ['info', 'Info'],
];
