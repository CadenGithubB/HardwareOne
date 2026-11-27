// System_Dictation.cpp — see System_Dictation.h for the round trip and for the
// security note on the event push.

#include "System_Dictation.h"

#if ENABLE_DICTATION

#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <algorithm>

#include "HAL_Audio.h"
#include "System_Cm5Presence.h"
#include "System_Debug.h"
#include "System_DictationPolicy.h"
#include "System_Microphone.h"
#include "System_ESPSR.h"
#include "System_STT.h"
#include "System_TaskUtils.h"
#include "System_UartLink.h"
#include "System_User.h"
#include "System_Utils.h"  // secureClearString
#include <esp_attr.h>  // EXT_RAM_BSS_ATTR

// Silence that ends a dictation. Shorter than the EvenAI flow's window on
// purpose: a person typing a field pauses far less than one asking a question,
// and every extra millisecond here is dead air the wearer waits through.
static constexpr uint32_t kDictationVadSilenceMs = 1200;

// This task owns every potentially blocking terminal operation: UART event
// framing, filesystem deletion, and source shutdown. Display and recorder tasks
// only publish fixed-size work under sDictMux and notify it.
// Both builds run the transcript filesystem writer here; local STT adds model
// file probes and snapshot/chunk copies on top, so it needs the larger stack.
static constexpr uint32_t kDictationWorkerStackBytes = ENABLE_LOCAL_STT ? 6144 : 5120;
static constexpr uint32_t kDictationWorkerRetryMs = 50;

struct DictationPublishedCapture {
  bool pending;
  uint64_t owner;
  bool saved;
  char path[64];
  char failure[48];
};

struct DictationCancelEvent {
  bool pending;
  uint64_t owner;
  uint32_t hostEpoch;
};

struct DictationControl {
  DictationState state;
  uint64_t owner;                    // 0 when nothing is in flight
  CommandSource displaySource;       // OLED/G2 keyboard or OLED/G2/web App authority
  TransportSessionEpoch displayEpoch;
  uint32_t requestHostEpoch;          // UART epoch that received the EVT
  bool requestPushInFlight;
  bool requestWasPushed;
  uint32_t stateEnteredMs;
  bool weStartedMic;                 // only stop what we started
  bool micStopPending;               // deferred teardown; see dictationTick()
  bool textPending;
  char text[DICTATION_MAX_TEXT + 1];
  char failure[192];
  uint64_t exchangePathOwner;
  char exchangePath[64];
  // At most one debt exists because admission is refused until it clears. The
  // `resolved` bit distinguishes "begin may still start this owner" from a
  // proven terminal NOT_FOUND, so cancellation before start cannot be lost.
  uint64_t cleanupOwner;
  bool cleanupResolved;
  char cleanupPath[64];
  DictationPublishedCapture published;
  DictationCancelEvent cancelEvent;
  // Common delivery mailbox. Pi v1 supplies one final chunk; local inference
  // supplies successive chunks. UI consumers never select a provider.
  uint64_t deliveryExchange = 0;
  uint32_t deliverySequence = 0;
  uint16_t deliveryOffset = 0;
  uint16_t deliveryTotal = 0;
  bool deliveryNeedsAck = false;
  bool deliveryAckPending = false;
  bool continuous = false; // Latched capture capability, independent of UI.
  bool stopRequested = false;
  TranscriptOptions transcriptOptions;
  TranscriptStatus transcriptStatus;
  uint64_t transcriptExchange = 0;
  bool appConsumer = false;
  DictationAppLease appLease; // Retained until the next admission.
  DictationTextReceipt appLastReceipt;
  uint16_t appLastAccepted = 0;
  SttSpeakerSnapshot appSpeakers;

};

// UART acceptance copies one bounded job; only dictation_svc writes it.
// This identity survives UI cancellation/drain after recognition was accepted.
struct DictationSaveJob {
  bool pending = false;
  bool inFlight = false;
  uint64_t exchange = 0;
  TranscriptOptions options;
  char text[DICTATION_MAX_TEXT + 1] = {};
};
static DictationSaveJob sDictSave;

static portMUX_TYPE sDictMux = portMUX_INITIALIZER_UNLOCKED;
static DictationControl sDict = {
    DictationState::IDLE, 0, SOURCE_INTERNAL, kNoTransportSessionEpoch,
    0, false, false, 0, false, false, false, {}, {}, 0, {}, 0, false,
    {}, {false, 0, false, {}, {}}, {false, 0, 0}};

// Lock-only identity predicate. Keyboard calls cannot impersonate an App on
// the same display; the exchange also distinguishes successive same-user runs,
// and the purpose keeps a Transcription handle off a QuickNote and vice versa.
static bool dictationConsumerMatchesLocked(CommandSource source,
                                           const DictationAppLease* app = nullptr) {
  if (!app) return !sDict.appConsumer && sDict.displaySource == source;
  return sDict.appConsumer && app->exchange && source == app->source &&
      sDict.appLease.source == app->source && sDict.appLease.epoch == app->epoch &&
      sDict.appLease.exchange == app->exchange && sDict.appLease.purpose == app->purpose;
}

// `app` carries the requested purpose in; the admitted lease is copied back.
static void dictationAdmitConsumerLocked(CommandSource source,
                                         TransportSessionEpoch epoch,
                                         uint64_t exchange,
                                         DictationAppLease* app) {
  sDict.appConsumer = app != nullptr;
  sDict.appLease = app ? DictationAppLease{source, epoch, exchange, app->purpose}
                       : DictationAppLease{};
  sDict.appLastReceipt = DictationTextReceipt{};
  sDict.appLastAccepted = 0;
  sDict.appSpeakers = SttSpeakerSnapshot{};
  if (app) *app = sDict.appLease;
}

// All mailbox mutations run under sDictMux. Keep delivery identity independent
// of recorder/host ownership: Pi accepts its result and clears owner before UI
// delivery, and its exact WAV cleanup may finish before the field consumes text.
static void dictationClearDeliveryLocked() {
  memset(sDict.text, 0, sizeof(sDict.text));
  sDict.textPending = false;
  sDict.deliveryExchange = 0;
  sDict.deliverySequence = 0;
  sDict.deliveryOffset = sDict.deliveryTotal = 0;
  sDict.deliveryNeedsAck = sDict.deliveryAckPending = false;
}

static void dictationFinishInputLocked() {
  dictationClearDeliveryLocked();
  sDict.owner = 0;
  sDict.state = DictationState::IDLE;
  sDict.stateEnteredMs = millis();
  sDict.micStopPending = sDict.weStartedMic;
  sDict.displaySource = SOURCE_INTERNAL;
  sDict.displayEpoch = kNoTransportSessionEpoch;
  sDict.requestHostEpoch = 0;
  sDict.requestPushInFlight = sDict.requestWasPushed = false;
}

static void dictationStageTextLocked(uint64_t exchange, uint32_t sequence,
                                    const char* text, size_t total, size_t offset,
                                    bool needsAck) {
  const size_t amount = std::min(total - offset, static_cast<size_t>(DICTATION_MAX_TEXT));
  memcpy(sDict.text, text + offset, amount);
  sDict.text[amount] = '\0';
  sDict.textPending = amount != 0;
  sDict.deliveryExchange = exchange;
  sDict.deliverySequence = sequence;
  sDict.deliveryOffset = static_cast<uint16_t>(offset);
  sDict.deliveryTotal = static_cast<uint16_t>(total);
  sDict.deliveryNeedsAck = needsAck;
  // Silence-only backend segments may validly decode to empty text. They need
  // acknowledgment but must not become a phantom UI action or a speech failure.
  sDict.deliveryAckPending = needsAck && total == offset;
}

// Capability is deliberately an epoch, not a sticky bool: UART logout/login
// revokes it without depending on a delayed session-change callback.
static uint32_t sDictHostReadyEpoch = 0;
// The worker (System_TaskUtils.h ServiceWorker: created on the first use,
// leaves idle; its handle, claim and wake live under sDictMux with the work).
static ServiceWorker sDictWorker{sDictMux};

static void dictationWakeWorker();
static bool dictationEnsureWorker();
static void dictationSupervise();

