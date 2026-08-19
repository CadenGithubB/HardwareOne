#ifndef SYSTEM_SENSORSTUBS_H
#define SYSTEM_SENSORSTUBS_H

#include <Arduino.h>
#include <IPAddress.h>
#include "System_BuildConfig.h"
#include "System_Microphone.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// =============================================================================
// SENSOR STUBS - Uniform stub definitions for disabled sensors/modules
// =============================================================================
// Struct definitions live in each sensor header, above their #if ENABLE_* guard,
// so they are always available regardless of feature flags (single source of truth).
// These stubs provide externs and inline no-op functions for disabled sensors
// so the rest of the codebase compiles and links cleanly.

#if !ENABLE_THERMAL_SENSOR
  #include "i2csensor_mlx90640.h"  // Provides ThermalCache struct
  extern ThermalCache gThermalCache;
  extern bool gThermalRunning;
  extern bool gThermalConnected;
  extern unsigned long gThermalLastStopTime;
  extern TaskHandle_t gThermalTaskHandle;
  extern volatile UBaseType_t gThermalWatermarkNow;
  extern volatile UBaseType_t gThermalWatermarkMin;
  extern const struct CommandEntry thermalCommands[];
  extern const size_t thermalCommandsCount;
  // Thermal stub functions
  inline int thermalBuildDataJSON(char* buf, size_t bufSize) { return 0; }
  inline bool thermalStartInternal() { return false; }
#endif

#if !ENABLE_TOF_SENSOR
  #include "i2csensor_vl53l4cx.h"  // Provides TofCache struct
  extern TofCache gTofCache;
  extern bool gTofRunning;
  extern bool gTofConnected;
  extern uint32_t gTofLastStopTime;
  extern TaskHandle_t gTofTaskHandle;
  extern volatile UBaseType_t gTofWatermarkNow;
  extern volatile UBaseType_t gTofWatermarkMin;
  extern const struct CommandEntry tofCommands[];
  extern const size_t tofCommandsCount;
  // ToF stub functions
  inline int tofBuildDataJSON(char* buf, size_t bufSize) { return 0; }
  inline bool tofStartInternal() { return false; }
#endif

#if !ENABLE_IMU_SENSOR
  #include "i2csensor_bno055.h"  // Provides ImuCache and IMUActionState structs
  extern ImuCache gImuCache;
  extern bool gImuRunning;
  extern bool gImuConnected;
  extern unsigned long gImuLastStopTime;
  extern TaskHandle_t gImuTaskHandle;
  extern volatile UBaseType_t gImuWatermarkNow;
  extern volatile UBaseType_t gImuWatermarkMin;
  extern const struct CommandEntry imuCommands[];
  extern const size_t imuCommandsCount;
  // IMU stub functions
  inline int imuBuildDataJSON(char* buf, size_t bufSize) { return 0; }
  inline bool imuStartInternal() { return false; }
  void imuUpdateActions();
#endif

#if !ENABLE_GAMEPAD_SENSOR
  #include "i2csensor_seesaw.h"  // Provides InputCache struct
  extern InputCache gInputCache;
  extern bool gInputRunning;
  extern bool gInputConnected;
  extern unsigned long gGamepadLastStopTime;
  extern TaskHandle_t gInputTaskHandle;
  extern volatile UBaseType_t gGamepadWatermarkMin;
  extern volatile UBaseType_t gGamepadWatermarkNow;
  extern const struct CommandEntry gamepadCommands[];
  extern const size_t gamepadCommandsCount;
  // inputStartInternal() is provided by the ANO encoder driver when that is
  // the active input device; only stub it when NO input driver is compiled in.
#if !ENABLE_ANO_ENCODER
  inline bool inputStartInternal() { return false; }
#endif
#endif

