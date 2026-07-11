/**
 * app.js — Root entry point.
 * Handles: clock, hash-based router, device loading, page lifecycle.
 */

import { loadDevices, onDeviceChange as onDeviceChangeCb } from '/js/devices.js';
import { start as mapStart, stop as mapStop, onDeviceChange as mapDeviceChange } from '/js/map.js';
import { start as logsStart, stop as logsStop, onDeviceChange as logsDeviceChange } from '/js/logs.js';

// ── Clock ───────────────────────────────────────────────────
const clockEl = document.getElementById('clock');
function tickClock() {
  clockEl.textContent = new Date().toLocaleTimeString([], {
    hour: '2-digit', minute: '2-digit', second: '2-digit',
  });
}
tickClock();
setInterval(tickClock, 1000);

// ── Router ───────────────────────────────────────────────────
const PAGES    = ['map', 'logs', 'events', 'settings'];
const navItems = document.querySelectorAll('.nav-item[data-page]');
let currentPage = null;

function getPage() {
  const hash = window.location.hash.replace('#', '');
  return PAGES.includes(hash) ? hash : 'map';
}

function navigate(page) {
  if (page === currentPage) return;

  if (currentPage === 'map') mapStop();
  if (currentPage === 'logs') logsStop();

  PAGES.forEach(p => {
    const el = document.getElementById(`page-${p}`);
    if (el) el.classList.toggle('hidden', p !== page);
  });

  navItems.forEach(n => n.classList.toggle('active', n.dataset.page === page));

  currentPage = page;

  if (page === 'map') {
    // Small delay so Leaflet gets correct container dimensions.
    setTimeout(() => {
      mapStart();
      if (window._mapView) window._mapView.invalidate();
    }, 50);
  } else if (page === 'logs') {
    logsStart();
  }
}

window.addEventListener('hashchange', () => navigate(getPage()));

// ── Device change ────────────────────────────────────────────
onDeviceChangeCb(() => {
  mapDeviceChange();
  logsDeviceChange();
});

// ── Bootstrap ────────────────────────────────────────────────
async function boot() {
  await loadDevices();
  navigate(getPage());
}

boot();
