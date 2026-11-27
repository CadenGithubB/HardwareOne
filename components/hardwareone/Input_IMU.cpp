#include "Input_IMU.h"

#if ENABLE_IMU_INPUT
#include <Arduino.h>
#include <atomic>
#include <esp_attr.h>  // EXT_RAM_BSS_ATTR
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "HAL_Input.h"
#include "HAL_InputInternal.h"
#include "HAL_Motion.h"
#include "HAL_MotionBackend.h"  // motionBackendInfo: the sample period (decimation)
#include "HAL_MotionHookCore.h"
#include "Input_IMUCore.h"
#include "System_Command.h"
#include "System_CommandRegistry.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_SensorJsonKeys.h"  // inter-device sensor JSON key names (receivers read the same)
#include "System_Settings.h"
#include "System_Utils.h"
#if ENABLE_IMU_KNOCK_DETECTOR
#include "HAL_Motion_KnockFeed.h"  // knockFeedActive / knockFeedTeachCancel: the configuration's knock stages
#endif
#if ENABLE_OLED_DISPLAY
#include "OLED_UI.h"  // oledToastUpdate: the configuration's prompts
#endif

// The OLED-facing input state, as every input driver defines it. Running: the
// input is open (hook registered, IMU held). Connected: samples are arriving
// from a chip that detects taps and gives gravity.
bool gInputRunning = false;
bool gInputConnected = false;

// The name this device holds the IMU under ('closeimu' and 'imustatus' show it).
static const char kHolder[] = "input";

// Start and stop, one at a time (the command task, the menu, the boot).
static StaticSemaphore_t sStateMutexStorage;
static SemaphoreHandle_t sStateMutex = xSemaphoreCreateMutexStatic(&sStateMutexStorage);

// ---- What the hook owns -----------------------------------------------------
// Only the IMU task touches these, and only inside the hook: start and stop
// change them while the hook is not registered.
EXT_RAM_BSS_ATTR static imuinput::Engine sEngine;
// A feed faster than the engine's 20 ms (the BNO055: 10 ms) reaches it
// through this, every tap kept (Input_IMUCore.h).
EXT_RAM_BSS_ATTR static imuinput::Decimator sDecimator;
static uint32_t sUnpublishedPresses = 0;   // presses the input reading has not taken yet
static bool sUnpublishedActivity = false;

static std::atomic<bool> sRecentreRequest{false};
static std::atomic<bool> sLearnRequest{false};
// The teach step's result for the main loop and the waiting command: the
// mount code learned (-1 none) and a count of finished teach steps.
static std::atomic<int> sLearnedMount{-1};

#if ENABLE_IMU_KNOCK_DETECTOR
// A finished shake configuration's two knock steps, for the main loop to save
// (imuInputTick): the hook writes them only while ready is false, then sets
// it; the main loop copies them, then clears it.
struct ConfigTaught {
  knock::Teacher side, face;
};
EXT_RAM_BSS_ATTR static ConfigTaught sConfigTaught;
static std::atomic<bool> sConfigTaughtReady{false};
#endif

// ---- What a command reads ---------------------------------------------------
// Written by the hook, copied by readers: a version counter that is odd while
// the hook writes, so the hook never waits for a reader.
struct ImuInputStatus {
  uint32_t ms;          // the last sample
  int16_t joyX, joyY;
  uint8_t held;         // imuinput::kButton*
  uint8_t tapCount;     // taps counted in the run in progress
  bool centred;         // a reference pose is in use
  bool centring;        // one is being taken
  bool shaking;
  bool calibrating;     // a shake configuration is under way (any stage)
  uint8_t calStage;     // ...its stage (imuinput::kStage*)
  uint8_t calKnocks;    // ...and knocks taken in a knock stage
  bool unsupported;     // the IMU's backend gives no taps or no gravity
  bool imuStopped;      // the IMU closed under the input
  float leanX, leanY;   // degrees from the centre pose
  uint32_t presses;     // buttons pressed since the input opened
  uint32_t recentres;   // reference poses taken since it opened
  uint8_t lastPress;    // the buttons of the most recent press (kButton*), kept until the next
  uint8_t lastTaps;     // the taps in the run that made it
  uint32_t drops;       // runs of taps that made no button
  uint8_t dropTaps;     // the last such run: its taps, side or face, and why (imuinput::kDrop*)
  bool dropFace;
  uint8_t dropWhy;
  bool learning;        // the teach step is waiting for a lean
  uint8_t lastLearn;    // how the last teach step ended (kLastLearn*)
  uint8_t lastConfig;   // how the last shake configuration ended (kLastConfig*)
  uint8_t lastConfigStage;  // ...in which stage (imuinput::kStage*), for a cancel or a time-out
};
EXT_RAM_BSS_ATTR static ImuInputStatus sStatus;
static std::atomic<uint32_t> sStatusVersion{0};

