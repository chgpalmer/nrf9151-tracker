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

import { fetchPositions, fetchCells, fetchCurrent } from '/js/api.js';
import { createMapView }   from '/js/mapview.js';
import { createFixTable }  from '/js/fixtable.js';
import { createJourneysTable } from '/js/journeys.js';
import { createCellsTable } from '/js/cells.js';
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
let cellsTable = null;
let isInitialized = false;
let timer    = null;

let dayStart = 0;      // epoch of 00:00 local for the selected day
let isToday  = false;
let dayFixes = [];     // chronological
let dayCells = [];     // serving-cell history for the day (/api/cells)
/* Server-chosen best current position (/api/current): while parked this is
 * the last GPS fix, not the newest heartbeat cell — the live dot shows
 * where the device IS, not which tower last vouched for it. */
let liveFix  = null;
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

// ── timeline input: ONE gesture handler on the track ────────
// Trip bands, the selection body, and the handles share pixels, and one
// pixel can mean "select trip" (click) or "slide window" (drag) — intents
// split by GESTURE, which per-element routing can't express: whichever
// element sits on top steals every gesture there (bands were unclickable
// wherever the window overlapped them). So the children are
// pointer-events:none visuals, and this handler classifies the gesture
// first (drag = moved past DRAG_PX), the geometry second — the same model
// as the map's canvas renderer, d3-brush, and editor timelines.
const DRAG_PX   = 5; // the OS click-vs-drag convention (SM_CXDRAG)
const HANDLE_PX = 8; // resize grab zone around each window edge

function fracFromEvent(clientX) {
  const r = track.getBoundingClientRect();
  return Math.min(1, Math.max(0, (clientX - r.left) / r.width));
}

/* Rendered band geometry (0.01 min-width keeps short trips visible) — hit
 * tests use the SAME math, so what you see is what you can click. */
function tripAtFrac(f) {
  for (let i = 0; i < trips.length; i++) {
    const left  = (trips[i].start_ts - dayStart) / DAY;
    const width = Math.max(0.01, (trips[i].end_ts - trips[i].start_ts) / DAY);
    if (f >= left && f <= left + width) return i;
  }
  return -1;
}

/* Drag zone at a fraction: window edges win (resize), then the interior
 * (slide) — except a full-day window, which has nowhere to slide, so
 * dragging inside it rubber-bands a fresh window instead (DAY mode). */
function dragZone(f) {
  const w = track.getBoundingClientRect().width;
  if (Math.abs((f - selFrac[0]) * w) <= HANDLE_PX) return 'start';
  if (Math.abs((f - selFrac[1]) * w) <= HANDLE_PX) return 'end';
  if ((selFrac[0] > 0.0001 || selFrac[1] < 0.9999) &&
      f > selFrac[0] && f < selFrac[1]) return 'move';
  return 'create';
}

function setWindowFrac(start, width) {
  const s = Math.min(Math.max(0, start), 1 - width);
  selFrac = [s, s + width];
}

let armed = null; // pointerdown seen; click-vs-drag verdict pending

track.addEventListener('pointerdown', e => {
  const f = fracFromEvent(e.clientX);
  armed = { kind: dragZone(f), f0: f, x0: e.clientX };
  track.setPointerCapture(e.pointerId);
});

track.addEventListener('pointermove', e => {
  const f = fracFromEvent(e.clientX);
  if (!armed && !dragging) { hoverAt(f); return; }
  if (armed && !dragging) {
    if (Math.abs(e.clientX - armed.x0) <= DRAG_PX) return; // still a click
    dragging = armed.kind;
    if (dragging === 'move') grabOffset = armed.f0 - selFrac[0];
  }
  if (dragging === 'start')       selFrac[0] = Math.min(f, selFrac[1] - MIN_FRAC);
  else if (dragging === 'end')    selFrac[1] = Math.max(f, selFrac[0] + MIN_FRAC);
  else if (dragging === 'create') selFrac = f >= armed.f0
      ? [armed.f0, Math.max(f, armed.f0 + MIN_FRAC)]
      : [Math.min(f, armed.f0 - MIN_FRAC), armed.f0];
  else setWindowFrac(f - grabOffset, selFrac[1] - selFrac[0]);
  sel = { mode: 'range' };
  renderChips();
  applyView(false); // no re-fit mid-drag
});

