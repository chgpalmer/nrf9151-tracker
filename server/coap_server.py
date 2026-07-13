#!/usr/bin/env python3
"""
CoAP → SQLite ingest.

Receives non-confirmable POSTs on /obs (UDP 5683) carrying protobuf-encoded
observations (see proto/tracker.proto) and stores position fixes. The wire
format is defined once, in that .proto: the firmware encodes with nanopb and
this server decodes with the generated tracker_pb2, so the two cannot drift
apart silently. The server stamps its own receive time (received_ts, Unix
epoch); each entry carries its age-at-send, so absolute time is received_ts
minus that age and no device clock can corrupt history.

A track segment is an anchor fix plus fixed-cadence delta arrays: the server
reconstructs absolute positions by accumulating the deltas, timestamps point i
at received_ts - age + i*dt_ms, takes speed from the GNSS Doppler array, and
derives heading from the delta bearing.

Devices send the RFC 7967 No-Response option: replying would page the tracker
after RAI already released its radio connection, costing it the whole saving.
aiocoap honours the option, so a well-formed report produces no downlink.

Usage:
  python3 server/coap_server.py [--port PORT] [--db DB] [--cells-db DB]
"""

import argparse
import asyncio
import math
import sqlite3
import sys
import time
from pathlib import Path

import aiocoap
import aiocoap.resource as resource

import tracker_pb2 as pb
from agnss import AgnssCache, assemble

DB_DEFAULT = Path(__file__).parent / "tracker.db"

# Local OpenCelliD lookup DB (built by build_cells.py from *.csv.gz exports).
CELLS_DB_DEFAULT = Path(__file__).parent / "cells.db"


def init_db(path: Path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(path), check_same_thread=False)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS positions (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id   TEXT    NOT NULL,
            received_ts REAL    NOT NULL,
            source      TEXT    NOT NULL DEFAULT 'gps',
            lat REAL, lon REAL, alt REAL, acc REAL,
            spd REAL, hdg REAL, sats INTEGER
        )
    """)
    conn.execute("""
        CREATE INDEX IF NOT EXISTS idx_pos_dev_time
        ON positions(device_id, received_ts)
    """)
    # Log lines (level uses Zephyr numbering: 1=ERR 2=WRN 3=INF 4=DBG).
    # origin 'device' = shipped over the uplink; 'server' = this process's
    # own events (slog), stored so anomaly investigations can correlate
    # them — device_id then names the device the event concerns, or
    # '_server' for global events (startup, cache refresh).
    conn.execute("""
        CREATE TABLE IF NOT EXISTS logs (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id   TEXT    NOT NULL,
            received_ts REAL    NOT NULL,
            level       INTEGER NOT NULL,
            module      TEXT,
            text        TEXT,
            origin      TEXT    NOT NULL DEFAULT 'device'
        )
    """)
    conn.execute("""
        CREATE INDEX IF NOT EXISTS idx_log_dev_time
        ON logs(device_id, received_ts)
    """)
    # Serving-cell history: who the device was connected to over time — a
    # connectivity record, deliberately separate from the location timeline.
    # EVERY reported cell lands here, including ones cells.db can't place
    # (lat/lon/acc NULL — before 2026-07-13 those were dropped entirely).
    # act = 3GPP access technology (7 LTE-M, 9 NB-IoT, 0 = old firmware).
    conn.execute("""
        CREATE TABLE IF NOT EXISTS cell_events (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id   TEXT    NOT NULL,
            received_ts REAL    NOT NULL,
            mcc INTEGER, mnc INTEGER, tac INTEGER, cell_id INTEGER,
            rsrp_dbm INTEGER, act INTEGER,
            lat REAL, lon REAL, acc REAL
        )
    """)
    conn.execute("""
        CREATE INDEX IF NOT EXISTS idx_cell_dev_time
        ON cell_events(device_id, received_ts)
    """)
    # One row per datagram: the device's data-usage ledger. bytes is the CoAP
    # payload length (kind 'agnss' counts request + response — downlink bills
    # too); the API adds per-datagram framing when estimating on-air cost.
    conn.execute("""
        CREATE TABLE IF NOT EXISTS usage (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id   TEXT    NOT NULL,
            received_ts REAL    NOT NULL,
            bytes       INTEGER NOT NULL,
            kind        TEXT    NOT NULL DEFAULT 'obs'
        )
    """)
    conn.execute("""
        CREATE INDEX IF NOT EXISTS idx_usage_dev_time
        ON usage(device_id, received_ts)
    """)
    # Migrate older DBs created before the source column existed.
    cols = {r[1] for r in conn.execute("PRAGMA table_info(positions)")}
    if "source" not in cols:
        conn.execute(
            "ALTER TABLE positions ADD COLUMN source TEXT NOT NULL DEFAULT 'gps'")
    # ...and before logs grew the origin column (2026-07-13).
    cols = {r[1] for r in conn.execute("PRAGMA table_info(logs)")}
    if "origin" not in cols:
        conn.execute(
            "ALTER TABLE logs ADD COLUMN origin TEXT NOT NULL DEFAULT 'device'")
    conn.commit()
    return conn


def slog(conn, level, module, text, device="_server"):
    """Server-side log: printed (serve.log keeps working) AND stored with
    origin='server' so `scripts/anomaly-report.py at` and the webapp can
    correlate server events with device logs and positions. Not for
    per-datagram chatter — the usage table already ledgers arrivals."""
    print(f"[{module}] {text}", file=sys.stderr if level <= 2 else sys.stdout)
    conn.execute(
        """INSERT INTO logs (device_id, received_ts, level, module, text, origin)
           VALUES (?, ?, ?, ?, ?, 'server')""",
        (device, time.time(), level, module, text))
    conn.commit()


def _haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Great-circle distance in metres."""
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


