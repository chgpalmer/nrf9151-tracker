#!/usr/bin/env python3
"""
CoAP → SQLite ingest.

Receives non-confirmable POSTs on /obs (UDP 5683) carrying CBOR observations
and stores position fixes. The wire format is defined once, in
proto/tracker.cddl: this server decodes and validates against that same file
via the zcbor package, so firmware and server cannot drift apart silently.
The server stamps its own receive time (received_ts, Unix epoch).

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
from zcbor import DataTranslator, CddlValidationError

DB_DEFAULT = Path(__file__).parent / "tracker.db"

# Local OpenCelliD lookup DB (built by build_cells.py from *.csv.gz exports).
CELLS_DB_DEFAULT = Path(__file__).parent / "cells.db"

CDDL_PATH = Path(__file__).parent.parent / "proto" / "tracker.cddl"


def init_db(path: Path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(path), check_same_thread=False)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS positions (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id   TEXT    NOT NULL,
            received_ts INTEGER NOT NULL,
            source      TEXT    NOT NULL DEFAULT 'gps',
            lat REAL, lon REAL, alt REAL, acc REAL,
            spd REAL, hdg REAL, sats INTEGER
        )
    """)
    conn.execute("""
        CREATE INDEX IF NOT EXISTS idx_pos_dev_time
        ON positions(device_id, received_ts)
    """)
    # Migrate older DBs created before the source column existed.
    cols = {r[1] for r in conn.execute("PRAGMA table_info(positions)")}
    if "source" not in cols:
        conn.execute(
            "ALTER TABLE positions ADD COLUMN source TEXT NOT NULL DEFAULT 'gps'")
    conn.commit()
    return conn


def handle_gps(conn: sqlite3.Connection, g, ts: int) -> None:
    """Store one GPS entry (already schema-validated). Wire units are scaled
    integers (see tracker.cddl); the DB keeps the human units the web UI
    already expects: degrees, metres, m/s. ts is the observation time
    (server receive time minus the entry's age)."""
    received_ts = ts
    # (0, 0) is the Gulf of Guinea, not this tracker: it is what an unfixed PVT
    # frame used to leak before the firmware published from a fix-valid
    # snapshot. Drop it defensively so a regression can't dirty the map again.
    if g.lat_e7_m == 0 and g.lon_e7_m == 0:
        print(f"[coap] {g.device_id_m} gps  dropped (0,0) — unfixed frame?",
              file=sys.stderr)
        return
    conn.execute(
        """INSERT INTO positions
           (device_id, received_ts, source, lat, lon, alt, acc, spd, hdg, sats)
           VALUES (?, ?, 'gps', ?, ?, ?, ?, ?, ?, ?)""",
        (
            g.device_id_m, received_ts,
            g.lat_e7_m / 1e7, g.lon_e7_m / 1e7,
            g.alt_dm_m / 10.0, g.acc_dm_m / 10.0,
            g.spd_dms_m / 10.0, g.hdg_ddeg_m / 10.0, g.sats_m,
        ),
    )
    conn.commit()
    print(f"[coap] {g.device_id_m} gps  {g.lat_e7_m / 1e7:.6f}, "
          f"{g.lon_e7_m / 1e7:.6f}  acc {g.acc_dm_m / 10.0:.1f}m  @{received_ts}")


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


