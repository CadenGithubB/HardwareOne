// Runs OLED_Mode_Bluetooth.cpp as shipped, with the real BLE_LinkStatus.cpp
// behind it. test_oled_bluetooth_link_watch.py copies both (with
// BLE_LinkStatus.h, BLE_LinkStatusCore.h and OLED_Layout.h) beside empty
// stand-ins for every other header they include, so what those headers would
// provide is faked here. Command line: ENABLE_G2_GLASSES, ENABLE_BLE_PHONE_PIPE
// and OLED_CONTENT_HEIGHT (43 on a 64 px OLED, 107 on the 128 px P4 canvas).
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "OLED_Layout.h"

#define ENABLE_OLED_DISPLAY 1
#define ENABLE_BLUETOOTH 1
#define ENABLE_R1_HEALTH 0
#define EXT_RAM_BSS_ATTR

static unsigned checks = 0;
#define CHECK(c)                                                                \
  do {                                                                          \
    ++checks;                                                                   \
    if (!(c)) {                                                                 \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      std::exit(1);                                                             \
    }                                                                           \
  } while (0)

// ---- Arduino -------------------------------------------------------------------
struct String {
  std::string s;
  String(const char* v = "") : s(v ? v : "") {}
  size_t length() const { return s.size(); }
  const char* c_str() const { return s.c_str(); }
};
static uint32_t gMillis = 100000;
static uint32_t millis() { return gMillis; }

// ---- Display: size-1 text as Adafruit_GFX lays it out (6x8 cells, wrap at 128) --
#define SSD1306_WHITE 1
#define OLED_CONTENT_START_Y 11
static constexpr int kContentEnd = OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT;
static constexpr bool kTall = OLED_CONTENT_HEIGHT > oled_layout::kOledContentHeight;

struct FakeDisplay {
  int x = 0, y = 0;
  std::map<int, std::string> rows;  // cursor y -> characters by column
  bool wrapped = false;             // a row ran past 21 columns
  bool intoFooter = false;          // text where drawOLEDFooter clears
  void clear() { *this = FakeDisplay(); }
  void setTextSize(int) {}
  void setTextColor(int) {}
  void setCursor(int cx, int cy) { x = cx; y = cy; }
  int16_t getCursorY() const { return int16_t(y); }
  void put(char c) {
    if (c == '\n') { x = 0; y += 8; return; }
    if (x + 6 > 128) { wrapped = true; x = 0; y += 8; }
    if (y + 8 > kContentEnd) intoFooter = true;
    std::string& row = rows[y];
    const size_t col = size_t(x / 6);
    if (row.size() <= col) row.resize(col + 1, ' ');
    row[col] = c;
    x += 6;
  }
  void print(const char* t) { for (; t && *t; ++t) put(*t); }
  void println(const char* t) { print(t); put('\n'); }
  void println() { put('\n'); }
};
static FakeDisplay gScreen;
static FakeDisplay* oledDisplay = &gScreen;

// ---- OLED framework ------------------------------------------------------------
enum OLEDMode { OLED_BLUETOOTH, OLED_BLUETOOTH_STATUS, OLED_BLUETOOTH_G2, OLED_BLUETOOTH_G2_STATUS,
                OLED_BLUETOOTH_R1, OLED_BLUETOOTH_PHONE, OLED_BLUETOOTH_PHONE_CODE, OLED_SETTINGS };
static OLEDMode currentOLEDMode = OLED_BLUETOOTH;
typedef void (*OLEDDisplayFunc)();
typedef bool (*OLEDAvailabilityFunc)(String*);
typedef bool (*OLEDInputFunc)(int, int, uint32_t);
typedef void (*OLEDModeEnterFunc)(bool);
struct OLEDModeEntry {
  OLEDMode mode;
  const char* name;
  const char* iconName;
  OLEDDisplayFunc displayFunc;
  OLEDAvailabilityFunc availFunc;
  OLEDInputFunc inputFunc;
  bool showInMenu;
  int menuOrder;
  const char* hints;
  OLEDModeEnterFunc onEnterFunc;
};
#define REGISTER_OLED_MODE_MODULE(modes, count, name)               \
  static const OLEDModeEntry* const kRegisteredModes = (modes);     \
  static const size_t kRegisteredModeCount = (count)

#define OLED_SCROLL_MAX_ITEMS 16
struct OLEDScrollItem { const char* line1; const char* line2; bool isSelectable; };
struct OLEDScrollState {
  OLEDScrollItem items[OLED_SCROLL_MAX_ITEMS];
  int itemCount, selectedIndex, scrollOffset, visibleLines;
};
static void oledScrollInit(OLEDScrollState* st, const char*, int visibleLines) {
  *st = OLEDScrollState{};
  st->visibleLines = visibleLines;
}
static bool oledScrollAddItem(OLEDScrollState* st, const char* line1, const char* line2 = nullptr,
                              bool selectable = true) {
  if (st->itemCount >= OLED_SCROLL_MAX_ITEMS) return false;
  st->items[st->itemCount++] = {line1, line2, selectable};
  return true;
}
static void oledScrollClearKeepSelection(OLEDScrollState* st) { st->itemCount = 0; }
static void oledScrollClampSelection(OLEDScrollState* st) {
  if (st->selectedIndex >= st->itemCount) st->selectedIndex = st->itemCount - 1;
  if (st->selectedIndex < 0) st->selectedIndex = 0;
  if (st->selectedIndex < st->scrollOffset) st->scrollOffset = st->selectedIndex;
  if (st->selectedIndex >= st->scrollOffset + st->visibleLines)
    st->scrollOffset = st->selectedIndex - st->visibleLines + 1;
}
static bool oledScrollHandleNav(OLEDScrollState*) { return false; }
// OLED_Utils.cpp oledScrollRenderSimple: one 8 px row per visible item, "> " on
// the selection. Its scrollbar is pixels, not text, so it is left out here.
static void oledScrollRenderSimple(FakeDisplay* display, OLEDScrollState* state,
                                   bool showSelection = true, int startY = OLED_CONTENT_START_Y) {
  oledScrollClampSelection(state);
  const int end = std::min(state->itemCount, state->scrollOffset + state->visibleLines);
  int y = startY;
  for (int i = state->scrollOffset; i < end; i++, y += 8) {
    display->setCursor(0, y);
    display->print((showSelection && i == state->selectedIndex) ? "> " : "  ");
    if (state->items[i].line1) display->print(state->items[i].line1);
    display->println();
  }
}
static void oledScrollUp(OLEDScrollState*) {}
static void oledScrollDown(OLEDScrollState*) {}
inline constexpr int oledVisibleRows(int rowHeight, int reservedHeight = 0, int baselineRows = 0) {
  return oled_layout::visibleRows(OLED_CONTENT_HEIGHT, rowHeight, reservedHeight, baselineRows);
}

#define INPUT_BUTTON_A 1u
#define INPUT_BUTTON_X 2u
#define INPUT_BUTTON_B 4u
#define INPUT_CHECK(state, btn) (((state) & (btn)) != 0)
struct NavEvents { bool up = false, down = false; };
[[maybe_unused]] static NavEvents gNavEvents;  // read by the G2/R1/Phone input handlers

