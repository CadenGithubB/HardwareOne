// test_g2_network_async_redraw.py compiles the production Network page state,
// renderers, background tasks and tap handlers, the redraw helper, the lens
// job types and the lens worker's Redraw arm beside these host boundaries.
// Each case runs in its own process: `harness <case>`; `harness --list`.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); } } while (false)
#define EXPECT(condition, message) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: FAILED: %s\n  (%s)\n", __FILE__, __LINE__, \
               message, #condition); \
  std::abort(); } } while (false)

// Ownership accounting for production's two nothrow allocations per redraw.
static void* owned[32]{};
static unsigned liveAllocations = 0;
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  void* pointer = std::malloc(size);
  CHECK(pointer != nullptr);
  for (auto& slot : owned) if (!slot) {
    slot = pointer; ++liveAllocations; return pointer;
  }
  std::abort();
}
void operator delete(void* pointer) noexcept {
  if (!pointer) return;
  for (auto& slot : owned) if (slot == pointer) {
    slot = nullptr; --liveAllocations; break;
  }
  std::free(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }
#if defined(__cpp_sized_deallocation)
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
#endif

#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
#define ENABLE_WIFI 1
#define ENABLE_HTTP_SERVER 0
#define ENABLE_ESPNOW 0
#define EXT_RAM_BSS_ATTR
template <typename... Args> void quietLog(const char*, Args...) {}
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)
#define BROADCAST_PRINTF(...) quietLog(__VA_ARGS__)
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
static unsigned criticalDepth = 0;
#define portENTER_CRITICAL(m) ((m)->lock(), ++criticalDepth)
#define portEXIT_CRITICAL(m) (--criticalDepth, (m)->unlock())
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
#define pdFAIL 0
#define APP_CORE 1
#define WIFI_AUTH_OPEN 0

struct PageSwapArgs;
struct ToastSpec;
struct NotifySpec;
struct CustomSpec;
struct NativeNotifSpec;
// INSERT_TYPES

class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& value) : std::string(value) {}
  String substring(size_t from, size_t to) const { return String(substr(from, to - from)); }
};
static String operator+(const String& left, const char* right) {
  return String(static_cast<const std::string&>(left) + right);
}

// ---- Clock, scripted events and task context ---------------------------------
// True while a spawned task body (watchdog / scan worker) is running.
static bool inTask = false;
static uint32_t now = 1000;
uint32_t millis() { return now; }
struct Event { uint32_t at; void (*run)(); bool done; };
static Event events[8]{};
static size_t eventCount = 0;
static void at(uint32_t when, void (*run)()) {
  CHECK(eventCount < 8);
  events[eventCount++] = Event{when, run, false};
}
static void advance(uint32_t ms) {
  CHECK(criticalDepth == 0);
  now += ms;
  for (size_t i = 0; i < eventCount; ++i) {
    if (!events[i].done && (int32_t)(now - events[i].at) >= 0) {
      events[i].done = true;
      const bool wasInTask = inTask;   // events are user input, not the task
      inTask = false;
      events[i].run();
      inTask = wasInTask;
    }
  }
}
void vTaskDelay(uint32_t ticks) { advance(ticks); }

// ---- Lens, hijack page and menu generation -----------------------------------
static uint32_t menuGen = 1;
static G2HijackPage page = G2_HIJACK_PAGE_MAIN;
static unsigned listsShown = 0, paintsFromTask = 0, paintsOffNetwork = 0;
// Page-swap admission: a list accepted inside one lens-applier job keeps its
// swap in flight until its PageSwap job (queued behind that job) runs, so the
// production g2ShowListPage refuses a second list render in the same job.
static bool inApplierJob = false;
static unsigned listsThisJob = 0;
static char shownRows[12][80]{};
static size_t shownCount = 0;