#if !ENABLE_OLED_DISPLAY
  // Only the most basic OLED stubs - variables referenced externally
  extern bool gOledRunning;
  extern bool oledConnected;
  extern class Adafruit_SSD1306* oledDisplay;
  // Minimal stub functions for initialization calls
  inline bool earlyOLEDInit() { return false; }
  inline void processOLEDBootSequence() { }
  inline void updateOLEDDisplay() { }
  // Command stubs for system_utils.cpp command registry
  extern const struct CommandEntry oledCommands[];
  extern const size_t oledCommandsCount;
#endif

#if !ENABLE_APDS_SENSOR
  #include "i2csensor_apds9960.h"  // Provides APDSCache struct
  extern APDSCache gApdsCache;
  extern bool gApdsConnected;
  extern bool gApdsColorRunning;
  extern bool gApdsProximityRunning;
  extern bool gApdsGestureRunning;
  extern unsigned long gApdsLastStopTime;
  extern TaskHandle_t gApdsTaskHandle;
  extern const struct CommandEntry apdsCommands[];
  extern const size_t apdsCommandsCount;
  // APDS stub functions
  inline bool apdsStartInternal() { return false; }
#endif

#if !ENABLE_GPS_SENSOR
  #include "i2csensor_pa1010d.h"  // Provides GPSCache struct
  extern GPSCache gGpsCache;
  extern bool gGpsRunning;
  extern bool gGpsConnected;
  extern unsigned long gGpsLastStopTime;
  extern class Adafruit_GPS* gPA1010D;
  extern TaskHandle_t gGpsTaskHandle;
  extern const struct CommandEntry gpsCommands[];
  extern const size_t gpsCommandsCount;
  // GPS stub functions
  inline bool gpsStartInternal() { return false; }
#endif

#if !ENABLE_FM_RADIO
  // FM Radio stubs when disabled
  #include "i2csensor_rda5807.h"  // Provides FMRadioCache struct
  extern bool gFmRadioRunning;
  extern bool gFmRadioConnected;
  extern unsigned long gFmRadioLastStopTime;
  extern bool gRadioInitialized;
  extern FMRadioCache gFmRadioCache;
  extern TaskHandle_t gFmRadioTaskHandle;
  extern const struct CommandEntry fmRadioCommands[];
  extern const size_t fmRadioCommandsCount;
  inline bool fmRadioInit() { return false; }
  inline void fmRadioDeinit() {}
  inline void fmRadioPoll() {}
  inline int fmRadioBuildDataJSON(char* buf, size_t bufSize) { return 0; }
  inline bool fmRadioStartInternal() { return false; }
#endif

#if !ENABLE_PRESENCE_SENSOR
  #include "i2csensor_sths34pf80.h"  // Provides PresenceCache struct
  extern PresenceCache gPresenceCache;
  extern bool gPresenceRunning;
  extern bool gPresenceConnected;
  extern unsigned long gPresenceLastStopTime;
  extern TaskHandle_t gPresenceTaskHandle;
  extern const struct CommandEntry presenceCommands[];
  extern const size_t presenceCommandsCount;
  // Presence stub functions
  inline bool presenceStartInternal() { return false; }
  inline bool presenceInit() { return false; }
  inline bool presencePoll() { return false; }
  inline int presenceBuildDataJSON(char* buf, size_t bufSize) { return 0; }
#endif

#if !ENABLE_RTC_SENSOR
  #include "i2csensor_ds3231.h"  // Provides RTCDateTime and RTCCache structs
  extern RTCCache gRtcCache;
  extern bool gRtcRunning;
  extern bool gRtcConnected;
  extern unsigned long gRtcLastStopTime;
  extern TaskHandle_t gRtcTaskHandle;
  extern volatile UBaseType_t gRtcWatermarkNow;
  extern volatile UBaseType_t gRtcWatermarkMin;
  extern const struct CommandEntry rtcCommands[];
  extern const size_t rtcCommandsCount;
  // RTC stub functions
  bool rtcStartInternal();
  inline int rtcBuildDataJSON(char* buf, size_t bufSize) { return 0; }
#endif

