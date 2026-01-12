#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include <ArduinoJson.h>

#include "System_BuildConfig.h"

// CommandEntry is defined in system_utils.h (included by files that need it)
// Forward declare here for header-only usage
struct CommandEntry;

// Settings structure - shared between .ino and .cpp files
struct Settings {
  // Constructor to ensure all String members are initialized
  Settings()
    : wifiSSID(""),
      wifiPassword(""),
      wifiEnabled(true),
      wifiAutoReconnect(true),
      cliHistorySize(10),
      ntpServer(""),
      tzOffsetMinutes(0),
      outSerial(true),
      outWeb(false),
      outTft(false),
      thermalPollingMs(250),
      tofPollingMs(220),
      tofStabilityThreshold(3),
      thermalPaletteDefault(""),
      thermalInterpolationEnabled(true),
      thermalInterpolationSteps(5),
      thermalInterpolationBufferSize(2),
      thermalUpscaleFactor(1),
      thermalEWMAFactor(0.2f),
      thermalTransitionMs(80),
      tofTransitionMs(200),
      tofUiMaxDistanceMm(3400),
      i2cClockThermalHz(800000),
      i2cClockToFHz(200000),
      thermalTargetFps(8),
      thermalWebMaxFps(10),
      thermalRollingMinMaxEnabled(true),
      thermalRollingMinMaxAlpha(0.6f),
      thermalRollingMinMaxGuardC(0.3f),
      thermalTemporalAlpha(0.5f),
      thermalRotation(0),
      thermalDevicePollMs(100),
      tofDevicePollMs(220),
      imuDevicePollMs(200),
      imuOrientationCorrectionEnabled(true),
      imuOrientationMode(8),
      imuPitchOffset(0.0f),
      imuRollOffset(0.0f),
      imuYawOffset(0.0f),
      debugHttp(true),
      debugSse(true),
      debugCli(true),
      debugSensorsFrame(true),
      debugSensorsData(true),
      debugSensorsGeneral(true),
      debugWifi(true),
      debugStorage(true),
      debugPerformance(true),
      debugDateTime(true),
      debugCommandFlow(true),
      debugUsers(true),
      debugSystem(true),
      debugAutomations(true),
      debugLogger(true),
      debugEspNowStream(true),
      debugEspNowCore(true),
      debugEspNowRouter(true),
      debugEspNowMesh(false),
      debugEspNowTopo(false),
      debugEspNowEncryption(false),
      debugAutoScheduler(false),
      debugAutoExec(false),
      debugAutoCondition(false),
      debugAutoTiming(false),
      debugMemory(true),
      debugCommandSystem(true),
      debugSettingsSystem(true),
      debugFmRadio(true),
      logLevel(3),                    // Default: LOG_LEVEL_DEBUG (show everything)
      memorySampleIntervalSec(30),
      espnowenabled(false),
      espnowmesh(false),
      espnowUserSyncEnabled(false),
      espnowDeviceName(""),
      espnowFirstTimeSetup(false),
      espnowPassphrase(""),
      meshRole(0),
      meshMasterMAC(""),
      meshBackupMAC(""),
      meshMasterHeartbeatInterval(10000),
      meshFailoverTimeout(20000),
      meshWorkerStatusInterval(30000),
      meshTopoDiscoveryInterval(0),
      meshTopoAutoRefresh(false),
      meshHeartbeatBroadcast(true),
      meshTTL(3),
      meshAdaptiveTTL(false),
      automationsEnabled(true),
      i2cBusEnabled(true),
      i2cSensorsEnabled(true),
      ledBrightness(100),
      ledStartupEnabled(true),
      ledStartupEffect(""),
      ledStartupColor(""),
      ledStartupColor2(""),
      ledStartupDuration(1000),
      oledEnabled(true),
      oledAutoInit(true),
      localDisplayRequireAuth(true),
      oledBootMode(""),
      oledDefaultMode(""),
      oledBootDuration(2000),
      oledUpdateInterval(200),
      oledBrightness(255),
      oledThermalScale(2.5f),
      oledThermalColorMode(""),
      gamepadAutoStart(true),
      thermalAutoStart(false),
      tofAutoStart(false),
      imuAutoStart(false),
      gpsAutoStart(false),
      fmRadioAutoStart(false),
      apdsAutoStart(false),
      httpAutoStart(true),
      bluetoothAutoStart(true),
      bluetoothRequireAuth(true),
      bleDeviceName("HardwareOne"),
      bleTxPower(3),
      bleGlassesLeftMAC(""),
      bleGlassesRightMAC(""),
      bleRingMAC(""),
      blePhoneMAC(""),
      powerMode(0),
      powerAutoMode(false),
      powerBatteryThreshold(20),
      powerDisplayDimLevel(30) {
    // String members are now initialized in initializer list
  }

