// The production close must not retire a client an active setup still owns.
#include "BLE_SetupWorkerCore.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
#define DEBUG_RING_LIFECYCLEF(...) ((void)0)
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
using BleSetupClient = hw1setup::Client;
static uint8_t busyMask;
static bool open, writeAvailable, retireSucceeds;
static unsigned disconnects, retires, releases;
static int nativeClient;
static struct {
  int* client = &nativeClient;
  void* writeMutex = nullptr;
  bool clientStale = true;
} gRing;
static bool g2RingIsOpen() { return open; }
static void g2RingDisconnect(bool userInitiated) {
  assert(userInitiated);
  ++disconnects;
}
static bool bleSetupIdle(uint8_t mask) { return !(busyMask & mask); }
#if ENABLE_BLE_PHONE_PIPE
static int bleSetupWithdraw(BleSetupClient, unsigned) { return 0; }
#endif
static int xSemaphoreTake(void*, unsigned) { return writeAvailable; }
static void xSemaphoreGive(void*) {}
static constexpr unsigned BLE_CLIENT_RETIRE_R1 = 1, BLE_PEER_R1_RING = 1;
static bool bleRetireClientForReplacement(int*& client, unsigned, unsigned,
                                          const char*) {
  ++retires;
  if (retireSucceeds) client = nullptr;
  return retireSucceeds;
}
static bool g2RingOwnerRelease(unsigned) { ++releases; open = false; return true; }
// INSERT_CLOSE
int main() {
  for (unsigned mask = 0; mask <= hw1setup::kAll; ++mask) {
    busyMask = mask;
    open = writeAvailable = retireSucceeds = true;
    disconnects = retires = releases = 0;
    gRing.client = &nativeClient;
    gRing.clientStale = true;
    const bool result = g2RingClose(true);
    const bool ringBusy = mask & (hw1setup::clientMask(BleSetupClient::Ring) |
                                hw1setup::clientMask(BleSetupClient::RingRetire));
    assert(disconnects == 1 && result == !ringBusy);
    assert(retires == (ringBusy ? 0u : 1u));
    assert(releases == (ringBusy ? 0u : 1u));
    assert(gRing.client == (ringBusy ? &nativeClient : nullptr));
    assert(open == ringBusy);
  }
  // Once a cancelled running turn finishes, the same close can safely retry.
  busyMask = hw1setup::clientMask(BleSetupClient::Ring);
  open = true; gRing.client = &nativeClient;
  assert(!g2RingClose(true) && gRing.client == &nativeClient);
  busyMask = 0;
  assert(g2RingClose(true) && !gRing.client);
  // A writer or outstanding native route still prevents owner release.
  for (bool busyWriter : {false, true}) {
    open = true; gRing.client = &nativeClient;
    gRing.writeMutex = &nativeClient;
    writeAvailable = !busyWriter;
    retireSucceeds = false;
    const unsigned before = releases;
    assert(!g2RingClose(true) && open && releases == before);
  }
  std::puts("ring close: active ring setup/retirement keeps its client until finish; other peers do not block close");
}
