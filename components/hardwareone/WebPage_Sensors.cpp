/**
 * Web Sensors - HTTP handlers for sensor data and status
 */

#include "System_BuildConfig.h"

#if ENABLE_HTTP_SERVER

#include <Arduino.h>
#include "WebServer_Server.h"           // httpd types, JSON_RESPONSE_SIZE, gJsonResponseBuffer
#include "System_User.h"          // AuthContext, tgRequireAuth
#include "System_Debug.h"         // DEBUG_* macros
#include "System_Settings.h"             // Settings, gSettings
#include "System_Mutex.h"         // gJsonResponseMutex
#include "WebPage_Sensors.h"          // streamSensorsContent/Inner, page helpers
#include "System_I2C.h"           // I2C system helpers
#include "System_BuildConfig.h"        // Conditional sensor configuration
#if ENABLE_THERMAL_SENSOR
  #include "i2csensor-mlx90640.h"     // ThermalCache, gThermalCache, buildThermalDataJSON
#endif
#if ENABLE_TOF_SENSOR
  #include "i2csensor-vl53l4cx.h"         // buildToFDataJSON
#endif
#if ENABLE_IMU_SENSOR
  #include "i2csensor-bno055.h"         // buildIMUDataJSON
#endif
#if ENABLE_GAMEPAD_SENSOR
  #include "i2csensor-seesaw.h"     // gamepadEnabled, gamepadConnected
#endif
#if ENABLE_ESPNOW
  #include "System_ESPNow_Sensors.h"            // Remote sensor functions
#endif
#include "System_SensorStubs.h" // Stubs for disabled sensors
#include "i2csensor-rda5807.h"             // fmRadioEnabled, radioInitialized, buildFMRadioDataJSON
#include "System_MemUtil.h"             // ps_alloc, AllocPref

// External helpers
extern void getClientIP(httpd_req_t* req, String& ipOut);
extern const char* buildSensorStatusJson();
extern void streamBeginHtml(httpd_req_t* req, const char* title, bool isPublic, const String& username, const String& activePage);
extern void streamEndHtml(httpd_req_t* req);
extern void streamChunk(httpd_req_t* req, const char* chunk);

// streamSensorsContent() is now defined as inline in web_sensors.h with I2C_SENSORS_ENABLED conditional

// Local response sizing (stack buffers)
static const size_t TOF_RESPONSE_SIZE = 1024;   // 1KB sufficient for 4 ToF objects
static const size_t IMU_RESPONSE_SIZE = 512;    // 512 bytes sufficient for IMU data

// GET /sensors: sensors page
esp_err_t handleSensorsPage(httpd_req_t* req) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  // httpd_req_t::uri is a fixed-size char array; it is never NULL
  ctx.path = req->uri;
  getClientIP(req, ctx.ip);
  if (!tgRequireAuth(ctx)) return ESP_OK;  // 401 already sent
  logAuthAttempt(true, req->uri, ctx.user, ctx.ip, "");

  DEBUG_HTTPF("handler enter uri=%s user=%s page=%s", ctx.path.c_str(), ctx.user.c_str(), "sensors");
  streamPageWithContent(req, "sensors", ctx.user, streamSensorsContent);
  return ESP_OK;
}

