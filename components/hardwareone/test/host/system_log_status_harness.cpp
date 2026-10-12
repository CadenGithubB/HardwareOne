// Executes the shipping system-log status/flags formatters and the persisted
// flag restore from System_Debug.cpp over host globals. Only millis() and the
// globals the formatters read are host-provided.
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
bool gSystemLogRunning = false;
unsigned long gSystemLogLastWrite = 0;
DebugFlagMask gDebugFlags;
struct HostSettings { bool systemLogAutoStart = false; String systemLogFlags; } gSettings;

bool parseSystemLogFlags(const String& flagsStr, DebugFlagMask& out);
void formatSystemLogFlags(DebugFlagMask mask, char* out, size_t outSize);
String formatSystemLogFlags(DebugFlagMask mask);
bool systemLogApplyPersistedFlags();
void formatSystemLogStatus(char* out, size_t outSize);
// INSERT_PRODUCTION

static constexpr size_t kCliStatus = HW1_CLI_STATUS_BYTES;
static constexpr size_t kWebStatus = HW1_WEB_STATUS_BYTES;
static constexpr size_t kFlagsText = HW1_FLAGS_TEXT_BYTES;

static std::string status(size_t size) {
  std::string buffer(size + 8, '\x7e');
  formatSystemLogStatus(&buffer[0], size);
  CHECK(buffer.compare(size, 8, std::string(8, '\x7e')) == 0);  // never writes past outSize
  CHECK(std::memchr(buffer.data(), '\0', size) != nullptr);
  return std::string(buffer.c_str());
}

static std::string hex64(uint64_t value) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(value));
  return text;
}

int main() {
  // Three states, identical in the CLI and web buffers when they fit.
  gSystemLogRunning = false;
  gSettings.systemLogAutoStart = false;
  for (size_t size : {kCliStatus, kWebStatus})
    CHECK(status(size) == "System logging is INACTIVE\n  Auto-start: OFF");
  gSettings.systemLogAutoStart = true;
  CHECK(status(kWebStatus) == "System logging is INACTIVE\n  Auto-start: ON");

  gSystemLogRunning = true;
  gSystemLogPath = "/logging_captures/system/debug.log";
  gOutputFlags = MSG_ROUTE_SERIAL | MSG_ROUTE_FILE;
  gSystemLogLastWrite = 1000;
  nowMs = 63500;
  char routes[8];
  std::snprintf(routes, sizeof(routes), "0x%02X", static_cast<unsigned>(MSG_ROUTE_SERIAL | MSG_ROUTE_FILE));
  const std::string active = std::string("System logging ACTIVE\n  File: /logging_captures/system/debug.log\n"
                                         "  Last write: 62s ago\n  Output flags: ") + routes + "\n  Auto-start: ON";
  for (size_t size : {kCliStatus, kWebStatus}) CHECK(status(size) == active);

  gOutputFlags = MSG_ROUTE_SERIAL;
  gSettings.systemLogAutoStart = false;
  const std::string configured = "System logging CONFIGURED but MSG_ROUTE_FILE flag not set\n"
                                 "  File: /logging_captures/system/debug.log\n"
                                 "  Use 'log start' to enable\n  Auto-start: OFF";
  for (size_t size : {kCliStatus, kWebStatus}) CHECK(status(size) == configured);

  // A long path fills the web buffer to 511 bytes plus NUL; the CLI keeps all of it.
  gOutputFlags = MSG_ROUTE_FILE;
  gSystemLogPath = String(std::string("/logging_captures/system/") + std::string(600, 'p') + ".log");
  const std::string full = status(kCliStatus);
  CHECK(full.size() > kWebStatus && full.size() < kCliStatus);
  const std::string web = status(kWebStatus);
  CHECK(web.size() == kWebStatus - 1 && full.compare(0, web.size(), web) == 0);

  // Null and zero-size outputs write nothing.
  formatSystemLogStatus(nullptr, kCliStatus);
  formatSystemLogFlags(DebugFlagMask(1), nullptr, kFlagsText);
  char canary[4] = {'x', 'y', 'z', '\0'};
  formatSystemLogStatus(canary, 0);
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

  // Persisted restore: empty or prefix-only text leaves the live flags alone.
  const DebugFlagMask live(0x77, 0, 0, 1);
  for (const char* rejected : {"", "0x"}) {
    gDebugFlags = live;
    gSettings.systemLogFlags = rejected;
    CHECK(!systemLogApplyPersistedFlags() && gDebugFlags == live);
  }
  gSettings.systemLogFlags = "0x0000000000000000:0000000000000000:0000000000000001:0000000000000000";
  CHECK(systemLogApplyPersistedFlags() && gDebugFlags == DEBUG_BIT(64));
  // Current behavior, pinned so a change is deliberate: non-hex text parses as
  // zero (strtoull) and clears the live flags.
  gDebugFlags = live;
  gSettings.systemLogFlags = "zz";
  CHECK(systemLogApplyPersistedFlags() && gDebugFlags == DebugFlagMask(0));

  std::fprintf(stderr, "checks=%d\n", checks);
  std::puts("SYSTEM_LOG_STATUS PASS");
  return 0;
}
