// OLED_Mode_FileBrowser.cpp - File browser display mode
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "OLED_Utils.h"             // oledAuthContext() — shared OLED identity builder
#include "OLED_UI.h"                // oledToastShow() — transfer confirmations (thread-safe, self-dirties)
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include <esp_attr.h>               // EXT_RAM_BSS_ATTR — PSRAM viewer buffer
#include <cstring>                  // strrchr — extension detection
#include "HAL_Input.h"
#include "System_FileManager.h"
#include "System_Icons.h"
#include "System_TextPager.h"       // textWrapInto — shared wrap/sanitize core
#include "System_AuthIdentity.h"    // CommandIdentityScope — composed identity + notif install
#include "System_User.h"            // AuthContext, SOURCE_LOCAL_DISPLAY
#include "System_Settings.h"        // gSettings (for displayRequireAuth via globals)

// OLED login state — set by OLED_Mode_Auth on successful login. The FileManager
// runs under whoever is in the OLED render task's TLS identity slot at call
// time, so the file browser MUST install one before touching VFS or every
// guarded read denies. See prepareFileBrowserData below for the install site
// and the G2_Page_Files counterpart (g2ShowFilesMenu) for the same pattern
// on the lens side.
extern String gLocalDisplayUser;
extern bool   gLocalDisplayAuthed;

#if ENABLE_ESPNOW
#include "System_ESPNow_Wire.h"     // V4PayloadFsListReplyHeader, V4PayloadFsEntry, FsListStatus
#include "System_ESPNow_FsList.h"   // fsListSendRequest / fsGetSendRequest / fsListCancel
#include "System_BondedPeer.h"      // BondedPeer::isPaired, peerMacBytes
#include "System_MemUtil.h"         // ps_alloc / ps_free for the peer entry cache
#include <new>                       // placement-new: gOledFileManager lives in PSRAM
#include "System_CommandTypes.h"    // ExecReq::DeferredFn (submitDeferredToCmdExec)
// Externs into System_ESPNow.cpp (avoid including the heavy System_ESPNow.h here,
// which redeclares drawIcon and clashes with this file's local extern).
//  - sendFileToMac: pump a local file to a peer (multi-second).
//  - submitDeferredToCmdExec: run a job on cmd_exec_task so the send never blocks
//    the OLED render/input task.
extern bool sendFileToMac(const uint8_t* mac, const String& localPath);
extern bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg);
#endif

// Thin wrapper over the shared oledAuthContext() builder — names the file-
// browser-specific path so the OLEDFileBrowserCtxGuard below reads cleanly
// and audit lines / [PERM] denials show "/oled/files" rather than the
// generic "/oled". Identity body and AuthBypass behavior are the shared
// helper's job; see OLED_Utils.h::oledAuthContext for the rationale.
static AuthContext oledFileBrowserAuthContext() {
  return oledAuthContext("/oled/files");
}

// Sugar over CommandIdentityScope for the OLED file browser scope. Parallel
// to G2HijackCtxGuard — same composed-guard pattern, OLED identity instead
// of G2 paired-user identity. Drop one at the top of the file browser's
// data-prepare entry point to install identity + NOTIF_SOURCE_OLED in one
// move.
class OLEDFileBrowserCtxGuard {
 public:
  OLEDFileBrowserCtxGuard() : scope_(oledFileBrowserAuthContext()) {}
  ~OLEDFileBrowserCtxGuard() = default;
  OLEDFileBrowserCtxGuard(const OLEDFileBrowserCtxGuard&)            = delete;
  OLEDFileBrowserCtxGuard& operator=(const OLEDFileBrowserCtxGuard&) = delete;
  OLEDFileBrowserCtxGuard(OLEDFileBrowserCtxGuard&&)                 = delete;
  OLEDFileBrowserCtxGuard& operator=(OLEDFileBrowserCtxGuard&&)      = delete;
 private:
  CommandIdentityScope scope_;
};

#if ENABLE_GPS_SENSOR
#include "System_Maps.h"
#endif

// External references
extern FileManager* gOledFileManager;
extern bool oledFileBrowserNeedsInit;
extern bool oledMenuBack();

// Icon functions (from System_Icons.cpp)
extern void drawIcon(Adafruit_SSD1306* display, const char* iconName, int x, int y, uint16_t color);
extern const char* getIconNameForExtension(const char* extension);
extern String formatFileSize(size_t bytes);

// ============================================================================
// File Browser State
// ============================================================================

// Pending action enum for deferred filesystem operations. FS I/O and mutations
// may ONLY run in prepareFileBrowserData() (under OLEDFileBrowserCtxGuard);
// input/confirm callbacks set these flags and prepare executes them next frame.
enum class FileBrowserPendingAction {
  NONE,
  NAVIGATE_INTO,
  NAVIGATE_UP,
  NAVIGATE_BACK,
  LOAD_VIEW,        // read the selected file into the text viewer
  DELETE_SELECTED   // delete the selected file (set only by the confirm dialog)
};
static FileBrowserPendingAction fileBrowserPendingAction = FileBrowserPendingAction::NONE;

// ============================================================================
// Per-file action menu + text viewer (LOCAL source only)
// ============================================================================
// Pressing A on a regular file opens an action menu (View / Rename / Delete)
// instead of the old silent no-op. .hwmap still auto-loads into the map, and
// picker mode still fires its callback — both take precedence. PEER (remote)
// files never reach this code: the input handler and renderer branch on
// FsSource::PEER first, and the wire protocol has no read/rename opcode.
enum class FbLevel : uint8_t { LIST = 0, ACTION_MENU = 1, VIEW = 2 };
static FbLevel sFbLevel = FbLevel::LIST;

// Snapshot of the file the action menu / viewer is acting on. FileManager
// mutations (rename/delete/read) all act on the CURRENT SELECTION with no path
// arg, and selection can't move while a level/overlay owns input — so the
// snapshot is only for display + the extension check.
static char     sFbActionName[FILE_MANAGER_MAX_NAME] = {0};
static bool     sFbActionViewable = false;
static bool     sFbRenameActive = false;         // keyboard open for a rename
static char     sFbDeletePrompt[40] = {0};       // outlives the confirm dialog (raw ptr)

// The per-file action menu (View/Rename/Delete/Back) renders through the shared
// OLEDScrollState scrolling system — the same primitive Power/Network/LED use —
// instead of the old fixed 4-row layout, whose last row ("< Back") spilled under
// the global footer with no way to reach it. The filename occupies one header
// line above the list, so this is the Power main menu's "status line + scrollable
// list" shape: the list window is offset one row (OLED_CONTENT_START_Y + 8) and
// visibleLines is sized to the remaining content height.
EXT_RAM_BSS_ATTR static OLEDScrollState sFbActionScroll;
static bool     sFbActionScrollInited = false;

static void fbInitActionScroll() {
  if (sFbActionScrollInited) return;
  // 8px single-line rows; reserve the top line for the filename header.
  const int vis = (OLED_CONTENT_HEIGHT - 8) / 8;
  oledScrollInit(&sFbActionScroll, nullptr, vis > 0 ? vis : 1);
  sFbActionScroll.wrapAround = true;   // preserve the action menu's wrap feel
  sFbActionScrollInited = true;
}

// Rebuild the action rows into the shared scroll state from the single-source
// fbBuildActionRows(), so display / navigation / dispatch can never disagree.
// KeepSelection preserves the cursor across the per-frame rebuild; the string
// literals stored by oledScrollAddItem() have program lifetime.
static int fbBuildActionRows(const char* labels[4]);  // defined below
static void fbPopulateActionScroll() {
  fbInitActionScroll();
  oledScrollClearKeepSelection(&sFbActionScroll);
  const char* rows[4];
  const int n = fbBuildActionRows(rows);
  for (int i = 0; i < n; i++) oledScrollAddItem(&sFbActionScroll, rows[i]);
  oledScrollClampSelection(&sFbActionScroll);
}

