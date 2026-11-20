// System_CollectStore.cpp - file catch-up, the collector's SD card side
//
// See System_CollectStore.h. Everything between the two host markers below
// reaches the card only through VFS::*Guarded,
// VFS::StagedWrite and FsLockGuard, so the host suite test_collect_store.py
// runs it as it is over the shared fake VFS; the SHA-256 and the yield
// between digest blocks are the three functions above the markers.
#include "System_BuildConfig.h"

#if ENABLE_ESPNOW && ENABLE_ESPNOW_COLLECT

#include "System_CollectStore.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_attr.h>
#include <stdio.h>
#include <string.h>

#include "System_MacText.h"   // macToDisplay
#include "System_MemUtil.h"   // PSRAM_JSON_DOC
#include "System_Mutex.h"     // FsLockGuard
#include "System_VFS.h"
#include "mbedtls/sha256.h"

namespace collectstore {
namespace {
struct Sha256 { mbedtls_sha256_context c; };
void shaBegin(Sha256& s) { mbedtls_sha256_init(&s.c); mbedtls_sha256_starts(&s.c, 0); }
void shaUpdate(Sha256& s, const uint8_t* p, size_t n) { mbedtls_sha256_update(&s.c, p, n); }
void shaFinish(Sha256& s, uint8_t out[32]) { mbedtls_sha256_finish(&s.c, out); mbedtls_sha256_free(&s.c); }
// Between digest blocks the card is let go (other writers, the watchdog).
void storeYield() { vTaskDelay(1); }
}  // namespace
}  // namespace collectstore

