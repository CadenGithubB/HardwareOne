// g2RingYieldToPhonePipe and g2RingRetireForPhonePipe (G2_Ring.cpp) against
// the real BLE_CentralCoordinator.cpp, compiled with the phone pipe in and out
// (ENABLE_BLE_PHONE_PIPE). The main loop's yield only queues the retirement on
// the setup worker, and only with a ring client to retire; the retirement
// itself never frees an active writer and keeps the link reserved until the
// native client is really gone. test_ring_phone_yield.py inserts the code.
#include "BLE_CentralCoordinator.h"
#include <freertos/FreeRTOS.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int gChecks = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    ++gChecks;                                                                \
    if (!(x)) {                                                               \
      std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x);       \
      std::exit(1);                                                           \
    }                                                                         \
  } while (0)

static uint8_t limit = 3;
uint8_t radioHalCompanionBleLinks() { return limit; }
struct Client {};
static Client left, right, ring, phone;
static struct { Client* client = &ring; void* writeMutex = nullptr; bool clientStale = false; } gRing;
static unsigned disconnects = 0, retirements = 0, scansCancelled = 0, submits = 0;
static bool retireComplete = true, writeAvailable = true, g2Client = true;
// Without the G2 client (standalone phone pipe) a ring client is left over
// from a host the teardown already freed: never retire it.
[[maybe_unused]] static bool isG2ClientInitialized() { return g2Client; }
// The shared client stack (BLE_CentralStack.cpp) follows the client flag; the ring owner is open.
[[maybe_unused]] static bool bleStackReady() { return g2Client; }
[[maybe_unused]] static bool g2RingIsOpen() { return true; }
constexpr unsigned BLE_CLIENT_RETIRE_R1 = 2, BLE_PEER_R1_RING = 1;
[[maybe_unused]] static void g2CancelActiveRingScan() { ++scansCancelled; }
static void g2RingDisconnect(bool user) { CHECK(!user); ++disconnects; gRing.clientStale = true; }
static int xSemaphoreTake(void*, uint32_t wait) { CHECK(wait == 50); return writeAvailable ? pdTRUE : 0; }
static void xSemaphoreGive(void*) {}
static bool bleRetireClientForReplacement(Client*& client, unsigned slot, uint8_t peer, const char* why) {
  CHECK(slot == BLE_CLIENT_RETIRE_R1 && peer == BLE_PEER_R1_RING && !std::strcmp(why, "RING-YIELD"));
  ++retirements;
  if (retireComplete) bleCentralLinkReleaseClient(client);
  client = nullptr;  // an incomplete retirement hands the client to the quarantine owner
  return retireComplete;
}
#if ENABLE_BLE_PHONE_PIPE
static bool submitAccepted = true;
static bool g2SubmitRingRetire() { ++submits; return submitAccepted; }
#endif

// INSERT_PRODUCTION

static void reserve(BleCentralLink link, Client* client) {
  BleCentralLinkGuard guard(link);
  CHECK(guard);
  guard.retain(client);
}