static std::vector<std::string> gCommands;
static std::string gToast;
static int gMarks = 0;
static void oledMarkDirty() { ++gMarks; }
static bool oledRunCommand(const char* cmd) { gCommands.push_back(cmd); return true; }
static void executeOLEDCommand(const char* cmd) { gCommands.push_back(cmd); }
static bool executeOLEDCommandWithResult(const char* cmd, char* out, size_t n) {
  gCommands.push_back(cmd);
  std::snprintf(out, n, "OK");
  return true;
}
static std::vector<std::string> gToasts;  // every toast, for the code's no-toast check
static void oledToastShow(const char* message, uint32_t = 3000) { gToast = message; gToasts.push_back(message); }
// oledRunCommandAsync (OLED_Utils.cpp): queued on cmd_exec, its done callback.
typedef void (*OledCommandDoneFn)(bool ok, const void* context);
static std::vector<std::string> gAsyncCommands;
static bool oledRunCommandAsync(const char* cmd, OledCommandDoneFn, const void*) {
  gAsyncCommands.push_back(cmd);
  return true;
}
typedef void (*OLEDConfirmCallback)(void*);
// The open confirm: lines as drawn, the default and the Yes callback.
struct Confirm {
  bool open = false;
  std::string line1, line2;
  bool defaultYes = true;
  OLEDConfirmCallback onYes = nullptr;
  void* userData = nullptr;
};
static Confirm gConfirm;
static bool oledConfirmRequest(const char* line1, const char* line2, OLEDConfirmCallback onYes,
                               void* userData, bool defaultYes = true) {
  if (gConfirm.open) return false;  // one at a time, as OLED_Utils.cpp
  gConfirm = Confirm{true, line1 ? line1 : "", line2 ? line2 : "", defaultYes, onYes, userData};
  return true;
}
// Yes on the open confirm, as oledConfirmHandleInput: the callback, then close.
static void confirmYes() {
  CHECK(gConfirm.open);
  const Confirm c = gConfirm;
  if (c.onYes) c.onYes(c.userData);
  gConfirm = Confirm();
}
static bool oledGuestBlocksMutate() { return false; }
static void requestOLEDMode(OLEDMode mode, const char*) { currentOLEDMode = mode; }
static bool openSettingsEditorForModule(const char*) { return false; }

// ---- The live state bleLinkStatus() and the screens read ----------------------------
struct BleConsoleView {
  bool up = false;
  bool advertising = false;
  uint8_t clients = 0;
  bool signedIn = false;
  bool registered = false, attachable = false, attached = false, secret = false;
};
struct World {
  bool server = false;         // console GATT server up
  bool glassesClient = false;  // G2 client up
  bool glasses = false, ring = false;
  bool pipeActive = false, pipeReady = false, enrolling = false;
  uint8_t step = 0;            // BLE_PhonePipe.cpp's Step, in hw1link::PipeStep order
  const char* label = "stopped";
  bool paired = false, autoReconnect = false;
  BleConsoleView console;
  const char* recent[2] = {nullptr, nullptr};
  int recentCount = 0;
};
static World w;

// Bluetooth.h
#define BLE_MAX_CONNECTIONS 4
enum BLEConnectionState { BLE_STATE_IDLE, BLE_STATE_ADVERTISING, BLE_STATE_SCANNING,
                          BLE_STATE_CONNECTING, BLE_STATE_CONNECTED, BLE_STATE_DISCONNECTING };
enum { BLE_STREAM_SENSORS = 1, BLE_STREAM_SYSTEM = 2, BLE_STREAM_EVENTS = 4 };
struct BLEConnection { bool active; uint32_t connectedSince; String deviceName; };
struct BLESystemState {
  bool initialized;
  BLEConnectionState connectionState;
  BLEConnection connections[BLE_MAX_CONNECTIONS];
  uint8_t activeConnectionCount;
  uint32_t totalConnections, commandsReceived, responsesSent;
  uint8_t streamFlags;
  uint32_t sensorStreamInterval, systemStreamInterval;
};
static BLESystemState* gBLEState = nullptr;  // freed (nullptr) unless a test hosts the console
static bool isBleServerInitialized() { return w.server; }
static bool bleServerUp() { return w.server; }
static bool bleSubsystemActive() { return w.pipeActive || w.server || w.glassesClient; }
static BleConsoleView bleConsoleView() { return w.console; }
static int bleGetRecentMessages(const char* out[], int maxLines) {
  int n = w.recentCount < maxLines ? w.recentCount : maxLines;
  for (int i = 0; i < n; ++i) out[i] = w.recent[i];
  return n;
}

// G2_Glasses.h, G2_Ring.h, BLE_Peers.h, BLE_CentralCoordinator.h
enum G2State { G2_STATE_IDLE, G2_STATE_SCANNING, G2_STATE_CONNECTING, G2_STATE_AUTHENTICATING };
static bool isG2Connected() { return w.glasses; }
static bool isG2ClientInitialized() { return w.glassesClient; }
static G2State getG2State() { return G2_STATE_IDLE; }
static const char* getG2StateString() { return "idle"; }
static void getG2Status(char* out, size_t n) { std::snprintf(out, n, "ok"); }
static void g2ShowText(const char*) {}
bool gG2MenuNavEnabled = false;
static bool g2RingIsConnected() { return w.ring; }
enum BlePeerKind { BLE_PEER_G2_GLASSES, BLE_PEER_R1_RING, BLE_PEER_PHONE, BLE_PEER_MAX };  // BLE_Peers.h
struct BlePeerReconnectSnapshot { bool autoReconnect = false; bool hasSavedTarget = false; };
static bool blePeerReconnectSnapshot(BlePeerKind kind, BlePeerReconnectSnapshot& out) {
  out = BlePeerReconnectSnapshot{};
  if (kind == BLE_PEER_PHONE) {
    out.hasSavedTarget = w.paired;
    out.autoReconnect = w.autoReconnect;
  }
  return true;
}
static bool blePeerAutoReconnectEnabled(BlePeerKind) { return false; }
static bool bleCentralRingShouldYield() { return false; }

// BLE_PhonePipe.h
static bool phonePipeActive() { return w.pipeActive; }
static bool phonePipeReady() { return w.pipeReady; }
static const char* phonePipeStatus() { return w.label; }
static uint8_t phonePipeStep() { return w.step; }
static bool phonePipeEnrolling() { return w.enrolling; }
#if ENABLE_BLE_PHONE_PIPE
// BLE_PhonePipe.h, BLE_PhonePipeConsole.h and Bluetooth.h (phone link v2), as
// far as BLE_LinkStatus.cpp reads them: no summary yet and no console stream,
// so the console words stay the ones these screens pin (test_phone_status
// runs the stream's words).
struct PhonePipeLinkSummary {
  bool active = false, ready = false, relayOn = false, consoleOffered = false;
  uint8_t relayStreams = 0, consoleStreams = 0;
  uint32_t ended = 0;
  uint16_t lastEnd = 0;
  uint32_t endReason[19] = {};
};
static bool phonePipeLinkSummary(PhonePipeLinkSummary* out) { if (out) *out = {}; return false; }
enum class PipeConsoleState : uint8_t { Off, NotOffered, Waiting, Opening, Open, Closing, Backoff, Stopped };
struct PipeConsoleStatus { PipeConsoleState state = PipeConsoleState::Off; uint16_t streamId = 0; };
static void pipeConsoleStatus(PipeConsoleStatus* out) { if (out) *out = {}; }
struct BlePipeConsoleIdentity { bool bound = false; uint16_t streamId = 0; bool authed = false; };
static void blePipeConsoleIdentity(BlePipeConsoleIdentity* out) { if (out) *out = {}; }
// System_User.h: the code screen draws a code only while the display session
// that issued it is live (a logout or another login ends it).
enum CommandSource { SOURCE_WEB = 0, SOURCE_SERIAL = 1, SOURCE_LOCAL_DISPLAY = 4 };
using TransportSessionEpoch = uint32_t;
static TransportSessionEpoch gDisplayEpoch = 21;  // the live display session's
static unsigned gEpochChecks = 0;
static bool transportSessionEpochIsLive(CommandSource source, TransportSessionEpoch epoch) {
  ++gEpochChecks;
  return source == SOURCE_LOCAL_DISPLAY && epoch && epoch == gDisplayEpoch;
}
// `phone pair new`'s window (BLE_PhonePipe.h), as the pipe owner shows it to
// the code screen: the code only while open with its code on the display.
// test_oled_bluetooth_link_watch.py pins these fields to the real header's.
enum class PhonePairWindow : uint8_t { None, Open, Paired, Expired, Closed };
struct PhonePairView {
  PhonePairWindow state = PhonePairWindow::None;
  uint32_t id = 0, msLeft = 0, ageMs = 0;
  bool onDisplay = false;
  CommandSource source = SOURCE_SERIAL;
  TransportSessionEpoch epoch = 0;
};
struct PairWorld { PhonePairView view; char code[20] = {}; };
static PairWorld gPair;
static std::vector<uint32_t> gPairCloses;
static PhonePairView phonePipePairView(char code[20]) {
  const bool shown = gPair.view.state == PhonePairWindow::Open && gPair.view.onDisplay;
  if (code) { if (shown) std::memcpy(code, gPair.code, 20); else std::memset(code, 0, 20); }
  PhonePairView v = gPair.view;
  if (v.state != PhonePairWindow::Open) v = PhonePairView{v.state};
  return v;
}
static void phonePipePairClose(uint32_t id) {
  gPairCloses.push_back(id);
  if (id == gPair.view.id && gPair.view.onDisplay) { std::memset(gPair.code, 0, 20); gPair.view.onDisplay = false; }
}
#endif

