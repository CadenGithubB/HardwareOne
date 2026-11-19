// test_phone_status.py compiles the production BLE_PhoneStatus.cpp beside the
// real CompactJson and argWantsJson, and links the real BLE_LinkStatus.cpp. The
// pipe, the console server, the glasses, the peer registry and the SSE sink are
// host boundaries the cases drive; output lines are checked by Python.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "BLE_PhoneStatus.h"
#include "BLE_Events.h"
#include "BLE_Peers.h"        // host stub: the real BlePeerKind + BlePeerReconnectSnapshot
#include "BLE_PhonePipe.h"    // host stub: the pipe's public status calls + link summary
#include "BLE_PhonePipeConsole.h"  // the real console driver header (its status)
#include "BLE_Server.h"        // host stub: the real BleConsoleView + console row identity
#include "G2_Glasses.h"       // host stub: isG2ClientInitialized
#include "System_Settings.h"  // host stub: bleMode, the console setting, the phone secret

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); } } while (false)

// ---- Boundaries ----------------------------------------------------------------
uint32_t gNow = 1000;
uint32_t millis() { return gNow; }
void webBatchTestStringAppend() {}

static bool pipeActive = false, pipeStandalone = false, pipeReady = false, pipeEnrolling = false;
static hw1link::PipeStep pipeStep = hw1link::PipeStep::Idle;
static const char* pipeState = "stopped";
bool phonePipeActive() { return pipeActive; }
bool phonePipeStandalone() { return pipeActive && pipeStandalone; }
bool phonePipeReady() { return pipeReady; }
const char* phonePipeStatus() { return pipeState; }
// The status whose text is `label`: the scenarios name a pipe status by its text.
static uint8_t pipeStatusCodeOf(const char* label) {
  if (!label) return uint8_t(hw1link::PipeStatus::Stopped);
  for (const auto& row : hw1link::kPipeStatusRows)
    if (row.label && !strcmp(row.label, label)) return uint8_t(row.status);
  return uint8_t(hw1link::PipeStatus::StackRefused);
}
uint8_t phonePipeStatusCode() { return pipeStatusCodeOf(phonePipeStatus()); }
uint8_t phonePipeStep() { return uint8_t(pipeStep); }
bool phonePipeEnrolling() { return pipeEnrolling; }
// The phone path (BLE_PhonePipePath.h): the view the cases set, and the nudge
// (never called by the status owner).
static PhonePathView pathView;
static unsigned pathReads = 0;
PhonePathView phonePipePathView() { ++pathReads; return pathView; }
bool phonePipeNudgeReconnect() { CHECK(false); return false; }
static PhonePathView path(PhonePathState state, PhonePathHint hint = PhonePathHint::None,
                          uint32_t retryInMs = 0, uint8_t attempt = 0) {
  PhonePathView v; v.state = state; v.hint = hint; v.retryInMs = retryInMs; v.attempt = attempt; return v;
}

// Phone link v2: the owner's summary (false before the pipe has resources), the
// console driver's status and its row's leaf snapshot.
static bool summaryKnown = false;
static PhonePipeLinkSummary summary;
static unsigned summaryReads = 0;
bool phonePipeLinkSummary(PhonePipeLinkSummary* out) {
  ++summaryReads;
  if (out) *out = summaryKnown ? summary : PhonePipeLinkSummary{};
  return summaryKnown;
}
static PipeConsoleStatus consoleStream;
void pipeConsoleStatus(PipeConsoleStatus* out) { if (out) *out = consoleStream; }
static BlePipeConsoleIdentity consoleRow;
void blePipeConsoleIdentity(BlePipeConsoleIdentity* out) { if (out) *out = consoleRow; }
// `phone pair new`'s mark on a device-made code.
static void markGenerated(bool on) { gSettings.phonePipeSecretGenerated = on; }

static bool serverUp = false, glassesUp = false;
static BleConsoleView consoleView;
bool isBleServerInitialized() { return serverUp; }
bool bleServerUp() { return serverUp; }
bool isG2ClientInitialized() { return glassesUp; }
BleConsoleView bleConsoleView() { return consoleView; }
HostSettings gSettings;

