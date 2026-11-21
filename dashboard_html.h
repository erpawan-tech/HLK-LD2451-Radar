// =============================================================================
//  dashboard_html.h - Single-page real-time dashboard for the HLK-LD2451
//  Served from flash by the ESP32 at "/". Talks to ws://<host>/ws.
//  - Radar sector view: pure Canvas (works offline)
//  - Distance / Speed charts: Chart.js from CDN, with an automatic built-in
//    canvas fallback when the CDN is unreachable (e.g. ESP32 AP mode)
// =============================================================================
#pragma once
#include <pgmspace.h>

static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LD2451 Vehicle Radar</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
<style>
:root{
  --bg:#0b0f14;--panel:#121821;--panel2:#18202b;--line:#243041;--text:#d7e0ea;
  --muted:#7d8a9a;--accent:#3fb6ff;--red:#ff4d5e;--green:#2fd18a;--amber:#ffb547;
  --grid:rgba(63,182,255,.22);--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--text);
  font:14px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{display:flex;align-items:center;gap:12px;flex-wrap:wrap;padding:12px 18px;
  border-bottom:1px solid var(--line);background:linear-gradient(#0f151d,#0b0f14)}
header h1{font-size:16px;margin:0;font-weight:600;letter-spacing:.3px}
header h1 span{color:var(--accent)}
.pill{padding:4px 10px;border-radius:999px;font-size:12px;border:1px solid var(--line);
  background:var(--panel);color:var(--muted);font-family:var(--mono)}
.pill.ok{color:var(--green);border-color:rgba(47,209,138,.4)}
.pill.bad{color:var(--red);border-color:rgba(255,77,94,.4)}
.pill.warn{color:var(--amber);border-color:rgba(255,181,71,.4)}
#alarm{display:none;background:var(--red);color:#fff;border-color:var(--red);
  font-weight:700;animation:blink .8s steps(2) infinite}
#alarm.on{display:inline-block}
@keyframes blink{50%{opacity:.35}}
.spacer{flex:1}
main{display:grid;gap:14px;padding:14px;
  grid-template-columns:minmax(0,1.25fr) minmax(0,1fr);
  grid-template-areas:"radar table" "radar table" "dist spd" "foot foot"}
@media (max-width:900px){main{grid-template-columns:1fr;
  grid-template-areas:"radar" "table" "dist" "spd" "foot"}}
.card{background:var(--panel);border:1px solid var(--line);border-radius:12px;
  padding:12px 14px;min-width:0}
.card h2{margin:0 0 8px;font-size:12px;font-weight:600;letter-spacing:1px;
  text-transform:uppercase;color:var(--muted);display:flex;justify-content:space-between}
#radarCard{grid-area:radar}#tableCard{grid-area:table}
#distCard{grid-area:dist}#spdCard{grid-area:spd}#foot{grid-area:foot}
.canvasWrap{position:relative;width:100%}
#radarWrap{aspect-ratio:1.08/1;max-height:560px;margin:0 auto}
.chartWrap{height:220px}
canvas{display:block;width:100%;height:100%}
.legend{display:flex;gap:14px;font-size:12px;color:var(--muted);margin-top:6px}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:5px;
  vertical-align:-1px}
table{width:100%;border-collapse:collapse;font-family:var(--mono);font-size:13px}
th,td{padding:7px 8px;text-align:right;border-bottom:1px solid var(--line);white-space:nowrap}
th{color:var(--muted);font-weight:500;font-size:11px;text-transform:uppercase;
  letter-spacing:.6px;position:sticky;top:0;background:var(--panel)}
