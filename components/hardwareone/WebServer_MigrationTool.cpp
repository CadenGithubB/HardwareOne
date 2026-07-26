/**
 * WebServer_MigrationTool.cpp - Migration tool backup/restore endpoints
 *
 * This file holds two logically distinct sub-features, gated independently:
 *
 *   1. Backup export (requires ENABLE_HTTP_SERVER)
 *      POST /api/backup    - Authenticated export of device configuration.
 *      Registered alongside the main web UI; uses makeWebAuthCtx /
 *      tgRequireAuth from WebServer_Server.h. Compiles only when the
 *      regular web UI is available.
 *
 *   2. FTS restore-only server (requires ENABLE_MIGRATION_TOOL)
 *      GET  /                  - Plain-text splash explaining restore mode.
 *      POST /api/restore       - Unauthenticated staging, triple-gated to
 *                                first-time setup only (gFirstTimeSetupState
 *                                + gAcceptingRestore + handler unregister).
 *                                Does NOT write flash until the operator
 *                                confirms on OLED or serial.
 *      OPTIONS+POST /api/ping  - Connection test + CORS preflight.
 *      Spun up by FTS "Import from Backup", torn down on completion or
 *      user abort. Self-contained — no dependency on WebServer_Server.cpp.
 *      Works in headless builds (ENABLE_HTTP_SERVER=0) as a recovery path.
 *
 * CORS headers are applied ONLY to these endpoints. All other device
 * endpoints remain same-origin only.
 */

#include "System_BuildConfig.h"

#if ENABLE_MIGRATION_TOOL

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_app_desc.h>

#include "System_Debug.h"
#include "System_FirstTimeSetup.h"
#if ENABLE_HTTPS
#include "esp_https_server.h"
#endif
#include "System_Filesystem.h"
#include "System_SelfDevice.h"
#include "System_VFS.h"
#include "System_MemUtil.h"
#include "System_Settings.h"
#include "System_User.h"
#include "System_Utils.h"
#include "System_Events.h"  // systemEventPost — SYSEVT_BACKUP_CREATED/RESTORED producer
#if ENABLE_HTTP_SERVER
#include "WebServer_Server.h"  // auth helpers (makeWebAuthCtx, tgRequireAuth) for backup endpoint
#include "WebServer_Utils.h"   // webGuestAccessAllowed — guest role HTTP gate
#endif
#include "WebServer_MigrationTool.h"

// External dependencies (httpd_handle_t server declared in WebServer_Server.h)
extern bool readText(const char* path, String& out);
extern bool writeText(const char* path, const String& content);

// File path constants (must match firmware layout)
static const char* SETTINGS_FILE     = "/system/settings.json";
static const char* DEBUG_FILE        = "/system/debug.json";  // debug flags split out of settings.json
static const char* USERS_FILE        = "/system/users/users.json";
static const char* AUTOMATIONS_FILE  = "/system/automations.json";
static const char* ESPNOW_DEVICES    = "/system/espnow/devices.json";
static const char* ESPNOW_MESH_PEERS = "/system/espnow/mesh_peers.json";
static const char* USER_SETTINGS_DIR  = "/system/users/user_settings";
static const char* BOOT_ANCHORS_FILE  = "/system/boot_anchors.json";  // pending-user NTP anchors (split out of users.json)
static const char* MAPS_DIR          = "/maps";

// ============================================================================
// CORS Helper (scoped to migration endpoints only)
// ============================================================================

static void setCorsHeaders(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
}

