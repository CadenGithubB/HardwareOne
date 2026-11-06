// Execute production discovery and transport admission over a scripted GATT
// boundary. Characteristic objects remain cached through disconnect; only the
// callback-visible handles and request generation change.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define ERROR_RINGF(...) ((void)0)
#define DEBUG_RING_LIFECYCLEF(...) ((void)0)
#define G2RING_SERVICE_UUID "service"
#define G2RING_CHAR_WRITE_UUID "write"
#define G2RING_CHAR_NOTIFY_UUID "notify"
#define G2RING_CHAR_CCCD_UUID "cccd"
#define ESP_GATT_AUTH_REQ_NO_MITM 0
struct BLEUUID {
  const char* text;
  BLEUUID(const char* value) : text(value) {}
  BLEUUID(uint16_t) : text("cccd") {}
};
static bool haveService, haveWrite, haveNotify, notifyCapable;
static bool notifySucceeds, telemetrySucceeds, completionAvailable, current;
static bool disconnectAtDescriptor, disconnectAtRegister, disconnectAtTelemetry;
static unsigned serviceCalls, characteristicCalls, disconnectCalls, clockResets;
static unsigned descriptorCalls, registrationCalls, telemetryCalls;
static bool transportOnline;
static const char* failureStage;
static uint32_t millis() { return 100; }
static void simulateDisconnect();
static void ringNoteConnectFailure(const char* stage, uint32_t) { failureStage = stage; }
struct BLERemoteDescriptor { void setAuth(int) {} };
static BLERemoteDescriptor descriptor;
struct BLERemoteNotifyResult {
  bool success;
  int apiStatus = 0, eventStatus = 0, descriptorApiStatus = 0, descriptorEventStatus = 0;
  bool timedOut = false;
};
static void ringNotifyThunk() {}
struct BLERemoteCharacteristic {
  bool canNotify() { return notifyCapable; }
  BLERemoteDescriptor* getDescriptor(BLEUUID) {
    ++descriptorCalls;
    if (disconnectAtDescriptor) simulateDisconnect();
    return &descriptor;
  }
  BLERemoteNotifyResult registerForNotify(void (*)()) {
    ++registrationCalls;
    if (disconnectAtRegister) simulateDisconnect();
    // Both an error and a success delivered just before disconnect are legal.
    return {notifySucceeds};
  }
};
static BLERemoteCharacteristic writeHandle, notifyHandle;
struct BLERemoteService {
  BLERemoteCharacteristic* getCharacteristic(BLEUUID uuid) {
    ++characteristicCalls;
    return !std::strcmp(uuid.text, "write")
        ? (haveWrite ? &writeHandle : nullptr)
        : (haveNotify ? &notifyHandle : nullptr);
  }
};
static BLERemoteService service;
struct Client {
  bool connected = true;
  BLERemoteService* getService(BLEUUID) {
    ++serviceCalls;
    return haveService ? &service : nullptr;
  }
  void disconnect() { ++disconnectCalls; connected = false; }
  bool isConnected() { return connected; }
  // No getServices/getCharacteristics: failed lookup never rediscovers.
};
static Client client;
static struct {
  bool connected = false, clientStale = false;
  BLERemoteCharacteristic* writeChar = nullptr;
  BLERemoteCharacteristic* notifyChar = nullptr;
  Client* client = nullptr;
  uint32_t connectedSince = 0;
} gRing;
static void simulateDisconnect() {
  // The current-client callback invalidates this request and its handles.
  current = false;
  transportOnline = false;
  client.connected = false;
  gRing.connected = false;
  gRing.writeChar = gRing.notifyChar = nullptr;
  gRing.clientStale = true;
}
static void ringTransportDisconnected() { transportOnline = false; }
static void ringClockCustodyReset() { ++clockResets; }
// INSERT_CLEAR_GATT
struct BlePeerConnectRequest {};
struct RingCompletionGuard { explicit operator bool() const { return completionAvailable; } };
static bool ringRequestCurrent(const BlePeerConnectRequest*, uint32_t) { return current; }
static bool ringAbortSupersededRequest(const BlePeerConnectRequest*, uint32_t, const char*) {
  if (current) return false;
  client.disconnect();
  ringClearGattPointers(false);
  return true;
}
static bool ringResetTelemetryForNewLink() {
  ++telemetryCalls;
  if (disconnectAtTelemetry) simulateDisconnect();
  return telemetrySucceeds;
}
static unsigned sRingConnFailStreak;
static bool discover() {
  const uint32_t t0 = 0, cancelGeneration = 1;
  const BlePeerConnectRequest* expectedRequest = nullptr;
  // INSERT_DISCOVERY
  return true;
}
static void reset() {
  haveService = haveWrite = haveNotify = notifyCapable = true;
  notifySucceeds = telemetrySucceeds = completionAvailable = current = true;
  disconnectAtDescriptor = disconnectAtRegister = disconnectAtTelemetry = false;
  serviceCalls = characteristicCalls = disconnectCalls = clockResets = 0;
  descriptorCalls = registrationCalls = telemetryCalls = 0;
  failureStage = nullptr;
  transportOnline = true;
  client.connected = true;
  gRing = {};
  gRing.client = &client;
  sRingConnFailStreak = 1;
}
static void requireDown() {
  assert(!gRing.connected && !transportOnline && gRing.clientStale);
  assert(!gRing.writeChar && !gRing.notifyChar && !gRing.connectedSince);
  assert(gRing.client == &client); // async event still owns this object
  assert(disconnectCalls == 1 && clockResets == 1);
}
int main() {
  for (bool servicePresent : {false, true}) {
    for (bool writePresent : {false, true}) {
      for (bool notifyPresent : {false, true}) {
        for (bool canNotify : {false, true}) {
          reset();
          haveService = servicePresent; haveWrite = writePresent;
          haveNotify = notifyPresent; notifyCapable = canNotify;
          const bool result = discover();
          assert(serviceCalls == 1);
          assert(characteristicCalls == (servicePresent ? 2u : 0u));
          assert(result == (servicePresent && writePresent && notifyPresent && canNotify));
          if (!result) {
            requireDown();
            assert(!std::strcmp(failureStage, !servicePresent ? "service"
                : !notifyPresent ? "notify-char" : "notify-capability"));
          } else {
            assert(gRing.connected && !gRing.clientStale);
            assert(gRing.writeChar == &writeHandle && gRing.notifyChar == &notifyHandle);
            assert(gRing.connectedSince == 100 && sRingConnFailStreak == 0);
            assert(disconnectCalls == 0 && clockResets == 0 && !failureStage);
          }
        }
      }
    }
  }
  for (bool success : {false, true}) {
    for (unsigned phase = 0; phase < 3; ++phase) {
      reset();
      notifySucceeds = success;
      disconnectAtDescriptor = phase == 0;
      disconnectAtRegister = phase == 1;
      disconnectAtTelemetry = phase == 2;
      assert(!discover());
      requireDown();
      assert(descriptorCalls == 1 && registrationCalls == 1);
      assert(telemetryCalls == (success && phase == 2 ? 1u : 0u));
    }
  }
  reset(); telemetrySucceeds = false;
  assert(!discover()); requireDown();
  assert(!std::strcmp(failureStage, "telemetry-reset"));
  reset(); completionAvailable = false;
  assert(!discover()); requireDown();
  reset(); client.connected = false; // DISCONNECT_EVT not delivered yet
  assert(!discover()); requireDown();
  std::puts("ring discovery: bounded lookup cleanup, disconnect during descriptor/notify/reset, and fenced transport admission");
}
