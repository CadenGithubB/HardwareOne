// Host harness for the ESP-NOW metadata push (test_espnow_metadata_push.py).
// The runner compiles the production metadata setters (metaGetSet,
// cmd_espnow_stationary, cmd_espnow_setname), the payload builder, the
// encrypted sender and the push tick beside these doubles; the session table,
// identity subscriptions, the encrypted transport, settings and the clock are
// the boundaries. A failed CHECK is reported and the run continues, so one run
// names every case that fails; the exit status is non-zero if any failed.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <Arduino.h>  // test/host/web_batch_stubs String (shared, read-only)
void webBatchTestStringAppend() {}

static unsigned checks = 0, failures = 0;
static const char* currentCase = "";
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
  std::fprintf(stderr, "%s:%d: [%s] CHECK failed: %s\n", __FILE__, __LINE__, currentCase, #condition); \
  } } while (false)

// ---- clock ---------------------------------------------------------------
static uint32_t gNowMs = 0;
uint32_t millis() { return gNowMs; }

// ---- logging doubles -----------------------------------------------------
static unsigned warns = 0, infos = 0;
template <typename... A> void quietLog(const char*, A...) {}
template <typename... A> void warnLog(const char*, A...) { ++warns; }
template <typename... A> void infoLog(const char*, A...) { ++infos; }
#define DEBUG_ESPNOW_METADATAF(...) quietLog(__VA_ARGS__)
#define WARN_ESPNOWF(...) warnLog(__VA_ARGS__)
#define INFO_ESPNOWF(...) infoLog(__VA_ARGS__)
static const char* macStr(const uint8_t* m) {
  static char b[18];
  std::snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}
#define MAC_STR(m) macStr(m)

// ---- settings and command plumbing ---------------------------------------
struct Settings {
  String espnowDeviceName, espnowFriendlyName, espnowRoom, espnowZone, espnowTags;
  bool espnowStationary = false;
  bool espnowFirstTimeSetup = false;
};
static Settings gSettings;
template <typename T> void setSetting(T& field, const T& value) { field = value; }
static unsigned events = 0;
enum { SYSEVT_SETTING_CHANGED = 7 };
void systemEventPost(int, const char*, const char*) { ++events; }
static char debugBuf[1024];
char* getDebugBuffer() { return debugBuf; }
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (false)
class CommandArgs {
  String a_;
 public:
  explicit CommandArgs(const String& in) : a_(in) { a_.trim(); }
  int count() const { return a_.length() ? 1 : 0; }
  String arg(int) const { return a_; }
  bool argBool(int, bool d = false) const {
    if (!count()) return d;
    return a_ == "1" || a_ == "on" || a_ == "true";
  }
};

// ---- ESP-NOW runtime -----------------------------------------------------
struct EspNowDevice { uint8_t mac[6]; String name; };
struct EspNowState {
  bool initialized = true;
  int deviceCount = 0;
  EspNowDevice devices[4];
};
static EspNowState state;
static EspNowState* gEspNow = &state;
typedef int esp_err_t;
static const uint8_t kSelfMac[6] = {0x02, 0, 0, 0, 0, 0x01};
esp_err_t espnowSelfStaMac(uint8_t out[6]) { std::memcpy(out, kSelfMac, 6); return 0; }
void addEspNowDevice(const uint8_t* mac, const String& name, bool, const uint8_t*) {
  if (state.deviceCount >= 4) return;
  std::memcpy(state.devices[state.deviceCount].mac, mac, 6);
  state.devices[state.deviceCount].name = name;
  ++state.deviceCount;
}
uint32_t generateMessageId() { static uint32_t n = 100; return ++n; }
constexpr uint8_t ESPNOW_V4_TYPE_METADATA_RESP = 54, ESPNOW_V4_TYPE_METADATA_PUSH = 55;
// INSERT_WIRE
// INSERT_SESSIONS
// INSERT_IDENTITY

// Session table double, read through the production accessor signatures.
static std::vector<SessionState> sessions;
uint8_t sessionSlotCount() { return (uint8_t)sessions.size(); }
const SessionState* sessionAt(uint8_t i) { return i < sessions.size() ? &sessions[i] : nullptr; }
static void addSession(uint8_t last, uint8_t lifecycle) {
  SessionState s;
  std::memset(&s, 0, sizeof s);
  s.peerMac[0] = 0x02; s.peerMac[5] = last;
  s.state = lifecycle;
  sessions.push_back(s);
}
static std::vector<uint8_t> optedOut;  // peers whose subscription excludes METADATA_PUSH
bool peerIdentityWantsEvent(const uint8_t mac[6], uint32_t category) {
  if (category != ESPNOW_EVT_METADATA_PUSH) return true;
  for (uint8_t m : optedOut) if (m == mac[5]) return false;
  return true;
}

