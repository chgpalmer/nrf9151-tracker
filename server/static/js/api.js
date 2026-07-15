/**
 * api.js — thin wrappers over the tracker REST API
 */

/**
 * GET /api/devices
 * @returns {Promise<Array<{device_id:string, last_seen:number, n:number}>>}
 */
export async function fetchDevices() {
  const r = await fetch('/api/devices');
  if (!r.ok) throw new Error(`/api/devices: ${r.status}`);
  return r.json();
}

/**
 * GET /api/positions with flexible query params.
 *
 * @param {string} deviceId
 * @param {object} opts
 * @param {number} [opts.since]    — minutes back from now
 * @param {number} [opts.from_ts] — epoch seconds
 * @param {number} [opts.to_ts]   — epoch seconds
 * @param {number} [opts.limit]
 * @returns {Promise<Array>}       — fixes in chronological order (oldest first)
 */
export async function fetchPositions(deviceId, opts = {}) {
  const params = new URLSearchParams({ device: deviceId });
  if (opts.since  != null) params.set('since',   opts.since);
  if (opts.from_ts != null) params.set('from_ts', opts.from_ts);
  if (opts.to_ts   != null) params.set('to_ts',   opts.to_ts);
  if (opts.limit   != null) params.set('limit',   opts.limit);

  const r = await fetch(`/api/positions?${params}`);
  if (!r.ok) throw new Error(`/api/positions: ${r.status}`);
  return r.json();
}

/**
 * GET /api/cells — serving-cell history, chronological. Each row: mcc/mnc/
 * tac/cell_id/rsrp_dbm/act (3GPP: 7 LTE-M, 9 NB-IoT, 0 unknown) + resolved
 * lat/lon/acc (null when the tower DB couldn't place it).
 */
export async function fetchCells(deviceId, opts = {}) {
  const params = new URLSearchParams({ device: deviceId });
  if (opts.from_ts != null) params.set('from_ts', opts.from_ts);
  if (opts.to_ts   != null) params.set('to_ts',   opts.to_ts);
  if (opts.limit   != null) params.set('limit',   opts.limit);

  const r = await fetch(`/api/cells?${params}`);
  if (!r.ok) throw new Error(`/api/cells: ${r.status}`);
  return r.json();
}

/**
 * GET /api/logs — device log lines, chronological.
 * min_level uses Zephyr numbering (1=ERR … 4=DBG); lower = more severe.
 */
/**
 * GET /api/usage — per-UTC-day data usage from the ingest ledger.
 */
export async function fetchUsage(deviceId, days = 14) {
  const params = new URLSearchParams({ device: deviceId, days });
  const r = await fetch(`/api/usage?${params}`);
  if (!r.ok) throw new Error(`/api/usage: ${r.status}`);
  return r.json();
}

export async function fetchLogs(deviceId, opts = {}) {
  const params = new URLSearchParams({ device: deviceId });
  if (opts.from_ts   != null) params.set('from_ts',   opts.from_ts);
  if (opts.to_ts     != null) params.set('to_ts',     opts.to_ts);
  if (opts.min_level != null) params.set('min_level', opts.min_level);
  if (opts.limit     != null) params.set('limit',     opts.limit);

  const r = await fetch(`/api/logs?${params}`);
  if (!r.ok) throw new Error(`/api/logs: ${r.status}`);
  return r.json();
}

/**
 * GET /api/events — events (something happened), newest first. Each row
 * carries the nearest position (lat/lon) at that time, or null.
 */
export async function fetchEvents(deviceId, opts = {}) {
  const params = new URLSearchParams({ device: deviceId });
  if (opts.from_ts != null) params.set('from_ts', opts.from_ts);
  if (opts.to_ts   != null) params.set('to_ts',   opts.to_ts);
  if (opts.limit   != null) params.set('limit',   opts.limit);

  const r = await fetch(`/api/events?${params}`);
  if (!r.ok) throw new Error(`/api/events: ${r.status}`);
  return r.json();
}

/**
 * POST /api/arm — arm/disarm a device's motion alerts. Returns {device, armed}.
 */
export async function setArm(deviceId, armed) {
  const r = await fetch('/api/arm', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ device: deviceId, armed: !!armed }),
  });
  if (!r.ok) throw new Error(`/api/arm: ${r.status}`);
  return r.json();
}