// System_Settings.h
struct Settings {
  String bleDeviceName; int bleTxPower = 3; int bleMode = 0; bool bleAutoStart = false;
  bool bleConsoleOnPipe = true; String phonePipeSecret; bool phonePipeSecretGenerated = false;
};
static Settings gSettings;

// ArduinoJson.h: bleLinkStatusJson and bleLinkPhoneJson compile; other tests run their keys.
struct JsonObject {
  struct Slot {
    template <class T> Slot& operator=(const T&) { return *this; }
    template <class T> T to() { return T(); }
  };
  bool isNull() const { return true; }
  Slot operator[](const char*) { return Slot(); }
};

// ---- The production code ---------------------------------------------------------
#include "OLED_Mode_Bluetooth.cpp"
#include "BLE_LinkStatus.cpp"

// ---- Helpers -----------------------------------------------------------------------
using Rows = std::vector<std::string>;

static const OLEDModeEntry& modeEntry(OLEDMode mode) {
  for (size_t i = 0; i < kRegisteredModeCount; ++i)
    if (kRegisteredModes[i].mode == mode) return kRegisteredModes[i];
  std::fprintf(stderr, "mode %d is not registered\n", int(mode));
  std::exit(1);
}

// Draws a mode and returns its content rows, trailing blanks and the scroll
// arrows trimmed. Nothing may wrap past 21 columns or reach the footer.
static Rows render(OLEDMode mode) {
  gScreen.clear();
  modeEntry(mode).displayFunc();
  CHECK(!gScreen.wrapped);
  CHECK(!gScreen.intoFooter);
  Rows out;
  for (const auto& kv : gScreen.rows) {
    CHECK(kv.first >= OLED_CONTENT_START_Y && (kv.first - OLED_CONTENT_START_Y) % 8 == 0);
    const size_t r = size_t(kv.first - OLED_CONTENT_START_Y) / 8;
    if (out.size() <= r) out.resize(r + 1);
    std::string text = kv.second;
    for (char& c : text) if (c == '\x18' || c == '\x19') c = ' ';
    while (!text.empty() && text.back() == ' ') text.pop_back();
    CHECK(text.size() <= 21);
    out[r] = text;
  }
  return out;
}

static void expectRows(const Rows& got, const Rows& want, int line) {
  bool same = got.size() >= want.size();
  for (size_t i = 0; same && i < want.size(); ++i) same = got[i] == want[i];
  if (same) { ++checks; return; }
  std::fprintf(stderr, "line %d: rows differ\n  got:", line);
  for (const auto& r : got) std::fprintf(stderr, " [%s]", r.c_str());
  std::fprintf(stderr, "\n  want:");
  for (const auto& r : want) std::fprintf(stderr, " [%s]", r.c_str());
  std::fprintf(stderr, "\n");
  std::exit(1);
}
#define EXPECT_ROWS(got, ...) expectRows((got), Rows{__VA_ARGS__}, __LINE__)
#define EXPECT_EXACT(got, ...) do { const Rows want_{__VA_ARGS__}; const Rows got_ = (got); \
  CHECK(got_.size() == want_.size()); expectRows(got_, want_, __LINE__); } while (0)

static bool anyRowStarts(const Rows& rows, const char* prefix) {
  for (const auto& r : rows) if (r.compare(0, std::strlen(prefix), prefix) == 0) return true;
  return false;
}

static BLESystemState gState;

static void reset() {
  w = World();
  gBLEState = nullptr;
  gSettings = Settings();
  gCommands.clear();
  gToast.clear();
  gConfirm = Confirm();
  gToasts.clear();
  gAsyncCommands.clear();
  currentOLEDMode = OLED_BLUETOOTH;
#if ENABLE_BLE_PHONE_PIPE
  gPair = PairWorld();
  gPairCloses.clear();
  gDisplayEpoch = 21;
#endif
}

// The console server up, one signed-in app, no advertising.
static void hostConsole() {
  w.server = true;
  w.console.up = true;
  w.console.clients = 1;
  w.console.signedIn = true;
  gState = BLESystemState{};
  gState.initialized = true;
  gState.connectionState = BLE_STATE_CONNECTED;
  gState.connections[0].active = true;
  gState.connections[0].connectedSince = gMillis - 5000;
  gState.connections[0].deviceName = String("Phone");
  gState.activeConnectionCount = 1;
  gState.totalConnections = 3;
  gState.commandsReceived = 12;
  gState.responsesSent = 30;
  gState.streamFlags = BLE_STREAM_SENSORS;
  gState.sensorStreamInterval = 1000;
  gState.systemStreamInterval = 5000;
  gBLEState = &gState;
}

// hw1link::PipeStep values, as BLE_PhonePipe.cpp's phonePipeStep() reports them.
static uint8_t stepOf(hw1link::PipeStep s) { return uint8_t(s); }

static void runPipe(hw1link::PipeStep step, const char* label, bool ready = false) {
  w.pipeActive = true;
  w.step = stepOf(step);
  w.label = label;
  w.pipeReady = ready;
}

// The app console registered on the glasses' host (bleconsole on, blesecret
// set), with a slot on the pipe's link when attached: never the server.
static void phoneLinkConsole(bool attached, bool signedIn) {
  w.console = BleConsoleView();
  w.console.registered = w.console.attachable = w.console.secret = true;
  w.console.attached = attached;
  w.console.clients = attached ? 1 : 0;
  w.console.signedIn = attached && signedIn;
}

// ---- Bluetooth status screen ------------------------------------------------------
static void statusOff() {
  reset();
  gSettings.bleMode = 0;
  EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS),
               "Role: Off", "Saved: App console", "", "Select a role in", "the Bluetooth menu");
  gSettings.bleMode = 1;
  EXPECT_ROWS(render(OLED_BLUETOOTH_STATUS), "Role: Off",
              ENABLE_BLE_PHONE_PIPE ? "Saved: Glasses+pipe" : "Saved: Glasses");
}

static void statusCentral() {
  using hw1link::PipeStep;
#if ENABLE_G2_GLASSES
  reset();
  gSettings.bleMode = 1;
  w.glassesClient = true;
  w.glasses = true;
#if ENABLE_BLE_PHONE_PIPE
  w.paired = true;
  runPipe(PipeStep::Link, "ready", true);
  Rows rows = render(OLED_BLUETOOTH_STATUS);
  EXPECT_EXACT(rows, "Role: Glasses+pipe", "Glasses: connected", "Ring: --", "Pipe: ready",
               "Console: n/a here");
  CHECK(!anyRowStarts(rows, "State:") && !anyRowStarts(rows, "Phone:") && !anyRowStarts(rows, "Mode:"));
  // The app console on the pipe's link: the Console row is its own words, and
  // no server Clients/Adv row.
  phoneLinkConsole(true, true);
  rows = render(OLED_BLUETOOTH_STATUS);
  EXPECT_EXACT(rows, "Role: Glasses+pipe", "Glasses: connected", "Ring: --", "Pipe: ready",
               "Console: signed in");
  CHECK(!anyRowStarts(rows, "Clients:"));
  w.console.signedIn = false;
  EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: Glasses+pipe", "Glasses: connected", "Ring: --",
               "Pipe: ready", "Console: signed out");
  phoneLinkConsole(false, false);
  EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: Glasses+pipe", "Glasses: connected", "Ring: --",
               "Pipe: ready", "Console: waiting");
  w.console.secret = false;
  EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: Glasses+pipe", "Glasses: connected", "Ring: --",
               "Pipe: ready", "Console: no secret");
  w.console.secret = true;
  w.console.attachable = false;  // bleconsole off
  EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: Glasses+pipe", "Glasses: connected", "Ring: --",
               "Pipe: ready", "Console: off");
  w.console = BleConsoleView();
  // Saved as the console: six rows on a 64 px OLED, so the glasses and ring share one.
  gSettings.bleMode = 0;
  if (kTall) {
    EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: Glasses+pipe", "Saved: App console",
                 "Glasses: connected", "Ring: --", "Pipe: ready", "Console: n/a here");
  } else {
    EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: Glasses+pipe", "Saved: App console",
                 "Glasses: ok Ring: --", "Pipe: ready", "Console: n/a here");
  }
  // The glasses alone while the pipe build is in: same saved mode, the pipe off.
  gSettings.bleMode = 1;
  w.pipeActive = w.pipeReady = false;
  w.label = "stopped";
  w.ring = true;
  EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: Glasses", "Glasses: connected", "Ring: connected",
               "Pipe: off", "Console: n/a here");
