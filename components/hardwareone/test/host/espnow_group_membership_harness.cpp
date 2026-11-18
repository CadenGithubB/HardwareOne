// Host harness for test_espnow_group_membership.py.
//
// The runner pastes the production types (EspNowInlineText, EspNowDevice,
// MeshPeerMeta, EspNowGroupView, V4PayloadMetadata) at INSERT_TYPES and the
// production membership code at INSERT_PRODUCTION: the record glue with its
// lock and the stored fields' writer and reader, the metadata inbox, the
// announcement functions with the report limit and the save gap, the
// acceptance functions, the metadata refresh tick, processMetadata,
// addEspNowDevice, removeEspNowDevice, resolveDeviceNameOrMac,
// countMeshPeerMetaByRoom, the listing marks with espnowSerializeMeshPeerMeta,
// cmd_espnow_devices and cmd_espnow_rooms, and the commands (espnowroomcmd, espnowtagcmd,
// espnowgroups, espnowgroupaccept, espnowgroupdrop). The rules header
// System_ESPNow_GroupCore.h, System_MacText.h and System_CommandReply.h are
// compiled as they are, ArduinoJson is the vendored library. Everything else
// is a small double: the registry state and its save, the mesh cache, the
// sessions, the encrypted send, the metadata request, the pending-frame
// queue, the mutex, events, logging and the buffers. No radio or flash is
// touched.
#include <Arduino.h>
#include <ArduinoJson.h>

#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <strings.h>
#include <vector>

#include "System_CommandReply.h"
#include "System_ESPNow_GroupCore.h"
#include "System_MacText.h"

void webBatchTestStringAppend() {}

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond, ...)                                              \
  do {                                                                \
    ++gChecks;                                                        \
    if (!(cond)) {                                                    \
      ++gFailures;                                                    \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                \
      std::printf(__VA_ARGS__);                                       \
      std::printf("\n");                                              \
    }                                                                 \
  } while (0)

#define PSRAM_JSON_DOC(name) JsonDocument name
#define EXT_RAM_BSS_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (false)
#define MESH_PEER_MAX 16
static constexpr size_t CMD_RESULT_MAX = 4096;

// ---- The membership lock: a mutex that one task must never take twice, and
// that is given back before anything is logged, saved or sent. gOnLockTake
// stands for the other task getting in just before a critical section.
struct StaticSemaphore_t {};
using SemaphoreHandle_t = int*;
static constexpr uint32_t portMAX_DELAY = 0xffffffffu;
static int gLockToken = 0;
static int gLockDepth = 0;
static int gLockTakes = 0;
static std::function<void()> gOnLockTake;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*) { return &gLockToken; }
static void outsideLock(const char* what) {
  if (gLockDepth == 0) return;
  ++gFailures;
  std::printf("FAIL: %s while the membership lock is held\n", what);
}
static bool xSemaphoreTake(SemaphoreHandle_t, uint32_t) {
  if (gOnLockTake) {
    auto other = gOnLockTake;
    gOnLockTake = nullptr;
    other();
  }
  outsideLock("the lock is taken a second time");
  ++gLockDepth;
  ++gLockTakes;
  return true;
}
static void xSemaphoreGive(SemaphoreHandle_t) { --gLockDepth; }

// ---- Logging and events: every line is kept so the cases can count them.
static std::vector<std::string> gEvents;  // logSystemEvent lines
static std::vector<std::string> gInfos;
static std::vector<std::string> gWarns;
__attribute__((format(printf, 2, 3)))
static void keepLine(std::vector<std::string>* into, const char* fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  outsideLock("a log line is written");
  if (into) into->push_back(line);
}
#define INFO_ESPNOWF(...) keepLine(&gInfos, __VA_ARGS__)
#define WARN_ESPNOWF(...) keepLine(&gWarns, __VA_ARGS__)
#define DEBUG_ESPNOW_METADATAF(...) keepLine(nullptr, __VA_ARGS__)
__attribute__((format(printf, 2, 3)))
static void logSystemEvent(const char* category, const char* fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  // The firmware cuts an event line at 255 bytes with its lead.
  CHECK(std::strlen(line) + std::strlen("[EVENT][] ") + std::strlen(category) < 256, "event line too long: %s", line);
  outsideLock("an event line is written");
  gEvents.push_back(line);
}
static const char* MAC_STR(const uint8_t* m) {
  static char buf[18];
  macToDisplay(m, buf, sizeof(buf));
  return buf;
}
static std::vector<std::string> gHints;
static void cliHint(const char* text) { gHints.push_back(text); }
static void emitListingTrailer(const char*, const char*, const char* = nullptr) {}

static uint32_t gNowMs = 1000;
static uint32_t millis() { return gNowMs; }

// INSERT_TYPES

// ---- Registry state: the production device record in a small state double.
struct EspNowState {
  bool initialized = true;
  bool encryptionEnabled = true;  // the group commands carry a login: encrypted only
  EspNowDevice devices[16];
  int deviceCount = 0;
};
static EspNowState gState;
static EspNowState* gEspNow = &gState;
static constexpr int kMetaSlots = 16;
static MeshPeerMeta gMetaSlots[kMetaSlots];
static MeshPeerMeta* gMeshPeerMeta = gMetaSlots;
static int gMeshPeerSlots = kMetaSlots;  // a case may lower it: the cache is smaller than the registry

static const uint8_t kSelf[6] = {0x02, 0xAA, 0, 0, 0, 0x01};
static bool isSelfMac(const uint8_t* mac) { return std::memcmp(mac, kSelf, 6) == 0; }

static int gSaves = 0;
static bool gSaveFails = false;
static bool saveEspNowDevices() {
  outsideLock("the registry is saved");
  ++gSaves;
  return !gSaveFails;
}

// Frames waiting for a session with a peer: only the drop is seen here.
static std::vector<std::string> gPendingDrops;
static uint8_t pendingFrameDropForPeer(const uint8_t mac[6]) {
  outsideLock("waiting frames are dropped");
  gPendingDrops.push_back(MAC_STR(mac));
  return 1;
}

// processMetadata asks for this when the cache is full. Every slot here is a
// paired peer's, and those are never taken back.
[[maybe_unused]] static int meshPeerMetaReclaimSlot(const uint8_t*) { return -1; }

// ---- Sessions: only what the metadata refresh reads.
enum : uint8_t { SESSION_NONE = 0, SESSION_ACTIVE = 2, SESSION_REKEYING = 3 };
struct SessionState {
  uint8_t peerMac[6];
  uint32_t establishedAtMs;
  uint8_t state;
};
static std::vector<SessionState> gSessions;
static uint8_t sessionSlotCount() { return static_cast<uint8_t>(gSessions.size()); }
static const SessionState* sessionAt(uint8_t i) { return i < gSessions.size() ? &gSessions[i] : nullptr; }
// The metadata request this device sends: who was asked.
static std::vector<std::string> gMetaRequests;
static void requestMetadata(const uint8_t* peerMac, bool force) {
  outsideLock("a metadata request is sent");
  if (!force) {
    ++gFailures;
    std::printf("FAIL: the refresh request was not forced past the manual request's cooldown\n");
  }
  char text[18];
  macToDisplay(peerMac, text, sizeof(text));
  gMetaRequests.push_back(text);
}

// Peer health: every peer with a metadata slot counts as heard just now.
struct MeshPeerHealth {
  uint32_t lastMeshHeartbeatMs;
  uint32_t lastRxActivityMs;
};
static MeshPeerHealth gHealth = {900, 950};
static MeshPeerHealth* getMeshPeerHealth(const uint8_t*, bool) {
  gHealth.lastMeshHeartbeatMs = gNowMs;
  gHealth.lastRxActivityMs = gNowMs;
  return &gHealth;
}
static bool isMeshPeerAlive(const MeshPeerHealth*) { return true; }

// ---- The encrypted send: destination and plaintext of every frame handed over.
struct SentFrame {
  std::string dst;
  std::string payload;
  uint8_t type;
  uint16_t flags;
};
static std::vector<SentFrame> gSent;
enum : uint8_t { ESPNOW_V4_TYPE_CMD = 50 };
enum : uint16_t { ESPNOW_V4_FLAG_ACK_REQ = 0x0001 };
// INSERT_WIRE
// A destination whose frame is refused (no room to park it), as the real send
// says with its status text.
static std::string gRefusedDst;
// Destinations whose frame is parked until their session opens (status
// "queued", as the real send says it).
static std::vector<std::string> gQueuedDst;
static bool v4_send_encrypted_or_queue(const uint8_t dst[6], uint8_t type, uint16_t flags, uint32_t,
                                       const uint8_t* plaintext, uint16_t plaintextLen, uint8_t, char* status,
                                       size_t statusLen) {
  outsideLock("a frame is sent");
  if (!gRefusedDst.empty() && gRefusedDst == MAC_STR(dst)) {
    if (status && statusLen) std::snprintf(status, statusLen, "4 messages already wait for this device's session");
    return false;
  }
  bool queued = false;
  for (const auto& q : gQueuedDst) queued = queued || q == MAC_STR(dst);
  if (status && statusLen) std::snprintf(status, statusLen, "%s", queued ? "queued" : "sent");
  gSent.push_back({MAC_STR(dst), std::string(reinterpret_cast<const char*>(plaintext), plaintextLen), type, flags});
  return true;
}
static uint32_t generateMessageId() {
  static uint32_t next = 100;
  return ++next;
}
// The audit form of a forwarded command: the real redactor is run by
// test_redact_audit_nesting.py; here it only has to be told from the raw text.
static String redactCmdForAudit(const String& command) { return command; }

// ---- Command plumbing.
static constexpr size_t GLOBAL_DEBUG_BUFFER_SIZE = 4096;
static char gDebugBuffer[GLOBAL_DEBUG_BUFFER_SIZE];
static char* getDebugBuffer() { return gDebugBuffer; }
static bool ensureDebugBuffer() { return true; }
enum class AllocPref : uint8_t { PreferPsram = 0 };
static int gAllocs = 0;
static void* ps_alloc(size_t size, AllocPref, const char*) {
  ++gAllocs;
  return std::malloc(size);
}

