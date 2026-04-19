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

const state = Array.from({length: 3}, () => ({
  online: true, hits: 0, score: 0, zones: [0, 0, 0, 0], lastSensor: 0, lastHitMs: 0,
}));
const SCORES = [10, 8, 6, 4];

setInterval(() => {
  const t = Math.floor(Math.random()*3);
  const z = Math.floor(Math.random()*4);           // 0..3 -> Z10/Z8/Z6/Z4
  state[t].hits++; state[t].zones[z]++; state[t].score += SCORES[z];
  state[t].lastSensor = z+1; state[t].lastHitMs = Date.now();
}, 900);

http.createServer((req, res) => {
  if (req.url === '/' || req.url === '/index.html') {
    res.writeHead(200, {'content-type': 'text/html; charset=utf-8'});
    res.end(HTML); return;
  }
  if (req.url === '/status') {
    const body = {
      targets: state.map((s, i) => ({
        id: i+1, online: s.online, hits: s.hits, score: s.score,
        zones: s.zones, lastSensor: s.lastSensor, lastZone: s.lastSensor,
        lastAmp: 2000, ago: s.lastHitMs ? Date.now() - s.lastHitMs : 0,
      })),
    };
    res.writeHead(200, {'content-type':'application/json','cache-control':'no-store'});
    res.end(JSON.stringify(body)); return;
  }
  if (req.url === '/reset' && req.method === 'POST') {
    for (const s of state) { s.hits=0; s.score=0; s.zones=[0,0,0,0]; s.lastHitMs=0; }
    res.writeHead(200, {'content-type':'application/json'});
    res.end('{"ok":true}'); return;
  }
  res.writeHead(404); res.end();
}).listen(8081, () => console.log('Demo preview: http://localhost:8081'));
