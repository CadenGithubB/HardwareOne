// Edge Impulse lifecycle suite driven by test_edge_impulse_lifecycle.py. The
// whole module is compiled after edge_impulse_host_fakes.h: the continuous
// task is a std::thread, FreeRTOS semaphores are std::mutex/condition
// variables, and the fake interpreter can hold one Invoke open until the
// scenario releases it, so each race is driven to the point the firmware can
// hit and then checked. One scenario per run (argv[2]).
#include "edge_impulse_host_fakes.h"

#include <thread>

static std::string gFixtures;

// INSERT_EDGE_IMPULSE_MODULE

// The f4cc542 module dispatched `eicontinuous 0` and `eimodelunload` through
// the command registry; route those to this module's table.
String executeCommandThroughRegistry(const String& line) {
  const std::string text = line.c_str();
  const size_t space = text.find(' ');
  const std::string name = text.substr(0, space);
  const std::string args = space == std::string::npos ? std::string() : text.substr(space + 1);
  for (size_t i = 0; i < edgeImpulseCommandsCount; ++i) {
    if (name == edgeImpulseCommands[i].name) return String(edgeImpulseCommands[i].handler(String(args)));
  }
  return String("Error: unknown command");
}

static const char* const kModelA = "/EI Models/a/a.tflite";
static const char* const kModelB = "/EI Models/b/b.tflite";

// A classification model (96x96x3 in, the geometry the f4cc542 module assumed,
// two classes out) and a camera frame; both model files use the same spec, so
// a swap never changes the tensor sizes an Invoke in progress writes.
static void setUp(int intervalMs) {
  gFakeModel = FakeModelSpec{};
  gFakeModel.input.type = kTfLiteUInt8;
  gFakeModel.input.dims = {1, 96, 96, 3};
  gFakeModel.output.type = kTfLiteFloat32;
  gFakeModel.output.dims = {1, 2};
  gFakeModel.outputValues = {0.1f, 0.9f};
  installModel(kModelA, {"bg", "obj"});
  installModel(kModelB, {"bg", "obj"});
  gFakeCameraJpeg = readFixture(gFixtures + "/rgb420_24x24.jpg");
  gSettings.edgeImpulseIntervalMs = intervalMs;
  initEdgeImpulse();
  eiCheck(loadModelFromFile(kModelA), "model loads");
  eiCheck(gFakeLiveInterpreters.load() == 1, "one interpreter after load");
}

static bool noTaskLeft(int ms) {
  return waitFor([] { return gFakeTasksRunning.load() == 0; }, ms);
}

// Continuous mode with the next Invoke held open inside the continuous task.
static void startWithBlockedFrame() {
  gFakeReleaseInvoke = false;
  gFakeBlockNextInvoke = true;
  eiCheck(startContinuousInference(), "continuous mode starts");
  eiCheck(waitFor([] { return gFakeInvokeBlocked.load(); }, 3000), "continuous frame reaches Invoke");
  std::lock_guard<std::mutex> lock(gFakeInvokeLog);
  eiCheck(!gFakeInvokeTaskIds.empty() && gFakeInvokeTaskIds.back() != 0,
          "the held Invoke runs on the continuous task");
}

static void tearDown() {
  gFakeReleaseInvoke = true;
  stopContinuousInference();
  eiCheck(noTaskLeft(2000), "no continuous task left at the end");
  joinFakeTasks();
  if (isEdgeImpulseModelLoaded()) unloadModel();
  eiCheck(gFakeLiveInterpreters.load() == 0, "no interpreter left at the end");
  eiCheck(!gFakeConcurrentInvoke, "never two Invokes at once");
  eiCheck(!gFakeDestroyedDuringInvoke, "never an interpreter freed under its Invoke");
  eiCheck(!gFakeGuardBroken, "tensor guard bands intact");
}

// `eicontinuous 0` then `eicontinuous 1` inside one interval: the first task
// must be gone before the second starts, so exactly one task runs frames.
static void scenarioStopStart() {
  setUp(300);
  eiCheck(startContinuousInference(), "continuous mode starts");
  eiCheck(waitFor([] { return gFakeInvokes.load() >= 1; }, 3000), "first frame runs");
  // The first task is now in its interval sleep.
  stopContinuousInference();
  eiCheck(startContinuousInference(), "restart inside the interval");
  size_t restartIndex = 0;
  {
    std::lock_guard<std::mutex> lock(gFakeInvokeLog);
    restartIndex = gFakeInvokeTaskIds.size();
  }
  sleepMs(1000);  // more than three intervals
  eiCheck(gFakeTasksCreated.load() == 2, "two tasks created in total");
  eiCheck(gFakeTasksRunning.load() == 1, "exactly one continuous task after stop+start");
  {
    std::lock_guard<std::mutex> lock(gFakeInvokeLog);
    std::set<int> ids(gFakeInvokeTaskIds.begin() + static_cast<long>(restartIndex), gFakeInvokeTaskIds.end());
    eiCheck(!ids.empty(), "the restarted task runs frames");
    eiCheck(ids.size() == 1 && *ids.begin() == 2, "only the new task runs frames after the restart");
  }
  // A stop returns once the task has exited, not one interval later.
  stopContinuousInference();
  eiCheck(noTaskLeft(100), "stop joins the continuous task");
  eiCheck(!isContinuousInferenceRunning() && !gSettings.edgeImpulseContinuous, "stop is reported and saved");
  tearDown();
}