uint32_t g2CurrentMenuGen() { return menuGen; }
void g2BumpMenuGen() { ++menuGen; }
G2HijackPage g2GetHijackPage() { return page; }
void g2SetHijackPage(G2HijackPage value) {
  if (value != page) g2BumpMenuGen();
  page = value;
}
bool g2ShowListPage(const char* const* rows, size_t count) {
  CHECK(criticalDepth == 0);
  CHECK(count <= 12);
  if (inApplierJob && listsThisJob > 0) return false;
  if (inApplierJob) ++listsThisJob;
  if (inTask) ++paintsFromTask;
  if (page != G2_HIJACK_PAGE_NETWORK) ++paintsOffNetwork;
  shownCount = count;
  for (size_t i = 0; i < count; ++i)
    std::snprintf(shownRows[i], sizeof(shownRows[i]), "%s", rows[i]);
  ++listsShown;
  return true;
}
static bool cardSucceeds = true;
static unsigned cardShows = 0, cardHides = 0;
bool g2ShowEvenAIReplyNoAsk(const char*) {
  CHECK(criticalDepth == 0);
  ++cardShows;
  return cardSucceeds;
}
bool g2HideEvenAICard() { ++cardHides; return true; }

// ---- Commands, lens queue and tasks -------------------------------------------
using Callback = void (*)(bool, const char*, const G2CmdCookie&, void*);
static char lastCommand[64]{};
static unsigned submits = 0;
bool g2SubmitHijackCommand(const char* line, const G2CmdCookie&, Callback, void*) {
  CHECK(!inTask && criticalDepth == 0);
  std::snprintf(lastCommand, sizeof(lastCommand), "%s", line);
  ++submits;
  return true;
}

static LensUiJob* jobs[16]{};
static size_t jobCount = 0;
bool g2EnqueueLensJob(LensUiJob* job, G2LensEnqueueWait wait) {
  CHECK(criticalDepth == 0);
  CHECK(wait == G2LensEnqueueWait::Bounded50Ms);
  CHECK(job != nullptr && job->kind == LensJobKind::Redraw);
  CHECK(job->payload.redraw && job->payload.redraw->render);
  CHECK(jobCount < 16);
  jobs[jobCount++] = job;
  return true;
}

struct Spawned { void (*run)(void*); char name[24]; };
static Spawned spawned[4]{};
static size_t spawnedCount = 0;
static bool taskCreateFails = false;
static void taskStackRecord(const char*, int) {}
static int xTaskCreatePinnedToCore(void (*run)(void*), const char* name, int, void*,
                                   int, void*, int) {
  if (taskCreateFails) return pdFAIL;
  CHECK(spawnedCount < 4);
  spawned[spawnedCount].run = run;
  std::snprintf(spawned[spawnedCount].name, sizeof(spawned[spawnedCount].name), "%s", name);
  ++spawnedCount;
  return pdPASS;
}
static void vTaskDelete(void*) {}

// ---- WiFi, ring and settings boundaries --------------------------------------
static bool radioOn = true, wifiWillConnect = false;
static uint32_t wifiConnectAt = 0;
static void wifiConnectsAt(uint32_t when) { wifiWillConnect = true; wifiConnectAt = when; }
struct FakeWiFi {
  bool isConnected() const {
    return wifiWillConnect && (int32_t)(now - wifiConnectAt) >= 0;
  }
  String SSID() const { return String("HomeNet"); }
} WiFi;
static bool wifiRadioOn() { return radioOn; }
struct { bool wifiEnabled = true; bool wifiAutoStart = false; } gSettings;
struct WifiNetworkSnapshot { int count = 0; };

static bool ringClientActive = true;
static bool ringWillConnect = false;
static uint32_t ringConnectAt = 0, ringInFlightUntil = 0;
static void ringConnectsAt(uint32_t when) { ringWillConnect = true; ringConnectAt = when; }
static bool isG2ClientInitialized() { return ringClientActive; }
static bool g2RingIsConnected() {
  return ringWillConnect && (int32_t)(now - ringConnectAt) >= 0;
}
static bool g2RingConnectInFlight() {
  return !g2RingIsConnected() && (int32_t)(now - ringInFlightUntil) < 0;
}
enum { BLE_PEER_R1_RING = 1 };
static bool blePeerAutoReconnectEnabled(int) { return false; }

