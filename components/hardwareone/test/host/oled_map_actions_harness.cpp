// OLED Map mode actions. test_oled_map_actions.py inserts the shipping Map
// state block, track scan, menu dispatch, input handler and the head of
// displayGPSMap (through the map-centering step) at the markers below.
// Everything defined here is a boundary double. The identity double mirrors
// the firmware's per-task slot: CommandIdentityScope installs a context and
// currentAuthContext() returns it, or the empty anonymous context when none is
// installed. The Map owner and VFS doubles record the identity at each call and
// refuse the anonymous one, as checkPerm does for FsRole::ANON.
// Input: HAL_Input's real button mapping tables and INPUT_CHECK are inserted
// for the device chosen with -DMAP_TEST_DEVICE, and frame() hands the handler
// what processOLEDInput hands a mode for that device.
#include <algorithm>
using std::max;
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>

#include "map_actions_test_string.h"

#define ENABLE_OLED_INPUT 0
#define ENABLE_ESPNOW 0
#define EXT_RAM_BSS_ATTR
#define DEBUG_MAPSF(...) do {} while (0)
#define INFO_MAPSF(...) do {} while (0)
#define DEBUG_MAPS_RENDERINGF(...) do {} while (0)

// Input device under test (-DMAP_TEST_DEVICE): the seesaw gamepad, the ANO
// rotary encoder, or the GPIO wheel with the P4X-EYE's three keys. These pick
// HAL_Input's mapping table as the build's INPUT_DEVICE_TYPE does.
#define MAP_TEST_SEESAW 0
#define MAP_TEST_ANO 1
#define MAP_TEST_GPIO 2
#ifndef MAP_TEST_DEVICE
#define MAP_TEST_DEVICE MAP_TEST_SEESAW
#endif
#define ENABLE_GAMEPAD_SENSOR (MAP_TEST_DEVICE == MAP_TEST_SEESAW)
#define ENABLE_ANO_ENCODER (MAP_TEST_DEVICE == MAP_TEST_ANO)
#define ENABLE_GPIO_ENCODER_BUTTONS (MAP_TEST_DEVICE == MAP_TEST_GPIO)
#define ERROR_INPUTF(...) do {} while (0)

static int failures;
static unsigned checks;
#define EXPECT(condition, what) do { ++checks; if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL: %s\n      %s:%d: %s\n", what, __FILE__, __LINE__, #condition); } } while (0)

// INSERT_TYPES

// ---------------------------------------------------------------------------
// Input boundary: HAL_Input's controller for this device, its mapping tables,
// inputGetButtonMask/inputIsButtonPressed and INPUT_CHECK (inserted), and the
// router's per-frame nav events.
// ---------------------------------------------------------------------------
static InputControllerType gCurrentControllerType =
#if MAP_TEST_DEVICE == MAP_TEST_ANO
    INPUT_CONTROLLER_ANO_ENCODER;
#elif MAP_TEST_DEVICE == MAP_TEST_GPIO
    INPUT_CONTROLLER_GPIO_ENCODER;
#else
    INPUT_CONTROLLER_GAMEPAD_SEESAW;
#endif

// INSERT_INPUT

NavEvents gNavEvents{};

// ---------------------------------------------------------------------------
// Identity boundary
// ---------------------------------------------------------------------------
static const AuthContext kAnonymous{};
static std::vector<AuthContext> gIdentityStack;
static const AuthContext& currentAuthContext() {
  return gIdentityStack.empty() ? kAnonymous : gIdentityStack.back();
}
class CommandIdentityScope {
 public:
  explicit CommandIdentityScope(const AuthContext& ctx) { gIdentityStack.push_back(ctx); }
  ~CommandIdentityScope() { gIdentityStack.pop_back(); }
  CommandIdentityScope(const CommandIdentityScope&) = delete;
  CommandIdentityScope& operator=(const CommandIdentityScope&) = delete;
};
static const char* const kOledUser = "oled-operator";
static AuthContext oledAuthContext(const char* path) {
  AuthContext ctx{};
  ctx.transport = SOURCE_LOCAL_DISPLAY;
  ctx.user = kOledUser;
  ctx.ip = "oled";
  ctx.path = path ? path : "/oled";
  ctx.opaque = nullptr;
  return ctx;
}
static bool isOledIdentity(const AuthContext& ctx) {
  return ctx.transport == SOURCE_LOCAL_DISPLAY && ctx.user == kOledUser;
}
// checkPerm refuses an empty user (FsRole::ANON) before any path rule.
static bool permitted() { return currentAuthContext().user.length() > 0; }

struct OwnerCall {
  std::string what, arg;
  bool asOled;
};
static std::vector<OwnerCall> calls;
static void record(const char* what, const std::string& arg) {
  calls.push_back({what, arg, isOledIdentity(currentAuthContext())});
}
static int countCalls(const char* what) {
  int n = 0;
  for (const auto& c : calls) n += c.what == what;
  return n;
}
static const OwnerCall* lastCall(const char* what) {
  const OwnerCall* found = nullptr;
  for (const auto& c : calls) if (c.what == what) found = &c;
  return found;
}

