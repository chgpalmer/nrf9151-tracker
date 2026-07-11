"""agnss.py unit tests, run against REAL broadcast data.

brdc_fixture.rnx is 12 satellites trimmed from a live BKG BRDC00WRD_S file
(2026-07-12, DOY 193 epochs). The fixture epoch below matches its records so
freshness/elevation math sees them as current.
"""

import math
import sys
import calendar
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

import agnss
import tracker_pb2 as pb
from agnss import (AgnssCache, Eph, assemble, elevation_deg, parse_rinex_gps,
                   scale_ephemeris, sv_pos_ecef, unc_k, ura_index,
                   GPS_EPOCH_UNIX, GPS_UTC_LEAP_S)

FIXTURE = (Path(__file__).parent / "brdc_fixture.rnx").read_text()
# The fixture's records are epoch 2026-07-12 ~00:00 GPS time; test "now"
# shortly after, in UTC (GPS - leap).
NOW_UNIX = calendar.timegm((2026, 7, 12, 0, 30, 0)) - GPS_UTC_LEAP_S
CAMBRIDGE = (52.2027, 0.1418)


# The fixture holds 12 PRNs but G13 is broadcasting unhealthy (health=63)
# in the real data — the parser must drop it, leaving 11.
HEALTHY = 11


def cache():
    c = AgnssCache()
    n = c.load_text(FIXTURE, now=NOW_UNIX)
    assert n == HEALTHY
    return c


def test_parse_counts_and_fields():
    ephs = parse_rinex_gps(FIXTURE)
    assert len(ephs) == HEALTHY
    assert 13 not in ephs, "G13 is unhealthy in the fixture; must be dropped"
    e = ephs[1]  # G01
    assert e.prn == 1 and e.health == 0
    # physical plausibility of the raw floats
    assert 5153 < e.sqrt_a < 5154          # GPS semi-major axis ~26560 km
    assert 0 < e.e < 0.03
    assert 0 <= e.toe < agnss.WEEK_S
    assert abs(e.af0) < 1e-3
    assert e.week > 2400


def test_scaling_round_trip_within_lsb():
    ephs = parse_rinex_gps(FIXTURE)
    for e in ephs.values():
        s = scale_ephemeris(e)
        # invert each scaling; must land within one quantization step
        checks = [
            (s["sqrt_a"] * 2.0 ** -19, e.sqrt_a, 2.0 ** -19),
            (s["e"] * 2.0 ** -33, e.e, 2.0 ** -33),
            (s["m0"] * 2.0 ** -31 * agnss.PI, e.m0, 2.0 ** -31 * agnss.PI),
            (s["omega0"] * 2.0 ** -31 * agnss.PI, e.omega0,
             2.0 ** -31 * agnss.PI),
            (s["af0"] * 2.0 ** -31, e.af0, 2.0 ** -31),
            (s["delta_n"] * 2.0 ** -43 * agnss.PI, e.delta_n,
             2.0 ** -43 * agnss.PI),
            (s["crs"] * 2.0 ** -5, e.crs, 2.0 ** -5),
            (s["cuc"] * 2.0 ** -29, e.cuc, 2.0 ** -29),
            (s["toe"] * 16.0, e.toe, 16.0),
        ]
        for got, want, lsb in checks:
            assert abs(got - want) <= lsb, (e.prn, got, want)
        # range sanity for the narrow struct fields
        assert -128 <= s["af2"] <= 127
        assert -32768 <= s["af1"] <= 32767
        assert -2097152 <= s["af0"] <= 2097151
        assert -128 <= s["tgd"] <= 127
        assert -32768 <= s["delta_n"] <= 32767
        assert -8388608 <= s["omega_dot"] <= 8388607
        assert -8192 <= s["idot"] <= 8191
        assert 0 <= s["ura"] <= 15


