#include "System_LiveAudio.h"

#include <atomic>
#include <ctype.h>
#include <esp_attr.h>
#include <esp_crc.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "G2_Glasses.h"
#include "System_AuthIdentity.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_TaskUtils.h"
#include "System_UartLink.h"
#include "System_User.h"
#include "System_Utils.h"

namespace {

constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kSyntheticFlag = 0x01;
constexpr uint8_t kSourceSynthetic = 0;
constexpr uint8_t kFormatS16LeMono = 1;
constexpr uint32_t kSampleRate = 16000;
constexpr uint16_t kLogicalChunkSamples = 2048;
constexpr uint16_t kPhysicalFrameSamples = 500;
constexpr uint32_t kLeaseTtlMs = 3000;
constexpr uint32_t kLeaseRenewMs = 1000;
constexpr uint32_t kCaptureArmTtlMs = 5000;
constexpr uint32_t kMinBaud = 921600;
constexpr uint32_t kMaxDurationMs = 60000;
constexpr uint32_t kFrameAdmissionMs = 100;
constexpr uint32_t kRecorderPollMs = 20;

constexpr uint32_t kShadowSlots = 4;
constexpr size_t kShadowSlotBytes = 4096;
constexpr size_t kShadowBytes = kShadowSlots * kShadowSlotBytes;

static_assert(24 + kPhysicalFrameSamples * 2 == UARTLINK_FRAME_MAX_PAYLOAD,
              "live PCM frame must exactly fit the UART payload ceiling");
static_assert(kShadowSlotBytes == kLogicalChunkSamples * sizeof(int16_t),
              "one recorder chunk must fit one shadow slot");

enum AbortReason : uint8_t {
  ABORT_NONE = 0,
  ABORT_LEASE_EXPIRED = 1,
  ABORT_AUTH_LOST = 2,
  ABORT_LINK_LOST = 3,
  ABORT_RELEASED = 4,
  ABORT_HOST_REQUEST = 5,
  ABORT_TX_BACKPRESSURE = 6,
  ABORT_INTERNAL = 7,
};

enum StreamMode : uint8_t {
  STREAM_NONE = 0,
  STREAM_SYNTHETIC = 1,
  STREAM_RECORDER = 2,
};

enum TerminalDecision : uint8_t {
  TERMINAL_OPEN = 0,
  TERMINAL_ABORT = 1,
  TERMINAL_END = 2,
};

struct LeaseState {
  bool valid = false;
  bool shadowEnabled = false;
  bool shadowNative = false;
  uint64_t controller = 0;
  uint64_t shadowExchange = 0;
  uint32_t sessionEpoch = 0;
  uint32_t deadlineMs = 0;
};

struct CaptureArm {
  bool valid = false;
  bool native = false;
  uint64_t controller = 0;
  uint64_t exchange = 0;
  uint32_t sessionEpoch = 0;
  uint32_t deadlineMs = 0;
};

struct StreamState {
  bool active = false;
  bool abortRequested = false;
  bool endRequested = false;
  uint8_t abortReason = ABORT_NONE;
  uint8_t mode = STREAM_NONE;
  uint8_t flags = 0;
  uint8_t source = kSourceSynthetic;
  uint64_t controller = 0;
  uint64_t exchange = 0;
  uint32_t sessionEpoch = 0;
  uint32_t queueGeneration = 0;
  uint32_t durationMs = 0;
  uint32_t totalSamples = 0;
  uint32_t sentSamples = 0;
  uint32_t crc32 = 0;
  uint32_t pcmFrames = 0;
  uint32_t startedMs = 0;
};

struct LastState {
  bool valid = false;
  bool ended = false;
  bool terminalSent = false;
  uint8_t reason = ABORT_NONE;
  uint8_t mode = STREAM_NONE;
  uint8_t source = kSourceSynthetic;
  uint64_t controller = 0;
  uint64_t exchange = 0;
  uint32_t sessionEpoch = 0;
  uint32_t sentSamples = 0;
  uint32_t droppedSamples = 0;
  uint32_t crc32 = 0;
  uint32_t elapsedMs = 0;
};

struct ShadowStats {
  uint32_t starts = 0;
  uint32_t skips = 0;
  uint32_t overflows = 0;
  uint32_t allocFailures = 0;
  uint32_t highWater = 0;
};

static portMUX_TYPE sStateMux = portMUX_INITIALIZER_UNLOCKED;
static LeaseState sLease;
static CaptureArm sCaptureArm;
static StreamState sStream;
static LastState sLast;
static ShadowStats sShadowStats;
static bool sBulkTransferActive = false;
static TaskHandle_t sTxTask = nullptr;
EXT_RAM_BSS_ATTR static char sReply[1024];

// Strict SPSC queue. Storage is exactly 16 KiB of PSRAM and is never silently
// replaced by DRAM. The recorder task is the sole producer; live_audio_tx is
// the sole consumer.
static uint8_t* sShadowStorage = nullptr;
static uint16_t sShadowLengths[kShadowSlots] = {};
static std::atomic<uint32_t> sShadowHead{0};
static std::atomic<uint32_t> sShadowTail{0};
static std::atomic<uint32_t> sShadowGeneration{1};
static std::atomic<bool> sShadowAccepting{false};
// First-wins terminal linearization shared by cancellation and the TX worker.
static std::atomic<uint8_t> sTerminalDecision{TERMINAL_OPEN};

bool timeReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool idStructurallyValid(uint64_t id) {
  return static_cast<uint32_t>(id >> 32) != 0 &&
         static_cast<uint32_t>(id) != 0;
}

bool parseStrictId(const String& token, uint64_t& out) {
  if (token.length() != 16) return false;
  uint32_t high = 0;
  uint32_t low = 0;
  for (size_t i = 0; i < 16; ++i) {
    const int nibble = hexNibble(token[i]);
    if (nibble < 0) return false;
    if (i < 8) high = (high << 4) | static_cast<uint32_t>(nibble);
    else low = (low << 4) | static_cast<uint32_t>(nibble);
  }
  if (high == 0 || low == 0) return false;
  out = (static_cast<uint64_t>(high) << 32) | low;
  return true;
}

bool parseStrictIdSpan(const char* text, size_t len, uint64_t& out) {
  if (!text || len != 16) return false;
  uint32_t high = 0;
  uint32_t low = 0;
  for (size_t i = 0; i < 16; ++i) {
    const int nibble = hexNibble(text[i]);
    if (nibble < 0) return false;
    if (i < 8) high = (high << 4) | static_cast<uint32_t>(nibble);
    else low = (low << 4) | static_cast<uint32_t>(nibble);
  }
  if (high == 0 || low == 0) return false;
  out = (static_cast<uint64_t>(high) << 32) | low;
  return true;
}

struct TextToken {
  const char* ptr = nullptr;
  size_t len = 0;
};

bool nextTextToken(const char*& cursor, TextToken& token) {
  if (!cursor) return false;
  while (*cursor && isspace(static_cast<unsigned char>(*cursor))) ++cursor;
  if (!*cursor) {
    token = {};
    return false;
  }
  token.ptr = cursor;
  while (*cursor && !isspace(static_cast<unsigned char>(*cursor))) ++cursor;
  token.len = static_cast<size_t>(cursor - token.ptr);
  return true;
}

bool textTokenEquals(const TextToken& token, const char* literal) {
  if (!literal) return false;
  const size_t len = strlen(literal);
  return token.len == len && memcmp(token.ptr, literal, len) == 0;
}

bool textTokenEqualsIgnoreCase(const TextToken& token, const char* literal) {
  if (!literal) return false;
  const size_t len = strlen(literal);
  if (token.len != len) return false;
  for (size_t i = 0; i < len; ++i) {
    const unsigned char lhs = static_cast<unsigned char>(token.ptr[i]);
    const unsigned char rhs = static_cast<unsigned char>(literal[i]);
    if (tolower(lhs) != tolower(rhs)) return false;
  }
  return true;
}

bool parseCanonicalReadyLine(const char* line, uint64_t& controller) {
  const char* cursor = line;
  TextToken root;
  TextToken verb;
  TextToken version;
  TextToken id;
  TextToken extra;
  return nextTextToken(cursor, root) && textTokenEquals(root, "liveaudio") &&
         nextTextToken(cursor, verb) && textTokenEquals(verb, "ready") &&
         nextTextToken(cursor, version) && textTokenEquals(version, "1") &&
         nextTextToken(cursor, id) && parseStrictIdSpan(id.ptr, id.len, controller) &&
         !nextTextToken(cursor, extra);
}

void formatId(uint64_t id, char out[17]) {
  snprintf(out, 17, "%08lx%08lx",
           static_cast<unsigned long>(id >> 32),
           static_cast<unsigned long>(id & 0xffffffffu));
}

bool parseDurationMs(const String& token, uint32_t& out) {
  if (token.length() == 0 || token.length() > 5) return false;
  uint32_t value = 0;
  for (size_t i = 0; i < token.length(); ++i) {
    const char c = token[i];
    if (c < '0' || c > '9') return false;
    value = value * 10u + static_cast<uint32_t>(c - '0');
  }
  if (value == 0 || value > kMaxDurationMs) return false;
  out = value;
  return true;
}

void putLe16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

void putLe32(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
}

void putLe64(uint8_t* p, uint64_t value) {
  putLe32(p, static_cast<uint32_t>(value));
  putLe32(p + 4, static_cast<uint32_t>(value >> 32));
}

const char* abortReasonName(uint8_t reason) {
  switch (reason) {
    case ABORT_LEASE_EXPIRED: return "lease_expired";
    case ABORT_AUTH_LOST: return "auth_lost";
    case ABORT_LINK_LOST: return "link_lost";
    case ABORT_RELEASED: return "released";
    case ABORT_HOST_REQUEST: return "host_abort";
    case ABORT_TX_BACKPRESSURE: return "tx_backpressure";
    case ABORT_INTERNAL: return "internal";
    default: return "none";
  }
}

const char* streamModeName(uint8_t mode) {
  switch (mode) {
    case STREAM_SYNTHETIC: return "synthetic";
    case STREAM_RECORDER: return "recorder";
    default: return "none";
  }
}

bool crc32SelfTest() {
  static const uint8_t kGoldenInput[] = {'1', '2', '3', '4', '5',
                                          '6', '7', '8', '9'};
  return esp_crc32_le(0, kGoldenInput, sizeof(kGoldenInput)) == 0xcbf43926u;
}

bool realUartCaller() {
  const AuthContext& ctx = currentAuthContext();
  return ctx.transport == SOURCE_UART && uartLinkSessionEpoch() != 0 &&
         ctx.user.length() > 0 && ctx.user != "AuthBypass";
}

uint32_t leaseRemainingLocked(uint32_t now) {
  if (!sLease.valid || sLease.sessionEpoch == 0 ||
      sLease.sessionEpoch != uartLinkSessionEpoch() ||
      timeReached(now, sLease.deadlineMs)) return 0;
  return sLease.deadlineMs - now;
}

bool leaseMatchesLocked(uint64_t controller, uint32_t sessionEpoch,
                        uint32_t now) {
  return sLease.valid && sLease.controller == controller &&
         sLease.sessionEpoch == sessionEpoch && sessionEpoch != 0 &&
         !timeReached(now, sLease.deadlineMs);
}

void requestAbortLocked(uint8_t reason) {
  if (!sStream.active) return;
  uint8_t expected = TERMINAL_OPEN;
  if (!sTerminalDecision.compare_exchange_strong(
          expected, static_cast<uint8_t>(TERMINAL_ABORT),
          std::memory_order_acq_rel, std::memory_order_acquire) &&
      expected == TERMINAL_END) {
    return;  // END already committed before this cancellation.
  }
  if (sStream.active && !sStream.abortRequested) {
    sStream.abortRequested = true;
    sStream.abortReason = reason;
  }
}

uint8_t streamAbortReason(uint64_t controller, uint64_t exchange,
                          uint32_t sessionEpoch) {
  uint8_t reason = ABORT_NONE;
  portENTER_CRITICAL(&sStateMux);
  if (!sStream.active || sStream.controller != controller ||
      sStream.exchange != exchange ||
      sStream.sessionEpoch != sessionEpoch) {
    reason = ABORT_INTERNAL;
  } else if (sStream.abortRequested) {
    reason = sStream.abortReason;
  }
  portEXIT_CRITICAL(&sStateMux);
  if (reason != ABORT_NONE) return reason;

  if (!uartLinkIsRunning()) reason = ABORT_LINK_LOST;
  else if (uartLinkSessionEpoch() != sessionEpoch)
    reason = ABORT_AUTH_LOST;
  else {
    const uint32_t now = millis();
    portENTER_CRITICAL(&sStateMux);
    if (!leaseMatchesLocked(controller, sessionEpoch, now))
      reason = ABORT_LEASE_EXPIRED;
    portEXIT_CRITICAL(&sStateMux);
  }

  if (reason != ABORT_NONE) {
    portENTER_CRITICAL(&sStateMux);
    requestAbortLocked(reason);
    portEXIT_CRITICAL(&sStateMux);
  }
  return reason;
}

uint8_t currentRequestedAbort(uint64_t controller, uint64_t exchange,
                              uint32_t sessionEpoch) {
  uint8_t reason = ABORT_INTERNAL;
  portENTER_CRITICAL(&sStateMux);
  if (sStream.active && sStream.controller == controller &&
      sStream.exchange == exchange &&
      sStream.sessionEpoch == sessionEpoch) {
    reason = sStream.abortRequested
                 ? sStream.abortReason
                 : static_cast<uint8_t>(ABORT_NONE);
  }
  portEXIT_CRITICAL(&sStateMux);
  return reason;
}

bool writeStreamFrame(uint8_t type, uint16_t seq, const uint8_t* payload,
                      size_t len, uint64_t controller, uint64_t exchange,
                      uint32_t sessionEpoch) {
  const uint32_t started = millis();
  while (true) {
    if (streamAbortReason(controller, exchange, sessionEpoch) != ABORT_NONE)
      return false;
    if (uartLinkTryWriteFrameForSession(sessionEpoch, type, seq, payload, len))
      return true;
    if (millis() - started >= kFrameAdmissionMs) {
      portENTER_CRITICAL(&sStateMux);
      requestAbortLocked(ABORT_TX_BACKPRESSURE);
      portEXIT_CRITICAL(&sStateMux);
      return false;
    }
    vTaskDelay(1);
  }
}

bool writeTerminalFrame(uint8_t type, uint16_t seq, const uint8_t* payload,
                        size_t len, uint32_t sessionEpoch) {
  const uint32_t started = millis();
  while (uartLinkIsRunning() &&
         uartLinkSessionEpoch() == sessionEpoch) {
    if (uartLinkTryWriteFrameForSession(sessionEpoch, type, seq, payload, len))
      return true;
    if (millis() - started >= kFrameAdmissionMs) return false;
    vTaskDelay(1);
  }
  return false;
}

void encodeBegin(uint8_t out[28], const StreamState& job) {
  out[0] = kProtocolVersion;
  out[1] = job.flags;
  out[2] = job.source;
  out[3] = kFormatS16LeMono;
  putLe32(out + 4, kSampleRate);
  putLe64(out + 8, job.exchange);
  putLe64(out + 16, job.controller);
  putLe16(out + 24, kLogicalChunkSamples);
  putLe16(out + 26, 0);
}

void encodeTerminal(uint8_t out[30], uint8_t reason, uint64_t controller,
                    uint64_t exchange, uint32_t sentSamples, uint32_t crc32,
                    uint32_t droppedSamples) {
  out[0] = kProtocolVersion;
  out[1] = reason;
  putLe64(out + 2, exchange);
  putLe64(out + 10, controller);
  putLe32(out + 18, sentSamples);
  putLe32(out + 22, crc32);
  putLe32(out + 26, droppedSamples);
}

void publishProgress(const StreamState& job, uint32_t sentSamples,
                     uint32_t crc32, uint32_t pcmFrames) {
  portENTER_CRITICAL(&sStateMux);
  if (sStream.active && sStream.controller == job.controller &&
      sStream.exchange == job.exchange &&
      sStream.sessionEpoch == job.sessionEpoch) {
    sStream.sentSamples = sentSamples;
    sStream.crc32 = crc32;
    sStream.pcmFrames = pcmFrames;
  }
  portEXIT_CRITICAL(&sStateMux);
}

uint32_t currentTotalSamples(const StreamState& job) {
  uint32_t total = job.totalSamples;
  portENTER_CRITICAL(&sStateMux);
  if (sStream.active && sStream.controller == job.controller &&
      sStream.exchange == job.exchange &&
      sStream.sessionEpoch == job.sessionEpoch) {
    total = sStream.totalSamples;
  }
  portEXIT_CRITICAL(&sStateMux);
  return total;
}

bool recorderEndRequested(const StreamState& job) {
  bool requested = false;
  portENTER_CRITICAL(&sStateMux);
  if (sStream.active && sStream.controller == job.controller &&
      sStream.exchange == job.exchange &&
      sStream.sessionEpoch == job.sessionEpoch) {
    requested = sStream.endRequested;
  }
  portEXIT_CRITICAL(&sStateMux);
  return requested;
}

void finishStream(const StreamState& job, bool ended, bool terminalSent,
                  uint8_t reason, uint32_t sentSamples,
                  uint32_t totalSamples, uint32_t crc32) {
  const uint32_t dropped = totalSamples > sentSamples
                               ? totalSamples - sentSamples : 0;
  if (job.mode == STREAM_RECORDER) {
    sShadowAccepting.store(false, std::memory_order_release);
    sShadowGeneration.fetch_add(1, std::memory_order_acq_rel);
    // Reset while the old sStream is still active, so no new admission can
    // publish before these indices are cleared.
    sShadowHead.store(0, std::memory_order_release);
    sShadowTail.store(0, std::memory_order_release);
  }
  portENTER_CRITICAL(&sStateMux);
  if (sStream.active && sStream.controller == job.controller &&
      sStream.exchange == job.exchange &&
      sStream.sessionEpoch == job.sessionEpoch) {
    sLast.valid = true;
    sLast.ended = ended;
    sLast.terminalSent = terminalSent;
    sLast.reason = reason;
    sLast.mode = job.mode;
    sLast.source = job.source;
    sLast.controller = job.controller;
    sLast.exchange = job.exchange;
    sLast.sessionEpoch = job.sessionEpoch;
    sLast.sentSamples = sentSamples;
    sLast.droppedSamples = dropped;
    sLast.crc32 = crc32;
    sLast.elapsedMs = millis() - job.startedMs;
    sTerminalDecision.store(TERMINAL_OPEN, std::memory_order_release);
    sStream = StreamState{};
  }
  portEXIT_CRITICAL(&sStateMux);
}

bool sendPcmFrame(const StreamState& job, uint16_t& seq, uint8_t* pcm,
                  const uint8_t* bytes, uint16_t frameSamples,
                  uint32_t sentSamples, uint32_t& crc32,
                  uint32_t& pcmFrames) {
  pcm[0] = kProtocolVersion;
  pcm[1] = job.flags;
  putLe64(pcm + 2, job.exchange);
  putLe64(pcm + 10, job.controller);
  putLe32(pcm + 18, sentSamples);
  putLe16(pcm + 22, frameSamples);
  const size_t pcmBytes = static_cast<size_t>(frameSamples) * sizeof(int16_t);
  memcpy(pcm + 24, bytes, pcmBytes);
  if (!writeStreamFrame(UARTLINK_FRAME_LIVE_PCM, seq, pcm, 24 + pcmBytes,
                        job.controller, job.exchange, job.sessionEpoch)) {
    return false;
  }
  crc32 = esp_crc32_le(crc32, pcm + 24, pcmBytes);
  ++pcmFrames;
  ++seq;
  return true;
}

void runSynthetic(const StreamState& job, uint16_t& seq,
                  uint32_t& sentSamples, uint32_t& crc32,
                  uint32_t& pcmFrames, uint8_t& reason) {
  uint8_t pcm[UARTLINK_FRAME_MAX_PAYLOAD];
  TickType_t logicalWake = xTaskGetTickCount();
  while (reason == ABORT_NONE && sentSamples < job.totalSamples) {
    vTaskDelayUntil(&logicalWake, pdMS_TO_TICKS(128));
    reason = streamAbortReason(job.controller, job.exchange, job.sessionEpoch);
    if (reason != ABORT_NONE) break;
    uint32_t logicalRemaining = job.totalSamples - sentSamples;
    if (logicalRemaining > kLogicalChunkSamples)
      logicalRemaining = kLogicalChunkSamples;
    while (reason == ABORT_NONE && logicalRemaining > 0) {
      uint16_t frameSamples = static_cast<uint16_t>(logicalRemaining);
      if (frameSamples > kPhysicalFrameSamples)
        frameSamples = kPhysicalFrameSamples;
      const uint16_t exchangeLow = static_cast<uint16_t>(job.exchange);
      for (uint16_t i = 0; i < frameSamples; ++i) {
        const uint32_t sampleIndex = sentSamples + i;
        const uint16_t bits = static_cast<uint16_t>(
            ((sampleIndex * 257u) ^ exchangeLow) & 0xffffu);
        putLe16(pcm + 24 + static_cast<size_t>(i) * 2, bits);
      }
      if (!sendPcmFrame(job, seq, pcm, pcm + 24, frameSamples,
                        sentSamples, crc32, pcmFrames)) {
        reason = currentRequestedAbort(job.controller, job.exchange,
                                       job.sessionEpoch);
        if (reason == ABORT_NONE) reason = ABORT_INTERNAL;
        break;
      }
      sentSamples += frameSamples;
      logicalRemaining -= frameSamples;
      publishProgress(job, sentSamples, crc32, pcmFrames);
    }
  }
}

void runRecorder(const StreamState& job, uint16_t& seq,
                 uint32_t& sentSamples, uint32_t& crc32,
                 uint32_t& pcmFrames, uint8_t& reason) {
  uint8_t pcm[UARTLINK_FRAME_MAX_PAYLOAD];
  while (reason == ABORT_NONE) {
    reason = streamAbortReason(job.controller, job.exchange, job.sessionEpoch);
    if (reason != ABORT_NONE) break;
    const uint32_t tail = sShadowTail.load(std::memory_order_relaxed);
    const uint32_t head = sShadowHead.load(std::memory_order_acquire);
    if (tail != head) {
      const uint32_t slot = tail % kShadowSlots;
      const uint16_t slotBytes = sShadowLengths[slot];
      size_t offset = 0;
      while (reason == ABORT_NONE && offset < slotBytes) {
        size_t bytes = slotBytes - offset;
        const size_t maxBytes = kPhysicalFrameSamples * sizeof(int16_t);
        if (bytes > maxBytes) bytes = maxBytes;
        const uint16_t frameSamples = static_cast<uint16_t>(bytes / 2);
        if (!sendPcmFrame(job, seq, pcm,
                          sShadowStorage + slot * kShadowSlotBytes + offset,
                          frameSamples, sentSamples, crc32, pcmFrames)) {
          reason = currentRequestedAbort(job.controller, job.exchange,
                                         job.sessionEpoch);
          if (reason == ABORT_NONE) reason = ABORT_INTERNAL;
          break;
        }
        offset += bytes;
        sentSamples += frameSamples;
        publishProgress(job, sentSamples, crc32, pcmFrames);
      }
      if (reason == ABORT_NONE)
        sShadowTail.store(tail + 1, std::memory_order_release);
      continue;
    }
    if (recorderEndRequested(job)) break;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kRecorderPollMs));
  }
}