// ---------------------------------------------------------------------------
// Map owners
// ---------------------------------------------------------------------------
struct LoadedMapHeader {
  int32_t minLat, maxLat, minLon, maxLon;
  uint16_t featureTypeMask;
};
// Only the fields the extracted Map code reads.
struct LoadedMap {
  bool valid;
  LoadedMapHeader header;
  char filepath[128];
  uint16_t nameCount;
  uint8_t tileGridSize;
  const void* tileDir;
  uint16_t tileCount;
  int32_t tileW, tileH, haloW, haloH;
};
static LoadedMap gMap{};
static std::vector<std::string> gMapFiles;  // /maps entries the owner lists
static std::vector<uint8_t> gTile;          // payload of tile 0 (routes fixture)
static std::vector<std::string> gNames;     // the map's name table
static bool gLoadMapOk = true;
static void setLoadedMap(const char* path) {
  gMap = LoadedMap{};
  gMap.valid = true;
  std::snprintf(gMap.filepath, sizeof(gMap.filepath), "%s", path);
  gMap.header = {40000000, 41000000, -75000000, -74000000, 0xFFFF};
}
struct MapCore {
  static const LoadedMap& getCurrentMap() { return gMap; }
  static int getAvailableMaps(char maps[][96], int maxMaps) {
    record("getAvailableMaps", "");
    if (!permitted()) return 0;
    int count = 0;
    for (const auto& name : gMapFiles) {
      if (count >= maxMaps) break;
      std::snprintf(maps[count++], 96, "%s", name.c_str());
    }
    return count;
  }
  static bool loadMapFile(const char* path) {
    record("loadMapFile", path);
    if (!permitted() || !gLoadMapOk) return false;
    setLoadedMap(path);
    return true;
  }
  static bool isPositionInMap(float, float) { return false; }
  static const uint8_t* loadTileData(uint16_t index, size_t* size) {
    if (index != 0 || gTile.empty()) { *size = 0; return nullptr; }
    *size = gTile.size();
    return gTile.data();
  }
  static const char* getName(uint16_t index) {
    return index < gNames.size() ? gNames[index].c_str() : nullptr;
  }
  static int searchNamesByPrefix(const char*, const char**, int) { return 0; }
};
struct MapCacheGuard { explicit MapCacheGuard(const char*) {} };

static Waypoint gWaypoints[MAX_WAYPOINTS];
static bool gWaypointSaveOk = true;
// Owner contract: a write that fails is reported and leaves memory unchanged.
struct WaypointManager {
  static constexpr int kNotSaved = -2;
  static int addWaypoint(float lat, float lon, const char* name) {
    record("addWaypoint", name ? name : "");
    if (!permitted() || !gWaypointSaveOk) return kNotSaved;
    for (int i = 0; i < MAX_WAYPOINTS; i++) {
      if (gWaypoints[i].active) continue;
      gWaypoints[i] = Waypoint{};
      gWaypoints[i].lat = lat;
      gWaypoints[i].lon = lon;
      std::snprintf(gWaypoints[i].name, sizeof(gWaypoints[i].name), "%s", name ? name : "");
      gWaypoints[i].active = true;
      return i;
    }
    return -1;
  }
  static bool deleteWaypoint(int index) {
    record("deleteWaypoint", std::to_string(index));
    if (index < 0 || index >= MAX_WAYPOINTS || !gWaypoints[index].active) return false;
    if (!permitted() || !gWaypointSaveOk) return false;
    gWaypoints[index].active = false;
    return true;
  }
  static const Waypoint* getWaypoint(int index) {
    if (index < 0 || index >= MAX_WAYPOINTS || !gWaypoints[index].active) return nullptr;
    return &gWaypoints[index];
  }
  static int getActiveCount() {
    int n = 0;
    for (const auto& w : gWaypoints) n += w.active;
    return n;
  }
  static void selectTarget(int index) { record("selectTarget", std::to_string(index)); }
};

struct GPSTrackStatus { int pointCount; };
static std::vector<std::string> gTrackFilesOnDisk;
struct GPSTrackManager {
  static bool loadTrack(const char* path, String& error, GPSTrackStatus& out, bool) {
    record("loadTrack", path);
    out = {};
    bool exists = false;
    for (const auto& f : gTrackFilesOnDisk) exists = exists || f == path;
    if (!permitted() || !exists) { error = "File not found"; return false; }
    out.pointCount = 3;
    return true;
  }
  static bool deleteTrackFile(const char* path) {
    record("deleteTrackFile", path);
    return permitted();
  }
  static bool saveTrack(char* path, size_t length, int* = nullptr) {
    std::snprintf(path, length, "%s", "/logging_captures/tracks/saved.csv");
    return true;
  }
};

// ---------------------------------------------------------------------------
// VFS / File boundary for the track scan (a few fixed entries, read-only)
// ---------------------------------------------------------------------------
struct FsNode { std::string path; bool dir; std::string content; };
static std::vector<FsNode> gFs;
struct VfsOpen { std::string path; AuthContext ctx; };
static std::vector<VfsOpen> gVfsOpens;
static std::string parentOf(const std::string& path) {
  const auto slash = path.rfind('/');
  return slash == 0 ? "/" : path.substr(0, slash);
}
class File {
 public:
  File() = default;
  explicit File(int node) : node_(node) {}
  explicit operator bool() const { return node_ >= 0; }
  bool isDirectory() const { return node_ >= 0 && gFs[node_].dir; }
  const char* path() const { return node_ >= 0 ? gFs[node_].path.c_str() : ""; }
  File openNextFile() {
    if (!isDirectory()) return File();
    while (++cursor_ < static_cast<int>(gFs.size())) {
      if (cursor_ != node_ && parentOf(gFs[cursor_].path) == gFs[node_].path) return File(cursor_);
    }
    return File();
  }
  int available() const {
    return node_ >= 0 ? static_cast<int>(gFs[node_].content.size() - readPos_) : 0;
  }
  String readStringUntil(char terminator) {
    const std::string& text = gFs[node_].content;
    const auto end = text.find(terminator, readPos_);
    const std::string line = text.substr(readPos_, end == std::string::npos ? std::string::npos : end - readPos_);
    readPos_ = end == std::string::npos ? text.size() : end + 1;
    return String(line);
  }
  void close() { node_ = -1; }

