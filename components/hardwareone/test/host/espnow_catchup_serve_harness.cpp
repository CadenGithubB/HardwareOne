// Drives the production file catch-up server (System_ESPNow_Catchup.cpp, the
// CATCHUP_SERVE_COMMON and CATCHUP_SERVE_HOST regions) end to end over the
// real ledger store (System_CatchupLedger.cpp, CATCHUP_LEDGER_HOST) and the
// shared fake VFS: frames go in through the receive handlers, the cmd_exec job
// runs from a queue, and catchupServeTick sends through a recording
// espnowtx::sendAead. ESP-NOW sessions, the battery, the capture key's sealing
// (a toy that keeps the call shape and binds the AAD), the clock and SHA-256 (a
// small real implementation) are doubles defined here.
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#define FAKE_VFS_LOCK_DOUBLE
#include <Arduino.h>
#include "fake_vfs.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "System_ESPNow_CatchupCore.h"

void webBatchTestStringAppend() {}

#include "catchup_ledger_platform.h"

// ---- the ledger (production) -----------------------------------------------
// INSERT_LEDGER_HEADER

// INSERT_LEDGER

// ---- doubles for the server ------------------------------------------------
static unsigned gChecks = 0;
#define CHECK(cond)                                                                             \
  do {                                                                                          \
    ++gChecks;                                                                                  \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } \
  } while (0)

struct esp_now_recv_info { const uint8_t* src_addr; };
#include "System_ESPNow_AnswerCore.h"
#include "System_ESPNow_Verdict.h"
// The handler's view of a frame, with the verdict helpers of System_ESPNow_RxCtx.h.
struct V4RxCtx {
  const esp_now_recv_info* recv_info; const uint8_t* payload; uint16_t payloadLen;
  V4Verdict accepted() const { return v4Accepted(); }
  V4Verdict refuseWith(uint8_t status) const { return v4RefusedStatus(status); }
};

using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
static int gMuxDepth = 0;
static void muxEnter(portMUX_TYPE*) { CHECK(gMuxDepth == 0); ++gMuxDepth; }
static void muxExit(portMUX_TYPE*) { CHECK(gMuxDepth == 1); --gMuxDepth; }
#define portENTER_CRITICAL(m) muxEnter(m)
#define portEXIT_CRITICAL(m) muxExit(m)
#define ENABLE_BONDED_MODE 0
#define DEBUG_ESPNOWF(...) ((void)0)
#define MAC_FMT "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC_ARGS(m) (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]

static uint32_t millis() { return gNow; }
static unsigned gTaskDelays = 0;
static void vTaskDelay(int) { ++gTaskDelays; CHECK(!gFakeFsSemaphore.held); }

struct Settings { bool catchupServe = true; };
static Settings gSettings;

struct Sent { uint8_t mac[6]; uint8_t type; uint16_t flags; uint32_t msgId; uint8_t ttl; std::vector<uint8_t> bytes; };
static std::vector<Sent> gSent;
static uint8_t gDepth = 0;
static bool gSendFails = false;
static uint32_t gMsgId = 100;
namespace espnowtx {
static bool sendAead(const uint8_t mac[6], uint8_t type, uint16_t flags, uint32_t msgId, const uint8_t* payload,
                     uint16_t len, uint8_t ttl) {
  CHECK(gMuxDepth == 0);
  if (gSendFails) return false;
  Sent s;
  std::memcpy(s.mac, mac, 6);
  s.type = type; s.flags = flags; s.msgId = msgId; s.ttl = ttl;
  s.bytes.assign(payload, payload + len);
  gSent.push_back(s);
  return true;
}
static uint8_t queueDepth() { return gDepth; }
}  // namespace espnowtx
static uint32_t generateMessageId() { return ++gMsgId; }

static int gNamed = 1;
static bool gCapsKnown = true, gCapsConfirmed = true, gSettled = true, gUp = true, gSubmitFail = false;
static uint32_t gCaps = espnowcatchup::kCapCollect;
static uint16_t gSession = 7, gPathCap = 202;
static int espnowNamedMasterKind(const uint8_t*) { return gNamed; }
static bool espnowPeerSessionCaps(const uint8_t*, uint32_t* caps, bool* confirmed) {
  if (!gCapsKnown) return false;
  *caps = gCaps;
  if (confirmed) *confirmed = gCapsConfirmed;
  return true;
}
static uint16_t espnowSessionIdOf(const uint8_t*) { return gSession; }
static bool espnowSessionSettled(const uint8_t*, uint32_t) { return gSettled; }
static bool espnowSessionUp(const uint8_t*, uint32_t* = nullptr) { return gUp; }
static uint16_t v4_single_frame_cap(const uint8_t*) { return gPathCap; }
static uint16_t espnowSensorBootTag() { return 0x1234; }

struct BatteryState { float percentage = NAN; bool percentageValid = false, chargingKnown = false, stale = false, isCharging = false; };
static BatteryState gBattery;
static BatteryState getBatterySnapshot() { return gBattery; }

