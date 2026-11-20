// The Python runner inserts the production capture-key loader unchanged.
// NVS and libsodium are scripted fakes: every open/read/write is recorded so a
// case can prove which failures mint a key and which fail closed.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <climits>
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
// Key mint: 0xA5 bytes (the key cases check it). Catch-up nonces: a counter,
// so two seals of the same chunk differ as random nonces do.
static bool randomCounter = false;
static uint8_t randomNext = 1;
static void randombytes_buf(void* out, size_t len) {
  ++randomCalls;
  if (!randomCounter) { memset(out, 0xA5, len); return; }
  for (size_t i = 0; i < len; ++i) static_cast<uint8_t*>(out)[i] = randomNext++;
}
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
static int guards = 0, guardTakes = 0;
struct ScratchGuard {
  ScratchGuard() { ++guards; ++guardTakes; }
  ~ScratchGuard() { --guards; }
};

// ---- libsodium doubles for the catch-up chunk API ---------------------------
// A toy AEAD with the real detached call shapes: a keystream from key, nonce
// and position, and a 16-byte tag over key, nonce, AAD and ciphertext. It is
// not ChaCha20-Poly1305; it checks the plumbing (layout, AAD, key, fail closed).
constexpr size_t kNonceLen = 12;
constexpr size_t kTagLen = 16;
static uint8_t ks(const unsigned char* k, const unsigned char* n, size_t i) {
  return uint8_t(k[i % 32] ^ n[i % 12] ^ uint8_t(i * 31 + 7));
}
static void toyTag(const unsigned char* k, const unsigned char* n, const unsigned char* ad, unsigned long long adLen,
                   const unsigned char* c, unsigned long long cLen, unsigned char* tag) {
  for (size_t t = 0; t < kTagLen; ++t) {
    uint32_t h = 2166136261u ^ uint32_t(t * 977);
    auto mix = [&](const unsigned char* p, unsigned long long len) {
      for (unsigned long long i = 0; i < len; ++i) { h ^= p[i]; h *= 16777619u; }
      h ^= uint32_t(len); h *= 16777619u;
    };
    mix(k, 32); mix(n, kNonceLen); mix(ad, adLen); mix(c, cLen);
    tag[t] = uint8_t(h >> 8);
  }
}
static int crypto_aead_chacha20poly1305_ietf_encrypt_detached(
    unsigned char* c, unsigned char* mac, unsigned long long* maclen, const unsigned char* m, unsigned long long mlen,
    const unsigned char* ad, unsigned long long adlen, const unsigned char*, const unsigned char* npub,
    const unsigned char* k) {
  for (unsigned long long i = 0; i < mlen; ++i) c[i] = uint8_t(m[i] ^ ks(k, npub, size_t(i)));
  toyTag(k, npub, ad, adlen, c, mlen, mac);
  if (maclen) *maclen = kTagLen;
  return 0;
}
static int crypto_aead_chacha20poly1305_ietf_decrypt_detached(
    unsigned char* m, unsigned char*, const unsigned char* c, unsigned long long clen, const unsigned char* mac,
    const unsigned char* ad, unsigned long long adlen, const unsigned char* npub, const unsigned char* k) {
  unsigned char want[kTagLen];
  toyTag(k, npub, ad, adlen, c, clen, want);
  if (memcmp(want, mac, kTagLen) != 0) return -1;
  for (unsigned long long i = 0; i < clen; ++i) m[i] = uint8_t(c[i] ^ ks(k, npub, size_t(i)));
  return 0;
}
// Keyed hash double for captureCryptoPseudonym (the key id).
struct crypto_generichash_state { uint32_t h[4]; };
static int crypto_generichash_init(crypto_generichash_state* s, const unsigned char* key, size_t keyLen, size_t) {
  for (int i = 0; i < 4; ++i) s->h[i] = 2166136261u + uint32_t(i);
  for (size_t i = 0; i < keyLen; ++i) for (int j = 0; j < 4; ++j) { s->h[j] ^= key[i]; s->h[j] *= 16777619u + uint32_t(j); }
  return 0;
}
static int crypto_generichash_update(crypto_generichash_state* s, const unsigned char* p, unsigned long long n) {
  for (unsigned long long i = 0; i < n; ++i) for (int j = 0; j < 4; ++j) { s->h[j] ^= p[i]; s->h[j] *= 16777619u + uint32_t(j); }
  return 0;
}
static int crypto_generichash_final(crypto_generichash_state* s, unsigned char* out, size_t outLen) {
  for (size_t i = 0; i < outLen; ++i) out[i] = uint8_t(s->h[i % 4] >> (8 * (i / 4 % 4)));
  return 0;
}
static char* sodium_bin2hex(char* hex, size_t hexCap, const unsigned char* bin, size_t binLen) {
  static const char kHex[] = "0123456789abcdef";
  size_t i = 0;
  for (; i < binLen && 2 * i + 2 < hexCap; ++i) { hex[2 * i] = kHex[bin[i] >> 4]; hex[2 * i + 1] = kHex[bin[i] & 15]; }
  hex[2 * i] = 0;
  return hex;
}

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

