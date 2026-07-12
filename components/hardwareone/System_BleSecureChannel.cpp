#include "System_BuildConfig.h"

#if ENABLE_BLUETOOTH

#include "System_BleSecureChannel.h"
#include "System_Settings.h"
#include "BLE_Types.h"          // BLE_MAX_CONNECTIONS

#include <sodium.h>
#include <mbedtls/pkcs5.h>   // PBKDF2 on ESP32 HW-accelerated SHA-256 (CONFIG_MBEDTLS_HARDWARE_SHA)
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>     // vTaskDelay — inter-fragment pacing
#include <freertos/semphr.h>

// Raw notify into the RESPONSE characteristic (binary-safe; defined in Bluetooth.cpp).
extern bool bleRawNotify(const char* data, size_t len);
extern bool isBLEConnected();
extern Settings gSettings;
extern void broadcastOutput(const String& s);  // queue-based, BTC_TASK-safe

// Handshake diagnostics — one line per transition so interop can be watched on
// serial/web. Infrequent (per connection), so not gated behind a debug flag.
static void scLog(uint16_t connId, const char* msg) {
  char b[96];
  snprintf(b, sizeof(b), "[BLE-SC] conn %u: %s", (unsigned)connId, msg);
  broadcastOutput(b);
}

// ============================================================================
// Frame types (wire) — see System_BleSecureChannel.h for the full layout.
// ============================================================================
static const uint8_t SC_HELLO       = 0x01;
static const uint8_t SC_HELLO_ACK   = 0x02;
static const uint8_t SC_CONFIRM     = 0x03;
static const uint8_t SC_CONFIRM_ACK = 0x04;
static const uint8_t SC_DATA        = 0x10;
static const uint8_t SC_REJECT      = 0x05;   // device->app: handshake refused, body = reason(1)

// SC_REJECT reason codes — the app maps each to a human-readable message instead
// of showing a bare "handshake timed out". Sent best-effort right before the
// handshake aborts so the failure is never silent.
static const uint8_t SC_REJ_NO_PASSPHRASE = 0x01;  // device has no blesecret configured
static const uint8_t SC_REJ_AUTH_FAILED   = 0x02;  // wrong passphrase (or tampering/MITM)

static const char    SC_INFO[]      = "HW1-SC-v1";   // HKDF info
static const char    SC_PSK_SALT[]  = "HW1-SC-v1";   // PBKDF2 salt — MUST match app (verified vs test vectors)
static const uint32_t SC_PBKDF2_ITERS = 100000;

// Conservative plaintext budget per DATA frame so the framed ciphertext fits a
// negotiated MTU. Frame = 1(type)+8(ctr)+N(ct)+16(tag). 200 -> 225-byte frame,
// safe for any client that negotiated MTU >= ~230 (Android with requestMtu(517)
// negotiates far higher). The app MUST request a large MTU; a tiny default MTU
// can't carry a frame and the channel won't establish.
static const size_t SC_MAX_PT_PER_FRAME = 200;

// ============================================================================
// Per-connection state (small fixed table keyed by connId).
// ============================================================================
struct ScConn {
  bool     inUse;
  uint16_t connId;
  uint8_t  phase;        // 0 idle, 1 keys-derived (awaiting CONFIRM), 2 established
  uint8_t  ephSec[32];   // our ephemeral X25519 secret (handshake only)
  uint8_t  kC2D[32];     // app -> device
  uint8_t  kD2C[32];     // device -> app
  uint64_t rxCtr;        // last accepted c2d counter (strictly increasing)
  uint64_t txCtr;        // next d2c counter to emit
  uint16_t txMsgId;      // d2c application message id (increments per bleScSendEncrypted call)
};
static ScConn gSc[BLE_MAX_CONNECTIONS];

static uint8_t  gPsk[32];
static bool     gPskValid = false;

// Serializes device->app encryption: path A (debug_out task) and path B (cmd_exec task)
// both emit DATA frames using the per-connection txCtr. Without this they could race the
// counter (nonce reuse / out-of-order frames the app rejects). Created in bleScInit().
static SemaphoreHandle_t gScTxMutex = nullptr;

// ----- libsodium init (idempotent) -----
static bool scSodiumReady() {
  static bool ready = false;
  if (ready) return true;
  if (sodium_init() < 0) return false;
  ready = true;
  return true;
}

