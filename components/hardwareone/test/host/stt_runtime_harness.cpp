#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#define ENABLE_LOCAL_STT 1
#define ENABLE_MICROPHONE 1
#define ENABLE_ESP_SR 1
using String = std::string;
using TransportSessionEpoch = uint32_t;
enum CommandSource { SOURCE_WEB, SOURCE_SERIAL, SOURCE_INTERNAL, SOURCE_ESPNOW,
  SOURCE_LOCAL_DISPLAY, SOURCE_BLUETOOTH, SOURCE_MQTT, SOURCE_VOICE, SOURCE_G2_GLASSES, SOURCE_UART };
// INSERT_INTERFACE
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(x) (x)
constexpr int pdPASS = 1;
static uint32_t nowMs = 1;
uint32_t millis() { return nowMs; }
void vTaskDelay(uint32_t ms) { nowMs += ms; }
static int taskDeleted = 0;
void vTaskDelete(void*) { ++taskDeleted; }
uint32_t uxTaskGetStackHighWaterMark(void*) { return 8192; }
void taskStackRecord(const char*, uint32_t) {}
uint32_t esp_random() { return 0xabcdef01; }
static bool taskFails = false;
static void (*pendingTask)(void*) = nullptr;
int xTaskCreatePinnedToCore(void (*fn)(void*), const char*, uint32_t bytes,
                            void*, int priority, void*, int core) {
  assert(bytes == 12288 && priority == 1 && core == 1);
  if (taskFails) return 0;
  assert(!pendingTask);
  pendingTask = fn;
  return pdPASS;
}
static std::set<std::pair<int, uint32_t>> sessions;
bool transportSessionEpochIsLive(CommandSource source, uint32_t epoch) {
  return sessions.count({source, epoch});
}
static bool srRunning = false;
bool isESPSRRunning() { return srRunning; }
bool gMicRunning = false;
static bool recordingBusy = false;
bool micRecordingBusy() { return recordingBusy; }
struct { String micSource = "auto"; int microphoneGain = 70; int microphoneSampleRate = 48000; } gSettings;
enum AudioSource { AUDIO_SRC_NONE, AUDIO_SRC_LOCAL_PDM, AUDIO_SRC_G2_LEFT };
static AudioSource audioSource = AUDIO_SRC_LOCAL_PDM;
static std::string halOwner;
static bool sourceAvailable = true, startFails = false, active = false, fallbackOnStart = false;
static size_t nextSample = 0;
static size_t readLimit = 137; // Deliberately short reads.
static int starts = 0, stops = 0;
static uint32_t captureRate = 0;
static std::function<void()> readHook;
AudioSource audioGetSource() { return audioSource; }
bool audioCaptureBusy() { return !halOwner.empty(); }
bool audioCaptureActive() { return active; }
bool audioCaptureOwnedBy(const char* owner) { return halOwner == owner; }
bool audioSourceAvailable(AudioSource s) { return s != AUDIO_SRC_NONE && sourceAvailable; }
bool audioAnySourceAvailable() { return sourceAvailable; }
bool audioSetSource(AudioSource s) {
  if (audioCaptureBusy() || (s != AUDIO_SRC_NONE && !sourceAvailable)) return false;
  audioSource = s; return true;
}
bool audioCaptureStart(const char* owner, uint32_t rate) {
  if (startFails || audioCaptureBusy()) return false;
  halOwner = owner; active = true; captureRate = rate; ++starts; nextSample = 0;
  if (audioSource == AUDIO_SRC_NONE || fallbackOnStart) audioSource = AUDIO_SRC_LOCAL_PDM;
  return true;
}
void audioCaptureStop(const char* owner) {
  if (halOwner == owner) { halOwner.clear(); active = false; ++stops; }
}
size_t audioTrimBufferedPcm(const char*, size_t) { return 0; }
size_t audioReadPcm(int16_t* pcm, size_t requested, uint32_t timeout) {
  assert(halOwner == "stt" && timeout == 50);
  const size_t n = std::min(requested, readLimit);
  for (size_t i = 0; i < n; ++i) pcm[i] = static_cast<int16_t>((nextSample++ % 101) - 50);
  nowMs += n ? (n + 15) / 16 : timeout;
  if (readHook) readHook();
  return n;
}
enum class AllocPref { RequirePSRAM };
static bool allocationFails = false;
static std::set<void*> allocations;
void* ps_alloc(size_t n, AllocPref, const char*) {
  if (allocationFails) return nullptr;
  void* p = std::malloc(n);
  if (p) allocations.insert(p);
  return p;
}
void trackedFree(void* p) { assert(allocations.erase(p) == 1); std::free(p); }
static bool modelAvailable = true, engineOK = true, engineOverlong = false;
static size_t engineSamples = 0;
static unsigned engineCalls = 0;
static std::function<void(const STTLocalControl&)> engineHook;
bool sttLocalAvailable(char* error, size_t cap) {
  if (!modelAvailable) snprintf(error, cap, "Model missing");
  return modelAvailable;
}
bool sttLocalTranscribe(const int16_t* pcm, size_t count, char* text, size_t cap,
                        const STTLocalControl& control, STTLocalStats& stats,
                        char* error, size_t errorCap) {
  assert(halOwner == "stt" && allocations.size() == 1);
  ++engineCalls; engineSamples = count;
  for (size_t i = 0; i < count; ++i) assert(pcm[i] == static_cast<int16_t>((i % 101) - 50));
  control.progress(control.context, STTLocalPhase::Inference);
  if (engineHook) engineHook(control);
  stats.samples = count; stats.inferenceMs = 123;
  if (!engineOK) snprintf(error, errorCap, "Inference failed");
  if (engineOverlong) memset(text, 'x', cap);
  else snprintf(text, cap, "quote \" secret \\ transcript");
  return engineOK;
}
#define free trackedFree
// INSERT_BROKER
#undef free

