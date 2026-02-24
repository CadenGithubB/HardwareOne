#include "System_Settings.h"        // Settings struct definition and function declarations
#include "System_BuildConfig.h"   // ENABLE_WIFI, ENABLE_ESPNOW flags
#if ENABLE_WIFI
  #include "System_WiFi.h"   // WifiNetwork struct, OUTPUT_* macros, MAX_WIFI_NETWORKS
#endif
#include "System_Debug.h"    // DEBUG_* macros
#if ENABLE_ESPNOW
  #include "System_ESPNow.h" // EspNowMode enum
#endif
#include "System_MemUtil.h"  // PSRAM_JSON_DOC macro
#include "System_SensorStubs.h" // Network stubs when disabled
#include "System_Utils.h"    // RETURN_VALID_IF_VALIDATE_CSTR macro
#include "System_Command.h"  // For CommandModuleRegistrar
#include "System_Notifications.h"
#include <LittleFS.h>
#include <esp_system.h>
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#if ENABLE_WIFI
  #include <WiFi.h>
  #include <WiFiUdp.h>
#endif

// ============================================================================
// Settings Implementation
// ============================================================================

// Note: Settings struct is defined in settings.h
// Note: WifiNetwork struct and gWifiNetworks are defined in wifi_system.h

// External dependencies from main .ino
extern Settings gSettings;

extern bool filesystemReady;
extern volatile uint32_t gOutputFlags;
// gDebugFlags now from debug_system.h

// WiFi network constants - defined as macros in wifi_system.h (MAX_WIFI_NETWORKS)
// Debug flag constants - defined as macros in debug_system.h (DEBUG_*)
// Output flag constants - defined as macros in wifi_system.h (OUTPUT_*)
// ESP-NOW mode constants - defined as enum in espnow_system.h (ESPNOW_MODE_*)

// File paths - need to be non-static in .ino for access from .cpp files
extern const char* SETTINGS_JSON_FILE;

// Deferred write flag — when true, setSetting() updates RAM only; savesettings writes once
volatile bool gDeferWrites = false;

// Filesystem locking
extern void fsLock(const char* owner);
extern void fsUnlock();

// CommandEntry struct is defined in system_utils.h (included at top of file)

// ============================================================================
// Settings Command Implementations (moved from .ino)
// ============================================================================

const char* cmd_webclihistorysize(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) return "Usage: webclihistorysize <1..100>";
  int v = valStr.toInt();
  if (v < 1) v = 1;
  if (v > 100) v = 100;
  setSetting(gSettings.webCliHistorySize, v);
  snprintf(getDebugBuffer(), 1024, "webCliHistorySize set to %d", v);
  return getDebugBuffer();
}

const char* cmd_oledclihistorysize(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) return "Usage: oledclihistorysize <10..100>";
  int v = valStr.toInt();
  if (v < 10) v = 10;  // Minimum 10 lines for OLED
  if (v > 100) v = 100;
  setSetting(gSettings.oledCliHistorySize, v);
  snprintf(getDebugBuffer(), 1024, "oledCliHistorySize set to %d (requires reboot)", v);
  return getDebugBuffer();
}

const char* cmd_outserial(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  String t1, t2;
  int sp2 = a.indexOf(' ');
  if (sp2 >= 0) {
    t1 = a.substring(0, sp2);
    t2 = a.substring(sp2 + 1);
    t2.trim();
  } else {
    t1 = a;
  }
  t1.trim();
  bool modeTemp = false;
  int v = -1;
  auto parse01 = [](const String& s) -> int {
    String ss = s;
    ss.trim();
    return ss.toInt();
  };
  if (t1.length() && (t1 == "temp" || t1 == "persist")) {
    modeTemp = (t1 == "temp");
    if (t2.length()) v = parse01(t2);
  } else {
    if (t1.length()) v = parse01(t1);
    if (t2.length()) { modeTemp = (t2 == "temp"); }
  }
  if (v != 0) v = 1;
  if (v < 0) return "Usage: outserial <0|1> [persist|temp]";
  if (modeTemp) {
    if (v) gOutputFlags |= OUTPUT_SERIAL;
    else gOutputFlags &= ~OUTPUT_SERIAL;
    return v ? "outSerial (runtime) set to 1" : "outSerial (runtime) set to 0";
  } else {
    setSetting(gSettings.outSerial, (bool)(v != 0));
    if (v) gOutputFlags |= OUTPUT_SERIAL;
    else gOutputFlags &= ~OUTPUT_SERIAL;
    return gSettings.outSerial ? "outSerial (persisted) set to 1" : "outSerial (persisted) set to 0";
  }
}

const char* cmd_outweb(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  String t1, t2;
  int sp2 = a.indexOf(' ');
  if (sp2 >= 0) {
    t1 = a.substring(0, sp2);
    t2 = a.substring(sp2 + 1);
    t2.trim();
  } else {
    t1 = a;
  }
  t1.trim();
  bool modeTemp = false;
  int v = -1;
  auto parse01 = [](const String& s) -> int {
    String ss = s;
    ss.trim();
    return ss.toInt();
  };
  if (t1.length() && (t1 == "temp" || t1 == "persist")) {
    modeTemp = (t1 == "temp");
    if (t2.length()) v = parse01(t2);
  } else {
    if (t1.length()) v = parse01(t1);
    if (t2.length()) { modeTemp = (t2 == "temp"); }
  }
  if (v != 0) v = 1;
  if (v < 0) return "Usage: outweb <0|1> [persist|temp]";
  if (modeTemp) {
    if (v) gOutputFlags |= OUTPUT_WEB;
    else gOutputFlags &= ~OUTPUT_WEB;
    return v ? "outWeb (runtime) set to 1" : "outWeb (runtime) set to 0";
  } else {
    setSetting(gSettings.outWeb, (bool)(v != 0));
    if (v) gOutputFlags |= OUTPUT_WEB;
    else gOutputFlags &= ~OUTPUT_WEB;
    return gSettings.outWeb ? "outWeb (persisted) set to 1" : "outWeb (persisted) set to 0";
  }
}

// ============================================================================
// Settings Command Registry
// ============================================================================

const char* cmd_beginwrite(const String& args);
const char* cmd_savesettings(const String& args);

const CommandEntry settingsCommands[] = {
#if ENABLE_WIFI
  // ---- WiFi Network Settings ----
  { "wifitxpower", "Set WiFi TX power: <dBm>", true, cmd_wifitxpower },
  { "wifiautoreconnect", "WiFi auto-reconnect: <0|1>", true, cmd_wifiautoreconnect },
  
  // ---- System Time Settings ----
  { "ntpserver", "Set NTP server: <hostname>", true, cmd_ntpserver, "Usage: ntpserver <host>" },
#endif
  { "tzoffsetminutes", "Set timezone offset: <-720..720>", true, cmd_tzoffsetminutes, "Usage: tzoffsetminutes <-720..720>" },
  
  // Note: Thermal and ToF sensor settings are now in their respective sensor files:
  // - thermal_sensor.cpp: thermalCommands[]
  // - tof_sensor.cpp: tofCommands[]
  
#if ENABLE_ESPNOW
  // ---- Device Settings ----
  { "espnowenabled", "Enable/disable ESP-NOW: <0|1> (reboot required)", true, cmd_espnowenabled, "Usage: espnowenabled <0|1>" },
#endif
  
  // Note: I2C settings are now handled by the modular registry in i2c_system.cpp
  
  // ---- CLI Settings ----
  { "webclihistorysize", "Set web CLI history size: <1..100>", true, cmd_webclihistorysize, "Usage: webclihistorysize <1..100>" },
  { "oledclihistorysize", "Set OLED CLI history size: <10..100>", true, cmd_oledclihistorysize, "Usage: oledclihistorysize <10..100>" },

  // ---- Batch write ----
  { "beginwrite",   "Start a batch settings update — defers flash write until savesettings.", true, cmd_beginwrite },
  { "savesettings", "Flush deferred settings to flash (single write).",                       true, cmd_savesettings },
};

