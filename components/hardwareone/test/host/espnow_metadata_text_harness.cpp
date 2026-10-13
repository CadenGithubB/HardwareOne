// Host harness for test_espnow_metadata_text.py.
//
// The runner pastes the production metadata types (EspNowInlineText, MeshPeerMeta,
// V4PayloadMetadata, V4RxCtx) at INSERT_TYPES and the production functions
// (metadata text policy when present, normalizeMetadataWireText,
// v4h_metadata_resp_push, processMetadata, metaGetSet) at INSERT_PRODUCTION.
// Everything else here is a small double: the registry, the mesh cache, settings
// writes, events, logging and the debug buffer. No transport or flash is touched.
#include <Arduino.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>

void webBatchTestStringAppend() {}

static int gFailures = 0;
#define CHECK(cond, ...)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      ++gFailures;                                                    \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                \
      std::printf(__VA_ARGS__);                                       \
      std::printf("\n");                                              \
    }                                                                 \
  } while (0)

// ---- Logging doubles: WARN lines are kept so the suite can check what they carry.
static std::string gWarnLog;
static int gWarnCount = 0;
__attribute__((format(printf, 1, 2))) static void captureWarn(const char* fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  gWarnLog += line;
  gWarnLog += '\n';
  ++gWarnCount;
}
// Debug lines are formatted (so the arguments are evaluated) and discarded.
__attribute__((format(printf, 1, 2))) static void discardDebug(const char* fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
}
#define WARN_ESPNOWF(...) captureWarn(__VA_ARGS__)
#define DEBUG_ESPNOW_METADATAF(...) discardDebug(__VA_ARGS__)

static const char* MAC_STR(const uint8_t* m) {
  static char buf[18];
  std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return buf;
}

static uint32_t gNowMs = 1000;
static uint32_t millis() { return gNowMs; }

// ---- Transport doubles for V4RxCtx (only the fields the handler reads).
struct esp_now_recv_info {
  const uint8_t* src_addr;
};
struct EspNowV4Header {
  uint8_t type;
  uint32_t msgId;
};
enum : uint8_t { ESPNOW_V4_TYPE_METADATA_RESP = 54, ESPNOW_V4_TYPE_METADATA_PUSH = 55 };

// INSERT_TYPES

// ---- Registry and mesh cache doubles.
struct EspNowDevice {
  uint8_t mac[6];
  EspNowInlineText<32> name;
  EspNowInlineText<48> friendlyName;
  EspNowInlineText<32> room;
  EspNowInlineText<32> zone;
  EspNowInlineText<64> tags;
  bool stationary;
};
struct EspNowState {
  EspNowDevice devices[4];
  int deviceCount;
  bool deferredMetadataPending;
  uint8_t deferredMetadataSrcMac[6];
  uint8_t deferredMetadataPayload[216];
};
static EspNowState gState{};
static EspNowState* gEspNow = &gState;
static MeshPeerMeta gMetaSlots[4];
static MeshPeerMeta* gMeshPeerMeta = gMetaSlots;
static int gMeshPeerSlots = 4;

static int gSaves = 0;
static bool saveEspNowDevices() {
  ++gSaves;
  return true;
}

// ---- Settings / event doubles for metaGetSet.
static bool gMetadataChanged = false;
static char gDebugBuffer[1024];
static char* getDebugBuffer() { return gDebugBuffer; }
enum SystemEventKind : uint8_t { SYSEVT_SETTING_CHANGED = 7 };
static int gEvents = 0;
static std::string gLastEventDetail;
static void systemEventPost(uint8_t kind, const char*, const char* detail) {
  if (kind == SYSEVT_SETTING_CHANGED) ++gEvents;
  gLastEventDetail = detail ? detail : "";
}
static int gSettingWrites = 0;
static void setSetting(String& field, const String& value) {
  ++gSettingWrites;
  field = value;
}

// INSERT_PRODUCTION

// ---- Test helpers.
static bool hostileByte(char c) {
  unsigned char u = static_cast<unsigned char>(c);
  return u < 0x20 || u == 0x7F || std::strchr("<>\"'&\\", c) != nullptr;
}
static bool hasHostileByte(const char* s) {
  for (; *s; ++s) {
    if (hostileByte(*s)) return true;
  }
  return false;
}
static MeshPeerMeta* cacheFor(const uint8_t mac[6]) {
  for (int i = 0; i < gMeshPeerSlots; ++i) {
    if (gMeshPeerMeta[i].isActive && std::memcmp(gMeshPeerMeta[i].mac, mac, 6) == 0) return &gMeshPeerMeta[i];
  }
  return nullptr;
}

