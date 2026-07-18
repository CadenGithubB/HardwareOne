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
// every time g2ShowFilesMenu runs we call fm->refresh(), which is a
// no-op when the cache's generation matches gIdentityGeneration and a
// re-scan otherwise. Producers (pairing, user add/del/promote/demote)
// bump the generation, so the next refresh() picks up the new picture.
// If you add a new caching subsystem that depends on auth, follow the
// same pattern.

#include "G2_Page_Files.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_Glasses.h"
#include "G2_Page_Common.h"      // G2TextPager + paging ops (shared text viewer)
#include "G2_HijackCmd.h"        // G2HijackCtxGuard — installs pairedByUser identity
#include "System_FileManager.h"
#include "System_Filesystem.h"
#include "System_Debug.h"
#include "System_AuthIdentity.h"

#include <ArduinoJson.h>
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

static FileManager* gFilesFm = nullptr;

// Row buffer + pointer table for list rendering. Sized to fit a handful of
// "header" rows (back, ..) plus FILE_MANAGER_MAX_CACHED_ITEMS data rows.
#define FILES_MAX_ROWS  (FILE_MANAGER_MAX_CACHED_ITEMS + 4)
#define FILES_ROW_LEN   48
// Keep list placement visually stable across directories with very different
// item counts. The firmware tends to vertically center short lists in a tall
// container; padding to a fixed minimum row count keeps the first rows near
// the same top-left position as you browse (/photos vs /sd, etc.).
static constexpr size_t kFilesMinRenderRows = 8;

// Row buffers in PSRAM — ~3.3 KB total. Filled by buildRows() from the
// page worker, read by the BLE notify task; both run in regular task
// context, no DMA / ISR access.
EXT_RAM_BSS_ATTR static char        gFilesRows[FILES_MAX_ROWS][FILES_ROW_LEN];
EXT_RAM_BSS_ATTR static const char* gFilesRowPtrs[FILES_MAX_ROWS];
EXT_RAM_BSS_ATTR static char        gFilesPathTitleBuf[FILES_ROW_LEN];
static size_t      gFilesRowCount = 0;

// Mixed list+text layout: keep the path in a TextObject at top-right while
// pinning the list to the top-left quadrant/half (not vertically centered).
// LEFT_HALF is {8,8,280,272}, so row 0 starts near the top edge.
static constexpr G2ContainerGeom kFilesListGeom = G2_GEOM_LEFT_HALF;
// Text is top-left within its box — anchor the box on the right for "top right"
// path readout without consuming a list row.
static constexpr G2ContainerGeom kFilesPathGeom = { 280,   8, 288,  40 };

// Indices we keep so taps know what idx → which directory entry. -1 = not a
// real entry (back / blank); -2 = parent row ".. (up)".
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
                          //  (readTextLimited → wrap → G2TextPager). Its
                          //  chooser is 4 rows, so Info sits at idx 3.
};
static FilesChooserKind gFilesChooserKind = FILE_CHOOSER_BMP;

