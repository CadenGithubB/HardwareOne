// Two-peer routing harness for the BLE Secure Channel (test_ble_sc_directed.py).
//
// The production System_BleSecureChannel.cpp is compiled whole beside this file
// against the ble_sc_stubs/ headers. The driver splices the production directed
// raw sender, session sender, console-mirror entry and deferred inbound job from
// Bluetooth.cpp into the /*@NAME@*/ slots below. The fake GATT keeps one inbox
// per connection: a directed esp_ble_gatts_send_indicate lands in the addressed
// connection's inbox only, and the characteristic-wide notify (bleRawNotify, the
// pre-fix primitive, modeled on the Arduino wrapper's notify()) lands in every
// connected inbox. Crypto is a routing-neutral stand-in: "encryption" copies the
// plaintext and appends a fixed tag, so frame types and lengths are real while
// keys are not tested here.
#include "System_BuildConfig.h"
#include "System_User.h"
#include "System_Settings.h"
#include "BLE_Types.h"
#include "System_BleSecureChannel.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <mbedtls/md.h>
#include <sodium.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define ENABLE_OLED_DISPLAY 0
#define BLE_DEBUGF(...) ((void)0)
using esp_err_t = int;
using esp_gatt_if_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr int ESP_GATT_IF_NONE = 0xff;
constexpr uint8_t MSG_ROUTE_ALL = 0x3F;
constexpr uint8_t MSG_ROUTE_BLE = 0x10;

// Every failed expectation is printed with its case; the run exits non-zero at
// the end, so a pre-fix source shows each defect rather than only the first.
static int failures = 0;
static const char* currentCase = "setup";
#define EXPECT(cond, msg)                                                   \
  do {                                                                      \
    if (!(cond)) {                                                          \
      fprintf(stderr, "FAIL %s: %s  [%s]\n", currentCase, msg, #cond);     \
      ++failures;                                                           \
    }                                                                       \
  } while (0)

Settings gSettings;

// ---- crypto doubles (routing-neutral) --------------------------------------
static bool decryptFails = false;
static void (*keypairHook)() = nullptr;  // runs inside HELLO processing
int sodium_init() { return 0; }
void sodium_memzero(void* p, size_t n) { memset(p, 0, n); }
void randombytes_buf(void* p, size_t n) { memset(p, 0x5A, n); }
int crypto_kx_keypair(unsigned char* pk, unsigned char* sk) {
  memset(pk, 0x11, 32);
  memset(sk, 0x22, 32);
  if (keypairHook) keypairHook();
  return 0;
}
int crypto_scalarmult(unsigned char* q, const unsigned char*, const unsigned char*) {
  memset(q, 0x33, 32);
  return 0;
}
int crypto_auth_hmacsha256_init(crypto_auth_hmacsha256_state*, const unsigned char*, size_t) { return 0; }
int crypto_auth_hmacsha256_update(crypto_auth_hmacsha256_state*, const unsigned char*, unsigned long long) {
  return 0;
}
int crypto_auth_hmacsha256_final(crypto_auth_hmacsha256_state*, unsigned char* out) {
  memset(out, 0x44, 32);
  return 0;
}
int crypto_aead_chacha20poly1305_ietf_encrypt_detached(
    unsigned char* c, unsigned char* mac, unsigned long long* maclen, const unsigned char* m,
    unsigned long long mlen, const unsigned char*, unsigned long long, const unsigned char*,
    const unsigned char*, const unsigned char*) {
  if (mlen) memmove(c, m, mlen);
  memset(mac, 0xAA, 16);
  if (maclen) *maclen = 16;
  return 0;
}
int crypto_aead_chacha20poly1305_ietf_decrypt_detached(
    unsigned char* m, unsigned char*, const unsigned char* c, unsigned long long clen,
    const unsigned char*, const unsigned char*, unsigned long long, const unsigned char*,
    const unsigned char*) {
  if (decryptFails) return -1;
  if (clen) memmove(m, c, clen);
  return 0;
}
static mbedtls_md_info_t sMdInfo;
const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t) { return &sMdInfo; }
void mbedtls_md_init(mbedtls_md_context_t*) {}
int mbedtls_md_setup(mbedtls_md_context_t*, const mbedtls_md_info_t*, int) { return 0; }
void mbedtls_md_free(mbedtls_md_context_t*) {}
int mbedtls_md_hmac_starts(mbedtls_md_context_t*, const unsigned char*, size_t) { return 0; }
int mbedtls_md_hmac_update(mbedtls_md_context_t*, const unsigned char*, size_t) { return 0; }
int mbedtls_md_hmac_finish(mbedtls_md_context_t*, unsigned char* out) {
  memset(out, 0x55, 32);
  return 0;
}
int mbedtls_md_hmac_reset(mbedtls_md_context_t*) { return 0; }

