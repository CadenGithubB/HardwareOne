// Drives the production file catch-up ledger store (System_CatchupLedger.cpp,
// the part between CATCHUP_LEDGER_HOST_BEGIN and _END) over the shared fake
// VFS with the real ArduinoJson and the real pure core
// (System_ESPNow_CatchupCore.h). The platform adapters (clock, random, boot
// counter, capture key, PSRAM block, mutex, spinlock, UTC clock) are doubles
// defined here; StagedWrite is a double with the production contract (the
// destination changes only at commit). The runner inserts the header's
// declarations at INSERT_HEADER and the store at INSERT_LEDGER.
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#define FAKE_VFS_LOCK_DOUBLE
#include <Arduino.h>
#include "fake_vfs.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "System_ESPNow_CatchupCore.h"

void webBatchTestStringAppend() {}

#include "catchup_ledger_platform.h"

// ---- production ---------------------------------------------------------------
// INSERT_HEADER

// INSERT_LEDGER

// ---- cases --------------------------------------------------------------------
static unsigned gChecks = 0;
#define CHECK(cond)                                                                             \
  do {                                                                                          \
    ++gChecks;                                                                                  \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } \
  } while (0)

namespace ec = espnowcatchup;

static void resetStore() {
  for (void* p : gBlocks) std::free(p);
  gBlocks.clear();
  std::memset(&sLedger, 0, sizeof(sLedger));
  std::memset(&sRt, 0, sizeof(sRt));
  std::memset(sNotes, 0, sizeof(sNotes));
}
static void reset(bool sd = true) {
  resetStore();
  auto& fs = fake_vfs::fs();
  fs.reset();
  fake_vfs::wireLockDouble();
  fs.sdAvailable() = sd;
  fs.mkdirAll("/system");   // always present on a device
  gNow = 1000; gBoot = 41; gSdGen = 1; gRandom = 0x1a2b3c4d; gKeyReady = true; gAllocFail = false;
  gYields = gYieldsLocked = gAllocs = gNoteEnters = 0; gWarns = 0;
  gNoteHeld = gLedgerHeld = false;
}
// Simulates "a later full LIST": 30 s on, so the reconcile is due.
static bool fullList() { gNow += 31000; return catchupLedgerEnsure(gNow, true); }
static const ec::LedgerEntry* byPath(const char* path) { return ec::findByPath(sLedger, path); }
static uint32_t idOf(const char* path) { const ec::LedgerEntry* e = byPath(path); return e ? e->fileId : 0; }
static std::string bytes(char c, size_t n) { return std::string(n, c); }
static void move(const char* from, const char* to) {
  fake_vfs::Fs::Auth a;
  CHECK(fake_vfs::fs().rename(from, to, a));
}
static std::string ledgerText() { return fake_vfs::fs().text("/system/catchup/ledger.json"); }
static bool parsedLedger(JsonDocument& doc) { return !deserializeJson(doc, ledgerText()); }

// Every guarded call ran under the FS lock, in its kind's scope (battery is
// read unscoped by name, as System_Battery does), as the system identity.
static void checkCallsScoped() {
  for (const auto& c : fake_vfs::fs().log) {
    if (!c.guarded) continue;
    CHECK(c.locked);
    CHECK(c.authUser == "system");
    CHECK(c.authReason.rfind("system:catchup.", 0) == 0);
    if (c.authScope.empty()) {
      CHECK(c.path.rfind("/battery.csv", 0) == 0);
      continue;
    }
    if (!(c.path == c.authScope || c.path.rfind(c.authScope + "/", 0) == 0))
      std::fprintf(stderr, "outside scope: %s %s (scope %s)\n", c.op.c_str(), c.path.c_str(), c.authScope.c_str());
    CHECK(c.path == c.authScope || c.path.rfind(c.authScope + "/", 0) == 0);
  }
}

