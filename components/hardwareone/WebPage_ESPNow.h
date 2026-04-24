#ifndef WEBPAGE_ESPNOW_H
#define WEBPAGE_ESPNOW_H

#include <Arduino.h>
#include "System_BuildConfig.h"
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif
#include "WebServer_Utils.h"

// Forward declarations for ESP-NOW web handlers
#if ENABLE_HTTP_SERVER
esp_err_t handleEspNowMetadata(httpd_req_t* req);
#endif

// Streamed inner content for ESP-NOW page
inline void streamEspNowInner(httpd_req_t* req) {
  // CSS
  httpd_resp_send_chunk(req, R"CSS(
<style>
.en-header{background:var(--panel-bg);border:1px solid var(--border);border-radius:10px;margin-bottom:16px;padding:10px 16px}
.en-header-row{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:8px}
.en-header-left{display:flex;align-items:center;gap:10px}
.en-header-title{font-size:1.15em;font-weight:700;color:var(--panel-fg)}
.en-header-right{display:flex;gap:6px}
.en-header-status{font-family:'Courier New',monospace;font-size:.85em;color:var(--muted);line-height:1.35;margin-top:8px;padding-top:8px;border-top:1px solid var(--border);white-space:pre-line;display:none}
.en-not-init{display:flex;flex-direction:column;align-items:center;justify-content:center;padding:48px 20px;text-align:center;background:var(--panel-bg);border:1px solid var(--border);border-radius:10px;margin-bottom:16px}
.en-not-init h3{color:var(--panel-fg);margin-bottom:6px;font-size:1.1em}
.en-not-init p{color:var(--muted);margin-bottom:20px;max-width:380px;font-size:.9em;line-height:1.5}
.en-pane-content{margin-top:12px}
.en-form-row{display:flex;gap:8px;align-items:center;margin-bottom:10px;flex-wrap:wrap}
.en-form-row input,.en-form-row select{flex:1;min-width:140px}
.en-form-row .btn{flex-shrink:0}
.en-pair-inputs{display:flex;gap:8px;margin-bottom:12px;padding:10px;background:var(--crumb-bg);border-radius:8px;flex-wrap:wrap}
.en-pair-inputs input{flex:1;min-width:140px}
.device-list{margin-bottom:16px}
.device-item{display:flex;justify-content:space-between;align-items:center;padding:10px 12px;border-bottom:1px solid var(--border);transition:background .12s}
.device-item:hover{background:var(--crumb-bg)}
.device-item:last-child{border-bottom:none}
.device-mac{font-family:'Courier New',monospace;font-weight:bold;color:var(--link)}
.device-channel{color:var(--muted);font-size:.85em}
.device-actions{display:flex;gap:5px}
.device-encrypted{color:var(--accent);font-weight:bold}
.device-unencrypted{color:var(--muted)}
.encryption-indicator{display:inline-block;width:8px;height:8px;border-radius:50%;margin-left:8px}
.encryption-enabled{background:var(--accent)}
.encryption-disabled{background:var(--muted)}
.en-interact{margin-top:12px;background:var(--crumb-bg);border-radius:8px;border:1px solid var(--border);overflow:hidden}
.interact-tabs{display:flex;flex-direction:column;gap:6px}
.interact-tab{width:100%;text-align:left;padding:8px 12px;background:var(--panel-bg);color:var(--panel-fg);border:1px solid var(--border);border-radius:6px;transition:background .15s,border-color .15s,color .15s}
.interact-tab:hover{background:var(--hover-bg)}
.interact-tab-active{background:var(--accent);color:var(--panel-bg);border-color:var(--accent)}
.message-action-btn{background:var(--panel-bg);color:var(--panel-fg);border:1px solid var(--border)}
.message-action-btn:hover{background:var(--hover-bg)}
.file-mode-toggle{background:var(--panel-bg);color:var(--panel-fg);border:1px solid var(--border);transition:background .15s,border-color .15s,color .15s}
.file-mode-toggle.file-mode-active{background:var(--accent);color:var(--panel-bg);border-color:var(--accent)}
.remote-explorer{border:1px solid var(--border);border-radius:8px;background:var(--panel-bg);color:var(--panel-fg);overflow:hidden}
.remote-explorer-crumb{padding:8px;background:var(--crumb-bg);border-bottom:1px solid var(--border);font-size:0.85em;display:flex;flex-wrap:wrap;gap:6px;align-items:center}
.remote-explorer-crumb span{cursor:pointer}
.remote-explorer-body{max-height:260px;overflow-y:auto}
.remote-entry{display:flex;align-items:center;gap:10px;padding:8px 12px;border-bottom:1px solid var(--border);cursor:pointer;transition:background .12s}
.remote-entry:hover{background:var(--hover-bg)}
.remote-entry-label{font-weight:500;color:var(--panel-fg)}
.remote-entry-meta{margin-left:auto;font-size:0.78em;color:var(--muted)}
.remote-entry-empty{justify-content:center;color:var(--muted);font-style:italic}
.remote-entry-icon{font-family:'Courier New',monospace;color:var(--link)}
.sensor-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px;margin-bottom:12px}
.sensor-pill{display:flex;justify-content:center;align-items:center;padding:10px;border-radius:8px;border:1px solid var(--border);background:var(--panel-bg);color:var(--panel-fg);font-weight:500;cursor:pointer;transition:background .15s,border-color .15s,color .15s}
.sensor-pill:hover{background:var(--hover-bg)}
.sensor-pill.sensor-active{background:var(--accent);color:var(--panel-bg);border-color:var(--accent)}
.sensor-pill.sensor-pending{border-style:dashed;box-shadow:0 0 0 1px var(--accent) inset}
.en-interact-header{padding:12px 14px;border-bottom:1px solid var(--border);background:var(--panel-bg)}
.en-interact-title{font-weight:600;color:var(--panel-fg);font-size:.95em}
.en-interact-sub{font-size:.78em;color:var(--muted);margin-top:2px}
.en-interact-body{padding:14px;min-height:420px}
.message-log{background:var(--panel-bg);border-radius:8px;padding:12px;max-height:300px;overflow-y:auto;border:1px solid var(--border);display:flex;flex-direction:column;gap:8px}
.message-bubble{max-width:75%;width:fit-content;padding:10px 14px;border-radius:16px;word-wrap:break-word;overflow-wrap:break-word;min-width:0;animation:slideIn .2s ease-out;background:var(--panel-bg);border:1px solid var(--border)}
.message-received{align-self:flex-start;background:var(--crumb-bg);color:var(--panel-fg);border-bottom-left-radius:4px}
.message-sent{align-self:flex-end;background:var(--accent);color:var(--panel-bg);border-bottom-right-radius:4px;border-color:var(--accent)}
.message-error{align-self:flex-end;background:var(--danger);color:#fff;border-bottom-right-radius:4px;border-color:var(--danger)}
.message-text{margin:0;font-size:.9em;line-height:1.4;overflow-wrap:break-word;word-break:break-word}
.message-status{font-size:.72em;margin-top:4px;opacity:.7;display:flex;align-items:center;gap:4px}
.message-empty{text-align:center;color:var(--muted);padding:20px;font-style:italic}
.input-group{display:flex;gap:8px;margin-bottom:10px;flex-wrap:wrap;width:100%}
.input-group input{flex:1 1 200px;max-width:100%;min-width:0;box-sizing:border-box}
.mac-input{font-family:'Courier New',monospace}
.mesh-warning{display:none;background:var(--warning-bg);border:1px solid var(--warning-border);color:var(--warning-fg);padding:10px;border-radius:8px;margin-bottom:12px;font-size:.88em}
.en-data{background:var(--crumb-bg);border-radius:8px;padding:12px;font-family:'Courier New',monospace;font-size:.85em;color:var(--panel-fg);border:1px solid var(--border)}
.btn-small{padding:4px 8px;font-size:.8em}
.setup-modal{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.7);z-index:10000;align-items:center;justify-content:center}
.setup-modal.show{display:flex}
.setup-modal-content{background:var(--panel-bg);border-radius:12px;padding:24px;max-width:440px;width:90%;box-shadow:0 10px 40px rgba(0,0,0,.3);border:1px solid var(--border)}
.setup-modal-title{font-size:1.2em;font-weight:bold;margin-bottom:10px;color:var(--panel-fg)}
.setup-modal-description{color:var(--muted);margin-bottom:14px;line-height:1.5;font-size:.88em}
.setup-modal-input{width:100%;padding:10px;border:1px solid var(--border);border-radius:8px;font-size:1em;margin-bottom:12px;box-sizing:border-box;background:var(--crumb-bg);color:var(--panel-fg)}
.setup-modal-input:focus{outline:none;border-color:var(--accent)}
.setup-modal-buttons{display:flex;gap:8px;justify-content:flex-end}
.setup-modal-error{color:var(--danger);margin-bottom:10px;padding:8px;background:var(--crumb-bg);border-radius:5px;display:none;font-size:.88em}
.setup-modal-requirements{background:var(--crumb-bg);padding:10px;border-radius:8px;margin-bottom:12px;font-size:.85em;color:var(--muted)}
.setup-modal-requirements ul{margin:6px 0 0 20px;padding:0}
.mesh-pane-header{display:flex;gap:8px;margin-bottom:12px;flex-wrap:wrap}
.mesh-view-tabs{display:flex;gap:6px;margin-bottom:12px}
.mesh-view-tabs .btn{flex:1}
@media(max-width:768px){
.en-pair-inputs{flex-direction:column}
.en-pair-inputs input{width:100%}
}
</style>
)CSS", HTTPD_RESP_USE_STRLEN);
  
  // HTML structure
  httpd_resp_send_chunk(req, R"HTML(
<div class='en-header'>
<div class='en-header-row'>
<div class='en-header-left'>
<span class='status-indicator status-disabled' id='espnow-status-indicator'></span>
<span class='en-header-title'>ESP-NOW</span>
</div>
<div class='en-header-right'>
<button class='btn' id='btn-espnow-toggle-mode' style='display:none'>Mode: Direct</button>
<button class='btn' id='btn-espnow-init' style='display:none'>Initialize</button>
<button class='btn' id='btn-espnow-disable' style='display:none'>Disable</button>
<button class='btn' id='btn-espnow-refresh'>Refresh</button>
</div>
</div>
<div class='en-header-status' id='espnow-status-data'>Loading...</div>
</div>
<div class='en-not-init' id='en-not-init'>
<h3>ESP-NOW Not Initialized</h3>
<p>Initialize ESP-NOW to enable direct device-to-device wireless communication, mesh networking, and peer management.</p>
<div style='font-size:.78em;color:var(--muted);max-width:340px'>Click Initialize in the top right to enable ESP-NOW.</div>
</div>
<div id='en-panels' style='display:none'>
<div class='settings-panel' id='device-management-card'>
<div style='display:flex;align-items:center;justify-content:space-between'>
<div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Devices</div><div style='color:var(--panel-fg);font-size:0.9rem'>Paired devices, mesh peers, and network topology.</div></div>
<button class='btn' id='btn-devices-toggle' onclick="togglePane('devices-pane','btn-devices-toggle')">Expand</button>
</div>
<div id='devices-pane' style='display:none' class='en-pane-content'>
<div style='display:flex;justify-content:flex-end;gap:8px;margin-bottom:8px'>
<button class='btn' onclick="openBroadcastPanel()">Broadcast</button>
<button class='btn' id='btn-add-device-toggle' onclick="(function(){var p=document.getElementById('add-device-pane');var b=document.getElementById('btn-add-device-toggle');var show=p.style.display==='none'||!p.style.display;p.style.display=show?'block':'none';b.textContent=show?'Cancel':'+ Add Device';})()" >+ Add Device</button>
</div>
<div id='add-device-pane' style='display:none'>
<div class='en-pair-inputs'>
<input type='text' id='pair-mac' class='mac-input' placeholder='XX:XX:XX:XX:XX:XX' maxlength='17'>
<input type='text' id='pair-name' placeholder='Device Name'>
<button class='btn' id='btn-pair-device'>Pair</button>
<button class='btn' id='btn-pair-secure'>Pair Encrypted</button>
</div>
</div>
<div class='device-list' id='device-list'>
<div style='color:var(--muted);text-align:center;padding:20px'>No devices paired yet</div>
</div>
<div class='en-interact' id='device-panel-card' style='display:none'>
<div class='en-interact-header'>
<div class='en-interact-title' id='device-panel-title'>Device Panel</div>
<div class='en-interact-sub' id='device-panel-subtitle'>Select a device to interact</div>
</div>
<div class='en-interact-body panel' id='device-panel-content'></div>
</div>
<div id='mesh-views-card' style='display:none;margin-top:16px;padding-top:16px;border-top:1px solid var(--border)'>
<div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:10px;flex-wrap:wrap;gap:8px'>
<div class='mesh-view-tabs' style='margin-bottom:0'>
<button class='btn' id='btn-view-topology'>Topology</button>
<button class='btn' id='btn-view-graph'>Graph</button>
</div>
<div style='display:flex;gap:6px'>
<button class='btn' id='btn-refresh-mesh'>Refresh</button>
<button class='btn' id='btn-auto-topology'>Auto-Discover: OFF</button>
</div>
</div>
<div id='mesh-view-topology' style='display:none'>
<div class='mesh-peers' id='mesh-topology-view'>
<div style='color:var(--muted);text-align:center;padding:16px'>Click Refresh to load topology</div>
</div>
</div>
<div id='mesh-view-graph' style='display:none'>
<div class='mesh-peers' id='mesh-graph-view'>
<div style='color:var(--muted);text-align:center;padding:16px'>Network graph</div>
</div>
</div>
</div>
</div>
</div>
<div class='settings-panel' id='en-settings-card'>
<div style='display:flex;align-items:center;justify-content:space-between'>
<div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Settings</div><div style='color:var(--panel-fg);font-size:0.9rem'>Device identity, encryption, and mesh role configuration.</div></div>
<button class='btn' id='btn-settings-toggle' onclick="togglePane('settings-pane','btn-settings-toggle')">Expand</button>
</div>
<div id='settings-pane' style='display:none' class='en-pane-content'>
<div id='smarthome-card' style='margin-bottom:16px;padding-bottom:16px;border-bottom:1px solid var(--border)'>
<div style='font-size:1rem;font-weight:600;color:var(--panel-fg);margin-bottom:8px'>Smart Home Metadata</div>
<div style='color:var(--muted);font-size:.82em;margin-bottom:10px'>Device identity for home automation and mesh discovery.</div>
<div class='en-form-row'>
<input type='text' id='friendly-name' placeholder='Friendly Name (e.g., Living Room Light)' maxlength='47'>
<button class='btn' id='btn-set-friendly'>Set Name</button>
</div>
<div class='en-form-row'>
<input type='text' id='room-name' placeholder='Room (e.g., Living Room)' maxlength='31'>
<button class='btn' id='btn-set-room'>Set Room</button>
</div>
<div class='en-form-row'>
<input type='text' id='zone-name' placeholder='Zone (e.g., Upstairs)' maxlength='31'>
<button class='btn' id='btn-set-zone'>Set Zone</button>
</div>
<div class='en-form-row'>
<input type='text' id='tags-input' placeholder='Tags (comma-separated)' maxlength='63'>
<button class='btn' id='btn-set-tags'>Set Tags</button>
</div>
<label style='display:flex;align-items:center;gap:8px;cursor:pointer;margin-top:4px'>
<input type='checkbox' id='stationary-checkbox' style='width:auto;margin:0'>
<span style='color:var(--panel-fg);font-size:.9em'>Stationary Device</span>
</label>
</div>
<div id='encryption-card' style='margin-bottom:16px'>
<div style='font-size:1rem;font-weight:600;color:var(--panel-fg);margin-bottom:8px'>Encryption</div>
<div style='color:var(--muted);font-size:.82em;margin-bottom:10px'>All paired devices must share the same passphrase.</div>
<div class='en-form-row'>
<input type='password' id='encryption-passphrase' placeholder='Encryption passphrase' maxlength='64'>
<button class='btn' id='btn-set-passphrase'>Set</button>
<button class='btn' id='btn-clear-passphrase' style='display:none'>Clear</button>
</div>
<div style='background:var(--crumb-bg);border-radius:8px;padding:10px;font-size:.85em;color:var(--panel-fg);border:1px solid var(--border);margin-top:8px' id='encryption-status'>No encryption passphrase set</div>
</div>
<div id='mesh-role-card' style='display:none;padding-top:16px;border-top:1px solid var(--border)'>
<div style='font-size:1rem;font-weight:600;color:var(--panel-fg);margin-bottom:8px'>Mesh Role Configuration</div>
<div style='background:var(--crumb-bg);border-radius:8px;padding:10px;font-size:.85em;color:var(--panel-fg);border:1px solid var(--border);margin-bottom:12px' id='mesh-role-status'>Loading role configuration...</div>
<div style='display:flex;gap:8px;margin-bottom:12px;flex-wrap:wrap'>
<button class='btn' id='btn-role-worker'>Worker</button>
<button class='btn' id='btn-role-master'>Master</button>
<button class='btn' id='btn-role-backup'>Backup</button>
<button class='btn' id='btn-mesh-topo'>Discover Topology</button>
</div>
<div class='en-form-row'>
<input type='text' id='master-mac' class='mac-input' placeholder='Master MAC (XX:XX:XX:XX:XX:XX)' maxlength='17'>
<button class='btn' id='btn-set-master-mac'>Set Master</button>
</div>
<label style='display:flex;align-items:center;gap:8px;cursor:pointer;margin-top:8px'>
<input type='checkbox' id='backup-master-enabled' style='width:auto;margin:0'>
<span style='color:var(--panel-fg);font-size:.9em'>Enable Backup Master</span>
</label>
<div id='backup-mac-group' class='en-form-row' style='display:none;margin-top:8px'>
<input type='text' id='backup-mac' class='mac-input' placeholder='Backup MAC (XX:XX:XX:XX:XX:XX)' maxlength='17'>
<button class='btn' id='btn-set-backup-mac'>Set Backup</button>
</div>
<div style='background:var(--crumb-bg);border-radius:8px;padding:10px;font-size:.85em;color:var(--panel-fg);border:1px solid var(--border);margin-top:12px;display:none' id='mesh-topology-data'>
<div style='font-weight:bold;margin-bottom:8px'>Topology Discovery Results:</div>
<div id='topology-results'>No topology data yet</div>
</div>
</div>
</div>
</div>
</div>
</div>
<div class='setup-modal' id='setup-modal'>
<div class='setup-modal-content'>
<div class='setup-modal-title'>ESP-NOW First-Time Setup</div>
<div class='setup-modal-description'>Set a unique name for this device. This identifies your device in topology displays and mesh networks.</div>
<div class='setup-modal-requirements'>
<strong>Requirements:</strong>
<ul>
<li>1-20 characters</li>
<li>Letters, numbers, hyphens, underscores only</li>
<li>No spaces or special characters</li>
</ul>
</div>
<div class='setup-modal-error' id='setup-error'></div>
<input type='text' id='setup-device-name' class='setup-modal-input' placeholder='Enter device name (e.g., darkblue)' maxlength='20' autocomplete='off'>
<div class='setup-modal-buttons'>
<button class='btn' id='btn-setup-cancel'>Cancel</button>
<button class='btn' id='btn-setup-save'>Set Name & Initialize</button>
</div>
</div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);
  
  // Inject compile-time feature flags as JS variables
