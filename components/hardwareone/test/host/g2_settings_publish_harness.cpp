// test_g2_page_publish.py compiles the production Settings page state, row
// builders, edit/commit machinery, menu entry and tap dispatcher
// (G2_Page_Settings.cpp up to its JSON view) beside these boundaries. The
// g2ShowListPage double can refuse a render; a refused render leaves the
// previous rows on the lens, so taps must keep their old meaning.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

static unsigned checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (false)

#define EXT_RAM_BSS_ATTR
template <typename... Args> void quietLog(const char*, Args...) {}
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) ((m)->lock())
#define portEXIT_CRITICAL(m) ((m)->unlock())
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(unsigned) {}

// Arduino String, reduced to what the extracted code uses.
struct String {
  std::string value;
  size_t length() const { return value.size(); }
  const char* c_str() const { return value.c_str(); }
  int indexOf(char c) const {
    const auto at = value.find(c);
    return at == std::string::npos ? -1 : (int)at;
  }
};

// INSERT_BOUNDARY_TYPES
// INSERT_TEXT_ENTRY_TYPES
// INSERT_SETTINGS_TYPES
// INSERT_PAGINATOR

// --- Lens, page and command boundaries -------------------------------------
static bool listAccepts = true;
static unsigned listsShown = 0, listRefusals = 0, menuGen = 0;
static std::vector<std::string> shownRows;
static G2HijackPage page = G2_HIJACK_PAGE_MAIN;

bool g2ShowListPage(const char* const* rows, size_t count) {
  if (!listAccepts) { ++listRefusals; return false; }
  ++listsShown;
  shownRows.assign(rows, rows + count);
  return true;
}
bool g2ShowText(const char*) { return true; }
void g2BumpMenuGen() { ++menuGen; }
G2HijackPage g2GetHijackPage() { return page; }
void g2SetHijackPage(G2HijackPage value) { if (page != value) { page = value; ++menuGen; } }
void g2ShowConfigMenu() { page = G2_HIJACK_PAGE_MAIN; }
bool g2BeginTextEntry(const TextEntryConfig&) { return true; }

using Completion = void (*)(bool, const char*, const G2CmdCookie&, void*);
struct Command { std::string line; G2CmdCookie cookie; Completion done; void* userData; };
static std::vector<Command> commands;
bool g2SubmitHijackCommand(const char* line, const G2CmdCookie& cookie, Completion done,
                           void* userData) {
  G2CmdCookie captured = cookie;
  captured.menuGen = menuGen;
  commands.push_back({line, captured, done, userData});
  return true;
}
static std::vector<void (*)()> redraws;
bool g2EnqueueRedraw(const G2CmdCookie& cookie, void (*render)(), const char*) {
  if (cookie.menuGen != menuGen) return true;  // dropped as stale by the worker
  redraws.push_back(render);
  return true;
}

// --- Settings registry and editor-core doubles ------------------------------
// "alpha": Flag (bool), Mode (enum Off/On/Auto), Level (int, keyboard).
// "big": twelve bools B0..B11, so entries span two pages.
static bool flag = false;
static int mode = 0, level = 5;
static bool big[12] = {};
static char bigKeys[12][8];
static SettingEntry alphaEntries[3], bigEntries[12];
static SettingsModule alpha, bigModule;
static const SettingsModule* modules[2] = {&alpha, &bigModule};

static SettingEntry entry(const char* key, SettingType type, void* value, const char* options) {
  SettingEntry e{};
  e.jsonKey = key;
  e.type = type;
  e.valuePtr = value;
  e.label = key;
  e.options = options;
  return e;
}

static void registerModules() {
  alphaEntries[0] = entry("flag", SETTING_BOOL, &flag, nullptr);
  alphaEntries[1] = entry("mode", SETTING_INT, &mode, "0|Off,1|On,2|Auto");
  alphaEntries[2] = entry("level", SETTING_INT, &level, nullptr);
  alpha.name = "alpha"; alpha.entries = alphaEntries; alpha.count = 3;
  for (int i = 0; i < 12; ++i) {
    std::snprintf(bigKeys[i], sizeof(bigKeys[i]), "b%d", i);
    bigEntries[i] = entry(bigKeys[i], SETTING_BOOL, &big[i], nullptr);
  }
  bigModule.name = "big"; bigModule.entries = bigEntries; bigModule.count = 12;
}

const SettingsModule** getSettingsModules(size_t& count) { count = 2; return modules; }
bool settingsEditorIsVisible(const SettingEntry*) { return true; }
int settingsEditorCurrentValue(const SettingEntry* e) {
  return e->type == SETTING_BOOL ? (*(bool*)e->valuePtr ? 1 : 0) : *(int*)e->valuePtr;
}
bool settingsEditorHasEnumOptions(const SettingEntry* e) { return e->options != nullptr; }
// "value|Label" pairs separated by commas.
bool settingsEditorEnumAt(const char* options, int idx, char* valueOut, size_t valueCap,
                          char* labelOut, size_t labelCap) {
  const char* item = options;
  for (int i = 0; item && i < idx; ++i) {
    item = std::strchr(item, ',');
    if (item) ++item;
  }
  if (!item || !*item) return false;
  const char* bar = std::strchr(item, '|');
  const char* end = std::strchr(item, ',');
  if (!end) end = item + std::strlen(item);
  if (!bar || bar > end) return false;
  if (valueOut) std::snprintf(valueOut, valueCap, "%.*s", (int)(bar - item), item);
  if (labelOut) std::snprintf(labelOut, labelCap, "%.*s", (int)(end - bar - 1), bar + 1);
  return true;
}
int settingsEditorEnumCount(const char* options) {
  int n = 0;
  while (settingsEditorEnumAt(options, n, nullptr, 0, nullptr, 0)) ++n;
  return n;
}
int settingsEditorEnumIndexForCurrent(const SettingEntry* e) { return settingsEditorCurrentValue(e); }
const char* settingsEditorCommandName(const SettingEntry* e) { return e->jsonKey; }
bool settingsEditorHasCommand(const SettingEntry*) { return true; }

