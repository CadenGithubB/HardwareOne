#pragma once
// Bluetooth link status in the words every surface shows (OLED, lens, web, CLI,
// JSON): the role running now, the phone relay pipe and the app console, and
// the phone link block (its console stream, relay forwarding, session ends).
// - The relay pipe: the P4 is central to the GATT server the phone app hosts
//   (relay, LLM HTTP fallback, Terminal).
// - The app console: the phone app's client on the P4's command service (CLI
//   commands, device notifications). Server mode serves it alone, advertising;
//   with the pipe running it is the pipe's console stream (BLE_PhonePipeConsole.h),
//   and in G2 client mode also the command service on the pipe's own link
//   (design A, BLE_ConsoleAttachCore.h).
// Pure: callers gather the inputs, so the host test runs this file as is. See
// docs/PHONE_CONSOLE_VS_PIPE.md.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace hw1link {

// ---- Role ---------------------------------------------------------------------
enum class Role : uint8_t { Off, Console, Glasses, GlassesPipe, PhonePipe };

struct RoleInputs {
  bool server = false;        // the console GATT server is up
  bool glasses = false;       // the G2 client is up
  bool pipe = false;          // the phone pipe is running
  bool pipeOwnsHost = false;  // legacy: the pipe alone on the host; same reading as pipe without glasses
};

// The running role: the devices open on the shared client stack, or the
// console server. A pipe with no glasses runtime beside it is the phone-pipe
// role whether or not a ring is open; the ring has no role word of its own.
inline Role roleOf(const RoleInputs& in) {
  if (in.server) return Role::Console;
  if (in.pipe && (in.pipeOwnsHost || !in.glasses)) return Role::PhonePipe;
  if (in.glasses) return in.pipe ? Role::GlassesPipe : Role::Glasses;
  return Role::Off;
}

// The saved boot role bleMode (0 server, 1 the client stack) as the role it
// starts. The client stack opens the glasses and, when the pipe is built, the
// phone; which of them comes up is each peer's auto-reconnect setting.
inline Role roleOfMode(int mode, bool pipeBuilt) {
  switch (mode) {
    case 0: return Role::Console;
    case 1: return pipeBuilt ? Role::GlassesPipe : Role::Glasses;
    default: return Role::Off;
  }
}

inline const char* roleText(Role r) {
  switch (r) {
    case Role::Console: return "App console";
    case Role::Glasses: return "Glasses";
    case Role::GlassesPipe: return "Glasses+pipe";
    case Role::PhonePipe: return "Phone pipe";
    case Role::Off: break;
  }
  return "Off";
}

inline const char* roleId(Role r) {
  switch (r) {
    case Role::Console: return "console";
    case Role::Glasses: return "glasses";
    case Role::GlassesPipe: return "glasses-pipe";
    case Role::PhonePipe: return "phone-pipe";
    case Role::Off: break;
  }
  return "off";
}

// The saved role differs from what runs: the glasses with or without the pipe
// and the pipe alone are all the client stack (the saved role "client"), so
// they count as one. Off always shows the saved role it would start.
inline bool savedDiffers(Role running, Role saved) {
  auto family = [](Role r) {
    return (r == Role::GlassesPipe || r == Role::PhonePipe) ? Role::Glasses : r;
  };
  return running == Role::Off || family(running) != family(saved);
}

// "Glasses+pipe", or "Phone pipe (saved: App console)", or "Off (saved: Glasses)".
inline const char* roleLine(Role running, Role saved, char* buf, size_t n) {
  if (!buf || !n) return "";
  if (savedDiffers(running, saved)) {
    snprintf(buf, n, "%s (saved: %s)", roleText(running), roleText(saved));
  } else {
    snprintf(buf, n, "%s", roleText(running));
  }
  return buf;
}

// ---- Relay pipe -------------------------------------------------------------------
// Mirrors BLE_PhonePipe.cpp's setup step (phonePipeStep()).
enum class PipeStep : uint8_t { Idle, Waiting, ScanParams, Scanning, Link, Retiring, Fault };

enum class Pipe : uint8_t {
  NotBuilt, Off, NoSecret, NotPaired, Pairing, Waiting, Searching, Linking,
  Ready, Closing, Idle, Failed, Fault
};

struct PipeInputs {
  bool built = false;
  bool active = false;     // the pipe owner runs
  bool ready = false;      // authenticated pipe link up
  bool enrolling = false;  // `phone pair` in progress, or `phone pair new`'s window open
  bool enrolled = false;   // a pipe enrollment is saved
  PipeStep step = PipeStep::Idle;
  const char* label = nullptr;  // phonePipeStatus(): the pipe's own text
};

