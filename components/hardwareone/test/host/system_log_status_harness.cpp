// Executes the shipping system-log status reading, its text and json forms, the
// flags formatters and the persisted flag restore from System_Debug.cpp over
// host globals. Only millis() and the globals the formatters read are
// host-provided.
#include "Arduino.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int checks = 0;
#define CHECK(condition) do { \
  if (!(condition)) { std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); std::abort(); } \
  ++checks; } while (0)

// INSERT_TYPES

static unsigned long nowMs = 0;
static unsigned long millis() { return nowMs; }
volatile uint32_t gOutputFlags = 0;
String gSystemLogPath = "";
// B11: the file the lines go to (the card twin while a card takes them); empty until a file was opened.
static String gSystemLogActivePath = "";
bool gSystemLogRunning = false;
unsigned long gSystemLogLastWrite = 0;
uint32_t gSystemLogRotations = 0;  // production: static in System_Debug.cpp, shown by the ACTIVE status
uint32_t gSystemLogReserveSkips = 0;  // production: static, counted by the writer (G-39)
// The writer's reserve check reads the VFS free-space cache and floor.
static size_t littleFsFree = 1024 * 1024;
namespace VFS {
enum StorageType { INTERNAL = 0, SDCARD = 1 };
static StorageType getStorageType(const String& path) { return std::strncmp(path.c_str(), "/sd/", 4) == 0 ? SDCARD : INTERNAL; }
static size_t getCachedLittleFsFree() { return littleFsFree; }
static size_t logReserveBytes() { return 100 * 1024; }
}
DebugFlagMask gDebugFlags;
struct HostSettings { bool systemLogAutoStart = false; String systemLogFlags;
                      int systemLogMaxSize = 1048576; int systemLogMaxRotations = 3; } gSettings;

bool parseSystemLogFlags(const String& flagsStr, DebugFlagMask& out);
void formatSystemLogFlags(DebugFlagMask mask, char* out, size_t outSize);
String formatSystemLogFlags(DebugFlagMask mask);
bool systemLogApplyPersistedFlags();
bool systemLogFollowFlagChange(DebugFlagMask touched);
void formatSystemLogStatus(char* out, size_t outSize);
bool formatSystemLogStatusJson(char* out, size_t outSize);
// Production setSetting also requests a save; the harness counts the writes.
static int settingWrites = 0;
static void setSetting(String& field, const String& value) {
  if (field != value) { field = value; ++settingWrites; }
}
// The flag hooks after a whole-mask change (System_Debug.cpp
// applyDebugMaskLogLevels; test_radio_stack_logs pins its body): the restore
// must call it with the mask it replaced, and only when it replaced it.
static int maskApplies = 0;
static DebugFlagMask maskBefore;
static DebugFlagMask maskSeen;
void applyDebugMaskLogLevels(DebugFlagMask before) {
  ++maskApplies;
  maskBefore = before;
  maskSeen = gDebugFlags;
}
// INSERT_PRODUCTION

static constexpr size_t kCliStatus = HW1_CLI_STATUS_BYTES;
static constexpr size_t kWebStatus = HW1_WEB_STATUS_BYTES;  // the web endpoint's json buffer
static constexpr size_t kFlagsText = HW1_FLAGS_TEXT_BYTES;
// A caller with a small buffer gets the text cut at its size (no caller is this
// small today; the web endpoint sends the json form).
static constexpr size_t kSmallText = 512;

static std::string status(size_t size) {
  std::string buffer(size + 8, '\x7e');
  formatSystemLogStatus(&buffer[0], size);
  CHECK(buffer.compare(size, 8, std::string(8, '\x7e')) == 0);  // never writes past outSize
  CHECK(std::memchr(buffer.data(), '\0', size) != nullptr);
  return std::string(buffer.c_str());
}

// The json form: never writes past outSize, always NUL-terminated, and the
// return value says whether the document is the status (true) or the error
// object (false).
static std::string statusJson(size_t size, bool expectFits = true) {
  std::string buffer(size + 8, '\x7e');
  const bool fits = formatSystemLogStatusJson(&buffer[0], size);
  CHECK(fits == expectFits);
  CHECK(buffer.compare(size, 8, std::string(8, '\x7e')) == 0);
  CHECK(std::memchr(buffer.data(), '\0', size) != nullptr);
  return std::string(buffer.c_str());
}
static const std::string kTooLarge = "{\"schema\":1,\"ok\":false,\"error\":\"response too large\"}";

