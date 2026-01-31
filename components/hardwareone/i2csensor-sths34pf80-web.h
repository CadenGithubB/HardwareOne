// i2csensor-sths34pf80-web.h - STHS34PF80 Web interface functions
#ifndef I2CSENSOR_STHS34PF80_WEB_H
#define I2CSENSOR_STHS34PF80_WEB_H

#include "System_BuildConfig.h"

#if ENABLE_PRESENCE_SENSOR && ENABLE_HTTP_SERVER

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_server.h>
#include "i2csensor-sths34pf80.h"  // For PresenceCache definition

// Get presence sensor data as JSON for web API
inline void getPresenceDataJson(JsonObject& doc) {
  doc["enabled"] = presenceEnabled;
  doc["connected"] = presenceConnected;
  
  if (gPresenceCache.mutex && xSemaphoreTake(gPresenceCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    doc["dataValid"] = gPresenceCache.dataValid;
    doc["ambientTemp"] = gPresenceCache.ambientTemp;
    doc["objectTemp"] = gPresenceCache.objectTemp;
    doc["compObjectTemp"] = gPresenceCache.compObjectTemp;
    doc["presenceValue"] = gPresenceCache.presenceValue;
    doc["motionValue"] = gPresenceCache.motionValue;
    doc["tempShockValue"] = gPresenceCache.tempShockValue;
    doc["presenceDetected"] = gPresenceCache.presenceDetected;
    doc["motionDetected"] = gPresenceCache.motionDetected;
    doc["tempShockDetected"] = gPresenceCache.tempShockDetected;
    doc["lastUpdate"] = gPresenceCache.lastUpdate;
    xSemaphoreGive(gPresenceCache.mutex);
  } else {
    doc["dataValid"] = false;
  }
}

// Web page HTML fragment for presence sensor card
inline const char* getPresenceWebCard() {
  return R"HTML(
<div class="card" id="presence-card">
  <h3>IR Presence Sensor</h3>
  <div class="sensor-status">
    <span id="presence-status">Checking...</span>
  </div>
  <div class="sensor-data" id="presence-data">
    <div class="data-row">
      <span class="label">Ambient:</span>
      <span class="value" id="presence-ambient">--</span>
    </div>
    <div class="data-row">
      <span class="label">Presence:</span>
      <span class="value" id="presence-presence">--</span>
    </div>
    <div class="data-row">
      <span class="label">Motion:</span>
      <span class="value" id="presence-motion">--</span>
    </div>
    <div class="data-row">
      <span class="label">Temp Shock:</span>
      <span class="value" id="presence-shock">--</span>
    </div>
  </div>
  <div class="sensor-controls">
    <button onclick="togglePresence()" id="presence-toggle">Start</button>
  </div>
</div>
)HTML";
}

// JavaScript for presence sensor web interface
inline const char* getPresenceWebScript() {
  return R"JS(
function updatePresenceCard(data) {
  const statusEl = document.getElementById('presence-status');
  const toggleBtn = document.getElementById('presence-toggle');
  
  if (data.connected && data.enabled) {
    statusEl.textContent = 'Active';
    statusEl.className = 'status-active';
    toggleBtn.textContent = 'Close';
    
    if (data.dataValid) {
      document.getElementById('presence-ambient').textContent = data.ambientTemp.toFixed(1) + '°C';
      
      let presText = data.presenceValue.toString();
      if (data.presenceDetected) presText += ' [DETECTED]';
      document.getElementById('presence-presence').textContent = presText;
      
      let motionText = data.motionValue.toString();
      if (data.motionDetected) motionText += ' [DETECTED]';
      document.getElementById('presence-motion').textContent = motionText;
      
      let shockText = data.tempShockValue.toString();
      if (data.tempShockDetected) shockText += ' [DETECTED]';
      document.getElementById('presence-shock').textContent = shockText;
    }
  } else if (data.connected) {
    statusEl.textContent = 'Connected (Idle)';
    statusEl.className = 'status-idle';
    toggleBtn.textContent = 'Open';
  } else {
    statusEl.textContent = 'Not Connected';
    statusEl.className = 'status-disconnected';
    toggleBtn.textContent = 'Open';
  }
}

function togglePresence() {
  const cmd = document.getElementById('presence-toggle').textContent === 'Open' ? 'openpresence' : 'closepresence';
  fetch('/api/command', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({command: cmd})
  }).then(r => r.json()).then(d => console.log('Presence:', d));
}
)JS";
}

