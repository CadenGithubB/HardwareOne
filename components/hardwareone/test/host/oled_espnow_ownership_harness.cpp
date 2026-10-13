// Host substitutes around the extracted OLED ESP-NOW "Unpair Device" adapter
// and the discovery-screen input arm. The unpair/pair-window owner
// (cmd_espnow_unpair, cmd_espnow_pairmode), the OLED command executor with its
// admin gate, and the toast are recorded doubles: the cases check that the OLED
// submits the owner's command under the display identity, shows a short
// refusal, and never calls the owner's peer-delete, registry-remove or
// pair-window functions itself.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& value) : std::string(value) {}
};

#define EXT_RAM_BSS_ATTR
#define INPUT_BUTTON_A 1u
#define INPUT_BUTTON_B 2u
#define INPUT_CHECK(state, btn) (((state) & (btn)) != 0)

// INSERT_TYPES

static NavEvents gNavEvents{};
static OLEDEspNowState gOledEspNowState{};
struct EspNowState { bool initialized = true; bool encryptionEnabled = true; };
static EspNowState sEspNow;
static EspNowState* gEspNow = &sEspNow;

// INSERT_DECLARATIONS

// Recorded boundaries.
static std::vector<std::string> commands;   // every line handed to either executor
static std::vector<std::string> toasts;
static bool execOk = true;                  // executeOLEDCommandWithResult's return value
static std::string execReply;               // and the reply it copies out
static int dirtyMarks = 0;
static int deleteCalls = 0, removeCalls = 0, openCalls = 0, closeCalls = 0;
static bool pairModeActive = false;
static int candidateCount = 0;

[[maybe_unused]] static void executeOLEDCommand(const String& line) { commands.push_back(line); }
// Mirrors OLED_Utils.cpp: the reply is copied (truncated) into out; a replaced
// display session returns false with out[0] == '\0'.
[[maybe_unused]] static bool executeOLEDCommandWithResult(const String& line, char* out, size_t outSize) {
  commands.push_back(line);
  std::snprintf(out, outSize, "%s", execReply.c_str());
  return execOk;
}
[[maybe_unused]] static void oledToastShow(const char* message, uint32_t = 3000) { toasts.emplace_back(message); }
[[maybe_unused]] static void oledMarkDirty() { ++dirtyMarks; }
// Owner internals the OLED must not call directly.
[[maybe_unused]] static bool espnowDeletePeerRuntime(const uint8_t*) { ++deleteCalls; return true; }
[[maybe_unused]] static void removeEspNowDevice(const uint8_t*) { ++removeCalls; }
[[maybe_unused]] static void espnowPairModeOpen(uint32_t) { ++openCalls; }
[[maybe_unused]] static void espnowPairModeClose() { ++closeCalls; }
// Read-only owner queries the screen keeps using.
[[maybe_unused]] static bool espnowPairModeActive() { return pairModeActive; }
[[maybe_unused]] static int espnowGetPairCandidateCount() { return candidateCount; }
[[maybe_unused]] static bool espnowGetPairCandidate(int, char* name, size_t nameCap, uint8_t mac[6], int* rssi) {
  std::snprintf(name, nameCap, "peer");
  std::memset(mac, 0x11, 6);
  if (rssi) *rssi = -40;
  return true;
}

// INSERT_PRODUCTION

// The ESPNOW_VIEW_PAIRING arm of oledEspNowHandleInput(), verbatim.
static bool pairingViewInput(uint32_t newlyPressed) {
  switch (gOledEspNowState.currentView) {
    // INSERT_PAIRING_ARM
    default:
      return false;
  }
}

static int failures = 0;
static const char* currentCase = "";
#define CHECK(condition, message) do { if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL %s: %s\n  (%s:%d: %s)\n", currentCase, message, \
               __FILE__, __LINE__, #condition); } } while (false)