 private:
  int node_ = -1;
  int cursor_ = -1;
  size_t readPos_ = 0;
};
struct FsLockGuard { explicit FsLockGuard(const char*) {} };
namespace VFS {
static File openGuarded(const String& path, const char*, const AuthContext& ctx) {
  gVfsOpens.push_back({path.c_str(), ctx});
  if (ctx.user.length() == 0) return File();
  for (size_t i = 0; i < gFs.size(); i++) {
    if (gFs[i].path == path.c_str()) return File(static_cast<int>(i));
  }
  return File();
}
static bool isStagingName(const String& path) { return path.endsWith(".tmp"); }
static AuthContext systemAuth(const char* reason) {
  AuthContext ctx{};
  ctx.transport = SOURCE_INTERNAL;
  ctx.user = "system";
  ctx.path = reason;
  return ctx;
}
}  // namespace VFS

// ---------------------------------------------------------------------------
// OLED UI boundaries
// ---------------------------------------------------------------------------
struct TestDisplay {
  void setTextSize(int) {}
  void setTextColor(int) {}
  void setCursor(int, int) {}
  void print(const char*) {}
  void println(const char*) {}
};
static TestDisplay gTestDisplay;
static TestDisplay* oledDisplay = &gTestDisplay;
static constexpr int OLED_CONTENT_START_Y = 10;
static void drawMapMenu() {}

// The keyboard: init/complete/cancel/reset follow OLED_Utils (completing or
// cancelling clears `active`; while active the central router feeds it).
enum class OLEDKeyboardDictationPolicy { ALLOW_PLAINTEXT };
typedef int (*OLEDKeyboardAutocompleteFunc)(const char*, const char**, int, void*);
struct { bool active; } gOledKeyboardState;
static bool kbCompleted, kbCancelled;
static std::string kbText, kbTitle;
static int kbDraws, kbResets, kbInputs;
static void oledKeyboardInit(const char* title, const char* initialText, int,
                             OLEDKeyboardDictationPolicy) {
  gOledKeyboardState.active = true;
  kbCompleted = kbCancelled = false;
  kbTitle = title ? title : "";
  kbText = initialText ? initialText : "";
}
static bool oledKeyboardIsActive() { return gOledKeyboardState.active; }
static bool oledKeyboardIsCompleted() { return kbCompleted; }
static bool oledKeyboardIsCancelled() { return kbCancelled; }
static const char* oledKeyboardGetText() { return kbText.c_str(); }
static void oledKeyboardReset() {
  gOledKeyboardState.active = false;
  kbCompleted = kbCancelled = false;
  kbText.clear();
  ++kbResets;
}
static bool oledKeyboardHandleInput(int, int, uint32_t) { ++kbInputs; return true; }
static void oledKeyboardDisplay(TestDisplay*) { ++kbDraws; }
static void oledKeyboardSetAutocomplete(OLEDKeyboardAutocompleteFunc, void*) {}
static void keyboardFinish(const char* typed, bool complete) {
  kbText = typed;
  kbCompleted = complete;
  kbCancelled = !complete;
  gOledKeyboardState.active = false;
}

enum class OledUIIcon : uint8_t { NONE = 0, INFO, WARNING, ERROR, SUCCESS, QUESTION };
static std::vector<std::string> toasts;
static void oledToastShow(const char* message, uint32_t, OledUIIcon) { toasts.emplace_back(message); }
static bool gGuest;
static bool oledGuestBlocksMutate() { return gGuest; }
static std::vector<OLEDMode> modeRequests;
static void requestOLEDMode(OLEDMode mode, const char*, bool = true, bool = false) {
  modeRequests.push_back(mode);
}
static std::vector<std::string> commands;
static void executeOLEDCommand(const String& line) { commands.emplace_back(line.c_str()); }
static bool executeOLEDCommandWithResult(const String& line, char* out, size_t length) {
  commands.emplace_back(line.c_str());
  std::snprintf(out, length, "%s", "OK: GPS track cleared");
  return true;
}

// File picker (OLED_Mode_FileBrowser) and the viewer's FileManager.
static std::vector<FilePickerRequest> pickerPushes;
static bool gPickerPushOk = true;
static bool oledFilePickerPush(const FilePickerRequest& req) {
  if (!gPickerPushOk) return false;
  pickerPushes.push_back(req);
  return true;
}
class FileManager {
 public:
  bool navigate(const char* path) {
    record("navigate", path);
    return permitted();
  }
};
static FileManager gTestFileManager;
FileManager* gOledFileManager = &gTestFileManager;
bool oledFileBrowserNeedsInit = false;

// Sensors, layers, highlight.
volatile bool gSensorPollingPaused = false;
bool gGpsConnected = false;
bool gGpsRunning = false;
struct GPSCache { bool hasFix; float latitude, longitude; uint8_t satellites; };
static bool gpsCacheSnapshot(GPSCache& out) { out = {}; return false; }
static void mapLayerToggle(uint16_t) {}
struct { bool active; char name[64]; } gMapHighlight;
static void mapHighlightClear() {}
static void mapHighlightByNameAndType(const char* name, uint8_t, uint32_t) { record("highlight", name); }
static uint32_t gNowMs = 1000;
static uint32_t millis() { return gNowMs; }

