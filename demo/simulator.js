// Local preview helper for demo/central_demo.ino.
// Serves the embedded INDEX_HTML from central_demo.ino and provides
// mock /status + /reset endpoints so you can see the demo dashboard
// in a browser without flashing any hardware.
//
//   node demo/simulator.js
//   open http://localhost:8081
const http = require('http');
const fs   = require('fs');
const path = require('path');

const SKETCH = path.join(__dirname, 'central_demo', 'central_demo.ino');
const src = fs.readFileSync(SKETCH, 'utf8');
const m = src.match(/INDEX_HTML\[\]\s*PROGMEM\s*=\s*R"RAW\(([\s\S]*?)\)RAW";/);
if (!m) { console.error('INDEX_HTML not found in sketch'); process.exit(1); }
const HTML = m[1];

const NUM_TARGETS = 3;
const RECENT_N = 16;

const state = Array.from({length: NUM_TARGETS}, () => ({
  online: true, hits: 0, score: 0, zones: [0, 0, 0, 0], lastSensor: 0, lastHitMs: 0,
}));
const recent = [];           // newest first
const SCORES = [10, 8, 6, 4];

function pushRecent(targetID, sensor) {
  recent.unshift({
    t: targetID,
    s: sensor,
    z: sensor,                  // demo: zone == sensor
    sc: SCORES[sensor - 1],
    ts: Date.now(),
  });
  if (recent.length > RECENT_N) recent.length = RECENT_N;
}

setInterval(() => {
  const ti = Math.floor(Math.random() * NUM_TARGETS);
  const z  = Math.floor(Math.random() * 4);   // 0..3 -> Z10/Z8/Z6/Z4
  const sensor = z + 1;
  state[ti].hits++;
  state[ti].zones[z]++;
  state[ti].score += SCORES[z];
  state[ti].lastSensor = sensor;
  state[ti].lastHitMs = Date.now();
  pushRecent(ti + 1, sensor);
}, 900);

http.createServer((req, res) => {
  if (req.url === '/' || req.url === '/index.html') {
    res.writeHead(200, {'content-type': 'text/html; charset=utf-8'});
    res.end(HTML); return;
  }
  if (req.url === '/status') {
    const now = Date.now();
    const body = {
      targets: state.map((s, i) => ({
        id: i + 1, online: s.online, hits: s.hits, score: s.score,
        zones: s.zones, lastSensor: s.lastSensor, lastZone: s.lastSensor,
        lastAmp: 2000, ago: s.lastHitMs ? now - s.lastHitMs : 0,
      })),
      recent: recent.map(r => ({
        t: r.t, s: r.s, z: r.z, sc: r.sc, ago: now - r.ts,
      })),
      totalHits: recent.length > 0 ? state.reduce((a, s) => a + s.hits, 0) : 0,
    };
    res.writeHead(200, {'content-type':'application/json','cache-control':'no-store'});
    res.end(JSON.stringify(body)); return;
  }
  if (req.url === '/reset' && req.method === 'POST') {
    for (const s of state) { s.hits=0; s.score=0; s.zones=[0,0,0,0]; s.lastHitMs=0; s.lastSensor=0; }
    recent.length = 0;
    res.writeHead(200, {'content-type':'application/json'});
    res.end('{"ok":true}'); return;
  }
  res.writeHead(404); res.end();
}).listen(8081, () => console.log('Demo preview: http://localhost:8081'));
