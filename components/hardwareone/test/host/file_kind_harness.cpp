// test_file_kind.py compiles this beside production code read from
// --component: FileBrowser_FileKind.h (when the tree has it), the G2 Files
// page's file-tap branch (the `else` block after `if (e.isFolder)` in
// g2FilesHandleTap, with the classification functions it calls) and the OLED
// browser's "View" decision (its `sFbActionViewable = ...;` assignment, with
// fbIsViewableExt on a pre-fix tree).
//
// One file must get the same treatment in both browsers: the lens offers a
// text/JSON view exactly when the OLED offers "View".
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if __has_include("FileBrowser_FileKind.h")
#include "FileBrowser_FileKind.h"
#define HAVE_FILE_KIND 1
#endif

static unsigned checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (false)

#define DEBUG_G2F(...) ((void)0)

// Arduino String, reduced to what a pre-fix fbIsViewableExt uses.
struct String {
  std::string value;
  String(const char* text) : value(text ? text : "") {}
  void toLowerCase() {
    for (char& c : value) c = (char)std::tolower((unsigned char)c);
  }
  bool operator==(const char* other) const { return value == other; }
};

// INSERT_FILE_ENTRY

// --- G2 lens: the file-tap branch of g2FilesHandleTap -----------------------
// INSERT_G2_CHOOSER_KIND
static FilesChooserKind chosenKind = FILE_CHOOSER_OTHER;
static int chooserCalls = 0;
static void showFileChooser(const FileEntry&, FilesChooserKind kind) {
  chosenKind = kind;
  ++chooserCalls;
}

// INSERT_G2_CLASSIFIERS

static FilesChooserKind lensKind(const char* name) {
  FileEntry e{};
  std::snprintf(e.name, sizeof(e.name), "%s", name);
  chooserCalls = 0;
  // INSERT_G2_FILE_TAP
  CHECK(chooserCalls == 1);
  return chosenKind;
}

// --- OLED browser: the action menu's "View" decision ------------------------
// INSERT_OLED_CLASSIFIERS

static bool oledViewable(const char* name) {
  FileEntry entry{};
  std::snprintf(entry.name, sizeof(entry.name), "%s", name);
  return INSERT_OLED_VIEWABLE;
}

// --- Scenarios ---------------------------------------------------------------
static const char* kindName(FilesChooserKind kind) {
  switch (kind) {
    case FILE_CHOOSER_BMP:   return "BMP";
    case FILE_CHOOSER_JSON:  return "JSON";
    case FILE_CHOOSER_JPG:   return "JPG";
    case FILE_CHOOSER_TEXT:  return "TEXT";
    case FILE_CHOOSER_OTHER: return "OTHER";
  }
  return "?";
}

// `name` gets chooser `lens` on the G2 and "View" == `oled` on the OLED, and
// the two browsers agree on whether it reads as text.
static void expectBrowsers(const char* name, FilesChooserKind lens, bool oled) {
  const FilesChooserKind got = lensKind(name);
  const bool view = oledViewable(name);
  ++checks;
  if (got != lens) {
    ++failures;
    std::fprintf(stderr, "FAIL %s: lens chooser %s, want %s\n", name, kindName(got), kindName(lens));
  }
  ++checks;
  if (view != oled) {
    ++failures;
    std::fprintf(stderr, "FAIL %s: OLED View %s, want %s\n", name,
                 view ? "offered" : "absent", oled ? "offered" : "absent");
  }
  ++checks;
  const bool lensReads = got == FILE_CHOOSER_TEXT || got == FILE_CHOOSER_JSON;
  if (lensReads != view) {
    ++failures;
    std::fprintf(stderr, "FAIL %s: browsers disagree: lens %s, OLED View %s\n", name,
                 kindName(got), view ? "offered" : "absent");
  }
}

