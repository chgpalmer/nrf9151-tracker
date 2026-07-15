/**
 * devices.js — device picker and sidebar device info
 */

import { fetchDevices } from '/js/api.js';
import { fmtAge, deviceStatus, statusLabel } from '/js/format.js';

const selectEl      = document.getElementById('device-select');
const countEl       = document.getElementById('device-count');
const infoEl        = document.getElementById('sidebar-device-info');
const statusInd     = document.getElementById('status-indicator');
const statusLabelEl = document.getElementById('status-label');
const armedBadge    = document.getElementById('armed-badge');

let devices  = [];
let onChangeCb = null;

export function onDeviceChange(cb) { onChangeCb = cb; }

export function currentDevice() { return selectEl.value || null; }

export function currentDeviceObj() {
  return devices.find(d => d.device_id === selectEl.value) || null;
}

/* Reflect an arm/disarm done elsewhere (events.js) into the local device list
 * and the top-rail badge, without a full refetch. */
export function setArmedLocal(deviceId, armed) {
  const dev = devices.find(d => d.device_id === deviceId);
  if (dev) dev.armed = armed ? 1 : 0;
  if (armedBadge && selectEl.value === deviceId) {
    armedBadge.classList.toggle('hidden', !armed);
  }
}

selectEl.addEventListener('change', () => {
  updateSidebarInfo();
  onChangeCb && onChangeCb(selectEl.value);
});

export async function loadDevices() {
  try {
    devices = await fetchDevices();
    populatePicker(devices);
    updateSidebarInfo();
    if (devices.length > 0) {
      onChangeCb && onChangeCb(selectEl.value);
    }
  } catch (e) {
    console.error('Failed to load devices:', e);
    selectEl.innerHTML = '<option value="">— error loading —</option>';
  }
}

function populatePicker(devs) {
  selectEl.innerHTML = '';
  if (devs.length === 0) {
    selectEl.innerHTML = '<option value="">— no devices —</option>';
    countEl.textContent = '';
    return;
  }
  devs.forEach(d => {
    const opt = document.createElement('option');
    opt.value       = d.device_id;
    opt.textContent = d.device_id;
    selectEl.appendChild(opt);
  });
  countEl.textContent = `${devs.length} device${devs.length !== 1 ? 's' : ''}`;
}

function updateSidebarInfo() {
  const dev = currentDeviceObj();
  if (!dev) {
    infoEl.innerHTML = '';
    setStatus('offline');
    if (armedBadge) armedBadge.classList.add('hidden');
    return;
  }

  const status = deviceStatus(dev.last_seen);
  setStatus(status);
  if (armedBadge) armedBadge.classList.toggle('hidden', !dev.armed);

  infoEl.innerHTML = `
    <div class="info-row">
      <span class="info-key">Device ID</span>
      <span class="info-val">${dev.device_id}</span>
    </div>
    <div class="info-row">
      <span class="info-key">Last seen</span>
      <span class="info-val">${fmtAge(dev.last_seen)}</span>
    </div>
    <div class="info-row">
      <span class="info-key">Fix count</span>
      <span class="info-val">${dev.n.toLocaleString()}</span>
    </div>
    <div class="info-row">
      <span class="info-key">Battery</span>
      <span class="info-val muted">n/a</span>
    </div>
  `;
}

export function setStatus(status) {
  statusInd.dataset.status = status;
  statusLabelEl.textContent = statusLabel(status);
}

export function refreshStatus() {
  const dev = currentDeviceObj();
  if (dev) {
    const status = deviceStatus(dev.last_seen);
    setStatus(status);
  }
}
