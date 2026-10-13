// Host harness for the OLED session-role gate (test_oled_session_role.py).
//
// The suite pastes the shipping session-role code, oledModeAllowedForGuest,
// oledGuestBlocksMutate, getMenuAvailability and oledApplyPendingSessionBoundary
// from OLED_Utils.cpp, plus the launcher populate and right-pane decorator from
// OLED_Mode_Menu.cpp, into the markers below. The roster fakes count every
// users.json read (getUserAuthorizationRole and isAdminUser).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

// Every optional feature off: getMenuAvailability compiles its NOT_BUILT paths.
#define ENABLE_NEOPIXEL 0
#define ENABLE_AUTOMATION 0
#define ENABLE_ESPNOW 0
#define ENABLE_BONDED_MODE 0
#define ENABLE_THERMAL_SENSOR 0
#define ENABLE_FM_RADIO 0
#define ENABLE_GPS_SENSOR 0
#define ENABLE_IMU_SENSOR 0
#define ENABLE_TOF_SENSOR 0
#define ENABLE_APDS_SENSOR 0
#define ENABLE_GAMEPAD_SENSOR 0
#define ENABLE_RTC_SENSOR 0
#define ENABLE_PRESENCE_SENSOR 0
#define ENABLE_BLUETOOTH 0
#define ENABLE_WIFI 0
#define ENABLE_OLED_INPUT 0
#define EXT_RAM_BSS_ATTR
#define DISPLAY_COLOR_WHITE 1
#define OLED_CONTENT_START_Y 10
#define OLED_CONTENT_HEIGHT 54

class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  String& operator=(const char* text) {
    text_ = text ? text : "";
    return *this;
  }
  bool operator==(const char* other) const { return text_ == (other ? other : ""); }
  bool operator!=(const char* other) const { return !(*this == other); }
  const char* c_str() const { return text_.c_str(); }
  size_t length() const { return text_.length(); }
  void clear() { text_.clear(); }

 private:
  std::string text_;
};

// Recording display for the launcher's right-pane decorator.
class Adafruit_SSD1306 {
 public:
  std::string printed;
  void setTextSize(int) {}
  void setTextColor(int) {}
  void setCursor(int, int) {}
  void print(const char* text) {
    printed += text;
    printed += '|';
  }
};

// INSERT_TYPES

// ---------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------
using TaskHandle_t = void*;
using TransportSessionEpoch = uint32_t;
constexpr TransportSessionEpoch kNoTransportSessionEpoch = 0;

bool oledIsGuestSession();
bool oledSessionIsAdmin();
bool oledModeAllowedForGuest(OLEDMode mode);
MenuAvailability getMenuAvailability(OLEDMode mode, String* outReason);

struct Account {
  std::string role;
  bool banned;
};
std::map<std::string, Account> gRoster;
bool gRosterReadable = true;
int gRoleReads = 0;
int gAdminReads = 0;
int gReadsUnderWriter = 0;
int gWriterHeld = 0;

static int rosterReads() { return gRoleReads + gAdminReads; }

// Same contract as System_User.cpp: fail closed on an unreadable roster, an
// unknown or banned account; the role comes back lowercase.
bool getUserAuthorizationRole(const String& username, String& roleOut) {
  ++gRoleReads;
  if (gWriterHeld) ++gReadsUnderWriter;
  roleOut = String();
  if (!gRosterReadable || username.length() == 0) return false;
  const auto it = gRoster.find(username.c_str());
  if (it == gRoster.end() || it->second.banned) return false;
  roleOut = it->second.role.c_str();
  return true;
}

// Like adminFromRosterJson, the raw scan ignores "banned".
bool isAdminUser(const String& who) {
  ++gAdminReads;
  if (gWriterHeld) ++gReadsUnderWriter;
  if (!gRosterReadable || who.length() == 0) return false;
  const auto it = gRoster.find(who.c_str());
  return it != gRoster.end() &&
         (it->second.role == "admin" || it->second.role == "superadmin");
}

std::atomic<uint32_t> gIdentityGeneration{1};
bool gLocalDisplayAuthed = false;
String gLocalDisplayUser;