static bool peerEnrolled = false, peerAutoReconnect = false;
static unsigned peerReads = 0;
bool blePeerReconnectSnapshot(BlePeerKind kind, BlePeerReconnectSnapshot& out) {
  CHECK(kind == BLE_PEER_PHONE);
  ++peerReads;
  out = BlePeerReconnectSnapshot{};
  out.hasSavedTarget = peerEnrolled;
  out.autoReconnect = peerAutoReconnect;
  return true;
}

struct Pushed { std::string name, payload; bool clean; };
static std::vector<Pushed> pushed;
bool blePushEvent(const char* eventName, CompactJson& json) {
  const char* payload = json.c_str();
  pushed.push_back({eventName, payload, json.ok()});
  return json.ok();
}

// INSERT_PRODUCTION

// `status json` connectivity.bluetooth: System_Utils.cpp's block as shipped,
// over the aggregate-status boundaries below.
static bool bleSubsystemActive() { return isBleServerInitialized() || glassesUp || pipeActive; }
static const char* bleSubsystemStateString() { return "connected"; }
static const char* getBleModeString() {
  return gSettings.bleMode == 0 ? "server" : gSettings.bleMode == 1 ? "client" : "phone-client";
}
static void statusJsonBluetooth(JsonObject conn) {
// INSERT_STATUS_JSON
}
static void printStatusJson() {
  JsonDocument doc;
  statusJsonBluetooth(doc["connectivity"].to<JsonObject>());
  CHECK(!doc.overflowed());
  std::string json;
  serializeJson(doc["connectivity"]["bluetooth"], json);
  std::printf("STATUSBT\t%s\n", json.c_str());
}

using hw1link::PipeStep;
// `standalone` now means the pipe alone on the shared stack, i.e. the glasses
// runtime closed (BLE_PhoneStatus.cpp phonePipeAlone()): an active pipe sets
// the glasses flag the other way.
static void set(bool active, bool standalone, bool ready, PipeStep step, const char* state) {
  pipeActive = active; pipeStandalone = standalone; pipeReady = ready;
  pipeStep = step; pipeState = state;
  if (active) glassesUp = !standalone;
}
// One tick past the 250 ms cadence; true when it pushed an event (printed).
static bool tickPushed() {
  const size_t before = pushed.size();
  gNow += 300;
  phoneStatusTick();
  if (pushed.size() == before) return false;
  CHECK(pushed.size() == before + 1 && pushed.back().name == "phone-status");
  std::printf("EVENT\t%d\t%s\n", pushed.back().clean ? 1 : 0, pushed.back().payload.c_str());
  return true;
}
static void printJson(const char* kind) {
  JsonDocument doc;
  CHECK(phoneBuildStatusJson(doc.to<JsonObject>()));
  std::string json;
  serializeJson(doc, json);
  std::printf("%s\t%s\n", kind, json.c_str());
  std::printf("CMDJSON\t%s\n", phoneStatusCommand(String("json")));
  std::printf("TEXT\t%s\n", phoneStatusCommand(String("")));
}

