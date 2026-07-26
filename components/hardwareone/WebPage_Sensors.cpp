/**
 * Web Sensors - HTTP handlers for sensor data and status
 */

#include "System_BuildConfig.h"

#if ENABLE_WEB_SENSORS

#include <Arduino.h>
#include <LittleFS.h>
#include "System_VFS.h"
#include "WebServer_Server.h"           // httpd types, JSON_RESPONSE_SIZE, gJsonResponseBuffer, makeWebAuthCtx
#include "WebServer_Utils.h"            // WEB_AUTH_OR_RETURN, sendJsonResponse
#include "System_User.h"          // AuthContext, tgRequireAuth
#include "System_AuthIdentity.h"  // ExecIdentityGuard — run direct handlers under the web user's identity
#include "System_Debug.h"         // DEBUG_* macros
#include "System_Settings.h"             // Settings, gSettings
#include "System_Mutex.h"         // gJsonResponseMutex
#include "WebPage_Sensors.h"          // streamSensorsContent/Inner, page helpers
#include "System_I2C.h"           // I2C system helpers
#include "System_BuildConfig.h"        // Conditional sensor configuration
#if ENABLE_THERMAL_SENSOR
  #include "i2csensor_mlx90640.h"     // ThermalCache, gThermalCache, thermalBuildDataJSON
#endif
#if ENABLE_TOF_SENSOR
  #include "i2csensor_vl53l4cx.h"         // tofBuildDataJSON
#endif
#if ENABLE_IMU_SENSOR
  #include "i2csensor_bno055.h"         // imuBuildDataJSON
#endif
#if ENABLE_GAMEPAD_SENSOR
  #include "i2csensor_seesaw.h"     // gInputRunning, gInputConnected (gamepad build)
#endif
#if ENABLE_ANO_ENCODER
  #include "i2csensor_ano_encoder.h"  // anoEncoderBuildDataJSON + gAnoEncoder*
#endif
#if ENABLE_GPS_SENSOR
  #include "i2csensor_pa1010d.h"    // GPSCache, gGpsCache
#endif
#if ENABLE_RTC_SENSOR
  #include "i2csensor_ds3231.h"     // RTCCache, RTCDateTime, gRtcCache
#endif
#if ENABLE_PRESENCE_SENSOR
  #include "i2csensor_sths34pf80.h" // presenceEnabled, presenceConnected, gPresenceCache
#endif
#if ENABLE_EDGE_IMPULSE
  #include "System_EdgeImpulse.h"
#endif
#if ENABLE_ESPNOW
  #include "System_ESPNow_Sensors.h"            // Remote sensor functions
#endif
#include "System_SensorStubs.h" // Stubs for disabled sensors
#include "i2csensor_rda5807.h"             // fmRadioEnabled, radioInitialized, fmRadioBuildDataJSON
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
  WEB_AUTH_OR_RETURN(req, ctx);

  DEBUG_HTTPF("handler enter uri=%s user=%s page=%s", ctx.path.c_str(), ctx.user.c_str(), "sensors");
  streamPageWithContent(req, "sensors", ctx.user, streamSensorsContent);
  return ESP_OK;
}

