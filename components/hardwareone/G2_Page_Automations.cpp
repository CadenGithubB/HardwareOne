// =============================================================================
// G2 glasses — "Automations" App page implementation
// =============================================================================
// See header for the contract. Lists the device's saved automations and lets
// the wearer Run / Enable / Disable each one. Rows and the trigger text come
// from the automation owner's summary copies (automationCopySummaries /
// automationFindSummary), capped at G2_AUTO_MAX rows. The page keeps the ids
// of the rows the lens shows, so a tap opens the automation it showed.
//
// Action taps route through g2SubmitHijackCommand (cmd_exec_task, glasses
// auth). The completion callback marshals a re-render back onto the lens
// applier via g2EnqueueLensJob so the on/off badge reflects the TRUE result
// (an auth-denied toggle leaves the badge unchanged).

#include "G2_Page_Automations.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_AUTOMATION

#include <Arduino.h>
#include "G2_Glasses.h"        // g2ShowListPage / g2ShowTextAsList / g2Set/GetHijackPage
#include "G2_HijackCmd.h"      // G2CmdCookie / g2SubmitHijackCommand / redraw plumbing
#include "G2_Page_Common.h"    // g2ShowListPageAndPublish
#include "System_Automation.h" // AutomationSummary / automationCopySummaries / automationFindSummary
#include "System_MemUtil.h"    // ps_alloc for the list copy
#include "System_Debug.h"      // DEBUG_G2F / BROADCAST_PRINTF

// g2ShowAppsMenu is a non-static definition in G2_Glasses.cpp (exposed so the
// Apps sub-pages can return to the launcher). Forward-declared here rather
// than in a header since it has no other cross-TU callers.
extern void g2ShowAppsMenu();

// -----------------------------------------------------------------------------
// Sub-mode + selected automation identity
// -----------------------------------------------------------------------------
enum AutoSub : uint8_t { AUTO_SUB_LIST = 0, AUTO_SUB_DETAIL = 1 };
static AutoSub gSub = AUTO_SUB_LIST;

#define G2_AUTO_MAX 16
// Ids of the automation rows the lens shows (LIST row r+1 = gListIds[r]),
// published with those rows.
EXT_RAM_BSS_ATTR static long gListIds[G2_AUTO_MAX];
static size_t gListCount = 0;
static long gSelectedAutoId = -1;    // stable id while in DETAIL (set with it)
static bool gSelectedEnabled = false; // DETAIL shows "Disable" when true

// Last Run result text, shown on the lens by showRunResult().
EXT_RAM_BSS_ATTR static char gRunResult[96];

static void showListMenu();
static void showDetailMenu(long id);
static void showRunResult();

static inline void setSub(AutoSub s) {
  if (gSub == s) return;
  gSub = s;
  g2BumpMenuGen();   // drop stale cmd_exec redraws from the previous sub-mode
}

static G2CmdCookie buildCookie() {
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gSub;
  return cookie;
}

// -----------------------------------------------------------------------------
// Renderers
// -----------------------------------------------------------------------------
// One row per automation with an on/off badge, from a fresh owner copy each
// time so the badges reflect current state (e.g. after a toggle round-trips).
// LIST is the page's entry level, so once the lens accepted the rows its
// publish also claims the AUTOMATIONS hijack page (a no-op when already
// there) and records the ids behind the rows.
static void showListMenu() {
  char rows[1 + G2_AUTO_MAX][40];
  const char* ptrs[1 + G2_AUTO_MAX];
  long ids[G2_AUTO_MAX] = {};
  strcpy(rows[0], "<- Apps");
  ptrs[0] = rows[0];
  size_t n = 1;

  // 16 summaries are ~1.3 KB: a transient PSRAM copy, released before the render.
  AutomationSummary* list = (AutomationSummary*)ps_alloc(
      G2_AUTO_MAX * sizeof(AutomationSummary), AllocPref::PreferPSRAM, "g2.autolist");
  size_t count = 0;
  if (list && automationCopySummaries(list, G2_AUTO_MAX, &count)) {
    for (size_t i = 0; i < count; i++) {
      snprintf(rows[n], sizeof(rows[n]), "%s %s",
               list[i].enabled ? "[on] " : "[off]", list[i].name);
      ptrs[n] = rows[n];
      ids[i] = list[i].id;
      n++;
    }
  }
  if (list) ps_free(list);

  const size_t shown = n - 1;
  static const char* const kEmpty[] = { "<- Apps", "(no automations)" };
  const bool empty = (shown == 0);
  if (!g2ShowListPageAndPublish(empty ? kEmpty : ptrs, empty ? 2 : n, [&] {
        setSub(AUTO_SUB_LIST);
        gSelectedAutoId = -1;
        memcpy(gListIds, ids, shown * sizeof(ids[0]));
        gListCount = shown;
        g2SetHijackPage(G2_HIJACK_PAGE_AUTOMATIONS);
      })) {
    DEBUG_G2F("[G2-AUTO] list not shown");
    return;
  }
  DEBUG_G2F("[G2-AUTO] list shown (%u automations)", (unsigned)shown);
}

