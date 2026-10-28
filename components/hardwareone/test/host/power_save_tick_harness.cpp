// The real header, policy, activity functions and both production ticks are
// compiled: powerPolicyTick (System_Power.cpp, every build) and, when
// ENABLE_OLED_DISPLAY is 1, powerDisplayTick (HardwareOne.cpp). Only clock
// admission, UI, activity sources, workload busy flags and platform time are
// mocked. One scenario per process so the function-static state is genuine.
#include "System_Power.h"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static std::vector<std::string> trace;
static void check(bool condition, const char* description) {
  if (!condition) throw std::runtime_error(description);
}
static uint32_t nowMs = 1000;
static uint32_t liveMhz = POWER_CPU_PERFORMANCE_MHZ;
static uint32_t inputSeq = 1;
static bool cooldownAllowed = true, micBusy = false, sttActive = false, cameraSessionOpen = false;
static bool uartRunning = false;
static int uartBaud = 0;
#ifndef ENABLE_CAMERA_SENSOR
#define ENABLE_CAMERA_SENSOR 1
#endif
#ifndef ENABLE_MICROPHONE
#define ENABLE_MICROPHONE 1
#endif
#ifndef ENABLE_OLED_DISPLAY
#define ENABLE_OLED_DISPLAY 1
#endif
#if !ENABLE_CAMERA_SENSOR || !ENABLE_MICROPHONE
#error "the camera and recording branches of the workload boost must be compiled"
#endif
// The workloads' own busy answers. Production declares them extern.
bool cameraCaptureBusy() { return cameraSessionOpen; }
bool micRecordingBusy() { return micBusy; }
bool sttAnySessionActive() { return sttActive; }
static bool gOledRunning = true;
static struct {
  uint32_t powerSaveTimeoutMinutes = 1;
  uint8_t powerMode = POWER_MODE_PERFORMANCE;
} gSettings;

// INSERT_TYPES
// INSERT_STATE

uint32_t millis() { return nowMs; }
static uint32_t getCpuFrequencyMhz() { return liveMhz; }
[[maybe_unused]] static bool uartLinkIsRunning() { return uartRunning; }
[[maybe_unused]] static int uartLinkEffectiveBaud() { return uartBaud; }
[[maybe_unused]] static void diagnostic(const char*, ...) {}
#define DEBUG_SYSTEMF(...) diagnostic(__VA_ARGS__)
void batteryLogEvent(const char* text) { trace.push_back(std::string("log:") + text); }
bool powerSleepTransitionAllowed(unsigned long* remaining) {
  if (remaining) *remaining = cooldownAllowed ? 0 : 1000;
  return cooldownAllowed;
}
void powerSleepTransitionMark() { trace.push_back("mark"); }
#if ENABLE_OLED_DISPLAY
static bool inputActivitySequence(uint32_t& sequence, uint32_t waitMs) {
  check(waitMs <= 5, "input snapshot uses the bounded wait");
  sequence = inputSeq;
  return true;
}
static constexpr int SYSEVT_POWER_SAVE_ENTER = 1, SYSEVT_POWER_SAVE_EXIT = 2;
static void systemEventPost(int event, const char* detail = nullptr) {
  if (event == SYSEVT_POWER_SAVE_ENTER) {
    check(!gOledRunning && detail && std::string(detail) == "idle 1min",
          "entry is published after display state changes");
    check(!powerLocalDisplayAwake(), "the policy learns the panel is dark before the event");
    trace.push_back("enter");
  } else {
    check(event == SYSEVT_POWER_SAVE_EXIT && gOledRunning, "exit follows display restoration");
    check(powerLocalDisplayAwake(), "the policy learns the panel is lit before the event");
    trace.push_back("exit");
  }
}
static void oledPrepareForSleep() {
  check(gOledRunning, "display is only prepared for sleep once");
  trace.push_back("sleep");
}
static void oledResumeFromSleep() {
  check(gOledRunning, "resume publishes running state");
  trace.push_back("resume");
}
static void oledMarkDirty() { trace.push_back("dirty"); }
#endif

