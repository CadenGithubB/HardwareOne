// System_ESPNow_CatchupCore.h - file catch-up (CATCHUP_*, opcodes 82-85): the
// pure rules both sides share
//
// A worker (the device) keeps a ledger of the files it offers: sensor
// captures, events.log, battery.csv, sealed health, errors and crash history,
// the system log and saved transcripts. A named master or backup (the
// collector) lists that ledger and reads byte ranges of it by key alone. A
// byte range counts as delivered once ONE named master has stored it (owner
// decision Q8, 2026-11-19): the device keeps one `delivered` offset per file.
//
// This header holds everything both sides compute the same way:
//   * the four frames, byte by byte (pack and parse; headerLen, entryLen and
//     entryFixed let a later version grow them at the end, minLen never rises);
//   * who may read (readerAllowed) and its answer status;
//   * the kinds, the whitelist of paths, the sealing rule and the horizon;
//   * the ledger: stable file ids across rotation, trims, restarts, renames
//     by TimeAnchors and a card that is taken out (reconcile);
//   * the LIST page and READ checks over a ledger;
//   * read windows, the digest range and the device's pacing;
//   * the sealing AAD and the collector's on-card sealed record;
//   * the collector's safe local names and one-line display of device names.
//
// Pure: no heap, no OS calls, no logging; only <stdint.h>, <stddef.h> and
// <string.h>. Time is passed in. All state lives in caller-owned structs that
// are valid when zero-filled (EXT_RAM_BSS_ATTR in PSRAM on the device).
// Host test: test/host/test_espnow_catchup_core.cpp. Spec:
// BACKCAPTURE_DESIGN.md section 5 and FILES_IMPL_SPEC.md sections 2-3.
//
// System_ESPNow_Wire.h states the packed structs and asserts that the opcode,
// size and flag constants here are its numbers (phase 0 of the spec).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace espnowcatchup {

// ---------------------------------------------------------------------------
// 1. Constants
// ---------------------------------------------------------------------------

constexpr uint8_t  kOpListReq = 82, kOpListResp = 83, kOpReadReq = 84, kOpReadResp = 85;
constexpr size_t   kListReqLen = 16, kListRespHeaderLen = 24, kEntryFixedLen = 24;
constexpr size_t   kReadReqLen = 24, kReadRespHeaderLen = 20, kNameMax = 96;
constexpr size_t   kRetryAfterLen = 2;                // u16 after the header of a busy answer
constexpr uint16_t kDirectPayload = 202, kRoutedPayload = 146, kMinFrame = 64;
constexpr size_t   kSealNonce = 12, kSealTag = 16, kSealOverhead = 28;
constexpr uint8_t  kWindowMaxFrames = 8;
constexpr uint32_t kFrameSpacingMs = 20;           // device: between frames of a window
constexpr uint32_t kSendRetryMs = 500;             // device: retry one frame this long, then abandon the window
constexpr uint8_t  kTxDepthLimit = 16;             // device: send only while espnowtx::queueDepth() < 16
constexpr uint32_t kFullListMinMs = 60000;         // per peer MAC, whatever its sessionId
constexpr uint32_t kPageMinMs = 50;                // between pages of one run
constexpr uint8_t  kMaxPagesPerRun = 64;
constexpr uint32_t kReconcileMinMs = 30000;
constexpr uint32_t kRequesterIdleMs = 10000;       // one active requester; released after 10 s idle
constexpr uint32_t kGrowingRecentMs = 120000;      // "growing": the writer's path, or grew within 120 s
constexpr uint32_t kDigestMaxBytes = 1u << 20, kDigestBlock = 4096;
constexpr size_t   kDigestLen = 16;                // SHA-256 truncated
constexpr uint8_t  kBatteryFloorPct = 20;
constexpr uint16_t kBatteryRetryS = 600;
constexpr uint16_t kHeadMax = 512;
constexpr uint8_t  kHorizonTotal = 32;
constexpr uint8_t  kLedgerMax = 40;                // horizon + slack (entries kept while the card is out)
constexpr size_t   kPathMax = 128;
constexpr size_t   kObservedMax = 96;              // reconcile looks at most this many observed files
constexpr uint32_t kBaseLimit = 0x80000000u;       // a base at or past 2^31 starts a new id at base 0
constexpr uint8_t  kPacerPeers = 16, kDigestRing = 8;

// Status byte (rule F3). 0-15 are the shared codes of System_ESPNow_AnswerCore.h;
// the test asserts they are its numbers. 16 and up belong to the frame.
constexpr uint8_t kStOk = 0, kStBusy = 1, kStRefused = 2, kStUnsupported = 3, kStTooLarge = 4,
                  kStBadRequest = 6, kStNotReady = 7, kStIoError = 8;
constexpr uint8_t kStNoLink = 16;                      // LIST_RESP: link != 0 (bond proxy is package 6)
constexpr uint8_t kStGone = 16, kStTrimmed = 17, kStPastEnd = 18;   // READ_RESP

// Session capability bits this core reads (System_ESPNow_SessionCapsCore.h
// names them; the test asserts the numbers).
constexpr uint32_t kCapCollect = 1u << 4, kCapFilesServe = 1u << 9;

// LIST_RESP header flags.
constexpr uint8_t kLfAnySealed = 0x01;       // a listed file is sealed (at rest or on send)
constexpr uint8_t kLfWithheld = 0x02;        // files withheld (unsealed health, or no capture key)
constexpr uint8_t kLfBeyondHorizon = 0x04;   // older files exist beyond the 32-file horizon
// LIST entry flags.
constexpr uint8_t kEfSealedAtRest = 0x01, kEfGrowing = 0x02, kEfHeadTrimmed = 0x04, kEfOnSd = 0x08,
                  kEfNameOmitted = 0x10, kEfSealedOnSend = 0x20;
// READ_REQ mode and flags.
constexpr uint8_t kModeData = 0, kModeDigest = 1;
constexpr uint8_t kRqStoredBefore = 0x01;    // "I have stored every byte before `offset`" (Q8 claim)
// READ_RESP flags.
constexpr uint8_t kRfLast = 0x01, kRfAtEnd = 0x02, kRfDigest = 0x04, kRfGrowing = 0x08,
                  kRfSealedOnSend = 0x10;

// ---------------------------------------------------------------------------
// 2. Little-endian helpers
// ---------------------------------------------------------------------------

namespace le {
inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
inline uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
}  // namespace le

// CRC-32 (IEEE, reflected, the zlib value). The ledger's head fingerprint;
// crc32("123456789") == 0xCBF43926. Bitwise: heads are at most 512 bytes.
inline uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int b = 0; b < 8; ++b) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// ---------------------------------------------------------------------------
// 3. Frames
// ---------------------------------------------------------------------------

// 82 CATCHUP_LIST_REQ (16 B, minLen 16): 0 reqId, 4 fromFileId, 8 kindMask,
// 10 link, 11 maxEntries, 12 maxFrame, 13-15 reserved 0.
struct ListReq {
  uint32_t reqId, fromFileId;
  uint16_t kindMask;          // bit (kind-1); 0 = all
  uint8_t  link, maxEntries, maxFrame;
};
// 83 CATCHUP_LIST_RESP header (24 B, minLen 24): 0 reqId, 4 status, 5 link,
// 6 entryCount, 7 headerLen, 8 nextFileId, 12 ledgerGen, 16 ledgerId,
// 20 bootTag, 22 flags, 23 entryFixed.
struct ListRespHeader {
  uint32_t reqId;
  uint8_t  status, link, entryCount, headerLen;
  uint32_t nextFileId, ledgerGen, ledgerId;
  uint16_t bootTag;
  uint8_t  flags, entryFixed;
};
// A LIST entry: 0 entryLen, 1 kind, 2 flags, 3 nameLen, 4 fileId, 8 start,
// 12 end, 16 createdBoot, 20 delivered, entryFixed.. name (no NUL).
struct Entry {
  uint8_t  kind, flags, nameLen;
  uint32_t fileId, start, end, createdBoot, delivered;
  const char* name;           // parse: points into the frame (nameLen bytes, no NUL);
                              // pack: a NUL-terminated string of the caller (nameLen ignored)
};
// 84 CATCHUP_READ_REQ (24 B, minLen 24): 0 reqId, 4 fileId, 8 offset,
// 12 maxBytes, 16 link, 17 mode, 18 maxFrame, 19 flags, 20 ledgerId.
struct ReadReq {
  uint32_t reqId, fileId, offset, maxBytes;
  uint8_t  link, mode, maxFrame, flags;
  uint32_t ledgerId;
};
// 85 CATCHUP_READ_RESP header (20 B, minLen 20): 0 reqId, 4 status, 5 flags,
// 6 link, 7 headerLen, 8 fileId, 12 offset, 16 end; data from headerLen.
struct ReadRespHeader {
  uint32_t reqId;
  uint8_t  status, flags, link, headerLen;
  uint32_t fileId, offset, end;
};

// Structural parse only: reqId 0 is returned as is (a receiver drops it).
inline bool parseListReq(const uint8_t* p, size_t n, ListReq* out) {
  if (!p || !out || n < kListReqLen) return false;
  out->reqId = le::get32(p);
  out->fromFileId = le::get32(p + 4);
  out->kindMask = le::get16(p + 8);
  out->link = p[10];
  out->maxEntries = p[11];
  out->maxFrame = p[12];
  return true;   // bytes 13.. reserved or later fields: ignored
}

inline size_t packListReq(const ListReq& r, uint8_t* out, size_t cap) {
  if (!out || cap < kListReqLen) return 0;
  memset(out, 0, kListReqLen);
  le::put32(out, r.reqId);
  le::put32(out + 4, r.fromFileId);
  le::put16(out + 8, r.kindMask);
  out[10] = r.link;
  out[11] = r.maxEntries;
  out[12] = r.maxFrame;
  return kListReqLen;
}

// Writes the 24 header bytes as given (headerLen and entryFixed included).
inline void writeListRespHeader(const ListRespHeader& h, uint8_t* out) {
  le::put32(out, h.reqId);
  out[4] = h.status;
  out[5] = h.link;
  out[6] = h.entryCount;
  out[7] = h.headerLen;
  le::put32(out + 8, h.nextFileId);
  le::put32(out + 12, h.ledgerGen);
  le::put32(out + 16, h.ledgerId);
  le::put16(out + 20, h.bootTag);
  out[22] = h.flags;
  out[23] = h.entryFixed;
}

inline size_t boundedLen(const char* s, size_t max) {
  size_t n = 0;
  if (!s) return 0;
  while (n <= max && s[n]) ++n;
  return n;   // max + 1 means "longer than max"
}