// COLLECT_STORE_HOST_BEGIN
namespace collectstore {
namespace cc = collectcore;
namespace cu = espnowcatchup;

namespace {

constexpr size_t kIoChunk = 1024;
constexpr uint32_t kDigestLockBytes = 16384;   // the card is held for at most this much per digest step
constexpr uint8_t kCandidatesMax = 64;
constexpr uint8_t kFreshSegTries = 16;
const char kIndexName[] = "index.json";
const char kArchiveName[] = "index-archive.jsonl";

// PSRAM: the copy / digest buffer and the quota's candidate list.
EXT_RAM_BSS_ATTR uint8_t sIo[kIoChunk];
struct Candidate { char name[40]; uint32_t size, mtime; };
EXT_RAM_BSS_ATTR Candidate sCand[kCandidatesMax];

AuthContext storeAuth(const char* why) { return VFS::systemAuth(VFS::Scopes::COLLECT_SD, why); }

// ---- UTC without the C library's time zone ---------------------------------
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + int64_t(doe) - 719468;
}
void civilFromDays(int64_t z, int* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = unsigned(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = int(int64_t(yoe) + era * 400 + (*m <= 2));
}
// "20261119T180211Z": the stamp of a files.prev-<stamp> folder.
size_t formatStamp(uint32_t epoch, char* out, size_t cap) {
  int y; unsigned mo, d;
  civilFromDays(int64_t(epoch / 86400u), &y, &mo, &d);
  const uint32_t s = epoch % 86400u;
  const int n = snprintf(out, cap, "%04d%02u%02uT%02u%02u%02uZ", y, mo, d, unsigned(s / 3600), unsigned(s / 60 % 60),
                         unsigned(s % 60));
  return n > 0 && size_t(n) < cap ? size_t(n) : 0;
}

bool parseHex32(const char* s, uint32_t* out) {
  if (!s || strlen(s) != 8) return false;
  uint32_t v = 0;
  for (int i = 0; i < 8; ++i) {
    const char c = s[i];
    unsigned x;
    if (c >= '0' && c <= '9') x = unsigned(c - '0');
    else if (c >= 'a' && c <= 'f') x = unsigned(c - 'a' + 10);
    else return false;
    v = (v << 4) | x;
  }
  *out = v;
  return true;
}
void hex8(uint32_t v, char out[9]) {
  for (int i = 7; i >= 0; --i) { out[i] = "0123456789abcdef"[v & 15u]; v >>= 4; }
  out[8] = 0;
}
void macText(const uint8_t mac[6], char out[18]) { macToDisplay(mac, out, 18); }
const char* leafOf(const char* name) {
  const char* slash = name ? strrchr(name, '/') : nullptr;
  return slash ? slash + 1 : (name ? name : "");
}
// "<ledger8>-<file8>-s<k>.raw|.sealed": the only names this store makes.
bool parseSegmentName(const char* leaf, uint32_t* ledgerId, uint32_t* fileId, uint16_t* seg, bool* sealed) {
  char a[9], b[9];
  if (strlen(leaf) < 8 + 1 + 8 + 2 + 4 || leaf[8] != '-' || leaf[17] != '-' || leaf[18] != 's') return false;
  memcpy(a, leaf, 8); a[8] = 0;
  memcpy(b, leaf + 9, 8); b[8] = 0;
  if (!parseHex32(a, ledgerId) || !parseHex32(b, fileId)) return false;
  const char* p = leaf + 19;
  uint32_t k = 0;
  size_t digits = 0;
  while (*p >= '0' && *p <= '9' && digits < 6) { k = k * 10 + uint32_t(*p - '0'); ++p; ++digits; }
  if (!digits || k > 0xFFFF) return false;
  if (strcmp(p, ".raw") == 0) *sealed = false;
  else if (strcmp(p, ".sealed") == 0) *sealed = true;
  else return false;
  *seg = uint16_t(k);
  return true;
}

size_t joinPath(char* out, size_t cap, const char* dir, const char* leaf) {
  const int n = snprintf(out, cap, "%s/%s", dir, leaf);
  return n > 0 && size_t(n) < cap ? size_t(n) : 0;
}

bool ensureDir(const char* path, const AuthContext& ctx) {
  const String p(path);
  if (VFS::existsGuarded(p, ctx)) return true;
  return VFS::mkdirGuarded(p, ctx);
}
bool ensureDirs(const uint8_t mac[6]) {
  char dev[kPathCap], files[kPathCap];
  if (!deviceDir(mac, dev, sizeof(dev)) || !filesDir(mac, files, sizeof(files))) return false;
  const AuthContext ctx = storeAuth("collect.mkdir");
  FsLockGuard guard("collect.mkdir");
  return ensureDir(VFS::Scopes::COLLECT_SD, ctx) && ensureDir(dev, ctx) && ensureDir(files, ctx);
}

const char* sealKey(const cc::FileState& f) {
  if (f.flags & cc::kFfSealOnSend) return "on_send";
  if (f.flags & cc::kFfSealedAtRest) return "at_rest";
  return "none";
}
uint8_t kindFromName(const char* name) {
  for (uint8_t k = 1; k <= cu::kKindCount; ++k) {
    const char* n = cu::kindName(cu::Kind(k));
    if (name && n && strcmp(name, n) == 0) return k;
  }
  return 0;
}

void putUtc(JsonObject o, const char* key, uint32_t epoch) {
  char t[24];
  if (epoch && formatUtc(epoch, t, sizeof(t))) o[key] = t;
  else o[key] = nullptr;
}

void putSegments(JsonArray arr, const cc::DeviceFiles& d, const cc::FileState& f) {
  for (uint8_t i = 0; i < f.nSeg; ++i) {
    const cc::Segment& s = f.seg[i];
    char leaf[40];
    cu::localSegmentName(d.ledgerId, f.fileId, s.seg, cc::isSealOnSend(f), leaf, sizeof(leaf));
    JsonObject o = arr.add<JsonObject>();
    o["file"] = leaf;
    o["seg"] = s.seg;
    o["from"] = s.from;
    o["to"] = s.to;
    if (s.closed) o["closed"] = true;
  }
}
void putRanges(JsonObject o, const cc::FileState& f) {
  JsonArray gaps = o["gaps"].to<JsonArray>();
  for (uint8_t i = 0; i < f.nGap; ++i) {
    JsonObject g = gaps.add<JsonObject>();
    g["from"] = f.gap[i].from;
    g["to"] = f.gap[i].to;
    g["why"] = cc::gapWhyKey(f.gap[i].why);
  }
  JsonArray ew = o["elsewhere"].to<JsonArray>();
  for (uint8_t i = 0; i < f.nElse; ++i) {
    JsonObject e = ew.add<JsonObject>();
    e["from"] = f.elsewhere[i].from;
    e["to"] = f.elsewhere[i].to;
  }
}

void putFile(JsonObject o, const cc::DeviceFiles& d, const cc::FileState& f) {
  o["fileId"] = f.fileId;
  const char* kind = cu::kindName(cu::Kind(f.kind));
  if (kind) o["kind"] = kind;
  else o["kind"] = f.kind;
  o["name"] = f.name;   // already safe (collectcore::safeName); ArduinoJson escapes it
  o["onSd"] = (f.flags & cc::kFfOnSd) != 0;
  o["seal"] = sealKey(f);
  o["createdBoot"] = f.createdBoot;
  putUtc(o, "firstSeenUtc", f.firstSeenEpoch);
  o["deviceStart"] = f.deviceStart;
  o["lastSeenEnd"] = f.lastSeenEnd;
  o["lastSeenDelivered"] = f.lastSeenDelivered;
  o["pos"] = f.pos;
  o["nextSeg"] = f.nextSeg;
  o["lastListedRun"] = f.lastListedRun;
  putSegments(o["segments"].to<JsonArray>(), d, f);
  putRanges(o, f);
  JsonObject gb = o["gapBytes"].to<JsonObject>();
  for (uint8_t g = 0; g < cc::kGapWhyCount; ++g) gb[cc::gapWhyKey(g)] = f.gapBytes[g];
  o["elsewhereBytes"] = f.elsewhereBytes;
  o["rolledBytes"] = f.rolledBytes;
  o["state"] = cc::fstateKey(f.state);
  const char* dg = cc::digestKey(f.digest);
  if (dg) o["digest"] = dg;
  else o["digest"] = nullptr;
  o["growing"] = (f.flags & cc::kFfGrowing) != 0;
  if (f.flags & cc::kFfTooLarge) o["tooLarge"] = true;
  if (f.flags & cc::kFfNameOmitted) o["nameOmitted"] = true;
  if (f.flags & cc::kFfEvicted) o["evicted"] = true;
  if (f.flags & cc::kFfGoneByRead) o["goneByRead"] = true;
}

uint32_t u32(JsonVariantConst v, uint32_t dflt = 0) { return v.is<uint32_t>() ? v.as<uint32_t>() : dflt; }

// One file of index.json, checked field by field. False: the entry is not used.
bool readFile(JsonObjectConst o, cc::FileState& f) {
  memset(&f, 0, sizeof(f));
  f.fileId = u32(o["fileId"]);
  if (!f.fileId) return false;
  JsonVariantConst kind = o["kind"];
  f.kind = kind.is<const char*>() ? kindFromName(kind.as<const char*>()) : uint8_t(u32(kind));
  if (f.kind < 1 || f.kind > cu::kKindCount) return false;
  const char* name = o["name"].as<const char*>();
  if (name) cc::safeName(name, strlen(name), f.name);
  const char* seal = o["seal"].as<const char*>();
  if (seal && strcmp(seal, "on_send") == 0) f.flags |= cc::kFfSealOnSend;
  else if (seal && strcmp(seal, "at_rest") == 0) f.flags |= cc::kFfSealedAtRest;
  if (o["onSd"].as<bool>()) f.flags |= cc::kFfOnSd;
  if (o["growing"].as<bool>()) f.flags |= cc::kFfGrowing;
  if (o["tooLarge"].as<bool>()) f.flags |= cc::kFfTooLarge;
  if (o["nameOmitted"].as<bool>()) f.flags |= cc::kFfNameOmitted;
  if (o["evicted"].as<bool>()) f.flags |= cc::kFfEvicted;
  if (o["goneByRead"].as<bool>()) f.flags |= cc::kFfGoneByRead;
  f.createdBoot = u32(o["createdBoot"]);
  uint32_t t = 0;
  if (parseUtc(o["firstSeenUtc"].as<const char*>(), &t)) f.firstSeenEpoch = t;
  f.deviceStart = u32(o["deviceStart"]);
  f.lastSeenEnd = u32(o["lastSeenEnd"]);
  f.lastSeenDelivered = u32(o["lastSeenDelivered"]);
  f.pos = u32(o["pos"]);
  f.nextSeg = uint16_t(u32(o["nextSeg"]) > 0xFFFF ? 0xFFFF : u32(o["nextSeg"]));
  f.lastListedRun = u32(o["lastListedRun"]);
  for (JsonObjectConst s : o["segments"].as<JsonArrayConst>()) {
    if (f.nSeg >= cc::kSegMax) break;
    const uint32_t from = u32(s["from"]), to = u32(s["to"], 0), seg = u32(s["seg"], 0x10000);
    if (seg > 0xFFFF || to < from) continue;
    cc::Segment& x = f.seg[f.nSeg++];
    x.from = from; x.to = to; x.seg = uint16_t(seg); x.closed = s["closed"].as<bool>() ? 1 : 0;
    if (x.seg >= f.nextSeg) f.nextSeg = uint16_t(x.seg + 1 > 0xFFFF ? 0xFFFF : x.seg + 1);
  }
  for (JsonObjectConst g : o["gaps"].as<JsonArrayConst>()) {
    uint8_t why = 0;
    const uint32_t from = u32(g["from"]), to = u32(g["to"]);
    if (f.nGap >= cc::kGapMax || to <= from || !cc::gapWhyFromKey(g["why"].as<const char*>(), &why)) continue;
    cc::Gap& x = f.gap[f.nGap++];
    x.from = from; x.to = to; x.why = why;
  }
  for (JsonObjectConst e : o["elsewhere"].as<JsonArrayConst>()) {
    const uint32_t from = u32(e["from"]), to = u32(e["to"]);
    if (f.nElse >= cc::kElseMax || to <= from) continue;
    f.elsewhere[f.nElse].from = from;
    f.elsewhere[f.nElse].to = to;
    ++f.nElse;
  }
  JsonObjectConst gb = o["gapBytes"].as<JsonObjectConst>();
  for (uint8_t g = 0; g < cc::kGapWhyCount; ++g) f.gapBytes[g] = u32(gb[cc::gapWhyKey(g)]);
  f.elsewhereBytes = u32(o["elsewhereBytes"]);
  f.rolledBytes = u32(o["rolledBytes"]);
  cc::FState st = cc::FState::New;
  if (cc::fstateFromKey(o["state"].as<const char*>(), &st)) f.state = st;
  const char* dg = o["digest"].as<const char*>();
  f.digest = !dg ? cc::DigestState::None
             : strcmp(dg, "ok") == 0 ? cc::DigestState::Ok
             : strcmp(dg, "mismatch") == 0 ? cc::DigestState::Mismatch
             : strcmp(dg, "skipped") == 0 ? cc::DigestState::Skipped : cc::DigestState::None;
  return true;
}

// A .sealed segment: the magic, then records whose ledger, file and offsets
// continue from `from`. *validBytes: the bytes up to the last whole record;
// *plainTo: the logical end they cover.
bool scanSealed(File& in, uint32_t ledgerId, uint32_t fileId, uint32_t from, size_t* validBytes, uint32_t* plainTo) {
  *validBytes = 0;
  *plainTo = from;
  const size_t size = in.size();
  uint8_t head[cu::kSealedRecordHeaderLen];
  if (size < sizeof(cu::kSealedSegMagic)) return true;
  if (in.read(head, sizeof(cu::kSealedSegMagic)) != sizeof(cu::kSealedSegMagic) ||
      memcmp(head, cu::kSealedSegMagic, sizeof(cu::kSealedSegMagic)) != 0)
    return false;
  size_t at = sizeof(cu::kSealedSegMagic);
  *validBytes = at;
  uint32_t expect = from;
  while (at + cu::kSealedRecordHeaderLen <= size) {
    if (!in.seek(uint32_t(at))) break;
    if (in.read(head, sizeof(head)) != sizeof(head)) break;
    cu::SealedRecord r;
    if (!cu::parseSealedRecordHeader(head, sizeof(head), &r)) break;
    if (r.ledgerId != ledgerId || r.fileId != fileId || r.offset != expect || r.plainLen == 0) break;
    if (at + r.recLen > size) break;
    at += r.recLen;
    expect += r.plainLen;
    *validBytes = at;
    *plainTo = expect;
  }
  return true;
}

// Cuts `path` to its first `keep` bytes (a sealed segment's torn tail): the
// prefix is copied into a staged file that replaces it.
bool cutTo(const char* path, size_t keep, const AuthContext& ctx) {
  FsLockGuard guard("collect.cut");
  File in = VFS::openGuarded(String(path), "r", ctx);
  if (!in) return false;
  VFS::StagedWrite staged("collect.cut");
  if (!staged.begin(String(path), ctx)) { in.close(); return false; }
  size_t done = 0;
  bool ok = true;
  while (done < keep) {
    const size_t want = keep - done < kIoChunk ? keep - done : kIoChunk;
    if (in.read(sIo, want) != want || staged.file().write(sIo, want) != want) { ok = false; break; }
    done += want;
  }
  in.close();
  if (!ok || staged.file().getWriteError()) { staged.discard(ctx); return false; }
  return staged.commit(ctx);
}

bool removeFile(const char* path, const AuthContext& ctx) {
  FsLockGuard guard("collect.remove");
  const String p(path);
  return !VFS::existsGuarded(p, ctx) || VFS::removeGuarded(p, ctx);
}

bool appendJsonLine(const uint8_t mac[6], JsonDocument& doc) {
  char dir[kPathCap], path[kPathCap];
  if (!filesDir(mac, dir, sizeof(dir)) || !joinPath(path, sizeof(path), dir, kArchiveName)) return false;
  if (!ensureDirs(mac)) return false;
  const AuthContext ctx = storeAuth("collect.archive");
  FsLockGuard guard("collect.archive");
  File out = VFS::openGuarded(String(path), "a", ctx, true);
  if (!out) return false;
  const size_t want = measureJson(doc);
  const size_t wrote = serializeJson(doc, out);
  const bool ok = wrote == want && out.write(uint8_t('\n')) == 1;
  out.flush();
  const bool err = out.getWriteError() != 0;
  out.close();
  return ok && !err;
}

}  // namespace

size_t formatUtc(uint32_t epoch, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  out[0] = 0;
  if (!epoch) return 0;
  int y; unsigned mo, d;
  civilFromDays(int64_t(epoch / 86400u), &y, &mo, &d);
  const uint32_t s = epoch % 86400u;
  const int n = snprintf(out, cap, "%04d-%02u-%02uT%02u:%02u:%02uZ", y, mo, d, unsigned(s / 3600), unsigned(s / 60 % 60),
                         unsigned(s % 60));
  if (n <= 0 || size_t(n) >= cap) { out[0] = 0; return 0; }
  return size_t(n);
}

bool parseUtc(const char* t, uint32_t* epoch) {
  if (!t || strlen(t) != 20 || t[4] != '-' || t[7] != '-' || t[10] != 'T' || t[13] != ':' || t[16] != ':' || t[19] != 'Z')
    return false;
  auto num = [&](int at, int n, unsigned* v) {
    unsigned x = 0;
    for (int i = 0; i < n; ++i) {
      if (t[at + i] < '0' || t[at + i] > '9') return false;
      x = x * 10 + unsigned(t[at + i] - '0');
    }
    *v = x;
    return true;
  };
  unsigned y, mo, d, h, mi, s;
  if (!num(0, 4, &y) || !num(5, 2, &mo) || !num(8, 2, &d) || !num(11, 2, &h) || !num(14, 2, &mi) || !num(17, 2, &s))
    return false;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60 || y < 1970) return false;
  const int64_t v = daysFromCivil(y, mo, d) * 86400 + int64_t(h) * 3600 + int64_t(mi) * 60 + s;
  if (v < 0 || v > int64_t(0xFFFFFFFFu)) return false;
  *epoch = uint32_t(v);
  return true;
}

