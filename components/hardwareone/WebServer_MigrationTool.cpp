/**
 * WebServer_MigrationTool.cpp - Migration tool backup/restore endpoints
 *
 * Provides 3 CORS-enabled endpoints for the HardwareOne Migration Tool:
 *   GET  /api/ping    - Connection test (handled in WebServer_Server.cpp, OPTIONS here)
 *   POST /api/backup  - Authenticated export of device configuration
 *   POST /api/restore - Unauthenticated import, triple-gated to first-time setup only
 *
 * CORS headers are applied ONLY to these endpoints. All other device
 * endpoints remain same-origin only.
 */

#include "System_BuildConfig.h"

#if ENABLE_HTTP_SERVER

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>

#include "System_Debug.h"
#include "System_FirstTimeSetup.h"
#include "System_Filesystem.h"
#include "System_MemUtil.h"
#include "System_Settings.h"
#include "System_User.h"
#include "System_Utils.h"
#include "WebServer_Server.h"
#include "WebServer_MigrationTool.h"

// External dependencies
extern httpd_handle_t server;
extern bool filesystemReady;
extern bool readText(const char* path, String& out);
extern bool writeText(const char* path, const String& content);

// File path constants (must match firmware layout)
static const char* SETTINGS_FILE     = "/system/settings.json";
static const char* USERS_FILE        = "/system/users/users.json";
static const char* AUTOMATIONS_FILE  = "/system/automations.json";
static const char* ESPNOW_DEVICES    = "/system/espnow/devices.json";
static const char* ESPNOW_MESH_PEERS = "/system/espnow/mesh_peers.json";
static const char* USER_SETTINGS_DIR  = "/system/users/user_settings";
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
  JsonDocument tmpDoc;
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

