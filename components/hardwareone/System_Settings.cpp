#include "System_Settings.h"        // Settings struct definition and function declarations
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_BuildConfig.h"   // ENABLE_WIFI, ENABLE_ESPNOW flags
#include "System_PollPause.h"     // pollPause/pollResume — global sensor-poll pause
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
#include "System_Clock.h"    // Clock::applyTimezone — push tz offset into libc TZ
#include "BLE_Peers.h"       // bluetooth.peers JSON (de)serialization
#include "System_Command.h"
#include "System_Notifications.h"
#include "System_ESPSR.h"  // srSyncDebugLevel() — derive legacy gSrDebugLevel from flags
#include "System_SelfDevice.h"  // SelfDevice::firmwareVersion() — Stage 1 consolidation
#include "System_SensorLogging.h"  // gSensorLogMask / gSensorLogFormat sync from settings editors
#include <LittleFS.h>
#include "System_VFS.h"      // VFS::*Guarded + systemAuth (Phase 2 perm refactor)
#include <esp_system.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#include "esp_flash.h"
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
// (filesystemReady is provided by System_Filesystem.h, included below)
#include "System_Filesystem.h"

// ----------------------------------------------------------------------------
// Dotted-path JSON helpers
// ----------------------------------------------------------------------------
// SettingsModule::jsonSection now supports dotted paths like
// "hardware.sensors.camera" so settings can nest in JSON without bloating the
// flat top level. These two helpers walk such a path:
//   - jsonPathCreate: get-or-create each segment, returns the leaf JsonObject.
//   - jsonPathRead:   read-only traversal, returns null-variant on any miss.
// Path == nullptr or "" returns the document root (legacy behavior).
static JsonObject jsonPathCreate(JsonDocument& doc, const char* path) {
  if (!path || !*path) return doc.as<JsonObject>();
  JsonVariant current = doc.as<JsonVariant>();
  const char* segStart = path;
  while (true) {
    const char* dot = strchr(segStart, '.');
    size_t segLen = dot ? (size_t)(dot - segStart) : strlen(segStart);
    char segment[64];
    if (segLen == 0 || segLen >= sizeof(segment)) return JsonObject();
    memcpy(segment, segStart, segLen);
    segment[segLen] = '\0';
    JsonVariant next = current[segment];
    if (next.isNull() || !next.is<JsonObject>()) {
      next = current[segment].to<JsonObject>();
    }
    current = next;
    if (!dot) break;
    segStart = dot + 1;
  }
  return current.as<JsonObject>();
}

static JsonVariantConst jsonPathRead(const JsonDocument& doc, const char* path) {
  if (!path || !*path) return doc.as<JsonVariantConst>();
  JsonVariantConst current = doc.as<JsonVariantConst>();
  const char* segStart = path;
  while (true) {
    const char* dot = strchr(segStart, '.');
    size_t segLen = dot ? (size_t)(dot - segStart) : strlen(segStart);
    char segment[64];
    if (segLen == 0 || segLen >= sizeof(segment)) return JsonVariantConst();
    memcpy(segment, segStart, segLen);
    segment[segLen] = '\0';
    current = current[segment];
    if (current.isNull()) return current;
    if (!dot) break;
    segStart = dot + 1;
  }
  return current;
}
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

const char* cmd_webclihistorysize(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: webclihistorysize <1..100>";
  int v = valStr.toInt();
  if (v < 1) v = 1;
  if (v > 100) v = 100;
  setSetting(gSettings.webCliHistorySize, v);
  snprintf(getDebugBuffer(), 1024, "webCliHistorySize set to %d", v);
  return getDebugBuffer();
}

const char* cmd_oledclihistorysize(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: oledclihistorysize <10..100>";
  int v = valStr.toInt();
  if (v < 10) v = 10;  // Minimum 10 lines for OLED
  if (v > 100) v = 100;
  setSetting(gSettings.oledCliHistorySize, v);
  snprintf(getDebugBuffer(), 1024, "oledCliHistorySize set to %d (requires reboot)", v);
  return getDebugBuffer();
}

const char* cmd_outserial(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String t1 = ca.arg(0);
  String t2 = ca.arg(1);
  bool modeTemp = false;
  // "0" -> 0, any other integer -> 1, non-numeric/missing -> -1 (error)
  auto parseBit = [](const String& s) -> int {
    if (s == "0") return 0;
    return s.toInt() != 0 ? 1 : -1;
  };
  int v = -1;
  if (t1.length() && (t1 == "temp" || t1 == "persist")) {
    modeTemp = (t1 == "temp");
    if (t2.length()) v = parseBit(t2);
  } else {
    if (t1.length()) v = parseBit(t1);
    if (t2.length()) { modeTemp = (t2 == "temp"); }
  }
  if (v < 0) return "Error: invalid arguments — Usage: outserial <0|1> [persist|temp]";
  if (!v) {
    // The drain gates the UART on this bit, so once it clears, the command's
    // own reply can no longer reach a serial console. Print the confirmation
    // directly first so the console's last line explains why it went quiet.
    Serial.println(modeTemp ? "outSerial (runtime) set to 0"
                            : "outSerial (persisted) set to 0");
  }
  if (modeTemp) {
    if (v) gOutputFlags |= MSG_ROUTE_SERIAL;
    else gOutputFlags &= ~MSG_ROUTE_SERIAL;
    return v ? "outSerial (runtime) set to 1" : "outSerial (runtime) set to 0";
  } else {
    setSetting(gSettings.outSerial, (bool)(v != 0));
    if (v) gOutputFlags |= MSG_ROUTE_SERIAL;
    else gOutputFlags &= ~MSG_ROUTE_SERIAL;
    return gSettings.outSerial ? "outSerial (persisted) set to 1" : "outSerial (persisted) set to 0";
  }
}

// ============================================================================
// Settings Command Registry
// ============================================================================

const char* cmd_beginwrite(const String& argsInput);
const char* cmd_savesettings(const String& argsInput);
const char* cmd_serialrequireauth(const String&);
const char* cmd_displayrequireauth(const String&);
#if ENABLE_HTTP_SERVER
const char* cmd_httpAutoStart(const String& argsInput);
#endif
#if ENABLE_HTTPS
const char* cmd_httpsEnabled(const String& argsInput);
#endif

const char* cmd_controls(const String& argsInput);  // defined below; machine-readable control descriptor

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry settingsCommands[] = {
  { "controls", "Per-module control descriptor (JSON): controls json [module]", false, cmd_controls, "Usage: controls json <module>  (e.g. 'controls json imu'); 'controls json' lists modules" },
#if ENABLE_WIFI
  // ---- WiFi Network Settings ----
  { "wifitxpower", "Set WiFi TX power: <dBm>", true, cmd_wifitxpower, "Usage: wifitxpower <dBm>" },
  { "wifiautoreconnect", "Keep hunting for the AP after an unexpected drop: <0|1>", true, cmd_wifiautoreconnect,
    "Usage: wifiautoreconnect <0|1>\n  Separate from wifiautostart: this is about recovering a dropped link, not connecting at boot." },
  
  // ---- System Time Settings ----
  { "ntpserver", "Set NTP server: <hostname>", true, cmd_ntpserver, "Usage: ntpserver <host>" },
#endif
  { "tzoffsetminutes", "Set timezone offset: <-720..840>", true, cmd_tzoffsetminutes, "Usage: tzoffsetminutes <-720..840>" },
  
  // Note: Thermal and ToF sensor settings are now in their respective sensor files:
  // - thermal_sensor.cpp: thermalCommands[]
  // - tof_sensor.cpp: tofCommands[]
  
#if ENABLE_ESPNOW
  // ---- Device Settings ----
  { "espnowenabled", "Enable/disable ESP-NOW: <0|1> (reboot required)", true, cmd_espnowenabled, "Usage: espnowenabled <0|1>" },
#endif
#if ENABLE_HTTP_SERVER
  { "httpAutoStart", "Auto-start HTTP server at boot: <0|1>", true, cmd_httpAutoStart, "Usage: httpAutoStart <0|1>" },
#endif
#if ENABLE_HTTPS
  { "httpsEnabled", "Enable/disable HTTPS: <0|1> (reboot required)", true, cmd_httpsEnabled, "Usage: httpsEnabled <0|1>" },
#endif
  
  // Note: I2C settings are now handled by the modular registry in i2c_system.cpp
  
  // ---- CLI Settings ----
  { "webclihistorysize", "Set web CLI history size: <1..100>", true, cmd_webclihistorysize, "Usage: webclihistorysize <1..100>" },
  { "oledclihistorysize", "Set OLED CLI history size: <10..100>", true, cmd_oledclihistorysize, "Usage: oledclihistorysize <10..100>" },

  // ---- Output Settings ----
  { "outserial",          "Set serial output: <0|1> [persist|temp]", true, cmd_outserial, "Usage: outserial <0|1> [persist|temp]" },
  { "serialrequireauth",  "Require auth for serial: <0|1>", true, cmd_serialrequireauth, "Usage: serialrequireauth <0|1>", nullptr, nullptr, /*requiresSuperAdmin=*/true },
  { "displayrequireauth", "Require auth for display: <0|1>", true, cmd_displayrequireauth, "Usage: displayrequireauth <0|1>", nullptr, nullptr, /*requiresSuperAdmin=*/true },

  // ---- Batch write ----
  { "beginwrite",   "Start a batch settings update — defers flash write until savesettings.", true, cmd_beginwrite },
  { "savesettings", "Flush deferred settings to flash (single write).",                       true, cmd_savesettings },
};

