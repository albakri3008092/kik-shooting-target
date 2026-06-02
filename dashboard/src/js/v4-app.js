// ===========================================================
//  KIK Dashboard V4 — Industrial Panel Single-Page App
// ===========================================================
(function(){
  'use strict';
  const NUM_TARGETS = 15;
  const PASS_THRESHOLD = 16;

  // Difficulty presets: par time in seconds, threshold sensitivity
  const SETS = {
    1: { label:'SET 1 (Easy)',   parSec: 120, threshold: 1200 },
    2: { label:'SET 2 (Medium)', parSec: 60,  threshold: 1500 },
    3: { label:'SET 3 (Hard)',   parSec: 30,  threshold: 1800 },
  };

  // ---------- State ----------
  const state = {
    connected: false,
    currentSet: 1,
    session: { active:false, elapsed:0, par:0 },
    totalScore: 0,
    totalHits: 0,
    targets: Array.from({length: NUM_TARGETS+1}, ()=>({
      online: false,
      hits: 0,
      score: 0,
      status: [0,0,0,0],  // 4 sensors: 0=ok, 1=warn, 2=fail
    })),
    dots: [],   // all hit dots for the big bullseye
  };

  const $ = s => document.querySelector(s);
  const $$ = s => document.querySelectorAll(s);

  // ---------- Timer formatting ----------
  function fmtTimer(ms){
    const totalSec = Math.floor(ms / 1000);
    const m = Math.floor(totalSec / 60);
    const s = totalSec % 60;
    return m + ':' + String(s).padStart(2, '0');
  }

  // ---------- Render score ----------
  function renderScore(){
    const el = $('#total-score');
    state.totalScore = 0;
    state.totalHits = 0;
    for(let i=1; i<=NUM_TARGETS; i++){
      state.totalScore += state.targets[i].score;
      state.totalHits += state.targets[i].hits;
    }
    el.textContent = 'SCORE: ' + state.totalScore;
  }

  // ---------- Render pass/fail ----------
  function renderPassFail(){
    const pf = $('#passfail');
    const passRow = pf.querySelector('.pass-row');
    const failRow = pf.querySelector('.fail-row');
    if(state.totalScore >= PASS_THRESHOLD){
      passRow.classList.remove('dimmed');
      failRow.classList.add('dimmed');
    } else {
      passRow.classList.add('dimmed');
      failRow.classList.remove('dimmed');
    }
  }

  // ---------- Render target active count ----------
  function renderTargetActive(){
    let online = 0;
    for(let i=1; i<=NUM_TARGETS; i++){
      if(state.targets[i].online) online++;
    }
    const dot = $('#ta-dot');
    const txt = $('#ta-text');
    if(online > 0){
      dot.classList.remove('offline');
      txt.textContent = online + '/' + NUM_TARGETS + ' ACTIVE';
    } else {
      dot.classList.add('offline');
      txt.textContent = 'NO TARGET';
    }
    // Online indicator in status icons
    const si = $('#si-online');
    if(si) si.classList.toggle('offline', online === 0);
  }

  // ---------- Render timer ----------
  function renderTimer(){
    const el = $('#timer');
    el.textContent = fmtTimer(state.session.elapsed || 0);
    el.classList.toggle('running', state.session.active);
    if(state.session.par && state.session.elapsed > state.session.par){
      el.classList.add('warning');
    } else {
      el.classList.remove('warning');
      el.classList.remove('over');
    }
  }

  // ---------- Render target buttons ----------
  function renderTargets(){
    for(let i=1; i<=NUM_TARGETS; i++){
      const btn = $('#tbtn-'+i);
      if(!btn) continue;
      const t = state.targets[i];
      btn.textContent = t.hits || i;
      btn.classList.toggle('offline', !t.online);

      // Sensor dots — green = OK, red = faulty/offline
      const dotsEl = $('#sdots-'+i);
      if(dotsEl){
        const dots = dotsEl.querySelectorAll('.sdot');
        const statusArr = t.status || [0,0,0,0];
        for(let s=0; s<dots.length; s++){
          const st = statusArr[s];
          dots[s].className = 'sdot';
          if(!t.online || st >= 2){
            dots[s].classList.add('red');
          } else {
            dots[s].classList.add('green');
          }
        }
      }
    }
  }

  // ---------- Flash a target button on hit ----------
  function flashTarget(id){
    const btn = $('#tbtn-'+id);
    if(!btn) return;
    btn.classList.add('hit-flash');
    setTimeout(()=> btn.classList.remove('hit-flash'), 500);
  }

  // ---------- Draw hit dot on big bullseye ----------
  function drawHitDot(cx, cy, score){
    const g = $('#all-dots');
    if(!g) return;
    const ns = 'http://www.w3.org/2000/svg';
    const dot = document.createElementNS(ns, 'circle');
    // Scale: centroid is -900..900, map to -100..100
    const x = Math.max(-100, Math.min(100, cx / 9));
    const y = Math.max(-100, Math.min(100, -cy / 9));
    dot.setAttribute('cx', x.toFixed(1));
    dot.setAttribute('cy', y.toFixed(1));
    dot.setAttribute('r', '3.5');
    dot.setAttribute('class', 'hit-dot');
    // Color by score
    const color = score >= 10 ? '#ff3a3a'
                : score >= 8  ? '#ffcc00'
                : score >= 6  ? '#2a6aff'
                : '#00cc44';
    dot.setAttribute('fill', color);
    dot.setAttribute('opacity', '0.85');
    dot.style.animation = 'pop .3s ease';
    g.appendChild(dot);
    // Cap to 50 dots
    while(g.children.length > 50) g.removeChild(g.firstChild);
  }

  // ---------- SET buttons ----------
  function initSets(){
    for(let s=1; s<=3; s++){
      const btn = $('#set-'+s);
      if(!btn) continue;
      btn.onclick = () => {
        state.currentSet = s;
        $$('.set-btn').forEach(b => b.classList.remove('set-active'));
        btn.classList.add('set-active');
      };
    }
  }

  // ---------- START / RESET ----------
  function initControls(){
    const startBtn = $('#btn-start');
    const resetBtn = $('#btn-reset');

    startBtn.onclick = () => {
      const set = SETS[state.currentSet];
      const par = set.parSec * 1000;
      window.KIK_WS.send({
        command: 'start_session',
        name: 'Set ' + state.currentSet,
        shooter: '',
        par: par,
        random: false,
        set: state.currentSet,
      });
      startBtn.classList.add('active');
      startBtn.textContent = 'RUNNING...';
    };

    resetBtn.onclick = () => {
      window.KIK_WS.send({ command: 'reset' });
      startBtn.classList.remove('active');
      startBtn.textContent = 'START';
    };

    const csvBtn = $('#btn-csv');
    if(csvBtn) csvBtn.onclick = exportCSV;

    // Settings modal
    const settingsBtn = $('#btn-settings');
    const modal = $('#settings-modal');
    const slider = $('#thr-slider');
    const valDisplay = $('#thr-value');
    const saveBtn = $('#thr-save');
    const closeBtn = $('#thr-close');

    if(settingsBtn && modal){
      settingsBtn.onclick = () => modal.classList.add('open');
      closeBtn.onclick = () => modal.classList.remove('open');
      modal.onclick = (e) => { if(e.target === modal) modal.classList.remove('open'); };
      slider.oninput = () => { valDisplay.textContent = slider.value; };
      saveBtn.onclick = () => {
        const thr = parseInt(slider.value, 10);
        fetch('/api/config/push', {
          method: 'POST',
          headers: {'Content-Type':'application/json'},
          body: JSON.stringify({ threshold: thr, target: 0 })
        }).then(r => {
          if(r.ok){
            valDisplay.textContent = thr + ' ✓';
            setTimeout(() => { valDisplay.textContent = thr; }, 1500);
          }
        }).catch(() => {});
      };
    }
  }

  // ---------- Export CSV ----------
  function exportCSV(){
    const rows = [['Target','Hits','Score','Sensor 1','Sensor 2','Sensor 3','Sensor 4','Online']];
    for(let i=1; i<=NUM_TARGETS; i++){
      const t = state.targets[i];
      const sensors = (t.status || [0,0,0,0]).map(s => s >= 2 ? 'ROSAK' : 'OK');
      rows.push([i, t.hits, t.score, sensors[0], sensors[1], sensors[2], sensors[3], t.online ? 'YA' : 'TIDAK']);
    }
    const totalScore = state.totalScore || 0;
    const totalHits = state.totalHits || 0;
    const pf = totalHits >= PASS_THRESHOLD ? 'PASS' : 'FAIL';
    rows.push([]);
    rows.push(['JUMLAH', totalHits, totalScore]);
    rows.push(['KEPUTUSAN', pf]);
    rows.push(['SET', state.currentSet]);
    rows.push(['MASA', fmtTimer(state.session.elapsed)]);

    const csv = rows.map(r => r.join(',')).join('\n');
    const blob = new Blob(['\uFEFF' + csv], {type:'text/csv;charset=utf-8'});
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'KIK_Skor_' + new Date().toISOString().slice(0,10) + '.csv';
    a.click();
    URL.revokeObjectURL(url);
  }

  // ---------- WS message handler ----------
  function onMessage(m){
    if(m.type === '__ws'){
      state.connected = (m.state === 'open');
      return;
    }

    if(m.type === 'hit'){
      const t = state.targets[m.target];
      if(!t) return;
      t.hits++;
      t.score = m.total || (t.score + (m.score || 0));
      flashTarget(m.target);
      drawHitDot(m.cx || 0, m.cy || 0, m.score || 0);
      renderScore();
      renderPassFail();
      renderTargets();
      return;
    }

    if(m.type === 'health'){
      const t = state.targets[m.target];
      if(!t) return;
      t.online = m.online !== false;
      t.status = m.status || [0,0,0,0];
      renderTargets();
      renderTargetActive();
      return;
    }

    if(m.type === 'online'){
      const t = state.targets[m.target];
      if(!t) return;
      t.online = !!m.online;
      renderTargets();
      renderTargetActive();
      return;
    }

    if(m.type === 'reset'){
      for(let i=1; i<=NUM_TARGETS; i++){
        state.targets[i].hits = 0;
        state.targets[i].score = 0;
      }
      state.totalScore = 0;
      state.totalHits = 0;
      state.session.active = false;
      state.session.elapsed = 0;
      // Clear dots
      const g = $('#all-dots');
      if(g) g.innerHTML = '';
      // Reset UI
      const startBtn = $('#btn-start');
      if(startBtn){ startBtn.classList.remove('active'); startBtn.textContent = 'START'; }
      renderScore();
      renderPassFail();
      renderTargets();
      renderTimer();
      return;
    }

    if(m.type === 'session_start'){
      state.session.active = true;
      state.session.par = m.par || 0;
      state.session.elapsed = 0;
      const startBtn = $('#btn-start');
      if(startBtn){ startBtn.classList.add('active'); startBtn.textContent = 'RUNNING...'; }
      renderTimer();
      return;
    }

    if(m.type === 'tick'){
      state.session.elapsed = m.elapsed || 0;
      renderTimer();
      return;
    }

    if(m.type === 'session_stop'){
      state.session.active = false;
      const startBtn = $('#btn-start');
      if(startBtn){ startBtn.classList.remove('active'); startBtn.textContent = 'START'; }
      renderTimer();
      return;
    }

    if(m.type === 'par'){
      const el = $('#timer');
      if(el) el.classList.add('over');
      return;
    }

    // Initial status snapshot
    if(m.type === 'status' || m.targets){
      if(m.session){
        state.session.active = !!m.session.active;
        state.session.par = m.session.par || 0;
        state.session.elapsed = m.session.elapsed || 0;
      }
      if(Array.isArray(m.targets)){
        for(const o of m.targets){
          const t = state.targets[o.id];
          if(!t) continue;
          t.online = !!o.online;
          t.hits = (o.hits || []).reduce((a,b) => a+b, 0);
          t.score = o.score || 0;
          t.status = o.status || [0,0,0,0];
        }
      }
      renderScore();
      renderPassFail();
      renderTargets();
      renderTargetActive();
      renderTimer();
    }
  }

  // ---------- Boot ----------
  function boot(){
    initSets();
    initControls();
    renderScore();
    renderPassFail();
    renderTargets();
    renderTargetActive();
    renderTimer();

    window.KIK_WS.on(onMessage);
    window.KIK_WS.connect('/ws');

    if('serviceWorker' in navigator){
      navigator.serviceWorker.register('/sw.js').catch(()=>{});
    }
  }

  document.addEventListener('DOMContentLoaded', boot);
})();
