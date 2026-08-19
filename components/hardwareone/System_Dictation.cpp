// System_Dictation.cpp — see System_Dictation.h for the round trip and for the
// security note on the event push.

#include "System_Dictation.h"

#if ENABLE_DICTATION

#include <esp_random.h>

#include "HAL_Audio.h"
#include "System_AuthIdentity.h"   // currentAuthContext — transport boundary check
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Microphone.h"
#include "System_UartLink.h"
#include "System_User.h"

// Silence that ends a dictation. Shorter than the EvenAI flow's window on
// purpose: a person typing a field pauses far less than one asking a question,
// and every extra millisecond here is dead air the wearer waits through.
static constexpr uint32_t kDictationVadSilenceMs = 1200;

// Hard cap on one capture. The VAD normally ends it long before this; the cap
// exists so a noisy room (which never scores as silence) cannot record until
// the filesystem fills.
static constexpr uint32_t kDictationMaxRecordMs = 20000;

// How long the host gets to return a transcript before the wearer is told it
// failed. Generous enough for voicefetch + a cold model load on the CM5.
static constexpr uint32_t kDictationResultTimeoutMs = 15000;

struct DictationControl {
  DictationState state;
  uint64_t owner;                    // 0 when nothing is in flight
  TransportSessionEpoch displayEpoch;
  uint32_t stateEnteredMs;
  bool weStartedMic;                 // only stop what we started
  bool micStopPending;               // deferred teardown; see dictationTick()
  bool textPending;
  char text[DICTATION_MAX_TEXT + 1];
  char failure[40];
};

static portMUX_TYPE gDictMux = portMUX_INITIALIZER_UNLOCKED;
static DictationControl gDict = {
    DictationState::IDLE, 0, kNoTransportSessionEpoch, 0,
    false, false, false, {}, {}};

// Hand the mic back if — and only if — this module was what powered it up.
// Deferred rather than immediate because stopMicrophone() JOINS the recorder
// through FINALIZING, and every caller that wants the mic released (cancel,
// timeout, mode change) runs on the OLED display task, which must not stall a
// frame on a file close. Once the recorder is idle the join is trivial.
static void dictationReleaseMicIfDue() {
  bool due = false;
  portENTER_CRITICAL(&gDictMux);
  if (gDict.micStopPending && !gDict.owner) {
    due = true;
    gDict.micStopPending = false;
    gDict.weStartedMic = false;
  }
  portEXIT_CRITICAL(&gDictMux);
  if (!due) return;
  if (micRecordingBusy()) {
    // Still finalizing — put the request back and retry on the next tick.
    portENTER_CRITICAL(&gDictMux);
    gDict.micStopPending = true;
    portEXIT_CRITICAL(&gDictMux);
    return;
  }
  stopMicrophone();
}

// Boot nonce for dictation IDs, kept distinct from the EvenAI exchange nonce so
// the two owner spaces cannot collide in the recorder's completion ring.
static uint32_t gDictBootNonce = 0;
static uint32_t gDictCounter = 0;

// Is the display session that armed a dictation still the live one? Used on
// the delivery paths only. Deliberately NOT called from the recorder task: it
// takes the display lifecycle lock, and that task's terminal section has a
// documented FS-lock/TX-mutex ordering that no new lock should join.
static bool displaySessionStillLive(TransportSessionEpoch expected) {
  if (expected == kNoTransportSessionEpoch) return false;
  String user;
  bool authed = false;
  const TransportSessionEpoch live =
      localDisplayTransportSessionSnapshot(user, authed);
  secureClearString(user);
  return authed && live == expected;
}

static const char* sourceLabel(AudioSource src) {
  switch (src) {
    case AUDIO_SRC_LOCAL_PDM: return "PDM";
    case AUDIO_SRC_G2_LEFT:   return "G2";
    default:                  return "none";
  }
}

static void dictFormatId(uint64_t id, char out[17]) {
  snprintf(out, 17, "%08lx%08lx", (unsigned long)(id >> 32),
           (unsigned long)(id & 0xFFFFFFFFu));
}

static bool dictParseId(const char* text, size_t len, uint64_t& out) {
  if (len != 16) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < 16; ++i) {
    const char c = text[i];
    uint8_t nibble;
    if (c >= '0' && c <= '9')      nibble = (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f') nibble = (uint8_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') nibble = (uint8_t)(c - 'A' + 10);
    else return false;
    value = (value << 4) | nibble;
  }
  // Same both-halves rule the recorder enforces: a malformed or half-zero token
  // must not collapse into a manual-like identity.
  if ((uint32_t)(value >> 32) == 0 || (uint32_t)(value & 0xFFFFFFFFu) == 0) {
    return false;
  }
  out = value;
  return true;
}