const size_t settingsCommandsCount = sizeof(settingsCommands) / sizeof(settingsCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// WiFi Password Encryption Helpers
// ============================================================================

// Cached device key. File-scope (not function-local) so selectDeviceKeyEpoch()
// can install the candidate that actually opens the stored blobs before the
// first decrypt happens.
static String sDeviceKey;
static bool sDeviceKeyInit = false;

// Combine two hardware identifiers into the at-rest key:
// 1. eFuse MAC - unique per ESP32 chip (publicly visible via WiFi/BT)
// 2. Flash chip unique ID - unique per flash chip (NOT publicly visible,
//    requires physical SPI access to read)
static String deriveDeviceKeyFromIds(uint64_t chipId, uint64_t flashUid) {
  char combined[80];
  snprintf(combined, sizeof(combined), "%016llx:%016llx:HARDWAREONE_V1",
           (unsigned long long)chipId,
           (unsigned long long)flashUid);

  // Hash the combination with SHA-256 for a strong, deterministic key
  uint8_t hash[32];
  mbedtls_sha256((const uint8_t*)combined, strlen(combined), hash, 0);

  // Convert to hex string (64 chars)
  char hexBuf[65];
  for (int i = 0; i < 32; i++) {
    snprintf(hexBuf + (i * 2), 3, "%02x", hash[i]);
  }
  hexBuf[64] = '\0';
  return String(hexBuf);
}

// Short non-reversible key identifier for durable logs: lets system-events.log
// show WHICH key epoch each boot ran under without disclosing key material.
static String deviceKeyFingerprint(const String& keyMaterial) {
  uint8_t hash[32];
  mbedtls_sha256((const uint8_t*)keyMaterial.c_str(), keyMaterial.length(), hash, 0);
  char fp[9];
  for (int i = 0; i < 4; i++) {
    snprintf(fp + (i * 2), 3, "%02x", hash[i]);
  }
  fp[8] = '\0';
  return String(fp);
}

String getDeviceEncryptionKey() {
  // Cache the key so we only generate (and log) once per boot
  if (sDeviceKeyInit) {
    return sDeviceKey;
  }

  DEBUG_SYSTEMF("[Encryption] Generating device encryption key");

  uint64_t chipId = ESP.getEfuseMac();

  uint64_t flashUid = 0;
  esp_err_t err = esp_flash_read_unique_chip_id(NULL, &flashUid);
  if (err != ESP_OK) {
    DEBUG_SYSTEMF("[Encryption] Warning: could not read flash UID (0x%x), using MAC only", err);
    flashUid = 0;
  }

  sDeviceKey = deriveDeviceKeyFromIds(chipId, flashUid);
  sDeviceKeyInit = true;
  DEBUG_SYSTEMF("[Encryption] Key generated, length=%d", sDeviceKey.length());
  return sDeviceKey;
}

String getDeviceFingerprint() {
  static String sFingerprint;
  if (sFingerprint.length() > 0) return sFingerprint;

  // Hash a known constant with the device encryption key to produce
  // a one-way fingerprint that is safe to include in backup files.
  // Cannot be reversed to recover the encryption key.
  String key = getDeviceEncryptionKey();
  String input = key + ":HWBACKUP_DEVICE_FINGERPRINT";
  secureClearString(key);

  uint8_t hash[32];
  mbedtls_sha256((const uint8_t*)input.c_str(), input.length(), hash, 0);
  secureClearString(input);

  char hexBuf[65];
  for (int i = 0; i < 32; i++) {
    snprintf(hexBuf + (i * 2), 3, "%02x", hash[i]);
  }
  hexBuf[64] = '\0';

  sFingerprint = String(hexBuf);
  return sFingerprint;
}

String encryptString(const String& password) {
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
  uint8_t* plaintext = (uint8_t*)ps_alloc(paddedLen, AllocPref::PreferPSRAM, "aes.enc.plain");
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

  uint8_t* ciphertext = (uint8_t*)ps_alloc(paddedLen, AllocPref::PreferPSRAM, "aes.enc.cipher");
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
  char* result = (char*)ps_alloc(resultLen, AllocPref::PreferPSRAM, "aes.enc.result");
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

  DEBUG_STORAGEF("[AES] String encrypted (len=%d)", output.length());
  return output;
}

String decryptString(const String& encryptedPassword) {
  if (encryptedPassword.length() == 0) {
    return "";
  }

  if (!encryptedPassword.startsWith("AES:")) {
    ERROR_STORAGEF("[AES] Invalid encrypted string format (missing AES: prefix)");
    return "";
  }

  DEBUG_STORAGEF("[AES] Decrypting string (len=%d)", encryptedPassword.length());

  // Parse: AES:<32-hex-iv>:<hex-ciphertext>
  if (encryptedPassword.length() < 41) {  // "AES:" + 32 hex chars + ":" minimum
    ERROR_STORAGEF("[AES] Encrypted string too short");
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
  uint8_t* ciphertext = (uint8_t*)ps_alloc(ciphertextLen, AllocPref::PreferPSRAM, "aes.dec.cipher");
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

  uint8_t* plaintext = (uint8_t*)ps_alloc(ciphertextLen, AllocPref::PreferPSRAM, "aes.dec.plain");
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
  // Strict PKCS#7: every padding byte must equal padValue. A wrong-key decrypt
  // produces random bytes that pass the single-byte range check ~6% of the
  // time and then masquerade as a real (garbage) secret — e.g. a WiFi password
  // that associates but loops on the 4-way handshake forever.
  for (int pi = ciphertextLen - padValue; pi < ciphertextLen; pi++) {
    if (plaintext[pi] != padValue) {
      free(plaintext);
      ERROR_STORAGEF("[AES] Corrupt padding — wrong key or damaged blob");
      return "";
    }
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

  DEBUG_STORAGEF("[AES] String decrypted successfully (len=%d)", output.length());
  return output;
}

// JSON-field secret helpers (declared in System_Settings.h). Encrypt-on-write /
// decrypt-on-read for recoverable secrets stored inside a JSON object — the
// array/struct equivalent of a SettingEntry isSecret flag. Empty plaintext is
// stored as "" (not an encrypted blob), and getSecret returns "" for an absent
// or empty field (decryptString already rejects non-"AES:" input).
// Count of stored AES blobs that failed to decrypt during this boot's load.
// While non-zero, the save path refuses to replace a stored blob with "" —
// the RAM emptiness is damage, not intent, and the blob is still recoverable
// once the key epoch is right again.
static int gSecretLoadFailures = 0;

int secretLoadFailureCount() { return gSecretLoadFailures; }

// True once this boot's settings state is trustworthy as the source of truth
// for full-array rewrites: either readSettingsJson() completed, or there was
// no settings.json to load (first boot / post-erase — HardwareOne.cpp calls
// settingsMarkLoadedOk before writing defaults). While false (load FAILED
// mid-way), buildSettingsJsonDoc skips the wifi-networks rebuild so a save
// can't wipe the on-disk list with a half-loaded RAM copy.
static bool gSettingsLoadedOk = false;

void settingsMarkLoadedOk() { gSettingsLoadedOk = true; }

// Guarded secret write: never replaces a stored "AES:" blob with "" after a
// damaged load, and never persists an empty result from a FAILED encryption
// of a non-empty value (encryptString returns "" on alloc/mbedtls errors).
// prevBlob is the value that was on disk for this key (from the merge-read).
void putSecretPreserving(JsonObject obj, const char* key, const String& plaintext, const String& prevBlob) {
  bool prevIsBlob = prevBlob.startsWith("AES:");
  if (plaintext.length() > 0) {
    String enc = encryptString(plaintext);
    if (enc.length() == 0 && prevIsBlob) {
      obj[key] = prevBlob;
      logSystemEvent("SETTINGS", "secret '%s': encryption FAILED — kept previous stored value", key);
    } else {
      obj[key] = enc;
    }
  } else if (prevIsBlob && gSecretLoadFailures > 0) {
    obj[key] = prevBlob;
    logSystemEvent("SETTINGS", "secret '%s': empty after damaged load — kept previous stored value", key);
  } else {
    // Genuine empty (never set, or explicitly cleared on a healthy boot)
    obj[key] = String("");
  }
}

void putSecret(JsonObject obj, const char* key, const String& plaintext) {
  putSecretPreserving(obj, key, plaintext, String((const char*)(obj[key] | "")));
}

String getSecret(JsonObjectConst obj, const char* key) {
  const char* enc = obj[key] | "";
  return decryptString(String(enc));
}

// Standalone strict test: does `blob` decrypt cleanly under `keyMaterial`?
// Used by selectDeviceKeyEpoch to pick the key derivation that actually opens
// the stored secrets. Full PKCS#7 validation keeps the wrong-key false-accept
// rate negligible (~0.4% per blob, and candidates are majority-voted).
static bool aesBlobDecryptsWith(const String& keyMaterial, const char* blob) {
  size_t len = strlen(blob);
  if (len < 41 || strncmp(blob, "AES:", 4) != 0 || blob[36] != ':') return false;

  uint8_t key[16];
  uint8_t hash[32];
  mbedtls_sha256((const uint8_t*)keyMaterial.c_str(), keyMaterial.length(), hash, 0);
  memcpy(key, hash, 16);

  uint8_t iv[16];
  for (int i = 0; i < 16; i++) {
    char hx[3] = { blob[4 + i * 2], blob[5 + i * 2], '\0' };
    iv[i] = (uint8_t)strtol(hx, NULL, 16);
  }

  const char* ctHex = blob + 37;
  int ctLen = (int)strlen(ctHex) / 2;
  if (ctLen <= 0 || (ctLen % 16) != 0) return false;

  uint8_t* ct = (uint8_t*)ps_alloc(ctLen, AllocPref::PreferPSRAM, "aes.epoch.ct");
  uint8_t* pt = (uint8_t*)ps_alloc(ctLen, AllocPref::PreferPSRAM, "aes.epoch.pt");
  if (!ct || !pt) {
    if (ct) free(ct);
    if (pt) free(pt);
    return false;
  }
  for (int i = 0; i < ctLen; i++) {
    char hx[3] = { ctHex[i * 2], ctHex[i * 2 + 1], '\0' };
    ct[i] = (uint8_t)strtol(hx, NULL, 16);
  }

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, key, 128);
  int ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, ctLen, iv, ct, pt);
  mbedtls_aes_free(&aes);

  bool ok = false;
  if (ret == 0) {
    uint8_t pad = pt[ctLen - 1];
    if (pad >= 1 && pad <= 16) {
      ok = true;
      for (int i = ctLen - pad; i < ctLen; i++) {
        if (pt[i] != pad) { ok = false; break; }
      }
    }
  }
  memset(pt, 0, ctLen);
  free(ct);
  free(pt);
  return ok;
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
  // - output (System_Settings.cpp): outSerial
  // - i2c (System_I2C.cpp): bus settings, clock speeds
  // - thermal (i2csensor_mlx90640.cpp): autoStart, polling, interpolation, EWMA, rotation
  // - tof (i2csensor_vl53l4cx.cpp): autoStart, polling, stability, transition
  // - imu (i2csensor_bno055.cpp): autoStart, polling, EWMA, orientation correction
  // - gps (i2csensor_pa1010d.cpp): autoStart, polling
  // - apds (i2csensor_apds9960.cpp): autoStart, polling
  // - gamepad (i2csensor_seesaw.cpp): autoStart, polling
  // - fmradio (i2csensor_rda5807.cpp): autoStart, polling
  // - oled (OLED_Settings.cpp): enabled, autoInit, modes, brightness
  // - led (System_NeoPixel.cpp): brightness, startup effect/color/duration
  // - power (System_Power.cpp): mode, autoMode, thresholds
  // - bluetooth (Bluetooth.cpp): autoStart, requireAuth, deviceName
  // ============================================================================
  applyRegisteredDefaults();
}

// ============================================================================
// Apply Settings to Runtime Flags
// ============================================================================

// Settings→flag map — generated from DBG_FLAG_LIST (System_DebugFlags.h).
// Every row bearing a settingsField expands to { offsetof(Settings, field),
// DEBUG_<SYM> }, compiler-verified against the struct; the DBG_NO_SETTING
// sentinel (control bits) expands to nothing. Fields that raise a flag a
// prior row already carries live in DBG_FLAG_EXTRA_SETTINGS (debugAuthCookies
// also raises DEBUG_AUTH). Deliberately unconditional: feature-gated flags
// (LLM) keep their rows on every build — zero conditional compilation inside
// generated tables, so one green build proves the whole map.
struct DebugFlagMapping { size_t settingOffset; DebugFlagMask flag; };
// DEBUG_##SYM is pasted HERE, at the first macro level, before SYM is ever
// re-scanned as a plain argument — several row symbols (INPUT, ...) collide
// with Arduino core macros and would otherwise expand to their pin-mode
// values inside the deferred call.
#define DBG_MAP_ROW_0(constName, field) { offsetof(Settings, field), constName },
#define DBG_MAP_ROW_1(constName, field)
#define DBG_MAP(SYM, bit, BANK, parentBit, tag, field, ...) \
  DBG_PP_CAT(DBG_MAP_ROW_, DBG_SF_IS_NONE(field))(DEBUG_##SYM, field)
#define DBG_MAP_EXTRA(field, SYM) { offsetof(Settings, field), DEBUG_##SYM },
static constexpr DebugFlagMapping kDebugMappings[] = {
  DBG_FLAG_LIST(DBG_MAP)
  DBG_FLAG_EXTRA_SETTINGS(DBG_MAP_EXTRA)
};
#undef DBG_MAP_EXTRA
#undef DBG_MAP
#undef DBG_MAP_ROW_1
#undef DBG_MAP_ROW_0
static constexpr size_t kDebugMappingCount = sizeof(kDebugMappings) / sizeof(kDebugMappings[0]);

// 117 = 116 settings-bearing flag rows + 1 extra; the ALWAYS control row
// contributes nothing. Row-for-row the hand table this replaces.
static_assert(kDebugMappingCount == 117,
              "settings→flag map row count changed — reconcile the settingsField column and DBG_FLAG_EXTRA_SETTINGS");

// Two rows mapping one Settings field means a transposed settingsField
// column or a duplicated extras row — multiple fields per flag are fine,
// multiple rows per field are not.
static constexpr bool dbgMapFieldsDistinct() {
  for (size_t i = 0; i < kDebugMappingCount; ++i)
    for (size_t j = i + 1; j < kDebugMappingCount; ++j)
      if (kDebugMappings[i].settingOffset == kDebugMappings[j].settingOffset) return false;
  return true;
}
static_assert(dbgMapFieldsDistinct(), "two settings→flag map rows claim the same Settings field");

void applySettings() {
  DEBUG_SYSTEMF("[applySettings] START");

  // Make localtime_r()/mktime() honor the configured offset from this point
  // on. Must happen before anything formats or schedules a local time —
  // without it the C library defaults to UTC and only setupNTP()'s
  // configTime() (WiFi-only) would ever correct it. See Clock::applyTimezone.
  Clock::applyTimezone();

  // Apply the one persisted output lane (SERIAL). Every other lane is
  // runtime state and must survive applySettings re-runs (setup wizard,
  // first-time setup): WEB tracks the HTTP server lifecycle (raised in
  // startHttpServer, cleared by httpstop/closewifi), FILE opens and closes
  // with `log start`/`log stop`, BLE and G2 are per-session opt-in streams
  // (outble/outg2). OLED is deliberately absent: the OLED console is gated
  // by msg->routing only — see the System_Debug.h charter.
  gOutputFlags = (gOutputFlags & (MSG_ROUTE_WEB | MSG_ROUTE_FILE | MSG_ROUTE_BLE | MSG_ROUTE_G2))
               | (gSettings.outSerial ? MSG_ROUTE_SERIAL : 0);

  // Apply debug settings to runtime flags using the generated map above.
  // Multiple settings can map to the same flag (debugAuth and
  // debugAuthCookies both → DEBUG_AUTH); the loop only ever ORs bits in,
  // so row order is immaterial.
  setDebugFlags(0);  // Start with no flags, then enable based on settings
  const uint8_t* base = reinterpret_cast<const uint8_t*>(&gSettings);
  for (const auto& m : kDebugMappings) {
    if (*reinterpret_cast<const bool*>(base + m.settingOffset)) {
      setDebugFlag(m.flag);
    }
  }

  // Mirror every bitless sub-flag from the persistent layer (gSettings) into
  // the runtime layer (gDebugSubFlags). The layers are distinct on purpose —
  // `temp` toggles write only the runtime member — and boot is the one point
  // where they must agree. Generated from DBG_SUBBOOL_LIST.
#define DBG_SUB_MIRROR(SYM, subField, settingsField, PARENT_SYM, ...) \
  gDebugSubFlags.subField = gSettings.settingsField;
  DBG_SUBBOOL_LIST(DBG_SUB_MIRROR)
#undef DBG_SUB_MIRROR

  // ESP-SR mirrors — bit-backed subs, deliberately NOT in DBG_SUBBOOL_LIST
  // (each has its own bit and map row). Mirrored so fast paths can read
  // gDebugSubFlags without re-touching gSettings on every audio frame.
  gDebugSubFlags.srWake      = gSettings.debugSrWake;
  gDebugSubFlags.srCommand   = gSettings.debugSrCommand;
  gDebugSubFlags.srAfe       = gSettings.debugSrAfe;
  gDebugSubFlags.srLifecycle = gSettings.debugSrLifecycle;
  gDebugSubFlags.srTuning    = gSettings.debugSrTuning;

  // Recompute every aggregated parent bit (DBG_AGG_FAMILY_LIST). The full
  // sweep is safe here and only here: the mask was just rebuilt from
  // gSettings, so no temp-set bits exist to clobber — and bit==setting for
  // every mapped row, which reduces the BT/SR runtime child-bit terms to the
  // settings-only expressions this block previously spelled out per family.
  dbgRecomputeAllParents();
#if ENABLE_ESP_SR
  // Sync the legacy integer level from the new bool flags so existing
  // SR_DBG_L / SR_INFO_L call sites in System_ESPSR keep producing output.
  srSyncDebugLevel();
#endif

  // Apply severity-based log level from settings
  {
    int lvl = gSettings.logLevel;
    if (lvl < LOG_LEVEL_ERROR) lvl = LOG_LEVEL_ERROR;
    if (lvl > LOG_LEVEL_DEBUG) lvl = LOG_LEVEL_DEBUG;
    DEBUG_MANAGER.setLogLevel((uint8_t)lvl);
    gSettings.logLevel = lvl;

    // Map app log level to ESP-IDF log level for noisy framework components
    // App: 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG
    // ESP: ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG
    esp_log_level_t espLevel;
    switch (lvl) {
      case LOG_LEVEL_ERROR: espLevel = ESP_LOG_ERROR; break;
      case LOG_LEVEL_WARN:  espLevel = ESP_LOG_WARN;  break;
      case LOG_LEVEL_INFO:  espLevel = ESP_LOG_INFO;  break;
      default:              espLevel = ESP_LOG_DEBUG; break;
    }

    // Suppress noisy ESP-IDF components based on log level.
    // NOTE: the TLS/HTTPS tags (esp-tls-mbedtls, esp_https_server, httpd,
    // httpd_txrx) are intentionally NOT set here — they are owned by the
    // DEBUG_HTTPS flag via applyHttpsLogLevels() below, so a high global
    // loglevel can't re-spam the benign browser-disconnect flood.
    // WiFi driver state changes and init parameters
    esp_log_level_set("wifi", espLevel);
    esp_log_level_set("wifi_init", espLevel);
    // PHY calibration warnings
    esp_log_level_set("phy_init", espLevel);
  }

  // TLS/HTTPS framework log verbosity is gated by DEBUG_HTTPS, independent of
  // the global log level (mbedtls logs disconnects at ERROR, which the global
  // level can't drop below). OFF (default) → ESP_LOG_NONE for those tags.
  applyHttpsLogLevels(gSettings.debugHttps);

  DEBUG_SYSTEMF("[applySettings] Applied debug flags");

  // Apply ESP-NOW mode from settings (directly to gEspNow if initialized)
#if ENABLE_ESPNOW
  if (gEspNow) {
    gEspNow->mode = gSettings.espnowmesh ? ESPNOW_MODE_MESH : ESPNOW_MODE_DIRECT;
  }
#endif

  // Push persisted Maps defaults into the live runtime variables.
#if ENABLE_MAPS
  extern void mapsApplyPersistedSettings();
  mapsApplyPersistedSettings();
#endif

  // Apply power mode from settings
  #include "System_Power.h"
  applyPowerMode(gSettings.powerMode);

  DEBUG_SYSTEMF("Settings applied (I2C hardware config skipped - requires sensor restart to apply)");
}

// ============================================================================
// Build Settings JSON Document
// ============================================================================

void buildSettingsJsonDoc(JsonDocument& doc, bool excludePasswords, bool mainFileOnly) {
  // Stamp firmware version so we know which build last wrote this file
  doc["firmwareVersion"] = SelfDevice::firmwareVersion();

  // NOTE: All top-level settings are now owned by their respective modules.
  // NOTE: webCliHistorySize/oledCliHistorySize -> cli module
  // NOTE: wifiEnabled and all wifi settings -> wifi module

  // wifiPrimarySSID: runtime-only convenience field for web UI (not saved, not a setting)
#if ENABLE_WIFI
  {
    String cur = WiFi.SSID();
    if (cur.length() > 0) {
      doc["wifiPrimarySSID"] = cur;
    }
  }
#endif

  // NOTE: ntpServer, tzOffsetMinutes, wifiAutoStart are owned by the wifi
  //       module (written under "wifi" section). The legacy single-network
  //       wifiSSID/wifiPassword fields are gone (removed 2026-07-20).
  // NOTE: automationEnabled is owned by the automation module.
  // NOTE: power{} is owned by the power module.

  // Debug flags are now handled by modular registry - no manual fallbacks needed
  // Output settings are now handled by modular registry - no manual fallbacks needed

  // Thermal settings now handled by modular registry
  // Write registered module settings here so section lands at the root,
  // before ToF/hardware/oled/wifiNetworks blocks
  {
    // mainFileOnly excludes own-file modules (debug) from settings.json; the
    // web/bond/G2 callers keep allFiles=true so their payloads still carry debug.
    size_t registeredCount = writeRegisteredSettings(doc, nullptr, /*allFiles=*/!mainFileOnly);
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
        
        // Walk dotted jsonSection path (e.g. "hardware.sensors.camera")
        // Use create-or-get so we get a mutable handle for .remove() below.
        JsonObject section = jsonPathCreate(doc, mod->jsonSection);
        if (section.isNull()) continue;

        for (size_t i = 0; i < mod->count; i++) {
          const SettingEntry* e = &mod->entries[i];
          if (e->isSecret && e->type == SETTING_STRING) {
            // Remove secret field from web API response
            JsonObject target = section;
            if (e->group) {
              target = target[e->group].as<JsonObject>();
              if (target.isNull()) continue;
            }
            target.remove(e->jsonKey);
          }
        }
      }
    }
  }

  // BLE peers — nested under bluetooth.peers; serialised by BLE_Peers
  // since the structure is one level deeper than the SettingsModule
  // registry handles. See BLE_Peers.h.
#if ENABLE_BLUETOOTH
  blePeersWriteJson(doc);
#endif

  // ESP-NOW meshes — array-of-struct, serialised by System_ESPNow for the
  // same reason as BLE peers. Persists gSettings.meshes[] and the per-mesh
  // bond arrays under doc["espnow"]["meshes"] / doc["espnow"]["bondsByMesh"].
#if ENABLE_ESPNOW
  espnowMeshesWriteJson(doc, excludePasswords);
#endif

  // WiFi networks array - now nested under network.wifi.networks
  //
  // Rebuild from RAM whenever the array exists AND this boot's settings load
  // completed (or there was no file to load). The old `gWifiNetworkCount > 0`
  // gate skipped the rebuild when the LAST network was removed, so the
  // merge-read's stale entries were serialized straight back — wifirm of the
  // final network never persisted and it resurrected on reboot (the
  // SETTINGS_LIFECYCLE_AUDIT "empty-list resurrect", observed live 2026-07-20).
  // gSettingsLoadedOk keeps the one thing that gate accidentally protected:
  // a save issued after a FAILED load cannot wipe the on-disk list. The
  // null-check stays for ENABLE_WIFI=0 builds, where gWifiNetworks is never
  // allocated (a failed alloc halts boot, so null here can't mean that).
  if (gWifiNetworks && gSettingsLoadedOk) {
    // Capture the merge-read's on-disk password blobs BEFORE to<JsonArray>()
    // clears the array, so the guarded write below can keep a still-recoverable
    // blob when the RAM password is empty after a damaged load.
    String oldSsid[MAX_WIFI_NETWORKS];
    String oldBlob[MAX_WIFI_NETWORKS];
    int oldCount = 0;
    JsonArrayConst oldNets = doc["network"]["wifi"]["networks"].as<JsonArrayConst>();
    for (JsonObjectConst oldNet : oldNets) {
      if (oldCount >= MAX_WIFI_NETWORKS) break;
      const char* pb = oldNet["password"] | "";
      if (strncmp(pb, "AES:", 4) == 0) {
        oldSsid[oldCount] = (const char*)(oldNet["ssid"] | "");
        oldBlob[oldCount] = pb;
        oldCount++;
      }
    }
    JsonArray networks = doc["network"]["wifi"]["networks"].to<JsonArray>();
    for (int i = 0; i < gWifiNetworkCount; i++) {
      JsonObject net = networks.add<JsonObject>();
      net["ssid"] = gWifiNetworks[i].ssid;
      
      // Security: Encrypt passwords in file, exclude from web API
      if (!excludePasswords) {
        String prevBlob;
        for (int k = 0; k < oldCount; k++) {
          if (oldSsid[k] == gWifiNetworks[i].ssid) { prevBlob = oldBlob[k]; break; }
        }
        putSecretPreserving(net, "password", gWifiNetworks[i].password, prevBlob);  // at-rest AES (device key)
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

  // Pause sensor polling during settings I/O. Kept as explicit pause/resume
  // (not the RAII guard) — this function has many exit paths that resume at
  // different points; mechanically swapping them would risk altering behavior.
  pollPause();

  DEBUG_STORAGEF("[Settings] Writing to file using ArduinoJson");

  // Build JSON document (5120 bytes for settings + encrypted WiFi passwords)
  PSRAM_JSON_DOC(doc);
  
  // CRITICAL: Read existing settings first to preserve orphaned sensor sections
  // This allows settings from disabled sensors to persist and show as grayed-out in UI
  if (VFS::existsGuarded(SETTINGS_JSON_FILE, VFS::systemAuth("settings.write"))) {
    fsLock("settings.read_for_merge");
    File existingFile = VFS::openGuarded(SETTINGS_JSON_FILE, "r", VFS::systemAuth("settings.write"));
    if (existingFile) {
      DeserializationError err = deserializeJson(doc, existingFile);
      existingFile.close();
      if (err) {
        WARN_STORAGEF("Failed to read existing settings for merge: %s", err.c_str());
        logSystemEvent("SETTINGS", "merge-read of existing file failed (%s) — unregistered sections dropped from this save",
                       err.c_str());
        doc.clear();  // Start fresh if parse failed
      } else {
        INFO_STORAGEF("Loaded existing settings for merge (preserving orphaned sections)");
      }
    }
    fsUnlock();
  }
  
  // Now build/overwrite with current settings (orphaned sections remain untouched).
  // mainFileOnly=true → debug is excluded (it persists in DEBUG_JSON_FILE now).
  buildSettingsJsonDoc(doc, /*excludePasswords=*/false, /*mainFileOnly=*/true);

  // Remove runtime-only fields that must never be persisted to disk
  doc.remove("wifiPrimarySSID");

  // Debug now lives in DEBUG_JSON_FILE. Strip any stale system.debug block the
  // merge-read carried in from a pre-split settings.json so it can't linger here
  // or burn the 5120-byte budget. No-op once the on-disk file is already clean.
  { JsonObject sysObj = doc["system"].as<JsonObject>(); if (!sysObj.isNull()) sysObj.remove("debug"); }

  // Check for overflow
  if (doc.overflowed()) {
    ERROR_STORAGEF("JSON document overflowed during build (need more than 5120 bytes)");
    logSystemEvent("SETTINGS", "save FAILED (JSON build overflow) — settings NOT persisted");
    systemEventPost(SYSEVT_SETTINGS_SAVE_FAILED, "overflow", "settings.json");
    pollResume();
    return false;
  }

  // Atomic write: temp file then rename
  const char* tmp = "/settings.tmp";
  
  fsLock("settings.write");
  File file = VFS::openGuarded(tmp, "w", VFS::systemAuth("settings.write"));
  if (!file) {
    fsUnlock();
    ERROR_STORAGEF("Failed to open temp file for writing");
    logSystemEvent("SETTINGS", "save FAILED (cannot open %s) — settings NOT persisted", tmp);
    systemEventPost(SYSEVT_SETTINGS_SAVE_FAILED, "open", "settings.json");
    pollResume();
    return false;
  }

  // Serialize JSON directly to file (no intermediate buffer)
  size_t bytesWritten = serializeJson(doc, file);
  file.flush();
  file.close();
  fsUnlock();

  if (bytesWritten == 0) {
    ERROR_STORAGEF("Failed to serialize JSON");
    logSystemEvent("SETTINGS", "save FAILED (serialize wrote 0 bytes) — settings NOT persisted");
    systemEventPost(SYSEVT_SETTINGS_SAVE_FAILED, "serialize", "settings.json");
    VFS::removeGuarded(tmp, VFS::systemAuth("settings.write"));
    pollResume();
    return false;
  }

  DEBUG_STORAGEF("[Settings] Wrote %zu bytes to temp file", bytesWritten);

  // Atomic rename (LittleFS rename overwrites destination)
  fsLock("settings.rename");
  bool okRename = VFS::renameGuarded(tmp, SETTINGS_JSON_FILE, VFS::systemAuth("settings.write"));
  fsUnlock();

  if (!okRename) {
    WARN_STORAGEF("Rename failed, trying direct write");
    logSystemEvent("SETTINGS", "atomic rename failed — falling back to direct rewrite of %s", SETTINGS_JSON_FILE);
    // Fallback: write directly
    fsLock("settings.direct");
    File directFile = VFS::openGuarded(SETTINGS_JSON_FILE, "w", VFS::systemAuth("settings.write"));
    if (!directFile) {
      fsUnlock();
      logSystemEvent("SETTINGS", "save FAILED (rename and direct open both failed) — settings NOT persisted");
      systemEventPost(SYSEVT_SETTINGS_SAVE_FAILED, "rename", "settings.json");
      pollResume();
      return false;
    }
    serializeJson(doc, directFile);
    directFile.flush();
    directFile.close();
    fsUnlock();
  }

  DEBUG_STORAGEF("[Settings] Write complete");
  pollResume();
  
  // Recompute local settings hash so bond heartbeats reflect the change
#if ENABLE_ESPNOW && ENABLE_BONDED_MODE
  { extern void computeBondLocalSettingsHash(); computeBondLocalSettingsHash(); }
#endif

  return true;
}

// ============================================================================
// Debug settings file (DEBUG_JSON_FILE = /system/debug.json)
// ============================================================================
// Debug flags were split out of settings.json: 157 flags were ~half of the
// shared 5120-byte settings doc and every toggle rewrote the whole file. They
// persist here instead. The debug module keeps jsonSection "system.debug", so
// the in-RAM buildSettingsJsonDoc(scope=ALL) doc feeding /api/settings, the bond
// mirror, and G2 is UNCHANGED — only the on-disk file layout differs. This file
// has no secrets and no cross-section invariants, so it needs no merge-read and
// no crypto: the debug module fully owns it.

static void buildDebugJsonDoc(JsonDocument& doc) {
  doc["firmwareVersion"] = SelfDevice::firmwareVersion();
  registerAllSettingsModules();  // idempotent; safe if this runs before the main load
  writeRegisteredSettings(doc, DEBUG_JSON_FILE, /*allFiles=*/false);  // debug module only
}

bool writeDebugJson() {
  if (!filesystemReady) return false;
  pollPause();

  PSRAM_JSON_DOC(doc);
  buildDebugJsonDoc(doc);

  if (doc.overflowed()) {
    ERROR_STORAGEF("Debug JSON overflowed during build (need more than 5120 bytes)");
    logSystemEvent("SETTINGS", "debug save FAILED (JSON build overflow) — debug flags NOT persisted");
    pollResume();
    return false;
  }

  // Atomic write: temp at root (swept by the boot .tmp cleanup) then rename.
  const char* tmp = "/debug.tmp";
  fsLock("debug.write");
  File file = VFS::openGuarded(tmp, "w", VFS::systemAuth("settings.write"));
  if (!file) {
    fsUnlock();
    ERROR_STORAGEF("Failed to open temp file for debug write");
    pollResume();
    return false;
  }
  size_t bytesWritten = serializeJson(doc, file);
  file.flush();
  file.close();
  fsUnlock();

  if (bytesWritten == 0) {
    ERROR_STORAGEF("Failed to serialize debug JSON");
    VFS::removeGuarded(tmp, VFS::systemAuth("settings.write"));
    pollResume();
    return false;
  }

  fsLock("debug.rename");
  bool okRename = VFS::renameGuarded(tmp, DEBUG_JSON_FILE, VFS::systemAuth("settings.write"));
  fsUnlock();

  if (!okRename) {
    WARN_STORAGEF("Debug rename failed, trying direct write");
    fsLock("debug.direct");
    File directFile = VFS::openGuarded(DEBUG_JSON_FILE, "w", VFS::systemAuth("settings.write"));
    if (!directFile) {
      fsUnlock();
      logSystemEvent("SETTINGS", "debug save FAILED (rename and direct open both failed)");
      pollResume();
      return false;
    }
    serializeJson(doc, directFile);
    directFile.flush();
    directFile.close();
    fsUnlock();
  }

  pollResume();

  // A debug flag is a user-visible setting, so keep the bond dirty-hash in sync
  // (writeSettingsJson does the same). The hash content still includes debug via
  // buildSettingsJsonDoc(scope=ALL); this just re-fires the recompute trigger.
#if ENABLE_ESPNOW && ENABLE_BONDED_MODE
  { extern void computeBondLocalSettingsHash(); computeBondLocalSettingsHash(); }
#endif
  return true;
}

bool readDebugJson() {
  if (!filesystemReady) return false;
  if (!VFS::existsGuarded(DEBUG_JSON_FILE, VFS::systemAuth("settings.read"))) {
    return false;  // absent → debug stays on the defaults applied by settingsDefaults()
  }
  pollPause();

  File file = VFS::openGuarded(DEBUG_JSON_FILE, "r", VFS::systemAuth("settings.read"));
  if (!file) {
    ERROR_STORAGEF("Failed to open debug settings file");
    pollResume();
    return false;
  }
  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    ERROR_STORAGEF("Debug JSON parse error: %s", error.c_str());
    pollResume();
    return false;
  }

  registerAllSettingsModules();  // idempotent; safe if this read runs standalone
  size_t n = readRegisteredSettings(doc, DEBUG_JSON_FILE, /*allFiles=*/false);
  DEBUG_STORAGEF("[Settings] Applied %zu debug settings from debug.json", n);

  pollResume();
  return true;
}

// ============================================================================
// Device-key epoch selection (boot self-healing)
// ============================================================================
// The at-rest key is derived from eFuse MAC + the flash chip unique ID
// (esp_flash_read_unique_chip_id). That read can transiently fail (-> UID 0)
// or change representation across IDF/toolchain versions, silently re-keying
// the device on a reflash: every stored secret then decrypts to ""/garbage,
// logins break (PBKDF2 salt is this same key), and the next save clobbers the
// still-recoverable blobs. Before anything decrypts, test the stored blobs
// against every plausible derivation and adopt the one that actually opens
// them. The outcome is logged durably (system-events.log) on every boot.

static void collectAesBlobs(JsonVariantConst v, String* out, int cap, int& n) {
  if (n >= cap) return;
  if (v.is<JsonObjectConst>()) {
    for (JsonPairConst kv : v.as<JsonObjectConst>()) collectAesBlobs(kv.value(), out, cap, n);
  } else if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst e : v.as<JsonArrayConst>()) collectAesBlobs(e, out, cap, n);
  } else if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (s && strncmp(s, "AES:", 4) == 0 && n < cap) out[n++] = s;
  }
}