// Map view globals owned by the renderer.
float gMapRotation = 0, gMapZoom = 1.0f, gMapCenterLat = 0, gMapCenterLon = 0;
bool gMapCenterSet = false, gMapManuallyPanned = false;
float gMapVelocityLat = 0, gMapVelocityLon = 0, gMapRotationVelocity = 0;
static volatile bool sForceRender = false;
static void executeSubtypeMenuAction(int, int) {}

// INSERT_GUARD

// INSERT_PRODUCTION

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------
// This device's own press bits for A (HAL_Input's mapping for the build).
static const uint32_t kButtonA = inputGetButtonMask(INPUT_BUTTON_A);

static void resetAll() {
  calls.clear(); toasts.clear(); modeRequests.clear(); commands.clear();
  pickerPushes.clear(); gVfsOpens.clear(); gFs.clear(); gTrackFilesOnDisk.clear();
  gMapFiles.clear(); gIdentityStack.clear(); gTile.clear(); gNames.clear();
  for (auto& w : gWaypoints) w = Waypoint{};
  gWaypointSaveOk = true; gLoadMapOk = true; gPickerPushOk = true; gGuest = false;
  oledKeyboardReset(); kbDraws = kbResets = kbInputs = 0; kbTitle.clear();
  gMapMenuOpen = false; gMapSubmenuLevel = 0; gMapSubmenuType = 0; gMapMenuSelection = 0;
  gMapMenuScrollOffset = 0;
  gWaypointSelectMode = 0; gWaypointSelectIdx = 0; gTrackSelectMode = 0; gTrackFileIdx = 0;
  gTrackFileCount = 0; gShowTrackStatus = false; gMapSearchMode = false;
  gShowMapInfo = false; gShowFeatures = false; gShowRoutes = false;
  gRoutesSelectedIdx = 0; gRoutesScrollOffset = 0;
  gSearchResultsActive = false; gSearchResultCount = 0; gSearchResultCurrent = 0;
  gNavEvents = NavEvents{};
  sForceRender = false;
  setLoadedMap("/maps/alpha/alpha.hwmap");
  gMapCenterLat = 40.5f; gMapCenterLon = -74.25f; gMapCenterSet = true; gMapManuallyPanned = true;
  gMapZoom = 1.0f; gMapRotation = 0;
}

// Open the submenu `category` and press A on item `item`, as the menu does.
static void pressMenuItem(int category, int item) {
  gMapMenuOpen = true;
  gMapSubmenuLevel = 1;
  gMapSubmenuType = category;
  gMapMenuSelection = item;
  gpsMapInputHandler(0, 0, kButtonA);
}

static bool hasToast(const char* text) {
  for (const auto& t : toasts) if (t == text) return true;
  return false;
}

static void addWaypointFixture(int index, const char* name, float lat, float lon) {
  gWaypoints[index] = Waypoint{};
  std::snprintf(gWaypoints[index].name, sizeof(gWaypoints[index].name), "%s", name);
  gWaypoints[index].lat = lat;
  gWaypoints[index].lon = lon;
  gWaypoints[index].active = true;
}

static void caseMarkWaypoint() {
  resetAll();
  pressMenuItem(MENU_CAT_WAYPOINTS, 0);
  EXPECT(oledKeyboardIsActive() && kbTitle == "Name Waypoint", "Mark Waypoint opens the name keyboard");
  displayGPSMap();
  EXPECT(kbDraws == 1 && calls.empty(), "the open keyboard is drawn and nothing else runs");
  keyboardFinish("Camp", true);           // central router: the keyboard completes
  gpsMapInputHandler(0, 0, 0);            // the release reaches the mode
  displayGPSMap();
  const OwnerCall* add = lastCall("addWaypoint");
  EXPECT(countCalls("addWaypoint") == 1, "Mark Waypoint adds the waypoint once the name is entered");
  EXPECT(add && add->arg == "Camp" && add->asOled, "Mark Waypoint adds the typed name as the OLED user");
  EXPECT(gWaypoints[0].active && gWaypoints[0].lat == 40.5f && gWaypoints[0].lon == -74.25f,
         "the waypoint is placed at the map center");
  EXPECT(!oledKeyboardIsCompleted() && !oledKeyboardIsActive(), "the finished keyboard is consumed");
  EXPECT(toasts.empty() && gIdentityStack.empty(), "success is silent and the identity scope unwinds");

  // Cancel: nothing is added, the keyboard is consumed.
  resetAll();
  pressMenuItem(MENU_CAT_WAYPOINTS, 0);
  keyboardFinish("Nope", false);
  gpsMapInputHandler(0, 0, 0);
  displayGPSMap();
  EXPECT(countCalls("addWaypoint") == 0 && !oledKeyboardIsCancelled(),
         "a cancelled name adds nothing and is consumed");

  // A refused write is reported on the display.
  resetAll();
  gWaypointSaveOk = false;
  pressMenuItem(MENU_CAT_WAYPOINTS, 0);
  keyboardFinish("Lost", true);
  gpsMapInputHandler(0, 0, 0);
  EXPECT(hasToast("Waypoint not saved") && !gWaypoints[0].active, "a waypoint that was not saved is reported");

  // All slots taken.
  resetAll();
  for (int i = 0; i < MAX_WAYPOINTS; i++) addWaypointFixture(i, "W", 1.0f, 2.0f);
  pressMenuItem(MENU_CAT_WAYPOINTS, 0);
  keyboardFinish("Full", true);
  gpsMapInputHandler(0, 0, 0);
  EXPECT(hasToast("No free waypoint slots"), "a full waypoint table is reported");

  // Guests stay view-only.
  resetAll();
  gGuest = true;
  pressMenuItem(MENU_CAT_WAYPOINTS, 0);
  EXPECT(!oledKeyboardIsActive() && calls.empty(), "a guest cannot mark a waypoint");
}

