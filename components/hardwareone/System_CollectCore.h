// System_CollectCore.h - file catch-up, the collector's side: the pure rules
//
// A collector (a mesh master or backup master built with ENABLE_ESPNOW_COLLECT)
// lists the files a named worker offers (CATCHUP_LIST, opcode 82/83) and reads
// them in windows of up to 8 frames (CATCHUP_READ, 84/85) onto its SD card.
// This header holds everything the collector decides without the radio, the
// card or the clock:
//   * what it holds of each file: segments on the card, gaps (bytes it will
//     never get, with the reason) and `elsewhere` ranges (bytes the device says
//     another named master already stored: owner decision Q8, 2026-11-19);
//   * planFromList: a listing against what is held (new files, trims, files
//     that left the device, a ledger reset);
//   * the fetch order (most at risk first), nextRead, and the digest check of
//     a closed file;
//   * WindowAssembler: the frames of one READ window, in order only;
//   * onReadStatus: what a status answer does to a file and to the device;
//   * the Scheduler: one device in the file stage, lowest battery first, the
//     15-minute interval, the boot sweep, backoffs, the run ceiling, the frame
//     rate cap;
//   * progress and the ETA.
//
// Pure: no heap, no OS calls, no logging. Time is passed in. Every struct is
// valid zero-filled, so the firmware keeps them in EXT_RAM_BSS_ATTR (PSRAM).
// The wire, the kinds, the sealed record and the safe local names come from
// System_ESPNow_CatchupCore.h. Host test: test/host/test_collect_core.cpp
// (target collect_core_tests). Spec: FILES_IMPL_SPEC.md section 8.
//
// Master-to-master sync (a later package, BACKCAPTURE_DESIGN.md section 5,
// "Q8 follow-up") reads the `elsewhere` ranges kept here and in index.json.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "System_ESPNow_CatchupCore.h"