// Enter a terminal failure. Safe from any task.
static void dictFail(const char* reason) {
  portENTER_CRITICAL(&gDictMux);
  gDict.state = DictationState::FAILED;
  gDict.stateEnteredMs = millis();
  gDict.owner = 0;
  gDict.micStopPending = gDict.weStartedMic;
  snprintf(gDict.failure, sizeof(gDict.failure), "%s", reason ? reason : "failed");
  portEXIT_CRITICAL(&gDictMux);
}

bool dictationAvailable(const char** whyNot) {
  if (!audioAnySourceAvailable()) {
    // Covers both boards with no PDM silicon and a board whose glasses are not
    // connected right now. Either way there is nothing to record with.
    if (whyNot) *whyNot = "no mic";
    return false;
  }
  if (!uartLinkIsRunning()) {
    if (whyNot) *whyNot = "no host link";
    return false;
  }
  if (uartLinkSessionEpoch() == 0) {
    if (whyNot) *whyNot = "host not logged in";
    return false;
  }
  if (whyNot) *whyNot = nullptr;
  return true;
}

bool dictationBegin(TransportSessionEpoch displayEpoch) {
  if (displayEpoch == kNoTransportSessionEpoch) return false;

  const char* why = nullptr;
  if (!dictationAvailable(&why)) {
    dictFail(why ? why : "unavailable");
    return false;
  }

  // Refuse a second arm rather than stacking captures; the recorder would
  // reject the start anyway, but failing here keeps the UI state honest.
  bool armed = false;
  portENTER_CRITICAL(&gDictMux);
  if (gDict.state == DictationState::IDLE || gDict.state == DictationState::FAILED) {
    gDict.state = DictationState::RECORDING;
    gDict.stateEnteredMs = millis();
    gDict.failure[0] = '\0';
    gDict.textPending = false;
    gDict.text[0] = '\0';
    gDict.displayEpoch = displayEpoch;
    armed = true;
  }
  portEXIT_CRITICAL(&gDictMux);
  if (!armed) return false;

  // The mic may be idle — bring it up, but remember that WE did, so the wearer's
  // own openmic/closemic state survives a dictation untouched.
  bool startedMic = false;
  if (!gMicRunning) {
    if (!initMicrophone()) {
      dictFail("mic would not start");
      return false;
    }
    startedMic = true;
  }

  uint32_t nonceCandidate = esp_random();
  if (!nonceCandidate) nonceCandidate = 1;
  uint64_t owner = 0;
  portENTER_CRITICAL(&gDictMux);
  if (!gDictBootNonce) gDictBootNonce = nonceCandidate;
  if (++gDictCounter == 0) ++gDictCounter;
  owner = ((uint64_t)gDictBootNonce << 32) | (uint64_t)gDictCounter;
  gDict.owner = owner;
  gDict.weStartedMic = startedMic;
  portEXIT_CRITICAL(&gDictMux);

  // Source-agnostic by construction: startRecordingOwned re-resolves the
  // preference against what is connected now and owns every G2-only step.
  if (!startRecordingOwned(owner, kDictationVadSilenceMs, /*trim=*/true)) {
    dictFail("recorder busy");
    return false;
  }

  char id[17];
  dictFormatId(owner, id);
  INFO_SYSTEMF("[DICTATE] armed id=%s source=%s", id,
               sourceLabel(audioGetSource()));
  return true;
}

void dictationRequestStop() {
  uint64_t owner = 0;
  portENTER_CRITICAL(&gDictMux);
  if (gDict.state == DictationState::RECORDING) owner = gDict.owner;
  portEXIT_CRITICAL(&gDictMux);
  if (!owner) return;
  // Non-blocking: this runs on the display task, which must not stall a frame
  // waiting for FINALIZING. The terminal hook picks it up from the recorder.
  requestStopRecordingOwned(owner, /*discard=*/false);
}

void dictationCancel() {
  uint64_t owner = 0;
  portENTER_CRITICAL(&gDictMux);
  owner = gDict.owner;
  gDict.state = DictationState::IDLE;
  gDict.stateEnteredMs = millis();
  gDict.owner = 0;
  gDict.displayEpoch = kNoTransportSessionEpoch;
  gDict.micStopPending = gDict.weStartedMic;
  gDict.textPending = false;
  gDict.text[0] = '\0';
  gDict.failure[0] = '\0';
  portEXIT_CRITICAL(&gDictMux);

  if (owner) {
    // discard=true: the WAV of an abandoned dictation is removed by the
    // recorder at finalize. No wait here — same display-task rule as above.
    requestStopRecordingOwned(owner, /*discard=*/true);
  }
  dictationReleaseMicIfDue();
}

