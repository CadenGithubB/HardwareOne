#include "System_BuildConfig.h"

#if ENABLE_MAPS

#include "WebPage_Maps.h"
#include "WebServer_Utils.h"
#include "WebServer_Server.h"
#include "System_Maps.h"
#include "System_Debug.h"
#include "System_Mutex.h"
#include "System_Filesystem.h"
#include "System_User.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cstring>

// Forward declaration for auth function (defined in WebServer_Server.cpp)
bool isAuthed(httpd_req_t* req, String& outUser);

// Forward declarations for streaming functions (defined in WebServer_Server.cpp)
void streamPageHeader(httpd_req_t* req, const char* title);
void streamPageFooter(httpd_req_t* req);

// Thread-safe waypoint operations using existing mutex
extern SemaphoreHandle_t gJsonResponseMutex;
extern bool filesystemReady;
extern void logAuthAttempt(bool success, const char* path, const String& userTried, const String& ip, const String& reason);

// =============================================================================
// Maps Organize Helpers
// =============================================================================

bool isMapFileByMagic(const String& fullPath) {
  FsLockGuard guard("maps.magic");
  File f = LittleFS.open(fullPath, "r");
  if (!f) return false;
  char magic[4] = {0};
  size_t read = f.read((uint8_t*)magic, 4);
  f.close();
  return (read == 4 && memcmp(magic, "HWMP", 4) == 0);
}

static String mapBaseNameNoExt(const String& filename) {
  String base = filename;
  if (base.startsWith("/")) {
    int lastSlash = base.lastIndexOf('/');
    if (lastSlash >= 0) base = base.substring(lastSlash + 1);
  }
  if (base.endsWith(".hwmap")) base = base.substring(0, base.length() - 6);
  return base;
}

bool organizeMapFromAnyPath(const String& srcPath, String& outErr) {
  FsLockGuard guard("maps.organize.any");
  int lastSlash = srcPath.lastIndexOf('/');
  String fileName = (lastSlash >= 0) ? srcPath.substring(lastSlash + 1) : srcPath;
  
  String base = mapBaseNameNoExt(fileName);
  if (base.length() == 0) { outErr = "empty_base"; return false; }
  if (!LittleFS.exists(srcPath)) { outErr = "src_missing"; return false; }
  if (!isMapFileByMagic(srcPath)) { outErr = "not_map_file"; return false; }
  if (!LittleFS.exists("/maps")) {
    if (!LittleFS.mkdir("/maps")) { outErr = "maps_mkdir_failed"; return false; }
  }

  String dstDir = String("/maps/") + base;
  String dstMap = dstDir + "/" + base + ".hwmap";
  if (srcPath == dstMap) { outErr = "already_organized"; return false; }
  if (!LittleFS.exists(dstDir)) {
    if (!LittleFS.mkdir(dstDir)) { outErr = "mkdir_failed"; return false; }
  }
  if (LittleFS.exists(dstMap)) { outErr = "dst_exists"; return false; }
  if (!LittleFS.rename(srcPath.c_str(), dstMap.c_str())) { outErr = "rename_failed"; return false; }

  // Move legacy waypoints
  String legacyWp1 = String("/maps/waypoints_") + base + ".hwmap.json";
  String legacyWp2 = String("/maps/waypoints_") + base + ".json";
  String legacyWp = LittleFS.exists(legacyWp1) ? legacyWp1 : (LittleFS.exists(legacyWp2) ? legacyWp2 : "");
  if (legacyWp.length() > 0) {
    String wpFileName = legacyWp.substring(legacyWp.lastIndexOf('/') + 1);
    String dstWp = dstDir + "/" + wpFileName;
    if (!LittleFS.exists(dstWp)) LittleFS.rename(legacyWp.c_str(), dstWp.c_str());
  }
  return true;
}

