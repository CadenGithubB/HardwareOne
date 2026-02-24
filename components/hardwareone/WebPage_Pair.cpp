#include "System_BuildConfig.h"

#if ENABLE_WEB_PAIR

#include <Arduino.h>
#include <LittleFS.h>

#include "System_User.h"
#include "WebPage_Pair.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"
#include "System_ESPNow.h"
#include "System_ESPNow_Sensors.h"
#include "System_Settings.h"

// Forward declarations
extern void streamPageWithContent(httpd_req_t* req, const String& activePage, const String& username, void (*contentStreamer)(httpd_req_t*));
extern void streamBeginHtml(httpd_req_t* req, const char* title, bool isPublic, const String& username, const String& activePage);
extern void streamEndHtml(httpd_req_t* req);

extern bool parseMacAddress(const String& macStr, uint8_t mac[6]);

extern EspNowState* gEspNow;

// =============================================================================
// Helper Functions
// =============================================================================

static inline esp_err_t webPairSendChunk(httpd_req_t* req, const char* s) {
  return httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN);
}

static inline esp_err_t webPairSendChunkf(httpd_req_t* req, const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
}

// =============================================================================
// Pair Dashboard Page
// =============================================================================

void streamPairInner(httpd_req_t* req) {
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
@keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.6; } 100% { opacity: 1; } }
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
.sensor-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); gap: 10px; margin-top: 15px; }
.sensor-toggle { display: flex; align-items: center; justify-content: space-between; padding: 10px 12px; background: var(--crumb-bg); border-radius: 8px; cursor: pointer; transition: all 0.2s; }
.sensor-toggle:hover { background: var(--border); }
.sensor-toggle.active { background: rgba(40, 167, 69, 0.15); border: 1px solid #28a745; }
.sensor-toggle.disabled { opacity: 0.4; cursor: not-allowed; pointer-events: none; }
.sensor-toggle.disabled .sensor-name { text-decoration: line-through; }
.sensor-name { font-size: 0.9em; font-weight: 500; }
.toggle-switch { width: 40px; height: 22px; background: var(--border); border-radius: 11px; position: relative; transition: background 0.2s; }
.toggle-switch.on { background: #28a745; }
.toggle-switch::after { content: ''; position: absolute; width: 18px; height: 18px; background: white; border-radius: 50%; top: 2px; left: 2px; transition: left 0.2s; }
.toggle-switch.on::after { left: 20px; }
.cli-input { display: flex; gap: 10px; margin-top: 15px; }
.cli-input input { flex: 1; padding: 10px; border: 1px solid var(--border); border-radius: 8px; font-family: 'Courier New', monospace; background: var(--panel-bg); color: var(--panel-fg); }
.cli-output { background: #1e1e1e; color: #d4d4d4; border-radius: 8px; padding: 12px; font-family: 'Courier New', monospace; font-size: 0.85em; max-height: 200px; overflow-y: auto; margin-top: 10px; white-space: pre-wrap; }
.no-pair-warning { text-align: center; padding: 40px 20px; color: var(--muted); }
.no-pair-warning h3 { color: var(--panel-fg); margin-bottom: 10px; }
.refresh-btn { position: absolute; top: 15px; right: 15px; padding: 6px 12px; font-size: 0.85em; }
.link-quality { display: flex; align-items: center; gap: 8px; }
.signal-bars { display: flex; align-items: flex-end; gap: 2px; height: 16px; }
.signal-bar { width: 4px; background: var(--border); border-radius: 1px; }
.signal-bar.active { background: #28a745; }
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
  
  function renderDashboard(data) {
    const container = document.getElementById('remote-content');
    if (!container) return;
    
    if (!data.paired || !data.peerConfigured) {
      container.innerHTML = `
        <div class='no-pair-warning' id='no-pair-msg'>
          <h3>No Paired Device</h3>
          <p>Pair mode is not enabled or no peer is configured.</p>
          <p>Use <code>pair enable</code> and <code>pair setpeer &lt;MAC&gt;</code> to configure.</p>
        </div>
      `;
      return;
    }
    
    const online = data.peerOnline;
    const statusClass = online ? 'status-online' : 'status-offline';
    const statusText = online ? 'Online' : 'Offline';
    
    let html = '<div class="remote-grid">';
    
    // Connection Status Card
    html += '<div class="remote-card" style="position:relative">';
    html += '<button class="btn refresh-btn" onclick="window.refreshPair()">Refresh</button>';
    html += '<div class="remote-title"><span class="status-dot ' + statusClass + '"></span>Paired Device</div>';
    html += '<div class="remote-description">' + (data.peerName || 'Unknown') + ' (Role: ' + (data.role === 1 ? 'Master' : 'Worker') + ')</div>';
    
    html += '<div class="stat-row"><span class="stat-label">MAC Address</span><span class="stat-value">' + (data.peerMac || '—') + '</span></div>';
    html += '<div class="stat-row"><span class="stat-label">Status</span><span class="stat-value">' + statusText + '</span></div>';
    
    if (online && data.lastHeartbeatAgeSec !== undefined) {
      html += '<div class="stat-row"><span class="stat-label">Last Seen</span><span class="stat-value">' + data.lastHeartbeatAgeSec + 's ago</span></div>';
    }
    
    if (data.peerUptime !== undefined) {
      html += '<div class="stat-row"><span class="stat-label">Peer Uptime</span><span class="stat-value">' + formatUptime(data.peerUptime) + '</span></div>';
    }
    
    html += '</div>';
    
    // Link Quality Card
    html += '<div class="remote-card">';
    html += '<div class="remote-title">Link Quality</div>';
    
    const health = data.healthScore || 0;
    html += '<div class="health-bar"><div class="health-fill ' + getHealthClass(health) + '" style="width:' + health + '%"></div></div>';
    html += '<div style="text-align:center;font-size:0.9em;color:var(--panel-fg)">' + health + '% Health</div>';
    
    html += '<div class="stat-row"><span class="stat-label">RSSI</span><span class="stat-value link-quality">' + renderSignalBars(data.rssi || -90) + ' ' + (data.rssi || '—') + ' dBm</span></div>';
    html += '<div class="stat-row"><span class="stat-label">Heartbeats RX</span><span class="stat-value">' + (data.heartbeatsRx || 0) + '</span></div>';
    html += '<div class="stat-row"><span class="stat-label">Heartbeats TX</span><span class="stat-value">' + (data.heartbeatsTx || 0) + '</span></div>';
    
    if (data.packetLoss !== undefined) {
      html += '<div class="stat-row"><span class="stat-label">Packet Loss</span><span class="stat-value">' + data.packetLoss.toFixed(1) + '%</span></div>';
    }
    
    html += '</div>';
    
    // Sensor Streaming Card
    html += '<div class="remote-card">';
    html += '<div class="remote-title">Sensor Streaming</div>';
    html += '<div class="remote-description">Toggle sensors to stream data from paired device</div>';
    html += '<div class="sensor-grid">';
    
    // Sensor capability bit masks (must match System_ESPNow.h)
    const CAP_SENSOR_THERMAL = 0x01;
    const CAP_SENSOR_TOF = 0x02;
    const CAP_SENSOR_IMU = 0x04;
    const CAP_SENSOR_GAMEPAD = 0x08;
    const CAP_SENSOR_GPS = 0x20;
    
    const CAP_SENSOR_PRESENCE = 0x80;
    const sensors = [
      {id: 'thermal', name: 'Thermal', enabled: data.streamThermal, mask: CAP_SENSOR_THERMAL},
      {id: 'tof', name: 'ToF', enabled: data.streamTof, mask: CAP_SENSOR_TOF},
      {id: 'imu', name: 'IMU', enabled: data.streamImu, mask: CAP_SENSOR_IMU},
      {id: 'gps', name: 'GPS', enabled: data.streamGps, mask: CAP_SENSOR_GPS},
      {id: 'gamepad', name: 'Gamepad', enabled: data.streamGamepad, mask: CAP_SENSOR_GAMEPAD},
      {id: 'fmradio', name: 'FM Radio', enabled: data.streamFmradio, mask: 0},  // No mask yet
      {id: 'presence', name: 'Presence', enabled: data.streamPresence, mask: CAP_SENSOR_PRESENCE}
    ];
    
    const sensorMask = data.capabilities ? data.capabilities.sensorMask : 0;
    const sensorConnected = data.sensorConnected || {};
    
    for (const s of sensors) {
      // Only show if compiled in (mask bit set) or if no capability data yet
      const isCompiled = !data.capabilities || (sensorMask & s.mask) || s.mask === 0;
      if (!isCompiled) continue;
      
      // Check if actually connected
      const isConnected = sensorConnected[s.id] !== false;
      
      const activeClass = s.enabled ? ' active' : '';
      const toggleClass = s.enabled ? ' on' : '';
      const disabledClass = !isConnected ? ' disabled' : '';
      const clickHandler = isConnected ? 'onclick="window.toggleSensor(\'' + s.id + '\')"' : '';
      const title = !isConnected ? 'title="Sensor not connected"' : '';
      
      html += '<div class="sensor-toggle' + activeClass + disabledClass + '" ' + clickHandler + ' ' + title + '>';
      html += '<span class="sensor-name">' + s.name + '</span>';
      html += '<div class="toggle-switch' + toggleClass + '"></div>';
      html += '</div>';
    }
    
    html += '</div></div>';
    
    // Remote Capabilities Card
    if (data.capabilities) {
      html += '<div class="remote-card">';
      html += '<div class="remote-title">Remote Capabilities</div>';
      
      // Hardware
      html += '<div class="stat-row"><span class="stat-label">Flash</span><span class="stat-value">' + (data.capabilities.flashMB || '?') + ' MB</span></div>';
      html += '<div class="stat-row"><span class="stat-label">PSRAM</span><span class="stat-value">' + (data.capabilities.psramMB || '?') + ' MB</span></div>';
      
      // Features (compile-time)
      if (data.capabilities.features) {
        html += '<div class="stat-row"><span class="stat-label">Features</span><span class="stat-value" style="font-size:0.8em;max-width:60%;text-align:right">' + data.capabilities.features + '</span></div>';
      }
      
      // Services (runtime)
      if (data.capabilities.services) {
        html += '<div class="stat-row"><span class="stat-label">Services</span><span class="stat-value" style="font-size:0.8em;max-width:60%;text-align:right">' + data.capabilities.services + '</span></div>';
      }
      
      // Sensors - show individual list with connection status
      const sensorMask = data.capabilities.sensorMask || 0;
      const connected = data.sensorConnected || {};
      const sensorList = [];
      
      if (sensorMask & 0x01) sensorList.push('Thermal' + (connected.thermal ? ' ✓' : ' ✗'));
      if (sensorMask & 0x02) sensorList.push('ToF' + (connected.tof ? ' ✓' : ' ✗'));
      if (sensorMask & 0x04) sensorList.push('IMU' + (connected.imu ? ' ✓' : ' ✗'));
      if (sensorMask & 0x08) sensorList.push('Gamepad' + (connected.gamepad ? ' ✓' : ' ✗'));
      if (sensorMask & 0x10) sensorList.push('APDS' + (connected.apds ? ' ✓' : ' ✗'));
      if (sensorMask & 0x20) sensorList.push('GPS' + (connected.gps ? ' ✓' : ' ✗'));
      if (sensorMask & 0x40) sensorList.push('RTC' + (connected.rtc ? ' ✓' : ' ✗'));
      if (sensorMask & 0x80) sensorList.push('Presence' + (connected.presence ? ' ✓' : ' ✗'));
      
      if (sensorList.length > 0) {
        html += '<div class="stat-row"><span class="stat-label">I2C Sensors</span><span class="stat-value" style="font-size:0.8em;max-width:60%;text-align:right">' + sensorList.join(', ') + '</span></div>';
      }
      
      html += '</div>';
    }
    
    // Remote CLI Card
    html += '<div class="remote-card" style="grid-column: 1 / -1">';
    html += '<div class="remote-title">Remote Command Execution</div>';
    html += '<div class="remote-description">Execute CLI commands on the paired device</div>';
    html += '<div class="cli-input">';
    html += '<input type="text" id="remote-cmd" placeholder="Enter command (e.g., sensors, memory, status)" onkeypress="if(event.key===\'Enter\')window.execRemoteCmd()">';
    html += '<button class="btn" onclick="window.execRemoteCmd()">Execute</button>';
    html += '</div>';
    html += '<div class="cli-output" id="remote-output">Ready for commands...</div>';
    html += '</div>';
    
    html += '</div>';
    
    container.innerHTML = html;
  }
  
  window.refreshPair = function() {
    fetch('/api/bond/status')
      .then(r => r.json())
      .then(data => {
        lastStatus = data;
        renderDashboard(data);
      })
      .catch(e => {
        console.error('[Pair] Status fetch error:', e);
      });
  };
  
  window.toggleSensor = function(sensorId) {
    fetch('/api/bond/stream', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'sensor=' + encodeURIComponent(sensorId) + '&action=toggle'
    })
    .then(r => r.json())
    .then(data => {
      if (data.success) {
        window.refreshPair();
      } else {
        alert('Failed to toggle sensor: ' + (data.error || 'Unknown error'));
      }
    })
    .catch(e => {
      console.error('[Pair] Toggle error:', e);
    });
  };
  
  window.execRemoteCmd = function() {
    const input = document.getElementById('remote-cmd');
    const output = document.getElementById('remote-output');
    const cmd = input.value.trim();
    if (!cmd) return;
    
    output.textContent = 'Executing: ' + cmd + '...\n';
    
    fetch('/api/bond/exec', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'cmd=' + encodeURIComponent(cmd)
    })
    .then(r => r.json())
    .then(data => {
      if (data.success) {
        output.textContent = '> ' + cmd + '\n\n' + (data.result || '(no output)');
      } else {
        output.textContent = '> ' + cmd + '\n\nError: ' + (data.error || 'Command failed');
      }
    })
    .catch(e => {
      output.textContent = 'Error: ' + e.message;
    });
    
    input.value = '';
  };
  
  // Initial load and auto-refresh
  window.refreshPair();
  refreshInterval = setInterval(window.refreshPair, 5000);
  
  // Cleanup on page unload
  window.addEventListener('beforeunload', function() {
    if (refreshInterval) clearInterval(refreshInterval);
  });
})();
</script>
)JS", HTTPD_RESP_USE_STRLEN);
}

