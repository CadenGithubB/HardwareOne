// Production packet writer with disconnects at lock and shared-state reads.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <initializer_list>
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
static bool sLinkOnline, centralAvailable, writeAvailable, sendSucceeds;
static bool disconnectAtWriteLock, disconnectAfterLastCheck;
static bool centralHeld, writeHeld;
static unsigned staleReads, sends, centralTakes, writeTakes;
static void disconnectCallback();
struct SharedFlag {
  bool value = false;
  SharedFlag& operator=(bool next) { value = next; return *this; }
  operator bool() const {
    const bool observed = value;
    if (disconnectAfterLastCheck && ++staleReads == 2) disconnectCallback();
    return observed;
  }
};
struct BLERemoteCharacteristic {
  bool connected = true;
  bool writeValue(uint8_t* bytes, size_t len, bool response) {
    assert(centralHeld && writeHeld && bytes && len == 3 && !response);
    ++sends;
    return connected && sendSucceeds;
  }
};
static BLERemoteCharacteristic characteristic;
static struct {
  bool connected = false;
  SharedFlag clientStale;
  BLERemoteCharacteristic* writeChar = nullptr;
  void* writeMutex = nullptr;
  unsigned packetsSent = 0;
} gRing;
static void disconnectCallback() {
  // The callback invalidates published handles. The write mutex still keeps
  // the client's characteristic storage alive until the owner returns.
  sLinkOnline = false;
  gRing.connected = false;
  gRing.clientStale = true;
  gRing.writeChar = nullptr;
  characteristic.connected = false;
}
static constexpr unsigned RING_CENTRAL_TX_MS = 100, RING_WRITE_MUTEX_MS = 100;
static bool bleCentralTxTake(unsigned) {
  ++centralTakes;
  assert(!centralHeld);
  centralHeld = centralAvailable;
  return centralAvailable;
}
static void bleCentralTxGive() { assert(centralHeld && !writeHeld); centralHeld = false; }
static int xSemaphoreTake(void*, unsigned) {
  assert(centralHeld && !writeHeld);
  ++writeTakes;
  writeHeld = writeAvailable;
  if (disconnectAtWriteLock) disconnectCallback();
  return writeAvailable;
}
static void xSemaphoreGive(void*) { assert(writeHeld); writeHeld = false; }
// INSERT_WRITE
static uint8_t payload[] = {1, 2, 3};
static void reset() {
  sLinkOnline = centralAvailable = writeAvailable = sendSucceeds = true;
  disconnectAtWriteLock = disconnectAfterLastCheck = false;
  centralHeld = writeHeld = false;
  staleReads = sends = centralTakes = writeTakes = 0;
  characteristic.connected = true;
  gRing = {};
  gRing.connected = true;
  gRing.writeChar = &characteristic;
  gRing.writeMutex = &characteristic;
}
static RingWriteResult send() {
  const RingWriteResult result = ringOwnerWrite(payload, sizeof(payload));
  assert(!centralHeld && !writeHeld);
  return result;
}
int main() {
  reset(); assert(send() == RING_WRITE_OK && sends == 1 && gRing.packetsSent == 1);
  reset(); sendSucceeds = false;
  assert(send() == RING_WRITE_FAILED && sends == 1 && gRing.packetsSent == 0);
  for (unsigned reason = 0; reason < 6; ++reason) {
    reset();
    if (reason == 0) sLinkOnline = false;
    if (reason == 1) gRing.connected = false;
    if (reason == 2) gRing.writeChar = nullptr;
    if (reason == 3) gRing.writeMutex = nullptr;
    if (reason == 4) gRing.clientStale = true;
    const RingWriteResult result = reason == 5 ? ringOwnerWrite(nullptr, 3) : send();
    assert(result == RING_WRITE_DISCONNECTED && !centralTakes && !writeTakes && !sends);
  }
  reset(); assert(ringOwnerWrite(payload, 0) == RING_WRITE_DISCONNECTED && !centralTakes);
  reset(); centralAvailable = false;
  assert(send() == RING_WRITE_BUSY && centralTakes == 1 && !writeTakes && !sends);
  reset(); writeAvailable = false;
  assert(send() == RING_WRITE_BUSY && centralTakes == 1 && writeTakes == 1 && !sends);
  reset(); disconnectAtWriteLock = true;
  assert(send() == RING_WRITE_DISCONNECTED && !sends && !gRing.packetsSent);
  reset(); disconnectAfterLastCheck = true;
  assert(send() == RING_WRITE_FAILED && sends == 1 && !gRing.packetsSent);
  assert(!gRing.connected && !gRing.writeChar && !characteristic.connected);
  std::puts("ring owner write: disconnect at lock/check boundaries, retained handle, balanced locks and success counters");
}
