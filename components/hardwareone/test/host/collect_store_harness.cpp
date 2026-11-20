// Drives the production collector store (System_CollectStore.cpp, between its
// COLLECT_STORE_HOST markers) over the shared fake VFS with real ArduinoJson,
// the real FsLockGuard and the pure cores it uses unchanged
// (System_CollectCore.h, System_ESPNow_CatchupCore.h). StagedWrite is a host
// double with the production contract (stage to "<dest>.upload.tmp", promote
// with replaceFileGuarded, discard removes the temporary); SHA-256 is
// CommonCrypto. Nothing here models power loss beyond the files left behind.
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#define FAKE_VFS_LOCK_DOUBLE
#include <Arduino.h>
#include "fake_vfs.h"
#include <ArduinoJson.h>
#include <CommonCrypto/CommonDigest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "psram_json_doc_host.h"
#include "System_CollectStore.h"
#include "System_MacText.h"

void webBatchTestStringAppend() {}
#define EXT_RAM_BSS_ATTR

static unsigned checks = 0;
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "collect store check failed at line %d: %s\n", line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

// The real FsLockGuard (System_Mutex.h/.cpp) over the fake's lock double.
// INSERT_FS_GUARD

static std::string gLastWriter;   // the writer name of the last staged commit
namespace VFS {
class StagedWrite {
 public:
  explicit StagedWrite(const char* writer) : writer_(writer) {}
  bool begin(const String& destination, const AuthContext& ctx) {
    if (open_) return false;
    destination_ = destination.c_str();
    temporary_ = destination_ + ".upload.tmp";
    file_ = openGuarded(String(temporary_.c_str()), "w", ctx, true);
    if (!file_) return false;
    open_ = true;
    return true;
  }
  File& file() { return file_; }
  bool commit(const AuthContext& ctx) {
    if (!open_) return false;
    open_ = false;
    gLastWriter = writer_;
    file_.flush();
    const bool err = file_.getWriteError() != 0;
    file_.close();
    if (err) { removeGuarded(String(temporary_.c_str()), ctx); return false; }
    if (replaceFileGuarded(String(temporary_.c_str()), String(destination_.c_str()), ctx)) return true;
    removeGuarded(String(temporary_.c_str()), ctx);
    return false;
  }
  void discard(const AuthContext& ctx) {
    if (!open_) return;
    open_ = false;
    file_.close();
    removeGuarded(String(temporary_.c_str()), ctx);
  }

 private:
  const char* writer_;
  std::string destination_, temporary_;
  File file_;
  bool open_ = false;
};
}  // namespace VFS

static unsigned yields = 0;
namespace collectstore {
namespace {
struct Sha256 { CC_SHA256_CTX c; };
void shaBegin(Sha256& s) { CC_SHA256_Init(&s.c); }
void shaUpdate(Sha256& s, const uint8_t* p, size_t n) { CC_SHA256_Update(&s.c, p, CC_LONG(n)); }
void shaFinish(Sha256& s, uint8_t out[32]) { CC_SHA256_Final(out, &s.c); }
void storeYield() { ++yields; }
}  // namespace
}  // namespace collectstore

// INSERT_STORE

namespace cc = collectcore;
namespace cu = espnowcatchup;
using fake_vfs::fs;

static const uint8_t kMac[6] = { 0x8c, 0xbf, 0xea, 0x00, 0xa7, 0x73 };
static const char kDir[] = "/sd/collect/8cbfea00a773/files";
static cc::DeviceFiles gD, gD2;
static cc::WindowAssembler gW;

static void resetFs() {
  fs().reset();
  fake_vfs::wireLockDouble();
  fs().now = 1795370531;   // 2026-11-22T18:02:11Z
}
static std::string path(const char* leaf) { return std::string(kDir) + "/" + leaf; }

