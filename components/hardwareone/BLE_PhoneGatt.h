#pragma once
// Native, bounded GATT adapter. The phone owner serializes every method except
// post(), which only copies callback data into a static queue without waiting.
// Keep this object alive for the entire host lifetime, including quarantine.
#include "BLE_CoexRecords.h"
#include "BLE_PhonePipeCore.h"
#include "BLE_PhoneSecureChannel.h"
#include <atomic>
#include <esp_gattc_api.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace hw1phone {
class Gatt {
 public:
  // The Session (stream windows, control queue) and the callback bodies (the
  // Event slabs) live in PSRAM. The index queue, synchronization and
  // secure-channel state remain in this internal object. Both storages must
  // outlive this adapter, including host recovery/quarantine.
  // One queued callback. POD on purpose: the bodies live in the caller's slab
  // storage (PSRAM, where EXT_RAM_BSS is NOLOAD and no default member
  // initializer would survive), and post() writes every header field on every
  // fill. Three queue slots are reserved for control completions (write
  // response, close, disconnect); the rest absorb notifications that arrive
  // between worker polls. Thirteen covers several full connection events at
  // the 10 ms poll period, so the phone does not have to pace notifications to
  // one per connection interval. Notification overflow still fails the pipe;
  // no unbounded allocation or silent lost TLS bytes. One slab more than the
  // queue holds: the worker consumes one while the queue is full.
  struct Event {
    esp_gattc_cb_event_t kind;
    esp_gatt_if_t interface;
    esp_gatt_status_t status;
    uint16_t conn, handle, last, length;
    bool flag;
    uint8_t data[hw1pipe::kValue];
  };
  static constexpr unsigned kControlSlots = 3;
  static constexpr unsigned kQueueSize = 16;
  static constexpr unsigned kSlabCount = kQueueSize + 1;
  // `slabs` is the caller's Event[kSlabCount] (BLE_PhonePipe.cpp puts it in PSRAM).
  Gatt(hw1pipe::Session& session, Event* slabs) : pipe(session), slabs_(slabs) {}
  Gatt(const Gatt&) = delete;
  Gatt& operator=(const Gatt&) = delete;
  enum class Phase : uint8_t { Idle, Registering, Opening, Mtu,
    Discovering, Subscribe, Cccd, Secure, Pipe, Retiring, Quarantined };
  bool begin(uint16_t appId, const uint8_t address[6], uint8_t type, uint32_t now,const uint8_t psk[32]);
  // Invoke from a peer-scoped pre-dispatch hook. True means this event belongs
  // exclusively to the phone. Unrelated/broadcast events stay with other peers.
  bool post(esp_gattc_cb_event_t event, esp_gatt_if_t interface,
            const esp_ble_gattc_cb_param_t* param);
  // relayPressure: display, setup or mic AFE (relay streams spend a 2 s budget).
  // consoleDisplayBusy: display only; the console yields for as long as it lasts.
  void poll(uint32_t now, bool relayPressure, bool consoleDisplayBusy = false);
  void stop(uint32_t now, hw1pipe::Error reason = hw1pipe::Error::Cancelled);
  // Only after the role owner verifies terminal host/controller teardown.
  void hostStopped();
  Phase phase() const { return phase_; }
  bool idle() const { return phase_ == Phase::Idle; }
  bool ready() const { return phase_ == Phase::Pipe && pipe.state == hw1pipe::State::Ready; }
  bool authenticationFailed() const {return authenticationFailed_;}
  // This session received Service Changed (F3 counts those ends separately).
  bool serviceChanged() const {return serviceChanged_;}
  uint32_t generation() const { return generation_; }
  // When this attempt entered each phase up to Pipe (0: not reached) and when
  // Bluedroid's automatic discovery ended (0: not seen): the setup breakdown.
  uint32_t enteredAt(Phase p) const { return unsigned(p)<=unsigned(Phase::Pipe)?entered_[unsigned(p)]:0; }
  uint32_t discoveredAt() const { return autoDiscoveryDone_?autoDiscoveryAt_:0; }
  const uint8_t* address() const { return address_; }
  uint16_t connection() const { return conn_; }
  // How this attempt ended, for the coexistence record. Latched once, by the
  // first stop after a successful registration; later stops (our own close, an
  // overflow while retiring) never overwrite it. Task context only.
  //  stage: the step running at that stop (hw1coex::ConnectStage phone steps).
  //  gatt: the attempt's first non-OK GATT status (-1: none).
  //  reason: the first disconnect reason for the phone's own address: the one
  //    before a failed OPEN (0x3e after Bluedroid's silent retries) or a live
  //    link's (0x08). 0 does not rule out retries hidden inside a success.
  struct AttemptEnd {
    uint32_t at=0;
    hw1coex::ConnectStage stage=hw1coex::ConnectStage::None;
    hw1pipe::Error error=hw1pipe::Error::None;
    int16_t gatt=-1;
    uint16_t reason=0;
    bool serviceChanged=false;
  };
  const AttemptEnd* attemptEnd() const { return concluded_ ? &end_ : nullptr; }
  hw1pipe::Session& pipe;
 private:
  // The FreeRTOS queue carries slab indices only (one byte each); the bodies
  // sit in slabs_. post() reserves a slab (Free -> Writing) on the BTC task,
  // fills it after the reservation, marks it Queued and enqueues the index with
  // no wait; poll() consumes by index and frees the slab. Ownership is the
  // atomic state byte, so no critical section ever touches PSRAM.
  enum : uint8_t { kSlabFree = 0, kSlabWriting, kSlabQueued, kSlabProcessing };
  Event* slabs_;
  std::atomic<uint8_t> slabState_[kSlabCount]{};
  int slabReserve();
  void slabRelease(unsigned index) { slabState_[index].store(kSlabFree, std::memory_order_release); }
  void slabsReset() { for (auto& s : slabState_) s.store(kSlabFree, std::memory_order_relaxed); }
  StaticQueue_t queueControl_{};
  uint8_t indexBytes_[kQueueSize]{};
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
  // MTU setup waits behind Bluedroid's automatic discovery. bta_gattc_conn
  // starts it before our OPEN arrives (this build keeps no GATT cache, so every
  // connection rediscovers), and IDF 5.5.5 queues our CFG_MTU behind it: it
  // runs once DIS_SRVC_CMPL is posted. Phase::Mtu therefore lasts up to 10 s
  // until DIS_SRVC_CMPL, then 2 s for the answer. The request is sent once.
  bool autoDiscoveryDone_=false;
  uint32_t autoDiscoveryAt_=0;
  AttemptEnd end_;
  bool armed_=false, concluded_=false;
  // Bluedroid's own discovery failed: a non-OK DIS_SRVC_CMPL, or our queued
  // CFG_MTU ran without one (the partial cache is then searched). Either way
  // the attempt's stage reads discovery. mtuRefused_: the request never left.
  bool discoveryFailed_=false, mtuRefused_=false;
  void conclude(uint32_t now, hw1pipe::Error why);
  void noteStatus(esp_gatt_status_t status);
  hw1coex::ConnectStage stageNow() const;
  uint32_t entered_[unsigned(Phase::Pipe)+1]{};
  SecureChannel channel_;
  uint8_t psk_[32]{},reply_[49]{};
  size_t replyLength_=0;
  uint32_t secureAt_=0,rejectAt_=0;
  // A-5: time during the handshake when the P4 owed the next step (a reply
  // queued or a write not yet completed). Only peer silence is charged.
  uint32_t localSince_=0,localMs_=0;
  bool localOwed_=false;
  bool rejectPending_=false;
  bool descriptorPending_=false,authenticationFailed_=false,serviceChanged_=false;
  void consume(const Event& event, uint32_t now);
  void retire(uint32_t now);
  bool discover(uint32_t now);
  bool subscribe(uint32_t now);
  void enter(Phase phase, uint32_t now) {
    phase_=phase; phaseAt_=now;
    if(unsigned(phase)<=unsigned(Phase::Pipe))entered_[unsigned(phase)]=now;
  }
};
// Internal RAM: the index queue, the slab states and the secure channel. The
// 17 callback bodies (about 9.1 KB) are the caller's PSRAM slabs, and the
// Session has its own PSRAM budget (BLE_PhonePipeCore.h).
static_assert(sizeof(Gatt) <= 1024, "Phone GATT adapter exceeds its internal RAM budget");
}
