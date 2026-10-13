// Host harness for test_oled_mode_availability.py: launcher availability must
// apply each mode module's own entry rule (OLEDModeEntry::availFunc, found
// through the production registry) on top of the central build/hardware/
// feature classification.
//
// The suite pastes the production mode enum, callback typedefs, OLEDModeEntry
// and MenuAvailability (OLED_Display.h), the registry and getMenuAvailability
// (OLED_Utils.cpp) and the real availFuncs of CLI Input, Gamepad PW, Logout,
// GPS and thermal into the markers below.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

// GPS and thermal compiled in; every other optional feature off.
#define ENABLE_GPS_SENSOR 1
#define ENABLE_THERMAL_SENSOR 1
#define ENABLE_NEOPIXEL 0
#define ENABLE_AUTOMATION 0
#define ENABLE_ESPNOW 0
#define ENABLE_BONDED_MODE 0
#define ENABLE_FM_RADIO 0
#define ENABLE_IMU_SENSOR 0
#define ENABLE_TOF_SENSOR 0
#define ENABLE_APDS_SENSOR 0
#define ENABLE_GAMEPAD_SENSOR 0
#define ENABLE_RTC_SENSOR 0
#define ENABLE_PRESENCE_SENSOR 0
#define ENABLE_BLUETOOTH 0
#define ENABLE_WIFI 0
#define EXT_RAM_BSS_ATTR
#define DEBUG_SYSTEMF(...) do { } while (0)
#define I2C_ADDR_GPS 0x10
#define I2C_ADDR_THERMAL 0x33

class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  String& operator=(const char* text) {
    text_ = text ? text : "";
    return *this;
  }
  bool operator==(const char* other) const { return text_ == (other ? other : ""); }
  const char* c_str() const { return text_.c_str(); }
  size_t length() const { return text_.length(); }

 private:
  std::string text_;
};

// INSERT_TYPES

// ---------------------------------------------------------------------------
// Fakes for what the central classification and the module rules read
// ---------------------------------------------------------------------------
struct ConnectedDevice {
  uint8_t address;
  bool isConnected;
};
ConnectedDevice connectedDevices[4];
int connectedDeviceCount = 0;

bool gGpsConnected = false;
bool gGpsRunning = false;
bool gThermalConnected = false;
void* server = nullptr;

// Users row: base reads the display mirror and the roster, the worktree the
// cached session role. Neither is under test here.
bool gLocalDisplayAuthed = false;
String gLocalDisplayUser;
bool isAdminUser(const String&) { return false; }
bool oledSessionIsAdmin() { return false; }

enum CommandSource { SOURCE_LOCAL_DISPLAY = 1 };
bool gNamedDisplaySession = false;  // a user is logged in on the display
bool isTransportAuthenticated(CommandSource source) {
  return source == SOURCE_LOCAL_DISPLAY && gNamedDisplaySession;
}

struct Settings {
  bool bondModeEnabled = false;
  String bondPeerMac;
};
Settings gSettings;

// INSERT_REGISTRY

// INSERT_RULES

// Harness-local rules: one that allows entry but leaves a note, one that
// refuses without a reason.
static bool noteRule(String* outReason) {
  if (outReason) *outReason = "note";
  return true;
}
static bool silentRefusal(String*) { return false; }

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------
static int gChecks = 0;
static int gFailures = 0;

static void check(bool condition, const std::string& message) {
  ++gChecks;
  if (!condition) {
    ++gFailures;
    std::printf("FAIL: %s\n", message.c_str());
  }
}

static const char* className(MenuAvailability availability) {
  switch (availability) {
    case MenuAvailability::AVAILABLE: return "AVAILABLE";
    case MenuAvailability::FEATURE_DISABLED: return "FEATURE_DISABLED";
    case MenuAvailability::UNINITIALIZED: return "UNINITIALIZED";
    case MenuAvailability::NOT_BUILT: return "NOT_BUILT";
    case MenuAvailability::NOT_DETECTED: return "NOT_DETECTED";
  }
  return "?";
}

static std::string printable(const String& text) {
  std::string out;
  for (const char* p = text.c_str(); *p; ++p) {
    if (*p == '\n') {
      out += "\\n";
    } else {
      out += *p;
    }
  }
  return out;
}

static void expect(OLEDMode mode, const char* row, MenuAvailability wanted,
                   const char* wantedReason, const std::string& why) {
  String reason = "stale";
  const MenuAvailability got = getMenuAvailability(mode, &reason);
  check(got == wanted && reason == wantedReason,
        std::string(row) + ": " + why + ": expected " + className(wanted) + " \"" +
            printable(String(wantedReason)) + "\", got " + className(got) + " \"" +
            printable(reason) + "\"");
}

static OLEDModeEntry gEntries[8];
static size_t gEntryCount = 0;

static void addEntry(OLEDMode mode, const char* name, OLEDAvailabilityFunc rule) {
  OLEDModeEntry entry{};
  entry.mode = mode;
  entry.name = name;
  entry.availFunc = rule;
  gEntries[gEntryCount++] = entry;
}

