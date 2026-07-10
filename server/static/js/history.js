/**
 * history.js — History view.
 *
 * Model: load the whole selected day once, then filter with a draggable
 * timeline window (like trimming a video clip). Dragging re-filters the
 * already-loaded fixes client-side, so the map updates instantly with no
 * per-drag server calls. Fix-density ticks on the timeline show where data
 * exists so you aim straight at it.
 *
 * Selecting a window:
 *   - WINDOW pills set the width (15m/1h/6h/Day) keeping the current center.
 *   - Click anywhere on the track to center the window there.
 *   - Drag the selection band to slide the whole window.
 *   - Drag a handle to trim one edge (this sets a custom width).
 *   - Arrow keys nudge a focused handle 5 min; Shift+arrows 1 min.
 * The map re-fits on release, not during the drag, so it doesn't jump around
 * while you scrub.
 */

import { fetchPositions }  from '/js/api.js';
import { createMapView }   from '/js/mapview.js';
import { createFixTable }  from '/js/fixtable.js';
import { open as openDrawer } from '/js/drawer.js';
import { currentDevice }   from '/js/devices.js';
import { fmtAcc, mpsToKph } from '/js/format.js';

const DAY      = 86400;
const MIN_FRAC = 0.005;  // ~7 min: minimum window the handles can trim to

let mapView       = null;
let fixTable      = null;
let isInitialized = false;

let dayStart   = 0;      // epoch of 00:00 for the selected local day
let dayFixes   = [];     // all fixes for the day (chronological)
let selFrac    = [0, 1]; // [start, end] window as fractions of the day
let dragging   = null;   // 'start' | 'end' | 'move' | null
let grabOffset = 0;      // pointer-to-window-start offset for band drags

const dateInput = document.getElementById('hist-date');
const rangeEl   = document.getElementById('hist-range');
const summaryEl = document.getElementById('hist-summary');
const accToggle = document.getElementById('show-accuracy-hist');
const winPills  = document.querySelectorAll('#hist-window .pill');
const gpsChip   = document.getElementById('filter-gps-hist');
const cellChip  = document.getElementById('filter-cell-hist');

const track    = document.getElementById('tl-track');
const densityEl = document.getElementById('tl-density');
const selEl    = document.getElementById('tl-sel');
const startH   = document.getElementById('tl-start');
const endH     = document.getElementById('tl-end');
const axisEl   = document.getElementById('tl-axis');

dateInput.value = new Date().toISOString().slice(0, 10);

dateInput.addEventListener('change', loadDay);
accToggle.addEventListener('change', () => {
  mapView && mapView.setShowAccuracy(accToggle.checked);
});

[gpsChip, cellChip].forEach(chip => {
  chip.addEventListener('click', () => {
    chip.classList.toggle('active');
    chip.setAttribute('aria-pressed', String(chip.classList.contains('active')));
    applyWindow();
  });
});

// ── window width pills ──────────────────────────────────────
winPills.forEach(p => {
  p.addEventListener('click', () => {
    const w = Math.min(1, parseInt(p.dataset.minutes, 10) * 60 / DAY);
    setWindow(center() - w / 2, w);
    setActivePill(p);
    applyWindow();
  });
});

function center() { return (selFrac[0] + selFrac[1]) / 2; }

function setWindow(start, width) {
  const s = Math.min(Math.max(0, start), 1 - width);
  selFrac = [s, s + width];
}

function setActivePill(pill) {
  winPills.forEach(x => x.classList.toggle('active', x === pill));
}

// A handle drag sets a custom width, so no pill matches any more.
function clearActivePill() {
  winPills.forEach(x => x.classList.remove('active'));
}

// ── timeline drag ───────────────────────────────────────────
function fracFromEvent(clientX) {
  const r = track.getBoundingClientRect();
  return Math.min(1, Math.max(0, (clientX - r.left) / r.width));
}