// ----- HMAC-SHA256 (arbitrary key length via libsodium multipart) -----
static void scHmac(const uint8_t* key, size_t keyLen,
                   const uint8_t* msg, size_t msgLen, uint8_t out[32]) {
  crypto_auth_hmacsha256_state st;
  crypto_auth_hmacsha256_init(&st, key, keyLen);
  crypto_auth_hmacsha256_update(&st, msg, msgLen);
  crypto_auth_hmacsha256_final(&st, out);
}

// ----- HKDF-SHA256 (RFC 5869), outLen <= 64 (we only need 64) -----
static void scHkdf(const uint8_t* ikm, size_t ikmLen,
                   const uint8_t* salt, size_t saltLen,
                   const char* info, size_t infoLen,
                   uint8_t* out, size_t outLen) {
  uint8_t prk[32];
  scHmac(salt, saltLen, ikm, ikmLen, prk);            // Extract
  uint8_t t[32];
  size_t  tLen = 0;
  size_t  done = 0;
  uint8_t counter = 1;
  while (done < outLen) {
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, prk, sizeof(prk));
    if (tLen) crypto_auth_hmacsha256_update(&st, t, tLen);
    crypto_auth_hmacsha256_update(&st, (const uint8_t*)info, infoLen);
    crypto_auth_hmacsha256_update(&st, &counter, 1);
    crypto_auth_hmacsha256_final(&st, t);
    tLen = 32;
    size_t n = (outLen - done < 32) ? (outLen - done) : 32;
    memcpy(out + done, t, n);
    done += n;
    counter++;
  }
  sodium_memzero(prk, sizeof(prk));
  sodium_memzero(t, sizeof(t));
}

// ----- PBKDF2-HMAC-SHA256, 32-byte output -----
// Uses mbedTLS so the inner SHA-256 runs on the ESP32 hardware accelerator. A
// software HMAC loop (libsodium) took ~17 s for 100k iterations on the S3 and
// tripped the task watchdog; mbedTLS HW-SHA is ~10x faster. Output is identical
// standard PBKDF2-HMAC-SHA256 (the app's test vectors are unaffected).
static void scPbkdf2(const uint8_t* pw, size_t pwLen,
                     const uint8_t* salt, size_t saltLen,
                     uint32_t iters, uint8_t out[32]) {
  if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, pw, pwLen,
                                    salt, saltLen, iters, 32, out) != 0) {
    memset(out, 0, 32);  // derivation failed → zero key; handshake will fail safely
  }
}

static bool scDerivePsk() {
  if (gPskValid) return true;
  if (!scSodiumReady()) return false;
  const String& s = gSettings.bleSecureChannelSecret;
  if (s.length() == 0) return false;  // no secret -> channel can't establish
  scPbkdf2((const uint8_t*)s.c_str(), s.length(),
           (const uint8_t*)SC_PSK_SALT, strlen(SC_PSK_SALT),
           SC_PBKDF2_ITERS, gPsk);
  gPskValid = true;
  return true;
}

void bleScInvalidatePsk() { gPskValid = false; sodium_memzero(gPsk, sizeof(gPsk)); }

void bleScWarmPsk() { scDerivePsk(); }  // derive+cache now (call off BTC_TASK)

void bleScInit() { if (!gScTxMutex) gScTxMutex = xSemaphoreCreateMutex(); }

// ----- per-connection table helpers -----
static ScConn* scFind(uint16_t connId) {
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++)
    if (gSc[i].inUse && gSc[i].connId == connId) return &gSc[i];
  return nullptr;
}
static ScConn* scAlloc(uint16_t connId) {
  ScConn* c = scFind(connId);
  if (c) return c;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (!gSc[i].inUse) { memset(&gSc[i], 0, sizeof(gSc[i])); gSc[i].inUse = true; gSc[i].connId = connId; return &gSc[i]; }
  }
  return nullptr;
}

void bleScReset(uint16_t connId) {
  ScConn* c = scFind(connId);
  if (c) { sodium_memzero(c, sizeof(*c)); }  // clears inUse + keys
}

bool bleScEstablished(uint16_t connId) {
  ScConn* c = scFind(connId);
  return c && c->phase == 2;
}

bool bleScRequired() {
  return gSettings.bleRequireSecureChannel && gSettings.bleSecureChannelSecret.length() > 0;
}

// ----- AEAD wrappers (ChaCha20-Poly1305-IETF, 12B nonce, 16B tag, no AAD) -----
static void scNonce(uint8_t out[12], uint32_t dirTag, uint64_t ctr) {
  out[0] = (dirTag >> 24) & 0xFF; out[1] = (dirTag >> 16) & 0xFF;
  out[2] = (dirTag >> 8) & 0xFF;  out[3] = dirTag & 0xFF;
  for (int i = 0; i < 8; i++) out[4 + i] = (ctr >> (8 * (7 - i))) & 0xFF;
}
static const uint32_t DIR_C2D = 0x00000000;
static const uint32_t DIR_D2C = 0x00000001;