static void selectDeviceKeyEpoch(const JsonDocument& doc) {
  gSecretLoadFailures = 0;

  String blobs[8];
  int nBlobs = 0;
  collectAesBlobs(doc.as<JsonVariantConst>(), blobs, 8, nBlobs);

  uint64_t chipId = ESP.getEfuseMac();
  uint64_t flashUid = 0;
  bool uidOk = (esp_flash_read_unique_chip_id(NULL, &flashUid) == ESP_OK);
  if (!uidOk) flashUid = 0;

  String cand[3];
  const char* candName[3];
  int nCand = 0;
  cand[nCand] = deriveDeviceKeyFromIds(chipId, flashUid);
  candName[nCand++] = uidOk ? "mac+flashUID" : "mac+0 (flashUID read FAILED)";
  if (uidOk && flashUid != 0) {
    cand[nCand] = deriveDeviceKeyFromIds(chipId, 0);
    candName[nCand++] = "mac+0 (legacy/read-failed epoch)";
    cand[nCand] = deriveDeviceKeyFromIds(chipId, __builtin_bswap64(flashUid));
    candName[nCand++] = "mac+bswap(flashUID) (cross-IDF epoch)";
  }

  int okCount[3] = {0, 0, 0};
  int best = 0;
  for (int c = 0; c < nCand; c++) {
    for (int b = 0; b < nBlobs; b++) {
      if (aesBlobDecryptsWith(cand[c], blobs[b].c_str())) okCount[c]++;
    }
    if (okCount[c] > okCount[best]) best = c;
  }

  sDeviceKey = cand[best];
  sDeviceKeyInit = true;

  if (nBlobs == 0) {
    logSystemEvent("CRYPTO", "device key: %s (fp %s) — no stored secrets to validate against",
                   candName[best], deviceKeyFingerprint(sDeviceKey).c_str());
  } else {
    logSystemEvent("CRYPTO", "device key: %s (fp %s) opens %d/%d stored secrets%s",
                   candName[best], deviceKeyFingerprint(sDeviceKey).c_str(),
                   okCount[best], nBlobs,
                   okCount[best] == nBlobs ? "" : " — UNREADABLE SECRETS PRESERVED, not clobbered");
  }
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

  // Pause sensor polling during settings I/O. Kept as explicit pause/resume
  // (not the RAII guard) — this function has many exit paths that resume at
  // different points; mechanically swapping them would risk altering behavior.
  pollPause();

  if (!VFS::existsGuarded(SETTINGS_JSON_FILE, VFS::systemAuth("settings.read"))) {
    DEBUG_STORAGEF("[Settings] File does not exist: %s", SETTINGS_JSON_FILE);
    pollResume();
    return false;
  }

  // Open file and parse JSON directly (no intermediate String)
  File file = VFS::openGuarded(SETTINGS_JSON_FILE, "r", VFS::systemAuth("settings.read"));
  if (!file) {
    ERROR_STORAGEF("Failed to open settings file");
    pollResume();
    return false;
  }

  // Use JsonDocument for parsing settings JSON
  // Size calculated: ~2.5KB base settings + ~2KB for WiFi networks with encrypted strings
  // Encrypted strings are ~2x longer than plaintext (hex encoding + "AES:" prefix)
  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    ERROR_STORAGEF("JSON parse error: %s", error.c_str());
    if (error == DeserializationError::NoMemory) {
      ERROR_STORAGEF("JSON document too small (need more than 5120 bytes)");
    }
    pollResume();
    return false;
  }

  // Check for overflow
  if (doc.overflowed()) {
    DEBUG_STORAGEF("[Settings] WARNING: JSON document overflowed during parsing");
  }

  DEBUG_STORAGEF("[Settings] JSON parsed successfully, applying settings");

  // Check if settings were written by a different firmware version
  const char* savedVersion = doc["firmwareVersion"] | "";
  const char* runningVersion = SelfDevice::firmwareVersion();
  if (savedVersion[0] == '\0') {
    INFO_STORAGEF("[Settings] No firmwareVersion in settings file (pre-versioning build)");
  } else if (strcmp(savedVersion, runningVersion) != 0) {
    INFO_STORAGEF("[Settings] Settings written by v%s, running v%s", savedVersion, runningVersion);
    logSystemEvent("BOOT", "settings last saved by fw v%s (now running v%s)", savedVersion, runningVersion);
    { char verBuf[48]; snprintf(verBuf, sizeof(verBuf), "%s->%s", savedVersion, runningVersion); systemEventPost(SYSEVT_FIRMWARE_CHANGED, verBuf, "settings carried over from prior firmware"); }
  }

  registerAllSettingsModules();

  // BLE peers — read before registered-modules call to avoid races; the
  // peer modules will see the data populated when they bleRegisterPeer
  // during init.
