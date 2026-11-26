// =============================================================================
// Log retention: which capture files may go when a volume runs low, and in
// what order. Pure policy and a bounded step machine; no Arduino or ESP-IDF
// dependency (host test test_log_retention.py, archived 2026-11-26). The device wiring
// (directory listing, removal, the clock, free space) is in
// System_TimeAnchors.cpp, which steps it from timeAnchorsTick.
// =============================================================================
// Owner's rule: several days of system logs and health statistics come first
// and are protected; other dated capture data (sensor logs) is secondary and
// is removed oldest first before the volume fills; system files and
// transcripts are never removed (B12: a full volume refuses a new transcript
// with "Transcript storage full or unavailable", System_Transcript.cpp).
//
// What a pass may remove, in this order, and only while the volume is below
// its low-water mark:
//   1. Secondary files in the oldest dated sensor folder
//      (<root>/logging_captures/sensors/<YYYY-MM-DD>/): every file whose name
//      does not start with "health-". Never in today's folder, never in the
//      kRecentDaysKept newest stored days.
//   2. Secondary files in the boot-named folder of the lowest earlier boot
//      that has no time anchor (boot-<N>/, N below this boot). A folder with
//      an anchor is left to the anchor sweep, which gives its files a date.
//   3. Last, and only when 1-2 found nothing: "health-" files in dated folders
//      that are not among the kProtectedDays newest stored days.
// A pass ends after the files of ONE day (or one boot folder) are gone, or
// earlier when the freed bytes reach the high-water mark; the next pass starts
// kPassIntervalMs later. One step lists at most kEntriesPerStep directory
// entries and removes at most kRemovalsPerStep files. A listing names its
// entries and opens none of them; the one file a step removes is the only one
// whose size is read. No listing goes past kWalkMaxEntries entries: a larger
// folder is handled through its first kWalkMaxEntries entries. That is safe,
// because every "newest stored" rule below is counted over the entries that
// were listed, and a shorter listing can only protect more, never less; the
// rest of the folder comes into reach as the listed part is removed.
//
// Never removed, whatever the free space: the open session's file and its
// rotations, today's folder, files whose name starts with "." (.anchors.csv)
// or ends in ".tmp"/".bak", anything directly in the sensors folder (literal
// path sessions) and sub-folders of a day folder. The pass never looks at
// /system, <root>/stt (transcripts and notes), /logging_captures/system,
// /logging_captures/tracks, r1-health-history (its own budget,
// R1_HealthHistoryStore.cpp) or the collector's store.
//
// "Newest stored days" are counted from the day folders that exist, as the R1
// history store does, not from the clock: a clock that jumps forward removes
// nothing.
//
// The "recent" day horizon follows the catch-up ledger
// (System_CatchupLedger.cpp), which offers a peer the files of the three
// newest day folders: a file the ledger may have offered is not removed before
// the peer could fetch it. (The ledger also
// offers the two newest boot folders; an earlier boot's folder without an
// anchor can still be pruned here.)
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace LogRetention {

// ---------------------------------------------------------------------------
// The constants (an owner question: they are proposals, chosen on the careful
// side).
// ---------------------------------------------------------------------------
// Newest stored days whose health files are never removed.
constexpr unsigned kProtectedDays = 14;
// Newest stored days whose secondary files are never removed either: the
// catch-up ledger's day horizon (kSensorDayFolders in System_CatchupLedger.cpp).
constexpr unsigned kRecentDaysKept = 3;
// Pruning starts when free space is below the larger of this share of the
// volume (capped, so a large card full of other data keeps its captures) and
// this many log reserves.
constexpr unsigned kLowWaterPercent = 20;
constexpr uint64_t kLowWaterPercentCapBytes = 64ull * 1024 * 1024;
constexpr unsigned kLowWaterReserveMultiple = 2;
// It stops this far above the low-water mark: the larger of this share of the
// volume (capped) and one log reserve.
constexpr unsigned kHighWaterStepPercent = 5;
constexpr uint64_t kHighWaterStepCapBytes = 16ull * 1024 * 1024;
// Pace: never in the first minutes after boot, one pass an hour, a bounded
// amount of work per step.
constexpr uint32_t kFirstPassDelayMs = 10u * 60u * 1000u;
constexpr uint32_t kPassIntervalMs = 60u * 60u * 1000u;
// The pass decides on a cached free-space figure (it never samples one: a
// fresh reading of the internal flash walks the whole volume). A figure older
// than this is not acted on, and a decision that found no usable figure is
// taken again after kFreeFigureRetryMs instead of a whole pass interval. (The
// card's figure is read at the decision: nothing else renews it.)
constexpr uint32_t kFreeFigureMaxAgeMs = 10u * 60u * 1000u;
constexpr uint32_t kFreeFigureRetryMs = 60u * 1000u;
constexpr unsigned kEntriesPerStep = 16;
constexpr unsigned kRemovalsPerStep = 1;
// No listing goes past this many entries of one folder (see the top of this
// file): a step reads at most this many names, plus kEntriesPerStep.
constexpr unsigned kWalkMaxEntries = 512;
// Oldest days remembered by one survey of the sensors folder, so a run of days
// with nothing to remove does not cost one survey each.
constexpr unsigned kSurveyCandidates = 8;

inline uint64_t lowWaterBytes(uint64_t totalBytes, uint64_t reserveBytes) {
  uint64_t share = totalBytes / 100 * kLowWaterPercent;
  if (share > kLowWaterPercentCapBytes) share = kLowWaterPercentCapBytes;
  const uint64_t floor = reserveBytes * kLowWaterReserveMultiple;
  return share > floor ? share : floor;
}

inline uint64_t highWaterBytes(uint64_t totalBytes, uint64_t reserveBytes) {
  uint64_t step = totalBytes / 100 * kHighWaterStepPercent;
  if (step > kHighWaterStepCapBytes) step = kHighWaterStepCapBytes;
  if (step < reserveBytes) step = reserveBytes;
  return lowWaterBytes(totalBytes, reserveBytes) + step;
}

// Pruning starts when free space is below the low-water mark and goes on, a
// pass at a time, until it is back at the high-water mark. `pruning` is the
// volume's state between passes (false at boot). When a pass is wanted,
// *needBytes is what it has to free to reach the high-water mark.
inline bool passWanted(uint64_t freeBytes, uint64_t totalBytes, uint64_t reserveBytes, bool* pruning,
                       uint64_t* needBytes) {
  if (totalBytes == 0) return false;
  const uint64_t high = highWaterBytes(totalBytes, reserveBytes);
  if (freeBytes >= high || (!*pruning && freeBytes >= lowWaterBytes(totalBytes, reserveBytes))) {
    *pruning = false;
    return false;
  }
  *pruning = true;
  *needBytes = high - freeBytes;
  return true;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
// A calendar day as yyyymmdd; 0 is "no day".
using DayKey = uint32_t;

inline bool digits(const char* text, size_t count, uint32_t* value) {
  uint32_t v = 0;
  for (size_t i = 0; i < count; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    v = v * 10 + (uint32_t)(text[i] - '0');
  }
  *value = v;
  return true;
}

inline bool validDay(uint32_t year, uint32_t month, uint32_t day) {
  return year >= 2000 && year <= 2199 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

// "YYYY-MM-DD", nothing more.
inline bool parseDayFolder(const char* name, DayKey* out) {
  if (!name || strlen(name) != 10 || name[4] != '-' || name[7] != '-') return false;
  uint32_t y = 0, m = 0, d = 0;
  if (!digits(name, 4, &y) || !digits(name + 5, 2, &m) || !digits(name + 8, 2, &d)) return false;
  if (!validDay(y, m, d)) return false;
  *out = y * 10000 + m * 100 + d;
  return true;
}

inline void formatDayFolder(DayKey day, char* out, size_t cap) {
  snprintf(out, cap, "%04u-%02u-%02u", (unsigned)(day / 10000), (unsigned)(day / 100 % 100),
           (unsigned)(day % 100));
}

// "boot-<N>", nothing more.
inline bool parseBootFolder(const char* name, uint32_t* bootOut) {
  if (!name || strncmp(name, "boot-", 5) != 0) return false;
  const size_t count = strlen(name + 5);
  if (count == 0 || count > 9) return false;
  return digits(name + 5, count, bootOut);
}

inline bool endsWith(const char* name, const char* suffix) {
  const size_t n = strlen(name), s = strlen(suffix);
  return n >= s && strcmp(name + n - s, suffix) == 0;
}

// Files the pass never removes, whatever folder they are in.
inline bool nameIsNeverRemoved(const char* name) {
  return !name || !name[0] || name[0] == '.' || endsWith(name, ".tmp") || endsWith(name, ".bak");
}

// Health statistics: the per-day health CSV and its rotations.
inline bool nameIsHealth(const char* name) {
  return name && strncmp(name, "health-", 7) == 0;
}

// `path` is the open session's file or one of its rotations ("<file>.<n>").
inline bool pathIsOpenSession(const char* path, const char* openFile) {
  if (!path || !openFile || !openFile[0]) return false;
  const size_t n = strlen(openFile);
  if (strncmp(path, openFile, n) != 0) return false;
  if (path[n] == '\0') return true;
  if (path[n] != '.') return false;
  uint32_t generation = 0;
  const size_t count = strlen(path + n + 1);
  return count > 0 && count <= 3 && digits(path + n + 1, count, &generation);
}

// The newest stored days seen so far, newest first.
struct NewestDays {
  DayKey days[kProtectedDays];
  unsigned count;

  void clear() { count = 0; }
  void add(DayKey day) {
    unsigned at = 0;
    while (at < count && days[at] > day) ++at;
    if (at < count && days[at] == day) return;
    if (at >= kProtectedDays) return;
    const unsigned last = count < kProtectedDays ? count : kProtectedDays - 1;
    for (unsigned i = last; i > at; --i) days[i] = days[i - 1];
    days[at] = day;
    if (count < kProtectedDays) ++count;
  }
  // `day` is one of the `newest` newest stored days.
  bool holds(DayKey day, unsigned newest) const {
    const unsigned limit = newest < count ? newest : count;
    for (unsigned i = 0; i < limit; ++i) {
      if (days[i] == day) return true;
    }
    return false;
  }
};

// ---------------------------------------------------------------------------
// The device side of a pass
// ---------------------------------------------------------------------------
struct Entry {
  char name[64];   // the leaf name; an entry whose name does not fit is reported empty
  bool directory;
};

struct Port {
  // Up to `capacity` entries of `directory`, starting at entry `skip` of its
  // listing. *end is set when the listing is complete. False when the
  // directory cannot be opened (not there, or refused). The entries are named
  // only: a listing opens none of them, so it costs skip + capacity directory
  // reads (the pass keeps skip below kWalkMaxEntries).
  virtual bool list(const char* directory, size_t skip, Entry* out, size_t capacity,
                    size_t* count, bool* end) = 0;
  // Removes one file; *freedBytes is its size (0 when it could not be read).
  virtual bool removeFile(const char* path, uint32_t* freedBytes) = 0;
  // Removes the directory when it is empty; false otherwise.
  virtual bool removeEmptyDirectory(const char* path) = 0;
  // The anchor sweep will date this boot's folder: leave it alone.
  virtual bool bootIsAnchored(uint32_t boot) = 0;
  virtual ~Port() = default;
};

// What the pass may not touch, and where "now" is.
struct Limits {
  DayKey today;                // local date: today's sensor folder is never touched
  uint32_t currentBoot;        // only boot folders of earlier boots may be looked at
  const char* openSessionFile; // the sensor log's active file as a path on this volume, or ""
};

enum class Step : uint8_t {
  Working,   // call step() again on a later lap
  Finished,  // the pass is over (see removedFiles / freedBytes)
};

class Pass {
 public:
  // Starts a pass on the volume whose paths begin with `root` ("" for the
  // internal flash, "/sd" for the card). `needBytes` is how much has to be
  // freed to reach the high-water mark. `forgetFloors` drops what earlier
  // passes learned about folders with nothing to remove (a new day, another
  // volume); without it a pass does not walk through them again.
  void begin(const char* root, uint64_t needBytes, bool forgetFloors) {
    if (forgetFloors || strcmp(root_, root ? root : "") != 0) {
      dayFloor_[0] = dayFloor_[1] = 0;
      bootFloor_ = 0;
    }
    snprintf(root_, sizeof(root_), "%s", root ? root : "");
    need_ = needBytes;
    freed_ = 0;
    removed_ = 0;
    removedHere_ = 0;
    healthRound_ = false;
    lastDay_ = 0;
    startSurvey(Phase::SurveyDays);
  }

  bool active() const { return phase_ != Phase::Idle; }
  unsigned removedFiles() const { return removed_; }
  uint64_t freedBytes() const { return freed_; }
  // The day (or 0 for a boot folder) the last removed file belonged to.
  DayKey lastDay() const { return lastDay_; }
  const char* root() const { return root_; }

  Step step(Port& port, const Limits& limits) {
    switch (phase_) {
      case Phase::SurveyDays: return surveyDays(port, limits);
      case Phase::PruneFolder: return pruneFolder(port, limits);
      case Phase::SurveyBoots: return surveyBoots(port, limits);
      case Phase::Idle: break;
    }
    return Step::Finished;
  }

 private:
  enum class Phase : uint8_t {
    Idle = 0, SurveyDays, PruneFolder, SurveyBoots,
  };

  void sensorsDirectory(char* out, size_t cap) const {
    snprintf(out, cap, "%s/logging_captures/sensors", root_);
  }

  void startSurvey(Phase phase) {
    phase_ = phase;
    cursor_ = 0;
    newest_.clear();
    candidateCount_ = 0;
    candidateIndex_ = 0;
    candidateBoot_ = 0;
    haveCandidateBoot_ = false;
  }

  DayKey& dayFloor() { return dayFloor_[healthRound_ ? 1 : 0]; }

  // Keeps the kSurveyCandidates oldest days above the floor, oldest first.
  void noteCandidate(DayKey day) {
    unsigned at = 0;
    while (at < candidateCount_ && candidates_[at] < day) ++at;
    if (at < candidateCount_ && candidates_[at] == day) return;
    if (at >= kSurveyCandidates) return;
    const unsigned last = candidateCount_ < kSurveyCandidates ? candidateCount_ : kSurveyCandidates - 1;
    for (unsigned i = last; i > at; --i) candidates_[i] = candidates_[i - 1];
    candidates_[at] = day;
    if (candidateCount_ < kSurveyCandidates) ++candidateCount_;
  }

  // The folder is gone from the listing when nothing is left in it.
  void tidyFolder(Port& port) {
    size_t count = 0;
    bool end = false;
    if (port.list(folder_, 0, scratch_, 1, &count, &end) && count == 0) {
      (void)port.removeEmptyDirectory(folder_);
    }
  }

  Step finish() {
    phase_ = Phase::Idle;
    return Step::Finished;
  }

  // The listing that `cursor_` walks is over: its end was reached, or the
  // walk limit was (the rest of a larger folder is not looked at).
  bool listingDone(bool end, size_t count) const {
    return end || count == 0 || cursor_ >= kWalkMaxEntries;
  }

  // One listing step over the sensors folder: the newest stored days and the
  // oldest day above the floor.
  Step surveyDays(Port& port, const Limits& limits) {
    char directory[96];
    sensorsDirectory(directory, sizeof(directory));
    size_t count = 0;
    bool end = false;
    if (!port.list(directory, cursor_, scratch_, kEntriesPerStep, &count, &end)) {
      // No capture tree on this volume.
      return afterDays();
    }
    for (size_t i = 0; i < count; ++i) {
      DayKey day = 0;
      if (!scratch_[i].directory || !parseDayFolder(scratch_[i].name, &day)) continue;
      newest_.add(day);
      if (day > dayFloor()) noteCandidate(day);
    }
    cursor_ += count;
    if (!listingDone(end, count)) return Step::Working;
    // The whole listing is in: the oldest days above the floor, oldest first.
    return nextCandidateDay(limits);
  }

  // The next surveyed day, if it may be pruned in this round: before today and
  // not one of the newest stored days. The candidates are in date order, so
  // the first one that may not be pruned ends the round.
  Step nextCandidateDay(const Limits& limits) {
    if (candidateIndex_ >= candidateCount_) {
      // A full set was tried and nothing could go: there may be more above it.
      if (candidateCount_ == kSurveyCandidates) {
        startSurvey(Phase::SurveyDays);
        return Step::Working;
      }
      return afterDays();
    }
    const DayKey day = candidates_[candidateIndex_++];
    const unsigned keep = healthRound_ ? kProtectedDays : kRecentDaysKept;
    if (day >= limits.today || newest_.holds(day, keep)) return afterDays();
    char name[16];
    char directory[96];
    sensorsDirectory(directory, sizeof(directory));
    formatDayFolder(day, name, sizeof(name));
    snprintf(folder_, sizeof(folder_), "%s/%s", directory, name);
    folderDay_ = day;
    folderIsBoot_ = false;
    cursor_ = 0;
    removedHere_ = 0;
    phase_ = Phase::PruneFolder;
    return Step::Working;
  }

  // No (further) day to prune in this round.
  Step afterDays() {
    if (healthRound_) return finish();
    startSurvey(Phase::SurveyBoots);
    return Step::Working;
  }

  // The file may go in this round.
  bool prunable(const Entry& entry, const char* path, const Limits& limits) const {
    if (entry.directory || nameIsNeverRemoved(entry.name)) return false;
    // The open session is named by its primary path even while its rows go to
    // the card's mirror of it, or by a path on the card itself: both are kept.
    if (pathIsOpenSession(path, limits.openSessionFile) ||
        pathIsOpenSession(path + strlen(root_), limits.openSessionFile)) return false;
    const bool health = nameIsHealth(entry.name);
    // Round 1 takes secondary files only; the last round takes health files
    // of days that are no longer protected. A boot folder's health files have
    // no date and stay.
    if (healthRound_) return health && !folderIsBoot_;
    return !health;
  }

  // One step inside the chosen folder: remove the first file that may go.
  Step pruneFolder(Port& port, const Limits& limits) {
    size_t count = 0;
    bool end = false;
    if (!port.list(folder_, cursor_, scratch_, kEntriesPerStep, &count, &end)) return folderDone(port, limits);
    unsigned removedThisStep = 0;
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
      char path[192];
      const int n = snprintf(path, sizeof(path), "%s/%s", folder_, scratch_[i].name);
      const bool fits = n > 0 && (size_t)n < sizeof(path);
      uint32_t size = 0;
      if (removedThisStep < kRemovalsPerStep && fits && prunable(scratch_[i], path, limits) &&
          port.removeFile(path, &size)) {
        ++removedThisStep;
        ++removed_;
        ++removedHere_;
        freed_ += size;
        lastDay_ = folderIsBoot_ ? 0 : folderDay_;
        continue;
      }
      if (removedThisStep >= kRemovalsPerStep) break;  // the rest is looked at next step
      ++kept;  // stays where it is in the listing: skipped from now on
    }
    cursor_ += kept;
    if (freed_ >= need_ && removed_ > 0) {
      // The high-water mark is reached; an emptied folder goes on the way out.
      tidyFolder(port);
      return finish();
    }
    if (removedThisStep > 0) return Step::Working;
    if (!listingDone(end, count)) return Step::Working;
    return folderDone(port, limits);
  }

  // Nothing (more) to remove in this folder.
  Step folderDone(Port& port, const Limits& limits) {
    if (removedHere_ > 0) {
      // Its files are gone: the folder too when nothing is left in it. One
      // day (or boot folder) per pass.
      tidyFolder(port);
      return finish();
    }
    // Nothing here could go: remember that, and look at the next one above it.
    if (folderIsBoot_) {
      bootFloor_ = folderBoot_ + 1;
      startSurvey(Phase::SurveyBoots);
      return Step::Working;
    }
    dayFloor() = folderDay_;
    return nextCandidateDay(limits);
  }

  // The lowest earlier boot whose folder has no anchor.
  Step surveyBoots(Port& port, const Limits& limits) {
    char directory[96];
    sensorsDirectory(directory, sizeof(directory));
    size_t count = 0;
    bool end = false;
    if (!port.list(directory, cursor_, scratch_, kEntriesPerStep, &count, &end)) return afterBoots();
    for (size_t i = 0; i < count; ++i) {
      uint32_t boot = 0;
      if (!scratch_[i].directory || !parseBootFolder(scratch_[i].name, &boot)) continue;
      if (boot >= limits.currentBoot || boot < bootFloor_) continue;
      if (haveCandidateBoot_ && boot >= candidateBoot_) continue;
      if (port.bootIsAnchored(boot)) continue;
      char path[sizeof(folder_)];
      const int n = snprintf(path, sizeof(path), "%s/%s", directory, scratch_[i].name);
      if (n <= 0 || (size_t)n >= sizeof(path)) continue;  // never taken as a cut path
      candidateBoot_ = boot;
      haveCandidateBoot_ = true;
      memcpy(folder_, path, (size_t)n + 1);
    }
    cursor_ += count;
    if (!listingDone(end, count)) return Step::Working;
    if (!haveCandidateBoot_) return afterBoots();
    folderBoot_ = candidateBoot_;
    folderIsBoot_ = true;
    cursor_ = 0;
    removedHere_ = 0;
    phase_ = Phase::PruneFolder;
    return Step::Working;
  }

  // Nothing (more) secondary may go in this pass.
  Step afterBoots() {
    if (removed_ > 0 || healthRound_) return finish();
    // Nothing secondary could go anywhere on this volume: the last round,
    // health files of days that are no longer protected.
    healthRound_ = true;
    startSurvey(Phase::SurveyDays);
    return Step::Working;
  }

  // No default member initializers: a zeroed object (static storage, PSRAM
  // .bss on the device) is an idle pass, and Phase::Idle is 0.
  Phase phase_;
  char root_[8];
  uint64_t need_;
  uint64_t freed_;
  unsigned removed_;
  unsigned removedHere_;   // in the folder being pruned
  bool healthRound_;   // the last round: expired health files
  DayKey lastDay_;

  size_t cursor_;          // entries of the current listing already looked at
  NewestDays newest_;
  DayKey candidates_[kSurveyCandidates];  // oldest days above the floor, oldest first
  unsigned candidateCount_;
  unsigned candidateIndex_;
  // Days up to this one had nothing to remove: [0] secondary files, [1] the
  // last round's expired health files. Kept from pass to pass (begin()).
  DayKey dayFloor_[2];
  uint32_t candidateBoot_;
  bool haveCandidateBoot_;
  uint32_t bootFloor_;     // boots below this one had nothing to remove

  char folder_[128];      // the folder being pruned or surveyed
  DayKey folderDay_;
  uint32_t folderBoot_;
  bool folderIsBoot_;

  Entry scratch_[kEntriesPerStep];
};

}  // namespace LogRetention
