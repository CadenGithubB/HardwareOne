// Host harness for the IMU's holder list and `closeimu`.
//
// test_motion_hook_wiring.py inserts, from HAL_Motion.cpp: imuHold,
// imuRelease, imuHoldersCopy, imuHolderCount and cmd_imustop. The holder
// rules are the shipping header's (HAL_MotionHookCore.h). Host boundaries:
// the critical section (counted: the list is only touched inside it), the
// open/close argument check, the reply buffer and the stop queue (recorded).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "HAL_MotionHookCore.h"

struct String {
  std::string text;
  String(const char* s = "") : text(s) {}
};
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (0)
enum I2cDeviceType { I2C_DEVICE_IMU = 2 };

// The critical section: entered and left in pairs, never nested.
struct portMUX_TYPE { int depth = 0; unsigned entered = 0; };
static void portENTER_CRITICAL(portMUX_TYPE* mux) { assert(mux->depth == 0); ++mux->depth; ++mux->entered; }
static void portEXIT_CRITICAL(portMUX_TYPE* mux) { assert(mux->depth == 1); --mux->depth; }
static portMUX_TYPE sImuHoldersMux;
static MotionHolders sImuHolders;

static const char* gArgError = nullptr;
static const char* i2cOpenCloseArgError(I2cDeviceType type, const String&, const char* command) {
  assert(type == I2C_DEVICE_IMU && std::string(command) == "closeimu");
  return gArgError;
}
static bool gHaveBuffer = true;
static char gBuffer[1024];
static bool ensureDebugBuffer() { return gHaveBuffer; }
static char* getDebugBuffer() { return gBuffer; }
static unsigned gStops = 0;
static const char* cmd_sensorstop_queued(I2cDeviceType type, const char* name) {
  assert(type == I2C_DEVICE_IMU && std::string(name) == "IMU");
  assert(sImuHoldersMux.depth == 0);  // the queue is never asked inside the critical section
  ++gStops;
  return "[IMU] Stop requested; cleanup will complete asynchronously";
}

// INSERT_PRODUCTION

static int failures = 0, checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

int main() {
  const String none("");
  // Nobody holds the IMU: closeimu goes to the stop queue.
  check(imuHolderCount() == 0, "no holder at the start");
  check(std::string(cmd_imustop(none)) == "[IMU] Stop requested; cleanup will complete asynchronously" && gStops == 1,
        "closeimu without a holder asks the stop queue");

  // The IMU input device holds it: closeimu refuses, names it, stops nothing.
  check(imuHold("input") && imuHolderCount() == 1, "a holder is counted");
  check(!imuHold("input") && imuHolderCount() == 1, "a holder holds once");
  check(!imuHold("") && !imuHold(nullptr) && imuHolderCount() == 1, "a holder has a name");
  const std::string refused = cmd_imustop(none);
  check(refused == "Error: The IMU is in use by input and stays open for it. Stop it first, then run 'closeimu'.",
        "closeimu names the holder: " + refused);
  check(gStops == 1, "a refused closeimu asks the stop queue for nothing");
  check(refused.rfind("Error: ", 0) == 0, "the refusal is an error reply");
  // Without the reply buffer it still refuses, with a reason.
  gHaveBuffer = false;
  const std::string bare = cmd_imustop(none);
  check(bare.rfind("Error: The IMU is in use", 0) == 0 && gStops == 1, "refused without the reply buffer: " + bare);
  gHaveBuffer = true;

  // Two holders: both are named, and it stays refused until the last one lets go.
  check(imuHold("gestures") && imuHolderCount() == 2, "a second holder");
  check(std::string(cmd_imustop(none)) ==
            "Error: The IMU is in use by input, gestures and stays open for them. Stop them first, then run 'closeimu'.",
        "closeimu names both holders");
  check(imuRelease("input") && imuHolderCount() == 1, "one holder lets go");
  check(std::string(cmd_imustop(none)).find("in use by gestures and stays open for it") != std::string::npos &&
            gStops == 1, "still held by the other");
  check(!imuRelease("input") && !imuRelease("nobody") && imuHolderCount() == 1, "only a holder can let go");
  check(imuRelease("gestures") && imuHolderCount() == 0, "the last holder lets go");
  check(std::string(cmd_imustop(none)).rfind("[IMU] Stop requested", 0) == 0 && gStops == 2,
        "closeimu works again once nobody holds the IMU");

  // A bad argument is refused before anything else, held or not.
  gArgError = "Error: imu has one instance.";
  check(std::string(cmd_imustop(String("2"))) == gArgError && gStops == 2, "the argument check comes first");
  check(imuHold("input"), "held again");
  check(std::string(cmd_imustop(String("2"))) == gArgError, "the argument check comes first while held");
  gArgError = nullptr;
  check(imuRelease("input"), "released");

  // The list was only ever touched inside its critical section, which is
  // left again: every call above entered it at least once.
  check(sImuHoldersMux.depth == 0 && sImuHoldersMux.entered >= 20, "the critical section is balanced");

  if (failures) {
    std::fprintf(stderr, "motion hook wiring: %d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("motion hook wiring: %d checks passed (holders, closeimu refusal, argument check first)\n", checks);
  return 0;
}