static std::deque<void (*)(void*)> gJobs;
static bool submitDeferredToCmdExec(void (*fn)(void*), void*) {
  if (gSubmitFail) return false;
  gJobs.push_back(fn);
  return true;
}

// Toy sealing with the production call shape: nonce12 (a counter) || in ^ 0x5A
// ^ aad || 16 x 0xEE. A test opens it with the AAD it expects, so a wrong
// AAD (ledger, file, offset, length, kind) shows as wrong bytes.
static bool gSealFail = false;
static uint8_t gNonce = 0;
static int captureCryptoSealChunk(const uint8_t* aad, size_t aadLen, const uint8_t* in, size_t inLen, uint8_t* out,
                                  size_t outCap) {
  if (gSealFail || outCap < inLen + 28 || !aadLen) return -1;
  for (int i = 0; i < 12; ++i) out[i] = ++gNonce;
  for (size_t i = 0; i < inLen; ++i) out[12 + i] = uint8_t(in[i] ^ 0x5A ^ aad[i % aadLen]);
  std::memset(out + 12 + inLen, 0xEE, 16);
  return int(inLen + 28);
}
static std::string toyOpen(const uint8_t* data, size_t n, const uint8_t* aad, size_t aadLen) {
  std::string out;
  if (n < 28) return "<short>";
  for (size_t i = 0; i < n - 28; ++i) out.push_back(char(data[12 + i] ^ 0x5A ^ aad[i % aadLen]));
  return out;
}

namespace Clock { static bool isValidEpoch(time_t t) { return t > 1600000000; } }

// SHA-256 (FIPS 180-4), small and plain, behind the mbedtls names the server uses.
struct mbedtls_sha256_context { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t used; };
static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void shaBlock(mbedtls_sha256_context* c, const uint8_t* p) {
  static const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,
    0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,
    0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
    const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
    h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
static void mbedtls_sha256_init(mbedtls_sha256_context* c) { std::memset(c, 0, sizeof(*c)); }
static void mbedtls_sha256_free(mbedtls_sha256_context*) {}
static int mbedtls_sha256_starts(mbedtls_sha256_context* c, int is224) {
  static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  if (is224) return -1;
  std::memcpy(c->h, iv, sizeof(iv)); c->len = 0; c->used = 0;
  return 0;
}
static int mbedtls_sha256_update(mbedtls_sha256_context* c, const uint8_t* p, size_t n) {
  c->len += n;
  while (n--) { c->buf[c->used++] = *p++; if (c->used == 64) { shaBlock(c, c->buf); c->used = 0; } }
  return 0;
}
static int mbedtls_sha256_finish(mbedtls_sha256_context* c, uint8_t out[32]) {
  const uint64_t bits = c->len * 8;
  uint8_t pad = 0x80;
  mbedtls_sha256_update(c, &pad, 1);
  pad = 0;
  while (c->used != 56) mbedtls_sha256_update(c, &pad, 1);
  uint8_t l[8];
  for (int i = 0; i < 8; ++i) l[i] = uint8_t(bits >> (56 - 8 * i));
  for (int i = 0; i < 8; ++i) { c->buf[c->used++] = l[i]; }
  shaBlock(c, c->buf);
  for (int i = 0; i < 8; ++i) { out[4*i] = uint8_t(c->h[i] >> 24); out[4*i+1] = uint8_t(c->h[i] >> 16); out[4*i+2] = uint8_t(c->h[i] >> 8); out[4*i+3] = uint8_t(c->h[i]); }
  return 0;
}
static std::string sha256(const std::string& s) {
  mbedtls_sha256_context c; mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c, 0);
  mbedtls_sha256_update(&c, reinterpret_cast<const uint8_t*>(s.data()), s.size());
  uint8_t out[32]; mbedtls_sha256_finish(&c, out);
  return std::string(reinterpret_cast<const char*>(out), 32);
}

// ---- the server (production) -------------------------------------------------
// INSERT_SERVE

// ---- cases --------------------------------------------------------------------
namespace ec = espnowcatchup;
static const uint8_t kPeer[6] = {0x8c, 0xbf, 0xea, 0x00, 0xa7, 0x73};

static void resetAll() {
  for (void* p : gBlocks) std::free(p);
  gBlocks.clear();
  std::memset(&sLedger, 0, sizeof(sLedger));
  std::memset(&sRt, 0, sizeof(sRt));
  std::memset(sNotes, 0, sizeof(sNotes));
  std::memset(&sSlot, 0, sizeof(sSlot));
  std::memset(&sPacer, 0, sizeof(sPacer));
  std::memset(&sStats, 0, sizeof(sStats));
  auto& fs = fake_vfs::fs();
  fs.reset();
  fake_vfs::wireLockDouble();
  fs.sdAvailable() = true;
  fs.mkdirAll("/system");
  gNow = 100000; gBoot = 41; gSdGen = 1; gRandom = 0x1a2b3c4d; gKeyReady = true; gAllocFail = false;
  gSettings.catchupServe = true;
  gSent.clear(); gJobs.clear();
  gDepth = 0; gSendFails = false; gNamed = 1; gCapsKnown = true; gCapsConfirmed = true; gCaps = ec::kCapCollect;
  gSettled = true; gUp = true; gSubmitFail = false; gSession = 7; gPathCap = 202; gSealFail = false;
  gBattery = BatteryState{};
  gTaskDelays = 0;
}