static const char* adminRefusal(const char* command) {
  static char text[128];
  std::snprintf(text, sizeof(text),
                "Error: Admin access required for command '%s'. Contact an administrator.", command);
  return text;
}

static bool noOwnerCalls() {
  return deleteCalls == 0 && removeCalls == 0 && openCalls == 0 && closeCalls == 0;
}

static void beginCase(const char* name, OLEDEspNowView view) {
  currentCase = name;
  commands.clear();
  toasts.clear();
  deleteCalls = removeCalls = openCalls = closeCalls = 0;
  dirtyMarks = 0;
  execOk = true;
  execReply.clear();
  pairModeActive = false;
  candidateCount = 0;
  sEspNow = EspNowState{};
  gNavEvents = NavEvents{};
  gOledEspNowState.currentView = view;
  gOledEspNowState.needsRefresh = false;
  sPairingSel = 0;
}

static const char* kUnpairCommand = "espnowunpair AA:BB:CC:01:02:03";

static void expectRefusedUnpair(const char* toast) {
  CHECK(commands.size() == 1 && commands[0] == kUnpairCommand,
        "the OLED submits exactly one 'espnowunpair <MAC>' to the owner");
  CHECK(noOwnerCalls(), "the OLED never deletes the peer or the registry entry itself");
  CHECK(gOledEspNowState.currentView == ESPNOW_VIEW_DEVICE_CONFIG && !gOledEspNowState.needsRefresh,
        "a refused unpair stays on the device-config view");
  CHECK(toasts.size() == 1 && toasts[0] == toast, "a refused unpair shows the short refusal toast");
}