// GET /api/sensors: multiplexed sensor JSON endpoint
esp_err_t handleSensorData(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  // Intentionally NO CORS headers. hw.fetchJSON sends `credentials: 'include'`
  // (see WebServer_Utils.cpp:528), and the W3C CORS spec forbids
  // `Access-Control-Allow-Origin: *` on credentialed responses — Safari and
  // Chromium-derived browsers reject the response with "access control checks"
  // failure even for same-origin fetches once the wildcard header appears.
  // The peer endpoints (`/api/sensors/status`, `/api/devices`, `/api/cli`)
  // omit CORS headers entirely and work correctly; matching that pattern here
  // unbreaks per-sensor polling (the encoder card was the first to expose this
  // because it polls at 56 ms; gamepad has the same latent bug).

  // Get query parameter to determine which sensor data to return
  char query[256];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char sensor[32];
    if (httpd_query_key_value(query, "sensor", sensor, sizeof(sensor)) == ESP_OK) {
      String sensorType = String(sensor);
      DEBUG_HTTPF("/api/sensors request sensor=%s", sensorType.c_str());

      if (sensorType == "thermal") {
#if !ENABLE_THERMAL_SENSOR
        sendJsonResponse(req, "{\"schema\":1,\"ok\":false, \"error\":\"not_compiled\"}");
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
            int jsonLen = thermalBuildDataJSON(buf, JSON_RESPONSE_SIZE);
            if (jsonLen > 0) {
              // DEBUG: Log buffer usage
              int usagePct = (jsonLen * 100) / JSON_RESPONSE_SIZE;
              DEBUG_MEMORY_BUFFERSF("[JSON_RESP_BUF] Thermal JSON: %d/%u bytes (%d%%)", jsonLen, (unsigned)JSON_RESPONSE_SIZE, usagePct);

              sendJsonResponse(req, buf, jsonLen);
              xSemaphoreGive(gJsonResponseMutex);
              return ESP_OK;
            }
          }
          xSemaphoreGive(gJsonResponseMutex);
        }

        // Fallback to ArduinoJson path (avoids String concatenation heap fragmentation)
        String json;
        if (lockThermalCache(pdMS_TO_TICKS(CACHE_MUTEX_TIMEOUT_MS))) {
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
          PSRAM_JSON_DOC(doc);
          doc["schema"] = 1;
          doc["ok"] = gThermalCache.thermalDataValid;
          doc["seq"] = gThermalCache.thermalSeq;
          doc["mn"] = serialized(String(gThermalCache.thermalMinTemp, 1));
          doc["mx"] = serialized(String(gThermalCache.thermalMaxTemp, 1));
          doc["w"] = width;
          doc["h"] = height;

          JsonArray data = doc["data"].to<JsonArray>();
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
        sendJsonResponse(req, "{\"schema\":1,\"ok\":false, \"error\":\"not_compiled\"}");
        return ESP_OK;
#endif
        // Always return ToF data, using stack-allocated buffer (no String allocations)

        // Use stack-allocated buffer for ToF response
        char tofResponseBuffer[TOF_RESPONSE_SIZE];
        int jsonLen = tofBuildDataJSON(tofResponseBuffer, TOF_RESPONSE_SIZE);

        // Send response
        httpd_resp_set_type(req, "application/json");
        DEBUG_HTTPF("/api/sensors tof json_len=%d", jsonLen);
        httpd_resp_send(req, tofResponseBuffer, jsonLen);

        return ESP_OK;
      } else if (sensorType == "imu") {
#if !ENABLE_IMU_SENSOR
        sendJsonResponse(req, "{\"schema\":1,\"ok\":false, \"error\":\"not_compiled\"}");
        return ESP_OK;
#endif
        // Always return IMU data, using stack-allocated buffer (no String allocations)

        // Use stack-allocated buffer for IMU response
        char imuResponseBuffer[IMU_RESPONSE_SIZE];
        int jsonLen = imuBuildDataJSON(imuResponseBuffer, IMU_RESPONSE_SIZE);

        // Send response
        httpd_resp_set_type(req, "application/json");
        DEBUG_HTTPF("/api/sensors imu json_len=%d", jsonLen);
        httpd_resp_send(req, imuResponseBuffer, jsonLen);

        return ESP_OK;
      } else if (sensorType == "input") {
#if !ENABLE_OLED_INPUT
        sendJsonResponse(req, "{\"val\":0, \"error\":\"not_compiled\"}");
        return ESP_OK;
#endif
        // Gamepad now follows queued start paradigm; read from shared state only
        if (!gInputRunning || !gInputConnected) {
          sendJsonResponse(req, "{\"val\":0, \"error\":\"not_connected\"}");
          return ESP_OK;
        }

        // Read from shared state (no direct I2C access)
        uint32_t buttons = 0;
        int x = 0, y = 0;
        bool dataValid = false;

        {
          SensorCacheGuard g(gInputCache.mutex, pdMS_TO_TICKS(50), "web.gamepadApi");
          if (g.held) {
            buttons = gInputCache.buttons;
            x = gInputCache.joyX;
            y = gInputCache.joyY;
            dataValid = gInputCache.dataValid;
          }
        }

        if (!dataValid) {
          sendJsonResponse(req, "{\"val\":0, \"error\":\"no_data\"}");
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
      } else if (sensorType == "anoencoder") {
#if !ENABLE_ANO_ENCODER
        sendJsonResponse(req, "{\"val\":0, \"error\":\"not_compiled\"}");
        return ESP_OK;
#else
        // ANO-shaped JSON: {val, pos, axis, buttons}. Built from the native
        // gAnoEncoderCache (encoder position + per-mode axis) so the sensor
        // card shows the real driver state, not the gamepad-shaped proxy.
        if (!gAnoEncoderEnabled || !gAnoEncoderConnected) {
          sendJsonResponse(req, "{\"val\":0, \"error\":\"not_connected\"}");
          return ESP_OK;
        }
        char anoBuf[160];
        int jsonLen = anoEncoderBuildDataJSON(anoBuf, sizeof(anoBuf));
        if (jsonLen <= 0) {
          sendJsonResponse(req, "{\"val\":0, \"error\":\"no_data\"}");
          return ESP_OK;
        }
        httpd_resp_set_type(req, "application/json");
        DEBUG_HTTPF("/api/sensors anoencoder json_len=%d", jsonLen);
        httpd_resp_send(req, anoBuf, jsonLen);
        return ESP_OK;
#endif
      } else if (sensorType == "fmradio") {
#if ENABLE_FM_RADIO
        // FM radio data - use stack-allocated buffer
        if (!gFmRadioRunning || !gRadioInitialized) {
          sendJsonResponse(req, "{\"schema\":1,\"ok\":false, \"error\":\"not_enabled\"}");
          return ESP_OK;
        }

        // Use stack-allocated buffer for FM radio response
        char fmRadioResponseBuffer[512];
        int jsonLen = fmRadioBuildDataJSON(fmRadioResponseBuffer, sizeof(fmRadioResponseBuffer));

        if (jsonLen > 0) {
          httpd_resp_set_type(req, "application/json");
          DEBUG_HTTPF("/api/sensors fmradio json_len=%d", jsonLen);
          httpd_resp_send(req, fmRadioResponseBuffer, jsonLen);
          return ESP_OK;
        } else {
          sendJsonResponse(req, "{\"schema\":1,\"ok\":false, \"error\":\"data_unavailable\"}");
          return ESP_OK;
        }
#else
        sendJsonResponse(req, "{\"schema\":1,\"ok\":false, \"error\":\"not_compiled\"}");
        return ESP_OK;
#endif
      } else if (sensorType == "camera") {
#if ENABLE_CAMERA_SENSOR
        extern const char* buildCameraStatusJson();
        const char* json = buildCameraStatusJson();
        sendJsonResponse(req, json);
        return ESP_OK;
#else
        sendJsonResponse(req, "{\"enabled\":false, \"error\":\"not_compiled\"}");
        return ESP_OK;
#endif
      } else if (sensorType == "microphone") {
#if ENABLE_MICROPHONE
        extern const char* buildMicrophoneStatusJson();
        const char* json = buildMicrophoneStatusJson();
        sendJsonResponse(req, json);
        return ESP_OK;
#else
        sendJsonResponse(req, "{\"enabled\":false, \"error\":\"not_compiled\"}");
        return ESP_OK;
#endif
      } else if (sensorType == "presence") {
#if ENABLE_PRESENCE_SENSOR
        extern bool gPresenceRunning;
        extern bool gPresenceConnected;
        extern PresenceCache gPresenceCache;
        
        if (!gPresenceRunning || !gPresenceConnected) {
          sendJsonResponse(req, "{\"error\":\"not_enabled\"}");
          return ESP_OK;
        }
        
        // Read from cache
        char buf[256];
        bool dataValid = false;
        float ambient = 0;
        int16_t presenceVal = 0, motionVal = 0;
        bool presenceDetected = false, motionDetected = false;
        
        {
          SensorCacheGuard g(gPresenceCache.mutex, pdMS_TO_TICKS(50), "web.presenceApi");
          if (g.held) {
            dataValid = gPresenceCache.dataValid;
            ambient = gPresenceCache.ambientTemp;
            presenceVal = gPresenceCache.presenceValue;
            motionVal = gPresenceCache.motionValue;
            presenceDetected = gPresenceCache.presenceDetected;
            motionDetected = gPresenceCache.motionDetected;
          }
        }
        
        if (!dataValid) {
          sendJsonResponse(req, "{\"error\":\"no_data\"}");
          return ESP_OK;
        }
        
        int len = snprintf(buf, sizeof(buf),
          "{\"ambientTemp\":%.1f,\"presenceValue\":%d,\"motionValue\":%d,\"presenceDetected\":%s,\"motionDetected\":%s}",
          ambient, presenceVal, motionVal,
          presenceDetected ? "true" : "false",
          motionDetected ? "true" : "false");
        
        sendJsonResponse(req, buf, len);
        return ESP_OK;
#else
        sendJsonResponse(req, "{\"error\":\"not_compiled\"}");
        return ESP_OK;
#endif
      } else if (sensorType == "gps") {
#if ENABLE_GPS_SENSOR
        extern bool gGpsRunning;
        extern bool gGpsConnected;
        extern GPSCache gGpsCache;
        
        if (!gGpsRunning || !gGpsConnected) {
          sendJsonResponse(req, "{\"error\":\"not_enabled\"}");
          return ESP_OK;
        }
        
        // Read from GPS cache (no I2C access)
        char buf[512];
        bool dataValid = false;
        float lat = 0, lon = 0, alt = 0, speed = 0, angle = 0;
        bool hasFix = false;
        uint8_t quality = 0, sats = 0;
        uint8_t hour = 0, minute = 0, second = 0, day = 0, month = 0;
        uint16_t year = 0;
        
        {
          SensorCacheGuard g(gGpsCache.mutex, pdMS_TO_TICKS(50), "web.gpsApi");
          if (g.held) {
            dataValid = gGpsCache.dataValid;
            lat = gGpsCache.latitude;
            lon = gGpsCache.longitude;
            alt = gGpsCache.altitude;
            speed = gGpsCache.speed;
            angle = gGpsCache.angle;
            hasFix = gGpsCache.hasFix;
            quality = gGpsCache.fixQuality;
            sats = gGpsCache.satellites;
            hour = gGpsCache.hour;
            minute = gGpsCache.minute;
            second = gGpsCache.second;
            day = gGpsCache.day;
            month = gGpsCache.month;
            year = gGpsCache.year;
          }
        }
        
        if (!dataValid) {
          sendJsonResponse(req, "{\"error\":\"no_data\"}");
          return ESP_OK;
        }
        
        int len = snprintf(buf, sizeof(buf),
          "{\"fix\":%s,\"quality\":%d,\"satellites\":%d,\"latitude\":%.6f,\"longitude\":%.6f,\"altitude\":%.1f,\"speed\":%.1f,\"angle\":%.1f,\"time\":\"%02d:%02d:%02d\",\"date\":\"%04d-%02d-%02d\"}",
          hasFix ? "true" : "false", quality, sats, lat, lon, alt, speed, angle,
          hour, minute, second, year, month, day);
        
        sendJsonResponse(req, buf, len);
        return ESP_OK;
#else
        sendJsonResponse(req, "{\"error\":\"not_compiled\"}");
        return ESP_OK;
#endif
      } else if (sensorType == "rtc") {
#if ENABLE_RTC_SENSOR
        extern bool gRtcRunning;
        extern bool gRtcConnected;
        extern RTCCache gRtcCache;
        
        if (!gRtcRunning || !gRtcConnected) {
          sendJsonResponse(req, "{\"error\":\"not_enabled\"}");
          return ESP_OK;
        }
        
        // Read from RTC cache (no I2C access)
        char buf[256];
        bool dataValid = false;
        RTCDateTime dt = {0};
        float temp = 0.0f;
        
        {
          SensorCacheGuard g(gRtcCache.mutex, pdMS_TO_TICKS(50), "web.rtcApi");
          if (g.held) {
            dataValid = gRtcCache.dataValid;
            dt = gRtcCache.dateTime;
            temp = gRtcCache.temperature;
          }
        }
        
        if (!dataValid) {
          sendJsonResponse(req, "{\"error\":\"no_data\"}");
          return ESP_OK;
        }
        
        // RTC stores UTC time - convert to local time using gSettings.tzOffsetMinutes
        // Manual calculation to avoid unsafe setenv/tzset that causes watchdog timeout
        extern Settings gSettings;
        int offsetMinutes = gSettings.tzOffsetMinutes;
        
        // Convert to minutes since midnight, apply offset, handle day rollover
        int totalMinutes = dt.hour * 60 + dt.minute + offsetMinutes;
        int localHour = dt.hour;
        int localMinute = dt.minute;
        int localDay = dt.day;
        int localMonth = dt.month;
        int localYear = dt.year;
        
        if (totalMinutes < 0) {
          // Rolled back to previous day
          totalMinutes += 1440; // 24 * 60
          localDay--;
          if (localDay < 1) {
            localMonth--;
            if (localMonth < 1) {
              localMonth = 12;
              localYear--;
            }
            // Get days in previous month
            int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            if (localMonth == 2 && ((localYear % 4 == 0 && localYear % 100 != 0) || (localYear % 400 == 0))) {
              daysInMonth[1] = 29; // Leap year
            }
            localDay = daysInMonth[localMonth - 1];
          }
        } else if (totalMinutes >= 1440) {
          // Rolled forward to next day
          totalMinutes -= 1440;
          localDay++;
          int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
          if (localMonth == 2 && ((localYear % 4 == 0 && localYear % 100 != 0) || (localYear % 400 == 0))) {
            daysInMonth[1] = 29;
          }
          if (localDay > daysInMonth[localMonth - 1]) {
            localDay = 1;
            localMonth++;
            if (localMonth > 12) {
              localMonth = 1;
              localYear++;
            }
          }
        }
        
        localHour = totalMinutes / 60;
        localMinute = totalMinutes % 60;
        
        // Calculate day of week for local date using Zeller's congruence
        int y = localYear;
        int m = localMonth;
        int d = localDay;
        if (m < 3) {
          m += 12;
          y--;
        }
        int q = d;
        int k = y % 100;
        int j = y / 100;
        int h = (q + ((13 * (m + 1)) / 5) + k + (k / 4) + (j / 4) - (2 * j)) % 7;
        // Convert Zeller result (0=Sat) to standard (0=Sun)
        int localDayOfWeek = (h + 6) % 7;
        
        const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        const char* dayName = (localDayOfWeek >= 0 && localDayOfWeek <= 6) ? days[localDayOfWeek] : "???";
        
        int len = snprintf(buf, sizeof(buf),
          "{\"year\":%d,\"month\":%d,\"day\":%d,\"dayOfWeek\":\"%s\",\"hour\":%d,\"minute\":%d,\"second\":%d,\"temperature\":%.1f}",
          localYear, localMonth, localDay, dayName, 
          localHour, localMinute, dt.second, temp);
        
        sendJsonResponse(req, buf, len);
        return ESP_OK;
#else
        sendJsonResponse(req, "{\"error\":\"not_compiled\"}");
        return ESP_OK;
#endif
      }
    }
  }

  // Default response for invalid/missing sensor parameter
  sendJsonResponse(req, "{\"valid\":false, \"error\":\"Invalid sensor parameter\"}");
  return ESP_OK;
}