static void createSaveLoad() {
  reset();
  CHECK(catchupLedgerEnsure(gNow, false));
  JsonDocument doc;
  CHECK(parsedLedger(doc));
  CHECK(doc["schema"].as<int>() == 1);
  CHECK(std::string(doc["ledgerId"].as<const char*>()) == "1a2b3c4d");
  CHECK(std::string(doc["updatedUtc"].as<const char*>()).back() == 'Z');
  CHECK(doc["files"].as<JsonArrayConst>().size() == 0);
  CHECK(!fake_vfs::fs().exists("/system/catchup/ledger.json.upload.tmp"));
  // A second boot loads the same ledger id.
  resetStore();
  gRandom = 0x99;
  CHECK(catchupLedgerEnsure(gNow, false));
  CHECK(sLedger.ledgerId == 0x1a2b3c4d);
  CatchupLedgerStats st;
  catchupLedgerStats(&st);
  CHECK(st.loaded && st.loadResets == 0 && st.reconciles == 1);
  // A broken file starts a new ledger with a new id.
  resetStore();
  fake_vfs::fs().put("/system/catchup/ledger.json", "{not json");
  CHECK(catchupLedgerEnsure(gNow, false));
  CHECK(sLedger.ledgerId == 0x99);
  catchupLedgerStats(&st);
  CHECK(st.loadResets == 1);
  checkCallsScoped();
}

static void systemLogRotationKeepsIds() {
  reset();
  auto& fs = fake_vfs::fs();
  const char* base = "/sd/logging_captures/system/system.log";
  fs.put(base, "A" + bytes('a', 700));
  CHECK(catchupLedgerEnsure(gNow, false));
  const uint32_t a = idOf(base);
  CHECK(a != 0);
  CHECK(byPath(base)->seal == uint8_t(ec::Seal::OnSend));
  const uint32_t gen0 = sLedger.gen;
  // Growth only: the same id, no gen step, no write.
  fs.put(base, "A" + bytes('a', 900));
  const unsigned writesBefore = fs.count("rename");
  CHECK(fullList());
  CHECK(idOf(base) == a && byPath(base)->size == 901 && sLedger.gen == gen0);
  CHECK(fs.count("rename") == writesBefore);
  // log rotations: system.log -> .1, a new system.log.
  move(base, "/sd/logging_captures/system/system.log.1");
  fs.put(base, "B" + bytes('b', 300));
  CHECK(fullList());
  CHECK(idOf("/sd/logging_captures/system/system.log.1") == a);
  const uint32_t b = idOf(base);
  CHECK(b != 0 && b != a && sLedger.gen > gen0);
  // Two more rotations between two walks: .1 -> .3 and base -> .2 keep their ids.
  move("/sd/logging_captures/system/system.log.1", "/sd/logging_captures/system/system.log.3");
  move(base, "/sd/logging_captures/system/system.log.2");
  fs.put(base, "C" + bytes('c', 100));
  CHECK(fullList());
  CHECK(idOf("/sd/logging_captures/system/system.log.3") == a);
  CHECK(idOf("/sd/logging_captures/system/system.log.2") == b);
  CHECK(idOf(base) > b);
  // Survives a reboot.
  const uint32_t ledgerId = sLedger.ledgerId;
  resetStore();
  CHECK(catchupLedgerEnsure(gNow, false));
  CHECK(sLedger.ledgerId == ledgerId && idOf("/sd/logging_captures/system/system.log.3") == a);
  checkCallsScoped();
}

