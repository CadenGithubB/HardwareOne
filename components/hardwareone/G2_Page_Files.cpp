// =============================================================================
// G2 glasses — "Files" page implementation
// =============================================================================
// Wraps a private FileManager instance. We deliberately don't share
// gOLEDFileManager — the OLED browser uses it from the OLED render thread, so
// reusing it here would mean either coordinating selection state across two
// UIs or fighting over the cursor. A separate instance is cheap (~couple KB)
// and decouples the two paths cleanly.
//
// File-metadata overlay: when the user taps a file, we replace the list
// briefly with a multi-line text page (name, size, type). After
// kFileInfoOverlayMs the tick handler swaps back to the list. We can't get a
// "list still showing" signal from the lens, so we fudge it with a millis()
// timer that ticks from g2FilesTick().

#include "G2_Page_Files.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "Optional_EvenG2.h"
#include "System_FileManager.h"
#include "System_Debug.h"

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

static FileManager* gFilesFm = nullptr;

// Row buffer + pointer table for list rendering. Sized to fit a handful of
// "header" rows (back, ..) plus FILE_MANAGER_MAX_CACHED_ITEMS data rows.
#define FILES_MAX_ROWS  (FILE_MANAGER_MAX_CACHED_ITEMS + 4)
#define FILES_ROW_LEN   48

static char        gFilesRows[FILES_MAX_ROWS][FILES_ROW_LEN];
static const char* gFilesRowPtrs[FILES_MAX_ROWS];
static size_t      gFilesRowCount = 0;

// Indices we keep so taps know what idx → which directory entry. -1 = not a
// real entry (back / parent / blank).
static int  gFilesEntryForRow[FILES_MAX_ROWS];

// File-info overlay duration. The deadline lives in the centralized
// g2LensState (see Optional_EvenG2.h G2LensState). When the deadline
// expires, the overlay-expired callback below fires and we redraw the
// file list.
static const uint32_t kFileInfoOverlayMs = 2200;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static void filesOverlayExpired(G2OverlayKind kind);  // forward decl

static FileManager* ensureFm() {
  if (!gFilesFm) {
    gFilesFm = new FileManager();
    if (gFilesFm) gFilesFm->navigate("/");
    // First-time init: register our overlay-expired callback so the
    // lens-state tick auto-redraws our list when the file-info flash
    // ends. Idempotent — the lens module only stores one pointer; if
    // multiple modules want overlay events later, the lens module will
    // need a multiplexer (see Optional_EvenG2.h G2OverlayKind for how
    // we'd dispatch by kind).
    g2LensSetOverlayExpiredCb(filesOverlayExpired);
  }
  return gFilesFm;
}

// Truncate a name to fit within a row, leaving room for prefix + suffix.
static void truncateInto(char* dst, size_t cap, const char* src,
                         size_t maxLen) {
  if (cap == 0) return;
  size_t srcLen = strlen(src);
  if (srcLen <= maxLen) {
    snprintf(dst, cap, "%s", src);
  } else {
    // Reserve 1 char for "~" tail marker.
    if (maxLen < 2) maxLen = 2;
    size_t take = maxLen - 1;
    if (take >= cap) take = cap - 1;
    memcpy(dst, src, take);
    dst[take] = '~';
    dst[take + 1] = '\0';
  }
}

