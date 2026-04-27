#ifndef G2_PAGE_FILES_H
#define G2_PAGE_FILES_H

// =============================================================================
// G2 glasses — "Files" page
// =============================================================================
// Directory navigator that mirrors the OLED FileBrowser model: walk the SD /
// LittleFS tree, tap folders to descend, tap files to see metadata. Content
// preview is intentionally omitted — same constraint as the OLED port (no
// viewer for arbitrary file contents).
//
// List layout per page:
//   [0] "<- Back"        — leave Files, return to main hijack menu
//   [1] ".."             — parent dir (skipped at root)
//   [2..N] entries       — folders rendered with "/" prefix, files with size suffix
//
// Tap behavior:
//   • On a folder → navigate into it, REBUILD list with new contents
//   • On a file   → show name/size/type as a brief text overlay (~2 s) then
//                   redraw the list. We don't try to open the file.
//
// Uses the existing FileManager class so we get the same caching / permission
// behavior the OLED browser has.

#include "System_BuildConfig.h"
#include <Arduino.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// Text-mode summary (CLI g2files command). Lists current path + first few
// entries; useful when you want a quick dump without engaging the full
// hijack list.
void g2BuildFilesInfo(char* out, size_t cap);
bool g2ShowFilesPage();

// Show the interactive list view. Sets the page-mode tracker to FILES.
void g2ShowFilesMenu();

// Tap dispatch from handleHijackMenuTap when gHijackPage == FILES.
void g2FilesHandleTap(uint32_t idx);

// Called by the hijack tick (once per ~50 ms) so the file-info overlay can
// time itself out and redraw the list. Cheap when no overlay is active.
void g2FilesTick();

#else
inline void g2BuildFilesInfo(char* out, size_t cap) {
  if (out && cap > 0) out[0] = '\0';
}
inline bool g2ShowFilesPage() { return false; }
inline void g2ShowFilesMenu() {}
inline void g2FilesHandleTap(uint32_t idx) { (void)idx; }
inline void g2FilesTick() {}
#endif

#endif  // G2_PAGE_FILES_H
