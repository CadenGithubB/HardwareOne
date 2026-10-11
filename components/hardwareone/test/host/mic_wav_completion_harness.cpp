// Production header/finalization code is inserted by test_mic_wav_completion.py.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

static size_t checks = 0, scenarios = 0;
static void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

// INSERT_TYPES

static_assert(sizeof(WavHeader) == 44, "PCM WAV header layout");
static constexpr uint32_t AUDIO_HAL_SAMPLE_RATE = 16000;
static constexpr int SYSEVT_MIC_SAVED = 17;
static unsigned micChannels;
static bool gRecCaptureFailed, gRecVadAutoStopped, gRecWasG2Source;
static uint32_t gRecWrittenSamples, gRecSampleRate, gRecFirstPcmMs,
    gRecVadEndMs, gRecPrerollKeptSamples;
static unsigned long recordingSamples, recordingStartTime;
static char gRecFailureReason[96], currentRecordingPath[128];
static void* recordingTaskHandle;
static std::vector<std::string> events;
static int lockDepth;
static bool removeOk, degraded, liveShadowStarted, discardDuringWrite, pushOk;
static MicRecordingDispositionSnapshot dispositionFixture;
static struct { MicRecordingOwner owner; } shadowCapture;
static struct {
  bool published, failed, discarded, degraded;
  MicRecordingOwner owner;
  unsigned long samples;
  uint32_t rate;
  std::string path, reason;
} result;

static void observe(const std::string& event) { events.push_back(event); }
class FsLockGuard {
public:
  explicit FsLockGuard(const char* tag) {
    check(lockDepth == 0, "filesystem scope starts unlocked");
    ++lockDepth;
    observe(std::string("lock:") + tag);
  }
  ~FsLockGuard() { --lockDepth; observe("unlock"); }
};

class File {
public:
  bool open = true, seekOk = true, reportedWriteError = false;
  size_t writeLimit = std::numeric_limits<size_t>::max(), position = 0;
  unsigned writes = 0, flushes = 0, closes = 0, errorReads = 0, sizeReads = 0;
  std::vector<uint8_t> bytes;
  explicit operator bool() const { return open; }
  bool seek(size_t offset) {
    check(lockDepth == 1 && open, "seek inside locked open file");
    observe("seek:" + std::to_string(offset));
    if (!seekOk) return false;
    position = offset;
    return true;
  }
  size_t write(const uint8_t* input, size_t size) {
    check(lockDepth == 1 && open, "header write inside locked open file");
    ++writes;
    const size_t count = std::min(size, writeLimit);
    if (bytes.size() < position + count) bytes.resize(position + count);
    std::copy(input, input + count, bytes.begin() + position);
    position += count;
    if (discardDuringWrite) dispositionFixture.discard = true;
    observe("write:" + std::to_string(count));
    return count;
  }
  void flush() {
    check(lockDepth == 1 && open, "flush before close inside filesystem lock");
    ++flushes;
    // The explicit flush is new. Assert its ordering separately from the
    // unchanged lifecycle/publication transcript used for baseline comparison.
  }
  int getWriteError() {
    check(lockDepth == 1 && open && flushes == 1, "check error after flush before close");
    ++errorReads;
    // This is a reported Print error, not a model of backend flush failures:
    // Arduino File flush discards fflush/fsync return values.
    return reportedWriteError;
  }
  size_t size() {
    check(lockDepth == 1 && open && flushes == 1, "check size after flush before close");
    ++sizeReads;
    return bytes.size();
  }
  void close() {
    check(lockDepth == 1 && open && flushes == HAS_FINAL_FLUSH,
          "close inside lock after expected explicit flush");
    ++closes;
    open = false;
    observe("close");
  }
};
static File recordingFile;