namespace collectcore {

namespace cu = espnowcatchup;

// ---------------------------------------------------------------------------
// 1. Constants
// ---------------------------------------------------------------------------

constexpr uint8_t  kMaxDevices = 16;
constexpr uint8_t  kMaxFiles = 48;             // FileStates per device (the device offers at most 32)
constexpr uint8_t  kMaxListed = 48;            // entries kept from one listing
constexpr uint8_t  kSegMax = 8, kGapMax = 8, kElseMax = 4;
constexpr uint32_t kIntervalMs = 15u * 60u * 1000u;     // a pull per device every 15 min
constexpr uint32_t kBootStaggerMs = 1000;               // boot / card sweep: peers 1 s apart
constexpr uint32_t kRunMaxBytes = 64u << 20;            // per run, then yield to the next device
constexpr uint32_t kRunMaxMs = 30u * 60u * 1000u;
constexpr uint32_t kFileCapBytes = 256u << 20;          // a file is fetched to 256 MB, then "too large"
constexpr uint32_t kTimeoutDirectMs = 2000, kTimeoutRoutedMs = 4000;
constexpr uint8_t  kTries = 3;                          // per request, then SUSPENDED ("no answer")
constexpr uint32_t kNotReadyBackoffMs = 60000;
constexpr uint32_t kPastEndRelistMs = cu::kFullListMinMs;   // the device allows a full LIST every 60 s
constexpr uint32_t kIndexSaveMs = 30000;                // index.json while it changed
constexpr uint8_t  kWindowFrames = cu::kWindowMaxFrames;
constexpr uint16_t kFrameDataMax = uint16_t(cu::kDirectPayload - cu::kReadRespHeaderLen);   // 182
constexpr uint8_t  kBatteryUnknown = 0xFF;
constexpr uint32_t kQuotaCheckBytes = 1u << 20;         // re-check the card quota every 1 MB written

// ---------------------------------------------------------------------------
// 2. What is held of one file
// ---------------------------------------------------------------------------

enum GapWhy : uint8_t { kGapBeforeFirstFetch = 0, kGapTrimmed = 1, kGapLostOnDevice = 2, kGapNoLongerOffered = 3 };
constexpr uint8_t kGapWhyCount = 4;
inline const char* gapWhyKey(uint8_t why) {
  switch (why) {
    case kGapBeforeFirstFetch: return "before_first_fetch";
    case kGapTrimmed:          return "trimmed";
    case kGapLostOnDevice:     return "lost_on_device";
    case kGapNoLongerOffered:  return "no_longer_offered";
    default:                   return nullptr;
  }
}
inline bool gapWhyFromKey(const char* key, uint8_t* why) {
  for (uint8_t i = 0; i < kGapWhyCount; ++i)
    if (key && strcmp(key, gapWhyKey(i)) == 0) { *why = i; return true; }
  return false;
}

enum class FState : uint8_t { New = 0, Fetching = 1, Current = 2, Closed = 3, Gone = 4 };
inline const char* fstateKey(FState s) {
  switch (s) {
    case FState::New:      return "new";
    case FState::Fetching: return "fetching";
    case FState::Current:  return "current";
    case FState::Closed:   return "closed";
    case FState::Gone:     return "gone";
  }
  return "new";
}
inline bool fstateFromKey(const char* k, FState* out) {
  for (uint8_t i = 0; i <= uint8_t(FState::Gone); ++i)
    if (k && strcmp(k, fstateKey(FState(i))) == 0) { *out = FState(i); return true; }
  return false;
}

// Digest of a closed plain file: not yet, ok, mismatch, or skipped (the device
// refused it: a range over 1 MiB, an unsupported kind, its once-per-session limit).
enum class DigestState : uint8_t { None = 0, Ok = 1, Mismatch = 2, Skipped = 3 };
inline const char* digestKey(DigestState d) {
  switch (d) {
    case DigestState::Ok:       return "ok";
    case DigestState::Mismatch: return "mismatch";
    case DigestState::Skipped:  return "skipped";
    default:                    return nullptr;   // JSON null
  }
}

// FileState.flags
constexpr uint8_t kFfSealedAtRest = 0x01, kFfSealOnSend = 0x02, kFfOnSd = 0x04, kFfGrowing = 0x08,
                  kFfTooLarge = 0x10, kFfNameOmitted = 0x20, kFfEvicted = 0x40,
                  kFfGoneByRead = 0x80;   // a READ answered gone; a complete listing confirms it

struct Segment { uint32_t from, to; uint16_t seg; uint8_t closed, _pad; };   // logical [from, to) in <..>-s<seg>
struct Gap { uint32_t from, to; uint8_t why, _pad[3]; };
struct Range { uint32_t from, to; };

struct FileState {              // ~400 B; PSRAM, at most kMaxFiles per device
  uint32_t fileId;
  uint8_t  kind, flags;
  FState   state;
  DigestState digest;
  uint32_t createdBoot;
  char     name[cu::kNameMax + 1];   // the device's name, made safe for display (safeName); never a path
  uint32_t deviceStart, lastSeenEnd, lastSeenDelivered;
  uint32_t firstSeenEpoch;      // UTC seconds, when this collector first listed the file
  uint32_t lastListedRun;
  uint32_t pos;                 // everything below is held, a gap or elsewhere: the next forward offset
  uint32_t digestTriedRun;      // a digest answered busy/refused in this run: not asked again in it
  uint16_t nextSeg;
  uint8_t  nSeg, nGap, nElse;
  Segment  seg[kSegMax];
  Gap      gap[kGapMax];
  Range    elsewhere[kElseMax];
  uint32_t gapBytes[kGapWhyCount];   // totals; kept exact when ranges merge
  uint32_t elsewhereBytes;
  uint32_t rolledBytes;         // bytes of segments that left the array (still on the card, in the archive)
};

struct DeviceFiles {            // the active device; ~19 KB PSRAM
  uint8_t  mac[6];
  uint8_t  used, haveLedger;
  uint32_t ledgerId;
  char     peerKeyFp[17];       // 16 hex of the paired Ed25519 key: a new pairing starts afresh
  uint32_t lastCaughtUpEpoch, updatedEpoch;
  uint32_t runId;
  uint8_t  count;
  FileState f[kMaxFiles];
};

inline bool isSealOnSend(const FileState& f) { return (f.flags & kFfSealOnSend) != 0; }
inline uint32_t effectiveEnd(const FileState& f) {
  return f.lastSeenEnd > kFileCapBytes ? kFileCapBytes : f.lastSeenEnd;
}
inline uint32_t heldBytes(const FileState& f) {
  uint64_t n = f.rolledBytes;
  for (uint8_t i = 0; i < f.nSeg; ++i) n += f.seg[i].to - f.seg[i].from;
  return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : uint32_t(n);
}
inline uint32_t bytesLeft(const FileState& f) {
  if (f.state == FState::Gone || (f.flags & kFfEvicted)) return 0;
  const uint32_t e = effectiveEnd(f);
  return e > f.pos ? e - f.pos : 0;
}

// The device's name, kept for display and index.json: valid UTF-8 is kept,
// NUL, control bytes, DEL and every byte of an invalid sequence become '?'.
// At most 96 bytes; always NUL-terminated. Never used as a path.
inline size_t safeName(const char* in, size_t n, char out[cu::kNameMax + 1]) {
  size_t w = 0, i = 0;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(in);
  if (n > cu::kNameMax) n = cu::kNameMax;
  while (p && i < n) {
    const uint8_t c = p[i];
    if (c < 0x20 || c == 0x7F) { out[w++] = '?'; ++i; continue; }
    const size_t sl = cu::detail::utf8Seq(p + i, n - i);
    if (sl == 0) { out[w++] = '?'; ++i; continue; }
    memcpy(out + w, p + i, sl);
    w += sl;
    i += sl;
  }
  out[w] = 0;
  return w;
}

namespace detail {
template <class T>
inline void removeAt(T* a, uint8_t* n, uint8_t i) {
  for (uint8_t k = i; k + 1 < *n; ++k) a[k] = a[k + 1];
  if (*n) --*n;
}
}  // namespace detail

// A gap [from, to) with its reason. Joins the last gap when it continues it
// with the same reason; when the array is full the two nearest gaps merge
// (the totals in gapBytes stay exact; the merged range takes the larger
// part's reason).
inline void addGap(FileState& f, uint32_t from, uint32_t to, uint8_t why) {
  if (to <= from || why >= kGapWhyCount) return;
  f.gapBytes[why] += to - from;
  if (f.nGap && f.gap[f.nGap - 1].to == from && f.gap[f.nGap - 1].why == why) { f.gap[f.nGap - 1].to = to; return; }
  if (f.nGap == kGapMax) {
    uint8_t best = 0;
    uint32_t bestSpan = 0xFFFFFFFFu;
    for (uint8_t i = 0; i + 1 < f.nGap; ++i) {
      const uint32_t span = f.gap[i + 1].from > f.gap[i].to ? f.gap[i + 1].from - f.gap[i].to : 0;
      if (span < bestSpan) { bestSpan = span; best = i; }
    }
    Gap& a = f.gap[best];
    const Gap& b = f.gap[best + 1];
    if ((b.to - b.from) > (a.to - a.from)) a.why = b.why;
    if (b.from < a.from) a.from = b.from;
    if (b.to > a.to) a.to = b.to;
    detail::removeAt(f.gap, &f.nGap, uint8_t(best + 1));
  }
  Gap g = {};
  g.from = from; g.to = to; g.why = why;
  f.gap[f.nGap++] = g;
}

inline void addElsewhere(FileState& f, uint32_t from, uint32_t to) {
  if (to <= from) return;
  f.elsewhereBytes += to - from;
  if (f.nElse && f.elsewhere[f.nElse - 1].to == from) { f.elsewhere[f.nElse - 1].to = to; return; }
  if (f.nElse == kElseMax) {
    uint8_t best = 0;
    uint32_t bestSpan = 0xFFFFFFFFu;
    for (uint8_t i = 0; i + 1 < f.nElse; ++i) {
      const uint32_t span = f.elsewhere[i + 1].from - f.elsewhere[i].to;
      if (span < bestSpan) { bestSpan = span; best = i; }
    }
    // The merged range claims the bytes between as elsewhere too: count them.
    f.elsewhereBytes += f.elsewhere[best + 1].from - f.elsewhere[best].to;
    f.elsewhere[best].to = f.elsewhere[best + 1].to;
    detail::removeAt(f.elsewhere, &f.nElse, uint8_t(best + 1));
  }
  f.elsewhere[f.nElse].from = from;
  f.elsewhere[f.nElse].to = to;
  ++f.nElse;
}

// Before a `full` run: an elsewhere range that straddles the device's start
// is split, so its part the device still holds can be filled.
inline void splitElsewhereAtStart(FileState& f) {
  for (uint8_t k = 0; k < f.nElse; ++k) {
    Range& e = f.elsewhere[k];
    if (e.from < f.deviceStart && e.to > f.deviceStart && f.nElse < kElseMax) {
      for (uint8_t j = f.nElse; j > k + 1; --j) f.elsewhere[j] = f.elsewhere[j - 1];
      f.elsewhere[k + 1].from = f.deviceStart;
      f.elsewhere[k + 1].to = e.to;
      e.to = f.deviceStart;
      ++f.nElse;
      return;
    }
  }
}

// The state that follows from what is held: Fetching while bytes are left,
// otherwise Current (still growing on the device) or Closed. A rotated
// sibling ("x.log.2") is closed even when the device saw it grow lately.
inline bool rotatedSibling(const FileState& f) {
  size_t stem = 0;
  return cu::detail::rotationIndex(f.name, strlen(f.name), &stem) > 0;
}
inline void settleState(FileState& f) {
  if (f.state == FState::Gone) return;
  if (f.pos < effectiveEnd(f)) { f.state = FState::Fetching; return; }
  const bool growing = (f.flags & kFfGrowing) && !rotatedSibling(f) && !(f.flags & kFfTooLarge);
  f.state = growing ? FState::Current : FState::Closed;
}

// ---------------------------------------------------------------------------
// 3. Segments: where a window's bytes go on the card
// ---------------------------------------------------------------------------

// A window at `offset` extends the open segment that ends there, or starts a
// new one (number nextSeg). The store writes the bytes, then commitWrite.
struct SegChoice { uint16_t seg; int8_t index; bool fresh; };
inline SegChoice segmentFor(const FileState& f, uint32_t offset) {
  for (uint8_t i = 0; i < f.nSeg; ++i)
    if (!f.seg[i].closed && f.seg[i].to == offset && f.seg[i].to > f.seg[i].from)
      return { f.seg[i].seg, int8_t(i), false };
  return { f.nextSeg, -1, true };
}

// The elsewhere range a hole-filling read starts at (`collectnow ... full`).
inline int8_t elsewhereAt(const FileState& f, uint32_t offset) {
  for (uint8_t i = 0; i < f.nElse; ++i)
    if (f.elsewhere[i].from == offset && f.elsewhere[i].to > offset) return int8_t(i);
  return -1;
}

// Records that [offset, offset+len) is on the card in segment `c`. The offset
// must be the forward position or the start of an elsewhere range (a hole
// filled on a `full` run). A fresh segment on a full array pushes the oldest
// out (*rolled; its file stays on the card, the store writes an archive line).
// False: the window is not where the file stands (nothing changes).
inline bool commitWrite(FileState& f, const SegChoice& c, uint32_t offset, uint32_t len,
                        Segment* rolled = nullptr, bool* didRoll = nullptr) {
  if (didRoll) *didRoll = false;
  if (len == 0) return true;
  const bool forward = offset == f.pos;
  const int8_t hole = forward ? int8_t(-1) : elsewhereAt(f, offset);
  if (!forward && hole < 0) return false;
  if (hole >= 0 && uint64_t(offset) + len > f.elsewhere[hole].to) return false;
  if (uint64_t(offset) + len > 0xFFFFFFFFu) return false;
  if (c.fresh) {
    if (f.nSeg == kSegMax) {
      if (rolled) *rolled = f.seg[0];
      if (didRoll) *didRoll = true;
      f.rolledBytes += f.seg[0].to - f.seg[0].from;
      detail::removeAt(f.seg, &f.nSeg, 0);
    }
    Segment s = {};
    s.from = offset; s.to = offset + len; s.seg = c.seg;
    f.seg[f.nSeg++] = s;
    if (c.seg >= f.nextSeg) f.nextSeg = uint16_t(c.seg + 1);
  } else {
    if (c.index < 0 || c.index >= int8_t(f.nSeg) || f.seg[c.index].to != offset) return false;
    f.seg[c.index].to += len;
  }
  if (forward) {
    f.pos += len;
  } else {
    Range& r = f.elsewhere[hole];
    r.from += len;
    f.elsewhereBytes = f.elsewhereBytes >= len ? f.elsewhereBytes - len : 0;
    if (r.from >= r.to) detail::removeAt(f.elsewhere, &f.nElse, uint8_t(hole));
  }
  settleState(f);
  return true;
}

// A segment whose tail could not be written whole (a sealed record cut short):
// later windows start a new segment.
inline void closeSegment(FileState& f, uint16_t segNo) {
  for (uint8_t i = 0; i < f.nSeg; ++i)
    if (f.seg[i].seg == segNo) f.seg[i].closed = 1;
}

// Recovery at index load: the card is the truth. `onCardTo` is the logical end
// the segment file really covers (a .raw: from + its size; a .sealed: the end
// of its last whole record). A shorter file pulls the forward position back
// when the segment was its end; a longer one extends it when nothing was
// recorded after the segment, and otherwise is closed (its extra bytes are
// not trusted). A segment with no bytes left is dropped.
inline void recoverSegment(FileState& f, uint8_t i, uint32_t onCardTo) {
  if (i >= f.nSeg) return;
  Segment& s = f.seg[i];
  if (onCardTo < s.from) onCardTo = s.from;
  const uint32_t oldTo = s.to;
  if (onCardTo < oldTo) {
    if (f.pos == oldTo) f.pos = onCardTo;
    s.to = onCardTo;
    s.closed = 1;
  } else if (onCardTo > oldTo) {
    if (f.pos == oldTo) { s.to = onCardTo; f.pos = onCardTo; }
    else s.closed = 1;
  }
  if (s.to == s.from) detail::removeAt(f.seg, &f.nSeg, i);
  if (f.state != FState::Gone) settleState(f);
}

// ---------------------------------------------------------------------------
// 4. Planning from a listing
// ---------------------------------------------------------------------------

struct Listed {
  uint8_t  kind, flags, nameLen;
  uint32_t fileId, start, end, createdBoot, delivered;
  char     name[cu::kNameMax + 1];   // raw bytes as sent (nameLen of them), NUL after
};
struct Listing {
  uint32_t ledgerId, ledgerGen;
  uint16_t bootTag;
  uint8_t  respFlags;               // OR of the pages' header flags (kLf*)
  uint8_t  count, pages;
  bool     complete;                // every page from fileId 0 to nextFileId 0 arrived
  bool     overflow;                // more entries than kMaxListed
  bool     haveHeader;
  Listed   e[kMaxListed];
};
inline void listingReset(Listing& l) { memset(&l, 0, sizeof(l)); }
// One page's header. A ledgerId that changes between pages restarts the listing.
inline bool listingPage(Listing& l, const cu::ListRespHeader& h) {
  if (l.haveHeader && h.ledgerId != l.ledgerId) {
    listingReset(l);
    return false;
  }
  l.haveHeader = true;
  l.ledgerId = h.ledgerId;
  l.ledgerGen = h.ledgerGen;
  l.bootTag = h.bootTag;
  l.respFlags |= h.flags;
  ++l.pages;
  return true;
}
inline bool listingAdd(Listing& l, const cu::Entry& e) {
  for (uint8_t i = 0; i < l.count; ++i)
    if (l.e[i].fileId == e.fileId) return true;   // a page asked twice
  if (l.count >= kMaxListed) { l.overflow = true; return false; }
  Listed& d = l.e[l.count++];
  memset(&d, 0, sizeof(d));
  d.kind = e.kind; d.flags = e.flags; d.fileId = e.fileId; d.start = e.start; d.end = e.end;
  d.createdBoot = e.createdBoot; d.delivered = e.delivered;
  d.nameLen = e.nameLen > cu::kNameMax ? uint8_t(cu::kNameMax) : e.nameLen;
  if (d.nameLen && e.name) memcpy(d.name, e.name, d.nameLen);
  d.name[d.nameLen] = 0;
  return true;
}

inline FileState* findFile(DeviceFiles& d, uint32_t fileId) {
  for (uint8_t i = 0; i < d.count; ++i)
    if (d.f[i].fileId == fileId) return &d.f[i];
  return nullptr;
}
inline const FileState* findFile(const DeviceFiles& d, uint32_t fileId) {
  for (uint8_t i = 0; i < d.count; ++i)
    if (d.f[i].fileId == fileId) return &d.f[i];
  return nullptr;
}

// A different ledgerId: the device made a new ledger (lost or reset). The
// caller archives every file (one index-archive.jsonl line each), then
// retireAll starts the new namespace.
inline bool ledgerChanged(const DeviceFiles& d, uint32_t ledgerId) { return d.haveLedger && d.ledgerId != ledgerId; }
inline void retireAll(DeviceFiles& d, uint32_t newLedgerId) {
  d.count = 0;
  memset(d.f, 0, sizeof(d.f));
  d.ledgerId = newLedgerId;
  d.haveLedger = 1;
  d.lastCaughtUpEpoch = 0;
}

struct PlanOut {
  uint8_t added, updated, gone, revived, dropped;
  uint8_t evictedFromList;      // Gone files pushed out of a full list (archive them first: evictIndex)
  bool    ledgerSet;
};

// The slot a new file takes in a full list: the oldest Gone file. -1: none.
inline int listSlotToFree(const DeviceFiles& d) {
  int best = -1;
  for (uint8_t i = 0; i < d.count; ++i) {
    const FileState& f = d.f[i];
    if (f.state != FState::Gone && !(f.flags & kFfEvicted)) continue;
    if (best < 0 || f.fileId < d.f[best].fileId) best = i;
  }
  return best;
}
inline void removeFile(DeviceFiles& d, uint8_t i) { detail::removeAt(d.f, &d.count, i); }

// Applies a listing (FILES_IMPL_SPEC 8.2). `full`: this run fetches what
// another master already has (collectnow ... full), so `delivered` is not
// skipped. `l.complete`: every page from 0 arrived, so a known file that is
// not listed has left the device's offer. The caller handles a ledger change
// first (ledgerChanged / retireAll) and archives what `onEvict` reports
// before a Gone file is pushed out of a full list.
template <class OnEvict>
inline PlanOut planFromList(DeviceFiles& d, const Listing& l, bool full, uint32_t nowEpoch, OnEvict onEvict) {
  PlanOut out = {};
  if (!d.haveLedger) { d.ledgerId = l.ledgerId; d.haveLedger = 1; out.ledgerSet = true; }
  ++d.runId;
  for (uint8_t k = 0; k < l.count; ++k) {
    const Listed& e = l.e[k];
    if (e.kind < 1 || e.kind > cu::kKindCount || e.fileId == 0) { ++out.dropped; continue; }
    FileState* f = findFile(d, e.fileId);
    if (!f) {
      if (d.count >= kMaxFiles) {
        const int slot = listSlotToFree(d);
        if (slot < 0) { ++out.dropped; continue; }
        onEvict(d.f[slot]);
        removeFile(d, uint8_t(slot));
        ++out.evictedFromList;
      }
      f = &d.f[d.count++];
      memset(f, 0, sizeof(*f));
      f->fileId = e.fileId;
      f->firstSeenEpoch = nowEpoch;
      f->createdBoot = e.createdBoot;
      f->state = FState::New;
      if (e.start > 0) { addGap(*f, 0, e.start, kGapBeforeFirstFetch); f->pos = e.start; }
      ++out.added;
    } else {
      if (f->state == FState::Gone && !(f->flags & kFfEvicted)) {
        // Back in the offer (a card put back, the horizon moved): the
        // tentative no_longer_offered gaps at or after the position go.
        for (uint8_t i = f->nGap; i-- > 0;) {
          const Gap& g = f->gap[i];
          if (g.why == kGapNoLongerOffered && g.from >= f->pos) {
            f->gapBytes[kGapNoLongerOffered] -= g.to - g.from;
            detail::removeAt(f->gap, &f->nGap, i);
          }
        }
        f->state = FState::Fetching;
        ++out.revived;
      }
      ++out.updated;
    }
    f->kind = e.kind;
    uint8_t flags = uint8_t(f->flags & (kFfEvicted | kFfTooLarge));
    if (e.flags & cu::kEfSealedAtRest) flags |= kFfSealedAtRest;
    if (e.flags & cu::kEfSealedOnSend) flags |= kFfSealOnSend;
    if (e.flags & cu::kEfOnSd) flags |= kFfOnSd;
    if (e.flags & cu::kEfGrowing) flags |= kFfGrowing;
    if (e.flags & cu::kEfNameOmitted) flags |= kFfNameOmitted;
    // A file is stored one way for good: a segment never mixes plain and sealed.
    if (f->nSeg && ((f->flags ^ flags) & kFfSealOnSend)) flags = uint8_t((flags & ~kFfSealOnSend) | (f->flags & kFfSealOnSend));
    if (e.end > kFileCapBytes) flags |= kFfTooLarge;
    f->flags = flags;
    if (e.nameLen) safeName(e.name, e.nameLen, f->name);
    f->createdBoot = e.createdBoot;
    f->deviceStart = e.start;
    f->lastSeenEnd = e.end;
    f->lastSeenDelivered = e.delivered;
    f->lastListedRun = d.runId;
    if (f->flags & kFfEvicted) continue;   // dropped for the card quota: kept so it is not fetched again
    if (e.start > f->pos) { addGap(*f, f->pos, e.start, kGapTrimmed); f->pos = e.start; }
    if (!full) {
      uint32_t del = e.delivered > e.end ? e.end : e.delivered;
      if (del > effectiveEnd(*f)) del = effectiveEnd(*f);
      if (del > f->pos) { addElsewhere(*f, f->pos, del); f->pos = del; }
    }
    if (full) splitElsewhereAtStart(*f);
    settleState(*f);
  }
  if (l.complete && !l.overflow) {
    for (uint8_t i = 0; i < d.count; ++i) {
      FileState& f = d.f[i];
      if (f.lastListedRun == d.runId) continue;
      if (f.state == FState::Gone && (f.flags & kFfGoneByRead)) {
        // The device said gone and a complete listing agrees: the tentative
        // gaps at or after the position are lost on the device.
        for (uint8_t g = 0; g < f.nGap; ++g) {
          Gap& x = f.gap[g];
          if (x.why != kGapNoLongerOffered || x.from < f.pos) continue;
          const uint32_t n = x.to - x.from;
          f.gapBytes[kGapNoLongerOffered] = f.gapBytes[kGapNoLongerOffered] > n ? f.gapBytes[kGapNoLongerOffered] - n : 0;
          f.gapBytes[kGapLostOnDevice] += n;
          x.why = kGapLostOnDevice;
          if (x.to > f.pos) f.pos = x.to;
        }
        f.flags &= uint8_t(~kFfGoneByRead);
        continue;
      }
      if (f.state == FState::Gone) continue;
      if (f.pos < f.lastSeenEnd) addGap(f, f.pos, f.lastSeenEnd, kGapNoLongerOffered);
      f.state = FState::Gone;
      ++out.gone;
    }
  }
  return out;
}
inline PlanOut planFromList(DeviceFiles& d, const Listing& l, bool full, uint32_t nowEpoch) {
  return planFromList(d, l, full, nowEpoch, [](const FileState&) {});
}

// ---------------------------------------------------------------------------
// 5. What to read next
// ---------------------------------------------------------------------------

// Most at risk first: the highest rotation suffix (".N", the next to be
// deleted), then the capped logs (events, errors: trimmed at the head), then
// the oldest createdBoot, then fileId.
inline bool riskBefore(const FileState& a, const FileState& b) {
  size_t s = 0;
  const unsigned ra = cu::detail::rotationIndex(a.name, strlen(a.name), &s);
  const unsigned rb = cu::detail::rotationIndex(b.name, strlen(b.name), &s);
  if (ra != rb) return ra > rb;
  const bool ca = a.kind == uint8_t(cu::Kind::EventsLog) || a.kind == uint8_t(cu::Kind::ErrorLogs);
  const bool cb = b.kind == uint8_t(cu::Kind::EventsLog) || b.kind == uint8_t(cu::Kind::ErrorLogs);
  if (ca != cb) return ca;
  if (a.createdBoot != b.createdBoot) return a.createdBoot < b.createdBoot;
  return a.fileId < b.fileId;
}

// The single segment that holds [from, to), or -1.
inline int8_t segmentCovering(const FileState& f, uint32_t from, uint32_t to) {
  for (uint8_t i = 0; i < f.nSeg; ++i)
    if (f.seg[i].from <= from && f.seg[i].to >= to) return int8_t(i);
  return -1;
}
// A closed plain file held contiguously up to its end, not checked yet: its
// last <= 1 MiB is compared with the device's digest. *offset/*len: the range.
inline bool digestDue(const FileState& f, uint32_t runId, uint32_t* offset, uint32_t* len) {
  if (f.state != FState::Closed || f.digest != DigestState::None || isSealOnSend(f)) return false;
  if (f.flags & (kFfTooLarge | kFfEvicted)) return false;
  if (f.digestTriedRun == runId && runId != 0) return false;
  const uint32_t end = f.lastSeenEnd;
  if (end <= f.deviceStart || f.pos != end) return false;
  uint32_t from = end - f.deviceStart > cu::kDigestMaxBytes ? end - cu::kDigestMaxBytes : f.deviceStart;
  const int8_t s = segmentCovering(f, from, end);
  if (s < 0) return false;
  if (offset) *offset = from;
  if (len) *len = end - from;
  return true;
}

struct ReadPlan {
  bool     any;
  uint8_t  fileIndex;
  uint8_t  mode;               // cu::kModeData or kModeDigest
  bool     holeFill;           // a `full` run fetching an elsewhere range
  bool     claimOnly;          // a READ at the held end that only carries storedBefore (Q8)
  uint32_t fileId, offset, maxBytes;
  uint8_t  flags;              // cu::kRqStoredBefore
};

// Plaintext bytes per frame on a path of `pathCap` payload bytes.
inline uint16_t plainPerFrame(uint16_t pathCap, bool sealOnSend) {
  const uint16_t over = uint16_t(cu::kReadRespHeaderLen + (sealOnSend ? cu::kSealOverhead : 0));
  uint16_t cap = pathCap > cu::kDirectPayload ? cu::kDirectPayload : pathCap;
  return cap > over ? uint16_t(cap - over) : 0;
}

// The next READ for this device: forward fetches first (most at risk), then
// holes on a `full` run, then digests. `flushed`: every byte the collector
// took has reached the card, so the request may carry storedBefore (the Q8
// delivery claim: the device may tell the other master to skip below offset).
inline ReadPlan nextRead(const DeviceFiles& d, uint16_t pathCap, bool flushed, bool fillHoles) {
  ReadPlan r = {};
  int best = -1;
  for (uint8_t i = 0; i < d.count; ++i) {
    const FileState& f = d.f[i];
    if (f.state == FState::Gone || (f.flags & kFfEvicted) || f.pos >= effectiveEnd(f)) continue;
    if (best < 0 || riskBefore(f, d.f[best])) best = i;
  }
  if (best >= 0) {
    const FileState& f = d.f[best];
    const uint16_t ppf = plainPerFrame(pathCap, isSealOnSend(f));
    if (!ppf) return r;
    uint32_t want = effectiveEnd(f) - f.pos;
    const uint32_t most = uint32_t(kWindowFrames) * ppf;
    r.any = true;
    r.fileIndex = uint8_t(best);
    r.fileId = f.fileId;
    r.mode = cu::kModeData;
    r.offset = f.pos;
    r.maxBytes = want < most ? want : most;
    r.flags = (flushed && f.pos > 0) ? cu::kRqStoredBefore : 0;
    return r;
  }
  if (fillHoles) {
    for (uint8_t i = 0; i < d.count; ++i) {
      const FileState& f = d.f[i];
      if (f.state == FState::Gone || (f.flags & kFfEvicted)) continue;
      for (uint8_t k = 0; k < f.nElse; ++k) {
        const Range& e = f.elsewhere[k];
        if (e.to <= f.deviceStart) continue;   // the device no longer holds it
        const uint32_t from = e.from < f.deviceStart ? f.deviceStart : e.from;
        if (from != e.from) continue;          // the part below start stays elsewhere: see trimElsewhere
        const uint16_t ppf = plainPerFrame(pathCap, isSealOnSend(f));
        if (!ppf) return r;
        const uint32_t most = uint32_t(kWindowFrames) * ppf;
        r.any = true;
        r.fileIndex = i;
        r.fileId = f.fileId;
        r.mode = cu::kModeData;
        r.holeFill = true;
        r.offset = from;
        r.maxBytes = e.to - from < most ? e.to - from : most;
        r.flags = (flushed && f.pos > 0) ? cu::kRqStoredBefore : 0;
        return r;
      }
    }
  }
  for (uint8_t i = 0; i < d.count; ++i) {
    uint32_t off = 0, len = 0;
    if (!digestDue(d.f[i], d.runId, &off, &len)) continue;
    r.any = true;
    r.fileIndex = i;
    r.fileId = d.f[i].fileId;
    r.mode = cu::kModeDigest;
    r.offset = off;
    r.maxBytes = len;
    r.flags = flushed ? cu::kRqStoredBefore : 0;
    return r;
  }
  // Q8: storedBefore rides on the next READ of the same file, so the last
  // window of a file (all of a small one) would never be claimed and the
  // other master would fetch it again. A file held to its end with more than
  // the device last said was delivered gets one READ at that end: the device
  // answers one empty at-end frame (or the first byte of new growth) and
  // raises `delivered` (tests review 2026-11-19). The caller records the
  // claim in lastSeenDelivered when it sends it.
  if (flushed) {
    for (uint8_t i = 0; i < d.count; ++i) {
      const FileState& f = d.f[i];
      if (f.state == FState::Gone || (f.flags & kFfEvicted) || f.pos == 0) continue;
      if (f.pos < effectiveEnd(f) || f.pos <= f.lastSeenDelivered) continue;
      r.any = true;
      r.fileIndex = i;
      r.fileId = f.fileId;
      r.mode = cu::kModeData;
      r.claimOnly = true;
      r.offset = f.pos;
      r.maxBytes = 1;
      r.flags = cu::kRqStoredBefore;
      return r;
    }
  }
  return r;
}

// ---------------------------------------------------------------------------
// 6. One READ window
// ---------------------------------------------------------------------------

struct AsmFrame {
  uint32_t offset;
  uint16_t plainLen;
  uint8_t  dataLen, flags;
  uint8_t  data[kFrameDataMax];
};

struct WindowAssembler {
  enum class Took : uint8_t { Took, Duplicate, Closed, Ignored };
  uint32_t reqId, fileId, startOffset, expected, end;
  uint32_t startedMs, lastMs, timeoutMs;
  uint8_t  frames, lastFlags;
  bool     sealed, active, done, gapSeen, mismatch, atEnd, growing;
  AsmFrame f[kWindowFrames];

