#pragma once
// Android phone relay pipe, as the status surfaces show it (G2 lens Bluetooth
// page, OLED Bluetooth menu, web Bluetooth page and dashboard, SSE, CLI). The
// pipe and its enrollment stay with BLE_PhonePipe and BLE_Peers; this module
// only reads their public state, the way the glasses and ring surfaces read
// G2_Glasses and G2_Ring. Nothing here prints an address, IP or passphrase.
#include "System_BuildConfig.h"
#include "BLE_LinkStatusCore.h"
#include <Arduino.h>
#include <ArduinoJson.h>

#if ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE
struct PhoneStatusSnapshot {
  bool active = false;      // pipe owner running (with the glasses, or phone-only)
  bool standalone = false;  // the pipe owns the BLE host (phone-client mode)
  bool ready = false;       // authenticated pipe link up
  bool enrolled = false;    // an app-channel enrollment is saved
  bool autoReconnect = false;
  const char* state = "stopped";  // phonePipeStatus(): static text
  hw1link::PipeView pipe;         // the shared pipe words (BLE_LinkStatusCore.h)
};
// The pipe part of bleLinkStatus() (BLE_LinkStatus.h).
PhoneStatusSnapshot phoneStatusSnapshot();
// /api/ble/status "phone" object (schema 2); `phone status json` serializes the
// same. "text" is pipeLine ("ready", "fail: not found"); "pipe", "pipeText" and
// "pipeReason" are the pipe words, "console" and "consoleText" the app console;
// "state" keeps the pipe's own text for diagnostics.
bool phoneBuildStatusJson(JsonObject out);
// The same from a snapshot the caller already took (one reading per response).
struct BleLinkStatus;
bool phoneBuildStatusJson(JsonObject out, const BleLinkStatus& link);
// Main loop: pushes a compact `phone-status` SSE event ("p" the pipe id, "r"
// its reason) when the pipe's flags, setup step, enrolling or text change.
void phoneStatusTick();
// `phone status [json]`; a row of BLE_PhonePipe.cpp's phoneCommands. Text:
// pipe=<pipeLine> role=<roleId> paired=yes|no auto=on|off console=<consoleId>
// state='<pipe text>'.
const char* phoneStatusCommand(const String& args);
#else
inline void phoneStatusTick() {}
#endif
