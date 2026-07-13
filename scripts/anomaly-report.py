#!/usr/bin/env python3
"""Find and dissect implausible GPS in tracker.db — the recurring field
investigation ("what was that phantom trip?") as one command instead of
hand-written SQL each time.

  scan     find anomaly events: adjacent GPS fixes whose implied speed is
           impossible, grouped into events with peak jump / duration
  at       correlated report around a moment: positions (with distance from
           the local anchor and implied speeds), device logs, server logs
           (device_id '_server', once those exist), and usage rows

Read-only. Works on any tracker.db, including over ssh:
  python3 scripts/anomaly-report.py server/tracker.db scan --days 2
  python3 scripts/anomaly-report.py server/tracker.db at "2026-07-13 04:12:27"

The webapp has no anomaly surface yet; this is the searchable half Charlie
and Claude share. Detection thresholds are deliberately loose constants —
retune here, not in a schema.
"""
import argparse
import math
import sqlite3
import statistics
import sys
import time
from datetime import datetime, timezone

# An adjacent-fix pair is implausible when the device would have had to move
# faster than any ride (SPEED_KMH) AND further than GNSS jitter explains
# (MIN_JUMP_M). 1 Hz jitter of 10 m reads as 36 km/h — the jump floor is
# what keeps honest noise out.
SPEED_KMH  = 150.0
MIN_JUMP_M = 150.0
MAX_GAP_S  = 130.0   # pairs across longer gaps prove nothing (parked checks)
GROUP_S    = 600.0   # flags within 10 min are one event


def utc(ts):
    return datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


def dist_m(lat1, lon1, lat2, lon2):
    return math.hypot((lat1 - lat2) * 111320.0,
                      (lon1 - lon2) * 111320.0 * math.cos(math.radians(lat1)))


def fetch_gps(db, dev, t0, t1):
    return db.execute(
        """SELECT received_ts, lat, lon, acc, spd, sats FROM positions
           WHERE device_id = ? AND source = 'gps'
           AND received_ts BETWEEN ? AND ? ORDER BY received_ts""",
        (dev, t0, t1)).fetchall()


def devices(db):
    return [r[0] for r in db.execute(
        "SELECT DISTINCT device_id FROM positions").fetchall()]


def scan(db, args):
    t1 = time.time()
    t0 = t1 - args.days * 86400
    total = 0
    for dev in devices(db):
        rows = fetch_gps(db, dev, t0, t1)
        flags = []
        for a, b in zip(rows, rows[1:]):
            dt = b[0] - a[0]
            if dt <= 0 or dt > MAX_GAP_S:
                continue
            d = dist_m(a[1], a[2], b[1], b[2])
            kmh = d / dt * 3.6
            if d >= MIN_JUMP_M and kmh >= SPEED_KMH:
                flags.append((a[0], b[0], d, kmh))
        # group into events
        events = []
        for f in flags:
            if events and f[0] - events[-1][-1][1] <= GROUP_S:
                events[-1].append(f)
            else:
                events.append([f])
        for ev in events:
            total += 1
            peak = max(ev, key=lambda f: f[2])
            print(f"{dev}  {utc(ev[0][0])} .. {utc(ev[-1][1])}  "
                  f"{len(ev)} impossible pair(s), peak jump {peak[2]:.0f} m "
                  f"@ {peak[3]:.0f} km/h implied")
            print(f"    inspect: at \"{utc(peak[0])}\"")
    if not total:
        print(f"no impossible GPS pairs in the last {args.days:g} day(s) "
              f"(> {MIN_JUMP_M:.0f} m and > {SPEED_KMH:.0f} km/h within "
              f"{MAX_GAP_S:.0f} s)")


def report(db, args):
    t = datetime.strptime(args.when, "%Y-%m-%d %H:%M:%S")\
        .replace(tzinfo=timezone.utc).timestamp()
    t0, t1 = t - args.window, t + args.window
    for dev in devices(db):
        rows = fetch_gps(db, dev, t0, t1)
        if not rows:
            continue
        print(f"=== {dev}  {utc(t0)} .. {utc(t1)} ===")

        # Anchor = median of the window (robust: the anomaly is the minority)
        lat0 = statistics.median(r[1] for r in rows)
        lon0 = statistics.median(r[2] for r in rows)
        print(f"--- gps ({len(rows)} fixes; d = from window median "
              f"{lat0:.5f},{lon0:.5f}) ---")
        prev = None
        for ts, lat, lon, acc, spd, sats in rows:
            d = dist_m(lat, lon, lat0, lon0)
            imp = ""
            if prev is not None:
                dt = ts - prev[0]
                if 0 < dt <= MAX_GAP_S:
                    kmh = dist_m(lat, lon, prev[1], prev[2]) / dt * 3.6
                    if kmh >= SPEED_KMH:
                        imp = f"  <<< {kmh:.0f} km/h implied"
            # keep quiet stretches short: print only interesting rows unless -v
            hot = d > 50 or imp or (acc or 0) > 30 or prev is None
            if args.verbose or hot:
                print(f"{utc(ts)[11:]}  d={d:6.0f}m acc={acc or 0:5.1f} "
                      f"spd={(spd or 0) * 3.6:5.1f}km/h sats={sats}{imp}")
            prev = (ts, lat, lon)

        has_origin = any(r[1] == "origin"
                         for r in db.execute("PRAGMA table_info(logs)"))
        if has_origin:
            sections = (
                ("device logs", "origin = 'device' AND device_id = ?", (dev,)),
                ("server logs",
                 "origin = 'server' AND device_id IN (?, '_server')", (dev,)),
            )
        else:  # pre-origin schema: everything in the table is device logs
            sections = (("device logs", "device_id = ?", (dev,)),
                        ("server logs (schema too old)", "0 = 1", ()))
        for label, where, extra in sections:
            logs = db.execute(
                f"""SELECT received_ts, level, module, text FROM logs
                    WHERE {where} AND received_ts BETWEEN ? AND ?
                    ORDER BY received_ts""", (*extra, t0, t1)).fetchall()
            print(f"--- {label} ({len(logs)}) ---")
            for ts, lvl, mod, text in logs:
                tag = "?EWID"[lvl] if lvl and 0 < lvl <= 4 else "?"
                print(f"{utc(ts)[11:]}  <{tag}> {mod}: {text}")

        usage = db.execute(
            """SELECT received_ts, kind, bytes FROM usage
               WHERE device_id = ? AND received_ts BETWEEN ? AND ?
               ORDER BY received_ts""", (dev, t0, t1)).fetchall()
        print(f"--- usage ({len(usage)} datagrams, "
              f"{sum(u[2] for u in usage)} B) ---")
        for ts, kind, nbytes in usage:
            print(f"{utc(ts)[11:]}  {kind:6} {nbytes:5d} B")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("db", help="path to tracker.db")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan", help="find impossible-motion events")
    s.add_argument("--days", type=float, default=2.0,
                   help="how far back to scan (default 2)")
    a = sub.add_parser("at", help="correlated report around a moment (UTC)")
    a.add_argument("when", help='"YYYY-MM-DD HH:MM:SS" (UTC)')
    a.add_argument("--window", type=int, default=300,
                   help="seconds either side (default 300)")
    a.add_argument("-v", "--verbose", action="store_true",
                   help="print every fix, not just the interesting ones")
    args = p.parse_args()

    db = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    if args.cmd == "scan":
        scan(db, args)
    else:
        report(db, args)


if __name__ == "__main__":
    main()