// Packs the header and as many entries as fit min(maxPayload, cap) and
// maxEntries (0 = no count limit). Sets headerLen 24, entryFixed 24 and
// entryCount itself. An entry whose name is longer than 96 bytes goes out with
// nameLen 0 and kEfNameOmitted, never cut. When not even the first entry fits
// with its name (a small maxFrame), it goes out without its name, so a page
// always makes progress. nextFileId = the fileId of the first entry not
// packed, or h.nextFileId when all n were packed (the caller passes the id
// that follows them, or 0 for the end of the list). Returns the bytes
// written (0 when the header does not fit); *packed = entries written.
inline size_t packListResp(const ListRespHeader& h, const Entry* e, size_t n, uint16_t maxPayload,
                           uint8_t maxEntries, uint8_t* out, size_t cap, size_t* packed) {
  size_t k = 0;
  if (packed) *packed = 0;
  const size_t limit = cap < maxPayload ? cap : maxPayload;
  if (!out || limit < kListRespHeaderLen || (n && !e)) return 0;
  size_t at = kListRespHeaderLen;
  while (k < n && k < 255 && (maxEntries == 0 || k < maxEntries)) {
    size_t len = boundedLen(e[k].name, kNameMax);
    bool omitted = len > kNameMax;
    if (omitted) len = 0;
    if (at + kEntryFixedLen + len > limit) {
      if (k == 0 && len && at + kEntryFixedLen <= limit) { omitted = true; len = 0; }
      else break;
    }
    uint8_t* q = out + at;
    q[0] = uint8_t(kEntryFixedLen + len);
    q[1] = e[k].kind;
    q[2] = uint8_t((e[k].flags & ~kEfNameOmitted) | (omitted ? kEfNameOmitted : 0));
    q[3] = uint8_t(len);
    le::put32(q + 4, e[k].fileId);
    le::put32(q + 8, e[k].start);
    le::put32(q + 12, e[k].end);
    le::put32(q + 16, e[k].createdBoot);
    le::put32(q + 20, e[k].delivered);
    if (len) memcpy(q + kEntryFixedLen, e[k].name, len);
    at += kEntryFixedLen + len;
    ++k;
  }
  ListRespHeader hh = h;
  hh.entryCount = uint8_t(k);
  hh.headerLen = uint8_t(kListRespHeaderLen);
  hh.entryFixed = uint8_t(kEntryFixedLen);
  hh.nextFileId = k < n ? e[k].fileId : h.nextFileId;
  writeListRespHeader(hh, out);
  if (packed) *packed = k;
  return at;
}

// False if n < 24, headerLen < 24 or headerLen > n.
inline bool parseListRespHeader(const uint8_t* p, size_t n, ListRespHeader* out) {
  if (!p || !out || n < kListRespHeaderLen) return false;
  if (p[7] < kListRespHeaderLen || p[7] > n) return false;
  out->reqId = le::get32(p);
  out->status = p[4];
  out->link = p[5];
  out->entryCount = p[6];
  out->headerLen = p[7];
  out->nextFileId = le::get32(p + 8);
  out->ledgerGen = le::get32(p + 12);
  out->ledgerId = le::get32(p + 16);
  out->bootTag = le::get16(p + 20);
  out->flags = p[22];
  out->entryFixed = p[23];
  return true;
}

// Walks the entries after headerLen. Stops (for good) on entryLen 0,
// entryFixed < 24, entryLen < entryFixed + nameLen, nameLen > 96, an overrun
// of the frame, or after entryCount entries; never loops. Bytes of an entry
// past entryFixed + nameLen (a later version's fields) are skipped.
struct EntryCursor {
  const uint8_t* p;
  size_t n, at;
  uint8_t left, fixed;
  bool next(Entry* out) {
    if (!left || !p || !out) return false;
    const uint8_t remaining = left;
    left = 0;   // a malformed entry stops the walk for good
    if (fixed < kEntryFixedLen || at >= n) return false;
    const uint8_t* q = p + at;
    const size_t len = q[0];
    if (len == 0 || len > n - at || len < fixed) return false;
    const uint8_t nameLen = q[3];
    if (nameLen > kNameMax || len < size_t(fixed) + nameLen) return false;
    out->kind = q[1];
    out->flags = q[2];
    out->nameLen = nameLen;
    out->fileId = le::get32(q + 4);
    out->start = le::get32(q + 8);
    out->end = le::get32(q + 12);
    out->createdBoot = le::get32(q + 16);
    out->delivered = le::get32(q + 20);
    out->name = reinterpret_cast<const char*>(q + fixed);
    at += len;
    left = uint8_t(remaining - 1);
    return true;
  }
};
// `h` must come from parseListRespHeader on the same p, n.
inline EntryCursor entries(const uint8_t* p, size_t n, const ListRespHeader& h) {
  EntryCursor c;
  c.p = p; c.n = n; c.at = h.headerLen; c.fixed = h.entryFixed; c.left = h.entryCount;
  return c;
}

inline bool parseReadReq(const uint8_t* p, size_t n, ReadReq* out) {
  if (!p || !out || n < kReadReqLen) return false;
  out->reqId = le::get32(p);
  out->fileId = le::get32(p + 4);
  out->offset = le::get32(p + 8);
  out->maxBytes = le::get32(p + 12);
  out->link = p[16];
  out->mode = p[17];
  out->maxFrame = p[18];
  out->flags = p[19];
  out->ledgerId = le::get32(p + 20);
  return true;
}

inline size_t packReadReq(const ReadReq& r, uint8_t* out, size_t cap) {
  if (!out || cap < kReadReqLen) return 0;
  le::put32(out, r.reqId);
  le::put32(out + 4, r.fileId);
  le::put32(out + 8, r.offset);
  le::put32(out + 12, r.maxBytes);
  out[16] = r.link;
  out[17] = r.mode;
  out[18] = r.maxFrame;
  out[19] = r.flags;
  le::put32(out + 20, r.ledgerId);
  return kReadReqLen;
}

// Writes 20 bytes; headerLen is always written as 20.
inline size_t packReadRespHeader(const ReadRespHeader& h, uint8_t* out, size_t cap) {
  if (!out || cap < kReadRespHeaderLen) return 0;
  le::put32(out, h.reqId);
  out[4] = h.status;
  out[5] = h.flags;
  out[6] = h.link;
  out[7] = uint8_t(kReadRespHeaderLen);
  le::put32(out + 8, h.fileId);
  le::put32(out + 12, h.offset);
  le::put32(out + 16, h.end);
  return kReadRespHeaderLen;
}

// False if n < 20, headerLen < 20 or headerLen > n. *data = p + headerLen.
inline bool parseReadResp(const uint8_t* p, size_t n, ReadRespHeader* out,
                          const uint8_t** data, size_t* dataLen) {
  if (!p || !out || n < kReadRespHeaderLen) return false;
  if (p[7] < kReadRespHeaderLen || p[7] > n) return false;
  out->reqId = le::get32(p);
  out->status = p[4];
  out->flags = p[5];
  out->link = p[6];
  out->headerLen = p[7];
  out->fileId = le::get32(p + 8);
  out->offset = le::get32(p + 12);
  out->end = le::get32(p + 16);
  if (data) *data = p + out->headerLen;
  if (dataLen) *dataLen = n - out->headerLen;
  return true;
}

// A one-frame answer with no data: LIST_RESP (op 83, 24 B) or READ_RESP
// (op 85, 20 B) carrying `status`, reqId and link, every other field 0. Any
// other op: 0. For trimmed (17), which carries the new start in `offset`,
// use packReadRespHeader.
inline size_t packStatus(uint8_t op, uint32_t reqId, uint8_t link, uint8_t status,
                         uint8_t* out, size_t cap) {
  if (op == kOpListResp) {
    if (!out || cap < kListRespHeaderLen) return 0;
    ListRespHeader h = {};
    h.reqId = reqId; h.status = status; h.link = link;
    h.headerLen = uint8_t(kListRespHeaderLen); h.entryFixed = uint8_t(kEntryFixedLen);
    writeListRespHeader(h, out);
    return kListRespHeaderLen;
  }
  if (op == kOpReadResp) {
    ReadRespHeader h = {};
    h.reqId = reqId; h.status = status; h.link = link;
    return packReadRespHeader(h, out, cap);
  }
  return 0;
}

// busy (1) with u16 retryAfterS right after the header: 26 B (op 83) or 22 B (op 85).
inline size_t packBusy(uint8_t op, uint32_t reqId, uint8_t link, uint16_t retryAfterS,
                       uint8_t* out, size_t cap) {
  const size_t hl = op == kOpListResp ? kListRespHeaderLen : op == kOpReadResp ? kReadRespHeaderLen : 0;
  if (!hl || !out || cap < hl + kRetryAfterLen) return 0;
  if (!packStatus(op, reqId, link, kStBusy, out, cap)) return 0;
  le::put16(out + hl, retryAfterS);
  return hl + kRetryAfterLen;
}

// The retryAfterS of a busy answer: the 2 bytes at headerLen. 0 when absent.
inline uint16_t busyRetryAfter(const uint8_t* p, size_t n, uint8_t headerLen) {
  if (!p || size_t(headerLen) + kRetryAfterLen > n) return 0;
  return le::get16(p + headerLen);
}

// ---------------------------------------------------------------------------
// 4. Who may read
// ---------------------------------------------------------------------------

struct ReaderInputs {
  bool     built;                    // ENABLE_ESPNOW_SEND
  bool     serveOn;                  // gSettings.catchupServe
  uint8_t  link;                     // the request's link byte
  bool     bondWorkerInBondMode;     // gSettings.bondModeEnabled && isBondWorker()
  bool     requesterIsBondPartner, bondTokenValid;
  int      namedKind;                // espnowNamedMasterKind(src): 0 none, 1 primary, 2 backup
  bool     capsConfirmed;
  uint32_t requesterCaps;            // espnowPeerSessionCaps(src, &caps, &confirmed)
};
enum class Reader : uint8_t { Allowed, NotBuilt, SettingOff, NoLink, BondWorker, NotNamed, NotCollector };