static void sensorRenamesKeepIds() {
  reset();
  auto& fs = fake_vfs::fs();
  const char* bootFile = "/logging_captures/sensors/boot-41/sensors-boot41-12345.csv";
  const std::string header = "timestamp_ms,temp\n";
  fs.put(bootFile, header + "1,20\n");
  // Two boot files with the same head never swap.
  const char* other = "/logging_captures/sensors/boot-41/sensors-boot41-99999.csv";
  fs.put(other, header + "1,20\n");
  CHECK(catchupLedgerEnsure(gNow, false));
  const uint32_t a = idOf(bootFile), o = idOf(other);
  CHECK(a && o && a != o);
  // TimeAnchors promotes the first one into its day folder.
  const char* promoted = "/logging_captures/sensors/2026-11-19/sensors-20261119T120000Z-boot41-000012345.csv";
  fs.mkdirAll("/logging_captures/sensors/2026-11-19");
  move(bootFile, promoted);
  CHECK(fullList());
  CHECK(idOf(promoted) == a && idOf(other) == o && !byPath(bootFile));
  // Sensor rotation (X -> X.1) keeps the id.
  move(promoted, (std::string(promoted) + ".1").c_str());
  fs.put(promoted, header + "9,30\n");
  CHECK(fullList());
  CHECK(idOf((std::string(promoted) + ".1").c_str()) == a);
  CHECK(idOf(promoted) != a && idOf(promoted) != 0);
  // A file replaced by a different one at the same path gets a new id.
  const uint32_t before = idOf(other);
  fs.put(other, "different,header\n1,2\n");
  CHECK(fullList());
  CHECK(idOf(other) != before && idOf(other) != 0);
  checkCallsScoped();
}

static void trimAndRestart() {
  reset();
  auto& fs = fake_vfs::fs();
  const char* events = "/system/sys_logs/events.log";
  std::string text;
  for (int i = 0; i < 40; ++i) text += "line " + std::to_string(i) + " xxxxxxxxxxxxxxxxxxxxxxx\r\n";
  fs.put(events, text);
  CHECK(catchupLedgerEnsure(gNow, false));
  const uint32_t id = idOf(events);
  CHECK(id && byPath(events)->base == 0);
  const uint32_t gen0 = sLedger.gen;
  // appendLineWithCap drops the first 330 bytes and appends.
  const size_t tailStart = 330;
  fs.put(events, text.substr(tailStart) + "new line\r\n");
  const unsigned entersBefore = gNoteEnters;
  catchupNoteTrim("/system/sys_logs/events.log", tailStart);
  CHECK(gNoteEnters == entersBefore + 1 && !gNoteHeld);
  // Pending notes make the next ensure reconcile at once (no 30 s wait).
  gNow += 10;
  CHECK(catchupLedgerEnsure(gNow, false));
  CHECK(idOf(events) == id);
  CHECK(byPath(events)->base == tailStart);
  CHECK(ec::logicalEnd(*byPath(events)) == text.size() + 10);
  CHECK(sLedger.gen == gen0 + 1);
  // Two trims before one reconcile add up.
  const std::string now = fs.text(events);
  fs.put(events, now.substr(100));
  catchupNoteTrim(events, 60);
  catchupNoteTrim(events, 40);
  gNow += 10;
  CHECK(catchupLedgerEnsure(gNow, false));
  CHECK(idOf(events) == id && byPath(events)->base == tailStart + 100);
  // A restart (rotation copy failed) gives a new id; the old one is removed.
  fs.put(events, "[LOG] rotation blocked (low space): 999 older bytes dropped\r\nline\r\n");
  catchupNoteRestart(events);
  gNow += 10;
  CHECK(catchupLedgerEnsure(gNow, false));
  CHECK(idOf(events) != id && idOf(events) != 0 && byPath(events)->base == 0);
  CHECK(ec::findById(sLedger, id) == nullptr);
  // A crash between the trim's rename and its note: the head changed, no
  // note, the file shrank -> a new id, never a wrong base.
  const uint32_t id2 = idOf(events);
  fs.put(events, "shorter\r\n");
  CHECK(fullList());
  CHECK(idOf(events) != id2 && byPath(events)->base == 0);
  // Paths the ledger does not follow return at once.
  const unsigned enters = gNoteEnters;
  catchupNoteTrim("/system/sys_logs/command-audit.log", 50);
  catchupNoteRestart("/system/sys_logs/failed_login.log");
  catchupNoteTrim(nullptr, 5);
  CHECK(gNoteEnters == enters);
  checkCallsScoped();
}

