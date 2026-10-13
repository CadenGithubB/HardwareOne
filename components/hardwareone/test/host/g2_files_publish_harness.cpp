// test_g2_page_publish.py compiles the production Files chooser, delete
// confirm, info overlay and tap dispatcher (G2_Page_Files.cpp) beside these
// boundaries. The g2ShowListPage double can refuse a render; a refused render
// leaves the previous rows on the lens, so taps must keep their old meaning.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static unsigned checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (false)

#define EXT_RAM_BSS_ATTR
template <typename... Args> void quietLog(const char*, Args...) {}
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)

// INSERT_BOUNDARY_TYPES
// INSERT_TEXT_ENTRY_TYPES
// INSERT_FILE_MANAGER

// Arduino String, reduced to what the extracted functions use.
struct String {
  std::string value;
  String() = default;
  String(const char* text) : value(text ? text : "") {}
  const char* c_str() const { return value.c_str(); }
  size_t length() const { return value.size(); }
};
static String operator+(const String& left, const char* right) {
  String out = left;
  out.value += right ? right : "";
  return out;
}

// --- Lens, page and command boundaries -------------------------------------
static bool listAccepts = true;
static unsigned listsShown = 0, listRefusals = 0, filesMenus = 0, overlayStarts = 0;
static std::vector<std::string> shownRows;
static bool overlayActive = false;
static G2HijackPage page = G2_HIJACK_PAGE_FILES;
static std::vector<std::string> submitted;

bool g2ShowListPage(const char* const* rows, size_t count) {
  if (!listAccepts) { ++listRefusals; return false; }
  ++listsShown;
  shownRows.assign(rows, rows + count);
  return true;
}
void g2LensClearOverlay() { overlayActive = false; }
void g2LensStartOverlay(G2OverlayKind kind, uint32_t ms) {
  CHECK(kind == G2_OVERLAY_FILE_INFO && ms > 0);
  overlayActive = true;
  ++overlayStarts;
}
bool g2LensInOverlay() { return overlayActive; }
G2HijackPage g2GetHijackPage() { return page; }
void g2SetHijackPage(G2HijackPage value) { page = value; }
uint32_t g2CurrentMenuGen() { return 0; }
void g2RedrawHijackMainMenu() {}
void g2ShowFilesMenu() { ++filesMenus; }
bool g2ShowTextAsList(const char*, const char*) { return true; }
bool g2ShowBmpFile(const char*, void (*)()) { return true; }
bool g2ShowBmpFileFullScreen(const char*, void (*)()) { return true; }
bool g2ShowJpgFile(const char*, void (*)()) { return true; }
bool g2ShowJpgFileFullScreen(const char*, void (*)()) { return true; }
bool g2BeginTextEntry(const TextEntryConfig&) { return true; }
using Completion = void (*)(bool, const char*, const G2CmdCookie&, void*);
bool g2SubmitHijackCommand(const char* line, const G2CmdCookie&, Completion, void*) {
  submitted.emplace_back(line);
  return true;
}

// Identity, permission and sealed-capture boundaries of showFileInfo.
struct G2HijackCtxGuard { G2HijackCtxGuard() {} };
static int currentAuthContext() { return 1; }
static bool canRead(const String&, int) { return true; }
static bool readTextLimited(const char*, String&, size_t) { return false; }
static bool captureCryptoIsMagicLine(const char*) { return false; }
#define CAPCRYPT_MAGIC_PREFIX "HW1CAP1"

// Files page lock and queued list redraw (g2_files_redraw_harness.cpp covers
// them); here every call runs on one task.
struct FilesPageLock { FilesPageLock() {} };
void filesQueueListRedraw() {}

// Directory side of the page: one folder level, no rows mapped.
struct FileManager {
  const char* getCurrentPath() const { return "/data"; }
  bool getItem(int, FileEntry&) const { return false; }
  int getSelectedIndex() const { return 0; }
  bool getSelectedPath(String& out) const { out = String("/data/notes.bin"); return true; }
  void moveDown() {}
  void moveUp() {}
  void navigateInto() {}
  void navigateUp() {}
};
static FileManager fileManager;
static FileManager* ensureFm() { return &fileManager; }

// Page state not used by the extracted functions (e.g. a pre-fix tree's
// completion banner) is fine here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
// INSERT_FILES_STATE
#pragma GCC diagnostic pop

static size_t gFilesRowCount = 0;
static int    gFilesEntryForRow[FILES_MAX_ROWS];
static size_t gFilesPage = 0;

