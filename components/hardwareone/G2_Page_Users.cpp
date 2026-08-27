// =============================================================================
// G2 glasses — "Users" App page implementation (admin user manager)
// =============================================================================
// See header. Read-only display parses /system/users/users.json each render
// (like the Automations page re-reads its JSON); every mutation dispatches a
// real user/session command through g2SubmitHijackCommand, so command-layer
// authorization remains authoritative and this page never mutates the account
// store directly. Completion is marshalled through the lens applier.

#include "G2_Page_Users.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <Arduino.h>
#include <ArduinoJson.h>
#include <new>                  // std::nothrow — RedrawSpec / LensUiJob

#include "G2_Glasses.h"        // g2ShowListPage / g2Set/GetHijackPage / g2BumpMenuGen
#include "G2_HijackCmd.h"      // G2CmdCookie / g2SubmitHijackCommand / g2HijackAuthContext
#include "G2_Page_TextEntry.h" // on-lens keyboard for Add User
#include "System_Utils.h"      // readText
#include "System_MemUtil.h"    // PSRAM_JSON_DOC
#include "System_User.h"       // USERS_JSON_FILE / isAdminUser / isSuperAdminUser / userRoleRank
#include "System_Debug.h"      // DEBUG_G2F

extern void g2ShowConfigMenu();  // return to the Config launcher on Back (menu reorg; non-static in G2_Glasses.cpp)

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
enum UsersSub : uint8_t {
  USERS_SUB_LIST = 0,
  USERS_SUB_DETAIL,
  USERS_SUB_ROLE,
  USERS_SUB_CONFIRM_ACTION,
  USERS_SUB_ADD_ROLE,
  USERS_SUB_RESET_POLICY,
  USERS_SUB_PENDING,
  USERS_SUB_RESULT,
};
static UsersSub gSub = USERS_SUB_LIST;
static inline void setSub(UsersSub s) {
  if (gSub == s) return;
  gSub = s;
  g2BumpMenuGen();   // drop stale cmd_exec redraws from the previous sub-mode
}

#define G2_USERS_MAX 16
struct G2UserRow {
  char name[kPublicUsernameMaxLen + 1];
  char role[12];
  long id;
  bool banned;
};
EXT_RAM_BSS_ATTR static G2UserRow gUsers[G2_USERS_MAX];
static size_t    gUserCount = 0;
static int       gSelected  = -1;   // index into gUsers while in DETAIL/ROLE/DELETE

// Add-user staging (filled by the keyboard commits).
static char gAddUser[kPublicUsernameMaxLen + 1] = {0};
static char gAddPass[kPublicPasswordMaxLen + 1] = {0};

// Password staging always lives in internal DRAM and is explicitly scrubbed
// after submit/cancel. The command queue owns its own String copy by then.
static char gPasswordCurrent[kPublicPasswordMaxLen + 1] = {0};
static char gPasswordNew[kPublicPasswordMaxLen + 1] = {0};
static char gPasswordConfirm[kPublicPasswordMaxLen + 1] = {0};

enum PasswordFlow : uint8_t {
  PASSWORD_FLOW_NONE = 0,
  PASSWORD_FLOW_ADD,
  PASSWORD_FLOW_SELF_CHANGE,
  PASSWORD_FLOW_ADMIN_RESET,
};
static PasswordFlow gPasswordFlow = PASSWORD_FLOW_NONE;
static int8_t gAddRoleRank = -1;
static bool gResetMustChange = false;

enum DetailAction : uint8_t {
  DETAIL_ACTION_BACK = 0,
  DETAIL_ACTION_INFO,
  DETAIL_ACTION_ROLE,
  DETAIL_ACTION_CHANGE_PASSWORD,
  DETAIL_ACTION_RESET_PASSWORD,
#if ENABLE_HTTP_SERVER
  DETAIL_ACTION_KICK,
  DETAIL_ACTION_BAN,
  DETAIL_ACTION_UNBAN,
#endif
  DETAIL_ACTION_DELETE,
};
static DetailAction gDetailActions[10];
static size_t gDetailActionCount = 0;