// GET /api/sensors: multiplexed sensor JSON endpoint
esp_err_t handleSensorData(httpd_req_t* req) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  // httpd_req_t::uri is a fixed-size char array; it is never NULL
  ctx.path = req->uri;
  getClientIP(req, ctx.ip);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  // Add CORS headers to prevent access control errors
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

  // Get query parameter to determine which sensor data to return
  char query[256];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char sensor[32];
    if (httpd_query_key_value(query, "sensor", sensor, sizeof(sensor)) == ESP_OK) {
      String sensorType = String(sensor);
      DEBUG_HTTPF("/api/sensors request sensor=%s", sensorType.c_str());

      if (sensorType == "thermal") {
#if !ENABLE_THERMAL_SENSOR
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"v\":0,\"error\":\"not_compiled\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
#endif
        // Preferred path: use shared response buffer (avoids large stack usage)
        // Lazy-init mutex and buffer in case requests arrive before global init completes
        if (!gJsonResponseMutex) {
          gJsonResponseMutex = xSemaphoreCreateMutex();
        }
        if (gJsonResponseMutex && xSemaphoreTake(gJsonResponseMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          if (!gJsonResponseBuffer) {
            gJsonResponseBuffer = (char*)ps_alloc(JSON_RESPONSE_SIZE, AllocPref::PreferPSRAM, "json.resp.buf");
          }
          if (gJsonResponseBuffer) {
            char* buf = gJsonResponseBuffer;
            int jsonLen = buildThermalDataJSON(buf, JSON_RESPONSE_SIZE);
            if (jsonLen > 0) {
              // DEBUG: Log buffer usage
              int usagePct = (jsonLen * 100) / JSON_RESPONSE_SIZE;
              DEBUG_MEMORYF("[JSON_RESP_BUF] Thermal JSON: %d/%u bytes (%d%%)", jsonLen, (unsigned)JSON_RESPONSE_SIZE, usagePct);

              httpd_resp_set_type(req, "application/json");
              httpd_resp_send(req, buf, jsonLen);
              xSemaphoreGive(gJsonResponseMutex);
              return ESP_OK;
            }
          }
          xSemaphoreGive(gJsonResponseMutex);
        }

        // Fallback to ArduinoJson path (avoids String concatenation heap fragmentation)
        String json;
        if (lockThermalCache(pdMS_TO_TICKS(100))) {  // 100ms timeout for HTTP response
          bool useInterpolated = (gThermalCache.thermalInterpolated != nullptr && gThermalCache.thermalInterpolatedWidth > 0 && gThermalCache.thermalInterpolatedHeight > 0);
          // For raw frame, convert int16_t to float on the fly
          float* frame = useInterpolated ? gThermalCache.thermalInterpolated : nullptr;
          int frameSize = useInterpolated ? (gThermalCache.thermalInterpolatedWidth * gThermalCache.thermalInterpolatedHeight) : 768;
          // For raw frame, swap dimensions if rotation is 90° or 270°
          int width = useInterpolated ? gThermalCache.thermalInterpolatedWidth : ((gSettings.thermalRotation == 1 || gSettings.thermalRotation == 3) ? 24 : 32);
          int height = useInterpolated ? gThermalCache.thermalInterpolatedHeight : ((gSettings.thermalRotation == 1 || gSettings.thermalRotation == 3) ? 32 : 24);

          // Debug: Track rotation value when JSON dimensions are determined (fallback path)
          DEBUG_MEMORYF("[ROTATION_DEBUG] JSON fallback: rotation=%d, w=%d, h=%d, seq=%lu",
                        gSettings.thermalRotation, width, height,
                        (unsigned long)gThermalCache.thermalSeq);

          // Use ArduinoJson to avoid 768+ String concatenations
          DynamicJsonDocument doc(8192);
          doc["v"] = gThermalCache.thermalDataValid ? 1 : 0;
          doc["seq"] = gThermalCache.thermalSeq;
          doc["mn"] = serialized(String(gThermalCache.thermalMinTemp, 1));
          doc["mx"] = serialized(String(gThermalCache.thermalMaxTemp, 1));
          doc["w"] = width;
          doc["h"] = height;
          
          JsonArray data = doc.createNestedArray("data");
          if (useInterpolated && frame) {
            for (int i = 0; i < frameSize; i++) {
              data.add((int)frame[i]);
            }
          } else if (gThermalCache.thermalFrame) {
            for (int i = 0; i < frameSize; i++) {
              data.add(gThermalCache.thermalFrame[i]);
            }
          }
          
          unlockThermalCache();
          
          String json;
          serializeJson(doc, json);
        } else {
          json = "{\"error\":\"Sensor data temporarily unavailable\"}";
        }

        httpd_resp_set_type(req, "application/json");
        DEBUG_HTTPF("/api/sensors thermal json_len=%u", (unsigned)json.length());
        httpd_resp_send(req, json.c_str(), json.length());
        return ESP_OK;
      } else if (sensorType == "tof") {
#if !ENABLE_TOF_SENSOR
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"v\":0,\"error\":\"not_compiled\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
#endif
        // Always return ToF data, using stack-allocated buffer (no String allocations)
        DEBUG_FRAMEF("handleSensorData: ToF data requested via /api/sensors?sensor=tof");

        // Use stack-allocated buffer for ToF response
        char tofResponseBuffer[TOF_RESPONSE_SIZE];
        int jsonLen = buildToFDataJSON(tofResponseBuffer, TOF_RESPONSE_SIZE);
        DEBUG_FRAMEF("handleSensorData: ToF JSON response length=%d", jsonLen);

        // Send response
        httpd_resp_set_type(req, "application/json");
        DEBUG_HTTPF("/api/sensors tof json_len=%d", jsonLen);
        httpd_resp_send(req, tofResponseBuffer, jsonLen);

        return ESP_OK;
      } else if (sensorType == "imu") {
#if !ENABLE_IMU_SENSOR
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"v\":0,\"error\":\"not_compiled\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
#endif
        // Always return IMU data, using stack-allocated buffer (no String allocations)

        // Use stack-allocated buffer for IMU response
        char imuResponseBuffer[IMU_RESPONSE_SIZE];
        int jsonLen = buildIMUDataJSON(imuResponseBuffer, IMU_RESPONSE_SIZE);

        // Send response
        httpd_resp_set_type(req, "application/json");
        DEBUG_HTTPF("/api/sensors imu json_len=%d", jsonLen);
        httpd_resp_send(req, imuResponseBuffer, jsonLen);

        return ESP_OK;
      } else if (sensorType == "gamepad") {
#if !ENABLE_GAMEPAD_SENSOR
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"val\":0,\"error\":\"not_compiled\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
#endif
        // Gamepad now follows queued start paradigm; read from shared state only
        if (!gamepadEnabled || !gamepadConnected) {
          // Log why we're rejecting
          Serial.printf("[GAMEPAD_API] Rejecting request: enabled=%d connected=%d\n", gamepadEnabled, gamepadConnected);
          httpd_resp_set_type(req, "application/json");
          httpd_resp_send(req, "{\"val\":0,\"error\":\"not_connected\"}", HTTPD_RESP_USE_STRLEN);
          return ESP_OK;
        }
        Serial.printf("[GAMEPAD_API] Flags OK: enabled=%d connected=%d\n", gamepadEnabled, gamepadConnected);

        // Read from shared state (no direct I2C access)
        uint32_t buttons = 0;
        int x = 0, y = 0;
        bool dataValid = false;

        if (gControlCache.mutex && xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          buttons = gControlCache.gamepadButtons;
          x = gControlCache.gamepadX;
          y = gControlCache.gamepadY;
          dataValid = gControlCache.gamepadDataValid;
          xSemaphoreGive(gControlCache.mutex);
        }

        if (!dataValid) {
          httpd_resp_set_type(req, "application/json");
          httpd_resp_send(req, "{\"val\":0,\"error\":\"no_data\"}", HTTPD_RESP_USE_STRLEN);
          return ESP_OK;
        }

        // Use stack buffer to avoid String heap fragmentation
        char gamepadBuf[128];
        int jsonLen = snprintf(gamepadBuf, sizeof(gamepadBuf),
                               "{\"val\":1,\"x\":%d,\"y\":%d,\"buttons\":%lu}",
                               x, y, (unsigned long)buttons);

        httpd_resp_set_type(req, "application/json");
        DEBUG_HTTPF("/api/sensors gamepad json_len=%d", jsonLen);
        httpd_resp_send(req, gamepadBuf, jsonLen);
        return ESP_OK;
      } else if (sensorType == "fmradio") {
        // FM radio data - use stack-allocated buffer
        if (!fmRadioEnabled || !radioInitialized) {
          httpd_resp_set_type(req, "application/json");
          httpd_resp_send(req, "{\"v\":0,\"error\":\"not_enabled\"}", HTTPD_RESP_USE_STRLEN);
          return ESP_OK;
        }

        // Use stack-allocated buffer for FM radio response
        char fmRadioResponseBuffer[512];
        int jsonLen = buildFMRadioDataJSON(fmRadioResponseBuffer, sizeof(fmRadioResponseBuffer));
        
        if (jsonLen > 0) {
          httpd_resp_set_type(req, "application/json");
          DEBUG_HTTPF("/api/sensors fmradio json_len=%d", jsonLen);
          httpd_resp_send(req, fmRadioResponseBuffer, jsonLen);
          return ESP_OK;
        } else {
          httpd_resp_set_type(req, "application/json");
          httpd_resp_send(req, "{\"v\":0,\"error\":\"data_unavailable\"}", HTTPD_RESP_USE_STRLEN);
          return ESP_OK;
        }
      }
    }
  }

  // Default response for invalid/missing sensor parameter
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"valid\":false,\"error\":\"Invalid sensor parameter\"}", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// Sensors status endpoint (auth-protected): returns current enable flags and seq
esp_err_t handleSensorsStatus(httpd_req_t* req) {
  DEBUG_STORAGEF("[handleSensorsStatus] START");
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  ctx.path = "/api/sensors/status";
  getClientIP(req, ctx.ip);
  DEBUG_STORAGEF("[handleSensorsStatus] Auth check for user from IP: %s", ctx.ip.c_str());
  if (!tgRequireAuth(ctx)) {
    WARN_SESSIONF("Sensors status auth failed");
    return ESP_OK;
  }
  DEBUG_STORAGEF("[handleSensorsStatus] Auth SUCCESS for user: %s", ctx.user.c_str());

  httpd_resp_set_type(req, "application/json");
  DEBUG_STORAGEF("[handleSensorsStatus] Building sensor status JSON...");
  const char* j = buildSensorStatusJson();
  size_t jLen = strlen(j);
  DEBUG_STORAGEF("[handleSensorsStatus] JSON built, length: %zu bytes", jLen);

  // Debug: log payload to serial (truncate if large)
  char jDbg[201];
  size_t copyLen = jLen > 200 ? 200 : jLen;
  strncpy(jDbg, j, copyLen);
  jDbg[copyLen] = '\0';
  DEBUG_HTTPF("/api/sensors/status by %s @ %s: seq=%lu, json_len=%zu, json_snippet=%s",
              ctx.user.c_str(), ctx.ip.c_str(), (unsigned long)gSensorStatusSeq, jLen, jDbg);

  DEBUG_STORAGEF("[handleSensorsStatus] Sending response...");
  httpd_resp_send(req, j, HTTPD_RESP_USE_STRLEN);
  DEBUG_STORAGEF("[handleSensorsStatus] COMPLETE: Success");
  return ESP_OK;
}