size_t deviceDir(const uint8_t mac[6], char* out, size_t cap) {
  char m[13];
  cu::mac12(mac, m);
  const int n = snprintf(out, cap, "%s/%s", VFS::Scopes::COLLECT_SD, m);
  return n > 0 && size_t(n) < cap ? size_t(n) : 0;
}
size_t filesDir(const uint8_t mac[6], char* out, size_t cap) {
  char m[13];
  cu::mac12(mac, m);
  const int n = snprintf(out, cap, "%s/%s/files", VFS::Scopes::COLLECT_SD, m);
  return n > 0 && size_t(n) < cap ? size_t(n) : 0;
}
size_t segmentPath(const cc::DeviceFiles& d, const cc::FileState& f, uint16_t seg, char* out, size_t cap) {
  char dir[kPathCap], leaf[40];
  if (!filesDir(d.mac, dir, sizeof(dir))) return 0;
  if (!cu::localSegmentName(d.ledgerId, f.fileId, seg, cc::isSealOnSend(f), leaf, sizeof(leaf))) return 0;
  return joinPath(out, cap, dir, leaf);
}

bool loadIndex(const uint8_t mac[6], const char* peerKeyFp, uint32_t nowEpoch, cc::DeviceFiles& d, LoadResult* out,
               bool readOnly) {
  LoadResult r = {};
  memset(&d, 0, sizeof(d));
  memcpy(d.mac, mac, 6);
  d.used = 1;
  if (peerKeyFp) strlcpy(d.peerKeyFp, peerKeyFp, sizeof(d.peerKeyFp));
  if (readOnly ? !VFS::isSdAvailable() : !VFS::isSdWritable()) { if (out) *out = r; return false; }
  if (!readOnly && !ensureDirs(mac)) { if (out) *out = r; return false; }
  char dir[kPathCap], path[kPathCap];
  if (!filesDir(mac, dir, sizeof(dir)) || !joinPath(path, sizeof(path), dir, kIndexName)) return false;
  const AuthContext ctx = storeAuth("collect.load");

  PSRAM_JSON_DOC(doc);
  bool have = false, bad = false;
  {
    FsLockGuard guard("collect.load");
    File in = VFS::openGuarded(String(path), "r", ctx);
    if (in) {
      const DeserializationError err = deserializeJson(doc, in);
      in.close();
      have = true;
      bad = err || doc["schema"].as<int>() != kIndexSchema || !doc["files"].is<JsonArrayConst>();
    }
  }
  if (have && !bad) {
    char want[18];
    macText(mac, want);
    const char* m = doc["mac"].as<const char*>();
    uint32_t ledger = 0;
    if (!m || strcmp(m, want) != 0 || !parseHex32(doc["ledgerId"].as<const char*>(), &ledger)) bad = true;
    else { d.ledgerId = ledger; d.haveLedger = 1; }
  }
  if (have && bad) {
    // Kept for a look; a fresh index starts.
    char aside[kPathCap];
    if (!readOnly && joinPath(aside, sizeof(aside), dir, "index.json.bad")) {
      FsLockGuard guard("collect.bad");
      VFS::removeGuarded(String(aside), ctx);
      VFS::renameGuarded(String(path), String(aside), ctx);
    }
    r.badIndex = true;
    have = false;
    d.haveLedger = 0;
  }
  const char* storedFp = have ? doc["peerKeyFp"].as<const char*>() : nullptr;
  if (have && storedFp && storedFp[0] && peerKeyFp && peerKeyFp[0] && strcmp(storedFp, peerKeyFp) != 0) {
    // The device was paired again: its earlier files are kept aside.
    if (readOnly) {
      r.identityChanged = true;
      r.fresh = true;
      d.haveLedger = 0;
      d.ledgerId = 0;
      if (out) *out = r;
      return true;
    }
    char stamp[20], prev[kPathCap];
    if (!formatStamp(nowEpoch, stamp, sizeof(stamp))) return false;
    const int n = snprintf(prev, sizeof(prev), "%s.prev-%s", dir, stamp);
    if (n <= 0 || size_t(n) >= sizeof(prev)) return false;
    {
      FsLockGuard guard("collect.identity");
      if (!VFS::renameGuarded(String(dir), String(prev), ctx)) return false;
    }
    if (!ensureDirs(mac)) return false;
    r.identityChanged = true;
    have = false;
    d.haveLedger = 0;
    d.ledgerId = 0;
  }
  if (!have) {
    r.fresh = true;
    if (out) *out = r;
    return true;
  }
  uint32_t t = 0;
  if (parseUtc(doc["lastCaughtUpUtc"].as<const char*>(), &t)) d.lastCaughtUpEpoch = t;
  if (parseUtc(doc["updatedUtc"].as<const char*>(), &t)) d.updatedEpoch = t;
  d.runId = u32(doc["runId"]);
  for (JsonObjectConst o : doc["files"].as<JsonArrayConst>()) {
    if (d.count >= cc::kMaxFiles) break;
    cc::FileState& f = d.f[d.count];
    if (!readFile(o, f) || cc::findFile(static_cast<const cc::DeviceFiles&>(d), f.fileId)) continue;
    ++d.count;
  }
  doc.clear();

  // The card is the truth.
  for (uint8_t k = 0; k < d.count; ++k) {
    cc::FileState& f = d.f[k];
    for (uint8_t i = f.nSeg; i-- > 0;) {
      const cc::Segment s = f.seg[i];
      char seg[kPathCap];
      if (!segmentPath(d, f, s.seg, seg, sizeof(seg))) continue;
      size_t size = 0, valid = 0;
      uint32_t plainTo = s.from;
      bool exists = false, scanned = true;
      {
        FsLockGuard guard("collect.recover");
        File in = VFS::openGuarded(String(seg), "r", ctx);
        if (in) {
          exists = true;
          size = in.size();
          if (cc::isSealOnSend(f)) scanned = scanSealed(in, d.ledgerId, f.fileId, s.from, &valid, &plainTo);
          in.close();
        }
      }
      if (!exists) {
        ++r.missing;
        cc::recoverSegment(f, i, s.from);
        continue;
      }
      uint32_t onCardTo;
      if (cc::isSealOnSend(f)) {
        if (!scanned) { valid = 0; plainTo = s.from; }
        if (valid < size) {
          // A torn tail (power cut mid-record, or not a segment of ours): cut it.
          if (readOnly) ++r.tornCut;
          else if (valid == 0 ? removeFile(seg, ctx) : cutTo(seg, valid, ctx)) ++r.tornCut;
          if (valid == 0) plainTo = s.from;
        }
        onCardTo = plainTo;
      } else {
        const uint64_t to = uint64_t(s.from) + size;
        onCardTo = to > 0xFFFFFFFFu ? 0xFFFFFFFFu : uint32_t(to);
      }
      if (onCardTo != s.to) ++r.recovered;
      if (s.closed && onCardTo > s.to) onCardTo = s.to;   // a closed segment never grows back
      cc::recoverSegment(f, i, onCardTo);
    }
  }
  if (out) *out = r;
  return true;
}