int gOwnerToken = 0;
int gForeignToken = 0;
TaskHandle_t gCurrentTask = &gOwnerToken;
TaskHandle_t xTaskGetCurrentTaskHandle() { return gCurrentTask; }
std::atomic<TaskHandle_t> sOledOwnerTask{&gOwnerToken};

int gToasts = 0;
std::string gLastToast;
void oledToastShow(const char* message, uint32_t /*durationMs*/) {
  ++gToasts;
  gLastToast = message ? message : "";
}

// Session mirror plumbing used by oledApplyPendingSessionBoundary.
std::atomic<uint32_t> sOledSessionBoundaryRequested{0};
uint32_t sOledSessionBoundaryApplied = 0;
bool sOledSessionMirrorInitialized = false;
TransportSessionEpoch sOledUiSessionEpoch = kNoTransportSessionEpoch;
bool sOledUiSessionAuthed = false;

std::string gAuthorityUser;
bool gAuthorityAuthed = false;
uint32_t gAuthorityEpoch = 0;
TransportSessionEpoch localDisplayTransportSessionBeginUiSync(String& user, bool& authed) {
  ++gWriterHeld;
  user = gAuthorityUser.c_str();
  authed = gAuthorityAuthed;
  return gAuthorityEpoch;
}
void localDisplayTransportSessionEndUiSync() { --gWriterHeld; }
void secureClearString(String& value) { value.clear(); }
void resetOLEDTranscription() {}
void oledResetLocalDisplaySessionTransients() {}
void oledMarkDirty() {}
void tryAutoStartInputForMenu() {}
bool gRequireAuth = false;
bool shouldBlockForDisplayAuth() { return gRequireAuth && !gLocalDisplayAuthed; }
bool gOledRunning = true;
bool oledConnected = true;
void* server = nullptr;
// No mode module registers an entry rule here (test_oled_mode_availability
// covers them): every mode passes its own rule.
bool oledModeRuleAllows(OLEDMode /*mode*/, String* /*outReason*/) { return true; }

OLEDMode currentOLEDMode = OLED_SYSTEM_STATUS;
int gModeRequests = 0;
bool gLastGateGuest = false;
// Mirrors requestOLEDMode's guest gate: the boundary's own navigation must see
// the role of the identity it just installed.
void requestOLEDMode(OLEDMode newMode, const char* /*reason*/, bool /*pushStack*/ = true,
                     bool /*isBackNav*/ = false) {
  ++gModeRequests;
  gLastGateGuest = oledIsGuestSession();
  if (gLastGateGuest && !oledModeAllowedForGuest(newMode)) newMode = OLED_MENU;
  currentOLEDMode = newMode;
}

static bool oledIsOwnerTask() {
  return sOledOwnerTask.load(std::memory_order_acquire) == xTaskGetCurrentTaskHandle();
}

// Scroll primitives (OLED_Utils.cpp) reduced to the list bookkeeping.
void oledScrollInit(OLEDScrollState* state, const char* title, int visibleLines) {
  *state = OLEDScrollState{};
  state->title = title;
  state->visibleLines = visibleLines;
}
void oledScrollSetSplitPane(OLEDScrollState* state, int listWidth, int separatorX, int iconSize) {
  state->listWidth = listWidth;
  state->separatorX = separatorX;
  state->iconSize = iconSize;
}
void oledScrollClearKeepSelection(OLEDScrollState* state) { state->itemCount = 0; }
bool oledScrollAddItem(OLEDScrollState* state, const char* line1, const char* line2,
                       bool selectable, void* userData) {
  if (state->itemCount >= OLED_SCROLL_MAX_ITEMS) return false;
  OLEDScrollItem& item = state->items[state->itemCount++];
  item = OLEDScrollItem{};
  item.line1 = line1;
  item.line2 = line2;
  item.isSelectable = selectable;
  item.userData = userData;
  return true;
}
void oledScrollClampSelection(OLEDScrollState* state) {
  if (state->itemCount <= 0) {
    state->selectedIndex = 0;
  } else if (state->selectedIndex >= state->itemCount) {
    state->selectedIndex = state->itemCount - 1;
  } else if (state->selectedIndex < 0) {
    state->selectedIndex = 0;
  }
}

