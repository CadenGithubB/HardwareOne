// test_g2_files_psram.py compiles the production G2 Files page
// (G2_Page_Files.cpp with its includes stripped and the text reader/viewer
// replaced by a stub) beside the doubles below, and drives it from simulated
// tasks: taps on g2_tap_disp, queued redraws on the lens applier, the
// rename/delete completion on cmd_exec_task, the info-overlay expiry on the
// main loop and the image viewers' onDone on the session worker.
//
// The completion, the expiry and the viewer return must only queue a
// menuGen-fenced redraw (no lens draw, no delay, no FileManager use). Every
// FileManager call and every lens draw from the page must happen while the
// calling task holds the Files page lock.
//
// Chooser actions (view, Info's sniff, rename, delete) must act on the
// selected entry's full path from FileManager::getSelectedPath(), not on the
// 63-byte display name, and refuse a path that cannot be resolved or does not
// fit. Folder rows keep the item-count badge whole.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "G2_RenderTypes.h"
#include "System_TextPager.h"
// The page's file classifier (a pre-fix tree has private predicates instead).
#if __has_include("FileBrowser_FileKind.h")
#include "FileBrowser_FileKind.h"
#endif

static unsigned checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (false)

#define EXT_RAM_BSS_ATTR
template <typename... Args> void quietLog(const char*, Args...) {}
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)

// INSERT_BOUNDARY_TYPES
// INSERT_G2_TYPES
// INSERT_TEXT_ENTRY_TYPES
// INSERT_FILE_MANAGER_TYPES

// --- Simulated tasks ---------------------------------------------------------
enum Task : int {
  TASK_NONE = 0, TASK_TAP, TASK_APPLIER, TASK_CMD_EXEC, TASK_MAIN_LOOP, TASK_SESSION,
};
static Task currentTask = TASK_TAP;
struct OnTask {
  Task saved;
  explicit OnTask(Task task) : saved(currentTask) { currentTask = task; }
  ~OnTask() { currentTask = saved; }
};
static bool uiWorker() { return currentTask == TASK_TAP || currentTask == TASK_APPLIER; }

// --- FreeRTOS doubles ---------------------------------------------------------
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
struct StaticSemaphore_t { int unused; };
struct FakeMutex { Task owner; unsigned depth; };
typedef FakeMutex* SemaphoreHandle_t;
static FakeMutex filesLock{TASK_NONE, 0};
static unsigned lockTakes = 0, lockContended = 0, contendedDepth = 0;
// Runs once when the lens applier next asks for the lock: a tap that held the
// lock while the applier's already-admitted Redraw job waited for it.
static void (*beforeApplierLock)() = nullptr;
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t*) {
  return &filesLock;
}
// A real take would block while another task holds the lock; the scenarios
// never let that happen on a correct page, so contention is recorded.
inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t) {
  if (beforeApplierLock && currentTask == TASK_APPLIER && !mutex->depth) {
    void (*tap)() = beforeApplierLock;
    beforeApplierLock = nullptr;
    tap();
  }
  ++lockTakes;
  if (mutex->depth && mutex->owner != currentTask) {
    ++lockContended;
    ++contendedDepth;
    return pdTRUE;
  }
  mutex->owner = currentTask;
  ++mutex->depth;
  return pdTRUE;
}
inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) {
  if (mutex->depth && mutex->owner == currentTask) {
    if (--mutex->depth == 0) mutex->owner = TASK_NONE;
  } else {
    CHECK(contendedDepth > 0);
    if (contendedDepth) --contendedDepth;
  }
  return pdTRUE;
}
static bool filesLockHeld() { return filesLock.depth > 0 && filesLock.owner == currentTask; }

struct portMUX_TYPE { int taken; };
#define portMUX_INITIALIZER_UNLOCKED {0}
static int criticalDepth = 0;
inline void muxEnter(portMUX_TYPE* mux) { CHECK(!mux->taken); mux->taken = 1; ++criticalDepth; }
inline void muxExit(portMUX_TYPE* mux) { CHECK(mux->taken); mux->taken = 0; --criticalDepth; }
#define portENTER_CRITICAL(mux) muxEnter(mux)
#define portEXIT_CRITICAL(mux) muxExit(mux)

static unsigned delays = 0;
inline void vTaskDelay(TickType_t) { ++delays; }

// --- Arduino String, identity and sealed-capture boundaries -------------------
struct String {
  std::string value;
  String() = default;
  String(const char* text) : value(text ? text : "") {}
  const char* c_str() const { return value.c_str(); }
  void reserve(size_t n) { value.reserve(n); }
  size_t length() const { return value.size(); }
  String& operator+=(const char* text) { value += text ? text : ""; return *this; }
};
inline String operator+(const String& left, const char* right) {
  String out = left;
  out.value += right ? right : "";
  return out;
}
struct G2HijackCtxGuard { G2HijackCtxGuard() {} };
struct AuthContext { int user; };
inline AuthContext currentAuthContext() { return AuthContext{1}; }
inline bool canRead(const String&, const AuthContext&) { return true; }
static std::string sniffPath;  // the Info overlay's sealed-capture peek
inline bool readTextLimited(const char* path, String&, size_t, const AuthContext&) {
  sniffPath = path ? path : "";
  return false;
}
inline bool captureCryptoIsMagicLine(const char*) { return false; }
#define CAPCRYPT_MAGIC_PREFIX "#HW1ENC"