static void caseWaypointSelect() {
  resetAll();
  addWaypointFixture(0, "Home", 40.7f, -74.1f);
  addWaypointFixture(1, "Work", 40.8f, -74.2f);
  pressMenuItem(MENU_CAT_WAYPOINTS, 1);  // Goto Waypoint
  gWaypointSelectIdx = 1;
  gpsMapInputHandler(0, 0, kButtonA);
  const OwnerCall* target = lastCall("selectTarget");
  EXPECT(target && target->arg == "1" && target->asOled, "Goto Waypoint saves the target as the OLED user");
  EXPECT(gMapCenterLat == 40.8f && gMapCenterLon == -74.2f, "Goto Waypoint centers on it");

  calls.clear();
  pressMenuItem(MENU_CAT_WAYPOINTS, 2);  // Clear Nav
  target = lastCall("selectTarget");
  EXPECT(target && target->arg == "-1" && target->asOled, "Clear Nav saves as the OLED user");

  calls.clear();
  pressMenuItem(MENU_CAT_WAYPOINTS, 3);  // Delete Waypoint
  gpsMapInputHandler(0, 0, kButtonA);    // first active waypoint
  const OwnerCall* del = lastCall("deleteWaypoint");
  EXPECT(del && del->arg == "0" && del->asOled, "Delete Waypoint runs as the OLED user");
  EXPECT(!gWaypoints[0].active && gWaypoints[1].active && toasts.empty(), "the chosen waypoint is deleted");
  EXPECT(gIdentityStack.empty(), "the identity scope unwinds");

  gWaypointSaveOk = false;
  pressMenuItem(MENU_CAT_WAYPOINTS, 3);
  gpsMapInputHandler(0, 0, kButtonA);
  EXPECT(gWaypoints[1].active && hasToast("Delete not saved"),
         "a deletion that was not saved is reported and the waypoint stays");
}

static void addTrackFixtures() {
  const std::string gps = "# time,lat,lon\n1000,40.71234,-74.00123\n";
  gFs = {
      {"/logging_captures", true, ""},
      {"/logging_captures/notes.txt", false, "hello\nworld\n"},
      {"/logging_captures/tracks", true, ""},
      {"/logging_captures/tracks/day.csv", false, gps},
      {"/logging_captures/sensors", true, ""},
      {"/logging_captures/sensors/2026-10-12", true, ""},
      {"/logging_captures/sensors/2026-10-12/walk.csv", false, gps},
      {"/logging_captures/sensors/2026-10-12/walk.csv.tmp", false, gps},
  };
  gTrackFilesOnDisk = {"/logging_captures/tracks/day.csv",
                       "/logging_captures/sensors/2026-10-12/walk.csv"};
}

static void caseTracks() {
  resetAll();
  addTrackFixtures();
  pressMenuItem(MENU_CAT_TRACKS, 0);  // Load Track: scans
  bool allOled = !gVfsOpens.empty();
  for (const auto& open : gVfsOpens) allOled = allOled && isOledIdentity(open.ctx);
  EXPECT(allOled, "the track picker scans as the OLED user (the identity Load Track opens with)");
  auto listed = [](const char* path) {
    for (int i = 0; i < gTrackFileCount; i++) if (std::strcmp(gTrackFiles[i], path) == 0) return true;
    return false;
  };
  EXPECT(gTrackSelectMode == 1 && listed("/logging_captures/tracks/day.csv") &&
         listed("/logging_captures/sensors/2026-10-12/walk.csv") &&
         !listed("/logging_captures/notes.txt") &&
         !listed("/logging_captures/sensors/2026-10-12/walk.csv.tmp"),
         "the GPS logs are listed, the note and the staging file are not");
  gpsMapInputHandler(0, 0, kButtonA);
  const OwnerCall* load = lastCall("loadTrack");
  EXPECT(load && load->asOled, "Load Track opens the listed file as the OLED user");
  EXPECT(toasts.empty() && sForceRender, "a listed track loads (no \"File not found\")");
  EXPECT(gIdentityStack.empty(), "the identity scope unwinds");

  resetAll();
  addTrackFixtures();
  pressMenuItem(MENU_CAT_TRACKS, 3);  // Delete Track
  gpsMapInputHandler(0, 0, kButtonA);
  const OwnerCall* del = lastCall("deleteTrackFile");
  EXPECT(del && del->asOled && toasts.empty(), "Delete Track removes the file as the OLED user");

  // Clear Track keeps its authorized command path.
  resetAll();
  pressMenuItem(MENU_CAT_TRACKS, 1);
  EXPECT(commands.size() == 1 && commands[0] == "gpstrack clear", "Clear Track still runs the command");
}