static void deliver(uint8_t op, const uint8_t* p, size_t n) {
  esp_now_recv_info info{kPeer};
  V4RxCtx ctx{&info, p, uint16_t(n)};
  if (op == ec::kOpListReq) v4hCatchupListReq(ctx);
  else v4hCatchupReadReq(ctx);
  CHECK(gMuxDepth == 0);
}
static void listReq(uint32_t reqId, uint32_t from, uint8_t maxFrame = 0, uint16_t mask = 0, uint8_t link = 0) {
  ec::ListReq r = {};
  r.reqId = reqId; r.fromFileId = from; r.kindMask = mask; r.link = link; r.maxFrame = maxFrame;
  uint8_t b[16];
  CHECK(ec::packListReq(r, b, sizeof(b)) == 16);
  deliver(ec::kOpListReq, b, sizeof(b));
}
static void readReq(uint32_t reqId, uint32_t fileId, uint32_t offset, uint32_t maxBytes = 0, uint8_t mode = 0,
                    uint8_t flags = 0, uint32_t ledgerId = 0, uint8_t maxFrame = 0) {
  ec::ReadReq r = {};
  r.reqId = reqId; r.fileId = fileId; r.offset = offset; r.maxBytes = maxBytes; r.mode = mode; r.flags = flags;
  r.maxFrame = maxFrame;
  r.ledgerId = ledgerId ? ledgerId : sLedger.ledgerId;
  uint8_t b[24];
  CHECK(ec::packReadReq(r, b, sizeof(b)) == 24);
  deliver(ec::kOpReadReq, b, sizeof(b));
}
// Runs the queued cmd_exec job, then ticks every 10 ms until the window is out.
static void pump(unsigned maxTicks = 200) {
  while (!gJobs.empty()) { auto fn = gJobs.front(); gJobs.pop_front(); fn(nullptr); }
  for (unsigned i = 0; i < maxTicks && sSlot.state != SlotState::Free; ++i) {
    catchupServeTick(gNow);
    gNow += 10;
  }
}
// The ledger's ids need a reconcile: the first request after boot does it.
static void boot() {
  listReq(1, 0);
  pump();
  gSent.clear();
  gNow += ec::kFullListMinMs + 1000;   // the next full LIST is not paced
}
static uint32_t idOf(const char* path) { const ec::LedgerEntry* e = ec::findByPath(sLedger, path); return e ? e->fileId : 0; }

static void sentConvention() {
  for (const Sent& s : gSent) {
    CHECK(std::memcmp(s.mac, kPeer, 6) == 0 && s.flags == 0 && s.ttl == 1);
    CHECK(s.type == ec::kOpListResp || s.type == ec::kOpReadResp);
    CHECK(s.bytes.size() <= gPathCap);
  }
  for (size_t i = 1; i < gSent.size(); ++i) CHECK(gSent[i].msgId != gSent[i - 1].msgId);
}

static void refusals() {
  resetAll();
  gNamed = 0;   // not a named master: refused once per session, then silent
  listReq(5, 0);
  CHECK(gSent.size() == 1 && gSent[0].type == ec::kOpListResp);
  ec::ListRespHeader h;
  CHECK(ec::parseListRespHeader(gSent[0].bytes.data(), gSent[0].bytes.size(), &h));
  CHECK(h.reqId == 5 && h.status == ec::kStRefused && h.entryCount == 0);
  listReq(6, 0);
  readReq(7, 1, 0);
  CHECK(gSent.size() == 1 && sStats.refused == 1 && sStats.refusedSilent == 2);
  gSession = 8;   // a new session is answered again
  readReq(8, 1, 0);
  CHECK(gSent.size() == 2 && gSent[1].type == ec::kOpReadResp && gSent[1].bytes[4] == ec::kStRefused);
  // A backup is served exactly as the primary (Q9).
  resetAll();
  gNamed = 2;
  listReq(9, 0);
  pump();
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStOk);
  // Setting off, link != 0, a confirmed peer without collect.
  resetAll();
  gSettings.catchupServe = false;
  listReq(10, 0);
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStRefused);
  resetAll();
  listReq(11, 0, 0, 0, 3);
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStNoLink && gSent[0].bytes[5] == 3);
  resetAll();
  gCaps = 0;
  listReq(12, 0);
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStRefused);
  resetAll();
  gCaps = 0; gCapsConfirmed = false;   // unconfirmed caps gate nothing
  listReq(13, 0);
  pump();
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStOk);
  // reqId 0 is dropped, counted, never answered.
  resetAll();
  listReq(0, 0);
  CHECK(gSent.empty() && sStats.badRequests == 1);
  sentConvention();
}

