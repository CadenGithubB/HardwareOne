// ============================================================================
// OLED User Manager Mode (admin) — Config → Users
// ============================================================================
// Admin-only user manager: a scrollable user list, a per-user action menu
// (Change Role / Delete), and an Add-User wizard (username → password → role).
// Read-only display parses /system/users/users.json; every mutation dispatches
// a real command (useradd / userdelete confirm / userpromote / userdemote)
// through executeOLEDCommandWithResult so the rank/admin gating in the command
// layer applies and we never touch the user store directly.

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include "OLED_Utils.h"
#include "System_Debug.h"
#include "System_User.h"      // isAdminUser / isSuperAdminUser / userRoleRank / USERS_JSON_FILE / gLocalDisplay*
#include "System_Utils.h"     // readText
#include "System_MemUtil.h"   // PSRAM_JSON_DOC

extern DisplayDriver* oledDisplay;

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------
enum UmLevel : uint8_t { UM_LIST, UM_ACTIONS, UM_ROLE, UM_ADD_ROLE };
static UmLevel gUmLevel = UM_LIST;

#define UM_MAX_USERS 16
struct UmUser { char name[24]; char role[12]; long id; };
EXT_RAM_BSS_ATTR static UmUser gUsers[UM_MAX_USERS];
static int    gUmCount = 0;

static int gUmSel    = 0;   // list cursor (0..gUmCount; gUmCount == "+ Add User")
static int gUmScroll = 0;
static int gUmTarget = -1;  // selected user index for ACTIONS / ROLE
static int gUmActSel = 0;   // action-menu cursor
static int gUmRoleSel = 0;  // role-picker cursor

static bool    gUmKbActive = false;
static uint8_t gUmKbStage  = 0;   // 0 = username, 1 = password
static char    gAddUser[33] = {0};
static char    gAddPass[65] = {0};
static char    gUmDelName[24] = {0};   // captured at delete-confirm request

static String        umError;
static unsigned long umErrorUntil = 0;

static const char* const kUmRoles[4] = { "guest", "user", "admin", "superadmin" };

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------
static bool umIsAdmin() { return gLocalDisplayAuthed && isAdminUser(gLocalDisplayUser); }
static int  umMaxRank() { return isSuperAdminUser(gLocalDisplayUser) ? 3 : 2; }

static void umShowError(const char* msg) {
  umError = String(msg).substring(0, 40);
  umErrorUntil = millis() + 2500;
  oledMarkDirtyUntil(umErrorUntil);
}

static void loadUsers() {
  gUmCount = 0;
  String json;
  if (!readText(USERS_JSON_FILE, json)) return;
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json)) return;
  JsonArrayConst users = doc["users"].as<JsonArrayConst>();
  if (users.isNull()) return;
  for (JsonObjectConst u : users) {
    if (gUmCount >= UM_MAX_USERS) break;
    const char* nm = u["username"] | "";
    if (nm[0] == '\0') continue;
    UmUser& r = gUsers[gUmCount];
    strncpy(r.name, nm, sizeof(r.name) - 1); r.name[sizeof(r.name) - 1] = '\0';
    const char* role = u["role"] | "user";
    strncpy(r.role, role, sizeof(r.role) - 1); r.role[sizeof(r.role) - 1] = '\0';
    r.id = u["id"] | 0L;
    gUmCount++;
  }
}

// Synchronous dispatch (submitAndExecuteSync under the OLED identity). On
// failure, surface the Error: text. Always reload + return to the list.
static void umDispatch(const String& cmd) {
  char out[96];
  bool ok = executeOLEDCommandWithResult(cmd, out, sizeof(out));
  if (!ok || strncmp(out, "Error", 5) == 0) {
    const char* m = out[0] ? out : "Failed";
    if (strncmp(m, "Error: ", 7) == 0) m += 7;
    umShowError(m);
  }
  loadUsers();
  gUmLevel = UM_LIST;
  if (gUmSel > gUmCount) gUmSel = gUmCount;
}

// Deferred delete confirm callback (oledConfirmRequest).
static void umDeleteConfirmed(void* /*ud*/) {
  if (gUmDelName[0]) umDispatch(String("userdelete \"") + gUmDelName + "\" confirm");
}

// ----------------------------------------------------------------------------
// Display
// ----------------------------------------------------------------------------
static void umDrawRole(int y, int cursor, int maxRank, int curRank, const char* header) {
  oledDisplay->setCursor(0, y);
  oledDisplay->print(header);
  int yy = y + 11;
  for (int rank = 0; rank <= maxRank; rank++) {
    oledDisplay->setCursor(0, yy);
    oledDisplay->print(rank == cursor ? "> " : "  ");
    oledDisplay->print(kUmRoles[rank]);
    if (rank == curRank) oledDisplay->print(" *");
    yy += 10;
  }
}

