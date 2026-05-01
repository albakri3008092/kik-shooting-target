// =====================================================================
//  KIK Shooting Target — PROTOTYPE V2 (central receiver)
// =====================================================================
//  Improvements over demo (v1):
//    1. Receives HitPacketV2 with all 4 sensor amplitudes per hit, then
//       triangulates an (X, Y) impact point on the target face using a
//       weighted centroid of the per-sensor peaks. Each target is mapped
//       in the [-1, +1] x [-1, +1] normalized square; the dashboard
//       paints dots over a 10-ring ISSF target visualization.
//    2. Split-time tracking: every consecutive pair of hits on the same
//       target produces a "split" in milliseconds. The dashboard shows
//       the most recent split plus the running average — useful for
//       tactical / double-tap evaluation.
//    3. Score zones use ISSF rifle target rings (10X .. 1) computed from
//       the distance from center, not the simple S1=10 / S2=8 demo
//       mapping. Each hit's zone score is included in the ring buffer
//       and the per-target running total.
//    4. Sensor health monitoring kept from demo (HealthPacket type 3),
//       with the same OK / NOISY / BROKEN / UNKNOWN classification.
//
//  No external libraries required (ESP32 core only).
//  Embedded dashboard HTML is served from / over the AP at 192.168.4.1.
// =====================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>

// ---------- Config ----------
#define NUM_TARGETS       3
#define RECENT_N          24
static const char*  AP_SSID     = "Target_System_V2";
static const char*  AP_PASSWORD = "12345678";
static const uint8_t BUZZER_PIN = 25;
static const int     BUZZ_CH    = 0;

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #define KIK_BUZZ_SETUP(pin, ch, f, res) ledcAttach((pin), (f), (res))
  #define KIK_BUZZ_TONE(pin, ch, f)       ledcWriteTone((pin), (f))
#else
  #define KIK_BUZZ_SETUP(pin, ch, f, res) do { ledcSetup((ch), (f), (res)); ledcAttachPin((pin), (ch)); } while (0)
  #define KIK_BUZZ_TONE(pin, ch, f)       ledcWriteTone((ch), (f))
#endif

// Sensor positions in the normalized [-1, +1] target square.
//   S1 top-left  S2 top-right
//   S3 bot-left  S4 bot-right
// Triangulation is a peak-weighted centroid in this space.
static const float SENSOR_X[4] = { -1.f, +1.f, -1.f, +1.f };
static const float SENSOR_Y[4] = { +1.f, +1.f, -1.f, -1.f };

// ISSF rifle target ring radii (outer edge) in normalized [0, 1].
// Zone score == 11 - index. Ring 0 is bullseye (10X, scored as 11);
// the visualization maps 10X to a tighter inner ring inside ring 1.
static const float RING_R[11] = {
  0.05f, // 10X (inner ten)
  0.10f, // 10
  0.20f, // 9
  0.30f, // 8
  0.40f, // 7
  0.50f, // 6
  0.65f, // 5
  0.80f, // 4
  0.95f, // 3
  1.00f, // 2
  1.30f, // 1 (extreme outer; anything beyond is a miss)
};
static const uint8_t RING_SCORE[11] = { 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 };

#pragma pack(push, 1)
// MUST match target_v2.ino HitPacketV2.
struct HitPacketV2 {
  uint8_t  type;
  uint8_t  targetID;
  uint16_t hitSeq;
  uint32_t timestampMs;
  uint16_t peak[4];
  uint8_t  triggerSensor;
  uint8_t  windowMs;
};
struct HeartbeatPacket {
  uint8_t  type;
  uint8_t  targetID;
  uint16_t hitSeq;
};
struct HealthPacket {
  uint8_t  type;
  uint8_t  targetID;
  uint16_t baseline[4];
  uint16_t peak[4];
};
#pragma pack(pop)

// ---------- Per-target state ----------
struct TargetState {
  bool     online        = false;
  uint32_t lastSeen      = 0;
  uint32_t lastHitMs     = 0;
  uint16_t hitCount      = 0;
  uint32_t scoreSum      = 0;     // ISSF score sum (can exceed 16 bits over a long session)
  uint16_t lastHitSeq    = 0;
  uint8_t  lastTrigger   = 0;     // 1..4
  uint8_t  lastZone      = 0;     // 1..11 (11 = 10X)
  uint8_t  lastScore     = 0;
  float    lastX         = 0.f;
  float    lastY         = 0.f;
  uint16_t lastPeak[4]   = {0, 0, 0, 0};