static void statusWrite(const ImuInputStatus& next) {
  sStatusVersion.fetch_add(1);
  sStatus = next;
  sStatusVersion.fetch_add(1);
}

static ImuInputStatus statusRead() {
  ImuInputStatus copy;
  for (int tries = 0; tries < 8; ++tries) {
    const uint32_t before = sStatusVersion.load();
    copy = sStatus;
    if (!(before & 1u) && sStatusVersion.load() == before) break;
  }
  return copy;
}

static imuinput::Config configFromSettings() {
  imuinput::Config cfg;
  cfg.tapWindowMs = static_cast<uint16_t>(constrain(gSettings.imuInputTapWindowMs, 200, 800));
  cfg.tapMinGapMs = static_cast<uint16_t>(constrain(gSettings.imuInputTapMergeMs, 0, 300));
  cfg.engageDeg = static_cast<float>(constrain(gSettings.imuInputTiltDeg, 8, 40));
  cfg.releaseDeg = cfg.engageDeg - 4.0f;
  cfg.fullDeg = cfg.engageDeg + 15.0f;
  const input_rotation::Mount mount = input_rotation::mountFromCode(gSettings.imuInputMount);
  cfg.mountTurns = static_cast<uint8_t>(mount.quarterTurns);
  cfg.mountMirror = mount.mirror;
  cfg.shakeCalibrate = gSettings.imuInputShakeCalibrate;
  // The configuration teaches the side and the top (face) when the
  // firmware's knock detector finds the taps (their peak jumps come with
  // them); a chip's own tap engine gives none, and the configuration is the
  // tilt centre only. (The mount teach step is no longer offered after it:
  // owner, 2026-11-23; its face tap would be a face-stage knock.)
#if ENABLE_IMU_KNOCK_DETECTOR
  cfg.teachKnocks = knockFeedActive();
#else
  cfg.teachKnocks = false;
#endif
  return cfg;
}

// ---- The hook ---------------------------------------------------------------
// On the IMU task, once per sample (HAL_MotionHookCore.h has the contract):
// no waiting, no logging, a few hundred bytes of stack. The input reading is
// published with no wait for its lock; presses that could not be handed over
// go with the next engine update, 20 ms later. A feed faster than 20 ms is
// decimated first (sDecimator), its taps carried to the next update. A deflection is never left behind
// when the samples stop: the input layer treats a reading older than a
// quarter of a second as centred (HAL_Input.cpp).
// The configuration's prompts: one toast, updated in place as it goes
// (kConfigToastTag; the main loop's final "Configured" line too), and a log
// line per stage.
constexpr uint32_t kConfigToastTag = 0x494D5543u;  // "IMUC"

static void configToast(const char* text, uint32_t ms, bool done, bool warn) {
  INFO_INPUT_LIFECYCLEF("[IMU input] %s", text);
#if ENABLE_OLED_DISPLAY
  oledToastUpdate(kConfigToastTag, text, ms,
                  warn ? OledUiIcon::WARNING : done ? OledUiIcon::SUCCESS : OledUiIcon::INFO);
#else
  (void)ms;
  (void)done;
  (void)warn;
#endif
}

// How the last shake configuration ended (imuinputread's lastConfig).
enum : uint8_t {
  kLastConfigNone = 0,
  kLastConfigDone,        // configured: the new values are in use
  kLastConfigShake,       // a shake cancelled it
  kLastConfigTimeout,     // a knock stage ran out of time
  kLastConfigNeverStill,  // it was never set down still
};
static const char* lastConfigWord(uint8_t v) {
  switch (v) {
    case kLastConfigDone: return "configured";
    case kLastConfigShake: return "cancelled";
    case kLastConfigTimeout: return "timeout";
    case kLastConfigNeverStill: return "never-still";
    default: return "none";
  }
}

// One configuration event (Output::calEvent) on the screen. msLeft: the knock
// stage's time left. With the knock stages, "Configured" comes from the main
// loop once both surfaces are saved (imuInputTick).
static void showConfiguration(const imuinput::Output& out, uint32_t msLeft, bool taught) {
  char text[72];
  const unsigned knocks = knock::kTeachKnocks;
  switch (out.calEvent) {
    case imuinput::kCalStarted:
    case imuinput::kCalMoved:
      configToast("Set the sign down and hold still", imuinput::kCalTimeoutMs, false, false);
      return;
    case imuinput::kCalCountdown:
      snprintf(text, sizeof(text), "Hold still: %u", (unsigned)out.calSecondsLeft);
      configToast(text, 1500, false, false);
      return;
    case imuinput::kCalSideStart:
      snprintf(text, sizeof(text), "Knock the side %u times", knocks);
      configToast(text, knock::kTeachTimeoutMs + 500, false, false);
      return;
    case imuinput::kCalSideKnock:
      snprintf(text, sizeof(text), "Side: %u of %u", (unsigned)out.calKnocks, knocks);
      configToast(text, msLeft + 500, false, false);
      return;
    case imuinput::kCalFaceStart:
      snprintf(text, sizeof(text), "Knock the top %u times", knocks);
      configToast(text, knock::kTeachTimeoutMs + 500, false, false);
      return;
    case imuinput::kCalFaceKnock:
      snprintf(text, sizeof(text), "Top: %u of %u", (unsigned)out.calKnocks, knocks);
      configToast(text, msLeft + 500, false, false);
      return;
    case imuinput::kCalDone:
      if (!taught) configToast("Configured", 3000, true, false);
      return;
    case imuinput::kCalCancelled:
      configToast("Configuration cancelled: it never held still", 5000, true, true);
      return;
    case imuinput::kCalTimedOut:
      snprintf(text, sizeof(text), "No %u knocks on the %s in %u s: configuration cancelled", knocks,
               out.calStage == imuinput::kStageFace ? "top" : "side", (unsigned)(knock::kTeachTimeoutMs / 1000));
      configToast(text, 5000, true, true);
      return;
    case imuinput::kCalShakeCancelled:
      configToast("Configuration cancelled", 4000, true, true);
      return;
    default:
      return;
  }
}

