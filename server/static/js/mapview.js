/**
 * mapview.js — Leaflet map logic for rendering tracks
 *
 * Accuracy-circle strategy:
 *   - No per-fix circles drawn by default.
 *   - Circles are opt-in via `showAccuracy` flag (controlled by a toggle).
 *   - Even when enabled, cell circles are capped to an opacity that makes
 *     stacking visible but not opaque; we use a very low individual opacity
 *     so the user can see the density gradient (many cell fixes → more amber
 *     glow) without an opaque blob occluding the map.
 *   - On hover/selected, the single-fix circle is shown at full clarity.
 *   - Fix detail (accuracy as a number) is always visible in the popup/drawer.
 */

import { fmtDateTime, fmtAge, mpsToKph, fmtAcc, fmtHdg, fmtAlt, fmtCoord } from '/js/format.js';

// Design tokens mirrored from CSS (can't import CSS vars into JS easily)
const TOKEN = {
  signal:    '#00E5A0',
  signalDim: 'rgba(0,229,160,0.08)',
  amber:     '#F5A623',
  amberDim:  'rgba(245,166,35,0.07)',
  ink:       '#0F1117',
  border:    '#2A3347',
  text:      '#D4DCE8',
  textSec:   '#8896AA',
};

/**
 * Create and return a MapView instance bound to a container element id.
 * @param {string} containerId  — id of the <div> to init Leaflet in
 * @param {function} onFixClick — called with the fix object when a marker is clicked
 */