void liveAudioTxTask(void*) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    StreamState job;
    portENTER_CRITICAL(&sStateMux);
    job = sStream;
    portEXIT_CRITICAL(&sStateMux);
    if (!job.active) continue;

    uint16_t seq = 0;
    uint32_t sentSamples = 0;
    uint32_t crc32 = 0;
    uint32_t pcmFrames = 0;
    uint8_t reason = ABORT_NONE;

    uint8_t begin[28];
    encodeBegin(begin, job);
    if (!writeStreamFrame(UARTLINK_FRAME_LIVE_BEGIN, seq, begin,
                          sizeof(begin), job.controller, job.exchange,
                          job.sessionEpoch)) {
      reason = currentRequestedAbort(job.controller, job.exchange,
                                     job.sessionEpoch);
      if (reason == ABORT_NONE) reason = ABORT_INTERNAL;
    } else {
      ++seq;
    }

    if (reason == ABORT_NONE) {
      if (job.mode == STREAM_SYNTHETIC)
        runSynthetic(job, seq, sentSamples, crc32, pcmFrames, reason);
      else if (job.mode == STREAM_RECORDER)
        runRecorder(job, seq, sentSamples, crc32, pcmFrames, reason);
      else
        reason = ABORT_INTERNAL;
    }

    if (reason == ABORT_NONE)
      reason = streamAbortReason(job.controller, job.exchange,
                                 job.sessionEpoch);
    bool ended = false;
    if (reason == ABORT_NONE) {
      uint8_t expected = TERMINAL_OPEN;
      ended = sTerminalDecision.compare_exchange_strong(
          expected, static_cast<uint8_t>(TERMINAL_END),
          std::memory_order_acq_rel, std::memory_order_acquire);
      if (!ended) {
        reason = currentRequestedAbort(job.controller, job.exchange,
                                       job.sessionEpoch);
        if (reason == ABORT_NONE) reason = ABORT_INTERNAL;
      }
    } else {
      uint8_t expected = TERMINAL_OPEN;
      (void)sTerminalDecision.compare_exchange_strong(
          expected, static_cast<uint8_t>(TERMINAL_ABORT),
          std::memory_order_acq_rel, std::memory_order_acquire);
    }
    const uint32_t totalSamples = currentTotalSamples(job);
    const uint32_t droppedSamples = totalSamples > sentSamples
                                        ? totalSamples - sentSamples : 0;
    uint8_t terminal[30];
    encodeTerminal(terminal, reason, job.controller, job.exchange, sentSamples,
                   crc32, droppedSamples);
    const bool terminalSent = writeTerminalFrame(
        ended ? UARTLINK_FRAME_LIVE_END : UARTLINK_FRAME_LIVE_ABORT,
        seq, terminal, sizeof(terminal), job.sessionEpoch);
    uint8_t finalReason = reason;
    if (ended && !terminalSent) {
      ended = false;
      finalReason = ABORT_TX_BACKPRESSURE;
    }
    finishStream(job, ended, terminalSent, finalReason, sentSamples,
                 totalSamples, crc32);

    char exchangeText[17];
    formatId(job.exchange, exchangeText);
    DEBUG_SYSTEMF("[liveaudio] %s %s exchange=%s sent=%lu dropped=%lu "
                  "crc32=%08lx terminal=%d reason=%s",
                  streamModeName(job.mode), ended ? "end" : "abort",
                  exchangeText, static_cast<unsigned long>(sentSamples),
                  static_cast<unsigned long>(droppedSamples),
                  static_cast<unsigned long>(crc32), terminalSent ? 1 : 0,
                  abortReasonName(finalReason));
  }
}

