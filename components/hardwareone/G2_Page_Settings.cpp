// =============================================================================
// G2 glasses — "Settings" page implementation
// =============================================================================
// Multi-level navigation, designed to fit every CREATE-list inside a single
// envelope fragment (≤253 pb bytes):
//
//   Level 1 — module list:
//     <- Config
//     View: INTERACTIVE (tap to switch)   ← persists the chosen view into Level 2
//     [crash] (2)
//     [debug] (89)
//     [output] (6)
//     ...
//     << Prev / Next >>                    ← paginator chrome as needed
//
//   Level 2 — entries of one module, rendered in the chosen view:
//     INTERACTIVE: one "key=value" row per entry, paginated when the
//              module has more entries than fit on one page (same
//              "<< Prev / Next >> (p/total)" chrome as Level 1).
//     JSON:    serialized JSON for THIS module, split into ~180 B
//              chunks at line boundaries and shown in a TEXT widget.
//              Every page uses the normal SHUTDOWN+CREATE text swap because
//              REBUILD-text is unreliable on the tested firmware. Tap/scroll
//              cycles pages with wrap; double-tap exits the JSON view.
//
// Why pagination instead of multi-fragment CREATE: empirical testing
// against firmware 2.1.1.10 + 2.2.0.242 showed the EvenCore reassembler
// does NOT rebuild Cmd=0 messages from multiple same-seq fragments —
// single-frag CREATE works, multi-frag CREATE never acks. (The
// reassembler IS used for image streaming on Cmd=3, just not the
// page-creation path.) See docs/G2_PROTOCOL.md "Hijack page-swap
// lifecycle" for the test evidence.
//
// The INTERACTIVE view (formerly "Pretty") is a live editor. Modules whose
// settings carry groups (e.g. debug) drill module -> group -> entries;
// tapping an entry flips a boolean, opens an enum pick-list, or launches the
// character keyboard, committing through the real per-setting CLI command
// (the same mechanism the OLED settings editor uses, via the shared
// System_SettingsEditorCore helpers). The JSON view stays read-only.

#include "G2_Page_Settings.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_Glasses.h"
#include "System_Settings.h"
#include "System_SettingsEditorCore.h"   // shared visibility/editability/enum/value helpers
#include "System_Debug.h"
#include "System_MemUtil.h"   // PSRAM_JSON_DOC
#include "G2_Page_Common.h"
#include "G2_HijackCmd.h"      // g2SubmitHijackCommand / G2CmdCookie
#include "G2_Page_TextEntry.h" // g2BeginTextEntry (string / numeric editing)
#include <ArduinoJson.h>
#include <new>          // std::nothrow
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"     // vTaskDelay (result banners)

// -----------------------------------------------------------------------------
// Navigation + view state
// -----------------------------------------------------------------------------

enum SettingsLevel : uint8_t {
  SET_LEVEL_MODULES = 0,    // module-list view (root)
  SET_LEVEL_GROUPS  = 1,    // group list for a module whose settings are grouped
  SET_LEVEL_ENTRIES = 2,    // entry list (all entries, or one group), rendered per gView
  SET_LEVEL_PICK    = 3,    // enum pick-list while editing one entry
};

enum SettingsView : uint8_t {
  SET_VIEW_INTERACTIVE = 0, // label=value rows, tappable to edit
  SET_VIEW_JSON        = 1, // serialized JSON, chunked (read-only)
};

static SettingsLevel gLevel        = SET_LEVEL_MODULES;
static SettingsView  gView         = SET_VIEW_INTERACTIVE;  // persists across drill-in/out
static int           gActiveModule = -1;                // registry index once past the module list
static size_t        gModulePage   = 0;                 // 0-based page index for module list
static size_t        gEntryPage    = 0;                 // 0-based page index for entries view

// -----------------------------------------------------------------------------
// Interactive-editor drill-down state
// -----------------------------------------------------------------------------
// Sentinels for the entry-view group filter (compared by pointer identity):
//   kGroupAll        — flat module: show every entry (no group level)
//   kGroupUngrouped  — the synthetic "(General)" bucket inside a grouped module
// Any other value is a real SettingEntry.group string pointer.
static const char kGroupAll[]       = "\x01" "ALL";
static const char kGroupUngrouped[] = "\x01" "UNGROUPED";

static const char*         gActiveGroup     = kGroupAll;  // active entry-view filter
static bool                gModuleHasGroups = false;      // entries reached via a group level?
static size_t              gGroupPage       = 0;          // 0-based page index for group list
static size_t              gPickPage        = 0;          // 0-based page index for pick-list
static const SettingEntry* gEditEntry       = nullptr;    // entry being edited (pick-list/keyboard)

// Display generation — bumped on every settings list re-render. A commit
// captures it at submit time (passed as the callback userData); if it differs
// by completion, the settings display changed (the user navigated between the
// four sub-levels, which all share the single SETTINGS hijack page, or another
// render ran) and the stale redraw is dropped. It complements g2MenuGen: every
// Settings render now bumps both, while this local value also keys the pending
// result record consumed by the lens-applier worker.
static uint32_t            gNavGen          = 0;

// At most one setting mutation is allowed to own the current Settings view.
// The command itself runs on cmd_exec_task; its completion only publishes a
// gen-guarded Redraw job. The lens-applier worker consumes this small result
// record and performs the actual refresh/banner work. Protect it because the
// tap dispatcher, cmd_exec task, and lens applier can run on different cores.
struct SettingsCommitUiState {
  bool     pending;
  bool     resultReady;
  bool     success;
  uint32_t navGen;
  char     message[72];
};

static portMUX_TYPE         gCommitUiMux = portMUX_INITIALIZER_UNLOCKED;
static SettingsCommitUiState gCommitUi{};

// Group buckets for the active module (built on drill-in). Each is kGroupUngrouped
// or a SettingEntry.group pointer.
#define SET_MAX_BUCKETS 48
EXT_RAM_BSS_ATTR static const char* gBuckets[SET_MAX_BUCKETS];
static size_t      gBucketCount = 0;

// -----------------------------------------------------------------------------
// Row buffer + per-page caps
// -----------------------------------------------------------------------------
// Single-fragment ceiling on encoded pb body is 253 B; the firmware does not
// reassemble multi-fragment CREATE, so every list page must fit one fragment.
// Empirically 13 total rows (back + up to ~10 content + prev/next) is the
// proven-safe budget.
//
// The MODULE list is the tightest page: it also carries the "View:" toggle
// row, so its worst case is back + toggle + N modules + prev + next = N + 4.
// To stay within the 13-row budget the module page shows at most 9 modules
// (9 + 4 = 13) — this is what lets a middle page render BOTH prev and next, so
// every module page is reachable. (With 10 modules the 14th row was silently
// dropped, hiding later pages.) Group / entry / pick pages have no toggle row,
// so they can show 10 items (back + 10 + prev + next = 13).
#define SET_VISIBLE_MODULE_ROWS  9   // module rows on the module list page
#define SET_VISIBLE_ENTRY_ROWS   10  // items per page on group / entry / pick pages
#define SET_TOTAL_MODULES_ROWS   13  // buffer height (proven single-fragment budget)
#define SET_ROW_LEN              40

