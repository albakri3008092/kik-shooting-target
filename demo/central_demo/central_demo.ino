// =====================================================================
//  KIK Shooting Target — DEMO (central receiver, v1 simple)
// =====================================================================
//  Flash this sketch ONCE to the central ESP32 board.
//  No external libraries required — ESP32 core only.
//  No LittleFS / SPIFFS upload step — the dashboard HTML is embedded
//  right inside this sketch.
//
//  How to use:
//   1. Upload this sketch to the central ESP32.
//   2. On your phone/tablet: connect to Wi-Fi `Target_System` (pwd 12345678).
//   3. Open http://192.168.4.1/ — the colourful dashboard appears.
//   4. Power on the 3 target boards; their hits show up live.
//
//  Optional: wire a small passive buzzer to GPIO25 → GND for an audible
//  hit beep. If you don't have a buzzer, leave it unconnected.
// =====================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ---------- Config ----------
#define NUM_TARGETS       3
static const char*  AP_SSID     = "Target_System";
static const char*  AP_PASSWORD = "12345678";
static const uint8_t BUZZER_PIN = 25;          // GND through a passive buzzer
static const int     BUZZ_CH    = 0;           // LEDC channel (only used on core 2.x)

// ESP32 Arduino core 2.x vs 3.x LEDC API shim.
// 2.x: ledcSetup(ch, f, res) + ledcAttachPin(pin, ch); ledcWriteTone(ch, f)
// 3.x: ledcAttach(pin, f, res); ledcWriteTone(pin, f)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #define KIK_BUZZ_SETUP(pin, ch, f, res) ledcAttach((pin), (f), (res))
  #define KIK_BUZZ_TONE(pin, ch, f)       ledcWriteTone((pin), (f))
#else
  #define KIK_BUZZ_SETUP(pin, ch, f, res) do { ledcSetup((ch), (f), (res)); ledcAttachPin((pin), (ch)); } while (0)
  #define KIK_BUZZ_TONE(pin, ch, f)       ledcWriteTone((ch), (f))
#endif

#pragma pack(push, 1)
struct HitPacket {            // MUST match target_demo.ino
  uint8_t  type;              // 1 = hit, 2 = heartbeat
  uint8_t  targetID;
  uint8_t  sensorID;
  uint8_t  zone;
  uint8_t  score;
  uint16_t total;
  uint16_t amp;
};
#pragma pack(pop)

struct TargetState {
  bool     online     = false;
  uint32_t lastSeen   = 0;
  uint32_t lastHitMs  = 0;
  uint16_t hitCount   = 0;
  uint16_t score      = 0;
  uint16_t zoneHits[4] = {0, 0, 0, 0};    // Z10, Z8, Z6, Z4 (== S1..S4)
  uint16_t lastAmp    = 0;
  uint8_t  lastSensor = 0;            // 1..4 of the piezo that detected
};
static TargetState T[NUM_TARGETS + 1];    // index 1..NUM_TARGETS

// Rolling log of the last RECENT_N hits across all targets. The tablet
// shows these as "Tembakan Terkini" so the audience can see exactly
// which sensor on which target caught each bullet, in order.
#define RECENT_N 16
struct RecentHit {
  uint32_t ts;        // millis() at capture
  uint8_t  targetID;
  uint8_t  sensorID;  // 1..4
  uint8_t  zone;      // 1..4 (== sensorID in this demo)
  uint8_t  score;     // 10 / 8 / 6 / 4
};
static RecentHit recentBuf[RECENT_N] = {};
static uint8_t   recentHead = 0;     // next slot to write
static uint16_t  recentCount = 0;    // total hits since boot (caps the view)

static WebServer server(80);
static volatile bool  gHitPending = false;
static volatile uint8_t gBuzzLen  = 0;    // ms remaining

