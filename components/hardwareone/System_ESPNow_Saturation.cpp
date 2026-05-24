// ESP-NOW Link Saturation — implementation. See header for design rationale.

#include "System_ESPNow_Saturation.h"

#if ENABLE_ESPNOW

#include "System_ESPNow.h"
#include "System_ESPNow_Sessions.h"  // pendingFrameCount()
#include "System_Debug.h"
#include <Arduino.h>
#include <string.h>

namespace {

constexpr uint8_t kWindowSamples = 30;  // 30 seconds at 1 Hz

struct Sample {
  uint32_t ts;                  // millis() at snapshot
  uint32_t messagesSent;        // cumulative
  uint32_t messagesReceived;    // cumulative
  uint32_t messagesFailed;      // cumulative
  uint32_t streamReceivedCount; // cumulative (RX path observations)
  uint32_t streamDroppedCount;  // cumulative (ring overflow drops — the smoking gun)
  uint16_t streamQDepth;        // instantaneous depth at sample time (0..streamQueueCap)
  uint16_t pendingFrames;       // instantaneous pendingFrameCount()
  uint32_t ackRttSumMs;         // sum of ACK RTTs observed in this 1s bucket
  uint16_t ackRttCount;         // count of ACK RTTs in this bucket
  uint16_t ackRttMaxMs;         // max ACK RTT in this bucket
};

Sample      gRing[kWindowSamples] = {};
uint8_t     gRingHead              = 0;   // next slot to write
uint8_t     gRingFill              = 0;   // 0..kWindowSamples
uint32_t    gLastTickMs            = 0;

// Accumulators for the in-progress 1-second bucket (committed on tick).
uint32_t    gAckRttSumMs           = 0;
uint16_t    gAckRttCount           = 0;
uint16_t    gAckRttMaxMs           = 0;

inline uint16_t computeStreamQDepth() {
  if (!gEspNow || !gEspNow->streamQueue || gEspNow->streamQueueCap == 0) return 0;
  int head = gEspNow->streamQueueHead;
  int tail = gEspNow->streamQueueTail;
  int depth = (head - tail) & gEspNow->streamQueueMask;
  return (uint16_t)depth;
}

}  // namespace

void espnowSaturationNoteAckRtt(uint32_t rttMs) {
  // Single-task accumulators — only touched from contexts where the broadcast
  // tracker sweeps (periodic tick). No mutex needed.
  gAckRttSumMs += rttMs;
  if (gAckRttCount < UINT16_MAX) gAckRttCount++;
  if (rttMs > gAckRttMaxMs && rttMs <= UINT16_MAX) gAckRttMaxMs = (uint16_t)rttMs;
}

void espnowSaturationTick() {
  uint32_t now = (uint32_t)millis();
  if (gLastTickMs != 0 && (now - gLastTickMs) < 1000) return;
  gLastTickMs = now;
  if (!gEspNow) return;

  Sample& s = gRing[gRingHead];
  s.ts                  = now;
  s.messagesSent        = gEspNow->routerMetrics.messagesSent;
  s.messagesReceived    = gEspNow->routerMetrics.messagesReceived;
  s.messagesFailed      = gEspNow->routerMetrics.messagesFailed;
  s.streamReceivedCount = gEspNow->streamReceivedCount;
  s.streamDroppedCount  = gEspNow->streamDroppedCount;
  s.streamQDepth        = computeStreamQDepth();
  s.pendingFrames       = (uint16_t)pendingFrameCount();
  s.ackRttSumMs         = gAckRttSumMs;
  s.ackRttCount         = gAckRttCount;
  s.ackRttMaxMs         = gAckRttMaxMs;

  // Roll bucket
  gAckRttSumMs  = 0;
  gAckRttCount  = 0;
  gAckRttMaxMs  = 0;
  gRingHead     = (gRingHead + 1) % kWindowSamples;
  if (gRingFill < kWindowSamples) gRingFill++;
}

void espnowSaturationReset() {
  memset(gRing, 0, sizeof(gRing));
  gRingHead = gRingFill = 0;
  gLastTickMs = 0;
  gAckRttSumMs = 0;
  gAckRttCount = 0;
  gAckRttMaxMs = 0;
}