static void busyAnswers() {
  resetAll();
  gSettled = false;
  listReq(20, 0);
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStBusy);
  CHECK(ec::busyRetryAfter(gSent[0].bytes.data(), gSent[0].bytes.size(), 24) == 2);
  gSettled = true;
  // Battery floor: only measured, discharging and below 20 %.
  gBattery.percentageValid = true; gBattery.chargingKnown = true; gBattery.percentage = 15; gBattery.isCharging = false;
  listReq(21, 0);
  CHECK(gSent.size() == 2 && ec::busyRetryAfter(gSent[1].bytes.data(), gSent[1].bytes.size(), 24) == 600);
  gBattery.percentageValid = false;   // the P4X-EYE: no reading, no floor
  listReq(22, 0);
  CHECK(gSent.size() == 2 && sSlot.state == SlotState::Queued);
  // The slot is taken: the next request is busy 1 s.
  readReq(23, 1, 0);
  CHECK(gSent.size() == 3 && gSent[2].type == ec::kOpReadResp && gSent[2].bytes[4] == ec::kStBusy &&
        ec::busyRetryAfter(gSent[2].bytes.data(), gSent[2].bytes.size(), 20) == 1);
  pump();
  // A full LIST again within 60 s of this peer's last: busy for the rest.
  gSent.clear();
  gNow += 5000;
  listReq(24, 0);
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStBusy &&
        ec::busyRetryAfter(gSent[0].bytes.data(), gSent[0].bytes.size(), 24) >= 50);
  // The TX queue is deep: busy, live traffic first.
  gSent.clear();
  gDepth = 16;
  readReq(25, 1, 0);
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStBusy);
  gDepth = 0;
  // cmd_exec queue full: busy, the slot is free again.
  gSent.clear();
  gSubmitFail = true;
  readReq(26, 1, 0);
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStBusy && sSlot.state == SlotState::Free);
  sentConvention();
}

static std::string events(size_t n) {
  std::string s;
  for (size_t i = 0; s.size() < n; ++i) s += "ev " + std::to_string(i) + " xxxxxxxxxxxxxxxxxxxxxxxxxxxxx\r\n";
  s.resize(n);
  return s;
}

static void listPages() {
  resetAll();
  auto& fs = fake_vfs::fs();
  fs.put("/system/sys_logs/events.log", events(1000));
  fs.put("/sd/logging_captures/system/system.log", "boot line\n");
  fs.put("/logging_captures/sensors/2026-11-19/health-2026-11-19.csv", "time,r1_hr\n1,60\n");
  fs.put("/stt/u3/20261119T120000Z-citrinet-0011223344556677.txt", "words\n");
  listReq(30, 0);
  pump();
  CHECK(gSent.size() == 1);
  ec::ListRespHeader h;
  const std::vector<uint8_t>& b = gSent[0].bytes;
  CHECK(ec::parseListRespHeader(b.data(), b.size(), &h));
  CHECK(h.status == ec::kStOk && h.reqId == 30 && h.ledgerId == sLedger.ledgerId && h.bootTag == 0x1234);
  CHECK(h.entryCount == 3 && h.nextFileId == 0);
  CHECK(h.flags & ec::kLfWithheld);      // the unsealed health file
  CHECK(h.flags & ec::kLfAnySealed);     // the system log and the transcript go sealed
  ec::EntryCursor c = ec::entries(b.data(), b.size(), h);
  ec::Entry e;
  unsigned sealedOnSend = 0;
  while (c.next(&e)) {
    const std::string name(e.name, e.nameLen);
    CHECK(name.find("health") == std::string::npos);
    if (e.flags & ec::kEfSealedOnSend) ++sealedOnSend;
    if (name == "events.log")
      CHECK(e.start == 0 && e.end == 1000 && e.kind == uint8_t(ec::Kind::EventsLog) && (e.flags & ec::kEfSealedOnSend));
  }
  CHECK(sealedOnSend == 3);   // the system log, the transcript and events.log
  // Small frames: one entry a page, chained by nextFileId.
  std::vector<uint32_t> seen;
  uint32_t from = 1;
  for (int page = 0; page < 10 && from; ++page) {
    gSent.clear();
    gNow += 60;
    listReq(31 + page, from, 64);
    pump();
    CHECK(gSent.size() == 1 && gSent[0].bytes.size() <= 64);
    CHECK(ec::parseListRespHeader(gSent[0].bytes.data(), gSent[0].bytes.size(), &h));
    CHECK(h.entryCount == 1);
    ec::EntryCursor pc = ec::entries(gSent[0].bytes.data(), gSent[0].bytes.size(), h);
    CHECK(pc.next(&e));
    seen.push_back(e.fileId);
    from = h.nextFileId;
  }
  CHECK(seen.size() == 3 && seen[0] < seen[1] && seen[1] < seen[2]);
  // A kind mask lists only that kind.
  gSent.clear();
  gNow += 60;
  listReq(40, 1, 0, ec::kindBit(ec::Kind::EventsLog));
  pump();
  CHECK(ec::parseListRespHeader(gSent[0].bytes.data(), gSent[0].bytes.size(), &h) && h.entryCount == 1);
  sentConvention();
}

