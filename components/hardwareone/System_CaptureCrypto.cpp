// =============================================================================
// Capture at-rest sealing — implementation. See System_CaptureCrypto.h for the
// format and docs/HEALTH_AT_REST_ENCRYPTION_PLAN.md for the design/consumers.
// =============================================================================

#include "System_CaptureCrypto.h"

#include "System_Debug.h"
#include "System_MemUtil.h"   // ps_alloc — scratch lives in PSRAM, key does NOT

#include <sodium.h>
#include <mbedtls/base64.h>
#include <nvs.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "System_Events.h"   // systemEventPost — SYSEVT_SECRET_DECRYPT_FAILED

namespace {

constexpr const char* kNvsNamespace = "hw1cap";
constexpr const char* kNvsKeyName   = "k1";
constexpr char  kAad[]        = "HW1ENC1";     // constant AAD (7 bytes, no NUL)
constexpr size_t kAadLen      = sizeof(kAad) - 1;
constexpr size_t kNonceLen    = 12;
constexpr size_t kTagLen      = 16;
constexpr size_t kPrefixLen   = sizeof(CAPCRYPT_ROW_PREFIX) - 1;  // "ENC1:"
constexpr size_t kMagicPfxLen = sizeof(CAPCRYPT_MAGIC_PREFIX) - 1;
// nonce || ct || tag for a max-size row
constexpr size_t kBinCap      = kNonceLen + CAPCRYPT_MAX_ROW + kTagLen + 4;

// The key lives in internal DRAM (static BSS) — never PSRAM, which is
// electrically probeable on this hardware.
uint8_t sKey[32];
bool    sKeyLoaded = false;

// PSRAM scratch: [0..kBinCap) binary stage, then a plaintext stage. Plaintext
// ROWS already transit PSRAM today (the row builders in System_SensorLogging
// are ps_alloc'd), so scratch placement adds no new exposure.
char*  sScratch = nullptr;
char*  binScratch()   { return sScratch; }
char*  plainScratch() { return sScratch + kBinCap; }
constexpr size_t kPlainCap   = CAPCRYPT_MAX_ROW + 1;
constexpr size_t kScratchCap = kBinCap + kPlainCap;

SemaphoreHandle_t mx() {
  // Function-local static: thread-safe init without a boot-order hook.
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}

struct ScratchGuard {
  ScratchGuard()  { xSemaphoreTake(mx(), portMAX_DELAY); }
  ~ScratchGuard() { xSemaphoreGive(mx()); }
};

bool ensureScratchLocked() {
  if (!sScratch) {
    sScratch = (char*)ps_alloc(kScratchCap, AllocPref::PreferPSRAM, "capcrypt.scratch");
  }
  return sScratch != nullptr;
}

// Open one sealed line into plainScratch(). Caller holds the mutex and has
// verified the key is resident. Returns plaintext length or -1.
int openLineLocked(const char* line, size_t lineLen) {
  if (lineLen <= kPrefixLen) return -1;
  if (strncmp(line, CAPCRYPT_ROW_PREFIX, kPrefixLen) != 0) return -1;
  if (!ensureScratchLocked()) return -1;
  size_t binLen = 0;
  if (mbedtls_base64_decode((unsigned char*)binScratch(), kBinCap, &binLen,
                            (const unsigned char*)(line + kPrefixLen),
                            lineLen - kPrefixLen) != 0) return -1;
  if (binLen < kNonceLen + kTagLen) return -1;
  const size_t ctLen = binLen - kNonceLen - kTagLen;
  if (ctLen + 1 > kPlainCap) return -1;
  const unsigned char* nonce = (const unsigned char*)binScratch();
  const unsigned char* ct    = nonce + kNonceLen;
  const unsigned char* tag   = ct + ctLen;
  if (crypto_aead_chacha20poly1305_ietf_decrypt_detached(
          (unsigned char*)plainScratch(), nullptr,
          ct, ctLen, tag,
          (const unsigned char*)kAad, kAadLen,
          nonce, sKey) != 0) {
    return -1;  // tag mismatch: torn tail line, or another device's key
  }
  plainScratch()[ctLen] = '\0';
  return (int)ctLen;
}

const char kUndecryptable[] = CAPCRYPT_UNDECRYPTABLE;

}  // namespace

bool captureCryptoKeyReady() {
  if (sKeyLoaded) return true;
  // Honest status without minting: does the blob exist?
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;
  size_t len = 0;
  bool present = (nvs_get_blob(h, kNvsKeyName, nullptr, &len) == ESP_OK &&
                  len == sizeof(sKey));
  nvs_close(h);
  return present;
}