// Build the row list for the current directory.
static size_t buildRows() {
  for (size_t i = 0; i < FILES_MAX_ROWS; i++) gFilesEntryForRow[i] = -1;

  FileManager* fm = ensureFm();
  if (!fm) {
    strncpy(gFilesRows[0], "<- Back", FILES_ROW_LEN);
    gFilesRowPtrs[0] = gFilesRows[0];
    strncpy(gFilesRows[1], "(file mgr init failed)", FILES_ROW_LEN);
    gFilesRowPtrs[1] = gFilesRows[1];
    gFilesRowCount = 2;
    return 2;
  }

  const char* path = fm->getCurrentPath();
  bool atRoot = (strcmp(path, "/") == 0);

  size_t row = 0;

  // Row 0: back to main menu.
  strncpy(gFilesRows[row], "<- Back", FILES_ROW_LEN);
  gFilesRows[row][FILES_ROW_LEN - 1] = '\0';
  gFilesRowPtrs[row] = gFilesRows[row];
  row++;

  // Row 1: path indicator (informational, no-op tap).
  {
    char shortPath[FILES_ROW_LEN];
    truncateInto(shortPath, sizeof(shortPath), path, FILES_ROW_LEN - 6);
    snprintf(gFilesRows[row], FILES_ROW_LEN, "@ %s", shortPath);
    gFilesRowPtrs[row] = gFilesRows[row];
    row++;
  }

  // Row 2: parent dir (skip when at root).
  size_t parentRow = SIZE_MAX;
  if (!atRoot) {
    snprintf(gFilesRows[row], FILES_ROW_LEN, ".. (up)");
    gFilesRowPtrs[row] = gFilesRows[row];
    parentRow = row;
    row++;
  }

  // Then directory entries.
  int total = fm->getItemCount();
  for (int i = 0; i < total && row < FILES_MAX_ROWS; i++) {
    FileEntry entry;
    if (!fm->getItem(i, entry)) continue;
    if (entry.isFolder) {
      char nm[FILES_ROW_LEN];
      truncateInto(nm, sizeof(nm), entry.name, FILES_ROW_LEN - 4);
      snprintf(gFilesRows[row], FILES_ROW_LEN, "/ %s", nm);
    } else {
      // Compact size: B / K / M
      uint32_t sz = entry.size;
      char sizeStr[12];
      if (sz < 1024)              snprintf(sizeStr, sizeof(sizeStr), "%uB",  (unsigned)sz);
      else if (sz < 1024 * 1024)  snprintf(sizeStr, sizeof(sizeStr), "%uK",  (unsigned)(sz / 1024));
      else                        snprintf(sizeStr, sizeof(sizeStr), "%uM",  (unsigned)(sz / (1024 * 1024)));
      // 6-char tail reserved for size column.
      char nm[FILES_ROW_LEN];
      truncateInto(nm, sizeof(nm), entry.name, FILES_ROW_LEN - 10);
      snprintf(gFilesRows[row], FILES_ROW_LEN, "%s  %s", nm, sizeStr);
    }
    gFilesRowPtrs[row] = gFilesRows[row];
    gFilesEntryForRow[row] = i;
    row++;
  }

  if (row == (atRoot ? 2u : 3u)) {
    // No entries — show empty marker.
    snprintf(gFilesRows[row], FILES_ROW_LEN, "(empty)");
    gFilesRowPtrs[row] = gFilesRows[row];
    row++;
  }

  // Mark parent row separately so dispatch knows.
  if (parentRow != SIZE_MAX) {
    gFilesEntryForRow[parentRow] = -2;  // sentinel: parent
  }

  gFilesRowCount = row;
  return row;
}

// -----------------------------------------------------------------------------
// File metadata overlay
// -----------------------------------------------------------------------------

static void showFileInfo(const FileEntry& e) {
  // Render the metadata as a list page (one item per field) so we stay in
  // a list-widget container — the firmware bails out if we REBUILD a text
  // widget into a container CREATEd as a list. Page stays as FILES so
  // the existing Files tap dispatcher handles "back" correctly (returns to
  // the file list, not the main hijack menu).
  const char* ext = strrchr(e.name, '.');
  if (!ext || !*ext) ext = "(no ext)";
  uint32_t sz = e.size;
  char sizeStr[24];
  if      (sz < 1024)             snprintf(sizeStr, sizeof(sizeStr), "%u bytes",  (unsigned)sz);
  else if (sz < 1024UL * 1024UL)  snprintf(sizeStr, sizeof(sizeStr), "%.1f KB",   sz / 1024.0);
  else                            snprintf(sizeStr, sizeof(sizeStr), "%.2f MB",   sz / (1024.0 * 1024.0));

  static char nameRow[FILES_ROW_LEN];
  static char sizeRow[FILES_ROW_LEN];
  static char typeRow[FILES_ROW_LEN];
  static const char* rows[5];
  rows[0] = "<- Back";
  snprintf(nameRow, sizeof(nameRow), "name %s", e.name);
  snprintf(sizeRow, sizeof(sizeRow), "size %s", sizeStr);
  snprintf(typeRow, sizeof(typeRow), "type %s", ext);
  rows[1] = nameRow;
  rows[2] = sizeRow;
  rows[3] = typeRow;
  rows[4] = "(closing...)";
  g2ShowListPage(rows, 5);
  // Deliberately do NOT change the hijack page — leave it as FILES so a
  // tap on idx=0 routes through g2FilesHandleTap and goes back to the
  // file list. The lens overlay tracker handles auto-dismiss; the
  // expired callback (filesOverlayExpired) redraws the file list.
  g2LensStartOverlay(G2_OVERLAY_FILE_INFO, kFileInfoOverlayMs);
  DEBUG_G2F("[G2] Files: file-info overlay for '%s' (%u B, %s)",
            e.name, (unsigned)sz, ext);
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void g2BuildFilesInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';

  String s;
  s.reserve(384);
  s += "Files\n";

  FileManager* fm = ensureFm();
  if (!fm) {
    s += "fm init failed";
    strncpy(out, s.c_str(), cap - 1);
    out[cap - 1] = '\0';
    return;
  }

  char line[80];
  snprintf(line, sizeof(line), "@ %s\n", fm->getCurrentPath());
  s += line;
  int total = fm->getItemCount();
  snprintf(line, sizeof(line), "%d items\n", total);
  s += line;

  // Sample first few entries.
  int show = total < 5 ? total : 5;
  for (int i = 0; i < show; i++) {
    FileEntry e;
    if (!fm->getItem(i, e)) continue;
    snprintf(line, sizeof(line), "%s%s\n",
             e.isFolder ? "/" : " ", e.name);
    s += line;
    if (s.length() > cap - 64) break;
  }

  strncpy(out, s.c_str(), cap - 1);
  out[cap - 1] = '\0';
}