#else
  w.ring = true;
  EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: Glasses", "Glasses: connected", "Ring: connected",
               "Console: n/a here");
#endif
#endif
#if ENABLE_BLE_PHONE_PIPE
  // The phone pipe alone: the client stack with the glasses closed, which is
  // the saved client mode too (no Saved row).
  reset();
  gSettings.bleMode = 1;
  w.paired = true;
  runPipe(PipeStep::Scanning, "scanning");
  EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: Phone pipe", "Pipe: searching", "Console: n/a here");
  runPipe(PipeStep::Idle, "phone not found");
  EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: Phone pipe", "Pipe: fail: not found",
               "Console: n/a here");
  // `openphone` opened the pipe while the saved mode is the console.
  gSettings.bleMode = 0;
  EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: Phone pipe", "Saved: App console",
               "Pipe: fail: not found", "Console: n/a here");
  // A fault names no reason on the line (JSON keeps it).
  runPipe(PipeStep::Fault, "phone pipe enrollment forgotten");
  EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: Phone pipe", "Saved: App console",
               "Pipe: fault", "Console: n/a here");
  // A 9-character reason fills the 21 columns exactly, never wrapped.
  runPipe(PipeStep::Idle, "phone pipe enrollment forgotten");
  EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: Phone pipe", "Saved: App console",
               "Pipe: fail: forgotten", "Console: n/a here");
#endif
}

static void statusConsole() {
  reset();
  hostConsole();
  w.recent[0] = "status";
  w.recent[1] = "help";
  w.recentCount = 2;
  const Rows rows = render(OLED_BLUETOOTH_STATUS);
  const Rows head = ENABLE_BLE_PHONE_PIPE
      ? Rows{"Role: App console", "Console: signed in", "Clients: 1/4 Adv: off", "Pipe: off",
             "Name: HardwareOne", "Rx:12 Tx:30"}
      : Rows{"Role: App console", "Console: signed in", "Clients: 1/4 Adv: off",
             "Name: HardwareOne", "Rx:12 Tx:30"};
  if (kTall) {
    Rows all = head;
    for (const char* r : {"Peers:", "#0 Phone 5s", "Streams: S sys ev", "Int:1000/5000", "", "Last:",
                          "status", "help"})
      all.push_back(r);
    all.resize(13);  // the P4 canvas holds 13 rows: the rest is not drawn
    EXPECT_EXACT(rows, all[0], all[1], all[2], all[3], all[4], all[5], all[6], all[7], all[8], all[9],
                 all[10], all[11], all[12]);
  } else {
    const Rows five(head.begin(), head.begin() + 5);  // the detail stops at the footer
    EXPECT_EXACT(rows, five[0], five[1], five[2], five[3], five[4]);
  }
  CHECK(!anyRowStarts(rows, "State:"));

  // No app yet, advertising; the idle detail.
  w.console.clients = 0;
  w.console.signedIn = false;
  w.console.advertising = true;
  gState.connectionState = BLE_STATE_ADVERTISING;
  gState.connections[0].active = false;
  EXPECT_ROWS(render(OLED_BLUETOOTH_STATUS), "Role: App console", "Console: waiting", "Clients: 0/4 Adv: on");
  if (kTall) {
    const Rows idle = render(OLED_BLUETOOTH_STATUS);
    const size_t at = ENABLE_BLE_PHONE_PIPE ? 4 : 3;
    CHECK(idle[at] == "Name: HardwareOne" && idle[at + 1] == "TX Power: 3" && idle[at + 2] == "Total: 3");
  }
  w.console.advertising = false;
  EXPECT_ROWS(render(OLED_BLUETOOTH_STATUS), "Role: App console", "Console: hidden", "Clients: 0/4 Adv: off");

  // A long device name is cut with a tilde.
  gSettings.bleDeviceName = String("HardwareOne-Kitchen-01");
  CHECK(render(OLED_BLUETOOTH_STATUS)[ENABLE_BLE_PHONE_PIPE ? 4 : 3] == "Name: HardwareOne~");

  // Saved as the glasses: the Saved row follows the role.
  gSettings.bleMode = 1;
  EXPECT_ROWS(render(OLED_BLUETOOTH_STATUS), "Role: App console",
              ENABLE_BLE_PHONE_PIPE ? "Saved: Glasses+pipe" : "Saved: Glasses", "Console: hidden");

  // Freed since the snapshot: the axes rows only, gBLEState never read.
  gSettings.bleMode = 0;
  w.console.clients = 1;
  w.console.signedIn = true;
  gBLEState = nullptr;
  if (ENABLE_BLE_PHONE_PIPE) {
    EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: App console", "Console: signed in",
                 "Clients: 1/4 Adv: off", "Pipe: off");
  } else {
    EXPECT_EXACT(render(OLED_BLUETOOTH_STATUS), "Role: App console", "Console: signed in",
                 "Clients: 1/4 Adv: off");
  }
#if ENABLE_BLE_PHONE_PIPE
  // A pipe still closing after the switch to the console shows its own words.
  runPipe(hw1link::PipeStep::Retiring, "stopped");
  EXPECT_ROWS(render(OLED_BLUETOOTH_STATUS), "Role: App console", "Console: signed in",
              "Clients: 1/4 Adv: off", "Pipe: closing");
#endif
}

// ---- Bluetooth main menu --------------------------------------------------------------
static int menuIndex(const char* label) {
  for (int i = 0; i < sBtMenuScroll.itemCount; ++i)
    if (std::strcmp(sBtMenuScroll.items[i].line1, label) == 0) return i;
  return -1;
}

// Selects a row and returns it as drawn.
static std::string menuRow(const char* label) {
  render(OLED_BLUETOOTH);  // builds the rows
  const int idx = menuIndex(label);
  if (idx < 0) return "";
  sBtMenuScroll.selectedIndex = idx;
  const Rows rows = render(OLED_BLUETOOTH);
  return rows.at(size_t(idx - sBtMenuScroll.scrollOffset));
}

