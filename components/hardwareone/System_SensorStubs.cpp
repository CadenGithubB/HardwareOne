#include "System_SensorStubs.h"
#include "System_Utils.h"
#include "WebServer_Utils.h"

// Forward declarations for stubs
class Adafruit_GPS;
class Adafruit_SSD1306;

// =============================================================================
// GLOBAL SENSOR STUB VARIABLE DEFINITIONS
// =============================================================================
// These provide the actual variable definitions that extern declarations reference

#if !ENABLE_HTTP_SERVER
WebMirrorBuf gWebMirror;
size_t gWebMirrorCap = 0;
#endif

#if !ENABLE_THERMAL_SENSOR
// Thermal stub variables (global definitions)
ThermalCache gThermalCache;
bool gThermalRunning = false;
bool gThermalConnected = false;
unsigned long gThermalLastStopTime = 0;
TaskHandle_t gThermalTaskHandle = nullptr;
volatile UBaseType_t gThermalWatermarkNow = 0;
volatile UBaseType_t gThermalWatermarkMin = 0;
const struct CommandEntry thermalCommands[] = {};
const size_t thermalCommandsCount = 0;
#endif

#if !ENABLE_TOF_SENSOR
// ToF stub variables (global definitions)
TofCache gTofCache;
bool gTofRunning = false;
bool gTofConnected = false;
uint32_t gTofLastStopTime = 0;
TaskHandle_t gTofTaskHandle = nullptr;
volatile UBaseType_t gTofWatermarkNow = 0;
volatile UBaseType_t gTofWatermarkMin = 0;
const struct CommandEntry tofCommands[] = {};
const size_t tofCommandsCount = 0;
#endif

#if !ENABLE_IMU_SENSOR
// IMU stub variables (global definitions)
ImuCache gImuCache;
bool gImuRunning = false;
bool gImuConnected = false;
unsigned long gImuLastStopTime = 0;
TaskHandle_t gImuTaskHandle = nullptr;
volatile UBaseType_t gImuWatermarkNow = 0;
volatile UBaseType_t gImuWatermarkMin = 0;
const struct CommandEntry imuCommands[] = {};
const size_t imuCommandsCount = 0;

void imuUpdateActions() {
}
#endif

#if !ENABLE_GAMEPAD_SENSOR
// Gamepad stub variables. Only the symbols NOT supplied by the ANO encoder
// driver get stubbed here — gInputRunning/gInputConnected/gInputCache
// are populated live by the ANO task when ENABLE_ANO_ENCODER is on, so the
// OLED input pipeline sees real state instead of zeros.
#if !ENABLE_ANO_ENCODER
InputCache gInputCache;
bool gInputRunning = false;
bool gInputConnected = false;
#endif
unsigned long gGamepadLastStopTime = 0;
TaskHandle_t gInputTaskHandle = nullptr;
volatile UBaseType_t gGamepadWatermarkMin = 0;
volatile UBaseType_t gGamepadWatermarkNow = 0;
const struct CommandEntry gamepadCommands[] = {};
const size_t gamepadCommandsCount = 0;
#endif

#if !ENABLE_APDS_SENSOR
// APDS stub variables (global definitions)
APDSCache gApdsCache;
bool gApdsConnected = false;
bool gApdsColorRunning = false;
bool gApdsProximityRunning = false;
bool gApdsGestureRunning = false;
unsigned long gApdsLastStopTime = 0;
TaskHandle_t gApdsTaskHandle = nullptr;
const struct CommandEntry apdsCommands[] = {};
const size_t apdsCommandsCount = 0;
#endif

#if !ENABLE_GPS_SENSOR
// GPS stub variables (global definitions)
GPSCache gGpsCache;
bool gGpsRunning = false;
bool gGpsConnected = false;
unsigned long gGpsLastStopTime = 0;
Adafruit_GPS* gPA1010D = nullptr;
TaskHandle_t gGpsTaskHandle = nullptr;
const struct CommandEntry gpsCommands[] = {};
const size_t gpsCommandsCount = 0;
#endif

#if !ENABLE_PRESENCE_SENSOR
// Presence sensor stub variables (global definitions)
PresenceCache gPresenceCache;
bool gPresenceRunning = false;
bool gPresenceConnected = false;
unsigned long gPresenceLastStopTime = 0;
TaskHandle_t gPresenceTaskHandle = nullptr;
const struct CommandEntry presenceCommands[] = {};
const size_t presenceCommandsCount = 0;
#endif

#if !ENABLE_OLED_DISPLAY
// OLED stub variables (global definitions)
bool gOledRunning = false;
bool oledConnected = false;
Adafruit_SSD1306* oledDisplay = nullptr;
const struct CommandEntry oledCommands[] = {};
const size_t oledCommandsCount = 0;
#endif