// A window of `n` plaintext bytes at `offset` in frames of `per` (sealed:
// each frame's data is plaintext + 28 bytes of stand-in seal).
static void window(uint32_t fileId, uint32_t offset, uint32_t n, uint16_t per, bool sealed, uint8_t fill) {
  gW.begin(42, fileId, offset, sealed, 0, false);
  cu::ReadRespHeader h = {};
  h.reqId = 42; h.fileId = fileId; h.end = offset + n;
  uint8_t data[cc::kFrameDataMax];
  uint32_t at = offset;
  while (at < offset + n) {
    const uint32_t plain = offset + n - at < per ? offset + n - at : per;
    const size_t len = sealed ? plain + cu::kSealOverhead : plain;
    for (size_t i = 0; i < len; ++i) data[i] = uint8_t(fill + i);
    h.offset = at;
    h.flags = uint8_t((sealed ? cu::kRfSealedOnSend : 0) | (at + plain >= offset + n ? cu::kRfLast : 0));
    check(gW.onFrame(h, data, len, 1) == cc::WindowAssembler::Took::Took);
    at += plain;
  }
}

static cc::FileState& addFile(uint32_t id, uint8_t kind, const char* name, uint32_t end, uint8_t flags = 0) {
  cc::FileState& f = gD.f[gD.count++];
  memset(&f, 0, sizeof(f));
  f.fileId = id; f.kind = kind; f.flags = flags; f.lastSeenEnd = end; f.state = cc::FState::Fetching;
  f.createdBoot = id;
  cc::safeName(name, strlen(name), f.name);
  return f;
}
static void freshDevice() {
  memset(&gD, 0, sizeof(gD));
  memcpy(gD.mac, kMac, 6);
  gD.used = 1; gD.haveLedger = 1; gD.ledgerId = 0x1a2b3c4d;
  strcpy(gD.peerKeyFp, "00112233aabbccdd");
}

static void testUtc() {
  char t[24];
  check(collectstore::formatUtc(1795370531, t, sizeof(t)) == 20 && strcmp(t, "2026-11-22T18:02:11Z") == 0);
  uint32_t e = 0;
  check(collectstore::parseUtc(t, &e) && e == 1795370531);
  check(collectstore::formatUtc(0, t, sizeof(t)) == 0 && t[0] == 0);
  check(collectstore::parseUtc("2000-02-29T00:00:00Z", &e) && e == 951782400);
  check(!collectstore::parseUtc("2026-11-22 18:02:11Z", &e) && !collectstore::parseUtc("2026-13-22T18:02:11Z", &e));
  check(!collectstore::parseUtc(nullptr, &e));
}

static void testAppendRawAndFlushOrder() {
  resetFs();
  freshDevice();
  cc::FileState& f = addFile(12, 2, "events.log", 1000);
  window(12, 0, 400, 182, false, 7);
  // The flush of the segment comes before the file records the bytes.
  bool sawFlush = false;
  fs().onCall = [&](const fake_vfs::Call& c) {
    if (c.op == "flush" && c.path == path("1a2b3c4d-0000000c-s0.raw")) { sawFlush = true; check(f.pos == 0); }
  };
  const collectstore::WriteResult r = collectstore::appendWindow(gD, f, gW, fs().now);
  fs().onCall = nullptr;
  check(sawFlush && r.committed == 400 && !r.ioError && !r.rejected);
  check(f.pos == 400 && f.nSeg == 1 && f.seg[0].from == 0 && f.seg[0].to == 400 && f.nextSeg == 1);
  const std::string bytes = fs().text(path("1a2b3c4d-0000000c-s0.raw"));
  check(bytes.size() == 400);
  check(uint8_t(bytes[0]) == 7 && uint8_t(bytes[182]) == 7 && uint8_t(bytes[364]) == 7);   // three frames, in order
  // The next window extends the same segment.
  window(12, 400, 100, 182, false, 1);
  check(collectstore::appendWindow(gD, f, gW, fs().now).committed == 100 && f.seg[0].to == 500 && f.nSeg == 1);
  check(fs().text(path("1a2b3c4d-0000000c-s0.raw")).size() == 500);
  // A window not where the file stands is refused, nothing written.
  window(12, 900, 10, 182, false, 1);
  check(collectstore::appendWindow(gD, f, gW, fs().now).rejected && f.pos == 500);
  // Every card operation ran under the filesystem lock.
  check(fs().unlockedCalls() == 0 && fs().liveHandles() == 0);
}

