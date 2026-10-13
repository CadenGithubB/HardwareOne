// Host harness for the bond JSON buffer path of System_ESPNow.cpp.
//
// test_espnow_bond_json.py splices the production serializer, checked writer,
// manifest/schema receivers, manifest cache and cached-manifest reader in at
// the insertion marker below. ArduinoJson is the vendored library; the filesystem is an
// in-memory double whose file reads and writes assert the filesystem lock is
// held, and whose write and read sizes can be capped to model a full disk or a
// failing flash read. ps_alloc can be made to fail and counts live buffers.
#include <ArduinoJson.h>

#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

// ---- Arduino String (only c_str() is used by the spliced code) -------------
class String {
 public:
  String(const char* s = "") : s_(s ? s : "") {}
  const char* c_str() const { return s_.c_str(); }

 private:
  std::string s_;
};

// ---- Allocation ------------------------------------------------------------
enum class AllocPref { PreferPSRAM, PreferInternal };
static bool gAllocFail = false;
static int gLiveAllocs = 0;
static size_t gLastAllocSize = 0;
void* ps_alloc(size_t size, AllocPref, const char*) {
  gLastAllocSize = size;
  if (gAllocFail) return nullptr;
  ++gLiveAllocs;
  return std::malloc(size);
}
void ps_free(void* p) {
  if (!p) return;
  --gLiveAllocs;
  std::free(p);
}

// ---- Debug output queue (System_Debug owns the real one) ------------------
// When the model is on, broadcastOutput feeds a 64-line queue (the size on
// boards without PSRAM) that drops lines when full, and every vTaskDelay lets
// the console print one line.
using QueueHandle_t = void*;
using UBaseType_t = unsigned;
using TickType_t = uint32_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
static int gQueueToken = 0;
static bool gQueueModel = false;
static unsigned gQueued = 0, gQueuePeak = 0, gQueueDrops = 0;
static std::string gPrinted;
QueueHandle_t getDebugQueue() { return gQueueModel ? &gQueueToken : nullptr; }
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t) { return gQueued; }
void vTaskDelay(TickType_t) {
  if (gQueued) --gQueued;
}

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
static void broadcastOutput(const char* s) {
  if (!gQueueModel) {
    gLog.emplace_back(s);
    return;
  }
  if (gQueued >= 64) {
    ++gQueueDrops;
    return;
  }
  if (++gQueued > gQueuePeak) gQueuePeak = gQueued;
  gPrinted += s;
}
#define ERROR_ESPNOWF(...) logf(__VA_ARGS__)
#define BROADCAST_PRINTF(...) logf(__VA_ARGS__)
#define DEBUG_ESPNOWF(...) do { } while (0)
static bool logHas(const char* needle) {
  for (const auto& l : gLog) if (l.find(needle) != std::string::npos) return true;
  return false;
}

// ---- Production constants and state --------------------------------------
constexpr uint32_t kFileSlotMaxFileSize = 131072;
static bool filesystemReady = true;
enum BondSyncState : uint8_t { BOND_SYNC_NONE = 0, BOND_SYNC_MANIFEST = 1 };
struct CapabilitySummary { uint8_t fwHash[16]; };
struct EspNowState {
  bool bondPeerOnline = true;
  uint8_t bondSyncInFlight = BOND_SYNC_MANIFEST;
  uint8_t bondSyncRetryCount = 2;
  bool bondManifestReceived = false;
  bool bondSchemaReceived = false;
  bool lastRemoteCapValid = true;
  CapabilitySummary lastRemoteCap{};
};
static EspNowState gEspNowState;
static EspNowState* gEspNow = &gEspNowState;
struct Settings { int bondRole = 1; };
static Settings gSettings;

// ---- Filesystem lock -------------------------------------------------------
static int gFsLockDepth = 0;
struct FsLockGuard {
  explicit FsLockGuard(const char*) { ++gFsLockDepth; }
  ~FsLockGuard() { --gFsLockDepth; }
};

// ---- In-memory VFS ---------------------------------------------------------
struct AuthContext { std::string reason; };
static std::map<std::string, std::shared_ptr<std::string>> gFiles;
static std::map<std::string, bool> gDirs;
static int gFsCalls = 0;
static size_t gWriteCap = SIZE_MAX;   // bytes one write() may store
static size_t gReadChunk = 4096;      // bytes one read() returns at most
static size_t gReadFailAt = SIZE_MAX; // read() returns 0 from this offset on
static bool gOpenFail = false;

