#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); } } while (false)
template <typename... Args> void quietLog(const char*, Args...) {}
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)
#define BROADCAST_PRINTF(...) quietLog(__VA_ARGS__)

struct PageSwapArgs;
struct ToastSpec;
struct NotifySpec;
struct CustomSpec;
struct NativeNotifSpec;
// INSERT_TYPES

struct G2Temple { bool connected = true; bool pluginDead = false; } gR, gL;
struct G2ContainerGeom { int x, y, w, h; };
struct G2TextChildSpec {
  const char* containerName;
  uint32_t containerId;
  const char* content;
  G2ContainerGeom geom;
  bool eventCapture;
};
struct G2PageModule { uint32_t liveIntervalMs; bool (*liveRender)(); };
struct LensState { bool containerReady = true; } lens;
static constexpr uint32_t BLOCKS_WIDGET_ID = 1;
static constexpr G2ContainerGeom kMicDetailListGeom{8, 8, 350, 270};
static constexpr G2ContainerGeom kMicDetailReadoutGeom{370, 180, 200, 100};
static constexpr uint32_t kMicDetailReadoutCid = 2;
static const char* const kMicDetailReadoutName = "micLive";
static char gMicDetailLastReadout[80]{};
static uint32_t gMicDetailRecreateMenuGen = 0;
static uint32_t gWidgetLifecycleEpoch = 9;
static uint32_t currentGeneration = 37;
static G2HijackPage currentPage = G2_HIJACK_PAGE_MIC_DETAIL;
static uint32_t presentationEpoch = 100;
static uint32_t publishedEpoch = 0, publishedLifecycle = 0;
static unsigned teardownCalls = 0, createCalls = 0, updateCalls = 0, kicks = 0;
static unsigned publishCalls = 0, rearmCalls = 0;
static unsigned loggingRenders = 0;
static bool queueAccepts = true, shutdownOk = true, createOk = true;
static bool commitOk = true, publishOk = true, startOk = true;
static bool navigateDuringShutdown = false, revokeDuringShutdown = false;
static bool sessionCurrent = true, navigateDuringCreate = false;
static bool actorIsSession = false;
static std::vector<LensUiJob*> queued;
static std::vector<std::string> commands;
static G2CmdCookie submittedCookie{};
static std::vector<std::string> createdRows;
static std::string createdText;
static void (*publishedExit)() = nullptr;

static bool gMicRunning = true;
enum AudioSource { AUDIO_SRC_G2_LEFT, AUDIO_SRC_LOCAL_PDM };
struct { std::string micSource = "auto"; } gSettings;
static bool gMicStreamOn = false;
[[maybe_unused]] static int micSampleRate = 22050, micChannels = 1;
static bool recording = false;
[[maybe_unused]] static AudioSource audioGetSource() { return AUDIO_SRC_G2_LEFT; }
[[maybe_unused]] static bool audioCaptureActive() { return gMicRunning; }
static bool micRecordingBusy() { return recording; }
[[maybe_unused]] static int getAudioLevel() { return 25; }
[[maybe_unused]] static bool g2TempleReadyAtGeneration(const G2Temple& arm) {
  return arm.connected && !arm.pluginDead;
}
[[maybe_unused]] static bool g2MicStreamEnable(bool on) {
  ++rearmCalls; gMicStreamOn = on; return true;
}
static G2HijackPage g2GetHijackPage() { return currentPage; }
static void g2SetHijackPage(G2HijackPage page) {
  if (currentPage != page) ++currentGeneration;
  currentPage = page;
}
uint32_t g2CurrentMenuGen() { return currentGeneration; }
static LensState g2LensGetState() { return lens; }
static bool g2SessionLifecycleAllows(uint32_t epoch) {
  return actorIsSession && sessionCurrent && epoch == gWidgetLifecycleEpoch;
}
static void liveTextKickRefresh() { ++kicks; }
static void liveTextExitToHijackMenu() {}
static bool tearDownActiveContainer(G2Temple&) {
  CHECK(actorIsSession);
  ++teardownCalls;
  if (navigateDuringShutdown) { currentPage = G2_HIJACK_PAGE_MAIN; ++currentGeneration; }
  if (revokeDuringShutdown) { ++gWidgetLifecycleEpoch; sessionCurrent = false; }
  // Leave lens ready to model delayed FSM mirror publication.
  if (shutdownOk) { ++presentationEpoch; gMicStreamOn = false; }
  return shutdownOk;
}
static bool sendCreateMixedListMultiTextAndWait(
    G2Temple&, const char* const* rows, size_t count, const G2ContainerGeom&,
    const G2TextChildSpec* text, size_t textCount, uint32_t) {
  CHECK(actorIsSession);
  CHECK(textCount == 1);
  ++createCalls;
  createdRows.clear();
  for (size_t i = 0; i < count; ++i) createdRows.emplace_back(rows[i]);
  createdText = text[0].content;
  if (navigateDuringCreate) ++gWidgetLifecycleEpoch;
  return createOk;
}
static bool g2NoteCreateSuccess(G2Temple&, bool list, uint32_t,
                                uint32_t lifecycle, uint32_t* committed) {
  CHECK(!list);
  if (!commitOk || lifecycle != gWidgetLifecycleEpoch) return false;
  *committed = ++presentationEpoch; lens.containerReady = true; return true;
}
static bool g2TextViewPublishForLifecycle(void (*exit)(), void*,
                                         uint32_t lifecycle, uint32_t epoch) {
  ++publishCalls;
  CHECK(lifecycle == gWidgetLifecycleEpoch);
  CHECK(epoch == presentationEpoch);
  if (!publishOk) return false;
  publishedExit = exit; publishedEpoch = epoch; publishedLifecycle = lifecycle;
  return true;
}
static bool sendUpdateTextNamed(G2Temple&, const char*, uint32_t, const char*) {
  CHECK(actorIsSession); ++updateCalls; return true;
}
static bool g2StartLiveTextPage(void*, uint32_t interval, bool (*render)()) {
  CHECK(interval == 1000 && render != nullptr);
  CHECK(gMicDetailRecreateMenuGen == 0);
  return startOk;
}
static void g2ShowLoggingMenu() { CHECK(!actorIsSession); ++loggingRenders; }
[[maybe_unused]] static bool g2SubmitHijackCommand(
    const char* line, const G2CmdCookie& cookie, void*, void*) {
  commands.emplace_back(line); submittedCookie = cookie; return true;
}
bool g2EnqueueLensJob(LensUiJob* job, G2LensEnqueueWait) {
  if (!queueAccepts) return false;
  queued.push_back(job); return true;
}

