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
#include "HAL_MotionHookCore.h"
#include "Input_IMUCore.h"
#include "System_Command.h"
#include "System_CommandRegistry.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_SensorJsonKeys.h"  // inter-device sensor JSON key names (receivers read the same)
#include "System_Settings.h"
#include "System_Utils.h"
#if ENABLE_OLED_DISPLAY
#include "OLED_UI.h"  // oledToastUpdate: the calibration prompts
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
static uint32_t sUnpublishedPresses = 0;   // presses the input reading has not taken yet
static bool sUnpublishedActivity = false;

static std::atomic<bool> sRecentreRequest{false};

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
  bool calibrating;     // a shake's calibration is waiting for the device to be still
  bool unsupported;     // the IMU's chip has no tap detection or no gravity
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
  cfg.swapXY = gSettings.imuInputSwapXY;
  cfg.invertX = gSettings.imuInputInvertX;
  cfg.invertY = gSettings.imuInputInvertY;
  cfg.shakeCalibrate = gSettings.imuInputShakeCalibrate;
  return cfg;
}

// ---- The hook ---------------------------------------------------------------
// On the IMU task, once per sample (HAL_MotionHookCore.h has the contract):
// no waiting, no logging, a few hundred bytes of stack. The input reading is
// published with no wait for its lock; presses that could not be handed over
// go with the next sample, 20 ms later. A deflection is never left behind
// when the samples stop: the input layer treats a reading older than a
// quarter of a second as centred (HAL_Input.cpp).
// The calibration's prompts: one toast, updated in place as it goes, and a
// log line per stage.
static void showCalibration(uint8_t event, uint8_t secondsLeft) {
  char text[64];
  uint32_t ms = 3000;
  switch (event) {
    case imuinput::kCalStarted:
    case imuinput::kCalMoved:
      snprintf(text, sizeof(text), "Calibrating tilt: set the sign down the way you will use it");
      ms = imuinput::kCalTimeoutMs;
      break;
    case imuinput::kCalCountdown:
      snprintf(text, sizeof(text), "Hold still: %u", (unsigned)secondsLeft);
      ms = 1500;
      break;
    case imuinput::kCalDone:
      snprintf(text, sizeof(text), "Tilt calibrated");
      break;
    case imuinput::kCalCancelled:
      snprintf(text, sizeof(text), "Calibration cancelled: it never held still");
      break;
    default:
      return;
  }
  INFO_INPUT_LIFECYCLEF("[IMU input] %s", text);
#if ENABLE_OLED_DISPLAY
  constexpr uint32_t kCalToastTag = 0x494D5543u;  // "IMUC"
  const OledUiIcon icon = event == imuinput::kCalDone        ? OledUiIcon::SUCCESS
                          : event == imuinput::kCalCancelled ? OledUiIcon::WARNING
                                                             : OledUiIcon::INFO;
  oledToastUpdate(kCalToastTag, text, ms, icon);
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

  if (sRecentreRequest.exchange(false)) sEngine.recentre();
  imuinput::Sample in;
  in.ms = sample.ms;
  for (int i = 0; i < 3; ++i) {
    in.up[i] = sample.gravity[i];
    in.accel[i] = sample.accel[i];
  }
  in.tap = sample.tapLatched;
  in.tapKind = sample.tapKind;
  const uint8_t tapsBefore = sEngine.tapCount();
  const imuinput::Output out = sEngine.update(in, configFromSettings());

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
  status.drops = sEngine.drops();
  status.dropTaps = sEngine.lastDropTaps();
  status.dropFace = sEngine.lastDropFace();
  status.dropWhy = sEngine.lastDrop();
  if (out.calEvent != imuinput::kCalNone) showCalibration(out.calEvent, out.calSecondsLeft);
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
             "\"lastPress\":%u,\"lastTaps\":%u,\"drops\":%lu,\"lastDrop\":{\"taps\":%u,\"hit\":\"%s\",\"why\":\"%s\"},\"tapWindowMs\":%d,\"tiltDeg\":%d,\"shakeCalibrate\":%s,\"swapXY\":%s,\"invertX\":%s,\"invertY\":%s}",
             state, gInputRunning ? "true" : "false", gInputConnected ? "true" : "false", x, y,
             static_cast<unsigned>(live ? s.held : 0), static_cast<double>(live ? s.leanX : 0.0f),
             static_cast<double>(live ? s.leanY : 0.0f), static_cast<unsigned>(live ? s.tapCount : 0),
             live && s.shaking ? "true" : "false", static_cast<unsigned long>(s.presses),
             static_cast<unsigned long>(s.recentres), static_cast<unsigned>(s.lastPress),
             static_cast<unsigned>(s.lastTaps), static_cast<unsigned long>(s.drops),
             static_cast<unsigned>(s.dropTaps), s.dropFace ? "face" : "side", imuinput::dropName(s.dropWhy),
             gSettings.imuInputTapWindowMs, gSettings.imuInputTiltDeg,
             gSettings.imuInputShakeCalibrate ? "true" : "false", gSettings.imuInputSwapXY ? "true" : "false",
             gSettings.imuInputInvertX ? "true" : "false", gSettings.imuInputInvertY ? "true" : "false");
    return getDebugBuffer();
  }
  if (!gInputRunning) return "Error: IMU input is not running. Start it with 'openinput'.";
  if (s.unsupported) {
    return "Error: This build's IMU chip does not detect taps or give the direction of gravity, so it cannot "
           "be the input device. IMU input needs the Adafruit 4517 (IMU_DEVICE_TYPE 2).";
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
  snprintf(getDebugBuffer(), 1024,
           "[IMU input] %s; lean right %+.1f up %+.1f deg; joystick %s (x %d, y %d); taps counted %u; "
           "presses %lu; centre taken %lu time(s)",
           state, static_cast<double>(s.leanX), static_cast<double>(s.leanY), direction, static_cast<int>(s.joyX),
           static_cast<int>(s.joyY), static_cast<unsigned>(s.tapCount), static_cast<unsigned long>(s.presses),
           static_cast<unsigned long>(s.recentres));
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
    if (degrees < 8 || degrees > 40) return "Error: The tilt angle must be 8..40 degrees.";
    setSetting(gSettings.imuInputTiltDeg, degrees);
  }
  snprintf(getDebugBuffer(), 1024, "[IMU input] Tilt angle: %d degrees from the centre pose moves the joystick",
           gSettings.imuInputTiltDeg);
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
                     "[IMU input] Shake: calibrate the tilt (set the device down, hold it still 3 s)",
                     "[IMU input] Shake: nothing");
}

