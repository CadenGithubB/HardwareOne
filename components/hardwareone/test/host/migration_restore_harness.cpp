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

// SHA-256 boundary: records what was hashed and returns a digest whose first
// four bytes are fixed, so the summary's fingerprint text is predictable.
static std::string hashedInput;
static int mbedtls_sha256(const unsigned char* input, size_t length, unsigned char output[32], int is224) {
  check(is224 == 0);
  hashedInput.assign(reinterpret_cast<const char*>(input), length);
  for (int i = 0; i < 32; ++i) output[i] = static_cast<unsigned char>(0xc0 + i);
  return 0;
}

// free() boundary inside the production code: when the watched staged buffer
// is released, record the confirm flag a concurrent /api/restore would see.
static void harnessFree(void* pointer);
static const void* watchedBuffer = nullptr;
static bool watchedFreed = false;
static bool flagSetAtWatchedFree = false;

// INSERT_FS_GUARD
// INSERT_TEXT_IO
// INSERT_CONSTANTS
#define free(pointer) harnessFree(pointer)
// INSERT_PRODUCTION
#undef free
// INSERT_BACKUP_ADAPTER

static void harnessFree(void* pointer) {
  if (pointer && pointer == watchedBuffer) {
    watchedFreed = true;
    flagSetAtWatchedFree = sRestoreAwaitingConfirm;
  }
  std::free(pointer);
}

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

// Stage through the production path the /api/restore handler uses.
static void stage(const std::string& json, bool compatible) {
  char* buf = static_cast<char*>(std::calloc(json.size() + 1, 1));
  std::memcpy(buf, json.data(), json.size());
  JsonDocument doc;
  check(!deserializeJson(doc, json));
  stageMigrationRestore(buf, json.size(), compatible, doc.as<JsonVariantConst>());
}

// extra is inserted before "files", e.g. "\"force\":true,".
static std::string bundleFrom(const std::string& hostname, const char* board, const char* fingerprint = "fp",
                              const char* extra = "") {
  std::string json = bundle();
  json.replace(json.find("\"files\":{"), 0,
               std::string(extra) + "\"device\":{\"hostname\":\"" + hostname + "\",\"board\":\"" + board +
               "\",\"fingerprint\":\"" + fingerprint + "\"},");
  return json;
}

static bool contains(const String& text, const std::string& part) {
  return std::string(text.c_str()).find(part) != std::string::npos;
}

static std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t nl; (nl = text.find('\n', start)) != std::string::npos; start = nl + 1) out.push_back(text.substr(start, nl - start));
  out.push_back(text.substr(start));
  return out;
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

// A bundle may only name what a backup emits. Anything else is refused before
// a byte is staged, and the device's files stay as they were.
static void caseForeignPathRefused() {
  auto& fs = resetDevice();
  std::string json = bundle();
  json.replace(json.find("\"files\":{"), 9, "\"files\":{\"/system/sys_logs/errors.log\":\"planted\",");
  stage(json, true);
  check(!applyStagedMigrationRestore());
  check(!gRestoreComplete);
  check(fs.text(kSettings) == "{\"device\":\"old\"}");
  check(!fs.exists("/system/sys_logs/errors.log"));
  check(!fs.exists("/system/sys_logs/errors.log.restore.tmp"));
  check(noStagingLeft(fs));
}

// The map tree is the one place outside /system a backup carries; its files
// restore under the /maps scope and everything else under /system.
static void caseMapFileRestored() {
  auto& fs = resetDevice();
  fs.mkdirAll("/maps");
  std::string json = bundle();
  json.replace(json.find("\"files\":{"), 9, "\"files\":{\"/maps/tiles/a.txt\":\"tile\",");
  stage(json, true);
  check(applyStagedMigrationRestore());
  check(gRestoreComplete);
  check(fs.text("/maps/tiles/a.txt") == "tile");
  check(fs.text(kSettings).find("new") != std::string::npos);
  check(!fs.exists("/maps/tiles/a.txt.restore.tmp"));
  check(noStagingLeft(fs));
}

