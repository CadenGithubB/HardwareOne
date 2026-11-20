// Host tests of System_CollectCore.h, the collector's pure rules for file
// catch-up (FILES_IMPL_SPEC.md section 8):
//   * planFromList for every gap reason (before_first_fetch, trimmed,
//     no_longer_offered; lost_on_device through onReadStatus), the Q8
//     `elsewhere` skip and the `full` override with hole filling, a ledger
//     change, a file that comes back into the offer, a full file list;
//   * the fetch order (most at risk first), nextRead (plaintext per frame
//     182/126/154/98, storedBefore only when flushed), the digest of a
//     closed file;
//   * WindowAssembler: in order, duplicates, a frame past the expected
//     offset, sealed lengths, a seal mismatch, the empty frame at the end, a
//     timeout keeping the prefix;
//   * onReadStatus and onListStatus for every status;
//   * the scheduler: one device in the file stage, collectnow first, lowest
//     battery first, the 15-minute interval, the boot sweep 1 s apart, a new
//     sessionId as a trigger, refusals held per session, backoffs, the run
//     ceiling and yield, the frame rate cap;
//   * progress and ETA; range merges at capacity with exact totals; segment
//     roll-over; recovery; the 256 MB cap; safe names; the gates.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "System_CollectCore.h"

using namespace collectcore;
namespace cu = espnowcatchup;