// Buffer sized for the largest list-mode view. JSON view doesn't use
// this buffer at all — it goes through g2ShowTextPage directly with a
// String built on the heap. PSRAM-resident — no DMA / ISR access.
EXT_RAM_BSS_ATTR static char        gRows[SET_TOTAL_MODULES_ROWS][SET_ROW_LEN];
EXT_RAM_BSS_ATTR static const char* gRowPtrs[SET_TOTAL_MODULES_ROWS];

// A command failure is surfaced on the next entries render by temporarily
// replacing its back-row label (the row still performs the normal Back action).
// This keeps the feedback on the lens-applier worker without blocking that
// worker or queueing a TEXT page that would need a second delayed page swap.
static char gCommitFailureBackRow[SET_ROW_LEN] = {0};

// Action tables: map a rendered row index to the semantic item it represents,
// so tap dispatch never re-derives layout (the fragile pattern the Files page
// abandoned). Rebuilt by each build*Rows() call.
static int         gModuleRowItem[SET_TOTAL_MODULES_ROWS]; // registry module index per row (-1 = none)
static const char* gGroupRowItem[SET_TOTAL_MODULES_ROWS];  // group bucket per row (nullptr = none)
static int         gEntryRowItem[SET_TOTAL_MODULES_ROWS];  // module-entry index per row (-1 = none)
static int         gPickRowItem[SET_TOTAL_MODULES_ROWS];   // enum option index per row (-1 = none)

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static void writeBackRow(const char* label) {
  const char* lbl = (label && label[0]) ? label : "<- Back";
  strncpy(gRows[0], lbl, SET_ROW_LEN);
  gRows[0][SET_ROW_LEN - 1] = '\0';
  gRowPtrs[0] = gRows[0];
}

static void writeViewToggleRow(size_t row) {
  // Show the CURRENT view name; tapping flips it. Less ambiguous than
  // showing "tap to switch to JSON" — users glance at the row and see
  // what mode they're in right now.
  snprintf(gRows[row], SET_ROW_LEN, "View: %s",
           gView == SET_VIEW_INTERACTIVE ? "INTERACTIVE" : "JSON");
  gRowPtrs[row] = gRows[row];
}

// Format a setting entry's value into `out`. Truncates safely on
// overflow. Secrets are redacted (isSecret flag in the registry).
static void formatSettingValue(const SettingEntry& e, char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';

  if (e.isSecret) {
    snprintf(out, cap, "<secret>");
    return;
  }

  switch (e.type) {
    case SETTING_BOOL: {
      bool v = *(bool*)e.valuePtr;
      snprintf(out, cap, "%s", v ? "true" : "false");
      break;
    }
    case SETTING_INT: {
      int v = *(int*)e.valuePtr;
      snprintf(out, cap, "%d", v);
      break;
    }
    // Width-correct reads for the explicit uint tags (added 2026-05-18 after
    // the heap-corruption fix). Old code read 4 bytes through a uint8 ptr,
    // showing garbage values in the UI; mostly cosmetic on the read path
    // but the write path (handleSettingCommand) actually corrupted memory.
    case SETTING_U8: {
      snprintf(out, cap, "%u", (unsigned)*(uint8_t*)e.valuePtr);
      break;
    }
    case SETTING_U16: {
      snprintf(out, cap, "%u", (unsigned)*(uint16_t*)e.valuePtr);
      break;
    }
    case SETTING_U32: {
      snprintf(out, cap, "%lu", (unsigned long)*(uint32_t*)e.valuePtr);
      break;
    }
    case SETTING_FLOAT: {
      float v = *(float*)e.valuePtr;
      snprintf(out, cap, "%.3f", (double)v);
      break;
    }
    case SETTING_STRING: {
      const String& v = *(const String*)e.valuePtr;
      snprintf(out, cap, "%s", v.length() > 0 ? v.c_str() : "");
      break;
    }
  }
}

// -----------------------------------------------------------------------------
// Level 1 — module list, paginated
// -----------------------------------------------------------------------------
// Layout per page (variable rows depending on whether prev/next apply):
//   row 0       <- Back                     (always)
//   row 1       View: PRETTY | JSON         (always)
//   row 2..N    [module] (count)            (up to SET_VISIBLE_MODULE_ROWS)
//   row N+1     << Prev page                (only if gModulePage > 0)
//   row N+2     Next page >>                (only if more modules remain)
//
// The prev/next rows are conditional, so a build with ≤10 modules stays
// at 12 rows total (back + toggle + 10) and never shows the pagination
// chrome. Once modules > 10, both nav rows can appear simultaneously
// on middle pages.

// Static state populated by buildModuleRows() so the tap handler knows
// the row layout it just rendered. Avoids re-running the layout math
// in two places.
static size_t gPageFirstModuleRow = 2;     // first module row index in current page
static size_t gPageModuleCount    = 0;     // how many module rows on current page
static int    gPagePrevRow        = -1;    // row idx of "<< Prev" if shown, else -1
static int    gPageNextRow        = -1;    // row idx of "Next >>" if shown, else -1
static size_t gPageStartIdx       = 0;     // registry index of first module on this page

static size_t buildModuleRows() {
  writeBackRow("<- Config");       // row 0 — out of Settings to the Config launcher
  writeViewToggleRow(1);           // row 1 — view switcher
  for (size_t i = 0; i < SET_TOTAL_MODULES_ROWS; i++) gModuleRowItem[i] = -1;

  size_t modCount = 0;
  const SettingsModule** mods = getSettingsModules(modCount);

  G2Paginator p = g2PaginatorPrepare(modCount, SET_VISIBLE_MODULE_ROWS, gModulePage);
  gPageStartIdx       = p.startIdx;
  gPageFirstModuleRow = 2;

  size_t row = 2;
  size_t shown = 0;
  for (size_t mi = p.startIdx; mi < p.endIdx && row < SET_TOTAL_MODULES_ROWS; mi++) {
    const SettingsModule* m = mods[mi];
    if (!m || !m->name) continue;
    snprintf(gRows[row], SET_ROW_LEN, "[%s] (%u)",
             m->name, (unsigned)m->count);
    gRowPtrs[row] = gRows[row];
    gModuleRowItem[row] = (int)mi;
    row++;
    shown++;
  }
  gPageModuleCount = shown;

  if (shown == 0) {
    strncpy(gRows[row], "(no modules)", SET_ROW_LEN);
    gRows[row][SET_ROW_LEN - 1] = '\0';
    gRowPtrs[row] = gRows[row];
    row++;
  }

  row = g2PaginatorWriteChrome(p, gModulePage, row, SET_TOTAL_MODULES_ROWS,
                                &gRows[0][0], SET_ROW_LEN, gRowPtrs);
  gPagePrevRow = p.prevRow;
  gPageNextRow = p.nextRow;
  return row;
}