static esp_err_t handleCorsOptions(httpd_req_t* req) {
  setCorsHeaders(req);
  httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

// ============================================================================
// Shared helper used by both backup-export and restore — kept outside the
// ENABLE_HTTP_SERVER block so the restore-only path (HTTP=0, MIG=1) can
// still classify cert files in incoming backup payloads.
// ============================================================================

static bool isCertFile(const String& path) {
  return path.endsWith(".pem") || path.endsWith(".crt") || path.endsWith(".key");
}

// Cross-device restore: blank any device-AES ciphertext (AES:…) so we don't
// persist dead blobs (BLE secret, mesh passphrase, ESP-NOW encrypted MACs, …).
// WiFi networks are handled separately by overlaying THIS device's creds.
static void stripAesCiphertextValues(JsonVariant v, int depth = 0) {
  if (depth > 12) return;
  if (v.is<JsonObject>()) {
    JsonObject obj = v.as<JsonObject>();
    for (JsonPair kv : obj) {
      if (kv.value().is<const char*>()) {
        const char* s = kv.value().as<const char*>();
        if (s && strncmp(s, "AES:", 4) == 0) obj[kv.key()] = "";
      } else {
        stripAesCiphertextValues(kv.value(), depth + 1);
      }
    }
  } else if (v.is<JsonArray>()) {
    for (JsonVariant el : v.as<JsonArray>()) {
      stripAesCiphertextValues(el, depth + 1);
    }
  }
}

// ============================================================================
// BACKUP EXPORT (authenticated; requires ENABLE_HTTP_SERVER for auth helpers)
// ============================================================================
// Everything between this guard and the matching `#endif // ENABLE_HTTP_SERVER`
// further down depends on makeWebAuthCtx / tgRequireAuth from
// WebServer_Server.h. In a (HTTP=0, MIG=1) build, the backup endpoint is
// silently omitted but the restore-only server below remains available.
#if ENABLE_HTTP_SERVER

// Walk bundled backup content and record every device-AES ciphertext (AES:…)
// plus user password hashes. encryptedFields used to be a hardcoded stub
// ("wifiNetworks[].password") that drifted from the nested settings schema
// and missed other secrets (BLE secure channel, mesh passphrase, certs, …).
static void collectAesEncryptedFields(JsonVariantConst v, const char* filePath,
                                      String& pathBuf, JsonArray& encFields, int depth = 0) {
  if (depth > 12) return;
  if (v.is<JsonObjectConst>()) {
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
      size_t mark = pathBuf.length();
      if (mark > 0) pathBuf += '.';
      pathBuf += kv.key().c_str();
      collectAesEncryptedFields(kv.value(), filePath, pathBuf, encFields, depth + 1);
      pathBuf.remove(mark);
    }
  } else if (v.is<JsonArrayConst>()) {
    size_t i = 0;
    for (JsonVariantConst el : v.as<JsonArrayConst>()) {
      size_t mark = pathBuf.length();
      char idx[16];
      snprintf(idx, sizeof(idx), "[%u]", (unsigned)i);
      pathBuf += idx;
      collectAesEncryptedFields(el, filePath, pathBuf, encFields, depth + 1);
      pathBuf.remove(mark);
      ++i;
    }
  } else if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (s && strncmp(s, "AES:", 4) == 0) {
      char entry[256];
      snprintf(entry, sizeof(entry), "%s:%s", filePath, pathBuf.c_str());
      encFields.add(entry);
    }
  }
}

static void collectEncryptedFieldsFromFiles(JsonObject files, JsonArray encFields) {
  for (JsonPair kv : files) {
    const char* filePath = kv.key().c_str();
    JsonVariant v = kv.value();
    if (v.is<const char*>()) {
      const char* s = v.as<const char*>();
      // Whole-file ciphertext (certs/keys encrypted at export time)
      if (s && strncmp(s, "AES:", 4) == 0) encFields.add(filePath);
      continue;
    }
    String pathBuf;
    pathBuf.reserve(96);
    collectAesEncryptedFields(v, filePath, pathBuf, encFields);
  }

  // User passwords are PBKDF2 hashes (not AES:), but still credential material
  // the migration tool treats as device-bound — list concrete paths when present.
  for (JsonPair kv : files) {
    const char* filePath = kv.key().c_str();
    if (!strstr(filePath, "/user_settings/") || !strstr(filePath, ".json")) continue;
    if (!kv.value().is<JsonObject>()) continue;
    JsonVariantConst pwVar = kv.value()["password"];
    if (pwVar.is<const char*>()) {
      const char* pw = pwVar.as<const char*>();
      if (pw && pw[0]) {
        char entry[160];
        snprintf(entry, sizeof(entry), "%s:password", filePath);
        encFields.add(entry);
      }
    }
  }
}

// ============================================================================
// Helper: Add a file to the backup JSON if it exists
// ============================================================================

static void addFileToBackup(JsonObject& files, JsonArray& warnings, const char* path) {
  if (!filesystemReady) return;
  String content;
  if (!readText(path, content)) {
    char warn[128];
    snprintf(warn, sizeof(warn), "%s not found, skipped", path);
    warnings.add(warn);
    return;
  }
  // Try to parse as JSON; if valid, store as object; otherwise store as string
  PSRAM_JSON_DOC(tmpDoc);
  DeserializationError err = deserializeJson(tmpDoc, content);
  if (!err) {
    files[path] = tmpDoc.as<JsonVariant>();
  } else {
    files[path] = content;
  }
}

// ============================================================================
// Helper: Recursively add all files from a directory
// ============================================================================

