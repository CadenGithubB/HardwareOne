// Behavioural harness for the production QuickNote service and `note` CLI
// (System_QuickNote.cpp). test_quicknote_service.py splices the real header
// types, QuickNote/transcript policies, the CommandArgs parser, the dictation
// lease types and the whole service + CLI at the INSERT_* markers; everything
// below is a fake of the platform, dictation, auth, the paired wearer and the
// note store. It is compiled once per transcription backend (ENABLE_LOCAL_STT
// 1 = on-device, 0 = Pi/CM5).
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef ENABLE_LOCAL_STT
#error "test_quicknote_service.py defines ENABLE_LOCAL_STT"
#endif
#define ENABLE_QUICKNOTE 1
#define DICTATION_MAX_TEXT 256
#define EXT_RAM_BSS_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (0)
static constexpr uint32_t MIC_STT_MAX_CAPTURE_MS = 30000u;
void webBatchTestStringAppend() {}

static int failures = 0;
#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                               \
    }                                                                           \
  } while (0)

// ---- platform ----------------------------------------------------------------
struct portMUX_TYPE { int unused; };
#define portMUX_INITIALIZER_UNLOCKED {0}
static int gDepth = 0;
static void portENTER_CRITICAL(portMUX_TYPE*) { CHECK(gDepth == 0); ++gDepth; }
static void portEXIT_CRITICAL(portMUX_TYPE*) { CHECK(gDepth == 1); --gDepth; }
static uint32_t gNow = 100000;
static uint32_t millis() { return gNow; }
using TaskHandle_t = void*;
using BaseType_t = int;
using UBaseType_t = unsigned;
using TaskFunction_t = void (*)(void*);
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) (ms)
static constexpr UBaseType_t TASK_PRIORITY_LOW = 1;
static constexpr BaseType_t PRO_CORE = 0;
static int gTaskToken = 0;
static unsigned tasksCreated = 0, notifies = 0;
static std::string taskName;
static uint32_t taskStack = 0;
static BaseType_t xTaskCreateLogged(TaskFunction_t, const char* name, uint32_t stack, void*,
                                    UBaseType_t, TaskHandle_t* out, const char*, BaseType_t) {
  ++tasksCreated;
  taskName = name;
  taskStack = stack;
  *out = &gTaskToken;
  return pdPASS;
}
static uint32_t ulTaskNotifyTake(int, uint32_t) { return 0; }
static void xTaskNotifyGive(TaskHandle_t task) { CHECK(task == &gTaskToken); CHECK(gDepth == 0); ++notifies; }
static uint32_t gRandom = 0x1000;
static uint32_t esp_random() { return ++gRandom; }

static std::vector<std::string> logs;
static void fakeLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void fakeLog(const char* fmt, ...) {
  CHECK(gDepth == 0);
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  logs.push_back(line);
}
#define INFO_SYSTEMF(fmt, ...) fakeLog(fmt, ##__VA_ARGS__)
#define WARN_SYSTEMF(fmt, ...) fakeLog(fmt, ##__VA_ARGS__)

// ---- users, sessions, events ---------------------------------------------------
using TransportSessionEpoch = uint32_t;
constexpr TransportSessionEpoch kNoTransportSessionEpoch = 0;
enum CommandSource {
  SOURCE_WEB = 0, SOURCE_SERIAL = 1, SOURCE_INTERNAL = 2, SOURCE_ESPNOW = 3,
  SOURCE_LOCAL_DISPLAY = 4, SOURCE_BLUETOOTH = 5, SOURCE_MQTT = 6, SOURCE_VOICE = 7,
  SOURCE_G2_GLASSES = 8, SOURCE_UART = 9,
};
struct AuthContext {
  CommandSource transport = SOURCE_WEB;
  String path, ip, user, sid;
  void* opaque = nullptr;
  String scope;
};
static AuthContext auth;
static uint32_t callerEpoch = 7;
static std::set<std::pair<int, uint32_t>> liveEpochs;
static const AuthContext& currentAuthContext() { return auth; }
static uint32_t captureTransportSessionEpoch(const AuthContext&) { return callerEpoch; }
static bool transportSessionEpochIsLive(CommandSource source, TransportSessionEpoch epoch) {
  return epoch && liveEpochs.count({source, epoch});
}
static std::map<std::string, std::string> roles = {
    {"alice", "user"}, {"bob", "user"}, {"carol", "admin"}, {"gina", "guest"}};
static bool getUserAuthorizationRole(const String& user, String& role) {
  const auto found = roles.find(user.c_str());
  if (found == roles.end()) return false;
  role = found->second.c_str();
  return true;
}
static bool currentExecIsAdmin() {
  const auto found = roles.find(auth.user.c_str());
  return found != roles.end() && found->second == "admin";
}
static uint8_t transportToNotifSource(CommandSource source) { return static_cast<uint8_t>(100 + source); }
static void secureClearString(String& text) { text = ""; }

enum BlePeerKind { BLE_PEER_G2_GLASSES = 1 };
// INSERT_BLE_OWNER
static struct { std::string user = "alice"; uint32_t epoch = 9; bool paired = true; } wearer;
static bool blePeerOwnerSessionSnapshot(BlePeerKind kind, BlePeerOwnerSession& out) {
  CHECK(kind == BLE_PEER_G2_GLASSES);
  if (!wearer.paired) return false;
  out.user = wearer.user.c_str();
  out.generation = 1;
  out.transportEpoch = wearer.epoch;
  return true;
}

enum : uint8_t { SYSEVT_NOTE_STARTED = 162, SYSEVT_NOTE_SAVED = 163, SYSEVT_NOTE_FAILED = 164 };
struct PostedEvent { uint8_t kind; std::string subject, detail; uint8_t source; std::string who; };
static std::vector<PostedEvent> events;
static void systemEventPost(uint8_t kind, const char* subject = nullptr, const char* detail = nullptr,
                            uint8_t source = 0xFF, const char* who = nullptr) {
  CHECK(gDepth == 0);
  events.push_back({kind, subject ? subject : "", detail ? detail : "", source, who ? who : ""});
}

// ---- audio, glasses, settings ---------------------------------------------------
enum AudioSource : uint8_t { AUDIO_SRC_NONE = 0, AUDIO_SRC_LOCAL_PDM = 1, AUDIO_SRC_G2_LEFT = 2 };
static bool g2MicAvailable = true, pdmAvailable = true;
static bool audioSourceAvailable(AudioSource source) {
  return source == AUDIO_SRC_G2_LEFT ? g2MicAvailable : source == AUDIO_SRC_LOCAL_PDM && pdmAvailable;
}
static bool nativeVoice = false;
static bool g2NativeVoiceActive() { CHECK(gDepth == 0); return nativeVoice; }
struct FakeSettings { uint16_t noteMaxSec = 60; uint8_t noteMic = 0; };
static FakeSettings gSettings;
// Before clockSet the clock is unset (every note is stamped "boot"); after
// it, uptime 0 of this boot (#76) was 15:00:00Z.
static bool clockSet = false;
namespace Clock {
static int tzOffset = 0;  // UTC
static int tzOffsetMinutes() { return tzOffset; }
static bool isSynced() { return clockSet; }
}  // namespace Clock