// ---- Cases -----------------------------------------------------------------------
int main() {
  using hw1link::Pipe;
  // The lens/OLED word is the pipe phase alone; the reason is pipeLine's.
  PhoneStatusSnapshot s;
  CHECK(std::strcmp(hw1link::pipeText(s.pipe.phase), "off") == 0);
  s.pipe = {Pipe::Ready, nullptr};
  CHECK(std::strcmp(hw1link::pipeText(s.pipe.phase), "ready") == 0);
  s.pipe = {Pipe::Failed, "not found"};
  CHECK(std::strcmp(hw1link::pipeText(s.pipe.phase), "fail") == 0);

  // SSE: the first tick only records the state; nothing changes, nothing goes out.
  set(false, false, false, PipeStep::Idle, "stopped");
  phoneStatusTick();
  gNow += 1000; phoneStatusTick();
  CHECK(pushed.empty() && peerReads == 0);

  // A change pushes once, then the same state stays quiet.
  static const char* const kScanning = "scanning";
  set(true, false, false, PipeStep::Scanning, kScanning);
  CHECK(tickPushed());
  CHECK(!tickPushed());

  // Within the 250 ms cadence a change waits for the next check.
  peerEnrolled = peerAutoReconnect = true;
  set(true, false, true, PipeStep::Link, "ready");
  gNow += 100; phoneStatusTick();
  CHECK(pushed.size() == 1);
  gNow += 200; phoneStatusTick();
  CHECK(pushed.size() == 2);
  std::printf("EVENT\t%d\t%s\n", pushed[1].clean ? 1 : 0, pushed[1].payload.c_str());

  // The setup step alone is a change: the same text, searching -> linking.
  set(true, false, false, PipeStep::Scanning, kScanning);
  CHECK(tickPushed());
  set(true, false, false, PipeStep::Link, kScanning);
  CHECK(tickPushed());

  // Enrolling alone is a change: waiting -> pairing.
  peerEnrolled = false;
  set(true, true, false, PipeStep::Waiting, "waiting to enroll");
  CHECK(tickPushed());
  pipeEnrolling = true;
  CHECK(tickPushed());

  // The text alone is a change, and a failure carries its short reason.
  pipeEnrolling = false; peerEnrolled = true;
  set(true, true, false, PipeStep::Idle, "phone not found");
  CHECK(tickPushed());
  set(true, true, false, PipeStep::Idle, "BLE setup timed out");
  CHECK(tickPushed());
  // The longest pipe text never enters the event; not paired has no reason.
  peerEnrolled = false;
  set(true, true, false, PipeStep::Idle, "pipe authentication cooldown or secret unavailable");
  CHECK(tickPushed());
  peerEnrolled = true;
  set(true, true, false, PipeStep::Fault, "phone cleanup incomplete");
  CHECK(tickPushed());

  // The documented worst case: every flag false with the longest id and a
  // 9-character reason (the longest the core allows) fits the 128-char cap.
  size_t longest = 0;
  for (int p = 0; p <= int(Pipe::Fault); ++p) {
    PhoneStatusSnapshot worst;
    worst.pipe = {Pipe(p), "forgotten"};
    const size_t before = pushed.size();
    phonePushStatusEvent(worst);
    CHECK(pushed.size() == before + 1 && pushed.back().clean);
    longest = std::max(longest, pushed.back().payload.size());
    pushed.pop_back();
  }
  CHECK(longest < 128);
  std::printf("BOUND\t%zu\n", longest);

  // /api/ble/status `phone` object, `phone status json` and `phone status`.
  CHECK(!phoneBuildStatusJson(JsonObject()));
  // G2 mode with the pipe ready, keyed by a passphrase (relay on, two sessions
  // ended): the design-A console signed in on the pipe's link; the stream is
  // not offered without a device code.
  gSettings.bleMode = 1; glassesUp = true; serverUp = false; consoleView = {};
  consoleView.registered = consoleView.attachable = consoleView.attached = true;
  consoleView.secret = true; consoleView.clients = 1; consoleView.signedIn = true;
  peerEnrolled = true; peerAutoReconnect = false;
  gSettings.phonePipeSecret = "Pass-phrase1!"; markGenerated(false);
  summaryKnown = true; summary = {};
  summary.active = summary.ready = summary.relayOn = true; summary.relayStreams = 1;
  summary.ended = 2; summary.lastEnd = 18; summary.endReason[14] = 1; summary.endReason[18] = 1;
  consoleStream = {}; consoleStream.state = PipeConsoleState::NotOffered;
  pathView = path(PhonePathState::Up);
  set(true, false, true, PipeStep::Link, "ready");
  printJson("JSON");
  printStatusJson();
  // Phone-only after a failed search (enrolled, auto-reconnect on), no secret:
  // nothing offers the console on its host.
  gSettings.bleMode = 1; glassesUp = false; peerAutoReconnect = true; consoleView = {};  // the pipe alone is the client role (no phone-client mode)
  gSettings.phonePipeSecret = "";
  summary.ready = summary.relayOn = false; summary.relayStreams = 0;
  // The last scan heard 14 reports, none the phone's: retrying in 13 s, try 3.
  summary.scanKnown = true; summary.scanReports = 14; summary.scanMatches = 0;
  consoleStream = {};
  pathView = path(PhonePathState::Retrying, PhonePathHint::NotHeard, 12001, 3);
  set(true, true, false, PipeStep::Idle, "phone not found");
  printJson("JSON");
  printStatusJson();
  // Server mode: the pipe is off (never started: no summary) and a signed-in
  // app holds the console.
  gSettings.bleMode = 0; serverUp = true;
  consoleView.up = true; consoleView.advertising = true;
  consoleView.clients = 1; consoleView.signedIn = true;
  peerEnrolled = peerAutoReconnect = false;
  summaryKnown = false;
  pathView = path(PhonePathState::Off, PhonePathHint::ConsoleServer);
  set(false, false, false, PipeStep::Idle, "stopped");
  printJson("JSON");
  printStatusJson();
  // A standalone pipe while the saved mode is server: no console here.
  serverUp = false; consoleView = {};
  summaryKnown = true; summary = {}; summary.active = true;
  pathView = path(PhonePathState::Connecting, PhonePathHint::None, 0, 1);
  set(true, true, false, PipeStep::Scanning, kScanning);
  printJson("JSON");
  // G2 mode, the pipe searching again: the slot its old link left is neither
  // shown as connected nor counted; the console waits for the phone link.
  gSettings.bleMode = 1; glassesUp = true;
  consoleView.registered = consoleView.attachable = consoleView.attached = true;
  consoleView.secret = true; consoleView.clients = 1; consoleView.signedIn = true;
  set(true, false, false, PipeStep::Scanning, kScanning);
  printJson("JSON");
  printStatusJson();
  // Phone link v2, phone-only and READY with a device code: the console stream
  // open and its row signed in (the console view not read yet: still one
  // session), relay forwarding off.
  gSettings.bleMode = 1; glassesUp = false; consoleView = {};  // the pipe alone is the client role (no phone-client mode)
  peerEnrolled = true;
  gSettings.phonePipeSecret = "0123456789ABCDEF"; markGenerated(true);
  summary = {}; summary.active = summary.ready = true;
  summary.consoleOffered = summary.consoleWanted = true; summary.consoleStreams = 1;
  consoleStream = {}; consoleStream.state = PipeConsoleState::Open;
  consoleStream.offered = consoleStream.wanted = true; consoleStream.streamId = 3;
  consoleRow = {}; consoleRow.bound = true; consoleRow.streamId = 3; consoleRow.epoch = 9;
  consoleRow.authed = true;
  pathView = path(PhonePathState::RelayOff);
  set(true, true, true, PipeStep::Link, "ready");
  printJson("JSON");
  printStatusJson();
  // Signed out on a row of another stream is not this stream's sign-in.
  consoleRow.streamId = 5;
  {
    const BleLinkStatus link = bleLinkStatus();
    CHECK(link.console == hw1link::Console::Connected && !link.phone.streamSignedIn);
  }
  consoleRow.streamId = 3;
  // The BLE idle logout signs the console row out (a new epoch): the stream
  // stays open, signed out, and the link and relay read the same (the relay
  // never depends on a login).
  summary.relayOn = true;
  {
    const BleLinkStatus before = bleLinkStatus();
    consoleRow.authed = false; consoleRow.epoch = 10;
    const BleLinkStatus after = bleLinkStatus();
    CHECK(before.console == hw1link::Console::SignedIn && after.console == hw1link::Console::Connected);
    CHECK(hw1link::linkConsoleOf(before.phone) == hw1link::LinkConsole::SignedIn &&
          hw1link::linkConsoleOf(after.phone) == hw1link::LinkConsole::Open);
    CHECK(std::strcmp(hw1link::relayId(after.phone), "on") == 0 &&
          std::strcmp(hw1link::relayId(before.phone), "on") == 0 &&
          after.pipe.phase == hw1link::Pipe::Ready && after.phone.consoleStreams == 1);
    consoleRow.authed = true; consoleRow.epoch = 9;
  }
  summary.relayOn = false;
  // Offered while the app does not want it: the console waits for the phone.
  summary.consoleWanted = false; summary.consoleStreams = 0;
  consoleStream.state = PipeConsoleState::Waiting; consoleStream.wanted = false;
  consoleStream.streamId = 0; consoleRow = {};
  printJson("JSON");
  printStatusJson();
  markGenerated(false); gSettings.phonePipeSecret = ""; peerEnrolled = false;

  // The largest real object (longest text, a reason, the longest console
  // words, the longest path text with a 3-digit retry and attempt) and the
  // largest text row stay inside the 640-byte reply.
  serverUp = true; consoleView = {}; consoleView.up = true;
  summaryKnown = true; summary = {}; summary.scanKnown = true;
  summary.scanReports = 4294967295u; summary.scanMatches = 4294967295u;
  pathView = path(PhonePathState::Retrying, PhonePathHint::NotHeard, 999000, 255);
  set(true, true, false, PipeStep::Retiring, "pipe authentication cooldown or secret unavailable");
  {
    JsonDocument doc;
    CHECK(phoneBuildStatusJson(doc.to<JsonObject>()));
    CHECK(doc["pipeReason"] == "auth fail" && doc["consoleText"] == "not visible (advertising off)");
    CHECK(doc["retryInS"] == 999 && doc["attempt"] == 255);
    std::printf("LARGEST\t%zu\n", measureJson(doc));
    CHECK(measureJson(doc) < 640);
    const char* text = phoneStatusCommand(String(""));
    CHECK(std::strncmp(text, "pipe=", 5) == 0);
    std::printf("LONGESTTEXT\t%zu\n", std::strlen(text));
  }
  // phonePathText: every state's long and short text; the short form fits a
  // 21-column OLED line even at its widest.
  {
    const PhonePathState states[] = {PhonePathState::Up, PhonePathState::RelayOff, PhonePathState::Connecting,
                                     PhonePathState::Retrying, PhonePathState::Held, PhonePathState::NotPaired,
                                     PhonePathState::Off};
    const PhonePathHint hints[] = {PhonePathHint::None, PhonePathHint::NotHeard, PhonePathHint::RadioSilent,
                                   PhonePathHint::FoundNoLink, PhonePathHint::ClosedByUser, PhonePathHint::AppTooOld,
                                   PhonePathHint::RadioFault, PhonePathHint::ConsoleServer};
    for (PhonePathState st : states)
      for (PhonePathHint h : hints)
        for (uint32_t ms : {0u, 1u, 12001u, 60000u, 600000u, 4294967295u}) {
          const PhonePathView v = path(st, h, ms, 7);
          char longText[112], shortText[22];
          const size_t n = phonePathText(v, longText, sizeof(longText));
          const size_t m = phonePathShortText(v, shortText, sizeof(shortText));
          CHECK(n == std::strlen(longText) && n > 0 && n < sizeof(longText) - 1);
          CHECK(m == std::strlen(shortText) && m > 0 && m <= 21);
          for (const char* c = longText; *c; ++c) CHECK(*c >= 0x20 && *c < 0x7f);
          for (const char* c = shortText; *c; ++c) CHECK(*c >= 0x20 && *c < 0x7f);
          if (ms == 12001u && st == PhonePathState::Retrying)
            std::printf("PATHTEXT\t%d\t%d\t%s\t%s\n", int(st), int(h), longText, shortText);
          if (ms == 0u && st != PhonePathState::Retrying)
            std::printf("PATHTEXT\t%d\t%d\t%s\t%s\n", int(st), int(h), longText, shortText);
        }
    // A small buffer is cut, never overrun; a zero-size one writes nothing.
    char tiny[8];
    CHECK(phonePathText(path(PhonePathState::RelayOff), tiny, sizeof(tiny)) == 7 && std::strcmp(tiny, "relay o") == 0);
    CHECK(phonePathText(path(PhonePathState::RelayOff), tiny, 0) == 0);
    CHECK(phonePathRecoverable(path(PhonePathState::Connecting)) && phonePathRecoverable(path(PhonePathState::Retrying)));
    CHECK(!phonePathRecoverable(path(PhonePathState::Held)) && !phonePathRecoverable(path(PhonePathState::RelayOff)) &&
          !phonePathRecoverable(path(PhonePathState::Up)));
  }
  // A text nobody bounded is refused, never cut.
  const std::string huge(700, 'x');
  set(true, true, false, PipeStep::Idle, huge.c_str());
  CHECK(std::strcmp(phoneStatusCommand(String("")), "Error: phone status too large") == 0);
  CHECK(std::strcmp(phoneStatusCommand(String("json")), "{\"error\":\"status too large\"}") == 0);
  std::printf("CHECKS\t%u\n", checks);
  return 0;
}
