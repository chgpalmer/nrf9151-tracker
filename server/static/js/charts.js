/**
 * charts.js — Chart.js mini-charts for speed and accuracy
 */

const TOKEN = {
  signal:    '#00E5A0',
  amber:     '#F5A623',
  border:    '#2A3347',
  textDim:   '#4D607A',
  textSec:   '#8896AA',
  panel:     '#1C2230',
};

const CHART_DEFAULTS = {
  animation: false,
  responsive: true,
  maintainAspectRatio: false,
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
        fill:         true,
        backgroundColor: color + '18',
        tension:      0.3,
      }],
    },
    options: {
      ...CHART_DEFAULTS,
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

let speedChart  = null;
let accChart    = null;

export function initCharts() {
  speedChart = makeChart('chart-speed', TOKEN.signal, 'km/h');
  accChart   = makeChart('chart-acc',   TOKEN.amber,  'm');
}

/**
 * Update charts from a fixes array.
 * GPS speed in km/h; accuracy in metres for all fixes.
 */
export function updateCharts(fixes) {
  if (!speedChart || !accChart) return;
  if (!fixes || fixes.length === 0) {
    speedChart.data.labels   = [];
    speedChart.data.datasets[0].data = [];
    accChart.data.labels     = [];
    accChart.data.datasets[0].data  = [];
    speedChart.update();
    accChart.update();
    return;
  }

  const labels = fixes.map(f => {
    const d = new Date(f.received_ts * 1000);
    return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  });

  const speeds = fixes.map(f =>
    f.spd != null ? parseFloat((f.spd * 3.6).toFixed(1)) : null
  );

  const accs = fixes.map(f =>
    f.acc != null ? Math.round(f.acc) : null
  );

  speedChart.data.labels = labels;
  speedChart.data.datasets[0].data = speeds;
  accChart.data.labels   = labels;
  accChart.data.datasets[0].data  = accs;

  speedChart.update('none');
  accChart.update('none');
}
