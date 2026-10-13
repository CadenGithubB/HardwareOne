// Secure-channel policy harness for the BLE command ingress (test_ble_sc_policy.py).
//
// The production System_BleSecureChannel.cpp is compiled whole beside this file
// against the ble_sc_stubs/ headers, so bleScRequired, bleScEstablished, the
// handshake and DATA-frame decryption are the real ones. The driver splices the
// production ingress from Bluetooth.cpp into the /*@NAME@*/ slots below: the
// GATT write entry (processIncomingBLECommand), the deferred secure-frame and
// plaintext jobs, the command tail with its file-browser gate, and the directed
// and session senders. The fake GATT keeps one inbox per connection. write()
// runs the GATT entry as BTC_TASK; deferred jobs queue in submission order and
// run as cmd_exec_task once it returns. submitCommandAsync records what would
// execute. The FreeRTOS doubles count every mutex take that may wait (only the
// secure-channel tx mutex is taken) and every delay made on BTC_TASK. Crypto is a
// routing-neutral stand-in ("encryption" copies the plaintext and appends a
// fixed tag), so an encrypted reply still shows its text inside the DATA frame.
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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>
#include <utility>
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
constexpr uint8_t MSG_ROUTE_FILE = 0x04;

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
int sodium_init() { return 0; }
void sodium_memzero(void* p, size_t n) { memset(p, 0, n); }
void randombytes_buf(void* p, size_t n) { memset(p, 0x5A, n); }
int crypto_kx_keypair(unsigned char* pk, unsigned char* sk) {
  memset(pk, 0x11, 32);
  memset(sk, 0x22, 32);
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
// True while the GATT write entry runs, i.e. on BTC_TASK, which serves the
// GATT/GAP callbacks of every link. Deferred jobs run with it false (cmd_exec).
static bool onBtcTask = false;
static int btcTxMutexWaits = 0;  // a take that may wait, made on BTC_TASK
static int btcSleeps = 0;        // a delay made on BTC_TASK
static int backoffDelays = 0;  // the directed sender's 15 ms retry backoff
void vTaskDelay(TickType_t ticks) {
  if (ticks == pdMS_TO_TICKS(15)) ++backoffDelays;
  if (onBtcTask) ++btcSleeps;
}
static int sTxMutexObject = 0;
SemaphoreHandle_t xSemaphoreCreateMutex() { return &sTxMutexObject; }
// The only mutex the compiled code takes is the secure-channel tx mutex, which a
// paced multi-fragment reply holds for up to ~0.65 s. A take with a wait made on
// BTC_TASK would stall every link for that long.
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t wait) {
  if (onBtcTask && wait != 0) ++btcTxMutexWaits;
  return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
static uint32_t millis() { return 1000; }

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
static bool bleIsAuthed(uint16_t connId, String& outUser) {
  const int slot = slotFor(connId);
  if (slot < 0 || !sState.connections[slot].authed) return false;
  outUser = sState.connections[slot].user;
  return true;
}
static TransportSessionEpoch bleLogout(uint16_t connId) { return bleSessionEpochForConnection(connId); }
static bool isAdminUser(const String&) { return false; }
static void bleMarkActivity(uint16_t) {}

struct FakeResponseCharacteristic {
  uint16_t getHandle() const { return 42; }
};
static FakeResponseCharacteristic sResponseChar;
static FakeResponseCharacteristic* pCmdResponseChar = &sResponseChar;

static std::vector<std::string> inbox[BLE_MAX_CONNECTIONS];
static int broadcastSends = 0;

esp_err_t esp_ble_gatts_send_indicate(esp_gatt_if_t gattsIf, uint16_t connId, uint16_t handle,
                                      size_t len, uint8_t* value, bool needConfirm) {
  EXPECT(guardDepth >= 1, "directed admission ran outside the BLE lifecycle guard");
  EXPECT(gattsIf == 3 && handle == 42 && !needConfirm, "directed send used the wrong interface/handle");
  const int slot = slotFor(connId);
  if (slot < 0) return ESP_FAIL;
  inbox[slot].emplace_back(reinterpret_cast<const char*>(value), len);
  return ESP_OK;
}

// The characteristic-wide primitive that pre-fix secure-channel sources call:
// one shared value, sent to every connected peer. Fixed sources never call it.
bool bleRawNotify(const char* data, size_t len) {
  if (!isBLEConnected() || !pCmdResponseChar) return false;
  ++broadcastSends;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) {
    if (sState.connections[i].active) inbox[i].emplace_back(data, len);
  }
  return true;
}

