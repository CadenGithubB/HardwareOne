// =============================================================================
// G2 glasses — "Settings" page implementation
// =============================================================================
// Two-level navigation, designed to fit every CREATE-list inside a single
// envelope fragment (≤253 pb bytes):
//
//   Level 1 — module list:
//     <- Back
//     View: PRETTY (tap to switch)        ← persists the chosen view into Level 2
//     [crash] (2)
//     [debug] (89)
//     [output] (6)
//     ...
//     ... +N more (web UI)                ← if module count exceeds the cap
//
//   Level 2 — entries of one module, rendered in the chosen view:
//     PRETTY:  one "key=value" row per entry, paginated when the
//              module has more entries than fit on one page (same
//              "<< Prev / Next >> (p/total)" chrome as Level 1).
//     JSON:    serialized JSON for THIS module, split into ~180 B
//              chunks at line boundaries and shown in a TEXT widget.
//              First page goes via CREATE-text; tap advances to the
//              next page via REBUILD_PAGE (Cmd=7) — flicker-free, no
//              widget churn. Real exit gestures (sid=0xE0 SysEvent —
//              ring tap, double-tap, swipe) leave the JSON view; lens
//              taps cycle pages with wrap. See the JSON-view
//              pagination block below for the state machine.
//
// Why pagination instead of multi-fragment CREATE: empirical testing
// against firmware 2.1.1.10 + 2.2.0.242 showed the EvenCore reassembler
// does NOT rebuild Cmd=0 messages from multiple same-seq fragments —
// single-frag CREATE works, multi-frag CREATE never acks. (The
// reassembler IS used for image streaming on Cmd=3, just not the
// page-creation path.) See docs/G2_PROTOCOL.md "Hijack page-swap
// lifecycle" for the test evidence.
//
// We deliberately don't try to support editing — the G2 lens has no
// keyboard or dial, so read-only is the honest contract.

#include "G2_Page_Settings.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_Glasses.h"
#include "System_Settings.h"
#include "System_Debug.h"
#include "System_MemUtil.h"   // PSRAM_JSON_DOC
#include "G2_Page_Common.h"
#include <ArduinoJson.h>
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR

// -----------------------------------------------------------------------------
// Navigation + view state
// -----------------------------------------------------------------------------

enum SettingsLevel : uint8_t {
  SET_LEVEL_MODULES = 0,    // module-list view (root)
  SET_LEVEL_ENTRIES = 1,    // inside a module, rendered per gView
};

enum SettingsView : uint8_t {
  SET_VIEW_PRETTY = 0,      // key=value rows
  SET_VIEW_JSON   = 1,      // serialized JSON, chunked
};

static SettingsLevel gLevel        = SET_LEVEL_MODULES;
static SettingsView  gView         = SET_VIEW_PRETTY;   // persists across drill-in/out
static int           gActiveModule = -1;                // registry index when in LEVEL_ENTRIES
static size_t        gModulePage   = 0;                 // 0-based page index for module list
static size_t        gEntryPage    = 0;                 // 0-based page index for entries view

// -----------------------------------------------------------------------------
// Row buffer + per-page caps
// -----------------------------------------------------------------------------
// Single-fragment ceiling on encoded pb body is 253 B. Each list row
// costs ~3 B pb overhead (item-name tag + length varint) on top of its
// content; container metadata costs ~50 B.
//
// Module list rows include the entry count: "[modulename] (NN)" — about
// 14 chars average. The cap below is "what fits single-frag with that
// format, plus the back row, plus the view-toggle row, plus a possible
// +N-more trailer." If the registry grows past the visible cap, the
// trailer points the user to the web UI for the rest.
//
// Entry rows ("key=value") are typically shorter; the entry cap is set
// by what fits visually, not pb size.
#define SET_VISIBLE_MODULE_ROWS  10  // module rows on Level-1 page
#define SET_VISIBLE_ENTRY_ROWS   10  // entry rows on Level-2 PRETTY page
#define SET_TOTAL_MODULES_ROWS   13  // back + toggle + 10 modules + prev/next
#define SET_ROW_LEN              40