// How the last teach step ended (imuinputread's lastLearn).
enum : uint8_t { kLastLearnNone = 0, kLastLearnLearned, kLastLearnTimeout, kLastLearnCancelled };
static const char* lastLearnWord(uint8_t v) {
  switch (v) {
    case kLastLearnLearned: return "learned";
    case kLastLearnTimeout: return "timeout";
    case kLastLearnCancelled: return "cancelled";
    default: return "none";
  }
}

// The teach step's prompts and outcome, in the calibration's toast.
static void showLearn(uint8_t event, int code) {
  char text[64];
  uint32_t ms = 3000;
  switch (event) {
    case imuinput::kLearnStarted:
      snprintf(text, sizeof(text), "Tilt the sign toward the top of the screen");
      ms = imuinput::kLearnTimeoutMs;
      break;
    case imuinput::kLearnDone:
      snprintf(text, sizeof(text), "Tilt learned: mount %s", input_rotation::mountName(code));
      break;
    case imuinput::kLearnTimedOut:
      snprintf(text, sizeof(text), "No tilt: mount unchanged");
      break;
    case imuinput::kLearnCancelled:
      snprintf(text, sizeof(text), "Teach step cancelled: mount unchanged");
      break;
    default:
      return;
  }
  INFO_INPUT_LIFECYCLEF("[IMU input] %s", text);
#if ENABLE_OLED_DISPLAY
  oledToastUpdate(kConfigToastTag, text, ms,
                  event == imuinput::kLearnDone        ? OledUiIcon::SUCCESS
                  : event == imuinput::kLearnTimedOut || event == imuinput::kLearnCancelled ? OledUiIcon::WARNING
                                                                                            : OledUiIcon::INFO);
#endif
}

