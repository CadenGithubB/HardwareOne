// Host harness for System_LogRetentionCore.h: the retention rule, its water
// marks and the bounded step machine, against an in-memory volume. The header
// is compiled as it is (no Arduino or ESP-IDF dependency).
#include "System_LogRetentionCore.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace LogRetention;

static unsigned checks = 0;
static const char* currentCase = "";
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "log retention check failed in %s at line %d: %s\n", currentCase, line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

// ---------------------------------------------------------------------------
// The volume
// ---------------------------------------------------------------------------
struct Volume final : Port {
  std::map<std::string, uint32_t> files;  // path -> size
  std::set<std::string> directories;
  std::set<uint32_t> anchoredBoots;
  std::set<std::string> refuseRemoval;
  std::vector<std::string> removed;
  unsigned listCalls = 0, entriesListed = 0, removalsTried = 0;

  void dir(const std::string& path) {
    for (size_t at = path.find('/', 1); at != std::string::npos; at = path.find('/', at + 1)) {
      directories.insert(path.substr(0, at));
    }
    directories.insert(path);
  }
  void file(const std::string& path, uint32_t size = 1000) {
    dir(path.substr(0, path.rfind('/')));
    files[path] = size;
  }
  bool has(const std::string& path) const { return files.count(path) || directories.count(path); }

  std::vector<std::pair<std::string, bool>> children(const std::string& directory) const {
    std::vector<std::pair<std::string, bool>> out;
    const std::string prefix = directory + "/";
    for (const auto& d : directories) {
      if (d.size() > prefix.size() && d.compare(0, prefix.size(), prefix) == 0 &&
          d.find('/', prefix.size()) == std::string::npos) out.emplace_back(d.substr(prefix.size()), true);
    }
    for (const auto& f : files) {
      if (f.first.size() > prefix.size() && f.first.compare(0, prefix.size(), prefix) == 0 &&
          f.first.find('/', prefix.size()) == std::string::npos) out.emplace_back(f.first.substr(prefix.size()), false);
    }
    return out;  // directories then files, each in name order: a stable listing
  }

  bool list(const char* directory, size_t skip, Entry* out, size_t capacity, size_t* count, bool* end) override {
    ++listCalls;
    *count = 0;
    *end = false;
    if (!directories.count(directory)) return false;
    const auto all = children(directory);
    check(skip < kWalkMaxEntries);  // the pass never asks beyond the walk limit
    for (size_t i = skip; i < all.size(); ++i) {
      if (*count == capacity) return true;
      Entry& entry = out[*count];
      if (all[i].first.size() < sizeof(entry.name)) std::snprintf(entry.name, sizeof(entry.name), "%s", all[i].first.c_str());
      else entry.name[0] = '\0';
      entry.directory = all[i].second;
      ++*count;
      ++entriesListed;
    }
    *end = true;
    return true;
  }
  bool removeFile(const char* path, uint32_t* freedBytes) override {
    ++removalsTried;
    *freedBytes = 0;
    if (refuseRemoval.count(path) || !files.count(path)) return false;
    *freedBytes = files.at(path);
    files.erase(path);
    removed.emplace_back(path);
    return true;
  }
  bool removeEmptyDirectory(const char* path) override {
    if (!directories.count(path) || !children(path).empty()) return false;
    directories.erase(path);
    return true;
  }
  bool bootIsAnchored(uint32_t boot) override { return anchoredBoots.count(boot) != 0; }
};

static const std::string kSensors = "/logging_captures/sensors";
static std::string dayOf(int index) {  // 2026-03-01 .. 2026-03-31
  char name[16];
  std::snprintf(name, sizeof(name), "2026-03-%02d", index);
  return name;
}
static std::string sensorFile(const std::string& root, int day) {
  return root + kSensors + "/" + dayOf(day) + "/sensors-" + dayOf(day) + ".csv";
}
static std::string healthFile(const std::string& root, int day) {
  return root + kSensors + "/" + dayOf(day) + "/health-" + dayOf(day) + ".csv";
}