// --- Lens and page boundaries -------------------------------------------------
static bool listAccepts = true;
static unsigned draws = 0, drawsOffUiWorkers = 0, drawsWithoutFilesLock = 0;
static unsigned listsShown = 0;
static std::vector<std::string> shownRows;  // last accepted list (list or mixed)

static void noteDraw() {
  ++draws;
  if (!uiWorker()) ++drawsOffUiWorkers;
  if (!filesLockHeld()) ++drawsWithoutFilesLock;
  CHECK(criticalDepth == 0);
}
static bool acceptRows(const char* const* rows, size_t count) {
  noteDraw();
  if (!listAccepts) return false;
  ++listsShown;
  shownRows.assign(rows, rows + count);
  return true;
}
bool g2ShowListPage(const char* const* rows, size_t count,
                    const G2ContainerGeom& = G2_GEOM_LARGE) {
  return acceptRows(rows, count);
}
bool g2ShowMixedListText(const char* const* rows, size_t count,
                         const G2ContainerGeom&, const G2TextChildSpec& title) {
  CHECK(title.content && std::strstr(title.content, "Location") != nullptr);
  return acceptRows(rows, count);
}
bool g2ShowText(const char*) { noteDraw(); return true; }
static std::string lastTextAsList;
bool g2ShowTextAsList(const char* text, const char* = nullptr) {
  noteDraw();
  lastTextAsList = text ? text : "";
  return true;
}
bool g2ShowTextPage(const char*, const G2ContainerGeom&, void (*)(), G2TapFn) {
  noteDraw();
  return true;
}
bool g2TextPagerRender(TextPager&, char*, size_t, const G2TextPageChrome&,
                       const G2ContainerGeom&, void (*)(), G2TapFn) {
  noteDraw();
  return true;
}

static void (*viewerDone)() = nullptr;
static std::string viewerPath;
static bool startViewer(const char* path, void (*onDone)()) {
  CHECK(currentTask == TASK_TAP);
  viewerPath = path ? path : "";
  viewerDone = onDone;
  return true;
}
bool g2ShowBmpFile(const char* path, void (*onDone)()) { return startViewer(path, onDone); }
bool g2ShowBmpFileFullScreen(const char* path, void (*onDone)()) { return startViewer(path, onDone); }
bool g2ShowJpgFile(const char* path, void (*onDone)()) { return startViewer(path, onDone); }
bool g2ShowJpgFileFullScreen(const char* path, void (*onDone)()) { return startViewer(path, onDone); }

static bool overlayActive = false;
static void (*overlayExpired)(G2OverlayKind) = nullptr;
void g2LensStartOverlay(G2OverlayKind kind, uint32_t ms) {
  CHECK(kind == G2_OVERLAY_FILE_INFO && ms > 0);
  overlayActive = true;
}
void g2LensClearOverlay() { overlayActive = false; }
inline bool g2LensInOverlay() { return overlayActive; }
void g2LensSetOverlayExpiredCb(void (*cb)(G2OverlayKind)) { overlayExpired = cb; }

static G2HijackPage page = G2_HIJACK_PAGE_MAIN;
static uint32_t menuGen = 1;
G2HijackPage g2GetHijackPage() { return page; }
void g2SetHijackPage(G2HijackPage value) {
  if (page == value) return;
  page = value;
  ++menuGen;  // like G2_Glasses.cpp: a page change invalidates snapshots
}
uint32_t g2CurrentMenuGen() { return menuGen; }
void g2BumpMenuGen() { ++menuGen; }
static unsigned mainMenus = 0;
void g2RedrawHijackMainMenu() { noteDraw(); ++mainMenus; }

static TextEntryConfig keyboard{};
bool g2BeginTextEntry(const TextEntryConfig& cfg) {
  keyboard = cfg;
  ++menuGen;  // entering text entry invalidates view snapshots
  return true;
}

// Hijack command bridge: the helper stamps seq and menuGen like
// G2_HijackCmd.cpp and keeps the completion for the test to run.
using Completion = void (*)(bool, const char*, const G2CmdCookie&, void*);
struct Submitted { std::string line; G2CmdCookie cookie; Completion done; void* user; };
static std::vector<Submitted> submitted;
static uint64_t cmdSeq = 0;
bool g2SubmitHijackCommand(const char* line, const G2CmdCookie& cookie,
                           Completion done, void* user) {
  CHECK(currentTask == TASK_TAP);
  Submitted entry{line ? line : "", cookie, done, user};
  entry.cookie.seq = ++cmdSeq;
  entry.cookie.menuGen = menuGen;
  submitted.push_back(entry);
  return true;
}

