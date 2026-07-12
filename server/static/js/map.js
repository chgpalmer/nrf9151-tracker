/**
 * map.js — the unified map page (live + history).
 *
 * Model: load one whole local day, segment it into trips (trips.js), then all
 * selection is client-side and instant. The SHOW chips pick what the map,
 * charts, summary and table describe:
 *
 *   LIVE   (today only) follow the device: the current/latest trip plus the
 *          live position, auto-refreshing every few seconds. The map fits
 *          once on selection, then holds still while the blue dot moves.
 *   DAY    the full day.
 *   T1..Tn one detected trip (stationary >2 min or a gap >5 min splits trips).
 *
 * The timeline shows fix density (canvas — a 1 Hz day is thousands of fixes)
 * with clickable trip bands; the drag handles/band still allow any custom
 * window, which switches the selection to "range". Hovering the charts or
 * table rings the same point on the map.
 */

import { fetchPositions }  from '/js/api.js';
import { createMapView }   from '/js/mapview.js';
import { createFixTable }  from '/js/fixtable.js';
import { createJourneysTable } from '/js/journeys.js';
import { initCharts, updateCharts, setActiveFix, resizeCharts } from '/js/charts.js';
import { open as openDrawer, onClose as onDrawerClose } from '/js/drawer.js';
import { currentDevice, setStatus } from '/js/devices.js';
import { fmtAcc, mpsToKph, deviceStatus } from '/js/format.js';
import { segmentTrips, haversineM, fmtDistM, tripWindows, inAnyWindow } from '/js/trips.js';

const DAY        = 86400;
const MIN_FRAC   = 0.002;
const REFRESH_MS = 5000;
const DAY_LIMIT  = 50000; // API rows per day-load; a full 1 Hz day can exceed this

let mapView  = null;
let fixTable = null;
let journeysTable = null;
let isInitialized = false;
let timer    = null;

let dayStart = 0;      // epoch of 00:00 local for the selected day
let isToday  = false;
let dayFixes = [];     // chronological
let trips    = [];
let sel      = { mode: 'day' };   // 'live' | 'day' | {mode:'trip', i} | 'range'
let selFrac  = [0, 1]; // current window as day fractions (source of truth for 'range')
let dragging = null;
let grabOffset = 0;

// ── controls ────────────────────────────────────────────────
const dateInput = document.getElementById('map-date');
const chipsEl   = document.getElementById('trip-chips');
const rangeEl   = document.getElementById('range-label');
const summaryEl = document.getElementById('summary');
const gpsChip     = document.getElementById('filter-gps');
const cellChip    = document.getElementById('filter-cell');
const pointsChip  = document.getElementById('show-points');

const track     = document.getElementById('tl-track');
const densityEl = document.getElementById('tl-density');
const tripsEl   = document.getElementById('tl-trips');
const selEl     = document.getElementById('tl-sel');
const startH    = document.getElementById('tl-start');
const endH      = document.getElementById('tl-end');
const axisEl    = document.getElementById('tl-axis');

function todayStr() {
  const d = new Date();
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
}

dateInput.value = todayStr();
dateInput.addEventListener('change', loadDay);