// The frames of the last window, in order, checked for contiguity; returns the data.
static std::string window(uint32_t reqId, uint32_t fileId, uint32_t offset, uint32_t end, size_t frames,
                          bool sealed, uint8_t kind) {
  CHECK(gSent.size() == frames);
  std::string data;
  uint32_t at = offset;
  for (size_t i = 0; i < frames; ++i) {
    ec::ReadRespHeader h;
    const uint8_t* d = nullptr;
    size_t n = 0;
    CHECK(ec::parseReadResp(gSent[i].bytes.data(), gSent[i].bytes.size(), &h, &d, &n));
    CHECK(h.reqId == reqId && h.status == ec::kStOk && h.fileId == fileId && h.offset == at && h.end == end);
    CHECK(bool(h.flags & ec::kRfLast) == (i + 1 == frames));
    if (sealed && n) {
      CHECK(h.flags & ec::kRfSealedOnSend);
      const uint16_t p = ec::sealedPlainLen(n);
      uint8_t aad[ec::kAadLen];
      ec::buildSealAad(aad, sLedger.ledgerId, fileId, h.offset, p, kind);
      data += toyOpen(d, n, aad, sizeof(aad));
      at += p;
    } else {
      CHECK(!(h.flags & ec::kRfSealedOnSend));
      data.append(reinterpret_cast<const char*>(d), n);
      at += uint32_t(n);
    }
    CHECK(bool(h.flags & ec::kRfAtEnd) == (at == end));
  }
  return data;
}

static void reads() {
  // errors.log: a plain capped log (events.log is sealed on send since the
  // security review of 2026-11-19: it holds logins and remote command lines).
  resetAll();
  auto& fs = fake_vfs::fs();
  const std::string ev = events(1000);
  fs.put("/system/sys_logs/errors.log", ev);
  boot();
  const uint32_t id = idOf("/system/sys_logs/errors.log");
  // Plain, direct: 182 per frame, 6 frames for 1000 bytes, 20 ms apart.
  readReq(50, id, 0);
  while (!gJobs.empty()) { auto fn = gJobs.front(); gJobs.pop_front(); fn(nullptr); }
  CHECK(sSlot.state == SlotState::Ready && sSlot.nFrames == 6);
  catchupServeTick(gNow);
  CHECK(gSent.size() == 1);
  catchupServeTick(gNow + 10);
  CHECK(gSent.size() == 1);                       // spacing
  gDepth = 16;
  catchupServeTick(gNow + 20);
  CHECK(gSent.size() == 1);                       // the TX queue is deep: wait
  gDepth = 0;
  gNow += 20;
  pump();
  CHECK(window(50, id, 0, 1000, 6, false, 0) == ev);
  CHECK(gSent[0].bytes.size() == 202 && sStats.lastOp == ec::kOpReadReq && sStats.lastFrames == 6 && sStats.lastBytes == 1000);
  // maxBytes and a routed path (146 B: 126 per frame).
  gSent.clear();
  gPathCap = 146;
  readReq(51, id, 100, 300);
  pump();
  CHECK(window(51, id, 100, 1000, 3, false, 0) == ev.substr(100, 300));
  gPathCap = 202;
  // At the end: one empty frame, last and atEnd.
  gSent.clear();
  readReq(52, id, 1000);
  pump();
  CHECK(window(52, id, 1000, 1000, 1, false, 0).empty());
  // Growth after the LIST: the READ serves to the new end.
  gSent.clear();
  fs.put("/system/sys_logs/errors.log", ev + "more lines\r\n");
  readReq(53, id, 1000);
  pump();
  CHECK(window(53, id, 1000, 1012, 1, false, 0) == "more lines\r\n");
  CHECK(ec::logicalEnd(*ec::findById(sLedger, id)) == 1012);
  // Statuses: past end, unknown id, another ledger, trimmed.
  gSent.clear();
  readReq(54, id, 5000);
  pump();
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStPastEnd);
  gSent.clear();
  readReq(55, 9999, 0);
  pump();
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStGone);
  gSent.clear();
  readReq(56, id, 0, 0, 0, 0, sLedger.ledgerId ^ 1);
  pump();
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStGone);
  gSent.clear();
  fs.put("/system/sys_logs/errors.log", (ev + "more lines\r\n").substr(400));
  catchupNoteTrim("/system/sys_logs/errors.log", 400);
  readReq(57, id, 0);
  pump();
  ec::ReadRespHeader rh;
  CHECK(gSent.size() == 1 && ec::parseReadResp(gSent[0].bytes.data(), gSent[0].bytes.size(), &rh, nullptr, nullptr));
  CHECK(rh.status == ec::kStTrimmed && rh.offset == 400 && rh.end == 1012);
  // Q8: a delivery claim raises `delivered`, never lowers it.
  gSent.clear();
  readReq(58, id, 900, 10, 0, ec::kRqStoredBefore);
  pump();
  CHECK(ec::findById(sLedger, id)->delivered == 900);
  readReq(59, id, 500, 10, 0, ec::kRqStoredBefore);
  pump();
  CHECK(ec::findById(sLedger, id)->delivered == 900);
  sentConvention();
}