// Space-separated arguments with double-quoted ones taken whole, and the text
// after an argument, as the firmware's CommandArgs gives them.
class CommandArgs {
 public:
  explicit CommandArgs(const String& input) : raw_(input.c_str()) {
    size_t i = 0;
    while (i < raw_.size()) {
      while (i < raw_.size() && raw_[i] == ' ') ++i;
      if (i >= raw_.size()) break;
      std::string token;
      if (raw_[i] == '"') {
        const size_t close = raw_.find('"', i + 1);
        const size_t end = close == std::string::npos ? raw_.size() : close;
        token = raw_.substr(i + 1, end - i - 1);
        i = close == std::string::npos ? raw_.size() : close + 1;
      } else {
        const size_t end = raw_.find(' ', i);
        token = raw_.substr(i, end == std::string::npos ? std::string::npos : end - i);
        i = end == std::string::npos ? raw_.size() : end;
      }
      args_.push_back(String(token.c_str()));
      ends_.push_back(i);
    }
  }
  const String& arg(int index) const {
    static const String empty;
    return (index >= 0 && index < (int)args_.size()) ? args_[(size_t)index] : empty;
  }
  int count() const { return (int)args_.size(); }
  bool hasMinArgs(int n) const { return count() >= n; }
  String remaining(int afterIndex) const {
    if (afterIndex < 0) return String(raw_.c_str());
    if (afterIndex >= count()) return String("");
    size_t i = ends_[(size_t)afterIndex];
    while (i < raw_.size() && raw_[i] == ' ') ++i;
    return String(raw_.substr(i).c_str());
  }

 private:
  std::string raw_;
  std::vector<String> args_;
  std::vector<size_t> ends_;
};
static std::string firstToken(const String& args) {
  std::string s = args.c_str();
  const size_t start = s.find_first_not_of(' ');
  if (start == std::string::npos) return "";
  const size_t end = s.find(' ', start);
  return s.substr(start, end == std::string::npos ? std::string::npos : end - start);
}
static bool argLeadingTokenIsJson(const String& args) { return firstToken(args) == "json"; }
static bool argWantsJson(const String& args) {
  std::string s = std::string(" ") + args.c_str() + " ";
  return s.find(" json ") != std::string::npos;
}
static bool parseMacAddress(const String& text, uint8_t mac[6]) { return macParse(text.c_str(), mac); }

// INSERT_PRODUCTION

// ---- Test helpers.
static uint8_t* macOf(uint8_t last) {
  static uint8_t macs[32][6];
  uint8_t* m = macs[last % 32];
  const uint8_t value[6] = {0x02, 0xBB, 0, 0, 0, last};
  std::memcpy(m, value, 6);
  return m;
}
static std::string macText(uint8_t last) { return MAC_STR(macOf(last)); }

static void resetAll() {
  for (auto& d : gState.devices) d = EspNowDevice{};
  gState.deviceCount = 0;
  gState.initialized = true;
  gState.encryptionEnabled = true;
  gRefusedDst.clear();
  gQueuedDst.clear();
  for (auto& m : gMetaSlots) m.clear();
  gMeshPeerSlots = kMetaSlots;
  gSessions.clear();
  gMetaRequests.clear();
  gOnLockTake = nullptr;
  // The production statics of the report limit, the save gap, the inbox and
  // the refresh, as after a restart.
  std::memset(sGroupNotice, 0, sizeof(sGroupNotice));
  sAnnounceSaveDue = false;
  sAnnounceSaved = false;
  sAnnounceSavedMs = 0;
  if (sMetadataInbox) std::memset(sMetadataInbox, 0, kMetadataInboxSlots * sizeof(MetadataInboxEntry));
  sMetadataInboxNext = 0;
  sMetadataRefreshLookMs = 0;
  gEvents.clear();
  gInfos.clear();
  gWarns.clear();
  gHints.clear();
  gSent.clear();
  gPendingDrops.clear();
  gSaves = 0;
  gSaveFails = false;
  // This device has an entry of its own in the registry, as on a device.
  addEspNowDevice(kSelf, String("self"), false, nullptr);
}
static EspNowDevice* pair(uint8_t last, const char* name) {
  addEspNowDevice(macOf(last), String(name), true, nullptr);
  return &gState.devices[gState.deviceCount - 1];
}
static EspNowDevice* device(uint8_t last) {
  for (int i = 0; i < gState.deviceCount; ++i) {
    if (std::memcmp(gState.devices[i].mac, macOf(last), 6) == 0) return &gState.devices[i];
  }
  return nullptr;
}
// A metadata frame from a paired peer, as espnow_task hands it to
// processMetadata, at the present time.
static void announceNow(uint8_t last, const char* room, const char* tags, const char* deviceName = "") {
  V4PayloadMetadata p;
  std::memset(&p, 0, sizeof(p));
  std::snprintf(p.deviceName, sizeof(p.deviceName), "%s", deviceName);
  std::snprintf(p.room, sizeof(p.room), "%s", room);
  std::snprintf(p.tags, sizeof(p.tags), "%s", tags);
  processMetadata(macOf(last), &p);
}
// The same more than a minute after whatever came before: the usual case of a
// peer that changes its room now and then, which the report limit and the save
// gap leave alone. The cases for those two use announceNow.
static void announce(uint8_t last, const char* room, const char* tags, const char* deviceName = "") {
  gNowMs += espnowgroup::kNoticeGapMs + 1000;
  announceNow(last, room, tags, deviceName);
}
// The loop steps of espnow_task that belong to membership.
static void tick() {
  groupNoticeTick(gNowMs);
  announceSaveTick(gNowMs);
}
static std::string run(const char* (*handler)(const String&), const char* args) {
  gSent.clear();
  gHints.clear();
  const char* reply = handler(String(args));
  return reply ? reply : "(null)";
}
static bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
static bool sentTo(uint8_t last) {
  for (const auto& f : gSent) {
    if (f.dst == macText(last)) return true;
  }
  return false;
}
static int eventsWith(const char* part) {
  int n = 0;
  for (const auto& e : gEvents) n += has(e, part) ? 1 : 0;
  return n;
}

// G1: a peer that newly says it is in a room gets no room command until accepted.
static void testNewClaimNotTargeted() {
  resetAll();
  pair(1, "Fridge");
  announce(1, "Kitchen", "");
  CHECK(device(1)->room == "Kitchen" && device(1)->acceptedRoom == "", "G1: registry room '%s' accepted '%s'",
        device(1)->room.c_str(), device(1)->acceptedRoom.c_str());
  CHECK(gEvents.size() == 1 &&
            gEvents[0] == "Fridge says it is in room 'Kitchen'; not used for room commands until accepted here: "
                          "espnowgroupaccept Fridge",
        "G1: notice lines %zu, first '%s'", gEvents.size(), gEvents.empty() ? "" : gEvents[0].c_str());
  CHECK(gSaves == 1, "G1: the announcement was saved %d times, expected 1", gSaves);

  std::string reply = run(cmd_espnow_roomcmd, "Kitchen admin secret status");
  CHECK(gSent.empty(), "G1: %zu frame(s) went to a room nobody was accepted into", gSent.size());
  CHECK(has(reply, "No devices found in room 'Kitchen'"), "G1: reply '%s'", reply.c_str());
  CHECK(has(reply, "Skipped 1 device(s) whose room is pending (announced, not accepted here): Fridge. "
                   "Accept with 'espnowgroupaccept <name_or_mac|all>'"),
        "G1: reply does not count the pending peer: '%s'", reply.c_str());
  CHECK(!has(reply, "secret"), "G1: reply carries the password: '%s'", reply.c_str());
  CHECK(countMeshPeerMetaByRoom("Kitchen") == 0, "G1: the room counts a pending peer");

  // The same frame again and again: not another line, not another save.
  announce(1, "Kitchen", "");
  announce(1, "Kitchen", "");
  CHECK(gEvents.size() == 1 && gSaves == 1, "G1: repeats printed %zu lines and saved %d times", gEvents.size(),
        gSaves);
}

// G2: an accepted peer is targeted; a pending one beside it is skipped and counted.
static void testAcceptedTargeted() {
  resetAll();
  pair(1, "Fridge");
  pair(2, "Lamp");
  pair(3, "Hall");
  announce(1, "Kitchen", "");
  announce(2, "kitchen", "");
  announce(3, "Hallway", "");
  std::string accepted = run(cmd_espnow_groupaccept, "Lamp");
  CHECK(has(accepted, "Lamp (" ) && has(accepted, "): accepted room 'kitchen'"), "G2: accept reply '%s'",
        accepted.c_str());
  CHECK(device(2)->acceptedRoom == "kitchen" && device(1)->acceptedRoom == "", "G2: accept touched another peer");
  CHECK(gSaves == 4, "G2: saves %d, expected 3 announcements and 1 accept", gSaves);

  std::string reply = run(cmd_espnow_roomcmd, "Kitchen admin secret lightcolor red");
  CHECK(gSent.size() == 1 && sentTo(2) && !sentTo(1) && !sentTo(3), "G2: %zu frame(s), to Lamp=%d Fridge=%d",
        gSent.size(), (int)sentTo(2), (int)sentTo(1));
  CHECK(!gSent.empty() && gSent[0].payload == "admin:secret:lightcolor red" && gSent[0].type == ESPNOW_V4_TYPE_CMD &&
            gSent[0].flags == ESPNOW_V4_FLAG_ACK_REQ,
        "G2: frame payload '%s'", gSent.empty() ? "" : gSent[0].payload.c_str());
  CHECK(has(reply, "  -> Lamp (") && has(reply, "Sent 'lightcolor red' to 1 device(s) in Kitchen"), "G2: reply '%s'",
        reply.c_str());
  CHECK(has(reply, "Skipped 1 device(s) whose room is pending (announced, not accepted here): Fridge."),
        "G2: reply '%s'", reply.c_str());
  CHECK(countMeshPeerMetaByRoom("KITCHEN") == 1 && countMeshPeerMetaByRoom("Hallway") == 0 &&
            countMeshPeerMetaByRoom("") == 0 && countMeshPeerMetaByRoom(nullptr) == 0,
        "G2: room counts %d/%d", countMeshPeerMetaByRoom("KITCHEN"), countMeshPeerMetaByRoom("Hallway"));
  CHECK(espnowGroupRoomAccepted(macOf(2), "Kitchen") && !espnowGroupRoomAccepted(macOf(1), "Kitchen") &&
            !espnowGroupRoomAccepted(macOf(9), "Kitchen") && !espnowGroupRoomAccepted(kSelf, "Kitchen"),
        "G2: espnowGroupRoomAccepted disagrees with the accepted set");
}