static void registerModules() {
  addEntry(OLED_CLI_INPUT, "CLI Input", isCLIInputAvailable);
  addEntry(OLED_SET_PATTERN, "Gamepad Password", isSetPatternAvailable);
  addEntry(OLED_LOGOUT, "Logout", isLogoutModeAvailable);
  addEntry(OLED_GPS_DATA, "GPS", gpsOLEDModeAvailable);
  addEntry(OLED_THERMAL_VISUAL, "Thermal", thermalOLEDModeAvailable);
  addEntry(OLED_LOGGING, "Logging", noteRule);
  addEntry(OLED_PERF_STATS, "Perf", silentRefusal);
  registerOLEDModes(gEntries, gEntryCount);
}

static void detect(uint8_t address) {
  connectedDevices[connectedDeviceCount].address = address;
  connectedDevices[connectedDeviceCount].isConnected = true;
  ++connectedDeviceCount;
}

static void resetWorld() {
  gNamedDisplaySession = false;
  gSettings = Settings();
  connectedDeviceCount = 0;
  gGpsConnected = gGpsRunning = false;
  gThermalConnected = false;
}

// R1: the login rules of CLI Input, Gamepad PW and Logout apply.
static void caseLoginRules() {
  resetWorld();
  expect(OLED_CLI_INPUT, "CLI Input", MenuAvailability::FEATURE_DISABLED, "Login required",
         "no named display session (display auth off): the mode's rule refuses entry");
  expect(OLED_SET_PATTERN, "Gamepad PW", MenuAvailability::FEATURE_DISABLED, "Login required",
         "no named display session: the mode's rule refuses entry");
  expect(OLED_LOGOUT, "Logout", MenuAvailability::FEATURE_DISABLED, "Not logged in",
         "no named display session: the mode's rule refuses entry");
  check(getMenuAvailability(OLED_CLI_INPUT, nullptr) == MenuAvailability::FEATURE_DISABLED,
        "CLI Input: a null outReason still gets the refusal");

  gNamedDisplaySession = true;
  expect(OLED_CLI_INPUT, "CLI Input", MenuAvailability::AVAILABLE, "",
         "named display session: available");
  expect(OLED_SET_PATTERN, "Gamepad PW", MenuAvailability::AVAILABLE, "",
         "named display session: available");
  expect(OLED_LOGOUT, "Logout", MenuAvailability::AVAILABLE, "",
         "named display session: available");
}

// R2: the central classification keeps its finer class and reason.
static void caseSensorClassification() {
  resetWorld();
  expect(OLED_GPS_DATA, "GPS", MenuAvailability::NOT_DETECTED, "Not detected",
         "GPS not detected (both rules refuse): the launcher keeps the No-HW class");
  detect(I2C_ADDR_GPS);
  expect(OLED_GPS_DATA, "GPS", MenuAvailability::FEATURE_DISABLED, "Disabled\nPress X to start",
         "GPS detected but stopped: stays Off with the X:Start reason");
  gGpsConnected = gGpsRunning = true;
  expect(OLED_GPS_DATA, "GPS", MenuAvailability::AVAILABLE, "", "GPS running: available");

  resetWorld();
  expect(OLED_THERMAL_VISUAL, "Thermal", MenuAvailability::NOT_DETECTED, "Not detected",
         "thermal not detected (the module allows navigation): still No HW");
  detect(I2C_ADDR_THERMAL);
  expect(OLED_THERMAL_VISUAL, "Thermal", MenuAvailability::FEATURE_DISABLED,
         "Disabled\nPress X to start", "thermal detected but stopped: still Off");
  gThermalConnected = true;
  expect(OLED_THERMAL_VISUAL, "Thermal", MenuAvailability::AVAILABLE, "",
         "thermal connected: available");
}

// R3: modes without a registered rule, and rules that allow, are unchanged.
static void caseUnchanged() {
  resetWorld();
  expect(OLED_BLUETOOTH, "Bluetooth", MenuAvailability::NOT_BUILT, "Not built",
         "no registered entry, not built: central class");
  expect(OLED_SYSTEM_STATUS, "Status", MenuAvailability::AVAILABLE, "",
         "no registered entry: available");
  expect(OLED_LOGGING, "Logging", MenuAvailability::AVAILABLE, "",
         "a rule that allows entry: its note does not reach the launcher");
}

// R4: a refusal without a reason is still a refusal (the launcher fills "Disabled").
static void caseSilentRefusal() {
  resetWorld();
  expect(OLED_PERF_STATS, "Perf", MenuAvailability::FEATURE_DISABLED, "",
         "a rule that refuses without a reason: refused");
}

int main() {
  registerModules();
  caseLoginRules();
  caseSensorClassification();
  caseUnchanged();
  caseSilentRefusal();
  std::printf("oled_mode_availability: %d checks, %d failed\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
