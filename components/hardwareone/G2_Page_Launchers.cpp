// =============================================================================
// G2 glasses: the Apps, System and Config launcher menus
// =============================================================================
// Part of the glasses client (G2_Glasses.cpp). The shared state and the core
// functions this file uses are declared in G2_GlassesInternal.h.

#include "G2_Glasses.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "BLE_Server.h"
#include "G2_HijackCmd.h"

#include "G2_Page_Settings.h"
#include "G2_Page_Transcription.h"
#include "G2_Page_Notes.h"
#include "G2_Page_Files.h"
#include "G2_Page_TestSuite.h"
#include "G2_Page_Users.h"
#include "G2_Page_ESPNow.h"
#include "G2_Page_Automations.h"
#if ENABLE_MAPS
#include "System_Maps.h"
#endif
#include "G2_GlassesInternal.h"

// ─────────────────────────────────────────────────────────────────────
// Apps launcher — a top-level submenu that groups the "app" pages
// (ESP-NOW App, Files, Automations, Ring) plus the Maps viewer and LLM
// reader under one entry, so the main hijack menu stays short. Each row
// forwards to another page's show*Menu() (which flips gHijackPage itself)
// or launches the map/ring viewer. Stateless — no sub-mode tracking
// needed. The sub-pages stay registered (with hijackLabel=nullptr) so tap
// routing to their handleTap still works; they're just no longer
// top-level rows.
// ─────────────────────────────────────────────────────────────────────

// Category launchers (they mirror the OLED's System and Config categories).
// g2ShowAppsMenu, g2ShowSystemMenu and g2ShowConfigMenu are declared in
// G2_Glasses.h: the sub-pages in their own files (Settings, Tests, Users)
// return to them on Back. The System launcher opens the Status page through
// invokePageFromMain, which lives with the tap dispatcher in G2_Glasses.cpp.

// Apps launcher rows. A single builder emits {label, id} pairs under the
// compile-flag guards so g2ShowAppsMenu (renders labels) and g2AppsHandleTap
// (dispatches on id) stay in lockstep — every row keeps a stable id
// regardless of which optional apps (Maps, LLM, Automations, Health) are
// compiled in. g2BuildAppsInfo reuses it too so the CLI text view can't drift.
enum G2AppRow : uint8_t {
  APP_ROW_BACK = 0,
  APP_ROW_ESPNOW,
  APP_ROW_FILES,
  APP_ROW_MAPS,
  APP_ROW_LLM,
  APP_ROW_AUTOMATIONS,
  APP_ROW_HEALTH,
  APP_ROW_TRANSCRIPTION,
  APP_ROW_NOTES,
  // System Events + Logging moved to the System launcher; Users moved to the
  // Config launcher (menu reorg — mirrors the OLED categories).
};

// Max rows = Back + ESPNow + Files + Maps + LLM + Automations + Health +
// Transcription + Notes.
#define G2_APPS_MAX_ROWS 9

static size_t g2AppsBuildRows(const char** labels, G2AppRow* ids, size_t cap) {
  size_t n = 0;
  auto add = [&](const char* label, G2AppRow id) {
    if (n < cap) { labels[n] = label; ids[n] = id; n++; }
  };
  add("<- Main Menu", APP_ROW_BACK);
  // Row label is just "ESP-NOW" — the "App" suffix was redundant next to the
  // other rows (Files / Maps / LLM / Automations / Health), which don't carry it.
  // The page itself is still the "ESP-NOW App" module internally (kEspNowAppPage,
  // name="espnowapp"), distinct from Network -> ESP-NOW, which owns settings.
  add("ESP-NOW",      APP_ROW_ESPNOW);
  add("Files",        APP_ROW_FILES);
#if ENABLE_DICTATION
  add("Transcription", APP_ROW_TRANSCRIPTION);
#endif
#if ENABLE_QUICKNOTE
  add("Notes",        APP_ROW_NOTES);
#endif
#if ENABLE_MAPS
  // Reflect map availability in the label so tapping "Maps" with no tiles
  // on the device isn't a silent no-op (getAvailableMaps scans /maps for
  // <name>/<name>.hwmap subdirs — the same check the worker does).
  char probe[1][96];
  const bool haveMap = MapCore::hasValidMap() || (MapCore::getAvailableMaps(probe, 1) > 0);
  add(haveMap ? "Maps" : "Maps (none)", APP_ROW_MAPS);
#endif
#if ENABLE_LLM_BACKEND
  add("LLM", APP_ROW_LLM);
#endif
#if ENABLE_AUTOMATION
  add("Automations", APP_ROW_AUTOMATIONS);
#endif
#if ENABLE_R1_HEALTH
  add("Health",      APP_ROW_HEALTH);
#endif
  // Menu reorg: System Events + Logging now live under the System launcher,
  // and Users under the Config launcher (see g2SystemBuildRows /
  // g2ConfigBuildRows). Apps keeps only the app-like tools, mirroring the
  // OLED's "Apps" category.
  return n;
}