  String wifiSSID;
  String wifiPassword;
  bool wifiEnabled;        // Enable/disable WiFi at boot (default: true)
  bool wifiAutoReconnect;
  int cliHistorySize;
  String ntpServer;
  int tzOffsetMinutes;
  bool outSerial;  // persist output lanes
  bool outWeb;
  bool outTft;
  // Sensors UI (non-advanced)
  int thermalPollingMs;
  int tofPollingMs;
  int tofStabilityThreshold;
  String thermalPaletteDefault;
  // Thermal interpolation settings
  bool thermalInterpolationEnabled;
  int thermalInterpolationSteps;
  int thermalInterpolationBufferSize;
  int thermalUpscaleFactor;     // 1x, 2x, 4x upscaling on-device
  // Advanced UI + firmware-affecting
  float thermalEWMAFactor;
  int thermalTransitionMs;
  int tofTransitionMs;
  int tofUiMaxDistanceMm;
  int i2cClockThermalHz;
  int i2cClockToFHz;
  int thermalTargetFps;
  int thermalWebMaxFps;
  // Thermal rolling min/max stabilization
  bool thermalRollingMinMaxEnabled;
  float thermalRollingMinMaxAlpha;
  float thermalRollingMinMaxGuardC;
  // Temporal frame smoothing (reduces interpolation jitter, but can cause ghosting)
  float thermalTemporalAlpha;  // 0.0-1.0, higher = more smoothing but more ghosting
  // Thermal rotation (0=0°, 1=90°, 2=180°, 3=270°)
  int thermalRotation;
  // Device-side sensor settings (affect firmware runtime)
  int thermalDevicePollMs;
  int tofDevicePollMs;
  int imuDevicePollMs;
  int gpsDevicePollMs;
  int apdsDevicePollMs;
  int gamepadDevicePollMs;
  int fmRadioDevicePollMs;
  // IMU UI settings (client-side visualization)
  int imuPollingMs;
  float imuEWMAFactor;
  int imuTransitionMs;
  int imuWebMaxFps;
  // IMU orientation correction settings (device-side)
  bool imuOrientationCorrectionEnabled;
  int imuOrientationMode;  // 0=normal, 1=flip_pitch, 2=flip_roll, 3=flip_yaw, 4=flip_pitch_roll, etc.
  float imuPitchOffset;    // Pitch correction offset in degrees
  float imuRollOffset;     // Roll correction offset in degrees
  float imuYawOffset;      // Yaw correction offset in degrees
  // Debug settings (parent flags - kept for backward compatibility)
  bool debugHttp;
  bool debugSse;
  bool debugCli;
  bool debugSensorsFrame;
  bool debugSensorsData;
  bool debugSensorsGeneral;
  bool debugWifi;
  bool debugStorage;
  bool debugPerformance;
  bool debugDateTime;
  bool debugCommandFlow;
  bool debugUsers;
  bool debugSystem;
  bool debugAutomations;
  bool debugLogger;
  bool debugEspNowStream;
  bool debugEspNowCore;
  bool debugEspNowRouter;
  bool debugEspNowMesh;
  bool debugEspNowTopo;
  bool debugEspNowEncryption;
  bool debugAutoScheduler;
  bool debugAutoExec;
  bool debugAutoCondition;
  bool debugAutoTiming;
  bool debugMemory;
  bool debugCommandSystem;
  bool debugSettingsSystem;
  bool debugFmRadio;
  // Auth sub-flags
  bool debugAuthSessions;
  bool debugAuthCookies;
  bool debugAuthLogin;
  bool debugAuthBootId;
  // HTTP sub-flags
  bool debugHttpHandlers;
  bool debugHttpRequests;
  bool debugHttpResponses;
  bool debugHttpStreaming;
  // WiFi sub-flags
  bool debugWifiConnection;
  bool debugWifiConfig;
  bool debugWifiScanning;
  bool debugWifiDriver;
  // Storage sub-flags
  bool debugStorageFiles;
  bool debugStorageJson;
  bool debugStorageSettings;
  bool debugStorageMigration;
  // System sub-flags
  bool debugSystemBoot;
  bool debugSystemConfig;
  bool debugSystemTasks;
  bool debugSystemHardware;
  // Users sub-flags
  bool debugUsersMgmt;
  bool debugUsersRegister;
  bool debugUsersQuery;
  // CLI sub-flags
  bool debugCliExecution;
  bool debugCliQueue;
  bool debugCliValidation;
  // Performance sub-flags
  bool debugPerfStack;
  bool debugPerfHeap;
  bool debugPerfTiming;
  // SSE sub-flags
  bool debugSseConnection;
  bool debugSseEvents;
  bool debugSseBroadcast;
  // Command Flow sub-flags
  bool debugCmdflowRouting;
  bool debugCmdflowQueue;
  bool debugCmdflowContext;
  int logLevel;                        // Severity-based logging level (0=error, 1=warn, 2=info, 3=debug)
  int memorySampleIntervalSec;  // Periodic memory sampling interval in seconds (0=disabled, default: 30)
  // ESP-NOW settings
  bool espnowenabled;
  bool espnowmesh;
  bool espnowUserSyncEnabled;          // Enable user credential sync across devices (default: false, admin-only)
  // ESP-NOW device identity
  String espnowDeviceName;             // Device name for ESP-NOW topology (user-configurable)
  bool espnowFirstTimeSetup;           // True if ESP-NOW setup has been completed
  String espnowPassphrase;             // Encryption passphrase for secure pairing (persisted)
  // Mesh role settings
  uint8_t meshRole;                    // 0=worker, 1=master, 2=backup_master
  String meshMasterMAC;                // MAC of current master (empty if this is master)
  String meshBackupMAC;                // MAC of designated backup master
  uint32_t meshMasterHeartbeatInterval;  // Master heartbeat interval (ms, default: 10000)
  uint32_t meshFailoverTimeout;        // Backup promotes after this timeout (ms, default: 20000)
  uint32_t meshWorkerStatusInterval;   // Worker status report interval (ms, default: 30000)
  uint32_t meshTopoDiscoveryInterval;  // Topology discovery interval (ms, 0=on-demand, default: 0)
  bool meshTopoAutoRefresh;            // Auto-refresh topology (default: false)
  bool meshHeartbeatBroadcast;         // Broadcast heartbeats (true=public/discovery, false=private/paired-only)
  uint8_t meshTTL;                     // TTL for mesh-routed messages (default: 3, range: 1-10, updated by adaptive mode)
  bool meshAdaptiveTTL;                // Use adaptive TTL based on peer count: ceil(log2(peers))+1 (default: false)
  // Automation system
  bool automationsEnabled;  // Enable/disable automation scheduler (runs from main loop)
  // I2C Hardware system
  bool i2cBusEnabled;       // Enable/disable I2C bus hardware (Wire/Wire1 init and transactions)
  bool i2cSensorsEnabled;   // Enable/disable I2C sensor subsystem (runtime toggle like automation/espnow)
  int i2cSdaPin = I2C_SDA_PIN_DEFAULT;  // I2C SDA pin (board-specific, see System_BuildConfig.h)
  int i2cSclPin = I2C_SCL_PIN_DEFAULT;  // I2C SCL pin (board-specific, see System_BuildConfig.h)
  // Hardware settings (LED)
  int ledBrightness;        // 0-100%
  bool ledStartupEnabled;   // Enable startup effect
  String ledStartupEffect;  // Effect type: none, rainbow, pulse, fade, blink, strobe
  String ledStartupColor;   // Primary color name
  String ledStartupColor2;  // Secondary color (for fade)
  int ledStartupDuration;   // Duration in ms
  // OLED Display settings
  bool oledEnabled;             // Enable/disable OLED at boot
  bool oledAutoInit;            // Auto-initialize if detected
  bool localDisplayRequireAuth; // Require login before accessing display modes
  String oledBootMode;          // Initial mode during boot: logo, status, sensors, thermal, off
  String oledDefaultMode;       // Mode to switch to after boot completes
  int oledBootDuration;         // Milliseconds to show boot mode before switching to default
  int oledUpdateInterval;       // Update interval in milliseconds (5 Hz = 200ms)
  int oledBrightness;           // Display brightness/contrast 0-255
  float oledThermalScale;       // Scaling factor for thermal image (2.5 = 80x60)
  String oledThermalColorMode;  // Visualization style: 3level, 2level, gradient
  // Sensor Auto-Start settings (all I2C sensors)
  bool gamepadAutoStart;        // Auto-start gamepad after boot completes
  bool thermalAutoStart;        // Auto-start thermal camera after boot
  bool tofAutoStart;            // Auto-start ToF distance sensor after boot
  bool imuAutoStart;            // Auto-start IMU after boot
  bool gpsAutoStart;            // Auto-start GPS after boot
  bool fmRadioAutoStart;        // Auto-start FM radio after boot
  bool apdsAutoStart;           // Auto-start APDS gesture/color sensor after boot
  // HTTP server settings
  bool httpAutoStart;           // Auto-start HTTP server at boot if WiFi connected
  // Bluetooth settings
  bool bluetoothAutoStart;      // Auto-start Bluetooth at boot (enables BLE server)
  bool bluetoothRequireAuth;    // Require login before accepting BLE commands (always required, per-connection)
  String bleDeviceName;         // BLE advertised device name (default: "HardwareOne")
  int bleTxPower;               // BLE TX power level 0-7 (0=min, 7=max, default: 3)
  String bleGlassesLeftMAC;     // MAC address of left glasses lens (format: "AA:BB:CC:DD:EE:FF")
  String bleGlassesRightMAC;    // MAC address of right glasses lens
  String bleRingMAC;            // MAC address of smart ring
  String blePhoneMAC;           // MAC address of phone
  // Power Management settings
  uint8_t powerMode;            // 0=Performance(240MHz), 1=Balanced(160MHz), 2=PowerSaver(80MHz), 3=UltraSaver(40MHz)
  bool powerAutoMode;           // Auto-adjust power mode based on battery level
  uint8_t powerBatteryThreshold; // Switch to power saver below this battery % (default: 20)
  uint8_t powerDisplayDimLevel; // Brightness % in power saver modes (0-100, default: 30)
};