// ---- shared types and policies (production) ---------------------------------------
// INSERT_TYPES
// INSERT_POLICIES
// INSERT_PARSER
struct CommandEntry {
  const char* name;
  const char* help;
  bool requiresAdmin;
  const char* (*handler)(const String&);
  const char* usage;
};

// ---- dictation (one App lease) -------------------------------------------------------
static struct FakeDictation {
  bool available = true;
  const char* unavailable = "no mic";
  DictationState global = DictationState::IDLE;
  bool allowBegin = true, haveLease = false;
  DictationAppLease lease;
  DictationAppOptions options;
  unsigned begins = 0, stops = 0, cancels = 0, commits = 0;
  bool captureActive = false, preparing = false, done = false;
  int level = 0;
  DictationState state = DictationState::RECORDING;
  std::string failure;
  std::deque<std::pair<uint32_t, std::string>> pieces;
  size_t offset = 0;
  void (*onBegin)() = nullptr;  // runs inside dictationAppBeginWith (a worker pass mid-start)
} dict;
static bool owns(const DictationAppLease& l) {
  return dict.haveLease && l.exchange && l.source == dict.lease.source &&
         l.epoch == dict.lease.epoch && l.exchange == dict.lease.exchange &&
         l.purpose == dict.lease.purpose && transportSessionEpochIsLive(l.source, l.epoch);
}
static bool dictationAvailable(const char** why) {
  if (why) *why = dict.available ? nullptr : dict.unavailable;
  return dict.available;
}
static DictationSnapshot dictationSnapshotNow() {
  DictationSnapshot out{};
  out.state = dict.global;
  return out;
}
static bool dictationAppBeginWith(CommandSource source, TransportSessionEpoch epoch,
                                  const DictationAppOptions& options, DictationAppLease* lease) {
  CHECK(gDepth == 0);
  ++dict.begins;
  dict.options = options;
  *lease = DictationAppLease{};
  if (dict.onBegin) dict.onBegin();
  if (!dict.allowBegin) return false;
  dict.lease = DictationAppLease{source, epoch, 0x1122334455667788ULL, options.purpose};
  dict.haveLease = true;
  dict.done = false;
  *lease = dict.lease;
  return true;
}
static bool dictationAppSnapshot(const DictationAppLease& lease, DictationAppSnapshot* out) {
  CHECK(gDepth == 0);
  *out = DictationAppSnapshot{};
  out->busy = dict.haveLease && !(dict.done && dict.pieces.empty());
  if (!owns(lease)) return false;
  out->valid = true;
  out->textPending = !dict.pieces.empty();
  out->done = dict.done && dict.pieces.empty();
  out->active = !out->done;
  out->status.state = dict.state;
  out->status.captureActive = dict.captureActive;
  out->status.preparing = dict.preparing;
  out->status.level = dict.level;
  std::snprintf(out->status.failure, sizeof(out->status.failure), "%s", dict.failure.c_str());
  return true;
}
static bool dictationAppPeekText(const DictationAppLease& lease, char* out, size_t size,
                                 DictationTextReceipt* receipt) {
  CHECK(gDepth == 0);
  out[0] = '\0';
  *receipt = DictationTextReceipt{};
  if (!owns(lease) || dict.pieces.empty()) return false;
  const std::string rest = dict.pieces.front().second.substr(dict.offset);
  const size_t count = std::min(rest.size(), size - 1);
  std::memcpy(out, rest.data(), count);
  out[count] = '\0';
  *receipt = DictationTextReceipt{lease.exchange, dict.pieces.front().first,
                                  static_cast<uint16_t>(dict.offset), static_cast<uint16_t>(count)};
  return count != 0;
}
static bool dictationAppCommitText(const DictationAppLease& lease, const DictationTextReceipt& receipt,
                                   size_t accepted) {
  ++dict.commits;
  if (!owns(lease) || dict.pieces.empty() || receipt.sequence != dict.pieces.front().first ||
      receipt.offset != dict.offset || accepted != receipt.length) return false;
  dict.offset += accepted;
  if (dict.offset == dict.pieces.front().second.size()) {
    dict.pieces.pop_front();
    dict.offset = 0;
  }
  return true;
}
static bool dictationAppRequestStop(const DictationAppLease& lease) {
  CHECK(gDepth == 0);
  ++dict.stops;
  return owns(lease);
}
static bool dictationAppCancel(const DictationAppLease& lease) {
  CHECK(gDepth == 0);
  ++dict.cancels;
  const bool mine = owns(lease);
  if (mine) { dict.done = true; dict.pieces.clear(); }
  return mine;
}

// The named user of a live source+epoch, as System_Transcript resolves it.
static bool transcriptCaptureIdentity(CommandSource source, TransportSessionEpoch epoch, char* out,
                                      size_t cap) {
  std::string user;
  if (source == SOURCE_G2_GLASSES) {
    if (wearer.paired && wearer.epoch == epoch) user = wearer.user;
  } else if (source == auth.transport && epoch == callerEpoch) {
    user = auth.user.c_str();
  }
  const bool ok = !user.empty() && user.size() < cap && transportSessionEpochIsLive(source, epoch);
  if (ok) std::memcpy(out, user.c_str(), user.size() + 1);
  else out[0] = '\0';
  return ok;
}

// ---- local STT keep-warm ------------------------------------------------------------
static std::vector<std::pair<uint32_t, bool>> warmCalls;
static void sttKeepWarm(uint32_t holdMs, bool load) {
  CHECK(gDepth == 0);
  warmCalls.push_back({holdMs, load});
}

