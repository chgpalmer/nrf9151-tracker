"""A-GNSS assistance: IGS broadcast ephemeris -> nRF91-ready integers.

The device asks for assistance over CoAP (POST /agnss, AgnssRequest) and gets
an AgnssData protobuf whose every numeric field is PRE-SCALED to the units of
nrfxlib's nrf_modem_gnss_agnss_* structs (GPS ICD-200 scale factors). The
firmware copies fields 1:1 and calls nrf_modem_gnss_agnss_write() -- all the
format knowledge lives here, where it can be unit-tested against real
broadcast data.

Source: BKG's streamed combined broadcast file (free, anonymous HTTPS,
refreshed ~15 min from ~100 IGS station RTCM streams):
  https://igs.bkg.bund.de/root_ftp/IGS/BRDC/{Y}/{DDD}/BRDC00WRD_S_{Y}{DDD}0000_01D_MN.rnx.gz
The cumulative day file always contains the current ephemeris set; near
midnight the fetcher falls back to yesterday's file (fit interval ~4 h).
This source has no ionosphere/UTC header records, so those assistance types
are simply not served -- they polish accuracy, not TTFF.

The parser handles exactly what we consume: RINEX 3.x GPS LNAV records.
"""

from __future__ import annotations

import calendar
import gzip
import json
import math
import os
import sys
import time
import urllib.request
from dataclasses import dataclass, asdict
from pathlib import Path

import tracker_pb2 as pb

BKG_URL = ("https://igs.bkg.bund.de/root_ftp/IGS/BRDC/"
           "{y}/{doy:03d}/BRDC00WRD_S_{y}{doy:03d}0000_01D_MN.rnx.gz")

GPS_EPOCH_UNIX = 315964800          # 1980-01-06 00:00:00 UTC
GPS_UTC_LEAP_S = 18                 # constant until the next leap second
WEEK_S = 604800

# Ephemeris older than this is worse than none: the device would compute
# wrong satellite positions instead of downloading right ones.
MAX_EPHE_AGE_S = 4 * 3600

# Keep the response inside one unfragmented datagram on LTE.
PAYLOAD_BUDGET = 1150
MAX_SVS = 12

PI = 3.1415926535898               # ICD-200 pi, used for semicircle scaling


# ── RINEX 3 GPS nav parsing ─────────────────────────────────────────────────

@dataclass
class Eph:
    """One GPS LNAV ephemeris, physical units straight from RINEX."""
    prn: int
    toc_gps: int      # GPS seconds (epoch line, GPS time frame)
    af0: float
    af1: float
    af2: float
    iode: float
    crs: float        # m
    delta_n: float    # rad/s
    m0: float         # rad
    cuc: float        # rad
    e: float
    cus: float        # rad
    sqrt_a: float     # sqrt(m)
    toe: float        # s of GPS week
    cic: float        # rad
    omega0: float     # rad
    cis: float        # rad
    i0: float         # rad
    crc: float        # m
    w: float          # rad
    omega_dot: float  # rad/s
    idot: float       # rad/s
    week: int         # GPS week (goes with toe)
    ura_m: float      # SV accuracy, metres
    health: int
    tgd: float        # s
    iodc: float
    fit_hours: float  # 0/blank = 4h


def _f(line: str, k: int) -> float:
    """k-th 19-char float field of a nav line (fields butt together)."""
    s = line[4 + 19 * k: 4 + 19 * (k + 1)].strip().replace("D", "e")
    return float(s) if s else 0.0