// Launcher fixture: System (guest-visible), Config (Users first), Power (no
// guest-visible item).
int oledMenuSelectedIndex = 0;
int oledMenuCategorySelected = -1;
int oledMenuCategoryItemIndex = 0;
const OLEDMenuItem oledMenuCategories[] = {
    {"System", "system", (OLEDMode)0}, {"Config", "settings", (OLEDMode)1},
    {"Power", "power", (OLEDMode)2}};
const int oledMenuCategoryCount = 3;
const OLEDMenuItem oledMenuCategory0[] = {{"Status", "status", OLED_SYSTEM_STATUS},
                                          {"Memory", "memory", OLED_MEMORY_STATS}};
const OLEDMenuItem oledMenuCategory1[] = {{"Users", "user", OLED_USER_MANAGER},
                                          {"Settings", "settings", OLED_SETTINGS},
                                          {"Notifications", "notify", OLED_NOTIFICATIONS}};
const OLEDMenuItem oledMenuCategory2[] = {{"Power", "power", OLED_POWER},
                                          {"Sleep", "sleep", OLED_POWER_SLEEP}};
const OLEDMenuItem oledMenuCategory3[] = {{"Unused", "x", OLED_LOGO}};
const OLEDMenuItem oledMenuCategory4[] = {{"Unused", "x", OLED_LOGO}};
const OLEDMenuItem oledMenuCategory5[] = {{"Unused", "x", OLED_LOGO}};
const int oledMenuCategory0Count = 2;
const int oledMenuCategory1Count = 3;
const int oledMenuCategory2Count = 2;
const int oledMenuCategory3Count = 1;
const int oledMenuCategory4Count = 1;
const int oledMenuCategory5Count = 1;
void resetOLEDMenu() {
  oledMenuSelectedIndex = 0;
  oledMenuCategorySelected = -1;
  oledMenuCategoryItemIndex = 0;
}

// INSERT_OLED_UTILS

// INSERT_MENU

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

static std::string num(int value) { return std::to_string(value); }

static void resetRoster() {
  gRoster.clear();
  gRoster["alice"] = {"user", false};
  gRoster["root"] = {"superadmin", false};
  gRoster["ada"] = {"admin", false};
  gRoster["gina"] = {"guest", false};
  gRoster["bob"] = {"admin", true};  // banned; revocation boundary not applied yet
  gRosterReadable = true;
}

// Publish an authority change the way System_User does (bump `requested`) and
// let the owner apply it. nullptr = logout.
static void applyBoundary(const char* user) {
  gAuthorityUser = user ? user : "";
  gAuthorityAuthed = user != nullptr;
  ++gAuthorityEpoch;
  sOledSessionBoundaryRequested.fetch_add(1, std::memory_order_release);
  currentOLEDMode = OLED_SYSTEM_STATUS;  // so the boundary navigates to the menu
  gModeRequests = 0;
  oledApplyPendingSessionBoundary();
}

// Open the Config category with the cursor on its first row.
static void openConfig() {
  oledMenuCategorySelected = 1;
  oledMenuCategoryItemIndex = 0;
}

static Adafruit_SSD1306 gPanel;

// One launcher step, as the main loop runs it: the input handler and the
// render each rebuild the list, the render decorates the selected row, the
// mode's input handler asks oledGuestBlocksMutate, and a mode request passes
// the guest gate.
static void launcherStep() {
  mainMenuPopulate();  // mainMenuInputHandler
  mainMenuPopulate();  // displayMenuListStyle
  gPanel.printed.clear();
  if (sMainScroll.rightPaneDraw && sMainScroll.itemCount > 0) {
    sMainScroll.rightPaneDraw(&gPanel, &sMainScroll.items[sMainScroll.selectedIndex], 78, 12, 32);
  }
  (void)oledGuestBlocksMutate();
  (void)oledIsGuestSession();
}

static std::string visibleRows() {
  std::string rows;
  for (int i = 0; i < sMainScroll.itemCount; ++i) {
    rows += sMainScroll.items[i].line1;
    rows += ';';
  }
  return rows;
}

static MenuAvailability usersRow(std::string* reason = nullptr) {
  String text;
  const MenuAvailability availability = getMenuAvailability(OLED_USER_MANAGER, &text);
  if (reason) *reason = text.c_str();
  return availability;
}

