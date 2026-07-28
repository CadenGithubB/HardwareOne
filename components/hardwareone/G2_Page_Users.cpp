// =============================================================================
// G2 glasses — "Users" App page implementation (admin user manager)
// =============================================================================
// See header. Read-only display parses /system/users/users.json each render
// (like the Automations page re-reads its JSON); every mutation dispatches a
// real command (useradd / userdelete confirm / userpromote / userdemote) via
// g2SubmitHijackCommand, so the rank/admin gating in the command layer applies
// and we never touch the user store directly. Completion re-renders the list.

#include "G2_Page_Users.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <Arduino.h>
#include <ArduinoJson.h>

#include "G2_Glasses.h"        // g2ShowListPage / g2ShowText / g2Set/GetHijackPage / g2BumpMenuGen
#include "G2_HijackCmd.h"      // G2CmdCookie / g2SubmitHijackCommand / g2HijackAuthContext
#include "G2_Page_TextEntry.h" // on-lens keyboard for Add User
#include "System_Utils.h"      // readText
#include "System_MemUtil.h"    // PSRAM_JSON_DOC
#include "System_User.h"       // USERS_JSON_FILE / isAdminUser / isSuperAdminUser / userRoleRank
#include "System_Debug.h"      // DEBUG_G2F
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern void g2ShowConfigMenu();  // return to the Config launcher on Back (menu reorg; non-static in G2_Glasses.cpp)

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
enum UsersSub : uint8_t {
  USERS_SUB_LIST = 0,
  USERS_SUB_DETAIL,
  USERS_SUB_ROLE,
  USERS_SUB_CONFIRM_DELETE,
  USERS_SUB_ADD_ROLE,
};
static UsersSub gSub = USERS_SUB_LIST;
static inline void setSub(UsersSub s) {
  if (gSub == s) return;
  gSub = s;
  g2BumpMenuGen();   // drop stale cmd_exec redraws from the previous sub-mode
}

#define G2_USERS_MAX 16
struct G2UserRow { char name[24]; char role[12]; long id; };
EXT_RAM_BSS_ATTR static G2UserRow gUsers[G2_USERS_MAX];
static size_t    gUserCount = 0;
static int       gSelected  = -1;   // index into gUsers while in DETAIL/ROLE/DELETE

// Add-user staging (filled by the keyboard commits).
static char gAddUser[33] = {0};
static char gAddPass[33] = {0};

// Role vocabulary, indexed by rank (guest=0 .. superadmin=3).
static const char* const kRoleNames[4] = { "guest", "user", "admin", "superadmin" };

static void showListMenu();
static void showDetailMenu();
static void showRoleMenu();
static void showDeleteConfirm();
static void showAddRoleMenu();

// -----------------------------------------------------------------------------
// Load users from JSON (display only)
// -----------------------------------------------------------------------------
static void loadUsers() {
  gUserCount = 0;
  String json;
  if (!readText(USERS_JSON_FILE, json)) return;   // no file / denied → empty
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json)) return;          // malformed → empty
  JsonArrayConst users = doc["users"].as<JsonArrayConst>();
  if (users.isNull()) return;
  for (JsonObjectConst u : users) {
    if (gUserCount >= G2_USERS_MAX) break;
    const char* nm = u["username"] | "";
    if (nm[0] == '\0') continue;
    G2UserRow& r = gUsers[gUserCount];
    strncpy(r.name, nm, sizeof(r.name) - 1); r.name[sizeof(r.name) - 1] = '\0';
    const char* role = u["role"] | "user";
    strncpy(r.role, role, sizeof(r.role) - 1); r.role[sizeof(r.role) - 1] = '\0';
    r.id = u["id"] | 0L;
    gUserCount++;
  }
}

// Max role rank the CURRENT pairer may grant (can't grant above your own; the
// command re-checks, this just hides un-grantable rows). Page is admin-gated,
// so the pairer is at least admin (rank 2).
static int pairerMaxRank() {
  return isSuperAdminUser(g2HijackAuthContext().user) ? 3 : 2;
}

// -----------------------------------------------------------------------------
// Command dispatch + completion
// -----------------------------------------------------------------------------
// Completion on cmd_exec_task. On failure show the Error: text briefly; then
// always reload + re-render the list (a mutation may have added/removed/renamed
// a role, so the list is the safe landing spot). Direct lens draw from the
// callback is the established Power-page pattern.
static void onUserCmdDone(bool ok, const char* result,
                          const G2CmdCookie& /*cookie*/, void* /*userData*/) {
  const bool failed = !ok || (result && strncmp(result, "Error", 5) == 0);
  if (failed) {
    const char* msg = (result && result[0]) ? result : "Failed";
    if (strncmp(msg, "Error: ", 7) == 0) msg += 7;
    char banner[72];
    snprintf(banner, sizeof(banner), "%.68s", msg);
    g2ShowText(banner);
    vTaskDelay(pdMS_TO_TICKS(1600));
  }
  if (g2GetHijackPage() != G2_HIJACK_PAGE_USERS) return;
  showListMenu();
}