static void caseNextPreviousMap() {
  resetAll();
  gMapFiles = {"alpha/alpha.hwmap", "beta/beta.hwmap"};
  pressMenuItem(MENU_CAT_MAPS, 1);  // Next Map
  const OwnerCall* load = lastCall("loadMapFile");
  EXPECT(load && load->arg == "/maps/beta/beta.hwmap" && load->asOled,
         "Next Map lists and loads the next map as the OLED user");
  pressMenuItem(MENU_CAT_MAPS, 2);  // Previous Map
  load = lastCall("loadMapFile");
  EXPECT(load && load->arg == "/maps/alpha/alpha.hwmap" && load->asOled,
         "Previous Map loads the previous map as the OLED user");
  EXPECT(gIdentityStack.empty(), "the identity scope unwinds");
}

static void caseAutoLoad() {
  resetAll();
  gMap = LoadedMap{};
  gMapFiles = {"alpha/alpha.hwmap"};
  gNowMs = 100000;
  displayGPSMap();
  const OwnerCall* load = lastCall("loadMapFile");
  EXPECT(load && load->arg == "/maps/alpha/alpha.hwmap" && load->asOled,
         "with no map loaded the first map auto-loads as the OLED user");
  EXPECT(gMap.valid && gIdentityStack.empty(), "the auto-loaded map is current");

  // A map that fails to load is not re-read on every frame.
  gMap = LoadedMap{};
  gLoadMapOk = false;
  calls.clear();
  gNowMs = 200000;
  displayGPSMap();
  gNowMs += 100;
  displayGPSMap();
  EXPECT(countCalls("getAvailableMaps") == 1, "a failed auto-load is not retried on the next frame");
  gNowMs += 5000;
  displayGPSMap();
  EXPECT(countCalls("getAvailableMaps") == 2 && countCalls("loadMapFile") == 2,
         "the auto-load is retried after 5 s");
}

static void caseSelectMap() {
  resetAll();
  pressMenuItem(MENU_CAT_MAPS, 0);  // Select Map
  EXPECT(pickerPushes.size() == 1, "Select Map opens the file browser as a map picker");
  EXPECT(countCalls("navigate") == 0, "Select Map does not navigate the browser without an identity");
  EXPECT(modeRequests.size() == 1 && modeRequests[0] == OLED_FILE_BROWSER, "the file browser is opened");
  EXPECT(!gMapMenuOpen, "the map menu closes");
  if (!pickerPushes.empty()) {
    const FilePickerRequest req = pickerPushes[0];
    EXPECT(std::strcmp(req.startPath, "/maps") == 0 && req.requesterMode == OLED_GPS_MAP &&
           std::strcmp(req.title, "Select Map") == 0, "the picker starts in /maps and returns to the map");
    FileEntry entry{};
    auto shows = [&](const char* name) {
      std::snprintf(entry.name, sizeof(entry.name), "%s", name);
      return req.filter && req.filter(entry);
    };
    EXPECT(shows("city.hwmap") && shows("CITY.HWMAP") && !shows("waypoints_city.json") && !shows("hwmap"),
           "the picker shows .hwmap files only");
    calls.clear();
    req.onPicked("/maps/beta/beta.hwmap", false);
    const OwnerCall* load = lastCall("loadMapFile");
    EXPECT(load && load->arg == "/maps/beta/beta.hwmap" && load->asOled && !gMapCenterSet,
           "the picked map loads as the OLED user and is recentered");
    calls.clear();
    req.onPicked(nullptr, true);
    EXPECT(calls.empty(), "a cancelled pick loads nothing");
    gLoadMapOk = false;
    req.onPicked("/maps/bad/bad.hwmap", false);
    EXPECT(hasToast("Map load failed"), "a picked map that fails to load is reported");
  }

  resetAll();
  gPickerPushOk = false;
  pressMenuItem(MENU_CAT_MAPS, 0);
  EXPECT(modeRequests.empty() && hasToast("File picker busy"), "a refused picker is reported, no mode change");
}

// ---------------------------------------------------------------------------
// Input devices: one frame as processOLEDInput (OLED_Utils.cpp) hands it to
// the mode. newlyPressed is the device's raw press bits; gNavEvents carries
// the wheel's detents and its sign as up/down (vertical axis), the ANO dpad's
// press edges as nav booleans, and the stick's deflection with a nav event
// only when the router emits one (first deflection past JOYSTICK_DEADZONE or
// a due auto-repeat).
// ---------------------------------------------------------------------------
static bool frame(uint32_t pressed, int wheel = 0, int stickX = 0, int stickY = 0, bool navDue = true) {
  gNavEvents = NavEvents{};
  gNavEvents.deltaX = stickX;
  gNavEvents.deltaY = stickY;
  gNavEvents.wheelDelta = wheel;
  if (wheel > 0) gNavEvents.down = true;
  if (wheel < 0) gNavEvents.up = true;
#if MAP_TEST_DEVICE == MAP_TEST_ANO
  if (pressed & ANO_BTN_UP) gNavEvents.up = true;
  if (pressed & ANO_BTN_DOWN) gNavEvents.down = true;
  if (pressed & ANO_BTN_LEFT) gNavEvents.left = true;
  if (pressed & ANO_BTN_RIGHT) gNavEvents.right = true;
#endif
  if (navDue) {
    if (stickX > JOYSTICK_DEADZONE) gNavEvents.right = true;
    if (stickX < -JOYSTICK_DEADZONE) gNavEvents.left = true;
    if (stickY > JOYSTICK_DEADZONE) gNavEvents.down = true;
    if (stickY < -JOYSTICK_DEADZONE) gNavEvents.up = true;
  }
  const bool handled = gpsMapInputHandler(stickX, stickY, pressed);
  gNavEvents = NavEvents{};
  return handled;
}
static bool press(InputButton button) { return frame(inputGetButtonMask(button)); }