track.addEventListener('pointerup', () => {
  const a = armed, wasDrag = dragging;
  armed = null;
  dragging = null;
  if (wasDrag) { applyView(true); return; }
  if (!a) return;
  const i = tripAtFrac(a.f0);  // click: a trip band wins where one exists
  if (i >= 0) {
    sel = { mode: 'trip', i };
  } else {                     // empty track: center the window there
    setWindowFrac(a.f0 - (selFrac[1] - selFrac[0]) / 2,
                  selFrac[1] - selFrac[0]);
    sel = { mode: 'range' };
  }
  renderChips();
  applyView(true);
});

track.addEventListener('pointercancel', () => {
  const wasDrag = dragging;
  armed = null;
  dragging = null;
  if (wasDrag) applyView(true);
});

/* Hover affordances (CSS :hover died with pointer-events:none): cursor by
 * zone, band highlight + tooltip forwarded from the band element. */
let hoverTrip = -1;
function hoverAt(f) {
  const zone = f < 0 ? null : dragZone(f);
  const trip = f < 0 ? -1 : tripAtFrac(f);

  track.style.cursor =
    zone === 'start' || zone === 'end' ? 'ew-resize' :
    trip >= 0                          ? 'pointer'   :
    zone === 'move'                    ? 'grab'      :
    zone === 'create'                  ? 'crosshair' : '';
  if (trip !== hoverTrip) {
    const bands = tripsEl.children;
    if (bands[hoverTrip]) bands[hoverTrip].classList.remove('hover');
    if (bands[trip])      bands[trip].classList.add('hover');
    track.title = bands[trip] ? bands[trip].title : '';
    hoverTrip = trip;
  }
}
track.addEventListener('pointerleave', () => hoverAt(-1));

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

// The popup's "Full detail →" is the one EXPLICIT ask for the detail pane
// (incidental selection deliberately never yanks there). drawer.js fills
// the pane on this event; without the tab switch that happened invisibly.
document.addEventListener('fix-detail-click', () => setTab('detail'));

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
  cellsTable = createCellsTable('cells-table', {
    onSelect: i => {
      const c = dayCells[i];
      cellsTable.render(dayCells, i);
      // A resolved cell has a twin positions row (same insert timestamp):
      // ring it on the map. Unresolved ones have nowhere to point.
      const fix = c && c.lat != null && dayFixes.find(f =>
        f.source === 'cell' && f.received_ts === c.received_ts);
      if (fix) selectPoint(fix);
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
    dayFixes = []; trips = []; dayCells = [];
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

  // Serving-cell history (CELLS tab) — sparse, best-effort, never blocks
  // the map on failure.
  try {
    dayCells = await fetchCells(deviceId, {
      from_ts: dayStart, to_ts: dayStart + DAY,
    });
  } catch (e) {
    dayCells = [];
  }
  cellsTable.render(dayCells);

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

  // Best current position (parked = last GPS fix, not the newest heartbeat
  // cell). Fetched AFTER the first paint so it never delays the page; the
  // dot corrects itself one render later.
  liveFix = null;
  if (isToday) {
    try {
      liveFix = await fetchCurrent(deviceId);
      if (liveFix) applyView(false);
    } catch (e) { /* dot falls back to newest row */ }
  }
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
    try { liveFix = await fetchCurrent(deviceId); } catch (e) { /* keep previous */ }
    trips = segmentTrips(dayFixes);
    renderDensity();
    renderTripBands();
    renderChips();
    applyView(false);
    // Cell reports are sparse; a full-day refetch is a few dozen rows.
    try {
      dayCells = await fetchCells(deviceId, {
        from_ts: dayStart, to_ts: dayStart + DAY,
      });
      cellsTable.render(dayCells);
    } catch (e) { /* keep the old table */ }
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
    liveFix: sel.mode === 'live' ? liveFix : null,
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
    // No listener: the band is a visual + tooltip carrier; clicks are
    // routed by the track's gesture handler (tripAtFrac).
    band.title = `T${i + 1}  ${hhmm(t.start_ts)}–${hhmm(t.end_ts)}  ${fmtDistM(t.dist_m)}`;
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