static bool organizeOneMapAtRoot(const String& mapFileName, String& outErr) {
  FsLockGuard guard("maps.organize.root");
  if (mapFileName.indexOf('/') >= 0) { outErr = "invalid_name"; return false; }
  String base = mapBaseNameNoExt(mapFileName);
  if (base.length() == 0) { outErr = "empty_base"; return false; }
  String srcMap = String("/maps/") + mapFileName;
  if (!LittleFS.exists(srcMap)) { outErr = "src_missing"; return false; }
  if (!isMapFileByMagic(srcMap)) { outErr = "not_map_file"; return false; }

  String dstDir = String("/maps/") + base;
  String dstMap = dstDir + "/" + base + ".hwmap";
  if (!LittleFS.exists(dstDir)) {
    if (!LittleFS.mkdir(dstDir)) { outErr = "mkdir_failed"; return false; }
  }
  if (LittleFS.exists(dstMap)) { outErr = "dst_exists"; return false; }
  if (!LittleFS.rename(srcMap.c_str(), dstMap.c_str())) { outErr = "rename_failed"; return false; }

  String legacyWp = String("/maps/waypoints_") + mapFileName + ".json";
  if (LittleFS.exists(legacyWp)) {
    String wpFileName = legacyWp.substring(legacyWp.lastIndexOf('/') + 1);
    String dstWp = dstDir + "/" + wpFileName;
    if (LittleFS.exists(dstWp)) { outErr = "waypoints_dst_exists"; return false; }
    if (!LittleFS.rename(legacyWp.c_str(), dstWp.c_str())) { outErr = "waypoints_rename_failed"; return false; }
  }
  return true;
}

bool tryOrganizeLegacyWaypointsAtRoot(const String& wpFileName, String& outErr) {
  FsLockGuard guard("maps.organize.legacy_wp");
  if (!wpFileName.startsWith("waypoints_") || !wpFileName.endsWith(".json")) { outErr = "not_waypoints"; return false; }
  if (wpFileName.indexOf('/') >= 0) { outErr = "invalid_name"; return false; }
  String mapFileName = wpFileName.substring(10, wpFileName.length() - 5);
  String base = mapFileName.endsWith(".hwmap") ? mapFileName.substring(0, mapFileName.length() - 6) : mapFileName;
  if (base.length() == 0) { outErr = "empty_base"; return false; }
  
  String srcWp = String("/maps/") + wpFileName;
  if (!LittleFS.exists(srcWp)) { outErr = "src_missing"; return false; }
  String dstDir = String("/maps/") + base;
  String dstWp = dstDir + "/" + wpFileName;
  if (!LittleFS.exists(dstDir)) { outErr = "dst_dir_missing"; return false; }
  if (LittleFS.exists(dstWp)) { outErr = "dst_exists"; return false; }
  if (!LittleFS.rename(srcWp.c_str(), dstWp.c_str())) { outErr = "rename_failed"; return false; }
  return true;
}

static esp_err_t handleMapsOrganize(httpd_req_t* req) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  ctx.path = req ? req->uri : "/api/maps/organize";
  getClientIP(req, ctx.ip);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  logAuthAttempt(true, req->uri, ctx.user, ctx.ip, "");

  if (!filesystemReady) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"filesystem_not_ready\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  FsLockGuard fsGuard("maps.organize.handler");

  File dir = LittleFS.open("/maps");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"maps_dir_missing\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  int moved = 0, skipped = 0, failed = 0;
  String details = "";

  File entry = dir.openNextFile();
  while (entry) {
    String full = String(entry.name());
    bool isDir = entry.isDirectory();
    entry.close();

    if (!isDir) {
      String fn = full;
      if (fn.startsWith("/maps/")) fn = fn.substring(6);
      if (fn.startsWith("/")) fn = fn.substring(1);
      if (fn.indexOf('/') != -1) { entry = dir.openNextFile(); continue; }

      bool handled = false;
      bool isMapByExt = fn.endsWith(".hwmap");
      bool isMapByMagic = (!isMapByExt && !fn.endsWith(".json")) ? isMapFileByMagic(full) : false;
      
      if (isMapByExt || isMapByMagic) {
        handled = true;
        String err;
        if (organizeOneMapAtRoot(fn, err)) { moved++; }
        else {
          failed++;
          if (details.length() < 1800) {
            String esc = fn; esc.replace("\\", "\\\\"); esc.replace("\"", "\\\"");
            String escErr = err; escErr.replace("\\", "\\\\"); escErr.replace("\"", "\\\"");
            details += String(details.length() ? "," : "") + "{\"file\":\"" + esc + "\",\"error\":\"" + escErr + "\"}";
          }
        }
      } else if (fn.startsWith("waypoints_") && fn.endsWith(".json")) {
        handled = true;
        String err;
        if (tryOrganizeLegacyWaypointsAtRoot(fn, err)) { moved++; }
        else {
          failed++;
          if (details.length() < 1800) {
            String esc = fn; esc.replace("\\", "\\\\"); esc.replace("\"", "\\\"");
            String escErr = err; escErr.replace("\\", "\\\\"); escErr.replace("\"", "\\\"");
            details += String(details.length() ? "," : "") + "{\"file\":\"" + esc + "\",\"error\":\"" + escErr + "\"}";
          }
        }
      }
      if (!handled) skipped++;
    } else {
      skipped++;
    }
    entry = dir.openNextFile();
  }
  dir.close();

  httpd_resp_set_type(req, "application/json");
  String json = "{\"success\":true,\"moved\":" + String(moved) + ",\"skipped\":" + String(skipped) + ",\"failed\":" + String(failed) + ",\"failures\":[" + details + "]}";
  httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// =============================================================================
