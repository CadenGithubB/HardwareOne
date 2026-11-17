// Host harness for test_espnow_system_notice.py.
//
// The runner pastes LogMessageType, MSG_TYPE_BOOT and TextQueueEntry from
// System_ESPNow.h, and from System_ESPNow.cpp v2_init_envelope,
// buildBootNotification, formatEspNowSystemNotice (when the tree has it) and the
// espnow_task text-queue drain ("// 11. Drain text message queue") at the
// INSERT_* tags. ArduinoJson is the vendored library and String is the shared
// host stub. History, the console, the event bus and the G2 push-kick are
// recording doubles; no radio is touched.
#include <Arduino.h>
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "System_MacText.h"  // production MAC text helpers (copied beside this unit)

#ifndef HW1_HAVE_NOTICE
#error "HW1_HAVE_NOTICE must be defined by the runner"
#endif

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

#define PSRAM_JSON_DOC(name) JsonDocument name
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
struct StoredMessage { std::string name, content; bool encrypted; int type; uint32_t reqId; uint8_t piece, of; };
static std::vector<StoredMessage> gStored;
static bool storeMessageInPeerHistory(uint8_t*, const char* name, const char* message, bool encrypted,
                                      LogMessageType type, uint32_t reqId = 0, uint8_t piece = 1,
                                      uint8_t pieceTotal = 1) {
  gStored.push_back({name ? name : "", message ? message : "", encrypted, (int)type, reqId, piece, pieceTotal});
  return true;
}
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

// One system notice in, one record and one console line out, no chat event.
static bool oneNotice() {
  return gStored.size() == 1 && gPrinted.size() == 1 && gStored[0].type == MSG_SYSTEM_EVENT &&
         gStored[0].reqId == 0 && gStored[0].piece == 1 && gStored[0].of == 1 && gTextEvents == 0;
}