static void imuInputHook(const MotionHookSample& sample, void*) {
  ImuInputStatus status = sStatus;  // only this function writes it
  status.ms = sample.ms;
  if (sample.stopped) {
    gInputConnected = false;
    status.imuStopped = true;
    status.joyX = status.joyY = JOYSTICK_CENTER;
    status.held = 0;
    statusWrite(status);
    return;
  }
  status.imuStopped = false;
  if (!sample.hasHwTap || !sample.hasGravity) {
    gInputConnected = false;
    status.unsupported = true;
    statusWrite(status);
    return;
  }
  status.unsupported = false;

  imuinput::Sample raw;
  raw.ms = sample.ms;
  for (int i = 0; i < 3; ++i) {
    raw.up[i] = sample.gravity[i];
    raw.accel[i] = sample.accel[i];
  }
  raw.tap = sample.tapLatched;
  raw.tapKind = sample.tapKind;
  for (int i = 0; i < 3; ++i) raw.tapVec[i] = sample.tapVec[i];
  imuinput::Sample in;
  if (!sDecimator.add(raw, imuinput::decimationFor(motionBackendInfo().samplePeriodMs), in)) {
    gInputConnected = true;  // samples arrive; the engine takes the next one
    statusWrite(status);
    return;
  }
  if (sRecentreRequest.exchange(false)) sEngine.recentre();
  imuinput::Output learnStart;
  if (sLearnRequest.exchange(false)) sEngine.startLearn(sample.ms, &learnStart);
  const uint8_t tapsBefore = sEngine.tapCount();
  const imuinput::Config cfg = configFromSettings();
  const imuinput::Output out = sEngine.update(in, cfg);

  sUnpublishedPresses |= out.pressed;
  sUnpublishedActivity |= out.activity;
  // Buttons at the input layer's logical positions, active low like every backend.
  if (inputPublishState(~static_cast<uint32_t>(out.held), out.joyX, out.joyY, sUnpublishedPresses,
                        sUnpublishedActivity, sample.ms, 0, 0)) {
    sUnpublishedPresses = 0;
    sUnpublishedActivity = false;
  }
  gInputConnected = true;

  status.joyX = static_cast<int16_t>(out.joyX);
  status.joyY = static_cast<int16_t>(out.joyY);
  status.held = out.held;
  status.tapCount = sEngine.tapCount();
  status.centred = sEngine.hasReference();
  status.centring = sEngine.capturing();
  status.shaking = sEngine.shaking();
  status.calibrating = sEngine.calibrating();
  status.calStage = sEngine.calStage();
  status.calKnocks = sEngine.calKnocks();
  status.drops = sEngine.drops();
  status.dropTaps = sEngine.lastDropTaps();
  status.dropFace = sEngine.lastDropFace();
  status.dropWhy = sEngine.lastDrop();
  status.learning = sEngine.learning();
  if (out.calEvent != imuinput::kCalNone) {
    const bool taught = sEngine.sideTeach().state == knock::TeachState::Done &&
                        sEngine.faceTeach().state == knock::TeachState::Done;
    switch (out.calEvent) {
#if ENABLE_IMU_KNOCK_DETECTOR
      case imuinput::kCalSideStart:
        knockFeedTeachCancelRecording();  // a step still recording would take the knocks; a finished one can still be saved
        break;
#endif
      case imuinput::kCalDone:
        status.lastConfig = kLastConfigDone;
        status.lastConfigStage = imuinput::kStageNone;
#if ENABLE_IMU_KNOCK_DETECTOR
        if (taught && !sConfigTaughtReady.load()) {  // the main loop saves them (never this task)
          sConfigTaught.side = sEngine.sideTeach();
          sConfigTaught.face = sEngine.faceTeach();
          sConfigTaughtReady.store(true);
        }
#endif
        break;
      case imuinput::kCalCancelled:
      case imuinput::kCalTimedOut:
      case imuinput::kCalShakeCancelled:
        status.lastConfig = out.calEvent == imuinput::kCalCancelled ? kLastConfigNeverStill
                            : out.calEvent == imuinput::kCalTimedOut ? kLastConfigTimeout
                                                                       : kLastConfigShake;
        status.lastConfigStage = out.calStage;
        break;
      default:
        break;
    }
    showConfiguration(out, sEngine.calMsLeft(in.ms), taught && cfg.teachKnocks);
  }
  const uint8_t learnEvent = learnStart.learnEvent ? learnStart.learnEvent : out.learnEvent;
  if (learnEvent == imuinput::kLearnDone) {
    // Up on the screen as it is seen: the screen's rotation is applied after
    // the mount, so it is part of the answer; the mirror is kept.
    const bool mirror = input_rotation::mountFromCode(gSettings.imuInputMount).mirror;
    const int turns = input_rotation::learnQuarterTurns(static_cast<input_rotation::Dir>(out.learnDir), mirror,
                                                        inputScreenQuarterTurns());
    const int code = input_rotation::mountCode(turns * 90, mirror);
    sLearnedMount.store(code);
    status.lastLearn = kLastLearnLearned;
    showLearn(learnEvent, code);
  } else if (learnEvent != imuinput::kLearnNone) {
    if (learnEvent == imuinput::kLearnTimedOut) status.lastLearn = kLastLearnTimeout;
    if (learnEvent == imuinput::kLearnCancelled) status.lastLearn = kLastLearnCancelled;
    showLearn(learnEvent, 0);
  }
  status.leanX = sEngine.leanX();
  status.leanY = sEngine.leanY();
  if (out.pressed) {
    ++status.presses;
    status.lastPress = out.pressed;
    status.lastTaps = tapsBefore;
  }
  if (out.recentred) ++status.recentres;
  statusWrite(status);
}

// ---- Start and stop ---------------------------------------------------------
EXT_RAM_BSS_ATTR static char sStartWhy[176];

// Have the IMU opened through its normal queued start when it is not open.
// nullptr: it is open, opening or asked for. Otherwise why it cannot be.
static const char* askForImu() {
  if (gImuRunning || i2cSensorStartOutstanding(I2C_DEVICE_IMU, 1)) return nullptr;
  SensorStartRefusal why = SensorStartRefusal::None;
  if (requestSensorStart(I2C_DEVICE_IMU, "imu@input", &why)) return nullptr;
  if (why == SensorStartRefusal::AlreadyRunning || why == SensorStartRefusal::AlreadyQueued) return nullptr;
  snprintf(sStartWhy, sizeof(sStartWhy),
           "Error: IMU input needs the IMU, and it could not be opened: %s. Check it with 'imustatus', "
           "then retry openinput.",
           sensorStartRefusalText(why));
  return sStartWhy;
}