// Stream presence sensor card HTML
inline void streamSTHS34PF80PresenceSensorCard(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"HTML(
<div class='sensor-card' id='sensor-card-presence' style='display:none'>
  <div class='sensor-header'>
    <span class='sensor-title'>IR Presence</span>
    <span class='status-indicator status-disabled' id='presence-status-indicator'></span>
  </div>
  <div class='sensor-body'>
    <div class='sensor-value'><span class='label'>Ambient:</span><span id='presence-ambient'>--</span></div>
    <div class='sensor-value'><span class='label'>Presence:</span><span id='presence-presence'>--</span></div>
    <div class='sensor-value'><span class='label'>Motion:</span><span id='presence-motion'>--</span></div>
  </div>
  <div class='sensor-controls'>
    <button class='btn' id='btn-presence-start'>Open</button>
    <button class='btn' id='btn-presence-stop'>Close</button>
  </div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);
}

// Stream presence sensor button bindings
inline void streamSTHS34PF80PresenceSensorBindButtons(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "bind('btn-presence-start','openpresence');bind('btn-presence-stop','closepresence');", HTTPD_RESP_USE_STRLEN);
}

// Dashboard sensor definition for presence sensor
inline void streamSTHS34PF80PresenceDashboardDef(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "window.__dashSensorDefs.push({device:'STHS34PF80',key:'presence',name:'IR Presence (STHS34PF80)',desc:'Presence & Motion'});", HTTPD_RESP_USE_STRLEN);
}

// Presence sensor JS reader for sensors page
inline void streamSTHS34PF80PresenceSensorJs(httpd_req_t* req) {
  httpd_resp_send_chunk(req,
    "window._sensorReaders = window._sensorReaders || {};\n"
    "window._sensorDataIds = window._sensorDataIds || {};\n"
    "window._sensorPollingIntervals = window._sensorPollingIntervals || {};\n"
    "window._sensorPollingIntervals.presence = 500;\n"
    "window._sensorReaders.presence = function() {\n"
    "  return fetch('/api/sensors/status', {cache: 'no-store', credentials: 'include'})\n"
    "    .then(function(r) { return r.json(); })\n"
    "    .then(function(status) {\n"
    "      var ambEl = document.getElementById('presence-ambient');\n"
    "      var presEl = document.getElementById('presence-presence');\n"
    "      var motEl = document.getElementById('presence-motion');\n"
    "      if (!ambEl && !presEl && !motEl) return;\n"
    "      if (!status.presenceCompiled) {\n"
    "        if (ambEl) ambEl.textContent = '--';\n"
    "        if (presEl) presEl.textContent = 'not_compiled';\n"
    "        if (motEl) motEl.textContent = '--';\n"
    "        return 'not_compiled';\n"
    "      }\n"
    "      if (!status.presenceEnabled) {\n"
    "        if (ambEl) ambEl.textContent = '--';\n"
    "        if (presEl) presEl.textContent = '--';\n"
    "        if (motEl) motEl.textContent = '--';\n"
    "        return 'stopped';\n"
    "      }\n"
    "      return fetch('/api/sensors?sensor=presence&ts=' + Date.now(), {cache: 'no-store', credentials: 'include'})\n"
    "        .then(function(r) { return r.json(); })\n"
    "        .then(function(data) {\n"
    "          if (!data || data.error) {\n"
    "            if (ambEl) ambEl.textContent = '--';\n"
    "            if (presEl) presEl.textContent = '--';\n"
    "            if (motEl) motEl.textContent = '--';\n"
    "            return;\n"
    "          }\n"
    "          if (ambEl) ambEl.textContent = (data.ambientTemp !== undefined ? data.ambientTemp.toFixed(1) + '°C' : '--');\n"
    "          if (presEl) {\n"
    "            var p = (data.presenceValue !== undefined ? String(data.presenceValue) : '--');\n"
    "            if (data.presenceDetected) p += ' [DETECTED]';\n"
    "            presEl.textContent = p;\n"
    "          }\n"
    "          if (motEl) {\n"
    "            var m = (data.motionValue !== undefined ? String(data.motionValue) : '--');\n"
    "            if (data.motionDetected) m += ' [DETECTED]';\n"
    "            motEl.textContent = m;\n"
    "          }\n"
    "        });\n"
    "    })\n"
    "    .catch(function(e) {\n"
    "      console.error('[Sensors] Presence read error', e);\n"
    "      if (ambEl) ambEl.textContent = '--';\n"
    "      if (presEl) presEl.textContent = '--';\n"
    "      if (motEl) motEl.textContent = '--';\n"
    "    });\n"
    "};\n",
    HTTPD_RESP_USE_STRLEN);
}

#endif // ENABLE_PRESENCE_SENSOR && ENABLE_HTTP_SERVER
#endif // I2CSENSOR_STHS34PF80_WEB_H