static void testAppendSealed() {
  resetFs();
  freshDevice();
  cc::FileState& f = addFile(5, 6, "system.log.1", 10000, cc::kFfSealOnSend);
  window(5, 0, 300, 154, true, 3);
  const collectstore::WriteResult r = collectstore::appendWindow(gD, f, gW, fs().now);
  check(r.committed == 300 && f.pos == 300);
  const std::string bytes = fs().text(path("1a2b3c4d-00000005-s0.sealed"));
  check(bytes.size() == 8 + 2 * (20 + 28) + 300 && bytes.compare(0, 8, "HW1CSG1\n") == 0);
  cu::SealedRecord rec;
  check(cu::parseSealedRecordHeader(reinterpret_cast<const uint8_t*>(bytes.data()) + 8, 20, &rec));
  check(rec.kind == 6 && rec.ledgerId == 0x1a2b3c4d && rec.fileId == 5 && rec.offset == 0 && rec.plainLen == 154 &&
        rec.recLen == 20 + 154 + 28);
  check(uint8_t(bytes[8 + 20]) == 3);   // the frame data follows unchanged
  check(cu::parseSealedRecordHeader(reinterpret_cast<const uint8_t*>(bytes.data()) + 8 + rec.recLen, 20, &rec) &&
        rec.offset == 154 && rec.plainLen == 146);
  // A plain window for a sealed file is refused.
  window(5, 300, 10, 182, false, 1);
  check(collectstore::appendWindow(gD, f, gW, fs().now).rejected);
}

static void testIndexRoundTrip() {
  resetFs();
  freshDevice();
  gD.lastCaughtUpEpoch = 1795370000;
  gD.runId = 7;
  cc::FileState& a = addFile(12, 6, "system.log.1", 600, cc::kFfSealOnSend | cc::kFfOnSd);
  a.firstSeenEpoch = 1795360000;
  a.deviceStart = 100;
  cc::addGap(a, 0, 100, cc::kGapBeforeFirstFetch);
  a.pos = 100;
  window(12, 100, 200, 154, true, 9);
  check(collectstore::appendWindow(gD, a, gW, fs().now).committed == 200);
  cc::addElsewhere(a, 300, 400);
  a.pos = 400;
  cc::FileState& b = addFile(19, 2, "events.log", 50, cc::kFfGrowing);
  window(19, 0, 50, 182, false, 1);
  check(collectstore::appendWindow(gD, b, gW, fs().now).committed == 50 && b.state == cc::FState::Current);
  b.digest = cc::DigestState::Ok;
  const unsigned before = gHostPsramJson.allocations;
  check(collectstore::saveIndex(gD, fs().now));
  check(gHostPsramJson.allocations > before);   // the PSRAM document, not a default one
  const std::string text = fs().text(path("index.json"));
  check(text.find("\"schema\":1") != std::string::npos && text.find("\"mac\":\"8C:BF:EA:00:A7:73\"") != std::string::npos);
  check(text.find("\"updatedUtc\":\"2026-11-22T18:02:11Z\"") != std::string::npos);
  check(text.find("\"ledgerId\":\"1a2b3c4d\"") != std::string::npos);
  check(text.find("\"file\":\"1a2b3c4d-0000000c-s0.sealed\"") != std::string::npos);
  check(text.find("\"seal\":\"on_send\"") != std::string::npos && text.find("\"kind\":\"system_log\"") != std::string::npos);
  check(text.find("\"why\":\"before_first_fetch\"") != std::string::npos && text.find("\"digest\":\"ok\"") != std::string::npos);
  check(!fs().exists(path("index.json.upload.tmp")) && gLastWriter == "collect.index");
  collectstore::LoadResult lr = {};
  check(collectstore::loadIndex(kMac, "00112233aabbccdd", fs().now, gD2, &lr));
  check(!lr.fresh && !lr.badIndex && !lr.identityChanged && lr.recovered == 0 && lr.missing == 0);
  check(gD2.count == 2 && gD2.ledgerId == 0x1a2b3c4d && gD2.lastCaughtUpEpoch == 1795370000 && gD2.runId == 7);
  const cc::FileState* a2 = cc::findFile(static_cast<const cc::DeviceFiles&>(gD2), 12);
  check(a2 && a2->kind == 6 && (a2->flags & cc::kFfSealOnSend) && (a2->flags & cc::kFfOnSd) && a2->pos == 400 &&
        a2->nSeg == 1 && a2->seg[0].from == 100 && a2->seg[0].to == 300 && a2->nGap == 1 && a2->nElse == 1 &&
        a2->elsewhereBytes == 100 && a2->gapBytes[cc::kGapBeforeFirstFetch] == 100 && a2->firstSeenEpoch == 1795360000 &&
        strcmp(a2->name, "system.log.1") == 0 && a2->nextSeg == 1);
  const cc::FileState* b2 = cc::findFile(static_cast<const cc::DeviceFiles&>(gD2), 19);
  check(b2 && b2->digest == cc::DigestState::Ok && (b2->flags & cc::kFfGrowing) && b2->state == cc::FState::Current);
  check(fs().unlockedCalls() == 0);
}