// The confirm prompt names the bundle's claimed source, the bytes received
// and their SHA-256 prefix, and whether it came from this device.
static void caseStagedSummary() {
  resetDevice();
  check(migrationStagedSummary().length() == 0);   // nothing staged
  const std::string json = bundleFrom("kitchen-hw1", "XIAO_ESP32S3");
  stage(json, true);
  check(hashedInput == json);                      // the hash covers exactly the staged bytes
  const std::string summary = migrationStagedSummary().c_str();
  const std::string size = std::to_string(json.size());
  check(summary == "from kitchen-hw1 [XIAO_ESP32S3], " + size + " bytes, sha256 c0c1c2c3..., this device");
  const std::string oled = migrationStagedOledPrompt().c_str();
  check(oled == "Apply kitchen-hw1?\n" + size + "B sha c0c1c2c3\nthis dev, reboots");
  for (const std::string& line : lines(oled)) check(line.size() <= 21);
  check(lines(oled).size() == 3);

  stage(bundleFrom("other-hw1", "FeatherS3"), false);
  check(std::string(migrationStagedSummary().c_str()).find("other device (credentials skipped)") != std::string::npos);
  check(std::string(migrationStagedOledPrompt().c_str()).find("other dev, reboots") != std::string::npos);

  discardStagedMigrationRestore();
  check(migrationStagedSummary().length() == 0);
  check(migrationStagedOledPrompt().length() == 0);
}

// Bundle claims are LAN input: terminal escapes are neutralised, long names cut,
// missing names shown as unknown, and the OLED lines still fit.
static void caseHostileClaims() {
  resetDevice();
  stage(bundleFrom("\\u001b[2J" + std::string(60, 'h'), "B\\u0007"), false);
  const std::string summary = migrationStagedSummary().c_str();
  check(summary.find('\x1b') == std::string::npos && summary.find('\x07') == std::string::npos);
  check(summary.find("from ?[2J" + std::string(28, 'h') + " [B?]") == 0);
  for (const std::string& line : lines(migrationStagedOledPrompt().c_str())) check(line.size() <= 21);
  stage(bundle(), true);                           // no device block at all
  check(std::string(migrationStagedSummary().c_str()).find("from unknown [unknown]") == 0);
  check(!contains(migrationStagedSummary(), "this device"));
  discardStagedMigrationRestore();
}

// "this device" needs a fingerprint that matched. A bundle with no fingerprint
// (handleRestore then leaves compatible true) is shown as unverified instead.
static void caseNoFingerprintUnverified() {
  resetDevice();
  for (const std::string& json : {bundle(), bundleFrom("kitchen-hw1", "XIAO_ESP32S3", "")}) {
    stage(json, true);
    check(!contains(migrationStagedSummary(), "this device"));
    check(contains(migrationStagedSummary(), ", no fingerprint (unverified, credentials restored)"));
    check(contains(migrationStagedOledPrompt(), "\nunverified, reboots"));
    for (const std::string& line : lines(migrationStagedOledPrompt().c_str())) check(line.size() <= 21);
  }
  discardStagedMigrationRestore();
}

// What the operator is told to compare: an unforced body may be the file as
// exported; a forced cross-device body has "force" added and cannot match it.
static void caseCheckAdvice() {
  resetDevice();
  check(migrationStagedCheckAdvice().length() == 0);
  stage(bundleFrom("kitchen-hw1", "XIAO_ESP32S3"), true);
  check(contains(migrationStagedCheckAdvice(), "shasum -a 256"));
  check(!contains(migrationStagedCheckAdvice(), "will not match"));
  stage(bundleFrom("other-hw1", "FeatherS3", "other-fp", "\"force\":true,"), false);
  check(contains(migrationStagedSummary(), "other device (credentials skipped)"));
  check(contains(migrationStagedCheckAdvice(), "will not match your file"));
  check(!contains(migrationStagedCheckAdvice(), "shasum"));
  discardStagedMigrationRestore();
  check(migrationStagedCheckAdvice().length() == 0);
}