void dictationResetForSessionBoundary() {
  bool inFlight = false;
  portENTER_CRITICAL(&gDictMux);
  inFlight = (gDict.state != DictationState::IDLE) || gDict.owner != 0;
  portEXIT_CRITICAL(&gDictMux);
  if (inFlight) dictationCancel();
}

DictationSnapshot dictationSnapshotNow() {
  DictationSnapshot out{};
  const uint32_t now = millis();
  portENTER_CRITICAL(&gDictMux);
  out.state = gDict.state;
  out.elapsedMs = (uint32_t)(now - gDict.stateEnteredMs);
  snprintf(out.failure, sizeof(out.failure), "%s", gDict.failure);
  portEXIT_CRITICAL(&gDictMux);
  out.sourceName = sourceLabel(audioGetSource());
  out.level = (out.state == DictationState::RECORDING) ? getAudioLevel() : 0;
  return out;
}

void dictationTick() {
  DictationState state;
  uint32_t elapsed;
  uint64_t owner;
  const uint32_t now = millis();
  portENTER_CRITICAL(&gDictMux);
  state = gDict.state;
  elapsed = (uint32_t)(now - gDict.stateEnteredMs);
  owner = gDict.owner;
  portEXIT_CRITICAL(&gDictMux);

  if (state == DictationState::RECORDING && elapsed >= kDictationMaxRecordMs) {
    // Cap reached without the VAD ever seeing silence. Stop the capture and let
    // the terminal hook take it from there — a long noisy take may still hold a
    // usable phrase, so this is a stop, not a discard.
    if (owner) requestStopRecordingOwned(owner, /*discard=*/false);
    return;
  }

  if (state == DictationState::WAITING && elapsed >= kDictationResultTimeoutMs) {
    if (owner) deleteRecordingOwned(owner, nullptr);
    dictFail("host did not answer");
  }

  dictationReleaseMicIfDue();
}

bool dictationTakeText(char* out, size_t outSize) {
  if (!out || outSize == 0) return false;
  bool took = false;
  portENTER_CRITICAL(&gDictMux);
  if (gDict.textPending) {
    snprintf(out, outSize, "%s", gDict.text);
    gDict.textPending = false;
    gDict.text[0] = '\0';
    gDict.state = DictationState::IDLE;
    gDict.stateEnteredMs = millis();
    gDict.micStopPending = gDict.weStartedMic;
    took = true;
  }
  portEXIT_CRITICAL(&gDictMux);
  if (took) dictationReleaseMicIfDue();
  return took;
}

void dictationOnCaptureClosed(uint64_t owner, const char* path, bool saved) {
  if (!owner || !path || !path[0]) return;

  // Claim the transition before doing anything observable: this runs on the
  // recorder task while the wearer may be cancelling on the display task.
  bool mine = false;
  portENTER_CRITICAL(&gDictMux);
  if (gDict.state == DictationState::RECORDING && gDict.owner == owner) {
    mine = true;
    gDict.state = DictationState::WAITING;
    gDict.stateEnteredMs = millis();
  }
  portEXIT_CRITICAL(&gDictMux);
  if (!mine) return;

  if (!saved) {
    dictFail("capture failed");
    return;
  }

  // No display-session liveness check here — see displaySessionStillLive() for
  // why this task must not take that lock. The fence that matters is on
  // delivery: dictDeliver() refuses a transcript whose arming session is gone,
  // and a session boundary cancels and discards outright. The worst case here
  // is a WAV path pushed to a host that is already entitled to voicefetch it.
  //
  // The deliberate widening documented in the header: target the currently
  // authenticated UART session, because a wearer-armed capture has no
  // admitting one. Everything narrowing it has already been checked above.
  const uint32_t hostEpoch = uartLinkSessionEpoch();
  if (hostEpoch == 0) {
    deleteRecordingOwned(owner, nullptr);
    dictFail("no host session");
    return;
  }

  char id[17];
  dictFormatId(owner, id);
  char evt[128];
  snprintf(evt, sizeof(evt), "dictate_request %s %s", id, path);
  if (!uartLinkPushEventForSession(hostEpoch, evt)) {
    deleteRecordingOwned(owner, nullptr);
    dictFail("host unreachable");
    return;
  }
  INFO_SYSTEMF("[DICTATE] request pushed id=%s", id);
}

// ============================================================================
// Command
// ============================================================================