#if ENABLE_BLUETOOTH
  blePeersReadJson(doc);
#endif

  // ESP-NOW meshes — array-of-struct, deserialised by System_ESPNow.
  // Reads gSettings.meshes[] and per-mesh bond arrays. fingerprint is
  // recomputed from the label inside the read function.
#if ENABLE_ESPNOW
  espnowMeshesReadJson(doc);
#endif

  // Pick the key epoch that actually opens the stored blobs BEFORE any decrypt
  selectDeviceKeyEpoch(doc);

  // Apply settings from registered modules first (handles defaults automatically).
  // Main-file modules only — debug loads separately from DEBUG_JSON_FILE.
  size_t registeredCount = readRegisteredSettings(doc, nullptr, /*allFiles=*/false);
  if (registeredCount > 0) {
    DEBUG_STORAGEF("[Settings] Applied %zu settings from registered modules", registeredCount);
  }

  // WiFi networks array
#if ENABLE_WIFI
  JsonArray networks = doc["network"]["wifi"]["networks"];
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
        gWifiNetworks[gWifiNetworkCount].password = getSecret(net, "password");
        if (strncmp(password, "AES:", 4) == 0 && gWifiNetworks[gWifiNetworkCount].password.length() == 0) {
          gSecretLoadFailures++;
          logSystemEvent("CRYPTO", "wifi password for '%s' failed to decrypt at load — stored blob preserved", ssid);
          { char sBuf[48]; snprintf(sBuf, sizeof(sBuf), "wifi:%s", ssid); systemEventPost(SYSEVT_SECRET_DECRYPT_FAILED, sBuf, "stored blob preserved"); }
        }
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
  gSettingsLoadedOk = true;  // full-array rewrites (wifi networks) now trusted
  pollResume();
  return true;
}

// ============================================================================
// Settings command implementations (migrated from .ino)
// ============================================================================

extern void setupNTP();

const char* cmd_tzoffsetminutes(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: tzoffsetminutes <-720..840>";
  int offset = atoi(valStr.c_str());
  // -720..+840 = UTC-12 .. UTC+14, the real-world timezone span (e.g. Kiribati
  // Line Islands at +14). Matches the tzOffsetMinutes setting's min/max.
  if (offset < -720 || offset > 840) return "Error: timezone offset must be between -720 and 840 minutes";
  setSetting(gSettings.tzOffsetMinutes, offset);
  Clock::applyTimezone();  // takes effect with or without WiFi
#if ENABLE_WIFI
  setupNTP();
#endif
  snprintf(getDebugBuffer(), 1024, "Timezone offset set to %d minutes", offset);
  return getDebugBuffer();
}

#if ENABLE_WIFI
const char* cmd_ntpserver(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: ntpserver <host>";
  const char* p = valStr.c_str();

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
const char* cmd_ntpserver(const String& argsInput) {
  return "Error: NTP server command requires WiFi to be enabled";
}
#endif // ENABLE_WIFI

const char* cmd_espnowenabled(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: espnowenabled <0|1>";
  const char* p = valStr.c_str();
  bool enabled = (*p == '1' || strncasecmp(p, "true", 4) == 0);
  setSetting(gSettings.espnowEnabled, enabled);
  if (!enabled) {
#if ENABLE_ESPNOW
    (void)executeCommandThroughRegistry("closeespnow");
#endif
    snprintf(getDebugBuffer(), 1024, "espnowenabled set to 0 (ESP-NOW stopped)");
  } else {
    snprintf(getDebugBuffer(), 1024, "espnowenabled set to 1 (use openespnow or reboot with autostart)");
  }
  return getDebugBuffer();
}

#if ENABLE_HTTP_SERVER
const char* cmd_httpAutoStart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: httpAutoStart <0|1>";
  const char* p = valStr.c_str();
  bool enabled = (*p == '1' || strncasecmp(p, "true", 4) == 0);
  setSetting(gSettings.httpAutoStart, enabled);
  snprintf(getDebugBuffer(), 1024, "httpAutoStart set to %s", enabled ? "1" : "0");
  return getDebugBuffer();
}
#endif

#if ENABLE_HTTPS
const char* cmd_httpsEnabled(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: httpsEnabled <0|1>";
  const char* p = valStr.c_str();
  bool enabled = (*p == '1' || strncasecmp(p, "true", 4) == 0);
  setSetting(gSettings.httpsEnabled, enabled);
  snprintf(getDebugBuffer(), 1024, "httpsEnabled set to %s (takes effect after reboot)", enabled ? "1" : "0");
  return getDebugBuffer();
}
#endif

// ============================================================================
// Modular Settings Registry Implementation
// ============================================================================

// ============================================================================
// Debug Settings Module (for modular settings registry)
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
//
// Generated from the X-macro tables in System_DebugFlags.h (C2). Two row
// pools hold one SettingEntry per settings-backed table row, in TABLE order
// (indexed by DbgFlagIdx / DbgSubIdx); debugSettingEntries then PICKS rows in
// the pre-C2 hand-table order, which is a UI property in its own right
// (esp-now lists stream first, automations scheduler-first, sensor groups sit
// in card order — none of it derivable from bit order, so the order lives in
// the pick list, not the tables). The three rows that are not flag/sub-backed
// (memorysampleintervalsec, webconsole, loglevel) stay hand-written, verbatim,
// in their historical positions.
//
// Registry strings (group, jsonKey, label, cmdKey) are VERBATIM transcriptions
// carried in the table columns — never computed from symbols (NTP ↔ group
// "datetime", DISPLAY ↔ group "oled"); renaming one renames persisted
// debug.json keys.
//
// Deliberately unconditional (zero conditional compilation inside generated
// tables): the former ENABLE_ONDEVICE_LLM and ENABLE_BLUETOOTH+ENABLE_G2_GLASSES
// gates around the llm and g2 groups are gone, so gated-off builds carry their
// 13 keys as inert-but-present settings — plan-sanctioned
// (docs/DEBUG_FLAG_XMACRO_PLAN.md §3), and one green build proves the whole
// registry.
//
// intDefault is the constant 0 for every generated row — the C2 defaults audit
// found registry-0 is the effective persisted default for all 156 bools
// (applyRegisteredDefaults() stomps every field with the registry default at
// each boot before debug.json loads). The one dissenter, debugStoragePermissions'
// NSDMI {true} (System_Settings.h), already loses to the registry every boot
// and keeps losing — resolved in favor of registry-0. kBootDefaultDebugFlags
// (System_Debug.cpp) encodes a SECOND, intentionally different early-boot
// default policy for 29 flags and therefore stays hand-written — no dflt
// column exists.

// Row pools — every DBG_FLAG_LIST row bearing a settingsField and every
// DBG_SUBBOOL_LIST row becomes one SettingEntry; the ALWAYS control row
// (DBG_NO_SETTING) leaves a zeroed placeholder that nothing picks. The C1
// paste rule holds: cmdIdent is stringized and the string columns consumed at
// the FIRST macro level — the only non-string token reaching the deferred
// call is the settings field name (safe: same exposure as DBG_MAP above).
#define DBG_REG_ROW_0(field, cmdStr, group, jsonKey, label) \
  { jsonKey, SETTING_BOOL, &gSettings.field, 0, 0, nullptr, 0, 1, label, nullptr, false, group, cmdStr },