// Twenty stored days, 1..20, each with a sensor log and a health file.
static void twentyDays(Volume& volume, const std::string& root = "") {
  for (int day = 1; day <= 20; ++day) {
    volume.file(sensorFile(root, day), 40000);
    volume.file(healthFile(root, day), 2000);
  }
}

static Limits limitsOn(int today, const char* openFile = "") {
  Limits limits{};
  limits.today = 20260300u + static_cast<uint32_t>(today);
  limits.currentBoot = 50;
  limits.openSessionFile = openFile;
  return limits;
}

struct Outcome { unsigned steps, removed; uint64_t freed; };

// Runs one pass to its end. Every step lists at most kEntriesPerStep entries
// (plus the one-entry look that decides whether a folder is empty) and tries
// at most kRemovalsPerStep removals.
static Outcome runPass(Pass& pass, Volume& volume, const Limits& limits, const char* root, uint64_t need,
                       bool forgetFloors = true) {
  pass.begin(root, need, forgetFloors);
  Outcome outcome{0, 0, 0};
  while (true) {
    const unsigned listed = volume.entriesListed, tried = volume.removalsTried, calls = volume.listCalls;
    const Step step = pass.step(volume, limits);
    ++outcome.steps;
    check(volume.entriesListed - listed <= kEntriesPerStep + 1);
    check(volume.removalsTried - tried <= kRemovalsPerStep);
    check(volume.listCalls - calls <= 2);
    check(outcome.steps < 5000);  // every pass ends
    if (step == Step::Finished) break;
  }
  check(!pass.active());
  outcome.removed = pass.removedFiles();
  outcome.freed = pass.freedBytes();
  return outcome;
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------
static void waterMarks() {
  currentCase = "waterMarks";
  const uint64_t reserve = 100 * 1024;
  // The P4X-EYE's internal flash (9068 KiB): prune below 1.81 MB free, stop at 2.27 MB.
  const uint64_t flash = 9068ull * 1024;
  check(lowWaterBytes(flash, reserve) == flash / 100 * 20);
  check(lowWaterBytes(flash, reserve) / 1024 == 1813 && highWaterBytes(flash, reserve) / 1024 == 2266);
  // A small flash: two reserves are more than 20 %.
  check(lowWaterBytes(900 * 1024, reserve) == 2 * reserve);
  check(highWaterBytes(900 * 1024, reserve) == 3 * reserve);
  // A large card: the share is capped, so a card full of other data keeps its captures.
  const uint64_t card = 32ull * 1024 * 1024 * 1024;
  check(lowWaterBytes(card, reserve) == kLowWaterPercentCapBytes);
  check(highWaterBytes(card, reserve) == kLowWaterPercentCapBytes + kHighWaterStepCapBytes);

  // A pass is wanted only below the low-water mark, and then until the high one.
  bool pruning = false;
  uint64_t need = 0;
  const uint64_t low = lowWaterBytes(flash, reserve), high = highWaterBytes(flash, reserve);
  check(!passWanted(low, flash, reserve, &pruning, &need) && !pruning);
  check(!passWanted(flash, flash, reserve, &pruning, &need));
  check(passWanted(low - 1, flash, reserve, &pruning, &need) && pruning && need == high - low + 1);
  check(passWanted(low + 1000, flash, reserve, &pruning, &need) && need == high - low - 1000);  // between the marks: goes on
  check(!passWanted(high, flash, reserve, &pruning, &need) && !pruning);                        // reached: stops
  check(!passWanted(low + 1000, flash, reserve, &pruning, &need));                              // and stays stopped
  check(!passWanted(0, 0, reserve, &pruning, &need));                                           // no volume figure
}

static void names() {
  currentCase = "names";
  DayKey day = 0;
  check(parseDayFolder("2026-03-07", &day) && day == 20260307u);
  for (const char* bad : {"2026-3-7", "2026-03-07x", "boot-12", "20260307", "2026-13-01", "2026-00-10", "2026-01-32",
                          "1999-01-01", "abcd-ef-gh", "", "2026-03-07/"}) {
    check(!parseDayFolder(bad, &day));
  }
  check(!parseDayFolder(nullptr, &day));
  char text[16];
  formatDayFolder(20260307u, text, sizeof(text));
  check(std::string(text) == "2026-03-07");
  uint32_t boot = 0;
  check(parseBootFolder("boot-12", &boot) && boot == 12 && parseBootFolder("boot-000007", &boot) && boot == 7);
  for (const char* bad : {"boot-", "boot-x", "boot-12a", "boot12", "Boot-12", "boot-1234567890"}) check(!parseBootFolder(bad, &boot));
  check(nameIsNeverRemoved(".anchors.csv") && nameIsNeverRemoved("x.csv.tmp") && nameIsNeverRemoved("x.bak") &&
        nameIsNeverRemoved("") && nameIsNeverRemoved(nullptr) && !nameIsNeverRemoved("sensors-2026-03-07.csv"));
  check(nameIsHealth("health-2026-03-07.csv") && nameIsHealth("health-2026-03-07.csv.1") && !nameIsHealth("sensors-health-.csv"));
  check(pathIsOpenSession("/a/b.csv", "/a/b.csv") && pathIsOpenSession("/a/b.csv.1", "/a/b.csv") &&
        pathIsOpenSession("/a/b.csv.12", "/a/b.csv") && !pathIsOpenSession("/a/b.csv.x", "/a/b.csv") &&
        !pathIsOpenSession("/a/b.csvx", "/a/b.csv") && !pathIsOpenSession("/a/b.csv", "") &&
        !pathIsOpenSession("/a/c.csv", "/a/b.csv"));

  NewestDays newest;
  newest.clear();
  for (uint32_t d : {5u, 3u, 9u, 3u, 1u}) newest.add(20260300u + d);
  check(newest.count == 4 && newest.days[0] == 20260309u && newest.days[3] == 20260301u);
  check(newest.holds(20260309u, 1) && !newest.holds(20260305u, 1) && newest.holds(20260305u, 2) && !newest.holds(20260307u, 14));
  for (uint32_t d = 10; d <= 31; ++d) newest.add(20260300u + d);
  check(newest.count == kProtectedDays && newest.days[0] == 20260331u && newest.days[kProtectedDays - 1] == 20260318u);
  check(!newest.holds(20260317u, kProtectedDays) && newest.holds(20260318u, kProtectedDays));
}

// The brief's volume: twenty day folders. Secondary files go oldest first, a
// day a pass; the health files of the newest fourteen days stay whatever
// happens, and the others only go once nothing secondary is left.
static void twentyDayVolume() {
  currentCase = "twentyDayVolume";
  Volume volume;
  twentyDays(volume);
  volume.file(kSensors + "/.anchors.csv", 60);
  volume.file(kSensors + "/sensors.txt", 5000);                       // a literal-path session at the tree root
  volume.file(kSensors + "/2026-03-03/.hidden", 10);
  volume.file(kSensors + "/2026-03-03/half.csv.tmp", 10);
  volume.file(kSensors + "/2026-03-03/old.csv.bak", 10);
  volume.file(kSensors + "/2026-03-03/nested/inner.csv", 10);         // a sub-folder is not looked into
  volume.file("/logging_captures/system/system-2026-03-01T10-00-00.log", 9000);
  volume.file("/logging_captures/tracks/track-2026-03-01.csv", 9000);
  volume.file("/system/settings.json", 4000);
  volume.file("/r1-health-history/2026-03-01.bin", 4000);
  const Limits limits = limitsOn(25);  // today is after every stored day
  Pass pass{};

  // Pass 1: the oldest day's sensor log, and nothing else.
  Outcome out = runPass(pass, volume, limits, "", 1000000);
  check(out.removed == 1 && out.freed == 40000 && pass.lastDay() == 20260301u);
  check(volume.removed.size() == 1 && volume.removed[0] == sensorFile("", 1));
  check(volume.has(healthFile("", 1)) && volume.has(kSensors + "/2026-03-01"));
  // Day by day, oldest first, up to the three newest stored days.
  for (int day = 2; day <= 17; ++day) {
    out = runPass(pass, volume, limits, "", 1000000, false);
    check(out.removed == 1 && volume.removed.back() == sensorFile("", day));
  }
  // The three newest stored days keep their sensor logs: nothing secondary is
  // left, so the last round takes the health files of the days that are no
  // longer among the fourteen newest (1..6), oldest first, one day a pass.
  for (int day = 1; day <= 6; ++day) {
    out = runPass(pass, volume, limits, "", 1000000, false);
    check(out.removed == 1 && volume.removed.back() == healthFile("", day) && out.freed == 2000);
    if (day != 3) check(!volume.has(kSensors + "/" + dayOf(day)));  // an emptied day folder goes too
  }
  // Now nothing may go: the pass ends empty-handed, every time.
  for (int repeat = 0; repeat < 3; ++repeat) {
    out = runPass(pass, volume, limits, "", 1000000, repeat == 0);
    check(out.removed == 0 && out.freed == 0);
  }
  for (int day = 7; day <= 20; ++day) check(volume.has(healthFile("", day)));   // the fourteen newest days' health
  for (int day = 18; day <= 20; ++day) check(volume.has(sensorFile("", day)));  // the three newest days' sensor logs
  // What is never removed.
  for (const char* kept : {"/logging_captures/sensors/.anchors.csv", "/logging_captures/sensors/sensors.txt",
                           "/logging_captures/sensors/2026-03-03/.hidden",
                           "/logging_captures/sensors/2026-03-03/half.csv.tmp",
                           "/logging_captures/sensors/2026-03-03/old.csv.bak",
                           "/logging_captures/sensors/2026-03-03/nested/inner.csv",
                           "/logging_captures/system/system-2026-03-01T10-00-00.log",
                           "/logging_captures/tracks/track-2026-03-01.csv", "/system/settings.json",
                           "/r1-health-history/2026-03-01.bin"}) {
    check(volume.has(kept));
  }
  check(volume.removed.size() == 17 + 6);
}

// The high-water mark ends a pass inside a day; today's folder, a folder dated
// after today, and the open session with its rotations are never touched.
static void limitsAreKept() {
  currentCase = "limitsAreKept";
  Volume volume;
  twentyDays(volume);
  for (int part = 1; part <= 5; ++part) volume.file(kSensors + "/2026-03-01/part-" + std::to_string(part) + ".csv", 10000);
  Pass pass{};
  // 25 KB needed: three 10 KB files of the oldest day, not the whole day.
  Outcome out = runPass(pass, volume, limitsOn(25), "", 25000);
  check(out.removed == 3 && out.freed == 30000 && volume.removed.back() == kSensors + "/2026-03-01/part-3.csv");
  check(volume.has(sensorFile("", 1)) && volume.has(kSensors + "/2026-03-01/part-4.csv"));

  // Today is day 18: 18 is never touched, 19 and 20 are dated after today.
  Volume later;
  twentyDays(later);
  const std::string open = sensorFile("", 1);
  later.file(open + ".1", 500);
  later.file(open + ".2", 500);
  Pass second{};
  const Limits limits = limitsOn(18, open.c_str());
  unsigned passes = 0;
  while (runPass(second, later, limits, "", 100000000, passes == 0).removed > 0) check(++passes < 100);
  check(later.has(open) && later.has(open + ".1") && later.has(open + ".2"));   // the open session and its rotations
  for (int day = 2; day <= 17; ++day) check(!later.has(sensorFile("", day)));
  for (int day = 18; day <= 20; ++day) check(later.has(sensorFile("", day)) && later.has(healthFile("", day)));
  // The newest stored days are counted from the folders, not from the clock:
  // with today far in the future the three newest stored days still stay.
  Volume jump;
  twentyDays(jump);
  Pass third{};
  Limits future = limitsOn(25);
  future.today = 20991231u;
  passes = 0;
  while (runPass(third, jump, future, "", 100000000, passes == 0).removed > 0) check(++passes < 100);
  for (int day = 18; day <= 20; ++day) check(jump.has(sensorFile("", day)));
  check(!jump.has(sensorFile("", 17)));
  for (int day = 7; day <= 20; ++day) check(jump.has(healthFile("", day)));
  // A file that cannot be removed is skipped, not retried forever.
  Volume stuck;
  twentyDays(stuck);
  stuck.refuseRemoval.insert(sensorFile("", 1));
  Pass fourth{};
  out = runPass(fourth, stuck, limitsOn(25), "", 1000000);
  check(out.removed == 1 && stuck.removed[0] == sensorFile("", 2) && stuck.has(sensorFile("", 1)));
}

// Boot-named folders: only earlier boots without an anchor, lowest first, and
// never their health files.
static void bootFolders() {
  currentCase = "bootFolders";
  Volume volume;
  volume.file(kSensors + "/boot-7/sensors-boot7-1000.csv", 3000);
  volume.file(kSensors + "/boot-7/health-boot7-1000.csv", 300);
  volume.file(kSensors + "/boot-3/sensors-boot3-1000.csv", 3000);
  volume.file(kSensors + "/boot-3/sensors-boot3-9000.csv", 3000);
  volume.file(kSensors + "/boot-5/sensors-boot5-1000.csv", 3000);   // anchored: the sweep will date it
  volume.file(kSensors + "/boot-50/sensors-boot50-1000.csv", 3000); // this boot
  volume.file(kSensors + "/boot-60/sensors-boot60-1000.csv", 3000); // a later number: not an earlier boot
  volume.anchoredBoots.insert(5);
  // One recent day: nothing to prune among the days.
  volume.file(sensorFile("", 20), 1000);
  Pass pass{};
  const Limits limits = limitsOn(25);
  Outcome out = runPass(pass, volume, limits, "", 1000000);
  check(out.removed == 2 && pass.lastDay() == 0 && !volume.has(kSensors + "/boot-3"));
  out = runPass(pass, volume, limits, "", 1000000, false);
  check(out.removed == 1 && volume.removed.back() == kSensors + "/boot-7/sensors-boot7-1000.csv");
  check(volume.has(kSensors + "/boot-7/health-boot7-1000.csv"));
  out = runPass(pass, volume, limits, "", 1000000, false);
  check(out.removed == 0);
  for (const char* kept : {"/logging_captures/sensors/boot-5/sensors-boot5-1000.csv",
                           "/logging_captures/sensors/boot-50/sensors-boot50-1000.csv",
                           "/logging_captures/sensors/boot-60/sensors-boot60-1000.csv"}) check(volume.has(kept));
  check(volume.has(sensorFile("", 20)));
}

// B12: transcripts are never removed. Old dated ones, undated ones and notes
// stay while the sensor days and then the expired health files go, and a
// volume with only transcripts ends every pass empty-handed.
static void transcriptsKept() {
  currentCase = "transcriptsKept";
  Volume volume;
  const std::string a = "/stt/u1/", b = "/stt/u27/";
  const std::vector<std::string> kept = {
      a + "20250101T080000Z-local-00000001000000aa.txt", b + "20250102T090000Z-pi-00000001000000bb.txt",
      a + "20260301T080000Z-local-00000001000000cc.txt", a + "boot-local-00000001000000ff.txt",
      a + "notes/20250101T080000Z-local-00000001000000aa.txt", "/stt/readme.txt"};
  for (const std::string& path : kept) volume.file(path, 900);
  for (int day = 1; day <= 20; ++day) {
    volume.file(sensorFile("", day), 5000);
    volume.file(healthFile("", day), 2000);
  }
  Pass pass{};
  const Limits limits = limitsOn(25);
  unsigned passes = 0;
  while (runPass(pass, volume, limits, "", 100000000, passes == 0).removed > 0) check(++passes < 40);
  check(!volume.has(sensorFile("", 1)) && !volume.has(healthFile("", 1)));  // the rest did go
  for (const std::string& path : kept) check(volume.has(path));
  for (const std::string& gone : volume.removed) check(gone.compare(0, 5, "/stt/") != 0);
  Volume only;
  for (const std::string& path : kept) only.file(path, 900);
  Pass none{};
  check(runPass(none, only, limits, "", 100000000).removed == 0 && only.removalsTried == 0);
}

// The card's tree is the same rule under /sd; a volume without the trees, or
// with nothing that may go, ends at once and removes nothing.
static void otherVolumes() {
  currentCase = "otherVolumes";
  Volume volume;
  twentyDays(volume, "/sd");
  twentyDays(volume);
  volume.file("/sd/collect/aabbccddeeff/files/2026-03-01/sensors.csv", 9000);
  volume.file("/sd/stt/u1/20260301T080000Z-local-00000001000000aa.txt", 700);
  Pass pass{};
  const std::string open = sensorFile("", 1);  // the open file is named by its primary path
  Outcome out = runPass(pass, volume, limitsOn(25, open.c_str()), "/sd", 1000000);
  check(out.removed == 1 && volume.removed[0] == sensorFile("/sd", 2));   // the card's copy of the open file stays
  check(volume.has(sensorFile("/sd", 1)) && std::string(pass.root()) == "/sd");
  for (int day = 1; day <= 20; ++day) check(volume.has(sensorFile("", day)));  // nothing on the flash
  check(volume.has("/sd/collect/aabbccddeeff/files/2026-03-01/sensors.csv"));
  check(volume.has("/sd/stt/u1/20260301T080000Z-local-00000001000000aa.txt"));

  // A session started with a path on the card itself is kept by that path too.
  Volume direct;
  twentyDays(direct, "/sd");
  Pass own{};
  const std::string onCard = sensorFile("/sd", 1);
  direct.file(onCard + ".1", 500);
  out = runPass(own, direct, limitsOn(25, onCard.c_str()), "/sd", 1000000);
  check(out.removed == 1 && direct.removed[0] == sensorFile("/sd", 2));
  check(direct.has(onCard) && direct.has(onCard + ".1"));

  Volume empty;
  Pass none{};
  out = runPass(none, empty, limitsOn(25), "", 1000000);
  check(out.removed == 0 && out.steps <= 6 && empty.removalsTried == 0);
  Volume recent;
  recent.file(sensorFile("", 24), 1000);
  recent.file(sensorFile("", 25), 1000);
  recent.file(healthFile("", 25), 1000);
  out = runPass(none, recent, limitsOn(25), "", 1000000);
  check(out.removed == 0 && recent.removalsTried == 0 && recent.files.size() == 3);
}

// Many stored days and large folders: the work of one step stays bounded and
// the pass still finds the oldest day; days with nothing to remove are not
// walked again by the next pass.
static void boundedWork() {
  currentCase = "boundedWork";
  Volume volume;
  // 120 days of health-only folders (2026-01-01 .. 2026-04-30), then one old
  // day with a sensor log in the middle, and a day with 60 files.
  for (int month = 1; month <= 4; ++month) {
    for (int day = 1; day <= 30; ++day) {
      char name[16];
      std::snprintf(name, sizeof(name), "2026-%02d-%02d", month, day);
      volume.file(kSensors + "/" + name + "/health-" + name + ".csv", 2000);
    }
  }
  volume.file(kSensors + "/2026-02-15/sensors-2026-02-15.csv", 7000);
  for (int part = 0; part < 60; ++part) {
    char name[48];
    std::snprintf(name, sizeof(name), "/2026-03-10/part-%02d.csv", part);
    volume.file(kSensors + name, 100);
  }
  Limits limits = limitsOn(25);
  limits.today = 20260601u;
  Pass pass{};
  Outcome out = runPass(pass, volume, limits, "", 100000000);
  check(out.removed == 1 && volume.removed[0] == kSensors + "/2026-02-15/sensors-2026-02-15.csv");
  const unsigned firstSteps = out.steps;
  // The next pass does not walk through January and February again.
  const unsigned listedBefore = volume.entriesListed;
  out = runPass(pass, volume, limits, "", 100000000, false);
  check(out.removed == 60 && out.steps >= 60);  // one removal a step
  check(volume.entriesListed - listedBefore < 60 * (kEntriesPerStep + 1) + 400);
  check(firstSteps < 200);
  // Nothing secondary is left. A pass that starts without that memory walks
  // through all 120 days (still one bounded step at a time), finds nothing
  // secondary and takes the oldest health file that is no longer protected.
  Volume again = volume;
  Pass fresh{};
  out = runPass(fresh, again, limits, "", 100000000, true);
  check(out.removed == 1 && again.removed.back() == kSensors + "/2026-01-01/health-2026-01-01.csv");
  check(out.steps > 120 && out.steps < 600);
  check(again.has(kSensors + "/2026-04-17/health-2026-04-17.csv"));

  // More day folders than the walk limit: only the first kWalkMaxEntries of
  // the listing are looked at (the volume double asserts that no listing is
  // asked beyond it). The oldest listed day goes; the newest stored days are
  // counted over what was listed, so nothing gets less protection.
  Volume large;
  std::vector<std::string> days;
  for (int year = 2023; year <= 2024; ++year) {
    for (int month = 1; month <= 12; ++month) {
      for (int day = 1; day <= 28 && days.size() < 600; ++day) {
        char name[16];
        std::snprintf(name, sizeof(name), "%04d-%02d-%02d", year, month, day);
        days.emplace_back(name);
      }
    }
  }
  check(days.size() == 600);
  for (const std::string& day : days) large.file(kSensors + "/" + day + "/sensors-" + day + ".csv", 500);
  Pass wide{};
  out = runPass(wide, large, limits, "", 100000000);
  check(out.removed == 1 && large.removed[0] == kSensors + "/" + days[0] + "/sensors-" + days[0] + ".csv");
  check(out.steps <= kWalkMaxEntries / kEntriesPerStep + 8);
  // Days at the end of the listed part count as its newest and keep theirs;
  // nothing beyond the limit is touched while the listed part has work.
  for (unsigned pass = 2; pass <= 12; ++pass) {
    out = runPass(wide, large, limits, "", 100000000, false);
    check(out.removed == 1 && large.removed.back() == kSensors + "/" + days[pass - 1] + "/sensors-" + days[pass - 1] + ".csv");
  }
  for (size_t index = kWalkMaxEntries - kRecentDaysKept; index < days.size(); ++index) {
    check(large.has(kSensors + "/" + days[index] + "/sensors-" + days[index] + ".csv"));
  }
}

int main(int argc, char** argv) {
  struct Case { const char* name; void (*run)(); };
  static const Case cases[] = {
      {"waterMarks", waterMarks}, {"names", names}, {"twentyDayVolume", twentyDayVolume},
      {"limitsAreKept", limitsAreKept}, {"bootFolders", bootFolders}, {"transcriptsKept", transcriptsKept},
      {"otherVolumes", otherVolumes}, {"boundedWork", boundedWork},
  };
  unsigned ran = 0;
  for (const Case& c : cases) {
    if (argc > 1 && std::string(argv[1]) != c.name) continue;
    c.run();
    ++ran;
  }
  if (ran == 0) { std::fprintf(stderr, "no such case\n"); return 2; }
  std::printf("log retention: %u checks passed in %u case(s)\n", checks, ran);
  return 0;
}