static const char* dictDeliver(uint64_t id, const char* text) {
  static char reply[96];

  bool matched = false;
  TransportSessionEpoch displayEpoch = kNoTransportSessionEpoch;
  portENTER_CRITICAL(&gDictMux);
  if (gDict.state == DictationState::WAITING && gDict.owner == id) {
    matched = true;
    displayEpoch = gDict.displayEpoch;
  }
  portEXIT_CRITICAL(&gDictMux);
  if (!matched) {
    // Single-use by construction: the id is cleared on delivery, so a replay
    // finds no pending dictation and is refused rather than re-typing itself.
    return "Error: no dictation is waiting for that id";
  }
  if (!displaySessionStillLive(displayEpoch)) {
    deleteRecordingOwned(id, nullptr);
    dictFail("session changed");
    return "Error: the display session that armed this dictation is gone";
  }

  portENTER_CRITICAL(&gDictMux);
  snprintf(gDict.text, sizeof(gDict.text), "%s", text ? text : "");
  gDict.textPending = true;
  gDict.owner = 0;
  // State stays WAITING until the keyboard drains it; dictationTakeText()
  // publishes IDLE. That keeps the mode from flashing "ready" for one frame
  // before the text actually appears.
  gDict.stateEnteredMs = millis();
  portEXIT_CRITICAL(&gDictMux);

  // The host has what it needs and the text is delivered; the WAV of somebody's
  // dictation should not outlive the exchange.
  deleteRecordingOwned(id, nullptr);

  snprintf(reply, sizeof(reply), "OK: dictation delivered (%u chars)",
           (unsigned)(text ? strlen(text) : 0));
  return reply;
}

const char* cmd_dictate(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Same transport boundary as voicefetch: this is host-to-firmware plumbing,
  // never a console command, and it writes into a UI field.
  const AuthContext& ctx = currentAuthContext();
  if (ctx.transport != SOURCE_UART)
    return "Error: dictate only serves the UART link";
  if (uartLinkSessionEpoch() == 0)
    return "Error: dictate requires a logged-in UART session";

  String args = argsInput;
  args.trim();

  if (args.length() == 0 || args.equalsIgnoreCase("status")) {
    static char status[160];
    DictationSnapshot snap = dictationSnapshotNow();
    const char* stateName = "idle";
    switch (snap.state) {
      case DictationState::RECORDING: stateName = "recording"; break;
      case DictationState::WAITING:   stateName = "waiting";   break;
      case DictationState::FAILED:    stateName = "failed";    break;
      case DictationState::IDLE:      stateName = "idle";      break;
    }
    snprintf(status, sizeof(status),
             "Dictation: state=%s source=%s elapsed=%lums%s%s", stateName,
             snap.sourceName, (unsigned long)snap.elapsedMs,
             snap.failure[0] ? " failure=" : "", snap.failure);
    return status;
  }

  // `result <16hex> <text...>` — the text is taken verbatim to end of line, so
  // a transcript may contain spaces, quotes and punctuation without escaping.
  if (args.startsWith("result ")) {
    const char* cursor = args.c_str() + 7;
    while (*cursor == ' ') ++cursor;
    const char* idStart = cursor;
    while (*cursor && *cursor != ' ') ++cursor;
    uint64_t id = 0;
    if (!dictParseId(idStart, (size_t)(cursor - idStart), id))
      return "Error: usage: dictate result <16hex> <text>";
    while (*cursor == ' ') ++cursor;
    if (!*cursor) return "Error: dictate result needs text";
    return dictDeliver(id, cursor);
  }

  if (args.startsWith("fail ")) {
    const char* cursor = args.c_str() + 5;
    while (*cursor == ' ') ++cursor;
    const char* idStart = cursor;
    while (*cursor && *cursor != ' ') ++cursor;
    uint64_t id = 0;
    if (!dictParseId(idStart, (size_t)(cursor - idStart), id))
      return "Error: usage: dictate fail <16hex> [reason]";
    bool matched = false;
    portENTER_CRITICAL(&gDictMux);
    matched = (gDict.state == DictationState::WAITING && gDict.owner == id);
    portEXIT_CRITICAL(&gDictMux);
    if (!matched) return "Error: no dictation is waiting for that id";
    while (*cursor == ' ') ++cursor;
    deleteRecordingOwned(id, nullptr);
    dictFail(*cursor ? cursor : "host reported failure");
    return "OK: dictation marked failed";
  }

  return "Error: invalid arguments — Usage: dictate status | result <16hex> <text> | fail <16hex> [reason]";
}

const CommandEntry dictationCommands[] = {
    {"dictate", "Deliver a host transcript into the on-device text field.", false,
     cmd_dictate,
     "Usage: dictate status\n"
     "       dictate result <16hex> <text>   - text runs to end of line\n"
     "       dictate fail <16hex> [reason]"},
};

const size_t dictationCommandsCount =
    sizeof(dictationCommands) / sizeof(dictationCommands[0]);

#endif  // ENABLE_DICTATION