// Sensors status endpoint (auth-protected): returns current enable flags and seq
esp_err_t handleSensorsStatus(httpd_req_t* req) {
  DEBUG_STORAGEF("[handleSensorsStatus] START");
  AuthContext ctx = makeWebAuthCtx(req);
  DEBUG_STORAGEF("[handleSensorsStatus] Auth check for user from IP: %s", ctx.ip.c_str());
  if (!tgRequireAuth(ctx)) {
    return ESP_OK;
  }
  if (!webGuestAccessAllowed(req, ctx)) return ESP_OK;
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
  WEB_AUTH_OR_RETURN(req, ctx);

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
        
        sendJsonResponse(req, jsonData.c_str(), jsonData.length());
        return ESP_OK;
      } else {
        DEBUG_HTTPF("/api/sensors/remote bad_mac device=%s sensor=%s", deviceMac, sensorType);
      }
    }
  }
  
  // Return list of all remote devices with sensors
  String devicesList = getRemoteDevicesListJSON();
  DEBUG_HTTPF("/api/sensors/remote list json_len=%u", (unsigned)devicesList.length());
  // Report ESP-NOW's *runtime* state, not just the compile flag. ENABLE_ESPNOW
  // only means the feature is compiled in — the radio may still be uninitialized
  // (user hasn't clicked Initialize). The web UI uses this to tell "not
  // initialized" apart from "active but no peers reporting".
  const bool espnowActive = isEspNowInitialized();
  String resp = devicesList;
  if (resp.startsWith("{")) {
    resp = String("{\"enabled\":") + (espnowActive ? "true" : "false") + "," + resp.substring(1);
  }
  sendJsonResponse(req, resp.c_str(), resp.length());
