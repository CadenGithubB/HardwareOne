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
  // Bluetooth not compiled - show disabled message
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
  // Basic CSS and layout
  httpd_resp_send_chunk(req, R"CSS(
<style>
.bt-container { max-width: 960px; margin: 0 auto; padding: 20px; }
.bt-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 20px; }
.bt-card { background: var(--panel-bg); border-radius: 15px; padding: 20px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); border: 1px solid var(--border); overflow: hidden; }
.bt-title { font-size: 1.3em; font-weight: bold; margin-bottom: 10px; color: var(--panel-fg); display: flex; align-items: center; gap: 10px; }
.bt-description { color: var(--muted); margin-bottom: 15px; font-size: 0.9em; }
.bt-controls { display: flex; flex-wrap: wrap; gap: 10px; margin-bottom: 15px; }
.bt-status { background: var(--crumb-bg); border-radius: 8px; padding: 15px; font-family: 'Courier New', monospace; font-size: 0.9em; border-left: 4px solid var(--link); min-height: 60px; color: var(--panel-fg); white-space: pre-wrap; }
.status-indicator { display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }
.status-enabled { background: #28a745; animation: pulse 2s infinite; }
.status-disabled { background: #dc3545; }
@keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.5; } 100% { opacity: 1; } }
.bt-meta { color: var(--muted); font-size: 0.85em; margin-top: 8px; }
.bt-warning { background:var(--warning-bg); border:1px solid var(--warning-border); border-left:4px solid var(--warning-accent); color:var(--warning-fg); border-radius:6px; padding:10px 12px; font-size:0.85em; margin-top:10px; }
</style>
)CSS", HTTPD_RESP_USE_STRLEN);

  // HTML structure
  httpd_resp_send_chunk(req, R"HTML(
<div class='bt-container'>
  <div class='bt-grid'>
    <div class='bt-card'>
      <div class='bt-title'>
        <span>Bluetooth Status</span>
        <span class='status-indicator status-disabled' id='ble-status-indicator'></span>
      </div>
      <div class='bt-description'>
        Control the built-in BLE peripheral and inspect current connection state.
        This UI is a thin wrapper around the existing <code>openble</code>, <code>closeble</code>, <code>blestatus</code>, <code>bledisconnect</code> and <code>bleadv</code> CLI commands.
      </div>
      <div class='bt-controls'>
        <button class='btn' id='btn-ble-start'>Open Bluetooth</button>
        <button class='btn' id='btn-ble-stop'>Close Bluetooth</button>
        <button class='btn' id='btn-ble-adv'>Start Advertising</button>
        <button class='btn' id='btn-ble-disconnect'>Disconnect Client</button>
        <button class='btn' id='btn-ble-refresh'>Refresh Status</button>
      </div>
      <div class='bt-status' id='ble-status-box'>Click "Refresh Status" to query Bluetooth state...</div>
      <div class='bt-meta' id='ble-meta-line'></div>
      <div class='bt-warning'>
        <strong>Note:</strong> Bluetooth and Wi-Fi share the 2.4GHz radio. Throughput and latency can change when both are active. If you see instability, try stopping BLE or Wi-Fi temporarily.
      </div>
    </div>
  </div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  // JavaScript helpers
  httpd_resp_send_chunk(req, R"JS(
<script>
(function(){
  function postCli(cmd){
    return fetch('/api/cli',{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      credentials:'same-origin',
      body:'cmd='+encodeURIComponent(cmd)
    }).then(function(r){return r.text();});
  }

  function setStatus(text){
    var box=document.getElementById('ble-status-box');
    if(box) box.textContent=text||'';
  }

  function setIndicator(state){
    var el=document.getElementById('ble-status-indicator');
    if(!el) return;
    if(state==='connected' || state==='advertising' || state==='on'){
      el.className='status-indicator status-enabled';
    }else{
      el.className='status-indicator status-disabled';
    }
  }

  function updateButtonVisibility(state){
    var startBtn=document.getElementById('btn-ble-start');
    var stopBtn=document.getElementById('btn-ble-stop');
    var advBtn=document.getElementById('btn-ble-adv');
    var disconnectBtn=document.getElementById('btn-ble-disconnect');
    var isEnabled=(state==='connected' || state==='advertising' || state==='on');
    if(startBtn) startBtn.style.display=isEnabled?'none':'inline-block';
    if(stopBtn) stopBtn.style.display=isEnabled?'inline-block':'none';
    if(advBtn) advBtn.style.display=isEnabled?'inline-block':'none';
    if(disconnectBtn) disconnectBtn.style.display=isEnabled?'inline-block':'none';
  }

  function parseStatusLine(line){
    var lower = (line||'').toLowerCase();
    if(lower.indexOf('connected')>=0) return 'connected';
    if(lower.indexOf('advertising')>=0) return 'advertising';
    if(lower.indexOf('disabled')>=0 || lower.indexOf('stopped')>=0 || lower.indexOf('not initialized')>=0) return 'off';
    if(lower === 'ok' || lower.indexOf('ble status:')>=0) return 'on';
    return 'unknown';
  }

  function refreshStatus(){
    setStatus('Querying status...');
    postCli('blestatus').then(function(out){
      setStatus(out||'No status');
      var lines=(out||'').split('\n');
      var first=lines[0]||'';
      var state=parseStatusLine(first);
      setIndicator(state);
      updateButtonVisibility(state);
      var meta=document.getElementById('ble-meta-line');
      if(meta){ meta.textContent='Last updated: '+(new Date()).toLocaleTimeString(); }
    }).catch(function(e){
      setStatus('Error: '+e.message);
      setIndicator('off');
      updateButtonVisibility('off');
    });
  }

  function runCommand(cmd, successMsg){
    setStatus('Running '+cmd+'...');
    postCli(cmd).then(function(out){
      setStatus(out||successMsg||('Command '+cmd+' completed')); 
      // After any mutating command, refresh status shortly after
      setTimeout(refreshStatus, 300);
    }).catch(function(e){
      setStatus('Error: '+e.message);
    });
  }

  function bind(id, handler){
    var el=document.getElementById(id);
    if(el) el.addEventListener('click', handler);
  }

  document.addEventListener('DOMContentLoaded', function(){
    bind('btn-ble-refresh', function(){ refreshStatus(); });
    bind('btn-ble-start', function(){ runCommand('openble', 'Bluetooth opened'); });
    bind('btn-ble-stop', function(){ runCommand('closeble', 'Bluetooth closed'); });
    bind('btn-ble-adv', function(){ runCommand('bleadv', 'Advertising started'); });
    bind('btn-ble-disconnect', function(){ runCommand('bledisconnect', 'Disconnect requested'); });
    // Initial status fetch
    setTimeout(refreshStatus, 200);
  });
})();
</script>
)JS", HTTPD_RESP_USE_STRLEN);
}

#endif