  // Split-time tracking (between consecutive hits on this target).
  uint16_t lastSplitMs   = 0;     // most recent split
  uint16_t minSplitMs    = 0;     // best (smallest) split observed
  uint32_t splitSumMs    = 0;     // running sum for average
  uint16_t splitCount    = 0;     // number of splits recorded

  // Per-sensor health (HealthPacket).
  uint32_t lastHealthMs  = 0;
  uint16_t baseline[4]   = {0, 0, 0, 0};
  uint16_t peak[4]       = {0, 0, 0, 0};
};
static TargetState T[NUM_TARGETS + 1];   // index 1..NUM_TARGETS

// ---------- Recent hits ring buffer ----------
struct RecentHit {
  uint32_t ts;
  uint8_t  targetID;
  uint8_t  triggerSensor;     // 1..4
  uint8_t  zone;              // 1..11 (11 = 10X)
  uint8_t  score;             // ISSF ring score
  int16_t  x10;               // X * 1000 (normalized -1000..+1000)
  int16_t  y10;               // Y * 1000
  uint16_t splitMs;           // split from previous hit on same target
};
static RecentHit recentBuf[RECENT_N] = {};
static uint8_t   recentHead  = 0;
static uint16_t  recentCount = 0;

static WebServer server(80);
static volatile bool gHitPending = false;

// =====================================================================
//  Triangulation + scoring
// =====================================================================
// Peak-weighted centroid in the normalized [-1, +1] square. Returns
// (X, Y) and the magnitude r = sqrt(X^2 + Y^2) for ring lookup.
static void triangulate(const uint16_t peak[4], float& outX, float& outY) {
  uint32_t total = 0;
  for (int i = 0; i < 4; i++) total += peak[i];
  if (total == 0) { outX = 0.f; outY = 0.f; return; }
  float fx = 0.f, fy = 0.f;
  for (int i = 0; i < 4; i++) {
    float w = (float)peak[i] / (float)total;
    fx += w * SENSOR_X[i];
    fy += w * SENSOR_Y[i];
  }
  // Empirical gain: a perfectly centered hit puts equal weight on all 4
  // sensors so the centroid sits at (0, 0); a corner hit ~+/-0.6. Apply
  // a 1.6x stretch so the visualization fills the target area.
  const float kStretch = 1.6f;
  fx *= kStretch;
  fy *= kStretch;
  if (fx >  1.3f) fx =  1.3f;
  if (fx < -1.3f) fx = -1.3f;
  if (fy >  1.3f) fy =  1.3f;
  if (fy < -1.3f) fy = -1.3f;
  outX = fx;
  outY = fy;
}

// Map (X, Y) -> ISSF zone index (0=10X, 1=10, 2=9, ... 10=1) and score.
static void scoreZone(float x, float y, uint8_t& zoneIdx, uint8_t& score) {
  float r = sqrtf(x * x + y * y);
  for (int i = 0; i < 11; i++) {
    if (r <= RING_R[i]) {
      zoneIdx = (uint8_t)i;
      score   = RING_SCORE[i];
      return;
    }
  }
  zoneIdx = 11;   // off target
  score   = 0;
}

