// test_g2_sensors_redraw.py compiles the complete production Sensors page
// (G2_Page_Sensors.cpp, includes stripped; camera and microphone compiled in,
// the other sensors off) beside these boundaries. Background completions
// (the cam_pwr post-hook, the openmic/closemic completion on cmd_exec) must
// not render or write page state on their own task: the page queues a Redraw
// for the lens applier, which drops it when the user navigated since. An
// "interleave" runs a scripted user tap at the first page boundary the
// completion reaches after its checks (or right after it returns), which is
// the window the race needs. The CAM toggle submits the camera owner's gated
// `opencamera async` / `closecamera async` (never powering the camera on the
// tap task) and persists cameraautostart only after the owner admitted it.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

static unsigned checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (false)

#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
#define ENABLE_CAMERA_SENSOR 1
#define ENABLE_MICROPHONE 1
#define ENABLE_IMU_SENSOR 0
#define ENABLE_TOF_SENSOR 0
#define ENABLE_THERMAL_SENSOR 0
#define ENABLE_GAMEPAD_SENSOR 0
#define ENABLE_ANO_ENCODER 0
#define ENABLE_APDS_SENSOR 0
#define ENABLE_RTC_SENSOR 0
#define ENABLE_FM_RADIO 0
#define ENABLE_GPS_SENSOR 0
#define ENABLE_PRESENCE_SENSOR 0
#define ENABLE_OLED_DISPLAY 0
#define ENABLE_NEOPIXEL 0
#define EXT_RAM_BSS_ATTR
template <typename... Args> void quietLog(const char*, Args...) {}
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)
#define BROADCAST_PRINTF(...) quietLog(__VA_ARGS__)
using String = std::string;

// INSERT_BOUNDARY_TYPES

// --- Task contexts, interleaving -------------------------------------------
enum class Ctx { Tap, CamPwr, CmdExec, Applier };
static Ctx ctx = Ctx::Tap;
static std::function<void()> interleave;  // scripted user tap, fired once
static void dispatchTap(uint32_t idx);

static void runInterleave() {
  if (!interleave) return;
  std::function<void()> step = std::move(interleave);
  interleave = nullptr;
  const Ctx saved = ctx;
  ctx = Ctx::Tap;
  step();
  ctx = saved;
}

// --- Lens: list renders, swap admission, hijack page, generation ------------
struct RenderAttempt { Ctx ctx; bool accepted; };
static std::vector<RenderAttempt> renders;
static std::vector<std::string> lensRows;  // rows of the last admitted render
static bool swapInFlight = false;          // admitted swap not yet completed
static bool listAccepts = true;
static G2HijackPage page = G2_HIJACK_PAGE_MAIN;
static unsigned menuGen = 1;

bool g2ShowListPage(const char* const* rows, size_t count) {
  runInterleave();
  const bool accepted = listAccepts && !swapInFlight && rows && count > 0;
  renders.push_back({ctx, accepted});
  if (!accepted) return false;
  lensRows.assign(rows, rows + count);
  swapInFlight = true;
  return true;
}
bool g2ShowText(const char*) { return false; }
static void settle() { swapInFlight = false; }  // the lens finished the swap
G2HijackPage g2GetHijackPage() { return page; }
void g2SetHijackPage(G2HijackPage value) { if (page != value) { page = value; ++menuGen; } }
void g2BumpMenuGen() { ++menuGen; }
uint32_t g2CurrentMenuGen() { return menuGen; }

// Compound pages started from the Sensors page.
static bool micDetailAccepts = false, sensorLiveAccepts = false;
static unsigned liveStarts = 0;
static int levelDuringLiveStart = -1;
bool g2ShowMicDetail() {
  if (!micDetailAccepts) return false;
  g2SetHijackPage(G2_HIJACK_PAGE_MIC_DETAIL);
  return true;
}
static int currentSensorsLevel();
bool g2ShowSensorLive() {
  runInterleave();
  levelDuringLiveStart = currentSensorsLevel();
  if (!sensorLiveAccepts) return false;
  ++liveStarts;
  renders.push_back({ctx, true});
  lensRows.assign({"<- Hardware", "Run: live"});
  g2SetHijackPage(G2_HIJACK_PAGE_SENSORS);  // as the shipping live page does
  return true;
}
static unsigned viewers = 0, ledMenus = 0, mainMenus = 0;
bool g2ShowCameraViewer(void (*)()) { ++viewers; return true; }
bool g2ShowCameraStream(void (*)()) { ++viewers; return true; }
void g2ShowLedMenu() { ++ledMenus; }
void g2RedrawHijackMainMenu() { ++mainMenus; }

