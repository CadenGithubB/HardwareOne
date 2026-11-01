# ES8311 codec driver

Apache-2.0 Espressif driver from Waveshare's LCD-5 playback example:
https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-5/tree/main/examples/arduino/examples/09_Audio_Playback
Retrieved 2026-11-01. Source copyright/SPDX headers are preserved.
The vendor port uses Arduino's numbered I2C HAL. Hardware One wraps codec
operations in its existing I2C device transaction and selects Wire1/I2C1.
I2S, amplifier ownership and cleanup belong to HAL_Speaker.cpp.