// ---- note store ------------------------------------------------------------------
// INSERT_STORE_API
struct Created {
  CommandSource source; uint32_t epoch; std::string user, text; QuickNotePolicy::NoteHeader header;
};
static std::vector<Created> creates;
static bool roomOk = true, createFails = false, listOk = true, sdWritable = false;
namespace VFS {
bool isSDWritable() { return sdWritable; }  // the tier a save would use now
}  // namespace VFS
static std::string listUser, readPath, removedPath;
static uint32_t listOwner = 0;
static bool listSd = false;
// This is boot #76; gUptimeS seconds since it (clockSet: see Clock).
static uint32_t gUptimeS = 1000;
struct Restamped { CommandSource source; std::string user, path, stamp; };
static std::vector<Restamped> restamps;
static int restampFailures = 0;   // the next N restamps fail...
static bool restampGone = false;  // ...for good (else a retry may succeed)
namespace QuickNoteStore {
void stamp(char* out, size_t cap) { std::snprintf(out, cap, "boot"); }
void bootClock(uint32_t* boot, uint32_t* uptimeS) {
  if (boot) *boot = 76;
  if (uptimeS) *uptimeS = gUptimeS;
}
bool stampSince(uint32_t uptimeS, char* out, size_t cap) {
  CHECK(gDepth == 0);
  if (!clockSet) { out[0] = '\0'; return false; }
  std::snprintf(out, cap, "20261004T%02u%02u%02uZ", unsigned(15 + uptimeS / 3600),
                unsigned(uptimeS / 60 % 60), unsigned(uptimeS % 60));
  return true;
}
bool restamp(CommandSource source, const char* user, const char* path, const char* stamp,
             char* pathOut, size_t pathCap, bool* retry, const char** why) {
  CHECK(gDepth == 0);
  restamps.push_back({source, user, path, stamp});
  if (pathOut && pathCap) pathOut[0] = '\0';
  if (retry) *retry = false;
  if (why) *why = nullptr;
  if (!restampFailures) return true;
  --restampFailures;
  if (retry) *retry = !restampGone;
  if (why) *why = restampGone ? "Note unavailable" : "Note storage busy";
  return false;
}
bool hasRoom(const char** why) {
  CHECK(gDepth == 0);
  if (why) *why = roomOk ? nullptr : "Note storage full or unavailable";
  return roomOk;
}
bool create(CommandSource source, TransportSessionEpoch epoch, const char* user,
            QuickNotePolicy::NoteHeader& header, const char* text, size_t length, char* pathOut,
            size_t pathCap, const char** why) {
  CHECK(gDepth == 0);
  if (why) *why = nullptr;
  creates.push_back({source, epoch, user, std::string(text, length), header});
  if (createFails) { if (why) *why = "Note storage full or unavailable"; return false; }
  stamp(header.created, sizeof(header.created));
  bootClock(&header.boot, &header.uptimeS);
  header.words = QuickNotePolicy::noteWordCount(text, length);
  QuickNotePolicy::noteTitle(text, length, header.title, sizeof(header.title));
  char name[48];
  QuickNotePolicy::noteFileName(header.created, header.id, false, name, sizeof(name));
  std::snprintf(pathOut, pathCap, "/stt/u7/notes/%s", name);
  return true;
}
bool list(const AuthContext& ctx, uint32_t owner, bool sd, uint32_t offset, Page* out, const char** why) {
  listUser = ctx.user.c_str();
  listOwner = owner;
  listSd = sd;
  if (!listOk) { if (why) *why = "Notes unavailable"; return false; }
  out->count = 4;
  out->total = 9;
  out->nextOffset = offset + 4;
  out->more = true;
  out->capped = false;
  for (size_t i = 0; i < 4; ++i) {
    Row& row = out->rows[i];
    row.header = QuickNotePolicy::NoteHeader{};
    row.header.id = 0x10 + i;
    std::snprintf(row.header.created, sizeof(row.header.created), "20261004T15301%uZ", unsigned(i));
    std::snprintf(row.path, sizeof(row.path), "/stt/u7/notes/%s-%016llx.txt", row.header.created,
                  static_cast<unsigned long long>(row.header.id));
    row.headerValid = i == 0;
    std::snprintf(row.header.title, sizeof(row.header.title), "Private title %u", unsigned(i));
    row.header.words = 4;
    row.header.trigger = QuickNotePolicy::Trigger::Hold;
    row.header.mic = QuickNotePolicy::Mic::G2;
    row.header.durationMs = 3000;
  }
  // Two notes saved before the clock was set: 5 minutes ago this boot, and
  // in boot #75.
  for (size_t i = 2; i < 4; ++i) {
    Row& row = out->rows[i];
    std::snprintf(row.header.created, sizeof(row.header.created), "boot");
    std::snprintf(row.path, sizeof(row.path), "/stt/u7/notes/boot-%016llx.txt",
                  static_cast<unsigned long long>(row.header.id));
    row.headerValid = true;
    row.header.boot = i == 2 ? 76 : 75;
    row.header.uptimeS = gUptimeS - 300;
  }
  return true;
}
bool read(const AuthContext&, const char* path, uint32_t offset, char* out, size_t cap,
          size_t* got, uint32_t* total, const char** why) {
  readPath = path;
  CHECK(cap >= kReadWindow + 1);
  if (std::strcmp(path, "/stt/u7/notes/boot-0000000000000011.txt")) {
    if (why) *why = "Note unavailable";
    return false;
  }
  const char* text = "secret window words";
  const size_t length = std::strlen(text) - std::min<size_t>(offset, std::strlen(text));
  std::memcpy(out, text + (std::strlen(text) - length), length + 1);
  *got = length;
  *total = static_cast<uint32_t>(std::strlen(text));
  return true;
}
bool remove(const AuthContext&, const char* path, const char** why) {
  removedPath = path;
  if (why) *why = nullptr;
  return true;
}
}  // namespace QuickNoteStore

// ---- production -----------------------------------------------------------------
// INSERT_PRODUCTION

// ---- tests ------------------------------------------------------------------------
static std::string lastReply;
static JsonDocument note(const char* line) {
  const char* out = commandNote(String(line));
  CHECK(gDepth == 0);
  lastReply = out;
  JsonDocument doc;
  CHECK(!deserializeJson(doc, out));
  CHECK(doc["success"].is<bool>());
  // Every reply is tagged private: shared mirrors and logs redact it whole.
  CHECK(!doc["transcriptPath"].isNull());
  return doc;
}
static bool ok(const JsonDocument& doc) { return doc["success"] == true; }
static bool says(const JsonDocument& doc, const char* text) {
  return doc["error"].as<std::string>().find(text) != std::string::npos;
}
static void caller(CommandSource source, const char* user, uint32_t epoch) {
  auth = AuthContext{};
  auth.transport = source;
  auth.user = user;
  callerEpoch = epoch;
}
static void reset() {
  CHECK(gDepth == 0);
  gNote = Note{};
  gLast = Last{};
  wipeText();
  for (auto& u : gUnstamped) u = Unstamped{};
  gUnstampedCount = 0;
  gRestampDue = false;
  restamps.clear();
  restampFailures = 0;
  restampGone = clockSet = false;
  warmCalls.clear();
  dict = FakeDictation{};
  events.clear();
  logs.clear();
  creates.clear();
  roomOk = listOk = true;
  createFails = sdWritable = false;
  listUser = readPath = removedPath = "";
  liveEpochs = {{SOURCE_WEB, 7}, {SOURCE_G2_GLASSES, 9}, {SOURCE_LOCAL_DISPLAY, 5},
                {SOURCE_SERIAL, 3}, {SOURCE_BLUETOOTH, 4}, {SOURCE_UART, 6}};
  wearer.user = "alice";
  wearer.epoch = 9;
  wearer.paired = true;
  g2MicAvailable = pdmAvailable = true;
  nativeVoice = false;
  gSettings = FakeSettings{};
  gNow += 10000;
  caller(SOURCE_WEB, "alice", 7);
}
static uint64_t idOf(const JsonDocument& doc) {
  uint64_t id = 0;
  const std::string text = doc["id"].as<std::string>();
  CHECK(QuickNotePolicy::noteParseId(text.c_str(), text.size(), &id));
  return id;
}
static std::string hex(uint64_t id) {
  char text[17];
  formatId(id, text);
  return text;
}
static JsonDocument noteFor(const char* verb, uint64_t id) {
  return note((std::string(verb) + " " + hex(id)).c_str());
}
static bool anywhere(const char* word) {
  for (const auto& line : logs) if (line.find(word) != std::string::npos) return true;
  for (const auto& e : events)
    if (e.subject.find(word) != std::string::npos || e.detail.find(word) != std::string::npos) return true;
  return false;
}
static bool textWiped() {
  if (gQnTextLen) return false;
  for (char c : gQnText) if (c) return false;
  return true;
}
// One worker pass with the dictation reporting audio.
static void capture() { dict.captureActive = true; servicePass(); }