// Text viewer: whole (capped) file wrapped at OLED width into a PSRAM buffer,
// scrolled a line at a time (the OLED_Mode_CLI cliScrollOffset model). Reads
// go through FileManager::readFile which pulls the WHOLE file byte-at-a-time
// into a String (hot-path fragmentation offender), so refuse large files and
// point the user at serial `fileview` (paged) instead.
static constexpr size_t   FB_VIEW_MAX_FILE = 10240;   // refuse-above (entry.size)
static constexpr int      FB_VIEW_COLS     = 20;       // 21-char rows at textSize 1
static EXT_RAM_BSS_ATTR char sFbViewBody[12288];       // wrapped body (grows vs raw)
EXT_RAM_BSS_ATTR static uint16_t           sFbViewLineOff[640];         // start offset per line
static int                sFbViewLineCount = 0;
static int                sFbViewScroll = 0;
static bool               sFbViewTruncated = false;

// Exposed to the central footer-hint switch (OLED_Utils.cpp) so it can show the
// right hints per level without reaching into these statics.
int oledFileBrowserActiveLevel() { return (int)sFbLevel; }

static int fbViewVisibleLines() {
  const int v = OLED_CONTENT_HEIGHT / 10;
  return v > 0 ? v : 1;
}

// Text-ish extensions the viewer offers "View" for. Everything else gets only
// Rename/Delete (a binary/image on a mono OLED is not worth rendering; jpg
// lives on G2/web).
static bool fbIsViewableExt(const char* name) {
  const char* dot = strrchr(name, '.');
  if (!dot) return false;
  String e(dot);
  e.toLowerCase();
  return e == ".txt" || e == ".csv" || e == ".json" || e == ".log" ||
         e == ".md"  || e == ".ini" || e == ".cfg"  || e == ".conf" ||
         e == ".yaml"|| e == ".yml" || e == ".xml"  || e == ".html" ||
         e == ".js"  || e == ".c"   || e == ".h"    || e == ".cpp";
}

// The action-menu rows, built in one place so render / count / dispatch agree.
// Returns the row count; fills labels[] (cap 4).
static int fbBuildActionRows(const char* labels[4]) {
  int n = 0;
  if (sFbActionViewable) labels[n++] = "View";
  labels[n++] = "Rename";
  labels[n++] = "Delete";
  labels[n++] = "< Back";
  return n;
}

// Pre-rendered file browser data to avoid filesystem I/O inside I2C transaction
// (struct defined in System_FileManager.h)
FileBrowserRenderData fileBrowserRenderData = {0};  // Non-static so footer can access it

// Button/navigation state
static unsigned long oledFileBrowserLastInput = 0;
static const unsigned long OLED_FILE_BROWSER_DEBOUNCE = 200; // ms

// ============================================================================
// File Picker State (modal callback layer — see OLED_Display.h)
// ============================================================================
// Stored as a single static request slot — only one picker can be active at
// a time. Push transitions: viewer (nothing pending) → push() → active until
// callback fires (file pick) OR cancel fires (back at root).
static FilePickerRequest sPickerReq = {};
static bool sPickerActive = false;

bool oledFilePickerIsActive() { return sPickerActive; }

// Title accessor used by OLED_Utils.cpp's breadcrumb override.
const char* oledFilePickerTitle() {
  return sPickerActive ? sPickerReq.title : nullptr;
}

bool oledFilePickerPush(const FilePickerRequest& req) {
  if (sPickerActive) return false;  // already pending; caller must wait
  sPickerReq = req;
  sPickerActive = true;
  // Force the next prepareFileBrowserData to re-init under the new request:
  //   - navigate to startPath (instead of "/")
  //   - apply the visibility filter
  oledFileBrowserNeedsInit = true;
  return true;
}

// Resolve the picker — fire callback exactly once, clear state, pop mode.
// Called from inside prepareFileBrowserData's pending-action processing,
// which is the safe context (not inside an I2C transaction). After this
// returns, currentOLEDMode is the requester's mode (assuming oledMenuBack
// successfully popped) and the FileBrowser will not render again under
// the consumed picker.
static void firePickerCallback(const char* fullPath, bool cancelled) {
  // Snapshot before any mutation — the request slot gets cleared and
  // gOledFileManager's filter is released BEFORE we hand control back to
  // the requester, so the callback can freely re-push another picker.
  void (*onPicked)(const char*, bool) = sPickerReq.onPicked;
  OLEDMode requester = sPickerReq.requesterMode;

  sPickerActive = false;
  sPickerReq = FilePickerRequest{};
  if (gOledFileManager) gOledFileManager->setVisibilityFilter(nullptr);
  fileBrowserPendingAction = FileBrowserPendingAction::NONE;
  fileBrowserRenderData.valid = false;

  // Pop back to the requester mode. After this currentOLEDMode == requester
  // (unless something funky happened to the stack — guarded below).
  oledMenuBack();

  // Lifecycle safety: if the user navigated AWAY from the requester mode
  // mid-picker (e.g. Home button took them elsewhere), don't fire — the
  // requester's locals may be invalidated. Drops the callback silently.
  if (onPicked && currentOLEDMode == requester) {
    onPicked(fullPath, cancelled);
  }
}

// ============================================================================
// Filesystem Source Switcher
// ============================================================================
// The browser supports multiple "sources" — places it can list files from.
//   LOCAL    — full VFS root, includes /sd as a virtual mount
//   SD_QUICK — jumps straight to /sd (saves the user a few presses)
//   PEER     — the bonded peer's filesystem, fetched via the FS_LIST_REQ /
//              FS_LIST_REPLY ESP-NOW opcodes (see System_ESPNow_FsList.h)
//
// X button rotates through sources. Source indicator appears in the header.
//
// PEER source operates on its OWN state (sPeer*) and does NOT use FileManager
// — the existing FileManager is tightly coupled to local VFS access and the
// peer's listing comes pre-built over the wire. Navigation triggers fresh
// FS_LIST_REQ frames; replies populate sPeerEntries via a callback. The
// browser shows "Loading…" between request and reply, "(busy)" / "(not
// found)" / etc. on status errors, and the entries on success.

enum class FsSource : uint8_t {
  LOCAL    = 0,   // /  (VFS root — already includes /sd as virtual mount)
  SD_QUICK = 1,   // /sd (shortcut for users who live on SD)
  PEER     = 2,   // bonded peer's filesystem via FS_LIST_REQ
};

static FsSource sCurrentSource = FsSource::LOCAL;

// One-shot flag — set when X cycles the source, consumed by prepare to
// re-init the browser (navigate to the new source's root + clear filter).
static bool sSourceChangePending = false;

// Returns a 1-char label for the header ("L" / "S" / "P").
static char sourceLabel(FsSource s) {
  switch (s) {
    case FsSource::LOCAL:    return 'L';
    case FsSource::SD_QUICK: return 'S';
    case FsSource::PEER:     return 'P';
  }
  return '?';
}

// Cycle to the next source. Picker mode is honored: cycling is suppressed
// while a picker is pending so callers don't get a different source than
// they asked for. Returns true if the source actually changed.
static bool cycleSourceForward() {
  if (sPickerActive) return false;  // picker scoped to its own startPath
  uint8_t next = (uint8_t)sCurrentSource + 1;
  if (next > (uint8_t)FsSource::PEER) next = 0;
  sCurrentSource = (FsSource)next;
  sSourceChangePending = true;
  oledFileBrowserNeedsInit = true;
  return true;
}

// Where each source roots its initial navigation. PEER returns nullptr —
// the peer-state handlers below take over and issue FS_LIST_REQs instead
// of touching FileManager.
static const char* sourceRootPath(FsSource s) {
  switch (s) {
    case FsSource::LOCAL:    return "/";
    case FsSource::SD_QUICK: return "/sd";
    case FsSource::PEER:     return nullptr;
  }
  return "/";
}

// ============================================================================
// PEER source state (only used while sCurrentSource == FsSource::PEER)
// ============================================================================
// Memory: sPeerEntries is ps_alloc'd in PSRAM on first entry to the PEER
// source (2.4 KB) and never freed for the device's lifetime — we'd just
// re-alloc it next time. DRAM cost is just the small pointers + state
// machine (~200 B).

#if ENABLE_ESPNOW

enum class PeerListStatus : uint8_t {
  IDLE        = 0,  // No peer connected, or just entered the source
  LOADING     = 1,  // Request in flight, waiting for reply / timeout
  READY       = 2,  // sPeerEntries holds a valid listing
  NOT_BONDED  = 3,  // No bonded peer is paired → show hint
  ERR_NOT_FOUND = 4,
  ERR_PERM    = 5,
  ERR_BUSY    = 6,  // Peer was already serving another request
  ERR_TIMEOUT = 7,  // No reply within FS_LIST_REQUEST_TIMEOUT_MS
  ERR_OTHER   = 8,
};

