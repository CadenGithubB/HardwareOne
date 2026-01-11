#include "System_SensorStubs.h"
#include "System_Utils.h"

// Forward declarations for stubs
class Adafruit_GPS;
class Adafruit_SSD1306;

// =============================================================================
// GLOBAL SENSOR STUB VARIABLE DEFINITIONS
// =============================================================================
// These provide the actual variable definitions that extern declarations reference

#if !ENABLE_THERMAL_SENSOR
// Thermal stub variables (global definitions)
ThermalCache gThermalCache;
bool thermalEnabled = false;
bool thermalConnected = false;
TaskHandle_t thermalTaskHandle = nullptr;
volatile UBaseType_t gThermalWatermarkNow = 0;
volatile UBaseType_t gThermalWatermarkMin = 0;
const struct CommandEntry thermalCommands[] = {};
const size_t thermalCommandsCount = 0;
#endif

#if !ENABLE_TOF_SENSOR
// ToF stub variables (global definitions)
TofCache gTofCache;
bool tofEnabled = false;
bool tofConnected = false;
TaskHandle_t tofTaskHandle = nullptr;
volatile UBaseType_t gTofWatermarkNow = 0;
volatile UBaseType_t gTofWatermarkMin = 0;
const struct CommandEntry tofCommands[] = {};
const size_t tofCommandsCount = 0;
#endif

#if !ENABLE_IMU_SENSOR
// IMU stub variables (global definitions)
ImuCache gImuCache;
bool imuEnabled = false;
bool imuConnected = false;
TaskHandle_t imuTaskHandle = nullptr;
volatile UBaseType_t gIMUWatermarkNow = 0;
volatile UBaseType_t gIMUWatermarkMin = 0;
const struct CommandEntry imuCommands[] = {};
const size_t imuCommandsCount = 0;

void updateIMUActions() {
}
#endif

#if !ENABLE_GAMEPAD_SENSOR
// Gamepad stub variables (global definitions)
ControlCache gControlCache;
bool gamepadEnabled = false;
bool gamepadConnected = false;
TaskHandle_t gamepadTaskHandle = nullptr;
const struct CommandEntry gamepadCommands[] = {};
const size_t gamepadCommandsCount = 0;
#warning "GAMEPAD STUBS ARE BEING COMPILED - THIS SHOULD NOT HAPPEN IF GAMEPAD IS ENABLED"
#endif

#if !ENABLE_APDS_SENSOR
// APDS stub variables (global definitions)
PeripheralCache gPeripheralCache;
bool apdsConnected = false;
bool apdsColorEnabled = false;
bool apdsProximityEnabled = false;
bool apdsGestureEnabled = false;
const struct CommandEntry apdsCommands[] = {};
const size_t apdsCommandsCount = 0;
#endif

#if !ENABLE_GPS_SENSOR
// GPS stub variables (global definitions)
bool gpsEnabled = false;
bool gpsConnected = false;
Adafruit_GPS* gPA1010D = nullptr;
const struct CommandEntry gpsCommands[] = {};
const size_t gpsCommandsCount = 0;
#endif

#if !ENABLE_OLED_DISPLAY
// OLED stub variables (global definitions)
bool oledEnabled = false;
bool oledConnected = false;
Adafruit_SSD1306* oledDisplay = nullptr;
const struct CommandEntry oledCommands[] = {};
const size_t oledCommandsCount = 0;
#endif

#if !ENABLE_FM_RADIO
// FM Radio stub variables (global definitions)
bool fmRadioEnabled = false;
bool fmRadioConnected = false;
bool radioInitialized = false;
uint16_t fmRadioFrequency = 0;
uint8_t fmRadioVolume = 0;
bool fmRadioMuted = false;
bool fmRadioStereo = false;
char fmRadioStationName[9] = "";
char fmRadioStationText[65] = "";
uint8_t fmRadioRSSI = 0;
uint8_t fmRadioSNR = 0;
TaskHandle_t fmRadioTaskHandle = nullptr;
const struct CommandEntry fmRadioCommands[] = {};
const size_t fmRadioCommandsCount = 0;
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
String gAuthUser = "";
String gAuthPass = "";
String gBootId = "";
#endif

#if !ENABLE_ESPNOW
// ESP-NOW stub variables (global definitions)
static EspNowState _gEspNowStub = { false, ESPNOW_MODE_DISABLED, "", 0, false, nullptr };
EspNowState* gEspNow = &_gEspNowStub;
MeshPeerHealth gMeshPeers[MAX_MESH_PEERS] = {};
MeshTopoNode* gMeshTopology = nullptr;
bool gMeshActivitySuspended = false;
const struct CommandEntry espNowCommands[] = {};
const size_t espNowCommandsCount = 0;
#endif
