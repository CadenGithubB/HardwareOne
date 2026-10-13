// =============================================================================
// G2 glasses — "Files" page implementation
// =============================================================================
// Wraps a private FileManager instance. We deliberately don't share
// gOledFileManager — the OLED browser uses it from the OLED render thread, so
// reusing it here would mean either coordinating selection state across two
// UIs or fighting over the cursor. A separate instance is cheap (~couple KB)
// and decouples the two paths cleanly.
//
// File-metadata overlay: Info replaces the list briefly with metadata rows
// (name, size, type). We can't get a "list still showing" signal from the
// lens, so the centralized lens overlay clock (g2Tick) times it: after
// kFileInfoOverlayMs filesOverlayExpired queues a list redraw.
//
// TASKS / LOCKING
// ---------------
// Taps, the rename keyboard and the text pager run on g2_tap_disp; queued
// redraws run on the lens applier (g2_page_swap_w); the g2files CLI summary
// runs on cmd_exec_task. All of them take the Files page lock (FilesPageLock)
// before touching gFilesFm, the row buffers or the page/mode state. The
// rename/delete completion (cmd_exec_task), the overlay expiry (main loop) and
// the image viewers' onDone (session worker) never take it and never draw:
// they publish under gFilesRedrawMux and queue a menuGen-fenced redraw.
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
#include "FileBrowser_FileKind.h"  // file classifier shared with the OLED browser
#include "System_FileManager.h"
#include "System_Filesystem.h"
#include "System_VFS.h"          // internal / SD capacity + mount state
#include "System_CaptureCrypto.h"  // reveal sealed captures on the lens viewer
#include "System_MemUtil.h"   // ps_alloc — gFilesFm lives in PSRAM (placement-new)
#include "System_PsramBuffer.h"
#include "System_TextWrapStream.h"
#include "System_Mutex.h"
#include <new>                // placement-new
#include "System_Debug.h"
#include "System_AuthIdentity.h"

#include <ArduinoJson.h>
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

static FileManager* gFilesFm = nullptr;

// Files page lock (see TASKS / LOCKING above). Recursive because public
// entries nest (a tap redraws the list). While holding it a task waits only on
// tasks that never take it (FS lock, session worker, lens BLE exchanges); the
// one wait that can involve the lens applier, a full lens queue, gives up
// after 50 ms. Created at static init like G2_Health's series mutex.
static StaticSemaphore_t gFilesLockStorage;
static SemaphoreHandle_t gFilesLock =
    xSemaphoreCreateRecursiveMutexStatic(&gFilesLockStorage);

class FilesPageLock {
 public:
  FilesPageLock()
      : held_(gFilesLock &&
              xSemaphoreTakeRecursive(gFilesLock, portMAX_DELAY) == pdTRUE) {}
  ~FilesPageLock() {
    if (held_) xSemaphoreGiveRecursive(gFilesLock);
  }
  FilesPageLock(const FilesPageLock&) = delete;
  FilesPageLock& operator=(const FilesPageLock&) = delete;

 private:
  bool held_;
};

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

// Row buffers in PSRAM — ~1 KB total. Filled and read only under the Files
// page lock (taps, lens-applier redraws); never by the BLE callback. Regular
// task context only, with no DMA / ISR access.
EXT_RAM_BSS_ATTR static char        gFilesRows[FILES_MAX_ROWS][FILES_ROW_LEN];
EXT_RAM_BSS_ATTR static const char* gFilesRowPtrs[FILES_MAX_ROWS];
// Path is shown in the right-hand TextObject (not a list row). Keep its own
// bound so deep VFS paths can wrap without consuming the complete CREATE
// payload. The surrounding sidebar also carries compact storage rows.
#define FILES_PATH_TITLE_LEN  96
EXT_RAM_BSS_ATTR static char        gFilesPathTitleBuf[FILES_PATH_TITLE_LEN];
#define FILES_SIDEBAR_LEN  192
EXT_RAM_BSS_ATTR static char        gFilesSidebarBuf[FILES_SIDEBAR_LEN];
static size_t      gFilesRowCount = 0;

// Current paginator page for the directory listing. Reset to 0 on any
// directory change (navigate in/up, re-entry from outside Files);
// preserved across in-page redraws (chooser dismiss, overlay expiry) —
// g2PaginatorPrepare clamps it if the directory shrank meanwhile.
static size_t      gFilesPage = 0;

// Mixed list+text layout: storage + path in a TextObject at right, list
// pinned top-left (not vertically centered). Uses the production split
// (264px list / 288px right column) shared with Health, Maps, and the
// keyboard pad — not LEFT_HALF/RIGHT_HALF, whose 280px right pane is
// narrower than that column. The rename keyboard is a separate page.
static constexpr G2ContainerGeom kFilesListGeom    = G2_GEOM_SPLIT_LIST;
static constexpr G2ContainerGeom kFilesSidebarGeom = G2_GEOM_SPLIT_RIGHT;
static constexpr size_t kFilesSidebarCols =
    (G2_GEOM_SPLIT_RIGHT.w * G2_TEXT_DEFAULT_COLS) / G2_GEOM_LARGE.w;

