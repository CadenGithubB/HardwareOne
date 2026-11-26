// WebPage_MQTT.cpp - MQTT status and control webpage
// Provides web interface for MQTT client management

#include "System_BuildConfig.h"

#if ENABLE_WEB_MQTT

#include <Arduino.h>
#include <esp_http_server.h>

#include "System_User.h"
#include "System_MQTT.h"
#include "System_Settings.h"
#include "System_SensorRegistry.h"   // kSensorSpecs: built, publish setting
#include "WebPage_MQTT.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"
#include "WebServer_GuestSettingsView.h"  // guestview::kGuestHiddenText

// Bytes of a received value the External Sensors table shows.
static constexpr size_t kMqttSensorValueShownMax = 100;

// One row of the External Sensors table. The name is the last segment of a
// received topic and the value is a received payload: whoever publishes to a
// subscribed topic chooses both, so both go out HTML-escaped. Each is shown up
// to its first NUL, as `%s` showed it.
static void streamMqttExternalSensorRow(httpd_req_t* req, const String& name,
                                        const String& value, unsigned long ageSec) {
  httpd_resp_send_chunk(req, "<tr><td style=\"width:140px;font-weight:500;\">",
                        HTTPD_RESP_USE_STRLEN);
  streamHtmlEscaped(req, name.c_str(), strlen(name.c_str()));
  httpd_resp_send_chunk(req, "</td><td>", HTTPD_RESP_USE_STRLEN);
  streamHtmlEscaped(req, value.c_str(), strnlen(value.c_str(), kMqttSensorValueShownMax));
  char tail[128];
  snprintf(tail, sizeof(tail),
           "</td><td style=\"width:80px;color:var(--panel-fg);font-size:0.85em;\">%lus ago</td></tr>",
           ageSec);
  httpd_resp_send_chunk(req, tail, HTTPD_RESP_USE_STRLEN);
}