// G3: a change of room is out of the old one at once and pending for the new.
static void testRoomChange() {
  resetAll();
  pair(1, "Mover");
  announce(1, "Garage", "");
  run(cmd_espnow_groupaccept, "Mover");
  CHECK(run(cmd_espnow_roomcmd, "Garage u p status").find("Sent 'status' to 1 device(s) in Garage") !=
            std::string::npos && sentTo(1),
        "G3: the accepted peer was not targeted in Garage");
  gEvents.clear();

  announce(1, "Kitchen", "");
  CHECK(device(1)->room == "Kitchen" && device(1)->acceptedRoom == "", "G3: after the move accepted is '%s'",
        device(1)->acceptedRoom.c_str());
  std::string garage = run(cmd_espnow_roomcmd, "Garage u p status");
  CHECK(gSent.empty() && has(garage, "No devices found in room 'Garage'") && !has(garage, "Skipped"),
        "G3: Garage after the move: %zu frame(s), reply '%s'", gSent.size(), garage.c_str());
  std::string kitchen = run(cmd_espnow_roomcmd, "Kitchen u p status");
  CHECK(gSent.empty() && has(kitchen, "Skipped 1 device(s) whose room is pending") && has(kitchen, "Mover"),
        "G3: Kitchen before accept: %zu frame(s), reply '%s'", gSent.size(), kitchen.c_str());
  CHECK(gEvents.size() == 1 && has(gEvents[0], "Mover says it is in room 'Kitchen'"), "G3: %zu notice line(s)",
        gEvents.size());
  CHECK(gInfos.size() == 1 && has(gInfos[0], "Mover no longer says it is in room 'Garage'"), "G3: leave line '%s'",
        gInfos.empty() ? "" : gInfos[0].c_str());
  announce(1, "Kitchen", "");
  CHECK(gEvents.size() == 1, "G3: the repeated frame printed the notice again");

  // The peer clears its room: out, nothing pending, no line.
  run(cmd_espnow_groupaccept, "Mover");
  gEvents.clear();
  announce(1, "", "");
  CHECK(device(1)->room == "" && device(1)->acceptedRoom == "", "G3: a cleared room stays '%s' / accepted '%s'",
        device(1)->room.c_str(), device(1)->acceptedRoom.c_str());
  std::string cleared = run(cmd_espnow_roomcmd, "Kitchen u p status");
  CHECK(gSent.empty() && !has(cleared, "Skipped") && gEvents.empty(), "G3: after clearing: reply '%s', %zu event(s)",
        cleared.c_str(), gEvents.size());

  // Back to the room it was accepted for earlier: pending again, said once.
  announce(1, "Kitchen", "");
  CHECK(gEvents.size() == 1 && run(cmd_espnow_roomcmd, "Kitchen u p status").find("Skipped 1") != std::string::npos &&
            gSent.empty(),
        "G3: coming back to a room is not pending again");
}

// G4: tags are accepted one by one.
static void testTagsPerTag() {
  resetAll();
  pair(1, "Lamp");
  pair(2, "Fan");
  announce(1, "", "light,dimmer");
  announce(2, "", "air");
  CHECK(eventsWith("Lamp says it has tags 'light,dimmer'; not used for tag commands until accepted here: "
                   "espnowgroupaccept Lamp") == 1 &&
            eventsWith("Fan says it has tag 'air'") == 1 && gEvents.size() == 2,
        "G4: %zu notice line(s)", gEvents.size());
  std::string before = run(cmd_espnow_tagcmd, "light u p status");
  CHECK(gSent.empty() && has(before, "No devices found with tag 'light'") &&
            has(before, "Skipped 1 device(s) whose tag is pending (announced, not accepted here): Lamp."),
        "G4: before accept: %zu frame(s), reply '%s'", gSent.size(), before.c_str());

  run(cmd_espnow_groupaccept, "Lamp");
  CHECK(device(1)->acceptedTags == "light,dimmer" && device(2)->acceptedTags == "", "G4: accepted tags '%s'",
        device(1)->acceptedTags.c_str());
  std::string light = run(cmd_espnow_tagcmd, "LIGHT u p status");
  CHECK(gSent.size() == 1 && sentTo(1) && gSent[0].payload == "u:p:status" &&
            has(light, "Sent 'status' to 1 device(s) with tag 'LIGHT'") && !has(light, "Skipped"),
        "G4: accepted tag: %zu frame(s), reply '%s'", gSent.size(), light.c_str());

  // One more tag announced: the accepted ones keep working, the new one waits.
  gEvents.clear();
  announce(1, "", "light,dimmer,admin");
  CHECK(gEvents.size() == 1 && has(gEvents[0], "Lamp says it has tag 'admin'"), "G4: line for the added tag: %zu",
        gEvents.size());
  CHECK(run(cmd_espnow_tagcmd, "dimmer u p x").find("to 1 device(s)") != std::string::npos && sentTo(1),
        "G4: an accepted tag stopped working when another was added");
  std::string admin = run(cmd_espnow_tagcmd, "admin u p x");
  CHECK(gSent.empty() && has(admin, "Skipped 1 device(s) whose tag is pending"), "G4: new tag used unaccepted: '%s'",
        admin.c_str());

  // An accepted tag dropped by the peer leaves at once; the others stay.
  gEvents.clear();
  announce(1, "", "light,admin");
  CHECK(device(1)->acceptedTags == "light" && gEvents.empty(), "G4: after dropping dimmer accepted is '%s'",
        device(1)->acceptedTags.c_str());
  CHECK(run(cmd_espnow_tagcmd, "dimmer u p x").find("No devices found") != std::string::npos && gSent.empty(),
        "G4: a tag the peer dropped is still targeted");
  CHECK(run(cmd_espnow_tagcmd, "light u p x").find("to 1 device(s)") != std::string::npos,
        "G4: the kept tag stopped working");
  // The dropped tag announced again waits again.
  announce(1, "", "light,admin,dimmer");
  CHECK(gEvents.size() == 1 && has(gEvents[0], "tag 'dimmer'") && device(1)->acceptedTags == "light",
        "G4: a tag that came back was taken from memory: accepted '%s'", device(1)->acceptedTags.c_str());
}

// G5: accepting records what is announced at that moment, and the reply says what.
static void testAcceptRecordsAnnounced() {
  resetAll();
  pair(1, "Lamp");
  announce(1, "Garage", "tools");
  std::string list = run(cmd_espnow_groups, "");
  CHECK(has(list, "Lamp (") && has(list, "  announced: room 'Garage', tag 'tools'\n") &&
            has(list, "  accepted:  nothing\n") && has(list, "  pending:   room 'Garage', tag 'tools'\n") &&
            has(list, "1 peer(s), 1 with pending room or tags."),
        "G5: list '%s'", list.c_str());
  CHECK(!has(list, "self"), "G5: the list shows this device itself: '%s'", list.c_str());

  // The announcement changes between the list and the accept.
  announce(1, "Kitchen", "light,dimmer");
  gEvents.clear();
  std::string reply = run(cmd_espnow_groupaccept, "lamp");
  CHECK(has(reply, "): accepted room 'Kitchen', tags 'light,dimmer'\n") && has(reply, "1 peer(s), 1 changed."),
        "G5: accept reply '%s'", reply.c_str());
  CHECK(device(1)->acceptedRoom == "Kitchen" && device(1)->acceptedTags == "light,dimmer",
        "G5: accepted '%s' / '%s'", device(1)->acceptedRoom.c_str(), device(1)->acceptedTags.c_str());
  CHECK(eventsWith("Lamp: accepted for group commands: room 'Kitchen', tags 'light,dimmer'") == 1,
        "G5: no event line for the accept");
  const int saves = gSaves;
  std::string again = run(cmd_espnow_groupaccept, macText(1).c_str());
  CHECK(has(again, "accepted room 'Kitchen', tags 'light,dimmer' (as before)") && has(again, "1 peer(s), 0 changed.") &&
            gSaves == saves,
        "G5: accepting again: '%s' (saves %d -> %d)", again.c_str(), saves, gSaves);

  list = run(cmd_espnow_groups, "Lamp");
  CHECK(has(list, "  accepted:  room 'Kitchen', tags 'light,dimmer'\n") && has(list, "  pending:   nothing\n") &&
            has(list, "1 peer(s), 0 with pending room or tags."),
        "G5: list after accept '%s'", list.c_str());

  // JSON forms.
  announce(1, "Kitchen", "light,dimmer,heater");
  JsonDocument doc;
  std::string json = run(cmd_espnow_groups, "json");
  CHECK(!deserializeJson(doc, json), "G5: espnowgroups json does not parse: '%s'", json.c_str());
  JsonObject peer = doc["peers"][0];
  CHECK(doc["schema"] == 1 && doc["count"] == 1 && doc["pendingCount"] == 1 && doc["peers"].size() == 1 &&
            peer["name"] == "Lamp" && peer["mac"] == macText(1).c_str() && peer["room"] == "Kitchen" &&
            peer["tags"] == "light,dimmer,heater" && peer["acceptedRoom"] == "Kitchen" &&
            peer["acceptedTags"] == "light,dimmer" && peer["pendingRoom"] == "" && peer["pendingTags"] == "heater",
        "G5: espnowgroups json '%s'", json.c_str());
  json = run(cmd_espnow_groupaccept, "json all");
  CHECK(!deserializeJson(doc, json), "G5: espnowgroupaccept json does not parse: '%s'", json.c_str());
  peer = doc["peers"][0];
  CHECK(doc["ok"] == true && doc["saved"] == true && doc["count"] == 1 && doc["changed"] == 1 &&
            peer["changed"] == true && peer["acceptedTags"] == "light,dimmer,heater" && peer["pendingTags"] == "",
        "G5: espnowgroupaccept json '%s'", json.c_str());

  // Refusals.
  CHECK(has(run(cmd_espnow_groupaccept, ""), "Error: invalid arguments") &&
            has(run(cmd_espnow_groupaccept, "json"), "Error: invalid arguments") &&
            has(run(cmd_espnow_groupdrop, ""), "Error: invalid arguments"),
        "G5: a missing peer is not refused");
  CHECK(commandReplyIsFailure(run(cmd_espnow_groupaccept, "Nobody").c_str()) &&
            commandReplyIsFailure(run(cmd_espnow_groups, "Nobody").c_str()),
        "G5: an unknown peer is not refused");
  CHECK(has(run(cmd_espnow_groupaccept, MAC_STR(kSelf)), "Error: That is this device"),
        "G5: this device itself is not refused");
  CHECK(gAllocs == 1, "G5: the reply block was made %d times, expected once", gAllocs);
}

