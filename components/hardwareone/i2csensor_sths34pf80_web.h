// i2csensor_sths34pf80_web.h - STHS34PF80 Web interface functions
#ifndef I2CSENSOR_STHS34PF80_WEB_H
#define I2CSENSOR_STHS34PF80_WEB_H

#include "System_BuildConfig.h"

#if ENABLE_PRESENCE_SENSOR && ENABLE_HTTP_SERVER

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_server.h>
#include "i2csensor_sths34pf80.h"  // For PresenceCache definition

// Get presence sensor data as JSON for web API
inline void getPresenceDataJson(JsonObject& doc) {
  doc["enabled"] = gPresenceRunning;
  doc["connected"] = gPresenceConnected;
  
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
    "  var ambEl = hw.$('presence-ambient');\n"
    "  var presEl = hw.$('presence-presence');\n"
    "  var motEl = hw.$('presence-motion');\n"
    "  if (!ambEl && !presEl && !motEl) return Promise.resolve();\n"
    "  return hw.fetchJSON('/api/sensors?sensor=presence&ts=' + Date.now())\n"
    "    .then(function(data) {\n"
    "      if (!data || data.error) {\n"
    "        if (ambEl) ambEl.textContent = '--';\n"
    "        if (presEl) presEl.textContent = data && data.error ? data.error : '--';\n"
    "        if (motEl) motEl.textContent = '--';\n"
    "        return;\n"
    "      }\n"
    "      if (ambEl) ambEl.textContent = (data.ambientTemp !== undefined ? data.ambientTemp.toFixed(1) + '°C' : '--');\n"
    "      if (presEl) {\n"
    "        var p = (data.presenceValue !== undefined ? String(data.presenceValue) : '--');\n"
    "        if (data.presenceDetected) p += ' [DETECTED]';\n"
    "        presEl.textContent = p;\n"
    "      }\n"
    "      if (motEl) {\n"
    "        var m = (data.motionValue !== undefined ? String(data.motionValue) : '--');\n"
    "        if (data.motionDetected) m += ' [DETECTED]';\n"
    "        motEl.textContent = m;\n"
    "      }\n"
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
