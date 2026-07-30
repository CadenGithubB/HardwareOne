// =============================================================================
// G2 glasses — "Files" page implementation
// =============================================================================
// Wraps a private FileManager instance. We deliberately don't share
// gOledFileManager — the OLED browser uses it from the OLED render thread, so
// reusing it here would mean either coordinating selection state across two
// UIs or fighting over the cursor. A separate instance is cheap (~couple KB)
// and decouples the two paths cleanly.
//
// File-metadata overlay: when the user taps a file, we replace the list
// briefly with a multi-line text page (name, size, type). After
// kFileInfoOverlayMs the tick handler swaps back to the list. We can't get a
// "list still showing" signal from the lens, so we fudge it with a millis()
// timer that ticks from g2FilesTick().
//
// IDENTITY / CACHE INVALIDATION
// -----------------------------
// FileManager caches directory entries, and those entries depend on the
// calling task's auth identity (VFS::openGuarded permission checks per
// entry). The cache is invalidated via the identity-generation protocol
// — see System_AuthIdentity.h for the full doc block. The short version:
// while already on the Files page, g2ShowFilesMenu calls fm->refresh(),
// which is a no-op when the cache's generation matches gIdentityGeneration
// and a re-scan otherwise. Producers (pairing, user add/del/promote/demote)
// bump the generation, so the next refresh() picks up the new picture.
// Re-entering Files from another hijack page (or after a safety timeout)
// uses forceRescan() so sizes / new files written while away are visible
// without requiring an up-then-down folder hop. If you add a new caching
// subsystem that depends on auth, follow the same pattern.

#include "G2_Page_Files.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_Glasses.h"
#include "G2_Page_Common.h"      // TextPager + paging ops (shared text viewer)
#include "G2_HijackCmd.h"        // G2HijackCtxGuard — installs pairedByUser identity
#include "G2_Page_TextEntry.h"   // on-lens keyboard for Rename
#include "System_FileManager.h"
#include "System_Filesystem.h"
#include "System_CaptureCrypto.h"  // reveal sealed captures on the lens viewer
#include "System_MemUtil.h"   // ps_alloc — gFilesFm lives in PSRAM (placement-new)
#include <new>                // placement-new
#include "System_Debug.h"
#include "System_AuthIdentity.h"

#include <ArduinoJson.h>
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

static FileManager* gFilesFm = nullptr;

// Paginated directory view. The lens can NOT render a whole big directory
// as one list: the compound CREATE payload caps at 1 KB (~20 filename rows
// before g2BuildCreateMixedListTextPb runs out of buffer and the page
// silently fails) and the firmware list widget itself tops out near 20
// items. Directories that outgrow one screen (e.g. months of accumulated
// health-*.csv captures in /logging_captures/sensors) page through the
// shared G2Paginator instead — same chrome as Settings / LED / LLM menu.
#define FILES_VISIBLE_ENTRIES 12
// back + ".. (up)" + FILES_VISIBLE_ENTRIES entries + Prev + Next = 16;
// +2 slack for the "(empty)" marker / min-render padding.
#define FILES_MAX_ROWS  (FILES_VISIBLE_ENTRIES + 6)
#define FILES_ROW_LEN   48
// Keep list placement visually stable across directories with very different
// item counts. The firmware tends to vertically center short lists in a tall
// container; padding to a fixed minimum row count keeps the first rows near
// the same top-left position as you browse (/photos vs /sd, etc.).
static constexpr size_t kFilesMinRenderRows = 8;

// Row buffers in PSRAM — ~1 KB total. Filled by buildRows() from the
// page worker, read by the BLE notify task; both run in regular task
// context, no DMA / ISR access.
EXT_RAM_BSS_ATTR static char        gFilesRows[FILES_MAX_ROWS][FILES_ROW_LEN];
EXT_RAM_BSS_ATTR static const char* gFilesRowPtrs[FILES_MAX_ROWS];
// Path title is shown in its own TextObject (not a list row). Needs room for
// multi-line wrap of deep VFS paths; FILES_ROW_LEN was only enough for one
// truncated line and the old 40 px-tall box scrolled uselessly.
#define FILES_PATH_TITLE_LEN  96
EXT_RAM_BSS_ATTR static char        gFilesPathTitleBuf[FILES_PATH_TITLE_LEN];
static size_t      gFilesRowCount = 0;

// Current paginator page for the directory listing. Reset to 0 on any
// directory change (navigate in/up, re-entry from outside Files);
// preserved across in-page redraws (chooser dismiss, overlay expiry) —
// g2PaginatorPrepare clamps it if the directory shrank meanwhile.
static size_t      gFilesPage = 0;

// Mixed list+text layout: keep the path in a TextObject at top-right while
// pinning the list to the top-left quadrant/half (not vertically centered).
// LEFT_HALF is {8,8,280,272}, so row 0 starts near the top edge.
static constexpr G2ContainerGeom kFilesListGeom = G2_GEOM_LEFT_HALF;
// Path readout on the right. Was {280,8,288,40} — one-line height with a
// firmware scrollbar when the path wrapped; focus stays on the list so that
// scrollbar was unusable. Align with RIGHT_HALF x/w and give ~2–3 lines of
// height so wrapped paths stay fully visible.
static constexpr G2ContainerGeom kFilesPathGeom = { 288,   8, 280, 100 };

// Indices we keep so taps know what idx → which directory entry. -1 = not a
// real entry (back / blank); -2 = parent row ".. (up)"; -3 = "<< Prev page";
// -4 = "Next page >>".
EXT_RAM_BSS_ATTR static int  gFilesEntryForRow[FILES_MAX_ROWS];