enum ConfirmAction : uint8_t {
  CONFIRM_ACTION_NONE = 0,
#if ENABLE_HTTP_SERVER
  CONFIRM_ACTION_KICK,
  CONFIRM_ACTION_BAN,
  CONFIRM_ACTION_UNBAN,
#endif
  CONFIRM_ACTION_DELETE,
};
static ConfirmAction gConfirmAction = CONFIRM_ACTION_NONE;

EXT_RAM_BSS_ATTR static char gResultMessage[112] = {0};
static bool gResultFailed = false;
static uint32_t gUserCmdTokenNext = 0;
static uint32_t gPendingCmdToken = 0;
static uint32_t gResultReadyToken = 0;

// Role vocabulary, indexed by rank (guest=0 .. superadmin=3).
static const char* const kRoleNames[4] = { "guest", "user", "admin", "superadmin" };

static void showListMenu();
static void showDetailMenu();
static void showRoleMenu();
static void showConfirmMenu(ConfirmAction action);
static void showAddRoleMenu();
static void showResetPolicyMenu();
static void showPendingMenu();
static void showResultMenu();

static void scrubBytes(char* buf, size_t cap) {
  if (!buf || cap == 0) return;
  volatile char* p = reinterpret_cast<volatile char*>(buf);
  while (cap--) *p++ = '\0';
}

static void scrubPasswordStaging() {
  scrubBytes(gAddPass, sizeof(gAddPass));
  scrubBytes(gPasswordCurrent, sizeof(gPasswordCurrent));
  scrubBytes(gPasswordNew, sizeof(gPasswordNew));
  scrubBytes(gPasswordConfirm, sizeof(gPasswordConfirm));
  gPasswordFlow = PASSWORD_FLOW_NONE;
  gAddRoleRank = -1;
  gResetMustChange = false;
}

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
    r.banned = u["banned"] | false;
    gUserCount++;
  }
}

static bool selectedUserValid() {
  return gSelected >= 0 && (size_t)gSelected < gUserCount;
}

static bool selectedIsFounder() {
  return selectedUserValid() && gUsers[gSelected].id == 1;
}

static String pairerUsername() {
  return g2HijackAuthContext().user;
}

static int pairerRank() {
  const String pairer = pairerUsername();
  return pairer.length() ? userAccountRank(pairer) : -1;
}

static bool selectedIsPairer() {
  if (!selectedUserValid()) return false;
  const String pairer = pairerUsername();
  // Account lookup is case-sensitive: "Alice" and "alice" may identify
  // different records. Treating them as the same here could offer the
  // self-service password flow while displaying the other account's detail.
  return pairer.length() && pairer == gUsers[gSelected].name;
}

static bool selectedSharesPairerSessionIdentity() {
  if (!selectedUserValid()) return false;
  const String pairer = pairerUsername();
  // Account lookup is exact, but revokeUserSessions and the saved G2-owner
  // fence compare usernames case-insensitively. Protect a case-only account
  // collision from every action that could revoke the active lens owner.
  return pairer.length() &&
         pairer.equalsIgnoreCase(gUsers[gSelected].name);
}

static bool selectedIsHigherRank() {
  return selectedUserValid() &&
         userAccountRank(gUsers[gSelected].name) > pairerRank();
}

// Refresh the selected row by stable username after a mutation. Deletion (or a
// concurrent roster edit) simply lands back on the list instead of displaying
// an old gSelected index against a rewritten users.json snapshot.
static bool reloadSelectedUser() {
  if (!selectedUserValid()) return false;
  char wanted[kPublicUsernameMaxLen + 1];
  strncpy(wanted, gUsers[gSelected].name, sizeof(wanted) - 1);
  wanted[sizeof(wanted) - 1] = '\0';
  loadUsers();
  for (size_t i = 0; i < gUserCount; ++i) {
    if (strcmp(gUsers[i].name, wanted) == 0) {
      gSelected = (int)i;
      return true;
    }
  }
  gSelected = -1;
  return false;
}

// Max role rank the CURRENT pairer may grant (can't grant above your own; the
// command re-checks, this just hides un-grantable rows). Page is admin-gated,
// so the pairer is at least admin (rank 2).
static int pairerMaxRank() {
  const int rank = pairerRank();
  if (rank < kRoleRankGuest) return kRoleRankGuest;
  if (rank > kRoleRankSuperAdmin) return kRoleRankSuperAdmin;
  return rank;
}