static void (*duringScan)() = nullptr;
static unsigned scans = 0;
WifiScanResult wifiScanForEach(bool includeHidden, WifiScanVisitor visitor,
                               void* context, uint32_t acquireTimeoutMs) {
  CHECK(includeHidden && acquireTimeoutMs == 0 && criticalDepth == 0);
  ++scans;
  if (duringScan) duringScan();
  advance(3000);  // the synchronous scan
  static const char* ssids[] = {"HomeNet", "Cafe", "Lab"};
  WifiScanRecord record{};
  for (uint16_t i = 0; i < 3; ++i) {
    std::snprintf(record.ssid, sizeof(record.ssid), "%s", ssids[i]);
    record.rssi = (int8_t)(-40 - 10 * i);
    record.authMode = i == 1 ? WIFI_AUTH_OPEN : 3;
    if (!visitor(record, i, 3, context)) break;
  }
  WifiScanResult result{};
  result.status = WifiScanStatus::OK;
  result.found = 3;
  result.delivered = 3;
  return result;
}
const char* wifiScanStatusText(WifiScanStatus) { return "busy"; }

// INSERT_REDRAW_HELPER

// INSERT_STATE

// Network views this suite does not exercise: each is a plain navigation that
// moves gNetSub (and so the menu generation) and draws a list.
static void showRow(NetworkSubMode sub, const char* title) {
  setNetSub(sub);
  const char* rows[] = {"<- Back", title};
  g2ShowListPage(rows, 2);
}
static void g2ShowNetworkMenu() { showRow(NET_SUB_MAIN, "Network"); }
static void showWiFiStatusPage() { showRow(NET_SUB_WIFI_STATUS, "Status"); }
static void showWiFiSavedList() { showRow(NET_SUB_WIFI_SAVED, "Saved"); }
static void showHttpMenu() { showRow(NET_SUB_HTTP, "HTTP"); }
static void showEspNowMenu() { showRow(NET_SUB_ESPNOW, "ESP-NOW"); }
static void showBluetoothMenu() { showRow(NET_SUB_BLUETOOTH, "Bluetooth"); }

// INSERT_FUNCTIONS

// ---- Drivers -------------------------------------------------------------------
// The lens applier: drains the queue through the production Redraw arm.
static void drainUi() {
  const bool wasInTask = inTask;
  inTask = false;
  while (jobCount) {
    LensUiJob* job = jobs[0];
    for (size_t i = 1; i < jobCount; ++i) jobs[i - 1] = jobs[i];
    --jobCount;
    const uint32_t liveGen = g2CurrentMenuGen();
    inApplierJob = true;
    listsThisJob = 0;
    switch (job->kind) {
      // INSERT_REDRAW_DISPATCH
      default: std::abort();
    }
    inApplierJob = false;
    CHECK(job->payload.redraw == nullptr);
    delete job;
  }
  CHECK(liveAllocations == 0);
  inTask = wasInTask;
}

static void runSpawned() {
  while (spawnedCount) {
    Spawned task = spawned[0];
    for (size_t i = 1; i < spawnedCount; ++i) spawned[i - 1] = spawned[i];
    --spawnedCount;
    inTask = true;
    task.run(nullptr);
    inTask = false;
  }
}

static bool rowHas(size_t row, const char* text) {
  return row < shownCount && std::strstr(shownRows[row], text) != nullptr;
}
static size_t rowOf(const char* text) {
  for (size_t i = 0; i < shownCount; ++i)
    if (std::strstr(shownRows[i], text)) return i;
  std::fprintf(stderr, "row '%s' not on the lens\n", text);
  std::abort();
}
static bool spawnedTask(const char* name) {
  return spawnedCount == 1 && std::strcmp(spawned[0].name, name) == 0;
}

// The global hold opens Notes (G2_Page_Notes: g2SetHijackPage(NOTES)).
static void holdOpensNotes() { g2SetHijackPage(G2_HIJACK_PAGE_NOTES); }

static void enterNetwork() {
  page = G2_HIJACK_PAGE_MAIN;
  g2SetHijackPage(G2_HIJACK_PAGE_NETWORK);
}

