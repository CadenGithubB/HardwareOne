// The real header, policy, activity functions and complete tick are compiled.
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
static bool cooldownAllowed = true, micBusy = false, sttActive = false;
static bool uartRunning = false;
static int uartBaud = 0;
bool cameraStreaming = false;
static bool gOledRunning = true;
static struct {
  uint32_t powerSaveTimeoutMinutes = 1;
  uint8_t powerMode = POWER_MODE_PERFORMANCE;
} gSettings;

// INSERT_TYPES
// INSERT_STATE

uint32_t millis() { return nowMs; }
static uint32_t getCpuFrequencyMhz() { return liveMhz; }
static bool micRecordingBusy() { return micBusy; }
bool sttAnySessionActive() { return sttActive; }
[[maybe_unused]] static bool uartLinkIsRunning() { return uartRunning; }
[[maybe_unused]] static int uartLinkEffectiveBaud() { return uartBaud; }
[[maybe_unused]] static void diagnostic(const char*, ...) {}
#define DEBUG_SYSTEMF(...) diagnostic(__VA_ARGS__)
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
    trace.push_back("enter");
  } else {
    check(event == SYSEVT_POWER_SAVE_EXIT && gOledRunning, "exit follows display restoration");
    trace.push_back("exit");
  }
}
static void batteryLogEvent(const char* text) { trace.push_back(std::string("log:") + text); }
static void oledPrepareForSleep() {
  check(gOledRunning, "display is only prepared for sleep once");
  trace.push_back("sleep");
}
static void oledResumeFromSleep() {
  check(gOledRunning, "resume publishes running state");
  trace.push_back("resume");
}
static void oledMarkDirty() { trace.push_back("dirty"); }
bool powerSleepTransitionAllowed(unsigned long* remaining) {
  if (remaining) *remaining = cooldownAllowed ? 0 : 1000;
  return cooldownAllowed;
}
void powerSleepTransitionMark() { trace.push_back("mark"); }

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

static constexpr unsigned long kClockRetryBackoffMs = 5000;  // production backoff in powerSaveTick
static void clearTrace() { trace.clear(); clockRequests.clear(); }
static std::string cpu(uint32_t frequency) { return "cpu:" + std::to_string(frequency); }
static void expectTrace(std::vector<std::string> expected) {
  check(trace == expected, "clock/display/cooldown/event ordering matches the operation outcome");
}
static void boot() {
  powerSaveTick();
  check(trace.empty() && gOledRunning, "first tick initializes without entering sleep");
}
static void idle() { nowMs += 60000; powerSaveTick(); }
static void enterTrace(uint32_t requested = POWER_INTERACTIVE_FLOOR_MHZ) {
  std::vector<std::string> expected;
  if (requested) expected.push_back(cpu(requested));
  for (const auto* item : {"mark", "sleep", "log:powersave:enter", "enter"}) expected.emplace_back(item);
  expectTrace(expected);
  check(!gOledRunning, "successful entry leaves display stopped");
}
static void wakeTrace(uint32_t requested = POWER_CPU_PERFORMANCE_MHZ) {
  std::vector<std::string> expected;
  if (requested) expected.push_back(cpu(requested));
  for (const auto* item : {"resume", "dirty", "log:powersave:wake", "exit"}) expected.emplace_back(item);
  expectTrace(expected);
  check(gOledRunning, "successful wake leaves display running");
}
static void activity() { ++nowMs; powerSaveNoteActivity(); }