// -----------------------------------------------------------------------------
// Command dispatch + completion
// -----------------------------------------------------------------------------
static void setResultMessage(bool failed, const char* result) {
  gResultFailed = failed;
  const char* msg = (result && result[0]) ? result
                                          : (failed ? "Action failed" : "Action completed");
  snprintf(gResultMessage, sizeof(gResultMessage), "%.108s", msg);
}

// Marshal cmd_exec completion back to the lens applier. Redraw jobs are
// menu-generation guarded, so navigating away while a command is in flight can
// never snap the wearer back to a stale Users page.
static void enqueueResultFromCallback(const G2CmdCookie& cookie) {
  RedrawSpec* spec = new (std::nothrow) RedrawSpec{};
  if (!spec) {
    DEBUG_G2F("[G2-USERS] result RedrawSpec alloc failed");
    return;
  }
  spec->render = showResultMenu;
  LensUiJob* job = new (std::nothrow) LensUiJob{};
  if (!job) {
    DEBUG_G2F("[G2-USERS] result LensUiJob alloc failed");
    delete spec;
    return;
  }
  job->kind           = LensJobKind::Redraw;
  job->submitMenuGen  = cookie.menuGen;
  job->cmdSeq         = cookie.seq;
  job->targetPage     = cookie.targetPage;
  job->targetNetSub   = cookie.targetNetSub;
  job->payload.redraw = spec;
  if (!g2EnqueueLensJob(job)) {
    DEBUG_G2F("[G2-USERS] result lens job enqueue failed");
    delete spec;
    delete job;
  }
}

// Completion runs on cmd_exec_task and never touches the lens directly.
static void onUserCmdDone(bool ok, const char* result,
                          const G2CmdCookie& cookie, void* userData) {
  const uint32_t token = (uint32_t)(uintptr_t)userData;
  if (!token || __atomic_load_n(&gPendingCmdToken, __ATOMIC_ACQUIRE) != token) {
    DEBUG_G2F("[G2-USERS] stale result ignored (token=%u)", (unsigned)token);
    return;
  }
  const bool failed = !ok || (result && strncmp(result, "Error", 5) == 0);
  setResultMessage(failed, result);
  // Publish only after the message is complete. If the lens redraw allocation
  // or enqueue fails, a tap on the pending row can safely recover the result.
  __atomic_store_n(&gResultReadyToken, token, __ATOMIC_RELEASE);
  enqueueResultFromCallback(cookie);
}