// One step through a list the way this device moves: the stick on the
// seesaw, the wheel on the encoders.
static constexpr bool kStickDevice = MAP_TEST_DEVICE == MAP_TEST_SEESAW;
static constexpr int kStick = JOYSTICK_DEADZONE + 100;
static void stepDown() { if (kStickDevice) frame(0, 0, 0, kStick); else frame(0, 1); }
static void stepUp() { if (kStickDevice) frame(0, 0, 0, -kStick); else frame(0, -1); }
// Next/previous search result: stick right/left on the seesaw, the wheel otherwise.
static void stepNext() { if (kStickDevice) frame(0, 0, kStick, 0); else frame(0, 1); }
static void stepPrevious() { if (kStickDevice) frame(0, 0, -kStick, 0); else frame(0, -1); }

static void caseMenuInput() {
  resetAll();
  EXPECT(press(INPUT_BUTTON_START) && gMapMenuOpen && gMapSubmenuLevel == 0 && gMapMenuSelection == 0,
         "START opens the map menu");
  stepDown();
  EXPECT(gMapMenuSelection == 1, "the wheel or stick moves the menu selection down");
  stepUp();
  EXPECT(gMapMenuSelection == 0, "the wheel or stick moves the menu selection up");
  stepDown();
  press(INPUT_BUTTON_A);
  EXPECT(gMapMenuOpen && gMapSubmenuLevel == 1 && gMapSubmenuType == MENU_CAT_MAPS && gMapMenuSelection == 0,
         "A opens the selected submenu");
  stepDown();
  EXPECT(gMapSubmenuLevel == 1 && gMapMenuSelection == 1, "the submenu selection moves");
  press(INPUT_BUTTON_B);
  EXPECT(gMapMenuOpen && gMapSubmenuLevel == 0 && gMapMenuSelection == MENU_CAT_MAPS,
         "B goes back to the main menu");
  press(INPUT_BUTTON_B);
  EXPECT(!gMapMenuOpen, "B on the main menu closes it");

#if MAP_TEST_DEVICE == MAP_TEST_SEESAW
  // A held stick moves the menu at the router's cadence (first deflection,
  // then auto-repeat), not on every frame it stays deflected.
  gMapMenuOpen = true;
  gMapMenuSelection = 0;
  frame(0, 0, 0, kStick, false);
  EXPECT(gMapMenuSelection == 0, "a held stick moves the menu only on the router's nav events");
#elif MAP_TEST_DEVICE == MAP_TEST_ANO
  gMapMenuOpen = true;
  gMapMenuSelection = 1;
  frame(ANO_BTN_UP);
  EXPECT(gMapMenuOpen && gMapSubmenuLevel == 0 && gMapMenuSelection == 0,
         "ANO UP moves up the menu and does not back out");
  frame(ANO_BTN_DOWN);
  EXPECT(gMapMenuOpen && gMapMenuSelection == 1 && gMapZoom == 1.0f, "ANO DOWN moves down the menu");
#else
  gMapMenuOpen = true;
  gMapMenuSelection = 1;
  press(INPUT_BUTTON_SELECT);
  EXPECT(gMapMenuOpen && gMapSubmenuLevel == 0, "a double tap (SELECT) does not confirm a menu item");
#endif
}

static void casePickerInput() {
  resetAll();
  addTrackFixtures();
  pressMenuItem(MENU_CAT_TRACKS, 0);  // Load Track
  EXPECT(gTrackSelectMode == 1 && gTrackFileCount >= 2 && gTrackFileIdx == 0, "Load Track opens the track picker");
  stepDown();
  EXPECT(gTrackFileIdx == 1, "the wheel or stick moves down the track list");
  stepUp();
  EXPECT(gTrackFileIdx == 0, "the wheel or stick moves up the track list");
  stepDown();
  press(INPUT_BUTTON_A);
  const OwnerCall* load = lastCall("loadTrack");
  EXPECT(load && load->arg == gTrackFiles[1] && gTrackSelectMode == 0, "A loads the selected track");
  pressMenuItem(MENU_CAT_TRACKS, 3);  // Delete Track
  press(INPUT_BUTTON_B);
  EXPECT(gTrackSelectMode == 0 && countCalls("deleteTrackFile") == 0, "B cancels the track picker");

  resetAll();
  addWaypointFixture(0, "Home", 40.7f, -74.1f);
  addWaypointFixture(1, "Work", 40.8f, -74.2f);
  pressMenuItem(MENU_CAT_WAYPOINTS, 1);  // Goto Waypoint
  EXPECT(gWaypointSelectMode == 1 && gWaypointSelectIdx == 0, "Goto Waypoint opens the waypoint picker");
  stepDown();
  EXPECT(gWaypointSelectIdx == 1, "the wheel or stick moves through the waypoints");
  press(INPUT_BUTTON_A);
  const OwnerCall* target = lastCall("selectTarget");
  EXPECT(target && target->arg == "1" && gMapCenterLat == 40.8f && gWaypointSelectMode == 0,
         "A goes to the selected waypoint");
  pressMenuItem(MENU_CAT_WAYPOINTS, 3);  // Delete Waypoint
  press(INPUT_BUTTON_B);
  EXPECT(gWaypointSelectMode == 0 && countCalls("deleteWaypoint") == 0, "B cancels the waypoint picker");
}