static void g2BuildAppsInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  const char* labels[G2_APPS_MAX_ROWS];
  G2AppRow    ids[G2_APPS_MAX_ROWS];
  const size_t n = g2AppsBuildRows(labels, ids, G2_APPS_MAX_ROWS);
  size_t off = 0;
  off += snprintf(out + off, cap - off, "Apps");
  for (size_t i = 1; i < n && off < cap; i++) {   // skip the Back row
    off += snprintf(out + off, cap - off, "\n%s", labels[i]);
  }
}

// Non-static: Apps sub-pages that live in their own TUs (e.g.
// G2_Page_Automations.cpp) call this to return to the launcher on Back.
void g2ShowAppsMenu() {
  const char* labels[G2_APPS_MAX_ROWS];
  G2AppRow    ids[G2_APPS_MAX_ROWS];
  const size_t n = g2AppsBuildRows(labels, ids, G2_APPS_MAX_ROWS);
  if (g2ShowListPage(labels, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_APPS);
    DEBUG_G2F("[G2] Apps menu shown (%u items)", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Apps menu show FAILED");
  }
}

static void g2AppsHandleTap(uint32_t idx) {
  const char* labels[G2_APPS_MAX_ROWS];
  G2AppRow    ids[G2_APPS_MAX_ROWS];
  const size_t n = g2AppsBuildRows(labels, ids, G2_APPS_MAX_ROWS);
  if (idx >= n) return;
  switch (ids[idx]) {
    case APP_ROW_BACK: {  // <- back to the root hijack menu
      g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
      g2RedrawHijackMainMenu();
      return;
    }
    case APP_ROW_ESPNOW: g2ShowEspNowAppMenu(); return;  // callee sets gHijackPage
    case APP_ROW_FILES:  g2ShowFilesMenu();     return;  // callee sets gHijackPage
#if ENABLE_DICTATION
    case APP_ROW_TRANSCRIPTION: g2ShowTranscriptionMenu(); return;
#endif
#if ENABLE_QUICKNOTE
    case APP_ROW_NOTES:  g2ShowNotesMenu();     return;  // callee sets gHijackPage
#endif
#if ENABLE_MAPS
    case APP_ROW_MAPS:   g2ShowMapPage(&g2ShowAppsMenu); return;   // Back → Apps
#endif
#if ENABLE_LLM_BACKEND
    case APP_ROW_LLM:    g2ShowLlmMenu(); return;   // opens the LLM submenu (chat / guided / re-run / model)
#endif
#if ENABLE_AUTOMATION
    case APP_ROW_AUTOMATIONS: g2ShowAutomationsMenu(); return;     // Back → Apps (own TU)
#endif
#if ENABLE_R1_HEALTH
    case APP_ROW_HEALTH:      g2ShowHealthPage(&g2ShowAppsMenu); return; // Back → Apps
#endif
    // System Events / Logging / Users moved to the System & Config launchers
    // (menu reorg) — no longer dispatched from Apps.
    default: return;
  }
}

const G2PageModule kAppsPage = {
  "apps", "Apps",
  // Deliberately NOT an app list: the rows are compile-flag dependent (Maps,
  // LLM) so any hardcoded enumeration here goes stale — which is exactly what
  // happened to the old "(ESP-NOW App, Files, Maps)" text. g2BuildAppsInfo
  // derives the real, build-accurate list from g2AppsBuildRows(), and that's
  // what `g2apps` actually prints.
  "Show the Apps launcher on the lens",
  g2BuildAppsInfo,
  g2ShowAppsMenu,
  g2AppsHandleTap,
  G2_HIJACK_PAGE_APPS,
  /*liveIntervalMs=*/ 0,
  /*backLabel=*/    "<- Main Menu",
  /*prefersTextWidget=*/ false,   // own showMenu — flag is irrelevant
  /*liveRender=*/    nullptr,
};

// ─────────────────────────────────────────────────────────────────────
// System launcher (menu reorg) — mirrors the OLED's "System" category.
// Groups Status + System Events + Logging + Tests under one top-level row.
// Same shape as the Apps launcher: a {label,id} builder keeps the renderer
// and the tap dispatch in lockstep. Rows forward to existing entry points;
// Status has no show*Menu of its own so it reuses invokePageFromMain (the
// exact path a MAIN tap would take), which starts its live-text page.
// ─────────────────────────────────────────────────────────────────────
enum G2SystemRow : uint8_t {
  SYS_ROW_BACK = 0,
  SYS_ROW_STATUS,
  SYS_ROW_SYSEVENTS,
  SYS_ROW_LOGGING,
  SYS_ROW_TESTS,
};
#define G2_SYSTEM_MAX_ROWS 5

static size_t g2SystemBuildRows(const char** labels, G2SystemRow* ids, size_t cap) {
  size_t n = 0;
  auto add = [&](const char* label, G2SystemRow id) {
    if (n < cap) { labels[n] = label; ids[n] = id; n++; }
  };
  add("<- Main Menu",  SYS_ROW_BACK);
  add("Status",        SYS_ROW_STATUS);
  add("System Events", SYS_ROW_SYSEVENTS);
  add("Logging",       SYS_ROW_LOGGING);
#if ENABLE_G2_TESTSUITE
  add("Tests",         SYS_ROW_TESTS);
#endif
  return n;
}

static void g2BuildSystemInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
#if ENABLE_G2_TESTSUITE
  snprintf(out, cap, "System\nStatus\nSystem Events\nLogging\nTests");
#else
  snprintf(out, cap, "System\nStatus\nSystem Events\nLogging");
#endif
}