export function createMapView(containerId, onFixClick) {
  const map = L.map(containerId, {
    zoomControl: true,
    attributionControl: true,
  }).setView([51.505, -0.09], 13);

  // Voyager: a light, legible basemap. The dark_all basemap read as "too dark"
  // and washed the track/markers out; a light base makes the green/amber fixes
  // pop.
  L.tileLayer('https://{s}.basemaps.cartocdn.com/rastertiles/voyager/{z}/{x}/{y}{r}.png', {
    attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> &copy; <a href="https://carto.com/">CARTO</a>',
    subdomains: 'abcd',
    maxZoom: 19,
  }).addTo(map);

  // Layer groups
  const trackLayer     = L.layerGroup().addTo(map);
  const markerLayer    = L.layerGroup().addTo(map);
  const arrowLayer     = L.layerGroup().addTo(map);
  const accuracyLayer  = L.layerGroup().addTo(map);
  const selectedLayer  = L.layerGroup().addTo(map);
  const hoverLayer     = L.layerGroup().addTo(map);

  let currentFixes    = [];
  let showAccuracy    = false;
  let showArrows      = true;
  let liveDot         = false;
  let selectedFixId   = null;

  // ── Private helpers ──────────────────────────────────────

  function markerIcon(fix, isLatest) {
    const isGps = fix.source === 'gps';
    let cls = isGps
      ? (isLatest ? 'lm-gps-latest' : 'lm-gps')
      : (isLatest ? 'lm-cell-latest' : 'lm-cell');

    return L.divIcon({
      className: '',
      html: `<div class="${cls}"></div>`,
      iconSize:   isLatest ? [14, 14] : [10, 10],
      iconAnchor: isLatest ? [7, 7]  : [5, 5],
    });
  }

  /** Live view only: the device's current position as a pulsing blue dot. */
  function liveIcon() {
    return L.divIcon({
      className: '',
      html: `<div class="lm-live"></div>`,
      iconSize:   [16, 16],
      iconAnchor: [8, 8],
    });
  }

  /**
   * SVG arrow for direction of travel (GPS fixes with hdg only)
   * rotated to heading angle; small so it sits on the polyline, not on the dot.
   */
  function arrowIcon(headingDeg) {
    // Arrow points "up" (north) at 0°, rotates clockwise
    const svg = `
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="-8 -8 16 16">
        <polygon points="0,-6 3,4 0,1 -3,4" fill="${TOKEN.signal}" fill-opacity="0.85" />
      </svg>`;
    return L.divIcon({
      className: '',
      html: `<div style="transform:rotate(${headingDeg}deg);width:16px;height:16px;line-height:0">${svg}</div>`,
      iconSize:   [16, 16],
      iconAnchor: [8, 8],
    });
  }

  /**
   * Build a Leaflet circle for a fix's accuracy radius.
   * Uses very low opacity for stacking; cell is even lower than GPS.
   */
  function accuracyCircle(fix) {
    const isGps = fix.source === 'gps';
    return L.circle([fix.lat, fix.lon], {
      radius:      fix.acc,
      color:       isGps ? TOKEN.signal : TOKEN.amber,
      weight:      1,
      opacity:     isGps ? 0.25 : 0.12,
      fillColor:   isGps ? TOKEN.signal : TOKEN.amber,
      fillOpacity: isGps ? 0.04 : 0.025,
      interactive: false,
    });
  }

  /**
   * Build a highlighted accuracy circle for the selected fix (full opacity).
   */
  function selectedAccuracyCircle(fix) {
    const isGps = fix.source === 'gps';
    return L.circle([fix.lat, fix.lon], {
      radius:      fix.acc,
      color:       isGps ? TOKEN.signal : TOKEN.amber,
      weight:      1.5,
      opacity:     0.7,
      fillColor:   isGps ? TOKEN.signal : TOKEN.amber,
      fillOpacity: 0.08,
      interactive: false,
      dashArray:   isGps ? null : '4 4',
    });
  }

  // Fix lookup by id for popup "Full detail" link
  const fixById = new Map();

  function popupContent(fix) {
    const isGps = fix.source === 'gps';
    const kph = mpsToKph(fix.spd);
    fixById.set(fix.id, fix);
    return `<div style="padding:10px 12px;min-width:180px">
      <div style="font-weight:700;color:${isGps ? TOKEN.signal : TOKEN.amber};letter-spacing:.08em;margin-bottom:6px;font-size:10px">
        ${isGps ? '● GPS' : '◆ CELL'}
      </div>
      <div style="color:${TOKEN.textSec};font-size:10px;margin-bottom:8px">${fmtDateTime(fix.received_ts)}</div>
      <div style="color:${TOKEN.textSec};font-size:9px;letter-spacing:.08em;margin-bottom:2px">ACCURACY</div>
      <div style="color:${isGps ? TOKEN.text : TOKEN.amber};font-size:12px;font-weight:600;margin-bottom:8px">${fmtAcc(fix.acc)}</div>
      ${kph != null ? `<div style="color:${TOKEN.textSec};font-size:9px;letter-spacing:.08em;margin-bottom:2px">SPEED</div>
      <div style="color:${TOKEN.text};font-size:12px;margin-bottom:8px">${kph} km/h</div>` : ''}
      <div style="font-size:9px;color:${TOKEN.textSec};margin-top:4px;cursor:pointer;text-decoration:underline"
           data-fix-id="${fix.id}" class="popup-detail-link">
        Full detail →
      </div>
    </div>`;
  }

  // Map element + its parent, declared before any use (the click delegate and
  // the empty-state overlay both need them).
  const mapEl    = document.getElementById(containerId);
  const container = mapEl.parentElement || mapEl;
  let emptyOverlay = null;

  // Delegate click on popup detail links (map container catches bubbled events)
  mapEl.addEventListener('click', e => {
    const link = e.target.closest('.popup-detail-link');
    if (!link) return;
    const id  = parseInt(link.dataset.fixId, 10);
    const fix = fixById.get(id);
    if (fix) {
      document.dispatchEvent(new CustomEvent('fix-detail-click', { detail: fix }));
    }
  });

  // ── Public API ───────────────────────────────────────────

  function showEmpty(msg, sub) {
    if (!emptyOverlay) {
      emptyOverlay = document.createElement('div');
      emptyOverlay.className = 'empty-state';
      container.appendChild(emptyOverlay);
    }
    emptyOverlay.innerHTML = `
      <div class="empty-state-label">${msg}</div>
      ${sub ? `<div class="empty-state-sub">${sub}</div>` : ''}
    `;
    emptyOverlay.style.display = 'flex';
  }

  function hideEmpty() {
    if (emptyOverlay) emptyOverlay.style.display = 'none';
  }

  function render(fixes, opts = {}) {
    if (opts.showAccuracy != null) showAccuracy = opts.showAccuracy;
    if (opts.showArrows   != null) showArrows   = opts.showArrows;
    if (opts.liveDot      != null) liveDot      = opts.liveDot;

    currentFixes = fixes;

    trackLayer.clearLayers();
    markerLayer.clearLayers();
    arrowLayer.clearLayers();
    accuracyLayer.clearLayers();
    selectedLayer.clearLayers();

    if (!fixes || fixes.length === 0) {
      showEmpty(opts.emptyMsg || 'No fixes in this window',
                opts.emptySub != null ? opts.emptySub
                                      : 'Try a wider time range or check device connection.');
      return;
    }

    hideEmpty();

    // Track polyline: GPS fixes only. Cell fixes are tower-resolution
    // estimates — a line through them draws travel that never happened.
    const gpsLatlngs = fixes.filter(f => f.source === 'gps').map(f => [f.lat, f.lon]);
    if (gpsLatlngs.length > 1) {
      L.polyline(gpsLatlngs, {
        color:   TOKEN.signal,
        weight:  2,
        opacity: 0.55,
      }).addTo(trackLayer);
    }

    // Accuracy circles (opt-in)
    if (showAccuracy) {
      fixes.forEach(f => accuracyCircle(f).addTo(accuracyLayer));
    }

    const latest = fixes[fixes.length - 1];

    // Markers + arrows
    fixes.forEach((fix, i) => {
      const isLatest = fix === latest;
      const icon = (isLatest && liveDot) ? liveIcon() : markerIcon(fix, isLatest);

      const marker = L.marker([fix.lat, fix.lon], { icon, zIndexOffset: isLatest ? 1000 : 0 })
        .bindPopup(popupContent(fix), { maxWidth: 240, className: 'trk-popup' })
        .on('click', () => {
          selectFix(fix);
          onFixClick && onFixClick(fix);
        })
        .addTo(markerLayer);

      // Direction arrows for GPS fixes that have a heading
      if (showArrows && fix.source === 'gps' && fix.hdg != null && !isLatest) {
        // Place arrow slightly behind this fix toward previous fix
        // (midpoint between prev and this fix to avoid cluttering the marker)
        if (i > 0) {
          const prev = fixes[i - 1];
          const midLat = (prev.lat + fix.lat) / 2;
          const midLon = (prev.lon + fix.lon) / 2;
          L.marker([midLat, midLon], { icon: arrowIcon(fix.hdg), interactive: false })
            .addTo(arrowLayer);
        }
      }
    });

    // Re-apply an existing selection so a live refresh doesn't wipe the
    // highlighted accuracy circle out from under the user.
    if (selectedFixId != null) {
      const sel = fixes.find(f => f.id === selectedFixId);
      if (sel) highlightSelected(sel);
    }

    // Fit map to track
    if (opts.fitBounds !== false) {
      map.fitBounds(computeBounds(fixes), { padding: [30, 30], maxZoom: 17 });
    }
  }

  /**
   * Select a fix: highlight it, and if its accuracy circle isn't already
   * fully in view, zoom out just enough to show it (never zoom *in* on a
   * tight GPS circle — that would be jarring on every click).
   */
  function selectFix(fix) {
    selectedFixId = fix.id;
    highlightSelected(fix);
    if (fix.acc > 0) {
      const circleBounds = L.latLng(fix.lat, fix.lon).toBounds(fix.acc * 2);
      if (!map.getBounds().contains(circleBounds)) {
        map.fitBounds(circleBounds, { padding: [40, 40], maxZoom: map.getZoom() });
      }
    }
  }

  /**
   * Bounds covering the fix positions — and, when accuracy circles are
   * visible, the full extent of each circle, so no radius gets cut off.
   */
  function computeBounds(fixes) {
    const bounds = L.latLngBounds(fixes.map(f => [f.lat, f.lon]));
    if (showAccuracy) {
      fixes.forEach(f => {
        if (f.acc > 0) bounds.extend(L.latLng(f.lat, f.lon).toBounds(f.acc * 2));
      });
    }
    return bounds;
  }

  function highlightSelected(fix) {
    selectedLayer.clearLayers();
    // Show accuracy circle for selected fix regardless of toggle
    selectedAccuracyCircle(fix).addTo(selectedLayer);
  }

  function setShowAccuracy(val) {
    showAccuracy = val;
    // Re-render accuracy layer
    accuracyLayer.clearLayers();
    if (showAccuracy && currentFixes.length > 0) {
      currentFixes.forEach(f => accuracyCircle(f).addTo(accuracyLayer));
      // Re-fit so the newly revealed circles aren't cut off at the edges
      map.fitBounds(computeBounds(currentFixes), { padding: [30, 30], maxZoom: 17 });
    }
  }

  function setShowArrows(val) {
    showArrows = val;
    // Full re-render needed (arrows baked into arrowLayer)
    render(currentFixes, { fitBounds: false });
  }

  function invalidate() {
    map.invalidateSize();
  }

  /** Pan to a fix and highlight it (used by the locations table). */
  function focusFix(fix) {
    selectFix(fix);
    map.panTo([fix.lat, fix.lon]);
  }

  /**
   * Transient crosshair ring for chart/table hover — independent of the click
   * selection so hovering doesn't clobber it. Pass null to clear.
   */
  function hoverFix(fix) {
    hoverLayer.clearLayers();
    if (!fix) return;
    L.circleMarker([fix.lat, fix.lon], {
      radius: 9,
      color: fix.source === 'gps' ? TOKEN.signal : TOKEN.amber,
      weight: 2.5,
      fill: false,
      interactive: false,
    }).addTo(hoverLayer);
  }

  return { render, setShowAccuracy, setShowArrows, focusFix, hoverFix, invalidate, map };
}