static unsigned gChecks = 0;
#define CHECK(c)                                                                 \
  do {                                                                           \
    ++gChecks;                                                                   \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

static DeviceFiles gD;   // ~19 KB: static, as in PSRAM on the device
static Listing gL;

static cu::Entry entry(uint32_t id, uint8_t kind, const char* name, uint32_t start, uint32_t end,
                       uint32_t delivered = 0, uint8_t flags = 0, uint32_t boot = 1) {
  cu::Entry e = {};
  e.kind = kind; e.flags = flags; e.fileId = id; e.start = start; e.end = end; e.createdBoot = boot;
  e.delivered = delivered; e.name = name; e.nameLen = uint8_t(strlen(name));
  return e;
}
static void list(std::initializer_list<cu::Entry> es, uint32_t ledgerId = 0x1a2b3c4d, bool complete = true) {
  listingReset(gL);
  cu::ListRespHeader h = {};
  h.ledgerId = ledgerId; h.ledgerGen = 5; h.headerLen = 24; h.entryFixed = 24;
  CHECK(listingPage(gL, h));
  for (const cu::Entry& e : es) CHECK(listingAdd(gL, e));
  gL.complete = complete;
}
static void reset() { memset(&gD, 0, sizeof(gD)); }

// Writes [offset, offset+len) as the store would: segmentFor, then commitWrite.
static bool write(FileState& f, uint32_t offset, uint32_t len, bool* rolled = nullptr, Segment* out = nullptr) {
  const SegChoice c = segmentFor(f, offset);
  return commitWrite(f, c, offset, len, out, rolled);
}

static void testSafeNameAndGates() {
  char out[cu::kNameMax + 1];
  CHECK(safeName("system.log.1", 12, out) == 12 && strcmp(out, "system.log.1") == 0);
  const char bad[] = { 'a', 0, 'b', '\n', char(0x7F), char(0xC3), char(0xA9), char(0xFF), '"', '\\' };
  CHECK(safeName(bad, sizeof(bad), out) == 10);
  CHECK(memcmp(out, "a?b??\xC3\xA9?\"\\", 10) == 0);
  std::string longName(120, 'x');
  CHECK(safeName(longName.c_str(), longName.size(), out) == 96);
  CHECK(gate(false, true, 1, true, true) == Gate::NotBuilt);
  CHECK(gate(true, false, 1, true, true) == Gate::Off);
  CHECK(gate(true, true, 0, true, true) == Gate::Role);
  CHECK(gate(true, true, 2, true, true) == Gate::Open);   // a backup master is the same (Q9)
  CHECK(gate(true, true, 1, false, true) == Gate::NoCard);
  CHECK(gate(true, true, 1, true, false) == Gate::NoClock);
  CHECK(gateKey(Gate::Open) == nullptr && strcmp(gateKey(Gate::NoClock), "clock_not_set") == 0);
  for (uint8_t g = 0; g < kGapWhyCount; ++g) {
    uint8_t back = 9;
    CHECK(gapWhyFromKey(gapWhyKey(g), &back) && back == g);
  }
  FState s;
  CHECK(fstateFromKey("closed", &s) && s == FState::Closed && !fstateFromKey("x", &s));
}

static void testPlanGaps() {
  reset();
  // A new file whose head the device already trimmed: before_first_fetch.
  list({ entry(12, 6, "system.log.1", 0, 1000, 0, cu::kEfSealedOnSend | cu::kEfOnSd),
         entry(19, 2, "events.log", 300, 900) });
  PlanOut o = planFromList(gD, gL, false, 1800000000u);
  CHECK(o.added == 2 && o.ledgerSet && gD.ledgerId == 0x1a2b3c4d && gD.count == 2 && gD.runId == 1);
  FileState* sys = findFile(gD, 12);
  FileState* ev = findFile(gD, 19);
  CHECK(sys && ev && sys->state == FState::Fetching && (sys->flags & kFfSealOnSend) && (sys->flags & kFfOnSd));
  CHECK(sys->firstSeenEpoch == 1800000000u && strcmp(sys->name, "system.log.1") == 0);
  CHECK(ev->pos == 300 && ev->nGap == 1 && ev->gap[0].from == 0 && ev->gap[0].to == 300 &&
        ev->gap[0].why == kGapBeforeFirstFetch && ev->gapBytes[kGapBeforeFirstFetch] == 300);
  CHECK(write(*ev, 300, 200));   // held up to 500
  // The device trimmed again past what is held: a trimmed gap, the next
  // segment starts at the new start.
  list({ entry(12, 6, "system.log.1", 0, 1000, 0, cu::kEfSealedOnSend), entry(19, 2, "events.log", 650, 1400) });
  o = planFromList(gD, gL, false, 1800000100u);
  CHECK(o.updated == 2 && o.added == 0);
  CHECK(ev->pos == 650 && ev->gap[1].from == 500 && ev->gap[1].to == 650 && ev->gap[1].why == kGapTrimmed);
  CHECK(ev->gapBytes[kGapTrimmed] == 150 && ev->deviceStart == 650 && ev->lastSeenEnd == 1400);
  SegChoice c = segmentFor(*ev, 650);
  CHECK(c.fresh && c.seg == 1);
  // A file missing from a complete listing: no_longer_offered, Gone.
  list({ entry(19, 2, "events.log", 650, 1400) });
  o = planFromList(gD, gL, false, 1800000200u);
  CHECK(o.gone == 1 && sys->state == FState::Gone && sys->nGap == 1 && sys->gap[0].why == kGapNoLongerOffered &&
        sys->gap[0].from == 0 && sys->gap[0].to == 1000);
  // An incomplete listing proves nothing: nothing goes.
  list({}, 0x1a2b3c4d, false);
  o = planFromList(gD, gL, false, 1800000300u);
  CHECK(o.gone == 0 && ev->state == FState::Fetching);
  // It comes back (the card was put back): the tentative gap goes, it resumes.
  list({ entry(12, 6, "system.log.1", 0, 1000, 0, cu::kEfSealedOnSend), entry(19, 2, "events.log", 650, 1400) });
  o = planFromList(gD, gL, false, 1800000400u);
  CHECK(o.revived == 1 && sys->state == FState::Fetching && sys->nGap == 0 &&
        sys->gapBytes[kGapNoLongerOffered] == 0 && sys->pos == 0);
  // A READ answered gone: tentative (no_longer_offered, the position stays),
  // because the device also answers gone in passing (an old ledgerId, a cut
  // walk). Listed again: revived from the position, nothing lost.
  StatusAction a = onReadStatus(gD, *sys, cu::kModeData, cu::kStGone, 0, 0);
  CHECK(a.act == Act::Continue && sys->state == FState::Gone && sys->pos == 0 &&
        sys->gap[0].why == kGapNoLongerOffered && sys->gapBytes[kGapNoLongerOffered] == 1000 &&
        sys->gapBytes[kGapLostOnDevice] == 0 && (sys->flags & kFfGoneByRead));
  list({ entry(12, 6, "system.log.1", 0, 1000, 0, cu::kEfSealedOnSend), entry(19, 2, "events.log", 650, 1400) });
  o = planFromList(gD, gL, false, 1800000450u);
  CHECK(o.revived == 1 && sys->state == FState::Fetching && sys->nGap == 0 && sys->pos == 0 &&
        sys->gapBytes[kGapNoLongerOffered] == 0 && !(sys->flags & kFfGoneByRead));
  // Gone again, and a complete listing agrees: lost_on_device up to the last seen end.
  onReadStatus(gD, *sys, cu::kModeData, cu::kStGone, 0, 0);
  list({ entry(19, 2, "events.log", 650, 1400) });
  o = planFromList(gD, gL, false, 1800000460u);
  CHECK(sys->state == FState::Gone && sys->pos == 1000 && sys->gap[0].why == kGapLostOnDevice &&
        sys->gapBytes[kGapLostOnDevice] == 1000 && sys->gapBytes[kGapNoLongerOffered] == 0 &&
        !(sys->flags & kFfGoneByRead));
  // A rename by rotation keeps the id and takes the new name.
  list({ entry(19, 2, "events.log.1", 650, 1400) });
  planFromList(gD, gL, false, 1800000500u);
  CHECK(strcmp(ev->name, "events.log.1") == 0);
}

static void testElsewhereAndFull() {
  reset();
  // Q8: the other master stored up to 600; this one records it and starts there.
  list({ entry(7, 5, "errors.log", 0, 1000, 600) });
  planFromList(gD, gL, false, 1u);
  FileState* f = findFile(gD, 7);
  CHECK(f->pos == 600 && f->nElse == 1 && f->elsewhere[0].from == 0 && f->elsewhere[0].to == 600 &&
        f->elsewhereBytes == 600 && f->nGap == 0);
  ReadPlan r = nextRead(gD, cu::kDirectPayload, true, false);
  CHECK(r.any && r.offset == 600 && r.maxBytes == 400 && r.mode == cu::kModeData && (r.flags & cu::kRqStoredBefore));
  CHECK(write(*f, 600, 400) && f->state == FState::Closed);
  // delivered beyond what is held after a later fetch: another elsewhere range.
  list({ entry(7, 5, "errors.log", 0, 2000, 1500) });
  planFromList(gD, gL, false, 2u);
  CHECK(f->pos == 1500 && f->nElse == 2 && f->elsewhere[1].from == 1000 && f->elsewhereBytes == 1100);
  // delivered is clamped to end.
  list({ entry(8, 3, "battery.csv", 0, 100, 5000) });
  planFromList(gD, gL, false, 3u);
  CHECK(findFile(gD, 8)->pos == 100 && findFile(gD, 8)->elsewhereBytes == 100);

  // `full` (collectnow ... full): delivered is not skipped, and holes the
  // device still holds are filled.
  reset();
  list({ entry(7, 5, "errors.log", 0, 1000, 600) });
  planFromList(gD, gL, false, 1u);
  f = findFile(gD, 7);
  CHECK(write(*f, 600, 400));
  list({ entry(7, 5, "errors.log", 200, 1300, 1300) });
  planFromList(gD, gL, true, 2u);
  CHECK(f->pos == 1000 && f->nElse == 2 && f->elsewhere[0].to == 200 && f->elsewhere[1].from == 200 &&
        f->elsewhere[1].to == 600);   // split at the device's start
  r = nextRead(gD, cu::kDirectPayload, true, true);
  CHECK(r.any && !r.holeFill && r.offset == 1000 && r.maxBytes == 300);   // forward first
  CHECK(write(*f, 1000, 300));
  r = nextRead(gD, cu::kDirectPayload, true, true);
  CHECK(r.any && r.holeFill && r.offset == 200 && r.maxBytes == 400);
  CHECK(write(*f, 200, 400));
  CHECK(f->nElse == 1 && f->elsewhere[0].from == 0 && f->elsewhere[0].to == 200 && f->elsewhereBytes == 200);
  // The filled hole is its own segment; it ends where the forward segment starts.
  CHECK(f->nSeg == 2 && f->seg[1].from == 200 && f->seg[1].to == 600 && f->seg[0].from == 600 && f->seg[0].to == 1300);
  r = nextRead(gD, cu::kDirectPayload, true, true);
  CHECK(!r.any || r.mode == cu::kModeDigest);   // the part below start stays elsewhere
  // Without `full`, holes are not filled.
  CHECK(!nextRead(gD, cu::kDirectPayload, true, false).any || nextRead(gD, cu::kDirectPayload, true, false).mode == cu::kModeDigest);
  // A write that is neither forward nor a hole start is refused.
  CHECK(!write(*f, 50, 10));
}

static void testLedgerChangeAndFullList() {
  reset();
  list({ entry(1, 2, "events.log", 0, 10) });
  planFromList(gD, gL, false, 1u);
  CHECK(!ledgerChanged(gD, 0x1a2b3c4d) && ledgerChanged(gD, 0x99));
  retireAll(gD, 0x99);
  CHECK(gD.count == 0 && gD.ledgerId == 0x99 && gD.haveLedger);
  // A full list: a new file takes the oldest Gone file's slot (reported for the archive).
  reset();
  for (uint32_t i = 1; i <= kMaxFiles; ++i) {
    FileState& f = gD.f[gD.count++];
    f.fileId = i; f.kind = 1; f.state = i == 5 || i == 3 ? FState::Gone : FState::Closed;
  }
  gD.haveLedger = 1; gD.ledgerId = 0x1a2b3c4d;
  list({ entry(100, 1, "new.csv", 0, 10) }, 0x1a2b3c4d, false);
  uint32_t evicted = 0;
  PlanOut o = planFromList(gD, gL, false, 1u, [&](const FileState& f) { evicted = f.fileId; });
  CHECK(o.added == 1 && o.evictedFromList == 1 && evicted == 3 && gD.count == kMaxFiles && findFile(gD, 100));
  // No Gone file left to push out after both are used: a new file is dropped.
  list({ entry(101, 1, "b.csv", 0, 10), entry(102, 1, "c.csv", 0, 10) }, 0x1a2b3c4d, false);
  o = planFromList(gD, gL, false, 1u, [&](const FileState&) {});
  CHECK(o.added == 1 && o.dropped == 1);
  // Kind 0, kind 8 and fileId 0 are not taken.
  reset();
  list({ entry(1, 0, "a", 0, 1), entry(2, 8, "b", 0, 1), entry(0, 1, "c", 0, 1) });
  o = planFromList(gD, gL, false, 1u);
  CHECK(o.dropped == 3 && gD.count == 0);
}

static void testListing() {
  listingReset(gL);
  cu::ListRespHeader h = {};
  h.ledgerId = 1; h.flags = cu::kLfWithheld;
  CHECK(listingPage(gL, h));
  CHECK(listingAdd(gL, entry(3, 1, "x", 0, 1)) && listingAdd(gL, entry(3, 1, "x", 0, 1)) && gL.count == 1);
  h.flags = cu::kLfBeyondHorizon;
  CHECK(listingPage(gL, h) && gL.respFlags == (cu::kLfWithheld | cu::kLfBeyondHorizon) && gL.pages == 2);
  h.ledgerId = 2;   // the ledger moved between pages: start over
  CHECK(!listingPage(gL, h) && gL.count == 0 && !gL.haveHeader);
  for (uint32_t i = 1; i <= kMaxListed; ++i) CHECK(listingAdd(gL, entry(i, 1, "x", 0, 1)));
  CHECK(!listingAdd(gL, entry(999, 1, "x", 0, 1)) && gL.overflow);
  // A name is copied with its length (no NUL in the frame).
  listingReset(gL);
  cu::Entry e = entry(4, 1, "abcdef", 0, 1);
  e.nameLen = 3;
  CHECK(listingAdd(gL, e) && strcmp(gL.e[0].name, "abc") == 0);
  // An overflowing listing never marks files gone.
  reset();
  list({ entry(1, 1, "a", 0, 10) });
  planFromList(gD, gL, false, 1u);
  list({}, 0x1a2b3c4d, true);
  gL.overflow = true;
  CHECK(planFromList(gD, gL, false, 1u).gone == 0);
}

static void testRiskOrderAndNextRead() {
  reset();
  list({ entry(1, 1, "2026-11-19/env.csv", 0, 50, 0, 0, 3),
         entry(2, 6, "system.log", 0, 50, 0, cu::kEfSealedOnSend, 9),
         entry(3, 6, "system.log.2", 0, 50, 0, cu::kEfSealedOnSend, 9),
         entry(4, 2, "events.log", 0, 50, 0, 0, 9),
         entry(5, 1, "2026-11-18/env.csv", 0, 50, 0, 0, 2),
         entry(6, 6, "system.log.1", 0, 50, 0, cu::kEfSealedOnSend, 9) });
  planFromList(gD, gL, false, 1u);
  const uint32_t order[] = { 3, 6, 4, 5, 1, 2 };
  for (uint32_t id : order) {
    ReadPlan r = nextRead(gD, cu::kDirectPayload, false, false);
    CHECK(r.any && r.fileId == id && r.flags == 0);
    FileState* f = findFile(gD, id);
    CHECK(write(*f, r.offset, r.maxBytes));
  }
  // Plaintext per frame: 182 / 126 direct / routed, 154 / 98 sealed.
  CHECK(plainPerFrame(cu::kDirectPayload, false) == 182 && plainPerFrame(cu::kRoutedPayload, false) == 126);
  CHECK(plainPerFrame(cu::kDirectPayload, true) == 154 && plainPerFrame(cu::kRoutedPayload, true) == 98);
  CHECK(plainPerFrame(40, true) == 0);
  reset();
  list({ entry(9, 6, "system.log", 0, 100000, 0, cu::kEfSealedOnSend) });
  planFromList(gD, gL, false, 1u);
  ReadPlan r = nextRead(gD, cu::kRoutedPayload, true, false);
  CHECK(r.maxBytes == 8u * 98u && r.flags == 0);   // pos 0: no claim yet
  CHECK(write(*findFile(gD, 9), 0, 784));
  r = nextRead(gD, cu::kRoutedPayload, true, false);
  CHECK(r.offset == 784 && (r.flags & cu::kRqStoredBefore));
  r = nextRead(gD, cu::kRoutedPayload, false, false);
  CHECK(!(r.flags & cu::kRqStoredBefore));   // not flushed: no claim

  // The 256 MB cap.
  reset();
  list({ entry(9, 1, "huge.csv", 0, kFileCapBytes + 10) });
  planFromList(gD, gL, false, 1u);
  FileState* f = findFile(gD, 9);
  CHECK((f->flags & kFfTooLarge) && effectiveEnd(*f) == kFileCapBytes && bytesLeft(*f) == kFileCapBytes);
  f->pos = kFileCapBytes - 5;
  r = nextRead(gD, cu::kDirectPayload, true, false);
  CHECK(r.any && r.maxBytes == 5);
  f->pos = kFileCapBytes;
  settleState(*f);
  // Too large: no digest; what is held is claimed once (Q8), then nothing.
  r = nextRead(gD, cu::kDirectPayload, true, false);
  CHECK(f->state == FState::Closed && r.any && r.claimOnly && r.offset == kFileCapBytes);
  f->lastSeenDelivered = r.offset;
  CHECK(!nextRead(gD, cu::kDirectPayload, true, false).any);

  // Q8: a file held to its end is claimed with one READ at the end (the last
  // window of a file, all of a small transcript), once, and only when flushed.
  reset();
  list({ entry(4, 7, "u3/20261119T120000Z-a.txt", 0, 35, 0, cu::kEfSealedOnSend) });
  planFromList(gD, gL, false, 1u);
  f = findFile(gD, 4);
  r = nextRead(gD, cu::kDirectPayload, true, false);
  CHECK(r.any && !r.claimOnly && r.offset == 0 && r.maxBytes == 35 && r.flags == 0);
  CHECK(write(*f, 0, 35) && f->state == FState::Closed);
  CHECK(!nextRead(gD, cu::kDirectPayload, false, false).any);   // not flushed: no claim
  r = nextRead(gD, cu::kDirectPayload, true, false);
  CHECK(r.any && r.claimOnly && r.fileId == 4 && r.mode == cu::kModeData && r.offset == 35 && r.maxBytes == 1 &&
        r.flags == cu::kRqStoredBefore);
  f->lastSeenDelivered = r.offset;   // the collector records it when it sends it
  CHECK(!nextRead(gD, cu::kDirectPayload, true, false).any);
  // The device's next listing says delivered 35: nothing more to claim.
  list({ entry(4, 7, "u3/20261119T120000Z-a.txt", 0, 35, 35, cu::kEfSealedOnSend) });
  planFromList(gD, gL, false, 2u);
  CHECK(f->pos == 35 && f->elsewhereBytes == 0 && !nextRead(gD, cu::kDirectPayload, true, false).any);
}

static void testDigest() {
  reset();
  list({ entry(1, 2, "events.log.1", 100, 400) });
  planFromList(gD, gL, false, 1u);
  FileState* f = findFile(gD, 1);
  CHECK(write(*f, 100, 300) && f->state == FState::Closed);
  uint32_t off = 0, len = 0;
  CHECK(digestDue(*f, gD.runId, &off, &len) && off == 100 && len == 300);
  ReadPlan r = nextRead(gD, cu::kDirectPayload, true, false);
  CHECK(r.any && r.mode == cu::kModeDigest && r.offset == 100 && r.maxBytes == 300);
  // Busy: not again in this run; next run it may.
  StatusAction a = onReadStatus(gD, *f, cu::kModeDigest, cu::kStBusy, 0, 3600);
  CHECK(a.act == Act::Continue && f->digest == DigestState::None && !digestDue(*f, gD.runId, nullptr, nullptr));
  list({ entry(1, 2, "events.log.1", 100, 400) });
  planFromList(gD, gL, false, 2u);
  CHECK(digestDue(*f, gD.runId, nullptr, nullptr));
  CHECK(onReadStatus(gD, *f, cu::kModeDigest, cu::kStTooLarge, 0, 0).act == Act::Continue &&
        f->digest == DigestState::Skipped && !digestDue(*f, gD.runId, nullptr, nullptr));
  f->digest = DigestState::None;
  CHECK(onReadStatus(gD, *f, cu::kModeDigest, cu::kStUnsupported, 0, 0).act == Act::Continue &&
        f->digest == DigestState::Skipped);
  // A growing file is Current, never digested; a rotated sibling is closed.
  reset();
  list({ entry(1, 2, "events.log", 0, 10, 0, cu::kEfGrowing), entry(2, 6, "system.log.1", 0, 10, 0, cu::kEfGrowing | cu::kEfSealedOnSend) });
  planFromList(gD, gL, false, 1u);
  CHECK(write(*findFile(gD, 1), 0, 10) && findFile(gD, 1)->state == FState::Current);
  CHECK(write(*findFile(gD, 2), 0, 10) && findFile(gD, 2)->state == FState::Closed);
  CHECK(!digestDue(*findFile(gD, 1), 1, nullptr, nullptr));
  CHECK(!digestDue(*findFile(gD, 2), 1, nullptr, nullptr));   // sealed on send: no digest (the device answers 3)
  // A file over 1 MiB: its last 1 MiB, when one segment holds it.
  reset();
  list({ entry(1, 1, "big.csv", 0, 3u << 20) });
  planFromList(gD, gL, false, 1u);
  f = findFile(gD, 1);
  CHECK(write(*f, 0, 3u << 20));
  CHECK(digestDue(*f, 1, &off, &len) && off == (2u << 20) && len == cu::kDigestMaxBytes);
  // A hole in the range: no digest.
  reset();
  list({ entry(1, 1, "a.csv", 0, 100, 40) });
  planFromList(gD, gL, false, 1u);
  f = findFile(gD, 1);
  CHECK(write(*f, 40, 60) && digestDue(*f, 1, &off, &len) == false);
}

static void testSegmentsAndRecovery() {
  reset();
  list({ entry(1, 1, "a.csv", 0, 100000) });
  planFromList(gD, gL, false, 1u);
  FileState* f = findFile(gD, 1);
  CHECK(write(*f, 0, 100) && f->nSeg == 1 && f->seg[0].seg == 0 && f->nextSeg == 1);
  CHECK(write(*f, 100, 100) && f->nSeg == 1 && f->seg[0].to == 200);   // extends
  closeSegment(*f, 0);
  CHECK(write(*f, 200, 50) && f->nSeg == 2 && f->seg[1].seg == 1 && f->seg[1].from == 200);
  // Eight segments, then the oldest rolls out (its bytes counted, reported).
  for (uint32_t k = 0; k < 6; ++k) {
    f->pos += 10;   // a gap between, so each write starts a segment
    CHECK(write(*f, f->pos, 10));
  }
  CHECK(f->nSeg == kSegMax);
  bool rolled = false;
  Segment out = {};
  f->pos += 10;
  CHECK(write(*f, f->pos, 10, &rolled, &out));
  CHECK(rolled && out.seg == 0 && out.from == 0 && out.to == 200 && f->rolledBytes == 200 && f->nSeg == kSegMax &&
        f->seg[kSegMax - 1].seg == 8 && heldBytes(*f) == 200 + 50 + 7 * 10);
  // Recovery: a shorter .raw pulls the position back when it was the end.
  reset();
  list({ entry(1, 1, "a.csv", 0, 1000) });
  planFromList(gD, gL, false, 1u);
  f = findFile(gD, 1);
  CHECK(write(*f, 0, 500));
  recoverSegment(*f, 0, 450);
  CHECK(f->seg[0].to == 450 && f->pos == 450 && f->seg[0].closed && f->state == FState::Fetching);
  // A longer one extends it when nothing came after.
  f->seg[0].closed = 0;
  recoverSegment(*f, 0, 600);
  CHECK(f->seg[0].to == 600 && f->pos == 600);
  // ... and is closed when something was recorded after it.
  addGap(*f, 600, 700, kGapTrimmed);
  f->pos = 700;
  recoverSegment(*f, 0, 650);
  CHECK(f->seg[0].to == 600 && f->seg[0].closed && f->pos == 700);
  // An empty segment is dropped.
  recoverSegment(*f, 0, 0);
  CHECK(f->nSeg == 0);
}

static void testMerges() {
  FileState f = {};
  for (uint32_t i = 0; i < kGapMax; ++i) addGap(f, i * 100, i * 100 + 10, uint8_t(i % 2 ? kGapTrimmed : kGapBeforeFirstFetch));
  CHECK(f.nGap == kGapMax);
  addGap(f, 5000, 5100, kGapLostOnDevice);
  CHECK(f.nGap == kGapMax && f.gap[kGapMax - 1].from == 5000);
  CHECK(f.gapBytes[kGapBeforeFirstFetch] == 40 && f.gapBytes[kGapTrimmed] == 40 && f.gapBytes[kGapLostOnDevice] == 100);
  CHECK(f.gap[0].from == 0 && f.gap[0].to == 110);   // nearest pair merged
  // A continuing gap of the same reason joins.
  addGap(f, 5100, 5200, kGapLostOnDevice);
  CHECK(f.gap[kGapMax - 1].to == 5200 && f.gapBytes[kGapLostOnDevice] == 200);
  addGap(f, 7, 7, kGapTrimmed);   // empty: nothing
  CHECK(f.gapBytes[kGapTrimmed] == 40);
  FileState g = {};
  for (uint32_t i = 0; i < kElseMax; ++i) addElsewhere(g, i * 100, i * 100 + 50);
  addElsewhere(g, 1000, 1010);
  CHECK(g.nElse == kElseMax && g.elsewhere[0].from == 0 && g.elsewhere[0].to == 150 && g.elsewhereBytes == 260);
}

static void testAssembler() {
  static WindowAssembler w;
  cu::ReadRespHeader h = {};
  uint8_t data[200];
  memset(data, 0xAB, sizeof(data));
  w.begin(77, 12, 1000, false, 0, false);
  h.reqId = 77; h.fileId = 12; h.offset = 1000; h.end = 5000;
  CHECK(w.onFrame(h, data, 182, 10) == WindowAssembler::Took::Took && w.expected == 1182);
  CHECK(w.onFrame(h, data, 182, 11) == WindowAssembler::Took::Duplicate);
  h.reqId = 78;
  CHECK(w.onFrame(h, data, 182, 11) == WindowAssembler::Took::Ignored);
  h.reqId = 77; h.offset = 1182;
  CHECK(w.onFrame(h, data, 183, 12) == WindowAssembler::Took::Closed && w.mismatch && w.done);   // too long
  w.begin(77, 12, 1000, false, 0, false);
  h.offset = 1000;
  CHECK(w.onFrame(h, data, 100, 10) == WindowAssembler::Took::Took);
  h.offset = 1150;   // a frame was lost: the prefix is kept
  CHECK(w.onFrame(h, data, 100, 10) == WindowAssembler::Took::Closed && w.gapSeen && w.done && w.plainBytes() == 100);
  // Sealed: plaintext = data - 28; a plain frame for a sealed file closes it.
  w.begin(5, 6, 0, true, 0, true);
  h.reqId = 5; h.fileId = 6; h.offset = 0; h.flags = cu::kRfSealedOnSend;
  CHECK(w.onFrame(h, data, 182, 1) == WindowAssembler::Took::Took && w.expected == 154 && w.f[0].plainLen == 154);
  h.offset = 154; h.flags = 0;
  CHECK(w.onFrame(h, data, 50, 2) == WindowAssembler::Took::Closed && w.mismatch);
  w.begin(5, 6, 0, true, 0, true);
  h.offset = 0; h.flags = cu::kRfSealedOnSend;
  CHECK(w.onFrame(h, data, 28, 1) == WindowAssembler::Took::Closed && w.mismatch);   // no plaintext
  // The last flag, atEnd and growing.
  w.begin(5, 6, 0, false, 0, false);
  h.flags = cu::kRfLast | cu::kRfAtEnd | cu::kRfGrowing;
  CHECK(w.onFrame(h, data, 10, 1) == WindowAssembler::Took::Took && w.done && w.atEnd && w.growing);
  // offset == end: one empty frame, done.
  w.begin(5, 6, 300, true, 0, false);
  h.offset = 300; h.flags = cu::kRfLast | cu::kRfAtEnd | cu::kRfSealedOnSend;
  CHECK(w.onFrame(h, data, 0, 1) == WindowAssembler::Took::Took && w.done && w.plainBytes() == 0);
  // Eight frames end the window even without the last flag.
  w.begin(1, 1, 0, false, 0, false);
  h.reqId = 1; h.fileId = 1; h.flags = 0;
  for (uint8_t k = 0; k < 8; ++k) { h.offset = k * 10u; CHECK(w.onFrame(h, data, 10, k) == WindowAssembler::Took::Took); }
  CHECK(w.done && w.frames == 8 && w.plainBytes() == 80);
  // Timeout from the last frame: 2 s direct, 4 s routed; the prefix stays.
  w.begin(1, 1, 0, false, 100, false);
  h.offset = 0; h.flags = 0;
  w.onFrame(h, data, 10, 500);
  CHECK(!w.timedOut(2499) && w.timedOut(2500) && w.plainBytes() == 10);
  w.begin(1, 1, 0, false, 100, true);
  CHECK(!w.timedOut(4099) && w.timedOut(4100));
  w.clear();
  CHECK(!w.timedOut(100000));
}

static void testStatuses() {
  reset();
  list({ entry(1, 2, "events.log", 0, 1000) });
  planFromList(gD, gL, false, 1u);
  FileState* f = findFile(gD, 1);
  CHECK(write(*f, 0, 100));
  StatusAction a = onReadStatus(gD, *f, cu::kModeData, cu::kStTrimmed, 400, 0);
  CHECK(a.act == Act::Continue && f->pos == 400 && f->deviceStart == 400 && f->gapBytes[kGapTrimmed] == 300 &&
        segmentFor(*f, 400).fresh);
  CHECK(onReadStatus(gD, *f, cu::kModeData, cu::kStPastEnd, 0, 0).act == Act::Relist);
  a = onReadStatus(gD, *f, cu::kModeData, cu::kStBusy, 0, 7);
  CHECK(a.act == Act::Backoff && a.backoffMs == 7000);
  CHECK(onReadStatus(gD, *f, cu::kModeData, cu::kStBusy, 0, 0).backoffMs == 1000);
  CHECK(onReadStatus(gD, *f, cu::kModeData, cu::kStUnsupported, 0, 0).act == Act::DeviceUnsupported);
  a = onReadStatus(gD, *f, cu::kModeData, cu::kStNotReady, 0, 0);
  CHECK(a.act == Act::Backoff && a.backoffMs == kNotReadyBackoffMs);
  CHECK(onReadStatus(gD, *f, cu::kModeData, cu::kStIoError, 0, 0).backoffMs == kNotReadyBackoffMs);
  CHECK(onReadStatus(gD, *f, cu::kModeData, cu::kStOk, 0, 0).act == Act::Continue);
  // A refused READ after an allowed listing is about the file: it leaves the offer tentatively.
  a = onReadStatus(gD, *f, cu::kModeData, cu::kStRefused, 0, 0);
  CHECK(a.act == Act::Continue && f->state == FState::Gone && f->gapBytes[kGapNoLongerOffered] == 600 && f->pos == 400);
  CHECK(onListStatus(cu::kStRefused, 0).act == Act::DeviceRefused);
  CHECK(onListStatus(cu::kStUnsupported, 0).act == Act::DeviceUnsupported);
  CHECK(onListStatus(cu::kStBusy, 30).act == Act::Backoff && onListStatus(cu::kStBusy, 30).backoffMs == 30000);
  CHECK(onListStatus(cu::kStNoLink, 0).backoffMs == kNotReadyBackoffMs);
  CHECK(onListStatus(cu::kStOk, 0).act == Act::Continue);
}

static void testProgress() {
  reset();
  list({ entry(1, 2, "events.log", 0, 1000), entry(2, 5, "errors.log", 0, 500, 200), entry(3, 1, "x.csv", 50, 100) });
  planFromList(gD, gL, false, 1u);
  CHECK(write(*findFile(gD, 1), 0, 600));
  Progress p = progressOf(gD);
  CHECK(p.files == 3 && p.filesLeft == 3 && p.bytesLeft == 400 + 300 + 50 && p.held == 600 && p.elsewhere == 200 &&
        p.gaps[kGapBeforeFirstFetch] == 50);
  CHECK(p.pct == uint8_t(600 * 100 / (600 + 750)));
  CHECK(ewmaBps(0, 1000, 500) == 2000 && ewmaBps(2000, 1000, 1000) == 1750 && ewmaBps(0, 10, 0) == 10000);
  CHECK(etaSeconds(0, 0) == 0 && etaSeconds(10, 0) == 0xFFFFFFFFu && etaSeconds(1001, 1000) == 2);
  reset();
  CHECK(progressOf(gD).pct == 100);
}

static void testScheduler() {
  static Scheduler s;
  memset(&s, 0, sizeof(s));
  const uint8_t a[6] = { 1, 1, 1, 1, 1, 1 }, b[6] = { 2, 2, 2, 2, 2, 2 }, c[6] = { 3, 3, 3, 3, 3, 3 };
  // Not settled, or no file catch-up: not picked.
  s.noteSession(a, 10, false, true, true, kBatteryUnknown, 0);
  CHECK(s.pick(0) < 0);
  s.noteSession(a, 10, true, true, true, kBatteryUnknown, 0);
  s.noteSession(b, 20, true, true, false, 50, 0);
  CHECK(s.find(b)->why == Why::NoFileServe);
  int i = s.pick(0);
  CHECK(i >= 0 && memcmp(s.dev[i].mac, a, 6) == 0);
  // One device in the file stage.
  s.start(i, 0);
  s.noteSession(c, 30, true, true, true, 10, 0);
  CHECK(s.pick(0) < 0 && s.activeIndex() == i && s.dev[i].state == DevState::Listing && !s.dev[i].due);
  s.end(Scheduler::Outcome::Current, 1000, 0, 1800000000u, Why::None);
  CHECK(s.activeIndex() < 0 && s.find(a)->state == DevState::Current && s.find(a)->lastCaughtUpEpoch == 1800000000u);
  // Lowest battery next (c at 10 %).
  s.noteSession(a, 11, true, true, true, 90, 1000);   // a reconnect: due again
  CHECK(s.find(a)->due);
  i = s.pick(1000);
  CHECK(memcmp(s.dev[i].mac, c, 6) == 0);
  // collectnow goes first.
  s.force(*s.find(a), true, 1000);
  i = s.pick(1000);
  CHECK(memcmp(s.dev[i].mac, a, 6) == 0 && s.dev[i].forceFull);
  s.start(i, 1000);
  s.end(Scheduler::Outcome::Backoff, 2000, 5000, 0, Why::Busy);
  CHECK(!s.find(a)->forced && !s.find(a)->forceFull && s.find(a)->due && s.find(a)->why == Why::Busy);
  i = s.pick(2000);
  CHECK(memcmp(s.dev[i].mac, c, 6) == 0);   // a waits its backoff
  CHECK(!s.eligible(*s.find(a), 2000) && s.eligible(*s.find(a), 7000));
  s.start(i, 2000);
  s.end(Scheduler::Outcome::Refused, 2100, 0, 0, Why::Refused);
  CHECK(s.find(c)->state == DevState::Refused && s.pick(7000) >= 0 && memcmp(s.dev[s.pick(7000)].mac, a, 6) == 0);
  s.find(c)->due = 1;
  CHECK(!s.eligible(*s.find(c), 7000));   // refused: not in this session
  s.noteSession(c, 31, true, true, true, 10, 8000);   // a new session clears it
  CHECK(s.eligible(*s.find(c), 8000) && s.find(c)->state == DevState::Idle);
  // The session goes: Suspended (it ran before) or Waiting (it never did).
  s.noteNoSession(*s.find(a));
  CHECK(s.find(a)->state == DevState::Suspended && s.find(a)->why == Why::NoSession && !s.eligible(*s.find(a), 9000));
  s.noteNoSession(*s.find(b));
  CHECK(s.find(b)->state == DevState::Waiting);
  // A reconnect runs at once, whatever backoff the old session had.
  s.find(a)->notBeforeMs = 900000;
  s.noteSession(a, 12, true, true, true, 90, 9500);
  CHECK(s.eligible(*s.find(a), 9500) && s.find(a)->state == DevState::Idle);
  // The 15-minute interval.
  memset(&s, 0, sizeof(s));
  s.noteSession(a, 1, true, true, true, kBatteryUnknown, 0);
  s.start(s.pick(0), 0);
  s.end(Scheduler::Outcome::Current, 100, 0, 1, Why::None);
  s.periodic(100 + kIntervalMs - 1);
  CHECK(!s.find(a)->due);
  s.periodic(100 + kIntervalMs);
  CHECK(s.find(a)->due);
  // Yield at the run ceiling: due again, behind the others.
  s.noteSession(b, 2, true, true, true, kBatteryUnknown, 0);
  s.find(b)->due = 0;
  s.start(s.pick(1000000), 1000000);
  s.end(Scheduler::Outcome::Yield, 1000001, 0, 0, Why::Yielded);
  s.find(b)->due = 1;
  i = s.pick(1000002);
  CHECK(memcmp(s.dev[i].mac, b, 6) == 0);
  s.find(b)->due = 0;
  i = s.pick(1000002);
  CHECK(memcmp(s.dev[i].mac, a, 6) == 0);   // alone, the yielded one runs again
  CHECK(runCeiling(kRunMaxBytes, 0, 1) && runCeiling(0, 0, kRunMaxMs) && !runCeiling(kRunMaxBytes - 1, 0, kRunMaxMs - 1));
  // The boot sweep: every peer once, 1 s apart.
  memset(&s, 0, sizeof(s));
  s.noteSession(a, 1, true, true, true, kBatteryUnknown, 0);
  s.noteSession(b, 2, true, true, true, kBatteryUnknown, 0);
  s.find(a)->due = s.find(b)->due = 0;
  s.bootSweep(5000);
  CHECK(s.find(a)->due && s.find(b)->due && s.find(b)->notBeforeMs == s.find(a)->notBeforeMs + kBootStaggerMs);
  CHECK(s.pick(5000) >= 0);
  s.noteSession(c, 3, true, true, true, kBatteryUnknown, 6000);   // seen after the sweep: due too
  CHECK(s.find(c)->due);
  // A full table drops the device heard least recently (never the active one).
  memset(&s, 0, sizeof(s));
  for (uint8_t k = 0; k < kMaxDevices; ++k) {
    uint8_t m[6] = { 9, 9, 9, 9, 9, k };
    s.noteSession(m, uint16_t(k + 1), true, true, true, kBatteryUnknown, 100u * k);
  }
  s.activePlus1 = 1;   // dev[0] (heard at 0) is active
  uint8_t m[6] = { 8, 8, 8, 8, 8, 8 };
  s.noteSession(m, 99, true, true, true, kBatteryUnknown, 5000);
  CHECK(s.find(m) && s.dev[0].mac[5] == 0 && s.dev[1].mac[0] == 8);
  // Unknown caps do not block (an older peer that confirms later).
  memset(&s, 0, sizeof(s));
  s.noteSession(a, 1, true, false, false, kBatteryUnknown, 0);
  CHECK(s.pick(0) >= 0);
}

static void testTokenBucket() {
  TokenBucket t = {};
  CHECK(t.take(0, 40, 8) && t.take(0, 40, 8) && t.take(0, 40, 8) && t.take(0, 40, 8) && t.take(0, 40, 8));
  CHECK(!t.take(0, 40, 8));        // 40 frames used in this second
  CHECK(!t.take(199, 40, 8));      // 7.96 frames back
  CHECK(t.take(200, 40, 8));       // 8 back after 200 ms
  TokenBucket slow = {};
  CHECK(slow.take(0, 5, 8));       // a window is never refused for good at a low rate
  CHECK(!slow.take(1000, 5, 8) && slow.take(1600, 5, 8));
}

int main() {
  testSafeNameAndGates();
  testPlanGaps();
  testElsewhereAndFull();
  testLedgerChangeAndFullList();
  testListing();
  testRiskOrderAndNextRead();
  testDigest();
  testSegmentsAndRecovery();
  testMerges();
  testAssembler();
  testStatuses();
  testProgress();
  testScheduler();
  testTokenBucket();
  std::printf("collect_core_tests: %u checks passed\n", gChecks);
  return 0;
}
