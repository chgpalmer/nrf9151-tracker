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
 * GET /api/logs — device log lines, chronological.
 * min_level uses Zephyr numbering (1=ERR … 4=DBG); lower = more severe.
 */
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
