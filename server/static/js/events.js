/**
 * events.js — the Events page: things that HAPPENED, newest first.
 *
 * Positions say where the asset was; an event says something occurred and may
 * deserve a human's attention. Today the only kind is a motion wake (the
 * accelerometer fired while parked — "your asset moved"). Each row links to
 * the map at the position recorded nearest that moment. Day-based like the
 * Logs page: pick a date, load that day's events.
 */

import { fetchEvents, setArm } from '/js/api.js';
import { currentDevice, currentDeviceObj, setArmedLocal } from '/js/devices.js';
import { fmtTime } from '/js/format.js';

const DAY = 86400;
const REASON = { 1: 'Motion detected' };

let started = false;
const tbody = () => document.getElementById('events-tbody');
const empty = () => document.getElementById('events-empty');
const dateInput = () => document.getElementById('events-date');

function todayStr() {
  const d = new Date();
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
}

/** Local-midnight epoch (seconds) for the picked date. */
function dayStart() {
  return new Date(`${dateInput().value}T00:00:00`).getTime() / 1000;
}

function stepDay(days) {
  const d = new Date(`${dateInput().value}T12:00:00`);
  d.setDate(d.getDate() + days);
  const s = `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
  if (s > todayStr()) return;
  dateInput().value = s;
  load();
}

function fmtDate(ts) {
  return new Date(ts * 1000).toLocaleDateString([], {
    year: 'numeric', month: 'short', day: 'numeric',
  });
}

function render(rows) {
  const body = tbody();
  if (!body) return;
  body.innerHTML = '';
  const e = empty();
  if (e) e.classList.toggle('hidden', rows.length > 0);

  for (const ev of rows) {
    const tr = document.createElement('tr');
    const label = ev.kind === 'motion'
      ? (REASON[ev.reason] || 'Motion detected') : ev.kind;
    const place = (ev.lat != null && ev.lon != null)
      ? `<a href="#map">${ev.lat.toFixed(5)}, ${ev.lon.toFixed(5)}</a>`
      : '<span class="muted">no position</span>';
    tr.innerHTML =
      `<td>${fmtDate(ev.received_ts)} ${fmtTime(ev.received_ts)}</td>` +
      `<td><span class="event-chip event-${ev.kind}">${label}</span></td>` +
      `<td>${place}</td>`;
    body.appendChild(tr);
  }
}

export async function load() {
  const dev = currentDevice();
  if (!dev || !dateInput()) { render([]); return; }
  const from = dayStart();
  try {
    render(await fetchEvents(dev, { from_ts: from, to_ts: from + DAY, limit: 500 }));
  } catch (err) {
    console.error('events load failed', err);
    render([]);
  }
}

/* Arm/disarm toggle — reflects the current device's armed flag and flips it. */
const armBtn = () => document.getElementById('arm-toggle');

function renderArm(armed) {
  const b = armBtn();
  if (!b) return;
  b.textContent = armed ? 'ARMED' : 'DISARMED';
  b.setAttribute('aria-pressed', armed ? 'true' : 'false');
  b.classList.toggle('armed', !!armed);
}

async function toggleArm() {
  const dev = currentDevice();
  if (!dev) return;
  const next = !(currentDeviceObj()?.armed);
  try {
    const res = await setArm(dev, next);
    setArmedLocal(dev, res.armed);
    renderArm(res.armed);
  } catch (err) {
    console.error('arm toggle failed', err);
  }
}

export function start() {
  started = true;
  const di = dateInput();
  if (di && !di.dataset.wired) {
    di.dataset.wired = '1';
    di.value = todayStr();
    di.addEventListener('change', load);
    document.getElementById('events-date-prev')?.addEventListener('click', () => stepDay(-1));
    document.getElementById('events-date-next')?.addEventListener('click', () => stepDay(1));
  } else if (di && !di.value) {
    di.value = todayStr();
  }
  const b = armBtn();
  if (b && !b.dataset.wired) {
    b.dataset.wired = '1';
    b.addEventListener('click', toggleArm);
  }
  renderArm(currentDeviceObj()?.armed);
  load();
}

export function stop() { started = false; }

export function onDeviceChange() {
  if (started) {
    renderArm(currentDeviceObj()?.armed);
    load();
  }
}