static void micRecordingRequestStop() { observe("stop-request"); }
static MicRecordingDispositionSnapshot micRecordingSealDisposition() {
  check(!recordingFile.open && lockDepth == 0, "seal disposition after unlocked close");
  observe("seal:" + std::to_string(dispositionFixture.owner));
  return dispositionFixture;
}
static bool micRecordingRemoveExactPath(const char* path, const char* tag) {
  check(!recordingFile.open && lockDepth == 0, "remove boundary follows unlocked close");
  check(std::string(path) == currentRecordingPath, "cleanup exact recording path");
  observe(std::string("remove:") + tag + ":" + path);
  return removeOk;
}
static void liveAudioRecorderFinish(MicRecordingOwner owner, LiveAudioRecorderOutcome outcome) {
  check(lockDepth == 0 && !recordingFile.open, "live outcome after close outside filesystem lock");
  check(owner == shadowCapture.owner, "live completion preserves admitted owner");
  observe("live:" + std::to_string(owner) + ":" + std::to_string(static_cast<int>(outcome)));
}
static void micRecordingOwnerHex(MicRecordingOwner owner, char* out) {
  std::snprintf(out, 17, "%016llx", static_cast<unsigned long long>(owner));
}
static void systemEventPost(int type, const char* filename) {
  check(type == SYSEVT_MIC_SAVED && !recordingFile.open && lockDepth == 0,
        "saved event follows close outside filesystem lock");
  observe(std::string("saved:") + filename);
}
static bool uartLinkPushEventForSession(uint32_t epoch, const char* text) {
  check(!recordingFile.open && lockDepth == 0, "host callback after close outside filesystem lock");
  check(epoch == dispositionFixture.admissionSessionEpoch, "host callback uses admission epoch");
  observe("host:" + std::to_string(epoch) + ":" + text);
  return pushOk;
}
static unsigned long g2EvenAiLastWakeMs() { return 100; }
static unsigned long millis() { return 900; }
static bool g2MicCaptureDegraded() { return degraded; }
static void g2MicLinkFastRelease() { observe("g2-fast-release"); }
static void g2MicReleaseCaptureContainer() { observe("g2-container-release"); }
static void micRecordingPublishIdle(bool publish, bool failed, bool discarded,
    MicRecordingOwner owner, unsigned long samples, uint32_t rate,
    const char* path, const char* reason, bool isDegraded) {
  check(publish && !recordingFile.open && !recordingTaskHandle && lockDepth == 0,
        "IDLE publication follows closed file and cleared task handle");
  result = {true, failed, discarded, isDegraded, owner, samples, rate, path, reason};
  observe("idle:" + std::to_string(owner) + ":" + std::to_string(failed) + ":" +
          std::to_string(discarded) + ":" + path + ":" + reason);
}
static void sensorStatusBumpWith(const char* tag) {
  check(result.published, "status bump after IDLE publication");
  observe(std::string("status:") + tag);
}
static void vTaskDelete(void* task) {
  check(!task && result.published, "task deletion after completion publication");
  observe("delete-task");
}
#define DEBUG_MIC_LIFECYCLEF(...) ((void)0)
#define INFO_MIC_LIFECYCLEF(...) ((void)0)

// INSERT_PRODUCTION