bool ensureTxTask() {
  if (sTxTask != nullptr) return true;
  TaskHandle_t created = nullptr;
  const BaseType_t result = xTaskCreateLogged(
      liveAudioTxTask, "live_audio_tx", LIVE_AUDIO_TX_STACK_WORDS, nullptr,
      TASK_PRIORITY_LOW, &created, "liveaudio.tx", PRO_CORE);
  if (result != pdPASS) return false;
  sTxTask = created;
  return true;
}

bool ensureShadowQueue() {
  if (sShadowStorage) return true;
  void* allocation = heap_caps_malloc(
      kShadowBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!allocation) {
    portENTER_CRITICAL(&sStateMux);
    ++sShadowStats.allocFailures;
    portEXIT_CRITICAL(&sStateMux);
    return false;
  }
  bool keep = false;
  portENTER_CRITICAL(&sStateMux);
  if (!sShadowStorage) {
    sShadowStorage = static_cast<uint8_t*>(allocation);
    keep = true;
  }
  portEXIT_CRITICAL(&sStateMux);
  if (!keep) heap_caps_free(allocation);
  return sShadowStorage != nullptr;
}

const char* requireMutableTransport() {
  if (!realUartCaller())
    return "Error: liveaudio control requires a real logged-in UART session";
  if (!uartLinkIsRunning()) return "Error: UART link not running";
  if (uartLinkEffectiveBaud() < static_cast<int>(kMinBaud))
    return "Error: UART baud below liveaudio minimum (921600)";
  return nullptr;
}

