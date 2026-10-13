#pragma once
#include "Arduino.h"
struct Settings {
  bool bleRequireAuth = false;  // read by Bluetooth.cpp's command tail (test_ble_sc_policy)
  bool bleRequireSecureChannel = false;
  String bleSecureChannelSecret;
};