static void testAuthority() {
  // Web cookie session: admitted; a non-wearer's Auto mic is the board mic.
  reset();
  caller(SOURCE_WEB, "bob", 7);
  auto d = note("start");
  CHECK(ok(d) && d["phase"] == "starting" && d["trigger"] == "web" && d["mic"] == "pdm");
  CHECK(dict.begins == 1 && dict.lease.source == SOURCE_WEB && dict.lease.epoch == 7);
  CHECK(dict.options.purpose == DictationAppPurpose::QuickNote);
  CHECK(!dict.options.saveTranscript);  // no /stt/u<id>/*.txt duplicate
  CHECK(dict.options.preferredSource == AUDIO_SRC_LOCAL_PDM && dict.options.silenceStopMs == 0);
  CHECK(tasksCreated == 1 && taskName == "qnote" && taskStack == 5120 && notifies > 0);
  CHECK(events.size() == 1 && events[0].kind == SYSEVT_NOTE_STARTED && events[0].subject == "web" &&
        events[0].detail == (ENABLE_LOCAL_STT ? "local" : "pi"));
  // The glasses mic is the paired wearer's alone.
  reset(); caller(SOURCE_WEB, "bob", 7);
  d = note("start mic=g2");
  CHECK(!ok(d) && says(d, "paired wearer") && dict.begins == 0);
  reset(); caller(SOURCE_WEB, "bob", 7); gSettings.noteMic = 1;
  CHECK(!ok(note("start")) && dict.begins == 0);
  reset();  // alice is the wearer: Auto picks her glasses
  d = note("start");
  CHECK(ok(d) && d["mic"] == "g2" && dict.options.preferredSource == AUDIO_SRC_G2_LEFT);
  reset(); g2MicAvailable = false;
  CHECK(ok(note("start")) && dict.options.preferredSource == AUDIO_SRC_LOCAL_PDM);
  reset();
  d = note("start mic=pdm toggle");
  CHECK(ok(d) && d["mic"] == "pdm");
  reset(); gSettings.noteMic = 2;
  CHECK(ok(note("start")) && dict.options.preferredSource == AUDIO_SRC_LOCAL_PDM);
  // Local display: admitted as a CLI note.
  reset(); caller(SOURCE_LOCAL_DISPLAY, "bob", 5);
  d = note("start");
  CHECK(ok(d) && d["trigger"] == "cli" && dict.lease.source == SOURCE_LOCAL_DISPLAY);
  // The lens (paired wearer's G2 session): glasses mic; hold ends on release.
  reset(); caller(SOURCE_G2_GLASSES, "alice", 9);
  d = note("start");
  CHECK(ok(d) && d["trigger"] == "lens" && d["mic"] == "g2" && dict.lease.source == SOURCE_G2_GLASSES &&
        dict.lease.epoch == 9 && dict.options.silenceStopMs == 0);
  reset(); caller(SOURCE_G2_GLASSES, "alice", 9);
  d = note("start hold");
  CHECK(ok(d) && d["trigger"] == "hold" && d["hold"] == true && dict.options.silenceStopMs == 6000);
  reset(); caller(SOURCE_G2_GLASSES, "alice", 9);
  CHECK(!ok(note("start mic=pdm")) && dict.begins == 0);
  // Serial, BLE app and UART: only `note start glasses`.
  for (auto source : {SOURCE_SERIAL, SOURCE_BLUETOOTH, SOURCE_UART}) {
    reset();
    caller(source, "alice", source == SOURCE_SERIAL ? 3 : source == SOURCE_BLUETOOTH ? 4 : 6);
    d = note("start");
    CHECK(!ok(d) && says(d, "From this console use: note start glasses") && dict.begins == 0);
    reset();
    caller(source, "alice", source == SOURCE_SERIAL ? 3 : source == SOURCE_BLUETOOTH ? 4 : 6);
    d = note("start glasses");
    CHECK(ok(d) && d["trigger"] == "cli" && d["mic"] == "g2");
    // The lease is the wearer's G2 session, not the console's.
    CHECK(dict.lease.source == SOURCE_G2_GLASSES && dict.lease.epoch == 9 &&
          dict.options.preferredSource == AUDIO_SRC_G2_LEFT && !dict.options.saveTranscript);
  }
  reset(); caller(SOURCE_SERIAL, "alice", 3);
  d = note("start glasses hold");
  CHECK(ok(d) && d["trigger"] == "hold" && dict.options.silenceStopMs == 6000);
  reset(); caller(SOURCE_SERIAL, "alice", 3);
  CHECK(!ok(note("start glasses mic=pdm")) && dict.begins == 0);
  // Not the wearer: refused, and an admin gets no override.
  reset(); caller(SOURCE_SERIAL, "bob", 3);
  d = note("start glasses");
  CHECK(!ok(d) && says(d, "paired wearer") && dict.begins == 0);
  reset(); caller(SOURCE_SERIAL, "carol", 3);
  CHECK(currentExecIsAdmin());
  d = note("start glasses");
  CHECK(!ok(d) && says(d, "paired wearer") && dict.begins == 0);
  reset(); caller(SOURCE_SERIAL, "alice", 3); wearer.paired = false;
  CHECK(!ok(note("start glasses")) && dict.begins == 0);
  // Guests never record.
  reset(); caller(SOURCE_WEB, "gina", 7);
  d = note("start");
  CHECK(!ok(d) && says(d, "non-guest") && dict.begins == 0);
  reset(); caller(SOURCE_SERIAL, "gina", 3); wearer.user = "gina";
  CHECK(!ok(note("start glasses")) && dict.begins == 0);
  // Automations, ESP-NOW, MQTT and voice: refused even with a (fake) live
  // epoch, by name; with no epoch (production) they never get past sign-in.
  for (auto source : {SOURCE_INTERNAL, SOURCE_ESPNOW, SOURCE_MQTT, SOURCE_VOICE}) {
    reset(); liveEpochs.insert({source, 11}); caller(source, "alice", 11);
    d = note("start glasses");
    CHECK(!ok(d) && says(d, "automations") && dict.begins == 0);
    reset(); liveEpochs.insert({source, 11}); caller(source, "alice", 11);
    CHECK(!ok(note("start")) && dict.begins == 0);
    reset(); caller(source, "alice", 0);
    d = note("start glasses");
    CHECK(!ok(d) && says(d, "Sign in") && dict.begins == 0);
  }
  // Bad grammar.
  for (const char* line : {"start hold toggle", "start hold hold", "start mic=usb", "start now",
                           "start mic=g2 mic=pdm"}) {
    reset();
    CHECK(!ok(note(line)) && dict.begins == 0);
  }
}