static void test(const std::string& name) {
  if (name == "locked") gSettings.powerMode = POWER_MODE_LOCKED;
  if (name == "already_idle") liveMhz = POWER_INTERACTIVE_FLOOR_MHZ;
  const bool uartScenario = name.rfind("uart_", 0) == 0;
  if (uartScenario) {
    gSettings.powerMode = POWER_MODE_ULTRASAVER;
    liveMhz = POWER_INTERACTIVE_FLOOR_MHZ;
    uartRunning = name != "uart_stopped";
    uartBaud = name == "uart_slow" ? UART_LINK_REF_TICK_BAUD_LIMIT : UART_LINK_REF_TICK_BAUD_LIMIT + 1;
  }
  boot();

  if (name == "idle_refusal") {
    clockReplies.push_back({false, 0, false});
    idle();
    expectTrace({cpu(POWER_INTERACTIVE_FLOOR_MHZ)});
    check(gOledRunning && liveMhz == POWER_CPU_PERFORMANCE_MHZ, "idle refusal preserves display and clock");
    clearTrace(); ++nowMs; powerSaveTick();
    check(trace.empty() && gOledRunning, "a refused clock change is not retried on the next lap");
    nowMs += kClockRetryBackoffMs; powerSaveTick();
    enterTrace();
  } else if (name == "persistent_refusal") {
    // The bus stays wedged: one attempt per backoff window, the panel stays up
    // and the loop is never pinned to back-to-back waits.
    for (int i = 0; i < 3; ++i) clockReplies.push_back({false, 0, false});
    idle();
    for (int lap = 0; lap < 50; ++lap) { ++nowMs; powerSaveTick(); }
    check(clockRequests.size() == 1 && gOledRunning, "refused idle entry waits out the backoff");
    nowMs += kClockRetryBackoffMs; powerSaveTick();
    for (int lap = 0; lap < 50; ++lap) { ++nowMs; powerSaveTick(); }
    check(clockRequests.size() == 2 && gOledRunning, "each refusal starts a new backoff");
    nowMs += kClockRetryBackoffMs; powerSaveTick();
    check(clockRequests.size() == 3 && gOledRunning && liveMhz == POWER_CPU_PERFORMANCE_MHZ,
          "a persistently refused clock change leaves display and clock alone");
    clearTrace(); nowMs += kClockRetryBackoffMs; powerSaveTick();
    enterTrace();
  } else if (name == "foreign_change_refusal") {
    clockReplies.push_back({false, POWER_CPU_BALANCED_MHZ, false});
    idle();
    expectTrace({cpu(POWER_INTERACTIVE_FLOOR_MHZ)});
    check(gOledRunning, "refused idle does not sleep after a foreign clock change");
    // The other command also updates its selected mode and stamps activity.
    gSettings.powerMode = POWER_MODE_BALANCED;
    clearTrace(); activity(); powerSaveTick();
    check(trace.empty() && liveMhz == POWER_CPU_BALANCED_MHZ,
          "refused idle never restores a foreign accepted change to the old clock");
    idle(); enterTrace();
    clearTrace(); activity(); powerSaveTick();
    wakeTrace(POWER_CPU_BALANCED_MHZ);
    check(liveMhz == POWER_CPU_BALANCED_MHZ, "later wake restores the newer captured active clock");
  } else if (name == "own_partial_failure") {
    clockReplies.push_back({false, POWER_INTERACTIVE_FLOOR_MHZ, true});
    idle();
    expectTrace({cpu(POWER_INTERACTIVE_FLOOR_MHZ)});
    check(gOledRunning, "unconfirmed own transition never sleeps the display");
    clearTrace(); clockReplies.push_back({false, 0, false});
    ++nowMs; powerSaveTick();
    check(trace.empty(), "recovery of an own partial change waits out the backoff");
    nowMs += kClockRetryBackoffMs; powerSaveTick();
    expectTrace({cpu(POWER_CPU_PERFORMANCE_MHZ)});
    check(gOledRunning, "failed recovery keeps the previously awake panel");
    clearTrace(); nowMs += kClockRetryBackoffMs; powerSaveTick();
    expectTrace({cpu(POWER_CPU_PERFORMANCE_MHZ)});
    check(liveMhz == POWER_CPU_PERFORMANCE_MHZ && gOledRunning,
          "own partial change recovers without a fabricated sleep-exit event");
  } else if (name.rfind("wake_", 0) == 0) {
    idle(); enterTrace(); clearTrace(); ++nowMs;
    if (name == "wake_input") ++inputSeq;
    else if (name == "wake_command") powerSaveNoteActivity();
    else if (name == "wake_disabled") gSettings.powerSaveTimeoutMinutes = 0;
    else if (name == "wake_stt") sttActive = true;
    else check(false, "known wake scenario");
    clockReplies.push_back({false, 0, false});
    powerSaveTick();
#if !ENABLE_LOCAL_STT
    if (name == "wake_stt") {
      check(trace.empty() && !gOledRunning, "disabled STT does not trigger a wake");
      return;
    }
#endif
    wakeTrace();
    check(gOledRunning && liveMhz == POWER_INTERACTIVE_FLOOR_MHZ,
          "a refused clock restore still brings the display back; only the clock stays pending");
    clearTrace(); ++nowMs; powerSaveTick();
    check(trace.empty(), "the pending clock restore waits out the backoff");
    nowMs += kClockRetryBackoffMs; powerSaveTick();
    expectTrace({cpu(POWER_CPU_PERFORMANCE_MHZ)});
    check(liveMhz == POWER_CPU_PERFORMANCE_MHZ && gOledRunning, "clock retry restores the captured frequency without a second resume");
    clearTrace(); ++nowMs; powerSaveTick();
    check(trace.empty(), "completed wake is not repeated");
  } else if (name == "locked" || name == "already_idle") {
    const uint32_t before = liveMhz;
    idle(); enterTrace(0);
    check(liveMhz == before, "no lowering leaves the clock alone");
    clearTrace(); activity(); powerSaveTick();
    wakeTrace(0);
    check(liveMhz == before, "wake without prior lowering does not request a raise");
  } else if (uartScenario) {
    const bool clamp = SOC_UART_SUPPORT_REF_TICK && uartRunning &&
                       uartBaud > UART_LINK_REF_TICK_BAUD_LIMIT;
    idle();
    enterTrace(clamp ? 0 : POWER_CPU_ULTRA_IDLE_MHZ);
    check(liveMhz == (clamp ? POWER_INTERACTIVE_FLOOR_MHZ : POWER_CPU_ULTRA_IDLE_MHZ),
          "UART link applies only its existing idle-floor restriction");
    clearTrace(); activity(); powerSaveTick();
    wakeTrace(clamp ? 0 : POWER_INTERACTIVE_FLOOR_MHZ);
  } else if (name == "inhibited") {
    micBusy = true;
    idle(); check(trace.empty() && gOledRunning, "microphone activity delays idle entry");
    micBusy = false; cameraStreaming = true;
    idle(); check(trace.empty() && gOledRunning, "camera activity delays idle entry");
    cameraStreaming = false; cooldownAllowed = false;
    idle(); check(trace.empty(), "cooldown refusal performs no clock or display mutation");
    cooldownAllowed = true; ++nowMs; powerSaveTick(); enterTrace();
  } else if (name == "normal") {
    idle(); enterTrace();
    clearTrace(); activity(); powerSaveTick(); wakeTrace();
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