// G6: 'all', drop, and a save that fails.
static void testAllDropAndSave() {
  resetAll();
  pair(1, "Lamp");
  pair(2, "Fridge");
  pair(3, "Quiet");
  announce(1, "Kitchen", "light");
  announce(2, "Kitchen", "");
  const int saves = gSaves;
  std::string reply = run(cmd_espnow_groupaccept, "all");
  CHECK(has(reply, "): accepted room 'Kitchen', tag 'light'\n") && has(reply, "): accepted room 'Kitchen'\n") &&
            has(reply, "): announces no room or tags; accepted nothing\n") && has(reply, "3 peer(s), 2 changed."),
        "G6: accept all '%s'", reply.c_str());
  CHECK(gSaves == saves + 1, "G6: accept all saved %d times, expected once", gSaves - saves);
  CHECK(run(cmd_espnow_roomcmd, "Kitchen u p x").find("to 2 device(s)") != std::string::npos && sentTo(1) && sentTo(2),
        "G6: both accepted peers are not targeted");

  gEvents.clear();
  reply = run(cmd_espnow_groupdrop, "Fridge");
  CHECK(has(reply, "): no longer accepted: room 'Kitchen'\n") && has(reply, "1 peer(s), 1 changed.") &&
            has(reply, "espnowgroupaccept <name_or_mac|all>"),
        "G6: drop '%s'", reply.c_str());
  CHECK(device(2)->acceptedRoom == "" && device(2)->room == "Kitchen" && device(1)->acceptedRoom == "Kitchen",
        "G6: drop left accepted '%s'", device(2)->acceptedRoom.c_str());
  CHECK(eventsWith("Fridge: no longer accepted for group commands: room 'Kitchen'") == 1, "G6: no event for the drop");
  std::string after = run(cmd_espnow_roomcmd, "Kitchen u p x");
  CHECK(gSent.size() == 1 && sentTo(1) && has(after, "Skipped 1 device(s) whose room is pending") &&
            has(after, "Fridge"),
        "G6: after the drop: %zu frame(s), '%s'", gSent.size(), after.c_str());
  // What it still announces is pending, without a new line: the admin did this.
  gEvents.clear();
  announce(2, "Kitchen", "");
  CHECK(gEvents.empty(), "G6: a dropped peer's unchanged announcement printed a notice");
  reply = run(cmd_espnow_groupdrop, "Fridge");
  CHECK(has(reply, "nothing was accepted; no change") && has(reply, "1 peer(s), 0 changed."), "G6: second drop '%s'",
        reply.c_str());
  reply = run(cmd_espnow_groupdrop, "all");
  CHECK(has(reply, "3 peer(s), 1 changed.") && device(1)->acceptedRoom == "" && device(1)->acceptedTags == "",
        "G6: drop all '%s'", reply.c_str());

  // The registry cannot be saved: said in the reply, in words and in JSON.
  gSaveFails = true;
  reply = run(cmd_espnow_groupaccept, "Lamp");
  CHECK(has(reply, "The device registry could not be saved") && device(1)->acceptedRoom == "Kitchen",
        "G6: failed save not reported: '%s'", reply.c_str());
  std::string json = run(cmd_espnow_groupdrop, "json Lamp");
  JsonDocument doc;
  CHECK(!deserializeJson(doc, json) && doc["ok"] == false && doc["saved"] == false && doc["changed"] == 1 &&
            commandReplyJsonFailed(json.c_str()),
        "G6: failed save in JSON: '%s'", json.c_str());
}

// G7: unpairing removes the accepted state; an identity change clears it.
static void testUnpairAndIdentity() {
  resetAll();
  pair(1, "Lamp");
  pair(2, "Fridge");
  announce(1, "Kitchen", "light");
  announce(2, "Kitchen", "");
  run(cmd_espnow_groupaccept, "all");

  // Unpair (the registry part of espnowunpair), then the same address pairs again.
  removeEspNowDevice(macOf(1));
  CHECK(device(1) == nullptr && device(2) != nullptr && device(2)->acceptedRoom == "Kitchen",
        "G7: removing one record disturbed another");
  CHECK(gState.devices[gState.deviceCount].acceptedRoom == "" && gState.devices[gState.deviceCount].acceptedTags == "",
        "G7: the freed registry slot keeps accepted text");
  EspNowDevice* again = pair(1, "Lamp");
  CHECK(again->acceptedRoom == "" && again->acceptedTags == "" && again->room == "",
        "G7: a re-paired address inherited '%s' / '%s'", again->acceptedRoom.c_str(), again->acceptedTags.c_str());
  gEvents.clear();
  announce(1, "Kitchen", "light");
  CHECK(gEvents.size() == 2, "G7: the re-paired peer's room and tags gave %zu notice line(s), expected 2",
        gEvents.size());
  std::string reply = run(cmd_espnow_roomcmd, "Kitchen u p x");
  CHECK(gSent.size() == 1 && sentTo(2) && !sentTo(1) && has(reply, "Skipped 1"), "G7: re-paired peer targeted: '%s'",
        reply.c_str());

  // Another key at an accepted peer's address: nothing stays accepted, frames
  // waiting for a session with it are dropped, and it is saved and said once.
  gEvents.clear();
  gPendingDrops.clear();
  int saves = gSaves;
  espnowGroupAcceptanceClear(macOf(2), "its identity key is new");
  CHECK(device(2)->acceptedRoom == "" && device(2)->room == "Kitchen", "G7: identity change kept accepted '%s'",
        device(2)->acceptedRoom.c_str());
  CHECK(gPendingDrops.size() == 1 && gPendingDrops[0] == macText(2), "G7: waiting frames dropped %zu time(s)",
        gPendingDrops.size());
  CHECK(gSaves == saves + 1 && gEvents.size() == 1 &&
            gEvents[0] == "Fridge: accepted room and tags cleared (its identity key is new); accept them again with "
                          "'espnowgroupaccept Fridge'",
        "G7: clear saved %d time(s), line '%s'", gSaves - saves, gEvents.empty() ? "" : gEvents[0].c_str());
  CHECK(run(cmd_espnow_roomcmd, "Kitchen u p x").find("Skipped 2") != std::string::npos && gSent.empty(),
        "G7: a peer with a new identity is still targeted");

  // Nothing accepted (a first pairing), an unknown address, this device: no effect.
  gEvents.clear();
  gPendingDrops.clear();
  saves = gSaves;
  espnowGroupAcceptanceClear(macOf(2), "again");
  espnowGroupAcceptanceClear(macOf(9), "unknown");
  espnowGroupAcceptanceClear(kSelf, "self");
  CHECK(gEvents.empty() && gPendingDrops.empty() && gSaves == saves,
        "G7: a clear with nothing accepted did something (%zu events, %zu drops, %d saves)", gEvents.size(),
        gPendingDrops.size(), gSaves - saves);
  gSaveFails = true;
  run(cmd_espnow_groupaccept, "Fridge");
  gWarns.clear();
  espnowGroupAcceptanceClear(macOf(2), "its identity key is new");
  CHECK(device(2)->acceptedRoom == "" && gWarns.size() == 1, "G7: a clear that cannot be saved: %zu warning(s)",
        gWarns.size());
}

// G8: the stored record: round trip, a record without the fields, bad values.
static void testStoredRecord() {
  resetAll();
  pair(1, "Lamp");
  announce(1, "Kitchen", "light,dimmer,heater");
  run(cmd_espnow_groupaccept, "Lamp");
  announce(1, "Kitchen", "light,dimmer,heater,fan");  // fan stays pending
  const EspNowDevice& live = *device(1);

  JsonDocument doc;
  JsonObject entry = doc.to<JsonObject>();
  entry["room"] = live.room.c_str();
  entry["tags"] = live.tags.c_str();
  groupAcceptedToJson(entry, live);
  std::string text;
  serializeJson(doc, text);
  CHECK(has(text, "\"acceptedRoom\":\"Kitchen\"") && has(text, "\"acceptedTags\":\"light,dimmer,heater\""),
        "G8: stored record '%s'", text.c_str());

  // Read back after a restart.
  JsonDocument read;
  CHECK(!deserializeJson(read, text), "G8: stored record does not parse");
  EspNowDevice loaded{};
  std::memcpy(loaded.mac, macOf(1), 6);
  loaded.room = read["room"] | "";
  loaded.tags = read["tags"] | "";
  CHECK(groupAcceptedFromJson(read.as<JsonObjectConst>(), loaded), "G8: clean stored values reported as not fitting");
  CHECK(loaded.acceptedRoom == "Kitchen" && loaded.acceptedTags == "light,dimmer,heater" &&
            loaded.room == "Kitchen" && loaded.tags == "light,dimmer,heater,fan",
        "G8: round trip gave '%s' / '%s'", loaded.acceptedRoom.c_str(), loaded.acceptedTags.c_str());

  // The restarted device hears the same announcement: no line for what waited before.
  resetAll();
  gState.devices[gState.deviceCount] = loaded;
  gState.devices[gState.deviceCount].name = "Lamp";
  gState.deviceCount++;
  announce(1, "Kitchen", "light,dimmer,heater,fan");
  CHECK(gEvents.empty() && gSaves == 0, "G8: after a restart the same announcement printed %zu line(s), saved %d",
        gEvents.size(), gSaves);
  CHECK(run(cmd_espnow_tagcmd, "heater u p x").find("to 1 device(s)") != std::string::npos &&
            run(cmd_espnow_tagcmd, "fan u p x").find("Skipped 1") != std::string::npos,
        "G8: membership after a restart differs from before it");

  // A record written before these fields existed: nothing accepted, all pending.
  JsonDocument oldDoc;
  CHECK(!deserializeJson(oldDoc, "{\"name\":\"Old\",\"room\":\"Kitchen\",\"tags\":\"light\"}"), "G8: old record");
  EspNowDevice old{};
  old.room = oldDoc["room"] | "";
  old.tags = oldDoc["tags"] | "";
  CHECK(groupAcceptedFromJson(oldDoc.as<JsonObjectConst>(), old), "G8: an old record reported as not fitting");
  CHECK(old.acceptedRoom == "" && old.acceptedTags == "" && old.room == "Kitchen" && old.tags == "light",
        "G8: an old record loaded accepted '%s' / '%s'", old.acceptedRoom.c_str(), old.acceptedTags.c_str());
  JsonDocument none;
  JsonObject noneEntry = none.to<JsonObject>();
  groupAcceptedToJson(noneEntry, old);
  CHECK(noneEntry.size() == 0, "G8: a record with nothing accepted writes the fields");

  // A file that names as accepted what its own announcement does not.
  JsonDocument editedDoc;
  CHECK(!deserializeJson(editedDoc, "{\"room\":\"Garage\",\"tags\":\"light\",\"acceptedRoom\":\"Kitchen\","
                                    "\"acceptedTags\":\"light,admin\"}"),
        "G8: edited record");
  EspNowDevice edited{};
  edited.room = editedDoc["room"] | "";
  edited.tags = editedDoc["tags"] | "";
  CHECK(groupAcceptedFromJson(editedDoc.as<JsonObjectConst>(), edited), "G8: edited record fits");
  CHECK(edited.acceptedRoom == "" && edited.acceptedTags == "light", "G8: edited record kept '%s' / '%s'",
        edited.acceptedRoom.c_str(), edited.acceptedTags.c_str());

  // Values too long for the record are dropped, and that is reported.
  JsonDocument longDoc;
  JsonObject longEntry = longDoc.to<JsonObject>();
  const std::string longRoom(40, 'r');
  longEntry["acceptedRoom"] = longRoom.c_str();
  longEntry["acceptedTags"] = "light";
  EspNowDevice tooLong{};
  tooLong.room = "Kitchen";
  tooLong.tags = "light";
  CHECK(!groupAcceptedFromJson(longDoc.as<JsonObjectConst>(), tooLong) && tooLong.acceptedRoom == "" &&
            tooLong.acceptedTags == "light",
        "G8: an oversized accepted room was kept or not reported");
}

