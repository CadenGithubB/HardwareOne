#pragma once
#include <assert.h>
#include <stdint.h>
#include <vector>

// The production fragmenter is compiled immediately after these boundaries.
struct G2NoteContext; // Opaque in Terminal/reader tests; native Notes has its own suite.
using SemaphoreHandle_t = int;
struct G2Temple {
  char side = 'R';
  uint32_t connectionGeneration = 52;
  SemaphoreHandle_t writeMutex = 1;
  bool connected = true;
};
static constexpr uint32_t G2_CENTRAL_TX_ENVELOPE_MS = 1500;
static constexpr uint32_t G2_SEND_ENVELOPE_MUTEX_MS = 1500;
static volatile uint32_t gFragNextBurstDelayMs = 0;
static constexpr uint32_t kTestLifecycle = 9;
static bool testLifecycleOpen = true, testCentralHeld = false, testPeerHeld = false;
static uint32_t testLifecycle = kTestLifecycle;
static constexpr uint32_t kTestPresentation = 71;
static uint32_t testPresentation = kTestPresentation;
static constexpr uint32_t kTestNativeTerminalEpoch = 73;
static uint32_t testNativeTerminalEpoch = kTestNativeTerminalEpoch;
static unsigned testDelayCount = 0, testWriteAttempts = 0;
enum class WireFault { None, DisconnectAfterFirst, TerminalAfterFirst,
                       PresentationAfterFirst, NativeTerminalAfterFirst, FailSecondWrite };
static WireFault testFault = WireFault::None;
static G2Temple* testArm;
static std::vector<std::vector<uint8_t>> testFrames;

#define DEBUG_G2F(...) ((void)0)
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
static bool g2TempleReadyAtGeneration(const G2Temple& arm, uint32_t generation) {
  return arm.connected && (!generation || generation == arm.connectionGeneration);
}
static bool g2NativeTerminalGuardMatches(uint32_t epoch, const G2Temple& arm) {
  return epoch && epoch == testNativeTerminalEpoch && g2TempleReadyAtGeneration(arm, 52);
}
static bool g2LensTxFenceCurrent(const G2Temple& arm, uint32_t generation, uint32_t life) {
  return testLifecycleOpen && life == testLifecycle &&
      g2TempleReadyAtGeneration(arm, generation);
}
static bool g2CreateWireCurrent(uint32_t, const G2Temple&) { return true; }
static uint32_t g2PresentationEpochCurrent() { return testPresentation; }
static bool bleCentralTxTake(uint32_t) {
  assert(!testCentralHeld && !testPeerHeld);
  testCentralHeld = true;
  return true;
}
static void bleCentralTxGive() {
  assert(testCentralHeld && !testPeerHeld);
  testCentralHeld = false;
}
static int xSemaphoreTake(SemaphoreHandle_t, uint32_t) {
  assert(testCentralHeld && !testPeerHeld);
  testPeerHeld = true;
  return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t) {
  assert(testCentralHeld && testPeerHeld);
  testPeerHeld = false;
}
static void g2RingTryDrainPendingTx() { assert(!testCentralHeld && !testPeerHeld); }
static void vTaskDelay(uint32_t) {
  assert(!testCentralHeld && !testPeerHeld);
  ++testDelayCount;
  if (testFrames.size() == 1) {
    if (testFault == WireFault::DisconnectAfterFirst) ++testArm->connectionGeneration;
    if (testFault == WireFault::TerminalAfterFirst) testLifecycleOpen = false;
    if (testFault == WireFault::PresentationAfterFirst) ++testPresentation;
    if (testFault == WireFault::NativeTerminalAfterFirst) testNativeTerminalEpoch = 0;
  }
}
static bool sendEnvelopeNoMutex(G2Temple& arm, const uint8_t* bytes, size_t length,
    uint64_t = 0, bool* attempted = nullptr, uint32_t = 0, uint32_t = 0,
    uint32_t exactGeneration = 0, uint32_t exactLifecycle = 0,
    uint32_t exactPresentation = 0, uint32_t nativeTerminalEpoch = 0,
    const G2NoteContext* noteContext = nullptr) {
  assert(!noteContext);
  if (nativeTerminalEpoch && !g2NativeTerminalGuardMatches(nativeTerminalEpoch, arm)) return false;
  assert(testCentralHeld && testPeerHeld);
  assert(exactGeneration == 52 && exactLifecycle == kTestLifecycle);
  assert(exactPresentation == kTestPresentation && exactPresentation == testPresentation);
  assert(g2LensTxFenceCurrent(arm, exactGeneration, exactLifecycle));
  ++testWriteAttempts;
  if (testFault == WireFault::FailSecondWrite && testWriteAttempts == 2) {
    if (attempted) *attempted = false;
    return false;
  }
  assert(length && length <= 241); // Qualified MTU 244, minus ATT overhead.
  testFrames.emplace_back(bytes, bytes + length);
  if (attempted) *attempted = true;
  return true;
}
