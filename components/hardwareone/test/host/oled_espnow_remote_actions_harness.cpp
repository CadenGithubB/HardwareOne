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
struct EspNowState { bool initialized = true; bool encryptionEnabled = true; };
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
// OLED_Utils.cpp): the admin gate's short form, else the reply, else the fallback.
namespace oled_message { constexpr size_t kMessageBytes = 192; constexpr uint32_t kErrorDurationMs = 8000; }
enum class OledUiIcon : uint8_t { NONE, INFO, WARNING, ERROR, SUCCESS, QUESTION };
[[maybe_unused]] static void oledCommandReplyText(bool, const char* reply, const char*, const char* failText,
                                                  char* out, size_t outSize) {
  if (reply && commandReplyIsAdminRefusal(reply)) std::snprintf(out, outSize, "Admin required");
  else std::snprintf(out, outSize, "%s", (reply && reply[0]) ? reply : (failText ? failText : ""));
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
static void oledKeyboardInit(const char* title, const char* initialText, int maxLength,
                             OledKeyboardDictationPolicy, bool masked = false) {
  keyboard = Keyboard{};
  keyboard.active = true;
  keyboard.title = title;
  keyboard.text = initialText;
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
  // "<registered command> <value>".
  struct ValueRow { const char* label; const char* value; const char* command; };
  const ValueRow valueRows[] = {
      {"Set Role", "backup", "espnowmeshrole backup"},
      {"Set Name", "bench2", "espnowsetname bench2"},
      {"Set Room", "Kitchen", "espnowroom Kitchen"},
      {"Set Zone", "Door", "espnowzone Door"},
      {"Set Pretty Name", "Bench Two", "espnowfriendlyname Bench Two"},
  };
  for (const ValueRow& row : valueRows) {
    beginCase(row.label, ESPNOW_VIEW_DEVICE_CONFIG);
    gOledEspNowState.deviceConfigMenuIndex = rowOf(row.label);
    CHECK(oledEspNowHandleDeviceConfigInput(0, 0, INPUT_BUTTON_A), "A on a row is consumed");
    CHECK(commands.empty(), "a value row sends nothing before its value is typed");
    CHECK(gOledEspNowState.currentView == ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD && keyboard.inits == 1,
          "a value row opens its keyboard");
    typeAndConfirm(row.value);
    CHECK(configKeyboardInput(0), "the confirmed keyboard is consumed");
    expectFormOpenedWith(row.command);
  }

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

  // S3: every refusal the owner returns is shown as a refusal, including the
  // ones the executor reports as done because they do not lead with "Error".
  for (size_t i = 0; i < sizeof(kRefusals) / sizeof(kRefusals[0]); i++) {
    beginCase("S3 owner refusal", ESPNOW_VIEW_REMOTE_FORM);
    execReply = kRefusals[i];
    sendForm("reboot");
    expectSentAndClosed("reboot");
    CHECK(toasts.size() == 1 && toasts[0] == "Remote not sent", kRefusals[i]);
  }

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

  if (failures) {
    std::fprintf(stderr, "%d OLED ESP-NOW remote action check(s) failed\n", failures);
    return 1;
  }
  std::puts("OLED ESP-NOW remote action checks passed");
  return 0;
}