static void testRefusals() {
  // One note at a time.
  reset();
  CHECK(ok(note("start")));
  auto d = note("start");
  CHECK(!ok(d) && says(d, "already recording") && dict.begins == 1);
  const char* why = nullptr;  // the front-end gate says so too
  CHECK(!quickNoteCanStart(&why) && why && std::strstr(why, "already recording"));
  // Shared dictation busy (a Transcription or a keyboard).
  reset(); dict.global = DictationState::RECORDING;
  d = note("start");
  CHECK(!ok(d) && says(d, "Dictation is busy") && dict.begins == 0);
  reset(); dict.allowBegin = false;
  d = note("start");
  CHECK(!ok(d) && says(d, "Dictation is busy") && !quickNoteActive());
  CHECK(events.empty());
  reset(); dict.available = false;
  d = note("start");
  CHECK(!ok(d) && says(d, "no mic") && dict.begins == 0);
  // Native Even AI / Conversate session (R16).
  reset(); nativeVoice = true;
  d = note("start");
  CHECK(!ok(d) && says(d, "Even AI") && dict.begins == 0);
  // Below the storage reserve.
  reset(); roomOk = false;
  d = note("start");
  CHECK(!ok(d) && says(d, "storage") && dict.begins == 0);
  // The chosen mic is gone.
  reset(); caller(SOURCE_G2_GLASSES, "alice", 9); g2MicAvailable = false;
  d = note("start");
  CHECK(!ok(d) && says(d, "Glasses mic unavailable") && dict.begins == 0);
  // The start gate a front end shows while idle.
  reset(); nativeVoice = true;
  d = note("status");
  CHECK(ok(d) && d["available"] == false && d["active"] == false && says(d, "") &&
        d["reason"].as<std::string>().find("Even AI") != std::string::npos);
}

static void testSaveFlow() {
  reset();
  auto d = note("start");
  const uint64_t id = idOf(d);
  CHECK(quickNoteActive());
  servicePass();  // preparing: no audio yet
  CHECK(gNote.phase == QuickNotePolicy::Phase::Starting);
  gNow += 100;
  capture();
  CHECK(gNote.phase == QuickNotePolicy::Phase::Recording);
  dict.level = 40;
  dict.pieces.push_back({1, "Buy oat milk"});
  dict.pieces.push_back({2, "and call the dentist."});
  gNow += 1000;
  servicePass();
  CHECK(dict.pieces.empty() && dict.commits == 2);
  d = noteFor("status", id);
  CHECK(ok(d) && d["phase"] == "recording" && d["captureActive"] == true && d["level"] == 40);
  CHECK(lastReply.find("oat") == std::string::npos && lastReply.find("dentist") == std::string::npos);
  gNow += 2000;
  d = noteFor("stop", id);
  CHECK(ok(d) && gNote.stopWanted && dict.stops == 0);  // the worker issues it
  servicePass();
  CHECK(dict.stops == 1 && gNote.phase == QuickNotePolicy::Phase::Finishing);
  servicePass();  // still finishing: no second stop
  CHECK(dict.stops == 1 && creates.empty());
  dict.done = true;
  dict.captureActive = false;
  servicePass();
  CHECK(creates.size() == 1 && !quickNoteActive());
  const Created& c = creates[0];
  CHECK(c.text == "Buy oat milk and call the dentist." && c.user == "alice");
  CHECK(c.source == SOURCE_WEB && c.epoch == 7 && c.header.id == id);
  CHECK(c.header.trigger == QuickNotePolicy::Trigger::Web && c.header.mic == QuickNotePolicy::Mic::G2);
  CHECK(c.header.durationMs == 3000);
  CHECK(events.size() == 2 && events[1].kind == SYSEVT_NOTE_SAVED && events[1].subject == "web" &&
        events[1].detail == "7w 3000ms" && events[1].who == "alice" &&
        events[1].source == 100 + SOURCE_WEB);
  d = noteFor("status", id);
  CHECK(ok(d) && d["outcome"] == "saved" && d["words"] == 7 && d["phase"] == "idle" &&
        d["path"].as<std::string>().rfind("/stt/u7/notes/boot-", 0) == 0 && d["durationMs"] == 3000);
  // R22: the words reach the store only; logs, events and status never see them.
  for (const char* word : {"oat", "milk", "dentist", "Buy"}) CHECK(!anywhere(word));
  CHECK(lastReply.find("oat") == std::string::npos);
  CHECK(textWiped());
  // Another user sees nothing of it; an admin may.
  caller(SOURCE_WEB, "bob", 7);
  CHECK(!ok(noteFor("status", id)));
  caller(SOURCE_WEB, "carol", 7);
  CHECK(ok(noteFor("status", id)));
}