// Transport double: records every encrypted send.
struct Sent { uint8_t dst[6]; uint8_t type; V4PayloadMetadata payload; };
static std::vector<Sent> sent;
static bool sendFails = false;
bool v4_send_encrypted_or_queue(const uint8_t dst[6], uint8_t type, uint16_t, uint32_t,
                                const uint8_t* plaintext, uint16_t len, uint8_t,
                                char* outStatus, size_t outLen) {
  if (sendFails) {
    if (outStatus && outLen) std::snprintf(outStatus, outLen, "pending queue full");
    return false;
  }
  CHECK(len == sizeof(V4PayloadMetadata));
  Sent s;
  std::memset(&s, 0, sizeof s);
  std::memcpy(s.dst, dst, 6);
  s.type = type;
  std::memcpy(&s.payload, plaintext, len < sizeof s.payload ? len : sizeof s.payload);
  sent.push_back(s);
  if (outStatus && outLen) std::snprintf(outStatus, outLen, "sent");
  return true;
}

// Debounce statics that sendMetadata uses (defined beside it in production).
static uint32_t sLastMetadataSendMs = 0;
static const uint32_t METADATA_DEBOUNCE_MS = 3000;

// INSERT_TRACKING
// INSERT_PRODUCTION
// INSERT_TICK

static unsigned pushes() {
  unsigned n = 0;
  for (const Sent& s : sent) if (s.type == ESPNOW_V4_TYPE_METADATA_PUSH) ++n;
  return n;
}
// Out-of-range reads return an all-zero frame so a failing case cannot index past the log.
static const Sent& sentAt(size_t i) {
  static const Sent none = {};
  return i < sent.size() ? sent[i] : none;
}

static void reset(const char* name) {
  currentCase = name;
  sessions.clear(); optedOut.clear(); sent.clear();
  sendFails = false; gNowMs = 10000; gMetadataChanged = false;
  gSettings = Settings{}; warns = infos = events = 0;
  state.initialized = true; state.deviceCount = 0;
  metadataPushTick(gNowMs);  // disarms any pending settle window
  sLastMetadataSendMs = 0;
}

// Arms the settle window, then advances past it; returns what the due tick returned.
static int settleAndTick() {
  metadataPushTick(gNowMs);
  gNowMs += METADATA_PUSH_SETTLE_MS;
  return metadataPushTick(gNowMs);
}