static void addDirectoryToBackup(JsonObject& files, JsonArray& warnings, const char* dirPath, int depth = 0) {
  if (!filesystemReady || depth > 3) return;

  File dir = LittleFS.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    char warn[128];
    snprintf(warn, sizeof(warn), "%s directory not found, skipped", dirPath);
    warnings.add(warn);
    return;
  }

  File entry;
  while ((entry = dir.openNextFile())) {
    String path = String(entry.name());
    // Ensure absolute path
    if (!path.startsWith("/")) path = String(dirPath) + "/" + path;

    if (entry.isDirectory()) {
      addDirectoryToBackup(files, warnings, path.c_str(), depth + 1);
    } else {
      // Include .hwmap and .json files
      if (path.endsWith(".hwmap") || path.endsWith(".json")) {
        String content = entry.readString();
        if (content.length() > 0) {
          // Try to parse JSON files as objects
          if (path.endsWith(".json")) {
            JsonDocument tmpDoc;
            DeserializationError err = deserializeJson(tmpDoc, content);
            if (!err) {
              files[path] = tmpDoc.as<JsonVariant>();
            } else {
              files[path] = content;
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
    DEBUG_HTTPF("[Backup] Authentication failed - returning 401");
    return ESP_OK;
  }
  DEBUG_HTTPF("[Backup] Authentication successful, user=%s", ctx.user.c_str());

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
  bool wantEspnow = false, wantMaps = false;

  if (received > 0) {
    JsonDocument reqDoc;
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
      }
    }
  }

  // If no categories specified, include all
  if (!wantSettings && !wantUsers && !wantAutomations && !wantEspnow && !wantMaps) {
    wantSettings = wantUsers = wantAutomations = wantEspnow = wantMaps = true;
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
  device["mac"] = WiFi.macAddress();
  device["fingerprint"] = getDeviceFingerprint();

  // Build system info for version/board
  JsonDocument sysDoc;
  buildSystemInfoJson(sysDoc);
  if (sysDoc.containsKey("net")) {
    device["ip"] = sysDoc["net"]["ip"].as<String>();
  }

  // Files and warnings
  JsonObject files = doc["files"].to<JsonObject>();
  JsonArray warnings = doc["warnings"].to<JsonArray>();

  if (wantSettings)    addFileToBackup(files, warnings, SETTINGS_FILE);
  if (wantUsers) {
    addFileToBackup(files, warnings, USERS_FILE);
    addDirectoryToBackup(files, warnings, USER_SETTINGS_DIR);
  }
  if (wantAutomations) addFileToBackup(files, warnings, AUTOMATIONS_FILE);
  if (wantEspnow) {
    addFileToBackup(files, warnings, ESPNOW_DEVICES);
    addFileToBackup(files, warnings, ESPNOW_MESH_PEERS);
  }
  if (wantMaps) addDirectoryToBackup(files, warnings, MAPS_DIR);

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

  return ESP_OK;
}

// ============================================================================
// POST /api/restore - Unauthenticated, CORS-enabled, triple-gated
// ============================================================================

static esp_err_t handleRestore(httpd_req_t* req) {
  setCorsHeaders(req);

  // Gate 2: Check that we're actually accepting restores
  if (gFirstTimeSetupState != SETUP_IN_PROGRESS || !gAcceptingRestore) {
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"Restore not available. Device must be in first-time setup with Import from Backup selected.\"}", HTTPD_RESP_USE_STRLEN);
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

  // Allocate buffer for the payload (prefer PSRAM)
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

  // Receive the full body
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

  // Parse the backup JSON
  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, buf, totalReceived);
  free(buf);

  if (err) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    char errMsg[128];
    snprintf(errMsg, sizeof(errMsg), "{\"error\":\"Invalid JSON: %s\"}", err.c_str());
    httpd_resp_send(req, errMsg, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Validate magic header
  const char* magic = doc["magic"];
  if (!magic || strcmp(magic, "HWBACKUP") != 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"Not a valid HWBACKUP file\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Check device fingerprint compatibility
  bool compatible = true;
  const char* backupFingerprint = doc["device"]["fingerprint"];
  if (backupFingerprint && strlen(backupFingerprint) > 0) {
    String deviceFp = getDeviceFingerprint();
    compatible = (deviceFp == backupFingerprint);
  }

  // Check if force flag is set (allows restore to different device)
  bool forceRestore = false;
  if (doc.containsKey("force")) {
    forceRestore = doc["force"].as<bool>();
  }

  // If not compatible and not forced, reject with compatibility info
  if (!compatible && !forceRestore) {
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

  // Write each file from the backup
  JsonObject files = doc["files"].as<JsonObject>();
  int filesWritten = 0;
  int filesErrored = 0;
  JsonDocument resultDoc;
  JsonArray warnings = resultDoc["warnings"].to<JsonArray>();

  for (JsonPair kv : files) {
    const char* path = kv.key().c_str();
    if (!path || path[0] != '/') {
      char warn[128];
      snprintf(warn, sizeof(warn), "Skipped invalid path: %s", path ? path : "(null)");
      warnings.add(warn);
      filesErrored++;
      continue;
    }

    // On different device: skip user credential files (passwords won't work)
    if (!compatible) {
      String pathStr = String(path);
      if (pathStr.startsWith("/system/users/")) {
        char warn[128];
        snprintf(warn, sizeof(warn), "Skipped (different device): %s", path);
        warnings.add(warn);
        continue;
      }
    }

    // Ensure parent directories exist
    String pathStr = String(path);
    int lastSlash = pathStr.lastIndexOf('/');
    if (lastSlash > 0) {
      String dir = pathStr.substring(0, lastSlash);
      LittleFS.mkdir(dir);
    }

    // Serialize the value to a string for writing
    String content;
    if (kv.value().is<const char*>()) {
      content = kv.value().as<String>();
    } else {
      serializeJsonPretty(kv.value(), content);
    }

    if (writeText(path, content)) {
      filesWritten++;
    } else {
      char warn[128];
      snprintf(warn, sizeof(warn), "Failed to write: %s", path);
      warnings.add(warn);
      filesErrored++;
    }
  }

  // Build response
  resultDoc["success"] = true;
  resultDoc["compatible"] = compatible;
  resultDoc["filesWritten"] = filesWritten;
  resultDoc["filesErrored"] = filesErrored;
  if (!compatible) {
    resultDoc["credentialsSkipped"] = true;
    resultDoc["message"] = "Non-sensitive data restored. Device will reboot into first-time setup for credential creation.";
  }

  httpd_resp_set_type(req, "application/json");
  String response;
  serializeJson(resultDoc, response);
  httpd_resp_send(req, response.c_str(), response.length());

  // Signal that restore is complete (the setup flow will handle reboot)
  gAcceptingRestore = false;
  gRestoreComplete = true;

  return ESP_OK;
}

// ============================================================================
// Handler Registration
// ============================================================================

void registerMigrationBackupHandler(httpd_handle_t server) {
  static httpd_uri_t backupPost = {
    .uri = "/api/backup",
    .method = HTTP_POST,
    .handler = handleBackup,
    .user_ctx = NULL
  };
  static httpd_uri_t backupOptions = {
    .uri = "/api/backup",
    .method = HTTP_OPTIONS,
    .handler = handleCorsOptions,
    .user_ctx = NULL
  };
  httpd_register_uri_handler(server, &backupPost);
  httpd_register_uri_handler(server, &backupOptions);
}

void registerPingOptionsHandler(httpd_handle_t server) {
  static httpd_uri_t pingOptions = {
    .uri = "/api/ping",
    .method = HTTP_OPTIONS,
    .handler = handleCorsOptions,
    .user_ctx = NULL
  };
  httpd_register_uri_handler(server, &pingOptions);
}

// Gate 1: Only register /api/restore when user selects "Import from Backup"
void registerMigrationRestoreHandler(httpd_handle_t server) {
  static httpd_uri_t restorePost = {
    .uri = "/api/restore",
    .method = HTTP_POST,
    .handler = handleRestore,
    .user_ctx = NULL
  };
  static httpd_uri_t restoreOptions = {
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
  const char* body =
    "HardwareOne Restore Mode\n"
    "\n"
    "This device is waiting for a backup restore.\n"
    "\n"
    "Do not use the normal web interface during this step.\n"
    "Do not try to configure this device from this page.\n"
    "\n"
    "Use the HardwareOne Migration Tool browser application\n"
    "on another device to send your .hwbackup file here.\n"
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
  String resp = "{\"ok\":true,\"mode\":\"restore\",\"fingerprint\":\"" + fp + "\"}";
  httpd_resp_send(req, resp.c_str(), resp.length());
  return ESP_OK;
}

void startRestoreOnlyHttpServer() {
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

  static httpd_uri_t splashGet = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = handleRestoreSplash,
    .user_ctx = NULL
  };
  static httpd_uri_t pingGet = {
    .uri = "/api/ping",
    .method = HTTP_GET,
    .handler = handlePingRestore,
    .user_ctx = NULL
  };
  static httpd_uri_t pingOptions = {
    .uri = "/api/ping",
    .method = HTTP_OPTIONS,
    .handler = handleCorsOptions,
    .user_ctx = NULL
  };
  static httpd_uri_t restorePost = {
    .uri = "/api/restore",
    .method = HTTP_POST,
    .handler = handleRestore,
    .user_ctx = NULL
  };
  static httpd_uri_t restoreOptions = {
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

  broadcastOutput("HTTP server started (restore-only mode)");
}

void stopRestoreOnlyHttpServer() {
  if (server) {
    httpd_stop(server);
    server = NULL;
  }
}

#endif // ENABLE_HTTP_SERVER
