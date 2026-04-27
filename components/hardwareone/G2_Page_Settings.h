#ifndef G2_PAGE_SETTINGS_H
#define G2_PAGE_SETTINGS_H

// =============================================================================
// G2 glasses — "Settings" page
// =============================================================================
// Read-only settings inspector. The OLED Settings mode is interactive (cycle
// through entries with the dial, edit values), but the G2 has no keyboard or
// dial — so the port is "look but don't touch".
//
// Two view modes, toggled by tapping the first list item:
//   • PRETTY — one row per setting: "module.key=value"  (default)
//   • JSON   — serialized full settings JSON, line-wrapped for the lens
//
// Both modes share the same registry source (registered SettingsModule list),
// so additions to the settings registry show up automatically here. We cap
// the total row count to avoid blowing up the list widget — the lens scrolls
// natively so a long list is fine, but unbounded growth would fragment the
// row buffer.

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

// Tap dispatch from handleHijackMenuTap when gHijackPage == SETTINGS.
//   idx == 0 → back to main menu
//   idx == 1 → toggle view mode (PRETTY ↔ JSON), redraw
//   idx >= 2 → no-op (read-only entries)
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