// Indices we keep so taps know what idx → which directory entry. -1 = not a
// real entry (back / blank); -2 = parent row ".. (up)"; -3 = "<< Prev page";
// -4 = "Next page >>".
EXT_RAM_BSS_ATTR static int  gFilesEntryForRow[FILES_MAX_ROWS];

// File-info overlay duration. The deadline lives in the centralized
// g2LensState (see G2_Glasses.h G2LensState). When the deadline
// expires, the overlay-expired callback below fires and we redraw the
// file list.
static const uint32_t kFileInfoOverlayMs = 2200;

// File-action chooser state. When the user taps a file we show the
// chooser for its kind (filesChooserKindFor): view rows for previewable
// types, then Info / Rename / Delete. Active flag gates the tap
// dispatcher; the cached entry lets View/Info reuse the entry the user
// picked without a re-lookup.
static bool      gFilesChooserActive = false;
static FileEntry gFilesChooserEntry  = {};
enum FilesChooserKind : uint8_t {
  FILE_CHOOSER_BMP  = 0,
  FILE_CHOOSER_JSON = 1,
  FILE_CHOOSER_JPG  = 2,  // same chooser shape as BMP, but View/View
                          //  Full dispatch through g2ShowJpgFile path
                          //  (decode JPEG → 4-bpp BMP, then same wire
                          //   transport as the BMP viewers).
  FILE_CHOOSER_TEXT = 3,  // text (.txt, .csv, .log, .md, ...) — one raw
                          //  paged view + Info (no pretty-parse). Shares the
                          //  JSON viewer's engine (readTextLimited → wrap →
                          //  TextPager).
  FILE_CHOOSER_OTHER = 4, // any other file type — no viewer, but Info /
                          //  Rename / Delete must still be reachable (the old
                          //  direct-to-info-overlay path made mutation
                          //  impossible for most file types).
};
static FilesChooserKind gFilesChooserKind = FILE_CHOOSER_BMP;

// Info rows are on the lens: set when the lens accepted them, cleared when a
// list render is accepted. The overlay clock only schedules the list redraw;
// until that list is up (or if it is refused) the info rows keep their
// meaning — "<- Files" returns to the list, the rest are read-only.
static bool gFilesInfoActive = false;

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

// Capacity calls use VFS's shared, opt-in presentation snapshots.
// LittleFS.usedBytes() walks the filesystem, so doing it on every quick folder
// / chooser redraw makes the explorer sluggish. More importantly, Files shares
// the physical G2 widget with Rename's keyboard: there must be no background
// timer that can repaint this page while TextEntry owns it. Re-entry forces a
// fresh sample; browsing reuses one for at most 10 seconds, SD lifecycle changes
// are invalidated centrally by VFS, and successful mutations invalidate both
// displayed tiers immediately.
struct FilesStorageSnapshot {
  bool     sdMounted;
  bool     internalOk;
  bool     sdOk;
  uint64_t internalTotal;
  uint64_t internalFree;
  uint64_t sdTotal;
  uint64_t sdFree;
};
static constexpr uint32_t kFilesStorageTtlMs = 10000;

// Pending redraws. The rename/delete completion, the overlay expiry and the
// image viewers' onDone run on tasks that must not draw or wait on the Files
// page lock; they only write this state and queue a Redraw that the lens
// applier runs. A result or list request is consumed only while its menuGen is
// still current. `rescan` survives a dropped result: the next list render
// re-reads the directory and capacity after any successful mutation.
static portMUX_TYPE gFilesRedrawMux = portMUX_INITIALIZER_UNLOCKED;
struct FilesRedrawState {
  bool     resultReady;
  bool     rescan;
  bool     listPending;
  uint32_t resultMenuGen;
  uint32_t listMenuGen;
  char     resultMessage[FILES_ROW_LEN];  // shown in the list's back row
};
EXT_RAM_BSS_ATTR static FilesRedrawState gFilesRedraw;

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
static void showFilesList(const char* backResult);
static void filesQueueListRedraw();
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

// Compact binary-unit formatter for the narrow right pane. One decimal is
// retained while it is useful ("2.6M"); large / exact values stay shorter
// ("128M"). This is display-only — capacity math remains full uint64_t.
static void formatStorageAmount(uint64_t bytes, char* out, size_t cap) {
  if (!out || cap == 0) return;

  uint64_t divisor = 1;
  char unit = 'B';
  if (bytes >= (1ULL << 40)) {
    divisor = 1ULL << 40;
    unit = 'T';
  } else if (bytes >= (1ULL << 30)) {
    divisor = 1ULL << 30;
    unit = 'G';
  } else if (bytes >= (1ULL << 20)) {
    divisor = 1ULL << 20;
    unit = 'M';
  } else if (bytes >= (1ULL << 10)) {
    divisor = 1ULL << 10;
    unit = 'K';
  }

  if (divisor == 1) {
    snprintf(out, cap, "%lluB", (unsigned long long)bytes);
    return;
  }

  uint64_t whole = bytes / divisor;
  uint64_t tenth = ((bytes % divisor) * 10ULL + divisor / 2ULL) / divisor;
  if (tenth == 10) {
    whole++;
    tenth = 0;
  }
  // With the unit ladder above, even UINT64_MAX is only ~16.7 million T.
  // Narrowing here gives snprintf an honest bounded type and avoids a false
  // truncation warning based on an impossible 17-digit post-scaling value.
  const uint32_t displayWhole = (uint32_t)whole;
  if (whole >= 100 || tenth == 0) {
    snprintf(out, cap, "%lu%c", (unsigned long)displayWhole, unit);
  } else {
    snprintf(out, cap, "%lu.%u%c", (unsigned long)displayWhole,
             (unsigned)tenth, unit);
  }
}