static bool submitUserCmd(const char* line) {
  if (!line || !line[0]) return false;
  const String safeLine = redactCmdForAudit(String(line));
  DEBUG_G2F("[G2-USERS] submit '%s'", safeLine.c_str());

  uint32_t token = __atomic_add_fetch(&gUserCmdTokenNext, 1u,
                                      __ATOMIC_ACQ_REL);
  if (!token) {
    token = __atomic_add_fetch(&gUserCmdTokenNext, 1u, __ATOMIC_ACQ_REL);
  }
  __atomic_store_n(&gPendingCmdToken, token, __ATOMIC_RELEASE);
  __atomic_store_n(&gResultReadyToken, 0u, __ATOMIC_RELEASE);

  // Move off the actionable menu before queueing so repeated taps cannot submit
  // the same destructive command twice. The cookie captures this generation.
  showPendingMenu();
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gSub;
  if (!g2SubmitHijackCommand(
          line, cookie, onUserCmdDone,
          reinterpret_cast<void*>((uintptr_t)token))) {
    DEBUG_G2F("[G2-USERS] submit FAILED — no inline mutate");
    setResultMessage(true, "Busy - try again");
    // The Working page swap may already be in flight. Publish the failure and
    // stay in PENDING; its explicit tap-to-check path cannot race that swap.
    __atomic_store_n(&gResultReadyToken, token, __ATOMIC_RELEASE);
    return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
// Renderers
// -----------------------------------------------------------------------------
static void showListMenu() {
  setSub(USERS_SUB_LIST);
  __atomic_store_n(&gPendingCmdToken, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&gResultReadyToken, 0u, __ATOMIC_RELEASE);
  scrubPasswordStaging();
  gConfirmAction = CONFIRM_ACTION_NONE;
  gSelected = -1;
  loadUsers();

  EXT_RAM_BSS_ATTR static char rows[2 + G2_USERS_MAX][72];
  const char* ptrs[2 + G2_USERS_MAX];
  strcpy(rows[0], "<- Config");  ptrs[0] = rows[0];
  strcpy(rows[1], "+ Add User"); ptrs[1] = rows[1];
  size_t n = 2;
  for (size_t i = 0; i < gUserCount; i++) {
    snprintf(rows[n], sizeof(rows[n]), "%.48s (%.10s)%s", gUsers[i].name,
             gUsers[i].role, gUsers[i].banned ? " [BANNED]" : "");
    ptrs[n] = rows[n];
    n++;
  }
  if (g2ShowListPage(ptrs, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_USERS);
    DEBUG_G2F("[G2-USERS] list shown (%u users)", (unsigned)gUserCount);
  }
}

static void showDetailMenu() {
  if (!selectedUserValid()) { showListMenu(); return; }
  setSub(USERS_SUB_DETAIL);
  const G2UserRow& r = gUsers[gSelected];
  const bool founder = selectedIsFounder();
  const bool self = selectedIsPairer();
  const bool ownerIdentity = selectedSharesPairerSessionIdentity();
  const bool higher = selectedIsHigherRank();

  EXT_RAM_BSS_ATTR static char rows[10][72];
  const char* ptrs[10];
  size_t n = 0;
  auto add = [&](DetailAction action, const char* text) {
    if (n >= sizeof(gDetailActions) / sizeof(gDetailActions[0])) return;
    snprintf(rows[n], sizeof(rows[n]), "%s", text ? text : "");
    ptrs[n] = rows[n];
    gDetailActions[n] = action;
    ++n;
  };

  snprintf(rows[n], sizeof(rows[n]), "<- %.60s", r.name);
  ptrs[n] = rows[n];
  gDetailActions[n++] = DETAIL_ACTION_BACK;

  char roleRow[32];
  const bool roleMutable = !founder && !ownerIdentity && !higher;
  snprintf(roleRow, sizeof(roleRow), "Role: %s%s", r.role,
           roleMutable ? " >" : "");
  add(roleMutable ? DETAIL_ACTION_ROLE : DETAIL_ACTION_INFO, roleRow);
  add(DETAIL_ACTION_INFO, r.banned ? "Status: Banned" : "Status: Active");

  if (higher) {
    add(DETAIL_ACTION_INFO, "Higher role - protected");
  } else if (founder) {
    add(DETAIL_ACTION_INFO, "Founder - protected");
    if (self) add(DETAIL_ACTION_CHANGE_PASSWORD, "Change Password");
  } else if (ownerIdentity) {
    // Role/demote, reset, kick, ban, and delete all invalidate G2 owner
    // authority. Keep only self-service password change, whose backend exempts
    // SOURCE_G2_GLASSES from its revoke sweep.
    add(DETAIL_ACTION_INFO,
        self ? "Current G2 owner" : "G2 owner name collision");
    if (self) add(DETAIL_ACTION_CHANGE_PASSWORD, "Change Password");
  } else {
    add(DETAIL_ACTION_RESET_PASSWORD, "Reset Password");
#if ENABLE_HTTP_SERVER
    add(DETAIL_ACTION_KICK, "Kick Sessions");
    add(r.banned ? DETAIL_ACTION_UNBAN : DETAIL_ACTION_BAN,
        r.banned ? "Unban User" : "Ban User");
#endif
    add(DETAIL_ACTION_DELETE, "Delete User");
  }

  gDetailActionCount = n;
  if (g2ShowListPage(ptrs, n)) g2SetHijackPage(G2_HIJACK_PAGE_USERS);
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

static void showConfirmMenu(ConfirmAction action) {
  if (!selectedUserValid() || selectedIsFounder() ||
      selectedSharesPairerSessionIdentity() ||
      selectedIsHigherRank()) {
    showDetailMenu();
    return;
  }
  gConfirmAction = action;
  setSub(USERS_SUB_CONFIRM_ACTION);
  static char warning[72];
  static char confirm[72];
  switch (action) {
#if ENABLE_HTTP_SERVER
    case CONFIRM_ACTION_KICK:
      snprintf(warning, sizeof(warning), "Sign out %.55s?", gUsers[gSelected].name);
      strcpy(confirm, "Confirm Kick Sessions");
      break;
    case CONFIRM_ACTION_BAN:
      snprintf(warning, sizeof(warning), "Ban %.60s?", gUsers[gSelected].name);
      strcpy(confirm, "Confirm Ban + Kick");
      break;
    case CONFIRM_ACTION_UNBAN:
      snprintf(warning, sizeof(warning), "Unban %.58s?", gUsers[gSelected].name);
      strcpy(confirm, "Confirm Unban");
      break;
#endif
    case CONFIRM_ACTION_DELETE:
      snprintf(warning, sizeof(warning), "Delete %.57s?", gUsers[gSelected].name);
      strcpy(confirm, "Confirm Delete");
      break;
    default:
      showDetailMenu();
      return;
  }
  const char* items[] = { "<- Cancel", warning, confirm };
  if (g2ShowListPage(items, 3)) g2SetHijackPage(G2_HIJACK_PAGE_USERS);
}

static void showResetPolicyMenu() {
  if (!selectedUserValid() || gPasswordFlow != PASSWORD_FLOW_ADMIN_RESET ||
      selectedIsFounder() || selectedSharesPairerSessionIdentity() ||
      selectedIsHigherRank()) {
    scrubPasswordStaging();
    showDetailMenu();
    return;
  }
  setSub(USERS_SUB_RESET_POLICY);
  const char* items[] = {
    "<- Cancel",
    "Save Password",
    "Save + Require Change",
  };
  if (g2ShowListPage(items, 3)) g2SetHijackPage(G2_HIJACK_PAGE_USERS);
}

static void showPendingMenu() {
  setSub(USERS_SUB_PENDING);
  const char* items[] = { "Working... tap to check" };
  if (g2ShowListPage(items, 1)) g2SetHijackPage(G2_HIJACK_PAGE_USERS);
}

static void showResultMenu() {
  setSub(USERS_SUB_RESULT);
  static char back[72];
  if (selectedUserValid())
    snprintf(back, sizeof(back), "<- %.60s", gUsers[gSelected].name);
  else
    strcpy(back, "<- Users");
  const char* items[] = {
    back,
    gResultFailed ? "Action Failed" : "Done",
    gResultMessage[0] ? gResultMessage : (gResultFailed ? "Action failed" : "Action completed"),
  };
  if (g2ShowListPage(items, 3)) g2SetHijackPage(G2_HIJACK_PAGE_USERS);
}

// -----------------------------------------------------------------------------
// Password + add-user flows
// -----------------------------------------------------------------------------
static bool passwordLengthValid(const char* text) {
  if (!text) return false;
  const size_t len = strlen(text);
  return len >= kPublicPasswordMinLen && len <= kPublicPasswordMaxLen;
}

static void showPasswordError(const char* message) {
  const bool wasAdd = gPasswordFlow == PASSWORD_FLOW_ADD;
  scrubPasswordStaging();
  if (wasAdd) gAddUser[0] = '\0';
  setResultMessage(true, message);
  showResultMenu();
}

static void passwordFlowCancel() {
  const bool wasAdd = gPasswordFlow == PASSWORD_FLOW_ADD;
  scrubPasswordStaging();
  if (wasAdd) gAddUser[0] = '\0';
  if (wasAdd || !selectedUserValid()) showListMenu();
  else showDetailMenu();
}

static bool beginSecretEntry(const char* prompt, TextEntryCommitFn onCommit) {
  TextEntryConfig cfg = {};
  cfg.prompt   = prompt;
  cfg.initial  = "";
  cfg.maxLen   = kPublicPasswordMaxLen;
  cfg.onCommit = onCommit;
  cfg.onCancel = passwordFlowCancel;
  cfg.onAbandon = scrubPasswordStaging;
  cfg.isSecret = true;
  return g2BeginTextEntry(cfg);
}

static void passwordConfirmCommit(const char* text);
static void newPasswordCommit(const char* text) {
  if (!text || !text[0]) { passwordFlowCancel(); return; }
  if (!passwordLengthValid(text)) {
    showPasswordError("Password must be 6-64 characters");
    return;
  }
  strncpy(gPasswordNew, text, sizeof(gPasswordNew) - 1);
  gPasswordNew[sizeof(gPasswordNew) - 1] = '\0';
  if (!beginSecretEntry("Confirm Password", passwordConfirmCommit)) {
    DEBUG_G2F("[G2-USERS] password confirmation entry failed");
    passwordFlowCancel();
  }
}

static void currentPasswordCommit(const char* text) {
  if (!text || !text[0]) { passwordFlowCancel(); return; }
  strncpy(gPasswordCurrent, text, sizeof(gPasswordCurrent) - 1);
  gPasswordCurrent[sizeof(gPasswordCurrent) - 1] = '\0';
  if (!beginSecretEntry("New Password", newPasswordCommit)) {
    DEBUG_G2F("[G2-USERS] new-password entry failed");
    passwordFlowCancel();
  }
}

static void passwordConfirmCommit(const char* text) {
  if (!text || !text[0]) { passwordFlowCancel(); return; }
  strncpy(gPasswordConfirm, text, sizeof(gPasswordConfirm) - 1);
  gPasswordConfirm[sizeof(gPasswordConfirm) - 1] = '\0';
  if (strcmp(gPasswordNew, gPasswordConfirm) != 0) {
    showPasswordError("Passwords do not match");
    return;
  }

  if (gPasswordFlow == PASSWORD_FLOW_SELF_CHANGE) {
    if (!selectedUserValid() || !selectedIsPairer()) {
      passwordFlowCancel();
      return;
    }
    String line = String("userchangepassword \"") + gPasswordCurrent + "\" \"" +
                  gPasswordNew + "\" \"" + gPasswordConfirm + "\"";
    submitUserCmd(line.c_str());
    scrubPasswordStaging();
    return;
  }
  if (gPasswordFlow == PASSWORD_FLOW_ADMIN_RESET) {
    if (!selectedUserValid() || selectedIsFounder() ||
        selectedSharesPairerSessionIdentity() ||
        selectedIsHigherRank()) {
      passwordFlowCancel();
      return;
    }
    String line = String("userresetpassword \"") + gUsers[gSelected].name +
                  "\" \"" + gPasswordNew + "\" " +
                  (gResetMustChange ? "1" : "0");
    submitUserCmd(line.c_str());
    scrubPasswordStaging();
    return;
  }
  passwordFlowCancel();
}

static void beginSelfPasswordChange() {
  if (!selectedUserValid() || !selectedIsPairer()) return;
  scrubPasswordStaging();
  gPasswordFlow = PASSWORD_FLOW_SELF_CHANGE;
  if (!beginSecretEntry("Current Password", currentPasswordCommit)) {
    DEBUG_G2F("[G2-USERS] current-password entry failed");
    passwordFlowCancel();
  }
}

static void beginAdminPasswordReset() {
  if (!selectedUserValid() || selectedIsFounder() ||
      selectedSharesPairerSessionIdentity() ||
      selectedIsHigherRank()) return;
  scrubPasswordStaging();
  gPasswordFlow = PASSWORD_FLOW_ADMIN_RESET;
  showResetPolicyMenu();
}

static void addUserCancel() {
  DEBUG_G2F("[G2-USERS] add cancelled");
  scrubPasswordStaging();
  gAddUser[0] = '\0';
  showListMenu();
}

static void addPassConfirmCommit(const char* text) {
  if (!text || !text[0]) { addUserCancel(); return; }
  if (strcmp(gAddPass, text) != 0) {
    showPasswordError("Passwords do not match");
    return;
  }
  if (gAddRoleRank < 0 || gAddRoleRank > pairerMaxRank()) {
    showPasswordError("Selected role is no longer allowed");
    return;
  }
  // useradd <user> <pass> 0 <role>  (0 = don't force a password change).
  // The keyboard charset can't produce a double-quote, so quoting is safe.
  String line = String("useradd \"") + gAddUser + "\" \"" + gAddPass +
                "\" 0 " + kRoleNames[gAddRoleRank];
  submitUserCmd(line.c_str());
  scrubPasswordStaging();
  gAddUser[0] = '\0';
}

// Text-entry commits run on g2_tap_disp. Keep each callback to bounded copies
// plus the next keyboard/menu transition.
static void addPassCommit(const char* text) {
  if (!text || !text[0]) { addUserCancel(); return; }
  if (!passwordLengthValid(text)) {
    showPasswordError("Password must be 6-64 characters");
    return;
  }
  strncpy(gAddPass, text, sizeof(gAddPass) - 1);
  gAddPass[sizeof(gAddPass) - 1] = '\0';
  if (!beginSecretEntry("Confirm Password", addPassConfirmCommit)) {
    DEBUG_G2F("[G2-USERS] add-password confirmation entry failed");
    addUserCancel();
  }
}

static void addUserCommit(const char* text) {
  if (!text || text[0] == '\0') { showListMenu(); return; }
  strncpy(gAddUser, text, sizeof(gAddUser) - 1);
  gAddUser[sizeof(gAddUser) - 1] = '\0';
  showAddRoleMenu();
}

static void beginAddUser() {
  scrubPasswordStaging();
  gPasswordFlow = PASSWORD_FLOW_ADD;
  gAddUser[0] = '\0';
  TextEntryConfig cfg = {};
  cfg.prompt   = "New Username";
  cfg.initial  = "";
  cfg.maxLen   = kPublicUsernameMaxLen;
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
  // We arrive here straight off the username keyboard, which left the lens
  // hijackPage parked at TEXT_VIEW (g2StartLiveListPage → g2ShowTextAsList).
  // Without restoring it, taps route to the read-only TEXT_VIEW fallback and
  // the role pick is silently dropped — reclaim USERS so g2UsersHandleTap runs.
  if (g2ShowListPage(ptrs, n)) g2SetHijackPage(G2_HIJACK_PAGE_USERS);
}

static void addRoleHandleTap(uint32_t idx) {
  if (idx == 0) { addUserCancel(); return; }
  int rank = (int)idx - 1;                          // rows 1.. = ranks 0..
  if (rank < 0 || rank > pairerMaxRank()) return;
  gAddRoleRank = (int8_t)rank;
  if (!beginSecretEntry("Password", addPassCommit)) {
    DEBUG_G2F("[G2-USERS] password entry failed");
    addUserCancel();
  }
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
  if (!selectedUserValid()) { showListMenu(); return; }
  if (idx >= gDetailActionCount) return;
  switch (gDetailActions[idx]) {
    case DETAIL_ACTION_BACK:
      showListMenu();
      return;
    case DETAIL_ACTION_INFO:
      return;
    case DETAIL_ACTION_ROLE:
      if (!selectedIsFounder() && !selectedSharesPairerSessionIdentity() &&
          !selectedIsHigherRank())
        showRoleMenu();
      return;
    case DETAIL_ACTION_CHANGE_PASSWORD:
      beginSelfPasswordChange();
      return;
    case DETAIL_ACTION_RESET_PASSWORD:
      beginAdminPasswordReset();
      return;
#if ENABLE_HTTP_SERVER
    case DETAIL_ACTION_KICK:
      showConfirmMenu(CONFIRM_ACTION_KICK);
      return;
    case DETAIL_ACTION_BAN:
      showConfirmMenu(CONFIRM_ACTION_BAN);
      return;
    case DETAIL_ACTION_UNBAN:
      if (selectedIsFounder() || selectedSharesPairerSessionIdentity() ||
          selectedIsHigherRank()) return;
      showConfirmMenu(CONFIRM_ACTION_UNBAN);
      return;
#endif
    case DETAIL_ACTION_DELETE:
      showConfirmMenu(CONFIRM_ACTION_DELETE);
      return;
  }
}

static void roleHandleTap(uint32_t idx) {
  if (idx == 0) { showDetailMenu(); return; }
  if (!selectedUserValid()) { showListMenu(); return; }
  if (selectedIsFounder() || selectedSharesPairerSessionIdentity() ||
      selectedIsHigherRank()) {
    showDetailMenu();
    return;
  }
  const G2UserRow& r = gUsers[gSelected];
  int targetRank = (int)idx - 1;               // rows 1.. = ranks 0..
  if (targetRank < 0 || targetRank > pairerMaxRank()) return;
  int curRank = userRoleRank(r.role);
  if (targetRank == curRank) return;           // already that role — no-op
  // promote raises, demote lowers — there is no single set-role command.
  String line = String(targetRank > curRank ? "userpromote \"" : "userdemote \"") +
                r.name + "\" " + kRoleNames[targetRank];
  submitUserCmd(line.c_str());
}

static void confirmHandleTap(uint32_t idx) {
  if (idx == 0) { showDetailMenu(); return; }
  if (idx != 2) return;  // warning row is informational
  if (!selectedUserValid() || selectedIsFounder() ||
      selectedSharesPairerSessionIdentity() ||
      selectedIsHigherRank()) {
    showDetailMenu();
    return;
  }
  String line;
  switch (gConfirmAction) {
#if ENABLE_HTTP_SERVER
    case CONFIRM_ACTION_KICK:
      line = String("sessionrevoke user \"") + gUsers[gSelected].name + "\"";
      break;
    case CONFIRM_ACTION_BAN:
      line = String("banuser \"") + gUsers[gSelected].name + "\"";
      break;
    case CONFIRM_ACTION_UNBAN:
      // unbanuser's legacy parser consumes the whole remainder rather than
      // CommandArgs, so keep the public-username token unquoted.
      line = String("unbanuser ") + gUsers[gSelected].name;
      break;
#endif
    case CONFIRM_ACTION_DELETE:
      // One-shot token bypasses the CLI interactive prompt only after this
      // explicit on-lens confirmation.
      line = String("userdelete \"") + gUsers[gSelected].name + "\" confirm";
      break;
    default:
      showDetailMenu();
      return;
  }
  gConfirmAction = CONFIRM_ACTION_NONE;
  submitUserCmd(line.c_str());
}

static void resetPolicyHandleTap(uint32_t idx) {
  if (idx == 0) { passwordFlowCancel(); return; }
  if (idx > 2 || !selectedUserValid() ||
      gPasswordFlow != PASSWORD_FLOW_ADMIN_RESET || selectedIsFounder() ||
      selectedSharesPairerSessionIdentity() || selectedIsHigherRank()) {
    passwordFlowCancel();
    return;
  }
  gResetMustChange = idx == 2;
  if (!beginSecretEntry("New Password", newPasswordCommit)) {
    DEBUG_G2F("[G2-USERS] reset-password entry failed");
    passwordFlowCancel();
  }
}

static void resultHandleTap(uint32_t idx) {
  if (idx != 0) return;
  if (selectedUserValid() && reloadSelectedUser()) showDetailMenu();
  else showListMenu();
}

// -----------------------------------------------------------------------------
// Public entry points
// -----------------------------------------------------------------------------
void g2UsersHandleTap(uint32_t idx) {
  switch (gSub) {
    case USERS_SUB_LIST:           listHandleTap(idx);   break;
    case USERS_SUB_DETAIL:         detailHandleTap(idx); break;
    case USERS_SUB_ROLE:           roleHandleTap(idx);   break;
    case USERS_SUB_CONFIRM_ACTION: confirmHandleTap(idx); break;
    case USERS_SUB_ADD_ROLE:       addRoleHandleTap(idx); break;
    case USERS_SUB_RESET_POLICY:   resetPolicyHandleTap(idx); break;
    case USERS_SUB_PENDING: {
      const uint32_t pending =
          __atomic_load_n(&gPendingCmdToken, __ATOMIC_ACQUIRE);
      if (idx == 0 && pending &&
          __atomic_load_n(&gResultReadyToken, __ATOMIC_ACQUIRE) == pending) {
        showResultMenu();
      }
      break;
    }
    case USERS_SUB_RESULT:         resultHandleTap(idx); break;
  }
}

void g2ShowUsersMenu() {
  gSub = USERS_SUB_LIST;
  showListMenu();
  g2SetHijackPage(G2_HIJACK_PAGE_USERS);
}

void g2BuildUsersInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap, "User manager (add / role / password / sessions / ban / delete on lens)");
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