int main() {
  reset("P1 room change is pushed after the settle window");
  addSession(0x11, SESSION_ACTIVE);
  CHECK(std::strcmp(metaGetSet(String("Kitchen"), gSettings.espnowRoom, "Room", 31),
                    "Room set to: Kitchen") == 0);
  CHECK(gMetadataChanged);
  CHECK(metadataPushTick(gNowMs) == 0 && sent.empty());          // arms the settle window
  gNowMs += METADATA_PUSH_SETTLE_MS - 1;
  CHECK(metadataPushTick(gNowMs) == 0 && sent.empty());          // not due yet
  gNowMs += 1;
  CHECK(metadataPushTick(gNowMs) == 1);
  CHECK(pushes() == 1 && sent.size() == 1);
  CHECK(sentAt(0).type == ESPNOW_V4_TYPE_METADATA_PUSH && sentAt(0).dst[5] == 0x11);
  CHECK(std::strcmp(sentAt(0).payload.room, "Kitchen") == 0);
  CHECK(!gMetadataChanged);
  CHECK(infos == 1);
  gNowMs += 5000;
  CHECK(metadataPushTick(gNowMs) == 0 && pushes() == 1);         // no repeat without a change

  reset("P2 only ACTIVE sessions that subscribe to METADATA_PUSH");
  addSession(0x11, SESSION_ACTIVE);
  addSession(0x22, SESSION_ESTABLISHING);
  addSession(0x33, SESSION_ACTIVE); optedOut.push_back(0x33);
  addSession(0x44, SESSION_REKEYING);
  addSession(0x55, SESSION_CLOSED);
  addSession(0x66, SESSION_ACTIVE);
  addSession(0x77, SESSION_FREE);
  metaGetSet(String("Lab"), gSettings.espnowZone, "Zone", 31);
  CHECK(settleAndTick() == 2);
  CHECK(pushes() == 2 && sentAt(0).dst[5] == 0x11 && sentAt(1).dst[5] == 0x66);
  CHECK(std::strcmp(sentAt(1).payload.zone, "Lab") == 0);

  reset("P3 a burst of edits yields one PUSH carrying all of them");
  addSession(0x11, SESSION_ACTIVE);
  metaGetSet(String("Kitchen"), gSettings.espnowRoom, "Room", 31);
  metadataPushTick(gNowMs);
  gNowMs += 500;
  metaGetSet(String("north"), gSettings.espnowZone, "Zone", 31);
  metaGetSet(String("\"a,b\""), gSettings.espnowTags, "Tags", 53);
  metaGetSet(String("Bench"), gSettings.espnowFriendlyName, "Friendly name", 47);
  CHECK(metadataPushTick(gNowMs) == 0 && sent.empty());
  gNowMs += METADATA_PUSH_SETTLE_MS;
  CHECK(metadataPushTick(gNowMs) == 1 && pushes() == 1);
  CHECK(std::strcmp(sentAt(0).payload.room, "Kitchen") == 0);
  CHECK(std::strcmp(sentAt(0).payload.zone, "north") == 0);
  CHECK(std::strcmp(sentAt(0).payload.tags, "a,b") == 0);
  CHECK(std::strcmp(sentAt(0).payload.friendlyName, "Bench") == 0);

  reset("P4 nothing changed: no sends");
  addSession(0x11, SESSION_ACTIVE);
  for (int i = 0; i < 10; ++i) { gNowMs += 1000; CHECK(metadataPushTick(gNowMs) == 0); }
  CHECK(sent.empty() && !gMetadataChanged && infos == 0);

  reset("P5 espnowstationary marks metadata dirty");
  addSession(0x11, SESSION_ACTIVE);
  cmd_espnow_stationary(String("1"));
  CHECK(gSettings.espnowStationary);
  CHECK(gMetadataChanged);
  CHECK(settleAndTick() == 1 && sentAt(0).payload.stationary == 1);

  reset("P6 espnowsetname marks metadata dirty");
  addSession(0x11, SESSION_ACTIVE);
  CHECK(std::strcmp(cmd_espnow_setname(String("bench-2")), "Device name set to: bench-2") == 0);
  CHECK(gSettings.espnowDeviceName == String("bench-2") && state.deviceCount == 1);
  CHECK(gMetadataChanged);
  CHECK(settleAndTick() == 1 && std::strcmp(sentAt(0).payload.deviceName, "bench-2") == 0);

  reset("P7 a change made while ESP-NOW is down is kept, not dropped");
  addSession(0x11, SESSION_ACTIVE);
  state.initialized = false;
  metaGetSet(String("Garage"), gSettings.espnowRoom, "Room", 31);
  for (int i = 0; i < 5; ++i) { gNowMs += 1000; CHECK(metadataPushTick(gNowMs) == 0); }
  CHECK(gMetadataChanged && sent.empty());
  state.initialized = true;
  CHECK(settleAndTick() == 1 && pushes() == 1);

  reset("P8 the settle deadline is millis()-wrap safe");
  addSession(0x11, SESSION_ACTIVE);
  gNowMs = 0xFFFFFF00u;
  metaGetSet(String("Attic"), gSettings.espnowRoom, "Room", 31);
  CHECK(metadataPushTick(gNowMs) == 0);
  gNowMs = 0x00000100u;                                           // 512 ms after arming, past the wrap
  CHECK(metadataPushTick(gNowMs) == 0 && sent.empty());
  gNowMs = 0x00000600u;                                           // 1792 ms after arming
  CHECK(metadataPushTick(gNowMs) == 1 && pushes() == 1);

  reset("P9 a failed send clears the flag (no retry storm)");
  addSession(0x11, SESSION_ACTIVE);
  sendFails = true;
  metaGetSet(String("Porch"), gSettings.espnowRoom, "Room", 31);
  CHECK(settleAndTick() == 1 && sent.empty() && warns == 1);
  CHECK(!gMetadataChanged);
  gNowMs += 5000;
  CHECK(metadataPushTick(gNowMs) == 0 && warns == 1);

  reset("P10 the deferred RESP path is unchanged");
  {
    const uint8_t peer[6] = {0x02, 0, 0, 0, 0, 0x77};
    sendMetadata(peer, false, true);
    sendMetadata(peer, false, true);
    CHECK(sent.size() == 2 && sentAt(0).type == ESPNOW_V4_TYPE_METADATA_RESP && pushes() == 0);
    sendMetadata(peer, false, false);                              // debounced
    CHECK(sent.size() == 2);
  }

  reset("P11 a clear is pushed; a rejected value is not");
  addSession(0x11, SESSION_ACTIVE);
  gSettings.espnowRoom = "Old";
  CHECK(std::strcmp(metaGetSet(String("clear"), gSettings.espnowRoom, "Room", 31), "Room cleared") == 0);
  CHECK(settleAndTick() == 1 && sentAt(0).payload.room[0] == '\0');
  sent.clear();
  metaGetSet(String("this-room-name-is-longer-than-31-bytes"), gSettings.espnowRoom, "Room", 31);
#ifdef HW1_HAVE_METADATA_POLICY
  metaGetSet(String("<b>"), gSettings.espnowRoom, "Room", 31);
#endif
  CHECK(!gMetadataChanged);
  CHECK(settleAndTick() == 0 && sent.empty());

  std::printf("espnow_metadata_push: %u checks, %u failed\n", checks, failures);
  return failures ? 1 : 0;
}