function onPointerMove(e) {
  if (!dragging) return;
  const f = fracFromEvent(e.clientX);
  if (dragging === 'start')     { selFrac[0] = Math.min(f, selFrac[1] - MIN_FRAC); clearActivePill(); }
  else if (dragging === 'end')  { selFrac[1] = Math.max(f, selFrac[0] + MIN_FRAC); clearActivePill(); }
  else /* move */               { setWindow(f - grabOffset, selFrac[1] - selFrac[0]); }
  applyWindow(false);   // no map re-fit mid-drag — it would jump around
}

function endDrag() {
  if (!dragging) return;
  dragging = null;
  applyWindow();        // settle: re-fit the map to the final window
}

startH.addEventListener('pointerdown', (e) => { dragging = 'start'; startH.setPointerCapture(e.pointerId); });
endH.addEventListener('pointerdown',   (e) => { dragging = 'end';   endH.setPointerCapture(e.pointerId); });
selEl.addEventListener('pointerdown',  (e) => {
  dragging = 'move';
  grabOffset = fracFromEvent(e.clientX) - selFrac[0];
  selEl.setPointerCapture(e.pointerId);
});
track.addEventListener('pointermove', onPointerMove);
window.addEventListener('pointerup', endDrag);

// Click an empty part of the track: center the current window there.
track.addEventListener('pointerdown', (e) => {
  if (e.target === startH || e.target === endH || e.target === selEl) return;
  setWindow(fracFromEvent(e.clientX) - (selFrac[1] - selFrac[0]) / 2,
            selFrac[1] - selFrac[0]);
  applyWindow();
});

// keyboard: arrows nudge a focused handle by 5 minutes (1 min with Shift)
function nudge(which, deltaFrac) {
  if (which === 'start') selFrac[0] = Math.min(Math.max(0, selFrac[0] + deltaFrac), selFrac[1] - MIN_FRAC);
  else                   selFrac[1] = Math.max(Math.min(1, selFrac[1] + deltaFrac), selFrac[0] + MIN_FRAC);
  clearActivePill();
  applyWindow();
}
startH.addEventListener('keydown', (e) => keyNudge(e, 'start'));
endH.addEventListener('keydown',   (e) => keyNudge(e, 'end'));
function keyNudge(e, which) {
  const step = (e.shiftKey ? 60 : 300) / DAY;
  if (e.key === 'ArrowLeft')  { nudge(which, -step); e.preventDefault(); }
  if (e.key === 'ArrowRight') { nudge(which,  step); e.preventDefault(); }
}

export function init() {
  if (isInitialized) return;
  isInitialized = true;
  mapView = createMapView('map-history', fix => openDrawer(fix));
  fixTable = createFixTable('fix-table-history', {
    onRowClick: fix => { mapView.focusFix(fix); openDrawer(fix); },
    startOpen: true,
  });
  window._histMapView = mapView;
  buildAxis();
  loadDay();
}

export function onDeviceChange() {
  if (isInitialized) loadDay();
}

// Fetch the whole selected local day, reset the window to full, render.
async function loadDay() {
  const deviceId = currentDevice();
  if (!deviceId || !dateInput.value) {
    dayFixes = [];
    summaryEl.innerHTML = `<span class="hist-hint">Select a device and date.</span>`;
    return;
  }

  dayStart = Math.floor(new Date(`${dateInput.value}T00:00:00`).getTime() / 1000);
  const dayEnd = dayStart + DAY;

  try {
    dayFixes = await fetchPositions(deviceId, { from_ts: dayStart, to_ts: dayEnd });
  } catch (e) {
    console.error('History load error:', e);
    summaryEl.innerHTML = `<span class="hist-hint err">Failed to load: ${e.message}</span>`;
    return;
  }

  selFrac = [0, 1];
  setActivePill(document.querySelector('#hist-window .pill[data-minutes="1440"]'));
  renderDensity();
  applyWindow();
}

