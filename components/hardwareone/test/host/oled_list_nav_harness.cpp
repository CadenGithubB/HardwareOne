// Host harness for test_oled_list_nav.py. The driver splices in, from the
// production sources:
//   * OLED_Mode_Automations.cpp: AUTO_LIST_MAX, struct AutomationRenderData,
//     the autoRenderData declaration, autoListUp/autoListDown (and any
//     file-scope debounce state those functions use on the tree under test);
//   * OLED_Mode_FileBrowser.cpp: enum class FileBrowserPendingAction, the
//     pending-action slot, oledFileBrowserUp/Down/Select/Back (and any debounce
//     state they use);
//   * System_Settings.h / OLED_SettingsEditor.h: the settings and editor types;
//   * OLED_SettingsEditor.cpp: the OLED predicate wrappers, the module source,
//     initSettingsEditor, displaySettingsEditor, settingsEditorDown/Select/Back
//     and openSettingsEditorForModule.
// The clock, FileManager, the settings registry, the settings-editor core, the
// keyboard, the commit path and the display are doubles. The display records
// what each frame draws.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#define EXT_RAM_BSS_ATTR
#define ENABLE_ESPNOW 0
#define ENABLE_BONDED_MODE 0
#define DEBUG_SYSTEMF(...) ((void)0)
#define OLED_CONTENT_START_Y 11  // 10 px header + 1 px gap
#define OLED_CONTENT_HEIGHT 43   // 64 - 10 header - 10 footer - 1
#define DISPLAY_COLOR_WHITE 1
#define DISPLAY_COLOR_BLACK 0