// Six cells fit alongside "Internal" and a human-readable free amount in the
// 288 px right column. Follow the conventional capacity-bar vocabulary: '#' is used
// space and '.' is free space. The heading and numeric amount still report
// free capacity, so a volume with 8.7 MB free no longer looks nearly full.
static void formatStorageUsageBar(uint64_t total, uint64_t freeBytes,
                                  char out[9]) {
  static constexpr size_t kCells = 6;
  uint64_t filled = 0;
  if (total > 0) {
    if (freeBytes > total) freeBytes = total;
    const uint64_t usedBytes = total - freeBytes;
    filled = (usedBytes * kCells + total / 2ULL) / total;
    if (filled > kCells) filled = kCells;
  }

  out[0] = '[';
  for (size_t i = 0; i < kCells; i++) {
    out[i + 1] = (i < filled) ? '#' : '.';
  }
  out[kCells + 1] = ']';
  out[kCells + 2] = '\0';
}

static void invalidateFilesStorageSnapshot() {
  // The old G2-local cache stored one composite sample, so a mutation always
  // invalidated both rows. Preserve that exact redraw behavior while moving the
  // storage and generation fencing into VFS's shared per-tier cache.
  VFS::invalidateStatsSnapshot(VFS::INTERNAL);
  VFS::invalidateStatsSnapshot(VFS::SDCARD);
}

static FilesStorageSnapshot getFilesStorageSnapshot(bool force) {
  FilesStorageSnapshot next = {};
  const uint32_t maxAgeMs = force ? 0 : kFilesStorageTtlMs;

  VFS::CapacitySnapshot internal = {};
  next.internalOk = VFS::getStatsSnapshot(
      VFS::INTERNAL, internal, maxAgeMs) && internal.totalBytes > 0;
  next.internalTotal = internal.totalBytes;
  next.internalFree = internal.freeBytes;

  // Query SD even when unmounted. An unavailable or unreadable tier returns
  // false on a fresh read/cache miss, leaving the local zero snapshot untouched.
  VFS::CapacitySnapshot sd = {};
  const bool sdStatsOk =
      VFS::getStatsSnapshot(VFS::SDCARD, sd, maxAgeMs);
  next.sdMounted = sdStatsOk;
  next.sdOk = sdStatsOk && sd.totalBytes > 0;
  next.sdTotal = sd.totalBytes;
  next.sdFree = sd.freeBytes;
  return next;
}