#else
  sendJsonResponse(req, "{\"enabled\":false, \"devices\":[]}");
#endif
  
  return ESP_OK;
}

// Camera status endpoint (auth-protected): returns camera state
esp_err_t handleCameraStatus(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

#if ENABLE_CAMERA_SENSOR
  extern const char* buildCameraStatusJson();
  const char* j = buildCameraStatusJson();
  sendJsonResponse(req, j);
#else
  sendJsonResponse(req, "{\"enabled\":false, \"compiled\":false}");
#endif
  return ESP_OK;
}

// Camera frame endpoint (auth-protected): returns JPEG frame
esp_err_t handleCameraFrame(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  DEBUG_HTTPF("/api/camera/frame enter user=%s ip=%s", ctx.user.c_str(), ctx.ip.c_str());

  if (!tgRequireAuth(ctx)) {
    DEBUG_HTTPF("/api/camera/frame auth failed ip=%s", ctx.ip.c_str());
    return ESP_OK;
  }
  if (!webGuestAccessAllowed(req, ctx)) return ESP_OK;

#if ENABLE_CAMERA_SENSOR
  extern bool gCameraRunning;
  extern uint8_t* captureFrame(size_t* outLen);

  if (!gCameraRunning) {
    DEBUG_CAMERAF("/api/camera/frame: camera not enabled, returning 503");
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Camera not enabled", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  size_t len = 0;
  uint8_t* frame = captureFrame(&len);

  if (!frame || len == 0) {
    DEBUG_CAMERAF("/api/camera/frame: capture failed frame=%p len=%u", frame, (unsigned)len);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Frame capture failed", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=frame.jpg");
  esp_err_t sendErr = httpd_resp_send(req, (const char*)frame, len);
  DEBUG_HTTPF("/api/camera/frame sent %u bytes status=%d", (unsigned)len, (int)sendErr);
  free(frame);
#else
  httpd_resp_set_status(req, "501 Not Implemented");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "Camera not compiled", HTTPD_RESP_USE_STRLEN);
#endif
  return ESP_OK;
}