  void begin(uint32_t req, uint32_t file, uint32_t offset, bool sealOnSend, uint32_t nowMs, bool routed) {
    reqId = req; fileId = file; startOffset = offset; expected = offset; end = 0;
    startedMs = nowMs; lastMs = nowMs; timeoutMs = routed ? kTimeoutRoutedMs : kTimeoutDirectMs;
    frames = 0; lastFlags = 0;
    sealed = sealOnSend; active = true; done = false; gapSeen = false; mismatch = false; atEnd = false; growing = false;
  }
  void clear() { active = false; done = false; frames = 0; }
  uint32_t plainBytes() const { return expected - startOffset; }

  // In order only: offset == expected. A duplicate is ignored; a frame past
  // the expected offset closes the window with the prefix kept. A sealed
  // frame's plaintext length is its data less 28; a frame whose seal flag is
  // not the file's, or whose data does not fit, closes the window as well.
  Took onFrame(const cu::ReadRespHeader& h, const uint8_t* data, size_t n, uint32_t nowMs) {
    if (!active || done || h.reqId != reqId || h.fileId != fileId) return Took::Ignored;
    if (h.offset < expected) return Took::Duplicate;
    if (h.offset > expected) { gapSeen = true; done = true; return Took::Closed; }
    const bool frameSealed = (h.flags & cu::kRfSealedOnSend) != 0;
    uint16_t plain = 0;
    if (n > kFrameDataMax) { mismatch = true; done = true; return Took::Closed; }
    if (n > 0) {
      if (frameSealed != sealed) { mismatch = true; done = true; return Took::Closed; }
      if (frameSealed) {
        if (n <= cu::kSealOverhead) { mismatch = true; done = true; return Took::Closed; }
        plain = cu::sealedPlainLen(n);
      } else {
        plain = uint16_t(n);
      }
    }
    if (frames >= kWindowFrames) { done = true; return Took::Closed; }
    AsmFrame& a = f[frames++];
    a.offset = h.offset;
    a.plainLen = plain;
    a.dataLen = uint8_t(n);
    a.flags = h.flags;
    if (n) memcpy(a.data, data, n);
    expected += plain;
    end = h.end;
    lastFlags = h.flags;
    lastMs = nowMs;
    if (h.flags & cu::kRfAtEnd) atEnd = true;
    growing = (h.flags & cu::kRfGrowing) != 0;
    if ((h.flags & cu::kRfLast) || frames >= kWindowFrames || (plain == 0 && n == 0)) done = true;
    return Took::Took;
  }
  bool timedOut(uint32_t nowMs) const {
    return active && !done && uint32_t(nowMs - lastMs) >= timeoutMs;
  }
};

// ---------------------------------------------------------------------------
// 7. Status answers
// ---------------------------------------------------------------------------

enum class Act : uint8_t { Continue, Relist, Backoff, DeviceRefused, DeviceUnsupported };
struct StatusAction { Act act; uint32_t backoffMs; };

// What a READ status does (FILES_IMPL_SPEC 8.2). `respOffset`: the answer's
// offset (17 trimmed carries the device's new start there).
inline StatusAction onReadStatus(DeviceFiles& d, FileState& f, uint8_t mode, uint8_t status, uint32_t respOffset,
                                 uint16_t retryAfterS) {
  if (mode == cu::kModeDigest) {
    switch (status) {
      case cu::kStOk: return { Act::Continue, 0 };
      case cu::kStBusy:
      case cu::kStUnsupported:
      case cu::kStTooLarge:
      case cu::kStRefused:
        // Not the device's fault: the digest is skipped (busy: once per file
        // per session on the device; a later session may try again).
        if (status == cu::kStBusy) f.digestTriedRun = d.runId;
        else f.digest = DigestState::Skipped;
        return { Act::Continue, 0 };
      default: break;
    }
  }
  switch (status) {
    case cu::kStOk:
      return { Act::Continue, 0 };
    case cu::kStGone:
      // Tentative, like a file missing from a listing: the device also
      // answers gone for a READ under an old ledgerId or for a file it could
      // not find in a cut walk, and may list the same id again. Only a
      // complete listing without it makes the bytes lost_on_device
      // (planFromList); a listing with it revives the file from `pos`
      // (robustness review 2026-11-19).
      if (f.pos < f.lastSeenEnd) addGap(f, f.pos, f.lastSeenEnd, kGapNoLongerOffered);
      f.state = FState::Gone;
      f.flags |= kFfGoneByRead;
      return { Act::Continue, 0 };
    case cu::kStTrimmed:
      if (respOffset > f.pos) { addGap(f, f.pos, respOffset, kGapTrimmed); f.pos = respOffset; }
      if (respOffset > f.deviceStart) f.deviceStart = respOffset;
      settleState(f);
      return { Act::Continue, 0 };
    case cu::kStPastEnd:
      return { Act::Relist, kPastEndRelistMs };
    case cu::kStBusy:
      return { Act::Backoff, (retryAfterS ? uint32_t(retryAfterS) : 1u) * 1000u };
    case cu::kStRefused:
      // The listing was allowed in this run, so a refused READ is about this
      // file (an unsealed health file is never served): it leaves the offer,
      // tentatively, as a file missing from a full listing does.
      if (f.pos < f.lastSeenEnd) addGap(f, f.pos, f.lastSeenEnd, kGapNoLongerOffered);
      f.state = FState::Gone;
      return { Act::Continue, 0 };
    case cu::kStUnsupported:
      return { Act::DeviceUnsupported, 0 };
    default:   // 7 not_ready, 8 io_error, 6 bad_request, anything later
      return { Act::Backoff, kNotReadyBackoffMs };
  }
}

// A LIST status (the page header's): busy, refused, unsupported, not ready.
inline StatusAction onListStatus(uint8_t status, uint16_t retryAfterS) {
  switch (status) {
    case cu::kStOk:          return { Act::Continue, 0 };
    case cu::kStBusy:        return { Act::Backoff, (retryAfterS ? uint32_t(retryAfterS) : 1u) * 1000u };
    case cu::kStRefused:     return { Act::DeviceRefused, 0 };
    case cu::kStUnsupported: return { Act::DeviceUnsupported, 0 };
    default:                 return { Act::Backoff, kNotReadyBackoffMs };   // 7, 16 no_link, others
  }
}

// ---------------------------------------------------------------------------
// 8. Progress
// ---------------------------------------------------------------------------

struct Progress {
  uint16_t files, filesLeft;
  uint32_t bytesLeft, held, elsewhere;
  uint32_t gaps[kGapWhyCount];
  uint8_t  pct;
};
inline Progress progressOf(const DeviceFiles& d) {
  Progress p = {};
  uint64_t left = 0, held = 0, ew = 0, gaps[kGapWhyCount] = {};
  for (uint8_t i = 0; i < d.count; ++i) {
    const FileState& f = d.f[i];
    ++p.files;
    const uint32_t l = bytesLeft(f);
    if (l) ++p.filesLeft;
    left += l;
    held += heldBytes(f);
    ew += f.elsewhereBytes;
    for (uint8_t g = 0; g < kGapWhyCount; ++g) gaps[g] += f.gapBytes[g];
  }
  auto clip = [](uint64_t v) { return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : uint32_t(v); };
  p.bytesLeft = clip(left);
  p.held = clip(held);
  p.elsewhere = clip(ew);
  for (uint8_t g = 0; g < kGapWhyCount; ++g) p.gaps[g] = clip(gaps[g]);
  p.pct = (held + left) ? uint8_t((held * 100) / (held + left)) : 100;
  if (left && p.pct == 100) p.pct = 99;
  return p;
}
// EWMA of bytes per second (alpha 1/4), from one window of `bytes` over `ms`.
inline uint32_t ewmaBps(uint32_t prev, uint32_t bytes, uint32_t ms) {
  if (ms == 0) ms = 1;
  const uint64_t sample = uint64_t(bytes) * 1000u / ms;
  const uint32_t s = sample > 0xFFFFFFFFu ? 0xFFFFFFFFu : uint32_t(sample);
  if (prev == 0) return s;
  return uint32_t((uint64_t(prev) * 3u + s) / 4u);
}
inline uint32_t etaSeconds(uint32_t bytesLeft, uint32_t bps) {
  if (!bytesLeft) return 0;
  if (!bps) return 0xFFFFFFFFu;   // unknown
  return uint32_t((uint64_t(bytesLeft) + bps - 1) / bps);
}

// ---------------------------------------------------------------------------
// 9. Gates and the scheduler
// ---------------------------------------------------------------------------

// Why the collector does not pull at all (FILES_IMPL_SPEC 8.1), first match.
enum class Gate : uint8_t { Open = 0, NotBuilt, Off, Role, NoCard, NoClock };
inline Gate gate(bool built, bool enabled, uint8_t meshRole, bool sdWritable, bool clockSet) {
  if (!built) return Gate::NotBuilt;
  if (!enabled) return Gate::Off;
  if (meshRole != 1 && meshRole != 2) return Gate::Role;   // master or backup master (Q9: the same)
  if (!sdWritable) return Gate::NoCard;
  if (!clockSet) return Gate::NoClock;
  return Gate::Open;
}
inline const char* gateKey(Gate g) {
  switch (g) {
    case Gate::Open:     return nullptr;
    case Gate::NotBuilt: return "not_built";
    case Gate::Off:      return "off";
    case Gate::Role:     return "not_master";
    case Gate::NoCard:   return "no_card";
    case Gate::NoClock:  return "clock_not_set";
  }
  return "?";
}

enum class DevState : uint8_t { Idle = 0, Listing, Fetching, Verifying, Current, Suspended, Waiting, Refused, Unsupported };
inline const char* devStateKey(DevState s) {
  switch (s) {
    case DevState::Idle:        return "idle";
    case DevState::Listing:     return "listing";
    case DevState::Fetching:    return "fetching";
    case DevState::Verifying:   return "verifying";
    case DevState::Current:     return "current";
    case DevState::Suspended:   return "suspended";
    case DevState::Waiting:     return "waiting";
    case DevState::Refused:     return "refused";
    case DevState::Unsupported: return "unsupported";
  }
  return "idle";
}
enum class Why : uint8_t { None = 0, NoSession, Settling, NoFileServe, NoAnswer, Busy, NotReady, Refused, Unsupported,
                           CardError, Yielded, LedgerReset };
inline const char* whyKey(Why w) {
  switch (w) {
    case Why::None:        return nullptr;
    case Why::NoSession:   return "no_session";
    case Why::Settling:    return "session_settling";
    case Why::NoFileServe: return "no_file_catchup";
    case Why::NoAnswer:    return "no_answer";
    case Why::Busy:        return "busy";
    case Why::NotReady:    return "not_ready";
    case Why::Refused:     return "refused";
    case Why::Unsupported: return "unsupported";
    case Why::CardError:   return "card_error";
    case Why::Yielded:     return "yielded";
    case Why::LedgerReset: return "ledger_reset";
  }
  return "?";
}
inline const char* whyText(Why w) {
  switch (w) {
    case Why::None:        return "";
    case Why::NoSession:   return "no session";
    case Why::Settling:    return "the session is settling";
    case Why::NoFileServe: return "no file catch-up in its firmware";
    case Why::NoAnswer:    return "no answer";
    case Why::Busy:        return "busy";
    case Why::NotReady:    return "the device is not ready";
    case Why::Refused:     return "refused (it does not name this hub, or serving is off)";
    case Why::Unsupported: return "its firmware has no file catch-up";
    case Why::CardError:   return "the card write failed";
    case Why::Yielded:     return "yielded to another device";
    case Why::LedgerReset: return "ledger reset on the device";
  }
  return "";
}

struct Device {
  uint8_t  mac[6];
  uint8_t  used;
  DevState state;
  Why      why;
  uint8_t  batteryPct;          // kBatteryUnknown: no reading
  uint8_t  due, forced, forceFull, filesServe, capsKnown, settled, everRun, listFlags;
  uint16_t sessionId;           // the session seen last (0: none now)
  uint16_t blockedSession;      // refused/unsupported in this session: not asked again in it
  uint32_t notBeforeMs;         // backoff and the boot stagger
  uint32_t lastRunEndMs, lastHeardMs, yieldedMs;
  uint32_t lastCaughtUpEpoch;
  uint32_t ledgerId;
  // The summary of what the card holds for it (refreshed after each job).
  uint16_t files, filesLeft;
  uint32_t bytesLeft, held, elsewhere;
  uint32_t gaps[kGapWhyCount];
  uint8_t  pct;
  uint32_t bps;                 // EWMA bytes per second of its last run
  uint64_t bytesOnCard;
  uint32_t fetchingFileId;
  char     fetchingName[33];
};

struct TokenBucket {
  uint32_t lastMs;
  uint32_t milli;               // tokens x 1000
  uint8_t  primed;
  // A window costs `n` frames; `fps` frames refill per second, up to
  // max(fps, n) in store. False: wait.
  bool take(uint32_t nowMs, uint16_t fps, uint8_t n) {
    if (fps == 0) fps = 1;
    const uint64_t capMilli = uint64_t(fps > n ? fps : n) * 1000u;
    if (!primed) { primed = 1; milli = uint32_t(capMilli); lastMs = nowMs; }
    uint64_t m = milli + uint64_t(uint32_t(nowMs - lastMs)) * fps;
    if (m > capMilli) m = capMilli;
    milli = uint32_t(m);
    lastMs = nowMs;
    if (milli < uint32_t(n) * 1000u) return false;
    milli -= uint32_t(n) * 1000u;
    return true;
  }
};

struct Scheduler {
  Device   dev[kMaxDevices];
  uint8_t  activePlus1;         // the device in the file stage + 1; 0 none (valid zero-filled)
  uint8_t  sweepPending;
  uint32_t sweepAtMs;
  TokenBucket rate;