#if !ENABLE_FM_RADIO
// FM Radio stub variables (global definitions)
bool gFmRadioRunning = false;
bool gFmRadioConnected = false;
unsigned long gFmRadioLastStopTime = 0;
bool gRadioInitialized = false;
FMRadioCache gFmRadioCache;
TaskHandle_t gFmRadioTaskHandle = nullptr;
const struct CommandEntry fmRadioCommands[] = {};
const size_t fmRadioCommandsCount = 0;
#endif

#if !ENABLE_RTC_SENSOR
// RTC stub variables (global definitions)
// Types are defined in System_SensorStubs.h when disabled
RTCCache gRtcCache = {nullptr, {0, 0, 0, 0, 0, 0, 0}, 0.0f, false, 0};
bool gRtcRunning = false;
bool gRtcConnected = false;
unsigned long gRtcLastStopTime = 0;
TaskHandle_t gRtcTaskHandle = nullptr;
volatile UBaseType_t gRtcWatermarkNow = 0;
volatile UBaseType_t gRtcWatermarkMin = 0;
const struct CommandEntry rtcCommands[] = {};
const size_t rtcCommandsCount = 0;
bool rtcStartInternal() { return false; }
#endif

#if !ENABLE_SERVO
// Servo stub variables (global definitions)
const struct CommandEntry servoCommands[] = {};
const size_t servoCommandsCount = 0;
#endif

#if !ENABLE_MICROPHONE
// Microphone stub variables (global definitions). Gated on ENABLE_MICROPHONE
// (not ..._SENSOR) in lockstep with System_Microphone.cpp: whenever the real mic
// module compiles (PDM board OR G2-capable board), these stubs MUST be absent or
// the linker sees duplicate definitions of all 9 globals + micCommands[].
bool gMicRunning = false;
bool micConnected = false;
int micSampleRate = 0;
int micBitDepth = 0;
int micChannels = 0;
int micGain = 0;
const struct CommandEntry micCommands[] = {};
const size_t micCommandsCount = 0;
#endif

#if !ENABLE_CAMERA_SENSOR
// Camera stub variables (global definitions)
bool gCameraRunning = false;
bool cameraConnected = false;
bool cameraStreaming = false;
bool cameraDetected = false;   // no camera compiled in — never latches
const char* cameraModel = "None";
int cameraWidth = 0;
int cameraHeight = 0;
const struct CommandEntry cameraCommands[] = {};
const size_t cameraCommandsCount = 0;
#endif

#if !ENABLE_BLUETOOTH
// Bluetooth stub variables (global definitions)
const struct CommandEntry bluetoothCommands[] = {};
const size_t bluetoothCommandsCount = 0;
#endif

// =============================================================================
// NETWORK MODULE STUB VARIABLE DEFINITIONS
// =============================================================================

#if !ENABLE_WIFI
// WiFi stub variables (global definitions)
WifiNetwork* gWifiNetworks = nullptr;
int gWifiNetworkCount = 0;
bool gSkipNTPInWifiConnect = false;
const struct CommandEntry wifiCommands[] = {};
const size_t wifiCommandsCount = 0;
// WiFi stub class instance - defined in header as inline class
WiFiClass WiFi;
#endif

#if !ENABLE_HTTP_SERVER
// HTTP server stub variables (global definitions)
httpd_handle_t server = nullptr;
SessionEntry* gSessions = nullptr;
LogoutReason* gLogoutReasons = nullptr;
char* gJsonResponseBuffer = nullptr;
String gBootId = "";
// Pre-existing readers (G2_Page_Network, System_WiFi, OLED_Mode_Network) reference
// this even with HTTP=0. Migration-tool-only builds need the stub too because
// startRestoreOnlyHttpServer() doesn't set it.
bool gServerIsHttps = false;
// Deferred-teardown symbols (WebServer_Handle.h). Nothing can be in flight with
// no server, so the stub stop always "succeeds" and the tick is a no-op.
volatile int gWebCmdWaiters = 0;
bool httpServerStopSafe() { return true; }
void httpServerStopPendingTick() {}
#endif

#if !ENABLE_ESPNOW
// ESP-NOW stub variables (global definitions)
static EspNowState _gEspNowStub = { false, ESPNOW_MODE_DIRECT, "", 0, false, nullptr };
EspNowState* gEspNow = &_gEspNowStub;
MeshPeerHealth gMeshPeers[MAX_MESH_PEERS] = {};
MeshTopoNode* gMeshTopology = nullptr;
bool gMeshActivitySuspended = false;
const struct CommandEntry espNowCommands[] = {};
const size_t espNowCommandsCount = 0;
#endif

#if !ENABLE_MQTT
// MQTT stub variables (global definitions)
const struct CommandEntry mqttCommands[] = {};
const size_t mqttCommandsCount = 0;
#endif