// =====================================================================
//  Dashboard (single-file HTML + CSS + JS, polls /status)
// =====================================================================
static const char INDEX_HTML[] PROGMEM = R"RAW(
<!doctype html>
<html lang="ms">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>KIK Target V2</title>
<style>
  :root{
    --bg:#0b1221;--surf:#162036;--surf2:#1e2b4a;--bd:#263556;
    --tx:#f4f6fb;--mut:#8896b8;
    --p:#00e5a8;--i:#4cc9f0;--a:#b56cff;--d:#ff4d6d;--w:#ffd43b;
    --s1:#ff4d6d;--s2:#ffd43b;--s3:#4cc9f0;--s4:#00e5a8;
  }
  *{box-sizing:border-box;margin:0;padding:0}
  body{
    font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif;
    background:radial-gradient(1200px 500px at 0% -10%,rgba(0,229,168,.14),transparent 60%),
               radial-gradient(1000px 500px at 100% 0%,rgba(181,108,255,.12),transparent 60%),
               var(--bg);
    color:var(--tx);min-height:100vh;padding:14px;
  }
  header{display:flex;align-items:center;gap:14px;margin-bottom:14px;flex-wrap:wrap}
  .logo{
    width:42px;height:42px;border-radius:12px;
    background:linear-gradient(135deg,var(--p),var(--i));
    display:grid;place-items:center;font-weight:900;font-size:19px;color:#0b1221;
  }
  h1{font-size:18px;letter-spacing:.4px}
  h1 small{color:var(--mut);font-weight:500;font-size:12px;display:block}
  .pill{
    margin-left:auto;display:inline-flex;align-items:center;gap:8px;
    padding:6px 12px;border:1px solid var(--bd);border-radius:999px;
    background:rgba(255,255,255,.04);font-size:12px;color:var(--mut);
  }
  .dot{width:8px;height:8px;border-radius:50%;background:var(--mut)}
  .dot.on{background:var(--p);box-shadow:0 0 10px var(--p)}
  .stat-row{display:grid;grid-template-columns:repeat(5,1fr);gap:10px;margin-bottom:14px}
  .stat{
    background:var(--surf);border:1px solid var(--bd);border-radius:14px;padding:10px 12px;
  }
  .stat .l{font-size:10px;color:var(--mut);text-transform:uppercase;letter-spacing:.6px}
  .stat .v{font-size:20px;font-weight:800;margin-top:2px}
  .stat .v small{font-size:11px;color:var(--mut);font-weight:500}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(330px,1fr));gap:14px}
  .card{
    background:var(--surf);border:1px solid var(--bd);border-radius:18px;
    padding:14px;display:flex;flex-direction:column;gap:10px;
  }
  .card.off{opacity:.4}
  .card-head{display:flex;align-items:center;gap:10px}
  .badge{
    background:var(--surf2);border:1px solid var(--bd);
    border-radius:999px;padding:4px 9px;font-size:11px;color:var(--mut);
  }
  .badge.online{color:var(--p);border-color:rgba(0,229,168,.4)}
  .ttl{font-size:15px;font-weight:800;letter-spacing:.4px}
  .ttl small{color:var(--mut);font-weight:500;font-size:11px}
  .target-vis{
    width:100%;aspect-ratio:1/1;background:#0d1628;
    border-radius:14px;border:1px solid var(--bd);position:relative;overflow:hidden;
  }
  .target-vis canvas{position:absolute;inset:0;width:100%;height:100%;display:block}
  .stats2{display:grid;grid-template-columns:repeat(2,1fr);gap:8px;font-size:12px}
  .stats2 .k{
    background:var(--surf2);border:1px solid var(--bd);border-radius:10px;
    padding:7px 9px;display:flex;align-items:center;gap:6px;
  }
  .stats2 .k b{color:var(--mut);text-transform:uppercase;font-size:10px;letter-spacing:.5px}
  .stats2 .k span{margin-left:auto;font-weight:800;font-size:14px}
  .health-row{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}
  .hp{
    border-radius:10px;border:1px solid var(--bd);padding:6px 4px;
    text-align:center;background:rgba(255,255,255,.02);
  }
  .hp .lbl{font-size:9px;color:var(--mut);text-transform:uppercase;letter-spacing:.5px}
  .hp .st{font-size:10px;font-weight:800;letter-spacing:.4px;margin-top:2px}
  .hp.ok     .st{color:var(--p)}
  .hp.noisy  .st{color:var(--w)}
  .hp.broken .st{color:var(--d);animation:blink 1s steps(2) infinite}
  .hp.unknown .st{color:var(--mut)}
  @keyframes blink{50%{opacity:.4}}
  .feed{
    background:var(--surf);border:1px solid var(--bd);border-radius:18px;padding:14px;
    margin-top:14px;
  }
  .feed h3{font-size:13px;color:var(--mut);text-transform:uppercase;letter-spacing:.6px;margin-bottom:8px}
  .row{
    display:grid;grid-template-columns:54px 60px 50px 70px 110px 70px;gap:8px;
    padding:7px 6px;border-bottom:1px dashed rgba(255,255,255,.06);
    font-size:12px;align-items:center;
  }
  .row:last-child{border-bottom:none}
  .row .t{font-weight:800}
  .row .s{font-weight:800;text-align:center;border-radius:8px;padding:2px 0}
  .row .s.s1{background:rgba(255,77,109,.15);color:var(--s1)}
  .row .s.s2{background:rgba(255,212,59,.15);color:var(--s2)}
  .row .s.s3{background:rgba(76,201,240,.15);color:var(--s3)}
  .row .s.s4{background:rgba(0,229,168,.15);color:var(--s4)}
  .row .z{color:var(--mut);font-size:11px}
  .row .xy{color:var(--mut);font-family:Menlo,monospace;font-size:11px}
  .row .sp{color:var(--w);text-align:right;font-family:Menlo,monospace}
  .actions{margin-top:12px;display:flex;gap:8px;flex-wrap:wrap}
  button{
    background:var(--surf2);border:1px solid var(--bd);color:var(--tx);
    padding:8px 14px;border-radius:10px;font-weight:700;cursor:pointer;
  }
  button:hover{background:var(--bd)}
  button.danger{color:var(--d);border-color:rgba(255,77,109,.4)}