  int activeIndex() const { return int(activePlus1) - 1; }
  Device* find(const uint8_t mac[6]) {
    for (uint8_t i = 0; i < kMaxDevices; ++i)
      if (dev[i].used && memcmp(dev[i].mac, mac, 6) == 0) return &dev[i];
    return nullptr;
  }
  const Device* find(const uint8_t mac[6]) const {
    for (uint8_t i = 0; i < kMaxDevices; ++i)
      if (dev[i].used && memcmp(dev[i].mac, mac, 6) == 0) return &dev[i];
    return nullptr;
  }
  // Finds or adds; a full table drops the device heard least recently that is
  // not the active one. nullptr only when every slot is the active device.
  Device* touch(const uint8_t mac[6], uint32_t nowMs) {
    Device* d = find(mac);
    if (d) return d;
    int victim = -1;
    for (uint8_t i = 0; i < kMaxDevices; ++i) {
      if (!dev[i].used) { victim = i; break; }
      if (int(i) == activeIndex()) continue;
      if (victim < 0 || uint32_t(nowMs - dev[i].lastHeardMs) > uint32_t(nowMs - dev[victim].lastHeardMs)) victim = i;
    }
    if (victim < 0) return nullptr;
    d = &dev[victim];
    memset(d, 0, sizeof(*d));
    memcpy(d->mac, mac, 6);
    d->used = 1;
    d->batteryPct = kBatteryUnknown;
    d->lastHeardMs = nowMs;
    d->state = DevState::Waiting;
    d->why = Why::NoSession;
    if (sweepPending) { d->due = 1; d->notBeforeMs = nowMs; }
    return d;
  }

