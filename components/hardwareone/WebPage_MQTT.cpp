// WebPage_MQTT.cpp - MQTT status and control webpage
// Provides web interface for MQTT client management

#include "System_BuildConfig.h"

#if ENABLE_HTTP_SERVER && ENABLE_MQTT

#include <Arduino.h>
#include <esp_http_server.h>

#include "System_User.h"
#include "System_MQTT.h"
#include "System_Settings.h"
#include "WebPage_MQTT.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"

// Forward declarations
extern bool isAuthed(httpd_req_t* req, String& outUser);
extern void logAuthAttempt(bool success, const char* path, const String& userTried, const String& ip, const String& reason);
extern void streamBeginHtml(httpd_req_t* req, const char* title, bool isPublic, const String& username, const String& activePage);
extern void streamEndHtml(httpd_req_t* req);
extern Settings gSettings;

// Stream MQTT page inner content
void streamMqttInner(httpd_req_t* req) {
  // Get current MQTT status
  bool connected = isMqttConnected();
  const char* statusText = connected ? "Connected" : "Disconnected";
  const char* statusClass = connected ? "status-active" : "status-inactive";
  
  // Header section with status
  httpd_resp_send_chunk(req,
    "<h2>MQTT Client</h2>"
    "<div class=\"settings-panel\">"
    "<h3>Status</h3>"
    "<div style=\"display:flex;align-items:center;gap:12px;margin-bottom:16px;\">"
    "<span class=\"status-dot ", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, statusClass, HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "\"></span>"
    "<span id=\"mqtt-status\" style=\"font-weight:600;\">", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, statusText, HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req,
    "</span></div>"
    "<div class=\"btn-row\">"
    "<button class=\"btn\" id=\"btn-connect\" onclick=\"mqttConnect()\">Connect</button>"
    "<button class=\"btn\" id=\"btn-disconnect\" onclick=\"mqttDisconnect()\">Disconnect</button>"
    "<button class=\"btn\" onclick=\"mqttRefresh()\">Refresh Status</button>"
    "</div></div>", HTTPD_RESP_USE_STRLEN);

  // Configuration section
  httpd_resp_send_chunk(req,
    "<div class=\"settings-panel\" style=\"margin-top:16px;\">"
    "<h3>Configuration</h3>"
    "<table class=\"table\">"
    "<tr><td style=\"width:140px;font-weight:500;\">Host</td><td id=\"cfg-host\">", HTTPD_RESP_USE_STRLEN);
  
  // Show configured values (mask password)
  if (gSettings.mqttHost.length()) {
    httpd_resp_send_chunk(req, gSettings.mqttHost.c_str(), HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send_chunk(req, "<em style=\"color:var(--muted);\">Not configured</em>", HTTPD_RESP_USE_STRLEN);
  }
  
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Port</td><td id=\"cfg-port\">", HTTPD_RESP_USE_STRLEN);
  
  char portBuf[8];
  snprintf(portBuf, sizeof(portBuf), "%d", gSettings.mqttPort);
  httpd_resp_send_chunk(req, portBuf, HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Security</td><td id=\"cfg-tls\">", HTTPD_RESP_USE_STRLEN);
  
  // Display TLS mode as a clear status
  if (gSettings.mqttTLSMode == 0) {
    httpd_resp_send_chunk(req, "<span style=\"color:var(--warning);\">None</span> <span style=\"font-size:0.85em;color:var(--muted);\">(unencrypted)</span>", HTTPD_RESP_USE_STRLEN);
  } else if (gSettings.mqttTLSMode == 1) {
    httpd_resp_send_chunk(req, "<span style=\"color:var(--success);\">TLS</span> <span style=\"font-size:0.85em;color:var(--muted);\">(encrypted, trusts any server)</span>", HTTPD_RESP_USE_STRLEN);
  } else if (gSettings.mqttTLSMode == 2) {
    httpd_resp_send_chunk(req, "<span style=\"color:var(--success);\">TLS + Verify</span>", HTTPD_RESP_USE_STRLEN);
    if (gSettings.mqttCACertPath.length() > 0) {
      httpd_resp_send_chunk(req, " <span style=\"font-size:0.85em;color:var(--muted);\">(", HTTPD_RESP_USE_STRLEN);
      httpd_resp_send_chunk(req, gSettings.mqttCACertPath.c_str(), HTTPD_RESP_USE_STRLEN);
      httpd_resp_send_chunk(req, ")</span>", HTTPD_RESP_USE_STRLEN);
    }
  }
  
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Username</td><td id=\"cfg-user\">", HTTPD_RESP_USE_STRLEN);
  
  if (gSettings.mqttUser.length()) {
    httpd_resp_send_chunk(req, gSettings.mqttUser.c_str(), HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send_chunk(req, "<em style=\"color:var(--muted);\">None</em>", HTTPD_RESP_USE_STRLEN);
  }
  
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Password</td><td id=\"cfg-pass\">", HTTPD_RESP_USE_STRLEN);
  
  if (gSettings.mqttPassword.length()) {
    httpd_resp_send_chunk(req, "********", HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send_chunk(req, "<em style=\"color:var(--muted);\">None</em>", HTTPD_RESP_USE_STRLEN);
  }
  
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Base Topic</td><td id=\"cfg-topic\">", HTTPD_RESP_USE_STRLEN);
  
  if (gSettings.mqttBaseTopic.length()) {
    httpd_resp_send_chunk(req, gSettings.mqttBaseTopic.c_str(), HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send_chunk(req, "<em style=\"color:var(--muted);\">Default (hardwareone/&lt;mac&gt;)</em>", HTTPD_RESP_USE_STRLEN);
  }
  
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Publish Interval</td><td id=\"cfg-interval\">", HTTPD_RESP_USE_STRLEN);
  
  char intervalBuf[16];
  snprintf(intervalBuf, sizeof(intervalBuf), "%lu ms", (unsigned long)gSettings.mqttPublishIntervalMs);
  httpd_resp_send_chunk(req, intervalBuf, HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Auto-Start</td><td id=\"cfg-autostart\">", HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req, gSettings.mqttAutoStart ? "Enabled" : "Disabled", HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req,
    "</td></tr></table>"
    "<div class=\"btn-row\" style=\"margin-top:12px;\">"
    "<a href=\"/settings\" class=\"btn\">Edit Settings</a>"
    "</div></div>", HTTPD_RESP_USE_STRLEN);

  // Data Configuration section
  httpd_resp_send_chunk(req,
    "<div class=\"settings-panel\" style=\"margin-top:16px;\">"
    "<h3>Published Data</h3>"
    "<p style=\"color:var(--muted);font-size:0.85em;margin-bottom:12px;\">"
    "Configure which data is included in MQTT publications. Edit via Settings page.</p>"
    "<table class=\"table\">", HTTPD_RESP_USE_STRLEN);
  
  // System info toggle
  httpd_resp_send_chunk(req, "<tr><td>System Info</td><td>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, gSettings.mqttPublishSystem ? 
    "<span style=\"color:var(--success);\">✓ Enabled</span>" : 
    "<span style=\"color:var(--muted);\">Disabled</span>", HTTPD_RESP_USE_STRLEN);
  
  // WiFi info toggle
  httpd_resp_send_chunk(req, "</td></tr><tr><td>WiFi Info</td><td>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, gSettings.mqttPublishWiFi ? 
    "<span style=\"color:var(--success);\">✓ Enabled</span>" : 
    "<span style=\"color:var(--muted);\">Disabled</span>", HTTPD_RESP_USE_STRLEN);
  