void espnowSaturationReport() {
  if (!gEspNow) {
    BROADCAST_PRINTF("ESP-NOW not initialized");
    return;
  }
  if (gRingFill < 2) {
    BROADCAST_PRINTF("ESP-NOW Saturation: not enough samples yet (need ≥2s; have %us)", gRingFill);
    return;
  }

  // Oldest sample in window = the slot just past head when full, or slot 0 when
  // not yet full.
  uint8_t oldestIdx = (gRingFill == kWindowSamples)
                        ? gRingHead
                        : 0;
  uint8_t newestIdx = (gRingHead == 0) ? (kWindowSamples - 1) : (gRingHead - 1);
  const Sample& oldest = gRing[oldestIdx];
  const Sample& newest = gRing[newestIdx];

  uint32_t windowMs = newest.ts - oldest.ts;
  if (windowMs == 0) windowMs = 1;  // avoid div0 on rapid back-to-back calls
  float windowSec = (float)windowMs / 1000.0f;

  uint32_t dSent = newest.messagesSent - oldest.messagesSent;
  uint32_t dRecv = newest.messagesReceived - oldest.messagesReceived;
  uint32_t dFail = newest.messagesFailed - oldest.messagesFailed;
  uint32_t dDrop = newest.streamDroppedCount - oldest.streamDroppedCount;

  float fpsSent = dSent / windowSec;
  float fpsRecv = dRecv / windowSec;
  float failPct = (dSent > 0) ? (100.0f * dFail / (float)dSent) : 0.0f;

  // Walk the ring to find peaks + collect ACK RTT
  uint16_t qPeak = 0;
  uint16_t pendPeak = 0;
  uint32_t fpsRxPeakWindow = 0;  // peak per-second RX rate observed
  uint32_t fpsTxPeakWindow = 0;
  uint32_t ackSum = 0;
  uint32_t ackCnt = 0;
  uint16_t ackMax = 0;
  for (uint8_t i = 0; i < gRingFill; i++) {
    uint8_t idx = (oldestIdx + i) % kWindowSamples;
    const Sample& s = gRing[idx];
    if (s.streamQDepth > qPeak)    qPeak    = s.streamQDepth;
    if (s.pendingFrames > pendPeak) pendPeak = s.pendingFrames;
    ackSum += s.ackRttSumMs;
    ackCnt += s.ackRttCount;
    if (s.ackRttMaxMs > ackMax)   ackMax   = s.ackRttMaxMs;
    // Per-second deltas (compare to previous slot)
    if (i > 0) {
      uint8_t prevIdx = (oldestIdx + i - 1) % kWindowSamples;
      const Sample& p = gRing[prevIdx];
      uint32_t dSec_tx = s.messagesSent     - p.messagesSent;
      uint32_t dSec_rx = s.messagesReceived - p.messagesReceived;
      if (dSec_tx > fpsTxPeakWindow) fpsTxPeakWindow = dSec_tx;
      if (dSec_rx > fpsRxPeakWindow) fpsRxPeakWindow = dSec_rx;
    }
  }

  uint16_t qCap   = (uint16_t)(gEspNow->streamQueueCap ? gEspNow->streamQueueCap : 1);
  uint16_t qNow   = computeStreamQDepth();
  uint16_t pendNow= (uint16_t)pendingFrameCount();
  float    qNowPct  = 100.0f * qNow  / qCap;
  float    qPeakPct = 100.0f * qPeak / qCap;
  float    ackAvg   = ackCnt ? (float)ackSum / ackCnt : 0.0f;

  BROADCAST_PRINTF("ESP-NOW Saturation (window=%us, %u samples):", (unsigned)(windowMs/1000), gRingFill);
  BROADCAST_PRINTF("  Frames TX: %.1f fps avg, peak %lu fps (%lu in window)",
                   fpsSent, (unsigned long)fpsTxPeakWindow, (unsigned long)dSent);
  BROADCAST_PRINTF("  Frames RX: %.1f fps avg, peak %lu fps (%lu in window)",
                   fpsRecv, (unsigned long)fpsRxPeakWindow, (unsigned long)dRecv);
  BROADCAST_PRINTF("  Send fail: %lu/%lu (%.2f%%) in window",
                   (unsigned long)dFail, (unsigned long)dSent, failPct);
  BROADCAST_PRINTF("  StreamQ:   %u/%u (%.0f%%) now, peak %u/%u (%.0f%%)",
                   qNow, qCap, qNowPct, qPeak, qCap, qPeakPct);
  if (dDrop > 0) {
    BROADCAST_PRINTF("  StreamDrop: %lu in window  *** SATURATED — ring overflowed ***",
                     (unsigned long)dDrop);
  } else {
    BROADCAST_PRINTF("  StreamDrop: 0 in window");
  }
  BROADCAST_PRINTF("  PendFrames: %u now, peak %u (cap is small — encryption backlog)",
                   pendNow, pendPeak);
  if (ackCnt > 0) {
    BROADCAST_PRINTF("  ACK RTT:   %lu samples, avg %.0fms, max %ums",
                     (unsigned long)ackCnt, ackAvg, ackMax);
  } else {
    BROADCAST_PRINTF("  ACK RTT:   (no broadcast RTTs sampled in window)");
  }
}

#endif  // ENABLE_ESPNOW
