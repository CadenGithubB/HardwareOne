// Host harness for test_espnow_system_notice.py.
//
// The runner pastes LogMessageType and TextQueueEntry from System_ESPNow.h,
// and from System_ESPNow.cpp espnowFilePeerRestart (the record a peer's
// restart leaves since the BOOT frame went in ESP-NOW 1.0 step 8) and the
// espnow_task text-queue drain ("// 11. Drain text message queue") at the
// INSERT_* tags. String is the shared host stub. History, the console, the
// device names, the event bus and the G2 push-kick are recording doubles; no
// radio is touched.
#include <Arduino.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "System_MacText.h"  // production MAC text helpers (copied beside this unit)

void webBatchTestStringAppend() {}

static int gChecks = 0;
static int gFailures = 0;
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

#define EXT_RAM_BSS_ATTR

// Console double: every BROADCAST_PRINTF line, formatted.
static std::vector<std::string> gPrinted;
#define BROADCAST_PRINTF(...)                                         \
  do {                                                                \
    char line_[512];                                                  \
    std::snprintf(line_, sizeof(line_), __VA_ARGS__);                 \
    gPrinted.push_back(line_);                                        \
  } while (0)

// INSERT_MESSAGE_TYPES

// INSERT_TEXT_QUEUE_ENTRY

// Owner state double: only the members the drain touches.
struct EspNowState {
  static constexpr int TEXT_QUEUE_SIZE = 16;
  TextQueueEntry textQueue[TEXT_QUEUE_SIZE] = {};
  volatile int textQueueHead = 0;
  volatile int textQueueTail = 0;
};
static EspNowState gState;
static EspNowState* gEspNow = &gState;

static String macToDisplayStr(const uint8_t* mac) {  // System_Utils.h
  char b[18];
  macToDisplay(mac, b, sizeof b);
  return String(b);
}
// System_ESPNow.h: how a received record's sender is shown.
enum class HistoryAttribution : uint8_t { FromName = 0, Attributed = 1, Unattributed = 2 };
struct StoredMessage {
  std::string name, content; bool encrypted; int type; uint32_t reqId; uint8_t piece, of;
  HistoryAttribution attribution;
};
static std::vector<StoredMessage> gStored;
static bool storeMessageInPeerHistory(uint8_t*, const char* name, const char* message, bool encrypted,
                                      LogMessageType type, uint32_t reqId = 0, uint8_t piece = 1,
                                      uint8_t pieceTotal = 1,
                                      HistoryAttribution attribution = HistoryAttribution::FromName) {
  gStored.push_back({name ? name : "", message ? message : "", encrypted, (int)type, reqId, piece, pieceTotal,
                     attribution});
  return true;
}
// Device names (System_ESPNow.cpp): the registry name of a paired peer.
static std::string gName;
static String getEspNowDeviceName(const uint8_t*) { return String(gName.c_str()); }
static unsigned gPushKicks = 0;
static void g2EspNowAppOnRxText(const uint8_t*) { ++gPushKicks; }
static constexpr uint8_t SYSEVT_TEXT_RX = 7;
static unsigned gTextEvents = 0;
static void systemEventPost(uint8_t kind, const char* = nullptr, const char* = nullptr,
                            uint8_t = 0, const char* = nullptr) {
  if (kind == SYSEVT_TEXT_RX) ++gTextEvents;
}

// INSERT_PRODUCTION

static void drainTextQueueForTest() {
  // INSERT_DRAIN
}

static const uint8_t kPeer[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};

static void reset() {
  gState = EspNowState();
  gStored.clear();
  gPrinted.clear();
  gPushKicks = 0;
  gTextEvents = 0;
}

// Queue one received entry the way v4h_text does. content is copied with at
// most sizeof(content) bytes; fill (when non-zero) fills the whole field with
// no terminator, to show the drain bounds its read.
static void queue(LogMessageType type, const char* name, const char* content, uint32_t msgId,
                  char fill = 0, uint8_t fragIndex = 0, uint8_t fragCount = 1) {
  TextQueueEntry& e = gState.textQueue[gState.textQueueHead];
  e = TextQueueEntry();
  std::memcpy(e.srcMac, kPeer, 6);
  std::snprintf(e.deviceName, sizeof(e.deviceName), "%s", name);
  if (fill) {
    std::memset(e.content, fill, sizeof(e.content));
  } else {
    std::snprintf(e.content, sizeof(e.content), "%s", content);
  }
  e.msgId = msgId;
  e.fragIndex = fragIndex;
  e.fragCount = fragCount;
  e.encrypted = true;
  e.authenticated = true;
  e.msgType = type;
  e.used = true;
  gState.textQueueHead = (gState.textQueueHead + 1) & (EspNowState::TEXT_QUEUE_SIZE - 1);
}