// Runs the receive path as the firmware does: the RX handler fills the deferred
// slot, espnow_task (step 10) passes that slot to processMetadata.
static void receiveMetadata(const uint8_t mac[6], const V4PayloadMetadata& payload) {
  esp_now_recv_info info{mac};
  EspNowV4Header header{ESPNOW_V4_TYPE_METADATA_RESP, 42};
  V4RxCtx ctx{};
  ctx.recv_info = &info;
  ctx.h = &header;
  ctx.payload = reinterpret_cast<const uint8_t*>(&payload);
  ctx.payloadLen = static_cast<uint16_t>(sizeof(V4PayloadMetadata));
  ctx.isPaired = true;
  ctx.isAuthenticated = true;
  ctx.isSessionEncrypted = true;
  ctx.deviceName = "peer";
  v4h_metadata_resp_push(ctx);
  CHECK(gEspNow->deferredMetadataPending, "RX handler did not queue the metadata for espnow_task");
  if (!gEspNow->deferredMetadataPending) return;
  gEspNow->deferredMetadataPending = false;
  processMetadata(gEspNow->deferredMetadataSrcMac,
                  reinterpret_cast<const V4PayloadMetadata*>(gEspNow->deferredMetadataPayload));
}

static void resetSetterDoubles() {
  gMetadataChanged = false;
  gEvents = 0;
  gSettingWrites = 0;
  gLastEventDetail.clear();
}

static bool startsWith(const char* s, const char* prefix) { return std::strncmp(s, prefix, std::strlen(prefix)) == 0; }

// M1: hostile peer text reaches neither the mesh cache nor the persisted registry.
static void testWireScrub() {
  const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
  EspNowDevice& dev = gState.devices[0];
  std::memcpy(dev.mac, mac, 6);
  gState.deviceCount = 1;
  gSaves = 0;
  gWarnLog.clear();
  gWarnCount = 0;

  V4PayloadMetadata p;
  std::memset(&p, 0, sizeof(p));
  std::strcpy(p.deviceName, "peer\x01" "B");
  std::strcpy(p.friendlyName, "a\"b&c'd\\e");
  std::strcpy(p.room, "<b>x</b>");
  std::memset(p.zone, 'z', sizeof(p.zone));  // fully occupied field, no NUL on the wire
  std::strcpy(p.tags, "t1,<img src=x onerror=1>");
  p.stationary = 1;
  receiveMetadata(mac, p);

  MeshPeerMeta* m = cacheFor(mac);
  CHECK(m != nullptr, "M1: no mesh cache slot for the sender");
  if (!m) return;
  CHECK(!hasHostileByte(m->room), "M1: cache room stores peer markup verbatim: '%s'", m->room);
  CHECK(!hasHostileByte(m->friendlyName), "M1: cache friendlyName stores quotes/&/\\ verbatim: '%s'", m->friendlyName);
  CHECK(!hasHostileByte(m->tags), "M1: cache tags store peer markup verbatim: '%s'", m->tags);
  CHECK(!hasHostileByte(m->name), "M1: cache name stores a control byte verbatim");
  CHECK(!hasHostileByte(m->zone), "M1: cache zone has a disallowed byte");
  CHECK(!hasHostileByte(dev.room.c_str()), "M1: persisted registry room stores peer markup verbatim: '%s'", dev.room.c_str());
  CHECK(!hasHostileByte(dev.friendlyName.c_str()), "M1: persisted registry friendlyName stores '%s' verbatim",
        dev.friendlyName.c_str());
  CHECK(!hasHostileByte(dev.tags.c_str()), "M1: persisted registry tags store '%s' verbatim", dev.tags.c_str());
  // Replacement, not deletion: field lengths and the clean bytes are kept.
  CHECK(std::strcmp(m->room, "_b_x_/b_") == 0, "M1: cache room '%s', expected '_b_x_/b_'", m->room);
  CHECK(std::strcmp(m->friendlyName, "a_b_c_d_e") == 0, "M1: cache friendlyName '%s', expected 'a_b_c_d_e'",
        m->friendlyName);
  CHECK(std::strcmp(m->name, "peer_B") == 0, "M1: cache name '%s', expected 'peer_B'", m->name);
  CHECK(std::strcmp(m->tags, "t1,_img src=x onerror=1_") == 0, "M1: cache tags '%s'", m->tags);
  CHECK(std::strlen(m->zone) == 31, "M1: unterminated wire zone stored with length %zu, expected 31",
        std::strlen(m->zone));
  CHECK(dev.room == "_b_x_/b_", "M1: registry room '%s', expected '_b_x_/b_'", dev.room.c_str());
  CHECK(m->stationary && dev.stationary, "M1: stationary flag not stored");
  CHECK(gSaves == 1, "M1: registry saved %d times, expected 1", gSaves);
  // One WARN with the count and the sender MAC only, never the raw text.
  CHECK(gWarnCount == 1, "M1: %d WARN lines for a payload with replaced bytes, expected 1", gWarnCount);
  CHECK(gWarnLog.find("replaced 11 disallowed byte(s)") != std::string::npos,
        "M1: WARN does not report the 11 replaced bytes: '%s'", gWarnLog.c_str());
  CHECK(gWarnLog.find("02:00:00:00:00:02") != std::string::npos, "M1: WARN does not name the sender MAC");
  CHECK(gWarnLog.find('<') == std::string::npos && gWarnLog.find('"') == std::string::npos,
        "M1: WARN carries raw peer text: '%s'", gWarnLog.c_str());
}