static void mainMenu() {
  reset();
  render(OLED_BLUETOOTH);
  // The glasses' order: on/off first, then Status; Settings last.
  CHECK(menuIndex("Bluetooth: OFF") == 0 && menuIndex("Status") == 1);
  CHECK(menuIndex("Advertising") < 0 && menuIndex("Disconnect") < 0);
  CHECK((menuIndex("Phone >>") >= 0) == bool(ENABLE_BLE_PHONE_PIPE));
  // One row per role in the build where Start/Stop was.
  CHECK(menuIndex("Start/Stop") < 0);
  CHECK(menuIndex("App console") == 2 && menuIndex("Settings") == sBtMenuScroll.itemCount - 1);
  // Boot role and Auto Start sit above Settings, as on the glasses' page.
  CHECK(menuIndex("Boot: App console") >= 0 && menuIndex("Auto Start: OFF") >= 0);
  // Off: the first row opens the client stack, with no confirm.
  gCommands.clear();
  sBtMenuScroll.selectedIndex = 0;
  CHECK(modeEntry(OLED_BLUETOOTH).inputFunc(0, 0, INPUT_BUTTON_A));
  CHECK(!gConfirm.open && gCommands.size() == 1 && gCommands.back() == "openble");
  // Auto Start sends the opposite of the setting; the boot role toggles the
  // saved mode only, and says when it applies.
  gCommands.clear();
  sBtMenuScroll.selectedIndex = menuIndex("Auto Start: OFF");
  CHECK(modeEntry(OLED_BLUETOOTH).inputFunc(0, 0, INPUT_BUTTON_A));
  CHECK(gCommands.size() == 1 && gCommands.back() == "bleautostart on");
  gSettings.bleAutoStart = true;
  render(OLED_BLUETOOTH);
  CHECK(menuIndex("Auto Start: ON") >= 0);
  gCommands.clear();
  sBtMenuScroll.selectedIndex = menuIndex("Boot: App console");
  CHECK(modeEntry(OLED_BLUETOOTH).inputFunc(0, 0, INPUT_BUTTON_A));
  CHECK(gCommands.size() == 1 && gCommands.back() == "blemode client");
  CHECK(gToast == "Applies at next boot");
  // On: the first row stops all of Bluetooth only after a confirm (default No).
  w.server = true;
  render(OLED_BLUETOOTH);
  CHECK(menuIndex("Bluetooth: ON") == 0);
  gCommands.clear();
  sBtMenuScroll.selectedIndex = 0;
  CHECK(modeEntry(OLED_BLUETOOTH).inputFunc(0, 0, INPUT_BUTTON_A));
  CHECK(gConfirm.open && !gConfirm.defaultYes && gCommands.empty());
  confirmYes();
  CHECK(gCommands.size() == 1 && gCommands.back() == "closeble");
  w.server = false;
  gSettings.bleAutoStart = false;
  render(OLED_BLUETOOTH);
  CHECK(menuIndex("Glasses") == (ENABLE_G2_GLASSES ? 3 : -1));
  CHECK(menuIndex("Phone pipe") == (ENABLE_BLE_PHONE_PIPE ? 3 + ENABLE_G2_GLASSES : -1));
  // The console's rows come from bleConsoleView(): gBLEState stays freed.
  w.server = true;
  w.console.up = true;
  w.console.advertising = true;
  render(OLED_BLUETOOTH);
  CHECK(menuIndex("Advertising") >= 0 && menuIndex("Disconnect") < 0);
  CHECK(menuRow("Advertising") == "> Advertising *");
  w.console.advertising = false;
  w.console.clients = 1;
  CHECK(menuRow("Advertising") == "> Advertising");
  CHECK(menuIndex("Disconnect") >= 0);
  sBtMenuScroll.selectedIndex = menuIndex("Disconnect");
  CHECK(modeEntry(OLED_BLUETOOTH).inputFunc(0, 0, INPUT_BUTTON_A));
  CHECK(!gCommands.empty() && gCommands.back() == "bledisconnect");
  sBtMenuScroll.selectedIndex = menuIndex("Advertising");
  CHECK(modeEntry(OLED_BLUETOOTH).inputFunc(0, 0, INPUT_BUTTON_A));
  CHECK(gCommands.back() == "bleadv toggle");
#if ENABLE_BLE_PHONE_PIPE
  // The Phone >> marker stays: the pipe is ready.
  reset();
  w.paired = true;
  runPipe(hw1link::PipeStep::Link, "ready", true);
  CHECK(menuRow("Phone >>") == "> Phone >> *");
  w.pipeReady = false;
  CHECK(menuRow("Phone >>") == "> Phone >>");
#if ENABLE_G2_GLASSES
  // The app console on the pipe's link beside the glasses: no Advertising, and
  // its row ends the console session (bledisconnect), never the phone link.
  reset();
  gSettings.bleMode = 1;
  w.glassesClient = w.glasses = true;
  w.paired = true;
  runPipe(hw1link::PipeStep::Link, "ready", true);
  phoneLinkConsole(true, true);
  render(OLED_BLUETOOTH);
  CHECK(menuIndex("Advertising") < 0 && menuIndex("Disconnect") < 0 && menuIndex("End console") >= 0);
  gCommands.clear();
  sBtMenuScroll.selectedIndex = menuIndex("End console");
  CHECK(modeEntry(OLED_BLUETOOTH).inputFunc(0, 0, INPUT_BUTTON_A));
  CHECK(gCommands.size() == 1 && gCommands.back() == "bledisconnect");
  phoneLinkConsole(false, false);  // waiting: nothing to end
  render(OLED_BLUETOOTH);
  CHECK(menuIndex("End console") < 0 && menuIndex("Disconnect") < 0);
#endif
#endif
  sBtMenuScroll.selectedIndex = 0;
}

#if ENABLE_G2_GLASSES
// The G2 and R1 submenus: the glasses' rows (AutoReconnect, Reconnect) in a
// scrolling list under a status line, so render() finds nothing under the
// footer; each row sends the command the glasses' row sends.
static int rowIndex(const OLEDScrollState& st, const char* label) {
  for (int i = 0; i < st.itemCount; ++i)
    if (std::strcmp(st.items[i].line1, label) == 0) return i;
  return -1;
}

static void peerMenus() {
  reset();
  render(OLED_BLUETOOTH_G2);
  CHECK(rowIndex(sG2MenuScroll, "Connect") == 0 && rowIndex(sG2MenuScroll, "Reconnect") < 0);
  CHECK(rowIndex(sG2MenuScroll, "AutoReconnect: OFF") >= 0);
  CHECK(rowIndex(sG2MenuScroll, "Nav Mode: OFF") >= 0);
  gCommands.clear();
  sG2MenuScroll.selectedIndex = rowIndex(sG2MenuScroll, "AutoReconnect: OFF");
  CHECK(modeEntry(OLED_BLUETOOTH_G2).inputFunc(0, 0, INPUT_BUTTON_A));
  CHECK(gCommands.size() == 1 && gCommands.back() == "bleautoreconnect g2-glasses on");
  w.glassesClient = w.glasses = true;
  render(OLED_BLUETOOTH_G2);
  CHECK(rowIndex(sG2MenuScroll, "Disconnect") == 0 && rowIndex(sG2MenuScroll, "Reconnect") == 1);
  CHECK(rowIndex(sG2MenuScroll, "Show Text") >= 0);
  gCommands.clear();
  sG2MenuScroll.selectedIndex = 1;
  CHECK(modeEntry(OLED_BLUETOOTH_G2).inputFunc(0, 0, INPUT_BUTTON_A));
  CHECK(gCommands.size() == 1 && gCommands.back() == "openg2 saved");

  reset();
  render(OLED_BLUETOOTH_R1);
  CHECK(rowIndex(sR1MenuScroll, "Connect") == 0);
  CHECK(rowIndex(sR1MenuScroll, "AutoReconnect: OFF") == 1);   // was under the footer
  gCommands.clear();
  sR1MenuScroll.selectedIndex = 1;
  CHECK(modeEntry(OLED_BLUETOOTH_R1).inputFunc(0, 0, INPUT_BUTTON_A));
  CHECK(gCommands.size() == 1 && gCommands.back() == "bleautoreconnect r1-ring on");
  w.ring = true;
  render(OLED_BLUETOOTH_R1);
  CHECK(rowIndex(sR1MenuScroll, "Reconnect") == 0 && rowIndex(sR1MenuScroll, "Disconnect") == 1);
  gCommands.clear();
  sR1MenuScroll.selectedIndex = 0;
  CHECK(modeEntry(OLED_BLUETOOTH_R1).inputFunc(0, 0, INPUT_BUTTON_A));
  CHECK(gCommands.size() == 1 && gCommands.back() == "openring reconnect");
  reset();
}
#endif

// ---- Role rows ---------------------------------------------------------------------------
// Taps a role row as last drawn.
static void tapRole(const char* label) {
  const int idx = menuIndex(label);
  CHECK(idx >= 0);
  sBtMenuScroll.selectedIndex = idx;
  CHECK(modeEntry(OLED_BLUETOOTH).inputFunc(0, 0, INPUT_BUTTON_A));
}