// Encrypt one DATA frame into buf (must hold 1+8+len+16). Returns frame length.
static size_t scBuildData(ScConn* c, const uint8_t* pt, size_t len, uint8_t* buf) {
  uint64_t ctr = c->txCtr++;
  uint8_t nonce[12]; scNonce(nonce, DIR_D2C, ctr);
  buf[0] = SC_DATA;
  for (int i = 0; i < 8; i++) buf[1 + i] = (ctr >> (8 * (7 - i))) & 0xFF;
  unsigned long long tagLen = 0;
  crypto_aead_chacha20poly1305_ietf_encrypt_detached(
      buf + 9, buf + 9 + len, &tagLen, pt, len, nullptr, 0, nullptr, nonce, c->kD2C);
  return 9 + len + 16;
}

// Per-frame application framing header (inside the encrypted plaintext, device->app only).
// Lets the app reassemble multi-frame messages and DETECT loss: the secure channel's d2c
// counter already flags a dropped frame as a gap, but without this header the app can't tell
// which message is incomplete or where the next one begins. With it, every frame self-
// describes (msgId, fragIdx, fragCount), so a lost fragment leaves an identifiable hole — the
// app discards that msgId and re-requests WITHOUT advancing its cursor, instead of silently
// parsing a short/garbled page and stalling. See docs/BLE_SECURE_CHANNEL_FRAMING.md for the
// wire format the app must implement.
//   layout: [ver(1)][msgId lo][msgId hi][fragIdx(1)][fragCount(1)][payload...]
//   ver 0x01 = text (UTF-8 command reply, the default); ver 0x02 = raw binary blob (payload is
//   opaque bytes the app must NOT UTF-8-decode or console-echo — e.g. `fileread ... bin`).
static const uint8_t  SC_FRAME_VER     = 0x01;
static const uint8_t  SC_FRAME_VER_BIN = 0x02;
static const size_t   SC_APP_HDR       = 5;
static const size_t   SC_MAX_PAY_FRAME = SC_MAX_PT_PER_FRAME - SC_APP_HDR;  // 195 payload bytes/frame

// Inter-fragment pacing. notify() only queues into the controller's tx buffers; firing a
// multi-fragment message back-to-back (~2ms apart) overruns what a connection event can
// carry and Android silently drops the surplus notifications — so the app never reassembles
// the page and re-requests it. Spacing fragments at ~one connection interval lets the phone
// keep up. 30ms matches Android's default (unforced) ~30ms interval — we deliberately do NOT
// request a faster interval because WiFi/ESP-NOW share this radio (coexistence). Applied
// between frames only (single-frame messages — the common case — pay nothing).
static const TickType_t SC_FRAME_PACING = pdMS_TO_TICKS(30);