// G9: sixteen paired peers: every member is sent to and the reply stays whole.
static void testManyPeers() {
  resetAll();
  char name[32];
  for (uint8_t i = 1; i <= 15; ++i) {
    std::snprintf(name, sizeof(name), "PeerWithAVeryLongLocalName%02u", (unsigned)i);
    pair(i, name);
    announce(i, "Living Room Upstairs North 1234", "alpha,beta,gamma,delta,epsilon");
  }
  for (uint8_t i = 1; i <= 8; ++i) run(cmd_espnow_groupaccept, macText(i).c_str());
  const std::string command(120, 'c');
  const std::string args = "\"Living Room Upstairs North 1234\" user pass " + command;
  std::string reply = run(cmd_espnow_roomcmd, args.c_str());
  CHECK(gSent.size() == 8, "G9: %zu frame(s) for 8 accepted peers", gSent.size());
  CHECK(reply.size() < 1024 && has(reply, "to 8 device(s) in Living Room Upstairs North 1234") &&
            has(reply, "Skipped 7 device(s) whose room is pending") && has(reply, ", ...") &&
            has(reply, "Accept with 'espnowgroupaccept <name_or_mac|all>'"),
        "G9: reply (%zu bytes) '%s'", reply.size(), reply.c_str());
  for (uint8_t i = 9; i <= 15; ++i) CHECK(!sentTo(i), "G9: pending peer %u was sent to", (unsigned)i);

  // Fifteen peers with long names do not fit one reply: the counts cover all
  // of them, the reply says how many it leaves out, and one can be named.
  std::string list = run(cmd_espnow_groups, "");
  CHECK(list.size() < CMD_RESULT_MAX && has(list, "15 peer(s), 7 with pending room or tags.") &&
            has(list, "PeerWithAVeryLongLocalName01") && !has(list, "PeerWithAVeryLongLocalName15") &&
            has(list, " more peer(s) not shown; name one: espnowgroups <name_or_mac>\n") &&
            has(list, "Accept what a peer announces: espnowgroupaccept <name_or_mac|all>."),
        "G9: list (%zu bytes) ends '%s'", list.size(), list.size() > 300 ? list.c_str() + list.size() - 300 : "");
  list = run(cmd_espnow_groups, "PeerWithAVeryLongLocalName15");
  CHECK(has(list, "PeerWithAVeryLongLocalName15 (") && has(list, "  pending:   room 'Living Room Upstairs North 1234'") &&
            has(list, "1 peer(s), 1 with pending room or tags."),
        "G9: one named peer '%s'", list.c_str());
  JsonDocument doc;
  std::string json = run(cmd_espnow_groups, "json");
  CHECK(json.size() < CMD_RESULT_MAX && !deserializeJson(doc, json) && doc["count"] == 15 &&
            doc["pendingCount"] == 7 && doc["peers"].size() >= 8 && doc["peers"].size() < 15 &&
            doc["notShown"].as<int>() == 15 - (int)doc["peers"].size(),
        "G9: list json (%zu bytes) '%s'", json.size(), json.size() > 200 ? json.c_str() + json.size() - 200 : "");
  json = run(cmd_espnow_groups, "json PeerWithAVeryLongLocalName15");
  CHECK(!deserializeJson(doc, json) && doc["count"] == 1 && doc["peers"].size() == 1 &&
            doc["peers"][0]["pendingTags"] == "alpha,beta,gamma,delta,epsilon" && doc["notShown"].isNull(),
        "G9: one named peer as JSON '%s'", json.c_str());
  std::string all = run(cmd_espnow_groupaccept, "all");
  CHECK(all.size() < CMD_RESULT_MAX && has(all, "15 peer(s), 7 changed."), "G9: accept all (%zu bytes) '%s'",
        all.size(), all.size() > 300 ? all.c_str() + all.size() - 300 : "");
  for (uint8_t i = 1; i <= 15; ++i) {
    CHECK(device(i)->acceptedRoom == "Living Room Upstairs North 1234", "G9: accept all left out peer %u", (unsigned)i);
  }
  json = run(cmd_espnow_groupdrop, "json all");
  CHECK(json.size() < CMD_RESULT_MAX && !deserializeJson(doc, json) && doc["ok"] == true && doc["count"] == 15 &&
            doc["changed"] == 15 && doc["notShown"].as<int>() == 15 - (int)doc["peers"].size(),
        "G9: drop all as JSON (%zu bytes)", json.size());
  for (uint8_t i = 1; i <= 15; ++i) CHECK(device(i)->acceptedRoom == "", "G9: drop all left out peer %u", (unsigned)i);

  // A command that cannot fit one frame is refused before anything is sent.
  const std::string tooLong = "\"Living Room Upstairs North 1234\" user pass " + std::string(200, 'x');
  reply = run(cmd_espnow_roomcmd, tooLong.c_str());
  CHECK(gSent.empty() && has(reply, "Error: command too long"), "G9: an over-long command: %zu frame(s), '%s'",
        gSent.size(), reply.c_str());
  CHECK(has(run(cmd_espnow_roomcmd, "Kitchen u p"), "Error: invalid arguments") &&
            has(run(cmd_espnow_tagcmd, "light u"), "Error: invalid arguments"),
        "G9: missing arguments are not refused");
  gState.initialized = false;
  CHECK(commandReplyIsFailure(run(cmd_espnow_tagcmd, "alpha u p x").c_str()) && gSent.empty(),
        "G9: a command while ESP-NOW is stopped was sent");
}

// G10: what the listings show: the announced value, marked when not accepted.
static void testListingMarks() {
  resetAll();
  pair(1, "Lamp");
  pair(2, "Fridge");
  announce(1, "Kitchen", "light,dimmer", "lamp-dev");
  announce(2, "Kitchen", "cold", "fridge-dev");
  run(cmd_espnow_groupaccept, "Lamp");
  announce(1, "Kitchen", "light,dimmer,admin", "lamp-dev");

  std::string rooms = run(cmd_espnow_rooms, "");
  CHECK(has(rooms, "  lamp-dev [light,dimmer,admin] (online) [pending: tags admin]\n") &&
            has(rooms, "  fridge-dev [cold] (online) [pending: room, tags cold]\n"),
        "G10: espnowrooms '%s'", rooms.c_str());
  JsonDocument doc;
  std::string json = run(cmd_espnow_rooms, "json");
  CHECK(!deserializeJson(doc, json), "G10: espnowrooms json does not parse: '%s'", json.c_str());
  JsonArray devs = doc["rooms"][0]["devices"];
  CHECK(devs.size() == 2 && devs[0]["accepted"] == true && devs[0]["pendingTags"] == "admin" &&
            devs[1]["accepted"] == false && devs[1]["pendingTags"] == "cold",
        "G10: espnowrooms json '%s'", json.c_str());

  std::string devices = run(cmd_espnow_devices, "");
  CHECK(has(devices, "  lamp-dev [Kitchen] (online, 0s ago) [pending: tags admin]\n") &&
            has(devices, "  fridge-dev [Kitchen] (online, 0s ago) [pending: room, tags cold]\n") &&
            has(devices, "Total: 2 devices"),
        "G10: espnowdevices '%s'", devices.c_str());
  json = run(cmd_espnow_devices, "json");
  CHECK(!deserializeJson(doc, json) && doc["count"] == 2 && doc["devices"][0]["room"] == "Kitchen" &&
            doc["devices"][0]["pendingRoom"].isNull() && doc["devices"][0]["pendingTags"] == "admin" &&
            doc["devices"][1]["pendingRoom"] == "Kitchen" && doc["devices"][1]["pendingTags"] == "cold" &&
            doc["notShown"].isNull() && !doc["hint"].isNull(),
        "G10: espnowdevices json '%s'", json.c_str());

  // The per-peer shape the web page reads too: only what waits is added, and
  // only when something does.
  JsonDocument one;
  espnowSerializeMeshPeerMeta(one.to<JsonObject>(), gMetaSlots[1]);
  CHECK(one["room"] == "Kitchen" && one["pendingRoom"] == "Kitchen" && one["pendingTags"] == "cold" &&
            one["acceptedRoom"].isNull() && one["acceptedTags"].isNull(),
        "G10: peer metadata JSON of a pending peer is wrong");
  JsonDocument two;
  espnowSerializeMeshPeerMeta(two.to<JsonObject>(), gMetaSlots[0]);
  CHECK(two["pendingRoom"].isNull() && two["pendingTags"] == "admin", "G10: peer metadata JSON of an accepted peer is wrong");
  run(cmd_espnow_groupaccept, "Lamp");
  JsonDocument three;
  espnowSerializeMeshPeerMeta(three.to<JsonObject>(), gMetaSlots[0]);
  CHECK(three["pendingRoom"].isNull() && three["pendingTags"].isNull() && three["tags"] == "light,dimmer,admin",
        "G10: a peer with everything accepted still carries pending fields");

  EspNowGroupView view;
  CHECK(!espnowGroupView(macOf(9), "Attic", "x,y", view) && std::strcmp(view.pendingRoom, "Attic") == 0 &&
            std::strcmp(view.pendingTags, "x,y") == 0 && view.acceptedRoom[0] == '\0',
        "G10: an unpaired address has something accepted");
  char mark[kGroupPendingMarkCap];
  const std::string everyTag = std::string(20, 'a') + "," + std::string(20, 'b') + "," + std::string(20, 'c');
  groupPendingMark(macOf(9), "Attic", everyTag.c_str(), mark, sizeof(mark));
  CHECK(std::strlen(mark) < sizeof(mark) - 1 && mark[std::strlen(mark) - 1] == ']', "G10: longest mark is cut: '%s'",
        mark);
}

// G11: a peer's local name decides how it is called; its own names do not.
static void testNames() {
  resetAll();
  pair(1, "all");
  pair(2, "");
  pair(3, "Lamp");
  announce(1, "Kitchen", "", "Lamp");   // announces another peer's name
  announce(2, "Kitchen", "", "Fridge");
  CHECK(eventsWith((macText(1) + " says it is in room 'Kitchen'; not used for room commands until accepted here: "
                    "espnowgroupaccept " + macText(1)).c_str()) == 1,
        "G11: a peer named 'all' is not called by its address: '%s'", gEvents.empty() ? "" : gEvents[0].c_str());
  CHECK(eventsWith((macText(2) + " says it is in room").c_str()) == 1, "G11: a peer without a name");
  CHECK(eventsWith("Lamp says") == 0 && eventsWith("Fridge says") == 0,
        "G11: a notice uses a name the peer announced");
  // Accepting by the address the notice gave accepts that peer only.
  std::string reply = run(cmd_espnow_groupaccept, macText(1).c_str());
  CHECK(has(reply, "1 peer(s), 1 changed.") && device(1)->acceptedRoom == "Kitchen" && device(2)->acceptedRoom == "" &&
            device(3)->acceptedRoom == "",
        "G11: accept by address '%s'", reply.c_str());
}