  // A peer seen with an ACTIVE session (from the session table, every second).
  // A new sessionId (a reconnect) makes it due once settled, whatever backoff
  // it had; a refusal or "unsupported" holds only for the session it came in.
  void noteSession(const uint8_t mac[6], uint16_t sessionId, bool settled, bool capsKnown, bool filesServe,
                   uint8_t batteryPct, uint32_t nowMs) {
    Device* d = touch(mac, nowMs);
    if (!d) return;
    d->lastHeardMs = nowMs;
    d->batteryPct = batteryPct;
    d->capsKnown = capsKnown;
    d->filesServe = filesServe;
    d->settled = settled;
    if (sessionId && sessionId != d->sessionId) {
      // A reconnect runs at once: a backoff belonged to the old session.
      d->sessionId = sessionId;
      d->due = 1;
      d->notBeforeMs = nowMs;
      if (d->blockedSession != sessionId) d->blockedSession = 0;
      if (d->state == DevState::Refused || d->state == DevState::Unsupported || d->state == DevState::Suspended) {
        d->state = DevState::Idle;
        d->why = Why::None;
      }
    }
    if (d->state == DevState::Waiting) { d->state = DevState::Idle; d->why = Why::None; }
    if (capsKnown && !filesServe && d->state != DevState::Unsupported) { d->why = Why::NoFileServe; }
  }
  void noteNoSession(Device& d) {
    if (d.sessionId == 0) return;
    d.sessionId = 0;
    d.settled = 0;
    if (d.state == DevState::Refused || d.state == DevState::Unsupported) return;
    d.state = d.everRun ? DevState::Suspended : DevState::Waiting;
    d.why = Why::NoSession;
  }

