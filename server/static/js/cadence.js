/**
 * cadence.js — the device build's contact contract, mirrored by hand.
 *
 * Settings don't sync to the device yet (downlink waits on DTLS-PSK), so
 * these mirror the firmware's Kconfig defaults — change them together:
 *   LIVE_FLUSH_S   CONFIG_TRACKER_FLUSH_INTERVAL_S      (moving)
 *   KEEPALIVE_S    CONFIG_TRACKER_QUIESCENT_FLUSH_S and
 *                  CONFIG_TRACKER_REST_HEARTBEAT_S      (parked)
 *
 * KEEPALIVE_S is the device's worst-case silence: parked it either
 * flushes the uplink (QUIESCENT) or heartbeats a cell fix (REST) at this
 * interval, so a healthy device is never quiet longer — whatever mode
 * it is in. Device status derives from it (format.js deviceStatus).
 */
export const LIVE_FLUSH_S = 120;
export const KEEPALIVE_S = 1200;
