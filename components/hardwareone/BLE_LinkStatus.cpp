#include "BLE_LinkStatus.h"

#if ENABLE_BLUETOOTH

#include "BLE_Peers.h"
#include "BLE_PhonePipe.h"
#include "G2_Glasses.h"
#include "System_Settings.h"
#if ENABLE_BLE_PHONE_PIPE
#include "BLE_PhonePipeConsole.h"
#endif

#if ENABLE_BLE_PHONE_PIPE
static_assert(sizeof(PhonePipeLinkSummary::endReason) / sizeof(uint32_t) == hw1link::kEndReasons,
              "one end-reason name per pipe error code");
static_assert(uint8_t(PipeConsoleState::Off) == uint8_t(hw1link::StreamState::Off) &&
              uint8_t(PipeConsoleState::NotOffered) == uint8_t(hw1link::StreamState::NotOffered) &&
              uint8_t(PipeConsoleState::Waiting) == uint8_t(hw1link::StreamState::Waiting) &&
              uint8_t(PipeConsoleState::Opening) == uint8_t(hw1link::StreamState::Opening) &&
              uint8_t(PipeConsoleState::Open) == uint8_t(hw1link::StreamState::Open) &&
              uint8_t(PipeConsoleState::Closing) == uint8_t(hw1link::StreamState::Closing) &&
              uint8_t(PipeConsoleState::Backoff) == uint8_t(hw1link::StreamState::Backoff) &&
              uint8_t(PipeConsoleState::Stopped) == uint8_t(hw1link::StreamState::Stopped),
              "hw1link::StreamState mirrors PipeConsoleState");

hw1link::PhoneLinkInputs bleLinkPhoneInputs() {
  hw1link::PhoneLinkInputs in;
  // The secret's kind from its length and the mark `phone pair new` sets on
  // the code it made (`phone secret` clears it), never its bytes.
  const bool secret = gSettings.phonePipeSecret.length() > 0;
  in.secret = !secret ? hw1link::Secret::None
            : gSettings.phonePipeSecretGenerated ? hw1link::Secret::Code : hw1link::Secret::Passphrase;
  in.consoleEnabled = gSettings.bleConsoleOnPipe;
  PhonePipeLinkSummary sum;
  in.known = phonePipeLinkSummary(&sum);
  // What the running pipe announces; otherwise what a start would offer.
  in.consoleOffered = in.known && sum.active ? sum.consoleOffered
                                             : in.consoleEnabled && in.secret == hw1link::Secret::Code;
  in.ready = sum.ready;
  in.relayOn = sum.ready && sum.relayOn;
  in.relayStreams = sum.relayStreams;
  in.consoleStreams = sum.consoleStreams;
  in.ended = sum.ended;
  in.lastEnd = sum.lastEnd;
  PipeConsoleStatus console;
  pipeConsoleStatus(&console);
  in.stream = hw1link::StreamState(uint8_t(console.state));
  if (console.state == PipeConsoleState::Open) {
    BlePipeConsoleIdentity row;
    blePipeConsoleIdentity(&row);
    in.streamSignedIn = row.bound && row.streamId == console.streamId && row.authed;
  }
  return in;
}
#endif

BleLinkStatus bleLinkStatus() {
  BleLinkStatus s;
  hw1link::RoleInputs role;
  role.server = bleServerUp();
  role.glasses = isG2ClientInitialized();
  role.pipe = phonePipeActive();
  // The shared stack owns the host for every device; a pipe running with no
  // glasses runtime beside it still reads as the phone-pipe role.
  role.pipeOwnsHost = false;
  s.role = hw1link::roleOf(role);
  s.pipeBuilt = ENABLE_BLE_PHONE_PIPE;
  s.saved = hw1link::roleOfMode(gSettings.bleMode, s.pipeBuilt);

  hw1link::PipeInputs pipe;
  pipe.built = s.pipeBuilt;
  pipe.active = role.pipe;
  pipe.ready = phonePipeReady();
  pipe.enrolling = phonePipeEnrolling();
  pipe.step = hw1link::PipeStep(phonePipeStep());
  pipe.label = phonePipeStatus();
#if ENABLE_BLE_PHONE_PIPE
  // The phone's saved target is its pipe enrollment, never an address.
  BlePeerReconnectSnapshot reconnect;
  if (blePeerReconnectSnapshot(BLE_PEER_PHONE, reconnect)) {
    s.pipePaired = reconnect.hasSavedTarget;
    s.pipeAutoReconnect = reconnect.autoReconnect;
  }
#endif
  pipe.enrolled = s.pipePaired;
  s.pipe = hw1link::pipeOf(pipe);
  s.pipeLabel = pipe.label ? pipe.label : "";

  s.phone = bleLinkPhoneInputs();
  s.consoleView = bleConsoleView();
  hw1link::ConsoleInputs console;
  console.up = role.server && s.consoleView.up;
  console.advertising = s.consoleView.advertising;
  console.clients = s.consoleView.clients;
  console.signedIn = s.consoleView.signedIn;
  // The client host's console on the phone pipe's link: a slot counts only
  // while the pipe is ready.
  console.registered = s.consoleView.registered;
  console.attachable = s.consoleView.attachable;
  console.attached = s.consoleView.attached;
  console.pipeReady = pipe.ready;
  console.secret = s.consoleView.secret;
  // The console stream inside the pipe (phone link v2) comes first.
  console.streamOffered = s.phone.consoleOffered;
  console.streamOpen = s.phone.stream == hw1link::StreamState::Open;
  console.streamSignedIn = s.phone.streamSignedIn;
  console.role = s.role;
  s.console = hw1link::consoleOf(console);
  return s;
}

