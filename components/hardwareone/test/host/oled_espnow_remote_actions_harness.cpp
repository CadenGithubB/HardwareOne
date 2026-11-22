// Host substitutes around the extracted OLED ESP-NOW "Device Config" menu, its
// value keyboard arm, the remote-command form and the form's send. The command
// executor, the on-screen keyboard and the toast are recorded doubles. The
// cases check that a Device Config row sends nothing by itself, that it opens
// the remote form with a registered command filled in, and that the form's
// send shows the verdict of `espnowremote` (whose replies are taken from
// System_ESPNow.cpp by the Python driver: the generated tables below).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "System_MacText.h"  // production MAC text helpers (copied beside this unit)
#include "OLED_Layout.h"     // production: oled_layout::visibleRows (copied beside this unit)
#include "System_ESPNow_PeerStateCore.h"  // production: the peer-state rule (copied beside this unit)

class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& value) : std::string(value) {}
};

#define EXT_RAM_BSS_ATTR
#define INPUT_BUTTON_A 1u
#define INPUT_BUTTON_B 2u
#define INPUT_BUTTON_Y 8u
#define INPUT_CHECK(state, btn) (((state) & (btn)) != 0)

// INSERT_TYPES

static NavEvents gNavEvents{};
static OledEspNowState gOledEspNowState{};
struct EspNowDevice { uint8_t mac[6]; };
struct EspNowState {
  bool initialized = true;
  bool encryptionEnabled = true;
  EspNowDevice devices[16] = {};   // the paired registry (System_ESPNow.h: devices[16])
  int deviceCount = 0;
};
static EspNowState sEspNow;
static EspNowState* gEspNow = &sEspNow;

// Recorded boundaries.
static std::vector<std::string> commands;   // every line handed to either executor
static std::vector<std::string> toasts;
static bool execOk = true;                  // the executor's verdict before the reply rule
static std::string execReply;               // the reply it copies out
static int scrubs = 0;

[[maybe_unused]] static void executeOledCommand(const String& line) { commands.push_back(line); }
// Mirrors OLED_Utils.cpp: the reply is copied (truncated) into out; the
// executor reports a reply that is a failure by the reply rule as false.
[[maybe_unused]] static bool executeOledCommandWithResult(const String& line, char* out, size_t outSize) {
  commands.push_back(line);
  std::snprintf(out, outSize, "%s", execReply.c_str());
  return execOk && !commandReplyIsFailure(out);
}
// Shared OLED feedback (OLED_UI.h, OLED_MessageText.h, oledCommandReplyText in
// OLED_Utils.cpp) for a refusal, as the device shows it: the admin gate's
// short form, else the reply's reason (its "Error: " lead taken off) up to the
// first line break, else the fallback.
namespace oled_message { constexpr size_t kMessageBytes = 192; constexpr uint32_t kErrorDurationMs = 8000; }
enum class OledUiIcon : uint8_t { NONE, INFO, WARNING, ERROR, SUCCESS, QUESTION };
[[maybe_unused]] static void oledCommandReplyText(bool, const char* reply, const char*, const char* failText,
                                                  char* out, size_t outSize) {
  if (!reply) reply = "";
  if (commandReplyIsAdminRefusal(reply)) {
    std::snprintf(out, outSize, "Admin required");
    return;
  }
  const char* reason = commandReplyReason(reply);
  const size_t lineLength = std::strcspn(reason, "\r\n");
  if (lineLength == 0) std::snprintf(out, outSize, "%s", failText ? failText : "");
  else std::snprintf(out, outSize, "%.*s", (int)lineLength, reason);
}
// The toast for a refusal of `espnowremote` that leads with "Error": its reason,
// first line only, as much as the toast text holds.
static std::string shownReason(const char* reply) {
  std::string reason = commandReplyReason(reply);
  reason = reason.substr(0, reason.find_first_of("\r\n"));
  return reason.substr(0, oled_message::kMessageBytes - 1);
}
[[maybe_unused]] static void oledToastShow(const char* message, uint32_t = 3000, OledUiIcon = OledUiIcon::NONE) { toasts.emplace_back(message); }
static void secureClearString(String& text) { ++scrubs; text.clear(); }

