// =====================================================================
//  KIK Shooting Target — Node.js Simulator
//  Mimics the ESP32 Central Receiver so you can preview the full-colour
//  dashboard in any browser without hardware.
// =====================================================================
//  Usage:
//    cd simulator && npm install && npm start
//    open http://localhost:8080
// =====================================================================
const http  = require('http');
const fs    = require('fs');
const path  = require('path');
const url   = require('url');
const { WebSocketServer } = require('ws');

const PORT       = process.env.PORT ? +process.env.PORT : 8080;
const DASH_DIR   = path.join(__dirname, '..', 'dashboard', 'src');
const SESSIONS_DIR = path.join(__dirname, 'data', 'sessions');
const SHOOTERS_FILE= path.join(__dirname, 'data', 'shooters.json');
fs.mkdirSync(SESSIONS_DIR, { recursive: true });

const NUM_TARGETS = 15;

// ---------- State ----------
const targets = Array.from({length: NUM_TARGETS+1}, () => ({
  online: true,
  hits:   [0,0,0,0,0],
  zones:  [0,0,0,0,0],
  score:  0,
  status: [0,0,0,0,0],
  readings:[0,500,600,550,580],
  batt:   7800 - Math.floor(Math.random()*800),
  rssi:   -(50 + Math.floor(Math.random()*30)),
  fw:     '2.0',
}));
let autoHits = true;   // can be toggled via /api/sim/toggle
const session = {
  active:false, startedAtMs:0, parMs:0, startEpoch:0,
  name:'', shooter:'', shooterID:0, randomStart:false,
  randomFireMs:0, buzzerFired:false, shots:[],
};

// ---------- Helpers ----------
const now = () => Date.now();
const clients = new Set();
const broadcast = (obj) => { const s = JSON.stringify(obj); for(const c of clients) if(c.readyState===1) c.send(s); };

function statusJson(){
  return {
    fw: '2.0', uptime: Math.floor(process.uptime()),
    session: {
      active: session.active, name: session.name, shooter: session.shooter,
      par: session.parMs,
      elapsed: session.active ? (now() - session.startedAtMs) : 0,
      shots: session.shots.length, epoch: session.startEpoch,
    },
    targets: Array.from({length:NUM_TARGETS}, (_,i)=>{
      const t = targets[i+1];
      return { id:i+1, online:t.online, score:t.score,
        hits:t.hits.slice(1), zones:t.zones.slice(1), status:t.status.slice(1),
        batt:t.batt, rssi:t.rssi, fw:t.fw,
      };
    }),
  };
}

// ---------- Simulated hit engine ----------
function simulateHit(forceTarget = 0){
  const t = forceTarget || (1 + Math.floor(Math.random()*NUM_TARGETS));
  const tgt = targets[t]; if(!tgt.online) return;
  // random centroid biased toward center
  const r    = Math.pow(Math.random(), 1.6);            // 0..1
  const ang  = Math.random() * Math.PI * 2;
  const cx = Math.round(Math.cos(ang) * r * 900);       // -900..900
  const cy = Math.round(Math.sin(ang) * r * 900);
  // zone 1=TL 2=TR 3=BL 4=BR
  const zone = (cx <= 0 && cy >= 0) ? 1 : (cx > 0 && cy >= 0) ? 2 : (cx <= 0) ? 3 : 4;
  const zoneScores = [0, 10, 8, 6, 4];
  const sensor = zone;
  tgt.hits[sensor]++; tgt.zones[zone]++;
  tgt.score += zoneScores[zone];
  const payload = {
    type: 'hit', target: t, sensor, zone,
    count: tgt.hits[sensor], score: zoneScores[zone], total: tgt.score,
    cx, cy, ts: now(),
    sess: session.active ? (now() - session.startedAtMs) : 0,
    shooter: session.shooter || '',
  };
  if(session.active){
    session.shots.push({
      t: now() - session.startedAtMs, target:t, sensor, zone,
      score: zoneScores[zone], cx, cy, shooterID: session.shooterID,
    });
  }
  broadcast(payload);
}

