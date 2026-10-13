// test_automation_scheduler.py compiles the complete production G2 Automations
// page (G2_Page_Automations.cpp, includes stripped) beside lens, command and
// automations-file doubles. When the tree's automation owner provides the
// summary list, its real trigger parser and summary code are inserted too, so
// the lens shows exactly what the owner builds.
#include <Arduino.h>
#include <ArduinoJson.h>
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
void webBatchTestStringAppend() {}

#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
#define ENABLE_AUTOMATION 1
#define EXT_RAM_BSS_ATTR
#define PSRAM_JSON_DOC(name) JsonDocument name
template <typename... Args> void quietLog(const char*, Args...) {}
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)

// INSERT_BOUNDARY_TYPES

// --- automations.json, allocation and event-name boundaries ----------------
const char* AUTOMATIONS_JSON_FILE = "/system/automations.json";
static String store;
static bool storeReadable = true;
static unsigned reads = 0;
bool readText(const char* path, String& out) {
  ++reads;
  if (!storeReadable || std::strcmp(path, AUTOMATIONS_JSON_FILE) != 0) return false;
  out = store;
  return true;
}
enum class AllocPref : uint8_t { PreferPSRAM = 0 };
static int liveAllocations = 0;
void* ps_alloc(size_t size, AllocPref, const char*) { ++liveAllocations; return std::malloc(size); }
void ps_free(void* memory) { if (memory) --liveAllocations; std::free(memory); }
static constexpr size_t SYSEVT_DETAIL_LEN = 80;
static int systemEventKindFromName(const char* name) { return std::strcmp(name, "edge") == 0 ? 1 : -1; }
static const char* systemEventKindName(uint8_t kind) { return kind == 1 ? "edge" : "?"; }

// INSERT_OWNER_SUMMARY

// --- Lens, page and command boundaries -------------------------------------
static bool listAccepts = true;
static std::vector<std::string> shownRows;
static std::string shownText;
static G2HijackPage page = G2_HIJACK_PAGE_APPS;
static unsigned appsMenus = 0, menuGen = 0;
using Completion = void (*)(bool, const char*, const G2CmdCookie&, void*);
static std::vector<std::string> submitted;
static Completion lastDone = nullptr;
static void (*pendingRedraw)() = nullptr;

bool g2ShowListPage(const char* const* rows, size_t count) {
  if (!listAccepts) return false;
  shownRows.assign(rows, rows + count);
  return true;
}
bool g2ShowTextAsList(const char* text, const char* = nullptr) { shownText = text; return true; }
void g2BumpMenuGen() { ++menuGen; }
G2HijackPage g2GetHijackPage() { return page; }
void g2SetHijackPage(G2HijackPage value) { if (page != value) { page = value; ++menuGen; } }
void g2ShowAppsMenu() { ++appsMenus; page = G2_HIJACK_PAGE_APPS; }
bool g2SubmitHijackCommand(const char* line, const G2CmdCookie&, Completion done, void*) {
  submitted.emplace_back(line);
  lastDone = done;
  return true;
}
bool g2EnqueueRedraw(const G2CmdCookie&, void (*render)(), const char*) {
  pendingRedraw = render;
  return true;
}

// INSERT_LIST_PUBLISH_HELPER

// INSERT_AUTOMATIONS_PAGE

// --- Scenarios --------------------------------------------------------------
static bool shown(size_t row, const char* text) {
  return row < shownRows.size() && shownRows[row] == text;
}

static void openList(const char* json) {
  store = json;
  storeReadable = true;
  listAccepts = true;
  submitted.clear();
  lastDone = nullptr;
  pendingRedraw = nullptr;
  page = G2_HIJACK_PAGE_APPS;
  g2ShowAutomationsMenu();
  CHECK(page == G2_HIJACK_PAGE_AUTOMATIONS);
  CHECK(liveAllocations == 0);
}

static const char* kSchedules = R"({"automations":[
  {"id":11,"name":"Monthly","enabled":true,"triggers":[{"type":"time","time":"07:30","recurrence":"monthly","dayOfMonth":15}]},
  {"id":12,"name":"Mon Wed","enabled":true,"triggers":[{"type":"time","time":"08:00","days":"mon,wed"}]},
  {"id":13,"name":"Poll","enabled":false,"triggers":[{"type":"interval","intervalMs":300000},{"type":"event","on":"edge"}]}
]})";
static const char* kThree = R"({"automations":[
  {"id":1,"name":"A","enabled":true,"triggers":[{"type":"interval","intervalMs":60000}]},
  {"id":2,"name":"B","enabled":true,"triggers":[{"type":"interval","intervalMs":60000}]},
  {"id":3,"name":"C","enabled":true,"triggers":[{"type":"interval","intervalMs":60000}]}
]})";
static const char* kWithoutA = R"({"automations":[
  {"id":2,"name":"B","enabled":true,"triggers":[{"type":"interval","intervalMs":60000}]},
  {"id":3,"name":"C","enabled":true,"triggers":[{"type":"interval","intervalMs":60000}]}
]})";
static const char* kBDisabled = R"({"automations":[
  {"id":1,"name":"A","enabled":true,"triggers":[{"type":"interval","intervalMs":60000}]},
  {"id":2,"name":"B","enabled":false,"triggers":[{"type":"interval","intervalMs":60000}]},
  {"id":3,"name":"C","enabled":true,"triggers":[{"type":"interval","intervalMs":60000}]}
]})";
static const char* kWithoutB = R"({"automations":[
  {"id":1,"name":"A","enabled":true,"triggers":[{"type":"interval","intervalMs":60000}]},
  {"id":3,"name":"C","enabled":true,"triggers":[{"type":"interval","intervalMs":60000}]}
]})";