// Lens applier queue: Redraw jobs are dropped when their menuGen is stale.
struct QueuedRedraw { G2CmdCookie cookie; void (*render)(); Task from; std::string tag; };
static std::vector<QueuedRedraw> redraws;
static unsigned applierDropped = 0;
bool g2EnqueueRedraw(const G2CmdCookie& cookie, void (*render)(), const char* tag) {
  if (!render) return false;
  redraws.push_back({cookie, render, currentTask, tag ? tag : ""});
  return true;
}
static void runApplier() {
  OnTask applier(TASK_APPLIER);
  std::vector<QueuedRedraw> jobs;
  jobs.swap(redraws);
  for (const QueuedRedraw& job : jobs) {
    if (job.cookie.menuGen != menuGen) { ++applierDropped; continue; }
    job.render();
  }
}

// --- VFS capacity snapshots ---------------------------------------------------
static unsigned invalidations = 0, invalidationsOffUiWorkers = 0;
namespace VFS {
enum Tier { INTERNAL, SDCARD };
struct CapacitySnapshot { uint64_t totalBytes; uint64_t freeBytes; };
inline bool getStatsSnapshot(Tier tier, CapacitySnapshot& out, uint32_t) {
  if (tier == SDCARD) return false;
  out.totalBytes = 1u << 20;
  out.freeBytes = 1u << 19;
  return true;
}
inline void invalidateStatsSnapshot(Tier) {
  ++invalidations;
  if (!uiWorker()) ++invalidationsOffUiWorkers;
}
}  // namespace VFS

// --- FileManager over a two-level directory model -----------------------------
static FileEntry entry(const char* name, bool folder, uint32_t size, uint16_t children = 0) {
  FileEntry e{};
  std::snprintf(e.name, sizeof(e.name), "%s", name);
  e.isFolder = folder;
  e.size = size;
  e.childCount = children;
  return e;
}
static std::vector<FileEntry> liveRoot, liveData, liveDeep;
static std::string deepPath;
static std::vector<FileEntry>& liveDir(const std::string& path) {
  if (path == "/data") return liveData;
  if (!deepPath.empty() && path == deepPath) return liveDeep;
  return liveRoot;
}
// FileEntry.name is FileManager's display copy, cut at FILE_MANAGER_MAX_NAME - 1
// bytes; the live directory keeps the full name (display -> full).
static std::vector<std::pair<std::string, std::string>> fullNames;
static FileEntry longEntry(const std::string& name, bool folder, uint32_t size,
                           uint16_t children = 0) {
  FileEntry e = entry(name.c_str(), folder, size, children);
  fullNames.emplace_back(e.name, name);
  return e;
}
static std::string fullName(const char* shown) {
  for (const auto& known : fullNames)
    if (known.first == shown) return known.second;
  return shown;
}
static bool selectedPathFails = false;  // the selection no longer resolves

static unsigned fmCalls = 0, fmWithoutFilesLock = 0, fmReentered = 0, rescans = 0;
static Task fmActiveTask = TASK_NONE;
static unsigned fmDepth = 0;
static void (*midReadHook)() = nullptr;  // fired once inside a tap's getItem

class FileManager {
 public:
  FileManager() = default;
  void setCountFolderChildren(bool) { Use use; }
  bool navigate(const char* path) { Use use; path_ = path; load(); return true; }
  const char* getCurrentPath() { Use use; return path_.c_str(); }
  int getItemCount() { Use use; return (int)cache_.size(); }
  bool getItem(int index, FileEntry& out) {
    Use use;
    if (midReadHook && currentTask == TASK_TAP) {
      void (*hook)() = midReadHook;
      midReadHook = nullptr;
      hook();
    }
    if (index < 0 || index >= (int)cache_.size()) return false;
    out = cache_[(size_t)index];
    return true;
  }
  bool refresh() { Use use; return true; }  // generation-gated: keeps the cache
  bool forceRescan() { Use use; ++rescans; load(); return true; }
  bool navigateUp() { Use use; path_ = "/"; load(); return true; }
  bool navigateInto() {
    Use use;
    if (selected_ < 0 || selected_ >= (int)cache_.size()) return false;
    path_ = (path_ == "/" ? "/" : path_ + "/") + cache_[(size_t)selected_].name;
    selected_ = 0;
    load();
    return true;
  }
  int getSelectedIndex() { Use use; return selected_; }
  void moveDown() { Use use; ++selected_; }
  void moveUp() { Use use; --selected_; }
  // Like FileManager::resolveSelectedPath: the selected entry's full path.
  bool getSelectedPath(String& out) {
    Use use;
    if (selectedPathFails || selected_ < 0 || selected_ >= (int)cache_.size()) return false;
    out.value = (path_ == "/" ? std::string() : path_) + "/" +
                fullName(cache_[(size_t)selected_].name);
    return true;
  }
  void resetForTest(const char* path = "/") { path_ = path; selected_ = 0; load(); }

 private:
  struct Use {
    Use() {
      ++fmCalls;
      if (!filesLockHeld()) ++fmWithoutFilesLock;
      if (fmDepth && fmActiveTask != currentTask) ++fmReentered;
      CHECK(criticalDepth == 0);
      if (!fmDepth) fmActiveTask = currentTask;
      ++fmDepth;
    }
    ~Use() { if (--fmDepth == 0) fmActiveTask = TASK_NONE; }
  };
  void load() { cache_ = liveDir(path_); }
  std::string path_ = "/";
  int selected_ = 0;
  std::vector<FileEntry> cache_;
};