#if !ENABLE_SERVO
  #include "i2csensor_pca9685.h"  // Provides ServoProfile, PCA9685_I2C_ADDRESS, MAX_SERVO_CHANNELS
  extern const struct CommandEntry servoCommands[];
  extern const size_t servoCommandsCount;
  // Servo stub functions
  inline bool servoInit() { return false; }
  inline void servoSetAngle(uint8_t channel, int angle) {}
  inline void servoSetPWM(uint8_t channel, uint16_t value) {}
#endif

#if !ENABLE_BLUETOOTH
  // Bluetooth stubs when disabled
  extern const struct CommandEntry bluetoothCommands[];
  extern const size_t bluetoothCommandsCount;
#endif

#if !ENABLE_CAMERA_SENSOR
  // Camera stubs when disabled
  extern bool gCameraRunning;
  extern bool cameraConnected;
  extern bool cameraStreaming;
  extern bool cameraDetected;
  extern const char* cameraModel;
  extern int cameraWidth;
  extern int cameraHeight;
  extern const struct CommandEntry cameraCommands[];
  extern const size_t cameraCommandsCount;
  inline bool initCamera() { return false; }
  inline void stopCamera() {}
  inline uint8_t* captureFrame(size_t* outLen) { if (outLen) *outLen = 0; return nullptr; }
  // Same key set as the real builder so clients can branch on supported/detected
  // without special-casing the compiled-out build. An empty {} used to be
  // indistinguishable from "camera present but stopped" on the wire.
  inline const char* buildCameraStatusJson() {
    return "{\"supported\":false,\"detected\":false,\"enabled\":false,"
           "\"connected\":false,\"streaming\":false,\"model\":\"None\","
           "\"width\":0,\"height\":0,\"psram\":false}";
  }
#endif

#if !ENABLE_MICROPHONE
  // Microphone stubs when the mic subsystem is absent entirely (no PDM AND no
  // G2-capable build). Gated on ENABLE_MICROPHONE in lockstep with
  // System_Microphone.{h,cpp} and System_SensorStubs.cpp — otherwise a PDM-less
  // but G2-capable board (FeatherS3) compiles both these inline stubs and the
  // real prototypes/definitions → ODR + duplicate-symbol link errors.
  extern bool gMicRunning;
  extern bool micConnected;
  extern int micSampleRate;
  extern int micBitDepth;
  extern int micChannels;
  extern int micGain;
  extern const struct CommandEntry micCommands[];
  extern const size_t micCommandsCount;
  // Microphone stub functions
  inline bool initMicrophone() { return false; }
  inline bool stopMicrophone() { return true; }
  inline int16_t* captureAudioSamples(size_t sampleCount, size_t* outLen) { if (outLen) *outLen = 0; return nullptr; }
  inline int getAudioLevel() { return 0; }
  inline void applyMicAudioProcessing(int16_t* buf, size_t sampleCount, float gainMultiplier = 0.0f, bool filtersEnabled = true) {}
  inline void resetMicAudioProcessingState() {}
  inline float getMicSoftwareGainMultiplier() { return 0.0f; }
  inline int32_t getMicDcOffset() { return 0; }
  inline const char* buildMicrophoneStatusJson() { return "{}"; }
  inline MicRecordingState getMicRecordingState() { return MicRecordingState::IDLE; }
  inline const char* micRecordingStateName(MicRecordingState) { return "idle"; }
  inline bool micRecordingBusy() { return false; }
  inline bool micRecordingCapturing() { return false; }
  inline bool startRecording(uint32_t silenceStopMs = 0, bool trim = false) { return false; }
  inline bool stopRecording(uint32_t timeoutMs = 3000) { (void)timeoutMs; return true; }
  inline bool startRecordingOwned(MicRecordingOwner owner,
                                  uint32_t silenceStopMs = 0,
                                  bool trim = false) {
    (void)owner; (void)silenceStopMs; (void)trim; return false;
  }
  inline MicRecordingOwnedOp stopRecordingOwned(MicRecordingOwner owner,
                                                bool discard = false,
                                                uint32_t timeoutMs = 3000) {
    (void)owner; (void)discard; (void)timeoutMs;
    return ((uint32_t)(owner >> 32) && (uint32_t)owner)
               ? MicRecordingOwnedOp::NOT_FOUND
               : MicRecordingOwnedOp::INVALID_OWNER;
  }
  inline MicRecordingOwnedOp getRecordingResultOwned(MicRecordingOwner owner,
                                                     MicRecordingResult* out) {
    if (out) *out = MicRecordingResult{};
    return ((uint32_t)(owner >> 32) && (uint32_t)owner)
               ? MicRecordingOwnedOp::NOT_FOUND
               : MicRecordingOwnedOp::INVALID_OWNER;
  }
  inline MicRecordingOwnedOp deleteRecordingOwned(
      MicRecordingOwner owner, const char* expectedFilename) {
    (void)expectedFilename;
    return ((uint32_t)(owner >> 32) && (uint32_t)owner)
               ? MicRecordingOwnedOp::NOT_FOUND
               : MicRecordingOwnedOp::INVALID_OWNER;
  }
  inline void microphoneNotifySourceLost() {}
  inline int getRecordingCount() { return 0; }
  inline String getRecordingsList() { return "[]"; }
  inline bool deleteRecording(const char* filename) { return false; }