// Build the complete non-interactive right pane in one TextObject. Keeping
// storage rows and the path together avoids adding another compound child,
// preserves the list's row indices / hit testing, and keeps the worst current
// page within the 1 KB CREATE protobuf ceiling.
static void buildFilesSidebar(const char* path, bool forceStorageRefresh) {
  truncateInto(gFilesPathTitleBuf, sizeof(gFilesPathTitleBuf),
               path ? path : "/", FILES_PATH_TITLE_LEN - 1);

  // Hard-wrap the location at the right-column budget so a long VFS path
  // uses the 288px pane instead of firmware-wrapping short of its edge.
  char wrappedPath[FILES_PATH_TITLE_LEN + 8];
  textWrapInto(wrappedPath, sizeof(wrappedPath), gFilesPathTitleBuf,
               kFilesSidebarCols, /*contIndent=*/0, /*stripCtrl=*/true,
               nullptr);

  const FilesStorageSnapshot storage =
      getFilesStorageSnapshot(forceStorageRefresh);
  const bool intOk = storage.internalOk;
  const bool sdOk = storage.sdOk;

  char intAmount[16] = {};
  char intBar[9] = {};
  if (intOk) {
    formatStorageAmount(storage.internalFree,
                        intAmount, sizeof(intAmount));
    formatStorageUsageBar(storage.internalTotal,
                          storage.internalFree, intBar);
  }

  char sdAmount[16] = {};
  char sdBar[9] = {};
  if (sdOk) {
    formatStorageAmount(storage.sdFree, sdAmount, sizeof(sdAmount));
    formatStorageUsageBar(storage.sdTotal, storage.sdFree, sdBar);
  }

  if (intOk && sdOk) {
    snprintf(gFilesSidebarBuf, sizeof(gFilesSidebarBuf),
             "Free space\nInternal %s %s\nSD %s %s\nLocation\n%s",
             intBar, intAmount, sdBar, sdAmount, wrappedPath);
  } else if (intOk) {
    snprintf(gFilesSidebarBuf, sizeof(gFilesSidebarBuf),
             "Free space\nInternal %s %s\nLocation\n%s",
             intBar, intAmount, wrappedPath);
  } else if (sdOk) {
    snprintf(gFilesSidebarBuf, sizeof(gFilesSidebarBuf),
             "Free space\nInternal unavailable\nSD %s %s\nLocation\n%s",
             sdBar, sdAmount, wrappedPath);
  } else {
    snprintf(gFilesSidebarBuf, sizeof(gFilesSidebarBuf),
             "Free space\nInternal unavailable\nLocation\n%s",
             wrappedPath);
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
      // Reserve "/ " (2) + " " (1) + badge + NUL (1) for the name column.
      const int reserve = 4 + (int)strlen(badge);
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

// Chooser kind for a tapped file, from the classifier the OLED browser shares
// (FileBrowser_FileKind.h). BMP and JPEG get View / View Full (JPEG is decoded
// to a 4-bpp BMP before the same wire transport), JSON gets Pretty / JSON
// View, text (.txt, .csv, .log, .md, ...) gets the raw paged view: a quick peek
// whose rows soft-wrap at the lens width (no aligned-table mode; the web UI has
// that). Everything else gets Info / Rename / Delete only.
static FilesChooserKind filesChooserKindFor(const char* name) {
  switch (fileKindForName(name)) {
    case FileKind::ImageBmp:  return FILE_CHOOSER_BMP;
    case FileKind::ImageJpeg: return FILE_CHOOSER_JPG;
    case FileKind::Json:      return FILE_CHOOSER_JSON;
    case FileKind::Text:      return FILE_CHOOSER_TEXT;
    case FileKind::Other:     break;
  }
  return FILE_CHOOSER_OTHER;
}

// Absolute VFS path of the chooser entry. The file tap selected the entry in
// FileManager (as folder taps do for navigateInto), and nothing moves the
// selection while the chooser is up; FileManager resolves the selection's
// full path (FileEntry.name is a display copy cut at 63 bytes). Returns false,
// with `out` empty, when the entry cannot be resolved or its path does not fit
// `cap`; callers then refuse the action.
static bool buildChooserPath(char* out, size_t cap) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  FileManager* fm = ensureFm();
  if (!fm) return false;
  String path;
  if (!fm->getSelectedPath(path) || path.length() >= cap) return false;
  memcpy(out, path.c_str(), path.length() + 1);
  return true;
}

// ---------------------------------------------------------------------------
// Rename / Delete flows. Both dispatch the REAL commands through cmd_exec
// (auth + [CMD] audit as the paired user; the OLED browser submits the same
// user-level commands): `filerename "<old>" "<new>"` and the
// ONE-SHOT `filedelete "<path>" confirm` (the bare form arms an interactive
// CLI confirm no tap UI can answer). The handlers apply the per-path policy
// through the guarded VFS (System_Filesystem.cpp), so /system config stays
// protected; their refusal shows in the result row.
// ---------------------------------------------------------------------------

// Lens applier: draw the list with the published result in its back row.
// The result is consumed only if its submit menuGen is still current.
static void renderFilesResult() {
  FilesPageLock lock;
  if (g2GetHijackPage() != G2_HIJACK_PAGE_FILES) return;
  char message[FILES_ROW_LEN];
  bool ready = false;
  portENTER_CRITICAL(&gFilesRedrawMux);
  if (gFilesRedraw.resultReady &&
      gFilesRedraw.resultMenuGen == g2CurrentMenuGen()) {
    memcpy(message, gFilesRedraw.resultMessage, sizeof(message));
    gFilesRedraw.resultReady = false;
    ready = true;
  }
  portEXIT_CRITICAL(&gFilesRedrawMux);
  if (ready) showFilesList(message);
}

// Completion on cmd_exec_task: copy the result and queue a redraw with the
// submit cookie (G2HijackCmd.h contract — no lens, FileManager or Files page
// lock here). The lens applier drops the redraw if the user navigated since
// the submit. Success marks a forced re-scan for the next list render —
// FileManager::refresh() is generation-gated and would keep showing the
// deleted/old name.
static void onFilesMutateDone(bool ok, const char* result,
                              const G2CmdCookie& cookie, void* /*userData*/) {
  // executeCommand reports ok=true even for handler "Error: ..." strings —
  // judge by the result prefix, not the flag.
  const bool success = ok && result && strncmp(result, "OK", 2) == 0;
  const char* msg = (result && result[0]) ? result : (ok ? "Done" : "Failed");
  if (strncmp(msg, "OK: ", 4) == 0) msg += 4;  // back-row brevity
  char copy[FILES_ROW_LEN];
  snprintf(copy, sizeof(copy), "%s", msg);
  DEBUG_G2F("[G2] Files: mutate done ok=%d success=%d '%s'",
            (int)ok, (int)success, msg);

  bool published = false;
  portENTER_CRITICAL(&gFilesRedrawMux);
  if (success) gFilesRedraw.rescan = true;
  if (g2GetHijackPage() == G2_HIJACK_PAGE_FILES &&
      cookie.menuGen == g2CurrentMenuGen()) {
    gFilesRedraw.resultMenuGen = cookie.menuGen;
    memcpy(gFilesRedraw.resultMessage, copy, sizeof(copy));
    gFilesRedraw.resultReady = true;
    published = true;
  }
  portEXIT_CRITICAL(&gFilesRedrawMux);
  if (!published) return;  // user navigated away
  if (!g2EnqueueRedraw(cookie, renderFilesResult, "Files result")) {
    portENTER_CRITICAL(&gFilesRedrawMux);
    if (gFilesRedraw.resultMenuGen == cookie.menuGen)
      gFilesRedraw.resultReady = false;
    portEXIT_CRITICAL(&gFilesRedrawMux);
  }
}

static void filesSubmitMutate(const char* line) {
  G2CmdCookie cookie{};
  cookie.menuGen    = g2CurrentMenuGen();
  cookie.targetPage = g2GetHijackPage();
  if (!g2SubmitHijackCommand(line, cookie, onFilesMutateDone, nullptr)) {
    DEBUG_G2F("[G2] Files: mutate submit FAILED — no inline fallback");
    g2ShowFilesMenu();
  }
}

// Keyboard callbacks run on g2_tap_disp — String build + submit only; the
// list redraw on cancel is cache-hot, no FS scan.
static void filesRenameCommit(const char* text) {
  FilesPageLock lock;
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
// "Delete <name>?" header, explicit Confirm row. Called while the chooser is
// up; the confirm replaces it only once the lens accepted the confirm rows,
// so a refused render leaves the chooser rows with their chooser meaning.
static void showDeleteConfirm() {
  snprintf(gFilesConfirmRow, sizeof(gFilesConfirmRow), "Delete %.56s?", gFilesActionName);
  const char* rows[] = { "<- Cancel", gFilesConfirmRow, "Confirm Delete" };
  const bool shown = g2ShowListPageAndPublish(rows, 3, [] {
    gFilesChooserActive = false;
    gFilesConfirmActive = true;
  });
  g2LensClearOverlay();
  if (!shown) {
    DEBUG_G2F("[G2] Files: delete confirm not shown — chooser stays");
    return;
  }
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
  static char title[FILES_ROW_LEN];
  snprintf(title, sizeof(title), "%s", e.name);

  // Build rows + the action table in lockstep (see FilesAction). The table,
  // entry and kind become the chooser's only once the lens accepted the rows.
  static const char* rows[FILES_CHOOSER_MAX_ROWS];
  FilesAction acts[FILES_CHOOSER_MAX_ROWS];
  uint8_t rowN = 0;
  auto add = [&](const char* label, FilesAction act) {
    if (rowN < FILES_CHOOSER_MAX_ROWS) {
      rows[rowN] = label;
      acts[rowN] = act;
      rowN++;
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
  const bool shown = g2ShowListPageAndPublish(rows, rowN, [&] {
    gFilesChooserEntry  = e;
    gFilesChooserKind   = kind;
    memcpy(gFilesChooserAct, acts, rowN * sizeof(acts[0]));
    gFilesChooserRowN   = rowN;
    gFilesChooserActive = true;
  });

  // No overlay — chooser is a deliberate stop, not a flash.
  g2LensClearOverlay();
  if (!shown) {
    DEBUG_G2F("[G2] Files: chooser for '%s' not shown", e.name);
    return;
  }
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
  FilesPageLock lock;
  const int oldPage = gFilesPager.curPage;
  textNavPage(gFilesPager, kind != G2_TAP_PAGE_PREV);
  if (!filesRenderTextPage()) gFilesPager.curPage = oldPage;
}

static void exitTextViewBackToFiles() {
  FilesPageLock lock;
  gFilesPager.pageCount = 0;
  gFilesPager.curPage   = 0;
  gFilesPager.truncated = false;
  gFilesTextTitle[0]    = '\0';
  g2ShowFilesMenu();
}

// G2-only checked reader. The shared String reader remains unchanged for CLI
// fileview and the tiny Info sniff. The caller has installed the paired user's
// identity and checked canRead; like readTextLimited this helper is policy-free
// and serializes its whole VFS read with the filesystem lock.
static bool filesReadTextBuffer(const char* path, PsramBuffer& out,
                                 size_t maxBytes) {
  out.clear();
  FsLockGuard guard("g2.files.text");
  File f = VFS::open(String(path), "r");
  if (!f) return false;
  // Avoid reserving a full 12 KiB for a tiny file. File writes using the same
  // FS lock cannot grow it during this read; retain the caller's hard cap.
  size_t toRead = f.size();
  if (toRead > maxBytes) toRead = maxBytes;
  if (toRead == SIZE_MAX || !out.reserve(toRead + 1)) return false;
  size_t total = 0;
  while (total < toRead) {
    size_t chunk = toRead - total;
    if (chunk > 512) chunk = 512;
    const size_t n = f.readBytes(out.data() + total, chunk);
    if (!n) break;
    total += n;
  }
  f.close();
  return out.setSize(total);
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
  if (!buildChooserPath(path, sizeof(path))) {
    DEBUG_G2F("[G2] Files text: chooser path unavailable");
    return false;
  }

  if (!canRead(String(path), currentAuthContext())) {
    DEBUG_G2F("[G2] Files text: blocked by canRead '%s'", path);
    return g2ShowTextPage("Viewer blocked: read permission denied.", G2_GEOM_LARGE,
                          exitTextViewBackToFiles, nullptr);
  }

  PsramBuffer raw(SIZE_MAX, "g2.files.input");
  if (!filesReadTextBuffer(path, raw, kFilesTextReadCapBytes)) {
    DEBUG_G2F("[G2] Files text: read failed '%s'", path);
    return false;
  }
  const bool hitReadCap = raw.length() >= kFilesTextReadCapBytes;

  // Sealed capture? Reveal for the lens — this runs under the G2-paired
  // user's identity (ctxGuard above) and behind the canRead gate; the
  // '#HW1ENC' first line stays visible as the mark.
  const size_t revealCapacity = captureCryptoRevealCapacity(raw.c_str(), raw.size());
  size_t revealedLength = raw.size();
  if (!revealCapacity || !raw.reserve(revealCapacity) ||
      !captureCryptoRevealText(raw.data(), revealedLength, raw.capacity()) ||
      !raw.setSize(revealedLength)) {
    DEBUG_G2F("[G2] Files text: reveal buffer failed '%s'", path);
    return false;
  }

  // Serialize directly into the existing PSRAM body. The stateful sink keeps
  // wrap columns across JSON serializer writes; no display String is needed.
  TextWrapStream display(gFilesTextBody, sizeof(gFilesTextBody),
                         G2_TEXT_DEFAULT_COLS, /*contIndent=*/0,
                         /*stripControl=*/true);
  if (pretty) {
    PSRAM_JSON_DOC(doc);
    // Match ArduinoJson's String reader, which is length-bounded (including
    // embedded NUL bytes); presentation wrapping still stops at the first NUL.
    DeserializationError err = deserializeJson(doc, raw.c_str(), raw.size());
    if (err) {
      display.write("// Pretty parse failed: ");
      display.write(err.c_str());
      display.write("\n// Falling back to raw JSON text\n\n");
      display.write(raw.c_str());
      DEBUG_G2F("[G2] Files text: pretty parse failed '%s' (%s)",
                path, err.c_str());
    } else {
      serializeJsonPretty(doc, display);
    }
  } else {
    display.write(raw.c_str());
  }

  // Wrap into the shared body buffer: strips '\r' + control bytes, soft-wraps
  // long lines at the lens column width (so wide CSV rows stay legible), and
  // flags truncation when the source outgrows the displayable body.
  const bool wrapTrunc = display.truncated();
  const size_t bodyLen = display.size();
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
            pretty ? "pretty" : "raw", path, (unsigned)display.sourceBytes(),
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
  // Without a resolvable path the sniff is skipped; the rows still show.
  bool sealedFile = false;
  if (!e.isFolder) {
    G2HijackCtxGuard ctxGuard;
    char p[FILE_MANAGER_MAX_PATH + 32];
    String head;
    if (buildChooserPath(p, sizeof(p)) && canRead(String(p), currentAuthContext()) &&
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
  // Deliberately do NOT change the hijack page — leave it as FILES so a
  // tap on idx=0 routes through g2FilesHandleTap and goes back to the
  // file list. The lens overlay tracker handles auto-dismiss; the
  // expired callback (filesOverlayExpired) queues the file-list redraw. The
  // info mode and overlay replace the chooser only once the lens accepted
  // these rows.
  if (!g2ShowListPageAndPublish(rows, 4, [] {
        gFilesChooserActive = false;
        gFilesInfoActive    = true;
        g2LensStartOverlay(G2_OVERLAY_FILE_INFO, kFileInfoOverlayMs);
      })) {
    DEBUG_G2F("[G2] Files: file-info overlay for '%s' not shown — chooser stays",
              e.name);
    return;
  }
  DEBUG_G2F("[G2] Files: file-info overlay for '%s' (%u B, %s)",
            e.name, (unsigned)sz, ext);
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void g2BuildFilesInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  FilesPageLock lock;  // g2files CLI on cmd_exec_task shares gFilesFm

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

// The directory list; `backResult` (a rename/delete result) replaces the
// back-row label for this render only. Callers hold the Files page lock.
static void showFilesList(const char* backResult) {
  // Run all FS work under the paired-by user's identity. Without this guard
  // the task's TLS identity stays at ANON (the safe default) and
  // FileManager::navigate() denies every read.
  G2HijackCtxGuard ctxGuard;
  const bool enteringFiles =
      g2GetHijackPage() != G2_HIJACK_PAGE_FILES;
  portENTER_CRITICAL(&gFilesRedrawMux);
  const bool mutated = gFilesRedraw.rescan;
  gFilesRedraw.rescan = false;
  portEXIT_CRITICAL(&gFilesRedrawMux);

  // Directory cache policy for the per-boot FileManager singleton:
  //
  //   • Re-entry from outside Files (Apps → Files, post safety-timeout,
  //     Main Menu → … → Files): forceRescan(). refresh() is identity-
  //     generation gated and would keep showing stale sizes / missing
  //     files written by sensorlog / web / CLI while the user was away.
  //     Selection is preserved by loadDirectory().
  //
  //   • After a successful lens rename/delete: forceRescan() too, and the
  //     capacity snapshots are invalidated.
  //
  //   • Otherwise already on Files (overlay dismiss, viewer callback, folder
  //     navigate which already loaded): refresh() only. That covers the
  //     ANON-first-tap → later-paired empty-cache case without paying
  //     the full VFS permission walk on every in-page redraw.
  if (FileManager* fmRefresh = ensureFm()) {
    if (enteringFiles) {
      fmRefresh->forceRescan();
      gFilesPage = 0;  // re-entry from outside → start at the first page
    } else if (mutated) {
      fmRefresh->forceRescan();
    } else {
      fmRefresh->refresh();
    }
  }
  if (mutated) invalidateFilesStorageSnapshot();

  // Whenever the real file list comes back up, the chooser and the
  // delete-confirm are unconditionally gone — covers the View/Info → Back
  // path, the post-BMP-dismiss callback, and any "redraw Files from main
  // menu" entry, all without each call site having to remember.
  gFilesChooserActive = false;
  gFilesConfirmActive = false;
  size_t n = buildRows();
  if (backResult && backResult[0]) {
    snprintf(gFilesRows[0], FILES_ROW_LEN, "<- %.*s",
             (int)(FILES_ROW_LEN - 4), backResult);
  }
  const char* path = "/";
  FileManager* fmMenu = ensureFm();
  if (fmMenu) path = fmMenu->getCurrentPath();
  buildFilesSidebar(path, enteringFiles);

  const G2TextChildSpec sidebar = {
      "filesSidebar", gFilesSidebarBuf, 99, kFilesSidebarGeom, false };

  if (g2ShowMixedListText(gFilesRowPtrs, n, kFilesListGeom, sidebar)) {
    // The info rows are gone and a queued list redraw is now redundant.
    gFilesInfoActive = false;
    portENTER_CRITICAL(&gFilesRedrawMux);
    gFilesRedraw.listPending = false;
    portEXIT_CRITICAL(&gFilesRedrawMux);
    g2SetHijackPage(G2_HIJACK_PAGE_FILES);
    DEBUG_G2F("[G2] Files menu shown (list+storage/path, rows=%u)",
              (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Files menu show FAILED");
  }
  // Cancel any in-flight overlay since we explicitly reshowed the list.
  g2LensClearOverlay();
}

void g2ShowFilesMenu() {
  FilesPageLock lock;
  showFilesList(nullptr);
}

// Lens applier: redraw the list for a pending overlay-expiry / viewer-return
// request. Its menuGen is rechecked under the lock: a tap that opened the
// rename keyboard while this job waited for the lock has bumped it.
static void renderFilesListIfCurrent() {
  FilesPageLock lock;
  if (g2GetHijackPage() != G2_HIJACK_PAGE_FILES) return;
  bool pending = false;
  portENTER_CRITICAL(&gFilesRedrawMux);
  if (gFilesRedraw.listPending &&
      gFilesRedraw.listMenuGen == g2CurrentMenuGen()) {
    gFilesRedraw.listPending = false;
    pending = true;
  }
  portEXIT_CRITICAL(&gFilesRedrawMux);
  if (pending) showFilesList(nullptr);
}

// Overlay expiry (main loop) and the image viewers' onDone (session worker)
// queue the list redraw on the lens applier instead of drawing on their own
// task. The current menuGen fences it: navigation before it runs drops it.
static void filesQueueListRedraw() {
  G2CmdCookie cookie{};
  cookie.menuGen    = g2CurrentMenuGen();
  cookie.targetPage = G2_HIJACK_PAGE_FILES;
  portENTER_CRITICAL(&gFilesRedrawMux);
  gFilesRedraw.listPending = true;
  gFilesRedraw.listMenuGen = cookie.menuGen;
  portEXIT_CRITICAL(&gFilesRedrawMux);
  if (!g2EnqueueRedraw(cookie, renderFilesListIfCurrent, "Files list")) {
    DEBUG_G2F("[G2] Files: list redraw not queued");
  }
}

void g2FilesHandleTap(uint32_t idx) {
  FilesPageLock lock;
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
          // directly; JPG decodes via the shared JPEG HAL then shares the transport.
          char path[FILE_MANAGER_MAX_PATH + 32];
          const bool resolved = buildChooserPath(path, sizeof(path));
          const bool isJpg = (gFilesChooserKind == FILE_CHOOSER_JPG);
          gFilesChooserActive = false;
          DEBUG_G2F("[G2] Files: chooser View -> '%s' (%s)", path, isJpg ? "JPG" : "BMP");
          const bool ok = resolved &&
                          (isJpg ? g2ShowJpgFile(path, &filesQueueListRedraw)
                                 : g2ShowBmpFile(path, &filesQueueListRedraw));
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
        const bool resolved = buildChooserPath(path, sizeof(path));
        const bool isJpg = (gFilesChooserKind == FILE_CHOOSER_JPG);
        gFilesChooserActive = false;
        DEBUG_G2F("[G2] Files: chooser View Full -> '%s' (%s)", path, isJpg ? "JPG" : "BMP");
        const bool ok = resolved &&
                        (isJpg ? g2ShowJpgFileFullScreen(path, &filesQueueListRedraw)
                               : g2ShowBmpFileFullScreen(path, &filesQueueListRedraw));
        if (!ok) g2ShowFilesMenu();
        return;
      }

      case FACT_JSON_RAW:
        gFilesChooserActive = false;
        DEBUG_G2F("[G2] Files: chooser JSON View -> '%s'", cached.name);
        if (!showTextFileViaWidget(/*pretty=*/false)) g2ShowFilesMenu();
        return;

      case FACT_INFO:
        DEBUG_G2F("[G2] Files: chooser Info → '%s'", cached.name);
        showFileInfo(cached);   // closes the chooser once the info rows are shown
        return;

      case FACT_RENAME: {
        // Snapshot the target NOW — the chooser slot is cleared by list
        // redraws and the keyboard doesn't block the scan worker.
        const bool resolved = buildChooserPath(gFilesActionPath, sizeof(gFilesActionPath));
        strncpy(gFilesActionName, cached.name, sizeof(gFilesActionName) - 1);
        gFilesActionName[sizeof(gFilesActionName) - 1] = '\0';
        gFilesChooserActive = false;
        if (!resolved) {
          g2ShowTextAsList("File path unavailable - rename on web", "<- Back");
          return;
        }
        if (strchr(gFilesActionPath, '"')) {
          // CommandArgs has no escapes — an embedded quote breaks the line.
          g2ShowTextAsList("Name has a quote char - rename on web", "<- Back");
          return;
        }
        if (strlen(gFilesActionName) > 32) {
          // This rename field is capped at 32 and pre-fill truncates — an
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
        strncpy(gFilesActionName, cached.name, sizeof(gFilesActionName) - 1);
        gFilesActionName[sizeof(gFilesActionName) - 1] = '\0';
        if (!buildChooserPath(gFilesActionPath, sizeof(gFilesActionPath))) {
          gFilesChooserActive = false;
          g2ShowTextAsList("File path unavailable - delete on web", "<- Back");
          return;
        }
        if (strchr(gFilesActionPath, '"')) {
          gFilesChooserActive = false;
          g2ShowTextAsList("Name has a quote char - delete on web", "<- Back");
          return;
        }
        showDeleteConfirm();   // closes the chooser once the confirm is shown
        return;
    }
    return;
  }

  if (idx == 0) {
    // Context-sensitive back. While the file-info rows are up (overlay
    // running, or expired with its list redraw not shown yet), "<- Files"
    // returns to the file list rather than the main hijack menu — matches
    // what the user just navigated away from. g2ShowFilesMenu clears the
    // info mode and the overlay state.
    if (gFilesInfoActive) {
      DEBUG_G2F("[G2] Files: back from overlay → file list");
      g2ShowFilesMenu();
      return;
    }
    g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
    extern void g2RedrawHijackMainMenu();
    g2RedrawHijackMainMenu();
    return;
  }

  // While the info rows are up, idx>0 are read-only metadata rows. No-op.
  if (gFilesInfoActive) {
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

  // Select the entry: navigateInto (folders) and the chooser's actions
  // (buildChooserPath) resolve the *selected* entry's full path, which
  // e.name, a bounded display copy, may not hold.
  while (fm->getSelectedIndex() < entryIdx) fm->moveDown();
  while (fm->getSelectedIndex() > entryIdx) fm->moveUp();

  if (e.isFolder) {
    DEBUG_G2F("[G2] Files: navigateInto '%s'", e.name);
    fm->navigateInto();
    gFilesPage = 0;  // fresh directory → fresh page
    g2ShowFilesMenu();
  } else {
    // File tap → the chooser for its kind: view rows for previewable types,
    // and Info / Rename / Delete for every file (FILE_CHOOSER_OTHER has no
    // viewer but keeps those reachable).
    showFileChooser(e, filesChooserKindFor(e.name));
  }
}

// Overlay-expired callback. Registered with the lens-state tracker via
// g2LensSetOverlayExpiredCb at first ensureFm() call. Fires on the main loop
// (g2Tick) when the shared overlay clock crosses our deadline. We only queue
// the list redraw if both (a) the expired overlay was ours, and (b) we're
// still on the Files page (user might have swiped away).
static void filesOverlayExpired(G2OverlayKind kind) {
  if (kind != G2_OVERLAY_FILE_INFO) return;
  if (g2GetHijackPage() != G2_HIJACK_PAGE_FILES) return;
  filesQueueListRedraw();
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
