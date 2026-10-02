// The production broker and segmenter are inserted without their ESP includes.
// Real threads/mutexes exercise publication and cooperative teardown. Virtual
// time/audio credits make >minute sessions fast without a simulated scheduler.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#define ENABLE_LOCAL_STT 1
#define ENABLE_MICROPHONE 1
#define ENABLE_ESP_SR 1
#define INFO_SYSTEMF(...) ((void)0)
using String = std::string;
using TransportSessionEpoch = uint32_t;
enum CommandSource { SOURCE_WEB, SOURCE_SERIAL, SOURCE_INTERNAL, SOURCE_ESPNOW,
  SOURCE_LOCAL_DISPLAY, SOURCE_BLUETOOTH, SOURCE_MQTT, SOURCE_VOICE, SOURCE_G2_GLASSES, SOURCE_UART };
// INSERT_INTERFACE
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
static thread_local unsigned criticalDepth = 0;
#define portENTER_CRITICAL(x) do { (x)->lock(); ++criticalDepth; } while (0)
#define portEXIT_CRITICAL(x) do { assert(criticalDepth); --criticalDepth; (x)->unlock(); } while (0)
#define pdMS_TO_TICKS(x) (x)
constexpr int pdPASS = 1;
static std::atomic<uint64_t> clockMs{1};
uint32_t millis() { return static_cast<uint32_t>(clockMs.load()); }
void vTaskDelay(uint32_t) { std::this_thread::sleep_for(std::chrono::microseconds(100)); }
static std::atomic<unsigned> deletedTasks{0}, activeTasks{0};
void vTaskDelete(void*);
static thread_local bool mainWorker = false, taskSelfDeleted = false, backendResetBeforeDelete = false;
uint32_t uxTaskGetStackHighWaterMark(void*) { return 2048; }
void taskStackRecord(const char*, uint32_t) {}
uint32_t esp_random() { return 0xabcdef01; }
static std::mutex tasksMutex;
static std::vector<std::thread> tasks;
static std::string failedTask;
int xTaskCreatePinnedToCore(void (*fn)(void*), const char* name, uint32_t bytes,
                            void* context, int priority, void*, int core) {
  assert(core == 1);
  assert((std::string(name) == "stt" && bytes == 12288 && priority == 2) ||
         (std::string(name) == "stt_capture" && bytes >= 4096 && bytes <= 8192 && priority == 3));
  if (failedTask == name) return 0;
  ++activeTasks;
  std::lock_guard<std::mutex> lock(tasksMutex);
  tasks.emplace_back([=, main = std::string(name) == "stt"] {
    mainWorker = main; fn(context); --activeTasks;
  });
  return pdPASS;
}
static std::atomic<bool> serialLive{true}, webLive{true}, successorLive{true};
bool transportSessionEpochIsLive(CommandSource source, uint32_t epoch) {
  if (source == SOURCE_SERIAL && epoch == 5) return serialLive.load();
  if (source == SOURCE_SERIAL && epoch == 6) return successorLive.load();
  if (source == SOURCE_WEB && epoch == 5) return webLive.load();
  return false;
}
static bool srRunning = false;
bool isESPSRRunning() { return srRunning; }
bool gMicRunning = false;
static bool recordingBusy = false;
bool micRecordingBusy() { return recordingBusy; }
struct { String micSource = "auto"; int microphoneGain = 70; int microphoneSampleRate = 48000; } gSettings;
enum AudioSource { AUDIO_SRC_NONE, AUDIO_SRC_LOCAL_PDM, AUDIO_SRC_G2_LEFT };
static std::mutex halMutex;
static AudioSource audioSource = AUDIO_SRC_LOCAL_PDM;
static std::string halOwner;
static bool halActive = false;
static std::atomic<bool> sourceAvailable{true}, startFails{false}, fallbackOnStart{false};
static std::atomic<bool> stopHeld{false}, stopRequested{false}, integrityAvailable{true};
static std::atomic<bool> invalidRead{false}, readTimeout{false};
static std::atomic<uint32_t> halOverruns{0};
static std::atomic<bool> gateNextIntegrity{false}, integrityGateEntered{false}, releaseIntegrityGate{false};
static std::atomic<uint64_t> allowedSamples{0}, deliveredSamples{0};
static std::atomic<unsigned> starts{0}, stops{0};
static size_t readLimit = 1024;
static size_t readCounter = 0;
static bool irregularReads = false;
// These are changed only after every worker has joined.
constexpr uint64_t kCalibrationSamples = 8000; // Real production500ms; never bypass adaptive mode.
static bool allVoice = true;
static int16_t dc = 0;
static std::vector<std::pair<uint64_t, uint64_t>> voiceIntervals;
static int16_t rawSample(uint64_t sample) {
  if (sample < kCalibrationSamples) return dc; // Ambient quiet while UI says preparing.
  sample -= kCalibrationSamples;
  bool voice = allVoice;
  for (const auto& interval : voiceIntervals)
    voice = voice || (sample >= interval.first && sample < interval.second);
  const int tone = voice ? ((sample & 1) ? -1 : 1) * (1000 + int(sample % 97)) : 0;
  return static_cast<int16_t>(dc + tone);
}
AudioSource audioGetSource() { std::lock_guard<std::mutex> lock(halMutex); return audioSource; }
bool audioCaptureBusy() { std::lock_guard<std::mutex> lock(halMutex); return !halOwner.empty(); }
bool audioCaptureActive() { std::lock_guard<std::mutex> lock(halMutex); return halActive; }
bool audioCaptureOwnedBy(const char* owner) { std::lock_guard<std::mutex> lock(halMutex); return halOwner == owner; }
// G2 recording-scoped capture support: counted so tests can check balance.
static int g2FastHolds = 0, g2Containers = 0, g2Kicks = 0;
void g2MicLinkFastAcquire() { ++g2FastHolds; }
void g2MicLinkFastRelease() { --g2FastHolds; }
void g2MicEnsureCaptureContainer() { ++g2Containers; }
void g2MicReleaseCaptureContainer() { --g2Containers; }
bool g2MicKickStream() { ++g2Kicks; return true; }
bool audioSourceAvailable(AudioSource s) { return s != AUDIO_SRC_NONE && sourceAvailable.load(); }
bool audioAnySourceAvailable() { return sourceAvailable.load(); }
bool audioSetSource(AudioSource s) {
  std::lock_guard<std::mutex> lock(halMutex);
  if (!halOwner.empty() || (s != AUDIO_SRC_NONE && !sourceAvailable)) return false;
  audioSource = s; return true;
}
bool audioCaptureStart(const char* owner, uint32_t rate) {
  std::lock_guard<std::mutex> lock(halMutex);
  assert(rate == 16000);
  if (startFails || !halOwner.empty()) return false;
  halOwner = owner; halActive = true; ++starts;
  if (audioSource == AUDIO_SRC_NONE || fallbackOnStart) audioSource = AUDIO_SRC_LOCAL_PDM;
  return true;
}
void audioCaptureStop(const char* owner) {
  std::lock_guard<std::mutex> lock(halMutex);
  if (halOwner == owner) {
    stopRequested = true;
    if (!stopHeld) { halOwner.clear(); halActive = false; ++stops; }
  }
}
static void releaseHALStop() {
  std::lock_guard<std::mutex> lock(halMutex);
  stopHeld = false;
  if (stopRequested && !halOwner.empty()) { halOwner.clear(); halActive = false; ++stops; }
}
size_t audioTrimBufferedPcm(const char* owner, size_t keep) {
  assert(std::string(owner) == "stt" && keep == 0); return 0;
}
bool audioCaptureOverruns(const char* owner, uint32_t* out) {
  if (!out || !integrityAvailable || !audioCaptureOwnedBy(owner)) return false;
  *out = halOverruns.load();
  // Pause after sampling the counter but before returning it. The test can
  // request stop and add a loss here, forcing the separate final-stop check.
  if (gateNextIntegrity.exchange(false)) {
    integrityGateEntered = true;
    while (!releaseIntegrityGate) std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return true;
}
size_t audioReadPcm(int16_t* pcm, size_t requested, uint32_t timeout) {
  assert(audioCaptureOwnedBy("stt") && timeout == 80);
  if (invalidRead) return requested + 1;
  if (readTimeout) { clockMs += timeout; std::this_thread::yield(); return 0; }
  const uint64_t first = deliveredSamples.load();
  const uint64_t allowed = allowedSamples.load();
  assert(first <= allowed);
  size_t limit = readLimit;
  if (irregularReads) {
    static constexpr size_t sizes[] = {1, 137, 509, 1024, 17, 640};
    limit = sizes[readCounter++ % 6];
  }
  const size_t n = std::min<size_t>(std::min(requested, limit), allowed - first);
  if (!n) { std::this_thread::sleep_for(std::chrono::microseconds(100)); return 0; }
  for (size_t i = 0; i < n; ++i) pcm[i] = rawSample(first + i);
  clockMs += (first + n) / 16 - first / 16;
  deliveredSamples = first + n;
  return n;
}
enum class AllocPref { RequirePSRAM };
static std::mutex allocationMutex;
static std::map<void*, size_t> allocations;
static unsigned allocationAttempt = 0, failAllocation = 0;
void* ps_alloc(size_t n, AllocPref, const char*) {
  std::lock_guard<std::mutex> lock(allocationMutex);
  if (++allocationAttempt == failAllocation) return nullptr;
  void* p = std::malloc(n);
  if (p) { memset(p, 0xa5, n); allocations[p] = n; }
  return p;
}
static size_t allocationCount() { std::lock_guard<std::mutex> lock(allocationMutex); return allocations.size(); }
void trackedFree(void* p) {
  std::lock_guard<std::mutex> lock(allocationMutex);
  const auto it = allocations.find(p); assert(it != allocations.end());
  const auto* bytes = static_cast<uint8_t*>(p);
  for (size_t i = 0; i < it->second; ++i) assert(bytes[i] == 0);
  allocations.erase(it); std::free(p);
}
static std::atomic<unsigned> engineCalls{0}, enginePermits{UINT32_MAX};
static std::atomic<bool> modelAvailable{true}, engineOK{true}, engineOverlong{false};
static std::atomic<bool> ignoreEngineCancel{false}, engineActive{false}, cacheStartFails{false};
static std::atomic<unsigned> liveWeightCaches{0}, weightLoads{0}, weightReuses{0}, sessionCloses{0};
struct FakeWeights { STTToken token; unsigned id; };
struct EngineCall {
  size_t rawSamples, inferenceSamples;
  uint64_t start, end;
  uint32_t sequence;
  unsigned cacheId = 0;
  bool weightsReused = false;
};
static std::mutex engineMutex;
static std::vector<EngineCall> engineHistory;
bool sttLocalAvailable(char* error, size_t cap) {
  if (!modelAvailable) snprintf(error, cap, "Model missing");
  return modelAvailable.load();
}
// Definition follows the injected broker so it can check real queue metadata.
bool sttLocalTranscribe(const int16_t*, size_t, char*, size_t,
                        const STTLocalControl&, STTLocalStats&, char*, size_t, STTLocalSession*);
#define free trackedFree
// INSERT_BROKER
#undef free

static std::atomic<bool> savePreference{false}, saveFailure{false}, saveHeld{false}, saveEntered{false};
static std::atomic<bool> finishHeld{false}, finishEntered{false};
struct SavedTranscript {
  TranscriptOptions options;
  std::vector<std::pair<uint32_t, std::string>> chunks;
  std::string outcome;
};
static std::mutex saveMutex;
static std::map<STTToken, SavedTranscript> savedTranscripts;
static thread_local bool transcriptFinished = false;
TranscriptOptions transcriptCaptureOptions(CommandSource source, TransportSessionEpoch epoch) {
  assert(!criticalDepth);
  TranscriptOptions out; out.enabled = savePreference; out.source = source; out.epoch = epoch;
  strcpy(out.user, "alice"); return out;
}
static void assertSaveWorker() {
  assert(mainWorker && !criticalDepth && !taskSelfDeleted && !engineActive);
  std::lock_guard<std::mutex> lock(gSTTMux); assert(gSTT.snapshot.workerActive);
}
void TranscriptSession::begin(const TranscriptOptions& options, const char* provider, uint64_t id) {
  assertSaveWorker(); assert(std::string(provider) == "local");
  options_ = options; id_ = id; status_ = {}; status_.enabled = options.enabled;
  if (options.enabled && !options.user[0]) strcpy(status_.error, "Transcript owner unavailable");
  std::lock_guard<std::mutex> lock(saveMutex);
  assert(!savedTranscripts.count(id)); savedTranscripts[id].options = options;
}
bool TranscriptSession::append(uint32_t sequence, const char* text, size_t length) {
  assertSaveWorker(); assert(!finished_ && sequence > sequence_); sequence_ = sequence;
  if (!options_.enabled) return true;
  if (status_.error[0]) return false;
  saveEntered = true;
  while (saveHeld) std::this_thread::sleep_for(std::chrono::microseconds(100));
  if (saveFailure || (!status_.saved && !transportSessionEpochIsLive(options_.source, options_.epoch))) {
    strcpy(status_.error, "Injected save failure"); return false;
  }
  { std::lock_guard<std::mutex> lock(saveMutex);
    savedTranscripts[id_].chunks.emplace_back(sequence, std::string(text, length)); }
  ++status_.chunks; status_.bytes += length;
  if (length) { status_.saved = true; strcpy(status_.path, "/stt/u1/test.txt"); }
  return true;
}
void TranscriptSession::finish(const char* outcome) {
  assertSaveWorker(); assert(!audioCaptureOwnedBy("stt") && !finished_);
  finishEntered = true;
  while (finishHeld) std::this_thread::sleep_for(std::chrono::microseconds(100));
  finished_ = true; transcriptFinished = true;
  status_.complete = !status_.error[0];
  std::lock_guard<std::mutex> lock(saveMutex); savedTranscripts[id_].outcome = outcome;
}
static SavedTranscript saved(STTToken token) {
  std::lock_guard<std::mutex> lock(saveMutex); return savedTranscripts.at(token);
}

void STTLocalSession::reset() {
  if (!taskSelfDeleted) {
    assert(mainWorker && !backendResetBeforeDelete);
    assert(!engineActive && !audioCaptureOwnedBy("stt"));
    { std::lock_guard<std::mutex> lock(gSTTMux); assert(gSTT.snapshot.workerActive); }
    backendResetBeforeDelete = true;
    ++sessionCloses;
  }
  if (backendState_) {
    delete static_cast<FakeWeights*>(backendState_);
    backendState_ = nullptr;
    assert(liveWeightCaches.fetch_sub(1) == 1);
  }
}
STTLocalSession::~STTLocalSession() {
  // Host threads return and unwind after our vTaskDelete fake. Real FreeRTOS
  // does not: the deletion check below must already have seen explicit reset.
  assert(!backendState_);
  reset(); // Also verify that the reset after an explicit close is idempotent.
}
void vTaskDelete(void*) {
  if (mainWorker) {
    assert(backendResetBeforeDelete && liveWeightCaches == 0 && transcriptFinished);
    assert(!engineActive && !audioCaptureOwnedBy("stt"));
    { std::lock_guard<std::mutex> lock(gSTTMux); assert(!gSTT.snapshot.workerActive); }
  }
  taskSelfDeleted = true;
  ++deletedTasks;
}

static std::atomic<int> draftCalls{0};
bool sttLocalTranscribe(const int16_t* pcm, size_t count, char* text, size_t cap,
                        const STTLocalControl& control, STTLocalStats& stats,
                        char* error, size_t errorCap, STTLocalSession* session) {
  assert(mainWorker && session && !backendResetBeforeDelete);
  auto& run = *static_cast<StreamRun*>(control.context);
  if (pcm == run.draftPcm) {
    // Live-mode draft of the utterance in progress: never a queued segment.
    // These tests pin final-segment behaviour, so drafts decline (production
    // simply publishes no draft on failure) without touching the backend.
    { std::lock_guard<std::mutex> lock(gSTTMux); assert(run.draftBusy && count <= 320000); }
    ++draftCalls;
    snprintf(error, errorCap, "draft declined by harness");
    return false;
  }
  EngineCall call{};
  {
    std::lock_guard<std::mutex> lock(gSTTMux);
    for (const auto& slot : run.slots) if (slot.pcm == pcm) {
      assert(slot.state == StreamSlotState::Processing);
      call = {slot.samples, count, slot.start, slot.end, slot.sequence};
    }
  }
  assert(call.sequence && call.end - call.start == call.rawSamples);
  assert(count == std::max(call.rawSamples, size_t(320)) && count <= 320000);
  assert(allocationCount() == 5);
  auto assertPCM = [&] {
    for (size_t i = 0; i < call.rawSamples; ++i) assert(pcm[i] == rawSample(call.start + i));
    for (size_t i = call.rawSamples; i < count; ++i) assert(pcm[i] == 0);
  };
  assertPCM();
  if (cacheStartFails) {
    assert(!session->backendState_);
    snprintf(error, errorCap, "Injected cache initialization failure");
    return false;
  }
  call.weightsReused = session->backendState_ != nullptr;
  if (!session->backendState_) {
    assert(liveWeightCaches.fetch_add(1) == 0);
    session->backendState_ = new FakeWeights{run.token, ++weightLoads};
  } else ++weightReuses;
  const auto& weights = *static_cast<FakeWeights*>(session->backendState_);
  assert(weights.token == run.token); // A successor must receive fresh ownership.
  call.cacheId = weights.id;
  {
    std::lock_guard<std::mutex> lock(engineMutex); engineHistory.push_back(call);
  }
  engineActive = true;
  const unsigned number = ++engineCalls;
  control.progress(control.context, STTLocalPhase::Inference);
  while (enginePermits < number && (ignoreEngineCancel || !control.cancelled(control.context)))
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  assertPCM(); // The producer may have filled/reused other slots meanwhile.
  stats.samples = count; stats.inferenceMs = 123; stats.weightsReused = call.weightsReused;
  if (!engineOK) snprintf(error, errorCap, "Injected inference failure");
  if (engineOverlong) memset(text, 'x', cap);
  else snprintf(text, cap, "segment %u", call.sequence);
  engineActive = false;
  return engineOK.load() && !control.cancelled(control.context);
}

static const STTOwner owner{SOURCE_SERIAL, 5}, other{SOURCE_WEB, 5}, successor{SOURCE_SERIAL, 6};
static void await(const std::function<bool()>& predicate, const char* what) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      fprintf(stderr, "Timed out waiting for %s\n", what); std::abort();
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}
static void joinTasks() {
  await([] { return activeTasks == 0; }, "all workers to return");
  std::vector<std::thread> joined;
  { std::lock_guard<std::mutex> lock(tasksMutex); joined.swap(tasks); }
  for (auto& task : joined) task.join();
  assert(allocationCount() == 0 && !audioCaptureBusy() && !engineActive && liveWeightCaches == 0);
}
static STTSnapshot snapshot(STTToken token) {
  STTSnapshot out; assert(sttSnapshot(owner, token, &out)); return out;
}
static void reset() {
  joinTasks();
  gSTT = STTRun{}; memset(gSTTChunks, 0, sizeof(gSTTChunks));
  clockMs = 1; deletedTasks = 0;
  serialLive = webLive = successorLive = true;
  failedTask.clear(); allocationAttempt = failAllocation = 0;
  srRunning = gMicRunning = recordingBusy = false;
  gSettings.micSource = "auto";
  audioSource = AUDIO_SRC_LOCAL_PDM; halOwner.clear(); halActive = false;
  sourceAvailable = integrityAvailable = true; startFails = fallbackOnStart = false;
  stopHeld = stopRequested = invalidRead = readTimeout = false; halOverruns = 0;
  gateNextIntegrity = integrityGateEntered = releaseIntegrityGate = false;
  allowedSamples = deliveredSamples = 0; starts = stops = 0;
  readLimit = 1024; readCounter = 0; irregularReads = false;
  allVoice = true; dc = 0; voiceIntervals.clear();
  modelAvailable = engineOK = true; engineOverlong = ignoreEngineCancel = engineActive = false;
  engineCalls = 0; enginePermits = UINT32_MAX; engineHistory.clear();
  cacheStartFails = false; weightLoads = weightReuses = sessionCloses = 0;
  savePreference = saveFailure = saveHeld = saveEntered = finishHeld = finishEntered = false;
  savedTranscripts.clear();
}
static STTToken begin() {
  allowedSamples = deliveredSamples = 0; readCounter = 0;
  STTToken token = 0; char error[96] = {};
  assert(sttBeginContinuous(owner, &token, error, sizeof(error)) && token && !error[0]);
  return token;
}
static void started(STTToken token) {
  const unsigned previousEngineCalls = engineCalls.load();
  await([] { return audioCaptureOwnedBy("stt"); }, "HAL startup");
  // The real broker must opt into calibration and retain Preparing until the
  // complete500ms ambient period. Readiness must not rely on fake VAD settings.
  assert(snapshot(token).state == STTState::Preparing);
  allowedSamples = kCalibrationSamples;
  await([&] { return snapshot(token).state == STTState::Recording; }, "adaptive capture readiness");
  const auto s = snapshot(token);
  assert(s.captureActive && s.recordedSamples == kCalibrationSamples && engineCalls == previousEngineCalls);
}
static void feed(STTToken token, uint64_t totalSamples) {
  totalSamples += kCalibrationSamples; // All test durations below are after readiness.
  allowedSamples = totalSamples;
  await([&] { const auto s = snapshot(token); return s.recordedSamples >= totalSamples || !s.workerActive; }, "PCM credit consumed");
  assert(snapshot(token).recordedSamples == totalSamples);
}
static void complete(STTToken token) {
  await([&] { return !sttRunActive(token); }, "terminal session");
  // Check before host stack unwinding could hide a missing FreeRTOS close.
  assert(liveWeightCaches == 0);
  joinTasks();
  assert(gSettings.micSource == "auto" && gSettings.microphoneGain == 70 && gSettings.microphoneSampleRate == 48000);
}
static STTTextChunk peek(STTToken token) {
  STTTextChunk out; assert(sttReadChunk(owner, token, &out)); return out;
}
static void rejectBegin() {
  STTToken token = 0; char error[96] = {};
  assert(!sttBeginContinuous(owner, &token, error, sizeof(error)) && error[0]);
}
static void acknowledgeAll(STTToken token, std::vector<STTTextChunk>* saved = nullptr) {
  STTTextChunk chunk;
  while (sttReadChunk(owner, token, &chunk)) {
    if (saved) saved->push_back(chunk);
    assert(sttAcknowledgeChunk(owner, token, chunk.sequence));
  }
}
static void testAdaptiveReadiness() {
  reset(); irregularReads = true;
  const auto token = begin();
  await([] { return audioCaptureOwnedBy("stt"); }, "calibration HAL claim");
  assert(snapshot(token).state == STTState::Preparing);
  allowedSamples = kCalibrationSamples - 1;
  await([&] { return snapshot(token).recordedSamples == kCalibrationSamples - 1; }, "incomplete calibration");
  assert(snapshot(token).state == STTState::Preparing && engineCalls == 0);
  allowedSamples = kCalibrationSamples;
  await([&] { return snapshot(token).state == STTState::Recording; }, "exact calibration boundary");
  assert(snapshot(token).recordedSamples == kCalibrationSamples && engineCalls == 0);
  // Start speaking on the first sample after readiness. A250ms utterance is
  // enough to qualify, even though normal pause segments have an8s floor.
  feed(token, 4000);
  assert(sttRequestFinish(owner, token)); complete(token);
  assert(snapshot(token).state == STTState::Done && engineHistory.size() == 1);
  const auto first = peek(token);
  assert(first.startSample == kCalibrationSamples && first.endSample == kCalibrationSamples + 4000);
  assert(engineHistory[0].rawSamples == 4000 && engineHistory[0].inferenceSamples == 4000);

  reset(); const auto early = begin();
  await([] { return audioCaptureOwnedBy("stt"); }, "early-stop calibration claim");
  allowedSamples = kCalibrationSamples / 2;
  await([&] { return snapshot(early).recordedSamples == kCalibrationSamples / 2; }, "partial calibration consumed");
  assert(snapshot(early).state == STTState::Preparing);
  assert(sttRequestFinish(owner, early)); complete(early);
  assert(snapshot(early).state == STTState::Done && engineCalls == 0 && !snapshot(early).pendingTexts);
}
static void testOverlapAndDrain() {
  reset(); enginePermits = 0; irregularReads = true;
  const auto token = begin(); started(token);
  feed(token, 20 * 16000); await([] { return engineCalls == 1; }, "first blocked inference");
  const auto before = snapshot(token); assert(before.captureActive && before.inferenceActive);
  feed(token, 45 * 16000); // Capture must advance by 25 s while inference is blocked.
  assert(engineCalls == 1 && snapshot(token).segmentsCaptured == 2);
  assert(sttRequestFinish(owner, token));
  await([] { return stopRequested.load(); }, "normal capture stop");
  assert(sttRunActive(token) && allocationCount() == 5);
  assert(snapshot(token).segmentsCaptured == 3);
  enginePermits = UINT32_MAX; complete(token);
  auto s = snapshot(token); assert(s.state == STTState::Done && s.segmentsCompleted == 3 && s.recordedSamples == kCalibrationSamples + 720000);
  assert(starts == 1 && stops == 1 && deletedTasks == 2);
  assert(weightLoads == 1 && weightReuses == 2 && sessionCloses == 1);
  assert(!engineHistory[0].weightsReused && engineHistory[1].weightsReused && engineHistory[2].weightsReused);
  for (const auto& call : engineHistory) assert(call.cacheId == engineHistory[0].cacheId);
  std::vector<STTTextChunk> result; acknowledgeAll(token, &result);
  assert(result.size() == 3);
  uint64_t end = kCalibrationSamples;
  for (size_t i = 0; i < result.size(); ++i) {
    assert(result[i].sequence == i + 1 && result[i].startSample == end);
    end = result[i].endSample;
    assert(result[i].forcedBoundary == (i < 2));
  }
  assert(end == kCalibrationSamples + 45 * 16000);
}
static void testLongRunsAndPauses() {
  reset(); const auto token = begin(); started(token);
  for (unsigned seconds : {20u, 40u, 60u}) {
    feed(token, seconds * 16000);
    await([&] { return snapshot(token).segmentsCompleted == seconds / 20; }, "hard-cut inference");
  }
  feed(token, 65 * 16000); assert(sttRequestFinish(owner, token)); complete(token);
  auto s = snapshot(token); assert(s.state == STTState::Done && s.recordedSamples == kCalibrationSamples + 1040000 && s.sessionMs >= 65500);
  std::vector<STTTextChunk> chunks; acknowledgeAll(token, &chunks);
  assert(chunks.size() == 4 && chunks.back().endSample == kCalibrationSamples + 1040000);
  for (size_t i = 0; i < chunks.size(); ++i) {
    assert(chunks[i].startSample == kCalibrationSamples + i * 320000 && chunks[i].endSample - chunks[i].startSample <= 320000);
    assert(chunks[i].forcedBoundary == (i < 3));
  }

  reset(); allVoice = false; dc = 11000; irregularReads = true;
  for (unsigned i = 0; i < 10; ++i) voiceIntervals.push_back({i * 160000, i * 160000 + 16000});
  const auto paused = begin(); started(paused);
  uint64_t previousEnd = kCalibrationSamples;
  for (unsigned i = 0; i < 10; ++i) {
    feed(paused, (i + 1) * 160000);
    await([&] { return snapshot(paused).segmentsCompleted == i + 1; }, "minimum-span pause segment");
    const auto chunk = peek(paused);
    assert(chunk.sequence == i + 1 && !chunk.forcedBoundary);
    assert(chunk.endSample - chunk.startSample == 128000); // Default 8 s throughput floor.
    assert(chunk.startSample >= previousEnd);
    previousEnd = chunk.endSample;
    assert(sttAcknowledgeChunk(owner, paused, chunk.sequence));
  }
  assert(sttRequestFinish(owner, paused)); complete(paused);
  assert(snapshot(paused).state == STTState::Done && engineCalls == 10 && snapshot(paused).sessionMs >= 100000);

  reset(); allVoice = false; dc = 16000;
  const auto silent = begin(); started(silent); feed(silent, 120 * 16000);
  assert(engineCalls == 0 && snapshot(silent).segmentsCaptured == 0);
  assert(sttRequestFinish(owner, silent)); complete(silent);
  assert(snapshot(silent).state == STTState::Done && !snapshot(silent).textReady);
}
static void testShortFinalTail() {
  reset(); const auto token = begin(); started(token);
  feed(token, 320000); await([&] { return snapshot(token).segmentsCompleted == 1; }, "first hard cut");
  feed(token, 320037); assert(sttRequestFinish(owner, token)); complete(token);
  assert(snapshot(token).state == STTState::Done && snapshot(token).recordedSamples == kCalibrationSamples + 320037);
  assert(engineHistory.size() == 2 && engineHistory[1].rawSamples == 37 && engineHistory[1].inferenceSamples == 320);
  assert(sttAcknowledgeChunk(owner, token, 1));
  const auto tail = peek(token); assert(tail.startSample == kCalibrationSamples + 320000 && tail.endSample == kCalibrationSamples + 320037 && !tail.forcedBoundary);
}
static void testCancelAndJoin() {
  reset(); enginePermits = 0; ignoreEngineCancel = true; stopHeld = true;
  const auto token = begin(); started(token); feed(token, 320000);
  await([] { return engineCalls == 1; }, "blocked model");
  feed(token, 700000);
  assert(sttCancel(owner, token));
  await([] { return stopRequested.load(); }, "HAL asynchronous stop");
  assert(sttRunActive(token) && allocationCount() == 5 && engineActive);
  assert(liveWeightCaches == 1 && sessionCloses == 0);
  rejectBegin();
  enginePermits = UINT32_MAX;
  await([] { return !engineActive.load(); }, "model cooperative join");
  assert(sttRunActive(token) && allocationCount() == 5); // HAL still owns caller memory/lifecycle.
  assert(liveWeightCaches == 1 && sessionCloses == 0);
  releaseHALStop(); complete(token);
  assert(snapshot(token).state == STTState::Cancelled && engineCalls == 1 && !snapshot(token).pendingTexts);
  STTTextChunk out; assert(!sttReadChunk(owner, token, &out) && !out.text[0]);
  for (const auto& chunk : gSTTChunks) for (char c : chunk.text) assert(c == 0);
}
static void testQueueBounds() {
  reset(); enginePermits = 0;
  const auto token = begin(); started(token); feed(token, 320000);
  await([] { return engineCalls == 1; }, "first blocked model");
  allowedSamples = kCalibrationSamples + 1280000; // Fourth 20 s segment cannot fit three occupied slots.
  complete(token);
  auto s = snapshot(token);
  assert(s.state == STTState::Failed && strstr(s.error, "audio queue full"));
  assert(s.segmentsCaptured == 3 && s.segmentsCompleted == 0 && !s.pendingAudio);

  reset(); const auto textFull = begin(); started(textFull);
  for (unsigned i = 1; i <= 8; ++i) {
    feed(textFull, i * 320000);
    await([&] { return snapshot(textFull).segmentsCompleted == i; }, "unacknowledged text");
  }
  allowedSamples = kCalibrationSamples + 9 * 320000; complete(textFull); s = snapshot(textFull);
  assert(s.state == STTState::Failed && strstr(s.error, "text queue full"));
  assert(s.pendingTexts == 8 && s.segmentsCompleted == 8 && engineCalls == 9);
  for (uint32_t sequence = 1; sequence <= 8; ++sequence) {
    assert(peek(textFull).sequence == sequence);
    assert(sttAcknowledgeChunk(owner, textFull, sequence));
  }
  assert(!snapshot(textFull).textReady);
}
static void testDeliveryAndRevocation() {
  reset(); const auto token = begin(); started(token);
  for (unsigned i = 1; i <= 3; ++i) {
    feed(token, i * 320000);
    await([&] { return snapshot(token).segmentsCompleted == i; }, "delivery fixture");
  }
  STTTextChunk denied{};
  assert(!sttReadChunk(other, token, &denied) && !sttReadChunk(successor, token, &denied));
  assert(!sttReadChunk(owner, token + 1, &denied));
  assert(!sttAcknowledgeChunk(other, token, 1) && !sttAcknowledgeChunk(successor, token, 1));
  assert(!sttAcknowledgeChunk(owner, token, 0) && !sttAcknowledgeChunk(owner, token, 2));
  const auto first = peek(token), retry = peek(token);
  assert(first.sequence == 1 && retry.sequence == 1 && std::string(first.text) == retry.text);
  assert(sttAcknowledgeChunk(owner, token, 1) && sttAcknowledgeChunk(owner, token, 1));
  assert(peek(token).sequence == 2 && !sttAcknowledgeChunk(owner, token, 3));
  assert(sttAcknowledgeChunk(owner, token, 2) && sttAcknowledgeChunk(owner, token, 1));
  assert(peek(token).sequence == 3);
  serialLive = false; // Revocation cancels capture and wipes even already queued private text.
  await([&] { return !sttRunActive(token); }, "revocation shutdown"); joinTasks();
  assert(gSTT.snapshot.state == STTState::Cancelled && !gSTT.snapshot.pendingTexts);
  assert(weightLoads == 1 && weightReuses == 2 && sessionCloses == 1 && liveWeightCaches == 0);
  memset(denied.text, 'x', sizeof(denied.text));
  assert(!sttReadChunk(owner, token, &denied) && !denied.text[0]);
  assert(!sttReadChunk(successor, token, &denied) && !sttAcknowledgeChunk(successor, token, 3));
  assert(!sttCancel(owner, token) && !sttRequestFinish(owner, token));
  serialLive = true;
  const auto next = begin(); assert(next != token && !sttCancel(owner, token)); started(next);
  feed(next, 16000);
  assert(sttRequestFinish(owner, next)); complete(next);
  assert(weightLoads == 2 && weightReuses == 2 && sessionCloses == 2);
  assert(!engineHistory.back().weightsReused && engineHistory.back().cacheId != engineHistory.front().cacheId);
}
static void testStopIntegrityRace() {
  reset(); halOverruns = 4;
  auto token = begin(); complete(token);
  assert(snapshot(token).state == STTState::Failed && snapshot(token).audioOverruns == 4);
  assert(strstr(snapshot(token).error, "startup") && engineCalls == 0);

  reset(); token = begin(); started(token); feed(token, 16000);
  gateNextIntegrity = true;
  await([] { return integrityGateEntered.load(); }, "sampled pre-stop integrity counter");
  assert(sttRequestFinish(owner, token));
  halOverruns = 3;
  releaseIntegrityGate = true;
  complete(token);
  const auto s = snapshot(token);
  assert(s.state == STTState::Failed && s.audioOverruns == 3 && strstr(s.error, "at stop"));
  assert(s.recordedSamples == kCalibrationSamples + 16000 && engineCalls == 0 && !s.pendingTexts);
  assert(s.captureStackFreeBytes == 2048);
}
static void testFaults() {
  for (unsigned allocation = 1; allocation <= 4; ++allocation) {
    reset(); failAllocation = allocation; const auto token = begin(); complete(token);
    assert(snapshot(token).state == STTState::Failed && strstr(snapshot(token).error, "PSRAM") && starts == 0);
  }
  reset(); failedTask = "stt";
  STTToken token = 0; char error[96];
  assert(!sttBeginContinuous(owner, &token, error, sizeof(error)) && token && !sttRunActive(token));
  assert(snapshot(token).state == STTState::Failed && allocationCount() == 0);
  reset(); failedTask = "stt_capture"; token = begin(); complete(token);
  assert(snapshot(token).state == STTState::Failed && strstr(snapshot(token).error, "capture worker") && starts == 0);
  reset(); startFails = true; token = begin(); complete(token);
  assert(snapshot(token).state == STTState::Failed && starts == 0);
  reset(); integrityAvailable = false; token = begin(); complete(token);
  assert(snapshot(token).state == STTState::Failed && strstr(snapshot(token).error, "integrity") && stops == 1);
  reset(); invalidRead = true; token = begin(); complete(token);
  assert(snapshot(token).state == STTState::Failed && strstr(snapshot(token).error, "sample count"));
  reset(); readTimeout = true; token = begin(); complete(token);
  assert(snapshot(token).state == STTState::Failed && strstr(snapshot(token).error, "timed out"));
  reset(); token = begin(); started(token); halOverruns = 7; allowedSamples = kCalibrationSamples + 1024; complete(token);
  assert(snapshot(token).state == STTState::Failed && snapshot(token).audioOverruns == 7 && strstr(snapshot(token).error, "audio loss"));
  reset(); token = begin(); started(token); sourceAvailable = false; complete(token);
  assert(snapshot(token).state == STTState::Failed && strstr(snapshot(token).error, "source lost"));
  reset(); cacheStartFails = true; token = begin(); started(token); allowedSamples = kCalibrationSamples + 320000; complete(token);
  assert(snapshot(token).state == STTState::Failed && strstr(snapshot(token).error, "initialization"));
  assert(weightLoads == 0 && sessionCloses == 1 && liveWeightCaches == 0);
  reset(); engineOK = false; token = begin(); started(token); allowedSamples = kCalibrationSamples + 320000; complete(token);
  assert(snapshot(token).state == STTState::Failed && strstr(snapshot(token).error, "Injected") && !snapshot(token).pendingTexts);
  assert(weightLoads == 1 && weightReuses == 0 && sessionCloses == 1 && liveWeightCaches == 0);
  reset(); engineOverlong = true; token = begin(); started(token); allowedSamples = kCalibrationSamples + 320000; complete(token);
  assert(snapshot(token).state == STTState::Failed && !snapshot(token).pendingTexts);
  reset(); token = begin(); started(token);
  { std::lock_guard<std::mutex> lock(gSTTMux); gSTT.snapshot.segmentsCaptured = UINT32_MAX; }
  allowedSamples = kCalibrationSamples + 320000; complete(token);
  assert(snapshot(token).state == STTState::Failed && strstr(snapshot(token).error, "sequence exhausted"));
}
static void testTranscriptSaving() {
  reset(); savePreference = true; saveHeld = true;
  auto token = begin(); savePreference = false; started(token); feed(token, 320000);
  await([] { return saveEntered.load(); }, "accepted chunk reaches saving worker");
  // Publication/ACK can run while slow storage holds its own complete copy.
  auto first = peek(token); assert(first.sequence == 1);
  assert(sttReadChunk(owner, token, &first));
  assert(sttAcknowledgeChunk(owner, token, 1) && sttAcknowledgeChunk(owner, token, 1));
  assert(saved(token).chunks.empty() && snapshot(token).workerActive);
  saveHeld = false;
  await([&] { return saved(token).chunks.size() == 1; }, "first saved chunk");
  feed(token, 640000);
  await([&] { return saved(token).chunks.size() == 2; }, "second saved chunk despite setting off");
  finishHeld = true; assert(sttRequestFinish(owner, token));
  await([] { return finishEntered.load(); }, "saving finalization");
  assert(sttRunActive(token) && saved(token).outcome.empty());
  rejectBegin(); finishHeld = false; complete(token);
  const auto accepted = saved(token);
  assert(accepted.options.enabled && accepted.chunks.size() == 2 && accepted.outcome == "done");
  assert(accepted.chunks[0].first == 1 && accepted.chunks[0].second == "segment 1");
  assert(snapshot(token).transcript.saved && snapshot(token).transcript.complete && snapshot(token).transcript.chunks == 2);
  assert(sttCancel(owner, token)); // Mailbox retirement must not rewrite finished files.
  assert(saved(token).outcome == "done" && saved(token).chunks.size() == 2);

  reset(); token = begin(); savePreference = true; started(token); feed(token, 16000);
  assert(sttRequestFinish(owner, token)); complete(token);
  assert(!saved(token).options.enabled && saved(token).chunks.empty());
  assert(!snapshot(token).transcript.enabled && snapshot(token).state == STTState::Done);

  reset(); savePreference = true; saveFailure = true; token = begin(); started(token); feed(token, 16000);
  assert(sttRequestFinish(owner, token)); complete(token);
  assert(snapshot(token).state == STTState::Done && !snapshot(token).error[0]);
  assert(snapshot(token).transcript.error[0] && peek(token).text[0]);

  reset(); savePreference = true; token = begin(); started(token); feed(token, 320000);
  await([&] { return saved(token).chunks.size() == 1; }, "accepted words before cancel");
  assert(sttCancel(owner, token)); complete(token);
  assert(saved(token).chunks.size() == 1 && saved(token).outcome == "cancelled");
  assert(snapshot(token).state == STTState::Cancelled && !snapshot(token).pendingTexts);

  reset(); savePreference = true; enginePermits = 0; token = begin(); started(token); feed(token, 320000);
  await([] { return engineCalls == 1; }, "inference before acceptance");
  assert(sttCancel(owner, token)); complete(token);
  assert(saved(token).chunks.empty() && saved(token).outcome == "cancelled");

  reset(); savePreference = true; finishHeld = true; token = begin(); started(token);
  feed(token, 16000); assert(sttRequestFinish(owner, token));
  await([] { return finishEntered.load(); }, "final outcome fixed before finish I/O");
  assert(sttCancel(owner, token)); finishHeld = false; complete(token);
  assert(snapshot(token).state == STTState::Cancelled && !snapshot(token).pendingTexts);
  assert(saved(token).outcome == "done" && saved(token).chunks.size() == 1);

  reset(); savePreference = false;
  const auto uiOptions = [] { TranscriptOptions o; o.enabled = true; o.source = owner.source;
    o.epoch = owner.epoch; strcpy(o.user, "alice"); return o; }();
  char error[96]; token = 0;
  assert(sttBeginContinuous(owner, &token, error, sizeof(error), &uiOptions));
  started(token); feed(token, 16000); assert(sttRequestFinish(owner, token)); complete(token);
  assert(saved(token).options.enabled && saved(token).chunks.size() == 1);

  for (bool wrongSource : {false, true}) {
    reset(); auto mismatched = uiOptions;
    if (wrongSource) mismatched.source = SOURCE_WEB; else ++mismatched.epoch;
    token = 0;
    assert(sttBeginContinuous(owner, &token, error, sizeof(error), &mismatched));
    started(token); feed(token, 16000); assert(sttRequestFinish(owner, token)); complete(token);
    assert(snapshot(token).state == STTState::Done && snapshot(token).transcript.error[0]);
    assert(saved(token).chunks.empty() && saved(token).options.user[0] == 0);
  }
}
static void testWideCountersAndRetention() {
  // White-box boundary injection avoids simulating days of microphone input.
  // Exercise actual accumulation through two uint32 millis wraps, and public
  // delivery of >2^32 absolute sample positions without truncation.
  reset(); StreamRun stream; stream.clockMs = 0;
  clockMs = 0xfffffff0ULL; sttStreamTick(stream);
  clockMs = 0x100000020ULL; sttStreamTick(stream);
  clockMs = 0x1fffffff0ULL; sttStreamTick(stream);
  clockMs = 0x200000020ULL; sttStreamTick(stream);
  assert(gSTT.snapshot.sessionMs == 0x200000020ULL);
  gSTT.owner = owner; gSTT.snapshot.token = 0x100000009ULL;
  gSTT.snapshot.continuous = true; gSTT.snapshot.state = STTState::Done;
  gSTT.snapshot.recordedSamples = 0x100040000ULL;
  gSTT.snapshot.pendingTexts = 1; gSTT.snapshot.textReady = true; gSTT.completedMs = millis();
  gSTTChunks[0].sequence = 1; gSTTChunks[0].startSample = 0x100000003ULL;
  gSTTChunks[0].endSample = 0x100040000ULL; strcpy(gSTTChunks[0].text, "wide");
  const auto token = gSTT.snapshot.token;
  assert(snapshot(token).recordedSamples == 0x100040000ULL);
  const auto chunk = peek(token); assert(chunk.startSample == 0x100000003ULL && chunk.endSample == 0x100040000ULL);
  assert(sttAcknowledgeChunk(owner, token, 1));
  gSTT.snapshot.pendingTexts = 1; gSTT.snapshot.textReady = true; gSTT.textHead = 0;
  strcpy(gSTTChunks[0].text, "private"); clockMs += 300000;
  STTTextChunk expired; assert(!sttReadChunk(owner, token, &expired) && !expired.text[0]);
  assert(!gSTT.snapshot.pendingTexts);
}
int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  testAdaptiveReadiness(); puts("PASS real adaptive500ms readiness, immediate speech, calibration stop");
  testOverlapAndDrain(); puts("PASS concurrent capture, exact PCM, stop/drain, zeroed cleanup");
  testLongRunsAndPauses(); puts("PASS 65 s hard cuts, 100 s pauses, 120 s DC-only silence");
  testShortFinalTail(); puts("PASS inference-only 37-to-320 sample padding and exact offsets");
  testCancelAndJoin(); puts("PASS cancellation joins both blocked inference and delayed HAL stop");
  testQueueBounds(); puts("PASS explicit audio/text queue pressure without overwrite");
  testDeliveryAndRevocation(); puts("PASS retry/ack ordering, transport/epoch/token fencing, revocation");
  testStopIntegrityRace(); puts("PASS initial audio loss and simultaneous finish/overrun between reads");
  testFaults(); puts("PASS allocation/task/HAL/model faults and sequence exhaustion");
  testWideCountersAndRetention(); puts("PASS 64-bit time/positions and result expiry");
  testTranscriptSaving(); puts("PASS transcript admission latch, ACK-independent saving, save failure, cancel and finalization");
  reset(); puts("Continuous production STT broker multithread tests passed, including explicit backend-session cleanup");
}