const size_t settingsCommandsCount = sizeof(settingsCommands) / sizeof(settingsCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _settings_cmd_registrar(settingsCommands, settingsCommandsCount, "settings");

// ============================================================================
// WiFi Password Encryption Helpers
// ============================================================================

String getDeviceEncryptionKey() {
  // Cache the key so we only generate (and log) once per boot
  static bool sInit = false;
  static String sKey;
  if (sInit) {
    return sKey;
  }

  DEBUG_SYSTEMF("[Encryption] Generating device encryption key");

  // Create device-unique key from Chip ID only (deterministic)
  uint64_t chipId = ESP.getEfuseMac();

  // Build key using safer char buffer approach to avoid String concatenation issues
  char keyBuf[32];
  snprintf(keyBuf, sizeof(keyBuf), "%08lx%08lx",
           (unsigned long)((uint32_t)(chipId >> 32)),
           (unsigned long)((uint32_t)chipId));

  sKey = String(keyBuf);

  // Pad key to ensure sufficient length for XOR
  while (sKey.length() < 32) {
    sKey += sKey;  // Double the key until we have enough length
  }

  DEBUG_SYSTEMF("[Encryption] Key generated, length=%d", sKey.length());
  sInit = true;
  return sKey;
}

String encryptWifiPassword(const String& password) {
  if (password.length() == 0) return "";

  // Derive 16-byte AES key from device encryption key
  String keyMaterial = getDeviceEncryptionKey();
  uint8_t key[16];
  uint8_t hash[32];
  mbedtls_sha256((const uint8_t*)keyMaterial.c_str(), keyMaterial.length(), hash, 0);
  memcpy(key, hash, 16);
  secureClearString(keyMaterial);

  // Generate random IV
  uint8_t iv[16];
  esp_fill_random(iv, 16);

  // PKCS#7 padding
  int paddedLen = ((password.length() / 16) + 1) * 16;
  uint8_t* plaintext = (uint8_t*)malloc(paddedLen);
  if (!plaintext) {
    ERROR_MEMORYF("[AES] Failed to allocate plaintext buffer");
    return "";
  }
  
  memcpy(plaintext, password.c_str(), password.length());
  uint8_t padValue = paddedLen - password.length();
  for (int i = password.length(); i < paddedLen; i++) {
    plaintext[i] = padValue;
  }

  // Encrypt with AES-128-CBC
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key, 128);

  uint8_t* ciphertext = (uint8_t*)malloc(paddedLen);
  if (!ciphertext) {
    free(plaintext);
    mbedtls_aes_free(&aes);
    ERROR_MEMORYF("[AES] Failed to allocate ciphertext buffer");
    return "";
  }

  // Make a copy of IV since mbedtls_aes_crypt_cbc modifies it
  uint8_t iv_copy[16];
  memcpy(iv_copy, iv, 16);
  
  int ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, paddedLen, iv_copy, plaintext, ciphertext);
  mbedtls_aes_free(&aes);
  
  // Securely clear plaintext
  memset(plaintext, 0, paddedLen);
  free(plaintext);

  if (ret != 0) {
    free(ciphertext);
    ERROR_STORAGEF("[AES] Encryption failed: %d", ret);
    return "";
  }

  // Encode: AES:<hex-iv>:<hex-ciphertext>
  int resultLen = 4 + 32 + 1 + (paddedLen * 2) + 1;  // "AES:" + IV + ":" + ciphertext + null
  char* result = (char*)malloc(resultLen);
  if (!result) {
    free(ciphertext);
    ERROR_MEMORYF("[AES] Failed to allocate result buffer");
    return "";
  }

  int pos = sprintf(result, "AES:");
  for (int i = 0; i < 16; i++) {
    pos += sprintf(result + pos, "%02X", iv[i]);
  }
  pos += sprintf(result + pos, ":");
  for (int i = 0; i < paddedLen; i++) {
    pos += sprintf(result + pos, "%02X", ciphertext[i]);
  }

  String output = String(result);
  free(result);
  free(ciphertext);

  DEBUG_STORAGEF("[AES] WiFi password encrypted (len=%d)", output.length());
  return output;
}

String decryptWifiPassword(const String& encryptedPassword) {
  if (encryptedPassword.length() == 0) {
    return "";
  }

  if (!encryptedPassword.startsWith("AES:")) {
    ERROR_STORAGEF("[AES] Invalid WiFi password format detected");
    return "";
  }

  DEBUG_STORAGEF("[AES] Decrypting WiFi password (len=%d)", encryptedPassword.length());

  // Parse: AES:<32-hex-iv>:<hex-ciphertext>
  if (encryptedPassword.length() < 41) {  // "AES:" + 32 hex chars + ":" minimum
    ERROR_STORAGEF("[AES] Encrypted password too short");
    return "";
  }

  String ivHex = encryptedPassword.substring(4, 36);  // 32 hex chars = 16 bytes
  if (encryptedPassword[36] != ':') {
    ERROR_STORAGEF("[AES] Missing separator after IV");
    return "";
  }
  String ciphertextHex = encryptedPassword.substring(37);

  // Derive AES key
  String keyMaterial = getDeviceEncryptionKey();
  uint8_t key[16];
  uint8_t hash[32];
  mbedtls_sha256((const uint8_t*)keyMaterial.c_str(), keyMaterial.length(), hash, 0);
  memcpy(key, hash, 16);
  secureClearString(keyMaterial);

  // Decode IV
  uint8_t iv[16];
  for (int i = 0; i < 16; i++) {
    char hexByte[3] = { ivHex[i*2], ivHex[i*2+1], '\0' };
    iv[i] = strtol(hexByte, NULL, 16);
  }

  // Decode ciphertext
  int ciphertextLen = ciphertextHex.length() / 2;
  uint8_t* ciphertext = (uint8_t*)malloc(ciphertextLen);
  if (!ciphertext) {
    ERROR_MEMORYF("[AES] Failed to allocate ciphertext buffer");
    return "";
  }

  for (int i = 0; i < ciphertextLen; i++) {
    char hexByte[3] = { ciphertextHex[i*2], ciphertextHex[i*2+1], '\0' };
    ciphertext[i] = strtol(hexByte, NULL, 16);
  }

  // Decrypt with AES-128-CBC
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, key, 128);

  uint8_t* plaintext = (uint8_t*)malloc(ciphertextLen);
  if (!plaintext) {
    free(ciphertext);
    mbedtls_aes_free(&aes);
    ERROR_MEMORYF("[AES] Failed to allocate plaintext buffer");
    return "";
  }

  // Make a copy of IV since mbedtls_aes_crypt_cbc modifies it
  uint8_t iv_copy[16];
  memcpy(iv_copy, iv, 16);

  int ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, ciphertextLen, iv_copy, ciphertext, plaintext);
  mbedtls_aes_free(&aes);
  free(ciphertext);

  if (ret != 0) {
    free(plaintext);
    ERROR_STORAGEF("[AES] Decryption failed: %d", ret);
    return "";
  }

  // Remove PKCS#7 padding
  uint8_t padValue = plaintext[ciphertextLen - 1];
  if (padValue < 1 || padValue > 16) {
    free(plaintext);
    ERROR_STORAGEF("[AES] Invalid padding value: %d", padValue);
    return "";
  }

  int plaintextLen = ciphertextLen - padValue;
  char* result = (char*)malloc(plaintextLen + 1);
  if (!result) {
    free(plaintext);
    ERROR_MEMORYF("[AES] Failed to allocate result buffer");
    return "";
  }

  memcpy(result, plaintext, plaintextLen);
  result[plaintextLen] = '\0';

  // Securely clear plaintext
  memset(plaintext, 0, ciphertextLen);
  free(plaintext);

  String output = String(result);
  free(result);

  DEBUG_STORAGEF("[AES] WiFi password decrypted successfully (len=%d)", output.length());
  return output;
}

// ============================================================================
// Settings Defaults
// ============================================================================

void settingsDefaults() {
  DEBUG_STORAGEF("[Settings] Initializing default settings");

  // Register ALL settings modules BEFORE applying defaults
  // This ensures all compiled modules are registered even on fresh boot
  registerAllSettingsModules();

  // ============================================================================
  // Apply defaults from all registered settings modules
  // Each subsystem owns its own defaults in its respective file:
  // - cli (System_Command.cpp): historySize
  // - wifi (System_WiFi.cpp): ssid, password, autoReconnect, ntpServer, tzOffset
  // - http (System_WiFi.cpp): autoStart
  // - espnow (System_ESPNow.cpp): enabled, mesh, userSync, device, mesh role/timing
  // - automation (System_Automation.cpp): enabled
  // - debug (System_Settings.cpp): all debug flags
  // - output (System_Settings.cpp): outSerial, outWeb, outDisplay
  // - i2c (System_I2C.cpp): bus settings, clock speeds
  // - thermal (i2csensor-mlx90640.cpp): autoStart, polling, interpolation, EWMA, rotation
  // - tof (i2csensor-vl53l4cx.cpp): autoStart, polling, stability, transition
  // - imu (i2csensor-bno055.cpp): autoStart, polling, EWMA, orientation correction
  // - gps (i2csensor-pa1010d.cpp): autoStart, polling
  // - apds (i2csensor-apds9960.cpp): autoStart, polling
  // - gamepad (i2csensor-seesaw.cpp): autoStart, polling
  // - fmradio (i2csensor-rda5807.cpp): autoStart, polling
  // - oled (OLED_Settings.cpp): enabled, autoInit, modes, brightness
  // - led (System_NeoPixel.cpp): brightness, startup effect/color/duration
  // - power (System_Power.cpp): mode, autoMode, thresholds
  // - bluetooth (Optional_Bluetooth.cpp): autoStart, requireAuth, deviceName
  // ============================================================================
  applyRegisteredDefaults();
}

// ============================================================================
// Apply Settings to Runtime Flags
// ============================================================================