// The DETAIL trigger row describes the schedule the scheduler runs.
static void testTriggerSummaries() {
  openList(kSchedules);
  CHECK(shownRows.size() == 4 && shown(1, "[on]  Monthly") && shown(3, "[off] Poll"));
  g2AutomationsHandleTap(1);
  CHECK(shown(0, "<- Monthly"));
  const bool lensShowedDailyForMonthly = shown(1, "daily 07:30");
  CHECK(!lensShowedDailyForMonthly);
  CHECK(shown(1, "monthly 15 07:30"));
  g2AutomationsHandleTap(0);
  g2AutomationsHandleTap(2);
  CHECK(shown(0, "<- Mon Wed"));
  const bool lensShowedDailyForWeekdays = shown(1, "daily 08:00");
  CHECK(!lensShowedDailyForWeekdays);
  CHECK(shown(1, "weekly 08:00 MoWe"));
  g2AutomationsHandleTap(0);
  g2AutomationsHandleTap(3);
  CHECK(shown(0, "<- Poll") && shown(1, "every 5m +1") && shown(3, "Enable"));
  CHECK(liveAllocations == 0);
}

// A web delete between the list render and the tap: the tapped row still
// opens the automation it showed, and drilling in reads the file once.
static void testRowsKeepTheirAutomation() {
  openList(kThree);
  CHECK(shown(2, "[on]  B"));
  store = kWithoutA;
  reads = 0;
  g2AutomationsHandleTap(2);
  const bool tapOpenedDifferentAutomation = !shown(0, "<- B");
  CHECK(!tapOpenedDifferentAutomation);
  const unsigned drillInReads = reads;
  CHECK(drillInReads == 1);
  g2AutomationsHandleTap(2);
  CHECK(!submitted.empty() && submitted.back() == "automation run id=2");
}

// The Enable/Disable row submits what it reads, without re-reading the file,
// even when the web changed the automation after the render.
static void testToggleMatchesVisibleRow() {
  openList(kThree);
  g2AutomationsHandleTap(2);
  CHECK(shown(0, "<- B") && shown(3, "Disable"));
  store = kBDisabled;
  reads = 0;
  g2AutomationsHandleTap(3);
  const bool visibleDisableRowEnabled =
      !submitted.empty() && submitted.back() == "automation enable id=2";
  CHECK(!visibleDisableRowEnabled);
  CHECK(!submitted.empty() && submitted.back() == "automation disable id=2");
  const bool detailTapReadFile = reads != 0;
  CHECK(!detailTapReadFile);
  // The completion redraw shows the owner's current state.
  CHECK(lastDone != nullptr);
  pendingRedraw = nullptr;
  if (lastDone) lastDone(true, "OK", G2CmdCookie{}, nullptr);
  CHECK(pendingRedraw != nullptr);
  if (pendingRedraw) pendingRedraw();
  CHECK(shown(0, "<- B") && shown(3, "Enable"));
  g2AutomationsHandleTap(3);
  CHECK(submitted.back() == "automation enable id=2");
}

// Deleted while DETAIL is shown: the toggle redraw falls back to the list.
static void testDeletedWhileDetailShown() {
  openList(kThree);
  g2AutomationsHandleTap(2);
  store = kWithoutB;
  g2AutomationsHandleTap(3);
  CHECK(!submitted.empty() && submitted.back() == "automation disable id=2");
  pendingRedraw = nullptr;
  if (lastDone) lastDone(false, "Error: automation not found", G2CmdCookie{}, nullptr);
  if (pendingRedraw) pendingRedraw();
  CHECK(shownRows.size() == 3 && shown(1, "[on]  A") && shown(2, "[on]  C"));
  g2AutomationsHandleTap(2);
  CHECK(shown(0, "<- C"));
}

static void testEmptyAndUnreadable() {
  openList(R"({"automations":[]})");
  CHECK(shownRows.size() == 2 && shown(1, "(no automations)"));
  g2AutomationsHandleTap(1);
  CHECK(shownRows.size() == 2 && submitted.empty());
  const unsigned appsBefore = appsMenus;
  g2AutomationsHandleTap(0);
  CHECK(appsMenus == appsBefore + 1);
  storeReadable = false;
  page = G2_HIJACK_PAGE_APPS;
  g2ShowAutomationsMenu();
  CHECK(shownRows.size() == 2 && shown(1, "(no automations)"));
  CHECK(liveAllocations == 0);
}

int main() {
  testTriggerSummaries();
  testRowsKeepTheirAutomation();
  testToggleMatchesVisibleRow();
  testDeletedWhileDetailShown();
  testEmptyAndUnreadable();
  if (failures) {
    std::fprintf(stderr, "G2 automations page: %u of %u checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("G2 automations page: %u summary and row-identity checks passed\n", checks);
  return 0;
}