enum class AllocPref { PreferPSRAM };
alignas(FileManager) static unsigned char fileManagerStorage[sizeof(FileManager)];
static FileManager* fileManagerInstance = nullptr;
inline void* ps_alloc(size_t size, AllocPref, const char*) {
  CHECK(size == sizeof(FileManager) && !fileManagerInstance);
  fileManagerInstance = reinterpret_cast<FileManager*>(fileManagerStorage);
  return fileManagerStorage;
}

// INSERT_PAGE_COMMON

// The text reader/viewer is covered by g2_files_psram_harness.cpp.
static unsigned textViews = 0;
static bool showTextFileViaWidget(bool) { noteDraw(); ++textViews; return true; }

// --- Production page ----------------------------------------------------------
// Public API (G2_Page_Files.h).
void g2BuildFilesInfo(char* out, size_t cap);
void g2ShowFilesMenu();
void g2FilesHandleTap(uint32_t idx);

// INSERT_FILES_PAGE

// --- Scenarios ----------------------------------------------------------------
static bool rowIs(size_t row, const char* text) {
  return row < shownRows.size() && shownRows[row] == text;
}
static bool rowHas(size_t row, const char* text) {
  return row < shownRows.size() && shownRows[row].find(text) != std::string::npos;
}
// Directory rows only: row 0 is the back row, which may carry a result.
static bool anyEntryRowHas(const char* text) {
  for (size_t row = 1; row < shownRows.size(); ++row)
    if (shownRows[row].find(text) != std::string::npos) return true;
  return false;
}

// Apps -> Files, then into /data: rows "<- Main Menu", ".. (up)", notes.bin,
// photo.bmp, log.txt, then `extraData` from row 5.
static void openFilesAtData(const std::vector<FileEntry>& extraData = {}) {
  liveRoot = {entry("data", true, 0, 3), entry("readme.txt", false, 10)};
  liveData = {entry("notes.bin", false, 2048), entry("photo.bmp", false, 4096),
              entry("log.txt", false, 100)};
  liveData.insert(liveData.end(), extraData.begin(), extraData.end());
  if (fileManagerInstance) fileManagerInstance->resetForTest();
  selectedPathFails = false;
  listAccepts = true;
  overlayActive = false;
  redraws.clear();
  submitted.clear();
  page = G2_HIJACK_PAGE_MAIN;
  OnTask tap(TASK_TAP);
  g2ShowFilesMenu();
  CHECK(page == G2_HIJACK_PAGE_FILES && rowHas(1, "/ data [3]"));
  g2FilesHandleTap(1);
  CHECK(rowIs(0, "<- Main Menu") && rowHas(1, ".. (up)") && rowHas(2, "notes.bin"));
  CHECK(rowHas(3, "photo.bmp") && rowHas(4, "log.txt"));
}

struct Marks {
  unsigned draws, delays, fmCalls, lockTakes, redraws, invalidations;
};
static Marks marks() {
  return {draws, delays, fmCalls, lockTakes, (unsigned)redraws.size(), invalidations};
}

// Delete notes.bin through the chooser and its confirm; returns the submit.
static Submitted deleteNotes() {
  OnTask tap(TASK_TAP);
  g2FilesHandleTap(2);  // notes.bin -> chooser: <- Files, name, Info, Rename, Delete
  CHECK(shownRows.size() == 5 && rowIs(4, "Delete"));
  g2FilesHandleTap(4);  // Delete -> confirm
  CHECK(rowIs(2, "Confirm Delete"));
  g2FilesHandleTap(2);  // Confirm Delete
  CHECK(submitted.size() == 1);
  CHECK(!submitted.empty() && submitted[0].line == "filedelete \"/data/notes.bin\" confirm");
  liveData.erase(liveData.begin());  // the command removed the file
  return submitted.empty() ? Submitted{} : submitted[0];
}

static void completeOnExecutor(const Submitted& sent, bool ok, const char* result) {
  OnTask exec(TASK_CMD_EXEC);
  CHECK(sent.done != nullptr);
  if (sent.done) sent.done(ok, result, sent.cookie, sent.user);
}

static void testCompletionQueuesResult() {
  openFilesAtData();
  const Submitted sent = deleteNotes();
  const Marks before = marks();
  completeOnExecutor(sent, true, "OK: Deleted file: /data/notes.bin");
  const bool completionDelayedExecutor = delays != before.delays;
  const bool completionDrewOnExecutor = draws != before.draws;
  const bool completionUsedFileManager = fmCalls != before.fmCalls;
  const bool completionTookFilesLock = lockTakes != before.lockTakes;
  const bool completionQueuedSubmitCookie =
      redraws.size() == 1 && redraws[0].from == TASK_CMD_EXEC &&
      redraws[0].cookie.menuGen == sent.cookie.menuGen &&
      redraws[0].cookie.seq == sent.cookie.seq &&
      redraws[0].cookie.targetPage == G2_HIJACK_PAGE_FILES;
  CHECK(!completionDelayedExecutor);
  CHECK(!completionDrewOnExecutor);
  CHECK(!completionUsedFileManager);
  CHECK(!completionTookFilesLock);
  CHECK(completionQueuedSubmitCookie);

  runApplier();
  const bool resultInBackRow = rowIs(0, "<- Deleted file: /data/notes.bin");
  CHECK(resultInBackRow);
  CHECK(!anyEntryRowHas("notes.bin") && rowHas(2, "photo.bmp"));
  CHECK(invalidations == before.invalidations + 2);

  // The result shows once; the next list has the plain back row again.
  OnTask tap(TASK_TAP);
  g2ShowFilesMenu();
  CHECK(rowIs(0, "<- Main Menu") && rowHas(2, "photo.bmp"));
}