static bool hasPrefix(const std::string& prefix) {
  return std::any_of(events.begin(), events.end(), [&](const std::string& item) {
    return item.compare(0, prefix.size(), prefix) == 0;
  });
}
static size_t indexOf(const std::string& prefix) {
  for (size_t i = 0; i < events.size(); ++i)
    if (events[i].compare(0, prefix.size(), prefix) == 0) return i;
  return events.size();
}
static void reset() {
  events.clear();
  lockDepth = 0;
  result = {};
  gRecCaptureFailed = gRecVadAutoStopped = gRecWasG2Source = false;
  discardDuringWrite = degraded = false;
  removeOk = pushOk = liveShadowStarted = true;
  gRecWrittenSamples = 9;
  recordingSamples = 25; // Captured count differs from the retained/trimmed PCM.
  micChannels = 1;
  gRecSampleRate = 24000;
  gRecFirstPcmMs = 300;
  gRecVadEndMs = 700;
  gRecPrerollKeptSamples = 24;
  recordingStartTime = 200;
  std::strcpy(currentRecordingPath, "/captures/audio/test.wav");
  gRecFailureReason[0] = '\0';
  recordingTaskHandle = reinterpret_cast<void*>(uintptr_t{1});
  dispositionFixture = {0x12345678abcdef12ULL, false, 73};
  shadowCapture.owner = dispositionFixture.owner;
  recordingFile = File{};
  recordingFile.bytes.resize(sizeof(WavHeader) + gRecWrittenSamples * sizeof(int16_t), 0xa5);
}
static void verifyHeader() {
  WavHeader header;
  std::memcpy(&header, recordingFile.bytes.data(), sizeof(header));
  check(std::memcmp(header.riff, "RIFF", 4) == 0 &&
        std::memcmp(header.wave, "WAVE", 4) == 0 &&
        std::memcmp(header.fmt, "fmt ", 4) == 0 &&
        std::memcmp(header.data, "data", 4) == 0, "WAV signatures");
  check(header.dataSize == gRecWrittenSamples * sizeof(int16_t), "header uses retained PCM count");
  check(header.fileSize == sizeof(WavHeader) + header.dataSize - 8, "RIFF size");
  check(header.bitsPerSample == 16 && header.audioFormat == 1 && header.fmtSize == 16,
        "PCM header type");
  check(header.numChannels == micChannels && header.blockAlign == micChannels * 2,
        "WAV channels and sample alignment");
  check(header.sampleRate == (gRecSampleRate ? gRecSampleRate : AUDIO_HAL_SAMPLE_RATE) &&
        header.byteRate == header.sampleRate * header.blockAlign, "WAV rate and fallback");
  check(std::all_of(recordingFile.bytes.begin() + sizeof(WavHeader), recordingFile.bytes.end(),
                   [](uint8_t byte) { return byte == 0xa5; }), "header patch preserves PCM bytes");
}
static void verifyOutcome(bool failed, bool discarded, const char* reason, bool removed) {
  check(result.published && result.failed == failed && result.discarded == discarded,
        "completion result matches outcome");
  check(result.owner == dispositionFixture.owner && result.samples == recordingSamples &&
        result.rate == gRecSampleRate, "completion preserves owner and capture metadata");
  check(result.degraded == (gRecWasG2Source && degraded), "completion source degradation");
  check(result.reason == reason, "first failure reason preserved");
  check(result.path == (removed ? "" : currentRecordingPath), "published path retention");
  const bool saved = !failed && !dispositionFixture.discard;
  check(hasPrefix("saved:") == saved, "only successful retained recordings emit saved event");
  check(hasPrefix("host:") == (saved && gRecVadAutoStopped), "only saved VAD captures notify host");
  check(hasPrefix("remove:") == (failed || dispositionFixture.discard), "failed/discarded captures request cleanup");
  check(indexOf("seal:") < indexOf("idle:"), "sealed disposition precedes IDLE");
  check(hasPrefix("g2-fast-release") == gRecWasG2Source &&
        hasPrefix("g2-container-release") == gRecWasG2Source, "source teardown only for G2");
  if (gRecWasG2Source) {
    check(indexOf("g2-fast-release") < indexOf("g2-container-release") &&
          indexOf("g2-container-release") < indexOf("idle:"), "G2 release ordering before IDLE");
  }
  check(hasPrefix("live:") == liveShadowStarted, "live completion only for admitted shadow");
  if (liveShadowStarted) {
    const auto outcome = saved ? LiveAudioRecorderOutcome::SAVED :
        (discarded ? LiveAudioRecorderOutcome::DISCARDED : LiveAudioRecorderOutcome::FAILED);
    const std::string expected = "live:" + std::to_string(shadowCapture.owner) + ":" +
        std::to_string(static_cast<int>(outcome));
    check(hasPrefix(expected), "live shadow receives matching terminal outcome");
    check(indexOf("seal:") < indexOf("live:") && indexOf("live:") < indexOf("idle:"),
          "live finish between sealed close and IDLE");
  }
  check(!recordingFile.open && lockDepth == 0, "finalization leaves file closed and lock released");
  check(events.back() == "delete-task", "task deletion is last action");
}
static void recordTranscript(const std::string& name) {
  ++scenarios;
  std::cout << "CASE " << name << '\n';
  for (const auto& event : events) std::cout << event << '\n';
  std::cout << "bytes=";
  for (uint8_t byte : recordingFile.bytes) {
    const char* digits = "0123456789abcdef";
    std::cout << digits[byte >> 4] << digits[byte & 15];
  }
  std::cout << '\n';
}
static void unchanged() {
  for (int bits = 0; bits < 32; ++bits) {
    reset();
    gRecVadAutoStopped = bits & 1;
    gRecWasG2Source = bits & 2;
    liveShadowStarted = bits & 4;
    if (bits & 8) dispositionFixture.owner = shadowCapture.owner = MIC_RECORDING_OWNER_MANUAL;
    degraded = bits & 16;
    pushOk = !(bits & 16);
    if (bits == 0) gRecSampleRate = 0;
    if (bits == 1) micChannels = 2;
    if (bits == 2) std::strcpy(currentRecordingPath, "test.wav");
    runFinalization();
    verifyHeader();
    verifyOutcome(false, false, "", false);
    check(recordingFile.writes == 1 && recordingFile.flushes == HAS_FINAL_FLUSH && recordingFile.closes == 1,
          "success writes header and closes once with expected explicit flush");
    if (gRecVadAutoStopped) {
      const auto hostCount = std::count_if(events.begin(), events.end(), [](const std::string& e) {
        return e.compare(0, 5, "host:") == 0;
      });
      check(hostCount == 2, "VAD sends autostop and timing events even if push declines");
      check(indexOf("saved:") < indexOf("host:") && indexOf("host:") < indexOf("idle:"),
            "host callback after saved event before IDLE");
    }
    recordTranscript("success-" + std::to_string(bits));
  }
  for (int fault = 0; fault < 6; ++fault) {
    for (bool cleanup : {false, true}) {
      reset();
      removeOk = cleanup;
      gRecVadAutoStopped = gRecWasG2Source = true;
      const char* reason = "";
      switch (fault) {
        case 0: recordingFile.seekOk = false; reason = "WAV header finalization failed"; break;
        case 1: recordingFile.writeLimit = sizeof(WavHeader) - 1; reason = "WAV header finalization failed"; break;
        case 2: gRecWrittenSamples = 0; reason = "recording contained no PCM"; break;
        case 3: gRecCaptureFailed = true; reason = "short PCM write";
                std::strcpy(gRecFailureReason, reason); recordingFile.reportedWriteError = true; break;
        case 4: recordingFile.open = false; reason = "recording file unavailable during finalization"; break;
        case 5: recordingFile.writeLimit = 0; reason = "WAV header finalization failed"; break;
      }
      runFinalization();
      verifyOutcome(true, false, reason, cleanup);
      check(recordingFile.writes == static_cast<unsigned>(fault == 1 || fault == 5),
            "prior/empty/unavailable/seek failures do not write header");
      check(recordingFile.closes == static_cast<unsigned>(fault != 4), "open file closed on failure");
      check(recordingFile.errorReads == 0 && recordingFile.sizeReads == 0,
            "earlier failure bypasses final size/error query");
      recordTranscript("fault-" + std::to_string(fault) + "-cleanup-" + std::to_string(cleanup));
    }
  }
  for (bool cleanup : {false, true}) {
    for (bool during : {false, true}) {
      reset();
      gRecVadAutoStopped = gRecWasG2Source = true;
      removeOk = cleanup;
      dispositionFixture.discard = !during;
      discardDuringWrite = during;
      runFinalization();
      verifyHeader();
      verifyOutcome(!cleanup, cleanup, cleanup ? "" : "discard cleanup failed", cleanup);
      check(indexOf("close") < indexOf("seal:") && indexOf("seal:") < indexOf("remove:"),
            "late discard seals after close and then removes");
      recordTranscript("discard-" + std::to_string(cleanup) + "-during-" + std::to_string(during));
    }
  }
}
static void regression(const std::string& fault) {
  for (bool cleanup : {false, true}) {
    reset();
    gRecVadAutoStopped = gRecWasG2Source = true;
    removeOk = cleanup;
    if (fault == "short") recordingFile.bytes.resize(recordingFile.bytes.size() - 2);
    else if (fault == "long") recordingFile.bytes.resize(recordingFile.bytes.size() + 2);
    else if (fault == "error") recordingFile.reportedWriteError = true;
    else throw std::runtime_error("unknown regression selector");
    runFinalization();
    check(gRecCaptureFailed, "incomplete WAV must fail");
    verifyOutcome(true, false, "WAV file incomplete", cleanup);
    check(recordingFile.errorReads == 1, "completion reads stream error once");
    check(recordingFile.sizeReads == static_cast<unsigned>(fault != "error"),
          "reported error short-circuits size query");
    check(recordingFile.closes == 1 && recordingFile.flushes == 1,
          "incomplete file is flushed and closed once");
    recordTranscript(fault + "-cleanup-" + std::to_string(cleanup));
  }
}
int main(int argc, char** argv) {
  try {
    check(argc == 2, "expected unchanged or regression selector");
    if (std::string(argv[1]) == "unchanged") unchanged();
    else regression(argv[1]);
    std::cerr << scenarios << " scenarios; " << checks << " assertions passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 2;
  }
}