// Map Select API
// =============================================================================

esp_err_t handleMapSelectAPI(httpd_req_t* req) {
  String user;
  if (!isAuthed(req, user)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Authentication required\"}");
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/json");

  char query[256];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Missing query\"}");
    return ESP_OK;
  }

  char filepathRaw[128] = {0};
  if (httpd_query_key_value(query, "file", filepathRaw, sizeof(filepathRaw)) != ESP_OK) {
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Missing file parameter\"}");
    return ESP_OK;
  }

  // URL decode (httpd_query_key_value does not percent-decode)
  char filepath[128] = {0};
  {
    size_t wi = 0;
    for (size_t ri = 0; filepathRaw[ri] != '\0' && wi + 1 < sizeof(filepath); ri++) {
      char c = filepathRaw[ri];
      if (c == '+') {
        filepath[wi++] = ' ';
        continue;
      }
      if (c == '%' && filepathRaw[ri + 1] && filepathRaw[ri + 2]) {
        char hex[3] = { filepathRaw[ri + 1], filepathRaw[ri + 2], 0 };
        filepath[wi++] = (char)strtol(hex, nullptr, 16);
        ri += 2;
        continue;
      }
      filepath[wi++] = c;
    }
    filepath[wi] = '\0';
  }

  if (filepath[0] != '/' || strncmp(filepath, "/maps/", 6) != 0) {
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid file path\"}");
    return ESP_OK;
  }

  const char* ext = strrchr(filepath, '.');
  if (!ext || strcmp(ext, ".hwmap") != 0) {
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid file type\"}");
    return ESP_OK;
  }

  if (!MapCore::loadMapFile(filepath)) {
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Failed to load map\"}");
    return ESP_OK;
  }

  const LoadedMap& map = MapCore::getCurrentMap();
  String json = "{\"success\":true,\"mapName\":\"" + String(map.filename) + "\"}";
  httpd_resp_sendstr(req, json.c_str());
  return ESP_OK;
}

// =============================================================================
// Map Features API
// =============================================================================

esp_err_t handleMapFeaturesAPI(httpd_req_t* req) {
  String user;
  if (!isAuthed(req, user)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "{\"error\":\"Authentication required\"}");
    return ESP_OK;
  }
  
  httpd_resp_set_type(req, "application/json");
  
  if (!MapCore::hasValidMap()) {
    httpd_resp_sendstr(req, "{\"error\":\"No map loaded\"}");
    return ESP_OK;
  }
  
  const LoadedMap& map = MapCore::getCurrentMap();
  
  // Build JSON response
  String json = "{";
  json += "\"mapName\":\"" + String(map.filename) + "\",";
  json += "\"hasNames\":" + String(map.nameCount > 0 ? "true" : "false") + ",";
  json += "\"featureCount\":" + String(map.header.featureCount) + ",";
  
  if (map.nameCount > 0) {
    json += "\"nameCount\":" + String(map.nameCount) + ",";
    json += "\"names\":[";
    bool firstName = true;
    for (int i = 0; i < map.nameCount; i++) {
      if (!firstName) json += ",";
      firstName = false;
      
      // Escape quotes in name
      String name = String(map.names[i].name);
      name.replace("\"", "\\\"");
      json += "\"" + name + "\"";
    }
    json += "]";
  } else {
    json += "\"nameCount\":0";
  }
  
  json += "}";
  
  httpd_resp_sendstr(req, json.c_str());
  return ESP_OK;
}

