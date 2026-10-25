#pragma once
// The Bluetooth link status every surface shows: the role running now (and the
// saved mode it differs from), the phone relay pipe and the app console, in the
// words of BLE_LinkStatusCore.h. One snapshot reads the live state once; the
// surfaces lay it out. Task context; never blocks. Nothing here prints an
// address, IP or passphrase.
#include "System_BuildConfig.h"
#include "BLE_LinkStatusCore.h"
#include <ArduinoJson.h>

#if ENABLE_BLUETOOTH
#include "Bluetooth.h"  // BleConsoleView

struct BleLinkStatus {
  hw1link::Role role = hw1link::Role::Off;
  hw1link::Role saved = hw1link::Role::Off;   // what the saved bleMode starts
  bool pipeBuilt = false;                     // ENABLE_BLE_PHONE_PIPE
  hw1link::PipeView pipe;
  bool pipePaired = false;                    // a pipe enrollment is saved
  bool pipeAutoReconnect = false;
  const char* pipeLabel = "";                 // the pipe's own static text
  hw1link::Console console = hw1link::Console::Off;
  BleConsoleView consoleView;
  hw1link::PhoneLinkInputs phone;             // the phone link block (pipe builds)
};

BleLinkStatus bleLinkStatus();
// The phone link's own reads for bleLinkStatus(): the pipe owner's summary, the
// console driver and its row's leaf snapshot, the console setting and the
// secret's kind (never its bytes). Lock-free.
#if ENABLE_BLE_PHONE_PIPE
hw1link::PhoneLinkInputs bleLinkPhoneInputs();
#else
inline hw1link::PhoneLinkInputs bleLinkPhoneInputs() { return {}; }
#endif
// Adds role, roleText, saved, savedText, savedDiffers, pipe, pipeText,
// pipeReason, pipePaired, console, consoleText, consoleClients and advertising.
// consoleClients and advertising are 0/false while the console is off or not
// here; on the phone link consoleClients counts its console sessions (the
// stream's row, and design A's slot once joined) only while they show as
// connected or signed in (the pipe ready), and advertising stays false.
void bleLinkStatusJson(JsonObject out, const BleLinkStatus& s);
// The "phoneLink" object (pipe builds; nothing otherwise): state, stateText,
// reason, paired, secret, secretText, console, consoleText, relay, streams
// {relay, console}, ended, lastEnd and endReasons {name: count, nonzero only}.
// The compact form (full false, `status json`) leaves out the texts and
// endReasons.
// The full form reads the end counts once more (ended, lastEnd and endReasons
// from one reading).
void bleLinkPhoneJson(JsonObject out, const BleLinkStatus& s, bool full);
// The block's text lines (hw1link::phoneLinkLines) with the end reasons; ""
// without the pipe. Returns the length written (at most n - 1).
int bleLinkPhoneLines(char* buf, size_t n, const BleLinkStatus& s);
#endif