int main() {
  // Compiled-out pipe and compiled-in-but-stopped pipe both keep three links
  // and leave the ring alone.
  reserve(BleCentralLink::Left, &left);
  reserve(BleCentralLink::Right, &right);
  reserve(BleCentralLink::Ring, &ring);
  for (int i = 0; i < 10; ++i) g2RingYieldToPhonePipe();
  CHECK(!disconnects && !retirements && !scansCancelled && !submits);
  CHECK(g2RingRetireForPhonePipe() && !disconnects && gRing.client == &ring);  // nothing to give way to
  CHECK(!bleCentralLinkReserve(BleCentralLink::Phone));
  bleCentralPhoneEnabled(true);
  CHECK(bleCentralRingShouldYield());
#if ENABLE_BLE_PHONE_PIPE
  // Yielding with a client: the main loop queues the retirement (and cancels a
  // ring scan begun before the pipe started); it never retires inline.
  // With no G2 client the leftover client is never touched (the host it
  // belonged to is gone), but a scan begun before is still cancelled.
  g2Client = false;
  g2RingYieldToPhonePipe();
  CHECK(submits == 0 && scansCancelled == 1 && !disconnects && !retirements);
  g2Client = true;
  g2RingYieldToPhonePipe();
  CHECK(submits == 1 && scansCancelled == 2 && !disconnects && !retirements);
  // A refused submit (already queued) is asked again on the next pass.
  submitAccepted = false;
  g2RingYieldToPhonePipe();
  CHECK(submits == 2 && scansCancelled == 3);
  submitAccepted = true;
  // The retirement, on the worker: an existing writer cannot be freed to make
  // room; the next pass retries.
  gRing.writeMutex = &ring;
  writeAvailable = false;
  CHECK(!g2RingRetireForPhonePipe());
  CHECK(disconnects == 1 && retirements == 0 && gRing.client == &ring);
  CHECK(!bleCentralLinkReserve(BleCentralLink::Phone));
  // An incomplete retirement hands the client over but keeps the link.
  writeAvailable = true;
  retireComplete = false;
  CHECK(!g2RingRetireForPhonePipe());
  CHECK(disconnects == 2 && retirements == 1 && !gRing.client);
  // With no client left, later passes queue nothing (only the scan cancel).
  {
    const unsigned cancelled = scansCancelled, queued = submits;
    for (int i = 0; i < 100; ++i) g2RingYieldToPhonePipe();
    CHECK(submits == queued && scansCancelled == cancelled + 100);
    CHECK(g2RingRetireForPhonePipe() && disconnects == 2 && retirements == 1);
  }
  CHECK(!bleCentralLinkReserve(BleCentralLink::Phone));  // a late UNREG still owns the capacity
  bleCentralLinkReleaseClient(&ring);                     // verified native retirement completion
  reserve(BleCentralLink::Phone, &phone);
  CHECK(!bleCentralLinkReserve(BleCentralLink::Ring));
  // A complete retirement frees the link at once and clears the stale mark.
  bleCentralLinkReleaseClient(&phone);
  bleCentralPhoneEnabled(false);  // a yielding ring can reserve nothing
  reserve(BleCentralLink::Ring, &ring);
  bleCentralPhoneEnabled(true);
  gRing.client = &ring;
  gRing.clientStale = false;
  retireComplete = true;
  CHECK(g2RingRetireForPhonePipe());
  CHECK(disconnects == 3 && retirements == 2 && !gRing.client && !gRing.clientStale);
  reserve(BleCentralLink::Phone, &phone);
  bleCentralLinkReleaseClient(&phone);
  // The pipe stops: the ring is left alone again.
  bleCentralPhoneEnabled(false);
  reserve(BleCentralLink::Ring, &ring);
  gRing.client = &ring;
  {
    const unsigned queued = submits, cancelled = scansCancelled;
    g2RingYieldToPhonePipe();
    CHECK(submits == queued && scansCancelled == cancelled);
    // A four-link radio keeps the ring when the pipe starts.
    limit = 4;
    bleCentralPhoneEnabled(true);
    CHECK(!bleCentralRingShouldYield());
    g2RingYieldToPhonePipe();
    CHECK(submits == queued && scansCancelled == cancelled && disconnects == 3);
    CHECK(g2RingRetireForPhonePipe() && disconnects == 3);
    reserve(BleCentralLink::Phone, &phone);
  }
#else
  // Without the pipe compiled in, nothing is ever queued for it.
  for (int i = 0; i < 10; ++i) g2RingYieldToPhonePipe();
  CHECK(!disconnects && !retirements && scansCancelled == 10);
  (void)phone;
#endif
  std::printf("Ring yield: pipe compiled=%d, %d checks; the yield only queues a retirement, with a client\n",
              ENABLE_BLE_PHONE_PIPE, gChecks);
  return 0;
}
