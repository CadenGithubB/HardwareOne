// The Python runner inserts the production capture-key loader unchanged.
// NVS and libsodium are scripted fakes: every open/read/write is recorded so a
// case can prove which failures mint a key and which fail closed.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using esp_err_t = int;
using nvs_handle_t = uint32_t;
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_ERR_NO_MEM = 0x101;
constexpr esp_err_t ESP_ERR_NVS_NOT_INITIALIZED = 0x1101;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 0x110c;
constexpr int SYSEVT_SECRET_DECRYPT_FAILED = 7;

static std::vector<std::string> trace;
static esp_err_t roOpen = ESP_OK, rwOpen = ESP_OK, getRc = ESP_OK, setRc = ESP_OK, commitRc = ESP_OK;
static size_t storedLen = 32;
static uint8_t stored[32];
static int setCalls = 0, randomCalls = 0, events = 0, openHandles = 0;

static esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* out) {
  assert(std::string(ns) == "hw1cap");
  const esp_err_t rc = mode == NVS_READONLY ? roOpen : rwOpen;
  trace.push_back(std::string(mode == NVS_READONLY ? "open:ro:" : "open:rw:") + std::to_string(rc));
  if (rc == ESP_OK) { *out = 42; ++openHandles; }
  return rc;
}
static void nvs_close(nvs_handle_t h) { assert(h == 42); --openHandles; trace.push_back("close"); }
static esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* out, size_t* len) {
  assert(h == 42 && std::string(key) == "k1");
  trace.push_back("get:" + std::to_string(getRc));
  if (getRc != ESP_OK) return getRc;
  if (!out) { *len = storedLen; return ESP_OK; }
  if (*len < storedLen) return ESP_ERR_NVS_INVALID_LENGTH;
  memcpy(out, stored, storedLen);
  *len = storedLen;
  return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t h, const char* key, const void* value, size_t len) {
  assert(h == 42 && std::string(key) == "k1");
  ++setCalls; trace.push_back("set");
  if (setRc == ESP_OK) { memcpy(stored, value, len); storedLen = len; }
  return setRc;
}
static esp_err_t nvs_commit(nvs_handle_t h) { assert(h == 42); trace.push_back("commit"); return commitRc; }
static int sodium_init() { return 0; }
static void randombytes_buf(void* out, size_t len) { ++randomCalls; memset(out, 0xA5, len); }
static void sodium_memzero(void* p, size_t len) { memset(p, 0, len); }
static void systemEventPost(int type, const char*, const char*) {
  assert(type == SYSEVT_SECRET_DECRYPT_FAILED); ++events;
}
#define ERROR_SYSTEMF(...) ((void)0)
#define DEBUG_SYSTEMF(...) ((void)0)

constexpr const char* kNvsNamespace = "hw1cap";
constexpr const char* kNvsKeyName   = "k1";
static uint8_t sKey[32];
static bool sKeyLoaded = false;
static int guards = 0;
struct ScratchGuard {
  ScratchGuard() { ++guards; }
  ~ScratchGuard() { --guards; }
};

// INSERT_PRODUCTION

static void reset() {
  trace.clear();
  roOpen = rwOpen = getRc = setRc = commitRc = ESP_OK;
  storedLen = 32;
  for (size_t i = 0; i < sizeof(stored); ++i) stored[i] = uint8_t(i + 1);
  setCalls = randomCalls = events = 0;
  memset(sKey, 0, sizeof(sKey)); sKeyLoaded = false;
}
static bool keyIsZero() {
  for (uint8_t b : sKey) if (b) return false;
  return true;
}
static void settled() { assert(openHandles == 0 && guards == 0); }

static void transientOpenFailsClosed() {
  // The read-only open cannot allocate its handle; the read-write open would
  // succeed. No key may be minted over the stored one.
  for (esp_err_t rc : {ESP_ERR_NO_MEM, ESP_ERR_NVS_NOT_INITIALIZED}) {
    reset(); roOpen = rc;
    assert(!captureCryptoEnsureKey());
    assert(setCalls == 0 && randomCalls == 0);
    assert(!sKeyLoaded && keyIsZero());
    assert(stored[0] == 1 && storedLen == 32);
    settled();
  }
  // The next call, with NVS healthy again, loads the real key.
  reset(); roOpen = ESP_ERR_NO_MEM;
  assert(!captureCryptoEnsureKey());
  roOpen = ESP_OK;
  assert(captureCryptoEnsureKey());
  assert(sKeyLoaded && memcmp(sKey, stored, 32) == 0 && setCalls == 0);
  settled();
}

static void firstUseMints() {
  // Missing namespace and missing blob are the two first-use cases.
  reset(); roOpen = ESP_ERR_NVS_NOT_FOUND;
  assert(captureCryptoEnsureKey());
  assert(setCalls == 1 && randomCalls == 1 && sKeyLoaded && sKey[0] == 0xA5 && stored[0] == 0xA5);
  settled();
  reset(); getRc = ESP_ERR_NVS_NOT_FOUND;
  assert(captureCryptoEnsureKey());
  assert(setCalls == 1 && sKeyLoaded);
  settled();
  // A mint that cannot be persisted is not used.
  reset(); roOpen = ESP_ERR_NVS_NOT_FOUND; commitRc = ESP_ERR_NO_MEM;
  assert(!captureCryptoEnsureKey());
  assert(!sKeyLoaded && keyIsZero());
  settled();
  reset(); roOpen = ESP_ERR_NVS_NOT_FOUND; rwOpen = ESP_ERR_NO_MEM;
  assert(!captureCryptoEnsureKey());
  assert(setCalls == 0 && !sKeyLoaded && keyIsZero());
  settled();
}

static void storedKeyPaths() {
  reset();
  assert(captureCryptoEnsureKey());
  assert(memcmp(sKey, stored, 32) == 0 && setCalls == 0 && randomCalls == 0);
  assert(trace == std::vector<std::string>({"open:ro:0", "get:0", "close"}));
  settled();
  // Loaded once: no further NVS traffic.
  trace.clear();
  assert(captureCryptoEnsureKey() && trace.empty());
  // An unreadable stored blob is never replaced.
  for (esp_err_t rc : {ESP_ERR_NVS_INVALID_LENGTH, ESP_ERR_NO_MEM}) {
    reset(); getRc = rc;
    assert(!captureCryptoEnsureKey());
    assert(setCalls == 0 && events == 1 && !sKeyLoaded && keyIsZero());
    settled();
  }
  reset(); storedLen = 16;
  assert(!captureCryptoEnsureKey());
  assert(setCalls == 0 && events == 1 && !sKeyLoaded);
  settled();
}

int main() {
  storedKeyPaths();
  firstUseMints();
  transientOpenFailsClosed();
  std::printf("PASS capture key loader: transient NVS errors fail closed, first use mints\n");
  return 0;
}