static void browsersAgree() {
  // Text the OLED has always viewed; the lens must open it too.
  expectBrowsers("server.log", FILE_CHOOSER_TEXT, true);
  expectBrowsers("notes.md", FILE_CHOOSER_TEXT, true);
  expectBrowsers("boot.ini", FILE_CHOOSER_TEXT, true);
  expectBrowsers("wifi.cfg", FILE_CHOOSER_TEXT, true);
  expectBrowsers("mesh.conf", FILE_CHOOSER_TEXT, true);
  expectBrowsers("rules.yaml", FILE_CHOOSER_TEXT, true);
  expectBrowsers("rules.yml", FILE_CHOOSER_TEXT, true);
  expectBrowsers("layout.xml", FILE_CHOOSER_TEXT, true);
  expectBrowsers("index.html", FILE_CHOOSER_TEXT, true);
  expectBrowsers("app.js", FILE_CHOOSER_TEXT, true);
  expectBrowsers("main.c", FILE_CHOOSER_TEXT, true);
  expectBrowsers("main.h", FILE_CHOOSER_TEXT, true);
  expectBrowsers("main.cpp", FILE_CHOOSER_TEXT, true);
  expectBrowsers("Server.LOG", FILE_CHOOSER_TEXT, true);
  // Kinds both browsers already shared.
  expectBrowsers("readme.txt", FILE_CHOOSER_TEXT, true);
  expectBrowsers("README.TXT", FILE_CHOOSER_TEXT, true);
  expectBrowsers("readings.csv", FILE_CHOOSER_TEXT, true);
  expectBrowsers(".txt", FILE_CHOOSER_TEXT, true);
  expectBrowsers("settings.json", FILE_CHOOSER_JSON, true);
  expectBrowsers("Settings.JSON", FILE_CHOOSER_JSON, true);
  // Images: lens viewers; the mono OLED offers Rename/Delete only.
  expectBrowsers("photo.bmp", FILE_CHOOSER_BMP, false);
  expectBrowsers("photo.BMP", FILE_CHOOSER_BMP, false);
  expectBrowsers("snap.jpg", FILE_CHOOSER_JPG, false);
  expectBrowsers("snap.JPEG", FILE_CHOOSER_JPG, false);
  // No viewer on either device.
  expectBrowsers("photo.png", FILE_CHOOSER_OTHER, false);
  expectBrowsers("firmware.bin", FILE_CHOOSER_OTHER, false);
  expectBrowsers("archive.tar.gz", FILE_CHOOSER_OTHER, false);
  expectBrowsers("server.log.bak", FILE_CHOOSER_OTHER, false);
  expectBrowsers("route.hwmap", FILE_CHOOSER_OTHER, false);
  expectBrowsers("Makefile", FILE_CHOOSER_OTHER, false);
  expectBrowsers("txt", FILE_CHOOSER_OTHER, false);
  expectBrowsers("trailing.", FILE_CHOOSER_OTHER, false);
}

static void sharedClassifier() {
#if HAVE_FILE_KIND
  CHECK(fileKindForName(nullptr) == FileKind::Other);
  CHECK(fileKindForName("") == FileKind::Other);
  CHECK(fileKindForName("server.log") == FileKind::Text);
  CHECK(fileKindForName("NOTES.Md") == FileKind::Text);
  CHECK(fileKindForName("data.json") == FileKind::Json);
  CHECK(fileKindForName("a.bmp") == FileKind::ImageBmp);
  CHECK(fileKindForName("a.jpg") == FileKind::ImageJpeg);
  CHECK(fileKindForName("a.jpeg") == FileKind::ImageJpeg);
  CHECK(fileKindForName("a.jpe") == FileKind::Other);
  CHECK(fileKindForName("a.jsonx") == FileKind::Other);
  CHECK(fileKindForName("a.cc") == FileKind::Other);
  CHECK(fileKindForName("a.") == FileKind::Other);
  CHECK(fileKindForName("noext") == FileKind::Other);
  // Full paths: only the final segment's extension counts.
  CHECK(fileKindForName("/sd/logs/today.LOG") == FileKind::Text);
  CHECK(fileKindForName("/sd/logs.d/readme") == FileKind::Other);
  CHECK(fileKindForName("/sd/v1.2/") == FileKind::Other);
  CHECK(fileKindIsImage(FileKind::ImageBmp) && fileKindIsImage(FileKind::ImageJpeg));
  CHECK(!fileKindIsImage(FileKind::Text) && !fileKindIsImage(FileKind::Other));
  CHECK(fileKindIsReadableText(FileKind::Text) && fileKindIsReadableText(FileKind::Json));
  CHECK(!fileKindIsReadableText(FileKind::ImageBmp) && !fileKindIsReadableText(FileKind::Other));
#else
  ++checks;
  ++failures;
  std::fprintf(stderr, "FAIL FileBrowser_FileKind.h missing: no shared classifier for the two browsers\n");
#endif
}

int main() {
  browsersAgree();
  sharedClassifier();
  if (failures) {
    std::fprintf(stderr, "file kind: %u of %u checks failed\n", failures, checks);
    return 1;
  }
  std::printf("file kind: %u checks passed\n", checks);
  return 0;
}
