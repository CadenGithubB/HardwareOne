#ifndef G2_PAGE_SETTINGS_H
#define G2_PAGE_SETTINGS_H

// =============================================================================
// G2 glasses — "Settings" page
// =============================================================================
// Two view modes, toggled by tapping the "View:" list item:
//   • INTERACTIVE — drill module -> (group ->) entries; tap an entry to edit it
//                   (boolean flip / enum pick-list / character keyboard),
//                   committing through the real per-setting CLI command — the
//                   same mechanism the OLED settings editor uses.  (default)
//   • JSON        — serialized settings JSON, line-wrapped for the lens
//                   (read-only)
//
// Both modes share the same registry source (registered SettingsModule list),
// so additions to the settings registry show up automatically here. Each list
// page is capped to a single BLE fragment (the lens scrolls natively).

#include "System_BuildConfig.h"
#include <Arduino.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// Build a text-mode summary. Used by the CLI `g2settings` command for direct
// invocation (no list-widget container required). Format is the same as the
// PRETTY view but newline-separated.
void g2BuildSettingsInfo(char* out, size_t cap);
bool g2ShowSettingsPage();

// Render the list-mode page (with view toggle). Called when the user taps
// "Settings" from the main hijack menu. Sets the page-mode tracker so taps
// route here.
void g2ShowSettingsMenu();

// Tap dispatch from handleHijackMenuTap when gHijackPage == SETTINGS. Drives
// the module / group / entry / pick-list navigation and the edit actions; see
// the per-level state machine in G2_Page_Settings.cpp.
void g2SettingsHandleTap(uint32_t idx);

#else
inline void g2BuildSettingsInfo(char* out, size_t cap) {
  if (out && cap > 0) out[0] = '\0';
}
inline bool g2ShowSettingsPage() { return false; }
inline void g2ShowSettingsMenu() {}
inline void g2SettingsHandleTap(uint32_t idx) { (void)idx; }
#endif

#endif  // G2_PAGE_SETTINGS_H