// ---- FreeRTOS doubles ------------------------------------------------------
static int pacingDelays = 0;  // the SC sender's 30 ms inter-fragment pacing
void vTaskDelay(TickType_t ticks) {
  if (ticks == pdMS_TO_TICKS(30)) ++pacingDelays;
}
static int sTxMutexObject = 0;
static int txMutexHeld = 0;
SemaphoreHandle_t xSemaphoreCreateMutex() { return &sTxMutexObject; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) {
  EXPECT(txMutexHeld == 0, "secure-channel tx mutex taken while already held");
  ++txMutexHeld;
  return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t) {
  EXPECT(txMutexHeld == 1, "secure-channel tx mutex released while not held");
  --txMutexHeld;
  return pdTRUE;
}

// ---- Bluetooth.cpp context: connection table, lifecycle guard, fake GATT ----
struct BLEConnection {
  bool active = false;
  uint16_t connId = 0;
  bool authed = false;
  String user;
  bool mirrorEligible = false;
};
struct BLESystemState {
  BLEConnection connections[BLE_MAX_CONNECTIONS];
  uint32_t responsesSent = 0;
};
static BLESystemState sState;
static BLESystemState* gBLEState = &sState;
static std::atomic<TransportSessionEpoch> sBleSessionEpoch[BLE_MAX_CONNECTIONS];
static std::atomic<int> sBleGattsIf{3};

static int guardDepth = 0;
struct BleLifecycleGuard {
  BleLifecycleGuard() { ++guardDepth; }
  ~BleLifecycleGuard() { --guardDepth; }
  explicit operator bool() const { return true; }
};

static int slotFor(uint16_t connId) {
  for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) {
    if (sState.connections[i].active && sState.connections[i].connId == connId) return i;
  }
  return -1;
}
static bool bleConnectionEpochMatchesLocked(uint16_t connId, TransportSessionEpoch epoch) {
  EXPECT(guardDepth >= 1, "epoch match read outside the lifecycle guard");
  const int slot = slotFor(connId);
  return epoch != 0 && slot >= 0 && sBleSessionEpoch[slot].load() == epoch;
}
TransportSessionEpoch bleSessionEpochForConnection(uint16_t connId) {
  BleLifecycleGuard guard;
  const int slot = slotFor(connId);
  return slot >= 0 ? sBleSessionEpoch[slot].load() : 0;
}
bool bleSessionEpochMatches(uint16_t connId, TransportSessionEpoch epoch) {
  return epoch != 0 && bleSessionEpochForConnection(connId) == epoch;
}
bool isBLEConnected() {
  for (const auto& c : sState.connections) {
    if (c.active) return true;
  }
  return false;
}

struct FakeResponseCharacteristic {
  uint16_t getHandle() const { return 42; }
};
static FakeResponseCharacteristic sResponseChar;
static FakeResponseCharacteristic* pCmdResponseChar = &sResponseChar;

static std::vector<std::string> inbox[BLE_MAX_CONNECTIONS];
static int directedSends = 0;
static int broadcastSends = 0;
static void (*afterWireHook)() = nullptr;  // runs after each frame leaves