void countShadowSkip() {
  portENTER_CRITICAL(&sStateMux);
  ++sShadowStats.skips;
  portEXIT_CRITICAL(&sStateMux);
}

}  // namespace

bool liveAudioIsHousekeepingCommand(const char* line) {
  const char* cursor = line;
  TextToken root;
  TextToken verb;
  if (!nextTextToken(cursor, root) ||
      !textTokenEqualsIgnoreCase(root, "liveaudio") ||
      !nextTextToken(cursor, verb)) {
    return false;
  }
  return textTokenEqualsIgnoreCase(verb, "status") ||
         textTokenEqualsIgnoreCase(verb, "capabilities");
}

LiveAudioReadyIntrinsicResult liveAudioHandleReadyIntrinsic(
    const char* line, uint32_t namedSessionEpoch,
    bool namedSessionMayControl, char* reply, size_t replySize) {
  uint64_t controller = 0;
  if (!parseCanonicalReadyLine(line, controller) ||
      !namedSessionMayControl || namedSessionEpoch == 0 || !reply ||
      replySize == 0 || !uartLinkIsRunning() ||
      uartLinkEffectiveBaud() < static_cast<int>(kMinBaud)) {
    return LiveAudioReadyIntrinsicResult::NotHandled;
  }

  // Renewal only.  Never create or resurrect authority here: initial setup,
  // an expired lease, a changed login epoch, or a different controller must
  // retain the ordinary command's CRC/task/lifecycle path.
  bool renewed = false;
  portENTER_CRITICAL(&sStateMux);
  // Sample after acquiring the lease lock. A timestamp captured before a
  // preemption or lock wait could make a lease that expired in the meantime
  // appear current and let this renewal-only path resurrect it.
  const uint32_t now = millis();
  // A valid lease can only be minted by the ordinary ready path after
  // ensureTxTask() succeeds, so the lease itself is the synchronized proof
  // that the persistent transmitter exists. Do not read sTxTask here: its
  // one-time publication belongs to the serialized setup path.
  if (sLease.valid && sLease.controller == controller &&
      sLease.sessionEpoch == namedSessionEpoch &&
      namedSessionEpoch == uartLinkSessionEpoch() &&
      !timeReached(now, sLease.deadlineMs)) {
    sLease.deadlineMs = now + kLeaseTtlMs;
    renewed = true;
  }
  portEXIT_CRITICAL(&sStateMux);
  if (!renewed) return LiveAudioReadyIntrinsicResult::NotHandled;

  char controllerText[17];
  formatId(controller, controllerText);
  snprintf(reply, replySize,
           "OK: liveaudio ready version=1 controller=%s session_epoch=%lu "
           "renew_direct=1 lease_ttl_ms=%lu renew_ms=%lu baud=%d",
           controllerText, static_cast<unsigned long>(namedSessionEpoch),
           static_cast<unsigned long>(kLeaseTtlMs),
           static_cast<unsigned long>(kLeaseRenewMs),
           uartLinkEffectiveBaud());
  return LiveAudioReadyIntrinsicResult::Handled;
}