void bleLinkStatusJson(JsonObject out, const BleLinkStatus& s) {
  if (out.isNull()) return;
  out["role"] = hw1link::roleId(s.role);
  out["roleText"] = hw1link::roleText(s.role);
  out["saved"] = hw1link::roleId(s.saved);
  out["savedText"] = hw1link::roleText(s.saved);
  out["savedDiffers"] = hw1link::savedDiffers(s.role, s.saved);
  if (s.pipeBuilt) {
    out["pipe"] = hw1link::pipeId(s.pipe.phase);
    out["pipeText"] = hw1link::pipeText(s.pipe.phase);
    if (s.pipe.reason) out["pipeReason"] = s.pipe.reason;
    out["pipePaired"] = s.pipePaired;
  }
  out["console"] = hw1link::consoleId(s.console);
  out["consoleText"] = hw1link::consoleLongText(s.console);
  // A busy lock leaves the last reading in consoleView; it counts only while
  // the console server is up, or on the phone link once its console shows as
  // connected (a slot left by a pipe that is no longer ready is not). A
  // connected phone link has at least the one session the words name.
  using hw1link::Console;
  const bool server = s.consoleView.up && s.console != Console::Off && s.console != Console::NotHere;
  const bool phoneLink = !s.consoleView.up &&
                         (s.console == Console::Connected || s.console == Console::SignedIn);
  const unsigned clients = phoneLink && !s.consoleView.clients ? 1u : s.consoleView.clients;
  out["consoleClients"] = (server || phoneLink) ? clients : 0u;
  out["advertising"] = server && s.consoleView.advertising;
}

void bleLinkPhoneJson(JsonObject out, const BleLinkStatus& s, bool full) {
#if ENABLE_BLE_PHONE_PIPE
  if (out.isNull()) return;
  const hw1link::PhoneLinkInputs& p = s.phone;
  out["state"] = hw1link::pipeId(s.pipe.phase);
  if (full) out["stateText"] = hw1link::pipeText(s.pipe.phase);
  if (s.pipe.reason) out["reason"] = s.pipe.reason;
  out["paired"] = s.pipePaired;
  out["secret"] = hw1link::secretId(p.secret);
  if (full) out["secretText"] = hw1link::secretText(p.secret);
  const hw1link::LinkConsole console = hw1link::linkConsoleOf(p);
  out["console"] = hw1link::linkConsoleId(console);
  if (full) out["consoleText"] = hw1link::linkConsoleText(console);
  out["relay"] = hw1link::relayId(p);
  JsonObject streams = out["streams"].to<JsonObject>();
  streams["relay"] = p.relayStreams;
  streams["console"] = p.consoleStreams;
  // The full form takes ended, lastEnd and the counts from one reading.
  PhonePipeLinkSummary sum;
  const bool counts = full && phonePipeLinkSummary(&sum);
  const uint32_t ended = counts ? sum.ended : p.ended;
  out["ended"] = ended;
  if (ended) out["lastEnd"] = hw1link::endReasonId(counts ? sum.lastEnd : p.lastEnd);
  if (!full) return;
  JsonObject reasons = out["endReasons"].to<JsonObject>();
  for (unsigned i = 0; counts && i < hw1link::kEndReasons; ++i) {
    if (sum.endReason[i]) reasons[hw1link::endReasonId(uint16_t(i))] = sum.endReason[i];
  }
#else
  (void)out; (void)s; (void)full;
#endif
}

int bleLinkPhoneLines(char* buf, size_t n, const BleLinkStatus& s) {
  if (!buf || !n) return 0;
  buf[0] = '\0';
#if ENABLE_BLE_PHONE_PIPE
  // ended, lastEnd and the counts from one reading, as in the JSON.
  hw1link::PhoneLinkInputs in = s.phone;
  PhonePipeLinkSummary sum;
  const bool counts = phonePipeLinkSummary(&sum);
  if (counts) { in.ended = sum.ended; in.lastEnd = sum.lastEnd; }
  return hw1link::phoneLinkLines(buf, n, s.pipe, s.pipePaired, in, counts ? sum.endReason : nullptr);
#else
  (void)s;
  return 0;
#endif
}

#endif  // ENABLE_BLUETOOTH
