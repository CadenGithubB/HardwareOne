#include "System_Settings.h"        // Settings struct definition and function declarations
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
#include "BLE_Peers.h"       // bluetooth.peers JSON (de)serialization
#include "System_Command.h"
#include "System_Notifications.h"
#include "System_ESPSR.h"  // srSyncDebugLevel() — derive legacy gSrDebugLevel from flags
#include "System_SelfDevice.h"  // SelfDevice::firmwareVersion() — Stage 1 consolidation
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
  int v = -1;
  if (t1.length() && (t1 == "temp" || t1 == "persist")) {
    modeTemp = (t1 == "temp");
    if (t2.length()) v = t2.toInt();
  } else {
    if (t1.length()) v = t1.toInt();
    if (t2.length()) { modeTemp = (t2 == "temp"); }
  }
  if (v != 0) v = 1;
  if (v < 0) return "Error: invalid arguments — Usage: outserial <0|1> [persist|temp]";
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

const char* cmd_outweb(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String t1 = ca.arg(0);
  String t2 = ca.arg(1);
  bool modeTemp = false;
  int v = -1;
  if (t1.length() && (t1 == "temp" || t1 == "persist")) {
    modeTemp = (t1 == "temp");
    if (t2.length()) v = t2.toInt();
  } else {
    if (t1.length()) v = t1.toInt();
    if (t2.length()) { modeTemp = (t2 == "temp"); }
  }
  if (v != 0) v = 1;
  if (v < 0) return "Error: invalid arguments — Usage: outweb <0|1> [persist|temp]";
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
  { "wifiautoreconnect", "WiFi auto-reconnect: <0|1>", true, cmd_wifiautoreconnect, "Usage: wifiautoreconnect <0|1>" },
  
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
  { "outweb",             "Set web output: <0|1> [persist|temp]", true, cmd_outweb, "Usage: outweb <0|1> [persist|temp]" },
  { "serialrequireauth",  "Require auth for serial: <0|1>", true, cmd_serialrequireauth, "Usage: serialrequireauth <0|1>" },
  { "displayrequireauth", "Require auth for display: <0|1>", true, cmd_displayrequireauth, "Usage: displayrequireauth <0|1>" },

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
  // - output (System_Settings.cpp): outSerial, outWeb, outDisplay
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

  // Apply debug settings to runtime flags using table-driven loop.
  // Each entry maps a bool field in gSettings to the runtime debug flag it enables.
  // Multiple settings can map to the same flag (e.g. debugAuth and debugAuthCookies both → DEBUG_AUTH).
  struct DebugFlagMapping { size_t settingOffset; DebugFlagMask flag; };
  #define DBG_MAP(field, flag) { offsetof(Settings, field), flag }
  static const DebugFlagMapping kDebugMappings[] = {
    // Core debug flags
    DBG_MAP(debugAuth,             DEBUG_AUTH),
    DBG_MAP(debugAuthCookies,      DEBUG_AUTH),
    DBG_MAP(debugHttp,             DEBUG_HTTP),
    DBG_MAP(debugHttps,            DEBUG_HTTPS),
    DBG_MAP(debugSse,              DEBUG_SSE),
    DBG_MAP(debugCli,              DEBUG_CLI),
    DBG_MAP(debugCamera,           DEBUG_CAMERA),
    DBG_MAP(debugCameraLifecycle,  DEBUG_CAMERA_LIFECYCLE),
    DBG_MAP(debugCameraCapture,    DEBUG_CAMERA_CAPTURE),
    DBG_MAP(debugCameraSettings,   DEBUG_CAMERA_SETTINGS),
    DBG_MAP(debugCameraVideo,      DEBUG_CAMERA_VIDEO),
    DBG_MAP(debugDisplay,          DEBUG_DISPLAY),
    DBG_MAP(debugMicrophone,       DEBUG_MICROPHONE),
    DBG_MAP(debugWifi,             DEBUG_WIFI),
    DBG_MAP(debugStorage,          DEBUG_STORAGE),
    DBG_MAP(debugPerformance,      DEBUG_PERFORMANCE),
    DBG_MAP(debugDateTime,         DEBUG_NTP),
    DBG_MAP(debugCommandFlow,      DEBUG_CMD_FLOW),
    DBG_MAP(debugUsers,            DEBUG_USERS),
    DBG_MAP(debugSystem,           DEBUG_SYSTEM),
    DBG_MAP(debugAutomations,      DEBUG_AUTOMATIONS),
    DBG_MAP(debugLogger,           DEBUG_LOGGER),
    DBG_MAP(debugMemory,           DEBUG_MEMORY),
    DBG_MAP(debugMemoryHeap,       DEBUG_MEMORY_HEAP),
    DBG_MAP(debugMemoryStack,      DEBUG_MEMORY_STACK),
    DBG_MAP(debugMemoryBuffers,    DEBUG_MEMORY_BUFFERS),
    DBG_MAP(debugCommandSystem,    DEBUG_COMMAND_SYSTEM),
    DBG_MAP(debugBluetooth,        DEBUG_BLUETOOTH),
    DBG_MAP(debugBluetoothCore,    DEBUG_BLUETOOTH_CORE),
    DBG_MAP(debugBluetoothGatt,    DEBUG_BLUETOOTH_GATT),
    DBG_MAP(debugBluetoothData,    DEBUG_BLUETOOTH_DATA),
    DBG_MAP(debugG2Lifecycle,      DEBUG_G2_LIFECYCLE),
    DBG_MAP(debugG2Protocol,       DEBUG_G2_PROTOCOL),
    DBG_MAP(debugG2Events,         DEBUG_G2_EVENTS),
    DBG_MAP(debugG2Pages,          DEBUG_G2_PAGES),
    DBG_MAP(debugG2Heartbeat,      DEBUG_G2_HEARTBEAT),
    DBG_MAP(debugG2Dump,           DEBUG_G2_DUMP),
    DBG_MAP(debugEspNow,           DEBUG_ESPNOW_CORE),
    DBG_MAP(debugEspNowStream,     DEBUG_ESPNOW_STREAM),
    DBG_MAP(debugEspNowCore,       DEBUG_ESPNOW_CORE),
    DBG_MAP(debugEspNowRouter,     DEBUG_ESPNOW_ROUTER),
    DBG_MAP(debugEspNowMesh,       DEBUG_ESPNOW_MESH),
    DBG_MAP(debugEspNowTopo,       DEBUG_ESPNOW_TOPO),
    DBG_MAP(debugEspNowEncryption, DEBUG_ESPNOW_ENCRYPTION),
    DBG_MAP(debugEspNowMetadata,   DEBUG_ESPNOW_METADATA),
    DBG_MAP(debugAutoScheduler,    DEBUG_AUTO_SCHEDULER),
    DBG_MAP(debugAutoExec,         DEBUG_AUTO_EXEC),
    DBG_MAP(debugAutoCondition,    DEBUG_AUTO_CONDITION),
    DBG_MAP(debugAutoTiming,       DEBUG_AUTO_TIMING),
    DBG_MAP(debugFmRadio,          DEBUG_FMRADIO),
    DBG_MAP(debugG2,               DEBUG_G2),
    DBG_MAP(debugI2C,              DEBUG_I2C),
    DBG_MAP(debugI2CBus,           DEBUG_I2C_BUS),
    DBG_MAP(debugI2CDiscovery,     DEBUG_I2C_DISCOVERY),
    DBG_MAP(debugI2CAutoStart,     DEBUG_I2C_AUTOSTART),
    // Per-sensor sub-flags (Lifecycle / Polling / Values)
    DBG_MAP(debugThermalLifecycle,   DEBUG_THERMAL_LIFECYCLE),
    DBG_MAP(debugThermalPolling,     DEBUG_THERMAL_POLLING),
    DBG_MAP(debugThermalValues,      DEBUG_THERMAL_VALUES),
    DBG_MAP(debugTofLifecycle,       DEBUG_TOF_LIFECYCLE),
    DBG_MAP(debugTofPolling,         DEBUG_TOF_POLLING),
    DBG_MAP(debugTofValues,          DEBUG_TOF_VALUES),
    DBG_MAP(debugInputLifecycle,   DEBUG_INPUT_LIFECYCLE),
    DBG_MAP(debugInputPolling,     DEBUG_INPUT_POLLING),
    DBG_MAP(debugInputValues,      DEBUG_INPUT_VALUES),
    DBG_MAP(debugImuLifecycle,       DEBUG_IMU_LIFECYCLE),
    DBG_MAP(debugImuPolling,         DEBUG_IMU_POLLING),
    DBG_MAP(debugImuValues,          DEBUG_IMU_VALUES),
    DBG_MAP(debugApdsLifecycle,      DEBUG_APDS_LIFECYCLE),
    DBG_MAP(debugApdsPolling,        DEBUG_APDS_POLLING),
    DBG_MAP(debugApdsValues,         DEBUG_APDS_VALUES),
    DBG_MAP(debugGpsLifecycle,       DEBUG_GPS_LIFECYCLE),
    DBG_MAP(debugGpsPolling,         DEBUG_GPS_POLLING),
    DBG_MAP(debugGpsValues,          DEBUG_GPS_VALUES),
    DBG_MAP(debugRtcLifecycle,       DEBUG_RTC_LIFECYCLE),
    DBG_MAP(debugRtcPolling,         DEBUG_RTC_POLLING),
    DBG_MAP(debugRtcValues,          DEBUG_RTC_VALUES),
    DBG_MAP(debugFmRadioLifecycle,   DEBUG_FMRADIO_LIFECYCLE),
    DBG_MAP(debugFmRadioPolling,     DEBUG_FMRADIO_POLLING),
    DBG_MAP(debugFmRadioValues,      DEBUG_FMRADIO_VALUES),
    DBG_MAP(debugMicLifecycle,       DEBUG_MIC_LIFECYCLE),
    DBG_MAP(debugMicPolling,         DEBUG_MIC_POLLING),
    DBG_MAP(debugMicValues,          DEBUG_MIC_VALUES),
    DBG_MAP(debugPresenceLifecycle,  DEBUG_PRESENCE_LIFECYCLE),
    DBG_MAP(debugPresencePolling,    DEBUG_PRESENCE_POLLING),
    DBG_MAP(debugPresenceValues,     DEBUG_PRESENCE_VALUES),
    // Maps flags
    DBG_MAP(debugMaps,             DEBUG_MAPS),
    DBG_MAP(debugMapsLoading,      DEBUG_MAPS_LOADING),
    DBG_MAP(debugMapsRendering,    DEBUG_MAPS_RENDERING),
    DBG_MAP(debugMapsPerf,         DEBUG_MAPS_PERF),
#if ENABLE_ONDEVICE_LLM
    DBG_MAP(debugLlm,              DEBUG_LLM),
    DBG_MAP(debugLlmLoad,          DEBUG_LLM_LOAD),
    DBG_MAP(debugLlmTokenizer,     DEBUG_LLM_TOKENIZER),
    DBG_MAP(debugLlmForward,       DEBUG_LLM_FORWARD),
    DBG_MAP(debugLlmGenerate,      DEBUG_LLM_GENERATE),
    DBG_MAP(debugLlmMemory,        DEBUG_LLM_MEMORY),
#endif
    // ESP-SR (parent + 5 sub-flags). Wired the same way as G2 — sub-flags
    // are independent; parent debugSr is the explicit master switch.
    DBG_MAP(debugSr,               DEBUG_SR),
    DBG_MAP(debugSrWake,           DEBUG_SR_WAKE),
    DBG_MAP(debugSrCommand,        DEBUG_SR_COMMAND),
    DBG_MAP(debugSrAfe,            DEBUG_SR_AFE),
    DBG_MAP(debugSrLifecycle,      DEBUG_SR_LIFECYCLE),
    DBG_MAP(debugSrTuning,         DEBUG_SR_TUNING),
    DBG_MAP(debugMqtt,             DEBUG_MQTT),
    DBG_MAP(debugMqttConnection,   DEBUG_MQTT_CONNECTION),
    DBG_MAP(debugMqttPubsub,       DEBUG_MQTT_PUBSUB),
    DBG_MAP(debugMqttDiscovery,    DEBUG_MQTT_DISCOVERY),
    DBG_MAP(debugMqttCommands,     DEBUG_MQTT_COMMANDS),
  };
  #undef DBG_MAP

  setDebugFlags(0);  // Start with no flags, then enable based on settings
  const uint8_t* base = reinterpret_cast<const uint8_t*>(&gSettings);
  for (const auto& m : kDebugMappings) {
    if (*reinterpret_cast<const bool*>(base + m.settingOffset)) {
      setDebugFlag(m.flag);
    }
  }

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
  gDebugSubFlags.storagePermissions = gSettings.debugStoragePermissions;
  updateParentDebugFlag(DEBUG_STORAGE, gSettings.debugStorage || gDebugSubFlags.storageFiles || gDebugSubFlags.storageJson || gDebugSubFlags.storageSettings || gDebugSubFlags.storageMigration || gDebugSubFlags.storagePermissions);
  
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

  // NTP / DateTime sub-flags
  gDebugSubFlags.ntpSync    = gSettings.debugDatetimeSync;
  gDebugSubFlags.ntpSetup   = gSettings.debugDatetimeSetup;
  gDebugSubFlags.ntpAnchor  = gSettings.debugDatetimeAnchor;
  gDebugSubFlags.ntpResolve = gSettings.debugDatetimeResolve;
  updateParentDebugFlag(DEBUG_NTP, gSettings.debugDateTime || gDebugSubFlags.ntpSync || gDebugSubFlags.ntpSetup || gDebugSubFlags.ntpAnchor || gDebugSubFlags.ntpResolve);

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

#if ENABLE_ONDEVICE_LLM
  updateParentDebugFlag(DEBUG_LLM,
                        gSettings.debugLlm ||
                        gSettings.debugLlmLoad ||
                        gSettings.debugLlmTokenizer ||
                        gSettings.debugLlmForward ||
                        gSettings.debugLlmGenerate ||
                        gSettings.debugLlmMemory);
#endif

  // Bluetooth parent flag
  updateParentDebugFlag(DEBUG_BLUETOOTH,
                        gSettings.debugBluetooth ||
                        gSettings.debugBluetoothCore ||
                        gSettings.debugBluetoothGatt ||
                        gSettings.debugBluetoothData);

  // ESP-SR sub-flags + parent. Mirror to gDebugSubFlags so System_ESPSR
  // can read them without re-touching gSettings on every audio frame.
  gDebugSubFlags.srWake      = gSettings.debugSrWake;
  gDebugSubFlags.srCommand   = gSettings.debugSrCommand;
  gDebugSubFlags.srAfe       = gSettings.debugSrAfe;
  gDebugSubFlags.srLifecycle = gSettings.debugSrLifecycle;
  gDebugSubFlags.srTuning    = gSettings.debugSrTuning;
  updateParentDebugFlag(DEBUG_SR,
                        gSettings.debugSr ||
                        gDebugSubFlags.srWake ||
                        gDebugSubFlags.srCommand ||
                        gDebugSubFlags.srAfe ||
                        gDebugSubFlags.srLifecycle ||
                        gDebugSubFlags.srTuning);
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

void buildSettingsJsonDoc(JsonDocument& doc, bool excludePasswords) {
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

  // NOTE: ntpServer, tzOffsetMinutes, wifiSSID, wifiPassword, wifiAutoReconnect
  //       are owned by the wifi module (written under "wifi" section).
  // NOTE: automationsEnabled is owned by the automation module.
  // NOTE: power{} is owned by the power module.

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
  if (gWifiNetworks && gWifiNetworkCount > 0) {
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
  
  // Now build/overwrite with current settings (orphaned sections remain untouched)
  buildSettingsJsonDoc(doc);

  // Remove runtime-only fields that must never be persisted to disk
  doc.remove("wifiPrimarySSID");

  // Check for overflow
  if (doc.overflowed()) {
    ERROR_STORAGEF("JSON document overflowed during build (need more than 5120 bytes)");
    logSystemEvent("SETTINGS", "save FAILED (JSON build overflow) — settings NOT persisted");
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

  // Apply settings from registered modules first (handles defaults automatically)
  size_t registeredCount = readRegisteredSettings(doc);
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
  setSetting(gSettings.espnowenabled, enabled);
  snprintf(getDebugBuffer(), 1024, "espnowenabled set to %s (takes effect after reboot)", enabled ? "1" : "0");
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
static const SettingEntry debugSettingEntries[] = {
  // --- authentication group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugAuth,          0, 0, nullptr, 0, 1, "All Authentication",  nullptr, false, "authentication", "debugauth" },
  { "sessions",   SETTING_BOOL, &gSettings.debugAuthSessions,  0, 0, nullptr, 0, 1, "Sessions",            nullptr, false, "authentication", "debugauthsessions" },
  { "cookies",    SETTING_BOOL, &gSettings.debugAuthCookies,   0, 0, nullptr, 0, 1, "Cookies",             nullptr, false, "authentication", "debugauthcookies" },
  { "login",      SETTING_BOOL, &gSettings.debugAuthLogin,     0, 0, nullptr, 0, 1, "Login",               nullptr, false, "authentication", "debugauthlogin" },
  { "bootId",     SETTING_BOOL, &gSettings.debugAuthBootId,    0, 0, nullptr, 0, 1, "Boot ID",             nullptr, false, "authentication", "debugauthbootid" },
  // --- http group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugHttp,          0, 0, nullptr, 0, 1, "All HTTP",            nullptr, false, "http", "debughttp" },
  { "handlers",   SETTING_BOOL, &gSettings.debugHttpHandlers,  0, 0, nullptr, 0, 1, "Handlers",            nullptr, false, "http", "debughttphandlers" },
  { "requests",   SETTING_BOOL, &gSettings.debugHttpRequests,  0, 0, nullptr, 0, 1, "Requests",            nullptr, false, "http", "debughttprequests" },
  { "responses",  SETTING_BOOL, &gSettings.debugHttpResponses, 0, 0, nullptr, 0, 1, "Responses",           nullptr, false, "http", "debughttpresponses" },
  { "streaming",  SETTING_BOOL, &gSettings.debugHttpStreaming,  0, 0, nullptr, 0, 1, "Streaming",           nullptr, false, "http", "debughttpstreaming" },
  // --- https group (TLS handshake + connection-error noise from ESP-IDF) ---
  { "enabled",    SETTING_BOOL, &gSettings.debugHttps,         0, 0, nullptr, 0, 1, "All HTTPS/TLS",       nullptr, false, "https", "debughttps" },
  // --- sse group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugSse,           0, 0, nullptr, 0, 1, "All SSE",             nullptr, false, "sse", "debugsse" },
  { "connection", SETTING_BOOL, &gSettings.debugSseConnection, 0, 0, nullptr, 0, 1, "Connection",          nullptr, false, "sse", "debugsseconnection" },
  { "events",     SETTING_BOOL, &gSettings.debugSseEvents,     0, 0, nullptr, 0, 1, "Events",              nullptr, false, "sse", "debugsseevents" },
  { "broadcast",  SETTING_BOOL, &gSettings.debugSseBroadcast,  0, 0, nullptr, 0, 1, "Broadcast",           nullptr, false, "sse", "debugssebroadcast" },
  // --- wifi group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugWifi,           0, 0, nullptr, 0, 1, "All WiFi",            nullptr, false, "wifi", "debugwifi" },
  { "connection", SETTING_BOOL, &gSettings.debugWifiConnection, 0, 0, nullptr, 0, 1, "Connection",          nullptr, false, "wifi", "debugwificonnection" },
  { "config",     SETTING_BOOL, &gSettings.debugWifiConfig,     0, 0, nullptr, 0, 1, "Config",              nullptr, false, "wifi", "debugwificonfig" },
  { "scanning",   SETTING_BOOL, &gSettings.debugWifiScanning,   0, 0, nullptr, 0, 1, "Scanning",            nullptr, false, "wifi", "debugwifiscanning" },
  { "driver",     SETTING_BOOL, &gSettings.debugWifiDriver,     0, 0, nullptr, 0, 1, "Driver",              nullptr, false, "wifi", "debugwifidriver" },
  // --- storage group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugStorage,          0, 0, nullptr, 0, 1, "All Storage",       nullptr, false, "storage", "debugstorage" },
  { "files",       SETTING_BOOL, &gSettings.debugStorageFiles,       0, 0, nullptr, 0, 1, "Files",       nullptr, false, "storage", "debugstoragefiles" },
  { "json",        SETTING_BOOL, &gSettings.debugStorageJson,        0, 0, nullptr, 0, 1, "JSON",        nullptr, false, "storage", "debugstoragejson" },
  { "settings",    SETTING_BOOL, &gSettings.debugStorageSettings,    0, 0, nullptr, 0, 1, "Settings",    nullptr, false, "storage", "debugstoragesettings" },
  { "migration",   SETTING_BOOL, &gSettings.debugStorageMigration,   0, 0, nullptr, 0, 1, "Migration",   nullptr, false, "storage", "debugstoragemigration" },
  { "permissions", SETTING_BOOL, &gSettings.debugStoragePermissions, 0, 0, nullptr, 0, 1, "Permissions", nullptr, false, "storage", "debugstoragepermissions" },
  // --- esp-now group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugEspNow,           0, 0, nullptr, 0, 1, "All ESP-NOW",       nullptr, false, "esp-now", "debugespnow" },
  { "stream",     SETTING_BOOL, &gSettings.debugEspNowStream,     0, 0, nullptr, 0, 1, "Stream",            nullptr, false, "esp-now", "debugespnowstream" },
  { "core",       SETTING_BOOL, &gSettings.debugEspNowCore,       0, 0, nullptr, 0, 1, "Core",              nullptr, false, "esp-now", "debugespnowcore" },
  { "router",     SETTING_BOOL, &gSettings.debugEspNowRouter,     0, 0, nullptr, 0, 1, "Router",            nullptr, false, "esp-now", "debugespnowrouter" },
  { "mesh",       SETTING_BOOL, &gSettings.debugEspNowMesh,       0, 0, nullptr, 0, 1, "Mesh",              nullptr, false, "esp-now", "debugespnowmesh" },
  { "topology",   SETTING_BOOL, &gSettings.debugEspNowTopo,       0, 0, nullptr, 0, 1, "Topology",          nullptr, false, "esp-now", "debugespnowtopo" },
  { "encryption", SETTING_BOOL, &gSettings.debugEspNowEncryption, 0, 0, nullptr, 0, 1, "Encryption",        nullptr, false, "esp-now", "debugespnowencryption" },
  { "metadata",   SETTING_BOOL, &gSettings.debugEspNowMetadata,   0, 0, nullptr, 0, 1, "Metadata",          nullptr, false, "esp-now", "debugespnowmetadata" },
  // --- bluetooth group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugBluetooth,        0, 0, nullptr, 0, 1, "All Bluetooth",     nullptr, false, "bluetooth", "debugbluetooth" },
  { "core",       SETTING_BOOL, &gSettings.debugBluetoothCore,    0, 0, nullptr, 0, 1, "Core",              nullptr, false, "bluetooth", "debugbluetoothcore" },
  { "gatt",       SETTING_BOOL, &gSettings.debugBluetoothGatt,    0, 0, nullptr, 0, 1, "GATT",              nullptr, false, "bluetooth", "debugbluetoothgatt" },
  { "data",       SETTING_BOOL, &gSettings.debugBluetoothData,    0, 0, nullptr, 0, 1, "Data",              nullptr, false, "bluetooth", "debugbluetoothdata" },
  // --- system group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugSystem,         0, 0, nullptr, 0, 1, "All System",          nullptr, false, "system", "debugsystem" },
  { "boot",       SETTING_BOOL, &gSettings.debugSystemBoot,     0, 0, nullptr, 0, 1, "Boot",                nullptr, false, "system", "debugsystemboot" },
  { "config",     SETTING_BOOL, &gSettings.debugSystemConfig,   0, 0, nullptr, 0, 1, "Config",              nullptr, false, "system", "debugsystemconfig" },
  { "tasks",      SETTING_BOOL, &gSettings.debugSystemTasks,    0, 0, nullptr, 0, 1, "Tasks",               nullptr, false, "system", "debugsystemtasks" },
  { "hardware",   SETTING_BOOL, &gSettings.debugSystemHardware, 0, 0, nullptr, 0, 1, "Hardware",            nullptr, false, "system", "debugsystemhardware" },
  // --- users group ---
  { "enabled",      SETTING_BOOL, &gSettings.debugUsers,         0, 0, nullptr, 0, 1, "All Users",           nullptr, false, "users", "debugusers" },
  { "management",   SETTING_BOOL, &gSettings.debugUsersMgmt,     0, 0, nullptr, 0, 1, "Management",          nullptr, false, "users", "debugusersmgmt" },
  { "registration", SETTING_BOOL, &gSettings.debugUsersRegister, 0, 0, nullptr, 0, 1, "Registration",        nullptr, false, "users", "debugusersregister" },
  { "query",        SETTING_BOOL, &gSettings.debugUsersQuery,    0, 0, nullptr, 0, 1, "Query",               nullptr, false, "users", "debugusersquery" },
  // --- cli group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugCli,            0, 0, nullptr, 0, 1, "All CLI",             nullptr, false, "cli", "debugcli" },
  { "execution",  SETTING_BOOL, &gSettings.debugCliExecution,   0, 0, nullptr, 0, 1, "Execution",           nullptr, false, "cli", "debugcliexecution" },
  { "queue",      SETTING_BOOL, &gSettings.debugCliQueue,       0, 0, nullptr, 0, 1, "Queue",               nullptr, false, "cli", "debugcliqueue" },
  { "validation", SETTING_BOOL, &gSettings.debugCliValidation,  0, 0, nullptr, 0, 1, "Validation",          nullptr, false, "cli", "debugclivalidation" },
  // --- commands group (merged command-flow + command system) ---
  { "enabled",    SETTING_BOOL, &gSettings.debugCommandFlow,     0, 0, nullptr, 0, 1, "All Commands",        nullptr, false, "commands", "debugcommandflow" },
  { "system",     SETTING_BOOL, &gSettings.debugCommandSystem,   0, 0, nullptr, 0, 1, "System",              nullptr, false, "commands", "debugcommandsystem" },
  { "routing",    SETTING_BOOL, &gSettings.debugCmdflowRouting,  0, 0, nullptr, 0, 1, "Routing",             nullptr, false, "commands", "debugcmdflowrouting" },
  { "queue",      SETTING_BOOL, &gSettings.debugCmdflowQueue,   0, 0, nullptr, 0, 1, "Queue",               nullptr, false, "commands", "debugcmdflowqueue" },
  { "context",    SETTING_BOOL, &gSettings.debugCmdflowContext,  0, 0, nullptr, 0, 1, "Context",             nullptr, false, "commands", "debugcmdflowcontext" },
  // --- performance group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugPerformance,    0, 0, nullptr, 0, 1, "All Performance",     nullptr, false, "performance", "debugperformance" },
  { "stack",      SETTING_BOOL, &gSettings.debugPerfStack,      0, 0, nullptr, 0, 1, "Stack",               nullptr, false, "performance", "debugperfstack" },
  { "heap",       SETTING_BOOL, &gSettings.debugPerfHeap,       0, 0, nullptr, 0, 1, "Heap",                nullptr, false, "performance", "debugperfheap" },
  { "timing",     SETTING_BOOL, &gSettings.debugPerfTiming,     0, 0, nullptr, 0, 1, "Timing",              nullptr, false, "performance", "debugperftiming" },
  // --- automations group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugAutomations,    0, 0, nullptr, 0, 1, "All Automations",     nullptr, false, "automations", "debugautomations" },
  { "scheduler",  SETTING_BOOL, &gSettings.debugAutoScheduler,  0, 0, nullptr, 0, 1, "Scheduler",           nullptr, false, "automations", "debugautoscheduler" },
  { "execution",  SETTING_BOOL, &gSettings.debugAutoExec,       0, 0, nullptr, 0, 1, "Execution",           nullptr, false, "automations", "debugautoexec" },
  { "condition",  SETTING_BOOL, &gSettings.debugAutoCondition,  0, 0, nullptr, 0, 1, "Condition",           nullptr, false, "automations", "debugautocondition" },
  { "timing",     SETTING_BOOL, &gSettings.debugAutoTiming,     0, 0, nullptr, 0, 1, "Timing",              nullptr, false, "automations", "debugautotiming" },
  // --- per-sensor groups (each sensor gets its own card in the debug UI) ---
  { "enabled",    SETTING_BOOL, &gSettings.debugCamera,          0, 0, nullptr, 0, 1, "All Camera",          nullptr, false, "camera",      "debugcamera" },
  { "lifecycle",  SETTING_BOOL, &gSettings.debugCameraLifecycle, 0, 0, nullptr, 0, 1, "Lifecycle",           nullptr, false, "camera",      "debugcameralifecycle" },
  { "capture",    SETTING_BOOL, &gSettings.debugCameraCapture,   0, 0, nullptr, 0, 1, "Capture",             nullptr, false, "camera",      "debugcameracapture" },
  { "settings",   SETTING_BOOL, &gSettings.debugCameraSettings,  0, 0, nullptr, 0, 1, "Settings",            nullptr, false, "camera",      "debugcamerasettings" },
  { "video",      SETTING_BOOL, &gSettings.debugCameraVideo,     0, 0, nullptr, 0, 1, "Video",               nullptr, false, "camera",      "debugcameravideo" },
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
  { "enabled",    SETTING_BOOL, &gSettings.debugDisplay,         0, 0, nullptr, 0, 1, "All OLED",            nullptr, false, "oled",        "debugdisplay" },
  // --- microphone group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugMicrophone,      0, 0, nullptr, 0, 1, "All Microphone",      nullptr, false, "microphone",  "debugmicrophone" },
  { "lifecycle",  SETTING_BOOL, &gSettings.debugMicLifecycle,    0, 0, nullptr, 0, 1, "Lifecycle",           nullptr, false, "microphone",  "debugmiclifecycle" },
  { "polling",    SETTING_BOOL, &gSettings.debugMicPolling,      0, 0, nullptr, 0, 1, "Polling",             nullptr, false, "microphone",  "debugmicpolling" },
  { "values",     SETTING_BOOL, &gSettings.debugMicValues,       0, 0, nullptr, 0, 1, "Values",              nullptr, false, "microphone",  "debugmicvalues" },
  // --- gps group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugGps,             0, 0, nullptr, 0, 1, "All GPS",             nullptr, false, "gps",         "debuggps" },
  { "lifecycle",  SETTING_BOOL, &gSettings.debugGpsLifecycle,    0, 0, nullptr, 0, 1, "Lifecycle",           nullptr, false, "gps",         "debuggpslifecycle" },
  { "polling",    SETTING_BOOL, &gSettings.debugGpsPolling,      0, 0, nullptr, 0, 1, "Polling",             nullptr, false, "gps",         "debuggpspolling" },
  { "values",     SETTING_BOOL, &gSettings.debugGpsValues,       0, 0, nullptr, 0, 1, "Values",              nullptr, false, "gps",         "debuggpsvalues" },
  // --- rtc group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugRtc,             0, 0, nullptr, 0, 1, "All RTC",             nullptr, false, "rtc",         "debugrtc" },
  { "lifecycle",  SETTING_BOOL, &gSettings.debugRtcLifecycle,    0, 0, nullptr, 0, 1, "Lifecycle",           nullptr, false, "rtc",         "debugrtclifecycle" },
  { "polling",    SETTING_BOOL, &gSettings.debugRtcPolling,      0, 0, nullptr, 0, 1, "Polling",             nullptr, false, "rtc",         "debugrtcpolling" },
  { "values",     SETTING_BOOL, &gSettings.debugRtcValues,       0, 0, nullptr, 0, 1, "Values",              nullptr, false, "rtc",         "debugrtcvalues" },
  // --- presence group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugPresence,        0, 0, nullptr, 0, 1, "All Presence",        nullptr, false, "presence",    "debugpresence" },
  { "lifecycle",  SETTING_BOOL, &gSettings.debugPresenceLifecycle, 0, 0, nullptr, 0, 1, "Lifecycle",         nullptr, false, "presence",    "debugpresencelifecycle" },
  { "polling",    SETTING_BOOL, &gSettings.debugPresencePolling, 0, 0, nullptr, 0, 1, "Polling",             nullptr, false, "presence",    "debugpresencepolling" },
  { "values",     SETTING_BOOL, &gSettings.debugPresenceValues,  0, 0, nullptr, 0, 1, "Values",              nullptr, false, "presence",    "debugpresencevalues" },
  // --- fm radio group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugFmRadio,         0, 0, nullptr, 0, 1, "All FM Radio",        nullptr, false, "fmradio",     "debugfmradio" },
  { "lifecycle",  SETTING_BOOL, &gSettings.debugFmRadioLifecycle, 0, 0, nullptr, 0, 1, "Lifecycle",          nullptr, false, "fmradio",     "debugfmradiolifecycle" },
  { "polling",    SETTING_BOOL, &gSettings.debugFmRadioPolling,  0, 0, nullptr, 0, 1, "Polling",             nullptr, false, "fmradio",     "debugfmradiopolling" },
  { "values",     SETTING_BOOL, &gSettings.debugFmRadioValues,   0, 0, nullptr, 0, 1, "Values",              nullptr, false, "fmradio",     "debugfmradiovalues" },
  // --- thermal group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugThermal,         0, 0, nullptr, 0, 1, "All Thermal",         nullptr, false, "thermal",     "debugthermal" },
  { "lifecycle",  SETTING_BOOL, &gSettings.debugThermalLifecycle, 0, 0, nullptr, 0, 1, "Lifecycle",          nullptr, false, "thermal",     "debugthermallifecycle" },
  { "polling",    SETTING_BOOL, &gSettings.debugThermalPolling,  0, 0, nullptr, 0, 1, "Polling",             nullptr, false, "thermal",     "debugthermalpolling" },
  { "values",     SETTING_BOOL, &gSettings.debugThermalValues,   0, 0, nullptr, 0, 1, "Values",              nullptr, false, "thermal",     "debugthermalvalues" },
  // --- imu group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugImu,             0, 0, nullptr, 0, 1, "All IMU",             nullptr, false, "imu",         "debugimu" },
  { "lifecycle",  SETTING_BOOL, &gSettings.debugImuLifecycle,    0, 0, nullptr, 0, 1, "Lifecycle",           nullptr, false, "imu",         "debugimulifecycle" },
  { "polling",    SETTING_BOOL, &gSettings.debugImuPolling,      0, 0, nullptr, 0, 1, "Polling",             nullptr, false, "imu",         "debugimupolling" },
  { "values",     SETTING_BOOL, &gSettings.debugImuValues,       0, 0, nullptr, 0, 1, "Values",              nullptr, false, "imu",         "debugimuvalues" },
  // --- input abstraction group (HAL_Input + OLED input dispatch) ---
  { "enabled",    SETTING_BOOL, &gSettings.debugInput,          0, 0, nullptr, 0, 1, "All Input",           nullptr, false, "input",       "debuginput" },
  { "lifecycle",  SETTING_BOOL, &gSettings.debugInputLifecycle, 0, 0, nullptr, 0, 1, "Lifecycle",           nullptr, false, "input",       "debuginputlifecycle" },
  { "polling",    SETTING_BOOL, &gSettings.debugInputPolling,   0, 0, nullptr, 0, 1, "Polling",             nullptr, false, "input",       "debuginputpolling" },
  { "values",     SETTING_BOOL, &gSettings.debugInputValues,    0, 0, nullptr, 0, 1, "Values",              nullptr, false, "input",       "debuginputvalues" },
  // --- ANO encoder driver-specific group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugAnoEncoder,          0, 0, nullptr, 0, 1, "All ANO Encoder", nullptr, false, "anoencoder",  "debuganoencoder" },
  { "lifecycle",  SETTING_BOOL, &gSettings.debugAnoEncoderLifecycle, 0, 0, nullptr, 0, 1, "Lifecycle",       nullptr, false, "anoencoder",  "debuganoencoderlifecycle" },
  { "polling",    SETTING_BOOL, &gSettings.debugAnoEncoderPolling,   0, 0, nullptr, 0, 1, "Polling",         nullptr, false, "anoencoder",  "debuganoencoderpolling" },
  { "values",     SETTING_BOOL, &gSettings.debugAnoEncoderValues,    0, 0, nullptr, 0, 1, "Values",          nullptr, false, "anoencoder",  "debuganoencodervalues" },
  // --- tof group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugTof,             0, 0, nullptr, 0, 1, "All ToF",             nullptr, false, "tof",         "debugtof" },
  { "lifecycle",  SETTING_BOOL, &gSettings.debugTofLifecycle,    0, 0, nullptr, 0, 1, "Lifecycle",           nullptr, false, "tof",         "debugtoflifecycle" },
  { "polling",    SETTING_BOOL, &gSettings.debugTofPolling,      0, 0, nullptr, 0, 1, "Polling",             nullptr, false, "tof",         "debugtofpolling" },
  { "values",     SETTING_BOOL, &gSettings.debugTofValues,       0, 0, nullptr, 0, 1, "Values",              nullptr, false, "tof",         "debugtofvalues" },
  // --- apds group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugApds,            0, 0, nullptr, 0, 1, "All APDS",            nullptr, false, "apds",        "debugapds" },
  { "lifecycle",  SETTING_BOOL, &gSettings.debugApdsLifecycle,   0, 0, nullptr, 0, 1, "Lifecycle",           nullptr, false, "apds",        "debugapdslifecycle" },
  { "polling",    SETTING_BOOL, &gSettings.debugApdsPolling,     0, 0, nullptr, 0, 1, "Polling",             nullptr, false, "apds",        "debugapdspolling" },
  { "values",     SETTING_BOOL, &gSettings.debugApdsValues,      0, 0, nullptr, 0, 1, "Values",              nullptr, false, "apds",        "debugapdsvalues" },
  // --- maps group ---
  { "enabled",    SETTING_BOOL, &gSettings.debugMaps,            0, 0, nullptr, 0, 1, "All Maps",            nullptr, false, "maps", "debugmaps" },
  { "loading",    SETTING_BOOL, &gSettings.debugMapsLoading,     0, 0, nullptr, 0, 1, "Loading",             nullptr, false, "maps", "debugmapsloading" },
  { "rendering",  SETTING_BOOL, &gSettings.debugMapsRendering,   0, 0, nullptr, 0, 1, "Rendering",           nullptr, false, "maps", "debugmapsrendering" },
  { "perf",       SETTING_BOOL, &gSettings.debugMapsPerf,       0, 0, nullptr, 0, 1, "Performance",         nullptr, false, "maps", "debugmapsperf" },
#if ENABLE_ONDEVICE_LLM
  // --- llm group (on-device LLM) ---
  { "enabled",    SETTING_BOOL, &gSettings.debugLlm,             0, 0, nullptr, 0, 1, "All LLM",             nullptr, false, "llm", "debugllm" },
  { "load",       SETTING_BOOL, &gSettings.debugLlmLoad,         0, 0, nullptr, 0, 1, "Load / checkpoint",   nullptr, false, "llm", "debugllmload" },
  { "tokenizer",  SETTING_BOOL, &gSettings.debugLlmTokenizer,  0, 0, nullptr, 0, 1, "Tokenizer",           nullptr, false, "llm", "debugllmtokenizer" },
  { "forward",    SETTING_BOOL, &gSettings.debugLlmForward,      0, 0, nullptr, 0, 1, "Forward",             nullptr, false, "llm", "debugllmforward" },
  { "generate",   SETTING_BOOL, &gSettings.debugLlmGenerate,     0, 0, nullptr, 0, 1, "Generate",            nullptr, false, "llm", "debugllmgenerate" },
  { "memory",     SETTING_BOOL, &gSettings.debugLlmMemory,       0, 0, nullptr, 0, 1, "Memory / PSRAM",      nullptr, false, "llm", "debugllmmemory" },
#endif
  // --- NTP / DateTime group ---
  { "enabled",   SETTING_BOOL, &gSettings.debugDateTime,       0, 0, nullptr, 0, 1, "All NTP/DateTime",      nullptr, false, "datetime", "debugdatetime" },
  { "sync",      SETTING_BOOL, &gSettings.debugDatetimeSync,   0, 0, nullptr, 0, 1, "Sync loop",             nullptr, false, "datetime", "debugdatetimesync" },
  { "setup",     SETTING_BOOL, &gSettings.debugDatetimeSetup,  0, 0, nullptr, 0, 1, "Setup/configTime",      nullptr, false, "datetime", "debugdatetimesetup" },
  { "anchor",    SETTING_BOOL, &gSettings.debugDatetimeAnchor, 0, 0, nullptr, 0, 1, "Boot anchors",          nullptr, false, "datetime", "debugdatetimeanchor" },
  { "resolve",   SETTING_BOOL, &gSettings.debugDatetimeResolve,0, 0, nullptr, 0, 1, "Timestamp resolution",  nullptr, false, "datetime", "debugdatetimeresolve" },
  // --- standalone (no group) ---
  { "enabled",    SETTING_BOOL, &gSettings.debugLogger,         0, 0, nullptr, 0, 1, "Enabled",     nullptr, false, "logger", "debuglogger" },
  { "enabled",    SETTING_BOOL, &gSettings.debugMemory,         0, 0, nullptr, 0, 1, "All Memory",  nullptr, false, "memory", "debugmemory" },
  { "heap",       SETTING_BOOL, &gSettings.debugMemoryHeap,     0, 0, nullptr, 0, 1, "Heap",        nullptr, false, "memory", "debugmemoryheap" },
  { "stack",      SETTING_BOOL, &gSettings.debugMemoryStack,    0, 0, nullptr, 0, 1, "Stack",       nullptr, false, "memory", "debugmemorystack" },
  { "buffers",    SETTING_BOOL, &gSettings.debugMemoryBuffers,  0, 0, nullptr, 0, 1, "Buffers",     nullptr, false, "memory", "debugmemorybuffers" },
  { "sampleIntervalSec", SETTING_INT, &gSettings.memorySampleIntervalSec, 30, 0, nullptr, 0, 300, "Sample Interval (sec)", nullptr, false, "memory", "memorysampleintervalsec" },
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  // --- g2 group (Even Realities G2 glasses) ---
  { "enabled",    SETTING_BOOL, &gSettings.debugG2,           0, 0, nullptr, 0, 1, "All G2",            nullptr, false, "g2", "debugg2" },
  { "lifecycle",  SETTING_BOOL, &gSettings.debugG2Lifecycle,  0, 0, nullptr, 0, 1, "Lifecycle",         nullptr, false, "g2", "debugg2lifecycle" },
  { "protocol",   SETTING_BOOL, &gSettings.debugG2Protocol,   0, 0, nullptr, 0, 1, "Protocol",          nullptr, false, "g2", "debugg2protocol" },
  { "events",     SETTING_BOOL, &gSettings.debugG2Events,     0, 0, nullptr, 0, 1, "Events",            nullptr, false, "g2", "debugg2events" },
  { "pages",      SETTING_BOOL, &gSettings.debugG2Pages,      0, 0, nullptr, 0, 1, "Pages",             nullptr, false, "g2", "debugg2pages" },
  { "heartbeat",  SETTING_BOOL, &gSettings.debugG2Heartbeat,  0, 0, nullptr, 0, 1, "Heartbeat",         nullptr, false, "g2", "debugg2heartbeat" },
  { "dump",       SETTING_BOOL, &gSettings.debugG2Dump,       0, 0, nullptr, 0, 1, "Dump",              nullptr, false, "g2", "debugg2dump" },
#endif
  // --- espsr group (ESP-SR speech recognition) ---
  { "enabled",    SETTING_BOOL, &gSettings.debugSr,           0, 0, nullptr, 0, 1, "All SR",            nullptr, false, "espsr", "debugsr" },
  { "wake",       SETTING_BOOL, &gSettings.debugSrWake,       0, 0, nullptr, 0, 1, "Wake word",         nullptr, false, "espsr", "debugsrwake" },
  { "command",    SETTING_BOOL, &gSettings.debugSrCommand,    0, 0, nullptr, 0, 1, "Command match",     nullptr, false, "espsr", "debugsrcommand" },
  { "afe",        SETTING_BOOL, &gSettings.debugSrAfe,        0, 0, nullptr, 0, 1, "AFE / VAD",         nullptr, false, "espsr", "debugsrafe" },
  { "lifecycle",  SETTING_BOOL, &gSettings.debugSrLifecycle,  0, 0, nullptr, 0, 1, "Lifecycle",         nullptr, false, "espsr", "debugsrlifecycle" },
  { "tuning",     SETTING_BOOL, &gSettings.debugSrTuning,     0, 0, nullptr, 0, 1, "Tuning / threshold",nullptr, false, "espsr", "debugsrtuning" },
  { "enabled",    SETTING_BOOL, &gSettings.debugI2C,            0, 0, nullptr, 0, 1, "All I2C",     nullptr, false, "i2c", "debugi2c" },
  { "bus",        SETTING_BOOL, &gSettings.debugI2CBus,         0, 0, nullptr, 0, 1, "Bus",         nullptr, false, "i2c", "debugi2cbus" },
  { "discovery",  SETTING_BOOL, &gSettings.debugI2CDiscovery,   0, 0, nullptr, 0, 1, "Discovery",   nullptr, false, "i2c", "debugi2cdiscovery" },
  { "autoStart",  SETTING_BOOL, &gSettings.debugI2CAutoStart,   0, 0, nullptr, 0, 1, "AutoStart",   nullptr, false, "i2c", "debugi2cautostart" },
  { "enabled",    SETTING_BOOL, &gSettings.debugMqtt,           0, 0, nullptr, 0, 1, "All MQTT",   nullptr, false, "mqtt", "debugmqtt" },
  { "connection", SETTING_BOOL, &gSettings.debugMqttConnection, 0, 0, nullptr, 0, 1, "Connection", nullptr, false, "mqtt", "debugmqttconnection" },
  { "pubsub",     SETTING_BOOL, &gSettings.debugMqttPubsub,     0, 0, nullptr, 0, 1, "Pub/Sub",    nullptr, false, "mqtt", "debugmqttpubsub" },
  { "discovery",  SETTING_BOOL, &gSettings.debugMqttDiscovery,  0, 0, nullptr, 0, 1, "Discovery",  nullptr, false, "mqtt", "debugmqttdiscovery" },
  { "commands",   SETTING_BOOL, &gSettings.debugMqttCommands,   0, 0, nullptr, 0, 1, "Commands",   nullptr, false, "mqtt", "debugmqttcommands" },
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

// Columns: name, jsonSection, entries, count, isConnected, description
static const SettingsModule debugSettingsModule = {
  "debug",
  "system.debug",
  debugSettingEntries,
  sizeof(debugSettingEntries) / sizeof(debugSettingEntries[0]),
  nullptr,  // Always available
  "Debug output flags for various subsystems"
};

// ============================================================================
// Output Settings Module (for modular settings registry)
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry outputSettingEntries[] = {
  // --- channels: where firmware output is routed ---
  { "serial",     SETTING_BOOL, &gSettings.outSerial,           1, 0, nullptr, 0, 1, "Serial Output",     nullptr, false, "channels", "outserial" },
  { "web",        SETTING_BOOL, &gSettings.outWeb,              1, 0, nullptr, 0, 1, "Web Output",        nullptr, false, "channels", "outweb" },
  { "display",    SETTING_BOOL, &gSettings.outDisplay,          0, 0, nullptr, 0, 1, "Display Output",    nullptr, false, "channels", "outdisplay" },
  // NOTE: `webConsole` (gSettings.webConsoleDebug) used to live here. It was
  // mis-categorized as a fourth output channel — but the flag only controls
  // whether the served HTML page suppresses its own JS console.log calls
  // (WebServer_Utils.cpp:515). It does NOT route firmware broadcastOutput
  // anywhere. Moved to the debug module under group "page" with a more
  // honest label. The CLI command `webconsole 0|1` is unchanged.
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  { "g2",         SETTING_BOOL, &gSettings.outG2,               0, 0, nullptr, 0, 1, "G2 Glasses Output", nullptr, false, "channels", "outg2" },
#endif
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
#if ENABLE_MICROPHONE_SENSOR
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

size_t readRegisteredSettings(JsonDocument& doc) {
  size_t count = 0;

  for (size_t m = 0; m < gSettingsModuleCount; m++) {
    const SettingsModule* mod = gSettingsModules[m];
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

size_t writeRegisteredSettings(JsonDocument& doc) {
  size_t count = 0;

  for (size_t m = 0; m < gSettingsModuleCount; m++) {
    const SettingsModule* mod = gSettingsModules[m];
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

const char* handleSettingCommand(const SettingEntry* entry, const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) {
    // No argument - show current value
    static char buf[128];
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
          static char errBuf[128];
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
      if (entry->isSecret) {
        BROADCAST_PRINTF("%s updated", entry->jsonKey);
        notifySettingChanged(entry->label ? entry->label : entry->jsonKey, "********");
      } else {
        BROADCAST_PRINTF("%s set to %s", entry->jsonKey, p);
        notifySettingChanged(entry->label ? entry->label : entry->jsonKey, p);
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
  { "eirequirelabels",     "Set Edge Impulse require-labels flag",       true, cmd_set_eirequirelabels,     "Usage: eirequirelabels <0|1>" },
  { "eimaxdetections",     "Set Edge Impulse max detections",            true, cmd_set_eimaxdetections,     "Usage: eimaxdetections <value>" },
  { "eiinputsize",         "Set Edge Impulse input size",                true, cmd_set_eiinputsize,         "Usage: eiinputsize <value>" },
  { "eiinterval",          "Set Edge Impulse inference interval (ms)",   true, cmd_set_eiinterval,          "Usage: eiinterval <100-10000>" },
  { "srautostart",         "Set ESP-SR auto-start flag",                 true, cmd_set_srautostart,         "Usage: srautostart <0|1>" },
  { "srmodelsource",       "Set ESP-SR model source",                    true, cmd_set_srmodelsource,       "Usage: srmodelsource <value>" },
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
      o["key"]   = e->jsonKey;                          // also the set command (case-insensitive)
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
      return written > 0;
    }
  }

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