// Camera Settings TOP as shipped: render first, claim the page once shown.
static std::vector<uint32_t> cameraSettingsTaps;
void g2ShowCameraSettingsMenu() {
  static const char* const rows[] = {"<- Back", "Stream: 640x480", "FPS: 10", "Quality"};
  if (g2ShowListPage(rows, 4)) g2SetHijackPage(G2_HIJACK_PAGE_CAMERA_SETTINGS);
}

// --- Commands and the lens applier queue ------------------------------------
using G2HijackCmdCallback = void (*)(bool, const char*, const G2CmdCookie&, void*);
static std::vector<std::string> submitted;
static G2HijackCmdCallback lastCallback = nullptr;
static void* lastUserData = nullptr;
static G2CmdCookie lastCookie{};
static uint64_t cmdSeq = 0;
static bool camAuto = false, micAuto = false;
static bool submitAccepts = true;  // false: the executor queue refuses (no-op)
bool g2SubmitHijackCommand(const char* line, const G2CmdCookie& cookie,
                           G2HijackCmdCallback callback, void* userData) {
  if (!submitAccepts) return false;
  submitted.emplace_back(line);
  lastCookie = cookie;              // helper-owned fields, as shipped
  lastCookie.seq = ++cmdSeq;
  lastCookie.menuGen = menuGen;
  lastCallback = callback;
  lastUserData = userData;
  if (std::strcmp(line, "cameraautostart on") == 0) camAuto = true;
  if (std::strcmp(line, "cameraautostart off") == 0) camAuto = false;
  return true;
}
struct QueuedRedraw { G2CmdCookie cookie; void (*render)(); Ctx from; };
static std::vector<QueuedRedraw> lensQueue;
static unsigned redrawsDropped = 0, redrawsRun = 0;
bool g2EnqueueRedraw(const G2CmdCookie& cookie, void (*render)(), const char*) {
  runInterleave();
  if (!render) return false;
  lensQueue.push_back({cookie, render, ctx});
  return true;
}
// The lens applier's Redraw rule: drop when the generation moved.
static void runApplier() {
  const Ctx saved = ctx;
  ctx = Ctx::Applier;
  std::vector<QueuedRedraw> jobs;
  jobs.swap(lensQueue);
  for (const QueuedRedraw& job : jobs) {
    if (job.cookie.menuGen != menuGen) { ++redrawsDropped; continue; }
    ++redrawsRun;
    job.render();
  }
  ctx = saved;
}

// --- Camera, microphone, audio, settings, registry --------------------------
bool gCameraRunning = false;
bool gMicRunning = false;
// Camera power requested by the page itself on the tap task (no command:
// no authorization, enable gate or audit). Must stay zero.
static unsigned camStarts = 0, camStops = 0;
bool cameraPowerRequestStartAsync() { ++camStarts; return true; }
bool cameraPowerRequestStopAsync() { ++camStops; return true; }
// The camera owner behind `opencamera async` / `closecamera async`
// (System_Camera_DVP): refused while the camera is disabled (start only),
// admitted when cam_pwr accepts. It runs when a case executes the submitted
// command on cmd_exec.
static bool cameraEnabled = true, camPwrAccepts = true;
static unsigned ownerStarts = 0, ownerStops = 0;
typedef void (*CameraPowerPostHook)(void);
static CameraPowerPostHook cameraHook = nullptr;
void cameraPowerSetPostHook(CameraPowerPostHook hook) { cameraHook = hook; }

enum AudioSource { AUDIO_SRC_NONE = 0, AUDIO_SRC_LOCAL_PDM, AUDIO_SRC_G2_LEFT };
bool audioAnySourceAvailable() { runInterleave(); return true; }
AudioSource audioGetSource() { return AUDIO_SRC_LOCAL_PDM; }
bool audioSourceAvailable(AudioSource source) { return source == AUDIO_SRC_LOCAL_PDM; }