// M2: clean ASCII and UTF-8 metadata is stored byte-identical, without a WARN.
static void testCleanPayloadUnchanged() {
  const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x03};
  EspNowDevice& dev = gState.devices[1];
  std::memcpy(dev.mac, mac, 6);
  gState.deviceCount = 2;
  gSaves = 0;
  gWarnLog.clear();
  gWarnCount = 0;

  V4PayloadMetadata p;
  std::memset(&p, 0, sizeof(p));
  std::strcpy(p.deviceName, "peerB");
  std::strcpy(p.friendlyName, "K\xC3\xBC" "che (Nord)");
  std::strcpy(p.room, "Living-Room 2");
  std::strcpy(p.zone, "Upstairs/East");
  std::strcpy(p.tags, "stationary,thermal;rev=2.1");
  receiveMetadata(mac, p);

  MeshPeerMeta* m = cacheFor(mac);
  CHECK(m != nullptr, "M2: no mesh cache slot for the sender");
  if (!m) return;
  CHECK(std::strcmp(m->name, "peerB") == 0, "M2: clean name changed to '%s'", m->name);
  CHECK(std::strcmp(m->friendlyName, "K\xC3\xBC" "che (Nord)") == 0, "M2: UTF-8 friendlyName changed to '%s'",
        m->friendlyName);
  CHECK(std::strcmp(m->room, "Living-Room 2") == 0, "M2: clean room changed to '%s'", m->room);
  CHECK(std::strcmp(m->zone, "Upstairs/East") == 0, "M2: clean zone changed to '%s'", m->zone);
  CHECK(std::strcmp(m->tags, "stationary,thermal;rev=2.1") == 0, "M2: clean tags changed to '%s'", m->tags);
  CHECK(dev.friendlyName == "K\xC3\xBC" "che (Nord)" && dev.room == "Living-Room 2" && dev.zone == "Upstairs/East" &&
            dev.tags == "stationary,thermal;rev=2.1",
        "M2: clean registry values changed");
  CHECK(gWarnCount == 0, "M2: clean payload produced a WARN: '%s'", gWarnLog.c_str());
}

// M3: the local setters reject control bytes and < > " ' & \ before any write or event.
static void testSetterRejects() {
  const char* hostile[] = {"<svg onload=alert(1)>", "a\tb", "Tom&Co", "back\\slash", "Tom's room",
                           "say \"hi\" now", "bell\x07", "del\x7F"};
  for (const char* value : hostile) {
    String field("Kitchen");
    resetSetterDoubles();
    const char* r = metaGetSet(String(value), field, "Room", 31);
    CHECK(startsWith(r, "Error: Room may not contain"), "M3: setter accepted disallowed text, replied '%s'", r);
    CHECK(field == String("Kitchen"), "M3: setter stored '%s'", field.c_str());
    CHECK(gSettingWrites == 0 && gEvents == 0 && !gMetadataChanged,
          "M3: rejected value still wrote the setting (%d), posted events (%d) or marked metadata dirty",
          gSettingWrites, gEvents);
  }
  String tags;
  resetSetterDoubles();
  const char* r = metaGetSet(String("t1,<img src=x onerror=alert(1)>"), tags, "Tags", 53);
  CHECK(startsWith(r, "Error: Tags may not contain"), "M3: tags setter accepted markup, replied '%s'", r);
  CHECK(tags.length() == 0 && gSettingWrites == 0, "M3: tags setter stored '%s'", tags.c_str());
}

