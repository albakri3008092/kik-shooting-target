// Robust WebSocket client with auto-reconnect + pub/sub.
(function(){
  const listeners = new Set();
  let ws = null;
  let reconnectTimer = null;
  let url = null;
  let connected = false;
  const pending = [];

  function defaultUrl(path='/ws'){
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    return proto + '//' + location.host + path;
  }
  function emit(msg){ listeners.forEach(fn => { try{ fn(msg) }catch(e){console.error(e)} }); }

  function connect(path='/ws'){
    url = defaultUrl(path);
    try{ ws = new WebSocket(url); }
    catch(e){ schedule(); return; }
    ws.onopen = () => {
      connected = true;
      emit({type:'__ws', state:'open'});
      while(pending.length){ ws.send(pending.shift()); }
    };
    ws.onclose = () => {
      connected = false;
      emit({type:'__ws', state:'close'});
      schedule();
    };
    ws.onerror = () => { try{ ws.close() }catch(_){} };
    ws.onmessage = (e) => {
      try{ emit(JSON.parse(e.data)); }
      catch(err){ /* ignore non-json */ }
    };
  }
  function schedule(){
    if(reconnectTimer) return;
    reconnectTimer = setTimeout(()=>{ reconnectTimer=null; connect(url ? new URL(url).pathname : '/ws'); }, 1500);
  }
  function send(obj){
    const s = typeof obj === 'string' ? obj : JSON.stringify(obj);
    if(ws && ws.readyState === WebSocket.OPEN) ws.send(s);
    else pending.push(s);
  }
  window.KIK_WS = {
    connect,
    send,
    on: (fn) => { listeners.add(fn); return () => listeners.delete(fn); },
    isConnected: () => connected,
  };
})();