// Draws the menu, taps a role row: the confirm it opens runs nothing until Yes,
// then exactly its command. Confirm lines fit the 19-column confirm box.
static void expectRoleTap(const char* label, const char* ask, const char* note, bool defaultYes,
                          const char* cmd, int line) {
  render(OLED_BLUETOOTH);
  gCommands.clear();
  tapRole(label);
  const bool asked = gConfirm.open && gConfirm.line1 == ask && gConfirm.line2 == note &&
                     gConfirm.defaultYes == defaultYes && gCommands.empty();
  if (!asked) {
    std::fprintf(stderr, "line %d: %s opened [%s] [%s] default %s, %zu command(s)\n", line, label,
                 gConfirm.line1.c_str(), gConfirm.line2.c_str(), gConfirm.defaultYes ? "Yes" : "No",
                 gCommands.size());
    std::exit(1);
  }
  CHECK(gConfirm.line1.size() <= 19 && gConfirm.line2.size() <= 19);
  confirmYes();
  if (gCommands.size() == 1 && gCommands[0] == cmd) { ++checks; return; }
  std::fprintf(stderr, "line %d: %s ran [%s], wanted [%s]\n", line, label,
               gCommands.empty() ? "" : gCommands.back().c_str(), cmd);
  std::exit(1);
}
#define EXPECT_ROLE_TAP(label, ask, note, defaultYes, cmd) \
  expectRoleTap((label), (ask), (note), (defaultYes), (cmd), __LINE__)
// The server confirm names only the roles this build has.
#if ENABLE_G2_GLASSES && ENABLE_BLE_PHONE_PIPE
static const char* const kServerStops = "Stops glasses/pipe";
#elif ENABLE_G2_GLASSES
static const char* const kServerStops = "Stops the glasses";
#elif ENABLE_BLE_PHONE_PIPE
static const char* const kServerStops = "Stops the pipe";
#endif

static void roleRows() {
  using hw1link::PipeStep;
  // Off: nothing marked; each row opens its role.
  reset();
  CHECK(menuRow("App console") == "> App console");
  if (ENABLE_G2_GLASSES) CHECK(menuRow("Glasses") == "> Glasses");
  if (ENABLE_BLE_PHONE_PIPE) CHECK(menuRow("Phone pipe") == "> Phone pipe");
  EXPECT_ROLE_TAP("App console", "Switch to server?", "", true, "openconsole");
#if ENABLE_G2_GLASSES
  EXPECT_ROLE_TAP("Glasses", "Open glasses?", "", true, "openg2");
#endif
#if ENABLE_BLE_PHONE_PIPE
  EXPECT_ROLE_TAP("Phone pipe", "Open phone pipe?", "", true, "openphone");
#endif

  // The app console runs: its row is marked and stops it; the others switch.
  reset();
  hostConsole();
  gBLEState = nullptr;  // the rows read bleLinkStatus(), never gBLEState
  CHECK(menuRow("App console") == "> App console *");
  EXPECT_ROLE_TAP("App console", "Stop app console?", "", false, "closeconsole");
#if ENABLE_G2_GLASSES
  CHECK(menuRow("Glasses") == "> Glasses");
  EXPECT_ROLE_TAP("Glasses", "Open glasses?", "", false, "openg2");
#endif
#if ENABLE_BLE_PHONE_PIPE
  CHECK(menuRow("Phone pipe") == "> Phone pipe");
  EXPECT_ROLE_TAP("Phone pipe", "Open phone pipe?", "", false, "openphone");
#endif

#if ENABLE_G2_GLASSES
  // The glasses run: the app console is one tap away, as a week ago.
  reset();
  gSettings.bleMode = 1;
  w.glassesClient = true;
  w.glasses = true;
  CHECK(menuRow("Glasses") == "> Glasses *");
  CHECK(menuRow("App console") == "> App console");
  EXPECT_ROLE_TAP("App console", "Switch to server?", kServerStops, false, "openconsole");
  EXPECT_ROLE_TAP("Glasses", "Close glasses?", "", false, "closeg2");
#if ENABLE_BLE_PHONE_PIPE
  // Glasses+pipe marks Glasses. No Phone pipe row beside the glasses: there
  // `openphone` only adds the pipe (Phone >> Start Pipe).
  CHECK(menuIndex("Phone pipe") < 0);
  w.paired = true;
  runPipe(PipeStep::Link, "ready", true);
  CHECK(menuRow("Glasses") == "> Glasses *");
  CHECK(menuIndex("Phone pipe") < 0);
  EXPECT_ROLE_TAP("App console", "Switch to server?", kServerStops, false, "openconsole");
  EXPECT_ROLE_TAP("Glasses", "Close glasses?", "", false, "closeg2");
#endif
#endif

#if ENABLE_BLE_PHONE_PIPE
  // The phone-only pipe runs, even while the saved mode is the console.
  reset();
  w.paired = true;
  runPipe(PipeStep::Scanning, "scanning");
  CHECK(menuRow("Phone pipe") == "> Phone pipe *");
  CHECK(menuRow("App console") == "> App console");
  EXPECT_ROLE_TAP("Phone pipe", "Close phone pipe?", "", false, "closephone");
  EXPECT_ROLE_TAP("App console", "Switch to server?", kServerStops, false, "openconsole");
#if ENABLE_G2_GLASSES
  CHECK(menuRow("Glasses") == "> Glasses");
  EXPECT_ROLE_TAP("Glasses", "Open glasses?", "", false, "openg2");
#endif
#endif

  // A tap does what its row said, even if the role changed since the frame.
  reset();
  render(OLED_BLUETOOTH);
  w.server = true;  // the console came up elsewhere; not drawn yet
  w.console.up = true;
  tapRole("App console");
  CHECK(gConfirm.line1 == "Switch to server?");
  // A second confirm is refused while one is open: the open one keeps its command.
  render(OLED_BLUETOOTH);  // now drawn as running
  tapRole("App console");
  CHECK(gConfirm.line1 == "Switch to server?");
  confirmYes();
  CHECK(gCommands.size() == 1 && gCommands[0] == "openconsole");
  sBtMenuScroll.selectedIndex = 0;
}

// ---- Phone screen ---------------------------------------------------------------------
#if ENABLE_BLE_PHONE_PIPE
static int phoneIndex(PhoneAction act) {
  for (int i = 0; i < sPhoneScroll.itemCount; ++i) if (sPhoneActions[i] == act) return i;
  return -1;
}

static void tapPhone(PhoneAction act) {
  sPhoneScroll.selectedIndex = phoneIndex(act);
  CHECK(sPhoneScroll.selectedIndex >= 0);
  CHECK(modeEntry(OLED_BLUETOOTH_PHONE).inputFunc(0, 0, INPUT_BUTTON_A));
}

