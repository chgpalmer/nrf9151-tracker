/**
 * usage.js — data spent per day, straight from the ingest ledger.
 *
 * The server records every datagram it receives (and both halves of A-GNSS
 * exchanges), so this page is the device's data diary: what a parked day
 * costs, what a ride costs, and whether a change made things better or
 * worse — without logging into the SIM portal. The "est. on-air" column is
 * payload + framing; real billing adds per-RRC-session rounding on top
 * (field-calibrated roughly 2x on quiet days), which a per-datagram ledger
 * cannot see — the header note says so.
 */

import { fetchUsage } from '/js/api.js';
import { currentDevice } from '/js/devices.js';

const REFRESH_MS = 30000;

let isInitialized = false;
let timer = null;

const tbody   = document.getElementById('usage-tbody');
const todayEl = document.getElementById('usage-today');

function fmtKB(bytes) {
  if (bytes >= 1048576) return `${(bytes / 1048576).toFixed(2)} MB`;
  return `${(bytes / 1024).toFixed(1)} KB`;
}

function todayStr() {
  return new Date().toISOString().slice(0, 10); // ledger days are UTC
}

export function start() {
  isInitialized = true;
  load();
  clearInterval(timer);
  timer = setInterval(load, REFRESH_MS);
}

export function stop() {
  clearInterval(timer);
  timer = null;
}

export function onDeviceChange() {
  if (isInitialized) load();
}

async function load() {
  const deviceId = currentDevice();
  if (!deviceId) return;

  let days;
  try {
    days = await fetchUsage(deviceId, 14);
  } catch (e) {
    console.error('Usage load error:', e);
    todayEl.textContent = `failed: ${e.message}`;
    return;
  }
  render(days);
}

function render(days) {
  const today = days.find(d => d.day === todayStr());
  todayEl.innerHTML = today
    ? `<span class="usage-big">${fmtKB(today.est_wire)}</span>
       <span class="usage-big-label">estimated on-air today
       (${today.datagrams} datagrams)</span>`
    : '<span class="usage-big-label">nothing received today yet</span>';

  const max = Math.max(1, ...days.map(d => d.est_wire));
  // Newest first, like the SIM portal reads.
  tbody.innerHTML = [...days].reverse().map(d => `
    <tr>
      <td class="log-time">${d.day}</td>
      <td>${d.datagrams}</td>
      <td>${fmtKB(d.obs_bytes)}</td>
      <td>${fmtKB(d.agnss_bytes)}</td>
      <td><b>${fmtKB(d.est_wire)}</b></td>
      <td class="usage-bar-cell">
        <div class="usage-bar" style="width:${(100 * d.est_wire / max).toFixed(1)}%"></div>
      </td>
    </tr>`).join('');

  if (!days.length) {
    tbody.innerHTML =
      '<tr><td colspan="6" class="log-time">no usage recorded yet — the ledger starts with the first datagram after this deploy</td></tr>';
  }
}