void applySettings() {
  DEBUG_SYSTEMF("[applySettings] START");

  // Apply persisted output lanes
  uint8_t flags = 0;
  if (gSettings.outSerial) flags |= OUTPUT_SERIAL;
  if (gSettings.outDisplay) flags |= OUTPUT_DISPLAY;
  if (gSettings.outWeb) flags |= OUTPUT_WEB;
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  if (gSettings.outG2) flags |= OUTPUT_G2;
#endif
  gOutputFlags = flags;  // replace current routing with persisted lanes

  // Apply debug settings to runtime flags using accessor functions
  setDebugFlags(0);  // Start with no flags, then enable based on settings
  if (gSettings.debugAuth) setDebugFlag(DEBUG_AUTH);
  if (gSettings.debugAuthCookies) setDebugFlag(DEBUG_AUTH);
  if (gSettings.debugHttp) setDebugFlag(DEBUG_HTTP);
  if (gSettings.debugSse) setDebugFlag(DEBUG_SSE);
  if (gSettings.debugCli) setDebugFlag(DEBUG_CLI);
  if (gSettings.debugSensors) setDebugFlag(DEBUG_SENSORS);
  if (gSettings.debugSensorsGeneral) setDebugFlag(DEBUG_SENSORS);
  if (gSettings.debugCamera) setDebugFlag(DEBUG_CAMERA);
  if (gSettings.debugMicrophone) setDebugFlag(DEBUG_MICROPHONE);
  if (gSettings.debugWifi) setDebugFlag(DEBUG_WIFI);
  if (gSettings.debugStorage) setDebugFlag(DEBUG_STORAGE);
  if (gSettings.debugPerformance) setDebugFlag(DEBUG_PERFORMANCE);
  if (gSettings.debugDateTime) setDebugFlag(DEBUG_SYSTEM);
  if (gSettings.debugCommandFlow) setDebugFlag(DEBUG_CMD_FLOW);
  if (gSettings.debugUsers) setDebugFlag(DEBUG_USERS);
  if (gSettings.debugSystem) setDebugFlag(DEBUG_SYSTEM);
  if (gSettings.debugAutomations) setDebugFlag(DEBUG_AUTOMATIONS);
  if (gSettings.debugLogger) setDebugFlag(DEBUG_LOGGER);
  if (gSettings.debugMemory) setDebugFlag(DEBUG_MEMORY);
  if (gSettings.debugCommandSystem) setDebugFlag(DEBUG_COMMAND_SYSTEM);
  if (gSettings.debugSettingsSystem) setDebugFlag(DEBUG_SETTINGS_SYSTEM);
  if (gSettings.debugEspNow) setDebugFlag(DEBUG_ESPNOW_CORE);
  if (gSettings.debugEspNowStream) setDebugFlag(DEBUG_ESPNOW_STREAM);
  if (gSettings.debugEspNowCore) setDebugFlag(DEBUG_ESPNOW_CORE);
  if (gSettings.debugEspNowRouter) setDebugFlag(DEBUG_ESPNOW_ROUTER);
  if (gSettings.debugEspNowMesh) setDebugFlag(DEBUG_ESPNOW_MESH);
  if (gSettings.debugEspNowTopo) setDebugFlag(DEBUG_ESPNOW_TOPO);
  if (gSettings.debugEspNowEncryption) setDebugFlag(DEBUG_ESPNOW_ENCRYPTION);
  if (gSettings.debugAutoScheduler) setDebugFlag(DEBUG_AUTO_SCHEDULER);
  if (gSettings.debugAutoExec) setDebugFlag(DEBUG_AUTO_EXEC);
  if (gSettings.debugAutoCondition) setDebugFlag(DEBUG_AUTO_CONDITION);
  if (gSettings.debugAutoTiming) setDebugFlag(DEBUG_AUTO_TIMING);
  if (gSettings.debugFmRadio) setDebugFlag(DEBUG_FMRADIO);
  if (gSettings.debugG2) setDebugFlag(DEBUG_G2);
  if (gSettings.debugI2C) setDebugFlag(DEBUG_I2C);

  // Apply per-sensor frame/data debug flags
  if (gSettings.debugThermalFrame) setDebugFlag(DEBUG_THERMAL_FRAME);
  if (gSettings.debugThermalData) setDebugFlag(DEBUG_THERMAL_DATA);
  if (gSettings.debugTofFrame) setDebugFlag(DEBUG_TOF_FRAME);
  if (gSettings.debugGamepadFrame) setDebugFlag(DEBUG_GAMEPAD_FRAME);
  if (gSettings.debugGamepadData) setDebugFlag(DEBUG_GAMEPAD_DATA);
  if (gSettings.debugImuFrame) setDebugFlag(DEBUG_IMU_FRAME);
  if (gSettings.debugImuData) setDebugFlag(DEBUG_IMU_DATA);
  if (gSettings.debugApdsFrame) setDebugFlag(DEBUG_APDS_FRAME);

  // Apply debug sub-flags to gDebugSubFlags and update parent flags
  // Auth sub-flags
  gDebugSubFlags.authSessions = gSettings.debugAuthSessions;
  gDebugSubFlags.authCookies = gSettings.debugAuthCookies;
  gDebugSubFlags.authLogin = gSettings.debugAuthLogin;
  gDebugSubFlags.authBootId = gSettings.debugAuthBootId;
  updateParentDebugFlag(DEBUG_AUTH, gSettings.debugAuth || gDebugSubFlags.authSessions || gDebugSubFlags.authCookies || gDebugSubFlags.authLogin || gDebugSubFlags.authBootId);
  
  // HTTP sub-flags
  gDebugSubFlags.httpHandlers = gSettings.debugHttpHandlers;
  gDebugSubFlags.httpRequests = gSettings.debugHttpRequests;
  gDebugSubFlags.httpResponses = gSettings.debugHttpResponses;
  gDebugSubFlags.httpStreaming = gSettings.debugHttpStreaming;
  updateParentDebugFlag(DEBUG_HTTP, gSettings.debugHttp || gDebugSubFlags.httpHandlers || gDebugSubFlags.httpRequests || gDebugSubFlags.httpResponses || gDebugSubFlags.httpStreaming);
  
  // WiFi sub-flags
  gDebugSubFlags.wifiConnection = gSettings.debugWifiConnection;
  gDebugSubFlags.wifiConfig = gSettings.debugWifiConfig;
  gDebugSubFlags.wifiScanning = gSettings.debugWifiScanning;
  gDebugSubFlags.wifiDriver = gSettings.debugWifiDriver;
  updateParentDebugFlag(DEBUG_WIFI, gSettings.debugWifi || gDebugSubFlags.wifiConnection || gDebugSubFlags.wifiConfig || gDebugSubFlags.wifiScanning || gDebugSubFlags.wifiDriver);
  
  // Storage sub-flags
  gDebugSubFlags.storageFiles = gSettings.debugStorageFiles;
  gDebugSubFlags.storageJson = gSettings.debugStorageJson;
  gDebugSubFlags.storageSettings = gSettings.debugStorageSettings;
  gDebugSubFlags.storageMigration = gSettings.debugStorageMigration;
  updateParentDebugFlag(DEBUG_STORAGE, gSettings.debugStorage || gDebugSubFlags.storageFiles || gDebugSubFlags.storageJson || gDebugSubFlags.storageSettings || gDebugSubFlags.storageMigration);
  
  // System sub-flags
  gDebugSubFlags.systemBoot = gSettings.debugSystemBoot;
  gDebugSubFlags.systemConfig = gSettings.debugSystemConfig;
  gDebugSubFlags.systemTasks = gSettings.debugSystemTasks;
  gDebugSubFlags.systemHardware = gSettings.debugSystemHardware;
  updateParentDebugFlag(DEBUG_SYSTEM, gSettings.debugSystem || gDebugSubFlags.systemBoot || gDebugSubFlags.systemConfig || gDebugSubFlags.systemTasks || gDebugSubFlags.systemHardware);
  
  // Users sub-flags
  gDebugSubFlags.usersMgmt = gSettings.debugUsersMgmt;
  gDebugSubFlags.usersRegister = gSettings.debugUsersRegister;
  gDebugSubFlags.usersQuery = gSettings.debugUsersQuery;
  updateParentDebugFlag(DEBUG_USERS, gSettings.debugUsers || gDebugSubFlags.usersMgmt || gDebugSubFlags.usersRegister || gDebugSubFlags.usersQuery);
  
  // CLI sub-flags
  gDebugSubFlags.cliExecution = gSettings.debugCliExecution;
  gDebugSubFlags.cliQueue = gSettings.debugCliQueue;
  gDebugSubFlags.cliValidation = gSettings.debugCliValidation;
  updateParentDebugFlag(DEBUG_CLI, gSettings.debugCli || gDebugSubFlags.cliExecution || gDebugSubFlags.cliQueue || gDebugSubFlags.cliValidation);
  
  // Performance sub-flags
  gDebugSubFlags.perfStack = gSettings.debugPerfStack;
  gDebugSubFlags.perfHeap = gSettings.debugPerfHeap;
  gDebugSubFlags.perfTiming = gSettings.debugPerfTiming;
  updateParentDebugFlag(DEBUG_PERFORMANCE, gSettings.debugPerformance || gDebugSubFlags.perfStack || gDebugSubFlags.perfHeap || gDebugSubFlags.perfTiming);
  
  // SSE sub-flags
  gDebugSubFlags.sseConnection = gSettings.debugSseConnection;
  gDebugSubFlags.sseEvents = gSettings.debugSseEvents;
  gDebugSubFlags.sseBroadcast = gSettings.debugSseBroadcast;
  updateParentDebugFlag(DEBUG_SSE, gSettings.debugSse || gDebugSubFlags.sseConnection || gDebugSubFlags.sseEvents || gDebugSubFlags.sseBroadcast);
  
  // Command Flow sub-flags
  gDebugSubFlags.cmdflowRouting = gSettings.debugCmdflowRouting;
  gDebugSubFlags.cmdflowQueue = gSettings.debugCmdflowQueue;
  gDebugSubFlags.cmdflowContext = gSettings.debugCmdflowContext;
  updateParentDebugFlag(DEBUG_CMD_FLOW, gSettings.debugCommandFlow || gDebugSubFlags.cmdflowRouting || gDebugSubFlags.cmdflowQueue || gDebugSubFlags.cmdflowContext);

  // Apply severity-based log level from settings
  {
    int lvl = gSettings.logLevel;
    if (lvl < LOG_LEVEL_ERROR) lvl = LOG_LEVEL_ERROR;
    if (lvl > LOG_LEVEL_DEBUG) lvl = LOG_LEVEL_DEBUG;
    DEBUG_MANAGER.setLogLevel((uint8_t)lvl);
    gSettings.logLevel = lvl;
  }

  DEBUG_SYSTEMF("[applySettings] Applied debug flags");

  // Apply ESP-NOW mode from settings (directly to gEspNow if initialized)
#if ENABLE_ESPNOW
  if (gEspNow) {
    gEspNow->mode = gSettings.espnowmesh ? ESPNOW_MODE_MESH : ESPNOW_MODE_DIRECT;
  }
#endif

  // Apply power mode from settings
  #include "System_Power.h"
  applyPowerMode(gSettings.powerMode);

  DEBUG_SYSTEMF("Settings applied (I2C hardware config skipped - requires sensor restart to apply)");
}