// Actions for automation `id`. Back row is labelled with the name so the user
// keeps context on which automation they drilled into. DETAIL and the selected
// id become live only once the lens accepted these rows.
static void showDetailMenu(long id) {
  AutomationSummary r;
  if (id < 0 || !automationFindSummary(id, &r)) {
    showListMenu();
    return;
  }
  char backRow[40];
  snprintf(backRow, sizeof(backRow), "<- %.28s", r.name);
  // Inert trigger row: WHAT fires this automation, in the owner's words
  // ("on peer_online", "every 5m", "monthly 15 07:30", ...). Tap is a no-op
  // (detailHandleTap ignores idx 1) — it's information, not an action.
  char trigRow[40];
  snprintf(trigRow, sizeof(trigRow), "%.36s", r.trigger[0] ? r.trigger : "(no trigger)");
  const char* items[] = {
    backRow,                            // 0 — back to list
    trigRow,                            // 1 — inert trigger summary
    "Run now",                          // 2
    r.enabled ? "Disable" : "Enable",   // 3
  };
  const bool enabled = r.enabled;
  g2ShowListPageAndPublish(items, 4, [id, enabled] {
    setSub(AUTO_SUB_DETAIL);
    gSelectedAutoId = id;
    gSelectedEnabled = enabled;
  });
}

// Completion redraw: refresh the detail of the automation selected now.
static void redrawDetailMenu() {
  showDetailMenu(gSelectedAutoId);
}

// Dump the OK/Error text from a Run onto the lens. g2ShowTextAsList flips the
// page to TEXT_VIEW, so its Back row returns to the main hijack menu — an
// accepted v1 limitation (a run result is a terminal confirmation).
static void showRunResult() {
  g2ShowTextAsList(gRunResult, "<- Back");
}

// -----------------------------------------------------------------------------
// Command completion callbacks (run on cmd_exec_task)
// -----------------------------------------------------------------------------
static void autoToggleDone(bool ok, const char* /*result*/,
                           const G2CmdCookie& c, void* /*ud*/) {
  (void)ok;
  (void)g2EnqueueRedraw(c, redrawDetailMenu, "Automation toggle");
}

static void autoRunDone(bool ok, const char* result,
                        const G2CmdCookie& c, void* /*ud*/) {
  snprintf(gRunResult, sizeof(gRunResult), "%s",
           (result && result[0]) ? result : (ok ? "OK" : "Error"));
  (void)g2EnqueueRedraw(c, showRunResult, "Automation run");
}

// -----------------------------------------------------------------------------
// Tap dispatch
// -----------------------------------------------------------------------------
static void listHandleTap(uint32_t idx) {
  if (idx == 0) { g2ShowAppsMenu(); return; }   // <- Apps
  const size_t sel = idx - 1;
  if (sel >= gListCount) return;                // "(no automations)"
  showDetailMenu(gListIds[sel]);
}

// Acts on the automation and state DETAIL showed; the owner answers with an
// error if it was deleted meanwhile (a toggle redraw then returns to LIST).
static void detailHandleTap(uint32_t idx) {
  if (idx == 0) { showListMenu(); return; }
  if (gSelectedAutoId < 0) {
    showListMenu();
    return;
  }
  char line[56];
  G2CmdCookie cookie = buildCookie();
  if (idx == 1) return;                   // inert trigger-summary row
  if (idx == 2) {                         // Run now
    snprintf(line, sizeof(line), "automation run id=%ld", gSelectedAutoId);
    if (!g2SubmitHijackCommand(line, cookie, autoRunDone, nullptr))
      DEBUG_G2F("[G2-AUTO] run submit failed (queue full)");
    return;
  }
  if (idx == 3) {                         // Enable / Disable, as the row reads
    const bool target = !gSelectedEnabled;
    snprintf(line, sizeof(line), "automation %s id=%ld", target ? "enable" : "disable",
             gSelectedAutoId);
    if (!g2SubmitHijackCommand(line, cookie, autoToggleDone, nullptr))
      DEBUG_G2F("[G2-AUTO] toggle submit failed (queue full)");
    return;
  }
}

void g2AutomationsHandleTap(uint32_t idx) {
  if (gSub == AUTO_SUB_DETAIL) detailHandleTap(idx);
  else                         listHandleTap(idx);
}

// -----------------------------------------------------------------------------
// Entry + registry stub
// -----------------------------------------------------------------------------
void g2ShowAutomationsMenu() {
  showListMenu();   // claims the AUTOMATIONS page only if the lens shows it
}

void g2BuildAutomationsInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap, "Automations (list / run / enable / disable on lens)");
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_AUTOMATION