static void phoneScreen() {
  using hw1link::PipeStep;
  reset();
  gSettings.bleMode = 1;  // the pipe alone: the client stack with the glasses closed
  w.paired = true;
  w.autoReconnect = true;
  runPipe(PipeStep::Link, "ready", true);
  const Rows rows = render(OLED_BLUETOOTH_PHONE);
  if (kTall) {
    EXPECT_EXACT(rows, "PIPE [OK] paired", "ready", "Console: n/a here", "> Close Phone *", "  AutoRecon ON",
                 "- Pipe settings -", "  Pair Phone", "  New code and pair", "  Forget Phone");
  } else {
    // Three status rows leave two menu rows on a 64 px OLED.
    EXPECT_EXACT(rows, "PIPE [OK] paired", "ready", "Console: n/a here", "> Close Phone *", "  AutoRecon ON");
  }

  runPipe(PipeStep::Scanning, "scanning");
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [..] paired", "searching", "Console: n/a here",
              "> Close Phone");
  runPipe(PipeStep::Waiting, "waiting for radio");
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [..] paired", "waiting");
  runPipe(PipeStep::Link, "authenticating pipe");
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [..] paired", "linking");
  runPipe(PipeStep::Retiring, "stopped");
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [..] paired", "closing");
  runPipe(PipeStep::Idle, "phone not found");
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [!!] paired", "fail: not found");
  runPipe(PipeStep::Fault, "phone cleanup incomplete");
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [!!] paired", "fault");
  runPipe(PipeStep::Idle, "disconnected");
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [--] paired", "idle");

  // Not paired, then a pairing window.
  w.paired = false;
  runPipe(PipeStep::Idle, "waiting to enroll");
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [!!] not paired", "not paired");
  w.enrolling = true;
  runPipe(PipeStep::Scanning, "scanning");
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [..] not paired", "pairing");
  w.enrolling = false;

  // Stopped, and stopped for want of a secret.
  w.pipeActive = false;
  w.label = "configure phone secret first";
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [!!] not paired", "no secret", "Console: off");
  w.paired = true;
  w.label = "stopped";
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [off] paired", "off", "Console: off");
  w.paired = false;
  CHECK(render(OLED_BLUETOOTH_PHONE)[0] == "PIPE [off] not paired");  // 21 columns exactly

  // The console runs instead: the Phone screen names it.
  reset();
  hostConsole();
  w.paired = true;
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [off] paired", "off", "Console: signed in");
  w.console.clients = 0;
  w.console.signedIn = false;
  w.console.advertising = true;
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [off] paired", "off", "Console: waiting");
  w.console.clients = 2;
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [off] paired", "off", "Console: signed out");

#if ENABLE_G2_GLASSES
  // Beside the glasses the console rides the pipe's own link: signed in while
  // the pipe is ready; once the link drops, a slot it left is not shown.
  reset();
  gSettings.bleMode = 1;
  w.glassesClient = w.glasses = true;
  w.paired = true;
  runPipe(PipeStep::Link, "ready", true);
  phoneLinkConsole(true, true);
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [OK] paired", "ready", "Console: signed in");
  runPipe(PipeStep::Scanning, "scanning");
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [..] paired", "searching", "Console: waiting");
  w.console.secret = false;
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE), "PIPE [..] paired", "searching", "Console: no secret");
#endif

  // A tap does what its row said, even if the pipe changed since the frame.
  reset();
  w.paired = true;
  w.autoReconnect = true;
  runPipe(PipeStep::Idle, "disconnected");
  render(OLED_BLUETOOTH_PHONE);
  CHECK(std::strcmp(sPhoneScroll.items[phoneIndex(PH_ACT_PIPE)].line1, "Close Phone") == 0);
  w.pipeActive = false;  // stopped elsewhere; not drawn yet
  tapPhone(PH_ACT_PIPE);
  CHECK(gCommands.size() == 1 && gCommands.back() == "closephone");
  render(OLED_BLUETOOTH_PHONE);
  CHECK(std::strcmp(sPhoneScroll.items[phoneIndex(PH_ACT_PIPE)].line1, "Open Phone") == 0);
  runPipe(PipeStep::Scanning, "scanning");  // opened elsewhere; not drawn yet
  tapPhone(PH_ACT_PIPE);  // opens the pipe and connects the paired phone
  CHECK(gCommands.size() == 2 && gCommands.back() == "openphone" && gToast == "Connecting phone");
  tapPhone(PH_ACT_AUTO);
  CHECK(gCommands.back() == "bleautoreconnect phone off");
  const size_t before = gCommands.size();
  w.paired = false;
  w.pipeActive = false;
  w.label = "stopped";
  render(OLED_BLUETOOTH_PHONE);
  tapPhone(PH_ACT_PIPE);  // nothing paired: the pipe opens for pairing
  CHECK(gCommands.size() == before + 1 && gCommands.back() == "openphone" &&
        gToast == "Phone open; pair it");
  tapPhone(PH_ACT_FORGET);
  CHECK(gToast == "No phone paired");
  w.paired = true;
  runPipe(PipeStep::Link, "ready", true);
  render(OLED_BLUETOOTH_PHONE);
  tapPhone(PH_ACT_PIPE);
  CHECK(gCommands.back() == "closephone");
  tapPhone(PH_ACT_PAIR);
  CHECK(gToast == "Disconnect first");
  sPhoneScroll.selectedIndex = 0;
}
#endif

// ---- Link watcher -------------------------------------------------------------------
static void tickExpect(int wanted, int line) {
  const int before = gMarks;
  oledBluetoothLinkWatch();
  if (gMarks - before == wanted) { ++checks; return; }
  std::fprintf(stderr, "line %d: %d marks, wanted %d\n", line, gMarks - before, wanted);
  std::exit(1);
}
#define TICK(n) tickExpect((n), __LINE__)

static void linkWatch() {
  using hw1link::PipeStep;
  reset();
  TICK(0);  // the first tick records the starting state
  TICK(0);
  // The app console: role, its state words, the client count and advertising.
  w.server = true;
  w.console.up = true;
  TICK(1);
  TICK(0);
  w.console.advertising = true; TICK(1);   // hidden -> waiting
  w.console.clients = 1;        TICK(1);   // connected
  w.console.signedIn = true;    TICK(1);   // signed in
  w.console.clients = 2;        TICK(1);   // Clients: 2/4
  w.console.advertising = false; TICK(1);  // Adv: off, still signed in
  gSettings.bleMode = 1;        TICK(1);   // the saved mode (Saved: row)
  TICK(0);
  w.server = false;
  w.console = BleConsoleView();
  TICK(1);  // one tick, one mark
#if ENABLE_G2_GLASSES
  w.glassesClient = true; TICK(1);  // role Glasses
  CHECK(menuRow("Glasses") == "> Glasses *" && menuRow("App console") == "> App console");
  w.glasses = true;       TICK(1);
  w.ring = true;          TICK(1);
#endif
#if ENABLE_BLE_PHONE_PIPE
  w.paired = true;
  runPipe(PipeStep::Waiting, "waiting for radio"); TICK(1);  // role and phase
  runPipe(PipeStep::Scanning, "scanning");         TICK(1);
  runPipe(PipeStep::Link, "ready", true);          TICK(1);
  TICK(0);
#if ENABLE_G2_GLASSES
  // The app console on the pipe's link: registered, attached, signed in, gone.
  phoneLinkConsole(false, false); TICK(1);  // waiting for the phone link
  phoneLinkConsole(true, false);  TICK(1);  // connected
  w.console.signedIn = true;      TICK(1);  // signed in
  TICK(0);
  w.console = BleConsoleView();   TICK(1);
#endif
  runPipe(PipeStep::Idle, "phone not found");      TICK(1);  // fail: not found
  runPipe(PipeStep::Idle, "BLE setup timed out");  TICK(1);  // same phase, new reason
  w.autoReconnect = true; TICK(1);
  w.paired = false;       TICK(1);  // PIPE [!!] not paired
  w.pipeActive = false;
  w.label = "stopped";
  TICK(1);
#endif
#if ENABLE_G2_GLASSES
  w.ring = false; TICK(1);
  w.glassesClient = w.glasses = false; TICK(1);
#endif
  TICK(0);
}


// ---- `phone pair new`: the menu entry, the code screen, the window's watch ----------
#if ENABLE_BLE_PHONE_PIPE
static const char kShownCode[] = "7K2M-QX4P-9TVW-H3RA";
static bool carriesShownCode(const std::string& s) {
  for (const char* group : {"7K2M", "QX4P", "9TVW", "H3RA"}) if (s.find(group) != std::string::npos) return true;
  return false;
}
// A serial window carries the serial session (any live-looking epoch); an OLED
// one the live display session.
static void openPairWindow(uint32_t id, uint32_t ageMs, bool onDisplay) {
  gPair.view = PhonePairView{PhonePairWindow::Open, id, 180000 - ageMs, ageMs, onDisplay,
                             onDisplay ? SOURCE_LOCAL_DISPLAY : SOURCE_SERIAL, onDisplay ? gDisplayEpoch : 5};
  std::memcpy(gPair.code, kShownCode, sizeof(kShownCode));
}