// ============================================================================
// Build Settings JSON Document
// ============================================================================

void buildSettingsJsonDoc(JsonDocument& doc, bool excludePasswords) {
  // Top-level settings
  doc["ntpServer"] = gSettings.ntpServer;
  doc["tzOffsetMinutes"] = gSettings.tzOffsetMinutes;
  doc["wifiEnabled"] = gSettings.wifiEnabled;
  doc["wifiAutoReconnect"] = gSettings.wifiAutoReconnect;
  doc["webCliHistorySize"] = gSettings.webCliHistorySize;
  doc["oledCliHistorySize"] = gSettings.oledCliHistorySize;
  
  // ESP-NOW settings (nested structure) - now handled by modular registry
  // Note: passphrase encryption is now automatic via isSecret flag
  
  // WiFi SSID fields for web UI
  // wifiPrimarySSID = currently connected network (preferred);
  // wifiSSID = primary saved SSID from settings (fallback/display)
  {
#if ENABLE_WIFI
    String cur = WiFi.SSID();
    if (cur.length() > 0) {
      doc["wifiPrimarySSID"] = cur;
    }
#endif
    doc["wifiSSID"] = gSettings.wifiSSID;
  }
  
#if ENABLE_AUTOMATION
  doc["automationsEnabled"] = gSettings.automationsEnabled;
#endif

  // Power management settings
  JsonObject powerObj = doc["power"].to<JsonObject>();
  powerObj["mode"] = gSettings.powerMode;
  powerObj["autoMode"] = gSettings.powerAutoMode;
  powerObj["batteryThreshold"] = gSettings.powerBatteryThreshold;
  powerObj["displayDimLevel"] = gSettings.powerDisplayDimLevel;

  // Debug flags are now handled by modular registry - no manual fallbacks needed
  // Output settings are now handled by modular registry - no manual fallbacks needed

  // Thermal settings now handled by modular registry
  // Write registered module settings here so section lands at the root,
  // before ToF/hardware/oled/wifiNetworks blocks
  {
    size_t registeredCount = writeRegisteredSettings(doc);
    if (registeredCount > 0) {
      DEBUG_STORAGEF("[Settings] Wrote %zu settings from registered modules", registeredCount);
    }
    
    // If building for web API, remove secret fields from all modules
    if (excludePasswords) {
      size_t modCount = 0;
      const SettingsModule** mods = getSettingsModules(modCount);
      for (size_t m = 0; m < modCount; m++) {
        const SettingsModule* mod = mods[m];
        if (!mod) continue;
        
        JsonObject section = mod->jsonSection ? doc[mod->jsonSection].as<JsonObject>() : doc.as<JsonObject>();
        if (section.isNull()) continue;
        
        for (size_t i = 0; i < mod->count; i++) {
          const SettingEntry* e = &mod->entries[i];
          if (e->isSecret && e->type == SETTING_STRING) {
            // Remove secret field from web API response
            const char* key = e->jsonKey;
            JsonObject target = section;
            const char* p = key;
            const char* dot = strchr(p, '.');
            while (dot) {
              String seg = String(p).substring(0, dot - p);
              target = target[seg.c_str()].as<JsonObject>();
              if (target.isNull()) break;
              p = dot + 1;
              dot = strchr(p, '.');
            }
            if (!target.isNull()) {
              target.remove(p);
            }
          }
        }
      }
    }
  }

  // WiFi networks array
  if (gWifiNetworks && gWifiNetworkCount > 0) {
    JsonArray networks = doc["wifiNetworks"].to<JsonArray>();
    for (int i = 0; i < gWifiNetworkCount; i++) {
      JsonObject net = networks.add<JsonObject>();
      net["ssid"] = gWifiNetworks[i].ssid;
      
      // Security: Encrypt passwords in file, exclude from web API
      if (!excludePasswords) {
        // Encrypt password for filesystem storage (protects against file access)
        String encryptedPassword = encryptWifiPassword(gWifiNetworks[i].password);
        net["password"] = encryptedPassword;
      }
      // For web API (excludePasswords=true), password field is omitted entirely
      
      net["priority"] = gWifiNetworks[i].priority;
      net["hidden"] = gWifiNetworks[i].hidden;
      net["lastConnected"] = gWifiNetworks[i].lastConnected;
    }
  }
}

// ============================================================================
// Write Settings to JSON File
// ============================================================================

bool writeSettingsJson() {
  if (!filesystemReady) return false;

  // Pause sensor polling during settings I/O
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;

  DEBUG_STORAGEF("[Settings] Writing to file using ArduinoJson");

  // Build JSON document (5120 bytes for settings + encrypted WiFi passwords)
  PSRAM_JSON_DOC(doc);
  
  // CRITICAL: Read existing settings first to preserve orphaned sensor sections
  // This allows settings from disabled sensors to persist and show as grayed-out in UI
  if (LittleFS.exists(SETTINGS_JSON_FILE)) {
    fsLock("settings.read_for_merge");
    File existingFile = LittleFS.open(SETTINGS_JSON_FILE, "r");
    if (existingFile) {
      DeserializationError err = deserializeJson(doc, existingFile);
      existingFile.close();
      if (err) {
        WARN_STORAGEF("Failed to read existing settings for merge: %s", err.c_str());
        doc.clear();  // Start fresh if parse failed
      } else {
        INFO_STORAGEF("Loaded existing settings for merge (preserving orphaned sections)");
      }
    }
    fsUnlock();
  }
  
  // Now build/overwrite with current settings (orphaned sections remain untouched)
  buildSettingsJsonDoc(doc);

  // Check for overflow
  if (doc.overflowed()) {
    ERROR_STORAGEF("JSON document overflowed during build (need more than 5120 bytes)");
    gSensorPollingPaused = wasPaused;
    return false;
  }

  // Atomic write: temp file then rename
  const char* tmp = "/settings.tmp";
  
  fsLock("settings.write");
  File file = LittleFS.open(tmp, "w");
  if (!file) {
    fsUnlock();
    ERROR_STORAGEF("Failed to open temp file for writing");
    gSensorPollingPaused = wasPaused;
    return false;
  }

  // Serialize JSON directly to file (no intermediate buffer)
  size_t bytesWritten = serializeJson(doc, file);
  file.close();
  fsUnlock();

  if (bytesWritten == 0) {
    ERROR_STORAGEF("Failed to serialize JSON");
    gSensorPollingPaused = wasPaused;
    return false;
  }

  DEBUG_STORAGEF("[Settings] Wrote %zu bytes to temp file", bytesWritten);

  // Atomic rename
  fsLock("settings.rename");
  LittleFS.remove(SETTINGS_JSON_FILE);
  bool okRename = LittleFS.rename(tmp, SETTINGS_JSON_FILE);
  fsUnlock();

  if (!okRename) {
    WARN_STORAGEF("Rename failed, trying direct write");
    // Fallback: write directly
    fsLock("settings.direct");
    File directFile = LittleFS.open(SETTINGS_JSON_FILE, "w");
    if (!directFile) {
      fsUnlock();
      gSensorPollingPaused = wasPaused;
      return false;
    }
    serializeJson(doc, directFile);
    directFile.close();
    fsUnlock();
  }

  DEBUG_STORAGEF("[Settings] Write complete");
  gSensorPollingPaused = wasPaused;
  
  // Push settings update to paired peer if in paired mode
#if ENABLE_ESPNOW
  extern Settings gSettings;
  if (gSettings.bondModeEnabled && gSettings.bondPeerMac.length() >= 12) {
    // Convert MAC string to bytes
    uint8_t peerMac[6];
    String macHex = gSettings.bondPeerMac;
    macHex.replace(":", "");
    for (int i = 0; i < 6; i++) {
      char byteStr[3] = {macHex[i*2], macHex[i*2+1], '\0'};
      peerMac[i] = (uint8_t)strtol(byteStr, nullptr, 16);
    }
    
    // Send settings push notification (async, non-blocking)
    extern void sendPairedSettings(const uint8_t* peerMac, uint32_t msgId, bool isPush);
    sendPairedSettings(peerMac, 0, true);  // isPush=true for push notification
  }
#endif
  
  return true;
}

// ============================================================================
// Read Settings from JSON File
// ============================================================================