static void addDirectoryToBackup(JsonObject& files, JsonArray& warnings, const char* dirPath, const AuthContext& ctx, int depth = 0) {
  if (!filesystemReady || depth > 3) return;

  File dir = VFS::openGuarded(dirPath, "r", ctx);
  if (!dir || !dir.isDirectory()) {
    // openGuarded returns an empty File for two very different reasons: a
    // permission denial (canRead said no) or the directory genuinely not
    // existing. Distinguish them so the backup warning isn't misleading.
    char warn[160];
    if (!canRead(String(dirPath), ctx)) {
      snprintf(warn, sizeof(warn), "%s access denied (admin required), skipped", dirPath);
    } else {
      snprintf(warn, sizeof(warn), "%s not found, skipped", dirPath);
    }
    warnings.add(warn);
    return;
  }

  File entry;
  while ((entry = dir.openNextFile())) {
    String path = String(entry.name());
    // Ensure absolute path
    if (!path.startsWith("/")) { char pBuf[128]; snprintf(pBuf, sizeof(pBuf), "%s/%s", dirPath, path.c_str()); path = pBuf; }

    if (entry.isDirectory()) {
      addDirectoryToBackup(files, warnings, path.c_str(), ctx, depth + 1);
    } else {
      // Include .hwmap, .json, and certificate files
      if (path.endsWith(".hwmap") || path.endsWith(".json") ||
          path.endsWith(".pem") || path.endsWith(".crt") || path.endsWith(".key")) {
        String content = entry.readString();
        if (content.length() > 0) {
          // Try to parse JSON files as objects
          if (path.endsWith(".json")) {
            PSRAM_JSON_DOC(tmpDoc);
            DeserializationError err = deserializeJson(tmpDoc, content);
            if (!err) {
              files[path] = tmpDoc.as<JsonVariant>();
            } else {
              files[path] = content;
            }
          } else if (isCertFile(path)) {
            // Encrypt certificate/key file contents with device AES key
            // so they are not stored in plain text in the backup file
            String encrypted = encryptString(content);
            if (encrypted.length() > 0) {
              files[path] = encrypted;
            } else {
              char warn[128];
              snprintf(warn, sizeof(warn), "Failed to encrypt %s, skipped", path.c_str());
              warnings.add(warn);
            }
          } else {
            // Binary .hwmap files stored as raw string (base64 would be better but
            // matches existing tool expectations)
            files[path] = content;
          }
        }
      }
    }
    entry.close();
  }
  dir.close();
}

// ============================================================================
// POST /api/backup - Authenticated, CORS-enabled
// ============================================================================