// Convert a Level-1 tap idx to a registry module index. Returns -1 if
// the tap is on a non-module row (back, toggle, prev/next page).
static int moduleIdxFromTap(uint32_t idx) {
  // Action-table lookup (buildModuleRows records the registry index per row) —
  // robust even if a null/nameless module were skipped mid-page, which the old
  // linear reconstruction would have mis-mapped.
  if (idx >= SET_TOTAL_MODULES_ROWS) return -1;
  return gModuleRowItem[idx];
}

// -----------------------------------------------------------------------------
// Interactive editor — group / entry / pick-list builders + edit machinery
// -----------------------------------------------------------------------------

// True if entry `e` belongs to the active group filter (see kGroupAll /
// kGroupUngrouped sentinels).
static bool settingsEntryInGroup(const SettingEntry& e, const char* group) {
  if (group == kGroupAll) return true;
  if (group == kGroupUngrouped) return (!e.group || !e.group[0]);
  return (e.group && group && strcmp(e.group, group) == 0);
}

// Display name for a group bucket row.
static const char* settingsBucketLabel(const char* group) {
  if (group == kGroupUngrouped) return "General";
  return group ? group : "?";
}

// Count visible entries in a bucket (for the "[group] (N)" row).
static size_t settingsBucketCount(const SettingsModule* m, const char* group) {
  size_t n = 0;
  for (size_t ei = 0; ei < m->count; ei++) {
    const SettingEntry& e = m->entries[ei];
    if (!settingsEditorIsVisible(&e)) continue;
    if (settingsEntryInGroup(e, group)) n++;
  }
  return n;
}

// Populate gBuckets/gBucketCount/gModuleHasGroups for a module. A module gets a
// group level only when it has >=2 buckets (distinct named groups plus a
// synthetic "General" bucket for any ungrouped entries). Otherwise it is flat.
static void settingsBuildBuckets(const SettingsModule* m) {
  gBucketCount = 0;
  gModuleHasGroups = false;
  if (!m || !m->entries) return;

  bool sawUngrouped = false;
  for (size_t ei = 0; ei < m->count; ei++) {
    const SettingEntry& e = m->entries[ei];
    if (!settingsEditorIsVisible(&e)) continue;
    const char* g = e.group;
    if (!g || !g[0]) { sawUngrouped = true; continue; }
    bool seen = false;
    for (size_t b = 0; b < gBucketCount; b++) {
      if (gBuckets[b] != kGroupUngrouped && strcmp(gBuckets[b], g) == 0) { seen = true; break; }
    }
    if (!seen && gBucketCount < SET_MAX_BUCKETS) gBuckets[gBucketCount++] = g;
  }

  const size_t total = gBucketCount + (sawUngrouped ? 1 : 0);
  gModuleHasGroups = (total >= 2);
  if (gModuleHasGroups && sawUngrouped && gBucketCount < SET_MAX_BUCKETS) {
    // Prepend the synthetic "General" bucket for the ungrouped entries.
    for (size_t b = gBucketCount; b > 0; b--) gBuckets[b] = gBuckets[b - 1];
    gBuckets[0] = kGroupUngrouped;
    gBucketCount++;
  }
}

// Level 2a — group list for a grouped module.
static size_t buildGroupRows(int moduleIdx) {
  writeBackRow("<- Settings");
  for (size_t i = 0; i < SET_TOTAL_MODULES_ROWS; i++) gGroupRowItem[i] = nullptr;

  size_t modCount = 0;
  const SettingsModule** mods = getSettingsModules(modCount);
  const SettingsModule* m = (moduleIdx >= 0 && (size_t)moduleIdx < modCount) ? mods[moduleIdx] : nullptr;
  if (!m) {
    snprintf(gRows[1], SET_ROW_LEN, "(invalid)");
    gRowPtrs[1] = gRows[1];
    return 2;
  }

  G2Paginator p = g2PaginatorPrepare(gBucketCount, SET_VISIBLE_ENTRY_ROWS, gGroupPage);
  size_t row = 1;
  for (size_t bi = p.startIdx; bi < p.endIdx && row < SET_TOTAL_MODULES_ROWS; bi++) {
    const char* g = gBuckets[bi];
    snprintf(gRows[row], SET_ROW_LEN, "[%s] (%u)",
             settingsBucketLabel(g), (unsigned)settingsBucketCount(m, g));
    gRowPtrs[row] = gRows[row];
    gGroupRowItem[row] = g;
    row++;
  }
  row = g2PaginatorWriteChrome(p, gGroupPage, row, SET_TOTAL_MODULES_ROWS,
                                &gRows[0][0], SET_ROW_LEN, gRowPtrs);
  gPagePrevRow = p.prevRow;
  gPageNextRow = p.nextRow;
  return row;
}

// Level 2b — entries of one module, filtered by the active group. Two passes
// over the (possibly non-contiguous) filtered set so pagination is exact; the
// row->entry map is recorded for tap dispatch.
static size_t buildEntryRows(int moduleIdx, const char* group) {
  if (gCommitFailureBackRow[0]) {
    writeBackRow(gCommitFailureBackRow);
    gCommitFailureBackRow[0] = '\0';
  } else {
    writeBackRow(gModuleHasGroups ? "<- Groups" : "<- Settings");
  }
  for (size_t i = 0; i < SET_TOTAL_MODULES_ROWS; i++) gEntryRowItem[i] = -1;

  size_t modCount = 0;
  const SettingsModule** mods = getSettingsModules(modCount);
  const SettingsModule* m = (moduleIdx >= 0 && (size_t)moduleIdx < modCount) ? mods[moduleIdx] : nullptr;
  if (!m || !m->entries || m->count == 0) {
    snprintf(gRows[1], SET_ROW_LEN, "(empty: %s)", m && m->name ? m->name : "?");
    gRowPtrs[1] = gRows[1];
    return 2;
  }

  // Pass 1 — count matches (visible + in group).
  size_t matchCount = 0;
  for (size_t ei = 0; ei < m->count; ei++) {
    const SettingEntry& e = m->entries[ei];
    if (!settingsEditorIsVisible(&e)) continue;
    if (!settingsEntryInGroup(e, group)) continue;
    matchCount++;
  }

  G2Paginator p = g2PaginatorPrepare(matchCount, SET_VISIBLE_ENTRY_ROWS, gEntryPage);

  // Pass 2 — render the [startIdx, endIdx) slice of the filtered set.
  size_t row = 1;
  size_t mi = 0;
  for (size_t ei = 0; ei < m->count && row < SET_TOTAL_MODULES_ROWS; ei++) {
    const SettingEntry& e = m->entries[ei];
    if (!settingsEditorIsVisible(&e)) continue;
    if (!settingsEntryInGroup(e, group)) continue;
    if (mi < p.startIdx) { mi++; continue; }
    if (mi >= p.endIdx) break;
    char val[20];
    formatSettingValue(e, val, sizeof(val));
    // Prefer the human label — debug entries all share jsonKey "enabled", so
    // jsonKey alone would render every row identically.
    const char* key = e.label ? e.label : (e.jsonKey ? e.jsonKey : "?");
    snprintf(gRows[row], SET_ROW_LEN, "%s=%s", key, val);
    gRowPtrs[row] = gRows[row];
    gEntryRowItem[row] = (int)ei;
    row++;
    mi++;
  }

  row = g2PaginatorWriteChrome(p, gEntryPage, row, SET_TOTAL_MODULES_ROWS,
                                &gRows[0][0], SET_ROW_LEN, gRowPtrs);
  gPagePrevRow = p.prevRow;
  gPageNextRow = p.nextRow;
  return row;
}

