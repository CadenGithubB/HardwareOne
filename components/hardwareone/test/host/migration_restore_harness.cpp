// Drives the production migration restore (applyStagedMigrationRestore ->
// writeRestoreFilesFromDoc) and the backup directory walk from
// WebServer_MigrationTool.cpp against the shared fake VFS with real ArduinoJson,
// the real FsLockGuard and the real checked text writers. Crypto, events and
// output are doubles.
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#define FAKE_VFS_LOCK_DOUBLE
#include <Arduino.h>
#include "fake_vfs.h"
#include <ArduinoJson.h>
#include "System_PollPause.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}
volatile bool gSensorPollingPaused = false;
void pollPause(uint8_t) {}
void pollResume(uint8_t) {}
bool pollPaused(uint8_t) { return false; }

static unsigned checks = 0;
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "migration check failed at line %d: %s\n", line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

// ---------------------------------------------------------------------------
// Doubles
// ---------------------------------------------------------------------------
#define PSRAM_JSON_DOC(name) JsonDocument name
#define DEBUG_HTTPF(...) ((void)0)
#define ERROR_STORAGEF(...) ((void)0)
bool filesystemReady = true;
volatile bool gAcceptingRestore = true;
volatile bool gRestoreComplete = false;
const char* PENDING_CRED_SETUP_FILE = "/system/.pending_credential_setup";

static std::vector<std::string> output, events;
static void broadcastOutput(const String& line) { output.emplace_back(line.c_str()); }
enum : int { SYSEVT_CONFIG_FILE_CORRUPT = 1, SYSEVT_BACKUP_RESTORED };
static void systemEventPost(int kind, const char*, const char*) { events.emplace_back(std::to_string(kind)); }
static String encryptString(const String& plain) { return String(std::string("AES:") + plain.c_str()); }
static String decryptString(const String& sealed) {
  std::string text(sealed.c_str());
  return text.rfind("AES:", 0) == 0 ? String(text.substr(4)) : String("");
}
static bool canRead(const String&, const AuthContext&) { return true; }

// INSERT_FS_GUARD
// INSERT_TEXT_IO
// INSERT_CONSTANTS
// INSERT_PRODUCTION
// INSERT_BACKUP_ADAPTER

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
static const char* kSettings = "/system/settings.json";
static const char* kAutomations = "/system/automations.json";
static const char* kUsers = "/system/users/users.json";
static const char* kUserSettings = "/system/users/user_settings/2.json";

static fake_vfs::Fs& resetDevice() {
  auto& fs = fake_vfs::fs();
  fs.reset();
  fake_vfs::wireLockDouble();
  gFakeFsSemaphore = FakeFsSemaphore{};
  fs.mkdirAll("/system/users/user_settings");
  fs.put(kSettings, "{\"device\":\"old\"}");
  fs.put(kAutomations, "{\"automations\":[]}");
  output.clear(); events.clear();
  gAcceptingRestore = true;
  gRestoreComplete = false;
  return fs;
}

// Same-device bundle: users.json first, as a migration client may order it.
static std::string bundle(const char* fingerprint = nullptr) {
  std::string json = "{\"magic\":\"HWBACKUP\",\"formatVersion\":1,";
  if (fingerprint) json += std::string("\"device\":{\"fingerprint\":\"") + fingerprint + "\"},";
  json += "\"files\":{"
          "\"/system/users/users.json\":{\"users\":[{\"id\":2,\"username\":\"alice\"}]},"
          "\"/system/settings.json\":{\"device\":\"new\"},"
          "\"/system/automations.json\":{\"automations\":[{\"id\":1}]},"
          "\"/system/users/user_settings/2.json\":{\"password\":\"hash\"}"
          "}}";
  return json;
}

static void stage(const std::string& json, bool compatible) {
  discardStagedMigrationRestore();
  sStagedRestoreBuf = static_cast<char*>(std::calloc(json.size() + 1, 1));
  std::memcpy(sStagedRestoreBuf, json.data(), json.size());
  sStagedRestoreLen = json.size();
  sStagedRestoreCompatible = compatible;
  sRestoreAwaitingConfirm = true;
}

static void failWritesTo(fake_vfs::Fs& fs, const std::string& path) {
  // Every name a writer may use for this destination accepts no bytes.
  for (const std::string& name : {path, path + ".tmp", path + ".restore.tmp"}) fs.faults.shortWriteAfter[name] = 0;
}