bool saveIndex(cc::DeviceFiles& d, uint32_t nowEpoch) {
  if (!VFS::isSdWritable() || !ensureDirs(d.mac)) return false;
  char dir[kPathCap], path[kPathCap];
  if (!filesDir(d.mac, dir, sizeof(dir)) || !joinPath(path, sizeof(path), dir, kIndexName)) return false;
  d.updatedEpoch = nowEpoch;
  PSRAM_JSON_DOC(doc);
  char mac[18], ledger[9];
  macText(d.mac, mac);
  doc["schema"] = kIndexSchema;
  doc["mac"] = mac;
  doc["peerKeyFp"] = d.peerKeyFp;
  if (d.haveLedger) { hex8(d.ledgerId, ledger); doc["ledgerId"] = ledger; }
  else doc["ledgerId"] = nullptr;
  char t[24];
  if (formatUtc(nowEpoch, t, sizeof(t))) doc["updatedUtc"] = t;
  else doc["updatedUtc"] = nullptr;
  char c[24];
  if (d.lastCaughtUpEpoch && formatUtc(d.lastCaughtUpEpoch, c, sizeof(c))) doc["lastCaughtUpUtc"] = c;
  else doc["lastCaughtUpUtc"] = nullptr;
  doc["runId"] = d.runId;
  JsonArray files = doc["files"].to<JsonArray>();
  for (uint8_t i = 0; i < d.count; ++i) putFile(files.add<JsonObject>(), d, d.f[i]);
  if (doc.overflowed()) return false;
  const AuthContext ctx = storeAuth("collect.index");
  FsLockGuard guard("collect.index");
  VFS::StagedWrite staged("collect.index");
  if (!staged.begin(String(path), ctx)) return false;
  const size_t want = measureJson(doc);
  const size_t wrote = serializeJson(doc, staged.file());
  if (wrote != want || staged.file().getWriteError()) { staged.discard(ctx); return false; }
  return staged.commit(ctx);
}