static void testFailedResultKeepsCache() {
  openFilesAtData();
  const Submitted sent = deleteNotes();
  liveData.insert(liveData.begin(), entry("notes.bin", false, 2048));  // not deleted
  const unsigned rescansBefore = rescans, invalidationsBefore = invalidations;
  completeOnExecutor(sent, true, "Error: File no longer exists or access denied");
  runApplier();
  CHECK(rowHas(0, "<- Error: File no longer exists or access"));
  CHECK(shownRows.empty() || shownRows[0].size() == FILES_ROW_LEN - 1);
  CHECK(rescans == rescansBefore && invalidations == invalidationsBefore);
  CHECK(rowHas(2, "notes.bin"));
}

static void testStaleResultDropped() {
  openFilesAtData();
  const Submitted sent = deleteNotes();
  completeOnExecutor(sent, true, "OK: Deleted file: /data/notes.bin");
  const bool completionQueuedOneRedraw = redraws.size() == 1;
  CHECK(completionQueuedOneRedraw);
  void (*render)() = redraws.empty() ? nullptr : redraws[0].render;

  g2BumpMenuGen();  // the user navigated before the applier ran
  const unsigned drawsBefore = draws, dropsBefore = applierDropped;
  runApplier();
  CHECK(draws == drawsBefore && applierDropped == dropsBefore + (render ? 1u : 0u));
  if (render) {  // the page fences its own result as well
    OnTask applier(TASK_APPLIER);
    render();
  }
  const bool staleResultDrawn = draws != drawsBefore;
  CHECK(!staleResultDrawn);

  // The deletion is still picked up by the next list render.
  OnTask tap(TASK_TAP);
  g2ShowFilesMenu();
  CHECK(rowIs(0, "<- Main Menu") && !anyEntryRowHas("notes.bin") && rowHas(2, "photo.bmp"));
}

static void testRenameResultAfterKeyboard() {
  openFilesAtData();
  {
    OnTask tap(TASK_TAP);
    g2FilesHandleTap(2);  // chooser
    g2FilesHandleTap(3);  // Rename -> keyboard
    CHECK(keyboard.onCommit != nullptr && keyboard.initial &&
          std::strcmp(keyboard.initial, "notes.bin") == 0);
    g2BumpMenuGen();      // text entry finishing bumps before its callback
    if (keyboard.onCommit) keyboard.onCommit("renamed.bin");
  }
  CHECK(submitted.size() == 1 &&
        submitted[0].line == "filerename \"/data/notes.bin\" \"renamed.bin\"");
  if (submitted.empty()) return;
  liveData[0] = entry("renamed.bin", false, 2048);
  completeOnExecutor(submitted[0], true,
                     "OK: Renamed: /data/notes.bin -> /data/renamed.bin");
  runApplier();
  const bool renameResultShown = rowHas(0, "<- Renamed: /data/notes.bin -> /data/ren");
  CHECK(renameResultShown);
  CHECK(rowHas(2, "renamed.bin"));
}

static void showInfoForNotes() {
  OnTask tap(TASK_TAP);
  g2FilesHandleTap(2);  // chooser
  g2FilesHandleTap(2);  // Info
  CHECK(rowIs(0, "<- Files") && rowHas(1, "Name: notes.bin") && overlayActive);
}

// g2LensTickOverlay clears the overlay, then calls the page's callback.
static void expireOverlayOnMainLoop() {
  OnTask loop(TASK_MAIN_LOOP);
  overlayActive = false;
  CHECK(overlayExpired != nullptr);
  if (overlayExpired) overlayExpired(G2_OVERLAY_FILE_INFO);
}

static void testOverlayExpiryQueued() {
  openFilesAtData();
  showInfoForNotes();
  Marks before = marks();
  expireOverlayOnMainLoop();
  const bool expiryDrewOnMainLoop = draws != before.draws;
  const bool expiryUsedFileManager = fmCalls != before.fmCalls;
  const bool expiryQueuedRedraw = redraws.size() == 1 && redraws[0].from == TASK_MAIN_LOOP &&
                                  redraws[0].cookie.menuGen == menuGen;
  CHECK(!expiryDrewOnMainLoop);
  CHECK(!expiryUsedFileManager);
  CHECK(lockTakes == before.lockTakes);
  CHECK(expiryQueuedRedraw);
  runApplier();
  CHECK(rowIs(0, "<- Main Menu") && rowHas(2, "notes.bin"));

  // Navigation before the applier runs drops the queued redraw.
  showInfoForNotes();
  expireOverlayOnMainLoop();
  g2BumpMenuGen();
  before = marks();
  runApplier();
  CHECK(draws == before.draws);
  OnTask tap(TASK_TAP);
  g2FilesHandleTap(0);  // "<- Files" still returns to the list
  CHECK(page == G2_HIJACK_PAGE_FILES && rowIs(0, "<- Main Menu") && rowHas(2, "notes.bin"));
}

