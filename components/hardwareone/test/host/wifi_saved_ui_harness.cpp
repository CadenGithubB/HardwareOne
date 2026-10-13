// Host substitutes exercise the extracted production menu functions. This
// fixture neither duplicates owner mutation logic nor implements authorization.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& value) : std::string(value) {}
  explicit String(uint32_t value) : std::string(std::to_string(value)) {}
};
static String operator+(const char* left, const String& right) {
  return String(std::string(left) + right);
}

#define ENABLE_WIFI 1
#define MAX_WIFI_NETWORKS 8
#define EXT_RAM_BSS_ATTR
#define DEBUG_G2F(...) do {} while (0)
#define OLED_SCROLL_MAX_ITEMS 32
#define OLED_CONTENT_HEIGHT 48
#define OLED_CONTENT_START_Y 12
#define DISPLAY_COLOR_WHITE 1
#define INPUT_BUTTON_A 1
#define INPUT_BUTTON_X 2
#define INPUT_CHECK(value, mask) ((value) & (mask))
#define pdMS_TO_TICKS(value) (value)
#define pdPASS 1
#define APP_CORE 1

// INSERT_PRODUCTION_TYPES

struct OLEDScrollState {
  const char* items[OLED_SCROLL_MAX_ITEMS] = {};
  int itemCount = 0;
  int selectedIndex = 0;
  int scrollOffset = 0;
};
struct Display {
  std::string message;
  void setTextSize(int) {}
  void setTextColor(int) {}
  void setCursor(int, int) {}
  void println(const char* text) { message = text; }
};
static Display display;
static Display* oledDisplay = &display;
static bool oledConnected = true;
static bool guest = false;
static bool navConsumed = false;
static bool snapshotAvailable = true;
static int snapshotsTaken = 0;
static WifiNetworkSnapshot ownerSnapshot;
static std::vector<std::string> commands;
static std::vector<std::string> rendered;
static void (*confirmYes)(void*) = nullptr;
static bool pageAdmission = true;
static uint32_t menuGeneration = 0;
static uint32_t enqueuedGeneration = 0;
static volatile bool gWifiPendingTaskActive = false;

// INSERT_PRODUCTION_STATE

static bool wifiNetworkSnapshot(WifiNetworkSnapshot& out) {
  ++snapshotsTaken;
  out = snapshotAvailable ? ownerSnapshot : WifiNetworkSnapshot{};
  return snapshotAvailable;
}
static void oledScrollInit(OLEDScrollState* state, void*, int) { *state = {}; }
static void oledScrollClearKeepSelection(OLEDScrollState* state) { state->itemCount = 0; }
static void oledScrollAddItem(OLEDScrollState* state, const char* label) {
  assert(state->itemCount < OLED_SCROLL_MAX_ITEMS);
  state->items[state->itemCount++] = label;
}
static void oledScrollClampSelection(OLEDScrollState* state) {
  if (state->selectedIndex >= state->itemCount) state->selectedIndex = state->itemCount - 1;
  if (state->selectedIndex < 0) state->selectedIndex = 0;
}
static void oledScrollRenderSimple(Display*, const OLEDScrollState* state) {
  rendered.clear();
  for (int i = 0; i < state->itemCount; ++i) rendered.emplace_back(state->items[i]);
}
static bool oledGuestBlocksMutate() { return guest; }
static bool oledScrollHandleNav(OLEDScrollState*) { return navConsumed; }
static void executeOLEDCommand(const String& line) { commands.push_back(line); }
static bool oledRunCommand(const String& line) { commands.push_back(line); return true; }
static void oledMenuBack() {}
static void oledMarkDirty() {}
static bool oledConfirmRequest(const char*, void*, void (*yes)(void*), void*, bool) {
  confirmYes = yes;
  return true;
}
static void wifiAddReset() {}
static void g2BumpMenuGen() { ++menuGeneration; }
static bool g2ShowListPage(const char* const* items, size_t count) {
  if (!pageAdmission) return false;
  rendered.clear();
  for (size_t i = 0; i < count; ++i) rendered.emplace_back(items[i]);
  enqueuedGeneration = menuGeneration;
  return true;
}
struct G2CmdCookie { int targetPage = 0; int targetNetSub = 0; };
static int g2GetHijackPage() { return 1; }
static bool g2SubmitHijackCommand(const char* line, const G2CmdCookie&, void (*)(), void*) {
  commands.emplace_back(line);
  return true;
}
static void onWifiConnectRefreshDone() {}
static void onWifiSavedListRefreshDone() {}
static uint32_t armWifiPendingOverlay() { return 1; }
static void clearWifiPendingOverlayIfOwned(uint32_t) {}
static bool claimWifiPendingWatchdog() { return false; }
static void taskStackRecord(const char*, int) {}
static void wifiPendingWatchdogTask(void*) {}
static int xTaskCreatePinnedToCore(void (*)(void*), const char*, int, void*, int, void*, int) {
  return pdPASS;
}
static void vTaskDelay(int) {}
static void showWiFiMenu();

// INSERT_PRODUCTION_FUNCTIONS

static void showWiFiMenu() { setNetSub(NET_SUB_WIFI); }

static void setOwner(uint32_t first, const char* name, uint32_t second = 0) {
  ownerSnapshot = {};
  ownerSnapshot.entries[0].id = first;
  ownerSnapshot.entries[0].ssid = name;
  ownerSnapshot.entries[0].priority = 1;
  ownerSnapshot.count = 1;
  if (second) {
    ownerSnapshot.entries[1].id = second;
    ownerSnapshot.entries[1].ssid = "Other";
    ownerSnapshot.entries[1].priority = 2;
    ownerSnapshot.count = 2;
  }
}