// Level 3 — enum pick-list for the entry being edited. "[X]" marks the option
// matching the current value.
static size_t buildPickRows(const SettingEntry* e) {
  writeBackRow("<- Cancel");
  for (size_t i = 0; i < SET_TOTAL_MODULES_ROWS; i++) gPickRowItem[i] = -1;
  if (!e || !e->options) {
    snprintf(gRows[1], SET_ROW_LEN, "(no options)");
    gRowPtrs[1] = gRows[1];
    return 2;
  }

  const int n = settingsEditorEnumCount(e->options);
  const int curIdx = settingsEditorEnumIndexForCurrent(e);
  G2Paginator p = g2PaginatorPrepare((size_t)n, SET_VISIBLE_ENTRY_ROWS, gPickPage);
  size_t row = 1;
  for (size_t oi = p.startIdx; oi < p.endIdx && row < SET_TOTAL_MODULES_ROWS; oi++) {
    char lab[32];
    if (!settingsEditorEnumAt(e->options, (int)oi, nullptr, 0, lab, sizeof(lab))) continue;
    snprintf(gRows[row], SET_ROW_LEN, "%s %s", ((int)oi == curIdx) ? "[X]" : "[ ]", lab);
    gRowPtrs[row] = gRows[row];
    gPickRowItem[row] = (int)oi;
    row++;
  }
  row = g2PaginatorWriteChrome(p, gPickPage, row, SET_TOTAL_MODULES_ROWS,
                                &gRows[0][0], SET_ROW_LEN, gRowPtrs);
  gPagePrevRow = p.prevRow;
  gPageNextRow = p.nextRow;
  return row;
}

// Format an entry's current value into an editable string for keyboard prefill.
static void settingsFormatValueForEdit(const SettingEntry* e, char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  switch (e->type) {
    case SETTING_FLOAT: snprintf(out, cap, "%.3f", (double)*(float*)e->valuePtr); break;
    case SETTING_U32:   snprintf(out, cap, "%lu", (unsigned long)*(uint32_t*)e->valuePtr); break;
    case SETTING_INT:
    case SETTING_U8:
    case SETTING_U16:   snprintf(out, cap, "%d", settingsEditorCurrentValue(e)); break;
    default:            break;  // STRING handled by the caller
  }
}

static void settingsClearCommitLocked() {
  gCommitUi.pending = false;
  gCommitUi.resultReady = false;
  gCommitUi.success = false;
  gCommitUi.navGen = 0;
  gCommitUi.message[0] = '\0';
}

// Start a new display generation. The local generation catches navigation
// among Settings' four sub-levels (which all share one hijack-page enum); the
// global generation lets the lens-applier drop a completion that was queued
// just before that navigation. Any commit tied to the old view is now stale.
static void settingsBeginRender() {
  gNavGen++;
  g2BumpMenuGen();
  portENTER_CRITICAL(&gCommitUiMux);
  if (gCommitUi.pending && gCommitUi.navGen != gNavGen) {
    settingsClearCommitLocked();
  }
  portEXIT_CRITICAL(&gCommitUiMux);
}

// --- Render helpers. Each (a) bumps both Settings-local and lens-global
// generations so any in-flight commit redraw becomes stale once the display
// changes, and (b) re-asserts the hijack page, since a preceding keyboard
// session leaves it at TEXT_VIEW and g2ShowListPage never sets it. ---

static void settingsShowModules() {
  settingsBeginRender();
  size_t n = buildModuleRows();
  g2SetHijackPage(G2_HIJACK_PAGE_SETTINGS);
  g2ShowListPage(gRowPtrs, n);
}

static void settingsRenderGroups() {
  settingsBeginRender();
  size_t n = buildGroupRows(gActiveModule);
  g2SetHijackPage(G2_HIJACK_PAGE_SETTINGS);
  g2ShowListPage(gRowPtrs, n);
}

static void settingsRenderEntries() {
  settingsBeginRender();
  size_t n = buildEntryRows(gActiveModule, gActiveGroup);
  g2SetHijackPage(G2_HIJACK_PAGE_SETTINGS);
  g2ShowListPage(gRowPtrs, n);
}

static void settingsRenderPick() {
  settingsBeginRender();
  size_t n = buildPickRows(gEditEntry);
  g2SetHijackPage(G2_HIJACK_PAGE_SETTINGS);
  g2ShowListPage(gRowPtrs, n);
}

// Flash a short message, then return to the entry list. This is only for
// synchronous pre-flight refusals on g2_tap_disp; async command completions
// marshal their UI work to the lens-applier worker below.
static void settingsShowBannerThenEntries(const char* msg) {
  g2ShowText(msg);
  vTaskDelay(pdMS_TO_TICKS(1300));
  settingsRenderEntries();
}

// Match executeCommand's result contract. Successful human results are
// stamped "OK:" unless they already begin with SUCCESS; structured JSON is
// deliberately left byte-exact. `ok` alone is insufficient because command
// lookup currently returns true with an "Unknown command" result.
static bool settingsCommitSucceeded(bool ok, const char* result) {
  if (!ok || !result) return false;
  while (*result == ' ' || *result == '\t' || *result == '\r' || *result == '\n') result++;
  if (!result[0]) return false;
  if (strncmp(result, "Error", 5) == 0 ||
      strncmp(result, "ERROR", 5) == 0 ||
      strncmp(result, "Unknown command", 15) == 0) {
    return false;
  }
  if (strncmp(result, "OK", 2) == 0 || strncmp(result, "SUCCESS", 7) == 0) {
    return true;
  }
  // executeCommand preserves structured results instead of prefixing them.
  // A registered setting command that completed with a JSON object/array is
  // therefore a valid completion, not an error banner.
  return result[0] == '{' || result[0] == '[';
}

static bool settingsTryBeginCommit(uint32_t navGen) {
  bool accepted = false;
  portENTER_CRITICAL(&gCommitUiMux);
  // A pending result from an older view is already stale and can be replaced.
  if (!gCommitUi.pending || gCommitUi.navGen != navGen) {
    settingsClearCommitLocked();
    gCommitUi.pending = true;
    gCommitUi.navGen = navGen;
    accepted = true;
  }
  portEXIT_CRITICAL(&gCommitUiMux);
  return accepted;
}