struct { std::string micSource = "auto"; int ledBrightness = 50; } gSettings;
struct FeatureEntry { const char* id; bool* enabledSetting; };
static const FeatureEntry kCameraFeature = {"camera", &camAuto};
static const FeatureEntry kMicFeature = {"microphone", &micAuto};
const FeatureEntry* getFeatureById(const char* id) {
  runInterleave();
  if (id && std::strcmp(id, "camera") == 0) return &kCameraFeature;
  if (id && std::strcmp(id, "microphone") == 0) return &kMicFeature;
  return nullptr;
}
static uint32_t nowMs = 10000;
uint32_t millis() { return nowMs; }

// INSERT_SENSORS_PROTOTYPES

// INSERT_LIST_PUBLISH_HELPER

// INSERT_SENSORS_PAGE

static int currentSensorsLevel() { return (int)gSensorsLevel; }

// --- Scenario helpers -------------------------------------------------------
// The tap dispatcher: one tap per settled presentation, routed by page.
static void dispatchTap(uint32_t idx) {
  nowMs += 1000;  // past the detail-toggle debounce
  if (page == G2_HIJACK_PAGE_SENSORS) g2SensorsHandleTap(idx);
  else if (page == G2_HIJACK_PAGE_CAMERA_SETTINGS) cameraSettingsTaps.push_back(idx);
}
static void tap(uint32_t idx) { dispatchTap(idx); settle(); }

static bool shows(size_t row, const char* text) {
  return row < lensRows.size() && lensRows[row].find(text) != std::string::npos;
}
static bool renderedOn(Ctx where) {
  for (const RenderAttempt& r : renders) if (r.ctx == where) return true;
  return false;
}

static void reset() {
  ctx = Ctx::Tap;
  interleave = nullptr;
  renders.clear();
  lensRows.clear();
  swapInFlight = false;
  listAccepts = true;
  micDetailAccepts = false;
  sensorLiveAccepts = false;
  levelDuringLiveStart = -1;
  page = G2_HIJACK_PAGE_MAIN;
  ++menuGen;
  cameraSettingsTaps.clear();
  submitted.clear();
  lastCallback = nullptr;
  lensQueue.clear();
  gCameraRunning = false;
  gMicRunning = false;
  camAuto = false;
  micAuto = false;
  camStarts = camStops = 0;
  viewers = 0;
  submitAccepts = true;
  cameraEnabled = camPwrAccepts = true;
  ownerStarts = ownerStops = 0;
  sG2SensorsCamAsyncStartPending = false;
}

// cmd_exec runs the camera command the page submitted last: the registry
// (authorized: the line reaches the owner), the owner's enable gate and
// cam_pwr admission, then the page's completion with the owner's reply. A
// disabled camera answers "ERROR: ..." with ok=true, as the executor reports
// a handler that ran.
static bool runCameraCommand(bool authorized = true) {
  if (submitted.empty() || !lastCallback) return false;
  const std::string line = submitted.back();
  const bool start = line == "opencamera async";
  if (!start && line != "closecamera async") return false;
  const G2HijackCmdCallback done = lastCallback;
  const G2CmdCookie cookie = lastCookie;
  void* const userData = lastUserData;
  bool ok = true;
  const char* reply;
  if (!authorized) {
    ok = false;
    reply = "Error: Guest accounts are view-only.";
  } else if (start && !cameraEnabled) {
    reply = "ERROR: Camera is disabled - run 'cameraenabled 1' first";
  } else if (!camPwrAccepts) {
    reply = "Error: camera start not queued (camera power worker unavailable)";
  } else {
    ++(start ? ownerStarts : ownerStops);
    reply = start ? "OK: Camera start queued" : "OK: Camera stop queued";
  }
  ctx = Ctx::CmdExec;
  done(ok, reply, cookie, userData);
  ctx = Ctx::Tap;
  return true;
}

// Sensors list (rows: back, CAM, MIC), then the CAM detail.
static void openCamDetail() {
  g2ShowSensorsMenu();
  settle();
  CHECK(page == G2_HIJACK_PAGE_SENSORS);
  CHECK(shows(0, "<- Main Menu") && shows(1, "CAM") && shows(2, "MIC"));
  tap(1);
  CHECK(shows(1, "CAM: OFF") && shows(5, "Settings >"));
}