// Buffer sized for the largest list-mode view. JSON view doesn't use
// this buffer at all — it goes through g2ShowTextPage directly with a
// String built on the heap. PSRAM-resident — no DMA / ISR access.
EXT_RAM_BSS_ATTR static char        gRows[SET_TOTAL_MODULES_ROWS][SET_ROW_LEN];
EXT_RAM_BSS_ATTR static const char* gRowPtrs[SET_TOTAL_MODULES_ROWS];

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
           gView == SET_VIEW_PRETTY ? "PRETTY" : "JSON");
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
      // Registry stores several integer-ish fields as raw void*
      // (uint8_t / uint16_t / uint32_t / int). Read as int — the type
      // system here is too loose to do better without extending
      // SettingEntry. Good enough for read-only display.
      int v = *(int*)e.valuePtr;
      snprintf(out, cap, "%d", v);
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
  writeBackRow("<- Main Menu");    // row 0 — out of Settings to hijack root
  writeViewToggleRow(1);           // row 1 — view switcher

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
  if ((int)idx == gPagePrevRow) return -1;
  if ((int)idx == gPageNextRow) return -1;
  if (idx < gPageFirstModuleRow) return -1;
  const size_t pos = idx - gPageFirstModuleRow;
  if (pos >= gPageModuleCount) return -1;
  return (int)(gPageStartIdx + pos);
}

// -----------------------------------------------------------------------------
// Level 2 PRETTY — entries of one module as key=value rows
// -----------------------------------------------------------------------------

static size_t buildEntryRowsPretty(int moduleIdx) {
  writeBackRow("<- Settings");    // back to module list

  size_t modCount = 0;
  const SettingsModule** mods = getSettingsModules(modCount);
  if (moduleIdx < 0 || (size_t)moduleIdx >= modCount) {
    strncpy(gRows[1], "(invalid)", SET_ROW_LEN);
    gRows[1][SET_ROW_LEN - 1] = '\0';
    gRowPtrs[1] = gRows[1];
    return 2;
  }

  const SettingsModule* m = mods[moduleIdx];
  if (!m || !m->entries || m->count == 0) {
    snprintf(gRows[1], SET_ROW_LEN, "(empty: %s)", m && m->name ? m->name : "?");
    gRowPtrs[1] = gRows[1];
    return 2;
  }

  // Same paginator as Level 1, just over the module's entry array.
  G2Paginator p = g2PaginatorPrepare(m->count, SET_VISIBLE_ENTRY_ROWS, gEntryPage);

  size_t row = 1;
  for (size_t ei = p.startIdx; ei < p.endIdx && row < SET_TOTAL_MODULES_ROWS; ei++) {
    const SettingEntry& e = m->entries[ei];
    char val[20];
    formatSettingValue(e, val, sizeof(val));
    const char* key = e.jsonKey ? e.jsonKey : "?";
    snprintf(gRows[row], SET_ROW_LEN, "%s=%s", key, val);
    gRowPtrs[row] = gRows[row];
    row++;
  }

  row = g2PaginatorWriteChrome(p, gEntryPage, row, SET_TOTAL_MODULES_ROWS,
                                &gRows[0][0], SET_ROW_LEN, gRowPtrs);
  // Reuse the same dispatcher hooks as Level 1 — the SET_LEVEL_ENTRIES
  // case below detects prev/next via gPagePrevRow / gPageNextRow.
  gPagePrevRow = p.prevRow;
  gPageNextRow = p.nextRow;
  return row;
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

#define JSON_MAX_PAGES         16
#define JSON_PAGE_BODY_BUDGET 180   // chars/page; leaves headroom for
                                    // the header + footer (~50 B) and
                                    // pb framing (~20 B) within the
                                    // ~250 B single-fragment ceiling.

// Cached page bodies + metadata. Built once per drill-in, kept in
// memory until the user exits the JSON view (cleared by the exit fn).
// Array-of-Strings rather than a single big buffer because the strings
// can have variable length up to JSON_PAGE_BODY_BUDGET.
static String   gJsonPages[JSON_MAX_PAGES];
static size_t   gJsonPageCount   = 0;
static size_t   gJsonCurrentPage = 0;
static char     gJsonModuleName[24] = {0};

// Forward decls — exit handler needs the module list builder, the tap
// handler needs the page renderer. Defined out of order intentionally
// so the showModule* function can reference both.
static void rebuildAndShowModuleList();
static void exitJsonViewBackToModuleList();
static void navJsonPage(G2TapKind kind);
static bool renderCurrentJsonPage(bool isInitialCreate);

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
  for (size_t i = 0; i < gJsonPageCount; i++) gJsonPages[i] = "";
  gJsonPageCount   = 0;
  gJsonCurrentPage = 0;
  gJsonModuleName[0] = '\0';

  gLevel        = SET_LEVEL_MODULES;
  gActiveModule = -1;
  rebuildAndShowModuleList();
}

