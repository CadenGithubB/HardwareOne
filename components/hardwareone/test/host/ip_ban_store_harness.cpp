// Drives the production IP ban list from WebServer_Server.cpp (load, save,
// ban, unban, the httpd-side lookup, the JSON listing) against the shared fake
// VFS with real ArduinoJson and the real checked text writers. Encryption,
// sessions, events and output are doubles.
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#define FAKE_VFS_LOCK_DOUBLE
#include <Arduino.h>
#include "fake_vfs.h"
#include <ArduinoJson.h>
#include "System_PollPause.h"

#include <atomic>
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
  std::fprintf(stderr, "ip ban check failed at line %d: %s\n", line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

// ---------------------------------------------------------------------------
// Doubles
// ---------------------------------------------------------------------------
#define EXT_RAM_BSS_ATTR
#define PSRAM_JSON_DOC(name) JsonDocument name
#define DEBUG_AUTHF(...) ((void)0)
bool filesystemReady = true;

static std::vector<std::string> output, events;
static void broadcastPrintf(const char* format, ...) {
  char line[512];
  va_list args; va_start(args, format); std::vsnprintf(line, sizeof(line), format, args); va_end(args);
  output.emplace_back(line);
}
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)
static void broadcastOutput(const char* line) { output.emplace_back(line); }
enum : int { SYSEVT_IP_BANNED = 1 };
static void systemEventPost(int, const char* subject, const char*) { events.emplace_back(subject ? subject : ""); }
static unsigned long millis() { return 1234; }
static void clearLoginAttempts(const char*) {}

// Reversible stand-in for the device AES helpers; the prefix is what the
// production loader keys on.
static bool encryptionFails = false;
static String encryptString(const String& plain) {
  if (encryptionFails) return String("");
  std::string text(plain.c_str());
  return String("AES:" + std::string(text.rbegin(), text.rend()));
}
static String decryptString(const String& sealed) {
  std::string text(sealed.c_str());
  if (text.rfind("AES:", 0) != 0) return String("");
  text = text.substr(4);
  return String(std::string(text.rbegin(), text.rend()));
}

#define MAX_SESSIONS 4
struct SessionEntry { String sid; String ip; };
static SessionEntry sessionStorage[MAX_SESSIONS];
static SessionEntry* gSessions = sessionStorage;
static std::vector<int> revoked;

enum class AllocPref : uint8_t { PreferPSRAM };
static void* ps_calloc(size_t n, size_t size, AllocPref, const char*) { return std::calloc(n, size); }

constexpr size_t GLOBAL_DEBUG_BUFFER_SIZE = 4096;
static char debugBuffer[GLOBAL_DEBUG_BUFFER_SIZE];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return debugBuffer; }

// Recursive-mutex double for the ban table lock, separate from the FS lock.
struct FakeRecursiveMutex { unsigned depth = 0, takes = 0; };
using StaticSemaphore_t = FakeRecursiveMutex;
static unsigned banLockDepth = 0, banLockTakes = 0;
static SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t*) {
  static FakeFsSemaphore handle;  // only its identity is used
  return &handle;
}
static int xSemaphoreTakeRecursive(SemaphoreHandle_t, TickType_t) { ++banLockDepth; ++banLockTakes; return pdTRUE; }
static int xSemaphoreGiveRecursive(SemaphoreHandle_t) { --banLockDepth; return pdTRUE; }

// INSERT_FS_GUARD
// INSERT_TEXT_IO
// INSERT_TYPES
// INSERT_PRODUCTION

void enqueueTargetedRevokeForSessionIdx(int idx, const String&) { revoked.push_back(idx); }

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
static const std::string kBanFile = IP_BANS_FILE;
static const std::string kBanTemp = kBanFile + ".tmp";
static const std::string kPrevious =
    "{\"bans\":[{\"ip\":\"AES:1.1.1.01\",\"reason\":\"old\",\"bannedAt\":5}]}";  // 10.1.1.1

static fake_vfs::Fs& resetStore(bool withPrevious) {
  auto& fs = fake_vfs::fs();
  fs.reset();
  fake_vfs::wireLockDouble();
  gFakeFsSemaphore = FakeFsSemaphore{};
  fs.mkdirAll("/system/users");
  if (withPrevious) fs.put(kBanFile, kPrevious);
  output.clear(); events.clear(); revoked.clear();
  encryptionFails = false;
  for (auto& session : sessionStorage) session = SessionEntry{};
  loadIpBans();
  return fs;
}

static void failBanWrites(fake_vfs::Fs& fs) {
  // The direct file and its checked-writer temp both accept only 10 bytes.
  fs.faults.shortWriteAfter[kBanFile] = 10;
  fs.faults.shortWriteAfter[kBanTemp] = 10;
}

static bool anyOutputContains(const char* needle) {
  for (const auto& line : output) if (line.find(needle) != std::string::npos) return true;
  return false;
}