// S-8 (2026-11-20): the capped-log writer batches lines and rotates most logs
// by rename ("<log>.1"). The three logs the ledger follows keep the copy
// rotation in a build that serves catch-up, because the rules above cannot
// follow such a rename; this case holds both halves of that.
static void rotatedFollowedLog() {
  reset();
  auto& fs = fake_vfs::fs();
  const char* errors = "/system/sys_logs/errors.log";
  std::string text;
  for (int i = 0; i < 60; ++i) text += "error " + std::to_string(i) + " yyyyyyyyyyyyyyyyyyyyyy\r\n";
  fs.put(errors, text);
  CHECK(catchupLedgerEnsure(gNow, false));
  const uint32_t id = idOf(errors);
  CHECK(id && byPath(errors)->base == 0);
  // A flushed batch: several lines arrive in one append and the copy rotation
  // that append triggers drops the head. One trim note, as for a single line:
  // the id stays, the base moves, and every byte the collector has not
  // fetched (from the new base on) is still in the file.
  const std::string batch = "batched 1\r\nbatched 2\r\nbatched 3\r\n";
  const size_t tailStart = 480;
  fs.put(errors, (text + batch).substr(tailStart));
  catchupNoteTrim(errors, tailStart);
  gNow += 10;
  CHECK(catchupLedgerEnsure(gNow, false));
  CHECK(idOf(errors) == id && byPath(errors)->base == tailStart);
  CHECK(ec::logicalEnd(*byPath(errors)) == text.size() + batch.size());
  // Batches between two reconciles without a rotation: the file only grew.
  fs.put(errors, fs.text(errors) + batch + batch);
  CHECK(fullList());
  CHECK(idOf(errors) == id && byPath(errors)->base == tailStart);
  CHECK(ec::logicalEnd(*byPath(errors)) == text.size() + 3 * batch.size());
  // What a rename rotation would do to a followed log: the path holds a new
  // file (a new id, base 0) and the renamed generation is not on the
  // whitelist, so the rows the collector had not fetched are out of its
  // reach. This is why these three logs are not two-generation logs here.
  const uint32_t end = ec::logicalEnd(*byPath(errors));
  move(errors, "/system/sys_logs/errors.log.1");
  fs.put(errors, "first line of the new file\r\n");
  CHECK(fullList());
  CHECK(idOf(errors) != id && idOf(errors) != 0 && byPath(errors)->base == 0);
  CHECK(byPath("/system/sys_logs/errors.log.1") == nullptr);
  CHECK(ec::findById(sLedger, id) == nullptr);  // as after a restart: the old id is gone
  CHECK(end > tailStart);
  // The older generation of a two-generation log is never offered either.
  fs.put("/system/sys_logs/system-events.log.1", "older generation\r\n");
  fs.put("/system/sys_logs/command-audit.log.1", "older generation\r\n");
  CHECK(fullList());
  CHECK(byPath("/system/sys_logs/system-events.log.1") == nullptr);
  CHECK(byPath("/system/sys_logs/command-audit.log.1") == nullptr);
  checkCallsScoped();
}

static void cardOutAndBack() {
  reset();
  auto& fs = fake_vfs::fs();
  const char* f = "/sd/logging_captures/system/system.log";
  fs.put(f, "hello\n");
  CHECK(catchupLedgerEnsure(gNow, false));
  const uint32_t id = idOf(f);
  const uint32_t gen0 = sLedger.gen;
  fs.sdAvailable() = false;
  ++gSdGen;
  gNow += 10;
  CHECK(catchupLedgerEnsure(gNow, false));   // a new mount generation reconciles at once
  CHECK(byPath(f) && byPath(f)->state == uint8_t(ec::EntryState::MissingCardOut));
  CHECK(sLedger.gen == gen0);
  ec::Entry list[4];
  uint32_t next = 0;
  uint8_t flags = 0;
  CHECK(ec::collectListEntries(sLedger, 0, 0, true, nullptr, 0, gNow, list, 4, &next, &flags) == 0);
  size_t got = 0;
  uint32_t size = 0;
  uint8_t buf[16];
  CHECK(catchupLedgerRead(id, true, 0, buf, sizeof(buf), &size, &got) == CatchupReadResult::Missing);
  fs.sdAvailable() = true;
  ++gSdGen;
  gNow += 10;
  CHECK(catchupLedgerEnsure(gNow, false));
  CHECK(idOf(f) == id && byPath(f)->state == uint8_t(ec::EntryState::Present));
  checkCallsScoped();
}