const char* cmd_imuinputswapxy(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return boolSetting(gSettings.imuInputSwapXY, argsInput, true, "[IMU input] Tilt axes: swapped",
                     "[IMU input] Tilt axes: normal");
}

const char* cmd_imuinputinvertx(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return boolSetting(gSettings.imuInputInvertX, argsInput, true, "[IMU input] Left/right tilt: inverted",
                     "[IMU input] Left/right tilt: normal");
}

const char* cmd_imuinputinverty(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return boolSetting(gSettings.imuInputInvertY, argsInput, true, "[IMU input] Up/down tilt: inverted",
                     "[IMU input] Up/down tilt: normal");
}

// The open/close/autostart commands are the input module's (HAL_Input.cpp).
const CommandEntry imuInputCommands[] = {
  { "imuinputread",    "IMU input: state, lean, joystick and taps. (add 'json' for JSON output)", false, cmd_imuinputread, "Usage: imuinputread [json]" },
  { "imuinputzero",    "Take the present pose as the joystick's centre (a shake does the same, with a prompt).", true, cmd_imuinputzero, "Usage: imuinputzero" },
  { "imuinputtapms",   "Tap window in ms: a button arrives this long after its last tap <200..800>", true, cmd_imuinputtapms, "Usage: imuinputtapms [200..800]" },
  { "imuinputtapmerge", "Tap flags closer than this many ms are one knock (its rebound) <0..300>", true, cmd_imuinputtapmerge, "Usage: imuinputtapmerge [0..300]" },
  { "imuinputtilt",    "Tilt angle in degrees that moves the joystick <8..40>", true, cmd_imuinputtilt, "Usage: imuinputtilt [8..40]" },
  { "imuinputshake",   "A shake calibrates the tilt: set the device down, hold still 3 s [on|off]", true, cmd_imuinputshake, "Usage: imuinputshake [on|off]" },
  { "imuinputswapxy",  "Swap the tilt axes [on|off|toggle]", true, cmd_imuinputswapxy, "Usage: imuinputswapxy [on|off|toggle]" },
  { "imuinputinvertx", "Invert left/right tilt [on|off|toggle]", true, cmd_imuinputinvertx, "Usage: imuinputinvertx [on|off|toggle]" },
  { "imuinputinverty", "Invert up/down tilt [on|off|toggle]", true, cmd_imuinputinverty, "Usage: imuinputinverty [on|off|toggle]" },
};
const size_t imuInputCommandsCount = sizeof(imuInputCommands) / sizeof(imuInputCommands[0]);

// ---- Settings ---------------------------------------------------------------
// Only this device's entries; autostart is the input module's (HAL_Input.cpp).
static const SettingEntry imuInputSettingEntries[] = {
  { "imuInputTapWindowMs", SETTING_INT,  &gSettings.imuInputTapWindowMs, 450, 0, nullptr, 200, 800, "Tap window (ms)",       nullptr, false, nullptr, "imuinputtapms" },
  { "imuInputTapMergeMs",  SETTING_INT,  &gSettings.imuInputTapMergeMs,  0,   0, nullptr, 0,   300, "Tap merge (ms)",        nullptr, false, nullptr, "imuinputtapmerge" },
  { "imuInputTiltDeg",     SETTING_INT,  &gSettings.imuInputTiltDeg,      15, 0, nullptr, 8,   40,  "Tilt angle (degrees)",  nullptr, false, nullptr, "imuinputtilt" },
  { "imuInputShakeCalibrate",  SETTING_BOOL, &gSettings.imuInputShakeCalibrate,    1, 0, nullptr, 0,   1,   "Shake calibrates tilt",        nullptr, false, nullptr, "imuinputshake" },
  { "imuInputSwapXY",      SETTING_BOOL, &gSettings.imuInputSwapXY,        0, 0, nullptr, 0,   1,   "Swap tilt axes",        nullptr, false, nullptr, "imuinputswapxy" },
  { "imuInputInvertX",     SETTING_BOOL, &gSettings.imuInputInvertX,       0, 0, nullptr, 0,   1,   "Invert left/right tilt", nullptr, false, nullptr, "imuinputinvertx" },
  { "imuInputInvertY",     SETTING_BOOL, &gSettings.imuInputInvertY,       0, 0, nullptr, 0,   1,   "Invert up/down tilt",   nullptr, false, nullptr, "imuinputinverty" },
};

static bool isImuInputConnected() { return gInputConnected; }

extern const SettingsModule imuInputSettingsModule = {
  "imuinput",
  "hardware.sensors.imuinput",
  imuInputSettingEntries,
  sizeof(imuInputSettingEntries) / sizeof(imuInputSettingEntries[0]),
  isImuInputConnected,
  "IMU as the input device: taps are buttons, tilt is the joystick"
};

#endif  // ENABLE_IMU_INPUT