bool archiveFile(const cc::DeviceFiles& d, const cc::FileState& f, const char* reason, uint32_t nowEpoch) {
  PSRAM_JSON_DOC(doc);
  char t[24], ledger[9];
  if (formatUtc(nowEpoch, t, sizeof(t))) doc["utc"] = t;
  else doc["utc"] = nullptr;
  doc["reason"] = reason ? reason : "retired";
  hex8(d.ledgerId, ledger);
  doc["ledgerId"] = ledger;
  putFile(doc.as<JsonObject>(), d, f);
  return appendJsonLine(d.mac, doc);
}

bool archiveSegment(const cc::DeviceFiles& d, const cc::FileState& f, const cc::Segment& s, const char* reason,
                    uint32_t nowEpoch) {
  PSRAM_JSON_DOC(doc);
  char t[24], ledger[9], leaf[40];
  if (formatUtc(nowEpoch, t, sizeof(t))) doc["utc"] = t;
  else doc["utc"] = nullptr;
  doc["reason"] = reason ? reason : "segment_rolled";
  hex8(d.ledgerId, ledger);
  doc["ledgerId"] = ledger;
  doc["fileId"] = f.fileId;
  doc["name"] = f.name;
  cu::localSegmentName(d.ledgerId, f.fileId, s.seg, cc::isSealOnSend(f), leaf, sizeof(leaf));
  JsonObject seg = doc["segment"].to<JsonObject>();
  seg["file"] = leaf;
  seg["seg"] = s.seg;
  seg["from"] = s.from;
  seg["to"] = s.to;
  return appendJsonLine(d.mac, doc);
}