// ---- checks -------------------------------------------------------------------
static int gFailures = 0;
static const char* gCase = "";
#define EXPECT(cond, msg)                                                         \
  do {                                                                            \
    if (!(cond)) {                                                                \
      std::fprintf(stderr, "FAIL [%s] %s  (%s, line %d)\n", gCase,                \
                   std::string(msg).c_str(), #cond, __LINE__);                    \
      ++gFailures;                                                                \
    }                                                                             \
  } while (0)

// ---- Arduino doubles --------------------------------------------------------
static unsigned long gNowMs = 10000;
static unsigned long millis() { return gNowMs; }

class String {
 public:
  String() = default;
  String(const char* text) : value_(text ? text : "") {}
  String(const std::string& text) : value_(text) {}
  template <class T, typename std::enable_if<std::is_integral<T>::value &&
                                                 !std::is_same<T, bool>::value &&
                                                 !std::is_same<T, char>::value,
                                             int>::type = 0>
  explicit String(T number) : value_(std::to_string(number)) {}
  String& operator=(const char* text) { value_ = text ? text : ""; return *this; }
  String& operator+=(char c) { value_ += c; return *this; }
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  String substring(size_t from, size_t to) const {
    if (from > value_.size()) return String();
    return String(value_.substr(from, to - from));
  }
 private:
  std::string value_;
};

// ---- recording display ------------------------------------------------------
struct TextOp { int x, y; std::string text; };
class Adafruit_SSD1306 {
 public:
  std::vector<TextOp> text;
  std::vector<int> barYs;  // selection bars (fillRect)
  void setTextSize(int) {}
  void setTextColor(uint16_t) {}
  void setTextColor(uint16_t, uint16_t) {}
  void setCursor(int x, int y) { cx_ = x; cy_ = y; }
  void print(const char* value) { add(value ? value : ""); }
  void print(const String& value) { add(value.c_str()); }
  void print(int value) { add(std::to_string(value)); }
  void println(const char* value) { print(value); cx_ = 0; cy_ += 8; }
  void println(const String& value) { print(value); cx_ = 0; cy_ += 8; }
  void fillRect(int, int y, int, int, uint16_t) { barYs.push_back(y); }
  void clear() { text.clear(); barYs.clear(); }
  bool has(const std::string& value) const {
    for (const auto& op : text) if (op.text == value) return true;
    return false;
  }
 private:
  void add(const std::string& value) {
    if (!value.empty()) text.push_back({cx_, cy_, value});
    cx_ += 6 * static_cast<int>(value.size());
  }
  int cx_ = 0, cy_ = 0;
};
static Adafruit_SSD1306 gDisplay;
static Adafruit_SSD1306* oledDisplay = &gDisplay;

// ============================================================================
// Automations list
// ============================================================================
struct AutomationSummary { long id; bool enabled; };  // rows are opaque here
struct AutoListItem { long id; bool enabled; };       // the saved tree's row type

// INSERT_AUTOMATIONS

// ============================================================================
// File list
// ============================================================================
class FileManager {
 public:
  int ups = 0, downs = 0;
  void moveUp() { ++ups; }
  void moveDown() { ++downs; }
};
static FileManager gFileManagerDouble;
FileManager* gOledFileManager = &gFileManagerDouble;

// INSERT_FILE_BROWSER

// ============================================================================
// Settings editor
// ============================================================================
// INSERT_SETTINGS_TYPES
// INSERT_EDITOR_TYPES

SettingsEditorContext gSettingsEditor;

// Settings-editor core double: an entry is editable unless read-only or
// secret; no entry carries enum options in these cases.
#define SETTINGS_EDIT_MASK_OLED 0u
static bool settingsEditorIsVisible(const SettingEntry*) { return true; }
static bool settingsEditorIsEditable(const SettingEntry* entry, uint32_t) {
  return entry && !entry->readOnly && !entry->isSecret;
}
static bool settingsEditorHasEnumOptions(const SettingEntry*) { return false; }
static int settingsEditorEnumCount(const char*) { return 0; }
static bool settingsEditorEnumAt(const char*, int, char*, size_t, char*, size_t) { return false; }
static int settingsEditorEnumIndexForCurrent(const SettingEntry*) { return 0; }

// Registry double.
static const SettingsModule** gModules = nullptr;
static size_t gModuleCount = 0;
const SettingsModule** getSettingsModules(size_t& count) { count = gModuleCount; return gModules; }

// Keyboard, value read/commit and slider doubles (not under test).
#define OLED_KEYBOARD_MAX_LENGTH 128
enum class OLEDKeyboardDictationPolicy : uint8_t { DENY = 0, ALLOW_PLAINTEXT = 1 };
static bool oledKeyboardDrawIfActive(Adafruit_SSD1306*) { return false; }
static void oledKeyboardInit(const char*, const char*, int, OLEDKeyboardDictationPolicy) {}
int getSettingCurrentValue(const SettingEntry* entry) { return entry ? entry->intDefault : 0; }
static bool setSettingValueStr(const SettingEntry*, const String&) { return true; }
bool setSettingValue(const SettingEntry*, int) { return true; }
bool validateSettingValue(const SettingEntry*, int, String&) { return true; }
void drawSettingsSlider(Adafruit_SSD1306*, int, int, int, int, bool) {}
void initSettingsEditor();

// INSERT_EDITOR

// ============================================================================
// Cases
// ============================================================================
static void automationListNav() {
  gCase = "automations: one row per nav event";
  autoRenderData = AutomationRenderData();
  autoRenderData.count = 3;  // rows 0..2 plus the virtual "+ New" row (index 3)
  autoRenderData.valid = true;
  // A briskly turned ANO wheel: one nav event per frame, 40 ms apart.
  for (int step = 1; step <= 3; ++step) {
    gNowMs += 40;
    autoListDown();
    EXPECT(autoRenderData.selectedIdx == step,
           "Down 40 ms after the previous nav event did not move (row " +
               std::to_string(autoRenderData.selectedIdx) + ", expected " + std::to_string(step) + ")");
    autoRenderData.selectedIdx = step;  // keep later steps independent of one failure
  }
  gNowMs += 40;
  autoListDown();
  EXPECT(autoRenderData.selectedIdx == 3, "Down past the \"+ New\" row moved the cursor");
  for (int step = 2; step >= 0; --step) {
    gNowMs += 40;
    autoListUp();
    EXPECT(autoRenderData.selectedIdx == step,
           "Up 40 ms after the previous nav event did not move (row " +
               std::to_string(autoRenderData.selectedIdx) + ", expected " + std::to_string(step) + ")");
    autoRenderData.selectedIdx = step;
  }
  gNowMs += 40;
  autoListUp();
  EXPECT(autoRenderData.selectedIdx == 0, "Up above the first row moved the cursor");
  // Two events in the same millisecond (two frames inside one tick) both count.
  autoListDown();
  autoListDown();
  EXPECT(autoRenderData.selectedIdx == 2, "two nav events in the same tick did not move two rows");
  autoRenderData.count = 0;
  autoRenderData.selectedIdx = 0;
  gNowMs += 500;
  autoListUp();
  autoListDown();
  EXPECT(autoRenderData.selectedIdx == 0, "an empty list moved past the \"+ New\" row");
}

static void fileListNav() {
  gCase = "files: one step per nav event or press";
  gFileManagerDouble = FileManager();
  fileBrowserPendingAction = FileBrowserPendingAction::NONE;
  gNowMs += 1000;
  for (int step = 1; step <= 3; ++step) {
    gNowMs += 40;
    oledFileBrowserDown();
    EXPECT(gFileManagerDouble.downs == step,
           "Down 40 ms after the previous nav event did not move (" +
               std::to_string(gFileManagerDouble.downs) + " of " + std::to_string(step) + " moves)");
    gFileManagerDouble.downs = step;
  }
  gNowMs += 40;
  oledFileBrowserUp();
  EXPECT(gFileManagerDouble.ups == 1, "Up 40 ms after a Down did not move");
  gNowMs += 40;
  oledFileBrowserSelect();
  EXPECT(fileBrowserPendingAction == FileBrowserPendingAction::NAVIGATE_INTO,
         "A 40 ms after a move did not request opening the selected entry");
  fileBrowserPendingAction = FileBrowserPendingAction::NONE;  // the prepare pass ran
  gNowMs += 40;
  oledFileBrowserBack();
  EXPECT(fileBrowserPendingAction == FileBrowserPendingAction::NAVIGATE_BACK,
         "B 40 ms after A did not request the parent folder");

  gCase = "files: no file manager";
  gOledFileManager = nullptr;
  fileBrowserPendingAction = FileBrowserPendingAction::NONE;
  gNowMs += 1000;
  oledFileBrowserDown();
  oledFileBrowserSelect();
  EXPECT(fileBrowserPendingAction == FileBrowserPendingAction::NONE,
         "a press without a file manager requested navigation");
  gOledFileManager = &gFileManagerDouble;
}

// ---- settings editor ---------------------------------------------------------
static int gIntValue = 0;
static SettingEntry editableEntry(const char* key, const char* label) {
  SettingEntry entry{};
  entry.jsonKey = key;
  entry.type = SETTING_INT;
  entry.valuePtr = &gIntValue;
  entry.intDefault = 1;
  entry.minVal = 0;
  entry.maxVal = 9;
  entry.label = label;
  return entry;
}

static int arrowCount(const char* glyph) {
  int count = 0;
  for (const auto& op : gDisplay.text) if (op.text == glyph) ++count;
  return count;
}

static void render() {
  gDisplay.clear();
  displaySettingsEditor();
}

static void expectRows(const std::vector<const char*>& shown, const std::vector<const char*>& hidden,
                       bool upArrow, bool downArrow, const char* when) {
  for (const char* label : shown) EXPECT(gDisplay.has(label), std::string(when) + ": row " + label + " not drawn");
  for (const char* label : hidden) EXPECT(!gDisplay.has(label), std::string(when) + ": row " + label + " drawn");
  EXPECT((arrowCount("\x18") == 1) == upArrow,
         std::string(when) + (upArrow ? ": up arrow missing" : ": up arrow drawn"));
  EXPECT((arrowCount("\x19") == 1) == downArrow,
         std::string(when) + (downArrow ? ": down arrow missing" : ": down arrow drawn although every row is shown"));
}

static void settingsEditorWindows() {
  SettingEntry hiddenEntries[2] = {editableEntry("counterA", "Counter A"), editableEntry("counterB", "Counter B")};
  hiddenEntries[0].readOnly = true;
  hiddenEntries[1].isSecret = true;
  SettingEntry fourEntries[4] = {editableEntry("alpha", "Alpha"), editableEntry("bravo", "Bravo"),
                                 editableEntry("charlie", "Charlie"), editableEntry("delta", "Delta")};
  SettingEntry sixEntries[6] = {editableEntry("s0", "Six0"), editableEntry("s1", "Six1"),
                                editableEntry("s2", "Six2"), editableEntry("s3", "Six3"),
                                editableEntry("s4", "Six4"), editableEntry("s5", "Six5")};
  SettingsModule readOnlyModule{};
  readOnlyModule.name = "status";
  readOnlyModule.entries = hiddenEntries;
  readOnlyModule.count = 2;
  SettingsModule fourModule{};
  fourModule.name = "four";
  fourModule.entries = fourEntries;
  fourModule.count = 4;
  SettingsModule sixModule{};
  sixModule.name = "six";
  sixModule.entries = sixEntries;
  sixModule.count = 6;
  const SettingsModule* modules[] = {&readOnlyModule, &fourModule, &sixModule};
  gModules = modules;
  gModuleCount = 3;

  gCase = "settings editor: a module's window does not carry into the next";
  initSettingsEditor();
  render();
  settingsEditorSelect();  // "status": no OLED-editable row
  EXPECT(gSettingsEditor.state == SETTINGS_ITEM_SELECT, "A did not open the first module");
  render();
  settingsEditorBack();
  render();
  settingsEditorDown();
  settingsEditorSelect();  // "four": exactly one screen of rows
  EXPECT(gSettingsEditor.currentModule == &fourModule, "A did not open the second module");
  render();
  expectRows({"Alpha", "Bravo", "Charlie", "Delta"}, {}, false, false,
             "4-row module opened after a module without editable rows");
  for (int step = 0; step < 3; ++step) settingsEditorDown();
  render();
  expectRows({"Alpha", "Bravo", "Charlie", "Delta"}, {}, false, false, "4-row module, cursor on the last row");

  gCase = "settings editor: the window follows the cursor";
  settingsEditorBack();
  settingsEditorDown();
  settingsEditorSelect();  // "six"
  EXPECT(gSettingsEditor.currentModule == &sixModule, "A did not open the third module");
  render();
  expectRows({"Six0", "Six1", "Six2", "Six3"}, {"Six4", "Six5"}, false, true, "6-row module opened");
  for (int step = 0; step < 5; ++step) settingsEditorDown();
  render();
  expectRows({"Six2", "Six3", "Six4", "Six5"}, {"Six0", "Six1"}, true, false, "6-row module, cursor on the last row");

  gCase = "settings editor: a scrolled module does not shift the next one";
  settingsEditorBack();
  render();
  settingsEditorDown();  // wraps to "status"
  settingsEditorDown();  // "four"
  settingsEditorSelect();
  render();
  expectRows({"Alpha", "Bravo", "Charlie", "Delta"}, {}, false, false, "4-row module after a scrolled module");

  gCase = "settings editor: opening a module directly";
  EXPECT(openSettingsEditorForModule("status"), "openSettingsEditorForModule(\"status\") failed");
  render();
  settingsEditorBack();
  render();
  settingsEditorDown();  // "four"
  settingsEditorSelect();
  render();
  expectRows({"Alpha", "Bravo", "Charlie", "Delta"}, {}, false, false,
             "4-row module after opening a module without editable rows directly");
  gModules = nullptr;
  gModuleCount = 0;
}

int main() {
  automationListNav();
  fileListNav();
  settingsEditorWindows();
  if (gFailures) {
    std::fprintf(stderr, "oled list nav: %d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("oled list nav: automations, files, settings editor windows\n");
  return 0;
}