#if ENABLE_THERMAL_SENSOR
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Thermal Sensor</td><td>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, gSettings.mqttPublishThermal ?
    "<span style=\"color:var(--success);\">✓ Enabled</span>" :
    "<span style=\"color:var(--muted);\">Disabled</span>", HTTPD_RESP_USE_STRLEN);
#else
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Thermal Sensor</td><td><span style=\"color:var(--muted);\">Not compiled</span>", HTTPD_RESP_USE_STRLEN);
#endif

#if ENABLE_TOF_SENSOR
  httpd_resp_send_chunk(req, "</td></tr><tr><td>ToF Sensor</td><td>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, gSettings.mqttPublishToF ?
    "<span style=\"color:var(--success);\">✓ Enabled</span>" :
    "<span style=\"color:var(--muted);\">Disabled</span>", HTTPD_RESP_USE_STRLEN);
#else
  httpd_resp_send_chunk(req, "</td></tr><tr><td>ToF Sensor</td><td><span style=\"color:var(--muted);\">Not compiled</span>", HTTPD_RESP_USE_STRLEN);
#endif

#if ENABLE_IMU_SENSOR
  httpd_resp_send_chunk(req, "</td></tr><tr><td>IMU Sensor</td><td>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, gSettings.mqttPublishIMU ?
    "<span style=\"color:var(--success);\">✓ Enabled</span>" :
    "<span style=\"color:var(--muted);\">Disabled</span>", HTTPD_RESP_USE_STRLEN);