enum class OledKeyboardDictationPolicy : uint8_t { DENY = 0, ALLOW_PLAINTEXT = 1 };
struct Keyboard {
  bool active = false, completed = false, cancelled = false, masked = false;
  std::string title, text;
  int maxLength = 0;
  int inits = 0;
};
static Keyboard keyboard;
// OLED_Utils.h: the most the on-screen keyboard holds (hand copy).
static constexpr int OLED_KEYBOARD_MAX_LENGTH = 128;
// As the real keyboard: the limit is clamped to what it holds, and text it is
// opened with is cut to that limit.
static void oledKeyboardInit(const char* title, const char* initialText, int maxLength,
                             OledKeyboardDictationPolicy, bool masked = false) {
  if (maxLength > OLED_KEYBOARD_MAX_LENGTH) maxLength = OLED_KEYBOARD_MAX_LENGTH;
  keyboard = Keyboard{};
  keyboard.active = true;
  keyboard.title = title;
  keyboard.text = std::string(initialText).substr(0, (size_t)maxLength);
  keyboard.maxLength = maxLength;
  keyboard.masked = masked;
  keyboard.inits = 1;
}
static void oledKeyboardReset() { keyboard = Keyboard{}; }
static bool oledKeyboardIsActive() { return keyboard.active; }
static bool oledKeyboardIsCompleted() { return keyboard.completed; }
static bool oledKeyboardIsCancelled() { return keyboard.cancelled; }
static const char* oledKeyboardGetText() { return keyboard.text.c_str(); }
static bool oledKeyboardHandleInput(int, int, uint32_t) { return keyboard.active; }
// The user types `text` and confirms.
static void typeAndConfirm(const char* text) { keyboard.text = text; keyboard.completed = true; }

// Owner internals the OLED must not call directly (linked only if it does).
[[maybe_unused]] static bool espnowDeletePeerRuntime(const uint8_t*) { std::abort(); }
[[maybe_unused]] static void removeEspNowDevice(const uint8_t*) { std::abort(); }

bool oledEspNowSendRemoteCommand();

// What the Device Config table takes its value limits from, as far as it reads
// them: the device name's rule (System_DeviceName.h) and the text fields of
// the metadata frame (System_ESPNow_Wire.h V4PayloadMetadata, whose real
// layout the espnow_wire_layout suite pins). Hand copies: the asserts below
// stop a change here that the headers did not make.
namespace devicename { constexpr size_t kMaxLen = 31; }
struct V4PayloadMetadata {
  char deviceName[32];
  char friendlyName[48];
  char room[32];
  char zone[32];
  char tags[54];
  uint8_t stationary;
  uint8_t reserved[3];
};
static_assert(sizeof(V4PayloadMetadata) == 202 && sizeof(V4PayloadMetadata::deviceName) - 1 == devicename::kMaxLen &&
              sizeof(V4PayloadMetadata::room) - 1 == 31 && sizeof(V4PayloadMetadata::zone) - 1 == 31 &&
              sizeof(V4PayloadMetadata::friendlyName) - 1 == 47 && sizeof(V4PayloadMetadata::tags) - 1 == 53,
              "the stand-in is not the metadata frame's text fields (31/31/47/53 without the NUL)");

// ---- the drawing surface of the Device Config menu ---------------------------
// The content height follows the canvas, as HAL_Display.h derives it: 43 px on
// the 64 px OLED, 107 px on the P4X-EYE's 128 px canvas.
static int canvasHeight = 64;
#define OLED_HEADER_HEIGHT 10
#define OLED_CONTENT_START_Y 11
#define OLED_CONTENT_HEIGHT (canvasHeight - 21)
#define SCREEN_WIDTH 128
#define DISPLAY_COLOR_WHITE 1
static int oledVisibleRows(int rowHeight, int reservedHeight = 0, int baselineRows = 0) {
  return oled_layout::visibleRows(OLED_CONTENT_HEIGHT, rowHeight, reservedHeight, baselineRows);
}
// What a frame drew: text (print and the bounded label fields), the selection
// bars and the rules.
struct Drawn { int x, y; std::string text; bool active; };
class Adafruit_SSD1306 {
 public:
  std::vector<Drawn> text;
  std::vector<int> barYs, ruleYs;
  void setTextSize(int) {}
  void setTextColor(int) {}
  void setCursor(int x, int y) { x_ = x; y_ = y; }
  void print(const char* value) { text.push_back({x_, y_, value, false}); }
  void println(const char* value) { print(value); x_ = 0; y_ += 8; }
  void fillRect(int, int y, int, int, int) { barYs.push_back(y); }
  void drawFastHLine(int, int y, int, int) { ruleYs.push_back(y); }
 private:
  int x_ = 0, y_ = 0;
};
static void oledTextFieldDraw(Adafruit_SSD1306* display, int x, int y, int, const char* label, uint16_t,
                              uint16_t, bool active, const void*) {
  display->text.push_back({x, y, label ? label : "", active});
}