#define DBG_REG_ROW_1(field, cmdStr, group, jsonKey, label) {},
#define DBG_REG(SYM, bit, BANK, parentBit, tag, field, cmdIdent, group, jsonKey, label) \
  DBG_PP_CAT(DBG_REG_ROW_, DBG_SF_IS_NONE(field))(field, #cmdIdent, group, jsonKey, label)
static constexpr SettingEntry kDbgFlagRegEntry[DBG_FLAG_COUNT] = {
  DBG_FLAG_LIST(DBG_REG)
};
#undef DBG_REG
#undef DBG_REG_ROW_1
#undef DBG_REG_ROW_0

#define DBG_REG_SUB(SYM, subField, field, PARENT_SYM, cmdIdent, group, jsonKey, label) \
  { jsonKey, SETTING_BOOL, &gSettings.field, 0, 0, nullptr, 0, 1, label, nullptr, false, group, #cmdIdent },
static constexpr SettingEntry kDbgSubRegEntry[DBG_SUBBOOL_COUNT] = {
  DBG_SUBBOOL_LIST(DBG_REG_SUB)
};
#undef DBG_REG_SUB

// Pickers — SYM is pasted into its dense index at the FIRST macro level (the
// C1 lesson: row symbols like INPUT collide with core macros if a bare SYM is
// ever re-scanned inside a deferred call).
#define DBG_ROW(SYM)    kDbgFlagRegEntry[DBG_##SYM]
#define DBG_SUBROW(SYM) kDbgSubRegEntry[DBG_SUB_##SYM]

static constexpr SettingEntry debugSettingEntries[] = {
  // --- authentication group ---
  DBG_ROW(AUTH),
  DBG_SUBROW(AUTH_SESSIONS),
  DBG_SUBROW(AUTH_COOKIES),
  DBG_SUBROW(AUTH_LOGIN),
  DBG_SUBROW(AUTH_BOOTID),
  // --- http group ---
  DBG_ROW(HTTP),
  DBG_SUBROW(HTTP_HANDLERS),
  DBG_SUBROW(HTTP_REQUESTS),
  DBG_SUBROW(HTTP_RESPONSES),
  DBG_SUBROW(HTTP_STREAMING),
  // --- https group (TLS handshake + connection-error noise from ESP-IDF) ---
  DBG_ROW(HTTPS),
  // --- sse group ---
  DBG_ROW(SSE),
  DBG_SUBROW(SSE_CONNECTION),
  DBG_SUBROW(SSE_EVENTS),
  DBG_SUBROW(SSE_BROADCAST),
  // --- wifi group ---
  DBG_ROW(WIFI),
  DBG_SUBROW(WIFI_CONNECTION),
  DBG_SUBROW(WIFI_CONFIG),
  DBG_SUBROW(WIFI_SCANNING),
  DBG_SUBROW(WIFI_DRIVER),
  // --- storage group ---
  DBG_ROW(STORAGE),
  DBG_SUBROW(STORAGE_FILES),
  DBG_SUBROW(STORAGE_JSON),
  DBG_SUBROW(STORAGE_SETTINGS),
  DBG_SUBROW(STORAGE_MIGRATION),
  DBG_SUBROW(STORAGE_PERMISSIONS),
  // --- esp-now group ---
  DBG_ROW(ESPNOW_STREAM),
  DBG_ROW(ESPNOW_CORE),
  DBG_ROW(ESPNOW_ROUTER),
  DBG_ROW(ESPNOW_MESH),
  DBG_ROW(ESPNOW_TOPO),
  DBG_ROW(ESPNOW_METADATA),
  // --- bluetooth group ---
  DBG_ROW(BLUETOOTH),
  DBG_ROW(BLUETOOTH_CORE),
  DBG_ROW(BLUETOOTH_GATT),
  DBG_ROW(BLUETOOTH_DATA),
  // --- system group ---
  DBG_ROW(SYSTEM),
  DBG_SUBROW(SYSTEM_BOOT),
  DBG_SUBROW(SYSTEM_CONFIG),
  DBG_SUBROW(SYSTEM_TASKS),
  DBG_SUBROW(SYSTEM_HARDWARE),
  // --- users group ---
  DBG_ROW(USERS),
  DBG_SUBROW(USERS_MGMT),
  DBG_SUBROW(USERS_REGISTER),
  DBG_SUBROW(USERS_QUERY),
  // --- cli group ---
  DBG_ROW(CLI),
  DBG_SUBROW(CLI_EXECUTION),
  DBG_SUBROW(CLI_QUEUE),
  DBG_SUBROW(CLI_VALIDATION),
  // --- commands group (merged command-flow + command system) ---
  DBG_ROW(CMD_FLOW),
  DBG_ROW(COMMAND_SYSTEM),
  DBG_SUBROW(CMDFLOW_ROUTING),
  DBG_SUBROW(CMDFLOW_QUEUE),
  DBG_SUBROW(CMDFLOW_CONTEXT),
  // --- performance group ---
  DBG_ROW(PERFORMANCE),
  DBG_SUBROW(PERF_STACK),
  DBG_SUBROW(PERF_HEAP),
  DBG_SUBROW(PERF_TIMING),
  // --- automations group ---
  DBG_ROW(AUTOMATIONS),
  DBG_ROW(AUTO_SCHEDULER),
  DBG_ROW(AUTO_EXEC),
  DBG_ROW(AUTO_CONDITION),
  DBG_ROW(AUTO_TIMING),
  // --- per-sensor groups (each sensor gets its own card in the debug UI) ---
  DBG_ROW(CAMERA),
  DBG_ROW(CAMERA_LIFECYCLE),
  DBG_ROW(CAMERA_CAPTURE),
  DBG_ROW(CAMERA_SETTINGS),
  DBG_ROW(CAMERA_VIDEO),
  // DEBUG_DISPLAY now catches every OLED-internal log line (~106 callsites
  // across OLED_Utils.cpp): keyboard input, mode transitions + entry hooks,
  // menu construction, gamepad input handling on OLED, render dispatch, I2C
  // discovery + probe results, boot animation sequence. The earlier era when
  // OLED state events were misclassified through DEBUG_USERSF is gone (see
  // commit message for the migration).
  //
  // What this flag does NOT catch: events that *happen via* OLED but belong
  // to another subsystem semantically (login = USERS, settings save = SYSTEM,
  // map rendering = MAPS). Those still log through their owner flag — grep
  // `[CMD] *@oled:` in command-audit.log if you want every OLED-triggered
  // command regardless of the underlying event type.
  DBG_ROW(DISPLAY),
  DBG_ROW(NOTIFICATIONS),
  // --- microphone group ---
  DBG_ROW(MICROPHONE),
  DBG_ROW(MIC_LIFECYCLE),
  DBG_ROW(MIC_POLLING),
  DBG_ROW(MIC_VALUES),
  // --- gps group ---
  DBG_ROW(GPS),
  DBG_ROW(GPS_LIFECYCLE),
  DBG_ROW(GPS_POLLING),
  DBG_ROW(GPS_VALUES),
  // --- rtc group ---
  DBG_ROW(RTC),
  DBG_ROW(RTC_LIFECYCLE),
  DBG_ROW(RTC_POLLING),
  DBG_ROW(RTC_VALUES),
  // --- presence group ---
  DBG_ROW(PRESENCE),
  DBG_ROW(PRESENCE_LIFECYCLE),
  DBG_ROW(PRESENCE_POLLING),
  DBG_ROW(PRESENCE_VALUES),
  // --- fm radio group ---
  DBG_ROW(FMRADIO),
  DBG_ROW(FMRADIO_LIFECYCLE),
  DBG_ROW(FMRADIO_POLLING),
  DBG_ROW(FMRADIO_VALUES),
  // --- thermal group ---
  DBG_ROW(THERMAL),
  DBG_ROW(THERMAL_LIFECYCLE),
  DBG_ROW(THERMAL_POLLING),
  DBG_ROW(THERMAL_VALUES),
  // --- imu group ---
  DBG_ROW(IMU),
  DBG_ROW(IMU_LIFECYCLE),
  DBG_ROW(IMU_POLLING),
  DBG_ROW(IMU_VALUES),
  // --- input abstraction group (HAL_Input + OLED input dispatch) ---
  DBG_ROW(INPUT),
  DBG_ROW(INPUT_LIFECYCLE),
  DBG_ROW(INPUT_POLLING),
  DBG_ROW(INPUT_VALUES),
  // --- ANO encoder driver-specific group ---
  DBG_ROW(ANO_ENCODER),
  DBG_ROW(ANO_ENCODER_LIFECYCLE),
  DBG_ROW(ANO_ENCODER_POLLING),
  DBG_ROW(ANO_ENCODER_VALUES),
  // --- tof group ---
  DBG_ROW(TOF),
  DBG_ROW(TOF_LIFECYCLE),
  DBG_ROW(TOF_POLLING),
  DBG_ROW(TOF_VALUES),
  // --- apds group ---
  DBG_ROW(APDS),
  DBG_ROW(APDS_LIFECYCLE),
  DBG_ROW(APDS_POLLING),
  DBG_ROW(APDS_VALUES),
  // --- maps group ---
  DBG_ROW(MAPS),
  DBG_ROW(MAPS_LOADING),
  DBG_ROW(MAPS_RENDERING),
  DBG_ROW(MAPS_PERF),
  // --- llm group (on-device LLM) ---
  DBG_ROW(LLM),
  DBG_ROW(LLM_LOAD),
  DBG_ROW(LLM_TOKENIZER),
  DBG_ROW(LLM_FORWARD),
  DBG_ROW(LLM_GENERATE),
  DBG_ROW(LLM_MEMORY),
  // --- NTP / DateTime group ---
  DBG_ROW(NTP),
  DBG_SUBROW(NTP_SYNC),
  DBG_SUBROW(NTP_SETUP),
  DBG_SUBROW(NTP_ANCHOR),
  DBG_SUBROW(NTP_RESOLVE),
  // --- standalone (no group) ---
  DBG_ROW(LOGGER),
  DBG_ROW(MEMORY),
  DBG_ROW(MEMORY_HEAP),
  DBG_ROW(MEMORY_STACK),
  DBG_ROW(MEMORY_BUFFERS),
  { "sampleIntervalSec", SETTING_INT, &gSettings.memorySampleIntervalSec, 30, 0, nullptr, 0, 300, "Sample Interval (sec)", nullptr, false, "memory", "memorysampleintervalsec" },
  // --- g2 group (Even Realities G2 glasses) ---
  DBG_ROW(G2),
  DBG_ROW(G2_LIFECYCLE),
  DBG_ROW(G2_PROTOCOL),
  DBG_ROW(G2_EVENTS),
  DBG_ROW(G2_PAGES),
  DBG_ROW(G2_HEARTBEAT),
  DBG_ROW(G2_DUMP),
  // --- espsr group (ESP-SR speech recognition) ---
  DBG_ROW(SR),
  DBG_ROW(SR_WAKE),
  DBG_ROW(SR_COMMAND),
  DBG_ROW(SR_AFE),
  DBG_ROW(SR_LIFECYCLE),
  DBG_ROW(SR_TUNING),
  DBG_ROW(I2C),
  DBG_ROW(I2C_BUS),
  DBG_ROW(I2C_DISCOVERY),
  DBG_ROW(I2C_AUTOSTART),
  DBG_ROW(MQTT),
  DBG_ROW(MQTT_CONNECTION),
  DBG_ROW(MQTT_PUBSUB),
  DBG_ROW(MQTT_DISCOVERY),
  DBG_ROW(MQTT_COMMANDS),
  // --- page group: developer-facing toggles for the served web pages ---
  // webConsole was previously in the output module's "channels" group, which
  // mislabelled it as a fourth output destination. It's actually just a flag
  // that controls whether the page's own JS console.log/warn/debug calls are
  // suppressed via a <script> injection (WebServer_Utils.cpp:515). Default
  // OFF = suppress; ON = allow normal JS console output. Does NOT route
  // firmware broadcastOutput anywhere — that's plumbed via MSG_ROUTE_*.
  { "webConsole", SETTING_BOOL, &gSettings.webConsoleDebug,     0, 0, nullptr, 0, 1, "Allow page console.log", nullptr, false, "page", "webconsole" },
  { "logLevel",         SETTING_INT,  &gSettings.logLevel,            3, 0, nullptr, 0, 3, "Log Level",            "0:error,1:warn,2:info,3:debug", false, nullptr, "loglevel" },
};
#undef DBG_SUBROW
#undef DBG_ROW

// Row accounting, pinned: 159 = 116 flag-backed + 40 bitless-sub + 3 hand rows.
static constexpr size_t kDbgRegGeneratedRows = (DBG_FLAG_COUNT - 1) + DBG_SUBBOOL_COUNT;  // ALWAYS emits no row
static constexpr size_t kDbgRegHandRows = 3;  // memorysampleintervalsec, webconsole, loglevel
static_assert(sizeof(debugSettingEntries) / sizeof(debugSettingEntries[0])
                  == kDbgRegGeneratedRows + kDbgRegHandRows,
              "debug registry row count drifted — a pick or table row changed without the other (or a hand row came/went without updating kDbgRegHandRows)");

// Every pool row is picked exactly once — a forgotten or doubled DBG_ROW /
// DBG_SUBROW is a build error, not a silently missing checkbox. Matching is
// by valuePtr (every pool pointer lands inside gSettings, so constexpr
// equality is well-defined); hand rows live in neither pool and are outside
// the check.
static constexpr bool dbgRegRowsPickedOnce() {
  for (int i = 0; i < DBG_FLAG_COUNT; ++i) {
    if (kDbgFlagRegEntry[i].valuePtr == nullptr) continue;  // ALWAYS placeholder
    int n = 0;
    for (const auto& e : debugSettingEntries)
      if (e.valuePtr == kDbgFlagRegEntry[i].valuePtr) ++n;
    if (n != 1) return false;
  }
  for (int i = 0; i < DBG_SUBBOOL_COUNT; ++i) {
    int n = 0;
    for (const auto& e : debugSettingEntries)
      if (e.valuePtr == kDbgSubRegEntry[i].valuePtr) ++n;
    if (n != 1) return false;
  }
  return true;
}
static_assert(dbgRegRowsPickedOnce(),
              "a generated registry row is missing from (or duplicated in) debugSettingEntries — reconcile the pick list with the tables");

// Generated rows carry non-empty registry strings. The logLevel hand row is
// deliberately OUTSIDE this check — its group is a null pointer (ungrouped/
// root), and a constexpr string walk over it would be a compile error, not a
// passing check.
static constexpr bool dbgRegColumnsPresent() {
  for (int i = 0; i < DBG_FLAG_COUNT; ++i) {
    const SettingEntry& e = kDbgFlagRegEntry[i];
    if (e.valuePtr == nullptr) continue;  // ALWAYS placeholder
    if (e.jsonKey == nullptr || e.jsonKey[0] == '\0') return false;
    if (e.group   == nullptr || e.group[0]   == '\0') return false;
    if (e.label   == nullptr || e.label[0]   == '\0') return false;
    if (e.cmdKey  == nullptr || e.cmdKey[0]  == '\0') return false;
  }
  for (int i = 0; i < DBG_SUBBOOL_COUNT; ++i) {
    const SettingEntry& e = kDbgSubRegEntry[i];
    if (e.jsonKey == nullptr || e.jsonKey[0] == '\0') return false;
    if (e.group   == nullptr || e.group[0]   == '\0') return false;
    if (e.label   == nullptr || e.label[0]   == '\0') return false;
    if (e.cmdKey  == nullptr || e.cmdKey[0]  == '\0') return false;
  }
  return true;
}
static_assert(dbgRegColumnsPresent(),
              "a generated registry row has a null/empty group, jsonKey, label, or cmdKey column");

// No two generated rows share (group, jsonKey) — that pair is the JSON
// nesting identity under "system.debug" (jsonKey alone repeats across ~30
// groups by design), so a transposed column would silently merge two
// settings into one persisted key.
static constexpr bool dbgRegGroupKeysUnique() {
  constexpr int nf = DBG_FLAG_COUNT, ns = DBG_SUBBOOL_COUNT;
  for (int i = 0; i < nf + ns; ++i) {
    const SettingEntry& a = (i < nf) ? kDbgFlagRegEntry[i] : kDbgSubRegEntry[i - nf];
    if (a.valuePtr == nullptr) continue;  // ALWAYS placeholder
    for (int j = i + 1; j < nf + ns; ++j) {
      const SettingEntry& b = (j < nf) ? kDbgFlagRegEntry[j] : kDbgSubRegEntry[j - nf];
      if (b.valuePtr == nullptr) continue;
      if (dbgStrEq(a.group, b.group) && dbgStrEq(a.jsonKey, b.jsonKey)) return false;
    }
  }
  return true;
}
static_assert(dbgRegGroupKeysUnique(), "two generated registry rows share (group, jsonKey)");

// Columns: name, jsonSection, entries, count, isConnected, description
static const SettingsModule debugSettingsModule = {
  "debug",
  "system.debug",
  debugSettingEntries,
  sizeof(debugSettingEntries) / sizeof(debugSettingEntries[0]),
  nullptr,  // Always available
  "Debug output flags for various subsystems",
  DEBUG_JSON_FILE  // split out of settings.json — persists in its own file
};

// ============================================================================
// Output Settings Module (for modular settings registry)
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry outputSettingEntries[] = {
  // --- channels: the one persisted output lane ---
  // The group used to hold web/display/g2 rows too (plus webConsole, evicted
  // earlier for the same reason). All three were removed pre-1.0: delivery
  // never honored the web/display bits, and the g2 row's command (outg2)
  // never persisted, so the toggles were decorative. The surviving runtime
  // lanes are session/lifecycle-managed — see the System_Debug.h charter.
  { "serial",     SETTING_BOOL, &gSettings.outSerial,           1, 0, nullptr, 0, 1, "Serial Output",     nullptr, false, "channels", "outserial" },
  // --- auth: per-channel access gates ---
  { "serialRequireAuth",  SETTING_BOOL, &gSettings.serialRequireAuth,       1, 0, nullptr, 0, 1, "Serial Require Auth",  nullptr, false, "auth", "serialrequireauth" },
  { "displayRequireAuth", SETTING_BOOL, &gSettings.localDisplayRequireAuth, 1, 0, nullptr, 0, 1, "Display Require Auth", nullptr, false, "auth", "displayrequireauth" },
  { "sessionIdleWeb",     SETTING_INT,  &gSettings.sessionIdleWeb,          60, 0, nullptr, 0, 1440, "Web Idle Logout (min, 0=off)",    nullptr, false, "auth", "sessionidleweb" },
  { "sessionIdleSerial",  SETTING_INT,  &gSettings.sessionIdleSerial,       60, 0, nullptr, 0, 1440, "Serial Idle Logout (min, 0=off)", nullptr, false, "auth", "sessionidleserial" },
  { "sessionIdleBle",     SETTING_INT,  &gSettings.sessionIdleBle,          15, 0, nullptr, 0, 1440, "BLE Idle Logout (min, 0=off)",    nullptr, false, "auth", "sessionidleble" },
  { "sessionIdleDisplay", SETTING_INT,  &gSettings.sessionIdleDisplay,      60, 0, nullptr, 0, 1440, "Display Idle Logout (min, 0=off)", nullptr, false, "auth", "sessionidledisplay" },
};

// Helper: find an output setting entry by jsonKey
static const SettingEntry* findOutputEntry(const char* key) {
  for (size_t i = 0; i < sizeof(outputSettingEntries)/sizeof(outputSettingEntries[0]); i++) {
    if (strcmp(outputSettingEntries[i].jsonKey, key) == 0) return &outputSettingEntries[i];
  }
  return nullptr;
}

const char* cmd_serialrequireauth(const String& a) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return handleSettingCommand(findOutputEntry("serialRequireAuth"), a);
}

const char* cmd_displayrequireauth(const String& a) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return handleSettingCommand(findOutputEntry("displayRequireAuth"), a);
}

// Columns: name, jsonSection, entries, count, isConnected, description
static const SettingsModule outputSettingsModule = {
  "output",
  "system.output",
  outputSettingEntries,
  sizeof(outputSettingEntries) / sizeof(outputSettingEntries[0]),
  nullptr,  // Always available
  "Output routing for serial, web, and display"
};

// ============================================================================
// Crash / Reset Tracking Settings Module
// ============================================================================

static const SettingEntry crashSettingEntries[] = {
  // crashCount + lastResetReason are populated by the boot path from RTC memory
  // (HardwareOne.cpp:1044), never set by user input. readOnly=true so the schema
  // marks them as display-only and the UI renders text instead of an editable input.
  { "crashCount", SETTING_INT, &gSettings.crashCount, 0, 0, nullptr, 0, 0xFFFF, "Abnormal Reset Count", nullptr, false, nullptr, nullptr, true },
  { "lastResetReason", SETTING_INT, &gSettings.lastResetReason, 0, 0, nullptr, 0, 0xFF, "Last Reset Reason", nullptr, false, nullptr, nullptr, true },
};

static const SettingsModule crashSettingsModule = {
  "crash",
  "crash",
  crashSettingEntries,
  sizeof(crashSettingEntries) / sizeof(crashSettingEntries[0]),
  nullptr,
  "Crash and reset tracking (populated from RTC memory on each boot)"
};

// Registry storage
EXT_RAM_BSS_ATTR static const SettingsModule* gSettingsModules[MAX_SETTINGS_MODULES];
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
#if ENABLE_OLED_INPUT
extern const SettingsModule inputSettingsModule;
#endif
#if ENABLE_ANO_ENCODER
extern const SettingsModule anoEncoderSettingsModule;
#endif
#if ENABLE_APDS_SENSOR
extern const SettingsModule apdsSettingsModule;
#endif
#if ENABLE_GPS_SENSOR
extern const SettingsModule gpsSettingsModule;
#endif
#if ENABLE_FM_RADIO
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
#if ENABLE_MICROPHONE
extern const SettingsModule micSettingsModule;
#endif
#if ENABLE_EDGE_IMPULSE
extern const SettingsModule edgeImpulseSettingsModule;
#endif
#if ENABLE_ESP_SR
extern const SettingsModule espsrSettingsModule;
#endif

