// HAL_Motion.h - the motion sensor (IMU) layer: one reading contract and one
// API whichever chip the build carries.
//
// The device type stays I2C_DEVICE_IMU (feature "imu", commands "imu*"). One
// backend per build, chosen by IMU_DEVICE_TYPE (System_BuildConfig.h):
//   1 = BNO055        HAL_Motion_BNO055.cpp (fuses on the chip)
//   2 = Adafruit 4517 HAL_Motion_LSM6DSOX.cpp (LSM6DSOX + LIS3MDL, fused here
//                     with HAL_Motion_Fusion.h at 50 Hz)
// HAL_Motion.cpp owns the cache, the task, the start/stop lifecycle, the JSON
// builder, the action detector, the commands and the settings; a backend only
// starts its chip and reads one sample (HAL_MotionBackend.h).
#ifndef HAL_MOTION_H
#define HAL_MOTION_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stddef.h>

// One reading, always available for type-safe references. Units: accel m/s^2,
// gyro rad/s, mag uT, Euler degrees (yaw 0..360, pitch +-90, roll +-180),
// temp Celsius.
struct ImuCache {
  float accelX = 0.0, accelY = 0.0, accelZ = 0.0;
  float gyroX = 0.0, gyroY = 0.0, gyroZ = 0.0;
  float imuTemp = 0.0;
  float oriYaw = 0.0, oriPitch = 0.0, oriRoll = 0.0;  // Euler angles (degrees)
  float magX = 0.0, magY = 0.0, magZ = 0.0;           // when hasMag
  unsigned long imuLastUpdate = 0;
  bool imuDataValid = false;
  uint32_t imuSeq = 0;
  bool hasMag = false;          // the backend reads a magnetometer
  bool hasOrientation = false;  // ori is filled (the chip's or the layer's fusion)
  bool hasHeading = false;      // yaw is a heading from magnetic north, not a relative angle
  bool hasCal = false;          // cal carries the backend's calibration state
  uint8_t cal = 0;              // BNO055: CALIB_STAT (sys, gyro, accel, mag: 2 bits each, 3 = done)
  uint8_t backend = 0;          // kMotionBackend* (HAL_MotionCore.h); 0 = none
};

// The per-sample hook's types (HAL_MotionHookCore.h; this alias is the same one).
struct MotionHookSample;
using MotionSampleHookFn = void (*)(const MotionHookSample& sample, void* ctx);

// Values only: callers own their copy; the live cache and lock stay in the layer.
#if ENABLE_IMU_SENSOR

// Forward declarations
class String;
struct MotionActionState;
struct MotionActionSample;

// IMU watermark tracking
extern volatile UBaseType_t gImuWatermarkMin;
extern volatile UBaseType_t gImuWatermarkNow;

// IMU initialization handoff flags
extern volatile bool gImuInitRequested;
extern volatile bool gImuInitDone;
extern volatile bool gImuInitResult;

// IMU command handlers
const char* cmd_imu(const String& argsInput);
const char* cmd_imustart(const String& argsInput);
const char* cmd_imustop(const String& argsInput);
const char* cmd_imuactions(const String& argsInput);
const char* cmd_imustatus(const String& argsInput);
// IMU web view settings (read by the web Sensors page)
const char* cmd_imupollingms(const String& argsInput);
const char* cmd_imuewmafactor(const String& argsInput);
const char* cmd_imutransitionms(const String& argsInput);
const char* cmd_imuwebmaxfps(const String& argsInput);
// IMU device settings
const char* cmd_imudevicepollms(const String& argsInput);
const char* cmd_imutapthreshold(const String& argsInput);
const char* cmd_imutapshock(const String& argsInput);
const char* cmd_imutapquiet(const String& argsInput);
const char* cmd_imutapmaxms(const String& argsInput);
const char* cmd_imutapringms(const String& argsInput);
const char* cmd_imutaptrace(const String& argsInput);
const char* cmd_imutapsideaxis(const String& argsInput);
const char* cmd_imutapsideratio(const String& argsInput);
const char* cmd_imutapteach(const String& argsInput);
const char* cmd_imuorientationmode(const String& argsInput);
const char* cmd_imuorientationcorrection(const String& argsInput);
const char* cmd_imupitchoffset(const String& argsInput);
const char* cmd_imurolloffset(const String& argsInput);
const char* cmd_imuyawoffset(const String& argsInput);
const char* cmd_imuaddress(const String& argsInput);
#if ENABLE_IMU_LSM6DSOX
const char* cmd_imumagaddress(const String& argsInput);
#endif

// IMU state and control
extern bool gImuRunning;
extern bool gImuConnected;
extern unsigned long gImuLastStopTime;
extern TaskHandle_t gImuTaskHandle;

// Internal function called by queue processor
bool imuStartInternal();

// The IMU's address: the one it was opened at while it is open (a changed
// imuaddress applies at the next openimu), else its imuAddress setting
// through the backend's rule; and the chip name of this build's backend
// ("BNO055", "LSM6DSOX").
uint8_t imuDeviceAddress();
// A two-chip backend's second address the same way (the Adafruit 4517's
// LIS3MDL, imuMagAddress); 0 when the backend has one chip.
uint8_t imuMagDeviceAddress();
const char* imuBackendName();

// The action detector runs once per sample on the IMU task, on every sample
// whether or not it refreshes the cache (postEvents: the task posts the imu_*
// events). Other callers read its state with imuActionsSnapshot() and never
// run it, so renders do not count as samples.
void imuUpdateActions(const MotionActionSample& sample, bool postEvents = false);
bool imuActionsSnapshot(MotionActionState& out, uint32_t timeoutMs = 10);

