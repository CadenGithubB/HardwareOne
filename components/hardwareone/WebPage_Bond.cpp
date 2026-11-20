#include "System_BuildConfig.h"
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>  // Required for httpd_handle_t in stub when ENABLE_WEB_BOND=0
#endif

#if ENABLE_WEB_BOND

#include <Arduino.h>
#include <memory>

#include "System_User.h"
#include "System_Utils.h"
#include "WebPage_Bond.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"
#include "System_ESPNow.h"
#include "System_ESPNow_Sensors.h"
#include "System_ESPNow_BondForward.h"  // the "forward" object of the bond status
#include "System_Settings.h"
#include "System_Filesystem.h"
#include "System_MemUtil.h"       // ps_alloc / AllocPref for batch body buffer
#include "System_CommandLimits.h"  // the exec body and command buffers match the command limits
#include <esp_attr.h>
#include "System_BondedPeer.h"    // BondedPeer:: — unified accessor for the bonded worker (settings/schema sync, cache reads)
#include "System_SelfDevice.h"    // SelfDevice:: — local MAC / name / uptime / heap accessors
#include "System_ESPNow_Wire.h"   // V4PayloadFsListReplyHeader, V4PayloadFsEntry, FsListStatus
#include "System_ESPNow_FsList.h" // fsListSendRequest / fsListCancel — structured peer FS listing
#include "WebBondFsPollCore.h"    // bond_fs_poll: start-and-poll state of the three /api/bond/fs routes
#include "System_ArgParse.h"      // parseIntArg: the id of a poll
#include "System_FileManager.h"   // FILE_MANAGER_MAX_PATH (path buffer sizing)
#include <freertos/semphr.h>
#include "System_CommandRegistry.h"



// =============================================================================
// Helper Functions
// =============================================================================

static inline esp_err_t webBondSendChunk(httpd_req_t* req, const char* s) {
  return httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN);
}

static inline esp_err_t webBondSendChunkf(httpd_req_t* req, const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
}

// =============================================================================
// Bond Dashboard Page
// =============================================================================

#if ENABLE_BONDED_MODE