#else
  httpd_resp_send_chunk(req, "</td></tr><tr><td>IMU Sensor</td><td><span style=\"color:var(--muted);\">Not compiled</span>", HTTPD_RESP_USE_STRLEN);
#endif

#if ENABLE_PRESENCE_SENSOR
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Presence Sensor</td><td>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, gSettings.mqttPublishPresence ?
    "<span style=\"color:var(--success);\">✓ Enabled</span>" :
    "<span style=\"color:var(--muted);\">Disabled</span>", HTTPD_RESP_USE_STRLEN);
#else
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Presence Sensor</td><td><span style=\"color:var(--muted);\">Not compiled</span>", HTTPD_RESP_USE_STRLEN);
#endif

#if ENABLE_GPS_SENSOR
  httpd_resp_send_chunk(req, "</td></tr><tr><td>GPS Location</td><td>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, gSettings.mqttPublishGPS ?
    "<span style=\"color:var(--success);\">✓ Enabled</span>" :
    "<span style=\"color:var(--muted);\">Disabled</span>", HTTPD_RESP_USE_STRLEN);
#else
  httpd_resp_send_chunk(req, "</td></tr><tr><td>GPS Location</td><td><span style=\"color:var(--muted);\">Not compiled</span>", HTTPD_RESP_USE_STRLEN);
#endif

#if ENABLE_APDS_SENSOR
  httpd_resp_send_chunk(req, "</td></tr><tr><td>APDS (Proximity)</td><td>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, gSettings.mqttPublishAPDS ?
    "<span style=\"color:var(--success);\">✓ Enabled</span>" :
    "<span style=\"color:var(--muted);\">Disabled</span>", HTTPD_RESP_USE_STRLEN);
#else
  httpd_resp_send_chunk(req, "</td></tr><tr><td>APDS (Proximity)</td><td><span style=\"color:var(--muted);\">Not compiled</span>", HTTPD_RESP_USE_STRLEN);
#endif

#if ENABLE_RTC_SENSOR
  httpd_resp_send_chunk(req, "</td></tr><tr><td>RTC Time</td><td>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, gSettings.mqttPublishRTC ?
    "<span style=\"color:var(--success);\">✓ Enabled</span>" :
    "<span style=\"color:var(--muted);\">Disabled</span>", HTTPD_RESP_USE_STRLEN);
#else
  httpd_resp_send_chunk(req, "</td></tr><tr><td>RTC Time</td><td><span style=\"color:var(--muted);\">Not compiled</span>", HTTPD_RESP_USE_STRLEN);
#endif

#if ENABLE_GAMEPAD_SENSOR
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Gamepad Input</td><td>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, gSettings.mqttPublishGamepad ?
    "<span style=\"color:var(--success);\">✓ Enabled</span>" :
    "<span style=\"color:var(--muted);\">Disabled</span>", HTTPD_RESP_USE_STRLEN);