static void settingsClearCommitForGeneration(uint32_t navGen) {
  portENTER_CRITICAL(&gCommitUiMux);
  if (gCommitUi.pending && gCommitUi.navGen == navGen) {
    settingsClearCommitLocked();
  }
  portEXIT_CRITICAL(&gCommitUiMux);
}

static bool settingsPublishCommitResult(uint32_t navGen, bool success,
                                        const char* message) {
  char copy[sizeof(gCommitUi.message)] = {0};
  snprintf(copy, sizeof(copy), "%s", message ? message : "Save failed");

  bool published = false;
  portENTER_CRITICAL(&gCommitUiMux);
  if (gCommitUi.pending && gCommitUi.navGen == navGen) {
    gCommitUi.success = success;
    gCommitUi.resultReady = true;
    memcpy(gCommitUi.message, copy, sizeof(gCommitUi.message));
    published = true;
  }
  portEXIT_CRITICAL(&gCommitUiMux);
  return published;
}

static bool settingsConsumeCommitResult(bool expectedSuccess,
                                        char* message, size_t messageCap) {
  bool consumed = false;
  portENTER_CRITICAL(&gCommitUiMux);
  if (gCommitUi.pending && gCommitUi.resultReady &&
      gCommitUi.navGen == gNavGen &&
      gCommitUi.success == expectedSuccess) {
    if (message && messageCap > 0) {
      const size_t take = messageCap < sizeof(gCommitUi.message)
                            ? messageCap : sizeof(gCommitUi.message);
      memcpy(message, gCommitUi.message, take);
      message[take - 1] = '\0';
    }
    settingsClearCommitLocked();
    consumed = true;
  }
  portEXIT_CRITICAL(&gCommitUiMux);
  return consumed;
}

// These render functions run only on the lens-applier worker. They repeat the
// Settings-local generation/page checks because all four Settings sub-levels
// share one G2HijackPage value and a navigation can race queue dispatch.
static void settingsRenderCommitSuccess() {
  if (g2GetHijackPage() != G2_HIJACK_PAGE_SETTINGS) return;
  if (!settingsConsumeCommitResult(true, nullptr, 0)) return;
  gLevel = SET_LEVEL_ENTRIES;
  settingsRenderEntries();  // value re-reads live from valuePtr
}

static void settingsRenderCommitFailure() {
  if (g2GetHijackPage() != G2_HIJACK_PAGE_SETTINGS) return;
  char message[sizeof(gCommitUi.message)];
  if (!settingsConsumeCommitResult(false, message, sizeof(message))) return;
  gLevel = SET_LEVEL_ENTRIES;
  snprintf(gCommitFailureBackRow, sizeof(gCommitFailureBackRow),
           "<- Failed: %.27s", message);
  settingsRenderEntries();
}

// Marshal command-completion UI work to the single lens-applier worker. The
// command has already executed on cmd_exec_task; this helper never executes or
// re-dispatches it inline.
static bool settingsEnqueueCommitResult(const G2CmdCookie& cookie, bool success) {
  RedrawSpec* spec = new (std::nothrow) RedrawSpec{};
  if (!spec) {
    DEBUG_G2F("[G2] Settings: RedrawSpec alloc failed");
    return false;
  }
  spec->render = success ? settingsRenderCommitSuccess : settingsRenderCommitFailure;

  LensUiJob* job = new (std::nothrow) LensUiJob{};
  if (!job) {
    DEBUG_G2F("[G2] Settings: LensUiJob alloc failed");
    delete spec;
    return false;
  }
  job->kind           = LensJobKind::Redraw;
  job->submitMenuGen  = cookie.menuGen;
  job->cmdSeq         = cookie.seq;
  job->targetPage     = cookie.targetPage;
  job->targetNetSub   = cookie.targetNetSub;
  job->payload.redraw = spec;
  if (!g2EnqueueLensJob(job)) {
    DEBUG_G2F("[G2] Settings: commit redraw enqueue failed");
    delete spec;
    delete job;
    return false;
  }
  return true;
}

// cmd_exec completion callback for a setting mutation. userData carries the
// gNavGen captured at submit time. This callback publishes state and enqueues
// a Redraw only; all lens access stays on the lens-applier worker.
static void onSettingsCommitDone(bool ok, const char* result,
                                 const G2CmdCookie& cookie, void* userData) {
  const uint32_t submitNavGen = (uint32_t)(uintptr_t)userData;
  // Drop the result if the user is no longer on the settings page.
  if (g2GetHijackPage() != G2_HIJACK_PAGE_SETTINGS) {
    settingsClearCommitForGeneration(submitNavGen);
    return;
  }
  // Drop the redraw if the display changed since submit — the user navigated
  // between sub-levels (all share the SETTINGS hijack page, so the page guard
  // above can't catch it) or a newer render ran. The write already happened;
  // only its stale UI refresh is discarded.
  if (submitNavGen != gNavGen) {
    settingsClearCommitForGeneration(submitNavGen);
    return;
  }

  const bool success = settingsCommitSucceeded(ok, result);
  const char* msg = (result && result[0]) ? result : "Save failed";
  if (!success && strncmp(msg, "Error: ", 7) == 0) msg += 7;
  if (!settingsPublishCommitResult(submitNavGen, success, msg)) return;
  if (!settingsEnqueueCommitResult(cookie, success)) {
    settingsClearCommitForGeneration(submitNavGen);
  }
}

// Build "<command> <value>" and submit through cmd_exec (auth + audit as the
// paired user). The hijack page must already be SETTINGS before this is called
// (the keyboard path re-asserts it first) so the completion guard passes.
static void settingsCommit(const SettingEntry* e, const char* value) {
  const char* cmdName = settingsEditorCommandName(e);
  if (!cmdName || !cmdName[0]) { settingsShowBannerThenEntries("No command"); return; }
  // Empty args mean "show current value" to the command handler — a no-op that
  // stamps "OK" and would read as a successful save. Refuse it (covers an empty
  // enum value token; the keyboard path also refuses empty input up front).
  if (!value || !value[0]) { settingsShowBannerThenEntries("Empty - not saved"); return; }
  const uint32_t submitNavGen = gNavGen;
  if (!settingsTryBeginCommit(submitNavGen)) {
    DEBUG_G2F("[G2] Settings: save already pending for navGen=%u",
              (unsigned)submitNavGen);
    return;
  }
  char line[96];
  snprintf(line, sizeof(line), "%s %s", cmdName, value);
  G2CmdCookie cookie = { 0, 0, g2GetHijackPage(), 0 };
  if (!g2SubmitHijackCommand(line, cookie, onSettingsCommitDone,
                             (void*)(uintptr_t)submitNavGen)) {
    settingsClearCommitForGeneration(submitNavGen);
    settingsShowBannerThenEntries("Busy - try again");
  }
}