def _bearing(dlat_e7: int, dlon_e7: int, lat_deg: float) -> float | None:
    """Compass heading (degrees, 0=N) implied by a position delta. Returns None
    for a zero delta (a stationary point has no direction). dlon is scaled by
    cos(lat) because a degree of longitude shrinks toward the poles."""
    if dlat_e7 == 0 and dlon_e7 == 0:
        return None
    north = dlat_e7
    east = dlon_e7 * math.cos(math.radians(lat_deg))
    return math.degrees(math.atan2(east, north)) % 360.0


def resolve_cell(mcc, mnc, tac, cid, cells: sqlite3.Connection | None):
    """Resolve a serving cell to (lat, lon, accuracy, how) via the local
    OpenCelliD DB. Returns None on miss or if no DB is loaded.

    Two strategies, in order:

    'exact'  the full mcc/net/area/cell 4-tuple. Best, but OpenCelliD is
             crowdsourced and sparse: a given sector is often absent, and
             operators re-plan tracking areas, so a live TAC may not match the
             recorded one even when the tower itself is well surveyed.

    'enb'    LTE only. An LTE cell id is (eNB << 8) | sector, so we drop the
             sector and the TAC and average the sectors we do have for that
             eNB. Accuracy becomes the sector spread, since we know the tower,
             not which face of it is serving us.
    """
    if cells is None:
        return None

    row = cells.execute(
        "SELECT lat, lon, range FROM cells "
        "WHERE mcc=? AND net=? AND area=? AND cell=?",
        (mcc, mnc, tac, cid),
    ).fetchone()
    if row is not None:
        lat, lon, rng = row
        # OpenCelliD 'range' is a coverage radius estimate; use it as accuracy,
        # falling back to a coarse default when absent.
        return lat, lon, float(rng) if rng else 2000.0, "exact"

    enb = cid >> 8
    rows = cells.execute(
        "SELECT lat, lon, range FROM cells "
        "WHERE mcc=? AND net=? AND cell BETWEEN ? AND ? AND radio='LTE'",
        (mcc, mnc, enb << 8, (enb << 8) | 0xFF),
    ).fetchall()
    if not rows:
        return None

    lat = sum(r[0] for r in rows) / len(rows)
    lon = sum(r[1] for r in rows) / len(rows)
    # Accuracy: far enough out to cover every sector we averaged, but never
    # tighter than the towers' own claimed coverage radius.
    spread = max(_haversine_m(lat, lon, r[0], r[1]) for r in rows)
    rng = max((r[2] or 0) for r in rows)
    return lat, lon, max(spread, rng, 2000.0), f"enb {enb}, {len(rows)} sectors"


def _insert_gps(conn, dev, ts, lat, lon, alt, acc, spd, hdg, sats):
    conn.execute(
        """INSERT INTO positions
           (device_id, received_ts, source, lat, lon, alt, acc, spd, hdg, sats)
           VALUES (?, ?, 'gps', ?, ?, ?, ?, ?, ?, ?)""",
        (dev, ts, lat, lon, alt, acc, spd, hdg, sats))


