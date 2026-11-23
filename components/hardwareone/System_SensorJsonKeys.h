#pragma once

// Inter-device sensor JSON keys: the one spelling of each key that a receiving
// device reads by name (2026-11-14 sweep, census-duplicates-12).
//
// A sensor driver's <x>BuildDataJson writes its reading as hand-built JSON text
// (snprintf, no JSON library); ESP-NOW carries that text verbatim to the
// controller, which caches it per device and sensor type (gRemoteSensorCache)
// and parses it again by key: getRemoteGpsData (System_ESPNow_Sensors.cpp)
// and the remote-sensor OLED screen (displayRemoteSensors,
// OLED_Mode_Network.cpp). Both sides take the key from here, so a rename on
// one side can no longer silently zero the other side's reading.
//
// Only keys that firmware on the receiving device reads by name live here. The
// other keys of the same payloads (accel, gyro, temp, volume, id, ...) are
// shown generically (formatRemoteSensorReadable, the web page's generic card)
// or not read at all, and stay spelled in their builder. The envelope opening
// {"valid":..,"connected":..,"ts":.. is owned by sensorEnvelopeBegin
// (System_I2C.cpp, see System_I2C.h).
//
// Macros, not constants: the senders splice them into snprintf format strings
// at compile time, so the format stays one literal the compiler checks, e.g.
//   "," HW1_SK_Q(HW1_SK_SATS) "%d"   ==   ",\"sats\":%d"
// and the receivers index with them: doc[HW1_SK_SATS]. Dependency-free on
// purpose (host tests paste or include it on its own).

// "\"<key>\":" — a quoted key and its colon, for the senders' format strings.
#define HW1_SK_Q(key) "\"" key "\":"

// GPS (REMOTE_SENSOR_GPS): gpsBuildDataJson, i2csensor_pa1010d.cpp.
#define HW1_SK_FIX         "fix"
#define HW1_SK_QUALITY     "quality"
#define HW1_SK_SATS        "sats"
#define HW1_SK_LAT         "lat"
#define HW1_SK_LON         "lon"
#define HW1_SK_ALT         "alt"
#define HW1_SK_SPEED       "speed"

// IMU (REMOTE_SENSOR_IMU): imuBuildDataJson, HAL_Motion.cpp. "ori" is an
// object holding the three angles in degrees; "mag" (x/y/z in uT) is present
// when the backend reads a magnetometer.
#define HW1_SK_ORI         "ori"
#define HW1_SK_YAW         "yaw"
#define HW1_SK_PITCH       "pitch"
#define HW1_SK_ROLL        "roll"
#define HW1_SK_MAG         "mag"

// Time of flight (REMOTE_SENSOR_TOF): tofBuildDataJson, i2csensor_vl53l4cx.cpp.
// "objects" is an array of detected targets; each carries these two keys.
#define HW1_SK_OBJECTS     "objects"
#define HW1_SK_DISTANCE_MM "distance_mm"
#define HW1_SK_STATUS      "status"

// FM radio (REMOTE_SENSOR_FMRADIO): fmRadioBuildDataJson, i2csensor_rda5807.cpp.
#define HW1_SK_FREQUENCY   "frequency"
#define HW1_SK_RSSI        "rssi"
#define HW1_SK_STATION     "station"

// Input (REMOTE_SENSOR_INPUT), one device per build: the gamepad
// (gamepadBuildDataJson, i2csensor_seesaw.cpp) sends x/y/buttons; the ANO
// encoder (anoEncoderBuildDataJson, i2csensor_ano_encoder.cpp) and the GPIO
// encoder (gpioEncoderBuildDataJson, Input_GPIOEncoder.cpp) send
// pos/axis/buttons. The receiver tells them apart by the presence of "pos".
#define HW1_SK_X           "x"
#define HW1_SK_Y           "y"
#define HW1_SK_BUTTONS     "buttons"
#define HW1_SK_POS         "pos"
#define HW1_SK_AXIS        "axis"