// C1: a logged-in session reads the roster once per session, not per step.
static void caseLauncherCost() {
  resetRoster();
  gRoleReads = gAdminReads = 0;
  applyBoundary("alice");
  openConfig();
  launcherStep();
  const int firstStep = rosterReads();
  for (int i = 0; i < 9; ++i) launcherStep();
  const int total = rosterReads();
  check(total <= 2,
        "10 launcher steps as alice (user) at the Users row read users.json " + num(total) +
            " times (role " + num(gRoleReads) + ", admin " + num(gAdminReads) +
            "); the session role must be read at most once per session and identity generation");
  check(total == firstStep,
        "launcher steps after the first still read users.json (" + num(total - firstStep) +
            " reads in 9 steps)");
  check(visibleRows() == "Users;Settings;Notifications;",
        "alice's Config rows are '" + visibleRows() + "'");
  check(gPanel.printed == "D|Off|",
        "alice's Users row right pane printed '" + gPanel.printed + "', expected the Admin-only badge");
  check(!oledIsGuestSession(), "alice (user) is treated as a guest");
  check(usersRow() == MenuAvailability::FEATURE_DISABLED, "alice (user) gets the Users row");
}

// C2: fail closed for admin too: a banned admin is guest and non-admin.
static void caseBannedAdmin() {
  resetRoster();
  gToasts = 0;
  applyBoundary("bob");
  std::string reason;
  const MenuAvailability users = usersRow(&reason);
  check(users == MenuAvailability::FEATURE_DISABLED && reason == "Admin only",
        "banned admin bob (role lookup fails) still gets the Users row (availability " +
            num((int)users) + ", reason '" + reason + "'); a failed lookup must count as non-admin");
  check(oledIsGuestSession(), "banned bob is not treated as a guest");
  check(oledGuestBlocksMutate() && gToasts == 1 && gLastToast == "View-only",
        "banned bob's mutate is not blocked with the View-only toast");
}

// C3: a gIdentityGeneration change re-resolves the role, once.
static void caseGenerationBump() {
  resetRoster();
  applyBoundary("alice");
  openConfig();
  launcherStep();
  check(usersRow() == MenuAvailability::FEATURE_DISABLED, "alice (user) gets the Users row");

  gRoster["alice"].role = "admin";
  gIdentityGeneration.fetch_add(1, std::memory_order_release);
  gRoleReads = gAdminReads = 0;
  launcherStep();
  launcherStep();
  check(usersRow() == MenuAvailability::AVAILABLE,
        "alice promoted to admin (generation bumped) does not get the Users row");
  check(gPanel.printed == "Ready|", "promoted alice's Users pane printed '" + gPanel.printed + "'");
  check(rosterReads() <= 2,
        "after one generation bump, 2 launcher steps read users.json " + num(rosterReads()) +
            " times; expected one role and one admin read");

  gRoster["alice"].role = "guest";
  gIdentityGeneration.fetch_add(1, std::memory_order_release);
  gToasts = 0;
  oledMenuCategorySelected = -1;
  oledMenuSelectedIndex = 0;
  mainMenuPopulate();
  check(visibleRows() == "System >;Config >;",
        "demoted alice's categories are '" + visibleRows() + "' (Power has no guest row)");
  openConfig();
  mainMenuPopulate();
  check(visibleRows() == "Notifications;", "demoted alice's Config rows are '" + visibleRows() + "'");
  check(oledIsGuestSession(), "alice demoted to guest (generation bumped) is not a guest");
  check(oledGuestBlocksMutate() && gToasts == 1, "demoted alice's mutate is not blocked");
  check(usersRow() == MenuAvailability::FEATURE_DISABLED, "demoted alice still gets the Users row");
}

