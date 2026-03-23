#ifndef SENSOR_STUBS_MINIMAL_H
#define SENSOR_STUBS_MINIMAL_H

#include <Arduino.h>
#include <IPAddress.h>
#include "System_BuildConfig.h"
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
  #include "i2csensor-mlx90640.h"  // Provides ThermalCache struct
  extern ThermalCache gThermalCache;
  extern bool thermalEnabled;
  extern bool thermalConnected;
  extern unsigned long thermalLastStopTime;
  extern TaskHandle_t thermalTaskHandle;
  extern volatile UBaseType_t gThermalWatermarkNow;
  extern volatile UBaseType_t gThermalWatermarkMin;
  extern const struct CommandEntry thermalCommands[];
  extern const size_t thermalCommandsCount;
  // Thermal stub functions
  inline int buildThermalDataJSON(char* buf, size_t bufSize) { return 0; }
  inline bool startThermalSensorInternal() { return false; }
#endif

#if !ENABLE_TOF_SENSOR
  #include "i2csensor-vl53l4cx.h"  // Provides TofCache struct
  extern TofCache gTofCache;
  extern bool tofEnabled;
  extern bool tofConnected;
  extern uint32_t tofLastStopTime;
  extern TaskHandle_t tofTaskHandle;
  extern volatile UBaseType_t gTofWatermarkNow;
  extern volatile UBaseType_t gTofWatermarkMin;
  extern const struct CommandEntry tofCommands[];
  extern const size_t tofCommandsCount;
  // ToF stub functions
  inline int buildToFDataJSON(char* buf, size_t bufSize) { return 0; }
  inline bool startToFSensorInternal() { return false; }
#endif

#if !ENABLE_IMU_SENSOR
  #include "i2csensor-bno055.h"  // Provides ImuCache and IMUActionState structs
  extern ImuCache gImuCache;
  extern bool imuEnabled;
  extern bool imuConnected;
  extern unsigned long imuLastStopTime;
  extern TaskHandle_t imuTaskHandle;
  extern volatile UBaseType_t gIMUWatermarkNow;
  extern volatile UBaseType_t gIMUWatermarkMin;
  extern const struct CommandEntry imuCommands[];
  extern const size_t imuCommandsCount;
  // IMU stub functions
  inline int buildIMUDataJSON(char* buf, size_t bufSize) { return 0; }
  inline bool startIMUSensorInternal() { return false; }
  void updateIMUActions();
#endif

#if !ENABLE_GAMEPAD_SENSOR
  #include "i2csensor-seesaw.h"  // Provides ControlCache struct
  extern ControlCache gControlCache;
  extern bool gamepadEnabled;
  extern bool gamepadConnected;
  extern unsigned long gamepadLastStopTime;
  extern TaskHandle_t gamepadTaskHandle;
  extern volatile UBaseType_t gGamepadWatermarkMin;
  extern volatile UBaseType_t gGamepadWatermarkNow;
  extern const struct CommandEntry gamepadCommands[];
  extern const size_t gamepadCommandsCount;
  // Gamepad stub functions
  inline const char* startGamepadInternal() { return "Gamepad disabled"; }
#endif

#if !ENABLE_OLED_DISPLAY
  // Only the most basic OLED stubs - variables referenced externally
  extern bool oledEnabled;
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
  #include "i2csensor-apds9960.h"  // Provides PeripheralCache struct
  extern PeripheralCache gPeripheralCache;
  extern bool apdsEnabled;
  extern bool apdsConnected;
  extern bool apdsColorEnabled;
  extern bool apdsProximityEnabled;
  extern bool apdsGestureEnabled;
  extern unsigned long apdsLastStopTime;
  extern TaskHandle_t apdsTaskHandle;
  extern const struct CommandEntry apdsCommands[];
  extern const size_t apdsCommandsCount;
  // APDS stub functions
  inline bool startAPDSSensorInternal() { return false; }
#endif