// The cam_pwr worker finishes a power operation and runs the post-hook.
static void cameraPowerDone(bool running) {
  gCameraRunning = running;
  renders.clear();
  ctx = Ctx::CamPwr;
  cameraHook();
  ctx = Ctx::Tap;
  runInterleave();  // the navigation lands before any applier render
  settle();
  runApplier();
  settle();
}

// --- Cases ------------------------------------------------------------------
// CAM: OFF -> start queued; the user opens Camera Settings while cam_pwr runs.
static void testHookVsCameraSettings() {
  reset();
  openCamDetail();
  tap(1);  // CAM: OFF -> `opencamera async`; admitted -> cameraautostart on
  CHECK(runCameraCommand());
  CHECK(camStarts == 0 && ownerStarts == 1 && camAuto);
  interleave = [] { dispatchTap(5); };  // "Settings >"
  cameraPowerDone(true);
  const bool hookRenderedOnCamPwr = renderedOn(Ctx::CamPwr);
  const bool hookStoleCameraSettingsPage = page != G2_HIJACK_PAGE_CAMERA_SETTINGS;
  CHECK(!hookRenderedOnCamPwr);
  CHECK(!hookStoleCameraSettingsPage);
  CHECK(shows(1, "Stream: 640x480"));
  // Camera Settings row 1 ("Stream: WxH") reaches the Camera Settings page.
  const size_t commands = submitted.size();
  tap(1);
  const bool cameraSettingsRowToggledCamera =
      camStops != 0 || submitted.size() != commands;
  CHECK(!cameraSettingsRowToggledCamera);
  CHECK(cameraSettingsTaps.size() == 1 && cameraSettingsTaps[0] == 1);
}

// Same, but the Camera Settings swap completed before the hook's render:
// the stale refresh must not pull the lens back to the CAM detail.
static void testHookAfterCameraSettingsShown() {
  reset();
  openCamDetail();
  interleave = [] { dispatchTap(5); settle(); };
  cameraPowerDone(true);
  CHECK(!renderedOn(Ctx::CamPwr));
  const bool staleHookReplacedCameraSettings =
      page != G2_HIJACK_PAGE_CAMERA_SETTINGS || !shows(1, "Stream: 640x480");
  CHECK(!staleHookReplacedCameraSettings);
}

// The user backs out to the Sensors list while cam_pwr runs.
static void testHookVsBackToList() {
  reset();
  openCamDetail();
  interleave = [] { dispatchTap(0); };  // "<- Hardware"
  cameraPowerDone(true);
  CHECK(!renderedOn(Ctx::CamPwr));
  CHECK(shows(0, "<- Main Menu"));
  // List row 1 (CAM) must open the CAM detail, not toggle the camera.
  tap(1);
  const bool hookLeftDetailOverList =
      camStarts != 0 || camStops != 0 || !submitted.empty() || !shows(4, "HW:");
  CHECK(!hookLeftDetailOverList);
  CHECK(gSensorsLevel == SENSORS_LEVEL_DETAIL && shows(1, "CAM: ON"));
}

// No navigation: the applier re-renders the CAM detail with the new state.
static void testHookRefreshesDetail() {
  reset();
  openCamDetail();
  const unsigned run = redrawsRun;
  cameraPowerDone(true);
  CHECK(!renderedOn(Ctx::CamPwr));
  CHECK(redrawsRun == run + 1);
  CHECK(renders.size() == 1 && renders[0].ctx == Ctx::Applier && renders[0].accepted);
  CHECK(shows(1, "CAM: ON") && shows(2, "Capture"));
  CHECK(page == G2_HIJACK_PAGE_SENSORS && gSensorsLevel == SENSORS_LEVEL_DETAIL);
}