void broadcastOutput(const String&) {}
void broadcastOutputCore_Routed(const char*, size_t, uint8_t) {}
bool bleDataDebugEnabled() { return false; }

// ---- command-system doubles ------------------------------------------------
// CommandArgs::arg returns the Arduino String, which has equalsIgnoreCase; the
// host String is std::string, so the argument type adds just that member.
struct HarnessArg : std::string {
  using std::string::string;
  bool equalsIgnoreCase(const char* other) const { return strcasecmp(c_str(), other) == 0; }
};
class CommandArgs {
 public:
  explicit CommandArgs(const String& input) {
    size_t i = 0;
    while (i < input.size()) {
      while (i < input.size() && input[i] == ' ') ++i;
      size_t start = i;
      while (i < input.size() && input[i] != ' ') ++i;
      if (i > start) args_.emplace_back(input.substr(start, i - start).c_str());
    }
  }
  const HarnessArg& arg(int index) const {
    static const HarnessArg empty;
    return (index >= 0 && index < count()) ? args_[static_cast<size_t>(index)] : empty;
  }
  int count() const { return static_cast<int>(args_.size()); }
  bool unterminatedQuote() const { return false; }

 private:
  std::vector<HarnessArg> args_;
};
static String redactCmdForAudit(const String& line) { return line; }
static constexpr size_t kPublicUsernameMaxLen = 64;  // System_User.h
static constexpr size_t kPublicPasswordMaxLen = 64;

enum CommandOrigin { ORIGIN_BLUETOOTH = 4 };
enum CommandSource { SOURCE_BLUETOOTH = 4 };
constexpr uint32_t COMMAND_CONTEXT_REQUIRE_LIVE_SESSION = 1u << 0;
constexpr uint32_t COMMAND_CONTEXT_MODE_INDEPENDENT = 1u << 1;
struct AuthContext {
  CommandSource transport = SOURCE_BLUETOOTH;
  String user;
};
struct CommandContext {
  CommandOrigin origin = ORIGIN_BLUETOOTH;
  AuthContext auth;
  TransportSessionEpoch transportSessionEpoch = 0;
  uint32_t behaviorFlags = 0;
  bool validateOnly = false;
  uint8_t outputMask = 0;
  void* replyHandle = nullptr;
  void* httpReq = nullptr;
  uint32_t id = 0;
  uint32_t timestampMs = 0;
};
struct Command {
  String line;
  CommandContext ctx;
};
typedef void (*ExecAsyncCallback)(bool ok, const char* result, void* userData);
typedef void (*DeferredFn)(void* arg);

static bool bleFillCommandAuth(uint16_t connId, AuthContext& auth, bool,
                               TransportSessionEpoch& epochOut) {
  epochOut = bleSessionEpochForConnection(connId);
  if (epochOut == 0) return false;
  auth.transport = SOURCE_BLUETOOTH;
  String user;
  if (bleIsAuthed(connId, user)) auth.user = user;
  return true;
}

enum class AllocPref { PreferPSRAM, RequireInternal };
static void* ps_alloc(size_t size, AllocPref, const char*) { return malloc(size); }

static int drops = 0;
static void cmdExecDropCountIncrement() { ++drops; }
// cmd_exec_task stand-in: deferred jobs queue in submission order and run when
// write() has returned from the GATT entry (drainCmdExec). cmdExecQueueFull
// models a full gCmdExecQ, which refuses both submit paths.
static bool cmdExecQueueFull = false;
static std::vector<std::pair<DeferredFn, void*>> cmdExecJobs;
static bool submitDeferredToCmdExec(DeferredFn fn, void* arg) {
  if (cmdExecQueueFull) {
    cmdExecDropCountIncrement();
    return false;
  }
  cmdExecJobs.emplace_back(fn, arg);
  return true;
}
static void drainCmdExec() {
  while (!cmdExecJobs.empty()) {
    const auto job = cmdExecJobs.front();
    cmdExecJobs.erase(cmdExecJobs.begin());
    job.first(job.second);
  }
}
static int otaFrames = 0;
bool otaBleHandleEncryptedFrame(uint16_t, const uint8_t*, size_t) {
  ++otaFrames;
  return true;
}