int main() {
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03};
  std::memcpy(gOledEspNowState.selectedDeviceMac, mac, 6);

  // U1: "Unpair Device" submits the owner's command; the OLED edits nothing itself.
  beginCase("U1 unpair accepted", ESPNOW_VIEW_DEVICE_CONFIG);
  execReply = "Unpaired device: lab (AA:BB:CC:01:02:03)";
  oledEspNowUnpairDevice();
  CHECK(commands.size() == 1 && commands[0] == kUnpairCommand,
        "the OLED submits exactly one 'espnowunpair <MAC>' to the owner");
  CHECK(deleteCalls == 0, "the OLED never calls espnowDeletePeerRuntime itself");
  CHECK(removeCalls == 0, "the OLED never calls removeEspNowDevice itself");
  CHECK(gOledEspNowState.currentView == ESPNOW_VIEW_DEVICE_LIST && gOledEspNowState.needsRefresh,
        "an accepted unpair returns to a refreshed device list");
  CHECK(toasts.empty(), "an accepted unpair shows no toast");

  // U2: the owner's admin refusal is shown short and the view does not move.
  beginCase("U2 unpair admin refusal", ESPNOW_VIEW_DEVICE_CONFIG);
  execOk = false;
  execReply = adminRefusal("espnowunpair");
  oledEspNowUnpairDevice();
  expectRefusedUnpair("Admin required");

  // U3: an "Error" reply with a true executor result is still a refusal.
  beginCase("U3 unpair unknown peer", ESPNOW_VIEW_DEVICE_CONFIG);
  execReply = "Error: Device 'AA:BB:CC:01:02:03' not found. Use 'espnowdevices' to see paired devices.";
  oledEspNowUnpairDevice();
  expectRefusedUnpair("Unpair failed");

  // U4: a replaced display session (false, empty reply) falls back to the generic toast.
  beginCase("U4 unpair replaced session", ESPNOW_VIEW_DEVICE_CONFIG);
  execOk = false;
  oledEspNowUnpairDevice();
  expectRefusedUnpair("Unpair failed");

  // U5: the owner's save-warning reply is not an error: the peer is gone in RAM.
  beginCase("U5 unpair save warning", ESPNOW_VIEW_DEVICE_CONFIG);
  execReply = "Unpaired device but failed to encrypt and save the registry - device encryption key "
              "unavailable. The pairing may not persist across reboot.";
  oledEspNowUnpairDevice();
  CHECK(commands.size() == 1 && commands[0] == kUnpairCommand, "the OLED submits 'espnowunpair <MAC>'");
  CHECK(noOwnerCalls(), "the OLED never deletes the peer or the registry entry itself");
  CHECK(gOledEspNowState.currentView == ESPNOW_VIEW_DEVICE_LIST && gOledEspNowState.needsRefresh,
        "a save-warning reply is treated as an accepted unpair");
  CHECK(toasts.empty(), "a save-warning reply shows no toast");

  // U6: with the radio down the owner answers, and the OLED shows it.
  beginCase("U6 unpair radio down", ESPNOW_VIEW_DEVICE_CONFIG);
  sEspNow.initialized = false;
  execReply = "Error: ESP-NOW not initialized. Run 'openespnow' first.";
  oledEspNowUnpairDevice();
  expectRefusedUnpair("Unpair failed");

  // P1: A on the closed discovery screen opens the window through the owner.
  beginCase("P1 discovery open accepted", ESPNOW_VIEW_PAIRING);
  sPairingSel = 2;
  execReply = "Discovery ON for 120s on channel 1.";
  CHECK(pairingViewInput(INPUT_BUTTON_A), "A on the discovery screen is consumed");
  CHECK(commands.size() == 1 && commands[0] == "espnowpairmode 120",
        "A submits 'espnowpairmode 120' to the owner");
  CHECK(openCalls == 0, "the OLED never calls espnowPairModeOpen itself");
  CHECK(sPairingSel == 0 && toasts.empty() && dirtyMarks == 1,
        "an accepted open resets the selection, shows no toast and repaints");
  CHECK(gOledEspNowState.currentView == ESPNOW_VIEW_PAIRING, "the screen stays on discovery");

  // P2: the owner's admin refusal of the open is shown; the window is not opened.
  beginCase("P2 discovery open admin refusal", ESPNOW_VIEW_PAIRING);
  execOk = false;
  execReply = adminRefusal("espnowpairmode");
  CHECK(pairingViewInput(INPUT_BUTTON_A), "A on the discovery screen is consumed");
  CHECK(commands.size() == 1 && commands[0] == "espnowpairmode 120",
        "A submits 'espnowpairmode 120' to the owner");
  CHECK(openCalls == 0, "a refused open never reaches espnowPairModeOpen");
  CHECK(toasts.size() == 1 && toasts[0] == "Admin required", "a refused open toasts 'Admin required'");
  CHECK(gOledEspNowState.currentView == ESPNOW_VIEW_PAIRING, "the screen stays on discovery");

  // P3: without a mesh passphrase the screen still submits nothing (screen hint).
  beginCase("P3 discovery open without passphrase", ESPNOW_VIEW_PAIRING);
  sEspNow.encryptionEnabled = false;
  CHECK(pairingViewInput(INPUT_BUTTON_A), "A on the discovery screen is consumed");
  CHECK(commands.empty() && noOwnerCalls() && toasts.empty(),
        "without a passphrase A submits nothing and shows no toast");
  CHECK(dirtyMarks == 1, "the screen repaints");

  // P4: B on the open discovery screen closes the window through the owner.
  beginCase("P4 discovery close accepted", ESPNOW_VIEW_PAIRING);
  pairModeActive = true;
  execReply = "Pairing mode off.";
  CHECK(pairingViewInput(INPUT_BUTTON_B), "B on the discovery screen is consumed");
  CHECK(commands.size() == 1 && commands[0] == "espnowpairmode off",
        "B submits 'espnowpairmode off' to the owner");
  CHECK(closeCalls == 0, "the OLED never calls espnowPairModeClose itself");
  CHECK(gOledEspNowState.currentView == ESPNOW_VIEW_MAIN_MENU && toasts.empty(),
        "an accepted close returns to the main menu without a toast");

  // P5: a refused close is shown, and the screen is still left.
  beginCase("P5 discovery close admin refusal", ESPNOW_VIEW_PAIRING);
  pairModeActive = true;
  execOk = false;
  execReply = adminRefusal("espnowpairmode");
  CHECK(pairingViewInput(INPUT_BUTTON_B), "B on the discovery screen is consumed");
  CHECK(commands.size() == 1 && commands[0] == "espnowpairmode off",
        "B submits 'espnowpairmode off' to the owner");
  CHECK(closeCalls == 0, "a refused close never reaches espnowPairModeClose");
  CHECK(toasts.size() == 1 && toasts[0] == "Admin required", "a refused close toasts 'Admin required'");
  CHECK(gOledEspNowState.currentView == ESPNOW_VIEW_MAIN_MENU, "B still leaves the discovery screen");

  // P6: candidates: A requests a pair (unchanged path); up/down clamp the selection.
  beginCase("P6 candidate pair request", ESPNOW_VIEW_PAIRING);
  pairModeActive = true;
  candidateCount = 1;
  CHECK(pairingViewInput(INPUT_BUTTON_A), "A on a candidate is consumed");
  CHECK(commands.size() == 1 && commands[0] == "espnowpairrequest 11:11:11:11:11:11",
        "A on a candidate submits 'espnowpairrequest <MAC>'");
  CHECK(noOwnerCalls() && toasts.empty() && dirtyMarks == 1,
        "a pair request touches no pair-window function and shows no toast");
  beginCase("P6 candidate navigation", ESPNOW_VIEW_PAIRING);
  pairModeActive = true;
  candidateCount = 3;
  gNavEvents.up = true;
  CHECK(!pairingViewInput(0) && sPairingSel == 0, "up at the first candidate is not consumed");
  gNavEvents = NavEvents{};
  gNavEvents.down = true;
  CHECK(pairingViewInput(0) && sPairingSel == 1, "down moves to the next candidate");
  CHECK(pairingViewInput(0) && sPairingSel == 2, "down moves to the last candidate");
  CHECK(!pairingViewInput(0) && sPairingSel == 2, "down at the last candidate is not consumed");
  gNavEvents = NavEvents{};
  gNavEvents.up = true;
  CHECK(pairingViewInput(0) && sPairingSel == 1, "up moves back");
  CHECK(commands.empty() && noOwnerCalls(), "navigation submits nothing");

  // P7: B with the window closed only leaves the screen.
  beginCase("P7 closed screen back", ESPNOW_VIEW_PAIRING);
  CHECK(pairingViewInput(INPUT_BUTTON_B), "B on the closed discovery screen is consumed");
  CHECK(gOledEspNowState.currentView == ESPNOW_VIEW_MAIN_MENU, "B returns to the main menu");
  CHECK(commands.empty() && noOwnerCalls() && toasts.empty(), "B with the window closed submits nothing");

  // P8: an owner refusal other than the admin gate toasts the generic text.
  beginCase("P8 discovery open owner error", ESPNOW_VIEW_PAIRING);
  execReply = "Error: Set a mesh passphrase first (espnowsetpassphrase). Pairing needs the shared mesh key.";
  CHECK(pairingViewInput(INPUT_BUTTON_A), "A on the discovery screen is consumed");
  CHECK(commands.size() == 1 && commands[0] == "espnowpairmode 120",
        "A submits 'espnowpairmode 120' to the owner");
  CHECK(openCalls == 0, "the OLED never calls espnowPairModeOpen itself");
  CHECK(toasts.size() == 1 && toasts[0] == "Discovery failed", "an owner error toasts 'Discovery failed'");

  if (failures) {
    std::fprintf(stderr, "%d OLED ESP-NOW ownership check(s) failed\n", failures);
    return 1;
  }
  std::puts("OLED ESP-NOW unpair/discovery ownership checks passed");
  return 0;
}