</style>
</head>
<body>
<header>
  <div class="logo">KT</div>
  <h1>KIK Target — Prototype V2 <small>4-sensor + triangulation + split-time</small></h1>
  <div class="pill"><span class="dot" id="dot"></span><span id="conn">Menyambung…</span></div>
</header>
<div class="stat-row">
  <div class="stat"><div class="l">Sasaran Aktif</div><div class="v" id="sa">0/0</div></div>
  <div class="stat"><div class="l">Jumlah Hit</div><div class="v" id="sh">0</div></div>
  <div class="stat"><div class="l">Jumlah Skor</div><div class="v" id="ss">0</div></div>
  <div class="stat"><div class="l">Avg Split</div><div class="v" id="avgs">— <small>ms</small></div></div>
  <div class="stat"><div class="l">Best Split</div><div class="v" id="bests">— <small>ms</small></div></div>
</div>
<div class="grid" id="grid"></div>
<div class="feed">
  <h3>Tembakan Terkini</h3>
  <div id="feed"></div>
  <div class="actions">
    <button onclick="resetAll()" class="danger">Reset Sesi</button>
  </div>
</div>
<script>
const N = 3;
const POLL_MS = 200;
const RING_R = [0.05,0.10,0.20,0.30,0.40,0.50,0.65,0.80,0.95,1.00];
function fmtAgo(ms){
  if(!ms || ms<1000) return 'baru';
  const s = Math.round(ms/1000);
  if(s<60) return s+'s';
  return Math.round(s/60)+'m';
}
function buildGrid(){
  const g = document.getElementById('grid');
  g.innerHTML = '';
  for(let i=1;i<=N;i++){
    const c = document.createElement('div');
    c.className = 'card off';
    c.id = 'c'+i;
    c.innerHTML = `
      <div class="card-head">
        <div class="ttl">Sasaran ${i} <small>4 piezo</small></div>
        <div class="badge" id="bd${i}">offline</div>
      </div>
      <div class="target-vis"><canvas id="cv${i}" width="240" height="240"></canvas></div>
      <div class="stats2">
        <div class="k"><b>Hit</b><span id="h${i}">0</span></div>
        <div class="k"><b>Skor</b><span id="sc${i}">0</span></div>
        <div class="k"><b>Last Split</b><span id="ls${i}">—</span></div>
        <div class="k"><b>Avg Split</b><span id="as${i}">—</span></div>
        <div class="k"><b>Last Zon</b><span id="lz${i}">—</span></div>
        <div class="k"><b>Last Trig</b><span id="lt${i}">—</span></div>
      </div>
      <div class="health-row">
        <div class="hp unknown" id="hp${i}_1"><div class="lbl">S1</div><div class="st">?</div></div>
        <div class="hp unknown" id="hp${i}_2"><div class="lbl">S2</div><div class="st">?</div></div>
        <div class="hp unknown" id="hp${i}_3"><div class="lbl">S3</div><div class="st">?</div></div>
        <div class="hp unknown" id="hp${i}_4"><div class="lbl">S4</div><div class="st">?</div></div>
      </div>
    `;
    g.appendChild(c);
  }
}
const dotsHistory = {};   // {targetId: [{x,y,score,ts}, ...]}
function drawTarget(i, t){
  const cv = document.getElementById('cv'+i);
  if(!cv) return;
  const ctx = cv.getContext('2d');
  const W = cv.width, H = cv.height;
  const cx = W/2, cy = H/2, R = Math.min(W,H)/2 * 0.95;
  ctx.fillStyle = '#0d1628';
  ctx.fillRect(0,0,W,H);
  // Draw concentric ring scoring
  for(let r=RING_R.length-1;r>=0;r--){
    const rr = RING_R[r] * R;
    ctx.beginPath();
    ctx.arc(cx, cy, rr, 0, Math.PI*2);
    const fillByRing = ['#ff4d6d','#ff8fa3','#ffb3c1','#ffd43b','#ffe066','#fff3bf','#a5d8ff','#74c0fc','#4dabf7','#1971c2'];
    ctx.fillStyle = r % 2 === 0 ? '#0d1628' : 'rgba(255,255,255,0.03)';
    ctx.fill();
    ctx.strokeStyle = 'rgba(255,255,255,0.2)';
    ctx.lineWidth = 1;
    ctx.stroke();
  }
  // Bullseye dot (10X inner)
  ctx.beginPath();
  ctx.arc(cx, cy, RING_R[0]*R, 0, Math.PI*2);
  ctx.fillStyle = '#ffd43b';
  ctx.fill();
  // Sensor markers (S1..S4 at corners)
  ctx.fillStyle = '#8896b8';
  ctx.font = '10px monospace';
  ctx.fillText('S1', cx - R*0.95, cy - R*0.85);
  ctx.fillText('S2', cx + R*0.85, cy - R*0.85);
  ctx.fillText('S3', cx - R*0.95, cy + R*0.95);
  ctx.fillText('S4', cx + R*0.85, cy + R*0.95);
  // Hit dots from history (older = faded)
  const hist = dotsHistory[i] || [];
  for(let k=0;k<hist.length;k++){
    const d = hist[k];
    const age = (hist.length - 1 - k);
    const alpha = Math.max(0.18, 1 - age*0.06);
    const px = cx + d.x * R;
    const py = cy - d.y * R;     // flip Y so +Y is up on screen
    ctx.beginPath();
    ctx.arc(px, py, 5, 0, Math.PI*2);
    ctx.fillStyle = 'rgba(255,77,109,'+alpha+')';
    ctx.fill();
    ctx.strokeStyle = 'rgba(255,255,255,0.6)';
    ctx.lineWidth = 1;
    ctx.stroke();
  }
}
function pushDot(i, x, y, score){
  if(!dotsHistory[i]) dotsHistory[i] = [];
  dotsHistory[i].push({x:x, y:y, score:score, ts:Date.now()});
  if(dotsHistory[i].length > 30) dotsHistory[i].shift();
}
function clearDots(i){ dotsHistory[i] = []; }
const lastSeq = {};
async function tick(){
  try{
    const r = await fetch('/status');
    const d = await r.json();
    document.getElementById('dot').classList.add('on');
    document.getElementById('conn').textContent = 'Tersambung';
    let active = 0, hits = 0, score = 0, splitSum = 0, splitN = 0, bestSplit = null;
    for(let i=1;i<=N;i++){
      const t = (d.targets || []).find(x=>x.id===i);
      const card = document.getElementById('c'+i);
      const bd = document.getElementById('bd'+i);
      if(!t || !t.online){
        card.classList.add('off');
        bd.textContent = 'offline';
        bd.classList.remove('online');
        continue;
      }
      card.classList.remove('off');
      bd.textContent = 'online';
      bd.classList.add('online');
      active++;
      hits  += t.hits||0;
      score += t.scoreSum||0;
      if(t.splitN > 0){
        splitSum += t.splitSum||0;
        splitN   += t.splitN;
      }
      if(t.minSplit && (bestSplit===null || t.minSplit < bestSplit)) bestSplit = t.minSplit;
      // Per-target stats
      document.getElementById('h'+i).textContent  = t.hits||0;
      document.getElementById('sc'+i).textContent = t.scoreSum||0;
      document.getElementById('ls'+i).textContent = (t.lastSplit||t.lastSplit===0)
          ? (t.lastSplit + ' ms') : '—';
      document.getElementById('as'+i).textContent = (t.splitN > 0)
          ? Math.round(t.splitSum / t.splitN) + ' ms' : '—';
      document.getElementById('lz'+i).textContent = t.lastZoneLabel || '—';
      document.getElementById('lt'+i).textContent = t.lastTrigger ? ('S'+t.lastTrigger) : '—';
      // Sensor health badges
      const healthLabel = {ok:'OK', noisy:'NOISY', broken:'ROSAK', unknown:'?'};
      for(let s=1;s<=4;s++){
        const sd = (t.sensors || [])[s-1] || {};
        const h = sd.h || 'unknown';
        const el = document.getElementById('hp'+i+'_'+s);
        if(!el) continue;
        el.classList.remove('ok','noisy','broken','unknown');
        el.classList.add(h);
        el.querySelector('.st').textContent = healthLabel[h];
        el.title = 'baseline='+(sd.bl||0)+'  peak='+(sd.pk||0);
      }
      // New hit since last poll? Push new dot.
      const last = lastSeq[i] || 0;
      if(t.lastHitSeq && t.lastHitSeq > last){
        pushDot(i, t.lastX || 0, t.lastY || 0, t.lastScore || 0);
        lastSeq[i] = t.lastHitSeq;
      }
      drawTarget(i, t);
    }
    document.getElementById('sh').textContent = hits;
    document.getElementById('ss').textContent = score;
    document.getElementById('sa').textContent = active+'/'+N;
    document.getElementById('avgs').innerHTML = (splitN > 0)
        ? Math.round(splitSum/splitN) + ' <small>ms</small>'
        : '— <small>ms</small>';
    document.getElementById('bests').innerHTML = bestSplit !== null
        ? bestSplit + ' <small>ms</small>'
        : '— <small>ms</small>';
    renderFeed(d.recent || []);
  }catch(e){
    document.getElementById('dot').classList.remove('on');
    document.getElementById('conn').textContent = 'Terputus';
  }
}
function renderFeed(recent){
  const f = document.getElementById('feed');
  f.innerHTML = '';
  if(!recent || recent.length === 0){
    f.innerHTML = '<div style="color:var(--mut);font-size:12px;padding:8px">Belum ada tembakan…</div>';
    return;
  }
  for(let k=recent.length-1;k>=0;k--){
    const r = recent[k];
    const row = document.createElement('div');
    row.className = 'row';
    const x = (r.x10/1000).toFixed(2);
    const y = (r.y10/1000).toFixed(2);
    row.innerHTML = `
      <div class="t">T${r.t}</div>
      <div class="s s${r.trig}">S${r.trig}</div>
      <div class="z">Z${r.zone}=${r.score}</div>
      <div class="z">${zoneLabel(r.zone)}</div>
      <div class="xy">x=${x} y=${y}</div>
      <div class="sp">${r.split>=0 ? r.split+'ms' : '—'}</div>
    `;
    f.appendChild(row);
  }
}
function zoneLabel(z){
  // 0=10X 1=10 2=9 ... 10=1, 11=miss
  if(z === 0) return '10X';
  if(z >= 1 && z <= 10) return ''+(11-z);
  return 'M';
}
async function resetAll(){
  if(!confirm('Reset semua kaunter & dot history?')) return;
  await fetch('/reset', {method:'POST'});
  for(let i=1;i<=N;i++){ clearDots(i); lastSeq[i] = 0; }
}
buildGrid();
setInterval(tick, POLL_MS);
tick();
</script>
</body>
</html>
)RAW";