// Camera MJPEG stream endpoint (auth-protected): returns multipart JPEG stream
esp_err_t handleCameraStream(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

#if ENABLE_CAMERA_SENSOR
  extern bool gCameraRunning;
  extern bool cameraStreaming;
  extern uint8_t* captureFrame(size_t* outLen);
  extern Settings gSettings;
  extern String getCookieSID(httpd_req_t* req);
  
  if (!gCameraRunning) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Camera not enabled", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Single-stream lock: only allow one active MJPEG client at a time.
  // Key by session cookie SID; fall back to IP if SID missing.
  static String s_streamOwner;
  static unsigned long s_streamLastBeat = 0;
  static uint32_t s_streamGen = 0;
  String sid = getCookieSID(req);
  char keyBuf[64];
  snprintf(keyBuf, sizeof(keyBuf), "%s:%s", sid.length() ? "sid" : "ip", sid.length() ? sid.c_str() : ctx.ip.c_str());
  String key = keyBuf;
  unsigned long nowMs = millis();
  const unsigned long staleMs = 5000UL;
  bool stale = (s_streamOwner.length() > 0) && ((long)(nowMs - s_streamLastBeat) > (long)staleMs);
  if (s_streamOwner.length() > 0 && s_streamOwner != key && !stale) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Camera stream already in use by another session", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  // Takeover semantics: if a new stream is requested (even from same session),
  // bump generation so the old loop will exit on its next iteration.
  s_streamOwner = key;
  uint32_t myGen = ++s_streamGen;
  s_streamLastBeat = nowMs;

  // Set MJPEG multipart content type
  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

  char partHeader[128];
  unsigned long lastFrameSentMs = 0;
  // Stream indefinitely until client disconnects (or error occurs)
  // Previously limited to 300 frames (~30s) but users expect continuous streaming
  
  // Set streaming flag for status indicator
  cameraStreaming = true;
  
  while (true) {
    // Heartbeat ownership; helps new sessions take over if client drops.
    s_streamLastBeat = millis();

    // If a newer stream has taken over, exit quickly.
    if (myGen != s_streamGen) {
      break;
    }

    // If camera is stopped while a client is streaming, end stream promptly.
    if (!gCameraRunning) {
      break;
    }

 #if ENABLE_EDGE_IMPULSE
    if (isContinuousInferenceRunning()) {
      unsigned long now = millis();
      // cameraStreamFps is 1..20; convert to ms per frame.
      int fps = (gSettings.cameraStreamFps > 0) ? gSettings.cameraStreamFps : 5;
      int delayMs = 1000 / fps;
      if (delayMs < 50) delayMs = 50;
      if (delayMs > 2000) delayMs = 2000;

      int minIntervalMs = delayMs;
      if (gSettings.edgeImpulseIntervalMs > 0) {
        int halfEI = gSettings.edgeImpulseIntervalMs / 2;
        if (halfEI > minIntervalMs) minIntervalMs = halfEI;
      }
      if (minIntervalMs < 200) minIntervalMs = 200;

      if (lastFrameSentMs && (long)(now - lastFrameSentMs) < (long)minIntervalMs) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
    }
 #endif

    size_t len = 0;
    uint8_t* frame = captureFrame(&len);
    if (!frame || len == 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Send boundary and headers
    int hdrLen = snprintf(partHeader, sizeof(partHeader),
      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)len);
    
    esp_err_t err = httpd_resp_send_chunk(req, partHeader, hdrLen);
    if (err != ESP_OK) {
      free(frame);
      break;  // Client disconnected
    }

    // Send frame data
    err = httpd_resp_send_chunk(req, (const char*)frame, len);
    free(frame);
    if (err != ESP_OK) break;

    lastFrameSentMs = millis();

    // Send trailing newline
    err = httpd_resp_send_chunk(req, "\r\n", 2);
    if (err != ESP_OK) break;

    // cameraStreamFps is 1..20; convert to ms per frame.
    int fps2 = (gSettings.cameraStreamFps > 0) ? gSettings.cameraStreamFps : 5;
    int delayMs = 1000 / fps2;
    if (delayMs < 50) delayMs = 50;
    if (delayMs > 2000) delayMs = 2000;
    vTaskDelay(pdMS_TO_TICKS(delayMs));
  }

  // Clear streaming flag when done
  cameraStreaming = false;
  
  // Release stream lock if we still own it.
  if (s_streamOwner == key && myGen == s_streamGen) {
    s_streamOwner = "";
  }

  // End multipart stream
  httpd_resp_send_chunk(req, NULL, 0);
#else
  httpd_resp_set_status(req, "501 Not Implemented");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "Camera not compiled", HTTPD_RESP_USE_STRLEN);