static void caseFailedBanSave() {
  auto& fs = resetStore(true);
  check(isIpBanned("10.1.1.1"));
  failBanWrites(fs);
  const bool ok = banIp("10.2.2.2", "probe");
  check(!ok);                                   // the failed save is reported
  check(fs.text(kBanFile) == kPrevious);        // the previous list is intact
  check(isIpBanned("10.2.2.2"));                // fail closed: the RAM ban holds
  check(isIpBanned("10.1.1.1"));
  check(anyOutputContains("not saved"));
  check(!fs.exists(kBanTemp));                  // no stray temp

  // Banning again retries the save once the filesystem recovers.
  fs.faults.clear();
  check(banIp("10.2.2.2", "probe"));
  loadIpBans();                                 // as at the next boot
  check(isIpBanned("10.1.1.1") && isIpBanned("10.2.2.2"));
}

static void caseFailedUnbanSave() {
  auto& fs = resetStore(true);
  failBanWrites(fs);
  check(!unbanIp("10.1.1.1"));
  check(fs.text(kBanFile) == kPrevious);
  check(isIpBanned("10.1.1.1"));                // memory matches the saved list
  fs.faults.clear();
  check(unbanIp("10.1.1.1"));
  check(!isIpBanned("10.1.1.1"));
  loadIpBans();
  check(!isIpBanned("10.1.1.1"));
}

static void caseEncryptionFailureRefusesSave() {
  auto& fs = resetStore(true);
  encryptionFails = true;
  check(!banIp("10.3.3.3", nullptr));
  check(fs.text(kBanFile) == kPrevious);        // never a list missing an entry
  check(isIpBanned("10.3.3.3"));
}

static std::string ipv6(int n) {
  char text[48];
  std::snprintf(text, sizeof(text), "fd00:1111:2222:3333:4444:5555:6666:%04x", n);  // 39 characters
  return text;
}

static void caseFullListJson() {
  auto& fs = resetStore(false);
  const std::string reason(79, 'r');
  for (int i = 0; i < 16; ++i) check(banIp(ipv6(i).c_str(), reason.c_str()));
  check(!banIp("10.9.9.9", "seventeenth"));     // the list is full
  const char* json = banListJson();
  JsonDocument doc;
  check(!deserializeJson(doc, json));           // complete, valid JSON
  check(doc["count"].as<int>() == 16);
  check(doc["bans"].size() == 16);
  for (JsonObjectConst ban : doc["bans"].as<JsonArrayConst>()) {
    check(std::strlen(ban["reason"] | "") == 79);
    check(std::strlen(ban["ip"] | "") == 39);
  }
  // The saved file round-trips too.
  JsonDocument saved;
  check(!deserializeJson(saved, fs.text(kBanFile)));
  check(saved["bans"].size() == 16);
  loadIpBans();
  check(isIpBanned(ipv6(15).c_str()));
}

static void caseOversizeJsonIsAnError() {
  resetStore(false);
  // Control characters escape to six bytes each: 16 such reasons need far
  // more than the 4 KB reply buffer.
  const std::string reason(79, '\x01');
  for (int i = 0; i < 16; ++i) check(banIp(ipv6(i).c_str(), reason.c_str()));
  const char* json = banListJson();
  JsonDocument doc;
  check(!deserializeJson(doc, json));           // never a truncated document
  check(doc["error"].is<const char*>());
  check(doc["bans"].isNull());
}

static void caseLookupHoldsTheBanLock() {
  auto& fs = resetStore(true);
  bool fsCallUnderBanLock = false;
  fs.onCall = [&](const fake_vfs::Call&) { if (banLockDepth != 0) fsCallUnderBanLock = true; };
  const unsigned before = banLockTakes;
  check(isIpBanned("10.1.1.1"));
  check(banLockTakes > before);                 // the httpd reader takes the table lock
  check(banIp("10.4.4.4", nullptr));
  check(unbanIp("10.4.4.4"));
  (void)banListJson();
  loadIpBans();
  check(!fsCallUnderBanLock);                   // file I/O never runs under it
  check(banLockDepth == 0);
  fs.onCall = nullptr;
}

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  struct Case { const char* name; void (*run)(); };
  const Case cases[] = {
      {"failed-ban-save", caseFailedBanSave},
      {"failed-unban-save", caseFailedUnbanSave},
      {"encryption-failure", caseEncryptionFailureRefusesSave},
      {"full-list-json", caseFullListJson},
      {"oversize-json", caseOversizeJsonIsAnError},
      {"lookup-lock", caseLookupHoldsTheBanLock},
  };
  bool ran = false;
  for (const Case& c : cases) {
    if (!only.empty() && only != c.name) continue;
    c.run();
    ran = true;
    std::printf("ok %s\n", c.name);
  }
  check(ran);
  std::printf("ip ban store: %u checks passed\n", checks);
  return 0;
}
