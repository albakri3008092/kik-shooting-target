/* =====================================================================
   ATOVCD operator console — polls the FastAPI server over the local Wi-Fi.
   ===================================================================== */
'use strict';

const STATE_CLASS = { NEW: 'new', OLD: 'old', UNCERTAIN: 'unc', DETECTED: 'det' };
const POLL_MS = 700;

const $ = (id) => document.getElementById(id);
const stateClass = (s) => STATE_CLASS[s] || 'det';

let currentView = 'live';
let activeSession = null;
let historySelection = '';
let reportSelection = '';

/* ------------------------------------------------------------ utilities */
function pad(n) { return String(n).padStart(2, '0'); }

function clockText(epoch) {
  const d = new Date(epoch * 1000);
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}

function durationText(seconds) {
  const s = Math.max(0, Math.floor(seconds));
  return `${pad(Math.floor(s / 3600))}:${pad(Math.floor((s % 3600) / 60))}:${pad(s % 60)}`;
}

async function api(path, options) {
  const res = await fetch(path, options);
  if (!res.ok) throw new Error(`${path} -> ${res.status}`);
  return res.headers.get('content-type').includes('json') ? res.json() : res.text();
}

/* ---------------------------------------------------------------- tabs */
function showView(view) {
  currentView = view;
  document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t.dataset.view === view));
  document.querySelectorAll('.view').forEach((v) => v.classList.toggle('active', v.id === `view-${view}`));
  if (view === 'history') refreshHistory();
  if (view === 'report') refreshReport();
}

/* ---------------------------------------------------------------- live */
function drawOverlay(targets) {
  const img = $('stream');
  const canvas = $('overlay');
  const stage = canvas.parentElement;
  canvas.width = stage.clientWidth;
  canvas.height = stage.clientHeight;
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  if (!img.naturalWidth) return;

  // Match the letterbox that object-fit:contain produces.
  const scale = Math.min(canvas.width / img.naturalWidth, canvas.height / img.naturalHeight);
  const vw = img.naturalWidth * scale;
  const vh = img.naturalHeight * scale;
  const ox = (canvas.width - vw) / 2;
  const oy = (canvas.height - vh) / 2;
  const colours = { NEW: '#ff5d6c', OLD: '#8fa3ab', UNCERTAIN: '#ffcf4d', DETECTED: '#2fe08a' };

  ctx.font = '600 12px ui-monospace, monospace';
  ctx.lineWidth = 2;
  targets.forEach((t) => {
    const colour = colours[t.state] || colours.DETECTED;
    const x = ox + t.bbox.x * vw;
    const y = oy + t.bbox.y * vh;
    const w = t.bbox.w * vw;
    const h = t.bbox.h * vh;
    ctx.strokeStyle = colour;
    ctx.strokeRect(x, y, w, h);
    const label = `${t.id}  ${t.state}  ${Math.round(t.confidence * 100)}%`;
    const tw = ctx.measureText(label).width + 10;
    ctx.fillStyle = colour;
    ctx.fillRect(x, Math.max(0, y - 18), tw, 18);
    ctx.fillStyle = '#04090b';
    ctx.fillText(label, x + 5, Math.max(12, y - 5));
  });
}

function renderLive(status) {
  const primary = status.primary_target;
  $('primary-name').textContent = primary.id;
  $('primary-dot').className = `dot ${stateClass(primary.state)}`;
  $('primary-change').textContent = primary.state;
  $('primary-change').className = `primary-change ${stateClass(primary.state)}`;
  $('primary-conf').textContent = `${Math.round(primary.confidence * 100)}%`;
  $('primary-bar').style.width = `${Math.round(primary.confidence * 100)}%`;

  $('target-list').innerHTML = status.targets
    .map((t) => `<li><span class="dot ${stateClass(t.state)}"></span>
        <b>${t.id}</b><span class="grow">${t.state}</span>
        <span class="mono">${Math.round(t.confidence * 100)}%</span></li>`)
    .join('');

  $('cam-res').textContent = `${status.camera.width}×${status.camera.height} @ ${status.camera.fps}fps`;
  $('hp-camera').textContent = status.camera.status;
  $('hp-imu').textContent = `${status.imu.status} · P${status.imu.pitch}° R${status.imu.roll}°`;
  $('hp-batt').textContent = status.battery.monitored ? `${status.battery.percent}%` : '—';
  $('hp-fps').textContent = `${status.camera.fps} fps`;

  $('c-new').textContent = pad(status.counts.new);
  $('c-old').textContent = pad(status.counts.old);
  $('c-unc').textContent = pad(status.counts.uncertain);
  $('c-tot').textContent = pad(status.counts.total);
  drawOverlay(status.targets);
}

