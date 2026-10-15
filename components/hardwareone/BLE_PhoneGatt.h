#pragma once
// Native, bounded GATT adapter. The phone owner serializes every method except
// post(), which only copies callback data into a static queue without waiting.
// Keep this object alive for the entire host lifetime, including quarantine.
#include "BLE_PhonePipeCore.h"
#include <atomic>
#include <esp_gattc_api.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace hw1phone {
class Gatt {
 public:
  enum class Phase : uint8_t { Idle, Registering, Opening, Security, Mtu,
    Discovering, Subscribe, Cccd, Pipe, Retiring, Quarantined };
  bool begin(uint16_t appId, const uint8_t address[6], uint8_t type, uint32_t now);
  // Invoke from a peer-scoped pre-dispatch hook. True means this event belongs
  // exclusively to the phone. Unrelated/broadcast events stay with other peers.
  bool post(esp_gattc_cb_event_t event, esp_gatt_if_t interface,
            const esp_ble_gattc_cb_param_t* param);
  void poll(uint32_t now, bool accessoryPressure);
  // Only the security owner can advance past this barrier, after verifying
  // the current link's SMP result AND the exact enrolled/confirmed bond.
  bool authenticated(uint32_t now);
  void stop(uint32_t now, hw1pipe::Error reason = hw1pipe::Error::Cancelled);
  // Only after the role owner verifies terminal host/controller teardown.
  void hostStopped();
  Phase phase() const { return phase_; }
  bool idle() const { return phase_ == Phase::Idle; }
  bool ready() const { return phase_ == Phase::Pipe && pipe.state == hw1pipe::State::Ready; }
  uint32_t generation() const { return generation_; }
  const uint8_t* address() const { return address_; }
  uint16_t connection() const { return conn_; }
  hw1pipe::Session pipe;
 private:
  struct Event {
    esp_gattc_cb_event_t kind{};
    esp_gatt_if_t interface = ESP_GATT_IF_NONE;
    esp_gatt_status_t status = ESP_GATT_OK;
    uint16_t conn=0xffff, handle=0, last=0, length=0;
    bool flag=false;
    uint8_t data[hw1pipe::kValue]{};
  };
  // Three slots are reserved for control completions (write response, close,
  // disconnect); the rest absorb notifications that arrive between worker polls.
  // Thirteen covers several full connection events at the 10 ms poll period, so
  // the phone does not have to pace notifications to one per connection
  // interval. Notification overflow still fails the pipe; no unbounded
  // allocation or silent lost TLS bytes.
  static constexpr unsigned kControlSlots = 3;
  static constexpr unsigned kQueueSize = 16;
  StaticQueue_t queueControl_{};
  alignas(4) uint8_t queueBytes_[kQueueSize * sizeof(Event)]{};
  QueueHandle_t queue_ = nullptr;
  std::atomic<bool> overflow_{false}, registering_{false};
  std::atomic<esp_gatt_if_t> interface_{ESP_GATT_IF_NONE};
  std::atomic<uint16_t> appId_{0};
  Phase phase_ = Phase::Idle;
  uint8_t address_[6]{};
  uint8_t type_=0;
  uint16_t conn_=0xffff, mtu_=23, serviceStart_=0, serviceEnd_=0;
  uint16_t rx_=0,tx_=0,cccd_=0;
  unsigned services_=0;
  uint32_t generation_=0, phaseAt_=0, discoveryAt_=0, lastSend_=0, writeAt_=0;
  bool opened_=false, openPending_=false, cancelPending_=false;
  bool closePending_=false, unregisterPending_=false, writePending_=false;
  bool congested_=false;
  void consume(const Event& event, uint32_t now);
  void retire(uint32_t now);
  bool discover(uint32_t now);
  bool subscribe(uint32_t now);
  void enter(Phase phase, uint32_t now) { phase_=phase; phaseAt_=now; }
};
// Two 4 KiB stream windows per direction plus the callback queue; the queue
// grew from 6 to 16 slots (about 5 KB) so the phone need not pace notifications.
static_assert(sizeof(Gatt) <= 28 * 1024, "Phone pipe storage exceeds its budget");
}