static void testRecovery() {
  resetFs();
  freshDevice();
  cc::FileState& a = addFile(1, 2, "events.log", 1000);
  window(1, 0, 300, 182, false, 1);
  collectstore::appendWindow(gD, a, gW, fs().now);
  cc::FileState& b = addFile(2, 7, "u3/20261122T120000Z-x.txt", 1000, cc::kFfSealOnSend);
  window(2, 0, 300, 154, true, 1);
  collectstore::appendWindow(gD, b, gW, fs().now);
  cc::FileState& c = addFile(3, 5, "errors.log", 1000);
  window(3, 0, 100, 182, false, 1);
  collectstore::appendWindow(gD, c, gW, fs().now);
  check(collectstore::saveIndex(gD, fs().now));
  // After the save: 40 more bytes reached a.raw (a cut before the index
  // save), half a record reached b.sealed, and c's segment is gone.
  fs().put(path("1a2b3c4d-00000001-s0.raw"), fs().text(path("1a2b3c4d-00000001-s0.raw")) + std::string(40, 'x'));
  const std::string sealed = fs().text(path("1a2b3c4d-00000002-s0.sealed"));
  fs().put(path("1a2b3c4d-00000002-s0.sealed"), sealed + std::string(30, 'y'));
  {
    const AuthContext ctx = VFS::systemAuth("test");
    FsLockGuard g("test");
    check(VFS::removeGuarded(String(path("1a2b3c4d-00000003-s0.raw").c_str()), ctx));
  }
  collectstore::LoadResult lr = {};
  check(collectstore::loadIndex(kMac, "00112233aabbccdd", fs().now, gD2, &lr));
  check(lr.recovered == 1 && lr.tornCut == 1 && lr.missing == 1);
  const cc::FileState* a2 = cc::findFile(static_cast<const cc::DeviceFiles&>(gD2), 1);
  const cc::FileState* b2 = cc::findFile(static_cast<const cc::DeviceFiles&>(gD2), 2);
  const cc::FileState* c2 = cc::findFile(static_cast<const cc::DeviceFiles&>(gD2), 3);
  check(a2->seg[0].to == 340 && a2->pos == 340);                       // the .raw length wins
  check(b2->seg[0].to == 300 && b2->pos == 300);                       // whole records only
  check(fs().text(path("1a2b3c4d-00000002-s0.sealed")) == sealed);     // the torn tail is cut
  check(c2->nSeg == 0 && c2->pos == 0 && c2->state == cc::FState::Fetching);   // refetched from 0
  // A shorter .raw: the segment shrinks, is closed, and the next window starts a new one.
  fs().put(path("1a2b3c4d-00000001-s0.raw"), std::string(200, 'z'));
  check(collectstore::loadIndex(kMac, "00112233aabbccdd", fs().now, gD2, &lr));
  cc::FileState* a3 = cc::findFile(gD2, 1);
  check(a3->seg[0].to == 200 && a3->pos == 200 && a3->seg[0].closed);
  window(1, 200, 10, 182, false, 1);
  check(collectstore::appendWindow(gD2, *a3, gW, fs().now).committed == 10 && a3->nSeg == 2 && a3->seg[1].seg == 1);
  check(fs().unlockedCalls() == 0);
}