// M4: everything else the setter does is unchanged.
static void testSetterPreserved() {
  String field;
  resetSetterDoubles();
  const char* r = metaGetSet(String(""), field, "Room", 31);
  CHECK(std::strcmp(r, "Room: (not set)") == 0, "M4: empty-args display '%s'", r);
  CHECK(gSettingWrites == 0 && gEvents == 0, "M4: display wrote the setting");

  resetSetterDoubles();
  r = metaGetSet(String("  \"Kitchen 2\"  "), field, "Room", 31);
  CHECK(std::strcmp(r, "Room set to: Kitchen 2") == 0, "M4: quoted value reply '%s'", r);
  CHECK(field == String("Kitchen 2"), "M4: quoted value stored '%s'", field.c_str());
  CHECK(gSettingWrites == 1 && gEvents == 1 && gMetadataChanged && gLastEventDetail == "Kitchen 2",
        "M4: accepted value: writes=%d events=%d changed=%d detail='%s'", gSettingWrites, gEvents,
        (int)gMetadataChanged, gLastEventDetail.c_str());

  resetSetterDoubles();
  r = metaGetSet(String(""), field, "Room", 31);
  CHECK(std::strcmp(r, "Room: Kitchen 2") == 0, "M4: display of a set value '%s'", r);

  resetSetterDoubles();
  r = metaGetSet(String("K\xC3\xBC" "che"), field, "Room", 31);
  CHECK(std::strcmp(r, "Room set to: K\xC3\xBC" "che") == 0, "M4: UTF-8 value reply '%s'", r);

  resetSetterDoubles();
  r = metaGetSet(String("living-room_2 (north), floor 1; v2.0/A"), field, "Tags", 53);
  CHECK(startsWith(r, "Tags set to: living-room_2"), "M4: punctuation-only clean value rejected: '%s'", r);

  resetSetterDoubles();
  r = metaGetSet(String("clear"), field, "Room", 31);
  CHECK(std::strcmp(r, "Room cleared") == 0 && field.length() == 0 && gEvents == 1 && gMetadataChanged,
        "M4: clear reply '%s'", r);

  resetSetterDoubles();
  r = metaGetSet(String(std::string(32, 'r')), field, "Room", 31);
  CHECK(std::strcmp(r, "Error: Room too long (max 31 chars)") == 0, "M4: length error '%s'", r);
  CHECK(gSettingWrites == 0, "M4: over-long value was written");
}

#if HW1_HAVE_METADATA_POLICY
// M5: the policy table itself, byte by byte.
static void testPolicyTable() {
  for (int b = 1; b < 256; ++b) {
    char c = static_cast<char>(b);
    bool expected = !hostileByte(c);
    CHECK(metadataTextByteAllowed(c) == expected, "M5: byte 0x%02X allowed=%d, expected %d", b,
          (int)metadataTextByteAllowed(c), (int)expected);
  }
  CHECK(metadataTextAllowed(nullptr) && metadataTextAllowed(""), "M5: empty text must be allowed");
  CHECK(scrubMetadataText(nullptr) == 0, "M5: scrub(nullptr) must be a no-op");
  char text[] = "a<b\x01" "c";
  CHECK(scrubMetadataText(text) == 2 && std::strcmp(text, "a_b_c") == 0, "M5: scrub result '%s'", text);
}
#endif

int main() {
  testWireScrub();
  testCleanPayloadUnchanged();
  testSetterRejects();
  testSetterPreserved();
#if HW1_HAVE_METADATA_POLICY
  testPolicyTable();
#endif
  if (gFailures) {
    std::printf("%d metadata text check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("espnow metadata text: all checks passed\n");
  return 0;
}