static void sealedReads() {
  resetAll();
  auto& fs = fake_vfs::fs();
  std::string log;
  for (int i = 0; i < 40; ++i) log += "12:00:0" + std::to_string(i % 10) + " cmd: wifiadd net" + std::to_string(i) + "\n";
  fs.put("/sd/logging_captures/system/system.log", log);
  boot();
  const uint32_t id = idOf("/sd/logging_captures/system/system.log");
  const uint8_t kind = uint8_t(ec::Kind::SystemLog);
  // 154 plaintext bytes per direct frame when sealed.
  readReq(60, id, 0, 400);
  pump();
  CHECK(gSent.size() == 3 && gSent[0].bytes.size() == 202);
  CHECK(window(60, id, 0, uint32_t(log.size()), 3, true, kind) == log.substr(0, 400));
  for (const Sent& s : gSent) CHECK(std::string(s.bytes.begin(), s.bytes.end()).find("wifiadd") == std::string::npos);
  // A digest of a sealed-on-send kind is unsupported.
  gSent.clear();
  readReq(61, id, 0, 0, ec::kModeDigest);
  pump();
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStUnsupported);
  // Sealing fails: one not_ready frame, never the plaintext.
  gSent.clear();
  gSealFail = true;
  readReq(62, id, 0);
  pump();
  CHECK(gSent.size() == 1 && gSent[0].bytes.size() == 20 && gSent[0].bytes[4] == ec::kStNotReady);
  CHECK(sStats.sealFailures == 1);
  gSealFail = false;
  // No capture key: the file is not listed and a READ answers not_ready.
  gKeyReady = false;
  gSent.clear();
  readReq(63, id, 0);
  pump();
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStNotReady);
  gKeyReady = true;
  // Rotated between the LIST and the READ: the id follows the file to .1.
  fs.put("/sd/logging_captures/system/system.log.1", log);
  fs.put("/sd/logging_captures/system/system.log", "fresh\n");
  gSent.clear();
  readReq(64, id, 100, 50);
  pump();
  CHECK(window(64, id, 100, uint32_t(log.size()), 1, true, kind) == log.substr(100, 50));
  CHECK(idOf("/sd/logging_captures/system/system.log.1") == id);
  sentConvention();
}

static void digests() {
  resetAll();
  auto& fs = fake_vfs::fs();
  CHECK(sha256("abc") == std::string("\xba\x78\x16\xbf\x8f\x01\xcf\xea\x41\x41\x40\xde\x5d\xae\x22\x23"
                                     "\xb0\x03\x61\xa3\x96\x17\x7a\x9c\xb4\x10\xff\x61\xf2\x00\x15\xad", 32));
  const std::string ev = events(10000);
  fs.put("/system/sys_logs/errors.log", ev);
  boot();
  const uint32_t id = idOf("/system/sys_logs/errors.log");
  readReq(70, id, 1000, 5000, ec::kModeDigest);
  pump();
  CHECK(gSent.size() == 1);
  ec::ReadRespHeader h;
  const uint8_t* d = nullptr;
  size_t n = 0;
  CHECK(ec::parseReadResp(gSent[0].bytes.data(), gSent[0].bytes.size(), &h, &d, &n));
  CHECK(h.status == ec::kStOk && (h.flags & ec::kRfDigest) && (h.flags & ec::kRfLast) && !(h.flags & ec::kRfAtEnd));
  CHECK(h.offset == 1000 && h.end == 10000 && n == ec::kDigestLen);
  CHECK(std::string(reinterpret_cast<const char*>(d), n) == sha256(ev.substr(1000, 5000)).substr(0, 16));
  CHECK(gTaskDelays >= 2);   // the lock is released between 4 KB blocks
  // Once per file per session.
  gSent.clear();
  readReq(71, id, 0, 0, ec::kModeDigest);
  pump();
  CHECK(gSent.size() == 1 && gSent[0].bytes[4] == ec::kStBusy &&
        ec::busyRetryAfter(gSent[0].bytes.data(), gSent[0].bytes.size(), 20) == 3600);
  gSession = 9;
  gSent.clear();
  readReq(72, id, 0, 0, ec::kModeDigest);
  pump();
  CHECK(ec::parseReadResp(gSent[0].bytes.data(), gSent[0].bytes.size(), &h, &d, &n) && (h.flags & ec::kRfAtEnd));
  CHECK(std::string(reinterpret_cast<const char*>(d), n) == sha256(ev).substr(0, 16));
  sentConvention();
}