// Off the CAM detail the hook queues and renders nothing.
static void testHookIgnoredOffDetail() {
  reset();
  g2ShowSensorsMenu();
  settle();
  cameraPowerDone(false);
  CHECK(renders.empty() && lensQueue.empty());
  CHECK(shows(0, "<- Main Menu"));
  openCamDetail();
  tap(5);  // Camera Settings
  CHECK(page == G2_HIJACK_PAGE_CAMERA_SETTINGS);
  cameraPowerDone(false);
  CHECK(renders.empty() && page == G2_HIJACK_PAGE_CAMERA_SETTINGS);
  CHECK(shows(1, "Stream: 640x480"));
}

// MIC static detail (its compound page and the live view refused).
static void openMicDetail() {
  g2ShowSensorsMenu();
  settle();
  tap(2);
  CHECK(gSensorsLevel == SENSORS_LEVEL_DETAIL && shows(1, "MIC: Off"));
}

// openmic completes on cmd_exec after the user went back to the list.
static void testMicCompletionVsBackToList() {
  reset();
  openMicDetail();
  tap(1);  // MIC: Off -> openmic, completion persists micautostart
  CHECK(submitted.size() == 1 && submitted[0] == "openmic" && lastCallback != nullptr);
  const G2HijackCmdCallback done = lastCallback;
  const G2CmdCookie cookie = lastCookie;
  void* const userData = lastUserData;
  gMicRunning = true;
  renders.clear();
  interleave = [] { dispatchTap(0); };  // "<- Hardware"
  ctx = Ctx::CmdExec;
  done(true, "Microphone opened", cookie, userData);
  ctx = Ctx::Tap;
  runInterleave();
  settle();
  runApplier();
  settle();
  CHECK(submitted.size() == 2 && submitted[1] == "micautostart on");
  const bool micRenderedOnCmdExec = renderedOn(Ctx::CmdExec);
  CHECK(!micRenderedOnCmdExec);
  CHECK(shows(0, "<- Main Menu"));
  tap(1);  // list row 1 = CAM
  const bool micLeftDetailOverList = submitted.size() != 2 || !shows(5, "Settings >");
  CHECK(!micLeftDetailOverList);
}

// Without navigation the applier redraws the MIC detail with the new state.
static void testMicCompletionRefreshes() {
  reset();
  openMicDetail();
  tap(1);
  const G2HijackCmdCallback done = lastCallback;
  CHECK(done != nullptr);
  if (!done) return;
  const G2CmdCookie cookie = lastCookie;
  gMicRunning = true;
  renders.clear();
  ctx = Ctx::CmdExec;
  done(true, "Microphone opened", cookie, lastUserData);
  ctx = Ctx::Tap;
  settle();
  runApplier();
  settle();
  CHECK(!renderedOn(Ctx::CmdExec));
  CHECK(renders.size() == 1 && renders[0].ctx == Ctx::Applier);
  CHECK(shows(1, "MIC: On") && gSensorsLevel == SENSORS_LEVEL_DETAIL);
  // A failed runtime skips the persist but still refreshes the rows.
  const size_t commands = submitted.size();
  ctx = Ctx::CmdExec;
  done(true, "Error: no source", lastCookie, lastUserData);
  ctx = Ctx::Tap;
  runApplier();
  settle();
  CHECK(submitted.size() == commands);
}

// A list tap past the last row does nothing.
static void testOutOfRangeListTap() {
  reset();
  g2ShowSensorsMenu();
  settle();
  renders.clear();
  tap(4);
  const bool outOfRangeListTapActed =
      !renders.empty() || viewers != 0 || camStarts != 0 || !submitted.empty();
  CHECK(!outOfRangeListTapActed);
  CHECK(gSensorsLevel == SENSORS_LEVEL_LIST && shows(0, "<- Main Menu"));
}

// The live view's level becomes current only once the live page started.
static void testLiveLevelCommittedAfterStart() {
  reset();
  g2ShowSensorsMenu();
  settle();
  sensorLiveAccepts = true;
  tap(2);  // MIC: compound refused, live view starts
  const bool liveLevelCommittedBeforeStart =
      levelDuringLiveStart != (int)SENSORS_LEVEL_LIST;
  CHECK(!liveLevelCommittedBeforeStart);
  CHECK(gSensorsLevel == SENSORS_LEVEL_LIVE && page == G2_HIJACK_PAGE_SENSORS);
  // Live view and static fallback both refused: the list keeps routing.
  reset();
  g2ShowSensorsMenu();
  settle();
  listAccepts = false;
  tap(2);
  listAccepts = true;
  CHECK(gSensorsLevel == SENSORS_LEVEL_LIST && shows(0, "<- Main Menu"));
}

