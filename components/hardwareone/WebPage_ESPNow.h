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
.remote-explorer{border:1px solid var(--border);border-radius:8px;background:var(--panel-bg);color:var(--panel-fg);overflow:hidden}
.remote-explorer-crumb{padding:8px;background:var(--crumb-bg);border-bottom:1px solid var(--border);font-size:0.85em;display:flex;flex-wrap:wrap;gap:6px;align-items:center}
.remote-explorer-crumb span{cursor:pointer}
.remote-entry{display:flex;align-items:center;gap:10px;padding:8px 12px;border-bottom:1px solid var(--border);cursor:pointer;transition:background .12s}
.remote-entry:hover{background:var(--hover-bg)}
.remote-entry-empty{justify-content:center;color:var(--muted);font-style:italic}
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
<button class='btn' id='btn-espnow-toggle-mode' style='display:none' data-guest-hide>Mode: Direct</button>
<button class='btn' id='btn-espnow-init' style='display:none' data-guest-hide>Initialize</button>
<button class='btn' id='btn-espnow-disable' style='display:none' data-guest-hide>Disable</button>
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
<button class='btn' onclick="openBroadcastPanel()" data-guest-hide>Broadcast</button>
<button class='btn' id='btn-add-device-toggle' onclick="(function(){var p=hw.$('add-device-pane');var b=hw.$('btn-add-device-toggle');var show=p.style.display==='none'||!p.style.display;p.style.display=show?'block':'none';b.textContent=show?'Cancel':'+ Add Device';})()" >+ Add Device</button>
</div>
<div id='add-device-pane' style='display:none' data-guest-hide>
<div class='en-pair-inputs'>
<input type='text' id='pair-mac' class='mac-input' placeholder='XX:XX:XX:XX:XX:XX' maxlength='17'>
<input type='text' id='pair-name' placeholder='Device Name'>
<select id='pair-mesh' class='input-fit input-m' title='Target mesh for this pairing' style='display:none'></select>
<button class='btn' id='btn-pair-secure'>Pair</button>
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
<button class='btn' id='btn-auto-topology' data-guest-hide>Auto-Discover: OFF</button>
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
<div id='device-metadata-card' style='margin-bottom:16px;padding-bottom:16px;border-bottom:1px solid var(--border)' data-guest-hide>
<div style='font-size:1rem;font-weight:600;color:var(--panel-fg);margin-bottom:8px'>Device Metadata</div>
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
<input type='text' id='tags-input' placeholder='Tags (comma-separated)' maxlength='53'>
<button class='btn' id='btn-set-tags'>Set Tags</button>
</div>
<label style='display:flex;align-items:center;gap:8px;cursor:pointer;margin-top:4px'>
<input type='checkbox' id='stationary-checkbox' style='width:auto;margin:0'>
<span style='color:var(--panel-fg);font-size:.9em'>Stationary Device</span>
</label>
</div>
<div id='channel-card' style='margin-bottom:16px;padding-bottom:16px;border-bottom:1px solid var(--border)'>
<div style='font-size:1rem;font-weight:600;color:var(--panel-fg);margin-bottom:8px'>Radio Channel</div>
<div style='color:var(--muted);font-size:.82em;margin-bottom:10px'>Two devices only find each other on the same channel. <b>Auto</b> follows your Wi-Fi; a fixed channel is pinned whenever the device is off Wi-Fi &mdash; set the <b>same</b> value on both devices for off-grid pairing. (While joined to Wi-Fi the network's channel is used.)</div>
<div id='channel-status' style='background:var(--crumb-bg);border-radius:8px;padding:10px;font-size:.85em;color:var(--panel-fg);border:1px solid var(--border);margin-bottom:10px'>Loading channel&hellip;</div>
<div class='en-form-row'>
<select id='channel-select' data-guest-hide></select>
<button class='btn' id='btn-set-channel' data-guest-hide>Set Channel</button>
</div>
</div>
<div id='meshes-card' style='margin-bottom:16px'>
<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:8px;flex-wrap:wrap;gap:8px'>
<div style='font-size:1rem;font-weight:600;color:var(--panel-fg)'>Meshes</div>
<div id='mesh-active-pill' style='font-size:.82em;color:var(--panel-fg);padding:3px 10px;border-radius:8px;background:var(--crumb-bg);border:1px solid var(--border)'>Mesh: <span id='mesh-active-label' style='font-weight:600'>&hellip;</span></div>
</div>
<div style='color:var(--muted);font-size:.82em;margin-bottom:10px'>Up to 4 mesh slots. The default mesh is used when no mesh is specified on pair/send.</div>
<div id='meshes-table' style='display:flex;flex-direction:column;gap:6px;margin-bottom:10px'>
<div style='color:var(--muted);font-size:.85em;padding:8px 0' id='meshes-loading'>Loading meshes&hellip;</div>
</div>
<div id='meshes-add-row' class='en-form-row' style='display:none' data-guest-hide>
<input type='text' id='mesh-add-label' placeholder='New mesh label (e.g., lab2)' maxlength='16'>
<button class='btn' id='btn-mesh-add'>Add Mesh</button>
</div>
<div id='meshes-full-msg' style='display:none;color:var(--muted);font-size:.82em;text-align:center;padding:6px'>All 4 mesh slots are in use. Disable a mesh and delete it to free a slot.</div>
</div>
<div id='mesh-role-card' style='display:none;padding-top:16px;border-top:1px solid var(--border)'>
<div style='font-size:1rem;font-weight:600;color:var(--panel-fg);margin-bottom:8px'>Mesh Role Configuration</div>
<div style='background:var(--crumb-bg);border-radius:8px;padding:10px;font-size:.85em;color:var(--panel-fg);border:1px solid var(--border);margin-bottom:12px' id='mesh-role-status'>Loading role configuration...</div>
<div style='display:flex;gap:8px;margin-bottom:12px;flex-wrap:wrap'>
<button class='btn' id='btn-role-worker' data-guest-hide>Worker</button>
<button class='btn' id='btn-role-master' data-guest-hide>Master</button>
<button class='btn' id='btn-role-backup' data-guest-hide>Backup</button>
<button class='btn' id='btn-mesh-topo' data-guest-hide>Discover Topology</button>
</div>
<div class='en-form-row'>
<input type='text' id='master-mac' class='mac-input' placeholder='Master MAC (XX:XX:XX:XX:XX:XX)' maxlength='17' data-guest-hide>
<button class='btn' id='btn-set-master-mac' data-guest-hide>Set Master</button>
</div>
<label style='display:flex;align-items:center;gap:8px;cursor:pointer;margin-top:8px' data-guest-hide>
<input type='checkbox' id='backup-master-enabled' style='width:auto;margin:0'>
<span style='color:var(--panel-fg);font-size:.9em'>Enable Backup Master</span>
</label>
<div id='backup-mac-group' class='en-form-row' style='display:none;margin-top:8px'>
<input type='text' id='backup-mac' class='mac-input' placeholder='Backup MAC (XX:XX:XX:XX:XX:XX)' maxlength='17' data-guest-hide>
<button class='btn' id='btn-set-backup-mac' data-guest-hide>Set Backup</button>
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
<li>1-31 letters, numbers and hyphens</li>
<li>Not starting or ending with a hyphen; no spaces, underscores or other characters</li>
<li>Also the Bluetooth name and the Wi-Fi hostname</li>
</ul>
</div>
<div class='setup-modal-error' id='setup-error'></div>
<input type='text' id='setup-device-name' class='setup-modal-input' placeholder='Enter device name (e.g., darkblue)' maxlength='31' autocomplete='off' data-guest-hide>
<div class='setup-modal-buttons'>
<button class='btn' id='btn-setup-cancel'>Cancel</button>
<button class='btn' id='btn-setup-save' data-guest-hide>Set Name & Initialize</button>
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
</script>
<script>console.log('[ESP-NOW] Section 1: Pre-script sentinel');</script>
<script>
(function() {
  try {
    console.log('[ESP-NOW] Chunk 1: Global variables start');
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
      var btn = hw.$('btn-load-autos-' + mac);
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
    // The page decides from parsed json replies only (window.espnowParseReply,
    // espnow-core.js); the human status text is shown but never matched on.
    //   statusObj   'espnowstatus json' (null when the reply was not json)
    //   statusText  'espnowstatus' as text, for the status box
    //   modeObj     'espnowmode json'
    // Returns { initialized, mesh }.
    window.espnowApplyStatus = function(statusObj, statusText, modeObj) {
      const indicator = hw.$('espnow-status-indicator');
      const box = hw.$('espnow-status-data');
      const isInitialized = !!(statusObj && statusObj.initialized === true);
      box.textContent = statusText || '';
      if (isInitialized) {
        indicator.className = 'status-indicator status-enabled';
        hw.$('btn-espnow-init').style.display = 'none';
        hw.$('btn-espnow-disable').style.display = '';
        hw.$('btn-espnow-toggle-mode').style.display = '';
        hw.$('en-not-init').style.display = 'none';
        hw.$('en-panels').style.display = 'block';
        box.style.display = 'block';
        if (typeof window.espnowStartPolling === 'function') window.espnowStartPolling();
      } else {
        indicator.className = 'status-indicator status-disabled';
        hw.$('btn-espnow-init').style.display = '';
        hw.$('btn-espnow-disable').style.display = 'none';
        hw.$('btn-espnow-toggle-mode').style.display = 'none';
        hw.$('en-not-init').style.display = 'flex';
        hw.$('en-panels').style.display = 'none';
        // The status text stays visible when it says why the last start failed.
        box.style.display = (statusObj && statusObj.lastInitError) ? 'block' : 'none';
        if (typeof window.espnowStopPolling === 'function') window.espnowStopPolling();
      }
      const isMesh = !!(modeObj && modeObj.mode === 'mesh');
      try {
        hw.setText(hw.$('btn-espnow-toggle-mode'), 'Mode: ' + (isMesh ? 'Mesh' : 'Direct'));
        hw.toggle(hw.$('mesh-views-card'), (isMesh && isInitialized));
        hw.toggle(hw.$('mesh-role-card'), (isMesh && isInitialized));
        window.espnowIsMesh = isMesh;
        if (isMesh && isInitialized) {
          if (typeof window.startMeshStatusPolling === 'function') window.startMeshStatusPolling();
        } else {
          if (typeof window.stopMeshStatusPolling === 'function') window.stopMeshStatusPolling();
        }
      } catch(e) { console.error('[ESP-NOW] Error setting mode:', e); }
      return { initialized: isInitialized, mesh: isMesh };
    };

    // Applies one /api/espnow/status reply. Slot order is handleEspNowStatus's
    // CMDS[] (WebPage_ESPNow.cpp): 0 espnowstatus json, 1 espnowmode json,
    // 2 bondstatus json, 3 espnowlist, 4 espnowmeshes listjson,
    // 5 espnowdeviceinfo json, 6 espnowmeshrole json, 7 espnowmeshstatus,
    // 8 espnowstatus (text, status box only).
    window.espnowApplyStatusSlots = function(results) {
      var parse = window.espnowParseReply;
      var statusObj = parse(results[0]);
      var st = window.espnowApplyStatus(statusObj, results[8] || '', parse(results[1]));
      if (!st.initialized) return st;
      try { if (typeof listDevices === 'function') listDevices(results[2], results[3]); } catch(e) { console.warn('[ESP-NOW] listDevices error:', e); }
      try { if (typeof window.loadMeshes === 'function') window.loadMeshes(results[4]); } catch(e) { console.warn('[ESP-NOW] loadMeshes error:', e); }
      try { if (typeof window.loadLocalDeviceMetadata === 'function') window.loadLocalDeviceMetadata(results[5]); } catch(e) { console.warn('[ESP-NOW] deviceinfo error:', e); }
      try { if (typeof window.refreshChannel === 'function') window.refreshChannel(statusObj); } catch(e) { console.warn('[ESP-NOW] channel error:', e); }
      if (st.mesh) {
        try { if (typeof window.refreshMeshRole === 'function') window.refreshMeshRole(results[6]); } catch(e) { console.warn('[ESP-NOW] meshrole error:', e); }
        try { if (typeof window.refreshMeshStatus === 'function') window.refreshMeshStatus(results[7]); } catch(e) { console.warn('[ESP-NOW] meshstatus error:', e); }
      }
      return st;
    };

    // Fallback when /api/espnow/status cannot be fetched: the same json forms,
    // one CLI request each. 'espnowmode' is an admin command; a refusal leaves
    // the mode unknown (shown as Direct) instead of failing the refresh.
    window.espnowRefreshStatusFromCli = function() {
      var parse = window.espnowParseReply;
      var statusObj = null;
      var statusText = '';
      return hw.postFormText('/api/cli', { cmd: 'espnowstatus json' })
      .then(function(text) {
        statusObj = parse(text);
        return hw.postFormText('/api/cli', { cmd: 'espnowstatus' });
      })
      .then(function(text) {
        statusText = text;
        return hw.postFormText('/api/cli', { cmd: 'espnowmode json' }).catch(function() { return ''; });
      })
      .then(function(modeText) {
        var st = window.espnowApplyStatus(statusObj, statusText, parse(modeText));
        if (!st.initialized) return;
        try { if (typeof listDevices === 'function') listDevices(); } catch(e) { console.warn('[ESP-NOW] listDevices error:', e); }
        try { if (typeof window.loadMeshes === 'function') window.loadMeshes(); } catch(e) { console.warn('[ESP-NOW] loadMeshes error:', e); }
        try { if (typeof window.loadLocalDeviceMetadata === 'function') window.loadLocalDeviceMetadata(); } catch(e) { console.warn('[ESP-NOW] deviceinfo error:', e); }
        try { if (typeof window.refreshChannel === 'function') window.refreshChannel(statusObj); } catch(e) { console.warn('[ESP-NOW] channel error:', e); }
        if (st.mesh) {
          try { if (typeof window.refreshMeshRole === 'function') window.refreshMeshRole(); } catch(e) { console.warn('[ESP-NOW] meshrole error:', e); }
          try { if (typeof window.refreshMeshStatus === 'function') window.refreshMeshStatus(); } catch(e) { console.warn('[ESP-NOW] meshstatus error:', e); }
        }
      })
      .catch(function(error) {
        hw.$('espnow-status-data').textContent = 'Error: ' + (error && error.message ? error.message : error);
      });
    };

    // Page load and every Refresh: one request for all status slots. The endpoint
    // runs the commands as the system, so a guest's page and Refresh both work.
    window.refreshStatus = function() {
      return hw.fetchJSON('/api/espnow/status')
      .then(function(data) {
        if (!data || !Array.isArray(data.results) || data.results.length < 2) {
          console.warn('[ESP-NOW] Status response invalid, falling back to individual requests');
          return window.espnowRefreshStatusFromCli();
        }
        window.espnowApplyStatusSlots(data.results);
      }, function(error) {
        console.warn('[ESP-NOW] Status fetch failed, falling back to individual requests:', error);
        return window.espnowRefreshStatusFromCli();
      })
      .catch(function(error) {
        // The reply could not be applied (for example /assets/espnow-core.js
        // did not load): say so instead of leaving the page on "Loading...".
        console.error('[ESP-NOW] Status apply error:', error);
        var box = hw.$('espnow-status-data');
        box.textContent = 'Error: ' + (error && error.message ? error.message : error);
        box.style.display = 'block';
      });
    };
    window.refreshStatusBatch = window.refreshStatus;

    console.log('[ESP-NOW] Chunk 2: Status functions ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 2 error:', e); }
})();
</script>
<script src="/assets/espnow-core.js"></script>
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
          var viewsCard = hw.$('mesh-views-card');
          hw.show(viewsCard);
          window.renderUnifiedDeviceList();
        } catch(e) {
          console.error('[ESP-NOW] Error parsing mesh status:', e);
        }
      }
      if (preloadedText !== undefined) {
        applyMeshStatus(preloadedText);
        return;
      }
      hw.postFormText('/api/cli', { cmd: 'espnowmeshstatus' })
      .then(function(text) { window.meshStatusPollInFlight = false; applyMeshStatus(text); })
      .catch(error => {
        window.meshStatusPollInFlight = false;
        console.error('[ESP-NOW] Mesh status fetch error:', error);
      });

      // Topology view is refreshed only on explicit user action (tab switch or Discover button),
      // not on the 10-second mesh status poll — that would spam the CLI with toporesults requests.
    };
    
    // Pair a discovered device: an encrypted pair request. The other device shows
    // its Accept prompt (or runs 'espnowaccept'); both need discovery open.
    window.pairUnpairedDevice = async function(mac, name) {
      console.log('[ESP-NOW] Pair request to:', mac, name);
      if (!await hwConfirm('Send a pair request to "' + name + '" (' + mac + ')? The other device must accept it.')) {
        return;
      }
      
      hw.postFormText('/api/cli', { cmd: 'espnowpairrequest ' + mac })
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
        var elem = hw.$('mesh-view-' + v);
        var btn = hw.$(buttons[idx]);
        if (v === view) {
          hw.show(elem);
          if (btn) btn.style.background = 'var(--crumb-bg)';
        } else {
          hw.hide(elem);
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
    
    window.topologyEscapeHtml = function(value) {
      return String(value == null ? '' : value).replace(/[&<>"']/g, function(ch) {
        return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[ch];
      });
    };
    window.topologyResultIsError = function(value) {
      return !value || /^\s*(?:OK:\s*)?Error(?::|\b)/i.test(String(value));
    };

    // The command result contract is 4 KiB, so topology responses are paged two
    // responders at a time. Fetch pages serially: the command uses one reusable
    // PSRAM render buffer and concurrent requests would race that buffer.
    window.fetchTopologyResultsPages = function() {
      function pageIdentity(text) {
        var requestMatch = text && text.match(/^Request ID:\s*(\d+)\s*$/m);
        var pageMatch = text && text.match(/^Page:\s*(\d+)\/(\d+)\s*$/m);
        if (!requestMatch || !pageMatch) return null;
        return {
          requestId: requestMatch[1],
          page: parseInt(pageMatch[1], 10),
          pages: parseInt(pageMatch[2], 10)
        };
      }

      function fetchSnapshot(retriesLeft) {
        return hw.postFormText('/api/cli', { cmd: 'espnowtoporesults' })
        .then(function(firstPage) {
          var firstIdentity = pageIdentity(firstPage);
          // "Still collecting" and normal command errors have no page header;
          // preserve them for the existing polling/error classifier.
          if (!firstIdentity) return firstPage;
          if (firstIdentity.page !== 1 || !firstIdentity.pages) {
            throw new Error('Invalid topology page header');
          }
          var pageCount = Math.min(firstIdentity.pages, 8); // MESH_PEER_MAX / 2
          if (pageCount < 2) return firstPage;
          var chain = Promise.resolve([firstPage]);
          for (var page = 2; page <= pageCount; page++) {
            (function(pageNumber) {
              chain = chain.then(function(parts) {
                return hw.postFormText('/api/cli', {
                  cmd: 'espnowtoporesults ' + pageNumber + ' ' + firstIdentity.requestId
                }).then(function(nextPage) {
                  var nextIdentity = pageIdentity(nextPage);
                  if (!nextIdentity || nextIdentity.requestId !== firstIdentity.requestId ||
                      nextIdentity.page !== pageNumber || nextIdentity.pages !== firstIdentity.pages) {
                    var changed = new Error('Topology changed while fetching pages');
                    changed.topologySnapshotChanged = true;
                    throw changed;
                  }
                  parts.push(nextPage);
                  return parts;
                });
              });
            })(page);
          }
          return chain.then(function(parts) { return parts.join('\n'); });
        })
        .catch(function(error) {
          if (error && error.topologySnapshotChanged && retriesLeft > 0) {
            return fetchSnapshot(retriesLeft - 1);
          }
          throw error;
        });
      }

      return fetchSnapshot(1);
    };

    // Refresh full topology view
    window.refreshTopologyView = function() {
      console.log('[ESP-NOW] Refreshing topology view...');
      window.fetchTopologyResultsPages()
      .then(output => {
        console.log('[ESP-NOW] Topology results:', output);
        var container = hw.$('mesh-topology-view');
        if (!container) return;
        
        // Check if we have topology data
        if (window.topologyResultIsError(output)) {
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
          var deviceMatch = line.match(/^(.+?)\s*\(([A-Fa-f0-9:]{17})\)\s*:\s*(\[partial\])?$/i);
          if (deviceMatch) {
            currentDevice = {
              name: deviceMatch[1],
              mac: deviceMatch[2],
              peers: [],
              peerCount: 0,
              partial: !!deviceMatch[3],
              path: ''
            };
            continue;
          }
          
          // Match path: "Path: ..."
          var pathMatch = line.match(/^Path:\s*(.+)$/i);
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
          var peerMatch = line.match(/^→\s*(.+)\s+\(([A-Fa-f0-9:]{17})\)\s*$/);
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
          if (line === '' && currentDevice) {
            deviceData[currentDevice.mac] = currentDevice;
            currentDevice = null;
          }
        }
        
        // Save last device if exists
        if (currentDevice) {
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
            html += '<div style="font-weight:bold;font-size:1.05em;color:var(--success);margin-bottom:8px;">' + window.topologyEscapeHtml(dev.name) + (dev.partial ? ' [partial]' : '') + '</div>';
            html += '<div style="font-size:0.85em;color:var(--panel-fg);margin-bottom:4px;">' + window.topologyEscapeHtml(dev.mac) + ' • ' + dev.peerCount + ' peer(s)</div>';
            if (dev.path) {
              html += '<div style="font-size:0.8em;color:var(--link);margin-bottom:8px;">Path: ' + window.topologyEscapeHtml(dev.path) + ' (' + hopCount + ' hop' + (hopCount !== 1 ? 's' : '') + ')</div>';
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
                html += '<div style="font-weight:500;color:var(--panel-fg);">' + statusDot + window.topologyEscapeHtml(peer.name) + '</div>';
                html += '<div style="font-size:0.8em;color:var(--panel-fg);margin-top:2px;">';
                html += window.topologyEscapeHtml(peer.mac);
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
        var container = hw.$('mesh-topology-view');
        hw.setHTML(container, '<div style="color:var(--danger);text-align:center;">Error loading topology: ' + error + '</div>');
      });
    };
    
    // Refresh network graph view
    window.refreshGraphView = function() {
      console.log('[ESP-NOW] Refreshing graph view...');
      var container = hw.$('mesh-graph-view');
      if (!container) return;
      
      // Fetch both direct peers and current device status
      Promise.all([
        hw.postFormText('/api/cli', { cmd: 'espnowmeshstatus' }),
        hw.postFormText('/api/cli', { cmd: 'espnowstatus json' })
      ])
      .then(function(results) {
        var meshStatus = results[0];
        var deviceStatus = window.espnowParseReply(results[1]);
        
        // Current device name and MAC (the json 'mac' field, shape-checked)
        var deviceName = 'THIS DEVICE';
        var deviceMac = '';
        if (deviceStatus && window.espnowIsMacText(deviceStatus.mac)) {
          deviceMac = deviceStatus.mac;
          // Try to get device name from page title
          var titleElem = hw.qs('h1');
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
        html += '<div style="color:var(--success);font-weight:bold;">' + selfDot + window.topologyEscapeHtml(deviceName);
        if (deviceMac) {
          html += ' (' + window.topologyEscapeHtml(deviceMac) + ')';
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
              html += '<div style="margin-left:20px;color:var(--panel-fg);">├─ ' + statusDot + window.topologyEscapeHtml(peer.name) + ' (' + window.topologyEscapeHtml(peer.mac) + ')</div>';
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
      var btn = hw.$('btn-auto-topology');
      if (!btn) return;
      
      if (window.autoTopoInterval) {
        clearInterval(window.autoTopoInterval);
        window.autoTopoInterval = null;
        btn.textContent = 'Auto-Discover: OFF';
        btn.style.background = '';
        btn.style.color = '';
      } else {
        window.autoTopoInterval = setInterval(function() {
          // A refused run (topology busy, no peer took the request) is an
          // HTTP 400 and rejects. The next run is the retry.
          hw.postFormText('/api/cli', { cmd: 'espnowmeshtopo' }).catch(function() {});
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
      // output: the 'espnowmeshrole json' reply. Role and MACs are settings
      // strings: shape-checked here and escaped before they reach innerHTML.
      function applyMeshRole(output) {
        console.log('[ESP-NOW] Mesh role response:', output);
        var statusDiv = hw.$('mesh-role-status');
        if (!statusDiv) return;
        var esc = window.topologyEscapeHtml;
        var isMac = window.espnowIsMacText;
        var j = window.espnowParseReply(output);
        var role = (j && typeof j.role === 'string' && /^\w+$/.test(j.role)) ? j.role : 'unknown';
        var masterMAC = (j && isMac(j.masterMac)) ? j.masterMac : 'Not set';
        var backupEnabled = !!(j && j.backupEnabled === true);
        var backupMAC = (j && isMac(j.backupMac)) ? j.backupMac : 'Not set';
        var html = '<strong>Current Role:</strong> ' + esc(role.charAt(0).toUpperCase() + role.slice(1));
        html += '<br><strong>Master MAC:</strong> ' + esc(masterMAC);
        if (backupEnabled) {
          html += '<br><strong>Backup MAC:</strong> ' + esc(backupMAC);
        }
        statusDiv.innerHTML = html;
        var backupCheckbox = hw.$('backup-master-enabled');
        if (backupCheckbox) backupCheckbox.checked = backupEnabled;
        var masterGroup = hw.$('master-mac')?.parentElement;
        var backupMacGroup = hw.$('backup-mac-group');
        if (role.toLowerCase() === 'master') {
          hw.hide(masterGroup);
          hw.toggle(backupMacGroup, (backupEnabled));
          if (backupEnabled && backupMAC !== 'Not set') {
            var backupInput = hw.$('backup-mac');
            if (backupInput) backupInput.value = backupMAC;
          }
        } else if (role.toLowerCase() === 'backup') {
          hw.show(masterGroup);
          hw.hide(backupMacGroup);
          if (masterMAC !== 'Not set') {
            var masterInput = hw.$('master-mac');
            if (masterInput) masterInput.value = masterMAC;
          }
        } else {
          hw.show(masterGroup);
          hw.toggle(backupMacGroup, (backupEnabled));
          if (masterMAC !== 'Not set') {
            var masterInput = hw.$('master-mac');
            if (masterInput) masterInput.value = masterMAC;
          }
          if (backupEnabled && backupMAC !== 'Not set') {
            var backupInput = hw.$('backup-mac');
            if (backupInput) backupInput.value = backupMAC;
          }
        }
      }
      if (preloadedText !== undefined) {
        applyMeshRole(preloadedText);
        return;
      }
      hw.postFormText('/api/cli', { cmd: 'espnowmeshrole json' })
      .then(applyMeshRole)
      .catch(error => {
        console.error('[ESP-NOW] Mesh role fetch error:', error);
        var statusDiv = hw.$('mesh-role-status');
        hw.setHTML(statusDiv, '<span style="color:var(--danger);">Error: ' + window.topologyEscapeHtml(error && error.message ? error.message : String(error)) + '</span>');
      });
    };
    
    window.setMeshRole = function(role) {
      console.log('[ESP-NOW] Setting mesh role to:', role);
      hw.postFormText('/api/cli', { cmd: 'espnowmeshrole ' + role })
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
      var mac = (hw.$('master-mac') || {}).value || '';
      if (!mac || mac.length !== 17) {
        alert('Enter a valid MAC address (XX:XX:XX:XX:XX:XX)');
        return;
      }
      console.log('[ESP-NOW] Setting master MAC to:', mac);
      hw.postFormText('/api/cli', { cmd: 'espnowmeshmaster ' + mac })
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
      var mac = (hw.$('backup-mac') || {}).value || '';
      if (!mac || mac.length !== 17) {
        alert('Enter a valid MAC address (XX:XX:XX:XX:XX:XX)');
        return;
      }
      console.log('[ESP-NOW] Setting backup MAC to:', mac);
      hw.postFormText('/api/cli', { cmd: 'espnowmeshbackup ' + mac })
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
      var backupMacGroup = hw.$('backup-mac-group');
      hw.postFormText('/api/cli', { cmd: 'espnowbackupenable ' + (enabled ? 'on' : 'off') })
      .then(output => {
        console.log('[ESP-NOW] Backup enable response:', output);
        hw.toggle(backupMacGroup, (enabled));
        window.refreshMeshRole();
      })
      .catch(error => {
        console.error('[ESP-NOW] Error toggling backup master:', error);
        var cb = hw.$('backup-master-enabled');
        if (cb) cb.checked = !enabled;
        hw.showCliError(error && error.message);
      });
    };
    
    window.__topoDiscoveryInterval = null;
    window.discoverTopology = function() {
      console.log('[ESP-NOW] Discovering topology...');
      var topoDiv = hw.$('mesh-topology-data');
      var resultsDiv = hw.$('topology-results');
      hw.show(topoDiv);
      hw.setHTML(resultsDiv, 'Discovering topology... (this may take up to 10 seconds)');
      
      // Clear any existing polling interval
      if (window.__topoDiscoveryInterval) {
        clearInterval(window.__topoDiscoveryInterval);
        window.__topoDiscoveryInterval = null;
      }
      
      hw.postFormText('/api/cli', { cmd: 'espnowmeshtopo' })
      .then(output => {
        console.log('[ESP-NOW] Topology discovery response:', output);
        hw.setHTML(resultsDiv, '<pre style="margin:0;white-space:pre-wrap;color:var(--panel-fg);">' + window.topologyEscapeHtml(output) + '</pre>');
        
        // Bail early if the command itself failed
        if (window.topologyResultIsError(output)) {
          return;
        }
        
        // Poll for topology results
        var pollCount = 0;
        window.__topoDiscoveryInterval = setInterval(function() {
          pollCount++;
          window.fetchTopologyResultsPages()
          .then(results => {
            console.log('[ESP-NOW] Topology results poll ' + pollCount + ':', results);
            var hasResults = results && results.indexOf('Responses received:') >= 0 && results.indexOf('Responses received: 0') < 0;
            if (hasResults && resultsDiv) {
              resultsDiv.innerHTML = '<pre style="margin:0;white-space:pre-wrap;color:var(--panel-fg);">' + window.topologyEscapeHtml(results) + '</pre>';
            }
            if (pollCount >= 5 || hasResults) {
              clearInterval(window.__topoDiscoveryInterval);
              window.__topoDiscoveryInterval = null;
              console.log('[ESP-NOW] Topology polling stopped (count=' + pollCount + ', hasResults=' + hasResults + ')');
            }
          })
          .catch(function() {
            // A refused read (results busy) is retried with the next poll.
            if (pollCount >= 5) {
              clearInterval(window.__topoDiscoveryInterval);
              window.__topoDiscoveryInterval = null;
            }
          });
        }, 2000);
      })
      .catch(error => {
        console.error('[ESP-NOW] Topology discovery error:', error);
        // A refused command rejects with the firmware's reason as the message
        // (its "Error: " lead already taken off).
        hw.setHTML(resultsDiv, '<span style="color:var(--danger);">Error: ' + window.topologyEscapeHtml(error && error.message ? error.message : String(error)) + '</span>');
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
      var _on = function(id, evt, fn){ var el = hw.$(id); hw.on(el, evt, fn); };
      // CLI post that preserves the device's message on failure: /api/cli
      // signals a failed command with HTTP 400 and the REAL reason in the
      // body, which postFormText discards as 'HTTP 400'.
      var cliText = function(cmd){
        return hw.postForm('/api/cli', { cmd: cmd }).then(function(r){ return r.text(); });
      };
      hw.$('btn-espnow-init').addEventListener('click', function() {
        // Check if first-time setup is needed
        hw.postFormText('/api/cli', { cmd: 'espnowdeviceinfo json' })
        .then(text => {
          // If the device name is not set (empty or not a valid saved name:
          // deviceNameSet / nameValid false), show the setup modal. A reply
          // that is not json goes on to init, whose own reply says what is wrong.
          var info = window.espnowParseReply(text);
          if (info && (!info.name || info.nameValid === false || info.deviceNameSet === false)) {
            hw.$('setup-modal').classList.add('show');
            hw.$('setup-device-name').focus();
          } else {
            // Name is set, proceed with init
            return cliText('openespnow')
            .then(text => {
              hw.$('espnow-status-data').textContent = text;
              refreshStatus();
            });
          }
        })
        .catch(error => {
          hw.$('espnow-status-data').textContent = 'Error: ' + (error && error.message ? error.message : error);
        });
      });
      hw.$('btn-espnow-disable').addEventListener('click', async function() {
        if (!await hwConfirm('Disable ESP-NOW? This will stop all ESP-NOW communication. Memory will remain allocated until reboot.')) {
          return;
        }
        hw.postFormText('/api/cli', { cmd: 'closeespnow' })
        .then(text => {
          hw.$('espnow-status-data').textContent = text;
          refreshStatus();
        })
        .catch(error => {
          hw.$('espnow-status-data').textContent = 'Error: ' + (error && error.message ? error.message : error);
        });
      });
      hw.$('btn-espnow-refresh').addEventListener('click', refreshStatus);
      hw.$('btn-espnow-toggle-mode').addEventListener('click', function() {
        /* Fetch current mode, then toggle to the other */
        hw.postFormText('/api/cli', { cmd: 'espnowmode json' })
          .then(curr => {
            var modeObj = window.espnowParseReply(curr);
            if (!modeObj || typeof modeObj.mode !== 'string') throw new Error('could not read the current mode');
            var isMesh = modeObj.mode === 'mesh';
            var next = isMesh ? 'direct' : 'mesh';
            // The flag follows the mode only once the device took the change.
            return hw.postFormText('/api/cli', { cmd: 'espnowmode ' + next })
              .then(()=>{ window.espnowIsMesh = (next === 'mesh'); });
          })
          .then(()=>{ refreshStatus(); })
          .catch(e=>{ try { alert('Error: ' + e.message); } catch(_) {}; });
      });
      // Build the optional ' <meshLabel>' suffix for pair commands. Only
      // appended when the pair-mesh <select> is actually visible (i.e. when
      // 2+ meshes are configured) AND the user picked something other than
      // the default. When omitted, the CLI uses the default mesh.
      function pairMeshArg() {
        var sel = hw.$('pair-mesh');
        if (!sel) return '';
        if (sel.style.display === 'none') return '';
        if (!sel.value) return '';
        return ' ' + sel.value;
      }
      hw.$('btn-pair-secure').addEventListener('click', function() {
        const mac = hw.$('pair-mac').value.trim();
        const name = hw.$('pair-name').value.trim();
        if (!mac || !name) {
          alert('Please enter both MAC address and device name');
          return;
        }
        hw.postFormText('/api/cli', { cmd: 'espnowpairsecure ' + mac + ' ' + name + pairMeshArg() })
        .then(text => {
          if (text && text.indexOf('paired successfully') >= 0) {
            hw.$('pair-mac').value = '';
            hw.$('pair-name').value = '';
            listDevices();
            // The key exchange completes when the other device pairs this one too.
            alert('Now run the same on the other device: espnowpairsecure <this device\'s MAC> <name>.');
          }
        })
        .catch(error => {
          console.error('[ESP-NOW] Secure pair error:', error);
          hw.showCliError(error && error.message);
          // A refused pair can leave the device paired until restart: show it.
          listDevices();
        });
      });
      hw.$('btn-refresh-mesh').addEventListener('click', function() {
        console.log('[ESP-NOW] Refresh mesh button clicked');
        if (typeof window.refreshMeshStatus === 'function') {
          window.refreshMeshStatus();
        }
      });
      hw.$('btn-auto-topology').addEventListener('click', function() {
        if (typeof window.toggleAutoTopology === 'function') {
          window.toggleAutoTopology();
        }
      });
      hw.$('btn-view-topology').addEventListener('click', function() {
        if (typeof window.switchMeshView === 'function') {
          window.switchMeshView('topology');
        }
      });
      hw.$('btn-view-graph').addEventListener('click', function() {
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
      /* Device metadata button handlers */
      _on('btn-set-friendly','click', function() {
        const val = hw.$('friendly-name').value;
        hw.postFormText('/api/cli', { cmd: 'espnowfriendlyname "'+val+'"' })
          .then(()=>{ if(typeof window.loadLocalDeviceMetadata==='function')window.loadLocalDeviceMetadata(); })
          .catch(e=>{ hw.showCliError(e && e.message); if(typeof window.loadLocalDeviceMetadata==='function')window.loadLocalDeviceMetadata(); });
      });
      _on('btn-set-room','click', function() {
        const val = hw.$('room-name').value;
        hw.postFormText('/api/cli', { cmd: 'espnowroom "'+val+'"' })
          .then(()=>{ if(typeof window.loadLocalDeviceMetadata==='function')window.loadLocalDeviceMetadata(); })
          .catch(e=>{ hw.showCliError(e && e.message); if(typeof window.loadLocalDeviceMetadata==='function')window.loadLocalDeviceMetadata(); });
      });
      _on('btn-set-zone','click', function() {
        const val = hw.$('zone-name').value;
        hw.postFormText('/api/cli', { cmd: 'espnowzone "'+val+'"' })
          .then(()=>{ if(typeof window.loadLocalDeviceMetadata==='function')window.loadLocalDeviceMetadata(); })
          .catch(e=>{ hw.showCliError(e && e.message); if(typeof window.loadLocalDeviceMetadata==='function')window.loadLocalDeviceMetadata(); });
      });
      _on('btn-set-tags','click', function() {
        const val = hw.$('tags-input').value;
        hw.postFormText('/api/cli', { cmd: 'espnowtags "'+val+'"' })
          .then(()=>{ if(typeof window.loadLocalDeviceMetadata==='function')window.loadLocalDeviceMetadata(); })
          .catch(e=>{ hw.showCliError(e && e.message); if(typeof window.loadLocalDeviceMetadata==='function')window.loadLocalDeviceMetadata(); });
      });
      _on('stationary-checkbox','change', function() {
        const checked = hw.$('stationary-checkbox').checked;
        hw.postFormText('/api/cli', { cmd: 'espnowstationary '+(checked?'on':'off') })
          .then(()=>{ if(typeof window.loadLocalDeviceMetadata==='function')window.loadLocalDeviceMetadata(); })
          .catch(e=>{ hw.showCliError(e && e.message); if(typeof window.loadLocalDeviceMetadata==='function')window.loadLocalDeviceMetadata(); });
      });
      _on('btn-set-channel','click', function() {
        var sel = hw.$('channel-select');
        if (sel && typeof window.setChannel === 'function') window.setChannel(sel.value);
      });
      // NOTE: The old single-passphrase Encryption card was replaced by the
      // Meshes card. Per-row action handlers (set passphrase, clear, rename,
      // enable, disable, remove, set default) are wired in the meshes JS
      // chunk below. The standalone btn-set-passphrase / btn-clear-passphrase
      // buttons no longer exist.
      /* First-time setup modal handlers */
      _on('btn-setup-save','click', function() {
        const deviceName = hw.$('setup-device-name').value.trim();
        const errorDiv = hw.$('setup-error');
        
        if (deviceName.length === 0) {
          errorDiv.textContent = 'Please enter a device name';
          errorDiv.style.display = 'block';
          return;
        }
        // The firmware checks the name (one rule, System_DeviceName.h) and its
        // reply says why a name is refused.
        hw.postFormText('/api/cli', { cmd: 'devicename "' + deviceName.replace(/"/g, '') + '"' })
        .then(text => {
          if (text.indexOf('Error') >= 0) {
            errorDiv.textContent = text;
            errorDiv.style.display = 'block';
          } else {
            // Success! Now initialize ESP-NOW
            return cliText('openespnow')
            .then(initText => {
              if (initText.indexOf('ERROR') === 0 || initText.indexOf('Error') === 0) {
                errorDiv.textContent = initText;
                errorDiv.style.display = 'block';
                return;
              }
              // Setting the device up keeps ESP-NOW on after a restart, as the
              // OLED setup does (espnowautostart is its only saved switch).
              cliText('espnowautostart 1');
              hw.$('espnow-status-data').textContent = 'Device name set to: ' + deviceName + '\n\n' + initText;
              hw.$('setup-modal').classList.remove('show');
              hw.$('setup-device-name').value = '';
              errorDiv.style.display = 'none';
              refreshStatus();
            });
          }
        })
        .catch(error => {
          errorDiv.textContent = 'Error: ' + (error && error.message ? error.message : error);
          errorDiv.style.display = 'block';
        });
      });
      
      _on('btn-setup-cancel','click', function() {
        hw.$('setup-modal').classList.remove('show');
        hw.$('setup-device-name').value = '';
        hw.$('setup-error').style.display = 'none';
      });
      
      /* Enter key support for setup modal */
      _on('setup-device-name','keypress', function(e) {
        if (e.key === 'Enter') {
          hw.$('btn-setup-save').click();
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
    console.log('[ESP-NOW] Chunk 5c: Device metadata functions');
    window.loadLocalDeviceMetadata = function(preloadedText) {
      // text: the 'espnowdeviceinfo json' reply. An unset field is an empty
      // string; a reply that is not json leaves the inputs alone.
      function applyMetadata(text) {
        const j = window.espnowParseReply(text);
        if (!j) return;
        const friendlyInput = hw.$('friendly-name');
        const roomInput = hw.$('room-name');
        const zoneInput = hw.$('zone-name');
        const tagsInput = hw.$('tags-input');
        const stationaryCheckbox = hw.$('stationary-checkbox');
        if (friendlyInput && typeof j.friendlyName === 'string') friendlyInput.value = j.friendlyName;
        if (roomInput && typeof j.room === 'string') roomInput.value = j.room;
        if (zoneInput && typeof j.zone === 'string') zoneInput.value = j.zone;
        if (tagsInput && typeof j.tags === 'string') tagsInput.value = j.tags;
        if (stationaryCheckbox && typeof j.stationary === 'boolean') stationaryCheckbox.checked = j.stationary;
      }
      if (preloadedText !== undefined) {
        applyMetadata(preloadedText);
        return;
      }
      hw.postFormText('/api/cli', { cmd: 'espnowdeviceinfo json' })
      .then(applyMetadata)
      .catch(error => {
        console.error('[ESP-NOW] Error loading device metadata:', error);
      });
    };

    /* Preferred ESP-NOW radio channel, from the 'espnowstatus json' fields
       channelPref (0 = auto, 1-13 = pinned) and channel (the applied channel).
       statusObj is the parsed reply when the caller already has it (null when
       that reply was not json); without it the reply is fetched here. The select
       is filled once (Auto + 1-13). */
    window.refreshChannel = function(statusObj) {
      var sel = hw.$('channel-select');
      if (sel && sel.options.length === 0) {
        var opts = '<option value="auto">Auto (follow Wi-Fi)</option>';
        for (var c = 1; c <= 13; c++) opts += '<option value="' + c + '">Channel ' + c + '</option>';
        sel.innerHTML = opts;
      }
      // Both values are coerced to whole numbers in range before they are shown.
      function channelNumber(v, max) {
        var n = (typeof v === 'number') ? v : NaN;
        return (n >= 1 && n <= max && Math.floor(n) === n) ? n : 0;
      }
      function applyChannel(s) {
        var statusDiv = hw.$('channel-status');
        var prefNum = s ? channelNumber(s.channelPref, 13) : 0;
        var radioNum = s ? channelNumber(s.channel, 14) : 0;
        if (sel) sel.value = prefNum ? String(prefNum) : 'auto';
        if (statusDiv) {
          statusDiv.innerHTML =
            '<strong>Preference:</strong> ' + (prefNum ? ('Channel ' + prefNum) : 'Auto (follow Wi-Fi)') +
            '<br><strong>Radio now:</strong> channel ' + (radioNum ? radioNum : '?') +
            '<br><span style="color:var(--muted)">Set the same value on both devices to pair off-grid.</span>';
        }
      }
      if (statusObj !== undefined) {
        applyChannel(statusObj);
        return;
      }
      hw.postFormText('/api/cli', { cmd: 'espnowstatus json' })
      .then(function(text) { applyChannel(window.espnowParseReply(text)); })
      .catch(function(error) {
        var statusDiv = hw.$('channel-status');
        hw.setHTML(statusDiv, '<span style="color:var(--danger);">Error: ' + window.topologyEscapeHtml(error && error.message ? error.message : String(error)) + '</span>');
      });
    };

    window.setChannel = function(val) {
      console.log('[ESP-NOW] Setting channel to:', val);
      hw.postFormText('/api/cli', { cmd: 'espnowchannel ' + val })
      .then(function(output) {
        alert(output);
        window.refreshChannel();
      })
      .catch(function(error) { alert('Error setting channel: ' + error); });
    };
    console.log('[ESP-NOW] Chunk 5c: Device metadata functions ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 5c error:', e); }
})();
</script>
<script>
(function() {
  try {
    console.log('[ESP-NOW] Chunk 5d: Meshes (multi-mesh management)');

    // window.gMeshes is the cached meshes JSON, kept so other UI bits (the
    // pair-form chooser, the device-row badge resolver) can look up labels
    // without re-fetching.
    window.gMeshes = { nMeshes: 4, configuredCount: 0, defaultSlot: -1, meshes: [] };

    function escHtml(s) {
      if (s === null || s === undefined) return '';
      return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
    }

    // Resolve a meshId (slot index) to its label. Used by the device list to
    // render the per-row mesh badge.
    window.meshLabelForSlot = function(slot) {
      var m = (window.gMeshes && window.gMeshes.meshes) || [];
      for (var i = 0; i < m.length; i++) {
        if (m[i].slot === slot) return m[i].label;
      }
      return '';
    };
    window.meshEnabledForSlot = function(slot) {
      var m = (window.gMeshes && window.gMeshes.meshes) || [];
      for (var i = 0; i < m.length; i++) {
        if (m[i].slot === slot) return !!m[i].enabled;
      }
      return false;
    };

    function renderMeshesTable(data) {
      var tbl = hw.$('meshes-table');
      var addRow = hw.$('meshes-add-row');
      var fullMsg = hw.$('meshes-full-msg');
      var activeLabel = hw.$('mesh-active-label');
      if (!tbl) return;

      // Active mesh pill
      var defLabel = '(none)';
      if (data && Array.isArray(data.meshes)) {
        for (var i = 0; i < data.meshes.length; i++) {
          if (data.meshes[i].isDefault && data.meshes[i].enabled) {
            defLabel = data.meshes[i].label;
            break;
          }
        }
      }
      hw.setText(activeLabel, defLabel);

      // Empty state
      if (!data || !Array.isArray(data.meshes) || data.meshes.length === 0) {
        tbl.innerHTML = '<div style="color:var(--muted);font-size:.85em;padding:8px 0">No meshes configured.</div>';
        hw.show(addRow);
        hw.hide(fullMsg);
        return;
      }

      // Render rows. Step 2 = skeleton, no actions wired yet. Step 3 will
      // attach handlers + inline-expand passphrase/rename forms.
      var html = '';
      for (var j = 0; j < data.meshes.length; j++) {
        var m = data.meshes[j];
        var label = escHtml(m.label);
        var enabledPill = m.enabled
          ? '<span style="font-size:.75em;padding:2px 8px;border-radius:6px;background:rgba(80,200,120,.15);color:#3a9d5d;border:1px solid rgba(80,200,120,.3)">enabled</span>'
          : '<span style="font-size:.75em;padding:2px 8px;border-radius:6px;background:rgba(200,120,80,.15);color:#b86a3a;border:1px solid rgba(200,120,80,.3)">disabled</span>';
        var defaultPill = m.isDefault
          ? '<span style="font-size:.75em;padding:2px 8px;border-radius:6px;background:var(--accent,#4a90e2);color:#fff">default</span>'
          : '';
        var passPill = m.hasPassphrase
          ? '<span style="font-size:.78em;color:var(--panel-fg)">passphrase ✓</span>'
          : '<span style="font-size:.78em;color:var(--muted)">no passphrase</span>';
        // The mesh whose passphrase the device itself uses (the fingerprint in
        // espnowencstatus, needed for sending and pairing); while ESP-NOW is
        // stopped, the one its next start loads. The firmware applies a change at once.
        if (m.slot === data.passphraseSlot) {
          passPill += data.running
            ? ' <span class="mesh-device-pass" title="This device uses the passphrase of this mesh now: its fingerprint is in espnowencstatus, and sending and pairing need one" style="font-size:.75em;color:var(--muted)">(device passphrase)</span>'
            : ' <span class="mesh-device-pass" title="ESP-NOW is stopped; its next start uses the passphrase of this mesh" style="font-size:.75em;color:var(--muted)">(device passphrase at next start)</span>';
        }

        // Prominent Enable button when disabled (per UX decision); the
        // catch-all action menu lives behind the ⋯ button for the rest.
        var primaryAction = m.enabled
          ? '<button class="btn btn-mesh-actions" data-guest-hide data-slot="' + m.slot + '" data-label="' + label + '" style="font-size:.82em;padding:4px 10px">⋯</button>'
          : '<button class="btn btn-mesh-enable" data-guest-hide data-slot="' + m.slot + '" data-label="' + label + '" style="font-size:.82em;padding:4px 10px">Enable</button>' +
            '<button class="btn btn-mesh-delete" data-guest-hide data-slot="' + m.slot + '" data-label="' + label + '"' + ((typeof window.__hwRank === 'number' && window.__hwRank < ((window.__hwRoleRank || {superadmin: 3}).superadmin)) ? ' disabled title="Super admin only"' : ' title="Free this slot. Devices paired in this mesh must be unpaired first."') + ' style="font-size:.82em;padding:4px 10px">Delete</button>';

        html += '<div class="mesh-row" data-slot="' + m.slot + '" data-label="' + label + '" style="display:flex;align-items:center;gap:8px;padding:8px 10px;border-radius:8px;background:var(--crumb-bg);border:1px solid var(--border);flex-wrap:wrap">' +
          '<span style="font-weight:600;color:var(--panel-fg);min-width:80px">' + label + '</span>' +
          enabledPill +
          defaultPill +
          '<span style="font-size:.78em;color:var(--muted);font-family:monospace">' + escHtml(m.fingerprintHex || '') + '</span>' +
          passPill +
          '<span style="flex:1"></span>' +
          primaryAction +
          '</div>' +
          '<div class="mesh-row-actions" data-slot="' + m.slot + '" style="display:none;padding:6px 10px;background:var(--bg);border-radius:8px;border:1px solid var(--border);margin-top:-2px"></div>';
      }
      tbl.innerHTML = html;

      // Show "+ Add mesh" form only if there's a free slot.
      var hasFreeSlot = (data.configuredCount || 0) < (data.nMeshes || 4);
      hw.toggle(addRow, (hasFreeSlot));
      hw.toggle(fullMsg, !(hasFreeSlot));
    }

    // Update the pair-form mesh chooser. Only visible when 2+ enabled meshes
    // exist (per UX decision D4). Default mesh is preselected.
    function updatePairChooser(data) {
      var sel = hw.$('pair-mesh');
      if (!sel) return;
      var meshes = (data && Array.isArray(data.meshes)) ? data.meshes.filter(function(m) { return m.enabled; }) : [];
      if (meshes.length < 2) {
        sel.style.display = 'none';
        sel.innerHTML = '';
        return;
      }
      var html = '';
      for (var i = 0; i < meshes.length; i++) {
        var sa = meshes[i].isDefault ? ' selected' : '';
        html += '<option value="' + escHtml(meshes[i].label) + '"' + sa + '>mesh: ' + escHtml(meshes[i].label) + '</option>';
      }
      sel.innerHTML = html;
      sel.style.display = '';
    }

    // ---- CLI command bridge -------------------------------------------------
    // All mesh management actions go through this — fires the CLI command,
    // logs the response, then refreshes the meshes table.
    // A refused command is an HTTP 400 and arrives as a rejection: its reason
    // is shown here and the tables are refreshed all the same, because a
    // command that failed at the save has still changed the mesh in RAM. The
    // rejection is passed on so a caller's .then does not run; every caller
    // ends its chain with .catch(meshCmdFailureShown).
    function fireMeshCmd(cmd, logTag) {
      function refreshTables() {
        if (typeof window.loadMeshes === 'function') window.loadMeshes();
        // Refresh device list too — meshId stamping or mesh enable state may
        // change which peers are reachable / how badges render.
        if (typeof window.listDevices === 'function') window.listDevices();
      }
      return hw.postFormText('/api/cli', { cmd: cmd })
      .then(function(text) {
        console.log('[MESH] ' + (logTag || '') + ': ' + text);
        refreshTables();
        return text;
      }, function(e) {
        console.warn('[MESH] ' + (logTag || '') + ' failed:', e);
        // The page is on its way to the login screen: nothing to show.
        if (e && e.message === 'auth_required') throw e;
        refreshTables();
        alert((e && e.reply) ? String(e.reply).trim() : ('Error: ' + (e && e.message ? e.message : e)));
        throw e;
      });
    }
    function meshCmdFailureShown() {}

    // ---- Per-row inline action panel ---------------------------------------
    function renderActionPanel(slot, label, isDefault, hasPassphrase) {
      // Escaped for the attributes: a label may hold '&' and ';', and the click
      // handler reads it back from dataset, which decodes it again.
      var lab = escHtml(label);
      var setpassText = hasPassphrase ? 'Change passphrase' : 'Set passphrase';
      // The default mesh can be disabled only when no other enabled mesh could
      // take over; the firmware applies the same rule.
      var all = (window.gMeshes && window.gMeshes.meshes) || [];
      var otherEnabled = all.some(function(o) { return o.enabled && String(o.slot) !== String(slot); });
      // Clearing slot 0's passphrase also disables its mesh, so the firmware
      // refuses it under the same rule as Disable.
      var clearpassBtn = !hasPassphrase
        ? ''
        : (String(slot) === '0' && isDefault && otherEnabled)
          ? '<button class="btn" disabled title="Clearing this passphrase disables the default mesh — set another as default first" style="font-size:.82em;padding:3px 10px;opacity:.5;cursor:not-allowed">Clear passphrase</button>'
          : '<button class="btn btn-mesh-clearpass" data-label="' + lab + '" data-slot="' + slot + '" style="font-size:.82em;padding:3px 10px">Clear passphrase</button>';
      var setdefaultBtn = isDefault
        ? ''
        : '<button class="btn btn-mesh-setdefault" data-label="' + lab + '" style="font-size:.82em;padding:3px 10px">Set as default</button>';
      var disableBtn = (isDefault && otherEnabled)
        ? '<button class="btn" disabled title="Cannot disable the default mesh — set another as default first" style="font-size:.82em;padding:3px 10px;opacity:.5;cursor:not-allowed">Disable</button>'
        : '<button class="btn btn-mesh-disable" data-label="' + lab + '" style="font-size:.82em;padding:3px 10px">Disable</button>';

      return '<div style="display:flex;flex-wrap:wrap;gap:6px;align-items:center">' +
        '<button class="btn btn-mesh-setpass" data-label="' + lab + '" data-slot="' + slot + '" style="font-size:.82em;padding:3px 10px">' + setpassText + '</button>' +
        clearpassBtn +
        '<button class="btn btn-mesh-rename" data-label="' + lab + '" data-slot="' + slot + '" style="font-size:.82em;padding:3px 10px">Rename</button>' +
        setdefaultBtn +
        disableBtn +
        '<button class="btn btn-mesh-close" data-slot="' + slot + '" style="font-size:.82em;padding:3px 10px;margin-left:auto">Close</button>' +
        '</div>' +
        '<div class="mesh-subform"></div>';
    }

    function findMesh(slot) {
      var m = (window.gMeshes && window.gMeshes.meshes) || [];
      for (var i = 0; i < m.length; i++) if (m[i].slot === parseInt(slot, 10)) return m[i];
      return null;
    }

    function toggleActionPanel(slot) {
      var mesh = findMesh(slot);
      if (!mesh) return;
      var panel = hw.qs('.mesh-row-actions[data-slot="' + slot + '"]');
      if (!panel) return;
      // Close any other open panels first
      hw.qsa('.mesh-row-actions').forEach(function(p) {
        if (p !== panel) { p.style.display = 'none'; p.innerHTML = ''; }
      });
      // Toggle this one
      if (panel.style.display === 'none' || !panel.style.display) {
        panel.innerHTML = renderActionPanel(slot, mesh.label, !!mesh.isDefault, !!mesh.hasPassphrase);
        panel.style.display = 'block';
      } else {
        panel.style.display = 'none';
        panel.innerHTML = '';
      }
    }

    function closeAllActionPanels() {
      hw.qsa('.mesh-row-actions').forEach(function(p) {
        p.style.display = 'none';
        p.innerHTML = '';
      });
    }

    // Inline-expand passphrase entry (UX decision D1).
    function showPassphraseSubform(slot, label) {
      var panel = hw.qs('.mesh-row-actions[data-slot="' + slot + '"]');
      if (!panel) return;
      var sf = panel.querySelector('.mesh-subform');
      if (!sf) return;
      sf.innerHTML =
        '<div style="margin-top:8px;background:var(--warning-bg);border:1px solid var(--warning-border);color:var(--warning-fg);padding:10px;border-radius:8px;font-size:.85em;line-height:1.4">' +
          '<strong>Click Save only once.</strong> Deriving and saving the mesh key takes about a minute — the button will look idle but work is happening in the background. ' +
          'Clicking again will queue duplicate work and can corrupt the saved key. Wait for the page to refresh on its own.' +
        '</div>' +
        '<div class="en-form-row" style="margin-top:8px">' +
        '<input type="password" class="mesh-pass-input" placeholder="New passphrase (min 8 chars)" maxlength="64">' +
        '<button class="btn btn-mesh-savepass" data-label="' + escHtml(label) + '" data-slot="' + slot + '" style="font-size:.82em">Save</button>' +
        '<button class="btn btn-mesh-cancelsub" data-slot="' + slot + '" style="font-size:.82em">Cancel</button>' +
        '</div>';
      var inp = sf.querySelector('.mesh-pass-input');
      if (inp) inp.focus();
    }

    function showRenameSubform(slot, label) {
      var panel = hw.qs('.mesh-row-actions[data-slot="' + slot + '"]');
      if (!panel) return;
      var sf = panel.querySelector('.mesh-subform');
      if (!sf) return;
      sf.innerHTML = '<div class="en-form-row" style="margin-top:8px">' +
        '<input type="text" class="mesh-rename-input" placeholder="New label" maxlength="16" value="' + escHtml(label) + '">' +
        '<button class="btn btn-mesh-saverename" data-oldlabel="' + escHtml(label) + '" data-slot="' + slot + '" style="font-size:.82em">Save</button>' +
        '<button class="btn btn-mesh-cancelsub" data-slot="' + slot + '" style="font-size:.82em">Cancel</button>' +
        '</div>';
      var inp = sf.querySelector('.mesh-rename-input');
      if (inp) { inp.focus(); inp.select(); }
    }

    function clearSubform(slot) {
      var panel = hw.qs('.mesh-row-actions[data-slot="' + slot + '"]');
      if (!panel) return;
      var sf = panel.querySelector('.mesh-subform');
      hw.setHTML(sf, '');
    }

    // ---- Event delegation: one listener for the whole meshes card ----------
    function attachMeshDelegation() {
      var card = hw.$('meshes-card');
      if (!card || card.__meshHandlersAttached) return;
      card.__meshHandlersAttached = true;
      card.addEventListener('click', function(e) {
        var t = e.target;
        if (!t || !t.classList) return;
        var slot = t.dataset ? t.dataset.slot : null;
        var label = t.dataset ? t.dataset.label : null;

        // Add a new mesh
        if (t.id === 'btn-mesh-add') {
          var inp = hw.$('mesh-add-label');
          var lbl = inp ? inp.value.trim() : '';
          if (!lbl) { alert('Please enter a mesh label'); return; }
          fireMeshCmd('espnowmeshes add ' + lbl, 'MESH_ADD').then(function() {
            if (inp) inp.value = '';
          }).catch(meshCmdFailureShown);
          return;
        }
        // Re-enable a disabled mesh
        if (t.classList.contains('btn-mesh-enable')) {
          if (!label) return;
          fireMeshCmd('espnowmeshes enable ' + label, 'MESH_ENABLE').catch(meshCmdFailureShown);
          return;
        }
        // Delete a disabled mesh: frees its slot. The firmware refuses while
        // paired devices still belong to it; fireMeshCmd shows that answer.
        if (t.classList.contains('btn-mesh-delete')) {
          if (!label) return;
          if (!confirm('Delete mesh "' + label + '"? Its label and passphrase are erased and the slot becomes free.')) return;
          fireMeshCmd('espnowmeshes delete ' + label, 'MESH_DELETE').catch(meshCmdFailureShown);
          return;
        }
        // Open/close action panel
        if (t.classList.contains('btn-mesh-actions')) {
          toggleActionPanel(slot);
          return;
        }
        if (t.classList.contains('btn-mesh-close')) {
          closeAllActionPanels();
          return;
        }
        // Set/change passphrase — opens inline form
        if (t.classList.contains('btn-mesh-setpass')) {
          showPassphraseSubform(slot, label);
          return;
        }
        if (t.classList.contains('btn-mesh-savepass')) {
          var panel = hw.qs('.mesh-row-actions[data-slot="' + slot + '"]');
          var passInp = panel ? panel.querySelector('.mesh-pass-input') : null;
          var pw = passInp ? passInp.value : '';
          if (!pw) { alert('Please enter a passphrase'); return; }
          if (pw.length < 8) { alert('Passphrase must be at least 8 characters'); return; }
          // The passphrase is sent in double quotes and the CLI has no escape
          // for one inside them, so the firmware refuses such a passphrase.
          if (pw.indexOf('"') >= 0) { alert('The passphrase cannot contain a double quote'); return; }
          // Wrap in quotes so the CLI parser keeps it as a single token. The
          // answer says what applies now and what other devices need, so show it.
          fireMeshCmd('espnowsetpassphrase ' + label + ' "' + pw + '"', 'MESH_PASS').then(function(text) {
            if (text) alert(text);
          }).catch(meshCmdFailureShown);
          return;
        }
        // Clear passphrase
        if (t.classList.contains('btn-mesh-clearpass')) {
          // The firmware disables slot 0's mesh when its passphrase is cleared, and
          // a disabled mesh's frames are dropped, so that dialog has to say so.
          var clearMsg = String(slot) === '0'
            ? 'Clear passphrase for mesh "' + label + '"? This also disables the mesh: its paired devices cannot exchange frames with this device until you enable it again.'
            : 'Clear passphrase for mesh "' + label + '"? New pairings on this mesh will need a passphrase again, and its heartbeats and time sync stop getting through to devices that still have one. Paired devices keep their keys.';
          if (!confirm(clearMsg)) return;
          fireMeshCmd('espnowsetpassphrase ' + label + ' clear', 'MESH_PASS_CLEAR').then(function(text) {
            if (text) alert(text);
          }).catch(meshCmdFailureShown);
          return;
        }
        // Rename — opens inline form
        if (t.classList.contains('btn-mesh-rename')) {
          showRenameSubform(slot, label);
          return;
        }
        if (t.classList.contains('btn-mesh-saverename')) {
          var oldLabel = t.dataset.oldlabel;
          var panel2 = hw.qs('.mesh-row-actions[data-slot="' + slot + '"]');
          var nameInp = panel2 ? panel2.querySelector('.mesh-rename-input') : null;
          var newLabel = nameInp ? nameInp.value.trim() : '';
          if (!newLabel) { alert('Please enter a new label'); return; }
          if (newLabel === oldLabel) { clearSubform(slot); return; }
          fireMeshCmd('espnowmeshes rename ' + oldLabel + ' ' + newLabel, 'MESH_RENAME').catch(meshCmdFailureShown);
          return;
        }
        // Cancel sub-form
        if (t.classList.contains('btn-mesh-cancelsub')) {
          clearSubform(slot);
          return;
        }
        // Set as default
        if (t.classList.contains('btn-mesh-setdefault')) {
          fireMeshCmd('espnowmeshes setdefault ' + label, 'MESH_DEFAULT').catch(meshCmdFailureShown);
          return;
        }
        // Disable (alias for remove — soft delete)
        if (t.classList.contains('btn-mesh-disable')) {
          if (!confirm('Disable mesh "' + label + '"? Paired peers in this mesh will stop receiving frames until you re-enable it.')) return;
          fireMeshCmd('espnowmeshes remove ' + label, 'MESH_DISABLE').catch(meshCmdFailureShown);
          return;
        }
      });
    }

    // Public entry point. Accepts an optional preloaded JSON string from the
    // batch refresh; otherwise fetches fresh.
    window.loadMeshes = function(preloadedJsonText) {
      function apply(jsonText) {
        var data = null;
        try { data = JSON.parse(jsonText); }
        catch (e) {
          console.warn('[ESP-NOW] loadMeshes: JSON parse failed:', e, 'raw=', jsonText);
          return;
        }
        if (data && data.error) {
          // Most commonly "ESP-NOW not initialized" before openespnow.
          console.log('[ESP-NOW] loadMeshes: backend not ready:', data.error);
          var tbl = hw.$('meshes-table');
          hw.setHTML(tbl, '<div style="color:var(--muted);font-size:.85em;padding:8px 0">ESP-NOW not initialized.</div>');
          return;
        }
        window.gMeshes = data;
        renderMeshesTable(data);
        updatePairChooser(data);
        attachMeshDelegation();
      }

      if (preloadedJsonText !== undefined) {
        apply(preloadedJsonText);
        return;
      }
      // Standalone (non-batch) fetch
      hw.postFormText('/api/cli', { cmd: 'espnowmeshes listjson' })
      .then(apply)
      .catch(function(err) {
        console.error('[ESP-NOW] loadMeshes error:', err);
      });
    };

    console.log('[ESP-NOW] Chunk 5d: Meshes ready (full)');
  } catch(e) { console.error('[ESP-NOW] Chunk 5d error:', e); }
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
  var refused = false;      // the message history is not this account's to read

  // The message history is admin-only, and private from a guest (owner, guest
  // privacy 1): its 403 says which (code guest_private / admin_required). The
  // conversation logs then say so instead of "No messages yet", and polling
  // stops. Panels drawn later read window.__espnowMessagesRefused
  // (renderDevicePanel in espnow-core.js).
  function messagesRefusedText(e) {
    var code = '';
    try { code = (JSON.parse((e && e.reply) || '{}') || {}).code || ''; } catch (_) {}
    return (code === 'guest_private')
      ? 'Message history is not available to guests.'
      : 'Message history is for administrators only.';
  }
  function showMessagesRefused(text) {
    window.__espnowMessagesRefused = text;
    var logs = document.querySelectorAll('.message-log');
    for (var i = 0; i < logs.length; i++) {
      var empty = logs[i].querySelector('.message-empty');
      if (empty) empty.textContent = text;
    }
  }
  var pendingGroups = {};   // reqId -> accumulating multi-piece (chunked) message
  var pollTick = 0;

  // Chunked text: a long message arrives as N pieces that share a reqId, each
  // tagged piece/of. The device stores them as separate small records and never
  // reassembles — WE stitch here, client-side, where RAM is free. Single-frame
  // messages (of<=1) render immediately, exactly as before.
  // sendState mirrors the device's SendStatusState: 0 pending, 1 delivered,
  // 2 timeout/no-ACK, 3 failed. For SENT rows we render the durable state from
  // the record (so it's right for messages sent elsewhere or after a reload) and
  // tag the bubble with data-msg-id so the live deliveries[] flip can still
  // upgrade pending → delivered within the tracker's window.
  function renderMessage(mac, text, partial, isSent, sendState, reqId) {
    if (typeof window.appendLogLine !== 'function') { console.error('[ESP-NOW] appendLogLine not available'); return; }
    if (!isSent) {
      window.appendLogLine('log-' + mac, 'RECEIVED', partial ? (text + '  …(partial — some pieces missing)') : text, null);
      return;
    }
    var bubble = window.appendLogLine('log-' + mac, 'SENT', text, (sendState === 1) ? 'delivered' : 'sent');
    if (!bubble) return;
    if (reqId) bubble.setAttribute('data-msg-id', reqId);
    if (sendState === 2 || sendState === 3) {
      var sd = bubble.querySelector('.message-status');
      if (sd) {
        sd.innerHTML = (sendState === 2) ? '<span class="status-icon">✗</span>No ACK (timeout)'
                                         : '<span class="status-icon">✗</span>Failed';
        bubble.classList.add('message-error');
      }
    }
  }
  function handleIncomingMessage(msg) {
    var mac = (msg.mac || '').toUpperCase();
    if (!mac) return;
    var text = msg.msg || '';
    var of = msg.of || 1;
    var piece = msg.piece || 1;
    var reqId = msg.reqId || 0;
    var isSent = !!msg.sent;
    var sendState = msg.sendState || 0;
    // Sent messages now come from the shared device history (so the OLED/BLE see
    // our sends and we see theirs). If this is the polled echo of a message WE
    // just sent from this browser, its optimistic bubble is already on screen
    // tagged with data-msg-id — skip it so it doesn't double. A sent message with
    // no matching bubble originated on another interface → render it as SENT.
    if (isSent && reqId &&
        hw.qs('.message-bubble[data-msg-id="' + reqId + '"]')) {
      return;
    }
    if (of <= 1 || !reqId) { renderMessage(mac, text, false, isSent, sendState, reqId); return; }  // not chunked
    var g = pendingGroups[reqId];
    if (!g) { g = pendingGroups[reqId] = { of: of, mac: mac, parts: {}, count: 0, tick: pollTick, isSent: isSent, sendState: sendState, reqId: reqId }; }
    g.sendState = sendState;  // latest fragment's state (all share it once resolved)
    if (!(piece in g.parts)) { g.parts[piece] = text; g.count++; }
    if (g.count >= g.of) {  // all pieces in — join in order and render once
      var full = '';
      for (var k = 1; k <= g.of; k++) full += (g.parts[k] || '');
      renderMessage(g.mac, full, false, g.isSent, g.sendState, g.reqId);
      delete pendingGroups[reqId];
    }
  }
  function flushStaleGroups() {
    // A group whose pieces were lost or aged out of the device ring will never
    // complete; after ~10s (20 polls) render what we have as a partial so it
    // doesn't accumulate forever.
    for (var id in pendingGroups) {
      var g = pendingGroups[id];
      if (pollTick - g.tick > 20) {
        var full = '';
        for (var k = 1; k <= g.of; k++) full += (g.parts[k] || '[missing] ');
        renderMessage(g.mac, full, true, g.isSent, g.sendState, g.reqId);
        delete pendingGroups[id];
      }
    }
  }

  function pollEspNowMessages() {
    if (authFailed || refused) return;
    pollTick++;
    flushStaleGroups();
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
        if (!data) return;
        if (data.messages) {
          console.log('[ESP-NOW] Processing ' + data.messages.length + ' messages');
          data.messages.forEach(function(msg){
            if (msg.seq > lastSeqNum) lastSeqNum = msg.seq;
            handleIncomingMessage(msg);  // groups chunked pieces by reqId, renders when complete
          });
        }
        // Phase 3.5 task #49 — apply delivery-state updates to bubbles tagged
        // with data-msg-id. The server sends a full snapshot of its tracked-
        // send ring on every poll; we update every bubble we can match. Idem-
        // potent — re-applying the same delivered state is a no-op.
        if (data.deliveries && Array.isArray(data.deliveries)) {
          data.deliveries.forEach(function(d){
            var bubble = hw.qs('.message-bubble[data-msg-id="' + d.msgId + '"]');
            if (!bubble) return;
            var statusDiv = bubble.querySelector('.message-status');
            if (!statusDiv) return;
            if (d.state === 'delivered') {
              statusDiv.innerHTML = '<span class="status-icon">✓✓</span>Delivered';
            } else if (d.state === 'timeout') {
              statusDiv.innerHTML = '<span class="status-icon">✗</span>No ACK (timeout)';
              bubble.classList.add('message-error');
            } else if (d.state === 'failed') {
              statusDiv.innerHTML = '<span class="status-icon">✗</span>Failed (handshake)';
              bubble.classList.add('message-error');
            }
            // 'pending' — leave the bubble at whatever the send-side set
            // (Sent / Queued); no transition needed.
          });
        }
      })
      .catch(function(e){ 
        if (e && e.message === 'auth_required') {
          console.log('[ESP-NOW] Auth required, stopping polling');
          authFailed = true;
          stopPolling();
        } else if (e && e.status === 403) {
          refused = true;
          stopPolling();
          showMessagesRefused(messagesRefusedText(e));
        } else {
          console.error('[ESP-NOW] Poll error:', e); 
        }
      });
  }
  
  function startPolling() {
    if (pollInterval || authFailed || refused) return;
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
  httpd_resp_send_chunk(req, getFileBrowserScript(), HTTPD_RESP_USE_STRLEN);
}

void registerEspNowHandlers(httpd_handle_t server);

#endif