extern const SettingsModule sensorLogSettingsModule;
extern const SettingsModule systemLogSettingsModule;
extern const SettingsModule batteryLogSettingsModule;

void registerAllSettingsModules() {
  if (gSettingsModulesRegistered) return;  // Only register once
  gSettingsModulesRegistered = true;
  
  // Core system modules
  registerSettingsModule(&crashSettingsModule);
  registerSettingsModule(&debugSettingsModule);
  registerSettingsModule(&outputSettingsModule);
#if ENABLE_I2C_SYSTEM
  registerSettingsModule(&i2cSettingsModule);
#endif
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
#if ENABLE_OLED_INPUT
  registerSettingsModule(&inputSettingsModule);
#endif
#if ENABLE_ANO_ENCODER
  registerSettingsModule(&anoEncoderSettingsModule);
#endif
#if ENABLE_APDS_SENSOR
  registerSettingsModule(&apdsSettingsModule);
#endif
#if ENABLE_GPS_SENSOR
  registerSettingsModule(&gpsSettingsModule);
#endif
#if ENABLE_FM_RADIO
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
#if ENABLE_MICROPHONE
  registerSettingsModule(&micSettingsModule);
#endif
#if ENABLE_EDGE_IMPULSE
  registerSettingsModule(&edgeImpulseSettingsModule);
#endif
#if ENABLE_ESP_SR
  registerSettingsModule(&espsrSettingsModule);
#endif

  // Logging modules
  registerSettingsModule(&sensorLogSettingsModule);
  registerSettingsModule(&systemLogSettingsModule);
  registerSettingsModule(&batteryLogSettingsModule);

  // Notification presentation (banners/toasts/queue + per-kind muting)
  extern const SettingsModule notifSettingsModule;
  registerSettingsModule(&notifSettingsModule);

#if ENABLE_ONDEVICE_LLM
  extern const SettingsModule llmSettingsModule;
  registerSettingsModule(&llmSettingsModule);
#endif

#if ENABLE_MAPS
  extern const SettingsModule mapsSettingsModule;
  registerSettingsModule(&mapsSettingsModule);
#endif

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
        case SETTING_U8:
          // 1-byte write — anything wider would clobber adjacent fields.
          *((uint8_t*)e->valuePtr) = (uint8_t)e->intDefault;
          break;
        case SETTING_U16:
          *((uint16_t*)e->valuePtr) = (uint16_t)e->intDefault;
          break;
        case SETTING_U32:
          *((uint32_t*)e->valuePtr) = (uint32_t)e->intDefault;
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

// Task #71 — bounds-check integer settings on load.
//
// Write path (handleSettingCommand at the bottom of this file) rejects
// out-of-range values with an error message. Load path historically did
// not — so a manually-edited settings.json, an old-firmware-written value,
// or an on-disk bit flip could leave a setting outside its declared
// [minVal, maxVal] for the entire boot. CLI reads would then surface the
// invalid value to the user and the firmware would consume it for code
// paths that assumed validation.
//
// Strategy: clamp to the nearest boundary (don't reject — that would
// orphan the field to default, throwing away user intent for a single
// corrupt byte) and log loudly so the corruption is visible. A field
// with min=0/max=0 means "no bounds declared" and is left alone.
static int settingsLoadClampInt(int raw, const SettingEntry* e) {
  if (e->minVal == 0 && e->maxVal == 0) return raw;  // no bounds declared
  if (raw < e->minVal) {
    WARN_STORAGEF("[Settings] %s on-disk value %d below min %d — clamping",
                  e->jsonKey, raw, e->minVal);
    return e->minVal;
  }
  if (raw > e->maxVal) {
    WARN_STORAGEF("[Settings] %s on-disk value %d above max %d — clamping",
                  e->jsonKey, raw, e->maxVal);
    return e->maxVal;
  }
  return raw;
}

static float settingsLoadClampFloat(float raw, const SettingEntry* e) {
  if (e->minVal == 0 && e->maxVal == 0) return raw;
  if (raw < (float)e->minVal) {
    WARN_STORAGEF("[Settings] %s on-disk value %.3f below min %d — clamping",
                  e->jsonKey, raw, e->minVal);
    return (float)e->minVal;
  }
  if (raw > (float)e->maxVal) {
    WARN_STORAGEF("[Settings] %s on-disk value %.3f above max %d — clamping",
                  e->jsonKey, raw, e->maxVal);
    return (float)e->maxVal;
  }
  return raw;
}

size_t readRegisteredSettings(JsonDocument& doc, const char* onlyPersistFile, bool allFiles) {
  size_t count = 0;

  for (size_t m = 0; m < gSettingsModuleCount; m++) {
    const SettingsModule* mod = gSettingsModules[m];
    // Scope filter for the settings.json/debug.json split (see header):
    // allFiles=false selects a single persistence target.
    if (!allFiles) {
      if (onlyPersistFile == nullptr) { if (mod->persistFile != nullptr) continue; }
      else if (mod->persistFile == nullptr || strcmp(mod->persistFile, onlyPersistFile) != 0) continue;
    }
    // Walk dotted jsonSection path
    JsonVariantConst section = jsonPathRead(doc, mod->jsonSection);
    if (section.isNull()) continue;

    for (size_t i = 0; i < mod->count; i++) {
      const SettingEntry* e = &mod->entries[i];
      // Navigate to group sub-object if specified
      JsonVariantConst current = section;
      if (e->group) {
        current = current[e->group];
        if (current.isNull()) continue;
      }
      JsonVariantConst val = current[e->jsonKey];
      if (val.isNull()) continue;
      switch (e->type) {
        case SETTING_INT: {
          int raw = val | e->intDefault;
          *((int*)e->valuePtr) = settingsLoadClampInt(raw, e);
          count++;
          break;
        }
        case SETTING_U8: {
          // Width-clamp first to guard against on-disk garbage from older
          // firmware that wrote 4 bytes into a 1-byte field, then range-clamp
          // via the shared helper (task #71). Both protections matter:
          //  - width-clamp recovers from struct corruption
          //  - range-clamp catches manual tampering / bit flips within range
          int raw = (int)((uint32_t)(val | e->intDefault) & 0xFFu);
          *((uint8_t*)e->valuePtr) = (uint8_t)settingsLoadClampInt(raw, e);
          count++;
          break;
        }
        case SETTING_U16: {
          int raw = (int)((uint32_t)(val | e->intDefault) & 0xFFFFu);
          *((uint16_t*)e->valuePtr) = (uint16_t)settingsLoadClampInt(raw, e);
          count++;
          break;
        }
        case SETTING_U32: {
          // SETTING_U32 values can exceed INT_MAX, so clamp via uint32 math.
          // Most U32 settings have maxVal that fits in int range anyway.
          uint32_t raw = (uint32_t)(val | e->intDefault);
          if (e->minVal != 0 || e->maxVal != 0) {
            // Use the signed-int helper when bounds fit; otherwise let raw
            // pass through (uint32 fields with bounds > INT_MAX are rare).
            int clamped = settingsLoadClampInt((int)raw, e);
            raw = (uint32_t)clamped;
          }
          *((uint32_t*)e->valuePtr) = raw;
          count++;
          break;
        }
        case SETTING_FLOAT: {
          float raw = val | e->floatDefault;
          *((float*)e->valuePtr) = settingsLoadClampFloat(raw, e);
          count++;
          break;
        }
        case SETTING_BOOL:
          *((bool*)e->valuePtr) = val | (e->intDefault != 0);
          count++;
          break;
        case SETTING_STRING:
          if (e->isSecret) {
            // Decrypt secret strings when reading from disk
            const char* encrypted = val | (e->stringDefault ? e->stringDefault : "");
            String dec = decryptString(encrypted);
            if (strncmp(encrypted, "AES:", 4) == 0 && dec.length() == 0) {
              gSecretLoadFailures++;
              logSystemEvent("CRYPTO", "secret '%s' failed to decrypt at load — stored blob preserved", e->jsonKey);
              systemEventPost(SYSEVT_SECRET_DECRYPT_FAILED, e->jsonKey, "stored blob preserved");
            }
            *((String*)e->valuePtr) = dec;
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

size_t writeRegisteredSettings(JsonDocument& doc, const char* onlyPersistFile, bool allFiles) {
  size_t count = 0;

  for (size_t m = 0; m < gSettingsModuleCount; m++) {
    const SettingsModule* mod = gSettingsModules[m];
    // Scope filter for the settings.json/debug.json split (see header):
    // allFiles=false selects a single persistence target.
    if (!allFiles) {
      if (onlyPersistFile == nullptr) { if (mod->persistFile != nullptr) continue; }
      else if (mod->persistFile == nullptr || strcmp(mod->persistFile, onlyPersistFile) != 0) continue;
    }
    // Walk dotted jsonSection path, creating intermediate objects as needed.
    // Empty/null jsonSection writes directly to the doc root.
    JsonObject section = jsonPathCreate(doc, mod->jsonSection);

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
      
      // Navigate to group sub-object if specified
      JsonObject target = section;
      if (e->group) {
        JsonObject groupObj = target[e->group].as<JsonObject>();
        if (groupObj.isNull()) {
          groupObj = target[e->group].to<JsonObject>();
        }
        target = groupObj;
      }
      const char* leaf = e->jsonKey;
      
      switch (e->type) {
        case SETTING_INT:
          target[leaf] = *((int*)e->valuePtr);
          count++;
          break;
        case SETTING_U8:
          // Width-correct READ as well — historically read 4 bytes through
          // a uint8_t pointer, which produced a 4-byte JSON value (the high
          // 3 bytes being whatever happened to be in the adjacent fields).
          // That's how the on-disk corruption persisted across reboots.
          target[leaf] = (int)*((uint8_t*)e->valuePtr);
          count++;
          break;
        case SETTING_U16:
          target[leaf] = (int)*((uint16_t*)e->valuePtr);
          count++;
          break;
        case SETTING_U32:
          target[leaf] = (uint32_t)*((uint32_t*)e->valuePtr);
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
            // Guarded write: never replaces the merge-read's stored blob with
            // "" after a damaged load or a failed encryption (target[leaf]
            // still holds the on-disk value here).
            putSecret(target, leaf, *((String*)e->valuePtr));
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

// Fire SYSEVT_SETTING_CHANGED for a field changed through setSetting(). The mutator
// only has a field reference, so reverse-look-up the registered SettingEntry by its
// valuePtr to recover the setting's name + type, format the just-written value
// (masking secrets), and post. A field with no registered entry (internal/runtime
// state) posts nothing — and because setSetting() writes flash on every change, this
// only ever fires on genuine, infrequent config changes. The handleSettingCommand
// path below posts its own SETTING_CHANGED (it writes valuePtr directly, not via
// setSetting), so the two paths are disjoint and never double-fire.
void notifySettingChanged(const void* fieldPtr) {
  if (!fieldPtr) return;
  for (size_t m = 0; m < gSettingsModuleCount; m++) {
    const SettingsModule* mod = gSettingsModules[m];
    if (!mod) continue;
    for (size_t i = 0; i < mod->count; i++) {
      const SettingEntry* e = &mod->entries[i];
      if (e->valuePtr != fieldPtr) continue;
      char val[48];
      switch (e->type) {
        case SETTING_INT:   snprintf(val, sizeof(val), "%d", *((const int*)e->valuePtr)); break;
        case SETTING_U8:    snprintf(val, sizeof(val), "%u", (unsigned)*((const uint8_t*)e->valuePtr)); break;
        case SETTING_U16:   snprintf(val, sizeof(val), "%u", (unsigned)*((const uint16_t*)e->valuePtr)); break;
        case SETTING_U32:   snprintf(val, sizeof(val), "%lu", (unsigned long)*((const uint32_t*)e->valuePtr)); break;
        case SETTING_FLOAT: snprintf(val, sizeof(val), "%.3f", *((const float*)e->valuePtr)); break;
        case SETTING_BOOL:  snprintf(val, sizeof(val), "%s", *((const bool*)e->valuePtr) ? "on" : "off"); break;
        case SETTING_STRING:
          if (e->isSecret) snprintf(val, sizeof(val), "********");
          else             snprintf(val, sizeof(val), "%s", ((const String*)e->valuePtr)->c_str());
          break;
        default: val[0] = '\0'; break;
      }
      systemEventPost(SYSEVT_SETTING_CHANGED, e->label ? e->label : e->jsonKey, val);
      return;  // valuePtr is unique across the registry — first match is the only one
    }
  }
}

// When a master *enabled* bool is cleared via handleSettingCommand, tear down
// any live instance immediately (mqttclientenabled pattern). Autostart flags
// are intentionally NOT here — those only affect the next boot.
// Uses executeCommandThroughRegistry (direct handler call, no cmd queue) so
// this is safe from inside a settings command on cmd_exec_task.
static void applyMasterEnableDisable(const char* cmdKey) {
  if (!cmdKey || !cmdKey[0]) return;

  struct Pair { const char* key; const char* stopCmd; };
  static const Pair kStops[] = {
    { "wifienabled",       "closewifi" },
    { "httpenabled",       "closehttp" },
    { "bleenabled",        "g2deinit" },       // client first
    { "thermalenabled",    "closethermal" },
    { "tofenabled",        "closetof" },
    { "imuenabled",        "closeimu" },
    { "gpsenabled",        "closegps" },
    { "fmradioenabled",    "closefmradio" },
    { "apdsenabled",       "closeapds" },
    { "rtcenabled",        "closertc" },
    { "presenceenabled",   "closepresence" },
    { "inputenabled",      "closeinput" },
    { "cameraenabled",     "closecamera" },
    { "micenabled",        "closemic" },
    { "srenabled",         "srstop" },
    { "llmenabled",        "llmunload" },
    { "sensorlogenabled",  "sensorlog stop" },
    { "systemlogenabled",  "log stop" },
  };

  for (size_t i = 0; i < sizeof(kStops) / sizeof(kStops[0]); ++i) {
    if (strcmp(cmdKey, kStops[i].key) != 0) continue;
    (void)executeCommandThroughRegistry(kStops[i].stopCmd);
    // BLE: also drop server mode if client teardown left it (or was server-only).
    if (strcmp(cmdKey, "bleenabled") == 0) {
      (void)executeCommandThroughRegistry("closeble");
    }
    return;
  }
}

const char* handleSettingCommand(const SettingEntry* entry, const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) {
    // No argument - show current value
    EXT_RAM_BSS_ATTR static char buf[128];
    switch (entry->type) {
      case SETTING_INT:
        snprintf(buf, sizeof(buf), "%s = %d", entry->jsonKey, *((int*)entry->valuePtr));
        break;
      case SETTING_U8:
        snprintf(buf, sizeof(buf), "%s = %u", entry->jsonKey, (unsigned)*((uint8_t*)entry->valuePtr));
        break;
      case SETTING_U16:
        snprintf(buf, sizeof(buf), "%s = %u", entry->jsonKey, (unsigned)*((uint16_t*)entry->valuePtr));
        break;
      case SETTING_U32:
        snprintf(buf, sizeof(buf), "%s = %lu", entry->jsonKey, (unsigned long)*((uint32_t*)entry->valuePtr));
        break;
      case SETTING_FLOAT:
        snprintf(buf, sizeof(buf), "%s = %.3f", entry->jsonKey, *((float*)entry->valuePtr));
        break;
      case SETTING_BOOL:
        snprintf(buf, sizeof(buf), "%s = %s", entry->jsonKey, *((bool*)entry->valuePtr) ? "true" : "false");
        break;
      case SETTING_STRING:
        if (entry->isSecret && ((String*)entry->valuePtr)->length() > 0) {
          snprintf(buf, sizeof(buf), "%s = ********", entry->jsonKey);
        } else {
          snprintf(buf, sizeof(buf), "%s = %s", entry->jsonKey, ((String*)entry->valuePtr)->c_str());
        }
        break;
    }
    return buf;
  }
  
  const char* p = valStr.c_str();
  
  // Parse and validate based on type
  switch (entry->type) {
    case SETTING_INT:
    case SETTING_U8:
    case SETTING_U16:
    case SETTING_U32: {
      int v = atoi(p);
      if (entry->minVal != 0 || entry->maxVal != 0) {
        if (v < entry->minVal || v > entry->maxVal) {
          EXT_RAM_BSS_ATTR static char errBuf[128];
          snprintf(errBuf, sizeof(errBuf), "Error: %s must be %d..%d", entry->jsonKey, entry->minVal, entry->maxVal);
          return errBuf;
        }
      }
      // Width-correct write — otherwise a uint8_t/uint16_t field would
      // overflow into adjacent struct members (the bug that caused the
      // 2026-05-18 heap-corruption / WDT crash on passphrase set).
      switch (entry->type) {
        case SETTING_INT: *((int*)entry->valuePtr)      = v;                          break;
        case SETTING_U8:  *((uint8_t*)entry->valuePtr)  = (uint8_t)(v & 0xFF);        break;
        case SETTING_U16: *((uint16_t*)entry->valuePtr) = (uint16_t)(v & 0xFFFF);     break;
        case SETTING_U32: *((uint32_t*)entry->valuePtr) = (uint32_t)v;                break;
        default: break;  // unreachable — outer switch already filtered
      }
      if (!gDeferWrites) writeSettingsJson();
      BROADCAST_PRINTF("%s set to %d", entry->jsonKey, v);
      { char vBuf[16]; snprintf(vBuf, sizeof(vBuf), "%d", v); systemEventPost(SYSEVT_SETTING_CHANGED, entry->label ? entry->label : entry->jsonKey, vBuf); }
      return "[Settings] Configuration updated";
    }
    case SETTING_FLOAT: {
      float f = strtof(p, nullptr);
      if (entry->minVal != 0 || entry->maxVal != 0) {
        if (f < (float)entry->minVal || f > (float)entry->maxVal) {
          EXT_RAM_BSS_ATTR static char errBuf[128];
          snprintf(errBuf, sizeof(errBuf), "Error: %s must be %d..%d", entry->jsonKey, entry->minVal, entry->maxVal);
          return errBuf;
        }
      }
      *((float*)entry->valuePtr) = f;
      if (!gDeferWrites) writeSettingsJson();
      BROADCAST_PRINTF("%s set to %.3f", entry->jsonKey, f);
      { char vBuf[16]; snprintf(vBuf, sizeof(vBuf), "%.3f", f); systemEventPost(SYSEVT_SETTING_CHANGED, entry->label ? entry->label : entry->jsonKey, vBuf); }
      return "[Settings] Configuration updated";
    }
    case SETTING_BOOL: {
      bool v = (*p == '1' || strncasecmp(p, "true", 4) == 0);
      *((bool*)entry->valuePtr) = v;
      if (!gDeferWrites) writeSettingsJson();
      // Master *enabled* off → stop the live subsystem now (not only next boot).
      if (!v) applyMasterEnableDisable(entry->cmdKey ? entry->cmdKey : entry->jsonKey);
      BROADCAST_PRINTF("%s set to %s", entry->jsonKey, v ? "true" : "false");
      systemEventPost(SYSEVT_SETTING_CHANGED, entry->label ? entry->label : entry->jsonKey, v ? "on" : "off");
      return "[Settings] Configuration updated";
    }
    case SETTING_STRING: {
      *((String*)entry->valuePtr) = p;
      if (!gDeferWrites) writeSettingsJson();
      if (entry->isSecret) {
        BROADCAST_PRINTF("%s updated", entry->jsonKey);
        systemEventPost(SYSEVT_SETTING_CHANGED, entry->label ? entry->label : entry->jsonKey, "********");
      } else {
        BROADCAST_PRINTF("%s set to %s", entry->jsonKey, p);
        systemEventPost(SYSEVT_SETTING_CHANGED, entry->label ? entry->label : entry->jsonKey, p);
      }
      return "[Settings] Configuration updated";
    }
  }
  return "Error: Unknown setting type";
}

// ============================================================================
// Settings-editor field commands
// ----------------------------------------------------------------------------
// Static, registered CLI commands — one per editable setting that had no
// dedicated module command — so the web/OLED settings screen can save those
// fields. NOT runtime auto-registration: these are real CommandEntry rows in a
// static table the skill scanner sees. Each looks up its OWN SettingEntry by
// its (unique) cmdKey and writes via handleSettingCommand. Settings that need a
// live apply action keep pointing their cmdKey at a dedicated command instead.
// ============================================================================
const SettingEntry* findSettingByCmdKey(const char* cmdKey) {
  if (!cmdKey || !cmdKey[0]) return nullptr;
  size_t n = 0;
  const SettingsModule** mods = getSettingsModules(n);
  for (size_t m = 0; m < n; m++) {
    const SettingsModule* mod = mods[m];
    if (!mod) continue;
    for (size_t i = 0; i < mod->count; i++) {
      const SettingEntry* e = &mod->entries[i];
      if (e->cmdKey && strcasecmp(e->cmdKey, cmdKey) == 0) return e;
    }
  }
  return nullptr;
}

#define SETTING_EDITOR_CMD(func, cmdKeyLit) \
  static const char* func(const String& a) { \
    RETURN_VALID_IF_VALIDATE_CSTR(); \
    const SettingEntry* e = findSettingByCmdKey(cmdKeyLit); \
    if (!e) return "Error: setting not found for this command"; \
    return handleSettingCommand(e, a); \
  }

SETTING_EDITOR_CMD(cmd_set_sessionidleweb,      "sessionidleweb")
SETTING_EDITOR_CMD(cmd_set_sessionidleserial,   "sessionidleserial")
SETTING_EDITOR_CMD(cmd_set_sessionidleble,      "sessionidleble")
SETTING_EDITOR_CMD(cmd_set_sessionidledisplay,  "sessionidledisplay")
SETTING_EDITOR_CMD(cmd_set_powerdim,            "powerdim")
SETTING_EDITOR_CMD(cmd_set_logcategorytags,     "logcategorytags")
SETTING_EDITOR_CMD(cmd_set_tofi2cclockhz,       "tofi2cclockhz")
SETTING_EDITOR_CMD(cmd_set_presencedevicepollms,"presencedevicepollms")
SETTING_EDITOR_CMD(cmd_set_apdsdevicepollms,    "apdsdevicepollms")
SETTING_EDITOR_CMD(cmd_set_fmradiodevicepollms, "fmradiodevicepollms")
SETTING_EDITOR_CMD(cmd_set_gpsdevicepollms,     "gpsdevicepollms")
SETTING_EDITOR_CMD(cmd_set_sensorlogpath,       "sensorlogpath")
SETTING_EDITOR_CMD(cmd_set_eirequirelabels,     "eirequirelabels")
SETTING_EDITOR_CMD(cmd_set_eimaxdetections,     "eimaxdetections")
SETTING_EDITOR_CMD(cmd_set_eiinputsize,         "eiinputsize")
SETTING_EDITOR_CMD(cmd_set_eiinterval,          "eiinterval")
SETTING_EDITOR_CMD(cmd_set_srautostart,         "srautostart")
SETTING_EDITOR_CMD(cmd_set_srmodelsource,       "srmodelsource")
SETTING_EDITOR_CMD(cmd_set_eventlog,            "eventlog")
SETTING_EDITOR_CMD(cmd_set_notifydevicebanners,        "notifydevicebanners")
SETTING_EDITOR_CMD(cmd_set_notifydevicetoasts,         "notifydevicetoasts")
SETTING_EDITOR_CMD(cmd_set_notifydevicequeue,          "notifydevicequeue")
SETTING_EDITOR_CMD(cmd_set_notifydeviceg2,             "notifydeviceg2")
// ESP-NOW frame capture. Both settings are READ by the capture path
// (System_ESPNow.cpp:5813) but nothing ever wrote them: their SettingEntry rows
// named these commands and the commands were never added, so the feature could
// not be switched on from any surface. Found by `settings_registry.py check`.
SETTING_EDITOR_CMD(cmd_set_espnowcapturetosd,          "espnowcapturetosd")
SETTING_EDITOR_CMD(cmd_set_espnowcaptureskipheartbeats, "espnowcaptureskipheartbeats")
SETTING_EDITOR_CMD(cmd_set_wifienabled,   "wifienabled")
SETTING_EDITOR_CMD(cmd_set_wifiautostart, "wifiautostart")
SETTING_EDITOR_CMD(cmd_set_sensorlogenabled, "sensorlogenabled")
SETTING_EDITOR_CMD(cmd_set_systemlogenabled, "systemlogenabled")
// Two-axis feature control (see the struct comment in System_Settings.h).
SETTING_EDITOR_CMD(cmd_set_thermalenabled, "thermalenabled")
SETTING_EDITOR_CMD(cmd_set_tofenabled, "tofenabled")
SETTING_EDITOR_CMD(cmd_set_imuenabled, "imuenabled")
SETTING_EDITOR_CMD(cmd_set_gpsenabled, "gpsenabled")
SETTING_EDITOR_CMD(cmd_set_fmradioenabled, "fmradioenabled")
SETTING_EDITOR_CMD(cmd_set_apdsenabled, "apdsenabled")
SETTING_EDITOR_CMD(cmd_set_rtcenabled, "rtcenabled")
SETTING_EDITOR_CMD(cmd_set_presenceenabled, "presenceenabled")
SETTING_EDITOR_CMD(cmd_set_inputenabled, "inputenabled")
SETTING_EDITOR_CMD(cmd_set_cameraenabled, "cameraenabled")
SETTING_EDITOR_CMD(cmd_set_micenabled, "micenabled")
SETTING_EDITOR_CMD(cmd_set_srenabled, "srenabled")
SETTING_EDITOR_CMD(cmd_set_llmenabled, "llmenabled")
SETTING_EDITOR_CMD(cmd_set_httpenabled, "httpenabled")
SETTING_EDITOR_CMD(cmd_set_bleenabled, "bleenabled")
SETTING_EDITOR_CMD(cmd_set_espnowautostart, "espnowautostart")
SETTING_EDITOR_CMD(cmd_set_oledautostart, "oledautostart")
SETTING_EDITOR_CMD(cmd_set_eiautostart, "eiautostart")
SETTING_EDITOR_CMD(cmd_set_automationautostart, "automationautostart")

// Bitmask / format editors — persist via SettingEntry, then sync live globals
// so Settings-page saves affect the next logging session without reboot.
static const char* cmd_set_sensorlogmask(const String& a) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const SettingEntry* e = findSettingByCmdKey("sensorlogmask");
  if (!e) return "Error: setting not found for this command";
  const char* r = handleSettingCommand(e, a);
  gSensorLogMask = (uint8_t)(gSettings.sensorLogMask & 0xFF);
  return r;
}
static const char* cmd_set_sensorlogformat(const String& a) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const SettingEntry* e = findSettingByCmdKey("sensorlogformat");
  if (!e) return "Error: setting not found for this command";
  const char* r = handleSettingCommand(e, a);
  if (gSettings.sensorLogFormat >= 0 && gSettings.sensorLogFormat <= 2)
    gSensorLogFormat = (SensorLogFormat)gSettings.sensorLogFormat;
  return r;
}
static const char* cmd_set_systemlogflags(const String& a) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const SettingEntry* e = findSettingByCmdKey("systemlogflags");
  if (!e) return "Error: setting not found for this command";
  const char* r = handleSettingCommand(e, a);
  systemLogApplyPersistedFlags();
  return r;
}

const CommandEntry settingEditorCommands[] = {
  { "sessionidleweb",      "Set web CLI session idle-logout (min)",      true, cmd_set_sessionidleweb,      "Usage: sessionidleweb <0-1440>" },
  { "sessionidleserial",   "Set serial session idle-logout (min)",       true, cmd_set_sessionidleserial,   "Usage: sessionidleserial <0-1440>" },
  { "sessionidleble",      "Set BLE session idle-logout (min)",          true, cmd_set_sessionidleble,      "Usage: sessionidleble <0-1440>" },
  { "sessionidledisplay",  "Set OLED session idle-logout (min)",         true, cmd_set_sessionidledisplay,  "Usage: sessionidledisplay <0-1440>" },
  { "powerdim",            "Set display dim level (%)",                  true, cmd_set_powerdim,            "Usage: powerdim <0-100>" },
  { "logcategorytags",     "Set log category-tags flag (persist only)",  true, cmd_set_logcategorytags,     "Usage: logcategorytags <0|1>" },
  { "tofi2cclockhz",       "Set ToF I2C clock (Hz)",                     true, cmd_set_tofi2cclockhz,       "Usage: tofi2cclockhz <50000-400000>" },
  { "presencedevicepollms","Set presence sensor poll interval (ms)",     true, cmd_set_presencedevicepollms,"Usage: presencedevicepollms <50-5000>" },
  { "apdsdevicepollms",    "Set APDS poll interval (ms)",                true, cmd_set_apdsdevicepollms,    "Usage: apdsdevicepollms <value>" },
  { "fmradiodevicepollms", "Set FM radio poll interval (ms)",            true, cmd_set_fmradiodevicepollms, "Usage: fmradiodevicepollms <value>" },
  { "gpsdevicepollms",     "Set GPS poll interval (ms)",                 true, cmd_set_gpsdevicepollms,     "Usage: gpsdevicepollms <value>" },
  { "sensorlogpath",       "Set default sensor-log file path",           true, cmd_set_sensorlogpath,       "Usage: sensorlogpath <\"/path\">" },
  { "sensorlogmask",       "Set sensor-log sensor bitmask",              true, cmd_set_sensorlogmask,       "Usage: sensorlogmask <0-255>" },
  { "sensorlogformat",     "Set sensor-log format (0=text,1=csv,2=track)", true, cmd_set_sensorlogformat,   "Usage: sensorlogformat <0|1|2>" },
  { "systemlogflags",      "Set system-log debug category mask (hex)",   true, cmd_set_systemlogflags,      "Usage: systemlogflags <0x...>" },
  { "eirequirelabels",     "Set Edge Impulse require-labels flag",       true, cmd_set_eirequirelabels,     "Usage: eirequirelabels <0|1>" },
  { "eimaxdetections",     "Set Edge Impulse max detections",            true, cmd_set_eimaxdetections,     "Usage: eimaxdetections <value>" },
  { "eiinputsize",         "Set Edge Impulse input size",                true, cmd_set_eiinputsize,         "Usage: eiinputsize <value>" },
  { "eiinterval",          "Set Edge Impulse inference interval (ms)",   true, cmd_set_eiinterval,          "Usage: eiinterval <100-10000>" },
  { "srautostart",         "Set ESP-SR auto-start flag",                 true, cmd_set_srautostart,         "Usage: srautostart <0|1>" },
  { "srmodelsource",       "Set ESP-SR model source",                    true, cmd_set_srmodelsource,       "Usage: srmodelsource <value>" },
  { "eventlog",            "Enable/disable the structured event-history log (events.log)", true, cmd_set_eventlog,
    "Usage: eventlog <0|1>\n  One line per system event, durable across reboots. Display/behavior unaffected." },
  { "notifydevicebanners",        "Enable/disable OLED notification banners",   true, cmd_set_notifydevicebanners,        "Usage: notifydevicebanners <0|1>" },
  { "notifydevicetoasts",         "Enable/disable web notification toasts",     true, cmd_set_notifydevicetoasts,         "Usage: notifydevicetoasts <0|1>" },
  { "notifydevicequeue",          "Enable/disable the notification-center queue", true, cmd_set_notifydevicequeue,        "Usage: notifydevicequeue <0|1>" },
  { "notifydeviceg2",             "Enable/disable G2 lens notification cards",  true, cmd_set_notifydeviceg2,             "Usage: notifydeviceg2 <0|1>" },
  { "espnowcapturetosd",          "Capture ESP-NOW frames to the SD card",      true, cmd_set_espnowcapturetosd,          "Usage: espnowcapturetosd <0|1>\n  Needs an SD card mounted; frames are appended as they arrive." },
  { "espnowcaptureskipheartbeats", "Omit heartbeat frames from the ESP-NOW capture", true, cmd_set_espnowcaptureskipheartbeats, "Usage: espnowcaptureskipheartbeats <0|1>" },
  { "wifienabled", "Enable/disable WiFi entirely (ESP-NOW unaffected): <0|1>", true, cmd_set_wifienabled, "Usage: wifienabled <0|1>" },
  { "wifiautostart", "Connect to a saved WiFi network at boot: <0|1>", true, cmd_set_wifiautostart, "Usage: wifiautostart <0|1>" },
  { "sensorlogenabled", "Enable/disable sensor logging entirely: <0|1>", true, cmd_set_sensorlogenabled, "Usage: sensorlogenabled <0|1>" },
  { "systemlogenabled", "Enable/disable system logging entirely: <0|1>", true, cmd_set_systemlogenabled, "Usage: systemlogenabled <0|1>" },
  { "thermalenabled", "Enable/disable the thermal camera subsystem", true, cmd_set_thermalenabled, "Usage: thermalenabled <0|1>" },
  { "tofenabled", "Enable/disable the ToF distance sensor subsystem", true, cmd_set_tofenabled, "Usage: tofenabled <0|1>" },
  { "imuenabled", "Enable/disable the IMU subsystem", true, cmd_set_imuenabled, "Usage: imuenabled <0|1>" },
  { "gpsenabled", "Enable/disable the GPS subsystem", true, cmd_set_gpsenabled, "Usage: gpsenabled <0|1>" },
  { "fmradioenabled", "Enable/disable the FM radio subsystem", true, cmd_set_fmradioenabled, "Usage: fmradioenabled <0|1>" },
  { "apdsenabled", "Enable/disable the APDS gesture sensor subsystem", true, cmd_set_apdsenabled, "Usage: apdsenabled <0|1>" },
  { "rtcenabled", "Enable/disable the RTC subsystem", true, cmd_set_rtcenabled, "Usage: rtcenabled <0|1>" },
  { "presenceenabled", "Enable/disable the presence sensor subsystem", true, cmd_set_presenceenabled, "Usage: presenceenabled <0|1>" },
  { "inputenabled", "Enable/disable the input device subsystem", true, cmd_set_inputenabled, "Usage: inputenabled <0|1>" },
  { "cameraenabled", "Enable/disable the camera subsystem", true, cmd_set_cameraenabled, "Usage: cameraenabled <0|1>" },
  { "micenabled", "Enable/disable the microphone subsystem", true, cmd_set_micenabled, "Usage: micenabled <0|1>" },
  { "srenabled", "Enable/disable the speech-recognition subsystem", true, cmd_set_srenabled, "Usage: srenabled <0|1>" },
  { "llmenabled", "Enable/disable the on-device LLM subsystem", true, cmd_set_llmenabled, "Usage: llmenabled <0|1>" },
  { "httpenabled", "Enable/disable the web server subsystem", true, cmd_set_httpenabled, "Usage: httpenabled <0|1>" },
  { "bleenabled", "Enable/disable the Bluetooth subsystem", true, cmd_set_bleenabled, "Usage: bleenabled <0|1>" },
  { "espnowautostart", "Start ESP-NOW at boot", true, cmd_set_espnowautostart, "Usage: espnowautostart <0|1>" },
  { "oledautostart", "Start the OLED display at boot", true, cmd_set_oledautostart, "Usage: oledautostart <0|1>" },
  { "eiautostart", "Start Edge Impulse inference at boot", true, cmd_set_eiautostart, "Usage: eiautostart <0|1>" },
  { "automationautostart", "Start the automation scheduler at boot", true, cmd_set_automationautostart, "Usage: automationautostart <0|1>" },
  { "notifydevicekind",           "Set per-event notification visibility (device-wide)", true, cmd_notifydevicekind,
    "Usage: notifydevicekind [list [json]] | <kind> [all|admin|off]\n  Bare: show non-default kinds; list: show every kind (json = machine form)\n  <kind> alone shows its level; with a level, sets and persists it\n  admin: only admin viewers see it; off: hidden for everyone\n  Levels affect banners/toasts/queue only - events and automations still fire" },
  { "notifyusermute",           "Mute event kinds from notifications for YOUR user", false, cmd_notifyusermute,
    "Usage: notifyusermute [<kind,kind,...>|none]\n  Bare: show your muted kinds; none: clear\n  Applies only to the logged-in user (stored with your dashboard preferences)\n  List valid kinds with 'events kinds'" },
  { "notifyusershow",           "Force event kinds through YOUR importance floor", false, cmd_notifyusershow,
    "Usage: notifyusershow [<kind,kind,...>|none]\n  Bare: show your forced kinds; none: clear\n  Opposite of notifyusermute: these interrupt even below your notifylevel\n  List valid kinds with 'events kinds'" },
  { "notifylevel",              "Set YOUR notification importance floor",     false, cmd_notifylevel,
    "Usage: notifylevel [verbose|standard|alert]\n  Bare: show your current floor (default: standard)\n  verbose: everything; standard: skip routine chatter; alert: security/safety only\n  Nothing is lost - filtered kinds still reach the notification center and automations" },
};
const size_t settingEditorCommandsCount = sizeof(settingEditorCommands) / sizeof(settingEditorCommands[0]);

// ============================================================================
// Settings schema JSON — shared by the local /api/settings/schema web handler
// and the worker's sendBondSchema() (which serializes this to a file for
// transport across the bond). One source of truth means master and worker
// emit identical JSON shapes by construction.
// ============================================================================

// ============================================================================
// controls json — per-module control descriptor (metadata + CURRENT value)
// ============================================================================
// Drives the app's sensor/feature control panels: each entry carries the
// control type + range/options AND the live value, so sliders/steppers/selects/
// toggles render at their real position and two-way-bind. Set a value by
// sending `<key> <value>` — command matching is case-insensitive, so the
// camelCase jsonKey works directly as the command. Scoped to ONE module; the
// full settings surface across all modules would exceed the return/BLE buffer.
// Contract: docs/BLE_SENSOR_CONTROLS_CONTRACT.md.
static void buildModuleControlsJson(JsonDocument& doc, const char* moduleName) {
  doc["schema"] = 1;
  doc["module"] = moduleName;
  JsonArray entries = doc["entries"].to<JsonArray>();
  size_t modCount = 0;
  const SettingsModule** mods = getSettingsModules(modCount);
  for (size_t mi = 0; mi < modCount; mi++) {
    const SettingsModule* m = mods[mi];
    if (!m || !m->name || strcasecmp(m->name, moduleName) != 0) continue;
    if (m->description) doc["name"] = m->description;
    for (size_t i = 0; i < m->count; i++) {
      const SettingEntry* e = &m->entries[i];
      if (!e || !e->jsonKey || !e->valuePtr || e->isSecret) continue;  // never expose secrets
      JsonObject o = entries.add<JsonObject>();
      // "key" identifies the setting. It is NOT reliably the set command: the
      // command is cmdKey, and only ~20% of rows happen to have a jsonKey that
      // also resolves to a command that writes this setting (audit:
      // docs/CONTROLS_WRITE_INTEGRITY.md — 243/407 DEAD, 6 MISFIRE). A MISFIRE
      // is the dangerous case: the debug row jsonKey "capture" resolves to the
      // camera's `capture` verb, so toggling a log flag took a real photo.
      // Clients MUST send "cmd" and MUST NOT derive a command from "key".
      // Rows with no "cmd" are not settable and should render read-only.
      o["key"]   = e->jsonKey;
      if (e->cmdKey) o["cmd"] = e->cmdKey;
      o["label"] = e->label ? e->label : e->jsonKey;
      switch (e->type) {
        case SETTING_INT:    o["type"] = "int";    o["value"] = *(int*)e->valuePtr; break;
        case SETTING_U8:     o["type"] = "int";    o["value"] = (int)*(uint8_t*)e->valuePtr; break;
        case SETTING_U16:    o["type"] = "int";    o["value"] = (int)*(uint16_t*)e->valuePtr; break;
        case SETTING_U32:    o["type"] = "int";    o["value"] = (uint32_t)*(uint32_t*)e->valuePtr; break;
        case SETTING_FLOAT:  o["type"] = "float";  o["value"] = *(float*)e->valuePtr; break;
        case SETTING_BOOL:   o["type"] = "bool";   o["value"] = *(bool*)e->valuePtr; break;
        case SETTING_STRING: o["type"] = "string"; o["value"] = *(String*)e->valuePtr; break;
      }
      if (e->type != SETTING_BOOL && e->type != SETTING_STRING &&
          (e->minVal != 0 || e->maxVal != 0)) {
        o["min"] = e->minVal;
        o["max"] = e->maxVal;
      }
      if (e->options)  o["options"]  = e->options;   // comma-separated -> select
      if (e->readOnly) o["readOnly"] = true;
      if (e->group)    o["group"]    = e->group;
    }
    return;  // module found + emitted
  }
  doc["error"] = "unknown module";
}

// `controls json <module>` -> that module's controls (metadata + current value).
// `controls json` (no module) -> the list of available module names.
const char* cmd_controls(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsInput; args.trim();

  if (!argWantsJson(args)) {
    return "Error: invalid arguments — Usage: controls json <module>   (e.g. 'controls json imu')\n"
           "       controls json            (lists available modules)";
  }

  // Extract the module token (the first non-'json' token).
  String module = "";
  int start = 0;
  while (start < (int)args.length()) {
    int sp = args.indexOf(' ', start);
    if (sp < 0) sp = args.length();
    String tok = args.substring(start, sp);
    tok.trim();
    if (tok.length() && !tok.equalsIgnoreCase("json")) { module = tok; break; }
    start = sp + 1;
  }

  PSRAM_JSON_DOC(doc);
  if (module.length() == 0) {
    doc["schema"] = 1;
    JsonArray arr = doc["modules"].to<JsonArray>();
    size_t modCount = 0;
    const SettingsModule** mods = getSettingsModules(modCount);
    for (size_t mi = 0; mi < modCount; mi++) {
      if (mods[mi] && mods[mi]->name) arr.add(mods[mi]->name);
    }
  } else {
    buildModuleControlsJson(doc, module.c_str());
  }

  static char* controlsJsonBuf = nullptr;
  if (!controlsJsonBuf) controlsJsonBuf = (char*)ps_alloc(4096, AllocPref::PreferPSRAM, "controls.json");
  if (!controlsJsonBuf) return "{\"error\":\"oom\"}";
  serializeJson(doc, controlsJsonBuf, 4096);
  return controlsJsonBuf;
}

void buildSettingsSchemaJson(JsonDocument& doc) {
  JsonArray modules = doc["modules"].to<JsonArray>();

  size_t modCount = 0;
  const SettingsModule** mods = getSettingsModules(modCount);

  for (size_t m = 0; m < modCount; m++) {
    const SettingsModule* mod = mods[m];
    if (!mod) continue;

    JsonObject modObj = modules.add<JsonObject>();
    modObj["name"] = mod->name;
    modObj["section"] = mod->jsonSection ? mod->jsonSection : mod->name;
    modObj["description"] = mod->description ? mod->description : "";
    if (mod->isConnected) {
      modObj["connected"] = mod->isConnected();
    }

    JsonArray entries = modObj["entries"].to<JsonArray>();
    for (size_t i = 0; i < mod->count; i++) {
      const SettingEntry* e = &mod->entries[i];
      if (!e || !e->jsonKey) continue;

      JsonObject entry = entries.add<JsonObject>();
      entry["key"] = e->jsonKey;
      entry["label"] = e->label ? e->label : e->jsonKey;

      switch (e->type) {
        case SETTING_INT:
        case SETTING_U8:
        case SETTING_U16:
        case SETTING_U32:    entry["type"] = "int"; break;
        case SETTING_FLOAT:  entry["type"] = "float"; break;
        case SETTING_BOOL:   entry["type"] = "bool"; break;
        case SETTING_STRING: entry["type"] = "string"; break;
      }

      if (e->isSecret) {
        entry["secret"] = true;
        // Report WHETHER a value exists, without ever exposing it, so the web UI
        // can show "(set)" vs "(not set)". The secret value itself is never
        // serialized to the web.
        if (e->type == SETTING_STRING && e->valuePtr)
          entry["set"] = (((String*)e->valuePtr)->length() > 0);
      }
      if (e->readOnly) entry["readOnly"] = true;
      if (e->group)    entry["group"] = e->group;
      if (e->cmdKey) entry["cmdKey"] = e->cmdKey;

      if (e->type == SETTING_INT || e->type == SETTING_U8 ||
          e->type == SETTING_U16 || e->type == SETTING_U32 ||
          e->type == SETTING_FLOAT) {
        if (e->minVal != 0 || e->maxVal != 0) {
          entry["min"] = e->minVal;
          entry["max"] = e->maxVal;
        }
      }
      if (e->options) entry["options"] = e->options;

      switch (e->type) {
        case SETTING_INT:
        case SETTING_U8:
        case SETTING_U16:
        case SETTING_U32:    entry["default"] = e->intDefault; break;
        case SETTING_FLOAT:  entry["default"] = e->floatDefault; break;
        case SETTING_BOOL:   entry["default"] = (bool)e->intDefault; break;
        case SETTING_STRING: entry["default"] = e->stringDefault ? e->stringDefault : ""; break;
      }
    }
  }

  doc["count"] = modCount;
}

// ============================================================================
// Per-User Settings (merged from user_settings.cpp)
// ============================================================================

#include "System_Mutex.h"  // For FsLockGuard

// ============================================================================
// Batch write commands
// ============================================================================

const char* cmd_beginwrite(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  gDeferWrites = true;
  return "Write deferred — changes batched until savesettings";
}

const char* cmd_savesettings(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  gDeferWrites = false;
  writeSettingsJson();
  writeDebugJson();  // a batch may include debug flags — flush their file too
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
    if (!VFS::existsGuarded(path.c_str(), VFS::systemAuth("user_settings.load"))) {
      doc.to<JsonObject>();
      return true;
    }
    File f = VFS::openGuarded(path.c_str(), "r", VFS::systemAuth("user_settings.load"));
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
    File f = VFS::openGuarded(tmp.c_str(), "w", VFS::systemAuth("user_settings.save"));
    if (!f) return false;
    size_t written = serializeJson(doc, f);
    f.flush();
    f.close();
    if (written == 0) {
      VFS::removeGuarded(tmp.c_str(), VFS::systemAuth("user_settings.save"));
      return false;
    }

    // Atomic rename (LittleFS rename overwrites destination)
    if (!VFS::renameGuarded(tmp.c_str(), path.c_str(), VFS::systemAuth("user_settings.save"))) {
      // Rename failed; fallback to direct overwrite
      File direct = VFS::openGuarded(path.c_str(), "w", VFS::systemAuth("user_settings.save"));
      if (!direct) {
        VFS::removeGuarded(tmp.c_str(), VFS::systemAuth("user_settings.save"));
        return false;
      }
      written = serializeJson(doc, direct);
      direct.flush();
      direct.close();
      VFS::removeGuarded(tmp.c_str(), VFS::systemAuth("user_settings.save"));
      if (written > 0) notifUserPrefsInvalidate();
      return written > 0;
    }
  }

  // Any user-settings write may change notification prefs — flush the
  // per-user cache (covers web /api/user/settings, notifyusermute, password ops).
  notifUserPrefsInvalidate();
  return true;
}

bool mergeAndSaveUserSettings(uint32_t userId, const JsonDocument& patch) {
  if (!filesystemReady) return false;
  if (!patch.is<JsonObjectConst>()) return false;

  PSRAM_JSON_DOC(base);
  if (!loadUserSettings(userId, base)) return false;
  if (!base.is<JsonObject>()) base.to<JsonObject>();

  JsonObject dst = base.as<JsonObject>();
  for (JsonPairConst kv : patch.as<JsonObjectConst>()) {
    dst[kv.key()] = kv.value();
  }
  return saveUserSettings(userId, base);
}