// Discard releases the buffer while the confirm flag is still set, so a
// concurrent /api/restore (which refuses while the flag is set) can never see
// a cleared flag with the old buffer still live. Applying discards the same way.
static void caseDiscardFreesBeforeClearing() {
  resetDevice();
  stage(bundle(), true);
  watchedBuffer = sStagedRestoreBuf; watchedFreed = false; flagSetAtWatchedFree = false;
  discardStagedMigrationRestore();
  check(watchedFreed);
  check(flagSetAtWatchedFree);
  check(!sRestoreAwaitingConfirm && sStagedRestoreBuf == nullptr && sStagedRestoreLen == 0);

  stage(bundle(), true);
  watchedBuffer = sStagedRestoreBuf; watchedFreed = false; flagSetAtWatchedFree = false;
  check(applyStagedMigrationRestore());
  check(watchedFreed && flagSetAtWatchedFree);
  check(!sRestoreAwaitingConfirm && sStagedRestoreBuf == nullptr);
  watchedBuffer = nullptr;
}

// The bundle's formatVersion: no field is format 1 (older exports), a whole
// number up to kBackupFormatVersion is supported, a newer one or anything
// that is not a whole number is refused (handleRestore then stages nothing).
static void caseBackupFormatVersion() {
  check(kBackupFormatVersion == 1);
  struct Want { const char* json; BackupFormatCheck result; uint32_t version; };
  const Want wants[] = {
      {"{\"magic\":\"HWBACKUP\",\"files\":{}}", BackupFormatCheck::Supported, 1},
      {"{\"magic\":\"HWBACKUP\",\"formatVersion\":1}", BackupFormatCheck::Supported, 1},
      {"{\"magic\":\"HWBACKUP\",\"formatVersion\":0}", BackupFormatCheck::Supported, 0},
      {"{\"magic\":\"HWBACKUP\",\"formatVersion\":2}", BackupFormatCheck::Newer, 2},
      {"{\"magic\":\"HWBACKUP\",\"formatVersion\":4000000000}", BackupFormatCheck::Newer, 4000000000u},
      {"{\"magic\":\"HWBACKUP\",\"formatVersion\":\"1\"}", BackupFormatCheck::Invalid, 0},
      {"{\"magic\":\"HWBACKUP\",\"formatVersion\":1.5}", BackupFormatCheck::Invalid, 0},
      {"{\"magic\":\"HWBACKUP\",\"formatVersion\":-1}", BackupFormatCheck::Invalid, 0},
      {"{\"magic\":\"HWBACKUP\",\"formatVersion\":null}", BackupFormatCheck::Supported, 1},
  };
  for (const Want& want : wants) {
    JsonDocument doc;
    check(!deserializeJson(doc, want.json));
    uint32_t version = 99;
    check(checkBackupFormat(doc.as<JsonVariantConst>(), &version) == want.result);
    check(version == want.version);
  }
  JsonDocument doc;
  check(!deserializeJson(doc, bundle()));
  check(checkBackupFormat(doc.as<JsonVariantConst>(), nullptr) == BackupFormatCheck::Supported);
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
      {"foreign-path-refused", caseForeignPathRefused},
      {"map-file-restored", caseMapFileRestored},
      {"staged-summary", caseStagedSummary},
      {"hostile-claims", caseHostileClaims},
      {"no-fingerprint-unverified", caseNoFingerprintUnverified},
      {"check-advice", caseCheckAdvice},
      {"discard-frees-before-clearing", caseDiscardFreesBeforeClearing},
      {"backup-format-version", caseBackupFormatVersion},
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
