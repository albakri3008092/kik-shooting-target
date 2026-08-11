/* Bahasa Malaysia (default) + English strings for the operator console. */
window.ATOVCD_I18N = {
  ms: {
    brand_sub: 'Pengesanan Perubahan Visual Sasaran Automatik',
    session: 'Sesi', uptime: 'Operasi',
    tab_live: 'LANGSUNG', tab_change: 'PETA PERUBAHAN', tab_history: 'REKOD',
    tab_report: 'LAPORAN', tab_settings: 'TETAPAN',
    start_session: 'MULA SESI', stop_session: 'TAMAT SESI',
    live_camera: 'KAMERA LANGSUNG', target_status: 'STATUS SASARAN',
    confidence: 'Keyakinan', system_health: 'KESIHATAN SISTEM',
    camera: 'Kamera', battery: 'Bateri', fps: 'Kadar bingkai',
    c_new: 'BARU', c_old: 'LAMA', c_unc: 'TIDAK PASTI', c_tot: 'JUMLAH',
    change_map: 'PETA PERUBAHAN VISUAL', legend: 'PETUNJUK',
    l_new: 'Perubahan visual baharu', l_old: 'Perubahan lama / historikal',
    l_unc: 'Tidak pasti — perlu semakan', l_det: 'Normal / dikesan',
    latest_changes: 'PERUBAHAN TERKINI', history_title: 'REKOD SESI',
    th_time: 'Masa', th_target: 'Sasaran', th_change: 'Perubahan', th_conf: 'Keyakinan',
    report_title: 'LAPORAN SESI', export: 'EKSPORT',
    report_hint: 'Fail disimpan terus ke tablet melalui pelayar — tiada internet diperlukan.',
    s_camera: 'KAMERA', s_res: 'Resolusi', s_fps: 'Kadar bingkai (fps)',
    s_ai: 'PENGESANAN AI', s_conf: 'Ambang keyakinan', s_sens: 'Kepekaan perubahan',
    s_net: 'RANGKAIAN & STORAN', s_chan: 'Saluran Wi-Fi', s_store: 'Had storan (MB)',
    s_power: 'KUASA & SESI', s_batt: 'Pantau bateri', s_save: 'SIMPAN',
    saved: 'Tetapan disimpan.', no_session: 'Tiada sesi aktif — tekan MULA SESI.',
    no_records: 'Tiada rekod.'
  },
  en: {
    brand_sub: 'Automated Target Observation & Visual Change Detection',
    session: 'Session', uptime: 'Runtime',
    tab_live: 'LIVE', tab_change: 'CHANGE MAP', tab_history: 'HISTORY',
    tab_report: 'REPORT', tab_settings: 'SETTINGS',
    start_session: 'START SESSION', stop_session: 'STOP SESSION',
    live_camera: 'LIVE CAMERA', target_status: 'TARGET STATUS',
    confidence: 'Confidence', system_health: 'SYSTEM HEALTH',
    camera: 'Camera', battery: 'Battery', fps: 'Frame rate',
    c_new: 'NEW', c_old: 'OLD', c_unc: 'UNCERTAIN', c_tot: 'TOTAL',
    change_map: 'VISUAL CHANGE MAP', legend: 'LEGEND',
    l_new: 'New visual change', l_old: 'Historical change',
    l_unc: 'Uncertain — needs review', l_det: 'Normal / detected',
    latest_changes: 'LATEST CHANGES', history_title: 'SESSION RECORDS',
    th_time: 'Time', th_target: 'Target', th_change: 'Change', th_conf: 'Confidence',
    report_title: 'SESSION REPORT', export: 'EXPORT',
    report_hint: 'Files download straight to the tablet through the browser — no internet needed.',
    s_camera: 'CAMERA', s_res: 'Resolution', s_fps: 'Frame rate (fps)',
    s_ai: 'AI DETECTION', s_conf: 'Confidence threshold', s_sens: 'Change sensitivity',
    s_net: 'NETWORK & STORAGE', s_chan: 'Wi-Fi channel', s_store: 'Storage limit (MB)',
    s_power: 'POWER & SESSION', s_batt: 'Monitor battery', s_save: 'SAVE',
    saved: 'Settings saved.', no_session: 'No active session — press START SESSION.',
    no_records: 'No records.'
  }
};

window.ATOVCD_LANG = localStorage.getItem('atovcd_lang') || 'ms';

window.t = function (key) {
  const dict = window.ATOVCD_I18N[window.ATOVCD_LANG] || window.ATOVCD_I18N.ms;
  return dict[key] || key;
};

window.applyI18n = function () {
  document.documentElement.lang = window.ATOVCD_LANG;
  document.querySelectorAll('[data-i]').forEach((el) => {
    el.textContent = window.t(el.dataset.i);
  });
};