// G12: the metadata cache has fewer slots than the registry has records. A
// peer without a slot is still a member by what was accepted for it, so its
// announcements must reach its record: above all the one that leaves a room.
static void testCacheSmallerThanRegistry() {
  resetAll();
  gMeshPeerSlots = 1;
  pair(1, "Other");
  pair(2, "Lamp");
  announce(1, "Hall", "");  // takes the only cache slot
  gWarns.clear();
  announce(2, "Kitchen", "light");
  CHECK(device(2)->room == "Kitchen" && device(2)->tags == "light" && device(2)->acceptedRoom == "",
        "G12: without a cache slot the record got room '%s' tags '%s'", device(2)->room.c_str(),
        device(2)->tags.c_str());
  CHECK(eventsWith("Lamp says it is in room 'Kitchen'") == 1 && eventsWith("Lamp says it has tag 'light'") == 1,
        "G12: no notice for a peer without a cache slot");
  CHECK(gWarns.size() == 1 && has(gWarns[0], "slots full"), "G12: the full cache is not reported (%zu warnings)",
        gWarns.size());
  CHECK(!gMetaSlots[0].isActive || std::memcmp(gMetaSlots[0].mac, macOf(1), 6) == 0,
        "G12: the cache slot of the other peer was taken");
  run(cmd_espnow_groupaccept, "Lamp");
  CHECK(run(cmd_espnow_roomcmd, "Kitchen u p x").find("to 1 device(s)") != std::string::npos && sentTo(2),
        "G12: the accepted peer without a cache slot is not targeted");

  // It leaves the room and drops the tag: out at once, although the cache is still full.
  announce(2, "", "");
  CHECK(device(2)->room == "" && device(2)->acceptedRoom == "" && device(2)->acceptedTags == "",
        "G12: the leave of a peer without a cache slot was lost: room '%s' accepted '%s' / '%s'",
        device(2)->room.c_str(), device(2)->acceptedRoom.c_str(), device(2)->acceptedTags.c_str());
  std::string after = run(cmd_espnow_roomcmd, "Kitchen u p x");
  CHECK(gSent.empty() && has(after, "No devices found in room 'Kitchen'") && !has(after, "Skipped"),
        "G12: a peer that left still gets the room's command: %zu frame(s), '%s'", gSent.size(), after.c_str());
  CHECK(run(cmd_espnow_tagcmd, "light u p x").find("No devices found") != std::string::npos && gSent.empty(),
        "G12: a peer that dropped a tag still gets the tag's command");

  // No cache at all (it could not be allocated): the registry is still kept.
  MeshPeerMeta* cache = gMeshPeerMeta;
  gMeshPeerMeta = nullptr;
  announce(2, "Garage", "");
  gMeshPeerMeta = cache;
  CHECK(device(2)->room == "Garage", "G12: without a cache the record did not follow the announcement");
}

// G13: a peer that keeps changing gets one report a minute and one registry
// save per gap; what it leaves still applies with every frame.
static void testFlippingPeer() {
  resetAll();
  pair(1, "Flip");
  pair(2, "Calm");
  announce(1, "A", "x");
  run(cmd_espnow_groupaccept, "Flip");
  gNowMs += 2 * espnowgroup::kNoticeGapMs;
  tick();
  gEvents.clear();
  const int savesBefore = gSaves;
  const uint32_t start = gNowMs;

  // Fifty frames in five seconds, room and tags different each time.
  bool leftAtOnce = true;
  for (int i = 0; i < 50; ++i) {
    gNowMs += 100;
    if (i % 2 == 0) {
      announceNow(1, "B", "");
      leftAtOnce = leftAtOnce && device(1)->acceptedRoom == "" && device(1)->acceptedTags == "" &&
                   device(1)->room == "B";
    } else {
      announceNow(1, "A", "x");
      leftAtOnce = leftAtOnce && device(1)->acceptedRoom == "" && device(1)->room == "A";
    }
    tick();
  }
  CHECK(leftAtOnce, "G13: a held-back report also held back what the peer left or announced");
  CHECK(gEvents.size() == 1 && has(gEvents[0], "Flip says it is in room 'B'"),
        "G13: 50 changing frames printed %zu event line(s), expected the first only", gEvents.size());
  CHECK(gSaves - savesBefore == 1, "G13: 50 changing frames saved the registry %d times, expected once",
        gSaves - savesBefore);
  // Another peer is not held back by this one.
  announceNow(2, "Kitchen", "");
  CHECK(eventsWith("Calm says it is in room 'Kitchen'") == 1, "G13: one peer's changes held back another's notice");

  // When the save gap is over the last state is written, once.
  gNowMs = start + kAnnounceSaveGapMs + 200;
  tick();
  tick();
  CHECK(gSaves - savesBefore == 2, "G13: after the gap the registry was saved %d times in all, expected 2",
        gSaves - savesBefore);
  // When the minute is over: how many changes were held back, and what waits now.
  gEvents.clear();
  gNowMs = start + espnowgroup::kNoticeGapMs + 200;
  tick();
  CHECK(gEvents.size() == 3 &&
            gEvents[0] == "Flip changed its room or tags 49 more time(s) within a minute; what waits for acceptance "
                          "now follows" &&
            gEvents[1] == "Flip says it is in room 'A'; not used for room commands until accepted here: "
                          "espnowgroupaccept Flip" &&
            gEvents[2] == "Flip says it has tag 'x'; not used for tag commands until accepted here: "
                          "espnowgroupaccept Flip",
        "G13: report after the minute: %zu line(s), first '%s'", gEvents.size(),
        gEvents.empty() ? "" : gEvents[0].c_str());
  tick();
  gNowMs += espnowgroup::kNoticeGapMs + 200;
  tick();
  CHECK(gEvents.size() == 3, "G13: the held-back report was printed again");

  // It goes on: the next minute brings one more report, not one per frame.
  gEvents.clear();
  for (int i = 0; i < 20; ++i) {
    gNowMs += 100;
    announceNow(1, i % 2 ? "A" : "C", "x");
  }
  CHECK(gEvents.size() == 1 && has(gEvents[0], "Flip says it is in room 'C'"), "G13: second minute printed %zu line(s)",
        gEvents.size());
  // Held-back changes that end on what is accepted: said, with nothing to accept.
  run(cmd_espnow_groupaccept, "Flip");
  gEvents.clear();
  gNowMs += espnowgroup::kNoticeGapMs + 200;
  tick();
  CHECK(gEvents.size() == 1 && gEvents[0] == "Flip changed its room or tags 19 more time(s) within a minute; nothing "
                                             "of it waits for acceptance now",
        "G13: report with nothing waiting: %zu line(s), '%s'", gEvents.size(), gEvents.empty() ? "" : gEvents[0].c_str());

  // A peer that is unpaired while its changes are held back is not reported.
  announce(2, "Garage", "");
  announceNow(2, "Attic", "");
  removeEspNowDevice(macOf(2));
  gEvents.clear();
  gNowMs += espnowgroup::kNoticeGapMs + 200;
  tick();
  CHECK(gEvents.empty(), "G13: a report for an unpaired peer: '%s'", gEvents.empty() ? "" : gEvents[0].c_str());

  // More peers than there are entries: none of them is printed unlimited.
  resetAll();
  for (uint8_t i = 1; i <= 15; ++i) {
    char name[16];
    std::snprintf(name, sizeof(name), "P%u", (unsigned)i);
    pair(i, name);
    announceNow(i, "R1", "");
    announceNow(i, "R2", "");
  }
  CHECK(gEvents.size() == 15, "G13: fifteen peers changing twice printed %zu line(s), expected one each", gEvents.size());
}

// G14: the other task gets in just before a critical section. Whatever a step
// uses, it reads inside the lock.
static void testOtherTaskGetsInFirst() {
  // An unpair moves the next record into the removed one's place while that
  // peer's announcement is on its way into the registry.
  resetAll();
  pair(1, "Gone");
  pair(2, "Stays");
  announce(2, "Hall", "");
  run(cmd_espnow_groupaccept, "Stays");
  int takes = 0;
  std::function<void()> unpairAtSecondTake = [&]() {
    if (++takes < 2) {
      gOnLockTake = unpairAtSecondTake;  // the first section found the record; wait for the store
      return;
    }
    removeEspNowDevice(macOf(1));
  };
  gNowMs += espnowgroup::kNoticeGapMs + 1000;
  gOnLockTake = unpairAtSecondTake;
  announceNow(1, "Kitchen", "light");
  CHECK(device(1) == nullptr && device(2) != nullptr && device(2)->room == "Hall" && device(2)->acceptedRoom == "Hall" &&
            device(2)->tags == "" && device(2)->name == "Stays",
        "G14: an announcement of a peer unpaired meanwhile landed in the next record: room '%s' accepted '%s'",
        device(2) ? device(2)->room.c_str() : "?", device(2) ? device(2)->acceptedRoom.c_str() : "?");
  CHECK(countMeshPeerMetaByRoom("Kitchen") == 0, "G14: the removed peer's room is counted");

  // A drop lands just before an announcement that keeps the accepted room:
  // the announcement must not bring the acceptance back.
  resetAll();
  pair(1, "Lamp");
  announce(1, "Kitchen", "light,dimmer");
  run(cmd_espnow_groupaccept, "Lamp");
  takes = 0;
  std::function<void()> dropAtSecondTake = [&]() {
    if (++takes < 2) {
      gOnLockTake = dropAtSecondTake;
      return;
    }
    groupAcceptedDrop(macOf(1));
  };
  gNowMs += espnowgroup::kNoticeGapMs + 1000;
  gOnLockTake = dropAtSecondTake;
  announceNow(1, "Kitchen", "light");
  CHECK(device(1)->acceptedRoom == "" && device(1)->acceptedTags == "" && device(1)->tags == "light",
        "G14: an announcement brought back what was dropped: accepted '%s' / '%s'", device(1)->acceptedRoom.c_str(),
        device(1)->acceptedTags.c_str());

  // An announcement lands just before the accept: what is announced then is
  // what gets accepted, and the reply says so.
  resetAll();
  pair(1, "Lamp");
  announce(1, "Garage", "");
  gNowMs += espnowgroup::kNoticeGapMs + 1000;
  bool armed = true;
  std::function<void()> announceBeforeAccept = [&]() {
    if (!armed) return;
    armed = false;
    announceNow(1, "Kitchen", "");
  };
  // The accept resolves the name first; the announcement goes in at its
  // critical section, the last lock the command takes before it writes.
  gOnLockTake = announceBeforeAccept;
  std::string reply = run(cmd_espnow_groupaccept, "Lamp");
  CHECK(device(1)->room == "Kitchen" && device(1)->acceptedRoom == "Kitchen" && has(reply, "accepted room 'Kitchen'"),
        "G14: accept beside a new announcement: room '%s' accepted '%s', reply '%s'", device(1)->room.c_str(),
        device(1)->acceptedRoom.c_str(), reply.c_str());
  CHECK(gLockTakes > 0 && gLockDepth == 0, "G14: the lock was taken %d times and is at depth %d", gLockTakes, gLockDepth);
}