struct ClockReply {
  bool accepted;
  uint32_t actual;  // 0 preserves live clock; otherwise explicit resulting clock.
  bool changedByCall;
};
static std::deque<ClockReply> clockReplies;
static std::vector<uint32_t> clockRequests;
bool powerSetCpuFrequencyMhz(uint32_t target, bool* outClockChanged) {
  trace.push_back("cpu:" + std::to_string(target));
  clockRequests.push_back(target);
  ClockReply reply{true, target, liveMhz != target};
  if (!clockReplies.empty()) {
    reply = clockReplies.front();
    clockReplies.pop_front();
  }
  if (reply.actual != 0) liveMhz = reply.actual;
  if (outClockChanged) *outClockChanged = reply.changedByCall;
  return reply.accepted;
}

// INSERT_FUNCTIONS

// Production backoffs in powerPolicyTick.
static constexpr unsigned long kRaiseRetryMs = 500;
static constexpr unsigned long kLowerRetryMs = 5000;
static constexpr uint32_t kMax = POWER_CPU_PERFORMANCE_MHZ;
static constexpr uint32_t kMid = POWER_CPU_BALANCED_MHZ;
static constexpr uint32_t kFloor = POWER_INTERACTIVE_FLOOR_MHZ;
// The heaviest workload this variant compiles: transcription where local STT
// is in, otherwise the camera.
struct Workload { bool* flag; const char* reason; };
static Workload heavyWork() {
#if ENABLE_LOCAL_STT
  return {&sttActive, "stt"};
#else
  return {&cameraSessionOpen, "camera"};
#endif
}

// The main loop's order: the panel tick first, then the clock policy.
static void tick() {
#if ENABLE_OLED_DISPLAY
  powerDisplayTick();
#endif
  powerPolicyTick();
}
static void clearTrace() { trace.clear(); clockRequests.clear(); }
static std::string cpu(uint32_t frequency) { return "cpu:" + std::to_string(frequency); }
static void expectTrace(std::vector<std::string> expected) {
  check(trace == expected, "display/cooldown/event/clock ordering matches the operation outcome");
}
static void boot() {
  tick();
  check(trace.empty() && gOledRunning, "first tick initializes without any transition");
}
static void idle() { nowMs += 60000; tick(); }
static void activity(bool local) { ++nowMs; powerSaveNoteActivity(local); tick(); }
static void steady() {
  clearTrace(); ++nowMs; tick();
  check(trace.empty(), "a settled state makes no further calls");
}
// A confirmed policy transition is followed by its battery-log note; a refused
// one (reason nullptr) is not.
static std::string clockLog(uint32_t mhz, const char* reason) {
  return std::string("log:cpu:") + reason + ":" + std::to_string(mhz) + "MHz";
}
// Panel blank (when there is a panel), then the clock the policy asks for.
static std::vector<std::string> enterItems(uint32_t requested, const char* reason = "idle") {
  std::vector<std::string> expected;
#if ENABLE_OLED_DISPLAY
  for (const auto* item : {"mark", "sleep", "log:powersave:enter", "enter"}) expected.emplace_back(item);
#endif
  if (requested) expected.push_back(cpu(requested));
  if (requested && reason) expected.push_back(clockLog(requested, reason));
  return expected;
}
static std::vector<std::string> wakeItems(uint32_t requested, const char* reason = "active") {
  std::vector<std::string> expected;
#if ENABLE_OLED_DISPLAY
  for (const auto* item : {"resume", "dirty", "log:powersave:wake", "exit"}) expected.emplace_back(item);
#endif
  if (requested) expected.push_back(cpu(requested));
  if (requested && reason) expected.push_back(clockLog(requested, reason));
  return expected;
}
static void enterTrace(uint32_t requested = kFloor) {
  expectTrace(enterItems(requested));
#if ENABLE_OLED_DISPLAY
  check(!gOledRunning && !powerLocalDisplayAwake(), "successful entry leaves display stopped");
#endif
}
static void wakeTrace(uint32_t requested = kMax) {
  expectTrace(wakeItems(requested));
  check(gOledRunning, "successful wake leaves display running");
}
static void expectBoostLog(uint32_t mhz, const char* reason) {
  expectTrace({cpu(mhz), clockLog(mhz, reason)});
}