#if ENABLE_AUTOMATION
  httpd_resp_send_chunk(req, "<script>window.__automationEnabled=true;</script>", HTTPD_RESP_USE_STRLEN);
#else
  httpd_resp_send_chunk(req, "<script>window.__automationEnabled=false;</script>", HTTPD_RESP_USE_STRLEN);
#endif

  // JavaScript (complete ESP-NOW logic)
  httpd_resp_send_chunk(req, R"JS(
<script>
window.togglePane = function(paneId, btnId) {
  var p = document.getElementById(paneId);
  var b = document.getElementById(btnId);
  if (!p || !b) { console.warn('[togglePane] Element not found:', paneId, btnId); return; }
  var isHidden = (p.style.display === 'none' || !p.style.display);
  p.style.display = isHidden ? 'block' : 'none';
  b.textContent = isHidden ? 'Collapse' : 'Expand';
};
</script>
<script>console.log('[ESP-NOW] Section 1: Pre-script sentinel');</script>
<script>
(function() {
  try {
    console.log('[ESP-NOW] Chunk 1: Global variables start');
    window.messageCount = 0;
    window.maxMessages = 50;
    window.__espnowDeviceNameToMac = {};  // Map device names to MAC addresses
    window.automationsInnerHtml = function(mac) {
      if (window.__automationEnabled) {
        return '<div class="input-group" style="margin-bottom:8px">'
          + '<input type="text" id="au-' + mac + '" placeholder="Username" style="flex:1">'
          + '<input type="password" id="ap-' + mac + '" placeholder="Password" style="flex:1">'
          + '</div>'
          + '<button class="btn auto-load-btn" id="btn-load-autos-' + mac + '" onclick="loadRemoteAutomations(\'' + mac + '\')">Load Automations</button>'
          + '<div id="automations-list-' + mac + '" style="margin-top:12px;min-height:40px"></div>';
      } else {
        return '<div style="text-align:center;color:var(--muted);padding:20px;font-size:0.9em">'
          + 'Automations not compiled in this build.<br>'
          + '<span style="font-size:0.85em">Enable ENABLE_AUTOMATION in System_BuildConfig.h and recompile.</span>'
          + '</div>';
      }
    };
    window.__autoFetchState = window.__autoFetchState || {};
    window.setAutoButtonState = function(mac, opts) {
      var btn = document.getElementById('btn-load-autos-' + mac);
      if (!btn) return;
      if (!btn.dataset.defaultLabel) btn.dataset.defaultLabel = btn.textContent || 'Load Automations';
      if (opts && opts.text) {
        btn.textContent = opts.text;
      } else {
        btn.textContent = btn.dataset.defaultLabel;
      }
      btn.disabled = !!(opts && opts.disabled);
    };
    window.markAutomationsFetchIdle = function(mac, nextLabel) {
      if (window.__autoFetchState) delete window.__autoFetchState[mac];
      window.setAutoButtonState(mac, { disabled: false, text: nextLabel || 'Refresh Automations' });
    };
    console.log('[ESP-NOW] Chunk 1: Global variables ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 1 error:', e); }
})();
</script>
<script>
(function() {
  try {
    console.log('[ESP-NOW] Chunk 2: Status functions start');
    window.refreshStatus = function() {
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnowstatus')
      })
      .then(response => response.text())
      .then(output => {
        console.log('[ESP-NOW] Status response:', output);
        console.log('[ESP-NOW] Response length:', output.length);
        const indicator = document.getElementById('espnow-status-indicator');
        // Check for initialization - be flexible with whitespace and case
        const isInitialized = output.match(/Initialized:\s*Yes/i) !== null;
        console.log('[ESP-NOW] Checking for initialization...');
        console.log('[ESP-NOW] isInitialized:', isInitialized);
        const chMatch = output.match(/Channel:\s*(\d+)/);
        const channel = chMatch ? chMatch[1] : '?';
        console.log('[ESP-NOW] Channel:', channel);
        /* Extract MAC without regex to avoid literal issues */
        const macLabel = 'MAC Address:';
        let mac = null;
        const macIdx = output.indexOf(macLabel);
        if (macIdx >= 0) {
          let rest = output.substring(macIdx + macLabel.length);
          rest = rest.trimStart();
          const nl = rest.indexOf("\n");
          const cr = rest.indexOf("\r");
          let end = rest.length;
          if (nl >= 0 && cr >= 0) end = Math.min(nl, cr);
          else if (nl >= 0) end = nl;
          else if (cr >= 0) end = cr;
          mac = rest.substring(0, end).trim();
        }
        // Display full status output instead of just friendly summary
        document.getElementById('espnow-status-data').textContent = output;
        if (isInitialized) {
          indicator.className = 'status-indicator status-enabled';
          document.getElementById('btn-espnow-init').style.display = 'none';
          document.getElementById('btn-espnow-disable').style.display = '';
          document.getElementById('btn-espnow-toggle-mode').style.display = '';
          document.getElementById('en-not-init').style.display = 'none';
          document.getElementById('en-panels').style.display = 'block';
          document.getElementById('espnow-status-data').style.display = 'block';
          /* Load device list */
          try { if (typeof listDevices === 'function') { listDevices(); } } catch(e) { console.warn('[ESP-NOW] listDevices not defined yet'); }
          /* Check encryption status now that ESP-NOW is initialized */
          try { if (typeof window.checkEncryptionStatus === 'function') { window.checkEncryptionStatus(); } } catch(e) { console.warn('[ESP-NOW] checkEncryptionStatus call error:', e); }
          /* Load smart home metadata */
          try { if (typeof window.loadSmartHomeMetadata === 'function') { window.loadSmartHomeMetadata(); } } catch(e) { console.warn('[ESP-NOW] loadSmartHomeMetadata call error:', e); }
          /* Start message polling now that ESP-NOW is initialized */
          if (typeof window.espnowStartPolling === 'function') { window.espnowStartPolling(); }
        } else {
          indicator.className = 'status-indicator status-disabled';
          document.getElementById('btn-espnow-init').style.display = '';
          document.getElementById('btn-espnow-disable').style.display = 'none';
          document.getElementById('btn-espnow-toggle-mode').style.display = 'none';
          document.getElementById('en-not-init').style.display = 'flex';
          document.getElementById('en-panels').style.display = 'none';
          document.getElementById('espnow-status-data').style.display = 'none';
          if (typeof window.espnowStopPolling === 'function') { window.espnowStopPolling(); }
        }
      })
      .then(() => {
        return fetch('/api/cli', { method: 'POST', headers:{ 'Content-Type':'application/x-www-form-urlencoded' }, body: 'cmd=' + encodeURIComponent('espnowmode') });
      })
      .then(r => r.text())
      .then(modeOut => {
        try {
          console.log('[ESP-NOW] Mode response:', modeOut);
          var btn = document.getElementById('btn-espnow-toggle-mode');
          if (!btn) return;
          var m = (modeOut || '').toLowerCase();
          var isMesh = m.indexOf('mesh') >= 0;
          console.log('[ESP-NOW] Detected mode:', isMesh ? 'MESH' : 'DIRECT');
          btn.textContent = 'Mode: ' + (isMesh ? 'Mesh' : 'Direct');
          var warn = document.getElementById('mesh-warning');
          if (warn) { warn.style.display = isMesh ? 'block' : 'none'; }
          
          // Show/hide mesh panels based on mode + init state
          var indicator = document.getElementById('espnow-status-indicator');
          var isInitialized = indicator && indicator.className.indexOf('status-enabled') >= 0;
          var meshViewsCard = document.getElementById('mesh-views-card');
          var meshRoleCard = document.getElementById('mesh-role-card');
          if (meshViewsCard) meshViewsCard.style.display = (isMesh && isInitialized) ? 'block' : 'none';
          if (meshRoleCard) meshRoleCard.style.display = (isMesh && isInitialized) ? 'block' : 'none';
          
          if (isMesh && isInitialized && typeof window.refreshMeshRole === 'function') {
            window.refreshMeshRole();
          }
          
          window.espnowIsMesh = !!isMesh;
          console.log('[ESP-NOW] window.espnowIsMesh set to:', window.espnowIsMesh);
          
          // Only start mesh polling if initialized
          if (isMesh && isInitialized) {
            if (typeof window.refreshMeshStatus === 'function') {
              window.refreshMeshStatus();
            }
            if (typeof window.startMeshStatusPolling === 'function') {
              window.startMeshStatusPolling();
            }
          } else {
            if (typeof window.stopMeshStatusPolling === 'function') {
              window.stopMeshStatusPolling();
            }
          }
        } catch(e) {
          console.error('[ESP-NOW] Error setting mode:', e);
        }
      })
      .catch(error => {
        document.getElementById('espnow-status-data').textContent = 'Error: ' + error;
      });
    };

    // Batch version: loads all 8 ESP-NOW CLI commands in a single HTTPS request on page load.
    // Falls back to individual refreshStatus() if the batch endpoint is unavailable.
    window.refreshStatusBatch = function() {
      var CMDS = [
        'espnowstatus',     // 0
        'espnowmode',      // 1
        'bondstatus',      // 2
        'espnowlist',      // 3
        'espnowencstatus', // 4
        'espnowdeviceinfo',// 5
        'espnowmeshrole',  // 6
        'espnowmeshstatus' // 7
      ];
      fetch('/api/cli/batch', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ commands: CMDS })
      })
      .then(r => r.json())
      .then(data => {
        if (!data || !Array.isArray(data.results) || data.results.length < 2) {
          console.warn('[ESP-NOW] Batch response invalid, falling back to individual requests');
          refreshStatus();
          return;
        }
        var results = data.results;
        var output  = results[0] || '';
        var modeOut = results[1] || '';

        // --- apply espnowstatus (same logic as refreshStatus) ---
        console.log('[ESP-NOW] Batch status:', output);
        const indicator = document.getElementById('espnow-status-indicator');
        const isInitialized = output.match(/Initialized:\s*Yes/i) !== null;
        document.getElementById('espnow-status-data').textContent = output;
        if (isInitialized) {
          indicator.className = 'status-indicator status-enabled';
          document.getElementById('btn-espnow-init').style.display = 'none';
          document.getElementById('btn-espnow-disable').style.display = '';
          document.getElementById('btn-espnow-toggle-mode').style.display = '';
          document.getElementById('en-not-init').style.display = 'none';
          document.getElementById('en-panels').style.display = 'block';
          document.getElementById('espnow-status-data').style.display = 'block';
          if (typeof window.espnowStartPolling === 'function') window.espnowStartPolling();
        } else {
          indicator.className = 'status-indicator status-disabled';
          document.getElementById('btn-espnow-init').style.display = '';
          document.getElementById('btn-espnow-disable').style.display = 'none';
          document.getElementById('btn-espnow-toggle-mode').style.display = 'none';
          document.getElementById('en-not-init').style.display = 'flex';
          document.getElementById('en-panels').style.display = 'none';
          document.getElementById('espnow-status-data').style.display = 'none';
          if (typeof window.espnowStopPolling === 'function') window.espnowStopPolling();
        }

        // --- apply espnow mode ---
        try {
          var m = (modeOut || '').toLowerCase();
          var isMesh = m.indexOf('mesh') >= 0;
          var btn = document.getElementById('btn-espnow-toggle-mode');
          if (btn) btn.textContent = 'Mode: ' + (isMesh ? 'Mesh' : 'Direct');
          var warn = document.getElementById('mesh-warning');
          if (warn) warn.style.display = isMesh ? 'block' : 'none';
          var meshViewsCard = document.getElementById('mesh-views-card');
          var meshRoleCard = document.getElementById('mesh-role-card');
          if (meshViewsCard) meshViewsCard.style.display = (isMesh && isInitialized) ? 'block' : 'none';
          if (meshRoleCard) meshRoleCard.style.display = (isMesh && isInitialized) ? 'block' : 'none';
          window.espnowIsMesh = !!isMesh;
          if (isMesh && isInitialized) {
            if (typeof window.startMeshStatusPolling === 'function') window.startMeshStatusPolling();
          } else {
            if (typeof window.stopMeshStatusPolling === 'function') window.stopMeshStatusPolling();
          }
        } catch(e) { console.error('[ESP-NOW] Batch mode parse error:', e); }

        // --- distribute remaining results to sub-functions ---
        if (isInitialized) {
          try { if (typeof listDevices === 'function') listDevices(results[2], results[3]); } catch(e) { console.warn('[ESP-NOW] Batch listDevices error:', e); }
          try { if (typeof window.checkEncryptionStatus === 'function') window.checkEncryptionStatus(results[4]); } catch(e) { console.warn('[ESP-NOW] Batch encstatus error:', e); }
          try { if (typeof window.loadSmartHomeMetadata === 'function') window.loadSmartHomeMetadata(results[5]); } catch(e) { console.warn('[ESP-NOW] Batch deviceinfo error:', e); }
          var isMeshInit = window.espnowIsMesh;
          if (isMeshInit) {
            try { if (typeof window.refreshMeshRole === 'function') window.refreshMeshRole(results[6]); } catch(e) { console.warn('[ESP-NOW] Batch meshrole error:', e); }
            try { if (typeof window.refreshMeshStatus === 'function') window.refreshMeshStatus(results[7]); } catch(e) { console.warn('[ESP-NOW] Batch meshstatus error:', e); }
          }
        }
      })
      .catch(error => {
        console.warn('[ESP-NOW] Batch fetch failed, falling back to individual requests:', error);
        refreshStatus();
      });
    };

    console.log('[ESP-NOW] Chunk 2: Status functions ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 2 error:', e); }
})();
</script>
<script>
(function() {
  try {
    console.log('[ESP-NOW] Chunk 3A: listDevices function start');
    window.listDevices = function(preloadedBondStatus, preloadedList) {
      function applyBondStatus(bondStatus) {
        window.__bondedPeerMac = null;
        const bondMatch = bondStatus.match(/Peer MAC:\s*([A-Fa-f0-9:]{17})/);
        if (bondMatch) { window.__bondedPeerMac = bondMatch[1].toUpperCase(); }
      }
      function applyList(output) {
        const deviceList = document.getElementById('device-list');
        try { console.log('[ESP-NOW][DEV] listDevices: output length', output ? output.length : -1); } catch(e){}
        window.espnowDevices = [];
        let parsed = null;
        try { parsed = JSON.parse(output); } catch(e) { parsed = null; }
        const devices = (parsed && Array.isArray(parsed.devices)) ? parsed.devices : [];
        // Store parsed device list globally for unified rendering
        window.__pairedDevices = devices;
        window.renderUnifiedDeviceList();
      }
      if (preloadedBondStatus !== undefined) {
        applyBondStatus(preloadedBondStatus);
        applyList(preloadedList !== undefined ? preloadedList : '');
        return;
      }
      // First get bond status to identify bonded device
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('bondstatus')
      })
      .then(r => r.text())
      .then(bondStatus => {
        applyBondStatus(bondStatus);
        // Now fetch device list
        return fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('espnowlist')
        });
      })
      .then(response => response.text())
      .then(applyList)
      .catch(error => {
        document.getElementById('device-list').innerHTML = '<div style="color: #dc3545;">Error loading devices: ' + error + '</div>';
      });
    };
    console.log('[ESP-NOW] Chunk 3A: listDevices function ready');
    /* Unified device list renderer: merges paired devices + mesh health into one table */
    console.log('[ESP-NOW] Chunk 3B: renderUnifiedDeviceList start');
    window.__pairedDevices = window.__pairedDevices || [];
    window.__meshPeers = window.__meshPeers || [];
    window.__meshUnpaired = window.__meshUnpaired || [];
    window.renderUnifiedDeviceList = function() {
      var deviceList = document.getElementById('device-list');
      if (!deviceList) return;
      var paired = window.__pairedDevices || [];
      var meshPeers = window.__meshPeers || [];
      var meshUnpaired = window.__meshUnpaired || [];
      var isMesh = !!window.espnowIsMesh;

      // Build MAC-keyed mesh health map
      var healthMap = {};
      for (var mi = 0; mi < meshPeers.length; mi++) {
        var mp = meshPeers[mi];
        if (mp.mac) healthMap[mp.mac.toUpperCase()] = mp;
      }

      // Build unified device entries from paired list
      window.espnowDevices = [];
      window.__espnowDeviceNameToMac = window.__espnowDeviceNameToMac || {};
      var seenMacs = {};
      var html = '';
      for (var i = 0; i < paired.length; i++) {
        var dev = paired[i];
        var mac = (dev.mac || '').toUpperCase();
        if (!mac) continue;
        seenMacs[mac] = true;
        var deviceName = dev.name || '';
        var isEncrypted = !!dev.encrypted;
        var isBonded = window.__bondedPeerMac && mac === window.__bondedPeerMac;
        if (deviceName) window.__espnowDeviceNameToMac[deviceName] = mac;
        window.espnowDevices.push({ mac: mac, name: deviceName, encrypted: isEncrypted, bonded: isBonded });

        // Mesh health for this device
        var health = healthMap[mac] || null;
        var statusDot = '';
        var statusLabel = '';
        var statsLine = '';
        if (isMesh && health) {
          var hbSec = (typeof health.secondsSinceHeartbeat === 'number') ? health.secondsSinceHeartbeat : null;
          var actSec = (typeof health.secondsSinceActivity === 'number') ? health.secondsSinceActivity : null;
          
          // Consider device online if either heartbeat OR recent activity (ACKs) within timeout
          var isOnline = health.alive || health.activityAlive;
          var isFresh = (hbSec !== null && hbSec <= 15) || (actSec !== null && actSec <= 15);
          
          if (isOnline) {
            if (isFresh) {
              statusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--success);margin-right:6px" title="Online"></span>';
              statusLabel = '<span style="color:var(--success);font-size:.8em;margin-left:6px">Online</span>';
            } else {
              statusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--warning);margin-right:6px" title="Online (Stale)"></span>';
              statusLabel = '<span style="color:var(--warning);font-size:.8em;margin-left:6px">Online</span>';
            }
          } else {
            statusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--danger);margin-right:6px" title="Offline"></span>';
            statusLabel = '<span style="color:var(--danger);font-size:.8em;margin-left:6px">Offline</span>';
          }
          statsLine = '<span style="color:var(--muted);font-size:.78em;margin-left:4px">HB: ' + (health.heartbeatCount || 0) + ' | ACKs: ' + (health.ackCount || 0) + '</span>';
        }

        var encText = isEncrypted ? 'Encrypted' : 'Unencrypted';
        var encClass = isEncrypted ? 'device-encrypted' : 'device-unencrypted';
        var encInd = isEncrypted ? 'encryption-enabled' : 'encryption-disabled';
        var bondBadge = isBonded ? '<span style="color:var(--warning);margin-right:4px;font-weight:bold" title="Bonded Device">[BOND]</span>' : '';

        html += '<div class="device-item">';
        html += '<div style="flex:1;min-width:0">';
        html += '<div class="device-mac">' + statusDot + bondBadge + '<strong>' + (deviceName || mac) + '</strong>';
        html += '<span class="encryption-indicator ' + encInd + '" title="' + encText + '"></span>';
        html += statusLabel + '</div>';
        html += '<div class="device-channel ' + encClass + '">' + mac + ' • ' + encText;
        if (isBonded) html += ' • <strong>Bonded</strong>';
        if (isMesh && health) html += ' • ' + statsLine;
        html += '</div>';
        html += '</div>';
        html += '<div class="device-actions">';
        html += '<button class="btn btn-small" onclick="toggleDevicePanel(\'' + mac + '\',\'message\')">Interact</button>';
        html += '<button class="btn btn-small" onclick="unpairDevice(\'' + mac + '\')">Unpair</button>';
        html += '</div>';
        html += '</div>';
      }

      // Mesh-only peers (in meshPeers but not in paired list)
      if (isMesh) {
        var meshOnly = [];
        for (var mi2 = 0; mi2 < meshPeers.length; mi2++) {
          var pm = meshPeers[mi2];
          if (pm.mac && !seenMacs[pm.mac.toUpperCase()]) {
            meshOnly.push(pm);
            seenMacs[pm.mac.toUpperCase()] = true;
          }
        }
        if (meshOnly.length > 0) {
          for (var j = 0; j < meshOnly.length; j++) {
            var mp2 = meshOnly[j];
            var mmac = (mp2.mac || '').toUpperCase();
            var mname = mp2.name || 'Unknown';
            var mStatusDot = '', mStatusLabel = '';
            var mhbSec = (typeof mp2.secondsSinceHeartbeat === 'number') ? mp2.secondsSinceHeartbeat : null;
            if (mp2.alive) {
              if (mhbSec !== null && mhbSec <= 15) {
                mStatusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--success);margin-right:6px" title="Online"></span>';
                mStatusLabel = '<span style="color:var(--success);font-size:.8em;margin-left:6px">Online</span>';
              } else {
                mStatusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--warning);margin-right:6px" title="Stale"></span>';
                mStatusLabel = '<span style="color:var(--warning);font-size:.8em;margin-left:6px">Stale</span>';
              }
            } else if (mp2.activityAlive) {
              mStatusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--warning);margin-right:6px" title="Stale"></span>';
              mStatusLabel = '<span style="color:var(--warning);font-size:.8em;margin-left:6px">Stale</span>';
            } else {
              mStatusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--danger);margin-right:6px" title="Offline"></span>';
              mStatusLabel = '<span style="color:var(--danger);font-size:.8em;margin-left:6px">Offline</span>';
            }
            var mStats = '<span style="color:var(--muted);font-size:.78em;margin-left:4px">HB: ' + (mp2.heartbeatCount || 0) + ' | ACKs: ' + (mp2.ackCount || 0) + '</span>';

            html += '<div class="device-item">';
            html += '<div style="flex:1;min-width:0">';
            html += '<div class="device-mac">' + mStatusDot + '<strong>' + mname + '</strong>';
            html += '<span style="background:var(--crumb-bg);color:var(--panel-fg);font-size:.72em;padding:2px 6px;border-radius:4px;margin-left:6px">Mesh</span>';
            html += mStatusLabel + '</div>';
            html += '<div class="device-channel" style="color:var(--muted)">' + mmac + ' • ' + mStats + '</div>';
            html += '</div>';
            html += '<div class="device-actions">';
            html += '<button class="btn btn-small" onclick="toggleDevicePanel(\'' + mmac + '\',\'message\')">Interact</button>';
            html += '</div>';
            html += '</div>';
          }
        }

        // Unpaired/discovered devices
        if (meshUnpaired.length > 0) {
          html += '<div style="margin-top:12px;padding-top:12px;border-top:1px solid var(--border)">';
          html += '<div style="font-size:.85em;font-weight:600;color:var(--muted);margin-bottom:8px">Discovered Devices (' + meshUnpaired.length + ')</div>';
          for (var ui = 0; ui < meshUnpaired.length; ui++) {
            var ud = meshUnpaired[ui];
            var umac = (ud.mac || '').toUpperCase();
            var uname = ud.name || 'Unknown';
            var rssiColor = ud.rssi > -60 ? 'var(--success)' : (ud.rssi > -75 ? 'var(--warning)' : 'var(--danger)');
            html += '<div class="device-item">';
            html += '<div style="flex:1;min-width:0">';
            html += '<div class="device-mac"><strong>' + uname + '</strong>';
            html += '<span style="background:var(--crumb-bg);color:var(--muted);font-size:.72em;padding:2px 6px;border-radius:4px;margin-left:6px">Discovered</span></div>';
            html += '<div class="device-channel" style="color:var(--muted)">' + umac;
            html += ' • RSSI: <span style="color:' + rssiColor + '">' + ud.rssi + ' dBm</span>';
            html += ' • Last: ' + ud.secondsSinceLastSeen + 's ago</div>';
            html += '</div>';
            html += '<div class="device-actions">';
            html += '<button class="btn btn-small" onclick="pairUnpairedDevice(\'' + umac + '\',\'' + uname.replace(/'/g, "\\'") + '\')">Pair</button>';
            html += '</div>';
            html += '</div>';
          }
          html += '</div>';
        }
      }

      if (!html) {
        deviceList.innerHTML = '<div style="color:var(--muted);text-align:center;padding:20px">No devices paired yet</div>';
      } else {
        deviceList.innerHTML = html;
      }
    };
    console.log('[ESP-NOW] Chunk 3B: renderUnifiedDeviceList ready');
    /* Per-device panel rendering and actions */
    console.log('[ESP-NOW] Chunk 3C: initializeFileBrowser start');
    window.initializeFileBrowser = function(mac) {
      if (typeof window.createFileExplorerWithInput === 'function') {
        window.createFileExplorerWithInput({
          explorerContainerId: 'fexplorer-' + mac,
          inputId: 'fp-' + mac,
          path: '/',
          height: '280px',
          mode: 'select',  // Select-only mode: no edit/delete/view actions
          selectFilesOnly: true,  // Only allow selecting files, not folders
          onSelect: function(filePath) {
            var statusDiv = document.getElementById('fstat-' + mac);
            if (statusDiv && filePath) {
              statusDiv.textContent = 'Ready to send: ' + filePath;
            }
          }
        });
      }
    };
    console.log('[ESP-NOW] Chunk 3C: initializeFileBrowser ready');
    console.log('[ESP-NOW] Chunk 3D: openBroadcastPanel start');
    window.openBroadcastPanel = function() {
      try {
        var card = document.getElementById('device-panel-card');
        var title = document.getElementById('device-panel-title');
        var subtitle = document.getElementById('device-panel-subtitle');
        var content = document.getElementById('device-panel-content');
        if (!card || !title || !content) return;
        
        var activeKey = (card.dataset.key || '');
        var nextKey = 'broadcast|broadcast';
        
        // Toggle off if already showing broadcast
        if (card.style.display !== 'none' && activeKey === nextKey) {
          card.style.display = 'none';
          return;
        }
        
        title.textContent = 'Broadcast Panel';
        subtitle.textContent = 'Send a message to all paired devices';
        content.innerHTML = renderDevicePanel('', 'broadcast');
        card.dataset.key = nextKey;
        card.style.display = 'block';
        try { card.scrollIntoView({behavior:'smooth', block:'nearest'}); } catch(_) {}
      } catch(e) { console.warn('[ESP-NOW] openBroadcastPanel error:', e); }
    };
    console.log('[ESP-NOW] Chunk 3D: openBroadcastPanel ready');
    console.log('[ESP-NOW] Chunk 3E: toggleDevicePanel start');
    window.toggleDevicePanel = function(mac, kind) {
      try {
        var card = document.getElementById('device-panel-card');
        var title = document.getElementById('device-panel-title');
        var subtitle = document.getElementById('device-panel-subtitle');
        var content = document.getElementById('device-panel-content');
        if (!card || !title || !content) return;
        var activeKey = (card.dataset.key || '');
        var nextKey = mac + '|' + kind;
        
        // Toggle off if clicking same panel
        if (card.style.display !== 'none' && activeKey === nextKey) {
          card.style.display = 'none';
          return;
        }
        
        var deviceInfo = mac;
        for (var i = 0; i < window.espnowDevices.length; i++) {
          if (window.espnowDevices[i].mac === mac) {
            deviceInfo = window.espnowDevices[i].name + ' • ' + mac;
            break;
          }
        }
        title.textContent = 'Interact — ' + deviceInfo;
        subtitle.textContent = 'Send messages, execute commands, or transfer files';
        content.innerHTML = renderDevicePanel(mac, kind);
        card.dataset.key = nextKey;
        card.style.display = 'block';
        if (kind === 'file' && typeof window.initializeFileBrowser === 'function') {
          setTimeout(function() { window.initializeFileBrowser(mac); }, 100);
        }
        try { card.scrollIntoView({behavior:'smooth', block:'nearest'}); } catch(_) {}
      } catch(e) { console.warn('[ESP-NOW] toggleDevicePanel error:', e); }
    };
    console.log('[ESP-NOW] Chunk 3E: toggleDevicePanel ready');
    console.log('[ESP-NOW] Chunk 3F: renderDevicePanel start');
    window.renderDevicePanel = function(mac, kind) {
      if (kind === 'message') {
        return (
          '<div style="display:grid;grid-template-columns:1fr 2fr;gap:12px;margin-bottom:12px">'
          + '<div class="interact-tabs">'
          + '<button class="btn interact-tab" id="btn-text-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'text\')">Text</button>'
          + '<button class="btn interact-tab" id="btn-remote-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'remote\')">Remote</button>'
          + '<button class="btn interact-tab" id="btn-file-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'file\')">File</button>'
          + '<button class="btn interact-tab" id="btn-metadata-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'metadata\')">Metadata</button>'
          + '<button class="btn interact-tab" id="btn-automations-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'automations\')">Automations</button>'
          + '<button class="btn interact-tab" id="btn-sensors-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'sensors\')">Sensor Streaming</button>'
          + '</div>'
          + '<div>'
          + '<div class="message-log" id="log-' + mac + '" style="margin-bottom:12px;max-height:300px;overflow-y:auto"><div class="message-empty">No messages yet. Start a conversation!</div></div>'
          + '<div id="text-input-' + mac + '" style="display:block">'
          + '<div style="display:flex;gap:8px;align-items:flex-start;flex-wrap:wrap">'
          + '<textarea id="msg-' + mac + '" placeholder="Message to send" style="flex:1;min-width:220px;min-height:60px;resize:vertical;font-family:inherit;padding:8px;border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg)"></textarea>'
          + '<button class="btn message-action-btn" onclick="doSendMessage(\'' + mac + '\')" style="align-self:flex-start">Send</button>'
          + '</div>'
          + '</div>'
          + '<div id="remote-input-' + mac + '" style="display:none">'
          + '<div class="input-group" style="margin-bottom:8px">'
          + '<input type="text" id="ru-' + mac + '" placeholder="Username" style="flex:1">'
          + '<input type="password" id="rp-' + mac + '" placeholder="Password" style="flex:1">'
          + '</div>'
          + '<div style="display:flex;gap:8px;align-items:flex-start">'
          + '<input type="text" id="rc-' + mac + '" placeholder="Command (e.g., sensors, memory)" style="flex:1;padding:8px;border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg)">'
          + '<button class="btn message-action-btn" onclick="doRemoteExec(\'' + mac + '\')" style="align-self:flex-start">Execute</button>'
          + '</div>'
          + '</div>'
          + '<div id="file-input-' + mac + '" style="display:none">'
          + '<div style="display:flex;gap:8px;margin-bottom:12px">'
          + '<button class="btn message-action-btn" id="btn-file-send-' + mac + '" onclick="toggleFileMode(\'' + mac + '\', \'send\')" style="flex:1">Send File</button>'
          + '<button class="btn message-action-btn" id="btn-file-receive-' + mac + '" onclick="toggleFileMode(\'' + mac + '\', \'receive\')" style="flex:1">Receive File</button>'
          + '</div>'
          + '<div id="file-send-panel-' + mac + '" style="display:block">'
          + '<div style="margin-bottom:12px">'
          + '<label style="display:block;margin-bottom:6px;font-weight:500;color:var(--panel-fg)">Browse Local Files:</label>'
          + '<div id="fexplorer-' + mac + '"></div>'
          + '</div>'
          + '<div style="margin-bottom:10px">'
          + '<label style="display:block;margin-bottom:5px;font-weight:500;color:var(--panel-fg)">File Path:</label>'
          + '<input type="text" id="fp-' + mac + '" placeholder="/path/to/file.ext or select from explorer" style="width:100%;padding:8px;border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg)">'
          + '<small style="color:var(--panel-fg);font-size:0.85em">Click a file in the explorer above or enter path manually</small>'
          + '</div>'
          + '<button class="btn message-action-btn" onclick="doSendFile(\'' + mac + '\')">Send File</button>'
          + '<div id="fstat-' + mac + '" style="margin-top:8px;padding:8px;border-radius:4px;font-size:0.9em;color:var(--panel-fg)">Select a file from the explorer or enter a file path manually</div>'
          + '</div>'
          + '<div id="file-receive-panel-' + mac + '" style="display:none">'
          + '<div class="input-group" style="margin-bottom:12px">'
          + '<input type="text" id="remote-user-' + mac + '" placeholder="Username" style="flex:1">'
          + '<input type="password" id="remote-pass-' + mac + '" placeholder="Password" style="flex:1">'
          + '<button class="btn message-action-btn" onclick="browseRemoteFiles(\'' + mac + '\', window.remoteCurrentPath && window.remoteCurrentPath[\'' + mac + '\'] ? window.remoteCurrentPath[\'' + mac + '\'] : \'/\')">Browse</button>'
          + '</div>'
          + '<div style="margin-bottom:12px">'
          + '<label style="display:block;margin-bottom:6px;font-weight:500;color:var(--panel-fg)">Remote Files:</label>'
          + '<div id="remote-fexplorer-' + mac + '" style="min-height:220px"></div>'
          + '</div>'
          + '<div style="margin-bottom:10px">'
          + '<label style="display:block;margin-bottom:5px;font-weight:500;color:var(--panel-fg)">Remote File Path:</label>'
          + '<input type="text" id="remote-fp-' + mac + '" placeholder="/path/to/remote/file.ext" style="width:100%;padding:8px;border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg)">'
          + '</div>'
          + '<button class="btn message-action-btn" onclick="fetchRemoteFile(\'' + mac + '\')">Fetch File</button>'
          + '<div id="remote-fstat-' + mac + '" style="margin-top:8px;padding:8px;border-radius:4px;font-size:0.9em;color:var(--panel-fg)">Enter credentials and browse remote device</div>'
          + '</div>'
          + '</div>'
          + '<div id="metadata-' + mac + '" style="display:none;padding:12px;background:var(--crumb-bg);border-radius:8px;min-height:200px">'
          + '<div style="margin-bottom:10px;text-align:right">'
          + '<button class="btn" onclick="syncMetadata(\'' + mac + '\')">Sync Metadata</button>'
          + '</div>'
          + '<div id="metadata-content-' + mac + '"><div style="text-align:center;color:var(--panel-fg);padding:20px">No metadata available. Click Sync Metadata to request from device.</div></div>'
          + '</div>'
          + '<div id="automations-input-' + mac + '" style="display:none">'
          + automationsInnerHtml(mac)
          + '</div>'
          + '<div id="sensors-input-' + mac + '" style="display:none;padding:12px;background:var(--crumb-bg);border-radius:8px;min-height:200px">'
          + '<p style="color:var(--panel-fg);font-size:0.9em;margin:0 0 10px 0">Select which sensors should stream, then apply the changes. Credentials are required.</p>'
          + '<div class="input-group" style="margin-bottom:12px">'
          + '<input type="text" id="su-' + mac + '" placeholder="Username" style="flex:1">'
          + '<input type="password" id="sp-' + mac + '" placeholder="Password" style="flex:1">'
          + '</div>'
          + '<div class="sensor-grid">'
          + ['thermal','tof','imu','gps','gamepad','fmradio','rtc','presence'].map(function(s){
              return '<div class="sensor-pill" id="sensor-pill-' + s + '-' + mac + '" onclick="toggleSensorSelection(\'' + mac + '\',\'' + s + '\')">' + s + '</div>';
            }).join('')
          + '</div>'
          + '<div style="display:flex;flex-wrap:wrap;gap:8px;margin-bottom:10px">'
          + '<button class="btn message-action-btn" onclick="applySensorStreaming(\'' + mac + '\')">Apply Streaming</button>'
          + '<div style="flex:1"></div>'
          + '<button class="btn message-action-btn" onclick="doSensorBroadcast(\'' + mac + '\', true)">Broadcast On</button>'
          + '<button class="btn message-action-btn" onclick="doSensorBroadcast(\'' + mac + '\', false)">Broadcast Off</button>'
          + '</div>'
          + '<div id="sensor-status-' + mac + '" style="font-size:0.85em;color:var(--muted)"></div>'
          + '</div>'
          + '</div>'
        );
      }
      if (kind === 'remote') {
        // Legacy remote panel - redirect to message panel with remote mode
        setTimeout(function() { toggleDevicePanel(mac, 'message'); toggleMessageType(mac, 'remote'); }, 0);
        return '<div style="color:var(--panel-fg);text-align:center;padding:20px">Redirecting to unified message panel...</div>';
      }
      if (kind === 'file') {
        // Legacy file panel - redirect to message panel with file mode
        setTimeout(function() { toggleDevicePanel(mac, 'message'); toggleMessageType(mac, 'file'); }, 0);
        return '<div style="color:var(--panel-fg);text-align:center;padding:20px">Redirecting to unified message panel...</div>';
      }
      if (kind === 'broadcast') {
        return (
          '<div style="margin-bottom:12px">'
          + '<p style="color:var(--panel-fg);margin-bottom:12px;">Send a message to all paired devices simultaneously.</p>'
          + '</div>'
          + '<div style="display:flex;gap:8px;align-items:flex-start;flex-wrap:wrap">'
          + '<textarea id="broadcast-msg" placeholder="Broadcast message to all devices" style="flex:1;min-width:220px;min-height:60px;resize:vertical;font-family:inherit;padding:8px;border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg)"></textarea>'
          + '<button class="btn" onclick="doBroadcast()" style="align-self:flex-start">Send Broadcast</button>'
          + '</div>'
          + '<div id="broadcast-status" style="margin-top:12px;padding:8px;border-radius:4px;display:none;"></div>'
        );
      }
      if (kind === 'metadata') {
        return (
          '<div id="metadata-' + mac + '" style="padding:12px;background:var(--crumb-bg);border-radius:8px;min-height:200px">'
          + '<div style="text-align:center;color:var(--panel-fg);padding:20px">Loading metadata...</div>'
          + '</div>'
        );
      }
      return '<div>Unknown panel</div>';
    };
    console.log('[ESP-NOW] Chunk 3F: renderDevicePanel ready');
    console.log('[ESP-NOW] Chunk 3G: appendLogLine start');
    window.appendLogLine = function(containerId, type, message, status) {
      console.log('[appendLogLine] Called with:', {containerId, type, message, status});
      const log = document.getElementById(containerId);
      if (!log) {
        console.warn('[appendLogLine] Container not found:', containerId);
        return;
      }
      
      // Dedup: skip if same RECEIVED message was just added to this container within 2s
      if (type === 'RECEIVED' || type === 'ERROR') {
        if (!window.__logDedup) window.__logDedup = {};
        var dedupKey = containerId + '|' + type + '|' + message;
        var now = Date.now();
        if (window.__logDedup[dedupKey] && (now - window.__logDedup[dedupKey]) < 2000) {
          console.log('[appendLogLine] Dedup: skipping duplicate message');
          return null;
        }
        window.__logDedup[dedupKey] = now;
        // Prune old dedup entries every 50 messages
        if (!window.__logDedupCount) window.__logDedupCount = 0;
        if (++window.__logDedupCount % 50 === 0) {
          for (var k in window.__logDedup) {
            if (now - window.__logDedup[k] > 5000) delete window.__logDedup[k];
          }
        }
      }
      console.log('[appendLogLine] Container found, appending message');
      
      // Remove empty state message if present
      const emptyMsg = log.querySelector('.message-empty');
      if (emptyMsg) emptyMsg.remove();
      
      const ts = new Date().toLocaleTimeString();
      const bubble = document.createElement('div');
      bubble.className = 'message-bubble ' + (type==='ERROR'?'message-error': type==='RECEIVED'?'message-received':'message-sent');
      
      const textDiv = document.createElement('div');
      textDiv.className = 'message-text';
      textDiv.textContent = message;
      
      const statusDiv = document.createElement('div');
      statusDiv.className = 'message-status';
      
      // Determine status icon and text
      let statusIcon = '';
      let statusText = '';
      if (type === 'RECEIVED') {
        statusIcon = 'RX';
        statusText = ts;
      } else if (type === 'ERROR') {
        statusIcon = 'ERR';
        statusText = status || 'Failed';
      } else if (type === 'SENT') {
        if (status === 'sending') {
          statusIcon = '...';
          statusText = 'Sending...';
        } else if (status === 'sent') {
          statusIcon = 'OK';
          statusText = 'Sent';
        } else if (status === 'delivered') {
          statusIcon = 'OK';
          statusText = 'Delivered';
        } else {
          statusIcon = 'OK';
          statusText = ts;
        }
      }
      
      statusDiv.innerHTML = '<span class="status-icon">' + statusIcon + '</span>' + statusText;
      
      bubble.appendChild(textDiv);
      bubble.appendChild(statusDiv);
      log.appendChild(bubble);
      log.scrollTop = log.scrollHeight;
      console.log('[appendLogLine] Message appended successfully');
      
      return bubble;
    };
    console.log('[ESP-NOW] Chunk 3G: appendLogLine ready');
    console.log('[ESP-NOW] Chunk 3H: doSendMessage start');
    window.doSendMessage = function(mac) {
      const val = (document.getElementById('msg-' + mac) || {}).value || '';
      if (!val) { alert('Enter a message'); return; }
      
      // Clear input immediately
      const input = document.getElementById('msg-' + mac);
      if (input) input.value = '';
      
      // Show message as "sending" immediately
      const bubble = appendLogLine('log-' + mac, 'SENT', val, 'sending');
      
      // Use already-tracked mode — no extra round-trip needed
      var cmd = 'espnowsend ' + mac + ' ' + val;
      fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent(cmd) })
        .then(r=>r.text())
        .then(t=>{
          console.log('[ESP-NOW] Send result:', t);
          // Update bubble status based on result
          if (bubble) {
            const statusDiv = bubble.querySelector('.message-status');
            const lowerResult = (t || '').toLowerCase();
            // Check for ACK confirmation (v2 protocol with ACK)
            if (lowerResult.indexOf('failed') >= 0 || lowerResult.indexOf('error') >= 0) {
              statusDiv.innerHTML = '<span class="status-icon">✗</span>Failed';
            } else if (lowerResult.indexOf('message sent') >= 0 || lowerResult.indexOf('sent via v3') >= 0) {
              statusDiv.innerHTML = '<span class="status-icon">✓✓</span>Delivered';
            } else if (lowerResult.indexOf('sent') >= 0) {
              statusDiv.innerHTML = '<span class="status-icon">✓</span>Sent';
            } else {
              statusDiv.innerHTML = '<span class="status-icon">✓</span>' + t;
            }
          }
        })
        .catch(e=> {
          // Update bubble to show error
          if (bubble) {
            bubble.className = 'message-bubble message-error';
            const statusDiv = bubble.querySelector('.message-status');
            statusDiv.innerHTML = '<span class="status-icon">✗</span>Failed: ' + e.message;
          }
        });
    };
    console.log('[ESP-NOW] Chunk 3H: doSendMessage ready');
    console.log('[ESP-NOW] Chunk 3I: doBroadcast start');
    window.doBroadcast = function(){
      const input = document.getElementById('broadcast-msg');
      const statusDiv = document.getElementById('broadcast-status');
      const msg = (input || {}).value || '';
      
      if (!msg) { 
        alert('Enter a broadcast message'); 
        return; 
      }
      
      // Show sending status
      if (statusDiv) {
        statusDiv.style.display = 'block';
        statusDiv.style.background = '#fff3cd';
        statusDiv.style.color = '#856404';
        statusDiv.textContent = 'Broadcasting message...';
      }
      
      fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent('espnowbroadcast ' + msg) })
        .then(r=>r.text())
        .then(t=> {
          // Clear input
          if (input) input.value = '';
          
          // Show success feedback
          if (statusDiv) {
            statusDiv.style.background = '#d4edda';
            statusDiv.style.color = '#155724';
            statusDiv.innerHTML = '<strong>Broadcast sent successfully!</strong><br><small>Message: "' + msg + '"</small>';
            setTimeout(function() { 
              statusDiv.style.display = 'none'; 
            }, 5000);
          }
        })
        .catch(e=> {
          // Show error feedback
          if (statusDiv) {
            statusDiv.style.display = 'block';
            statusDiv.style.background = '#f8d7da';
            statusDiv.style.color = '#721c24';
            statusDiv.textContent = 'Broadcast failed: ' + e.message;
          } else {
            alert('Broadcast error: ' + e.message);
          }
        });
    };
    console.log('[ESP-NOW] Chunk 3I: doBroadcast ready');
    console.log('[ESP-NOW] Chunk 3J: doSendFile start');
    window.doSendFile = function(mac) {
      const path = (document.getElementById('fp-' + mac) || {}).value || '';
      if (!path) { 
        alert('Enter a file path or select a file from the explorer'); 
        return; 
      }
      
      const statDiv = document.getElementById('fstat-' + mac);
      const filename = path.split('/').pop();
      
      // Show sending status
      if (statDiv) {
        statDiv.style.background = '#fff3cd';
        statDiv.style.color = '#856404';
        statDiv.textContent = 'Sending file: ' + filename + '...';
      }
      
      // Also show in message log
      appendLogLine('log-' + mac, 'SENT', 'Sending file: ' + filename, 'sending');
      
      fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent('espnowsendfile ' + mac + ' ' + path) })
        .then(r=>r.text())
        .then(t=>{
          const success = t.toLowerCase().indexOf('success') >= 0 || t.toLowerCase().indexOf('sent') >= 0;
          
          if (statDiv) {
            if (success) {
              statDiv.style.background = '#d4edda';
              statDiv.style.color = '#155724';
              statDiv.textContent = t;
            } else {
              statDiv.style.background = '#f8d7da';
              statDiv.style.color = '#721c24';
              statDiv.textContent = t;
            }
          }
          
          // Update message log with result
          if (success) {
            appendLogLine('log-' + mac, 'RECEIVED', 'File sent: ' + filename, null);
          } else {
            appendLogLine('log-' + mac, 'ERROR', 'File transfer failed: ' + t, null);
          }
          
          // Clear file path input on success
          if (success) {
            const fpInput = document.getElementById('fp-' + mac);
            if (fpInput) fpInput.value = '';
          }
        })
        .catch(e=>{
          if (statDiv) {
            statDiv.style.background = '#f8d7da';
            statDiv.style.color = '#721c24';
            statDiv.textContent = 'Error: ' + e.message;
          }
          appendLogLine('log-' + mac, 'ERROR', 'File transfer error: ' + e.message, null);
        });
    };
    console.log('[ESP-NOW] Chunk 3J: doSendFile ready');
    console.log('[ESP-NOW] Chunk 3K: toggleMessageType start');
    window.toggleMessageType = function(mac, type) {
      const textInput = document.getElementById('text-input-' + mac);
      const remoteInput = document.getElementById('remote-input-' + mac);
      const fileInput = document.getElementById('file-input-' + mac);
      const metadataDiv = document.getElementById('metadata-' + mac);
      const automationsInput = document.getElementById('automations-input-' + mac);
      const sensorsInput = document.getElementById('sensors-input-' + mac);
      const messageLog = document.getElementById('log-' + mac);
      const btnText = document.getElementById('btn-text-' + mac);
      const btnFile = document.getElementById('btn-file-' + mac);
      const btnMetadata = document.getElementById('btn-metadata-' + mac);
      const btnAutomations = document.getElementById('btn-automations-' + mac);
      const btnSensors = document.getElementById('btn-sensors-' + mac);
      
      if (!textInput || !remoteInput || !fileInput) return;
      
      // Reset all button styles
      [btnText, btnRemote, btnFile, btnMetadata, btnAutomations, btnSensors].forEach(function(btn) {
        if (btn) btn.classList.remove('interact-tab-active');
      });
      if (btnRemote) {
        btnRemote.style.background = '';
        btnRemote.style.color = '';
        btnRemote.style.border = '';
      }
      
      // Hide all inputs and metadata
      textInput.style.display = 'none';
      remoteInput.style.display = 'none';
      fileInput.style.display = 'none';
      if (metadataDiv) metadataDiv.style.display = 'none';
      if (automationsInput) automationsInput.style.display = 'none';
      if (sensorsInput) sensorsInput.style.display = 'none';
      
      // messageLog already declared as const above, reuse it
      
      // Show selected input and highlight button
      if (type === 'text') {
        textInput.style.display = 'block';
        if (messageLog) messageLog.style.display = 'block';
        if (btnText) btnText.classList.add('interact-tab-active');
      } else if (type === 'remote') {
        remoteInput.style.display = 'block';
        if (messageLog) messageLog.style.display = 'block';
        if (btnRemote) btnRemote.classList.add('interact-tab-active');
      } else if (type === 'file') {
        fileInput.style.display = 'block';
        if (messageLog) messageLog.style.display = 'none';
        if (btnFile) btnFile.classList.add('interact-tab-active');
        // Initialize file browser when file mode is selected
        if (typeof window.initializeFileBrowser === 'function') {
          setTimeout(function() { window.initializeFileBrowser(mac); }, 100);
        }
      } else if (type === 'metadata') {
        if (metadataDiv) metadataDiv.style.display = 'block';
        if (messageLog) messageLog.style.display = 'none';
        if (btnMetadata) btnMetadata.classList.add('interact-tab-active');
        // Load cached metadata if available; don't auto-request from device
        window.loadDeviceMetadata(mac);
      } else if (type === 'automations') {
        if (automationsInput) automationsInput.style.display = 'block';
        if (messageLog) messageLog.style.display = 'none';
        if (btnAutomations) btnAutomations.classList.add('interact-tab-active');
        // Auto-load already-received automations file if it exists
        window.tryLoadExistingAutomations(mac);
        // Pre-fill credentials from remote tab if available
        var ruEl = document.getElementById('ru-' + mac);
        var rpEl = document.getElementById('rp-' + mac);
        var auEl = document.getElementById('au-' + mac);
        var apEl = document.getElementById('ap-' + mac);
        if (ruEl && auEl && !auEl.value && ruEl.value) auEl.value = ruEl.value;
        if (rpEl && apEl && !apEl.value && rpEl.value) apEl.value = rpEl.value;
      } else if (type === 'sensors') {
        if (sensorsInput) sensorsInput.style.display = 'block';
        if (messageLog) messageLog.style.display = 'none';
        if (btnSensors) btnSensors.classList.add('interact-tab-active');
        // Pre-fill credentials from remote tab if available
        var ruEl2 = document.getElementById('ru-' + mac);
        var rpEl2 = document.getElementById('rp-' + mac);
        var suEl = document.getElementById('su-' + mac);
        var spEl = document.getElementById('sp-' + mac);
        if (ruEl2 && suEl && !suEl.value && ruEl2.value) suEl.value = ruEl2.value;
        if (rpEl2 && spEl && !spEl.value && rpEl2.value) spEl.value = rpEl2.value;
      }
    };
    console.log('[ESP-NOW] Chunk 3K: toggleMessageType ready');
    console.log('[ESP-NOW] Chunk 3L: loadRemoteAutomations start');
    window.loadRemoteAutomations = function(mac) {
      var listDiv = document.getElementById('automations-list-' + mac);
      if (!listDiv) return;
      if (window.__autoFetchState && window.__autoFetchState[mac]) {
        listDiv.innerHTML = '<div style="color:var(--muted);padding:12px;text-align:center">Transfer already in progress...</div>';
        window.setAutoButtonState(mac, { disabled: true, text: 'Transferring...' });
        return;
      }
      var u = (document.getElementById('au-' + mac) || {}).value || '';
      var p = (document.getElementById('ap-' + mac) || {}).value || '';
      var esc = (typeof hw !== 'undefined' && hw._esc)
        ? hw._esc
        : function(s){return String(s).replace(/[&<>"]/g,function(c){return({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'})[c]||c;});};
      if (!u || !p) {
        if (typeof hw !== 'undefined') hw.notify('warning', 'Enter username and password', 3000);
        else alert('Enter username and password');
        return;
      }
      if (window.__autoFetchState) window.__autoFetchState[mac] = true;
      window.setAutoButtonState(mac, { disabled: true, text: 'Requesting...' });
      listDiv.innerHTML = '<div style="color:var(--muted);padding:12px;text-align:center">Requesting automations via ESP-NOW...</div>';
      var macHex = mac.replace(/:/g, '').toUpperCase();
      var filePath = '/espnow/received/' + macHex + '/automations.json';
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'cmd=' + encodeURIComponent('espnowfetch ' + mac + ' ' + u + ' ' + p + ' /system/automations.json')
      })
      .then(function(r) { return r.text(); })
      .then(function(resp) {
        var lower = (resp || '').toLowerCase();
        if (lower.indexOf('error') >= 0 || lower.indexOf('not initialized') >= 0) {
          listDiv.innerHTML = '<div style="color:var(--danger);padding:12px">' + esc(resp) + '</div>';
          window.markAutomationsFetchIdle(mac, 'Retry Automations');
          return;
        }
        var attempts = 0;
        listDiv.innerHTML = '<div style="color:var(--muted);padding:12px;text-align:center">Transfer in progress (1/30)...</div>';
        function poll() {
          attempts++;
          window.setAutoButtonState(mac, { disabled: true, text: 'Receiving ' + attempts + '/30' });
          fetch('/api/files/read?name=' + encodeURIComponent(filePath))
            .then(function(r) {
              if (r.status === 404) {
                if (attempts < 30) {
                  listDiv.innerHTML = '<div style="color:var(--muted);padding:12px;text-align:center">Transfer in progress (' + attempts + '/30)...</div>';
                  setTimeout(poll, 1000);
                } else {
                  listDiv.innerHTML = '<div style="color:var(--danger);padding:12px">Timed out. Is encryption enabled and both devices securely paired?</div>';
                  window.markAutomationsFetchIdle(mac, 'Retry Automations');
                }
                return null;
              }
              if (!r.ok) throw new Error('HTTP ' + r.status);
              return r.text();
            })
            .then(function(text) {
              if (text === null || text === undefined) return;
              // File is now cached — use tryLoadExistingAutomations which renders
              // with proper click handlers, Run buttons, and event listeners
              window.tryLoadExistingAutomations(mac);
              if (typeof hw !== 'undefined') hw.notify('success', 'Automations loaded', 2000);
            })
            .catch(function(e) {
              listDiv.innerHTML = '<div style="color:var(--danger);padding:12px">Error: ' + esc(e.message) + '</div>';
              window.markAutomationsFetchIdle(mac, 'Retry Automations');
            });
        }
        setTimeout(poll, 1500);
      })
      .catch(function(e) {
        listDiv.innerHTML = '<div style="color:var(--danger);padding:12px">Error: ' + esc(e.message) + '</div>';
        window.markAutomationsFetchIdle(mac, 'Retry Automations');
      });
    };
    console.log('[ESP-NOW] Chunk 3L: loadRemoteAutomations ready');
    console.log('[ESP-NOW] Chunk 3M: doRemoteExec start');
    window.doRemoteExec = function(mac) {
      const u = (document.getElementById('ru-' + mac) || {}).value || '';
      const p = (document.getElementById('rp-' + mac) || {}).value || '';
      const c = (document.getElementById('rc-' + mac) || {}).value || '';
      if (!u || !p || !c) { alert('Enter username, password, and command'); return; }
      
      // Show command being executed in message log
      const sendingBubble = appendLogLine('log-' + mac, 'SENT', 'Remote: ' + c, 'sending');
      
      // Remote command output is picked up by the message polling loop
      // (/api/espnow/messages) which runs every 500ms — no SSE needed.
      const cmd = 'espnowremote ' + mac + ' ' + u + ' ' + p + ' ' + c;
      fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent(cmd) })
        .then(r=>r.text())
        .then(t=>{
          console.log('[ESP-NOW] Remote exec result:', t);
          // Update sending bubble to show completion
          if (sendingBubble) {
            const statusDiv = sendingBubble.querySelector('.message-status');
            const lowerResult = (t || '').toLowerCase();
            // Check for ACK confirmation (v2 protocol with ACK)
            if (lowerResult.indexOf('failed') >= 0 || lowerResult.indexOf('error') >= 0) {
              statusDiv.innerHTML = '<span class="status-icon">✗</span>Failed';
            } else if (lowerResult.indexOf('remote command sent') >= 0) {
              statusDiv.innerHTML = '<span class="status-icon">✓</span>Sent';
            } else {
              statusDiv.innerHTML = '<span class="status-icon">✓</span>' + t;
            }
          }
          
          // Show immediate result if it's not just the "sent" confirmation
          if (t && !t.includes('Remote command sent')) {
            appendLogLine('log-' + mac, 'RECEIVED', 'Result: ' + t, null);
          }
          
          // Clear command input
          const cmdInput = document.getElementById('rc-' + mac);
          if (cmdInput) cmdInput.value = '';
        })
        .catch(e=> {
          // Update sending bubble to show error
          if (sendingBubble) {
            sendingBubble.className = 'message-bubble message-error';
            const textDiv = sendingBubble.querySelector('.message-text');
            if (textDiv) textDiv.textContent = 'Remote: ' + c + ' (FAILED)';
            const statusDiv = sendingBubble.querySelector('.message-status');
            if (statusDiv) {
              statusDiv.innerHTML = '<span class="status-icon">✗</span>Failed';
            }
          }
        });
    };
    console.log('[ESP-NOW] Chunk 3M: doRemoteExec ready');
    console.log('[ESP-NOW] Chunk 3N: toggleFileMode start');
    window.toggleFileMode = function(mac, mode) {
      const sendPanel = document.getElementById('file-send-panel-' + mac);
      const receivePanel = document.getElementById('file-receive-panel-' + mac);
      const btnSend = document.getElementById('btn-file-send-' + mac);
      const btnReceive = document.getElementById('btn-file-receive-' + mac);
      
      if (!sendPanel || !receivePanel || !btnSend || !btnReceive) return;
      
      if (mode === 'send') {
        sendPanel.style.display = 'block';
        receivePanel.style.display = 'none';
        btnSend.style.background = 'var(--link)';
        btnSend.style.color = 'white';
        btnReceive.style.background = '';
        btnReceive.style.color = '';
      } else if (mode === 'receive') {
        sendPanel.style.display = 'none';
        receivePanel.style.display = 'block';
        btnSend.style.background = '';
        btnSend.style.color = '';
        btnReceive.style.background = 'var(--link)';
        btnReceive.style.color = 'white';
      }
    };
    console.log('[ESP-NOW] Chunk 3N: toggleFileMode ready');
    console.log('[ESP-NOW] Chunk 3O: browseRemoteFiles start');
    window.browseRemoteFiles = function(mac, path) {
      var u = (document.getElementById('remote-user-' + mac) || {}).value || '';
      var p = (document.getElementById('remote-pass-' + mac) || {}).value || '';
      var container = document.getElementById('remote-fexplorer-' + mac);
      var statusDiv = document.getElementById('remote-fstat-' + mac);
      
      if (!u || !p) {
        if (statusDiv) statusDiv.textContent = 'Enter username and password first';
        return;
      }
      
      if (!container) return;
      container.innerHTML = '<div class="remote-explorer"><div class="remote-explorer-crumb">Loading...</div><div class="remote-entry remote-entry-empty" style="display:flex">Requesting directory...</div></div>';
      if (statusDiv) statusDiv.textContent = 'Requesting directory listing from ' + mac + '...';
      
      var targetMac = String(mac || '').toUpperCase();
      var browsePath = path || '/';
      var seqBefore = 0;
      
      // Get current max sequence number so we only look at NEW messages
      fetch('/api/espnow/messages?since=0')
        .then(function(r) { return r.json(); })
        .then(function(data) {
          var msgs = Array.isArray(data) ? data : (data.messages || []);
          for (var i = 0; i < msgs.length; i++) {
            var s = msgs[i].seq || msgs[i].seqNum || 0;
            if (s > seqBefore) seqBefore = s;
          }
        })
        .catch(function() {})
        .finally(function() {
      
      // Send browse command (sends V3 CMD: user:pass:files /path)
      var cmd = 'espnowremote ' + mac + ' ' + u + ' ' + p + ' files ' + browsePath;
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) { return r.text(); })
      .then(function(text) {
        if (!text.includes('Remote command sent')) {
          container.innerHTML = '<pre style="margin:0;white-space:pre-wrap;font-size:0.85em">' + text + '</pre>';
          if (statusDiv) statusDiv.textContent = text;
          return;
        }
        if (statusDiv) statusDiv.textContent = 'Request sent, waiting for response...';
        
        // Poll peer messages for streamed file listing output
        var pollCount = 0;
        var maxPolls = 15;
        var pollInterval = setInterval(function() {
          pollCount++;
          if (pollCount > maxPolls) {
            clearInterval(pollInterval);
            container.innerHTML = '<div class="remote-explorer"><div class="remote-explorer-crumb">' + browsePath + '</div><div class="remote-entry remote-entry-empty" style="display:flex">Timed out waiting for response</div></div>';
            if (statusDiv) statusDiv.textContent = 'Timed out';
            return;
          }
          
          fetch('/api/espnow/messages?since=' + seqBefore)
            .then(function(r) { return r.json(); })
            .then(function(data) {
              var msgs = Array.isArray(data) ? data : (data.messages || []);
              var browseLines = [];
              var foundComplete = false;
              
              for (var i = 0; i < msgs.length; i++) {
                var m = msgs[i];
                var mMac = String(m.mac || m.from || '').toUpperCase();
                var mMsg = String(m.message || m.msg || m.text || '');
                
                if (mMac !== targetMac) continue;
                
                // Match streamed file listing output from 'files' command
                if (mMsg.indexOf('Files (') >= 0 || 
                    mMsg.indexOf('items)') >= 0 || 
                    mMsg.indexOf('bytes)') >= 0 ||
                    mMsg.indexOf('Total:') >= 0 ||
                    mMsg.indexOf('[DIR]') >= 0 || 
                    mMsg.indexOf('[FILE]') >= 0 ||
                    mMsg.indexOf('File listing') >= 0 ||
                    mMsg.indexOf('File browse FAILED') >= 0 ||
                    mMsg.indexOf('empty directory') >= 0 ||
                    mMsg.indexOf('Directory not found') >= 0 ||
                    mMsg.indexOf('Error:') >= 0) {
                  browseLines.push(mMsg);
                }
                if (mMsg.indexOf('Total:') >= 0) foundComplete = true;
              }
              
              if (foundComplete && browseLines.length > 0) {
                clearInterval(pollInterval);
                // Flatten multi-line messages into individual lines before parsing
                var flatLines = [];
                for (var li = 0; li < browseLines.length; li++) {
                  var subLines = browseLines[li].split('\n');
                  for (var si = 0; si < subLines.length; si++) {
                    flatLines.push(subLines[si]);
                  }
                }
                var entries = window.parseRemoteFileListing(flatLines);
                window.renderRemoteFileExplorer(mac, browsePath, entries);
                if (statusDiv) statusDiv.textContent = 'Browse complete - ' + entries.length + ' items in ' + browsePath;
              } else if (pollCount > 2) {
                if (statusDiv) statusDiv.textContent = 'Waiting for response... (' + pollCount + '/' + maxPolls + ')';
              }
            })
            .catch(function() {});
        }, 1000);
      })
      .catch(function(e) {
        container.innerHTML = '<div style="color:var(--danger);padding:12px">Error: ' + e.message + '</div>';
        if (statusDiv) statusDiv.textContent = 'Browse error: ' + e.message;
      });
      
      }); // end of .finally() from seqBefore fetch
    };
    window.fetchRemoteFile = function(mac) {
      var u = (document.getElementById('remote-user-' + mac) || {}).value || '';
      var p = (document.getElementById('remote-pass-' + mac) || {}).value || '';
      var remotePath = (document.getElementById('remote-fp-' + mac) || {}).value || '';
      var statusDiv = document.getElementById('remote-fstat-' + mac);
      if (!u || !p || !remotePath) {
        if (statusDiv) statusDiv.textContent = 'Enter username, password, and remote file path';
        return;
      }
      var filename = remotePath.split('/').pop();
      if (statusDiv) {
        statusDiv.style.background = '#fff3cd';
        statusDiv.style.color = '#856404';
        statusDiv.textContent = 'Fetching ' + filename + ' from ' + mac + '...';
      }
      var cmd = 'espnowfetch ' + mac + ' ' + u + ' ' + p + ' ' + remotePath;
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent(cmd)
      }).then(function(r) { return r.text(); }).then(function(text) {
        if (!text.includes('File fetch request sent') && !text.includes('Receiving file')) {
          if (statusDiv) {
            statusDiv.style.background = '#f8d7da';
            statusDiv.style.color = '#721c24';
            statusDiv.textContent = 'Failed: ' + text;
          }
          appendLogLine('log-' + mac, 'ERROR', 'Fetch failed: ' + text, null);
          return;
        }
        appendLogLine('log-' + mac, 'SENT', 'Fetch request sent for: ' + filename, null);
        fetch('/api/espnow/messages?mac=' + mac + '&since=0').then(function(r) {
          return r.json();
        }).then(function(data) {
          var existing = Array.isArray(data) ? data : (data.messages || []);
          var sinceSeq = existing.length > 0 ? (existing[existing.length - 1].seq || 0) : 0;
          var pollCount = 0;
          var pollMax = 15;
          var poll = setInterval(function() {
            pollCount++;
            fetch('/api/espnow/messages?mac=' + mac + '&since=' + sinceSeq).then(function(r) {
              return r.json();
            }).then(function(msgs_data) {
              var msgs = Array.isArray(msgs_data) ? msgs_data : (msgs_data.messages || []);
              for (var i = msgs.length - 1; i >= 0; i--) {
                var m = (msgs[i].msg || '');
                if (m.includes('File sent successfully') && m.includes(filename)) {
                  clearInterval(poll);
                  if (statusDiv) {
                    statusDiv.style.background = '#d4edda';
                    statusDiv.style.color = '#155724';
                    statusDiv.textContent = 'Received: ' + filename;
                  }
                  appendLogLine('log-' + mac, 'RECEIVED', 'File received: ' + filename, null);
                  return;
                }
                if ((m.includes('failed') || m.includes('error') || m.includes('Error')) && m.includes(filename)) {
                  clearInterval(poll);
                  if (statusDiv) {
                    statusDiv.style.background = '#f8d7da';
                    statusDiv.style.color = '#721c24';
                    statusDiv.textContent = 'Transfer failed: ' + m;
                  }
                  appendLogLine('log-' + mac, 'ERROR', 'Fetch failed: ' + m, null);
                  return;
                }
              }
              if (pollCount >= pollMax) {
                clearInterval(poll);
                if (statusDiv) {
                  statusDiv.style.background = '#f8d7da';
                  statusDiv.style.color = '#721c24';
                  statusDiv.textContent = 'Timed out waiting for ' + filename;
                }
                appendLogLine('log-' + mac, 'ERROR', 'Fetch timed out: ' + filename, null);
              }
            }).catch(function() { if (pollCount >= pollMax) clearInterval(poll); });
          }, 1000);
        }).catch(function() {});
      }).catch(function(e) {
        if (statusDiv) {
          statusDiv.style.background = '#f8d7da';
          statusDiv.style.color = '#721c24';
          statusDiv.textContent = 'Fetch error: ' + e.message;
        }
        appendLogLine('log-' + mac, 'ERROR', 'Fetch error: ' + e.message, null);
      });
    };
    window.unpairDevice = async function(mac) {
      if (await hwConfirm('Unpair device ' + mac + '?')) {
        fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('espnowunpair ' + mac)
        })
        .then(response => response.text())
        .then(text => {
          addMessageToLog('UNPAIR', text);
          listDevices();
        })
        .catch(error => {
          addMessageToLog('ERROR', 'Unpair error: ' + error);
        });
      }
    };
    window.loadDeviceMetadata = function(mac) {
      const container = document.getElementById('metadata-content-' + mac)
                     || document.getElementById('metadata-' + mac);
      if (!container) return;
      
      container.innerHTML = '<div style="text-align:center;color:var(--panel-fg);padding:20px">Loading metadata...</div>';
      
      fetch('/api/espnow/metadata?mac=' + encodeURIComponent(mac))
        .then(r => r.json())
        .then(data => {
          if (!data.found) {
            container.innerHTML = '<div style="text-align:center;color:var(--panel-fg);padding:20px">No metadata available for this device</div>';
            return;
          }
          
          var html = '<div style="display:grid;gap:12px">';
          
          if (data.deviceName) {
            html += '<div><label style="font-weight:600;color:var(--panel-fg);display:block;margin-bottom:4px">Device Name</label>';
            html += '<div style="padding:8px;background:var(--panel-bg);border-radius:4px;border:1px solid var(--border)">' + data.deviceName + '</div></div>';
          }
          
          if (data.friendlyName) {
            html += '<div><label style="font-weight:600;color:var(--panel-fg);display:block;margin-bottom:4px">Friendly Name</label>';
            html += '<div style="padding:8px;background:var(--panel-bg);border-radius:4px;border:1px solid var(--border)">' + data.friendlyName + '</div></div>';
          }
          
          if (data.room) {
            html += '<div><label style="font-weight:600;color:var(--panel-fg);display:block;margin-bottom:4px">Room</label>';
            html += '<div style="padding:8px;background:var(--panel-bg);border-radius:4px;border:1px solid var(--border)">' + data.room + '</div></div>';
          }
          
          if (data.zone) {
            html += '<div><label style="font-weight:600;color:var(--panel-fg);display:block;margin-bottom:4px">Zone</label>';
            html += '<div style="padding:8px;background:var(--panel-bg);border-radius:4px;border:1px solid var(--border)">' + data.zone + '</div></div>';
          }
          
          if (data.tags) {
            html += '<div><label style="font-weight:600;color:var(--panel-fg);display:block;margin-bottom:4px">Tags</label>';
            html += '<div style="padding:8px;background:var(--panel-bg);border-radius:4px;border:1px solid var(--border)">' + data.tags + '</div></div>';
          }
          
          html += '<div><label style="font-weight:600;color:var(--panel-fg);display:block;margin-bottom:4px">Stationary</label>';
          html += '<div style="padding:8px;background:var(--panel-bg);border-radius:4px;border:1px solid var(--border)">' + (data.stationary ? 'Yes' : 'No') + '</div></div>';
          
          html += '<div style="margin-top:8px;padding:8px;background:var(--panel-bg);border-radius:4px;font-size:0.85em;color:var(--panel-fg)">';
          html += '<strong>Source:</strong> ' + (data.source === 'mesh' ? 'Mesh/Pairing Mode' : 'Bonded Mode (Cached)');
          html += '</div>';

          html += '</div>';
          
          container.innerHTML = html;
        })
        .catch(e => {
          container.innerHTML = '<div style="text-align:center;color:var(--danger);padding:20px">Error loading metadata: ' + e.message + '</div>';
        });
    };
    window.syncMetadata = function(mac) {
      const container = document.getElementById('metadata-content-' + mac)
                     || document.getElementById('metadata-' + mac);
      if (!container) return;
      
      container.innerHTML = '<div style="text-align:center;color:var(--panel-fg);padding:20px">Requesting metadata from device...</div>';
      
      // Send V3 METADATA_REQ to the peer - no credentials needed, it's a protocol-level request
      // The peer responds with METADATA_RESP which populates gMeshPeerMeta on our side
      fetch('/api/cli', { 
        method: 'POST', 
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, 
        body: 'cmd=' + encodeURIComponent('espnowrequestmeta ' + mac) 
      })
        .then(function(r) { return r.text(); })
        .then(function(result) {
          if (typeof addMessageToLog === 'function') addMessageToLog('INFO', 'Metadata sync: ' + result);
          appendLogLine('log-' + mac, 'RECEIVED', 'Metadata request sent', null);
          // Poll for metadata to appear (peer responds within ~1-2 seconds)
          var metaPollCount = 0;
          var metaPollInterval = setInterval(function() {
            metaPollCount++;
            if (metaPollCount > 10) {
              clearInterval(metaPollInterval);
              container.innerHTML = '<div style="text-align:center;color:var(--muted);padding:20px">Timed out waiting for metadata response</div>';
              return;
            }
            fetch('/api/espnow/metadata?mac=' + encodeURIComponent(mac))
              .then(function(r) { return r.json(); })
              .then(function(data) {
                if (data.found) {
                  clearInterval(metaPollInterval);
                  window.loadDeviceMetadata(mac);
                }
              })
              .catch(function() {});
          }, 1000);
        })
        .catch(function(e) {
          container.innerHTML = '<div style="text-align:center;color:var(--danger);padding:20px">Sync error: ' + e.message + '</div>';
        });
    };
    window.tryLoadExistingAutomations = function(mac) {
      var listDiv = document.getElementById('automations-list-' + mac);
      if (!listDiv) return;
      
      var macHex = mac.replace(/:/g, '').toUpperCase();
      var filePath = '/espnow/received/' + macHex + '/automations.json';
      var esc = (typeof hw !== 'undefined' && hw._esc)
        ? hw._esc
        : function(s){return String(s).replace(/[&<>"]/g,function(c){return({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'})[c]||c;});};
      
      listDiv.innerHTML = '<div style="color:var(--muted);padding:12px;text-align:center">Checking for cached automations...</div>';
      
      fetch('/api/files/read?name=' + encodeURIComponent(filePath))
        .then(function(r) {
          if (r.status === 404) {
            listDiv.innerHTML = '<div style="color:var(--muted);padding:20px;text-align:center">No automations file cached. Click "Load Automations" to request from device.</div>';
            return null;
          }
          if (!r.ok) throw new Error('HTTP ' + r.status);
          return r.text();
        })
        .then(function(text) {
          if (text === null || text === undefined) return;
          var trimmed = text.trim();
          if (!trimmed || (trimmed[0] !== '{' && trimmed[0] !== '[')) {
            listDiv.innerHTML = '<div style="color:var(--muted);padding:20px;text-align:center">No automations file cached. Click "Load Automations" to request from device.</div>';
            return;
          }
          try {
            var data = JSON.parse(trimmed);
            var autos = Array.isArray(data.automations) ? data.automations : [];
            if (autos.length === 0) {
              listDiv.innerHTML = '<div style="color:var(--muted);padding:20px;text-align:center">No automations on this device.</div>';
              return;
            }
            var autoUid = 'auto_' + mac.replace(/:/g, '') + '_';
            if (!window.__autoCache) window.__autoCache = {};
            var html = '<div style="display:flex;flex-direction:column;gap:8px">';
            autos.forEach(function(a, idx) {
              var sched = a.schedule || {};
              var schedStr = sched.type || '?';
              if (sched.type === 'time' && sched.time) schedStr = sched.time;
              else if (sched.type === 'interval' && sched.intervalMs) schedStr = (sched.intervalMs / 1000) + 's';
              else if (sched.type === 'boot') schedStr = 'boot';
              var enabled = a.enabled !== false;
              var cmds = Array.isArray(a.commands) ? a.commands : [];
              var conditions = Array.isArray(a.conditions) ? a.conditions : [];
              var dot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:' + (enabled ? '#28a745' : '#dc3545') + ';margin-right:6px;vertical-align:middle"></span>';
              var detailId = autoUid + idx;
              html += '<div class="auto-entry" data-detail-id="' + detailId + '" style="padding:10px 12px;background:var(--crumb-bg);border-radius:8px;border:1px solid var(--border);cursor:pointer">';
              html += '<div style="display:flex;align-items:center;justify-content:space-between;gap:8px">';
              html += '<div>' + dot + '<strong>' + esc(a.name || '(unnamed)') + '</strong></div>';
              html += '<div style="color:var(--muted);font-size:0.85em;white-space:nowrap">' + esc(schedStr) + ' &bull; ' + cmds.length + ' cmd' + (cmds.length !== 1 ? 's' : '') + '</div>';
              html += '</div>';
              html += '<div id="' + detailId + '" style="display:none;margin-top:8px;padding-top:8px;border-top:1px solid var(--border);font-size:0.85em">';
              html += '<div style="margin-bottom:6px"><span style="color:var(--muted)">Schedule:</span> <strong>' + esc(sched.type || 'unknown') + '</strong>';
              if (sched.time) html += ' at ' + esc(sched.time);
              if (sched.intervalMs) html += ' every ' + (sched.intervalMs / 1000) + 's';
              if (sched.days) html += ' on ' + esc(String(sched.days));
              html += '</div>';
              if (conditions.length > 0) {
                html += '<div style="margin-bottom:6px"><span style="color:var(--muted)">Conditions:</span>';
                conditions.forEach(function(cond) {
                  var cs = typeof cond === 'string' ? cond : JSON.stringify(cond);
                  html += '<div style="padding-left:12px;color:var(--panel-fg)">' + esc(cs) + '</div>';
                });
                html += '</div>';
              }
              var cmdStrings = cmds.map(function(cmd) { return typeof cmd === 'string' ? cmd : (cmd && cmd.command ? cmd.command : JSON.stringify(cmd)); });
              window.__autoCache[detailId] = { mac: mac, name: a.name || '(unnamed)', cmds: cmdStrings };
              if (cmdStrings.length > 0) {
                html += '<div><span style="color:var(--muted)">Commands:</span>';
                cmdStrings.forEach(function(s) {
                  html += '<div style="padding:2px 0 2px 12px;color:var(--panel-fg);font-family:monospace;font-size:0.9em">' + esc(s) + '</div>';
                });
                html += '</div>';
              }
              html += '<button class="btn auto-run-btn" data-detail-id="' + detailId + '" style="margin-top:8px;width:100%;font-size:0.85em">Run on Device</button>';
              html += '</div>';
              html += '</div>';
            });
            html += '</div>';
            listDiv.innerHTML = html;
            var entries = listDiv.querySelectorAll('.auto-entry');
            entries.forEach(function(entryEl) {
              entryEl.addEventListener('click', function() {
                var did = entryEl.getAttribute('data-detail-id');
                if (did) window.toggleAutoDetail(did);
              });
            });
            var runButtons = listDiv.querySelectorAll('.auto-run-btn');
            runButtons.forEach(function(btnEl) {
              btnEl.addEventListener('click', function(event) {
                event.stopPropagation();
                var did = btnEl.getAttribute('data-detail-id');
                if (did) window.runRemoteAutomation(did, btnEl);
              });
            });
          } catch(e) {
            listDiv.innerHTML = '<div style="color:var(--danger);padding:12px">Parse error: ' + esc(e.message) + '</div>';
          }
        })
        .catch(function(e) {
          listDiv.innerHTML = '<div style="color:var(--muted);padding:20px;text-align:center">No automations file cached. Click "Load Automations" to request from device.</div>';
        });
    };
    window.toggleAutoDetail = function(detailId) {
      var d = document.getElementById(detailId);
      if (d) d.style.display = d.style.display === 'none' ? 'block' : 'none';
    };
    window.runRemoteAutomation = function(detailId, btn) {
      var entry = window.__autoCache && window.__autoCache[detailId];
      if (!entry || !entry.cmds || entry.cmds.length === 0) { alert('No commands to run'); return; }
      var mac = entry.mac;
      var u = (document.getElementById('au-' + mac) || {}).value || '';
      var p = (document.getElementById('ap-' + mac) || {}).value || '';
      if (!u || !p) { alert('Enter username and password at the top of the Automations tab first.'); return; }
      var cmds = entry.cmds;
      var total = cmds.length;
      var origText = btn.textContent;
      btn.disabled = true;
      btn.textContent = 'Running 1/' + total + '...';
      var i = 0;
      function next() {
        if (i >= total) {
          btn.textContent = 'Done!';
          setTimeout(function() { btn.textContent = origText; btn.disabled = false; }, 2000);
          return;
        }
        var cmd = 'espnowremote ' + mac + ' ' + u + ' ' + p + ' ' + cmds[i];
        btn.textContent = 'Running ' + (i + 1) + '/' + total + '...';
        fetch('/api/cli', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'cmd=' + encodeURIComponent(cmd) })
          .then(function(r) { return r.text(); })
          .then(function() { i++; next(); })
          .catch(function(e) {
            btn.textContent = 'Error at cmd ' + (i + 1);
            btn.style.background = '#dc3545';
            setTimeout(function() { btn.textContent = origText; btn.style.background = ''; btn.disabled = false; }, 3000);
          });
      }
      next();
    };
    window.__sensorList = window.__sensorList || ['thermal','tof','imu','gps','gamepad','fmradio','rtc','presence'];
    window.sensorActiveState = window.sensorActiveState || {};
    window.sensorPendingState = window.sensorPendingState || {};
    window.updateSensorStatus = function(mac) {
      var statusDiv = document.getElementById('sensor-status-' + mac);
      if (!statusDiv) return;
      var pendingCount = Object.keys(window.sensorPendingState[mac] || {}).length;
      statusDiv.textContent = pendingCount ? ('Pending changes: ' + pendingCount) : 'Select sensors and click Apply Streaming.';
    };
    window.updateSensorPill = function(mac, sensor) {
      var pill = document.getElementById('sensor-pill-' + sensor + '-' + mac);
      if (!pill) return;
      pill.classList.remove('sensor-active','sensor-pending');
      var activeState = window.sensorActiveState[mac] && window.sensorActiveState[mac][sensor] === 'on';
      var pendingMap = window.sensorPendingState[mac] || {};
      if (activeState) pill.classList.add('sensor-active');
      if (pendingMap.hasOwnProperty(sensor)) pill.classList.add('sensor-pending');
    };
    window.ensureSensorState = function(mac) {
      if (!window.sensorActiveState[mac]) window.sensorActiveState[mac] = {};
      if (!window.sensorPendingState[mac]) window.sensorPendingState[mac] = {};
      window.__sensorList.forEach(function(sensor){ window.updateSensorPill(mac, sensor); });
      window.updateSensorStatus(mac);
    };
    window.toggleSensorSelection = function(mac, sensor) {
      window.ensureSensorState(mac);
      var pendingMap = window.sensorPendingState[mac];
      var active = (window.sensorActiveState[mac][sensor] || 'off');
      if (pendingMap.hasOwnProperty(sensor)) {
        delete pendingMap[sensor];
      } else {
        pendingMap[sensor] = active === 'on' ? 'off' : 'on';
      }
      window.updateSensorPill(mac, sensor);
      window.updateSensorStatus(mac);
    };
    window.applySensorStreaming = function(mac) {
      window.ensureSensorState(mac);
      var pendingMap = window.sensorPendingState[mac];
      var entries = Object.entries(pendingMap);
      var statusDiv = document.getElementById('sensor-status-' + mac);
      var u = (document.getElementById('su-' + mac) || {}).value || '';
      var p = (document.getElementById('sp-' + mac) || {}).value || '';
      if (!u || !p) { alert('Enter username and password in Sensor Streaming tab'); return; }
      if (entries.length === 0) {
        if (statusDiv) statusDiv.textContent = 'No pending changes to apply';
        return;
      }
      var idx = 0;
      var results = [];
      function processNext() {
        if (idx >= entries.length) {
          window.updateSensorStatus(mac);
          if (statusDiv) statusDiv.textContent = results.join(' | ');
          return;
        }
        var sensor = entries[idx][0];
        var desired = entries[idx][1];
        idx++;
        if (statusDiv) statusDiv.textContent = 'Applying ' + sensor + ' ' + desired + '...';
        var cmd = 'espnowremote ' + mac + ' ' + u + ' ' + p + ' espnowsensorstream ' + sensor + ' ' + desired;
        fetch('/api/cli', {
          method:'POST',
          headers:{'Content-Type':'application/x-www-form-urlencoded'},
          body:'cmd=' + encodeURIComponent(cmd)
        })
        .then(function(r){ return r.text(); })
        .then(function(resp){
          window.sensorActiveState[mac][sensor] = desired;
          delete pendingMap[sensor];
          window.updateSensorPill(mac, sensor);
          results.push(sensor + ' ' + desired + ' ✓');
          processNext();
        })
        .catch(function(e){
          results.push(sensor + ' error: ' + e.message);
          delete pendingMap[sensor];
          window.updateSensorPill(mac, sensor);
          processNext();
        });
      }
      processNext();
    };
    console.log('[ESP-NOW] Chunk 3: helpers ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 3 error:', e); }
})();
</script>
<script>
(function() {
  try {
    console.log('[ESP-NOW] Chunk 4: Messaging functions start');
    window.sendMessage = function(mac, message) {
      // Fetch current mode dynamically to ensure we use the correct command
      console.log('[ESP-NOW] sendMessage: Fetching current mode...');
      fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent('espnowmode') })
        .then(r => r.text())
        .then(modeOut => {
          const isMesh = (modeOut || '').toLowerCase().indexOf('mesh') >= 0;
          console.log('[ESP-NOW] sendMessage: Current mode:', isMesh ? 'MESH' : 'DIRECT');
          var cmd = 'espnowsend ' + mac + ' ' + message;
          console.log('[ESP-NOW] sendMessage: Command:', cmd);
          return fetch('/api/cli', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'cmd=' + encodeURIComponent(cmd) });
        })
        .then(response => response.text())
        .then(text => {
          addMessageToLog('SENT', 'To ' + mac + ': ' + message);
          addMessageToLog('RESULT', text);
          if (text && text.indexOf('Message sent') >= 0) {
            document.getElementById('send-message').value = '';
          }
        })
        .catch(error => {
          addMessageToLog('ERROR', 'Send error: ' + error);
        });
    };
    window.addMessageToLog = function(type, message) {
      const log = document.getElementById('message-log');
      if (!log) return;
      const timestamp = new Date().toLocaleTimeString();
      let className = 'message-item';
      if (type === 'SENT' || type === 'BROADCAST') className += ' message-sent';
      else if (type === 'RECEIVED') className += ' message-received';
      else if (type === 'ERROR') className += ' message-error';
      const messageDiv = document.createElement('div');
      messageDiv.className = className;
      messageDiv.textContent = '[' + timestamp + '] ' + type + ': ' + message;
      if (log.children.length === 1 && log.children[0].textContent.includes('Message log will appear')) {
        log.innerHTML = '';
      }
      log.appendChild(messageDiv);
      window.messageCount++;
      if (window.messageCount > window.maxMessages) {
        log.removeChild(log.firstChild);
        window.messageCount--;
      }
      log.scrollTop = log.scrollHeight;
    };
    console.log('[ESP-NOW] Chunk 4: Messaging functions ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 4 error:', e); }
})();
</script>
<script>
(function() {
  try {
    console.log('[ESP-NOW] Chunk 4b: Mesh status functions start');
    window.meshStatusPollInterval = null;
    window.meshStatusPollInFlight = false;
    window.startMeshStatusPolling = function() {
      if (window.meshStatusPollInterval) {
        clearInterval(window.meshStatusPollInterval);
      }
      window.meshStatusPollInterval = setInterval(function() {
        if (document.hidden) return;
        if (window.meshStatusPollInFlight) return;  // skip if previous request still pending
        if (window.espnowIsMesh && typeof window.refreshMeshStatus === 'function') {
          window.refreshMeshStatus();
        }
      }, 10000);  // Refresh every 10 seconds
    };
    window.stopMeshStatusPolling = function() {
      console.log('[ESP-NOW] Stopping mesh status polling...');
      if (window.meshStatusPollInterval) {
        clearInterval(window.meshStatusPollInterval);
        window.meshStatusPollInterval = null;
      }
    };
    window.refreshMeshStatus = function(preloadedText) {
      if (!preloadedText) window.meshStatusPollInFlight = true;
      function applyMeshStatus(output) {
        try {
          var data = JSON.parse(output);
          if (data.error) {
            console.warn('[ESP-NOW] meshstatus error:', data.error);
            return;
          }
          // Store mesh health data globally and trigger unified re-render
          window.__meshPeers = data.peers || [];
          window.__meshUnpaired = data.unpaired || [];
          // Show mesh views card when we have mesh data
          var viewsCard = document.getElementById('mesh-views-card');
          if (viewsCard) viewsCard.style.display = 'block';
          window.renderUnifiedDeviceList();
        } catch(e) {
          console.error('[ESP-NOW] Error parsing mesh status:', e);
        }
      }
      if (preloadedText !== undefined) {
        applyMeshStatus(preloadedText);
        return;
      }
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnowmeshstatus')
      })
      .then(response => response.text())
      .then(function(text) { window.meshStatusPollInFlight = false; applyMeshStatus(text); })
      .catch(error => {
        window.meshStatusPollInFlight = false;
        console.error('[ESP-NOW] Mesh status fetch error:', error);
      });

      // Topology view is refreshed only on explicit user action (tab switch or Discover button),
      // not on the 3-second mesh status poll — that would spam the CLI with toporesults requests.
    };
    
    // Pair an unpaired device
    window.pairUnpairedDevice = async function(mac, name) {
      console.log('[ESP-NOW] Pairing device:', mac, name);
      if (!await hwConfirm('Pair device "' + name + '" (' + mac + ')?')) {
        return;
      }
      
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnowpair ' + mac + ' ' + name)
      })
      .then(response => response.text())
      .then(output => {
        console.log('[ESP-NOW] Pair response:', output);
        alert(output);
        // Refresh mesh status to update the display
        if (typeof window.refreshMeshStatus === 'function') {
          window.refreshMeshStatus();
        }
      })
      .catch(error => {
        console.error('[ESP-NOW] Pair error:', error);
        alert('Error pairing device: ' + error);
      });
    };
    
    // View switching
    window.switchMeshView = function(view) {
      var views = ['topology', 'graph'];
      var buttons = ['btn-view-topology', 'btn-view-graph'];
      
      views.forEach(function(v, idx) {
        var elem = document.getElementById('mesh-view-' + v);
        var btn = document.getElementById(buttons[idx]);
        if (v === view) {
          if (elem) elem.style.display = 'block';
          if (btn) btn.style.background = 'var(--crumb-bg)';
        } else {
          if (elem) elem.style.display = 'none';
          if (btn) btn.style.background = '';
        }
      });
      
      // Trigger refresh for the selected view
      if (view === 'topology') {
        window.refreshTopologyView();
      } else if (view === 'graph') {
        window.refreshGraphView();
      }
    };
    
    // Refresh full topology view
    window.refreshTopologyView = function() {
      console.log('[ESP-NOW] Refreshing topology view...');
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnowtoporesults')
      })
      .then(response => response.text())
      .then(output => {
        console.log('[ESP-NOW] Topology results:', output);
        var container = document.getElementById('mesh-topology-view');
        if (!container) return;
        
        // Check if we have topology data
        if (!output || output.indexOf('No topology results') >= 0 || output.indexOf('ERROR') >= 0) {
          container.innerHTML = '<div style="color:var(--panel-fg);text-align:center;padding:20px;">No topology data available.<br>Click "Discover Topology" in the Mesh Role Configuration section.</div>';
          return;
        }
        
        // Parse and format topology results
        var html = '<div style="background:var(--crumb-bg);padding:15px;border-radius:8px;color:var(--panel-fg);overflow-x:auto;">';
        html += '<div style="font-weight:bold;font-size:1.1em;margin-bottom:10px;color:var(--panel-fg);">Complete Mesh Topology</div>';
        
        // Extract device sections
        var lines = output.split('\n');
        var currentDevice = null;
        var deviceData = {};
        
        for (var i = 0; i < lines.length; i++) {
          var line = lines[i].trim();
          
          // Match device header: "name (MAC):" format from toporesults output
          var deviceMatch = line.match(/^(.+?)\s*\(([A-Fa-f0-9:]{17})\)\s*:$/);
          if (deviceMatch) {
            currentDevice = {
              name: deviceMatch[1],
              mac: deviceMatch[2],
              peers: [],
              peerCount: 0,
              path: ''
            };
            continue;
          }
          
          // Match path: "Path: ..."
          var pathMatch = line.match(/Path:\s*(.+)/i);
          if (pathMatch && currentDevice) {
            currentDevice.path = pathMatch[1];
            continue;
          }
          
          // Match peer count: "Peers: N"
          var peerCountMatch = line.match(/Peers:\s*(\d+)/i);
          if (peerCountMatch && currentDevice) {
            currentDevice.peerCount = parseInt(peerCountMatch[1]);
            continue;
          }
          
          // Match peer entry: "→ name (MAC)"
          var peerMatch = line.match(/→\s*(.+?)\s*\(([a-f0-9:]+)\)/i);
          if (peerMatch && currentDevice) {
            var peerInfo = {
              name: peerMatch[1],
              mac: peerMatch[2]
            };
            
            // Next line may have RSSI or heartbeat info
            if (i + 1 < lines.length) {
              var nextLine = lines[i + 1].trim();
              var rssiMatch = nextLine.match(/RSSI:\s*(-?\d+)\s*dBm/i);
              var hbMatch = nextLine.match(/Heartbeats:\s*(\d+),\s*Last seen:\s*(\d+)s ago/i);
              if (rssiMatch) {
                peerInfo.rssi = parseInt(rssiMatch[1]);
                i++; // Skip next line since we processed it
              } else if (hbMatch) {
                peerInfo.heartbeats = parseInt(hbMatch[1]);
                peerInfo.lastSeen = parseInt(hbMatch[2]);
                i++; // Skip next line since we processed it
              }
            }
            
            currentDevice.peers.push(peerInfo);
            continue;
          }
          
          // Empty line or separator - save current device
          if (line === '' && currentDevice && currentDevice.peers.length > 0) {
            deviceData[currentDevice.mac] = currentDevice;
            currentDevice = null;
          }
        }
        
        // Save last device if exists
        if (currentDevice && currentDevice.peers.length > 0) {
          deviceData[currentDevice.mac] = currentDevice;
        }
        
        // Render devices
        var deviceCount = Object.keys(deviceData).length;
        if (deviceCount === 0) {
          html += '<div style="color:var(--panel-fg);text-align:center;">No devices found in topology</div>';
        } else {
          html += '<div style="margin-bottom:10px;color:var(--panel-fg);">Found ' + deviceCount + ' device(s) in mesh</div>';
          
          for (var mac in deviceData) {
            var dev = deviceData[mac];
            // Calculate hop count from path
            var hopCount = dev.path ? (dev.path.split('→').length - 1) : 0;
            var indentPx = hopCount * 20;
            
            html += '<div style="background:var(--panel-bg);border:2px solid var(--success);border-radius:8px;padding:12px;margin-bottom:12px;margin-left:' + indentPx + 'px;">';
            html += '<div style="font-weight:bold;font-size:1.05em;color:var(--success);margin-bottom:8px;">' + dev.name + '</div>';
            html += '<div style="font-size:0.85em;color:var(--panel-fg);margin-bottom:4px;">' + dev.mac + ' • ' + dev.peerCount + ' peer(s)</div>';
            if (dev.path) {
              html += '<div style="font-size:0.8em;color:var(--link);margin-bottom:8px;">Path: ' + dev.path + ' (' + hopCount + ' hop' + (hopCount !== 1 ? 's' : '') + ')</div>';
            }
            
            if (dev.peers.length > 0) {
              html += '<div style="padding-left:12px;margin-left:8px;">'; 
              for (var j = 0; j < dev.peers.length; j++) {
                var peer = dev.peers[j];
                var signalColor = 'var(--success)';
                var detailText = '';
                if (typeof peer.rssi === 'number') {
                  signalColor = peer.rssi > -60 ? 'var(--success)' : (peer.rssi > -75 ? 'var(--warning)' : 'var(--danger)');
                  detailText = 'RSSI: <span style="color:' + signalColor + ';">' + peer.rssi + ' dBm</span>';
                } else if (typeof peer.lastSeen === 'number') {
                  signalColor = peer.lastSeen < 10 ? 'var(--success)' : (peer.lastSeen < 20 ? 'var(--warning)' : 'var(--danger)');
                  detailText = '<span style="color:' + signalColor + ';">Last seen: ' + peer.lastSeen + 's ago</span> • Heartbeats: ' + peer.heartbeats;
                }
                var statusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:' + signalColor + ';margin-right:6px;"></span>';
                
                html += '<div style="padding:6px 0;border-bottom:1px solid var(--border);">';
                html += '<div style="font-weight:500;color:var(--panel-fg);">' + statusDot + peer.name + '</div>';
                html += '<div style="font-size:0.8em;color:var(--panel-fg);margin-top:2px;">';
                html += peer.mac;
                if (detailText) html += ' • ' + detailText;
                html += '</div></div>';
              }
              html += '</div>';
            }
            
            html += '</div>';
          }
        }
        
        html += '</div>';
        container.innerHTML = html;
      })
      .catch(error => {
        console.error('[ESP-NOW] Topology fetch error:', error);
        var container = document.getElementById('mesh-topology-view');
        if (container) {
          container.innerHTML = '<div style="color:var(--danger);text-align:center;">Error loading topology: ' + error + '</div>';
        }
      });
    };
    
    // Refresh network graph view
    window.refreshGraphView = function() {
      console.log('[ESP-NOW] Refreshing graph view...');
      var container = document.getElementById('mesh-graph-view');
      if (!container) return;
      
      // Fetch both direct peers and current device status
      Promise.all([
        fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('espnowmeshstatus')
        }).then(r => r.text()),
        fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('espnowstatus')
        }).then(r => r.text())
      ])
      .then(function(results) {
        var meshStatus = results[0];
        var deviceStatus = results[1];
        
        // Extract current device name and MAC
        var deviceName = 'THIS DEVICE';
        var deviceMac = '';
        var macMatch = deviceStatus.match(/MAC:\s*([A-Fa-f0-9:]{17})/i);
        if (macMatch) {
          deviceMac = macMatch[1];
          // Try to get device name from page title
          var titleElem = document.querySelector('h1');
          if (titleElem && titleElem.textContent) {
            deviceName = titleElem.textContent.trim();
          }
        }
        
        // Build network graph
        var html = '<div style="background:var(--crumb-bg);padding:15px;border-radius:8px;color:var(--panel-fg);overflow-x:auto;">';
        html += '<div style="font-weight:bold;font-size:1.1em;margin-bottom:10px;color:var(--panel-fg);">Network Connection Graph</div>';
        html += '<div style="font-family:monospace;font-size:0.9em;line-height:1.8;background:var(--panel-bg);padding:15px;border-radius:4px;overflow-x:auto;white-space:pre;">';
        
        // Show current device
        var selfDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--success);margin-right:6px;"></span>';
        html += '<div style="color:var(--success);font-weight:bold;">' + selfDot + deviceName;
        if (deviceMac) {
          html += ' (' + deviceMac + ')';
        }
        html += '</div>';
        
        // Parse mesh status for current device's peers
        try {
          var meshData = JSON.parse(meshStatus);
          if (meshData.peers && meshData.peers.length > 0) {
            for (var i = 0; i < meshData.peers.length; i++) {
              var peer = meshData.peers[i];
              var statusColor = peer.alive ? 'var(--success)' : 'var(--danger)';
              var statusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:' + statusColor + ';margin-right:6px;"></span>';
              html += '<div style="margin-left:20px;color:var(--panel-fg);">├─ ' + statusDot + peer.name + ' (' + peer.mac + ')</div>';
            }
          } else {
            html += '<div style="margin-left:20px;color:var(--muted);">└─ No direct peers</div>';
          }
        } catch(e) {
          console.error('[ESP-NOW] Error parsing mesh status for graph:', e);
        }
        
        html += '</div></div>';
        container.innerHTML = html;
      })
      .catch(error => {
        console.error('[ESP-NOW] Graph fetch error:', error);
        container.innerHTML = '<div style="color:var(--danger);text-align:center;">Error loading graph: ' + error + '</div>';
      });
    };
    
    // Auto-topology toggle
    window.autoTopoInterval = null;
    window.toggleAutoTopology = function() {
      var btn = document.getElementById('btn-auto-topology');
      if (!btn) return;
      
      if (window.autoTopoInterval) {
        clearInterval(window.autoTopoInterval);
        window.autoTopoInterval = null;
        btn.textContent = 'Auto-Discover: OFF';
        btn.style.background = '';
        btn.style.color = '';
      } else {
        window.autoTopoInterval = setInterval(function() {
          fetch('/api/cli', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'cmd=' + encodeURIComponent('espnowmeshtopo')
          });
        }, 30000); // Every 30 seconds
        btn.textContent = 'Auto-Discover: ON';
        btn.style.background = '';
        btn.style.color = '#28a745';
        btn.style.fontWeight = 'bold';
      }
    };
    
    console.log('[ESP-NOW] Chunk 4b: Mesh status functions ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 4b error:', e); }
})();
</script>
<script>
(function() {
  try {
    console.log('[ESP-NOW] Chunk 4c: Mesh role functions start');
    window.refreshMeshRole = function(preloadedText) {
      console.log('[ESP-NOW] Refreshing mesh role...');
      function applyMeshRole(output) {
        console.log('[ESP-NOW] Mesh role response:', output);
        var statusDiv = document.getElementById('mesh-role-status');
        if (!statusDiv) return;
        var roleMatch = output.match(/Mesh role:\s*(\w+)/i);
        var masterMatch = output.match(/Master MAC:\s*([A-Fa-f0-9:]{17})/i);
        var backupEnabledMatch = output.match(/Backup enabled:\s*(yes|no)/i);
        var backupMatch = output.match(/Backup MAC:\s*([A-Fa-f0-9:]{17})/i);
        var role = roleMatch ? roleMatch[1] : 'unknown';
        var masterMAC = masterMatch ? masterMatch[1] : 'Not set';
        var backupEnabled = backupEnabledMatch ? backupEnabledMatch[1].toLowerCase() === 'yes' : false;
        var backupMAC = backupMatch ? backupMatch[1] : 'Not set';
        var html = '<strong>Current Role:</strong> ' + role.charAt(0).toUpperCase() + role.slice(1);
        html += '<br><strong>Master MAC:</strong> ' + masterMAC;
        if (backupEnabled) {
          html += '<br><strong>Backup MAC:</strong> ' + backupMAC;
        }
        statusDiv.innerHTML = html;
        var backupCheckbox = document.getElementById('backup-master-enabled');
        if (backupCheckbox) backupCheckbox.checked = backupEnabled;
        var masterGroup = document.getElementById('master-mac')?.parentElement;
        var backupMacGroup = document.getElementById('backup-mac-group');
        if (role.toLowerCase() === 'master') {
          if (masterGroup) masterGroup.style.display = 'none';
          if (backupMacGroup) backupMacGroup.style.display = backupEnabled ? 'flex' : 'none';
          if (backupEnabled && backupMAC !== 'Not set') {
            var backupInput = document.getElementById('backup-mac');
            if (backupInput) backupInput.value = backupMAC;
          }
        } else if (role.toLowerCase() === 'backup') {
          if (masterGroup) masterGroup.style.display = 'flex';
          if (backupMacGroup) backupMacGroup.style.display = 'none';
          if (masterMAC !== 'Not set') {
            var masterInput = document.getElementById('master-mac');
            if (masterInput) masterInput.value = masterMAC;
          }
        } else {
          if (masterGroup) masterGroup.style.display = 'flex';
          if (backupMacGroup) backupMacGroup.style.display = backupEnabled ? 'flex' : 'none';
          if (masterMAC !== 'Not set') {
            var masterInput = document.getElementById('master-mac');
            if (masterInput) masterInput.value = masterMAC;
          }
          if (backupEnabled && backupMAC !== 'Not set') {
            var backupInput = document.getElementById('backup-mac');
            if (backupInput) backupInput.value = backupMAC;
          }
        }
      }
      if (preloadedText !== undefined) {
        applyMeshRole(preloadedText);
        return;
      }
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnowmeshrole')
      })
      .then(response => response.text())
      .then(applyMeshRole)
      .catch(error => {
        console.error('[ESP-NOW] Mesh role fetch error:', error);
        var statusDiv = document.getElementById('mesh-role-status');
        if (statusDiv) statusDiv.innerHTML = '<span style="color:var(--danger);">Error: ' + error + '</span>';
      });
    };
    
    window.setMeshRole = function(role) {
      console.log('[ESP-NOW] Setting mesh role to:', role);
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnowmeshrole ' + role)
      })
      .then(response => response.text())
      .then(output => {
        console.log('[ESP-NOW] Set role response:', output);
        alert(output);
        window.refreshMeshRole();
      })
      .catch(error => {
        alert('Error setting role: ' + error);
      });
    };
    
    window.setMasterMAC = function() {
      var mac = (document.getElementById('master-mac') || {}).value || '';
      if (!mac || mac.length !== 17) {
        alert('Enter a valid MAC address (XX:XX:XX:XX:XX:XX)');
        return;
      }
      console.log('[ESP-NOW] Setting master MAC to:', mac);
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnowmeshmaster ' + mac)
      })
      .then(response => response.text())
      .then(output => {
        console.log('[ESP-NOW] Set master MAC response:', output);
        alert('Master MAC set: ' + output);
        window.refreshMeshRole();
      })
      .catch(error => {
        alert('Error setting master MAC: ' + error);
      });
    };
    
    window.setBackupMAC = function() {
      var mac = (document.getElementById('backup-mac') || {}).value || '';
      if (!mac || mac.length !== 17) {
        alert('Enter a valid MAC address (XX:XX:XX:XX:XX:XX)');
        return;
      }
      console.log('[ESP-NOW] Setting backup MAC to:', mac);
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnowmeshbackup ' + mac)
      })
      .then(response => response.text())
      .then(output => {
        console.log('[ESP-NOW] Set backup MAC response:', output);
        alert('Backup MAC set: ' + output);
        window.refreshMeshRole();
      })
      .catch(error => {
        alert('Error setting backup MAC: ' + error);
      });
    };
    
    window.toggleBackupMaster = function(enabled) {
      console.log('[ESP-NOW] Toggling backup master:', enabled);
      var backupMacGroup = document.getElementById('backup-mac-group');
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnowbackupenable ' + (enabled ? 'on' : 'off'))
      })
      .then(response => response.text())
      .then(output => {
        console.log('[ESP-NOW] Backup enable response:', output);
        if (backupMacGroup) backupMacGroup.style.display = enabled ? 'flex' : 'none';
        window.refreshMeshRole();
      })
      .catch(error => {
        console.error('[ESP-NOW] Error toggling backup master:', error);
        var cb = document.getElementById('backup-master-enabled');
        if (cb) cb.checked = !enabled;
      });
    };
    
    window.__topoDiscoveryInterval = null;
    window.discoverTopology = function() {
      console.log('[ESP-NOW] Discovering topology...');
      var topoDiv = document.getElementById('mesh-topology-data');
      var resultsDiv = document.getElementById('topology-results');
      if (topoDiv) topoDiv.style.display = 'block';
      if (resultsDiv) resultsDiv.innerHTML = 'Discovering topology... (this may take up to 10 seconds)';
      
      // Clear any existing polling interval
      if (window.__topoDiscoveryInterval) {
        clearInterval(window.__topoDiscoveryInterval);
        window.__topoDiscoveryInterval = null;
      }
      
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnowmeshtopo')
      })
      .then(response => response.text())
      .then(output => {
        console.log('[ESP-NOW] Topology discovery response:', output);
        if (resultsDiv) {
          resultsDiv.innerHTML = '<pre style="margin:0;white-space:pre-wrap;color:var(--panel-fg);">' + output + '</pre>';
        }
        
        // Bail early if the command itself failed
        if (!output || output.indexOf('ERROR') >= 0 || output.indexOf('No topology') >= 0 || output.indexOf('not enabled') >= 0) {
          return;
        }
        
        // Poll for topology results
        var pollCount = 0;
        window.__topoDiscoveryInterval = setInterval(function() {
          pollCount++;
          fetch('/api/cli', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'cmd=' + encodeURIComponent('espnowtoporesults')
          })
          .then(r => r.text())
          .then(results => {
            console.log('[ESP-NOW] Topology results poll ' + pollCount + ':', results);
            var hasResults = results && results.indexOf('Responses received:') >= 0 && results.indexOf('Responses received: 0') < 0;
            if (hasResults && resultsDiv) {
              resultsDiv.innerHTML = '<pre style="margin:0;white-space:pre-wrap;color:var(--panel-fg);">' + results + '</pre>';
            }
            if (pollCount >= 5 || hasResults) {
              clearInterval(window.__topoDiscoveryInterval);
              window.__topoDiscoveryInterval = null;
              console.log('[ESP-NOW] Topology polling stopped (count=' + pollCount + ', hasResults=' + hasResults + ')');
            }
          })
          .catch(function() {
            clearInterval(window.__topoDiscoveryInterval);
            window.__topoDiscoveryInterval = null;
          });
        }, 2000);
      })
      .catch(error => {
        console.error('[ESP-NOW] Topology discovery error:', error);
        if (resultsDiv) resultsDiv.innerHTML = '<span style="color:var(--danger);">Error: ' + error + '</span>';
      });
    };
    
    console.log('[ESP-NOW] Chunk 4c: Mesh role functions ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 4c error:', e); }
})();
</script>
<script>
(function() {
  try {
    console.log('[ESP-NOW] Chunk 5: Button handlers start');
    window.setupButtonHandlers = function() {
      var _on = function(id, evt, fn){ var el = document.getElementById(id); if (el) el.addEventListener(evt, fn); };
      document.getElementById('btn-espnow-init').addEventListener('click', function() {
        // Check if first-time setup is needed
        fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('espnowsetname')
        })
        .then(response => response.text())
        .then(text => {
          // If device name is not set, show setup modal
          if (text.indexOf('(not set)') >= 0) {
            document.getElementById('setup-modal').classList.add('show');
            document.getElementById('setup-device-name').focus();
          } else {
            // Name is set, proceed with init
            return fetch('/api/cli', {
              method: 'POST',
              headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
              body: 'cmd=' + encodeURIComponent('openespnow')
            })
            .then(response => response.text())
            .then(text => {
              document.getElementById('espnow-status-data').textContent = text;
              refreshStatus();
            });
          }
        })
        .catch(error => {
          document.getElementById('espnow-status-data').textContent = 'Error: ' + error;
        });
      });
      document.getElementById('btn-espnow-disable').addEventListener('click', async function() {
        if (!await hwConfirm('Disable ESP-NOW? This will stop all ESP-NOW communication. Memory will remain allocated until reboot.')) {
          return;
        }
        fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('closeespnow')
        })
        .then(response => response.text())
        .then(text => {
          document.getElementById('espnow-status-data').textContent = text;
          refreshStatus();
        })
        .catch(error => {
          document.getElementById('espnow-status-data').textContent = 'Error: ' + error;
        });
      });
      document.getElementById('btn-espnow-refresh').addEventListener('click', refreshStatus);
      document.getElementById('btn-espnow-toggle-mode').addEventListener('click', function() {
        /* Fetch current mode, then toggle to the other */
        fetch('/api/cli', { method:'POST', headers:{ 'Content-Type':'application/x-www-form-urlencoded' }, body: 'cmd=' + encodeURIComponent('espnowmode') })
          .then(r=>r.text())
          .then(curr => {
            var isMesh = (curr || '').toLowerCase().indexOf('mesh') >= 0;
            var next = isMesh ? 'direct' : 'mesh';
            // Update global flag immediately based on what we're switching TO
            window.espnowIsMesh = (next === 'mesh');
            return fetch('/api/cli', { method:'POST', headers:{ 'Content-Type':'application/x-www-form-urlencoded' }, body: 'cmd=' + encodeURIComponent('espnowmode ' + next) });
          })
          .then(r=>r.text())
          .then(t=>{ try { /* optional toast */ } catch(_) {}; refreshStatus(); })
          .catch(e=>{ try { alert('Error: ' + e.message); } catch(_) {}; });
      });
      document.getElementById('btn-pair-device').addEventListener('click', function() {
        const mac = document.getElementById('pair-mac').value.trim();
        const name = document.getElementById('pair-name').value.trim();
        if (!mac || !name) {
          alert('Please enter both MAC address and device name');
          return;
        }
        fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('espnowpair ' + mac + ' ' + name)
        })
        .then(response => response.text())
        .then(text => {
          addMessageToLog('PAIR', text);
          if (text && text.indexOf('paired successfully') >= 0) {
            document.getElementById('pair-mac').value = '';
            document.getElementById('pair-name').value = '';
            listDevices();
          }
        })
        .catch(error => {
          addMessageToLog('ERROR', 'Pair error: ' + error);
        });
      });
      document.getElementById('btn-pair-secure').addEventListener('click', function() {
        const mac = document.getElementById('pair-mac').value.trim();
        const name = document.getElementById('pair-name').value.trim();
        if (!mac || !name) {
          alert('Please enter both MAC address and device name');
          return;
        }
        fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('espnowpairsecure ' + mac + ' ' + name)
        })
        .then(response => response.text())
        .then(text => {
          addMessageToLog('PAIR_SECURE', text);
          if (text && text.indexOf('paired successfully') >= 0) {
            document.getElementById('pair-mac').value = '';
            document.getElementById('pair-name').value = '';
            listDevices();
          }
        })
        .catch(error => {
          addMessageToLog('ERROR', 'Secure pair error: ' + error);
        });
      });
      document.getElementById('btn-refresh-mesh').addEventListener('click', function() {
        console.log('[ESP-NOW] Refresh mesh button clicked');
        if (typeof window.refreshMeshStatus === 'function') {
          window.refreshMeshStatus();
        }
      });
      document.getElementById('btn-auto-topology').addEventListener('click', function() {
        if (typeof window.toggleAutoTopology === 'function') {
          window.toggleAutoTopology();
        }
      });
      document.getElementById('btn-view-topology').addEventListener('click', function() {
        if (typeof window.switchMeshView === 'function') {
          window.switchMeshView('topology');
        }
      });
      document.getElementById('btn-view-graph').addEventListener('click', function() {
        if (typeof window.switchMeshView === 'function') {
          window.switchMeshView('graph');
        }
      });
      /* Mesh role button handlers */
      _on('btn-role-worker','click', function() { window.setMeshRole('worker'); });
      _on('btn-role-master','click', function() { window.setMeshRole('master'); });
      _on('btn-role-backup','click', function() { window.setMeshRole('backup'); });
      _on('btn-set-master-mac','click', function() { window.setMasterMAC(); });
      _on('btn-set-backup-mac','click', function() { window.setBackupMAC(); });
      _on('btn-mesh-topo','click', function() { window.discoverTopology(); });
      _on('backup-master-enabled','change', function() { window.toggleBackupMaster(this.checked); });
      /* Smart home metadata button handlers */
      _on('btn-set-friendly','click', function() {
        const val = document.getElementById('friendly-name').value;
        fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd='+encodeURIComponent('espnowfriendlyname "'+val+'"') })
          .then(r=>r.text()).then(t=>{ var el=document.getElementById('smarthome-status'); if(el)el.textContent=t; if(typeof window.loadSmartHomeMetadata==='function')window.loadSmartHomeMetadata(); });
      });
      _on('btn-set-room','click', function() {
        const val = document.getElementById('room-name').value;
        fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd='+encodeURIComponent('espnowroom "'+val+'"') })
          .then(r=>r.text()).then(t=>{ var el=document.getElementById('smarthome-status'); if(el)el.textContent=t; if(typeof window.loadSmartHomeMetadata==='function')window.loadSmartHomeMetadata(); });
      });
      _on('btn-set-zone','click', function() {
        const val = document.getElementById('zone-name').value;
        fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd='+encodeURIComponent('espnowzone "'+val+'"') })
          .then(r=>r.text()).then(t=>{ var el=document.getElementById('smarthome-status'); if(el)el.textContent=t; if(typeof window.loadSmartHomeMetadata==='function')window.loadSmartHomeMetadata(); });
      });
      _on('btn-set-tags','click', function() {
        const val = document.getElementById('tags-input').value;
        fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd='+encodeURIComponent('espnowtags "'+val+'"') })
          .then(r=>r.text()).then(t=>{ var el=document.getElementById('smarthome-status'); if(el)el.textContent=t; if(typeof window.loadSmartHomeMetadata==='function')window.loadSmartHomeMetadata(); });
      });
      _on('stationary-checkbox','change', function() {
        const checked = document.getElementById('stationary-checkbox').checked;
        fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd='+encodeURIComponent('espnowstationary '+(checked?'on':'off')) })
          .then(r=>r.text()).then(t=>{ var el=document.getElementById('smarthome-status'); if(el)el.textContent=t; if(typeof window.loadSmartHomeMetadata==='function')window.loadSmartHomeMetadata(); });
      });
      document.getElementById('btn-set-passphrase').addEventListener('click', function() {
        const passphrase = document.getElementById('encryption-passphrase').value.trim();
        if (!passphrase) {
          alert('Please enter a passphrase');
          return;
        }
        fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('espnowsetpassphrase "' + passphrase + '"')
        })
        .then(response => response.text())
        .then(text => {
          document.getElementById('encryption-passphrase').value = '';
          addMessageToLog('ENCRYPTION', text);
          // Refresh status to show actual state
          if (typeof window.checkEncryptionStatus === 'function') {
            window.checkEncryptionStatus();
          }
        })
        .catch(error => {
          document.getElementById('encryption-status').textContent = 'Error setting passphrase';
          addMessageToLog('ERROR', 'Passphrase error: ' + error);
        });
      });
      document.getElementById('btn-clear-passphrase').addEventListener('click', function() {
        fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('espnowsetpassphrase clear')
        })
        .then(response => response.text())
        .then(text => {
          addMessageToLog('ENCRYPTION', text);
          // Refresh status to show actual state
          if (typeof window.checkEncryptionStatus === 'function') {
            window.checkEncryptionStatus();
          }
        })
        .catch(error => {
          document.getElementById('encryption-status').textContent = 'Error clearing passphrase';
          addMessageToLog('ERROR', 'Clear passphrase error: ' + error);
        });
      });
      _on('btn-send-message','click', function() {
        const mac = document.getElementById('send-mac').value.trim();
        const message = document.getElementById('send-message').value.trim();
        if (!message) {
          alert('Please enter a message to send');
          return;
        }
        if (!mac) {
          alert('Please enter a MAC address or use Broadcast button');
          return;
        }
        sendMessage(mac, message);
      });
      _on('btn-broadcast-message','click', function() {
        const message = document.getElementById('send-message').value.trim();
        if (!message) {
          alert('Please enter a message to broadcast');
          return;
        }
        fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('espnowbroadcast ' + message)
        })
        .then(response => response.text())
        .then(text => {
          addMessageToLog('BROADCAST', text);
          if (text && text.indexOf('Broadcast sent') >= 0) {
            document.getElementById('send-message').value = '';
          }
        })
        .catch(error => {
          addMessageToLog('ERROR', 'Broadcast error: ' + error);
        });
      });
      _on('btn-clear-log','click', function() {
        document.getElementById('message-log').innerHTML = '<div style="color:var(--muted); text-align: center;">Message log cleared</div>';
        window.messageCount = 0;
      });
      /* File transfer button handlers */
      _on('btn-send-file','click', function() {
        const mac = document.getElementById('file-target-mac').value.trim();
        const filepath = document.getElementById('file-path').value.trim();
        if (!mac || !filepath) {
          document.getElementById('file-transfer-status').textContent = 'Error: Please enter both MAC address and file path';
          return;
        }
        document.getElementById('file-transfer-status').textContent = 'Sending file...';
        fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('espnowsendfile ' + mac + ' ' + filepath)
        })
        .then(response => response.text())
        .then(text => {
          document.getElementById('file-transfer-status').textContent = text;
          if (text.indexOf('successfully') >= 0) {
            addMessageToLog('FILE', text);
          }
        })
        .catch(error => {
          document.getElementById('file-transfer-status').textContent = 'Error: ' + error;
        });
      });
      _on('btn-list-files','click', function() {
        document.getElementById('file-transfer-status').textContent = 'Listing files...';
        fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('ls')
        })
        .then(response => response.text())
        .then(text => {
          document.getElementById('file-transfer-status').innerHTML = '<pre style="margin:0;white-space:pre-wrap;word-wrap:break-word;">' + text + '</pre>';
        })
        .catch(error => {
          document.getElementById('file-transfer-status').textContent = 'Error: ' + error;
        });
      });
      /* Remote command button handlers */
      _on('btn-send-remote','click', executeRemoteCommand);
      _on('btn-clear-remote-log','click', function() {
        document.getElementById('remote-results-log').innerHTML = '<div style="color:var(--muted); text-align: center;">Remote command results cleared</div>';
      });
      /* Enter key support for remote command */
      _on('remote-command','keypress', function(e) {
        if (e.key === 'Enter') {
          executeRemoteCommand();
        }
      });
      
      /* First-time setup modal handlers */
      _on('btn-setup-save','click', function() {
        const deviceName = document.getElementById('setup-device-name').value.trim();
        const errorDiv = document.getElementById('setup-error');
        
        // Validate name
        if (deviceName.length === 0) {
          errorDiv.textContent = 'Please enter a device name';
          errorDiv.style.display = 'block';
          return;
        }
        if (deviceName.length > 20) {
          errorDiv.textContent = 'Device name must be 20 characters or less';
          errorDiv.style.display = 'block';
          return;
        }
        if (!/^[a-zA-Z0-9_-]+$/.test(deviceName)) {
          errorDiv.textContent = 'Device name can only contain letters, numbers, hyphens, and underscores';
          errorDiv.style.display = 'block';
          return;
        }
        
        // Set the device name
        fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('espnowsetname ' + deviceName)
        })
        .then(response => response.text())
        .then(text => {
          if (text.indexOf('Error') >= 0) {
            errorDiv.textContent = text;
            errorDiv.style.display = 'block';
          } else {
            // Success! Now initialize ESP-NOW
            return fetch('/api/cli', {
              method: 'POST',
              headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
              body: 'cmd=' + encodeURIComponent('openespnow')
            })
            .then(response => response.text())
            .then(initText => {
              document.getElementById('espnow-status-data').textContent = 'Device name set to: ' + deviceName + '\n\n' + initText;
              document.getElementById('setup-modal').classList.remove('show');
              document.getElementById('setup-device-name').value = '';
              errorDiv.style.display = 'none';
              refreshStatus();
            });
          }
        })
        .catch(error => {
          errorDiv.textContent = 'Error: ' + error;
          errorDiv.style.display = 'block';
        });
      });
      
      _on('btn-setup-cancel','click', function() {
        document.getElementById('setup-modal').classList.remove('show');
        document.getElementById('setup-device-name').value = '';
        document.getElementById('setup-error').style.display = 'none';
      });
      
      /* Enter key support for setup modal */
      _on('setup-device-name','keypress', function(e) {
        if (e.key === 'Enter') {
          document.getElementById('btn-setup-save').click();
        }
      });
    };
    console.log('[ESP-NOW] Chunk 5: Button handlers ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 5 error:', e); }
})();
</script>
<script>
(function() {
  try {
    console.log('[ESP-NOW] Chunk 5b: Remote command functions start');
    window.setRemoteCommand = function(command) {
      document.getElementById('remote-command').value = command;
    };
    window.addRemoteResultToLog = function(type, message) {
      const log = document.getElementById('remote-results-log');
      if (!log) return;
      const timestamp = new Date().toLocaleTimeString();
      let className = 'message-item';
      if (type === 'SUCCESS') className += ' message-received';
      else if (type === 'ERROR' || type === 'FAILED') className += ' message-error';
      else if (type === 'SENT') className += ' message-sent';
      const messageDiv = document.createElement('div');
      messageDiv.className = className;
      if (type === 'RESULT') {
        /* Multi-line result formatting */
        messageDiv.innerHTML = '<pre style="margin: 0; white-space: pre-wrap; font-family: inherit;">' + message + '</pre>';
      } else {
        messageDiv.textContent = '[' + timestamp + '] ' + type + ': ' + message;
      }
      if (log.children.length === 1 && log.children[0].textContent.includes('Remote command results will appear')) {
        log.innerHTML = '';
      }
      log.appendChild(messageDiv);
      /* Limit log size */
      if (log.children.length > 20) {
        log.removeChild(log.firstChild);
      }
      log.scrollTop = log.scrollHeight;
    };
    window.executeRemoteCommand = function() {
      const device = document.getElementById('remote-device').value.trim();
      const username = document.getElementById('remote-username').value.trim();
      const password = document.getElementById('remote-password').value.trim();
      const command = document.getElementById('remote-command').value.trim();
      
      if (!device || !username || !password || !command) {
        alert('Please fill in all fields: device, username, password, and command');
        return;
      }
      
      const remoteCmd = 'espnowremote ' + device + ' ' + username + ' ' + password + ' ' + command;
      addRemoteResultToLog('SENT', 'Executing on ' + device + ': ' + command);
      
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent(remoteCmd)
      })
      .then(response => response.text())
      .then(text => {
        if (text.includes('Remote command sent')) {
          addRemoteResultToLog('SUCCESS', 'Command sent successfully - waiting for response...');
        } else {
          addRemoteResultToLog('ERROR', text);
        }
      })
      .catch(error => {
        addRemoteResultToLog('ERROR', 'Send error: ' + error);
      });
    };
    console.log('[ESP-NOW] Chunk 5b: Remote command functions ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 5b error:', e); }
})();
</script>
<script>
(function() {
  try {
    console.log('[ESP-NOW] Chunk 5c: Smart home metadata functions');
    window.loadSmartHomeMetadata = function(preloadedText) {
      function applyMetadata(text) {
        const statusDiv = document.getElementById('smarthome-status');
        if (statusDiv) statusDiv.textContent = text;
        const friendlyMatch = text.match(/Friendly Name:\s*(.+)/);
        const roomMatch = text.match(/Room:\s*(.+)/);
        const zoneMatch = text.match(/Zone:\s*(.+)/);
        const tagsMatch = text.match(/Tags:\s*(.+)/);
        const stationaryMatch = text.match(/Stationary:\s*(true|false|yes|no)/i);
        const friendlyInput = document.getElementById('friendly-name');
        const roomInput = document.getElementById('room-name');
        const zoneInput = document.getElementById('zone-name');
        const tagsInput = document.getElementById('tags-input');
        const stationaryCheckbox = document.getElementById('stationary-checkbox');
        if (friendlyInput && friendlyMatch) {
          const val = friendlyMatch[1].trim();
          friendlyInput.value = (val === '(not set)') ? '' : val;
        }
        if (roomInput && roomMatch) {
          const val = roomMatch[1].trim();
          roomInput.value = (val === '(not set)') ? '' : val;
        }
        if (zoneInput && zoneMatch) {
          const val = zoneMatch[1].trim();
          zoneInput.value = (val === '(not set)') ? '' : val;
        }
        if (tagsInput && tagsMatch) {
          const val = tagsMatch[1].trim();
          tagsInput.value = (val === '(none)') ? '' : val;
        }
        if (stationaryCheckbox && stationaryMatch) {
          const sv = stationaryMatch[1].toLowerCase();
          stationaryCheckbox.checked = (sv === 'yes' || sv === 'true');
        }
      }
      if (preloadedText !== undefined) {
        applyMetadata(preloadedText);
        return;
      }
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnowdeviceinfo')
      })
      .then(response => response.text())
      .then(applyMetadata)
      .catch(error => {
        console.error('[ESP-NOW] Error loading smart home metadata:', error);
      });
    };
    console.log('[ESP-NOW] Chunk 5c: Smart home metadata functions ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 5c error:', e); }
})();
</script>
<script>
(function() {
  try {
    console.log('[ESP-NOW] Chunk 5d: Encryption status check');
    window.checkEncryptionStatus = function(preloadedEncText) {
      function applyEncStatus(text) {
        const statusDiv = document.getElementById('encryption-status');
        const setBtn = document.getElementById('btn-set-passphrase');
        const clearBtn = document.getElementById('btn-clear-passphrase');
        if (!statusDiv) return;
        var hasPass = text && text.indexOf('Passphrase Set: Yes') >= 0;
        if (hasPass) {
          statusDiv.textContent = 'Encryption passphrase is set';
        } else if (text.includes('Passphrase Set: No') || text.includes('Encryption Enabled: No')) {
          statusDiv.textContent = 'No encryption passphrase set';
        } else {
          statusDiv.textContent = 'Encryption status unknown';
        }
        if (setBtn) setBtn.style.display = hasPass ? 'none' : 'inline-flex';
        if (clearBtn) clearBtn.style.display = hasPass ? 'inline-flex' : 'none';
      }
      if (preloadedEncText !== undefined) {
        applyEncStatus(preloadedEncText);
        return;
      }
      // Only check if ESP-NOW is initialized
      const indicator = document.getElementById('espnow-status-indicator');
      const isInitialized = indicator && indicator.className.indexOf('status-enabled') >= 0;
      if (!isInitialized) {
        console.log('[ESP-NOW] Skipping encryption status check - ESP-NOW not initialized');
        return;
      }
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnowencstatus')
      })
      .then(response => response.text())
      .then(applyEncStatus)
      .catch(error => {
        console.error('[ESP-NOW] Error checking encryption status:', error);
      });
    };
    console.log('[ESP-NOW] Chunk 5c: Encryption status check ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 5c error:', e); }
})();
</script>
<script>
(function() {
  try {
    console.log('[ESP-NOW] Chunk 6: Main init start');
    document.addEventListener('DOMContentLoaded', function() {
      console.log('[ESP-NOW] DOMContentLoaded');
      setupButtonHandlers();
      refreshStatusBatch(); /* Single batch request: loads all ESP-NOW status in one HTTPS call */
      /* SSE-based: no legacy RX watcher */
    });
    console.log('[ESP-NOW] Chunk 6: Main init ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 6 error:', e); }
})();
</script>
<script>
(function(){
  var lastSeqNum = 0;
  var pollInterval = null;
  var authFailed = false;
  
  function pollEspNowMessages() {
    if (authFailed) return;
    console.log('[ESP-NOW] Polling messages since=' + lastSeqNum);
    /* Use hw.fetchJSON if available (handles auth errors), otherwise use fetch with proper headers */
    var fetchFn = (window.hw && window.hw.fetchJSON) ? 
      function(url) { return window.hw.fetchJSON(url); } :
      function(url) { 
        return fetch(url, {
          credentials: 'include',
          cache: 'no-store',
          headers: { 'Accept': 'application/json' }
        }).then(function(r) {
          if (r.status === 401) {
            authFailed = true;
            stopPolling();
            window.location.href = '/login';
            throw new Error('auth_required');
          }
          return r.json();
        });
      };
    
    fetchFn('/api/espnow/messages?since=' + lastSeqNum)
      .then(function(data){
        console.log('[ESP-NOW] Poll response:', data);
        if (!data || !data.messages) return;
        console.log('[ESP-NOW] Processing ' + data.messages.length + ' messages');
        data.messages.forEach(function(msg){
          if (msg.seq > lastSeqNum) lastSeqNum = msg.seq;
          var mac = (msg.mac || '').toUpperCase();
          var text = msg.msg || '';
          if (!mac) return;
          console.log('[ESP-NOW] Received message from ' + mac + ': ' + text);
          console.log('[ESP-NOW] Looking for container: log-' + mac);
          if (typeof window.appendLogLine === 'function') {
            window.appendLogLine('log-' + mac, 'RECEIVED', text, null);
          } else {
            console.error('[ESP-NOW] appendLogLine not available');
          }
        });
      })
      .catch(function(e){ 
        if (e && e.message === 'auth_required') {
          console.log('[ESP-NOW] Auth required, stopping polling');
          authFailed = true;
          stopPolling();
        } else {
          console.error('[ESP-NOW] Poll error:', e); 
        }
      });
  }
  
  function startPolling() {
    if (pollInterval || authFailed) return;
    console.log('[ESP-NOW] Starting message polling (500ms)');
    pollEspNowMessages();
    pollInterval = setInterval(pollEspNowMessages, 500);
  }

  function stopPolling() {
    if (pollInterval) {
      console.log('[ESP-NOW] Stopping message polling');
      clearInterval(pollInterval);
      pollInterval = null;
    }
  }

  // Expose so refreshStatus() can gate polling on init state
  window.espnowStartPolling = startPolling;
  window.espnowStopPolling = stopPolling;

  window.addEventListener('beforeunload', stopPolling);
})();
</script>
)JS", HTTPD_RESP_USE_STRLEN);
  
  // Include generic file browser utility
  httpd_resp_send_chunk(req, getFileBrowserScript().c_str(), HTTPD_RESP_USE_STRLEN);
}

void registerEspNowHandlers(httpd_handle_t server);

#endif
