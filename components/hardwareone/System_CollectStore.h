// System_CollectStore.h - file catch-up, the collector's SD card side
//
// What a collector pulled from a device lives under
//   /sd/collect/<mac12>/files/index.json              schema 1, StagedWrite, times in UTC
//   /sd/collect/<mac12>/files/index-archive.jsonl     one line per retired file or segment
//   /sd/collect/<mac12>/files/<ledgerId8>-<fileId8>-s<k>.raw     plain bytes, logical [from, to)
//   /sd/collect/<mac12>/files/<ledgerId8>-<fileId8>-s<k>.sealed  "HW1CSG1\n" + sealed records
// Every name comes from the MAC and the ids (espnowcatchup::localSegmentName);
// the device's own file name is only ever a JSON string in index.json. All of
// it stays inside VFS::Scopes::COLLECT_SD.
//
// The segments are the truth; index.json is advisory: at load a .raw
// segment's length overrides its `to`, and a .sealed segment is scanned to its
// last whole record (a torn tail is cut). A new pairing of the device (its key
// changed) moves files/ aside to files.prev-<UTC stamp>.
//
// Runs on the main loop (collectWriteTick, System_Collect.cpp), one job at a
// time; FsLockGuard is held per file operation, never across a whole job.
// Buffers are PSRAM statics; the JSON document is PSRAM_JSON_DOC and is
// serialised straight into the staged file. Host test: test_collect_store.py.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "System_CollectCore.h"

namespace collectstore {

constexpr size_t kPathCap = 96;
constexpr uint8_t kIndexSchema = 1;

// "/sd/collect/<mac12>" and "/sd/collect/<mac12>/files". The length, 0 if it does not fit.
size_t deviceDir(const uint8_t mac[6], char* out, size_t cap);
size_t filesDir(const uint8_t mac[6], char* out, size_t cap);
// The path of segment `seg` of file `f`, from ids only.
size_t segmentPath(const collectcore::DeviceFiles& d, const collectcore::FileState& f, uint16_t seg,
                   char* out, size_t cap);

struct LoadResult {
  bool     fresh;             // no index (or an unreadable one): a new namespace
  bool     badIndex;          // index.json did not parse: kept as index.json.bad
  bool     identityChanged;   // the paired key differs: files/ moved to files.prev-<stamp>
  uint8_t  recovered;         // segments whose length the card corrected
  uint8_t  tornCut;           // .sealed segments whose torn tail was cut
  uint8_t  missing;           // segments the index named that are not on the card
};
// Loads the device's index into `d` (fills mac, peerKeyFp). False only when the
// card cannot be used (not writable, the files folder cannot be made).
// `readOnly` (collectstatus of a device that is not being pulled): nothing on
// the card changes; the recovery is applied to `d` only.
bool loadIndex(const uint8_t mac[6], const char* peerKeyFp, uint32_t nowEpoch, collectcore::DeviceFiles& d,
               LoadResult* out, bool readOnly = false);
// Writes index.json (StagedWrite; the previous one stays until it succeeds).
bool saveIndex(collectcore::DeviceFiles& d, uint32_t nowEpoch);
// One index-archive.jsonl line: a retired file (reason "ledger_reset",
// "evicted", "dropped") with its segments and gaps, or one segment that left
// a file's array ("segment_rolled").
bool archiveFile(const collectcore::DeviceFiles& d, const collectcore::FileState& f, const char* reason,
                 uint32_t nowEpoch);
bool archiveSegment(const collectcore::DeviceFiles& d, const collectcore::FileState& f,
                    const collectcore::Segment& s, const char* reason, uint32_t nowEpoch);

struct WriteResult {
  uint32_t committed;         // plaintext bytes now on the card and in `f`
  bool     ioError;           // a write or flush failed (what landed whole is committed)
  bool     rejected;          // the window is not where the file stands: nothing written
  bool     rolled;            // a segment left the array (its archive line is written)
  bool     opened;            // a new segment file holds committed bytes: index.json must name it
                              // before any READ claims storedBefore past its start
};
// Appends one assembled window to its segment, flushes it, then records it in
// `f` (commitWrite). Only after this returns may the next READ claim
// storedBefore.
WriteResult appendWindow(collectcore::DeviceFiles& d, collectcore::FileState& f,
                         const collectcore::WindowAssembler& w, uint32_t nowEpoch);

// SHA-256 of the held logical range [from, from+len) of a plain file, from
// the one .raw segment that covers it, truncated to 16 bytes (the device's
// digest). False: no single segment covers it, or a read failed.
bool segmentDigest(const collectcore::DeviceFiles& d, const collectcore::FileState& f, uint32_t from, uint32_t len,
                   uint8_t out[espnowcatchup::kDigestLen]);

// What the device's files take on the card: the segments the index names
// plus the files in the folder it no longer names (retired). Walks the folder.
struct CardUse { uint64_t indexed, retired; uint16_t retiredFiles; };
bool cardUse(const collectcore::DeviceFiles& d, CardUse* out);

// The per-device quota: removes retired segment files first, oldest first,
// then evicts the oldest Closed files (their segments go, the file stays in
// the index as evicted so it is not fetched again). Never the file
// `protectFileId` (being fetched). Returns the bytes on the card after.
struct QuotaResult { uint64_t before, after; uint16_t removedRetired, evictedFiles; };
QuotaResult enforceQuota(collectcore::DeviceFiles& d, uint64_t quotaBytes, uint32_t protectFileId, uint32_t nowEpoch);

// "2026-11-19T18:02:11Z" (UTC); "" for 0.
size_t formatUtc(uint32_t epoch, char* out, size_t cap);
bool parseUtc(const char* text, uint32_t* epoch);

}  // namespace collectstore
