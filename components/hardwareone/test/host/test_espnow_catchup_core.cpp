// Host tests of System_ESPNow_CatchupCore.h, the production rules of file
// catch-up (CATCHUP_*, opcodes 82-85) that the device and the collector share:
//   * the four frames byte by byte (golden bytes), minLen - 1 refused, a
//     longer frame's tail ignored, a larger headerLen / entryLen / entryFixed
//     skipped, every way an entry walk stops, a 97-byte name omitted, page
//     fill (3 direct, 2 routed with 34-byte names), the busy payload;
//   * who may read: the whole rule table and its status mapping;
//   * the kinds: every whitelist row on both tiers, every never row, the
//     health / sealed / GPS / TEXT refinement, offer(), the horizon order and
//     caps;
//   * the ledger: rotation chains (one and several steps), TimeAnchors
//     promotion, equal heads never swap ids, trim and restart notes, a crash
//     between rename and note, a shrink without a note, card out and back,
//     horizon eviction, gen only on structural change, endSum wrap, base past
//     2^31, delivered;
//   * LIST pages and READ checks over a ledger;
//   * read windows, the digest range, the serve pacer;
//   * the sealing AAD, the on-card sealed record and the collector's names.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "System_ESPNow_AnswerCore.h"
#include "System_ESPNow_CatchupCore.h"
#include "System_ESPNow_SessionCapsCore.h"

using namespace espnowcatchup;