bool readSettingsJson() {
  DEBUG_STORAGEF("[Settings] Loading from file using ArduinoJson");

  if (!filesystemReady) {
    DEBUG_STORAGEF("[Settings] Filesystem not ready");
    return false;
  }

  // Pause sensor polling during settings I/O
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;

  if (!LittleFS.exists(SETTINGS_JSON_FILE)) {
    DEBUG_STORAGEF("[Settings] File does not exist: %s", SETTINGS_JSON_FILE);
    gSensorPollingPaused = wasPaused;
    return false;
  }

  // Open file and parse JSON directly (no intermediate String)
  File file = LittleFS.open(SETTINGS_JSON_FILE, "r");
  if (!file) {
    ERROR_STORAGEF("Failed to open settings file");
    gSensorPollingPaused = wasPaused;
    return false;
  }

  // Use JsonDocument for parsing settings JSON
  // Size calculated: ~2.5KB base settings + ~2KB for WiFi networks with encrypted passwords
  // Encrypted passwords are ~2x longer than plaintext (hex encoding + "ENC:" prefix)
  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    ERROR_STORAGEF("JSON parse error: %s", error.c_str());
    if (error == DeserializationError::NoMemory) {
      ERROR_STORAGEF("JSON document too small (need more than 5120 bytes)");
    }
    gSensorPollingPaused = wasPaused;
    return false;
  }

  // Check for overflow
  if (doc.overflowed()) {
    DEBUG_STORAGEF("[Settings] WARNING: JSON document overflowed during parsing");
  }

  DEBUG_STORAGEF("[Settings] JSON parsed successfully, applying settings");

  registerAllSettingsModules();

  // Apply settings from registered modules first (handles defaults automatically)
  size_t registeredCount = readRegisteredSettings(doc);
  if (registeredCount > 0) {
    DEBUG_STORAGEF("[Settings] Applied %zu settings from registered modules", registeredCount);
  }

  // If I2C bus is disabled, the I2C sensor subsystem must also be disabled
  if (!gSettings.i2cBusEnabled) {
    gSettings.i2cSensorsEnabled = false;
  }

  // Top-level settings with defaults (| operator provides fallback)
  gSettings.wifiEnabled = doc["wifiEnabled"] | true;
  gSettings.wifiAutoReconnect = doc["wifiAutoReconnect"] | true;
  gSettings.webCliHistorySize = doc["webCliHistorySize"] | 10;
  gSettings.oledCliHistorySize = doc["oledCliHistorySize"] | 50;
  gSettings.ntpServer = doc["ntpServer"] | "pool.ntp.org";
  gSettings.tzOffsetMinutes = doc["tzOffsetMinutes"] | 0;

  // Debug and output settings are now handled by modular registry - no manual fallbacks needed

  // ESP-NOW settings - now handled by modular registry
  // Note: passphrase decryption is now automatic via isSecret flag
  
#if ENABLE_AUTOMATION
  // Automation settings
  gSettings.automationsEnabled = doc["automationsEnabled"] | false;
#endif

  // Power management settings
  JsonObject power = doc["power"];
  if (power) {
    gSettings.powerMode = power["mode"] | 0;
    gSettings.powerAutoMode = power["autoMode"] | false;
    gSettings.powerBatteryThreshold = power["batteryThreshold"] | 20;
    gSettings.powerDisplayDimLevel = power["displayDimLevel"] | 30;
  }

  // Output settings
  JsonObject output = doc["output"];
  if (output) {
    gSettings.outSerial = output["serial"] | true;
    gSettings.outWeb = output["web"] | true;
    gSettings.outDisplay = output["display"] | false;
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
    gSettings.outG2 = output["g2"] | false;
#endif
  }

  // Thermal settings are loaded by the modular registry below (no backward compatibility)

  // OLED settings are now handled by the modular registry with backward compatibility
  // Registry checks for "oled_ssd1306" first, then falls back to "oled"

  // Note: readRegisteredSettings() already called above after module registration
  // to avoid duplicate processing of registered settings

  // WiFi networks array
#if ENABLE_WIFI
  JsonArray networks = doc["wifiNetworks"];
  if (networks && gWifiNetworks) {
    gWifiNetworkCount = 0;
    for (JsonObject net : networks) {
      if (gWifiNetworkCount >= MAX_WIFI_NETWORKS) {
        DEBUG_STORAGEF("[WiFi Networks] Max networks reached (%d), skipping rest", MAX_WIFI_NETWORKS);
        break;
      }
      
      const char* ssid = net["ssid"];
      const char* password = net["password"];
      int priority = net["priority"] | 99;
      bool hidden = net["hidden"] | false;
      uint32_t lastConnected = net["lastConnected"] | 0;
      
      if (ssid && password) {
        gWifiNetworks[gWifiNetworkCount].ssid = ssid;
        gWifiNetworks[gWifiNetworkCount].password = decryptWifiPassword(password);
        gWifiNetworks[gWifiNetworkCount].priority = priority;
        gWifiNetworks[gWifiNetworkCount].hidden = hidden;
        gWifiNetworks[gWifiNetworkCount].lastConnected = lastConnected;
        gWifiNetworkCount++;
      }
    }
    DEBUG_STORAGEF("[WiFi Networks] Loaded %d networks from JSON", gWifiNetworkCount);
  } else if (!networks) {
    WARN_STORAGEF("No wifiNetworks array found in JSON");
  } else if (!gWifiNetworks) {
    ERROR_STORAGEF("gWifiNetworks not allocated");
  }
#endif

  DEBUG_STORAGEF("[Settings] Load complete");
  gSensorPollingPaused = wasPaused;
  return true;
}

// ============================================================================
// Settings command implementations (migrated from .ino)
// ============================================================================

extern void setupNTP();

const char* cmd_tzoffsetminutes(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: tzoffsetminutes <-720..720>";
  while (*p == ' ') p++;  // Skip whitespace
  int offset = atoi(p);
  if (offset < -720 || offset > 720) return "Error: timezone offset must be between -720 and 720 minutes";
  setSetting(gSettings.tzOffsetMinutes, offset);
#if ENABLE_WIFI
  setupNTP();
#endif
  snprintf(getDebugBuffer(), 1024, "Timezone offset set to %d minutes", offset);
  return getDebugBuffer();
}

#if ENABLE_WIFI
const char* cmd_ntpserver(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: ntpserver <host>";
  while (*p == ' ') p++;  // Skip whitespace
  if (*p == '\0') return "Error: NTP server cannot be empty";

  // Resolve host first
  IPAddress ntpIP;
  if (!WiFi.hostByName(p, ntpIP)) {
    snprintf(getDebugBuffer(), 1024, "Error: Cannot resolve NTP server hostname '%s'", p);
    return getDebugBuffer();
  }

  // Send a minimal NTP request to verify reachability
  WiFiUDP udp;
  byte ntpPacket[48];
  memset(ntpPacket, 0, sizeof(ntpPacket));
  ntpPacket[0] = 0b11100011;
  ntpPacket[2] = 6;
  ntpPacket[3] = 0xEC;
  udp.begin(8888);
  if (!udp.beginPacket(ntpIP, 123)) {
    udp.stop();
    snprintf(getDebugBuffer(), 1024, "Error: Cannot connect to NTP server '%s'", p);
    return getDebugBuffer();
  }
  udp.write(ntpPacket, sizeof(ntpPacket));
  if (!udp.endPacket()) {
    udp.stop();
    snprintf(getDebugBuffer(), 1024, "Error: Failed to send NTP request to '%s'", p);
    return getDebugBuffer();
  }
  unsigned long startTime = millis();
  int packetSize = 0;
  while (millis() - startTime < 5000) {
    packetSize = udp.parsePacket();
    if (packetSize >= 48) break;
    delay(10);
  }
  udp.stop();
  if (packetSize < 48) {
    snprintf(getDebugBuffer(), 1024, "Error: No response from NTP server '%s'. Server may be down or not an NTP server.", p);
    return getDebugBuffer();
  }

  setSetting(gSettings.ntpServer, p);
  setupNTP();
  snprintf(getDebugBuffer(), 1024, "NTP server set to %s (connectivity verified)", p);
  return getDebugBuffer();
}
#else
const char* cmd_ntpserver(const String& cmd) {
  return "NTP server command requires WiFi to be enabled";
}
#endif // ENABLE_WIFI

const char* cmd_espnowenabled(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: espnowenabled <0|1>";
  while (*p == ' ') p++;  // Skip whitespace
  bool enabled = (*p == '1' || strncasecmp(p, "true", 4) == 0);
  setSetting(gSettings.espnowenabled, enabled);
  snprintf(getDebugBuffer(), 1024, "espnowenabled set to %s (takes effect after reboot)", enabled ? "1" : "0");
  return getDebugBuffer();
}

// ============================================================================
// Modular Settings Registry Implementation
// ============================================================================

// ============================================================================
// Debug Settings Module (for modular settings registry)
// ============================================================================

