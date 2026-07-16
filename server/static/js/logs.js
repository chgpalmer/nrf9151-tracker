/**
 * logs.js — device log lines shipped over the uplink, optionally interleaved
 * with the server's own events for that device (SOURCE pills: the ingest's
 * alert outcomes, cell resolution, assistance-supply health — the correlated
 * view that diagnosed the 2026-07-16 A-GNSS outage over ssh).
 *
 * Same day-based model as the Map page: pick a date, load its lines, filter
 * by severity client-visible via the LEVEL pills (min_level is server-side —
 * a re-fetch — because DBG days can be large). Today auto-refreshes with
 * incremental fetches and keeps the view pinned to the bottom (newest) when
 * it was already there, terminal-style.
 */

import { fetchLogs } from '/js/api.js';
import { currentDevice } from '/js/devices.js';
import { fmtTime } from '/js/format.js';

const DAY = 86400;
const REFRESH_MS = 5000;
const LVL = { 1: 'ERR', 2: 'WRN', 3: 'INF', 4: 'DBG' };

let isInitialized = false;
let timer = null;
let dayStart = 0;
let isToday = false;
let minLevel = 3;
/* 'device' | 'server' | 'all' — server rows are the ingest's own events
 * (alert outcomes, cell resolution, assistance-supply health) interleaved
 * with what the device was doing at that moment: the correlated view. */
let origin = 'all';
let rows = [];

const dateInput = document.getElementById('logs-date');
const pills   = document.querySelectorAll('#logs-level .pill');
const srcPills = document.querySelectorAll('#logs-origin .pill');
const tbody   = document.getElementById('log-tbody');
const scroll  = document.getElementById('log-scroll');
const countEl = document.getElementById('logs-count');

function todayStr() {
  const d = new Date();
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
}

function stepDay(days) {
  const d = new Date(`${dateInput.value}T12:00:00`);
  d.setDate(d.getDate() + days);
  const s = `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
  if (s > todayStr()) return;
  dateInput.value = s;
  loadDay();
}

export function init() {
  if (isInitialized) return;
  isInitialized = true;

  dateInput.value = todayStr();
  dateInput.addEventListener('change', loadDay);
  document.getElementById('logs-date-prev').addEventListener('click', () => stepDay(-1));
  document.getElementById('logs-date-next').addEventListener('click', () => stepDay(1));

  pills.forEach(p => {
    p.addEventListener('click', () => {
      pills.forEach(x => x.classList.remove('active'));
      p.classList.add('active');
      minLevel = parseInt(p.dataset.level, 10);
      loadDay();
    });
  });

  srcPills.forEach(p => {
    p.addEventListener('click', () => {
      srcPills.forEach(x => x.classList.remove('active'));
      p.classList.add('active');
      origin = p.dataset.origin;
      loadDay();
    });
  });
}

export function start() {
  init();
  loadDay();
  clearInterval(timer);
  timer = setInterval(refresh, REFRESH_MS);
}

export function stop() {
  clearInterval(timer);
  timer = null;
}

export function onDeviceChange() {
  if (isInitialized) loadDay();
}

async function loadDay() {
  const deviceId = currentDevice();
  if (!deviceId) return;

  dayStart = Math.floor(new Date(`${dateInput.value}T00:00:00`).getTime() / 1000);
  isToday = dateInput.value === todayStr();

  try {
    rows = await fetchLogs(deviceId, {
      from_ts: dayStart, to_ts: dayStart + DAY, min_level: minLevel,
      limit: 5000, origin,
    });
  } catch (e) {
    console.error('Logs load error:', e);
    countEl.textContent = `failed: ${e.message}`;
    return;
  }
  render(true);
}

async function refresh() {
  const deviceId = currentDevice();
  if (!deviceId || !isToday) return;

  const last = rows.length ? rows[rows.length - 1].received_ts : dayStart;
  let fresh;
  try {
    fresh = await fetchLogs(currentDevice(), {
      from_ts: last + 0.001, to_ts: dayStart + DAY, min_level: minLevel,
      limit: 5000, origin,
    });
  } catch (e) {
    return;
  }
  if (fresh.length > 0) {
    rows = rows.concat(fresh);
    render(false);
  }
}

function esc(s) {
  return String(s ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;');
}

function render(jumpToEnd) {
  // Pinned-to-bottom behaviour: keep following newest unless the user has
  // scrolled up to read something.
  const pinned = jumpToEnd ||
    scroll.scrollTop + scroll.clientHeight >= scroll.scrollHeight - 40;

  tbody.innerHTML = rows.map(r => `
    <tr class="lvl-${r.level}${r.origin === 'server' ? ' log-origin-server' : ''}">
      <td class="log-time">${fmtTime(r.received_ts)}</td>
      <td><span class="log-lvl lvl-${r.level}">${LVL[r.level] || r.level}</span></td>
      <td class="log-mod">${r.origin === 'server' ? 'srv:' : ''}${esc(r.module)}</td>
      <td class="log-text">${esc(r.text)}</td>
    </tr>`).join('');

  countEl.textContent = rows.length
    ? `${rows.length} line${rows.length !== 1 ? 's' : ''}`
    : 'no log lines in this window';

  if (pinned) {
    scroll.scrollTop = scroll.scrollHeight;
  }
}