// =====================================================================
//  Embedded dashboard (single-file HTML + CSS + JS, polls /status)
// =====================================================================
static const char INDEX_HTML[] PROGMEM = R"RAW(
<!doctype html>
<html lang="ms">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>KIK Target — Demo</title>
<style>
  :root{
    --bg:#0b1221;--surf:#162036;--surf2:#1e2b4a;--bd:#263556;
    --tx:#f4f6fb;--mut:#8896b8;
    --p:#00e5a8;--i:#4cc9f0;--a:#b56cff;--d:#ff4d6d;--w:#ffd43b;
    --r10:#ff4d6d;--r8:#ffd43b;--r6:#4cc9f0;--r4:#00e5a8;
  }
  *{box-sizing:border-box;margin:0;padding:0}
  body{
    font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif;
    background:radial-gradient(1200px 500px at 0% -10%,rgba(0,229,168,.14),transparent 60%),
               radial-gradient(1000px 500px at 100% 0%,rgba(181,108,255,.12),transparent 60%),
               var(--bg);
    color:var(--tx);min-height:100vh;padding:14px;
  }
  header{
    display:flex;align-items:center;gap:14px;margin-bottom:14px;
  }
  .logo{
    width:42px;height:42px;border-radius:12px;
    background:conic-gradient(from 180deg,var(--p),var(--i),var(--a),var(--d),var(--p));
    display:grid;place-items:center;
  }
  .logo svg{width:26px;height:26px;stroke:#05121c;fill:none;stroke-width:2.6}
  h1{font-size:18px;letter-spacing:.4px}
  .sub{color:var(--mut);font-size:12px;margin-top:2px}
  .spacer{flex:1}
  .conn{display:flex;align-items:center;gap:6px;font-size:12px;color:var(--mut)}
  .dot{width:10px;height:10px;border-radius:50%;background:var(--mut)}
  .dot.on{background:var(--p);box-shadow:0 0 8px var(--p)}
  button{
    font:inherit;cursor:pointer;border:0;padding:10px 14px;border-radius:10px;
    background:var(--surf2);color:var(--tx);border:1px solid var(--bd);font-weight:600;
  }
  button.d{background:var(--d);color:#fff;border:0}
  button:active{transform:scale(.97)}
  .sum{
    display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-bottom:14px;
  }
  .card{
    background:var(--surf);border:1px solid var(--bd);border-radius:14px;padding:14px;
  }
  .card .l{font-size:11px;color:var(--mut);text-transform:uppercase;letter-spacing:.8px}
  .card .v{font-size:30px;font-weight:900;font-variant-numeric:tabular-nums}
  .card.p .v{color:var(--p)}
  .card.i .v{color:var(--i)}
  .card.a .v{color:var(--a)}
  .grid{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}
  @media (max-width:720px){.grid,.sum{grid-template-columns:1fr}}
  .tc{
    background:var(--surf);border:1px solid var(--bd);border-radius:14px;padding:14px;
    position:relative;overflow:hidden;transition:.2s;
  }
  .tc.off{opacity:.5;filter:saturate(.3)}
  .tc.hit{box-shadow:0 0 0 3px var(--p),0 0 30px rgba(0,229,168,.6)}
  .tc .head{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px}
  .tc .ttl{font-weight:800;font-size:18px}
  .tc .pill{
    background:linear-gradient(135deg,var(--d),#d13458);color:#fff;padding:4px 12px;
    border-radius:999px;font-weight:800;font-size:16px;
  }
  .lsens{
    display:flex;align-items:center;justify-content:space-between;gap:8px;
    background:var(--surf2);border:1px solid var(--bd);border-radius:10px;
    padding:8px 10px;margin-bottom:10px;
  }
  .lsens .k{font-size:10px;color:var(--mut);text-transform:uppercase;letter-spacing:.8px}
  .lsens .v{
    font-size:22px;font-weight:900;letter-spacing:.5px;
    padding:2px 10px;border-radius:8px;background:#0b1221;color:var(--mut);
  }
  .lsens .v.s1{color:#05121c;background:var(--r10)}
  .lsens .v.s2{color:#05121c;background:var(--r8)}
  .lsens .v.s3{color:#05121c;background:var(--r6)}
  .lsens .v.s4{color:#05121c;background:var(--r4)}
  .lsens .ago{font-size:11px;color:var(--mut);font-variant-numeric:tabular-nums}
  .bull{aspect-ratio:1;background:radial-gradient(circle at 50% 50%,var(--surf2),var(--surf));border-radius:12px;margin-bottom:10px;overflow:hidden}
  .bull svg{width:100%;height:100%;display:block}
  .bull .dot{filter:drop-shadow(0 0 6px currentColor);animation:pop .3s ease}
  .zones{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}
  .z{background:var(--surf2);border-radius:8px;padding:6px 4px;text-align:center;transition:.15s}
  .z .s{font-size:12px;font-weight:900;letter-spacing:.4px}
  .z .k{font-size:9px;color:var(--mut);text-transform:uppercase;letter-spacing:.5px;margin-top:2px}
  .z .n{font-size:20px;font-weight:900;font-variant-numeric:tabular-nums;margin-top:2px}
  .z.z10{border-top:3px solid var(--r10)} .z.z10 .n,.z.z10 .s{color:var(--r10)}
  .z.z8 {border-top:3px solid var(--r8) } .z.z8 .n, .z.z8 .s{color:var(--r8)}
  .z.z6 {border-top:3px solid var(--r6) } .z.z6 .n, .z.z6 .s{color:var(--r6)}
  .z.z4 {border-top:3px solid var(--r4) } .z.z4 .n, .z.z4 .s{color:var(--r4)}
  .z.flash{transform:scale(1.08);box-shadow:0 0 0 2px currentColor,0 0 16px currentColor}
  .feed{
    margin-top:14px;background:var(--surf);border:1px solid var(--bd);
    border-radius:14px;padding:14px;
  }
  .feed h2{font-size:14px;margin-bottom:10px;letter-spacing:.4px}
  .feed ul{list-style:none;display:flex;flex-direction:column;gap:6px;max-height:240px;overflow:auto}
  .feed li{
    display:grid;grid-template-columns:40px 60px 1fr auto auto;gap:10px;align-items:center;
    background:var(--surf2);border-radius:10px;padding:8px 10px;font-size:13px;
    animation:slide .25s ease;
  }
  .feed .t{font-weight:900;color:var(--i)}
  .feed .s{font-weight:900;padding:2px 8px;border-radius:6px;color:#05121c;text-align:center}
  .feed .s.s1{background:var(--r10)} .feed .s.s2{background:var(--r8)}
  .feed .s.s3{background:var(--r6)}  .feed .s.s4{background:var(--r4)}
  .feed .z{color:var(--mut)}
  .feed .sc{font-weight:900;color:var(--p);font-variant-numeric:tabular-nums}
  .feed .ago{color:var(--mut);font-size:11px;font-variant-numeric:tabular-nums;min-width:42px;text-align:right}
  .feed .empty{color:var(--mut);text-align:center;padding:18px;font-style:italic}
  footer{margin-top:18px;text-align:center;color:var(--mut);font-size:12px}
  @keyframes pop{0%{transform:scale(.3);opacity:0}60%{transform:scale(1.3);opacity:1}100%{transform:scale(1)}}
  @keyframes slide{from{transform:translateY(-6px);opacity:0}to{transform:translateY(0);opacity:1}}
</style>
</head>
<body>
<header>
  <div class="logo">
    <svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="5"/><circle cx="12" cy="12" r="1.6" fill="#05121c"/></svg>
  </div>
  <div>
    <h1>KIK Target — Demo</h1>
    <div class="sub">Sistem Sasaran Menembak · 3 Sasaran</div>
  </div>
  <div class="spacer"></div>
  <div class="conn"><span id="dot" class="dot"></span><span id="conn">—</span></div>
  <button class="d" onclick="resetAll()">↻ Reset</button>
</header>

<div class="sum">
  <div class="card p"><div class="l">Jumlah Tembakan</div><div class="v" id="sh">0</div></div>
  <div class="card i"><div class="l">Jumlah Skor</div>    <div class="v" id="ss">0</div></div>
  <div class="card a"><div class="l">Sasaran Aktif</div>  <div class="v" id="sa">0/3</div></div>
</div>

<div class="grid" id="grid"></div>

<footer>KIK v1 — sambung ke Wi-Fi <b>Target_System</b> / <b>12345678</b> · buka <b>http://192.168.4.1/</b></footer>

<script>
const N=3;
const grid=document.getElementById('grid');
function bull(t){
  return `<svg viewBox="-100 -100 200 200">
    <path d="M-100,-100 L0,-100 L0,0 L-100,0 Z" fill="var(--r10)" opacity=".10"/>
    <path d="M0,-100 L100,-100 L100,0 L0,0 Z"   fill="var(--r8)"  opacity=".10"/>
    <path d="M-100,0 L0,0 L0,100 L-100,100 Z"  fill="var(--r6)"  opacity=".10"/>
    <path d="M0,0 L100,0 L100,100 L0,100 Z"    fill="var(--r4)"  opacity=".10"/>
    <circle cx="0" cy="0" r="85" fill="none" stroke="var(--r4)" stroke-width="1" opacity=".6"/>
    <circle cx="0" cy="0" r="60" fill="none" stroke="var(--r6)" stroke-width="1" opacity=".6"/>
    <circle cx="0" cy="0" r="36" fill="none" stroke="var(--r8)" stroke-width="1" opacity=".7"/>
    <circle cx="0" cy="0" r="14" fill="none" stroke="var(--r10)" stroke-width="1.8"/>
    <line x1="-100" y1="0" x2="100" y2="0" stroke="#263556" stroke-dasharray="2 3"/>
    <line x1="0" y1="-100" x2="0" y2="100" stroke="#263556" stroke-dasharray="2 3"/>
    <text x="-85" y="-70" font-size="12" fill="var(--r10)" font-weight="800">10</text>
    <text x=" 72" y="-70" font-size="12" fill="var(--r8)"  font-weight="800">8</text>
    <text x="-85" y=" 80" font-size="12" fill="var(--r6)"  font-weight="800">6</text>
    <text x=" 78" y=" 80" font-size="12" fill="var(--r4)"  font-weight="800">4</text>
    <g id="g${t}"></g>
  </svg>`;
}
grid.innerHTML = Array.from({length:N}).map((_,i)=>{
  const id=i+1;
  return `<div class="tc off" id="tc${id}">
    <div class="head"><div class="ttl">🎯 Sasaran ${id}</div><div class="pill" id="pill${id}">0</div></div>
    <div class="bull">${bull(id)}</div>
    <div class="lsens">
      <div><div class="k">Sensor Terakhir</div></div>
      <div class="v" id="ls${id}">—</div>
      <div class="ago" id="lsa${id}">—</div>
    </div>
    <div class="zones">
      <div class="z z10" id="zc1-${id}"><div class="s">S1</div><div class="k">Zon 10</div><div class="n" id="z10-${id}">0</div></div>
      <div class="z z8"  id="zc2-${id}"><div class="s">S2</div><div class="k">Zon 8</div> <div class="n" id="z8-${id}">0</div></div>
      <div class="z z6"  id="zc3-${id}"><div class="s">S3</div><div class="k">Zon 6</div> <div class="n" id="z6-${id}">0</div></div>
      <div class="z z4"  id="zc4-${id}"><div class="s">S4</div><div class="k">Zon 4</div> <div class="n" id="z4-${id}">0</div></div>
    </div>
  </div>`;
}).join('');

// Feed container appended once after the grid.
const feed = document.createElement('section');
feed.className = 'feed';
feed.innerHTML = '<h2>🔴 Tembakan Terkini</h2><ul id="feedList"><li class="empty">Belum ada tembakan — tembak sasaran untuk mula</li></ul>';
grid.parentNode.insertBefore(feed, grid.nextSibling);
const feedList = document.getElementById('feedList');

const prev = Array(N+1).fill(0);
const zonePos = {1:[-55,-55], 2:[55,-55], 3:[-55,55], 4:[55,55]};
const zoneColors = {1:'var(--r10)',2:'var(--r8)',3:'var(--r6)',4:'var(--r4)'};
const zoneLabels = {1:'Zon 10',2:'Zon 8',3:'Zon 6',4:'Zon 4'};

function fmtAgo(ms){
  if(ms == null) return '—';
  if(ms < 1000) return 'baru';
  const s = Math.floor(ms/1000);
  if(s < 60) return s+'s';
  const m = Math.floor(s/60);
  if(m < 60) return m+'m';
  return Math.floor(m/60)+'j';
}
function flashZone(tid, sensor){
  const el = document.getElementById('zc'+sensor+'-'+tid);
  if(!el) return;
  el.classList.add('flash');
  setTimeout(()=>el.classList.remove('flash'), 450);
}
let feedKey = '';
function renderFeed(recent){
  if(!Array.isArray(recent) || recent.length === 0){
    if(feedList.firstElementChild && feedList.firstElementChild.classList.contains('empty')) return;
    feedList.innerHTML = '<li class="empty">Belum ada tembakan — tembak sasaran untuk mula</li>';
    feedKey = '';
    return;
  }
  const key = recent.map(r => r.t+':'+r.s+':'+r.ago).join('|');
  if(key === feedKey){
    // Just refresh "ago" text without rebuilding
    [...feedList.children].forEach((li, i)=>{
      const r = recent[i]; if(!r) return;
      const a = li.querySelector('.ago'); if(a) a.textContent = fmtAgo(r.ago);
    });
    return;
  }
  feedKey = key;
  feedList.innerHTML = recent.map(r => `
    <li>
      <span class="t">T${r.t}</span>
      <span class="s s${r.s}">S${r.s}</span>
      <span class="z">${zoneLabels[r.z]||('Zon '+r.z)}</span>
      <span class="sc">+${r.sc}</span>
      <span class="ago">${fmtAgo(r.ago)}</span>
    </li>`).join('');
}

function placeDot(id, zone){
  const g = document.getElementById('g'+id); if(!g) return;
  const [bx,by] = zonePos[zone] || [0,0];
  const dx = bx + (Math.random()*30-15);
  const dy = by + (Math.random()*30-15);
  const ns = 'http://www.w3.org/2000/svg';
  const c = document.createElementNS(ns,'circle');
  c.setAttribute('cx',dx.toFixed(1));
  c.setAttribute('cy',dy.toFixed(1));
  c.setAttribute('r','5');
  c.setAttribute('fill', zoneColors[zone]||'#fff');
  c.setAttribute('class','dot');
  c.style.color = zoneColors[zone]||'#fff';
  g.appendChild(c);
  while(g.children.length > 25) g.removeChild(g.firstChild);
}

function flash(id){
  const c = document.getElementById('tc'+id); if(!c) return;
  c.classList.add('hit'); setTimeout(()=>c.classList.remove('hit'), 380);
}

async function tick(){
  try{
    const r = await fetch('/status', {cache:'no-store'});
    const d = await r.json();
    document.getElementById('dot').classList.add('on');
    document.getElementById('conn').textContent = 'Bersambung';
    let hits=0, score=0, active=0;
    for(let i=1;i<=N;i++){
      const t = d.targets[i-1]||{};
      const tc = document.getElementById('tc'+i);
      if(t.online){ tc.classList.remove('off'); active++; } else tc.classList.add('off');
      document.getElementById('pill'+i).textContent = t.score||0;
      document.getElementById('z10-'+i).textContent = (t.zones||[0])[0]||0;
      document.getElementById('z8-'+i).textContent  = (t.zones||[0,0])[1]||0;
      document.getElementById('z6-'+i).textContent  = (t.zones||[0,0,0])[2]||0;
      document.getElementById('z4-'+i).textContent  = (t.zones||[0,0,0,0])[3]||0;
      const ls = document.getElementById('ls'+i);
      const lsa = document.getElementById('lsa'+i);
      if(ls){
        ls.classList.remove('s1','s2','s3','s4');
        if(t.lastSensor && t.lastSensor>=1 && t.lastSensor<=4){
          ls.textContent = 'S'+t.lastSensor;
          ls.classList.add('s'+t.lastSensor);
          if(lsa) lsa.textContent = fmtAgo(t.ago);
        } else {
          ls.textContent = '—';
          if(lsa) lsa.textContent = '—';
        }
      }
      hits  += t.hits||0;
      score += t.score||0;
      if((t.hits||0) > prev[i]){
        const s = t.lastSensor || t.lastZone || 1;
        for(let k=prev[i];k<(t.hits||0);k++) placeDot(i, s);
        flash(i);
        flashZone(i, s);
      }
      prev[i] = t.hits||0;
    }
    document.getElementById('sh').textContent = hits;
    document.getElementById('ss').textContent = score;
    document.getElementById('sa').textContent = active+'/'+N;
    renderFeed(d.recent || []);
  }catch(e){
    document.getElementById('dot').classList.remove('on');
    document.getElementById('conn').textContent = 'Terputus';
  }
}
async function resetAll(){
  if(!confirm('Reset semua kaunter?')) return;
  await fetch('/reset', {method:'POST'});
  for(let i=1;i<=N;i++){
    const g = document.getElementById('g'+i); if(g) g.innerHTML='';
    prev[i] = 0;
  }
  feedKey = '';
  renderFeed([]);
}
setInterval(tick, 300); tick();
</script>
</body>
</html>
)RAW";

// =====================================================================
//  ESP-NOW receive callback
//  The signature changed between ESP32 Arduino core 2.x and 3.x:
//    2.x: void cb(const uint8_t* mac, const uint8_t* data, int len)
//    3.x: void cb(const esp_now_recv_info_t* info, const uint8_t* data, int len)
//  This shim lets the same sketch compile on both.
// =====================================================================
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void onEspNow(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  (void)info;
#else
static void onEspNow(const uint8_t* mac, const uint8_t* data, int len) {
  (void)mac;
#endif
  if (len != (int)sizeof(HitPacket)) return;
  HitPacket p; memcpy(&p, data, sizeof(p));
  if (p.targetID < 1 || p.targetID > NUM_TARGETS) return;
  TargetState& t = T[p.targetID];
  t.online   = true;
  t.lastSeen = millis();
  if (p.type == 1) {
    t.hitCount++;
    t.score += p.score;
    if (p.zone >= 1 && p.zone <= 4) t.zoneHits[p.zone - 1]++;
    t.lastSensor = p.sensorID;
    t.lastAmp    = p.amp;
    t.lastHitMs  = millis();
    RecentHit& r = recentBuf[recentHead];
    r.ts       = t.lastHitMs;
    r.targetID = p.targetID;
    r.sensorID = p.sensorID;
    r.zone     = p.zone;
    r.score    = p.score;
    recentHead = (recentHead + 1) % RECENT_N;
    if (recentCount < 0xFFFF) recentCount++;
    gHitPending = true;         // trigger buzzer in loop()
  }
}

// =====================================================================
//  HTTP handlers
// =====================================================================
static void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}
static void handleStatus() {
  String j = "{\"targets\":[";
  for (int i = 1; i <= NUM_TARGETS; i++) {
    TargetState& t = T[i];
    if (i > 1) j += ",";
    j += "{\"id\":";        j += i;
    j += ",\"online\":";    j += (t.online ? "true" : "false");
    j += ",\"hits\":";      j += t.hitCount;
    j += ",\"score\":";     j += t.score;
    j += ",\"zones\":[";
      j += t.zoneHits[0]; j += ",";
      j += t.zoneHits[1]; j += ",";
      j += t.zoneHits[2]; j += ",";
      j += t.zoneHits[3];
    j += "],\"lastSensor\":"; j += t.lastSensor;
    j += ",\"lastZone\":";    j += t.lastSensor;    // demo: zone == sensor
    j += ",\"lastAmp\":";     j += t.lastAmp;
    j += ",\"ago\":";         j += (uint32_t)(t.lastHitMs ? (millis() - t.lastHitMs) : 0);
    j += "}";
  }
  j += "]";
  // Append recent-hits feed (newest first)
  j += ",\"recent\":[";
  uint16_t shown = recentCount < RECENT_N ? recentCount : RECENT_N;
  uint32_t now = millis();
  for (uint16_t k = 0; k < shown; k++) {
    uint8_t idx = (uint8_t)((recentHead + RECENT_N - 1 - k) % RECENT_N);
    const RecentHit& r = recentBuf[idx];
    if (k) j += ",";
    j += "{\"t\":";  j += r.targetID;
    j += ",\"s\":";  j += r.sensorID;
    j += ",\"z\":";  j += r.zone;
    j += ",\"sc\":"; j += r.score;
    j += ",\"ago\":"; j += (uint32_t)(now - r.ts);
    j += "}";
  }
  j += "]";
  j += ",\"totalHits\":"; j += recentCount;
  j += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}
static void handleReset() {
  for (int i = 1; i <= NUM_TARGETS; i++) {
    T[i].hitCount = 0; T[i].score = 0;
    for (int z = 0; z < 4; z++) T[i].zoneHits[z] = 0;
    T[i].lastHitMs = 0;
    T[i].lastSensor = 0;
    T[i].lastAmp = 0;
  }
  for (uint8_t k = 0; k < RECENT_N; k++) recentBuf[k] = RecentHit{};
  recentHead = 0;
  recentCount = 0;
  server.send(200, "application/json", "{\"ok\":true}");
}

// =====================================================================
//  Setup / loop
// =====================================================================
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  KIK_BUZZ_SETUP(BUZZER_PIN, BUZZ_CH, 2500, 10);
  KIK_BUZZ_TONE(BUZZER_PIN, BUZZ_CH, 0);

  // AP on a fixed channel so ESP-NOW peers on STA side can match us.
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD, /*channel=*/1);
  IPAddress ip = WiFi.softAPIP();

  // Do NOT include WIFI_PROTOCOL_LR here — LR beacons are invisible to
  // standard phones/tablets, so the Target_System AP would not appear
  // in WiFi scan lists.
  esp_wifi_set_protocol(WIFI_IF_AP,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed"); while (1) delay(500);
  }
  esp_now_register_recv_cb(onEspNow);

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/reset",   HTTP_POST, handleReset);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();

  Serial.println("\nKIK Central DEMO ready.");
  // This is the AP MAC — it is what each target must put into
  // RECEIVER_MAC[] in target_demo.ino for ESP-NOW to reach us.
  Serial.printf("MAC:      %s   <-- paste this into target_demo RECEIVER_MAC\n",
                WiFi.softAPmacAddress().c_str());
  Serial.printf("AP SSID:  %s\n", AP_SSID);
  Serial.printf("AP IP:    %s\n", ip.toString().c_str());
  Serial.println("Open http://192.168.4.1/ on your phone.");
}

void loop() {
  server.handleClient();

  // Mark targets offline if we haven't heard from them in 15 s
  uint32_t now = millis();
  for (int i = 1; i <= NUM_TARGETS; i++) {
    if (T[i].online && (now - T[i].lastSeen) > 15000) T[i].online = false;
  }

  // Fire a quick beep on each new hit
  static uint32_t buzzOff = 0;
  if (gHitPending) {
    gHitPending = false;
    KIK_BUZZ_TONE(BUZZER_PIN, BUZZ_CH, 2500);
    buzzOff = now + 40;
  }
  if (buzzOff && now >= buzzOff) {
    KIK_BUZZ_TONE(BUZZER_PIN, BUZZ_CH, 0);
    buzzOff = 0;
  }
}