// G15: a tag command names one tag.
static void testOneTagPerCommand() {
  resetAll();
  pair(1, "Lamp");
  pair(2, "Door");
  announce(1, "", "light");
  announce(2, "", "door,light");
  run(cmd_espnow_groupaccept, "all");
  for (const char* target : {"light,door", "door,light", ",light", "light,", ","}) {
    const std::string args = std::string(target) + " u secret unlock";
    std::string reply = run(cmd_espnow_tagcmd, args.c_str());
    CHECK(gSent.empty() && commandReplyIsFailure(reply.c_str()) && has(reply, "one tag") && !has(reply, "secret"),
          "G15: tag target '%s': %zu frame(s), reply '%s'", target, gSent.size(), reply.c_str());
  }
  std::string one = run(cmd_espnow_tagcmd, "light u p x");
  CHECK(gSent.size() == 2 && has(one, "to 2 device(s) with tag 'light'"), "G15: a single tag: %zu frame(s), '%s'",
        gSent.size(), one.c_str());
  // A room is one name whatever it holds.
  CHECK(has(run(cmd_espnow_roomcmd, "a,b u p x"), "No devices found in room 'a,b'") && gSent.empty(),
        "G15: a room with a comma is not taken whole");
}

// G16: the JSON listings stay whole with a full table.
static void testFullTableJson() {
  resetAll();
  char name[32];
  char friendly[48];
  for (uint8_t i = 1; i <= 15; ++i) {
    std::snprintf(name, sizeof(name), "Peer%02u", (unsigned)i);
    pair(i, name);
    V4PayloadMetadata p;
    std::memset(&p, 0, sizeof(p));
    std::snprintf(p.deviceName, sizeof(p.deviceName), "kitchen-node-%02u", (unsigned)i);
    std::snprintf(friendly, sizeof(friendly), "Kitchen Sensor %02u", (unsigned)i);
    std::snprintf(p.friendlyName, sizeof(p.friendlyName), "%s", friendly);
    std::snprintf(p.room, sizeof(p.room), "Living Room");
    std::snprintf(p.zone, sizeof(p.zone), "Upstairs");
    std::snprintf(p.tags, sizeof(p.tags), "thermal,stationary");
    gNowMs += espnowgroup::kNoticeGapMs + 1000;
    processMetadata(macOf(i), &p);
  }
  // Nothing accepted, as right after the update: every peer carries what waits.
  JsonDocument doc;
  std::string json = run(cmd_espnow_devices, "json");
  CHECK(json.size() < CMD_RESULT_MAX && !deserializeJson(doc, json) && doc["count"] == 15 &&
            doc["devices"].size() + doc["notShown"].as<size_t>() == 15 && doc["devices"].size() >= 12 &&
            doc["devices"][0]["pendingRoom"] == "Living Room" && !doc["hint"].isNull(),
        "G16: espnowdevices json with 15 pending peers: %zu bytes, %zu shown, ends '%s'", json.size(),
        doc["devices"].size(), json.size() > 120 ? json.c_str() + json.size() - 120 : json.c_str());
  json = run(cmd_espnow_rooms, "json");
  CHECK(json.size() < CMD_RESULT_MAX && !deserializeJson(doc, json) && doc["count"] == 1 &&
            doc["rooms"][0]["devices"].size() == 15 && doc["notShown"].isNull() &&
            doc["rooms"][0]["devices"][6]["accepted"] == false &&
            doc["rooms"][0]["devices"][6]["pendingTags"] == "thermal,stationary",
        "G16: espnowrooms json with 15 pending peers in one room: %zu bytes, %zu shown", json.size(),
        doc["rooms"][0]["devices"].size());

  // Everything accepted: the typical full table fits whole.
  run(cmd_espnow_groupaccept, "all");
  json = run(cmd_espnow_devices, "json");
  CHECK(json.size() < CMD_RESULT_MAX && !deserializeJson(doc, json) && doc["count"] == 15 &&
            doc["devices"].size() == 15 && doc["notShown"].isNull() && doc["devices"][14]["pendingRoom"].isNull(),
        "G16: espnowdevices json with 15 accepted peers: %zu bytes, %zu shown", json.size(), doc["devices"].size());
  json = run(cmd_espnow_rooms, "json");
  CHECK(!deserializeJson(doc, json) && doc["rooms"][0]["devices"][0]["accepted"] == true &&
            doc["rooms"][0]["devices"][0]["pendingTags"].isNull(),
        "G16: espnowrooms json of accepted peers '%s'", json.c_str());

  // The longest names in every field, a room each: still valid, the rest counted.
  resetAll();
  for (uint8_t i = 1; i <= 15; ++i) {
    std::snprintf(name, sizeof(name), "Peer%02u", (unsigned)i);
    pair(i, name);
    V4PayloadMetadata p;
    std::memset(&p, 0, sizeof(p));
    std::memset(p.deviceName, 'n', sizeof(p.deviceName) - 1);
    std::memset(p.friendlyName, 'f', sizeof(p.friendlyName) - 1);
    std::memset(p.room, 'r', sizeof(p.room) - 1);
    p.room[0] = static_cast<char>('A' + i);
    std::memset(p.zone, 'z', sizeof(p.zone) - 1);
    std::memset(p.tags, 't', sizeof(p.tags) - 1);
    gNowMs += espnowgroup::kNoticeGapMs + 1000;
    processMetadata(macOf(i), &p);
  }
  json = run(cmd_espnow_devices, "json");
  CHECK(json.size() < CMD_RESULT_MAX && !deserializeJson(doc, json) && doc["count"] == 15 &&
            doc["notShown"].as<size_t>() > 0 && doc["devices"].size() + doc["notShown"].as<size_t>() == 15,
        "G16: espnowdevices json with the longest names: %zu bytes", json.size());
  json = run(cmd_espnow_rooms, "json");
  size_t shown = 0;
  bool emptyRoom = false;
  if (!deserializeJson(doc, json)) {
    for (JsonObject room : doc["rooms"].as<JsonArray>()) {
      shown += room["devices"].size();
      emptyRoom = emptyRoom || room["devices"].size() == 0;
    }
  }
  CHECK(json.size() < CMD_RESULT_MAX && doc["count"] == 15 && doc["notShown"].as<size_t>() > 0 &&
            shown + doc["notShown"].as<size_t>() == 15 && !emptyRoom && !doc["hint"].isNull(),
        "G16: espnowrooms json with the longest names: %zu bytes, %zu shown", json.size(), shown);
}

// G17: a name two records share is not used to call a peer or to accept one.
static void testSharedNames() {
  resetAll();
  pair(1, "Node");
  pair(2, "node");
  pair(3, "self");          // the name of this device's own entry
  pair(4, "02BB0000000F");  // reads as an address
  pair(5, "Lamp");
  announce(1, "Kitchen", "");
  announce(2, "Garage", "");
  announce(3, "Attic", "");
  announce(4, "Cellar", "");
  CHECK(eventsWith((macText(2) + " says it is in room 'Garage'; not used for room commands until accepted here: "
                    "espnowgroupaccept " + macText(2)).c_str()) == 1 &&
            eventsWith((macText(1) + " says it is in room 'Kitchen'").c_str()) == 1,
        "G17: two peers named alike are not called by their addresses");
  CHECK(eventsWith((macText(3) + " says it is in room 'Attic'").c_str()) == 1,
        "G17: a peer named like this device's own entry is not called by its address");
  CHECK(eventsWith((macText(4) + " says it is in room 'Cellar'").c_str()) == 1,
        "G17: a peer whose name reads as an address is not called by its own address");
  CHECK(eventsWith("ode says") == 0 && eventsWith("self says") == 0, "G17: a shared name is used in a notice");

  // The name alone is refused; nothing is accepted for either.
  for (const char* command : {"Node", "json node"}) {
    std::string reply = run(cmd_espnow_groupaccept, command);
    CHECK(commandReplyIsFailure(reply.c_str()) && has(reply, "More than one paired device has that name"),
          "G17: accept by a shared name '%s': '%s'", command, reply.c_str());
  }
  CHECK(commandReplyIsFailure(run(cmd_espnow_groupdrop, "Node").c_str()) &&
            commandReplyIsFailure(run(cmd_espnow_groups, "Node").c_str()) &&
            commandReplyIsFailure(run(cmd_espnow_groupaccept, "self").c_str()),
        "G17: a shared name is taken by another membership command");
  CHECK(device(1)->acceptedRoom == "" && device(2)->acceptedRoom == "" && device(3)->acceptedRoom == "",
        "G17: a refused accept changed something");
  // By the address the notice gave: that peer and no other.
  std::string reply = run(cmd_espnow_groupaccept, macText(2).c_str());
  CHECK(has(reply, (macText(2) + " (" + macText(2) + "): accepted room 'Garage'").c_str()) &&
            device(2)->acceptedRoom == "Garage" && device(1)->acceptedRoom == "",
        "G17: accept by address '%s'", reply.c_str());
  reply = run(cmd_espnow_groupaccept, macText(3).c_str());
  CHECK(device(3)->acceptedRoom == "Attic" && has(reply, "1 peer(s), 1 changed."), "G17: accept of the peer named self");
  // A group command's skipped list calls them the same way.
  reply = run(cmd_espnow_roomcmd, "Kitchen u p x");
  CHECK(has(reply, ("Skipped 1 device(s) whose room is pending (announced, not accepted here): " + macText(1)).c_str()),
        "G17: skipped list '%s'", reply.c_str());
  // A name of its own is still used.
  announce(5, "Kitchen", "");
  CHECK(eventsWith("Lamp says it is in room 'Kitchen'") == 1 && has(run(cmd_espnow_groupaccept, "lamp"), "Lamp ("),
        "G17: a name no other record has is not used");
}

// The answer to a refresh has longer to arrive than the requests are apart
// (the receive handler that uses the window is run by test_espnow_metadata_text.py).
static_assert(kMetadataRefreshAnswerMs > kMetadataRefreshGapMs, "an answer has time to arrive");