const char* imuInputStartReply() {
  if (!inputCacheInit()) return "Error: Input cache is not available. Reboot the device and retry openinput.";
  if (xSemaphoreTake(sStateMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return "Error: IMU input is busy. Retry openinput in a few seconds.";
  }
  const char* why = nullptr;
  if (gInputRunning) {
    // Open already. The IMU may have closed under it (repeated bus errors):
    // ask for it again, which is what a menu entry or 'openinput' is for.
    why = askForImu();
  } else if (!inputCacheReset(UINT32_MAX, JOYSTICK_CENTER, JOYSTICK_CENTER, true)) {
    why = "Error: IMU input is busy. Retry openinput in a few seconds.";
  } else {
    sEngine.reset();
    sDecimator.reset();
    sUnpublishedPresses = 0;
    sUnpublishedActivity = false;
    sRecentreRequest.store(false);
    statusWrite(ImuInputStatus{});
    inputAbstractionInit();
    if (!imuSampleHookRegister(imuInputHook, nullptr)) {
      why = "Error: Another consumer takes the IMU's samples, so IMU input cannot. Stop it first ('imustatus' "
            "shows sampleHook), then retry openinput.";
    } else if (!imuHold(kHolder)) {
      imuSampleHookUnregister(imuInputHook, nullptr);
      why = "Error: The IMU is closing or has too many users. Retry openinput in a few seconds.";
    } else {
      gInputRunning = true;
      gInputConnected = false;
      why = askForImu();
      if (why) {
        imuSampleHookUnregister(imuInputHook, nullptr);
        imuRelease(kHolder);
        gInputRunning = false;
      } else {
        INFO_INPUT_LIFECYCLEF("IMU input started (taps and tilt from the motion layer)");
      }
    }
  }
  xSemaphoreGive(sStateMutex);
  return why;
}

bool inputStartInternal() { return imuInputStartReply() == nullptr; }

bool imuInputStop() {
  if (xSemaphoreTake(sStateMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
  const bool wasRunning = gInputRunning;
  if (wasRunning) {
    // On return no hook call is in progress and none starts.
    imuSampleHookUnregister(imuInputHook, nullptr);
    imuRelease(kHolder);
    gInputRunning = gInputConnected = false;
    inputCacheReset(UINT32_MAX, JOYSTICK_CENTER, JOYSTICK_CENTER, true);
    INFO_INPUT_LIFECYCLEF("IMU input stopped");
  }
  xSemaphoreGive(sStateMutex);
  return wasRunning;
}

void imuInputRecentre() { sRecentreRequest.store(true); }

// ---- The reading as it travels ----------------------------------------------
// The Seesaw gamepad's button bits, active low: what a reader of the
// gamepad's shape expects (i2csensor_seesaw.h's GAMEPAD_BUTTON_*, which that
// header defines only in a gamepad build).
static uint32_t gamepadWireButtons(uint8_t held) {
  uint32_t pressed = 0;
  if (held & imuinput::kButtonA) pressed |= 1u << 5;
  if (held & imuinput::kButtonB) pressed |= 1u << 1;
  if (held & imuinput::kButtonX) pressed |= 1u << 6;
  if (held & imuinput::kButtonY) pressed |= 1u << 2;
  if (held & imuinput::kButtonStart) pressed |= 1u << 16;
  if (held & imuinput::kButtonSelect) pressed |= 1u << 0;  // the gamepad's SELECT (i2csensor_seesaw.h)
  return ~pressed;
}

int imuInputBuildDataJson(char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  InputCache reading;
  const bool have = inputCacheSnapshot(reading, 50);
  int pos = sensorEnvelopeBegin(buf, size, have && reading.dataValid, gInputConnected, have ? reading.lastUpdate : 0);
  if (pos == 0) return 0;
  const int x = have ? reading.joyX : 0, y = have ? reading.joyY : 0;
  const uint32_t buttons = have ? gamepadWireButtons(static_cast<uint8_t>(~reading.buttons)) : 0;
  const int n = snprintf(buf + pos, size - pos,
                         "," HW1_SK_Q(HW1_SK_X) "%d," HW1_SK_Q(HW1_SK_Y) "%d," HW1_SK_Q(HW1_SK_BUTTONS) "%lu}", x, y,
                         static_cast<unsigned long>(buttons));
  if (n < 0 || static_cast<size_t>(n) >= size - pos) return 0;
  return pos + n;
}

// ---- Commands ---------------------------------------------------------------
static const char* stateWord(const ImuInputStatus& s) {
  if (!gInputRunning) return "closed";
  if (s.unsupported) return "unsupported";
  if (s.imuStopped || !gImuRunning) return "imu-closed";
  if (!gInputConnected) return "starting";
  if (s.calibrating) return "calibrating";
  if (s.centring || !s.centred) return "centring";
  return "ready";
}

// imuinputread [json]: what the input makes of the motion right now.
const char* cmd_imuinputread(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String text = argsInput;
  text.trim();
  const bool json = argWantsJson(argsInput);
  if (text.length() > 0 && !json) return "Error: invalid arguments — Usage: imuinputread [json]";
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  const ImuInputStatus s = statusRead();
  const char* state = stateWord(s);
  const bool live = gInputRunning && gInputConnected;
  const int x = live ? s.joyX : JOYSTICK_CENTER, y = live ? s.joyY : JOYSTICK_CENTER;
  if (json) {
    snprintf(getDebugBuffer(), 1024,
             "{\"schema\":1,\"state\":\"%s\",\"running\":%s,\"connected\":%s,\"x\":%d,\"y\":%d,\"buttons\":%u,"
             "\"leanX\":%.1f,\"leanY\":%.1f,\"taps\":%u,\"shaking\":%s,\"presses\":%lu,\"recentres\":%lu,"
             "\"lastPress\":%u,\"lastTaps\":%u,\"drops\":%lu,\"lastDrop\":{\"taps\":%u,\"hit\":\"%s\",\"why\":\"%s\"},\"tapWindowMs\":%d,\"tiltDeg\":%d,\"shakeCalibrate\":%s,\"mount\":%d,\"mirror\":%s,\"learning\":%s,\"lastLearn\":\"%s\","
             "\"configStage\":\"%s\",\"configKnocks\":%u,\"lastConfig\":\"%s\",\"lastConfigStage\":\"%s\"}",
             state, gInputRunning ? "true" : "false", gInputConnected ? "true" : "false", x, y,
             static_cast<unsigned>(live ? s.held : 0), static_cast<double>(live ? s.leanX : 0.0f),
             static_cast<double>(live ? s.leanY : 0.0f), static_cast<unsigned>(live ? s.tapCount : 0),
             live && s.shaking ? "true" : "false", static_cast<unsigned long>(s.presses),
             static_cast<unsigned long>(s.recentres), static_cast<unsigned>(s.lastPress),
             static_cast<unsigned>(s.lastTaps), static_cast<unsigned long>(s.drops),
             static_cast<unsigned>(s.dropTaps), s.dropFace ? "face" : "side", imuinput::dropName(s.dropWhy),
             gSettings.imuInputTapWindowMs, gSettings.imuInputTiltDeg,
             gSettings.imuInputShakeCalibrate ? "true" : "false", input_rotation::mountDegrees(gSettings.imuInputMount),
             input_rotation::mountFromCode(gSettings.imuInputMount).mirror ? "true" : "false",
             s.learning ? "true" : "false", lastLearnWord(s.lastLearn),
             imuinput::stageName(live && s.calibrating ? s.calStage : imuinput::kStageNone),
             static_cast<unsigned>(live && s.calibrating ? s.calKnocks : 0), lastConfigWord(s.lastConfig),
             imuinput::stageName(s.lastConfigStage));
    return getDebugBuffer();
  }
  if (!gInputRunning) return "Error: IMU input is not running. Start it with 'openinput'.";
  if (s.unsupported) {
    return "Error: The IMU gives no taps or no direction of gravity yet, so it cannot be the input device. "
           "Check 'imustatus' (taps found by the firmware) and reopen the IMU ('closeinput', then 'openinput').";
  }
  if (s.imuStopped || !gImuRunning) {
    return "Error: The IMU is closed, so there are no taps or tilt to read. Open it with 'openimu' and check "
           "'imustatus'.";
  }
  if (!gInputConnected) return "[IMU input] Waiting for the IMU's first samples";
  const char* direction = s.joyX > JOYSTICK_CENTER   ? "right"
                          : s.joyX < JOYSTICK_CENTER ? "left"
                          : s.joyY > JOYSTICK_CENTER ? "up"
                          : s.joyY < JOYSTICK_CENTER ? "down"
                                                     : "centre";
  int n = snprintf(getDebugBuffer(), 1024,
                   "[IMU input] %s; lean right %+.1f up %+.1f deg; joystick %s (x %d, y %d); taps counted %u; "
                   "presses %lu; centre taken %lu time(s)",
                   state, static_cast<double>(s.leanX), static_cast<double>(s.leanY), direction,
                   static_cast<int>(s.joyX), static_cast<int>(s.joyY), static_cast<unsigned>(s.tapCount),
                   static_cast<unsigned long>(s.presses), static_cast<unsigned long>(s.recentres));
  if (s.calibrating && n > 0 && n < 1024) {
    if (s.calStage == imuinput::kStageSide || s.calStage == imuinput::kStageFace) {
      snprintf(getDebugBuffer() + n, 1024 - n, "; configuring: %s knocks %u of %u",
               s.calStage == imuinput::kStageSide ? "side" : "top", static_cast<unsigned>(s.calKnocks),
               static_cast<unsigned>(knock::kTeachKnocks));
    } else {
      snprintf(getDebugBuffer() + n, 1024 - n, "; configuring: %s", imuinput::stageName(s.calStage));
    }
  }
  return getDebugBuffer();
}

const char* cmd_imuinputzero(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String text = argsInput;
  text.trim();
  if (text.length() > 0) return "Error: invalid arguments — Usage: imuinputzero";
  if (!gInputRunning) return "Error: IMU input is not running. Start it with 'openinput'.";
  if (!gInputConnected) {
    return "Error: No samples are arriving from the IMU, so no pose can be taken. Check 'imustatus'.";
  }
  imuInputRecentre();
  return "[IMU input] Hold the device still: its present pose becomes the joystick's centre";
}

const char* cmd_imuinputtapms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String text = argsInput;
  text.trim();
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  if (text.length() > 0) {
    int ms = 0;
    if (!parseIntArg(text, ms)) return kCommandReplyNotANumber;
    if (ms < 200 || ms > 800) return "Error: The tap window must be 200..800 ms.";
    setSetting(gSettings.imuInputTapWindowMs, ms);
  }
  snprintf(getDebugBuffer(), 1024, "[IMU input] Tap window: %d ms (a button arrives this long after its last tap)",
           gSettings.imuInputTapWindowMs);
  return getDebugBuffer();
}

const char* cmd_imuinputtapmerge(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String text = argsInput;
  text.trim();
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  if (text.length() > 0) {
    int ms = 0;
    if (!parseIntArg(text, ms)) return kCommandReplyNotANumber;
    if (ms < 0 || ms > 300) return "Error: The tap merge must be 0..300 ms.";
    setSetting(gSettings.imuInputTapMergeMs, ms);
  }
  snprintf(getDebugBuffer(), 1024, "[IMU input] Tap merge: %d ms (tap flags closer than this are one knock)",
           gSettings.imuInputTapMergeMs);
  return getDebugBuffer();
}

const char* cmd_imuinputtilt(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String text = argsInput;
  text.trim();
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  if (text.length() > 0) {
    int degrees = 0;
    if (!parseIntArg(text, degrees)) return kCommandReplyNotANumber;
    if (degrees < 8 || degrees > 40) return "Error: The tilt dead zone must be 8..40 degrees.";
    setSetting(gSettings.imuInputTiltDeg, degrees);
  }
  snprintf(getDebugBuffer(), 1024,
           "[IMU input] Tilt dead zone: %d degrees. Leaning less than that from the centre pose does nothing; more "
           "moves the joystick (it lets go again below %d). Larger: steadier, needs a bigger lean. Smaller: more "
           "sensitive.",
           gSettings.imuInputTiltDeg, gSettings.imuInputTiltDeg - 4);
  return getDebugBuffer();
}

// One on/off (or toggle) setting: no value shows it.
static const char* boolSetting(bool& field, const String& argsInput, bool allowToggle, const char* onReply,
                               const char* offReply) {
  String arg = argsInput;
  arg.trim();
  if (arg.length() > 0) {
    bool value = false;
    if (allowToggle && arg.equalsIgnoreCase("toggle")) {
      value = !field;
    } else {
      const int parsed = parseBoolArg(arg);
      if (parsed < 0) return allowToggle ? "Error: Not an on/off value. Use on, off or toggle." : kCommandReplyNotOnOff;
      value = parsed == 1;
    }
    setSetting(field, value);
  }
  return field ? onReply : offReply;
}

const char* cmd_imuinputshake(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return boolSetting(gSettings.imuInputShakeCalibrate, argsInput, false,
                     "[IMU input] Shake: configure (set the device down and hold it still 3 s for the tilt "
                     "centre, knock the side 5 times, knock the top 5 times; a shake during it cancels it)",
                     "[IMU input] Shake: nothing");
}

// imuinputmount [0|90|180|270 [mirror]]: how the sensor sits against the
// panel at rotation 0, in the screen's terms (Input_RotationCore.h): mirrored
// first, then turned. The screen's own rotation (oledrotation) is applied
// after it. Replaces imuinputswapxy / imuinputinvertx / imuinputinverty.
const char* cmd_imuinputmount(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  String arg = argsInput;
  arg.trim();
  if (arg.length() > 0) {
    int code = 0;
    if (!input_rotation::parseMountArg(arg.c_str(), code)) {
      return "Error: The mount is 0, 90, 180 or 270, optionally followed by mirror. Usage: imuinputmount [0|90|180|270] [mirror]";
    }
    setSetting(gSettings.imuInputMount, code);
  }
  snprintf(getDebugBuffer(), 1024, "[IMU input] Mount: %s", input_rotation::mountName(gSettings.imuInputMount));
  return getDebugBuffer();
}

// Main loop: a mount the teach step learned, and the surfaces a finished
// shake configuration taught, are saved here (the hook runs on the IMU task
// and does not write settings). The configuration's centre is already in use.
void imuInputTick() {
  const int code = sLearnedMount.exchange(-1);
  if (code >= 0) setSetting(gSettings.imuInputMount, code);
#if ENABLE_IMU_KNOCK_DETECTOR
  if (sConfigTaughtReady.load()) {
    const ConfigTaught taught = sConfigTaught;
    sConfigTaughtReady.store(false);
    char msg[96];
    const bool warn = imuKnockSaveTaught(&taught.side, &taught.face, msg, sizeof(msg));
    configToast(msg, warn ? 8000 : 3000, true, warn);
  }
#endif
}

bool imuInputConfiguring() { return gInputRunning && statusRead().calibrating; }

// imuinputlearn: the teach step. Answers at once; the user then tilts the
// sign toward the top of the screen within 10 s and the lean sets
// imuinputmount (keeping its mirror). The outcome is on the screen (the toast)
// and in imuinputread (learning, lastLearn, mount); no lean, a shake or a
// configuration leaves the mount as it was.
const char* cmd_imuinputlearn(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String text = argsInput;
  text.trim();
  if (text.length() > 0) return "Error: invalid arguments — Usage: imuinputlearn";
  if (!gInputRunning) return "Error: IMU input is not running. Start it with 'openinput'.";
  if (!gInputConnected) return "Error: No samples are arriving from the IMU. Check 'imustatus'.";
  sLearnRequest.store(true);
  return "[IMU input] Tilt the device toward the top of the screen now (10 s). The result shows on the screen "
         "and in 'imuinputread' (lastLearn, mount).";
}

// The open/close/autostart commands are the input module's (HAL_Input.cpp).
const CommandEntry imuInputCommands[] = {
  { "imuinputread",    "IMU input: state, lean, joystick and taps. (add 'json' for JSON output)", false, cmd_imuinputread, "Usage: imuinputread [json]" },
  { "imuinputzero",    "Take the present pose as the joystick's centre (a shake's configuration takes it too, with prompts).", true, cmd_imuinputzero, "Usage: imuinputzero" },
  { "imuinputtapms",   "Tap window in ms: a button arrives this long after its last tap <200..800>", true, cmd_imuinputtapms, "Usage: imuinputtapms [200..800]" },
  { "imuinputtapmerge", "Tap flags closer than this many ms are one knock (its rebound) <0..300>", true, cmd_imuinputtapmerge, "Usage: imuinputtapmerge [0..300]" },
  { "imuinputtilt",    "Tilt dead zone in degrees: a smaller lean does nothing, a larger one moves the joystick <8..40>", true, cmd_imuinputtilt, "Usage: imuinputtilt [8..40]" },
  { "imuinputshake",   "A shake configures: hold still 3 s (tilt centre), knock the side 5x, the top 5x [on|off]", true, cmd_imuinputshake, "Usage: imuinputshake [on|off]" },
  { "imuinputmount",   "How the sensor sits, in the screen's terms: [0|90|180|270] [mirror]", true, cmd_imuinputmount, "Usage: imuinputmount [0|90|180|270] [mirror]" },
  { "imuinputlearn",   "Teach the mount: tilt the sign toward the top of the screen within 10 s", true, cmd_imuinputlearn, "Usage: imuinputlearn" },
};
const size_t imuInputCommandsCount = sizeof(imuInputCommands) / sizeof(imuInputCommands[0]);

// ---- Settings ---------------------------------------------------------------
// Only this device's entries; autostart is the input module's (HAL_Input.cpp).
static const char kImuMountOptions[] = INPUT_MOUNT_OPTIONS;
static const SettingEntry kImuInputSettingEntries[] = {
  { "imuInputTapWindowMs", SETTING_INT,  &gSettings.imuInputTapWindowMs, 450, 0, nullptr, 200, 800, "Tap window (ms)",       nullptr, false, nullptr, "imuinputtapms" },
  { "imuInputTapMergeMs",  SETTING_INT,  &gSettings.imuInputTapMergeMs,  0,   0, nullptr, 0,   300, "Tap merge (ms)",        nullptr, false, nullptr, "imuinputtapmerge" },
  { "imuInputTiltDeg",     SETTING_INT,  &gSettings.imuInputTiltDeg,      15, 0, nullptr, 8,   40,  "Tilt dead zone (degrees)", nullptr, false, nullptr, "imuinputtilt" },
  { "imuInputShakeCalibrate",  SETTING_BOOL, &gSettings.imuInputShakeCalibrate,    1, 0, nullptr, 0,   1,   "Shake configures (tilt + knocks)", nullptr, false, nullptr, "imuinputshake" },
  { "imuInputMount",       SETTING_INT,  &gSettings.imuInputMount,         0, 0, nullptr, 0,   271, "Mount (degrees, mirror)", kImuMountOptions, false, nullptr, "imuinputmount" },
};

static bool isImuInputConnected() { return gInputConnected; }

extern const SettingsModule imuInputSettingsModule = {
  "imuinput",
  "hardware.sensors.imuinput",
  kImuInputSettingEntries,
  sizeof(kImuInputSettingEntries) / sizeof(kImuInputSettingEntries[0]),
  isImuInputConnected,
  "IMU as the input device: taps are buttons, tilt is the joystick"
};

#endif  // ENABLE_IMU_INPUT
