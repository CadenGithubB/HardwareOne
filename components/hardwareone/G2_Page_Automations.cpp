// =============================================================================
// G2 glasses — "Automations" App page implementation
// =============================================================================
// See header for the contract. Lists the device's saved automations and lets
// the wearer Run / Enable / Disable each one. Automations have no in-RAM
// struct carrying names (the scheduler cache holds only id/nextAt/enabled), so
// we re-read + parse /system/automations.json on each list render — the same
// approach rebuildAutoCache() uses. Capped at G2_AUTO_MAX rows.
//
// Action taps route through g2SubmitHijackCommand (cmd_exec_task, glasses
// auth). The completion callback marshals a re-render back onto the lens
// applier via g2EnqueueLensJob so the on/off badge reflects the TRUE result
// (an auth-denied toggle leaves the badge unchanged).

#include "G2_Page_Automations.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <Arduino.h>
#include "G2_Glasses.h"        // g2ShowListPage / g2ShowTextAsList / g2Set/GetHijackPage
#include "G2_HijackCmd.h"      // G2CmdCookie / g2SubmitHijackCommand / redraw plumbing
#include "System_Utils.h"      // readText
#include "System_MemUtil.h"    // PSRAM_JSON_DOC
#include "System_Debug.h"      // DEBUG_G2F / BROADCAST_PRINTF
#include <ArduinoJson.h>
#include <new>                 // std::nothrow

// The automations JSON path is a global defined in the app globals (same
// extern System_Automation.cpp uses). Declared here rather than pulled from a
// header because it isn't exported by System_Automation.h.
extern const char* AUTOMATIONS_JSON_FILE;  // "/system/automations.json"

// g2ShowAppsMenu is a non-static definition in G2_Glasses.cpp (exposed so the
// Apps sub-pages can return to the launcher). Forward-declared here rather
// than in a header since it has no other cross-TU callers.
extern void g2ShowAppsMenu();

// -----------------------------------------------------------------------------
// Sub-mode + cached automation list
// -----------------------------------------------------------------------------
enum AutoSub : uint8_t { AUTO_SUB_LIST = 0, AUTO_SUB_DETAIL = 1 };
static AutoSub gSub = AUTO_SUB_LIST;

#define G2_AUTO_MAX 16
struct G2AutoRow { long id; bool enabled; char name[28]; };
static G2AutoRow gAutos[G2_AUTO_MAX];
static size_t    gAutoCount = 0;
static int       gSelected  = -1;   // index into gAutos while in DETAIL

// Pending optimistic toggle target, applied in the completion callback only
// if the command actually succeeded (so a denied non-admin toggle doesn't
// leave the row showing the wrong state).
static int  gPendingToggleIdx = -1;
static bool gPendingToggleTo  = false;

// Last Run result text, shown on the lens by showRunResult().
static char gRunResult[96] = {0};

static void showListMenu();
static void showDetailMenu();
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

// Marshal a re-render back onto the lens applier context from a cmd_exec
// completion callback (mirrors G2_Page_ESPNow.cpp's copy). The applier
// gen-guard drops the job if the user has since navigated away.
static void enqueueRedrawFromCallback(const G2CmdCookie& cookie,
                                      void (*renderFn)(),
                                      const char* tag) {
  RedrawSpec* spec = new (std::nothrow) RedrawSpec{};
  if (!spec) { DEBUG_G2F("[G2-AUTO] %s: RedrawSpec alloc failed", tag); return; }
  spec->render = renderFn;
  LensUiJob* job = new (std::nothrow) LensUiJob{};
  if (!job) { DEBUG_G2F("[G2-AUTO] %s: LensUiJob alloc failed", tag); delete spec; return; }
  job->kind           = LensJobKind::Redraw;
  job->submitMenuGen  = cookie.menuGen;
  job->cmdSeq         = cookie.seq;
  job->targetPage     = cookie.targetPage;
  job->targetNetSub   = cookie.targetNetSub;
  job->payload.redraw = spec;
  if (!g2EnqueueLensJob(job)) {
    DEBUG_G2F("[G2-AUTO] %s: lens job enqueue FAILED", tag);
    delete spec; delete job;
  }
}