class File {
 public:
  File() = default;
  File(std::shared_ptr<std::string> data, bool writable) : data_(std::move(data)), writable_(writable) {}
  explicit operator bool() const { return data_ != nullptr; }
  size_t size() const { assert(data_); return data_->size(); }
  size_t write(const uint8_t* p, size_t n) {
    assert(data_ && writable_ && gFsLockDepth > 0);
    const size_t take = n < gWriteCap ? n : gWriteCap;
    data_->append(reinterpret_cast<const char*>(p), take);
    return take;
  }
  int read() {
    assert(data_ && gFsLockDepth > 0);
    if (pos_ >= data_->size() || pos_ >= gReadFailAt) return -1;
    return static_cast<unsigned char>((*data_)[pos_++]);
  }
  size_t read(uint8_t* out, size_t n) {
    assert(data_ && gFsLockDepth > 0);
    if (pos_ >= gReadFailAt) return 0;
    size_t avail = data_->size() - pos_;
    if (avail > n) avail = n;
    if (avail > gReadChunk) avail = gReadChunk;
    if (gReadFailAt != SIZE_MAX && pos_ + avail > gReadFailAt) avail = gReadFailAt - pos_;
    std::memcpy(out, data_->data() + pos_, avail);
    pos_ += avail;
    return avail;
  }
  bool seek(size_t pos) {
    assert(data_);
    if (pos > data_->size()) return false;
    pos_ = pos;
    return true;
  }
  void close() { data_.reset(); }

 private:
  std::shared_ptr<std::string> data_;
  bool writable_ = false;
  size_t pos_ = 0;
};

namespace VFS {
AuthContext systemAuth(const char* reason) { return AuthContext{reason}; }
bool existsGuarded(const char* path, const AuthContext&) {
  ++gFsCalls;
  return gFiles.count(path) || gDirs.count(path);
}
bool mkdirGuarded(const char* path, const AuthContext&) {
  ++gFsCalls;
  gDirs[path] = true;
  return true;
}
File openGuarded(const char* path, const char* mode, const AuthContext&, bool create = false) {
  ++gFsCalls;
  if (gOpenFail) return File();
  if (std::strcmp(mode, "w") == 0) {
    assert(create);
    auto data = std::make_shared<std::string>();
    gFiles[path] = data;
    return File(data, true);
  }
  auto it = gFiles.find(path);
  return it == gFiles.end() ? File() : File(it->second, false);
}
File openGuarded(const String& path, const char* mode, const AuthContext& a, bool create = false) {
  return openGuarded(path.c_str(), mode, a, create);
}
bool removeGuarded(const char* path, const AuthContext&) {
  ++gFsCalls;
  return gFiles.erase(path) > 0;
}
}  // namespace VFS

