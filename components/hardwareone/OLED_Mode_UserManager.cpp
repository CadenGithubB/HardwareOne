// ============================================================================
// OLED User Manager Mode (admin) — Config → Users
// ============================================================================
// Admin-only user manager: a scrollable user list, a per-user action menu
// (Change Role / Delete), and an Add-User wizard (username → password → role).
// The list comes from System_User's read-only roster snapshot (full names,
// normalized roles); every mutation dispatches a real command (useradd /
// userdelete confirm / userpromote / userdemote) with the full username
// through executeOledCommandWithResult so the rank/admin gating in the command
// layer applies and we never touch the user store directly. Only the on-screen
// label is shortened.

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "OLED_TextField.h"
#include "OLED_UI.h"
#include "System_Debug.h"
#include "System_Mutex.h"
#include "System_User.h"      // readUserRoster / isAdminUser / isSuperAdminUser / userRoleRank / gLocalDisplay*
#include "System_Utils.h"

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------
enum UmLevel : uint8_t { UM_LIST, UM_ACTIONS, UM_ROLE, UM_ADD_ROLE };
static UmLevel sUmLevel = UM_LIST;

#define UM_MAX_USERS 16
EXT_RAM_BSS_ATTR static UserRosterEntry gUsers[UM_MAX_USERS];
static int    sUmCount = 0;

static int sUmSel    = 0;   // list cursor (0..sUmCount; sUmCount == "+ Add User")
static int sUmScroll = 0;
static int sUmTarget = -1;  // selected user index for ACTIONS / ROLE
static int sUmActSel = 0;   // action-menu cursor
static int sUmRoleSel = 0;  // role-picker cursor

static bool    sUmKbActive = false;
static uint8_t sUmKbStage  = 0;   // 0 = username, 1 = password
static char    sAddUser[33] = {0};
static char    sAddPass[65] = {0};
static char    sUmDelName[kPublicUsernameMaxLen + 1] = {0};   // full name, captured at delete-confirm request


static void umWipe(char* buffer, size_t size) {
  volatile char* p = reinterpret_cast<volatile char*>(buffer);
  while (size-- > 0) *p++ = '\0';
}