// Non-static: sub-pages in their own TUs (Tests) return here on Back.
void g2ShowSystemMenu() {
  const char* labels[G2_SYSTEM_MAX_ROWS];
  G2SystemRow ids[G2_SYSTEM_MAX_ROWS];
  const size_t n = g2SystemBuildRows(labels, ids, G2_SYSTEM_MAX_ROWS);
  if (g2ShowListPage(labels, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_SYSTEM);
    DEBUG_G2F("[G2] System menu shown (%u items)", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] System menu show FAILED");
  }
}

static void g2SystemHandleTap(uint32_t idx) {
  const char* labels[G2_SYSTEM_MAX_ROWS];
  G2SystemRow ids[G2_SYSTEM_MAX_ROWS];
  const size_t n = g2SystemBuildRows(labels, ids, G2_SYSTEM_MAX_ROWS);
  if (idx >= n) return;
  switch (ids[idx]) {
    case SYS_ROW_BACK: {  // <- back to the root hijack menu
      g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
      g2RedrawHijackMainMenu();
      return;
    }
    // Status opens via the same path a MAIN tap would take (live-text page,
    // own hijackPage G2_HIJACK_PAGE_STATUS). Its back row routes back to
    // System via g2StatusHandleTap; double-click exits to MAIN.
    case SYS_ROW_STATUS:    invokePageFromMain(kStatusPage); return;
    case SYS_ROW_SYSEVENTS: g2ShowSysEventsPage();           return;  // Back → System
    case SYS_ROW_LOGGING:   g2ShowLoggingMenu();             return;  // Back → System
#if ENABLE_G2_TESTSUITE
    case SYS_ROW_TESTS:     g2ShowTestSuiteMenu();           return;  // Back → System (own TU)
#endif
    default: return;
  }
}

// Status' back-row tap (idx 0 = "<- System"). Declared up by kStatusPage.
void g2StatusHandleTap(uint32_t idx) {
  if (idx == 0) { g2ShowSystemMenu(); return; }
}