// INSERT_PRODUCTION

static void dispatch() {
  CHECK(!actorIsSession && !queued.empty());
  LensUiJob* job = queued.front(); queued.erase(queued.begin());
  const uint32_t liveGen = g2CurrentMenuGen();
  switch (job->kind) {
    // INSERT_DISPATCH
    default: std::abort();
  }
  delete job;
}
static bool tick() {
  actorIsSession = true;
  const bool ok = renderMicDetailLive();
  actorIsSession = false;
  return ok;
}
static G2CmdCookie cookie() {
  return {UINT64_C(0xfedcba9876543210), currentGeneration,
          G2_HIJACK_PAGE_MIC_DETAIL, 3};
}
static void reset() {
  CHECK(queued.empty());
  gR = {}; gL = {}; lens.containerReady = true;
  currentGeneration = 37; currentPage = G2_HIJACK_PAGE_MIC_DETAIL;
  gWidgetLifecycleEpoch = 9; presentationEpoch = 100;
  gMicDetailRecreateMenuGen = 0; gMicDetailLastReadout[0] = '\0';
  teardownCalls = createCalls = updateCalls = kicks = publishCalls = rearmCalls = 0;
  loggingRenders = 0;
  queueAccepts = shutdownOk = createOk = commitOk = publishOk = startOk = true;
  navigateDuringShutdown = revokeDuringShutdown = navigateDuringCreate = false;
  sessionCurrent = true; actorIsSession = false;
  gMicRunning = true; gMicStreamOn = false; recording = false;
  commands.clear(); createdRows.clear(); publishedExit = nullptr;
}
int main() {
  reset();
  const G2CmdCookie original = cookie();
  onMicDetailSourceDone(true, "OK", original, nullptr);
  CHECK(teardownCalls == 0 && createCalls == 0 && kicks == 0);
  CHECK(queued.front()->submitMenuGen == original.menuGen);
  CHECK(queued.front()->cmdSeq == original.seq);
  CHECK(queued.front()->targetNetSub == original.targetNetSub);
  dispatch();
  CHECK(teardownCalls == 0 && kicks == 1);
  CHECK(gMicDetailRecreateMenuGen == original.menuGen);
  CHECK(tick());
  CHECK(teardownCalls == 1 && createCalls == 1 && updateCalls == 0);
  CHECK(rearmCalls == 0 && publishCalls == 1);
  CHECK(publishedExit == liveTextExitToHijackMenu);
  CHECK(publishedEpoch == 102 && publishedLifecycle == 9);
  CHECK(tick());
  CHECK(teardownCalls == 1 && createCalls == 1);
#if ENABLE_MICROPHONE
  CHECK(rearmCalls == 1);
  CHECK(createdRows[1] == "MIC: On" && createdRows[2] == "Record 60s");
  CHECK(createdRows[3] == "Src: auto (g2)");
  CHECK(createdText == "Lvl: 25%\n16000Hz 16b 1ch\nidle");
#endif

  // Original dispatch guard rejects leave-and-return to the same page.
  reset(); onMicDetailSourceDone(false, "Error", cookie(), nullptr);
  currentGeneration += 2; dispatch();
  CHECK(kicks == 0 && gMicDetailRecreateMenuGen == 0);
  CHECK(tick()); CHECK(teardownCalls == 0 && createCalls == 0);
  // A handoff accepted before navigation also cannot recreate after it.
  reset(); onMicDetailSourceDone(true, nullptr, cookie(), nullptr); dispatch();
  currentGeneration += 2;
  lens.containerReady = false;
  CHECK(!tick()); CHECK(teardownCalls == 0 && createCalls == 0 && updateCalls == 0);
  reset(); requestMicDetailRecreate(); CHECK(g2ShowMicDetail());
  CHECK(gMicDetailRecreateMenuGen == 0);
  reset(); currentPage = G2_HIJACK_PAGE_MAIN; requestMicDetailRecreate();
  CHECK(kicks == 0 && gMicDetailRecreateMenuGen == 0);

  // Enqueue failure leaves the live view alone; no inline mutation fallback.
  reset(); queueAccepts = false;
  onMicDetailSourceDone(true, nullptr, cookie(), nullptr);
  CHECK(queued.empty() && kicks == 0 && teardownCalls == 0);

  // SHUTDOWN failure, navigation or lifecycle revocation must not CREATE.
  for (int failure = 0; failure < 3; ++failure) {
    reset(); onMicDetailSourceDone(true, nullptr, cookie(), nullptr); dispatch();
    shutdownOk = failure != 0;
    navigateDuringShutdown = failure == 1;
    revokeDuringShutdown = failure == 2;
    CHECK(!tick()); CHECK(teardownCalls == 1 && createCalls == 0 && publishCalls == 0);
  }
  // Neither failed CREATE nor a revoked commit publishes an exit callback.
  for (int failure = 0; failure < 3; ++failure) {
    reset(); requestMicDetailRecreate();
    createOk = failure != 0; commitOk = failure != 1; navigateDuringCreate = failure == 2;
    CHECK(!tick()); CHECK(publishCalls == 0);
  }
  reset(); requestMicDetailRecreate(); publishOk = false;
  CHECK(!tick()); CHECK(publishCalls == 1 && publishedExit == nullptr);
  reset(); requestMicDetailRecreate(); gR.connected = gL.connected = false;
  CHECK(!tick()); CHECK(teardownCalls == 0 && createCalls == 0);
  // Initial CREATE's callback publication remains with the generic live worker.
  reset(); lens.containerReady = false;
  CHECK(tick()); CHECK(createCalls == 1 && teardownCalls == 0 && publishCalls == 0);

#if ENABLE_MICROPHONE
  for (bool on : {false, true}) {
    reset();
    onMicDetailRuntimeDone(true, "OK", cookie(), reinterpret_cast<void*>(uintptr_t(on)));
    CHECK(commands.size() == 1);
    CHECK(commands[0] == (on ? "micautostart on" : "micautostart off"));
    CHECK(submittedCookie.menuGen == currentGeneration && submittedCookie.seq == cookie().seq);
    CHECK(teardownCalls == 0); dispatch(); CHECK(tick());
  }
  for (const char* result : {"Error: refused", "ERROR: refused"}) {
    reset(); onMicDetailRuntimeDone(true, result, cookie(), nullptr);
    CHECK(commands.empty()); dispatch(); CHECK(tick());
  }
  reset(); onMicDetailRuntimeDone(false, "no", cookie(), nullptr);
  CHECK(commands.empty()); dispatch(); CHECK(tick());
  for (bool active : {false, true}) {
    reset(); recording = active;
    onMicDetailRecordDone(true, "OK", cookie(), nullptr);
    CHECK(commands.empty() && teardownCalls == 0); dispatch(); CHECK(tick());
    CHECK(createdRows[2] == (active ? "Stop recording" : "Record 60s"));
  }
#endif
  // Logging uses the same original-cookie dispatch contract for every result.
  for (bool ok : {false, true}) {
    reset(); currentPage = G2_HIJACK_PAGE_LOGGING;
    auto commandCookie = cookie(); commandCookie.targetPage = currentPage;
    onLoggingCmdDone(ok, ok ? "OK" : "Error: refused", commandCookie, nullptr);
    CHECK(loggingRenders == 0);
    CHECK(queued.front()->cmdSeq == commandCookie.seq);
    CHECK(queued.front()->targetPage == G2_HIJACK_PAGE_LOGGING);
    dispatch(); CHECK(loggingRenders == 1);
  }
  reset(); currentPage = G2_HIJACK_PAGE_LOGGING;
  onLoggingCmdDone(true, "OK", cookie(), nullptr);
  currentGeneration += 2; dispatch(); CHECK(loggingRenders == 0);
  reset(); onLoggingCmdDone(true, "OK", cookie(), nullptr);
  dispatch(); CHECK(loggingRenders == 0);
  reset(); currentPage = G2_HIJACK_PAGE_LOGGING; queueAccepts = false;
  onLoggingCmdDone(true, "OK", cookie(), nullptr);
  CHECK(queued.empty() && loggingRenders == 0);
  CHECK(queued.empty());
  std::printf("G2 mic/logging completion: %u checks passed (microphone=%d)\n", checks, ENABLE_MICROPHONE);
}