function simulateHeartbeat(){
  for(let i=1;i<=NUM_TARGETS;i++){
    const t = targets[i];
    // tiny drift on battery and rssi
    t.batt = Math.max(6000, t.batt - (Math.random() < .05 ? 5 : 0));
    t.rssi = -(45 + Math.floor(Math.random()*40));
    const status = [0,0,0,0];
    // rare warn/fail
    if(Math.random() < 0.02) status[Math.floor(Math.random()*4)] = 1;
    if(Math.random() < 0.005) status[Math.floor(Math.random()*4)] = 2;
    t.status = [0, ...status];
    broadcast({
      type:'health', target:i, online:t.online, status, readings:t.readings.slice(1),
      batt:t.batt, rssi:t.rssi, uptime:Math.floor(process.uptime()), fw:t.fw,
    });
  }
}

// ---------- Session control ----------
function startSession({name, shooter, shooterID, par, random}){
  session.active = true;
  session.parMs = par|0;
  session.shooter = shooter || '';
  session.shooterID = shooterID|0;
  session.name = name || 'Session';
  session.randomStart = !!random;
  session.buzzerFired = !random;
  session.startedAtMs = now();
  session.randomFireMs = random ? (now() + 1500 + Math.random()*2500) : now();
  session.startEpoch = Math.floor(now()/1000);
  session.shots = [];
  for(let i=1;i<=NUM_TARGETS;i++){ targets[i].hits=[0,0,0,0,0]; targets[i].zones=[0,0,0,0,0]; targets[i].score=0; }
  broadcast({type:'session_start', name:session.name, shooter:session.shooter, par:session.parMs,
             random:session.randomStart, fireInMs: session.randomStart ? (session.randomFireMs-now()) : 0});
  broadcast({type:'reset'});
}
function stopSession(){
  if(!session.active) return;
  session.active = false;
  const file = path.join(SESSIONS_DIR, session.startEpoch + '.json');
  const payload = {
    name: session.name, shooter: session.shooter, epoch: session.startEpoch,
    par: session.parMs,
    shots: session.shots.map(s => [s.t, s.target, s.sensor, s.zone, s.score, s.cx, s.cy, s.shooterID]),
  };
  try{ fs.writeFileSync(file, JSON.stringify(payload)); }catch(e){}
  broadcast({type:'session_stop', shots: session.shots.length, epoch: session.startEpoch});
}
function resetAll(){
  for(let i=1;i<=NUM_TARGETS;i++){ targets[i].hits=[0,0,0,0,0]; targets[i].zones=[0,0,0,0,0]; targets[i].score=0; }
  session.shots = [];
  broadcast({type:'reset'});
}

// ---------- Ticks ----------
setInterval(()=>{
  if(autoHits && Math.random() < 0.55) simulateHit();
}, 900);
setInterval(simulateHeartbeat, 5000);
setInterval(()=>{
  // timer tick
  if(session.active){
    if(session.randomStart && !session.buzzerFired && now() >= session.randomFireMs){
      session.buzzerFired = true;
      session.startedAtMs = now();
      broadcast({type:'timer_go'});
    }
    if(session.buzzerFired){
      broadcast({type:'tick', elapsed: now()-session.startedAtMs, shots: session.shots.length});
      if(session.parMs && now()-session.startedAtMs >= session.parMs && !session._parBeeped){
        session._parBeeped = true; broadcast({type:'par'});
      }
    }
  }
}, 100);

// ---------- HTTP server ----------
const MIME = {
  '.html':'text/html; charset=utf-8', '.js':'text/javascript; charset=utf-8',
  '.css':'text/css; charset=utf-8',   '.json':'application/json',
  '.svg':'image/svg+xml',             '.png':'image/png',
  '.ico':'image/x-icon',              '.webmanifest':'application/manifest+json',
};
function sendFile(res, fp){
  fs.readFile(fp, (err, data) => {
    if(err){ res.writeHead(404); res.end('Not found'); return; }
    res.writeHead(200, {'content-type': MIME[path.extname(fp)]||'application/octet-stream'});
    res.end(data);
  });
}
function readBody(req){
  return new Promise((resolve,reject) => {
    let body = ''; req.on('data', c => body += c);
    req.on('end', ()=>resolve(body)); req.on('error', reject);
  });
}
async function loadShooters(){
  try{ return JSON.parse(fs.readFileSync(SHOOTERS_FILE, 'utf8')); }
  catch(e){ return []; }
}
async function saveShooters(list){
  fs.mkdirSync(path.dirname(SHOOTERS_FILE), { recursive: true });
  fs.writeFileSync(SHOOTERS_FILE, JSON.stringify(list));
}

