#!/usr/bin/env python3
"""Seed a DB with a deterministic test day: two 1 Hz trips + cell keepalives.

Used by `make webtest` to exercise the web UI (trip chips, timeline bands,
charts, table) against known data. Writes to the path given as argv[1].
"""

import math
import sqlite3
import sys
import time

db = sqlite3.connect(sys.argv[1])
db.execute("""CREATE TABLE IF NOT EXISTS positions (
    id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT NOT NULL,
    received_ts REAL NOT NULL, source TEXT NOT NULL DEFAULT 'gps',
    lat REAL, lon REAL, alt REAL, acc REAL, spd REAL, hdg REAL, sats INTEGER)""")
db.execute("DELETE FROM positions")

now = time.time()
dev = "web-test"


def trip(t0, secs, lat0, lon0, spd=4.0):
    rows = []
    for s in range(secs):
        ang = s / 60.0
        lat = lat0 + s * spd * math.cos(ang) / 111320
        lon = lon0 + s * spd * math.sin(ang) / 70000
        rows.append((dev, t0 + s, 'gps', lat, lon, 30.0, 6.0 + (s % 5),
                     spd + math.sin(s / 10) * 1.5, (ang * 57) % 360, 8))
    return rows


rows = []
rows += trip(now - 3 * 3600, 300, 51.505, -0.09)           # trip 1: 5 min
rows += [(dev, now - 3 * 3600 + 900, 'cell', 51.507, -0.088,
          None, 1500.0, None, None, None)]
rows += trip(now - 2 * 3600, 420, 51.508, -0.086, spd=6)   # trip 2: 7 min
rows += [(dev, now - 600, 'cell', 51.509, -0.084, None, 1200.0, None, None, None)]

db.executemany(
    "INSERT INTO positions (device_id, received_ts, source, lat, lon, alt, acc,"
    " spd, hdg, sats) VALUES (?,?,?,?,?,?,?,?,?,?)", rows)

# Device log lines (level: 1=ERR 2=WRN 3=INF 4=DBG) for the Logs page.
db.execute("""CREATE TABLE IF NOT EXISTS logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT NOT NULL,
    received_ts REAL NOT NULL, level INTEGER NOT NULL, module TEXT, text TEXT)""")
db.execute("DELETE FROM logs")
log_rows = [
    (dev, now - 3 * 3600 - 90, 3, 'tracker', 'tracker starting'),
    (dev, now - 3 * 3600 - 5, 3, 'tracker', 'LTE registered (roaming)'),
    (dev, now - 3 * 3600, 3, 'loc_fsm', 'loc: LTE_ATTACH -> REPORT_CELL (registered; after 85 s)'),
    (dev, now - 2 * 3600 + 60, 2, 'obs_queue', 'gps ring full — dropping oldest'),
    (dev, now - 600, 1, 'coap_pub', 'send: errno 111 (Connection refused)'),
    (dev, now - 300, 4, 'uplink', 'sent 61 B (kinds 0x2)'),
]
db.executemany(
    "INSERT INTO logs (device_id, received_ts, level, module, text)"
    " VALUES (?,?,?,?,?)", log_rows)
db.commit()
print(f"seeded {len(rows)} positions + {len(log_rows)} logs for {dev} into {sys.argv[1]}")
