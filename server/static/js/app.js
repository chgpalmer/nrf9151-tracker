/**
 * app.js — Root entry point.
 * Handles: clock, hash-based router, device loading, page lifecycle.
 */

import { loadDevices, onDeviceChange as onDeviceChangeCb } from '/js/devices.js';
import { init as liveInit, start as liveStart, stop as liveStop, onDeviceChange as liveDeviceChange } from '/js/live.js';
import { init as histInit, onDeviceChange as histDeviceChange } from '/js/history.js';

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
const PAGES    = ['live', 'history', 'events', 'settings'];
const navItems = document.querySelectorAll('.nav-item[data-page]');
let currentPage = null;

function getPage() {
  const hash = window.location.hash.replace('#', '');
  return PAGES.includes(hash) ? hash : 'live';
}

function navigate(page) {
  if (page === currentPage) return;

  // Deactivate old page
  if (currentPage === 'live') liveStop();

  // Hide all, show target
  PAGES.forEach(p => {
    const el = document.getElementById(`page-${p}`);
    if (el) el.classList.toggle('hidden', p !== page);
  });

  // Nav active state
  navItems.forEach(n => n.classList.toggle('active', n.dataset.page === page));

  currentPage = page;

  // Activate new page
  if (page === 'live') {
    liveInit();
    // Small delay so Leaflet gets correct container dimensions
    setTimeout(() => {
      liveStart();
    }, 50);
  } else if (page === 'history') {
    histInit();
    // Invalidate map size when page becomes visible
    setTimeout(() => {
      if (window._histMapView) window._histMapView.invalidate();
    }, 50);
  }
}

window.addEventListener('hashchange', () => navigate(getPage()));

// ── Device change ────────────────────────────────────────────
onDeviceChangeCb(() => {
  liveDeviceChange();
  histDeviceChange();
});

// ── Bootstrap ────────────────────────────────────────────────
async function boot() {
  await loadDevices();
  navigate(getPage());
}

boot();