static char           sPeerPath[FILE_MANAGER_MAX_PATH] = "/";
static V4PayloadFsEntry* sPeerEntries = nullptr;  // ps_alloc on demand
static int            sPeerEntryCount = 0;
static int            sPeerSelectedIdx = 0;
static int            sPeerScrollOffset = 0;
static uint16_t       sPeerTotalEntries = 0;       // from reply (may exceed cached)
static bool           sPeerHasMore = false;
static PeerListStatus sPeerStatus = PeerListStatus::IDLE;
static uint32_t       sPeerCurrentReqId = 0;       // 0 = no request outstanding
static uint8_t        sPeerLastMac[6] = {0};       // remembered for callback validation

// ----------------------------------------------------------------------------
// Injected ESP-NOW file context. When the ESP-NOW device-detail view launches
// the browser via oledFileBrowserStartEspnow{Receive,Send}, it records WHICH
// peer and WHICH direction so the same OLED_FILE_BROWSER mode can:
//   RECEIVE — point the PEER source at this MAC (instead of the bonded peer)
//             and, on selecting a file, FS_GET it.
//   SEND    — drive the FilePickerRequest and, on pick, sendFileToMac to this MAC.
// NONE = ordinary local browsing (X-cycle), bond-mode PEER source uses BondedPeer.
// Cleared on browser reset / receive-exit / send-pick (see context lifecycle).
enum class EspnowFileCtx : uint8_t { NONE = 0, SEND = 1, RECEIVE = 2 };
static EspnowFileCtx sEspnowCtx = EspnowFileCtx::NONE;
static uint8_t       sEspnowCtxMac[6] = {0};
// Pending context set by the entry funcs and consumed once by initFileBrowser on
// the next (forward) entry. This makes a plain Tools->Files entry — which has NO
// pending — reliably reset the context to NONE, so a stale ESP-NOW target can't
// leak in if the user escaped a previous browse via the global Home button.
static EspnowFileCtx sEspnowCtxPending = EspnowFileCtx::NONE;
static uint8_t       sEspnowCtxPendingMac[6] = {0};

// Transient FS_GET (download) feedback shown in the PEER list view. Set on the
// render task (REQUESTING) and on BTC_TASK from the ack callback (terminal) —
// the callback only writes this enum, nothing heavier. Cleared on navigation.
enum class FsGetUi : uint8_t { NONE = 0, REQUESTING, OK, NOT_FOUND, DENIED, FAILED };
static volatile FsGetUi sGetUi = FsGetUi::NONE;

// Reply callback — runs on BTC_TASK (tiny stack). Keep this minimal: copy
// the data into our buffers and let the next render frame pick it up.
static void onPeerListReply(const uint8_t peerMac[6],
                            const V4PayloadFsListReplyHeader* hdr,
                            const V4PayloadFsEntry* entries) {
  if (!hdr) return;
  // Stale reply (we cancelled, or the user navigated since we sent). Drop.
  if (hdr->reqId != sPeerCurrentReqId) return;
  sPeerCurrentReqId = 0;

  switch ((FsListStatus)hdr->status) {
    case FS_LIST_STATUS_OK: {
      // Copy entries into our PSRAM cache. The pointer the callback gives
      // us is borrowed from stack memory owned by the receive path.
      if (sPeerEntries) {
        uint8_t n = hdr->entryCount;
        if (n > FS_LIST_ENTRIES_PER_REPLY) n = FS_LIST_ENTRIES_PER_REPLY;
        if (entries && n > 0) {
          memcpy(sPeerEntries, entries, n * sizeof(V4PayloadFsEntry));
        }
        sPeerEntryCount = n;
        sPeerTotalEntries = hdr->totalEntries;
        sPeerHasMore = hdr->hasMore != 0;
        sPeerSelectedIdx = 0;
        sPeerScrollOffset = 0;
        sPeerStatus = PeerListStatus::READY;
      } else {
        sPeerStatus = PeerListStatus::ERR_OTHER;
      }
      break;
    }
    case FS_LIST_STATUS_NOT_FOUND:   sPeerStatus = PeerListStatus::ERR_NOT_FOUND; break;
    case FS_LIST_STATUS_NOT_A_DIR:   sPeerStatus = PeerListStatus::ERR_NOT_FOUND; break;
    case FS_LIST_STATUS_PERM_DENIED: sPeerStatus = PeerListStatus::ERR_PERM;      break;
    case FS_LIST_STATUS_TOO_BUSY:    sPeerStatus = PeerListStatus::ERR_BUSY;      break;
    case FS_LIST_STATUS_NOT_READY:   sPeerStatus = PeerListStatus::ERR_OTHER;     break;
    case FS_LIST_STATUS_IO_ERROR:
    default:
      // The protocol module also synthesizes IO_ERROR on timeout. We can't
      // distinguish, so flag TIMEOUT when sCurrentReqId was the only thing
      // in flight (best guess).
      sPeerStatus = PeerListStatus::ERR_TIMEOUT;
      break;
  }
}

// Allocate the PSRAM entry cache on first use. Idempotent — returns true if
// the cache is available (already allocated or just allocated).
static bool ensurePeerCache() {
  if (sPeerEntries) return true;
  sPeerEntries = (V4PayloadFsEntry*)ps_alloc(
      FS_LIST_ENTRIES_PER_REPLY * sizeof(V4PayloadFsEntry),
      AllocPref::PreferPSRAM, "filebrowser.peer.entries");
  return sPeerEntries != nullptr;
}

// Kick off a fresh FS_LIST_REQ for sPeerPath. Cancels any in-flight request
// first so a navigation-while-loading doesn't show stale entries.
static void peerStartRequest() {
  if (!ensurePeerCache()) {
    sPeerStatus = PeerListStatus::ERR_OTHER;
    return;
  }
  uint8_t mac[6];
  if (sEspnowCtx != EspnowFileCtx::NONE) {
    // Driven by the ESP-NOW device-detail view: target the selected peer
    // directly (works without bond mode — FsList is base ESP-NOW now).
    memcpy(mac, sEspnowCtxMac, 6);
  } else if (BondedPeer::isPaired() && BondedPeer::peerMacBytes(mac)) {
    // Legacy bond-mode PEER source reachable by X-cycling the sources.
  } else {
    sPeerStatus = PeerListStatus::NOT_BONDED;
    return;
  }
  memcpy(sPeerLastMac, mac, 6);

  // Cancel previous in-flight (no-op if already resolved).
  if (sPeerCurrentReqId != 0) {
    fsListCancel(sPeerCurrentReqId);
    sPeerCurrentReqId = 0;
  }

  uint32_t reqId = fsListSendRequest(mac, sPeerPath, /*startIndex=*/0, onPeerListReply);
  if (reqId == 0) {
    sPeerStatus = PeerListStatus::ERR_OTHER;
    return;
  }
  sPeerCurrentReqId = reqId;
  sPeerStatus = PeerListStatus::LOADING;
}

// Navigate the peer view to a new path. Trims trailing slashes, kicks request.
static void peerNavigateTo(const char* newPath) {
  strlcpy(sPeerPath, newPath, sizeof(sPeerPath));
  // Normalize: drop trailing slash unless path IS just "/"
  size_t n = strlen(sPeerPath);
  if (n > 1 && sPeerPath[n - 1] == '/') sPeerPath[n - 1] = '\0';
  if (sPeerPath[0] == '\0') strlcpy(sPeerPath, "/", sizeof(sPeerPath));
  sGetUi = FsGetUi::NONE;  // clear stale download feedback on fresh listing
  peerStartRequest();
}

// FS_GET ack — BTC_TASK context. Record terminal feedback only (no VFS/display).
// On OK the peer begins a FILE_* transfer that lands via the normal inbound path.
static void onEspnowGetAck(const uint8_t /*peerMac*/[6], const V4PayloadFsGetAck* ack) {
  // Toast the outcome (overlays any screen; the actual "Received" confirmation is
  // toasted from the inbound file-complete path once the bytes land). oledToastShow
  // is thread-safe + self-marks dirty, so it's safe from this BTC_TASK callback.
  if (!ack) { sGetUi = FsGetUi::FAILED; oledToastShow("Download failed"); return; }
  switch ((FsListStatus)ack->status) {
    case FS_LIST_STATUS_OK:          sGetUi = FsGetUi::OK;        oledToastShow("Downloading file..."); break;
    case FS_LIST_STATUS_NOT_FOUND:   sGetUi = FsGetUi::NOT_FOUND; oledToastShow("File not found");      break;
    case FS_LIST_STATUS_PERM_DENIED: sGetUi = FsGetUi::DENIED;    oledToastShow("Permission denied");   break;
    default:                         sGetUi = FsGetUi::FAILED;    oledToastShow("Download failed");      break;
  }
}