static void pairCodeScreen() {
  using hw1link::PipeStep;
  // Phone >> New code and pair asks first (default No), then runs the command
  // off the display task; it is offered while the link is up too.
  reset();
  gSettings.bleMode = 1;
  w.paired = true;
  runPipe(PipeStep::Link, "ready", true);
  render(OLED_BLUETOOTH_PHONE);
  CHECK(std::strcmp(sPhoneScroll.items[phoneIndex(PH_ACT_PAIR_NEW)].line1, "New code and pair") == 0);
  CHECK(phoneIndex(PH_ACT_PAIR_NEW) == phoneIndex(PH_ACT_PAIR) + 1);
  tapPhone(PH_ACT_PAIR_NEW);
  CHECK(gConfirm.open && gConfirm.line1 == "New pairing code?" && gConfirm.line2 == "Replaces the secret");
  CHECK(!gConfirm.defaultYes && gAsyncCommands.empty() && gCommands.empty());
  confirmYes();
  CHECK(gAsyncCommands.size() == 1 && gAsyncCommands[0] == "phone pair new" && gCommands.empty());
  sPhoneScroll.selectedIndex = 0;

  // The code screen: the time left, then the code as XXXX-XXXX on two large rows.
  const OLEDModeEntry& screen = modeEntry(OLED_BLUETOOTH_PHONE_CODE);
  CHECK(!screen.showInMenu && std::strcmp(screen.hints, "B:Close") == 0);
  openPairWindow(41, 999, true);
  currentOLEDMode = OLED_BLUETOOTH_PHONE_CODE;
  EXPECT_EXACT(render(OLED_BLUETOOTH_PHONE_CODE), "Pairing code  3:00", "7K2M-QX4P", "", "9TVW-H3RA");
  gPair.view.msLeft = 61000;
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE_CODE), "Pairing code  1:01");
  gPair.view.msLeft = 400;
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE_CODE), "Pairing code  0:01");
  // A and the stick do nothing; B closes this window, then the global handler pops.
  CHECK(screen.inputFunc(0, 0, INPUT_BUTTON_A) && screen.inputFunc(0, 1, 0) && gPairCloses.empty());
  CHECK(!screen.inputFunc(0, 0, INPUT_BUTTON_B));
  CHECK(gPairCloses.size() == 1 && gPairCloses[0] == 41);
  EXPECT_EXACT(render(OLED_BLUETOOTH_PHONE_CODE), "No code to show");  // its code is gone at once
  // How the window ended.
  gPair.view.state = PhonePairWindow::Paired;
  EXPECT_EXACT(render(OLED_BLUETOOTH_PHONE_CODE), "Phone paired");
  gPair.view.state = PhonePairWindow::Expired;
  EXPECT_EXACT(render(OLED_BLUETOOTH_PHONE_CODE), "Code expired", "", "New code and pair", "makes another one");
  gPair.view.state = PhonePairWindow::Closed;
  EXPECT_EXACT(render(OLED_BLUETOOTH_PHONE_CODE), "Pairing closed", "", "New code and pair", "makes another one");
  gPair.view.state = PhonePairWindow::None;
  EXPECT_EXACT(render(OLED_BLUETOOTH_PHONE_CODE), "No code to show");
  // A window whose code is for the serial console is never drawn here.
  openPairWindow(42, 10, false);
  EXPECT_EXACT(render(OLED_BLUETOOTH_PHONE_CODE), "No code to show");
  // Only for the display session that issued it: once that session ended (a
  // logout, then B's login) the frame draws none of it, wipes its copy and
  // closes the window; it never reaches the next session's screen.
  gPairCloses.clear();
  openPairWindow(43, 10, true);
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE_CODE), "Pairing code  3:00", "7K2M-QX4P");
  const unsigned epochChecks = gEpochChecks;
  gDisplayEpoch = 22;
  Rows after = render(OLED_BLUETOOTH_PHONE_CODE);
  EXPECT_EXACT(after, "No code to show");
  for (const auto& row : after) CHECK(!carriesShownCode(row));
  CHECK(gEpochChecks > epochChecks && gPairCloses.size() == 1 && gPairCloses[0] == 43 && !gPair.code[0]);
  // Nor for a window another source issued with the display's epoch.
  gDisplayEpoch = 21;
  openPairWindow(44, 10, true);
  gPair.view.source = SOURCE_SERIAL;
  EXPECT_EXACT(render(OLED_BLUETOOTH_PHONE_CODE), "No code to show");
  CHECK(gPairCloses.size() == 2 && gPairCloses[1] == 44 && !gPair.code[0]);
  gPairCloses.clear();
  // The countdown is bounded at 59:59 whatever the view says (the line fits
  // its buffer and 21 columns; the window itself is 3 min).
  openPairWindow(45, 0, true);
  gPair.view.msLeft = 4000000000u;
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE_CODE), "Pairing code  59:59", "7K2M-QX4P");
  gPair.view.msLeft = 3600000;
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE_CODE), "Pairing code  59:59");
  gPair.view.msLeft = 3598001;
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE_CODE), "Pairing code  59:59");
  gPair.view.msLeft = 3598000;
  EXPECT_ROWS(render(OLED_BLUETOOTH_PHONE_CODE), "Pairing code  59:58");
  CHECK(gPairCloses.empty());
  // The code only ever reached the screen: no toast, command, async command or confirm.
  for (const auto& s : gToasts) CHECK(!carriesShownCode(s));
  for (const auto& s : gCommands) CHECK(!carriesShownCode(s));
  for (const auto& s : gAsyncCommands) CHECK(!carriesShownCode(s));
  CHECK(!carriesShownCode(gConfirm.line1) && !carriesShownCode(gConfirm.line2));
  currentOLEDMode = OLED_BLUETOOTH;
}

static void pairWatch() {
  reset();
  oledBluetoothLinkWatch();  // takes in this state after the last test's
  TICK(0);
  // A window whose code went to serial: the watcher leaves it alone.
  openPairWindow(3, 5000, false); TICK(1);
  gPair.view.ageMs = 9000; TICK(0);
  CHECK(gPairCloses.empty());
  // A window for the OLED while its screen request is on its way: 2 s grace.
  currentOLEDMode = OLED_BLUETOOTH_PHONE;
  openPairWindow(7, 0, true); TICK(1);
  gPair.view.ageMs = 1990; gPair.view.msLeft = 178010; TICK(1);  // a new second on the countdown
  CHECK(gPairCloses.empty());
  gPair.view.ageMs = 2000; gPair.view.msLeft = 178000; TICK(1);  // never came up: closed
  CHECK(gPairCloses.size() == 1 && gPairCloses[0] == 7);
  TICK(1);  // its code is gone
  // On its screen it lives; the countdown redraws once a second.
  gPairCloses.clear();
  openPairWindow(8, 0, true);
  currentOLEDMode = OLED_BLUETOOTH_PHONE_CODE;
  TICK(1);
  gPair.view.ageMs = 3500; gPair.view.msLeft = 176500; TICK(1);
  gPair.view.ageMs = 3900; gPair.view.msLeft = 176100; TICK(0);  // same second
  gPair.view.ageMs = 4000; gPair.view.msLeft = 176000; TICK(1);
  CHECK(gPairCloses.empty());
  // Leaving the screen (B popped it, a jump elsewhere, a logout) closes it.
  currentOLEDMode = OLED_BLUETOOTH_PHONE;
  TICK(0);
  CHECK(gPairCloses.size() == 1 && gPairCloses[0] == 8);
  TICK(1);
  // How it ended is a change too.
  gPair.view.state = PhonePairWindow::Paired; TICK(1);
  TICK(0);
  CHECK(gPairCloses.size() == 1);
}
#endif

int main() {
  statusOff();
  statusCentral();
  statusConsole();
  mainMenu();
#if ENABLE_G2_GLASSES
  peerMenus();
#endif
  roleRows();
#if ENABLE_BLE_PHONE_PIPE
  phoneScreen();
  pairCodeScreen();
#endif
  linkWatch();
#if ENABLE_BLE_PHONE_PIPE
  pairWatch();
#endif
  std::printf("%u\n", checks);
  return 0;
}