static STTOwner owner{SOURCE_SERIAL, 5};
static STTOwner other{SOURCE_WEB, 5}; // Same number, different transport.
static void reset() {
  assert(allocations.empty() && !pendingTask);
  gSTT = STTRun{};
  nowMs = 1; nextSample = 0; readLimit = 137;
  sessions = {{SOURCE_SERIAL, 5}, {SOURCE_WEB, 5}, {SOURCE_SERIAL, 6}};
  audioSource = AUDIO_SRC_LOCAL_PDM; halOwner.clear(); active = false;
  srRunning = recordingBusy = gMicRunning = false;
  sourceAvailable = modelAvailable = engineOK = true;
  taskFails = allocationFails = startFails = engineOverlong = fallbackOnStart = false;
  starts = stops = engineCalls = taskDeleted = 0;
  engineHook = nullptr; readHook = nullptr;
  gSettings.micSource = "auto";
}
static STTToken begin() {
  STTToken token = 0; char error[96];
  assert(sttBegin(owner, 1000, &token, error, sizeof(error)) && token);
  return token;
}
static void run() {
  assert(pendingTask);
  auto fn = pendingTask; pendingTask = nullptr; fn(nullptr);
  assert(allocations.empty());
  assert(!audioCaptureOwnedBy("stt"));
}
static STTSnapshot snapshot(STTToken token) {
  STTSnapshot out;
  assert(sttSnapshot(owner, token, &out)); return out;
}
static void rejectsBegin() {
  STTToken token = 7; char error[96];
  assert(!sttBegin(owner, 1000, &token, error, sizeof(error)) && error[0]);
}
int main() {
  reset();
  STTToken token = begin();
  assert(sttRunActive(token) && !sttRunActive(token + 1));
  assert(!sttCancel(other, token));
  assert(!sttCancel({SOURCE_SERIAL, 6}, token));
  assert(!sttRequestFinish(owner, token + 1));
  STTSnapshot denied;
  assert(!sttSnapshot(other, token, &denied));
  assert(!sttSnapshot({SOURCE_SERIAL, 6}, 0, &denied));
  run();
  assert(snapshot(token).state == STTState::Done);
  assert(!sttRunActive(token));
  assert(engineSamples == 16000 && starts == 1 && stops == 1 && captureRate == 16000);
  assert(gSettings.microphoneSampleRate == 48000 && gSettings.microphoneGain == 70 && gSettings.micSource == "auto");
  assert(audioSource == AUDIO_SRC_LOCAL_PDM);
  char text[513];
  assert(sttResult(owner, token, text, sizeof(text)) && strstr(text, "secret"));
  assert(!sttResult(other, token, text, sizeof(text)) && !text[0]);
  assert(!sttResult(owner, token, text, 3)); // No silent truncation.
  assert(sttCancel(owner, token));
  assert(!sttResult(owner, token, text, sizeof(text)));
  auto second = begin(); assert(second != token && !sttCancel(owner, token)); run();

  reset(); token = begin(); assert(sttCancel(owner, token)); run();
  assert(starts == 0 && engineCalls == 0 && snapshot(token).state == STTState::Cancelled);

  reset(); token = begin();
  readHook = [&] { sttCancel(owner, token); rejectsBegin(); };
  run(); assert(engineCalls == 0 && snapshot(token).state == STTState::Cancelled);

  reset(); token = begin();
  engineHook = [&](const STTLocalControl& ctl) {
    assert(!ctl.cancelled(ctl.context));
    assert(sttCancel(owner, token)); assert(ctl.cancelled(ctl.context));
    rejectsBegin(); assert(allocations.size() == 1 && halOwner == "stt");
  };
  run(); assert(snapshot(token).state == STTState::Cancelled);
  assert(!sttResult(owner, token, text, sizeof(text)));

  reset(); token = begin();
  readHook = [&] { sessions.erase({SOURCE_SERIAL, 5}); };
  run(); assert(gSTT.snapshot.state == STTState::Cancelled && engineCalls == 0);
  assert(!sttRunActive(token));
  assert(!sttResult(owner, token, text, sizeof(text)));

  reset(); token = begin(); run(); sessions.erase({SOURCE_SERIAL, 5});
  assert(!sttResult(owner, token, text, sizeof(text)) && !text[0]);
  assert(!sttSnapshot({SOURCE_SERIAL, 6}, token, &denied));

  reset(); token = begin(); readHook = [&] { if (nextSample >= 1700) sttRequestFinish(owner, token); };
  run(); assert(engineSamples >= 1700 && engineSamples < 2000 && snapshot(token).state == STTState::Done);

  reset(); token = begin(); sttRequestFinish(owner, token); run();
  assert(engineCalls == 0 && snapshot(token).state == STTState::Failed);

  reset(); token = begin(); readLimit = 0; run();
  assert(engineCalls == 0 && snapshot(token).state == STTState::Failed);
  assert(strstr(snapshot(token).error, "timed out"));

  reset(); token = begin(); readHook = [&] { sourceAvailable = false; }; run();
  assert(engineCalls == 0 && snapshot(token).state == STTState::Failed);

  reset(); allocationFails = true; token = begin(); run();
  assert(starts == 0 && snapshot(token).state == STTState::Failed);
  reset(); startFails = true; token = begin(); run();
  assert(starts == 0 && snapshot(token).state == STTState::Failed);
  reset(); engineOK = false; token = begin(); run();
  assert(snapshot(token).state == STTState::Failed && stops == 1);
  reset(); engineOverlong = true; token = begin(); run();
  assert(snapshot(token).state == STTState::Failed);

  reset(); token = begin(); gSettings.micSource = "g2"; run();
  assert(snapshot(token).audioSource == AUDIO_SRC_LOCAL_PDM); // Latched at admission.
  assert(gSettings.micSource == "g2"); // Later user preference survives.

  reset(); gSettings.micSource = "g2"; fallbackOnStart = true; token = begin(); run();
  assert(snapshot(token).state == STTState::Failed && starts == 1 && stops == 1);
  assert(nextSample == 0 && engineCalls == 0); // Never drain the wrong source.
  assert(strstr(snapshot(token).error, "changed during startup"));

  reset(); modelAvailable = false; rejectsBegin();
  reset(); srRunning = true; rejectsBegin();
  reset(); gMicRunning = true; rejectsBegin();
  reset(); recordingBusy = true; rejectsBegin();
  reset(); halOwner = "native"; rejectsBegin(); assert(stops == 0);
  reset(); taskFails = true; rejectsBegin(); assert(!gSTT.snapshot.workerActive);
  reset(); token = begin(); halOwner = "mic"; active = true; run();
  assert(halOwner == "mic" && stops == 0 && snapshot(token).state == STTState::Failed);
  halOwner.clear();

  reset(); token = begin(); run(); nowMs += kResultRetentionMs;
  assert(!sttResult(owner, token, text, sizeof(text)));
  STTToken third; char error[96];
  assert(sttBegin(other, 1000, &third, error, sizeof(error)));
  assert(!sttResult(owner, token, text, sizeof(text))); run();
  puts("STT production broker: ownership, cancellation, short reads, limits, failures and cleanup passed");
}