// File-info overlay duration. The deadline lives in the centralized
// g2LensState (see G2_Glasses.h G2LensState). When the deadline
// expires, the overlay-expired callback below fires and we redraw the
// file list.
static const uint32_t kFileInfoOverlayMs = 2200;

// File-action chooser state. When the user taps a BMP file we show a
// 3-row list (<- Files / View / Info) so they can pick whether to
// render the image on the lens or look at metadata. Non-BMP files
// route directly to the metadata page (no chooser). Active flag
// gates the tap dispatcher; the cached entry lets View/Info reuse
// the entry the user picked without a re-lookup.
static bool      gFilesChooserActive = false;
static FileEntry gFilesChooserEntry  = {};
enum FilesChooserKind : uint8_t {
  FILE_CHOOSER_BMP  = 0,
  FILE_CHOOSER_JSON = 1,
  FILE_CHOOSER_JPG  = 2,  // same chooser shape as BMP, but View/View
                          //  Full dispatch through g2ShowJpgFile path
                          //  (decode JPEG → 4-bpp BMP, then same wire
                          //   transport as the BMP viewers).
  FILE_CHOOSER_TEXT = 3,  // .txt / .csv — one raw paged view + Info (no
                          //  pretty-parse). Shares the JSON viewer's engine
                          //  (readTextLimited → wrap → TextPager).
  FILE_CHOOSER_OTHER = 4, // any other file type — no viewer, but Info /
                          //  Rename / Delete must still be reachable (the old
                          //  direct-to-info-overlay path made mutation
                          //  impossible for most file types).
};
static FilesChooserKind gFilesChooserKind = FILE_CHOOSER_BMP;

// Per-row action table for the chooser. showFileChooser fills rows[] and
// this table in ONE place so render and dispatch can never drift — the old
// fixed-index-per-kind dispatch was already fragile at three kinds and two
// more rows (Rename/Delete) across five kinds would have guaranteed a bug.
enum FilesAction : uint8_t {
  FACT_BACK = 0,    // <- Files
  FACT_HEADER,      // filename header — inert
  FACT_VIEW,        // image native / text raw / json pretty
  FACT_VIEW_FULL,   // image 2x upscale
  FACT_JSON_RAW,    // json raw view
  FACT_INFO,        // metadata overlay
  FACT_RENAME,      // keyboard → filerename
  FACT_DELETE,      // confirm  → filedelete
};
#define FILES_CHOOSER_MAX_ROWS 8
static FilesAction gFilesChooserAct[FILES_CHOOSER_MAX_ROWS];
static uint8_t     gFilesChooserRowN = 0;

// Mutation target snapshot + state. The chooser entry is a single-slot static
// cleared by every list redraw, so the absolute path and basename are copied
// here the moment a Rename/Delete action is tapped — the keyboard/confirm
// flows that follow must not depend on the chooser slot surviving.
EXT_RAM_BSS_ATTR static char gFilesActionPath[FILE_MANAGER_MAX_PATH + 32];
static char gFilesActionName[FILE_MANAGER_MAX_NAME];
static bool gFilesConfirmActive = false;   // delete-confirm list is up
static char gFilesConfirmRow[FILE_MANAGER_MAX_NAME + 12];
EXT_RAM_BSS_ATTR static char gFilesMutateMsg[112];          // result banner text

// Paged text viewer (JSON pretty/raw, .txt, .csv). Shares the TextPager
// engine in G2_Page_Common.h with Settings JSON + ESPNow chat: one flat PSRAM
// body buffer + offset table, hard-wrapped so long CSV rows / JSON values flow
// across lens lines instead of being clipped. tap/scroll advance, double-tap
// exits back to the Files list.
#define FILES_TEXT_MAX_PAGES     24
#define FILES_TEXT_PAGE_BUDGET   176   // < 180 proven-safe single-fragment body
// Displayable ceiling ≈ maxPages × budget. The read cap below is larger so the
// read is whole-file (line-honest), but only this much is ever wrapped/shown.
#define FILES_TEXT_BODY_CAP      (FILES_TEXT_MAX_PAGES * FILES_TEXT_PAGE_BUDGET + 128)
static constexpr size_t kFilesTextReadCapBytes = 12 * 1024;  // bounded heap use

EXT_RAM_BSS_ATTR static char gFilesTextBody[FILES_TEXT_BODY_CAP];        // wrapped body
static uint16_t              gFilesTextPageOff[FILES_TEXT_MAX_PAGES + 1]; // page offsets
EXT_RAM_BSS_ATTR static char gFilesTextPageBuf[FILES_TEXT_PAGE_BUDGET + 128]; // render scratch
static char                  gFilesTextTitle[FILES_ROW_LEN] = {0};
static TextPager gFilesPager = {
    gFilesTextBody, gFilesTextPageOff, FILES_TEXT_MAX_PAGES,
    FILES_TEXT_PAGE_BUDGET, /*pageCount=*/0, /*curPage=*/0, /*truncated=*/false };

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static void filesOverlayExpired(G2OverlayKind kind);  // forward decl
static void exitTextViewBackToFiles();
static void filesTextNav(G2TapKind kind);
static bool filesRenderTextPage();