// =============================================================================
// GPS Tracks API
// =============================================================================

esp_err_t handleGPSTracksAPI(httpd_req_t* req) {
  String user;
  if (!isAuthed(req, user)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Not authenticated\"}");
    return ESP_OK;
  }

  char query[256];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    // Check for live track request
    char liveParam[8] = {0};
    if (httpd_query_key_value(query, "live", liveParam, sizeof(liveParam)) == ESP_OK) {
      httpd_resp_set_type(req, "application/json");
      
      bool isLive = GPSTrackManager::isLiveTracking();
      int pointCount = GPSTrackManager::getPointCount();
      const GPSTrackStats& stats = GPSTrackManager::getStats();
      
      char header[384];
      snprintf(header, sizeof(header), 
               "{\"live\":%s,\"count\":%d,\"distance\":%.1f,\"duration\":%.0f,\"speed\":%.2f,\"lastUpdate\":%lu,\"points\":[",
               isLive ? "true" : "false", pointCount,
               stats.totalDistanceM, stats.durationSec, stats.avgSpeedMps,
               (unsigned long)GPSTrackManager::getLastUpdateTime());
      httpd_resp_sendstr_chunk(req, header);
      
      const GPSTrackPoint* points = GPSTrackManager::getPoints();
      int startIdx = (pointCount > 500) ? (pointCount - 500) : 0;
      
      for (int i = startIdx; i < pointCount; i++) {
        char pointJson[64];
        snprintf(pointJson, sizeof(pointJson), "%s{\"lat\":%.6f,\"lon\":%.6f}",
                 (i == startIdx) ? "" : ",", points[i].lat, points[i].lon);
        httpd_resp_sendstr_chunk(req, pointJson);
      }
      
      httpd_resp_sendstr_chunk(req, "]}");
      httpd_resp_sendstr_chunk(req, NULL);
      return ESP_OK;
    }
    
    char filepath[128] = {0};
    if (httpd_query_key_value(query, "file", filepath, sizeof(filepath)) == ESP_OK) {
      if (filepath[0] != '/') {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid file path\"}");
        return ESP_OK;
      }

      String errorMsg;
      if (!GPSTrackManager::loadTrack(filepath, errorMsg)) {
        httpd_resp_set_type(req, "application/json");
        char errJson[256];
        snprintf(errJson, sizeof(errJson), "{\"error\":\"%s\"}", errorMsg.c_str());
        httpd_resp_sendstr(req, errJson);
        return ESP_OK;
      }

      float coverage;
      TrackValidation validation = GPSTrackManager::validateTrack(coverage);
      const char* validMsg = GPSTrackManager::getValidationMessage(validation, coverage);

      httpd_resp_set_type(req, "application/json");
      
      char header[256];
      snprintf(header, sizeof(header), 
               "{\"success\":true,\"validation\":\"%s\",\"coverage\":%.1f,\"points\":[",
               (validation == TRACK_OUT_OF_BOUNDS) ? "out_of_bounds" :
               (validation == TRACK_PARTIAL) ? "partial" : "valid",
               coverage);
      httpd_resp_sendstr_chunk(req, header);

      const GPSTrackPoint* points = GPSTrackManager::getPoints();
      int pointCount = GPSTrackManager::getPointCount();
      
      for (int i = 0; i < pointCount; i++) {
        char pointJson[128];
        snprintf(pointJson, sizeof(pointJson), "%s{\"lat\":%.6f,\"lon\":%.6f}",
                 (i == 0) ? "" : ",", points[i].lat, points[i].lon);
        httpd_resp_sendstr_chunk(req, pointJson);
      }

      char footer[256];
      snprintf(footer, sizeof(footer), "],\"count\":%d,\"message\":\"%s\"}", 
               pointCount, validMsg);
      httpd_resp_sendstr_chunk(req, footer);
      httpd_resp_sendstr_chunk(req, NULL);
      return ESP_OK;
    }
  }
  
  // No query params - list available GPS log files
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr_chunk(req, "{\"success\":true,\"files\":[");

  fsLock("gps.tracks.list");
  
  // Scan /logs and /logs/tracks directories
  const char* dirs[] = {"/logs", "/logs/tracks"};
  bool firstFile = true;
  
  for (int d = 0; d < 2; d++) {
    File root = LittleFS.open(dirs[d]);
    if (!root || !root.isDirectory()) continue;
    
    File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        bool hasGPS = false;
        File check = LittleFS.open(file.path(), "r");
        if (check) {
          for (int i = 0; i < 15 && check.available(); i++) {
            String line = check.readStringUntil('\n');
            // Check for both sensor log format and CSV format
            if (line.indexOf("gps:") >= 0 || 
                (line.length() > 10 && line.charAt(0) != '#' && line.indexOf(',') > 0)) {
              hasGPS = true;
              break;
            }
          }
          check.close();
        }

        if (hasGPS) {
          char fileJson[256];
          snprintf(fileJson, sizeof(fileJson), "%s{\"path\":\"%s\",\"size\":%lu}",
                   firstFile ? "" : ",", file.path(), (unsigned long)file.size());
          httpd_resp_sendstr_chunk(req, fileJson);
          firstFile = false;
        }
      }
      file = root.openNextFile();
    }
  }
  fsUnlock();

  httpd_resp_sendstr_chunk(req, "]}");
  httpd_resp_sendstr_chunk(req, NULL);

  return ESP_OK;
}