// One configuration cell: the setting's text, HTML-escaped, `emptyHtml` when
// it is empty, or the guest sentence when `guestView` hides a set value
// (owner, guest privacy 4: a guest does not see the broker host or user name;
// the same text as the guest's /api/settings view).
static void streamMqttConfigText(httpd_req_t* req, const String& value,
                                 bool guestView, const char* emptyHtml) {
  if (!value.length()) {
    httpd_resp_send_chunk(req, emptyHtml, HTTPD_RESP_USE_STRLEN);
  } else if (guestView) {
    httpd_resp_send_chunk(req, "<em style=\"color:var(--panel-fg);\">", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, guestview::kGuestHiddenText, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</em>", HTTPD_RESP_USE_STRLEN);
  } else {
    streamHtmlEscaped(req, value.c_str(), strlen(value.c_str()));
  }
}

// The sensor rows of the Published Data table, in page order. Each row
// closes the one before it. A sensor in the build (kSensorSpecs[].built;
// input: any input device, GT911 touch too) sends `open`, then its publish
// setting; one not in the build sends `notBuilt` alone.
struct MqttPublishedSensorRow {
  uint8_t type;           // RemoteSensorType
  const char* open;
  const char* notBuilt;
};
static constexpr MqttPublishedSensorRow kMqttPublishedSensorRows[] = {
  { REMOTE_SENSOR_THERMAL, "</td></tr><tr><td>Thermal Sensor</td><td>",
    "</td></tr><tr><td>Thermal Sensor</td><td><span style=\"color:var(--panel-fg);\">Not compiled</span>" },
  { REMOTE_SENSOR_TOF, "</td></tr><tr><td>ToF Sensor</td><td>",
    "</td></tr><tr><td>ToF Sensor</td><td><span style=\"color:var(--panel-fg);\">Not compiled</span>" },
  { REMOTE_SENSOR_IMU, "</td></tr><tr><td>IMU Sensor</td><td>",
    "</td></tr><tr><td>IMU Sensor</td><td><span style=\"color:var(--panel-fg);\">Not compiled</span>" },
  { REMOTE_SENSOR_PRESENCE, "</td></tr><tr><td>Presence Sensor</td><td>",
    "</td></tr><tr><td>Presence Sensor</td><td><span style=\"color:var(--panel-fg);\">Not compiled</span>" },
  { REMOTE_SENSOR_GPS, "</td></tr><tr><td>GPS Location</td><td>",
    "</td></tr><tr><td>GPS Location</td><td><span style=\"color:var(--panel-fg);\">Not compiled</span>" },
  { REMOTE_SENSOR_APDS, "</td></tr><tr><td>APDS (Proximity)</td><td>",
    "</td></tr><tr><td>APDS (Proximity)</td><td><span style=\"color:var(--panel-fg);\">Not compiled</span>" },
  { REMOTE_SENSOR_RTC, "</td></tr><tr><td>RTC Time</td><td>",
    "</td></tr><tr><td>RTC Time</td><td><span style=\"color:var(--panel-fg);\">Not compiled</span>" },
  { REMOTE_SENSOR_INPUT, "</td></tr><tr><td>Input Device</td><td>",
    "</td></tr><tr><td>Input Device</td><td><span style=\"color:var(--panel-fg);\">Not compiled</span>" },
};
// A row reads its type's *mqtt.publish: the rows are the types with one.
static constexpr bool mqttPublishedRowsMatch() {
  espnowsensor::SensorMask seen = 0;
  for (const MqttPublishedSensorRow& row : kMqttPublishedSensorRows) {
    if (row.type >= REMOTE_SENSOR_MAX || (seen & espnowsensor::sensorBit(row.type))) return false;
    seen |= espnowsensor::sensorBit(row.type);
  }
  return seen == kSensorMqttTypes;
}
static_assert(mqttPublishedRowsMatch(),
              "kMqttPublishedSensorRows: one row per type of kSensorMqttTypes (System_SensorRegistry.h), "
              "with its label and its Not compiled text");

static void streamMqttPublishedSensorRows(httpd_req_t* req) {
  for (const MqttPublishedSensorRow& row : kMqttPublishedSensorRows) {
    const SensorSpec& spec = kSensorSpecs[row.type];
    if (!spec.built) {
      httpd_resp_send_chunk(req, row.notBuilt, HTTPD_RESP_USE_STRLEN);
      continue;
    }
    httpd_resp_send_chunk(req, row.open, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, *spec.mqtt.publish ?
      "<span style=\"color:var(--success);\">✓ Enabled</span>" :
      "<span style=\"color:var(--panel-fg);\">Disabled</span>", HTTPD_RESP_USE_STRLEN);
  }
}

// Stream MQTT page inner content. guestView: the signed-in account is a guest.
void streamMqttInner(httpd_req_t* req, bool guestView) {
  // Get current MQTT status
  bool connected = isMqttConnected();
  const char* statusText = connected ? "Connected" : "Disconnected";
  const char* statusClass = connected ? "status-active" : "status-inactive";

  httpd_resp_send_chunk(req, "<h2>MQTT Client</h2><div id='mqtt-body'>", HTTPD_RESP_USE_STRLEN);

  // Connection status panel
  httpd_resp_send_chunk(req,
    "<div class=\"settings-panel\" style=\"margin-top:16px;\">"
    "<h3>Connection</h3>"
    "<div style=\"display:flex;align-items:center;gap:12px;margin-bottom:16px;\">"
    "<span class=\"status-dot ", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, statusClass, HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "\"></span>"
    "<span id=\"mqtt-status\" style=\"font-weight:600;\">", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, statusText, HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req,
    "</span></div>"
    "<div class=\"btn-row\">"
    "<button class=\"btn\" id=\"btn-connect\" onclick=\"mqttConnect()\" data-guest-hide>Connect</button>"
    "<button class=\"btn\" id=\"btn-disconnect\" onclick=\"mqttDisconnect()\" data-guest-hide>Disconnect</button>"
    "<button class=\"btn\" onclick=\"mqttRefresh()\">Refresh Status</button>"
    "</div></div>", HTTPD_RESP_USE_STRLEN);

  // Info blurb when disconnected
  if (!connected) {
    if (!gSettings.mqttHost.length()) {
      httpd_resp_send_chunk(req,
        "<div class='alert alert-warning' style='margin-top:12px;'>"
        "<strong>MQTT Not Configured</strong><br>"
        "No broker host is set. Configure your MQTT broker in "
        "<a href='/settings' style='color:var(--warning-fg);text-decoration:underline;'>Settings</a> "
        "to enable MQTT publishing. RAM is not allocated until MQTT is started."
        "</div>", HTTPD_RESP_USE_STRLEN);
    } else {
      httpd_resp_send_chunk(req,
        "<div class='alert alert-warning' style='margin-top:12px;'>"
        "<strong>MQTT Disconnected</strong><br>"
        "MQTT is configured but not currently connected. "
        "Click <em>Connect</em> above or enable Auto-Start in Settings. "
        "RAM for the MQTT client is only allocated when connected."
        "</div>", HTTPD_RESP_USE_STRLEN);
    }
  }

  // Configuration section
  httpd_resp_send_chunk(req,
    "<div class=\"settings-panel\" style=\"margin-top:16px;\">"
    "<h3>Configuration</h3>"
    "<table class=\"table\">"
    "<tr><td style=\"width:140px;font-weight:500;\">Host</td><td id=\"cfg-host\">", HTTPD_RESP_USE_STRLEN);
  
  // Show configured values (mask password)
  streamMqttConfigText(req, gSettings.mqttHost, guestView,
                       "<em style=\"color:var(--panel-fg);\">Not configured</em>");
  
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Port</td><td id=\"cfg-port\">", HTTPD_RESP_USE_STRLEN);
  
  char portBuf[8];
  snprintf(portBuf, sizeof(portBuf), "%d", gSettings.mqttPort);
  httpd_resp_send_chunk(req, portBuf, HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Security</td><td id=\"cfg-tls\">", HTTPD_RESP_USE_STRLEN);
  
  // Display TLS mode as a clear status
  if (gSettings.mqttTlsMode == 0) {
    httpd_resp_send_chunk(req, "<span style=\"color:var(--warning);\">None</span> <span style=\"font-size:0.85em;color:var(--panel-fg);\">(unencrypted)</span>", HTTPD_RESP_USE_STRLEN);
  } else if (gSettings.mqttTlsMode == 1) {
    httpd_resp_send_chunk(req, "<span style=\"color:var(--success);\">TLS</span> <span style=\"font-size:0.85em;color:var(--panel-fg);\">(encrypted, trusts any server)</span>", HTTPD_RESP_USE_STRLEN);
  } else if (gSettings.mqttTlsMode == 2) {
    httpd_resp_send_chunk(req, "<span style=\"color:var(--success);\">TLS + Verify</span>", HTTPD_RESP_USE_STRLEN);
    if (gSettings.mqttCaCertPath.length() > 0) {
      httpd_resp_send_chunk(req, " <span style=\"font-size:0.85em;color:var(--panel-fg);\">(", HTTPD_RESP_USE_STRLEN);
      httpd_resp_send_chunk(req, gSettings.mqttCaCertPath.c_str(), HTTPD_RESP_USE_STRLEN);
      httpd_resp_send_chunk(req, ")</span>", HTTPD_RESP_USE_STRLEN);
    }
  }
  
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Username</td><td id=\"cfg-user\">", HTTPD_RESP_USE_STRLEN);
  
  streamMqttConfigText(req, gSettings.mqttUser, guestView,
                       "<em style=\"color:var(--panel-fg);\">None</em>");
  
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Password</td><td id=\"cfg-pass\">", HTTPD_RESP_USE_STRLEN);
  
  if (gSettings.mqttPassword.length()) {
    httpd_resp_send_chunk(req, "********", HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send_chunk(req, "<em style=\"color:var(--panel-fg);\">None</em>", HTTPD_RESP_USE_STRLEN);
  }
  
  httpd_resp_send_chunk(req, "</td></tr><tr><td>Base Topic</td><td id=\"cfg-topic\">", HTTPD_RESP_USE_STRLEN);
  
  if (gSettings.mqttBaseTopic.length()) {
    httpd_resp_send_chunk(req, gSettings.mqttBaseTopic.c_str(), HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send_chunk(req, "<em style=\"color:var(--panel-fg);\">Default (hardwareone/&lt;mac&gt;)</em>", HTTPD_RESP_USE_STRLEN);
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
    "<p style=\"color:var(--panel-fg);font-size:0.85em;margin-bottom:12px;\">"
    "Configure which data is included in MQTT publications. Edit via Settings page.</p>"
    "<table class=\"table\">", HTTPD_RESP_USE_STRLEN);
  
  // System info toggle
  httpd_resp_send_chunk(req, "<tr><td>System Info</td><td>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, gSettings.mqttPublishSystem ? 
    "<span style=\"color:var(--success);\">✓ Enabled</span>" : 
    "<span style=\"color:var(--panel-fg);\">Disabled</span>", HTTPD_RESP_USE_STRLEN);
  
  // WiFi info toggle
  httpd_resp_send_chunk(req, "</td></tr><tr><td>WiFi Info</td><td>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, gSettings.mqttPublishWifi ? 
    "<span style=\"color:var(--success);\">✓ Enabled</span>" : 
    "<span style=\"color:var(--panel-fg);\">Disabled</span>", HTTPD_RESP_USE_STRLEN);

  streamMqttPublishedSensorRows(req);

  httpd_resp_send_chunk(req, "</td></tr></table></div>", HTTPD_RESP_USE_STRLEN);

  // External Sensors section (only show if subscriptions enabled)
  if (gSettings.mqttSubscribeExternal) {
    httpd_resp_send_chunk(req,
      "<div class=\"settings-panel\" style=\"margin-top:16px;\">"
      "<h3>External Sensors</h3>", HTTPD_RESP_USE_STRLEN);
    
    int sensorCount = getExternalSensorCount();
    if (sensorCount == 0) {
      httpd_resp_send_chunk(req,
        "<p style=\"color:var(--panel-fg);font-style:italic;\">No external sensor data received yet.</p>",
        HTTPD_RESP_USE_STRLEN);
    } else {
      httpd_resp_send_chunk(req, "<table class=\"table\">", HTTPD_RESP_USE_STRLEN);
      
      unsigned long now = millis();
      for (int i = 0; i < sensorCount && i < 20; i++) {
        String topic, name, value;
        unsigned long lastUpdate;
        if (getExternalSensor(i, topic, name, value, lastUpdate)) {
          unsigned long ageSec = (now - lastUpdate) / 1000;
          streamMqttExternalSensorRow(req, name, value, ageSec);
        }
      }
      
      httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
    }
    
    httpd_resp_send_chunk(req,
      "<p style=\"color:var(--panel-fg);font-size:0.85em;margin-top:8px;\">"
      "Subscribed topics: ", HTTPD_RESP_USE_STRLEN);
    if (gSettings.mqttSubscribeTopics.length() > 0) {
      streamHtmlEscaped(req, gSettings.mqttSubscribeTopics.c_str(),
                        strlen(gSettings.mqttSubscribeTopics.c_str()));
    } else {
      httpd_resp_send_chunk(req, "(none)", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "</p></div>", HTTPD_RESP_USE_STRLEN);
  }

  // Info section
  httpd_resp_send_chunk(req, R"HTML(
<div class="settings-panel" style="margin-top:16px;">
  <h3>About MQTT</h3>
  <p style="color:var(--panel-fg);font-size:0.9em;">
    MQTT enables this device to publish sensor data to a broker for integration with 
    home automation systems like Home Assistant. When connected, the device periodically 
    publishes a JSON blob containing cached sensor readings to the configured base topic.
  </p>
  <p style="color:var(--panel-fg);font-size:0.9em;margin-top:8px;">
    <strong>CLI Commands:</strong> <code>openmqtt</code>, <code>closemqtt</code>, <code>mqttstatus</code>, <code>mqttautostart</code>
  </p>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  // Close mqtt-body div
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

  // Script: connection status + auto-refresh
  httpd_resp_send_chunk(req, R"HTML(
<script>
// ── Connection status ────────────────────────────────────────────────────────
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
      var dot = hw.qs('.status-dot');
      var txt = hw.$('mqtt-status');
      if (d && d.connected) {
        dot.className = 'status-dot status-active';
        txt.textContent = 'Connected';
      } else {
        dot.className = 'status-dot status-inactive';
        txt.textContent = 'Disconnected';
      }
    })
    .catch(e => console.error('Refresh failed:', e));
}

document.addEventListener('DOMContentLoaded', mqttRefresh);

// Auto-refresh connection status every 5 s
setInterval(mqttRefresh, 5000);
</script>
)HTML", HTTPD_RESP_USE_STRLEN);
}

// Full page handler
static esp_err_t handleMqttPage(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  streamBeginHtml(req, "MQTT", false, ctx.user, "mqtt");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamMqttInner(req, isGuestUser(ctx.user));
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
  return ESP_OK;
}

// API endpoint for MQTT status (broker connection).
static esp_err_t handleMqttStatus(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  bool connected = isMqttConnected();

  char json[64];
  snprintf(json, sizeof(json), "{\"schema\":1,\"connected\":%s}",
           connected ? "true" : "false");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// Register handlers
void registerMqttHandlers(httpd_handle_t server) {
  static const httpd_uri_t mqttPage = {
    .uri = "/mqtt",
    .method = HTTP_GET,
    .handler = handleMqttPage,
    .user_ctx = NULL
  };
  
  static const httpd_uri_t mqttStatusApi = {
    .uri = "/api/mqtt/status",
    .method = HTTP_GET,
    .handler = handleMqttStatus,
    .user_ctx = NULL
  };
  
  httpd_register_uri_handler(server, &mqttPage);
  httpd_register_uri_handler(server, &mqttStatusApi);
}

#endif // ENABLE_HTTP_SERVER && ENABLE_MQTT