static void onFilesMutateDone(bool, const char*, const G2CmdCookie&, void*) {}
static void filesRenameCommit(const char*) {}
static void filesRenameCancel() {}
static bool showTextFileViaWidget(bool) { return true; }
// File classification: a pre-fix tree calls its private predicates, the
// current page calls filesChooserKindFor (FileBrowser_FileKind.h). The
// scenarios never tap a list row, so whichever the tree uses stays unused.
[[maybe_unused]] static bool isBmpFilename(const char*) { return false; }
[[maybe_unused]] static bool isJpgFilename(const char*) { return false; }
[[maybe_unused]] static bool isJsonFilename(const char*) { return false; }
[[maybe_unused]] static bool isTxtFilename(const char*) { return false; }
[[maybe_unused]] static bool isCsvFilename(const char*) { return false; }
[[maybe_unused]] static FilesChooserKind filesChooserKindFor(const char*) {
  return FILE_CHOOSER_OTHER;
}

// INSERT_LIST_PUBLISH_HELPER

// INSERT_FILES_FUNCTIONS

// --- Scenarios --------------------------------------------------------------
static FileEntry file(const char* name) {
  FileEntry entry{};
  std::snprintf(entry.name, sizeof(entry.name), "%s", name);
  entry.size = 2048;
  return entry;
}

static bool shown(size_t row, const char* text) {
  return row < shownRows.size() && shownRows[row].find(text) != std::string::npos;
}

static bool anySubmitted(const char* prefix) {
  for (const auto& line : submitted)
    if (line.compare(0, std::strlen(prefix), prefix) == 0) return true;
  return false;
}

// The directory list is up: no chooser, confirm or overlay; no mapped rows.
static void directoryList() {
  listAccepts = true;
  submitted.clear();
  shownRows.clear();
  gFilesChooserActive = gFilesConfirmActive = false;
  overlayActive = false;
  gFilesRowCount = 0;
  page = G2_HIJACK_PAGE_FILES;
}

// "Info" / "Rename" / "Delete" for a file type without a viewer.
static void chooserUp() {
  directoryList();
  showFileChooser(file("notes.bin"), FILE_CHOOSER_OTHER);
  CHECK(shownRows.size() == 5 && shown(1, "notes.bin"));
  CHECK(shown(2, "Info") && shown(3, "Rename") && shown(4, "Delete"));
}

static void testRefusedDeleteConfirm() {
  chooserUp();
  listAccepts = false;
  const unsigned refusals = listRefusals;
  g2FilesHandleTap(4);  // Delete -> confirm list refused; chooser stays up.
  CHECK(listRefusals == refusals + 1 && shown(2, "Info"));
  listAccepts = true;
  g2FilesHandleTap(2);  // The visible row 2 is "Info".
  const bool refusedConfirmDeletedFile = anySubmitted("filedelete");
  CHECK(!refusedConfirmDeletedFile);
  CHECK(submitted.empty());
  CHECK(shown(1, "Name: notes.bin") && overlayActive);
}

static void testDeleteConfirmStillDeletes() {
  chooserUp();
  g2FilesHandleTap(4);
  CHECK(shownRows.size() == 3 && shown(1, "Delete notes.bin?") && shown(2, "Confirm Delete"));
  g2FilesHandleTap(2);
  CHECK(submitted.size() == 1 && submitted[0] == "filedelete \"/data/notes.bin\" confirm");
}

static void testRefusedChooser() {
  directoryList();
  const unsigned before = listsShown;
  listAccepts = false;
  showFileChooser(file("notes.bin"), FILE_CHOOSER_OTHER);
  listAccepts = true;
  g2FilesHandleTap(2);  // A directory row: unmapped here, so nothing happens.
  const bool refusedChooserRoutedDirectoryTap = listsShown != before || overlayActive;
  CHECK(!refusedChooserRoutedDirectoryTap);
  g2FilesHandleTap(4);
  CHECK(listsShown == before && submitted.empty());
}

static void testRefusedInfoOverlay() {
  chooserUp();
  const unsigned starts = overlayStarts;
  listAccepts = false;
  g2FilesHandleTap(2);  // Info -> info rows refused.
  const bool refusedInfoStartedOverlay = overlayStarts != starts || overlayActive;
  CHECK(!refusedInfoStartedOverlay);
  listAccepts = true;
  g2FilesHandleTap(4);  // The visible row 4 is still the chooser's Delete.
  CHECK(shownRows.size() == 3 && shown(2, "Confirm Delete"));
  CHECK(submitted.empty());
}

int main() {
  testRefusedDeleteConfirm();
  testDeleteConfirmStillDeletes();
  testRefusedChooser();
  testRefusedInfoOverlay();
  if (failures) {
    std::fprintf(stderr, "G2 Files publish: %u of %u checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("G2 Files publish: %u checks passed\n", checks);
  return 0;
}