static bool noStagingLeft(const fake_vfs::Fs& fs) {
  for (const char* path : {kSettings, kAutomations, kUsers, kUserSettings, "/system/.pending_credential_setup"}) {
    const std::string p(path);
    if (fs.exists(p + ".tmp") || fs.exists(p + ".restore.tmp")) return false;
  }
  return true;
}

static void caseOneFailingWrite() {
  auto& fs = resetDevice();
  failWritesTo(fs, kAutomations);
  stage(bundle(), true);
  check(!applyStagedMigrationRestore());       // a partial restore is a failure
  check(!gRestoreComplete);                    // so setup does not reboot into it
  check(gAcceptingRestore);
  check(fs.text(kSettings) == "{\"device\":\"old\"}");
  check(fs.text(kAutomations) == "{\"automations\":[]}");
  check(!fs.exists(kUsers));                   // no account roster without its credentials
  check(!fs.exists(kUserSettings));
  check(noStagingLeft(fs));
  check(events.empty());
}

static void caseAllWritesSucceed() {
  auto& fs = resetDevice();
  stage(bundle(), true);
  check(applyStagedMigrationRestore());
  check(gRestoreComplete);
  check(fs.text(kSettings).find("new") != std::string::npos);
  check(fs.text(kAutomations).find("\"id\"") != std::string::npos);
  check(fs.exists(kUsers) && fs.exists(kUserSettings));
  check(noStagingLeft(fs));
  // users.json is the last file moved into place.
  std::string lastRename;
  for (const auto& call : fs.log) if (call.op == "rename" && call.ok) lastRename = call.other;
  check(lastRename == kUsers);
}

static void caseCrossDeviceMarkerFailure() {
  auto& fs = resetDevice();
  failWritesTo(fs, "/system/.pending_credential_setup");
  stage(bundle(), false);
  check(!applyStagedMigrationRestore());
  check(!gRestoreComplete);
  check(fs.text(kSettings) == "{\"device\":\"old\"}");
  check(!fs.exists("/system/.pending_credential_setup"));
  check(noStagingLeft(fs));
}

static void caseCrossDeviceSuccess() {
  auto& fs = resetDevice();
  stage(bundle(), false);
  check(applyStagedMigrationRestore());
  check(gRestoreComplete);
  check(fs.text("/system/.pending_credential_setup") == "1");
  check(!fs.exists(kUsers));                   // credentials are never carried across devices
  check(fs.text(kSettings).find("new") != std::string::npos);
  check(noStagingLeft(fs));
}

static void caseBackupWalk() {
  auto& fs = resetDevice();
  const std::string longDir = "/maps/" + std::string(120, 'd');
  const std::string nested = longDir + "/tile.hwmap";
  fs.put(nested, "nested-map");
  fs.put("/maps/small.hwmap", std::string(1024, 's'));
  fs.put("/maps/big.hwmap", std::string(600 * 1024, 'b'));
  JsonDocument doc;
  JsonObject files = doc["files"].to<JsonObject>();
  JsonArray warnings = doc["warnings"].to<JsonArray>();
  addMapsToBackup(doc, files, warnings);
  check(nested.size() > 128);
  check(files[nested.c_str()].as<std::string>() == "nested-map");  // full nested path
  check(files["/maps/small.hwmap"].is<const char*>());
  check(files["/maps/big.hwmap"].isNull());    // would exceed what the restore accepts
  bool warned = false;
  for (JsonVariantConst w : warnings) {
    if (std::strstr(w.as<const char*>(), "/maps/big.hwmap")) warned = true;
  }
  check(warned);
  check(measureJson(doc) <= 512u * 1024u);
}

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  struct Case { const char* name; void (*run)(); };
  const Case cases[] = {
      {"one-failing-write", caseOneFailingWrite},
      {"all-writes-succeed", caseAllWritesSucceed},
      {"cross-device-marker-failure", caseCrossDeviceMarkerFailure},
      {"cross-device-success", caseCrossDeviceSuccess},
      {"backup-walk", caseBackupWalk},
  };
  bool ran = false;
  for (const Case& c : cases) {
    if (!only.empty() && only != c.name) continue;
    c.run();
    ran = true;
    std::printf("ok %s\n", c.name);
  }
  check(ran);
  discardStagedMigrationRestore();
  std::printf("migration restore: %u checks passed\n", checks);
  return 0;
}