static esp_err_t handleBackup(httpd_req_t* req) {
  setCorsHeaders(req);

  DEBUG_HTTPF("[Backup] Request received from client");
  
  // Log Authorization header if present
  size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (hdr_len > 0) {
    char* auth_hdr = (char*)malloc(hdr_len + 1);
    if (auth_hdr && httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, hdr_len + 1) == ESP_OK) {
      DEBUG_HTTPF("[Backup] Authorization header present (len=%d): %.20s...", hdr_len, auth_hdr);
      free(auth_hdr);
    } else {
      free(auth_hdr);
    }
  } else {
    DEBUG_HTTPF("[Backup] No Authorization header present");
  }

  // Authenticate
  AuthContext ctx = makeWebAuthCtx(req);
  DEBUG_HTTPF("[Backup] Auth context created, transport=%d", ctx.transport);
  if (!tgRequireAuth(ctx)) {
    return ESP_OK;
  }
  if (!webGuestAccessAllowed(req, ctx)) return ESP_OK;
  DEBUG_HTTPF("[Backup] Authentication successful, user=%s", ctx.user.c_str());

  // Backup exports whole-device configuration — every user's settings, TLS
  // certificates, and credential files. Restrict to admins. (A non-admin would
  // also receive a silently-incomplete bundle, since the guarded directory
  // reads below grant user_settings/ and certs/ to admins only.)
  if (!isAdminUser(ctx.user)) {
    DEBUG_HTTPF("[Backup] Denied: user '%s' is not an admin", ctx.user.c_str());
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"Admin access required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  if (!filesystemReady) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"Filesystem not ready\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Read POST body to get category selection
  char body[256] = {0};
  int received = 0;
  if (req->content_len > 0 && req->content_len < sizeof(body)) {
    received = httpd_req_recv(req, body, req->content_len);
    if (received > 0) body[received] = '\0';
  }

  // Parse categories from body: { "categories": ["settings", "users", ...] }
  bool wantSettings = false, wantUsers = false, wantAutomations = false;
  bool wantEspnow = false, wantMaps = false, wantCerts = false;

  if (received > 0) {
    PSRAM_JSON_DOC(reqDoc);
    if (deserializeJson(reqDoc, body) == DeserializationError::Ok) {
      JsonArray cats = reqDoc["categories"].as<JsonArray>();
      for (JsonVariant cat : cats) {
        const char* c = cat.as<const char*>();
        if (!c) continue;
        if (strcmp(c, "settings") == 0) wantSettings = true;
        else if (strcmp(c, "users") == 0) wantUsers = true;
        else if (strcmp(c, "automations") == 0) wantAutomations = true;
        else if (strcmp(c, "espnow") == 0) wantEspnow = true;
        else if (strcmp(c, "maps") == 0) wantMaps = true;
        else if (strcmp(c, "certs") == 0) wantCerts = true;
      }
    }
  }

  // If no categories specified, include all
  if (!wantSettings && !wantUsers && !wantAutomations && !wantEspnow && !wantMaps && !wantCerts) {
    wantSettings = wantUsers = wantAutomations = wantEspnow = wantMaps = wantCerts = true;
  }

  // Build backup JSON document
  PSRAM_JSON_DOC(doc);

  doc["magic"] = "HWBACKUP";
  doc["formatVersion"] = 1;

  // Timestamp
  char ts[32];
  unsigned long now = millis();
  snprintf(ts, sizeof(ts), "%lu", now);
  doc["timestamp"] = ts;

  // Device info
  JsonObject device = doc["device"].to<JsonObject>();
  device["hostname"] = WiFi.getHostname();
  device["mac"] = SelfDevice::macString();
  device["fingerprint"] = getDeviceFingerprint();
  device["board"] = BOARD_NAME;
  device["firmwareVersion"] = SelfDevice::firmwareVersion();

  // Build system info for ip
  PSRAM_JSON_DOC(sysDoc);
  buildSystemInfoJson(sysDoc);
  if (!sysDoc["net"].isNull()) {
    device["ip"] = sysDoc["net"]["ip"].as<String>();
  }

  // Files, warnings, and encrypted field tracking
  JsonObject files = doc["files"].to<JsonObject>();
  JsonArray warnings = doc["warnings"].to<JsonArray>();
  JsonArray encFields = doc["encryptedFields"].to<JsonArray>();

  if (wantSettings) {
    addFileToBackup(files, warnings, SETTINGS_FILE);
    addFileToBackup(files, warnings, DEBUG_FILE);  // debug flags live in their own file now — bundle with "settings"
  }
  if (wantUsers) {
    addFileToBackup(files, warnings, USERS_FILE);
    addFileToBackup(files, warnings, BOOT_ANCHORS_FILE);  // so pending-user timestamps stay resolvable after restore
    addDirectoryToBackup(files, warnings, USER_SETTINGS_DIR, ctx);
  }
  if (wantAutomations) addFileToBackup(files, warnings, AUTOMATIONS_FILE);
  if (wantEspnow) {
    addFileToBackup(files, warnings, ESPNOW_DEVICES);
    addFileToBackup(files, warnings, ESPNOW_MESH_PEERS);
  }
  if (wantMaps) addDirectoryToBackup(files, warnings, MAPS_DIR, ctx);
  if (wantCerts) addDirectoryToBackup(files, warnings, "/system/certs", ctx);

  // Discover secrets from actual bundled content (AES: blobs + user password hashes)
  collectEncryptedFieldsFromFiles(files, encFields);

  // Serialize and send
  httpd_resp_set_type(req, "application/json");

  // Measure output size first
  size_t jsonLen = measureJson(doc);
  if (jsonLen < 8192) {
    // Small enough for a static buffer
    char* buf = (char*)malloc(jsonLen + 1);
    if (buf) {
      serializeJson(doc, buf, jsonLen + 1);
      httpd_resp_send(req, buf, jsonLen);
      free(buf);
    } else {
      httpd_resp_send(req, "{\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
    }
  } else {
    // Large backup: use chunked response via String
    String json;
    serializeJson(doc, json);
    httpd_resp_send(req, json.c_str(), json.length());
  }

  // Bundle built and sent — record which categories went out.
  {
    char catBuf[48];
    if (wantSettings && wantUsers && wantAutomations && wantEspnow && wantMaps && wantCerts) {
      strcpy(catBuf, "all");
    } else {
      snprintf(catBuf, sizeof(catBuf), "%s%s%s%s%s%s",
               wantSettings ? "settings," : "",
               wantUsers ? "users," : "",
               wantAutomations ? "automations," : "",
               wantEspnow ? "espnow," : "",
               wantMaps ? "maps," : "",
               wantCerts ? "certs," : "");
      size_t l = strlen(catBuf);
      if (l > 0 && catBuf[l - 1] == ',') catBuf[l - 1] = '\0';
    }
    systemEventPost(SYSEVT_BACKUP_CREATED, catBuf, "device config exported");
  }

  return ESP_OK;
}

#endif // ENABLE_HTTP_SERVER  (end of backup-export block)

// ============================================================================
// POST /api/restore - Unauthenticated staging, CORS-enabled, triple-gated
// Flash writes happen only after OLED/serial confirm (applyStagedMigrationRestore).
// ============================================================================

static char* sStagedRestoreBuf = nullptr;
static size_t sStagedRestoreLen = 0;
static bool sStagedRestoreCompatible = true;
static volatile bool sRestoreAwaitingConfirm = false;

bool migrationRestoreAwaitingConfirm() {
  return sRestoreAwaitingConfirm;
}

void discardStagedMigrationRestore() {
  sRestoreAwaitingConfirm = false;
  if (sStagedRestoreBuf) {
    free(sStagedRestoreBuf);
    sStagedRestoreBuf = nullptr;
  }
  sStagedRestoreLen = 0;
  sStagedRestoreCompatible = true;
}

// Apply a validated HWBACKUP JsonDocument to flash. Used by applyStagedMigrationRestore.
static bool writeRestoreFilesFromDoc(JsonDocument& doc, bool compatible,
                                     int* outWritten, int* outErrored) {
  // Cross-device restore: preserve the WiFi network the user entered during Import.
  // The source device's wifiNetworks (under network.wifi.networks) carry passwords
  // encrypted with the source's device key and won't decrypt here, so we keep THIS
  // device's current creds instead of overwriting them with useless ones.
  PSRAM_JSON_DOC(deviceSettingsDoc);
  bool haveDeviceWifi = false;
  if (!compatible) {
    String curSettings;
    if (readText(SETTINGS_FILE, curSettings)) {
      DeserializationError derr = deserializeJson(deviceSettingsDoc, curSettings);
      if (!derr && deviceSettingsDoc["network"]["wifi"]["networks"].is<JsonArray>()) {
        haveDeviceWifi = true;
      }
    }
  }

  JsonObject files = doc["files"].as<JsonObject>();
  int filesWritten = 0;
  int filesErrored = 0;

  for (JsonPair kv : files) {
    const char* path = kv.key().c_str();
    if (!path || path[0] != '/') {
      filesErrored++;
      continue;
    }

    String pathStr = String(path);

    // On different device: skip user credential files and encrypted cert files
    if (!compatible) {
      if (pathStr.startsWith("/system/users/")) {
        continue;
      }
      if (isCertFile(pathStr)) {
        continue;
      }
    }

    int lastSlash = pathStr.lastIndexOf('/');
    if (lastSlash > 0) {
      String dir = pathStr.substring(0, lastSlash);
      VFS::mkdirGuarded(dir, VFS::systemAuth("migration.restore_mkdir"));
    }

    String content;
    if (!compatible && !kv.value().is<const char*>()) {
      PSRAM_JSON_DOC(mergedDoc);
      mergedDoc.set(kv.value());
      stripAesCiphertextValues(mergedDoc.as<JsonVariant>());
      if (haveDeviceWifi && pathStr == SETTINGS_FILE) {
        mergedDoc["network"]["wifi"]["networks"] = deviceSettingsDoc["network"]["wifi"]["networks"];
        // Force this device back online after a cross-device restore. WiFi is
        // three independent axes now, and ALL of them have to be on or the
        // guarantee is hollow: enabled (may it run), autoStart (connect at
        // boot), autoReconnect (keep trying after a drop). This used to be the
        // single key "autoReconnect", which no longer exists.
        mergedDoc["network"]["wifi"]["wifiEnabled"]       = true;
        mergedDoc["network"]["wifi"]["wifiAutoStart"]     = true;
        mergedDoc["network"]["wifi"]["wifiAutoReconnect"] = true;
      }
      serializeJsonPretty(mergedDoc, content);
    } else if (kv.value().is<const char*>()) {
      content = kv.value().as<String>();
    } else {
      serializeJsonPretty(kv.value(), content);
    }

    if (compatible && isCertFile(pathStr) && content.startsWith("AES:")) {
      String decrypted = decryptString(content);
      if (decrypted.length() > 0) {
        content = decrypted;
      } else {
        filesErrored++;
        continue;
      }
    }

    if (writeTextAtomic(path, content)) {
      filesWritten++;
    } else {
      filesErrored++;
    }
  }

  if (!compatible) {
    // Next boot: streamlined credential-only flow (admin login), not full FTS menu.
    writeText(PENDING_CRED_SETUP_FILE, "1");
  }

  if (outWritten) *outWritten = filesWritten;
  if (outErrored) *outErrored = filesErrored;
  return filesWritten > 0 || filesErrored == 0;
}

bool applyStagedMigrationRestore() {
  if (!sRestoreAwaitingConfirm || !sStagedRestoreBuf || sStagedRestoreLen == 0) {
    return false;
  }

  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, sStagedRestoreBuf, sStagedRestoreLen);
  if (err) {
    DEBUG_HTTPF("[Restore] Staged payload re-parse failed: %s", err.c_str());
    discardStagedMigrationRestore();
    return false;
  }

  const bool compatible = sStagedRestoreCompatible;
  int filesWritten = 0;
  int filesErrored = 0;
  bool ok = writeRestoreFilesFromDoc(doc, compatible, &filesWritten, &filesErrored);

  discardStagedMigrationRestore();

  if (!ok) {
    broadcastOutput("Restore failed: no files were written.");
    return false;
  }

  gAcceptingRestore = false;
  gRestoreComplete = true;

  char detBuf[80];
  snprintf(detBuf, sizeof(detBuf), "%d file(s) written, %d errored%s",
           filesWritten, filesErrored, compatible ? "" : " (cross-device)");
  systemEventPost(SYSEVT_BACKUP_RESTORED, "all", detBuf);
  broadcastOutput(String("Restore applied: ") + detBuf);
  return true;
}