// Global settings instance (defined in .ino)
extern Settings gSettings;

// ============================================================================
// Settings Management Functions (implemented in settings.cpp)
// ============================================================================

// Settings initialization and defaults
void settingsDefaults();

// Settings JSON serialization/deserialization
void buildSettingsJsonDoc(JsonDocument& doc, bool excludePasswords = false);
bool readSettingsJson();
bool writeSettingsJson();

// Apply settings to runtime flags
void applySettings();

// WiFi password encryption/decryption helpers
String encryptWifiPassword(const String& password);
String decryptWifiPassword(const String& encryptedPassword);
String getDeviceEncryptionKey();

// Settings command registry
extern const CommandEntry settingsCommands[];
extern const size_t settingsCommandsCount;

// ============================================================================
// Settings Command Handlers (implemented in settings.cpp)
// ============================================================================

// WiFi Settings
const char* cmd_wifitxpower(const String& cmd);
const char* cmd_wifiautoreconnect(const String& cmd);
const char* cmd_tzoffsetminutes(const String& cmd);
const char* cmd_ntpserver(const String& cmd);

// Thermal Settings
const char* cmd_thermalpalettedefault(const String& cmd);
const char* cmd_thermalewmafactor(const String& cmd);
const char* cmd_thermaltransitionms(const String& cmd);
const char* cmd_thermalupscalefactor(const String& cmd);
const char* cmd_thermalrollingminmaxenabled(const String& cmd);
const char* cmd_thermalrollingminmaxalpha(const String& cmd);
const char* cmd_thermalrollingminmaxguardc(const String& cmd);
const char* cmd_thermaltemporalalpha(const String& cmd);
const char* cmd_thermalrotation(const String& cmd);