#else
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Gamepad Input</td><td><span style=\"color:var(--muted);\">Not compiled</span>", HTTPD_RESP_USE_STRLEN);
#endif

  httpd_resp_send_chunk(req, "</td></tr></table></div>", HTTPD_RESP_USE_STRLEN);

  // External Sensors section (only show if subscriptions enabled)
  if (gSettings.mqttSubscribeExternal) {
    httpd_resp_send_chunk(req,
      "<div class=\"settings-panel\" style=\"margin-top:16px;\">"
      "<h3>External Sensors</h3>", HTTPD_RESP_USE_STRLEN);
    
    int sensorCount = getExternalSensorCount();
    if (sensorCount == 0) {
      httpd_resp_send_chunk(req,
        "<p style=\"color:var(--muted);font-style:italic;\">No external sensor data received yet.</p>",
        HTTPD_RESP_USE_STRLEN);
    } else {
      httpd_resp_send_chunk(req, "<table class=\"table\">", HTTPD_RESP_USE_STRLEN);
      
      unsigned long now = millis();
      for (int i = 0; i < sensorCount && i < 20; i++) {
        String topic, name, value;
        unsigned long lastUpdate;
        if (getExternalSensor(i, topic, name, value, lastUpdate)) {
          unsigned long ageSec = (now - lastUpdate) / 1000;
          char row[512];
          snprintf(row, sizeof(row),
            "<tr><td style=\"width:140px;font-weight:500;\">%s</td>"
            "<td>%s</td>"
            "<td style=\"width:80px;color:var(--muted);font-size:0.85em;\">%lus ago</td></tr>",
            name.c_str(),
            value.substring(0, 100).c_str(),
            ageSec);
          httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
        }
      }
      
      httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
    }
    
    httpd_resp_send_chunk(req,
      "<p style=\"color:var(--muted);font-size:0.85em;margin-top:8px;\">"
      "Subscribed topics: ", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, 
      gSettings.mqttSubscribeTopics.length() > 0 ? gSettings.mqttSubscribeTopics.c_str() : "(none)",
      HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</p></div>", HTTPD_RESP_USE_STRLEN);
  }

  // Info section
  httpd_resp_send_chunk(req, R"HTML(
<div class="settings-panel" style="margin-top:16px;">
  <h3>About MQTT</h3>
  <p style="color:var(--muted);font-size:0.9em;">
    MQTT enables this device to publish sensor data to a broker for integration with 
    home automation systems like Home Assistant. When connected, the device periodically 
    publishes a JSON blob containing cached sensor readings to the configured base topic.
  </p>
  <p style="color:var(--muted);font-size:0.9em;margin-top:8px;">
    <strong>CLI Commands:</strong> <code>openmqtt</code>, <code>closemqtt</code>, <code>mqttstatus</code>
  </p>
</div>

<script>
function mqttConnect() {
  hw.postForm('/api/cli', {cmd: 'openmqtt'})
    .then(r => r.text())
    .then(t => { alert(t); mqttRefresh(); })
    .catch(e => alert('Error: ' + e.message));
}
function mqttDisconnect() {
  hw.postForm('/api/cli', {cmd: 'closemqtt'})
    .then(r => r.text())
    .then(t => { alert(t); mqttRefresh(); })
    .catch(e => alert('Error: ' + e.message));
}
function mqttRefresh() {
  hw.fetchJSON('/api/mqtt/status')
    .then(d => {
      var dot = document.querySelector('.status-dot');
      var txt = document.getElementById('mqtt-status');
      if (d.connected) {
        dot.className = 'status-dot status-active';
        txt.textContent = 'Connected';
      } else {
        dot.className = 'status-dot status-inactive';
        txt.textContent = 'Disconnected';
      }
    })
    .catch(e => console.error('Refresh failed:', e));
}
// Auto-refresh every 5 seconds
setInterval(mqttRefresh, 5000);
</script>
)HTML", HTTPD_RESP_USE_STRLEN);
}

// Full page handler
static esp_err_t handleMqttPage(httpd_req_t* req) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  ctx.path = req ? req->uri : "/mqtt";
  getClientIP(req, ctx.ip);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  logAuthAttempt(true, req->uri, ctx.user, ctx.ip, "");

  streamBeginHtml(req, "MQTT", false, ctx.user, "mqtt");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamMqttInner(req);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
  return ESP_OK;
}

// API endpoint for MQTT status
static esp_err_t handleMqttStatus(httpd_req_t* req) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  ctx.path = "/api/mqtt/status";
  getClientIP(req, ctx.ip);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  bool connected = isMqttConnected();
  
  char json[64];
  snprintf(json, sizeof(json), "{\"connected\":%s}", connected ? "true" : "false");
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// Register handlers
void registerMqttHandlers(httpd_handle_t server) {
  static httpd_uri_t mqttPage = {
    .uri = "/mqtt",
    .method = HTTP_GET,
    .handler = handleMqttPage,
    .user_ctx = NULL
  };
  
  static httpd_uri_t mqttStatusApi = {
    .uri = "/api/mqtt/status",
    .method = HTTP_GET,
    .handler = handleMqttStatus,
    .user_ctx = NULL
  };
  
  httpd_register_uri_handler(server, &mqttPage);
  httpd_register_uri_handler(server, &mqttStatusApi);
}

#endif // ENABLE_HTTP_SERVER && ENABLE_MQTT
