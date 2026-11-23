#pragma once

#include "System_BuildConfig.h"
#include <stddef.h>
#include <stdint.h>

#if ENABLE_IMU_INPUT
// The IMU as the input device (INPUT_DEVICE_TYPE 5): taps are the buttons and
// tilt is the joystick. It has no chip and no task of its own: it takes every
// sample of the motion layer (HAL_Motion.h's per-sample hook), runs
// Input_IMUCore.h on it and publishes into the shared input reading, where
// the menus and the on-screen keyboard read it like a gamepad.
//
//   side taps: 1 = A   2 = B   3 = Y   4 = X
//   face taps: 2 = START   3 = SELECT
//   a sustained shake starts the configuration (imuinputshake): set it down
//   and hold still (the tilt centre), knock the side 5 times, knock the top
//   (face) 5 times; nothing changes unless every stage is done, and a shake
//   during it cancels it
//   tilt left / right / up / down from the centre pose = the joystick
//
// Either IMU backend: the firmware's knock detector finds the taps (the
// Adafruit 4517 at 416 Hz, the BNO055 at 100 Hz in a build with IMU input) and
// the backend gives the direction of gravity. While it is open it holds the
// IMU open ('closeimu' refuses and names "input").
class String;

// Start the input. nullptr: started (the IMU may still be opening; taps and
// tilt work from its first sample). Otherwise why not, as a command reply
// ("Error: ..."); the input is then stopped, except when it was running
// already and only the IMU could not be opened again.
const char* imuInputStartReply();
// The same as started or not, the form every input driver offers.
bool inputStartInternal();
// Stop the input and let go of the IMU (which stays open). False: it was not running.
bool imuInputStop();
// Take the present pose as the joystick's centre (the same as five taps).
void imuInputRecentre();

// The reading as it travels (ESP-NOW, MQTT, 'sensors json'): the gamepad's
// shape and button layout, {"valid","connected","ts","x","y","buttons"}, so
// every reader of a gamepad reads this device too.
int imuInputBuildDataJson(char* buf, size_t size);

const char* cmd_imuinputread(const String& argsInput);
const char* cmd_imuinputzero(const String& argsInput);
const char* cmd_imuinputtapms(const String& argsInput);
const char* cmd_imuinputtapmerge(const String& argsInput);
const char* cmd_imuinputtilt(const String& argsInput);
const char* cmd_imuinputshake(const String& argsInput);
const char* cmd_imuinputmount(const String& argsInput);
const char* cmd_imuinputlearn(const String& argsInput);
// Main loop: saves a mount the teach step learned (imuinputlearn) and the
// knock surfaces a finished shake configuration taught.
void imuInputTick();
// A shake configuration is under way (imutapteach waits for it).
bool imuInputConfiguring();
#else
inline void imuInputTick() {}
inline bool imuInputConfiguring() { return false; }
#endif
