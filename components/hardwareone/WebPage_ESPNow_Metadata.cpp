#include "System_BuildConfig.h"

#if ENABLE_WEB_ESPNOW && ENABLE_ESPNOW

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include "System_Debug.h"
#include "System_User.h"
#include "System_ESPNow.h"
#include "System_Utils.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"
#include "System_MemUtil.h"

// External declarations
extern MeshPeerMeta* gMeshPeerMeta;
extern int gMeshPeerSlots;
extern bool filesystemReady;

/**
 * @brief Get device metadata (smart home info) for a specific MAC address
 * @param req HTTP request with ?mac=XX:XX:XX:XX:XX:XX query parameter
 * @return ESP_OK
 * 
 * Returns JSON:
 * {
 *   "found": true,
 *   "mac": "XX:XX:XX:XX:XX:XX",
 *   "deviceName": "...",
 *   "friendlyName": "...",
 *   "room": "...",
 *   "zone": "...",
 *   "tags": "...",
 *   "stationary": true/false,
 *   "source": "mesh" | "cached"  // Where metadata came from
 * }
 */
esp_err_t handleEspNowMetadata(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  
  httpd_resp_set_type(req, "application/json");
  
  // Parse MAC parameter
  char queryBuf[128];
  if (httpd_req_get_url_query_str(req, queryBuf, sizeof(queryBuf)) != ESP_OK) {
    httpd_resp_send(req, "{\"found\":false,\"error\":\"Missing query parameters\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  char macParam[32];
  if (httpd_query_key_value(queryBuf, "mac", macParam, sizeof(macParam)) != ESP_OK) {
    httpd_resp_send(req, "{\"found\":false,\"error\":\"Missing mac parameter\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  // URL-decode: browser sends E8%3A6B%3A... (encodeURIComponent encodes ':' as '%3A')
  // httpd_query_key_value does not percent-decode, so we must do it here
  String macDecoded = urlDecode(String(macParam));
  strncpy(macParam, macDecoded.c_str(), sizeof(macParam) - 1);
  macParam[sizeof(macParam) - 1] = '\0';
  
  // Parse MAC address
  uint8_t targetMac[6];
  if (sscanf(macParam, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
             &targetMac[0], &targetMac[1], &targetMac[2],
             &targetMac[3], &targetMac[4], &targetMac[5]) != 6) {
    WARN_ESPNOWF("[METADATA] API: invalid MAC format after decode: '%s' (raw: '%s')", macParam, macDecoded.c_str());
    httpd_resp_send(req, "{\"found\":false,\"error\":\"Invalid MAC format\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  // Try to find metadata in gMeshPeerMeta (mesh/pairing mode)
  MeshPeerMeta* meta = nullptr;
  DEBUG_ESPNOW_METADATAF("[METADATA] API: query for %02X:%02X:%02X:%02X:%02X:%02X gMeshPeerMeta=%p slots=%d",
    targetMac[0], targetMac[1], targetMac[2], targetMac[3], targetMac[4], targetMac[5],
    gMeshPeerMeta, gMeshPeerSlots);
  if (gMeshPeerMeta) {
    for (int i = 0; i < gMeshPeerSlots; i++) {
      if (!gMeshPeerMeta[i].isActive) continue;
      DEBUG_ESPNOW_METADATAF("[METADATA] API: slot[%d] active mac=%02X:%02X:%02X:%02X:%02X:%02X name='%s'",
        i,
        gMeshPeerMeta[i].mac[0], gMeshPeerMeta[i].mac[1], gMeshPeerMeta[i].mac[2],
        gMeshPeerMeta[i].mac[3], gMeshPeerMeta[i].mac[4], gMeshPeerMeta[i].mac[5],
        gMeshPeerMeta[i].name);
      if (memcmp(gMeshPeerMeta[i].mac, targetMac, 6) == 0) {
        meta = &gMeshPeerMeta[i];
        DEBUG_ESPNOW_METADATAF("[METADATA] API: HIT slot=%d name='%s' room='%s'",
          i, meta->name, meta->room);
        break;
      }
    }
    if (!meta) {
      DEBUG_ESPNOW_METADATAF("[METADATA] API: miss — no active slot matches %02X:%02X:%02X:%02X:%02X:%02X",
        targetMac[0], targetMac[1], targetMac[2], targetMac[3], targetMac[4], targetMac[5]);
    }
  } else {
    DEBUG_ESPNOW_METADATAF("[METADATA] API: gMeshPeerMeta is null");
  }
  
  // If found in mesh metadata, return it
  if (meta) {
    char json[512];
    snprintf(json, sizeof(json),
             "{\"found\":true,"
             "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
             "\"deviceName\":\"%s\","
             "\"friendlyName\":\"%s\","
             "\"room\":\"%s\","
             "\"zone\":\"%s\","
             "\"tags\":\"%s\","
             "\"stationary\":%s,"
             "\"source\":\"mesh\"}",
             targetMac[0], targetMac[1], targetMac[2], targetMac[3], targetMac[4], targetMac[5],
             meta->name,
             meta->friendlyName,
             meta->room,
             meta->zone,
             meta->tags,
             meta->stationary ? "true" : "false");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  // Try to load from cached settings (bond mode)
  if (filesystemReady) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
             targetMac[0], targetMac[1], targetMac[2], targetMac[3], targetMac[4], targetMac[5]);
    String settingsPath = String("/cache/peers/") + macStr + "/settings.json";
    
    if (LittleFS.exists(settingsPath.c_str())) {
      File f = LittleFS.open(settingsPath.c_str(), "r");
      if (f) {
        String settingsJson = f.readString();
        f.close();
        
        // Parse cached settings
        DynamicJsonDocument doc(1024);
        DeserializationError err = deserializeJson(doc, settingsJson);
        if (!err) {
          // Extract metadata fields
          const char* deviceName = doc["espnowDeviceName"] | "";
          const char* friendlyName = doc["espnowFriendlyName"] | "";
          const char* room = doc["espnowRoom"] | "";
          const char* zone = doc["espnowZone"] | "";
          const char* tags = doc["espnowTags"] | "";
          bool stationary = doc["espnowStationary"] | false;
          
          char json[512];
          snprintf(json, sizeof(json),
                   "{\"found\":true,"
                   "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                   "\"deviceName\":\"%s\","
                   "\"friendlyName\":\"%s\","
                   "\"room\":\"%s\","
                   "\"zone\":\"%s\","
                   "\"tags\":\"%s\","
                   "\"stationary\":%s,"
                   "\"source\":\"cached\"}",
                   targetMac[0], targetMac[1], targetMac[2], targetMac[3], targetMac[4], targetMac[5],
                   deviceName,
                   friendlyName,
                   room,
                   zone,
                   tags,
                   stationary ? "true" : "false");
          httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
          return ESP_OK;
        }
      }
    }
  }
  
  // Not found
  httpd_resp_send(req, "{\"found\":false}", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

#endif // ENABLE_WEB_ESPNOW && ENABLE_ESPNOW