// =====================================================================
//  ESP-NOW receive callback (core 2.x and 3.x signature shim)
// =====================================================================
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void onEspNow(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  (void)info;
#else
static void onEspNow(const uint8_t* mac, const uint8_t* data, int len) {
  (void)mac;
#endif
  if (len < 2) return;
  const uint8_t ptype = data[0];
  const uint8_t tid   = data[1];
  if (tid < 1 || tid > NUM_TARGETS) return;
  TargetState& t = T[tid];

  if (ptype == 3) {
    if (len != (int)sizeof(HealthPacket)) return;
    HealthPacket h; memcpy(&h, data, sizeof(h));
    t.online       = true;
    t.lastSeen     = millis();
    t.lastHealthMs = millis();
    for (int i = 0; i < 4; i++) {
      t.baseline[i] = h.baseline[i];
      t.peak[i]     = h.peak[i];
    }
    return;
  }
  if (ptype == 2) {
    if (len != (int)sizeof(HeartbeatPacket)) return;
    HeartbeatPacket h; memcpy(&h, data, sizeof(h));
    t.online   = true;
    t.lastSeen = millis();
    return;
  }
  if (ptype != 1) return;
  if (len != (int)sizeof(HitPacketV2)) return;

  HitPacketV2 p; memcpy(&p, data, sizeof(p));
  t.online   = true;
  t.lastSeen = millis();

  // Triangulate position + score zone.
  float x, y;
  triangulate(p.peak, x, y);
  uint8_t zoneIdx, score;
  scoreZone(x, y, zoneIdx, score);

  // Split time: gap from last hit on this target. First hit -> 0.
  uint32_t now = millis();
  uint16_t splitMs = 0;
  if (t.hitCount > 0 && t.lastHitMs != 0) {
    uint32_t dt = now - t.lastHitMs;
    splitMs = dt > 0xFFFFu ? 0xFFFFu : (uint16_t)dt;
    t.splitSumMs += splitMs;
    t.splitCount++;
    if (t.minSplitMs == 0 || splitMs < t.minSplitMs) t.minSplitMs = splitMs;
  }
  t.lastSplitMs = splitMs;

  t.hitCount++;
  t.scoreSum    += score;
  t.lastHitMs    = now;
  t.lastHitSeq   = p.hitSeq;
  t.lastTrigger  = p.triggerSensor;
  t.lastZone     = zoneIdx;
  t.lastScore    = score;
  t.lastX        = x;
  t.lastY        = y;
  for (int i = 0; i < 4; i++) t.lastPeak[i] = p.peak[i];

  // Append to ring buffer.
  RecentHit& r = recentBuf[recentHead];
  r.ts            = now;
  r.targetID      = tid;
  r.triggerSensor = p.triggerSensor;
  r.zone          = zoneIdx;
  r.score         = score;
  r.x10           = (int16_t)(x * 1000.f);
  r.y10           = (int16_t)(y * 1000.f);
  r.splitMs       = splitMs;
  recentHead = (recentHead + 1) % RECENT_N;
  if (recentCount < 0xFFFF) recentCount++;

  gHitPending = true;
}

// =====================================================================
//  HTTP handlers
// =====================================================================
static void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

// Helper: JSON-escaped status of one sensor's health.
static const char* sensorHealthLabel(uint16_t baselineV, uint32_t healthAge,
                                     bool haveHealth) {
  if (!haveHealth)         return "unknown";
  if (baselineV >= 3000)   return "broken";
  if (baselineV >= 1000)   return "noisy";
  return "ok";
  (void)healthAge;
}

static void handleStatus() {
  String j;
  j.reserve(2048);
  j += "{\"targets\":[";
  for (int i = 1; i <= NUM_TARGETS; i++) {
    TargetState& t = T[i];
    if (i > 1) j += ",";
    j += "{\"id\":";          j += i;
    j += ",\"online\":";      j += (t.online ? "true" : "false");
    j += ",\"hits\":";        j += t.hitCount;
    j += ",\"scoreSum\":";    j += (uint32_t)t.scoreSum;
    j += ",\"lastHitSeq\":";  j += t.lastHitSeq;
    j += ",\"lastTrigger\":"; j += t.lastTrigger;
    j += ",\"lastZone\":";    j += t.lastZone;
    j += ",\"lastZoneLabel\":\"";
    if (t.lastZone == 0)               j += "10X";
    else if (t.lastZone >= 1 && t.lastZone <= 10) j += String(11 - t.lastZone);
    else                               j += "M";
    j += "\"";
    j += ",\"lastScore\":";   j += t.lastScore;
    // Floats serialized with 3 decimals.
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f", t.lastX); j += ",\"lastX\":"; j += buf;
    snprintf(buf, sizeof(buf), "%.3f", t.lastY); j += ",\"lastY\":"; j += buf;
    j += ",\"lastSplit\":";   j += t.lastSplitMs;
    j += ",\"minSplit\":";    j += t.minSplitMs;
    j += ",\"splitSum\":";    j += (uint32_t)t.splitSumMs;
    j += ",\"splitN\":";      j += t.splitCount;
    j += ",\"ago\":";         j += (uint32_t)(t.lastHitMs ? (millis() - t.lastHitMs) : 0);
    // Per-sensor health.
    uint32_t healthAge = t.lastHealthMs ? (millis() - t.lastHealthMs) : 0xFFFFFFFFu;
    bool haveHealth = t.lastHealthMs && healthAge < 8000;
    j += ",\"healthAge\":"; j += (uint32_t)healthAge;
    j += ",\"sensors\":[";
    for (int s = 0; s < 4; s++) {
      if (s) j += ",";
      uint16_t bl = t.baseline[s];
      uint16_t pk = t.peak[s];
      j += "{\"bl\":" ;  j += bl;
      j += ",\"pk\":" ; j += pk;
      j += ",\"h\":\""; j += sensorHealthLabel(bl, healthAge, haveHealth);
      j += "\"}";
    }
    j += "]}";
  }
  j += "],\"recent\":[";
  // Walk ring buffer in chronological order (oldest first).
  uint16_t n = recentCount < RECENT_N ? recentCount : RECENT_N;
  for (uint16_t k = 0; k < n; k++) {
    uint8_t idx = (uint8_t)((recentHead + RECENT_N - n + k) % RECENT_N);
    const RecentHit& r = recentBuf[idx];
    if (k > 0) j += ",";
    j += "{\"t\":";        j += r.targetID;
    j += ",\"trig\":";     j += r.triggerSensor;
    j += ",\"zone\":";     j += r.zone;
    j += ",\"score\":";    j += r.score;
    j += ",\"x10\":";      j += r.x10;
    j += ",\"y10\":";      j += r.y10;
    j += ",\"split\":";    j += r.splitMs;
    j += ",\"ts\":";       j += (uint32_t)r.ts;
    j += "}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

static void handleReset() {
  for (int i = 1; i <= NUM_TARGETS; i++) {
    TargetState& t = T[i];
    t.hitCount = 0;
    t.scoreSum = 0;
    t.lastHitSeq = 0;
    t.lastHitMs = 0;
    t.lastTrigger = 0;
    t.lastZone = 0;
    t.lastScore = 0;
    t.lastX = 0; t.lastY = 0;
    t.lastSplitMs = 0;
    t.minSplitMs = 0;
    t.splitSumMs = 0;
    t.splitCount = 0;
    for (int k = 0; k < 4; k++) t.lastPeak[k] = 0;
  }
  recentHead = 0; recentCount = 0;
  for (int k = 0; k < RECENT_N; k++) recentBuf[k] = RecentHit{};
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleNotFound() {
  server.send(404, "text/plain", "404");
}

// =====================================================================
//  Setup / loop
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  KIK_BUZZ_SETUP(BUZZER_PIN, BUZZ_CH, 4000, 8);
  KIK_BUZZ_TONE(BUZZER_PIN, BUZZ_CH, 0);

  // WiFi softAP (B/G/N only — DROP LR so phones/tablets see the SSID).
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD, /*channel=*/1, /*hidden=*/0,
              /*max_conn=*/4);
  esp_wifi_set_protocol(WIFI_IF_AP,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  esp_wifi_set_ps(WIFI_PS_NONE);

  Serial.println();
  Serial.println("KIK Central V2 ready.");
  Serial.printf("MAC:      %s   <-- paste this into target_v2 RECEIVER_MAC\n",
                WiFi.softAPmacAddress().c_str());
  Serial.printf("AP SSID:  %s\n", AP_SSID);
  Serial.println("AP IP:    192.168.4.1");
  Serial.println("Open http://192.168.4.1/ on your tablet.");

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed"); while (1) delay(500);
  }
  esp_now_register_recv_cb(onEspNow);

  server.on("/",       HTTP_GET,  handleRoot);
  server.on("/status", HTTP_GET,  handleStatus);
  server.on("/reset",  HTTP_POST, handleReset);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  server.handleClient();

  if (gHitPending) {
    gHitPending = false;
    KIK_BUZZ_TONE(BUZZER_PIN, BUZZ_CH, 4000);
    delay(20);
    KIK_BUZZ_TONE(BUZZER_PIN, BUZZ_CH, 0);
  }

  // Mark targets offline after silence.
  uint32_t now = millis();
  for (int i = 1; i <= NUM_TARGETS; i++) {
    TargetState& t = T[i];
    if (t.online && (now - t.lastSeen) > 8000) {
      t.online = false;
    }
  }
}