struct PipeView {
  Pipe phase = Pipe::Off;
  const char* reason = nullptr;  // short failure reason, or nullptr
};

// The pipe's labels that mean nothing went wrong.
inline bool pipeLabelNeutral(const char* label) {
  static const char* const kNeutral[] = {
    "stopped", "disconnected", "ready", "scanning", "authenticating pipe",
    "waiting for radio", "waiting to enroll", "phone close requested",
  };
  if (!label || !label[0]) return true;
  for (const char* k : kNeutral) if (strcmp(label, k) == 0) return true;
  return false;
}

// A failure label as a short reason of at most 9 characters ("not found",
// "timed out", ...), so "Pipe: fail: <reason>" fits 21 OLED columns. Neutral
// labels give nullptr; a label this table does not know gives "error", so a new
// failure is never shown as success (the host test pins every label in the pipe).
inline const char* pipeReason(const char* label) {
  struct Row { const char* label; const char* reason; };
  static const Row kReasons[] = {
    {"phone not found", "not found"},
    {"BLE setup timed out", "timed out"},
    {"BLE setup turn overran", "timed out"},
    {"phone link lost", "link lost"},
    {"scan setup failed", "scan fail"},
    {"scan start failed", "scan fail"},
    {"GATT registration unavailable", "busy"},
    {"central link capacity unavailable", "no slot"},
    {"pipe authentication cooldown or secret unavailable", "auth fail"},
    {"phone enrollment expired", "expired"},
    {"phone intent changed", "cancelled"},
    {"phone intent replaced", "cancelled"},
    {"phone enrollment changed", "cancelled"},
    {"phone forget authority changed", "cancelled"},
    {"draining phone for forget", "cancelled"},
    {"setup turn ended unused", "busy"},
    {"stream cleanup could not be queued", "busy"},
    {"BLE callback queue full", "busy"},
    {"phone cleanup incomplete", "cleanup"},
    {"phone pipe enrollment forgotten", "forgotten"},
    {"configure phone secret first", "no secret"},
    // Version mismatch (phone link v2): a strike, and the third one's hold.
    {"phone app version mismatch", "version"},
    {"phone app too old", "app old"},
    // `phone pair new`'s window (phone link v2): it ran out, or was closed.
    {"pairing code expired", "expired"},
    {"pairing code closed", "cancelled"},
  };
  if (pipeLabelNeutral(label)) return nullptr;
  for (const Row& r : kReasons) if (strcmp(label, r.label) == 0) return r.reason;
  return "error";
}

inline PipeView pipeOf(const PipeInputs& in) {
  PipeView v;
  if (!in.built) { v.phase = Pipe::NotBuilt; return v; }
  if (in.ready) { v.phase = Pipe::Ready; return v; }
  const char* reason = pipeReason(in.label);
  if (!in.active) {
    v.phase = (in.label && strcmp(in.label, "configure phone secret first") == 0)
        ? Pipe::NoSecret : Pipe::Off;
    return v;
  }
  switch (in.step) {
    case PipeStep::Fault: v.phase = Pipe::Fault; v.reason = reason; return v;
    case PipeStep::Retiring: v.phase = Pipe::Closing; v.reason = reason; return v;
    case PipeStep::Waiting:
    case PipeStep::ScanParams:
    case PipeStep::Scanning:
    case PipeStep::Link:
      if (in.enrolling) { v.phase = Pipe::Pairing; return v; }
      v.phase = in.step == PipeStep::Waiting ? Pipe::Waiting
              : in.step == PipeStep::Link ? Pipe::Linking : Pipe::Searching;
      return v;
    case PipeStep::Idle:
      // Between the attempts of `phone pair new`'s window: still pairing.
      if (in.enrolling) { v.phase = Pipe::Pairing; return v; }
      break;
  }
  if (!in.enrolled) { v.phase = Pipe::NotPaired; return v; }
  v.phase = reason ? Pipe::Failed : Pipe::Idle;
  v.reason = reason;
  return v;
}