static void walkBudgetAndLocking() {
  reset();
  auto& fs = fake_vfs::fs();
  fs.put("/system/sys_logs/errors.log", "e\n");
  fs.put("/logging_captures/system/system.log", "s\n");
  for (int i = 0; i < 70; ++i) {
    char path[96];
    std::snprintf(path, sizeof(path), "/stt/u3/20261119T%06dZ-citrinet-0011223344556677.txt", i);
    fs.put(path, "words\n");
  }
  fs.log.clear();
  CHECK(catchupLedgerEnsure(gNow, false));
  CatchupLedgerStats st;
  catchupLedgerStats(&st);
  // Names are listed in full (each category keeps its own picks); only the
  // picks are opened and read.
  CHECK(!st.lastWalkCut && st.lastWalkEntries >= 70);
  CHECK(st.lastHeadReads <= 9 + 6 + 4 + 4 + 11 + 8);
  CHECK(idOf("/system/sys_logs/errors.log") != 0 && idOf("/logging_captures/system/system.log") != 0);
  unsigned transcripts = 0;
  for (uint8_t i = 0; i < sLedger.count; ++i) transcripts += sLedger.e[i].kind == uint8_t(ec::Kind::Transcript);
  CHECK(transcripts == ec::horizonCap(ec::Kind::Transcript));
  // ... and they are the newest by name, whatever order the folder lists them in.
  for (int i = 70 - (int)ec::horizonCap(ec::Kind::Transcript); i < 70; ++i) {
    char path[96];
    std::snprintf(path, sizeof(path), "/stt/u3/20261119T%06dZ-citrinet-0011223344556677.txt", i);
    CHECK(idOf(path) != 0);
  }
  // The lock is held per folder, never across the walk: released between them.
  CHECK(gYields >= 4 && gYieldsLocked == 0);
  CHECK(gFakeFsSemaphore.takes >= 4 && !gFakeFsSemaphore.held && gFakeFsSemaphore.recursiveTakes == 0);
  CHECK(fs.liveHandles() == 0);
  checkCallsScoped();

  // Old day folders, a full r1 history and old boot folders never crowd out
  // today's health file, the newest r1 history files, the sensor captures
  // or the transcripts (robustness review 2026-11-19).
  reset();
  for (int d = 1; d <= 60; ++d) {
    char path[128];
    std::snprintf(path, sizeof(path), "/sd/logging_captures/sensors/2026-%02d-%02d/env.csv", 1 + d / 28, 1 + d % 28);
    fs.put(path, "timestamp_ms,temp\n1,20\n");
  }
  for (int b = 1; b <= 20; ++b) {
    char path[128];
    std::snprintf(path, sizeof(path), "/sd/logging_captures/sensors/boot-%d/env.csv", b);
    fs.put(path, "timestamp_ms,temp\n1,20\n");
  }
  for (int i = 0; i < 30; ++i) {
    char path[128];
    std::snprintf(path, sizeof(path),
                  "/logging_captures/r1-health-history/0123456789abcdef0123456789abcdef-17635%05d-+0060.r1h", i);
    fs.put(path, "#HW1ENC v1 k1\nENC1:abc\n");
  }
  const char* today = "/sd/logging_captures/sensors/2026-11-19/health-2026-11-19.csv";
  fs.put(today, "#HW1ENC v1 k1\ntimestamp_ms,r1_hr\nENC1:abc\n");
  fs.put("/sd/logging_captures/sensors/2026-11-19/env-2026-11-19.csv", "timestamp_ms,temp\n1,20\n");
  fs.put("/stt/u3/20261119T120000Z-citrinet-0011223344556677.txt", "words\n");
  fs.log.clear();
  CHECK(catchupLedgerEnsure(gNow, false));
  catchupLedgerStats(&st);
  CHECK(!st.lastWalkCut);
  CHECK(idOf(today) != 0 && byPath(today)->kind == uint8_t(ec::Kind::Health));
  CHECK(idOf("/sd/logging_captures/sensors/2026-11-19/env-2026-11-19.csv") != 0);
  CHECK(idOf("/stt/u3/20261119T120000Z-citrinet-0011223344556677.txt") != 0);
  unsigned r1h = 0;
  for (uint8_t i = 0; i < sLedger.count; ++i)
    r1h += ec::detail::endsWith(sLedger.e[i].path, ".r1h") ? 1u : 0u;
  CHECK(r1h == 3);   // the r1 history keeps its own share of the health horizon
  CHECK(idOf("/logging_captures/r1-health-history/0123456789abcdef0123456789abcdef-1763500029-+0060.r1h") != 0);
  CHECK(fs.liveHandles() == 0);
  checkCallsScoped();

  // A folder longer than its listing cap cuts the walk: entries it did not
  // reach are kept, not removed.
  reset();
  fs.put("/stt/u3/00000000T000000Z-first.txt", "words\n");
  CHECK(catchupLedgerEnsure(gNow, false));
  const uint32_t firstId = idOf("/stt/u3/00000000T000000Z-first.txt");
  CHECK(firstId != 0);
  for (int i = 0; i < 400; ++i) {
    char path[96];
    std::snprintf(path, sizeof(path), "/stt/u3/20261119T%06dZ-x.txt", i);
    fs.put(path, "w\n");
  }
  CHECK(fullList());
  catchupLedgerStats(&st);
  CHECK(st.lastWalkCut);
  CHECK(fs.liveHandles() == 0);
  checkCallsScoped();
}