bool liveAudioStreamActive() {
  portENTER_CRITICAL(&sStateMux);
  const bool active = sStream.active;
  portEXIT_CRITICAL(&sStateMux);
  return active;
}

bool liveAudioTryBeginBulkTransfer() {
  bool claimed = false;
  portENTER_CRITICAL(&sStateMux);
  if (!sStream.active && !sBulkTransferActive) {
    sBulkTransferActive = true;
    claimed = true;
  }
  portEXIT_CRITICAL(&sStateMux);
  return claimed;
}

void liveAudioEndBulkTransfer() {
  portENTER_CRITICAL(&sStateMux);
  sBulkTransferActive = false;
  portEXIT_CRITICAL(&sStateMux);
}

bool liveAudioRecorderArmNative(uint64_t exchangeId, uint32_t sessionEpoch) {
  if (!idStructurallyValid(exchangeId) || sessionEpoch == 0 ||
      sessionEpoch != uartLinkSessionEpoch() ||
      !g2EvenAiExchangeBoundToUartSession(exchangeId, sessionEpoch)) {
    return false;
  }
  const uint32_t now = millis();
  bool armed = false;
  portENTER_CRITICAL(&sStateMux);
  if (sLease.shadowEnabled && sLease.shadowNative &&
      leaseMatchesLocked(sLease.controller, sessionEpoch, now)) {
    sCaptureArm.valid = true;
    sCaptureArm.native = true;
    sCaptureArm.controller = sLease.controller;
    sCaptureArm.exchange = exchangeId;
    sCaptureArm.sessionEpoch = sessionEpoch;
    sCaptureArm.deadlineMs = now + kCaptureArmTtlMs;
    armed = true;
  }
  portEXIT_CRITICAL(&sStateMux);
  return armed;
}

bool liveAudioRecorderCaptureEligible(
    uint64_t exchangeId, LiveAudioRecorderAuthorization* outAuth) {
  if (outAuth) *outAuth = LiveAudioRecorderAuthorization{};
  if (!outAuth || !idStructurallyValid(exchangeId)) return false;
  const AuthContext& ctx = currentAuthContext();
  const uint32_t epoch = uartLinkSessionEpoch();
  const uint32_t now = millis();
  CaptureArm arm;
  LeaseState lease;
  portENTER_CRITICAL(&sStateMux);
  arm = sCaptureArm;
  lease = sLease;
  portEXIT_CRITICAL(&sStateMux);
  if (!arm.valid || arm.exchange != exchangeId || arm.sessionEpoch != epoch ||
      timeReached(now, arm.deadlineMs) || epoch == 0 ||
      !lease.valid || lease.controller != arm.controller ||
      lease.sessionEpoch != epoch || timeReached(now, lease.deadlineMs)) {
    return false;
  }
  const bool provenanceOk = arm.native
      ? (ctx.transport == SOURCE_INTERNAL && ctx.path == "/g2evenai" &&
         g2EvenAiExchangeBoundToUartSession(exchangeId, epoch))
      : (ctx.transport == SOURCE_UART && realUartCaller());
  if (!provenanceOk) return false;

  bool consumed = false;
  portENTER_CRITICAL(&sStateMux);
  if (sCaptureArm.valid && sCaptureArm.exchange == exchangeId &&
      sCaptureArm.controller == arm.controller &&
      sCaptureArm.sessionEpoch == epoch &&
      !timeReached(now, sCaptureArm.deadlineMs) &&
      sLease.shadowEnabled &&
      leaseMatchesLocked(arm.controller, epoch, now)) {
    sCaptureArm = CaptureArm{};
    outAuth->valid = true;
    outAuth->native = arm.native;
    outAuth->controller = arm.controller;
    outAuth->exchange = exchangeId;
    outAuth->sessionEpoch = epoch;
    consumed = true;
  }
  portEXIT_CRITICAL(&sStateMux);
  return consumed;
}

bool liveAudioRecorderBegin(uint64_t exchangeId,
                            LiveAudioRecorderSource source,
                            uint32_t sampleRate,
                            const LiveAudioRecorderAuthorization& auth) {
  if (!auth.valid || !idStructurallyValid(auth.controller) ||
      auth.exchange != exchangeId ||
      auth.sessionEpoch == 0 || !idStructurallyValid(exchangeId) ||
      sampleRate != kSampleRate ||
      (source != LiveAudioRecorderSource::PDM &&
       source != LiveAudioRecorderSource::G2) ||
      !sShadowStorage || sTxTask == nullptr) {
    countShadowSkip();
    return false;
  }
  if (auth.native &&
      !g2EvenAiExchangeBoundToUartSession(exchangeId,
                                           auth.sessionEpoch)) {
    countShadowSkip();
    return false;
  }
  const uint32_t now = millis();
  bool admitted = false;
  portENTER_CRITICAL(&sStateMux);
  if (!sBulkTransferActive && !sStream.active && sLease.shadowEnabled &&
      leaseMatchesLocked(auth.controller, auth.sessionEpoch, now) &&
      uartLinkSessionEpoch() == auth.sessionEpoch) {
    const uint32_t generation =
        sShadowGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    sShadowAccepting.store(false, std::memory_order_release);
    sShadowHead.store(0, std::memory_order_release);
    sShadowTail.store(0, std::memory_order_release);
    sTerminalDecision.store(TERMINAL_OPEN, std::memory_order_release);
    sStream = StreamState{};
    sStream.active = true;
    sStream.mode = STREAM_RECORDER;
    sStream.source = static_cast<uint8_t>(source);
    sStream.controller = auth.controller;
    sStream.exchange = exchangeId;
    sStream.sessionEpoch = auth.sessionEpoch;
    sStream.queueGeneration = generation;
    sStream.startedMs = now;
    ++sShadowStats.starts;
    sShadowAccepting.store(true, std::memory_order_release);
    admitted = true;
  } else {
    ++sShadowStats.skips;
  }
  portEXIT_CRITICAL(&sStateMux);
  if (admitted && auth.native &&
      !g2EvenAiExchangeBoundToUartSession(exchangeId,
                                           auth.sessionEpoch)) {
    sShadowAccepting.store(false, std::memory_order_release);
    portENTER_CRITICAL(&sStateMux);
    if (sStream.active && sStream.mode == STREAM_RECORDER &&
        sStream.exchange == exchangeId &&
        sStream.sessionEpoch == auth.sessionEpoch) {
      requestAbortLocked(ABORT_HOST_REQUEST);
      ++sShadowStats.skips;
    }
    portEXIT_CRITICAL(&sStateMux);
    if (sTxTask) xTaskNotifyGive(sTxTask);
    return false;
  }
  if (admitted) xTaskNotifyGive(sTxTask);
  return admitted;
}