static void testStopCancelAuthority() {
  reset();
  const uint64_t id = idOf(note("start"));
  capture();
  caller(SOURCE_WEB, "bob", 7);
  CHECK(!ok(noteFor("stop", id)) && !ok(noteFor("cancel", id)) && !ok(noteFor("status", id)));
  auto d = note("status");  // no id: nothing of bob's, only the busy flag
  CHECK(ok(d) && d["id"] == "" && d["busy"] == true && d["available"] == false);
  d = note("");  // bare `note` is `note status`
  CHECK(ok(d) && d["id"] == "" && d["busy"] == true && d["available"] == false);
  CHECK(!gNote.stopWanted && !gNote.cancelWanted);
  caller(SOURCE_SERIAL, "alice", 3);  // the owner from any live session
  CHECK(ok(noteFor("status", id)));
  d = note("");
  CHECK(ok(d) && idOf(d) == id && d["active"] == true && d["phase"] == "recording");
  caller(SOURCE_WEB, "carol", 7);     // an admin
  d = noteFor("cancel", id);
  CHECK(ok(d) && gNote.cancelWanted);
  servicePass();
  CHECK(dict.cancels == 1 && !quickNoteActive() && creates.empty() && gLast.outcome == QuickNoteOutcome::Discarded);
  CHECK(std::string(gLast.reason) == "cancelled");
  CHECK(events.size() == 1);  // a discard posts nothing
  // Malformed and unknown ids.
  for (const char* line : {"stop", "stop 12", "stop 0000000000000000", "stop zzzzzzzzzzzzzzzz",
                           "cancel 1122334455667788", "status 1122334455667788 x"}) {
    CHECK(!ok(note(line)));
  }
}

static void testRevocation() {
  reset();
  const uint64_t id = idOf(note("start"));
  capture();
  dict.pieces.push_back({1, "half a private thought"});
  servicePass();
  liveEpochs.erase({SOURCE_WEB, 7});  // logout / session replaced
  servicePass();
  CHECK(!quickNoteActive() && creates.empty() && gLast.outcome == QuickNoteOutcome::Failed);
  CHECK(std::string(gLast.reason) == "session ended" && gLast.id == id);
  CHECK(events.back().kind == SYSEVT_NOTE_FAILED && events.back().subject == "session ended");
  CHECK(dict.cancels == 1 && textWiped() && !anywhere("private thought"));
  // The glasses unpaired / G2 session replaced mid-note.
  reset(); caller(SOURCE_SERIAL, "alice", 3);
  CHECK(ok(note("start glasses")));
  capture();
  liveEpochs.erase({SOURCE_G2_GLASSES, 9});
  servicePass();
  CHECK(gLast.outcome == QuickNoteOutcome::Failed && creates.empty());
  // The caller's own session dying mid-command yields no reply data.
  reset();
  idOf(note("start"));
  liveEpochs.erase({SOURCE_WEB, 7});
  auto d = note("status");
  CHECK(!ok(d) && says(d, "Sign in"));
}

static void testDiscards() {
  // A hold released before 600 ms of audio is a slip: discarded, no file.
  reset(); caller(SOURCE_G2_GLASSES, "alice", 9);
  uint64_t id = idOf(note("start hold"));
  capture();
  gNow += 300;
  CHECK(ok(noteFor("stop", id)) && gNote.cancelWanted);
  servicePass();
  CHECK(gLast.outcome == QuickNoteOutcome::Discarded && std::string(gLast.reason) == "too short");
  CHECK(dict.cancels == 1 && dict.stops == 0 && creates.empty() && events.size() == 1);
  // A hold whose recording ended by itself under 600 ms is discarded too.
  reset(); caller(SOURCE_G2_GLASSES, "alice", 9);
  id = idOf(note("start hold"));
  capture();
  dict.pieces.push_back({1, "remember this"});
  gNow += 400;
  servicePass();
  dict.done = true;
  servicePass();
  CHECK(gLast.outcome == QuickNoteOutcome::Discarded && std::string(gLast.reason) == "too short");
  CHECK(creates.empty() && textWiped());
  // A hold whose dictation fails before any audio is a failure, not a slip.
  reset(); caller(SOURCE_G2_GLASSES, "alice", 9);
  id = idOf(note("start hold"));
  dict.done = true;
  dict.state = DictationState::FAILED;
  dict.failure = "local model unavailable";
  servicePass();
  CHECK(!quickNoteActive() && gLast.id == id && gLast.outcome == QuickNoteOutcome::Failed &&
        std::string(gLast.reason) == "local model unavailable" && creates.empty());
  CHECK(events.size() == 2 && events[1].kind == SYSEVT_NOTE_FAILED &&
        events[1].subject == "local model unavailable" && events[1].who == "alice");
  // Stopped before any audio.
  reset();
  id = idOf(note("start"));
  servicePass();
  CHECK(ok(noteFor("stop", id)));
  servicePass();
  CHECK(gLast.outcome == QuickNoteOutcome::Discarded &&
        std::string(gLast.reason) == "stopped before recording" && creates.empty());
  // A breath: two filler words in under 2.5 s.
  reset();
  id = idOf(note("start"));
  capture();
  dict.pieces.push_back({1, "Um, okay."});
  gNow += 1000;
  CHECK(ok(noteFor("stop", id)));
  servicePass();
  dict.done = true;
  servicePass();
  CHECK(gLast.outcome == QuickNoteOutcome::Discarded && std::string(gLast.reason) == "nothing heard");
  CHECK(creates.empty() && textWiped());
  // Silence: nothing at all.
  reset();
  id = idOf(note("start"));
  capture();
  gNow += 4000;
  dict.done = true;  // the recorder's own silence stop
  servicePass();
  CHECK(gLast.outcome == QuickNoteOutcome::Discarded && creates.empty());
  // A long-enough hold keeps what was said.
  reset(); caller(SOURCE_G2_GLASSES, "alice", 9);
  id = idOf(note("start hold"));
  capture();
  dict.pieces.push_back({1, "remember the parking level"});
  gNow += 2500;
  CHECK(ok(noteFor("stop", id)) && gNote.stopWanted);
  servicePass();
  dict.done = true;
  servicePass();
  CHECK(creates.size() == 1 && creates[0].header.trigger == QuickNotePolicy::Trigger::Hold &&
        creates[0].source == SOURCE_G2_GLASSES && gLast.outcome == QuickNoteOutcome::Saved);
}