WriteResult appendWindow(cc::DeviceFiles& d, cc::FileState& f, const cc::WindowAssembler& w, uint32_t nowEpoch) {
  WriteResult r = {};
  if (!w.frames || w.plainBytes() == 0) return r;
  const uint32_t offset = w.startOffset;
  const int8_t hole = offset == f.pos ? int8_t(-1) : cc::elsewhereAt(f, offset);
  if ((offset != f.pos && hole < 0) || w.fileId != f.fileId || w.sealed != cc::isSealOnSend(f) ||
      (hole >= 0 && uint64_t(offset) + w.plainBytes() > f.elsewhere[hole].to)) {
    r.rejected = true;
    return r;
  }
  if (!ensureDirs(d.mac)) { r.ioError = true; return r; }
  const AuthContext ctx = storeAuth("collect.write");
  cc::SegChoice c = cc::segmentFor(f, offset);
  char path[kPathCap];
  const bool sealed = cc::isSealOnSend(f);
  uint32_t committed = 0;
  {
    FsLockGuard guard("collect.write");
    if (!segmentPath(d, f, c.seg, path, sizeof(path))) { r.ioError = true; return r; }
    // A fresh segment never lands on a file left by a cut before the index
    // was saved: that name is skipped (the old file is retired).
    for (uint8_t tries = 0; c.fresh && VFS::existsGuarded(String(path), ctx); ++tries) {
      if (tries >= kFreshSegTries || c.seg == 0xFFFF) { r.ioError = true; return r; }
      ++c.seg;
      if (!segmentPath(d, f, c.seg, path, sizeof(path))) { r.ioError = true; return r; }
    }
    File out = VFS::openGuarded(String(path), "a", ctx, true);
    if (!out) { r.ioError = true; return r; }
    if (c.fresh && sealed) {
      if (out.write(reinterpret_cast<const uint8_t*>(cu::kSealedSegMagic), sizeof(cu::kSealedSegMagic)) !=
          sizeof(cu::kSealedSegMagic))
        r.ioError = true;
    }
    for (uint8_t k = 0; k < w.frames && !r.ioError; ++k) {
      const cc::AsmFrame& fr = w.f[k];
      if (!fr.plainLen) continue;
      if (sealed) {
        uint8_t head[cu::kSealedRecordHeaderLen];
        cu::packSealedRecordHeader(cu::sealedRecordFor(f.kind, d.ledgerId, f.fileId, fr.offset, fr.plainLen), head);
        const size_t a = out.write(head, sizeof(head));
        const size_t b = a == sizeof(head) ? out.write(fr.data, fr.dataLen) : 0;
        if (a != sizeof(head) || b != fr.dataLen) { r.ioError = true; break; }
        committed += fr.plainLen;
      } else {
        const size_t n = out.write(fr.data, fr.dataLen);
        committed += uint32_t(n);
        if (n != fr.dataLen) { r.ioError = true; break; }
      }
    }
    out.flush();
    if (out.getWriteError()) { r.ioError = true; committed = 0; }
    out.close();
  }
  // Only what reached the card whole is recorded; after a failure the
  // segment takes no more (recovery at the next load trims or keeps its tail).
  if (committed) {
    cc::Segment rolled = {};
    bool didRoll = false;
    if (!cc::commitWrite(f, c, offset, committed, &rolled, &didRoll)) {
      r.rejected = true;
      return r;
    }
    r.committed = committed;
    r.opened = c.fresh;
    if (didRoll) {
      r.rolled = true;
      archiveSegment(d, f, rolled, "segment_rolled", nowEpoch);
    }
  }
  if (r.ioError) {
    // Bytes may have landed past what is recorded: this segment is never
    // appended to again (a fresh one that was not recorded is skipped by name).
    if (!c.fresh || committed) cc::closeSegment(f, c.seg);
    else if (f.nextSeg <= c.seg) f.nextSeg = uint16_t(c.seg + 1 > 0xFFFF ? 0xFFFF : c.seg + 1);
  }
  return r;
}