bool captureCryptoEnsureKey() {
  if (sKeyLoaded) return true;
  if (sodium_init() < 0) {
    ERROR_SYSTEMF("[CapCrypt] sodium_init failed");
    return false;
  }
  ScratchGuard lock;
  if (sKeyLoaded) return true;  // raced another caller past the fast path

  nvs_handle_t h;
  size_t len = sizeof(sKey);
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) == ESP_OK) {
    esp_err_t rc = nvs_get_blob(h, kNvsKeyName, sKey, &len);
    if (rc == ESP_OK && len == sizeof(sKey)) {
      nvs_close(h);
      sKeyLoaded = true;
      return true;
    }
    // A blob EXISTS but could not be read as a whole key — wrong length returns
    // ESP_ERR_NVS_INVALID_LENGTH, and a short blob succeeds with len != sizeof.
    // Falling through to the mint below would overwrite the REAL key and strand
    // every capture and health file already sealed with it, unrecoverably. There
    // is no way back from that: the plaintext does not exist anywhere else.
    // Refuse instead, and leave the stored blob untouched so it can be recovered.
    // ESP_ERR_NVS_NOT_FOUND is the genuine first-use case and must still mint.
    if (rc != ESP_ERR_NVS_NOT_FOUND) {
      nvs_close(h);
      ERROR_SYSTEMF("[CapCrypt] stored capture key unreadable (0x%x, len=%u) - refusing to mint "
                    "a replacement; sealed captures would be permanently unreadable",
                    (unsigned)rc, (unsigned)len);
      systemEventPost(SYSEVT_SECRET_DECRYPT_FAILED, "capture_key", "stored blob unreadable");
      sodium_memzero(sKey, sizeof(sKey));
      return false;
    }
    nvs_close(h);
  }

  // First use on this device (or post-NVS-erase): mint + persist. Failing to
  // PERSIST is fatal — a key that exists only in RAM would strand every file
  // sealed with it at the next reboot.
  randombytes_buf(sKey, sizeof(sKey));
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ERROR_SYSTEMF("[CapCrypt] nvs_open(rw) failed: 0x%x", err);
    sodium_memzero(sKey, sizeof(sKey));
    return false;
  }
  bool ok = (nvs_set_blob(h, kNvsKeyName, sKey, sizeof(sKey)) == ESP_OK) &&
            (nvs_commit(h) == ESP_OK);
  nvs_close(h);
  if (!ok) {
    ERROR_SYSTEMF("[CapCrypt] key persist failed");
    sodium_memzero(sKey, sizeof(sKey));
    return false;
  }
  DEBUG_SYSTEMF("[CapCrypt] capture key minted and stored");
  sKeyLoaded = true;
  return true;
}

bool captureCryptoPseudonym(const char* domain, const char* value,
                            char* outHex, size_t outCap) {
  if (!domain || !domain[0] || !value || !value[0] || !outHex || outCap < 33)
    return false;
  const size_t domainLen = strlen(domain);
  const size_t valueLen = strlen(value);
  if (domainLen > 64 || valueLen > 128 || !captureCryptoEnsureKey()) return false;

  unsigned char digest[16];
  crypto_generichash_state state;
  if (crypto_generichash_init(&state, sKey, sizeof(sKey), sizeof(digest)) != 0)
    return false;
  // Length-delimit the domain so ("ab", "c") cannot collide structurally
  // with ("a", "bc") before the keyed hash is applied.
  const uint8_t domainSize = static_cast<uint8_t>(domainLen);
  if (crypto_generichash_update(&state, &domainSize, sizeof(domainSize)) != 0 ||
      crypto_generichash_update(
          &state, reinterpret_cast<const unsigned char*>(domain), domainLen) != 0 ||
      crypto_generichash_update(
          &state, reinterpret_cast<const unsigned char*>(value), valueLen) != 0 ||
      crypto_generichash_final(&state, digest, sizeof(digest)) != 0) {
    sodium_memzero(digest, sizeof(digest));
    return false;
  }
  sodium_bin2hex(outHex, outCap, digest, sizeof(digest));
  sodium_memzero(digest, sizeof(digest));
  sodium_memzero(&state, sizeof(state));
  return true;
}