bool liveAudioRecorderOffer(uint64_t exchangeId, const int16_t* samples,
                            size_t sampleCount) {
  if (!samples || sampleCount == 0 || sampleCount > kLogicalChunkSamples)
    return false;
  const size_t bytes = sampleCount * sizeof(int16_t);
  uint32_t generation = 0;
  portENTER_CRITICAL(&sStateMux);
  if (sStream.active && sStream.mode == STREAM_RECORDER &&
      sStream.exchange == exchangeId && !sStream.abortRequested &&
      !sStream.endRequested) {
    generation = sStream.queueGeneration;
  }
  portEXIT_CRITICAL(&sStateMux);
  if (!generation || !sShadowAccepting.load(std::memory_order_acquire) ||
      sShadowGeneration.load(std::memory_order_acquire) != generation) {
    return false;
  }

  const uint32_t head = sShadowHead.load(std::memory_order_relaxed);
  const uint32_t tail = sShadowTail.load(std::memory_order_acquire);
  if (head - tail >= kShadowSlots) {
    portENTER_CRITICAL(&sStateMux);
    if (sStream.active && sStream.mode == STREAM_RECORDER &&
        sStream.exchange == exchangeId &&
        sStream.queueGeneration == generation) {
      sStream.totalSamples += static_cast<uint32_t>(sampleCount);
      requestAbortLocked(ABORT_TX_BACKPRESSURE);
      // Retire this exact generation before dropping the state lock. Keeping
      // the gate change with the matching-stream check prevents a delayed old
      // producer from disabling a successor generation after TX cleanup.
      sShadowAccepting.store(false, std::memory_order_release);
      ++sShadowStats.overflows;
    }
    portEXIT_CRITICAL(&sStateMux);
    if (sTxTask) xTaskNotifyGive(sTxTask);
    return false;
  }

  const uint32_t slot = head % kShadowSlots;
  // The recorder producer performs exactly one bounded copy and never waits.
  memcpy(sShadowStorage + slot * kShadowSlotBytes, samples, bytes);

  bool published = false;
  portENTER_CRITICAL(&sStateMux);
  if (sStream.active && sStream.mode == STREAM_RECORDER &&
      sStream.exchange == exchangeId && !sStream.abortRequested &&
      !sStream.endRequested && sStream.queueGeneration == generation &&
      sShadowAccepting.load(std::memory_order_acquire) &&
      sShadowGeneration.load(std::memory_order_acquire) == generation) {
    sShadowLengths[slot] = static_cast<uint16_t>(bytes);
    sStream.totalSamples += static_cast<uint32_t>(sampleCount);
    sShadowHead.store(head + 1, std::memory_order_release);
    const uint32_t depth = head + 1 - tail;
    if (depth > sShadowStats.highWater) sShadowStats.highWater = depth;
    published = true;
  }
  portEXIT_CRITICAL(&sStateMux);
  if (published && sTxTask) xTaskNotifyGive(sTxTask);
  return published;
}

void liveAudioRecorderFinish(uint64_t exchangeId,
                             LiveAudioRecorderOutcome outcome) {
  bool matched = false;
  portENTER_CRITICAL(&sStateMux);
  if (sStream.active && sStream.mode == STREAM_RECORDER &&
      sStream.exchange == exchangeId) {
    if (outcome == LiveAudioRecorderOutcome::SAVED)
      sStream.endRequested = true;
    else
      requestAbortLocked(outcome == LiveAudioRecorderOutcome::DISCARDED
                             ? ABORT_HOST_REQUEST : ABORT_INTERNAL);
    matched = true;
  }
  portEXIT_CRITICAL(&sStateMux);
  if (matched && sTxTask) xTaskNotifyGive(sTxTask);
}

void liveAudioRecorderAbort(uint64_t exchangeId) {
  if (!exchangeId) return;
  bool matched = false;
  portENTER_CRITICAL(&sStateMux);
  if (sCaptureArm.valid && sCaptureArm.exchange == exchangeId)
    sCaptureArm = CaptureArm{};
  if (sStream.active && sStream.mode == STREAM_RECORDER &&
      sStream.exchange == exchangeId) {
    requestAbortLocked(ABORT_HOST_REQUEST);
    matched = true;
  }
  portEXIT_CRITICAL(&sStateMux);
  if (matched && sTxTask) xTaskNotifyGive(sTxTask);
}