// Go up one directory level in the peer view. At root, treats as exit (the
// input handler's NAVIGATE_BACK case handles the actual mode pop).
static bool peerNavigateUp() {
  if (strcmp(sPeerPath, "/") == 0) return false;  // already at root
  // Strip last segment
  char buf[FILE_MANAGER_MAX_PATH];
  strlcpy(buf, sPeerPath, sizeof(buf));
  char* lastSlash = strrchr(buf, '/');
  if (!lastSlash || lastSlash == buf) {
    strlcpy(sPeerPath, "/", sizeof(sPeerPath));
  } else {
    *lastSlash = '\0';
    strlcpy(sPeerPath, buf, sizeof(sPeerPath));
  }
  peerStartRequest();
  return true;
}

// Activate / A-button on the PEER list: enter folders; on a FILE, in RECEIVE
// context, FS_GET it (download to local VFS). Outside RECEIVE context a file
// is a no-op (the bond-mode browse source is list-only).
static void peerActivate() {
  if (sPeerStatus != PeerListStatus::READY) return;
  if (sPeerSelectedIdx < 0 || sPeerSelectedIdx >= sPeerEntryCount) return;
  const V4PayloadFsEntry& e = sPeerEntries[sPeerSelectedIdx];
  char joined[FILE_MANAGER_MAX_PATH];
  if (strcmp(sPeerPath, "/") == 0) {
    snprintf(joined, sizeof(joined), "/%s", e.name);
  } else {
    snprintf(joined, sizeof(joined), "%s/%s", sPeerPath, e.name);
  }
  if (e.isFolder) {
    peerNavigateTo(joined);
    return;
  }
  // File selected.
  if (sEspnowCtx == EspnowFileCtx::RECEIVE) {
    sGetUi = FsGetUi::REQUESTING;
    if (fsGetSendRequest(sEspnowCtxMac, joined, onEspnowGetAck) == 0) {
      sGetUi = FsGetUi::FAILED;
    }
  }
}

// ----------------------------------------------------------------------------
// ESP-NOW Send: defer sendFileToMac onto cmd_exec_task so the multi-second
// FILE_START/DATA/END pump never blocks the OLED render/input task.
struct EspnowSendFileJob { uint8_t mac[6]; char path[FILE_MANAGER_MAX_PATH]; };
static void runEspnowSendFile(void* arg) {
  EspnowSendFileJob* j = (EspnowSendFileJob*)arg;
  if (!j) return;
  bool ok;
  {
    SYSTEM_IDENTITY_SCOPE("oled.espnow.sendfile");  // read the local file at device level
    ok = sendFileToMac(j->mac, String(j->path));
  }
  oledToastShow(ok ? "File sent" : "Send failed");  // completion confirmation (cmd_exec task)
  free(j);
}

// Picker callback (fires after pop back to OLED_ESPNOW). Send the picked local
// file to the context peer; clear the context either way.
static void espnowSendOnPicked(const char* fullPath, bool cancelled) {
  EspnowFileCtx ctx = sEspnowCtx;
  uint8_t mac[6]; memcpy(mac, sEspnowCtxMac, 6);
  sEspnowCtx = EspnowFileCtx::NONE;
  if (cancelled || !fullPath || ctx != EspnowFileCtx::SEND) return;
  EspnowSendFileJob* j = (EspnowSendFileJob*)ps_alloc(
      sizeof(EspnowSendFileJob), AllocPref::PreferInternal, "oled.espnow.sendfile.job");
  if (!j) { oledToastShow("Send failed"); return; }
  memcpy(j->mac, mac, 6);
  strlcpy(j->path, fullPath, sizeof(j->path));
  if (!submitDeferredToCmdExec(runEspnowSendFile, j)) { free(j); oledToastShow("Send failed"); return; }
  oledToastShow("Sending file...");  // instant feedback; "File sent" follows on completion
}

// ----------------------------------------------------------------------------
// Public entry points used by the ESP-NOW device-detail "Send / Receive" chooser.
void oledFileBrowserStartEspnowReceive(const uint8_t mac[6]) {
  memcpy(sEspnowCtxPendingMac, mac, 6);
  sEspnowCtxPending = EspnowFileCtx::RECEIVE;  // consumed by initFileBrowser
  sGetUi = FsGetUi::NONE;
  sCurrentSource = FsSource::PEER;     // browse the remote peer
  sSourceChangePending = true;         // make initFileBrowser issue the root FS_LIST_REQ
  oledFileBrowserNeedsInit = true;
  // Caller transitions: requestOLEDMode(OLED_FILE_BROWSER, "espnow.receive").
}

void oledFileBrowserStartEspnowSend(const uint8_t mac[6]) {
  memcpy(sEspnowCtxPendingMac, mac, 6);
  sEspnowCtxPending = EspnowFileCtx::SEND;  // consumed by initFileBrowser
  sCurrentSource = FsSource::LOCAL;         // pick from local FS (picker)
  FilePickerRequest req = {};
  strlcpy(req.title, "Send to peer", sizeof(req.title));
  strlcpy(req.startPath, "/", sizeof(req.startPath));
  req.filter = nullptr;
  req.onPicked = espnowSendOnPicked;
  req.requesterMode = OLED_ESPNOW;
  oledFilePickerPush(req);
  // Caller transitions: requestOLEDMode(OLED_FILE_BROWSER, "espnow.send").
}

#endif // ENABLE_ESPNOW

// ============================================================================
// File Browser Initialization
// ============================================================================

/**
 * Initialize file browser. Honors a pending picker request by jumping to
 * its startPath and applying its visibility filter; otherwise navigates
 * to the current source's root. PEER source short-circuits (no FileManager
 * navigation) — the placeholder render path takes over.
 */
static bool initFileBrowser() {
  if (gOledFileManager == nullptr) {
    // PSRAM placement-new: FileManager's ~5 KB is an inline cachedEntries[64] POD
    // array (FileEntry has no String), so the whole object moves off internal DRAM.
    // Pair with ps_delete in resetOLEDFileBrowser() — never `delete`.
    void* fmBuf = ps_alloc(sizeof(FileManager), AllocPref::PreferPSRAM, "oled.filemgr");
    if (fmBuf == nullptr) {
      return false;
    }
    gOledFileManager = new (fmBuf) FileManager();
  }

#if ENABLE_ESPNOW
  // Consume any pending ESP-NOW context exactly once. A normal Tools->Files
  // entry has no pending, so this reliably clears a stale context (and target
  // MAC) from a previous send/receive that the user may have escaped via Home.
  sEspnowCtx = sEspnowCtxPending;
  if (sEspnowCtxPending != EspnowFileCtx::NONE) memcpy(sEspnowCtxMac, sEspnowCtxPendingMac, 6);
  sEspnowCtxPending = EspnowFileCtx::NONE;
#endif

  if (sPickerActive) {
    // Picker mode: filter first (changes loadDirectory's compaction step
    // before the navigate), then navigate to the requested start dir.
    gOledFileManager->setVisibilityFilter(sPickerReq.filter);
    const char* startPath = sPickerReq.startPath[0] ? sPickerReq.startPath : "/";
    if (!gOledFileManager->navigate(startPath)) {
      // Bad startPath (deleted, no permission, doesn't exist) — fall back to
      // root rather than wedge. Filter still applies for consistency.
      gOledFileManager->navigate("/");
    }
  } else {
    // Viewer mode: clear any stale filter from a previous picker session.
    gOledFileManager->setVisibilityFilter(nullptr);

    const char* root = sourceRootPath(sCurrentSource);
    if (root) {
      // LOCAL / SD_QUICK: always navigate to the source's root on init.
      // navigate() unconditionally re-loads the directory cache, which is
      // critical because:
      //   (a) on the very first init, the FileManager is fresh — its
      //       constructor seeds currentPath="/" but does NOT run
      //       loadDirectory. Without navigate() here the cache stays
      //       empty and the browser shows zero files.
      //   (b) on subsequent mode entries (oledFileBrowserNeedsInit=true
      //       is set on every entry by OLED_Utils.cpp:5276), the legacy
      //       behavior was "land at root" — preserve that.
      //   (c) we're now inside the ExecIdentityGuard scope from
      //       prepareFileBrowserData above, so the cache loads under the
      //       LOCAL_DISPLAY identity — recovers from the G2-style "first
      //       load under ANON, stuck empty forever" failure mode.
      gOledFileManager->navigate(root);
    }
#if ENABLE_ESPNOW
    else if (sCurrentSource == FsSource::PEER && sSourceChangePending) {
      // PEER source: kick a fresh FS_LIST_REQ at root. The display path
      // shows LOADING / error / entries based on sPeerStatus.
      strlcpy(sPeerPath, "/", sizeof(sPeerPath));
      sPeerSelectedIdx = 0;
      sPeerScrollOffset = 0;
      sPeerEntryCount = 0;
      peerStartRequest();
    }
#endif
    sSourceChangePending = false;
  }
  oledFileBrowserNeedsInit = false;
  return true;
}