const server = http.createServer(async (req, res) => {
  const u = url.parse(req.url, true);
  const pn = u.pathname;

  // --- API ---
  if(pn === '/api/status')       { res.writeHead(200,{'content-type':'application/json'}); res.end(JSON.stringify(statusJson())); return; }
  if(pn === '/api/shooters' && req.method === 'GET') { res.writeHead(200,{'content-type':'application/json'}); res.end(JSON.stringify(await loadShooters())); return; }
  if(pn === '/api/shooters' && req.method === 'POST'){ try{ await saveShooters(JSON.parse(await readBody(req))); res.writeHead(200).end('{"ok":true}'); }catch(e){ res.writeHead(400).end(e.message); } return; }
  if(pn === '/api/session/start'){ try{ startSession(JSON.parse(await readBody(req))); res.writeHead(200).end('{"ok":true}'); }catch(e){ res.writeHead(400).end(e.message); } return; }
  if(pn === '/api/session/stop') { stopSession(); res.writeHead(200).end('{"ok":true}'); return; }
  if(pn === '/api/session/reset'){ resetAll(); res.writeHead(200).end('{"ok":true}'); return; }
  if(pn === '/api/sessions'){
    const files = fs.readdirSync(SESSIONS_DIR).map(f => ({ file:'/api/sessions/'+f, size: fs.statSync(path.join(SESSIONS_DIR,f)).size }));
    res.writeHead(200,{'content-type':'application/json'}); res.end(JSON.stringify(files)); return;
  }
  if(pn.startsWith('/api/sessions/')){ sendFile(res, path.join(SESSIONS_DIR, pn.replace('/api/sessions/',''))); return; }
  if(pn === '/api/config/push'){ await readBody(req); res.writeHead(200).end('{"ok":true}'); return; }
  if(pn === '/api/sim/toggle'){ autoHits = !autoHits; res.writeHead(200,{'content-type':'application/json'}); res.end(JSON.stringify({autoHits})); return; }
  if(pn === '/api/sim/hit'){ simulateHit(+u.query.target||0); res.writeHead(200).end('{"ok":true}'); return; }

  // --- Static dashboard ---
  let rel = pn === '/' ? '/index.html' : pn;
  const fp = path.join(DASH_DIR, rel);
  if(!fp.startsWith(DASH_DIR)){ res.writeHead(400).end(); return; }
  if(fs.existsSync(fp) && fs.statSync(fp).isFile()){ sendFile(res, fp); return; }
  res.writeHead(404).end('Not found: ' + pn);
});

// --- WebSocket ---
const wss = new WebSocketServer({ noServer: true });
server.on('upgrade', (req, socket, head) => {
  const u = url.parse(req.url);
  if(u.pathname !== '/ws' && u.pathname !== '/instructor'){ socket.destroy(); return; }
  wss.handleUpgrade(req, socket, head, (ws) => {
    clients.add(ws);
    ws.send(JSON.stringify(statusJson()));
    ws.on('close', ()=> clients.delete(ws));
    ws.on('message', (buf) => {
      try{
        const m = JSON.parse(buf.toString());
        if(m.command === 'reset') resetAll();
        else if(m.command === 'start_session') startSession(m);
        else if(m.command === 'stop_session') stopSession();
        else if(m.command === 'identify') broadcast({type:'identify', target:m.target});
      }catch(e){}
    });
  });
});

server.listen(PORT, () => {
  console.log(`KIK simulator: http://localhost:${PORT}`);
  console.log(`  /            -> dashboard`);
  console.log(`  /tv.html     -> TV mode`);
  console.log(`  /instructor.html -> instructor view`);
  console.log(`  POST /api/sim/toggle to pause auto-hits`);
});