th:first-child,td:first-child{text-align:left}
td.dir-a{color:var(--red)}td.dir-g{color:var(--green)}
tr.primary td{background:rgba(63,182,255,.08)}
.empty td{text-align:center;color:var(--muted);padding:26px}
.big{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-bottom:12px}
.big div{background:var(--panel2);border:1px solid var(--line);border-radius:10px;padding:10px}
.big b{display:block;font:600 26px/1.1 var(--mono)}
.big small{color:var(--muted);font-size:11px;text-transform:uppercase;letter-spacing:.6px}
#foot{display:grid;grid-template-columns:1fr auto;gap:10px;align-items:start}
#raw{font-family:var(--mono);font-size:12px;color:var(--accent);word-break:break-all;min-height:18px}
#stats{font-family:var(--mono);font-size:12px;color:var(--muted);text-align:right}
.note{font-size:11px;color:var(--amber)}
</style>
</head>
<body>
<header>
  <h1><span>&#9673;</span> HLK-LD2451 &nbsp;Vehicle Radar</h1>
  <span id="alarm" class="pill">&#9888; APPROACHING ALARM</span>
  <div class="spacer"></div>
  <span id="fps" class="pill">0.0 fps</span>
  <span id="mode" class="pill">--</span>
  <span id="conn" class="pill bad">disconnected</span>
</header>

<main>
  <section class="card" id="radarCard">
    <h2><span>Radar sector &middot; &plusmn;30&deg; / 100 m</span><span id="tcount">0 targets</span></h2>
    <div class="canvasWrap" id="radarWrap"><canvas id="radar"></canvas></div>
    <div class="legend">
      <span><i class="dot" style="background:var(--red)"></i>Approaching</span>
      <span><i class="dot" style="background:var(--green)"></i>Moving away</span>
      <span><i class="dot" style="border:1px dashed var(--amber)"></i>&plusmn;20&deg; rated FOV</span>
    </div>
  </section>

  <section class="card" id="tableCard">
    <h2><span>Primary target (nearest)</span><span id="age">--</span></h2>
    <div class="big">
      <div><small>Distance</small><b id="pDist">--</b></div>
      <div><small>Speed</small><b id="pSpd">--</b></div>
      <div><small>Angle</small><b id="pAng">--</b></div>
    </div>
    <h2><span>Active targets</span></h2>
    <div style="overflow-x:auto"><table>
      <thead><tr><th>ID</th><th>Dist (m)</th><th>Speed (km/h)</th><th>Angle (&deg;)</th>
        <th>Direction</th><th>SNR</th></tr></thead>
      <tbody id="tbody"><tr class="empty"><td colspan="6">No targets</td></tr></tbody>
    </table></div>
  </section>

  <section class="card" id="distCard">
    <h2><span>Distance vs time (primary)</span><span>m</span></h2>
    <div class="canvasWrap chartWrap"><canvas id="distChart"></canvas></div>
  </section>

  <section class="card" id="spdCard">
    <h2><span>Speed vs time (primary)</span><span>km/h</span></h2>
    <div class="canvasWrap chartWrap"><canvas id="spdChart"></canvas></div>
  </section>

  <section class="card" id="foot">
    <div><h2><span>Last raw frame</span></h2><div id="raw">--</div>
      <div id="cdnNote" class="note"></div></div>
    <div id="stats">--</div>
  </section>
</main>