static bool printedHas(const char* needle) {
  for (const auto& l : gPrinted) if (l.find(needle) != std::string::npos) return true;
  return false;
}
static bool anyOutputHas(const char* needle) {
  if (printedHas(needle)) return true;
  for (const auto& s : gStored) if (s.content.find(needle) != std::string::npos) return true;
  return false;
}
static const char* lastLine() { return gPrinted.empty() ? "(none)" : gPrinted.back().c_str(); }

// One restart in, one record and one console line out, no chat event.
static bool oneNotice() {
  return gStored.size() == 1 && gPrinted.size() == 1 && gStored[0].type == MSG_SYSTEM_EVENT &&
         gStored[0].reqId == 0 && gStored[0].piece == 1 && gStored[0].of == 1 && gTextEvents == 0 &&
         gStored[0].attribution == HistoryAttribution::Attributed;
}

int main() {
  // N1: a paired peer's restart (its heartbeat's boot counter moved forward)
  // reads as a line naming the sender and one "Online (boot #N)" record.
  reset();
  gName = "benchxiao";
  espnowFilePeerRestart(kPeer, 42, false);
  CHECK(oneNotice(), "N1 stored=%zu printed=%zu events=%u", gStored.size(), gPrinted.size(), gTextEvents);
  CHECK(!gPrinted.empty() && gPrinted[0] == "[ESP-NOW] benchxiao is online (boot #42)", "N1 line: %s", lastLine());
  CHECK(!gStored.empty() && gStored[0].content == "Online (boot #42)" && gStored[0].name == "benchxiao" &&
            !gStored[0].encrypted,
        "N1 history text: %s", gStored.empty() ? "(none)" : gStored[0].content.c_str());

  // N2: sealed (the counter came in a sealed heartbeat) is marked encrypted.
  reset();
  espnowFilePeerRestart(kPeer, 7, true);
  CHECK(oneNotice() && gStored[0].encrypted && printedHas("benchxiao is online (boot #7)"), "N2 line: %s",
        lastLine());

  // N3: the largest counter fits the record and the line.
  reset();
  espnowFilePeerRestart(kPeer, 4294967295u, false);
  CHECK(oneNotice() && gStored[0].content == "Online (boot #4294967295)", "N3 history: %s",
        gStored.empty() ? "(none)" : gStored[0].content.c_str());

  // N8: with no registry name the MAC text is used, as for chat.
  reset();
  gName.clear();
  espnowFilePeerRestart(kPeer, 3, false);
  {
    char mac[18];
    macToDisplay(kPeer, mac, sizeof mac);
    const std::string want = std::string("[ESP-NOW] ") + mac + " is online (boot #3)";
    CHECK(oneNotice() && gPrinted[0] == want && gStored[0].name == mac, "N8 line: %s", lastLine());
  }

  // N9: a long name is cut to the record's name field, never overrun.
  reset();
  gName = std::string(60, 'n');
  espnowFilePeerRestart(kPeer, 5, false);
  CHECK(oneNotice() && gStored[0].name.size() < 40, "N9 name length %zu",
        gStored.empty() ? (size_t)0 : gStored[0].name.size());
  gName = "benchxiao";

  // N10: chat is unchanged: printed with its sender, stored per fragment under
  // its msgId, one text event at the last fragment.
  reset();
  queue(MSG_TEXT, "benchxiao", "hello", 21);
  queue(MSG_TEXT, "benchxiao", "part one ", 22, 0, 0, 2);
  queue(MSG_TEXT, "benchxiao", "part two", 22, 0, 1, 2);
  drainTextQueueForTest();
  CHECK(gPrinted.size() == 3 && gPrinted[0] == "[benchxiao [enc]] hello" &&
        gPrinted[2] == "[benchxiao [enc]] (part 2/2) part two", "N10 chat lines: %s", lastLine());
  CHECK(gStored.size() == 3 && gStored[0].type == MSG_TEXT && gStored[0].content == "hello" &&
        gStored[0].reqId == 21 && gStored[2].reqId == 22 && gStored[2].piece == 2 && gStored[2].of == 2,
        "N10 chat records");
  CHECK(gTextEvents == 2, "N10 text events %u", gTextEvents);

  // N11: the drain stores what the queue holds; nothing in it parses JSON any
  // more (a frame of the retired BOOT type has no row and never reaches it).
  reset();
  queue(MSG_TEXT, "benchxiao", "{\"type\":\"BOOT\",\"pld\":{\"boot\":4}}", 31);
  drainTextQueueForTest();
  CHECK(gStored.size() == 1 && gStored[0].type == MSG_TEXT && gTextEvents == 1, "N11 chat stays chat");

  std::printf("espnow_system_notice: %d checks, %d failed\n", gChecks, gFailures);
  return gFailures ? 1 : 0;
}