static void whitelistAndSealing() {
  reset();
  auto& fs = fake_vfs::fs();
  for (const char* never : {"/system/sys_logs/command-audit.log", "/system/sys_logs/failed_login.log",
                            "/system/sys_logs/successful_login.log", "/system/sys_logs/system-events.log",
                            "/stt/u3/notes/idea.txt", "/logging_captures/tracks/t.csv", "/g2/x.txt",
                            "/system/espnow/devices.json"})
    fs.put(never, "secret\n");
  fs.put("/logging_captures/sensors/2026-11-19/health-2026-11-19.csv", "time,r1_hr\n1,60\n");
  fs.put("/logging_captures/sensors/2026-11-19/health-2026-11-18.csv", "#HW1ENC v1 k1\ntime,r1_hr\nENC1:abc\n");
  fs.put("/logging_captures/sensors/2026-11-19/gps-2026-11-19.csv", "timestamp_ms,gps_lat,gps_lon\n1,2,3\n");
  fs.put("/logging_captures/sensors/2026-11-19/env-2026-11-19.csv", "timestamp_ms,temp\n1,20\n");
  fs.put("/stt/u3/20261119T120000Z-citrinet-0011223344556677.txt", "spoken words\n");
  fs.put("/battery.csv", "ts,mv\n1,3900\n");
  CHECK(catchupLedgerEnsure(gNow, false));
  for (uint8_t i = 0; i < sLedger.count; ++i) {
    const char* p = sLedger.e[i].path;
    CHECK(std::strstr(p, "audit") == nullptr && std::strstr(p, "login") == nullptr &&
          std::strstr(p, "system-events") == nullptr && std::strstr(p, "/notes/") == nullptr &&
          std::strstr(p, "/g2/") == nullptr && std::strstr(p, "/espnow/") == nullptr &&
          std::strstr(p, "tracks") == nullptr);
  }
  const ec::LedgerEntry* plainHealth = byPath("/logging_captures/sensors/2026-11-19/health-2026-11-19.csv");
  const ec::LedgerEntry* sealedHealth = byPath("/logging_captures/sensors/2026-11-19/health-2026-11-18.csv");
  CHECK(plainHealth && plainHealth->kind == uint8_t(ec::Kind::Health));
  CHECK(ec::entryOffer(*plainHealth, true) == ec::Offer::WithheldUnsealedHealth);
  CHECK(sealedHealth && ec::entryOffer(*sealedHealth, true) == ec::Offer::Serve);
  CHECK(byPath("/logging_captures/sensors/2026-11-19/gps-2026-11-19.csv")->seal == uint8_t(ec::Seal::OnSend));
  CHECK(byPath("/logging_captures/sensors/2026-11-19/env-2026-11-19.csv")->seal == uint8_t(ec::Seal::Plain));
  CHECK(byPath("/stt/u3/20261119T120000Z-citrinet-0011223344556677.txt")->seal == uint8_t(ec::Seal::OnSend));
  CHECK(byPath("/battery.csv") != nullptr);
  // No capture key: sealed-on-send files are withheld, never offered plain.
  gKeyReady = false;
  CHECK(fullList());
  CHECK(!catchupLedgerKeyReady());
  ec::Entry list[16];
  uint32_t next = 0;
  uint8_t flags = 0;
  const size_t n = ec::collectListEntries(sLedger, 0, 0, catchupLedgerKeyReady(), nullptr, 0, gNow, list, 16, &next, &flags);
  for (size_t i = 0; i < n; ++i) CHECK(!(list[i].flags & ec::kEfSealedOnSend));
  CHECK(flags & ec::kLfWithheld);
  // A hand-edited ledger cannot unseal a system log or a transcript.
  gKeyReady = true;
  resetStore();
  JsonDocument doc;
  CHECK(parsedLedger(doc));
  for (JsonObject o : doc["files"].as<JsonArray>()) o["seal"] = 0;
  JsonObject bad = doc["files"].as<JsonArray>().add<JsonObject>();
  bad["id"] = 9999; bad["kind"] = 2; bad["path"] = "/system/sys_logs/command-audit.log";
  bad["base"] = 0; bad["headLen"] = 0; bad["headCrc"] = "00000000"; bad["size"] = 1;
  std::string edited;
  serializeJson(doc, edited);
  fs.put("/system/catchup/ledger.json", edited);
  CHECK(catchupLedgerEnsure(gNow, false));
  CHECK(byPath("/stt/u3/20261119T120000Z-citrinet-0011223344556677.txt")->seal == uint8_t(ec::Seal::OnSend));
  CHECK(byPath("/system/sys_logs/command-audit.log") == nullptr);
  CHECK(gWarns >= 1);
  checkCallsScoped();
}