bool sendBLEResponse(const char* data, size_t len, bool blocking = true);
bool sendBLEResponseToSession(uint16_t connId, TransportSessionEpoch expectedEpoch,
                              const char* data, size_t len, bool blocking = true);

// ---- production blocks from Bluetooth.cpp ----------------------------------
/*@FENCE_STRUCT@*/;
/*@FENCE_LIVE@*/
/*@RAW_SESSION@*/
/*@SEND@*/
/*@LOGIN_JOB@*/;
/*@REPLY_CTX@*/;

static void bleLoginJobFree(BleLoginAsyncJob* job) { free(job); }
static void bleLoginDeferred(void* arg) { bleLoginJobFree(static_cast<BleLoginAsyncJob*>(arg)); }
static void bleCommandResultCallback(bool, const char*, void*) {}

// What reached the command executor: the session it was admitted for and the line.
struct Queued {
  uint16_t connId;
  TransportSessionEpoch epoch;
  std::string line;
};
static std::vector<Queued> queued;
static bool submitCommandAsync(const Command& cmd, ExecAsyncCallback callback, void* userData) {
  if (cmdExecQueueFull) return false;  // the caller frees userData
  EXPECT(callback == bleCommandResultCallback, "command queued without the BLE reply callback");
  const auto* reply = static_cast<const BleCommandReplyContext*>(userData);
  queued.push_back({reply ? reply->connId : static_cast<uint16_t>(0xFFFF),
                    cmd.ctx.transportSessionEpoch, cmd.line});
  free(userData);
  return true;
}

/*@AUTH_REQUIRED@*/
/*@FILE_VERB@*/
/*@LINE@*/
/*@DEFERRED_STRUCT@*/;
static BleScDeferred sPoolSlot;
static int poolClaims = 0, poolReleases = 0;
// Set when a slot is released while it still holds bytes of the write it carried.
// The W cases (plaintext writes only) require it to stay false: a plaintext line
// can carry a login password, and the production pool prefers PSRAM.
static bool releasedWithResidue = false;
static BleScDeferred* bleScPoolClaim() {
  ++poolClaims;
  return &sPoolSlot;
}
static void bleScPoolRelease(BleScDeferred* d) {
  ++poolReleases;
  for (size_t i = 0; i < d->len && i < sizeof(d->buf); ++i) {
    if (d->buf[i] != 0) {
      releasedWithResidue = true;
      break;
    }
  }
}
/*@DEFERRED@*/
/*@PLAIN_DEFERRED@*/
/*@INGRESS@*/

// ---- helpers ---------------------------------------------------------------
static const uint8_t kData = 0x10;
static const char kRequiredNotice[] = "Secure channel required — connect with the encrypted app.";
static const char kFileRefusal[] = "{\"success\":false,\"error\":\"secure_channel_required\"}";

// A GATT write from the app: what the characteristic's onWrite hands over, on
// BTC_TASK; then cmd_exec_task runs whatever the entry deferred.
static void write(uint16_t connId, const std::string& bytes) {
  onBtcTask = true;
  processIncomingBLECommand(connId, bytes.data(), bytes.size());
  onBtcTask = false;
  drainCmdExec();
}
static std::string hello() {
  std::string f(1 + 32 + 16, '\x77');
  f[0] = 0x01;
  return f;
}
static std::string confirm() {
  std::string f(1 + 2 + 16, '\x6F');
  f[0] = 0x03;
  f[1] = 'o';
  f[2] = 'k';
  return f;
}
static uint64_t nextCtr[BLE_MAX_CONNECTIONS];
// An app->device DATA frame carrying `text` (the stand-in AEAD copies it).
static std::string dataFrame(uint16_t connId, const std::string& text) {
  const uint64_t ctr = ++nextCtr[slotFor(connId)];
  std::string f(1 + 8, '\0');
  f[0] = static_cast<char>(kData);
  for (int i = 0; i < 8; ++i) f[1 + i] = static_cast<char>(ctr >> (8 * (7 - i)));
  f += text;
  f.append(16, '\xAA');
  return f;
}
static void peer(int slot, uint16_t connId, TransportSessionEpoch epoch) {
  BLEConnection& c = sState.connections[slot];
  c = BLEConnection{};
  c.active = true;
  c.connId = connId;
  sBleSessionEpoch[slot].store(epoch);
}
static void clearWire() {
  for (auto& box : inbox) box.clear();
  queued.clear();
  broadcastSends = backoffDelays = 0;
  btcTxMutexWaits = btcSleeps = 0;
}
// Conn 0 (slot 0, epoch 7) and conn 1 (slot 1, epoch 9) connected, no channel,
// passphrase "pw" set, encryption optional, BLE login not required.
static void resetWorld(const char* name, bool required) {
  currentCase = name;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) {
    bleScReset(static_cast<uint16_t>(i));
    sState.connections[i] = BLEConnection{};
    sBleSessionEpoch[i].store(0);
    nextCtr[i] = 0;
  }
  peer(0, 0, 7);
  peer(1, 1, 9);
  gSettings = Settings{};
  gSettings.bleSecureChannelSecret = "pw";
  gSettings.bleRequireSecureChannel = required;
  bleScInvalidatePsk();
  clearWire();
}
static void establish(uint16_t connId) {
  write(connId, hello());
  write(connId, confirm());
  EXPECT(bleScEstablished(connId), "test setup: the channel did not establish");
  clearWire();
}
static bool isDataFrameWith(const std::string& frame, const char* text) {
  return !frame.empty() && static_cast<uint8_t>(frame[0]) == kData &&
         frame.find(text) != std::string::npos;
}
static bool onlyQueued(uint16_t connId, TransportSessionEpoch epoch, const std::string& line) {
  return queued.size() == 1 && queued[0].connId == connId && queued[0].epoch == epoch &&
         queued[0].line == line;
}