const char* cmd_liveaudio(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs args(argsInput);
  if (args.count() == 1 && args.arg(0) == "capabilities") {
    snprintf(sReply, sizeof(sReply),
             "OK: live-pcm-v1 synthetic=1 recorder_shadow=1 "
             "shadow_default=off protocol=1 frames=0x10/0x11/0x12/0x13 "
             "source=0 recorder_source=1/2 format=1 rate=16000 "
             "renew_direct=1 lease_ttl_ms=%lu lease_renew_ms=%lu "
             "min_baud=921600 "
             "logical_chunk_samples=2048 physical_frame_samples=500 "
             "shadow_slots=4 shadow_bytes=16384 max_duration_ms=60000 "
             "id=hex16 crc32=CBF43926",
             static_cast<unsigned long>(kLeaseTtlMs),
             static_cast<unsigned long>(kLeaseRenewMs));
    return sReply;
  }

  if (args.count() == 1 && args.arg(0) == "status") {
    LeaseState lease;
    CaptureArm arm;
    StreamState stream;
    LastState last;
    ShadowStats stats;
    bool bulk = false;
    const uint32_t now = millis();
    const uint32_t currentSessionEpoch = uartLinkSessionEpoch();
    uint32_t leaseRemaining = 0;
    portENTER_CRITICAL(&sStateMux);
    lease = sLease;
    arm = sCaptureArm;
    stream = sStream;
    last = sLast;
    stats = sShadowStats;
    bulk = sBulkTransferActive;
    leaseRemaining = leaseRemainingLocked(now);
    portEXIT_CRITICAL(&sStateMux);
    const uint32_t qHead = sShadowHead.load(std::memory_order_acquire);
    const uint32_t qTail = sShadowTail.load(std::memory_order_acquire);
    // Reset publishes tail/head separately; a status snapshot that lands
    // between them must not render an unsigned-wrap queue depth.
    const uint32_t qDepth = qHead >= qTail ? qHead - qTail : 0;

    char leaseId[17] = "-";
    char streamController[17] = "-";
    char exchangeId[17] = "-";
    char shadowTarget[17] = "-";
    char armExchange[17] = "-";
    char lastExchange[17] = "-";
    if (lease.valid) formatId(lease.controller, leaseId);
    if (lease.shadowExchange) formatId(lease.shadowExchange, shadowTarget);
    if (arm.valid) formatId(arm.exchange, armExchange);
    if (stream.active) {
      formatId(stream.controller, streamController);
      formatId(stream.exchange, exchangeId);
    }
    if (last.valid) formatId(last.exchange, lastExchange);
    snprintf(sReply, sizeof(sReply),
             "OK: liveaudio task=%s baud=%d session_epoch=%lu lease=%s "
             "lease_epoch=%lu remaining_ms=%lu shadow=%s shadow_mode=%s "
             "shadow_target=%s shadow_arm=%s shadow_psram=%s q_depth=%lu "
             "q_hwm=%lu shadow_starts=%lu shadow_skips=%lu "
             "shadow_overflows=%lu shadow_alloc_failures=%lu bulk=%d "
             "active=%d mode=%s source=%u controller=%s exchange=%s "
             "sent=%lu total=%lu pcm_frames=%lu crc32=%08lx abort=%s "
             "last=%s last_mode=%s last_exchange=%s last_sent=%lu "
             "last_dropped=%lu last_crc32=%08lx last_terminal=%d degraded=%d",
             sTxTask ? "ready" : "dormant", uartLinkEffectiveBaud(),
             static_cast<unsigned long>(currentSessionEpoch), leaseId,
             static_cast<unsigned long>(lease.sessionEpoch),
             static_cast<unsigned long>(leaseRemaining),
             lease.shadowEnabled ? "on" : "off",
             lease.shadowNative ? "native" : "exact", shadowTarget,
             armExchange, sShadowStorage ? "ready" : "unavailable",
             static_cast<unsigned long>(qDepth),
             static_cast<unsigned long>(stats.highWater),
             static_cast<unsigned long>(stats.starts),
             static_cast<unsigned long>(stats.skips),
             static_cast<unsigned long>(stats.overflows),
             static_cast<unsigned long>(stats.allocFailures), bulk ? 1 : 0,
             stream.active ? 1 : 0, streamModeName(stream.mode), stream.source,
             streamController, exchangeId,
             static_cast<unsigned long>(stream.sentSamples),
             static_cast<unsigned long>(stream.totalSamples),
             static_cast<unsigned long>(stream.pcmFrames),
             static_cast<unsigned long>(stream.crc32),
             stream.abortRequested ? abortReasonName(stream.abortReason) : "none",
             !last.valid ? "none" : (last.ended ? "end" : abortReasonName(last.reason)),
             streamModeName(last.mode), lastExchange,
             static_cast<unsigned long>(last.sentSamples),
             static_cast<unsigned long>(last.droppedSamples),
             static_cast<unsigned long>(last.crc32), last.terminalSent ? 1 : 0,
             g2MicCaptureDegraded() ? 1 : 0);
    return sReply;
  }

  if (args.count() >= 1 && args.arg(0) == "ready") {
    if (args.count() != 3 || args.arg(1) != "1")
      return "Error: usage: liveaudio ready 1 <controller_hex16>";
    uint64_t controller = 0;
    if (!parseStrictId(args.arg(2), controller))
      return "Error: controller ID must be 16 hex digits with two nonzero 8-digit halves";
    if (const char* error = requireMutableTransport()) return error;
    if (!crc32SelfTest()) return "Error: liveaudio CRC32 self-test failed";
    if (!ensureTxTask()) return "Error: liveaudio TX task unavailable";

    const uint32_t now = millis();
    const uint32_t sessionEpoch = uartLinkSessionEpoch();
    if (sessionEpoch == 0) return "Error: UART login epoch unavailable";
    bool busy = false;
    portENTER_CRITICAL(&sStateMux);
    const bool unexpired = sLease.valid && !timeReached(now, sLease.deadlineMs);
    const bool current = unexpired && sLease.sessionEpoch == sessionEpoch;
    if (unexpired && !current) {
      if (sStream.active) {
        requestAbortLocked(ABORT_AUTH_LOST);
        busy = true;
        sLease = LeaseState{};
        sCaptureArm = CaptureArm{};
      } else {
        sLease = LeaseState{};
      }
    }
    if (!busy && current && sLease.controller != controller) {
      busy = true;
    } else if (!busy && !current && (sStream.active || sBulkTransferActive)) {
      busy = true;
    } else if (!busy) {
      if (!current) {
        sLease = LeaseState{};
        sLease.valid = true;
        sLease.controller = controller;
        sLease.sessionEpoch = sessionEpoch;
      }
      sLease.deadlineMs = now + kLeaseTtlMs;
    }
    portEXIT_CRITICAL(&sStateMux);
    if (busy) return "Error: liveaudio lease busy";

    char controllerText[17];
    formatId(controller, controllerText);
    snprintf(sReply, sizeof(sReply),
             "OK: liveaudio ready version=1 controller=%s session_epoch=%lu "
             "renew_direct=1 lease_ttl_ms=%lu renew_ms=%lu baud=%d",
             controllerText, static_cast<unsigned long>(sessionEpoch),
             static_cast<unsigned long>(kLeaseTtlMs),
             static_cast<unsigned long>(kLeaseRenewMs),
             uartLinkEffectiveBaud());
    return sReply;
  }

  if (args.count() >= 1 && args.arg(0) == "shadow") {
    if ((args.count() != 4 && args.count() != 5) || args.arg(1) != "1")
      return "Error: usage: liveaudio shadow 1 <controller_hex16> on <exchange_hex16|native> | off";
    uint64_t controller = 0;
    if (!parseStrictId(args.arg(2), controller))
      return "Error: controller ID must be 16 hex digits with two nonzero 8-digit halves";
    if (const char* error = requireMutableTransport()) return error;
    const uint32_t epoch = uartLinkSessionEpoch();
    const uint32_t now = millis();
    const bool turnOn = args.arg(3) == "on";
    const bool turnOff = args.arg(3) == "off";
    if ((!turnOn && !turnOff) || (turnOn && args.count() != 5) ||
        (turnOff && args.count() != 4))
      return "Error: usage: liveaudio shadow 1 <controller_hex16> on <exchange_hex16|native> | off";
    bool native = false;
    uint64_t exchange = 0;
    if (turnOn) {
      native = args.arg(4) == "native";
      if (!native && !parseStrictId(args.arg(4), exchange))
        return "Error: shadow target must be native or a strict 16-hex exchange ID";
      if (!ensureTxTask()) return "Error: liveaudio TX task unavailable";
      if (!ensureShadowQueue())
        return "Error: recorder shadow requires an exact 16384-byte PSRAM queue";
    }

    bool matched = false;
    bool aborted = false;
    portENTER_CRITICAL(&sStateMux);
    if (leaseMatchesLocked(controller, epoch, now)) {
      matched = true;
      sLease.shadowEnabled = turnOn;
      sLease.shadowNative = turnOn && native;
      sLease.shadowExchange = turnOn && !native ? exchange : 0;
      sCaptureArm = CaptureArm{};
      if (turnOn && !native) {
        sCaptureArm.valid = true;
        sCaptureArm.native = false;
        sCaptureArm.controller = controller;
        sCaptureArm.exchange = exchange;
        sCaptureArm.sessionEpoch = epoch;
        sCaptureArm.deadlineMs = now + kCaptureArmTtlMs;
      }
      if (turnOff && sStream.active &&
          sStream.mode == STREAM_RECORDER &&
          sStream.controller == controller) {
        requestAbortLocked(ABORT_RELEASED);
        aborted = true;
      }
    }
    portEXIT_CRITICAL(&sStateMux);
    if (!matched) return "Error: liveaudio lease does not match controller";
    if (aborted && sTxTask) xTaskNotifyGive(sTxTask);
    char controllerText[17];
    formatId(controller, controllerText);
    snprintf(sReply, sizeof(sReply),
             "OK: liveaudio shadow version=1 controller=%s state=%s "
             "mode=%s target=%s abort=%d",
             controllerText, turnOn ? "on" : "off",
             turnOn ? (native ? "native" : "exact") : "none",
             turnOn ? args.arg(4).c_str() : "-", aborted ? 1 : 0);
    return sReply;
  }

  if (args.count() >= 1 && args.arg(0) == "release") {
    if (args.count() != 3 || args.arg(1) != "1")
      return "Error: usage: liveaudio release 1 <controller_hex16>";
    uint64_t controller = 0;
    if (!parseStrictId(args.arg(2), controller))
      return "Error: controller ID must be 16 hex digits with two nonzero 8-digit halves";
    if (const char* error = requireMutableTransport()) return error;
    const uint32_t epoch = uartLinkSessionEpoch();
    bool matched = false;
    bool aborted = false;
    portENTER_CRITICAL(&sStateMux);
    if (sLease.valid && sLease.controller == controller &&
        sLease.sessionEpoch == epoch) {
      matched = true;
      sLease = LeaseState{};
      sCaptureArm = CaptureArm{};
      if (sStream.active && sStream.controller == controller) {
        requestAbortLocked(ABORT_RELEASED);
        aborted = true;
      }
    }
    portEXIT_CRITICAL(&sStateMux);
    if (!matched) return "Error: liveaudio lease does not match controller";
    if (aborted && sTxTask) xTaskNotifyGive(sTxTask);
    char controllerText[17];
    formatId(controller, controllerText);
    snprintf(sReply, sizeof(sReply),
             "OK: liveaudio released version=1 controller=%s abort=%d",
             controllerText, aborted ? 1 : 0);
    return sReply;
  }

  if (args.count() >= 1 && args.arg(0) == "synth") {
    if (args.count() != 5 || args.arg(1) != "1")
      return "Error: usage: liveaudio synth 1 <controller_hex16> <exchange_hex16> <duration_ms>";
    uint64_t controller = 0;
    uint64_t exchange = 0;
    uint32_t durationMs = 0;
    if (!parseStrictId(args.arg(2), controller))
      return "Error: controller ID must be 16 hex digits with two nonzero 8-digit halves";
    if (!parseStrictId(args.arg(3), exchange))
      return "Error: exchange ID must be 16 hex digits with two nonzero 8-digit halves";
    if (!parseDurationMs(args.arg(4), durationMs))
      return "Error: duration_ms must be 1..60000";
    if (const char* error = requireMutableTransport()) return error;
    if (!ensureTxTask()) return "Error: liveaudio TX task unavailable";

    const uint32_t totalSamples = static_cast<uint32_t>(
        (static_cast<uint64_t>(kSampleRate) * durationMs) / 1000u);
    const uint32_t epoch = uartLinkSessionEpoch();
    bool leaseOk = false;
    bool busy = false;
    const uint32_t now = millis();
    portENTER_CRITICAL(&sStateMux);
    leaseOk = leaseMatchesLocked(controller, epoch, now);
    busy = sStream.active || sBulkTransferActive;
    if (leaseOk && !busy) {
      sTerminalDecision.store(TERMINAL_OPEN, std::memory_order_release);
      sStream = StreamState{};
      sStream.active = true;
      sStream.mode = STREAM_SYNTHETIC;
      sStream.flags = kSyntheticFlag;
      sStream.source = kSourceSynthetic;
      sStream.controller = controller;
      sStream.exchange = exchange;
      sStream.sessionEpoch = epoch;
      sStream.durationMs = durationMs;
      sStream.totalSamples = totalSamples;
      sStream.startedMs = now;
    }
    portEXIT_CRITICAL(&sStateMux);
    if (!leaseOk) return "Error: liveaudio lease missing, mismatched, or expired";
    if (busy) return "Error: liveaudio stream or bulk transfer already active";
    xTaskNotifyGive(sTxTask);
    char controllerText[17];
    char exchangeText[17];
    formatId(controller, controllerText);
    formatId(exchange, exchangeText);
    snprintf(sReply, sizeof(sReply),
             "OK: liveaudio synth scheduled version=1 controller=%s "
             "exchange=%s duration_ms=%lu samples=%lu",
             controllerText, exchangeText,
             static_cast<unsigned long>(durationMs),
             static_cast<unsigned long>(totalSamples));
    return sReply;
  }

  if (args.count() >= 1 && args.arg(0) == "abort") {
    if (args.count() != 4 || args.arg(1) != "1")
      return "Error: usage: liveaudio abort 1 <controller_hex16> <exchange_hex16>";
    uint64_t controller = 0;
    uint64_t exchange = 0;
    if (!parseStrictId(args.arg(2), controller))
      return "Error: controller ID must be 16 hex digits with two nonzero 8-digit halves";
    if (!parseStrictId(args.arg(3), exchange))
      return "Error: exchange ID must be 16 hex digits with two nonzero 8-digit halves";
    if (const char* error = requireMutableTransport()) return error;
    const uint32_t epoch = uartLinkSessionEpoch();
    bool matched = false;
    portENTER_CRITICAL(&sStateMux);
    if (sStream.active && sStream.controller == controller &&
        sStream.exchange == exchange && sStream.sessionEpoch == epoch &&
        epoch != 0) {
      requestAbortLocked(ABORT_HOST_REQUEST);
      matched = true;
    }
    portEXIT_CRITICAL(&sStateMux);
    if (!matched) return "Error: liveaudio active stream does not match IDs";
    if (sTxTask) xTaskNotifyGive(sTxTask);
    char exchangeText[17];
    formatId(exchange, exchangeText);
    snprintf(sReply, sizeof(sReply),
             "OK: liveaudio abort requested version=1 exchange=%s",
             exchangeText);
    return sReply;
  }

  return "Error: usage: liveaudio <capabilities|status|ready 1 <controller_hex16>|shadow 1 <controller_hex16> on <exchange_hex16|native>|shadow 1 <controller_hex16> off|release 1 <controller_hex16>|synth 1 <controller_hex16> <exchange_hex16> <duration_ms>|abort 1 <controller_hex16> <exchange_hex16>>";
}

const CommandEntry liveAudioCommands[] = {
    {"liveaudio", "Opt-in live PCM transport, lease, and shadow diagnostics",
     false, cmd_liveaudio,
     "Usage: liveaudio <capabilities|status|ready 1 <controller_hex16>|shadow 1 <controller_hex16> on <exchange_hex16|native>|shadow 1 <controller_hex16> off|release 1 <controller_hex16>|synth 1 <controller_hex16> <exchange_hex16> <duration_ms>|abort 1 <controller_hex16> <exchange_hex16>>"},
};

const size_t liveAudioCommandsCount =
    sizeof(liveAudioCommands) / sizeof(liveAudioCommands[0]);
