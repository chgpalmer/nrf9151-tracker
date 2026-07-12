/**
 * app.js — Root entry point.
 * Handles: hash-based router, device loading, page lifecycle.
 */

import { loadDevices, onDeviceChange as onDeviceChangeCb } from '/js/devices.js';
import { start as mapStart, stop as mapStop, onDeviceChange as mapDeviceChange } from '/js/map.js';
import { start as logsStart, stop as logsStop, onDeviceChange as logsDeviceChange } from '/js/logs.js';
import { start as usageStart, stop as usageStop, onDeviceChange as usageDeviceChange } from '/js/usage.js';
import { init as settingsInit } from '/js/settings.js';

// ── Router ───────────────────────────────────────────────────
const PAGES    = ['map', 'logs', 'usage', 'events', 'settings'];
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
  if (currentPage === 'usage') usageStop();

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
  } else if (page === 'usage') {
    usageStart();
  }
}

window.addEventListener('hashchange', () => navigate(getPage()));

// ── Device change ────────────────────────────────────────────
onDeviceChangeCb(() => {
  mapDeviceChange();
  logsDeviceChange();
  usageDeviceChange();
});

// ── Bootstrap ────────────────────────────────────────────────
async function boot() {
  await loadDevices();
  settingsInit();
  navigate(getPage());
}

boot();