  // After the collector boots or a card is mounted: every peer once, 1 s apart.
  void bootSweep(uint32_t nowMs) {
    sweepPending = 1;
    uint32_t at = nowMs;
    for (uint8_t i = 0; i < kMaxDevices; ++i) {
      if (!dev[i].used) continue;
      dev[i].due = 1;
      dev[i].notBeforeMs = at;
      at += kBootStaggerMs;
    }
  }
  // Every 15 min per peer.
  void periodic(uint32_t nowMs) {
    for (uint8_t i = 0; i < kMaxDevices; ++i) {
      Device& d = dev[i];
      if (!d.used || d.due || !d.everRun) continue;
      if (uint32_t(nowMs - d.lastRunEndMs) >= kIntervalMs) d.due = 1;
    }
  }
  bool eligible(const Device& d, uint32_t nowMs) const {
    if (!d.used || !d.due || !d.sessionId || !d.settled) return false;
    if (d.capsKnown && !d.filesServe) return false;
    if (d.blockedSession && d.blockedSession == d.sessionId) return false;
    if (int32_t(nowMs - d.notBeforeMs) < 0) return false;
    return true;
  }
  // The next device to run, -1 none. Only one device is in the file stage.
  // collectnow first, then lowest battery (unknown last), then the longest
  // since it was last caught up; a device that yielded at its run ceiling
  // goes behind the others.
  int pick(uint32_t nowMs) const {
    if (activeIndex() >= 0) return -1;
    int best = -1;
    for (uint8_t i = 0; i < kMaxDevices; ++i) {
      const Device& d = dev[i];
      if (!eligible(d, nowMs)) continue;
      if (best < 0) { best = i; continue; }
      const Device& b = dev[best];
      if (d.forced != b.forced) { if (d.forced) best = i; continue; }
      const bool dy = d.yieldedMs != 0, by = b.yieldedMs != 0;
      if (dy != by) { if (!dy) best = i; continue; }
      if (d.batteryPct != b.batteryPct) { if (d.batteryPct < b.batteryPct) best = i; continue; }
      if (d.lastCaughtUpEpoch != b.lastCaughtUpEpoch) { if (d.lastCaughtUpEpoch < b.lastCaughtUpEpoch) best = i; continue; }
      if (uint32_t(nowMs - d.lastRunEndMs) > uint32_t(nowMs - b.lastRunEndMs)) best = i;
    }
    return best;
  }
  void start(int i, uint32_t) {
    activePlus1 = uint8_t(i + 1);
    Device& d = dev[i];
    d.due = 0;
    d.everRun = 1;
    d.state = DevState::Listing;
    d.why = Why::None;
    d.yieldedMs = 0;
  }