static void testBadIndexAndIdentity() {
  resetFs();
  fs().mkdirAll(kDir);
  fs().put(path("index.json"), "{not json");
  collectstore::LoadResult lr = {};
  check(collectstore::loadIndex(kMac, "00112233aabbccdd", fs().now, gD2, &lr));
  check(lr.fresh && lr.badIndex && fs().exists(path("index.json.bad")) && !fs().exists(path("index.json")));
  check(gD2.count == 0 && !gD2.haveLedger && strcmp(gD2.peerKeyFp, "00112233aabbccdd") == 0);
  // An index of another device's MAC is not used either.
  fs().put(path("index.json"), "{\"schema\":1,\"mac\":\"aa:aa:aa:aa:aa:aa\",\"ledgerId\":\"1a2b3c4d\",\"files\":[]}");
  check(collectstore::loadIndex(kMac, "00112233aabbccdd", fs().now, gD2, &lr) && lr.badIndex);
  // The device was paired again: files/ moves aside, a fresh one starts.
  resetFs();
  freshDevice();
  cc::FileState& a = addFile(1, 2, "events.log", 100);
  window(1, 0, 50, 182, false, 1);
  collectstore::appendWindow(gD, a, gW, fs().now);
  check(collectstore::saveIndex(gD, fs().now));
  fs().now += 60;
  check(collectstore::loadIndex(kMac, "ffffffffffffffff", fs().now, gD2, &lr));
  check(lr.identityChanged && lr.fresh && gD2.count == 0 && strcmp(gD2.peerKeyFp, "ffffffffffffffff") == 0);
  check(fs().exists("/sd/collect/8cbfea00a773/files.prev-20261122T180311Z/index.json"));
  check(fs().exists("/sd/collect/8cbfea00a773/files.prev-20261122T180311Z/1a2b3c4d-00000001-s0.raw"));
  check(fs().isDirectory(kDir) && fs().list(kDir).empty());
  // No card: nothing loads.
  fs().capacity[1].writable = false;
  check(!collectstore::loadIndex(kMac, "x", fs().now, gD2, &lr));
}

static void testHostileNames() {
  resetFs();
  freshDevice();
  const char* names[] = { "../../system/users.json", "a\\b", "\x01\x02\nevil\"", "/sd/collect/x" };
  uint32_t id = 100;
  for (const char* n : names) {
    cc::FileState& f = addFile(id, 1, n, 20);
    window(id, 0, 20, 182, false, 1);
    check(collectstore::appendWindow(gD, f, gW, fs().now).committed == 20);
    ++id;
  }
  check(collectstore::saveIndex(gD, fs().now));
  check(collectstore::archiveFile(gD, gD.f[0], "dropped", fs().now));
  for (const auto& c : fs().log) {
    check(c.path.rfind("/sd/collect", 0) == 0);
    check(c.path.find("users.json") == std::string::npos && c.path.find("..") == std::string::npos &&
          c.path.find('\\') == std::string::npos && c.path.find("evil") == std::string::npos);
    if (c.op == "write" || c.op == "open") check(c.authScope.empty() || c.authScope == "/sd/collect");
  }
  // Names are kept as escaped JSON strings only (control bytes made '?').
  const std::string text = fs().text(path("index.json"));
  check(text.find("\"name\":\"../../system/users.json\"") != std::string::npos);
  check(text.find("\"name\":\"a\\\\b\"") != std::string::npos);
  check(text.find("\"name\":\"???evil\\\"\"") != std::string::npos);
  PSRAM_JSON_DOC(doc);
  check(!deserializeJson(doc, text));   // still one valid document
  // A tampered index names no path either: segment names are rebuilt from ids.
  std::string tampered = text;
  const std::string from = "\"file\":\"1a2b3c4d-00000064-s0.raw\"";
  const size_t at = tampered.find(from);
  check(at != std::string::npos);
  tampered.replace(at, from.size(), "\"file\":\"../../system/users.json\"");
  fs().put(path("index.json"), tampered);
  fs().log.clear();
  collectstore::LoadResult lr = {};
  check(collectstore::loadIndex(kMac, "00112233aabbccdd", fs().now, gD2, &lr) && gD2.count == 4 && lr.missing == 0);
  for (const auto& c : fs().log) check(c.path.find("users.json") == std::string::npos);
}