static unsigned gChecks = 0;
#define CHECK(c)                                                                 \
  do {                                                                           \
    ++gChecks;                                                                   \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

// The shared numbers are the answer core's and the caps core's.
static_assert(kStOk == espnowanswer::kOk && kStBusy == espnowanswer::kBusy &&
              kStRefused == espnowanswer::kRefused && kStUnsupported == espnowanswer::kUnsupported &&
              kStTooLarge == espnowanswer::kTooLarge && kStBadRequest == espnowanswer::kBadRequest &&
              kStNotReady == espnowanswer::kNotReady && kStIoError == espnowanswer::kIoError,
              "shared status codes");
static_assert(kStNoLink >= espnowanswer::kFrameFirst && kStGone >= espnowanswer::kFrameFirst,
              "per-frame codes start at 16");
static_assert(kCapCollect == sessioncaps::kCollect && kCapFilesServe == sessioncaps::kFilesServe, "caps bits");
static_assert(kDirectPayload - kReadRespHeaderLen == 182 && kRoutedPayload - kReadRespHeaderLen == 126, "data caps");
static_assert(kDirectPayload - kReadRespHeaderLen - kSealOverhead == 154 &&
              kRoutedPayload - kReadRespHeaderLen - kSealOverhead == 98, "sealed caps");
static_assert(kSealNonce + kSealTag == kSealOverhead, "seal overhead");

static bool bytesEq(const uint8_t* a, const uint8_t* b, size_t n) { return std::memcmp(a, b, n) == 0; }
static bool is(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

static void testListReq() {
  ListReq r = { 0x11223344u, 0x55667788u, 0x0041, 0, 3, 200 };
  uint8_t b[32];
  std::memset(b, 0xEE, sizeof(b));
  CHECK(packListReq(r, b, 15) == 0);
  CHECK(packListReq(r, b, sizeof(b)) == 16);
  const uint8_t gold[16] = { 0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55, 0x41, 0x00, 0x00, 0x03, 0xC8, 0, 0, 0 };
  CHECK(bytesEq(b, gold, 16));
  ListReq p = {};
  CHECK(!parseListReq(gold, 15, &p));
  CHECK(parseListReq(gold, 16, &p));
  CHECK(p.reqId == 0x11223344u && p.fromFileId == 0x55667788u && p.kindMask == 0x41 && p.link == 0 &&
        p.maxEntries == 3 && p.maxFrame == 200);
  // A later, longer request: the tail (and the reserved bytes) are ignored.
  uint8_t longer[24];
  std::memcpy(longer, gold, 16);
  std::memset(longer + 13, 0x5A, sizeof(longer) - 13);
  CHECK(parseListReq(longer, sizeof(longer), &p) && p.maxFrame == 200 && p.reqId == 0x11223344u);
}

static void testReadReq() {
  ReadReq r = { 1, 2, 0x1000, 0, 0, kModeDigest, 0, kRqStoredBefore, 0xCAFEBABEu };
  uint8_t b[24];
  CHECK(packReadReq(r, b, 23) == 0);
  CHECK(packReadReq(r, b, 24) == 24);
  const uint8_t gold[24] = { 1, 0, 0, 0, 2, 0, 0, 0, 0x00, 0x10, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1,
                             0xBE, 0xBA, 0xFE, 0xCA };
  CHECK(bytesEq(b, gold, 24));
  ReadReq p = {};
  CHECK(!parseReadReq(gold, 23, &p));
  CHECK(parseReadReq(gold, 24, &p));
  CHECK(p.reqId == 1 && p.fileId == 2 && p.offset == 0x1000 && p.maxBytes == 0 && p.link == 0 &&
        p.mode == kModeDigest && p.maxFrame == 0 && p.flags == kRqStoredBefore && p.ledgerId == 0xCAFEBABEu);
  uint8_t longer[30];
  std::memcpy(longer, gold, 24);
  std::memset(longer + 24, 0x77, 6);
  CHECK(parseReadReq(longer, sizeof(longer), &p) && p.ledgerId == 0xCAFEBABEu);
}

static void testReadResp() {
  ReadRespHeader h = { 9, kStOk, kRfLast | kRfAtEnd, 0, 0, 2, 0x1000, 0x10B6 };
  uint8_t b[64];
  CHECK(packReadRespHeader(h, b, 19) == 0);
  CHECK(packReadRespHeader(h, b, sizeof(b)) == 20);
  const uint8_t gold[20] = { 9, 0, 0, 0, 0, 0x03, 0, 0x14, 2, 0, 0, 0, 0x00, 0x10, 0, 0, 0xB6, 0x10, 0, 0 };
  CHECK(bytesEq(b, gold, 20));
  std::memcpy(b + 20, "hello", 5);
  ReadRespHeader p = {};
  const uint8_t* data = nullptr;
  size_t dl = 0;
  CHECK(!parseReadResp(b, 19, &p, &data, &dl));
  CHECK(parseReadResp(b, 25, &p, &data, &dl));
  CHECK(p.reqId == 9 && p.flags == 3 && p.headerLen == 20 && p.fileId == 2 && p.offset == 0x1000 && p.end == 0x10B6);
  CHECK(data == b + 20 && dl == 5 && std::memcmp(data, "hello", 5) == 0);
  // A larger headerLen is skipped; a headerLen below 20 or past n is refused.
  uint8_t g[40] = {};
  std::memcpy(g, gold, 20);
  g[7] = 24;
  std::memcpy(g + 24, "xy", 2);
  CHECK(parseReadResp(g, 26, &p, &data, &dl) && data == g + 24 && dl == 2);
  CHECK(!parseReadResp(g, 23, &p, &data, &dl));
  g[7] = 19;
  CHECK(!parseReadResp(g, 26, &p, &data, &dl));
  // offset == end: one frame, no data, last and atEnd.
  CHECK(parseReadResp(gold, 20, &p, &data, &dl) && dl == 0 && (p.flags & kRfLast) && (p.flags & kRfAtEnd));
}

static Entry ent(uint32_t id, const char* name, uint8_t kind = 1, uint8_t flags = 0) {
  Entry e = {};
  e.kind = kind; e.flags = flags; e.fileId = id; e.start = 0; e.end = 100 * id; e.createdBoot = 41;
  e.delivered = 10 * id; e.name = name;
  return e;
}

static void testListResp() {
  // Golden: one entry.
  ListRespHeader h = {};
  h.reqId = 0x01020304u; h.ledgerGen = 0x0A; h.ledgerId = 0xDEADBEEFu; h.bootTag = 0x1234; h.flags = kLfAnySealed;
  Entry e = {};
  e.kind = 6; e.flags = kEfSealedOnSend | kEfOnSd; e.fileId = 7; e.start = 0x100; e.end = 0x200; e.createdBoot = 41;
  e.delivered = 0x180; e.name = "system.log";
  uint8_t b[256];
  size_t packed = 99;
  const size_t n = packListResp(h, &e, 1, kDirectPayload, 0, b, sizeof(b), &packed);
  CHECK(n == 58 && packed == 1);
  const uint8_t gold[58] = {
    0x04, 0x03, 0x02, 0x01, 0, 0, 1, 0x18, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0xEF, 0xBE, 0xAD, 0xDE, 0x34, 0x12, 0x01, 0x18,
    0x22, 6, 0x28, 10, 7, 0, 0, 0, 0x00, 0x01, 0, 0, 0x00, 0x02, 0, 0, 41, 0, 0, 0, 0x80, 0x01, 0, 0,
    's', 'y', 's', 't', 'e', 'm', '.', 'l', 'o', 'g' };
  CHECK(bytesEq(b, gold, 58));
  ListRespHeader p = {};
  CHECK(!parseListRespHeader(gold, 23, &p));
  CHECK(parseListRespHeader(gold, 58, &p));
  CHECK(p.reqId == 0x01020304u && p.entryCount == 1 && p.headerLen == 24 && p.nextFileId == 0 && p.ledgerGen == 10 &&
        p.ledgerId == 0xDEADBEEFu && p.bootTag == 0x1234 && p.flags == 1 && p.entryFixed == 24);
  EntryCursor c = entries(gold, 58, p);
  Entry x = {};
  CHECK(c.next(&x));
  CHECK(x.kind == 6 && x.flags == 0x28 && x.nameLen == 10 && x.fileId == 7 && x.start == 0x100 && x.end == 0x200 &&
        x.createdBoot == 41 && x.delivered == 0x180 && std::memcmp(x.name, "system.log", 10) == 0);
  CHECK(!c.next(&x));

  // Page fill with 34-byte names: 3 direct (198 <= 202), 2 routed (140 <= 146).
  const char* name34 = "2026-11-19/env-2026-11-19.csv.1234";
  CHECK(std::strlen(name34) == 34);
  Entry five[5] = { ent(1, name34), ent(2, name34), ent(3, name34), ent(4, name34), ent(5, name34) };
  ListRespHeader hh = {};
  hh.nextFileId = 77;
  CHECK(packListResp(hh, five, 5, kDirectPayload, 0, b, sizeof(b), &packed) == 198 && packed == 3);
  CHECK(parseListRespHeader(b, 198, &p) && p.entryCount == 3 && p.nextFileId == 4);
  CHECK(packListResp(hh, five, 5, kRoutedPayload, 0, b, sizeof(b), &packed) == 140 && packed == 2);
  CHECK(parseListRespHeader(b, 140, &p) && p.nextFileId == 3);
  // maxEntries; all packed -> the caller's nextFileId; cap below the payload wins.
  CHECK(packListResp(hh, five, 5, kDirectPayload, 1, b, sizeof(b), &packed) == 82 && packed == 1);
  CHECK(packListResp(hh, five, 2, kDirectPayload, 0, b, sizeof(b), &packed) == 140 && packed == 2);
  CHECK(parseListRespHeader(b, 140, &p) && p.nextFileId == 77);
  CHECK(packListResp(hh, five, 5, kDirectPayload, 0, b, 100, &packed) == 82 && packed == 1);
  CHECK(packListResp(hh, five, 5, kDirectPayload, 0, b, 23, &packed) == 0 && packed == 0);
  // Empty list.
  CHECK(packListResp(hh, nullptr, 0, kDirectPayload, 0, b, sizeof(b), &packed) == 24 && packed == 0);

  // A 97-byte name goes out with nameLen 0 and flag 4; 96 fits.
  std::string n97(97, 'a'), n96(96, 'b');
  Entry big[2] = { ent(1, n97.c_str()), ent(2, n96.c_str()) };
  const size_t bl = packListResp(hh, big, 2, 255, 0, b, sizeof(b), &packed);
  CHECK(packed == 2 && bl == 24 + 24 + 120);
  CHECK(parseListRespHeader(b, bl, &p));
  c = entries(b, bl, p);
  CHECK(c.next(&x) && x.nameLen == 0 && (x.flags & kEfNameOmitted) && x.fileId == 1);
  CHECK(c.next(&x) && x.nameLen == 96 && !(x.flags & kEfNameOmitted) && x.fileId == 2);
  CHECK(packListResp(hh, big + 1, 1, 255, 0, b, sizeof(b), &packed) == 144 && packed == 1);
  // The caller's own omitted bit is not trusted.
  Entry liar = ent(3, "ok.csv", 1, kEfNameOmitted);
  CHECK(packListResp(hh, &liar, 1, 255, 0, b, sizeof(b), &packed) == 54);
  CHECK(parseListRespHeader(b, 54, &p));
  c = entries(b, 54, p);
  CHECK(c.next(&x) && x.nameLen == 6 && !(x.flags & kEfNameOmitted));
  // The floor frame (64): the first entry still makes progress without its name.
  CHECK(packListResp(hh, five, 5, kMinFrame, 0, b, sizeof(b), &packed) == 48 && packed == 1);
  CHECK(parseListRespHeader(b, 48, &p) && p.nextFileId == 2);
  c = entries(b, 48, p);
  CHECK(c.next(&x) && x.nameLen == 0 && (x.flags & kEfNameOmitted));
}

static size_t buildEntry(uint8_t* q, uint8_t entryLen, uint8_t nameLen, uint32_t id, const char* name,
                         uint8_t fixed = 24) {
  std::memset(q, 0, entryLen ? entryLen : 1);
  q[0] = entryLen; q[1] = 1; q[2] = 0; q[3] = nameLen;
  le::put32(q + 4, id);
  if (name) std::memcpy(q + fixed, name, nameLen);
  return entryLen;
}

static void testEntryWalk() {
  uint8_t b[256];
  ListRespHeader h = {};
  Entry x = {};
  auto header = [&](uint8_t count, uint8_t headerLen, uint8_t fixed) {
    std::memset(b, 0, sizeof(b));
    ListRespHeader w = {};
    w.entryCount = count; w.headerLen = headerLen; w.entryFixed = fixed;
    writeListRespHeader(w, b);
  };
  // A larger headerLen is skipped; a larger entryLen is skipped; entryCount is respected.
  header(2, 28, 24);
  size_t at = 28;
  at += buildEntry(b + at, 24 + 3 + 5, 3, 1, "abc");   // 5 bytes of a later version's entry fields
  at += buildEntry(b + at, 24 + 2, 2, 2, "de");
  at += buildEntry(b + at, 24 + 2, 2, 3, "fg");      // beyond entryCount
  CHECK(parseListRespHeader(b, at, &h) && h.headerLen == 28);
  EntryCursor c = entries(b, at, h);
  CHECK(c.next(&x) && x.fileId == 1 && x.nameLen == 3 && std::memcmp(x.name, "abc", 3) == 0);
  CHECK(c.next(&x) && x.fileId == 2 && std::memcmp(x.name, "de", 2) == 0);
  CHECK(!c.next(&x));
  CHECK(!c.next(&x));
  // entryFixed 28: a later version added fixed fields before the name.
  header(1, 24, 28);
  at = 24;
  at += buildEntry(b + at, 28 + 4, 4, 9, "name", 28);
  CHECK(parseListRespHeader(b, at, &h));
  c = entries(b, at, h);
  CHECK(c.next(&x) && x.fileId == 9 && std::memcmp(x.name, "name", 4) == 0);
  // entryFixed below 24: nothing is read.
  header(1, 24, 20);
  at = 24 + buildEntry(b + 24, 30, 2, 9, "zz");
  CHECK(parseListRespHeader(b, at, &h));
  c = entries(b, at, h);
  CHECK(!c.next(&x));
  // entryLen < entryFixed + nameLen stops (and stays stopped).
  header(3, 24, 24);
  at = 24;
  at += buildEntry(b + at, 26, 2, 1, "ok");
  at += buildEntry(b + at, 26, 5, 2, "toolo");
  at += buildEntry(b + at, 26, 2, 3, "no");
  CHECK(parseListRespHeader(b, at, &h));
  c = entries(b, at, h);
  CHECK(c.next(&x) && x.fileId == 1);
  CHECK(!c.next(&x));
  CHECK(!c.next(&x));
  // entryLen 0 stops.
  header(2, 24, 24);
  at = 24;
  at += buildEntry(b + at, 26, 2, 1, "ok");
  b[at++] = 0;
  CHECK(parseListRespHeader(b, at, &h));
  c = entries(b, at, h);
  CHECK(c.next(&x) && !c.next(&x));
  // An overrun stops: entryLen runs past the frame.
  header(1, 24, 24);
  buildEntry(b + 24, 40, 2, 1, "ok");
  CHECK(parseListRespHeader(b, 24 + 30, &h));
  c = entries(b, 24 + 30, h);
  CHECK(!c.next(&x));
  // A short entry (less than the fixed fields) stops.
  header(1, 24, 24);
  b[24] = 10;
  CHECK(parseListRespHeader(b, 34, &h));
  c = entries(b, 34, h);
  CHECK(!c.next(&x));
  // nameLen over 96 stops.
  header(1, 24, 24);
  buildEntry(b + 24, 24 + 97, 97, 1, nullptr);
  CHECK(parseListRespHeader(b, 24 + 121, &h));
  c = entries(b, 24 + 121, h);
  CHECK(!c.next(&x));
  // A header whose headerLen is under 24 or past n is refused.
  header(0, 23, 24);
  CHECK(!parseListRespHeader(b, 30, &h));
  header(0, 40, 24);
  CHECK(!parseListRespHeader(b, 30, &h));
}

static void testBusyAndStatus() {
  uint8_t b[64];
  std::memset(b, 0xEE, sizeof(b));
  CHECK(packBusy(kOpListResp, 5, 0, 600, b, 25) == 0);
  CHECK(packBusy(kOpListResp, 5, 0, 600, b, sizeof(b)) == 26);
  const uint8_t gold[26] = { 5, 0, 0, 0, 1, 0, 0, 0x18, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x18, 0x58, 0x02 };
  CHECK(bytesEq(b, gold, 26));
  ListRespHeader h = {};
  CHECK(parseListRespHeader(b, 26, &h) && h.status == kStBusy && h.entryCount == 0);
  CHECK(busyRetryAfter(b, 26, h.headerLen) == 600);
  CHECK(busyRetryAfter(b, 25, h.headerLen) == 0);
  CHECK(packBusy(kOpReadResp, 6, 0, 10, b, sizeof(b)) == 22);
  ReadRespHeader r = {};
  const uint8_t* d = nullptr;
  size_t dl = 0;
  CHECK(parseReadResp(b, 22, &r, &d, &dl) && r.status == kStBusy && r.reqId == 6 && dl == 2 && le::get16(d) == 10);
  CHECK(packBusy(kOpListReq, 5, 0, 1, b, sizeof(b)) == 0);
  CHECK(packStatus(kOpReadResp, 7, 0, kStGone, b, sizeof(b)) == 20);
  CHECK(parseReadResp(b, 20, &r, &d, &dl) && r.status == kStGone && r.reqId == 7);
  CHECK(packStatus(kOpListResp, 8, 3, kStNoLink, b, sizeof(b)) == 24);
  CHECK(parseListRespHeader(b, 24, &h) && h.status == kStNoLink && h.link == 3 && h.entryFixed == 24);
  CHECK(packStatus(kOpReadReq, 8, 0, 0, b, sizeof(b)) == 0);
}

// ---------------------------------------------------------------------------
// Who may read
// ---------------------------------------------------------------------------

static void testReader() {
  ReaderInputs in = {};
  in.built = true; in.serveOn = true; in.namedKind = 1;
  CHECK(readerAllowed(in) == Reader::Allowed);
  in.namedKind = 2;   // a backup is the same as the primary (Q9)
  CHECK(readerAllowed(in) == Reader::Allowed);
  in.namedKind = 0;
  CHECK(readerAllowed(in) == Reader::NotNamed);
  in.namedKind = 2;
  in.capsConfirmed = true; in.requesterCaps = 0;
  CHECK(readerAllowed(in) == Reader::NotCollector);
  in.requesterCaps = kCapCollect;
  CHECK(readerAllowed(in) == Reader::Allowed);
  in.capsConfirmed = false; in.requesterCaps = 0;   // not yet confirmed: not refused for caps
  CHECK(readerAllowed(in) == Reader::Allowed);
  in.link = 1;
  CHECK(readerAllowed(in) == Reader::NoLink);
  in.link = 0;
  // A bond worker in bond mode serves nobody until package 6 (owner Q1:
  // named masters by key only; Q19: no bond-worker files now): not its bond
  // partner, not even a named master (security review 2026-11-19).
  in.bondWorkerInBondMode = true;
  CHECK(readerAllowed(in) == Reader::BondWorker);
  in.requesterIsBondPartner = true;
  CHECK(readerAllowed(in) == Reader::BondWorker);
  in.bondTokenValid = true;
  CHECK(readerAllowed(in) == Reader::BondWorker);
  in.namedKind = 0;
  CHECK(readerAllowed(in) == Reader::BondWorker);
  in.requesterIsBondPartner = false;
  CHECK(readerAllowed(in) == Reader::BondWorker);
  // First match wins.
  in = {};
  in.built = false; in.serveOn = false; in.link = 1;
  CHECK(readerAllowed(in) == Reader::NotBuilt);
  in.built = true;
  CHECK(readerAllowed(in) == Reader::SettingOff);
  in.serveOn = true;
  CHECK(readerAllowed(in) == Reader::NoLink);
  // Status mapping.
  CHECK(readerStatus(Reader::Allowed) == 0);
  CHECK(readerStatus(Reader::NotBuilt) == 3);
  CHECK(readerStatus(Reader::NoLink) == 16);
  CHECK(readerStatus(Reader::SettingOff) == 2 && readerStatus(Reader::BondWorker) == 2 &&
        readerStatus(Reader::NotNamed) == 2 && readerStatus(Reader::NotCollector) == 2);
  CHECK(is(readerName(Reader::NotCollector), "not_collector"));
}

// ---------------------------------------------------------------------------
// Kinds
// ---------------------------------------------------------------------------

static void expectKind(const char* path, Kind k, Seal s, const char* rel, uint8_t group = kNoSlot) {
  const Classified c = classifyPath(path);
  if (c.kind != k || (k != Kind::None && (c.seal != s || !is(c.rel, rel) || c.group != group ||
                                          c.onSd != (std::strncmp(path, "/sd/", 4) == 0)))) {
    std::fprintf(stderr, "classify %s: kind %u seal %u rel %s group %u\n", path, unsigned(c.kind), unsigned(c.seal),
                 c.rel ? c.rel : "(null)", unsigned(c.group));
  }
  CHECK(c.kind == k);
  if (k == Kind::None) return;
  CHECK(c.seal == s);
  CHECK(is(c.rel, rel));
  CHECK(c.group == group);
  CHECK(c.onSd == (std::strncmp(path, "/sd/", 4) == 0));
}

static void testKinds() {
  for (int tier = 0; tier < 2; ++tier) {
    const std::string T = tier ? "/sd" : "";
    auto P = [&](const char* p) { return T + p; };
    expectKind(P("/logging_captures/sensors/2026-11-19/env-2026-11-19.csv").c_str(), Kind::SensorCapture, Seal::Plain,
               "2026-11-19/env-2026-11-19.csv");
    expectKind(P("/logging_captures/sensors/boot-41/env-boot41-1234.csv").c_str(), Kind::SensorCapture, Seal::Plain,
               "boot-41/env-boot41-1234.csv");
    expectKind(P("/logging_captures/sensors/2026-11-19/health-2026-11-19.csv.1").c_str(), Kind::SensorCapture,
               Seal::Plain, "2026-11-19/health-2026-11-19.csv.1");
    expectKind(P("/logging_captures/r1-health-history/0123456789abcdef0123456789abcdef-1763550000-+0060.r1h").c_str(),
               Kind::Health, Seal::AtRestOnly, "0123456789abcdef0123456789abcdef-1763550000-+0060.r1h");
    expectKind(P("/system/sys_logs/events.log").c_str(), Kind::EventsLog, Seal::OnSend, "events.log",
               uint8_t(tier * 3 + 0));
    expectKind(P("/system/sys_logs/errors.log").c_str(), Kind::ErrorLogs, Seal::Plain, "errors.log",
               uint8_t(tier * 3 + 1));
    expectKind(P("/system/sys_logs/crash-history.log").c_str(), Kind::ErrorLogs, Seal::Plain, "crash-history.log",
               uint8_t(tier * 3 + 2));
    expectKind(P("/logging_captures/system/system.log").c_str(), Kind::SystemLog, Seal::OnSend, "system.log");
    expectKind(P("/logging_captures/system/system.log.3").c_str(), Kind::SystemLog, Seal::OnSend, "system.log.3");
    expectKind(P("/logging_captures/system/system-2026-11-19T10-00-00.log").c_str(), Kind::SystemLog, Seal::OnSend,
               "system-2026-11-19T10-00-00.log");
    expectKind(P("/logging_captures/system/system-uptime-1234.log.2").c_str(), Kind::SystemLog, Seal::OnSend,
               "system-uptime-1234.log.2");
    expectKind(P("/stt/u3/20261119T120000Z-citrinet-0011223344556677.txt").c_str(), Kind::Transcript, Seal::OnSend,
               "u3/20261119T120000Z-citrinet-0011223344556677.txt");
    expectKind(P("/stt/u12/a.txt").c_str(), Kind::Transcript, Seal::OnSend, "u12/a.txt");

    // Never.
    const char* never[] = {
      "/system/sys_logs/successful_login.log", "/system/sys_logs/failed_login.log",
      "/system/sys_logs/command-audit.log", "/system/sys_logs/system-events.log", "/system/sys_logs/i2c_errors.log",
      "/stt/u3/notes/x.txt", "/stt/u03/x.txt", "/stt/u0/x.txt", "/stt/x.txt", "/stt/u3/a/x.txt", "/stt/u3/x.tmp",
      "/stt/u3/.txt", "/stt/uu3/x.txt", "/stt/u99999999999/x.txt", "/stt/u3", "/g2/x.txt", "/g2/notes/a.txt",
      "/logging_captures/tracks/2026-11-19.gpx", "/system/espnow/peers.json", "/espnow/x", "/system/users.json",
      "/STT Raw/a.wav", "/logging_captures/sensors/x.csv", "/logging_captures/sensors/a/b/c.csv",
      "/logging_captures/sensors/2026-11-19/x.csv.tmp", "/logging_captures/sensors/2026-11-19/x.header.tmp",
      "/logging_captures/sensors/2026-11-19/x.csv.bak", "/logging_captures/sensors/2026-11-19/",
      "/logging_captures/r1-health-history/x.csv", "/logging_captures/r1-health-history/a/b.r1h",
      "/logging_captures/r1-health-history/.r1h", "/logging_captures/system/system.log.tmp",
      "/logging_captures/system/other.log", "/logging_captures/system/sub/system.log",
      "/logging_captures/system/system.log.01", "/logging_captures/system/system.log.100",
      "/logging_captures/system/system-.log", "/logging_captures/system/system-a b.log",
      "/system/sys_logs/events.log.1", "/system/sys_logs/errors.log.bak",
    };
    for (const char* p : never) expectKind(P(p).c_str(), Kind::None, Seal::Plain, nullptr);
  }
  // Non-canonical spellings are never offered.
  const char* odd[] = {
    "/sd/./stt/u3/x.txt", "/stt/../stt/u3/x.txt", "/stt/u3//x.txt", "/stt\\u3\\x.txt", "/sd/stt\\u3/x.txt",
    "/sd/STT/u3/x.txt", "stt/u3/x.txt", "", "/", "/stt/u3/x.txt/", "/sd//stt/u3/x.txt",
    "/sd/logging_captures/sensors/../system/system.log",
  };
  for (const char* p : odd) CHECK(classifyPath(p).kind == Kind::None);
  CHECK(classifyPath(nullptr).kind == Kind::None);
  // battery.csv lives on LittleFS only, .1 and .2 only.
  expectKind("/battery.csv", Kind::BatteryLog, Seal::Plain, "battery.csv");
  expectKind("/battery.csv.1", Kind::BatteryLog, Seal::Plain, "battery.csv.1");
  expectKind("/battery.csv.2", Kind::BatteryLog, Seal::Plain, "battery.csv.2");
  expectKind("/battery.csv.3", Kind::None, Seal::Plain, nullptr);
  expectKind("/sd/battery.csv", Kind::None, Seal::Plain, nullptr);
  // trimSlot is an exact compare.
  CHECK(trimSlot("/system/sys_logs/events.log") == 0 && trimSlot("/sd/system/sys_logs/crash-history.log") == 5);
  CHECK(trimSlot("/system/sys_logs/command-audit.log") == kNoSlot && trimSlot(nullptr) == kNoSlot);

  // Refinement (sensor captures only).
  auto refine = [](const char* path, const char* head) {
    const size_t n = std::strlen(head);
    const uint8_t* h = reinterpret_cast<const uint8_t*>(head);
    return refineWithHead(classifyPath(path), h, n, hasSealedMagic(h, n));
  };
  const char* day = "/sd/logging_captures/sensors/2026-11-19/";
  Classified c = refine((std::string(day) + "health-2026-11-19.csv").c_str(), "time,hr\n1,60\n");
  CHECK(c.kind == Kind::Health && c.seal == Seal::AtRestOnly);
  c = refine((std::string(day) + "env.csv").c_str(), "time,r1_hr,temp\n");
  CHECK(c.kind == Kind::Health && c.seal == Seal::AtRestOnly);
  c = refine((std::string(day) + "env.txt").c_str(), "12:00 temp=20\n12:01 r1: hr=60\n");
  CHECK(c.kind == Kind::Health && c.seal == Seal::AtRestOnly);
  // A sealed health file is still health (its header is plain text).
  c = refine((std::string(day) + "env.csv").c_str(), "#HW1ENC v1 k1\ntime,r1_hr\nAAAA\n");
  CHECK(c.kind == Kind::Health && c.seal == Seal::AtRestOnly);
  c = refine((std::string(day) + "env.csv").c_str(), "#HW1ENC v1 k1\ntime,gps_lat\nAAAA\n");
  CHECK(c.kind == Kind::SensorCapture && c.seal == Seal::Plain);
  c = refine((std::string(day) + "env.txt").c_str(), "#HW1ENC v1 k1\nAAAA\n");
  CHECK(c.kind == Kind::SensorCapture && c.seal == Seal::Plain);
  c = refine((std::string(day) + "gps.csv").c_str(), "timestamp_ms,gps_fix,gps_lat,gps_lon\n1,2,3\n");
  CHECK(c.kind == Kind::SensorCapture && c.seal == Seal::OnSend);
  c = refine((std::string(day) + "gps.csv.2").c_str(), "timestamp_ms,gps_lat,gps_lon\n1,2,3\n");
  CHECK(c.seal == Seal::OnSend);
  c = refine((std::string(day) + "env.csv").c_str(), "timestamp_ms,temp\n1,gps_lat\n");   // only the header line counts
  CHECK(c.seal == Seal::Plain);
  c = refine((std::string(day) + "env.txt").c_str(), "12:00 temp=20\n");
  CHECK(c.kind == Kind::SensorCapture && c.seal == Seal::OnSend);
  c = refine((std::string(day) + "env.txt.1").c_str(), "12:00 temp=20\n");
  CHECK(c.seal == Seal::OnSend);
  c = refine((std::string(day) + "env.csv").c_str(), "timestamp_ms,temp\n1,2\n");
  CHECK(c.kind == Kind::SensorCapture && c.seal == Seal::Plain);
  // Plain is an allow-list; everything else fails closed (security review).
  c = refine((std::string(day) + "env.jsonl").c_str(), "{}\n");
  CHECK(c.kind == Kind::SensorCapture && c.seal == Seal::OnSend);
  c = refine((std::string(day) + "env.csv").c_str(), "time,temp\n1,2\n");   // not a sensor-log header
  CHECK(c.seal == Seal::OnSend);
  // A GPS TRACK capture with a .csv name: sealed on send, never plain.
  c = refine((std::string(day) + "x-20261119.csv").c_str(), "# GPS Track Log\n# time,lat,lon\n1,52.1,4.3\n");
  CHECK(c.kind == Kind::SensorCapture && c.seal == Seal::OnSend);
  // A TEXT capture with another extension, holding a GPS fix: sealed on send.
  c = refine((std::string(day) + "env.log").c_str(), "12:00 gps: lat=52.1 lon=4.3\n");
  CHECK(c.kind == Kind::SensorCapture && c.seal == Seal::OnSend);
  c = refine((std::string(day) + "env").c_str(), "12:00 temp=20\n");
  CHECK(c.seal == Seal::OnSend);
  // An unsealed CSV whose header line runs past the 512-byte head may hide
  // the r1_ columns (they come last): withheld as possible health.
  {
    std::string longHeader = "timestamp_ms";
    while (longHeader.size() < kHeadMax + 40) longHeader += ",tof_obj0_dist";
    longHeader += ",r1_hr\n";
    const uint8_t* h = reinterpret_cast<const uint8_t*>(longHeader.data());
    c = refineWithHead(classifyPath((std::string(day) + "env.csv").c_str()), h, kHeadMax, false);
    CHECK(c.kind == Kind::Health && c.seal == Seal::AtRestOnly);
    CHECK(offer(c, false, true) == Offer::WithheldUnsealedHealth);
    // A shorter head of a file still being written: sealed on send for now.
    c = refineWithHead(classifyPath((std::string(day) + "env.csv").c_str()), h, 100, false);
    CHECK(c.kind == Kind::SensorCapture && c.seal == Seal::OnSend);
  }
  // Other kinds are untouched.
  c = refine("/system/sys_logs/events.log", "r1: x");
  CHECK(c.kind == Kind::EventsLog && c.seal == Seal::OnSend);

  // B11: logs on the card are sealed at rest, and their mark names the key
  // that sealed them ("#HW1ENC v1 k1 id=<16 hex>"). The classification of
  // such files is unchanged: the mark is found by its prefix, with or without
  // the id, and each kind keeps its sealing rule.
  {
    const char* withId = "#HW1ENC v1 k1 id=0123456789abcdef\n";
    const uint8_t* m = reinterpret_cast<const uint8_t*>(withId);
    CHECK(hasSealedMagic(m, std::strlen(withId)) && hasSealedMagic(m, 7) && !hasSealedMagic(m, 6));
    // Sensor captures sealed at rest are served as stored, never sealed twice.
    const std::string sealedGps = std::string(withId) + "timestamp_ms,gps_fix,gps_lat,gps_lon\nENC1:AAAA\n";
    c = refine((std::string(day) + "gps.csv").c_str(), sealedGps.c_str());
    CHECK(c.kind == Kind::SensorCapture && c.seal == Seal::Plain);
    c = refine((std::string(day) + "env.txt").c_str(), (std::string(withId) + "ENC1:AAAA\n").c_str());
    CHECK(c.kind == Kind::SensorCapture && c.seal == Seal::Plain);
    c = refine((std::string(day) + "track.csv").c_str(),
               (std::string(withId) + "# GPS Track Log\n# timestamp_ms,lat,lon\nENC1:AAAA\n").c_str());
    CHECK(c.kind == Kind::SensorCapture && c.seal == Seal::Plain);
    // Health stays health: its plain column header gives it away, sealed or not.
    c = refine((std::string(day) + "env.csv").c_str(), (std::string(withId) + "timestamp_ms,r1_hr\nENC1:AAAA\n").c_str());
    CHECK(c.kind == Kind::Health && c.seal == Seal::AtRestOnly);
    Classified sealedHealth = c;
    CHECK(offer(sealedHealth, true, true) == Offer::Serve);
    // The system log, events.log and transcripts on the card are still
    // sealed on send (a sealed-at-rest card file is sealed again on the air,
    // 28 bytes a frame): refinement does not touch them, whatever their head.
    for (const char* path : {"/sd/logging_captures/system/system.log", "/sd/logging_captures/system/system.log.2",
                             "/sd/system/sys_logs/events.log", "/sd/stt/u3/20261122T101500Z-local-0000000100000002.txt"}) {
      const Classified before = classifyPath(path);
      c = refine(path, (std::string(withId) + "ENC1:AAAA\n").c_str());
      CHECK(before.kind != Kind::None && c.kind == before.kind && c.seal == Seal::OnSend && before.seal == Seal::OnSend);
    }
    // errors.log and crash-history.log keep their rule on the card as well.
    c = refine("/sd/system/sys_logs/errors.log", (std::string(withId) + "ENC1:AAAA\n").c_str());
    CHECK(c.kind == Kind::ErrorLogs && c.seal == Seal::Plain);
    // What a card writer leaves beside a log is never offered: a file moved
    // aside (not this device's), a rotation's staging copy, a generation.
    for (const char* path : {"/sd/system/sys_logs/events.log.foreign-1", "/sd/system/sys_logs/errors.log.tmp",
                             "/sd/system/sys_logs/events.log.1", "/sd/logging_captures/system/system.log.foreign-1",
                             "/sd/stt/u3/x.txt.foreign-1", "/sd/logging_captures/sensors/2026-11-19/gps.csv.foreign-1",
                             "/sd/system/sys_logs/boot-debug-1.log"}) {
      CHECK(classifyPath(path).kind == Kind::None);
    }
    // The trim slots of the followed logs on the card are theirs alone.
    CHECK(trimSlot("/sd/system/sys_logs/events.log") == 3 && trimSlot("/sd/system/sys_logs/errors.log") == 4);
  }

  // offer().
  Classified h = { Kind::Health, Seal::AtRestOnly, false, kNoSlot, nullptr };
  CHECK(offer(h, false, true) == Offer::WithheldUnsealedHealth);
  CHECK(offer(h, true, false) == Offer::Serve);   // sent as stored, never sealed twice
  Classified s = { Kind::SystemLog, Seal::OnSend, false, kNoSlot, nullptr };
  CHECK(offer(s, false, false) == Offer::WithheldNoKey);
  CHECK(offer(s, false, true) == Offer::Serve);
  Classified p = { Kind::EventsLog, Seal::Plain, false, 0, nullptr };
  CHECK(offer(p, false, false) == Offer::Serve);
  Classified none = classifyPath("/system/users.json");
  CHECK(offer(none, true, true) == Offer::Never);

  // Names, bits and caps.
  unsigned sum = 0;
  for (uint8_t k = 1; k <= kKindCount; ++k) {
    sum += horizonCap(Kind(k));
    CHECK(kindName(Kind(k)) != nullptr);
    CHECK(kindBit(Kind(k)) == (1u << (k - 1)));
  }
  CHECK(sum == kHorizonTotal);
  CHECK(horizonCap(Kind::None) == 0 && kindName(Kind::None) == nullptr);
  CHECK(is(kindName(Kind::SystemLog), "system_log") && is(kindName(Kind::Transcript), "transcript"));
  CHECK(kindInMask(Kind::Health, 0) && kindInMask(Kind::Health, 1u << 3) && !kindInMask(Kind::Health, 1u << 2));

  // Horizon order.
  CHECK(newerThan(Kind::SystemLog, "system.log", "system.log.1"));
  CHECK(newerThan(Kind::SystemLog, "system.log.1", "system.log.2"));
  CHECK(!newerThan(Kind::SystemLog, "system.log.2", "system.log.1"));
  CHECK(newerThan(Kind::SystemLog, "system.log.2", "system.log.10") && !newerThan(Kind::SystemLog, "system.log.1", "system.log.1"));
  CHECK(newerThan(Kind::SensorCapture, "2026-11-19/a.csv", "2026-11-18/z.csv"));
  CHECK(newerThan(Kind::SensorCapture, "2026-11-19/b.csv", "2026-11-19/a.csv"));
  CHECK(newerThan(Kind::SensorCapture, "2026-11-19/a.csv", "2026-11-19/a.csv.1"));
  CHECK(newerThan(Kind::Health, "ffff0000ffff0000ffff0000ffff0000-1763550001-+0000.r1h",
                  "00000000000000000000000000000000-1763550000-+0000.r1h"));
  CHECK(newerThan(Kind::Health, "00000000000000000000000000000000-1763550002-+0000.r1h",
                  "ffff0000ffff0000ffff0000ffff0000-1763550001-+0000.r1h"));
  CHECK(newerThan(Kind::Health, "2026-11-19/health.csv", "00000000000000000000000000000000-1763550002-+0000.r1h"));
  CHECK(newerThan(Kind::Transcript, "u3/20261119T120001Z-a.txt", "u3/20261119T120000Z-a.txt"));
}

// ---------------------------------------------------------------------------
// Ledger
// ---------------------------------------------------------------------------

static LedgerState gS;   // 6.9 KB: static, as in PSRAM on the device

// One observed file. head == nullptr: the head was not read.
static Observed ob(const char* path, uint32_t size, const char* head) {
  Observed o;
  std::memset(&o, 0, sizeof(o));
  std::snprintf(o.path, sizeof(o.path), "%s", path);
  o.size = size;
  if (head) {
    o.head = reinterpret_cast<const uint8_t*>(head);
    const size_t n = std::strlen(head);
    o.headLen = uint16_t(n < kHeadMax ? n : kHeadMax);
    o.headRead = true;
  }
  o.sealedAtRest = hasSealedMagic(o.head, o.headLen);
  o.c = refineWithHead(classifyPath(o.path), o.head, o.headLen, o.sealedAtRest);
  o.c.rel = nullptr;   // pointed into this function's copy; reconcile does not use it
  return o;
}

static const LedgerEntry* at(const char* path) { return findByPath(gS, path); }
static uint32_t idAt(const char* path) {
  const LedgerEntry* e = at(path);
  return e ? e->fileId : 0;
}
static const ReconcileEnv kEnv = { 41, 1000, true, true };
static ReconcileEnv env(uint32_t now, bool sd = true, bool complete = true) {
  ReconcileEnv e = kEnv;
  e.nowMs = now; e.sdMounted = sd; e.walkComplete = complete;
  return e;
}

static void testRotationChain() {
  ledgerInit(gS, 0x1A2B3C4D);
  CHECK(gS.gen == 1 && gS.nextFileId == 1 && gS.count == 0);
  const char* X = "/sd/logging_captures/system/system.log";
  const char* X1 = "/sd/logging_captures/system/system.log.1";
  const char* X2 = "/sd/logging_captures/system/system.log.2";
  const char* X3 = "/sd/logging_captures/system/system.log.3";
  const char* X4 = "/sd/logging_captures/system/system.log.4";
  const char* A = "AAAA line one\n";
  const char* B = "BBBB line one\n";
  const char* C = "CCCC line one\n";
  const char* D = "DDDD line one\n";
  const char* E = "EEEE line one\n";
  Observed o[5];
  o[0] = ob(X, 100, A);
  ReconcileOut r = reconcile(gS, o, 1, nullptr, 0, env(1000));
  CHECK(r.changed && r.added == 1 && r.newIds == 1 && gS.gen == 2);
  const uint32_t idA = idAt(X);
  CHECK(idA == 1 && at(X)->createdBoot == 41 && at(X)->kind == uint8_t(Kind::SystemLog) &&
        at(X)->seal == uint8_t(Seal::OnSend) && (at(X)->flags & kLeOnSd));
  // Rotating kinds always need a head read, even when only grown.
  CHECK(needHead(at(X), X, 200));
  // X -> X.1, a new X.
  o[0] = ob(X1, 120, A);
  o[1] = ob(X, 10, B);
  r = reconcile(gS, o, 2, nullptr, 0, env(2000));
  CHECK(r.renamed == 1 && r.newIds == 1 && r.added == 0 && r.removed == 0);
  CHECK(idAt(X1) == idA && at(X1)->size == 120);
  const uint32_t idB = idAt(X);
  CHECK(idB == 2);
  // X.1 -> X.2, X -> X.1, a new X; the replaced X.1 is bigger than before.
  o[0] = ob(X2, 120, A);
  o[1] = ob(X1, 300, B);
  o[2] = ob(X, 5, C);
  r = reconcile(gS, o, 3, nullptr, 0, env(3000));
  CHECK(r.renamed == 2 && r.newIds == 1);
  CHECK(idAt(X2) == idA && idAt(X1) == idB);
  const uint32_t idC = idAt(X);
  // Once more: four files, cap 4.
  o[0] = ob(X3, 120, A);
  o[1] = ob(X2, 300, B);
  o[2] = ob(X1, 400, C);
  o[3] = ob(X, 7, D);
  r = reconcile(gS, o, 4, nullptr, 0, env(4000));
  CHECK(r.renamed == 3 && r.newIds == 1 && r.removed == 0 && !(gS.flags & kLsBeyondHorizon));
  CHECK(idAt(X3) == idA && idAt(X2) == idB && idAt(X1) == idC);
  const uint32_t idD = idAt(X);
  // A fifth file: the oldest (.4) leaves the horizon; its id is never reused.
  o[0] = ob(X4, 120, A);
  o[1] = ob(X3, 300, B);
  o[2] = ob(X2, 400, C);
  o[3] = ob(X1, 50, D);
  o[4] = ob(X, 1, E);
  const uint32_t genBefore = gS.gen;
  r = reconcile(gS, o, 5, nullptr, 0, env(5000));
  CHECK(r.removed == 1 && r.renamed == 4 && r.newIds == 1 && gS.gen == genBefore + 1);   // A moved to .4, then left the horizon
  CHECK(at(X4) == nullptr && findById(gS, idA) == nullptr && (gS.flags & kLsBeyondHorizon));
  CHECK(idAt(X3) == idB && idAt(X2) == idC && idAt(X1) == idD);
  const uint32_t idE = idAt(X);
  CHECK(idE == 5 && gS.nextFileId == 6);
  // Ascending fileId order is kept.
  for (uint8_t i = 1; i < gS.count; ++i) CHECK(gS.e[i - 1].fileId < gS.e[i].fileId);

  // Several rotations between two walks: X.3 holds what X was (equal head).
  ledgerInit(gS, 7);
  o[0] = ob(X, 100, A);
  reconcile(gS, o, 1, nullptr, 0, env(1000));
  const uint32_t id1 = idAt(X);
  o[0] = ob(X3, 150, A);
  o[1] = ob(X2, 150, B);
  o[2] = ob(X1, 150, C);
  o[3] = ob(X, 20, D);
  r = reconcile(gS, o, 4, nullptr, 0, env(2000));
  CHECK(idAt(X3) == id1 && r.renamed == 1 && r.newIds == 3);
  // A rotation never goes down (X.1 -> X) or across families.
  CHECK(isRotationSibling(X, X1) && isRotationSibling(X1, X2) && isRotationSibling(X, X3));
  CHECK(!isRotationSibling(X1, X) && !isRotationSibling(X1, X1));
  CHECK(!isRotationSibling(X, "/logging_captures/system/system.log.1"));   // another tier
  CHECK(!isRotationSibling("/battery.csv", "/battery.csv2") && isRotationSibling("/battery.csv.1", "/battery.csv.2"));
}

static void testAnchorPromotion() {
  const char* boot = "/sd/logging_captures/sensors/boot-41/env-boot41-123456.csv";
  const char* day = "/sd/logging_captures/sensors/2026-11-19/env-2026-11-19T10-00-00-boot41-000123456.csv";
  const char* dayK = "/sd/logging_captures/sensors/2026-11-19/env-2026-11-19T10-00-00-boot41-000123456-2.csv";
  CHECK(isAnchorPromotion(boot, day) && isAnchorPromotion(boot, dayK));
  CHECK(!isAnchorPromotion(day, boot));
  CHECK(!isAnchorPromotion(boot, "/logging_captures/sensors/2026-11-19/env-2026-11-19T10-00-00-boot41-000123456.csv"));
  CHECK(!isAnchorPromotion(boot, "/sd/logging_captures/sensors/2026-11-19/env-2026-11-19T10-00-00-boot42-000123456.csv"));
  CHECK(!isAnchorPromotion(boot, "/sd/logging_captures/sensors/2026-11-19/env-2026-11-19T10-00-00-boot41-000123457.csv"));
  CHECK(!isAnchorPromotion(boot, "/sd/logging_captures/sensors/2026-11-19/env-2026-11-19T10-00-00-boot41-000123456.txt"));
  CHECK(!isAnchorPromotion(boot, "/sd/logging_captures/sensors/2026-11-19/imu-2026-11-19T10-00-00-boot41-000123456.csv"));
  CHECK(!isAnchorPromotion(boot, "/sd/logging_captures/sensors/2026-11-19/env-boot41-000123456.csv"));   // no stamp
  CHECK(!isAnchorPromotion("/sd/logging_captures/sensors/boot-40/env-boot41-123456.csv", day));      // folder N
  CHECK(!isAnchorPromotion(boot, "/sd/logging_captures/sensors/boot-42/env-2026-boot41-000123456.csv"));

  ledgerInit(gS, 9);
  const char* H = "time,temp\n1,20.5\n";
  Observed o[3];
  o[0] = ob(boot, 400, H);
  reconcile(gS, o, 1, nullptr, 0, env(1000));
  const uint32_t id = idAt(boot);
  o[0] = ob(day, 400, H);
  ReconcileOut r = reconcile(gS, o, 1, nullptr, 0, env(2000));
  CHECK(r.renamed == 1 && r.newIds == 0 && idAt(day) == id && at(boot) == nullptr);
}

static void testEqualHeadsNeverSwap() {
  // Two boot files with identical heads, promoted in one sweep: each keeps its own id.
  const char* a0 = "/sd/logging_captures/sensors/boot-41/a-boot41-100.csv";
  const char* b0 = "/sd/logging_captures/sensors/boot-41/b-boot41-200.csv";
  const char* a1 = "/sd/logging_captures/sensors/2026-11-19/a-2026-11-19T10-00-00-boot41-000000100.csv";
  const char* b1 = "/sd/logging_captures/sensors/2026-11-19/b-2026-11-19T10-00-01-boot41-000000200.csv";
  const char* H = "time,temp\n";
  ledgerInit(gS, 11);
  Observed o[3];
  o[0] = ob(a0, 10, H);
  o[1] = ob(b0, 10, H);
  reconcile(gS, o, 2, nullptr, 0, env(1000));
  const uint32_t ia = idAt(a0), ib = idAt(b0);
  CHECK(ia && ib && ia != ib);
  o[0] = ob(b1, 10, H);   // listed first on purpose
  o[1] = ob(a1, 10, H);
  ReconcileOut r = reconcile(gS, o, 2, nullptr, 0, env(2000));
  CHECK(r.renamed == 2 && idAt(a1) == ia && idAt(b1) == ib);
  // An equal head alone never moves an id: a new file with the same head while
  // a known one vanishes gets a new id.
  const char* c0 = "/sd/logging_captures/sensors/2026-11-19/c.csv";
  o[0] = ob(b1, 10, H);
  o[1] = ob(c0, 10, H);
  r = reconcile(gS, o, 2, nullptr, 0, env(3000));
  CHECK(r.renamed == 0 && r.newIds == 1 && r.removed == 1 && findById(gS, ia) == nullptr && idAt(c0) > ib);
}

static void testTrimAndRestart() {
  const char* EV = "/system/sys_logs/events.log";
  ledgerInit(gS, 12);
  Observed o[2];
  o[0] = ob(EV, 1000, "2026-11-19T10:00:00Z boot 1\n");
  reconcile(gS, o, 1, nullptr, 0, env(1000));
  const uint32_t id = idAt(EV);
  CHECK(at(EV)->base == 0 && logicalEnd(*at(EV)) == 1000);
  // Growth only, full-enough head: no head read, no gen.
  LedgerEntry* e = findById(gS, id);
  e->headLen = kHeadMax;   // pretend the head covers 512 bytes
  CHECK(!needHead(e, EV, 1200));
  CHECK(needHead(e, EV, 1200, true));   // a trim note pending
  CHECK(needHead(e, EV, 900));          // shrank
  CHECK(needHead(e, "/sd/system/sys_logs/events.log", 1200));
  CHECK(needHead(nullptr, EV, 1200));
  e->headLen = 28;
  CHECK(needHead(e, EV, 1200));         // a short head is re-anchored
  o[0] = ob(EV, 1200, nullptr);
  uint32_t gen = gS.gen;
  ReconcileOut r = reconcile(gS, o, 1, nullptr, 0, env(2000));
  CHECK(!r.changed && gS.gen == gen && idAt(EV) == id && at(EV)->size == 1200 && at(EV)->lastGrowMs == 2000);
  // A re-anchored (longer) head does not step gen either.
  std::string full = std::string("2026-11-19T10:00:00Z boot 1\n") + std::string(600, 'x');
  o[0] = ob(EV, 1300, full.c_str());
  r = reconcile(gS, o, 1, nullptr, 0, env(2500));
  CHECK(!r.changed && idAt(EV) == id && at(EV)->headLen == kHeadMax);

  // A trim: base shifts by the drops, same id, gen++.
  TrimNote n = { 0, 900, false };
  o[0] = ob(EV, 500, "2026-11-19T11:00:00Z later\n");
  gen = gS.gen;
  r = reconcile(gS, o, 1, &n, 1, env(3000));
  CHECK(r.trimmed == 1 && r.changed && gS.gen == gen + 1 && idAt(EV) == id);
  CHECK(at(EV)->base == 900 && logicalEnd(*at(EV)) == 1400);
  uint32_t phys = 0;
  CHECK(toPhysical(*at(EV), 900, &phys) && phys == 0);
  CHECK(toPhysical(*at(EV), 1400, &phys) && phys == 500);
  CHECK(!toPhysical(*at(EV), 899, &phys) && !toPhysical(*at(EV), 1401, &phys));
  // Two trims since the last walk add up.
  TrimNote two[2] = { { 0, 100, false }, { 0, 50, false } };
  o[0] = ob(EV, 400, "2026-11-19T11:30:00Z\n");
  r = reconcile(gS, o, 1, two, 2, env(3500));
  CHECK(r.trimmed == 1 && idAt(EV) == id && at(EV)->base == 1050);
  // A note for another slot does nothing here.
  TrimNote other = { 3, 999, false };
  o[0] = ob(EV, 450, "2026-11-19T11:30:00Z\n");
  r = reconcile(gS, o, 1, &other, 1, env(3600));
  CHECK(r.trimmed == 0 && !r.changed && at(EV)->base == 1050);

  // A restart: a new id; base = the drops after the restart.
  TrimNote rs = { 0, 50, true };
  o[0] = ob(EV, 80, "restarted\n");
  r = reconcile(gS, o, 1, &rs, 1, env(4000));
  CHECK(r.newIds == 1 && r.added == 0 && r.removed == 1 && r.changed);
  const uint32_t id2 = idAt(EV);
  CHECK(id2 > id && at(EV)->base == 50 && findById(gS, id) == nullptr);
  // Restart then trim in one window: base = the trims after the restart.
  TrimNote rt[2] = { { 0, 0, true }, { 0, 30, false } };
  o[0] = ob(EV, 70, "restarted again\n");
  r = reconcile(gS, o, 1, rt, 2, env(4500));
  CHECK(r.newIds == 1 && idAt(EV) > id2 && at(EV)->base == 30);

  // A crash between rename and note: the head changed, no note -> new id.
  const uint32_t id3 = idAt(EV);
  o[0] = ob(EV, 60, "after the crash\n");
  r = reconcile(gS, o, 1, nullptr, 0, env(5000));
  CHECK(r.newIds == 1 && idAt(EV) > id3 && at(EV)->base == 0);
  // The head changed while the size grew: still a new id.
  const uint32_t id4 = idAt(EV);
  o[0] = ob(EV, 5000, "something else entirely\n");
  r = reconcile(gS, o, 1, nullptr, 0, env(5500));
  CHECK(r.newIds == 1 && idAt(EV) > id4);
  // A shrink without a note and the same head: a new id.
  const uint32_t id5 = idAt(EV);
  o[0] = ob(EV, 10, "something else entirely\n");
  r = reconcile(gS, o, 1, nullptr, 0, env(6000));
  CHECK(r.newIds == 1 && idAt(EV) > id5);
  // A trim note for a file the ledger never saw: a new id at base 0.
  ledgerInit(gS, 13);
  TrimNote fresh = { 1, 700, false };
  o[0] = ob("/system/sys_logs/errors.log", 100, "err\n");
  r = reconcile(gS, o, 1, &fresh, 1, env(1000));
  CHECK(r.added == 1 && at("/system/sys_logs/errors.log")->base == 0);
}

static void testBaseOverflow() {
  const char* EV = "/sd/system/sys_logs/errors.log";
  ledgerInit(gS, 14);
  Observed o[1];
  o[0] = ob(EV, 1000, "line\n");
  reconcile(gS, o, 1, nullptr, 0, env(1000));
  LedgerEntry* e = findById(gS, idAt(EV));
  e->base = 0x7FFFFF00u;
  const uint32_t id = e->fileId;
  TrimNote n = { 4, 0x200, false };
  o[0] = ob(EV, 300, "later\n");
  ReconcileOut r = reconcile(gS, o, 1, &n, 1, env(2000));
  CHECK(r.newIds == 1 && idAt(EV) != id && at(EV)->base == 0 && findById(gS, id) == nullptr);
  // Just under the limit is a normal trim.
  e = findById(gS, idAt(EV));
  e->base = 0x7FFFFE00u;
  const uint32_t id2 = e->fileId;
  TrimNote m = { 4, 0x100, false };
  o[0] = ob(EV, 200, "later still\n");
  r = reconcile(gS, o, 1, &m, 1, env(3000));
  CHECK(r.trimmed == 1 && idAt(EV) == id2 && at(EV)->base == 0x7FFFFF00u);
}

static void testCardOut() {
  const char* S = "/sd/logging_captures/sensors/2026-11-19/env-2026-11-19.csv";
  const char* L = "/system/sys_logs/events.log";
  const char* H = "time,temp\n";
  ledgerInit(gS, 15);
  Observed o[2];
  o[0] = ob(S, 100, H);
  o[1] = ob(L, 50, "ev\n");
  reconcile(gS, o, 2, nullptr, 0, env(1000));
  const uint32_t id = idAt(S), idL = idAt(L);
  const uint32_t gen = gS.gen;
  // Card out: only the LittleFS file is seen.
  o[0] = ob(L, 60, nullptr);
  ReconcileOut r = reconcile(gS, o, 1, nullptr, 0, env(2000, false));
  CHECK(r.cardOut == 1 && r.removed == 0 && !r.changed && gS.gen == gen);
  CHECK(at(S) && at(S)->state == uint8_t(EntryState::MissingCardOut) && at(S)->fileId == id);
  CHECK(!entryOffered(*at(S), true));
  Entry list[4];
  uint32_t next = 0;
  uint8_t flags = 0;
  CHECK(collectListEntries(gS, 0, 0, true, nullptr, 0, 2000, list, 4, &next, &flags) == 1 && list[0].fileId == idL);
  ReadReq rq = { 1, id, 0, 0, 0, kModeData, 0, 0, gS.ledgerId };
  CHECK(readCheck(gS, rq, true, nullptr) == kStNotReady);
  // Back: the same id, present again.
  o[0] = ob(S, 150, H);
  o[1] = ob(L, 60, nullptr);
  r = reconcile(gS, o, 2, nullptr, 0, env(3000));
  CHECK(!r.changed && idAt(S) == id && at(S)->state == uint8_t(EntryState::Present) && at(S)->size == 150);
  // A card in but a file gone: removed.
  o[0] = ob(L, 60, nullptr);
  r = reconcile(gS, o, 1, nullptr, 0, env(4000));
  CHECK(r.removed == 1 && r.changed && at(S) == nullptr);
  // An incomplete walk leaves an unseen entry alone.
  o[0] = ob(S, 10, H);
  o[1] = ob(L, 60, nullptr);
  reconcile(gS, o, 2, nullptr, 0, env(5000));
  const uint32_t idS2 = idAt(S);
  r = reconcile(gS, o, 0, nullptr, 0, env(6000, true, false));
  CHECK(r.removed == 0 && idAt(S) == idS2 && idAt(L) == idL);
}

static void testHorizonAndFullLedger() {
  // Eight sensor files: seven are kept (the newest), the oldest is never added.
  ledgerInit(gS, 16);
  Observed o[kObservedMax];
  char paths[10][kPathMax];
  for (int i = 0; i < 8; ++i) {
    std::snprintf(paths[i], kPathMax, "/sd/logging_captures/sensors/2026-11-%02d/env.csv", 10 + i);
    o[i] = ob(paths[i], 100, "time,temp\n");
  }
  ReconcileOut r = reconcile(gS, o, 8, nullptr, 0, env(1000));
  CHECK(r.added == 7 && gS.count == 7 && (gS.flags & kLsBeyondHorizon) && at(paths[0]) == nullptr);
  CHECK(gS.nextFileId == 8);
  // A newer day pushes the oldest kept one out; ids keep climbing.
  const uint32_t oldest = idAt(paths[1]);
  std::snprintf(paths[8], kPathMax, "/sd/logging_captures/sensors/2026-11-20/env.csv");
  o[8] = ob(paths[8], 100, "time,temp\n");
  r = reconcile(gS, o + 1, 8, nullptr, 0, env(2000));
  CHECK(r.added == 1 && r.removed == 1 && at(paths[1]) == nullptr && findById(gS, oldest) == nullptr);
  CHECK(idAt(paths[8]) == 8 && gS.nextFileId == 9);
  // Every kind at its cap: 32 entries. Health over its cap too.
  ledgerInit(gS, 17);
  size_t n = 0;
  static char many[kObservedMax][kPathMax];
  auto add = [&](const char* p, const char* head) {
    std::snprintf(many[n], kPathMax, "%s", p);
    o[n] = ob(many[n], 10, head);
    ++n;
  };
  for (int i = 0; i < 9; ++i) {
    char p[kPathMax];
    std::snprintf(p, sizeof(p), "/logging_captures/r1-health-history/%032d-17635500%02d-+0000.r1h", 0, i);
    add(p, "#HW1ENC v1 k1\nr1h\n");
  }
  add("/system/sys_logs/events.log", "e");
  add("/sd/system/sys_logs/events.log", "e");
  add("/battery.csv", "b");
  add("/battery.csv.1", "b1");
  add("/battery.csv.2", "b2");
  add("/system/sys_logs/errors.log", "x");
  add("/system/sys_logs/crash-history.log", "x");
  add("/sd/system/sys_logs/errors.log", "x");
  add("/sd/system/sys_logs/crash-history.log", "x");
  for (int i = 0; i < 4; ++i) {
    char p[kPathMax];
    std::snprintf(p, sizeof(p), i ? "/sd/logging_captures/system/system.log.%d" : "/sd/logging_captures/system/system.log", i);
    add(p, i ? "older\n" : "now\n");
  }
  for (int i = 0; i < 6; ++i) {
    char p[kPathMax];
    std::snprintf(p, sizeof(p), "/sd/stt/u3/20261119T1200%02dZ-a.txt", i);
    add(p, "hello\n");
  }
  for (int i = 0; i < 7; ++i) {
    char p[kPathMax];
    std::snprintf(p, sizeof(p), "/sd/logging_captures/sensors/2026-11-19/env-%d.csv", i);
    add(p, "timestamp_ms,temp\n");
  }
  // Health CSVs (one over their share of the health horizon).
  for (int i = 0; i < 4; ++i) {
    char p[kPathMax];
    std::snprintf(p, sizeof(p), "/sd/logging_captures/sensors/2026-11-%02d/health-2026-11-%02d.csv", 16 + i, 16 + i);
    add(p, "#HW1ENC v1 k1\ntimestamp_ms,r1_hr\nENC1:x\n");
  }
  r = reconcile(gS, o, n, nullptr, 0, env(1000));
  CHECK(gS.count == kHorizonTotal && r.added == kHorizonTotal && (gS.flags & kLsBeyondHorizon));
  // Health splits its six: the three newest r1h files and the three newest
  // health CSVs; neither source pushes the other out.
  CHECK(at(many[0]) == nullptr && at(many[5]) == nullptr && at(many[6]) != nullptr && at(many[8]) != nullptr);
  CHECK(at(many[n - 4]) == nullptr && at(many[n - 3]) != nullptr && at(many[n - 1]) != nullptr);
  CHECK(horizonGroupCap(Kind::Health, 0) + horizonGroupCap(Kind::Health, 1) == horizonCap(Kind::Health));
  CHECK(horizonGroup(Kind::Health, "x.r1h") == 1 && horizonGroup(Kind::Health, "d/health.csv") == 0 &&
        horizonGroup(Kind::SensorCapture, "x.r1h") == 0);
  // Card out: the 23 SD entries stay as MissingCardOut and are not in the
  // horizon, so 17 new LittleFS files enter; the ledger has 8 slots of slack,
  // then the oldest missing entries make room (40 at most).
  static char lfsPaths[17][kPathMax];
  Observed keep[kObservedMax];
  size_t lfs = 0;
  for (size_t j = 0; j < n; ++j)
    if (std::strncmp(many[j], "/sd/", 4) != 0 && at(many[j])) keep[lfs++] = o[j];
  CHECK(lfs == 9);
  const uint32_t firstSdId = [] {
    for (uint8_t i = 0; i < gS.count; ++i)
      if (gS.e[i].flags & kLeOnSd) return gS.e[i].fileId;
    return 0u;
  }();
  for (int i = 0; i < 17; ++i) {
    if (i < 7) std::snprintf(lfsPaths[i], kPathMax, "/logging_captures/sensors/2026-11-19/lfs-%d.csv", i);
    else if (i < 13) std::snprintf(lfsPaths[i], kPathMax, "/stt/u3/20261119T1300%02dZ-a.txt", i);
    else std::snprintf(lfsPaths[i], kPathMax, i == 13 ? "/logging_captures/system/system.log"
                                                      : "/logging_captures/system/system.log.%d", i - 13);
    keep[lfs++] = ob(lfsPaths[i], 10, "lfs\n");
  }
  r = reconcile(gS, keep, lfs, nullptr, 0, env(2000, false));
  CHECK(r.cardOut == 23 && r.added == 17 && r.removed == 9 && gS.count == kLedgerMax);
  CHECK(findById(gS, firstSdId) == nullptr);   // the oldest missing entry made room
  unsigned missing = 0, present = 0;
  for (uint8_t i = 0; i < gS.count; ++i) {
    missing += gS.e[i].state == uint8_t(EntryState::MissingCardOut);
    present += gS.e[i].state == uint8_t(EntryState::Present);
  }
  CHECK(missing == 14 && present == 26);
  for (uint8_t i = 1; i < gS.count; ++i) CHECK(gS.e[i - 1].fileId < gS.e[i].fileId);
}

static void testDeliveredAndEndSum() {
  LedgerEntry e = {};
  e.base = 100; e.size = 400;
  noteDelivered(e, 300);
  CHECK(e.delivered == 300);
  noteDelivered(e, 200);   // never goes back
  CHECK(e.delivered == 300);
  noteDelivered(e, 9999);  // clamped to end
  CHECK(e.delivered == 500);
  // endSum is mod 2^32 over offered entries.
  ledgerInit(gS, 18);
  gS.count = 3;
  for (int i = 0; i < 3; ++i) {
    LedgerEntry& x = gS.e[i];
    x.fileId = uint32_t(i + 1);
    x.kind = uint8_t(Kind::EventsLog);
    x.state = uint8_t(EntryState::Present);
    x.base = 0x7FFF0000u;
    x.size = 0x7FFF0000u;
  }
  gS.e[2].kind = uint8_t(Kind::Health);   // unsealed health: not offered
  gS.e[2].seal = uint8_t(Seal::AtRestOnly);
  CHECK(endSum(gS, true) == uint32_t(2ull * 0xFFFE0000ull));
  gS.e[2].flags = kLeSealedAtRest;
  CHECK(endSum(gS, true) == uint32_t(3ull * 0xFFFE0000ull));
  // growing().
  LedgerEntry g = {};
  CHECK(!growing(g, false, 1000) && growing(g, true, 1000));
  g.flags = kLeGrowSeen;
  g.lastGrowMs = 0xFFFFFF00u;   // across the ms wrap
  CHECK(growing(g, false, 1000) && !growing(g, false, 0xFFFFFF00u + kGrowingRecentMs));
}

static void testListAndRead() {
  ledgerInit(gS, 0xABCD0001u);
  Observed o[6];
  o[0] = ob("/sd/logging_captures/sensors/2026-11-19/env.csv", 300, "timestamp_ms,temp\n");
  o[1] = ob("/sd/logging_captures/sensors/2026-11-19/health-2026-11-19.csv", 200, "time,hr\n");   // unsealed
  o[2] = ob("/logging_captures/r1-health-history/00000000000000000000000000000000-1763550000-+0000.r1h", 90,
            "#HW1ENC v1 k1\nr1h\n");
  o[3] = ob("/sd/logging_captures/system/system.log", 5000, "boot\n");
  o[4] = ob("/sd/stt/u3/20261119T120000Z-a.txt", 40, "hello world\n");
  o[5] = ob("/system/sys_logs/events.log", 70, "ev\n");
  reconcile(gS, o, 6, nullptr, 0, env(1000));
  CHECK(gS.count == 6);
  Entry list[8];
  uint32_t next = 0;
  uint8_t f = 0;
  const char* active[1] = { "/sd/logging_captures/system/system.log" };
  // Key ready: everything but the unsealed health file.
  size_t k = collectListEntries(gS, 0, 0, true, active, 1, 500000, list, 8, &next, &f);
  CHECK(k == 5 && next == 0 && (f & kLfWithheld) && (f & kLfAnySealed) && !(f & kLfBeyondHorizon));
  for (size_t i = 1; i < k; ++i) CHECK(list[i - 1].fileId < list[i].fileId);
  const Entry& sys = list[2];
  CHECK(sys.kind == uint8_t(Kind::SystemLog) && (sys.flags & kEfSealedOnSend) && (sys.flags & kEfOnSd) &&
        (sys.flags & kEfGrowing) && is(sys.name, "system.log") && sys.end == 5000);
  CHECK(list[1].kind == uint8_t(Kind::Health) && (list[1].flags & kEfSealedAtRest) && !(list[1].flags & kEfOnSd));
  CHECK(!(list[0].flags & kEfGrowing));   // grew long ago, not the writer's path
  CHECK(is(list[3].name, "u3/20261119T120000Z-a.txt"));
  // No key: the sealed-on-send kinds (system log, transcript, events.log)
  // are withheld too.
  k = collectListEntries(gS, 0, 0, false, nullptr, 0, 1000, list, 8, &next, &f);
  CHECK(k == 2 && (f & kLfWithheld));
  // fromFileId, the kind mask and the cap.
  k = collectListEntries(gS, 4, 0, true, nullptr, 0, 1000, list, 8, &next, &f);
  CHECK(k == 3 && list[0].fileId == 4);
  k = collectListEntries(gS, 0, kindBit(Kind::Transcript) | kindBit(Kind::EventsLog), true, nullptr, 0, 1000, list,
                         8, &next, &f);
  CHECK(k == 2 && list[0].kind == uint8_t(Kind::Transcript) && list[1].kind == uint8_t(Kind::EventsLog));
  k = collectListEntries(gS, 0, 0, true, nullptr, 0, 1000, list, 2, &next, &f);
  CHECK(k == 2 && list[0].fileId == 1 && list[1].fileId == 3 && next == 4);   // id 2 (unsealed health) is skipped
  // Round trip through a page.
  ListRespHeader h = {};
  h.ledgerId = gS.ledgerId; h.ledgerGen = gS.gen; h.flags = f; h.nextFileId = next;
  uint8_t b[kDirectPayload];
  size_t packed = 0;
  const size_t len = packListResp(h, list, k, kDirectPayload, 0, b, sizeof(b), &packed);
  ListRespHeader p = {};
  CHECK(packed == 2 && parseListRespHeader(b, len, &p) && p.nextFileId == next && p.ledgerId == 0xABCD0001u);

  // READ checks.
  const uint32_t idSensor = idAt("/sd/logging_captures/sensors/2026-11-19/env.csv");
  const uint32_t idHealth = idAt("/sd/logging_captures/sensors/2026-11-19/health-2026-11-19.csv");
  const uint32_t idSys = idAt("/sd/logging_captures/system/system.log");
  ReadReq r = { 1, idSensor, 0, 0, 0, kModeData, 0, 0, gS.ledgerId };
  const LedgerEntry* e = nullptr;
  CHECK(readCheck(gS, r, true, &e) == kStOk && e && e->fileId == idSensor);
  r.ledgerId = 1;
  CHECK(readCheck(gS, r, true, &e) == kStGone && e == nullptr);
  r.ledgerId = gS.ledgerId;
  r.link = 1;
  CHECK(readCheck(gS, r, true, &e) == kStGone);
  r.link = 0;
  r.fileId = 999;
  CHECK(readCheck(gS, r, true, &e) == kStGone);
  r.fileId = idHealth;
  CHECK(readCheck(gS, r, true, &e) == kStRefused);
  r.fileId = idSys;
  CHECK(readCheck(gS, r, false, &e) == kStNotReady);
  r.mode = kModeDigest;
  CHECK(readCheck(gS, r, true, &e) == kStUnsupported);
  r.mode = 7;
  r.fileId = idSensor;
  CHECK(readCheck(gS, r, true, &e) == kStUnsupported);
  r.mode = kModeDigest;
  CHECK(readCheck(gS, r, true, &e) == kStOk);
  r.mode = kModeData;
  r.offset = 300;
  CHECK(readCheck(gS, r, true, &e) == kStOk);   // offset == end: one empty frame
  r.offset = 301;
  CHECK(readCheck(gS, r, true, &e) == kStPastEnd);
  findById(gS, idSensor)->base = 50;
  r.offset = 49;
  CHECK(readCheck(gS, r, true, &e) == kStTrimmed);
  r.offset = 50;
  CHECK(readCheck(gS, r, true, &e) == kStOk);
  Entry te = toListEntry(*findById(gS, idSensor), false);
  CHECK(te.start == 50 && te.end == 350 && (te.flags & kEfHeadTrimmed));
}

// ---------------------------------------------------------------------------
// Windows, digest, pacing
// ---------------------------------------------------------------------------

static void testWindows() {
  WindowPlan w = planWindow(0, 100000, 0, kDirectPayload, 0, false);
  CHECK(w.plainPerFrame == 182 && w.frames == 8 && w.plainBytes == 8 * 182 && !w.atEnd);
  w = planWindow(0, 100000, 0, kRoutedPayload, 0, false);
  CHECK(w.plainPerFrame == 126 && w.frames == 8);
  w = planWindow(0, 100000, 0, kDirectPayload, 0, true);
  CHECK(w.plainPerFrame == 154);
  w = planWindow(0, 100000, 0, kRoutedPayload, 0, true);
  CHECK(w.plainPerFrame == 98);
  // maxFrame: clamped to the path cap, with a 64 floor.
  CHECK(effectiveCap(kDirectPayload, 30) == 64 && effectiveCap(kDirectPayload, 100) == 100);
  CHECK(effectiveCap(kRoutedPayload, 250) == kRoutedPayload && effectiveCap(kDirectPayload, 0) == kDirectPayload);
  w = planWindow(0, 100000, 0, kDirectPayload, 30, false);
  CHECK(w.plainPerFrame == 44);
  w = planWindow(0, 100000, 0, kDirectPayload, 30, true);
  CHECK(w.plainPerFrame == 16);
  w = planWindow(0, 100000, 0, kDirectPayload, 100, false);
  CHECK(w.plainPerFrame == 80);
  // maxBytes, the end, offset == end, offset > end.
  w = planWindow(10, 100000, 10, kDirectPayload, 0, false);
  CHECK(w.frames == 1 && w.plainBytes == 10 && !w.atEnd);
  w = planWindow(1000, 1200, 0, kDirectPayload, 0, false);
  CHECK(w.frames == 2 && w.plainBytes == 200 && w.atEnd);
  w = planWindow(1200, 1200, 0, kDirectPayload, 0, false);
  CHECK(w.frames == 1 && w.plainBytes == 0 && w.atEnd);
  w = planWindow(1201, 1200, 0, kDirectPayload, 0, false);
  CHECK(w.frames == 0);
  // Frames of one window are contiguous.
  w = planWindow(1000, 1400, 0, kDirectPayload, 0, false);
  uint32_t off = 0, expect = 1000, total = 0;
  uint16_t len = 0;
  for (uint8_t k = 0; k < w.frames; ++k) {
    windowFrame(w, 1000, k, &off, &len);
    CHECK(off == expect && len > 0);
    expect += len;
    total += len;
  }
  CHECK(total == 400 && expect == 1400 && w.frames == 3);
  CHECK(sealedPlainLen(154 + 28) == 154 && sealedPlainLen(28) == 0);

  DigestPlan d = {};
  CHECK(planDigest(0, kDigestMaxBytes, 0, 5u << 20, &d) && d.from == 0 && d.to == kDigestMaxBytes && d.blocks == 256);
  CHECK(!planDigest(0, kDigestMaxBytes + 1, 0, 5u << 20, &d));
  CHECK(!planDigest(0, 0, 0, kDigestMaxBytes + 1, &d));                  // to the end, too far
  CHECK(planDigest(100, 0, 0, 5000, &d) && d.to == 5000 && d.blocks == 2);
  CHECK(planDigest(100, 0xFFFFFFF0u, 0, 5000, &d) && d.to == 5000);   // no wrap
  CHECK(planDigest(5000, 0, 0, 5000, &d) && d.blocks == 0);
  CHECK(!planDigest(10, 0, 20, 5000, &d) && !planDigest(5001, 0, 0, 5000, &d));
  uint32_t bf = 0, bl = 0;
  CHECK(planDigest(100, 0, 0, 5000, &d));
  digestBlock(d, 0, &bf, &bl);
  CHECK(bf == 100 && bl == 4096);
  digestBlock(d, 1, &bf, &bl);
  CHECK(bf == 4196 && bl == 804);
}

static ServePacer gP;   // zero-filled = empty, as in PSRAM on the device
static const uint8_t kA[6] = { 0x02, 0xA0, 0, 0, 0, 1 };
static const uint8_t kB[6] = { 0x02, 0xB0, 0, 0, 0, 1 };

static ServePacer::Decision listReq(const uint8_t* mac, uint16_t sid, uint32_t from, uint32_t now) {
  return gP.onRequest(mac, sid, kOpListReq, from, now, false, 0, true, false, false, 0);
}
static ServePacer::Decision readReq(const uint8_t* mac, uint32_t now) {
  return gP.onRequest(mac, 1, kOpReadReq, 0, now, false, 0, true, false, false, 0);
}

static void testPacer() {
  using V = ServePacer::Verdict;
  std::memset(&gP, 0, sizeof(gP));
  // Not settled; battery floor only when known, valid and discharging.
  ServePacer::Decision d = gP.onRequest(kA, 1, kOpReadReq, 0, 0, false, 0, false, true, true, 5);
  CHECK(d.v == V::Busy && d.retryAfterS == 2);
  d = gP.onRequest(kA, 1, kOpReadReq, 0, 0, false, 0, true, true, true, 19);
  CHECK(d.v == V::Busy && d.retryAfterS == 600);
  std::memset(&gP, 0, sizeof(gP));
  CHECK(gP.onRequest(kA, 1, kOpReadReq, 0, 0, false, 0, true, true, true, 20).v == V::Go);
  std::memset(&gP, 0, sizeof(gP));
  CHECK(gP.onRequest(kA, 1, kOpReadReq, 0, 0, false, 0, true, true, false, 5).v == V::Go);
  std::memset(&gP, 0, sizeof(gP));
  CHECK(gP.onRequest(kA, 1, kOpReadReq, 0, 0, false, 0, true, false, true, 5).v == V::Go);
  std::memset(&gP, 0, sizeof(gP));
  CHECK(gP.onRequest(kA, 1, kOpReadReq, 0, 0, false, 0, true, true, true, 255).v == V::Go);   // invalid %

  // One requester, a 10 s lease renewed by its requests and windows.
  std::memset(&gP, 0, sizeof(gP));
  CHECK(readReq(kA, 1000).v == V::Go && gP.isActiveRequester(kA, 1000));
  d = readReq(kB, 5000);
  CHECK(d.v == V::Busy && d.retryAfterS == 10);
  gP.noteWindowDone(kA, 8000);
  CHECK(readReq(kB, 17000).v == V::Busy);
  gP.expire(17999);
  CHECK(gP.activeValid);
  gP.expire(18000);
  CHECK(!gP.activeValid);
  CHECK(readReq(kB, 18000).v == V::Go);
  CHECK(readReq(kB, 18100).v == V::Go);   // the same requester again
  gP.release();
  CHECK(readReq(kA, 18200).v == V::Go);
  // Slot busy and a full tx queue.
  d = gP.onRequest(kA, 1, kOpReadReq, 0, 18300, true, 0, true, false, false, 0);
  CHECK(d.v == V::Busy && d.retryAfterS == 1);
  d = gP.onRequest(kA, 1, kOpReadReq, 0, 18300, false, kTxDepthLimit, true, false, false, 0);
  CHECK(d.v == V::Busy && d.retryAfterS == 1);
  CHECK(gP.onRequest(kA, 1, kOpReadReq, 0, 18300, false, kTxDepthLimit - 1, true, false, false, 0).v == V::Go);

  // A full LIST once per 60 s per MAC, whatever the sessionId.
  std::memset(&gP, 0, sizeof(gP));
  CHECK(listReq(kA, 1, 0, 1000).v == V::Go);
  d = listReq(kA, 2, 0, 31000);
  CHECK(d.v == V::Busy && d.retryAfterS == 30);
  d = listReq(kA, 3, 0, 60999);
  CHECK(d.v == V::Busy && d.retryAfterS == 1);
  CHECK(listReq(kA, 4, 0, 61000).v == V::Go);
  // Pages: at least 50 ms apart, at most 64 in a run (the full LIST counts).
  uint32_t t = 61000;
  d = listReq(kA, 4, 7, t + 10);
  CHECK(d.v == V::Busy && d.retryAfterS == 1);
  for (int i = 2; i <= kMaxPagesPerRun; ++i) {
    t += kPageMinMs;
    CHECK(listReq(kA, 4, uint32_t(i), t).v == V::Go);
  }
  t += kPageMinMs;
  d = listReq(kA, 4, 99, t);
  CHECK(d.v == V::Busy && d.retryAfterS == 60);
  // A new run resets the page count.
  t = 61000 + kFullListMinMs;
  CHECK(listReq(kA, 4, 0, t).v == V::Go && listReq(kA, 4, 2, t + kPageMinMs).v == V::Go);
  // Another MAC has its own full-list clock (after A's lease ends).
  CHECK(listReq(kB, 1, 0, t + kPageMinMs + kRequesterIdleMs).v == V::Go);

  // Digest once per file per session; the ring holds 8.
  std::memset(&gP, 0, sizeof(gP));
  CHECK(gP.digestAllowed(kA, 1, 5) && !gP.digestAllowed(kA, 1, 5));
  CHECK(gP.digestAllowed(kA, 2, 5) && gP.digestAllowed(kA, 1, 6) && gP.digestAllowed(kB, 1, 5));
  for (uint32_t f = 100; f < 108; ++f) CHECK(gP.digestAllowed(kA, 1, f));
  CHECK(gP.digestAllowed(kA, 1, 5));   // pushed out of the ring

  // A refusal is answered once per session, then dropped silently.
  std::memset(&gP, 0, sizeof(gP));
  CHECK(gP.refusal(kA, 7).v == V::Refuse);
  CHECK(gP.refusal(kA, 7).v == V::DropSilently && !gP.refusalAnsweredOnce(kA, 7));
  CHECK(gP.refusal(kA, 8).v == V::Refuse);
  CHECK(gP.refusal(kB, 8).v == V::Refuse);

  // The peer table recycles the least recently used of 16.
  std::memset(&gP, 0, sizeof(gP));
  uint8_t mac[6] = { 0x02, 0xC0, 0, 0, 0, 0 };
  for (uint8_t i = 0; i < kPacerPeers; ++i) {
    mac[5] = i;
    CHECK(gP.refusalAnsweredOnce(mac, 1));
  }
  mac[5] = 0;
  CHECK(!gP.refusalAnsweredOnce(mac, 1));   // 0 is now the most recent
  mac[5] = 99;
  CHECK(gP.refusalAnsweredOnce(mac, 1));    // evicts 1
  mac[5] = 0;
  CHECK(!gP.refusalAnsweredOnce(mac, 1));
  mac[5] = 1;
  CHECK(gP.refusalAnsweredOnce(mac, 1));
}

// ---------------------------------------------------------------------------
// AAD, sealed record, names
// ---------------------------------------------------------------------------

static void testSealAndNames() {
  uint8_t aad[kAadLen];
  CHECK(buildSealAad(aad, 0x11223344u, 5, 0x100, 154, 6) == 22);
  const uint8_t goldAad[22] = { 'H', 'W', '1', 'C', 'U', '1', 0x44, 0x33, 0x22, 0x11, 5, 0, 0, 0,
                                0x00, 0x01, 0, 0, 0x9A, 0, 6, 1 };
  CHECK(bytesEq(aad, goldAad, 22));

  SealedRecord r = sealedRecordFor(7, 0x11223344u, 5, 0x100, 10);
  CHECK(r.recLen == 58);
  uint8_t h[kSealedRecordHeaderLen];
  CHECK(packSealedRecordHeader(r, h) == 20);
  const uint8_t goldRec[20] = { 1, 7, 0x3A, 0, 0x44, 0x33, 0x22, 0x11, 5, 0, 0, 0, 0x00, 0x01, 0, 0, 10, 0, 0, 0 };
  CHECK(bytesEq(h, goldRec, 20));
  SealedRecord p = {};
  CHECK(parseSealedRecordHeader(h, 20, &p) && p.kind == 7 && p.plainLen == 10 && p.offset == 0x100 &&
        p.ledgerId == 0x11223344u && p.fileId == 5 && p.recLen == 58);
  CHECK(!parseSealedRecordHeader(h, 19, &p));
  uint8_t bad[20];
  std::memcpy(bad, h, 20);
  bad[2] = 57;
  CHECK(!parseSealedRecordHeader(bad, 20, &p));
  std::memcpy(bad, h, 20);
  bad[0] = 2;
  CHECK(!parseSealedRecordHeader(bad, 20, &p));
  CHECK(std::memcmp(kSealedSegMagic, "HW1CSG1\n", 8) == 0);
  CHECK(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u);

  char m[13];
  const uint8_t mac[6] = { 0xA0, 0xB1, 0xC2, 0xD3, 0xE4, 0xF5 };
  mac12(mac, m);
  CHECK(is(m, "a0b1c2d3e4f5"));

  char n[48];
  CHECK(localSegmentName(0x1A2B3C4Du, 12, 0, false, n, sizeof(n)) == 24 && is(n, "1a2b3c4d-0000000c-s0.raw"));
  CHECK(localSegmentName(0xFFFFFFFFu, 0, 65535, true, n, sizeof(n)) == 31 && is(n, "ffffffff-00000000-s65535.sealed"));
  CHECK(localSegmentName(1, 2, 3, false, n, 24) == 0 && n[0] == 0);
  CHECK(localSegmentName(1, 2, 3, false, n, 25) == 24);

  char out[64];
  CHECK(displayName("a\"b\\c", 5, out, sizeof(out)) == 7 && is(out, "a\\\"b\\\\c"));
  CHECK(displayName("x\ny", 3, out, sizeof(out)) == 6 && is(out, "x\\x0ay"));
  const char del[] = { 'a', 0x7F, 0 };
  CHECK(displayName(del, 2, out, sizeof(out)) == 5 && is(out, "a\\x7f"));
  CHECK(displayName("caf\xC3\xA9", 5, out, sizeof(out)) == 5 && is(out, "caf\xC3\xA9"));
  CHECK(displayName("\xE2\x82\xAC\xF0\x9F\x98\x80", 7, out, sizeof(out)) == 7);
  CHECK(displayName("a\xC3", 2, out, sizeof(out)) == 5 && is(out, "a\\xc3"));
  CHECK(displayName("\xC0\x80", 2, out, sizeof(out)) == 8 && is(out, "\\xc0\\x80"));             // overlong
  CHECK(displayName("\xED\xA0\x80", 3, out, sizeof(out)) == 12 && is(out, "\\xed\\xa0\\x80"));  // surrogate
  CHECK(displayName("\xF4\x90\x80\x80", 4, out, sizeof(out)) == 16);                             // past U+10FFFF
  const char nul[] = { 'a', 0, 'b' };
  CHECK(displayName(nul, 3, out, sizeof(out)) == 6 && is(out, "a\\x00b"));
  // Never splits an escape or a character at the cap.
  CHECK(displayName("\"\"", 2, out, 4) == 2 && is(out, "\\\""));
  CHECK(displayName("ab\xC3\xA9", 4, out, 4) == 2 && is(out, "ab"));
  CHECK(displayName("abc", 3, out, 1) == 0 && out[0] == 0);
  CHECK(displayName("../../system/users.json", 23, out, sizeof(out)) == 23);   // display only, never a path
}

int main() {
  testListReq();
  testReadReq();
  testReadResp();
  testListResp();
  testEntryWalk();
  testBusyAndStatus();
  testReader();
  testKinds();
  testRotationChain();
  testAnchorPromotion();
  testEqualHeadsNeverSwap();
  testTrimAndRestart();
  testBaseOverflow();
  testCardOut();
  testHorizonAndFullLedger();
  testDeliveredAndEndSum();
  testListAndRead();
  testWindows();
  testPacer();
  testSealAndNames();
  std::printf("espnow_catchup_core_tests: %u checks passed\n", gChecks);
  return 0;
}