// ---- Per-sample hook ---------------------------------------------------------
// The sample and the function type are HAL_MotionHookCore.h's (include it to
// read a sample).
// For a consumer that needs every sample (the IMU input device: taps as
// buttons, tilt as a joystick). One slot. The full contract is at the top of
// HAL_MotionHookCore.h; in short:
//   - The IMU task calls the hook once per sample that has a reading (50 Hz
//     on the Adafruit 4517, every imuDevicePollMs on the BNO055), after the
//     action detector has seen it, with no lock held.
//   - It runs on the IMU task's 4 KB stack: under about 512 bytes of its own,
//     never blocking, never opening or closing the IMU.
//   - Calls can pause without notice (a paused bus, failed reads, the first
//     second after an open): compare each sample's `ms` with the previous.
//   - When the IMU closes, for whatever reason, the hook gets one last call
//     with `stopped` true. The registration stays for the next open.
//
// imuSampleHookRegister: false when fn is null or another consumer has the
// slot. imuSampleHookUnregister: only with the fn and ctx that registered;
// true when this call removed the registration, false when it is not this
// consumer's (or another caller is removing it). Either way, when it returns
// no call into fn with this ctx is in progress and none will start, so what
// ctx points at may be freed. Called from the hook itself, it does not wait
// for its own call. A hook that blocks keeps unregister waiting (it logs a
// warning after half a second).
bool imuSampleHookRegister(MotionSampleHookFn fn, void* ctx);
bool imuSampleHookUnregister(MotionSampleHookFn fn, void* ctx);

// ---- Holders -----------------------------------------------------------------
// A consumer that needs the IMU to stay open holds it, by name (a string
// literal: "input"). While anyone holds it, 'closeimu' answers
// "Error: The IMU is in use by <who> ..." and closes nothing. Holding does
// not start the IMU: the holder starts it through the normal queued start
// ('openimu', or requestSensorStart(I2C_DEVICE_IMU)), and it still stops by
// itself after repeated bus failures (gImuRunning says so).
// imuHold: false when who is empty, holds already, the list is full (four
// holders), or a 'closeimu' accepted earlier is still on its way (the IMU is
// closing: hold again once it has closed). imuRelease: false when who was
// not holding.
// A hold taken while the IMU is closed, or one whose IMU stopped by itself,
// does not block 'closeimu' (there is nothing to close), and the hook's last
// call of a run says so (`stopped`).
bool imuHold(const char* who);
bool imuRelease(const char* who);
uint8_t imuHolderCount();

// JSON building. imuBuildDataJson is the reading as it travels (ESP-NOW,
// MQTT): its keys are a wire format. imuBuildLocalJson is the same reading
// for this device's own surfaces ('imuread json', the "imu" entry of
// 'sensors json', the web Sensors page), with two more keys at the end:
// "hdg" (yaw is a heading from magnetic north; false: it counts from the
// IMU's start) and "backend" (the chip, imuBackendName()).
int imuBuildDataJson(char* buf, size_t bufSize);
int imuBuildLocalJson(char* buf, size_t bufSize);

// Copy one reading; false clears out when uninitialized or the lock is busy.
// Success only means a copy was available; check imuDataValid where required.
bool imuCacheSnapshot(ImuCache& out, uint32_t timeoutMs = 10);
// Static cache data plus its mutex handle, for memory attribution (not RTOS allocation).
size_t imuCacheStorageBytes();

#else
// The IMU is not in this build. Its state reads as stopped and a start
// request fails, so callers need no build condition.
inline bool gImuRunning = false;
inline bool gImuConnected = false;
inline unsigned long gImuLastStopTime = 0;
inline TaskHandle_t gImuTaskHandle = nullptr;
inline bool imuStartInternal() { return false; }
struct MotionActionSample;
inline void imuUpdateActions(const MotionActionSample& sample, bool postEvents = false) {
  (void)sample;
  (void)postEvents;
}
inline bool imuCacheSnapshot(ImuCache& out, uint32_t timeoutMs = 10) {
  (void)timeoutMs;
  out = {};
  return false;
}
inline int imuBuildDataJson(char* buf, size_t bufSize) {
  if (buf && bufSize) buf[0] = '\0';
  return 0;
}
inline int imuBuildLocalJson(char* buf, size_t bufSize) { return imuBuildDataJson(buf, bufSize); }
// No IMU in this build: nothing to hook or to hold.
inline bool imuSampleHookRegister(MotionSampleHookFn fn, void* ctx) {
  (void)fn;
  (void)ctx;
  return false;
}
inline bool imuSampleHookUnregister(MotionSampleHookFn fn, void* ctx) {
  (void)fn;
  (void)ctx;
  return false;
}
inline bool imuHold(const char* who) {
  (void)who;
  return false;
}
inline bool imuRelease(const char* who) {
  (void)who;
  return false;
}
inline uint8_t imuHolderCount() { return 0; }
inline size_t imuCacheStorageBytes() { return 0; }
#endif // ENABLE_IMU_SENSOR

// Main loop: hands the saved taught surfaces (imutapteach) to the knock
// detector and saves a finished teach step; its progress on the screen.
#if ENABLE_IMU_KNOCK_DETECTOR
void imuKnockTick();
#include "HAL_Motion_KnockCore.h"
// Save finished teach steps and say how they turned out: one surface
// (imutapteach, the other null) or both (the shake configuration). Main loop
// or command task only (it writes settings). The report for the screen goes
// to msg (both: worded as the configuration's "Configured..." line); true
// when it is a warning. The surfaces are saved either way, as imutapteach
// always has.
bool imuKnockSaveTaught(const knock::Teacher* side, const knock::Teacher* face, char* msg, size_t len);
#else
inline void imuKnockTick() {}
#endif
#endif // HAL_MOTION_H
