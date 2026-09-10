// System_MQTTLifecycleGate.h - lock-free MQTT command/lifecycle handshake
//
// Kept dependency-free so the exact production protocol can be stress-tested
// on the host.  The high bits close command admission and describe lifecycle
// work; the low bits count client users that may be blocked on cmd_exec or
// publishing through the handle.
#ifndef SYSTEM_MQTT_LIFECYCLEGATE_H
#define SYSTEM_MQTT_LIFECYCLEGATE_H

#include <atomic>
#include <cassert>
#include <cstdint>

class MqttLifecycleGate final {
 public:
  MqttLifecycleGate() = default;
  MqttLifecycleGate(const MqttLifecycleGate&) = delete;
  MqttLifecycleGate& operator=(const MqttLifecycleGate&) = delete;

  // A client starts with command admission closed.  Only one starter can own
  // the CLOSED -> STARTING transition.
  bool beginStart() {
    uint32_t expected = kClosed;
    return word_.compare_exchange_strong(expected, kClosed | kStarting,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire);
  }

  // Complete a beginStart() attempt.  A stop requested during startup wins:
  // the newly started client remains closed to commands and is left pending
  // for teardown by the lifecycle owner.
  bool finishStart(bool clientStarted) {
    uint32_t current = word_.load(std::memory_order_acquire);
    for (;;) {
      assert((current & kStarting) != 0);
      assert((current & kInFlightMask) == 0);

      uint32_t desired = kClosed;
      if (clientStarted) {
        desired = (current & kStopRequested)
                      ? (kClosed | kStopRequested)
                      : 0u;
      }
      if (word_.compare_exchange_weak(current, desired,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
        return clientStarted && desired == 0u;
      }
    }
  }

  // Admit one non-lifecycle use of the client. The CAS increments the in-flight
  // count only while admission is open, so it linearizes cleanly against
  // requestStop(), which closes admission in the same atomic word.
  bool tryAdmitUse() {
    uint32_t current = word_.load(std::memory_order_acquire);
    for (;;) {
      if ((current & kClosed) != 0) return false;
      const uint32_t count = current & kInFlightMask;
      if (count == kInFlightMask) return false;
      if (word_.compare_exchange_weak(current, current + 1u,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
        return true;
      }
    }
  }

  void releaseUse() {
    const uint32_t previous =
        word_.fetch_sub(1u, std::memory_order_acq_rel);
    assert((previous & kInFlightMask) != 0);
  }

  // Close admission and request teardown.  False means the client is already
  // fully stopped.  A concurrent start is ordered naturally: if beginStart()
  // won first, this marks that start for teardown; if this observed CLOSED
  // first, a later start is a new operation and is not cancelled.
  bool requestStop() {
    uint32_t current = word_.load(std::memory_order_acquire);
    for (;;) {
      if ((current & (kStopRequested | kStopping)) != 0) return true;
      if (current == kClosed) return false;
      const uint32_t desired = current | kClosed | kStopRequested;
      if (word_.compare_exchange_weak(current, desired,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
        return true;
      }
    }
  }

  // Called only by the lifecycle owner.  It can claim teardown only after all
  // admitted users have finished publishing through the handle.
  bool tryBeginStop() {
    uint32_t expected = kClosed | kStopRequested;
    return word_.compare_exchange_strong(
        expected, kClosed | kStopRequested | kStopping,
        std::memory_order_acq_rel, std::memory_order_acquire);
  }

  // The driver can transiently refuse a graceful stop (for example, if it
  // cannot construct the connected-state DISCONNECT packet).  Keep admission
  // closed and return ownership to the main-loop retry path.
  void retryStop() {
    uint32_t expected = kClosed | kStopRequested | kStopping;
    const bool changed = word_.compare_exchange_strong(
        expected, kClosed | kStopRequested,
        std::memory_order_acq_rel, std::memory_order_acquire);
    assert(changed);
    (void)changed;
  }

  void finishStop() {
    const uint32_t previous = word_.exchange(kClosed,
                                             std::memory_order_acq_rel);
    assert((previous & kStopping) != 0);
    assert((previous & kInFlightMask) == 0);
  }

  bool isOpen() const {
    return word_.load(std::memory_order_acquire) == 0u;
  }

  bool stopPending() const {
    return (word_.load(std::memory_order_acquire) &
            (kStopRequested | kStopping)) != 0;
  }

  bool starting() const {
    return (word_.load(std::memory_order_acquire) & kStarting) != 0;
  }

  uint32_t usesInFlight() const {
    return word_.load(std::memory_order_acquire) & kInFlightMask;
  }

 private:
  static constexpr uint32_t kClosed = 1u << 31;
  static constexpr uint32_t kStopRequested = 1u << 30;
  static constexpr uint32_t kStarting = 1u << 29;
  static constexpr uint32_t kStopping = 1u << 28;
  static constexpr uint32_t kInFlightMask = (1u << 28) - 1u;

  std::atomic<uint32_t> word_{kClosed};
};

#endif  // SYSTEM_MQTT_LIFECYCLEGATE_H