// ============================================================================
// File Browser Rendered (two-phase rendering)
// ============================================================================

/**
 * Render file browser to OLED display
 * Compact layout for 128x64 screen:
 * - Line 0-9: Path (truncated)
 * - Line 10-53: File list (5 items, 9px each)
 * - Line 54-63: Navigation hints
 */
// Gather file browser data (called OUTSIDE I2C transaction to avoid blocking gamepad)
void prepareFileBrowserData() {
  // INSTALL OLED LOCAL_DISPLAY IDENTITY FOR THIS RENDER PASS.
  //
  // The OLED render task runs on the main loop, which has NO sticky identity
  // install — its TLS slot defaults to ANON. Without this guard, FileManager
  // / VFS::*Guarded calls deny every read and the browser shows an empty
  // listing for the rest of the boot (the cache caches the empty result and
  // never re-loads under a fresh identity).
  //
  // The G2 file browser hit this same bug and solved it via G2HijackCtxGuard
  // — see G2_Page_Files.cpp g2ShowFilesMenu / g2FilesHandleTap. This is the
  // OLED counterpart. The earlier comment claiming "sticky SYSTEM install in
  // app_main" was stale — that pattern was removed when identity moved to
  // per-task TLS (see commit history on System_AuthIdentity).
  //
  // OLEDFileBrowserCtxGuard installs:
  //   * Identity = OLED login state (gLocalDisplayUser, or "AuthBypass"
  //     reserved name when displayRequireAuth is off)
  //   * Notification source = NOTIF_SOURCE_OLED with the same user as
  //     subsource — anything via notify*() (notifyFileDeleted on a delete
  //     confirm, etc.) attributes to "OLED / <user>" instead of whichever
  //     stale context this render task last held.
  // Both via the composed CommandIdentityScope primitive — see G2HijackCtx-
  // Guard for the parallel construction on the G2 side.
  OLEDFileBrowserCtxGuard ctxGuard;

  // Initialize or reinitialize if needed
  if (!gOledFileManager || oledFileBrowserNeedsInit) {
    if (!initFileBrowser()) {
      fileBrowserRenderData.valid = false;
      return;
    }
  }

  // Rename-keyboard completion poll. While the keyboard is up, all input is
  // centrally intercepted (the mode's inputFunc is never called); once the
  // user completes/cancels we land here on the next prepare pass — INSIDE the
  // identity guard, which renameItem's guarded VFS call requires.
  if (sFbRenameActive) {
    if (oledKeyboardIsCompleted()) {
      String newName = oledKeyboardGetText();
      newName.trim();
      oledKeyboardReset();
      sFbRenameActive = false;
      if (newName.length() == 0) {
        oledToastShow("Empty name - kept", 2500);
      } else if (gOledFileManager->renameItem(newName.c_str())) {
        oledToastShow("Renamed", 2000);
      } else {
        oledToastShow("Rename failed", 2500);
      }
      sFbLevel = FbLevel::LIST;
    } else if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      sFbRenameActive = false;
      sFbLevel = FbLevel::ACTION_MENU;   // back to the menu, nothing changed
    }
  }
  
  // Process pending navigation actions (filesystem I/O happens here, OUTSIDE I2C transaction)
  if (fileBrowserPendingAction != FileBrowserPendingAction::NONE) {
    switch (fileBrowserPendingAction) {
      case FileBrowserPendingAction::NAVIGATE_INTO: {
        FileEntry entry;
        if (gOledFileManager->getCurrentItem(entry)) {
          if (entry.isFolder) {
            gOledFileManager->navigateInto();
          } else {
            // Resolve full path (used by both picker and the .hwmap fallback)
            String fullPath = String(gOledFileManager->getCurrentPath());
            if (!fullPath.endsWith("/")) fullPath += "/";
            fullPath += entry.name;

            if (sPickerActive) {
              // Picker mode wins: fire callback and pop. firePickerCallback
              // clears fileBrowserPendingAction itself, but we return early
              // here too so the post-switch `= NONE` below doesn't re-clobber
              // any pending state the callback might have set up.
              firePickerCallback(fullPath.c_str(), false);
              return;
            }

#if ENABLE_GPS_SENSOR && ENABLE_MAPS
            // Viewer-mode fallback: .hwmap files auto-load into the map.
            // Preserves the legacy "browse to find a map" workflow that
            // existed before the picker layer was added.
            String filename = String(entry.name);
            if (filename.endsWith(".hwmap")) {
              if (MapCore::loadMapFile(fullPath.c_str())) {
                extern bool gMapCenterSet;
                extern bool gMapManuallyPanned;
                requestOLEDMode(OLED_GPS_MAP, "filebrowser.loadmap", false);
                gMapCenterSet = false;
                gMapManuallyPanned = false;
              }
              break;   // .hwmap handled — don't also open the action menu
            }
#endif
            // Regular file: open the per-file action menu (View/Rename/
            // Delete). Replaces the old silent no-op. Snapshot the name for
            // display; the actual ops act on the current selection, which
            // can't move while a menu/overlay owns input.
            strncpy(sFbActionName, entry.name, sizeof(sFbActionName) - 1);
            sFbActionName[sizeof(sFbActionName) - 1] = '\0';
            sFbActionViewable = fbIsViewableExt(entry.name);
            // Fresh entry: start the shared action menu at the top row.
            fbInitActionScroll();
            sFbActionScroll.selectedIndex = 0;
            sFbActionScroll.scrollOffset = 0;
            sFbLevel = FbLevel::ACTION_MENU;
          }
        }
        break;
      }
      case FileBrowserPendingAction::LOAD_VIEW: {
        FileEntry entry;
        if (gOledFileManager->getCurrentItem(entry) && !entry.isFolder) {
          if (entry.size > FB_VIEW_MAX_FILE) {
            // FileManager::readFile pulls the whole file byte-at-a-time into
            // a String — refuse big files here; serial `fileview` pages them.
            oledToastShow("Too big - use fileview", 3000);
            sFbLevel = FbLevel::LIST;
            break;
          }
          String content;
          if (gOledFileManager->readFile(entry.name, content)) {
            bool trunc = false;
            const size_t len = textWrapInto(sFbViewBody, sizeof(sFbViewBody),
                                            content.c_str(), FB_VIEW_COLS,
                                            /*contIndent=*/0, /*stripCtrl=*/true,
                                            &trunc);
            // Line-start offsets for the scroll window (CLI-viewer model).
            constexpr int kMaxLines = (int)(sizeof(sFbViewLineOff) / sizeof(sFbViewLineOff[0]));
            int lc = 0;
            sFbViewLineOff[lc++] = 0;
            for (size_t i = 0; i + 1 < len && lc < kMaxLines; i++) {
              if (sFbViewBody[i] == '\n') sFbViewLineOff[lc++] = (uint16_t)(i + 1);
            }
            sFbViewLineCount = lc;
            sFbViewScroll    = 0;
            sFbViewTruncated = trunc;
            sFbLevel = FbLevel::VIEW;
          } else {
            oledToastShow("Read failed", 2500);
            sFbLevel = FbLevel::LIST;
          }
        } else {
          sFbLevel = FbLevel::LIST;
        }
        break;
      }
      case FileBrowserPendingAction::DELETE_SELECTED: {
        // Set ONLY by the confirm dialog's Yes callback (which runs on the
        // input path with no identity guard — the delete itself must happen
        // here, under ctxGuard, or removeGuarded denies/mis-attributes).
        if (gOledFileManager->deleteItem()) {
          oledToastShow("Deleted", 2000);
        } else {
          oledToastShow("Delete failed", 2500);
        }
        sFbLevel = FbLevel::LIST;
        break;
      }
      case FileBrowserPendingAction::NAVIGATE_UP:
        gOledFileManager->navigateUp();
        break;
      case FileBrowserPendingAction::NAVIGATE_BACK:
        if (strcmp(gOledFileManager->getCurrentPath(), "/") == 0) {
          // At root → exiting the browser. Picker mode treats this as
          // cancel; viewer mode just pops back to whoever opened Files.
          if (sPickerActive) {
            firePickerCallback(nullptr, true);
            return;
          }
          fileBrowserPendingAction = FileBrowserPendingAction::NONE;
          fileBrowserRenderData.valid = false;
          oledMenuBack();
          return;
        } else {
          gOledFileManager->navigateUp();
        }
        break;
      default:
        break;
    }
    fileBrowserPendingAction = FileBrowserPendingAction::NONE;
  }
  
  // Gather all data needed for rendering
  strncpy(fileBrowserRenderData.path, gOledFileManager->getCurrentPath(), FILE_MANAGER_MAX_PATH - 1);
  fileBrowserRenderData.path[FILE_MANAGER_MAX_PATH - 1] = '\0';
  fileBrowserRenderData.itemCount = gOledFileManager->getItemCount();
  fileBrowserRenderData.selectedIdx = gOledFileManager->getSelectedIndex();
  fileBrowserRenderData.pageStart = gOledFileManager->getPageStart();
  fileBrowserRenderData.pageEnd = gOledFileManager->getPageEnd();
  
  // Pre-fetch all visible items (filesystem I/O happens here)
  int itemsFetched = 0;
  for (int i = fileBrowserRenderData.pageStart; i < fileBrowserRenderData.pageEnd && i < fileBrowserRenderData.itemCount && itemsFetched < FILE_MANAGER_PAGE_SIZE; i++) {
    if (gOledFileManager->getItem(i, fileBrowserRenderData.items[itemsFetched])) {
      itemsFetched++;
    }
  }
  
  // Determine if selected item is a folder (for footer hints)
  fileBrowserRenderData.selectedIsFolder = false;
  if (fileBrowserRenderData.itemCount > 0) {
    int selectedItemIdx = fileBrowserRenderData.selectedIdx - fileBrowserRenderData.pageStart;
    if (selectedItemIdx >= 0 && selectedItemIdx < FILE_MANAGER_PAGE_SIZE && selectedItemIdx < itemsFetched) {
      fileBrowserRenderData.selectedIsFolder = fileBrowserRenderData.items[selectedItemIdx].isFolder;
    }
  }
  
  fileBrowserRenderData.valid = true;
}