int main() {
  bleScInit();

  // R1: secure channel required, conn 0 has no channel and writes plaintext:
  // the fixed refusal reaches conn 0 (plaintext, it carries no data) and nobody else.
  resetWorld("R1 required-mode refusal without a channel", true);
  write(0, "status");
  EXPECT(queued.empty(), "a plaintext command ran although the secure channel is required");
  EXPECT(inbox[0].size() == 1 && inbox[0][0] == kRequiredNotice,
         "required-mode refusal never reached the plaintext client");
  EXPECT(inbox[1].empty(), "the refusal for conn 0 reached conn 1");
  EXPECT(broadcastSends == 0, "the refusal used the characteristic-wide notify");
  EXPECT(backoffDelays == 0, "the refusal slept in the retry backoff");

  // R2: required, conn 1 holds a channel and writes plaintext: the refusal goes
  // over its channel as one DATA frame, to conn 1 only (unchanged).
  resetWorld("R2 required-mode refusal over an established channel", true);
  establish(1);
  write(1, "status");
  EXPECT(queued.empty(), "a plaintext command ran on a channel holder in required mode");
  EXPECT(inbox[1].size() == 1 && isDataFrameWith(inbox[1][0], kRequiredNotice),
         "the channel holder did not get the refusal as one encrypted DATA frame");
  EXPECT(inbox[0].empty(), "the refusal for conn 1 reached conn 0");
  EXPECT(btcTxMutexWaits == 0,
         "the refusal over the channel waited on the secure-channel tx mutex on BTC_TASK");
  // The same session's encrypted commands still run.
  clearWire();
  write(1, dataFrame(1, "status"));
  EXPECT(onlyQueued(1, 9, "status"), "an encrypted command did not run in required mode");

  // R3: no passphrase, so nothing is required: plaintext commands run.
  resetWorld("R3 no secret, plaintext allowed", true);
  gSettings.bleSecureChannelSecret = "";
  bleScInvalidatePsk();
  write(0, "status");
  EXPECT(onlyQueued(0, 7, "status"), "a plaintext command did not run without a secret");
  EXPECT(inbox[0].empty() && inbox[1].empty(), "a runnable plaintext command produced a refusal");

  // F1: encryption optional (secret set, `blesecure off`), conn 1 holds a channel
  // but writes `fileread` in plaintext: refused like any cleartext file verb.
  resetWorld("F1 plaintext fileread on a channel holder", false);
  establish(1);
  write(1, "fileread /notes.txt");
  EXPECT(queued.empty(), "plaintext fileread ran because the connection also holds a channel");
  EXPECT(inbox[1].size() == 1 && isDataFrameWith(inbox[1][0], kFileRefusal),
         "plaintext fileread on a channel holder did not get secure_channel_required");
  EXPECT(inbox[0].empty(), "the file-verb refusal for conn 1 reached conn 0");

  // F2: the same for filewrite, whose inline content crossed the air in clear.
  currentCase = "F2 plaintext filewrite on a channel holder";
  clearWire();
  write(1, "filewrite /a.txt hello");
  EXPECT(queued.empty(), "plaintext filewrite ran because the connection also holds a channel");
  EXPECT(inbox[1].size() == 1 && isDataFrameWith(inbox[1][0], kFileRefusal),
         "plaintext filewrite on a channel holder did not get secure_channel_required");

  // F3: the same verbs inside DATA frames run (unchanged).
  currentCase = "F3 encrypted file verbs";
  clearWire();
  write(1, dataFrame(1, "fileread /notes.txt"));
  EXPECT(onlyQueued(1, 9, "fileread /notes.txt"), "an encrypted fileread did not run");
  clearWire();
  write(1, dataFrame(1, "filewrite /a.txt hello"));
  EXPECT(onlyQueued(1, 9, "filewrite /a.txt hello"), "an encrypted filewrite did not run");
  EXPECT(inbox[0].empty() && inbox[1].empty(), "an encrypted file verb was refused");

  // F4: conn 0 without a channel, plaintext fileread: plaintext refusal to conn 0 (unchanged).
  currentCase = "F4 plaintext fileread without a channel";
  clearWire();
  write(0, "fileread /notes.txt");
  EXPECT(queued.empty(), "plaintext fileread ran without a channel");
  EXPECT(inbox[0].size() == 1 && inbox[0][0] == kFileRefusal,
         "plaintext fileread without a channel did not get the plaintext refusal");
  EXPECT(inbox[1].empty(), "the file-verb refusal for conn 0 reached conn 1");

  // F5: non-file plaintext commands on a channel holder still run in optional mode (unchanged).
  currentCase = "F5 plaintext non-file command on a channel holder";
  clearWire();
  write(1, "status");
  EXPECT(onlyQueued(1, 9, "status"), "a plaintext non-file command stopped running in optional mode");
  // A verb that only starts like a file verb is not one.
  clearWire();
  write(1, "filesystem");
  EXPECT(onlyQueued(1, 9, "filesystem"), "a non-file verb sharing a prefix was refused");

  // B1: encryption optional, conn 1 holds a channel and writes plaintext commands
  // that the command tail answers itself. Each reply reaches conn 1 as one
  // encrypted DATA frame, and none waited on the secure-channel tx mutex on
  // BTC_TASK: a paced reply holds that mutex for up to ~0.65 s, and BTC_TASK
  // serves the GATT/GAP callbacks of every link.
  struct TailReply {
    const char* name;
    const char* line;
    const char* reply;
    bool requireAuth;
  };
  static const TailReply kTailReplies[] = {
      {"B1 whoami on a channel holder", "whoami", "You are AuthBypass", false},
      {"B1 login usage on a channel holder", "login onlyuser", "Usage: login <username> <password>", false},
      {"B1 logout on a channel holder", "logout", "[ble] Logged out.", false},
      {"B1 auth-required on a channel holder", "status",
       "Authentication required. Use: login <username> <password>", true},
  };
  resetWorld("B1 setup", false);
  establish(1);
  for (const TailReply& t : kTailReplies) {
    currentCase = t.name;
    clearWire();
    gSettings.bleRequireAuth = t.requireAuth;
    write(1, t.line);
    EXPECT(btcTxMutexWaits == 0, "a reply sent on BTC_TASK waited on the secure-channel tx mutex");
    EXPECT(btcSleeps == 0, "BTC_TASK slept while handling a plaintext write");
    EXPECT(inbox[1].size() == 1 && isDataFrameWith(inbox[1][0], t.reply),
           "the reply did not reach the channel holder as one encrypted DATA frame");
    EXPECT(inbox[0].empty(), "the reply for conn 1 reached conn 0");
    EXPECT(queued.empty(), "a command the tail answers itself reached the executor");
  }
  gSettings.bleRequireAuth = false;

  // B2: the same session's plaintext command still runs, for the session it arrived on.
  currentCase = "B2 plaintext command on a channel holder";
  clearWire();
  write(1, "status");
  EXPECT(onlyQueued(1, 9, "status"), "a plaintext command on a channel holder did not run");
  EXPECT(btcTxMutexWaits == 0 && btcSleeps == 0, "BTC_TASK waited while handling a plaintext write");

  // B3: gCmdExecQ full. The plaintext command does not run; the writer gets one
  // best-effort error that does not wait on the tx mutex on BTC_TASK, and the
  // pool slot comes back.
  resetWorld("B3 command queue full on a channel holder", false);
  establish(1);
  const int claimsBefore = poolClaims, releasesBefore = poolReleases;
  cmdExecQueueFull = true;
  write(1, "status");
  cmdExecQueueFull = false;
  EXPECT(queued.empty(), "a command ran although the command queue was full");
  EXPECT(btcTxMutexWaits == 0,
         "the queue-full error waited on the secure-channel tx mutex on BTC_TASK");
  EXPECT(inbox[1].size() == 1 && isDataFrameWith(inbox[1][0], "Error: Failed to queue command"),
         "the queue-full error did not reach the channel holder");
  EXPECT(inbox[0].empty(), "the queue-full error for conn 1 reached conn 0");
  EXPECT(poolClaims - claimsBefore == poolReleases - releasesBefore,
         "the pool slot of an unqueued write was not returned");

  // W: a plaintext login line crosses the inbound pool slot, which prefers PSRAM.
  // Every release of a plaintext slot wipes it, so no byte of the password stays
  // in the pool after the write is handled: run, refused by a full queue, or
  // dropped because the session changed before cmd_exec ran it.
  static const char kPassword[] = "Wipe-Me-pw";
  static const std::string kLogin = std::string("login alice ") + kPassword;
  const auto slotHoldsPassword = [] {
    const uint8_t* begin = sPoolSlot.buf;
    const uint8_t* end = begin + sizeof(sPoolSlot.buf);
    return std::search(begin, end, kPassword, kPassword + sizeof(kPassword) - 1) != end;
  };
  const auto startWipeCase = [](const char* name) {
    resetWorld(name, false);
    memset(&sPoolSlot, 0, sizeof(sPoolSlot));
    releasedWithResidue = false;
  };

  startWipeCase("W1 plaintext login slot wiped after the job runs");
  write(0, kLogin);
  EXPECT(!releasedWithResidue, "the plaintext login slot was released still holding the login line");
  EXPECT(!slotHoldsPassword(), "the cleartext password stayed in the inbound pool after the job ran");
  // A shorter later write reuses the slot; no tail of the password may remain.
  write(0, "whoami");
  EXPECT(!slotHoldsPassword(), "a shorter later write left the password tail in the inbound pool");

  startWipeCase("W2 plaintext login slot wiped when the queue is full");
  cmdExecQueueFull = true;
  write(0, kLogin);
  cmdExecQueueFull = false;
  EXPECT(!releasedWithResidue, "an unqueued plaintext login slot was released still holding the line");
  EXPECT(!slotHoldsPassword(), "the cleartext password stayed in the inbound pool after a queue-full drop");

  startWipeCase("W3 plaintext login slot wiped when the session changed first");
  onBtcTask = true;
  processIncomingBLECommand(0, kLogin.data(), kLogin.size());
  onBtcTask = false;
  sBleSessionEpoch[0].store(8);  // conn 0 reconnected before cmd_exec ran the job
  drainCmdExec();
  EXPECT(!releasedWithResidue, "a dropped plaintext login slot was released still holding the line");
  EXPECT(!slotHoldsPassword(), "the cleartext password stayed in the inbound pool after a stale-session drop");

  startWipeCase("W4 plaintext login slot wiped after a required-mode refusal");
  gSettings.bleRequireSecureChannel = true;
  write(0, kLogin);
  EXPECT(!releasedWithResidue, "a refused plaintext login slot was released still holding the line");
  EXPECT(!slotHoldsPassword(), "the cleartext password stayed in the inbound pool after a refusal");

  EXPECT(guardDepth == 0, "lifecycle guard leaked");
  EXPECT(poolClaims == poolReleases, "secure-frame pool slot not released exactly once per frame");

  if (failures) {
    fprintf(stderr, "BLE secure-channel policy: %d expectation(s) failed\n", failures);
    return 1;
  }
  puts("BLE secure-channel policy: required-mode refusals reach the writing client (plaintext without a channel, "
       "encrypted with one); file verbs run only when the command arrived encrypted; no reply to a plaintext "
       "write waits on the secure-channel tx mutex on BTC_TASK; a plaintext pool slot is wiped before release");
  return 0;
}