bool bleScSendEncrypted(uint16_t connId, const char* plaintext, size_t len, bool blocking, bool binaryFrame) {
  // Take the tx mutex for the WHOLE chunked message so its frames stay contiguous with
  // monotonic counters even when path A and path B send concurrently from two tasks.
  // blocking=false is the best-effort console-mirror path (the single debugOutputTask): if a
  // paced command result currently holds the mutex (hundreds of ms with pacing), skip rather
  // than stall the debug task — stalling it would back up gDebugOutputQueue and starve
  // serial/web/OLED. Command results pass blocking=true (must not drop).
  if (gScTxMutex && xSemaphoreTake(gScTxMutex, blocking ? portMAX_DELAY : 0) != pdTRUE) return false;
  ScConn* c = scFind(connId);
  if (!c || c->phase != 2) { if (gScTxMutex) xSemaphoreGive(gScTxMutex); return false; }

  // fragCount is carried in one byte, so a single framed message tops out at 255 fragments
  // (~49 KB). Callers that emit more must page (the CLI already pages espnowmessages json).
  size_t fragCount = (len + SC_MAX_PAY_FRAME - 1) / SC_MAX_PAY_FRAME;
  if (fragCount == 0) fragCount = 1;             // a zero-length message still sends one frame
  if (fragCount > 255) { if (gScTxMutex) xSemaphoreGive(gScTxMutex); return false; }
  uint16_t msgId = c->txMsgId++;

  // BLE TX trace (debugging): gated behind `debugbluetoothdata` (bleDataDebugEnabled),
  // routed to serial/web/file but NOT BLE (0x2F = MSG_ROUTE_ALL & ~MSG_ROUTE_BLE) so the
  // trace can't feed back into the channel we're tracing.
  extern void broadcastOutputCore_Routed(const char* text, size_t len, uint8_t route);
  extern bool bleDataDebugEnabled();
  const bool scTrace = bleDataDebugEnabled();
  if (scTrace) { char b[100];
    int n = snprintf(b, sizeof(b), "[BLE-SC] send msgId=%u len=%zu frags=%zu", (unsigned)msgId, len, fragCount);
    if (n > 0) broadcastOutputCore_Routed(b, (size_t)n, 0x2F); }

  uint8_t framePt[SC_MAX_PT_PER_FRAME];          // app-header + payload (pre-encryption)
  uint8_t frame[9 + SC_MAX_PT_PER_FRAME + 16];   // ctr + ciphertext + tag (on the wire)
  size_t  off = 0;
  uint8_t idx = 0;
  bool ok = true;
  do {
    size_t chunk = (len - off < SC_MAX_PAY_FRAME) ? (len - off) : SC_MAX_PAY_FRAME;
    framePt[0] = binaryFrame ? SC_FRAME_VER_BIN : SC_FRAME_VER;
    framePt[1] = (uint8_t)(msgId & 0xFF);
    framePt[2] = (uint8_t)((msgId >> 8) & 0xFF);
    framePt[3] = idx;
    framePt[4] = (uint8_t)fragCount;
    if (chunk) memcpy(framePt + SC_APP_HDR, (const uint8_t*)plaintext + off, chunk);
    size_t flen = scBuildData(c, framePt, SC_APP_HDR + chunk, frame);
    bool nok = bleRawNotify((const char*)frame, flen);   // bounded backpressure lives in bleRawNotify
    if (scTrace) { char b[110];
      int n = snprintf(b, sizeof(b), "[BLE-SC]  frame %u/%u payload=%zu wire=%zu notify=%s",
                       (unsigned)idx, (unsigned)fragCount, chunk, flen, nok ? "ok" : "FAIL");
      if (n > 0) broadcastOutputCore_Routed(b, (size_t)n, 0x2F); }
    if (!nok) { ok = false; break; }
    off += chunk;
    idx++;
    if (off < len) vTaskDelay(SC_FRAME_PACING);   // pace fragments so the phone doesn't drop them
  } while (off < len);
  sodium_memzero(framePt, sizeof(framePt));
  sodium_memzero(frame, sizeof(frame));
  if (gScTxMutex) xSemaphoreGive(gScTxMutex);
  return ok;
}

// Tell the app *why* the handshake won't proceed instead of going silent — the
// app can only observe silence as a generic timeout. body = reason(1).
// Best-effort: a failed notify just degrades back to the old silent behavior.
static void scSendReject(uint8_t reason) {
  uint8_t frame[2] = { SC_REJECT, reason };
  bleRawNotify((const char*)frame, sizeof(frame));
}