// First match wins. Primary and backup are the same (Q9). Key-only, read-only
// (Q1): no credentials ride on these frames.
inline Reader readerAllowed(const ReaderInputs& in) {
  if (!in.built) return Reader::NotBuilt;
  if (!in.serveOn) return Reader::SettingOff;
  if (in.link != 0) return Reader::NoLink;
  // A bond worker serves nobody yet (owner Q1 and Q19, 2026-11-19): its
  // files reach a master through the bond-master proxy, package 6. The
  // partner and token inputs stay for that package.
  if (in.bondWorkerInBondMode) return Reader::BondWorker;
  if (in.namedKind == 0) return Reader::NotNamed;
  if (in.capsConfirmed && !(in.requesterCaps & kCapCollect)) return Reader::NotCollector;
  return Reader::Allowed;
}
// Allowed 0; NotBuilt 3 unsupported; NoLink 16 (no_link on LIST, gone on READ);
// every other refusal 2.
inline uint8_t readerStatus(Reader r) {
  switch (r) {
    case Reader::Allowed:  return kStOk;
    case Reader::NotBuilt: return kStUnsupported;
    case Reader::NoLink:   return kStNoLink;
    default:               return kStRefused;
  }
}
inline const char* readerName(Reader r) {
  switch (r) {
    case Reader::Allowed:      return "allowed";
    case Reader::NotBuilt:     return "not_built";
    case Reader::SettingOff:   return "setting_off";
    case Reader::NoLink:       return "no_link";
    case Reader::BondWorker:   return "bond_worker";
    case Reader::NotNamed:     return "not_named";
    case Reader::NotCollector: return "not_collector";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// 5. Kinds, the whitelist and the sealing rule
// ---------------------------------------------------------------------------

enum class Kind : uint8_t { None = 0, SensorCapture = 1, EventsLog = 2, BatteryLog = 3, Health = 4,
                            ErrorLogs = 5, SystemLog = 6, Transcript = 7 };
constexpr uint8_t kKindCount = 7;
enum class Seal : uint8_t { Plain = 0, AtRestOnly = 1, OnSend = 2 };   // AtRestOnly: serve only if sealed at rest
enum class Offer : uint8_t { Serve, WithheldUnsealedHealth, WithheldNoKey, Never };

constexpr uint8_t kNoSlot = 0xFF;
// Classified.group: the trim-note slot of a capped log (0-5), kNoSlot otherwise.
// Slots: 0 events.log, 1 errors.log, 2 crash-history.log on LittleFS; 3-5 the same on /sd.
struct Classified {
  Kind        kind;
  Seal        seal;
  bool        onSd;
  uint8_t     group;
  const char* rel;      // points into the classified path: the name relative to the kind's folder
};

inline const char* kindName(Kind k) {
  switch (k) {
    case Kind::SensorCapture: return "sensor";
    case Kind::EventsLog:     return "events";
    case Kind::BatteryLog:    return "battery";
    case Kind::Health:        return "health";
    case Kind::ErrorLogs:     return "errors";
    case Kind::SystemLog:     return "system_log";
    case Kind::Transcript:    return "transcript";
    default:                  return nullptr;
  }
}
inline uint16_t kindBit(Kind k) { return k == Kind::None ? 0 : uint16_t(1u << (uint8_t(k) - 1)); }
inline bool kindInMask(Kind k, uint16_t mask) { return mask == 0 || (mask & kindBit(k)) != 0; }

// Per-kind horizon caps; they sum to kHorizonTotal (32). Health has two
// sources with their own share of its 6 (horizonGroup below): the ring's
// history files (.r1h) and the health CSVs of the sensors folder, 3 each, so
// a month of daily health CSVs never pushes the ring history out (robustness
// review 2026-11-19).
inline uint8_t horizonCap(Kind k) {
  switch (k) {
    case Kind::SensorCapture: return 7;
    case Kind::EventsLog:     return 2;
    case Kind::BatteryLog:    return 3;
    case Kind::Health:        return 6;
    case Kind::ErrorLogs:     return 4;
    case Kind::SystemLog:     return 4;
    case Kind::Transcript:    return 6;
    default:                  return 0;
  }
}

namespace detail {
inline bool startsWith(const char* s, const char* pre) { return strncmp(s, pre, strlen(pre)) == 0; }
inline bool endsWithN(const char* s, size_t n, const char* suf) {
  const size_t m = strlen(suf);
  return n >= m && memcmp(s + n - m, suf, m) == 0;
}
inline bool endsWith(const char* s, const char* suf) { return endsWithN(s, strlen(s), suf); }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline const char* lastSlash(const char* s) { return strrchr(s, '/'); }
inline size_t countChar(const char* s, char c) {
  size_t k = 0;
  for (; *s; ++s) k += *s == c;
  return k;
}
inline bool contains(const uint8_t* h, size_t n, const char* needle) {
  const size_t m = strlen(needle);
  if (!h || m == 0 || n < m) return false;
  for (size_t i = 0; i + m <= n; ++i)
    if (memcmp(h + i, needle, m) == 0) return true;
  return false;
}
// Splits a trailing rotation suffix ".<k>" (1-2 digits, no leading zero) off
// a file name: *stemLen = the length before it, return k (0 = no suffix).
inline unsigned rotationIndex(const char* name, size_t n, size_t* stemLen) {
  *stemLen = n;
  size_t d = 0;
  while (d < n && d < 3 && isDigit(name[n - 1 - d])) ++d;
  if (d == 0 || d > 2 || d + 1 >= n || name[n - 1 - d] != '.' || name[n - d] == '0') return 0;
  unsigned k = 0;
  for (size_t i = n - d; i < n; ++i) k = k * 10 + unsigned(name[i] - '0');
  *stemLen = n - d - 1;
  return k;
}
// Canonical guarded-VFS form: absolute, no '\\', no empty, "." or ".." component,
// no trailing '/'.
inline bool canonical(const char* p) {
  if (!p || p[0] != '/' || !p[1]) return false;
  if (strchr(p, '\\')) return false;
  const char* c = p + 1;
  for (;;) {
    const char* e = strchr(c, '/');
    const size_t len = e ? size_t(e - c) : strlen(c);
    if (len == 0) return false;
    if (len == 1 && c[0] == '.') return false;
    if (len == 2 && c[0] == '.' && c[1] == '.') return false;
    if (!e) return true;
    c = e + 1;
  }
}
// system.log[.k] or system-<stamp>.log[.k]; stamp 1-40 of [A-Za-z0-9_-].
inline bool systemLogName(const char* name) {
  size_t stem = 0;
  rotationIndex(name, strlen(name), &stem);
  if (stem == 10 && memcmp(name, "system.log", 10) == 0) return true;
  if (stem < 7 + 1 + 4 || memcmp(name, "system-", 7) != 0 || memcmp(name + stem - 4, ".log", 4) != 0)
    return false;
  const size_t stampLen = stem - 7 - 4;
  if (stampLen > 40) return false;
  for (size_t i = 7; i < 7 + stampLen; ++i) {
    const char c = name[i];
    if (!(isDigit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-')) return false;
  }
  return true;
}
// "u<id>" exactly as TranscriptPathPolicy::classify accepts an Owner: u, a
// nonzero first digit, decimal, no overflow.
inline bool transcriptOwner(const char* s, size_t n) {
  if (n < 2 || s[0] != 'u' || s[1] < '1' || s[1] > '9') return false;
  uint32_t v = 0;
  for (size_t i = 1; i < n; ++i) {
    if (!isDigit(s[i])) return false;
    const unsigned d = unsigned(s[i] - '0');
    if (v > (UINT32_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  return true;
}
}  // namespace detail

// The six capped logs that leave trim notes, by exact canonical path.
inline uint8_t trimSlot(const char* path) {
  static const char* const kSlots[6] = {
    "/system/sys_logs/events.log", "/system/sys_logs/errors.log", "/system/sys_logs/crash-history.log",
    "/sd/system/sys_logs/events.log", "/sd/system/sys_logs/errors.log", "/sd/system/sys_logs/crash-history.log",
  };
  if (!path) return kNoSlot;
  for (uint8_t i = 0; i < 6; ++i)
    if (strcmp(path, kSlots[i]) == 0) return i;
  return kNoSlot;
}

// The whitelist (FILES_IMPL_SPEC 3.4). Everything else is kind None and is
// never offered: login logs, command-audit.log, system-events.log,
// i2c_errors.log, QuickNotes (/stt/u<id>/notes/...), /g2, tracks, recordings,
// /sd/STT Raw, /system state and keys, /espnow and /system/espnow.
namespace detail {
inline Classified classifyPathMatch(const char* path, bool sd) {
  Classified c = { Kind::None, Seal::Plain, sd, kNoSlot, nullptr };
  const char* t = sd ? path + 3 : path;   // starts with '/'
  const uint8_t slot = trimSlot(path);
  if (slot != kNoSlot) {
    c.kind = (slot % 3) == 0 ? Kind::EventsLog : Kind::ErrorLogs;
    // events.log holds login records (user and source) and remote command
    // lines (SYSEVT_LOGIN_*, SYSEVT_REMOTE_CMD_*): sealed on send like the
    // system log, never in clear (owner decision Q11).
    if (c.kind == Kind::EventsLog) c.seal = Seal::OnSend;
    c.group = slot;
    c.rel = detail::lastSlash(path) + 1;
    return c;
  }
  if (!sd && (strcmp(t, "/battery.csv") == 0 || strcmp(t, "/battery.csv.1") == 0 ||
              strcmp(t, "/battery.csv.2") == 0)) {
    c.kind = Kind::BatteryLog;
    c.rel = t + 1;
    return c;
  }
  static const char kSensors[] = "/logging_captures/sensors/";
  static const char kHealth[] = "/logging_captures/r1-health-history/";
  static const char kSystem[] = "/logging_captures/system/";
  static const char kStt[] = "/stt/";
  if (detail::startsWith(t, kSensors)) {
    const char* rest = t + sizeof(kSensors) - 1;
    if (detail::countChar(rest, '/') != 1) return c;
    const char* file = strchr(rest, '/') + 1;
    if (detail::endsWith(file, ".tmp") || detail::endsWith(file, ".bak")) return c;
    // B11: a file a card writer moved aside ("<name>.foreign-<n>": not this
    // device's sealed file, System_SealedLog.h) is not this device's capture
    // and is never offered as one.
    if (strstr(file, ".foreign-") != nullptr) return c;
    c.kind = Kind::SensorCapture;
    c.rel = rest;
    return c;
  }
  if (detail::startsWith(t, kHealth)) {
    const char* rest = t + sizeof(kHealth) - 1;
    if (strchr(rest, '/') || strlen(rest) <= 4 || !detail::endsWith(rest, ".r1h")) return c;
    c.kind = Kind::Health;
    c.seal = Seal::AtRestOnly;
    c.rel = rest;
    return c;
  }
  if (detail::startsWith(t, kSystem)) {
    const char* rest = t + sizeof(kSystem) - 1;
    if (strchr(rest, '/') || !detail::systemLogName(rest)) return c;
    c.kind = Kind::SystemLog;
    c.seal = Seal::OnSend;
    c.rel = rest;
    return c;
  }
  if (detail::startsWith(t, kStt)) {
    const char* rest = t + sizeof(kStt) - 1;
    const char* slash = strchr(rest, '/');
    if (!slash || strchr(slash + 1, '/')) return c;
    if (!detail::transcriptOwner(rest, size_t(slash - rest))) return c;
    const char* file = slash + 1;
    if (strlen(file) <= 4 || !detail::endsWith(file, ".txt")) return c;
    c.kind = Kind::Transcript;
    c.seal = Seal::OnSend;
    c.rel = rest;
    return c;
  }
  return c;
}
}  // namespace detail

inline Classified classifyPath(const char* path) {
  Classified none = { Kind::None, Seal::Plain, false, kNoSlot, nullptr };
  if (!detail::canonical(path)) return none;
  const Classified c = detail::classifyPathMatch(path, strncmp(path, "/sd/", 4) == 0);
  return c.kind == Kind::None ? none : c;
}

// The at-rest mark of CaptureCrypto (CAPCRYPT_MAGIC_PREFIX "#HW1ENC").
inline bool hasSealedMagic(const uint8_t* head, size_t n) {
  return head && n >= 7 && memcmp(head, "#HW1ENC", 7) == 0;
}

// Sensor captures only: decides health, sealed-as-stored and GPS from the
// first <= 512 bytes (`sealedMagic` = hasSealedMagic(head, n)). Plain is an
// allow-list; everything else fails closed (security review 2026-11-19):
//   health class: the file name starts with "health", or the head holds
//     "r1_" (a CSV column) or "r1:" (a TEXT row) -> Health, AtRestOnly;
//   sealed at rest -> Plain (served as stored);
//   a CSV header line ("timestamp_ms,...") that ends within the head:
//     with a "gps_" column -> OnSend (Q10 extended to files), else Plain;
//   a CSV header line that runs past the head (it may hide the r1_ columns,
//     which come last) -> Health, AtRestOnly (withheld unless sealed);
//   anything else (TEXT and TRACK captures, which can hold GPS fixes and ring
//     rows, or an unknown format), whatever its extension -> OnSend.
inline Classified refineWithHead(Classified c, const uint8_t* head, size_t n, bool sealedMagic) {
  if (c.kind != Kind::SensorCapture || !c.rel) return c;
  const char* slash = strchr(c.rel, '/');
  const char* file = slash ? slash + 1 : c.rel;
  if (detail::startsWith(file, "health") || detail::contains(head, n, "r1_") || detail::contains(head, n, "r1:")) {
    c.kind = Kind::Health;
    c.seal = Seal::AtRestOnly;
    return c;
  }
  if (sealedMagic) { c.seal = Seal::Plain; return c; }
  size_t line = 0;
  while (head && line < n && head[line] != '\n') ++line;
  const bool csvHeader = head && n >= 12 && memcmp(head, "timestamp_ms", 12) == 0;
  if (csvHeader && line >= n) {
    if (n >= kHeadMax) {   // the header line does not end within the head
      c.kind = Kind::Health;
      c.seal = Seal::AtRestOnly;
      return c;
    }
    c.seal = Seal::OnSend;   // a header still being written: decide again on a longer head
    return c;
  }
  if (!csvHeader) { c.seal = Seal::OnSend; return c; }
  c.seal = detail::contains(head, line, "gps_") ? Seal::OnSend : Seal::Plain;
  return c;
}

// AtRestOnly and not sealed -> withheld (never sent in clear, never sealed on
// send: Q12); OnSend without a key -> withheld (never falls back to plain).
inline Offer offer(Kind kind, Seal seal, bool sealedAtRest, bool keyReady) {
  if (kind == Kind::None) return Offer::Never;
  if (seal == Seal::AtRestOnly && !sealedAtRest) return Offer::WithheldUnsealedHealth;
  if (seal == Seal::OnSend && !keyReady) return Offer::WithheldNoKey;
  return Offer::Serve;
}
inline Offer offer(const Classified& c, bool sealedAtRest, bool keyReady) {
  return offer(c.kind, c.seal, sealedAtRest, keyReady);
}

namespace detail {
// r1h: <32 hex>-<10 digits>-<offset>.r1h; the 10-digit stamp, or false.
inline bool r1hStamp(const char* rel, uint32_t* stamp) {
  const char* dash = strchr(rel, '-');
  if (!dash) return false;
  uint64_t v = 0;
  int d = 0;
  for (const char* p = dash + 1; isDigit(*p) && d < 11; ++p, ++d) v = v * 10 + uint64_t(*p - '0');
  if (d != 10 || dash[11] != '-' || v > UINT32_MAX) return false;
  *stamp = uint32_t(v);
  return true;
}
}  // namespace detail

// Horizon order within a kind: true when rel name `a` is newer than `b`.
//   health .r1h: the 10-digit stamp descending; health files from the sensors
//     folder come before .r1h files;
//   everything else: the name without its rotation suffix descending (for a
//     sensor capture: the folder, then the file), then the base before .1, .2.
inline bool newerThan(Kind k, const char* a, const char* b) {
  if (k == Kind::Health) {
    const bool ra = detail::endsWith(a, ".r1h"), rb = detail::endsWith(b, ".r1h");
    if (ra != rb) return rb;
    uint32_t sa = 0, sb = 0;
    if (ra && detail::r1hStamp(a, &sa) && detail::r1hStamp(b, &sb) && sa != sb) return sa > sb;
  }
  size_t la = 0, lb = 0;
  const unsigned ka = detail::rotationIndex(a, strlen(a), &la);
  const unsigned kb = detail::rotationIndex(b, strlen(b), &lb);
  const size_t m = la < lb ? la : lb;
  const int c = memcmp(a, b, m);
  if (c != 0) return c > 0;
  if (la != lb) return la > lb;
  return ka < kb;
}

// The horizon group of a file of kind `k` named `rel`: 1 for a ring history
// file (.r1h), 0 for everything else. Files compete for the horizon only
// within their kind and group.
inline uint8_t horizonGroup(Kind k, const char* rel) {
  return k == Kind::Health && rel && detail::endsWith(rel, ".r1h") ? 1 : 0;
}
// The cap of one group: Health splits its 6 into 3 + 3.
inline uint8_t horizonGroupCap(Kind k, uint8_t group) {
  if (k == Kind::Health) return uint8_t(horizonCap(k) / 2 + (group ? 0 : horizonCap(k) % 2));
  return horizonCap(k);
}

// ---------------------------------------------------------------------------
// 6. The ledger (pure; System_CatchupLedger.cpp loads, walks and saves it)
// ---------------------------------------------------------------------------

enum class EntryState : uint8_t { Present = 0, MissingCardOut = 1, Gone = 2 };
// LedgerEntry.flags
constexpr uint8_t kLeSealedAtRest = 0x01, kLeOnSd = 0x02, kLeGrowSeen = 0x04;
// LedgerState.flags
constexpr uint8_t kLsBeyondHorizon = 0x01;

struct LedgerEntry {            // ~170 B; PSRAM array of kLedgerMax
  uint32_t fileId;
  uint8_t  kind, seal, flags, state;   // Kind, Seal, kLe*, EntryState
  char     path[kPathMax];             // canonical, NUL-terminated
  uint32_t base;                       // logical offset of physical byte 0
  uint16_t headLen;                    // bytes the head CRC covers (min(size, 512) when taken)
  uint32_t headCrc;
  uint32_t createdBoot, size, delivered, lastGrowMs;
};
struct LedgerState {
  uint32_t    ledgerId, gen, nextFileId;
  uint8_t     count, flags;
  LedgerEntry e[kLedgerMax];           // ascending fileId
};

// One file the walk saw. `head` (headLen bytes) is needed when needHead says
// so; it stays valid only for the reconcile call. `c` is classifyPath +
// refineWithHead of `path`; `sealedAtRest` = hasSealedMagic(head). For a
// known path whose head was not read, the entry's own kind, seal and
// sealed-at-rest mark are kept.
struct Observed {
  char           path[kPathMax];
  uint32_t       size;
  const uint8_t* head;
  uint16_t       headLen;
  bool           headRead;
  Classified     c;
  bool           sealedAtRest;
};
// A capped log trimmed or restarted since the last reconcile (5.3). The store
// takes and clears these under its mux before calling reconcile.
struct TrimNote { uint8_t slot; uint32_t drops; bool restarted; };

struct ReconcileEnv {
  uint32_t bootCounter;   // gBootCounter (createdBoot of new ids)
  uint32_t nowMs;
  bool     sdMounted;     // false: known /sd entries not seen become MissingCardOut
  bool     walkComplete;  // false (budget hit): known entries not seen are left as they are
};
struct ReconcileOut {
  bool    changed;        // gen stepped: the store writes the ledger
  uint8_t added;          // new ids for paths the ledger did not know
  uint8_t renamed;        // ids that moved to a new path (rotation, TimeAnchors)
  uint8_t trimmed;        // base shifts by a trim note
  uint8_t removed;        // entries removed (gone, replaced, left the horizon)
  uint8_t newIds;         // every new id issued (added + replaced files)
  uint8_t cardOut;        // entries now MissingCardOut
};

inline void ledgerInit(LedgerState& s, uint32_t ledgerId) {
  memset(&s, 0, sizeof(s));
  s.ledgerId = ledgerId ? ledgerId : 1;   // 0 means "none" to a collector
  s.gen = 1;
  s.nextFileId = 1;
}

inline uint32_t logicalEnd(const LedgerEntry& e) { return e.base + e.size; }
// false if logical < base or > end.
inline bool toPhysical(const LedgerEntry& e, uint32_t logical, uint32_t* phys) {
  if (logical < e.base || logical > logicalEnd(e)) return false;
  if (phys) *phys = logical - e.base;
  return true;
}
// delivered = max(delivered, min(claim, end)).
inline void noteDelivered(LedgerEntry& e, uint32_t claim) {
  const uint32_t end = logicalEnd(e);
  const uint32_t c = claim < end ? claim : end;
  if (c > e.delivered) e.delivered = c;
}
// The active path of its writer, or grown within the last 120 s.
inline bool growing(const LedgerEntry& e, bool isActiveWriterPath, uint32_t nowMs) {
  if (isActiveWriterPath) return true;
  return (e.flags & kLeGrowSeen) && uint32_t(nowMs - e.lastGrowMs) < kGrowingRecentMs;
}
inline Offer entryOffer(const LedgerEntry& e, bool keyReady) {
  return offer(Kind(e.kind), Seal(e.seal), (e.flags & kLeSealedAtRest) != 0, keyReady);
}
inline bool entryOffered(const LedgerEntry& e, bool keyReady) {
  return e.state == uint8_t(EntryState::Present) && entryOffer(e, keyReady) == Offer::Serve;
}
// Σ logicalEnd mod 2^32 over offered entries (package 1's notice compares it).
inline uint32_t endSum(const LedgerState& s, bool keyReady) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < s.count && i < kLedgerMax; ++i)
    if (entryOffered(s.e[i], keyReady)) sum += logicalEnd(s.e[i]);
  return sum;
}
inline const LedgerEntry* findByPath(const LedgerState& s, const char* path) {
  for (uint8_t i = 0; path && i < s.count && i < kLedgerMax; ++i)
    if (s.e[i].state != uint8_t(EntryState::Gone) && strcmp(s.e[i].path, path) == 0) return &s.e[i];
  return nullptr;
}
inline const LedgerEntry* findById(const LedgerState& s, uint32_t fileId) {
  for (uint8_t i = 0; fileId && i < s.count && i < kLedgerMax; ++i)
    if (s.e[i].fileId == fileId) return &s.e[i];
  return nullptr;
}
inline LedgerEntry* findById(LedgerState& s, uint32_t fileId) {
  return const_cast<LedgerEntry*>(findById(static_cast<const LedgerState&>(s), fileId));
}

namespace detail {
inline void dirOf(const char* p, size_t* len) {
  const char* s = strrchr(p, '/');
  *len = s ? size_t(s - p) : 0;
}
// Files that rotate by rename (X -> X.1): their size alone cannot show that
// another file took the path, so their head is read on every reconcile.
inline bool rotatesByRename(Kind k, const char* path) {
  switch (k) {
    case Kind::SystemLog: case Kind::BatteryLog: case Kind::SensorCapture: return true;
    case Kind::Health: return !endsWith(path, ".r1h");
    default: return false;
  }
}
// Known head == the first known.headLen bytes of the observed head.
inline bool headPrefixEqual(const LedgerEntry& k, const Observed& o) {
  if (!o.headRead || k.headLen > o.headLen) return false;
  return crc32(o.head, k.headLen) == k.headCrc;
}
inline void takeHead(LedgerEntry& e, const Observed& o) {
  if (!o.headRead) { e.headLen = 0; e.headCrc = crc32(nullptr, 0); return; }
  e.headLen = o.headLen;
  e.headCrc = crc32(o.head, o.headLen);
}
inline bool copyPath(char* dst, const char* src) {
  const size_t n = boundedLen(src, kPathMax - 1);
  if (n >= kPathMax) return false;
  memcpy(dst, src, n);
  dst[n] = 0;
  return true;
}
// Parses "<base>-boot<N>-<ms><ext>" the way TimeAnchors' parseBootFile does
// (the first "-boot"); ext is "" or starts with '.'.
inline bool bootFile(const char* name, size_t* baseLen, uint32_t* boot, uint32_t* ms, const char** ext) {
  const char* tag = strstr(name, "-boot");
  if (!tag || tag == name) return false;
  const char* p = tag + 5;
  uint64_t b = 0, m = 0;
  int d = 0;
  for (; isDigit(*p) && d < 11; ++p, ++d) b = b * 10 + uint64_t(*p - '0');
  if (d == 0 || *p != '-' || b > UINT32_MAX) return false;
  ++p;
  d = 0;
  for (; isDigit(*p) && d < 11; ++p, ++d) m = m * 10 + uint64_t(*p - '0');
  if (d == 0 || (*p != '.' && *p != 0) || m > UINT32_MAX) return false;
  *baseLen = size_t(tag - name);
  *boot = uint32_t(b);
  *ms = uint32_t(m);
  *ext = p;
  return true;
}
}  // namespace detail

// X -> X.k (k >= 1) or X.j -> X.k (k > j): same folder, so the same tier.
// More than one step is allowed (several rotations between two reconciles);
// the equal head is still required by reconcile.
inline bool isRotationSibling(const char* oldPath, const char* newPath) {
  if (!oldPath || !newPath) return false;
  size_t da = 0, db = 0;
  detail::dirOf(oldPath, &da);
  detail::dirOf(newPath, &db);
  if (da != db || memcmp(oldPath, newPath, da) != 0) return false;
  const char* a = oldPath + da + 1;
  const char* b = newPath + db + 1;
  size_t sa = 0, sb = 0;
  const unsigned ka = detail::rotationIndex(a, strlen(a), &sa);
  const unsigned kb = detail::rotationIndex(b, strlen(b), &sb);
  return sa == sb && sa > 0 && memcmp(a, b, sa) == 0 && kb > ka;
}

// TimeAnchors: <T>/logging_captures/sensors/boot-<N>/<base>-boot<N>-<ms><ext>
//   -> <T>/logging_captures/sensors/<day>/<base>-<stamp>-boot<N>-<ms9>[-k]<ext>
// with the same tier T, base, N, ms and ext (System_TimeAnchors.cpp).
inline bool isAnchorPromotion(const char* oldPath, const char* newPath) {
  if (!oldPath || !newPath) return false;
  static const char kSensors[] = "/logging_captures/sensors/";
  const bool sdA = strncmp(oldPath, "/sd/", 4) == 0, sdB = strncmp(newPath, "/sd/", 4) == 0;
  if (sdA != sdB) return false;
  const char* a = oldPath + (sdA ? 3 : 0);
  const char* b = newPath + (sdB ? 3 : 0);
  if (!detail::startsWith(a, kSensors) || !detail::startsWith(b, kSensors)) return false;
  a += sizeof(kSensors) - 1;
  b += sizeof(kSensors) - 1;
  if (!detail::startsWith(a, "boot-") || detail::startsWith(b, "boot-")) return false;
  const char* aSlash = strchr(a, '/');
  const char* bSlash = strchr(b, '/');
  if (!aSlash || !bSlash || strchr(aSlash + 1, '/') || strchr(bSlash + 1, '/') || bSlash == b) return false;
  uint64_t folderBoot = 0;
  int d = 0;
  for (const char* p = a + 5; p < aSlash && d < 11; ++p, ++d) {
    if (!detail::isDigit(*p)) return false;
    folderBoot = folderBoot * 10 + uint64_t(*p - '0');
  }
  if (d == 0 || a + 5 + d != aSlash) return false;
  const char* oldName = aSlash + 1;
  const char* newName = bSlash + 1;
  size_t baseLen = 0;
  uint32_t boot = 0, ms = 0;
  const char* ext = nullptr;
  if (!detail::bootFile(oldName, &baseLen, &boot, &ms, &ext) || boot != folderBoot) return false;
  // new: <base>-<stamp>-boot<N>-<ms9>[-k]<ext>
  if (strncmp(newName, oldName, baseLen) != 0 || newName[baseLen] != '-') return false;
  const char* stamp = newName + baseLen + 1;
  const char* tag = strstr(stamp, "-boot");
  if (!tag || tag == stamp) return false;
  const char* p = tag + 5;
  uint64_t nb = 0, nm = 0;
  d = 0;
  for (; detail::isDigit(*p) && d < 11; ++p, ++d) nb = nb * 10 + uint64_t(*p - '0');
  if (d == 0 || *p != '-' || nb != boot) return false;
  ++p;
  d = 0;
  for (; detail::isDigit(*p) && d < 11; ++p, ++d) nm = nm * 10 + uint64_t(*p - '0');
  if (d < 9 || nm != ms) return false;
  if (p[0] == '-' && p[1] >= '2' && p[1] <= '9') p += 2;
  return strcmp(p, ext) == 0;
}

// Does the walk need to read this file's head? `known` = findByPath(path).
// False only for a known path that only grew, whose head is already full (or
// covers the whole file), of a kind that never rotates by rename, with no
// trim note pending. Rotating kinds always need it: X.1 can be replaced by an
// equal-or-larger X without any size shrinking.
inline bool needHead(const LedgerEntry* known, const char* path, uint32_t size, bool trimNotePending = false) {
  if (!known || !path || strcmp(known->path, path) != 0) return true;
  if (trimNotePending || size < known->size) return true;
  if (detail::rotatesByRename(Kind(known->kind), path)) return true;
  if (known->headLen < kHeadMax && size > known->headLen) return true;   // re-anchor a short head
  return false;
}

// Reconcile the ledger with one walk (FILES_IMPL_SPEC 3.5), in order:
//  1. same path, head not read, size >= known: same id, new size;
//  2. same path, head read and the known head is its prefix, size >= known:
//     same id; a longer head re-anchors (no gen);
//  3. a trim note for the path's slot, before any head compare: restarted ->
//     new id with base = drops after the restart; trimmed -> base += drops,
//     head re-taken, same id; gen++ either way;
//  4. head differs or size shrank without a note: a known entry not matched
//     to its own path with an equal head and a rotation or TimeAnchors
//     relation keeps its id under the new path (gen++); otherwise a new id
//     (gen++). An equal head alone never moves an id;
//  5. known entries not matched: on /sd with the card out -> MissingCardOut,
//     kept, not listed; with an incomplete walk and the path not seen ->
//     left as they are; otherwise removed (gen++);
//  6. the horizon per kind, after matching: entries beyond it are removed
//     (gen++), new files beyond it are not added; ids are never reused
//     (nextFileId only climbs);
//  7. a base that would reach 2^31 starts a new id at base 0.
// New ids are issued in the order of `obs`. Card-out transitions and growth
// do not step gen.
inline ReconcileOut reconcile(LedgerState& s, const Observed* obs, size_t n, const TrimNote* notes,
                              size_t nNotes, const ReconcileEnv& env) {
  ReconcileOut out = {};
  if (n > kObservedMax) n = kObservedMax;
  if (s.count > kLedgerMax) s.count = kLedgerMax;
  const uint8_t kc = s.count;
  int16_t knownBind[kLedgerMax];       // observed index bound to known i, -1 none
  bool    knownDead[kLedgerMax];       // replaced in place (restart, base overflow): never re-bound
  bool    knownDrop[kLedgerMax];       // to be removed
  int16_t obsBind[kObservedMax];       // known index bound to observed j, -1 none
  uint32_t obsBase[kObservedMax];      // base for a new id
  bool    obsNoRename[kObservedMax];   // a restarted or overflowed file: never rename-matched
  bool    obsNew[kObservedMax];        // gets a new id (after the horizon)
  bool    obsReplaces[kObservedMax];   // the new id replaces a known file at this path
  for (uint8_t i = 0; i < kLedgerMax; ++i) { knownBind[i] = -1; knownDead[i] = false; knownDrop[i] = false; }
  for (size_t j = 0; j < kObservedMax; ++j) {
    obsBind[j] = -1; obsBase[j] = 0; obsNoRename[j] = false; obsNew[j] = false; obsReplaces[j] = false;
  }
  bool gen = false;

  auto noteFor = [&](uint8_t slot, TrimNote* got) -> bool {
    if (slot == kNoSlot || !notes) return false;
    bool any = false;
    TrimNote acc = { slot, 0, false };
    for (size_t k = 0; k < nNotes; ++k) {
      if (notes[k].slot != slot) continue;
      any = true;
      if (notes[k].restarted) { acc.restarted = true; acc.drops = notes[k].drops; }
      else acc.drops += notes[k].drops;
    }
    if (any) *got = acc;
    return any;
  };
  auto bindTo = [&](uint8_t i, size_t j, bool takeKind) {
    LedgerEntry& e = s.e[i];
    const Observed& o = obs[j];
    knownBind[i] = int16_t(j);
    obsBind[j] = int16_t(i);
    if (o.size > e.size) { e.lastGrowMs = env.nowMs; e.flags |= kLeGrowSeen; }
    e.size = o.size;
    e.state = uint8_t(EntryState::Present);
    if (takeKind) {
      e.kind = uint8_t(o.c.kind);
      e.seal = uint8_t(o.c.seal);
      e.flags = uint8_t((e.flags & ~(kLeSealedAtRest | kLeOnSd)) | (o.sealedAtRest ? kLeSealedAtRest : 0) |
                        (o.c.onSd ? kLeOnSd : 0));
    }
  };

  // Pass 1: the same path.
  for (size_t j = 0; j < n; ++j) {
    const Observed& o = obs[j];
    if (o.c.kind == Kind::None || boundedLen(o.path, kPathMax - 1) >= kPathMax) continue;
    int16_t ki = -1;
    for (uint8_t i = 0; i < kc; ++i)
      if (knownBind[i] < 0 && !knownDead[i] && strcmp(s.e[i].path, o.path) == 0) { ki = int16_t(i); break; }
    obsReplaces[j] = ki >= 0;   // read before pass 2 moves paths
    TrimNote note;
    if (noteFor(o.c.group, &note)) {
      obsNoRename[j] = true;
      if (note.restarted) {
        obsBase[j] = note.drops < kBaseLimit ? note.drops : 0;
        if (ki >= 0) knownDead[ki] = true;
        continue;
      }
      if (ki < 0) continue;   // never seen before: a new id at base 0
      LedgerEntry& e = s.e[ki];
      const uint64_t nb = uint64_t(e.base) + note.drops;
      if (nb >= kBaseLimit) {   // rule 7
        knownDead[ki] = true;
        continue;
      }
      bindTo(uint8_t(ki), j, o.headRead);
      e.base = uint32_t(nb);
      detail::takeHead(e, o);
      ++out.trimmed;
      gen = true;
      continue;
    }
    if (ki < 0) continue;
    LedgerEntry& e = s.e[ki];
    if (!o.headRead) {
      if (o.size >= e.size) bindTo(uint8_t(ki), j, false);
      continue;
    }
    if (o.size >= e.size && detail::headPrefixEqual(e, o)) {
      bindTo(uint8_t(ki), j, true);
      if (o.headLen > e.headLen) detail::takeHead(e, o);
    }
  }

  // Pass 2: renames (rotation, TimeAnchors promotion).
  for (size_t j = 0; j < n; ++j) {
    const Observed& o = obs[j];
    if (o.c.kind == Kind::None || obsBind[j] >= 0 || obsNoRename[j] || !o.headRead) continue;
    if (boundedLen(o.path, kPathMax - 1) >= kPathMax) continue;
    for (uint8_t i = 0; i < kc; ++i) {
      const LedgerEntry& e = s.e[i];
      if (knownBind[i] >= 0 || knownDead[i] || o.size < e.size || !detail::headPrefixEqual(e, o)) continue;
      if (!isRotationSibling(e.path, o.path) && !isAnchorPromotion(e.path, o.path)) continue;
      bindTo(i, j, true);
      detail::copyPath(s.e[i].path, o.path);
      ++out.renamed;
      gen = true;
      break;
    }
  }

  // Pass 3: what is left.
  for (size_t j = 0; j < n; ++j) {
    const Observed& o = obs[j];
    if (o.c.kind == Kind::None || obsBind[j] >= 0 || boundedLen(o.path, kPathMax - 1) >= kPathMax) continue;
    bool dup = false;   // a path listed twice by the walk: the first one wins
    for (size_t q = 0; q < j && !dup; ++q) dup = strcmp(obs[q].path, o.path) == 0 && (obsBind[q] >= 0 || obsNew[q]);
    if (dup) continue;
    obsNew[j] = true;
  }
  for (uint8_t i = 0; i < kc; ++i) {
    if (knownBind[i] >= 0) continue;
    LedgerEntry& e = s.e[i];
    bool pathSeen = false;
    for (size_t j = 0; j < n && !pathSeen; ++j) pathSeen = strcmp(obs[j].path, e.path) == 0;
    if (knownDead[i] || pathSeen) { knownDrop[i] = true; continue; }
    if ((e.flags & kLeOnSd) && !env.sdMounted) {
      if (e.state != uint8_t(EntryState::MissingCardOut)) ++out.cardOut;
      e.state = uint8_t(EntryState::MissingCardOut);
      continue;
    }
    if (!env.walkComplete) continue;
    knownDrop[i] = true;
  }

  // Horizon (rule 6), per kind, over present survivors and new files.
  // Name offsets into each path, worked out once (paths are < 128 bytes).
  uint8_t relK[kLedgerMax], relO[kObservedMax];
  for (uint8_t i = 0; i < kc; ++i) {
    const char* r = classifyPath(s.e[i].path).rel;
    relK[i] = uint8_t(r ? r - s.e[i].path : 0);
  }
  for (size_t j = 0; j < n; ++j) {
    const char* r = obsNew[j] ? classifyPath(obs[j].path).rel : nullptr;
    relO[j] = uint8_t(r ? r - obs[j].path : 0);
  }
  bool beyond = false;
  for (uint8_t kind = 1; kind <= kKindCount; ++kind) {
    // Rank every candidate by how many candidates of its group are newer;
    // keep rank < the group's cap.
    for (int side = 0; side < 2; ++side) {
      const size_t lim = side == 0 ? kc : n;
      for (size_t x = 0; x < lim; ++x) {
        const char* px;
        if (side == 0) {
          const LedgerEntry& e = s.e[x];
          if (knownDrop[x] || e.kind != kind || e.state != uint8_t(EntryState::Present)) continue;
          px = e.path;
        } else {
          if (!obsNew[x] || uint8_t(obs[x].c.kind) != kind) continue;
          px = obs[x].path;
        }
        const char* rx = px + (side == 0 ? relK[x] : relO[x]);
        const uint8_t gx = horizonGroup(Kind(kind), rx);
        const uint8_t cap = horizonGroupCap(Kind(kind), gx);
        unsigned rank = 0;
        for (int s2 = 0; s2 < 2; ++s2) {
          const size_t l2 = s2 == 0 ? kc : n;
          for (size_t y = 0; y < l2; ++y) {
            if (s2 == side && y == x) continue;
            const char* py;
            if (s2 == 0) {
              const LedgerEntry& f = s.e[y];
              if (knownDrop[y] || f.kind != kind || f.state != uint8_t(EntryState::Present)) continue;
              py = f.path;
            } else {
              if (!obsNew[y] || uint8_t(obs[y].c.kind) != kind) continue;
              py = obs[y].path;
            }
            const char* ry = py + (s2 == 0 ? relK[y] : relO[y]);
            if (horizonGroup(Kind(kind), ry) != gx) continue;
            const bool yNewer = newerThan(Kind(kind), ry, rx) ||
                                (!newerThan(Kind(kind), rx, ry) && strcmp(py, px) > 0);
            rank += yNewer;
          }
        }
        if (rank < cap) continue;
        beyond = true;
        if (side == 0) knownDrop[x] = true;
        else obsNew[x] = false;
      }
    }
  }

  // Remove, keeping ascending fileId order.
  uint8_t w = 0;
  for (uint8_t i = 0; i < kc; ++i) {
    if (knownDrop[i]) { ++out.removed; gen = true; continue; }
    if (w != i) s.e[w] = s.e[i];
    ++w;
  }
  s.count = w;

  // Add the new ids.
  for (size_t j = 0; j < n; ++j) {
    if (!obsNew[j]) continue;
    const Observed& o = obs[j];
    if (s.count >= kLedgerMax) {   // make room: the oldest entry kept for a missing card
      int16_t victim = -1;
      for (uint8_t i = 0; i < s.count && victim < 0; ++i)
        if (s.e[i].state == uint8_t(EntryState::MissingCardOut)) victim = int16_t(i);
      if (victim < 0) { beyond = true; continue; }
      for (uint8_t i = uint8_t(victim); i + 1 < s.count; ++i) s.e[i] = s.e[i + 1];
      --s.count;
      ++out.removed;
    }
    if (s.nextFileId == 0) s.nextFileId = 1;
    LedgerEntry& e = s.e[s.count];
    memset(&e, 0, sizeof(e));
    e.fileId = s.nextFileId++;
    e.kind = uint8_t(o.c.kind);
    e.seal = uint8_t(o.c.seal);
    e.flags = uint8_t((o.sealedAtRest ? kLeSealedAtRest : 0) | (o.c.onSd ? kLeOnSd : 0) | kLeGrowSeen);
    e.state = uint8_t(EntryState::Present);
    detail::copyPath(e.path, o.path);
    e.base = obsBase[j];
    detail::takeHead(e, o);
    e.createdBoot = env.bootCounter;
    e.size = o.size;
    e.delivered = 0;
    e.lastGrowMs = env.nowMs;
    ++s.count;
    ++out.newIds;
    if (!obsReplaces[j]) ++out.added;
    gen = true;
  }

  s.flags = uint8_t((s.flags & ~kLsBeyondHorizon) | (beyond ? kLsBeyondHorizon : 0));
  if (gen) { ++s.gen; out.changed = true; }
  return out;
}

// ---------------------------------------------------------------------------
// 7. LIST pages and READ checks over a ledger (device)
// ---------------------------------------------------------------------------

// The name a LIST entry carries: the path relative to its kind's folder.
inline const char* entryName(const LedgerEntry& e) {
  const char* rel = classifyPath(e.path).rel;
  return rel ? rel : "";
}

inline Entry toListEntry(const LedgerEntry& e, bool growingNow) {
  Entry x = {};
  x.kind = e.kind;
  x.flags = uint8_t(((e.flags & kLeSealedAtRest) ? kEfSealedAtRest : 0) | (growingNow ? kEfGrowing : 0) |
                    (e.base > 0 ? kEfHeadTrimmed : 0) | ((e.flags & kLeOnSd) ? kEfOnSd : 0) |
                    (e.seal == uint8_t(Seal::OnSend) ? kEfSealedOnSend : 0));
  x.fileId = e.fileId;
  x.start = e.base;
  x.end = logicalEnd(e);
  x.createdBoot = e.createdBoot;
  x.delivered = e.delivered < x.end ? e.delivered : x.end;
  x.name = entryName(e);
  x.nameLen = uint8_t(boundedLen(x.name, 255));
  return x;
}

// The served entries with fileId >= fromFileId in kindMask, ascending, at most
// `cap` of them. *nextFileId = the fileId of the next served entry beyond
// `cap` (0 = none): pass it as ListRespHeader.nextFileId to packListResp.
// *respFlags = the LIST header flags (sealed, withheld, beyond horizon).
// `activePaths` are the paths open for writing now (sensor log, system log).
inline size_t collectListEntries(const LedgerState& s, uint32_t fromFileId, uint16_t kindMask, bool keyReady,
                                 const char* const* activePaths, size_t nActive, uint32_t nowMs, Entry* out,
                                 size_t cap, uint32_t* nextFileId, uint8_t* respFlags) {
  size_t k = 0;
  uint8_t f = (s.flags & kLsBeyondHorizon) ? kLfBeyondHorizon : 0;
  if (nextFileId) *nextFileId = 0;
  for (uint8_t i = 0; i < s.count && i < kLedgerMax; ++i) {
    const LedgerEntry& e = s.e[i];
    if (e.state != uint8_t(EntryState::Present) || !kindInMask(Kind(e.kind), kindMask)) continue;
    const Offer o = entryOffer(e, keyReady);
    if (o == Offer::WithheldUnsealedHealth || o == Offer::WithheldNoKey) { f |= kLfWithheld; continue; }
    if (o != Offer::Serve || e.fileId < fromFileId) continue;
    if (k >= cap || !out) {
      if (nextFileId && !*nextFileId) *nextFileId = e.fileId;
      continue;
    }
    bool active = false;
    for (size_t a = 0; a < nActive && !active; ++a) active = activePaths[a] && strcmp(activePaths[a], e.path) == 0;
    out[k] = toListEntry(e, growing(e, active, nowMs));
    if (out[k].flags & (kEfSealedAtRest | kEfSealedOnSend)) f |= kLfAnySealed;
    ++k;
  }
  if (respFlags) *respFlags = f;
  return k;
}

// The READ status before any I/O: 0 and *entry, or the answer status:
// ledgerId mismatch, link != 0, unknown id -> 16 gone; card out or no key ->
// 7 not_ready; unsealed health -> 2 refused; an unknown mode, or a digest of
// a sealed-on-send kind -> 3 unsupported; offset < start -> 17 trimmed (the
// answer's offset = start); offset > end -> 18 past_end.
inline uint8_t readCheck(const LedgerState& s, const ReadReq& r, bool keyReady, const LedgerEntry** entry) {
  if (entry) *entry = nullptr;
  if (r.link != 0 || r.ledgerId != s.ledgerId) return kStGone;
  const LedgerEntry* e = findById(s, r.fileId);
  if (!e || e->state == uint8_t(EntryState::Gone)) return kStGone;
  if (e->state == uint8_t(EntryState::MissingCardOut)) return kStNotReady;
  const Offer o = entryOffer(*e, keyReady);
  if (o == Offer::WithheldNoKey) return kStNotReady;
  if (o != Offer::Serve) return kStRefused;
  if (r.mode != kModeData && r.mode != kModeDigest) return kStUnsupported;
  if (r.mode == kModeDigest && e->seal == uint8_t(Seal::OnSend)) return kStUnsupported;
  if (r.offset < e->base) return kStTrimmed;
  if (r.offset > logicalEnd(*e)) return kStPastEnd;
  if (entry) *entry = e;
  return kStOk;
}

// ---------------------------------------------------------------------------
// 8. Windows, digest and pacing
// ---------------------------------------------------------------------------

// The answer payload cap: min(pathCap, maxFrame ? max(maxFrame, 64) : pathCap).
inline uint16_t effectiveCap(uint16_t pathCap, uint8_t reqMaxFrame) {
  if (!reqMaxFrame) return pathCap;
  const uint16_t want = reqMaxFrame < kMinFrame ? kMinFrame : reqMaxFrame;
  return want < pathCap ? want : pathCap;
}

struct WindowPlan { uint8_t frames; uint16_t plainPerFrame; uint32_t plainBytes; bool atEnd; };
// One READ window: up to 8 frames of plainPerFrame plaintext bytes each
// (cap - 20, less 28 when sealed on send). offset == end -> 1 empty frame,
// atEnd. offset > end -> 0 frames (answer 18 past_end). maxBytes 0 = to the end.
inline WindowPlan planWindow(uint32_t offset, uint32_t end, uint32_t maxBytes, uint16_t pathCap,
                             uint8_t reqMaxFrame, bool sealOnSend) {
  WindowPlan w = {};
  const uint16_t cap = effectiveCap(pathCap, reqMaxFrame);
  const uint16_t over = uint16_t(kReadRespHeaderLen + (sealOnSend ? kSealOverhead : 0));
  w.plainPerFrame = cap > over ? uint16_t(cap - over) : 0;
  if (offset > end) return w;
  if (offset == end) { w.frames = 1; w.atEnd = true; return w; }
  if (!w.plainPerFrame) return w;
  uint32_t want = end - offset;
  if (maxBytes && maxBytes < want) want = maxBytes;
  const uint32_t most = uint32_t(kWindowMaxFrames) * w.plainPerFrame;
  if (want > most) want = most;
  w.plainBytes = want;
  w.frames = uint8_t((want + w.plainPerFrame - 1) / w.plainPerFrame);
  w.atEnd = offset + want == end;
  return w;
}
// Frame k (0-based) of a window: its logical offset and plaintext length.
inline void windowFrame(const WindowPlan& w, uint32_t offset, uint8_t k, uint32_t* frameOffset, uint16_t* len) {
  const uint32_t at = uint32_t(k) * w.plainPerFrame;
  const uint32_t left = at < w.plainBytes ? w.plainBytes - at : 0;
  if (frameOffset) *frameOffset = offset + at;
  if (len) *len = uint16_t(left < w.plainPerFrame ? left : w.plainPerFrame);
}

struct DigestPlan { uint32_t from, to; uint16_t blocks; };
// The digest range [offset, offset+maxBytes) clipped to end (maxBytes 0 = to
// the end), in 4096-byte blocks. Call after readCheck (start <= offset <= end).
// false -> 4 too_large when the range is over 1 MiB, or it is outside [start, end].
inline bool planDigest(uint32_t offset, uint32_t maxBytes, uint32_t start, uint32_t end, DigestPlan* out) {
  if (!out || offset < start || offset > end) return false;
  uint64_t to = maxBytes ? uint64_t(offset) + maxBytes : uint64_t(end);
  if (to > end) to = end;
  if (to - offset > kDigestMaxBytes) return false;
  out->from = offset;
  out->to = uint32_t(to);
  out->blocks = uint16_t((out->to - out->from + kDigestBlock - 1) / kDigestBlock);
  return true;
}
// Block k of a digest plan: [*from, *from + *len).
inline void digestBlock(const DigestPlan& d, uint16_t k, uint32_t* from, uint32_t* len) {
  const uint32_t f = d.from + uint32_t(k) * kDigestBlock;
  const uint32_t rest = f < d.to ? d.to - f : 0;
  if (from) *from = f;
  if (len) *len = rest < kDigestBlock ? rest : kDigestBlock;
}

// The device's pacing (6.4). Zero-filled = empty; the caller owns it (PSRAM).
struct PacerPeer {
  uint8_t  mac[6];
  uint8_t  used, hasFullList, refusedValid, pagesThisRun;
  uint16_t refusedSession;
  uint32_t lastFullListMs, lastPageMs, lru;
};
struct DigestMark { uint8_t mac[6]; uint8_t used; uint16_t sessionId; uint32_t fileId; };

struct ServePacer {
  enum class Verdict : uint8_t { Go, Busy, Refuse, DropSilently };
  struct Decision { Verdict v; uint16_t retryAfterS; };

  PacerPeer  peers[kPacerPeers];
  DigestMark digests[kDigestRing];
  uint8_t    digestNext;
  uint8_t    activeMac[6];
  uint8_t    activeValid;
  uint32_t   activeLastMs;
  uint32_t   lruClock;

  PacerPeer* peer(const uint8_t mac[6]) {
    PacerPeer* victim = &peers[0];
    for (uint8_t i = 0; i < kPacerPeers; ++i) {
      PacerPeer& p = peers[i];
      if (p.used && memcmp(p.mac, mac, 6) == 0) { p.lru = ++lruClock; return &p; }
      if (!p.used) { if (victim->used) victim = &p; }
      else if (victim->used && p.lru < victim->lru) victim = &p;
    }
    memset(victim, 0, sizeof(*victim));
    memcpy(victim->mac, mac, 6);
    victim->used = 1;
    victim->lru = ++lruClock;
    return victim;
  }
  bool active(uint32_t nowMs) const {
    return activeValid && uint32_t(nowMs - activeLastMs) < kRequesterIdleMs;
  }
  bool isActiveRequester(const uint8_t mac[6], uint32_t nowMs) const {
    return active(nowMs) && memcmp(activeMac, mac, 6) == 0;
  }
  // Called from the serve tick: drops the lease after 10 s idle.
  void expire(uint32_t nowMs) { if (activeValid && !active(nowMs)) activeValid = 0; }
  void release() { activeValid = 0; }

  // First match wins: not settled -> busy 2 s; battery known, discharging and
  // below 20 % -> busy 600 s; another requester seen within 10 s -> busy
  // 10 s; slot busy or tx queue >= 16 -> busy 1 s; LIST: a full list (from 0)
  // within 60 s of this MAC's last one -> busy for the rest (any sessionId);
  // a page within 50 ms of the last -> busy 1 s; more than 64 pages in a run
  // -> busy 60 s. Go takes the requester lease.
  Decision onRequest(const uint8_t mac[6], uint16_t sessionId, uint8_t op, uint32_t fromFileId, uint32_t nowMs,
                     bool slotBusy, uint8_t txDepth, bool sessionSettled, bool batteryKnown, bool discharging,
                     uint8_t batteryPct) {
    (void)sessionId;
    if (!sessionSettled) return { Verdict::Busy, 2 };
    if (batteryKnown && discharging && batteryPct <= 100 && batteryPct < kBatteryFloorPct)
      return { Verdict::Busy, kBatteryRetryS };
    if (active(nowMs) && memcmp(activeMac, mac, 6) != 0) return { Verdict::Busy, uint16_t(kRequesterIdleMs / 1000) };
    if (slotBusy || txDepth >= kTxDepthLimit) return { Verdict::Busy, 1 };
    PacerPeer* p = peer(mac);
    if (op == kOpListReq) {
      if (fromFileId == 0) {
        const uint32_t since = uint32_t(nowMs - p->lastFullListMs);
        if (p->hasFullList && since < kFullListMinMs) {
          const uint32_t left = kFullListMinMs - since;
          return { Verdict::Busy, uint16_t((left + 999) / 1000) };
        }
        p->hasFullList = 1;
        p->lastFullListMs = nowMs;
        p->pagesThisRun = 0;
      } else {
        if (p->pagesThisRun && uint32_t(nowMs - p->lastPageMs) < kPageMinMs) return { Verdict::Busy, 1 };
        if (p->pagesThisRun >= kMaxPagesPerRun) return { Verdict::Busy, 60 };
      }
      ++p->pagesThisRun;
      p->lastPageMs = nowMs;
    }
    memcpy(activeMac, mac, 6);
    activeValid = 1;
    activeLastMs = nowMs;
    return { Verdict::Go, 0 };
  }
  // A refused requester is answered once per session, then dropped silently.
  bool refusalAnsweredOnce(const uint8_t mac[6], uint16_t sessionId) {
    PacerPeer* p = peer(mac);
    if (p->refusedValid && p->refusedSession == sessionId) return false;
    p->refusedValid = 1;
    p->refusedSession = sessionId;
    return true;
  }
  Decision refusal(const uint8_t mac[6], uint16_t sessionId) {
    return refusalAnsweredOnce(mac, sessionId) ? Decision{ Verdict::Refuse, 0 } : Decision{ Verdict::DropSilently, 0 };
  }
  // One digest per file per session (an 8-entry ring).
  bool digestAllowed(const uint8_t mac[6], uint16_t sessionId, uint32_t fileId) {
    for (uint8_t i = 0; i < kDigestRing; ++i) {
      const DigestMark& d = digests[i];
      if (d.used && d.sessionId == sessionId && d.fileId == fileId && memcmp(d.mac, mac, 6) == 0) return false;
    }
    DigestMark& d = digests[digestNext % kDigestRing];
    digestNext = uint8_t((digestNext + 1) % kDigestRing);
    memcpy(d.mac, mac, 6);
    d.used = 1;
    d.sessionId = sessionId;
    d.fileId = fileId;
    return true;
  }
  void noteWindowDone(const uint8_t mac[6], uint32_t nowMs) {
    if (activeValid && memcmp(activeMac, mac, 6) == 0) activeLastMs = nowMs;
  }
};

// ---------------------------------------------------------------------------
// 9. Sealing on send: the AAD and the collector's on-card record
// ---------------------------------------------------------------------------

inline constexpr char kAadMagic[6] = { 'H', 'W', '1', 'C', 'U', '1' };
constexpr size_t kAadLen = 22;
constexpr uint8_t kAadVersion = 1;
// "HW1CU1" || ledgerId || fileId || offset || plainLen (u16) || kind || 0x01, LE.
inline size_t buildSealAad(uint8_t out[kAadLen], uint32_t ledgerId, uint32_t fileId, uint32_t offset,
                           uint16_t plainLen, uint8_t kind) {
  memcpy(out, kAadMagic, 6);
  le::put32(out + 6, ledgerId);
  le::put32(out + 10, fileId);
  le::put32(out + 14, offset);
  le::put16(out + 18, plainLen);
  out[20] = kind;
  out[21] = kAadVersion;
  return kAadLen;
}
// A sealed frame's plaintext length: dataLen - 28 (0 if shorter).
inline uint16_t sealedPlainLen(size_t dataLen) {
  return dataLen > kSealOverhead ? uint16_t(dataLen - kSealOverhead) : 0;
}

inline constexpr char kSealedSegMagic[8] = { 'H', 'W', '1', 'C', 'S', 'G', '1', '\n' };
constexpr size_t kSealedRecordHeaderLen = 20;
constexpr uint8_t kSealedRecordVersion = 1;
// 0 ver, 1 kind, 2 recLen, 4 ledgerId, 8 fileId, 12 offset, 16 plainLen,
// 18 reserved 0; then nonce12 || ct[plainLen] || tag16 (the frame data as received).
struct SealedRecord {
  uint8_t  ver, kind;
  uint16_t recLen;              // 20 + 12 + plainLen + 16
  uint32_t ledgerId, fileId, offset;
  uint16_t plainLen;
};
inline size_t packSealedRecordHeader(const SealedRecord& r, uint8_t out[kSealedRecordHeaderLen]) {
  out[0] = r.ver;
  out[1] = r.kind;
  le::put16(out + 2, r.recLen);
  le::put32(out + 4, r.ledgerId);
  le::put32(out + 8, r.fileId);
  le::put32(out + 12, r.offset);
  le::put16(out + 16, r.plainLen);
  out[18] = 0;
  out[19] = 0;
  return kSealedRecordHeaderLen;
}
// A record header for one received sealed frame (ver 1, recLen computed).
inline SealedRecord sealedRecordFor(uint8_t kind, uint32_t ledgerId, uint32_t fileId, uint32_t offset,
                                    uint16_t plainLen) {
  SealedRecord r = {};
  r.ver = kSealedRecordVersion;
  r.kind = kind;
  r.recLen = uint16_t(kSealedRecordHeaderLen + kSealOverhead + plainLen);
  r.ledgerId = ledgerId;
  r.fileId = fileId;
  r.offset = offset;
  r.plainLen = plainLen;
  return r;
}
// false if n < 20, ver != 1 or recLen != 20 + 12 + plainLen + 16.
inline bool parseSealedRecordHeader(const uint8_t* p, size_t n, SealedRecord* out) {
  if (!p || !out || n < kSealedRecordHeaderLen) return false;
  SealedRecord r;
  r.ver = p[0];
  r.kind = p[1];
  r.recLen = le::get16(p + 2);
  r.ledgerId = le::get32(p + 4);
  r.fileId = le::get32(p + 8);
  r.offset = le::get32(p + 12);
  r.plainLen = le::get16(p + 16);
  if (r.ver != kSealedRecordVersion) return false;
  if (uint32_t(r.recLen) != kSealedRecordHeaderLen + kSealOverhead + uint32_t(r.plainLen)) return false;
  *out = r;
  return true;
}

// ---------------------------------------------------------------------------
// 10. Names (both sides)
// ---------------------------------------------------------------------------

namespace detail {
inline char hexDigit(unsigned v) { return "0123456789abcdef"[v & 15u]; }
inline void hex32(uint32_t v, char* out) {
  for (int i = 7; i >= 0; --i) { out[i] = hexDigit(v); v >>= 4; }
}
// Length of a valid UTF-8 sequence at p (n bytes left), 0 if invalid.
inline size_t utf8Seq(const uint8_t* p, size_t n) {
  const uint8_t c = p[0];
  if (c < 0x80) return 1;
  auto cont = [&](size_t i) { return i < n && (p[i] & 0xC0) == 0x80; };
  if (c >= 0xC2 && c <= 0xDF) return cont(1) ? 2 : 0;
  if (c == 0xE0) return n > 1 && p[1] >= 0xA0 && p[1] <= 0xBF && cont(2) ? 3 : 0;
  if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) return cont(1) && cont(2) ? 3 : 0;
  if (c == 0xED) return n > 1 && p[1] >= 0x80 && p[1] <= 0x9F && cont(2) ? 3 : 0;
  if (c == 0xF0) return n > 1 && p[1] >= 0x90 && p[1] <= 0xBF && cont(2) && cont(3) ? 4 : 0;
  if (c >= 0xF1 && c <= 0xF3) return cont(1) && cont(2) && cont(3) ? 4 : 0;
  if (c == 0xF4) return n > 1 && p[1] >= 0x80 && p[1] <= 0x8F && cont(2) && cont(3) ? 4 : 0;
  return 0;
}
}  // namespace detail

// Lowercase hex, no colons: "a0b1c2d3e4f5".
inline void mac12(const uint8_t mac[6], char out[13]) {
  for (int i = 0; i < 6; ++i) {
    out[2 * i] = detail::hexDigit(mac[i] >> 4);
    out[2 * i + 1] = detail::hexDigit(mac[i]);
  }
  out[12] = 0;
}

// The collector's segment name, from ids only (never from the device's name):
// "%08lx-%08lx-s%u.raw" or ".sealed". Returns the length, or 0 (out = "")
// when it does not fit cap.
inline size_t localSegmentName(uint32_t ledgerId, uint32_t fileId, uint16_t seg, bool sealedOnSend,
                               char* out, size_t cap) {
  char buf[40];
  size_t n = 0;
  detail::hex32(ledgerId, buf); n = 8;
  buf[n++] = '-';
  detail::hex32(fileId, buf + n); n += 8;
  buf[n++] = '-';
  buf[n++] = 's';
  char dig[6];
  int d = 0;
  unsigned v = seg;
  do { dig[d++] = char('0' + v % 10); v /= 10; } while (v);
  while (d) buf[n++] = dig[--d];
  const char* ext = sealedOnSend ? ".sealed" : ".raw";
  const size_t el = strlen(ext);
  memcpy(buf + n, ext, el);
  n += el;
  if (!out || cap == 0) return 0;
  if (n + 1 > cap) { out[0] = 0; return 0; }
  memcpy(out, buf, n);
  out[n] = 0;
  return n;
}

// One-line display of a device-supplied name: '"' -> \", '\\' -> \\, control
// bytes, DEL and every byte of an invalid UTF-8 sequence -> \xNN; valid UTF-8
// is kept. Never splits an escape or a character at cap. Always
// NUL-terminates (cap > 0). Returns the length written. Display only: the
// device's name is never used as a path.
inline size_t displayName(const char* in, size_t n, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t w = 0;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(in);
  size_t i = 0;
  while (p && i < n) {
    const uint8_t c = p[i];
    char tmp[4];
    size_t tl = 0, adv = 1;
    const char* src = tmp;
    if (c == '"' || c == '\\') { tmp[0] = '\\'; tmp[1] = char(c); tl = 2; }
    else if (c < 0x20 || c == 0x7F) { tmp[0] = '\\'; tmp[1] = 'x'; tmp[2] = detail::hexDigit(c >> 4); tmp[3] = detail::hexDigit(c); tl = 4; }
    else {
      const size_t sl = detail::utf8Seq(p + i, n - i);
      if (sl == 0) { tmp[0] = '\\'; tmp[1] = 'x'; tmp[2] = detail::hexDigit(c >> 4); tmp[3] = detail::hexDigit(c); tl = 4; }
      else { src = reinterpret_cast<const char*>(p + i); tl = sl; adv = sl; }
    }
    if (w + tl + 1 > cap) break;
    memcpy(out + w, src, tl);
    w += tl;
    i += adv;
  }
  out[w] = 0;
  return w;
}

}  // namespace espnowcatchup