static void testViewerDoneQueued() {
  openFilesAtData();
  {
    OnTask tap(TASK_TAP);
    g2FilesHandleTap(3);  // photo.bmp -> chooser: <- Files, name, View, View Full, ...
    CHECK(rowIs(2, "View") && rowIs(3, "View Full"));
    viewerDone = nullptr;
    g2FilesHandleTap(2);  // View
  }
  CHECK(viewerPath == "/data/photo.bmp" && viewerDone != nullptr);
  Marks before = marks();
  {
    OnTask session(TASK_SESSION);
    if (viewerDone) viewerDone();
  }
  const bool viewerDoneDrewOnSession = draws != before.draws;
  const bool viewerDoneUsedFileManager = fmCalls != before.fmCalls;
  const bool viewerDoneQueuedRedraw = redraws.size() == 1 && redraws[0].from == TASK_SESSION &&
                                      redraws[0].cookie.menuGen == menuGen;
  CHECK(!viewerDoneDrewOnSession);
  CHECK(!viewerDoneUsedFileManager);
  CHECK(lockTakes == before.lockTakes);
  CHECK(viewerDoneQueuedRedraw);
  runApplier();
  CHECK(rowIs(0, "<- Main Menu") && rowHas(3, "photo.bmp"));

  // View Full, then navigation before the applier runs: dropped.
  {
    OnTask tap(TASK_TAP);
    g2FilesHandleTap(3);
    viewerDone = nullptr;
    g2FilesHandleTap(3);  // View Full
  }
  CHECK(viewerDone != nullptr);
  {
    OnTask session(TASK_SESSION);
    if (viewerDone) viewerDone();
  }
  g2BumpMenuGen();
  before = marks();
  runApplier();
  CHECK(draws == before.draws);
}

// The completion, the expiry and the viewer return arrive while a tap is in
// the middle of a FileManager read.
static Submitted pendingDelete;
static void deliverCallbacksMidRead() {
  { OnTask exec(TASK_CMD_EXEC);
    if (pendingDelete.done)
      pendingDelete.done(true, "OK: Deleted file: /data/notes.bin", pendingDelete.cookie,
                         pendingDelete.user); }
  { OnTask loop(TASK_MAIN_LOOP); if (overlayExpired) overlayExpired(G2_OVERLAY_FILE_INFO); }
  { OnTask session(TASK_SESSION); if (viewerDone) viewerDone(); }
}
static void testCallbacksDuringTapRead() {
  openFilesAtData();
  {
    OnTask tap(TASK_TAP);
    g2FilesHandleTap(3);  // photo.bmp chooser
    viewerDone = nullptr;
    g2FilesHandleTap(2);  // View: viewer up, its onDone captured
    g2ShowFilesMenu();    // back on the list
  }
  CHECK(viewerDone != nullptr);
  pendingDelete = deleteNotes();
  const unsigned reenteredBefore = fmReentered, contendedBefore = lockContended;
  midReadHook = deliverCallbacksMidRead;
  {
    OnTask tap(TASK_TAP);
    g2ShowFilesMenu();  // a tap's list render reads entries
  }
  CHECK(midReadHook == nullptr);
  const bool fileManagerReenteredFromOtherTask = fmReentered != reenteredBefore;
  CHECK(!fileManagerReenteredFromOtherTask);
  CHECK(lockContended == contendedBefore);
  runApplier();
  CHECK(rowIs(0, "<- Deleted file: /data/notes.bin") && !anyEntryRowHas("notes.bin"));
}

static void testInfoRowsAfterRefusedExpiry() {
  openFilesAtData();
  showInfoForNotes();
  listAccepts = false;  // the list after the overlay is refused
  expireOverlayOnMainLoop();
  runApplier();
  listAccepts = true;
  CHECK(rowHas(2, "Size: 2.0 KB"));
  const unsigned listsBefore = listsShown, drawsBefore = draws;
  {
    OnTask tap(TASK_TAP);
    g2FilesHandleTap(2);  // the visible "Size:" row
  }
  const bool infoRowOpenedEntry = listsShown != listsBefore || draws != drawsBefore;
  CHECK(!infoRowOpenedEntry);
  {
    OnTask tap(TASK_TAP);
    g2FilesHandleTap(0);  // "<- Files"
  }
  const bool infoBackReturnedToList =
      page == G2_HIJACK_PAGE_FILES && rowIs(0, "<- Main Menu") && rowHas(2, "notes.bin");
  CHECK(infoBackReturnedToList);
}