static void submitUserCmd(const char* line) {
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gSub;
  DEBUG_G2F("[G2-USERS] submit '%s'", line);
  if (!g2SubmitHijackCommand(line, cookie, onUserCmdDone, nullptr)) {
    DEBUG_G2F("[G2-USERS] submit FAILED — no inline mutate");
    g2ShowText("Busy - try again");
    vTaskDelay(pdMS_TO_TICKS(1000));
    showListMenu();
  }
}

// -----------------------------------------------------------------------------
// Renderers
// -----------------------------------------------------------------------------
static void showListMenu() {
  setSub(USERS_SUB_LIST);
  gSelected = -1;
  loadUsers();

  EXT_RAM_BSS_ATTR static char rows[2 + G2_USERS_MAX][32];
  const char* ptrs[2 + G2_USERS_MAX];
  strcpy(rows[0], "<- Config");  ptrs[0] = rows[0];
  strcpy(rows[1], "+ Add User"); ptrs[1] = rows[1];
  size_t n = 2;
  for (size_t i = 0; i < gUserCount; i++) {
    snprintf(rows[n], 32, "%s (%s)", gUsers[i].name, gUsers[i].role);
    ptrs[n] = rows[n];
    n++;
  }
  if (g2ShowListPage(ptrs, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_USERS);
    DEBUG_G2F("[G2-USERS] list shown (%u users)", (unsigned)gUserCount);
  }
}

static void showDetailMenu() {
  if (gSelected < 0 || (size_t)gSelected >= gUserCount) { showListMenu(); return; }
  setSub(USERS_SUB_DETAIL);
  const G2UserRow& r = gUsers[gSelected];
  static char back[40], roleRow[40];
  snprintf(back, sizeof(back), "<- %.28s", r.name);
  const bool founder = (r.id == 1);   // first account: immutable in the core
  snprintf(roleRow, sizeof(roleRow), "Role: %s%s", r.role, founder ? "" : " >");
  const char* items[3] = {
    back,
    roleRow,
    founder ? "(founder - protected)" : "Delete",
  };
  if (g2ShowListPage(items, 3)) g2SetHijackPage(G2_HIJACK_PAGE_USERS);
}

static void showRoleMenu() {
  if (gSelected < 0 || (size_t)gSelected >= gUserCount) { showListMenu(); return; }
  setSub(USERS_SUB_ROLE);
  const int curRank = userRoleRank(gUsers[gSelected].role);
  const int maxRank = pairerMaxRank();
  EXT_RAM_BSS_ATTR static char rows[5][28];
  const char* ptrs[5];
  strcpy(rows[0], "<- Back"); ptrs[0] = rows[0];
  size_t n = 1;
  for (int rank = 0; rank <= maxRank && n < 5; rank++) {   // rows 1..N = ranks 0..maxRank
    snprintf(rows[n], 28, "[%c] %s", (rank == curRank) ? 'X' : ' ', kRoleNames[rank]);
    ptrs[n] = rows[n];
    n++;
  }
  if (g2ShowListPage(ptrs, n)) g2SetHijackPage(G2_HIJACK_PAGE_USERS);
}

static void showDeleteConfirm() {
  if (gSelected < 0 || (size_t)gSelected >= gUserCount) { showListMenu(); return; }
  setSub(USERS_SUB_CONFIRM_DELETE);
  static char row[40];
  snprintf(row, sizeof(row), "Confirm del %.16s", gUsers[gSelected].name);
  const char* items[] = { "<- Cancel", row };
  if (g2ShowListPage(items, 2)) g2SetHijackPage(G2_HIJACK_PAGE_USERS);
}

// -----------------------------------------------------------------------------
// Add-user flow (keyboard → keyboard → role pick → useradd)
// -----------------------------------------------------------------------------
static void addUserCancel() {
  DEBUG_G2F("[G2-USERS] add cancelled");
  showListMenu();
}

// Runs on the BLE notify task — String build + next-step only.
static void addPassCommit(const char* text) {
  if (!text || text[0] == '\0') { showListMenu(); return; }  // empty = cancel
  strncpy(gAddPass, text, sizeof(gAddPass) - 1);
  gAddPass[sizeof(gAddPass) - 1] = '\0';
  showAddRoleMenu();   // the command validates min-6; short passwords banner via the result
}

static void addUserCommit(const char* text) {
  if (!text || text[0] == '\0') { showListMenu(); return; }
  strncpy(gAddUser, text, sizeof(gAddUser) - 1);
  gAddUser[sizeof(gAddUser) - 1] = '\0';
  TextEntryConfig cfg = {};
  cfg.prompt   = "Password";
  cfg.initial  = "";
  cfg.maxLen   = 32;
  cfg.onCommit = addPassCommit;
  cfg.onCancel = addUserCancel;
  cfg.isSecret = true;   // new-user password — keep out of debug logs
  if (!g2BeginTextEntry(cfg)) { DEBUG_G2F("[G2-USERS] password entry failed"); showListMenu(); }
}