def parse_rinex_gps(text: str) -> dict[int, Eph]:
    """Latest healthy ephemeris per PRN from a RINEX 3 mixed nav file."""
    lines = text.splitlines()
    try:
        start = next(i for i, l in enumerate(lines) if "END OF HEADER" in l) + 1
    except StopIteration:
        start = 0
    out: dict[int, Eph] = {}
    i = start
    while i < len(lines):
        l = lines[i]
        if not l.startswith("G") or len(l) < 23:
            i += 1  # other constellations have other record lengths; resync on 'G'
            continue
        rec = lines[i:i + 8]
        if len(rec) < 8:
            break
        i += 8
        try:
            prn = int(l[1:3])
            y, mo, d, h, mi, s = (int(l[4:8]), int(l[9:11]), int(l[12:14]),
                                  int(l[15:17]), int(l[18:20]), int(l[21:23]))
            toc_gps = calendar.timegm((y, mo, d, h, mi, s)) - GPS_EPOCH_UNIX
            eph = Eph(
                prn=prn, toc_gps=toc_gps,
                af0=_f(rec[0], 1), af1=_f(rec[0], 2), af2=_f(rec[0], 3),
                iode=_f(rec[1], 0), crs=_f(rec[1], 1),
                delta_n=_f(rec[1], 2), m0=_f(rec[1], 3),
                cuc=_f(rec[2], 0), e=_f(rec[2], 1),
                cus=_f(rec[2], 2), sqrt_a=_f(rec[2], 3),
                toe=_f(rec[3], 0), cic=_f(rec[3], 1),
                omega0=_f(rec[3], 2), cis=_f(rec[3], 3),
                i0=_f(rec[4], 0), crc=_f(rec[4], 1),
                w=_f(rec[4], 2), omega_dot=_f(rec[4], 3),
                idot=_f(rec[5], 0), week=int(_f(rec[5], 2)),
                ura_m=_f(rec[6], 0), health=int(_f(rec[6], 1)),
                tgd=_f(rec[6], 2), iodc=_f(rec[6], 3),
                fit_hours=_f(rec[7], 1),
            )
        except (ValueError, IndexError):
            continue
        if eph.health != 0:
            continue
        prev = out.get(prn)
        if prev is None or eph.toc_gps > prev.toc_gps:
            out[prn] = eph
    return out


# ── ICD-200 scaling (floats -> the integers the modem structs want) ────────

# URA index N covers accuracies up to these metres (ICD-200 table 20-I).
_URA_M = [2.4, 3.4, 4.85, 6.85, 9.65, 13.65, 24.0, 48.0,
          96.0, 192.0, 384.0, 768.0, 1536.0, 3072.0, 6144.0]


def ura_index(metres: float) -> int:
    for i, lim in enumerate(_URA_M):
        if metres <= lim:
            return i
    return 15


def _q(value: float, scale_pow2: int) -> int:
    """Quantize to an integer of scale 2^scale_pow2."""
    return round(value * (2.0 ** -scale_pow2))


def scale_ephemeris(e: Eph) -> dict:
    """Eph (physical floats) -> AgnssEphemeris field dict (ICD integers).

    Semicircle fields arrive from RINEX in radians: /pi first. toc arrives as
    a GPS-time epoch: reduce to seconds-of-week before the 2^4 scaling.
    """
    toc_sow = e.toc_gps % WEEK_S
    return dict(
        sv_id=e.prn,
        health=e.health,
        iodc=int(e.iodc) & 0x7FF,
        toc=round(toc_sow / 16.0),
        af2=_q(e.af2, -55),
        af1=_q(e.af1, -43),
        af0=_q(e.af0, -31),
        tgd=_q(e.tgd, -31),
        ura=ura_index(e.ura_m),
        fit_int=1 if e.fit_hours > 4.0 else 0,
        toe=round(e.toe / 16.0),
        w=_q(e.w / PI, -31),
        delta_n=_q(e.delta_n / PI, -43),
        m0=_q(e.m0 / PI, -31),
        omega_dot=_q(e.omega_dot / PI, -43),
        e=_q(e.e, -33),
        idot=_q(e.idot / PI, -43),
        sqrt_a=_q(e.sqrt_a, -19),
        i0=_q(e.i0 / PI, -31),
        omega0=_q(e.omega0 / PI, -31),
        crs=_q(e.crs, -5),
        cis=_q(e.cis, -29),
        cus=_q(e.cus, -29),
        crc=_q(e.crc, -5),
        cic=_q(e.cic, -29),
        cuc=_q(e.cuc, -29),
    )


# ── satellite position / elevation (for choosing which SVs to send) ────────

_MU = 3.986005e14
_OMEGA_E = 7.2921151467e-5