static void testQuota() {
  resetFs();
  freshDevice();
  fs().mkdirAll(kDir);
  // Retired segments (another ledger), older first by write time.
  fs().now = 1000; fs().put(path("99999999-00000001-s0.raw"), std::string(1000, 'a'));
  fs().now = 3000; fs().put(path("99999999-00000002-s0.raw"), std::string(1000, 'b'));
  fs().now = 2000; fs().put(path("99999999-00000003-s0.sealed"), std::string(1000, 'c'));
  fs().now = 5000;
  cc::FileState& old = addFile(1, 2, "events.log.1", 2000);
  old.createdBoot = 1;
  window(1, 0, 1000, 182, false, 1);
  collectstore::appendWindow(gD, old, gW, fs().now);
  old.lastSeenEnd = 1000;
  cc::settleState(old);
  cc::FileState& active = addFile(2, 2, "events.log", 5000);
  active.createdBoot = 0;   // older, but being fetched
  window(2, 0, 1000, 182, false, 1);
  collectstore::appendWindow(gD, active, gW, fs().now);
  cc::FileState& newer = addFile(3, 5, "errors.log", 1000);
  newer.createdBoot = 5;
  window(3, 0, 1000, 182, false, 1);
  collectstore::appendWindow(gD, newer, gW, fs().now);
  check(old.state == cc::FState::Closed && newer.state == cc::FState::Closed);
  check(collectstore::saveIndex(gD, fs().now));
  collectstore::CardUse use = {};
  check(collectstore::cardUse(gD, &use) && use.retired == 3000 && use.retiredFiles == 3 && use.indexed >= 3000);
  const uint64_t indexOnly = use.indexed - 3000;   // index.json
  // Over by 1500: the two oldest retired go (1000 then 2000), nothing else.
  collectstore::QuotaResult q = collectstore::enforceQuota(gD, use.indexed + use.retired - 1500, 2, fs().now);
  check(q.removedRetired == 2 && q.evictedFiles == 0);
  check(!fs().exists(path("99999999-00000001-s0.raw")) && !fs().exists(path("99999999-00000003-s0.sealed")) &&
        fs().exists(path("99999999-00000002-s0.raw")));
  // Down to under one file's worth: the last retired, then the oldest closed
  // file (not the one being fetched); it stays in the index as evicted.
  q = collectstore::enforceQuota(gD, 2000 + indexOnly + 500, 2, fs().now);
  check(q.removedRetired == 1 && q.evictedFiles == 1 && (old.flags & cc::kFfEvicted) && old.nSeg == 0);
  check(!fs().exists(path("1a2b3c4d-00000001-s0.raw")) && fs().exists(path("1a2b3c4d-00000002-s0.raw")) &&
        fs().exists(path("1a2b3c4d-00000003-s0.raw")));
  check(cc::bytesLeft(old) == 0);
  const std::string archive = fs().text(path("index-archive.jsonl"));
  check(archive.find("\"reason\":\"evicted\"") != std::string::npos && archive.find("\"fileId\":1,") != std::string::npos);
  // Never the protected file, even when nothing else is left.
  q = collectstore::enforceQuota(gD, 0, 2, fs().now);
  check(fs().exists(path("1a2b3c4d-00000002-s0.raw")) && (newer.flags & cc::kFfEvicted));
  check(fs().unlockedCalls() == 0);
}