static void enterWifiMenu() {
  enterNetwork();
  showWiFiMenu();
  CHECK(gNetSub == NET_SUB_WIFI && rowHas(rowOf("WiFi:"), "Disconnected"));
}

static void tapWifiConnect() {
  enterWifiMenu();
  handleWiFiTap((uint32_t)rowOf("WiFi:"));
  CHECK(std::strcmp(lastCommand, "openwifi") == 0);
  CHECK(rowHas(rowOf("WiFi:"), "Pending..."));
  CHECK(spawnedTask("g2_wifi_pending"));
}

static void tapRingConnect() {
  enterNetwork();
  showBluetoothR1Menu();
  CHECK(gNetSub == NET_SUB_BLUETOOTH_R1 && rowHas(1, "Ring: disconnected"));
  handleBluetoothR1Tap(3);
  CHECK(std::strcmp(lastCommand, "ringconnect") == 0);
  CHECK(rowHas(1, "Ring: connecting..."));
  CHECK(spawnedTask("g2_ring_pending"));
}

static void tapScan() {
  enterWifiMenu();
  handleWiFiTap((uint32_t)rowOf("Scan Networks"));
}

static void finish() {
  drainUi();
  CHECK(jobCount == 0 && spawnedCount == 0 && criticalDepth == 0);
  CHECK(liveAllocations == 0);
}

// ---- Cases that the base tree fails ----------------------------------------------
static void wifiWatchdogAfterHold() {
  tapWifiConnect();
  holdOpensNotes();       // a held note starts within the 10 s overlay
  wifiConnectsAt(now + 1500);
  const unsigned before = listsShown;
  runSpawned();
  drainUi();
  EXPECT(paintsOffNetwork == 0,
         "WiFi pending watchdog painted the WiFi menu over Notes");
  EXPECT(paintsFromTask == 0, "WiFi pending watchdog drew from its own task");
  EXPECT(listsShown == before && page == G2_HIJACK_PAGE_NOTES,
         "Notes must stay the current view");
  CHECK(wifiPendingOverlayDeadline() == 0 && !gWifiPendingTaskActive);
  finish();
}

static void ringWatchdogAfterHold() {
  tapRingConnect();
  // The connect waits behind the G2 capture of a ring-held note, then lands.
  ringInFlightUntil = now + 20000;
  ringConnectsAt(now + 20000);
  at(now + 2000, holdOpensNotes);
  const unsigned before = listsShown;
  runSpawned();
  drainUi();
  EXPECT(paintsOffNetwork == 0,
         "R1 pending watchdog painted the R1 menu over Notes when the ring connected");
  EXPECT(paintsFromTask == 0, "R1 pending watchdog drew from its own task");
  EXPECT(listsShown == before && page == G2_HIJACK_PAGE_NOTES,
         "Notes must stay the current view");
  CHECK(gRingPendingDeadlineMs == 0 && !gRingPendingTaskActive);
  finish();
}

static void scanAfterHold() {
  tapScan();
  CHECK(spawnedTask("g2_net_scan"));
  at(now + 1500, holdOpensNotes);   // during the scan
  const unsigned before = listsShown;
  runSpawned();
  drainUi();
  EXPECT(paintsOffNetwork == 0, "scan worker painted scan results over Notes");
  EXPECT(paintsFromTask == 0, "scan worker drew from its own task");
  EXPECT(listsShown == before && page == G2_HIJACK_PAGE_NOTES,
         "Notes must stay the current view");
  CHECK(gScanCacheCount == 3 && scans == 1);   // results still published
  finish();
}

static void scanPlaceholderAfterHold() {
  cardSucceeds = false;
  tapScan();
  duringScan = [] {
    drainUi();            // the placeholder is on the lens
    CHECK(rowHas(1, "Scanning"));
    holdOpensNotes();
  };
  runSpawned();
  const unsigned before = listsShown;
  drainUi();
  EXPECT(paintsOffNetwork == 0,
         "scan worker painted results over Notes after its placeholder");
  EXPECT(paintsFromTask == 0, "scan worker drew its placeholder from its own task");
  EXPECT(listsShown == before && page == G2_HIJACK_PAGE_NOTES,
         "Notes must stay the current view");
  finish();
}