static bool showModuleJsonViaTextWidget(int) { return false; }

// INSERT_LIST_PUBLISH_HELPER

// INSERT_SETTINGS_PAGE

// --- Scenarios --------------------------------------------------------------
static bool shown(size_t row, const char* text) {
  return row < shownRows.size() && shownRows[row].find(text) != std::string::npos;
}

static std::string lastCommand() { return commands.empty() ? "" : commands.back().line; }

static void completeAll(bool ok, const char* result) {
  while (!commands.empty()) {
    Command command = commands.front();
    commands.erase(commands.begin());
    command.done(ok, result, command.cookie, command.userData);
  }
  const auto queued = redraws;
  redraws.clear();
  for (auto render : queued) render();
}

static void openModule(unsigned row, const char* firstEntry) {
  completeAll(true, "OK");
  listAccepts = true;
  page = G2_HIJACK_PAGE_MAIN;
  g2ShowSettingsMenu();
  CHECK(page == G2_HIJACK_PAGE_SETTINGS);
  CHECK(shown(0, "<- Config") && shown(2, "[alpha]") && shown(3, "[big]"));
  g2SettingsHandleTap(row);
  CHECK(shown(0, "<- Settings") && shown(1, firstEntry));
}

// Enum entry -> pick list refused: "Flag" on the lens must still flip Flag,
// never commit pick option 0 to Mode.
static void testRefusedPickList() {
  openModule(2, "flag=false");
  CHECK(shown(2, "mode=0"));
  listAccepts = false;
  g2SettingsHandleTap(2);
  CHECK(shown(2, "mode=0"));
  listAccepts = true;
  g2SettingsHandleTap(1);
  const bool refusedPickCommittedEnumOption = lastCommand().compare(0, 5, "mode ") == 0;
  CHECK(!refusedPickCommittedEnumOption);
  CHECK(lastCommand() == "flag 1");
}

// Cancel refused while the pick list stays visible: its options still commit.
static void testRefusedPickCancel() {
  openModule(2, "flag=");
  g2SettingsHandleTap(2);
  CHECK(shown(0, "<- Cancel") && shown(2, "[ ] On"));
  listAccepts = false;
  g2SettingsHandleTap(0);
  listAccepts = true;
  g2SettingsHandleTap(2);
  CHECK(lastCommand() == "mode 1");
}

// "Next page" refused: row 1 is still B0 on the lens.
static void testRefusedPageTurn() {
  openModule(3, "b0=false");
  CHECK(shown(10, "b9=false") && shown(11, "Next page"));
  listAccepts = false;
  g2SettingsHandleTap(11);
  listAccepts = true;
  g2SettingsHandleTap(1);
  const bool refusedPageTurnEditedHiddenEntry = lastCommand() == "b10 1";
  CHECK(!refusedPageTurnEditedHiddenEntry);
  CHECK(lastCommand() == "b0 1");
}

// Back to the module list refused: the visible entries keep editing alpha.
static void testRefusedBackToModules() {
  openModule(2, "flag=");
  listAccepts = false;
  g2SettingsHandleTap(0);
  listAccepts = true;
  g2SettingsHandleTap(1);
  CHECK(lastCommand() == "flag 1");
}

// A refused failure redraw keeps the failure label for the next entries view.
static void testRefusedFailureRedraw() {
  openModule(2, "flag=");
  g2SettingsHandleTap(1);
  CHECK(lastCommand() == "flag 1");
  listAccepts = false;
  completeAll(false, "Error: denied");
  listAccepts = true;
  g2SettingsHandleTap(2);  // Mode -> pick list
  CHECK(shown(0, "<- Cancel"));
  g2SettingsHandleTap(0);  // back to entries
  CHECK(shown(0, "Failed: denied"));
  g2SettingsHandleTap(2);
  g2SettingsHandleTap(0);
  CHECK(shown(0, "<- Settings"));
}

// Entry refused from another page: the page id is not claimed.
static void testRefusedEntry() {
  completeAll(true, "OK");
  page = G2_HIJACK_PAGE_MAIN;
  listAccepts = false;
  g2ShowSettingsMenu();
  listAccepts = true;
  CHECK(page == G2_HIJACK_PAGE_MAIN);
}

int main() {
  registerModules();
  testRefusedPickList();
  testRefusedPickCancel();
  testRefusedPageTurn();
  testRefusedBackToModules();
  testRefusedFailureRedraw();
  testRefusedEntry();
  completeAll(true, "OK");
  if (failures) {
    std::fprintf(stderr, "G2 Settings publish: %u of %u checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("G2 Settings publish: %u checks passed\n", checks);
  return 0;
}