bool segmentDigest(const cc::DeviceFiles& d, const cc::FileState& f, uint32_t from, uint32_t len,
                   uint8_t out[cu::kDigestLen]) {
  if (cc::isSealOnSend(f)) return false;
  const int8_t i = cc::segmentCovering(f, from, from + len);
  if (i < 0 || uint64_t(from) + len > 0xFFFFFFFFu) return false;
  const cc::Segment& s = f.seg[i];
  char path[kPathCap];
  if (!segmentPath(d, f, s.seg, path, sizeof(path))) return false;
  const AuthContext ctx = storeAuth("collect.digest");
  Sha256 sha;
  shaBegin(sha);
  uint32_t done = 0;
  bool ok = true;
  while (ok && done < len) {
    {
      FsLockGuard guard("collect.digest");
      File in = VFS::openGuarded(String(path), "r", ctx);
      if (!in || !in.seek(from - s.from + done)) { ok = false; break; }
      uint32_t step = 0;
      while (ok && done < len && step < kDigestLockBytes) {
        const uint32_t want = len - done < kIoChunk ? len - done : uint32_t(kIoChunk);
        if (in.read(sIo, want) != want) { ok = false; break; }
        shaUpdate(sha, sIo, want);
        done += want;
        step += want;
      }
      in.close();
    }
    if (ok && done < len) storeYield();
  }
  uint8_t full[32];
  shaFinish(sha, full);
  if (!ok) return false;
  memcpy(out, full, cu::kDigestLen);
  return true;
}

