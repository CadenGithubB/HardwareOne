#include <Arduino.h>
#include <esp_http_server.h>
#include "WebPage_Waypoints.h"
#include "WebServer_Server.h"
#include "System_GPSMapRenderer.h"
#include "System_Debug.h"
#include "System_Mutex.h"
#include <ArduinoJson.h>

// Forward declaration for auth function (defined in WebCore_Server.cpp)
bool isAuthed(httpd_req_t* req, String& outUser);

// Forward declarations for streaming functions (defined in WebCore_Server.cpp)
void streamPageHeader(httpd_req_t* req, const char* title);
void streamPageFooter(httpd_req_t* req);

// Thread-safe waypoint operations using existing mutex
extern SemaphoreHandle_t gJsonResponseMutex;

esp_err_t handleWaypointsPage(httpd_req_t* req) {
  String user;
  if (!isAuthed(req, user)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "Authentication required", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  streamPageHeader(req, "Waypoints");
  
  httpd_resp_send_chunk(req,
    "<div class='container'>\n"
    "  <h1>Waypoint Management</h1>\n"
    "  <div id='waypoint-status' style='margin: 10px 0; padding: 10px; background: #f0f0f0; border-radius: 4px;'>\n"
    "    Loading waypoints...\n"
    "  </div>\n"
    "  <div style='margin: 20px 0;'>\n"
    "    <h2>Add Waypoint</h2>\n"
    "    <div style='display: grid; gap: 10px; max-width: 400px;'>\n"
    "      <input type='text' id='wp-name' placeholder='Waypoint Name' maxlength='11' />\n"
    "      <input type='number' id='wp-lat' placeholder='Latitude' step='0.000001' />\n"
    "      <input type='number' id='wp-lon' placeholder='Longitude' step='0.000001' />\n"
    "      <button onclick='addWaypoint()' style='padding: 10px; background: #4CAF50; color: white; border: none; border-radius: 4px; cursor: pointer;'>Add Waypoint</button>\n"
    "    </div>\n"
    "  </div>\n"
    "  <div>\n"
    "    <h2>Current Waypoints</h2>\n"
    "    <div id='waypoint-list' style='margin: 10px 0;'></div>\n"
    "  </div>\n"
    "</div>\n"
    "<script>\n"
    "function loadWaypoints() {\n"
    "  fetch('/api/waypoints', {credentials: 'include'})\n"
    "    .then(function(r) { return r.json(); })\n"
    "    .then(function(data) {\n"
    "      var status = document.getElementById('waypoint-status');\n"
    "      var list = document.getElementById('waypoint-list');\n"
    "      if (!data.success) {\n"
    "        status.innerHTML = '<strong>Error:</strong> ' + (data.error || 'Failed to load waypoints');\n"
    "        status.style.background = '#ffebee';\n"
    "        return;\n"
    "      }\n"
    "      status.innerHTML = '<strong>Map:</strong> ' + (data.mapName || 'None') + ' | <strong>Waypoints:</strong> ' + data.count + '/' + data.max;\n"
    "      status.style.background = '#e8f5e9';\n"
    "      if (data.waypoints && data.waypoints.length > 0) {\n"
    "        var html = '<table style=\"width: 100%; border-collapse: collapse;\">';\n"
    "        html += '<tr style=\"background: #f5f5f5;\"><th style=\"padding: 8px; text-align: left;\">Name</th><th>Latitude</th><th>Longitude</th><th>Target</th><th>Actions</th></tr>';\n"
    "        data.waypoints.forEach(function(wp, idx) {\n"
    "          var isTarget = (idx === data.target);\n"
    "          html += '<tr style=\"border-bottom: 1px solid #ddd;' + (isTarget ? ' background: #fff3e0;' : '') + '\">';\n"
    "          html += '<td style=\"padding: 8px;\">' + wp.name + (isTarget ? ' ⭐' : '') + '</td>';\n"
    "          html += '<td style=\"text-align: center;\">' + wp.lat.toFixed(6) + '</td>';\n"
    "          html += '<td style=\"text-align: center;\">' + wp.lon.toFixed(6) + '</td>';\n"
    "          html += '<td style=\"text-align: center;\">';\n"
    "          if (!isTarget) {\n"
    "            html += '<button onclick=\"gotoWaypoint(' + idx + ')\" style=\"padding: 4px 8px; background: #2196F3; color: white; border: none; border-radius: 3px; cursor: pointer;\">Set Target</button>';\n"
    "          } else {\n"
    "            html += '<button onclick=\"clearTarget()\" style=\"padding: 4px 8px; background: #FF9800; color: white; border: none; border-radius: 3px; cursor: pointer;\">Clear</button>';\n"
    "          }\n"
    "          html += '</td>';\n"
    "          html += '<td style=\"text-align: center;\"><button onclick=\"deleteWaypoint(' + idx + ')\" style=\"padding: 4px 8px; background: #f44336; color: white; border: none; border-radius: 3px; cursor: pointer;\">Delete</button></td>';\n"
    "          html += '</tr>';\n"
    "        });\n"
    "        html += '</table>';\n"
    "        list.innerHTML = html;\n"
    "      } else {\n"
    "        list.innerHTML = '<p style=\"color: #666;\">No waypoints for this map.</p>';\n"
    "      }\n"
    "    })\n"
    "    .catch(function(e) {\n"
    "      document.getElementById('waypoint-status').innerHTML = '<strong>Error:</strong> ' + e.message;\n"
    "      document.getElementById('waypoint-status').style.background = '#ffebee';\n"
    "    });\n"
    "}\n"
    "function addWaypoint() {\n"
    "  var name = document.getElementById('wp-name').value.trim();\n"
    "  var lat = parseFloat(document.getElementById('wp-lat').value);\n"
    "  var lon = parseFloat(document.getElementById('wp-lon').value);\n"
    "  if (!name || isNaN(lat) || isNaN(lon)) {\n"
    "    alert('Please fill in all fields with valid values');\n"
    "    return;\n"
    "  }\n"
    "  var formData = new FormData();\n"
    "  formData.append('action', 'add');\n"
    "  formData.append('name', name);\n"
    "  formData.append('lat', lat);\n"
    "  formData.append('lon', lon);\n"
    "  fetch('/api/waypoints', {method: 'POST', body: formData, credentials: 'include'})\n"
    "    .then(function(r) { return r.json(); })\n"
    "    .then(function(data) {\n"
    "      if (data.success) {\n"
    "        document.getElementById('wp-name').value = '';\n"
    "        document.getElementById('wp-lat').value = '';\n"
    "        document.getElementById('wp-lon').value = '';\n"
    "        loadWaypoints();\n"
    "      } else {\n"
    "        alert('Error: ' + (data.error || 'Failed to add waypoint'));\n"
    "      }\n"
    "    })\n"
    "    .catch(function(e) { alert('Error: ' + e.message); });\n"
    "}\n"
    "function deleteWaypoint(idx) {\n"
    "  if (!confirm('Delete this waypoint?')) return;\n"
    "  var formData = new FormData();\n"
    "  formData.append('action', 'delete');\n"
    "  formData.append('index', idx);\n"
    "  fetch('/api/waypoints', {method: 'POST', body: formData, credentials: 'include'})\n"
    "    .then(function(r) { return r.json(); })\n"
    "    .then(function(data) {\n"
    "      if (data.success) loadWaypoints();\n"
    "      else alert('Error: ' + (data.error || 'Failed to delete waypoint'));\n"
    "    })\n"
    "    .catch(function(e) { alert('Error: ' + e.message); });\n"
    "}\n"
    "function gotoWaypoint(idx) {\n"
    "  var formData = new FormData();\n"
    "  formData.append('action', 'goto');\n"
    "  formData.append('index', idx);\n"
    "  fetch('/api/waypoints', {method: 'POST', body: formData, credentials: 'include'})\n"
    "    .then(function(r) { return r.json(); })\n"
    "    .then(function(data) {\n"
    "      if (data.success) loadWaypoints();\n"
    "      else alert('Error: ' + (data.error || 'Failed to set target'));\n"
    "    })\n"
    "    .catch(function(e) { alert('Error: ' + e.message); });\n"
    "}\n"
    "function clearTarget() {\n"
    "  var formData = new FormData();\n"
    "  formData.append('action', 'clear');\n"
    "  fetch('/api/waypoints', {method: 'POST', body: formData, credentials: 'include'})\n"
    "    .then(function(r) { return r.json(); })\n"
    "    .then(function(data) {\n"
    "      if (data.success) loadWaypoints();\n"
    "      else alert('Error: ' + (data.error || 'Failed to clear target'));\n"
    "    })\n"
    "    .catch(function(e) { alert('Error: ' + e.message); });\n"
    "}\n"
    "loadWaypoints();\n"
    "setInterval(loadWaypoints, 5000);\n"
    "</script>\n", HTTPD_RESP_USE_STRLEN);
  
  streamPageFooter(req);
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

esp_err_t handleWaypointsAPI(httpd_req_t* req) {
  String user;
  if (!isAuthed(req, user)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Authentication required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  // Thread-safe JSON response
  if (!gJsonResponseMutex || xSemaphoreTake(gJsonResponseMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Mutex timeout\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  if (req->method == HTTP_GET) {
    // Get waypoints list
    JsonDocument doc;
    const LoadedMap& map = MapCore::getCurrentMap();
    
    if (!map.valid) {
      doc["success"] = false;
      doc["error"] = "No map loaded";
    } else {
      doc["success"] = true;
      doc["mapName"] = map.filename;
      doc["count"] = WaypointManager::getActiveCount();
      doc["max"] = MAX_WAYPOINTS;
      doc["target"] = WaypointManager::getSelectedTarget();
      
      JsonArray waypoints = doc["waypoints"].to<JsonArray>();
      for (int i = 0; i < MAX_WAYPOINTS; i++) {
        const Waypoint* wp = WaypointManager::getWaypoint(i);
        if (wp) {
          JsonObject wpObj = waypoints.add<JsonObject>();
          wpObj["name"] = wp->name;
          wpObj["lat"] = wp->lat;
          wpObj["lon"] = wp->lon;
        }
      }
    }
    
    String response;
    serializeJson(doc, response);
    xSemaphoreGive(gJsonResponseMutex);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response.c_str(), response.length());
    
  } else if (req->method == HTTP_POST) {
    // Parse form data
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
      xSemaphoreGive(gJsonResponseMutex);
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_send(req, "{\"success\":false,\"error\":\"No data\"}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    buf[ret] = '\0';
    
    String data(buf);
    String action, name, latStr, lonStr, indexStr;
    
    // Parse form fields
    int pos = 0;
    while (pos < data.length()) {
      int amp = data.indexOf('&', pos);
      if (amp == -1) amp = data.length();
      String pair = data.substring(pos, amp);
      int eq = pair.indexOf('=');
      if (eq > 0) {
        String key = pair.substring(0, eq);
        String value = pair.substring(eq + 1);
        value.replace("+", " ");
        // URL decode
        for (int i = 0; i < value.length(); i++) {
          if (value[i] == '%' && i + 2 < value.length()) {
            char hex[3] = {value[i+1], value[i+2], 0};
            value[i] = (char)strtol(hex, NULL, 16);
            value.remove(i+1, 2);
          }
        }
        if (key == "action") action = value;
        else if (key == "name") name = value;
        else if (key == "lat") latStr = value;
        else if (key == "lon") lonStr = value;
        else if (key == "index") indexStr = value;
      }
      pos = amp + 1;
    }
    
    JsonDocument doc;
    
    if (action == "add") {
      float lat = latStr.toFloat();
      float lon = lonStr.toFloat();
      if (name.length() == 0 || lat == 0.0f || lon == 0.0f) {
        doc["success"] = false;
        doc["error"] = "Invalid parameters";
      } else {
        int idx = WaypointManager::addWaypoint(lat, lon, name.c_str());
        if (idx >= 0) {
          doc["success"] = true;
          doc["index"] = idx;
        } else {
          doc["success"] = false;
          doc["error"] = "No free slots";
        }
      }
    } else if (action == "delete") {
      int idx = indexStr.toInt();
      if (WaypointManager::deleteWaypoint(idx)) {
        doc["success"] = true;
      } else {
        doc["success"] = false;
        doc["error"] = "Invalid index";
      }
    } else if (action == "goto") {
      int idx = indexStr.toInt();
      const Waypoint* wp = WaypointManager::getWaypoint(idx);
      if (wp) {
        WaypointManager::selectTarget(idx);
        doc["success"] = true;
      } else {
        doc["success"] = false;
        doc["error"] = "Invalid index";
      }
    } else if (action == "clear") {
      WaypointManager::selectTarget(-1);
      doc["success"] = true;
    } else {
      doc["success"] = false;
      doc["error"] = "Unknown action";
    }
    
    String response;
    serializeJson(doc, response);
    xSemaphoreGive(gJsonResponseMutex);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response.c_str(), response.length());
  }
  
  return ESP_OK;
}