#endif
  return ESP_OK;
}

// Microphone recordings list endpoint (auth-protected)
esp_err_t handleMicRecordingsList(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

#if ENABLE_MICROPHONE
  extern int getRecordingCount();
  extern String getRecordingsList();

  // getRecordingCount/List enumerate via currentAuthContext() (per-task TLS).
  // This is a direct HTTP handler with no identity installed, so the read of
  // /sd/recordings would otherwise be denied. Install the authenticated web
  // user's identity for the duration — the listing then runs with the user's
  // own permissions (same identity the playback handler uses). No system bypass.
  ExecIdentityGuard identity(ctx);

  int count = getRecordingCount();
  String list = getRecordingsList();
  
  // Build JSON response
  char jsonHdr[32];
  snprintf(jsonHdr, sizeof(jsonHdr), "{\"count\":%d,\"files\":[", count);
  String json = jsonHdr;
  
  if (count > 0 && list.length() > 0) {
    // Parse "name:size,name:size" format
    int start = 0;
    bool first = true;
    while (start < (int)list.length()) {
      int comma = list.indexOf(',', start);
      if (comma < 0) comma = list.length();
      
      String item = list.substring(start, comma);
      int colon = item.indexOf(':');
      if (colon > 0) {
        String name = item.substring(0, colon);
        String size = item.substring(colon + 1);
        
        if (!first) json += ",";
        json += "{\"name\":\"" + name + "\",\"size\":" + size + "}";
        first = false;
      }
      start = comma + 1;
    }
  }
  
  json += "]}";
  
  sendJsonResponse(req, json.c_str(), json.length());
#else
  sendJsonResponse(req, "{\"count\":0, \"files\":[],\"error\":\"not_compiled\"}");
#endif
  return ESP_OK;
}