// ---- Peer cache paths (System_BondedPeer owns the real ones) --------------
static void peerCacheDir(const uint8_t* mac, char* out, size_t n) {
  snprintf(out, n, "/system/peers/%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
static void peerCachePath(const uint8_t* mac, const char* name, char* out, size_t n) {
  char dir[48];
  peerCacheDir(mac, dir, sizeof(dir));
  snprintf(out, n, "%s/%s", dir, name);
}

// INSERT_PRODUCTION

// ---- Fixtures --------------------------------------------------------------
// A manifest-shaped document: modules of commands with name and help text,
// sized by the number of commands.
static void buildManifestLike(JsonDocument& doc, int commands) {
  JsonArray mods = doc["cli"].to<JsonArray>();
  JsonObject mod;
  JsonArray cmds;
  for (int i = 0; i < commands; ++i) {
    if (i % 40 == 0) {
      mod = mods.add<JsonObject>();
      mod["name"] = std::string("module") + std::to_string(i / 40);
      cmds = mod["commands"].to<JsonArray>();
    }
    JsonObject c = cmds.add<JsonObject>();
    c["name"] = std::string("command") + std::to_string(i);
    c["help"] = std::string("Does thing number ") + std::to_string(i) +
                " with a description long enough to look like real help text";
  }
}

static std::string hexOf(const uint8_t h[16]) {
  char s[33];
  for (int i = 0; i < 16; ++i) snprintf(s + i * 2, 3, "%02x", h[i]);
  return s;
}

static void resetFs() {
  gFiles.clear();
  gDirs.clear();
  gLog.clear();
  gFsCalls = 0;
  gWriteCap = SIZE_MAX;
  gReadChunk = 4096;
  gReadFailAt = SIZE_MAX;
  gOpenFail = false;
  gAllocFail = false;
  filesystemReady = true;
}

// A serialized manifest larger than an Arduino String can hold (65,535 bytes)
// comes back whole, NUL-terminated, in a buffer of exactly measureJson() + 1.
static void testSerializeLargeManifest(std::string* outJson) {
  resetFs();
  JsonDocument doc;
  buildManifestLike(doc, 1000);
  const size_t expect = measureJson(doc);
  assert(expect > 65535 && expect < kFileSlotMaxFileSize);
  size_t len = 0;
  char* buf = serializeBondJson(doc, &len, "test.manifest");
  assert(buf && len == expect && gLastAllocSize == expect + 1 && buf[len] == '\0');
  JsonDocument back;
  assert(deserializeJson(back, buf, len) == DeserializationError::Ok);
  assert(back["cli"].as<JsonArray>().size() == 25);
  assert(std::string(back["cli"][24]["commands"][39]["name"].as<const char*>()) == "command999");
  outJson->assign(buf, len);
  ps_free(buf);
  assert(gLiveAllocs == 0);
}

// Over the receiver's RAM limit the document is refused with an error and
// nothing is allocated; an allocation failure is an error too.
static void testSerializeRefusals() {
  resetFs();
  JsonDocument big;
  buildManifestLike(big, 2000);
  assert(measureJson(big) > kFileSlotMaxFileSize);
  size_t len = 7;
  gLastAllocSize = 0;
  assert(serializeBondJson(big, &len, "test.big") == nullptr && len == 0 && gLastAllocSize == 0);
  assert(logHas("limit 131072") && logHas("not sent"));

  JsonDocument small;
  small["a"] = 1;
  gAllocFail = true;
  len = 7;
  assert(serializeBondJson(small, &len, "test.oom") == nullptr && len == 0);
  assert(logHas("no memory"));
  gAllocFail = false;
  assert(gLiveAllocs == 0);
}

// One write of the whole buffer under the lock; a short write (full disk)
// removes the partial file; an open failure is reported.
static void testCheckedWrite(const std::string& json) {
  resetFs();
  {
    FsLockGuard g("test");
    assert(writeBondJsonFileLocked("/system/a.json", json.data(), json.size(), "test.write"));
  }
  assert(*gFiles["/system/a.json"] == json);

  gWriteCap = 4096;
  {
    FsLockGuard g("test");
    assert(!writeBondJsonFileLocked("/system/b.json", json.data(), json.size(), "test.write"));
  }
  assert(!gFiles.count("/system/b.json") && logHas("incomplete write"));
  gWriteCap = SIZE_MAX;

  gOpenFail = true;
  {
    FsLockGuard g("test");
    assert(!writeBondJsonFileLocked("/system/c.json", json.data(), json.size(), "test.write"));
  }
  assert(!gFiles.count("/system/c.json") && logHas("cannot open"));
}

// The receiver checks the transfer buffer in place and caches all of it.
static void testReceiveManifest(const std::string& json) {
  resetFs();
  for (int i = 0; i < 16; ++i) gEspNow->lastRemoteCap.fwHash[i] = static_cast<uint8_t>(0xa0 + i);
  const std::string path = "/system/manifests/" + hexOf(gEspNow->lastRemoteCap.fwHash) + ".json";
  const uint8_t mac[6] = {2, 0, 0, 0, 0, 1};

  // Truncated (what a String-capped sender delivered): rejected, not cached.
  gEspNowState.bondManifestReceived = false;
  gEspNowState.bondSyncInFlight = BOND_SYNC_MANIFEST;
  processBondModeManifestResp(mac, String("peer"), json.data(), 65503);
  assert(!gEspNowState.bondManifestReceived && gEspNowState.bondSyncInFlight == BOND_SYNC_NONE);
  assert(!gFiles.count(path) && logHas("REJECTED corrupt manifest (len=65503"));

  // Whole: accepted and cached byte for byte.
  gEspNowState.bondSyncInFlight = BOND_SYNC_MANIFEST;
  gEspNowState.bondSyncRetryCount = 2;
  processBondModeManifestResp(mac, String("peer"), json.data(), json.size());
  assert(gEspNowState.bondManifestReceived && gEspNowState.bondSyncInFlight == BOND_SYNC_NONE &&
         gEspNowState.bondSyncRetryCount == 0);
  assert(gFiles.count(path) && *gFiles[path] == json);
  assert(manifestCachedFor(gEspNow->lastRemoteCap.fwHash));
  assert(gFsLockDepth == 0);

  // A cached file cut short is not "cached".
  gFiles[path]->resize(65503);
  assert(!manifestCachedFor(gEspNow->lastRemoteCap.fwHash));
  gFiles.erase(path);
  assert(!manifestCachedFor(gEspNow->lastRemoteCap.fwHash));

  // The schema receiver takes the same buffer path.
  gEspNowState.bondSchemaReceived = false;
  processBondSchema(mac, String("peer"), json.data(), json.size());
  assert(gEspNowState.bondSchemaReceived);
  assert(*gFiles["/system/peers/020000000001/schema.json"] == json);
  gEspNowState.bondSchemaReceived = false;
  processBondSchema(mac, String("peer"), json.data(), json.size() - 1);
  assert(!gEspNowState.bondSchemaReceived && logHas("REJECTED corrupt schema"));
}

// The reader copies the cached file into one buffer under the lock and
// returns with the lock released; bad hashes never reach the filesystem.
static void testReadCached(const std::string& json) {
  resetFs();
  const std::string hash = "00112233445566778899aabbccddeeff";
  const std::string path = "/system/manifests/" + hash + ".json";
  gFiles[path] = std::make_shared<std::string>(json);
  const AuthContext auth{"user"};
  char* out = nullptr;
  const char* err = nullptr;

  size_t len = espnowReadCachedManifest(hash.c_str(), auth, &out, &err);
  assert(len == json.size() && out && std::string(out, len) == json && out[len] == '\0');
  assert(gFsLockDepth == 0);
  ps_free(out);

  for (const char* bad : {"../../users.json", "00112233445566778899AABBCCDDEEFF",
                          "00112233445566778899aabbccddeef", "00112233445566778899aabbccddeeff0", ""}) {
    gFsCalls = 0;
    out = reinterpret_cast<char*>(1);
    assert(espnowReadCachedManifest(bad, auth, &out, &err) == 0);
    assert(out == nullptr && std::strcmp(err, "invalid fwHash") == 0 && gFsCalls == 0);
  }

  assert(espnowReadCachedManifest("ffffffffffffffffffffffffffffffff", auth, &out, &err) == 0);
  assert(std::strcmp(err, "not found") == 0);

  gFiles[path]->assign(kFileSlotMaxFileSize + 1, ' ');
  assert(espnowReadCachedManifest(hash.c_str(), auth, &out, &err) == 0 && std::strcmp(err, "unexpected size") == 0);
  gFiles[path]->assign(json);

  gAllocFail = true;
  assert(espnowReadCachedManifest(hash.c_str(), auth, &out, &err) == 0 && std::strcmp(err, "out of memory") == 0);
  gAllocFail = false;

  gReadFailAt = 50000;
  assert(espnowReadCachedManifest(hash.c_str(), auth, &out, &err) == 0 && std::strcmp(err, "read failed") == 0);
  assert(out == nullptr && gLiveAllocs == 0 && gFsLockDepth == 0);
  gReadFailAt = SIZE_MAX;

  filesystemReady = false;
  assert(espnowReadCachedManifest(hash.c_str(), auth, &out, &err) == 0 &&
         std::strcmp(err, "filesystem not ready") == 0);
  filesystemReady = true;
}

// `bondshowmanifest` / `bondshowremotemanifest <hash>` print several hundred
// lines; the printer waits for the output queue instead of overrunning it.
static void testPacedDump(const std::string& json) {
  gQueueModel = true;
  gQueued = gQueuePeak = gQueueDrops = 0;
  gPrinted.clear();
  broadcastBondJsonChunks(json.data(), json.size());
  assert(gQueueDrops == 0 && gPrinted == json && gQueuePeak <= 17);
  gQueueModel = false;
}

int main() {
  std::string json;
  testSerializeLargeManifest(&json);
  testSerializeRefusals();
  testCheckedWrite(json);
  testReceiveManifest(json);
  testReadCached(json);
  testPacedDump(json);
  assert(gLiveAllocs == 0);
  std::printf("bond JSON buffers: %zu-byte manifest serialized, cached, checked and read back\n", json.size());
  return 0;
}