def store_track(conn, dev, base_ts, seg) -> int:
    """Expand one TrackSegment into per-point GPS rows. Returns the count.

    Point 0 is the absolute anchor; point i+1 = point i + (dlat[i], dlon[i]).
    Time is base_ts + i*dt_ms. Speed comes from the Doppler array (deriving it
    from the deltas is too noisy at 1 Hz+ to use). alt/acc/sats are carried from
    the anchor — they barely change across a <=50 s segment and we don't spend
    wire bytes resending them. Heading is derived from each delta's bearing."""
    a = seg.anchor
    n = len(seg.dlat)
    if len(seg.dlon) != n:
        slog(conn, 2, "coap", f"track: dlat/dlon length mismatch "
             f"({n}/{len(seg.dlon)}) — dropped", device=dev)
        return 0
    # (0,0) is the Gulf of Guinea, not this tracker: an unfixed PVT frame used
    # to leak it. Drop the whole segment if its anchor is (0,0).
    if a.lat_e7 == 0 and a.lon_e7 == 0:
        slog(conn, 2, "coap", "track: dropped (0,0) anchor — unfixed frame?",
             device=dev)
        return 0

    dt = seg.dt_ms / 1000.0
    alt = a.alt_dm / 10.0
    acc = a.acc_dm / 10.0
    sats = a.sats
    lat_e7, lon_e7 = a.lat_e7, a.lon_e7

    # Anchor (point 0): its own speed/heading come from the fix itself.
    _insert_gps(conn, dev, base_ts, lat_e7 / 1e7, lon_e7 / 1e7, alt, acc,
                a.spd_dms / 10.0, a.hdg_ddeg / 10.0, sats)
    count = 1

    for i in range(n):
        hdg = _bearing(seg.dlat[i], seg.dlon[i], lat_e7 / 1e7)
        lat_e7 += seg.dlat[i]
        lon_e7 += seg.dlon[i]
        ts = base_ts + (i + 1) * dt
        spd = seg.spd_dms[i] / 10.0 if i < len(seg.spd_dms) else None
        _insert_gps(conn, dev, ts, lat_e7 / 1e7, lon_e7 / 1e7, alt, acc,
                    spd, hdg, sats)
        count += 1
    return count