namespace {
// True when the folder entry `leaf` is a segment the index names.
bool indexed(const cc::DeviceFiles& d, const char* leaf) {
  uint32_t ledger = 0, fileId = 0;
  uint16_t seg = 0;
  bool sealed = false;
  if (!parseSegmentName(leaf, &ledger, &fileId, &seg, &sealed)) return true;   // index.json, the archive: kept
  if (!d.haveLedger || ledger != d.ledgerId) return false;
  const cc::FileState* f = cc::findFile(d, fileId);
  if (!f || cc::isSealOnSend(*f) != sealed) return false;
  for (uint8_t i = 0; i < f->nSeg; ++i)
    if (f->seg[i].seg == seg) return true;
  return false;
}
// Walks the folder; `onRetired` sees each segment file the index does not name.
template <class F>
bool walk(const cc::DeviceFiles& d, CardUse* use, F onRetired) {
  char dir[kPathCap];
  if (!filesDir(d.mac, dir, sizeof(dir))) return false;
  const AuthContext ctx = storeAuth("collect.walk");
  FsLockGuard guard("collect.walk");
  File folder = VFS::openGuarded(String(dir), "r", ctx);
  if (!folder || !folder.isDirectory()) return false;
  CardUse u = {};
  for (File e = folder.openNextFile(); e; e = folder.openNextFile()) {
    if (e.isDirectory()) { e.close(); continue; }
    const char* leaf = leafOf(e.name());
    const uint32_t size = uint32_t(e.size());
    const uint32_t mtime = uint32_t(e.getLastWrite());
    char name[40];
    strlcpy(name, leaf, sizeof(name));
    e.close();
    if (indexed(d, name)) { u.indexed += size; continue; }
    u.retired += size;
    ++u.retiredFiles;
    onRetired(name, size, mtime);
  }
  folder.close();
  if (use) *use = u;
  return true;
}
}  // namespace

bool cardUse(const cc::DeviceFiles& d, CardUse* out) {
  return walk(d, out, [](const char*, uint32_t, uint32_t) {});
}

QuotaResult enforceQuota(cc::DeviceFiles& d, uint64_t quotaBytes, uint32_t protectFileId, uint32_t nowEpoch) {
  QuotaResult q = {};
  uint8_t n = 0;
  CardUse u = {};
  if (!walk(d, &u, [&](const char* name, uint32_t size, uint32_t mtime) {
        if (n >= kCandidatesMax) return;
        strlcpy(sCand[n].name, name, sizeof(sCand[n].name));
        sCand[n].size = size;
        sCand[n].mtime = mtime;
        ++n;
      }))
    return q;
  q.before = u.indexed + u.retired;
  uint64_t now = q.before;
  if (now <= quotaBytes) { q.after = now; return q; }
  char dir[kPathCap], path[kPathCap];
  if (!filesDir(d.mac, dir, sizeof(dir))) { q.after = now; return q; }
  const AuthContext ctx = storeAuth("collect.quota");
  // 1. Retired segments, oldest first.
  while (now > quotaBytes && n) {
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < n; ++i)
      if (sCand[i].mtime < sCand[oldest].mtime ||
          (sCand[i].mtime == sCand[oldest].mtime && strcmp(sCand[i].name, sCand[oldest].name) < 0))
        oldest = i;
    if (joinPath(path, sizeof(path), dir, sCand[oldest].name) && removeFile(path, ctx)) {
      now = now > sCand[oldest].size ? now - sCand[oldest].size : 0;
      ++q.removedRetired;
    }
    sCand[oldest] = sCand[--n];
  }
  // 2. The oldest closed (or gone) files; never the one being fetched.
  while (now > quotaBytes) {
    int best = -1;
    for (uint8_t i = 0; i < d.count; ++i) {
      const cc::FileState& f = d.f[i];
      if (f.fileId == protectFileId || (f.flags & cc::kFfEvicted) || f.nSeg == 0) continue;
      if (f.state != cc::FState::Closed && f.state != cc::FState::Gone) continue;
      if (best < 0 || f.createdBoot < d.f[best].createdBoot ||
          (f.createdBoot == d.f[best].createdBoot && f.fileId < d.f[best].fileId))
        best = i;
    }
    if (best < 0) break;
    cc::FileState& f = d.f[best];
    archiveFile(d, f, "evicted", nowEpoch);
    uint64_t freed = 0;
    for (uint8_t i = 0; i < f.nSeg; ++i) {
      if (!segmentPath(d, f, f.seg[i].seg, path, sizeof(path))) continue;
      uint32_t size = 0;
      {
        FsLockGuard guard("collect.quota");
        File in = VFS::openGuarded(String(path), "r", ctx);
        if (in) { size = uint32_t(in.size()); in.close(); }
      }
      if (removeFile(path, ctx)) freed += size;
    }
    f.flags |= cc::kFfEvicted;
    f.nSeg = 0;
    f.rolledBytes = 0;
    now = now > freed ? now - freed : 0;
    ++q.evictedFiles;
  }
  q.after = now;
  return q;
}

}  // namespace collectstore
// COLLECT_STORE_HOST_END

#endif  // ENABLE_ESPNOW && ENABLE_ESPNOW_COLLECT
