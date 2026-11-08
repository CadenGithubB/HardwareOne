#include "System_STTRawCapture.h"
#include "System_BuildConfig.h"
#if ENABLE_LOCAL_STT

#include "HAL_Audio.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_STT.h"
#include "System_TaskUtils.h"
#include "System_VFS.h"
#include "stt/stt_raw_ring.h"

#include <Arduino.h>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char* kTag = "STT";
// 2^17 samples = 8.2 s at 16 kHz (256 KiB of PSRAM, only while recording):
// covers SD write stalls far longer than the card normally shows.
constexpr size_t kRingSamples = size_t(1) << 17;
constexpr size_t kWriteSamples = 4096;            // 8 KiB per locked write
constexpr uint32_t kFlushEveryMs = 5000;          // bounds loss on power cut
constexpr uint32_t kDrainStackBytes = 4096;
constexpr uint32_t kEndWaitMs = 15000;            // SessionEnd's wait for the drain

struct RawState {
  std::atomic<bool> armed{false};
  // Producer side (capture task only).
  bool producing = false;
  std::atomic<bool> stopRequested{false};
  std::atomic<bool> producerStopped{true};
  // Drain task lifetime; it owns the file and ring memory while alive.
  std::atomic<bool> drainAlive{false};
  std::atomic<bool> writeFailed{false};
  std::atomic<uint64_t> written{0};
  hw1::stt::RawRing ring;
  int16_t* memory = nullptr;
  File file;
  // Status fields shared with the command task (guarded by gMux).
  bool active = false;
  char path[64] = {};
  char result[160] = "no recording yet";
  uint32_t startedMs = 0;
  uint32_t sampleRate = 16000;
  uint8_t source = 0;
  // Mic-path and detector tuning in effect when the session began (sidecar).
  uint8_t pdmAmplify = 1;
  uint16_t pdmHpfHz = 36;
  int speechFloor = 45, silenceFloor = 16, noisePercentile = 25, decisionPre = 0;
};

RawState gRaw;
portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;

AuthContext rawAuth(const char* reason) { return VFS::systemAuth(VFS::Scopes::STT_RAW_SD, reason); }

void setResult(const char* text) {
  portENTER_CRITICAL(&gMux);
  snprintf(gRaw.result, sizeof(gRaw.result), "%s", text);
  portEXIT_CRITICAL(&gMux);
}

bool writeSpan(const int16_t* span, size_t samples) {
  FsLockGuard guard("stt.raw.write");
  const size_t bytes = samples * sizeof(int16_t);
  return gRaw.file.write(reinterpret_cast<const uint8_t*>(span), bytes) == bytes;
}