static void scanPlaceholderLeftBeforeDrawn() {
  cardSucceeds = false;
  tapScan();
  duringScan = holdOpensNotes;   // the placeholder is still queued
  runSpawned();
  const unsigned before = listsShown;
  drainUi();
  EXPECT(paintsOffNetwork == 0, "scan worker painted scan results over Notes");
  EXPECT(listsShown == before && page == G2_HIJACK_PAGE_NOTES,
         "neither the queued placeholder nor the results reach the lens");
  finish();
}

static void scanLeftDuringSettle() {
  tapScan();
  at(now + 200, holdOpensNotes);   // inside the 400 ms settle window
  runSpawned();
  drainUi();
  EXPECT(paintsOffNetwork == 0, "scan worker painted scan results over Notes");
  EXPECT(cardShows == 0, "scan worker opened its scanning card after the user left");
  CHECK(page == G2_HIJACK_PAGE_NOTES && scans == 1);
  finish();
}

// ---- Behaviour kept (base and fixed pass) ---------------------------------------
static void wifiWatchdogOnMenu() {
  tapWifiConnect();
  wifiConnectsAt(now + 1500);
  const unsigned before = listsShown;
  runSpawned();
  drainUi();
  EXPECT(listsShown == before + 1 && rowHas(rowOf("WiFi:"), "HomeNet"),
         "WiFi menu repaints with the SSID when the link comes up");
  CHECK(gNetSub == NET_SUB_WIFI && page == G2_HIJACK_PAGE_NETWORK);
  CHECK(wifiPendingOverlayDeadline() == 0 && !gWifiPendingTaskActive);
  finish();
}

static void wifiWatchdogExpiryOnMenu() {
  tapWifiConnect();
  const unsigned before = listsShown;
  runSpawned();
  drainUi();
  EXPECT(listsShown == before + 1 && rowHas(rowOf("WiFi:"), "Disconnected"),
         "an expired overlay repaints the live state");
  CHECK(wifiPendingOverlayDeadline() == 0 && !gWifiPendingTaskActive);
  finish();
}

static void wifiWatchdogReentry() {
  tapWifiConnect();
  handleWiFiTap((uint32_t)rowOf("Status"));     // leave the WiFi menu ...
  CHECK(gNetSub == NET_SUB_WIFI_STATUS);
  showWiFiMenu();                               // ... and come back, still pending
  CHECK(rowHas(rowOf("WiFi:"), "Pending..."));
  wifiConnectsAt(now + 1500);
  const unsigned before = listsShown;
  runSpawned();
  drainUi();
  EXPECT(listsShown == before + 1 && rowHas(rowOf("WiFi:"), "HomeNet"),
         "a re-entered pending WiFi menu still repaints on connect");
  finish();
}

static void wifiWatchdogOtherNetworkView() {
  tapWifiConnect();
  handleWiFiTap((uint32_t)rowOf("Status"));
  wifiConnectsAt(now + 1500);
  const unsigned before = listsShown;
  runSpawned();
  drainUi();
  EXPECT(listsShown == before && rowHas(1, "Status"),
         "another Network view is not replaced by the WiFi menu");
  finish();
}

static void ringWatchdogOnMenu() {
  tapRingConnect();
  ringInFlightUntil = now + 6000;
  ringConnectsAt(now + 6000);
  const unsigned before = listsShown;
  runSpawned();
  drainUi();
  EXPECT(listsShown == before + 1 && shownCount == 5 && rowHas(1, "Ring: connected"),
         "R1 menu repaints when the ring connects");
  CHECK(gRingPendingDeadlineMs == 0 && !gRingPendingTaskActive);
  finish();
}

static void ringWatchdogFailedOnMenu() {
  tapRingConnect();
  ringInFlightUntil = now + 5000;   // the worker gives up without a link
  const unsigned before = listsShown;
  runSpawned();
  drainUi();
  EXPECT(listsShown == before + 1 && rowHas(1, "Ring: disconnected"),
         "R1 menu drops the connecting overlay when the job ends");
  CHECK(gRingPendingDeadlineMs == 0 && !gRingPendingTaskActive);
  finish();
}