static const SettingEntry debugSettingEntries[] = {
  { "authCookies", SETTING_BOOL, &gSettings.debugAuthCookies, 0, 0, nullptr, 0, 1, "Auth Cookies", nullptr },
  { "http", SETTING_BOOL, &gSettings.debugHttp, 0, 0, nullptr, 0, 1, "HTTP", nullptr },
  { "sse", SETTING_BOOL, &gSettings.debugSse, 0, 0, nullptr, 0, 1, "SSE", nullptr },
  { "cli", SETTING_BOOL, &gSettings.debugCli, 0, 0, nullptr, 0, 1, "CLI", nullptr },
  { "auth", SETTING_BOOL, &gSettings.debugAuth, 0, 0, nullptr, 0, 1, "Auth", nullptr },
  { "sensors", SETTING_BOOL, &gSettings.debugSensors, 0, 0, nullptr, 0, 1, "Sensors", nullptr },
  { "espNow", SETTING_BOOL, &gSettings.debugEspNow, 0, 0, nullptr, 0, 1, "ESP-NOW", nullptr },
  { "wifi", SETTING_BOOL, &gSettings.debugWifi, 0, 0, nullptr, 0, 1, "WiFi", nullptr },
  { "storage", SETTING_BOOL, &gSettings.debugStorage, 0, 0, nullptr, 0, 1, "Storage", nullptr },
  { "performance", SETTING_BOOL, &gSettings.debugPerformance, 0, 0, nullptr, 0, 1, "Performance", nullptr },
  { "dateTime", SETTING_BOOL, &gSettings.debugDateTime, 0, 0, nullptr, 0, 1, "Date/Time", nullptr },
  { "cmdFlow", SETTING_BOOL, &gSettings.debugCommandFlow, 0, 0, nullptr, 0, 1, "Command Flow", nullptr },
  { "users", SETTING_BOOL, &gSettings.debugUsers, 0, 0, nullptr, 0, 1, "Users", nullptr },
  { "system", SETTING_BOOL, &gSettings.debugSystem, 0, 0, nullptr, 0, 1, "System", nullptr },
  { "automations", SETTING_BOOL, &gSettings.debugAutomations, 0, 0, nullptr, 0, 1, "Automations", nullptr },
  { "logger", SETTING_BOOL, &gSettings.debugLogger, 0, 0, nullptr, 0, 1, "Logger", nullptr },
  { "espNowStream", SETTING_BOOL, &gSettings.debugEspNowStream, 0, 0, nullptr, 0, 1, "ESP-NOW Stream", nullptr },
  { "espNowCore", SETTING_BOOL, &gSettings.debugEspNowCore, 0, 0, nullptr, 0, 1, "ESP-NOW Core", nullptr },
  { "espNowRouter", SETTING_BOOL, &gSettings.debugEspNowRouter, 0, 0, nullptr, 0, 1, "ESP-NOW Router", nullptr },
  { "espNowMesh", SETTING_BOOL, &gSettings.debugEspNowMesh, 0, 0, nullptr, 0, 1, "ESP-NOW Mesh", nullptr },
  { "espNowTopo", SETTING_BOOL, &gSettings.debugEspNowTopo, 0, 0, nullptr, 0, 1, "ESP-NOW Topology", nullptr },
  { "espNowEncryption", SETTING_BOOL, &gSettings.debugEspNowEncryption, 0, 0, nullptr, 0, 1, "ESP-NOW Encryption", nullptr },
  { "autoScheduler", SETTING_BOOL, &gSettings.debugAutoScheduler, 0, 0, nullptr, 0, 1, "Automations Scheduler", nullptr },
  { "autoExec", SETTING_BOOL, &gSettings.debugAutoExec, 0, 0, nullptr, 0, 1, "Automations Execution", nullptr },
  { "autoCondition", SETTING_BOOL, &gSettings.debugAutoCondition, 0, 0, nullptr, 0, 1, "Automations Conditions", nullptr },
  { "autoTiming", SETTING_BOOL, &gSettings.debugAutoTiming, 0, 0, nullptr, 0, 1, "Automations Timing", nullptr },
  { "memory", SETTING_BOOL, &gSettings.debugMemory, 1, 0, nullptr, 0, 1, "Memory", nullptr },
  { "commandSystem", SETTING_BOOL, &gSettings.debugCommandSystem, 0, 0, nullptr, 0, 1, "Command System", nullptr },
  { "settingsSystem", SETTING_BOOL, &gSettings.debugSettingsSystem, 0, 0, nullptr, 0, 1, "Settings System", nullptr },
  { "fmRadio", SETTING_BOOL, &gSettings.debugFmRadio, 0, 0, nullptr, 0, 1, "FM Radio", nullptr },
  { "g2", SETTING_BOOL, &gSettings.debugG2, 1, 0, nullptr, 0, 1, "G2 Glasses", nullptr },
  { "i2c", SETTING_BOOL, &gSettings.debugI2C, 1, 0, nullptr, 0, 1, "I2C Bus", nullptr },
  { "authSessions", SETTING_BOOL, &gSettings.debugAuthSessions, 0, 0, nullptr, 0, 1, "Auth Sessions", nullptr },
  { "authCookies", SETTING_BOOL, &gSettings.debugAuthCookies, 0, 0, nullptr, 0, 1, "Auth Cookies", nullptr },
  { "authLogin", SETTING_BOOL, &gSettings.debugAuthLogin, 0, 0, nullptr, 0, 1, "Auth Login", nullptr },
  { "authBootId", SETTING_BOOL, &gSettings.debugAuthBootId, 0, 0, nullptr, 0, 1, "Auth BootID", nullptr },
  { "httpHandlers", SETTING_BOOL, &gSettings.debugHttpHandlers, 0, 0, nullptr, 0, 1, "HTTP Handlers", nullptr },
  { "httpRequests", SETTING_BOOL, &gSettings.debugHttpRequests, 0, 0, nullptr, 0, 1, "HTTP Requests", nullptr },
  { "httpResponses", SETTING_BOOL, &gSettings.debugHttpResponses, 0, 0, nullptr, 0, 1, "HTTP Responses", nullptr },
  { "httpStreaming", SETTING_BOOL, &gSettings.debugHttpStreaming, 0, 0, nullptr, 0, 1, "HTTP Streaming", nullptr },
  { "wifiConnection", SETTING_BOOL, &gSettings.debugWifiConnection, 0, 0, nullptr, 0, 1, "WiFi Connection", nullptr },
  { "wifiConfig", SETTING_BOOL, &gSettings.debugWifiConfig, 0, 0, nullptr, 0, 1, "WiFi Config", nullptr },
  { "wifiScanning", SETTING_BOOL, &gSettings.debugWifiScanning, 0, 0, nullptr, 0, 1, "WiFi Scanning", nullptr },
  { "wifiDriver", SETTING_BOOL, &gSettings.debugWifiDriver, 0, 0, nullptr, 0, 1, "WiFi Driver", nullptr },
  { "storageFiles", SETTING_BOOL, &gSettings.debugStorageFiles, 0, 0, nullptr, 0, 1, "Storage Files", nullptr },
  { "storageJson", SETTING_BOOL, &gSettings.debugStorageJson, 0, 0, nullptr, 0, 1, "Storage JSON", nullptr },
  { "storageSettings", SETTING_BOOL, &gSettings.debugStorageSettings, 0, 0, nullptr, 0, 1, "Storage Settings", nullptr },
  { "storageMigration", SETTING_BOOL, &gSettings.debugStorageMigration, 0, 0, nullptr, 0, 1, "Storage Migration", nullptr },
  { "systemBoot", SETTING_BOOL, &gSettings.debugSystemBoot, 0, 0, nullptr, 0, 1, "System Boot", nullptr },
  { "systemConfig", SETTING_BOOL, &gSettings.debugSystemConfig, 0, 0, nullptr, 0, 1, "System Config", nullptr },
  { "systemTasks", SETTING_BOOL, &gSettings.debugSystemTasks, 0, 0, nullptr, 0, 1, "System Tasks", nullptr },
  { "systemHardware", SETTING_BOOL, &gSettings.debugSystemHardware, 0, 0, nullptr, 0, 1, "System Hardware", nullptr },
  { "usersMgmt", SETTING_BOOL, &gSettings.debugUsersMgmt, 0, 0, nullptr, 0, 1, "Users Management", nullptr },
  { "usersRegister", SETTING_BOOL, &gSettings.debugUsersRegister, 0, 0, nullptr, 0, 1, "Users Registration", nullptr },
  { "usersQuery", SETTING_BOOL, &gSettings.debugUsersQuery, 0, 0, nullptr, 0, 1, "Users Query", nullptr },
  { "cliExecution", SETTING_BOOL, &gSettings.debugCliExecution, 0, 0, nullptr, 0, 1, "CLI Execution", nullptr },
  { "cliQueue", SETTING_BOOL, &gSettings.debugCliQueue, 0, 0, nullptr, 0, 1, "CLI Queue", nullptr },
  { "cliValidation", SETTING_BOOL, &gSettings.debugCliValidation, 0, 0, nullptr, 0, 1, "CLI Validation", nullptr },
  { "perfStack", SETTING_BOOL, &gSettings.debugPerfStack, 0, 0, nullptr, 0, 1, "Performance Stack", nullptr },
  { "perfHeap", SETTING_BOOL, &gSettings.debugPerfHeap, 0, 0, nullptr, 0, 1, "Performance Heap", nullptr },
  { "perfTiming", SETTING_BOOL, &gSettings.debugPerfTiming, 0, 0, nullptr, 0, 1, "Performance Timing", nullptr },
  { "sseConnection", SETTING_BOOL, &gSettings.debugSseConnection, 0, 0, nullptr, 0, 1, "SSE Connection", nullptr },
  { "sseEvents", SETTING_BOOL, &gSettings.debugSseEvents, 0, 0, nullptr, 0, 1, "SSE Events", nullptr },
  { "sseBroadcast", SETTING_BOOL, &gSettings.debugSseBroadcast, 0, 0, nullptr, 0, 1, "SSE Broadcast", nullptr },
  { "cmdflowRouting", SETTING_BOOL, &gSettings.debugCmdflowRouting, 0, 0, nullptr, 0, 1, "Command Flow Routing", nullptr },
  { "cmdflowQueue", SETTING_BOOL, &gSettings.debugCmdflowQueue, 0, 0, nullptr, 0, 1, "Command Flow Queue", nullptr },
  { "cmdflowContext", SETTING_BOOL, &gSettings.debugCmdflowContext, 0, 0, nullptr, 0, 1, "Command Flow Context", nullptr },
  { "logLevel", SETTING_INT, &gSettings.logLevel, 3, 0, nullptr, 0, 3, "Log Level", nullptr },
  { "memorySampleIntervalSec", SETTING_INT, &gSettings.memorySampleIntervalSec, 30, 0, nullptr, 0, 300, "Memory Sample Interval (sec)", nullptr }
};

static const SettingsModule debugSettingsModule = {
  "debug",
  "debug",
  debugSettingEntries,
  sizeof(debugSettingEntries) / sizeof(debugSettingEntries[0]),
  nullptr,  // Always available
  "Debug output flags for various subsystems"
};