const G2PageModule kSystemPage = {
  "system", "System",
#if ENABLE_G2_TESTSUITE
  "Show the System launcher (Status / System Events / Logging / Tests) on the lens",
#else
  "Show the System launcher (Status / System Events / Logging) on the lens",
#endif
  g2BuildSystemInfo,
  g2ShowSystemMenu,
  g2SystemHandleTap,
  G2_HIJACK_PAGE_SYSTEM,
  /*liveIntervalMs=*/ 0,
  /*backLabel=*/    "<- Main Menu",
  /*prefersTextWidget=*/ false,   // own showMenu — flag is irrelevant
  /*liveRender=*/    nullptr,
};

// ─────────────────────────────────────────────────────────────────────
// Config launcher (menu reorg) — mirrors the OLED's "Config" category.
// Groups Settings + Users (admin). Users is admin-gated in the builder (the
// command layer is the backstop).
// ─────────────────────────────────────────────────────────────────────
enum G2ConfigRow : uint8_t {
  CFG_ROW_BACK = 0,
  CFG_ROW_SETTINGS,
  CFG_ROW_USERS,
};
#define G2_CONFIG_MAX_ROWS 3

static size_t g2ConfigBuildRows(const char** labels, G2ConfigRow* ids, size_t cap) {
  size_t n = 0;
  auto add = [&](const char* label, G2ConfigRow id) {
    if (n < cap) { labels[n] = label; ids[n] = id; n++; }
  };
  add("<- Main Menu", CFG_ROW_BACK);
  add("Settings",     CFG_ROW_SETTINGS);
  // Admin-only, same gate as the old Apps → Users row.
  if (isAdminUser(g2HijackAuthContext().user)) add("Users", CFG_ROW_USERS);
  return n;
}

static void g2BuildConfigInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  const char* labels[G2_CONFIG_MAX_ROWS];
  G2ConfigRow ids[G2_CONFIG_MAX_ROWS];
  const size_t n = g2ConfigBuildRows(labels, ids, G2_CONFIG_MAX_ROWS);
  size_t off = 0;
  off += snprintf(out + off, cap - off, "Config");
  for (size_t i = 1; i < n && off < cap; i++) {   // skip the Back row
    off += snprintf(out + off, cap - off, "\n%s", labels[i]);
  }
}

// Non-static: sub-pages in their own TUs (Settings, Users) return here on Back.
void g2ShowConfigMenu() {
  const char* labels[G2_CONFIG_MAX_ROWS];
  G2ConfigRow ids[G2_CONFIG_MAX_ROWS];
  const size_t n = g2ConfigBuildRows(labels, ids, G2_CONFIG_MAX_ROWS);
  if (g2ShowListPage(labels, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_CONFIG);
    DEBUG_G2F("[G2] Config menu shown (%u items)", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Config menu show FAILED");
  }
}

static void g2ConfigHandleTap(uint32_t idx) {
  const char* labels[G2_CONFIG_MAX_ROWS];
  G2ConfigRow ids[G2_CONFIG_MAX_ROWS];
  const size_t n = g2ConfigBuildRows(labels, ids, G2_CONFIG_MAX_ROWS);
  if (idx >= n) return;
  switch (ids[idx]) {
    case CFG_ROW_BACK: {  // <- back to the root hijack menu
      g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
      g2RedrawHijackMainMenu();
      return;
    }
    case CFG_ROW_SETTINGS: g2ShowSettingsMenu(); return;   // Back → Config
    case CFG_ROW_USERS:    g2ShowUsersMenu();    return;   // Back → Config (own TU)
    default: return;
  }
}

const G2PageModule kConfigPage = {
  "config", "Config",
  "Show the Config launcher (Settings / Users) on the lens",
  g2BuildConfigInfo,
  g2ShowConfigMenu,
  g2ConfigHandleTap,
  G2_HIJACK_PAGE_CONFIG,
  /*liveIntervalMs=*/ 0,
  /*backLabel=*/    "<- Main Menu",
  /*prefersTextWidget=*/ false,   // own showMenu — flag is irrelevant
  /*liveRender=*/    nullptr,
};

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