static void streamPairContent(httpd_req_t* req) {
  String u;
  isAuthed(req, u);
  streamBeginHtml(req, "Bonded Device", false, u, "bond");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamPairInner(req);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

static esp_err_t handleBondPage(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  
  streamPageWithContent(req, "bond", ctx.user, streamPairContent);
  return ESP_OK;
}

// =============================================================================
// API: Bond Status
// =============================================================================

static esp_err_t handleBondStatus(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  
  if (!tgRequireAuth(ctx)) return ESP_OK;
  
  httpd_resp_set_type(req, "application/json");
  
  bool paired = gSettings.bondModeEnabled;
  bool peerConfigured = false;
  uint8_t peerMac[6] = {0};
  char macStr[18] = "00:00:00:00:00:00";
  if (paired && gSettings.bondPeerMac.length() > 0 && parseMacAddress(gSettings.bondPeerMac, peerMac)) {
    peerConfigured = true;
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5]);
  }
  
  // Get peer name from capability cache
  const char* peerName = "Unknown";
  if (gEspNow && gEspNow->lastRemoteCapValid) {
    peerName = gEspNow->lastRemoteCap.deviceName;
  }
  
  // Online status and health metrics (use pre-calculated values from EspNowState)
  bool peerOnline = gEspNow ? gEspNow->bondPeerOnline : false;
  unsigned long lastHb = gEspNow ? gEspNow->lastBondHeartbeatReceivedMs : 0;
  uint32_t hbRx = gEspNow ? gEspNow->bondHeartbeatsReceived : 0;
  uint32_t hbTx = gEspNow ? gEspNow->bondHeartbeatsSent : 0;
  
  // Use pre-calculated health metrics from EspNowState (updated in processPairedHeartbeats)
  int healthScore = gEspNow ? gEspNow->bondHealthScore : 0;
  float packetLoss = gEspNow ? (gEspNow->bondPacketLoss / 10.0f) : 100.0f;  // 0-1000 -> 0.0-100.0%
  int rssi = gEspNow ? gEspNow->bondRssiAvg : -100;
  int rssiLast = gEspNow ? gEspNow->bondRssiLast : -100;
  uint16_t latencyMs = gEspNow ? gEspNow->bondLatencyMs : 0;
  uint16_t missedHb = gEspNow ? gEspNow->bondMissedHeartbeats : 0;
  uint32_t peerUptime = 0;
  
  if (peerOnline && gEspNow && gEspNow->lastRemoteCapValid) {
    peerUptime = gEspNow->lastRemoteCap.uptimeSeconds;
  }
  
  uint32_t lastHeartbeatAgeSec = 0;
  if (peerOnline && lastHb > 0) {
    unsigned long now = millis();
    if (now >= lastHb) {
      lastHeartbeatAgeSec = (uint32_t)((now - lastHb) / 1000UL);
    }
  }
  
  // Stream JSON response
  webPairSendChunk(req, "{");
  webPairSendChunkf(req, "\"paired\":%s,", paired ? "true" : "false");
  webPairSendChunkf(req, "\"peerConfigured\":%s,", peerConfigured ? "true" : "false");
  webPairSendChunkf(req, "\"peerOnline\":%s,", peerOnline ? "true" : "false");
  webPairSendChunkf(req, "\"peerMac\":\"%s\",", macStr);
  webPairSendChunkf(req, "\"peerName\":\"%s\",", peerName);
  webPairSendChunkf(req, "\"role\":%d,", gSettings.bondRole);
  webPairSendChunkf(req, "\"lastHeartbeat\":%lu,", lastHb);
  webPairSendChunkf(req, "\"lastHeartbeatAgeSec\":%lu,", (unsigned long)lastHeartbeatAgeSec);
  webPairSendChunkf(req, "\"heartbeatsRx\":%lu,", (unsigned long)hbRx);
  webPairSendChunkf(req, "\"heartbeatsTx\":%lu,", (unsigned long)hbTx);
  webPairSendChunkf(req, "\"healthScore\":%d,", healthScore);
  webPairSendChunkf(req, "\"packetLoss\":%.1f,", packetLoss);
  webPairSendChunkf(req, "\"rssi\":%d,", rssi);
  webPairSendChunkf(req, "\"rssiLast\":%d,", rssiLast);
  webPairSendChunkf(req, "\"latencyMs\":%u,", latencyMs);
  webPairSendChunkf(req, "\"missedHeartbeats\":%u,", missedHb);
  webPairSendChunkf(req, "\"peerUptime\":%lu,", (unsigned long)peerUptime);
  
  // Streaming settings
  webPairSendChunkf(req, "\"streamThermal\":%s,", gSettings.bondStreamThermal ? "true" : "false");
  webPairSendChunkf(req, "\"streamTof\":%s,", gSettings.bondStreamTof ? "true" : "false");
  webPairSendChunkf(req, "\"streamImu\":%s,", gSettings.bondStreamImu ? "true" : "false");
  webPairSendChunkf(req, "\"streamGps\":%s,", gSettings.bondStreamGps ? "true" : "false");
  webPairSendChunkf(req, "\"streamGamepad\":%s,", gSettings.bondStreamGamepad ? "true" : "false");
  webPairSendChunkf(req, "\"streamFmradio\":%s,", gSettings.bondStreamFmradio ? "true" : "false");
  webPairSendChunkf(req, "\"streamPresence\":%s,", gSettings.bondStreamPresence ? "true" : "false");
  
  // Capabilities (if available)
  if (gEspNow && gEspNow->lastRemoteCapValid) {
    CapabilitySummary& cap = gEspNow->lastRemoteCap;
    String features = getCapabilityListLong(cap.featureMask, FEATURE_NAMES);
    String sensors = getCapabilityListLong(cap.sensorMask, SENSOR_NAMES);
    String services = getCapabilityListLong(cap.serviceMask, SERVICE_NAMES);
    
    webPairSendChunk(req, "\"capabilities\":{");
    webPairSendChunkf(req, "\"features\":\"%s\",", features.c_str());
    webPairSendChunkf(req, "\"sensors\":\"%s\",", sensors.c_str());
    webPairSendChunkf(req, "\"services\":\"%s\",", services.c_str());
    webPairSendChunkf(req, "\"flashMB\":%lu,", (unsigned long)cap.flashSizeMB);
    webPairSendChunkf(req, "\"psramMB\":%lu,", (unsigned long)cap.psramSizeMB);
    
    // Add individual sensor/feature masks for UI logic
    webPairSendChunkf(req, "\"featureMask\":%lu,", (unsigned long)cap.featureMask);
    webPairSendChunkf(req, "\"sensorMask\":%lu,", (unsigned long)cap.sensorMask);
    webPairSendChunkf(req, "\"serviceMask\":%lu", (unsigned long)cap.serviceMask);
    webPairSendChunk(req, "},");
    
    // Add sensor connectivity status (from manifest if available)
    webPairSendChunk(req, "\"sensorConnected\":{");
    webPairSendChunkf(req, "\"thermal\":%s,", (cap.sensorMask & CAP_SENSOR_THERMAL) ? "true" : "false");
    webPairSendChunkf(req, "\"tof\":%s,", (cap.sensorMask & CAP_SENSOR_TOF) ? "true" : "false");
    webPairSendChunkf(req, "\"imu\":%s,", (cap.sensorMask & CAP_SENSOR_IMU) ? "true" : "false");
    webPairSendChunkf(req, "\"gps\":%s,", (cap.sensorMask & CAP_SENSOR_GPS) ? "true" : "false");
    webPairSendChunkf(req, "\"gamepad\":%s,", (cap.sensorMask & CAP_SENSOR_GAMEPAD) ? "true" : "false");
    webPairSendChunkf(req, "\"fmradio\":%s,", false ? "true" : "false");  // FM radio not in capability mask yet
    webPairSendChunkf(req, "\"presence\":%s", (cap.sensorMask & CAP_SENSOR_PRESENCE) ? "true" : "false");
    webPairSendChunk(req, "}");
  } else {
    webPairSendChunk(req, "\"capabilities\":null,\"sensorConnected\":null");
  }
  
  webPairSendChunk(req, "}");
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// =============================================================================
// API: Bond Stream Control
// =============================================================================