int main() {
  // N1: the bench notice (empty src) reads as a line naming the sender.
  reset();
  queue(MSG_SYSTEM_EVENT, "benchxiao",
        "{\"type\":\"BOOT\",\"id\":308,\"src\":\"\",\"ttl\":3,\"pld\":{\"boot\":42,\"ts\":0}}", 308);
  drainTextQueueForTest();
  CHECK(oneNotice(), "N1 stored=%zu printed=%zu events=%u", gStored.size(), gPrinted.size(), gTextEvents);
  CHECK(!gPrinted.empty() && gPrinted[0] == "[ESP-NOW] benchxiao is online (boot #42)", "N1 line: %s", lastLine());
  CHECK(!gStored.empty() && gStored[0].content == "Online (boot #42)" && gStored[0].name == "benchxiao",
        "N1 history text: %s", gStored.empty() ? "(none)" : gStored[0].content.c_str());
  CHECK(!anyOutputHas("{") && !anyOutputHas("\"type\""), "N1 the raw JSON reached the console or history");
  CHECK(gPushKicks == 1, "N1 the G2 inbox was not kicked");

  // N2: the notice's own "src" is never shown; the name is the resolved one.
  reset();
  queue(MSG_SYSTEM_EVENT, "benchxiao",
        "{\"type\":\"BOOT\",\"id\":9,\"src\":\"admin-console\",\"ttl\":3,\"pld\":{\"boot\":7,\"ts\":0}}", 9);
  drainTextQueueForTest();
  CHECK(oneNotice() && !anyOutputHas("admin-console") && printedHas("benchxiao is online (boot #7)"),
        "N2 line: %s", lastLine());

  // N3: malformed JSON gets the neutral line, not the text.
  reset();
  queue(MSG_SYSTEM_EVENT, "benchxiao", "{\"type\":\"BOOT\",\"pld\":{\"boot\":4", 10);
  drainTextQueueForTest();
  CHECK(oneNotice() && gPrinted[0] == "[ESP-NOW] System notice from benchxiao" &&
        gStored[0].content == "System notice", "N3 line: %s", lastLine());

  // N4: a well-formed notice of another type gets the neutral line.
  reset();
  queue(MSG_SYSTEM_EVENT, "benchxiao", "{\"type\":\"REBOOT_ALL\",\"pld\":{\"boot\":4}}", 11);
  drainTextQueueForTest();
  CHECK(oneNotice() && gPrinted[0] == "[ESP-NOW] System notice from benchxiao" && !anyOutputHas("REBOOT_ALL"),
        "N4 line: %s", lastLine());

  // N5: plain text queued as a system event is not echoed either.
  reset();
  queue(MSG_SYSTEM_EVENT, "benchxiao", "run espnowsetname clear", 12);
  drainTextQueueForTest();
  CHECK(oneNotice() && !anyOutputHas("espnowsetname"), "N5 line: %s", lastLine());

  // N6: a BOOT whose counter is not an unsigned number says "online" without one.
  const char* badCounters[] = {
    "{\"type\":\"BOOT\",\"pld\":{\"boot\":-1}}",
    "{\"type\":\"BOOT\",\"pld\":{\"boot\":\"42\"}}",
    "{\"type\":\"BOOT\",\"pld\":{\"boot\":99999999999}}",
    "{\"type\":\"BOOT\"}",
  };
  for (const char* text : badCounters) {
    reset();
    queue(MSG_SYSTEM_EVENT, "benchxiao", text, 13);
    drainTextQueueForTest();
    CHECK(oneNotice() && gPrinted[0] == "[ESP-NOW] benchxiao is online" && gStored[0].content == "Online",
          "N6 %s -> %s", text, lastLine());
  }

  // N7: content that fills the field with no terminator is read within it
  // (the sanitizer build fails on an over-read) and gets the neutral line.
  reset();
  queue(MSG_SYSTEM_EVENT, "benchxiao", nullptr, 14, 'x');
  drainTextQueueForTest();
  CHECK(oneNotice() && gPrinted[0] == "[ESP-NOW] System notice from benchxiao", "N7 line: %s", lastLine());

  // N8: with no resolved name the MAC text is used, as for chat.
  reset();
  queue(MSG_SYSTEM_EVENT, "", "{\"type\":\"BOOT\",\"pld\":{\"boot\":3}}", 15);
  drainTextQueueForTest();
  {
    char mac[18];
    macToDisplay(kPeer, mac, sizeof mac);
    const std::string want = std::string("[ESP-NOW] ") + mac + " is online (boot #3)";
    CHECK(oneNotice() && gPrinted[0] == want, "N8 line: %s", lastLine());
  }

  // N9: the sender's notice, as buildBootNotification makes it, round-trips.
  reset();
  {
    const String built = buildBootNotification(308, "benchxiao", 42, 0);
    JsonDocument doc;
    CHECK(!deserializeJson(doc, built.c_str()) && std::strcmp(doc["src"] | "", "benchxiao") == 0 &&
          std::strcmp(doc["type"] | "", "BOOT") == 0 && (doc["pld"]["boot"] | 0u) == 42u,
          "N9 built notice: %s", built.c_str());
    queue(MSG_SYSTEM_EVENT, "benchxiao", built.c_str(), 308);
    drainTextQueueForTest();
    CHECK(oneNotice() && gPrinted[0] == "[ESP-NOW] benchxiao is online (boot #42)", "N9 line: %s", lastLine());
  }

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

#if HW1_HAVE_NOTICE
  // N11: the formatter truncates into small buffers without overflow.
  {
    char history[6];
    char line[12];
    const char* text = "{\"type\":\"BOOT\",\"pld\":{\"boot\":4294967295}}";
    formatEspNowSystemNotice(text, std::strlen(text) + 1, "a-rather-long-device-name",
                             history, sizeof(history), line, sizeof(line));
    CHECK(std::strlen(history) == sizeof(history) - 1 && std::strlen(line) == sizeof(line) - 1,
          "N11 truncated '%s' '%s'", history, line);
    formatEspNowSystemNotice(nullptr, 0, nullptr, history, sizeof(history), line, sizeof(line));
    CHECK(std::strcmp(history, "Syste") == 0, "N11 null text: '%s'", history);
  }
#else
  CHECK(false, "N11 formatEspNowSystemNotice is absent");
#endif

  std::printf("espnow_system_notice: %d checks, %d failed\n", gChecks, gFailures);
  return gFailures ? 1 : 0;
}