static void testArchiveAndRoll() {
  resetFs();
  freshDevice();
  cc::FileState& f = addFile(1, 1, "a.csv", 100000);
  for (uint32_t k = 0; k <= cc::kSegMax; ++k) {
    window(1, f.pos, 10, 182, false, 1);
    const collectstore::WriteResult r = collectstore::appendWindow(gD, f, gW, fs().now);
    check(r.committed == 10 && r.rolled == (k == cc::kSegMax));
    cc::closeSegment(f, f.seg[f.nSeg - 1].seg);   // each window its own segment
  }
  check(collectstore::archiveFile(gD, f, "ledger_reset", fs().now));
  const std::string archive = fs().text(path("index-archive.jsonl"));
  size_t lines = 0;
  for (char c : archive) lines += c == '\n';
  check(lines == 2);
  check(archive.find("\"reason\":\"segment_rolled\"") < archive.find("\"reason\":\"ledger_reset\""));
  check(archive.find("\"segment\":{\"file\":\"1a2b3c4d-00000001-s0.raw\",\"seg\":0,\"from\":0,\"to\":10}") != std::string::npos);
  // Each line is one JSON document with its UTC time.
  size_t start = 0;
  while (start < archive.size()) {
    const size_t end = archive.find('\n', start);
    PSRAM_JSON_DOC(doc);
    check(!deserializeJson(doc, archive.substr(start, end - start)));
    check(std::string(doc["utc"].as<const char*>()) == "2026-11-22T18:02:11Z");
    start = end + 1;
  }
}

static void testWriteFailures() {
  resetFs();
  freshDevice();
  cc::FileState& f = addFile(1, 2, "events.log", 1000);
  // A file left by a cut before the index was saved is never appended to.
  fs().mkdirAll(kDir);
  fs().put(path("1a2b3c4d-00000001-s0.raw"), "stale");
  window(1, 0, 100, 182, false, 1);
  collectstore::WriteResult r = collectstore::appendWindow(gD, f, gW, fs().now);
  check(r.committed == 100 && f.seg[0].seg == 1 && fs().text(path("1a2b3c4d-00000001-s0.raw")) == "stale");
  // A short write: what landed is recorded and the segment takes no more.
  fs().faults.shortWriteAfter[path("1a2b3c4d-00000001-s1.raw")] = 130;
  window(1, 100, 100, 182, false, 1);
  r = collectstore::appendWindow(gD, f, gW, fs().now);
  check(r.ioError && r.committed == 30 && f.pos == 130 && f.seg[0].closed);
  fs().faults.shortWriteAfter.clear();
  window(1, 130, 20, 182, false, 1);
  r = collectstore::appendWindow(gD, f, gW, fs().now);
  check(!r.ioError && r.committed == 20 && f.nSeg == 2 && f.seg[1].seg == 2 && f.seg[1].from == 130);
  // A failed flush records nothing.
  fs().faults.flush.add(path("1a2b3c4d-00000001-s2.raw"));
  window(1, 150, 20, 182, false, 1);
  r = collectstore::appendWindow(gD, f, gW, fs().now);
  check(r.ioError && r.committed == 0 && f.pos == 150 && f.seg[1].closed);   // bytes may have landed: closed
  fs().faults.flush.clear();
  window(1, 150, 20, 182, false, 1);
  r = collectstore::appendWindow(gD, f, gW, fs().now);
  check(!r.ioError && r.committed == 20 && f.nSeg == 3 && f.seg[2].seg == 3 && f.seg[2].from == 150);
  // A sealed record cut short: whole records only, the segment closed.
  cc::FileState& s = addFile(2, 6, "system.log", 1000, cc::kFfSealOnSend);
  fs().faults.shortWriteAfter[path("1a2b3c4d-00000002-s0.sealed")] = 8 + 48 + 154 + 10;
  window(2, 0, 300, 154, true, 1);
  r = collectstore::appendWindow(gD, s, gW, fs().now);
  check(r.ioError && r.committed == 154 && s.pos == 154 && s.seg[0].closed);
  // No card: refused at once.
  fs().faults.shortWriteAfter.clear();
  fs().faults.open.add(path("1a2b3c4d-00000002-s1.sealed"));
  window(2, 154, 10, 154, true, 1);
  r = collectstore::appendWindow(gD, s, gW, fs().now);
  check(r.ioError && r.committed == 0 && s.pos == 154);
  check(fs().liveHandles() == 0);
}