def sv_pos_ecef(e: Eph, gps_sec: float) -> tuple[float, float, float]:
    """ICD-200 broadcast orbit propagation -> ECEF metres."""
    a = e.sqrt_a ** 2
    tk = (gps_sec % WEEK_S) - e.toe
    if tk > 302400:
        tk -= WEEK_S
    elif tk < -302400:
        tk += WEEK_S
    n = math.sqrt(_MU / a ** 3) + e.delta_n
    mk = e.m0 + n * tk
    ek = mk
    for _ in range(10):
        ek = mk + e.e * math.sin(ek)
    nu = math.atan2(math.sqrt(1 - e.e ** 2) * math.sin(ek),
                    math.cos(ek) - e.e)
    phi = nu + e.w
    du = e.cus * math.sin(2 * phi) + e.cuc * math.cos(2 * phi)
    dr = e.crs * math.sin(2 * phi) + e.crc * math.cos(2 * phi)
    di = e.cis * math.sin(2 * phi) + e.cic * math.cos(2 * phi)
    u = phi + du
    r = a * (1 - e.e * math.cos(ek)) + dr
    inc = e.i0 + di + e.idot * tk
    xo, yo = r * math.cos(u), r * math.sin(u)
    om = (e.omega0 + (e.omega_dot - _OMEGA_E) * tk -
          _OMEGA_E * e.toe)
    x = xo * math.cos(om) - yo * math.cos(inc) * math.sin(om)
    y = xo * math.sin(om) + yo * math.cos(inc) * math.cos(om)
    z = yo * math.sin(inc)
    return x, y, z


def elevation_deg(sv_ecef: tuple[float, float, float],
                  lat_deg: float, lon_deg: float) -> float:
    """Elevation of a satellite above the observer's horizon (spherical obs)."""
    lat, lon = math.radians(lat_deg), math.radians(lon_deg)
    re = 6371000.0
    ox = re * math.cos(lat) * math.cos(lon)
    oy = re * math.cos(lat) * math.sin(lon)
    oz = re * math.sin(lat)
    dx, dy, dz = sv_ecef[0] - ox, sv_ecef[1] - oy, sv_ecef[2] - oz
    # ENU up-component
    up = (dx * math.cos(lat) * math.cos(lon) +
          dy * math.cos(lat) * math.sin(lon) +
          dz * math.sin(lat))
    return math.degrees(math.asin(up / math.sqrt(dx * dx + dy * dy + dz * dz)))


# ── assistance assembly ─────────────────────────────────────────────────────

def unc_k(metres: float) -> int:
    """3GPP uncertainty coding: r = 10 * (1.1^K - 1)."""
    if metres <= 0:
        return 0
    return max(0, min(127, math.ceil(math.log(metres / 10.0 + 1) /
                                     math.log(1.1))))


def fill_time(msg: pb.AgnssTime, unix_now: float) -> None:
    gps_sec = int(unix_now) - GPS_EPOCH_UNIX + GPS_UTC_LEAP_S
    msg.date_day = gps_sec // 86400
    msg.time_full_s = gps_sec % 86400
    msg.time_frac_ms = int((unix_now % 1) * 1000)


def fill_location(msg: pb.AgnssLocation, lat: float, lon: float,
                  unc_m: float) -> None:
    msg.latitude = math.floor(lat * (1 << 23) / 90.0)
    msg.longitude = math.floor(lon * (1 << 24) / 360.0)
    msg.altitude = 0
    msg.unc_semimajor = unc_k(unc_m)
    msg.unc_semiminor = unc_k(unc_m)
    msg.unc_altitude = 255      # unknown
    msg.confidence = 68