// Keyboard commit/cancel (run on g2_tap_disp). The keyboard teardown leaves the
// hijack page at TEXT_VIEW, so both re-assert SETTINGS before doing anything.
static void onSettingsKeyboardCommit(const char* text) {
  g2SetHijackPage(G2_HIJACK_PAGE_SETTINGS);
  gLevel = SET_LEVEL_ENTRIES;
  if (!gEditEntry) { settingsRenderEntries(); return; }

  char buf[40];
  snprintf(buf, sizeof(buf), "%s", text ? text : "");
  char* s = buf;
  while (*s == ' ') s++;
  size_t len = strlen(s);
  while (len > 0 && s[len - 1] == ' ') s[--len] = '\0';
  if (len == 0) {
    // Empty args mean "show current value" to the command handler, so an empty
    // commit is a silent no-op — refuse it (strings cannot be cleared here).
    settingsShowBannerThenEntries("Empty - not saved");
    return;
  }
  settingsCommit(gEditEntry, s);
}

static void onSettingsKeyboardCancel(void) {
  g2SetHijackPage(G2_HIJACK_PAGE_SETTINGS);
  gLevel = SET_LEVEL_ENTRIES;
  settingsRenderEntries();
}

// Launch the character keyboard prefilled with the entry's current value.
static void settingsLaunchKeyboard(const SettingEntry* e) {
  char cur[40];
  if (e->type == SETTING_STRING) {
    const String& s = *((String*)e->valuePtr);
    if (s.length() > 32) { settingsShowBannerThenEntries("Too long - edit on web"); return; }
    if (s.indexOf('"') >= 0) { settingsShowBannerThenEntries("Has quote - edit on web"); return; }
    snprintf(cur, sizeof(cur), "%s", s.c_str());
  } else {
    settingsFormatValueForEdit(e, cur, sizeof(cur));
  }

  TextEntryConfig cfg;
  cfg.prompt   = e->label ? e->label : (e->jsonKey ? e->jsonKey : "value");
  cfg.initial  = cur;
  cfg.maxLen   = 32;
  cfg.onCommit = onSettingsKeyboardCommit;
  cfg.onCancel = onSettingsKeyboardCancel;
  if (!g2BeginTextEntry(cfg)) {
    settingsShowBannerThenEntries("Keyboard busy");
  }
}

// Decide how to edit a tapped entry: refuse the non-editable kinds, flip a
// boolean, open an enum pick-list, or launch the keyboard.
static void settingsEditEntry(int moduleIdx, int entryIdx) {
  size_t modCount = 0;
  const SettingsModule** mods = getSettingsModules(modCount);
  if (moduleIdx < 0 || (size_t)moduleIdx >= modCount) return;
  const SettingsModule* m = mods[moduleIdx];
  if (!m || entryIdx < 0 || (size_t)entryIdx >= m->count) return;
  const SettingEntry& e = m->entries[entryIdx];

  if (e.readOnly) { settingsShowBannerThenEntries("Read-only"); return; }
  if (e.isSecret) { settingsShowBannerThenEntries("Secret - edit on web"); return; }
  if (e.options && strncmp(e.options, "bitmask:", 8) == 0) {
    settingsShowBannerThenEntries("Bitmask - edit on web");
    return;
  }
  if (!settingsEditorHasCommand(&e)) { settingsShowBannerThenEntries("No command"); return; }

  gEditEntry = &e;

  if (e.type == SETTING_BOOL) {
    const int cur = settingsEditorCurrentValue(&e);
    settingsCommit(&e, cur ? "0" : "1");   // page already SETTINGS; stays on entries
    return;
  }
  if (settingsEditorHasEnumOptions(&e)) {
    gLevel = SET_LEVEL_PICK;
    gPickPage = 0;
    settingsRenderPick();
    return;
  }
  settingsLaunchKeyboard(&e);
}

// -----------------------------------------------------------------------------
// Level 2 JSON — serialize one module as JSON and render via TEXT widget
// -----------------------------------------------------------------------------
// Unlike the PRETTY view (which renders entries through the LIST
// widget — one selectable row per key=value), JSON view uses a TEXT
// widget so the content flows freely without per-row selection
// borders. The wrong-tool fit was visible: list widgets draw a
// selection rectangle around every line, and JSON content fragmented
// at arbitrary character boundaries didn't align with those
// rectangles cleanly.
//
// Side-effect of using TEXT widget: there's no "<- Back" row to tap.
// Exit is handled by `gTextViewExitFn` (registered when we call
// g2ShowTextPage) — the next user-input event after the page renders
// returns the user to the module list. See
// G2_Glasses.cpp:dispatchEventPayload for the fallback path and
// the TextEvent CLICK handler in handleDevEvent for the cleaner
// firmware-cooperates path.

// -----------------------------------------------------------------------------
// JSON-view pagination
// -----------------------------------------------------------------------------
// The G2 firmware doesn't reassemble multi-fragment CREATE/REBUILD for
// hijack widgets, so a single TEXT page is bounded to ~250 B pb. Most
// modules' JSON exceeds that — espnow's 42 settings serialize to ~1.5
// KB pretty-printed. Rather than truncating, we split the JSON into
// chunks that fit a single fragment and let the user advance pages by
// tapping. Real exit gestures (sid=0xE0 SysEvent — ring tap, double-
// tap, swipe) still leave the JSON view via the existing exit hook.
//
// State machine:
//   Entry from module list → build full JSON → split into kJsonPages
//                          → render page 0 via CREATE-text
//                          → arm exit fn (back to module list) AND
//                            tap fn (advance page)
//   USER_ACTIVITY post-grace → tap fn fires → page index ++ (wrap)
//                            → rebuild body → g2ShowText (REBUILD_PAGE,
//                              flicker-free)
//   SysEvent CLICK/etc.    → exit fn fires → back to module list

#define JSON_MAX_PAGES     16
#define JSON_PAGE_BUDGET   176   // < 180 proven-safe single-fragment body
#define JSON_BODY_CAP      (JSON_MAX_PAGES * JSON_PAGE_BUDGET + 128)

// Shared TextPager engine (see G2_Page_Common.h) — one flat PSRAM body buffer
// + offset table, hard-wrapped so long JSON string values flow across lens
// lines instead of being clipped at the page budget. Built once per drill-in,
// cleared by the exit fn. gJsonModuleName is the per-page header title.
EXT_RAM_BSS_ATTR static char gJsonBody[JSON_BODY_CAP];            // wrapped body
static uint16_t              gJsonPageOff[JSON_MAX_PAGES + 1];     // page offsets
EXT_RAM_BSS_ATTR static char gJsonPageBuf[JSON_PAGE_BUDGET + 128]; // render scratch
static char     gJsonModuleName[24] = {0};
static TextPager gJsonPager = {
    gJsonBody, gJsonPageOff, JSON_MAX_PAGES, JSON_PAGE_BUDGET,
    /*pageCount=*/0, /*curPage=*/0, /*truncated=*/false };

// Forward decls — exit handler needs the module list builder, the tap
// handler needs the page renderer. Defined out of order intentionally
// so the showModule* function can reference both.
static void rebuildAndShowModuleList();
static void exitJsonViewBackToModuleList();
static void settingsJsonNav(G2TapKind kind);
static bool settingsRenderJsonPage();

