#pragma once
#include "Arduino.h"
struct Settings {
  bool bleRequireAuth = false;  // read by BLE_Server.cpp's command tail (test_ble_sc_policy)
  bool bleRequireSecureChannel = false;
  String bleSecureChannelSecret;
};
extern Settings gSettings;