static void beginAddUser() {
  gAddUser[0] = '\0';
  gAddPass[0] = '\0';
  TextEntryConfig cfg = {};
  cfg.prompt   = "New Username";
  cfg.initial  = "";
  cfg.maxLen   = 32;
  cfg.onCommit = addUserCommit;
  cfg.onCancel = addUserCancel;
  if (!g2BeginTextEntry(cfg)) { DEBUG_G2F("[G2-USERS] username entry failed"); showListMenu(); }
}

static void showAddRoleMenu() {
  setSub(USERS_SUB_ADD_ROLE);
  const int maxRank = pairerMaxRank();
  EXT_RAM_BSS_ATTR static char rows[5][24];
  const char* ptrs[5];
  strcpy(rows[0], "<- Cancel"); ptrs[0] = rows[0];
  size_t n = 1;
  for (int rank = 0; rank <= maxRank && n < 5; rank++) {   // rows 1..N = ranks 0..maxRank
    snprintf(rows[n], 24, "%s", kRoleNames[rank]);
    ptrs[n] = rows[n];
    n++;
  }
  // We arrive here straight off the password keyboard, which left the lens
  // hijackPage parked at TEXT_VIEW (g2StartLiveListPage → g2ShowTextAsList).
  // Without restoring it, taps route to the read-only TEXT_VIEW fallback and
  // the role pick is silently dropped — reclaim USERS so g2UsersHandleTap runs.
  if (g2ShowListPage(ptrs, n)) g2SetHijackPage(G2_HIJACK_PAGE_USERS);
}

static void addRoleHandleTap(uint32_t idx) {
  if (idx == 0) { showListMenu(); return; }       // cancel
  int rank = (int)idx - 1;                          // rows 1.. = ranks 0..
  if (rank < 0 || rank > pairerMaxRank()) return;
  // useradd <user> <pass> 0 <role>  (0 = don't force a password change).
  // The keyboard charset can't produce a double-quote, so quoting is safe.
  String line = String("useradd \"") + gAddUser + "\" \"" + gAddPass + "\" 0 " + kRoleNames[rank];
  submitUserCmd(line.c_str());
}

// -----------------------------------------------------------------------------
// Tap dispatch per sub-mode
// -----------------------------------------------------------------------------
static void listHandleTap(uint32_t idx) {
  if (idx == 0) { g2ShowConfigMenu(); return; }
  if (idx == 1) { beginAddUser();  return; }   // + Add User
  size_t ui = idx - 2;
  if (ui < gUserCount) { gSelected = (int)ui; showDetailMenu(); }
}

static void detailHandleTap(uint32_t idx) {
  if (idx == 0) { showListMenu(); return; }
  if (gSelected < 0 || (size_t)gSelected >= gUserCount) { showListMenu(); return; }
  if (gUsers[gSelected].id == 1) return;   // founder — both action rows inert
  if (idx == 1) { showRoleMenu();     return; }
  if (idx == 2) { showDeleteConfirm(); return; }
}

static void roleHandleTap(uint32_t idx) {
  if (idx == 0) { showDetailMenu(); return; }
  if (gSelected < 0 || (size_t)gSelected >= gUserCount) { showListMenu(); return; }
  const G2UserRow& r = gUsers[gSelected];
  int targetRank = (int)idx - 1;               // rows 1.. = ranks 0..
  if (targetRank < 0 || targetRank > 3) return;
  int curRank = userRoleRank(r.role);
  if (targetRank == curRank) return;           // already that role — no-op
  char line[80];
  // promote raises, demote lowers — there is no single set-role command.
  snprintf(line, sizeof(line), "%s %s %s",
           targetRank > curRank ? "userpromote" : "userdemote", r.name, kRoleNames[targetRank]);
  submitUserCmd(line);
}

static void deleteHandleTap(uint32_t idx) {
  if (idx == 0) { showDetailMenu(); return; }
  if (gSelected < 0 || (size_t)gSelected >= gUserCount) { showListMenu(); return; }
  if (idx == 1) {
    // One-shot confirm token — the interactive `userdelete` prompt can't be
    // answered from a tap; we already confirmed on-lens.
    String line = String("userdelete \"") + gUsers[gSelected].name + "\" confirm";
    submitUserCmd(line.c_str());
  }
}

// -----------------------------------------------------------------------------
// Public entry points
// -----------------------------------------------------------------------------
void g2UsersHandleTap(uint32_t idx) {
  switch (gSub) {
    case USERS_SUB_LIST:           listHandleTap(idx);   break;
    case USERS_SUB_DETAIL:         detailHandleTap(idx); break;
    case USERS_SUB_ROLE:           roleHandleTap(idx);   break;
    case USERS_SUB_CONFIRM_DELETE: deleteHandleTap(idx); break;
    case USERS_SUB_ADD_ROLE:       addRoleHandleTap(idx); break;
  }
}

void g2ShowUsersMenu() {
  gSub = USERS_SUB_LIST;
  showListMenu();
  g2SetHijackPage(G2_HIJACK_PAGE_USERS);
}

void g2BuildUsersInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap, "User manager (list / add / delete / role on lens; 'userlist' for CLI)");
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