esp_err_t esp_ble_gatts_send_indicate(esp_gatt_if_t gattsIf, uint16_t connId, uint16_t handle,
                                      size_t len, uint8_t* value, bool needConfirm) {
  EXPECT(guardDepth >= 1, "directed admission ran outside the BLE lifecycle guard");
  EXPECT(gattsIf == 3 && handle == 42 && !needConfirm, "directed send used the wrong interface/handle");
  const int slot = slotFor(connId);
  if (slot < 0) return ESP_FAIL;
  inbox[slot].emplace_back(reinterpret_cast<const char*>(value), len);
  ++directedSends;
  if (afterWireHook) afterWireHook();
  return ESP_OK;
}

// The characteristic-wide primitive the pre-fix sources call (Bluetooth.cpp's
// bleRawNotify → Arduino BLECharacteristic::notify()): one shared value, sent
// to every connected peer, with no session check. Fixed sources never call it.
bool bleRawNotify(const char* data, size_t len) {
  if (!isBLEConnected() || !pCmdResponseChar) return false;
  ++broadcastSends;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) {
    if (sState.connections[i].active) inbox[i].emplace_back(data, len);
  }
  if (afterWireHook) afterWireHook();
  return true;
}

void broadcastOutput(const String&) {}
void broadcastOutputCore_Routed(const char*, size_t, uint8_t) {}
bool bleDataDebugEnabled() { return true; }  // exercise the trace formatting

static std::vector<std::string> commandLines;
static int poolReleases = 0;
// The arrival flag is defaulted so the pre-fix four-argument call also compiles;
// test_ble_sc_policy.py covers what the command tail does with it.
static void processBleCommandLine(uint16_t, TransportSessionEpoch, const char* data, size_t len,
                                  bool /*arrivedEncrypted*/ = false) {
  commandLines.emplace_back(data, len);
}
static void bleMarkActivity(uint16_t) {}
bool otaBleHandleEncryptedFrame(uint16_t, const uint8_t*, size_t) { return true; }

bool sendBLEResponse(const char* data, size_t len, bool blocking = true);
bool sendBLEResponseToSession(uint16_t connId, TransportSessionEpoch expectedEpoch,
                              const char* data, size_t len, bool blocking = true);

// ---- production blocks from Bluetooth.cpp ----------------------------------
/*@FENCE_STRUCT@*/;
/*@FENCE_LIVE@*/
/*@RAW_SESSION@*/
/*@SEND@*/
/*@MIRROR_COLLECT@*/
/*@MIRROR_SEND@*/
/*@DEFERRED_STRUCT@*/;
static void bleScPoolRelease(BleScDeferred*) { ++poolReleases; }
/*@DEFERRED@*/

// ---- helpers ---------------------------------------------------------------
static const uint8_t kHelloAck = 0x02, kConfirmAck = 0x04, kReject = 0x05, kData = 0x10;
static std::string reject(uint8_t reason) { return std::string{static_cast<char>(kReject), static_cast<char>(reason)}; }