static void sendingFailures() {
  resetAll();
  auto& fs = fake_vfs::fs();
  fs.put("/system/sys_logs/errors.log", events(1000));
  boot();
  const uint32_t id = idOf("/system/sys_logs/errors.log");
  gSendFails = true;
  readReq(80, id, 0);
  pump(100);
  CHECK(sSlot.state == SlotState::Free && sStats.abandoned == 1 && gSent.empty());
  gSendFails = false;
  // The session goes away mid-window: the slot is freed.
  readReq(81, id, 0);
  while (!gJobs.empty()) { auto fn = gJobs.front(); gJobs.pop_front(); fn(nullptr); }
  catchupServeTick(gNow);
  CHECK(gSent.size() == 1);
  gUp = false;
  gNow += 20;
  catchupServeTick(gNow);
  CHECK(sSlot.state == SlotState::Free && sStats.abandoned == 2);
  gUp = true;
  // Another requester while the lease is held: busy 10 s; it ends after 10 s idle.
  gSent.clear();
  readReq(82, id, 0);
  pump();
  const size_t before = gSent.size();
  uint8_t other[6] = {1, 2, 3, 4, 5, 6};
  ec::ReadReq r = {};
  r.reqId = 83; r.fileId = id; r.ledgerId = sLedger.ledgerId;
  uint8_t b[24];
  ec::packReadReq(r, b, sizeof(b));
  esp_now_recv_info info{other};
  V4RxCtx ctx{&info, b, 24};
  v4hCatchupReadReq(ctx);
  CHECK(gSent.size() == before + 1 && gSent.back().bytes[4] == ec::kStBusy &&
        ec::busyRetryAfter(gSent.back().bytes.data(), gSent.back().bytes.size(), 20) == 10);
  gNow += 10001;
  catchupServeTick(gNow);
  v4hCatchupReadReq(ctx);
  CHECK(sSlot.state == SlotState::Queued);
  pump();
  // A job cmd_exec lost: the slot frees itself after 30 s; the late job is a no-op.
  gNow += 10001;   // the other requester's lease runs out
  catchupServeTick(gNow);
  readReq(84, id, 0);
  CHECK(sSlot.state == SlotState::Queued && gJobs.size() == 1);
  auto lost = gJobs.front();
  gJobs.clear();
  const uint32_t abandoned = sStats.abandoned;
  catchupServeTick(gNow + 29000);
  CHECK(sSlot.state == SlotState::Queued);
  catchupServeTick(gNow + 30000);
  CHECK(sSlot.state == SlotState::Free && sStats.abandoned == abandoned + 1);
  const size_t sent = gSent.size();
  lost(nullptr);
  CHECK(sSlot.state == SlotState::Free && gSent.size() == sent);
}

// ---- Both sides: the real server and ledger produce the frames; the
// collector's own core lists, plans and reassembles them (tests review
// 2026-11-19). Every byte comes back, sealed frames open with the AAD the
// on-card record rebuilds, and the Q8 claims leave nothing for a second master.
#include "System_CollectCore.h"
namespace cc = collectcore;
static cc::DeviceFiles gXD;
static cc::Listing gXL;

// One complete listing from id 0, paged at `listMaxFrame`, applied to `d`.
static void crossList(cc::DeviceFiles& d, uint8_t listMaxFrame, uint32_t* req) {
  cc::listingReset(gXL);
  uint32_t from = 0;
  for (int page = 0; page < 64; ++page) {
    gSent.clear();
    listReq(++*req, from, listMaxFrame);
    pump();
    CHECK(gSent.size() == 1);
    if (gSent.empty()) return;
    ec::ListRespHeader h;
    CHECK(ec::parseListRespHeader(gSent[0].bytes.data(), gSent[0].bytes.size(), &h));
    CHECK(h.status == ec::kStOk && h.reqId == *req);
    CHECK(cc::listingPage(gXL, h));
    ec::EntryCursor c = ec::entries(gSent[0].bytes.data(), gSent[0].bytes.size(), h);
    ec::Entry e;
    uint8_t got = 0;
    while (c.next(&e)) { CHECK(cc::listingAdd(gXL, e)); ++got; }
    CHECK(got == h.entryCount);
    gNow += ec::kPageMinMs + 10;
    if (h.nextFileId == 0) { gXL.complete = true; break; }
    CHECK(h.nextFileId > from);
    from = h.nextFileId;
  }
  CHECK(gXL.complete);
  cc::planFromList(d, gXL, false, 1763575000u);
}