  enum class Outcome : uint8_t { Current, Yield, Backoff, Suspend, Refused, Unsupported, Relist, CardError };
  // The end of a run. Current: caught up. Yield: the run ceiling; due again,
  // behind the others. Backoff: due again after `backoffMs`. Suspend: the
  // session went or no answer; due again on the next trigger. Refused /
  // Unsupported: not asked again in this session. Relist: due again after
  // `backoffMs` (past_end: the device's ledger moved).
  void end(Outcome o, uint32_t nowMs, uint32_t backoffMs, uint32_t nowEpoch, Why why) {
    const int i = activeIndex();
    activePlus1 = 0;
    if (i < 0) return;
    Device& d = dev[i];
    d.lastRunEndMs = nowMs;
    d.forced = 0;
    d.forceFull = 0;
    d.why = why;
    switch (o) {
      case Outcome::Current:
        d.state = DevState::Current; d.lastCaughtUpEpoch = nowEpoch; d.why = Why::None; break;
      case Outcome::Yield:
        d.state = DevState::Idle; d.due = 1; d.yieldedMs = nowMs; d.notBeforeMs = nowMs; d.why = Why::Yielded; break;
      case Outcome::Backoff:
      case Outcome::Relist:
        d.state = DevState::Idle; d.due = 1; d.notBeforeMs = nowMs + backoffMs; break;
      case Outcome::Suspend:
        d.state = DevState::Suspended; d.due = 1; d.notBeforeMs = nowMs + backoffMs; break;
      case Outcome::Refused:
        d.state = DevState::Refused; d.blockedSession = d.sessionId ? d.sessionId : 0xFFFF; d.why = Why::Refused; break;
      case Outcome::Unsupported:
        d.state = DevState::Unsupported; d.blockedSession = d.sessionId ? d.sessionId : 0xFFFF; d.why = Why::Unsupported; break;
      case Outcome::CardError:
        d.state = DevState::Idle; d.due = 1; d.notBeforeMs = nowMs + kNotReadyBackoffMs; d.why = Why::CardError; break;
    }
  }
  // collectnow: due at once, first in line; `full` fetches what another
  // master already has.
  void force(Device& d, bool full, uint32_t nowMs) {
    d.due = 1;
    d.forced = 1;
    d.forceFull = full ? 1 : 0;
    d.notBeforeMs = nowMs;
    d.blockedSession = 0;
    if (d.state == DevState::Refused || d.state == DevState::Unsupported) d.state = DevState::Idle;
  }
};

// The run ceiling: 64 MB or 30 min, then the next device.
inline bool runCeiling(uint32_t bytesThisRun, uint32_t runStartMs, uint32_t nowMs) {
  return bytesThisRun >= kRunMaxBytes || uint32_t(nowMs - runStartMs) >= kRunMaxMs;
}

}  // namespace collectcore
