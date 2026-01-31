#ifndef SENSOR_STUBS_MINIMAL_H
#define SENSOR_STUBS_MINIMAL_H

#include <Arduino.h>
#include <IPAddress.h>
#include "System_BuildConfig.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// =============================================================================
// MINIMAL SENSOR STUBS - ONLY WHERE ABSOLUTELY REQUIRED
// =============================================================================
// These stubs only exist to prevent compilation errors from extern references
// No complex functionality - just basic variables to satisfy the linker

#if !ENABLE_THERMAL_SENSOR
  // Thermal stub structure (matches real ThermalCache exactly)
  struct ThermalCache {
    SemaphoreHandle_t mutex = nullptr;
    int16_t* thermalFrame = nullptr;
    float* thermalInterpolated = nullptr;
    int thermalInterpolatedWidth = 0;
    int thermalInterpolatedHeight = 0;
    float thermalMinTemp = 0.0;
    float thermalMaxTemp = 0.0;
    float thermalAvgTemp = 0.0;
    unsigned long thermalLastUpdate = 0;
    bool thermalDataValid = false;
    uint32_t thermalSeq = 0;
  };
  extern ThermalCache gThermalCache;
  extern bool thermalEnabled;
  extern bool thermalConnected;
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
  // ToF stub structure (matches real TofCache exactly)
  struct TofCache {
    SemaphoreHandle_t mutex = nullptr;
    struct TofObject {
      bool detected = false;
      bool valid = false;
      int distance_mm = 0;
      float distance_cm = 0.0;
      int status = 0;
      float smoothed_distance_mm = 0.0;
      float smoothed_distance_cm = 0.0;
      bool hasHistory = false;
    } tofObjects[4];
    int tofTotalObjects = 0;
    unsigned long tofLastUpdate = 0;
    bool tofDataValid = false;
    uint32_t tofSeq = 0;
  };
  extern TofCache gTofCache;
  extern bool tofEnabled;
  extern bool tofConnected;
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
  // IMU stub structure (matches real ImuCache exactly)
  struct ImuCache {
    SemaphoreHandle_t mutex = nullptr;
    float accelX = 0.0, accelY = 0.0, accelZ = 0.0;
    float gyroX = 0.0, gyroY = 0.0, gyroZ = 0.0;
    float imuTemp = 0.0;
    float oriYaw = 0.0, oriPitch = 0.0, oriRoll = 0.0;
    unsigned long imuLastUpdate = 0;
    bool imuDataValid = false;
    uint32_t imuSeq = 0;
  };
  extern ImuCache gImuCache;
  extern bool imuEnabled;
  extern bool imuConnected;
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
  // Gamepad stub structure (matches real ControlCache exactly)
  struct ControlCache {
    SemaphoreHandle_t mutex = nullptr;
    uint32_t gamepadButtons = 0;
    int gamepadX = 0, gamepadY = 0;
    unsigned long gamepadLastUpdate = 0;
    bool gamepadDataValid = false;
    uint32_t gamepadSeq = 0;
  };
  extern ControlCache gControlCache;
  extern bool gamepadEnabled;
  extern bool gamepadConnected;
  extern TaskHandle_t gamepadTaskHandle;
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
  // APDS command stub
  inline const char* cmd_apdscolorstart(const String& cmd) { return "APDS disabled"; }
  // APDS stub structure (matches real PeripheralCache exactly)
  struct PeripheralCache {
    SemaphoreHandle_t mutex = nullptr;
    uint16_t apdsRed = 0, apdsGreen = 0, apdsBlue = 0, apdsClear = 0;
    uint8_t apdsProximity = 0;
    uint8_t apdsGesture = 0;
    unsigned long apdsLastUpdate = 0;
    bool apdsDataValid = false;
  };
  extern PeripheralCache gPeripheralCache;
  extern bool apdsConnected;
  extern bool apdsColorEnabled;
  extern bool apdsProximityEnabled;
  extern bool apdsGestureEnabled;
  extern const struct CommandEntry apdsCommands[];
  extern const size_t apdsCommandsCount;
#endif

#if !ENABLE_GPS_SENSOR
  // GPS stubs when disabled
  extern bool gpsEnabled;
  extern bool gpsConnected;
  extern class Adafruit_GPS* gPA1010D;
  extern const struct CommandEntry gpsCommands[];
  extern const size_t gpsCommandsCount;
  inline void startGPSInternal() {}
#endif

#if !ENABLE_FM_RADIO
  // FM Radio stubs when disabled
  extern bool fmRadioEnabled;
  extern bool fmRadioConnected;
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