// Filter dayFixes to the current window + source filter, render everything.
function applyWindow(fit = true) {
  const from_ts = dayStart + Math.round(selFrac[0] * DAY);
  const to_ts   = dayStart + Math.round(selFrac[1] * DAY);

  // position handles + selection band
  startH.style.left = (selFrac[0] * 100) + '%';
  endH.style.left   = (selFrac[1] * 100) + '%';
  selEl.style.left  = (selFrac[0] * 100) + '%';
  selEl.style.width = ((selFrac[1] - selFrac[0]) * 100) + '%';
  rangeEl.textContent = `${hhmm(from_ts)} – ${hhmm(to_ts)} · ${durLabel(to_ts - from_ts)}`;

  const gps  = gpsChip.classList.contains('active');
  const cell = cellChip.classList.contains('active');
  const win = dayFixes.filter(f =>
    f.received_ts >= from_ts && f.received_ts <= to_ts &&
    (f.source === 'gps' ? gps : cell));

  const filtered = win.length === 0 && dayFixes.length > 0 && !(gps && cell);
  mapView.render(win, {
    fitBounds: fit,
    showAccuracy: accToggle.checked,
    showArrows: true,
    emptyMsg: filtered ? 'All fixes hidden by source filter' : undefined,
    emptySub: filtered ? 'Re-enable GPS or CELL above.' : undefined,
  });
  fixTable.render(win);
  renderSummary(win);
}

// Density ticks: one bar per fix, positioned by time-of-day.
function renderDensity() {
  densityEl.innerHTML = '';
  for (const f of dayFixes) {
    const frac = (f.received_ts - dayStart) / DAY;
    if (frac < 0 || frac > 1) continue;
    const tick = document.createElement('div');
    tick.className = 'tl-tick ' + (f.source === 'cell' ? 'tl-tick-cell' : 'tl-tick-gps');
    tick.style.left = (frac * 100) + '%';
    densityEl.appendChild(tick);
  }
}

function buildAxis() {
  axisEl.innerHTML = '';
  for (let h = 0; h <= 24; h += 6) {
    const m = document.createElement('span');
    m.className = 'tl-axis-mark';
    m.style.left = (h / 24 * 100) + '%';
    m.textContent = String(h).padStart(2, '0') + ':00';
    axisEl.appendChild(m);
  }
}

function hhmm(ts) {
  return new Date(ts * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function durLabel(secs) {
  const m = Math.round(secs / 60);
  if (m < 60) return `${m}m`;
  const h = Math.floor(m / 60), rm = m % 60;
  return rm > 0 ? `${h}h ${String(rm).padStart(2, '0')}m` : `${h}h`;
}

function renderSummary(fixes) {
  if (fixes.length === 0) {
    summaryEl.innerHTML = `<span class="hist-hint">No fixes in this window.</span>`;
    return;
  }
  const gps  = fixes.filter(f => f.source === 'gps').length;
  const cell = fixes.filter(f => f.source === 'cell').length;
  const moving = fixes.filter(f => f.source === 'gps' && f.spd != null);
  const maxSpd = moving.length ? Math.max(...moving.map(f => f.spd)) : null;
  const avgAcc = fixes.reduce((s, f) => s + f.acc, 0) / fixes.length;
  const spanMin = Math.round((fixes[fixes.length - 1].received_ts - fixes[0].received_ts) / 60);

  const stat = (l, v) => `<div class="hist-stat"><span class="hist-stat-label">${l}</span><span class="hist-stat-val">${v}</span></div>`;
  summaryEl.innerHTML =
    stat('FIXES', fixes.length) + stat('GPS', gps) + stat('CELL', cell)
    + stat('SPAN', spanMin > 0 ? `${spanMin}m` : '<1m')
    + (maxSpd != null ? stat('MAX SPEED', `${mpsToKph(maxSpd)} km/h`) : '')
    + stat('AVG ACCURACY', fmtAcc(avgAcc));
}