// Paged text viewer (JSON pretty/raw, .txt, .csv). Shares the G2TextPager
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
static G2TextPager gFilesPager = {
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
    gFilesFm = new FileManager();
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

  if (row == (atRoot ? 1u : 2u)) {
    // No entries — show empty marker.
    snprintf(gFilesRows[row], FILES_ROW_LEN, "(empty)");
    gFilesRowPtrs[row] = gFilesRows[row];
    row++;
  }

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
  if (kind == FILE_CHOOSER_BMP || kind == FILE_CHOOSER_JPG) {
    static const char* rows[5];
    rows[0] = "<- Files";
    rows[1] = title;       // header line so the user knows which file
    rows[2] = "View";
    rows[3] = "View Full"; // 2x upscale -> 4-tile full-canvas
    rows[4] = "Info";
    g2ShowListPage(rows, 5);
  } else if (kind == FILE_CHOOSER_TEXT) {
    // .txt / .csv — one raw paged view + metadata. No pretty-parse, so this
    // is a 4-row list with Info at idx 3 (g2FilesHandleTap keys off the
    // chooser kind to route idx 2/3 correctly).
    static const char* rows[4];
    rows[0] = "<- Files";
    rows[1] = title;
    rows[2] = "View";
    rows[3] = "Info";
    g2ShowListPage(rows, 4);
  } else {
    static const char* rows[5];
    rows[0] = "<- Files";
    rows[1] = title;
    rows[2] = "Pretty View";
    rows[3] = "JSON View";
    rows[4] = "Info";
    g2ShowListPage(rows, 5);
  }
  // No overlay — chooser is a deliberate stop, not a flash.
  g2LensClearOverlay();
  const char* kindStr = (kind == FILE_CHOOSER_BMP)  ? "bmp"
                      : (kind == FILE_CHOOSER_JPG)  ? "jpg"
                      : (kind == FILE_CHOOSER_TEXT) ? "text"
                                                    : "json";
  DEBUG_G2F("[G2] Files: chooser shown for '%s' (%s)", e.name, kindStr);
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
  g2TextNavPage(gFilesPager, kind != G2_TAP_PAGE_PREV);
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
// onto the lens via the shared G2TextPager. `pretty` only applies to JSON;
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
  size_t bodyLen = g2TextWrapInto(gFilesTextBody, sizeof(gFilesTextBody),
                                  display.c_str(), G2_TEXT_DEFAULT_COLS,
                                  /*contIndent=*/0, /*stripCtrl=*/true,
                                  &wrapTrunc);
  gFilesPager.curPage   = 0;
  gFilesPager.truncated = (hitReadCap || wrapTrunc);
  g2TextSplitPages(gFilesPager, bodyLen);
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
  snprintf(typeRow, sizeof(typeRow), "Type: %s", ext);
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

  // Re-scan the current directory under the now-installed identity. The
  // FileManager is a per-boot singleton: its first navigate("/") happens
  // on the very first Files tap, and if that tap ran with ANON
  // (pairedByUser blank — pre-pairing or stuck-state — the cache lands
  // at totalItems=0 and would stay there for the rest of the boot, even
  // after pairing is fixed. refresh() rebuilds the entry list without
  // touching scroll/selection state. Cheap, runs once per menu (re-)entry.
  if (FileManager* fmRefresh = ensureFm()) {
    fmRefresh->refresh();
  }

  // Whenever the real file list comes back up, the chooser is
  // unconditionally gone — covers the View/Info → Back path, the
  // post-BMP-dismiss callback, and any "redraw Files from main menu"
  // entry, all without each call site having to remember.
  gFilesChooserActive = false;
  size_t n = buildRows();
  const char* path = "/";
  FileManager* fmMenu = ensureFm();
  if (fmMenu) path = fmMenu->getCurrentPath();
  // Shorten for the ~288 px-wide title strip (monospace-ish lens font).
  truncateInto(gFilesPathTitleBuf, sizeof(gFilesPathTitleBuf), path, 36);

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

  // Chooser dispatcher — runs ahead of every other path so the chooser
  // is the only consumer of taps while it's up. Rows: 0=<- Files,
  // 1=filename header (no-op), 2/3 action rows, 4=Info.
  if (gFilesChooserActive) {
    FileEntry cached = gFilesChooserEntry;
    if (idx == 0) {
      gFilesChooserActive = false;
      DEBUG_G2F("[G2] Files: chooser back → file list");
      g2ShowFilesMenu();
      return;
    }
    if (idx == 2) {
      if (gFilesChooserKind == FILE_CHOOSER_BMP ||
          gFilesChooserKind == FILE_CHOOSER_JPG) {
        // View -> push at native 288×144. BMP path uses the file
        // bytes directly; JPG path decodes via fmt2rgb888 first then
        // shares the same wire transport.
        char path[FILE_MANAGER_MAX_PATH + 32];
        buildChooserPath(path, sizeof(path));
        const bool isJpg = (gFilesChooserKind == FILE_CHOOSER_JPG);
        gFilesChooserActive = false;
        DEBUG_G2F("[G2] Files: chooser View -> '%s' (%s)",
                  path, isJpg ? "JPG" : "BMP");
        const bool ok = isJpg ? g2ShowJpgFile(path, &g2ShowFilesMenu)
                              : g2ShowBmpFile(path, &g2ShowFilesMenu);
        if (!ok) {
          DEBUG_G2F("[G2] Files: View dispatch failed — falling back to list");
          g2ShowFilesMenu();
        }
      } else if (gFilesChooserKind == FILE_CHOOSER_TEXT) {
        // .txt / .csv — single raw paged view.
        gFilesChooserActive = false;
        DEBUG_G2F("[G2] Files: chooser text View -> '%s'", cached.name);
        if (!showTextFileViaWidget(/*pretty=*/false)) {
          g2ShowFilesMenu();
        }
      } else {
        gFilesChooserActive = false;
        DEBUG_G2F("[G2] Files: chooser Pretty View -> '%s'", cached.name);
        if (!showTextFileViaWidget(/*pretty=*/true)) {
          g2ShowFilesMenu();
        }
      }
      return;
    }
    if (idx == 3) {
      if (gFilesChooserKind == FILE_CHOOSER_BMP ||
          gFilesChooserKind == FILE_CHOOSER_JPG) {
        // View Full -> 2× upscale to 576×288 via 4-tile push. Same
        // fork as View above.
        char path[FILE_MANAGER_MAX_PATH + 32];
        buildChooserPath(path, sizeof(path));
        const bool isJpg = (gFilesChooserKind == FILE_CHOOSER_JPG);
        gFilesChooserActive = false;
        DEBUG_G2F("[G2] Files: chooser View Full -> '%s' (%s)",
                  path, isJpg ? "JPG" : "BMP");
        const bool ok = isJpg ? g2ShowJpgFileFullScreen(path, &g2ShowFilesMenu)
                              : g2ShowBmpFileFullScreen(path, &g2ShowFilesMenu);
        if (!ok) {
          DEBUG_G2F("[G2] Files: View Full dispatch failed — falling back to list");
          g2ShowFilesMenu();
        }
      } else if (gFilesChooserKind == FILE_CHOOSER_TEXT) {
        // Text chooser row 3 is Info (metadata), not a second view mode.
        gFilesChooserActive = false;
        DEBUG_G2F("[G2] Files: chooser text Info -> '%s'", cached.name);
        showFileInfo(cached);
      } else {
        gFilesChooserActive = false;
        DEBUG_G2F("[G2] Files: chooser JSON View -> '%s'", cached.name);
        if (!showTextFileViaWidget(/*pretty=*/false)) {
          g2ShowFilesMenu();
        }
      }
      return;
    }
    if (idx == 4) {
      gFilesChooserActive = false;
      DEBUG_G2F("[G2] Files: chooser Info → '%s'", cached.name);
      showFileInfo(cached);
      return;
    }
    // idx 1 (filename header) and any unexpected row — leave chooser up.
    DEBUG_G2F("[G2] Files: chooser tap idx=%u — no-op", (unsigned)idx);
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
      showFileInfo(e);
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