inline const char* pipeText(Pipe p) {
  switch (p) {
    case Pipe::NotBuilt: return "not in build";
    case Pipe::Off: return "off";
    case Pipe::NoSecret: return "no secret";
    case Pipe::NotPaired: return "not paired";
    case Pipe::Pairing: return "pairing";
    case Pipe::Waiting: return "waiting";
    case Pipe::Searching: return "searching";
    case Pipe::Linking: return "linking";
    case Pipe::Ready: return "ready";
    case Pipe::Closing: return "closing";
    case Pipe::Idle: return "idle";
    case Pipe::Failed: return "fail";
    case Pipe::Fault: return "fault";
  }
  return "off";
}

// At most 6 characters, for the lens status corner ("PH: ready").
inline const char* pipeBrief(Pipe p) {
  switch (p) {
    case Pipe::NotBuilt: return "none";
    case Pipe::Off: return "off";
    case Pipe::NoSecret: return "nokey";
    case Pipe::NotPaired: return "unpair";
    case Pipe::Pairing: return "pair";
    case Pipe::Waiting: return "wait";
    case Pipe::Searching: return "scan";
    case Pipe::Linking: return "link";
    case Pipe::Ready: return "ready";
    case Pipe::Closing: return "close";
    case Pipe::Idle: return "idle";
    case Pipe::Failed: return "fail";
    case Pipe::Fault: return "fault";
  }
  return "off";
}

inline const char* pipeId(Pipe p) {
  switch (p) {
    case Pipe::NotBuilt: return "not-built";
    case Pipe::NoSecret: return "no-secret";
    case Pipe::NotPaired: return "not-paired";
    default: return pipeText(p);
  }
}

// "ready", "fail: not found", "fault". Only a failure names its reason on the
// line ("Pipe: fail: not found" is the 21-column OLED limit); JSON carries the
// reason of a closing or faulted pipe too.
inline const char* pipeLine(const PipeView& v, char* buf, size_t n) {
  if (!buf || !n) return "";
  if (v.reason && v.phase == Pipe::Failed)
    snprintf(buf, n, "%s: %s", pipeText(v.phase), v.reason);
  else snprintf(buf, n, "%s", pipeText(v.phase));
  return buf;
}

// ---- App console ----------------------------------------------------------------
// Waiting and Hidden are the advertising server's; WaitingPhone and NoSecret the
// phone link's: its console stream (phone link v2, slot 2 of the pipe), or the
// client host's command service on the same link (design A, NoSecret).
enum class Console : uint8_t {
  NotHere, Off, Waiting, Hidden, Connected, SignedIn, WaitingPhone, NoSecret
};

struct ConsoleInputs {
  bool up = false;           // the console GATT server owns the host (server mode)
  bool advertising = false;
  uint8_t clients = 0;       // app centrals connected (peer links excluded)
  bool signedIn = false;     // one of them is logged in (client mode: the phone link's)
  // Client mode: the command service on the client host, on the pipe's link.
  bool registered = false;   // registered on this client host
  bool attachable = false;   // ... and the console setting is on
  bool attached = false;     // the app has joined the console slot on the phone pipe's link
  bool pipeReady = false;    // the pipe is authenticated and ready
  bool secret = false;       // blesecret is set: client mode is Secure Channel only
  // The console stream inside the pipe (phone link v2).
  bool streamOffered = false;   // this P4 offers it: bleconsole on and a device code
  bool streamOpen = false;      // open, its session row bound
  bool streamSignedIn = false;  // that row is logged in
  Role role = Role::Off;
};

inline Console consoleOf(const ConsoleInputs& in) {
  if (in.up) {
    if (in.clients) return in.signedIn ? Console::SignedIn : Console::Connected;
    return in.advertising ? Console::Waiting : Console::Hidden;
  }
  if (in.role == Role::Off || in.role == Role::Console) return Console::Off;
  // The pipe runs (only then can the stream be open or offered): the stream
  // comes first. A design-A slot the app has joined still shows (it keeps
  // working beside the stream); otherwise an offered stream waits for the
  // phone, and the phone pipe's own host has no other console.
  const bool pipeRole = in.role == Role::GlassesPipe || in.role == Role::PhonePipe;
  if (pipeRole && in.streamOpen && in.pipeReady) {
    return in.streamSignedIn ? Console::SignedIn : Console::Connected;
  }
  if (in.attachable && in.attached && in.pipeReady) {
    return in.signedIn ? Console::SignedIn : Console::Connected;
  }
  if (pipeRole && in.streamOffered) return Console::WaitingPhone;
  if (in.role == Role::PhonePipe) return Console::NotHere;
  // Design A: the command service exists only where it was registered on the
  // host (never without the pipe built). A slot the app has joined counts only
  // while the pipe is ready; until the app joins it the console waits.
  if (!in.attachable) return in.registered ? Console::Off : Console::NotHere;
  return in.secret ? Console::WaitingPhone : Console::NoSecret;
}