// G18: frames and requests wait per sender, and a session that came up gets
// the peer asked for its metadata.
static void testInboxAndRefresh() {
  resetAll();
  // Two senders in one pass: both frames are there afterwards, each once.
  MetadataInboxEntry* a = metadataInboxFor(macOf(1));
  MetadataInboxEntry* b = metadataInboxFor(macOf(2));
  CHECK(a && b && a != b && metadataInboxFor(macOf(1)) == a, "G18: two senders do not get an entry each");
  if (!a || !b) return;
  a->frameWaiting = true;
  a->payload[0] = 'A';
  b->frameWaiting = true;
  b->payload[0] = 'B';
  a->payload[0] = 'a';  // a newer frame of the same sender replaces its older one
  MetadataInboxEntry* first = metadataInboxTakeFrame();
  MetadataInboxEntry* second = metadataInboxTakeFrame();
  CHECK(first == a && first->payload[0] == 'a' && second == b && second->payload[0] == 'B' &&
            metadataInboxTakeFrame() == nullptr,
        "G18: the frames of two senders were not both handed out once");
  // One sender that sends with every pass does not keep another from its turn.
  a->frameWaiting = true;
  b->frameWaiting = true;
  first = metadataInboxTakeFrame();
  a->frameWaiting = true;
  second = metadataInboxTakeFrame();
  CHECK((first == a && second == b) || (first == b && second == a), "G18: one sender was served twice before the other");
  while (metadataInboxTakeFrame()) {}
  // Requests for our metadata: every asker is answered.
  a->replyDue = true;
  b->replyDue = true;
  uint8_t asker1[6], asker2[6], none[6];
  CHECK(metadataInboxTakeReply(asker1) && metadataInboxTakeReply(asker2) && !metadataInboxTakeReply(none) &&
            std::memcmp(asker1, asker2, 6) != 0,
        "G18: two peers that asked in one pass are not both answered");
  // Sixteen senders have an entry each; a seventeenth takes one that holds nothing.
  for (uint8_t i = 1; i <= 16; ++i) {
    MetadataInboxEntry* e = metadataInboxFor(macOf(i));
    CHECK(e != nullptr, "G18: no entry for sender %u", (unsigned)i);
    if (e) e->frameWaiting = (i != 7);
  }
  MetadataInboxEntry* extra = metadataInboxFor(macOf(17));
  CHECK(extra != nullptr && std::memcmp(extra->mac, macOf(17), 6) == 0 && !extra->frameWaiting && !extra->refreshAsked,
        "G18: a new sender did not get the one entry without work");
  if (extra) extra->frameWaiting = true;
  CHECK(metadataInboxFor(macOf(18)) == nullptr, "G18: an entry that holds a frame was given away");

  // Refresh: asked once per session, after the session has settled, one peer per look.
  resetAll();
  pair(1, "Lamp");
  pair(2, "Fridge");
  gNowMs = 500000;
  const uint8_t stranger[6] = {0x02, 0xCC, 0, 0, 0, 0x09};
  SessionState s1{}, s2{}, s3{}, s4{};
  std::memcpy(s1.peerMac, macOf(1), 6);
  s1.establishedAtMs = gNowMs;
  s1.state = SESSION_ACTIVE;
  std::memcpy(s2.peerMac, macOf(2), 6);
  s2.establishedAtMs = gNowMs;
  s2.state = SESSION_REKEYING;  // not usable now
  std::memcpy(s3.peerMac, stranger, 6);  // a session with an address that is not paired
  s3.establishedAtMs = gNowMs;
  s3.state = SESSION_ACTIVE;
  std::memcpy(s4.peerMac, kSelf, 6);
  s4.establishedAtMs = gNowMs;
  s4.state = SESSION_ACTIVE;
  gSessions = {s3, s4, s1, s2};
  gNowMs += 1000;
  CHECK(metadataRefreshTick(gNowMs) == 0 && gMetaRequests.empty(), "G18: asked before the session settled");
  gNowMs += kMetadataRefreshSettleMs;
  CHECK(metadataRefreshTick(gNowMs) == 1 && gMetaRequests.size() == 1 && gMetaRequests[0] == macText(1),
        "G18: the peer of a new session was not asked (%zu request(s))", gMetaRequests.size());
  for (int i = 0; i < 20; ++i) {
    gNowMs += kMetadataRefreshGapMs;
    metadataRefreshTick(gNowMs);
  }
  CHECK(gMetaRequests.size() == 1, "G18: one session was asked for %zu times", gMetaRequests.size());
  // The second peer's session becomes usable with new keys: asked then; and
  // the two are not asked in the same look.
  gSessions[3].state = SESSION_ACTIVE;
  gSessions[3].establishedAtMs = gNowMs;
  gSessions[2].establishedAtMs = gNowMs;  // the first peer's session was set up again
  gNowMs += kMetadataRefreshSettleMs + kMetadataRefreshGapMs;
  CHECK(metadataRefreshTick(gNowMs) == 1 && gMetaRequests.size() == 2, "G18: a renewed session was not asked for");
  CHECK(metadataRefreshTick(gNowMs + 1) == 0 && gMetaRequests.size() == 2, "G18: two requests in one look");
  gNowMs += kMetadataRefreshGapMs;
  CHECK(metadataRefreshTick(gNowMs) == 1 && gMetaRequests.size() == 3 && gMetaRequests[1] != gMetaRequests[2],
        "G18: the second renewed session was not asked for");
  // Two sessions with one peer, an older and a newer: no asking back and forth.
  SessionState older = gSessions[2];
  older.establishedAtMs -= 5000;
  gSessions.push_back(older);
  for (int i = 0; i < 20; ++i) {
    gNowMs += kMetadataRefreshGapMs;
    metadataRefreshTick(gNowMs);
  }
  CHECK(gMetaRequests.size() == 3, "G18: an older session of the same peer was asked for (%zu requests)",
        gMetaRequests.size());
  // The answer to such a request is marked to be stored without a history line.
  MetadataInboxEntry* asked = metadataInboxFor(macOf(1));
  CHECK(asked && asked->refreshAsked && asked->refreshQuiet && asked->refreshAskedMs != 0,
        "G18: the request is not remembered with its entry");
  // While ESP-NOW is stopped nothing is asked.
  gState.initialized = false;
  gSessions[2].establishedAtMs = gNowMs;
  gNowMs += kMetadataRefreshSettleMs + kMetadataRefreshGapMs;
  CHECK(metadataRefreshTick(gNowMs) == 0 && gMetaRequests.size() == 3, "G18: asked while ESP-NOW is stopped");
}

// G19: the command reply contract on a group command. Without encryption
// nothing is sent; a member whose frame is refused makes the reply an Error
// that names it and why, with the members it went to still listed; with every
// member refused the reply says none was sent. Pending peers are still counted.
static void testGroupSendVerdicts() {
  resetAll();
  pair(1, "Fridge");
  pair(2, "Oven");
  pair(3, "Toaster");
  announce(1, "Kitchen", "");
  announce(2, "Kitchen", "");
  announce(3, "Kitchen", "");
  run(cmd_espnow_groupaccept, "Fridge");
  run(cmd_espnow_groupaccept, "Oven");

  gState.encryptionEnabled = false;
  std::string reply = run(cmd_espnow_roomcmd, "Kitchen u p status");
  CHECK(gSent.empty() && commandReplyIsFailure(reply.c_str()) && has(reply, "ESP-NOW encryption required"),
        "G19: sent without encryption: '%s'", reply.c_str());
  gState.encryptionEnabled = true;

  gRefusedDst = macText(2);
  reply = run(cmd_espnow_roomcmd, "Kitchen u p status");
  CHECK(gSent.size() == 1 && sentTo(1) && commandReplyIsFailure(reply.c_str()) &&
            has(reply, "Error: Command sent to 1 of 2 device(s) in room 'Kitchen'; not sent to Oven "
                       "(4 messages already wait for this device's session).") &&
            has(reply, "  -> Fridge (") && has(reply, "Skipped 1 device(s) whose room is pending") &&
            has(reply, "Toaster"),
        "G19: one member refused: '%s'", reply.c_str());

  run(cmd_espnow_groupdrop, "Fridge");
  reply = run(cmd_espnow_roomcmd, "Kitchen u p status");
  CHECK(gSent.empty() && commandReplyIsFailure(reply.c_str()) &&
            has(reply, "Error: Command not sent to any of 1 device(s) in room 'Kitchen' "
                       "(4 messages already wait for this device's session).") &&
            !has(reply, "->"),
        "G19: every member refused: '%s'", reply.c_str());
  gRefusedDst.clear();

  reply = run(cmd_espnow_roomcmd, "Kitchen u p status");
  CHECK(gSent.size() == 1 && sentTo(2) && !commandReplyIsFailure(reply.c_str()) &&
            has(reply, "Sent 'status' to 1 device(s) in Kitchen"),
        "G19: all members sent: '%s'", reply.c_str());
  reply = run(cmd_espnow_roomcmd, "Garage u p status");
  CHECK(gSent.empty() && commandReplyIsFailure(reply.c_str()) &&
            has(reply, "Error: No devices found in room 'Garage'. List rooms and members with 'espnowgroups'."),
        "G19: an empty room: '%s'", reply.c_str());

  // A member without a session yet: its frame waits for the session to open,
  // and the reply says queued, not sent (bench 2026-11-18).
  run(cmd_espnow_groupaccept, "Fridge");
  gQueuedDst = {macText(2)};
  reply = run(cmd_espnow_roomcmd, "Kitchen u p status");
  CHECK(gSent.size() == 2 && !commandReplyIsFailure(reply.c_str()) &&
            has(reply, "  -> Oven (") && has(reply, ") queued until its session opens\n") &&
            has(reply, "Sent 'status' to 2 device(s) in Kitchen (1 of them queued until their session opens)"),
        "G19: one member queued: '%s'", reply.c_str());
  gQueuedDst = {macText(1), macText(2)};
  reply = run(cmd_espnow_roomcmd, "Kitchen u p status");
  CHECK(gSent.size() == 2 && !commandReplyIsFailure(reply.c_str()) && !has(reply, "Sent '") &&
            has(reply, "Queued 'status' for 2 device(s) in Kitchen until their sessions open"),
        "G19: every member queued: '%s'", reply.c_str());
  gQueuedDst = {macText(1)};
  gRefusedDst = macText(2);
  reply = run(cmd_espnow_roomcmd, "Kitchen u p status");
  CHECK(commandReplyIsFailure(reply.c_str()) &&
            has(reply, "Error: Command sent to 1 of 2 device(s) in room 'Kitchen' (1 of them queued until their "
                       "session opens); not sent to Oven"),
        "G19: one queued, one refused: '%s'", reply.c_str());
}

int main() {
  testNewClaimNotTargeted();
  testAcceptedTargeted();
  testRoomChange();
  testTagsPerTag();
  testAcceptRecordsAnnounced();
  testAllDropAndSave();
  testUnpairAndIdentity();
  testStoredRecord();
  testManyPeers();
  testListingMarks();
  testNames();
  testCacheSmallerThanRegistry();
  testFlippingPeer();
  testOtherTaskGetsInFirst();
  testOneTagPerCommand();
  testFullTableJson();
  testSharedNames();
  testInboxAndRefresh();
  testGroupSendVerdicts();
  std::printf("espnow_group_membership: %d checks, %d failed\n", gChecks, gFailures);
  return gFailures ? 1 : 0;
}