def _haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Great-circle distance in metres."""
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


def handle_cell(conn: sqlite3.Connection, c, ts: int,
                cells: sqlite3.Connection | None) -> None:
    received_ts = ts

    fix = resolve_cell(c.mcc_m, c.mnc_m, c.tac_m, c.cell_id_m, cells)
    if fix is None:
        print(f"[coap] {c.device_id_m} cell  unresolved "
              f"(mcc={c.mcc_m} mnc={c.mnc_m} tac={c.tac_m} cid={c.cell_id_m})")
        return

    lat, lon, acc, how = fix
    conn.execute(
        """INSERT INTO positions
           (device_id, received_ts, source, lat, lon, alt, acc, spd, hdg, sats)
           VALUES (?, ?, 'cell', ?, ?, NULL, ?, NULL, NULL, NULL)""",
        (c.device_id_m, received_ts, lat, lon, acc),
    )
    conn.commit()
    print(f"[coap] {c.device_id_m} cell  {lat:.6f}, {lon:.6f}  "
          f"acc {acc:.0f}m ({how})")


def handle_batch(conn: sqlite3.Connection, b,
                 cells: sqlite3.Connection | None) -> None:
    """Store a v2 batch: entries carry age-at-send; absolute time is the
    server receive stamp minus that age (worst error = uplink latency, and no
    device clock can ever corrupt history). A repeat entry re-asserts the
    previous GPS position at a new time; the chain seeds from the batch or,
    for a leading repeat, from the device's last stored GPS row."""
    now = int(time.time())
    dev = b.device_id_m
    prev_gps = None  # (lat, lon, alt, acc, spd, hdg, sats) in DB units

    row = conn.execute(
        "SELECT lat, lon, alt, acc, spd, hdg, sats FROM positions "
        "WHERE device_id=? AND source='gps' ORDER BY received_ts DESC LIMIT 1",
        (dev,)).fetchone()
    if row:
        prev_gps = tuple(row)

    n_gps = n_rep = n_cell = 0
    for e in b.entry_m_l.entry_m:
        kind = e.union_choice
        if kind == "gps_entry_m":
            g = e.gps_entry_m
            ts = now - g.age_s_m
            lat, lon = g.lat_e7_m / 1e7, g.lon_e7_m / 1e7
            if g.lat_e7_m == 0 and g.lon_e7_m == 0:
                continue  # unfixed-frame leak; see handle_gps
            vals = (lat, lon, g.alt_dm_m / 10.0, g.acc_dm_m / 10.0,
                    g.spd_dms_m / 10.0, g.hdg_ddeg_m / 10.0, g.sats_m)
            conn.execute(
                """INSERT INTO positions
                   (device_id, received_ts, source, lat, lon, alt, acc, spd, hdg, sats)
                   VALUES (?, ?, 'gps', ?, ?, ?, ?, ?, ?, ?)""",
                (dev, ts) + vals)
            prev_gps = vals
            n_gps += 1
        elif kind == "repeat_entry_m":
            ts = now - e.repeat_entry_m.age_s_m
            if prev_gps is None:
                print(f"[coap] {dev} repeat with no prior gps — skipped",
                      file=sys.stderr)
                continue
            conn.execute(
                """INSERT INTO positions
                   (device_id, received_ts, source, lat, lon, alt, acc, spd, hdg, sats)
                   VALUES (?, ?, 'gps', ?, ?, ?, ?, ?, ?, ?)""",
                (dev, ts) + prev_gps)
            n_rep += 1
        else:
            c = e.cell_entry_m
            ts = now - c.age_s_m
            fix = resolve_cell(c.mcc_m, c.mnc_m, c.tac_m, c.cell_id_m, cells)
            if fix is None:
                print(f"[coap] {dev} cell unresolved "
                      f"(mcc={c.mcc_m} mnc={c.mnc_m} tac={c.tac_m} "
                      f"cid={c.cell_id_m})")
                continue
            lat, lon, acc, how = fix
            conn.execute(
                """INSERT INTO positions
                   (device_id, received_ts, source, lat, lon, alt, acc, spd, hdg, sats)
                   VALUES (?, ?, 'cell', ?, ?, NULL, ?, NULL, NULL, NULL)""",
                (dev, ts, lat, lon, acc))
            n_cell += 1
    conn.commit()
    print(f"[coap] {dev} batch: {n_gps} gps + {n_rep} repeat + {n_cell} cell")


class ObsResource(resource.Resource):
    def __init__(self, conn, cells, observation):
        super().__init__()
        self.conn = conn
        self.cells = cells
        self.observation = observation

    async def render_post(self, request):
        try:
            obs = self.observation.decode_str(request.payload)
        except (CddlValidationError, Exception) as e:
            print(f"[coap] rejected payload ({len(request.payload)} B): {e}",
                  file=sys.stderr)
            return aiocoap.Message(code=aiocoap.Code.BAD_REQUEST)

        now = int(time.time())
        if obs.union_choice == "batch_m":
            handle_batch(self.conn, obs.batch_m, self.cells)
        elif obs.union_choice == "gps_obs_m":
            handle_gps(self.conn, obs.gps_obs_m, now)
        else:
            handle_cell(self.conn, obs.cell_obs_m, now, self.cells)

        # Devices set No-Response, so aiocoap drops this on the floor for
        # them; it still answers curious human clients (coap-client etc).
        return aiocoap.Message(code=aiocoap.Code.CHANGED)


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
        print(f"[coap] no cell DB at {args.cells_db} — cell fixes won't "
              f"resolve (run: make cells)")

    # One schema for both sides: the firmware's encoders are generated from
    # this same file (make proto).
    translator = DataTranslator.from_cddl(CDDL_PATH.read_text(), 16)
    observation = translator.my_types["observation"]
    print(f"[coap] schema: {CDDL_PATH}")

    site = resource.Site()
    site.add_resource(["obs"], ObsResource(conn, cells, observation))

    await aiocoap.Context.create_server_context(site, bind=("::", args.port))
    print(f"[coap] listening on UDP {args.port} (POST /obs)")
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