// ============================================================================
// Output Settings Module (for modular settings registry)
// ============================================================================

static const SettingEntry outputSettingEntries[] = {
  { "serial", SETTING_BOOL, &gSettings.outSerial, 1, 0, nullptr, 0, 1, "Serial Output", nullptr },
  { "web", SETTING_BOOL, &gSettings.outWeb, 1, 0, nullptr, 0, 1, "Web Output", nullptr },
  { "display", SETTING_BOOL, &gSettings.outDisplay, 0, 0, nullptr, 0, 1, "Display Output", nullptr },
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  { "g2", SETTING_BOOL, &gSettings.outG2, 0, 0, nullptr, 0, 1, "G2 Glasses Output", nullptr },
#endif
};

static const SettingsModule outputSettingsModule = {
  "output",
  "output",
  outputSettingEntries,
  sizeof(outputSettingEntries) / sizeof(outputSettingEntries[0]),
  nullptr,  // Always available
  "Output routing for serial, web, and display"
};

// Registry storage
static const SettingsModule* gSettingsModules[MAX_SETTINGS_MODULES] = {nullptr};
static size_t gSettingsModuleCount = 0;
static bool gSettingsModulesRegistered = false;

void registerSettingsModule(const SettingsModule* module) {
  if (!module) return;
  if (gSettingsModuleCount >= MAX_SETTINGS_MODULES) {
    ERROR_SYSTEMF("Max settings modules reached");
    return;
  }
  // Check for duplicate registration
  for (size_t i = 0; i < gSettingsModuleCount; i++) {
    if (gSettingsModules[i] == module) return;  // Already registered
  }
  gSettingsModules[gSettingsModuleCount++] = module;
  DEBUG_SYSTEMF("[Settings] Registered module: %s (%zu entries)", module->name, module->count);
}

// ============================================================================
// Explicit Registration of ALL Settings Modules
// Called once early in boot to ensure all modules are available for defaults
// ============================================================================

// Extern declarations for all settings modules
extern const SettingsModule i2cSettingsModule;
extern const SettingsModule cliSettingsModule;
#if ENABLE_WIFI
extern const SettingsModule wifiSettingsModule;
#endif
#if ENABLE_HTTP_SERVER
extern const SettingsModule httpSettingsModule;
#endif
#if ENABLE_ESPNOW
extern const SettingsModule espnowSettingsModule;
#endif
#if ENABLE_MQTT
extern const SettingsModule mqttSettingsModule;
#endif
#if ENABLE_AUTOMATION
extern const SettingsModule automationSettingsModule;
#endif
extern const SettingsModule powerSettingsModule;
extern const SettingsModule ledSettingsModule;
#if ENABLE_OLED_DISPLAY
extern const SettingsModule oledSettingsModule;
#endif
#if ENABLE_BLUETOOTH
extern const SettingsModule bluetoothSettingsModule;
#endif
#if ENABLE_THERMAL_SENSOR
extern const SettingsModule thermalSettingsModule;
#endif
#if ENABLE_TOF_SENSOR
extern const SettingsModule tofSettingsModule;
#endif
#if ENABLE_IMU_SENSOR
extern const SettingsModule imuSettingsModule;
#endif
#if ENABLE_GAMEPAD_SENSOR
extern const SettingsModule gamepadSettingsModule;
#endif
#if ENABLE_APDS_SENSOR
extern const SettingsModule apdsSettingsModule;
#endif
#if ENABLE_GPS_SENSOR
extern const SettingsModule gpsSettingsModule;
#endif
#if ENABLE_FMRADIO_SENSOR
extern const SettingsModule fmRadioSettingsModule;
#endif
#if ENABLE_RTC_SENSOR
extern const SettingsModule rtcSettingsModule;
#endif

#if ENABLE_PRESENCE_SENSOR
extern const SettingsModule presenceSettingsModule;
#endif

#if ENABLE_CAMERA_SENSOR
extern const SettingsModule cameraSettingsModule;
#endif
#if ENABLE_MICROPHONE_SENSOR
extern const SettingsModule micSettingsModule;
#endif
#if ENABLE_EDGE_IMPULSE
extern const SettingsModule edgeImpulseSettingsModule;
#endif
#if ENABLE_ESP_SR
extern const SettingsModule espsrSettingsModule;
#endif

extern const SettingsModule sensorLogSettingsModule;

void registerAllSettingsModules() {
  if (gSettingsModulesRegistered) return;  // Only register once
  gSettingsModulesRegistered = true;
  
  // Core system modules
  registerSettingsModule(&debugSettingsModule);
  registerSettingsModule(&outputSettingsModule);
  registerSettingsModule(&i2cSettingsModule);
  registerSettingsModule(&cliSettingsModule);
#if ENABLE_AUTOMATION
  registerSettingsModule(&automationSettingsModule);
#endif
  registerSettingsModule(&powerSettingsModule);
  registerSettingsModule(&ledSettingsModule);
  
  // Network modules
#if ENABLE_WIFI
  registerSettingsModule(&wifiSettingsModule);
#endif
#if ENABLE_HTTP_SERVER
  registerSettingsModule(&httpSettingsModule);
#endif
#if ENABLE_ESPNOW
  registerSettingsModule(&espnowSettingsModule);
#endif
#if ENABLE_MQTT
  registerSettingsModule(&mqttSettingsModule);
#endif
#if ENABLE_BLUETOOTH
  registerSettingsModule(&bluetoothSettingsModule);
#endif

  // Display modules
#if ENABLE_OLED_DISPLAY
  registerSettingsModule(&oledSettingsModule);
#endif

  // Sensor modules
#if ENABLE_THERMAL_SENSOR
  registerSettingsModule(&thermalSettingsModule);
#endif
#if ENABLE_TOF_SENSOR
  registerSettingsModule(&tofSettingsModule);
#endif
#if ENABLE_IMU_SENSOR
  registerSettingsModule(&imuSettingsModule);
#endif
#if ENABLE_GAMEPAD_SENSOR
  registerSettingsModule(&gamepadSettingsModule);
#endif
#if ENABLE_APDS_SENSOR
  registerSettingsModule(&apdsSettingsModule);
#endif
#if ENABLE_GPS_SENSOR
  registerSettingsModule(&gpsSettingsModule);
#endif
#if ENABLE_FMRADIO_SENSOR
  registerSettingsModule(&fmRadioSettingsModule);
#endif
#if ENABLE_RTC_SENSOR
  registerSettingsModule(&rtcSettingsModule);
#endif

#if ENABLE_PRESENCE_SENSOR
  registerSettingsModule(&presenceSettingsModule);
#endif

#if ENABLE_CAMERA_SENSOR
  registerSettingsModule(&cameraSettingsModule);
#endif
#if ENABLE_MICROPHONE_SENSOR
  registerSettingsModule(&micSettingsModule);
#endif
#if ENABLE_EDGE_IMPULSE
  registerSettingsModule(&edgeImpulseSettingsModule);
#endif
#if ENABLE_ESP_SR
  registerSettingsModule(&espsrSettingsModule);
#endif

  // Sensor logging
  registerSettingsModule(&sensorLogSettingsModule);

  DEBUG_SYSTEMF("[Settings] All %zu modules registered", gSettingsModuleCount);
}


const SettingsModule** getSettingsModules(size_t& count) {
  count = gSettingsModuleCount;
  return gSettingsModules;
}

void applyRegisteredDefaults() {
  for (size_t m = 0; m < gSettingsModuleCount; m++) {
    const SettingsModule* mod = gSettingsModules[m];
    for (size_t i = 0; i < mod->count; i++) {
      const SettingEntry* e = &mod->entries[i];
      switch (e->type) {
        case SETTING_INT:
          *((int*)e->valuePtr) = e->intDefault;
          break;
        case SETTING_FLOAT:
          *((float*)e->valuePtr) = e->floatDefault;
          break;
        case SETTING_BOOL:
          *((bool*)e->valuePtr) = (e->intDefault != 0);
          break;
        case SETTING_STRING:
          *((String*)e->valuePtr) = e->stringDefault ? e->stringDefault : "";
          break;
      }
    }
  }
}

size_t readRegisteredSettings(JsonDocument& doc) {
  size_t count = 0;

  for (size_t m = 0; m < gSettingsModuleCount; m++) {
    const SettingsModule* mod = gSettingsModules[m];
    JsonVariant section = mod->jsonSection ? doc[mod->jsonSection].as<JsonObject>() : doc.as<JsonObject>();
    if (section.isNull()) continue;
    
    for (size_t i = 0; i < mod->count; i++) {
      const SettingEntry* e = &mod->entries[i];
      const char* key = e->jsonKey;
      JsonVariant current = section;
      const char* p = key;
      const char* dot = strchr(p, '.');
      while (dot) {
        String seg = String(p).substring(0, dot - p);
        current = current[seg.c_str()];
        if (current.isNull()) break;
        p = dot + 1;
        dot = strchr(p, '.');
      }
      if (current.isNull()) continue;
      JsonVariant val = current[p];
      if (val.isNull()) continue;
      switch (e->type) {
        case SETTING_INT:
          *((int*)e->valuePtr) = val | e->intDefault;
          count++;
          break;
        case SETTING_FLOAT:
          *((float*)e->valuePtr) = val | e->floatDefault;
          count++;
          break;
        case SETTING_BOOL:
          *((bool*)e->valuePtr) = val | (e->intDefault != 0);
          count++;
          break;
        case SETTING_STRING:
          if (e->isSecret) {
            // Decrypt secret strings when reading from disk
            const char* encrypted = val | (e->stringDefault ? e->stringDefault : "");
            *((String*)e->valuePtr) = decryptWifiPassword(encrypted);
          } else {
            *((String*)e->valuePtr) = val | (e->stringDefault ? e->stringDefault : "");
          }
          count++;
          break;
      }
    }
  }
  return count;
}