// `eienable 0` while the continuous task is inside Invoke: the interpreter
// stays alive until that frame ends, then the task exits and the model goes.
static void scenarioEnableOffDuringInvoke() {
  setUp(100);
  startWithBlockedFrame();
  std::atomic<bool> finished{false};
  std::thread off([&finished] {
    cmd_ei_enable(String("0"));
    finished = true;
  });
  sleepMs(200);
  eiCheck(!gFakeDestroyedDuringInvoke, "eienable 0 does not free the interpreter under Invoke");
  eiCheck(gFakeLiveInterpreters.load() == 1, "interpreter alive while its frame runs");
  eiCheck(!finished, "eienable 0 waits for the running frame");
  gFakeReleaseInvoke = true;
  off.join();
  eiCheck(!gFakeDestroyedDuringInvoke, "frame finished before the free");
  eiCheck(!isEdgeImpulseModelLoaded() && gFakeLiveInterpreters.load() == 0, "model unloaded afterwards");
  eiCheck(noTaskLeft(100), "continuous task exited");
  eiCheck(!gSettings.eiEnabled && !gSettings.edgeImpulseContinuous, "disabled and continuous off");
  tearDown();
}

// `eimodelload` while the continuous task is inside Invoke: the swap waits
// for the frame, then continuous mode resumes on the new model.
static void scenarioLoadDuringInvoke() {
  setUp(100);
  startWithBlockedFrame();
  std::atomic<int> loaded{-1};
  std::thread swap([&loaded] { loaded = loadModelFromFile(kModelB) ? 1 : 0; });
  sleepMs(200);
  eiCheck(!gFakeDestroyedDuringInvoke, "model load does not free the interpreter under Invoke");
  eiCheck(loaded.load() == -1, "model load waits for the running frame");
  gFakeReleaseInvoke = true;
  swap.join();
  eiCheck(loaded.load() == 1, "the new model loads");
  eiCheck(gFakeLiveInterpreters.load() == 1, "one interpreter after the swap");
  eiCheck(isContinuousInferenceRunning(), "continuous mode resumes on the new model");
  const int invokes = gFakeInvokes.load();
  eiCheck(waitFor([invokes] { return gFakeInvokes.load() > invokes; }, 2000), "resumed task runs frames");
  eiCheck(waitFor([] { return gFakeTasksRunning.load() == 1; }, 500), "one continuous task after the swap");
  tearDown();
}

// Detect commands while a continuous frame runs: CLI, file and web detect
// report busy at once and run no Invoke; between frames a detect runs.
static void scenarioDetectDuringContinuous() {
  setUp(100);
  fakePutFile("/images/frame.jpg", std::string(gFakeCameraJpeg.begin(), gFakeCameraJpeg.end()));
  startWithBlockedFrame();
  const int invokes = gFakeInvokes.load();

  const EIResults camera = runEdgeImpulseInference();
  eiCheck(!gFakeConcurrentInvoke, "detect does not Invoke beside the continuous frame");
  eiCheck(!camera.success && camera.errorMessage && std::strstr(camera.errorMessage, "busy"),
          "camera detect reports busy");
  const std::string cli = cmd_ei_detect(String(""));
  eiCheck(cli.find("busy") != std::string::npos, "eidetect reports busy");
  const EIResults file = runInferenceFromFile("/images/frame.jpg");
  eiCheck(!file.success && file.errorMessage && std::strstr(file.errorMessage, "busy"),
          "file inference reports busy");
  httpd_req_t req;
  eiCheck(handleEdgeImpulseDetect(&req) == ESP_OK, "web detect answers");
  JsonDocument doc;
  eiCheck(!deserializeJson(doc, req.body.c_str()), "web reply parses");
  eiCheck(!doc["success"].as<bool>() &&
              std::string(doc["error"] | "").find("busy") != std::string::npos,
          "web detect reports busy");
  eiCheck(gFakeInvokes.load() == invokes, "a busy detect runs no Invoke");

  gFakeReleaseInvoke = true;
  eiCheck(waitFor([] { return runEdgeImpulseInference().success; }, 3000), "detect between frames runs");
  tearDown();
}

// `eimodelunload` while a single detect is inside Invoke waits for it.
static void scenarioUnloadDuringDetect() {
  setUp(1000);
  gFakeReleaseInvoke = false;
  gFakeBlockNextInvoke = true;
  std::atomic<bool> detected{false};
  std::thread detect([&detected] { detected = runEdgeImpulseInference().success; });
  eiCheck(waitFor([] { return gFakeInvokeBlocked.load(); }, 3000), "detect reaches Invoke");
  std::atomic<bool> unloaded{false};
  std::thread unload([&unloaded] {
    unloadModel();
    unloaded = true;
  });
  sleepMs(200);
  eiCheck(!gFakeDestroyedDuringInvoke, "unload does not free the interpreter under Invoke");
  eiCheck(!unloaded, "unload waits for the running detect");
  gFakeReleaseInvoke = true;
  detect.join();
  unload.join();
  eiCheck(detected.load(), "the detect completes");
  eiCheck(!isEdgeImpulseModelLoaded() && gFakeLiveInterpreters.load() == 0, "model unloaded afterwards");
  tearDown();
}

int main(int argc, char** argv) {
  eiCheck(argc == 3, "usage: harness <fixtures> <scenario>");
  gFixtures = argv[1];
  const std::string scenario = argv[2];
  const std::map<std::string, void (*)()> scenarios = {
    {"stop_start", scenarioStopStart},
    {"enable_off_during_invoke", scenarioEnableOffDuringInvoke},
    {"load_during_invoke", scenarioLoadDuringInvoke},
    {"detect_during_continuous", scenarioDetectDuringContinuous},
    {"unload_during_detect", scenarioUnloadDuringDetect},
  };
  auto it = scenarios.find(scenario);
  eiCheck(it != scenarios.end(), "known scenario");
  it->second();
  std::printf("EDGE_IMPULSE_LIFECYCLE %s PASS\n", scenario.c_str());
}