static std::vector<uint8_t> hello() {
  std::vector<uint8_t> f(1 + 32 + 16, 0x77);
  f[0] = 0x01;
  return f;
}
static std::vector<uint8_t> confirm() {
  std::vector<uint8_t> f(1 + 2 + 16, 0x6F);
  f[0] = 0x03;
  f[1] = 'o';
  f[2] = 'k';
  return f;
}
static std::vector<uint8_t> dataFrame(uint64_t ctr, const std::string& text) {
  std::vector<uint8_t> f(1 + 8, 0);
  f[0] = kData;
  for (int i = 0; i < 8; ++i) f[1 + i] = static_cast<uint8_t>(ctr >> (8 * (7 - i)));
  f.insert(f.end(), text.begin(), text.end());
  f.insert(f.end(), 16, 0xAA);
  return f;
}
// What processIncomingBLECommand hands to cmd_exec: the frame plus the session
// epoch captured on BTC_TASK.
static void deliver(uint16_t connId, TransportSessionEpoch epoch, const std::vector<uint8_t>& frame) {
  static BleScDeferred job;
  memset(&job, 0, sizeof(job));
  job.connId = connId;
  job.sessionEpoch = epoch;
  job.len = static_cast<uint16_t>(frame.size());
  memcpy(job.buf, frame.data(), frame.size());
  bleScDeferredInbound(&job);
}
static void peer(int slot, uint16_t connId, TransportSessionEpoch epoch, const char* user = nullptr) {
  BLEConnection& c = sState.connections[slot];
  c = BLEConnection{};
  c.active = true;
  c.connId = connId;
  c.authed = user != nullptr;
  c.user = user ? user : "";
  c.mirrorEligible = user != nullptr;
  sBleSessionEpoch[slot].store(epoch);
}
static void clearWire() {
  for (auto& box : inbox) box.clear();
  directedSends = broadcastSends = pacingDelays = 0;
  commandLines.clear();
}
// Conn 0 (slot 0, epoch 7) and conn 1 (slot 1, epoch 9) connected, nobody
// logged in, no channel, passphrase "pw".
static void resetWorld(const char* name) {
  currentCase = name;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) {
    bleScReset(static_cast<uint16_t>(i));
    sState.connections[i] = BLEConnection{};
    sBleSessionEpoch[i].store(0);
  }
  peer(0, 0, 7);
  peer(1, 1, 9);
  gSettings = Settings{};
  gSettings.bleSecureChannelSecret = "pw";
  bleScInvalidatePsk();
  decryptFails = false;
  keypairHook = nullptr;
  afterWireHook = nullptr;
  pCmdResponseChar = &sResponseChar;
  clearWire();
}
static void establish(uint16_t connId, TransportSessionEpoch epoch) {
  deliver(connId, epoch, hello());
  deliver(connId, epoch, confirm());
}
static bool frameIs(const std::string& f, uint8_t type, size_t len) {
  return f.size() == len && static_cast<uint8_t>(f[0]) == type;
}

static void rotateConn1() { sBleSessionEpoch[1].store(0); }
static void stopServerNow() {
  // deinitBluetooth: every session epoch is invalidated under the lifecycle
  // guard before the GATT tree is deleted and the response pointer cleared.
  BleLifecycleGuard guard;
  for (auto& epoch : sBleSessionEpoch) epoch.store(0);
}

