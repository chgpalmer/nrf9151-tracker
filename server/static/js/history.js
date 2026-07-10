/**
 * history.js — History view.
 *
 * Model: load the whole selected day once, then filter with a draggable
 * timeline window (like trimming a video clip). Dragging re-filters the
 * already-loaded fixes client-side, so the map updates instantly with no
 * per-drag server calls. Fix-density ticks on the timeline show where data
 * exists so you drag straight to it.
 */

import { fetchPositions }  from '/js/api.js';
import { createMapView }   from '/js/mapview.js';
import { open as openDrawer } from '/js/drawer.js';
import { currentDevice }   from '/js/devices.js';
import { fmtAcc, mpsToKph } from '/js/format.js';

const DAY = 86400;

let mapView       = null;
let isInitialized = false;

let dayStart   = 0;      // epoch of 00:00 for the selected local day
let dayFixes   = [];     // all fixes for the day (chronological)
let selFrac    = [0, 1]; // [start, end] window as fractions of the day
let dragging   = null;   // 'start' | 'end' | null

const dateInput = document.getElementById('hist-date');
const rangeEl   = document.getElementById('hist-range');
const summaryEl = document.getElementById('hist-summary');
const accToggle = document.getElementById('show-accuracy-hist');

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

// ── timeline drag ───────────────────────────────────────────
function fracFromEvent(clientX) {
  const r = track.getBoundingClientRect();
  return Math.min(1, Math.max(0, (clientX - r.left) / r.width));
}

function onPointerMove(e) {
  if (!dragging) return;
  const f = fracFromEvent(e.clientX);
  if (dragging === 'start') selFrac[0] = Math.min(f, selFrac[1] - 0.005);
  else                      selFrac[1] = Math.max(f, selFrac[0] + 0.005);
  applyWindow();
}

function endDrag() { dragging = null; }

startH.addEventListener('pointerdown', (e) => { dragging = 'start'; startH.setPointerCapture(e.pointerId); });
endH.addEventListener('pointerdown',   (e) => { dragging = 'end';   endH.setPointerCapture(e.pointerId); });
track.addEventListener('pointermove', onPointerMove);
window.addEventListener('pointerup', endDrag);

// keyboard: arrows nudge a focused handle by 5 minutes
function nudge(which, deltaFrac) {
  if (which === 'start') selFrac[0] = Math.min(Math.max(0, selFrac[0] + deltaFrac), selFrac[1] - 0.005);
  else                   selFrac[1] = Math.max(Math.min(1, selFrac[1] + deltaFrac), selFrac[0] + 0.005);
  applyWindow();
}
const STEP = 300 / DAY; // 5 min
startH.addEventListener('keydown', (e) => keyNudge(e, 'start'));
endH.addEventListener('keydown',   (e) => keyNudge(e, 'end'));
function keyNudge(e, which) {
  if (e.key === 'ArrowLeft')  { nudge(which, -STEP); e.preventDefault(); }
  if (e.key === 'ArrowRight') { nudge(which,  STEP); e.preventDefault(); }
}

export function init() {
  if (isInitialized) return;
  isInitialized = true;
  mapView = createMapView('map-history', fix => openDrawer(fix));
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
  renderDensity();
  applyWindow();
}

// Filter dayFixes to the current window and render map + summary + handles.
function applyWindow() {
  const from_ts = dayStart + Math.round(selFrac[0] * DAY);
  const to_ts   = dayStart + Math.round(selFrac[1] * DAY);

  // position handles + selection band
  startH.style.left = (selFrac[0] * 100) + '%';
  endH.style.left   = (selFrac[1] * 100) + '%';
  selEl.style.left  = (selFrac[0] * 100) + '%';
  selEl.style.width = ((selFrac[1] - selFrac[0]) * 100) + '%';
  rangeEl.textContent = `${hhmm(from_ts)} – ${hhmm(to_ts)}`;

  const win = dayFixes.filter(f => f.received_ts >= from_ts && f.received_ts <= to_ts);
  mapView.render(win, { fitBounds: true, showAccuracy: accToggle.checked, showArrows: true });
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
