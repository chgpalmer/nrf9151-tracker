/**
 * format.js — display helpers
 */

/**
 * Format unix epoch (seconds) as local time string
 */
export function fmtTime(epochSec) {
  if (epochSec == null) return 'n/a';
  return new Date(epochSec * 1000).toLocaleTimeString([], {
    hour: '2-digit', minute: '2-digit', second: '2-digit',
  });
}

export function fmtDateTime(epochSec) {
  if (epochSec == null) return 'n/a';
  const d = new Date(epochSec * 1000);
  return d.toLocaleDateString([], { month: 'short', day: 'numeric' })
    + ' ' + d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
}

/**
 * Human-readable age: "3s ago", "2m ago", "1h 4m ago"
 */
export function fmtAge(epochSec) {
  if (epochSec == null) return 'n/a';
  const secs = Math.floor(Date.now() / 1000) - epochSec;
  if (secs < 0) return 'just now';
  if (secs < 60) return `${secs}s ago`;
  const m = Math.floor(secs / 60);
  if (m < 60) return `${m}m ago`;
  const h = Math.floor(m / 60);
  const rm = m % 60;
  return `${h}h ${rm}m ago`;
}

/**
 * m/s → km/h, 1 decimal place
 */
export function mpsToKph(mps) {
  if (mps == null) return null;
  return (mps * 3.6).toFixed(1);
}

/**
 * Degrees to compass bearing label
 */
export function hdgToLabel(deg) {
  if (deg == null) return null;
  const dirs = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
  return dirs[Math.round(deg / 45) % 8];
}

/**
 * Accuracy: format with appropriate unit
 */
export function fmtAcc(metres) {
  if (metres == null) return 'n/a';
  if (metres >= 1000) return `${(metres / 1000).toFixed(1)} km`;
  return `${Math.round(metres)} m`;
}

/**
 * Determine status from last_seen epoch
 * <30s → online, <5min → stale, else → offline
 */
export function deviceStatus(lastSeenEpoch) {
  if (!lastSeenEpoch) return 'offline';
  const age = Date.now() / 1000 - lastSeenEpoch;
  if (age < 30)  return 'online';
  if (age < 300) return 'stale';
  return 'offline';
}

/**
 * Status label
 */
export function statusLabel(status) {
  return { online: 'ONLINE', stale: 'STALE', offline: 'OFFLINE' }[status] ?? 'OFFLINE';
}

/**
 * Coordinate: format to 6 decimal places
 */
export function fmtCoord(v) {
  if (v == null) return 'n/a';
  return v.toFixed(6);
}

/**
 * Altitude
 */
export function fmtAlt(v) {
  if (v == null) return 'n/a';
  return `${Math.round(v)} m`;
}

/**
 * Heading
 */
export function fmtHdg(deg) {
  if (deg == null) return 'n/a';
  return `${Math.round(deg)}° ${hdgToLabel(deg)}`;
}