inline const char* consoleText(Console c) {
  switch (c) {
    case Console::NotHere: return "n/a here";
    case Console::Off: return "off";
    case Console::Waiting: return "waiting";
    case Console::Hidden: return "hidden";
    // The app is attached but nobody has logged in. Not "connected": on the lens
    // that read as usable while every command needed a login (2026-10-20).
    case Console::Connected: return "signed out";
    case Console::SignedIn: return "signed in";
    case Console::WaitingPhone: return "waiting";
    case Console::NoSecret: return "no secret";
  }
  return "off";
}

inline const char* consoleLongText(Console c) {
  switch (c) {
    case Console::NotHere: return "not available in this mode";
    case Console::Off: return "off";
    case Console::Waiting: return "waiting for the app";
    case Console::Hidden: return "not visible (advertising off)";
    case Console::Connected: return "app connected, signed out";
    case Console::SignedIn: return "app signed in";
    case Console::WaitingPhone: return "waiting for the phone link";
    case Console::NoSecret: return "set blesecret first";
  }
  return "off";
}

inline const char* consoleId(Console c) {
  switch (c) {
    case Console::NotHere: return "not-here";
    case Console::Connected: return "signed-out";
    case Console::SignedIn: return "signed-in";
    case Console::WaitingPhone: return "waiting-phone";
    case Console::NoSecret: return "no-secret";
    default: return consoleText(c);
  }
}

// ---- Phone link (v2) ------------------------------------------------------------
// The "Phone link" block of bleinfo, blestatus, the web Bluetooth page and
// status json: the link (the pipe words above), its console stream (slot 2),
// relay forwarding and how READY sessions ended. Counts and words only.

// Mirrors BLE_PhonePipeConsole.h's PipeConsoleState (BLE_LinkStatus.cpp pins it).
enum class StreamState : uint8_t { Off, NotOffered, Waiting, Opening, Open, Closing, Backoff, Stopped };

// The phone secret: a code `phone pair new` made on this device offers the
// console; a passphrase set with `phone secret` carries the relay only.
enum class Secret : uint8_t { None, Passphrase, Code };

inline const char* secretId(Secret s) {
  switch (s) {
    case Secret::Passphrase: return "passphrase";
    case Secret::Code: return "code";
    case Secret::None: break;
  }
  return "none";
}

inline const char* secretText(Secret s) {
  switch (s) {
    case Secret::Passphrase: return "passphrase";
    case Secret::Code: return "device code";
    case Secret::None: break;
  }
  return "no secret";
}

struct PhoneLinkInputs {
  bool known = false;           // the pipe owner has published a summary since boot
  bool ready = false;           // the session is READY
  bool relayOn = false;         // ... and the phone reports RELAY_ON
  bool consoleEnabled = false;  // bleconsole on
  bool consoleOffered = false;  // ... and a device code: this P4 offers the console
  StreamState stream = StreamState::Off;  // the console driver (READY sessions only)
  bool streamSignedIn = false;  // the open stream's row is logged in
  Secret secret = Secret::None;
  uint8_t relayStreams = 0, consoleStreams = 0;  // live streams by class
  uint32_t ended = 0;           // READY sessions ended since boot
  uint16_t lastEnd = 0;         // the last one's end reason (endReasonId)
};

// The console stream as the block shows it: the driver's state while a session
// is READY, else why it is not offered.
enum class LinkConsole : uint8_t {
  Off, NeedsCode, Waiting, NotWanted, Opening, Open, SignedIn, Closing, Backoff, Stopped
};

inline LinkConsole linkConsoleOf(const PhoneLinkInputs& in) {
  switch (in.stream) {
    case StreamState::Open: return in.streamSignedIn ? LinkConsole::SignedIn : LinkConsole::Open;
    case StreamState::Opening: return LinkConsole::Opening;
    case StreamState::Closing: return LinkConsole::Closing;
    case StreamState::Backoff: return LinkConsole::Backoff;
    case StreamState::Stopped: return LinkConsole::Stopped;
    case StreamState::Waiting: return LinkConsole::NotWanted;  // offered; the app has it off
    case StreamState::Off:
    case StreamState::NotOffered: break;
  }
  if (!in.consoleOffered) return in.consoleEnabled ? LinkConsole::NeedsCode : LinkConsole::Off;
  return LinkConsole::Waiting;
}

