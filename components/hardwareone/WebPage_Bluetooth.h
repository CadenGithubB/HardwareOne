#ifndef WEBPAGE_BLUETOOTH_H
#define WEBPAGE_BLUETOOTH_H

#include <Arduino.h>
#include "WebServer_Utils.h"
#include "System_BuildConfig.h"
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif

// Registration function - registers /bluetooth URI handler
#if ENABLE_HTTP_SERVER
void registerBluetoothHandlers(httpd_handle_t server);
#else
inline void registerBluetoothHandlers(httpd_handle_t) {}
#endif

// Streamed inner content for Bluetooth page
inline void streamBluetoothInner(httpd_req_t* req) {
#if !ENABLE_BLUETOOTH
  httpd_resp_send_chunk(req, R"HTML(
<div style='text-align:center;padding:2rem'>
  <h2 style='color:var(--warning);margin-bottom:1rem'>Bluetooth Disabled</h2>
  <p style='color:var(--panel-fg);margin-bottom:2rem'>
    Bluetooth has been disabled during firmware compilation to save memory and resources.
  </p>
  <div style='background:var(--crumb-bg);padding:1.5rem;border-radius:10px;border:1px solid var(--border);max-width:500px;margin:0 auto;text-align:left'>
    <h3 style='color:var(--panel-fg);margin:0 0 1rem 0;font-size:1rem'>To Enable Bluetooth:</h3>
    <p style='color:var(--panel-fg);font-size:0.9rem;margin:0'>
      Recompile the firmware with <code style='background:rgba(0,0,0,0.1);padding:2px 6px;border-radius:3px'>ENABLE_BLUETOOTH=1</code>
      in your build configuration.
    </p>
  </div>
  <div style='margin-top:2rem'>
    <a href='/' class='btn'>← Back to Dashboard</a>
    <a href='/settings' class='btn' style='margin-left:1rem'>Settings</a>
  </div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);
  return;
#endif

  // CSS
  httpd_resp_send_chunk(req, R"CSS(
<style>
.bt-header{background:var(--panel-bg);border:1px solid var(--border);border-radius:10px;margin-bottom:16px;padding:10px 16px}
.bt-header-row{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:8px}
.bt-header-left{display:flex;align-items:center;gap:10px}
.bt-header-title{font-size:1.15em;font-weight:700;color:var(--panel-fg)}
.bt-header-right{display:flex;gap:6px;flex-wrap:wrap}
.bt-header-status{font-family:'Courier New',monospace;font-size:.85em;color:var(--muted);line-height:1.35;margin-top:8px;padding-top:8px;border-top:1px solid var(--border);white-space:pre-line;display:none}
.bt-not-init{display:flex;flex-direction:column;align-items:center;justify-content:center;padding:48px 20px;text-align:center;background:var(--panel-bg);border:1px solid var(--border);border-radius:10px;margin-bottom:16px}
.bt-not-init h3{color:var(--panel-fg);margin-bottom:6px;font-size:1.1em}
.bt-not-init p{color:var(--muted);margin-bottom:20px;max-width:380px;font-size:.9em;line-height:1.5}
.bt-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:16px;margin-bottom:16px}
.bt-card{background:var(--panel-bg);border:1px solid var(--border);border-radius:10px;overflow:hidden}
.bt-card-header{padding:12px 14px;border-bottom:1px solid var(--border);display:flex;align-items:center;justify-content:space-between;gap:8px}
.bt-card-title{font-weight:600;color:var(--panel-fg);font-size:.95em}
.bt-card-body{padding:14px}
.bt-conn-item{display:flex;justify-content:space-between;align-items:flex-start;padding:10px 0;border-bottom:1px solid var(--border)}
.bt-conn-item:last-child{border-bottom:none}
.bt-conn-name{font-weight:600;color:var(--panel-fg);font-size:.9em}
.bt-conn-mac{font-family:'Courier New',monospace;font-size:.8em;color:var(--link)}
.bt-conn-meta{font-size:.8em;color:var(--muted);margin-top:2px}
.bt-conn-actions{display:flex;gap:5px;flex-shrink:0}
.bt-empty{text-align:center;color:var(--muted);padding:24px;font-style:italic;font-size:.9em}
.bt-pill{display:inline-block;padding:2px 8px;border-radius:999px;font-size:.75em;font-weight:600}
.bt-pill.connected{background:rgba(0,200,83,0.15);color:#00c853;border:1px solid rgba(0,200,83,0.3)}
.bt-pill.advertising{background:rgba(100,180,255,0.15);color:#64b4ff;border:1px solid rgba(100,180,255,0.3)}
.bt-pill.idle{background:rgba(120,120,120,0.15);color:var(--muted);border:1px solid var(--border)}
.bt-toggle-row{display:flex;align-items:center;justify-content:space-between;padding:8px 0;border-bottom:1px solid var(--border)}
.bt-toggle-row:last-child{border-bottom:none}
.bt-toggle-label{font-size:.9em;color:var(--panel-fg)}
.bt-toggle-sub{font-size:.78em;color:var(--muted);margin-top:1px}
.bt-stream-grid{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-bottom:12px}
.bt-stream-btn{padding:7px 10px;font-size:.82em;text-align:center}
.bt-stream-btn.active{background:var(--accent);color:var(--panel-bg);border-color:var(--accent)}
.bt-field-row{display:flex;gap:8px;align-items:center;margin-bottom:10px}
.bt-field-row input{flex:1;min-width:0}
.bt-field-row .btn{flex-shrink:0}
.bt-section-label{font-size:.75em;font-weight:600;color:var(--muted);text-transform:uppercase;letter-spacing:.06em;margin-bottom:8px;margin-top:16px}
.bt-section-label:first-child{margin-top:0}
.bt-warn{background:var(--warning-bg,rgba(255,200,0,0.08));border:1px solid var(--warning-border,rgba(255,200,0,0.2));border-left:3px solid var(--warning-accent,#ffd600);color:var(--warning-fg,#c8a000);border-radius:6px;padding:8px 12px;font-size:.82em;margin-top:12px;line-height:1.4}
@media(max-width:700px){
  .bt-grid{grid-template-columns:1fr}
  .bt-stream-grid{grid-template-columns:1fr}
  .bt-header-right{flex-wrap:wrap}
}
</style>
)CSS", HTTPD_RESP_USE_STRLEN);

  // Header
  httpd_resp_send_chunk(req, R"HTML(
<div class='bt-header'>
  <div class='bt-header-row'>
    <div class='bt-header-left'>
      <span class='status-indicator status-disabled' id='bt-status-dot'></span>
      <span class='bt-header-title'>Bluetooth</span>
      <span class='bt-pill idle' id='bt-state-pill'>--</span>
    </div>
    <div class='bt-header-right'>
      <button class='btn' id='btn-bt-open'>Open</button>
      <button class='btn' id='btn-bt-close' style='display:none'>Close</button>
      <button class='btn' id='btn-bt-adv' style='display:none'>Advertise</button>
      <button class='btn' id='btn-bt-disconnect' style='display:none'>Disconnect</button>
      <button class='btn' id='btn-bt-refresh' style='background:none;border-color:var(--border);color:var(--muted)'>↻</button>
    </div>
  </div>
  <div class='bt-header-status' id='bt-status-text'></div>
</div>

<!-- Not initialized state -->
<div class='bt-not-init' id='bt-not-init'>
  <h3>Bluetooth is not running</h3>
  <p>Open the BLE server to begin advertising and accept connections from clients.</p>
  <button class='btn' id='btn-bt-open2'>Open Bluetooth</button>
</div>

<!-- Initialized panels -->
<div id='bt-panels' style='display:none'>
  <div class='bt-grid'>

    <!-- Connections -->
    <div class='bt-card'>
      <div class='bt-card-header'>
        <span class='bt-card-title'>Connected Clients</span>
        <span id='bt-conn-count' style='font-size:.8em;color:var(--muted)'>0 / 4</span>
      </div>
      <div class='bt-card-body' id='bt-conn-list'>
        <div class='bt-empty'>No active connections</div>
      </div>
    </div>

    <!-- Streaming -->
    <div class='bt-card'>
      <div class='bt-card-header'>
        <span class='bt-card-title'>Streaming</span>
      </div>
      <div class='bt-card-body'>
        <div class='bt-section-label'>Data Streams</div>
        <div class='bt-stream-grid'>
          <button class='btn bt-stream-btn' id='btn-stream-on'>All On</button>
          <button class='btn bt-stream-btn' id='btn-stream-off'>All Off</button>
          <button class='btn bt-stream-btn' id='btn-stream-sensors'>Sensors</button>
          <button class='btn bt-stream-btn' id='btn-stream-system'>System</button>
          <button class='btn bt-stream-btn' id='btn-stream-events'>Events</button>
        </div>
        <div class='bt-section-label'>Intervals (ms)</div>
        <div class='bt-field-row'>
          <input type='number' id='stream-sensor-ms' class='form-control' min='100' value='1000' placeholder='Sensor ms' />
          <input type='number' id='stream-system-ms' class='form-control' min='100' value='5000' placeholder='System ms' />
          <button class='btn' id='btn-stream-interval'>Apply</button>
        </div>
        <div id='bt-stream-status' style='font-size:.82em;color:var(--muted);min-height:1.2em'></div>
      </div>
    </div>

  </div>

  <!-- Configuration -->
  <div class='bt-card'>
    <div class='bt-card-header'>
      <span class='bt-card-title'>Configuration</span>
      <button class='btn' id='btn-bt-loadconfig' style='background:none;border-color:var(--border);color:var(--muted);font-size:.8em;padding:3px 10px'>Load</button>
    </div>
    <div class='bt-card-body'>
      <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:0 24px'>

        <div>
          <div class='bt-section-label'>Device Name</div>
          <div class='bt-field-row'>
            <input type='text' id='bt-cfg-name' class='form-control' maxlength='30' placeholder='e.g. HardwareOne' />
            <button class='btn' id='btn-bt-savename'>Save</button>
          </div>

          <div class='bt-section-label'>TX Power</div>
          <div class='bt-field-row'>
            <input type='number' id='bt-cfg-txpower' class='form-control' min='0' max='7' placeholder='0–7' style='max-width:90px' />
            <span style='font-size:.82em;color:var(--muted);flex:1'>0 = −12 dBm &nbsp; 7 = +9 dBm</span>
            <button class='btn' id='btn-bt-savetx'>Save</button>
          </div>
        </div>

        <div>
          <div class='bt-section-label'>Options</div>
          <div class='bt-toggle-row'>
            <div>
              <div class='bt-toggle-label'>Auto-Start on Boot</div>
              <div class='bt-toggle-sub'>Open Bluetooth automatically after reboot</div>
            </div>
            <button class='btn bt-stream-btn' id='btn-bt-autostart' style='min-width:52px'>--</button>
          </div>
          <div class='bt-toggle-row'>
            <div>
              <div class='bt-toggle-label'>Require Authentication</div>
              <div class='bt-toggle-sub'>Clients must authenticate before sending commands</div>
            </div>
            <button class='btn bt-stream-btn' id='btn-bt-requireauth' style='min-width:52px'>--</button>
          </div>
        </div>

      </div>
      <div id='bt-cfg-status' style='font-size:.82em;color:var(--muted);margin-top:10px;min-height:1.2em'></div>
    </div>
  </div>

  <div class='bt-warn'>
    <strong>Note:</strong> Bluetooth and Wi-Fi share the 2.4 GHz radio. Throughput and latency may
    degrade when both are active simultaneously.
  </div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  // JavaScript
  httpd_resp_send_chunk(req, R"JS(
<script>
(function(){
  // ── helpers ──────────────────────────────────────────────────────────────
  function cli(cmd){
    return fetch('/api/cli',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      credentials:'same-origin',
      body:'cmd='+encodeURIComponent(cmd)
    }).then(function(r){return r.text();});
  }

  function el(id){ return document.getElementById(id); }
  function setText(id,v){ var e=el(id); if(e) e.textContent=v||''; }

  // ── state ─────────────────────────────────────────────────────────────────
  var btState = 'unknown'; // 'off' | 'advertising' | 'connected' | 'on'
  var autoStartState = null;
  var requireAuthState = null;

  // ── UI helpers ────────────────────────────────────────────────────────────
  function applyState(state){
    btState = state;
    var dot = el('bt-status-dot');
    var pill = el('bt-state-pill');
    var isOn = (state === 'advertising' || state === 'connected' || state === 'on');

    if(dot) dot.className = 'status-indicator ' + (isOn ? 'status-enabled' : 'status-disabled');

    if(pill){
      pill.className = 'bt-pill ' + (state === 'connected' ? 'connected' : state === 'advertising' ? 'advertising' : 'idle');
      pill.textContent = state === 'off' ? 'Off' :
                         state === 'advertising' ? 'Advertising' :
                         state === 'connected'   ? 'Connected' :
                         state === 'on'          ? 'Running' : '--';
    }

    var panels = el('bt-panels');
    var notInit = el('bt-not-init');
    if(panels) panels.style.display = isOn ? '' : 'none';
    if(notInit) notInit.style.display = isOn ? 'none' : 'flex';

    var openBtn  = el('btn-bt-open');
    var closeBtn = el('btn-bt-close');
    var advBtn   = el('btn-bt-adv');
    var discBtn  = el('btn-bt-disconnect');
    if(openBtn)  openBtn.style.display  = isOn ? 'none' : '';
    if(closeBtn) closeBtn.style.display = isOn ? ''     : 'none';
    if(advBtn)   advBtn.style.display   = (isOn && state !== 'connected') ? '' : 'none';
    if(discBtn)  discBtn.style.display  = (state === 'connected') ? '' : 'none';
  }

  function parseState(text){
    var lower = (text || '').toLowerCase();
    if(lower.indexOf('not initialized') >= 0) return 'off';
    if(lower.indexOf('connected') >= 0) return 'connected';
    if(lower.indexOf('advertising') >= 0) return 'advertising';
    if(lower.indexOf('disabled') >= 0 || lower.indexOf('stopped') >= 0) return 'off';
    if(lower.indexOf('ble status') >= 0) return 'on';
    return 'unknown';
  }

  function renderConnections(text){
    var list = el('bt-conn-list');
    if(!list) return;

    var lines = (text || '').split('\n');
    // Parse "Active connections: X/Y"
    var countMatch = text.match(/Active connections:\s*(\d+)\/(\d+)/);
    if(countMatch) setText('bt-conn-count', countMatch[1] + ' / ' + countMatch[2]);

    var slots = [];
    for(var i = 0; i < lines.length; i++){
      var slotMatch = lines[i].match(/^\[(\d+)\]\s+(.+)/);
      if(slotMatch){
        var details = lines[i+1] || '';
        var macMatch  = details.match(/MAC:\s*([0-9A-Fa-f:]+)/);
        var durMatch  = details.match(/(\d+)\s*sec/);
        var cmdMatch  = details.match(/(\d+)\s*cmds/);
        slots.push({
          slot: slotMatch[1],
          name: slotMatch[2].trim(),
          mac:  macMatch  ? macMatch[1]  : '--',
          dur:  durMatch  ? durMatch[1]  + 's' : '--',
          cmds: cmdMatch  ? cmdMatch[1]  : '0'
        });
      }
    }

    if(slots.length === 0){
      list.innerHTML = '<div class="bt-empty">No active connections</div>';
      return;
    }

    var html = '';
    slots.forEach(function(s){
      html += '<div class="bt-conn-item">'
        + '<div>'
        + '<div class="bt-conn-name">' + s.name + '</div>'
        + '<div class="bt-conn-mac">'  + s.mac  + '</div>'
        + '<div class="bt-conn-meta">Slot ' + s.slot + ' &nbsp;·&nbsp; ' + s.dur + ' &nbsp;·&nbsp; ' + s.cmds + ' cmds</div>'
        + '</div>'
        + '<div class="bt-conn-actions">'
        + '<button class="btn" style="font-size:.78em;padding:3px 8px" onclick="btDisconnect()">Kick</button>'
        + '</div>'
        + '</div>';
    });
    list.innerHTML = html;
  }

  function showStatusText(text){
    var box = el('bt-status-text');
    if(!box) return;
    if(text && text.trim()){
      box.style.display = 'block';
      box.textContent = text;
    } else {
      box.style.display = 'none';
    }
  }

  // ── refresh ───────────────────────────────────────────────────────────────
  function refresh(){
    cli('blestatus').then(function(out){
      var state = parseState(out);
      applyState(state);
      if(state !== 'off') {
        renderConnections(out);
        showStatusText(out);
      } else {
        showStatusText('');
      }
    }).catch(function(){ applyState('off'); });
  }

  // ── config load ───────────────────────────────────────────────────────────
  function loadConfig(){
    setText('bt-cfg-status', 'Loading…');
    cli('bleinfo').then(function(out){
      var nameMatch = out.match(/Device Name:\s*(.+)/);
      if(nameMatch){ var inp = el('bt-cfg-name'); if(inp) inp.value = nameMatch[1].trim(); }

      var txMatch = out.match(/TX Power:\s*(\d+)/);
      if(txMatch){ var inp = el('bt-cfg-txpower'); if(inp) inp.value = txMatch[1]; }

      autoStartState   = /Auto-Start:\s*Yes/i.test(out);
      requireAuthState = /Require Auth:\s*Yes/i.test(out);
      updateToggle('btn-bt-autostart',   autoStartState);
      updateToggle('btn-bt-requireauth', requireAuthState);

      setText('bt-cfg-status', 'Loaded at ' + new Date().toLocaleTimeString());
    }).catch(function(){
      setText('bt-cfg-status', 'Failed to load config');
    });
  }

  function updateToggle(btnId, enabled){
    var btn = el(btnId);
    if(!btn) return;
    btn.textContent = enabled ? 'On' : 'Off';
    btn.className = 'btn bt-stream-btn' + (enabled ? ' active' : '');
  }

  // ── actions ───────────────────────────────────────────────────────────────
  window.btDisconnect = function(){
    cli('bledisconnect').then(function(){ setTimeout(refresh, 400); });
  };

  function runAndRefresh(cmd, statusEl, msg){
    if(statusEl) setText(statusEl, msg || 'Running…');
    cli(cmd).then(function(out){
      if(statusEl) setText(statusEl, out || 'Done');
      setTimeout(refresh, 400);
    }).catch(function(e){
      if(statusEl) setText(statusEl, 'Error: ' + e.message);
    });
  }

  function bind(id, fn){
    var e = el(id); if(e) e.addEventListener('click', fn);
  }

  // ── event bindings ────────────────────────────────────────────────────────
  document.addEventListener('DOMContentLoaded', function(){

    // Header buttons
    bind('btn-bt-open',       function(){ runAndRefresh('openble', null, null); });
    bind('btn-bt-open2',      function(){ runAndRefresh('openble', null, null); });
    bind('btn-bt-close',      function(){ runAndRefresh('closeble', null, null); });
    bind('btn-bt-adv',        function(){ cli('bleadv').then(function(out){ showStatusText(out); }); });
    bind('btn-bt-disconnect', function(){ cli('bledisconnect').then(function(){ setTimeout(refresh,400); }); });
    bind('btn-bt-refresh',    function(){ refresh(); });

    // Streaming
    bind('btn-stream-on',      function(){ cli('blestream on').then(function(o){ setText('bt-stream-status', o); }); });
    bind('btn-stream-off',     function(){ cli('blestream off').then(function(o){ setText('bt-stream-status', o); }); });
    bind('btn-stream-sensors', function(){ cli('blestream sensors').then(function(o){ setText('bt-stream-status', o); }); });
    bind('btn-stream-system',  function(){ cli('blestream system').then(function(o){ setText('bt-stream-status', o); }); });
    bind('btn-stream-events',  function(){ cli('blestream events').then(function(o){ setText('bt-stream-status', o); }); });
    bind('btn-stream-interval', function(){
      var sm = el('stream-sensor-ms'); var sy = el('stream-system-ms');
      var s = sm ? (sm.value || '1000') : '1000';
      var y = sy ? (sy.value || '5000') : '5000';
      cli('blestream interval ' + s + ' ' + y).then(function(o){ setText('bt-stream-status', o); });
    });

    // Config
    bind('btn-bt-loadconfig', function(){ loadConfig(); });
    bind('btn-bt-savename', function(){
      var inp = el('bt-cfg-name'); if(!inp) return;
      cli('blename ' + inp.value.trim()).then(function(o){ setText('bt-cfg-status', o); });
    });
    bind('btn-bt-savetx', function(){
      var inp = el('bt-cfg-txpower'); if(!inp) return;
      cli('bletxpower ' + inp.value).then(function(o){ setText('bt-cfg-status', o); });
    });
    bind('btn-bt-autostart', function(){
      autoStartState = !autoStartState;
      updateToggle('btn-bt-autostart', autoStartState);
      cli('bleautostart ' + (autoStartState ? 'on' : 'off')).then(function(o){ setText('bt-cfg-status', o); });
    });
    bind('btn-bt-requireauth', function(){
      requireAuthState = !requireAuthState;
      updateToggle('btn-bt-requireauth', requireAuthState);
      cli('blerequireauth ' + (requireAuthState ? 'on' : 'off')).then(function(o){ setText('bt-cfg-status', o); });
    });

    // Initial load — both BLE runtime status and the saved config so toggles
    // show their real state (On/Off) instead of the placeholder "--".
    refresh();
    loadConfig();
  });
})();
</script>
)JS", HTTPD_RESP_USE_STRLEN);
}

#endif