/* ----------------------------------------------------------- change map */
function renderChangeMap(status) {
  const canvas = $('map');
  const stage = canvas.parentElement;
  canvas.width = stage.clientWidth;
  canvas.height = stage.clientHeight;
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, canvas.width, canvas.height);

  ctx.strokeStyle = '#1f383f';
  ctx.lineWidth = 1;
  for (let i = 1; i < 8; i += 1) {
    const x = (canvas.width / 8) * i;
    const y = (canvas.height / 8) * i;
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, canvas.height); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(canvas.width, y); ctx.stroke();
  }
  ctx.strokeStyle = '#00e0c6';
  ctx.strokeRect(canvas.width * 0.06, canvas.height * 0.08, canvas.width * 0.88, canvas.height * 0.84);

  const colours = { NEW: '#ff5d6c', OLD: '#8fa3ab', UNCERTAIN: '#ffcf4d', DETECTED: '#2fe08a' };
  ctx.font = '600 12px ui-monospace, monospace';
  status.targets.forEach((t) => {
    const cx = (t.bbox.x + t.bbox.w / 2) * canvas.width;
    const cy = (t.bbox.y + t.bbox.h / 2) * canvas.height;
    const colour = colours[t.state] || colours.DETECTED;
    ctx.fillStyle = colour;
    ctx.beginPath(); ctx.arc(cx, cy, 9, 0, Math.PI * 2); ctx.fill();
    ctx.globalAlpha = 0.18;
    ctx.beginPath(); ctx.arc(cx, cy, 26, 0, Math.PI * 2); ctx.fill();
    ctx.globalAlpha = 1;
    ctx.fillText(`${t.id} · ${t.state}`, cx + 16, cy + 4);
  });
}

async function refreshFeed() {
  if (activeSession === null) { $('change-feed').innerHTML = `<li>${window.t('no_session')}</li>`; return; }
  const events = await api(`/api/history?limit=25&session_id=${activeSession}`);
  $('change-feed').innerHTML = events.length
    ? events.map((e) => `<li><span class="mono">${clockText(e.ts)}</span>
        <span class="grow">${e.target}</span>
        <span class="tag ${stateClass(e.change)}">${e.change}</span>
        <span class="mono">${Math.round(e.confidence * 100)}%</span></li>`).join('')
    : `<li>${window.t('no_records')}</li>`;
}

/* -------------------------------------------------------------- history */
async function fillSessionSelects() {
  const sessions = await api('/api/sessions');
  const options = sessions
    .map((s) => `<option value="${s.id}">#${pad(s.id)} · ${new Date(s.started_at * 1000)
      .toLocaleString()}${s.running ? ' · LIVE' : ''}</option>`)
    .join('');
  ['history-session', 'report-session'].forEach((id) => { $(id).innerHTML = options; });
  if (!sessions.length) return;
  historySelection = historySelection || String(activeSession ?? sessions[0].id);
  reportSelection = reportSelection || String(activeSession ?? sessions[0].id);
  $('history-session').value = historySelection;
  $('report-session').value = reportSelection;
}

async function refreshHistory() {
  await fillSessionSelects();
  const session = $('history-session').value;
  if (!session) { $('history-body').innerHTML = `<tr><td colspan="5" class="empty">${window.t('no_session')}</td></tr>`; return; }
  const events = await api(`/api/history?limit=300&session_id=${session}`);
  $('history-body').innerHTML = events.length
    ? events.map((e) => `<tr><td class="mono">${clockText(e.ts)}</td><td>${e.target}</td>
        <td><span class="tag ${stateClass(e.change)}">${e.change}</span></td>
        <td class="mono">${Math.round(e.confidence * 100)}%</td>
        <td class="mono">${e.bbox}</td></tr>`).join('')
    : `<tr><td colspan="5" class="empty">${window.t('no_records')}</td></tr>`;
}

/* --------------------------------------------------------------- report */
async function refreshReport() {
  await fillSessionSelects();
  const session = $('report-session').value;
  if (!session) { $('report-text').textContent = window.t('no_session'); return; }
  $('report-text').textContent = await api(`/api/report?format=text&session_id=${session}`);
}