void displayUserManager() {
  if (!oledDisplay) return;
  if (oledKeyboardDrawIfActive(oledDisplay)) return;   // keyboard curtain

  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  int y = OLED_CONTENT_START_Y;

  if (!umIsAdmin()) {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("Admin only");
    oledDisplay->setCursor(0, y + 12);
    oledDisplay->print("Log in as admin");
    return;
  }

  // Error banner (bottom strip) — drawn over whatever level is active.
  const bool showErr = (umError.length() && (long)(umErrorUntil - millis()) > 0);

  if (gUmLevel == UM_LIST) {
    const int total = gUmCount + 1;   // + Add User row
    const int lineH = 10, visible = 4;
    if (gUmSel < gUmScroll) gUmScroll = gUmSel;
    if (gUmSel >= gUmScroll + visible) gUmScroll = gUmSel - visible + 1;
    for (int row = 0; row < visible; row++) {
      int i = gUmScroll + row;
      if (i >= total) break;
      oledDisplay->setCursor(0, y + row * lineH);
      oledDisplay->print(i == gUmSel ? "> " : "  ");
      if (i == gUmCount) {
        oledDisplay->print("+ Add User");
      } else {
        char buf[22];
        snprintf(buf, sizeof(buf), "%.11s (%.5s)", gUsers[i].name, gUsers[i].role);
        oledDisplay->print(buf);
      }
    }
    if (gUmScroll > 0) { oledDisplay->setCursor(122, y); oledDisplay->print("\x18"); }
    if (gUmScroll + visible < total) {
      oledDisplay->setCursor(122, y + OLED_CONTENT_HEIGHT - 8); oledDisplay->print("\x19");
    }
  } else if (gUmLevel == UM_ACTIONS) {
    const UmUser& u = gUsers[gUmTarget];
    oledDisplay->setCursor(0, y);
    char hdr[22]; snprintf(hdr, sizeof(hdr), "%.11s (%.5s)", u.name, u.role);
    oledDisplay->print(hdr);
    const bool founder = (u.id == 1);
    const char* acts[3] = { "Change Role", "Delete", "< Back" };
    int yy = y + 12;
    for (int i = 0; i < 3; i++) {
      oledDisplay->setCursor(0, yy);
      oledDisplay->print(i == gUmActSel ? "> " : "  ");
      if (founder && i < 2) { oledDisplay->print(i == 0 ? "Change Role -" : "Delete -"); }
      else oledDisplay->print(acts[i]);
      yy += 10;
    }
    if (founder) { oledDisplay->setCursor(0, yy); oledDisplay->print("(founder locked)"); }
  } else if (gUmLevel == UM_ROLE) {
    umDrawRole(y, gUmRoleSel, umMaxRank(), userRoleRank(gUsers[gUmTarget].role), "Set role:");
  } else if (gUmLevel == UM_ADD_ROLE) {
    umDrawRole(y, gUmRoleSel, umMaxRank(), -1, "New user role:");
  }

  if (showErr) {
    int ey = OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 9;
    oledDisplay->fillRect(0, ey, 128, 9, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    oledDisplay->setCursor(1, ey + 1);
    oledDisplay->print(umError);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  }
}

// ----------------------------------------------------------------------------
// Input
// ----------------------------------------------------------------------------
static void umStartKeyboard(uint8_t stage, const char* title, int maxLen) {
  gUmKbStage = stage;
  oledKeyboardInit(title, "", maxLen);
  gUmKbActive = true;
}

bool userManagerInput(int /*dx*/, int /*dy*/, uint32_t newlyPressed) {
  // Keyboard steps (Add-User wizard).
  if (gUmKbActive) {
    if (oledKeyboardIsCompleted()) {
      const char* text = oledKeyboardGetText();
      if (gUmKbStage == 0) {           // username → password
        strncpy(gAddUser, text ? text : "", sizeof(gAddUser) - 1);
        gAddUser[sizeof(gAddUser) - 1] = '\0';
        oledKeyboardReset();
        if (gAddUser[0] == '\0') { gUmKbActive = false; gUmLevel = UM_LIST; return true; }
        umStartKeyboard(1, "Password", 64);
      } else {                          // password → role picker
        strncpy(gAddPass, text ? text : "", sizeof(gAddPass) - 1);
        gAddPass[sizeof(gAddPass) - 1] = '\0';
        oledKeyboardReset();
        gUmKbActive = false;
        if (gAddPass[0] == '\0') { gUmLevel = UM_LIST; return true; }
        gUmRoleSel = 1;                 // default "user"
        gUmLevel = UM_ADD_ROLE;
      }
      return true;
    }
    if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      gUmKbActive = false;
      gUmLevel = UM_LIST;
      return true;
    }
    return true;  // keyboard owns input while active
  }

  if (oledGuestBlocksMutate()) return true;
  if (!umIsAdmin()) return false;  // B falls through to back

  if (gUmLevel == UM_LIST) {
    const int total = gUmCount + 1;
    if (gNavEvents.up   && gUmSel > 0)          { gUmSel--; return true; }
    if (gNavEvents.down && gUmSel < total - 1)  { gUmSel++; return true; }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      if (gUmSel == gUmCount) {          // + Add User
        umStartKeyboard(0, "New Username", 32);
      } else {
        gUmTarget = gUmSel;
        gUmActSel = 0;
        gUmLevel = UM_ACTIONS;
      }
      return true;
    }
    return false;  // B → back out of the mode
  }

  if (gUmLevel == UM_ACTIONS) {
    if (gNavEvents.up   && gUmActSel > 0) { gUmActSel--; return true; }
    if (gNavEvents.down && gUmActSel < 2) { gUmActSel++; return true; }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      const bool founder = (gUsers[gUmTarget].id == 1);
      if (gUmActSel == 2) { gUmLevel = UM_LIST; return true; }   // < Back
      if (founder) { umShowError("Founder is protected"); return true; }
      if (gUmActSel == 0) {              // Change Role
        gUmRoleSel = userRoleRank(gUsers[gUmTarget].role);
        if (gUmRoleSel > umMaxRank()) gUmRoleSel = umMaxRank();
        gUmLevel = UM_ROLE;
      } else {                            // Delete
        strncpy(gUmDelName, gUsers[gUmTarget].name, sizeof(gUmDelName) - 1);
        gUmDelName[sizeof(gUmDelName) - 1] = '\0';
        oledConfirmRequest("Delete user?", gUmDelName, umDeleteConfirmed, nullptr, false);
      }
      return true;
    }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) { gUmLevel = UM_LIST; return true; }
    return true;  // stay in the action menu
  }

  if (gUmLevel == UM_ROLE || gUmLevel == UM_ADD_ROLE) {
    const int maxR = umMaxRank();
    if (gNavEvents.up   && gUmRoleSel > 0)    { gUmRoleSel--; return true; }
    if (gNavEvents.down && gUmRoleSel < maxR) { gUmRoleSel++; return true; }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      if (gUmLevel == UM_ROLE) {
        const UmUser& u = gUsers[gUmTarget];
        int cur = userRoleRank(u.role);
        if (gUmRoleSel == cur) { gUmLevel = UM_ACTIONS; return true; }  // no change
        String cmd = String(gUmRoleSel > cur ? "userpromote " : "userdemote ") + u.name + " " + kUmRoles[gUmRoleSel];
        umDispatch(cmd);
      } else {                            // UM_ADD_ROLE → useradd
        String cmd = String("useradd \"") + gAddUser + "\" \"" + gAddPass + "\" 0 " + kUmRoles[gUmRoleSel];
        umDispatch(cmd);
      }
      return true;
    }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      gUmLevel = (gUmLevel == UM_ROLE) ? UM_ACTIONS : UM_LIST;
      return true;
    }
    return true;
  }

  return false;
}

// Fresh view on forward entry.
static void userManagerOnEnter(bool isForward) {
  if (!isForward) return;
  gUmLevel = UM_LIST;
  gUmSel = 0; gUmScroll = 0; gUmTarget = -1;
  gUmKbActive = false;
  umError = "";
  loadUsers();
}

static bool userManagerAvailable(String* /*reason*/) { return true; }

static const OLEDModeEntry userManagerModes[] = {
  { OLED_USER_MANAGER, "User Manager", "user", displayUserManager,
    userManagerAvailable, userManagerInput, false, -1, "A:Select B:Back", userManagerOnEnter },
};

REGISTER_OLED_MODE_MODULE(userManagerModes, sizeof(userManagerModes) / sizeof(userManagerModes[0]), "UserManager");

// Force linker to include this file — called from OLED_Utils.cpp init block.
void oledUserManagerModeInit() {}

#endif // ENABLE_OLED_DISPLAY
