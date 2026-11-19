#pragma once
// What the board knows about its phone path, for the LLM source and status text.
// Lock-free snapshot, no addresses. Implemented in BLE_PhonePipe.cpp. Pure: no
// Arduino, IDF or project includes, so host harnesses include it as is.
//
// The two functions are declared here once, never inline: BLE_PhonePipe.cpp
// defines them in every build (the pipe's own in a pipe build; Off/false in a
// build without the pipe or without Bluetooth), so a translation unit that
// includes only this header links against the one definition. Host harnesses
// that do not compile BLE_PhonePipe.cpp define their own.
#include <cstddef>
#include <cstdint>
#include <cstdio>
enum class PhonePathState : uint8_t {
  Up,          // READY and the app's relay is on
  RelayOff,    // READY, the app's relay is off (only the app can change it)
  Connecting,  // an attempt is queued or running
  Retrying,    // down, the scheduler will try again in retryInMs
  Held,        // down; only a user action brings it back (see hint)
  NotPaired,   // no enrolled phone / no secret
  Off,         // pipe not compiled, Bluetooth disabled, or console server owns the radio
};
enum class PhonePathHint : uint8_t {
  None,
  NotHeard,      // last scan heard other devices but not the phone
  RadioSilent,   // last scan heard nothing at all
  FoundNoLink,   // phone heard, link/handshake failed
  ClosedByUser,  // closephone
  AppTooOld,     // phone answered with a version RESET
  RadioFault,    // cleanup fault / stack lifecycle fault
  ConsoleServer, // openconsole owns Bluetooth
};
struct PhonePathView {
  PhonePathState state = PhonePathState::Off;
  PhonePathHint hint = PhonePathHint::None;
  uint32_t retryInMs = 0;   // Retrying only (Held + AppTooOld: until the next version probe)
  uint8_t attempt = 0;      // attempts in this outage (0 when Up)
};
PhonePathView phonePipePathView();
// Demand: a caller needs the phone now. It brings the next attempt forward and
// never cancels one in flight; a connected (READY) link is left alone.
// Rate-limited to one call per 3 s. Returns true when the view is (or becomes)
// Connecting/Retrying.
bool phonePipeNudgeReconnect();
inline bool phonePathRecoverable(const PhonePathView& v) {
  return v.state == PhonePathState::Connecting || v.state == PhonePathState::Retrying;
}
namespace hw1path {
// Whole seconds, rounded up, so "retrying in 0 s" only shows when it is due.
inline unsigned ceilSeconds(uint32_t ms) { return unsigned((uint64_t(ms) + 999u) / 1000u); }
inline unsigned ceilMinutes(uint32_t ms) { return unsigned((uint64_t(ms) + 59999u) / 60000u); }
inline size_t finish(int n, char* out, size_t cap) {
  if (n < 0) { out[0] = 0; return 0; }
  return size_t(n) < cap ? size_t(n) : cap - 1;
}
}  // namespace hw1path
// The one status text every surface uses. Returns the length written (the text
// is cut to cap-1 characters when cap is too small). ASCII only.
inline size_t phonePathText(const PhonePathView& v, char* out, size_t cap) {
  if (!out || !cap) return 0;
  const unsigned s = hw1path::ceilSeconds(v.retryInMs);
  int n = 0;
  switch (v.state) {
    case PhonePathState::Up: n = snprintf(out, cap, "phone relay ready"); break;
    case PhonePathState::RelayOff: n = snprintf(out, cap, "relay off in the app"); break;
    case PhonePathState::Connecting: n = snprintf(out, cap, "connecting to phone"); break;
    case PhonePathState::Retrying:
      switch (v.hint) {
        case PhonePathHint::NotHeard:
          n = snprintf(out, cap, "phone not heard (out of range, or phone Bluetooth/app off?), retrying in %u s", s);
          break;
        case PhonePathHint::RadioSilent:
          n = snprintf(out, cap, "radio heard nothing, retrying in %u s", s); break;
        case PhonePathHint::FoundNoLink:
          n = snprintf(out, cap, "phone found but link failed, retrying in %u s", s); break;
        default: n = snprintf(out, cap, "phone link down, retrying in %u s", s); break;
      }
      break;
    case PhonePathState::Held:
      switch (v.hint) {
        case PhonePathHint::ClosedByUser:
          n = snprintf(out, cap, "phone link closed (openphone to reconnect)"); break;
        case PhonePathHint::AppTooOld:
          n = snprintf(out, cap, "phone app too old: update it (next check in %u min)",
                       hw1path::ceilMinutes(v.retryInMs));
          break;
        case PhonePathHint::RadioFault:
          n = snprintf(out, cap, "Bluetooth fault: restart may be needed"); break;
        case PhonePathHint::ConsoleServer:
          n = snprintf(out, cap, "console server owns Bluetooth (closeconsole)"); break;
        default: n = snprintf(out, cap, "phone link down (auto-reconnect off; openphone)"); break;
      }
      break;
    case PhonePathState::NotPaired: n = snprintf(out, cap, "no phone paired (phone pair new)"); break;
    case PhonePathState::Off:
      n = v.hint == PhonePathHint::ConsoleServer
              ? snprintf(out, cap, "console server owns Bluetooth (closeconsole)")
              : snprintf(out, cap, "Bluetooth off");
      break;
  }
  return hw1path::finish(n, out, cap);
}
// The short form for 21-column OLED lines (at most 21 characters). ASCII only.
inline size_t phonePathShortText(const PhonePathView& v, char* out, size_t cap) {
  if (!out || !cap) return 0;
  unsigned s = hw1path::ceilSeconds(v.retryInMs);
  if (s > 999) s = 999;  // "link fail, retry 999s" is the widest: 21 columns
  int n = 0;
  switch (v.state) {
    case PhonePathState::Up: n = snprintf(out, cap, "relay ready"); break;
    case PhonePathState::RelayOff: n = snprintf(out, cap, "relay off"); break;
    case PhonePathState::Connecting: n = snprintf(out, cap, "connecting"); break;
    case PhonePathState::Retrying:
      switch (v.hint) {
        case PhonePathHint::NotHeard: n = snprintf(out, cap, "no phone, retry %us", s); break;
        case PhonePathHint::RadioSilent: n = snprintf(out, cap, "no radio, retry %us", s); break;
        case PhonePathHint::FoundNoLink: n = snprintf(out, cap, "link fail, retry %us", s); break;
        default: n = snprintf(out, cap, "down, retry %us", s); break;
      }
      break;
    case PhonePathState::Held:
      switch (v.hint) {
        case PhonePathHint::ClosedByUser: n = snprintf(out, cap, "closed"); break;
        case PhonePathHint::AppTooOld: n = snprintf(out, cap, "app old"); break;
        case PhonePathHint::RadioFault: n = snprintf(out, cap, "BT fault"); break;
        case PhonePathHint::ConsoleServer: n = snprintf(out, cap, "console srv"); break;
        default: n = snprintf(out, cap, "down, auto off"); break;
      }
      break;
    case PhonePathState::NotPaired: n = snprintf(out, cap, "not paired"); break;
    case PhonePathState::Off:
      n = v.hint == PhonePathHint::ConsoleServer ? snprintf(out, cap, "console srv")
                                                 : snprintf(out, cap, "BT off");
      break;
  }
  return hw1path::finish(n, out, cap);
}