/* ------------------------------------------------------------- settings */
async function loadSettings() {
  const settings = await api('/api/settings');
  const form = $('settings-form');
  form.resolution.value = `${settings.camera_width}x${settings.camera_height}`;
  form.frame_rate.value = settings.frame_rate;
  form.detection_confidence.value = settings.detection_confidence;
  form.change_sensitivity.value = settings.change_sensitivity;
  form.wifi_ssid.value = settings.wifi_ssid;
  form.wifi_channel.value = settings.wifi_channel;
  form.storage_limit_mb.value = settings.storage_limit_mb;
  form.battery_monitoring.checked = settings.battery_monitoring;
}

async function saveSettings(event) {
  event.preventDefault();
  const form = $('settings-form');
  const [width, height] = form.resolution.value.split('x').map(Number);
  await api('/api/settings', {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      camera_width: width,
      camera_height: height,
      frame_rate: Number(form.frame_rate.value),
      detection_confidence: Number(form.detection_confidence.value),
      change_sensitivity: Number(form.change_sensitivity.value),
      wifi_ssid: form.wifi_ssid.value,
      wifi_channel: Number(form.wifi_channel.value),
      storage_limit_mb: Number(form.storage_limit_mb.value),
      battery_monitoring: form.battery_monitoring.checked
    })
  });
  $('settings-hint').textContent = window.t('saved');
  await loadSettings();
  $('stream').src = `/api/stream.mjpg?ts=${Date.now()}`;
}

/* -------------------------------------------------------------- session */
function renderSessionButton() {
  const button = $('btn-session');
  const running = activeSession !== null;
  button.textContent = window.t(running ? 'stop_session' : 'start_session');
  button.className = `btn ${running ? 'danger' : 'primary'}`;
}

async function toggleSession() {
  const running = activeSession !== null;
  await api(running ? '/api/session/stop' : '/api/session/start', { method: 'POST' });
  historySelection = '';
  reportSelection = '';
  await tick();
}

/* ------------------------------------------------------------ poll loop */
async function tick() {
  let status;
  try {
    status = await api('/api/status');
  } catch (err) {
    $('chip-dot').className = 'dot';
    $('chip-state').textContent = 'OFFLINE';
    return;
  }
  const wasRunning = activeSession;
  activeSession = status.session.running ? status.session.id : null;
  if (wasRunning !== activeSession) renderSessionButton();

  $('chip-dot').className = 'dot on';
  $('chip-state').textContent = 'ONLINE';
  $('chip-clock').textContent = clockText(status.server_time);
  $('chip-session').textContent = activeSession === null ? '—' : `#${pad(activeSession)}`;
  $('chip-duration').textContent = durationText(status.session.duration_s);

  if (currentView === 'live') renderLive(status);
  if (currentView === 'change') { renderChangeMap(status); await refreshFeed(); }
  if (currentView === 'history' && activeSession !== null && historySelection === String(activeSession)) {
    await refreshHistory();
  }
}

/* ----------------------------------------------------------------- boot */
document.addEventListener('DOMContentLoaded', async () => {
  window.applyI18n();
  $('btn-lang').textContent = window.ATOVCD_LANG === 'ms' ? 'EN' : 'MS';
  document.querySelectorAll('.tab').forEach((tab) => {
    tab.addEventListener('click', () => showView(tab.dataset.view));
  });
  $('btn-session').addEventListener('click', toggleSession);
  $('settings-form').addEventListener('submit', saveSettings);
  $('history-session').addEventListener('change', (e) => { historySelection = e.target.value; refreshHistory(); });
  $('report-session').addEventListener('change', (e) => { reportSelection = e.target.value; refreshReport(); });
  $('btn-pdf').addEventListener('click', () => window.open(`/api/report?format=pdf&session_id=${$('report-session').value}`, '_blank'));
  $('btn-csv').addEventListener('click', () => window.open(`/api/report?format=csv&session_id=${$('report-session').value}`, '_blank'));
  $('btn-lang').addEventListener('click', () => {
    window.ATOVCD_LANG = window.ATOVCD_LANG === 'ms' ? 'en' : 'ms';
    localStorage.setItem('atovcd_lang', window.ATOVCD_LANG);
    window.applyI18n();
    $('btn-lang').textContent = window.ATOVCD_LANG === 'ms' ? 'EN' : 'MS';
    renderSessionButton();
  });

  $('stream').src = '/api/stream.mjpg';
  await loadSettings();
  renderSessionButton();
  await tick();
  setInterval(tick, POLL_MS);
});
