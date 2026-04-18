// ===========================================================
//  KIK Dashboard — main app
// ===========================================================
(function(){
  const NUM_TARGETS = 15;
  const THEME_KEY = 'kik.theme';
  const ZONES = [0, 10, 8, 6, 4];        // zone index 0 unused
  const ZONE_COLORS = ['#162036','var(--ring-10)','var(--ring-8)','var(--ring-6)','var(--ring-4)'];

  // ---------- State ----------
  const state = {
    connected: false,
    targets: Array.from({length: NUM_TARGETS+1}, ()=>({
      online:false, hits:[0,0,0,0,0], zones:[0,0,0,0,0], score:0,
      status:[0,0,0,0,0], readings:[0,0,0,0,0], batt:0, rssi:0, fw:'-',
      dots:[]  // recent centroid dots {x,y,t}
    })),
    session: { active:false, elapsed:0, par:0, name:'', shooter:'', random:false, waitingGo:false, over:false },
    shots: [],
    config: { threshold:1500, debounce:150, heartbeat:5000, z1:10, z2:8, z3:6, z4:4 },
  };

  const $  = s => document.querySelector(s);
  const $$ = s => document.querySelectorAll(s);

  function toast(msg, ms=2000){
    let el = $('#toast');
    if(!el){ el = document.createElement('div'); el.id='toast'; el.className='toast'; document.body.appendChild(el); }
    el.textContent = msg; el.classList.add('show');
    clearTimeout(toast._t); toast._t = setTimeout(()=>el.classList.remove('show'), ms);
  }

  function fmtTime(ms){
    if(ms == null) return '00:00.0';
    const s = Math.floor(ms/1000), cs = Math.floor((ms%1000)/100);
    const m = Math.floor(s/60);
    return String(m).padStart(2,'0')+':'+String(s%60).padStart(2,'0')+'.'+cs;
  }
  function fmtBatt(mv){
    if(!mv) return '—';
    // 2S Li-ion: 6.0V empty -> 8.4V full. Map linearly to 0-100.
    const pct = Math.max(0, Math.min(100, Math.round((mv-6000)/24)));
    return pct + '%';
  }

  // ---------- Build UI shell ----------
  function buildShell(){
    const tabs = [
      ['live',     '🎯'], ['score',    '🏆'],
      ['health',   '💚'], ['session',  '⏱️'],
      ['history',  '📊'], ['settings', '⚙️'],
    ];
    const tabBar = $('#tabs');
    tabBar.innerHTML = tabs.map(([k,icon]) => `
      <button class="tab ${k==='live'?'active':''}" data-tab="${k}">
        <span>${icon}</span><span data-i="tab_${k}">${t('tab_'+k)}</span>
      </button>`).join('') + `
      <a class="tab" href="/tv.html" target="_blank"><span>📺</span><span data-i="tab_tv">${t('tab_tv')}</span></a>
    `;
    tabBar.querySelectorAll('.tab[data-tab]').forEach(b => b.onclick = () => showTab(b.dataset.tab));

    buildLive();
    buildScore();
    buildHealth();
    buildSession();
    buildHistory();
    buildSettings();
  }

  function showTab(tab){
    $$('.tab[data-tab]').forEach(b => b.classList.toggle('active', b.dataset.tab === tab));
    $$('.view').forEach(v => v.classList.toggle('active', v.id === 'view-'+tab));
  }

  // ---------- Live view (target grid + hit map) ----------
  function bullseyeSVG(targetId){
    // 4 concentric quadrant rings: 10 outer, ring colors by zone score.
    return `
      <svg viewBox="-100 -100 200 200" aria-label="Target ${targetId}">
        <defs>
          <radialGradient id="tg-bg-${targetId}" cx="50%" cy="50%" r="50%">
            <stop offset="0%"  stop-color="#1e2b4a"/>
            <stop offset="100%" stop-color="#0b1221"/>
          </radialGradient>
        </defs>
        <rect x="-100" y="-100" width="200" height="200" fill="url(#tg-bg-${targetId})"/>
        <!-- quadrant fills (Z1..Z4 ; top-left, top-right, bot-left, bot-right) -->
        <path d="M-100,-100 L 0,-100 L 0,0 L-100,0 Z" fill="${ZONE_COLORS[1]}" opacity=".10"/>
        <path d="M 0,-100 L 100,-100 L 100,0 L 0,0 Z"  fill="${ZONE_COLORS[2]}" opacity=".10"/>
        <path d="M-100,0 L 0,0 L 0,100 L-100,100 Z"   fill="${ZONE_COLORS[3]}" opacity=".10"/>
        <path d="M 0,0 L 100,0 L 100,100 L 0,100 Z"   fill="${ZONE_COLORS[4]}" opacity=".10"/>
        <!-- concentric rings -->
        <circle cx="0" cy="0" r="85" fill="none" stroke="var(--ring-4)" stroke-width="1" opacity=".55"/>
        <circle cx="0" cy="0" r="60" fill="none" stroke="var(--ring-6)" stroke-width="1" opacity=".55"/>
        <circle cx="0" cy="0" r="36" fill="none" stroke="var(--ring-8)" stroke-width="1" opacity=".7"/>
        <circle cx="0" cy="0" r="14" fill="none" stroke="var(--ring-10)" stroke-width="1.5"/>
        <!-- crosshairs -->
        <line x1="-100" y1="0" x2="100" y2="0" stroke="#263556" stroke-dasharray="2,3"/>
        <line x1="0" y1="-100" x2="0" y2="100" stroke="#263556" stroke-dasharray="2,3"/>
        <!-- quadrant score labels -->
        <text x="-85" y="-70" font-size="12" fill="var(--ring-10)" font-weight="800">10</text>
        <text x=" 72" y="-70" font-size="12" fill="var(--ring-8)"  font-weight="800">8</text>
        <text x="-85" y=" 80" font-size="12" fill="var(--ring-6)"  font-weight="800">6</text>
        <text x=" 78" y=" 80" font-size="12" fill="var(--ring-4)"  font-weight="800">4</text>
        <g class="dots" id="dots-${targetId}"></g>
      </svg>`;
  }

  function buildLive(){
    const view = $('#view-live');
    let html = `
      <div class="summary">
        <div class="sum-card primary"><div class="sum-label" data-i="sum_hits">${t('sum_hits')}</div><div class="sum-value" id="sum-hits">0</div></div>
        <div class="sum-card info"><div class="sum-label" data-i="sum_score">${t('sum_score')}</div><div class="sum-value" id="sum-score">0</div></div>
        <div class="sum-card accent"><div class="sum-label" data-i="sum_active">${t('sum_active')}</div><div class="sum-value" id="sum-active">0/${NUM_TARGETS}</div></div>
        <div class="sum-card warn"><div class="sum-label" data-i="sum_health">${t('sum_health')}</div><div class="sum-value" id="sum-health">60/60</div></div>
      </div>
      <div class="row" style="justify-content:flex-end;margin-bottom:10px">
        <button class="danger-btn" id="btn-reset">🔄 <span data-i="reset_all">${t('reset_all')}</span></button>
      </div>
      <div class="grid" id="grid"></div>`;
    view.innerHTML = html;
    const grid = $('#grid');
    grid.innerHTML = Array.from({length:NUM_TARGETS}).map((_,i)=>{
      const id = i+1;
      return `
      <div class="target-card offline" id="tc-${id}">
        <div class="tc-head">
          <div class="tc-title">T${id}</div>
          <div class="tc-meta">
            <span class="status-dot" id="dot-${id}"></span>
            <span class="batt" id="batt-${id}">—</span>
          </div>
          <div class="score-pill" id="score-${id}">0</div>
        </div>
        <div class="hitmap">${bullseyeSVG(id)}</div>
        <div class="tc-foot" id="sf-${id}">
          ${[1,2,3,4].map(s=>`<div class="sbox" id="sb-${id}-${s}"><div class="sl">S${s}</div><div class="sv">0</div></div>`).join('')}
        </div>
      </div>`;
    }).join('');
    $('#btn-reset').onclick = () => {
      if(confirm(t('confirm_reset'))) window.KIK_WS.send({command:'reset'});
    };
  }

  function renderLive(){
    let totalHits = 0, totalScore = 0, active = 0, healthy = 0, totalSensors = 0;
    for(let t=1;t<=NUM_TARGETS;t++){
      const s = state.targets[t];
      const sum = s.hits[1]+s.hits[2]+s.hits[3]+s.hits[4];
      totalHits += sum;
      totalScore += s.score;
      if(s.online) active++;
      for(let i=1;i<=4;i++){ totalSensors++; if(s.status[i]===0) healthy++; }
      const card = document.getElementById('tc-'+t);
      if(!card) continue;
      card.classList.toggle('offline', !s.online);
      document.getElementById('dot-'+t).className = 'status-dot ' + (s.online?'online':'offline');
      document.getElementById('score-'+t).textContent = s.score;
      document.getElementById('batt-'+t).textContent = s.batt ? fmtBatt(s.batt) : '—';
      for(let i=1;i<=4;i++){
        const sb = document.getElementById('sb-'+t+'-'+i);
        sb.className = 'sbox' + (s.status[i]===1?' warn':(s.status[i]===2?' fail':''));
        sb.querySelector('.sv').textContent = s.hits[i];
      }
    }
    $('#sum-hits').textContent   = totalHits;
    $('#sum-score').textContent  = totalScore;
    $('#sum-active').textContent = active + '/' + NUM_TARGETS;
    $('#sum-health').textContent = healthy + '/' + totalSensors;
  }

  function drawHitDot(target, cx, cy, score){
    const g = document.getElementById('dots-'+target);
    if(!g) return;
    const ns = 'http://www.w3.org/2000/svg';
    const dot = document.createElementNS(ns, 'circle');
    dot.setAttribute('cx', String(Math.max(-95, Math.min(95, cx/10))));
    dot.setAttribute('cy', String(-Math.max(-95, Math.min(95, cy/10))));
    dot.setAttribute('r', '5');
    dot.setAttribute('class', 'dot');
    dot.setAttribute('fill', score>=10 ? 'var(--ring-10)'
                        : score>=8 ? 'var(--ring-8)'
                        : score>=6 ? 'var(--ring-6)' : 'var(--ring-4)');
    dot.style.animation = 'pop .3s ease';
    g.appendChild(dot);
    // cap to 30 dots per target
    while(g.children.length > 30) g.removeChild(g.firstChild);
  }

  function flashTarget(id){
    const card = document.getElementById('tc-'+id);
    if(!card) return;
    card.classList.add('hit-flash');
    setTimeout(()=>card.classList.remove('hit-flash'), 450);
  }

  // ---------- Score view ----------
  function buildScore(){
    $('#view-score').innerHTML = `<div class="score-list" id="score-list"></div>`;
  }
  function renderScore(){
    const list = $('#score-list'); if(!list) return;
    const arr = [];
    for(let i=1;i<=NUM_TARGETS;i++){
      const s = state.targets[i];
      const hits = s.hits[1]+s.hits[2]+s.hits[3]+s.hits[4];
      const maxPossible = hits * 10;
      arr.push({id:i, score:s.score, hits, acc: maxPossible ? Math.round(100*s.score/maxPossible) : 0, online:s.online});
    }
    arr.sort((a,b)=> b.score - a.score || b.hits - a.hits);
    list.innerHTML = arr.map((r,i)=>{
      const medal = i===0?'🥇':i===1?'🥈':i===2?'🥉':(i+1);
      const cls = i===0?'gold':i===1?'silver':i===2?'bronze':'';
      return `
        <div class="score-row">
          <div class="rank ${cls}">${medal}</div>
          <div><div class="score-name">${t('target')} ${r.id}</div>
               <div class="score-sub">${r.hits} ${t('hits').toLowerCase()} · ${r.online ? t('online') : t('offline')}</div></div>
          <div class="score-score">${r.score}</div>
          <div class="score-pct">${r.acc}%</div>
        </div>`;
    }).join('');
  }

  // ---------- Health view ----------
  function buildHealth(){
    $('#view-health').innerHTML = `<div id="health-list"></div>`;
  }
  function renderHealth(){
    const list = $('#health-list'); if(!list) return;
    let html = '';
    for(let i=1;i<=NUM_TARGETS;i++){
      const s = state.targets[i];
      const pct = s.batt ? Math.max(0, Math.min(100, Math.round((s.batt-6000)/24))) : 0;
      html += `<div class="health-row">
        <div class="tname">T${i}</div>
        <div>
          <div style="display:flex;gap:8px;align-items:center;margin-bottom:4px;">
            <span class="status-dot ${s.online?'online':'offline'}"></span>
            <span>${s.online? t('online') : t('offline')}</span>
            <span class="rssi">${s.rssi?s.rssi+' dBm':''}</span>
            <span class="rssi">fw ${s.fw}</span>
          </div>
          <div class="health-sensors">${[1,2,3,4].map(x=>{
            const c = s.status[x]===0?'ok':s.status[x]===1?'warn':'fail';
            return `<div class="s ${c}">S${x}</div>`;
          }).join('')}</div>
        </div>
        <div style="text-align:right;min-width:110px">
          <div class="batt-bar"><span style="width:${pct}%"></span></div>
          <div class="rssi">${fmtBatt(s.batt)}</div>
        </div>
      </div>`;
    }
    list.innerHTML = html;
  }

  // ---------- Session view ----------
  function buildSession(){
    $('#view-session').innerHTML = `
      <div class="bigtimer" id="bigtimer">00:00.0</div>
      <div class="session-card">
        <h3 data-i="session_start">${t('session_start')}</h3>
        <div class="row" style="margin-bottom:8px">
          <label data-i="session_name">${t('session_name')}</label>
          <input id="sess-name" data-ip="placeholder_session" placeholder="${t('placeholder_session')}" style="flex:1;min-width:180px"/>
        </div>
        <div class="row" style="margin-bottom:8px">
          <label data-i="shooter">${t('shooter')}</label>
          <input id="sess-shooter" data-ip="placeholder_shooter" placeholder="${t('placeholder_shooter')}" style="flex:1;min-width:180px"/>
        </div>
        <div class="row" style="margin-bottom:8px">
          <label data-i="par_time">${t('par_time')}</label>
          <input id="sess-par" type="number" value="10" min="0" max="600" style="width:100px"/>
          <label style="min-width:auto"><input id="sess-random" type="checkbox" style="vertical-align:middle"/> <span data-i="random_start">${t('random_start')}</span></label>
        </div>
        <div class="row">
          <button class="primary-btn" id="btn-session-start"><span data-i="session_start">${t('session_start')}</span></button>
          <button class="danger-btn"  id="btn-session-stop" disabled><span data-i="session_stop">${t('session_stop')}</span></button>
          <button class="ghost-btn"   id="btn-export-csv"><span data-i="export_csv">${t('export_csv')}</span></button>
          <button class="ghost-btn"   id="btn-export-json"><span data-i="export_json">${t('export_json')}</span></button>
        </div>
      </div>

      <div class="charts">
        <div class="chart-card">
          <h4 data-i="chart_scoretime">${t('chart_scoretime')}</h4>
          <svg id="chart-score"></svg>
        </div>
        <div class="chart-card">
          <h4 data-i="chart_zone">${t('chart_zone')}</h4>
          <svg id="chart-zone"></svg>
        </div>
      </div>

      <div class="session-card">
        <h3 data-i="shot_log">${t('shot_log')}</h3>
        <div class="shotlog">
          <table>
            <thead><tr>
              <th>#</th><th data-i="time_col">${t('time_col')}</th>
              <th data-i="target_col">${t('target_col')}</th>
              <th data-i="zone_col">${t('zone_col')}</th>
              <th data-i="score_col">${t('score_col')}</th>
              <th data-i="cum_col">${t('cum_col')}</th>
            </tr></thead>
            <tbody id="shotlog-body"><tr><td colspan="6" style="color:var(--muted);text-align:center;padding:20px" data-i="no_shots">${t('no_shots')}</td></tr></tbody>
          </table>
        </div>
      </div>
    `;
    $('#btn-session-start').onclick = () => {
      const name    = $('#sess-name').value.trim() || 'Session';
      const shooter = $('#sess-shooter').value.trim();
      const par     = Math.round((parseFloat($('#sess-par').value)||0)*1000);
      const random  = $('#sess-random').checked;
      window.KIK_WS.send({command:'start_session', name, shooter, par, random});
      toast(t('session_start'));
    };
    $('#btn-session-stop').onclick = () => window.KIK_WS.send({command:'stop_session'});
    $('#btn-export-csv').onclick  = () => exportCsv();
    $('#btn-export-json').onclick = () => exportJson();
  }

  function renderBigTimer(){
    const b = $('#bigtimer'); if(!b) return;
    if(state.session.waitingGo){ b.textContent = t('ready'); b.className='bigtimer par'; return; }
    if(state.session.over){ b.textContent = t('over'); b.className='bigtimer over'; return; }
    const el = state.session.elapsed||0;
    b.textContent = fmtTime(el);
    b.className = 'bigtimer ' + (state.session.active?'run':'');
    if(state.session.par && el > state.session.par) b.classList.add('par');
    $('#btn-session-start').disabled = state.session.active;
    $('#btn-session-stop').disabled  = !state.session.active;
  }

  function appendShot(s){
    state.shots.push(s);
    const body = $('#shotlog-body'); if(!body) return;
    if(state.shots.length === 1) body.innerHTML = '';
    const tr = document.createElement('tr');
    const cum = state.shots.reduce((a,b)=>a+(b.score||0), 0);
    tr.innerHTML = `
      <td>${state.shots.length}</td>
      <td>${fmtTime(s.sess||0)}</td>
      <td>T${s.target}</td>
      <td>Z${s.zone}</td>
      <td>${s.score}</td>
      <td>${cum}</td>`;
    body.appendChild(tr);
    body.parentElement.parentElement.scrollTop = 999999;
    drawCharts();
  }

  // ---------- Tiny SVG charts ----------
  function drawCharts(){
    drawScoreOverTime();
    drawZoneDist();
  }
  function drawScoreOverTime(){
    const svg = $('#chart-score'); if(!svg) return;
    const W = svg.clientWidth || 400, H = svg.clientHeight || 180, P = 20;
    svg.setAttribute('viewBox',`0 0 ${W} ${H}`);
    const pts = state.shots.map(s=>s.sess||0);
    const scores = [];
    let cum = 0;
    for(const s of state.shots){ cum += s.score||0; scores.push(cum); }
    if(!scores.length){ svg.innerHTML=`<text x="50%" y="50%" fill="var(--muted)" text-anchor="middle" font-size="12">${t('no_shots')}</text>`; return; }
    const maxX = Math.max(1, pts[pts.length-1]);
    const maxY = Math.max(1, scores[scores.length-1]);
    const x = v => P + (v/maxX)*(W-2*P);
    const y = v => H-P - (v/maxY)*(H-2*P);
    const path = scores.map((v,i)=>`${i?'L':'M'}${x(pts[i]).toFixed(1)},${y(v).toFixed(1)}`).join(' ');
    const area = path + ` L${(W-P).toFixed(1)},${(H-P).toFixed(1)} L${P},${(H-P).toFixed(1)} Z`;
    svg.innerHTML = `
      <defs>
        <linearGradient id="g1" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="var(--primary)" stop-opacity=".6"/>
          <stop offset="100%" stop-color="var(--primary)" stop-opacity="0"/>
        </linearGradient>
      </defs>
      <path d="${area}" fill="url(#g1)"/>
      <path d="${path}" fill="none" stroke="var(--primary)" stroke-width="2"/>
      ${scores.map((v,i)=>`<circle cx="${x(pts[i]).toFixed(1)}" cy="${y(v).toFixed(1)}" r="3" fill="var(--primary)"/>`).join('')}
    `;
  }
  function drawZoneDist(){
    const svg = $('#chart-zone'); if(!svg) return;
    const zones = [0,0,0,0];
    for(const s of state.shots){ if(s.zone>=1&&s.zone<=4) zones[s.zone-1]++; }
    const W = svg.clientWidth || 400, H = svg.clientHeight || 180, P = 26;
    svg.setAttribute('viewBox',`0 0 ${W} ${H}`);
    const labels = ['10','8','6','4'];
    const colors = ['var(--ring-10)','var(--ring-8)','var(--ring-6)','var(--ring-4)'];
    const max = Math.max(1, ...zones);
    const bw = (W-2*P)/4 - 8;
    svg.innerHTML = zones.map((v,i)=>{
      const xL = P + i*((W-2*P)/4);
      const hh = (v/max)*(H-2*P);
      const yy = H-P-hh;
      return `
        <rect x="${xL}" y="${yy}" width="${bw}" height="${hh}" rx="6" fill="${colors[i]}"/>
        <text x="${xL+bw/2}" y="${H-8}" text-anchor="middle" font-size="11" fill="var(--muted)">${labels[i]} pts</text>
        <text x="${xL+bw/2}" y="${yy-4}" text-anchor="middle" font-size="12" fill="var(--text)" font-weight="700">${v}</text>
      `;
    }).join('');
  }

  // ---------- History view ----------
  function buildHistory(){
    $('#view-history').innerHTML = `
      <div class="row" style="margin-bottom:10px">
        <button class="ghost-btn" id="hist-refresh">↻ Refresh</button>
      </div>
      <div id="hist-list"></div>`;
    $('#hist-refresh').onclick = loadHistory;
    loadHistory();
  }
  async function loadHistory(){
    const list = $('#hist-list');
    if(!list) return;
    try{
      const r = await fetch('/api/sessions');
      const data = await r.json();
      if(!data.length){ list.innerHTML = `<div class="session-card" style="text-align:center;color:var(--muted)" data-i="no_sessions">${t('no_sessions')}</div>`; return; }
      list.innerHTML = data.map(s=>`
        <div class="session-card">
          <div class="row" style="justify-content:space-between">
            <div>
              <div style="font-weight:700">${s.file.split('/').pop().replace('.json','')}</div>
              <div style="font-size:12px;color:var(--muted)">${s.size} bytes</div>
            </div>
            <div>
              <a class="ghost-btn" href="${s.file}" download>⬇ JSON</a>
            </div>
          </div>
        </div>`).join('');
    }catch(e){
      list.innerHTML = `<div class="session-card" style="color:var(--danger)">${e.message}</div>`;
    }
  }

  // ---------- Settings view ----------
  function buildSettings(){
    $('#view-settings').innerHTML = `
      <div class="session-card">
        <h3 data-i="config_title">${t('config_title')}</h3>
        <div class="row" style="margin-bottom:8px"><label data-i="threshold">${t('threshold')}</label><input id="cfg-thr" type="number" value="1500" style="width:120px"/></div>
        <div class="row" style="margin-bottom:8px"><label data-i="debounce">${t('debounce')}</label><input id="cfg-deb" type="number" value="150" style="width:120px"/></div>
        <div class="row" style="margin-bottom:8px"><label data-i="heartbeat">${t('heartbeat')}</label><input id="cfg-hb" type="number" value="5000" style="width:120px"/></div>
        <div class="row" style="margin-bottom:8px">
          <label data-i="zone_scores">${t('zone_scores')}</label>
          <input id="cfg-z1" type="number" value="10" style="width:70px"/>
          <input id="cfg-z2" type="number" value="8"  style="width:70px"/>
          <input id="cfg-z3" type="number" value="6"  style="width:70px"/>
          <input id="cfg-z4" type="number" value="4"  style="width:70px"/>
        </div>
        <div class="row"><button class="primary-btn" id="cfg-push" data-i="push_to_targets">${t('push_to_targets')}</button></div>
      </div>

      <div class="session-card">
        <h3>${t('shooter')}s</h3>
        <div class="row" style="margin-bottom:10px">
          <input id="sh-name" data-ip="shooter_name" placeholder="${t('shooter_name')}" style="flex:1;min-width:180px"/>
          <button class="primary-btn" id="sh-add" data-i="add_shooter">${t('add_shooter')}</button>
        </div>
        <div id="sh-list"></div>
      </div>

      <div class="session-card">
        <h3>Identify target</h3>
        <div class="row">
          ${Array.from({length:NUM_TARGETS}).map((_,i)=>`<button class="ghost-btn" data-identify="${i+1}">T${i+1}</button>`).join('')}
        </div>
      </div>`;
    $('#cfg-push').onclick = () => {
      const body = {
        threshold: +$('#cfg-thr').value,
        debounce:  +$('#cfg-deb').value,
        heartbeat: +$('#cfg-hb').value,
        z1:+$('#cfg-z1').value, z2:+$('#cfg-z2').value, z3:+$('#cfg-z3').value, z4:+$('#cfg-z4').value,
        target: 0,
      };
      fetch('/api/config/push',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)})
        .then(()=>toast('Config pushed'))
        .catch(e=>toast('Error: '+e.message));
    };
    $('#sh-add').onclick = async () => {
      const n = $('#sh-name').value.trim(); if(!n) return;
      const r = await fetch('/api/shooters'); const list = await r.json();
      list.push({id: Date.now(), name: n});
      await fetch('/api/shooters',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(list)});
      $('#sh-name').value=''; renderShooters();
    };
    $('#view-settings').querySelectorAll('[data-identify]').forEach(b=>{
      b.onclick = () => window.KIK_WS.send({command:'identify', target:+b.dataset.identify});
    });
    renderShooters();
  }
  async function renderShooters(){
    try{
      const r = await fetch('/api/shooters'); const list = await r.json();
      const el = $('#sh-list');
      el.innerHTML = list.length ? list.map(s=>`<div class="health-row"><div class="tname">👤</div><div>${s.name}</div><div></div></div>`).join('')
                                : `<div style="color:var(--muted);text-align:center;padding:14px">—</div>`;
    }catch(e){}
  }

  // ---------- Export ----------
  function exportCsv(){
    if(!state.shots.length){ toast('No shots'); return; }
    const hdr = 'n,time_ms,target,sensor,zone,score,cum_score,cx,cy';
    let cum = 0;
    const rows = state.shots.map((s,i)=>{
      cum += s.score||0;
      return [i+1,s.sess||0,s.target,s.sensor,s.zone,s.score,cum,s.cx||0,s.cy||0].join(',');
    });
    const blob = new Blob([hdr+'\n'+rows.join('\n')], {type:'text/csv'});
    downloadBlob(blob, `kik_session_${Date.now()}.csv`);
  }
  function exportJson(){
    const payload = { session: state.session, shots: state.shots, exportedAt: new Date().toISOString() };
    const blob = new Blob([JSON.stringify(payload,null,2)], {type:'application/json'});
    downloadBlob(blob, `kik_session_${Date.now()}.json`);
  }
  function downloadBlob(blob, name){
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob); a.download = name;
    a.click(); setTimeout(()=>URL.revokeObjectURL(a.href), 5000);
  }

  // ---------- WS dispatch ----------
  function onMessage(m){
    if(m.type === '__ws'){
      state.connected = (m.state === 'open');
      $('#conn-label').textContent = state.connected ? t('connected') : t('disconnected');
      $('#conn-dot').className = 'status-dot ' + (state.connected?'online':'offline');
      return;
    }
    if(m.type === 'hit'){
      const s = state.targets[m.target]; if(!s) return;
      s.hits[m.sensor] = m.count;
      s.score = m.total || (s.score + (m.score||0));
      s.zones[m.zone] = (s.zones[m.zone]||0)+1;
      flashTarget(m.target);
      drawHitDot(m.target, m.cx||0, m.cy||0, m.score||0);
      renderLive(); renderScore();
      if(state.session.active) appendShot(m);
      return;
    }
    if(m.type === 'health'){
      const s = state.targets[m.target]; if(!s) return;
      s.online = m.online !== false;
      s.status = [0].concat(m.status||[]);
      s.readings = [0].concat(m.readings||[]);
      s.batt = m.batt||0; s.rssi = m.rssi||0; s.fw = m.fw||'-';
      renderLive(); renderHealth();
      return;
    }
    if(m.type === 'online'){
      const s = state.targets[m.target]; if(!s) return;
      s.online = !!m.online; renderLive();
      return;
    }
    if(m.type === 'reset'){
      for(let i=1;i<=NUM_TARGETS;i++){
        const s = state.targets[i];
        s.hits=[0,0,0,0,0]; s.zones=[0,0,0,0,0]; s.score=0;
      }
      state.shots.length = 0;
      $('#shotlog-body').innerHTML = `<tr><td colspan="6" style="color:var(--muted);text-align:center;padding:20px">${t('no_shots')}</td></tr>`;
      document.querySelectorAll('.dots').forEach(g => g.innerHTML='');
      renderLive(); renderScore(); renderHealth(); drawCharts();
      toast('Reset');
      return;
    }
    if(m.type === 'session_start'){
      state.session.active = true;
      state.session.name = m.name||''; state.session.shooter=m.shooter||'';
      state.session.par = m.par||0; state.session.random = !!m.random;
      state.session.waitingGo = !!m.random; state.session.over=false;
      state.session.elapsed = 0;
      state.shots.length = 0;
      $('#shotlog-body').innerHTML = `<tr><td colspan="6" style="color:var(--muted);text-align:center;padding:20px">${t('no_shots')}</td></tr>`;
      toast((t('session_start'))+': '+m.name);
      renderBigTimer();
      return;
    }
    if(m.type === 'timer_go'){
      state.session.waitingGo = false;
      renderBigTimer();
      return;
    }
    if(m.type === 'tick'){
      state.session.elapsed = m.elapsed;
      renderBigTimer();
      updateTopTimer();
      return;
    }
    if(m.type === 'par'){
      toast(t('par'));
      return;
    }
    if(m.type === 'session_stop'){
      state.session.active = false;
      state.session.over = true;
      renderBigTimer();
      toast('Session saved ('+m.shots+' '+t('shots')+')');
      setTimeout(()=>loadHistory(), 300);
      return;
    }
    if(m.type === 'status' || m.targets){
      // Initial snapshot
      if(m.session){
        state.session.active = !!m.session.active;
        state.session.par = m.session.par||0;
        state.session.elapsed = m.session.elapsed||0;
        state.session.name = m.session.name||'';
        state.session.shooter = m.session.shooter||'';
      }
      if(Array.isArray(m.targets)){
        for(const o of m.targets){
          const s = state.targets[o.id]; if(!s) continue;
          s.online = !!o.online;
          s.hits  = [0].concat(o.hits||[]);
          s.zones = [0].concat(o.zones||[]);
          s.status= [0].concat(o.status||[]);
          s.score = o.score||0;
          s.batt = o.batt||0; s.rssi = o.rssi||0; s.fw = o.fw||'-';
        }
      }
      renderLive(); renderScore(); renderHealth(); renderBigTimer();
    }
  }

  function updateTopTimer(){
    const top = $('#top-timer');
    if(!top) return;
    top.textContent = fmtTime(state.session.elapsed||0);
    top.className = 'timer ' + (state.session.active?'running':'');
    if(state.session.par && state.session.elapsed > state.session.par) top.classList.add('par');
  }

  // ---------- Theme ----------
  function toggleTheme(){
    const cur = document.documentElement.getAttribute('data-theme') || 'dark';
    const nxt = cur === 'dark' ? 'light' : 'dark';
    document.documentElement.setAttribute('data-theme', nxt);
    localStorage.setItem(THEME_KEY, nxt);
  }

  function initTheme(){
    const saved = localStorage.getItem(THEME_KEY) || 'dark';
    document.documentElement.setAttribute('data-theme', saved);
  }

  // ---------- Boot ----------
  function boot(){
    initTheme();
    buildShell();
    // i18n: update initial strings
    window.setLang(window.KIK_LANG);

    window.addEventListener('langchange', ()=>{
      renderLive(); renderScore(); renderHealth(); renderBigTimer(); drawCharts();
    });

    $('#btn-lang').onclick = () => setLang(window.KIK_LANG === 'ms' ? 'en' : 'ms');
    $('#btn-theme').onclick = toggleTheme;

    window.KIK_WS.on(onMessage);
    window.KIK_WS.connect('/ws');

    // Register service worker if available
    if('serviceWorker' in navigator){
      navigator.serviceWorker.register('/sw.js').catch(()=>{});
    }
  }
  document.addEventListener('DOMContentLoaded', boot);
})();