def test_orbit_sanity():
    """Propagated positions must look like GPS orbits: radius ~26 560 km,
    ground speed ~3.9 km/s. This catches any broken term in the ICD math."""
    ephs = parse_rinex_gps(FIXTURE)
    for e in ephs.values():
        t0 = e.toc_gps
        x1 = sv_pos_ecef(e, t0)
        x2 = sv_pos_ecef(e, t0 + 1)
        r = math.sqrt(sum(v * v for v in x1))
        # GPS orbits are mildly eccentric (e up to ~0.03): r = a(1 +/- e)
        assert 25_800_000 < r < 27_300_000, (e.prn, r)
        v = math.sqrt(sum((a - b) ** 2 for a, b in zip(x2, x1)))
        assert 2500 < v < 4500, (e.prn, v)  # ECEF speed incl. Earth rotation


def test_elevation_split():
    """From any point on Earth roughly half the constellation is below the
    horizon; elevations must span both signs and stay in [-90, 90]."""
    ephs = parse_rinex_gps(FIXTURE)
    gps_now = NOW_UNIX - GPS_EPOCH_UNIX + GPS_UTC_LEAP_S
    els = [elevation_deg(sv_pos_ecef(e, gps_now), *CAMBRIDGE)
           for e in ephs.values()]
    assert all(-90 <= el <= 90 for el in els)
    assert any(el > 10 for el in els), "someone must be overhead"
    assert any(el < 0 for el in els), "someone must be set"


def test_assemble_with_position():
    payload = assemble(cache(), (*CAMBRIDGE, 5000.0), unix_now=NOW_UNIX)
    assert len(payload) <= agnss.PAYLOAD_BUDGET
    data = pb.AgnssData.FromString(payload)
    # time sanity: date_day is ~46 years of days, tod < 86400
    assert 16900 < data.time.date_day < 17200
    assert 0 <= data.time.time_full_s < 86400
    # location round-trip within coding resolution (~1e-5 deg)
    assert abs(data.location.latitude * 90.0 / (1 << 23) - CAMBRIDGE[0]) < 1e-4
    assert abs(data.location.longitude * 360.0 / (1 << 24) - CAMBRIDGE[1]) < 1e-4
    assert data.location.unc_altitude == 255
    # only above-horizon SVs, sorted-in by elevation
    assert 4 <= len(data.ephemeris) <= agnss.MAX_SVS
    ids = {e.sv_id for e in data.ephemeris}
    assert ids <= set(range(1, 33)) and len(ids) == len(data.ephemeris)


def test_assemble_without_position_still_serves():
    payload = assemble(cache(), None, unix_now=NOW_UNIX)
    data = pb.AgnssData.FromString(payload)
    assert len(payload) <= agnss.PAYLOAD_BUDGET
    assert len(data.ephemeris) == min(HEALTHY, agnss.MAX_SVS)
    assert not data.HasField("location")


def test_stale_cache_serves_time_but_no_ephemeris():
    payload = assemble(cache(), (*CAMBRIDGE, 5000.0),
                       unix_now=NOW_UNIX + 6 * 3600)
    data = pb.AgnssData.FromString(payload)
    assert len(data.ephemeris) == 0
    assert data.time.date_day > 0


def test_request_round_trip():
    req = pb.AgnssRequest(device_id="359404230933351",
                          data_flags=0x69, sv_mask_ephe=(1 << 32) - 1)
    back = pb.AgnssRequest.FromString(req.SerializeToString())
    assert back.device_id == "359404230933351"
    assert back.data_flags == 0x69


def test_unc_and_ura_coding():
    assert unc_k(0) == 0
    assert unc_k(10) >= 1
    # decode r = 10*(1.1^K - 1) must be >= the requested radius (never lie small)
    for m in (100, 2000, 100_000):
        k = unc_k(m)
        assert 10 * (1.1 ** k - 1) >= m * 0.999
    assert ura_index(2.0) == 0
    assert ura_index(5.0) == 3
    assert ura_index(10_000) == 15