static void test(const std::string& name) {
  const bool uartScenario = name.rfind("uart_", 0) == 0;
  if (name == "locked") { gSettings.powerMode = POWER_MODE_LOCKED; }
  if (name == "timeout_zero") { gSettings.powerSaveTimeoutMinutes = 0; }
  if (name == "boot_late") { nowMs = 10 * 60000 + 5000; }
  if (name == "adaptive_default" || name == "raise_refusal" || name == "manual_override" ||
      name == "already_idle") {
    gSettings.powerMode = POWER_MODE_ADAPTIVE;
    liveMhz = kFloor;   // applyPowerMode set the mode's in-use clock at boot
  }
  if (name == "ultra_lit_panel" || uartScenario) {
    gSettings.powerMode = POWER_MODE_ULTRASAVER;
    liveMhz = kFloor;
  }
  if (name == "mode_change_during_boost") { gSettings.powerMode = POWER_MODE_ADAPTIVE; liveMhz = kFloor; }
  if (name == "wrap_uptime") { nowMs = 0xFFFFFFFFu - 200000; }   // boot shortly before the wrap
  if (uartScenario) {
    uartRunning = name != "uart_stopped";
    uartBaud = name == "uart_slow" ? UART_LINK_REF_TICK_BAUD_LIMIT : UART_LINK_REF_TICK_BAUD_LIMIT + 1;
  }
  boot();

  if (name == "normal") {
    idle(); expectTrace(enterItems(kFloor)); check(liveMhz == kFloor, "idle sinks to the floor");
#if ENABLE_OLED_DISPLAY
    check(!gOledRunning, "panel blanks on the same lap");
#endif
    clearTrace(); activity(true);
    expectTrace(wakeItems(kMax)); check(liveMhz == kMax, "local activity restores the in-use clock");
    steady();
  } else if (name == "boot_late") {
    // millis() is already past the timeout at the first tick: the countdown
    // starts at that tick, not at zero.
    nowMs += 59999; tick(); check(trace.empty() && liveMhz == kMax, "no idle before a full timeout after boot");
    ++nowMs; tick(); expectTrace(enterItems(kFloor));
  } else if (name == "timeout_zero") {
    nowMs += 3600000; tick();
    check(trace.empty() && liveMhz == kMax && gOledRunning, "a disabled timeout never idles");
  } else if (name == "remote_keeps_panel_dark") {
    idle(); enterTrace(); clearTrace();
    activity(false);
    // A glasses / phone / web command raises the clock but does not light the panel.
    expectTrace({cpu(kMax), "log:cpu:active:" + std::to_string(kMax) + "MHz"});
    check(liveMhz == kMax, "remote activity restores the in-use clock");
#if ENABLE_OLED_DISPLAY
    check(!gOledRunning && !powerLocalDisplayAwake(), "remote activity leaves the panel dark");
#endif
    steady();
    clearTrace(); activity(true);
    expectTrace(wakeItems(0));   // clock already up: only the panel moves
  } else if (name == "adaptive_default") {
    idle(); enterTrace(0); check(liveMhz == kFloor, "Adaptive idles where it already was");
    clearTrace(); activity(false);
    check(trace.empty() && liveMhz == kFloor, "Adaptive stays at the floor for a remote command");
#if ENABLE_OLED_DISPLAY
    check(!gOledRunning, "panel stays dark");
#endif
    const Workload work = heavyWork();
    *work.flag = true; ++nowMs; tick();
    expectBoostLog(kMax, work.reason);
    check(liveMhz == kMax, "heavy work runs at the maximum clock");
#if ENABLE_OLED_DISPLAY
    check(!gOledRunning, "heavy work does not light the panel");
#endif
    steady();
    // The remote command a moment ago still counts as use; Adaptive's in-use
    // clock is the floor all the same.
    *work.flag = false; clearTrace(); ++nowMs; tick();
    expectBoostLog(kFloor, "active");
    check(liveMhz == kFloor, "the boost ends with the work");
    clearTrace(); activity(true); expectTrace(wakeItems(0));
  } else if (name == "already_idle") {
    idle(); enterTrace(0);
    clearTrace(); activity(true); wakeTrace(0);
    check(liveMhz == kFloor, "no lowering, no raising");
  } else if (name.rfind("boost_", 0) == 0) {
    // Performance, idle and dark: each workload lifts the clock to maximum on
    // its own flag and drops it back when the flag clears.
    idle(); enterTrace(); clearTrace();
    bool* flag = name == "boost_stt" ? &sttActive : name == "boost_camera" ? &cameraSessionOpen : &micBusy;
    const char* reason = name == "boost_stt" ? "stt" : name == "boost_camera" ? "camera" : "recording";
    check(name == "boost_stt" || name == "boost_camera" || name == "boost_recording", "known boost scenario");
    *flag = true; ++nowMs; tick();
#if !ENABLE_LOCAL_STT
    if (name == "boost_stt") {
      check(trace.empty() && liveMhz == kFloor, "disabled STT cannot boost");
      return;
    }
#endif
    expectBoostLog(kMax, reason);
#if ENABLE_OLED_DISPLAY
    check(!gOledRunning, "a boost does not light the panel");
#endif
    steady();
    *flag = false; clearTrace(); ++nowMs; tick();
    expectBoostLog(kFloor, "idle");
  } else if (name == "capture_holds_panel") {
    // Local capture defers the panel countdown; the clock is boosted meanwhile.
    micBusy = true; idle();
    check(trace.empty() && gOledRunning && liveMhz == kMax, "recording holds the panel up without a transition");
    micBusy = false; cameraSessionOpen = true; idle();
    check(trace.empty() && gOledRunning, "an open camera session holds the panel up");
    cameraSessionOpen = false; idle();
    expectTrace(enterItems(kFloor));
    check(!gOledRunning, "the panel blanks a full timeout after capture ends");
  } else if (name == "stt_does_not_hold_panel") {
    sttActive = true; idle();
#if ENABLE_LOCAL_STT
    expectTrace(enterItems(0));   // boosted already at kMax; only the panel moves
    check(!gOledRunning && liveMhz == kMax, "transcription keeps the clock, not the panel");
    sttActive = false; clearTrace(); ++nowMs; tick(); expectBoostLog(kFloor, "idle");
#else
    expectTrace(enterItems(kFloor));
    check(!gOledRunning && liveMhz == kFloor, "disabled STT is not a workload");
    sttActive = false;
#endif
  } else if (name == "cooldown_keeps_panel_and_clock") {
    cooldownAllowed = false; idle();
    check(trace.empty() && gOledRunning && liveMhz == kMax,
          "a refused sleep cooldown keeps the panel lit and the clock in use");
    cooldownAllowed = true; ++nowMs; tick(); expectTrace(enterItems(kFloor));
  } else if (name == "ultra_lit_panel") {
    cooldownAllowed = false; idle();
    check(trace.empty() && gOledRunning && liveMhz == kFloor, "UltraSaver never sinks to 40 MHz under a lit panel");
    cooldownAllowed = true; ++nowMs; tick(); expectTrace(enterItems(POWER_CPU_ULTRA_IDLE_MHZ));
    check(liveMhz == POWER_CPU_ULTRA_IDLE_MHZ, "dark panel permits the idle-only clock");
    clearTrace(); activity(true); expectTrace(wakeItems(kFloor));
  } else if (uartScenario) {
    const bool clamp = SOC_UART_SUPPORT_REF_TICK && uartRunning &&
                       uartBaud > UART_LINK_REF_TICK_BAUD_LIMIT;
    idle();
    expectTrace(enterItems(clamp ? 0 : POWER_CPU_ULTRA_IDLE_MHZ));
    check(liveMhz == (clamp ? kFloor : POWER_CPU_ULTRA_IDLE_MHZ),
          "UART link applies only its existing idle-floor restriction");
    clearTrace(); activity(true); expectTrace(wakeItems(clamp ? 0 : kFloor));
  } else if (name == "wake_input") {
    idle(); enterTrace(); clearTrace();
    ++nowMs; ++inputSeq; tick(); wakeTrace();
  } else if (name == "wake_disabled") {
    idle(); enterTrace(); clearTrace();
    ++nowMs; gSettings.powerSaveTimeoutMinutes = 0; tick(); wakeTrace();
  } else if (name == "locked") {
    idle(); enterTrace(0); check(liveMhz == kMax, "Locked never lowers");
    clearTrace(); sttActive = true; ++nowMs; tick(); check(trace.empty(), "Locked is already at maximum");
    sttActive = false; clearTrace(); activity(true); wakeTrace(0);
  } else if (name == "raise_refusal") {
    idle(); enterTrace(0); clearTrace();
    const Workload work = heavyWork();
    *work.flag = true; clockReplies.push_back({false, 0, false});
    ++nowMs; tick(); expectTrace({cpu(kMax)});
    check(liveMhz == kFloor, "a refused raise leaves the clock alone");
    const uint32_t refusedAt = nowMs;
    clearTrace(); ++nowMs; tick(); check(trace.empty(), "a refused raise is not retried on the next lap");
    nowMs = refusedAt + kRaiseRetryMs - 1; tick(); check(trace.empty(), "the raise waits out its short backoff");
    nowMs = refusedAt + kRaiseRetryMs; tick(); expectBoostLog(kMax, work.reason);
    check(liveMhz == kMax, "the retried raise lands");
  } else if (name == "lower_refusal") {
    clockReplies.push_back({false, 0, false});
    idle(); expectTrace(enterItems(kFloor, nullptr));
#if ENABLE_OLED_DISPLAY
    check(!gOledRunning, "the panel blanks even though the clock change was refused");
#endif
    check(liveMhz == kMax, "idle refusal preserves the clock");
    const uint32_t refusedAt = nowMs;
    clearTrace(); ++nowMs; tick(); check(trace.empty(), "a refused lowering is not retried on the next lap");
    nowMs = refusedAt + kLowerRetryMs - 1; tick(); check(trace.empty(), "the lowering waits out its backoff");
    nowMs = refusedAt + kLowerRetryMs; tick(); expectBoostLog(kFloor, "idle");
  } else if (name == "persistent_refusal") {
    // The bus stays wedged: one attempt per backoff window, never a loop pinned
    // to back-to-back waits.
    for (int i = 0; i < 3; ++i) clockReplies.push_back({false, 0, false});
    idle();
    for (int lap = 0; lap < 50; ++lap) { ++nowMs; tick(); }
    check(clockRequests.size() == 1, "refused lowering waits out the backoff");
    nowMs += kLowerRetryMs; tick();
    for (int lap = 0; lap < 50; ++lap) { ++nowMs; tick(); }
    check(clockRequests.size() == 2, "each refusal starts a new backoff");
    nowMs += kLowerRetryMs; tick();
    check(clockRequests.size() == 3 && liveMhz == kMax, "a persistently refused change leaves the clock alone");
    clearTrace(); nowMs += kLowerRetryMs; tick(); expectBoostLog(kFloor, "idle");
  } else if (name == "foreign_change_refusal") {
    // Another command's accepted change (and its new mode) wins over our refused one.
    clockReplies.push_back({false, kMid, false});
    idle(); expectTrace(enterItems(kFloor, nullptr));
    gSettings.powerMode = POWER_MODE_BALANCED;
    clearTrace(); activity(false);
    check(trace.empty() && liveMhz == kMid, "the policy accepts the foreign clock as its in-use clock without a call");
    clearTrace(); idle(); expectBoostLog(kFloor, "idle");
    clearTrace(); activity(false); expectBoostLog(kMid, "active");
  } else if (name == "own_partial_failure") {
    // The HAL moved the clock to the target but reported failure: the target is
    // reached, so the retry needs no second call.
    clockReplies.push_back({false, kFloor, true});
    idle(); expectTrace(enterItems(kFloor, nullptr)); check(liveMhz == kFloor, "partial change landed");
    clearTrace(); ++nowMs; tick(); check(trace.empty(), "backoff first");
    nowMs += kLowerRetryMs; tick(); check(trace.empty() && liveMhz == kFloor, "a reached target is adopted without another call");
    clearTrace(); activity(true); wakeTrace(kMax);
  } else if (name == "manual_override") {
    idle(); enterTrace(0); clearTrace();
    liveMhz = kMid;   // the user ran `cpufreq <mid>`
    ++nowMs; tick(); check(trace.empty() && liveMhz == kMid, "a manual clock holds while the target is unchanged");
    activity(false); check(trace.empty() && liveMhz == kMid, "remote activity in Adaptive changes nothing");
    const Workload work = heavyWork();
    *work.flag = true; ++nowMs; tick(); expectBoostLog(kMax, work.reason);
    // Still within a minute of the remote command: in use, at Adaptive's floor.
    *work.flag = false; clearTrace(); ++nowMs; tick(); expectBoostLog(kFloor, "active");
    check(liveMhz == kFloor, "the next policy change replaces the override");
  } else if (name == "mode_change_no_double_apply") {
    idle(); enterTrace(); clearTrace();
    // `power mode locked` already applied its clock; the policy adopts it.
    gSettings.powerMode = POWER_MODE_LOCKED; liveMhz = kMax;
    ++nowMs; tick(); check(trace.empty() && liveMhz == kMax, "an applied mode change is not re-applied");
    gSettings.powerMode = POWER_MODE_BALANCED; liveMhz = kMid;
    activity(false); check(trace.empty() && liveMhz == kMid, "the command's own clock stands");
    clearTrace(); idle(); expectBoostLog(kFloor, "idle");
  } else if (name == "mode_change_during_boost") {
    // A `power mode` applied while a workload runs must not leave the work at
    // the mode's clock: a boost is re-asserted, not applied once.
    const Workload work = heavyWork();
    *work.flag = true; ++nowMs; tick(); expectBoostLog(kMax, work.reason);
    gSettings.powerMode = POWER_MODE_BALANCED; liveMhz = kMid;   // applyPowerMode set the mode's clock
    clearTrace(); activity(false); expectBoostLog(kMax, work.reason);
    check(liveMhz == kMax, "the workload is back at the maximum clock on the next lap");
    liveMhz = kFloor;   // and a `cpufreq` during the workload is undone the same way
    clearTrace(); ++nowMs; tick(); expectBoostLog(kMax, work.reason);
    *work.flag = false; clearTrace(); ++nowMs; tick(); expectBoostLog(kMid, "active");
  } else if (name == "cpufreq_in_locked") {
    gSettings.powerMode = POWER_MODE_LOCKED; liveMhz = kFloor;   // a `cpufreq` the user ran in Locked
    ++nowMs; tick(); expectBoostLog(kMax, "locked");
  } else if (name == "partial_failure_then_in_use") {
    // The HAL lowered the clock but reported failure. Activity a moment later
    // must raise it again; the lowering's backoff does not hold a different target.
    clockReplies.push_back({false, kFloor, true});
    idle(); expectTrace(enterItems(kFloor, nullptr)); check(liveMhz == kFloor, "partial change landed");
    clearTrace(); activity(false); expectBoostLog(kMax, "active");
    check(liveMhz == kMax, "in use again at the in-use clock");
  } else if (name == "wrap_uptime") {
    // millis() wraps 49.7 days in: an elapsed-time check and the retry gate
    // must keep working on both sides of it (32-bit arithmetic throughout).
    nowMs = 0xFFFFFFFFu - 1000; powerSaveNoteActivity(true);   // local: keeps the panel too
    nowMs += 2000; tick();   // 999 ms after the wrap, 2 s after activity: in use
    check(trace.empty() && liveMhz == kMax && gOledRunning, "activity across the wrap still counts as use");
    nowMs += 60000; tick(); expectTrace(enterItems(kFloor));
    const Workload work = heavyWork();
    *work.flag = true; clearTrace(); ++nowMs; tick(); expectBoostLog(kMax, work.reason);
    check(liveMhz == kMax, "a boost after the wrap is applied, not held by a stale gate");
  } else {
    check(false, "known scenario requested");
  }
  check(clockReplies.empty(), "all scripted helper results were exercised");
}

int main(int argc, char** argv) {
  try {
    check(argc == 2, "one fresh-process scenario is required");
    test(argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Power-save tick " << (argc > 1 ? argv[1] : "?") << ": " << error.what() << '\n';
    for (const auto& item : trace) std::cerr << item << '\n';
    return 1;
  }
}