int main() {
  // OLED: rendering and acting do not re-read a reordered/replaced owner list.
  setOwner(11, "Office \"East\"", 22);
  savedNetworksOnEnter(true);
  assert(snapshotsTaken == 1 && sSavedNetScroll.itemCount == 2);
  setOwner(22, "Other", 33);
  displaySavedNetworksRendered();
  assert(rendered[0] == "Office \"East\"" && snapshotsTaken == 1);
  networkWifiListInputHandler(0, 0, INPUT_BUTTON_A);
  assert(commands.back() == "openwifi --saved-id 11");
  savedNetworksOnEnter(false);
  assert(snapshotsTaken == 1);

  // Confirmation retains the exact displayed ID despite deletion/recreation.
  networkWifiRemoveInputHandler(0, 0, INPUT_BUTTON_A);
  assert(sPendingRemoveId == 11 && confirmYes);
  setOwner(44, "Office \"East\"");
  confirmYes(nullptr);
  assert(commands.back() == "wifirmid 11" && sPendingRemoveId == 0);
  assert(sSavedNetSnapshot.entries[0].id == 44 && snapshotsTaken == 2);

  // Guest, navigation-only input, and out-of-range selections submit nothing.
  const size_t commandCount = commands.size();
  guest = true;
  networkWifiListInputHandler(0, 0, INPUT_BUTTON_A);
  networkWifiRemoveInputHandler(0, 0, INPUT_BUTTON_A);
  guest = false;
  navConsumed = true;
  networkWifiListInputHandler(0, 0, INPUT_BUTTON_A);
  navConsumed = false;
  sSavedNetScroll.selectedIndex = MAX_WIFI_NETWORKS;
  networkWifiListInputHandler(0, 0, INPUT_BUTTON_A);
  networkWifiRemoveInputHandler(0, 0, INPUT_BUTTON_A);
  assert(commands.size() == commandCount);

  // Session reset drops transient IDs and labels. Even a stale callback is inert.
  sSavedNetScroll.selectedIndex = 0;
  networkWifiRemoveInputHandler(0, 0, INPUT_BUTTON_A);
  oledNetworkModeResetSessionState();
  assert(sPendingRemoveId == 0 && sSavedNetScroll.itemCount == 0);
  assert(sSavedNetSnapshot.count == 0 && !sSavedNetSnapshotValid);
  onWifiRemoveConfirmed(nullptr);
  networkWifiListInputHandler(0, 0, INPUT_BUTTON_A);
  assert(commands.size() == commandCount);
  snapshotAvailable = false;
  savedNetworksOnEnter(true);
  displaySavedNetworksRendered();
  assert(display.message == "Saved list unavailable");
  networkWifiRemoveInputHandler(0, 0, INPUT_BUTTON_A);
  assert(commands.size() == commandCount);

  // G2: selection uses the admitted snapshot, not the live array position.
  snapshotAvailable = true;
  setOwner(101, "Long SSID with spaces", 202);
  showWiFiSavedList();
  assert(gNetSub == NET_SUB_WIFI_SAVED && gWifiSavedSnapshot.entries[0].id == 101);
  assert(menuGeneration > enqueuedGeneration);  // commit occurs after admission
  const int beforeTapSnapshots = snapshotsTaken;
  setOwner(202, "Other", 303);
  handleWiFiSavedTap(1);
  assert(gNetSub == NET_SUB_WIFI_SAVED_ACT);
  assert(rendered[0] == "<- Long SSID with spaces");
  assert(snapshotsTaken == beforeTapSnapshots);
  handleWiFiSavedActionTap(1);
  assert(commands.back() == "openwifi --saved-id 101");

  // A failed list admission preserves both selection and prior page routing.
  setOwner(401, "Original");
  showWiFiSavedList();
  handleWiFiSavedTap(1);
  const uint32_t previousGeneration = menuGeneration;
  pageAdmission = false;
  setOwner(501, "Replacement");
  showWiFiSavedList();
  assert(gWifiSavedSnapshot.entries[0].id == 401 && gWifiSavedSel == 0);
  assert(gNetSub == NET_SUB_WIFI_SAVED_ACT && menuGeneration == previousGeneration);
  handleWiFiSavedActionTap(2);
  assert(commands.back() == "wifirmid 401");

  // Rejection of an action page keeps the list as the active input surface.
  pageAdmission = true;
  showWiFiSavedList();
  pageAdmission = false;
  handleWiFiSavedTap(1);
  assert(gNetSub == NET_SUB_WIFI_SAVED);
  const size_t beforeInvalid = commands.size();
  handleWiFiSavedTap(UINT32_MAX);
  handleWiFiSavedActionTap(99);
  assert(commands.size() == beforeInvalid);

  // An unavailable source admits an inert error row, not an old selectable ID.
  pageAdmission = true;
  snapshotAvailable = false;
  showWiFiSavedList();
  assert(gWifiSavedSnapshot.count == 0 && rendered[1] == "(saved list unavailable)");
  handleWiFiSavedTap(1);
  assert(gNetSub == NET_SUB_WIFI_SAVED && commands.size() == beforeInvalid);
  std::puts("OLED/G2 saved-network selection checks passed");
}