// Render file browser from pre-gathered data (called INSIDE I2C transaction)
void displayFileBrowserRendered() {
  if (!oledDisplay || !oledConnected) return;

#if ENABLE_ESPNOW
  // PEER source — render from sPeerEntries (populated via FS_LIST_REPLY).
  if (sCurrentSource == FsSource::PEER) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

    // Status banner / error states
    const char* statusLine = nullptr;
    switch (sPeerStatus) {
      case PeerListStatus::NOT_BONDED:    statusLine = "No bonded peer"; break;
      case PeerListStatus::LOADING:       statusLine = "Loading..."; break;
      case PeerListStatus::ERR_NOT_FOUND: statusLine = "Path not found"; break;
      case PeerListStatus::ERR_PERM:      statusLine = "Permission denied"; break;
      case PeerListStatus::ERR_BUSY:      statusLine = "Peer busy — retry"; break;
      case PeerListStatus::ERR_TIMEOUT:   statusLine = "Peer not responding"; break;
      case PeerListStatus::ERR_OTHER:     statusLine = "Error"; break;
      case PeerListStatus::IDLE:          statusLine = "Press X to switch"; break;
      case PeerListStatus::READY:         statusLine = nullptr; break;
    }
    if (statusLine) {
      int y = OLED_CONTENT_START_Y;
      oledDisplay->setCursor(0, y); oledDisplay->print("Peer: ");
      // Show truncated peer mac last 2 octets so the user knows which peer
      char tail[8];
      snprintf(tail, sizeof(tail), "%02X:%02X", sPeerLastMac[4], sPeerLastMac[5]);
      oledDisplay->print(tail);
      y += 14;
      oledDisplay->setCursor(0, y); oledDisplay->println(statusLine);
      y += 10;
      oledDisplay->setCursor(0, y); oledDisplay->print("Path: ");
      oledDisplay->print(sPeerPath);
      return;
    }

    // READY — render entries
    const int itemHeight = 10;
    const int maxVisible = 4;
    int startY = OLED_CONTENT_START_Y + 1;

    // Clamp + scroll math
    if (sPeerEntryCount == 0) {
      oledDisplay->setCursor(20, OLED_CONTENT_START_Y + 20);
      oledDisplay->print("(empty)");
    } else {
      // Keep cursor visible
      if (sPeerSelectedIdx < sPeerScrollOffset) sPeerScrollOffset = sPeerSelectedIdx;
      if (sPeerSelectedIdx >= sPeerScrollOffset + maxVisible) {
        sPeerScrollOffset = sPeerSelectedIdx - maxVisible + 1;
      }
      for (int i = 0; i < maxVisible && (sPeerScrollOffset + i) < sPeerEntryCount; i++) {
        int idx = sPeerScrollOffset + i;
        const V4PayloadFsEntry& e = sPeerEntries[idx];
        int y = startY + i * itemHeight;
        bool selected = (idx == sPeerSelectedIdx);
        if (selected) {
          oledDisplay->fillRect(0, y, 128, itemHeight - 1, DISPLAY_COLOR_WHITE);
          oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
        } else {
          oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        }
        oledDisplay->setCursor(2, y + 1);
        // Format: "[F] name" (folder) or "name 1.2K" (file)
        char line[24];
        if (e.isFolder) {
          snprintf(line, sizeof(line), "%s/", e.name);
        } else {
          snprintf(line, sizeof(line), "%.18s", e.name);
        }
        oledDisplay->print(line);
      }
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      // Scroll arrows + "x of N"
      if (sPeerScrollOffset > 0) {
        oledDisplay->setCursor(121, OLED_CONTENT_START_Y);
        oledDisplay->print("^");
      }
      if (sPeerScrollOffset + maxVisible < sPeerEntryCount) {
        oledDisplay->setCursor(121, OLED_CONTENT_START_Y + (maxVisible - 1) * itemHeight);
        oledDisplay->print("v");
      }
      // Bottom content line: FS_GET download feedback takes priority over the
      // "M/N" pagination indicator while a download is in progress / just done.
      const char* getMsg = nullptr;
      switch (sGetUi) {
        case FsGetUi::REQUESTING: getMsg = "Requesting file..."; break;
        case FsGetUi::OK:         getMsg = "Downloading...";      break;
        case FsGetUi::NOT_FOUND:  getMsg = "File not found";      break;
        case FsGetUi::DENIED:     getMsg = "Permission denied";   break;
        case FsGetUi::FAILED:     getMsg = "Download failed";     break;
        case FsGetUi::NONE:       getMsg = nullptr;               break;
      }
      if (getMsg) {
        oledDisplay->setCursor(0, DISPLAY_HEIGHT - 9);
        oledDisplay->print(getMsg);
      } else if (sPeerHasMore || sPeerTotalEntries > sPeerEntryCount) {
        char ftr[24];
        snprintf(ftr, sizeof(ftr), "%d/%u%s", sPeerEntryCount,
                 (unsigned)sPeerTotalEntries, sPeerHasMore ? "+" : "");
        oledDisplay->setCursor(0, DISPLAY_HEIGHT - 9);
        oledDisplay->print(ftr);
      }
    }
    return;
  }