bool g2ShowFilesPage() {
  char buf[400];
  g2BuildFilesInfo(buf, sizeof(buf));
  DEBUG_G2F("[G2] Files page (%u B):\n%s", (unsigned)strlen(buf), buf);
  return g2ShowText(buf);
}

void g2ShowFilesMenu() {
  size_t n = buildRows();
  if (g2ShowListPage(gFilesRowPtrs, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_FILES);
    DEBUG_G2F("[G2] Files menu shown (rows=%u)", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Files menu show FAILED");
  }
  // Cancel any in-flight overlay since we explicitly reshowed the list.
  g2LensClearOverlay();
}

void g2FilesHandleTap(uint32_t idx) {
  if (idx == 0) {
    // Context-sensitive back. If a file-info overlay is showing,
    // "<- Back" returns to the file list rather than the main hijack
    // menu — matches what the user just navigated away from.
    // g2ShowFilesMenu clears the overlay state.
    if (g2LensInOverlay()) {
      DEBUG_G2F("[G2] Files: back from overlay → file list");
      g2ShowFilesMenu();
      return;
    }
    g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
    extern void g2RedrawHijackMainMenu();
    g2RedrawHijackMainMenu();
    return;
  }

  // While an overlay is showing, idx>0 are read-only metadata rows. No-op.
  if (g2LensInOverlay()) {
    DEBUG_G2F("[G2] Files: tap on overlay row %u (read-only)",
              (unsigned)idx);
    return;
  }

  if (idx >= FILES_MAX_ROWS || idx >= gFilesRowCount) {
    DEBUG_G2F("[G2] Files: tap idx=%u out of range (rows=%u)",
              (unsigned)idx, (unsigned)gFilesRowCount);
    return;
  }

  int entryIdx = gFilesEntryForRow[idx];
  FileManager* fm = ensureFm();
  if (!fm) return;

  if (entryIdx == -2) {
    // Parent dir.
    DEBUG_G2F("[G2] Files: navigateUp from '%s'", fm->getCurrentPath());
    fm->navigateUp();
    g2ShowFilesMenu();
    return;
  }

  if (entryIdx < 0) {
    // Path indicator / blank — no-op.
    return;
  }

  FileEntry e;
  if (!fm->getItem(entryIdx, e)) {
    DEBUG_G2F("[G2] Files: getItem(%d) failed", entryIdx);
    return;
  }

  if (e.isFolder) {
    // Navigate into it. FileManager's navigateInto uses the *selected* index,
    // so set selection first.
    while (fm->getSelectedIndex() < entryIdx) fm->moveDown();
    while (fm->getSelectedIndex() > entryIdx) fm->moveUp();
    DEBUG_G2F("[G2] Files: navigateInto '%s'", e.name);
    fm->navigateInto();
    g2ShowFilesMenu();
  } else {
    // File tap → metadata overlay.
    showFileInfo(e);
  }
}

// Overlay-expired callback. Registered with the lens-state tracker via
// g2LensSetOverlayExpiredCb at first ensureFm() call. Fires when the
// shared overlay clock crosses our deadline. We only redraw if both
// (a) the expired overlay was ours, and (b) we're still on the Files
// page (user might have swiped away).
static void filesOverlayExpired(G2OverlayKind kind) {
  if (kind != G2_OVERLAY_FILE_INFO) return;
  if (g2GetHijackPage() != G2_HIJACK_PAGE_FILES) return;
  g2ShowFilesMenu();
}

void g2FilesTick() {
  // No longer needed — the centralized lens overlay tick handles auto-
  // dismiss. Kept as a no-op so existing call sites compile; safe to
  // remove once Optional_EvenG2.cpp's g2Tick stops calling it.
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
