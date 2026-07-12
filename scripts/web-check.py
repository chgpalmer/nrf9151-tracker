#!/usr/bin/env python3
"""
Headless-browser check of the web UI: load it, EXECUTE it, and fail loudly.

Parse checks and curl probes cannot catch a page that serves fine but dies at
runtime (broken import, missing element, bad API shape) — this can. It drives
the real page in headless Chromium and fails on:

  - any console error or uncaught page error
  - any failed subresource (404 module import, CDN miss)
  - the map page not actually rendering (no Leaflet container / no chips bar)

and then exercises the basic interactions (chip clicks, source toggles).

Usage:
  scripts/web-check.py [--url http://127.0.0.1:8080]

Needs: pip install playwright && playwright install chromium
(dev-only; not part of server requirements)
"""

import argparse
import sys
import time

from playwright.sync_api import sync_playwright


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8080")
    ap.add_argument("--screenshot", default=None, help="save a PNG here")
    args = ap.parse_args()

    problems = []
    console_errors = []

    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": 1400, "height": 900})

        page.on("console", lambda m: console_errors.append(f"console.{m.type}: {m.text}")
                if m.type in ("error",) else None)
        page.on("pageerror", lambda e: console_errors.append(f"pageerror: {e}"))
        # Leaflet aborts in-flight tile fetches whenever the view moves;
        # ERR_ABORTED on basemap tiles is normal, not a failure.
        page.on("requestfailed", lambda r: problems.append(
            f"request failed: {r.url} ({r.failure})")
            if not ("ERR_ABORTED" in str(r.failure) and "basemaps" in r.url) else None)
        page.on("response", lambda r: problems.append(
            f"HTTP {r.status}: {r.url}") if r.status >= 400 else None)

        page.goto(args.url, wait_until="networkidle", timeout=30000)

        # The page must have actually built itself.
        checks = [
            (".leaflet-container", "Leaflet map initialised"),
            ("#trip-chips", "trip chips bar present"),
            ("#tl-track", "timeline present"),
            ("#chart-speed", "charts present"),
        ]
        for selector, what in checks:
            if page.locator(selector).count() == 0:
                problems.append(f"missing: {what} ({selector})")

        # Exercise the chips and toggles; each click must not throw.
        for chip in page.locator("#trip-chips button").all():
            chip.click()
            page.wait_for_timeout(150)

        # Day view at 1 Hz density must stay responsive: click DAY (10k+
        # fixes) and require the main thread back within budget, with no
        # per-fix DOM markers (canvas renders the points).
        day_chip = page.locator("#trip-chips button", has_text="DAY")
        if day_chip.count():
            t0 = time.time()
            day_chip.first.click()
            page.evaluate("1 + 1")   # blocks until the main thread is free
            busy_s = time.time() - t0
            if busy_s > 5.0:
                problems.append(
                    f"Day view blocked the main thread {busy_s:.1f}s (>5s)")
            n_dom = page.evaluate(
                "document.querySelectorAll('#map-main .leaflet-marker-pane > *').length")
            if n_dom > 300:
                problems.append(
                    f"Day view created {n_dom} DOM markers (>300) — "
                    "per-fix DOM is back")
        else:
            problems.append("no DAY chip found for the density check")

        # Fullscreen control must expand the map to the viewport and back.
        fs = page.locator(".map-fs-btn")
        if fs.count() == 0:
            problems.append("fullscreen control missing")
        else:
            fs.first.click()
            page.wait_for_timeout(300)
            vw = page.evaluate("window.innerWidth")
            mw = page.evaluate(
                "document.getElementById('map-main').getBoundingClientRect().width")
            if abs(vw - mw) > 4:
                problems.append(f"fullscreen map width {mw} != viewport {vw}")
            # The top-rail must not overlay the map (it buried the zoom
            # buttons — sibling stacking context beats any child z-index).
            rail_visible = page.evaluate(
                "getComputedStyle(document.querySelector('.top-rail')).display !== 'none'")
            if rail_visible:
                problems.append("top-rail still visible over fullscreen map")
            page.keyboard.press("Escape")
            page.wait_for_timeout(300)
            mw2 = page.evaluate(
                "document.getElementById('map-main').getBoundingClientRect().width")
            if abs(vw - mw2) <= 4:
                problems.append("Escape did not exit fullscreen")
        for tid in ("#filter-gps", "#filter-cell", "#filter-gps", "#filter-cell"):
            if page.locator(tid).count():
                page.click(tid)
                page.wait_for_timeout(100)

        # Two-column layout at desktop width: side panel sits to the RIGHT
        # of the map, and the journeys table has selectable trip rows.
        disp = page.evaluate(
            "getComputedStyle(document.getElementById('page-map')).display")
        if disp != "grid":
            problems.append(f"desktop map page display is {disp}, want grid")
        n_rows = page.locator("#journeys-table tbody tr").count()
        if n_rows == 0:
            problems.append("journeys table has no rows")
        else:
            page.locator("#journeys-table tbody tr").first.click()
            page.wait_for_timeout(300)
            if page.locator("#journeys-table tbody tr.active").count() == 0:
                problems.append("journeys row click did not mark it active")

        # Chart tabs: ACCURACY tab must reveal the accuracy canvas.
        page.click('#chart-tabs .chart-tab[data-chart="acc"]')
        page.wait_for_timeout(200)
        acc_visible = page.evaluate(
            "document.querySelector('.chart-wrap[data-chart=\\'acc\\']')"
            ".classList.contains('active')")
        if not acc_visible:
            problems.append("accuracy chart tab did not activate")
        page.click('#chart-tabs .chart-tab[data-chart="speed"]')
        page.wait_for_timeout(200)

        # Cross-view selection: clicking the speed chart highlights the
        # table row and quietly fills the DETAIL panel — but must NOT
        # yank the side panel to the DETAIL tab (jarring). Tapping the
        # DETAIL tab then shows the populated detail.
        box = page.locator("#chart-speed").bounding_box()
        if box:
            page.mouse.click(box["x"] + box["width"] * 0.5,
                             box["y"] + box["height"] * 0.5)
            page.wait_for_timeout(400)
            if page.locator('#side-tabs .side-tab[data-tab="detail"].active').count():
                problems.append("chart click auto-switched to DETAIL tab")
            if page.locator(".ftable-table tr.sel").count() == 0:
                problems.append("chart click did not highlight a table row")
            if not (page.locator("#drawer-body").inner_text() or "").strip():
                problems.append("chart click did not populate the detail panel")
            page.click('#side-tabs .side-tab[data-tab="detail"]')
            page.wait_for_timeout(200)
            if page.locator('.side-tab-panel[data-tab="detail"].active').count() == 0:
                problems.append("DETAIL tab did not show the detail panel")
            page.click("#drawer-close")
            page.wait_for_timeout(200)
            if page.locator('#side-tabs .side-tab[data-tab="detail"].active').count():
                problems.append("closing detail did not return to the previous tab")

        # Day steppers must navigate without errors; forward stops (greyed)
        # at today — the seed day IS today, so it starts disabled.
        if not page.evaluate("document.getElementById('date-next').disabled"):
            problems.append("date-next not disabled on today")
        for tid in ("#date-prev", "#date-next"):
            if page.locator(tid).count():
                page.click(tid)
                page.wait_for_timeout(400)
        if not page.evaluate("document.getElementById('date-next').disabled"):
            problems.append("date-next not re-disabled after returning to today")

        # trips.js rule check: parked jitter must not form a trip; a loop
        # ride (returns to its start) must. Real-data calibration
        # 2026-07-12: 27 fake trips all ranged <= 102 m from start, the 3
        # real rides all >= 1565 m.
        trip_check = page.evaluate("""async () => {
          const { segmentTrips } = await import('/js/trips.js');
          const t0 = 1700000000;
          // Parked jitter: 10 fixes, 120 s apart, alternating +-0.00015 deg
          // lat (~33 m hops, never >40 m from start). Cumulative path
          // ~270 m, so a cumulative-distance-only rule wrongly trips.
          const jitter = [];
          for (let i = 0; i < 10; i++) {
            jitter.push({ source: 'gps', received_ts: t0 + i * 120,
                          lat: 52.2 + (i % 2 ? 0.00015 : -0.00015),
                          lon: 0.14, spd: 0.3 });
          }
          // Loop ride: 480 m out north at 4 m/s, straight back to start.
          // Net displacement ~0 -- must STILL count (radius, not net).
          const loop = [];
          for (let i = 0; i < 240; i++) {
            const out = i < 120 ? i : 240 - i;
            loop.push({ source: 'gps', received_ts: t0 + i,
                        lat: 52.2 + out * 4 / 111320, lon: 0.14,
                        spd: 4.0 });
          }
          return { jitter: segmentTrips(jitter).length,
                   loop: segmentTrips(loop).length };
        }""")
        if trip_check["jitter"] != 0:
            problems.append(
                f"trips.js: parked jitter formed {trip_check['jitter']} "
                "trip(s), want 0")
        if trip_check["loop"] != 1:
            problems.append(
                f"trips.js: loop ride formed {trip_check['loop']} "
                "trip(s), want 1")

        # Logs page: rows render, WRN filter narrows, ALL widens.
        page.goto(args.url + "/#logs", wait_until="networkidle")
        page.wait_for_timeout(600)
        n_inf = page.locator("#log-tbody tr").count()
        if n_inf == 0:
            problems.append("logs page rendered no rows at +INF")
        page.click('#logs-level .pill[data-level="2"]')
        page.wait_for_timeout(600)
        n_wrn = page.locator("#log-tbody tr").count()
        if not (0 < n_wrn < n_inf):
            problems.append(f"level filter broken: {n_inf} rows at INF, {n_wrn} at WRN")
        page.click('#logs-level .pill[data-level="4"]')
        page.wait_for_timeout(600)
        if page.locator("#log-tbody tr").count() <= n_inf:
            problems.append("ALL level filter did not widen the row set")

        # Usage page: ledger table renders with day rows and bars.
        page.goto(args.url + "/#usage", wait_until="networkidle")
        page.wait_for_timeout(600)
        n_days = page.locator("#usage-tbody tr").count()
        if n_days < 2:
            problems.append(f"usage page rendered {n_days} day rows (want >=2)")
        if page.locator("#usage-tbody .usage-bar").count() == 0:
            problems.append("usage page rendered no bars")
        if "KB" not in (page.locator("#usage-today").inner_text() or ""):
            problems.append("usage page has no today headline")

        # Settings page: placeholder toggles render.
        page.goto(args.url + "/#settings", wait_until="networkidle")
        page.wait_for_timeout(400)
        if page.locator("#page-settings .settings-row").count() < 2:
            problems.append("settings page missing placeholder rows")

        # Settings estimator: sliders drive the cost model.
        before = page.locator("#est-data").inner_text()
        page.locator("#set-flush").evaluate(
            "el => { el.value = 15; el.dispatchEvent(new Event('input')); }")
        page.wait_for_timeout(200)
        after = page.locator("#est-data").inner_text()
        if not before or before == after:
            problems.append(f"settings estimator not reactive ({before!r} -> {after!r})")

        page.wait_for_timeout(500)

        if args.screenshot:
            page.screenshot(path=args.screenshot)

        # ── Mobile pass (390x844): vertical stack, overview mode (the
        # SHOW/SOURCES control rows hide), and fix detail opens INLINE in
        # the DETAIL tab — same as desktop, no fullscreen takeover.
        mob = browser.new_page(viewport={"width": 390, "height": 844})
        mob.on("pageerror", lambda e: console_errors.append(f"mobile pageerror: {e}"))
        mob.goto(args.url, wait_until="networkidle", timeout=30000)
        mob.wait_for_timeout(800)
        mdisp = mob.evaluate(
            "getComputedStyle(document.getElementById('page-map')).display")
        if mdisp == "grid":
            problems.append("mobile map page is grid — stack expected")
        vis = mob.evaluate(
            "getComputedStyle(document.querySelector('.cg-sources')).display")
        if vis != "none":
            problems.append("mobile: .cg-sources row should be hidden")
        # LIVE/DAY chips stay reachable on the phone (slimmed SHOW group).
        if mob.locator("#trip-chips button").count() == 0:
            problems.append("mobile: LIVE/DAY chips missing")
        # Charts start collapsed on the phone — the map is king.
        if mob.locator(".chart-panel.collapsed").count() == 0:
            problems.append("mobile: chart panel should start collapsed")
        # The map keeps a roughly-square minimum; the summary stays one row.
        mh = mob.evaluate(
            "document.getElementById('map-main').getBoundingClientRect().height")
        if mh < 300:
            problems.append(f"mobile map height {mh}px — square-ish minimum expected")
        wrap = mob.evaluate(
            "getComputedStyle(document.getElementById('summary')).flexWrap")
        if wrap != "nowrap":
            problems.append(f"mobile summary flex-wrap is {wrap}, want nowrap")
        # Timeline shrinks to a density strip on phones: no axis, no handles.
        ax = mob.evaluate(
            "getComputedStyle(document.getElementById('tl-axis')).display")
        if ax != "none":
            problems.append("mobile timeline axis should be hidden")
        mob.click('#side-tabs .side-tab[data-tab="locations"]')
        mob.wait_for_timeout(200)
        rows = mob.locator("#fix-table tbody tr")
        if rows.count() == 0:
            problems.append("mobile locations table empty")
        else:
            rows.first.click()
            mob.wait_for_timeout(400)
            # Row click = show on map; it must NOT yank to the DETAIL tab.
            if mob.locator('#side-tabs .side-tab[data-tab="detail"].active').count():
                problems.append("mobile row click auto-switched to DETAIL tab")
            if mob.locator("#fix-table tr.sel").count() == 0:
                problems.append("mobile row click did not highlight the row")
            mob.click('#side-tabs .side-tab[data-tab="detail"]')
            mob.wait_for_timeout(200)
            if not (mob.locator("#drawer-body").inner_text() or "").strip():
                problems.append("mobile DETAIL tab empty after selecting a fix")
            pos = mob.evaluate(
                "getComputedStyle(document.getElementById('fix-drawer')).position")
            if pos == "fixed":
                problems.append("mobile fix detail is a fixed overlay — inline expected")
        mob.close()

        browser.close()

    problems.extend(console_errors)
    if problems:
        print("WEB CHECK FAILED:")
        for pr in problems:
            print("  -", pr)
        sys.exit(1)
    print("web check OK: page executes cleanly, UI renders, interactions work")


if __name__ == "__main__":
    main()