static esp_err_t handleBondStream(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  
  if (!tgRequireAuth(ctx)) return ESP_OK;
  
  httpd_resp_set_type(req, "application/json");
  
  // Parse POST body
  char buf[128];
  int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (len <= 0) {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"No data\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  buf[len] = '\0';
  
  // Parse sensor and action
  char sensorParam[32] = {0};
  char actionParam[16] = {0};
  
  // Simple URL-encoded parsing
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
  
  // Map sensor name to setting and streaming function
  bool* settingPtr = nullptr;
  RemoteSensorType sensorType = REMOTE_SENSOR_THERMAL;
  
  if (strcmp(sensorParam, "thermal") == 0) {
    settingPtr = &gSettings.bondStreamThermal;
    sensorType = REMOTE_SENSOR_THERMAL;
  } else if (strcmp(sensorParam, "tof") == 0) {
    settingPtr = &gSettings.bondStreamTof;
    sensorType = REMOTE_SENSOR_TOF;
  } else if (strcmp(sensorParam, "imu") == 0) {
    settingPtr = &gSettings.bondStreamImu;
    sensorType = REMOTE_SENSOR_IMU;
  } else if (strcmp(sensorParam, "gps") == 0) {
    settingPtr = &gSettings.bondStreamGps;
    sensorType = REMOTE_SENSOR_GPS;
  } else if (strcmp(sensorParam, "gamepad") == 0) {
    settingPtr = &gSettings.bondStreamGamepad;
    sensorType = REMOTE_SENSOR_GAMEPAD;
  } else if (strcmp(sensorParam, "fmradio") == 0) {
    settingPtr = &gSettings.bondStreamFmradio;
    sensorType = REMOTE_SENSOR_FMRADIO;
  } else if (strcmp(sensorParam, "presence") == 0) {
    settingPtr = &gSettings.bondStreamPresence;
    sensorType = REMOTE_SENSOR_PRESENCE;
  } else {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Unknown sensor\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  // Toggle or set explicitly
  bool newState;
  if (strcmp(actionParam, "toggle") == 0) {
    newState = !(*settingPtr);
  } else if (strcmp(actionParam, "on") == 0) {
    newState = true;
  } else if (strcmp(actionParam, "off") == 0) {
    newState = false;
  } else {
    newState = !(*settingPtr); // Default to toggle
  }
  
  // Update setting and start/stop streaming
  *settingPtr = newState;
  writeSettingsJson();
  
  if (newState) {
    startSensorDataStreaming(sensorType);
  } else {
    stopSensorDataStreaming(sensorType);
  }
  
  webPairSendChunkf(req, "{\"success\":true,\"sensor\":\"%s\",\"enabled\":%s}", 
                      sensorParam, newState ? "true" : "false");
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// =============================================================================
// API: Bond Command Execution
// =============================================================================

// Forward declaration for unified command execution
extern bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize);

static esp_err_t handleBondExec(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  
  if (!tgRequireAuth(ctx)) return ESP_OK;
  
  httpd_resp_set_type(req, "application/json");
  
  // Parse POST body
  char buf[512];
  int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (len <= 0) {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"No data\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  buf[len] = '\0';
  
  // Parse command
  char cmdParam[256] = {0};
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
  }
  
  if (strlen(cmdParam) == 0) {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Missing command\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  // Use unified remote command routing - prefix with "remote:"
  // executeCommand() handles session token auth automatically
  String remoteCmd = "remote:";
  remoteCmd += cmdParam;
  
  char resultBuf[1024];
  bool success = executeCommand(ctx, remoteCmd.c_str(), resultBuf, sizeof(resultBuf));
  
  webPairSendChunkf(req, "{\"success\":%s,\"result\":", success ? "true" : "false");
  
  // Escape result for JSON
  webPairSendChunk(req, "\"");
  for (const char* p = resultBuf; *p; p++) {
    if (*p == '"') webPairSendChunk(req, "\\\"");
    else if (*p == '\\') webPairSendChunk(req, "\\\\");
    else if (*p == '\n') webPairSendChunk(req, "\\n");
    else if (*p == '\r') webPairSendChunk(req, "\\r");
    else if (*p == '\t') webPairSendChunk(req, "\\t");
    else {
      char c[2] = {*p, 0};
      webPairSendChunk(req, c);
    }
  }
  webPairSendChunk(req, "\"}");
  
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// =============================================================================
// Register Handlers
// =============================================================================

void registerPairHandlers(httpd_handle_t server) {
  static httpd_uri_t bondPage = { .uri = "/bond", .method = HTTP_GET, .handler = handleBondPage, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondPage);
  
  static httpd_uri_t bondStatus = { .uri = "/api/bond/status", .method = HTTP_GET, .handler = handleBondStatus, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondStatus);
  
  static httpd_uri_t bondStream = { .uri = "/api/bond/stream", .method = HTTP_POST, .handler = handleBondStream, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondStream);
  
  static httpd_uri_t bondExec = { .uri = "/api/bond/exec", .method = HTTP_POST, .handler = handleBondExec, .user_ctx = NULL };
  httpd_register_uri_handler(server, &bondExec);
}

#else // !ENABLE_HTTP_SERVER || !ENABLE_ESPNOW

void registerPairHandlers(httpd_handle_t server) {
  (void)server;
}

#endif