// Render gJsonPages[gJsonCurrentPage] on the lens. On the first call
// (isInitialCreate), goes through g2ShowTextPage to do the
// SHUTDOWN+CREATE handshake and arm the exit/tap callbacks. Subsequent
// calls reuse the existing TEXT widget via g2ShowText, which routes to
// REBUILD_PAGE (Cmd=7) — flicker-free, no widget churn.
static bool renderCurrentJsonPage(bool isInitialCreate) {
  if (gJsonPageCount == 0 || gJsonCurrentPage >= gJsonPageCount) return false;

  // Header: module name + page indicator + tap hint. Footer is the
  // page indicator only (so users at the bottom of a long page can
  // still see where they are without scrolling up).
  String body;
  body.reserve(gJsonPages[gJsonCurrentPage].length() + 96);
  body += gJsonModuleName;
  if (gJsonPageCount > 1) {
    char hdr[64];
    snprintf(hdr, sizeof(hdr), " [%u/%u] tap/scroll=nav, 2x-tap=exit\n",
             (unsigned)(gJsonCurrentPage + 1), (unsigned)gJsonPageCount);
    body += hdr;
  } else {
    body += " - tap to back\n";
  }
  body += "--------------------\n";
  body += gJsonPages[gJsonCurrentPage];

  // Always go through g2ShowTextPage (full SHUTDOWN+CREATE handshake).
  // The "use REBUILD_PAGE for second-and-subsequent" optimisation broke
  // on this firmware (2026-04-26): REBUILD-text never gets a Cmd=8
  // RebuildResp ack and the BLE plugin emits "Connection lost" on the
  // lens after a few page advances. SHUTDOWN+CREATE costs ~700 ms per
  // page but is reliable. Re-arm the same exitFn / tapFn each time so
  // the dispatch hooks stay live across swaps.
  (void)isInitialCreate;
  return g2ShowTextPage(body.c_str(), G2_GEOM_LARGE,
                        exitJsonViewBackToModuleList,
                        gJsonPageCount > 1 ? navJsonPage : nullptr);
}

// Page-navigation handler for paginated TEXT view. Receives a directional
// hint from the wire layer:
//   G2_TAP_PAGE_NEXT — lens tap or ring scroll-down → forward
//   G2_TAP_PAGE_PREV — ring scroll-up               → backward
// Both directions wrap at the ends so the user can cycle.
static void navJsonPage(G2TapKind kind) {
  if (gJsonPageCount == 0) return;
  if (kind == G2_TAP_PAGE_PREV) {
    gJsonCurrentPage = (gJsonCurrentPage == 0)
                         ? gJsonPageCount - 1
                         : gJsonCurrentPage - 1;
  } else {
    gJsonCurrentPage = (gJsonCurrentPage + 1) % gJsonPageCount;
  }
  DEBUG_G2F("[G2] Settings JSON: %s → page %u/%u",
            kind == G2_TAP_PAGE_PREV ? "prev" : "next",
            (unsigned)(gJsonCurrentPage + 1), (unsigned)gJsonPageCount);
  renderCurrentJsonPage(/*isInitialCreate=*/false);
}