#if !ENABLE_GPS_SENSOR
  #include "i2csensor-pa1010d.h"  // Provides GPSCache struct
  extern GPSCache gGPSCache;
  extern bool gpsEnabled;
  extern bool gpsConnected;
  extern unsigned long gpsLastStopTime;
  extern class Adafruit_GPS* gPA1010D;
  extern TaskHandle_t gpsTaskHandle;
  extern const struct CommandEntry gpsCommands[];
  extern const size_t gpsCommandsCount;
  // GPS stub functions
  inline void startGPSInternal() {}
#endif

#if !ENABLE_FM_RADIO
  // FM Radio stubs when disabled
  extern bool fmRadioEnabled;
  extern bool fmRadioConnected;
  extern unsigned long fmRadioLastStopTime;
  extern bool radioInitialized;
  extern uint16_t fmRadioFrequency;
  extern uint8_t fmRadioVolume;
  extern bool fmRadioMuted;
  extern bool fmRadioStereo;
  extern char fmRadioStationName[9];
  extern char fmRadioStationText[65];
  extern uint8_t fmRadioRSSI;
  extern uint8_t fmRadioSNR;
  extern TaskHandle_t fmRadioTaskHandle;
  extern const struct CommandEntry fmRadioCommands[];
  extern const size_t fmRadioCommandsCount;
  inline bool initFMRadio() { return false; }
  inline void deinitFMRadio() {}
  inline void pollFMRadio() {}
  inline int buildFMRadioDataJSON(char* buf, size_t bufSize) { return 0; }
  inline void startFMRadioInternal() {}
#endif

#if !ENABLE_PRESENCE_SENSOR
  #include "i2csensor-sths34pf80.h"  // Provides PresenceCache struct
  extern PresenceCache gPresenceCache;
  extern bool presenceEnabled;
  extern bool presenceConnected;
  extern unsigned long presenceLastStopTime;
  extern TaskHandle_t presenceTaskHandle;
  extern const struct CommandEntry presenceCommands[];
  extern const size_t presenceCommandsCount;
  // Presence stub functions
  inline bool startPresenceSensorInternal() { return false; }
  inline bool initPresenceSensor() { return false; }
  inline bool readPresenceData() { return false; }
  inline int buildPresenceDataJSON(char* buf, size_t bufSize) { return 0; }
#endif

#if !ENABLE_RTC_SENSOR
  #include "i2csensor-ds3231.h"  // Provides RTCDateTime and RTCCache structs
  extern RTCCache gRTCCache;
  extern bool rtcEnabled;
  extern bool rtcConnected;
  extern unsigned long rtcLastStopTime;
  extern TaskHandle_t rtcTaskHandle;
  extern volatile UBaseType_t gRTCWatermarkNow;
  extern volatile UBaseType_t gRTCWatermarkMin;
  extern const struct CommandEntry rtcCommands[];
  extern const size_t rtcCommandsCount;
  // RTC stub functions
  void startRTCSensorInternal();
  inline int buildRTCDataJSON(char* buf, size_t bufSize) { return 0; }
#endif

#if !ENABLE_SERVO
  #include "i2csensor-pca9685.h"  // Provides ServoProfile, PCA9685_I2C_ADDRESS, MAX_SERVO_CHANNELS
  extern const struct CommandEntry servoCommands[];
  extern const size_t servoCommandsCount;
  // Servo stub functions
  inline bool initPCA9685() { return false; }
  inline void setServoAngle(uint8_t channel, int angle) {}
  inline void setPWMValue(uint8_t channel, uint16_t value) {}
#endif

#if !ENABLE_BLUETOOTH
  // Bluetooth stubs when disabled
  extern const struct CommandEntry bluetoothCommands[];
  extern const size_t bluetoothCommandsCount;
#endif

