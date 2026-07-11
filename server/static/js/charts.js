/**
 * charts.js — Chart.js mini-charts for speed and accuracy.
 *
 * Cross-referencing: hovering either chart snaps to the nearest-x point,
 * shows a dot, and reports the underlying fix via the onHover callback so the
 * map can ring the same point. 1 Hz days carry thousands of points, so the
 * series is decimated (stride sampling) above MAX_PTS — each chart point
 * keeps a reference to its original fix, so hover always maps back exactly.
 */

const TOKEN = {
  signal:    '#00E5A0',
  amber:     '#F5A623',
  border:    '#2A3347',
  textDim:   '#4D607A',
  textSec:   '#8896AA',
  panel:     '#1C2230',
};

const MAX_PTS = 1500;

const CHART_DEFAULTS = {
  animation: false,
  responsive: true,
  maintainAspectRatio: false,
  interaction: { mode: 'index', intersect: false },
  plugins: {
    legend:  { display: false },
    tooltip: {
      backgroundColor: '#1C2230',
      borderColor:     '#2A3347',
      borderWidth:     1,
      titleColor:      '#8896AA',
      bodyColor:       '#D4DCE8',
      titleFont:       { family: 'JetBrains Mono', size: 9 },
      bodyFont:        { family: 'JetBrains Mono', size: 11 },
      padding:         8,
    },
  },
  scales: {
    x: {
      display: false,
    },
    y: {
      grid:  { color: TOKEN.border, lineWidth: 0.5 },
      ticks: {
        color: TOKEN.textDim,
        font:  { family: 'JetBrains Mono', size: 9 },
        maxTicksLimit: 4,
        padding: 4,
      },
      border: { display: false },
    },
  },
};

let speedChart  = null;
let accChart    = null;
let chartFixes  = [];   // decimated series; parallel to both charts' data
let onHoverCb   = null; // (fix|null) => void

function makeChart(canvasId, color, unit) {
  const ctx = document.getElementById(canvasId);
  if (!ctx) return null;

  return new Chart(ctx, {
    type: 'line',
    data: {
      labels:   [],
      datasets: [{
        data:         [],
        borderColor:  color,
        borderWidth:  1.5,
        pointRadius:  0,
        pointHoverRadius: 4,
        pointHoverBackgroundColor: color,
        pointHoverBorderColor: '#0F1117',
        fill:         true,
        backgroundColor: color + '18',
        tension:      0.3,
      }],
    },
    options: {
      ...CHART_DEFAULTS,
      onHover: (evt, elements) => {
        if (!onHoverCb) return;
        onHoverCb(elements.length ? chartFixes[elements[0].index] : null);
      },
      plugins: {
        ...CHART_DEFAULTS.plugins,
        tooltip: {
          ...CHART_DEFAULTS.plugins.tooltip,
          callbacks: {
            label: ctx => `${ctx.parsed.y} ${unit}`,
          },
        },
      },
    },
  });
}

export function initCharts({ onHover } = {}) {
  onHoverCb  = onHover || null;
  speedChart = makeChart('chart-speed', TOKEN.signal, 'km/h');
  accChart   = makeChart('chart-acc',   TOKEN.amber,  'm');

  // Hover ends when the pointer leaves the canvas, not just the line.
  for (const c of [speedChart, accChart]) {
    if (c) c.canvas.addEventListener('mouseleave', () => onHoverCb && onHoverCb(null));
  }
}

/**
 * Update charts from a fixes array.
 * GPS speed in km/h; accuracy in metres for all fixes.
 */
export function updateCharts(fixes) {
  if (!speedChart || !accChart) return;

  // Decimate: even stride keeps the shape; hover still resolves to a real fix.
  let pts = fixes || [];
  if (pts.length > MAX_PTS) {
    const stride = Math.ceil(pts.length / MAX_PTS);
    pts = pts.filter((_, i) => i % stride === 0);
  }
  chartFixes = pts;

  const labels = pts.map(f => {
    const d = new Date(f.received_ts * 1000);
    return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  });

  speedChart.data.labels = labels;
  speedChart.data.datasets[0].data = pts.map(f =>
    f.spd != null ? parseFloat((f.spd * 3.6).toFixed(1)) : null);
  accChart.data.labels = labels;
  accChart.data.datasets[0].data = pts.map(f =>
    f.acc != null ? Math.round(f.acc) : null);

  speedChart.update('none');
  accChart.update('none');
}