#if !ENABLE_RTC_SENSOR
  // RTC stubs when disabled - declarations only, definitions in System_SensorStubs.cpp
  void startRTCSensorInternal();
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

// =============================================================================
// NETWORK MODULE STUBS
// =============================================================================

#if !ENABLE_WIFI
  // WiFi stubs when disabled
  #define MAX_WIFI_NETWORKS 8
  struct WifiNetwork {
    String ssid;
    String password;
    int priority;
    bool hidden;
    uint32_t lastConnected;
  };
  extern WifiNetwork* gWifiNetworks;
  extern int gWifiNetworkCount;
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
  inline const char* cmd_wifitxpower(const String& cmd) { return "WiFi disabled"; }
  inline const char* cmd_wifiautoreconnect(const String& cmd) { return "WiFi disabled"; }
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
  struct httpd_req;
  typedef struct httpd_req httpd_req_t;
  typedef void* httpd_handle_t;
  typedef int esp_err_t;
  #define ESP_OK 0
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
  // Session stubs
  #define MAX_SESSIONS 8
  #define MAX_LOGOUT_REASONS 16
  #define JSON_RESPONSE_SIZE 4096
  struct SessionEntry {
    String sid;
    String user;
    String ip;
    uint32_t created;
    uint32_t lastAccess;
    uint32_t createdAt;
    uint32_t lastSeen;
    uint32_t expiresAt;
    int sockfd;
    uint8_t nqCount;
    uint8_t eqCount;
    bool needsNotificationTick;
  };
  struct LogoutReason {
    String sid;
    String reason;
    uint32_t timestamp;
  };
  extern SessionEntry* gSessions;
  extern LogoutReason* gLogoutReasons;
  extern char* gJsonResponseBuffer;
  inline void sseEnqueueNotice(SessionEntry& s, const String& msg) {}
  inline bool sseDequeueNotice(SessionEntry& s, String& out) { return false; }
  inline void sseEnqueueEvent(SessionEntry& s, const String& name, const String& data) {}
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
  inline void logAuthAttempt(bool success, const char* transport, const String& ip, const String& user, const String& reason) {}
  // Note: tgRequireAuth, tgRequireAdmin are implemented in user_system.cpp with #else stubs
  inline bool authSuccessUnified(struct AuthContext& ctx, httpd_req_t* req) { return false; }
#endif

#if !ENABLE_ESPNOW
  // ESP-NOW stubs when disabled
  enum EspNowMode { ESPNOW_MODE_DISABLED = 0, ESPNOW_MODE_BROADCAST, ESPNOW_MODE_MESH, ESPNOW_MODE_DIRECT };
  #define MESH_ROLE_MASTER 0
  #define MESH_ROLE_BACKUP_MASTER 1
  #define MESH_ROLE_WORKER 2
  #define MAX_MESH_PEERS 16
  struct MeshPeerHealth {
    uint8_t mac[6];
    String name;
    bool isOnline;
    bool isActive;
    uint32_t lastSeen;
    int8_t rssi;
    uint8_t role;
  };
  struct MeshTopoNode {
    uint8_t mac[6];
    String name;
    uint8_t role;
    bool isOnline;
  };
  struct EspNowState {
    bool initialized;
    EspNowMode mode;
    String passphrase;
    uint8_t channel;
    bool encryptionEnabled;
    void* reserved;
  };
  extern EspNowState* gEspNow;
  extern MeshPeerHealth gMeshPeers[MAX_MESH_PEERS];
  extern MeshTopoNode* gMeshTopology;
  extern bool gMeshActivitySuspended;
  extern const struct CommandEntry espNowCommands[];
  extern const size_t espNowCommandsCount;
  // ESP-NOW stub functions
  inline const char* checkEspNowFirstTimeSetup() { return "ESP-NOW disabled"; }
  inline const char* cmd_espnow_init(const String& cmd) { return "ESP-NOW disabled"; }
  inline void sendEspNowStreamMessage(const char* topic, const char* payload) {}
  inline void processMessageQueue() {}
  inline void cleanupExpiredChunkedMessage() {}
  inline void cleanupExpiredBufferedPeers() {}
  inline void cleanupTimedOutChunks() {}
  inline bool isSelfMac(const uint8_t* mac) { return false; }
  inline bool isMeshPeerAlive(MeshPeerHealth* peer) { return false; }
#endif

#if !ENABLE_MQTT
  // MQTT stub variables (global definitions)
  const struct CommandEntry mqttCommands[] = {};
  const size_t mqttCommandsCount = 0;
#endif

#endif // SENSOR_STUBS_MINIMAL_H