static void rebuildAndShowModuleList() {
  size_t n = buildModuleRows();
  if (g2ShowListPage(gRowPtrs, n)) {
    DEBUG_G2F("[G2] Settings: re-shown module list (rows=%u)", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Settings: module-list re-show FAILED");
  }
}

// Fallback exit invoked by G2_Glasses.cpp's user-input handlers.
// Tears down whatever's on the lens and re-CREATEs the module list
// (LIST widget) so the user lands back where they started. Also clears
// the page cache so the next drill-in builds fresh content (settings
// values may have changed).
static void exitJsonViewBackToModuleList() {
  gJsonPager.pageCount = 0;
  gJsonPager.curPage   = 0;
  gJsonPager.truncated = false;
  gJsonModuleName[0]   = '\0';

  gLevel        = SET_LEVEL_MODULES;
  gActiveModule = -1;
  rebuildAndShowModuleList();
}

// Render the pager's current page via the shared engine. Chrome is fixed;
// only gJsonModuleName's contents change per drill-in. Each call is a full
// SHUTDOWN+CREATE (REBUILD-text was unreliable on this firmware, 2026-04-26)
// and re-arms the same exit/nav hooks.
static bool settingsRenderJsonPage() {
  static const G2TextPageChrome chrome = {
      gJsonModuleName,                 // title (mutated in place before render)
      "tap/scroll=nav, 2x-tap=exit",   // multi-page hint
      "2x-tap=exit",                   // single-page hint
      "--------------------",          // separator rule
      "(empty)" };                     // empty-slice text
  return g2TextPagerRender(gJsonPager, gJsonPageBuf, sizeof(gJsonPageBuf),
                           chrome, G2_GEOM_LARGE, exitJsonViewBackToModuleList,
                           settingsJsonNav);
}

// Page navigation. NEXT (lens tap / ring scroll-down) advances, PREV
// (scroll-up) goes back; both wrap at the ends so the user can cycle.
static void settingsJsonNav(G2TapKind kind) {
  textNavPage(gJsonPager, kind != G2_TAP_PAGE_PREV);
  DEBUG_G2F("[G2] Settings JSON: %s → page %d/%d",
            kind == G2_TAP_PAGE_PREV ? "prev" : "next",
            gJsonPager.curPage + 1, gJsonPager.pageCount);
  settingsRenderJsonPage();
}

// Build the JSON content for `moduleIdx`, split into pages, and ship
// the first page to the lens. Returns true if the swap worker was
// successfully spawned. Subsequent pages are rendered by advanceJsonPage
// when the user taps.
static bool showModuleJsonViaTextWidget(int moduleIdx) {
  size_t modCount = 0;
  const SettingsModule** mods = getSettingsModules(modCount);
  if (moduleIdx < 0 || (size_t)moduleIdx >= modCount) {
    DEBUG_G2F("[G2] Settings JSON: invalid moduleIdx=%d", moduleIdx);
    return false;
  }
  const SettingsModule* m = mods[moduleIdx];
  if (!m) {
    DEBUG_G2F("[G2] Settings JSON: null module at idx=%d", moduleIdx);
    return false;
  }

  // Reuse the same builder the web UI calls so on-lens JSON matches
  // /settings exactly. Wasteful — allocates the full tree just to
  // pull one section's slice — but this is a manual user action, not
  // a hot path. PSRAM-allocated so the 8 KB peak doesn't bite DRAM.
  PSRAM_JSON_DOC(fullDoc);
  buildSettingsJsonDoc(fullDoc, /*excludePasswords=*/ true);

  String s;
  s.reserve(1024);
  if (m->jsonSection) {
    // jsonSection may be a dotted path (e.g. "hardware.sensors.camera"),
    // so walk one segment at a time instead of doing a single keyed lookup.
    JsonVariantConst v = fullDoc.as<JsonVariantConst>();
    const char* segStart = m->jsonSection;
    while (true) {
      const char* dot = strchr(segStart, '.');
      size_t segLen = dot ? (size_t)(dot - segStart) : strlen(segStart);
      char segment[64];
      if (segLen == 0 || segLen >= sizeof(segment)) { v = JsonVariantConst(); break; }
      memcpy(segment, segStart, segLen);
      segment[segLen] = '\0';
      v = v[segment];
      if (v.isNull() || !dot) break;
      segStart = dot + 1;
    }
    if (v.isNull()) {
      s = "(no JSON section)";
    } else {
      serializeJsonPretty(v, s);
    }
  } else {
    serializeJsonPretty(fullDoc, s);
  }

  // Stash the module name for the per-page header.
  strncpy(gJsonModuleName, m->name ? m->name : "?",
          sizeof(gJsonModuleName) - 1);
  gJsonModuleName[sizeof(gJsonModuleName) - 1] = '\0';

  // Wrap into the shared body buffer (soft-wraps long JSON values, marks
  // truncation if the section outgrows the displayable body), then page it.
  bool wrapTrunc = false;
  size_t bodyLen = textWrapInto(gJsonBody, sizeof(gJsonBody), s.c_str(),
                                  G2_TEXT_DEFAULT_COLS, /*contIndent=*/0,
                                  /*stripCtrl=*/true, &wrapTrunc);
  gJsonPager.curPage   = 0;
  gJsonPager.truncated = wrapTrunc;
  textSplitPages(gJsonPager, bodyLen);
  if (gJsonPager.pageCount == 0) {
    DEBUG_G2F("[G2] Settings JSON: split produced zero pages "
              "(module='%s', src=%u B)",
              gJsonModuleName, (unsigned)s.length());
    return false;
  }

  DEBUG_G2F("[G2] Settings JSON: '%s' → %d page(s), src=%u B, trunc=%d",
            gJsonModuleName, gJsonPager.pageCount, (unsigned)s.length(),
            gJsonPager.truncated ? 1 : 0);
  return settingsRenderJsonPage();
}

// -----------------------------------------------------------------------------
// Public — text-mode info dump (CLI direct invocation)
// -----------------------------------------------------------------------------

void g2BuildSettingsInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';

  String s;
  s.reserve(512);
  s += "Settings\n";

  size_t modCount = 0;
  const SettingsModule** mods = getSettingsModules(modCount);
  for (size_t mi = 0; mi < modCount; mi++) {
    const SettingsModule* m = mods[mi];
    if (!m) continue;
    char line[64];
    snprintf(line, sizeof(line), "%s (%u)\n",
             m->name ? m->name : "?", (unsigned)m->count);
    s += line;
    if (s.length() > cap - 64) break;
  }

  strncpy(out, s.c_str(), cap - 1);
  out[cap - 1] = '\0';
}

bool g2ShowSettingsPage() {
  char buf[400];
  g2BuildSettingsInfo(buf, sizeof(buf));
  DEBUG_G2F("[G2] Settings page (%u B):\n%s", (unsigned)strlen(buf), buf);
  return g2ShowText(buf);
}