static FileManager* ensureFm() {
  if (!gFilesFm) {
    // PSRAM placement-new: FileManager's ~19 KB is an inline cachedEntries[256] POD
    // array, so the whole object moves off internal DRAM. This lens FileManager is
    // create-once-keep (no delete site anywhere) → no ps_delete pair; it persists
    // for the session. The `if (gFilesFm)` guards below handle an alloc failure.
    void* fmBuf = ps_alloc(sizeof(FileManager), AllocPref::PreferPSRAM, "g2.filemgr");
    gFilesFm = fmBuf ? new (fmBuf) FileManager() : nullptr;
    // Show a "[#]" item-count badge on folder rows (opt-in so only this lens
    // explorer pays the per-folder count; the OLED browser stays untouched).
    // Set BEFORE the first navigate so the initial load populates counts.
    if (gFilesFm) gFilesFm->setCountFolderChildren(true);
    if (gFilesFm) gFilesFm->navigate("/");
    // First-time init: register our overlay-expired callback so the
    // lens-state tick auto-redraws our list when the file-info flash
    // ends. Idempotent — the lens module only stores one pointer; if
    // multiple modules want overlay events later, the lens module will
    // need a multiplexer (see G2_Glasses.h G2OverlayKind for how
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
    strncpy(gFilesRows[0], "<- Main Menu", FILES_ROW_LEN);
    gFilesRowPtrs[0] = gFilesRows[0];
    strncpy(gFilesRows[1], "(file mgr init failed)", FILES_ROW_LEN);
    gFilesRowPtrs[1] = gFilesRows[1];
    gFilesRowCount = 2;
    return 2;
  }

  const char* path = fm->getCurrentPath();
  bool atRoot = (strcmp(path, "/") == 0);

  size_t row = 0;

  // Row 0: back. At root we exit Files entirely; in a subdir we'd go up
  // one level — but ".. (up)" is a separate row that handles parent
  // navigation, so the "Back" tap consistently exits Files for now.
  strncpy(gFilesRows[row], "<- Main Menu", FILES_ROW_LEN);
  gFilesRows[row][FILES_ROW_LEN - 1] = '\0';
  gFilesRowPtrs[row] = gFilesRows[row];
  row++;

  // Current path is shown via g2ShowMixedListText title (top-right), not here.

  // Parent dir row (skip when at root).
  size_t parentRow = SIZE_MAX;
  if (!atRoot) {
    snprintf(gFilesRows[row], FILES_ROW_LEN, ".. (up)");
    gFilesRowPtrs[row] = gFilesRows[row];
    parentRow = row;
    row++;
  }

  // Then one paginator page's worth of directory entries. getItemCount()
  // is the TRUE directory total (FileManager keeps counting past its
  // cache), so the "(p/total)" chrome is honest; getItem() falls back to
  // a per-index rescan for entries beyond the cache, so even directories
  // larger than the cache stay reachable — just slower per page.
  int total = fm->getItemCount();
  G2Paginator pag = g2PaginatorPrepare((size_t)total, FILES_VISIBLE_ENTRIES,
                                       gFilesPage);
  for (size_t i = pag.startIdx; i < pag.endIdx && row < FILES_MAX_ROWS; i++) {
    FileEntry entry;
    if (!fm->getItem((int)i, entry)) continue;
    if (entry.isFolder) {
      // "[#]" item-count badge (matches the web/app folder counts). Capped at
      // 99 by the FileManager → show "99+" so an empty folder reads "[0]" and
      // a full one never overflows the row.
      char badge[8];
      if (entry.childCount >= 99) snprintf(badge, sizeof(badge), "[99+]");
      else                        snprintf(badge, sizeof(badge), "[%u]", (unsigned)entry.childCount);
      char nm[FILES_ROW_LEN];
      // Reserve "/ " (2) + " " (1) + badge for the name column.
      const int reserve = 3 + (int)strlen(badge);
      truncateInto(nm, sizeof(nm), entry.name, FILES_ROW_LEN - reserve);
      snprintf(gFilesRows[row], FILES_ROW_LEN, "/ %s %s", nm, badge);
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
    gFilesEntryForRow[row] = (int)i;
    row++;
  }

  if (row == (atRoot ? 1u : 2u)) {
    // No entries — show empty marker.
    snprintf(gFilesRows[row], FILES_ROW_LEN, "(empty)");
    gFilesRowPtrs[row] = gFilesRows[row];
    row++;
  }

  // "<< Prev page" / "Next page >> (p/total)" chrome — only rendered when
  // the directory spans multiple pages. Tap routing goes through the
  // gFilesEntryForRow sentinels, not fixed indices.
  row = g2PaginatorWriteChrome(pag, gFilesPage, row, FILES_MAX_ROWS,
                               &gFilesRows[0][0], FILES_ROW_LEN,
                               gFilesRowPtrs);
  if (pag.prevRow >= 0) gFilesEntryForRow[pag.prevRow] = -3;  // sentinel: prev page
  if (pag.nextRow >= 0) gFilesEntryForRow[pag.nextRow] = -4;  // sentinel: next page

  // Pad with inert spacer rows so directories with only a few entries don't
  // jump to a different vertical origin than dense directories.
  while (row < kFilesMinRenderRows && row < FILES_MAX_ROWS) {
    gFilesRows[row][0] = ' ';
    gFilesRows[row][1] = '\0';
    gFilesRowPtrs[row] = gFilesRows[row];
    // gFilesEntryForRow[] is already initialized to -1 for no-op rows.
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

// Case-insensitive ".bmp" suffix check. We only want the chooser path
// for files we can actually preview — anything else falls through to
// the existing metadata-only flow.
static bool isBmpFilename(const char* name) {
  if (!name) return false;
  size_t n = strlen(name);
  if (n < 4) return false;
  const char* ext = name + n - 4;
  return (ext[0] == '.') &&
         (ext[1] == 'b' || ext[1] == 'B') &&
         (ext[2] == 'm' || ext[2] == 'M') &&
         (ext[3] == 'p' || ext[3] == 'P');
}

// Same shape for ".jpg" and ".jpeg". Both extensions accepted because
// the camera-stream snapshot writes ".jpg" but a user might drop
// either form on the SD card. Routes through the same View / View
// Full chooser as BMP since the on-lens experience is identical
// (g2ShowJpgFile / g2ShowJpgFileFullScreen do the JPEG → 4-bpp BMP
// conversion before pushing through the same wire transport).
static bool isJpgFilename(const char* name) {
  if (!name) return false;
  size_t n = strlen(name);
  // ".jpg" — 4 chars
  if (n >= 4) {
    const char* ext = name + n - 4;
    if ((ext[0] == '.') &&
        (ext[1] == 'j' || ext[1] == 'J') &&
        (ext[2] == 'p' || ext[2] == 'P') &&
        (ext[3] == 'g' || ext[3] == 'G')) {
      return true;
    }
  }
  // ".jpeg" — 5 chars
  if (n >= 5) {
    const char* ext = name + n - 5;
    if ((ext[0] == '.') &&
        (ext[1] == 'j' || ext[1] == 'J') &&
        (ext[2] == 'p' || ext[2] == 'P') &&
        (ext[3] == 'e' || ext[3] == 'E') &&
        (ext[4] == 'g' || ext[4] == 'G')) {
      return true;
    }
  }
  return false;
}

static bool isJsonFilename(const char* name) {
  if (!name) return false;
  size_t n = strlen(name);
  if (n < 5) return false;
  const char* ext = name + n - 5;
  return (ext[0] == '.') &&
         (ext[1] == 'j' || ext[1] == 'J') &&
         (ext[2] == 's' || ext[2] == 'S') &&
         (ext[3] == 'o' || ext[3] == 'O') &&
         (ext[4] == 'n' || ext[4] == 'N');
}

// ".txt" — routed to the raw paged-text viewer (no pretty-parse).
static bool isTxtFilename(const char* name) {
  if (!name) return false;
  size_t n = strlen(name);
  if (n < 4) return false;
  const char* ext = name + n - 4;
  return (ext[0] == '.') &&
         (ext[1] == 't' || ext[1] == 'T') &&
         (ext[2] == 'x' || ext[2] == 'X') &&
         (ext[3] == 't' || ext[3] == 'T');
}

// ".csv" — same raw paged-text viewer. On-lens is a quick peek: rows soft-wrap
// at the lens column width; there is deliberately no aligned-table mode (the
// firmware collapses runs of spaces and only ~48 chars fit per line). Full
// tabular analysis lives in the web UI.
static bool isCsvFilename(const char* name) {
  if (!name) return false;
  size_t n = strlen(name);
  if (n < 4) return false;
  const char* ext = name + n - 4;
  return (ext[0] == '.') &&
         (ext[1] == 'c' || ext[1] == 'C') &&
         (ext[2] == 's' || ext[2] == 'S') &&
         (ext[3] == 'v' || ext[3] == 'V');
}

// Build an absolute VFS path for the cached chooser entry. Joins the
// FileManager's current directory with the entry name, collapsing the
// duplicate slash when the dir is "/".
static void buildChooserPath(char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  FileManager* fm = ensureFm();
  if (!fm) return;
  const char* dir = fm->getCurrentPath();
  if (!dir || !*dir) dir = "/";
  if (dir[0] == '/' && dir[1] == '\0') {
    snprintf(out, cap, "/%s", gFilesChooserEntry.name);
  } else {
    snprintf(out, cap, "%s/%s", dir, gFilesChooserEntry.name);
  }
}

// ---------------------------------------------------------------------------
// Rename / Delete flows. Both dispatch the REAL commands through cmd_exec
// (auth + [CMD] audit as the paired user — deliberately stricter than the
// OLED's direct-FileManager ops): `filerename "<old>" "<new>"` and the
// ONE-SHOT `filedelete "<path>" confirm` (the bare form arms an interactive
// CLI confirm no tap UI can answer). /system config stays protected below
// us at the VFS layer.
// ---------------------------------------------------------------------------

// Completion on cmd_exec_task. The Power page proves direct lens draws +
// vTaskDelay are accepted practice here (onPowerCmdDone precedent). Success
// forces a directory re-scan — FileManager::refresh() is generation-gated
// and would keep showing the deleted/old name.
static void onFilesMutateDone(bool ok, const char* result,
                              const G2CmdCookie& /*cookie*/, void* /*userData*/) {
  // executeCommand reports ok=true even for handler "Error: ..." strings —
  // judge by the result prefix, not the flag.
  const bool success = ok && result && strncmp(result, "OK", 2) == 0;
  const char* msg = (result && result[0]) ? result : (ok ? "Done" : "Failed");
  if (strncmp(msg, "OK: ", 4) == 0) msg += 4;  // banner brevity
  strncpy(gFilesMutateMsg, msg, sizeof(gFilesMutateMsg) - 1);
  gFilesMutateMsg[sizeof(gFilesMutateMsg) - 1] = '\0';
  DEBUG_G2F("[G2] Files: mutate done ok=%d success=%d '%s'",
            (int)ok, (int)success, gFilesMutateMsg);

  if (g2GetHijackPage() != G2_HIJACK_PAGE_FILES) return;  // user navigated away

  g2ShowText(gFilesMutateMsg);
  vTaskDelay(pdMS_TO_TICKS(1300));
  if (success) {
    FileManager* fm = ensureFm();
    if (fm) fm->forceRescan();
  }
  if (g2GetHijackPage() == G2_HIJACK_PAGE_FILES) g2ShowFilesMenu();
}

static void filesSubmitMutate(const char* line) {
  G2CmdCookie cookie{};
  cookie.targetPage = g2GetHijackPage();
  if (!g2SubmitHijackCommand(line, cookie, onFilesMutateDone, nullptr)) {
    DEBUG_G2F("[G2] Files: mutate submit FAILED — no inline fallback");
    g2ShowFilesMenu();
  }
}

// Keyboard callbacks (BLE notify task — String build + submit only; the
// list redraw on cancel is cache-hot, no FS scan).
static void filesRenameCommit(const char* text) {
  if (!text || text[0] == '\0' || strcmp(text, gFilesActionName) == 0) {
    DEBUG_G2F("[G2] Files: rename empty/unchanged — cancelled");
    g2ShowFilesMenu();
    return;
  }
  String line = String("filerename \"") + gFilesActionPath + "\" \"" + text + "\"";
  filesSubmitMutate(line.c_str());
}

static void filesRenameCancel() {
  DEBUG_G2F("[G2] Files: rename cancelled");
  g2ShowFilesMenu();
}

// Delete confirm — Power-page two-level pattern: Cancel row, inert
// "Delete <name>?" header, explicit Confirm row.
static void showDeleteConfirm() {
  gFilesConfirmActive = true;
  snprintf(gFilesConfirmRow, sizeof(gFilesConfirmRow), "Delete %.56s?", gFilesActionName);
  const char* rows[] = { "<- Cancel", gFilesConfirmRow, "Confirm Delete" };
  g2ShowListPage(rows, 3);
  g2LensClearOverlay();
  DEBUG_G2F("[G2] Files: delete confirm for '%s'", gFilesActionPath);
}

// Show the View / View Full / Info chooser for a previewable file.
// Stays in the FILES hijack page so g2FilesHandleTap routes the
// chooser taps; the gFilesChooserActive flag tells the dispatcher to
// interpret rows as chooser actions instead of file-list rows.
//
// "View"      — small image at native 288×144 (top-left of the lens).
// "View Full" — same source upscaled 2× to 576×288 via 4-tile push.
// "Info"      — metadata page (name / size / type).
static void showFileChooser(const FileEntry& e, FilesChooserKind kind) {
  gFilesChooserEntry  = e;
  gFilesChooserActive = true;
  gFilesChooserKind   = kind;

  static char title[FILES_ROW_LEN];
  snprintf(title, sizeof(title), "%s", e.name);

  // Build rows + the action table in lockstep (see FilesAction).
  static const char* rows[FILES_CHOOSER_MAX_ROWS];
  gFilesChooserRowN = 0;
  auto add = [&](const char* label, FilesAction act) {
    if (gFilesChooserRowN < FILES_CHOOSER_MAX_ROWS) {
      rows[gFilesChooserRowN] = label;
      gFilesChooserAct[gFilesChooserRowN] = act;
      gFilesChooserRowN++;
    }
  };
  add("<- Files", FACT_BACK);
  add(title,      FACT_HEADER);   // header line so the user knows which file
  if (kind == FILE_CHOOSER_BMP || kind == FILE_CHOOSER_JPG) {
    add("View",      FACT_VIEW);
    add("View Full", FACT_VIEW_FULL);  // 2x upscale -> 4-tile full-canvas
  } else if (kind == FILE_CHOOSER_TEXT) {
    add("View", FACT_VIEW);
  } else if (kind == FILE_CHOOSER_JSON) {
    add("Pretty View", FACT_VIEW);
    add("JSON View",   FACT_JSON_RAW);
  }
  // FILE_CHOOSER_OTHER adds no view rows — Info/Rename/Delete only.
  add("Info",   FACT_INFO);
  add("Rename", FACT_RENAME);
  add("Delete", FACT_DELETE);
  g2ShowListPage(rows, gFilesChooserRowN);

  // No overlay — chooser is a deliberate stop, not a flash.
  g2LensClearOverlay();
  DEBUG_G2F("[G2] Files: chooser shown for '%s' (kind=%d, %u rows)",
            e.name, (int)kind, (unsigned)gFilesChooserRowN);
}

// Render the pager's current page. The chrome (title buffer, hints, separator)
// is fixed; only gFilesTextTitle's contents change per file. The single-page
// hint is "2x-tap=exit" because a single tap is swallowed on the FILES hijack
// page (its list dispatcher consumes CLICK), so double-tap is the real exit.
static bool filesRenderTextPage() {
  static const G2TextPageChrome chrome = {
      gFilesTextTitle,                 // title (mutated in place before render)
      "tap/scroll=nav, 2x-tap=exit",   // multi-page hint
      "2x-tap=exit",                   // single-page hint
      "--------------------",          // separator rule
      "(empty file)" };                // empty-slice text
  return g2TextPagerRender(gFilesPager, gFilesTextPageBuf, sizeof(gFilesTextPageBuf),
                           chrome, G2_GEOM_LARGE, exitTextViewBackToFiles,
                           filesTextNav);
}

// tap/scroll page navigation. NEXT (tap / scroll-down) advances; PREV
// (scroll-up) goes back; both wrap. Re-renders after moving.
static void filesTextNav(G2TapKind kind) {
  textNavPage(gFilesPager, kind != G2_TAP_PAGE_PREV);
  filesRenderTextPage();
}

static void exitTextViewBackToFiles() {
  gFilesPager.pageCount = 0;
  gFilesPager.curPage   = 0;
  gFilesPager.truncated = false;
  gFilesTextTitle[0]    = '\0';
  g2ShowFilesMenu();
}

// Read the chooser entry, optionally JSON-pretty-print it, wrap it, and page it
// onto the lens via the shared TextPager. `pretty` only applies to JSON;
// .txt/.csv always call with pretty=false. Returns false if the swap couldn't
// be shown (caller falls back to the file list).
static bool showTextFileViaWidget(bool pretty) {
  // Install the G2-paired user's identity + notification source for this
  // page action. The g2_tap_disp worker that drives this page leaves the
  // task's TLS slots at their defaults (ANON identity, UNKNOWN notif src),
  // so canRead/readTextLimited below would see no user and deny restricted
  // files, and any notify*() fired during the read would attribute to
  // "Unknown". G2HijackCtxGuard installs both in one composed RAII scope.
  G2HijackCtxGuard ctxGuard;

  char path[FILE_MANAGER_MAX_PATH + 32];
  buildChooserPath(path, sizeof(path));
  if (!path[0]) return false;

  if (!canRead(String(path), currentAuthContext())) {
    DEBUG_G2F("[G2] Files text: blocked by canRead '%s'", path);
    return g2ShowTextPage("Viewer blocked: read permission denied.", G2_GEOM_LARGE,
                          exitTextViewBackToFiles, nullptr);
  }

  String raw;
  if (!readTextLimited(path, raw, kFilesTextReadCapBytes)) {
    DEBUG_G2F("[G2] Files text: read failed '%s'", path);
    return false;
  }
  const bool hitReadCap = raw.length() >= kFilesTextReadCapBytes;

  // Sealed capture? Reveal for the lens — this runs under the G2-paired
  // user's identity (ctxGuard above) and behind the canRead gate; the
  // '#HW1ENC' first line stays visible as the mark.
  captureCryptoRevealText(raw);

  String display;
  if (pretty) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, raw);
    if (err) {
      display = "// Pretty parse failed: ";
      display += err.c_str();
      display += "\n// Falling back to raw JSON text\n\n";
      display += raw;
      DEBUG_G2F("[G2] Files text: pretty parse failed '%s' (%s)",
                path, err.c_str());
    } else {
      serializeJsonPretty(doc, display);
    }
  } else {
    display = raw;
  }

  // Wrap into the shared body buffer: strips '\r' + control bytes, soft-wraps
  // long lines at the lens column width (so wide CSV rows stay legible), and
  // flags truncation when the source outgrows the displayable body.
  bool wrapTrunc = false;
  size_t bodyLen = textWrapInto(gFilesTextBody, sizeof(gFilesTextBody),
                                  display.c_str(), G2_TEXT_DEFAULT_COLS,
                                  /*contIndent=*/0, /*stripCtrl=*/true,
                                  &wrapTrunc);
  gFilesPager.curPage   = 0;
  gFilesPager.truncated = (hitReadCap || wrapTrunc);
  textSplitPages(gFilesPager, bodyLen);
  if (gFilesPager.pageCount == 0) {
    // Empty (or all-control-byte) file — show a single "(empty file)" page.
    gFilesPager.pageCount = 1;
    gFilesTextPageOff[0]  = 0;
    gFilesTextPageOff[1]  = 0;
  }

  const char* base = strrchr(path, '/');
  base = base ? (base + 1) : path;
  snprintf(gFilesTextTitle, sizeof(gFilesTextTitle), "%s%s",
           pretty ? "Pretty " : "", base);

  DEBUG_G2F("[G2] Files text: %s '%s' src=%uB body=%uB cap=%uB pages=%u trunc=%d",
            pretty ? "pretty" : "raw", path, (unsigned)display.length(),
            (unsigned)bodyLen, (unsigned)kFilesTextReadCapBytes,
            (unsigned)gFilesPager.pageCount, gFilesPager.truncated ? 1 : 0);
  return filesRenderTextPage();
}

static void showFileInfo(const FileEntry& e) {
  // Render the metadata as a list page (one item per field) so we stay in
  // a list-widget container — the firmware bails out if we REBUILD a text
  // widget into a container CREATEd as a list. Page stays as FILES so
  // the existing Files tap dispatcher handles "back" correctly (returns to
  // the file list, not the main hijack menu).
  const char* ext = strrchr(e.name, '.');
  if (!ext || !*ext) ext = "(no ext)";
  // Sealed capture? One 7-byte guarded peek. The info overlay is where the
  // encrypted-at-rest mark lives on the lens — the directory listing stays
  // sniff-free (a per-entry open would drag on 256-file capture folders).
  bool sealedFile = false;
  if (!e.isFolder) {
    G2HijackCtxGuard ctxGuard;
    char p[FILE_MANAGER_MAX_PATH + 32];
    buildChooserPath(p, sizeof(p));
    String head;
    if (p[0] && canRead(String(p), currentAuthContext()) &&
        readTextLimited(p, head, sizeof(CAPCRYPT_MAGIC_PREFIX) - 1)) {
      sealedFile = captureCryptoIsMagicLine(head.c_str());
    }
  }
  uint32_t sz = e.size;
  char sizeStr[24];
  if      (sz < 1024)             snprintf(sizeStr, sizeof(sizeStr), "%u bytes",  (unsigned)sz);
  else if (sz < 1024UL * 1024UL)  snprintf(sizeStr, sizeof(sizeStr), "%.1f KB",   sz / 1024.0);
  else                            snprintf(sizeStr, sizeof(sizeStr), "%.2f MB",   sz / (1024.0 * 1024.0));

  static char nameRow[FILES_ROW_LEN];
  static char sizeRow[FILES_ROW_LEN];
  static char typeRow[FILES_ROW_LEN];
  static const char* rows[4];
  rows[0] = "<- Files";   // back to the directory listing
  snprintf(nameRow, sizeof(nameRow), "Name: %s", e.name);
  snprintf(sizeRow, sizeof(sizeRow), "Size: %s", sizeStr);
  snprintf(typeRow, sizeof(typeRow), "Type: %s%s", ext,
           sealedFile ? " (encrypted)" : "");
  rows[1] = nameRow;
  rows[2] = sizeRow;
  rows[3] = typeRow;
  g2ShowListPage(rows, 4);
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
  // Run all FS work under the paired-by user's identity. Without this guard
  // the task's TLS identity stays at ANON (the safe default) and
  // FileManager::navigate() denies every read.
  G2HijackCtxGuard ctxGuard;

  // Directory cache policy for the per-boot FileManager singleton:
  //
  //   • Re-entry from outside Files (Apps → Files, post safety-timeout,
  //     Main Menu → … → Files): forceRescan(). refresh() is identity-
  //     generation gated and would keep showing stale sizes / missing
  //     files written by sensorlog / web / CLI while the user was away.
  //     Selection is preserved by loadDirectory().
  //
  //   • Already on Files (overlay dismiss, viewer callback, folder
  //     navigate which already loaded): refresh() only. That covers the
  //     ANON-first-tap → later-paired empty-cache case without paying
  //     the full VFS permission walk on every in-page redraw.
  if (FileManager* fmRefresh = ensureFm()) {
    if (g2GetHijackPage() != G2_HIJACK_PAGE_FILES) {
      fmRefresh->forceRescan();
      gFilesPage = 0;  // re-entry from outside → start at the first page
    } else {
      fmRefresh->refresh();
    }
  }

  // Whenever the real file list comes back up, the chooser and the
  // delete-confirm are unconditionally gone — covers the View/Info → Back
  // path, the post-BMP-dismiss callback, and any "redraw Files from main
  // menu" entry, all without each call site having to remember.
  gFilesChooserActive = false;
  gFilesConfirmActive = false;
  size_t n = buildRows();
  const char* path = "/";
  FileManager* fmMenu = ensureFm();
  if (fmMenu) path = fmMenu->getCurrentPath();
  // Fit ~2–3 wrapped lines in kFilesPathGeom (280×100); still bound the
  // string so a pathological deep path cannot blow the CREATE pb.
  truncateInto(gFilesPathTitleBuf, sizeof(gFilesPathTitleBuf), path,
               FILES_PATH_TITLE_LEN - 1);

  const G2TextChildSpec pathTitle = {
      "filesPath", gFilesPathTitleBuf, 99, kFilesPathGeom, false };

  if (g2ShowMixedListText(gFilesRowPtrs, n, kFilesListGeom, pathTitle)) {
    g2SetHijackPage(G2_HIJACK_PAGE_FILES);
    DEBUG_G2F("[G2] Files menu shown (list+path title, rows=%u)", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Files menu show FAILED");
  }
  // Cancel any in-flight overlay since we explicitly reshowed the list.
  g2LensClearOverlay();
}

void g2FilesHandleTap(uint32_t idx) {
  // Same guard as g2ShowFilesMenu — every tap may navigate, view a
  // file, or invoke the JSON viewer, all of which hit guarded VFS.
  G2HijackCtxGuard ctxGuard;

  // Delete-confirm dispatcher — highest priority while up. Rows:
  // 0=<- Cancel, 1=inert "Delete <name>?", 2=Confirm Delete.
  if (gFilesConfirmActive) {
    if (idx == 0) {
      gFilesConfirmActive = false;
      DEBUG_G2F("[G2] Files: delete cancelled");
      g2ShowFilesMenu();
      return;
    }
    if (idx == 2) {
      gFilesConfirmActive = false;
      // ONE-SHOT confirm token (bare, unquoted) — the interactive two-step
      // flow can't be answered from a tap UI.
      String line = String("filedelete \"") + gFilesActionPath + "\" confirm";
      DEBUG_G2F("[G2] Files: delete confirmed -> '%s'", gFilesActionPath);
      filesSubmitMutate(line.c_str());
      return;
    }
    return;  // header row / unexpected — leave confirm up
  }

  // Chooser dispatcher — runs ahead of every other path so the chooser
  // is the only consumer of taps while it's up. Row meaning comes from
  // the action table filled by showFileChooser (never from fixed indices).
  if (gFilesChooserActive) {
    FileEntry cached = gFilesChooserEntry;
    if (idx >= gFilesChooserRowN) {
      DEBUG_G2F("[G2] Files: chooser tap idx=%u out of range", (unsigned)idx);
      return;
    }
    const FilesAction act = gFilesChooserAct[idx];
    const bool isImage = (gFilesChooserKind == FILE_CHOOSER_BMP ||
                          gFilesChooserKind == FILE_CHOOSER_JPG);
    switch (act) {
      case FACT_BACK:
        gFilesChooserActive = false;
        DEBUG_G2F("[G2] Files: chooser back → file list");
        g2ShowFilesMenu();
        return;

      case FACT_HEADER:
        return;  // inert filename row — leave chooser up

      case FACT_VIEW: {
        if (isImage) {
          // View -> push at native 288×144. BMP path uses the file bytes
          // directly; JPG decodes via fmt2rgb888 then shares the transport.
          char path[FILE_MANAGER_MAX_PATH + 32];
          buildChooserPath(path, sizeof(path));
          const bool isJpg = (gFilesChooserKind == FILE_CHOOSER_JPG);
          gFilesChooserActive = false;
          DEBUG_G2F("[G2] Files: chooser View -> '%s' (%s)", path, isJpg ? "JPG" : "BMP");
          const bool ok = isJpg ? g2ShowJpgFile(path, &g2ShowFilesMenu)
                                : g2ShowBmpFile(path, &g2ShowFilesMenu);
          if (!ok) g2ShowFilesMenu();
        } else {
          // TEXT = raw paged view; JSON's "Pretty View" = pretty-parsed.
          const bool pretty = (gFilesChooserKind == FILE_CHOOSER_JSON);
          gFilesChooserActive = false;
          DEBUG_G2F("[G2] Files: chooser View -> '%s' (pretty=%d)", cached.name, (int)pretty);
          if (!showTextFileViaWidget(pretty)) g2ShowFilesMenu();
        }
        return;
      }

      case FACT_VIEW_FULL: {
        // View Full -> 2× upscale to 576×288 via 4-tile push.
        char path[FILE_MANAGER_MAX_PATH + 32];
        buildChooserPath(path, sizeof(path));
        const bool isJpg = (gFilesChooserKind == FILE_CHOOSER_JPG);
        gFilesChooserActive = false;
        DEBUG_G2F("[G2] Files: chooser View Full -> '%s' (%s)", path, isJpg ? "JPG" : "BMP");
        const bool ok = isJpg ? g2ShowJpgFileFullScreen(path, &g2ShowFilesMenu)
                              : g2ShowBmpFileFullScreen(path, &g2ShowFilesMenu);
        if (!ok) g2ShowFilesMenu();
        return;
      }

      case FACT_JSON_RAW:
        gFilesChooserActive = false;
        DEBUG_G2F("[G2] Files: chooser JSON View -> '%s'", cached.name);
        if (!showTextFileViaWidget(/*pretty=*/false)) g2ShowFilesMenu();
        return;

      case FACT_INFO:
        gFilesChooserActive = false;
        DEBUG_G2F("[G2] Files: chooser Info → '%s'", cached.name);
        showFileInfo(cached);
        return;

      case FACT_RENAME: {
        // Snapshot the target NOW — the chooser slot is cleared by list
        // redraws and the keyboard doesn't block the scan worker.
        buildChooserPath(gFilesActionPath, sizeof(gFilesActionPath));
        strncpy(gFilesActionName, cached.name, sizeof(gFilesActionName) - 1);
        gFilesActionName[sizeof(gFilesActionName) - 1] = '\0';
        gFilesChooserActive = false;
        if (strchr(gFilesActionPath, '"')) {
          // CommandArgs has no escapes — an embedded quote breaks the line.
          g2ShowTextAsList("Name has a quote char - rename on web", "<- Back");
          return;
        }
        if (strlen(gFilesActionName) > 32) {
          // Keyboard cap is 32 and the pre-fill silently truncates — an
          // unguarded Done would rename to the 32-char prefix.
          g2ShowTextAsList("Name too long - rename on web", "<- Back");
          return;
        }
        TextEntryConfig cfg = {};
        cfg.prompt   = "Rename";
        cfg.initial  = gFilesActionName;
        cfg.maxLen   = 32;
        cfg.onCommit = filesRenameCommit;
        cfg.onCancel = filesRenameCancel;
        if (!g2BeginTextEntry(cfg)) {
          DEBUG_G2F("[G2] Files: rename text-entry failed to start");
          g2ShowFilesMenu();
        }
        return;
      }

      case FACT_DELETE:
        buildChooserPath(gFilesActionPath, sizeof(gFilesActionPath));
        strncpy(gFilesActionName, cached.name, sizeof(gFilesActionName) - 1);
        gFilesActionName[sizeof(gFilesActionName) - 1] = '\0';
        gFilesChooserActive = false;
        if (strchr(gFilesActionPath, '"')) {
          g2ShowTextAsList("Name has a quote char - delete on web", "<- Back");
          return;
        }
        showDeleteConfirm();
        return;
    }
    return;
  }

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

  if (entryIdx == -3) {
    // "<< Prev page"
    if (gFilesPage > 0) gFilesPage--;
    DEBUG_G2F("[G2] Files: prev page -> %u", (unsigned)gFilesPage);
    g2ShowFilesMenu();
    return;
  }
  if (entryIdx == -4) {
    // "Next page >>" — g2PaginatorPrepare clamps past-the-end, so a stale
    // tap after the directory shrank lands on the last valid page.
    gFilesPage++;
    DEBUG_G2F("[G2] Files: next page -> %u", (unsigned)gFilesPage);
    g2ShowFilesMenu();
    return;
  }

  if (entryIdx == -2) {
    // Parent dir. Fresh directory → fresh page.
    DEBUG_G2F("[G2] Files: navigateUp from '%s'", fm->getCurrentPath());
    fm->navigateUp();
    gFilesPage = 0;
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
    gFilesPage = 0;  // fresh directory → fresh page
    g2ShowFilesMenu();
  } else {
    // File tap → previewable types get the View/Info chooser; everything
    // else jumps straight to the metadata overlay (existing UX).
    if (isBmpFilename(e.name)) {
      showFileChooser(e, FILE_CHOOSER_BMP);
    } else if (isJpgFilename(e.name)) {
      showFileChooser(e, FILE_CHOOSER_JPG);
    } else if (isJsonFilename(e.name)) {
      showFileChooser(e, FILE_CHOOSER_JSON);
    } else if (isTxtFilename(e.name) || isCsvFilename(e.name)) {
      showFileChooser(e, FILE_CHOOSER_TEXT);
    } else {
      // No viewer for this type, but Info / Rename / Delete must still be
      // reachable — the old direct-to-info path made mutation impossible
      // for every non-previewable file.
      showFileChooser(e, FILE_CHOOSER_OTHER);
    }
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
  // remove once G2_Glasses.cpp's g2Tick stops calling it.
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