static void deliveredIsLazy() {
  reset();
  auto& fs = fake_vfs::fs();
  const char* f = "/system/sys_logs/errors.log";
  fs.put(f, bytes('e', 1000));
  CHECK(catchupLedgerEnsure(gNow, false));
  const uint32_t id = idOf(f);
  const std::string saved = ledgerText();
  CHECK(catchupLedgerNoteDelivered(id, 600));
  CHECK(byPath(f)->delivered == 600);
  CHECK(catchupLedgerNoteDelivered(id, 100));   // never lowers
  CHECK(byPath(f)->delivered == 600);
  CHECK(catchupLedgerNoteDelivered(id, 5000));  // clamped to the end
  CHECK(byPath(f)->delivered == 1000);
  CHECK(!catchupLedgerNoteDelivered(424242, 1));
  gNow += 60000;
  CHECK(catchupLedgerEnsure(gNow, false));
  CHECK(ledgerText() == saved);                // not yet: lazy
  gNow += 5 * 60 * 1000;
  CHECK(catchupLedgerEnsure(gNow, false));
  JsonDocument doc;
  CHECK(parsedLedger(doc));
  CHECK(doc["files"][0]["delivered"].as<uint32_t>() == 1000);
  // A flush (reboot intent) saves a pending change at once.
  fs.put(f, bytes('e', 2000));
  CHECK(fullList());
  CHECK(catchupLedgerNoteDelivered(id, 1500));
  catchupLedgerFlush();
  JsonDocument doc2;
  CHECK(parsedLedger(doc2));
  CHECK(doc2["files"][0]["delivered"].as<uint32_t>() == 1500);
  uint32_t lid = 0, gen = 0, sum = 0;
  catchupLedgerSummary(&lid, &gen, &sum);
  CHECK(lid == sLedger.ledgerId && gen == sLedger.gen && sum == 2000);
  checkCallsScoped();
}