// A list shown by a tap before the queued expiry redraw runs supersedes it:
// the chooser opened afterwards stays up and keeps its meaning.
static void testQueuedListSupersededByTap() {
  openFilesAtData();
  showInfoForNotes();
  expireOverlayOnMainLoop();
  {
    OnTask tap(TASK_TAP);
    g2FilesHandleTap(0);  // "<- Files" -> list
    g2FilesHandleTap(2);  // notes.bin -> chooser
  }
  CHECK(rowIs(4, "Delete"));
  const unsigned drawsBefore = draws;
  runApplier();
  const bool supersededListRedrawn = draws != drawsBefore;
  CHECK(!supersededListRedrawn);
  OnTask tap(TASK_TAP);
  g2FilesHandleTap(4);  // still the chooser's Delete
  CHECK(rowIs(2, "Confirm Delete"));
  g2FilesHandleTap(0);  // Cancel
  CHECK(rowIs(0, "<- Main Menu"));
}

// The viewer-return redraw passed the applier's menuGen check, then waited for
// the lock while a tap opened the rename keyboard (which bumps menuGen). The
// list must not replace the keyboard.
static void openKeyboardFromListRows() {
  OnTask tap(TASK_TAP);
  g2FilesHandleTap(2);  // the /data list map routes row 2: notes.bin chooser
  g2FilesHandleTap(3);  // Rename -> keyboard
}
static void testQueuedListAfterKeyboardOpened() {
  openFilesAtData();
  {
    OnTask tap(TASK_TAP);
    g2FilesHandleTap(3);  // photo.bmp chooser
    viewerDone = nullptr;
    g2FilesHandleTap(2);  // View
  }
  {
    OnTask session(TASK_SESSION);
    if (viewerDone) viewerDone();
  }
  keyboard = TextEntryConfig{};
  beforeApplierLock = openKeyboardFromListRows;
  const unsigned drawsBefore = draws;
  runApplier();
  const bool keyboardOpened = keyboard.onCommit != nullptr;
  CHECK(keyboardOpened);
  const unsigned keyboardDraws = draws - drawsBefore;  // the chooser only
  const bool listReplacedKeyboard = keyboardDraws != 1;
  CHECK(!listReplacedKeyboard);
  beforeApplierLock = nullptr;
  if (keyboard.onCancel) {
    OnTask tap(TASK_TAP);
    keyboard.onCancel();
  }
  CHECK(rowIs(0, "<- Main Menu"));
}

// --- Action paths ------------------------------------------------------------
// A 70-byte SD name: FileEntry.name holds its first 63 bytes only.
static std::string longName() {
  const std::string name = "sensor-capture-" + std::string(51, 'x') + ".bin";
  CHECK(name.size() == 70);
  return name;
}
static bool anySubmittedStartsWith(const std::string& prefix) {
  for (const Submitted& sent : submitted)
    if (sent.line.compare(0, prefix.size(), prefix) == 0) return true;
  return false;
}

static void testLongNameActionsUseFullPath() {
  const std::string name = longName();
  openFilesAtData({longEntry(name, false, 4096)});
  OnTask tap(TASK_TAP);
  CHECK(rowHas(5, "sensor-capture-"));
  g2FilesHandleTap(5);  // chooser: <- Files, name, Info, Rename, Delete
  CHECK(shownRows.size() == 5 && rowIs(2, "Info") && rowIs(4, "Delete"));
  sniffPath.clear();
  g2FilesHandleTap(2);  // Info peeks at the file for the sealed-capture mark
  const bool longNameInfoSniffedFullPath = sniffPath == "/data/" + name;
  CHECK(longNameInfoSniffedFullPath);
  CHECK(rowHas(1, "Name: sensor-capture-"));

  g2FilesHandleTap(0);  // <- Files
  g2FilesHandleTap(5);
  g2FilesHandleTap(3);  // Rename keeps its 32-byte name limit
  CHECK(lastTextAsList == "Name too long - rename on web" && submitted.empty());

  g2FilesHandleTap(0);  // <- Back (exits to the main menu)
  g2ShowFilesMenu();
  g2FilesHandleTap(5);
  g2FilesHandleTap(4);  // Delete -> confirm
  CHECK(rowIs(2, "Confirm Delete"));
  g2FilesHandleTap(2);  // Confirm Delete
  const bool longNameDeletedByFullPath =
      submitted.size() == 1 && submitted[0].line == "filedelete \"/data/" + name + "\" confirm";
  CHECK(longNameDeletedByFullPath);
  const bool deletedTruncatedName = anySubmittedStartsWith(
      "filedelete \"/data/" + name.substr(0, FILE_MANAGER_MAX_NAME - 1) + "\"");
  CHECK(!deletedTruncatedName);
}

// A 123-byte folder path plus the 70-byte name does not fit the 160-byte
// action path: refused, nothing submitted. A short name there still fits.
static void testDeepLongPathRefused() {
  const std::string name = longName();
  openFilesAtData();
  deepPath = "/data/" + std::string(57, 'a') + "/" + std::string(59, 'b');
  CHECK(deepPath.size() == 123 && deepPath.size() < FILE_MANAGER_MAX_PATH);
  liveDeep = {longEntry(name, false, 100), entry("ok.bin", false, 10)};
  if (fileManagerInstance) fileManagerInstance->resetForTest(deepPath.c_str());
  OnTask tap(TASK_TAP);
  g2ShowFilesMenu();
  CHECK(rowHas(2, "sensor-capture-") && rowHas(3, "ok.bin"));
  lastTextAsList.clear();
  g2FilesHandleTap(2);  // chooser
  g2FilesHandleTap(4);  // Delete
  const bool deepLongPathRefused = lastTextAsList == "File path unavailable - delete on web";
  CHECK(deepLongPathRefused);
  g2FilesHandleTap(2);  // the refused path's row 2: never a Confirm Delete
  const bool deepLongPathSubmitted = anySubmittedStartsWith("filedelete");
  CHECK(!deepLongPathSubmitted);

  g2ShowFilesMenu();
  g2FilesHandleTap(3);  // ok.bin
  g2FilesHandleTap(4);  // Delete -> confirm
  g2FilesHandleTap(2);  // Confirm Delete
  CHECK(submitted.size() == 1 &&
        submitted[0].line == "filedelete \"" + deepPath + "/ok.bin\" confirm");
  deepPath.clear();
}