// Remote sensors endpoint (auth-protected): returns list of remote devices with sensors
esp_err_t handleRemoteSensors(httpd_req_t* req) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  ctx.path = "/api/sensors/remote";
  getClientIP(req, ctx.ip);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  DEBUG_HTTPF("/api/sensors/remote by %s @ %s", ctx.user.c_str(), ctx.ip.c_str());

#if ENABLE_ESPNOW
  // Check for device+sensor query parameters
  char query[256];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char deviceMac[32];
    char sensorType[32];
    
    if (httpd_query_key_value(query, "device", deviceMac, sizeof(deviceMac)) == ESP_OK &&
        httpd_query_key_value(query, "sensor", sensorType, sizeof(sensorType)) == ESP_OK) {
      // URL-decode the MAC (browser sends E8%3A9F%3A... instead of E8:9F:...)
      extern String urlDecode(const String& s);
      String decodedMac = urlDecode(String(deviceMac));
      strncpy(deviceMac, decodedMac.c_str(), sizeof(deviceMac) - 1);
      deviceMac[sizeof(deviceMac) - 1] = '\0';
      
      // Return specific sensor data
      uint8_t mac[6];
      // Parse MAC address
      if (sscanf(deviceMac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                 &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
        RemoteSensorType type = stringToSensorType(sensorType);
        String jsonData = getRemoteSensorDataJSON(mac, type);

        char dbg[121];
        size_t n = jsonData.length();
        size_t cpy = (n > 120) ? 120 : n;
        if (cpy > 0) {
          memcpy(dbg, jsonData.c_str(), cpy);
        }
        dbg[cpy] = '\0';
        DEBUG_HTTPF("/api/sensors/remote data device=%s sensor=%s type=%d json_len=%u json_snip=%.120s",
                    deviceMac, sensorType, (int)type, (unsigned)jsonData.length(), dbg);
        
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, jsonData.c_str(), jsonData.length());
        return ESP_OK;
      } else {
        DEBUG_HTTPF("/api/sensors/remote bad_mac device=%s sensor=%s", deviceMac, sensorType);
      }
    }
  }
  
  // Return list of all remote devices with sensors
  String devicesList = getRemoteDevicesListJSON();
  DEBUG_HTTPF("/api/sensors/remote list json_len=%u", (unsigned)devicesList.length());
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, devicesList.c_str(), devicesList.length());
#else
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"devices\":[]}", HTTPD_RESP_USE_STRLEN);
#endif
  
  return ESP_OK;
}

#endif // ENABLE_HTTP_SERVER