// =============================================================================
// Waypoints Page (merged from WebPage_Waypoints.cpp)
// =============================================================================

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
    "function escapeHtml(s) {\n"
    "  s = (s === null || s === undefined) ? '' : String(s);\n"
    "  return s.replace(/&/g, '&amp;')\n"
    "          .replace(/</g, '&lt;')\n"
    "          .replace(/>/g, '&gt;')\n"
    "          .replace(/\"/g, '&quot;')\n"
    "          .replace(/'/g, '&#39;');\n"
    "}\n"
    "function loadWaypoints() {\n"
    "  fetch('/api/waypoints', {credentials: 'include'})\n"
    "    .then(function(r) { return r.json(); })\n"
    "    .then(function(data) {\n"
    "      var status = document.getElementById('waypoint-status');\n"
    "      var list = document.getElementById('waypoint-list');\n"
    "      if (!data.success) {\n"
    "        status.innerHTML = '<strong>Error:</strong> ' + escapeHtml(data.error || 'Failed to load waypoints');\n"
    "        status.style.background = '#ffebee';\n"
    "        return;\n"
    "      }\n"
    "      status.innerHTML = '<strong>Map:</strong> ' + escapeHtml(data.mapName || 'None') + ' | <strong>Waypoints:</strong> ' + data.count + '/' + data.max;\n"
    "      status.style.background = '#e8f5e9';\n"
    "      if (data.waypoints && data.waypoints.length > 0) {\n"
    "        var html = '<table style=\"width: 100%; border-collapse: collapse;\">';\n"
    "        html += '<tr style=\"background: #f5f5f5;\"><th style=\"padding: 8px; text-align: left;\">Name</th><th>Latitude</th><th>Longitude</th><th>Files</th><th>Target</th><th>Actions</th></tr>';\n"
    "        data.waypoints.forEach(function(wp, idx) {\n"
    "          var isTarget = (idx === data.target);\n"
    "          html += '<tr style=\"border-bottom: 1px solid #ddd;' + (isTarget ? ' background: #fff3e0;' : '') + '\">';\n"
    "          html += '<td style=\"padding: 8px;\">' + escapeHtml(wp.name) + (isTarget ? ' ' : '') + '</td>';\n"
    "          html += '<td style=\"text-align: center;\">' + wp.lat.toFixed(6) + '</td>';\n"
    "          html += '<td style=\"text-align: center;\">' + wp.lon.toFixed(6) + '</td>';\n"
    "          html += '<td style=\"text-align: center;\">';\n"
    "          if (wp.fileCount > 0) {\n"
    "            html += '<button onclick=\"viewFiles(' + idx + ')\" style=\"padding: 4px 8px; background: #9C27B0; color: white; border: none; border-radius: 3px; cursor: pointer;\"> ' + wp.fileCount + '</button>';\n"
    "          } else {\n"
    "            html += '<span style=\"color: #999;\">-</span>';\n"
    "          }\n"
    "          html += '</td>';\n"
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
    "      document.getElementById('waypoint-status').innerHTML = '<strong>Error:</strong> ' + escapeHtml(e.message);\n"
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
    "function viewFiles(idx) {\n"
    "  fetch('/api/waypoints', {credentials: 'include'})\n"
    "    .then(function(r) { return r.json(); })\n"
    "    .then(function(data) {\n"
    "      if (!data.success || !data.waypoints || !data.waypoints[idx]) {\n"
    "        alert('Could not load files');\n"
    "        return;\n"
    "      }\n"
    "      var wp = data.waypoints[idx];\n"
    "      if (!wp.files || wp.files.length === 0) {\n"
    "        alert('No files for this waypoint');\n"
    "        return;\n"
    "      }\n"
    "      var html = '<div style=\"padding:15px;\"><h3>Files for ' + escapeHtml(wp.name || '') + '</h3>';\n"
    "      wp.files.forEach(function(file, i) {\n"
    "        var ext = file.split('.').pop().toLowerCase();\n"
    "        var icon = (ext === 'jpg' || ext === 'jpeg' || ext === 'png' || ext === 'gif') ? 'img' : 'doc';\n"
    "        html += '<div style=\"margin:10px 0;padding:10px;background:#f5f5f5;border-radius:4px;display:flex;align-items:center;gap:10px;\">';\n"
    "        html += '<span>' + icon + '</span>';\n"
    "        html += '<span style=\"flex:1;font-family:monospace;font-size:0.9em;\">' + escapeHtml(file) + '</span>';\n"
    "        html += '<a href=\"/api/files/view?name=' + encodeURIComponent(file) + '\" target=\"_blank\" style=\"padding:6px 12px;background:#4CAF50;color:white;text-decoration:none;border-radius:4px;\">View</a>';\n"
    "        html += '</div>';\n"
    "      });\n"
    "      html += '<button onclick=\"this.parentElement.parentElement.remove()\" style=\"margin-top:10px;padding:8px 16px;background:#666;color:white;border:none;border-radius:4px;cursor:pointer;\">Close</button></div>';\n"
    "      var modal = document.createElement('div');\n"
    "      modal.style.cssText = 'position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.5);display:flex;align-items:center;justify-content:center;z-index:1000;';\n"
    "      modal.innerHTML = '<div style=\"background:white;border-radius:8px;max-width:500px;max-height:80vh;overflow:auto;\">' + html + '</div>';\n"
    "      modal.onclick = function(e) { if (e.target === modal) modal.remove(); };\n"
    "      document.body.appendChild(modal);\n"
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