def assemble(cache: "AgnssCache", pos: tuple[float, float, float] | None,
             unix_now: float | None = None) -> bytes:
    """Build the AgnssData payload.

    pos = (lat, lon, uncertainty_m) from the device's last DB row, or None.
    Ephemerides: healthy SVs above the horizon at pos, highest elevation
    first, capped by MAX_SVS and then by the datagram budget. Without a
    position (empty DB only -- a cell fix precedes any fetch), fall back to
    the newest MAX_SVS, which still halves a cold start on average.
    """
    now = time.time() if unix_now is None else unix_now
    data = pb.AgnssData()
    fill_time(data.time, now)
    if pos is not None:
        fill_location(data.location, pos[0], pos[1], pos[2])

    ephs = cache.fresh(now)
    if pos is not None:
        gps_sec = now - GPS_EPOCH_UNIX + GPS_UTC_LEAP_S
        ranked = sorted(
            ((elevation_deg(sv_pos_ecef(e, gps_sec), pos[0], pos[1]), e)
             for e in ephs),
            key=lambda t: t[0], reverse=True)
        chosen = [e for el, e in ranked if el > 0.0][:MAX_SVS]
    else:
        chosen = sorted(ephs, key=lambda e: e.toc_gps, reverse=True)[:MAX_SVS]

    for e in chosen:
        data.ephemeris.add(**scale_ephemeris(e))
    payload = data.SerializeToString()
    while len(payload) > PAYLOAD_BUDGET and len(data.ephemeris):
        del data.ephemeris[-1]  # lowest elevation went in last
        payload = data.SerializeToString()
    return payload


# ── cache + fetch ───────────────────────────────────────────────────────────

class AgnssCache:
    """Latest ephemeris per PRN + persistence, so a server restart serves
    immediately and tests/smoke can run from a local file with no network."""

    def __init__(self, persist: Path | None = None):
        self.ephs: dict[int, Eph] = {}
        self.fetched_at = 0.0
        self.persist = persist
        if persist and persist.exists():
            try:
                raw = json.loads(persist.read_text())
                self.fetched_at = raw["fetched_at"]
                self.ephs = {int(k): Eph(**v) for k, v in raw["ephs"].items()}
            except Exception as exc:  # corrupt cache = no cache
                print(f"[agnss] ignoring bad cache: {exc}", file=sys.stderr)

    def load_text(self, text: str, now: float | None = None) -> int:
        parsed = parse_rinex_gps(text)
        if parsed:
            self.ephs.update(parsed)
            self.fetched_at = time.time() if now is None else now
            self._save()
        return len(parsed)

    def _save(self) -> None:
        if self.persist:
            tmp = self.persist.with_suffix(".tmp")
            tmp.write_text(json.dumps({
                "fetched_at": self.fetched_at,
                "ephs": {k: asdict(v) for k, v in self.ephs.items()},
            }))
            tmp.replace(self.persist)

    def fresh(self, now: float) -> list[Eph]:
        """Ephemerides young enough to serve (stale ones are worse than
        none: the device would trust wrong orbits instead of downloading)."""
        gps_now = now - GPS_EPOCH_UNIX + GPS_UTC_LEAP_S
        # abs(): the stream forward-dates toc/toe (a 00:00 record appears at
        # ~23:20), so "fresh" must accept both slightly-future and recent-past.
        return [e for e in self.ephs.values()
                if abs(gps_now - e.toc_gps) < MAX_EPHE_AGE_S]

    def fetch(self) -> int:
        """Download the current BKG day file (yesterday's near midnight)."""
        override = os.environ.get("TRACKER_AGNSS_FILE")
        if override:
            p = Path(override)
            data = p.read_bytes()
            text = gzip.decompress(data).decode() if p.suffix == ".gz" \
                else data.decode()
            n = self.load_text(text)
            print(f"[agnss] loaded {n} SVs from {override}")
            return n
        last_err = None
        for back in (0, 1):
            t = time.gmtime(time.time() - back * 86400)
            url = BKG_URL.format(y=t.tm_year, doy=t.tm_yday)
            try:
                req = urllib.request.Request(
                    url, headers={"User-Agent": "nrf9151-tracker-agnss/1.0"})
                with urllib.request.urlopen(req, timeout=60) as r:
                    text = gzip.decompress(r.read()).decode()
                n = self.load_text(text)
                print(f"[agnss] fetched {n} SVs from {url}")
                return n
            except Exception as exc:
                last_err = exc
        print(f"[agnss] fetch failed: {last_err}", file=sys.stderr)
        return 0
