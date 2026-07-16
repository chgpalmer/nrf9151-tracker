"""best_current — the live-dot chooser (app.py), tested against a seeded DB.

The rule under test: the last GPS fix outranks newer heartbeat cell fixes
only while the evidence agrees the device never moved — no motion/boot event
after the fix, and every cell since corroborating it (accuracy circle
contains it). Any evidence of movement and the newest row wins: a bike in a
GPS-shielded van must be tracked by its cells, not pinned to the driveway.
"""

import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from app import best_current

DEV = "test-dev"

# ~52.2N: 0.01 deg lat ~= 1105 m. A cell at the GPS spot with acc=2000
# contains it; one 0.05 deg away (~5.5 km) with acc=2000 does not.
LAT, LON = 52.2, 0.12


def _db():
    conn = sqlite3.connect(":memory:")
    conn.row_factory = sqlite3.Row
    conn.execute(
        "CREATE TABLE positions (device_id TEXT, received_ts REAL, "
        "source TEXT, lat REAL, lon REAL, alt REAL, acc REAL, spd REAL, "
        "hdg REAL, sats INTEGER)")
    conn.execute(
        "CREATE TABLE events (device_id TEXT, received_ts REAL, "
        "kind TEXT, reason INTEGER)")
    return conn


def _pos(conn, ts, source, lat=LAT, lon=LON, acc=10.0):
    conn.execute(
        "INSERT INTO positions (device_id, received_ts, source, lat, lon, acc) "
        "VALUES (?, ?, ?, ?, ?, ?)", (DEV, ts, source, lat, lon, acc))


def test_no_positions_is_none():
    assert best_current(_db(), DEV) is None


def test_latest_gps_stands():
    conn = _db()
    _pos(conn, 1000, "gps")
    fix = best_current(conn, DEV)
    assert fix["basis"] == "latest" and fix["source"] == "gps"


def test_parked_prefers_gps_over_corroborating_cells():
    conn = _db()
    _pos(conn, 1000, "gps")
    for t in (2000, 3000, 4000):  # heartbeats at the local tower
        _pos(conn, t, "cell", lat=LAT + 0.005, lon=LON, acc=2000)
    fix = best_current(conn, DEV)
    assert fix["basis"] == "parked-gps"
    assert fix["received_ts"] == 1000
    assert fix["cells_since"] == 3
    assert fix["corroborated_ts"] == 4000


def test_noncorroborating_cell_returns_newest_row():
    """The van: cells marching away from the parked fix = movement the
    track never saw. The newest cell must win, even if a LATER cell circle
    happens to contain the old fix again."""
    conn = _db()
    _pos(conn, 1000, "gps")
    _pos(conn, 2000, "cell", lat=LAT + 0.05, lon=LON, acc=2000)  # ~5.5 km
    fix = best_current(conn, DEV)
    assert fix["basis"] == "latest" and fix["source"] == "cell"


def test_event_after_fix_returns_newest_row():
    """IMU wake (or a reboot) after the fix: the device itself says it
    moved; the parked fix may no longer be where it is."""
    conn = _db()
    _pos(conn, 1000, "gps")
    _pos(conn, 2000, "cell", lat=LAT, lon=LON, acc=2000)  # corroborates
    conn.execute("INSERT INTO events VALUES (?, 1500, 'motion', 1)", (DEV,))
    fix = best_current(conn, DEV)
    assert fix["basis"] == "latest"


def test_cell_only_device_uses_cells():
    conn = _db()
    _pos(conn, 1000, "cell", acc=2000)
    fix = best_current(conn, DEV)
    assert fix["basis"] == "latest" and fix["source"] == "cell"