void oledUserManagerModeResetSessionState() {
  memset(gUsers, 0, sizeof(gUsers));
  sUmCount = 0;
  sUmLevel = UM_LIST;
  sUmSel = 0;
  sUmScroll = 0;
  sUmTarget = -1;
  sUmActSel = 0;
  sUmRoleSel = 0;
  sUmKbActive = false;
  sUmKbStage = 0;
  umWipe(sAddUser, sizeof(sAddUser));
  umWipe(sAddPass, sizeof(sAddPass));
  umWipe(sUmDelName, sizeof(sUmDelName));
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------
static bool umIsAdmin() { return gLocalDisplayAuthed && isAdminUser(gLocalDisplayUser); }
static int  umMaxRank() { return isSuperAdminUser(gLocalDisplayUser) ? 3 : 2; }

static void umShowError(const char* msg) {
  oledToastShow(msg, oled_message::kErrorDurationMs, OledUiIcon::ERROR);
}

static void loadUsers() {
  size_t count = 0;
  (void)readUserRoster(gUsers, UM_MAX_USERS, count);
  sUmCount = (int)count;
}

// Synchronous dispatch (submitAndExecuteSync under the OLED identity). On
// failure, surface the Error: text. Always reload + return to the list.
static void umDispatch(const String& cmd) {
  String sessionUser;
  bool sessionAuthed = false;
  const TransportSessionEpoch sessionEpoch =
      localDisplayTransportSessionSnapshot(sessionUser, sessionAuthed);
  secureClearString(sessionUser);
  if (!sessionAuthed || sessionEpoch == kNoTransportSessionEpoch) return;

  char out[96] = {};
  bool ok = executeOledCommandWithResultForSession(
      cmd, sessionEpoch, out, sizeof(out));
  // Re-check authority before publishing owner-only UI state. Then take FS for
  // the users.json refresh without a display lifecycle lease; credential
  // publication uses FS -> writer and must never deadlock against writer -> FS.
  if (!transportSessionEpochIsLive(
          SOURCE_LOCAL_DISPLAY, sessionEpoch)) {
    umWipe(out, sizeof(out));
    return;
  }
  FsLockGuard fsGuard("oled.user_manager.result");
  if (!ok) {
    // The shared short refusal ("Needs a super admin" for userdelete,
    // userdemote and userresetpassword under an admin).
    char text[sizeof(out)];
    oledCommandReplyText(false, out, nullptr, "Failed", text, sizeof(text));
    umShowError(text);
    umWipe(text, sizeof(text));
  }
  loadUsers();
  sUmLevel = UM_LIST;
  if (sUmSel > sUmCount) sUmSel = sUmCount;
  umWipe(sAddUser, sizeof(sAddUser));
  umWipe(sAddPass, sizeof(sAddPass));
  umWipe(out, sizeof(out));
}

// Deferred delete confirm callback (oledConfirmRequest).
static void umDeleteConfirmed(void* /*ud*/) {
  if (sUmDelName[0]) umDispatch(String("userdelete \"") + sUmDelName + "\" confirm");
}

// ----------------------------------------------------------------------------
// Display
// ----------------------------------------------------------------------------
// One roster row: the username field from `x` up to the role badge, which is
// right-aligned in a fixed column before the scroll arrows and never moves.
static void umDrawUserRow(int x, int y, const UserRosterEntry& u, bool selected) {
  char role[16];
  snprintf(role, sizeof(role), "(%s)", u.role);
  int roleW = (int)strlen(role) * 6;
  if (roleW > 60) roleW = 60;
  const int roleX = SCREEN_WIDTH - 8 - roleW;
  oledTextFieldDraw(oledDisplay, x, y, roleX - 6 - x, u.username,
                    DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE, selected, gUsers);
  oledTextFieldDraw(oledDisplay, roleX, y, roleW, role,
                    DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE, false, gUsers);
}

static void umDrawRole(int y, int cursor, int maxRank, int curRank, const char* header) {
  oledDisplay->setCursor(0, y);
  oledDisplay->print(header);
  int yy = y + 11;
  for (int rank = 0; rank <= maxRank; rank++) {
    oledDisplay->setCursor(0, yy);
    oledDisplay->print(rank == cursor ? "> " : "  ");
    // Role label in a bounded field; the current-role marker keeps its column.
    oledTextFieldDraw(oledDisplay, 12, yy, SCREEN_WIDTH - 14 - 12 - 6, kUserRoleNames[rank],
                      DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE, rank == cursor, kUserRoleNames);
    if (rank == curRank) {
      oledDisplay->setCursor(SCREEN_WIDTH - 14, yy);
      oledDisplay->print("*");
    }
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

  if (sUmLevel == UM_LIST) {
    const int total = sUmCount + 1;   // + Add User row
    const int lineH = 10, visible = oledVisibleRows(lineH, 0, 4);
    if (sUmSel < sUmScroll) sUmScroll = sUmSel;
    if (sUmSel >= sUmScroll + visible) sUmScroll = sUmSel - visible + 1;
    for (int row = 0; row < visible; row++) {
      int i = sUmScroll + row;
      if (i >= total) break;
      const int ry = y + row * lineH;
      const bool selected = i == sUmSel;
      oledDisplay->setCursor(0, ry);
      oledDisplay->print(selected ? "> " : "  ");
      if (i == sUmCount) {
        oledTextFieldDraw(oledDisplay, 12, ry, SCREEN_WIDTH - 6 - 12, "+ Add User",
                          DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE, false, gUsers);
      } else {
        // The whole username in its own field (scrolling when selected); the
        // role badge keeps a fixed right column before the arrows.
        umDrawUserRow(12, ry, gUsers[i], selected);
      }
    }
    if (sUmScroll > 0) { oledDisplay->setCursor(SCREEN_WIDTH - 6, y); oledDisplay->print("\x18"); }
    if (sUmScroll + visible < total) {
      oledDisplay->setCursor(SCREEN_WIDTH - 6, y + OLED_CONTENT_HEIGHT - 8); oledDisplay->print("\x19");
    }
  } else if (sUmLevel == UM_ACTIONS) {
    const UserRosterEntry& u = gUsers[sUmTarget];
    umDrawUserRow(0, y, u, false);
    const bool founder = (u.id == 1);
    const char* acts[3] = { "Change Role", "Delete", "< Back" };
    int yy = y + 12;
    for (int i = 0; i < 3; i++) {
      oledDisplay->setCursor(0, yy);
      oledDisplay->print(i == sUmActSel ? "> " : "  ");
      const char* act = (founder && i < 2) ? (i == 0 ? "Change Role -" : "Delete -") : acts[i];
      oledTextFieldDraw(oledDisplay, 12, yy, SCREEN_WIDTH - 12, act,
                        DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE, i == sUmActSel, acts);
      yy += 10;
    }
    if (founder) { oledDisplay->setCursor(0, yy); oledDisplay->print("(founder locked)"); }
  } else if (sUmLevel == UM_ROLE) {
    umDrawRole(y, sUmRoleSel, umMaxRank(), userRoleRank(gUsers[sUmTarget].role), "Set role:");
  } else if (sUmLevel == UM_ADD_ROLE) {
    umDrawRole(y, sUmRoleSel, umMaxRank(), -1, "New user role:");
  }


}

// ----------------------------------------------------------------------------
// Input
// ----------------------------------------------------------------------------
static void umStartKeyboard(uint8_t stage, const char* title, int maxLen) {
  sUmKbStage = stage;
  // Stage 0 is a public username; stage 1 is the new account password, which
  // denies dictation and opens masked (one '*' per character).
  const OledKeyboardDictationPolicy policy =
      stage == 0 ? OledKeyboardDictationPolicy::ALLOW_PLAINTEXT
                 : OledKeyboardDictationPolicy::DENY;
  oledKeyboardInit(title, "", maxLen, policy, stage != 0);
  sUmKbActive = true;
}

bool userManagerInput(int /*dx*/, int /*dy*/, uint32_t newlyPressed) {
  // Keyboard steps (Add-User wizard).
  if (sUmKbActive) {
    if (oledKeyboardIsCompleted()) {
      const char* text = oledKeyboardGetText();
      if (sUmKbStage == 0) {           // username → password
        strlcpy(sAddUser, text ? text : "", sizeof(sAddUser));
        oledKeyboardReset();
        if (sAddUser[0] == '\0') { sUmKbActive = false; sUmLevel = UM_LIST; return true; }
        umStartKeyboard(1, "Password", 64);
      } else {                          // password → role picker
        strlcpy(sAddPass, text ? text : "", sizeof(sAddPass));
        oledKeyboardReset();
        sUmKbActive = false;
        if (sAddPass[0] == '\0') { sUmLevel = UM_LIST; return true; }
        sUmRoleSel = 1;                 // default "user"
        sUmLevel = UM_ADD_ROLE;
      }
      return true;
    }
    if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      sUmKbActive = false;
      sUmLevel = UM_LIST;
      return true;
    }
    return true;  // keyboard owns input while active
  }

  if (oledGuestBlocksMutate()) return true;
  if (!umIsAdmin()) return false;  // B falls through to back

  if (sUmLevel == UM_LIST) {
    const int total = sUmCount + 1;
    if (gNavEvents.up   && sUmSel > 0)          { sUmSel--; return true; }
    if (gNavEvents.down && sUmSel < total - 1)  { sUmSel++; return true; }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      if (sUmSel == sUmCount) {          // + Add User
        umStartKeyboard(0, "New Username", 32);
      } else {
        sUmTarget = sUmSel;
        sUmActSel = 0;
        sUmLevel = UM_ACTIONS;
      }
      return true;
    }
    return false;  // B → back out of the mode
  }

  if (sUmLevel == UM_ACTIONS) {
    if (gNavEvents.up   && sUmActSel > 0) { sUmActSel--; return true; }
    if (gNavEvents.down && sUmActSel < 2) { sUmActSel++; return true; }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      const bool founder = (gUsers[sUmTarget].id == 1);
      if (sUmActSel == 2) { sUmLevel = UM_LIST; return true; }   // < Back
      if (founder) { umShowError("Founder is protected"); return true; }
      if (sUmActSel == 0) {              // Change Role
        sUmRoleSel = userRoleRank(gUsers[sUmTarget].role);
        if (sUmRoleSel > umMaxRank()) sUmRoleSel = umMaxRank();
        sUmLevel = UM_ROLE;
      } else {                            // Delete
        strlcpy(sUmDelName, gUsers[sUmTarget].username, sizeof(sUmDelName));
        oledConfirmRequest("Delete user?", sUmDelName, umDeleteConfirmed, nullptr, false);
      }
      return true;
    }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) { sUmLevel = UM_LIST; return true; }
    return true;  // stay in the action menu
  }

  if (sUmLevel == UM_ROLE || sUmLevel == UM_ADD_ROLE) {
    const int maxR = umMaxRank();
    if (gNavEvents.up   && sUmRoleSel > 0)    { sUmRoleSel--; return true; }
    if (gNavEvents.down && sUmRoleSel < maxR) { sUmRoleSel++; return true; }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      if (sUmLevel == UM_ROLE) {
        const UserRosterEntry& u = gUsers[sUmTarget];
        int cur = userRoleRank(u.role);
        if (sUmRoleSel == cur) { sUmLevel = UM_ACTIONS; return true; }  // no change
        String cmd = String(sUmRoleSel > cur ? "userpromote " : "userdemote ") + u.username + " " + kUserRoleNames[sUmRoleSel];
        umDispatch(cmd);
      } else {                            // UM_ADD_ROLE → useradd
        String cmd = String("useradd \"") + sAddUser + "\" \"" + sAddPass + "\" 0 " + kUserRoleNames[sUmRoleSel];
        umDispatch(cmd);
        secureClearString(cmd);
      }
      return true;
    }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      sUmLevel = (sUmLevel == UM_ROLE) ? UM_ACTIONS : UM_LIST;
      return true;
    }
    return true;
  }

  return false;
}

// Fresh view on forward entry.
static void userManagerOnEnter(bool isForward) {
  if (!isForward) return;
  oledUserManagerModeResetSessionState();
  loadUsers();
}

static bool userManagerAvailable(String* /*reason*/) { return true; }

static const OledModeEntry kUserManagerModes[] = {
  { OLED_USER_MANAGER, "User Manager", "user", displayUserManager,
    userManagerAvailable, userManagerInput, false, -1, "A:Select B:Back", userManagerOnEnter },
};

REGISTER_OLED_MODE_MODULE(kUserManagerModes, sizeof(kUserManagerModes) / sizeof(kUserManagerModes[0]), "UserManager");

// Force linker to include this file — called from OLED_Utils.cpp init block.
void oledUserManagerModeInit() {}

#endif // ENABLE_OLED_DISPLAY