#endif

// =============================================================================
// NETWORK MODULE STUBS
// =============================================================================

#if !ENABLE_WIFI
  // WifiNetwork struct and constants come from System_WiFi.h (single source of truth)
  #include "System_WiFi.h"
  extern bool gSkipNTPInWifiConnect;
  extern const struct CommandEntry wifiCommands[];
  extern const size_t wifiCommandsCount;
  // WiFi stub functions
  inline void setupWiFi() {}
  inline bool connectToBestWiFiNetwork() { return false; }
  inline void setupNTP() {}
  inline bool upsertWiFiNetwork(const String& ssid, const String& password, int priority, bool enabled) { return false; }
  inline void sortWiFiByPriority() {}
  inline bool saveWiFiNetworks() { return false; }
  inline void wifiEventLogDrain() {}
  inline const char* cmd_wifitxpower(const String& argsInput) { return "WiFi disabled"; }
  inline const char* cmd_wifiautoreconnect(const String& argsInput) { return "WiFi disabled"; }
  // WiFi class stub
  class WiFiClass {
  public:
    bool isConnected() { return false; }
    String SSID() { return ""; }
    String localIP() { return "0.0.0.0"; }
    bool hostByName(const char*, IPAddress&) { return false; }
    void mode(int) {}
  };
  extern WiFiClass WiFi;
#endif

