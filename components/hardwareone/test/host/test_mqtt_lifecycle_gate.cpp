// Host tests for the exact packed atomic gate used by System_MQTT.cpp.
#include <atomic>
#include <cstdio>
#include <thread>

#include "../../System_MQTTLifecycleGate.h"

static int failures = 0;

#define CHECK(condition, message)                                            \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static void openGate(MqttLifecycleGate& gate) {
  CHECK(gate.beginStart(), "stopped gate must admit one starter");
  CHECK(gate.finishStart(true), "successful start must open admission");
  CHECK(gate.isOpen(), "successful start must leave gate open");
}

static void test_command_first_defers_stop() {
  MqttLifecycleGate gate;
  openGate(gate);

  CHECK(gate.tryAdmitUse(), "open client must admit command callback");
  CHECK(gate.usesInFlight() == 1, "admission count must increment");
  CHECK(gate.requestStop(), "running client stop must be accepted");
  CHECK(!gate.tryAdmitUse(), "stop request must close new admission");
  CHECK(!gate.tryBeginStop(), "active response must defer teardown");

  gate.releaseUse();
  CHECK(gate.tryBeginStop(), "last response release must unblock teardown");
  gate.finishStop();
  CHECK(!gate.isOpen(), "stopped client admission must remain closed");
  CHECK(!gate.requestStop(), "duplicate stop after completion is a no-op");
}

static void test_stop_first_rejects_command() {
  MqttLifecycleGate gate;
  openGate(gate);

  CHECK(gate.requestStop(), "running stop must be accepted");
  CHECK(!gate.tryAdmitUse(), "stop-first ordering must reject command");
  CHECK(gate.tryBeginStop(), "stop with no response may start immediately");
  CHECK(gate.requestStop(), "duplicate in-progress stop remains accepted");
  gate.finishStop();
}

static void test_all_admissions_must_release() {
  MqttLifecycleGate gate;
  openGate(gate);
  CHECK(gate.tryAdmitUse(), "first callback must be admitted");
  CHECK(gate.tryAdmitUse(), "second callback must be admitted");
  CHECK(gate.usesInFlight() == 2, "both callbacks must be counted");
  CHECK(gate.requestStop(), "stop with two callbacks must be accepted");
  gate.releaseUse();
  CHECK(!gate.tryBeginStop(), "one remaining callback must still defer stop");
  gate.releaseUse();
  CHECK(gate.tryBeginStop(), "only the last release may unblock stop");
  gate.finishStop();
}

static void test_start_stop_ordering() {
  MqttLifecycleGate gate;
  CHECK(!gate.requestStop(), "initial stopped client has nothing to stop");
  CHECK(gate.beginStart(), "initial start claim must succeed");
  CHECK(gate.requestStop(), "stop racing a claimed start must be retained");
  CHECK(!gate.finishStart(true), "stop must win over successful startup");
  CHECK(!gate.isOpen(), "stop-won startup must not open command admission");
  CHECK(gate.tryBeginStop(), "started client must remain queued for teardown");
  gate.finishStop();

  CHECK(gate.beginStart(), "restart after completed stop must be possible");
  CHECK(!gate.finishStart(false), "failed start must report closed");
  CHECK(!gate.stopPending(), "failed start must not strand a stop request");

  CHECK(gate.beginStart(), "second failed-start claim must succeed");
  CHECK(gate.requestStop(), "stop during failed start must be retained");
  CHECK(!gate.finishStart(false), "failed driver start cannot open admission");
  CHECK(!gate.stopPending(),
        "failed start satisfies its concurrent stop without stranding state");
  CHECK(gate.beginStart(), "restart after failed start+stop must remain possible");
  CHECK(gate.finishStart(true), "restart after failed start+stop must open");
  CHECK(gate.requestStop(), "cleanup stop must be accepted");
  CHECK(gate.tryBeginStop(), "cleanup stop must be claimable");
  gate.finishStop();
}

static void test_transient_stop_retry() {
  MqttLifecycleGate gate;
  openGate(gate);
  CHECK(gate.requestStop(), "retry test stop must be accepted");
  CHECK(gate.tryBeginStop(), "retry test must claim teardown");
  gate.retryStop();
  CHECK(gate.stopPending(), "failed driver stop must remain pending");
  CHECK(!gate.tryAdmitUse(), "retry must keep command admission closed");
  CHECK(gate.tryBeginStop(), "pending stop must be reclaimable for retry");
  gate.finishStop();
}

static void test_atomic_admit_stop_race() {
  // The admitted side deliberately holds its reference until the stop side
  // has attempted teardown.  Therefore both results being true would prove a
  // forbidden overlap, not merely two valid operations occurring in sequence.
  constexpr int kIterations = 500;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    MqttLifecycleGate gate;
    openGate(gate);

    std::atomic<bool> go{false};
    std::atomic<bool> stopAttempted{false};
    bool admitted = false;
    bool requested = false;
    bool beganStop = false;

    std::thread commandThread([&]() {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      admitted = gate.tryAdmitUse();
      if (admitted) {
        while (!stopAttempted.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        gate.releaseUse();
      }
    });
    std::thread stopThread([&]() {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      requested = gate.requestStop();
      beganStop = gate.tryBeginStop();
      stopAttempted.store(true, std::memory_order_release);
    });

    go.store(true, std::memory_order_release);
    commandThread.join();
    stopThread.join();

    CHECK(requested, "racing stop must be accepted for an open client");
    CHECK(!(admitted && beganStop),
          "command admission and active teardown must never overlap");
    if (!beganStop) {
      CHECK(gate.tryBeginStop(),
            "teardown must become claimable after admitted response releases");
    }
    gate.finishStop();
  }
}

static void test_atomic_start_stop_race() {
  constexpr int kIterations = 250;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    MqttLifecycleGate gate;
    std::atomic<bool> go{false};
    bool startClaimed = false;
    bool stopRequested = false;

    std::thread startThread([&]() {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      startClaimed = gate.beginStart();
    });
    std::thread stopThread([&]() {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      stopRequested = gate.requestStop();
    });
    go.store(true, std::memory_order_release);
    startThread.join();
    stopThread.join();

    CHECK(startClaimed, "start must claim either before or after stopped no-op");
    const bool opened = gate.finishStart(true);
    if (stopRequested) {
      CHECK(!opened, "stop that observes STARTING must win over gate opening");
      CHECK(gate.tryBeginStop(), "stop-won startup must be tear-downable");
    } else {
      CHECK(opened, "stop that linearizes while CLOSED must not cancel later start");
      CHECK(gate.requestStop(), "opened race winner must accept cleanup stop");
      CHECK(gate.tryBeginStop(), "opened race winner must be tear-downable");
    }
    gate.finishStop();
  }
}

int main() {
  test_command_first_defers_stop();
  test_stop_first_rejects_command();
  test_all_admissions_must_release();
  test_start_stop_ordering();
  test_transient_stop_retry();
  test_atomic_admit_stop_race();
  test_atomic_start_stop_race();

  if (failures != 0) {
    std::fprintf(stderr, "%d MQTT lifecycle gate test(s) failed\n", failures);
    return 1;
  }
  std::puts("MQTT lifecycle gate tests passed");
  return 0;
}
