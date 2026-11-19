// espnowsessions (cmd_espnow_sessions, System_ESPNow.cpp) over a session table
// double: with every slot ACTIVE and every capability named, the JSON form is
// whole JSON that fits the debug buffer (the last sessions left out and
// counted in "more"), and the text form ends in a NUL and names how many
// sessions it left out. test_espnow_sessions_output.py inserts the production
// function.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "Arduino.h"  // web_batch_stubs: the host String
#include <ArduinoJson.h>
#include "psram_json_doc_host.h"
#include "System_MacText.h"
#include "System_ESPNow_SessionCapsCore.h"

void webBatchTestStringAppend() {}

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                    \
  do {                                                      \
    ++checks;                                               \
    if (!(cond)) {                                          \
      ++failures;                                           \
      std::printf("FAIL line %d: ", __LINE__);              \
      std::printf(__VA_ARGS__);                             \
      std::printf("\n");                                    \
    }                                                       \
  } while (0)

#define EXT_RAM_BSS_ATTR
#define MAC_STR(mac)                                            \
  ({ static char _macbuf[18];                                   \
     snprintf(_macbuf, sizeof(_macbuf), MAC_FMT, MAC_ARGS(mac)); \
     (const char*)_macbuf; })
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (0)
constexpr size_t GLOBAL_DEBUG_BUFFER_SIZE = 4096;  // System_Debug.h
static char gDebug[GLOBAL_DEBUG_BUFFER_SIZE];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return gDebug; }
static bool argWantsJson(const String& args) { return std::strcmp(args.c_str(), "json") == 0; }
static uint32_t millis() { return 600000; }
static uint32_t gSelfCaps = 0;
static uint32_t espnowLocalSessionCaps() { return gSelfCaps; }

enum SessionStateLifecycle : uint8_t {
  SESSION_FREE = 0, SESSION_ESTABLISHING = 1, SESSION_ACTIVE = 2, SESSION_REKEYING = 3, SESSION_CLOSED = 4,
};
struct SessionState {
  uint8_t peerMac[6];
  uint8_t meshId;
  uint16_t sessionId;
  uint8_t myDirection;
  uint8_t state;
  uint32_t establishedAtMs;
  uint32_t txSeqNext;
  uint32_t rxSeqHighWater;
  uint32_t peerSessionCaps;
};
static SessionState gSessions[16];
static uint8_t sessionSlotCount() { return 16; }
static const SessionState* sessionAt(uint8_t i) {
  return (i < 16 && gSessions[i].state != SESSION_FREE) ? &gSessions[i] : nullptr;
}

// INSERT_PRODUCTION

// `n` sessions, the widest a real table holds: ACTIVE, every bit set, the
// largest numbers.
static void fill(unsigned n) {
  std::memset(gSessions, 0, sizeof gSessions);
  for (unsigned i = 0; i < n; i++) {
    SessionState& s = gSessions[i];
    for (int b = 0; b < 6; b++) s.peerMac[b] = (uint8_t)(0xA0 + i);
    s.meshId = 3;
    s.sessionId = 65535;
    s.myDirection = (uint8_t)(i & 1);
    s.state = SESSION_ACTIVE;
    s.establishedAtMs = 1;
    s.txSeqNext = 4294967295u;
    s.rxSeqHighWater = 4294967295u;
    s.peerSessionCaps = 0xFFFFFFFFu;
  }
}

static size_t terminated() {
  const void* nul = std::memchr(gDebug, '\0', sizeof gDebug);
  return nul ? (size_t)((const char*)nul - gDebug) : sizeof gDebug;
}

int main() {
  gSelfCaps = 0xFFFFFFFFu;
  for (unsigned n : {0u, 1u, 3u, 4u, 8u, 12u, 16u}) {
    fill(n);
    std::memset(gDebug, 'X', sizeof gDebug);
    const char* out = cmd_espnow_sessions(String("json"));
    const size_t len = terminated();
    CHECK(out == gDebug && len < sizeof gDebug, "%u sessions: the JSON reply has no terminator in the buffer", n);
    if (len >= sizeof gDebug) continue;
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, gDebug);
    CHECK(!err, "%u sessions: the JSON reply is not whole JSON (%s, %zu bytes)", n, err.c_str(), len);
    if (err) continue;
    const unsigned listed = (unsigned)doc["sessions"].as<JsonArrayConst>().size();
    const unsigned more = doc["more"] | 0u;
    CHECK(listed + more == n && listed >= (n < 12 ? n : 12u),
          "%u sessions: %u listed and %u counted in more", n, listed, more);
    CHECK(doc["schema"] == 1 && doc["selfCaps"].as<JsonArrayConst>().size() == sessioncaps::kBitCount,
          "%u sessions: schema and selfCaps", n);
    if (n > 0) {
      JsonObjectConst first = doc["sessions"][0];
      CHECK(first["caps"].as<JsonArrayConst>().size() == sessioncaps::kBitCount &&
                first["capsWord"] == 4294967295u && first["state"] == "ACTIVE",
            "%u sessions: the first entry carries its caps", n);
    }
    std::printf("json %2u sessions: %4zu bytes, %u listed, %u more\n", n, len, listed, more);

    std::memset(gDebug, 'X', sizeof gDebug);
    cmd_espnow_sessions(String(""));
    const size_t textLen = terminated();
    CHECK(textLen < sizeof gDebug, "%u sessions: the text reply has no terminator in the buffer", n);
    if (textLen >= sizeof gDebug) continue;
    unsigned lines = 0;
    for (const char* q = std::strstr(gDebug, "  ["); q; q = std::strstr(q + 1, "\n  [")) ++lines;
    unsigned notShown = 0;
    if (const char* m = std::strstr(gDebug, "more not shown")) {
      const char* open = m;
      while (open > gDebug && *open != '(') --open;
      notShown = (unsigned)std::strtoul(open + 1, nullptr, 10);
    }
    CHECK(lines + notShown == n && lines >= (n < 12 ? n : 12u),
          "%u sessions: the text lists %u and says %u more not shown", n, lines, notShown);
    std::printf("text %2u sessions: %4zu bytes, %u lines, %u not shown\n", n, textLen, lines, notShown);
  }
  std::printf("espnow_sessions_output: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