// C4: the session boundary drops the role with the identity.
static void caseSessionBoundary() {
  resetRoster();
  gReadsUnderWriter = 0;
  applyBoundary("root");
  openConfig();
  launcherStep();
  check(usersRow() == MenuAvailability::AVAILABLE, "superadmin root does not get the Users row");
  check(!gLastGateGuest, "the boundary's menu request saw root as a guest");

  applyBoundary("gina");  // no generation bump: login of another account
  check(gModeRequests == 1 && gLastGateGuest,
        "the boundary's menu request did not see the new identity gina as a guest");
  check(oledIsGuestSession(), "gina (guest) after root is not a guest: the role outlived the boundary");
  check(usersRow() == MenuAvailability::FEATURE_DISABLED,
        "gina (guest) after root gets the Users row: the admin flag outlived the boundary");
  openConfig();
  mainMenuPopulate();
  check(visibleRows() == "Notifications;", "gina's Config rows are '" + visibleRows() + "'");

  gRoleReads = gAdminReads = 0;
  gToasts = 0;
  applyBoundary(nullptr);  // logout
  std::string reason;
  check(!oledIsGuestSession(), "a logged-out display is treated as a guest");
  check(usersRow(&reason) == MenuAvailability::FEATURE_DISABLED && reason == "Admin only",
        "a logged-out display gets the Users row");
  check(!oledGuestBlocksMutate() && gToasts == 0, "a logged-out display's mutate is blocked");
  check(rosterReads() == 0, "a logged-out display read users.json " + num(rosterReads()) + " times");

  applyBoundary("root");
  check(usersRow() == MenuAvailability::AVAILABLE, "root logging in again does not get the Users row");
  check(gRoleReads >= 1, "root's new session did not resolve the role");
  check(gReadsUnderWriter == 0,
        "users.json was read " + num(gReadsUnderWriter) +
            " times while the display lifecycle writer was held (FS -> writer lock order)");
}

// C5: a failed lookup is guest and non-admin, and is retried.
static void caseFailClosed() {
  resetRoster();
  applyBoundary("mallory");  // no such account
  check(oledIsGuestSession(), "unknown account mallory is not treated as a guest");
  check(usersRow() == MenuAvailability::FEATURE_DISABLED, "unknown account mallory gets the Users row");

  gRosterReadable = false;
  applyBoundary("ada");
  check(oledIsGuestSession(), "admin ada with an unreadable roster is not treated as a guest");
  check(usersRow() == MenuAvailability::FEATURE_DISABLED,
        "admin ada with an unreadable roster gets the Users row");
  gRosterReadable = true;  // the read failure was transient; no generation bump
  check(!oledIsGuestSession(), "admin ada stays a guest after the roster became readable again");
  check(usersRow() == MenuAvailability::AVAILABLE,
        "admin ada does not get the Users row after the roster became readable again");
}

// C6: other tasks (cmd_oledmode on cmd_exec) read live and leave the cache alone.
static void caseForeignTask() {
  resetRoster();
  applyBoundary("alice");
  (void)oledIsGuestSession();  // owner fills the cache
  gRoleReads = gAdminReads = 0;
  gCurrentTask = &gForeignToken;
  for (int i = 0; i < 3; ++i) check(!oledIsGuestSession(), "foreign task sees alice as a guest");
  check(gRoleReads == 3 && gAdminReads == 0,
        "3 foreign-task guest checks did " + num(gRoleReads) + " role and " + num(gAdminReads) +
            " admin reads; expected one live role read each");
  gRoster["alice"].role = "guest";  // live view only; no bump
  check(oledIsGuestSession(), "foreign task does not read the role live");
  gRoster["alice"].role = "user";
  gCurrentTask = &gOwnerToken;
  gRoleReads = gAdminReads = 0;
  check(!oledIsGuestSession(), "owner sees alice as a guest after foreign checks");
  check(rosterReads() == 0,
        "owner read users.json " + num(rosterReads()) + " times after foreign checks; the cache was "
        "not kept for the session");
}

// C7: unauthenticated (AuthBypass) display: launcher unfiltered, no reads.
static void caseUnauthenticated() {
  resetRoster();
  applyBoundary(nullptr);
  gRoleReads = gAdminReads = 0;
  oledMenuCategorySelected = -1;
  mainMenuPopulate();
  check(visibleRows() == "System >;Config >;Power >;",
        "unauthenticated categories are '" + visibleRows() + "'");
  openConfig();
  launcherStep();
  check(visibleRows() == "Users;Settings;Notifications;",
        "unauthenticated Config rows are '" + visibleRows() + "'");
  check(rosterReads() == 0, "an unauthenticated launcher read users.json " + num(rosterReads()) + " times");
}

int main() {
  caseLauncherCost();
  caseBannedAdmin();
  caseGenerationBump();
  caseSessionBoundary();
  caseFailClosed();
  caseForeignTask();
  caseUnauthenticated();
  std::printf("oled_session_role: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
