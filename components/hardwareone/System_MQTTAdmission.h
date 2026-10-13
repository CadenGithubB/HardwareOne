// System_MQTTAdmission.h - movable admission on the MQTT lifecycle gate
//
// Kept dependency-free beside System_MQTTLifecycleGate.h so the host tests
// exercise the same type the firmware uses.
#ifndef SYSTEM_MQTT_ADMISSION_H
#define SYSTEM_MQTT_ADMISSION_H

#include "System_MQTTLifecycleGate.h"

// Owns at most one admitted use of a MqttLifecycleGate. Moving transfers the
// use, so a command admitted on the esp-mqtt task can hand it to the cmd_exec
// completion that publishes the response. Teardown stays blocked until the
// last owner releases it (explicitly or by destruction).
class MqttAdmission {
 public:
  MqttAdmission() = default;

  explicit MqttAdmission(MqttLifecycleGate& gate)
      : gate_(gate.tryAdmitUse() ? &gate : nullptr) {}

  MqttAdmission(MqttAdmission&& other) noexcept : gate_(other.gate_) {
    other.gate_ = nullptr;
  }

  MqttAdmission& operator=(MqttAdmission&& other) noexcept {
    if (this != &other) {
      release();
      gate_ = other.gate_;
      other.gate_ = nullptr;
    }
    return *this;
  }

  MqttAdmission(const MqttAdmission&) = delete;
  MqttAdmission& operator=(const MqttAdmission&) = delete;

  ~MqttAdmission() { release(); }

  bool admitted() const { return gate_ != nullptr; }

  void release() {
    MqttLifecycleGate* gate = gate_;
    gate_ = nullptr;
    if (gate) gate->releaseUse();
  }

 private:
  MqttLifecycleGate* gate_ = nullptr;
};

#endif  // SYSTEM_MQTT_ADMISSION_H
