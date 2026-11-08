// Host harness for the ESP-NOW identity writers of System_ESPNow_Identity.cpp.
//
// test_espnow_identity_writes.py splices the production helpers (hex
// encoding, directory creation, peer paths, the checked JSON promote and both
// writers) in at the insertion marker below. ArduinoJson is the vendored
// library. The filesystem is an in-memory double with a shared free-space
// budget: once it runs out, write() stores and reports only the bytes that
// fit, the way LittleFS does when the partition is nearly full. A second mode
// stores fewer bytes than write() reports, so the size check is exercised on
// its own. File writes assert the filesystem lock is held.
#include <ArduinoJson.h>

#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using String = std::string;

// ---- Logging ---------------------------------------------------------------
static std::vector<std::string> gLog;
static void logf(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  gLog.emplace_back(buf);
}
#define ERROR_ESPNOWF(...) logf(__VA_ARGS__)
static bool logHas(const char* needle) {
  for (const auto& l : gLog) if (l.find(needle) != std::string::npos) return true;
  return false;
}

// ---- Production dependencies ----------------------------------------------
static bool filesystemReady = true;
static String encryptString(const String& plain) { return "AES:00112233:" + plain; }
static void macToPathToken(const uint8_t mac[6], char* out13) {
  std::snprintf(out13, 13, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
static void macToDisplay(const uint8_t mac[6], char* out, size_t outLen) {
  std::snprintf(out, outLen, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ---- Filesystem lock -------------------------------------------------------
static int gFsLockDepth = 0;
struct FsLockGuard {
  explicit FsLockGuard(const char*) { ++gFsLockDepth; }
  ~FsLockGuard() { --gFsLockDepth; }
};

// ---- In-memory VFS ---------------------------------------------------------
struct AuthContext { std::string scope, reason; };
static std::map<std::string, std::shared_ptr<std::string>> gFiles;
static std::set<std::string> gDirs;
static size_t gFreeBytes = SIZE_MAX;  // shared budget; writes past it are cut short
static bool gStoreLess = false;       // write() reports n but stores n-1 (lying backend)
static bool gRenameFail = false;
static int gRenames = 0;

class File {
 public:
  File() = default;
  explicit File(std::shared_ptr<std::string> data) : data_(std::move(data)) {}
  explicit operator bool() const { return data_ != nullptr; }
  size_t write(uint8_t c) { return write(&c, 1); }
  size_t write(const uint8_t* p, size_t n) {
    assert(data_ && gFsLockDepth > 0);
    size_t take = n < gFreeBytes ? n : gFreeBytes;
    if (gFreeBytes != SIZE_MAX) gFreeBytes -= take;
    size_t keep = take;
    if (gStoreLess && keep) --keep;
    data_->append(reinterpret_cast<const char*>(p), keep);
    return take;
  }
  void flush() { assert(data_ && gFsLockDepth > 0); }
  size_t size() const { assert(data_ && gFsLockDepth > 0); return data_->size(); }
  void close() { data_.reset(); }

 private:
  std::shared_ptr<std::string> data_;
};

namespace VFS {
namespace Scopes {
inline constexpr const char* SYSTEM_STATE = "/system";
inline constexpr const char* ESPNOW_STATE = "/system/espnow";
}  // namespace Scopes
AuthContext systemAuth(const char* scope, const char* reason) { return AuthContext{scope, reason}; }
static bool inScope(const std::string& path, const AuthContext& auth) {
  return path.compare(0, auth.scope.size(), auth.scope) == 0;
}
bool existsGuarded(const std::string& path, const AuthContext& auth) {
  assert(inScope(path, auth));
  return gDirs.count(path) || gFiles.count(path);
}
bool mkdirGuarded(const std::string& path, const AuthContext& auth) {
  assert(inScope(path, auth));
  gDirs.insert(path);
  return true;
}
File openGuarded(const std::string& path, const char* mode, const AuthContext& auth, bool = false) {
  assert(inScope(path, auth) && std::strcmp(mode, "w") == 0 && gFsLockDepth > 0);
  auto data = std::make_shared<std::string>();
  gFiles[path] = data;
  return File(data);
}
bool removeGuarded(const std::string& path, const AuthContext& auth) {
  assert(inScope(path, auth));
  return gFiles.erase(path) != 0;
}
bool renameGuarded(const std::string& from, const std::string& to, const AuthContext& auth) {
  assert(inScope(from, auth) && inScope(to, auth) && gFsLockDepth > 0);
  ++gRenames;
  if (gRenameFail || !gFiles.count(from)) return false;
  gFiles[to] = gFiles[from];
  gFiles.erase(from);
  return true;
}
}  // namespace VFS

// ---- Production code -------------------------------------------------------
// INSERT_TYPES
namespace {
// INSERT_PRODUCTION
}  // namespace

// ---- Cases -----------------------------------------------------------------
static int gFailures = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++gFailures;
  }
}

static const char* kOld = "{\"version\":1,\"previous\":\"identity\"}";

static void reset() {
  gFiles.clear();
  gDirs = {"/system"};
  gLog.clear();
  gFreeBytes = SIZE_MAX;
  gStoreLess = gRenameFail = false;
  gRenames = 0;
}
static bool has(const char* path) { return gFiles.count(path) != 0; }
static std::string content(const char* path) { return has(path) ? *gFiles[path] : std::string(); }
static void seed(const char* path) { gFiles[path] = std::make_shared<std::string>(kOld); }

static EspNowIdentity sampleIdentity() {
  EspNowIdentity id{};
  for (int i = 0; i < 32; ++i) id.pub[i] = static_cast<uint8_t>(i);
  for (int i = 0; i < 64; ++i) id.sec[i] = static_cast<uint8_t>(0xA0 + i);
  id.createdAtSec = 1762560000u;
  id.regenCount = 3;
  id.valid = true;
  return id;
}
static PeerIdentity samplePeer() {
  PeerIdentity p{};
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  std::memcpy(p.mac, mac, 6);
  p.meshId = 1;
  for (int i = 0; i < 32; ++i) p.longTermPub[i] = static_cast<uint8_t>(0x40 + i);
  p.bondedAtSec = 1762560000u;
  p.lastSeenSec = 1762570000u;
  p.subscribedEvents = 0xFFFFFFFFu;
  p.valid = true;
  return p;
}

static const char* kId = "/system/espnow/identity.json";
static const char* kIdTmp = "/system/espnow/identity.tmp";
static const char* kPeer = "/system/espnow/peers/AABBCCDDEEFF/identity.json";
static const char* kPeerTmp = "/system/espnow/peers/AABBCCDDEEFF/identity.tmp";

static size_t completeSize(bool peer) {
  reset();
  const bool ok = peer ? writePeerIdentityFile(samplePeer()) : writeIdentityFile(sampleIdentity());
  check(ok, peer ? "peer identity: normal write succeeds" : "identity: normal write succeeds");
  const char* dest = peer ? kPeer : kId;
  check(!has(peer ? kPeerTmp : kIdTmp), "normal write leaves no temp");
  JsonDocument doc;
  check(!deserializeJson(doc, content(dest)), "normal write is complete JSON");
  check(doc["version"].as<int>() == (peer ? 2 : 1), "normal write carries the schema version");
  check(gFsLockDepth == 0, "filesystem lock released");
  return content(dest).size();
}

static void shortWrite(bool peer, size_t full, size_t budget, const char* what) {
  reset();
  const char* dest = peer ? kPeer : kId;
  const char* tmp = peer ? kPeerTmp : kIdTmp;
  gDirs.insert("/system/espnow");
  if (peer) {
    gDirs.insert("/system/espnow/peers");
    gDirs.insert("/system/espnow/peers/AABBCCDDEEFF");
  }
  seed(dest);
  gFreeBytes = budget;
  const bool ok = peer ? writePeerIdentityFile(samplePeer()) : writeIdentityFile(sampleIdentity());
  check(!ok, what);
  check(content(dest) == kOld, "short write leaves the previous file intact");
  check(!has(tmp), "short write removes the partial temp");
  check(gRenames == 0, "short write never reaches the rename");
  check(budget >= full || logHas("incomplete write"), "short write is logged");
  check(gFsLockDepth == 0, "filesystem lock released after a short write");
}

static void lyingBackend(bool peer) {
  reset();
  const char* dest = peer ? kPeer : kId;
  gDirs.insert("/system/espnow");
  if (peer) {
    gDirs.insert("/system/espnow/peers");
    gDirs.insert("/system/espnow/peers/AABBCCDDEEFF");
  }
  seed(dest);
  gStoreLess = true;
  const bool ok = peer ? writePeerIdentityFile(samplePeer()) : writeIdentityFile(sampleIdentity());
  check(!ok, "a file shorter than the reported byte count is refused");
  check(content(dest) == kOld, "size mismatch leaves the previous file intact");
  check(!has(peer ? kPeerTmp : kIdTmp), "size mismatch removes the temp");
}

static void renameFailure(bool peer) {
  reset();
  const char* dest = peer ? kPeer : kId;
  gDirs.insert("/system/espnow");
  if (peer) {
    gDirs.insert("/system/espnow/peers");
    gDirs.insert("/system/espnow/peers/AABBCCDDEEFF");
  }
  seed(dest);
  gRenameFail = true;
  const bool ok = peer ? writePeerIdentityFile(samplePeer()) : writeIdentityFile(sampleIdentity());
  check(!ok, "rename failure is reported");
  check(content(dest) == kOld, "rename failure leaves the previous file intact");
  check(!has(peer ? kPeerTmp : kIdTmp), "rename failure removes the temp");
}

int main() {
  for (bool peer : {false, true}) {
    const size_t full = completeSize(peer);
    check(full > 40, "fixture JSON has a realistic size");
    // A partition with room for everything but the closing bytes: the
    // write count is positive, which the old written==0 check accepted.
    shortWrite(peer, full, full - 1, "one byte short is refused");
    shortWrite(peer, full, full / 2, "half a file is refused");
    shortWrite(peer, full, 0, "a full partition is refused");
    lyingBackend(peer);
    renameFailure(peer);
    // Exactly enough room still succeeds.
    reset();
    gFreeBytes = full;
    check(peer ? writePeerIdentityFile(samplePeer()) : writeIdentityFile(sampleIdentity()),
          "a write that exactly fits succeeds");
    check(content(peer ? kPeer : kId).size() == full, "exact fit stores the whole file");
  }
  if (gFailures) {
    std::fprintf(stderr, "espnow_identity_writes: %d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("espnow_identity_writes: OK\n");
  return 0;
}