static void reads() {
  reset();
  auto& fs = fake_vfs::fs();
  const char* f = "/system/sys_logs/errors.log";
  std::string text;
  for (int i = 0; i < 100; ++i) text += char('A' + i % 26);
  fs.put(f, text);
  CHECK(catchupLedgerEnsure(gNow, false));
  const uint32_t id = idOf(f);
  uint8_t buf[64];
  size_t got = 0;
  uint32_t size = 0;
  CHECK(catchupLedgerRead(id, true, 10, buf, 20, &size, &got) == CatchupReadResult::Ok);
  CHECK(size == 100 && got == 20 && std::memcmp(buf, text.data() + 10, 20) == 0);
  CHECK(catchupLedgerRead(id, false, 90, buf, 64, &size, &got) == CatchupReadResult::Ok && got == 10);
  CHECK(catchupLedgerRead(id, false, 100, buf, 64, &size, &got) == CatchupReadResult::Ok && got == 0);
  CHECK(catchupLedgerRead(id, false, 101, buf, 64, &size, &got) == CatchupReadResult::IoError);
  // Growth is seen and taken into the ledger.
  fs.put(f, text + "more");
  CHECK(catchupLedgerRead(id, true, 0, nullptr, 0, &size, &got) == CatchupReadResult::Ok && size == 104);
  catchupLedgerNoteSize(id, size, gNow);
  CHECK(byPath(f)->size == 104);
  // Replaced (same size or larger, another head), shrunk, missing.
  fs.put(f, "Z" + text.substr(1) + "more");
  CHECK(catchupLedgerRead(id, true, 0, buf, 8, &size, &got) == CatchupReadResult::Replaced);
  fs.put(f, "short");
  CHECK(catchupLedgerRead(id, true, 0, buf, 8, &size, &got) == CatchupReadResult::Shrunk);
  fake_vfs::Fs::Auth a;
  CHECK(fs.remove(f, a));
  CHECK(catchupLedgerRead(id, true, 0, buf, 8, &size, &got) == CatchupReadResult::Missing);
  CHECK(catchupLedgerRead(77777, true, 0, buf, 8, &size, &got) == CatchupReadResult::Missing);
  CHECK(fs.liveHandles() == 0 && !gFakeFsSemaphore.held);
  checkCallsScoped();
}

static void noScratchNoLedger() {
  reset();
  gAllocFail = true;
  CHECK(!catchupLedgerEnsure(gNow, false));
  gAllocFail = false;
  CHECK(catchupLedgerEnsure(gNow, false));
}

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  struct Case { const char* name; void (*fn)(); } cases[] = {
    {"create_save_load", createSaveLoad},
    {"system_log_rotation", systemLogRotationKeepsIds},
    {"sensor_renames", sensorRenamesKeepIds},
    {"trim_and_restart", trimAndRestart},
    {"rotated_followed_log", rotatedFollowedLog},
    {"card_out_and_back", cardOutAndBack},
    {"walk_budget_and_locking", walkBudgetAndLocking},
    {"whitelist_and_sealing", whitelistAndSealing},
    {"delivered_is_lazy", deliveredIsLazy},
    {"reads", reads},
    {"no_scratch", noScratchNoLedger},
  };
  unsigned ran = 0;
  for (const Case& c : cases) {
    if (!only.empty() && only != c.name) continue;
    c.fn();
    ++ran;
    std::printf("catchup_ledger %s: ok\n", c.name);
  }
  resetStore();
  std::printf("catchup ledger: %u cases, %u checks passed\n", ran, gChecks);
  return ran ? 0 : 1;
}