// =============================================================================
// Maps Page Handler
// =============================================================================

// Forward declarations for auth and streaming
extern bool tgRequireAuth(AuthContext& ctx);
extern void getClientIP(httpd_req_t* req, String& out);
extern void streamPageWithContent(httpd_req_t* req, const String& activePage, const String& username, void (*contentStreamer)(httpd_req_t*));
extern bool isAuthed(httpd_req_t* req, String& outUser);
extern void streamBeginHtml(httpd_req_t* req, const char* title, bool isPublic, const String& username, const String& activePage);
extern void streamEndHtml(httpd_req_t* req);

static void streamMapsContent(httpd_req_t* req) {
  String u;
  isAuthed(req, u);
  streamBeginHtml(req, "Maps", false, u, "maps");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamMapsInner(req);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

static esp_err_t handleMapsPage(httpd_req_t* req) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  ctx.path = req ? req->uri : "/maps";
  getClientIP(req, ctx.ip);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  logAuthAttempt(true, req->uri, ctx.user, ctx.ip, "");

  streamPageWithContent(req, String("maps"), ctx.user, streamMapsContent);
  return ESP_OK;
}

// =============================================================================
// Register Maps Handlers
// =============================================================================

void registerMapsHandlers(httpd_handle_t server) {
  static httpd_uri_t mapsPage = { .uri = "/maps", .method = HTTP_GET, .handler = handleMapsPage, .user_ctx = NULL };
  static httpd_uri_t mapFeaturesGet = { .uri = "/api/maps/features", .method = HTTP_GET, .handler = handleMapFeaturesAPI, .user_ctx = NULL };
  static httpd_uri_t mapSelectGet = { .uri = "/api/maps/select", .method = HTTP_GET, .handler = handleMapSelectAPI, .user_ctx = NULL };
  static httpd_uri_t mapsOrganizePost = { .uri = "/api/maps/organize", .method = HTTP_POST, .handler = handleMapsOrganize, .user_ctx = NULL };
  static httpd_uri_t waypointsGet = { .uri = "/api/waypoints", .method = HTTP_GET, .handler = handleWaypointsAPI, .user_ctx = NULL };
  static httpd_uri_t waypointsPost = { .uri = "/api/waypoints", .method = HTTP_POST, .handler = handleWaypointsAPI, .user_ctx = NULL };
  static httpd_uri_t gpsTracksGet = { .uri = "/api/gps/tracks", .method = HTTP_GET, .handler = handleGPSTracksAPI, .user_ctx = NULL };
  
  httpd_register_uri_handler(server, &mapsPage);
  httpd_register_uri_handler(server, &mapFeaturesGet);
  httpd_register_uri_handler(server, &mapSelectGet);
  httpd_register_uri_handler(server, &mapsOrganizePost);
  httpd_register_uri_handler(server, &waypointsGet);
  httpd_register_uri_handler(server, &waypointsPost);
  httpd_register_uri_handler(server, &gpsTracksGet);
}