#endif // ENABLE_ESPNOW

  // Rename keyboard owns the whole content area while active.
  if (oledKeyboardDrawIfActive(oledDisplay)) return;

  // Per-file action menu (View / Rename / Delete / Back) — shared scrolling list
  // (OLEDScrollState) with the filename on a one-line header above it, so the
  // options fit the content area and scroll instead of spilling under the footer.
  if (sFbLevel == FbLevel::ACTION_MENU) {
    fbPopulateActionScroll();
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    // Filename header (clipped to one row).
    char nameRow[22];
    snprintf(nameRow, sizeof(nameRow), "%s", sFbActionName);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->print(nameRow);
    // List rendered one row below the header via the shared single-line renderer.
    oledScrollRenderSimple(oledDisplay, &sFbActionScroll, /*showSelection=*/true,
                           OLED_CONTENT_START_Y + 8);
    return;
  }

  // Text viewer: line-scrolled wrapped body (CLI-viewer window model).
  if (sFbLevel == FbLevel::VIEW) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    const int visible = fbViewVisibleLines();
    const int maxScroll = (sFbViewLineCount > visible) ? (sFbViewLineCount - visible) : 0;
    if (sFbViewScroll > maxScroll) sFbViewScroll = maxScroll;
    int y = OLED_CONTENT_START_Y;
    for (int i = 0; i < visible && (sFbViewScroll + i) < sFbViewLineCount; i++) {
      const uint16_t off = sFbViewLineOff[sFbViewScroll + i];
      char line[FB_VIEW_COLS + 2];
      size_t l = 0;
      for (const char* p = sFbViewBody + off;
           *p && *p != '\n' && l < (size_t)FB_VIEW_COLS + 1; ++p) {
        line[l++] = *p;
      }
      line[l] = '\0';
      oledDisplay->setCursor(0, y);
      oledDisplay->print(line);
      y += 10;
    }
    // Scroll indicators + truncation marker on the right edge.
    if (sFbViewScroll > 0) {
      oledDisplay->setCursor(122, OLED_CONTENT_START_Y);
      oledDisplay->print("\x18");
    }
    if (sFbViewScroll < maxScroll) {
      oledDisplay->setCursor(122, OLED_CONTENT_START_Y + (visible - 1) * 10);
      oledDisplay->print("\x19");
    } else if (sFbViewTruncated) {
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y + (visible - 1) * 10);
      oledDisplay->print("[truncated]");
    }
    return;
  }

  if (!fileBrowserRenderData.valid) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Init failed!");
    return;
  }
  
  // Layout constants (matching menu style) - adjusted for global header
  const int listWidth = 78;       // Width for text list area
  const int iconAreaX = 88;       // X position for icon area
  const int iconSize = 32;        // Full size icon for selected item
  const int itemHeight = 10;      // Height per item
  const int maxVisibleItems = 4;  // Items visible at once
  const int startY = OLED_CONTENT_START_Y + 1;  // Start 1px below header for even spacing
  
  // Header is now drawn globally
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Draw vertical separator between list and icon area
  oledDisplay->drawFastVLine(84, OLED_CONTENT_START_Y, OLED_CONTENT_HEIGHT, DISPLAY_COLOR_WHITE);
  
  // Calculate scroll offset to keep selected item visible
  int scrollOffset = 0;
  if (fileBrowserRenderData.selectedIdx >= maxVisibleItems) {
    scrollOffset = fileBrowserRenderData.selectedIdx - maxVisibleItems + 1;
  }
  
  // === File List: Show 4 items (text list on left) ===
  int itemIdx = 0;
  for (int i = 0; i < maxVisibleItems && (scrollOffset + i) < fileBrowserRenderData.itemCount; i++) {
    int idx = scrollOffset + i;
    int y = startY + i * itemHeight;
    
    // Get the item from pre-fetched data
    if (idx >= fileBrowserRenderData.pageStart && idx < fileBrowserRenderData.pageEnd && itemIdx < FILE_MANAGER_PAGE_SIZE) {
      FileEntry& entry = fileBrowserRenderData.items[itemIdx];
      itemIdx++;
      
      bool isSelected = (idx == fileBrowserRenderData.selectedIdx);
      
      // Highlight selected item (1px shorter to create gap, no -1 offset)
      if (isSelected) {
        oledDisplay->fillRect(0, y, listWidth, itemHeight - 1, DISPLAY_COLOR_WHITE);
        oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
      } else {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      }
      
      // Draw name (truncate to fit in list area, 1px down to align with highlight)
      oledDisplay->setCursor(2, y + 1);
      String name = String(entry.name);
      if (name.length() > 13) {
        name = name.substring(0, 10); name += "...";
      }
      oledDisplay->print(name);
    }
  }
  
  // Reset text color
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // === Draw selected item's icon and info on the right ===
  if (fileBrowserRenderData.itemCount > 0) {
    // Get selected item
    int selectedItemIdx = fileBrowserRenderData.selectedIdx - fileBrowserRenderData.pageStart;
    if (selectedItemIdx >= 0 && selectedItemIdx < FILE_MANAGER_PAGE_SIZE) {
      FileEntry& selectedEntry = fileBrowserRenderData.items[selectedItemIdx];
      
      // Draw icon (centered in content area, not touching header)
      const int availableIconHeight = OLED_CONTENT_HEIGHT - 10;
      int iconX = iconAreaX + (128 - iconAreaX - iconSize) / 2;
      int iconY = OLED_CONTENT_START_Y + (availableIconHeight - iconSize - 18) / 2;  // Relative to content area
      
      if (selectedEntry.isFolder) {
        drawIcon(oledDisplay, "folder", iconX, iconY, DISPLAY_COLOR_WHITE);
      } else {
        const char* ext = strrchr(selectedEntry.name, '.');
        const char* iconName = getIconNameForExtension(ext ? ext + 1 : "");
        drawIcon(oledDisplay, iconName, iconX, iconY, DISPLAY_COLOR_WHITE);
      }
      
      // Draw file info below icon
      int textY = iconY + iconSize + 2;
      if (textY + 16 <= OLED_CONTENT_HEIGHT) {
        oledDisplay->setTextSize(1);
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        
        // Show file size or folder indicator
        if (selectedEntry.isFolder) {
          oledDisplay->setCursor(iconAreaX + 2, textY);
          oledDisplay->print("Folder");
        } else {
          String sizeStr = formatFileSize(selectedEntry.size);
          // Center the size text
          int sizeWidth = sizeStr.length() * 6;
          int sizeX = iconAreaX + (128 - iconAreaX - sizeWidth) / 2;
          oledDisplay->setCursor(sizeX, textY);
          oledDisplay->print(sizeStr);
        }
      }
    }
  }
  
  // Show empty message if no items
  if (fileBrowserRenderData.itemCount == 0) {
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(20, 30);
    oledDisplay->print("(empty)");
  }
  
  // Draw scroll indicators if needed (must stay within content area)
  if (scrollOffset > 0) {
    oledDisplay->setCursor(78, OLED_CONTENT_START_Y + 1);
    oledDisplay->print("^");
  }
  if (scrollOffset + maxVisibleItems < fileBrowserRenderData.itemCount) {
    int scrollDownY = OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 9;
    oledDisplay->setCursor(78, scrollDownY);
    oledDisplay->print("v");
  }
  
  // Note: Footer navigation hints now handled by global footer system
  // Don't call display() here - let updateOLEDDisplay() render footer and display in same frame
}

// ============================================================================
// File Browser Navigation Functions
// ============================================================================

/**
 * File browser navigation functions
 * Call these from your button interrupt handlers or main loop
 */
void oledFileBrowserUp() {
  if (!gOledFileManager) return;
  
  unsigned long now = millis();
  if (now - oledFileBrowserLastInput < OLED_FILE_BROWSER_DEBOUNCE) return;
  oledFileBrowserLastInput = now;
  
  gOledFileManager->moveUp();
  // Display will update on next updateOLEDDisplay() call
}

void oledFileBrowserDown() {
  if (!gOledFileManager) return;
  
  unsigned long now = millis();
  if (now - oledFileBrowserLastInput < OLED_FILE_BROWSER_DEBOUNCE) return;
  oledFileBrowserLastInput = now;
  
  gOledFileManager->moveDown();
  // Display will update on next updateOLEDDisplay() call
}

