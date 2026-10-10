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
enum AudioSource { AUDIO_SRC_NONE, AUDIO_SRC_LOCAL_PDM, AUDIO_SRC_G2_LEFT };
// INSERT_INTERFACE
// INSERT_FRONTEND
using portMUX_TYPE = std::mutex;
using TaskHandle_t = void*;
constexpr int pdTRUE = 1;
static std::atomic<unsigned> feNotifies{0};
void xTaskNotifyGive(TaskHandle_t) { ++feNotifies; }
uint32_t ulTaskNotifyTake(int, uint32_t) { std::this_thread::sleep_for(std::chrono::microseconds(100)); return 0; }
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
  assert((core == 1 && std::string(name) == "stt" && bytes == 12288 && priority == 2) ||
         (core == 1 && std::string(name) == "stt_warm" && bytes == 12288 && priority == 1) ||
         (core == 1 && std::string(name) == "stt_capture" && bytes >= 4096 && bytes <= 8192 && priority == 3) ||
         (core == 0 && std::string(name) == "stt_fe" && bytes == 3072 && priority == 1));
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
static std::mutex halMutex;
static AudioSource audioSource = AUDIO_SRC_LOCAL_PDM;
static std::string halOwner;
static bool halActive = false;
static std::atomic<bool> sourceAvailable{true}, startFails{false}, fallbackOnStart{false};
static std::atomic<bool> g2Available{true}; // The glasses alone can be out of range.
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
bool audioSourceAvailable(AudioSource s) {
  return s != AUDIO_SRC_NONE && sourceAvailable.load() && (s != AUDIO_SRC_G2_LEFT || g2Available.load());
}
bool audioAnySourceAvailable() { return sourceAvailable.load(); }
bool audioSetSource(AudioSource s) {
  std::lock_guard<std::mutex> lock(halMutex);
  if (!halOwner.empty() || (s != AUDIO_SRC_NONE && !audioSourceAvailable(s))) return false;
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
// Conversate's native glasses stream: claims the left-temple source for one
// connection generation (the HAL never selects it any other way).
static std::atomic<uint32_t> nativeStarts{0};
// publish wake: the consumer wake the native session registers. Must run on the STT
// worker, outside gSTTMux; counted per published final/draft.
static std::atomic<unsigned> publishWakes{0}, wakesWithText{0};
bool audioCaptureStartG2Native(const char* owner, uint32_t generation) {
  std::lock_guard<std::mutex> lock(halMutex);
  if (startFails || !halOwner.empty() || !generation) return false;
  halOwner = owner; halActive = true; ++starts; ++nativeStarts;
  audioSource = AUDIO_SRC_G2_LEFT;
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
static std::atomic<unsigned> fePoolAllocs{0};  // native: capture-features pools allocated (one per session)
void* ps_alloc(size_t n, AllocPref, const char* name) {
  std::lock_guard<std::mutex> lock(allocationMutex);
  if (++allocationAttempt == failAllocation) return nullptr;
  if (name && !strcmp(name, "stt.fe_ring")) ++fePoolAllocs;
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
// Keep-warm: the model may outlive a run only in these tests (and only while
// the broker holds it parked); the warm loader can be held at a gate or fail.
static std::atomic<bool> warmTest{false}, warmLoadHeld{false}, warmLoadFails{false}, warmLoaderEntered{false};
static std::atomic<unsigned> warmMoves{0};
static std::atomic<size_t> psramFree{SIZE_MAX};
static std::atomic<size_t> warmLoadTakes{0};  // PSRAM a successful warm load takes
size_t sttPsramFree() { return psramFree.load(); }
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
                        const STTLocalControl&, STTLocalStats&, char*, size_t, STTLocalSession*,
                        const STTFeaturePrefix*);
// The runtime's per-call PSRAM rule (capture-features sizes its pool against it); the host
// fake PSRAM has no free-size accounting, so only the call is checked.
static std::atomic<unsigned> callBudgetQueries{0};
STTLocalCallBudget sttLocalCallBudget(size_t samples, const STTLocalSession* session) {
  assert(samples == 320000 && session); ++callBudgetQueries;
  return {3u << 20, 3u << 20};
}
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
  if (this == &gWarmSession) {  // the loader, a parking/adopting worker or the releaser
    if (backendState_) {
      delete static_cast<FakeWeights*>(backendState_);
      backendState_ = nullptr;
      assert(liveWeightCaches.fetch_sub(1) == 1);
    }
    return;
  }
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
bool STTLocalSession::warm() const { return backendState_ != nullptr; }
void STTLocalSession::moveTo(STTLocalSession& to) {
  assert(warmTest && !to.backendState_ && !engineActive);
  // A parked model belongs to no run until a run's next call takes it.
  if (backendState_) static_cast<FakeWeights*>(backendState_)->token = 0;
  to.backendState_ = backendState_;
  backendState_ = nullptr;
  ++warmMoves;
}
// The fake keeps no resident model: nothing a call leaves behind to drop.
static std::atomic<unsigned> callStateDrops{0};
void STTLocalSession::dropCallState() { ++callStateDrops; }
STTLocalSession::~STTLocalSession() {
  // Host threads return and unwind after our vTaskDelete fake. Real FreeRTOS
  // does not: the deletion check below must already have seen explicit reset.
  assert(!backendState_);
  reset(); // Also verify that the reset after an explicit close is idempotent.
}
void vTaskDelete(void*) {
  if (mainWorker) {
    // Only a model the broker parked for the next run may outlive the worker.
    assert(backendResetBeforeDelete && transcriptFinished &&
           (liveWeightCaches == 0 || (warmTest && liveWeightCaches == 1 && sttWarmResident())));
    assert(!engineActive && !audioCaptureOwnedBy("stt"));
    { std::lock_guard<std::mutex> lock(gSTTMux); assert(!gSTT.snapshot.workerActive); }
  }
  taskSelfDeleted = true;
  ++deletedTasks;
}

static std::atomic<int> draftCalls{0};
static std::atomic<bool> draftsSucceed{false};   // native: let drafts publish (publish wake draft wake)
static std::atomic<unsigned> draftsPublished{0};
static std::atomic<int> trimmedCalls{0};
struct SpecCall { uint64_t start; size_t samples; uint32_t sequence; };
static std::vector<SpecCall> specHistory;
// Phrase `sequence` was transcribed exactly once on `samples` of input (0: any):
// by one final run, or with no final run by its promoted speculative run. Short
// closes speculate too, so when a test feeds a whole phrase at once capture
// timing decides which of the two it is.
static void assertTranscribedOnce(uint32_t sequence, size_t samples = 0);
static std::atomic<int> specCalls{0}, specAborts{0};
static std::atomic<bool> specHold{false};
static StreamRun* gRun = nullptr;
static std::atomic<unsigned> cachedFinals{0}, cachedRows{0}, cachedDrafts{0}, rowMismatches{0};
static std::atomic<unsigned> cachedSpecs{0}, cachedTrimmedFinals{0};  // features x speculation / features x trim
// capture-features: every cached row must equal the batch frontend's row on this exact PCM.
// kind: 0 final, 1 draft, 2 speculative final (combined).
static void checkPrefix(const int16_t* pcm, size_t count, const STTFeaturePrefix* prefix, int kind) {
  if (!prefix) return;
  assert(prefix->rows && prefix->count && prefix->count <= hw1::stt::feature_frames(count));
  hw1::stt::FrontendWorkspace w;
  float row[64];
  // combined: finals and speculative finals are checked row for row. A draft is
  // checked on its first/last 8 rows and every 16th: a full -O0 check of a
  // long draft held the worker long enough that speculation's speculative snapshot
  // (copied by the capture task only while no draft runs, at the next block)
  // never happened once a test stopped feeding audio. Production drafts are
  // preempted between model stages instead.
  for (size_t r = 0; r < prefix->count; ++r) {
    if (kind == 1 && r >= 8 && r + 8 < prefix->count && r % 16) continue;
    assert(hw1::stt::log_mel_frame(pcm, count, r, row, &w) == hw1::stt::Status::Ok);
    if (memcmp(row, prefix->rows + r * 64, sizeof(row))) ++rowMismatches;
  }
  (kind == 1 ? cachedDrafts : kind == 2 ? cachedSpecs : cachedFinals) += 1; cachedRows += unsigned(prefix->count);
}
bool sttLocalTranscribe(const int16_t* pcm, size_t count, char* text, size_t cap,
                        const STTLocalControl& control, STTLocalStats& stats,
                        char* error, size_t errorCap, STTLocalSession* session,
                        const STTFeaturePrefix* prefix) {
  if (control.cancelled == sttWarmLoadCancelled) {
    // The warm loader: half a second of silence into the parked session.
    assert(warmTest && !mainWorker && session == &gWarmSession && !session->backendState_);
    assert(count == 8000 && !control.context);
    for (size_t i = 0; i < count; ++i) assert(pcm[i] == 0);
    warmLoaderEntered = true;
    while (warmLoadHeld) std::this_thread::sleep_for(std::chrono::microseconds(100));
    if (warmLoadFails || control.cancelled(nullptr)) { snprintf(error, errorCap, "warm load failed"); return false; }
    assert(liveWeightCaches.fetch_add(1) == 0);
    session->backendState_ = new FakeWeights{0, ++weightLoads};
    psramFree -= warmLoadTakes;
    stats.loadMs = 4400;
    snprintf(text, cap, "%s", "");
    return true;
  }
  assert(mainWorker && session && !backendResetBeforeDelete);
  auto& run = *static_cast<StreamRun*>(control.context);
  gRun = &run;
  bool speculative = false;
  uint64_t specStart = 0; uint32_t specSeq = 0;
  if (pcm == run.draftPcm) {
    std::lock_guard<std::mutex> lock(gSTTMux);
    speculative = run.specBusy;
    if (speculative) { assert(!run.draftBusy && count == run.specReadySamples); specStart = run.specReadyStart; specSeq = run.specReadySequence; }
  }
  // capture-features: every cached row equals the batch row of exactly this (possibly
  // trimmed or speculative) input.
  checkPrefix(pcm, count, prefix, speculative ? 2 : pcm == run.draftPcm ? 1 : 0);
  if (prefix) stats.featureCachedFrames = uint32_t(prefix->count);
  if (speculative) {
    // speculation: the snapshot must be exactly the phrase's stream samples from its start.
    for (size_t i = 0; i < count; ++i) assert(pcm[i] == rawSample(specStart + i));
    { std::lock_guard<std::mutex> lock(engineMutex); specHistory.push_back({specStart, count, specSeq}); }
    ++specCalls;
    while (specHold && !control.cancelled(control.context)) std::this_thread::sleep_for(std::chrono::microseconds(100));
    if (control.cancelled(control.context)) { ++specAborts; snprintf(error, errorCap, "Cancelled"); return false; }
    stats.samples = count; stats.inferenceMs = 77;
    snprintf(text, cap, "segment %u", specSeq);
    return true;
  }
  if (pcm == run.draftPcm) {
    // Live-mode draft of the utterance in progress: never a queued segment.
    // These tests pin final-segment behaviour, so drafts decline (production
    // simply publishes no draft on failure) without touching the backend.
    { std::lock_guard<std::mutex> lock(gSTTMux); assert(run.draftBusy && count <= 320000); }
    ++draftCalls;
    if (draftsSucceed) {
      uint32_t sequence = 0;
      { std::lock_guard<std::mutex> lock(gSTTMux); sequence = run.draftSequence; }
      snprintf(text, cap, "draft %u", unsigned(sequence));
      ++draftsPublished;  // the broker publishes it: its phrase has no final yet
      return true;
    }
    snprintf(error, errorCap, "draft declined by harness");
    return false;
  }
  EngineCall call{};
  size_t expectInference = 0;
  {
    std::lock_guard<std::mutex> lock(gSTTMux);
    for (const auto& slot : run.slots) if (slot.pcm == pcm) {
      assert(slot.state == StreamSlotState::Processing);
      call = {slot.samples, count, slot.start, slot.end, slot.sequence};
      expectInference = slot.samples;
      if (slot.endKind != hw1::stt::SegmentEnd::HardLimit && slot.voicedEnd &&
          slot.voicedEnd + 3200 < slot.samples) expectInference = slot.voicedEnd + 3200;
      if (expectInference != slot.samples) ++trimmedCalls;
    }
  }
  assert(call.sequence && call.end - call.start == call.rawSamples);
  assert(count == std::max(expectInference, size_t(320)) && count <= 320000);
  assert(allocationCount() == 5 || allocationCount() == 9);  // capture-features: + ring, workspace, 2 row caches
  if (prefix && count < call.rawSamples) ++cachedTrimmedFinals;  // combined: trim-trimmed input with cached rows
  auto assertPCM = [&] {
    for (size_t i = 0; i < std::min(call.rawSamples, count); ++i) assert(pcm[i] == rawSample(call.start + i));
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
  auto& weights = *static_cast<FakeWeights*>(session->backendState_);
  // A successor must receive fresh ownership: only a model the broker parked
  // (token 0, keep-warm tests only) passes to the next run.
  if (!weights.token) { assert(warmTest); weights.token = run.token; }
  assert(weights.token == run.token);
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
  assert(allocationCount() == 0 && !audioCaptureBusy() && !engineActive &&
         liveWeightCaches == (warmTest && sttWarmResident() ? 1u : 0u));
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
  sourceAvailable = integrityAvailable = g2Available = true; startFails = fallbackOnStart = false;
  stopHeld = stopRequested = invalidRead = readTimeout = false; halOverruns = 0;
  gateNextIntegrity = integrityGateEntered = releaseIntegrityGate = false;
  allowedSamples = deliveredSamples = 0; starts = stops = 0;
  readLimit = 1024; readCounter = 0; irregularReads = false;
  allVoice = true; dc = 0; voiceIntervals.clear();
  modelAvailable = engineOK = true; engineOverlong = ignoreEngineCancel = engineActive = false;
  engineCalls = 0; enginePermits = UINT32_MAX; engineHistory.clear();
  cacheStartFails = false; weightLoads = weightReuses = sessionCloses = 0;
  sttKeepWarm(0, false);  // drops a parked model (keep-warm tests)
  assert(!sttWarmResident() && liveWeightCaches == 0);
  gWarm = STTWarm{};
  warmTest = warmLoadHeld = warmLoadFails = warmLoaderEntered = false; warmMoves = 0; psramFree = SIZE_MAX; warmLoadTakes = 0;
  savePreference = saveFailure = saveHeld = saveEntered = finishHeld = finishEntered = false;
  savedTranscripts.clear();
  specHistory.clear(); specCalls = specAborts = 0; specHold = false; gRun = nullptr;
  publishWakes = 0; wakesWithText = 0; nativeStarts = 0; draftsSucceed = false; draftsPublished = 0;
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
static STTToken beginNative() {
  allowedSamples = deliveredSamples = 0; readCounter = 0;
  STTToken token = 0; char error[96] = {};
  assert(sttBeginContinuousG2Native(owner, 7, &token, error, sizeof(error), nullptr) && token && !error[0]);
  return token;
}
static void countPublishWake() {
  assert(mainWorker && !criticalDepth);  // never inside gSTTMux (production: a semaphore give)
  { std::lock_guard<std::mutex> lock(gSTTMux);  // what the consumer would read now
    if (gSTT.snapshot.pendingTexts || gSTT.draft.sequence) ++wakesWithText; }
  ++publishWakes;
}
static STTToken beginNativeWake() {
  allowedSamples = deliveredSamples = 0; readCounter = 0;
  STTToken token = 0; char error[96] = {};
  assert(sttBeginContinuousG2Native(owner, 7, &token, error, sizeof(error), nullptr, countPublishWake) && token && !error[0]);
  return token;
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
  assert(sttRunActive(token) && allocationCount() == 9);
  assert(snapshot(token).segmentsCaptured == 3);
  enginePermits = UINT32_MAX; complete(token);
  auto s = snapshot(token); assert(s.state == STTState::Done && s.segmentsCompleted == 3 && s.recordedSamples == kCalibrationSamples + 720000);
  assert(starts == 1 && stops == 1 && deletedTasks == 3);  // capture-features: + stt_fe
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
  // Continuous local STT cuts a phrase that reaches 20 s at the quietest
  // 10 ms frame of its last 2 s (hard_cut_search_samples): phrases stay
  // contiguous and complete, never exceed 20 s, and forced ones end within
  // the search window.
  for (size_t i = 0; i < chunks.size(); ++i) {
    const uint64_t span = chunks[i].endSample - chunks[i].startSample;
    assert(chunks[i].startSample == (i ? chunks[i - 1].endSample : kCalibrationSamples) && span <= 320000);
    assert(chunks[i].forcedBoundary == (i < 3));
    if (i < 3) assert(span >= 320000 - 32000 && span % 160 == 0);
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
    assert(chunk.endSample - chunk.startSample == (i ? 36800u : 32000u)); // short-close: short phrases close on their pause
    assert(chunk.startSample >= previousEnd);
    previousEnd = chunk.endSample;
    assert(sttAcknowledgeChunk(owner, paused, chunk.sequence));
  }
  assert(sttRequestFinish(owner, paused)); complete(paused);
  assert(snapshot(paused).state == STTState::Done && snapshot(paused).sessionMs >= 100000);
  for (uint32_t sequence = 1; sequence <= 10; ++sequence) assertTranscribedOnce(sequence);

  reset(); allVoice = false; dc = 16000;
  const auto silent = begin(); started(silent); feed(silent, 120 * 16000);
  assert(engineCalls == 0 && snapshot(silent).segmentsCaptured == 0);
  assert(sttRequestFinish(owner, silent)); complete(silent);
  assert(snapshot(silent).state == STTState::Done && !snapshot(silent).textReady);
}
static void testShortFinalTail() {
  // Voice up to the last 10 ms frame before 20 s, that frame silent, then a
  // 37-sample voiced tail: the quiet-point cut must take the silent frame.
  reset(); allVoice = false; voiceIntervals = {{0, 319840}, {320000, 400000}};
  const auto token = begin(); started(token);
  feed(token, 320000); await([&] { return snapshot(token).segmentsCompleted == 1; }, "first hard cut");
  feed(token, 320037); assert(sttRequestFinish(owner, token)); complete(token);
  assert(snapshot(token).state == STTState::Done && snapshot(token).recordedSamples == kCalibrationSamples + 320037);
  // The cut lands on the silent frame, so the tail is that frame plus the 37
  // extra samples: still short enough to need inference-only padding.
  const auto first = peek(token);
  assert(first.forcedBoundary && first.endSample == kCalibrationSamples + 319840);
  const uint64_t raw = kCalibrationSamples + 320037 - first.endSample;
  assert(raw == 197);
  assert(engineHistory.size() == 2 && engineHistory[1].rawSamples == raw && engineHistory[1].inferenceSamples == 320);
  assert(sttAcknowledgeChunk(owner, token, 1));
  const auto tail = peek(token); assert(tail.startSample == first.endSample && tail.endSample == kCalibrationSamples + 320037 && !tail.forcedBoundary);
}
static void testCancelAndJoin() {
  reset(); enginePermits = 0; ignoreEngineCancel = true; stopHeld = true;
  const auto token = begin(); started(token); feed(token, 320000);
  await([] { return engineCalls == 1; }, "blocked model");
  feed(token, 700000);
  assert(sttCancel(owner, token));
  await([] { return stopRequested.load(); }, "HAL asynchronous stop");
  assert(sttRunActive(token) && allocationCount() == 9 && engineActive);
  assert(liveWeightCaches == 1 && sessionCloses == 0);
  rejectBegin();
  enginePermits = UINT32_MAX;
  await([] { return !engineActive.load(); }, "model cooperative join");
  assert(sttRunActive(token) && allocationCount() == 9); // HAL still owns caller memory/lifecycle.
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
  // Loss first seen at stop (e.g. a concealed G2 frame from the stop tap) is
  // recorded but no longer fails the session or discards the voiced tail.
  assert(s.state == STTState::Done && s.audioOverruns == 3 && !s.error[0]);
  assert(s.recordedSamples == kCalibrationSamples + 16000 && engineCalls == 1 && s.pendingTexts == 1);
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
// Conversate sessions outlive glasses-mic faults: lost frames, a stalled
// stream and a full audio queue are counted, and the session (one transcript)
// continues until the wearer closes Conversate. The onboard mic stays strict
// (testFaults: audio loss and timeouts still fail a normal session).
static void testNativeFaultTolerance() {
  reset(); nativeStarts = 0;
  auto token = beginNative(); started(token);
  assert(nativeStarts == 1 && audioGetSource() == AUDIO_SRC_G2_LEFT);
  feed(token, 16000); halOverruns = 5; feed(token, 32000); halOverruns = 9; feed(token, 48000);
  auto s = snapshot(token);
  assert(s.workerActive && s.state == STTState::Recording && s.audioOverruns == 9 && !s.error[0]);
  readTimeout = true;
  const uint64_t stallStart = clockMs;  // each empty read advances the fake clock by its timeout
  await([&] { return clockMs > stallStart + 9000; }, "two native PCM timeouts elapse");
  assert(snapshot(token).workerActive && !snapshot(token).error[0]);
  readTimeout = false; feed(token, 64000);
  assert(snapshot(token).workerActive && !snapshot(token).error[0]);
  assert(sttRequestFinish(owner, token)); complete(token);
  s = snapshot(token);
  assert(s.state == STTState::Done && !s.error[0] && s.audioOverruns == 9);

  reset(); enginePermits = 0;
  token = beginNative(); started(token); feed(token, 320000);
  await([] { return engineCalls == 1; }, "first blocked native model");
  feed(token, 1280000);  // a fourth 20 s phrase finds three slots occupied: dropped, not fatal
  s = snapshot(token);
  assert(s.workerActive && !s.error[0] && s.segmentsCaptured == 3);
  enginePermits = UINT32_MAX;
  await([&] { return snapshot(token).segmentsCompleted == 3; }, "backlog drained");
  feed(token, 1600000);  // and later phrases continue in order
  await([&] { return snapshot(token).segmentsCompleted == 4; }, "later phrase after a drop");
  acknowledgeAll(token);
  assert(sttRequestFinish(owner, token)); complete(token);
  s = snapshot(token);
  // Finishing seals the voiced tail as one more phrase; nothing was lost after the drop.
  assert(s.state == STTState::Done && !s.error[0] && s.segmentsCaptured >= 4 &&
         s.segmentsCompleted == s.segmentsCaptured);
}
// A caller's per-session source (dictation's preferredSource) replaces the
// micsource setting for that session only, and is strict like the setting.
static void testRequestedSource() {
  const auto beginWith = [](AudioSource requested) {
    allowedSamples = deliveredSamples = 0; readCounter = 0;
    STTToken token = 0; char error[96] = {};
    assert(sttBeginContinuous(owner, &token, error, sizeof(error), nullptr, requested) && token && !error[0]);
    return token;
  };
  reset(); const int kicks = g2Kicks;
  auto token = beginWith(AUDIO_SRC_G2_LEFT); started(token);
  assert(audioGetSource() == AUDIO_SRC_G2_LEFT && snapshot(token).audioSource == AUDIO_SRC_G2_LEFT);
  feed(token, 16000); assert(sttRequestFinish(owner, token)); complete(token); // complete(): setting still "auto"
  assert(snapshot(token).state == STTState::Done && g2Kicks == kicks + 1 && !g2FastHolds && !g2Containers);
  // The default keeps "auto": the HAL resolves PDM first.
  reset(); token = beginWith(AUDIO_SRC_NONE); started(token);
  assert(audioGetSource() == AUDIO_SRC_LOCAL_PDM && snapshot(token).audioSource == AUDIO_SRC_LOCAL_PDM);
  assert(sttRequestFinish(owner, token)); complete(token); assert(snapshot(token).state == STTState::Done);
  // Glasses out of range: the request fails rather than record the board mic.
  reset(); g2Available = false; token = beginWith(AUDIO_SRC_G2_LEFT); complete(token);
  assert(snapshot(token).state == STTState::Failed && strstr(snapshot(token).error, "unavailable") && starts == 0);
  reset(); g2Available = false; token = beginWith(AUDIO_SRC_NONE); started(token);
  assert(audioGetSource() == AUDIO_SRC_LOCAL_PDM);
  assert(sttRequestFinish(owner, token)); complete(token); assert(snapshot(token).state == STTState::Done);
}
static void testTranscriptSaving() {
  reset(); savePreference = true; saveHeld = true;
  // A silent 10 ms frame before each phrase's 20 s limit (the second phrase
  // starts at the first cut, 319840): both quiet-point cuts land there and
  // the carries are silence, so finishing emits no third phrase.
  allVoice = false; voiceIntervals = {{0, 319840}, {320000, 639680}};
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
// One ordinary 2 s phrase, stopped and joined; the model may stay parked.
static STTToken warmRun() {
  const auto token = begin(); started(token);
  feed(token, 2 * 16000);
  assert(sttRequestFinish(owner, token));
  await([&] { return !sttRunActive(token); }, "terminal warm session");
  joinTasks();
  assert(snapshot(token).state == STTState::Done);
  acknowledgeAll(token);
  return token;
}
static void testKeepWarm() {
  // Held warm: the first run loads and parks the model, the next adopts it.
  reset(); warmTest = true;
  sttKeepWarm(60000, false);
  assert(!sttWarmResident() && activeTasks == 0);  // no load: nothing asked for one
  warmRun();
  assert(sttWarmResident() && liveWeightCaches == 1 && weightLoads == 1 && sessionCloses == 1);
  warmRun();
  assert(weightLoads == 1 && engineHistory.size() == 2 && engineHistory[1].weightsReused &&
         engineHistory[1].cacheId == engineHistory[0].cacheId && sessionCloses == 2);
  assert(sttWarmResident() && liveWeightCaches == 1 && warmMoves == 3);  // park, adopt, park
  // A tick inside the hold keeps it; once the hold passes it is freed.
  clockMs += 30000; sttWarmTick();
  assert(sttWarmResident());
  clockMs += 31000; sttWarmTick();
  assert(!sttWarmResident() && liveWeightCaches == 0);
  // With no hold left a run closes its model as before.
  warmRun();
  assert(!sttWarmResident() && liveWeightCaches == 0 && weightLoads == 2);

  // An explicit release frees it at once.
  reset(); warmTest = true;
  sttKeepWarm(60000, false); warmRun();
  assert(sttWarmResident());
  sttKeepWarm(0, false);
  assert(!sttWarmResident() && liveWeightCaches == 0);

  // Low PSRAM frees it even inside the hold, and no warm load follows for a minute.
  reset(); warmTest = true;
  sttKeepWarm(600000, false); warmRun();
  psramFree = (4u << 20) - 1; clockMs += 1000; sttWarmTick();
  assert(!sttWarmResident() && liveWeightCaches == 0);
  psramFree = SIZE_MAX;
  sttKeepWarm(600000, true);
  assert(activeTasks == 0 && !warmLoaderEntered);
  clockMs += 60000;
  sttKeepWarm(600000, true);
  await([] { return sttWarmResident(); }, "warm load after the back-off");
  joinTasks();

  // An idle broker loads one on request; the next run adopts it (one load).
  reset(); warmTest = true;
  sttKeepWarm(60000, true);
  await([] { return sttWarmResident(); }, "warm load");
  joinTasks();
  assert(weightLoads == 1 && liveWeightCaches == 1);
  sttKeepWarm(60000, true);  // already resident: no second load
  assert(activeTasks == 0);
  warmRun();
  assert(weightLoads == 1 && engineHistory[0].weightsReused && sttWarmResident());

  // A run that starts during the warm load waits for it and adopts the
  // result, while its capture keeps running: never a second copy.
  reset(); warmTest = true; warmLoadHeld = true;
  sttKeepWarm(60000, true);
  await([] { return warmLoaderEntered.load(); }, "warm load in progress");
  sttKeepWarm(60000, true);  // a load is in flight: no second loader
  {
    const auto token = begin(); started(token);
    feed(token, 2 * 16000);
    assert(engineCalls == 0);  // waiting for the loader, not loading
    warmLoadHeld = false;
    assert(sttRequestFinish(owner, token));
    await([&] { return !sttRunActive(token); }, "terminal session after the warm load");
    joinTasks();
    acknowledgeAll(token);
  }
  assert(weightLoads == 1 && engineHistory.size() == 1 && engineHistory[0].weightsReused && sttWarmResident());

  // A warm load slower than kWarmAdoptWaitMs is still awaited (21 s of
  // capture pass meanwhile), never raced with a second copy.
  reset(); warmTest = true; warmLoadHeld = true;
  sttKeepWarm(60000, true);
  await([] { return warmLoaderEntered.load(); }, "slow warm load in progress");
  {
    const auto token = begin(); started(token);
    feed(token, 21 * 16000);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(engineCalls == 0 && liveWeightCaches == 0);  // still waiting, not loading
    warmLoadHeld = false;
    assert(sttRequestFinish(owner, token));
    await([&] { return !sttRunActive(token); }, "terminal session after the slow warm load");
    joinTasks();
    acknowledgeAll(token);
  }
  assert(weightLoads == 1 && engineHistory.size() >= 1 && engineHistory[0].weightsReused && sttWarmResident());

  // A failed warm load leaves nothing and backs off.
  reset(); warmTest = true; warmLoadFails = true;
  sttKeepWarm(60000, true);
  await([] { return warmLoaderEntered.load(); }, "failing warm load");
  joinTasks();
  assert(!sttWarmResident() && liveWeightCaches == 0);
  warmLoaderEntered = false;
  sttKeepWarm(60000, true);
  assert(activeTasks == 0 && !warmLoaderEntered);
  // A run still loads its own model then, and parks it while held.
  warmLoadFails = false;
  warmRun();
  assert(weightLoads == 1 && sttWarmResident());

  // A warm load needs room for the model, as the last one took, above the
  // floor that frees it: one that left PSRAM short is not repeated (no
  // load/release loop) until that room is back.
  reset(); warmTest = true;
  psramFree = 24u << 20; warmLoadTakes = 21u << 20;
  sttKeepWarm(600000, true);
  await([] { return sttWarmResident(); }, "warm load into scarce PSRAM");
  joinTasks();
  clockMs += 1000; sttWarmTick();  // 3 MB left: freed at once
  assert(!sttWarmResident() && liveWeightCaches == 0);
  psramFree = 24u << 20; clockMs += 60000;  // its PSRAM is back, the back-off over
  warmLoaderEntered = false;
  sttKeepWarm(600000, true);
  assert(activeTasks == 0 && !warmLoaderEntered);  // 24 MB < 21 + 4 MB
  psramFree = 25u << 20;
  sttKeepWarm(600000, true);
  await([] { return sttWarmResident(); }, "warm load with room again");
  joinTasks();
  clockMs += 1000; sttWarmTick();
  assert(sttWarmResident() && weightLoads == 2);
  // Never beside ESP-SR, which no STT run starts with.
  reset(); warmTest = true; srRunning = true;
  sttKeepWarm(60000, true);
  assert(activeTasks == 0 && !warmLoaderEntered && !sttWarmResident());
  srRunning = false;
  sttKeepWarm(60000, true);
  await([] { return sttWarmResident(); }, "warm load once ESP-SR stopped");
  joinTasks();

  // A hold that ends with nothing parked is dropped by the tick: 2^31 ms
  // later (millis half way round) its stale deadline must not hold again.
  reset(); warmTest = true;
  sttKeepWarm(60000, false);
  clockMs += 61000; sttWarmTick();
  clockMs += 0x80000000ULL;
  warmRun();
  assert(!sttWarmResident() && liveWeightCaches == 0 && weightLoads == 1);
  reset();
}

// trim: pause/session-end phrases are inferred only up to the end of their last
// voiced run longer than 30 ms plus 200 ms; published spans stay whole.
static void testTrailingTrim() {
  struct Case { std::vector<std::pair<uint64_t, uint64_t>> voice; size_t raw, inference; };
  const Case cases[] = {
    {{{0, 80000}}, 89600, 83200},                          // 5 s voice, 600 ms pause
    {{{0, 80000}, {83200, 83680}}, 90080, 83200},          // + 30 ms blip: ignored
    {{{0, 80000}, {83200, 83840}}, 93440, 87040},          // + 40 ms run: counts
  };
  for (const auto& c : cases) {
    reset(); allVoice = false; voiceIntervals = c.voice;
    const auto token = begin(); started(token);
    feed(token, 8 * 16000);
    await([&] { return snapshot(token).segmentsCompleted == 1; }, "trimmed pause segment");
    const auto chunk = peek(token);
    assert(chunk.startSample == kCalibrationSamples && chunk.endSample - chunk.startSample == c.raw);
    assert(std::string(chunk.text) == "segment 1");
    if (engineHistory.empty()) {  // speculation: promoted speculative final on the same input
      assert(!specHistory.empty() && specHistory.back().samples == c.inference && specHistory.back().start == kCalibrationSamples);
    } else {
      assert(engineHistory.size() == 1 && engineHistory[0].rawSamples == c.raw);
      assert(engineHistory[0].inferenceSamples == c.inference);
    }
    assert(sttAcknowledgeChunk(owner, token, 1));
    assert(sttRequestFinish(owner, token)); complete(token);
  }
  // Session end while still speaking: voiced to the end, nothing trimmed.
  reset(); allVoice = false; voiceIntervals = {{0, 48000}};
  const auto token = begin(); started(token);
  feed(token, 48000); assert(sttRequestFinish(owner, token)); complete(token);
  assert(engineHistory.size() == 1 && engineHistory[0].rawSamples == 48000 && engineHistory[0].inferenceSamples == 48000);
  // Session end after a short pause: trimmed like a pause.
  reset(); allVoice = false; voiceIntervals = {{0, 48000}};
  const auto late = begin(); started(late);
  feed(late, 48000 + 8000); assert(sttRequestFinish(owner, late)); complete(late);
  assert(engineHistory.size() == 1 && engineHistory[0].rawSamples == 56000 && engineHistory[0].inferenceSamples == 51200);
  assert(trimmedCalls > 0);
}

static bool specHeld() { std::lock_guard<std::mutex> lock(gSTTMux); return gRun && gRun->specDoneId; }
static void assertTranscribedOnce(uint32_t sequence, size_t samples) {
  std::lock_guard<std::mutex> lock(engineMutex);
  unsigned finals = 0, specs = 0;
  for (const auto& call : engineHistory)
    if (call.sequence == sequence) { ++finals; assert(!samples || call.inferenceSamples == samples); }
  for (const auto& call : specHistory)
    if (call.sequence == sequence) { ++specs; assert(!samples || call.samples == samples); }
  assert(finals == 1 || (!finals && specs));
}
// combined: feed to under one block (1024) before a speculation point (voiced end
// + 200 ms), let the worker finish any draft, then feed past it, so the
// trigger falls in the first block, where the capture task's speculation
// check runs before any new draft snapshot. The capture task copies the
// snapshot only while no draft runs, at its next block (production: 64 ms
// later); a draft still running when a test stops feeding would leave it
// uncopied. Once feeding stops no new draft snapshot is taken.
static void feedPastSpeculation(STTToken token, uint64_t before, uint64_t after) {
  feed(token, before);
  await([] { std::lock_guard<std::mutex> lock(gSTTMux); return gRun && !gRun->draftBusy && !gRun->draftReady; },
        "drafts idle before the speculation point");
  feed(token, after);
}
// speculation: speculation starts at voiced end + 200 ms, is promoted when the pause
// closes the phrase with the same voiced end, and is dropped when speech
// resumes; short phrases closed by the 1 s short close speculate too, phrases
// cut at 20 s never do.
static void testSpeculativeFinal() {
  // Promoted: 5 s voice; feed to 5.35 s, let the run finish, then the pause.
  reset(); allVoice = false; voiceIntervals = {{0, 80000}};
  auto token = begin(); started(token);
  feedPastSpeculation(token, 83000, 85600);
  await([] { return specCalls == 1; }, "speculative run started");
  await([] { return specHeld(); }, "speculative result held");
  assert(engineCalls == 0);
  feed(token, 8 * 16000);
  await([&] { return snapshot(token).segmentsCompleted == 1; }, "promoted final");
  auto chunk = peek(token);
  assert(std::string(chunk.text) == "segment 1" && chunk.endSample - chunk.startSample == 89600);
  assert(engineCalls == 0 && engineHistory.empty() && specHistory.size() == 1);
  assert(specHistory[0].start == kCalibrationSamples && specHistory[0].samples == 83200 && specHistory[0].sequence == 1);
  assert(sttAcknowledgeChunk(owner, token, 1));
  assert(!specHeld());  // consumed by the promotion
  assert(sttRequestFinish(owner, token)); complete(token);

  // Speech resumes 400 ms later: the held result is dropped, and the second
  // pause (voiced end 6.0 s) speculates again and is promoted.
  reset(); allVoice = false; voiceIntervals = {{0, 80000}, {86400, 96000}};
  token = begin(); started(token);
  feedPastSpeculation(token, 83000, 85600);
  await([] { return specHeld(); }, "first speculative result held");
  feedPastSpeculation(token, 99000, 96000 + 5600);
  await([] { return specCalls == 2; }, "second speculative run");
  await([] { return specHeld(); }, "second speculative result held");
  feed(token, 9 * 16000);
  await([&] { return snapshot(token).segmentsCompleted == 1; }, "final after resumed speech");
  chunk = peek(token);
  assert(chunk.endSample - chunk.startSample == 96000 + 9600);
  assert(engineHistory.empty() && specHistory.size() == 2 && specHistory[1].samples == 96000 + 3200);
  assert(sttAcknowledgeChunk(owner, token, 1));
  assert(sttRequestFinish(owner, token)); complete(token);

  // A run still busy when speech resumes is aborted; the final then runs normally.
  reset(); allVoice = false; voiceIntervals = {{0, 80000}, {86400, 96000}}; specHold = true;
  token = begin(); started(token);
  feedPastSpeculation(token, 83000, 85600);
  await([] { return specCalls == 1; }, "held speculative run");
  feed(token, 9 * 16000);
  await([] { return specAborts == 1; }, "speculative run aborted by resumed speech");
  specHold = false;
  await([&] { return snapshot(token).segmentsCompleted == 1; }, "normal final");
  assert(sttRequestFinish(owner, token)); complete(token);
  assert(specAborts >= 1);
  for (const auto& call : engineHistory) assert(call.inferenceSamples <= call.rawSamples);

  // A sub-4 s phrase (1 s voice) that short-close closes on its 1 s pause
  // speculates on voiced end + 200 ms as well and is promoted.
  reset(); allVoice = false; voiceIntervals = {{0, 16000}};
  token = begin(); started(token);
  feedPastSpeculation(token, 19000, 16000 + 3200 + 2400);
  await([] { return specCalls == 1; }, "speculative run on a short phrase");
  await([] { return specHeld(); }, "short phrase's speculative result held");
  feed(token, 6 * 16000);
  await([&] { return snapshot(token).segmentsCompleted == 1; }, "promoted short phrase");
  chunk = peek(token);
  assert(chunk.endSample - chunk.startSample == 32000 && std::string(chunk.text) == "segment 1");
  assert(engineHistory.empty() && specHistory.size() == 1 && specHistory[0].samples == 16000 + 3200);
  assert(sttRequestFinish(owner, token)); complete(token);
  // A 20 s hard limit never speculates.
  reset();
  token = begin(); started(token); feed(token, 21 * 16000);
  await([&] { return snapshot(token).segmentsCompleted == 1; }, "hard-limit phrase");
  assert(specCalls == 0);
  assert(sttRequestFinish(owner, token)); complete(token);
}
// capture-features: a failed pool allocation or feature task leaves capture-features off for the session
// (batch frontend), never fails it; nothing leaks.
static void testFeatureFaults() {
  for (unsigned allocation = 6; allocation <= 9; ++allocation) {
    reset(); failAllocation = allocation; allVoice = false; voiceIntervals = {{0, 80000}};
    const unsigned before = cachedFinals.load();
    const auto token = begin(); started(token); feed(token, 8 * 16000);
    await([&] { return snapshot(token).segmentsCompleted == 1; }, "final without feature cache");
    assert(sttRequestFinish(owner, token)); complete(token);
    assert(snapshot(token).state == STTState::Done && cachedFinals.load() == before);
  }
  reset(); failedTask = "stt_fe"; allVoice = false; voiceIntervals = {{0, 80000}};
  const unsigned before = cachedFinals.load();
  const auto token = begin(); started(token); feed(token, 8 * 16000);
  await([&] { return snapshot(token).segmentsCompleted == 1; }, "final without feature task");
  assert(sttRequestFinish(owner, token)); complete(token);
  assert(snapshot(token).state == STTState::Done && cachedFinals.load() == before);
}

// short-close: below 4 s a pause closes a phrase only once it reaches 1 s (shorter
// pauses still merge); at 4 s or more the 600 ms rule is unchanged.
static void testShortClose() {
  struct Case { std::vector<std::pair<uint64_t, uint64_t>> voice; uint64_t span; };
  const Case cases[] = {
    {{{0, 16000}, {28800, 44800}}, 60800},  // 0.8 s pause merges; 1 s pause closes at 3.8 s
    {{{0, 72000}}, 81600},                  // 4.5 s voice: 600 ms pause as before
    {{{0, 16000}, {28800, 56000}}, 65600},  // reaches 4 s during the second pause: 600 ms
  };
  for (const auto& c : cases) {
    reset(); allVoice = false; voiceIntervals = c.voice;
    const auto token = begin(); started(token);
    feed(token, 10 * 16000);
    await([&] { return snapshot(token).segmentsCompleted == 1; }, "short-close phrase");
    const auto chunk = peek(token);
    assert(chunk.startSample == kCalibrationSamples && chunk.endSample - chunk.startSample == c.span);
    assert(sttAcknowledgeChunk(owner, token, 1));
    assert(sttRequestFinish(owner, token)); complete(token);
    assert(snapshot(token).segmentsCompleted == 1);
  }
}

// All four together: short-close's sub-4 s close (1 s of silence) is
// speculated on voiced end + 200 ms (the trimmed input) and promoted; a phrase
// that reaches 4 s during its pause closes on the 600 ms pause and is
// speculated/promoted as in speculation.
static void testShortCloseSpeculation() {
  // 2 s voice: 600 ms of silence leaves it at 2.6 s (< 4 s), so only short-close's 1 s
  // pause closes it (span 3 s); the speculative run takes voiced end + 200 ms.
  reset(); allVoice = false; voiceIntervals = {{0, 32000}};
  auto token = begin(); started(token);
  feedPastSpeculation(token, 35000, 32000 + 3200 + 2400);
  await([] { return specCalls == 1; }, "speculation on a short-close phrase");
  await([] { return specHeld(); }, "held short-close speculative result");
  feed(token, 6 * 16000);
  await([&] { return snapshot(token).segmentsCompleted == 1; }, "short-close short phrase");
  auto chunk = peek(token);
  assert(chunk.endSample - chunk.startSample == 48000 && std::string(chunk.text) == "segment 1");
  assert(engineHistory.empty() && specHistory.size() == 1 && specHistory[0].samples == 32000 + 3200);
  assert(sttAcknowledgeChunk(owner, token, 1));
  assert(sttRequestFinish(owner, token)); complete(token);

  // Voice 0-1 s and 1.8-3.5 s: 4 s is reached during the second pause, so the
  // 600 ms rule closes it (span 4.1 s) and it is speculated on voiced end +
  // 200 ms, then promoted. The first pause is speculated on too (a 1 s short
  // close could have followed); that result is dropped when speech resumes.
  reset(); allVoice = false; voiceIntervals = {{0, 16000}, {28800, 56000}};
  token = begin(); started(token);
  feedPastSpeculation(token, 19000, 16000 + 3200 + 2400);
  await([] { return specCalls == 1; }, "speculation in the first, short pause");
  await([] { return specHeld(); }, "held first-pause speculative result");
  feedPastSpeculation(token, 59000, 56000 + 3200 + 960);
  await([] { return specCalls == 2; }, "speculation on a phrase reaching 4 s in its pause");
  await([] { return specHeld(); }, "held speculative result");
  feed(token, 10 * 16000);
  await([&] { return snapshot(token).segmentsCompleted == 1; }, "promoted phrase reaching 4 s in its pause");
  chunk = peek(token);
  assert(chunk.endSample - chunk.startSample == 65600 && std::string(chunk.text) == "segment 1");
  assert(engineHistory.empty() && specHistory.size() == 2);
  assert(specHistory[0].samples == 16000 + 3200 && specHistory[1].samples == 56000 + 3200);
  assert(sttAcknowledgeChunk(owner, token, 1));
  assert(sttRequestFinish(owner, token)); complete(token);

  // Several short replies in one session: each closes on its 1 s pause and
  // is transcribed once on its trimmed input (final run or promoted
  // speculative run).
  reset(); allVoice = false;
  for (unsigned i = 0; i < 4; ++i) voiceIntervals.push_back({i * 64000, i * 64000 + 24000});
  token = begin(); started(token); feed(token, 4 * 64000 + 16000);
  await([&] { return snapshot(token).segmentsCompleted == 4; }, "four short replies");
  // Pre-roll: none before the first (calibration ends at its voice), 300 ms later.
  for (const auto& call : engineHistory) assert(call.rawSamples == (call.sequence > 1 ? 44800u : 40000u));
  for (uint32_t seq = 1; seq <= 4; ++seq) assertTranscribedOnce(seq, (seq > 1 ? 4800 : 0) + 24000 + 3200);
  for (uint32_t seq = 1; seq <= 4; ++seq) {
    STTTextChunk out; assert(sttReadChunk(owner, token, &out) && out.sequence == seq);
    assert(sttAcknowledgeChunk(owner, token, seq));
  }
  assert(sttRequestFinish(owner, token)); complete(token);
}


// ---------------------------------------------------------------------------
// The combined latency features on the native Conversate path (G2 left temple):
// 800-sample reads (one 50 ms BLE packet of five LC3 frames), the publish wake publish
// wake, the native pause, a full queue that drops phrases while capture-features caches and
// speculation speculation are live, and the per-session capture-features pool across restarts.
// ---------------------------------------------------------------------------
static uint32_t droppedPhrases() { std::lock_guard<std::mutex> lock(gSTTMux); return gRun ? gRun->droppedSegments : 0; }
static void testNativeMechanisms() {
  // short-close short close (2 s reply, 1 s pause), then a 5 s phrase that speculation
  // speculates on its voiced end + 200 ms and promotes; trim trims both (the
  // reply is transcribed once on its trimmed input: final or promoted run).
  reset(); allVoice = false; readLimit = 800;
  voiceIntervals = {{0, 32000}, {64000, 144000}};
  const unsigned rowsBefore = cachedRows.load(), mismatchesBefore = rowMismatches.load();
  auto token = beginNativeWake(); started(token);
  assert(nativeStarts == 1 && audioGetSource() == AUDIO_SRC_G2_LEFT);
  feed(token, 60000);
  await([&] { return snapshot(token).segmentsCompleted == 1; }, "native short-close short phrase");
  auto chunk = peek(token);
  assert(chunk.endSample - chunk.startSample == 48000 && std::string(chunk.text) == "segment 1");
  assertTranscribedOnce(1, 32000 + 3200);
  await([] { return publishWakes == 1; }, "publish wake wake for the first final");
  assert(sttAcknowledgeChunk(owner, token, 1));
  const size_t firstFinals = engineHistory.size(), firstSpecs = specHistory.size();
  feedPastSpeculation(token, 146800, 148000);
  await([&] { return size_t(specCalls) == firstSpecs + 1; }, "native speculative run");
  await([] { return specHeld(); }, "native speculative result held");
  feed(token, 160000);
  await([&] { return snapshot(token).segmentsCompleted == 2; }, "native promoted final");
  chunk = peek(token);
  assert(chunk.sequence == 2 && std::string(chunk.text) == "segment 2");
  assert(chunk.startSample == kCalibrationSamples + 64000 - 4800 && chunk.endSample == kCalibrationSamples + 144000 + 9600);
  assert(engineHistory.size() == firstFinals && specHistory.size() == firstSpecs + 1);
  assert(specHistory.back().start == kCalibrationSamples + 64000 - 4800 && specHistory.back().samples == 80000 + 4800 + 3200);
  await([] { return publishWakes == 2; }, "publish wake wake for the promoted final");
  assert(sttAcknowledgeChunk(owner, token, 2));
  assert(sttRequestFinish(owner, token)); complete(token);
  const auto s = snapshot(token);
  assert(s.state == STTState::Done && !s.error[0] && s.segmentsCompleted == 2 && publishWakes == 2 && wakesWithText == 2);
  assert(cachedRows.load() > rowsBefore && rowMismatches.load() == mismatchesBefore);  // capture-features ran on the native stream

  // A non-native session never calls a wake hook.
  reset(); allVoice = false; voiceIntervals = {{0, 32000}};
  token = begin(); started(token); feed(token, 60000);
  await([&] { return snapshot(token).segmentsCompleted == 1; }, "PDM phrase without a hook");
  assert(publishWakes == 0);
  assert(sttRequestFinish(owner, token)); complete(token);
}
static void testNativePauseCloses() {
  // The wearer pauses mid-word (Conversate PAUSE): no PCM arrives, the broker
  // feeds silence so the open phrase can close on its own rule. A sub-4 s
  // phrase needs short-close's full 1 s of silence after its last voiced 10 ms frame,
  // which a 1 s feed starting mid-frame never reaches.
  reset(); allVoice = false; readLimit = 800;
  voiceIntervals = {{0, 32080}};  // voice stops mid-frame, exactly at the pause
  const auto token = beginNativeWake(); started(token);
  feed(token, 32080);
  assert(sttSetPaused(owner, token, true));
  await([&] { return snapshot(token).segmentsCaptured == 1; }, "paused short phrase closes on the fed silence");
  await([&] { return snapshot(token).segmentsCompleted == 1; }, "paused phrase transcribed");
  const auto chunk = peek(token);
  assert(chunk.startSample == kCalibrationSamples && std::string(chunk.text) == "segment 1");
  assertTranscribedOnce(1, 32160 + 3200);  // trim: voiced frame end + 200 ms
  assert(publishWakes == 1 && snapshot(token).workerActive && !snapshot(token).error[0]);
  assert(sttAcknowledgeChunk(owner, token, 1));
  assert(sttSetPaused(owner, token, false));
  assert(sttRequestFinish(owner, token)); complete(token);
  assert(snapshot(token).state == STTState::Done && snapshot(token).segmentsCompleted == 1);
}
static void testNativeQueueFullWithCaches() {
  // Finals blocked: phrases 2 and 3 fill the queue (2 capture-features caches are then all
  // attached, so phrase 3 has none), 4 and 5 are dropped with their caches
  // and speculation; nothing fails, leaks or mismatches, and later phrases
  // continue in order with capture-features again.
  // Phrase 1's speculative run is held, so its final cannot be promoted;
  // phrase 2's close aborts it and phrase 1's final then blocks the engine.
  reset(); enginePermits = 0; specHold = true; allVoice = false; readLimit = 800;
  for (unsigned i = 0; i < 6; ++i) voiceIntervals.push_back({i * 96000, i * 96000 + 80000});
  const unsigned mismatchesBefore = rowMismatches.load();
  const auto token = beginNativeWake(); started(token);
  feedPastSpeculation(token, 82800, 84000);
  await([] { return specCalls == 1; }, "phrase 1 speculative run held");
  feed(token, 5 * 96000);
  await([] { return engineCalls == 1 && specAborts == 1; }, "speculation aborted, first native final blocked");
  auto s = snapshot(token);
  assert(s.workerActive && !s.error[0] && s.segmentsCaptured == 3 && droppedPhrases() == 2);
  specHold = false; enginePermits = UINT32_MAX;
  await([&] { return snapshot(token).segmentsCompleted == 3; }, "native backlog drained");
  feed(token, 6 * 96000);
  await([&] { return snapshot(token).segmentsCompleted == 4; }, "native phrase after the drops");
  std::vector<STTTextChunk> chunks; acknowledgeAll(token, &chunks);
  assert(chunks.size() == 4);
  for (size_t i = 0; i < chunks.size(); ++i) assert(chunks[i].sequence == i + 1);
  assert(chunks[3].startSample == kCalibrationSamples + 5 * 96000 - 4800);
  assert(publishWakes == 4);
  assert(sttRequestFinish(owner, token)); complete(token);
  s = snapshot(token);
  assert(s.state == STTState::Done && !s.error[0] && s.segmentsCompleted == 4);
  assert(rowMismatches.load() == mismatchesBefore);
}
static void testNativeRestarts() {
  // Conversate restarts transcription up to 3 times a minute: each session
  // allocates its own capture-features pool after the warm-up and frees it (with weights,
  // queue and draft buffers) before the worker exits, so a restart starts
  // from the same PSRAM and other features get it back in between.
  const unsigned poolsBefore = fePoolAllocs.load(), queriesBefore = callBudgetQueries.load();
  for (unsigned n = 0; n < 3; ++n) {
    reset(); allVoice = false; readLimit = 800; voiceIntervals = {{0, 80000}};
    const unsigned rowsBefore = cachedRows.load();
    const auto token = beginNativeWake(); started(token);
    feed(token, 8 * 16000);
    await([&] { return snapshot(token).segmentsCompleted == 1; }, "restarted native session final");
    assert(allocationCount() == 9);  // working + 3 slots + draft + capture-features ring, workspace, 2 row caches
    acknowledgeAll(token);
    assert(sttCancel(owner, token)); complete(token);  // an unexpected end: everything is freed (joinTasks)
    assert(allocationCount() == 0 && cachedRows.load() > rowsBefore);
  }
  assert(fePoolAllocs.load() == poolsBefore + 3 && callBudgetQueries.load() == queriesBefore + 3);
}
static void testNativeDraftWakes() {
  // publish wake also wakes the pump on every draft refresh; the draft is readable then.
  reset(); allVoice = false; readLimit = 800; draftsSucceed = true;
  voiceIntervals = {{0, 96000}};
  const auto token = beginNativeWake(); started(token);
  feed(token, 64000);
  await([] { return draftsPublished >= 2; }, "two native drafts");
  STTDraft draft;
  assert(sttReadDraft(owner, token, &draft) && draft.sequence == 1 && draft.version >= 2);
  assert(!strncmp(draft.text, "draft 1", 7));
  await([] { return publishWakes == draftsPublished; }, "a wake per published draft");
  feed(token, 9 * 16000);
  await([&] { return snapshot(token).segmentsCompleted == 1; }, "final after drafts");
  await([] { return publishWakes == draftsPublished + 1; }, "a wake for the final");
  assert(wakesWithText == publishWakes);
  assert(sttReadDraft(owner, token, &draft) && draft.sequence == 0);  // the final cleared it
  acknowledgeAll(token);
  assert(sttRequestFinish(owner, token)); complete(token);
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
  testNativeFaultTolerance(); puts("PASS Conversate sessions survive glasses audio loss, stalls and a full queue");
  testWideCountersAndRetention(); puts("PASS 64-bit time/positions and result expiry");
  testTranscriptSaving(); puts("PASS transcript admission latch, ACK-independent saving, save failure, cancel and finalization");
  testRequestedSource(); puts("PASS per-session requested source overrides the setting and fails strictly");
  testKeepWarm(); puts("PASS keep-warm: park/adopt across runs, hold expiry and millis wrap, low-PSRAM release, warm load (awaited past 20 s) and its back-off");
  testTrailingTrim(); puts("PASS trailing-silence trim: ambient gate, 30 ms blips ignored, spans whole, session end");
  testSpeculativeFinal(); puts("PASS speculative final: promoted on the same input, dropped on resumed speech, aborted when busy, short closes too, never at 20 s");
  testFeatureFaults(); puts("PASS capture-feature pool/task failures leave the session on the batch frontend");
  testShortClose(); puts("PASS short close: short phrases close after 1 s of silence, 600 ms from 4 s");
  testShortCloseSpeculation(); puts("PASS short closes speculate on their trimmed input and promote; 4 s-in-pause phrases speculate and promote");
  testNativeMechanisms(); puts("PASS native path: short close, speculative promote, trim, cached feature rows on 800-sample reads; a publish wake per final, none without a hook");
  testNativePauseCloses(); puts("PASS native pause: a mid-word paused short phrase closes on the fed silence");
  testNativeQueueFullWithCaches(); puts("PASS native full queue drops phrases with live feature caches/speculative requests, nothing leaks or mismatches");
  testNativeDraftWakes(); puts("PASS native publish wake: a wake per published draft and final, each readable when woken");
  testNativeRestarts(); puts("PASS native restarts: one feature pool per session, all PSRAM freed at session end");
  reset(); puts("Continuous production STT broker multithread tests passed, including explicit backend-session cleanup");
  printf("cached feature rows: finals %u drafts %u rows %u mismatches %u; speculative %u trimmed finals %u\n", cachedFinals.load(), cachedDrafts.load(), cachedRows.load(), rowMismatches.load(), cachedSpecs.load(), cachedTrimmedFinals.load());
  assert(rowMismatches == 0 && cachedFinals > 0);
  assert(cachedSpecs > 0 && cachedTrimmedFinals > 0);  // the features x speculation and features x trim paths ran
}