#if !ENABLE_CAMERA_SENSOR
  // Camera stubs when disabled
  extern bool cameraEnabled;
  extern bool cameraConnected;
  extern bool cameraStreaming;
  extern const char* cameraModel;
  extern int cameraWidth;
  extern int cameraHeight;
  extern const struct CommandEntry cameraCommands[];
  extern const size_t cameraCommandsCount;
  inline bool initCamera() { return false; }
  inline void stopCamera() {}
  inline uint8_t* captureFrame(size_t* outLen) { if (outLen) *outLen = 0; return nullptr; }
  inline const char* buildCameraStatusJson() { return "{}"; }
#endif

#if !ENABLE_MICROPHONE_SENSOR
  // Microphone stubs when disabled
  extern bool micEnabled;
  extern bool micConnected;
  extern bool micRecording;
  extern int micSampleRate;
  extern int micBitDepth;
  extern int micChannels;
  extern int micGain;
  extern const struct CommandEntry micCommands[];
  extern const size_t micCommandsCount;
  // Microphone stub functions
  inline bool initMicrophone() { return false; }
  inline void stopMicrophone() {}
  inline int16_t* captureAudioSamples(size_t sampleCount, size_t* outLen) { if (outLen) *outLen = 0; return nullptr; }
  inline int getAudioLevel() { return 0; }
  inline void applyMicAudioProcessing(int16_t* buf, size_t sampleCount, float gainMultiplier = 0.0f, bool filtersEnabled = true) {}
  inline void resetMicAudioProcessingState() {}
  inline float getMicSoftwareGainMultiplier() { return 0.0f; }
  inline int32_t getMicDcOffset() { return 0; }
  inline const char* buildMicrophoneStatusJson() { return "{}"; }
  inline bool startRecording() { return false; }
  inline void stopRecording() {}
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
  extern httpd_handle_t server;
  inline void startHttpServer() {}
  inline void stopHttpServer() {}
  inline bool isAdminUser(httpd_req_t* req) { return false; }
  inline void getClientIP(httpd_req_t* req, String& ipOut) { ipOut = "0.0.0.0"; }
  inline void getClientIP(httpd_req_t* req, char* ipBuf, size_t bufSize) { if (bufSize > 0) ipBuf[0] = '\0'; }
  inline size_t httpd_req_get_url_query_len(httpd_req_t* req) { return 0; }
  inline esp_err_t httpd_req_get_url_query_str(httpd_req_t* req, char* buf, size_t len) { return -1; }
  inline esp_err_t httpd_query_key_value(const char* qry, const char* key, char* val, size_t len) { return -1; }
  inline esp_err_t httpd_resp_set_type(httpd_req_t* req, const char* type) { return ESP_OK; }
  inline esp_err_t httpd_resp_send(httpd_req_t* req, const char* buf, int len) { return ESP_OK; }
  inline esp_err_t httpd_resp_send_chunk(httpd_req_t* req, const char* buf, int len) { return ESP_OK; }
  inline int httpd_req_to_sockfd(httpd_req_t* req) { return -1; }
  // Session and SSE stub functions
  inline void sseEnqueueNotice(SessionEntry& s, const String& msg) {}
  inline bool sseDequeueNotice(SessionEntry& s, String& out) { return false; }
  inline void sseEnqueueEvent(SessionEntry& s, const char* name, const char* data) {}
  inline String getCookieSID(httpd_req_t* req) { return ""; }
  inline int findSessionIndexBySID(const String& sid) { return -1; }
  inline esp_err_t httpd_resp_set_status(httpd_req_t* req, const char* status) { return ESP_OK; }
  inline void storeLogoutReason(const String& ip, const String& reason) {}
  inline void enqueueTargetedRevokeForSessionIdx(int idx, const String& reason) {}
  // Auth and session stubs
  extern String gAuthUser;
  extern String gAuthPass;
  extern String gBootId;
  inline void rebuildExpectedAuthHeader() {}
  inline void broadcastSensorStatusToAllSessions() {}
  inline void broadcastEventToAllSessions(const char* eventName, const char* jsonData) {}
  inline void logAuthAttempt(bool success, const char* transport, const String& ip, const String& user, const String& reason) {}
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

#endif // SENSOR_STUBS_MINIMAL_H