static void caseViewerInput() {
  resetAll();
  gMap.nameCount = 20;
  pressMenuItem(MENU_CAT_INFO, 1);  // Features
  EXPECT(gShowFeatures && gFeaturesScrollOffset == 0, "Features opens the features viewer");
  stepDown();
  EXPECT(gFeaturesScrollOffset == 1, "the wheel or stick scrolls the features list down");
  stepUp();
  EXPECT(gFeaturesScrollOffset == 0, "the wheel or stick scrolls the features list up");
#if MAP_TEST_DEVICE == MAP_TEST_SEESAW
  frame(0, 0, kStick, 0);
  EXPECT(gFeaturesCategory == 1, "stick right shows the next feature category");
  frame(0, 0, -kStick, 0);
  EXPECT(gFeaturesCategory == 0, "stick left shows the previous feature category");
#elif MAP_TEST_DEVICE == MAP_TEST_ANO
  frame(ANO_BTN_RIGHT);
  EXPECT(gFeaturesCategory == 1, "the ANO dpad RIGHT shows the next feature category");
#endif
  press(INPUT_BUTTON_B);
  EXPECT(!gShowFeatures && !gMapMenuOpen, "B closes the features viewer");

  // Two rail routes in tile 0: type, subtype, name index, point count, one point.
  resetAll();
  static_assert(HWMAP_FEATURE_HEADER_SIZE == 6, "route fixture layout");
  gNames = {"", "Red Line", "Blue Line"};
  auto put16 = [](unsigned value) {
    gTile.push_back(static_cast<uint8_t>(value & 0xFF));
    gTile.push_back(static_cast<uint8_t>(value >> 8));
  };
  auto rail = [&](unsigned nameIndex, unsigned qLat) {
    gTile.push_back(0x20);  // rail
    gTile.push_back(0);     // subtype
    put16(nameIndex);
    put16(1);               // one point: qLat, qLon
    put16(qLat);
    put16(0x8000);
  };
  put16(2);  // feature count
  rail(1, 0x4000);
  rail(2, 0xC000);
  gMap.tileDir = gTile.data();
  gMap.tileCount = 1;
  gMap.tileGridSize = 1;
  gMap.tileW = gMap.tileH = 1000000;
  pressMenuItem(MENU_CAT_INFO, 3);  // Transit Routes
  EXPECT(gShowRoutes && gRoutesSelectedIdx == 0, "Transit Routes opens the routes viewer");
  stepDown();
  EXPECT(gRoutesSelectedIdx == 1, "the wheel or stick moves through the routes");
  press(INPUT_BUTTON_A);
  EXPECT(!gShowRoutes && std::fabs(gMapCenterLat - 40.75f) < 1e-4f && std::fabs(gMapCenterLon + 74.5f) < 1e-4f,
         "A centers the map on the selected route");
  gShowRoutes = true;
  gRoutesSelectedIdx = 0;
  press(INPUT_BUTTON_X);
  const OwnerCall* highlight = lastCall("highlight");
  EXPECT(highlight && highlight->arg == "Red Line" && !gShowRoutes, "X highlights the selected route");
  gShowRoutes = true;
  press(INPUT_BUTTON_B);
  EXPECT(!gShowRoutes, "B closes the routes viewer");

  resetAll();
  gSearchResultsActive = true;
  gSearchResultCount = 3;
  for (int i = 0; i < 3; i++) gSearchResultCoords[i] = {41.0f + i, -73.0f - i};
  stepNext();
  EXPECT(gSearchResultCurrent == 1 && gMapCenterLat == 42.0f, "right or the wheel shows the next search result");
  stepPrevious();
  stepPrevious();
  EXPECT(gSearchResultCurrent == 2 && gMapCenterLat == 43.0f,
         "left or the wheel shows the previous search result (wrapping)");
  press(INPUT_BUTTON_B);
  EXPECT(!gSearchResultsActive, "B leaves the search results");

  resetAll();
  gShowMapInfo = true;
  gMapMenuOpen = true;
  press(INPUT_BUTTON_A);
  EXPECT(!gShowMapInfo && !gMapMenuOpen, "A dismisses the map info overlay");
  gShowTrackStatus = true;
  press(INPUT_BUTTON_B);
  EXPECT(!gShowTrackStatus, "B dismisses the track status overlay");
}

static void caseMapViewInput() {
  resetAll();
  EXPECT(press(INPUT_BUTTON_X) && gMapZoom == MAP_ZOOM_STEP, "X zooms in");
  EXPECT(press(INPUT_BUTTON_Y) && gMapZoom == 1.0f, "Y zooms out");
  EXPECT(!press(INPUT_BUTTON_B) && !gMapMenuOpen && gMapZoom == 1.0f,
         "B leaves the map (the handler hands it back to the router)");
}

int main() {
  caseMarkWaypoint();
  caseWaypointSelect();
  caseTracks();
  caseNextPreviousMap();
  caseAutoLoad();
  caseSelectMap();
  caseMenuInput();
  casePickerInput();
  caseViewerInput();
  caseMapViewInput();
  if (failures) {
    std::fprintf(stderr, "oled map actions: %d of %u checks failed\n", failures, checks);
    return 1;
  }
  std::printf("oled map actions: %u checks passed\n", checks);
  return 0;
}
