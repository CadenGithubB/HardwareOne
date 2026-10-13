// test_file_kind.py compiles the OLED file browser's rename/delete helpers
// (FbMutateResult, fbSubmitMutate, fbRenameSelected, fbDeleteSelected from
// OLED_Mode_FileBrowser.cpp) beside the doubles below.
//
// Rename and delete must go through the command executor as the admin-only
// `filerename` / `filedelete` commands (the lens' path), naming the selected
// entry's full path that FileManager resolves (not the bounded display name);
// re-scan only after the executor's "OK"; submit nothing for an unresolved
// selection or a quote the command line cannot carry.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

static unsigned checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (false)

// Arduino String, reduced to what the helpers use.
struct String {
  std::string value;
  String() = default;
  String(const char* text) : value(text ? text : "") {}
  String(std::string text) : value(std::move(text)) {}
  const char* c_str() const { return value.c_str(); }
  size_t length() const { return value.size(); }
};
static String operator+(const String& a, const String& b) { return String(a.value + b.value); }
static String operator+(const String& a, const char* b) { return String(a.value + b); }

// FileManager: the selection resolves to `selectedPath` unless `resolves` is off.
struct FileManager {
  bool resolves = true;
  std::string selectedPath;
  int resolveCalls = 0;
  int rescans = 0;
  bool getSelectedPath(String& out) {
    ++resolveCalls;
    if (!resolves) return false;
    out = String(selectedPath);
    return true;
  }
  bool forceRescan() { ++rescans; return true; }
};
static FileManager fileManager;
static FileManager* gOledFileManager = &fileManager;

// Shared executor (OLED_Utils.cpp): records each line and answers as scripted.
static std::vector<std::string> submitted;
static bool execOk = true;
static std::string execOut;
static bool executeOLEDCommandWithResult(const String& line, char* out, size_t outSize) {
  submitted.push_back(line.value);
  std::snprintf(out, outSize, "%s", execOut.c_str());
  return execOk;
}

// INSERT_OLED_FILE_COMMANDS

// --- Scenarios ---------------------------------------------------------------
static void reset(const std::string& path, bool ok, const std::string& out) {
  fileManager = FileManager{};
  fileManager.selectedPath = path;
  submitted.clear();
  execOk = ok;
  execOut = out;
}

// A 70-byte SD name: FileEntry.name holds only its first 63 bytes.
static const std::string kLongName =
    "field-recording-2026-10-13-north-ridge-weather-station-hourly-01.log";
static const std::string kLongPath = "/sd/Field Notes/" + kLongName;

static void renameUsesSelectedPath() {
  CHECK(kLongName.size() > 63);
  reset(kLongPath, true, "OK: Renamed: " + kLongPath + " -> /sd/Field Notes/ridge.log");
  CHECK(fbRenameSelected(String("ridge.log")) == FbMutateResult::Done);
  CHECK(fileManager.resolveCalls == 1);
  CHECK(submitted.size() == 1 &&
        submitted[0] == "filerename \"" + kLongPath + "\" \"ridge.log\"");
  CHECK(fileManager.rescans == 1);

  reset("/notes/a.txt", true, "OK: Renamed: /notes/a.txt -> /notes/my notes.txt");
  CHECK(fbRenameSelected(String("my notes.txt")) == FbMutateResult::Done);
  CHECK(submitted.size() == 1 && submitted[0] == "filerename \"/notes/a.txt\" \"my notes.txt\"");
}

static void deleteUsesOneShotCommand() {
  reset(kLongPath, true, "OK: Deleted file: " + kLongPath);
  CHECK(fbDeleteSelected() == FbMutateResult::Done);
  CHECK(fileManager.resolveCalls == 1);
  CHECK(submitted.size() == 1 && submitted[0] == "filedelete \"" + kLongPath + "\" confirm");
  CHECK(fileManager.rescans == 1);
}

static void executorRefusals() {
  // The executor's admin gate (filerename / filedelete are admin-only).
  reset("/notes/a.txt", false,
        "Error: Admin access required for command 'filedelete'. Contact an administrator.");
  CHECK(fbDeleteSelected() == FbMutateResult::AdminOnly);
  CHECK(submitted.size() == 1 && fileManager.rescans == 0);

  reset("/notes/a.txt", false,
        "Error: Admin access required for command 'filerename'. Contact an administrator.");
  CHECK(fbRenameSelected(String("b.txt")) == FbMutateResult::AdminOnly);
  CHECK(fileManager.rescans == 0);

  // Guest refusal and handler errors are plain failures.
  reset("/notes/a.txt", false, "Error: Guest accounts are view-only. Only local login/logout and whoami are allowed.");
  CHECK(fbDeleteSelected() == FbMutateResult::Failed);
  CHECK(fileManager.rescans == 0);
  reset("/system/users/users.json", false,
        "Error: Failed to delete file (denied or fs error): /system/users/users.json");
  CHECK(fbDeleteSelected() == FbMutateResult::Failed);
  CHECK(fileManager.rescans == 0);
  // A replaced display session gets no result.
  reset("/notes/a.txt", false, "");
  CHECK(fbRenameSelected(String("b.txt")) == FbMutateResult::Failed);
  CHECK(fileManager.rescans == 0);
  // Success is the executor's "OK" stamp, not the ok flag alone.
  reset("/notes/a.txt", true, "Error: File does not exist or access denied");
  CHECK(fbDeleteSelected() == FbMutateResult::Failed);
  CHECK(fileManager.rescans == 0);
}

static void refusedBeforeSubmit() {
  reset("/notes/a.txt", true, "OK: unexpected");
  fileManager.resolves = false;
  CHECK(fbRenameSelected(String("b.txt")) == FbMutateResult::Unresolved);
  CHECK(fbDeleteSelected() == FbMutateResult::Unresolved);
  CHECK(submitted.empty() && fileManager.rescans == 0);

  reset("/notes/say \"hi\".txt", true, "OK: unexpected");
  CHECK(fbRenameSelected(String("b.txt")) == FbMutateResult::QuoteInName);
  CHECK(fbDeleteSelected() == FbMutateResult::QuoteInName);
  CHECK(submitted.empty());

  reset("/notes/a.txt", true, "OK: unexpected");
  CHECK(fbRenameSelected(String("b\" \"c.txt")) == FbMutateResult::QuoteInName);
  CHECK(submitted.empty() && fileManager.rescans == 0);
}

int main() {
  renameUsesSelectedPath();
  deleteUsesOneShotCommand();
  executorRefusals();
  refusedBeforeSubmit();
  if (failures) {
    std::fprintf(stderr, "OLED file commands: %u of %u checks failed\n", failures, checks);
    return 1;
  }
  std::printf("OLED file commands: %u checks passed\n", checks);
  return 0;
}