static void testDigest() {
  resetFs();
  freshDevice();
  cc::FileState& f = addFile(1, 2, "events.log.1", 40000);
  std::string all;
  for (uint32_t at = 0; at < 40000; at += 1456) {
    const uint32_t n = 40000 - at < 1456 ? 40000 - at : 1456;
    window(1, at, n, 182, false, uint8_t(at));
    for (uint8_t k = 0; k < gW.frames; ++k) all.append(reinterpret_cast<const char*>(gW.f[k].data), gW.f[k].dataLen);
    check(collectstore::appendWindow(gD, f, gW, fs().now).committed == n);
  }
  check(fs().text(path("1a2b3c4d-00000001-s0.raw")) == all);
  uint8_t got[16], want[32];
  yields = 0;
  check(collectstore::segmentDigest(gD, f, 1000, 39000, got));
  CC_SHA256(all.data() + 1000, 39000, want);
  check(memcmp(got, want, 16) == 0);
  check(yields == 2);   // the card is let go between 16 KB steps
  check(!collectstore::segmentDigest(gD, f, 39000, 2000, got));   // past what is held
  check(fs().unlockedCalls() == 0 && fs().liveHandles() == 0);
}

// collectstatus of a device that is not being pulled: nothing on the card changes.
static void testReadOnlyLoad() {
  resetFs();
  freshDevice();
  cc::FileState& b = addFile(2, 6, "system.log", 1000, cc::kFfSealOnSend);
  window(2, 0, 300, 154, true, 1);
  collectstore::appendWindow(gD, b, gW, fs().now);
  check(collectstore::saveIndex(gD, fs().now));
  const std::string torn = fs().text(path("1a2b3c4d-00000002-s0.sealed")) + std::string(30, 'y');
  fs().put(path("1a2b3c4d-00000002-s0.sealed"), torn);
  fs().log.clear();
  collectstore::LoadResult lr = {};
  check(collectstore::loadIndex(kMac, "00112233aabbccdd", fs().now, gD2, &lr, true));
  check(lr.tornCut == 1 && gD2.count == 1 && cc::findFile(gD2, 2)->pos == 300);
  check(fs().text(path("1a2b3c4d-00000002-s0.sealed")) == torn);
  for (const auto& c : fs().log)
    check(c.op != "write" && c.op != "mkdir" && c.op != "rename" && c.op != "remove" && c.op != "replace" &&
          !(c.op == "open" && c.other != "r"));
  // Another pairing: shown as fresh, nothing moved.
  check(collectstore::loadIndex(kMac, "ffffffffffffffff", fs().now, gD2, &lr, true) && lr.identityChanged && lr.fresh);
  check(fs().exists(path("index.json")));
  // A device never pulled: fresh, and its folder is not made.
  const uint8_t other[6] = { 1, 2, 3, 4, 5, 6 };
  check(collectstore::loadIndex(other, "", fs().now, gD2, &lr, true) && lr.fresh);
  check(!fs().exists("/sd/collect/010203040506"));
}

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  struct Case { const char* name; void (*run)(); } cases[] = {
    { "utc", testUtc },
    { "append_raw", testAppendRawAndFlushOrder },
    { "append_sealed", testAppendSealed },
    { "index_round_trip", testIndexRoundTrip },
    { "recovery", testRecovery },
    { "bad_index_identity", testBadIndexAndIdentity },
    { "hostile_names", testHostileNames },
    { "quota", testQuota },
    { "archive_roll", testArchiveAndRoll },
    { "write_failures", testWriteFailures },
    { "digest", testDigest },
    { "read_only", testReadOnlyLoad },
  };
  for (const Case& c : cases) {
    if (!only.empty() && only != c.name) continue;
    c.run();
  }
  std::fprintf(stderr, "collect store: %u checks passed\n", checks);
  return 0;
}