// ESP-NOW Settings
const char* cmd_espnowenabled(const String& cmd);

// OLED Settings
const char* cmd_oled_enabled(const String& cmd);
const char* cmd_oled_autoinit(const String& cmd);
const char* cmd_oled_bootmode(const String& cmd);
const char* cmd_oled_defaultmode(const String& cmd);
const char* cmd_oled_bootduration(const String& cmd);
const char* cmd_oled_updateinterval(const String& cmd);
const char* cmd_oled_brightness(const String& cmd);
const char* cmd_oled_thermalscale(const String& cmd);
const char* cmd_oled_thermalcolormode(const String& cmd);

// LED Settings
const char* cmd_hardwareled_brightness(const String& cmd);
const char* cmd_hardwareled_startupenabled(const String& cmd);
const char* cmd_hardwareled_startupeffect(const String& cmd);
const char* cmd_hardwareled_startupcolor(const String& cmd);
const char* cmd_hardwareled_startupcolor2(const String& cmd);
const char* cmd_hardwareled_startupduration(const String& cmd);

// CLI Settings
const char* cmd_clihistorysize(const String& cmd);

// ============================================================================
// Modular Settings Registry System
// ============================================================================

// Setting data types
enum SettingType {
  SETTING_INT,
  SETTING_FLOAT,
  SETTING_BOOL,
  SETTING_STRING
};