// Prev/next day steppers; forward navigation stops at today.
function stepDay(days) {
  const d = new Date(`${dateInput.value}T12:00:00`); // noon dodges DST edges
  d.setDate(d.getDate() + days);
  const s = `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
  if (s > todayStr()) return;
  dateInput.value = s;
  loadDay();
}
document.getElementById('date-prev').addEventListener('click', () => stepDay(-1));
document.getElementById('date-next').addEventListener('click', () => stepDay(1));

[gpsChip, cellChip].forEach(chip => {
  chip.addEventListener('click', () => {
    chip.classList.toggle('active');
    chip.setAttribute('aria-pressed', String(chip.classList.contains('active')));
    renderDensity(); // the timeline respects the source filter too
    applyView(false);
  });
});

pointsChip.addEventListener('click', () => {
  pointsChip.classList.toggle('active');
  pointsChip.setAttribute('aria-pressed', String(pointsChip.classList.contains('active')));
  mapView && mapView.setShowPoints(pointsChip.classList.contains('active'));
});

// ── timeline drag (custom window = 'range' selection) ───────
function fracFromEvent(clientX) {
  const r = track.getBoundingClientRect();
  return Math.min(1, Math.max(0, (clientX - r.left) / r.width));
}

function onPointerMove(e) {
  if (!dragging) return;
  const f = fracFromEvent(e.clientX);
  if (dragging === 'start')    selFrac[0] = Math.min(f, selFrac[1] - MIN_FRAC);
  else if (dragging === 'end') selFrac[1] = Math.max(f, selFrac[0] + MIN_FRAC);
  else setWindowFrac(f - grabOffset, selFrac[1] - selFrac[0]);
  sel = { mode: 'range' };
  renderChips();
  applyView(false);  // no re-fit mid-drag
}

function endDrag() {
  if (!dragging) return;
  dragging = null;
  applyView(true);
}

function setWindowFrac(start, width) {
  const s = Math.min(Math.max(0, start), 1 - width);
  selFrac = [s, s + width];
}

startH.addEventListener('pointerdown', e => { dragging = 'start'; startH.setPointerCapture(e.pointerId); });
endH.addEventListener('pointerdown',   e => { dragging = 'end';   endH.setPointerCapture(e.pointerId); });
selEl.addEventListener('pointerdown',  e => {
  dragging = 'move';
  grabOffset = fracFromEvent(e.clientX) - selFrac[0];
  selEl.setPointerCapture(e.pointerId);
});
track.addEventListener('pointermove', onPointerMove);
window.addEventListener('pointerup', endDrag);

// Click empty track: center the current window there.
track.addEventListener('pointerdown', e => {
  if (e.target === startH || e.target === endH || e.target === selEl ||
      e.target.classList.contains('tl-trip-band')) return;
  setWindowFrac(fracFromEvent(e.clientX) - (selFrac[1] - selFrac[0]) / 2,
                selFrac[1] - selFrac[0]);
  sel = { mode: 'range' };
  renderChips();
  applyView(true);
});

function keyNudge(e, which) {
  const step = (e.shiftKey ? 60 : 300) / DAY;
  const dir = e.key === 'ArrowLeft' ? -1 : e.key === 'ArrowRight' ? 1 : 0;
  if (!dir) return;
  e.preventDefault();
  if (which === 'start') selFrac[0] = Math.min(Math.max(0, selFrac[0] + dir * step), selFrac[1] - MIN_FRAC);
  else                   selFrac[1] = Math.max(Math.min(1, selFrac[1] + dir * step), selFrac[0] + MIN_FRAC);
  sel = { mode: 'range' };
  renderChips();
  applyView(true);
}
startH.addEventListener('keydown', e => keyNudge(e, 'start'));
endH.addEventListener('keydown',   e => keyNudge(e, 'end'));

// ── side-panel tabs (JOURNEYS | LOCATIONS | DETAIL) ─────────
// One panel active at a time. Selecting a fix anywhere jumps to DETAIL;
// closing the detail returns to wherever you were.
let lastTab = 'journeys';

function setTab(name) {
  if (name !== 'detail') lastTab = name;
  document.querySelectorAll('#side-tabs .side-tab').forEach(b =>
    b.classList.toggle('active', b.dataset.tab === name));
  document.querySelectorAll('.side-tab-panel').forEach(p =>
    p.classList.toggle('active', p.dataset.tab === name));
}

document.querySelectorAll('#side-tabs .side-tab').forEach(b =>
  b.addEventListener('click', () => setTab(b.dataset.tab)));

onDrawerClose(() => setTab(lastTab));

// Chart tabs: one canvas visible at a time; Chart.js needs a resize poke
// when a hidden canvas becomes visible. The whole panel collapses to its
// tab bar (▾/▸) — default-collapsed on phones, where the map is king.
const chartPanel    = document.getElementById('chart-strip');
const chartCollapse = document.getElementById('chart-collapse');

function setChartCollapsed(v) {
  chartPanel.classList.toggle('collapsed', v);
  chartCollapse.textContent = v ? '▸' : '▾';
  if (!v) resizeCharts();
}

chartCollapse.addEventListener('click', () =>
  setChartCollapsed(!chartPanel.classList.contains('collapsed')));

if (window.matchMedia('(max-width: 1099px)').matches) setChartCollapsed(true);

document.querySelectorAll('#chart-tabs .chart-tab').forEach(b =>
  b.addEventListener('click', () => {
    document.querySelectorAll('#chart-tabs .chart-tab').forEach(t =>
      t.classList.toggle('active', t === b));
    document.querySelectorAll('.chart-wrap').forEach(w =>
      w.classList.toggle('active', w.dataset.chart === b.dataset.chart));
    setChartCollapsed(false); // picking a chart means you want to see it
  }));

// ── lifecycle ───────────────────────────────────────────────
// Cross-view selection: whichever surface a fix is picked on (map marker,
// chart click, table row), the others reflect it — map ring + pan, chart
// dot, highlighted table row. The DETAIL panel content is kept fresh
// silently but the tab does NOT auto-switch (that was jarring); tap
// DETAIL when you actually want the numbers.
function selectPoint(fix) {
  mapView.focusFix(fix);
  openDrawer(fix);
  setActiveFix(fix);
  fixTable.highlight(fix);
}

export function init() {
  if (isInitialized) return;
  isInitialized = true;

  mapView = createMapView('map-main', selectPoint);
  window._mapView = mapView;
  fixTable = createFixTable('fix-table', {
    onRowClick: selectPoint,
    onRowHover: fix => mapView.hoverFix(fix),
    startOpen: true, // it lives in a side-panel tab now; the tab is the toggle
  });
  journeysTable = createJourneysTable('journeys-table', {
    onSelect: i => {
      sel = { mode: 'trip', i };
      renderChips();
      applyView(true);
    },
  });
  initCharts({
    onHover: fix => mapView.hoverFix(fix),
    onSelect: selectPoint,
  });
  buildAxis();
  loadDay();
}

export function start() {
  init();
  scheduleTimer();
}

export function stop() {
  clearInterval(timer);
  timer = null;
}

export function onDeviceChange() {
  if (isInitialized) loadDay();
}

function scheduleTimer() {
  clearInterval(timer);
  timer = isToday ? setInterval(refresh, REFRESH_MS) : null;
}

// ── data ────────────────────────────────────────────────────
async function loadDay() {
  const deviceId = currentDevice();
  dayStart = Math.floor(new Date(`${dateInput.value}T00:00:00`).getTime() / 1000);
  isToday  = dateInput.value === todayStr();
  // Tomorrow doesn't exist yet: grey the forward stepper out at today.
  document.getElementById('date-next').disabled = isToday;

  if (!deviceId) {
    dayFixes = []; trips = [];
    summaryEl.innerHTML = `<span class="hist-hint">Select a device.</span>`;
    return;
  }

  try {
    dayFixes = await fetchPositions(deviceId, {
      from_ts: dayStart, to_ts: dayStart + DAY, limit: DAY_LIMIT,
    });
  } catch (e) {
    console.error('Day load error:', e);
    summaryEl.innerHTML = `<span class="hist-hint err">Failed to load: ${e.message}</span>`;
    return;
  }

  // Live must always show where the device is: an empty *today* falls back to
  // the single most recent fix so the device never vanishes from the map.
  if (dayFixes.length === 0 && isToday) {
    try { dayFixes = await fetchPositions(deviceId, { limit: 1 }); } catch (e) { /* keep empty */ }
  }

  trips = segmentTrips(dayFixes);
  sel = isToday ? { mode: 'live' } : { mode: 'day' };
  renderDensity();
  renderTripBands();
  renderChips();
  applyView(true);
  scheduleTimer();
}

// Incremental refresh (today only): append what's new, keep the view still.
async function refresh() {
  const deviceId = currentDevice();
  if (!deviceId || dragging) return;

  const last = dayFixes.length ? dayFixes[dayFixes.length - 1].received_ts : dayStart;
  let fresh;
  try {
    fresh = await fetchPositions(deviceId, {
      from_ts: last + 0.001, to_ts: dayStart + DAY, limit: DAY_LIMIT,
    });
  } catch (e) {
    return; // transient; next tick retries
  }
  if (fresh.length > 0) {
    dayFixes = dayFixes.concat(fresh);
    trips = segmentTrips(dayFixes);
    renderDensity();
    renderTripBands();
    renderChips();
    applyView(false);
  }
  if (dayFixes.length > 0) {
    setStatus(deviceStatus(dayFixes[dayFixes.length - 1].received_ts));
  }
}

// ── selection → window ──────────────────────────────────────
function windowTs() {
  const dayEnd = dayStart + DAY;
  if (sel.mode === 'live') {
    const lastTs = dayFixes.length ? dayFixes[dayFixes.length - 1].received_ts
                                   : Date.now() / 1000;
    const t = trips[trips.length - 1];
    // On (or just off) a trip: show it from its start. Otherwise trail 15 min.
    const from = (t && lastTs - t.end_ts < 300) ? t.start_ts - 30 : lastTs - 900;
    return [Math.max(dayStart, from), dayEnd];
  }
  if (sel.mode === 'trip' && trips[sel.i]) {
    return [trips[sel.i].start_ts - 30, trips[sel.i].end_ts + 30];
  }
  if (sel.mode === 'range') {
    return [dayStart + selFrac[0] * DAY, dayStart + selFrac[1] * DAY];
  }
  return [dayStart, dayEnd];
}

function applyView(fit) {
  const [fromTs, toTs] = windowTs();
  selFrac = [(fromTs - dayStart) / DAY, Math.min(1, (toTs - dayStart) / DAY)];

  // selection band + handles
  startH.style.left = (selFrac[0] * 100) + '%';
  endH.style.left   = (selFrac[1] * 100) + '%';
  selEl.style.left  = (selFrac[0] * 100) + '%';
  selEl.style.width = ((selFrac[1] - selFrac[0]) * 100) + '%';
  rangeEl.textContent = `${hhmm(fromTs)} – ${hhmm(toTs)} · ${durLabel(toTs - fromTs)}`;

  const gps  = gpsChip.classList.contains('active');
  const cell = cellChip.classList.contains('active');
  const win = dayFixes.filter(f =>
    f.received_ts >= fromTs && f.received_ts <= toTs &&
    (f.source === 'gps' ? gps : cell));

  const filtered = win.length === 0 && dayFixes.length > 0 && !(gps && cell);
  mapView.render(win, {
    fitBounds: fit,
    showPoints: pointsChip.classList.contains('active'),
    // DAY = journeys + current position: the track draws per-journey,
    // parked scatter hides behind the POINTS chip. Other modes are
    // explicit windows the user asked for — show everything.
    tripWindows: sel.mode === 'day' ? tripWindows(trips) : null,
    liveDot: sel.mode === 'live',
    emptyMsg: filtered ? 'All fixes hidden by source filter' : undefined,
    emptySub: filtered ? 'Re-enable GPS or CELL above.' : undefined,
  });
  updateCharts(win);
  fixTable.render(win);
  renderSummary(win);
}

// ── chips ───────────────────────────────────────────────────
function chipActive(c) {
  if (c.mode !== sel.mode) return false;
  return c.mode !== 'trip' || c.i === sel.i;
}

function renderChips() {
  // LIVE + DAY only — trips live in the Journeys side-panel table.
  const chips = [];
  if (isToday) chips.push({ mode: 'live', label: '● LIVE' });
  chips.push({ mode: 'day', label: 'DAY' });

  chipsEl.innerHTML = '';
  for (const c of chips) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'trip-chip' + (c.mode === 'live' ? ' live' : '') +
                  (chipActive(c) ? ' active' : '');
    b.textContent = c.label;
    b.addEventListener('click', () => {
      sel = { mode: c.mode };
      renderChips();
      applyView(true);
    });
    chipsEl.appendChild(b);
  }

  if (journeysTable) {
    journeysTable.render(trips, sel.mode === 'trip' ? sel.i : null);
  }
}

// ── timeline rendering ──────────────────────────────────────
function renderDensity() {
  const dpr = window.devicePixelRatio || 1;
  const w = densityEl.clientWidth, h = densityEl.clientHeight;
  if (!w) return;
  densityEl.width = w * dpr; densityEl.height = h * dpr;
  const ctx = densityEl.getContext('2d');
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, w, h);
  // Ticks only inside journeys — parked wakes (GPS jitter AND cell
  // heartbeats) are the vast majority of a day's points (measured
  // 2026-07-12: 86% GPS alone) and pure static here; the trip bands ARE
  // the shape of the day. The SOURCES chips filter here like everywhere
  // else.
  const wins = tripWindows(trips);
  const gps  = gpsChip.classList.contains('active');
  const cell = cellChip.classList.contains('active');
  for (const f of dayFixes) {
    if (!(f.source === 'gps' ? gps : cell)) continue;
    if (!inAnyWindow(f.received_ts, wins)) continue;
    const frac = (f.received_ts - dayStart) / DAY;
    if (frac < 0 || frac > 1) continue;
    ctx.fillStyle = f.source === 'cell' ? 'rgba(245,166,35,0.7)' : 'rgba(0,229,160,0.55)';
    ctx.fillRect(frac * w, h * 0.18, 1, h * 0.64);
  }
}

function renderTripBands() {
  tripsEl.innerHTML = '';
  trips.forEach((t, i) => {
    const band = document.createElement('div');
    band.className = 'tl-trip-band';
    band.style.left  = ((t.start_ts - dayStart) / DAY * 100) + '%';
    // 0.01 min-width (~15 min of day) keeps a 5-minute journey clickable.
    band.style.width = (Math.max(0.01, (t.end_ts - t.start_ts) / DAY) * 100) + '%';
    band.title = `T${i + 1}  ${hhmm(t.start_ts)}–${hhmm(t.end_ts)}  ${fmtDistM(t.dist_m)}`;
    band.addEventListener('click', () => {
      sel = { mode: 'trip', i };
      renderChips();
      applyView(true);
    });
    tripsEl.appendChild(band);
  });
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

// ── summary ─────────────────────────────────────────────────
function renderSummary(fixes) {
  if (fixes.length === 0) {
    summaryEl.innerHTML = `<span class="hist-hint">No fixes in this window.</span>`;
    return;
  }
  const gps  = fixes.filter(f => f.source === 'gps');
  const cell = fixes.length - gps.length;
  let dist = 0;
  for (let i = 1; i < gps.length; i++) {
    dist += haversineM(gps[i - 1].lat, gps[i - 1].lon, gps[i].lat, gps[i].lon);
  }
  const spds = gps.filter(f => f.spd != null).map(f => f.spd);
  const maxSpd = spds.length ? Math.max(...spds) : null;
  const avgAcc = fixes.reduce((s, f) => s + f.acc, 0) / fixes.length;
  const spanMin = Math.round((fixes[fixes.length - 1].received_ts - fixes[0].received_ts) / 60);

  const stat = (l, v) => `<div class="hist-stat"><span class="hist-stat-label">${l}</span><span class="hist-stat-val">${v}</span></div>`;
  summaryEl.innerHTML =
    stat('FIXES', fixes.length) + stat('GPS', gps.length) + stat('CELL', cell)
    + stat('SPAN', spanMin > 0 ? durLabel(spanMin * 60) : '<1m')
    + (dist > 20 ? stat('DIST', fmtDistM(dist)) : '')
    + (maxSpd != null ? stat('MAX SPEED', `${mpsToKph(maxSpd)} km/h`) : '')
    + stat('AVG ACCURACY', fmtAcc(avgAcc));
}

// ── helpers ─────────────────────────────────────────────────
function hhmm(ts) {
  return new Date(ts * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function durLabel(secs) {
  const m = Math.round(secs / 60);
  if (m < 60) return `${m}m`;
  const h = Math.floor(m / 60), rm = m % 60;
  return rm > 0 ? `${h}h ${String(rm).padStart(2, '0')}m` : `${h}h`;
}