// ----- inbound handshake / data -----
BleScResult bleScHandleInbound(uint16_t connId, const uint8_t* data, size_t len,
                               char* out, size_t outCap, size_t* outLen) {
  if (len < 1) return BLE_SC_NOT_A_FRAME;
  uint8_t type = data[0];
  // Only our control/data type bytes are channel frames; printable input (CLI
  // commands start >= 0x20) is plaintext.
  if (type != SC_HELLO && type != SC_CONFIRM && type != SC_DATA) return BLE_SC_NOT_A_FRAME;
  if (!scSodiumReady() || !scDerivePsk()) {                      // no secret -> can't run channel
    scLog(connId, "secure frame received but no passphrase set (blesecret) — rejecting");
    scSendReject(SC_REJ_NO_PASSPHRASE);
    return BLE_SC_ERROR;
  }

  if (type == SC_HELLO) {
    if (len != 1 + 32 + 16) return BLE_SC_ERROR;
    ScConn* c = scAlloc(connId);
    if (!c) return BLE_SC_ERROR;
    const uint8_t* appPub   = data + 1;
    const uint8_t* appNonce = data + 33;
    uint8_t devPub[32], devNonce[16];
    crypto_kx_keypair(devPub, c->ephSec);          // X25519 keypair
    randombytes_buf(devNonce, sizeof(devNonce));
    uint8_t ss[32];
    if (crypto_scalarmult(ss, c->ephSec, appPub) != 0) { scLog(connId, "HELLO: invalid peer key, aborting"); scSendReject(SC_REJ_AUTH_FAILED); bleScReset(connId); return BLE_SC_ERROR; }
    // K = HKDF(ikm = ss||PSK, salt = appNonce||devNonce, info)
    uint8_t ikm[64];  memcpy(ikm, ss, 32); memcpy(ikm + 32, gPsk, 32);
    uint8_t salt[32]; memcpy(salt, appNonce, 16); memcpy(salt + 16, devNonce, 16);
    uint8_t k[64];
    scHkdf(ikm, sizeof(ikm), salt, sizeof(salt), SC_INFO, strlen(SC_INFO), k, sizeof(k));
    memcpy(c->kC2D, k, 32); memcpy(c->kD2C, k + 32, 32);
    c->phase = 1; c->rxCtr = 0; c->txCtr = 1;  // CONFIRM uses ctr 0 both ways
    sodium_memzero(ss, sizeof(ss)); sodium_memzero(ikm, sizeof(ikm)); sodium_memzero(k, sizeof(k));
    // Reply HELLO_ACK = type || devPub || devNonce
    uint8_t ack[1 + 32 + 16];
    ack[0] = SC_HELLO_ACK; memcpy(ack + 1, devPub, 32); memcpy(ack + 33, devNonce, 16);
    bleRawNotify((const char*)ack, sizeof(ack));
    scLog(connId, "HELLO ok, keys derived, HELLO_ACK sent — awaiting CONFIRM");
    return BLE_SC_CONSUMED;
  }

  if (type == SC_CONFIRM) {
    ScConn* c = scFind(connId);
    if (!c || c->phase != 1 || len != 1 + 2 + 16) return BLE_SC_ERROR;
    uint8_t nonce[12]; scNonce(nonce, DIR_C2D, 0);
    uint8_t pt[2];
    if (crypto_aead_chacha20poly1305_ietf_decrypt_detached(
            pt, nullptr, data + 1, 2, data + 3, nullptr, 0, nonce, c->kC2D) != 0) {
      scLog(connId, "CONFIRM failed to decrypt — wrong passphrase or MITM; dropping channel");
      scSendReject(SC_REJ_AUTH_FAILED);
      bleScReset(connId); return BLE_SC_ERROR;   // wrong PSK / MITM
    }
    if (pt[0] != 'o' || pt[1] != 'k') { scLog(connId, "CONFIRM bad payload; dropping channel"); scSendReject(SC_REJ_AUTH_FAILED); bleScReset(connId); return BLE_SC_ERROR; }
    // Reply CONFIRM_ACK = AEAD("ok", K_d2c, ctr 0)
    uint8_t ackNonce[12]; scNonce(ackNonce, DIR_D2C, 0);
    uint8_t ack[1 + 2 + 16]; ack[0] = SC_CONFIRM_ACK;
    unsigned long long tl = 0;
    const uint8_t okMsg[2] = { 'o', 'k' };
    crypto_aead_chacha20poly1305_ietf_encrypt_detached(
        ack + 1, ack + 3, &tl, okMsg, 2, nullptr, 0, nullptr, ackNonce, c->kD2C);
    c->phase = 2;  // established; data counters: c2d>0 expected, d2c next = txCtr(1)
    bleRawNotify((const char*)ack, sizeof(ack));
    scLog(connId, "secure channel ESTABLISHED");
    return BLE_SC_CONSUMED;
  }

  // SC_DATA
  ScConn* c = scFind(connId);
  if (!c || c->phase != 2 || len < 1 + 8 + 16) return BLE_SC_ERROR;
  uint64_t ctr = 0;
  for (int i = 0; i < 8; i++) ctr = (ctr << 8) | data[1 + i];
  if (ctr <= c->rxCtr) return BLE_SC_ERROR;     // replay / reorder
  size_t ctLen = len - 9 - 16;
  if (ctLen + 1 > outCap) return BLE_SC_ERROR;
  uint8_t nonce[12]; scNonce(nonce, DIR_C2D, ctr);
  if (crypto_aead_chacha20poly1305_ietf_decrypt_detached(
          (uint8_t*)out, nullptr, data + 9, ctLen, data + 9 + ctLen, nullptr, 0, nonce, c->kC2D) != 0) {
    return BLE_SC_ERROR;
  }
  c->rxCtr = ctr;
  out[ctLen] = '\0';
  if (outLen) *outLen = ctLen;
  return BLE_SC_PLAINTEXT_READY;
}

#endif // ENABLE_BLUETOOTH