// -----------------------------------------------------------------------------
// Public — list-mode page (interactive)
// -----------------------------------------------------------------------------

void g2ShowSettingsMenu() {
  // Entry from MAIN always lands at the module list. View persists
  // across hijack sessions intentionally — a user who prefers JSON
  // shouldn't have to re-pick it every time.
  gLevel           = SET_LEVEL_MODULES;
  gActiveModule    = -1;
  gActiveGroup     = kGroupAll;
  gModuleHasGroups = false;
  gGroupPage       = 0;
  gPickPage        = 0;
  gEditEntry       = nullptr;

  size_t n = buildModuleRows();
  if (g2ShowListPage(gRowPtrs, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_SETTINGS);
    DEBUG_G2F("[G2] Settings menu shown (modules, view=%s, rows=%u)",
              gView == SET_VIEW_INTERACTIVE ? "INTERACTIVE" : "JSON", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Settings menu show FAILED");
  }
}

void g2SettingsHandleTap(uint32_t idx) {
  switch (gLevel) {

    case SET_LEVEL_MODULES: {
      if (idx == 0) {
        // <- Back: out of Settings, back to the Config launcher (menu reorg —
        // Settings now lives under Config, not as a top-level hijack row).
        // Reset to page 0 so the next entry to Settings starts fresh.
        gModulePage = 0;
        extern void g2ShowConfigMenu();
        g2ShowConfigMenu();
        return;
      }
      if (idx == 1) {
        // View toggle (INTERACTIVE <-> JSON). The current view name is on the
        // toggle row itself, so no other row changes.
        gView = (gView == SET_VIEW_INTERACTIVE) ? SET_VIEW_JSON : SET_VIEW_INTERACTIVE;
        DEBUG_G2F("[G2] Settings: view → %s",
                  gView == SET_VIEW_INTERACTIVE ? "INTERACTIVE" : "JSON");
        settingsShowModules();
        return;
      }
      // Pagination: prev/next page rows. Detect via stored row indices
      // so we don't have to re-derive layout here.
      if ((int)idx == gPagePrevRow) {
        if (gModulePage > 0) gModulePage--;
        settingsShowModules();
        return;
      }
      if ((int)idx == gPageNextRow) {
        gModulePage++;
        settingsShowModules();
        return;
      }
      const int modIdx = moduleIdxFromTap(idx);
      if (modIdx < 0) {
        DEBUG_G2F("[G2] Settings: tap idx=%u not on a module row",
                  (unsigned)idx);
        return;
      }
      gActiveModule = modIdx;
      gEntryPage    = 0;   // entering a new module always lands at page 0

      size_t modCount = 0;
      const SettingsModule** mods = getSettingsModules(modCount);
      const SettingsModule* m = ((size_t)modIdx < modCount) ? mods[modIdx] : nullptr;
      const char* modName = (m && m->name) ? m->name : "?";

      if (gView == SET_VIEW_JSON) {
        // Read-only JSON: TEXT widget, no group level.
        gLevel           = SET_LEVEL_ENTRIES;
        gModuleHasGroups = false;
        gActiveGroup     = kGroupAll;
        if (showModuleJsonViaTextWidget(gActiveModule)) {
          DEBUG_G2F("[G2] Settings: drilled into '%s' (JSON view)", modName);
        } else {
          DEBUG_G2F("[G2] Settings: JSON drill into '%s' FAILED", modName);
          gLevel        = SET_LEVEL_MODULES;
          gActiveModule = -1;
        }
        return;
      }

      // INTERACTIVE: group level if the module has grouped settings, else flat.
      settingsBuildBuckets(m);
      gGroupPage = 0;
      if (gModuleHasGroups) {
        gLevel = SET_LEVEL_GROUPS;
        settingsRenderGroups();
        DEBUG_G2F("[G2] Settings: '%s' → groups (%u buckets)",
                  modName, (unsigned)gBucketCount);
      } else {
        gActiveGroup = kGroupAll;
        gLevel       = SET_LEVEL_ENTRIES;
        settingsRenderEntries();
        DEBUG_G2F("[G2] Settings: '%s' → entries (flat)", modName);
      }
      return;
    }

    case SET_LEVEL_GROUPS: {
      if (idx == 0) {
        gLevel        = SET_LEVEL_MODULES;
        gActiveModule = -1;
        settingsShowModules();
        return;
      }
      if ((int)idx == gPagePrevRow) { if (gGroupPage > 0) gGroupPage--; settingsRenderGroups(); return; }
      if ((int)idx == gPageNextRow) { gGroupPage++; settingsRenderGroups(); return; }
      if (idx < SET_TOTAL_MODULES_ROWS && gGroupRowItem[idx]) {
        gActiveGroup = gGroupRowItem[idx];
        gLevel       = SET_LEVEL_ENTRIES;
        gEntryPage   = 0;
        settingsRenderEntries();
      }
      return;
    }

    case SET_LEVEL_ENTRIES: {
      // JSON view drives its own TEXT-widget nav (exit/nav hooks), so list
      // taps here only ever fire in the INTERACTIVE view.
      if (gView == SET_VIEW_JSON) return;

      if (idx == 0) {
        // Back: up to the group list (grouped module) or the module list.
        if (gModuleHasGroups) {
          gLevel = SET_LEVEL_GROUPS;
          settingsRenderGroups();
        } else {
          gLevel        = SET_LEVEL_MODULES;
          gActiveModule = -1;
          gEntryPage    = 0;
          settingsShowModules();
        }
        return;
      }
      if ((int)idx == gPagePrevRow) { if (gEntryPage > 0) gEntryPage--; settingsRenderEntries(); return; }
      if ((int)idx == gPageNextRow) { gEntryPage++; settingsRenderEntries(); return; }
      if (idx < SET_TOTAL_MODULES_ROWS && gEntryRowItem[idx] >= 0) {
        settingsEditEntry(gActiveModule, gEntryRowItem[idx]);
      }
      return;
    }

    case SET_LEVEL_PICK: {
      if (idx == 0) {
        // Cancel: back to the entry list, no change.
        gLevel = SET_LEVEL_ENTRIES;
        settingsRenderEntries();
        return;
      }
      if ((int)idx == gPagePrevRow) { if (gPickPage > 0) gPickPage--; settingsRenderPick(); return; }
      if ((int)idx == gPageNextRow) { gPickPage++; settingsRenderPick(); return; }
      if (idx < SET_TOTAL_MODULES_ROWS && gPickRowItem[idx] >= 0 && gEditEntry) {
        char val[48];
        if (settingsEditorEnumAt(gEditEntry->options, gPickRowItem[idx],
                                 val, sizeof(val), nullptr, 0)) {
          // Stay at SET_LEVEL_PICK; onSettingsCommitDone lands on entries once
          // the write completes. This keeps a tap in the in-flight window routed
          // as a re-pick rather than an edit against the stale entry row-map.
          settingsCommit(gEditEntry, val);
        }
      }
      return;
    }
  }
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
