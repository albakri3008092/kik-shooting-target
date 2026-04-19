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
static const int     BUZZ_CH    = 0;           // LEDC channel

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
  uint16_t zoneHits[4] = {0, 0, 0, 0};    // Z10, Z8, Z6, Z4
  uint16_t lastAmp    = 0;
  uint8_t  lastSensor = 0;
};
static TargetState T[NUM_TARGETS + 1];    // index 1..NUM_TARGETS

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
  .bull{aspect-ratio:1;background:radial-gradient(circle at 50% 50%,var(--surf2),var(--surf));border-radius:12px;margin-bottom:10px;overflow:hidden}
  .bull svg{width:100%;height:100%;display:block}
  .bull .dot{filter:drop-shadow(0 0 6px currentColor);animation:pop .3s ease}
  .zones{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}
  .z{background:var(--surf2);border-radius:8px;padding:6px 4px;text-align:center}
  .z .k{font-size:10px;color:var(--mut)}
  .z .n{font-size:18px;font-weight:900}
  .z.z10{border-top:3px solid var(--r10)} .z.z10 .n{color:var(--r10)}
  .z.z8 {border-top:3px solid var(--r8) } .z.z8 .n{color:var(--r8)}
  .z.z6 {border-top:3px solid var(--r6) } .z.z6 .n{color:var(--r6)}
  .z.z4 {border-top:3px solid var(--r4) } .z.z4 .n{color:var(--r4)}
  footer{margin-top:18px;text-align:center;color:var(--mut);font-size:12px}
  @keyframes pop{0%{transform:scale(.3);opacity:0}60%{transform:scale(1.3);opacity:1}100%{transform:scale(1)}}
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
    <div class="zones">
      <div class="z z10"><div class="k">Zon 10</div><div class="n" id="z10-${id}">0</div></div>
      <div class="z z8"><div class="k">Zon 8</div> <div class="n" id="z8-${id}">0</div></div>
      <div class="z z6"><div class="k">Zon 6</div> <div class="n" id="z6-${id}">0</div></div>
      <div class="z z4"><div class="k">Zon 4</div> <div class="n" id="z4-${id}">0</div></div>
    </div>
  </div>`;
}).join('');

const prev = Array(N+1).fill(0);
const zonePos = {1:[-55,-55], 2:[55,-55], 3:[-55,55], 4:[55,55]};
const zoneColors = {1:'var(--r10)',2:'var(--r8)',3:'var(--r6)',4:'var(--r4)'};

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
      hits  += t.hits||0;
      score += t.score||0;
      if((t.hits||0) > prev[i]){
        for(let k=prev[i];k<(t.hits||0);k++) placeDot(i, t.lastZone||1);
        flash(i);
      }
      prev[i] = t.hits||0;
    }
    document.getElementById('sh').textContent = hits;
    document.getElementById('ss').textContent = score;
    document.getElementById('sa').textContent = active+'/'+N;
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
}
setInterval(tick, 300); tick();
</script>
</body>
</html>
)RAW";

// =====================================================================
//  ESP-NOW receive callback
// =====================================================================
static void onEspNow(const uint8_t* mac, const uint8_t* data, int len) {
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
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}
static void handleReset() {
  for (int i = 1; i <= NUM_TARGETS; i++) {
    T[i].hitCount = 0; T[i].score = 0;
    for (int z = 0; z < 4; z++) T[i].zoneHits[z] = 0;
    T[i].lastHitMs = 0;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// =====================================================================
//  Setup / loop
// =====================================================================
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  ledcSetup(BUZZ_CH, 2500, 10);
  ledcAttachPin(BUZZER_PIN, BUZZ_CH);
  ledcWriteTone(BUZZ_CH, 0);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();

  esp_wifi_set_protocol(WIFI_IF_AP,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
      WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);

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
  Serial.printf("MAC:      %s\n", WiFi.macAddress().c_str());
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
    ledcWriteTone(BUZZ_CH, 2500);
    buzzOff = now + 40;
  }
  if (buzzOff && now >= buzzOff) {
    ledcWriteTone(BUZZ_CH, 0);
    buzzOff = 0;
  }
}