// Pulls everything nextRead plans; returns the plaintext per file id.
static std::map<uint32_t, std::string> crossPull(cc::DeviceFiles& d, uint16_t pathCap, uint32_t* req) {
  std::map<uint32_t, std::string> got;
  for (int it = 0; it < 500; ++it) {
    const cc::ReadPlan r = cc::nextRead(d, pathCap, true, false);
    if (!r.any || r.mode != ec::kModeData) break;
    cc::FileState& f = d.f[r.fileIndex];
    if (r.claimOnly) f.lastSeenDelivered = r.offset;
    gSent.clear();
    readReq(++*req, r.fileId, r.offset, r.maxBytes, ec::kModeData, r.flags, d.ledgerId);
    pump();
    cc::WindowAssembler w;
    w.begin(*req, r.fileId, r.offset, cc::isSealOnSend(f), gNow, pathCap < ec::kDirectPayload);
    for (const Sent& s : gSent) {
      ec::ReadRespHeader h;
      const uint8_t* dd = nullptr;
      size_t n = 0;
      CHECK(ec::parseReadResp(s.bytes.data(), s.bytes.size(), &h, &dd, &n));
      CHECK(h.status == ec::kStOk);
      CHECK(w.onFrame(h, dd, n, gNow) == cc::WindowAssembler::Took::Took);
    }
    CHECK(w.done && !w.mismatch && !w.gapSeen);
    if (!w.done) break;
    if (r.claimOnly) { CHECK(w.plainBytes() == 0 && w.atEnd); continue; }
    CHECK(w.plainBytes() > 0);
    if (w.plainBytes() == 0) break;
    for (uint8_t k = 0; k < w.frames; ++k) {
      const cc::AsmFrame& a = w.f[k];
      if (cc::isSealOnSend(f) && a.dataLen) {
        const ec::SealedRecord rec = ec::sealedRecordFor(f.kind, d.ledgerId, f.fileId, a.offset, a.plainLen);
        uint8_t aad[ec::kAadLen];
        ec::buildSealAad(aad, rec.ledgerId, rec.fileId, rec.offset, rec.plainLen, rec.kind);
        got[f.fileId] += toyOpen(a.data, a.dataLen, aad, sizeof(aad));
      } else {
        got[f.fileId].append(reinterpret_cast<const char*>(a.data), a.dataLen);
      }
    }
    if (w.end > f.lastSeenEnd) f.lastSeenEnd = w.end;
    CHECK(cc::commitWrite(f, cc::segmentFor(f, r.offset), r.offset, w.plainBytes()));
  }
  return got;
}

static void crossRun(uint8_t listMaxFrame, uint16_t pathCap) {
  resetAll();
  gPathCap = pathCap;
  auto& fs = fake_vfs::fs();
  const std::string ev = events(3000);
  std::string log;
  for (int i = 0; i < 60; ++i) log += "12:00:0" + std::to_string(i % 10) + " cmd: wifiadd net" + std::to_string(i) + "\n";
  const std::string er = events(700);
  const std::string tr = "hello transcript words\nsecond line\n";
  fs.put("/system/sys_logs/events.log", ev);
  fs.put("/system/sys_logs/errors.log", er);
  fs.put("/sd/logging_captures/system/system.log", log);
  fs.put("/stt/u3/20261119T120000Z-citrinet-0011223344556677.txt", tr);
  boot();
  // Master A.
  std::memset(&gXD, 0, sizeof(gXD));
  std::memcpy(gXD.mac, kPeer, 6);
  uint32_t req = 1000;
  crossList(gXD, listMaxFrame, &req);
  CHECK(gXD.count == 4 && gXD.ledgerId == sLedger.ledgerId);
  std::map<uint32_t, std::string> got = crossPull(gXD, pathCap, &req);
  const uint32_t evId = idOf("/system/sys_logs/events.log");
  const uint32_t erId = idOf("/system/sys_logs/errors.log");
  const uint32_t slId = idOf("/sd/logging_captures/system/system.log");
  const uint32_t trId = idOf("/stt/u3/20261119T120000Z-citrinet-0011223344556677.txt");
  CHECK(got[evId] == ev && got[erId] == er && got[slId] == log && got[trId] == tr);
  for (uint8_t i = 0; i < gXD.count; ++i) CHECK(gXD.f[i].pos == gXD.f[i].lastSeenEnd);
  // Q8: every file, the last window and the one-window transcript included,
  // counts as delivered on the device.
  for (uint32_t id : {evId, erId, slId, trId}) {
    const ec::LedgerEntry* e = ec::findById(sLedger, id);
    CHECK(e && e->delivered == ec::logicalEnd(*e));
  }
  // Master B (no index for this device): it lists, records everything as at
  // the other master, and fetches nothing again.
  cc::DeviceFiles& b = gXD;
  std::memset(&b, 0, sizeof(b));
  std::memcpy(b.mac, kPeer, 6);
  gNow += ec::kFullListMinMs + 1000;
  crossList(b, listMaxFrame, &req);
  uint32_t elsewhere = 0;
  for (uint8_t i = 0; i < b.count; ++i) elsewhere += b.f[i].elsewhereBytes;
  CHECK(elsewhere == uint32_t(ev.size() + er.size() + log.size() + tr.size()));
  CHECK(crossPull(b, pathCap, &req).empty());
  sentConvention();
}
static void crossSide() { crossRun(0, 202); crossRun(64, 202); crossRun(0, 146); crossRun(100, 146); }

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  struct Case { const char* name; void (*fn)(); } cases[] = {
    {"refusals", refusals}, {"busy", busyAnswers}, {"list_pages", listPages}, {"reads", reads},
    {"sealed_reads", sealedReads}, {"digests", digests}, {"sending_failures", sendingFailures},
    {"cross_side", crossSide},
  };
  unsigned ran = 0;
  for (const Case& c : cases) {
    if (!only.empty() && only != c.name) continue;
    c.fn();
    ++ran;
    std::printf("catchup_serve %s: ok\n", c.name);
  }
  for (void* p : gBlocks) std::free(p);
  std::printf("catchup serve: %u cases, %u checks passed\n", ran, gChecks);
  return ran ? 0 : 1;
}