static void testLimitsAndFailures() {
  // notemaxsec (clamped to the CM5 recorder's 30 s on a Pi build).
  reset(); gSettings.noteMaxSec = 60;
  uint64_t id = idOf(note("start"));
  const uint32_t maxMs = ENABLE_LOCAL_STT ? 60000u : 30000u;
  CHECK(note("status")["maxMs"] == maxMs);
  capture();
  gNow += maxMs - 1;
  servicePass();
  CHECK(dict.stops == 0);
  gNow += 1;
  servicePass();
  CHECK(dict.stops == 1 && gNote.phase == QuickNotePolicy::Phase::Finishing);
  reset(); gSettings.noteMaxSec = 5;  // below the range: 10 s
  idOf(note("start"));
  capture();
  gNow += 10000;
  servicePass();
  CHECK(dict.stops == 1);
  // A native voice feature takes the glasses mid-note: finish and keep (R16).
  reset();
  id = idOf(note("start"));
  capture();
  dict.pieces.push_back({1, "first half of the idea"});
  gNow += 3000;
  nativeVoice = true;
  servicePass();
  CHECK(dict.stops == 1);
  dict.done = true;
  dict.state = DictationState::FAILED;
  dict.failure = "microphone taken";
  servicePass();
  CHECK(creates.size() == 1 && creates[0].text == "first half of the idea" &&
        gLast.outcome == QuickNoteOutcome::Saved);
  // Before any audio, a native session or the limit abandons the note: no
  // stop request, the lease is cancelled and the note ends at once.
  reset();
  id = idOf(note("start"));
  servicePass();  // still preparing
  nativeVoice = true;
  servicePass();
  CHECK(!quickNoteActive() && dict.stops == 0 && dict.cancels == 1 && creates.empty());
  CHECK(gLast.outcome == QuickNoteOutcome::Discarded && gLast.id == id);
  CHECK(events.size() == 1);  // discards post nothing
  reset();
  id = idOf(note("start"));
  gNow += maxMs;
  servicePass();
  CHECK(!quickNoteActive() && dict.cancels == 1 && gLast.outcome == QuickNoteOutcome::Failed);
  CHECK(std::string(gLast.reason) == "recording did not start" &&
        events.back().kind == SYSEVT_NOTE_FAILED);
  CHECK(ok(note("start")));  // the slot is free again
  // A failure with nothing said is a failure, with dictation's reason.
  reset();
  id = idOf(note("start"));
  dict.done = true;
  dict.state = DictationState::FAILED;
  dict.failure = "local model unavailable";
  servicePass();
  CHECK(gLast.outcome == QuickNoteOutcome::Failed && std::string(gLast.reason) == "local model unavailable");
  CHECK(events.back().kind == SYSEVT_NOTE_FAILED && events.back().subject == "local model unavailable");
  // Stop, then no final text inside the finish window: cancel, keep what came.
  reset();
  id = idOf(note("start"));
  capture();
  dict.pieces.push_back({1, "words before the stall"});
  gNow += 2000;
  CHECK(ok(noteFor("stop", id)));
  servicePass();
  gNow += (ENABLE_LOCAL_STT ? 30000u : 95000u) - 1;
  servicePass();
  CHECK(quickNoteActive() && dict.cancels == 0);
  gNow += 1;
  servicePass();
  CHECK(dict.cancels == 1 && creates.size() == 1 && gLast.outcome == QuickNoteOutcome::Saved);
  // The store refuses: failed, with the store's reason, and no text kept.
  reset(); createFails = true;
  id = idOf(note("start"));
  capture();
  dict.pieces.push_back({1, "an unsaved private sentence"});
  gNow += 3000;
  servicePass();  // drained; the recorder then ends on silence
  dict.done = true;
  servicePass();
  CHECK(gLast.outcome == QuickNoteOutcome::Failed && creates.size() == 1);
  CHECK(events.back().kind == SYSEVT_NOTE_FAILED && events.back().subject == "Note storage full or unavailable");
  CHECK(textWiped() && !anywhere("unsaved"));
  // A flood of text: capped at kMaxNoteBytes, the rest acknowledged, stopped.
  reset();
  id = idOf(note("start"));
  capture();
  for (uint32_t s = 1; s <= 20; ++s) dict.pieces.push_back({s, std::string(250, 'a' + (s % 26))});
  for (int i = 0; i < 4; ++i) servicePass();
  CHECK(dict.pieces.empty() && dict.stops == 1 && gQnTextLen == QuickNotePolicy::kMaxNoteBytes);
  dict.done = true;
  gNow += 5000;
  servicePass();
  CHECK(creates.size() == 1 && creates[0].text.size() == QuickNotePolicy::kMaxNoteBytes);
}

static void testFiles() {
  reset();
  auto d = note("list");
  CHECK(ok(d) && listUser == "alice" && listOwner == 0 && !listSd && d["notes"].size() == 4);
  CHECK(d["notes"][0]["title"] == "Private title 0" && d["notes"][0]["trigger"] == "hold" &&
        d["notes"][0]["mic"] == "g2" && d["notes"][0]["words"] == 4);
  CHECK(d["notes"][1]["title"] == "" && d["notes"][1]["trigger"] == "");  // header did not parse
  CHECK(d["more"] == true && d["nextOffset"] == 4 && d["total"] == 9);
  // Each row's time: local hh:mm (UTC here), even from a name whose header
  // did not parse; a "boot" note's age this boot, or "earlier"; never "--:--".
  CHECK(d["notes"][0]["when"] == "15:30" && d["notes"][1]["when"] == "15:30");
  CHECK(d["notes"][2]["when"] == "5m ago" && d["notes"][2]["created"] == "boot");
  CHECK(d["notes"][3]["when"] == "earlier");
  CHECK(lastReply.find("--:--") == std::string::npos);
  Clock::tzOffset = -300;  // the wearer's timezone (tzoffsetminutes), UTC-5
  d = note("list");
  CHECK(d["notes"][0]["when"] == "10:30" && d["notes"][2]["when"] == "5m ago");
  Clock::tzOffset = 0;
  for (uint8_t b : gPageStorage) CHECK(b == 0);  // titles cleared once serialized
  d = note("list sd 8 u12");
  CHECK(ok(d) && listSd && listOwner == 12 && d["offset"] == 8);
  d = note("list 16");
  CHECK(ok(d) && !listSd && d["offset"] == 16);
  // No tier: the one a save would use now, so a card note lists after saving.
  sdWritable = true;
  CHECK(ok(note("list")) && listSd);
  CHECK(ok(note("list 8")) && listSd);
  CHECK(ok(note("list internal")) && !listSd);
  sdWritable = false;
  for (const char* line : {"list sd x", "list u0", "list ux", "list 1 2", "list sd 1 u3 x"})
    CHECK(!ok(note(line)));
  listOk = false;
  CHECK(!ok(note("list")));
  d = note("read \"/stt/u7/notes/boot-0000000000000011.txt\"");
  CHECK(ok(d) && d["sttText"] == "secret window words" && d["eof"] == true &&
        d["transcriptPath"] == "/stt/u7/notes/boot-0000000000000011.txt");
  d = note("read /stt/u7/notes/boot-0000000000000011.txt 7");
  CHECK(ok(d) && d["sttText"] == "window words" && d["offset"] == 7 && d["nextOffset"] == 19);
  d = note("read \"/stt/u8/notes/boot-0000000000000011.txt\" 0");
  CHECK(!ok(d) && !d["sttText"].is<const char*>());
  CHECK(!ok(note("read")) && !ok(note("read \"/x\" -1")));
  d = note("delete \"/stt/u7/notes/boot-0000000000000011.txt\"");
  CHECK(ok(d) && removedPath == "/stt/u7/notes/boot-0000000000000011.txt");
  CHECK(!ok(note("delete")) && !ok(note("bogus")));
  // Store calls never run inside the service lock.
  CHECK(gDepth == 0);
}