static void ringWatchdogReentry() {
  tapRingConnect();
  handleBluetoothR1Tap(0);                      // back to Bluetooth ...
  CHECK(gNetSub == NET_SUB_BLUETOOTH);
  ringInFlightUntil = now + 8000;
  ringConnectsAt(now + 8000);
  at(now + 3000, [] { showBluetoothR1Menu(); });   // ... and back to R1
  const unsigned before = listsShown;
  runSpawned();
  drainUi();
  EXPECT(listsShown == before + 2 && rowHas(1, "Ring: connected"),
         "a re-entered pending R1 menu still repaints on connect");
  finish();
}

static void checkResultsOnLens() {
  EXPECT(gNetSub == NET_SUB_WIFI_SCAN && shownCount == 4 && rowHas(0, "<- WiFi") &&
         rowHas(1, "HomeNet") && rowHas(2, "Cafe") && rowHas(3, "Lab"),
         "scan results are on the lens");
  CHECK(gScanCacheCount == 3 && page == G2_HIJACK_PAGE_NETWORK);
}

static void scanResultsOnMenu() {
  tapScan();
  runSpawned();
  drainUi();
  checkResultsOnLens();
  CHECK(cardShows == 1 && cardHides == 2 && scans == 1);
  finish();
}

static void scanPlaceholderOnMenu() {
  cardSucceeds = false;
  tapScan();
  duringScan = [] { drainUi(); CHECK(rowHas(1, "Scanning")); };
  runSpawned();
  drainUi();
  checkResultsOnLens();
  finish();
}

static void scanPlaceholderQueueBackedUp() {
  cardSucceeds = false;
  tapScan();
  runSpawned();        // the applier runs only after the whole scan
  drainUi();
  checkResultsOnLens();
  finish();
}

static void scanInlineFallback() {
  taskCreateFails = true;
  tapScan();           // runs the flow on the tap dispatcher
  CHECK(spawnedCount == 0 && scans == 1);
  drainUi();
  checkResultsOnLens();
  finish();
}

struct Case { const char* name; void (*run)(); };
static const Case cases[] = {
  {"wifi_watchdog_after_hold", wifiWatchdogAfterHold},
  {"ring_watchdog_after_hold", ringWatchdogAfterHold},
  {"scan_after_hold", scanAfterHold},
  {"scan_placeholder_after_hold", scanPlaceholderAfterHold},
  {"scan_placeholder_left_before_drawn", scanPlaceholderLeftBeforeDrawn},
  {"scan_left_during_settle", scanLeftDuringSettle},
  {"wifi_watchdog_on_menu", wifiWatchdogOnMenu},
  {"wifi_watchdog_expiry_on_menu", wifiWatchdogExpiryOnMenu},
  {"wifi_watchdog_reentry", wifiWatchdogReentry},
  {"wifi_watchdog_other_network_view", wifiWatchdogOtherNetworkView},
  {"ring_watchdog_on_menu", ringWatchdogOnMenu},
  {"ring_watchdog_failed_on_menu", ringWatchdogFailedOnMenu},
  {"ring_watchdog_reentry", ringWatchdogReentry},
  {"scan_results_on_menu", scanResultsOnMenu},
  {"scan_placeholder_on_menu", scanPlaceholderOnMenu},
  {"scan_placeholder_queue_backed_up", scanPlaceholderQueueBackedUp},
  {"scan_inline_fallback", scanInlineFallback},
};

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
    for (const Case& entry : cases) std::printf("%s\n", entry.name);
    return 0;
  }
  CHECK(argc == 2);
  for (const Case& entry : cases) {
    if (std::strcmp(argv[1], entry.name) != 0) continue;
    entry.run();
    std::printf("%s: %u checks passed\n", entry.name, checks);
    return 0;
  }
  std::fprintf(stderr, "unknown case %s\n", argv[1]);
  return 2;
}