void streamBondInner(httpd_req_t* req) {
  // CSS
  httpd_resp_send_chunk(req, R"CSS(
<style>
.remote-container { max-width: 1200px; margin: 0 auto; padding: 20px; }
.remote-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(350px, 1fr)); gap: 20px; margin-bottom: 20px; }
.remote-card { background: var(--panel-bg); border-radius: 15px; padding: 20px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); border: 1px solid var(--border); }
.remote-title { font-size: 1.3em; font-weight: bold; margin-bottom: 10px; color: var(--panel-fg); display: flex; align-items: center; gap: 10px; }
.remote-description { color: var(--muted); margin-bottom: 15px; font-size: 0.9em; }
.status-dot { display: inline-block; width: 12px; height: 12px; border-radius: 50%; }
.status-online { background: #28a745; animation: pulse 2s infinite; }
.status-offline { background: #dc3545; }
.status-unknown { background: #6c757d; }
.health-bar { height: 8px; background: var(--border); border-radius: 4px; overflow: hidden; margin: 8px 0; }
.health-fill { height: 100%; transition: width 0.5s, background 0.5s; }
.health-excellent { background: #28a745; }
.health-good { background: #7cb342; }
.health-fair { background: #ffc107; }
.health-poor { background: #ff9800; }
.health-bad { background: #dc3545; }
.stat-row { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid var(--border); }
.stat-row:last-child { border-bottom: none; }
.stat-label { color: var(--muted); }
.stat-value { font-weight: 500; font-family: 'Courier New', monospace; }
.sensor-table { width: 100%; margin-top: 10px; }
.sensor-table-header { display: flex; padding: 4px 0 8px; border-bottom: 1px solid var(--border); margin-bottom: 4px; font-size: 0.8em; color: var(--muted); text-transform: uppercase; letter-spacing: 0.5px; }
.sensor-table-header .st-name { flex: 1; }
.sensor-table-header .st-col { width: 60px; text-align: center; }
.sensor-row { display: flex; align-items: center; padding: 8px 0; border-bottom: 1px solid var(--border); }
.sensor-row:last-child { border-bottom: none; }
.sensor-row .st-name { flex: 1; font-size: 0.9em; font-weight: 500; }
.sensor-row .st-name.disconnected { color: var(--muted); text-decoration: line-through; }
.sensor-row .st-col { width: 60px; display: flex; justify-content: center; }
.toggle-switch { width: 36px; height: 20px; background: var(--border); border-radius: 10px; position: relative; transition: background 0.2s; cursor: pointer; flex-shrink: 0; }
.toggle-switch.on { background: #28a745; }
.toggle-switch.disabled { opacity: 0.35; cursor: not-allowed; pointer-events: none; }
.toggle-switch::after { content: ''; position: absolute; width: 16px; height: 16px; background: white; border-radius: 50%; top: 2px; left: 2px; transition: left 0.2s; }
.toggle-switch.on::after { left: 18px; }
.cli-input { display: flex; gap: 10px; margin-top: 15px; }
.cli-input input { flex: 1; padding: 10px; border: 1px solid var(--border); border-radius: 8px; font-family: 'Courier New', monospace; background: var(--panel-bg); color: var(--panel-fg); }
.cli-output { background: rgba(0, 0, 0, 0.5); color: #fff; border-radius: 8px; padding: 12px; font-family: 'Courier New', monospace; font-size: 0.85em; max-height: 400px; overflow-y: auto; margin-top: 10px; white-space: pre-wrap; border: 1px solid #333; }
.no-bond-warning { text-align: center; padding: 40px 20px; color: var(--muted); }
.no-bond-warning h3 { color: var(--panel-fg); margin-bottom: 10px; }
.refresh-btn { position: absolute; top: 15px; right: 15px; padding: 6px 12px; font-size: 0.85em; }
.link-quality { display: flex; align-items: center; gap: 8px; }
.signal-bars { display: flex; align-items: flex-end; gap: 2px; height: 16px; }
.signal-bar { width: 4px; background: var(--border); border-radius: 1px; }
.signal-bar.active { background: #28a745; }
.bond-progress-card { max-width: 520px; margin: 40px auto; padding: 30px; background: var(--panel-bg); border-radius: 15px; border: 1px solid var(--border); box-shadow: 0 4px 6px rgba(0,0,0,0.1); }
.bond-progress-title { font-size: 1.4em; font-weight: bold; margin-bottom: 5px; color: var(--panel-fg); text-align: center; }
.bond-progress-sub { color: var(--muted); text-align: center; margin-bottom: 20px; font-size: 0.9em; word-break: break-all; }
.bond-progress-bar { height: 10px; background: var(--border); border-radius: 5px; overflow: hidden; margin: 20px 0 8px; }
.bond-progress-fill { height: 100%; background: var(--accent); border-radius: 5px; transition: width 0.6s ease; }
.bond-progress-pct { text-align: center; font-size: 0.8em; color: var(--muted); margin-bottom: 10px; }
.bond-progress-steps { margin-top: 10px; }
.bond-progress-step { display: flex; align-items: center; gap: 12px; padding: 10px 0; border-bottom: 1px solid var(--border); }
.bond-progress-step:last-child { border-bottom: none; }
.bond-step-icon { width: 22px; height: 22px; border-radius: 50%; display: inline-flex; align-items: center; justify-content: center; flex-shrink: 0; font-size: 0.85em; font-weight: bold; box-sizing: border-box; }
.bond-step-done .bond-step-icon { background: var(--accent); color: #fff; }
.bond-step-current .bond-step-icon { background: transparent; border: 2px solid var(--border); border-top-color: var(--accent); animation: bondSpin 0.9s linear infinite; }
.bond-step-pending .bond-step-icon { background: var(--border); color: var(--muted); }
.bond-step-label { flex: 1; font-size: 0.95em; }
.bond-step-done .bond-step-label { color: var(--panel-fg); }
.bond-step-current .bond-step-label { color: var(--panel-fg); font-weight: 500; }
.bond-step-pending .bond-step-label { color: var(--muted); }
.bond-progress-hint { margin-top: 16px; padding: 10px 12px; background: var(--warning-bg); border-left: 3px solid var(--warning-accent); border-radius: 4px; font-size: 0.85em; color: var(--warning-fg); }
.bond-progress-actions { margin-top: 20px; display: flex; justify-content: center; gap: 10px; }
@keyframes bondSpin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }
</style>
)CSS", HTTPD_RESP_USE_STRLEN);

  // HTML Structure
  httpd_resp_send_chunk(req, R"HTML(
<div class='remote-container'>
<div id='remote-content'>
</div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  // JavaScript
  httpd_resp_send_chunk(req, R"JS(
<script>
(function() {
  let refreshInterval = null;
  let lastStatus = null;
  const sensorEverSeen = {};
  // First time the page sees a not-yet-synced bond on this load. Used to time
  // the "waiting for peer" hint in the progress overlay. Reset to null once
  // sync completes so a future drop+reconnect starts a fresh timer.
  let bondProgressStartMs = null;
  
  function formatUptime(seconds) {
    if (seconds < 60) return seconds + 's';
    if (seconds < 3600) return Math.floor(seconds/60) + 'm ' + (seconds%60) + 's';
    const h = Math.floor(seconds/3600);
    const m = Math.floor((seconds%3600)/60);
    return h + 'h ' + m + 'm';
  }
  
  function getHealthClass(score) {
    if (score >= 90) return 'health-excellent';
    if (score >= 70) return 'health-good';
    if (score >= 50) return 'health-fair';
    if (score >= 30) return 'health-poor';
    return 'health-bad';
  }
  
  function renderSignalBars(rssi) {
    const strength = Math.min(4, Math.max(0, Math.floor((rssi + 90) / 15) + 1));
    let html = '<div class="signal-bars">';
    for (let i = 1; i <= 4; i++) {
      html += '<div class="signal-bar' + (i <= strength ? ' active' : '') + '" style="height:' + (i*4) + 'px"></div>';
    }
    html += '</div>';
    return html;
  }

  // Bond is "fully synced" once the peer is responding AND we have both its
  // static capabilities and a live status snapshot. Until then we show a
  // progress overlay in place of the dashboard so the user never sees the
  // half-empty card with "pending..." placeholders.
  function isBondSynced(data) {
    return !!data.peerOnline
        && !!data.capabilities
        && !!(data.peerStatus && data.peerStatus.valid);
  }

  function renderBondProgress(data, container) {
    const steps = [
      { label: 'Bond configured',       done: !!(data.bonded && data.peerConfigured) },
      { label: 'Peer responding',       done: !!data.peerOnline },
      { label: 'Capabilities received', done: !!data.capabilities },
      { label: 'Live status synced',    done: !!(data.peerStatus && data.peerStatus.valid) }
    ];
    const doneCount = steps.filter(function(s){return s.done;}).length;
    const pct = Math.round((doneCount / steps.length) * 100);
    let currentIdx = -1;
    for (let i = 0; i < steps.length; i++) {
      if (!steps[i].done) { currentIdx = i; break; }
    }

    if (bondProgressStartMs === null) bondProgressStartMs = Date.now();
    const elapsedMs = Date.now() - bondProgressStartMs;
    let hint = '';
    // The peer name is free text (the pairing registry name when no capability
    // name has arrived): escape it before it reaches the hint's innerHTML.
    const peerLabel = hw.esc(data.peerName && data.peerName !== 'Unknown' ? data.peerName : 'The other device');
    if (data.agreement === 'declined') {
      // The partner answered: it has no bond with this device. Waiting will
      // not help; say what does.
      if (data.agreementReason === 'ended') {
        hint = peerLabel + ' ended the bond. Run bondconnect on it again to bond, or use Cancel / Unbond here.';
      } else if (data.agreementReason === 'bonded_elsewhere') {
        hint = peerLabel + ' is bonded to another device. Run bondconnect on it to bond it with this device, or use Cancel / Unbond here.';
      } else {
        hint = peerLabel + ' has bond mode off. Run bondconnect on it to bond, or use Cancel / Unbond here.';
      }
    } else if (elapsedMs > 8000 && !data.peerOnline) {
      hint = 'Waiting for the bonded peer to respond. Make sure the other device is powered on and within range.';
    } else if (elapsedMs > 15000 && data.peerOnline && !data.capabilities) {
      hint = 'Connected, but the peer has not sent its capabilities yet. It may be busy or running older firmware.';
    } else if (elapsedMs > 20000 && data.capabilities && !(data.peerStatus && data.peerStatus.valid)) {
      hint = 'Capabilities received. Waiting for the first live status update...';
    }

    let html = '';
    html += '<div class="bond-progress-card">';
    html += '<div class="bond-progress-title">' + (data.agreement === 'declined' ? 'Bond Not Confirmed' : 'Establishing Bond') + '</div>';
    html += '<div class="bond-progress-sub">' + hw.esc(data.peerName || 'Bonded device') + ' &middot; ' + hw.esc(data.peerMac || '') + '</div>';
    html += '<div class="bond-progress-bar"><div class="bond-progress-fill" style="width:' + pct + '%"></div></div>';
    html += '<div class="bond-progress-pct">' + doneCount + ' of ' + steps.length + ' steps complete</div>';
    html += '<div class="bond-progress-steps">';
    for (let i = 0; i < steps.length; i++) {
      const s = steps[i];
      let cls = 'bond-step-pending';
      let glyph = '';
      if (s.done) { cls = 'bond-step-done'; glyph = '&#10003;'; }
      else if (i === currentIdx) { cls = 'bond-step-current'; }
      html += '<div class="bond-progress-step ' + cls + '">';
      html += '<span class="bond-step-icon">' + glyph + '</span>';
      html += '<span class="bond-step-label">' + s.label + '</span>';
      html += '</div>';
    }
    html += '</div>';
    if (hint) {
      html += '<div class="bond-progress-hint">' + hint + '</div>';
    }
    html += '<div class="bond-progress-actions" data-guest-hide>';
    html += '<button class="btn" onclick="window.forceBondResync()" style="font-size:0.85em;padding:6px 14px;margin-right:8px">Force Re-sync</button>';
    html += '<button class="btn" onclick="window.unbondDevice()" style="font-size:0.85em;padding:6px 14px">Cancel / Unbond</button>';
    html += '</div>';
    html += '</div>';
    container.innerHTML = html;
  }

  function renderBondDashboard(data) {
    const container = hw.$('remote-content');
    if (!container) return;
    
    // Preserve CLI state across re-renders
    const cmdInput = hw.$('remote-cmd');
    const savedCmd = cmdInput ? cmdInput.value : '';
    const hadFocus = cmdInput && document.activeElement === cmdInput;
    const outputEl = hw.$('remote-output');
    const savedOutput = outputEl ? outputEl.textContent : '';
    
    // Check if ESP-NOW is enabled first
    if (!data.espnowEnabled) {
      container.innerHTML = `
        <div class='alert alert-warning' style='background:#fff3cd;border:1px solid #ffeaa7;color:#856404;padding:20px;border-radius:8px;margin:20px;'>
          <h3 style='margin-top:0;color:#856404;'>ESP-NOW Disabled</h3>
          <p style='margin-bottom:10px;'>ESP-NOW is currently disabled. Bond mode requires ESP-NOW to be initialized.</p>
          <p style='margin-bottom:0;'>Please visit the <a href='/espnow' style='color:#856404;text-decoration:underline;'>ESP-NOW page</a> to initialize ESP-NOW, then return here to configure bonding.</p>
        </div>
      `;
      return;
    }
    
    if (!data.bonded || !data.peerConfigured) {
      // Bond configuration UI. The whole page auto-refreshes every 5s; do NOT let
      // that rebuild the device list and wipe the user's selection. Once the config
      // UI is on screen, leave it (and the dropdown) untouched until the user
      // explicitly presses "Refresh List" (which calls refreshBondDevices()).
      if (hw.$('bond-device-select')) {
        return;
      }
      // Show bond configuration UI
      let html = '<div class="remote-grid">';
      html += '<div class="remote-card" style="grid-column:1/-1">';
      html += '<div class="remote-title">Bond Configuration</div>';
      html += '<div class="remote-description">Select a paired ESP-NOW device to bond with</div>';
      html += '<div style="margin-top:15px">';
      html += '<label style="display:block;margin-bottom:8px;font-weight:500">Available Devices:</label>';
      html += '<select id="bond-device-select" class="input-fit input-m" style="padding:10px;border:1px solid var(--border);border-radius:8px;background:var(--panel-bg);color:var(--panel-fg);font-size:0.95em">';
      html += '<option value="">Loading devices...</option>';
      html += '</select>';
      html += '</div>';
      html += '<div style="margin-top:15px;display:flex;gap:10px">';
      html += '<button class="btn" onclick="window.connectBondDevice()" id="btn-bond-connect" data-guest-hide>Connect</button>';
      html += '<button class="btn" onclick="window.refreshBondDevices()">Refresh List</button>';
      html += '</div>';
      html += '<div id="bond-config-status" style="margin-top:10px;padding:10px;border-radius:6px;display:none"></div>';
      html += '<div style="margin-top:15px;padding:10px;background:rgba(255,255,255,0.05);border-radius:6px;font-size:0.85em;color:var(--muted)">';
      html += 'No paired devices available? Visit the ESP-NOW page to pair devices first.';
      html += '</div>';
      html += '</div>';
      html += '</div>';
      container.innerHTML = html;
      window.refreshBondDevices();
      return;
    }

    // Bond is configured but the handshake/sync isn't fully through yet.
    // Render the progress overlay and re-poll faster than the 5s baseline.
    if (!isBondSynced(data)) {
      renderBondProgress(data, container);
      if (window.__bondProgressTimer) clearTimeout(window.__bondProgressTimer);
      window.__bondProgressTimer = setTimeout(window.refreshBond, 1200);
      return;
    }

    // Fully synced — clear any progress-only state so a future disconnect
    // starts fresh, and fall through to the normal dashboard render.
    if (window.__bondProgressTimer) {
      clearTimeout(window.__bondProgressTimer);
      window.__bondProgressTimer = null;
    }
    bondProgressStartMs = null;

    const online = data.peerOnline;
    const statusClass = online ? 'status-online' : 'status-offline';
    const statusText = online ? 'Online' : 'Offline';
    
    let html = '<div class="remote-grid">';
    
    // Bonded Device Card — ONE unified card: identity + connection status + the
    // bonded device's hardware/capabilities + live status + link quality. This was
    // previously split across two separate "Bonded Device" cards. The Remote
    // Command Execution box is intentionally kept as its own separate card below.
    html += '<div class="remote-card" style="position:relative">';
    html += '<button class="btn refresh-btn" onclick="window.forceBondResync()" title="Force the device to re-fetch capabilities/manifest/settings/schema from the bonded peer" data-guest-hide>Refresh</button>';
    html += '<div class="remote-title"><span class="status-dot ' + statusClass + '"></span>Bonded Device</div>';
    const localRole = data.role === 1 ? 'Master' : 'Worker';
    const remoteRole = data.role === 1 ? 'Worker' : 'Master';
    html += '<div class="remote-description">This device: ' + localRole + ' · Bonded device: ' + hw.esc(data.peerName || 'Unknown') + ' (' + remoteRole + ')</div>';
    if (data.agreementReason === 'role_conflict') {
      // Both ends report the same role, so the line above cannot be right.
      html += '<div class="remote-description" style="color:var(--danger,#c0392b)">Both devices are set to ' + localRole + '. Run bondrole on one of them so one is master and one is worker.</div>';
    }
    html += '<div style="margin:8px 0;display:flex;gap:8px;flex-wrap:wrap" data-guest-hide><button class="btn" onclick="window.swapRoles()" style="font-size:0.8em;padding:4px 12px">Swap Roles</button><button class="btn" onclick="window.unbondDevice()" style="font-size:0.8em;padding:4px 12px">Unbond</button></div>';

    // Rows are grouped by concern, separated by a thin divider. Each group
    // emits only the fields whose underlying data is available; if a whole
    // group has no data it's omitted entirely. A consolidated "pending..."
    // line appears at the bottom only when both data sources are missing.
    const hasCaps = !!data.capabilities;
    const hasPeerStatus = !!(data.peerStatus && data.peerStatus.valid);

    // 1) Specs — identity and static hardware
    html += '<div class="stat-row"><span class="stat-label">MAC Address</span><span class="stat-value">' + (data.peerMac || '—') + '</span></div>';
    if (hasCaps) {
      html += '<div class="stat-row"><span class="stat-label">Flash</span><span class="stat-value">' + (data.capabilities.flashMB || '?') + ' MB</span></div>';
      html += '<div class="stat-row"><span class="stat-label">PSRAM</span><span class="stat-value">' + (data.capabilities.psramMB || '?') + ' MB</span></div>';
      if (data.capabilities.features) {
        html += '<div class="stat-row"><span class="stat-label">Features</span><span class="stat-value" style="font-size:0.8em;max-width:60%;text-align:right">' + data.capabilities.features + '</span></div>';
      }
    }

    // 2) Status & Time — liveness and uptime metrics
    html += '<div style="border-top:1px solid var(--panel-border);margin-top:8px;padding-top:8px">';
    html += '<div class="stat-row"><span class="stat-label">Status</span><span class="stat-value">' + statusText + '</span></div>';
    if (online && data.lastHeartbeatAgeSec !== undefined) {
      html += '<div class="stat-row"><span class="stat-label">Last Seen</span><span class="stat-value">' + data.lastHeartbeatAgeSec + 's ago</span></div>';
    }
    if (data.peerUptime !== undefined) {
      html += '<div class="stat-row"><span class="stat-label">Peer Uptime</span><span class="stat-value">' + formatUptime(data.peerUptime) + '</span></div>';
    }
    if (hasPeerStatus) {
      html += '<div class="stat-row"><span class="stat-label">Status Age</span><span class="stat-value">' + data.peerStatus.ageSec + 's ago</span></div>';
    }
    html += '</div>';

    // 3) Network — WiFi state + active services
    const hasNetwork = hasPeerStatus || (hasCaps && data.capabilities.services);
    if (hasNetwork) {
      html += '<div style="border-top:1px solid var(--panel-border);margin-top:8px;padding-top:8px">';
      if (hasPeerStatus) {
        html += '<div class="stat-row"><span class="stat-label">WiFi</span><span class="stat-value">' + (data.peerStatus.wifiConnected ? 'Connected' : 'Disconnected') + '</span></div>';
      }
      if (hasCaps && data.capabilities.services) {
        html += '<div class="stat-row"><span class="stat-label">Services</span><span class="stat-value" style="font-size:0.8em;max-width:60%;text-align:right">' + data.capabilities.services + '</span></div>';
      }
      html += '</div>';
    }

    // 4) Memory — runtime heap stats
    if (hasPeerStatus) {
      html += '<div style="border-top:1px solid var(--panel-border);margin-top:8px;padding-top:8px">';
      html += '<div class="stat-row"><span class="stat-label">Free Heap</span><span class="stat-value">' + Math.round(data.peerStatus.freeHeap / 1024) + ' KB</span></div>';
      html += '<div class="stat-row"><span class="stat-label">Min Free Heap</span><span class="stat-value">' + Math.round(data.peerStatus.minFreeHeap / 1024) + ' KB</span></div>';
      html += '</div>';
    }

    // 5) I2C Sensors — capability-gated subgrid (kept inside capabilities check)
    if (hasCaps) {
      const capSensorMask = data.capabilities.sensorMask || 0;
      const connected = data.sensorConnected || {};
      // The mask bit of each sensor comes from the device (data.sensorBits, the
      // firmware's one sensor numbering); no number is typed in this page.
      const rBits = data.sensorBits || {};
      const rDefs = [{n:'Thermal',k:'thermal'},{n:'ToF',k:'tof'},{n:'IMU',k:'imu'},{n:'Input',k:'input'},{n:'APDS',k:'apds'},{n:'GPS',k:'gps'},{n:'RTC',k:'rtc'},{n:'Presence',k:'presence'}].map(function(d){d.m = rBits[d.k] || 0; return d;});
      const rRows = rDefs.filter(function(d){return capSensorMask & d.m;});
      if (rRows.length > 0) {
        html += '<div style="border-top:1px solid var(--panel-border);margin-top:8px;padding-top:8px">';
        html += '<div class="stat-row"><span class="stat-label">I2C Sensors</span></div>';
        html += '<div style="display:grid;grid-template-columns:1fr 1fr;gap:2px 8px;margin:2px 0 6px 0">';
        for (const d of rRows) {
          const on = connected[d.k] === true;
          const hasLiveR = connected.valid === true;
          const badge = !hasLiveR ? '<span style="color:var(--muted);font-size:0.78em">—</span>' : '<span style="font-size:0.78em;font-weight:600;color:' + (on ? '#2ecc71' : '#e74c3c') + '">' + (on ? 'ON' : 'OFF') + '</span>';
          html += '<span style="font-size:0.82em;color:var(--panel-fg);opacity:0.8">' + d.n + '</span>';
          html += '<span style="text-align:right">' + badge + '</span>';
        }
        html += '</div>';
        html += '</div>';
      }
    }

    // Pending hints (only when underlying data is genuinely missing — avoids
    // empty/duplicate "pending" spam when one source is present and the other isn't)
    if (!hasCaps && !hasPeerStatus) {
      html += '<div style="border-top:1px solid var(--panel-border);margin-top:8px;padding-top:8px;text-align:center;font-size:0.85em;color:var(--panel-fg);opacity:0.6">Specs & live status pending...</div>';
    } else if (!hasCaps) {
      html += '<div style="border-top:1px solid var(--panel-border);margin-top:8px;padding-top:8px;text-align:center;font-size:0.85em;color:var(--panel-fg);opacity:0.6">Capabilities pending...</div>';
    } else if (!hasPeerStatus) {
      html += '<div style="border-top:1px solid var(--panel-border);margin-top:8px;padding-top:8px;text-align:center;font-size:0.85em;color:var(--panel-fg);opacity:0.6">Live status pending...</div>';
    }

    // Link Quality — RSSI, heartbeat counts, packet loss and health for the link
    // to this peer.
    const health = data.heartbeatsTx > 0 ? Math.min(100, Math.round((data.heartbeatsRx / data.heartbeatsTx) * 100)) : 0;
    html += '<div style="border-top:1px solid var(--panel-border);margin-top:8px;padding-top:8px">';
    html += '<div class="stat-row"><span class="stat-label">Link Quality</span><span class="stat-value">' + health + '% Health</span></div>';
    html += '<div class="health-bar"><div class="health-fill ' + getHealthClass(health) + '" style="width:' + health + '%"></div></div>';
    html += '<div class="stat-row"><span class="stat-label">RSSI</span><span class="stat-value link-quality">' + renderSignalBars(data.rssi < 0 ? data.rssi : -90) + ' ' + (data.rssi < 0 ? data.rssi + ' dBm' : '—') + '</span></div>';
    html += '<div class="stat-row"><span class="stat-label">Heartbeats RX</span><span class="stat-value">' + (data.heartbeatsRx || 0) + '</span></div>';
    html += '<div class="stat-row"><span class="stat-label">Heartbeats TX</span><span class="stat-value">' + (data.heartbeatsTx || 0) + '</span></div>';
    if (data.packetLoss !== undefined) {
      html += '<div class="stat-row"><span class="stat-label">Packet Loss</span><span class="stat-value">' + data.packetLoss.toFixed(1) + '%</span></div>';
    }
    html += '</div>';

    html += '</div>';  // end unified Bonded Device card

    // Remote Sensors Card (master controls power + streaming on worker)
    {
      const synced = data._dbg_synced === true;
      const isMaster = data.role === 1;
      const remoteSensorMask = data.capabilities ? data.capabilities.sensorMask : 0;
      const sc = data.sensorConnected || {};
      const hasLive = sc.valid === true;
      
      // Resolve a specific name for the remote's input device from the type
      // byte the worker stamps into its capability summary. Falls back to
      // generic "Input" for pre-v0.94 peers that don't send the field (0).
      //   0 = unknown/legacy  → "Input"
      //   1 = Seesaw gamepad  → "Gamepad"
      //   2 = ANO encoder     → "ANO Encoder"
      const remoteInputType = (data.capabilities && data.capabilities.inputDeviceType) || 0;
      const remoteInputName = (remoteInputType === 2) ? 'ANO Encoder'
                            : (remoteInputType === 1) ? 'Gamepad'
                            : 'Input';

      // Each sensor's capability bit comes from the device (data.sensorBits,
      // the firmware's one sensor numbering, keyed by the id below); no mask
      // number is typed in this page. streamable=false for sensors that have
      // no bond streaming pipeline (APDS) — Enable still works, but the Stream
      // toggle is shown disabled rather than dead.
      const sBits = data.sensorBits || {};
      const sensors = [
        {id: 'thermal', name: 'Thermal',        stream: data.streamThermal,  on: sc.thermalOn,  streamable: true},
        {id: 'tof',     name: 'ToF',            stream: data.streamTof,      on: sc.tofOn,      streamable: true},
        {id: 'imu',     name: 'IMU',            stream: data.streamImu,      on: sc.imuOn,      streamable: true},
        {id: 'input',   name: remoteInputName,  stream: data.streamInput,    on: sc.inputOn,    streamable: true},
        {id: 'apds',    name: 'APDS',     stream: false,               on: sc.apdsOn,     streamable: false},
        {id: 'gps',     name: 'GPS',      stream: data.streamGps,      on: sc.gpsOn,      streamable: true},
        {id: 'rtc',     name: 'RTC',      stream: data.streamRtc,      on: sc.rtcOn,      streamable: true},
        {id: 'presence',name: 'Presence', stream: data.streamPresence, on: sc.presenceOn, streamable: true},
        {id: 'fmradio', name: 'FM Radio', stream: data.streamFmradio,  on: sc.fmradioOn,  streamable: true}
      ];
      for (const s of sensors) s.mask = sBits[s.id] || 0;
      
      // Latch: if a sensor was ever connected or enabled, it's physically present
      if (hasLive) {
        for (const s of sensors) {
          if (sc[s.id] === true || s.on === true) sensorEverSeen[s.id] = true;
        }
      }
      
      // Filter to sensors compiled on the bonded device
      const visible = sensors.filter(function(s) { return data.capabilities && (remoteSensorMask & s.mask); });

      // Only the master controls the peer's sensors. On the worker this card is
      // pure noise (every toggle disabled), and the peer's sensor state is already
      // shown read-only in the "Bonded Device" card — so render it master-only.
      if (isMaster && visible.length > 0) {
        html += '<div class="remote-card" data-guest-hide>';
        html += '<div class="remote-title">Remote Sensors</div>';
        if (!synced) {
          html += '<div class="remote-description" style="color:var(--muted)">Waiting for bond sync to complete...</div>';
        } else {
          html += '<div class="remote-description">Control sensors on the bonded device</div>';
        }
        html += '<div class="sensor-table">';
        html += '<div class="sensor-table-header"><span class="st-name">Sensor</span><span class="st-col">Enable</span><span class="st-col">Stream</span></div>';
        
        for (const s of visible) {
          const isOn = hasLive && s.on === true;
          const canControl = synced && isMaster;

          // NOTE: the remote reports a sensor as "connected" (gXxxConnected)
          // only AFTER its task has started — that flag flips true inside each
          // sensor's start path on successful I2C init, not from a boot-time
          // presence probe. So a compiled-but-idle sensor reads back as not
          // connected. Gating the *Enable* toggle on that made it impossible to
          // ever start a remote sensor from the master: the one control that
          // would set it running was disabled until it was already running.
          // Enable is therefore available whenever this is the synced master
          // (the list is already filtered to sensors compiled on the remote).
          // Its on/off state reflects the sensor's real running state once the
          // peer reports back; if no hardware is actually present the start
          // simply doesn't stick and the toggle returns to off.
          const nameClass = 'st-name' + (isOn ? '' : ' off');

          // Enable toggle: starts/stops the remote sensor's task.
          const enableOn = isOn ? ' on' : '';
          const enableDisabled = !canControl ? ' disabled' : '';
          const enableClick = canControl ? 'onclick="window.toggleSensorEnable(\'' + s.id + '\',' + (isOn ? 'false' : 'true') + ')"' : '';
          const enableTitle = !canControl ? 'title="Only the master device can control sensors"' : '';

          // Stream toggle: forwards the sensor's data over ESP-NOW. Requires the
          // sensor to be running first (can't stream an off sensor) and a bond
          // streaming pipeline to exist (streamable).
          const streamSupported = s.streamable !== false;
          const streamOn = s.stream ? ' on' : '';
          const streamDisabled = (!canControl || !isOn || !streamSupported) ? ' disabled' : '';
          const streamClick = (canControl && isOn && streamSupported) ? 'onclick="window.toggleSensor(\'' + s.id + '\')"' : '';
          const streamTitle = !streamSupported ? 'title="Streaming not supported for this sensor"' : (!canControl ? 'title="Only the master device can control sensors"' : (!isOn ? 'title="Enable the sensor first"' : ''));
          
          html += '<div class="sensor-row">';
          html += '<span class="' + nameClass + '">' + s.name + '</span>';
          html += '<div class="st-col"><div class="toggle-switch' + enableOn + enableDisabled + '" ' + enableClick + ' ' + enableTitle + '></div></div>';
          html += '<div class="st-col"><div class="toggle-switch' + streamOn + streamDisabled + '" ' + streamClick + ' ' + streamTitle + '></div></div>';
          html += '</div>';
        }
        
        html += '</div></div>';
      }
    }
    
    // Local Capabilities Card
    if (data.localCapabilities) {
      html += '<div class="remote-card">';
      html += '<div class="remote-title">This Device</div>';
      // Specs
      html += '<div class="stat-row"><span class="stat-label">Flash</span><span class="stat-value">' + (data.localCapabilities.flashMB || '?') + ' MB</span></div>';
      const localPsram = data.localCapabilities.psramKB ? (data.localCapabilities.psramKB / 1024).toFixed(1) : (data.localCapabilities.psramMB || '?');
      html += '<div class="stat-row"><span class="stat-label">PSRAM</span><span class="stat-value">' + localPsram + ' MB</span></div>';
      if (data.localCapabilities.features) {
        html += '<div class="stat-row"><span class="stat-label">Features</span><span class="stat-value" style="font-size:0.8em;max-width:60%;text-align:right">' + data.localCapabilities.features + '</span></div>';
      }
      // Memory (placed above I2C sensors so heap stats pair naturally with the
      // specs above; mirrors the Bonded Device card grouping).
      html += '<div style="border-top:1px solid var(--panel-border);margin-top:8px;padding-top:8px">';
      html += '<div class="stat-row"><span class="stat-label">Free Heap</span><span class="stat-value">' + Math.round(data.localCapabilities.freeHeap / 1024) + ' KB</span></div>';
      html += '<div class="stat-row"><span class="stat-label">Min Free Heap</span><span class="stat-value">' + Math.round(data.localCapabilities.minFreeHeap / 1024) + ' KB</span></div>';
      html += '</div>';
      // I2C Sensors
      const localSensorMask = data.localCapabilities.sensorMask || 0;
      if (localSensorMask) {
        const lConn = data.localCapabilities.sensorConnectedMask || 0;
        // Local input row label derives from THIS device's compile-time input
        // type (sent in localCapabilities.inputDeviceType). Was hardcoded
        // "Gamepad" which read wrong on ANO builds.
        const localInputType = (data.localCapabilities && data.localCapabilities.inputDeviceType) || 0;
        const localInputName = (localInputType === 2) ? 'ANO Encoder'
                             : (localInputType === 1) ? 'Gamepad'
                             : 'Input';
        const lBits = data.sensorBits || {};
        const lDefs = [{k:'thermal',n:'Thermal'},{k:'tof',n:'ToF'},{k:'imu',n:'IMU'},{k:'input',n:localInputName},{k:'apds',n:'APDS'},{k:'gps',n:'GPS'},{k:'rtc',n:'RTC'},{k:'presence',n:'Presence'},{k:'fmradio',n:'FM Radio'}].map(function(d){d.m = lBits[d.k] || 0; return d;});
        const lRows = lDefs.filter(function(d){return localSensorMask & d.m;});
        if (lRows.length > 0) {
          html += '<div style="border-top:1px solid var(--panel-border);margin-top:8px;padding-top:8px">';
          html += '<div class="stat-row"><span class="stat-label">I2C Sensors</span></div>';
          html += '<div style="display:grid;grid-template-columns:1fr 1fr;gap:2px 8px;margin:2px 0 6px 0">';
          for (const d of lRows) {
            const on = !!(lConn & d.m);
            html += '<span style="font-size:0.82em;color:var(--panel-fg);opacity:0.8">' + d.n + '</span>';
            html += '<span style="font-size:0.78em;font-weight:600;color:' + (on ? '#2ecc71' : '#e74c3c') + ';text-align:right">' + (on ? 'ON' : 'OFF') + '</span>';
          }
          html += '</div>';
          html += '</div>';
        }
      }
      html += '</div>';
    }
    
    // (The second "Bonded Device" card — remote hardware/capabilities + live
    // status — was merged into the unified Bonded Device card above.)

    // Remote CLI Card
    html += '<div class="remote-card" style="grid-column: 1 / -1" data-guest-hide>';
    html += '<div class="remote-title">Remote Command Execution</div>';
    html += '<div class="remote-description">Execute CLI commands on the bonded device</div>';
    html += '<div class="cli-input">';
    html += '<input type="text" id="remote-cmd" placeholder="Enter command (e.g., sensors, memory, status)" autocomplete="one-time-code" data-1p-ignore data-lpignore="true" data-form-type="other" onkeypress="if(event.key===\'Enter\')window.execRemoteCmd()">';
    html += '<button class="btn" onclick="window.execRemoteCmd()">Execute</button>';
    html += '</div>';
    html += '<div class="cli-output" id="remote-output">Ready for commands...</div>';
    html += '</div>';
    
    html += '</div>';
    
    container.innerHTML = html;
    
    // Restore CLI state
    const newInput = hw.$('remote-cmd');
    if (newInput && savedCmd) newInput.value = savedCmd;
    if (newInput && hadFocus) newInput.focus();
    const newOutput = hw.$('remote-output');
    if (newOutput && savedOutput && savedOutput !== 'Ready for commands...') {
      newOutput.textContent = savedOutput;
    }
  }
  
  // Force the device to re-fetch capabilities/manifest/settings/schema from the
  // bonded peer via the bondresync CLI verb, then refresh the UI. Used by the
  // "Refresh" button on the bonded device card AND the "Force Re-sync" button
  // on the "Establishing Bond" modal — both kick a real re-sync rather than
  // just re-rendering with stale data.
  //
  // Safe to call anytime: idempotent on the device side. If bond isn't even
  // configured the CLI returns an error string which we just log + ignore.
  window.forceBondResync = function() {
    // Use hw.postForm so we get the project's standard credentials:'include'
    // + cache:'no-store' + 401-redirect-to-login behavior. Raw fetch worked
    // most of the time but silently failed if the auth cookie was missing
    // or if the user's session had expired since page load.
    hw.postForm('/api/cli', {cmd: 'bondresync --all'})
      .then(function(r){ return r.text(); })
      .then(function(txt){
        console.log('[Bond] bondresync result:', txt);
        // Brief delay so the device has a moment to fire the request chain
        // before we re-fetch status. The full sync (cap+manifest+settings
        // file transfers) takes ~1-2 seconds, but with the new hybrid fix
        // most role-swap cases skip the transfers entirely (cached data is
        // preserved) so the modal often clears on the FIRST refresh. The
        // regular 5s auto-refresh interval handles the rest.
        setTimeout(window.refreshBond, 400);
      })
      .catch(function(e){
        console.error('[Bond] bondresync error:', e);
        // Still refresh display so the user sees current state
        window.refreshBond();
      });
  };

  window.refreshBond = function() {
    hw.fetchJSON('/api/bond/status')
      .then(data => {
        console.log('[Bond] API response:', JSON.stringify({
          role: data.role, bonded: data.bonded, peerOnline: data.peerOnline,
          peerName: data.peerName, hasCaps: !!data.capabilities,
          capSensorMask: data.capabilities ? data.capabilities.sensorMask : null,
          sensorConnected: data.sensorConnected,
          _dbg: {synced: data._dbg_synced, capValid: data._dbg_capValid, capSent: data._dbg_capSent, statusValid: data._dbg_statusValid}
        }));
        lastStatus = data;
        renderBondDashboard(data);
      })
      .catch(e => {
        console.error('[Bond] Status fetch error:', e);
      });
  };
  
  window.toggleSensor = function(sensorId) {
    hw.postForm('/api/bond/stream', {sensor: sensorId, action: 'toggle'})
    .then(function(r){ return r.json(); })
    .then(data => {
      if (data.success) {
        window.refreshBond();
      } else {
        alert('Failed to toggle sensor: ' + (data.error || 'Unknown error'));
      }
    })
    .catch(e => {
      console.error('[Bond] Toggle error:', e);
    });
  };
  
  window.toggleSensorEnable = function(sensorId, enable) {
    var cmd = (enable ? 'open' : 'close') + sensorId;
    hw.postForm('/api/bond/exec', {cmd: cmd})
    .then(function(r){ return r.json(); })
    .then(data => {
      if (data.success) {
        setTimeout(window.refreshBond, 1500);
      } else {
        alert('Failed to ' + (enable ? 'enable' : 'disable') + ' sensor: ' + (data.result || data.error || 'Unknown error'));
      }
    })
    .catch(e => {
      console.error('[Bond] Sensor enable error:', e);
    });
  };
  
  window.swapRoles = function() {
    if (!confirm('Swap master/worker roles on both devices?')) return;
    hw.postForm('/api/bond/role', {action: 'swap'})
    .then(function(r){ return r.json(); })
    .then(data => {
      if (data.success) {
        setTimeout(window.refreshBond, 1000);
      } else {
        alert('Failed to swap roles: ' + (data.error || 'Unknown error'));
      }
    })
    .catch(e => {
      console.error('[Bond] Role swap error:', e);
    });
  };

  window.unbondDevice = function() {
    if (!confirm('Unbond from this device? This ends the bond on THIS device. The peer will show offline until you unbond it there too.')) return;
    hw.postForm('/api/cli', {cmd: 'bonddisconnect'})
    .then(function(r){ return r.text(); })
    .then(function(){
      // bonddisconnect clears bond mode locally; reload so the page returns to
      // the device-selection view.
      setTimeout(function(){ window.location.reload(); }, 600);
    })
    .catch(function(e){ alert('Unbond failed: ' + e.message); });
  };

  // Track highest message sequence seen so we only show new messages
  var bondMsgSeq = 0;
  // Initialize bondMsgSeq on page load by fetching current max
  (function() {
    var mac = lastStatus ? (lastStatus.peerMac || '') : '';
    var url = '/api/espnow/messages?since=0' + (mac ? '&mac=' + encodeURIComponent(mac) : '');
    hw.fetchJSON(url).then(function(data) {
      if (data.messages && data.messages.length > 0) {
        for (var i = 0; i < data.messages.length; i++) {
          if (data.messages[i].seq > bondMsgSeq) bondMsgSeq = data.messages[i].seq;
        }
        console.log('[Bond] Initialized bondMsgSeq=' + bondMsgSeq);
      }
    }).catch(function(){});
  })();

  window.execRemoteCmd = function() {
    const input = hw.$('remote-cmd');
    const cmd = input.value.trim();
    if (!cmd) return;

    // While waiting for remote output, this page re-renders every 5s via refreshBond().
    // If we keep a stale DOM reference, output will be written to a detached node.
    // Pause auto-refresh during command execution and always re-resolve the output element.
    const hadRefresh = !!refreshInterval;
    if (refreshInterval) {
      clearInterval(refreshInterval);
      refreshInterval = null;
    }

    function getOutputEl() {
      return hw.$('remote-output');
    }

    function setOutputText(text) {
      const el = getOutputEl();
      hw.setText(el, text);
    }

    function setOutputBorder(color) {
      const el = getOutputEl();
      if (el) el.style.borderLeftColor = color || '';
    }

    function finishRemoteCmd() {
      if (hadRefresh && !refreshInterval) {
        refreshInterval = setInterval(window.refreshBond, 5000);
      }
    }

    setOutputText('> ' + cmd + '\nSending to bonded device...');
    setOutputBorder('');
    input.value = '';
    
    const bondPeerMac = lastStatus ? (lastStatus.peerMac || '') : '';
    console.log('[Bond] execRemoteCmd: mac=' + bondPeerMac + ' sinceSeq=' + bondMsgSeq);
    console.log('[Bond] lastStatus.peerMac=' + (lastStatus ? lastStatus.peerMac : 'NO_STATUS'));
    var gotOutput = false;
    var pollTimer = null;
    var pollCount = 0;
    var maxPolls = 30;  // 30 x 500ms = 15s timeout
    var lastNewMsgPoll = 0;  // poll count when last new message arrived
    var gracePolls = 6;  // keep polling 6 x 500ms = 3s after last new message
    
    // Poll /api/espnow/messages for new messages from bonded peer
    function pollMessages() {
      var url = '/api/espnow/messages?since=' + bondMsgSeq;
      if (bondPeerMac) url += '&mac=' + encodeURIComponent(bondPeerMac);
      hw.fetchJSON(url)
        .then(function(data) {
          if (data.messages && data.messages.length > 0) {
            for (var i = 0; i < data.messages.length; i++) {
              var m = data.messages[i];
              if (m.seq > bondMsgSeq) bondMsgSeq = m.seq;
              if (m.msg) {
                if (!gotOutput) {
                  setOutputText('> ' + cmd + '\n\n' + m.msg);
                  gotOutput = true;
                } else {
                  const el = getOutputEl();
                  if (el) el.textContent += '\n' + m.msg;
                }
                const el = getOutputEl();
                if (el) el.scrollTop = el.scrollHeight;
              }
            }
            lastNewMsgPoll = pollCount;
          }
          
          // Stop polling after grace period following last new message, or after timeout
          pollCount++;
          var graceExpired = gotOutput && (pollCount - lastNewMsgPoll) >= gracePolls;
          if (graceExpired || pollCount >= maxPolls) {
            clearInterval(pollTimer);
            if (!gotOutput) {
              setOutputText('> ' + cmd + '\n\nTimeout: No response received from bonded device');
            }
            setOutputBorder(gotOutput ? '#2ecc71' : '#e74c3c');
            setTimeout(function() { setOutputBorder(''); }, 5000);
            finishRemoteCmd();
          }
        })
        .catch(function(e) { console.error('[Bond] Poll error:', e); });
    }
    
    // Send the command
    hw.postForm('/api/bond/exec', {cmd: cmd})
    .then(function(r){ return r.json(); })
    .then(data => {
      if (!data.success) {
        setOutputText('> ' + cmd + '\n\nError: ' + (data.result || data.error || 'Command failed'));
        setOutputBorder('#e74c3c');
        setTimeout(function() { setOutputBorder(''); }, 5000);
        finishRemoteCmd();
      } else {
        setOutputText('> ' + cmd + '\nCommand sent, waiting for response...');
        // Start polling for messages from bonded peer
        pollTimer = setInterval(pollMessages, 500);
      }
    })
    .catch(e => {
      setOutputText('> ' + cmd + '\n\nError: ' + e.message);
      finishRemoteCmd();
    });
  };
  
  // Bond device selection functions
  window.refreshBondDevices = function() {
    const select = hw.$('bond-device-select');
    if (!select) return;

    const prevValue = select.value;  // preserve the user's selection across a manual refresh
    select.innerHTML = '<option value="">Loading devices...</option>';

    hw.fetchJSON('/api/bond/paired-devices')
      .then(data => {
        if (!data.devices || data.devices.length === 0) {
          select.innerHTML = '<option value="">No paired devices available</option>';
          return;
        }

        select.innerHTML = '<option value="">-- Select a device --</option>';
        data.devices.forEach(function(dev) {
          const label = dev.name + ' (' + dev.mac + ')' +
                       (dev.room ? ' - ' + dev.room + (dev.roomAccepted === false ? ' (not accepted)' : '') : '') +
                       (dev.zone ? '/' + dev.zone : '');
          const option = document.createElement('option');
          option.value = dev.mac;
          option.textContent = label;
          select.appendChild(option);
        });
        // Restore the previous selection if that device is still in the list
        if (prevValue) select.value = prevValue;
      })
      .catch(e => {
        console.error('[Bond] Failed to load devices:', e);
        select.innerHTML = '<option value="">Error loading devices</option>';
      });
  };
  
  window.connectBondDevice = function() {
    const select = hw.$('bond-device-select');
    const statusDiv = hw.$('bond-config-status');
    const btn = hw.$('btn-bond-connect');
    
    if (!select || !statusDiv || !btn) return;
    
    const mac = select.value;
    if (!mac) {
      statusDiv.style.display = 'block';
      statusDiv.style.background = '#fff3cd';
      statusDiv.style.color = '#856404';
      statusDiv.textContent = 'Please select a device first';
      setTimeout(function() { statusDiv.style.display = 'none'; }, 3000);
      return;
    }
    
    btn.disabled = true;
    btn.textContent = 'Connecting...';
    statusDiv.style.display = 'block';
    statusDiv.style.background = 'rgba(255,255,255,0.1)';
    statusDiv.style.color = 'var(--panel-fg)';
    statusDiv.textContent = 'Sending bond connect command...';
    
    hw.postForm('/api/cli', {cmd: 'bondconnect ' + mac})
    .then(function(r){ return r.text(); })
    .then(result => {
      var isError = result.toLowerCase().indexOf('error') !== -1 || result.toLowerCase().indexOf('failed') !== -1;
      if (!isError) {
        statusDiv.style.background = '#d4edda';
        statusDiv.style.color = '#155724';
        statusDiv.textContent = 'Bond connection initiated! Refreshing page...';
        setTimeout(function() {
          window.location.reload();
        }, 2000);
      } else {
        statusDiv.style.background = '#f8d7da';
        statusDiv.style.color = '#721c24';
        statusDiv.textContent = 'Failed to connect: ' + result;
        btn.disabled = false;
        btn.textContent = 'Connect';
      }
    })
    .catch(e => {
      statusDiv.style.background = '#f8d7da';
      statusDiv.style.color = '#721c24';
      statusDiv.textContent = 'Connection error: ' + e.message;
      btn.disabled = false;
      btn.textContent = 'Connect';
    });
  };
  
  // Initial load and auto-refresh
  window.refreshBond();
  refreshInterval = setInterval(window.refreshBond, 5000);
  
  // Cleanup on page unload
  window.addEventListener('beforeunload', function() {
    if (refreshInterval) clearInterval(refreshInterval);
    if (window.__bondProgressTimer) { clearTimeout(window.__bondProgressTimer); window.__bondProgressTimer = null; }
    if (window.__es) { window.__es.close(); window.__es = null; }
  });
})();
</script>
)JS", HTTPD_RESP_USE_STRLEN);
}

static void streamBondContent(httpd_req_t* req, const String& username) {
  streamCardPage(req, "Bonded Device", username, "bond", [&] { streamBondInner(req); });
}

static esp_err_t handleBondPage(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  streamPageWithContent(req, "bond", ctx.user, streamBondContent);
  return ESP_OK;
}

// =============================================================================
// API: Bond Status
// =============================================================================

static esp_err_t handleBondStatus(httpd_req_t* req) {
  WEB_AUTH_JSON_OR_RETURN(req, ctx);

  // Check if ESP-NOW is initialized
  bool espnowEnabled = gEspNow && gEspNow->initialized;
  
  bool bonded = gSettings.bondModeEnabled;
  uint8_t peerMac[6] = {0};
  char macStr[18] = "00:00:00:00:00:00";
  // peerMacBytes() also enforces bondModeEnabled — same precondition as the
  // old inline check, just routed through the facade.
  bool peerConfigured = BondedPeer::peerMacBytes(peerMac);
  if (peerConfigured) {
    macToDisplay(peerMac, macStr, sizeof(macStr));
  }

  // Local STA MAC — exposed so the Files page (master) can ask the bonded peer
  // to send a file back to us: "espnow sendfile <localMac> <path>".
  String localMacStr = SelfDevice::macString();

  // Get peer name: prefer capability cache, fall back to device registry
  String peerNameStr;
  if (gEspNow && gEspNow->lastRemoteCapValid) {
    peerNameStr = String(gEspNow->lastRemoteCap.deviceName);
  }
  if (peerNameStr.length() == 0 && peerConfigured) {
    peerNameStr = getEspNowDeviceName(peerMac);
  }
  if (peerNameStr.length() == 0) {
    peerNameStr = "Unknown";
  }
  
  // Online status and health metrics (use pre-calculated values from EspNowState)
  bool peerOnline = gEspNow ? gEspNow->bondPeerOnline : false;
  unsigned long lastHb = gEspNow ? gEspNow->lastBondHeartbeatReceivedMs : 0;
  uint32_t hbRx = gEspNow ? gEspNow->bondHeartbeatsReceived : 0;
  uint32_t hbTx = gEspNow ? gEspNow->bondHeartbeatsSent : 0;
  
  // Health metrics from EspNowState
  int rssi = gEspNow ? gEspNow->bondRssiAvg : -100;
  int rssiLast = gEspNow ? gEspNow->bondRssiLast : -100;
  uint32_t peerUptime = 0;
  
  if (peerOnline && gEspNow) {
    peerUptime = gEspNow->bondPeerUptime;
  }
  
  uint32_t lastHeartbeatAgeSec = 0;
  if (peerOnline && lastHb > 0) {
    unsigned long now = millis();
    if (now >= lastHb) {
      lastHeartbeatAgeSec = (uint32_t)((now - lastHb) / 1000UL);
    }
  }
  
  // Stream JSON response
  webBondSendChunk(req, "{");
  webBondSendChunkf(req, "\"espnowEnabled\":%s,", espnowEnabled ? "true" : "false");
  webBondSendChunkf(req, "\"bonded\":%s,", bonded ? "true" : "false");
  webBondSendChunkf(req, "\"peerConfigured\":%s,", peerConfigured ? "true" : "false");
  webBondSendChunkf(req, "\"peerOnline\":%s,", peerOnline ? "true" : "false");
  // What the partner said about its side of the bond: "unknown" (no answer
  // yet), "agreed" or "declined", with the reason for a decline or a role
  // conflict. The page explains a declined bond instead of waiting on it.
  {
    const BondAgreementView agreement = bondAgreementView();
    webBondSendChunkf(req, "\"agreement\":\"%s\",", bondAgreementKey(agreement));
    webBondSendChunkf(req, "\"agreementReason\":\"%s\",", bondAgreementReasonKey(agreement));
  }
#if ENABLE_BONDED_MODE
  // Whether the partner's data is forwarded to the mesh master (the "forward"
  // object of 'bondstatus json').
  {
    EXT_RAM_BSS_ATTR static char forward[espnowfwd::kStatusJsonCap];  // PSRAM: the longest forward object
    if (bondForwardStatusJson(forward, sizeof(forward)) > 0) {
      webBondSendChunk(req, "\"forward\":");
      webBondSendChunk(req, forward);
      webBondSendChunk(req, ",");
    } else {
      webBondSendChunk(req, "\"forward\":null,");
    }
  }
#endif
  webBondSendChunkf(req, "\"peerMac\":\"%s\",", macStr);
  webBondSendChunkf(req, "\"localMac\":\"%s\",", localMacStr.c_str());
  // Free text from the peer or the pairing registry: JSON-escaped.
  {
    String peerNameField = "\"peerName\":";
    peerNameField += jsonQuote(peerNameStr);
    peerNameField += ",";
    webBondSendChunk(req, peerNameField.c_str());
  }
  webBondSendChunkf(req, "\"role\":%d,", gSettings.bondRole);
  // The mask bit of each sensor type, by name, from the one sensor table: the
  // page takes every sensor mask from here and types no number of its own.
  webBondSendChunk(req, "\"sensorBits\":{");
  for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
    webBondSendChunkf(req, "%s\"%s\":%lu", i ? "," : "", espnowsensor::kSensorTable[i].name,
                      (unsigned long)espnowsensor::sensorBit((uint8_t)i));
  }
  webBondSendChunk(req, "},");
  webBondSendChunkf(req, "\"lastHeartbeat\":%lu,", lastHb);
  webBondSendChunkf(req, "\"lastHeartbeatAgeSec\":%lu,", (unsigned long)lastHeartbeatAgeSec);
  webBondSendChunkf(req, "\"heartbeatsRx\":%lu,", (unsigned long)hbRx);
  webBondSendChunkf(req, "\"heartbeatsTx\":%lu,", (unsigned long)hbTx);
  webBondSendChunkf(req, "\"rssi\":%d,", rssi);
  webBondSendChunkf(req, "\"rssiLast\":%d,", rssiLast);
  webBondSendChunkf(req, "\"peerUptime\":%lu,", (unsigned long)peerUptime);

  // peerSettingsHash = CRC32 the peer reported in its most recent heartbeat,
  // matching CRC32(generateDeviceSettings()) on their side. The bonded
  // settings panel captures this value at form-load time as formLoadedHash;
  // subsequent polls compare against it to detect that the worker has
  // changed settings since the user loaded the form.
  uint32_t peerSettingsHash = gEspNow ? gEspNow->bondPeerSettingsHash : 0;
  webBondSendChunkf(req, "\"peerSettingsHash\":%lu,", (unsigned long)peerSettingsHash);

  // Debug fields for diagnosing bond sync issues
  webBondSendChunkf(req, "\"_dbg_synced\":%s,", isBondSynced() ? "true" : "false");
  webBondSendChunkf(req, "\"_dbg_capValid\":%s,", (gEspNow && gEspNow->lastRemoteCapValid) ? "true" : "false");
  webBondSendChunkf(req, "\"_dbg_capSent\":%s,", (gEspNow && gEspNow->bondCapSent) ? "true" : "false");
  webBondSendChunkf(req, "\"_dbg_statusValid\":%s,", (gEspNow && gEspNow->bondPeerStatusValid) ? "true" : "false");
  
  // Streaming settings
  webBondSendChunkf(req, "\"streamThermal\":%s,", gSettings.bondStreamThermal ? "true" : "false");
  webBondSendChunkf(req, "\"streamTof\":%s,", gSettings.bondStreamTof ? "true" : "false");
  webBondSendChunkf(req, "\"streamImu\":%s,", gSettings.bondStreamImu ? "true" : "false");
  webBondSendChunkf(req, "\"streamGps\":%s,", gSettings.bondStreamGps ? "true" : "false");
  webBondSendChunkf(req, "\"streamInput\":%s,", gSettings.bondStreamInput ? "true" : "false");
  webBondSendChunkf(req, "\"streamFmradio\":%s,", gSettings.bondStreamFmradio ? "true" : "false");
  webBondSendChunkf(req, "\"streamRtc\":%s,", gSettings.bondStreamRtc ? "true" : "false");
  webBondSendChunkf(req, "\"streamPresence\":%s,", gSettings.bondStreamPresence ? "true" : "false");
  
  // Local device capabilities (compile-time)
  {
    uint32_t localFeatures = 0;
#if ENABLE_WIFI
    localFeatures |= CAP_FEATURE_WIFI;
#endif
#if ENABLE_BLUETOOTH
    localFeatures |= CAP_FEATURE_BLUETOOTH;
#endif
#if ENABLE_MQTT
    localFeatures |= CAP_FEATURE_MQTT;
#endif
#if ENABLE_CAMERA_SENSOR
    localFeatures |= CAP_FEATURE_CAMERA;
#endif
#if ENABLE_MICROPHONE
    localFeatures |= CAP_FEATURE_MICROPHONE;
#endif
#if ENABLE_ESP_SR
    localFeatures |= CAP_FEATURE_ESP_SR;
#endif
#if ENABLE_AUTOMATION
    localFeatures |= CAP_FEATURE_AUTOMATION;
#endif
#if ENABLE_OLED_DISPLAY
    localFeatures |= CAP_FEATURE_OLED;
#endif
#if ENABLE_ESPNOW
    localFeatures |= CAP_FEATURE_ESPNOW;
#endif
    uint32_t localSensors = 0;
#if ENABLE_THERMAL_SENSOR
    localSensors |= CAP_SENSOR_THERMAL;
#endif
#if ENABLE_TOF_SENSOR
    localSensors |= CAP_SENSOR_TOF;
#endif
#if ENABLE_IMU_SENSOR
    localSensors |= CAP_SENSOR_IMU;
#endif
#if ENABLE_OLED_INPUT  // either gamepad or ANO encoder → device has input capability
    localSensors |= CAP_SENSOR_INPUT;
#endif
#if ENABLE_GPS_SENSOR
    localSensors |= CAP_SENSOR_GPS;
#endif
#if ENABLE_APDS_SENSOR
    localSensors |= CAP_SENSOR_APDS;
#endif
#if ENABLE_RTC_SENSOR
    localSensors |= CAP_SENSOR_RTC;
#endif
#if ENABLE_PRESENCE_SENSOR
    localSensors |= CAP_SENSOR_PRESENCE;
#endif
    String lFeatures = getCapabilityListLong(localFeatures, FEATURE_NAMES);
    String lSensors = sensorMaskNames(localSensors, /*longForm=*/true);
    webBondSendChunk(req, "\"localCapabilities\":{");
    webBondSendChunkf(req, "\"features\":\"%s\",", lFeatures.c_str());
    webBondSendChunkf(req, "\"sensors\":\"%s\",", lSensors.c_str());
    webBondSendChunkf(req, "\"featureMask\":%lu,", (unsigned long)localFeatures);
    webBondSendChunkf(req, "\"sensorMask\":%lu,", (unsigned long)localSensors);
    webBondSendChunkf(req, "\"freeHeap\":%lu,", (unsigned long)hw1InternalFreeBytes());
    webBondSendChunkf(req, "\"minFreeHeap\":%lu,", (unsigned long)hw1InternalMinFreeBytes());
    webBondSendChunkf(req, "\"flashMB\":%lu,", (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)));
    uint32_t psramBytes = ESP.getPsramSize();
    webBondSendChunkf(req, "\"psramMB\":%lu,", (unsigned long)((psramBytes + 524288) / (1024 * 1024)));
    webBondSendChunkf(req, "\"psramKB\":%lu,", (unsigned long)(psramBytes / 1024));
    // Local sensor connected/enabled status. Reuse the SAME authoritative
    // builder that produces the outgoing BOND_STATUS_RESP, so a device's own
    // "This Device" view matches exactly how its bonded peer sees it. The old
    // hand-rolled mask here omitted RTC/APDS/FM Radio, so locally-present
    // sensors (notably RTC) wrongly showed OFF on this device even though the
    // peer correctly reported them ON from the same physical state.
    BondPeerStatus localStatus;
    buildLocalBondStatus(localStatus);
    webBondSendChunkf(req, "\"sensorConnectedMask\":%u,", (unsigned)localStatus.sensorConnectedMask);
    webBondSendChunkf(req, "\"sensorEnabledMask\":%u,", (unsigned)localStatus.sensorEnabledMask);
    // Local input device type (compile-time): 0=none, 1=Gamepad, 2=ANO Encoder.
    // Drives the I2C-sensor row label and the Remote Sensors row label on the
    // bonded peer (so they show "Gamepad"/"ANO" instead of generic "Input").
    webBondSendChunkf(req, "\"inputDeviceType\":%d", (int)INPUT_DEVICE_TYPE);
    webBondSendChunk(req, "},");
  }
  
  // Remote capabilities (if available)
  if (gEspNow && gEspNow->lastRemoteCapValid) {
    CapabilitySummary& cap = gEspNow->lastRemoteCap;
    String features = getCapabilityListLong(cap.featureMask, FEATURE_NAMES);
    String sensors = sensorMaskNames(cap.sensorMask, /*longForm=*/true);
    String services = getCapabilityListLong(cap.serviceMask, SERVICE_NAMES);
    
    webBondSendChunk(req, "\"capabilities\":{");
    webBondSendChunkf(req, "\"features\":\"%s\",", features.c_str());
    webBondSendChunkf(req, "\"sensors\":\"%s\",", sensors.c_str());
    webBondSendChunkf(req, "\"services\":\"%s\",", services.c_str());
    webBondSendChunkf(req, "\"flashMB\":%lu,", (unsigned long)cap.flashSizeMb);
    webBondSendChunkf(req, "\"psramMB\":%lu,", (unsigned long)cap.psramSizeMb);
    
    // Add individual sensor/feature masks for UI logic
    webBondSendChunkf(req, "\"featureMask\":%lu,", (unsigned long)cap.featureMask);
    webBondSendChunkf(req, "\"sensorMask\":%lu,", (unsigned long)cap.sensorMask);
    webBondSendChunkf(req, "\"serviceMask\":%lu,", (unsigned long)cap.serviceMask);
    // Peer's input device type: 0=none/legacy, 1=Gamepad, 2=ANO Encoder.
    // Came across in CapabilitySummary.inputDeviceType (was the reserved1
    // byte pre-v0.94). Lets the Remote Sensors row label specifically what
    // the bonded device is running, even if local and remote differ.
    webBondSendChunkf(req, "\"inputDeviceType\":%d", (int)cap.inputDeviceType);
    webBondSendChunk(req, "},");
    
    // Sensor connectivity from live BondPeerStatus cache (updated every ~30s)
    // Falls back to compiled sensorMask from capabilities if no live status yet
    bool hasLiveStatus = gEspNow->bondPeerStatusValid;
    uint32_t enabledMask = hasLiveStatus ? gEspNow->bondPeerStatus.sensorEnabledMask : 0;
    uint32_t connectedMask = hasLiveStatus ? gEspNow->bondPeerStatus.sensorConnectedMask : 0;
    
    // sensorConnected: per-sensor booleans for UI rendering
    //   valid       = have we received at least one live status from the peer?
    //   <sensor>    = sensor currently connected (I2C task running)
    //   <sensor>On  = sensor task currently enabled
    webBondSendChunk(req, "\"sensorConnected\":{");
    webBondSendChunkf(req, "\"valid\":%s,", hasLiveStatus ? "true" : "false");
    webBondSendChunkf(req, "\"thermal\":%s,", (connectedMask & CAP_SENSOR_THERMAL) ? "true" : "false");
    webBondSendChunkf(req, "\"tof\":%s,", (connectedMask & CAP_SENSOR_TOF) ? "true" : "false");
    webBondSendChunkf(req, "\"imu\":%s,", (connectedMask & CAP_SENSOR_IMU) ? "true" : "false");
    webBondSendChunkf(req, "\"gps\":%s,", (connectedMask & CAP_SENSOR_GPS) ? "true" : "false");
    webBondSendChunkf(req, "\"input\":%s,", (connectedMask & CAP_SENSOR_INPUT) ? "true" : "false");
    webBondSendChunkf(req, "\"apds\":%s,", (connectedMask & CAP_SENSOR_APDS) ? "true" : "false");
    webBondSendChunkf(req, "\"fmradio\":%s,", (connectedMask & CAP_SENSOR_FMRADIO) ? "true" : "false");
    webBondSendChunkf(req, "\"presence\":%s,", (connectedMask & CAP_SENSOR_PRESENCE) ? "true" : "false");
    webBondSendChunkf(req, "\"rtc\":%s,", (connectedMask & CAP_SENSOR_RTC) ? "true" : "false");
    // Per-sensor enabled (running) state from live status
    webBondSendChunkf(req, "\"thermalOn\":%s,", (enabledMask & CAP_SENSOR_THERMAL) ? "true" : "false");
    webBondSendChunkf(req, "\"tofOn\":%s,", (enabledMask & CAP_SENSOR_TOF) ? "true" : "false");
    webBondSendChunkf(req, "\"imuOn\":%s,", (enabledMask & CAP_SENSOR_IMU) ? "true" : "false");
    webBondSendChunkf(req, "\"gpsOn\":%s,", (enabledMask & CAP_SENSOR_GPS) ? "true" : "false");
    webBondSendChunkf(req, "\"inputOn\":%s,", (enabledMask & CAP_SENSOR_INPUT) ? "true" : "false");
    webBondSendChunkf(req, "\"presenceOn\":%s,", (enabledMask & CAP_SENSOR_PRESENCE) ? "true" : "false");
    webBondSendChunkf(req, "\"rtcOn\":%s,", (enabledMask & CAP_SENSOR_RTC) ? "true" : "false");
    webBondSendChunkf(req, "\"apdsOn\":%s,", (enabledMask & CAP_SENSOR_APDS) ? "true" : "false");
    webBondSendChunkf(req, "\"fmradioOn\":%s", (enabledMask & CAP_SENSOR_FMRADIO) ? "true" : "false");
    webBondSendChunk(req, "},");
    
    // Live peer status (from periodic ~30s poll)
    webBondSendChunkf(req, "\"peerStatus\":{\"valid\":%s,", hasLiveStatus ? "true" : "false");
    if (hasLiveStatus) {
      webBondSendChunkf(req, "\"sensorEnabled\":%u,", enabledMask);
      webBondSendChunkf(req, "\"sensorConnected\":%u,", connectedMask);
      webBondSendChunkf(req, "\"freeHeap\":%lu,", (unsigned long)gEspNow->bondPeerStatus.freeHeap);
      webBondSendChunkf(req, "\"minFreeHeap\":%lu,", (unsigned long)gEspNow->bondPeerStatus.minFreeHeap);
      webBondSendChunkf(req, "\"wifiConnected\":%s,", gEspNow->bondPeerStatus.wifiConnected ? "true" : "false");
      webBondSendChunkf(req, "\"bluetoothActive\":%s,", gEspNow->bondPeerStatus.bluetoothActive ? "true" : "false");
      webBondSendChunkf(req, "\"httpActive\":%s,", gEspNow->bondPeerStatus.httpActive ? "true" : "false");
      unsigned long ageMs = millis() - gEspNow->bondPeerStatusTimeMs;
      webBondSendChunkf(req, "\"ageSec\":%lu", (unsigned long)(ageMs / 1000));
    } else {
      webBondSendChunk(req, "\"ageSec\":0");
    }
    webBondSendChunk(req, "}");
  } else {
    webBondSendChunk(req, "\"capabilities\":null,\"sensorConnected\":null,");
    // Still send peerStatus even without cap exchange — periodic poll is independent
    bool hasLiveStatus = gEspNow && gEspNow->bondPeerStatusValid;
    webBondSendChunkf(req, "\"peerStatus\":{\"valid\":%s", hasLiveStatus ? "true" : "false");
    if (hasLiveStatus) {
      webBondSendChunkf(req, ",\"freeHeap\":%lu", (unsigned long)gEspNow->bondPeerStatus.freeHeap);
      webBondSendChunkf(req, ",\"minFreeHeap\":%lu", (unsigned long)gEspNow->bondPeerStatus.minFreeHeap);
      webBondSendChunkf(req, ",\"wifiConnected\":%s", gEspNow->bondPeerStatus.wifiConnected ? "true" : "false");
      unsigned long ageMs = millis() - gEspNow->bondPeerStatusTimeMs;
      webBondSendChunkf(req, ",\"ageSec\":%lu", (unsigned long)(ageMs / 1000));
    }
    webBondSendChunk(req, "}");
  }
  
  webBondSendChunk(req, "}");
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// =============================================================================
// API: Bond Stream Control
// =============================================================================

static esp_err_t handleBondStream(httpd_req_t* req) {
  WEB_AUTH_JSON_OR_RETURN(req, ctx);

  // Guard: only allow streaming control when fully synced
  if (!isBondSynced()) {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Bond not synced\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Parse POST body
  char buf[128];
  size_t len = 0;
  if (webRecvRequestBody(req, buf, sizeof(buf), &len) != WebBodyStatus::Ok) {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"No data\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Parse sensor and action
  char sensorParam[32] = {0};
  char actionParam[16] = {0};

  char* sensorStart = strstr(buf, "sensor=");
  if (sensorStart) {
    sensorStart += 7;
    char* sensorEnd = strchr(sensorStart, '&');
    if (!sensorEnd) sensorEnd = buf + len;
    size_t slen = sensorEnd - sensorStart;
    if (slen >= sizeof(sensorParam)) slen = sizeof(sensorParam) - 1;
    strncpy(sensorParam, sensorStart, slen);
  }

  char* actionStart = strstr(buf, "action=");
  if (actionStart) {
    actionStart += 7;
    char* actionEnd = strchr(actionStart, '&');
    if (!actionEnd) actionEnd = buf + len;
    size_t alen = actionEnd - actionStart;
    if (alen >= sizeof(actionParam)) alen = sizeof(actionParam) - 1;
    strncpy(actionParam, actionStart, alen);
  }

  if (strlen(sensorParam) == 0) {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Missing sensor parameter\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Resolve toggle → on/off using current setting state
  const char* onOff = nullptr;
  if (strcmp(actionParam, "on") == 0) {
    onOff = "on";
  } else if (strcmp(actionParam, "off") == 0) {
    onOff = "off";
  } else {
    // toggle: look up current state from gSettings
    RemoteSensorType sensorType = REMOTE_SENSOR_THERMAL;
    const bool* current = sensorTypeFromString(sensorParam, sensorType)
        ? bondStreamSetting(sensorType) : nullptr;
    if (!current) {
      httpd_resp_send(req, "{\"success\":false,\"error\":\"Unknown sensor\"}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    onOff = *current ? "off" : "on";
  }

  // Route through unified command: "bond stream <sensor> <on|off>"
  String cmdOut;
  String cmd = String("bondstream ") + sensorParam + " " + onOff;
  bool ok = executeUnifiedWebCommand(req, ctx, cmd, cmdOut);

  if (ok) {
    bool newState = (strcmp(onOff, "on") == 0);
    webBondSendChunkf(req, "{\"success\":true,\"sensor\":\"%s\",\"enabled\":%s}",
                      sensorParam, newState ? "true" : "false");
  } else {
    // The command's own reason: an admin refusal, bond not ready, unknown sensor.
    String body = "{\"success\":false,\"error\":";
    body += jsonQuote(cmdOut.length() ? commandReplyReason(cmdOut.c_str())
                                      : "Stream command failed");
    body += "}";
    webBondSendChunk(req, body.c_str());
  }
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// =============================================================================
// API: Bond Command Execution
// =============================================================================

static esp_err_t handleBondExec(httpd_req_t* req) {
  WEB_AUTH_JSON_OR_RETURN(req, ctx);
  
  // Parse POST body. Whole command line or nothing: the bonded link rejects
  // what it cannot carry, and the web side must not silently shorten it first.
  EXT_RAM_BSS_ATTR static char buf[CMD_RESULT_MAX];
  if (req->content_len >= sizeof(buf)) {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Request too large\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  if (webRecvRequestBody(req, buf, sizeof(buf)) != WebBodyStatus::Ok) {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"No data\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  // Parse command
  EXT_RAM_BSS_ATTR static char cmdParam[CMD_INPUT_MAX + 1];
  cmdParam[0] = '\0';
  char* cmdStart = strstr(buf, "cmd=");
  if (cmdStart) {
    cmdStart += 4;
    // URL decode (basic)
    char* out = cmdParam;
    char* end = cmdParam + sizeof(cmdParam) - 1;
    while (*cmdStart && *cmdStart != '&' && out < end) {
      if (*cmdStart == '+') {
        *out++ = ' ';
        cmdStart++;
      } else if (*cmdStart == '%' && cmdStart[1] && cmdStart[2]) {
        char hex[3] = {cmdStart[1], cmdStart[2], 0};
        *out++ = (char)strtol(hex, NULL, 16);
        cmdStart += 3;
      } else {
        *out++ = *cmdStart++;
      }
    }
    *out = '\0';
    if (*cmdStart && *cmdStart != '&') {
      httpd_resp_send(req, "{\"success\":false,\"error\":\"Command too long\"}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
  }
  
  if (strlen(cmdParam) == 0) {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Missing command\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  // Use unified remote command routing - prefix with "remote:"
  // executeUnifiedWebCommand() sets context for proper serial suppression
  String remoteCmd = "remote:";
  remoteCmd += cmdParam;
  
  String resultStr;
  bool success = executeUnifiedWebCommand(req, ctx, remoteCmd, resultStr);
  
  webBondSendChunkf(req, "{\"success\":%s,\"result\":", success ? "true" : "false");
  
  // Escape result for JSON
  webBondSendChunk(req, "\"");
  for (size_t i = 0; i < resultStr.length(); i++) {
    char ch = resultStr[i];
    if (ch == '"') webBondSendChunk(req, "\\\"");
    else if (ch == '\\') webBondSendChunk(req, "\\\\");
    else if (ch == '\n') webBondSendChunk(req, "\\n");
    else if (ch == '\r') webBondSendChunk(req, "\\r");
    else if (ch == '\t') webBondSendChunk(req, "\\t");
    else {
      char c[2] = {ch, 0};
      webBondSendChunk(req, c);
    }
  }
  // "success" of a sent command says only that the frame left. The request id
  // lets the caller match the bonded device's answer in the message history.
  uint32_t reqId = 0;
  if (success && parseRemoteCommandSentReqId(resultStr.c_str(), &reqId)) {
    webBondSendChunkf(req, "\",\"pending\":true,\"reqId\":%lu}", (unsigned long)reqId);
  } else {
    webBondSendChunk(req, "\"}");
  }
  
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// =============================================================================
// API: Bond Role Swap
// =============================================================================

static esp_err_t handleBondRole(httpd_req_t* req) {
  WEB_AUTH_JSON_OR_RETURN(req, ctx);

  if (!gSettings.bondModeEnabled) {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Bond mode not enabled\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  // Determine new roles
  const char* localNewRole = (isBondMaster()) ? "worker" : "master";
  const char* peerNewRole = (isBondMaster()) ? "master" : "worker";
  
  // IMPORTANT: Send remote role change FIRST so the peer processes its new role
  // before the local device starts the handshake. Reversing this order caused a
  // race condition where the local worker sent CAP_REQ before the peer became master,
  // and resetBondHandshake() on the peer then cleared the deferred flags.
  String remoteCmd = "remote:bondrole ";
  remoteCmd += peerNewRole;
  String remoteResult;
  bool remoteOk = executeUnifiedWebCommand(req, ctx, remoteCmd, remoteResult);
  
  if (!remoteOk) {
    // Abort — don't change local role if peer didn't change, or we get split-brain
    String body = "{\"success\":false,\"error\":";
    body += jsonQuote(String("Remote role change failed: ") + remoteResult);
    body += "}";
    webBondSendChunk(req, body.c_str());
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
  }
  
  // Change local role (handles handshake reset + cap invalidation)
  String localCmd = "bondrole ";
  localCmd += localNewRole;
  String localResult;
  if (!executeUnifiedWebCommand(req, ctx, localCmd, localResult)) {
    // The bonded device was already sent its new role: say which side is left.
    String error = "The bonded device was sent its new role, but this device did not change its own";
    if (localResult.length()) {
      error += ": ";
      error += commandReplyReason(localResult.c_str());
    }
    if (!error.endsWith(".")) error += ".";
    error += " Run bondrole ";
    error += localNewRole;
    error += " here.";
    String body = "{\"success\":false,\"error\":";
    body += jsonQuote(error);
    body += "}";
    webBondSendChunk(req, body.c_str());
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
  }
  
  uint8_t newRole = gSettings.bondRole;
  const char* roleName = (newRole == 1) ? "master" : "worker";
  webBondSendChunkf(req, "{\"success\":true,\"role\":%d,\"roleName\":\"%s\"}", newRole, roleName);
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// =============================================================================
// API: Bond CLI Batch — run multiple commands on the bonded peer in sequence
// =============================================================================
//
// Mirrors /api/cli/batch but routes each command through the bond session by
// prefixing with "remote:" before handing it to executeUnifiedWebCommand. Body
// is JSON: {"commands":["beginwrite","tz 480","savesettings"]}. Response is
// {"ok":true,"count":N,"results":[...],"failed":[i,...]} with one result per
// command (same order) and, as /api/cli/batch does, the results[] indexes of the
// commands that failed here: not delivered, or answered with a failure reply
// (commandReplyIsFailure). A delivered command's own verdict arrives later from
// the peer, so it is not in failed[]. In the interactive form slot 1 (the
// forwarded confirmation) is listed on the same rule. Used by the bonded-device
// settings panel (sendSequential) and hw.cliConfirm.

static esp_err_t handleBondCliBatch(httpd_req_t* req) {
  WEB_AUTH_JSON_OR_RETURN(req, ctx);

  // Keep one immutable cookie generation for the whole request. The generic
  // command facade fences each individual command, but a batch also owns all
  // earlier accumulated results and must revalidate immediately before its
  // final HTTP response.
  const TransportSessionEpoch requestSessionEpoch =
      captureTransportSessionEpoch(ctx);
  auto requestSessionLive = [&]() {
    return !ctx.sid.length() ||
           (requestSessionEpoch != 0 &&
            transportSessionEpochIsLive(SOURCE_WEB, requestSessionEpoch));
  };
  if (!requestSessionLive()) {
    webSendSessionChanged(req);
    return ESP_OK;
  }

  if (!gSettings.bondModeEnabled || !isBondMaster()) {
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"Bonded master required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  WebBodyBuffer body = webReadRequestBody(req, 32768, "http.bond.batch");
  if (!body.ok()) {
    const char* reply = "{\"ok\":false,\"error\":\"Request body incomplete\"}";
    if (body.status == WebBodyStatus::Empty || body.status == WebBodyStatus::TooLarge) {
      reply = "{\"ok\":false,\"error\":\"Invalid content length\"}";
    } else if (body.status == WebBodyStatus::NoMemory) {
      reply = "{\"ok\":false,\"error\":\"OOM\"}";
    }
    httpd_resp_send(req, reply, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  PSRAM_JSON_DOC(doc);
  DeserializationError jerr = deserializeJson(doc, body.c_str(), body.len);
  if (jerr || !doc["commands"].is<JsonArray>()) {
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"Expected {\\\"commands\\\":[...]} JSON body\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  JsonArray commands = doc["commands"].as<JsonArray>();
  const bool interactive = doc["interactive"] | false;
  // ArduinoJson owns reply text in PSRAM; do not retain a second String list.
  PSRAM_JSON_DOC(respDoc);
  respDoc["ok"] = false;
  respDoc["count"] = 0;
  JsonArray results = respDoc["results"].to<JsonArray>();
  JsonArray failed = respDoc["failed"].to<JsonArray>();
  bool resultsBuffered = !results.isNull() && !failed.isNull() && !respDoc.overflowed();
  int count = 0;
  // Lists the result just added as failed when it was not delivered or its
  // reply is a failure.
  auto noteFailed = [&](bool ok, const String& out) {
    if (!resultsBuffered || (ok && !commandReplyIsFailure(out.c_str()))) return;
    if (!failed.add(static_cast<uint32_t>(results.size() - 1))) resultsBuffered = false;
  };

  if (interactive) {
    // A CliMode is deliberately local to one transport-session generation;
    // it cannot span web -> ESP-NOW -> peer. The browser has already obtained
    // an explicit human confirmation, so translate only the destructive verbs
    // with supported one-shot forms. Preserve the two-result shape expected by
    // hw.cliConfirm: slot 0 is the local prompt phase and slot 1 is forwarding.
    String answer = commands.size() == 2 ? commands[1].as<String>() : String();
    answer.trim();
    answer.toLowerCase();
    String cmd = commands.size() > 0 ? commands[0].as<String>() : String();
    cmd.trim();
    String lower = cmd;
    lower.toLowerCase();
    const bool supported =
        lower.startsWith("filedelete ") || lower.startsWith("userdelete ");
    if (commands.size() != 2 || answer != "yes" || !supported) {
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_send(
          req,
          "{\"ok\":false,\"error\":\"bond interactive confirmation supports only filedelete/userdelete [command, yes]\"}",
          HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    if (!lower.endsWith(" confirm")) cmd += " confirm";

    String remoteCmd = "remote:";
    remoteCmd += cmd;
    String out;
    const bool ok = executeUnifiedWebCommand(req, ctx, remoteCmd, out);
    if (!requestSessionLive()) {
      results.clear();
      failed.clear();
      webSendSessionChanged(req);
      return ESP_OK;
    }
    if (!ok && out.length() == 0) out = "Error: command delivery failed";
    uint32_t sentReqId = 0;
    if (ok && parseRemoteCommandSentReqId(out.c_str(), &sentReqId)) {
      out = "Pending: remote deletion accepted for delivery; verify the bonded device state.";
    }
    if (resultsBuffered && !results.add("OK: confirmed in the local browser")) {
      resultsBuffered = false;
    }
    if (resultsBuffered && !results.add(out)) resultsBuffered = false;
    noteFailed(ok, out);
    count = 2;
  } else {
    for (JsonVariant v : commands) {
      if (!requestSessionLive()) {
        results.clear();
        failed.clear();
        break;
      }
      String cmd = v.as<String>();
      cmd.trim();
      if (cmd.length() == 0) {
        if (resultsBuffered && !results.add("")) resultsBuffered = false;
        continue;
      }

      // Route through the bond session — same "remote:" prefix that
      // executeUnifiedWebCommand uses in handleBondExec / handleBondRole.
      String remoteCmd = "remote:";
      remoteCmd += cmd;
      String out;
      bool ok = executeUnifiedWebCommand(req, ctx, remoteCmd, out);
      if (!requestSessionLive()) {
        results.clear();
        failed.clear();
        break;
      }
      if (!ok && out.length() == 0) {
        out = "Error: Command was not sent to the bonded device. Retry.";
      }
      // Keep execution order unchanged even if response buffering fails.
      if (resultsBuffered && !results.add(out)) resultsBuffered = false;
      noteFailed(ok, out);
      count++;

      if (server == NULL) break;
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }

  if (server == NULL) return ESP_OK;

  if (!requestSessionLive()) {
    results.clear();
    failed.clear();
    webSendSessionChanged(req);
    return ESP_OK;
  }

  respDoc["ok"] = true;
  respDoc["count"] = count;
  if (!resultsBuffered || respDoc.overflowed()) {
    sendJsonStatus(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"batch_response_oom\"}");
    return ESP_OK;
  }
  String respStr;
  serializeJson(respDoc, respStr);
  if (!requestSessionLive()) {
    respStr = String();
    webSendSessionChanged(req);
    return ESP_OK;
  }
  httpd_resp_sendstr(req, respStr.c_str());
  return ESP_OK;
}

// =============================================================================
// API: Bond Settings Sync — force a fresh resync from the worker
// =============================================================================
//
// Resets bondSettingsReceived so the sync tick will re-request the worker's
// settings.json. Polls bondSettingsReceived for up to ~6s (sync tick fires
// roughly every 1s + the file transfer takes ~1s for a typical settings
// payload). Returns {"ok":true,"elapsedMs":N} on success, or
// {"ok":false,"error":"..."} on timeout / preconditions. The client should
// call this before reading the cache so the editor never starts on stale data.

static esp_err_t handleBondSettingsSync(httpd_req_t* req) {
  WEB_AUTH_JSON_OR_RETURN(req, ctx);

  uint32_t elapsedMs = 0;
  if (!BondedPeer::requestSettingsSync(6000, &elapsedMs)) {
    char body[160];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", BondedPeer::lastError());
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Return the CRC32 of the bytes we just cached. The client uses this as
  // formLoadedHash — subsequent polls of /api/bond/status compare the peer's
  // live hash to this value to detect that the worker has changed settings
  // since the form was populated.
  char body[128];
  snprintf(body, sizeof(body), "{\"ok\":true,\"elapsedMs\":%u,\"peerSettingsHash\":%lu}",
           (unsigned)elapsedMs, (unsigned long)BondedPeer::cachedSettingsHash());
  httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// =============================================================================
// API: Bond Settings Schema — serve cached schema + trigger schema sync
// =============================================================================
//
// Schema transport mirrors the settings transport (see handleBondSettings /
// handleBondSettingsSync): the worker writes its schema JSON to a temp file
// under /system/espnow/this_device/_schema_out.json, ships it via the V4
// file pipeline, master receives in v4h_file_end → processBondSchema which
// caches it at /system/espnow/peers/<MAC>/schema.json and sets
// gEspNow->bondSchemaReceived.
//
// Why a file pipeline instead of a sync command response: the schema is
// ~8 KB and the unified-command capture buffer caps at 4 KB. The file
// pipeline chunks transparently. Same reason settings.json moves this way.

// GET /api/bond/settings/schema — the cached schema the worker emitted,
// narrowed to what a bonded settings editor may show (the remote-edit policy
// BondedPeer owns; the OLED editor applies the same one). Callers should hit
// /api/bond/settings/schema/sync first if the cache may be stale.
static esp_err_t handleBondSettingsSchema(httpd_req_t* req) {
  WEB_AUTH_JSON_OR_RETURN(req, ctx);

  char* schema = nullptr;
  size_t schemaLen = 0;
  if (!BondedPeer::readRemoteEditableSchemaJson(&schema, &schemaLen)) {
    char body[160];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", BondedPeer::lastError());
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  httpd_resp_send(req, schema, schemaLen);
  free(schema);
  return ESP_OK;
}

// POST /api/bond/settings/schema/sync — clear bondSchemaReceived, send
// SCHEMA_REQ over bond, poll for completion. Mirrors handleBondSettingsSync
// exactly, just with a different opcode + completion flag. ~6s timeout
// covers the ~1–2s typical file transfer plus retry margin.
static esp_err_t handleBondSettingsSchemaSync(httpd_req_t* req) {
  WEB_AUTH_JSON_OR_RETURN(req, ctx);

  uint32_t elapsedMs = 0;
  if (!BondedPeer::requestSchemaSync(6000, &elapsedMs)) {
    char body[160];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", BondedPeer::lastError());
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  char body[96];
  snprintf(body, sizeof(body), "{\"ok\":true,\"elapsedMs\":%u}", (unsigned)elapsedMs);
  httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// =============================================================================
// API: Bond Settings — serve the cached worker settings.json
// =============================================================================
//
// processBondSettings() writes the worker's settings.json to
// /system/espnow/peers/<MAC>/settings.json on every successful sync. This
// endpoint hands it back wrapped in {settings:{...}} to match the local
// /api/settings shape, so SchemaPanel can use the same response handling
// for both targets. Callers should hit /api/bond/settings/sync first if
// they want a freshly-pulled view; this endpoint is just the on-disk read.

static esp_err_t handleBondSettings(httpd_req_t* req) {
  WEB_AUTH_JSON_OR_RETURN(req, ctx);

  String cached = BondedPeer::readCachedSettingsJson();
  if (cached.length() == 0) {
    char body[160];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", BondedPeer::lastError());
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Wrap raw worker settings JSON in {"settings":{...}} so the response
  // mirrors /api/settings. Chunked so we don't have to grow a big String
  // just to prepend three characters.
  webBondSendChunk(req, "{\"settings\":");
  webBondSendChunk(req, cached.c_str());
  webBondSendChunk(req, "}");
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// =============================================================================
// API: Get Paired Devices
// =============================================================================

static esp_err_t handleBondPairedDevices(httpd_req_t* req) {
  WEB_AUTH_JSON_OR_RETURN(req, ctx);

  if (!gEspNow || gEspNow->deviceCount == 0) {
    httpd_resp_send(req, "{\"devices\":[]}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  webBondSendChunk(req, "{\"devices\":[");
  
  bool first = true;
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    EspNowDevice& dev = gEspNow->devices[i];
    if (isSelfMac(dev.mac)) continue;
    
    String macStr = macToDisplayStr(dev.mac);
    
    if (!first) webBondSendChunk(req, ",");
    first = false;

    // Keep this helper generic across Arduino String and the allocation-free
    // EspNowInlineText registry fields, and never pass a null C string to %s.
    auto sz = [](const auto& s) -> const char* { const char* p = s.c_str(); return p ? p : ""; };
    webBondSendChunk(req, "{");
    webBondSendChunkf(req, "\"mac\":\"%s\",", macStr.c_str());
    webBondSendChunkf(req, "\"name\":\"%s\",", sz(dev.name));
    webBondSendChunkf(req, "\"friendlyName\":\"%s\",", sz(dev.friendlyName));
    webBondSendChunkf(req, "\"room\":\"%s\",", sz(dev.room));
    // The room is what the peer announces; false while this device has not accepted it.
    webBondSendChunkf(req, "\"roomAccepted\":%s,",
                      (dev.room.length() == 0 || espnowGroupRoomAccepted(dev.mac, dev.room.c_str())) ? "true" : "false");
    webBondSendChunkf(req, "\"zone\":\"%s\",", sz(dev.zone));
    webBondSendChunkf(req, "\"tags\":\"%s\",", sz(dev.tags));
    webBondSendChunkf(req, "\"stationary\":%s,", dev.stationary ? "true" : "false");
    webBondSendChunkf(req, "\"encrypted\":%s", dev.encrypted ? "true" : "false");
    webBondSendChunk(req, "}");
  }
  
  webBondSendChunk(req, "]}");
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

#endif // ENABLE_BONDED_MODE — closes the wrapper that begins at line 61

// =============================================================================
// /api/bond/fs/list — structured peer directory listing via FS_LIST_REQ
// =============================================================================
// Replaces the old "remote:files /path" CLI scrape (BondFs.list in
// WebServer_Utils.h) with a typed RPC. The old path streams text through
// /api/espnow/messages and parses lines like "  name (3 items) [mount]"
// — the doneMarker collided with concurrent `fsusage` output until a fix
// renamed it from "Total:" to " entries", and even that's brittle.
//
// Start and poll (all three /api/bond/fs routes). esp_http_server is one
// task; a handler that waits for the peer makes every other web request wait
// with it, and these handlers used to wait up to 6 s on a semaphore. Now:
//
//   GET /api/bond/fs/list?path=/x   starts the ESP-NOW request and answers at
//                                   once: {"success":true,"pending":true,"id":N}
//   GET /api/bond/fs/list?id=N      {"success":true,"pending":true,"id":N}
//                                   while the peer has not answered; then the
//                                   answer (the same JSON as before) or the
//                                   error, once; {"success":false,
//                                   "error":"expired"} for an id that is not
//                                   the request in the slot.
//
// No task and no tick: the reply callback stores the answer in the route's
// bridge slot, and the transitions are in WebBondFsPollCore.h. One request per
// route at a time; a second start while one is in flight, or while its answer
// still waits for its page, gets "bridge busy" and the page retries. An
// answer nobody collects is dropped by the next start after
// bond_fs_poll::kReadyHoldMs (a browser that went away), and a request the
// protocol never answered is cancelled after kPendingLimitMs.
//
// What can still block here: the send itself. fs*SendRequest hands the frame
// to the ESP-NOW transmit queue and waits for it to be taken, a few
// milliseconds normally and up to about 4 s when that queue is congested
// (System_ESPNow_FsList.cpp, sendPendingRequest).
//
// Response shape stays close to the existing BondFs.list() expectations
// (entries[].name + entries[].isDir + entries[].meta) so the JS migration
// is one-line. Extra fields (size, perms, totalEntries, hasMore,
// nextStartIndex) are added on top for clients that want them.

#if ENABLE_BONDED_MODE  // the bridge and the three handlers, to the #endif after handleBondFsGet

namespace {

// One bridge per route, so a file listing does not hold up a storage-stat
// update. All three are PSRAM (.ext_ram.bss); the only internal RAM is the
// mutex pointer below and the mutex itself.
struct WebFsBridge {
  bond_fs_poll::Slot       slot;
  V4PayloadFsListReplyHeader hdr;
  V4PayloadFsEntry         entries[FS_LIST_ENTRIES_PER_REPLY];
  uint8_t                  entryCount;
};
struct WebFsStatBridge {
  bond_fs_poll::Slot     slot;
  V4PayloadFsStatReply   reply;
};
struct WebFsGetBridge {
  bond_fs_poll::Slot     slot;
  V4PayloadFsGetAck      ack;
};

EXT_RAM_BSS_ATTR static WebFsBridge     sWebFsBridge = {};
EXT_RAM_BSS_ATTR static WebFsStatBridge sWebFsStatBridge = {};
EXT_RAM_BSS_ATTR static WebFsGetBridge  sWebFsGetBridge = {};

// Guards the three slots and their stored answers between the httpd task (the
// handlers) and espnow_task (the reply callbacks). Never held across a send, a
// socket write or any wait: a holder keeps it for one memcpy at most.
static SemaphoreHandle_t sWebFsMutex = nullptr;

// A handler that cannot take the mutex answers "busy" or "pending" and the
// page asks again; a callback that cannot take it drops the reply and the
// request ends as a timeout on the page.
static const TickType_t kWebFsHandlerLockWait  = pdMS_TO_TICKS(100);
static const TickType_t kWebFsCallbackLockWait = pdMS_TO_TICKS(20);

struct WebFsLock {
  bool held;
  explicit WebFsLock(TickType_t wait) : held(false) {
    if (sWebFsMutex) held = (xSemaphoreTake(sWebFsMutex, wait) == pdTRUE);
  }
  ~WebFsLock() { if (held) xSemaphoreGive(sWebFsMutex); }
  WebFsLock(const WebFsLock&) = delete;
  WebFsLock& operator=(const WebFsLock&) = delete;
};

// Called from the handlers only (one httpd task), before any request is sent.
static bool webFsBridgeEnsureInit() {
  if (!sWebFsMutex) sWebFsMutex = xSemaphoreCreateMutex();
  return sWebFsMutex != nullptr;
}

// The reply callbacks run on espnow_task and must stay minimal: take the
// bridge mutex, let the slot decide whether this reply is the one it waits
// for (a reply of an earlier, cancelled request is refused), copy the payload.
// The protocol calls each exactly once per request: with the peer's reply, or
// with a synthesized IO_ERROR when its own 5 s timeout passes.
static void webFsListReplyCallback(const uint8_t /*mac*/[6],
                                   const V4PayloadFsListReplyHeader* hdr,
                                   const V4PayloadFsEntry* entries) {
  if (!hdr) return;
  WebFsLock lock(kWebFsCallbackLockWait);
  if (!lock.held) return;
  if (!bond_fs_poll::accept(sWebFsBridge.slot, hdr->reqId, millis())) return;  // stale; ignore
  memcpy(&sWebFsBridge.hdr, hdr, sizeof(*hdr));
  uint8_t n = hdr->entryCount;
  if (n > FS_LIST_ENTRIES_PER_REPLY) n = FS_LIST_ENTRIES_PER_REPLY;
  if (entries && n > 0) {
    memcpy(sWebFsBridge.entries, entries, n * sizeof(V4PayloadFsEntry));
  }
  sWebFsBridge.entryCount = (entries && n > 0) ? n : 0;
}

static void webFsStatReplyCallback(const uint8_t /*mac*/[6],
                                   const V4PayloadFsStatReply* reply) {
  if (!reply) return;
  WebFsLock lock(kWebFsCallbackLockWait);
  if (!lock.held) return;
  if (!bond_fs_poll::accept(sWebFsStatBridge.slot, reply->reqId, millis())) return;
  memcpy(&sWebFsStatBridge.reply, reply, sizeof(*reply));
}

// Get-ACK callback. The ACK is the first part of the two-stage GET protocol;
// the actual file content arrives later via FILE_START/DATA/END handled by
// System_ESPNow_Files.
static void webFsGetAckCallback(const uint8_t /*mac*/[6],
                                const V4PayloadFsGetAck* ack) {
  if (!ack) return;
  WebFsLock lock(kWebFsCallbackLockWait);
  if (!lock.held) return;
  if (!bond_fs_poll::accept(sWebFsGetBridge.slot, ack->reqId, millis())) return;
  memcpy(&sWebFsGetBridge.ack, ack, sizeof(*ack));
}

// The JSON error tag of an FS answer's status: the one name table of rule F3
// (System_ESPNow_AnswerCore.h), the same words the console and the OLED use;
// "unknown" for a code this firmware does not know.
static const char* fsStatusTag(uint8_t status) {
  const char* name = espnowanswer::statusName(ESPNOW_V4_TYPE_FS_LIST_REPLY, status);
  return name ? name : "unknown";
}

// The id parameter of a poll. None: the query has no id, so the request is a
// start. Invalid: an id that is not a whole number above 0 (answered as an
// unknown request, never treated as a start).
enum class WebFsQueryId : uint8_t { None, Valid, Invalid };

static WebFsQueryId webFsQueryId(const char* query, uint32_t& id) {
  id = 0;
  char idBuf[16];
  const esp_err_t err = httpd_query_key_value(query, "id", idBuf, sizeof(idBuf));
  if (err == ESP_ERR_NOT_FOUND) return WebFsQueryId::None;
  if (err != ESP_OK) return WebFsQueryId::Invalid;
  return parseIntArg(idBuf, 1, 0xFFFFFFFFLL, id) ? WebFsQueryId::Valid
                                                 : WebFsQueryId::Invalid;
}

static esp_err_t webFsSendPending(httpd_req_t* req, uint32_t id) {
  char body[64];
  snprintf(body, sizeof(body), "{\"success\":true,\"pending\":true,\"id\":%u}", (unsigned)id);
  return sendJsonResponse(req, body);
}

// The reply to a poll that has no stored answer to send.
static esp_err_t webFsSendNoAnswer(httpd_req_t* req, bond_fs_poll::Poll outcome, uint32_t id) {
  switch (outcome) {
    case bond_fs_poll::Poll::Pending:
      return webFsSendPending(req, id);
    case bond_fs_poll::Poll::TimedOut:
      return sendJsonResponse(req, "{\"success\":false,\"error\":\"timeout\"}");
    default:
      return sendJsonResponse(req, "{\"success\":false,\"error\":\"expired\"}");
  }
}

// Sends the listing stored in sWebFsBridge. The slot is Collecting while this
// runs: the callback refuses to write and no other handler runs (one httpd
// task), so the stored listing is read without the mutex and the mutex is not
// held across the socket writes.
static esp_err_t webFsSendListing(httpd_req_t* req) {
  // Build response JSON. Streamed in chunks since 32 entries × ~110 chars
  // = ~3.5 KB; we don't want a single contiguous String that big in DRAM.
  uint8_t status = sWebFsBridge.hdr.status;
  uint8_t count  = sWebFsBridge.entryCount;
  uint16_t total = sWebFsBridge.hdr.totalEntries;
  bool hasMore   = sWebFsBridge.hdr.hasMore != 0;
  uint16_t nextStart = sWebFsBridge.hdr.nextStartIndex;
  // header.path is the normalized path the peer echoed back; we use it so
  // the client sees a canonical form even after path traversal.
  char peerEchoPath[sizeof(sWebFsBridge.hdr.path)];
  strlcpy(peerEchoPath, sWebFsBridge.hdr.path, sizeof(peerEchoPath));

  if (status != FS_LIST_STATUS_OK) {
    char err[160];
    snprintf(err, sizeof(err),
             "{\"success\":false,\"error\":\"%s\",\"path\":\"%s\"}",
             fsStatusTag(status), peerEchoPath);
    return sendJsonResponse(req, err);
  }

  // OK path — emit the entries array. Build the header chunk first, then
  // each entry, then the trailer. Keeps a fixed ~256 B stack buffer in
  // play instead of an N×entry-size String concatenation.
  httpd_resp_set_type(req, "application/json");
  char chunk[300];
  int hlen = snprintf(chunk, sizeof(chunk),
                      "{\"success\":true,\"path\":\"%s\","
                      "\"totalEntries\":%u,\"hasMore\":%s,"
                      "\"nextStartIndex\":%u,\"entries\":[",
                      peerEchoPath, (unsigned)total,
                      hasMore ? "true" : "false", (unsigned)nextStart);
  httpd_resp_send_chunk(req, chunk, hlen);

  for (uint8_t i = 0; i < count; i++) {
    const V4PayloadFsEntry& e = sWebFsBridge.entries[i];

    // JSON-escape name. Limited set — control chars and backslash/quote
    // are the only ones we need to handle for VFS filenames in practice.
    char escName[80];
    size_t out = 0;
    for (size_t k = 0; k < sizeof(e.name) && e.name[k]; k++) {
      if (out >= sizeof(escName) - 6) break;
      char c = e.name[k];
      if (c == '"' || c == '\\') {
        escName[out++] = '\\';
        escName[out++] = c;
      } else if ((unsigned char)c < 0x20) {
        out += snprintf(escName + out, sizeof(escName) - out, "\\u%04x", c);
      } else {
        escName[out++] = c;
      }
    }
    escName[out] = '\0';

    int elen = snprintf(chunk, sizeof(chunk),
                        "%s{\"name\":\"%s\",\"isDir\":%s,\"size\":%u,\"perms\":%u}",
                        i == 0 ? "" : ",",
                        escName,
                        e.isFolder ? "true" : "false",
                        (unsigned)e.size, (unsigned)e.perms);
    httpd_resp_send_chunk(req, chunk, elen);
  }

  httpd_resp_send_chunk(req, "]}", 2);
  httpd_resp_send_chunk(req, NULL, 0);  // end stream
  return ESP_OK;
}

} // anonymous namespace

static esp_err_t handleBondFsList(httpd_req_t* req) {
  WEB_AUTH_JSON_OR_RETURN(req, ctx);
  // Browsing the bonded peer's files needs the same local authority as
  // `remote:files`: an administrator. The peer serves under its bond
  // identity, so this is the human half of the gate. It is checked on the
  // start and on every poll: an answer is only ever handed to an admin.
  if (!hasAdminPrivilege(ctx)) return sendJsonResponse(req, "{\"success\":false,\"error\":\"admin required\"}");
  if (!webFsBridgeEnsureInit()) {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"bridge init failed\"}");
  }

  // Parse query string. A poll carries id; a start carries path (required)
  // and start (optional, default 0).
  char qbuf[300];
  if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"missing query\"}");
  }

  uint32_t pollId = 0;
  const WebFsQueryId idKind = webFsQueryId(qbuf, pollId);
  if (idKind == WebFsQueryId::Invalid) {
    return webFsSendNoAnswer(req, bond_fs_poll::Poll::Unknown, 0);
  }
  if (idKind == WebFsQueryId::Valid) {
    uint32_t cancelId = 0;
    bond_fs_poll::Poll outcome;
    {
      WebFsLock lock(kWebFsHandlerLockWait);
      if (!lock.held) return webFsSendPending(req, pollId);  // ask again
      outcome = bond_fs_poll::poll(sWebFsBridge.slot, pollId, millis(), cancelId);
    }
    if (cancelId) fsListCancel(cancelId);
    if (outcome != bond_fs_poll::Poll::Ready) return webFsSendNoAnswer(req, outcome, pollId);
    const esp_err_t result = webFsSendListing(req);
    {
      WebFsLock lock(kWebFsHandlerLockWait);
      if (lock.held) bond_fs_poll::collected(sWebFsBridge.slot);
    }
    return result;
  }

  char pathBuf[FILE_MANAGER_MAX_PATH] = {0};
  if (httpd_query_key_value(qbuf, "path", pathBuf, sizeof(pathBuf)) != ESP_OK
      || pathBuf[0] == '\0') {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"missing path\"}");
  }
  // URL-decode the minimum the existing files API decodes — same level of
  // handling so behavior is consistent for users moving between local and
  // bonded views.
  String path(pathBuf);
  path.replace("%2F", "/");
  path.replace("%20", " ");

  uint16_t startIdx = 0;
  char startBuf[12];
  if (httpd_query_key_value(qbuf, "start", startBuf, sizeof(startBuf)) == ESP_OK) {
    int v = atoi(startBuf);
    if (v < 0) v = 0;
    if (v > 65535) v = 65535;
    startIdx = (uint16_t)v;
  }

  // Bonded peer must be paired AND we must have its MAC. peerMacBytes()
  // enforces bondModeEnabled too, so this is the single gate.
  uint8_t peerMac[6];
  if (!BondedPeer::peerMacBytes(peerMac)) {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"not bonded\"}");
  }

  // Take the route's one slot. Busy: another listing is in flight or its
  // answer still waits for its page; the page retries.
  uint32_t cancelId = 0;
  bond_fs_poll::Begin begun = bond_fs_poll::Begin::Busy;
  {
    WebFsLock lock(kWebFsHandlerLockWait);
    if (lock.held) begun = bond_fs_poll::begin(sWebFsBridge.slot, millis(), cancelId);
  }
  if (cancelId) fsListCancel(cancelId);  // an abandoned request: free the protocol's entry
  if (begun != bond_fs_poll::Begin::Started) {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"bridge busy\"}");
  }

  // Send without the mutex. The reply can reach the callback before this call
  // returns; the slot is Starting and accepts it.
  uint32_t reqId = fsListSendRequest(peerMac, path.c_str(), startIdx, webFsListReplyCallback);
  bool recorded = false;
  {
    WebFsLock lock(kWebFsHandlerLockWait);
    if (lock.held) {
      (void)bond_fs_poll::sent(sWebFsBridge.slot, reqId);
      recorded = true;
    }
  }
  if (!recorded) {
    // The slot stays Starting and is released by the next request once stale.
    if (reqId) fsListCancel(reqId);
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"bridge busy\"}");
  }
  if (reqId == 0) {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"send failed\"}");
  }
  return webFsSendPending(req, reqId);
}

// =============================================================================
// /api/bond/fs/stat — structured peer storage stats via FS_STAT_REQ
// =============================================================================
// Replaces the BondFs.exec('fsusage') CLI-scrape path. Same start-and-poll
// bridge as /api/bond/fs/list — fsStatSendRequest, then ?id=N until the typed
// reply is there. Query: ?path=/sd (or omit for /).
static esp_err_t handleBondFsStat(httpd_req_t* req) {
  WEB_AUTH_JSON_OR_RETURN(req, ctx);
  if (!webFsBridgeEnsureInit()) {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"bridge init failed\"}");
  }

  char pathBuf[FILE_MANAGER_MAX_PATH] = "/";
  char qbuf[200];
  if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
    uint32_t pollId = 0;
    const WebFsQueryId idKind = webFsQueryId(qbuf, pollId);
    if (idKind == WebFsQueryId::Invalid) {
      return webFsSendNoAnswer(req, bond_fs_poll::Poll::Unknown, 0);
    }
    if (idKind == WebFsQueryId::Valid) {
      uint32_t cancelId = 0;
      bond_fs_poll::Poll outcome;
      V4PayloadFsStatReply reply = {};
      {
        WebFsLock lock(kWebFsHandlerLockWait);
        if (!lock.held) return webFsSendPending(req, pollId);  // ask again
        outcome = bond_fs_poll::poll(sWebFsStatBridge.slot, pollId, millis(), cancelId);
        if (outcome == bond_fs_poll::Poll::Ready) {
          memcpy(&reply, &sWebFsStatBridge.reply, sizeof(reply));
          bond_fs_poll::collected(sWebFsStatBridge.slot);
        }
      }
      if (cancelId) fsListCancel(cancelId);
      if (outcome != bond_fs_poll::Poll::Ready) return webFsSendNoAnswer(req, outcome, pollId);

      uint8_t status = reply.status;
      uint64_t total = reply.totalBytes;
      uint64_t used  = reply.usedBytes;
      uint64_t freeB = reply.freeBytes;
      uint16_t pct10 = reply.percentUsedX10;
      char echoPath[sizeof(reply.path)];
      strlcpy(echoPath, reply.path, sizeof(echoPath));

      if (status != FS_LIST_STATUS_OK) {
        char err[160];
        snprintf(err, sizeof(err),
                 "{\"success\":false,\"error\":\"%s\",\"path\":\"%s\"}",
                 fsStatusTag(status), echoPath);
        return sendJsonResponse(req, err);
      }

      char body[256];
      // Doubles for the JSON output — 64-bit ints are fine numerically but JS
      // loses precision past 2^53. For storage sizes (< 1 EB) we're nowhere
      // near that limit so plain JSON numbers are safe.
      snprintf(body, sizeof(body),
               "{\"success\":true,\"path\":\"%s\","
               "\"total\":%llu,\"used\":%llu,\"free\":%llu,"
               "\"usagePercent\":%u.%u}",
               echoPath,
               (unsigned long long)total,
               (unsigned long long)used,
               (unsigned long long)freeB,
               pct10 / 10, pct10 % 10);
      return sendJsonResponse(req, body);
    }

    if (httpd_query_key_value(qbuf, "path", pathBuf, sizeof(pathBuf)) != ESP_OK
        || pathBuf[0] == '\0') {
      strlcpy(pathBuf, "/", sizeof(pathBuf));
    }
    String p(pathBuf);
    p.replace("%2F", "/");
    p.replace("%20", " ");
    strlcpy(pathBuf, p.c_str(), sizeof(pathBuf));
  }

  uint8_t peerMac[6];
  if (!BondedPeer::peerMacBytes(peerMac)) {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"not bonded\"}");
  }

  uint32_t cancelId = 0;
  bond_fs_poll::Begin begun = bond_fs_poll::Begin::Busy;
  {
    WebFsLock lock(kWebFsHandlerLockWait);
    if (lock.held) begun = bond_fs_poll::begin(sWebFsStatBridge.slot, millis(), cancelId);
  }
  if (cancelId) fsListCancel(cancelId);
  if (begun != bond_fs_poll::Begin::Started) {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"bridge busy\"}");
  }

  uint32_t reqId = fsStatSendRequest(peerMac, pathBuf, webFsStatReplyCallback);
  bool recorded = false;
  {
    WebFsLock lock(kWebFsHandlerLockWait);
    if (lock.held) {
      (void)bond_fs_poll::sent(sWebFsStatBridge.slot, reqId);
      recorded = true;
    }
  }
  if (!recorded) {
    if (reqId) fsListCancel(reqId);
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"bridge busy\"}");
  }
  if (reqId == 0) {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"send failed\"}");
  }
  return webFsSendPending(req, reqId);
}

// =============================================================================
// /api/bond/fs/get — structured peer file pull via FS_GET_REQ
// =============================================================================
// Replaces the BondFs.exec('espnowsendfile') CLI trigger. Two stages:
//   1. Start and poll: send FS_GET_REQ; the page asks with ?id=N until the
//      FS_GET_ACK is there and gets the ack status + fileSize.
//   2. If ACK was OK: the peer is now sending the file via FILE_START/DATA
//      /END (existing transfer pipeline). The file lands at
//      /espnow/received/<MAC>/<basename> via the standard inbound path —
//      same place BondFs.pull's polling expects to find it.
//
// An OK ack means the transfer is already running on the peer. Dropping an
// ack nobody collected does not stop it, and a second start for the same file
// would send it twice: the page starts once and never starts again after a
// failed poll.
//
// We don't wait for the file to fully transfer here; the client polls for
// the file to appear. That polling logic lives on the JS side because it
// makes sense to overlap with progress UI (and a long-poll handler would
// tie up an httpd worker for seconds with no value over client polling).
static esp_err_t handleBondFsGet(httpd_req_t* req) {
  WEB_AUTH_JSON_OR_RETURN(req, ctx);
  // Same local admin gate as /api/bond/fs/list (and `remote:files`), on the
  // start and on every poll.
  if (!hasAdminPrivilege(ctx)) return sendJsonResponse(req, "{\"success\":false,\"error\":\"admin required\"}");
  if (!webFsBridgeEnsureInit()) {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"bridge init failed\"}");
  }

  char qbuf[300];
  if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"missing query\"}");
  }

  uint32_t pollId = 0;
  const WebFsQueryId idKind = webFsQueryId(qbuf, pollId);
  if (idKind == WebFsQueryId::Invalid) {
    return webFsSendNoAnswer(req, bond_fs_poll::Poll::Unknown, 0);
  }
  if (idKind == WebFsQueryId::Valid) {
    uint32_t cancelId = 0;
    bond_fs_poll::Poll outcome;
    V4PayloadFsGetAck ack = {};
    {
      WebFsLock lock(kWebFsHandlerLockWait);
      if (!lock.held) return webFsSendPending(req, pollId);  // ask again
      outcome = bond_fs_poll::poll(sWebFsGetBridge.slot, pollId, millis(), cancelId);
      if (outcome == bond_fs_poll::Poll::Ready) {
        memcpy(&ack, &sWebFsGetBridge.ack, sizeof(ack));
        bond_fs_poll::collected(sWebFsGetBridge.slot);
      }
    }
    if (cancelId) fsListCancel(cancelId);
    if (outcome != bond_fs_poll::Poll::Ready) return webFsSendNoAnswer(req, outcome, pollId);

    uint8_t status = ack.status;
    uint32_t size = ack.fileSize;
    char echoPath[sizeof(ack.path)];
    strlcpy(echoPath, ack.path, sizeof(echoPath));

    if (status != FS_LIST_STATUS_OK) {
      char err[160];
      snprintf(err, sizeof(err),
               "{\"success\":false,\"error\":\"%s\",\"path\":\"%s\"}",
               fsStatusTag(status), echoPath);
      return sendJsonResponse(req, err);
    }

    // OK — peer is now sending the file. Tell the client what to expect.
    char body[256];
    snprintf(body, sizeof(body),
             "{\"success\":true,\"path\":\"%s\",\"size\":%u,"
             "\"message\":\"transfer initiated; poll local FS for file landing\"}",
             echoPath, (unsigned)size);
    return sendJsonResponse(req, body);
  }

  char pathBuf[FILE_MANAGER_MAX_PATH] = {0};
  if (httpd_query_key_value(qbuf, "path", pathBuf, sizeof(pathBuf)) != ESP_OK
      || pathBuf[0] == '\0') {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"missing path\"}");
  }
  String path(pathBuf);
  path.replace("%2F", "/");
  path.replace("%20", " ");

  uint8_t peerMac[6];
  if (!BondedPeer::peerMacBytes(peerMac)) {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"not bonded\"}");
  }

  uint32_t cancelId = 0;
  bond_fs_poll::Begin begun = bond_fs_poll::Begin::Busy;
  {
    WebFsLock lock(kWebFsHandlerLockWait);
    if (lock.held) begun = bond_fs_poll::begin(sWebFsGetBridge.slot, millis(), cancelId);
  }
  if (cancelId) fsListCancel(cancelId);
  if (begun != bond_fs_poll::Begin::Started) {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"bridge busy\"}");
  }

  uint32_t reqId = fsGetSendRequest(peerMac, path.c_str(), webFsGetAckCallback);
  bool recorded = false;
  {
    WebFsLock lock(kWebFsHandlerLockWait);
    if (lock.held) {
      (void)bond_fs_poll::sent(sWebFsGetBridge.slot, reqId);
      recorded = true;
    }
  }
  if (!recorded) {
    // Not cancelled: the peer may already be sending the file, and its ack
    // then finds the slot Starting and is stored for nobody.
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"bridge busy\"}");
  }
  if (reqId == 0) {
    return sendJsonResponse(req, "{\"success\":false,\"error\":\"send failed\"}");
  }
  return webFsSendPending(req, reqId);
}
#endif // ENABLE_BONDED_MODE

// =============================================================================
// Register Handlers
// =============================================================================

void registerBondHandlers(httpd_handle_t server) {
#if ENABLE_BONDED_MODE
  static const httpd_uri_t bondPage = { .uri = "/bond", .method = HTTP_GET, .handler = handleBondPage, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondPage);
  
  static const httpd_uri_t bondStatus = { .uri = "/api/bond/status", .method = HTTP_GET, .handler = handleBondStatus, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondStatus);
  
  static const httpd_uri_t bondStream = { .uri = "/api/bond/stream", .method = HTTP_POST, .handler = handleBondStream, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondStream);
  
  static const httpd_uri_t bondExec = { .uri = "/api/bond/exec", .method = HTTP_POST, .handler = handleBondExec, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondExec);
  
  static const httpd_uri_t bondRole = { .uri = "/api/bond/role", .method = HTTP_POST, .handler = handleBondRole, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondRole);

  static const httpd_uri_t bondCliBatch = { .uri = "/api/bond/cli/batch", .method = HTTP_POST, .handler = handleBondCliBatch, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondCliBatch);

  static const httpd_uri_t bondSettingsSync = { .uri = "/api/bond/settings/sync", .method = HTTP_POST, .handler = handleBondSettingsSync, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondSettingsSync);

  static const httpd_uri_t bondSettingsSchema = { .uri = "/api/bond/settings/schema", .method = HTTP_GET, .handler = handleBondSettingsSchema, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondSettingsSchema);

  static const httpd_uri_t bondSettingsSchemaSync = { .uri = "/api/bond/settings/schema/sync", .method = HTTP_POST, .handler = handleBondSettingsSchemaSync, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondSettingsSchemaSync);

  static const httpd_uri_t bondSettings = { .uri = "/api/bond/settings", .method = HTTP_GET, .handler = handleBondSettings, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondSettings);

  static const httpd_uri_t bondPairedDevices = { .uri = "/api/bond/paired-devices", .method = HTTP_GET, .handler = handleBondPairedDevices, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondPairedDevices);

  // Structured peer-FS endpoints — see handle* above. All three replace
  // CLI-scrape paths that previously rode the bonded `remote:` command
  // pipeline and parsed text output. Now they're typed RPCs over the
  // FS_LIST / FS_STAT / FS_GET opcode pairs.
  static const httpd_uri_t bondFsList = { .uri = "/api/bond/fs/list", .method = HTTP_GET, .handler = handleBondFsList, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondFsList);
  static const httpd_uri_t bondFsStat = { .uri = "/api/bond/fs/stat", .method = HTTP_GET, .handler = handleBondFsStat, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondFsStat);
  static const httpd_uri_t bondFsGet  = { .uri = "/api/bond/fs/get",  .method = HTTP_GET, .handler = handleBondFsGet,  .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondFsGet);
#else
  (void)server;  // Suppress unused parameter warning
#endif
}

#else // !ENABLE_HTTP_SERVER || !ENABLE_ESPNOW

void registerBondHandlers(httpd_handle_t server) {
  (void)server;
}

#endif