// -----------------------------------------------------------------------------
// Load automations from JSON into gAutos[]
// -----------------------------------------------------------------------------
static void loadAutomations() {
  gAutoCount = 0;
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) return;   // no file yet → empty
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json)) return;               // malformed → empty
  JsonArrayConst autos = doc["automations"].as<JsonArrayConst>();
  if (autos.isNull()) return;
  for (JsonObjectConst a : autos) {
    if (gAutoCount >= G2_AUTO_MAX) break;
    G2AutoRow& r = gAutos[gAutoCount];
    r.id      = a["id"].as<long>();
    r.enabled = a["enabled"] | false;
    const char* nm = a["name"] | "";
    if (nm[0] == '\0') snprintf(r.name, sizeof(r.name), "#%ld", r.id);
    else { strncpy(r.name, nm, sizeof(r.name) - 1); r.name[sizeof(r.name) - 1] = '\0'; }
    gAutoCount++;
  }
}

// -----------------------------------------------------------------------------
// Renderers
// -----------------------------------------------------------------------------
// One row per automation with an on/off badge. Re-reads the JSON each time so
// the badges reflect current state (e.g. after a toggle round-trips).
static void showListMenu() {
  setSub(AUTO_SUB_LIST);
  gSelected = -1;
  loadAutomations();

  static char rows[1 + G2_AUTO_MAX][40];
  const char* ptrs[1 + G2_AUTO_MAX];
  strcpy(rows[0], "<- Apps");
  ptrs[0] = rows[0];
  size_t n = 1;
  for (size_t i = 0; i < gAutoCount; i++) {
    snprintf(rows[n], sizeof(rows[n]), "%s %s",
             gAutos[i].enabled ? "[on] " : "[off]", gAutos[i].name);
    ptrs[n] = rows[n];
    n++;
  }
  if (n == 1) {
    static const char* empty[] = { "<- Apps", "(no automations)" };
    g2ShowListPage(empty, 2);
  } else {
    g2ShowListPage(ptrs, n);
  }
  DEBUG_G2F("[G2-AUTO] list shown (%u automations)", (unsigned)gAutoCount);
}

// Actions for the selected automation. Back row is labelled with the name so
// the user keeps context on which automation they drilled into.
static void showDetailMenu() {
  if (gSelected < 0 || (size_t)gSelected >= gAutoCount) { showListMenu(); return; }
  setSub(AUTO_SUB_DETAIL);
  const G2AutoRow& r = gAutos[gSelected];
  static char backRow[40];
  snprintf(backRow, sizeof(backRow), "<- %.28s", r.name);
  const char* items[] = {
    backRow,                            // 0 — back to list
    "Run now",                          // 1
    r.enabled ? "Disable" : "Enable",   // 2
  };
  g2ShowListPage(items, 3);
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
  if (ok && gPendingToggleIdx >= 0 && (size_t)gPendingToggleIdx < gAutoCount) {
    gAutos[gPendingToggleIdx].enabled = gPendingToggleTo;
  }
  gPendingToggleIdx = -1;
  enqueueRedrawFromCallback(c, showDetailMenu, "toggle-done");
}

static void autoRunDone(bool ok, const char* result,
                        const G2CmdCookie& c, void* /*ud*/) {
  snprintf(gRunResult, sizeof(gRunResult), "%s",
           (result && result[0]) ? result : (ok ? "OK" : "Error"));
  enqueueRedrawFromCallback(c, showRunResult, "run-done");
}

// -----------------------------------------------------------------------------
// Tap dispatch
// -----------------------------------------------------------------------------
static void listHandleTap(uint32_t idx) {
  if (idx == 0) { g2ShowAppsMenu(); return; }   // <- Apps
  const size_t sel = idx - 1;
  if (sel >= gAutoCount) return;
  gSelected = (int)sel;
  showDetailMenu();
}

static void detailHandleTap(uint32_t idx) {
  if (idx == 0) { showListMenu(); return; }
  if (gSelected < 0 || (size_t)gSelected >= gAutoCount) { showListMenu(); return; }
  const G2AutoRow& r = gAutos[gSelected];
  char line[56];
  G2CmdCookie cookie = buildCookie();
  if (idx == 1) {                         // Run now
    snprintf(line, sizeof(line), "automation run id=%ld", r.id);
    if (!g2SubmitHijackCommand(line, cookie, autoRunDone, nullptr))
      DEBUG_G2F("[G2-AUTO] run submit failed (queue full)");
    return;
  }
  if (idx == 2) {                         // Enable / Disable
    const bool target = !r.enabled;
    gPendingToggleIdx = gSelected;
    gPendingToggleTo  = target;
    snprintf(line, sizeof(line), "automation %s id=%ld", target ? "enable" : "disable", r.id);
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
  gSub = AUTO_SUB_LIST;
  showListMenu();
  g2SetHijackPage(G2_HIJACK_PAGE_AUTOMATIONS);
}

void g2BuildAutomationsInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap, "Automations (list / run / enable / disable on lens)");
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