// Individual setting entry - describes one setting field
struct SettingEntry {
  const char* jsonKey;        // Key in JSON file (e.g., "thermalPollingMs")
  SettingType type;           // Data type
  void* valuePtr;             // Pointer to actual value in gSettings
  int intDefault;             // Default for int (also used for bool: 0/1)
  float floatDefault;         // Default for float
  const char* stringDefault;  // Default for string
  int minVal;                 // Min value for int/float validation (0 to skip)
  int maxVal;                 // Max value for int/float validation (0 to skip)
  const char* label;          // Human-readable label for UI display (nullptr = use jsonKey)
  const char* options;        // Comma-separated options for select fields (nullptr = none)
};

// Connection check callback - returns true if module is available/connected
typedef bool (*ConnectionCheckFunc)();

// Settings module - a group of related settings
struct SettingsModule {
  const char* name;             // Module name (e.g., "thermal", "wifi")
  const char* jsonSection;      // JSON section name (nullptr for root level)
  const SettingEntry* entries;  // Array of setting entries
  size_t count;                 // Number of entries
  ConnectionCheckFunc isConnected; // Optional: check if module is available (nullptr = always available)
  const char* description;      // Optional: human-readable description for UI
};

// Maximum number of settings modules that can be registered
#define MAX_SETTINGS_MODULES 16

// Register a settings module (call during setup or static init)
void registerSettingsModule(const SettingsModule* module);

// Get all registered modules
const SettingsModule** getSettingsModules(size_t& count);

// Apply defaults from all registered modules
void applyRegisteredDefaults();

// Read settings from JSON using registered modules
// Returns number of settings successfully read
size_t readRegisteredSettings(JsonDocument& doc);

// Write settings to JSON using registered modules
// Returns number of settings written
size_t writeRegisteredSettings(JsonDocument& doc);

// Register ALL settings modules explicitly (called once early in boot)
// Ensures all compiled modules are available before applying defaults
void registerAllSettingsModules();

// Legacy function - now just calls registerAllSettingsModules()
void registerCoreSettingsModules();

// Debug: print a summary of all registered settings modules
void printSettingsModuleSummary();

// Generic setting command handler - parses value and updates setting
// Returns result message
const char* handleSettingCommand(const SettingEntry* entry, const String& cmd);

#endif // SETTINGS_H