// Hand the mic back if — and only if — this module was what powered it up.
// Deferred rather than immediate because stopMicrophone() JOINS the recorder
// through FINALIZING, and every caller that wants the mic released (cancel,
// timeout, mode change) runs on the OLED display task, which must not stall a
// frame on a file close. Once the recorder is idle the join is trivial.
static void dictationReleaseMicIfDue() {
  bool due = false;
  portENTER_CRITICAL(&sDictMux);
  if (sDict.micStopPending && !sDict.owner) {
    due = true;
    sDict.micStopPending = false;
    sDict.weStartedMic = false;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (!due) return;
  if (micRecordingBusy()) {
    // Still finalizing — put the request back and retry on the next tick.
    portENTER_CRITICAL(&sDictMux);
    sDict.micStopPending = true;
    portEXIT_CRITICAL(&sDictMux);
    return;
  }
  if (!stopMicrophone()) {
    // Source teardown has a bounded join. Preserve the debt if another capture
    // appeared between the idle check and the source-operation mutex.
    portENTER_CRITICAL(&sDictMux);
    sDict.micStopPending = true;
    portEXIT_CRITICAL(&sDictMux);
  }
}

// Boot nonce for dictation IDs, kept distinct from the EvenAI exchange nonce so
// the two owner spaces cannot collide in the recorder's completion ring.
static uint32_t sDictBootNonce = 0;
static uint32_t sDictCounter = 0;

// Is the named session that armed a dictation still the live one? Used on
// the delivery paths only. Deliberately NOT called from the recorder task: it
// takes the display lifecycle lock, and that task's terminal section has a
// documented FS-lock/TX-mutex ordering that no new lock should join.
static bool displaySessionStillLive(CommandSource source,
                                    TransportSessionEpoch expected) {
  if (expected == kNoTransportSessionEpoch) return false;
  if (source == SOURCE_G2_GLASSES || source == SOURCE_WEB) {
    return transportSessionEpochIsLive(source, expected);
  }
  if (source != SOURCE_LOCAL_DISPLAY) return false;
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
  return dictationParseWireId(text, len, &out);
}

// Enter a terminal failure. Safe from any task.
// Async paths must prove they still own the same exchange before publishing a
// failure. A wearer can cancel and immediately arm a new dictation while the
// recorder/UART task is finishing the old one; an unconditional failure there
// would otherwise clobber the new owner's state.
static bool dictFailOwned(uint64_t expectedOwner, const char* reason) {
  if (!expectedOwner) return false;
  bool failed = false;
  portENTER_CRITICAL(&sDictMux);
  if (sDict.owner == expectedOwner &&
      (sDict.state == DictationState::RECORDING ||
       sDict.state == DictationState::WAITING)) {
    sDict.state = DictationState::FAILED;
    sDict.stateEnteredMs = millis();
    sDict.owner = 0;
    sDict.requestHostEpoch = 0;
    sDict.requestPushInFlight = false;
    sDict.requestWasPushed = false;
    sDict.micStopPending = sDict.weStartedMic;
    snprintf(sDict.failure, sizeof(sDict.failure), "%s",
             reason ? reason : "failed");
    failed = true;
  }
  portEXIT_CRITICAL(&sDictMux);
  return failed;
}

static bool dictationHostReadyForEpoch(uint32_t liveEpoch,
                                       const char** whyNot = nullptr) {
  uint32_t capabilityEpoch = 0;
  portENTER_CRITICAL(&sDictMux);
  capabilityEpoch = sDictHostReadyEpoch;
  portEXIT_CRITICAL(&sDictMux);

  if (liveEpoch == 0 || capabilityEpoch != liveEpoch) {
    if (whyNot) *whyNot = "CM5 transcription service not ready. Start its dictation service.";
    return false;
  }

  const Cm5PresenceSnapshot presence =
      cm5PresenceSnapshotForSession(liveEpoch, millis());
  const bool readyOrBusy = presence.mode == Cm5PresenceMode::Ready ||
                           presence.mode == Cm5PresenceMode::Busy;
  const bool ready = dictationHostCapabilityReady(
      liveEpoch, capabilityEpoch, presence.seenForSession, presence.fresh,
      readyOrBusy);
  if (!ready && whyNot) {
    *whyNot = !presence.seenForSession ? "CM5 has not checked in. Check its host service."
             : !presence.fresh         ? "CM5 heartbeat expired. Check its connection and host service."
                                       : "CM5 transcription service not ready. Start its dictation service.";
  }
  if (ready && whyNot) *whyNot = nullptr;
  return ready;
}

// Wakes the worker. Work published to a worker that left idle brings one
// back first; a create that fails leaves the work for the next wake (the
// publishers that face the user refuse through dictationEnsureWorker).
static void dictationWakeWorker() {
  (void)dictationEnsureWorker();
  sDictWorker.give();
}

static bool dictationQueueCleanup(uint64_t owner, bool resolved,
                                  const char* exactPath = nullptr) {
  if (!owner) return false;
  char pathCopy[64] = {};
  if (exactPath) snprintf(pathCopy, sizeof(pathCopy), "%s", exactPath);
  bool queued = false;
  bool conflict = false;
  portENTER_CRITICAL(&sDictMux);
  if (!sDict.cleanupOwner || sDict.cleanupOwner == owner) {
    sDict.cleanupOwner = owner;
    sDict.cleanupResolved = sDict.cleanupResolved || resolved;
    if (pathCopy[0]) {
      memcpy(sDict.cleanupPath, pathCopy, sizeof(sDict.cleanupPath));
    } else if (!sDict.cleanupPath[0] && sDict.exchangePathOwner == owner) {
      memcpy(sDict.cleanupPath, sDict.exchangePath,
             sizeof(sDict.cleanupPath));
    }
    queued = true;
  } else {
    conflict = true;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (conflict) {
    char id[17];
    dictFormatId(owner, id);
    WARN_SYSTEMF("[DICTATE] cleanup debt collision for id=%s", id);
  }
  if (queued) dictationWakeWorker();
  return queued;
}

static void dictationResolveCleanup(uint64_t owner) {
  bool wake = false;
  portENTER_CRITICAL(&sDictMux);
  if (owner && sDict.cleanupOwner == owner) {
    sDict.cleanupResolved = true;
    wake = true;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (wake) dictationWakeWorker();
}

static void dictationClearPublished(uint64_t owner) {
  portENTER_CRITICAL(&sDictMux);
  if (sDict.published.pending && sDict.published.owner == owner) {
    sDict.published = DictationPublishedCapture{};
  }
  portEXIT_CRITICAL(&sDictMux);
}

static void dictationQueueCancelEvent(uint64_t owner, uint32_t hostEpoch) {
  if (!owner || !hostEpoch) return;
  bool queued = false;
  bool conflict = false;
  portENTER_CRITICAL(&sDictMux);
  if (!sDict.cancelEvent.pending ||
      (sDict.cancelEvent.owner == owner &&
       sDict.cancelEvent.hostEpoch == hostEpoch)) {
    sDict.cancelEvent.pending = true;
    sDict.cancelEvent.owner = owner;
    sDict.cancelEvent.hostEpoch = hostEpoch;
    queued = true;
  } else {
    conflict = true;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (conflict) {
    char id[17];
    dictFormatId(owner, id);
    WARN_SYSTEMF("[DICTATE] cancel event collision for id=%s", id);
  }
  if (queued) dictationWakeWorker();
}

static void dictationProcessCancelEvent() {
  DictationCancelEvent cancel{};
  portENTER_CRITICAL(&sDictMux);
  cancel = sDict.cancelEvent;
  portEXIT_CRITICAL(&sDictMux);
  if (!cancel.pending) return;

  char id[17];
  dictFormatId(cancel.owner, id);
  char evt[48];
  snprintf(evt, sizeof(evt), "dictate_cancel %s", id);
  const bool pushed =
      uartLinkPushEventForSession(cancel.hostEpoch, evt);
  const bool sessionGone =
      uartLinkSessionEpoch() != cancel.hostEpoch || !uartLinkIsRunning();

  portENTER_CRITICAL(&sDictMux);
  if (sDict.cancelEvent.pending &&
      sDict.cancelEvent.owner == cancel.owner &&
      sDict.cancelEvent.hostEpoch == cancel.hostEpoch &&
      (pushed || sessionGone)) {
    sDict.cancelEvent = DictationCancelEvent{};
  }
  portEXIT_CRITICAL(&sDictMux);
  if (pushed) INFO_SYSTEMF("[DICTATE] cancel pushed id=%s", id);
}

static void dictationFailAndClean(uint64_t owner, const char* reason) {
  char path[64] = {};
  portENTER_CRITICAL(&sDictMux);
  if (sDict.published.pending && sDict.published.owner == owner) {
    memcpy(path, sDict.published.path, sizeof(path));
  }
  portEXIT_CRITICAL(&sDictMux);
  dictationClearPublished(owner);
  (void)dictFailOwned(owner, reason);
  (void)dictationQueueCleanup(owner, /*resolved=*/true, path);
}

// Runs only on the dictation service task. Returns true when it consumed the
// queued publication, false when a recorder interloper means voicefetch would
// still reject the file and the item must remain queued.
static bool dictationProcessPublished() {
  DictationPublishedCapture capture{};
  bool exactOwner = false;
  CommandSource displaySource = SOURCE_INTERNAL;
  TransportSessionEpoch displayEpoch = kNoTransportSessionEpoch;
  portENTER_CRITICAL(&sDictMux);
  if (!sDict.published.pending) {
    portEXIT_CRITICAL(&sDictMux);
    return true;
  }
  capture = sDict.published;
  exactOwner = sDict.state == DictationState::RECORDING &&
               sDict.owner == capture.owner;
  if (exactOwner) {
    displaySource = sDict.displaySource;
    displayEpoch = sDict.displayEpoch;
  }
  portEXIT_CRITICAL(&sDictMux);

  // Cancellation already queued an exact-owner delete. Suppress the event and
  // let that debt consume the just-published completion.
  if (!exactOwner) {
    dictationClearPublished(capture.owner);
    return true;
  }

  MicRecordingResult result{};
  const MicRecordingOwnedOp resultOp =
      getRecordingResultOwned(capture.owner, &result);
  const bool resultPublished = resultOp == MicRecordingOwnedOp::OK &&
                               result.valid && result.owner == capture.owner;
  const bool recorderIdle = !micRecordingBusy();
  if (resultOp == MicRecordingOwnedOp::NOT_READY ||
      resultOp == MicRecordingOwnedOp::OWNER_MISMATCH || !recorderIdle) {
    return false;
  }

  const bool samePath = resultPublished && capture.path[0] && result.path[0] &&
                        strcmp(capture.path, result.path) == 0;
  const bool captureSaved = capture.saved && resultPublished &&
                            !result.failed && !result.discarded && samePath;
  if (!dictationRequestPublicationReady(exactOwner, captureSaved,
                                        resultPublished, recorderIdle)) {
    const char* failure = capture.failure[0] ? capture.failure
                          : result.failure[0] ? result.failure
                                              : "capture failed";
    dictationFailAndClean(capture.owner, failure);
    return true;
  }

  if (!displaySessionStillLive(displaySource, displayEpoch)) {
    dictationFailAndClean(capture.owner, "session changed");
    return true;
  }

  const uint32_t hostEpoch = uartLinkSessionEpoch();
  const char* why = nullptr;
  if (!uartLinkIsRunning() ||
      !dictationHostReadyForEpoch(hostEpoch, &why)) {
    dictationFailAndClean(capture.owner, why ? why : "no host session");
    return true;
  }

  // Arm WAITING before the event is visible. A zero-delay host response may run
  // on the UART command task before uartLinkPushEventForSession() returns.
  bool waiting = false;
  portENTER_CRITICAL(&sDictMux);
  if (sDict.state == DictationState::RECORDING &&
      sDict.owner == capture.owner &&
      sDict.displaySource == displaySource &&
      sDict.displayEpoch == displayEpoch) {
    sDict.state = DictationState::WAITING;
    sDict.requestHostEpoch = hostEpoch;
    sDict.requestPushInFlight = false;
    sDict.requestWasPushed = false;
    sDict.stateEnteredMs = millis();
    waiting = true;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (!waiting) {
    dictationClearPublished(capture.owner);
    return true;
  }

  // Final cancel fence. Cancellation that won before this critical section
  // clears owner/state, so no request is emitted. Once `inFlight` is published,
  // cancellation queues a same-session dictate_cancel and the single worker
  // guarantees it is written after this request attempt.
  bool beginPush = false;
  portENTER_CRITICAL(&sDictMux);
  const bool exactWaitingOwner =
      sDict.state == DictationState::WAITING &&
      sDict.owner == capture.owner;
  if (dictationRequestPushFenceReady(
          exactWaitingOwner, sDict.requestHostEpoch == hostEpoch)) {
    sDict.requestPushInFlight = true;
    beginPush = true;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (!beginPush) {
    dictationClearPublished(capture.owner);
    return true;
  }

  char id[17];
  dictFormatId(capture.owner, id);
  char evt[128];
  snprintf(evt, sizeof(evt), "dictate_request %s %s", id, capture.path);
  const bool pushed = uartLinkPushEventForSession(hostEpoch, evt);

  portENTER_CRITICAL(&sDictMux);
  if (sDict.state == DictationState::WAITING &&
      sDict.owner == capture.owner &&
      sDict.requestHostEpoch == hostEpoch) {
    sDict.requestPushInFlight = false;
    if (pushed) sDict.requestWasPushed = true;
  }
  portEXIT_CRITICAL(&sDictMux);
  dictationClearPublished(capture.owner);
  if (!pushed) {
    // TX false is not proof that no complete frame reached the peer. Tombstone
    // this exact session before deleting the WAV.
    dictationQueueCancelEvent(capture.owner, hostEpoch);
    dictationFailAndClean(capture.owner, "host unreachable");
    return true;
  }
  INFO_SYSTEMF("[DICTATE] request pushed id=%s", id);
  return true;
}

// One exact owner at a time. All filesystem work occurs here, never on the
// display/recorder/UART command tasks that publish terminal state.
static bool dictationProcessCleanup() {
  uint64_t owner = 0;
  bool resolved = false;
  char exactPath[64] = {};
  portENTER_CRITICAL(&sDictMux);
  owner = sDict.cleanupOwner;
  resolved = sDict.cleanupResolved;
  memcpy(exactPath, sDict.cleanupPath, sizeof(exactPath));
  portEXIT_CRITICAL(&sDictMux);
  if (!owner) return true;

  MicRecordingResult result{};
  const MicRecordingOwnedOp resultOp = getRecordingResultOwned(owner, &result);
  if (resultOp == MicRecordingOwnedOp::NOT_READY ||
      (resultOp == MicRecordingOwnedOp::NOT_FOUND && !resolved &&
       !exactPath[0])) {
    return false;
  }

  bool clear = false;
  if (resultOp == MicRecordingOwnedOp::OK) {
    if (result.discarded || !result.path[0]) {
      clear = true;
    } else {
      const MicRecordingOwnedOp deleteOp =
          exactPath[0] ? deleteRecordingOwnedPublished(owner, exactPath)
                       : deleteRecordingOwned(owner, nullptr);
      clear = deleteOp == MicRecordingOwnedOp::OK ||
              deleteOp == MicRecordingOwnedOp::NOT_FOUND;
    }
  } else if ((resultOp == MicRecordingOwnedOp::NOT_FOUND ||
              resultOp == MicRecordingOwnedOp::OWNER_MISMATCH) &&
             exactPath[0]) {
    // The completion ring is bounded and may have been displaced while an FS
    // delete was retrying. The callback-retained path is independently tied to
    // this full 64-bit owner before the microphone helper will remove it.
    const MicRecordingOwnedOp deleteOp =
        deleteRecordingOwnedPublished(owner, exactPath);
    clear = deleteOp == MicRecordingOwnedOp::OK ||
            deleteOp == MicRecordingOwnedOp::NOT_FOUND;
  } else if (resultOp == MicRecordingOwnedOp::NOT_FOUND && resolved) {
    // Begin was cancelled before it ever claimed the recorder, or setup failed
    // before creating a path. The exact-owner producer has now resolved, so no
    // future completion for this token can appear.
    clear = true;
  }

  if (clear) {
    portENTER_CRITICAL(&sDictMux);
    if (sDict.cleanupOwner == owner) {
      sDict.cleanupOwner = 0;
      sDict.cleanupResolved = false;
      sDict.cleanupPath[0] = '\0';
      if (sDict.exchangePathOwner == owner) {
        sDict.exchangePathOwner = 0;
        sDict.exchangePath[0] = '\0';
      }
    }
    portEXIT_CRITICAL(&sDictMux);
  }
  return clear;
}

// Admission-time transcript identity for one exchange. An App that keeps its
// own copy of the text (saveTranscript=false) gets the same shape as the
// setting being off, without resolving or retaining any user identity.
static TranscriptOptions dictationTranscriptOptions(CommandSource source,
                                                    TransportSessionEpoch epoch,
                                                    const DictationAppOptions* options) {
  if (!options || options->saveTranscript) return transcriptCaptureOptions(source, epoch);
  TranscriptOptions off;
  off.source = source;
  off.epoch = epoch;
  return off;
}

#if ENABLE_LOCAL_STT
// A local exchange never enters the UART/WAV path. This slot survives UI
// cancellation until a late begin/worker has acknowledged the exact token.
struct LocalDictation {
  uint64_t exchange = 0;
  SttOwner actor;
  SttToken token = 0;
  bool beginPending = false;
  bool beginInFlight = false;
  TranscriptOptions transcriptOptions;
  AudioSource requestedSource = AUDIO_SRC_NONE; // Latched App option; NONE = micsource.
};
static LocalDictation sLocalDict;
static bool sLocalReadyPending = false;
static bool sLocalReadyKnown = false;
static bool sLocalReady = false;
static const char* sLocalReadyReason = nullptr; // Static backend message; sDictMux protected.
static uint32_t sLocalReadyCheckedMs = 0;
static bool dictationEnsureWorker();

static void dictationRefreshLocalReady() {
  bool due;
  portENTER_CRITICAL(&sDictMux);
  due = sLocalReadyPending;
  sLocalReadyPending = false;
  portEXIT_CRITICAL(&sDictMux);
  if (!due) return;
  char error[192] = {};
  const char* reason = nullptr;
  const bool ready = sttLocalAvailable(error, sizeof(error), &reason);
  portENTER_CRITICAL(&sDictMux);
  sLocalReady = ready;
  sLocalReadyReason = reason;
  sLocalReadyKnown = true;
  sLocalReadyCheckedMs = millis();
  portEXIT_CRITICAL(&sDictMux);
}

static bool dictationLocalAvailable(const char** whyNot) {
  if (!dictationEnsureWorker()) {
    if (whyNot) *whyNot = "Transcription worker could not start. Stop other tasks and retry.";
    return false;
  }
  bool known, ready;
  const char* modelReason;
  portENTER_CRITICAL(&sDictMux);
  known = sLocalReadyKnown;
  ready = sLocalReady;
  modelReason = sLocalReadyReason;
  if (!known || millis() - sLocalReadyCheckedMs >= 5000) sLocalReadyPending = true;
  portEXIT_CRITICAL(&sDictMux);
  dictationWakeWorker();
  const char* why = nullptr;
#if ENABLE_ESP_SR
  if (isEspSrRunning() || audioCaptureOwnedBy("sr")) why = "Speech commands are using the mic. Stop speech recognition first.";
#endif
  if (!why && (gMicRunning || micRecordingBusy() || audioCaptureOwnedBy("mic")))
    why = "Mic recording is active. Stop recording before transcription.";
  if (!why && audioCaptureBusy() && !audioCaptureOwnedBy("stt")) why = "Microphone is in use. Stop the other audio session and retry.";
  if (!why && !known) why = "Checking speech model. Please wait.";
  if (!why && !ready) why = modelReason ? modelReason : "Speech model unavailable. Install the model for this build.";
  if (whyNot) *whyNot = why;
  return why == nullptr;
}

static bool dictationBeginLocal(CommandSource source, TransportSessionEpoch epoch,
                                 DictationAppLease* app = nullptr,
                                 const DictationAppOptions* options = nullptr) {
  const char* why = nullptr;
  if (!dictationAvailable(&why)) return false;
  const TranscriptOptions transcriptOptions = dictationTranscriptOptions(source, epoch, options);
  const AudioSource requestedSource = options ? options->preferredSource : AUDIO_SRC_NONE;
  const uint32_t nonce = esp_random();
  bool admitted = false;
  portENTER_CRITICAL(&sDictMux);
  if (!sLocalDict.exchange &&
      (sDict.state == DictationState::IDLE || sDict.state == DictationState::FAILED) &&
      !sDict.cleanupOwner && !sDict.micStopPending && !sDict.published.pending &&
      !sDict.cancelEvent.pending && !sDictSave.pending && !sDictSave.inFlight) {
    if (!sDictBootNonce) sDictBootNonce = nonce ? nonce : 1;
    if (++sDictCounter == 0) ++sDictCounter;
    const uint64_t id = (static_cast<uint64_t>(sDictBootNonce) << 32) | sDictCounter;
    sLocalDict = LocalDictation{};
    sLocalDict.exchange = id;
    sLocalDict.actor = SttOwner{source, epoch};
    sLocalDict.transcriptOptions = transcriptOptions;
    sLocalDict.requestedSource = requestedSource;
    sDict.transcriptOptions = transcriptOptions;
    sDict.transcriptStatus = TranscriptStatus{};
    sDict.transcriptStatus.enabled = transcriptOptions.enabled;
    sDict.transcriptExchange = id;
    sLocalDict.beginPending = true;
    sDict.owner = id;
    dictationAdmitConsumerLocked(source, epoch, id, app);
    sDict.displaySource = source;
    sDict.displayEpoch = epoch;
    sDict.state = DictationState::RECORDING;
    sDict.stateEnteredMs = millis();
    dictationClearDeliveryLocked();
    sDict.continuous = true;
    sDict.stopRequested = false;
    sDict.failure[0] = '\0';
    sDict.weStartedMic = false;
    sDict.requestHostEpoch = 0;
    sDict.requestPushInFlight = sDict.requestWasPushed = false;
    admitted = true;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (admitted) dictationWakeWorker();
  return admitted;
}

static bool dictationCancelLocal(CommandSource source, bool force,
                                 const char* failure = nullptr,
                                 const DictationAppLease* app = nullptr) {
  bool mine = false;
  portENTER_CRITICAL(&sDictMux);
  mine = force || dictationConsumerMatchesLocked(source, app);
  if (mine) {
    sDict.owner = 0; // Retained job identity below observes this and joins cancel.
    sDict.appSpeakers = SttSpeakerSnapshot{};
    sDict.state = failure ? DictationState::FAILED : DictationState::IDLE;
    sDict.stateEnteredMs = millis();
    if (!failure) {
      sDict.displaySource = SOURCE_INTERNAL;
      sDict.displayEpoch = 0;
    }
    dictationClearDeliveryLocked();
    snprintf(sDict.failure, sizeof(sDict.failure), "%s", failure ? failure : "");
  }
  portEXIT_CRITICAL(&sDictMux);
  if (mine) dictationWakeWorker();
  return mine;
}

static bool dictationLocalCurrentLocked(const LocalDictation& run) {
  return run.exchange && sDict.owner == run.exchange &&
      sDict.displaySource == run.actor.source && sDict.displayEpoch == run.actor.epoch;
}

static void dictationFailLocal(const LocalDictation& run, const char* reason) {
  portENTER_CRITICAL(&sDictMux);
  if (dictationLocalCurrentLocked(run)) {
    sDict.appSpeakers = SttSpeakerSnapshot{};
    sDict.owner = 0;
    dictationClearDeliveryLocked();
    sDict.state = DictationState::FAILED;
    sDict.stateEnteredMs = millis();
    snprintf(sDict.failure, sizeof(sDict.failure), "%s", reason);
  }
  portEXIT_CRITICAL(&sDictMux);
  dictationWakeWorker();
}

static void dictationProcessLocal() {
  dictationRefreshLocalReady();
  LocalDictation run;
  bool current = false, finish = false;
  portENTER_CRITICAL(&sDictMux);
  run = sLocalDict;
  current = dictationLocalCurrentLocked(run);
  if (run.beginPending) {
    sLocalDict.beginPending = false;
    sLocalDict.beginInFlight = true;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (!run.exchange) return;

  if (run.beginPending) {
    SttToken token = 0;
    char error[192] = {};
    const bool accepted = current &&
        displaySessionStillLive(run.actor.source, run.actor.epoch) &&
        sttBeginContinuous(run.actor, &token, error, sizeof(error), &run.transcriptOptions,
                           run.requestedSource);
    portENTER_CRITICAL(&sDictMux);
    if (sLocalDict.exchange == run.exchange) {
      sLocalDict.beginInFlight = false;
      if (accepted) sLocalDict.token = token;
      if (!accepted) {
        if (dictationLocalCurrentLocked(run)) {
          sDict.owner = 0;
          sDict.state = DictationState::FAILED;
          sDict.stateEnteredMs = millis();
          snprintf(sDict.failure, sizeof(sDict.failure), "%s",
                   error[0] ? error : "session changed");
        }
        sLocalDict = LocalDictation{};
      }
      run = sLocalDict;
    }
    portEXIT_CRITICAL(&sDictMux);
    if (!accepted) return;
  }
  if (!run.token) return;

  portENTER_CRITICAL(&sDictMux);
  current = dictationLocalCurrentLocked(run);
  finish = sDict.stopRequested;
  portEXIT_CRITICAL(&sDictMux);
  const bool live = displaySessionStillLive(run.actor.source, run.actor.epoch);
  if (!current || !live) {
    // The broker self-cancels on revocation. Metadata-only join still works
    // after that identity can no longer read status or invoke cancellation.
    (void)sttCancel(run.actor, run.token);
    if (!live && current) dictationFailLocal(run, "session changed");
    if (!sttRunActive(run.token)) {
      SttSnapshot terminalSnapshot;
      const bool haveStatus = sttSnapshot(run.actor, run.token, &terminalSnapshot);
      portENTER_CRITICAL(&sDictMux);
      if (haveStatus && sDict.transcriptExchange == run.exchange)
        sDict.transcriptStatus = terminalSnapshot.transcript;
      if (sLocalDict.exchange == run.exchange) sLocalDict = LocalDictation{};
      portEXIT_CRITICAL(&sDictMux);
    }
    return;
  }
  if (finish) (void)sttRequestFinish(run.actor, run.token);

  uint32_t ack = 0;
  portENTER_CRITICAL(&sDictMux);
  if (sDict.deliveryExchange == run.exchange && sDict.deliveryAckPending)
    ack = sDict.deliverySequence;
  portEXIT_CRITICAL(&sDictMux);
  if (ack) {
    if (!sttAcknowledgeChunk(run.actor, run.token, ack)) {
      dictationFailLocal(run, "text acknowledgment failed");
      return;
    }
    portENTER_CRITICAL(&sDictMux);
    if (sDict.deliveryExchange == run.exchange && sDict.deliverySequence == ack &&
        sDict.deliveryAckPending) dictationClearDeliveryLocked();
    portEXIT_CRITICAL(&sDictMux);
  }

  SttSnapshot snap;
  if (!sttSnapshot(run.actor, run.token, &snap)) {
    dictationFailLocal(run, "session changed");
    return;
  }
  if (snap.state == SttState::Failed || snap.state == SttState::Cancelled) {
    dictationFailLocal(run, snap.error[0] ? snap.error : "local transcription failed");
    return;
  }

  SttSpeakerSnapshot speakers;
  (void)sttReadSpeakers(run.actor, run.token, &speakers);
  uint16_t offset = 0;
  uint32_t sequence = 0;
  bool canStage = false;
  portENTER_CRITICAL(&sDictMux);
  if (dictationLocalCurrentLocked(run)) {
    sDict.transcriptStatus = snap.transcript;
    sDict.appSpeakers = speakers;
    const DictationState state = !sDict.stopRequested &&
        (snap.captureActive || snap.state == SttState::Preparing)
            ? DictationState::RECORDING : DictationState::WAITING;
    if (sDict.state != state) { sDict.state = state; sDict.stateEnteredMs = millis(); }
    canStage = !sDict.textPending && !sDict.deliveryAckPending;
    offset = sDict.deliveryOffset;
    sequence = sDict.deliverySequence;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (canStage) {
    SttTextChunk chunk;
    if (sttReadChunk(run.actor, run.token, &chunk)) {
      const size_t length = strnlen(chunk.text, sizeof(chunk.text));
      const bool valid = length <= STT_MAX_TEXT &&
          dictationPrintableAscii(chunk.text, length, /*allowEmpty=*/true) &&
          chunk.sequence && (!sequence || sequence == chunk.sequence) && offset <= length;
      const bool stillLive = displaySessionStillLive(run.actor.source, run.actor.epoch);
      if (!valid) {
        memset(chunk.text, 0, sizeof(chunk.text));
        dictationFailLocal(run, "invalid local text chunk");
        return;
      }
      portENTER_CRITICAL(&sDictMux);
      if (dictationLocalCurrentLocked(run) && !sDict.textPending &&
          !sDict.deliveryAckPending && sDict.deliveryOffset == offset && stillLive)
        dictationStageTextLocked(run.exchange, chunk.sequence, chunk.text,
                                 length, offset, /*needsAck=*/true);
      portEXIT_CRITICAL(&sDictMux);
      memset(chunk.text, 0, sizeof(chunk.text));
    }
  }

  if (sttRunActive(run.token)) return;
  // Completion may publish its final chunk after the earlier snapshot/read.
  // Recheck the same authenticated job once inactive before discarding it.
  if (!sttSnapshot(run.actor, run.token, &snap)) {
    dictationFailLocal(run, "session changed");
    return;
  }
  if (snap.state == SttState::Failed || snap.state == SttState::Cancelled) {
    dictationFailLocal(run, snap.error[0] ? snap.error : "local transcription failed");
    return;
  }
  (void)sttReadSpeakers(run.actor, run.token, &speakers);
  bool terminal = false;
  portENTER_CRITICAL(&sDictMux);
  if (dictationLocalCurrentLocked(run) && !snap.workerActive && snap.pendingTexts == 0 &&
      !sDict.deliveryExchange && !sDict.textPending) {
    sDict.transcriptStatus = snap.transcript;
    sDict.appSpeakers = speakers;
    dictationFinishInputLocked();
    terminal = true;
    if (sLocalDict.exchange == run.exchange) sLocalDict = LocalDictation{};
  }
  portEXIT_CRITICAL(&sDictMux);
  if (terminal) (void)sttCancel(run.actor, run.token);
}
#endif // ENABLE_LOCAL_STT

static void dictationProcessSave() {
  DictationSaveJob job;
  portENTER_CRITICAL(&sDictMux);
  if (sDictSave.pending && !sDictSave.inFlight) {
    job = sDictSave;
    sDictSave.pending = false;
    sDictSave.inFlight = true;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (!job.exchange) return;
  TranscriptSession transcript;
  transcript.begin(job.options, "pi", job.exchange);
  (void)transcript.append(1, job.text, strlen(job.text));
  transcript.finish("done");
  const TranscriptStatus status = transcript.snapshot();
  if (status.error[0]) INFO_SYSTEMF("[DICTATE] Transcript saving failed: %s", status.error);
  portENTER_CRITICAL(&sDictMux);
  if (sDict.transcriptExchange == job.exchange &&
      sDict.transcriptOptions.source == job.options.source &&
      sDict.transcriptOptions.epoch == job.options.epoch)
    sDict.transcriptStatus = status;
  if (sDictSave.exchange == job.exchange) sDictSave = DictationSaveJob{};
  portEXIT_CRITICAL(&sDictMux);
  volatile char* privateText = job.text;
  for (size_t i = 0; i < sizeof(job.text); ++i) privateText[i] = 0;
}

// The work predicate, sDictMux held: dictationWorkerHasWork takes the lock
// for it; the worker's idle exit reads it in the critical section that nulls
// the handle.
static bool dictationWorkPendingLocked() {
  return
#if ENABLE_LOCAL_STT
         sLocalReadyPending || sLocalDict.exchange != 0 ||
#endif
         sDict.owner != 0 || sDict.deliveryExchange != 0 ||
         sDictSave.pending || sDictSave.inFlight ||
         sDict.published.pending || sDict.cancelEvent.pending ||
         sDict.cleanupOwner != 0 ||
         (sDict.micStopPending && !sDict.owner);
}

static bool dictationWorkerHasWork() {
  portENTER_CRITICAL(&sDictMux);
  const bool work = dictationWorkPendingLocked();
  portEXIT_CRITICAL(&sDictMux);
  return work;
}

// The worker's own exit after an idle wait (SERVICE_TASK_IDLE_RETIRE_MS with
// nothing pending). ServiceWorker::retireIf reads the predicate and nulls the
// handle in one hold of sDictMux, so work published after it finds no worker
// and creates one (dictationWakeWorker).
static bool dictationRetireIfIdle() {
  return sDictWorker.retireIf([] { return !dictationWorkPendingLocked(); });
}

static void dictationWorkerBody(void*) {
  for (;;) {
    const bool woken = sDictWorker.wait(SERVICE_TASK_IDLE_RETIRE_MS);
    while (dictationWorkerHasWork()) {
      dictationSupervise(); // Independent of the visible keyboard/web page.
#if ENABLE_LOCAL_STT
      dictationProcessLocal();
#endif
      dictationProcessSave();
      (void)dictationProcessPublished();
      dictationProcessCancelEvent();
      (void)dictationProcessCleanup();
      dictationReleaseMicIfDue();
      if (dictationWorkerHasWork()) (void)sDictWorker.wait(kDictationWorkerRetryMs);
    }
    if (!woken && dictationRetireIfIdle()) vTaskDelete(nullptr);
  }
}

static bool dictationEnsureWorker() {
  if (sDictWorker.present()) return true;
  if (!sDictWorker.ensure(dictationWorkerBody, "dictation_svc", kDictationWorkerStackBytes,
                          "dictation.service")) {
    if (!sDictWorker.present()) WARN_SYSTEMF("[DICTATE] service task unavailable");
    return false;
  }
  dictationWakeWorker();
  return true;
}

bool dictationAvailable(const char** whyNot) {
  if (!audioAnySourceAvailable()) {
    // Covers both boards with no PDM silicon and a board whose glasses are not
    // connected right now. Either way there is nothing to record with.
    if (whyNot) *whyNot = "No microphone available. Connect glasses or enable a supported mic.";
    return false;
  }
#if ENABLE_LOCAL_STT
  // An enabled local provider never silently exports microphone audio to a
  // host, even if its model is missing or its inference fails.
  return dictationLocalAvailable(whyNot);
#endif
  if (!uartLinkIsRunning()) {
    if (whyNot) *whyNot = ENABLE_UART_HOST_LINK
        ? "CM5 UART link is off. Connect the CM5 and enable its host link."
        : "No transcription engine enabled. Use a local STT build or a CM5 host build.";
    return false;
  }
  const uint32_t hostEpoch = uartLinkSessionEpoch();
  if (hostEpoch == 0) {
    if (whyNot) *whyNot = "CM5 host is not signed in. Authenticate its UART session.";
    return false;
  }
  return dictationHostReadyForEpoch(hostEpoch, whyNot);
}

// `options` is null for the keyboard consumers; an App passes its own (the
// defaults reproduce the keyboard behaviour exactly).
static bool dictationBeginImpl(CommandSource displaySource,
                               TransportSessionEpoch displayEpoch,
                               DictationAppLease* app,
                               const DictationAppOptions* options) {
  if ((displaySource != SOURCE_LOCAL_DISPLAY &&
       displaySource != SOURCE_G2_GLASSES &&
       !(app && displaySource == SOURCE_WEB)) ||
      displayEpoch == kNoTransportSessionEpoch) return false;
  if (!displaySessionStillLive(displaySource, displayEpoch)) return false;

#if ENABLE_LOCAL_STT
  return dictationBeginLocal(displaySource, displayEpoch, app, options);
#endif

  if (!dictationEnsureWorker()) return false;

  const char* why = nullptr;
  if (!dictationAvailable(&why)) return false;

  const AudioSource preferredSource = options ? options->preferredSource : AUDIO_SRC_NONE;
  const uint32_t silenceStopMs = options && options->silenceStopMs
      ? options->silenceStopMs : kDictationVadSilenceMs;

  // Refuse a second arm rather than stacking captures; the recorder would
  // reject the start anyway. Cleanup/publication debt is also an admission
  // fence: its fixed slot must never be overwritten by a later path.
  const TranscriptOptions transcriptOptions =
      dictationTranscriptOptions(displaySource, displayEpoch, options);
  uint32_t nonceCandidate = esp_random();
  if (!nonceCandidate) nonceCandidate = 1;
  bool armed = false;
  uint64_t owner = 0;
  portENTER_CRITICAL(&sDictMux);
  if ((sDict.state == DictationState::IDLE ||
       sDict.state == DictationState::FAILED) &&
      !sDict.micStopPending && !sDict.cleanupOwner &&
      !sDict.published.pending && !sDict.cancelEvent.pending &&
      !sDictSave.pending && !sDictSave.inFlight) {
    if (!sDictBootNonce) sDictBootNonce = nonceCandidate;
    if (++sDictCounter == 0) ++sDictCounter;
    owner = ((uint64_t)sDictBootNonce << 32) | (uint64_t)sDictCounter;
    sDict.state = DictationState::RECORDING;
    sDict.owner = owner;
    dictationAdmitConsumerLocked(displaySource, displayEpoch, owner, app);
    sDict.transcriptOptions = transcriptOptions;
    sDict.transcriptStatus = TranscriptStatus{};
    sDict.transcriptStatus.enabled = transcriptOptions.enabled;
    sDict.transcriptExchange = owner;
    sDict.stateEnteredMs = millis();
    sDict.failure[0] = '\0';
    dictationClearDeliveryLocked();
    sDict.continuous = false;
    sDict.stopRequested = false;
    sDict.weStartedMic = false;
    sDict.displaySource = displaySource;
    sDict.displayEpoch = displayEpoch;
    sDict.requestHostEpoch = 0;
    sDict.requestPushInFlight = false;
    sDict.requestWasPushed = false;
    sDict.exchangePathOwner = 0;
    sDict.exchangePath[0] = '\0';
    armed = true;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (!armed) return false;

  // Availability and display authority may change between the optimistic gate
  // and the atomic claim. Every slow-boundary failure is exact-owner fenced.
  if (!displaySessionStillLive(displaySource, displayEpoch) ||
      !dictationAvailable(&why)) {
    (void)dictFailOwned(owner, why ? why : "session changed");
    dictationResolveCleanup(owner);
    dictationWakeWorker();
    return false;
  }

  // The mic may be idle — bring it up, but remember that WE did, so the wearer's
  // own openmic/closemic state survives a dictation untouched.
  bool startedMic = false;
  if (!gMicRunning) {
    if (!initMicrophone(preferredSource)) {
      (void)dictFailOwned(owner, "mic would not start");
      dictationResolveCleanup(owner);
      dictationWakeWorker();
      return false;
    }
    startedMic = true;
  } else if (preferredSource != AUDIO_SRC_NONE &&
             audioGetSource() != preferredSource) {
    // Someone else's mic is never re-sourced for an exchange: nothing here
    // could hand it back, and a moved G2 stream would outlive the note (R15).
    (void)dictFailOwned(owner, "microphone in use on another source");
    dictationResolveCleanup(owner);
    dictationWakeWorker();
    return false;
  }

  // Cancellation may run while initMicrophone() is opening the source. Publish
  // our source ownership only if the same admitted exchange still exists;
  // otherwise let the service worker hand it back.
  bool stillCurrent = false;
  portENTER_CRITICAL(&sDictMux);
  stillCurrent = sDict.state == DictationState::RECORDING &&
                 sDict.owner == owner &&
                 sDict.displaySource == displaySource &&
                 sDict.displayEpoch == displayEpoch;
  if (stillCurrent) {
    sDict.weStartedMic = startedMic;
  } else if (startedMic) {
    sDict.micStopPending = true;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (!stillCurrent) {
    dictationResolveCleanup(owner);
    dictationWakeWorker();
    return false;
  }

  if (!displaySessionStillLive(displaySource, displayEpoch) ||
      !dictationAvailable(&why)) {
    (void)dictFailOwned(owner, why ? why : "session changed");
    dictationResolveCleanup(owner);
    dictationWakeWorker();
    return false;
  }

  // Last owner check before calling into the recorder. Cancellation can still
  // linearize during startRecordingOwned(); the post-call check below catches
  // that window and issues a second exact-owner discard after the claim exists.
  portENTER_CRITICAL(&sDictMux);
  stillCurrent = sDict.state == DictationState::RECORDING &&
                 sDict.owner == owner;
  portEXIT_CRITICAL(&sDictMux);
  if (!stillCurrent) {
    dictationResolveCleanup(owner);
    dictationWakeWorker();
    return false;
  }

  // Source-agnostic by construction: startRecordingOwned re-resolves the
  // preference against what is connected now (or checks this exchange's
  // requested source, strictly) and owns every G2-only step.
  if (!startRecordingOwned(owner, silenceStopMs, /*trim=*/true, preferredSource)) {
    // A false start is not synonymous with contention. Setup can also fail
    // after the lifecycle claim (guarded WAV create/header/task startup), in
    // which case the recorder publishes the exact owner-scoped failure before
    // returning. Preserve that diagnosis for the UI; only call it busy when a
    // different live/finalizing capture actually owns the recorder.
    MicRecordingResult result{};
    const MicRecordingOwnedOp op = getRecordingResultOwned(owner, &result);
    const char* failure = "recorder would not start";
    if (op == MicRecordingOwnedOp::OK && result.failed && result.failure[0]) {
      failure = result.failure;
    } else if (micRecordingBusy()) {
      failure = "recorder busy";
    }
    INFO_SYSTEMF("[DICTATE] recorder start failed: %s", failure);
    (void)dictFailOwned(owner, failure);
    // A setup failure may retain an exact-owner partial WAV. Queueing every
    // false start is cheap; proven NOT_FOUND clears once this begin resolves.
    (void)dictationQueueCleanup(owner, /*resolved=*/true);
    dictationResolveCleanup(owner);
    dictationWakeWorker();
    return false;
  }

  portENTER_CRITICAL(&sDictMux);
  stillCurrent = sDict.state == DictationState::RECORDING &&
                 sDict.owner == owner;
  portEXIT_CRITICAL(&sDictMux);
  if (!stillCurrent) {
    // Cancel may have reached an IDLE recorder before this call claimed it. Its
    // first stop then returned NOT_FOUND; this post-start stop closes the race.
    (void)requestStopRecordingOwned(owner, /*discard=*/true);
    // A successful start has a future terminal publication. Leave the debt
    // unresolved until that callback carries the stable path; otherwise a
    // scheduler gap between IDLE publication and this module's callback could
    // erase the only late-delete obligation.
    (void)dictationQueueCleanup(owner, /*resolved=*/false);
    return false;
  }

  char id[17];
  dictFormatId(owner, id);
  INFO_SYSTEMF("[DICTATE] armed id=%s source=%s", id,
               sourceLabel(audioGetSource()));
  dictationWakeWorker();
  return true;
}

bool dictationBeginFor(CommandSource source, TransportSessionEpoch epoch) {
  return dictationBeginImpl(source, epoch, nullptr, nullptr);
}

bool dictationAppBeginWith(CommandSource source, TransportSessionEpoch epoch,
                           const DictationAppOptions& options,
                           DictationAppLease* lease) {
  if (!lease) return false;
  *lease = DictationAppLease{};
  lease->purpose = options.purpose;
  if (dictationBeginImpl(source, epoch, lease, &options)) return true;
  *lease = DictationAppLease{};
  return false;
}

bool dictationAppBegin(CommandSource source, TransportSessionEpoch epoch,
                       DictationAppLease* lease) {
  return dictationAppBeginWith(source, epoch, DictationAppOptions{}, lease);
}

bool dictationBegin(TransportSessionEpoch displayEpoch) {
  return dictationBeginFor(SOURCE_LOCAL_DISPLAY, displayEpoch);
}

static bool dictationRequestStopImpl(CommandSource displaySource,
                                     const DictationAppLease* app = nullptr) {
#if ENABLE_LOCAL_STT
  portENTER_CRITICAL(&sDictMux);
  const bool mine = dictationConsumerMatchesLocked(displaySource, app);
  if (mine && sLocalDict.exchange && sDict.owner == sLocalDict.exchange)
    sDict.stopRequested = true;
  portEXIT_CRITICAL(&sDictMux);
  if (mine) dictationWakeWorker();
  return mine;
#else
  uint64_t owner = 0;
  portENTER_CRITICAL(&sDictMux);
  const bool mine = dictationConsumerMatchesLocked(displaySource, app);
  if (mine && sDict.state == DictationState::RECORDING) owner = sDict.owner;
  portEXIT_CRITICAL(&sDictMux);
  if (!owner) return mine;
  // Non-blocking: this runs on the display task, which must not stall a frame
  // waiting for FINALIZING. The terminal hook picks it up from the recorder.
  requestStopRecordingOwned(owner, /*discard=*/false);
  return true;
#endif
}

void dictationRequestStopFor(CommandSource source) {
  (void)dictationRequestStopImpl(source);
}

bool dictationAppRequestStop(const DictationAppLease& lease) {
  return displaySessionStillLive(lease.source, lease.epoch) &&
      dictationRequestStopImpl(lease.source, &lease);
}

void dictationRequestStop() {
  dictationRequestStopFor(SOURCE_LOCAL_DISPLAY);
}

static bool dictationCancelImpl(CommandSource displaySource, bool force,
                                const char* failure = nullptr,
                                const DictationAppLease* app = nullptr) {
#if ENABLE_LOCAL_STT
  return dictationCancelLocal(displaySource, force, failure, app);
#endif
  uint64_t owner = 0;
  bool mine = false;
  DictationState priorState = DictationState::IDLE;
  bool cancelHostWork = false;
  uint32_t cancelHostEpoch = 0;
  portENTER_CRITICAL(&sDictMux);
  mine = force || dictationConsumerMatchesLocked(displaySource, app);
  if (mine) {
    owner = sDict.owner;
    priorState = sDict.state;
    cancelHostWork = dictationCancelEventNeeded(
        sDict.requestPushInFlight, sDict.requestWasPushed);
    cancelHostEpoch = sDict.requestHostEpoch;
    sDict.state = failure ? DictationState::FAILED : DictationState::IDLE;
    sDict.stateEnteredMs = millis();
    sDict.owner = 0;
    if (!failure) {
      sDict.displaySource = SOURCE_INTERNAL;
      sDict.displayEpoch = kNoTransportSessionEpoch;
    }
    sDict.requestHostEpoch = 0;
    sDict.requestPushInFlight = false;
    sDict.requestWasPushed = false;
    sDict.micStopPending = sDict.weStartedMic;
    dictationClearDeliveryLocked();
    snprintf(sDict.failure, sizeof(sDict.failure), "%s", failure ? failure : "");
  }
  portEXIT_CRITICAL(&sDictMux);

  if (!mine) return false;

  if (cancelHostWork) {
    dictationQueueCancelEvent(owner, cancelHostEpoch);
  }
  if (owner) {
    // Queue the late-delete before requesting stop. If begin is still between
    // admission and recorder claim, `resolved=false` prevents a transient
    // NOT_FOUND from erasing the debt; begin resolves it after its last start
    // boundary. No filesystem or join work runs on this display task.
    // RECORDING may still be before, inside, or after recorder admission. Its
    // terminal callback resolves the debt; if cancellation beat admission,
    // dictationBeginFor resolves it after proving no producer can appear.
    (void)dictationQueueCleanup(
        owner, /*resolved=*/priorState != DictationState::RECORDING);
    (void)requestStopRecordingOwned(owner, /*discard=*/true);
  }
  dictationWakeWorker();
  return true;
}

bool dictationAppCancel(const DictationAppLease& lease) {
  return displaySessionStillLive(lease.source, lease.epoch) &&
      dictationCancelImpl(lease.source, false, nullptr, &lease);
}

void dictationCancelFor(CommandSource displaySource) {
  dictationCancelImpl(displaySource, /*force*/ false);
}

void dictationCancel() {
  dictationCancelFor(SOURCE_LOCAL_DISPLAY);
}

void dictationResetForSessionBoundary() {
  // This hook belongs to the OLED/local-display identity boundary. A G2
  // dictation has an independent paired-owner epoch and must not be cancelled
  // just because the OLED user logs in or out while the glasses are speaking.
  dictationCancelFor(SOURCE_LOCAL_DISPLAY);
}

void dictationFieldFullFor(CommandSource source) {
  // All providers stop through their existing cancellation/cleanup path. The
  // visible failure explains why remaining speech/text was discarded.
  dictationCancelImpl(source, /*force=*/false, "Text field full; stopped");
}

DictationSnapshot dictationSnapshotNow() {
  DictationSnapshot out{};
  const uint32_t now = millis();
  portENTER_CRITICAL(&sDictMux);
  out.state = sDict.state;
  out.ownerSource = sDict.displaySource;
  out.continuous = sDict.continuous;
  out.transcript = sDict.transcriptStatus;
  out.transcript.path[0] = '\0'; // No requesting owner in this global UI snapshot.
  out.elapsedMs = (uint32_t)(now - sDict.stateEnteredMs);
  snprintf(out.failure, sizeof(out.failure), "%s", sDict.failure);
  portEXIT_CRITICAL(&sDictMux);
  out.captureActive = out.state == DictationState::RECORDING;
  out.inferenceActive = out.state == DictationState::WAITING;
  out.sourceName = sourceLabel(audioGetSource());
  out.level = (out.state == DictationState::RECORDING) ? getAudioLevel() : 0;
#if ENABLE_LOCAL_STT
  LocalDictation local;
  portENTER_CRITICAL(&sDictMux);
  local = sLocalDict;
  portEXIT_CRITICAL(&sDictMux);
  SttSnapshot snap;
  if (local.token && sttSnapshot(local.actor, local.token, &snap)) {
    out.sourceName = sourceLabel(static_cast<AudioSource>(snap.audioSource));
    out.level = snap.level;
    out.transcript = snap.transcript;
    out.transcript.path[0] = '\0';
    out.preparing = snap.state == SttState::Preparing;
    out.captureActive = snap.captureActive;
    out.inferenceActive = snap.inferenceActive;
    out.elapsedMs = static_cast<uint32_t>(std::min<uint64_t>(snap.sessionMs, UINT32_MAX));
  } else {
    out.level = 0;
    out.preparing = local.beginPending || local.beginInFlight;
  }
#endif
  return out;
}

bool dictationAppCurrent(CommandSource source, TransportSessionEpoch epoch,
                         DictationAppLease* lease, DictationAppPurpose purpose) {
  if (!lease) return false;
  *lease = DictationAppLease{};
  if (!displaySessionStillLive(source, epoch)) return false;
  portENTER_CRITICAL(&sDictMux);
  if (sDict.appConsumer && sDict.appLease.source == source &&
      sDict.appLease.epoch == epoch && sDict.appLease.purpose == purpose)
    *lease = sDict.appLease;
  portEXIT_CRITICAL(&sDictMux);
  if (!displaySessionStillLive(source, epoch)) {
    *lease = DictationAppLease{};
    return false;
  }
  return lease->exchange != 0;
}

bool dictationAppSnapshot(const DictationAppLease& lease,
                          DictationAppSnapshot* out) {
  if (!out) return false;
  *out = DictationAppSnapshot{};
  if (!displaySessionStillLive(lease.source, lease.epoch)) return false;
  bool mine;
  portENTER_CRITICAL(&sDictMux);
  mine = dictationConsumerMatchesLocked(lease.source, &lease);
  portEXIT_CRITICAL(&sDictMux);
  DictationSnapshot status{};
  if (mine) status = dictationSnapshotNow();
  portENTER_CRITICAL(&sDictMux);
  out->busy = sDict.owner || sDict.textPending || sDict.deliveryExchange ||
      sDict.micStopPending || sDict.cleanupOwner || sDict.published.pending ||
      sDict.cancelEvent.pending || sDictSave.pending || sDictSave.inFlight;
#if ENABLE_LOCAL_STT
  out->busy = out->busy || sLocalDict.exchange != 0;
#endif
  // Recheck after the provider snapshot: a new admission may have replaced it.
  if (mine && dictationConsumerMatchesLocked(lease.source, &lease)) {
    out->valid = true;
    out->active = out->busy;
    out->done = !out->active;
    out->textPending = sDict.textPending;
    out->status = status;
    out->status.ownerSource = lease.source;
    if (sDict.transcriptExchange == lease.exchange)
      snprintf(out->status.transcript.path, sizeof(out->status.transcript.path),
               "%s", sDict.transcriptStatus.path);
  }
  portEXIT_CRITICAL(&sDictMux);
  if (!displaySessionStillLive(lease.source, lease.epoch)) {
    *out = DictationAppSnapshot{};
    return false;
  }
  return out->valid;
}

static void dictationSupervise() {
#if ENABLE_LOCAL_STT
  // The local worker and broker supervise the capture/engine. Host heartbeat,
  // WAV publication and UART response deadlines do not apply to this provider.
  return;
#endif
  DictationState state;
  uint32_t elapsed;
  uint64_t owner, delivery;
  CommandSource source;
  TransportSessionEpoch epoch;
  bool cancelHostWork = false;
  uint32_t requestHostEpoch = 0;
  const uint32_t now = millis();
  portENTER_CRITICAL(&sDictMux);
  state = sDict.state;
  elapsed = (uint32_t)(now - sDict.stateEnteredMs);
  owner = sDict.owner;
  delivery = sDict.deliveryExchange;
  source = sDict.displaySource;
  epoch = sDict.displayEpoch;
  if (state == DictationState::WAITING) {
    cancelHostWork = dictationCancelEventNeeded(
        sDict.requestPushInFlight, sDict.requestWasPushed);
    requestHostEpoch = sDict.requestHostEpoch;
  }
  portEXIT_CRITICAL(&sDictMux);

  if ((owner || delivery) && !displaySessionStillLive(source, epoch)) {
    if (owner && dictFailOwned(owner, "session changed")) {
      if (cancelHostWork) dictationQueueCancelEvent(owner, requestHostEpoch);
      (void)dictationQueueCleanup(owner, state != DictationState::RECORDING);
      (void)requestStopRecordingOwned(owner, /*discard=*/true);
    } else if (!owner && delivery) {
      // Accepted Pi text can outlive capture. A vanished page/login must not
      // strand its mailbox or expose it to a later user of that transport.
      portENTER_CRITICAL(&sDictMux);
      if (!sDict.owner && sDict.deliveryExchange == delivery &&
          sDict.displaySource == source && sDict.displayEpoch == epoch)
        dictationFinishInputLocked();
      portEXIT_CRITICAL(&sDictMux);
    }
    return;
  }
  if (!owner) return;

  if (state == DictationState::RECORDING &&
      elapsed >= MIC_STT_MAX_CAPTURE_MS) {
    // Cap reached without the VAD ever seeing silence. Stop the capture and let
    // the terminal hook take it from there — a long noisy take may still hold a
    // usable phrase, so this is a stop, not a discard.
    if (owner) requestStopRecordingOwned(owner, /*discard=*/false);
    return;
  }

  if (state == DictationState::WAITING) {
    const uint32_t liveHostEpoch = uartLinkSessionEpoch();
    const bool hostReady =
        dictationHostReadyForEpoch(requestHostEpoch, nullptr);
    if (!dictationWaitingHostReady(
            uartLinkIsRunning(), requestHostEpoch, liveHostEpoch, hostReady)) {
      if (cancelHostWork) {
        dictationQueueCancelEvent(owner, requestHostEpoch);
      }
      if (dictFailOwned(owner, "host session lost")) {
        (void)dictationQueueCleanup(owner, /*resolved=*/true);
      }
      return;
    }
  }

  if (state == DictationState::WAITING &&
      dictationHostResultExpired(elapsed)) {
    if (cancelHostWork) {
      dictationQueueCancelEvent(owner, requestHostEpoch);
    }
    if (dictFailOwned(owner, "host did not answer")) {
      (void)dictationQueueCleanup(owner, /*resolved=*/true);
    }
  }

}

void dictationTick() {
  dictationSupervise();
  if (dictationWorkerHasWork()) dictationWakeWorker();
}

static bool dictationPeekTextImpl(CommandSource source, char* out, size_t outSize,
                                  DictationTextReceipt* receipt,
                                  const DictationAppLease* app = nullptr) {
  if (!out || !outSize || !receipt) return false;
  out[0] = '\0';
  *receipt = DictationTextReceipt{};
  TransportSessionEpoch epoch = 0;
  portENTER_CRITICAL(&sDictMux);
  if (dictationConsumerMatchesLocked(source, app) && sDict.textPending) epoch = sDict.displayEpoch;
  portEXIT_CRITICAL(&sDictMux);
  if (!epoch) return false;
  if (!displaySessionStillLive(source, epoch)) {
    portENTER_CRITICAL(&sDictMux);
    if (dictationConsumerMatchesLocked(source, app) && sDict.displayEpoch == epoch) {
      if (!sDict.owner) dictationFinishInputLocked();
      else dictationClearDeliveryLocked();
    }
    portEXIT_CRITICAL(&sDictMux);
    dictationWakeWorker();
    return false;
  }
  portENTER_CRITICAL(&sDictMux);
  if (dictationConsumerMatchesLocked(source, app) && sDict.displayEpoch == epoch &&
      sDict.deliveryExchange && sDict.textPending && outSize > 1) {
    const size_t count = std::min(strlen(sDict.text), outSize - 1);
    memcpy(out, sDict.text, count); out[count] = '\0';
    *receipt = {sDict.deliveryExchange, sDict.deliverySequence,
                sDict.deliveryOffset, static_cast<uint16_t>(count)};
  }
  portEXIT_CRITICAL(&sDictMux);
  if (!displaySessionStillLive(source, epoch)) {
    memset(out, 0, outSize); *receipt = DictationTextReceipt{}; return false;
  }
  return receipt->length != 0;
}

static bool dictationCommitTextImpl(CommandSource source,
                                    const DictationTextReceipt& receipt, size_t accepted,
                                    const DictationAppLease* app = nullptr) {
  if (!accepted || accepted > receipt.length) return false;
  TransportSessionEpoch epoch = 0;
  portENTER_CRITICAL(&sDictMux);
  if (dictationConsumerMatchesLocked(source, app))
    epoch = app ? app->epoch : sDict.displayEpoch;
  portEXIT_CRITICAL(&sDictMux);
  if (!epoch || !displaySessionStillLive(source, epoch)) return false;
  bool committed = false;
  portENTER_CRITICAL(&sDictMux);
  // The last exact App ACK may be retried after a lost HTTP response. Once a
  // newer piece commits (or admission changes), an older receipt is stale.
  if (app && dictationConsumerMatchesLocked(source, app) &&
      sDict.appLastAccepted == accepted &&
      sDict.appLastReceipt.exchange == receipt.exchange &&
      sDict.appLastReceipt.sequence == receipt.sequence &&
      sDict.appLastReceipt.offset == receipt.offset &&
      sDict.appLastReceipt.length == receipt.length) {
    portEXIT_CRITICAL(&sDictMux);
    return true;
  }
  if (dictationConsumerMatchesLocked(source, app) && sDict.displayEpoch == epoch &&
      receipt.exchange && sDict.deliveryExchange == receipt.exchange &&
      sDict.deliverySequence == receipt.sequence &&
      sDict.deliveryOffset == receipt.offset && sDict.textPending &&
      accepted <= strlen(sDict.text)) {
    const size_t left = strlen(sDict.text) - accepted;
    memmove(sDict.text, sDict.text + accepted, left + 1);
    memset(sDict.text + left + 1, 0, sizeof(sDict.text) - left - 1);
    sDict.textPending = left != 0;
    sDict.deliveryOffset += accepted;
    if (sDict.deliveryOffset == sDict.deliveryTotal) {
      if (sDict.deliveryNeedsAck) sDict.deliveryAckPending = true;
      else dictationFinishInputLocked(); // Pi v1's sole final segment.
    }
    if (app) { sDict.appLastReceipt = receipt; sDict.appLastAccepted = accepted; }
    committed = true;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (committed) dictationWakeWorker();
  return committed;
}

bool dictationPeekTextFor(CommandSource source, char* out, size_t outSize,
                         DictationTextReceipt* receipt) {
  return dictationPeekTextImpl(source, out, outSize, receipt);
}

bool dictationCommitTextFor(CommandSource source,
                           const DictationTextReceipt& receipt, size_t accepted) {
  return dictationCommitTextImpl(source, receipt, accepted);
}

bool dictationAppPeekText(const DictationAppLease& lease, char* out, size_t outSize,
                          DictationTextReceipt* receipt) {
  return dictationPeekTextImpl(lease.source, out, outSize, receipt, &lease);
}

bool dictationAppCommitText(const DictationAppLease& lease,
                            const DictationTextReceipt& receipt, size_t accepted) {
  return dictationCommitTextImpl(lease.source, receipt, accepted, &lease);
}

bool dictationAppReadSpeakers(const DictationAppLease& lease, SttSpeakerSnapshot* out) {
  if (!out) return false;
  *out = SttSpeakerSnapshot{};
  if (!displaySessionStillLive(lease.source, lease.epoch)) return false;
  portENTER_CRITICAL(&sDictMux);
  const bool mine = dictationConsumerMatchesLocked(lease.source, &lease);
  if (mine) *out = sDict.appSpeakers;
  portEXIT_CRITICAL(&sDictMux);
  const bool live = displaySessionStillLive(lease.source, lease.epoch);
  portENTER_CRITICAL(&sDictMux);
  const bool stillMine = dictationConsumerMatchesLocked(lease.source, &lease);
  portEXIT_CRITICAL(&sDictMux);
  if (!live || !stillMine) { *out = SttSpeakerSnapshot{}; return false; }
  return mine;
}

// Live mode: the provisional text of the utterance still being spoken, for
// the App that owns the local run. Never enters the committed text stream.
bool dictationAppPeekDraft(const DictationAppLease& lease, char* out, size_t outSize, uint32_t* version) {
  if (version) *version = 0;
  if (!out || !outSize) return false;
  out[0] = '\0';
#if ENABLE_LOCAL_STT
  SttOwner actor;
  SttToken token = 0;
  portENTER_CRITICAL(&sDictMux);
  if (dictationConsumerMatchesLocked(lease.source, &lease) && sLocalDict.exchange == lease.exchange) {
    actor = sLocalDict.actor;
    token = sLocalDict.token;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (!token) return false;
  SttDraft draft;
  if (!sttReadDraft(actor, token, &draft)) return false;
  snprintf(out, outSize, "%s", draft.text);
  if (version) *version = draft.version;
  volatile char* p = draft.text;
  for (size_t i = 0; i < sizeof(draft.text); ++i) p[i] = 0;
  return true;
#else
  (void)lease;
  return false;
#endif
}

bool dictationTakeTextFor(CommandSource source, char* out, size_t outSize) {
  // Compatibility API: copying into the caller's buffer accepts that bounded
  // piece. New field consumers use peek/commit after their actual insertion.
  TransportSessionEpoch epoch = 0;
  portENTER_CRITICAL(&sDictMux);
  if (sDict.displaySource == source) epoch = sDict.displayEpoch;
  portEXIT_CRITICAL(&sDictMux);
  DictationTextReceipt receipt;
  if (!dictationPeekTextFor(source, out, outSize, &receipt)) return false;
  if (!dictationCommitTextFor(source, receipt, receipt.length) ||
      !displaySessionStillLive(source, epoch)) {
    memset(out, 0, outSize);
    return false;
  }
  return true;
}

bool dictationTakeText(char* out, size_t outSize) {
  return dictationTakeTextFor(SOURCE_LOCAL_DISPLAY, out, outSize);
}

void dictationOnCapturePublished(uint64_t owner, const char* path, bool saved,
                                 const char* failure) {
  if (!owner) return;

  // Build the stable handoff before taking the cross-core mux. The caller gives
  // us its local result copy after IDLE; no pointer into sCurrentRecordingPath is
  // retained past this function.
  DictationPublishedCapture incoming{};
  incoming.pending = true;
  incoming.owner = owner;
  incoming.saved = saved;
  snprintf(incoming.path, sizeof(incoming.path), "%s", path ? path : "");
  snprintf(incoming.failure, sizeof(incoming.failure), "%s",
           failure ? failure : "");

  bool wake = false;
  portENTER_CRITICAL(&sDictMux);
  if ((sDict.state == DictationState::RECORDING && sDict.owner == owner) ||
      sDict.cleanupOwner == owner) {
    sDict.exchangePathOwner = owner;
    memcpy(sDict.exchangePath, incoming.path, sizeof(sDict.exchangePath));
  }
  if (sDict.state == DictationState::RECORDING && sDict.owner == owner &&
      (!sDict.published.pending || sDict.published.owner == owner)) {
    sDict.published = incoming;
    wake = true;
  }
  if (sDict.cleanupOwner == owner) {
    // Publication proves begin can no longer create a future result for this
    // token; NOT_FOUND is now terminal rather than a transient start race.
    sDict.cleanupResolved = true;
    if (incoming.path[0]) {
      memcpy(sDict.cleanupPath, incoming.path, sizeof(sDict.cleanupPath));
    }
    wake = true;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (wake) dictationWakeWorker();
}

// ============================================================================
// Direct UART control plane
// ============================================================================

static const char* dictDeliver(uint64_t id, const char* text, size_t textLen,
                               uint32_t commandHostEpoch) {
  EXT_RAM_BSS_ATTR static char reply[96];  // PSRAM: written after sDictMux exits; fixed format, no transcript

  bool matched = false;
  CommandSource displaySource = SOURCE_INTERNAL;
  TransportSessionEpoch displayEpoch = kNoTransportSessionEpoch;
  uint32_t requestHostEpoch = 0;
  portENTER_CRITICAL(&sDictMux);
  if (sDict.state == DictationState::WAITING && sDict.owner == id &&
      sDict.requestHostEpoch != 0) {
    matched = true;
    displaySource = sDict.displaySource;
    displayEpoch = sDict.displayEpoch;
    requestHostEpoch = sDict.requestHostEpoch;
  }
  portEXIT_CRITICAL(&sDictMux);
  if (!matched) {
    // Single-use by construction: the id is cleared on delivery, so a replay
    // finds no pending dictation and is refused rather than re-typing itself.
    return "Error: no dictation is waiting for that id";
  }
  if (!dictationResponseSessionReady(requestHostEpoch, commandHostEpoch)) {
    if (dictFailOwned(id, "host session changed")) {
      (void)dictationQueueCleanup(id, /*resolved=*/true);
    }
    return "Error: the UART session that received this dictation is gone";
  }
  if (!displaySessionStillLive(displaySource, displayEpoch)) {
    if (dictFailOwned(id, "session changed")) {
      (void)dictationQueueCleanup(id, /*resolved=*/true);
    }
    return "Error: the display session that armed this dictation is gone";
  }

  bool committed = false;
  portENTER_CRITICAL(&sDictMux);
  if (sDict.state == DictationState::WAITING && sDict.owner == id &&
      sDict.displaySource == displaySource &&
      sDict.displayEpoch == displayEpoch &&
      sDict.requestHostEpoch == commandHostEpoch &&
      !sDictSave.pending && !sDictSave.inFlight) {
    // Reserve an immutable accepted-result job before clearing host ownership.
    // No filesystem work occurs under this lock or on the UART callback.
    if (sDict.transcriptOptions.enabled) {
      sDictSave = DictationSaveJob{};
      sDictSave.pending = true;
      sDictSave.exchange = id;
      sDictSave.options = sDict.transcriptOptions;
      memcpy(sDictSave.text, text, textLen);
      sDictSave.text[textLen] = '\0';
    }
    // One final chunk through the same UI mailbox as local inference. Keep its
    // accepted ID after clearing host ownership so partial UI commits stay fenced.
    dictationStageTextLocked(id, 1, text ? text : "", textLen, 0, /*needsAck=*/false);
    sDict.owner = 0;
    sDict.requestHostEpoch = 0;
    sDict.requestPushInFlight = false;
    sDict.requestWasPushed = false;
    // State stays WAITING until the keyboard drains it; dictationTakeText()
    // publishes IDLE. That keeps the mode from flashing "ready" for one frame
    // before the text actually appears.
    sDict.stateEnteredMs = millis();
    committed = true;
  }
  portEXIT_CRITICAL(&sDictMux);

  if (!committed) {
    (void)dictationQueueCleanup(id, /*resolved=*/true);
    return "Error: dictation was cancelled before delivery";
  }

  // The host has what it needs and the text is delivered; the WAV of somebody's
  // dictation should not outlive the exchange. Deletion is serviced globally on
  // the worker so this UART command cannot block on the filesystem.
  (void)dictationQueueCleanup(id, /*resolved=*/true);
  dictationWakeWorker();

  snprintf(reply, sizeof(reply), "OK: dictation delivered (%u chars)",
           static_cast<unsigned>(textLen));
  return reply;
}

static const char* dictSkipWireSpace(const char* text) {
  if (!text) return "";
  while (*text && dictationAsciiSpace(*text)) ++text;
  return text;
}

static size_t dictTrimmedWireLength(const char* text) {
  if (!text) return 0;
  size_t length = strlen(text);
  while (length && dictationAsciiSpace(text[length - 1])) --length;
  return length;
}

static bool dictWireEqualsNoCase(const char* text, size_t textLen,
                                 const char* expected) {
  if (!text || !expected) return false;
  size_t i = 0;
  for (; i < textLen && expected[i]; ++i) {
    if (dictationAsciiLower(text[i]) != dictationAsciiLower(expected[i])) {
      return false;
    }
  }
  return i == textLen && expected[i] == '\0';
}

static bool dictConsumeWireVerb(const char* text, size_t textLen,
                                const char* verb, const char** tailOut) {
  if (!text || !verb) return false;
  size_t verbLen = strlen(verb);
  if (textLen <= verbLen || !dictWireEqualsNoCase(text, verbLen, verb) ||
      !dictationAsciiSpace(text[verbLen])) {
    return false;
  }
  if (tailOut) *tailOut = text + verbLen;
  return true;
}

static const char* dictationHandleArgs(const char* argsInput,
                                       uint32_t commandHostEpoch) {
  const char* args = dictSkipWireSpace(argsInput);
  const size_t argsLen = dictTrimmedWireLength(args);

  // Explicit daemon capability, scoped to this exact authenticated login. A
  // sticky bool would let a reconnect inherit authority it never advertised.
  if (dictWireEqualsNoCase(args, argsLen, "hostready v1")) {
    portENTER_CRITICAL(&sDictMux);
    sDictHostReadyEpoch = commandHostEpoch;
    portEXIT_CRITICAL(&sDictMux);
    return "OK: dictate hostready v1";
  }
  if (dictWireEqualsNoCase(args, argsLen, "hostready off")) {
    portENTER_CRITICAL(&sDictMux);
    if (sDictHostReadyEpoch == commandHostEpoch) sDictHostReadyEpoch = 0;
    portEXIT_CRITICAL(&sDictMux);
    return "OK: dictate hostready off";
  }

  if (argsLen == 0 || dictWireEqualsNoCase(args, argsLen, "status")) {
    EXT_RAM_BSS_ATTR static char status[256];  // PSRAM: written from the snapshot copy, outside sDictMux
    DictationSnapshot snap = dictationSnapshotNow();
    const char* stateName = "idle";
    switch (snap.state) {
      case DictationState::RECORDING: stateName = "recording"; break;
      case DictationState::WAITING:   stateName = "waiting";   break;
      case DictationState::FAILED:    stateName = "failed";    break;
      case DictationState::IDLE:      stateName = "idle";      break;
    }
    snprintf(status, sizeof(status),
             "OK: Dictation: state=%s source=%s elapsed=%lums%s%s save=%s%s%s", stateName,
             snap.sourceName, (unsigned long)snap.elapsedMs,
             snap.failure[0] ? " failure=" : "", snap.failure,
             !snap.transcript.enabled ? "off" : snap.transcript.error[0] ? "failed"
                 : snap.transcript.complete ? "complete" : snap.transcript.saved ? "saving" : "pending",
             snap.transcript.error[0] ? " saveError=" : "", snap.transcript.error);
    return status;
  }

  // `result <16hex> <text...>` — the text is taken verbatim to end of line, so
  // a transcript may contain spaces, quotes and punctuation without escaping.
  const char* cursor = nullptr;
  if (dictConsumeWireVerb(args, argsLen, "result", &cursor)) {
    cursor = dictSkipWireSpace(cursor);
    const char* idStart = cursor;
    while (*cursor && !dictationAsciiSpace(*cursor)) ++cursor;
    uint64_t id = 0;
    if (!dictParseId(idStart, (size_t)(cursor - idStart), id))
      return "Error: usage: dictate result <16hex> <text>";
    cursor = dictSkipWireSpace(cursor);
    if (!*cursor) return "Error: dictate result needs text";
    const size_t textLen = dictTrimmedWireLength(cursor);
    if (textLen > DICTATION_MAX_TEXT)
      return "Error: dictate result text exceeds 256 bytes";
    if (!dictationPrintableAscii(cursor, textLen, /*allowEmpty=*/false))
      return "Error: dictate result text must be printable ASCII";
    return dictDeliver(id, cursor, textLen, commandHostEpoch);
  }

  cursor = nullptr;
  if (dictConsumeWireVerb(args, argsLen, "fail", &cursor)) {
    cursor = dictSkipWireSpace(cursor);
    const char* idStart = cursor;
    while (*cursor && !dictationAsciiSpace(*cursor)) ++cursor;
    uint64_t id = 0;
    if (!dictParseId(idStart, (size_t)(cursor - idStart), id))
      return "Error: usage: dictate fail <16hex> [reason]";
    bool matched = false;
    uint32_t requestHostEpoch = 0;
    portENTER_CRITICAL(&sDictMux);
    matched = (sDict.state == DictationState::WAITING && sDict.owner == id &&
               sDict.requestHostEpoch != 0);
    if (matched) requestHostEpoch = sDict.requestHostEpoch;
    portEXIT_CRITICAL(&sDictMux);
    if (!matched) return "Error: no dictation is waiting for that id";
    if (!dictationResponseSessionReady(requestHostEpoch, commandHostEpoch)) {
      if (dictFailOwned(id, "host session changed")) {
        (void)dictationQueueCleanup(id, /*resolved=*/true);
      }
      return "Error: the UART session that received this dictation is gone";
    }
    cursor = dictSkipWireSpace(cursor);
    const size_t reasonLen = dictTrimmedWireLength(cursor);
    if (reasonLen >= sizeof(sDict.failure))
      return "Error: dictate fail reason is too long";
    if (!dictationPrintableAscii(cursor, reasonLen, /*allowEmpty=*/true))
      return "Error: dictate fail reason must be printable ASCII";
    char reason[sizeof(sDict.failure)] = {};
    if (reasonLen) memcpy(reason, cursor, reasonLen);
    const char* failure = reasonLen ? reason : "host reported failure";
    if (!dictFailOwned(id, failure))
      return "Error: dictation was cancelled before failure delivery";
    (void)dictationQueueCleanup(id, /*resolved=*/true);
    return "OK: dictation marked failed";
  }

  return "Error: invalid arguments — Usage: dictate hostready <v1|off> | status | result <16hex> <text> | fail <16hex> [reason]";
}

bool dictationIsUartProtocolLine(const char* line) {
  return dictationUartLineIsProtocol(line);
}

DictationUartIntrinsicResult dictationHandleUartIntrinsic(
    const char* line, uint32_t namedEpoch, bool controlAllowed,
    char* replyOut, size_t replyOutSize) {
  if (!dictationIsUartProtocolLine(line)) {
    return DictationUartIntrinsicResult::NotHandled;
  }
  const char* verbStart = dictSkipWireSpace(line);
  const char* args = verbStart + 7;

  const char* reply = nullptr;
  if (!controlAllowed || namedEpoch == 0) {
    reply = "Error: dictate requires an authorized CM5 session";
  } else {
    reply = dictationHandleArgs(args, namedEpoch);
  }
  if (replyOut && replyOutSize) {
    snprintf(replyOut, replyOutSize, "%s", reply ? reply : "Error: dictate failed");
  }
  return DictationUartIntrinsicResult::Handled;
}

#endif  // ENABLE_DICTATION