// Microphone recording file endpoint (auth-protected) - serves WAV file for playback
esp_err_t handleMicRecordingFile(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

#if ENABLE_MICROPHONE
  // Get filename from query string
  char query[128];
  char filename[64] = {0};
  
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "name", filename, sizeof(filename));
  }
  
  if (strlen(filename) == 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Missing filename parameter", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  if (strchr(filename, '/') != nullptr || strstr(filename, "..") != nullptr) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Invalid filename", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  String pathSd = String("/sd/recordings/") + filename;
  String pathLf = String("/recordings/") + filename;
  String path;
  if (VFS::existsGuarded(pathSd, ctx)) {
    path = pathSd;
  } else if (VFS::existsGuarded(pathLf, ctx)) {
    path = pathLf;
  } else {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Recording not found", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  File f = VFS::openGuarded(path, "r", ctx);
  if (!f) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Failed to open file", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  size_t fileSize = f.size();
  
  // Set headers for audio playback - Content-Length is required for browser audio seeking
  httpd_resp_set_type(req, "audio/wav");
  char contentLen[16];
  snprintf(contentLen, sizeof(contentLen), "%u", (unsigned)fileSize);
  httpd_resp_set_hdr(req, "Content-Length", contentLen);
  char contentDisp[128];
  snprintf(contentDisp, sizeof(contentDisp), "inline; filename=\"%s\"", filename);
  httpd_resp_set_hdr(req, "Content-Disposition", contentDisp);
  httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
  // CORS and caching headers for audio
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  
  // Read file into PSRAM (we have 8MB) and send with Content-Length for proper playback
  // Max recording is 60 sec * 16kHz * 2 bytes = ~1.9MB which fits in PSRAM
  char* buf = (char*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    // Fallback to regular malloc for smaller files
    buf = (char*)malloc(fileSize);
  }
  if (!buf) {
    f.close();
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, "Memory allocation failed", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  size_t bytesRead = f.read((uint8_t*)buf, fileSize);
  f.close();
  
  // Send entire file at once - this works with Content-Length header
  httpd_resp_send(req, buf, bytesRead);
  free(buf);
#else
  httpd_resp_set_status(req, "501 Not Implemented");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "Microphone not compiled", HTTPD_RESP_USE_STRLEN);
#endif
  return ESP_OK;
}

// Register all sensor-related URI handlers
void registerSensorHandlers(httpd_handle_t server) {
  // Sensors page
  static const httpd_uri_t sensorsPage = { .uri = "/sensors", .method = HTTP_GET, .handler = handleSensorsPage, .user_ctx = NULL };
  httpd_register_uri_handler(server, &sensorsPage);
  
  // Sensor data API
  static const httpd_uri_t sensorData = { .uri = "/api/sensors", .method = HTTP_GET, .handler = handleSensorData, .user_ctx = NULL };
  httpd_register_uri_handler(server, &sensorData);
  
  // Sensor status API
  static const httpd_uri_t sensorsStatus = { .uri = "/api/sensors/status", .method = HTTP_GET, .handler = handleSensorsStatusWithUpdates, .user_ctx = NULL };
  httpd_register_uri_handler(server, &sensorsStatus);
  
  // Remote sensors API
  static const httpd_uri_t remoteSensors = { .uri = "/api/sensors/remote", .method = HTTP_GET, .handler = handleRemoteSensors, .user_ctx = NULL };
  httpd_register_uri_handler(server, &remoteSensors);
  
  // Camera endpoints
  static const httpd_uri_t cameraStatus = { .uri = "/api/sensors/camera/status", .method = HTTP_GET, .handler = handleCameraStatus, .user_ctx = NULL };
  static const httpd_uri_t cameraFrame = { .uri = "/api/sensors/camera/frame", .method = HTTP_GET, .handler = handleCameraFrame, .user_ctx = NULL };
  static const httpd_uri_t cameraStream = { .uri = "/api/sensors/camera/stream", .method = HTTP_GET, .handler = handleCameraStream, .user_ctx = NULL };
  httpd_register_uri_handler(server, &cameraStatus);
  httpd_register_uri_handler(server, &cameraFrame);
  httpd_register_uri_handler(server, &cameraStream);
  
  // Microphone recording endpoints
  static const httpd_uri_t micRecordings = { .uri = "/api/recordings", .method = HTTP_GET, .handler = handleMicRecordingsList, .user_ctx = NULL };
  static const httpd_uri_t micRecordingFile = { .uri = "/api/recordings/file", .method = HTTP_GET, .handler = handleMicRecordingFile, .user_ctx = NULL };
  httpd_register_uri_handler(server, &micRecordings);
  httpd_register_uri_handler(server, &micRecordingFile);

  // Video viewer endpoints (list + download AVI recordings from SD). Part of
  // the base experience: viewing/downloading existing videos works on any
  // SD-equipped board, independent of whether a camera is compiled in.
  // (Recording is camera-gated; these handlers degrade gracefully — they return
  // an empty list / "SD unavailable" when there's no SD card.)
  extern esp_err_t handleVideoRecordingsList(httpd_req_t* req);
  extern esp_err_t handleVideoRecordingFile(httpd_req_t* req);
  static const httpd_uri_t vidList = { .uri = "/api/videos", .method = HTTP_GET, .handler = handleVideoRecordingsList, .user_ctx = NULL };
  static const httpd_uri_t vidFile = { .uri = "/api/videos/file", .method = HTTP_GET, .handler = handleVideoRecordingFile, .user_ctx = NULL };
  httpd_register_uri_handler(server, &vidList);
  httpd_register_uri_handler(server, &vidFile);
}

#endif // ENABLE_HTTP_SERVER