void drainTask(void*) {
  uint32_t lastFlushMs = millis();
  for (;;) {
    const int16_t* span = nullptr;
    const size_t n = gRaw.ring.peek(&span, kWriteSamples);
    if (n) {
      if (!gRaw.writeFailed.load(std::memory_order_relaxed) && !writeSpan(span, n)) {
        gRaw.writeFailed.store(true, std::memory_order_relaxed);
        gRaw.stopRequested.store(true, std::memory_order_release);  // producer stops at its next push
      }
      gRaw.ring.consume(n);
      if (!gRaw.writeFailed.load(std::memory_order_relaxed))
        gRaw.written.fetch_add(n, std::memory_order_relaxed);
      if (millis() - lastFlushMs >= kFlushEveryMs) {
        FsLockGuard guard("stt.raw.flush");
        gRaw.file.flush();
        lastFlushMs = millis();
      }
      continue;
    }
    // Empty: finish only once the producer can no longer add samples.
    if (gRaw.producerStopped.load(std::memory_order_acquire) && !gRaw.ring.pending()) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  {
    FsLockGuard guard("stt.raw.close");
    gRaw.file.flush();
    gRaw.file.close();
  }
  // The producer has stopped, so nothing reads the ring's buffer again; its
  // counters stay for SessionEnd's sidecar until the next SessionBegin resets it.
  int16_t* memory = gRaw.memory;
  gRaw.memory = nullptr;
  if (memory) { memset(memory, 0, kRingSamples * sizeof(int16_t)); free(memory); }
  gRaw.drainAlive.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

// The sidecar describes the .pcm exactly: file sample i is segmenter session
// sample i, except inside the listed gaps, whose samples were not written.
bool writeSidecar(const char* pcmPath, uint32_t endedMs, bool drained) {
  char jsonPath[sizeof(gRaw.path)];
  snprintf(jsonPath, sizeof(jsonPath), "%s", pcmPath);
  char* dot = strrchr(jsonPath, '.');
  if (!dot) return false;
  strcpy(dot, ".json");
  constexpr size_t kCap = 1536;
  char* text = static_cast<char*>(ps_alloc(kCap, AllocPref::PreferPSRAM, "stt.raw_json"));
  if (!text) return false;
  const auto& ring = gRaw.ring;
  const esp_app_desc_t* app = esp_app_get_description();
  int n = snprintf(text, kCap,
      "{\"format\":\"s16le\",\"channels\":1,\"rate\":%" PRIu32 ",\"source\":%u,"
      "\"pdm_amplify\":%u,\"pdm_hpf_hz\":%u,\"speech_floor\":%d,\"silence_floor\":%d,\"noise_percentile\":%d,\"decision_pre\":%d,"
      "\"firmware\":\"%s\",\"started_uptime_ms\":%" PRIu32 ",\"ended_uptime_ms\":%" PRIu32 ","
      "\"samples_seen\":%" PRIu64 ",\"samples_written\":%" PRIu64 ",\"samples_dropped\":%" PRIu64 ","
      "\"write_error\":%s,\"drain_complete\":%s,\"gaps_lost\":%" PRIu32 ",\"gaps\":[",
      gRaw.sampleRate, unsigned(gRaw.source),
      unsigned(gRaw.pdmAmplify), unsigned(gRaw.pdmHpfHz), gRaw.speechFloor, gRaw.silenceFloor,
      gRaw.noisePercentile, gRaw.decisionPre,
      app ? app->version : "?", gRaw.startedMs, endedMs,
      ring.seen(), gRaw.written.load(), ring.dropped(),
      gRaw.writeFailed.load() ? "true" : "false", drained ? "true" : "false", ring.gapsLost());
  for (size_t i = 0; i < ring.gapCount() && n > 0 && size_t(n) < kCap; ++i)
    n += snprintf(text + n, kCap - n, "%s[%" PRIu64 ",%" PRIu32 "]", i ? "," : "",
                  ring.gap(i).at, ring.gap(i).samples);
  if (n > 0 && size_t(n) < kCap) n += snprintf(text + n, kCap - n, "]}\n");
  bool ok = n > 0 && size_t(n) < kCap;
  if (ok) {
    FsLockGuard guard("stt.raw.sidecar");
    File f = VFS::openGuarded(String(jsonPath), "w", rawAuth("stt.raw.sidecar"), true);
    ok = f && f.write(reinterpret_cast<const uint8_t*>(text), size_t(n)) == size_t(n);
    if (f) f.close();
  }
  free(text);
  return ok;
}

}  // namespace

bool sttRawCaptureArm(bool on, char* reason, size_t reasonCap) {
  if (reason && reasonCap) reason[0] = '\0';
  if (on && !VFS::isSDWritable()) {
    if (reason) snprintf(reason, reasonCap, "the SD card is not writable");
    return false;
  }
  gRaw.armed.store(on, std::memory_order_release);
  if (!on) gRaw.stopRequested.store(true, std::memory_order_release);
  return true;
}

const char* sttRawCaptureStatus(char* out, size_t cap) {
  bool active;
  char path[sizeof(gRaw.path)], result[sizeof(gRaw.result)];
  uint32_t rate;
  portENTER_CRITICAL(&gMux);
  active = gRaw.active;
  memcpy(path, gRaw.path, sizeof(path));
  memcpy(result, gRaw.result, sizeof(result));
  rate = gRaw.sampleRate ? gRaw.sampleRate : 16000;
  portEXIT_CRITICAL(&gMux);
  const uint64_t written = gRaw.written.load(std::memory_order_relaxed);
  if (active) {
    snprintf(out, cap, "OK: STT raw input %s; recording %s (%.1f s written%s)",
             gRaw.armed.load() ? "armed" : "disarmed, stopping", path, double(written) / rate,
             gRaw.writeFailed.load() ? ", WRITE ERROR" : "");
  } else {
    snprintf(out, cap, "OK: STT raw input %s; last: %s",
             gRaw.armed.load() ? "armed for the next continuous session" : "off", result);
  }
  return out;
}

void sttRawCaptureSessionBegin(uint8_t audioSource, uint32_t sampleRate) {
  gRaw.producing = false;
  if (!gRaw.armed.load(std::memory_order_acquire)) return;
  if (gRaw.drainAlive.load(std::memory_order_acquire)) {
    setResult("not recorded: the previous recording is still being written");
    return;
  }
  if (!VFS::isSDWritable()) { setResult("not recorded: the SD card is not writable"); return; }
  auto* memory = static_cast<int16_t*>(ps_alloc(kRingSamples * sizeof(int16_t), AllocPref::RequirePSRAM, "stt.raw_ring"));
  if (!memory) { setResult("not recorded: no PSRAM for the 256 KiB ring"); return; }
  char path[sizeof(gRaw.path)];
  snprintf(path, sizeof(path), "%s/raw_%08" PRIx32 ".pcm", VFS::Scopes::STT_RAW_SD, esp_random());
  File file;
  {
    FsLockGuard guard("stt.raw.open");
    const AuthContext auth = rawAuth("stt.raw.open");
    if (!VFS::existsGuarded(VFS::Scopes::STT_RAW_SD, auth)) (void)VFS::mkdirGuarded(VFS::Scopes::STT_RAW_SD, auth);
    file = VFS::openGuarded(String(path), "w", auth, true);
  }
  if (!file) {
    free(memory);
    setResult("not recorded: could not create a file in /sd/STT Raw");
    return;
  }
  gRaw.memory = memory;
  gRaw.file = file;
  gRaw.ring.reset(memory, kRingSamples);
  gRaw.written.store(0);
  gRaw.writeFailed.store(false);
  gRaw.stopRequested.store(false);
  gRaw.producerStopped.store(false);
  gRaw.drainAlive.store(true, std::memory_order_release);
  portENTER_CRITICAL(&gMux);
  gRaw.active = true;
  gRaw.startedMs = millis();
  gRaw.sampleRate = sampleRate;
  gRaw.source = audioSource;
  audioGetPdmTuning(&gRaw.pdmAmplify, &gRaw.pdmHpfHz);
  sttDetectorProfileInUse(&gRaw.speechFloor, &gRaw.silenceFloor, &gRaw.noisePercentile, &gRaw.decisionPre);
  memcpy(gRaw.path, path, sizeof(path));
  portEXIT_CRITICAL(&gMux);
  taskStackRecord("stt_rawsave", kDrainStackBytes);
  if (xTaskCreatePinnedToCore(drainTask, "stt_rawsave", kDrainStackBytes, nullptr,
                              TASK_PRIORITY_LOW, nullptr, PRO_CORE) != pdPASS) {
    gRaw.producerStopped.store(true);
    {
      FsLockGuard guard("stt.raw.abort");
      gRaw.file.close();
      (void)VFS::removeGuarded(String(path), rawAuth("stt.raw.abort"));
    }
    gRaw.ring.reset(nullptr, 0);
    gRaw.memory = nullptr;
    free(memory);
    gRaw.drainAlive.store(false);
    portENTER_CRITICAL(&gMux);
    gRaw.active = false;
    portEXIT_CRITICAL(&gMux);
    setResult("not recorded: could not start the writer task");
    return;
  }
  gRaw.producing = true;
  ESP_LOGI(kTag, "TL raw_start path=%s rate=%" PRIu32 " source=%u", path, sampleRate, unsigned(audioSource));
}

void sttRawCapturePush(const int16_t* pcm, size_t samples) {
  if (!gRaw.producing) return;
  if (gRaw.stopRequested.load(std::memory_order_acquire)) {
    gRaw.producing = false;
    gRaw.producerStopped.store(true, std::memory_order_release);
    return;
  }
  (void)gRaw.ring.push(pcm, samples);
}

void sttRawCaptureSessionEnd() {
  bool active;
  portENTER_CRITICAL(&gMux);
  active = gRaw.active;
  portEXIT_CRITICAL(&gMux);
  if (!active) return;
  gRaw.producing = false;
  gRaw.producerStopped.store(true, std::memory_order_release);
  const uint32_t endedMs = millis();
  const uint32_t waitStart = endedMs;
  while (gRaw.drainAlive.load(std::memory_order_acquire) && millis() - waitStart < kEndWaitMs)
    vTaskDelay(pdMS_TO_TICKS(10));
  const bool drained = !gRaw.drainAlive.load(std::memory_order_acquire);
  char path[sizeof(gRaw.path)];
  portENTER_CRITICAL(&gMux);
  memcpy(path, gRaw.path, sizeof(path));
  portEXIT_CRITICAL(&gMux);
  const bool sidecar = writeSidecar(path, endedMs, drained);
  char result[sizeof(gRaw.result)];
  const uint32_t rate = gRaw.sampleRate ? gRaw.sampleRate : 16000;
  snprintf(result, sizeof(result), "%s, %.1f s, %" PRIu64 " dropped%s%s%s", path,
           double(gRaw.written.load()) / rate, gRaw.ring.dropped(),
           gRaw.writeFailed.load() ? ", write error" : "", drained ? "" : ", still flushing",
           sidecar ? "" : ", no sidecar");
  portENTER_CRITICAL(&gMux);
  gRaw.active = false;
  snprintf(gRaw.result, sizeof(gRaw.result), "%s", result);
  portEXIT_CRITICAL(&gMux);
  ESP_LOGI(kTag, "TL raw_end %s", result);
}

#endif  // ENABLE_LOCAL_STT