void oledFileBrowserSelect() {
  if (!gOledFileManager) return;
  
  unsigned long now = millis();
  if (now - oledFileBrowserLastInput < OLED_FILE_BROWSER_DEBOUNCE) return;
  oledFileBrowserLastInput = now;
  
  // Defer navigation to prevent filesystem I/O outside I2C transaction
  fileBrowserPendingAction = FileBrowserPendingAction::NAVIGATE_INTO;
  // Actual navigation will happen in displayFileBrowser() inside I2C transaction
}

void oledFileBrowserBack() {
  if (!gOledFileManager) return;
  
  unsigned long now = millis();
  if (now - oledFileBrowserLastInput < OLED_FILE_BROWSER_DEBOUNCE) return;
  oledFileBrowserLastInput = now;
  
  // Defer navigation to prevent filesystem I/O outside I2C transaction
  fileBrowserPendingAction = FileBrowserPendingAction::NAVIGATE_BACK;
  // Actual navigation will happen in displayFileBrowser() inside I2C transaction
}

// ============================================================================
// File Browser Input Handler (registered via OLEDModeEntry)
// ============================================================================

static bool fileBrowserInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  extern NavEvents gNavEvents;

#if ENABLE_ESPNOW
  // PEER source operates on its own state (sPeer*) — input goes through the
  // peerNavigateUp/peerActivate helpers rather than oledFileBrowserUp/etc.
  if (sCurrentSource == FsSource::PEER) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
      // In an ESP-NOW receive context the user chose "Receive from peer X" —
      // don't let X yank them to a different source. Plain bond-mode browse
      // (ctx==NONE) keeps the source-cycle behavior.
      if (sEspnowCtx == EspnowFileCtx::NONE) cycleSourceForward();
      return true;
    }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      // Try to go up a directory first. If already at root, exit the browser.
      if (!peerNavigateUp()) {
        if (sPeerCurrentReqId != 0) {
          fsListCancel(sPeerCurrentReqId);
          sPeerCurrentReqId = 0;
        }
        // Leaving an ESP-NOW receive: restore LOCAL so the next plain Tools->Files
        // entry isn't stuck in the PEER source (X-cycle choices are preserved).
        if (sEspnowCtx == EspnowFileCtx::RECEIVE) sCurrentSource = FsSource::LOCAL;
        sEspnowCtx = EspnowFileCtx::NONE;
        oledMenuBack();
      }
      return true;
    }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      // Retry on error states; enter folder on READY
      if (sPeerStatus == PeerListStatus::READY) {
        peerActivate();
      } else if (sPeerStatus == PeerListStatus::ERR_BUSY ||
                 sPeerStatus == PeerListStatus::ERR_TIMEOUT ||
                 sPeerStatus == PeerListStatus::NOT_BONDED) {
        peerStartRequest();  // retry
      }
      return true;
    }
    if (gNavEvents.up) {
      if (sPeerStatus == PeerListStatus::READY && sPeerSelectedIdx > 0) {
        sPeerSelectedIdx--;
      }
      return true;
    }
    if (gNavEvents.down) {
      if (sPeerStatus == PeerListStatus::READY &&
          sPeerSelectedIdx + 1 < sPeerEntryCount) {
        sPeerSelectedIdx++;
      }
      return true;
    }
    return true;  // swallow everything else
  }
#endif // ENABLE_ESPNOW

  // Per-file action menu level: joystick moves the selection, A dispatches,
  // B backs out to the list. No FS I/O here — every op defers to the guarded
  // prepare pass via pending flags / overlays.
  if (sFbLevel == FbLevel::ACTION_MENU) {
    // Rebuild first so itemCount/labels are current for nav-clamping + dispatch,
    // then let the shared scroll helper consume up/down (with wrap).
    fbPopulateActionScroll();
    const char* rows[4];
    const int n = fbBuildActionRows(rows);
    if (oledScrollHandleNav(&sFbActionScroll)) return true;
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      int sel = sFbActionScroll.selectedIndex;
      if (sel < 0 || sel >= n) sel = 0;
      const char* pick = rows[sel];
      if (strcmp(pick, "View") == 0) {
        fileBrowserPendingAction = FileBrowserPendingAction::LOAD_VIEW;
      } else if (strcmp(pick, "Rename") == 0) {
        if (!oledGuestBlocksMutate()) {
          sFbRenameActive = true;
          oledKeyboardInit("Rename:", sFbActionName, OLED_KEYBOARD_MAX_LENGTH);
        }
      } else if (strcmp(pick, "Delete") == 0) {
        if (!oledGuestBlocksMutate()) {
          snprintf(sFbDeletePrompt, sizeof(sFbDeletePrompt), "Delete %.28s?", sFbActionName);
          // onYes runs on the input path with NO identity guard — it only
          // sets the pending flag; the delete itself executes in prepare.
          // defaultYes=false so a stray A can't destroy a file.
          oledConfirmRequest(sFbDeletePrompt, nullptr,
                             [](void*) {
                               fileBrowserPendingAction = FileBrowserPendingAction::DELETE_SELECTED;
                             },
                             nullptr, /*defaultYes=*/false);
        }
      } else {  // "< Back"
        sFbLevel = FbLevel::LIST;
      }
      return true;
    }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      sFbLevel = FbLevel::LIST;
      return true;
    }
    return true;  // swallow everything else while the menu is up
  }

  // Viewer level: line scrolling only. B returns to the action menu.
  if (sFbLevel == FbLevel::VIEW) {
    const int visible = fbViewVisibleLines();
    const int maxScroll = (sFbViewLineCount > visible) ? (sFbViewLineCount - visible) : 0;
    if (gNavEvents.up)   { if (sFbViewScroll > 0)         sFbViewScroll--; return true; }
    if (gNavEvents.down) { if (sFbViewScroll < maxScroll) sFbViewScroll++; return true; }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      sFbLevel = FbLevel::ACTION_MENU;
      return true;
    }
    return true;  // swallow everything else while viewing
  }

  // X cycles the source (only when no picker is active — pickers are scoped
  // to their own startPath and shouldn't get yanked elsewhere).
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X) && !sPickerActive) {
    cycleSourceForward();
    return true;
  }

  if (gNavEvents.down) {
    oledFileBrowserDown();
    return true;
  } else if (gNavEvents.up) {
    oledFileBrowserUp();
    return true;
  }

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    oledFileBrowserSelect();
    return true;
  }
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    oledFileBrowserBack();
    return true;
  }
  return false;
}

extern void displayFileBrowserRendered();

// Entry hook. Previously the reset was duplicated two ways: cmd_oledmode called
// the heavy resetOLEDFileBrowser() (delete+reinit), while the menu set the lazy
// oledFileBrowserNeedsInit flag. Unify on the lazy flag (reinit happens on the
// next render) and gate on isForward so back-navigation keeps your folder/scroll
// position instead of snapping to root.
static void fileBrowserOnEnter(bool isForward) {
  if (isForward) {
    oledFileBrowserNeedsInit = true;
    // A Home-button escape mid-view/menu must not leak the level into the
    // next entry (same stale-context class the sEspnowCtxPending dance
    // guards against).
    sFbLevel = FbLevel::LIST;
    sFbRenameActive = false;
    sFbViewScroll = 0;
  }
}

// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints, onEnter
static const OLEDModeEntry sFileBrowserModes[] = {
  { OLED_FILE_BROWSER, "Files", "file_text", displayFileBrowserRendered, nullptr, fileBrowserInputHandler, false, -1, nullptr, fileBrowserOnEnter },
};

REGISTER_OLED_MODE_MODULE(sFileBrowserModes, sizeof(sFileBrowserModes) / sizeof(sFileBrowserModes[0]), "FileBrowser");

/**
 * Reset file browser (e.g., when switching to this mode)
 */
void resetOLEDFileBrowser() {
  // Clean up existing manager
  if (gOledFileManager) {
    ps_delete(gOledFileManager);
    gOledFileManager = nullptr;
  }
#if ENABLE_ESPNOW
  // A plain Tools→Files entry must never inherit a stale ESP-NOW send/receive
  // context from a previous device-detail launch.
  sEspnowCtx = EspnowFileCtx::NONE;
  sGetUi = FsGetUi::NONE;
#endif

  // Initialize immediately (not on next display call)
  initFileBrowser();
}

#endif // ENABLE_OLED_DISPLAY