int main() {
  bleScInit();

  // C1: HELLO on conn 1 gets its HELLO_ACK on conn 1 only.
  resetWorld("C1 HELLO_ACK");
  deliver(1, 9, hello());
  EXPECT(inbox[1].size() == 1 && frameIs(inbox[1][0], kHelloAck, 49), "conn 1 missing its HELLO_ACK");
  EXPECT(inbox[0].empty(), "HELLO_ACK for conn 1 reached conn 0");
  EXPECT(broadcastSends == 0, "HELLO_ACK used the characteristic-wide notify");
  EXPECT(directedSends == 1, "HELLO_ACK was not one directed send");

  // C2: CONFIRM on conn 1 gets CONFIRM_ACK on conn 1 only; only conn 1 is established.
  currentCase = "C2 CONFIRM_ACK";
  deliver(1, 9, confirm());
  EXPECT(inbox[1].size() == 2 && frameIs(inbox[1][1], kConfirmAck, 19), "conn 1 missing its CONFIRM_ACK");
  EXPECT(inbox[0].empty(), "CONFIRM_ACK for conn 1 reached conn 0");
  EXPECT(bleScEstablished(1) && !bleScEstablished(0), "channel state crossed connections");
  EXPECT(broadcastSends == 0 && directedSends == 2, "handshake replies were not directed");
  // An inbound DATA frame on the established channel runs as conn 1's command and sends nothing.
  clearWire();
  deliver(1, 9, dataFrame(1, "status"));
  EXPECT(commandLines.size() == 1 && commandLines[0] == "status", "DATA frame did not reach the command tail");
  EXPECT(inbox[0].empty() && inbox[1].empty(), "inbound DATA produced a frame");

  // C3a: CONFIRM that fails to decrypt on conn 0 → REJECT(0x02) to conn 0 only.
  resetWorld("C3a REJECT auth failed");
  establish(1, 9);
  clearWire();
  decryptFails = true;
  deliver(0, 7, hello());
  deliver(0, 7, confirm());
  decryptFails = false;
  EXPECT(inbox[0].size() == 2 && frameIs(inbox[0][0], kHelloAck, 49) && inbox[0][1] == reject(0x02),
         "conn 0 missing its HELLO_ACK + REJECT(auth failed)");
  EXPECT(inbox[1].empty(), "conn 0's handshake replies reached conn 1 (wrong-passphrase REJECT included)");
  EXPECT(!bleScEstablished(0) && bleScEstablished(1), "a failed handshake changed the other channel");
  EXPECT(broadcastSends == 0, "REJECT used the characteristic-wide notify");

  // C3b: no passphrase set → REJECT(0x01) to the sender only.
  resetWorld("C3b REJECT no passphrase");
  gSettings.bleSecureChannelSecret = "";
  bleScInvalidatePsk();
  deliver(0, 7, hello());
  EXPECT(inbox[0].size() == 1 && inbox[0][0] == reject(0x01), "conn 0 missing REJECT(no passphrase)");
  EXPECT(inbox[1].empty(), "REJECT for conn 0 reached conn 1");

  // C4: a 500-byte result for conn 1's session: three DATA fragments on conn 1 only.
  resetWorld("C4 DATA fragments");
  establish(1, 9);
  clearWire();
  const std::string big(500, 'x');
  EXPECT(sendBLEResponseToSession(1, 9, big.data(), big.size(), true), "secure result not sent");
  EXPECT(inbox[1].size() == 3, "conn 1 did not get exactly three fragments");
  if (inbox[1].size() == 3) {
    EXPECT(frameIs(inbox[1][0], kData, 225) && frameIs(inbox[1][1], kData, 225) &&
               frameIs(inbox[1][2], kData, 140),
           "fragment types/lengths changed");
  }
  EXPECT(inbox[0].empty(), "DATA fragments for conn 1 reached conn 0");
  EXPECT(broadcastSends == 0 && directedSends == 3, "DATA fragments were not directed");
  EXPECT(pacingDelays == 2, "inter-fragment pacing changed");
  EXPECT(guardDepth == 0 && txMutexHeld == 0, "guard or tx mutex leaked");
  // The unbound wrapper (`fileread ... bin` chunks) binds to the connection's current session.
  clearWire();
  EXPECT(bleScSendEncrypted(1, "bin", 3, true, true), "binary chunk not sent");
  EXPECT(inbox[1].size() == 1 && frameIs(inbox[1][0], kData, 1 + 8 + 5 + 3 + 16), "conn 1 missing its binary chunk");
  EXPECT(inbox[0].empty(), "binary chunk for conn 1 reached conn 0");

  // C5: a stale session epoch sends nothing.
  currentCase = "C5 stale epoch";
  clearWire();
  EXPECT(!sendBLEResponseToSession(1, 8, "q", 1, true), "stale-epoch send reported success");
  EXPECT(inbox[0].empty() && inbox[1].empty(), "stale-epoch send reached a peer");

  // C6: conn 1's session rotates after the first fragment leaves: the transfer
  // stops at the next fragment and the channel slot is untouched.
  currentCase = "C6 epoch rotates mid-message";
  clearWire();
  afterWireHook = rotateConn1;
  EXPECT(!sendBLEResponseToSession(1, 9, big.data(), big.size(), true), "transfer continued after rotation");
  afterWireHook = nullptr;
  EXPECT(inbox[1].size() == 1, "conn 1 got more than the fragment sent before the rotation");
  EXPECT(inbox[0].empty(), "a fragment for conn 1 reached conn 0");
  EXPECT(bleScEstablished(1), "rotation check reset the channel slot");
  sBleSessionEpoch[1].store(9);

  // C7a: plaintext console mirror to two logged-in sessions: one directed copy each.
  resetWorld("C7a plaintext mirror");
  peer(0, 0, 7, "alice");
  peer(1, 1, 9, "bob");
  EXPECT(sendBLEResponse("line", 4, false), "mirror not delivered");
  EXPECT(inbox[0] == std::vector<std::string>{"line"} && inbox[1] == std::vector<std::string>{"line"},
         "each logged-in session must get exactly one copy");
  EXPECT(broadcastSends == 0 && directedSends == 2, "plaintext mirror used the characteristic-wide notify");

  // C7b: secure console mirror to conn 1 (logged in, channel up); conn 0 is a
  // connected peer that never logged in and holds no channel.
  resetWorld("C7b secure mirror");
  establish(1, 9);
  peer(1, 1, 9, "bob");
  clearWire();
  EXPECT(sendBLEResponse("x", 1, true), "secure mirror not delivered");
  EXPECT(inbox[1].size() == 1 && frameIs(inbox[1][0], kData, 1 + 8 + 5 + 1 + 16), "conn 1 missing its encrypted mirror");
  EXPECT(inbox[0].empty(), "encrypted mirror frame for conn 1 reached conn 0");
  EXPECT(broadcastSends == 0, "secure mirror used the characteristic-wide notify");

  // C8: a frame whose session epoch is no longer live is dropped before the SC module.
  resetWorld("C8 dead ingress epoch");
  const int releasesBefore = poolReleases;
  deliver(0, 0, hello());
  deliver(0, 6, hello());
  EXPECT(inbox[0].empty() && inbox[1].empty(), "a dead-epoch frame produced a reply");
  EXPECT(poolReleases == releasesBefore + 2, "pool slot not released exactly once per frame");

  // C9: secure channel required, conn 0 has none: plaintext refused, nothing sent.
  resetWorld("C9 secure required");
  gSettings.bleRequireSecureChannel = true;
  EXPECT(!sendBLEResponseToSession(0, 7, "p", 1, true), "plaintext sent although the channel is required");
  EXPECT(inbox[0].empty() && inbox[1].empty(), "plaintext reached a peer in secure-required mode");

  // C10: the server stops (every epoch invalidated under the lifecycle guard)
  // while a HELLO is being processed on cmd_exec: the HELLO_ACK must not reach
  // the radio, because teardown is about to delete the GATT tree.
  resetWorld("C10 server stop mid-handshake");
  keypairHook = stopServerNow;
  deliver(0, 7, hello());
  keypairHook = nullptr;
  EXPECT(inbox[0].empty() && inbox[1].empty(), "HELLO_ACK sent after the server invalidated every session");
  EXPECT(directedSends == 0 && broadcastSends == 0, "a frame left during teardown");
  // And with the response characteristic already gone, nothing is attempted.
  resetWorld("C10b response characteristic gone");
  establish(1, 9);
  clearWire();
  pCmdResponseChar = nullptr;
  EXPECT(!sendBLEResponseToSession(1, 9, "z", 1, true), "send reported success without a characteristic");
  EXPECT(inbox[0].empty() && inbox[1].empty(), "a frame left without a characteristic");
  pCmdResponseChar = &sResponseChar;

  if (failures) {
    fprintf(stderr, "BLE secure-channel routing: %d expectation(s) failed\n", failures);
    return 1;
  }
  puts("BLE secure-channel routing: HELLO_ACK, CONFIRM_ACK, REJECT and every DATA fragment reach only their "
       "own connection and session under the lifecycle guard; stale, rotated and torn-down sessions receive "
       "nothing; the console mirror sends one directed copy per recipient");
  return 0;
}