#ifdef HAVE_CATCHUP_CHUNKS
// File catch-up sealing on send: one binary chunk per frame.
static void chunkSealing() {
  reset();
  randomCounter = true;
  uint8_t aad[22];
  for (size_t i = 0; i < sizeof(aad); ++i) aad[i] = uint8_t(0x40 + i);
  const char* text = "2026-11-19 cmd: wifiadd home secret";
  const size_t n = strlen(text);
  uint8_t sealed[96], again[96], plain[64];
  assert(captureCryptoEnsureKey());
  guardTakes = 0;
  const int len = captureCryptoSealChunk(aad, sizeof(aad), (const uint8_t*)text, n, sealed, sizeof(sealed));
  assert(len == int(n + CAPCRYPT_CHUNK_OVERHEAD) && guardTakes == 1 && guards == 0);
  assert(memcmp(sealed + 12, text, n) != 0);                       // no plaintext on the wire
  assert(captureCryptoOpenChunk(aad, sizeof(aad), sealed, size_t(len), plain, sizeof(plain)) == int(n));
  assert(memcmp(plain, text, n) == 0);
  // A random nonce per seal: the same chunk seals differently.
  assert(captureCryptoSealChunk(aad, sizeof(aad), (const uint8_t*)text, n, again, sizeof(again)) == len);
  assert(memcmp(again, sealed, 12) != 0 && memcmp(again, sealed, size_t(len)) != 0);
  // The AAD binds the chunk: another offset, file or ledger fails, and the
  // output is wiped.
  uint8_t moved[22];
  memcpy(moved, aad, sizeof(moved));
  moved[14] ^= 1;
  memset(plain, 0x77, sizeof(plain));
  assert(captureCryptoOpenChunk(moved, sizeof(moved), sealed, size_t(len), plain, sizeof(plain)) == -1);
  for (size_t i = 0; i < n; ++i) assert(plain[i] == 0);
  sealed[20] ^= 0x10;                                                // a damaged ciphertext byte
  assert(captureCryptoOpenChunk(aad, sizeof(aad), sealed, size_t(len), plain, sizeof(plain)) == -1);
  sealed[20] ^= 0x10;
  // Bounds: small output, short input, overlap, null pointers.
  assert(captureCryptoSealChunk(aad, sizeof(aad), (const uint8_t*)text, n, sealed, n + 27) == -1);
  assert(captureCryptoOpenChunk(aad, sizeof(aad), sealed, 27, plain, sizeof(plain)) == -1);
  assert(captureCryptoOpenChunk(aad, sizeof(aad), sealed, size_t(len), plain, n - 1) == -1);
  uint8_t shared[96] = {};
  assert(captureCryptoSealChunk(aad, sizeof(aad), shared + 20, 20, shared, sizeof(shared)) == -1);
  assert(captureCryptoSealChunk(nullptr, 4, (const uint8_t*)text, n, sealed, sizeof(sealed)) == -1);
  assert(captureCryptoSealChunk(aad, sizeof(aad), nullptr, 4, sealed, sizeof(sealed)) == -1);
  // An empty chunk is a nonce and a tag.
  assert(captureCryptoSealChunk(aad, sizeof(aad), nullptr, 0, sealed, sizeof(sealed)) == int(CAPCRYPT_CHUNK_OVERHEAD));
  assert(captureCryptoOpenChunk(aad, sizeof(aad), sealed, CAPCRYPT_CHUNK_OVERHEAD, plain, sizeof(plain)) == 0);
  // Another device's key does not open it.
  const int l2 = captureCryptoSealChunk(aad, sizeof(aad), (const uint8_t*)text, n, sealed, sizeof(sealed));
  sKey[3] ^= 0xFF;
  assert(captureCryptoOpenChunk(aad, sizeof(aad), sealed, size_t(l2), plain, sizeof(plain)) == -1);
  sKey[3] ^= 0xFF;
  // No key (NVS failure): fail closed, nothing written.
  reset();
  roOpen = ESP_ERR_NO_MEM;
  memset(sealed, 0x5A, sizeof(sealed));
  assert(captureCryptoSealChunk(aad, sizeof(aad), (const uint8_t*)text, n, sealed, sizeof(sealed)) == -1);
  for (uint8_t b : sealed) assert(b == 0x5A);
  randomCounter = false;
  settled();
}

static void keyIdIsStableAndNotTheKey() {
  reset();
  uint8_t a[8], b[8];
  assert(captureCryptoKeyId(a) && captureCryptoKeyId(b) && memcmp(a, b, 8) == 0);
  assert(memcmp(a, sKey, 8) != 0 && memcmp(a, stored, 8) != 0);
  // Another key, another id.
  reset();
  stored[0] ^= 0xFF;
  uint8_t c[8];
  assert(captureCryptoKeyId(c) && memcmp(a, c, 8) != 0);
  reset();
  roOpen = ESP_ERR_NO_MEM;
  assert(!captureCryptoKeyId(c));
  assert(!captureCryptoKeyId(nullptr));
  settled();
}
#endif

int main() {
  storedKeyPaths();
  firstUseMints();
  transientOpenFailsClosed();
  std::printf("PASS capture key loader: transient NVS errors fail closed, first use mints\n");
#ifdef HAVE_CATCHUP_CHUNKS
  chunkSealing();
  keyIdIsStableAndNotTheKey();
  std::printf("PASS catch-up chunks: round trip, AAD and key bound, random nonce, fail closed; key id stable\n");
#endif
  return 0;
}