static std::string hex64(uint64_t value) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(value));
  return text;
}

int main() {
  // Three states, identical in the CLI and web buffers when they fit.
  gSystemLogRunning = false;
  gSettings.systemLogAutoStart = false;
  for (size_t size : {kCliStatus, kSmallText})
    CHECK(status(size) == "System logging is INACTIVE\n  Auto-start: OFF");
  // The json form of the same reading, identical in the CLI and web buffers.
  CHECK(systemLogState() == SystemLogState::Inactive);
  const std::string limits = ",\"maxSizeKb\":1024,\"keep\":3,\"rotations\":0,\"reserveSkips\":0,\"reserveKb\":100}";
  for (size_t size : {kCliStatus, kWebStatus})
    CHECK(statusJson(size) == "{\"schema\":1,\"state\":\"inactive\",\"running\":false,\"autoStart\":false" + limits);
  gSettings.systemLogAutoStart = true;
  CHECK(status(kSmallText) == "System logging is INACTIVE\n  Auto-start: ON");
  CHECK(statusJson(kWebStatus) == "{\"schema\":1,\"state\":\"inactive\",\"running\":false,\"autoStart\":true" + limits);
  // A stale route flag without the running flag is still inactive.
  gOutputFlags = MSG_ROUTE_FILE;
  CHECK(systemLogState() == SystemLogState::Inactive);
  CHECK(status(kCliStatus) == "System logging is INACTIVE\n  Auto-start: ON");
  gOutputFlags = 0;

  gSystemLogRunning = true;
  gSystemLogPath = "/logging_captures/system/debug.log";
  gOutputFlags = MSG_ROUTE_SERIAL | MSG_ROUTE_FILE;
  gSystemLogLastWrite = 1000;
  nowMs = 63500;
  char routes[8];
  std::snprintf(routes, sizeof(routes), "0x%02X", static_cast<unsigned>(MSG_ROUTE_SERIAL | MSG_ROUTE_FILE));
  const std::string cap = "\n  Rotation: at 1024 KB, keep 3 old file(s); rotated 0 time(s) this boot";
  const std::string active = std::string("System logging ACTIVE\n  File: /logging_captures/system/debug.log\n"
                                         "  Last write: 62s ago\n  Output flags: ") + routes + "\n  Auto-start: ON" + cap;
  for (size_t size : {kCliStatus, kSmallText}) CHECK(status(size) == active);
  CHECK(systemLogState() == SystemLogState::Active);
  const std::string activeJson =
      "{\"schema\":1,\"state\":\"active\",\"running\":true,\"file\":\"/logging_captures/system/debug.log\","
      "\"lastWriteAgoSec\":62,\"outputFlags\":" +
      std::to_string(static_cast<unsigned>(MSG_ROUTE_SERIAL | MSG_ROUTE_FILE)) + ",\"autoStart\":true";
  for (size_t size : {kCliStatus, kWebStatus}) CHECK(statusJson(size) == activeJson + limits);
  // B11: while a card takes log lines the file is the card twin of the
  // session's path, and that is the file the status names; a session whose
  // file is on flash shows the path as before.
  gSystemLogActivePath = "/sd/logging_captures/system/debug.log";
  CHECK(status(kCliStatus).find("  File: /sd/logging_captures/system/debug.log\n") != std::string::npos);
  CHECK(statusJson(kWebStatus).find("\"file\":\"/sd/logging_captures/system/debug.log\",") != std::string::npos);
  CHECK(!systemLogBlockedByReserve(gSystemLogActivePath));   // the card is not held to the flash reserve
  gSystemLogActivePath = "";
  CHECK(status(kCliStatus) == active);
  gSystemLogRotations = 3;  // the writer's rotation count reaches the status line
  CHECK(status(kCliStatus).find("rotated 3 time(s) this boot") != std::string::npos);
  CHECK(statusJson(kWebStatus).find(",\"rotations\":3,") != std::string::npos);
  gSystemLogRotations = 0;
  gSettings.systemLogMaxSize = 20480; gSettings.systemLogMaxRotations = 0;  // settings drive the line
  CHECK(status(kCliStatus).find("Rotation: at 20 KB, keep 0 old file(s)") != std::string::npos);
  CHECK(statusJson(kWebStatus).find(",\"maxSizeKb\":20,\"keep\":0,") != std::string::npos);
  gSettings.systemLogMaxSize = 99; gSettings.systemLogMaxRotations = 42;    // out of range reads as default
  CHECK(status(kCliStatus).find("Rotation: at 1024 KB, keep 3 old file(s)") != std::string::npos);
  gSettings.systemLogMaxSize = 1048576; gSettings.systemLogMaxRotations = 3;

  // G-39: on LittleFS the writer skips lines below the reserve the overflow
  // latch keeps for settings saves; a log on SD is not held to it. Skips
  // reach the status line only once there are any.
  littleFsFree = 100 * 1024;
  CHECK(!systemLogBlockedByReserve(gSystemLogPath));
  littleFsFree = 100 * 1024 - 1;
  CHECK(systemLogBlockedByReserve(gSystemLogPath));
  CHECK(!systemLogBlockedByReserve(String("/sd/logging_captures/system/debug.log")));
  littleFsFree = 1024 * 1024;
  gSystemLogReserveSkips = 7;
  CHECK(status(kCliStatus) == active + "\n  Skipped: 7 line(s) while flash was below its 100 KB reserve");
  CHECK(statusJson(kWebStatus).find(",\"reserveSkips\":7,\"reserveKb\":100}") != std::string::npos);
  gSystemLogReserveSkips = 0;

  gOutputFlags = MSG_ROUTE_SERIAL;
  gSettings.systemLogAutoStart = false;
  const std::string configured = "System logging CONFIGURED but MSG_ROUTE_FILE flag not set\n"
                                 "  File: /logging_captures/system/debug.log\n"
                                 "  Use 'log start' to enable\n  Auto-start: OFF";
  for (size_t size : {kCliStatus, kSmallText}) CHECK(status(size) == configured);
  // The third state: the running flag is set, nothing is routed to the file.
  // The json carries both, so a reader's Start/Stop can follow `running`
  // (`log start` refuses here, `log stop` clears it) while its label follows `state`.
  CHECK(systemLogState() == SystemLogState::Configured);
  for (size_t size : {kCliStatus, kWebStatus})
    CHECK(statusJson(size) == "{\"schema\":1,\"state\":\"configured\",\"running\":true,"
                              "\"file\":\"/logging_captures/system/debug.log\",\"autoStart\":false" + limits);

  // The state's json value and its small-screen label (OLED: "Status: " + label in 21 columns).
  CHECK(std::string(systemLogStateKey(SystemLogState::Active)) == "active");
  CHECK(std::string(systemLogStateKey(SystemLogState::Configured)) == "configured");
  CHECK(std::string(systemLogStateKey(SystemLogState::Inactive)) == "inactive");
  CHECK(std::string(systemLogStateLabel(SystemLogState::Active)) == "ACTIVE");
  CHECK(std::string(systemLogStateLabel(SystemLogState::Configured)) == "NOT ROUTED");
  CHECK(std::string(systemLogStateLabel(SystemLogState::Inactive)) == "STOPPED");
  for (SystemLogState state : {SystemLogState::Active, SystemLogState::Configured, SystemLogState::Inactive})
    CHECK(std::strlen("Status: ") + std::strlen(systemLogStateLabel(state)) <= 21);

  // A path with characters JSON must escape stays one valid string value.
  gOutputFlags = MSG_ROUTE_FILE;
  gSystemLogPath = "/logs/a\"b\\c\n.log";
  CHECK(statusJson(kWebStatus).find(",\"file\":\"/logs/a\\\"b\\\\c\\n.log\",") != std::string::npos);

  // A long path: the text is cut at the caller's size, the CLI keeps all of it.
  gSystemLogPath = String(std::string("/logging_captures/system/") + std::string(600, 'p') + ".log");
  const std::string full = status(kCliStatus);
  CHECK(full.size() > kSmallText && full.size() < kCliStatus);
  const std::string small = status(kSmallText);
  CHECK(small.size() == kSmallText - 1 && full.compare(0, small.size(), small) == 0);
  // The json is never cut: it fits whole (and still ends the object), or the
  // reply is the error object.
  const std::string longJson = statusJson(kWebStatus);
  CHECK(longJson.size() > 700 && longJson.back() == '}' &&
        longJson.find(std::string(600, 'p') + ".log\"") != std::string::npos);
  CHECK(statusJson(kSmallText, false) == kTooLarge);
  gSystemLogPath = String(std::string("/") + std::string(1100, 'p'));
  for (size_t size : {kCliStatus, kWebStatus}) CHECK(statusJson(size, false) == kTooLarge);
  // Exactly at the edge: the largest path that fits, then one byte more.
  for (size_t length = 800; length < 1100; ++length) {
    gSystemLogPath = String(std::string("/") + std::string(length, 'p'));
    std::string probe(kWebStatus, '\0');
    if (formatSystemLogStatusJson(&probe[0], kWebStatus)) {
      CHECK(std::strlen(probe.c_str()) < kWebStatus && probe[std::strlen(probe.c_str()) - 1] == '}');
    } else {
      CHECK(std::string(probe.c_str()) == kTooLarge);
      gSystemLogPath = String(std::string("/") + std::string(length - 1, 'p'));
      const std::string last = statusJson(kWebStatus);
      CHECK(last.size() == kWebStatus - 1 && last.back() == '}');
      break;
    }
  }
  CHECK(std::string(gSystemLogPath.c_str()).size() < 1100);  // the edge was found
  gSystemLogPath = String(std::string("/logging_captures/system/") + std::string(600, 'p') + ".log");

  // Null and zero-size outputs write nothing.
  formatSystemLogStatus(nullptr, kCliStatus);
  CHECK(!formatSystemLogStatusJson(nullptr, kCliStatus));
  formatSystemLogFlags(DebugFlagMask(1), nullptr, kFlagsText);
  char canary[4] = {'x', 'y', 'z', '\0'};
  formatSystemLogStatus(canary, 0);
  CHECK(!formatSystemLogStatusJson(canary, 0));
  formatSystemLogFlags(DebugFlagMask(1), canary, 0);
  CHECK(std::strcmp(canary, "xyz") == 0);

  // Mask text is 69 characters: callers need at least 70 bytes (they use 80).
  const std::string text15 = "0x0000000000000000:0000000000000000:0000000000000000:0000000000000015";
  CHECK(text15.size() == 69 && kFlagsText >= text15.size() + 1);
  char flags[80];
  formatSystemLogFlags(DebugFlagMask(0x15), flags, 70);
  CHECK(flags == text15);
  formatSystemLogFlags(DebugFlagMask(0x15), flags, 69);
  CHECK(flags == text15.substr(0, 68));
  CHECK(std::string(formatSystemLogFlags(DebugFlagMask(0x15)).c_str()) == text15);

  // Every single-bit mask survives format -> parse in both accepted forms.
  for (int bit = 0; bit < 256; ++bit) {
    const DebugFlagMask mask = DEBUG_BIT(bit);
    formatSystemLogFlags(mask, flags, kFlagsText);
    DebugFlagMask parsed(0xDEAD);
    CHECK(parseSystemLogFlags(String(flags), parsed) && parsed == mask);
    const std::string plain = hex64(mask.w3) + hex64(mask.w2) + hex64(mask.w1) + hex64(mask.w0);
    parsed = DebugFlagMask(0xDEAD);
    CHECK(parseSystemLogFlags(String(plain), parsed) && parsed == mask);
  }
  DebugFlagMask parsed;
  CHECK(parseSystemLogFlags(String("0X15"), parsed) && parsed == DebugFlagMask(0x15));
  CHECK(parseSystemLogFlags(String("15"), parsed) && parsed == DebugFlagMask(0x15));
  CHECK(parseSystemLogFlags(String("0x1:0x2"), parsed) && parsed == DebugFlagMask(2, 1, 0, 0));
  CHECK(!parseSystemLogFlags(String(""), parsed) && !parseSystemLogFlags(String("0x"), parsed));
  CHECK(parseSystemLogFlags(String("1:2:3:4"), parsed) && parsed == DebugFlagMask(4, 3, 2, 1));
  CHECK(parseSystemLogFlags(String(" 0x15 "), parsed) && parsed == DebugFlagMask(0x15));
  // Text that is not a hex mask is refused and leaves the output alone: a
  // character that is not a hex digit, an empty word, a fifth word, a colon
  // word over 16 digits, a plain mask over 64 digits.
  for (const char* junk : {"zz", "15zz", "0x15:", ":", ":15", "1::2", "1:2:3:4:5", "0x0x15", "-1", "1 5",
                           "11111111111111111:0",
                           "11111111111111111111111111111111111111111111111111111111111111111"}) {
    parsed = DebugFlagMask(0xDEAD);
    CHECK(!parseSystemLogFlags(String(junk), parsed) && parsed == DebugFlagMask(0xDEAD));
  }

  // Persisted restore: empty or prefix-only text leaves the live flags alone.
  const DebugFlagMask live(0x77, 0, 0, 1);
  for (const char* rejected : {"", "0x"}) {
    gDebugFlags = live;
    gSettings.systemLogFlags = rejected;
    CHECK(!systemLogApplyPersistedFlags() && gDebugFlags == live);
  }
  CHECK(maskApplies == 0);
  gDebugFlags = live;
  gSettings.systemLogFlags = "0x0000000000000000:0000000000000000:0000000000000001:0000000000000000";
  CHECK(systemLogApplyPersistedFlags() && gDebugFlags == DEBUG_BIT(64));
  // The flag hooks ran once, after the new mask was live, told the old one.
  CHECK(maskApplies == 1 && maskBefore == live && maskSeen == DEBUG_BIT(64));
  // Saved text that is not a hex mask is refused too; it does not clear the
  // live flags.
  gDebugFlags = live;
  gSettings.systemLogFlags = "zz";
  CHECK(!systemLogApplyPersistedFlags() && gDebugFlags == live && maskApplies == 1);

  // A persistent per-flag change follows into the saved mask, so the next
  // boot's autostart does not bring a cleared category back.
  const DebugFlagMask ring = DEBUG_BIT(224), http = DEBUG_BIT(3), parent = DEBUG_BIT(64);
  gSettings.systemLogFlags = formatSystemLogFlags(ring | http | parent);
  settingWrites = 0;
  gDebugFlags = http | parent;  // `debugring 0` cleared the live bit
  CHECK(systemLogFollowFlagChange(ring) && settingWrites == 1);
  gDebugFlags = DebugFlagMask(0);  // reboot: the per-flag settings load, then autostart applies the saved mask
  CHECK(systemLogApplyPersistedFlags() && gDebugFlags == (http | parent));
  // Turning a category on reaches the saved mask too; other saved bits stay.
  gDebugFlags = ring;
  CHECK(systemLogFollowFlagChange(ring) && settingWrites == 2);
  CHECK(systemLogApplyPersistedFlags() && gDebugFlags == (ring | http | parent));
  // Only the touched bits are copied: live bits outside them never leak in,
  // and a saved bit outside them is not dropped.
  gDebugFlags = DEBUG_BIT(10);
  CHECK(!systemLogFollowFlagChange(DEBUG_BIT(11)) && settingWrites == 2);
  // A recomputed family parent travels with its member.
  gDebugFlags = ring | http;
  CHECK(systemLogFollowFlagChange(DEBUG_BIT(65) | parent) && settingWrites == 3);
  CHECK(systemLogApplyPersistedFlags() && gDebugFlags == (ring | http));
  // Already in agreement: no write.
  CHECK(!systemLogFollowFlagChange(ring | http | parent) && settingWrites == 3);
  // Nothing saved, or text that does not parse: the per-flag settings stand
  // alone and nothing is written.
  for (const char* none : {"", "0x"}) {
    gSettings.systemLogFlags = none;
    gDebugFlags = ring;
    CHECK(!systemLogFollowFlagChange(ring) && settingWrites == 3);
    CHECK(std::string(gSettings.systemLogFlags.c_str()) == none);
  }

  std::fprintf(stderr, "checks=%d\n", checks);
  std::puts("SYSTEM_LOG_STATUS PASS");
  return 0;
}