static esp_err_t handleRestore(httpd_req_t* req) {
  setCorsHeaders(req);

  // Gate 2: Check that we're actually accepting restores
  if (gFirstTimeSetupState != SETUP_IN_PROGRESS || !gAcceptingRestore) {
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"Restore not available. Device must be in first-time setup with Import from Backup selected.\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  if (sRestoreAwaitingConfirm) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req,
      "{\"error\":\"Confirm pending\",\"pendingConfirm\":true,"
      "\"message\":\"A backup is already staged. Confirm or cancel on the device "
      "(OLED or serial) before sending another.\"}",
      HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  if (!filesystemReady) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"Filesystem not ready\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Read POST body (backup payload can be large)
  if (req->content_len == 0 || req->content_len > 512 * 1024) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"Invalid payload size\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Allocate buffer for the payload (prefer PSRAM) — kept until confirm/discard
  char* buf = (char*)heap_caps_calloc(req->content_len + 1, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    buf = (char*)heap_caps_calloc(req->content_len + 1, 1, MALLOC_CAP_DEFAULT);
  }
  if (!buf) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  int totalReceived = 0;
  while (totalReceived < (int)req->content_len) {
    int ret = httpd_req_recv(req, buf + totalReceived, req->content_len - totalReceived);
    if (ret <= 0) {
      free(buf);
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send(req, "{\"error\":\"Failed to receive payload\"}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    totalReceived += ret;
  }
  buf[totalReceived] = '\0';

  // Validate without writing — parse a temporary doc, keep raw buffer staged
  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, buf, totalReceived);
  if (err) {
    free(buf);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    char errMsg[128];
    snprintf(errMsg, sizeof(errMsg), "{\"error\":\"Invalid JSON: %s\"}", err.c_str());
    httpd_resp_send(req, errMsg, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  const char* magic = doc["magic"];
  if (!magic || strcmp(magic, "HWBACKUP") != 0) {
    free(buf);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"Not a valid HWBACKUP file\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  bool compatible = true;
  const char* backupFingerprint = doc["device"]["fingerprint"];
  if (backupFingerprint && strlen(backupFingerprint) > 0) {
    String deviceFp = getDeviceFingerprint();
    compatible = (deviceFp == backupFingerprint);
  }

  bool forceRestore = doc["force"] | false;

  if (!compatible && !forceRestore) {
    free(buf);
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    const char* resp = "{\"error\":\"Device mismatch\","
      "\"compatible\":false,"
      "\"reason\":\"different_device\","
      "\"message\":\"This backup was created on a different device. "
      "User passwords and encrypted WiFi credentials will not work. "
      "You can force restore of non-sensitive data (settings, automations, maps). "
      "After reboot you will need to create new login credentials.\"}";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Stage for on-device confirm (OLED / serial). Do not touch flash yet.
  discardStagedMigrationRestore();  // clear any stale state
  sStagedRestoreBuf = buf;
  sStagedRestoreLen = (size_t)totalReceived;
  sStagedRestoreCompatible = compatible;
  sRestoreAwaitingConfirm = true;

  broadcastOutput("");
  broadcastOutput("========================================");
  broadcastOutput("  BACKUP RECEIVED — CONFIRM ON DEVICE");
  broadcastOutput("========================================");
  broadcastOutput(compatible
    ? "Same-device backup staged. Confirm to write files and reboot."
    : "Cross-device backup staged (credentials will be skipped).");
  broadcastOutput("OLED: Yes/No prompt  |  Serial: type yes or no");
  broadcastOutput("========================================");

  httpd_resp_set_status(req, "202 Accepted");
  httpd_resp_set_type(req, "application/json");
  const char* resp = compatible
    ? "{\"success\":true,\"pendingConfirm\":true,\"compatible\":true,"
      "\"message\":\"Backup staged. Confirm on the device OLED or serial console to apply.\"}"
    : "{\"success\":true,\"pendingConfirm\":true,\"compatible\":false,\"credentialsSkipped\":true,"
      "\"message\":\"Cross-device backup staged. Confirm on the device OLED or serial console to apply. "
      "Credentials will be skipped; guided login setup runs after reboot.\"}";
  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// ============================================================================
// Handler Registration
// ============================================================================

#if ENABLE_HTTP_SERVER
// Backup-export handlers and the /api/ping CORS preflight are registered
// from WebServer_Server.cpp's startup path. Both depend on the main web
// UI being compiled in.
void registerMigrationBackupHandler(httpd_handle_t server) {
  static const httpd_uri_t backupPost = {
    .uri = "/api/backup",
    .method = HTTP_POST,
    .handler = handleBackup,
    .user_ctx = NULL
  };
  static const httpd_uri_t backupOptions = {
    .uri = "/api/backup",
    .method = HTTP_OPTIONS,
    .handler = handleCorsOptions,
    .user_ctx = NULL
  };
  esp_err_t postErr = httpd_register_uri_handler(server, &backupPost);
  esp_err_t optErr = httpd_register_uri_handler(server, &backupOptions);
  if (postErr != ESP_OK || optErr != ESP_OK) {
    DEBUG_HTTPF("[Migration] /api/backup register failed (POST=%s OPTIONS=%s) — "
                "URI handler table full? Increase max_uri_handlers.",
                esp_err_to_name(postErr), esp_err_to_name(optErr));
    broadcastOutput("[Migration] WARNING: /api/backup CORS preflight failed to register");
  }
}

void registerPingOptionsHandler(httpd_handle_t server) {
  static const httpd_uri_t pingOptions = {
    .uri = "/api/ping",
    .method = HTTP_OPTIONS,
    .handler = handleCorsOptions,
    .user_ctx = NULL
  };
  esp_err_t err = httpd_register_uri_handler(server, &pingOptions);
  if (err != ESP_OK) {
    DEBUG_HTTPF("[Migration] /api/ping OPTIONS register failed: %s", esp_err_to_name(err));
  }
}
#endif // ENABLE_HTTP_SERVER  (end of backup-registration block)

// Gate 1: Only register /api/restore when user selects "Import from Backup"
void registerMigrationRestoreHandler(httpd_handle_t server) {
  static const httpd_uri_t restorePost = {
    .uri = "/api/restore",
    .method = HTTP_POST,
    .handler = handleRestore,
    .user_ctx = NULL
  };
  static const httpd_uri_t restoreOptions = {
    .uri = "/api/restore",
    .method = HTTP_OPTIONS,
    .handler = handleCorsOptions,
    .user_ctx = NULL
  };
  httpd_register_uri_handler(server, &restorePost);
  httpd_register_uri_handler(server, &restoreOptions);
}

// Gate 3: Unregister /api/restore after restore completes
void unregisterMigrationRestoreHandler(httpd_handle_t server) {
  httpd_unregister_uri_handler(server, "/api/restore", HTTP_POST);
  httpd_unregister_uri_handler(server, "/api/restore", HTTP_OPTIONS);
}

// ============================================================================
// Minimal restore-only HTTP server (used during "Import from Backup" setup)
// Only /api/ping and /api/restore are registered — nothing else is accessible.
// ============================================================================

static esp_err_t handleRestoreSplash(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  const char* body = sRestoreAwaitingConfirm
    ? "HardwareOne Restore Mode\n"
      "\n"
      "A backup has been received and is waiting for confirmation.\n"
      "\n"
      "Confirm on the device OLED (Yes/No) or type 'yes' / 'no'\n"
      "on the serial console. Files are not written until then.\n"
      "\n"
      "If you are the device owner and need to exit restore mode,\n"
      "cancel the confirm prompt, then use serial 'back' or gamepad B.\n"
    : "HardwareOne Restore Mode\n"
      "\n"
      "This device is waiting for a backup restore.\n"
      "\n"
      "Do not use the normal web interface during this step.\n"
      "Do not try to configure this device from this page.\n"
      "\n"
      "Use the HardwareOne Migration Tool browser application\n"
      "on another device to send your .hwbackup file here.\n"
      "After upload, confirm on the device OLED or serial console.\n"
      "\n"
      "Migration Tool repository:\n"
      "CadenGithubB/HardwareOne-Migration-Tool\n"
      "\n"
      "If you are the device owner and need to exit restore mode,\n"
      "use the serial console command 'back' or press B on the gamepad.\n";
  httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static esp_err_t handlePingRestore(httpd_req_t* req) {
  setCorsHeaders(req);
  httpd_resp_set_type(req, "application/json");

  // Include device fingerprint so the migration tool can pre-check compatibility
  String fp = getDeviceFingerprint();
  String resp = "{\"ok\":true,\"mode\":\"restore\",\"fingerprint\":\"" + fp +
                "\",\"pendingConfirm\":" + (sRestoreAwaitingConfirm ? "true" : "false") + "}";
  httpd_resp_send(req, resp.c_str(), resp.length());
  return ESP_OK;
}

#if ENABLE_HTTPS
static String sRestoreCertData;
static String sRestoreKeyData;
static bool sRestoreUsedHttps = false;
#endif

void startRestoreOnlyHttpServer() {
#if ENABLE_HTTPS
  // Try HTTPS if enabled and certs are present
  if (gSettings.httpsEnabled && filesystemReady) {
    static const char* CERT_PATH = "/system/certs/https_server.crt";
    static const char* KEY_PATH  = "/system/certs/https_server.key";
    bool certsOk = false;
    if (VFS::existsGuarded(CERT_PATH, VFS::systemAuth("migration.restore_certs")) &&
        VFS::existsGuarded(KEY_PATH, VFS::systemAuth("migration.restore_certs"))) {
      File cf = VFS::openGuarded(CERT_PATH, "r", VFS::systemAuth("migration.restore_certs"));
      File kf = VFS::openGuarded(KEY_PATH, "r", VFS::systemAuth("migration.restore_certs"));
      if (cf && kf) {
        sRestoreCertData = cf.readString();
        sRestoreKeyData  = kf.readString();
        cf.close(); kf.close();
        if (sRestoreCertData.length() > 0 && sRestoreKeyData.length() > 0) certsOk = true;
      }
      if (cf) cf.close();
      if (kf) kf.close();
    }
    if (certsOk) {
      httpd_ssl_config_t ssl = HTTPD_SSL_CONFIG_DEFAULT();
      ssl.httpd.max_uri_handlers = 8;
      ssl.httpd.lru_purge_enable = false;
      ssl.httpd.stack_size = 8192;
      ssl.httpd.recv_wait_timeout = 60;
      ssl.httpd.send_wait_timeout = 60;
      ssl.httpd.max_open_sockets = 2;
      ssl.servercert = (const uint8_t*)sRestoreCertData.c_str();
      ssl.servercert_len = sRestoreCertData.length() + 1;
      ssl.prvtkey_pem = (const uint8_t*)sRestoreKeyData.c_str();
      ssl.prvtkey_len = sRestoreKeyData.length() + 1;
      ssl.port_secure = 443;
      if (httpd_ssl_start(&server, &ssl) == ESP_OK) {
        sRestoreUsedHttps = true;
        broadcastOutput("HTTPS server started (restore-only mode)");
        goto restore_register;
      }
      broadcastOutput("WARNING: HTTPS failed, falling back to HTTP");
      sRestoreCertData = ""; sRestoreKeyData = "";
    }
  }
#endif

  // Plain HTTP fallback
  {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.lru_purge_enable  = false;
    config.stack_size        = 8192;
    config.recv_wait_timeout = 60;
    config.send_wait_timeout = 60;

    if (httpd_start(&server, &config) != ESP_OK) {
      broadcastOutput("ERROR: Failed to start restore HTTP server");
      return;
    }
    broadcastOutput("HTTP server started (restore-only mode)");
  }

#if ENABLE_HTTPS
restore_register:
#endif

  static const httpd_uri_t splashGet = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = handleRestoreSplash,
    .user_ctx = NULL
  };
  static const httpd_uri_t pingGet = {
    .uri = "/api/ping",
    .method = HTTP_GET,
    .handler = handlePingRestore,
    .user_ctx = NULL
  };
  static const httpd_uri_t pingOptions = {
    .uri = "/api/ping",
    .method = HTTP_OPTIONS,
    .handler = handleCorsOptions,
    .user_ctx = NULL
  };
  static const httpd_uri_t restorePost = {
    .uri = "/api/restore",
    .method = HTTP_POST,
    .handler = handleRestore,
    .user_ctx = NULL
  };
  static const httpd_uri_t restoreOptions = {
    .uri = "/api/restore",
    .method = HTTP_OPTIONS,
    .handler = handleCorsOptions,
    .user_ctx = NULL
  };

  httpd_register_uri_handler(server, &splashGet);
  httpd_register_uri_handler(server, &pingGet);
  httpd_register_uri_handler(server, &pingOptions);
  httpd_register_uri_handler(server, &restorePost);
  httpd_register_uri_handler(server, &restoreOptions);
}

void stopRestoreOnlyHttpServer() {
  discardStagedMigrationRestore();
  if (server) {
#if ENABLE_HTTPS
    if (sRestoreUsedHttps) {
      httpd_ssl_stop(server);
      sRestoreUsedHttps = false;
      sRestoreCertData = ""; sRestoreKeyData = "";
    } else
#endif
    {
      httpd_stop(server);
    }
    server = NULL;
  }
}

#endif // ENABLE_MIGRATION_TOOL