int captureCryptoSealLine(const char* in, size_t inLen, char* out, size_t outCap) {
  if (!in || !out) return -1;
  if (inLen > CAPCRYPT_MAX_ROW) return -1;
  if (!captureCryptoEnsureKey()) return -1;
  ScratchGuard lock;
  if (!ensureScratchLocked()) return -1;

  unsigned char* nonce = (unsigned char*)binScratch();
  unsigned char* ct    = nonce + kNonceLen;
  randombytes_buf(nonce, kNonceLen);
  unsigned long long tagLen = 0;
  unsigned char tag[kTagLen];
  if (crypto_aead_chacha20poly1305_ietf_encrypt_detached(
          ct, tag, &tagLen,
          (const unsigned char*)in, inLen,
          (const unsigned char*)kAad, kAadLen,
          nullptr, nonce, sKey) != 0) return -1;
  memcpy(ct + inLen, tag, kTagLen);

  if (outCap <= kPrefixLen) return -1;
  memcpy(out, CAPCRYPT_ROW_PREFIX, kPrefixLen);
  size_t b64Len = 0;
  if (mbedtls_base64_encode((unsigned char*)out + kPrefixLen, outCap - kPrefixLen - 1,
                            &b64Len, (const unsigned char*)binScratch(),
                            kNonceLen + inLen + kTagLen) != 0) return -1;
  out[kPrefixLen + b64Len] = '\0';
  return (int)(kPrefixLen + b64Len);
}

int captureCryptoOpenLine(const char* line, size_t lineLen, char* out, size_t outCap) {
  if (!line || !out) return -1;
  if (!captureCryptoEnsureKey()) return -1;
  ScratchGuard lock;
  int n = openLineLocked(line, lineLen);
  if (n < 0) return -1;
  if ((size_t)n + 1 > outCap) return -1;
  memcpy(out, plainScratch(), (size_t)n + 1);
  return n;
}

bool captureCryptoIsMagicLine(const char* line) {
  return line && strncmp(line, CAPCRYPT_MAGIC_PREFIX, kMagicPfxLen) == 0;
}

bool captureCryptoIsSealedRow(const char* line) {
  return line && strncmp(line, CAPCRYPT_ROW_PREFIX, kPrefixLen) == 0;
}

bool captureCryptoLooksSealed(File& f) {
  if (!f) return false;
  char head[kMagicPfxLen];
  f.seek(0);
  size_t got = f.read((uint8_t*)head, sizeof(head));
  f.seek(0);
  return got == sizeof(head) &&
         strncmp(head, CAPCRYPT_MAGIC_PREFIX, kMagicPfxLen) == 0;
}

bool captureCryptoRevealLine(String& line) {
  if (!captureCryptoIsSealedRow(line.c_str())) return false;
  if (!captureCryptoEnsureKey()) { line = kUndecryptable; return true; }
  ScratchGuard lock;
  int n = openLineLocked(line.c_str(), line.length());
  if (n >= 0) line = plainScratch();
  else        line = kUndecryptable;
  return true;
}

size_t captureCryptoRevealText(String& text) {
  // Only interpret blobs that carry the mark — stray "ENC1:" text inside an
  // ordinary file must never be treated as ciphertext.
  if (strncmp(text.c_str(), CAPCRYPT_MAGIC_PREFIX, kMagicPfxLen) != 0) return 0;
  const bool haveKey = captureCryptoEnsureKey();
  ScratchGuard lock;

  // In-place walk: plaintext (and the failure marker) is always shorter than
  // its sealed line, so the write cursor never overtakes the read cursor.
  // Arduino String exposes no mutable accessor; its buffer is contiguous and
  // NUL-terminated, so writing through c_str() and truncating with remove()
  // is safe here.
  char* buf = const_cast<char*>(text.c_str());
  const size_t len = text.length();
  size_t r = 0, w = 0, sealedRows = 0;
  while (r < len) {
    size_t lineStart = r;
    while (r < len && buf[r] != '\n') r++;
    size_t lineLen = r - lineStart;
    const bool hasNl = (r < len);
    if (hasNl) r++;

    if (lineLen > kPrefixLen &&
        strncmp(buf + lineStart, CAPCRYPT_ROW_PREFIX, kPrefixLen) == 0) {
      sealedRows++;
      int n = haveKey ? openLineLocked(buf + lineStart, lineLen) : -1;
      if (n >= 0) {
        memcpy(buf + w, plainScratch(), (size_t)n);
        w += (size_t)n;
      } else {
        memcpy(buf + w, kUndecryptable, sizeof(kUndecryptable) - 1);
        w += sizeof(kUndecryptable) - 1;
      }
    } else {
      memmove(buf + w, buf + lineStart, lineLen);
      w += lineLen;
    }
    if (hasNl) buf[w++] = '\n';
  }
  text.remove(w);
  return sealedRows;
}