// =============================================================================
// Waypoints API (merged from WebPage_Waypoints.cpp)
// =============================================================================

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
    StaticJsonDocument<8192> doc;
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
          wpObj["notes"] = wp->notes;
          wpObj["fileCount"] = wp->fileCount;
          if (wp->fileCount > 0) {
            JsonArray files = wpObj["files"].to<JsonArray>();
            for (int j = 0; j < wp->fileCount && j < MAX_WAYPOINT_FILES; j++) {
              if (wp->files[j][0]) {
                files.add(wp->files[j]);
              }
            }
          }
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
    String action, name, notes, latStr, lonStr, indexStr;
    
    // Parse form fields
    int pos = 0;
    while (pos < (int)data.length()) {
      int amp = data.indexOf('&', pos);
      if (amp == -1) amp = data.length();
      String pair = data.substring(pos, amp);
      int eq = pair.indexOf('=');
      if (eq > 0) {
        String key = pair.substring(0, eq);
        String value = pair.substring(eq + 1);
        value.replace("+", " ");
        // URL decode
        for (int i = 0; i < (int)value.length(); i++) {
          if (value[i] == '%' && i + 2 < (int)value.length()) {
            char hex[3] = {value[i+1], value[i+2], 0};
            value[i] = (char)strtol(hex, NULL, 16);
            value.remove(i+1, 2);
          }
        }
        if (key == "action") action = value;
        else if (key == "name") name = value;
        else if (key == "notes") notes = value;
        else if (key == "lat") latStr = value;
        else if (key == "lon") lonStr = value;
        else if (key == "index") indexStr = value;
      }
      pos = amp + 1;
    }
    
    StaticJsonDocument<2048> doc;
    
    if (action == "add") {
      float lat = latStr.toFloat();
      float lon = lonStr.toFloat();
      if (name.length() == 0 || lat == 0.0f || lon == 0.0f) {
        doc["success"] = false;
        doc["error"] = "Invalid parameters";
      } else {
        int idx = WaypointManager::addWaypoint(lat, lon, name.c_str(), notes.c_str());
        if (idx >= 0) {
          doc["success"] = true;
          doc["index"] = idx;
        } else {
          doc["success"] = false;
          doc["error"] = "No free slots";
        }
      }
    } else if (action == "rename") {
      int idx = indexStr.toInt();
      if (name.length() == 0) {
        doc["success"] = false;
        doc["error"] = "Missing name";
      } else if (WaypointManager::setName(idx, name.c_str())) {
        doc["success"] = true;
      } else {
        doc["success"] = false;
        doc["error"] = "Invalid index";
      }
    } else if (action == "set_notes") {
      int idx = indexStr.toInt();
      if (WaypointManager::setNotes(idx, notes.c_str())) {
        doc["success"] = true;
      } else {
        doc["success"] = false;
        doc["error"] = "Invalid index";
      }
    } else if (action == "clear_all") {
      WaypointManager::clearAll();
      doc["success"] = true;
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

#endif // ENABLE_MAPS