inline const char* linkConsoleText(LinkConsole c) {
  switch (c) {
    case LinkConsole::NeedsCode: return "needs a device code";
    case LinkConsole::Waiting: return "waiting for the link";
    case LinkConsole::NotWanted: return "not wanted by the app";
    case LinkConsole::Opening: return "opening";
    case LinkConsole::Open: return "open, signed out";
    case LinkConsole::SignedIn: return "signed in";
    case LinkConsole::Closing: return "closing";
    case LinkConsole::Backoff: return "backing off";
    case LinkConsole::Stopped: return "refused by the app";
    case LinkConsole::Off: break;
  }
  return "off";
}

inline const char* linkConsoleId(LinkConsole c) {
  switch (c) {
    case LinkConsole::NeedsCode: return "needs-code";
    case LinkConsole::Waiting: return "waiting";
    case LinkConsole::NotWanted: return "not-wanted";
    case LinkConsole::Opening: return "opening";
    case LinkConsole::Open: return "open";
    case LinkConsole::SignedIn: return "signed-in";
    case LinkConsole::Closing: return "closing";
    case LinkConsole::Backoff: return "backoff";
    case LinkConsole::Stopped: return "stopped";
    case LinkConsole::Off: break;
  }
  return "off";
}

// Relay forwarding as the phone reports it; unknown until the link is READY.
inline const char* relayId(const PhoneLinkInputs& in) {
  if (!in.ready) return "unknown";
  return in.relayOn ? "on" : "off";
}

// The pipe's stream slots by class (hw1pipe::kRelaySlots, and the console's one).
constexpr unsigned kRelayStreams = 2, kConsoleStreams = 1;

// READY sessions' end reasons, by the pipe error code (hw1pipe::Error) 0..18.
// 0 also collects codes the pipe does not know.
constexpr unsigned kEndReasons = 19;
inline const char* endReasonId(uint16_t code) {
  static const char* const kNames[kEndReasons] = {
    "other", "protocol", "version", "unauthorized", "target", "vpn", "dns", "connect",
    "connect-timeout", "busy", "resources", "tcp", "flow", "deadline", "cancelled",
    "display", "service-off", "unsupported", "paused",
  };
  return code < kEndReasons ? kNames[code] : kNames[0];
}

// The block's text lines (bleinfo, blestatus), no trailing newline:
//   Phone link: ready
//     Paired: yes (device code)
//     Console: signed in
//     Relay: off
//     Streams: relay 0/2, console 1/1
//     Ended: 3 (last: paused)
//     End reasons: cancelled=2 paused=1
// endReasons (kEndReasons counts) may be null: no End reasons line. Bounded;
// returns the length written (at most n - 1).
inline int phoneLinkLines(char* buf, size_t n, const PipeView& pipe, bool paired,
                          const PhoneLinkInputs& in, const uint32_t* endReasons) {
  if (!buf || !n) return 0;
  buf[0] = '\0';
  size_t used = 0;
  auto add = [&](int w) {
    if (w > 0) used = used + size_t(w) < n ? used + size_t(w) : n - 1;
  };
  char line[24];
  add(snprintf(buf + used, n - used, "Phone link: %s\n  Paired: %s (%s)\n  Console: %s\n  Relay: %s",
               pipeLine(pipe, line, sizeof(line)), paired ? "yes" : "no", secretText(in.secret),
               linkConsoleText(linkConsoleOf(in)), relayId(in)));
  add(snprintf(buf + used, n - used, "\n  Streams: relay %u/%u, console %u/%u",
               unsigned(in.relayStreams), kRelayStreams, unsigned(in.consoleStreams),
               kConsoleStreams));
  if (in.ended) {
    add(snprintf(buf + used, n - used, "\n  Ended: %lu (last: %s)", (unsigned long)in.ended,
                 endReasonId(in.lastEnd)));
  } else {
    add(snprintf(buf + used, n - used, "\n  Ended: 0"));
  }
  bool first = true;
  for (unsigned i = 0; endReasons && i < kEndReasons; ++i) {
    if (!endReasons[i]) continue;
    add(snprintf(buf + used, n - used, "%s%s=%lu", first ? "\n  End reasons: " : " ",
                 endReasonId(uint16_t(i)), (unsigned long)endReasons[i]));
    first = false;
  }
  return int(used);
}

}  // namespace hw1link