// The CAM toggle is a command: the owner's gated asynchronous start/stop runs
// on cmd_exec, and cameraautostart follows only once the owner admitted it.
static void testCameraToggleIsACommand() {
  reset();
  openCamDetail();
  const size_t rendersBefore = renders.size();
  tap(1);  // CAM: OFF -> ON
  const bool tapPoweredCameraInline = camStarts != 0 || camStops != 0;
  CHECK(!tapPoweredCameraInline);
  CHECK(submitted == std::vector<std::string>{"opencamera async"} && lastCallback != nullptr);
  CHECK(!camAuto);  // nothing persisted before the owner answered
  CHECK(sG2SensorsCamAsyncStartPending && renders.size() == rendersBefore);
  CHECK(runCameraCommand());
  CHECK(ownerStarts == 1 && submitted.back() == "cameraautostart on" && camAuto);
  CHECK(!renderedOn(Ctx::CmdExec) && lensQueue.empty());  // the post-hook redraws
  cameraPowerDone(true);
  CHECK(!sG2SensorsCamAsyncStartPending && shows(1, "CAM: ON"));
  tap(1);  // CAM: ON -> OFF
  CHECK(camStops == 0 && submitted.back() == "closecamera async");
  CHECK(camAuto);
  CHECK(runCameraCommand());
  CHECK(ownerStops == 1 && submitted.back() == "cameraautostart off" && !camAuto);
}

// A refused start (camera disabled, or the caller not authorized) persists
// nothing, clears the pending start and redraws through the lens applier.
static void testCameraRefusalPersistsNothing() {
  for (const bool authorized : {true, false}) {
    reset();
    cameraEnabled = false;
    openCamDetail();
    tap(1);
    renders.clear();
    CHECK(runCameraCommand(authorized));
    CHECK(ownerStarts == 0 && camStarts == 0 && !camAuto);
    CHECK(submitted == std::vector<std::string>{"opencamera async"});
    CHECK(!sG2SensorsCamAsyncStartPending);
    CHECK(!renderedOn(Ctx::CmdExec) && lensQueue.size() == 1);
    runApplier();
    settle();
    CHECK(renders.size() == 1 && renders[0].ctx == Ctx::Applier && shows(1, "CAM: OFF"));
  }
  // cam_pwr could not take the request: no preference saved either.
  reset();
  camPwrAccepts = false;
  openCamDetail();
  tap(1);
  CHECK(runCameraCommand());
  CHECK(!camAuto && submitted.size() == 1 && !sG2SensorsCamAsyncStartPending);
}

// The executor refused the submission: nothing runs inline, the pending start
// is withdrawn and the tap re-renders the unchanged detail on its own task.
static void testCameraSubmitRefused() {
  reset();
  openCamDetail();
  submitAccepts = false;
  renders.clear();
  tap(1);
  submitAccepts = true;
  CHECK(camStarts == 0 && submitted.empty() && !camAuto);
  CHECK(!sG2SensorsCamAsyncStartPending);
  CHECK(renders.size() == 1 && renders[0].ctx == Ctx::Tap && shows(1, "CAM: OFF"));
}

int main() {
  g2RegisterSensorsCameraPowerHook();
  CHECK(cameraHook != nullptr);
  if (!cameraHook) return 1;
  testHookVsCameraSettings();
  testHookAfterCameraSettingsShown();
  testHookVsBackToList();
  testHookRefreshesDetail();
  testHookIgnoredOffDetail();
  testMicCompletionVsBackToList();
  testMicCompletionRefreshes();
  testOutOfRangeListTap();
  testLiveLevelCommittedAfterStart();
  testCameraToggleIsACommand();
  testCameraRefusalPersistsNothing();
  testCameraSubmitRefused();
  if (failures) {
    std::fprintf(stderr, "G2 Sensors redraw: %u of %u checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("G2 Sensors redraw: %u checks passed (%u redraws run, %u dropped)\n",
              checks, redrawsRun, redrawsDropped);
  return 0;
}