def store_obs(conn, obs, cells, now: int) -> tuple[int, int, int]:
    """Store a decoded Obs. Returns (track_points, cell_fixes, log_lines)."""
    dev = obs.device_id
    n_pts = n_cell = n_log = 0
    for e in obs.entries:
        base_ts = now - e.age_s
        kind = e.WhichOneof("kind")
        if kind == "track":
            n_pts += store_track(conn, dev, base_ts, e.track)
        elif kind == "log":
            for line in e.log.lines:
                conn.execute(
                    """INSERT INTO logs (device_id, received_ts, level, module, text)
                       VALUES (?, ?, ?, ?, ?)""",
                    (dev, now - line.age_s, line.level, line.module, line.text))
                n_log += 1
        elif kind == "cell":
            c = e.cell
            fix = resolve_cell(c.mcc, c.mnc, c.tac, c.cell_id, cells)
            lat = lon = acc = None
            if fix is None:
                slog(conn, 2, "coap", f"cell unresolved (mcc={c.mcc} "
                     f"mnc={c.mnc} tac={c.tac} cid={c.cell_id})", device=dev)
            else:
                lat, lon, acc, how = fix
            # Connectivity history first: recorded whether or not the tower
            # is in cells.db — "who was I connected to" outlives "where".
            conn.execute(
                """INSERT INTO cell_events (device_id, received_ts, mcc, mnc,
                   tac, cell_id, rsrp_dbm, act, lat, lon, acc)
                   VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
                (dev, base_ts, c.mcc, c.mnc, c.tac, c.cell_id,
                 c.rsrp_dbm, c.act, lat, lon, acc))
            if fix is None:
                continue
            conn.execute(
                """INSERT INTO positions
                   (device_id, received_ts, source, lat, lon, alt, acc, spd, hdg, sats)
                   VALUES (?, ?, 'cell', ?, ?, NULL, ?, NULL, NULL, NULL)""",
                (dev, base_ts, lat, lon, acc))
            n_cell += 1
    conn.commit()
    print(f"[coap] {dev} v{obs.version}: {n_pts} track pts + {n_cell} cell "
          f"+ {n_log} log")
    return n_pts, n_cell, n_log


class ObsResource(resource.Resource):
    def __init__(self, conn, cells):
        super().__init__()
        self.conn = conn
        self.cells = cells

    async def render_post(self, request):
        try:
            obs = pb.Obs.FromString(request.payload)
        except Exception as e:
            slog(self.conn, 1, "coap",
                 f"rejected payload ({len(request.payload)} B): {e}")
            return aiocoap.Message(code=aiocoap.Code.BAD_REQUEST)

        store_obs(self.conn, obs, self.cells, int(time.time()))
        self.conn.execute(
            "INSERT INTO usage (device_id, received_ts, bytes, kind) "
            "VALUES (?, ?, ?, 'obs')",
            (obs.device_id, time.time(), len(request.payload)))
        self.conn.commit()

        # Devices set No-Response, so aiocoap drops this on the floor for
        # them; it still answers curious human clients (coap-client etc).
        return aiocoap.Message(code=aiocoap.Code.CHANGED)


class AgnssResource(resource.Resource):
    """POST /agnss: AgnssRequest in, AgnssData out — the system's one
    request/response exchange. The device's last DB position drives the
    elevation filter and the coarse-location seed."""

    def __init__(self, conn, cache):
        super().__init__()
        self.conn = conn
        self.cache = cache

    def _last_pos(self, dev):
        row = self.conn.execute(
            """SELECT lat, lon, acc, received_ts FROM positions
               WHERE device_id=? AND lat IS NOT NULL
               ORDER BY received_ts DESC LIMIT 1""", (dev,)).fetchone()
        if row is None:
            return None
        lat, lon, acc, ts = row
        # Inflate uncertainty with age: parked it's honest, driven it still
        # bounds the search (100 km covers ~1 h of driving).
        age_h = max(0.0, (time.time() - ts) / 3600.0)
        unc = min(1_000_000.0, (acc or 2000.0) + age_h * 100_000.0)
        return lat, lon, unc

    async def render_post(self, request):
        try:
            req = pb.AgnssRequest.FromString(request.payload)
        except Exception as e:
            slog(self.conn, 1, "agnss",
                 f"rejected request ({len(request.payload)} B): {e}")
            return aiocoap.Message(code=aiocoap.Code.BAD_REQUEST)
        payload = assemble(self.cache, self._last_pos(req.device_id))
        data = pb.AgnssData.FromString(payload)
        slog(self.conn, 3, "agnss",
             f"served {len(data.ephemeris)} ephe, {len(payload)} B "
             f"(flags 0x{req.data_flags:02x})", device=req.device_id)
        self.conn.execute(
            "INSERT INTO usage (device_id, received_ts, bytes, kind) "
            "VALUES (?, ?, ?, 'agnss')",
            (req.device_id, time.time(),
             len(request.payload) + len(payload)))
        self.conn.commit()
        return aiocoap.Message(code=aiocoap.Code.CONTENT, payload=payload)


async def agnss_refresh(cache, conn):
    """Keep the ephemeris cache ~15-30 min fresh, forever. Failures keep the
    last good data (assistance is an accelerator, never a dependency)."""
    while True:
        try:
            await asyncio.get_running_loop().run_in_executor(None, cache.fetch)
        except Exception as exc:
            slog(conn, 2, "agnss", f"refresh error: {exc}")
        await asyncio.sleep(1800)


async def serve(args):
    conn = init_db(Path(args.db))
    print(f"[coap] db: {args.db}")

    cells = None
    if Path(args.cells_db).exists():
        cells = sqlite3.connect(f"file:{args.cells_db}?mode=ro", uri=True,
                                check_same_thread=False)
        n = cells.execute("SELECT COUNT(*) FROM cells").fetchone()[0]
        print(f"[coap] cell DB: {args.cells_db} ({n} cells)")
    else:
        slog(conn, 2, "coap", f"no cell DB at {args.cells_db} — cell fixes "
             f"won't resolve (run: make cells)")

    cache = AgnssCache(persist=Path(args.db).parent / "agnss_cache.json")
    asyncio.create_task(agnss_refresh(cache, conn))

    site = resource.Site()
    site.add_resource(["obs"], ObsResource(conn, cells))
    site.add_resource(["agnss"], AgnssResource(conn, cache))

    await aiocoap.Context.create_server_context(site, bind=("::", args.port))
    # The one INF every start: a restart marker in every anomaly report.
    slog(conn, 3, "coap",
         f"listening on UDP {args.port} (POST /obs + /agnss, protobuf)")
    await asyncio.get_running_loop().create_future()  # run forever


def main():
    parser = argparse.ArgumentParser(description="CoAP → SQLite ingest")
    parser.add_argument("--port", default=5683, type=int)
    parser.add_argument("--db", default=str(DB_DEFAULT))
    parser.add_argument("--cells-db", default=str(CELLS_DB_DEFAULT),
                        help="OpenCelliD lookup DB (build with build_cells.py)")
    args = parser.parse_args()

    try:
        asyncio.run(serve(args))
    except KeyboardInterrupt:
        print("\n[coap] stopped")


if __name__ == "__main__":
    main()