// ---- the tables the peer state reads ------------------------------------------
struct MeshPeerHealth { uint8_t mac[6]; bool heartbeatRecent; };
static std::vector<MeshPeerHealth> sHealth;       // peers with a heartbeat slot
static std::vector<std::vector<uint8_t>> sSessions;  // peers with an ACTIVE session
static const uint8_t sSelf[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
static MeshPeerHealth* getMeshPeerHealth(const uint8_t mac[6], bool) {
  for (auto& health : sHealth) if (std::memcmp(health.mac, mac, 6) == 0) return &health;
  return nullptr;
}
static bool isMeshPeerAlive(const MeshPeerHealth* health) { return health && health->heartbeatRecent; }
static bool espnowSessionUp(const uint8_t* mac, uint32_t* = nullptr) {
  for (const auto& session : sSessions) if (std::memcmp(session.data(), mac, 6) == 0) return true;
  return false;
}
static bool isSelfMac(const uint8_t* mac) { return std::memcmp(mac, sSelf, 6) == 0; }

// INSERT_PEER_STATE

// ---- the discovery window the pairing screen reads ----------------------------
enum EspNowPairOutcome : uint8_t {
  ESPNOW_PAIR_OUT_NONE = 0, ESPNOW_PAIR_OUT_WAITING, ESPNOW_PAIR_OUT_PAIRED,
  ESPNOW_PAIR_OUT_DECLINED, ESPNOW_PAIR_OUT_TIMED_OUT,
};
static int sCandidates = 0;  // candidates the open window has heard
static bool espnowPairModeActive() { return true; }
static uint32_t espnowPairModeRemainingMs() { return 90000; }
static uint32_t millis() { return 1000; }
static void oledMarkDirtyUntil(uint32_t) {}
static int espnowGetPairCandidateCount() { return sCandidates; }
static bool espnowGetPairCandidate(int index, char* name, size_t cap, uint8_t mac[6], int* rssi) {
  if (index < 0 || index >= sCandidates) return false;
  std::snprintf(name, cap, "peer-%02d", index);
  std::memset(mac, index, 6);
  *rssi = -40;
  return true;
}
static EspNowPairOutcome espnowGetPairRequestOutcome(char* name, size_t cap) {
  if (cap) name[0] = '\0';
  return ESPNOW_PAIR_OUT_NONE;
}

// INSERT_PAIRING

// INSERT_PRODUCTION

// The ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD arm of oledEspNowHandleInput(), verbatim.
static bool configKeyboardInput(uint32_t newlyPressed) {
  const int deltaX = 0, deltaY = 0;
  switch (gOledEspNowState.currentView) {
    // INSERT_CONFIG_KEYBOARD_ARM
    default:
      return false;
  }
}

// INSERT_OWNER_REPLIES

static int failures = 0;
static const char* currentCase = "";
#define CHECK(condition, message) do { if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL %s: %s\n  (%s:%d: %s)\n", currentCase, message, \
               __FILE__, __LINE__, #condition); } } while (false)

static const char* kLine = "espnowremote AA:BB:CC:01:02:03 admin hunter2 ";

static void beginCase(const char* name, OledEspNowView view) {
  currentCase = name;
  commands.clear();
  toasts.clear();
  scrubs = 0;
  execOk = true;
  execReply.clear();
  keyboard = Keyboard{};
  sEspNow = EspNowState{};
  gNavEvents = NavEvents{};
  gOledEspNowState.currentView = view;
  gOledEspNowState.needsRefresh = false;
  gOledEspNowState.deviceConfigMenuIndex = 0;
  gOledEspNowState.deviceConfigEditField = -1;
  gOledEspNowState.deviceConfigScrollOffset = 0;
  gOledEspNowState.remoteFormField = 2;
  gOledEspNowState.remoteUsername.clear();
  gOledEspNowState.remotePassword.clear();
  gOledEspNowState.remoteCommand.clear();
  gOledEspNowState.selectedDeviceName = "lab";
}

static int rowOf(const char* label) {
  for (int i = 0; i < DEVICE_CONFIG_COUNT; i++) {
    if (std::strcmp(kDeviceConfigActions[i].label, label) == 0) return i;
  }
  std::fprintf(stderr, "FAIL: the Device Config menu has no row '%s'\n", label);
  std::exit(1);
}

// Device Config row -> (keyboard) -> remote form, with nothing sent.
static void expectFormOpenedWith(const char* command) {
  CHECK(commands.empty(), "a Device Config row sends no command by itself");
  CHECK(toasts.empty(), "opening the remote form shows no toast");
  CHECK(gOledEspNowState.currentView == ESPNOW_VIEW_REMOTE_FORM, "the row opens the remote form");
  CHECK(gOledEspNowState.remoteCommand == command, "the form's command line is filled in");
  CHECK(gOledEspNowState.remoteUsername.empty() && gOledEspNowState.remotePassword.empty(),
        "the form opens with no account typed");
  CHECK(gOledEspNowState.remoteFormField == 0, "the form opens on the user field");
  CHECK(!oledKeyboardIsActive(), "no keyboard is left open over the form");
  CHECK(gOledEspNowState.selectedDeviceName == "lab", "the name shown for the peer is not changed locally");
}

// Fill the form as a user would and press Y.
static void sendForm(const char* command) {
  gOledEspNowState.currentView = ESPNOW_VIEW_REMOTE_FORM;
  gOledEspNowState.remoteUsername = "admin";
  gOledEspNowState.remotePassword = "hunter2";
  gOledEspNowState.remoteCommand = command;
  CHECK(oledEspNowHandleRemoteFormInput(0, 0, INPUT_BUTTON_Y), "Y on the form is consumed");
}

static void expectSentAndClosed(const char* command) {
  CHECK(commands.size() == 1 && commands[0] == std::string(kLine) + command,
        "the form submits one 'espnowremote <MAC> <user> <pass> <command>'");
  CHECK(gOledEspNowState.remoteUsername.empty() && gOledEspNowState.remotePassword.empty() &&
        gOledEspNowState.remoteCommand.empty() && scrubs >= 3, "the form is scrubbed after the send");
  CHECK(gOledEspNowState.currentView == ESPNOW_VIEW_DEVICE_DETAIL && gOledEspNowState.needsRefresh,
        "the screen returns to the refreshed conversation");
}

int main() {
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03};
  std::memcpy(gOledEspNowState.selectedDeviceMac, mac, 6);

  // D1: "Restart Device" opens the form with `reboot`; nothing is sent.
  beginCase("D1 restart row", ESPNOW_VIEW_DEVICE_CONFIG);
  gOledEspNowState.deviceConfigMenuIndex = rowOf("Restart Device");
  CHECK(oledEspNowHandleDeviceConfigInput(0, 0, INPUT_BUTTON_A), "A on a row is consumed");
  expectFormOpenedWith("reboot");

  // D2: the five value rows ask for the value, then open the form with
  // "<registered command> <value>". The keyboard takes as many characters as
  // the peer's command accepts: 31 for the device name (its rule), 31/31/47
  // for room, zone and friendly name (the metadata frame's fields without the
  // NUL). They were 31/16/16/24, so a room the peer accepts could not be typed.
  struct ValueRow { const char* label; const char* value; const char* command;
                    const char* title; int maxLength; const char* initial; };
  const ValueRow valueRows[] = {
      {"Set Role", "backup", "espnowmeshrole backup", "Role (master/backup/worker):", 16, ""},
      {"Set Name", "bench2", "devicename bench2", "Device Name:", 31, "lab"},
      {"Set Room", "Kitchen", "espnowroom Kitchen", "Room:", 31, ""},
      {"Set Zone", "Door", "espnowzone Door", "Zone:", 31, ""},
      {"Set Friendly Name", "Bench Two", "espnowfriendlyname Bench Two", "Friendly Name:", 47, ""},
  };
  for (const ValueRow& row : valueRows) {
    beginCase(row.label, ESPNOW_VIEW_DEVICE_CONFIG);
    gOledEspNowState.deviceConfigMenuIndex = rowOf(row.label);
    CHECK(oledEspNowHandleDeviceConfigInput(0, 0, INPUT_BUTTON_A), "A on a row is consumed");
    CHECK(commands.empty(), "a value row sends nothing before its value is typed");
    CHECK(gOledEspNowState.currentView == ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD && keyboard.inits == 1,
          "a value row opens its keyboard");
    CHECK(keyboard.title == row.title, "the keyboard is titled with the row's prompt");
    CHECK(keyboard.maxLength == row.maxLength, "the keyboard takes what the peer's command accepts");
    CHECK(keyboard.text == row.initial, "only Set Name starts from the peer's shown name");
    typeAndConfirm(row.value);
    CHECK(configKeyboardInput(0), "the confirmed keyboard is consumed");
    expectFormOpenedWith(row.command);
  }

  // D2b: the limits come from the name rule and the metadata frame, the row
  // says "Friendly Name" as the local settings list does (it said "Pretty
  // Name"), and every label fits the menu row: 20 columns from x=4 on the
  // 128 px panel.
  beginCase("D2b limits and labels", ESPNOW_VIEW_DEVICE_CONFIG);
  CHECK(kDeviceConfigActions[rowOf("Set Name")].valueMax == devicename::kMaxLen &&
        kDeviceConfigActions[rowOf("Set Room")].valueMax == sizeof(V4PayloadMetadata::room) - 1 &&
        kDeviceConfigActions[rowOf("Set Zone")].valueMax == sizeof(V4PayloadMetadata::zone) - 1 &&
        kDeviceConfigActions[rowOf("Set Friendly Name")].valueMax == sizeof(V4PayloadMetadata::friendlyName) - 1,
        "a value limit is the name rule's or the metadata field's");
  for (int i = 0; i < DEVICE_CONFIG_COUNT; i++) {
    const DeviceConfigAction& action = kDeviceConfigActions[i];
    CHECK(std::strlen(action.label) <= 20, "a Device Config label fits its menu row (20 columns)");
    CHECK(!std::strstr(action.label, "Pretty") &&
          !(action.valuePrompt && std::strstr(action.valuePrompt, "Pretty")),
          "the friendly name is called 'Friendly Name'");
    CHECK(!action.valuePrompt || action.valueMax > 0, "a row that asks for a value accepts one");
  }

  // D2c: the form's Command field holds every command a Device Config row
  // can prefill, so opening that field and confirming it leaves the command as
  // it was. At 64 characters a 47-character friendly name ("espnowfriendlyname"
  // plus a space plus the name: 66) lost its last two characters there, and
  // the peer stored the shortened name. User and Pass keep their 64.
  beginCase("D2c long value through the Command field", ESPNOW_VIEW_DEVICE_CONFIG);
  const std::string longName(sizeof(V4PayloadMetadata::friendlyName) - 1, 'n');
  const std::string longCommand = "espnowfriendlyname " + longName;
  gOledEspNowState.deviceConfigMenuIndex = rowOf("Set Friendly Name");
  CHECK(oledEspNowHandleDeviceConfigInput(0, 0, INPUT_BUTTON_A), "A on a row is consumed");
  typeAndConfirm(longName.c_str());
  CHECK(configKeyboardInput(0), "the confirmed keyboard is consumed");
  expectFormOpenedWith(longCommand.c_str());
  gOledEspNowState.remoteFormField = 2;
  CHECK(oledEspNowHandleRemoteFormInput(0, 0, INPUT_BUTTON_A), "A on the Command field is consumed");
  CHECK(keyboard.inits == 1 && keyboard.title == "Command:" && !keyboard.masked,
        "A on the Command field opens its keyboard, unmasked");
  CHECK(keyboard.text == longCommand, "the Command keyboard opens with the whole prefilled command");
  for (int i = 0; i < DEVICE_CONFIG_COUNT; i++) {
    const DeviceConfigAction& action = kDeviceConfigActions[i];
    if (!action.remoteCommand) continue;
    const size_t longest = std::strlen(action.remoteCommand) + (action.valuePrompt ? 1 + action.valueMax : 0);
    CHECK(longest <= (size_t)keyboard.maxLength, "the Command field holds a row's longest command");
  }
  keyboard.completed = true;  // confirmed as it stands
  CHECK(oledEspNowHandleRemoteFormInput(0, 0, 0), "the confirmed keyboard is consumed");
  CHECK(gOledEspNowState.remoteCommand == longCommand && !oledKeyboardIsActive(),
        "confirming the Command field unchanged keeps the whole command");
  for (int field = 0; field < 2; field++) {
    gOledEspNowState.remoteFormField = field;
    CHECK(oledEspNowHandleRemoteFormInput(0, 0, INPUT_BUTTON_A), "A on a form field is consumed");
    CHECK(keyboard.maxLength == 64 && keyboard.masked == (field == 1),
          "User and Pass keep 64 characters; only Pass is masked");
    keyboard.cancelled = true;
    CHECK(oledEspNowHandleRemoteFormInput(0, 0, 0), "the cancelled keyboard is consumed");
  }
  gOledEspNowState.remoteFormField = 2;
  sendForm(longCommand.c_str());
  CHECK(commands.size() == 1 && commands[0] == std::string(kLine) + longCommand,
        "the long command is submitted whole");

  // D3: an empty or cancelled value returns to the menu; nothing is sent.
  beginCase("D3 empty value", ESPNOW_VIEW_DEVICE_CONFIG);
  gOledEspNowState.deviceConfigMenuIndex = rowOf("Set Room");
  oledEspNowHandleDeviceConfigInput(0, 0, INPUT_BUTTON_A);
  typeAndConfirm("");
  CHECK(configKeyboardInput(0), "the confirmed keyboard is consumed");
  CHECK(commands.empty() && gOledEspNowState.currentView == ESPNOW_VIEW_DEVICE_CONFIG &&
        gOledEspNowState.remoteCommand.empty() && gOledEspNowState.deviceConfigEditField == -1,
        "an empty value returns to the Device Config menu with nothing filled in");
  beginCase("D3 cancelled value", ESPNOW_VIEW_DEVICE_CONFIG);
  gOledEspNowState.deviceConfigMenuIndex = rowOf("Set Zone");
  oledEspNowHandleDeviceConfigInput(0, 0, INPUT_BUTTON_A);
  keyboard.cancelled = true;
  CHECK(configKeyboardInput(0), "the cancelled keyboard is consumed");
  CHECK(commands.empty() && gOledEspNowState.currentView == ESPNOW_VIEW_DEVICE_CONFIG,
        "a cancelled value returns to the Device Config menu");

  // D4: "Unpair Device" still goes to the unpair owner, not to the form.
  beginCase("D4 unpair row", ESPNOW_VIEW_DEVICE_CONFIG);
  gOledEspNowState.deviceConfigMenuIndex = rowOf("Unpair Device");
  execReply = "Unpaired device: lab (AA:BB:CC:01:02:03)";
  CHECK(oledEspNowHandleDeviceConfigInput(0, 0, INPUT_BUTTON_A), "A on a row is consumed");
  CHECK(commands.size() == 1 && commands[0] == "espnowunpair AA:BB:CC:01:02:03",
        "Unpair submits 'espnowunpair <MAC>'");
  CHECK(gOledEspNowState.currentView == ESPNOW_VIEW_DEVICE_LIST, "an accepted unpair returns to the list");

  // S1: the owner's text acknowledgement (as the executor stamps it): no toast.
  for (size_t i = 0; i < sizeof(kTextAcks) / sizeof(kTextAcks[0]); i++) {
    beginCase("S1 text acknowledgement", ESPNOW_VIEW_REMOTE_FORM);
    execReply = kTextAcks[i];
    sendForm("reboot");
    expectSentAndClosed("reboot");
    CHECK(toasts.empty(), "an acknowledged remote command shows no refusal");
    CHECK(oledEspNowRemoteAcknowledged(kTextAcks[i]), "the owner's acknowledgement is recognised");
  }

  // S2: the JSON acknowledgement (the typed command holds a `json` word).
  beginCase("S2 JSON acknowledgement", ESPNOW_VIEW_REMOTE_FORM);
  execReply = kJsonAck;
  sendForm("files json \"/\"");
  expectSentAndClosed("files json \"/\"");
  CHECK(toasts.empty(), "a JSON acknowledgement shows no refusal");

  // S3: every refusal the owner returns is shown as a refusal: one that leads
  // with "Error" as its reason (no "Error: " lead, first line), a usage or
  // JSON one as the fixed text.
  size_t errorRefusals = 0;
  for (size_t i = 0; i < sizeof(kRefusals) / sizeof(kRefusals[0]); i++) {
    beginCase("S3 owner refusal", ESPNOW_VIEW_REMOTE_FORM);
    execReply = kRefusals[i];
    sendForm("reboot");
    expectSentAndClosed("reboot");
    const bool isError = commandReplyIsError(kRefusals[i]);
    if (isError) errorRefusals++;
    const std::string shown = isError ? shownReason(kRefusals[i]) : "Remote not sent";
    CHECK(toasts.size() == 1 && toasts[0] == shown, kRefusals[i]);
    CHECK(toasts.size() == 1 && toasts[0].compare(0, 5, "Error") != 0 && !toasts[0].empty(),
          "the toast does not repeat the error lead");
  }
  CHECK(errorRefusals > 0, "the owner has refusals that lead with Error (the reason path is reached)");

  // S3b: a refusal of more than one line shows its first line only.
  beginCase("S3b two-line refusal", ESPNOW_VIEW_REMOTE_FORM);
  execReply = "Error: Target device 'lab' not found or not paired.\nPair the device first.";
  sendForm("reboot");
  expectSentAndClosed("reboot");
  CHECK(toasts.size() == 1 && toasts[0] == "Target device 'lab' not found or not paired.",
        "a refusal of two lines toasts its reason up to the line break");

  // S4: the executor's admin gate.
  beginCase("S4 admin refusal", ESPNOW_VIEW_REMOTE_FORM);
  execOk = false;
  execReply = "Error: Admin access required for command 'espnowremote'. Contact an administrator.";
  sendForm("reboot");
  expectSentAndClosed("reboot");
  CHECK(toasts.size() == 1 && toasts[0] == "Admin required", "the admin gate toasts 'Admin required'");

  // S5: a replaced display session (false, empty reply).
  beginCase("S5 replaced session", ESPNOW_VIEW_REMOTE_FORM);
  execOk = false;
  sendForm("reboot");
  expectSentAndClosed("reboot");
  CHECK(toasts.size() == 1 && toasts[0] == "Remote not sent", "a lost reply is not reported as sent");

  // S6: an echoed command cannot fake the acknowledgement.
  beginCase("S6 echo is not an acknowledgement", ESPNOW_VIEW_REMOTE_FORM);
  execReply = "OK: Target device 'x (match reqId 5)' not found or not paired. Pair the device first.";
  sendForm("reboot");
  CHECK(toasts.size() == 1 && toasts[0] == "Remote not sent", "an id inside the echo is not the request id");
  CHECK(!oledEspNowRemoteAcknowledged("(match reqId )") && !oledEspNowRemoteAcknowledged("(match reqId 7) x") &&
        !oledEspNowRemoteAcknowledged("") && !oledEspNowRemoteAcknowledged(nullptr) &&
        !oledEspNowRemoteAcknowledged("{\"schema\":1,\"ok\":true}") &&
        oledEspNowRemoteAcknowledged("x (match reqId 5) y (match reqId 6)"),
        "the request id must end the reply");

  // S7: an incomplete form sends nothing, says so and stays open as typed.
  beginCase("S7 incomplete form", ESPNOW_VIEW_REMOTE_FORM);
  gOledEspNowState.remoteCommand = "reboot";
  gOledEspNowState.remoteUsername = "admin";
  CHECK(oledEspNowHandleRemoteFormInput(0, 0, INPUT_BUTTON_Y), "Y on the form is consumed");
  CHECK(commands.empty(), "an incomplete form sends nothing");
  CHECK(toasts.size() == 1 && toasts[0] == "Fill all 3 fields", "an incomplete form says what is missing");
  CHECK(gOledEspNowState.currentView == ESPNOW_VIEW_REMOTE_FORM &&
        gOledEspNowState.remoteCommand == "reboot" && gOledEspNowState.remoteUsername == "admin",
        "an incomplete form stays open as typed");

  // S8: B on the form scrubs it and returns to the conversation.
  beginCase("S8 cancelled form", ESPNOW_VIEW_REMOTE_FORM);
  gOledEspNowState.remoteCommand = "reboot";
  gOledEspNowState.remotePassword = "hunter2";
  CHECK(oledEspNowHandleRemoteFormInput(0, 0, INPUT_BUTTON_B), "B on the form is consumed");
  CHECK(commands.empty() && gOledEspNowState.remotePassword.empty() && gOledEspNowState.remoteCommand.empty() &&
        gOledEspNowState.currentView == ESPNOW_VIEW_DEVICE_DETAIL, "a cancelled form is scrubbed and sends nothing");

  // L1: the Device Config menu at both canvas heights. Every row the cursor
  // reaches is drawn, selected, below the title and above the footer; the
  // 64 px OLED shows 3 rows at a time, the 128 px canvas all of them.
  for (int height : {64, 128}) {
    canvasHeight = height;
    beginCase(height == 64 ? "L1 Device Config window, 64 px" : "L1 Device Config window, 128 px",
              ESPNOW_VIEW_DEVICE_CONFIG);
    const int rows = height == 64 ? 3 : DEVICE_CONFIG_COUNT;
    const int listY = OLED_CONTENT_START_Y + 11;
    const int footerY = OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT;
    CHECK(oledEspNowDeviceConfigRows() >= rows, "the window holds the rows that fit");
    auto frame = [&](int expectedTop) {
      Adafruit_SSD1306 display;
      oledEspNowDisplayDeviceConfig(&display);
      const int selected = gOledEspNowState.deviceConfigMenuIndex;
      int labels = 0;
      bool selectedDrawn = false, titleDrawn = false;
      for (const Drawn& item : display.text) {
        if (item.y == OLED_CONTENT_START_Y) {
          titleDrawn = titleDrawn || item.text == "lab";
          continue;
        }
        if (item.x != 4) continue;  // the scroll arrows
        ++labels;
        CHECK(item.y >= listY && item.y + 8 <= footerY, "a row is drawn between the title and the footer");
        const int index = gOledEspNowState.deviceConfigScrollOffset + (item.y - listY) / 10;
        CHECK(index >= 0 && index < DEVICE_CONFIG_COUNT && item.text == kDeviceConfigActions[index].label,
              "each row shows its action's label");
        if (index == selected) selectedDrawn = item.active;
      }
      CHECK(titleDrawn, "the title names the peer being configured");
      CHECK(display.ruleYs.size() == 1 && display.ruleYs[0] == OLED_CONTENT_START_Y + 9,
            "one rule under the title");
      CHECK(labels == (DEVICE_CONFIG_COUNT < rows ? DEVICE_CONFIG_COUNT : rows), "the window is full");
      CHECK(selectedDrawn, "the selected row is drawn, and drawn as selected");
      CHECK(display.barYs.size() == 1 &&
            display.barYs[0] == listY + (selected - gOledEspNowState.deviceConfigScrollOffset) * 10,
            "the selection bar marks the selected row");
      CHECK(gOledEspNowState.deviceConfigScrollOffset == expectedTop, "the window follows the cursor");
    };
    frame(0);
    gNavEvents.down = true;
    for (int i = 1; i < DEVICE_CONFIG_COUNT; i++) {
      CHECK(oledEspNowHandleDeviceConfigInput(0, 0, 0), "down moves the cursor");
      frame(i < rows ? 0 : i - rows + 1);
    }
    CHECK(gOledEspNowState.deviceConfigMenuIndex == DEVICE_CONFIG_COUNT - 1 &&
          kDeviceConfigActions[DEVICE_CONFIG_COUNT - 1].remoteCommand == nullptr,
          "the last row (Unpair Device) is reached");
    gNavEvents = NavEvents{};
    gNavEvents.up = true;
    const int bottomTop = DEVICE_CONFIG_COUNT > rows ? DEVICE_CONFIG_COUNT - rows : 0;
    for (int i = DEVICE_CONFIG_COUNT - 2; i >= 0; i--) {
      CHECK(oledEspNowHandleDeviceConfigInput(0, 0, 0), "up moves the cursor");
      frame(i < bottomTop ? i : bottomTop);
    }
    gNavEvents = NavEvents{};
  }
  canvasHeight = 64;

  // L2: the discovery candidates at both canvas heights: as many rows as end
  // above the footer under the two status lines (2, then 8), the selected
  // one always among them (a third row used to sit under the 64 px footer).
  for (int height : {64, 128}) {
    canvasHeight = height;
    currentCase = height == 64 ? "L2 pairing candidates, 64 px" : "L2 pairing candidates, 128 px";
    const int rows = height == 64 ? 2 : 8;
    const int footerY = OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT;
    sCandidates = 12;
    for (sPairingSel = 0; sPairingSel < sCandidates; sPairingSel++) {
      Adafruit_SSD1306 display;
      oledEspNowDisplayPairing(&display);
      int drawn = 0;
      bool selectedDrawn = false;
      for (const Drawn& item : display.text) {
        if (item.x != 12) continue;  // the candidate names
        ++drawn;
        CHECK(item.y >= OLED_CONTENT_START_Y + 22 && item.y + 8 <= footerY,
              "a candidate row ends above the footer");
        if (item.text == "peer-" + std::string(sPairingSel < 10 ? "0" : "") + std::to_string(sPairingSel)) {
          selectedDrawn = item.active;
        }
      }
      CHECK(drawn == rows, "the window holds the rows that fit");
      CHECK(selectedDrawn, "the selected candidate is drawn, and drawn as selected");
    }
  }
  canvasHeight = 64;
  sPairingSel = 0;
  sCandidates = 0;

  // P1: the peer-state rule: online needs both a recent heartbeat and a
  // session; a heartbeat alone is connecting; no heartbeat is offline, also
  // with a session left over.
  currentCase = "P1 peer-state rule";
  CHECK(espnowPeerStateOf(true, true) == EspNowPeerState::Online, "heartbeat + session = online");
  CHECK(espnowPeerStateOf(true, false) == EspNowPeerState::Connecting, "heartbeat, no session = connecting");
  CHECK(espnowPeerStateOf(false, true) == EspNowPeerState::Offline, "a session without heartbeat = offline");
  CHECK(espnowPeerStateOf(false, false) == EspNowPeerState::Offline, "nothing = offline");
  CHECK(std::string(espnowPeerStateName(EspNowPeerState::Online)) == "online" &&
        std::string(espnowPeerStateName(EspNowPeerState::Connecting)) == "connecting" &&
        std::string(espnowPeerStateName(EspNowPeerState::Offline)) == "offline", "the state words");

  // P2: the counts walk the paired registry without this device; a heartbeat
  // from a device that is not paired counts nowhere.
  currentCase = "P2 peer counts";
  sEspNow = EspNowState{};
  const uint8_t up[6] = {1, 1, 1, 1, 1, 1}, joining[6] = {2, 2, 2, 2, 2, 2}, silent[6] = {3, 3, 3, 3, 3, 3},
                stale[6] = {4, 4, 4, 4, 4, 4}, stranger[6] = {5, 5, 5, 5, 5, 5};
  for (const uint8_t* peer : {sSelf, up, joining, silent, stale}) {
    std::memcpy(sEspNow.devices[sEspNow.deviceCount++].mac, peer, 6);
  }
  sHealth = {};
  for (const uint8_t* peer : {sSelf, up, joining, stranger}) {
    MeshPeerHealth health{};
    std::memcpy(health.mac, peer, 6);
    health.heartbeatRecent = true;
    sHealth.push_back(health);
  }
  MeshPeerHealth old{};
  std::memcpy(old.mac, stale, 6);
  sHealth.push_back(old);  // a slot whose heartbeat timed out
  sSessions = {std::vector<uint8_t>(sSelf, sSelf + 6), std::vector<uint8_t>(up, up + 6),
               std::vector<uint8_t>(stale, stale + 6), std::vector<uint8_t>(stranger, stranger + 6)};
  CHECK(espnowPeerState(up) == EspNowPeerState::Online, "heartbeat + session reads online");
  CHECK(espnowPeerState(joining) == EspNowPeerState::Connecting, "heartbeat only reads connecting");
  CHECK(espnowPeerState(silent) == EspNowPeerState::Offline, "no health slot reads offline");
  CHECK(espnowPeerState(stale) == EspNowPeerState::Offline, "a timed-out heartbeat reads offline");
  EspNowPeerCounts counts = espnowPeerCounts();
  CHECK(counts.online == 1 && counts.connecting == 1 && counts.offline == 2 && counts.paired() == 4,
        "online 1, connecting 1, offline 2 of 4 paired (this device and the stranger left out)");
  sEspNow.initialized = false;
  counts = espnowPeerCounts();
  CHECK(counts.paired() == 0, "nothing is counted while ESP-NOW is stopped");
  sEspNow = EspNowState{};

  if (failures) {
    std::fprintf(stderr, "%d OLED ESP-NOW remote action check(s) failed\n", failures);
    return 1;
  }
  std::puts("OLED ESP-NOW remote action checks passed");
  return 0;
}