// Split `s` into gJsonPages[] chunks of <= JSON_PAGE_BODY_BUDGET
// characters each, breaking only at newline boundaries so JSON lines
// stay intact. Returns the number of pages produced. If the source
// would overflow JSON_MAX_PAGES, the remaining content is appended to
// the last page (truncated only at JSON_PAGE_BODY_BUDGET there).
static size_t splitJsonIntoPages(const String& s) {
  for (size_t i = 0; i < JSON_MAX_PAGES; i++) gJsonPages[i] = "";

  size_t pageIdx   = 0;
  size_t lineStart = 0;
  String current;
  current.reserve(JSON_PAGE_BODY_BUDGET + 16);

  const size_t total = s.length();
  while (lineStart < total && pageIdx < JSON_MAX_PAGES) {
    int lineEnd = s.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = total;
    const size_t lineLen = (size_t)lineEnd - lineStart + 1;  // include '\n'

    // If adding this line would overflow the budget AND the current
    // page already has content, flush current and start a new page.
    if (current.length() > 0 &&
        current.length() + lineLen > JSON_PAGE_BODY_BUDGET) {
      gJsonPages[pageIdx++] = current;
      current = "";
      if (pageIdx >= JSON_MAX_PAGES) break;
    }

    // Pathological case: a single line longer than the budget. Hard-cut
    // it at the budget boundary — these are JSON values, not human
    // sentences, so a mid-string break is acceptable.
    size_t copyLen = lineLen;
    if (copyLen > JSON_PAGE_BODY_BUDGET) copyLen = JSON_PAGE_BODY_BUDGET;
    current += s.substring(lineStart, lineStart + copyLen);
    lineStart += copyLen;
    // If we hard-cut, restart from the cut point; the next iteration
    // picks up where we left off without expecting a newline.
    if (copyLen < lineLen) continue;
    lineStart = (size_t)lineEnd + 1;
  }

  if (current.length() > 0 && pageIdx < JSON_MAX_PAGES) {
    gJsonPages[pageIdx++] = current;
  }
  return pageIdx;
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
    JsonVariant v = fullDoc[m->jsonSection];
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

  gJsonPageCount   = splitJsonIntoPages(s);
  gJsonCurrentPage = 0;
  if (gJsonPageCount == 0) {
    DEBUG_G2F("[G2] Settings JSON: split produced zero pages "
              "(module='%s', src=%u B)",
              gJsonModuleName, (unsigned)s.length());
    return false;
  }

  DEBUG_G2F("[G2] Settings JSON: '%s' → %u page(s), src=%u B",
            gJsonModuleName, (unsigned)gJsonPageCount, (unsigned)s.length());
  return renderCurrentJsonPage(/*isInitialCreate=*/true);
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
  gLevel        = SET_LEVEL_MODULES;
  gActiveModule = -1;

  size_t n = buildModuleRows();
  if (g2ShowListPage(gRowPtrs, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_SETTINGS);
    DEBUG_G2F("[G2] Settings menu shown (modules, view=%s, rows=%u)",
              gView == SET_VIEW_PRETTY ? "PRETTY" : "JSON", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Settings menu show FAILED");
  }
}

void g2SettingsHandleTap(uint32_t idx) {
  switch (gLevel) {

    case SET_LEVEL_MODULES: {
      if (idx == 0) {
        // <- Back: out of Settings, back to root hijack menu.
        // Reset to page 0 so the next entry to Settings starts fresh.
        gModulePage = 0;
        g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
        extern void g2RedrawHijackMainMenu();
        g2RedrawHijackMainMenu();
        return;
      }
      if (idx == 1) {
        // View toggle. Flip and redraw the module list — current view
        // is shown on the toggle row itself, so no other row changes.
        gView = (gView == SET_VIEW_PRETTY) ? SET_VIEW_JSON : SET_VIEW_PRETTY;
        DEBUG_G2F("[G2] Settings: view → %s",
                  gView == SET_VIEW_PRETTY ? "PRETTY" : "JSON");
        size_t n = buildModuleRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      // Pagination: prev/next page rows. Detect via stored row indices
      // so we don't have to re-derive layout here.
      if ((int)idx == gPagePrevRow) {
        if (gModulePage > 0) gModulePage--;
        DEBUG_G2F("[G2] Settings: prev page → %u", (unsigned)gModulePage);
        size_t n = buildModuleRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      if ((int)idx == gPageNextRow) {
        gModulePage++;
        DEBUG_G2F("[G2] Settings: next page → %u", (unsigned)gModulePage);
        size_t n = buildModuleRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      const int modIdx = moduleIdxFromTap(idx);
      if (modIdx < 0) {
        DEBUG_G2F("[G2] Settings: tap idx=%u not on a module row",
                  (unsigned)idx);
        return;
      }
      gLevel        = SET_LEVEL_ENTRIES;
      gActiveModule = modIdx;
      gEntryPage    = 0;   // entering a new module always lands at page 0

      size_t modCount = 0;
      const SettingsModule** mods = getSettingsModules(modCount);
      const char* modName = (mods[modIdx] && mods[modIdx]->name)
                              ? mods[modIdx]->name : "?";

      bool ok = false;
      if (gView == SET_VIEW_JSON) {
        // TEXT widget render — no list chrome, content flows.
        ok = showModuleJsonViaTextWidget(gActiveModule);
        if (ok) {
          DEBUG_G2F("[G2] Settings: drilled into '%s' (JSON view, TEXT widget)",
                    modName);
        }
      } else {
        // PRETTY: key=value rows in a list widget.
        size_t n = buildEntryRowsPretty(gActiveModule);
        ok = g2ShowListPage(gRowPtrs, n);
        if (ok) {
          DEBUG_G2F("[G2] Settings: drilled into '%s' (PRETTY view, rows=%u)",
                    modName, (unsigned)n);
        }
      }

      if (!ok) {
        DEBUG_G2F("[G2] Settings: drill into '%s' FAILED — reverting to module list",
                  modName);
        gLevel        = SET_LEVEL_MODULES;
        gActiveModule = -1;
      }
      return;
    }

    case SET_LEVEL_ENTRIES: {
      if (idx == 0) {
        // <- Back: up one level to the module list.
        gLevel        = SET_LEVEL_MODULES;
        gActiveModule = -1;
        gEntryPage    = 0;
        size_t n = buildModuleRows();
        if (g2ShowListPage(gRowPtrs, n)) {
          DEBUG_G2F("[G2] Settings: back to module list (rows=%u)",
                    (unsigned)n);
        }
        return;
      }
      // Pagination — same row-index detection as Level 1. JSON view
      // doesn't paginate (it uses a TEXT widget with chunked rows
      // built by showModuleJsonViaTextWidget), so prev/next only fire
      // in PRETTY view where buildEntryRowsPretty set these.
      if ((int)idx == gPagePrevRow) {
        if (gEntryPage > 0) gEntryPage--;
        DEBUG_G2F("[G2] Settings: entries prev page → %u", (unsigned)gEntryPage);
        size_t n = buildEntryRowsPretty(gActiveModule);
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      if ((int)idx == gPageNextRow) {
        gEntryPage++;
        DEBUG_G2F("[G2] Settings: entries next page → %u", (unsigned)gEntryPage);
        size_t n = buildEntryRowsPretty(gActiveModule);
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      // Tapping a setting row is a no-op (read-only). Log for visibility.
      DEBUG_G2F("[G2] Settings: tapped entry idx=%u in module %d (read-only)",
                (unsigned)idx, gActiveModule);
      return;
    }
  }
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