<script>
(() => {
'use strict';
// ---------------- configuration ----------------
const MAX_RANGE = 100;      // m
const FOV       = 30;       // half-angle drawn (deg)
const RATED_FOV = 20;       // module spec (deg)
const WINDOW_S  = 60;       // chart history (s)
const ANGLE_SIGN = 1;       // set to -1 if left/right appear mirrored
const TRAIL_MS  = 1500;     // ghost trail duration on radar

const $ = id => document.getElementById(id);
const t0 = performance.now();
const nowS = () => (performance.now() - t0) / 1000;

// ---------------- state ----------------
let targets = [], alarm = 0, lastFrameAt = 0, stale = true;
const trail = [];           // [{t, pts:[{a,d,dir}]}]
const frameTimes = [];

// ================= RADAR CANVAS =================
const rc = $('radar'), rctx = rc.getContext('2d');
let geo = null;
function sizeCanvas(c){
  const dpr = window.devicePixelRatio || 1, r = c.getBoundingClientRect();
  const w = Math.max(1, Math.round(r.width * dpr)), h = Math.max(1, Math.round(r.height * dpr));
  if (c.width !== w || c.height !== h){ c.width = w; c.height = h; return true; }
  return false;
}
function computeGeo(){
  const dpr = window.devicePixelRatio || 1, w = rc.width, h = rc.height;
  const pad = 26 * dpr, half = Math.sin(FOV * Math.PI / 180);
  const R = Math.min(h - pad * 1.6, (w / 2 - pad) / half);
  geo = {dpr, w, h, R, ox: w / 2, oy: h - pad * 0.8};
}
const toRad = d => d * Math.PI / 180;
function polar(angleDeg, dist){
  const a = toRad(Math.max(-FOV, Math.min(FOV, angleDeg * ANGLE_SIGN)));
  const r = Math.min(dist, MAX_RANGE) / MAX_RANGE * geo.R;
  return [geo.ox + r * Math.sin(a), geo.oy - r * Math.cos(a)];
}
function wedge(ctx, r, a0, a1){
  ctx.beginPath(); ctx.moveTo(geo.ox, geo.oy);
  ctx.arc(geo.ox, geo.oy, r, -Math.PI/2 + toRad(a0), -Math.PI/2 + toRad(a1)); ctx.closePath();
}
function drawRadar(ts){
  if (sizeCanvas(rc) || !geo) computeGeo();
  const {dpr, w, h, R, ox, oy} = geo, ctx = rctx;
  ctx.clearRect(0, 0, w, h);

  // background sector
  const bg = ctx.createRadialGradient(ox, oy, 0, ox, oy, R);
  bg.addColorStop(0, 'rgba(63,182,255,.10)'); bg.addColorStop(1, 'rgba(63,182,255,.02)');
  wedge(ctx, R, -FOV, FOV); ctx.fillStyle = bg; ctx.fill();

  // range rings + labels
  ctx.lineWidth = 1 * dpr; ctx.font = `${11*dpr}px ui-monospace,monospace`;
  ctx.fillStyle = 'rgba(125,138,154,.9)'; ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
  for (let m = 20; m <= MAX_RANGE; m += 20){
    const r = m / MAX_RANGE * R;
    ctx.beginPath(); ctx.arc(ox, oy, r, -Math.PI/2 - toRad(FOV), -Math.PI/2 + toRad(FOV));
    ctx.strokeStyle = m === MAX_RANGE ? 'rgba(63,182,255,.5)' : 'rgba(63,182,255,.22)'; ctx.stroke();
    const [lx, ly] = polar(FOV, m);
    const lbl = ` ${m} m`, lw = ctx.measureText(lbl).width;
    ctx.fillText(lbl, Math.min(lx + 4*dpr, w - lw - 2*dpr), ly + (m === MAX_RANGE ? 16*dpr : 0));
  }
  // angle spokes
  ctx.textAlign = 'center'; ctx.textBaseline = 'bottom';
  for (let a = -FOV; a <= FOV; a += 10){
    const [x, y] = polar(a, MAX_RANGE);
    ctx.beginPath(); ctx.moveTo(ox, oy); ctx.lineTo(x, y);
    ctx.strokeStyle = a === 0 ? 'rgba(63,182,255,.45)' : 'rgba(63,182,255,.18)'; ctx.stroke();
    const [tx, ty] = polar(a, MAX_RANGE * 1.04); ctx.fillText(`${a > 0 ? '+' : ''}${a}°`, tx, ty);
  }
  // rated FOV (±20°)
  ctx.setLineDash([5*dpr, 5*dpr]); ctx.strokeStyle = 'rgba(255,181,71,.55)';
  for (const a of [-RATED_FOV, RATED_FOV]){
    const [x, y] = polar(a, MAX_RANGE); ctx.beginPath(); ctx.moveTo(ox, oy); ctx.lineTo(x, y); ctx.stroke();
  }
  ctx.setLineDash([]);

  // sweep beam (oscillates across the sector)
  const sweep = Math.sin(ts / 900) * FOV;
  const g = ctx.createRadialGradient(ox, oy, 0, ox, oy, R);
  g.addColorStop(0, 'rgba(63,182,255,.0)'); g.addColorStop(1, 'rgba(63,182,255,.18)');
  wedge(ctx, R, sweep - 4, sweep + 4); ctx.fillStyle = g; ctx.fill();
  const [sx, sy] = polar(sweep, MAX_RANGE);
  ctx.beginPath(); ctx.moveTo(ox, oy); ctx.lineTo(sx, sy);
  ctx.strokeStyle = 'rgba(63,182,255,.7)'; ctx.lineWidth = 1.5 * dpr; ctx.stroke();

  // ghost trail
  const now = performance.now();
  while (trail.length && now - trail[0].t > TRAIL_MS) trail.shift();
  for (const fr of trail){
    const alpha = 0.35 * (1 - (now - fr.t) / TRAIL_MS);
    for (const p of fr.pts){
      const [x, y] = polar(p.ang, p.dist);
      ctx.beginPath(); ctx.arc(x, y, 3 * dpr, 0, Math.PI * 2);
      ctx.fillStyle = p.dir ? `rgba(255,77,94,${alpha})` : `rgba(47,209,138,${alpha})`; ctx.fill();
    }
  }

  // live targets
  ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
  const pulse = 1 + 0.25 * Math.sin(ts / 160);
  for (const t of (stale ? [] : targets)){
    const [x, y] = polar(t.ang, t.dist), col = t.dir ? '#ff4d5e' : '#2fd18a';
    ctx.beginPath(); ctx.arc(x, y, 12 * dpr * pulse, 0, Math.PI * 2);
    ctx.fillStyle = t.dir ? 'rgba(255,77,94,.15)' : 'rgba(47,209,138,.15)'; ctx.fill();
    ctx.save(); ctx.shadowColor = col; ctx.shadowBlur = 14 * dpr;
    ctx.beginPath(); ctx.arc(x, y, 6 * dpr, 0, Math.PI * 2); ctx.fillStyle = col; ctx.fill();
    ctx.restore();
    const label = `#${t.id} ${t.spd} km/h ${t.dir ? '↓' : '↑'}`;
    ctx.font = `600 ${12*dpr}px ui-monospace,monospace`;
    const tw = ctx.measureText(label).width, lx = Math.min(x + 12*dpr, w - tw - 8*dpr);
    ctx.fillStyle = 'rgba(11,15,20,.8)'; ctx.fillRect(lx - 4*dpr, y - 10*dpr, tw + 8*dpr, 20*dpr);
    ctx.fillStyle = col; ctx.fillText(label, lx, y);
  }
  // origin (sensor)
  ctx.beginPath(); ctx.arc(ox, oy, 5 * dpr, 0, Math.PI * 2); ctx.fillStyle = '#3fb6ff'; ctx.fill();
}

// ================= CHARTS =================
// Chart.js when available, otherwise a minimal canvas plotter with same API.
class MiniChart {
  constructor(canvas, color){ this.c = canvas; this.ctx = canvas.getContext('2d'); this.col = color; this.data = []; }
  push(x, y){ this.data.push({x, y}); }
  trim(min){ while (this.data.length && this.data[0].x < min) this.data.shift(); }
  draw(xmin, xmax){
    sizeCanvas(this.c);
    const ctx = this.ctx, dpr = window.devicePixelRatio || 1, w = this.c.width, h = this.c.height;
    const L = 38*dpr, B = 20*dpr, T = 8*dpr, Rr = 8*dpr;
    ctx.clearRect(0, 0, w, h);
    const ys = this.data.filter(p => p.y != null).map(p => p.y);
    const ymax = Math.max(10, Math.ceil((ys.length ? Math.max(...ys) : 10) / 10) * 10);
    const X = x => L + (x - xmin) / (xmax - xmin) * (w - L - Rr);
    const Y = y => h - B - y / ymax * (h - B - T);
    ctx.font = `${10*dpr}px ui-monospace,monospace`; ctx.fillStyle = '#7d8a9a';
    ctx.strokeStyle = 'rgba(36,48,65,1)'; ctx.lineWidth = dpr; ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
    for (let i = 0; i <= 4; i++){ const v = ymax * i / 4, y = Y(v);
      ctx.beginPath(); ctx.moveTo(L, y); ctx.lineTo(w - Rr, y); ctx.stroke(); ctx.fillText(v.toFixed(0), L - 5*dpr, y); }
    ctx.textAlign = 'center'; ctx.textBaseline = 'top';
    for (let s = 0; s <= WINDOW_S; s += 10) ctx.fillText(`-${WINDOW_S - s}s`, X(xmin + s), h - B + 4*dpr);
    ctx.strokeStyle = this.col; ctx.lineWidth = 2*dpr; ctx.beginPath(); let pen = false;
    for (const p of this.data){
      if (p.y == null){ pen = false; continue; }
      const x = X(p.x), y = Y(p.y); pen ? ctx.lineTo(x, y) : ctx.moveTo(x, y); pen = true;
    }
    ctx.stroke();
  }
}
function makeChart(id, color, label){
  const canvas = $(id);
  if (!window.Chart) return new MiniChart(canvas, color);
  Chart.defaults.color = '#7d8a9a'; Chart.defaults.borderColor = '#243041';
  const ch = new Chart(canvas, {
    type: 'line',
    data: { datasets: [{ label, data: [], borderColor: color, backgroundColor: color + '22',
      fill: true, borderWidth: 2, pointRadius: 0, tension: 0.25, spanGaps: false }] },
    options: { animation: false, responsive: true, maintainAspectRatio: false, parsing: false,
      normalized: true, interaction: { intersect: false, mode: 'nearest', axis: 'x' },
      plugins: { legend: { display: false },
        tooltip: { callbacks: { title: it => `${(it[0].parsed.x - nowS()).toFixed(1)} s` } } },
      scales: { x: { type: 'linear', ticks: { stepSize: 10, callback: v => `${Math.round(v - nowS())}s` },
                     grid: { color: '#1b2430' } },
                y: { beginAtZero: true, suggestedMax: 20, grid: { color: '#1b2430' } } } }
  });
  return {
    push: (x, y) => ch.data.datasets[0].data.push({x, y}),
    trim: min => { const d = ch.data.datasets[0].data; while (d.length && d[0].x < min) d.shift(); },
    draw: (xmin, xmax) => { ch.options.scales.x.min = xmin; ch.options.scales.x.max = xmax; ch.update('none'); }
  };
}
let distChart, spdChart;
function initCharts(){
  distChart = makeChart('distChart', '#3fb6ff', 'Distance (m)');
  spdChart  = makeChart('spdChart',  '#ffb547', 'Speed (km/h)');
  if (!window.Chart) $('cdnNote').textContent =
    'Chart.js CDN unreachable (offline / AP mode) – using built-in plotter.';
}

// ================= TABLE / PRIMARY =================
function primaryOf(list){
  return list.length ? list.reduce((a, b) => (b.dist < a.dist ? b : a)) : null;
}
function renderTable(){
  const tb = $('tbody'), list = stale ? [] : targets, p = primaryOf(list);
  $('tcount').textContent = `${list.length} target${list.length === 1 ? '' : 's'}`;
  if (!list.length){
    tb.innerHTML = '<tr class="empty"><td colspan="6">No targets</td></tr>';
  } else {
    tb.innerHTML = list.map(t => `<tr class="${t === p ? 'primary' : ''}">
      <td>${t.id}</td><td>${t.dist}</td><td>${t.spd}</td><td>${t.ang > 0 ? '+' : ''}${t.ang}</td>
      <td class="${t.dir ? 'dir-a' : 'dir-g'}">${t.dir ? '▼ Approaching' : '▲ Moving away'}</td>
      <td>${t.snr}</td></tr>`).join('');
  }
  $('pDist').textContent = p ? `${p.dist} m` : '--';
  $('pSpd').textContent  = p ? `${p.spd}` : '--';
  $('pAng').textContent  = p ? `${p.ang > 0 ? '+' : ''}${p.ang}°` : '--';
  $('pSpd').style.color  = p ? (p.dir ? 'var(--red)' : 'var(--green)') : '';
}

// ================= WEBSOCKET =================
let ws, retry = 500;
function setConn(ok){ const c = $('conn'); c.textContent = ok ? 'live' : 'disconnected'; c.className = 'pill ' + (ok ? 'ok' : 'bad'); }
function connect(){
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onopen = () => { setConn(true); retry = 500; };
  ws.onclose = () => { setConn(false); stale = true; renderTable();
    setTimeout(connect, retry); retry = Math.min(retry * 2, 8000); };
  ws.onerror = () => ws.close();
  ws.onmessage = ev => { let m; try { m = JSON.parse(ev.data); } catch (e) { return; }
    if (m.type === 'frame') onFrame(m); else if (m.type === 'status') onStatus(m); };
}
function onFrame(m){
  targets = m.targets || []; alarm = m.alarm; stale = false;
  lastFrameAt = performance.now(); frameTimes.push(lastFrameAt);
  trail.push({ t: lastFrameAt, pts: targets.map(t => ({ang: t.ang, dist: t.dist, dir: t.dir})) });
  $('alarm').classList.toggle('on', alarm === 1);
  $('raw').textContent = m.raw || '--';
  const p = primaryOf(targets), x = nowS();
  distChart.push(x, p ? p.dist : null); spdChart.push(x, p ? p.spd : null);
  renderTable();
}
function onStatus(s){
  if (s.stale){ stale = true; $('alarm').classList.remove('on'); renderTable(); }
  const md = $('mode'); md.textContent = s.mode === 'AP' ? 'AP mode' : `STA ${s.rssi} dBm`;
  md.className = 'pill ' + (s.mode === 'AP' || s.rssi > -70 ? 'ok' : 'warn');
  $('stats').innerHTML = `frames ${s.frames} · bytes ${s.bytes}<br>len err ${s.lenErr} · tail err ${s.tailErr} · dropped ${s.dropped}<br>heap ${(s.heap/1024).toFixed(0)} KB · up ${s.up}s · clients ${s.clients}`;
}

// ================= RENDER LOOP =================
let lastChartDraw = 0;
function tick(ts){
  drawRadar(ts);
  const now = performance.now();
  while (frameTimes.length && now - frameTimes[0] > 2000) frameTimes.shift();
  $('fps').textContent = `${(frameTimes.length / 2).toFixed(1)} fps`;
  $('age').textContent = lastFrameAt ? `${((now - lastFrameAt) / 1000).toFixed(1)} s ago` : '--';
  if (now - lastChartDraw > 150){             // ~7 Hz chart refresh
    lastChartDraw = now; const x = nowS();
    for (const c of [distChart, spdChart]){ c.trim(x - WINDOW_S); c.draw(x - WINDOW_S, x); }
  }
  requestAnimationFrame(tick);
}
window.addEventListener('resize', () => { geo = null; });
initCharts(); connect(); requestAnimationFrame(tick);
})();
</script>
</body>
</html>
)rawliteral";
