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
.espnow-container { max-width: 1200px; margin: 0 auto; padding: 20px; }
.espnow-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(400px, 1fr)); gap: 20px; margin-bottom: 30px; }
.espnow-card { background: var(--panel-bg); border-radius: 15px; padding: 20px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); border: 1px solid var(--border); overflow: hidden; }
.espnow-title { font-size: 1.3em; font-weight: bold; margin-bottom: 10px; color: var(--panel-fg); display: flex; align-items: center; gap: 10px; }
.espnow-description { color: var(--muted); margin-bottom: 15px; font-size: 0.9em; }
.espnow-controls { display: flex; gap: 10px; margin-bottom: 15px; flex-wrap: wrap; }
.espnow-data { background: var(--crumb-bg); border-radius: 8px; padding: 15px; font-family: 'Courier New', monospace; font-size: 0.9em; border-left: 4px solid var(--link); min-height: 60px; color: var(--panel-fg); }
.status-indicator { display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }
.status-enabled { background: #28a745; animation: pulse 2s infinite; }
.status-disabled { background: #dc3545; }
@keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.5; } 100% { opacity: 1; } }
.device-list { background: var(--crumb-bg); border-radius: 8px; padding: 15px; margin-bottom: 15px; }
.device-item { display: flex; justify-content: space-between; align-items: center; padding: 8px 0; border-bottom: 1px solid var(--border); }
.device-item:last-child { border-bottom: none; }
.device-mac { font-family: 'Courier New', monospace; font-weight: bold; color: var(--link); }
.device-channel { color: var(--muted); font-size: 0.9em; }
.device-actions { display: flex; gap: 5px; }
.btn-small { padding: 4px 8px; font-size: 0.8em; }
.device-encrypted { color: var(--success); font-weight: bold; }
.device-unencrypted { color: var(--muted); }
.encryption-indicator { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-left: 8px; }
.encryption-enabled { background: var(--success); }
.encryption-disabled { background: var(--muted); }
.message-log { background: var(--crumb-bg); border-radius: 8px; padding: 15px; max-height: 300px; overflow-y: auto; border: 1px solid var(--border); display: flex; flex-direction: column; gap: 8px; }
.message-bubble { max-width: 75%; width: fit-content; padding: 10px 14px; border-radius: 16px; position: relative; word-wrap: break-word; animation: slideIn 0.2s ease-out; }
@keyframes slideIn { from { opacity: 0; transform: translateY(10px); } to { opacity: 1; transform: translateY(0); } }
.message-received { align-self: flex-start; background: rgba(128,128,128,0.25); color: var(--panel-fg); border-bottom-left-radius: 4px; border: 1px solid rgba(128,128,128,0.15); }
.message-sent { align-self: flex-end; background: #2563eb; color: #fff; border-bottom-right-radius: 4px; }
.message-error { align-self: flex-end; background: var(--danger); color: var(--panel-fg); border-bottom-right-radius: 4px; }
.message-text { margin: 0; font-size: 0.95em; line-height: 1.4; }
.message-status { font-size: 0.75em; margin-top: 4px; opacity: 0.8; display: flex; align-items: center; gap: 4px; }
.message-received .message-status { color: var(--muted); }
.message-sent .message-status { color: rgba(255,255,255,0.7); }
.message-error .message-status { color: var(--muted); }
.status-icon { display: inline-block; }
.message-empty { text-align: center; color: var(--muted); padding: 20px; font-style: italic; }
.input-group { display: flex; gap: 10px; margin-bottom: 10px; flex-wrap: wrap; width: 100%; }
.input-group input { flex: 1 1 200px; max-width: 100%; min-width: 0; box-sizing: border-box; }
.mesh-warning { display:none; background:var(--warning-bg); border:1px solid var(--warning-border); color:var(--warning-fg); border-left:4px solid var(--warning-accent); padding:12px; border-radius:8px; margin-top:10px; }
#pair-mac { flex: 1 1 260px; min-width: 200px; }
#pair-name { flex: 2 1 360px; min-width: 220px; }
.mac-input { font-family: 'Courier New', monospace; }
.espnow-container > .espnow-card + .espnow-card { margin-top: 30px; }
.setup-modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.7); z-index: 10000; align-items: center; justify-content: center; }
.setup-modal.show { display: flex; }
.setup-modal-content { background: var(--panel-bg); border-radius: 15px; padding: 30px; max-width: 500px; width: 90%; box-shadow: 0 10px 40px rgba(0,0,0,0.3); }
.setup-modal-title { font-size: 1.5em; font-weight: bold; margin-bottom: 15px; color: var(--panel-fg); }
.setup-modal-description { color: var(--muted); margin-bottom: 20px; line-height: 1.6; }
.setup-modal-input { width: 100%; padding: 12px; border: 2px solid var(--border); border-radius: 8px; font-size: 1em; margin-bottom: 15px; box-sizing: border-box; background: var(--panel-bg); color: var(--panel-fg); }
.setup-modal-input:focus { outline: none; border-color: var(--link); }
.setup-modal-buttons { display: flex; gap: 10px; justify-content: flex-end; }
.setup-modal-error { color: var(--danger); margin-bottom: 15px; padding: 10px; background: var(--warning-bg); border-radius: 5px; display: none; }
.setup-modal-requirements { background: var(--crumb-bg); padding: 12px; border-radius: 8px; margin-bottom: 15px; font-size: 0.9em; color: var(--muted); }
.setup-modal-requirements ul { margin: 8px 0 0 20px; padding: 0; }
</style>
)CSS", HTTPD_RESP_USE_STRLEN);
  
  // HTML structure
  httpd_resp_send_chunk(req, R"HTML(
<div class='espnow-container'>
<div class='espnow-grid'>
<div class='espnow-card'>
<div class='espnow-title'>
<span>ESP-NOW Status</span>
<span class='status-indicator status-disabled' id='espnow-status-indicator'></span>
</div>
<div class='espnow-description'>ESP-NOW wireless communication protocol for direct device-to-device messaging.</div>
<div id='resource-warning' class='alert alert-warning' style='margin-bottom:15px;'>
<strong>Resource Usage:</strong> Initializing ESP-NOW allocates ~10-15 KB heap (task stack, buffers, peer storage). Disabling ESP-NOW will not fully free memory, this memory remains allocated until device reboot.
</div>
<div class='espnow-controls'>
<button class='btn' id='btn-espnow-init' style='display:none'>Initialize ESP-NOW</button>
<button class='btn' id='btn-espnow-disable' style='display:none'>Disable ESP-NOW</button>
<button class='btn' id='btn-espnow-refresh'>Refresh Status</button>
<button class='btn' id='btn-espnow-toggle-mode' style='display:none'>Mode: Direct</button>
</div>
<div class='espnow-data' id='espnow-status-data'>Click 'Refresh Status' to check ESP-NOW status...</div>
</div>
<div class='espnow-card' id='smarthome-card' style='display:none;'>
<div class='espnow-title'>Smart Home Metadata</div>
<div class='espnow-description'>Configure device metadata for home automation, room assignment, and organization.</div>
<div class='input-group'>
<input type='text' id='friendly-name' placeholder='Friendly Name (e.g., Living Room Light)' maxlength='47'>
<button class='btn' id='btn-set-friendly'>Set Name</button>
</div>
<div class='input-group' style='margin-top:10px;'>
<input type='text' id='room-name' placeholder='Room (e.g., Living Room)' maxlength='31'>
<button class='btn' id='btn-set-room'>Set Room</button>
</div>
<div class='input-group' style='margin-top:10px;'>
<input type='text' id='zone-name' placeholder='Zone (e.g., Upstairs)' maxlength='31'>
<button class='btn' id='btn-set-zone'>Set Zone</button>
</div>
<div class='input-group' style='margin-top:10px;'>
<input type='text' id='tags-input' placeholder='Tags (comma-separated, e.g., light,dimmable)' maxlength='63'>
<button class='btn' id='btn-set-tags'>Set Tags</button>
</div>
<div style='margin-top:15px;display:flex;align-items:center;gap:10px;'>
<label style='display:flex;align-items:center;gap:8px;cursor:pointer;'>
<input type='checkbox' id='stationary-checkbox' style='width:auto;margin:0;'>
<span>Stationary Device (fixed location)</span>
</label>
</div>
</div>
<div class='espnow-card' id='encryption-card' style='display:none;'>
<div class='espnow-title'>Encryption Settings</div>
<div class='espnow-description'>Set passphrase for encrypted ESP-NOW communication. All devices must use the same passphrase.</div>
<div class='input-group'>
<input type='password' id='encryption-passphrase' placeholder='Enter encryption passphrase' maxlength='64'>
<button class='btn' id='btn-set-passphrase'>Set Passphrase</button>
<button class='btn' id='btn-clear-passphrase'>Clear</button>
</div>
<div class='espnow-data' id='encryption-status'>No encryption passphrase set</div>
</div>
<div class='espnow-card' id='device-management-card' style='display:none;'>
<div class='espnow-title'>Device Management</div>
<div class='espnow-description'>Pair and manage ESP-NOW devices for communication.</div>
<div class='input-group'>
<input type='text' id='pair-mac' class='mac-input' placeholder='XX:XX:XX:XX:XX:XX' maxlength='17'>
<input type='text' id='pair-name' placeholder='Device Name'>
</div>
<div class='espnow-controls'>
<button class='btn' id='btn-pair-device'>Pair (Unencrypted)</button>
<button class='btn' id='btn-pair-secure'>Pair (Encrypted)</button>
</div>
<div class='device-list' id='device-list'>
<div style='color: #666; text-align: center;'>No devices paired yet</div>
</div>
</div>
<div class='espnow-card' id='mesh-status-card' style='display:none;'>
<div class='espnow-title'>Mesh Network Status & Topology</div>
<div class='espnow-description'>Real-time mesh monitoring with direct peer connections and full network topology visualization.</div>
<div class='espnow-controls'>
<button class='btn' id='btn-refresh-mesh'>Refresh Status</button>
<button class='btn' id='btn-auto-topology' style='margin-left:8px;'>Auto-Discover: OFF</button>
</div>
<div style='margin-top:15px;'>
<div style='display:flex;gap:10px;margin-bottom:10px;'>
<button class='btn' id='btn-view-direct' style='flex:1;background:var(--crumb-bg);'>Direct Peers</button>
<button class='btn' id='btn-view-topology' style='flex:1;'>Full Topology</button>
<button class='btn' id='btn-view-graph' style='flex:1;'>Network Graph</button>
</div>
<div id='mesh-view-direct' style='display:block;'>
<div class='mesh-peers' id='mesh-peers-list'>
<div style='color:var(--panel-fg);text-align:center;'>No mesh peers detected yet</div>
</div>
</div>
<div id='mesh-view-topology' style='display:none;'>
<div class='mesh-peers' id='mesh-topology-view'>
<div style='color:var(--panel-fg);text-align:center;'>Click "Refresh Status" to load topology</div>
</div>
</div>
<div id='mesh-view-graph' style='display:none;'>
<div class='mesh-peers' id='mesh-graph-view'>
<div style='color:var(--panel-fg);text-align:center;'>Network graph visualization</div>
</div>
</div>
</div>
</div>
<div class='espnow-card' id='mesh-role-card' style='display:none;'>
<div class='espnow-title'>Mesh Role Configuration</div>
<div class='espnow-description'>Configure device role (Master/Worker/Backup) and topology discovery for structured mesh networking.</div>
<div class='espnow-data' id='mesh-role-status'>Loading role configuration...</div>
<div class='espnow-controls' style='margin-top:15px;'>
<button class='btn' id='btn-role-worker'>Set Worker</button>
<button class='btn' id='btn-role-master'>Set Master</button>
<button class='btn' id='btn-role-backup'>Set Backup</button>
<button class='btn' id='btn-mesh-topo'>Discover Topology</button>
</div>
<div class='input-group' style='margin-top:15px;'>
<input type='text' id='master-mac' class='mac-input' placeholder='Master MAC (XX:XX:XX:XX:XX:XX)' maxlength='17'>
<button class='btn' id='btn-set-master-mac'>Set Master MAC</button>
</div>
<div style='display:flex;align-items:center;gap:8px;margin-top:6px;margin-bottom:2px;'>
<input type='checkbox' id='backup-master-enabled' style='width:16px;height:16px;cursor:pointer;'>
<label for='backup-master-enabled' style='color:var(--panel-fg);font-size:0.92em;cursor:pointer;'>Enable Backup Master</label>
</div>
<div id='backup-mac-group' class='input-group' style='display:none;margin-top:4px;'>
<input type='text' id='backup-mac' class='mac-input' placeholder='Backup MAC (XX:XX:XX:XX:XX:XX)' maxlength='17'>
<button class='btn' id='btn-set-backup-mac'>Set Backup MAC</button>
</div>
<div class='espnow-data' id='mesh-topology-data' style='margin-top:15px;display:none;'>
<div style='font-weight:bold;margin-bottom:8px;'>Topology Discovery Results:</div>
<div id='topology-results'>No topology data yet</div>
</div>
</div>
<div class='espnow-card' id='device-panel-card' style='display:none;'>
<div class='espnow-title' id='device-panel-title'>Device Panel</div>
<div class='espnow-description' id='device-panel-subtitle'>Select Message, Remote, or File for a device to use this panel.</div>
<div class='panel' id='device-panel-content'></div>
</div>
</div>
<div class='setup-modal' id='setup-modal'>
<div class='setup-modal-content'>
<div class='setup-modal-title'>ESP-NOW First-Time Setup</div>
<div class='setup-modal-description'>
Before using ESP-NOW, you need to set a unique name for this device. This name will identify your device in topology displays and mesh networks.
</div>
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
<button class='btn btn-secondary' id='btn-setup-cancel'>Cancel</button>
<button class='btn btn-primary' id='btn-setup-save'>Set Name & Initialize</button>
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
          + '<button class="btn" onclick="loadRemoteAutomations(\'' + mac + '\')">' + 'Load Automations</button>'
          + '<div id="automations-list-' + mac + '" style="margin-top:12px;min-height:40px"></div>';
      } else {
        return '<div style="text-align:center;color:var(--muted);padding:20px;font-size:0.9em">'
          + 'Automations not compiled in this build.<br>'
          + '<span style="font-size:0.85em">Enable ENABLE_AUTOMATION in System_BuildConfig.h and recompile.</span>'
          + '</div>';
      }
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
          /* Hide init button, show disable button and other controls */
          document.getElementById('btn-espnow-init').style.display = 'none';
          document.getElementById('btn-espnow-disable').style.display = '';
          /* Show core features */
          document.getElementById('smarthome-card').style.display = 'block';
          document.getElementById('encryption-card').style.display = 'block';
          document.getElementById('device-management-card').style.display = 'block';
          document.getElementById('btn-espnow-toggle-mode').style.display = '';
          /* Load device list */
          try { if (typeof listDevices === 'function') { listDevices(); } } catch(e) { console.warn('[ESP-NOW] listDevices not defined yet'); }
          /* Check encryption status now that ESP-NOW is initialized */
          try { if (typeof window.checkEncryptionStatus === 'function') { window.checkEncryptionStatus(); } } catch(e) { console.warn('[ESP-NOW] checkEncryptionStatus call error:', e); }
          /* Load smart home metadata */
          try { if (typeof window.loadSmartHomeMetadata === 'function') { window.loadSmartHomeMetadata(); } } catch(e) { console.warn('[ESP-NOW] loadSmartHomeMetadata call error:', e); }
        } else {
          indicator.className = 'status-indicator status-disabled';
          /* Show init button, hide disable button */
          document.getElementById('btn-espnow-init').style.display = '';
          document.getElementById('btn-espnow-disable').style.display = 'none';
          /* Hide features until initialized */
          document.getElementById('smarthome-card').style.display = 'none';
          document.getElementById('encryption-card').style.display = 'none';
          document.getElementById('device-management-card').style.display = 'none';
          document.getElementById('btn-espnow-toggle-mode').style.display = 'none';
          /* Also hide mesh status card until initialized */
          var meshCard = document.getElementById('mesh-status-card');
          if (meshCard) { meshCard.style.display = 'none'; }
        }
      })
      .then(() => {
        return fetch('/api/cli', { method: 'POST', headers:{ 'Content-Type':'application/x-www-form-urlencoded' }, body: 'cmd=' + encodeURIComponent('espnow mode') });
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
          
          // Only show mesh card if ESP-NOW is initialized (check status indicator)
          var indicator = document.getElementById('espnow-status-indicator');
          var isInitialized = indicator && indicator.className.indexOf('status-enabled') >= 0;
          var meshCard = document.getElementById('mesh-status-card');
          if (meshCard) { 
            // Show mesh card only if mesh mode AND initialized
            meshCard.style.display = (isMesh && isInitialized) ? 'block' : 'none'; 
          }
          
          // Show mesh role card if mesh mode AND initialized
          var meshRoleCard = document.getElementById('mesh-role-card');
          if (meshRoleCard) {
            meshRoleCard.style.display = (isMesh && isInitialized) ? 'block' : 'none';
            if (isMesh && isInitialized && typeof window.refreshMeshRole === 'function') {
              window.refreshMeshRole();
            }
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
    console.log('[ESP-NOW] Chunk 2: Status functions ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 2 error:', e); }
})();
</script>
<script>
(function() {
  try {
    console.log('[ESP-NOW] Chunk 3B: listDevices start');
    window.listDevices = function() {
      // First get bond status to identify bonded device
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('bond status')
      })
      .then(r => r.text())
      .then(bondStatus => {
        // Extract bonded peer MAC if present
        window.__bondedPeerMac = null;
        const bondMatch = bondStatus.match(/Peer MAC:\s*([A-Fa-f0-9:]{17})/);
        if (bondMatch) {
          window.__bondedPeerMac = bondMatch[1].toUpperCase();
        }
        
        // Now fetch device list
        return fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('espnow list')
        });
      })
      .then(response => response.text())
      .then(output => {
        const deviceList = document.getElementById('device-list');
        try { console.log('[ESP-NOW][DEV] listDevices: output length', output ? output.length : -1); } catch(e){}
        window.espnowDevices = [];
        let parsed = null;
        try { parsed = JSON.parse(output); } catch(e) { parsed = null; }
        const devices = (parsed && Array.isArray(parsed.devices)) ? parsed.devices : [];
        if (devices.length === 0) {
          deviceList.innerHTML = '<div style="color: #666; text-align: center;">No devices paired yet</div>';
        } else {
          let html = '';
          for (const dev of devices) {
            const mac = (dev.mac || '').toUpperCase();
            const deviceName = dev.name || '';
            const isEncrypted = !!dev.encrypted;
            const encryptionClass = isEncrypted ? 'device-encrypted' : 'device-unencrypted';
            const encryptionIndicator = isEncrypted ? 'encryption-enabled' : 'encryption-disabled';
            const encryptionText = isEncrypted ? 'Encrypted' : 'Unencrypted';
            if (!mac) continue;
            if (deviceName) window.__espnowDeviceNameToMac[deviceName] = mac;
            const isBonded = window.__bondedPeerMac && mac === window.__bondedPeerMac;
            const bondIndicator = isBonded ? '<span style="color:var(--warning);margin-right:4px;font-weight:bold" title="Bonded Device">[BOND]</span>' : '';
            window.espnowDevices.push({ mac: mac, name: deviceName, encrypted: isEncrypted, bonded: isBonded });
            try { console.log('[ESP-NOW][DEV] appended device', mac); } catch(e){}
            html += '<div class="device-item">';
            html += '<div>';
            if (deviceName) {
              html += '<div class="device-mac">' + bondIndicator + '<strong>' + deviceName + '</strong><span class="encryption-indicator ' + encryptionIndicator + '" title="' + encryptionText + '"></span></div>';
              html += '<div class="device-channel ' + encryptionClass + '">' + mac + ' • ' + encryptionText + (isBonded ? ' • <strong>Bonded</strong>' : '') + '</div>';
            } else {
              html += '<div class="device-mac">' + bondIndicator + mac + '<span class="encryption-indicator ' + encryptionIndicator + '" title="' + encryptionText + '"></span></div>';
              html += '<div class="device-channel ' + encryptionClass + '">' + encryptionText + (isBonded ? ' • <strong>Bonded</strong>' : '') + '</div>';
            }
            html += '</div>';
            html += '<div class="device-actions">';
            html += '<button class="btn btn-small" onclick="toggleDevicePanel(\'' + mac + '\',\'message\')">Interact</button>';
            html += '<button class="btn btn-small" onclick="unpairDevice(\'' + mac + '\')">Unpair</button>';
            html += '</div>';
            html += '</div>';
          }
          try { console.log('[ESP-NOW][DEV] html length', html.length); } catch(e){}
          const broadcastBtn = '<div style="display:flex;justify-content:flex-end;margin-bottom:12px;">'
            + '<button class="btn" onclick="openBroadcastPanel()">Broadcast</button>'
            + '</div>';
          deviceList.innerHTML = broadcastBtn + html;
        }
      })
      .catch(error => {
        document.getElementById('device-list').innerHTML = '<div style="color: #dc3545;">Error loading devices: ' + error + '</div>';
      });
    };
    /* Per-device panel rendering and actions */
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
    window.renderDevicePanel = function(mac, kind) {
      if (kind === 'message') {
        return (
          '<div style="display:grid;grid-template-columns:1fr 2fr;gap:12px;margin-bottom:12px">'
          + '<div style="display:flex;flex-direction:column;gap:6px;max-height:250px;overflow-y:auto">'
          + '<button class="btn" id="btn-text-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'text\')" style="width:100%;text-align:left;padding:8px 12px">Text</button>'
          + '<button class="btn" id="btn-remote-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'remote\')" style="width:100%;text-align:left;padding:8px 12px">Remote</button>'
          + '<button class="btn" id="btn-file-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'file\')" style="width:100%;text-align:left;padding:8px 12px">File</button>'
          + '<button class="btn" id="btn-metadata-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'metadata\')" style="width:100%;text-align:left;padding:8px 12px">Metadata</button>'
          + '<button class="btn" id="btn-automations-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'automations\')" style="width:100%;text-align:left;padding:8px 12px">Automations</button>'
          + '</div>'
          + '<div>'
          + '<div class="message-log" id="log-' + mac + '" style="margin-bottom:12px;max-height:300px;overflow-y:auto"><div class="message-empty">No messages yet. Start a conversation!</div></div>'
          + '<div id="text-input-' + mac + '" style="display:block">'
          + '<div style="display:flex;gap:8px;align-items:flex-start;flex-wrap:wrap">'
          + '<textarea id="msg-' + mac + '" placeholder="Message to send" style="flex:1;min-width:220px;min-height:60px;resize:vertical;font-family:inherit;padding:8px;border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg)"></textarea>'
          + '<button class="btn" onclick="doSendMessage(\'' + mac + '\')" style="align-self:flex-start">Send</button>'
          + '</div>'
          + '</div>'
          + '<div id="remote-input-' + mac + '" style="display:none">'
          + '<div class="input-group" style="margin-bottom:8px">'
          + '<input type="text" id="ru-' + mac + '" placeholder="Username" style="flex:1">'
          + '<input type="password" id="rp-' + mac + '" placeholder="Password" style="flex:1">'
          + '</div>'
          + '<div style="display:flex;gap:8px;align-items:flex-start">'
          + '<input type="text" id="rc-' + mac + '" placeholder="Command (e.g., sensors, memory)" style="flex:1;padding:8px;border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg)">'
          + '<button class="btn" onclick="doRemoteExec(\'' + mac + '\')" style="align-self:flex-start">Execute</button>'
          + '</div>'
          + '</div>'
          + '<div id="file-input-' + mac + '" style="display:none">'
          + '<div style="display:flex;gap:8px;margin-bottom:12px">'
          + '<button class="btn" id="btn-file-send-' + mac + '" onclick="toggleFileMode(\'' + mac + '\', \'send\')" style="flex:1;background:var(--link);color:white">Send File</button>'
          + '<button class="btn" id="btn-file-receive-' + mac + '" onclick="toggleFileMode(\'' + mac + '\', \'receive\')" style="flex:1">Receive File</button>'
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
          + '<button class="btn" onclick="doSendFile(\'' + mac + '\')">Send File</button>'
          + '<div id="fstat-' + mac + '" style="margin-top:8px;padding:8px;border-radius:4px;font-size:0.9em;color:var(--panel-fg)">Select a file from the explorer or enter a file path manually</div>'
          + '</div>'
          + '<div id="file-receive-panel-' + mac + '" style="display:none">'
          + '<div class="input-group" style="margin-bottom:12px">'
          + '<input type="text" id="remote-user-' + mac + '" placeholder="Username" style="flex:1">'
          + '<input type="password" id="remote-pass-' + mac + '" placeholder="Password" style="flex:1">'
          + '<button class="btn" onclick="browseRemoteFiles(\'' + mac + '\', \'/\')">Browse Root</button>'
          + '</div>'
          + '<div style="margin-bottom:12px">'
          + '<label style="display:block;margin-bottom:6px;font-weight:500;color:var(--panel-fg)">Remote Files:</label>'
          + '<div id="remote-fexplorer-' + mac + '" style="background:var(--crumb-bg);border:1px solid var(--border);border-radius:8px;padding:12px;min-height:200px;max-height:300px;overflow-y:auto">'
          + '<div style="text-align:center;color:var(--muted);padding:20px">Click "Browse Root" to load remote files</div>'
          + '</div>'
          + '</div>'
          + '<div style="margin-bottom:10px">'
          + '<label style="display:block;margin-bottom:5px;font-weight:500;color:var(--panel-fg)">Remote File Path:</label>'
          + '<input type="text" id="remote-fp-' + mac + '" placeholder="/path/to/remote/file.ext" style="width:100%;padding:8px;border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg)">'
          + '</div>'
          + '<button class="btn" onclick="fetchRemoteFile(\'' + mac + '\')">Fetch File</button>'
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
    window.doSendMessage = function(mac) {
      const val = (document.getElementById('msg-' + mac) || {}).value || '';
      if (!val) { alert('Enter a message'); return; }
      
      // Clear input immediately
      const input = document.getElementById('msg-' + mac);
      if (input) input.value = '';
      
      // Show message as "sending" immediately
      const bubble = appendLogLine('log-' + mac, 'SENT', val, 'sending');
      
      // Fetch current mode dynamically to ensure we use the correct command
      console.log('[ESP-NOW] Fetching current mode before sending...');
      fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent('espnow mode') })
        .then(r => r.text())
        .then(modeOut => {
          const isMesh = (modeOut || '').toLowerCase().indexOf('mesh') >= 0;
          console.log('[ESP-NOW] Current mode:', isMesh ? 'MESH' : 'DIRECT');
          var cmd = 'espnow send ' + mac + ' ' + val;
          console.log('[ESP-NOW] Command:', cmd);
          return fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent(cmd) });
        })
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
      
      fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent('espnow broadcast ' + msg) })
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
        statDiv.textContent = '📤 Sending file: ' + filename + '...';
      }
      
      // Also show in message log
      appendLogLine('log-' + mac, 'SENT', '📤 Sending file: ' + filename, 'sending');
      
      fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent('espnow sendfile ' + mac + ' ' + path) })
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
    window.toggleMessageType = function(mac, type) {
      const textInput = document.getElementById('text-input-' + mac);
      const remoteInput = document.getElementById('remote-input-' + mac);
      const fileInput = document.getElementById('file-input-' + mac);
      const metadataDiv = document.getElementById('metadata-' + mac);
      const automationsInput = document.getElementById('automations-input-' + mac);
      const btnText = document.getElementById('btn-text-' + mac);
      const btnRemote = document.getElementById('btn-remote-' + mac);
      const btnFile = document.getElementById('btn-file-' + mac);
      const btnMetadata = document.getElementById('btn-metadata-' + mac);
      const btnAutomations = document.getElementById('btn-automations-' + mac);
      
      if (!textInput || !remoteInput || !fileInput) return;
      
      // Reset all button styles
      if (btnText) {
        btnText.style.background = '';
        btnText.style.color = '';
        btnText.style.border = '';
      }
      if (btnRemote) {
        btnRemote.style.background = '';
        btnRemote.style.color = '';
        btnRemote.style.border = '';
      }
      if (btnFile) {
        btnFile.style.background = '';
        btnFile.style.color = '';
        btnFile.style.border = '';
      }
      if (btnMetadata) {
        btnMetadata.style.background = '';
        btnMetadata.style.color = '';
        btnMetadata.style.border = '';
      }
      if (btnAutomations) {
        btnAutomations.style.background = '';
        btnAutomations.style.color = '';
        btnAutomations.style.border = '';
      }
      
      // Hide all inputs and metadata
      textInput.style.display = 'none';
      remoteInput.style.display = 'none';
      fileInput.style.display = 'none';
      if (metadataDiv) metadataDiv.style.display = 'none';
      if (automationsInput) automationsInput.style.display = 'none';
      
      // Get message log to show/hide based on mode
      var messageLog = document.getElementById('log-' + mac);
      
      // Show selected input and highlight button
      if (type === 'text') {
        textInput.style.display = 'block';
        if (messageLog) messageLog.style.display = 'block';
        if (btnText) {
          btnText.style.background = '#007bff';
          btnText.style.color = 'white';
          btnText.style.border = '2px solid #007bff';
        }
      } else if (type === 'remote') {
        remoteInput.style.display = 'block';
        if (messageLog) messageLog.style.display = 'block';
        if (btnRemote) {
          btnRemote.style.background = '#007bff';
          btnRemote.style.color = 'white';
          btnRemote.style.border = '2px solid #007bff';
        }
      } else if (type === 'file') {
        fileInput.style.display = 'block';
        if (messageLog) messageLog.style.display = 'none';
        if (btnFile) {
          btnFile.style.background = '#007bff';
          btnFile.style.color = 'white';
          btnFile.style.border = '2px solid #007bff';
        }
        // Initialize file browser when file mode is selected
        if (typeof window.initializeFileBrowser === 'function') {
          setTimeout(function() { window.initializeFileBrowser(mac); }, 100);
        }
      } else if (type === 'metadata') {
        if (metadataDiv) metadataDiv.style.display = 'block';
        if (messageLog) messageLog.style.display = 'none';
        if (btnMetadata) {
          btnMetadata.style.background = '#007bff';
          btnMetadata.style.color = 'white';
          btnMetadata.style.border = '2px solid #007bff';
        }
        // Load cached metadata if available; don't auto-request from device
        window.loadDeviceMetadata(mac);
      } else if (type === 'automations') {
        if (automationsInput) automationsInput.style.display = 'block';
        if (messageLog) messageLog.style.display = 'none';
        if (btnAutomations) {
          btnAutomations.style.background = '#007bff';
          btnAutomations.style.color = 'white';
          btnAutomations.style.border = '2px solid #007bff';
        }
        // Auto-load already-received automations file if it exists
        window.tryLoadExistingAutomations(mac);
        // Pre-fill credentials from remote tab if available
        var ruEl = document.getElementById('ru-' + mac);
        var rpEl = document.getElementById('rp-' + mac);
        var auEl = document.getElementById('au-' + mac);
        var apEl = document.getElementById('ap-' + mac);
        if (ruEl && auEl && !auEl.value && ruEl.value) auEl.value = ruEl.value;
        if (rpEl && apEl && !apEl.value && rpEl.value) apEl.value = rpEl.value;
      }
    };
    window.loadRemoteAutomations = function(mac) {
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
      var listDiv = document.getElementById('automations-list-' + mac);
      if (!listDiv) return;
      listDiv.innerHTML = '<div style="color:var(--muted);padding:12px;text-align:center">Requesting automations via ESP-NOW...</div>';
      var macHex = mac.replace(/:/g, '').toUpperCase();
      var filePath = '/espnow/received/' + macHex + '/automations.json';
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'cmd=' + encodeURIComponent('espnow fetch ' + mac + ' ' + u + ' ' + p + ' /system/automations.json')
      })
      .then(function(r) { return r.text(); })
      .then(function(resp) {
        var lower = (resp || '').toLowerCase();
        if (lower.indexOf('error') >= 0 || lower.indexOf('not initialized') >= 0) {
          listDiv.innerHTML = '<div style="color:var(--danger);padding:12px">' + esc(resp) + '</div>';
          return;
        }
        var attempts = 0;
        listDiv.innerHTML = '<div style="color:var(--muted);padding:12px;text-align:center">Transfer in progress (1/30)...</div>';
        function poll() {
          attempts++;
          fetch('/api/files/read?name=' + encodeURIComponent(filePath))
          .then(function(r) {
            if (r.status === 404) {
              if (attempts < 30) {
                listDiv.innerHTML = '<div style="color:var(--muted);padding:12px;text-align:center">Transfer in progress (' + attempts + '/30)...</div>';
                setTimeout(poll, 1000);
              } else {
                listDiv.innerHTML = '<div style="color:var(--danger);padding:12px">Timed out. Is encryption enabled and both devices securely paired?</div>';
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
          });
        }
        setTimeout(poll, 1500);
      })
      .catch(function(e) {
        listDiv.innerHTML = '<div style="color:var(--danger);padding:12px">Error: ' + esc(e.message) + '</div>';
      });
    };
    window.doRemoteExec = function(mac) {
      const u = (document.getElementById('ru-' + mac) || {}).value || '';
      const p = (document.getElementById('rp-' + mac) || {}).value || '';
      const c = (document.getElementById('rc-' + mac) || {}).value || '';
      if (!u || !p || !c) { alert('Enter username, password, and command'); return; }
      
      // Show command being executed in message log
      const sendingBubble = appendLogLine('log-' + mac, 'SENT', 'Remote: ' + c, 'sending');
      
      // Set up temporary SSE listener for streamed output
      const targetMac = String(mac || '').toUpperCase();
      const targetMacKey = targetMac.replace(/:/g, '').toUpperCase();
      var targetDeviceName = '';
      try {
        if (window.espnowDevices && window.espnowDevices.length) {
          for (var i = 0; i < window.espnowDevices.length; i++) {
            var d = window.espnowDevices[i];
            if (d && d.mac && String(d.mac).toUpperCase() === targetMac) {
              targetDeviceName = d.name || '';
              break;
            }
          }
        }
      } catch(_) {}
      let hasOutput = false;
      const streamListener = function(event) {
        try {
          const data = JSON.parse(event.data);
          const payload = (data && (data.msg || data.message || data.broadcast)) ? String(data.msg || data.message || data.broadcast) : '';
          if (!payload) return;
          
          // Check for [STREAM:device] prefix (real-time output)
          const streamPrefix = '[STREAM:';
          if (payload.startsWith(streamPrefix)) {
            const endBracket = payload.indexOf(']');
            if (endBracket <= streamPrefix.length) return;
            const streamDevice = payload.substring(streamPrefix.length, endBracket).trim();
            const streamContent = payload.substring(endBracket + 1).trim();
            var match = false;
            if (streamDevice.replace(/:/g, '').toUpperCase() === targetMacKey) match = true;
            else if (targetDeviceName && streamDevice === targetDeviceName) match = true;
            else if (window.__espnowDeviceNameToMac && window.__espnowDeviceNameToMac[streamDevice]) {
              if (String(window.__espnowDeviceNameToMac[streamDevice]).toUpperCase() === targetMac) match = true;
            }
            if (match && streamContent) {
              appendLogLine('log-' + mac, 'RECEIVED', streamContent, null);
              hasOutput = true;
            }
            return;
          }
          
          // Check for [ESP-NOW] Command result/FAILED (final CMD_RESP)
          var cmdResultMatch = payload.match(/\[ESP-NOW\] Command (?:result|FAILED) from ([^:]+): (.*)/);
          if (cmdResultMatch) {
            var respDevice = cmdResultMatch[1].trim();
            var respContent = cmdResultMatch[2].trim();
            var rMatch = false;
            if (respDevice.replace(/:/g, '').toUpperCase() === targetMacKey) rMatch = true;
            else if (targetDeviceName && respDevice === targetDeviceName) rMatch = true;
            if (rMatch && respContent) {
              var rType = payload.indexOf('FAILED') >= 0 ? 'ERROR' : 'RECEIVED';
              appendLogLine('log-' + mac, rType, respContent, null);
              hasOutput = true;
            }
          }
        } catch(e) { /* ignore parse errors */ }
      };
      
      // Add SSE listener - always ensure connected
      try {
        if (!window.__es || window.__es.readyState === 2) {
          window.__es = new EventSource('/api/events', { withCredentials: true });
        }
        window.__es.addEventListener('notice', streamListener);
      } catch(_) {}
      
      const cmd = 'espnow remote ' + mac + ' ' + u + ' ' + p + ' ' + c;
      fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent(cmd) })
        .then(r=>r.text())
        .then(t=>{
          // Remove SSE listener after delay to catch remote command results.
          // Remote commands need time for: ESP-NOW delivery + execution + response.
          setTimeout(function() {
            if (window.__es) {
              window.__es.removeEventListener('notice', streamListener);
            }
          }, 15000);
          
          // Update sending bubble to show completion
          if (sendingBubble) {
            const statusDiv = sendingBubble.querySelector('.message-status');
            if (statusDiv) {
              statusDiv.innerHTML = '<span class="status-icon">OK</span>Sent';
            }
          }
          
          // Show final result if no streamed output was received
          if (!hasOutput && t && !t.includes('Remote command sent')) {
            appendLogLine('log-' + mac, 'RECEIVED', 'Result: ' + t, null);
          }
          
          // Clear command input
          const cmdInput = document.getElementById('rc-' + mac);
          if (cmdInput) cmdInput.value = '';
        })
        .catch(e=> {
          // Remove SSE listener on error
          if (window.__es) {
            window.__es.removeEventListener('notice', streamListener);
          }
          
          // Update sending bubble to show error
          if (sendingBubble) {
            sendingBubble.className = 'message-bubble message-error';
            const textDiv = sendingBubble.querySelector('.message-text');
            if (textDiv) textDiv.textContent = 'Remote: ' + c + ' (FAILED)';
            const statusDiv = sendingBubble.querySelector('.message-status');
            if (statusDiv) {
              statusDiv.innerHTML = '<span class="status-icon">ERR</span>' + e.message;
            }
          }
        });
    };
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
    // Track current remote path per device
    window.remoteCurrentPath = window.remoteCurrentPath || {};
    
    // Helper to render interactive remote file browser UI
    window.renderRemoteFileExplorer = function(mac, path, entries) {
      var container = document.getElementById('remote-fexplorer-' + mac);
      if (!container) return;
      
      window.remoteCurrentPath[mac] = path;
      
      // Build breadcrumb navigation
      var parts = path.split('/').filter(function(p) { return p.length > 0; });
      var crumbHtml = '<div style="display:flex;align-items:center;gap:4px;margin-bottom:10px;flex-wrap:wrap">';
      crumbHtml += '<span onclick="browseRemoteFiles(\'' + mac + '\', \'/\')" style="cursor:pointer;color:var(--link);font-weight:500">Root</span>';
      var crumbPath = '';
      for (var i = 0; i < parts.length; i++) {
        crumbPath += '/' + parts[i];
        var isLast = (i === parts.length - 1);
        crumbHtml += '<span style="color:var(--muted)">/</span>';
        if (isLast) {
          crumbHtml += '<span style="font-weight:500;color:var(--panel-fg)">' + parts[i] + '</span>';
        } else {
          crumbHtml += '<span onclick="browseRemoteFiles(\'' + mac + '\', \'' + crumbPath + '\')" style="cursor:pointer;color:var(--link)">' + parts[i] + '</span>';
        }
      }
      crumbHtml += '</div>';
      
      // Build file/folder list
      var listHtml = '<div style="display:flex;flex-direction:column;gap:2px">';
      
      // Add parent directory link if not at root
      if (path !== '/') {
        var parentPath = path.replace(/\/[^\/]+\/?$/, '') || '/';
        listHtml += '<div onclick="browseRemoteFiles(\'' + mac + '\', \'' + parentPath + '\')" style="display:flex;align-items:center;gap:8px;padding:6px 8px;border-radius:4px;cursor:pointer;background:var(--panel-bg)" onmouseover="this.style.background=\'var(--hover-bg)\'" onmouseout="this.style.background=\'var(--panel-bg)\'">';
        listHtml += '<span style="color:var(--link)">..</span>';
        listHtml += '<span style="color:var(--muted);font-size:0.85em;margin-left:auto">(parent)</span>';
        listHtml += '</div>';
      }
      
      if (entries.length === 0) {
        listHtml += '<div style="padding:12px;color:var(--muted);text-align:center">(empty directory)</div>';
      }
      
      for (var j = 0; j < entries.length; j++) {
        var entry = entries[j];
        var fullPath = (path === '/' ? '/' : path + '/') + entry.name;
        
        if (entry.isDir) {
          listHtml += '<div onclick="browseRemoteFiles(\'' + mac + '\', \'' + fullPath + '\')" style="display:flex;align-items:center;gap:8px;padding:6px 8px;border-radius:4px;cursor:pointer;background:var(--panel-bg)" onmouseover="this.style.background=\'var(--hover-bg)\'" onmouseout="this.style.background=\'var(--panel-bg)\'">';
          listHtml += '<span style="color:var(--link);font-weight:500">' + entry.name + '</span>';
          listHtml += '<span style="color:var(--muted);font-size:0.85em;margin-left:auto">' + entry.info + '</span>';
          listHtml += '</div>';
        } else {
          listHtml += '<div onclick="selectRemoteFile(\'' + mac + '\', \'' + fullPath + '\')" style="display:flex;align-items:center;gap:8px;padding:6px 8px;border-radius:4px;cursor:pointer;background:var(--panel-bg)" onmouseover="this.style.background=\'var(--hover-bg)\'" onmouseout="this.style.background=\'var(--panel-bg)\'">';
          listHtml += '<span style="color:var(--panel-fg)">' + entry.name + '</span>';
          listHtml += '<span style="color:var(--muted);font-size:0.85em;margin-left:auto">' + entry.info + '</span>';
          listHtml += '</div>';
        }
      }
      listHtml += '</div>';
      
      container.innerHTML = crumbHtml + listHtml;
    };
    
    // Select a remote file (fills in the path input)
    window.selectRemoteFile = function(mac, filePath) {
      var input = document.getElementById('remote-fp-' + mac);
      if (input) input.value = filePath;
      var statusDiv = document.getElementById('remote-fstat-' + mac);
      if (statusDiv) statusDiv.textContent = 'Selected: ' + filePath;
    };
    
    // Parse remote file listing response into entries
    window.parseRemoteFileListing = function(lines) {
      var entries = [];
      for (var i = 0; i < lines.length; i++) {
        var line = lines[i].trim();
        // Format: "  dirname (N items)" for directories
        // Format: "  filename (N bytes)" for files
        var dirMatch = line.match(/^\s*(\S+)\s+\((\d+)\s+items?\)$/);
        var fileMatch = line.match(/^\s*(\S+)\s+\((\d+)\s+bytes?\)$/);
        
        if (dirMatch) {
          entries.push({ name: dirMatch[1], isDir: true, info: dirMatch[2] + ' items' });
        } else if (fileMatch) {
          var bytes = parseInt(fileMatch[2], 10);
          var sizeStr = bytes < 1024 ? bytes + ' B' : (bytes / 1024).toFixed(1) + ' KB';
          entries.push({ name: fileMatch[1], isDir: false, info: sizeStr });
        }
      }
      // Sort: directories first, then files, alphabetically
      entries.sort(function(a, b) {
        if (a.isDir !== b.isDir) return a.isDir ? -1 : 1;
        return a.name.localeCompare(b.name);
      });
      return entries;
    };
    
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
      container.innerHTML = '<div style="text-align:center;color:var(--muted);padding:20px">Loading remote files...</div>';
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
      var cmd = 'espnow remote ' + mac + ' ' + u + ' ' + p + ' files ' + browsePath;
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) { return r.text(); })
      .then(function(text) {
        if (!text.includes('Remote command sent')) {
          container.innerHTML = '<pre style="margin:0;white-space:pre-wrap;font-size:0.85em">' + text + '</pre>';
          if (statusDiv) statusDiv.textContent = 'Result received';
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
            if (container.innerHTML.indexOf('Loading') >= 0) {
              container.innerHTML = '<div style="text-align:center;color:var(--muted);padding:20px">Timed out waiting for response</div>';
              if (statusDiv) statusDiv.textContent = 'Timed out';
            }
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
      var cmd = 'espnow fetch ' + mac + ' ' + u + ' ' + p + ' ' + remotePath;
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
    window.unpairDevice = function(mac) {
      if (confirm('Unpair device ' + mac + '?')) {
        fetch('/api/cli', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'cmd=' + encodeURIComponent('espnow unpair ' + mac)
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
          
          html += '<div style="margin-top:8px;padding:8px;background:var(--panel-bg);border-radius:4px;border-left:3px solid var(--link);font-size:0.85em;color:var(--panel-fg)">';
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
        body: 'cmd=' + encodeURIComponent('espnow requestmeta ' + mac) 
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
        var cmd = 'espnow remote ' + mac + ' ' + u + ' ' + p + ' ' + cmds[i];
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
      fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent('espnow mode') })
        .then(r => r.text())
        .then(modeOut => {
          const isMesh = (modeOut || '').toLowerCase().indexOf('mesh') >= 0;
          console.log('[ESP-NOW] sendMessage: Current mode:', isMesh ? 'MESH' : 'DIRECT');
          var cmd = 'espnow send ' + mac + ' ' + message;
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
    window.startMeshStatusPolling = function() {
      console.log('[ESP-NOW] Starting mesh status polling...');
      if (window.meshStatusPollInterval) {
        clearInterval(window.meshStatusPollInterval);
      }
      window.meshStatusPollInterval = setInterval(function() {
        if (window.espnowIsMesh && typeof window.refreshMeshStatus === 'function') {
          window.refreshMeshStatus();
        }
      }, 3000);  // Refresh every 3 seconds
    };
    window.stopMeshStatusPolling = function() {
      console.log('[ESP-NOW] Stopping mesh status polling...');
      if (window.meshStatusPollInterval) {
        clearInterval(window.meshStatusPollInterval);
        window.meshStatusPollInterval = null;
      }
    };
    window.refreshMeshStatus = function() {
      console.log('[ESP-NOW] Refreshing mesh status...');
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnow meshstatus')
      })
      .then(response => response.text())
      .then(output => {
        console.log('[ESP-NOW] Mesh status response:', output);
        try {
          var data = JSON.parse(output);
          if (data.error) {
            document.getElementById('mesh-peers-list').innerHTML = '<div style="color:var(--danger);text-align:center;">' + data.error + '</div>';
            return;
          }
          var html = '';
          
          // Paired Devices Section
          html += '<div style="margin-bottom:20px;">';
          html += '<h4 style="color:var(--panel-fg);margin:0 0 10px 0;padding:8px;background:var(--crumb-bg);border-radius:4px;">Paired Devices (' + (data.peers ? data.peers.length : 0) + ')</h4>';
          
          if (!data.peers || data.peers.length === 0) {
            html += '<div style="color:var(--panel-fg);text-align:center;padding:20px;">No paired mesh peers detected yet</div>';
          } else {
            for (var i = 0; i < data.peers.length; i++) {
              var peer = data.peers[i];
              
              // Determine status indicator
              var indicator = '';
              var statusColor = '';
              var statusText = '';
              if (!peer.alive) {
                indicator = 'Offline';
                statusColor = 'var(--danger)';
              } else {
                var secondsSince = peer.secondsSinceHeartbeat;
                if (secondsSince < 15) {
                  indicator = 'Online';
                  statusColor = 'var(--success)';
                } else {
                  indicator = 'Stale';
                  statusColor = 'var(--warning)';
                }
              }
              
              html += '<div style="padding:10px;margin:5px 0;background:var(--panel-bg);border-left:4px solid ' + statusColor + ';border-radius:4px;">';
              html += '<div style="display:flex;align-items:center;justify-content:space-between;">';
              html += '<div style="flex:1;">';
              html += '<span style="font-size:1.2em;color:' + statusColor + ';">' + indicator + '</span> ';
              html += '<strong style="color:var(--panel-fg);">' + peer.name + '</strong> ';
              html += '<span style="color:var(--panel-fg);font-size:0.9em;">(' + peer.mac + ')</span>';
              html += '<br><span style="color:var(--panel-fg);font-size:0.85em;margin-left:20px;">';
              html += 'Last HB: ' + peer.secondsSinceHeartbeat + 's ago | ';
              html += 'Count: ' + peer.heartbeatCount + ' | ';
              html += 'ACKs: ' + peer.ackCount;
              html += '</span>';
              html += '</div>';
              html += '<div style="text-align:right;"><span style="color:' + statusColor + ';font-weight:bold;font-size:0.9em;">' + statusText + '</span></div>';
              html += '</div></div>';
            }
          }
          html += '</div>';
          
          // Unpaired Devices Section
          if (data.unpaired && data.unpaired.length > 0) {
            html += '<div style="margin-top:20px;">';
            html += '<h4 style="color:var(--panel-fg);margin:0 0 10px 0;padding:8px;background:var(--panel-bg);border-radius:4px;">Unpaired Devices Detected (' + data.unpaired.length + ')</h4>';
            
            for (var i = 0; i < data.unpaired.length; i++) {
              var device = data.unpaired[i];
              var rssiColor = device.rssi > -60 ? 'var(--success)' : (device.rssi > -75 ? 'var(--warning)' : 'var(--danger)');
              
              html += '<div style="padding:10px;margin:5px 0;background:var(--panel-bg);border-left:4px solid var(--border);border-radius:4px;">';
              html += '<div style="display:flex;align-items:center;justify-content:space-between;">';
              html += '<div style="flex:1;">';
              html += '<span style="font-size:1.2em;color:var(--panel-fg);">Unknown</span> ';
              html += '<strong style="color:var(--panel-fg);">' + (device.name || 'Unknown') + '</strong> ';
              html += '<span style="color:var(--panel-fg);font-size:0.9em;">(' + device.mac + ')</span>';
              html += '<br><span style="color:var(--panel-fg);font-size:0.85em;margin-left:20px;">';
              html += 'Last seen: ' + device.secondsSinceLastSeen + 's ago | ';
              html += 'RSSI: <span style="color:' + rssiColor + ';">' + device.rssi + ' dBm</span> | ';
              html += 'HB Count: ' + device.heartbeatCount;
              html += '</span>';
              html += '</div>';
              html += '<div style="text-align:right;">';
              html += '<button onclick="pairUnpairedDevice(\'' + device.mac + '\', \'' + (device.name || 'Unknown') + '\')" ';
              html += 'class="btn" style="padding:5px 12px;font-size:0.9em;">';
              html += 'Pair</button>';
              html += '</div>';
              html += '</div></div>';
            }
            
            html += '</div>';
          }
          
          document.getElementById('mesh-peers-list').innerHTML = html;
        } catch(e) {
          console.error('[ESP-NOW] Error parsing mesh status:', e);
          document.getElementById('mesh-peers-list').innerHTML = '<div style="color:var(--danger);text-align:center;">Error parsing mesh status</div>';
        }
      })
      .catch(error => {
        console.error('[ESP-NOW] Mesh status fetch error:', error);
        var el = document.getElementById('mesh-peers-list');
        if (el) el.innerHTML = '<div style="color:var(--danger);text-align:center;">Error: ' + error + '</div>';
      });
      
      // Also fetch and update topology view
      if (document.getElementById('mesh-view-topology').style.display !== 'none') {
        window.refreshTopologyView();
      }
    };
    
    // Pair an unpaired device
    window.pairUnpairedDevice = function(mac, name) {
      console.log('[ESP-NOW] Pairing device:', mac, name);
      if (!confirm('Pair device "' + name + '" (' + mac + ')?')) {
        return;
      }
      
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnow pair ' + mac + ' ' + name)
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
      var views = ['direct', 'topology', 'graph'];
      var buttons = ['btn-view-direct', 'btn-view-topology', 'btn-view-graph'];
      
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
        body: 'cmd=' + encodeURIComponent('espnow toporesults')
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
          
          // Match device header: "Device: name (MAC)"
          var deviceMatch = line.match(/Device:\s*(.+?)\s*\(([a-f0-9:]+)\)/i);
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
            
            // Next line should have heartbeat info
            if (i + 1 < lines.length) {
              var nextLine = lines[i + 1].trim();
              var hbMatch = nextLine.match(/Heartbeats:\s*(\d+),\s*Last seen:\s*(\d+)s ago/i);
              if (hbMatch) {
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
              html += '<div style="border-left:3px solid var(--success);padding-left:12px;margin-left:8px;">';
              for (var j = 0; j < dev.peers.length; j++) {
                var peer = dev.peers[j];
                var signalColor = peer.lastSeen < 10 ? 'var(--success)' : (peer.lastSeen < 20 ? 'var(--warning)' : 'var(--danger)');
                var statusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:' + signalColor + ';margin-right:6px;"></span>';
                
                html += '<div style="padding:6px 0;border-bottom:1px solid var(--border);">';
                html += '<div style="font-weight:500;color:var(--panel-fg);">' + statusDot + peer.name + '</div>';
                html += '<div style="font-size:0.8em;color:var(--panel-fg);margin-top:2px;">';
                html += peer.mac + ' • ';
                html += '<span style="color:' + signalColor + ';">Last seen: ' + peer.lastSeen + 's ago</span> • ';
                html += 'Heartbeats: ' + peer.heartbeats;
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
          body: 'cmd=' + encodeURIComponent('espnow meshstatus')
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
            body: 'cmd=' + encodeURIComponent('espnow meshtopo')
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
    window.refreshMeshRole = function() {
      console.log('[ESP-NOW] Refreshing mesh role...');
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnow meshrole')
      })
      .then(response => response.text())
      .then(output => {
        console.log('[ESP-NOW] Mesh role response:', output);
        var statusDiv = document.getElementById('mesh-role-status');
        if (!statusDiv) return;
        
        // Parse role, master MAC, backup enabled flag, and backup MAC from output
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
        
        // Sync the backup-enabled checkbox
        var backupCheckbox = document.getElementById('backup-master-enabled');
        if (backupCheckbox) backupCheckbox.checked = backupEnabled;
        
        // Show/hide MAC input fields based on role
        var masterGroup = document.getElementById('master-mac')?.parentElement;
        var backupMacGroup = document.getElementById('backup-mac-group');
        
        if (role.toLowerCase() === 'master') {
          // Master: hide master MAC field, show backup MAC group only if backup is enabled
          if (masterGroup) masterGroup.style.display = 'none';
          if (backupMacGroup) backupMacGroup.style.display = backupEnabled ? 'flex' : 'none';
          if (backupEnabled && backupMAC !== 'Not set') {
            var backupInput = document.getElementById('backup-mac');
            if (backupInput) backupInput.value = backupMAC;
          }
        } else if (role.toLowerCase() === 'backup') {
          // Backup: show master MAC field, hide backup MAC group (this IS the backup)
          if (masterGroup) masterGroup.style.display = 'flex';
          if (backupMacGroup) backupMacGroup.style.display = 'none';
          if (masterMAC !== 'Not set') {
            var masterInput = document.getElementById('master-mac');
            if (masterInput) masterInput.value = masterMAC;
          }
        } else {
          // Worker: show master MAC field, show backup MAC group only if backup is enabled
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
      })
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
        body: 'cmd=' + encodeURIComponent('espnow meshrole ' + role)
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
        body: 'cmd=' + encodeURIComponent('espnow meshmaster ' + mac)
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
        body: 'cmd=' + encodeURIComponent('espnow meshbackup ' + mac)
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
        body: 'cmd=' + encodeURIComponent('espnow backupenable ' + (enabled ? 'on' : 'off'))
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
        body: 'cmd=' + encodeURIComponent('espnow meshtopo')
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
            body: 'cmd=' + encodeURIComponent('espnow toporesults')
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
          body: 'cmd=' + encodeURIComponent('espnow setname')
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
      document.getElementById('btn-espnow-disable').addEventListener('click', function() {
        if (!confirm('Disable ESP-NOW? This will stop all ESP-NOW communication. Memory will remain allocated until reboot.')) {
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
        fetch('/api/cli', { method:'POST', headers:{ 'Content-Type':'application/x-www-form-urlencoded' }, body: 'cmd=' + encodeURIComponent('espnow mode') })
          .then(r=>r.text())
          .then(curr => {
            var isMesh = (curr || '').toLowerCase().indexOf('mesh') >= 0;
            var next = isMesh ? 'direct' : 'mesh';
            // Update global flag immediately based on what we're switching TO
            window.espnowIsMesh = (next === 'mesh');
            return fetch('/api/cli', { method:'POST', headers:{ 'Content-Type':'application/x-www-form-urlencoded' }, body: 'cmd=' + encodeURIComponent('espnow mode ' + next) });
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
          body: 'cmd=' + encodeURIComponent('espnow pair ' + mac + ' ' + name)
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
          body: 'cmd=' + encodeURIComponent('espnow pairsecure ' + mac + ' ' + name)
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
      document.getElementById('btn-view-direct').addEventListener('click', function() {
        if (typeof window.switchMeshView === 'function') {
          window.switchMeshView('direct');
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
        fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd='+encodeURIComponent('espnow friendlyname "'+val+'"') })
          .then(r=>r.text()).then(t=>{ var el=document.getElementById('smarthome-status'); if(el)el.textContent=t; if(typeof window.loadSmartHomeMetadata==='function')window.loadSmartHomeMetadata(); });
      });
      _on('btn-set-room','click', function() {
        const val = document.getElementById('room-name').value;
        fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd='+encodeURIComponent('espnow room "'+val+'"') })
          .then(r=>r.text()).then(t=>{ var el=document.getElementById('smarthome-status'); if(el)el.textContent=t; if(typeof window.loadSmartHomeMetadata==='function')window.loadSmartHomeMetadata(); });
      });
      _on('btn-set-zone','click', function() {
        const val = document.getElementById('zone-name').value;
        fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd='+encodeURIComponent('espnow zone "'+val+'"') })
          .then(r=>r.text()).then(t=>{ var el=document.getElementById('smarthome-status'); if(el)el.textContent=t; if(typeof window.loadSmartHomeMetadata==='function')window.loadSmartHomeMetadata(); });
      });
      _on('btn-set-tags','click', function() {
        const val = document.getElementById('tags-input').value;
        fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd='+encodeURIComponent('espnow tags "'+val+'"') })
          .then(r=>r.text()).then(t=>{ var el=document.getElementById('smarthome-status'); if(el)el.textContent=t; if(typeof window.loadSmartHomeMetadata==='function')window.loadSmartHomeMetadata(); });
      });
      _on('stationary-checkbox','change', function() {
        const checked = document.getElementById('stationary-checkbox').checked;
        fetch('/api/cli', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd='+encodeURIComponent('espnow stationary '+(checked?'on':'off')) })
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
          body: 'cmd=' + encodeURIComponent('espnow setpassphrase "' + passphrase + '"')
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
          body: 'cmd=' + encodeURIComponent('espnow clearpassphrase')
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
          body: 'cmd=' + encodeURIComponent('espnow broadcast ' + message)
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
        document.getElementById('message-log').innerHTML = '<div style="color: #666; text-align: center;">Message log cleared</div>';
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
          body: 'cmd=' + encodeURIComponent('espnow sendfile ' + mac + ' ' + filepath)
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
        document.getElementById('remote-results-log').innerHTML = '<div style="color: #666; text-align: center;">Remote command results cleared</div>';
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
          body: 'cmd=' + encodeURIComponent('espnow setname ' + deviceName)
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
      
      const remoteCmd = 'espnow remote ' + device + ' ' + username + ' ' + password + ' ' + command;
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
    window.loadSmartHomeMetadata = function() {
      fetch('/api/cli', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent('espnow deviceinfo')
      })
      .then(response => response.text())
      .then(text => {
        const statusDiv = document.getElementById('smarthome-status');
        if (!statusDiv) return;
        statusDiv.textContent = text;
        
        // Parse and populate input fields
        const friendlyMatch = text.match(/Friendly Name:\s*(.+)/);
        const roomMatch = text.match(/Room:\s*(.+)/);
        const zoneMatch = text.match(/Zone:\s*(.+)/);
        const tagsMatch = text.match(/Tags:\s*(.+)/);
        const stationaryMatch = text.match(/Stationary:\s*(Yes|No)/i);
        
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
          stationaryCheckbox.checked = (stationaryMatch[1].toLowerCase() === 'yes');
        }
      })
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
    window.checkEncryptionStatus = function() {
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
        body: 'cmd=' + encodeURIComponent('espnow encstatus')
      })
      .then(response => response.text())
      .then(text => {
        const statusDiv = document.getElementById('encryption-status');
        if (!statusDiv) return;
        
        // Parse the response to check if passphrase is set
        if (text.includes('Passphrase Set: Yes')) {
          statusDiv.textContent = 'Encryption passphrase is set';
        } else if (text.includes('Passphrase Set: No') || text.includes('Encryption Enabled: No')) {
          statusDiv.textContent = 'No encryption passphrase set';
        } else {
          statusDiv.textContent = 'Encryption status unknown';
        }
      })
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
      refreshStatus(); /* This will show/hide cards, load device list, and check encryption status if initialized */
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
    console.log('[ESP-NOW] startPolling called, pollInterval=' + pollInterval);
    if (pollInterval || authFailed) {
      console.log('[ESP-NOW] Polling already active or auth failed, skipping');
      return;
    }
    console.log('[ESP-NOW] Starting polling (500ms interval)');
    pollEspNowMessages();
    pollInterval = setInterval(pollEspNowMessages, 500);
    console.log('[ESP-NOW] Polling started, interval ID=' + pollInterval);
  }
  
  function stopPolling() {
    if (pollInterval) {
      clearInterval(pollInterval);
      pollInterval = null;
    }
  }
  
  window.addEventListener('beforeunload', stopPolling);
  
  document.addEventListener('DOMContentLoaded', function(){
    startPolling();
  });
})();
</script>
)JS", HTTPD_RESP_USE_STRLEN);
  
  // Include generic file browser utility
  httpd_resp_send_chunk(req, getFileBrowserScript().c_str(), HTTPD_RESP_USE_STRLEN);
}

void registerEspNowHandlers(httpd_handle_t server);

#endif