// The selection no longer resolves (entry gone or ambiguous): no viewer, no
// delete on a guessed path.
static void testUnresolvedPathRefused() {
  openFilesAtData();
  OnTask tap(TASK_TAP);
  g2FilesHandleTap(3);  // photo.bmp chooser
  selectedPathFails = true;
  viewerPath.clear();
  g2FilesHandleTap(2);  // View
  const bool unresolvedViewStarted = !viewerPath.empty();
  CHECK(!unresolvedViewStarted);
  CHECK(rowIs(0, "<- Main Menu") && rowHas(3, "photo.bmp"));
  g2FilesHandleTap(2);  // notes.bin chooser
  lastTextAsList.clear();
  g2FilesHandleTap(4);  // Delete
  g2FilesHandleTap(2);
  const bool unresolvedDeleteSubmitted = !submitted.empty();
  CHECK(!unresolvedDeleteSubmitted);
  CHECK(lastTextAsList == "File path unavailable - delete on web");
  selectedPathFails = false;
}

// Long folder names are cut so the "[n]" badge keeps its closing bracket.
static void testFolderBadgeClosed() {
  openFilesAtData();
  liveRoot = {entry(("photos-" + std::string(43, 'p')).c_str(), true, 0, 7),
              entry(("archive-" + std::string(42, 'q')).c_str(), true, 0, 120)};
  if (fileManagerInstance) fileManagerInstance->resetForTest();
  OnTask tap(TASK_TAP);
  g2ShowFilesMenu();
  auto endsWith = [](size_t row, const std::string& tail) {
    return row < shownRows.size() && shownRows[row].size() >= tail.size() &&
           shownRows[row].size() < FILES_ROW_LEN &&
           shownRows[row].compare(shownRows[row].size() - tail.size(), tail.size(), tail) == 0;
  };
  const bool folderBadgeClosed = endsWith(1, "~ [7]") && endsWith(2, "~ [99+]");
  CHECK(folderBadgeClosed);
  CHECK(rowHas(1, "/ photos-pppp") && rowHas(2, "/ archive-qqqq"));
}

static void testCliSummaryUsesLock() {
  openFilesAtData();
  char out[400];
  {
    OnTask exec(TASK_CMD_EXEC);
    g2BuildFilesInfo(out, sizeof(out));
  }
  CHECK(std::strstr(out, "@ /data") != nullptr && std::strstr(out, "3 items") != nullptr);
}

int main() {
  testCompletionQueuesResult();
  testFailedResultKeepsCache();
  testStaleResultDropped();
  testRenameResultAfterKeyboard();
  testOverlayExpiryQueued();
  testViewerDoneQueued();
  testCallbacksDuringTapRead();
  testInfoRowsAfterRefusedExpiry();
  testQueuedListSupersededByTap();
  testQueuedListAfterKeyboardOpened();
  testCliSummaryUsesLock();
  testLongNameActionsUseFullPath();
  testDeepLongPathRefused();
  testUnresolvedPathRefused();
  testFolderBadgeClosed();

  // Whole run: every FileManager call and lens draw from the page happened on
  // a task holding the Files page lock, no lock contention arose, the lens was
  // drawn only from the tap worker or the lens applier, and capacity was
  // invalidated only there.
  const bool fileManagerOutsideFilesLock = fmWithoutFilesLock != 0;
  const bool lensDrawOutsideFilesLock = drawsWithoutFilesLock != 0;
  const bool lensDrawOffUiWorkers = drawsOffUiWorkers != 0;
  CHECK(!fileManagerOutsideFilesLock);
  CHECK(!lensDrawOutsideFilesLock);
  CHECK(!lensDrawOffUiWorkers);
  CHECK(lockContended == 0 && contendedDepth == 0);
  CHECK(invalidationsOffUiWorkers == 0);
  CHECK(delays == 0);
  CHECK(filesLock.depth == 0 && criticalDepth == 0);
  if (failures) {
    std::fprintf(stderr, "G2 Files redraw: %u of %u checks FAILED "
                 "(FileManager calls without lock %u, lens draws without lock %u, "
                 "draws off UI workers %u, vTaskDelay %u)\n",
                 failures, checks, fmWithoutFilesLock, drawsWithoutFilesLock,
                 drawsOffUiWorkers, delays);
    return 1;
  }
  std::printf("G2 Files redraw: %u checks passed\n", checks);
  return 0;
}
