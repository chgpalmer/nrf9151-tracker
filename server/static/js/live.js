/**
 * live.js — Live view: auto-refreshing track with time-window selector
 */

import { fetchPositions } from '/js/api.js';
import { createMapView }  from '/js/mapview.js';
import { createFixTable } from '/js/fixtable.js';
import { updateCharts, initCharts } from '/js/charts.js';
import { open as openDrawer } from '/js/drawer.js';
import { currentDevice, setStatus } from '/js/devices.js';
import { deviceStatus } from '/js/format.js';

const REFRESH_MS = 5000;

let mapView      = null;
let fixTable     = null;
let timer        = null;
let minuteWindow = 60;   // default: 1 hour
let currentFixes = [];
let isInitialized = false;

// Controls
const pills = document.querySelectorAll('#time-window .pill');
const accToggle   = document.getElementById('show-accuracy');
const arrowToggle = document.getElementById('show-arrows');
const gpsChip     = document.getElementById('filter-gps');
const cellChip    = document.getElementById('filter-cell');

pills.forEach(p => {
  p.addEventListener('click', () => {
    pills.forEach(x => x.classList.remove('active'));
    p.classList.add('active');
    minuteWindow = parseInt(p.dataset.minutes, 10);
    refresh(true);
  });
});

accToggle.addEventListener('change', () => {
  mapView && mapView.setShowAccuracy(accToggle.checked);
});

arrowToggle.addEventListener('change', () => {
  mapView && mapView.setShowArrows(arrowToggle.checked);
});

// Source filter chips: re-render from the cached fixes, no refetch.
[gpsChip, cellChip].forEach(chip => {
  chip.addEventListener('click', () => {
    chip.classList.toggle('active');
    chip.setAttribute('aria-pressed', String(chip.classList.contains('active')));
    renderView(false);
  });
});

function visibleFixes() {
  const gps  = gpsChip.classList.contains('active');
  const cell = cellChip.classList.contains('active');
  return currentFixes.filter(f => (f.source === 'gps' ? gps : cell));
}

export function init() {
  if (isInitialized) return;
  isInitialized = true;

  mapView = createMapView('map-live', fix => openDrawer(fix));
  fixTable = createFixTable('fix-table-live', {
    onRowClick: fix => { mapView.focusFix(fix); openDrawer(fix); },
  });
  initCharts();

  // Overlay badges
  const mapCont = document.getElementById('map-live');
  mapCont.style.position = 'relative';

  const badge = document.createElement('div');
  badge.className = 'map-badge';
  badge.id = 'live-map-badge';
  badge.innerHTML = `
    <span class="map-badge-item"><span class="map-badge-dot"></span><span class="map-badge-count" id="badge-gps">0</span> GPS</span>
    <span class="map-badge-item"><span class="map-badge-diamond"></span><span class="map-badge-count" id="badge-cell">0</span> Cell</span>
  `;
  mapCont.appendChild(badge);

  const tick = document.createElement('div');
  tick.className = 'refresh-tick';
  tick.id = 'refresh-tick';
  tick.innerHTML = `<div class="refresh-tick-dot"></div><span id="tick-label">refreshing…</span>`;
  mapCont.appendChild(tick);
}

export function start() {
  stop();
  refresh(true);
  timer = setInterval(() => refresh(false), REFRESH_MS);
}

export function stop() {
  clearInterval(timer);
  timer = null;
}

// Render map + table + charts from the cached fixes through the source filter.
function renderView(fitBounds) {
  const vis = visibleFixes();
  const filtered = vis.length === 0 && currentFixes.length > 0;

  mapView.render(vis, {
    fitBounds:    fitBounds,
    showAccuracy: accToggle.checked,
    showArrows:   arrowToggle.checked,
    liveDot:      true,
    emptyMsg:     filtered ? 'All fixes hidden by source filter' : undefined,
    emptySub:     filtered ? 'Re-enable GPS or CELL above.' : undefined,
  });
  fixTable.render(vis);
  updateCharts(vis);
}

async function refresh(fitBounds) {
  const deviceId = currentDevice();
  if (!deviceId) return;

  showTick('refreshing…');

  try {
    const opts = minuteWindow > 0
      ? { since: minuteWindow }
      : {};

    const fixes = await fetchPositions(deviceId, opts);
    currentFixes = fixes;

    renderView(fitBounds);
    updateBadge(fixes);

    // Update status based on latest fix time
    if (fixes.length > 0) {
      const latest = fixes[fixes.length - 1];
      const status = deviceStatus(latest.received_ts);
      setStatus(status);
    }

    showTick('updated');
    setTimeout(() => hideTick(), 1800);

  } catch (e) {
    console.error('Live refresh error:', e);
    showTick('error');
    setTimeout(() => hideTick(), 3000);
  }
}

function updateBadge(fixes) {
  const gps  = fixes.filter(f => f.source === 'gps').length;
  const cell = fixes.filter(f => f.source === 'cell').length;
  const gEl  = document.getElementById('badge-gps');
  const cEl  = document.getElementById('badge-cell');
  if (gEl) gEl.textContent = gps;
  if (cEl) cEl.textContent = cell;
}

function showTick(msg) {
  const tick  = document.getElementById('refresh-tick');
  const label = document.getElementById('tick-label');
  if (tick)  tick.classList.add('show');
  if (label) label.textContent = msg;
}

function hideTick() {
  const tick = document.getElementById('refresh-tick');
  if (tick) tick.classList.remove('show');
}

// External trigger when device changes
export function onDeviceChange() {
  // If the live page is currently active (timer running or page visible), restart
  const livePage = document.getElementById('page-live');
  const isVisible = livePage && !livePage.classList.contains('hidden');
  if (isVisible) {
    start();
  }
}