void printSettingsModuleSummary() {
  size_t count = 0;
  const SettingsModule** mods = getSettingsModules(count);
  DEBUG_SYSTEMF("[SettingsSystem] %zu modules registered", count);
  for (size_t i = 0; i < count; ++i) {
    const SettingsModule* m = mods[i];
    const char* sect = m->jsonSection ? m->jsonSection : "<root>";
    DEBUG_SYSTEMF("[SettingsSystem]   Module '%s' section '%s': %zu entries", m->name, sect, m->count);
  }
}

size_t writeRegisteredSettings(JsonDocument& doc) {
  size_t count = 0;
  
  for (size_t m = 0; m < gSettingsModuleCount; m++) {
    const SettingsModule* mod = gSettingsModules[m];
    // Get or create section object if specified
    // IMPORTANT: Use as<JsonObject>() for root to avoid clearing existing content
    // Use to<JsonObject>() for named sections to create/replace them
    JsonObject section = mod->jsonSection
                           ? doc[mod->jsonSection].to<JsonObject>()
                           : doc.as<JsonObject>();
    
    if (section.isNull()) {
      ERROR_STORAGEF("Failed to create section for module %s", mod->name);
      continue;
    }
    
    for (size_t i = 0; i < mod->count; i++) {
      const SettingEntry* e = &mod->entries[i];
      
      // Check for null pointer before dereferencing
      if (!e->valuePtr) {
        ERROR_STORAGEF("Null pointer for setting %s", e->jsonKey);
        continue;
      }
      
      const char* key = e->jsonKey;
      JsonObject target = section;
      const char* p = key;
      const char* dot = strchr(p, '.');
      while (dot) {
        String seg = String(p).substring(0, dot - p);
        // Get existing JsonObject or create new one (don't overwrite)
        JsonObject existing = target[seg.c_str()].as<JsonObject>();
        if (existing.isNull()) {
          existing = target[seg.c_str()].to<JsonObject>();
        }
        target = existing;
        p = dot + 1;
        dot = strchr(p, '.');
      }
      const char* leaf = p;
      
      switch (e->type) {
        case SETTING_INT:
          target[leaf] = *((int*)e->valuePtr);
          count++;
          break;
        case SETTING_FLOAT:
          target[leaf] = *((float*)e->valuePtr);
          count++;
          break;
        case SETTING_BOOL:
          target[leaf] = *((bool*)e->valuePtr);
          count++;
          break;
        case SETTING_STRING:
          if (e->isSecret) {
            // Encrypt secret strings before writing to disk
            String plaintext = *((String*)e->valuePtr);
            if (plaintext.length() > 0) {
              target[leaf] = encryptWifiPassword(plaintext);
            } else {
              target[leaf] = "";
            }
          } else {
            target[leaf] = *((String*)e->valuePtr);
          }
          count++;
          break;
      }
    }
  }
  return count;
}

const char* handleSettingCommand(const SettingEntry* entry, const String& cmd) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  // Find the value after the command name
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) {
    // No argument - show current value
    static char buf[128];
    switch (entry->type) {
      case SETTING_INT:
        snprintf(buf, sizeof(buf), "%s = %d", entry->jsonKey, *((int*)entry->valuePtr));
        break;
      case SETTING_FLOAT:
        snprintf(buf, sizeof(buf), "%s = %.3f", entry->jsonKey, *((float*)entry->valuePtr));
        break;
      case SETTING_BOOL:
        snprintf(buf, sizeof(buf), "%s = %s", entry->jsonKey, *((bool*)entry->valuePtr) ? "true" : "false");
        break;
      case SETTING_STRING:
        snprintf(buf, sizeof(buf), "%s = %s", entry->jsonKey, ((String*)entry->valuePtr)->c_str());
        break;
    }
    return buf;
  }
  
  while (*p == ' ') p++;  // Skip whitespace
  
  // Parse and validate based on type
  switch (entry->type) {
    case SETTING_INT: {
      int v = atoi(p);
      if (entry->minVal != 0 || entry->maxVal != 0) {
        if (v < entry->minVal || v > entry->maxVal) {
          static char errBuf[128];
          snprintf(errBuf, sizeof(errBuf), "Error: %s must be %d..%d", entry->jsonKey, entry->minVal, entry->maxVal);
          return errBuf;
        }
      }
      *((int*)entry->valuePtr) = v;
      if (!gDeferWrites) writeSettingsJson();
      BROADCAST_PRINTF("%s set to %d", entry->jsonKey, v);
      { char vBuf[16]; snprintf(vBuf, sizeof(vBuf), "%d", v); notifySettingChanged(entry->label ? entry->label : entry->jsonKey, vBuf); }
      return "[Settings] Configuration updated";
    }
    case SETTING_FLOAT: {
      float f = strtof(p, nullptr);
      if (entry->minVal != 0 || entry->maxVal != 0) {
        if (f < (float)entry->minVal || f > (float)entry->maxVal) {
          static char errBuf[128];
          snprintf(errBuf, sizeof(errBuf), "Error: %s must be %d..%d", entry->jsonKey, entry->minVal, entry->maxVal);
          return errBuf;
        }
      }
      *((float*)entry->valuePtr) = f;
      if (!gDeferWrites) writeSettingsJson();
      BROADCAST_PRINTF("%s set to %.3f", entry->jsonKey, f);
      { char vBuf[16]; snprintf(vBuf, sizeof(vBuf), "%.3f", f); notifySettingChanged(entry->label ? entry->label : entry->jsonKey, vBuf); }
      return "[Settings] Configuration updated";
    }
    case SETTING_BOOL: {
      bool v = (*p == '1' || strncasecmp(p, "true", 4) == 0);
      *((bool*)entry->valuePtr) = v;
      if (!gDeferWrites) writeSettingsJson();
      BROADCAST_PRINTF("%s set to %s", entry->jsonKey, v ? "true" : "false");
      notifySettingChanged(entry->label ? entry->label : entry->jsonKey, v ? "on" : "off");
      return "[Settings] Configuration updated";
    }
    case SETTING_STRING: {
      *((String*)entry->valuePtr) = p;
      if (!gDeferWrites) writeSettingsJson();
      BROADCAST_PRINTF("%s set to %s", entry->jsonKey, p);
      notifySettingChanged(entry->label ? entry->label : entry->jsonKey, p);
      return "[Settings] Configuration updated";
    }
  }
  return "Error: Unknown setting type";
}

// ============================================================================
// Per-User Settings (merged from user_settings.cpp)
// ============================================================================

#include "System_Mutex.h"  // For FsLockGuard

// ============================================================================
// Batch write commands
// ============================================================================

const char* cmd_beginwrite(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  gDeferWrites = true;
  return "Write deferred — changes batched until savesettings";
}

const char* cmd_savesettings(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  gDeferWrites = false;
  writeSettingsJson();
  return "Settings saved";
}

// beginwrite and savesettings are registered in settingsCommands[] above.

// ============================================================================

String getUserSettingsPath(uint32_t userId) {
  char pathBuf[48];
  snprintf(pathBuf, sizeof(pathBuf), "/system/users/user_settings/%lu.json", (unsigned long)userId);
  return String(pathBuf);
}

bool loadUserSettings(uint32_t userId, JsonDocument& doc) {
  doc.clear();
  if (!filesystemReady) return false;

  String path = getUserSettingsPath(userId);
  {
    FsLockGuard guard("user_settings.load");
    if (!LittleFS.exists(path.c_str())) {
      doc.to<JsonObject>();
      return true;
    }
    File f = LittleFS.open(path.c_str(), "r");
    if (!f) return false;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
      doc.clear();
      return false;
    }
  }

  if (doc.isNull()) {
    doc.to<JsonObject>();
  }
  return true;
}

bool saveUserSettings(uint32_t userId, const JsonDocument& doc) {
  if (!filesystemReady) return false;

  String path = getUserSettingsPath(userId);
  String tmp = path + ".tmp";

  {
    FsLockGuard guard("user_settings.save");
    File f = LittleFS.open(tmp.c_str(), "w");
    if (!f) return false;
    size_t written = serializeJson(doc, f);
    f.close();
    if (written == 0) {
      LittleFS.remove(tmp.c_str());
      return false;
    }

    // Safe atomic replace: rename first, only delete original on success.
    // Do NOT delete the original before confirming rename succeeded —
    // if both rename and fallback fail, the original would be permanently lost.
    if (!LittleFS.rename(tmp.c_str(), path.c_str())) {
      // Rename failed (e.g. cross-dir); fallback to direct overwrite
      File direct = LittleFS.open(path.c_str(), "w");
      if (!direct) {
        LittleFS.remove(tmp.c_str());
        return false;
      }
      written = serializeJson(doc, direct);
      direct.close();
      LittleFS.remove(tmp.c_str());
      return written > 0;
    }
  }

  return true;
}

bool mergeAndSaveUserSettings(uint32_t userId, const JsonDocument& patch) {
  if (!filesystemReady) return false;
  if (!patch.is<JsonObjectConst>()) return false;

  JsonDocument base;
  if (!loadUserSettings(userId, base)) return false;
  if (!base.is<JsonObject>()) base.to<JsonObject>();

  JsonObject dst = base.as<JsonObject>();
  for (JsonPairConst kv : patch.as<JsonObjectConst>()) {
    dst[kv.key()] = kv.value();
  }
  return saveUserSettings(userId, base);
}