// One complete note through the worker, saved with the fake store's stamp.
static uint64_t saveNote(const char* words) {
  const uint64_t id = idOf(note("start"));
  capture();
  dict.pieces.push_back({1, words});
  gNow += 2000;
  CHECK(ok(noteFor("stop", id)));
  servicePass();
  dict.done = true;
  dict.captureActive = false;
  servicePass();
  restampPass();  // the worker's own pass after every servicePass
  CHECK(!quickNoteActive() && gLast.id == id && gLast.outcome == QuickNoteOutcome::Saved);
  return id;
}

static void testRestamp() {
  // Notes saved before the clock was set are remembered by the worker...
  reset();
  const uint64_t a = saveNote("first private words");
  gUptimeS += 60;
  const uint64_t b = saveNote("second private words");
  gUptimeS += 61;
  const uint64_t c = saveNote("third private words");
  CHECK(gUnstampedCount == 3 && restamps.empty() && !gRestampDue);
  CHECK(creates.size() == 3);
  // Their ids are boot #76 then the uptime at the start, so their boot-
  // names list newest first.
  CHECK(a >> 16 == ((76ull << 24) | 1000) && b >> 16 == ((76ull << 24) | 1060) &&
        c >> 16 == ((76ull << 24) | 1121));
  CHECK(hex(a) < hex(b) && hex(b) < hex(c));
  // ...and nothing happens until the clock is set (the hook wakes the worker).
  restampPass();
  CHECK(restamps.empty() && !gRestampDue && gUnstampedCount == 3);
  const unsigned woken = notifies;
  quickNoteClockSet();
  CHECK(notifies == woken + 1);
  clockSet = true;
  // Never while a note runs. With the clock set an id is random again.
  const uint64_t live = idOf(note("start"));
  CHECK(live == ((uint64_t(gRandom - 1) << 32) | gRandom) ||
        live == ((uint64_t(gRandom) << 32) | (gRandom - 1)));
  restampPass();
  CHECK(restamps.empty() && gRestampDue);
  CHECK(ok(noteFor("cancel", live)));
  servicePass();
  CHECK(!quickNoteActive());
  // Then at most two a pass, each as its owner, at the time its uptime gives.
  restampPass();
  CHECK(restamps.size() == 2 && gUnstampedCount == 1 && gRestampDue);
  CHECK(restamps[0].user == "alice" && restamps[0].source == SOURCE_WEB &&
        restamps[0].path == "/stt/u7/notes/boot-" + hex(a) + ".txt" &&
        restamps[0].stamp == "20261004T151640Z");
  restampPass();
  CHECK(restamps.size() == 3 && !gUnstampedCount && !gRestampDue);
  CHECK(restamps[1].stamp != restamps[2].stamp);
  CHECK((restamps[1].stamp == "20261004T151740Z" && restamps[2].stamp == "20261004T151841Z") ||
        (restamps[2].stamp == "20261004T151740Z" && restamps[1].stamp == "20261004T151841Z"));
  for (const char* word : {"first", "second", "third", "private"}) CHECK(!anywhere(word));
  size_t given = 0;
  for (const auto& line : logs) given += line.find("given its time 20261004T15") != std::string::npos;
  CHECK(given == 3);
  restampPass();
  CHECK(restamps.size() == 3);
}

static void testRestampFailures() {
  // A retryable failure is retried on later passes, then given up.
  reset();
  saveNote("kept words");
  clockSet = true;
  restampFailures = 100;
  for (uint8_t pass = 1; pass < 5; ++pass) {
    restampPass();
    CHECK(gUnstampedCount == 1 && gRestampDue && restamps.size() == pass);
  }
  restampPass();
  CHECK(!gUnstampedCount && !gRestampDue && restamps.size() == 5 && anywhere("keeps its boot name"));
  // One that cannot succeed is dropped at once.
  reset();
  saveNote("gone words");
  clockSet = true;
  restampFailures = 1;
  restampGone = true;
  restampPass();
  CHECK(!gUnstampedCount && restamps.size() == 1 && anywhere("Note unavailable"));
  // A pass while a note is still starting waits for a timed retry: a start
  // that then fails wakes nobody, and the note waiting must not be stranded.
  reset();
  saveNote("pending words");
  clockSet = true;
  dict.allowBegin = false;
  dict.onBegin = [] { restampPass(); };
  CHECK(!ok(note("start")) && dict.begins == 2 && !quickNoteActive());
  CHECK(restamps.empty() && gRestampDue && gUnstampedCount == 1);
  restampPass();  // the worker's timed wait (kRestampRetryMs) ran out
  CHECK(restamps.size() == 1 && !gUnstampedCount && !gRestampDue);
  // Bounded: past 16 notes this boot, a note keeps its boot name.
  reset();
  for (int i = 0; i < 17; ++i) saveNote("many notes");
  CHECK(gUnstampedCount == 16 && anywhere("over 16 notes before the clock was set"));
  clockSet = true;
  for (int i = 0; i < 8; ++i) restampPass();
  CHECK(!gUnstampedCount && restamps.size() == 16);
}

static void testWarm() {
  reset();
  quickNoteWarm();
  CHECK(warmCalls.size() == (ENABLE_LOCAL_STT ? 1u : 0u));
  if (ENABLE_LOCAL_STT) CHECK(warmCalls[0] == std::make_pair(120000u, true));
  warmCalls.clear();
  // A running note keeps the model resident until two minutes after it; it
  // never starts a load itself (its STT worker has the model).
  const uint64_t id = idOf(note("start"));
  servicePass();
  capture();
  CHECK(warmCalls.size() == (ENABLE_LOCAL_STT ? 2u : 0u));
  for (const auto& call : warmCalls) CHECK(call == std::make_pair(120000u, false));
  CHECK(ok(noteFor("cancel", id)));
  servicePass();
  warmCalls.clear();
  servicePass();  // idle: no note, no hold
  CHECK(warmCalls.empty());
}

int main() {
  testAuthority();
  testRefusals();
  testSaveFlow();
  testStopCancelAuthority();
  testRevocation();
  testDiscards();
  testLimitsAndFailures();
  testFiles();
  testRestamp();
  testRestampFailures();
  testWarm();
  if (failures) {
    std::fprintf(stderr, "%d QuickNote service check(s) failed\n", failures);
    return 1;
  }
  std::printf("QuickNote service (ENABLE_LOCAL_STT=%d): authority, one note, refusals, save/discard/fail, "
              "revocation, limits, private CLI replies, list times, restamping and keep-warm passed\n", ENABLE_LOCAL_STT);
  return 0;
}
