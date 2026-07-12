/**
 * drawer.js — Fix detail side drawer
 */

import {
  fmtDateTime, fmtAge, mpsToKph, fmtAcc, fmtHdg, fmtAlt, fmtCoord
} from '/js/format.js';

const drawerEl  = document.getElementById('fix-drawer');
const bodyEl    = document.getElementById('drawer-body');
const closeBtn  = document.getElementById('drawer-close');

let closeCb = null;

closeBtn.addEventListener('click', close);

// Allow map popups to trigger drawer via custom event
document.addEventListener('fix-detail-click', e => open(e.detail));

export function open(fix) {
  drawerEl.classList.add('open');
  // On small screens .open overlays the viewport; the body class lets CSS
  // hide the top-rail (sibling stacking context — it would paint over the
  // drawer header and swallow the ✕ otherwise, same trap as fullscreen).
  document.body.classList.add('detail-open');
  drawerEl.setAttribute('aria-hidden', 'false');
  bodyEl.innerHTML = renderFix(fix);
}

export function close() {
  drawerEl.classList.remove('open');
  document.body.classList.remove('detail-open');
  drawerEl.setAttribute('aria-hidden', 'true');
  if (closeCb) closeCb();
}

/** The side-panel tab controller returns to the previous tab on close. */
export function onClose(cb) {
  closeCb = cb;
}

function field(key, val, cls = '') {
  return `
    <div class="detail-field">
      <div class="detail-key">${key}</div>
      <div class="detail-val ${cls}">${val ?? '<span class="na">n/a</span>'}</div>
    </div>`;
}

function renderFix(fix) {
  const isGps = fix.source === 'gps';
  const kph   = mpsToKph(fix.spd);

  return `
    <div class="fix-badge ${isGps ? 'gps' : 'cell'}">
      <span class="fix-badge-dot"></span>
      ${isGps ? 'GPS fix' : 'Cell fix'}
    </div>

    <div class="detail-section">
      ${field('Received', fmtDateTime(fix.received_ts))}
      ${field('Age',      fmtAge(fix.received_ts))}
    </div>

    <div class="detail-divider"></div>

    <div class="detail-section">
      ${field('Latitude',  fmtCoord(fix.lat))}
      ${field('Longitude', fmtCoord(fix.lon))}
      ${field('Altitude',  fmtAlt(fix.alt))}
    </div>

    <div class="detail-divider"></div>

    <div class="detail-section">
      ${field('Accuracy', fmtAcc(fix.acc), isGps ? 'highlight' : 'highlight-amber')}
      ${isGps
        ? `${field('Speed',   kph != null ? `${kph} km/h` : null)}
           ${field('Heading', fmtHdg(fix.hdg))}
           ${field('Satellites', fix.sats != null ? fix.sats : null)}`
        : `${field('Speed',      '<span class="na">n/a — cell fix</span>')}
           ${field('Heading',    '<span class="na">n/a — cell fix</span>')}
           ${field('Satellites', '<span class="na">n/a — cell fix</span>')}`
      }
    </div>

    <div class="detail-divider"></div>

    <div class="detail-section">
      ${field('Fix ID', fix.id)}
      ${field('Source', isGps ? 'GPS' : 'Cell tower')}
    </div>
  `;
}
