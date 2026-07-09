#!/usr/bin/env python3
"""
Build a local cell-tower lookup DB from OpenCelliD CSV exports.

OpenCelliD per-MCC files (e.g. 234.csv.gz for the UK) are headerless CSV:
  radio, mcc, net, area, cell, unit, lon, lat, range, samples,
  changeable, created, updated, averageSignal

We keep the columns needed to resolve a serving cell (mcc, net=mnc,
area=tac, cell=cid) to a coordinate + range (accuracy in metres).

Usage:
  python3 server/build_cells.py [--db server/cells.db] FILE.csv.gz [FILE ...]
"""

import argparse
import csv
import gzip
import sqlite3
import sys
from pathlib import Path

DB_DEFAULT = Path(__file__).parent / "cells.db"


def init_db(path: Path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(path))
    conn.execute("""
        CREATE TABLE IF NOT EXISTS cells (
            mcc   INTEGER NOT NULL,
            net   INTEGER NOT NULL,
            area  INTEGER NOT NULL,
            cell  INTEGER NOT NULL,
            lat   REAL    NOT NULL,
            lon   REAL    NOT NULL,
            range INTEGER,
            PRIMARY KEY (mcc, net, area, cell)
        ) WITHOUT ROWID
    """)
    conn.commit()
    return conn


def import_file(conn: sqlite3.Connection, path: Path) -> int:
    opener = gzip.open if path.suffix == ".gz" else open
    n = 0
    with opener(path, "rt", newline="") as fh:
        reader = csv.reader(fh)
        batch = []
        for row in reader:
            if len(row) < 9:
                continue
            try:
                rec = (
                    int(row[1]), int(row[2]), int(row[3]), int(row[4]),
                    float(row[7]), float(row[6]),   # lat, lon (CSV is lon,lat)
                    int(row[8]) if row[8] else None,
                )
            except ValueError:
                continue  # skip malformed / header-ish lines
            batch.append(rec)
            if len(batch) >= 5000:
                conn.executemany(
                    "INSERT OR REPLACE INTO cells VALUES (?,?,?,?,?,?,?)", batch)
                n += len(batch)
                batch.clear()
        if batch:
            conn.executemany(
                "INSERT OR REPLACE INTO cells VALUES (?,?,?,?,?,?,?)", batch)
            n += len(batch)
    conn.commit()
    return n


def main():
    ap = argparse.ArgumentParser(description="Build cells.db from OpenCelliD CSVs")
    ap.add_argument("files", nargs="+", help="OpenCelliD .csv or .csv.gz files")
    ap.add_argument("--db", default=str(DB_DEFAULT))
    args = ap.parse_args()

    conn = init_db(Path(args.db))
    total = 0
    for f in args.files:
        p = Path(f)
        if not p.exists():
            print(f"[cells] skip missing {f}", file=sys.stderr)
            continue
        c = import_file(conn, p)
        total += c
        print(f"[cells] {p.name}: {c} cells")
    rows = conn.execute("SELECT COUNT(*) FROM cells").fetchone()[0]
    conn.close()
    print(f"[cells] db {args.db}: {rows} cells total ({total} imported)")


if __name__ == "__main__":
    main()