#if !ENABLE_HTTP_SERVER
  // HTTP server stubs when disabled
  // SessionEntry, LogoutReason, and constants come from WebServer_Server.h (single source of truth)
  #include "WebServer_Server.h"

  // Type/macro stubs — needed for *any* HTTP=0 build so headers that take
  // httpd_req_t* parameters can still compile. Compatible with the real
  // <esp_http_server.h> typedef (same struct tag), so safe to keep on even
  // when ENABLE_MIGRATION_TOOL=1 pulls in the real header in some TUs.
  #ifndef HW_HTTPD_TYPES_DEFINED
    #define HW_HTTPD_TYPES_DEFINED 1
    struct httpd_req;
    typedef struct httpd_req httpd_req_t;
    typedef void* httpd_handle_t;
  #endif
  #ifndef ESP_OK
    typedef int esp_err_t;
    #define ESP_OK 0
  #endif
  #define HTTPD_RESP_USE_STRLEN -1

  // Inline httpd_* function stubs — only when *nothing* in the build pulls
  // in the real <esp_http_server.h>. The migration tool always includes it
  // for restore-only mode, so under ENABLE_MIGRATION_TOOL=1 we must let the
  // real C-linkage declarations win to avoid signature/linkage conflicts
  // (real httpd_* fns are extern "C"; our inline definitions were not).
  #if !ENABLE_MIGRATION_TOOL
    inline size_t httpd_req_get_url_query_len(httpd_req_t* req) { return 0; }
    inline esp_err_t httpd_req_get_url_query_str(httpd_req_t* req, char* buf, size_t len) { return -1; }
    inline esp_err_t httpd_query_key_value(const char* qry, const char* key, char* val, size_t len) { return -1; }
    inline esp_err_t httpd_resp_set_type(httpd_req_t* req, const char* type) { return ESP_OK; }
    inline esp_err_t httpd_resp_send(httpd_req_t* req, const char* buf, int len) { return ESP_OK; }
    inline esp_err_t httpd_resp_send_chunk(httpd_req_t* req, const char* buf, int len) { return ESP_OK; }
    inline int httpd_req_to_sockfd(httpd_req_t* req) { return -1; }
    inline esp_err_t httpd_resp_set_status(httpd_req_t* req, const char* status) { return ESP_OK; }
  #endif // !ENABLE_MIGRATION_TOOL

  // App-level stubs — needed whenever the regular web UI is off, regardless
  // of whether the migration tool's restore-only server is being built.
  // These are *our* symbols (sessions, auth, SSE) — not in any external header.
  extern httpd_handle_t server;
  inline void startHttpServer() {}
  inline void stopHttpServer() {}
  inline bool isAdminUser(httpd_req_t* req) { return false; }
  inline void getClientIP(httpd_req_t* req, String& ipOut) { ipOut = "0.0.0.0"; }
  inline void getClientIP(httpd_req_t* req, char* ipBuf, size_t bufSize) { if (bufSize > 0) ipBuf[0] = '\0'; }
  // Session and SSE stub functions
  inline void sseEnqueueNotice(SessionEntry& s, const String& msg) {}
  inline bool sseDequeueNotice(SessionEntry& s, String& out) { return false; }
  inline void sseEnqueueEvent(SessionEntry& s, const char* name, const char* data) {}
  inline String getCookieSID(httpd_req_t* req) { return ""; }
  inline int findSessionIndexBySID(const String& sid) { return -1; }
  inline uint32_t webSessionEpochForSID(const String& sid) { return 0; }
  inline bool webSessionEpochIsLive(uint32_t epoch) { return false; }
  inline void storeLogoutReason(const String& ip, const String& reason) {}
  inline void enqueueTargetedRevokeForSessionIdx(int idx, const String& reason) {}
  // Auth and session stubs (gAuthUser/gAuthPass + rebuildExpectedAuthHeader
  // removed along with the Basic-Auth fast-path vestige in WebServer_Server.cpp)
  extern String gBootId;
  inline void broadcastSensorStatusToAllSessions() {}
  inline void broadcastEventToAllSessions(const char* eventName, const char* jsonData) {}
  inline void broadcastEventToSessionsIf(const char* eventName, const char* jsonData,
                                         bool (*allow)(const char* username, void* arg), void* arg) {}
  inline uint32_t sseEventDropsTotal() { return 0; }
  // No logAuthAttempt stub: it is real in every build now (System_Debug.cpp,
  // declared in System_User.h). The stub that used to live here also had `ip`
  // and `user` transposed relative to the real signature — same types, so it
  // compiled either way and would have silently produced reversed audit lines.
  inline bool authSuccessUnified(struct AuthContext& ctx, httpd_req_t* req) { return false; }
#endif

// ESP-NOW stubs now live in System_ESPNow.h (#else block) for grouping
#if !ENABLE_ESPNOW
  #include "System_ESPNow.h"
#endif

#if !ENABLE_MQTT
  // MQTT stub variables (global definitions)
  extern const struct CommandEntry mqttCommands[];
  extern const size_t mqttCommandsCount;
#endif

#endif // SYSTEM_SENSORSTUBS_H
