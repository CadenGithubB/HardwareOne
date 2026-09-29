// System_Dictation.cpp — see System_Dictation.h for the round trip and for the
// security note on the event push.

#include "System_Dictation.h"

#if ENABLE_DICTATION

#include <esp_random.h>
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
// only publish fixed-size work under gDictMux and notify it.
// Pi additionally owns a bounded transcript job and guarded filesystem writer.
static constexpr uint32_t kDictationWorkerStackBytes = ENABLE_LOCAL_STT ? 4096 : 5120;
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
  char failure[40];
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
static DictationSaveJob gDictSave;

static portMUX_TYPE gDictMux = portMUX_INITIALIZER_UNLOCKED;
static DictationControl gDict = {
    DictationState::IDLE, 0, SOURCE_INTERNAL, kNoTransportSessionEpoch,
    0, false, false, 0, false, false, false, {}, {}, 0, {}, 0, false,
    {}, {false, 0, false, {}, {}}, {false, 0, 0}};

// Lock-only identity predicate. Keyboard calls cannot impersonate an App on
// the same display; the exchange also distinguishes successive same-user runs.
static bool dictationConsumerMatchesLocked(CommandSource source,
                                           const DictationAppLease* app = nullptr) {
  if (!app) return !gDict.appConsumer && gDict.displaySource == source;
  return gDict.appConsumer && app->exchange && source == app->source &&
      gDict.appLease.source == app->source && gDict.appLease.epoch == app->epoch &&
      gDict.appLease.exchange == app->exchange;
}

static void dictationAdmitConsumerLocked(CommandSource source,
                                         TransportSessionEpoch epoch,
                                         uint64_t exchange,
                                         DictationAppLease* app) {
  gDict.appConsumer = app != nullptr;
  gDict.appLease = app ? DictationAppLease{source, epoch, exchange} : DictationAppLease{};
  gDict.appLastReceipt = DictationTextReceipt{};
  gDict.appLastAccepted = 0;
  if (app) *app = gDict.appLease;
}

// All mailbox mutations run under gDictMux. Keep delivery identity independent
// of recorder/host ownership: Pi accepts its result and clears owner before UI
// delivery, and its exact WAV cleanup may finish before the field consumes text.
static void dictationClearDeliveryLocked() {
  memset(gDict.text, 0, sizeof(gDict.text));
  gDict.textPending = false;
  gDict.deliveryExchange = 0;
  gDict.deliverySequence = 0;
  gDict.deliveryOffset = gDict.deliveryTotal = 0;
  gDict.deliveryNeedsAck = gDict.deliveryAckPending = false;
}

static void dictationFinishInputLocked() {
  dictationClearDeliveryLocked();
  gDict.owner = 0;
  gDict.state = DictationState::IDLE;
  gDict.stateEnteredMs = millis();
  gDict.micStopPending = gDict.weStartedMic;
  gDict.displaySource = SOURCE_INTERNAL;
  gDict.displayEpoch = kNoTransportSessionEpoch;
  gDict.requestHostEpoch = 0;
  gDict.requestPushInFlight = gDict.requestWasPushed = false;
}

static void dictationStageTextLocked(uint64_t exchange, uint32_t sequence,
                                    const char* text, size_t total, size_t offset,
                                    bool needsAck) {
  const size_t amount = std::min(total - offset, static_cast<size_t>(DICTATION_MAX_TEXT));
  memcpy(gDict.text, text + offset, amount);
  gDict.text[amount] = '\0';
  gDict.textPending = amount != 0;
  gDict.deliveryExchange = exchange;
  gDict.deliverySequence = sequence;
  gDict.deliveryOffset = static_cast<uint16_t>(offset);
  gDict.deliveryTotal = static_cast<uint16_t>(total);
  gDict.deliveryNeedsAck = needsAck;
  // Silence-only backend segments may validly decode to empty text. They need
  // acknowledgment but must not become a phantom UI action or a speech failure.
  gDict.deliveryAckPending = needsAck && total == offset;
}

// Capability is deliberately an epoch, not a sticky bool: UART logout/login
// revokes it without depending on a delayed session-change callback.
static uint32_t gDictHostReadyEpoch = 0;
static TaskHandle_t gDictWorkerTask = nullptr;
static bool gDictWorkerStarting = false;

static void dictationWakeWorker();
static void dictationSupervise();

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
  if (!stopMicrophone()) {
    // Source teardown has a bounded join. Preserve the debt if another capture
    // appeared between the idle check and the source-operation mutex.
    portENTER_CRITICAL(&gDictMux);
    gDict.micStopPending = true;
    portEXIT_CRITICAL(&gDictMux);
  }
}

// Boot nonce for dictation IDs, kept distinct from the EvenAI exchange nonce so
// the two owner spaces cannot collide in the recorder's completion ring.
static uint32_t gDictBootNonce = 0;
static uint32_t gDictCounter = 0;

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
  portENTER_CRITICAL(&gDictMux);
  if (gDict.owner == expectedOwner &&
      (gDict.state == DictationState::RECORDING ||
       gDict.state == DictationState::WAITING)) {
    gDict.state = DictationState::FAILED;
    gDict.stateEnteredMs = millis();
    gDict.owner = 0;
    gDict.requestHostEpoch = 0;
    gDict.requestPushInFlight = false;
    gDict.requestWasPushed = false;
    gDict.micStopPending = gDict.weStartedMic;
    snprintf(gDict.failure, sizeof(gDict.failure), "%s",
             reason ? reason : "failed");
    failed = true;
  }
  portEXIT_CRITICAL(&gDictMux);
  return failed;
}

static bool dictationHostReadyForEpoch(uint32_t liveEpoch,
                                       const char** whyNot = nullptr) {
  uint32_t capabilityEpoch = 0;
  portENTER_CRITICAL(&gDictMux);
  capabilityEpoch = gDictHostReadyEpoch;
  portEXIT_CRITICAL(&gDictMux);

  if (liveEpoch == 0 || capabilityEpoch != liveEpoch) {
    if (whyNot) *whyNot = "host not ready";
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
    *whyNot = !presence.seenForSession ? "host not present"
             : !presence.fresh         ? "host stale"
                                       : "host not ready";
  }
  if (ready && whyNot) *whyNot = nullptr;
  return ready;
}

static void dictationWakeWorker() {
  TaskHandle_t task = nullptr;
  portENTER_CRITICAL(&gDictMux);
  task = gDictWorkerTask;
  portEXIT_CRITICAL(&gDictMux);
  if (task) xTaskNotifyGive(task);
}

static bool dictationQueueCleanup(uint64_t owner, bool resolved,
                                  const char* exactPath = nullptr) {
  if (!owner) return false;
  char pathCopy[64] = {};
  if (exactPath) snprintf(pathCopy, sizeof(pathCopy), "%s", exactPath);
  bool queued = false;
  bool conflict = false;
  portENTER_CRITICAL(&gDictMux);
  if (!gDict.cleanupOwner || gDict.cleanupOwner == owner) {
    gDict.cleanupOwner = owner;
    gDict.cleanupResolved = gDict.cleanupResolved || resolved;
    if (pathCopy[0]) {
      memcpy(gDict.cleanupPath, pathCopy, sizeof(gDict.cleanupPath));
    } else if (!gDict.cleanupPath[0] && gDict.exchangePathOwner == owner) {
      memcpy(gDict.cleanupPath, gDict.exchangePath,
             sizeof(gDict.cleanupPath));
    }
    queued = true;
  } else {
    conflict = true;
  }
  portEXIT_CRITICAL(&gDictMux);
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
  portENTER_CRITICAL(&gDictMux);
  if (owner && gDict.cleanupOwner == owner) {
    gDict.cleanupResolved = true;
    wake = true;
  }
  portEXIT_CRITICAL(&gDictMux);
  if (wake) dictationWakeWorker();
}

static void dictationClearPublished(uint64_t owner) {
  portENTER_CRITICAL(&gDictMux);
  if (gDict.published.pending && gDict.published.owner == owner) {
    gDict.published = DictationPublishedCapture{};
  }
  portEXIT_CRITICAL(&gDictMux);
}

static void dictationQueueCancelEvent(uint64_t owner, uint32_t hostEpoch) {
  if (!owner || !hostEpoch) return;
  bool queued = false;
  bool conflict = false;
  portENTER_CRITICAL(&gDictMux);
  if (!gDict.cancelEvent.pending ||
      (gDict.cancelEvent.owner == owner &&
       gDict.cancelEvent.hostEpoch == hostEpoch)) {
    gDict.cancelEvent.pending = true;
    gDict.cancelEvent.owner = owner;
    gDict.cancelEvent.hostEpoch = hostEpoch;
    queued = true;
  } else {
    conflict = true;
  }
  portEXIT_CRITICAL(&gDictMux);
  if (conflict) {
    char id[17];
    dictFormatId(owner, id);
    WARN_SYSTEMF("[DICTATE] cancel event collision for id=%s", id);
  }
  if (queued) dictationWakeWorker();
}

static void dictationProcessCancelEvent() {
  DictationCancelEvent cancel{};
  portENTER_CRITICAL(&gDictMux);
  cancel = gDict.cancelEvent;
  portEXIT_CRITICAL(&gDictMux);
  if (!cancel.pending) return;

  char id[17];
  dictFormatId(cancel.owner, id);
  char evt[48];
  snprintf(evt, sizeof(evt), "dictate_cancel %s", id);
  const bool pushed =
      uartLinkPushEventForSession(cancel.hostEpoch, evt);
  const bool sessionGone =
      uartLinkSessionEpoch() != cancel.hostEpoch || !uartLinkIsRunning();

  portENTER_CRITICAL(&gDictMux);
  if (gDict.cancelEvent.pending &&
      gDict.cancelEvent.owner == cancel.owner &&
      gDict.cancelEvent.hostEpoch == cancel.hostEpoch &&
      (pushed || sessionGone)) {
    gDict.cancelEvent = DictationCancelEvent{};
  }
  portEXIT_CRITICAL(&gDictMux);
  if (pushed) INFO_SYSTEMF("[DICTATE] cancel pushed id=%s", id);
}

static void dictationFailAndClean(uint64_t owner, const char* reason) {
  char path[64] = {};
  portENTER_CRITICAL(&gDictMux);
  if (gDict.published.pending && gDict.published.owner == owner) {
    memcpy(path, gDict.published.path, sizeof(path));
  }
  portEXIT_CRITICAL(&gDictMux);
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
  portENTER_CRITICAL(&gDictMux);
  if (!gDict.published.pending) {
    portEXIT_CRITICAL(&gDictMux);
    return true;
  }
  capture = gDict.published;
  exactOwner = gDict.state == DictationState::RECORDING &&
               gDict.owner == capture.owner;
  if (exactOwner) {
    displaySource = gDict.displaySource;
    displayEpoch = gDict.displayEpoch;
  }
  portEXIT_CRITICAL(&gDictMux);

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
  portENTER_CRITICAL(&gDictMux);
  if (gDict.state == DictationState::RECORDING &&
      gDict.owner == capture.owner &&
      gDict.displaySource == displaySource &&
      gDict.displayEpoch == displayEpoch) {
    gDict.state = DictationState::WAITING;
    gDict.requestHostEpoch = hostEpoch;
    gDict.requestPushInFlight = false;
    gDict.requestWasPushed = false;
    gDict.stateEnteredMs = millis();
    waiting = true;
  }
  portEXIT_CRITICAL(&gDictMux);
  if (!waiting) {
    dictationClearPublished(capture.owner);
    return true;
  }

  // Final cancel fence. Cancellation that won before this critical section
  // clears owner/state, so no request is emitted. Once `inFlight` is published,
  // cancellation queues a same-session dictate_cancel and the single worker
  // guarantees it is written after this request attempt.
  bool beginPush = false;
  portENTER_CRITICAL(&gDictMux);
  const bool exactWaitingOwner =
      gDict.state == DictationState::WAITING &&
      gDict.owner == capture.owner;
  if (dictationRequestPushFenceReady(
          exactWaitingOwner, gDict.requestHostEpoch == hostEpoch)) {
    gDict.requestPushInFlight = true;
    beginPush = true;
  }
  portEXIT_CRITICAL(&gDictMux);
  if (!beginPush) {
    dictationClearPublished(capture.owner);
    return true;
  }

  char id[17];
  dictFormatId(capture.owner, id);
  char evt[128];
  snprintf(evt, sizeof(evt), "dictate_request %s %s", id, capture.path);
  const bool pushed = uartLinkPushEventForSession(hostEpoch, evt);

  portENTER_CRITICAL(&gDictMux);
  if (gDict.state == DictationState::WAITING &&
      gDict.owner == capture.owner &&
      gDict.requestHostEpoch == hostEpoch) {
    gDict.requestPushInFlight = false;
    if (pushed) gDict.requestWasPushed = true;
  }
  portEXIT_CRITICAL(&gDictMux);
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
  portENTER_CRITICAL(&gDictMux);
  owner = gDict.cleanupOwner;
  resolved = gDict.cleanupResolved;
  memcpy(exactPath, gDict.cleanupPath, sizeof(exactPath));
  portEXIT_CRITICAL(&gDictMux);
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
    portENTER_CRITICAL(&gDictMux);
    if (gDict.cleanupOwner == owner) {
      gDict.cleanupOwner = 0;
      gDict.cleanupResolved = false;
      gDict.cleanupPath[0] = '\0';
      if (gDict.exchangePathOwner == owner) {
        gDict.exchangePathOwner = 0;
        gDict.exchangePath[0] = '\0';
      }
    }
    portEXIT_CRITICAL(&gDictMux);
  }
  return clear;
}

#if ENABLE_LOCAL_STT
// A local exchange never enters the UART/WAV path. This slot survives UI
// cancellation until a late begin/worker has acknowledged the exact token.
struct LocalDictation {
  uint64_t exchange = 0;
  STTOwner actor;
  STTToken token = 0;
  bool beginPending = false;
  bool beginInFlight = false;
  TranscriptOptions transcriptOptions;
};
static LocalDictation gLocalDict;
static bool gLocalReadyPending = false;
static bool gLocalReadyKnown = false;
static bool gLocalReady = false;
static uint32_t gLocalReadyCheckedMs = 0;
static bool dictationEnsureWorker();

static void dictationRefreshLocalReady() {
  bool due;
  portENTER_CRITICAL(&gDictMux);
  due = gLocalReadyPending;
  gLocalReadyPending = false;
  portEXIT_CRITICAL(&gDictMux);
  if (!due) return;
  char error[96] = {};
  const bool ready = sttLocalAvailable(error, sizeof(error));
  portENTER_CRITICAL(&gDictMux);
  gLocalReady = ready;
  gLocalReadyKnown = true;
  gLocalReadyCheckedMs = millis();
  portEXIT_CRITICAL(&gDictMux);
}

static bool dictationLocalAvailable(const char** whyNot) {
  if (!dictationEnsureWorker()) {
    if (whyNot) *whyNot = "local worker unavailable";
    return false;
  }
  bool known, ready;
  portENTER_CRITICAL(&gDictMux);
  known = gLocalReadyKnown;
  ready = gLocalReady;
  if (!known || millis() - gLocalReadyCheckedMs >= 5000) gLocalReadyPending = true;
  portEXIT_CRITICAL(&gDictMux);
  dictationWakeWorker();
  const char* why = nullptr;
#if ENABLE_ESP_SR
  if (isESPSRRunning() || audioCaptureOwnedBy("sr")) why = "run closesr first";
#endif
  if (!why && (gMicRunning || micRecordingBusy() || audioCaptureOwnedBy("mic")))
    why = "run closemic first";
  if (!why && audioCaptureBusy() && !audioCaptureOwnedBy("stt")) why = "microphone busy";
  if (!why && !known) why = "checking local model";
  if (!why && !ready) why = "local model unavailable";
  if (whyNot) *whyNot = why;
  return why == nullptr;
}

static bool dictationBeginLocal(CommandSource source, TransportSessionEpoch epoch,
                                 DictationAppLease* app = nullptr) {
  const char* why = nullptr;
  if (!dictationAvailable(&why)) return false;
  const TranscriptOptions transcriptOptions = transcriptCaptureOptions(source, epoch);
  const uint32_t nonce = esp_random();
  bool admitted = false;
  portENTER_CRITICAL(&gDictMux);
  if (!gLocalDict.exchange &&
      (gDict.state == DictationState::IDLE || gDict.state == DictationState::FAILED) &&
      !gDict.cleanupOwner && !gDict.micStopPending && !gDict.published.pending &&
      !gDict.cancelEvent.pending && !gDictSave.pending && !gDictSave.inFlight) {
    if (!gDictBootNonce) gDictBootNonce = nonce ? nonce : 1;
    if (++gDictCounter == 0) ++gDictCounter;
    const uint64_t id = (static_cast<uint64_t>(gDictBootNonce) << 32) | gDictCounter;
    gLocalDict = LocalDictation{};
    gLocalDict.exchange = id;
    gLocalDict.actor = STTOwner{source, epoch};
    gLocalDict.transcriptOptions = transcriptOptions;
    gDict.transcriptOptions = transcriptOptions;
    gDict.transcriptStatus = TranscriptStatus{};
    gDict.transcriptStatus.enabled = transcriptOptions.enabled;
    gDict.transcriptExchange = id;
    gLocalDict.beginPending = true;
    gDict.owner = id;
    dictationAdmitConsumerLocked(source, epoch, id, app);
    gDict.displaySource = source;
    gDict.displayEpoch = epoch;
    gDict.state = DictationState::RECORDING;
    gDict.stateEnteredMs = millis();
    dictationClearDeliveryLocked();
    gDict.continuous = true;
    gDict.stopRequested = false;
    gDict.failure[0] = '\0';
    gDict.weStartedMic = false;
    gDict.requestHostEpoch = 0;
    gDict.requestPushInFlight = gDict.requestWasPushed = false;
    admitted = true;
  }
  portEXIT_CRITICAL(&gDictMux);
  if (admitted) dictationWakeWorker();
  return admitted;
}

static bool dictationCancelLocal(CommandSource source, bool force,
                                 const char* failure = nullptr,
                                 const DictationAppLease* app = nullptr) {
  bool mine = false;
  portENTER_CRITICAL(&gDictMux);
  mine = force || dictationConsumerMatchesLocked(source, app);
  if (mine) {
    gDict.owner = 0; // Retained job identity below observes this and joins cancel.
    gDict.state = failure ? DictationState::FAILED : DictationState::IDLE;
    gDict.stateEnteredMs = millis();
    if (!failure) {
      gDict.displaySource = SOURCE_INTERNAL;
      gDict.displayEpoch = 0;
    }
    dictationClearDeliveryLocked();
    snprintf(gDict.failure, sizeof(gDict.failure), "%s", failure ? failure : "");
  }
  portEXIT_CRITICAL(&gDictMux);
  if (mine) dictationWakeWorker();
  return mine;
}

static bool dictationLocalCurrentLocked(const LocalDictation& run) {
  return run.exchange && gDict.owner == run.exchange &&
      gDict.displaySource == run.actor.source && gDict.displayEpoch == run.actor.epoch;
}

static void dictationFailLocal(const LocalDictation& run, const char* reason) {
  portENTER_CRITICAL(&gDictMux);
  if (dictationLocalCurrentLocked(run)) {
    gDict.owner = 0;
    dictationClearDeliveryLocked();
    gDict.state = DictationState::FAILED;
    gDict.stateEnteredMs = millis();
    snprintf(gDict.failure, sizeof(gDict.failure), "%s", reason);
  }
  portEXIT_CRITICAL(&gDictMux);
  dictationWakeWorker();
}

static void dictationProcessLocal() {
  dictationRefreshLocalReady();
  LocalDictation run;
  bool current = false, finish = false;
  portENTER_CRITICAL(&gDictMux);
  run = gLocalDict;
  current = dictationLocalCurrentLocked(run);
  if (run.beginPending) {
    gLocalDict.beginPending = false;
    gLocalDict.beginInFlight = true;
  }
  portEXIT_CRITICAL(&gDictMux);
  if (!run.exchange) return;

  if (run.beginPending) {
    STTToken token = 0;
    char error[96] = {};
    const bool accepted = current &&
        displaySessionStillLive(run.actor.source, run.actor.epoch) &&
        sttBeginContinuous(run.actor, &token, error, sizeof(error), &run.transcriptOptions);
    portENTER_CRITICAL(&gDictMux);
    if (gLocalDict.exchange == run.exchange) {
      gLocalDict.beginInFlight = false;
      if (accepted) gLocalDict.token = token;
      if (!accepted) {
        if (dictationLocalCurrentLocked(run)) {
          gDict.owner = 0;
          gDict.state = DictationState::FAILED;
          gDict.stateEnteredMs = millis();
          snprintf(gDict.failure, sizeof(gDict.failure), "%s",
                   error[0] ? error : "session changed");
        }
        gLocalDict = LocalDictation{};
      }
      run = gLocalDict;
    }
    portEXIT_CRITICAL(&gDictMux);
    if (!accepted) return;
  }
  if (!run.token) return;

  portENTER_CRITICAL(&gDictMux);
  current = dictationLocalCurrentLocked(run);
  finish = gDict.stopRequested;
  portEXIT_CRITICAL(&gDictMux);
  const bool live = displaySessionStillLive(run.actor.source, run.actor.epoch);
  if (!current || !live) {
    // The broker self-cancels on revocation. Metadata-only join still works
    // after that identity can no longer read status or invoke cancellation.
    (void)sttCancel(run.actor, run.token);
    if (!live && current) dictationFailLocal(run, "session changed");
    if (!sttRunActive(run.token)) {
      STTSnapshot terminalSnapshot;
      const bool haveStatus = sttSnapshot(run.actor, run.token, &terminalSnapshot);
      portENTER_CRITICAL(&gDictMux);
      if (haveStatus && gDict.transcriptExchange == run.exchange)
        gDict.transcriptStatus = terminalSnapshot.transcript;
      if (gLocalDict.exchange == run.exchange) gLocalDict = LocalDictation{};
      portEXIT_CRITICAL(&gDictMux);
    }
    return;
  }
  if (finish) (void)sttRequestFinish(run.actor, run.token);

  uint32_t ack = 0;
  portENTER_CRITICAL(&gDictMux);
  if (gDict.deliveryExchange == run.exchange && gDict.deliveryAckPending)
    ack = gDict.deliverySequence;
  portEXIT_CRITICAL(&gDictMux);
  if (ack) {
    if (!sttAcknowledgeChunk(run.actor, run.token, ack)) {
      dictationFailLocal(run, "text acknowledgment failed");
      return;
    }
    portENTER_CRITICAL(&gDictMux);
    if (gDict.deliveryExchange == run.exchange && gDict.deliverySequence == ack &&
        gDict.deliveryAckPending) dictationClearDeliveryLocked();
    portEXIT_CRITICAL(&gDictMux);
  }

  STTSnapshot snap;
  if (!sttSnapshot(run.actor, run.token, &snap)) {
    dictationFailLocal(run, "session changed");
    return;
  }
  if (snap.state == STTState::Failed || snap.state == STTState::Cancelled) {
    dictationFailLocal(run, snap.error[0] ? snap.error : "local transcription failed");
    return;
  }

  uint16_t offset = 0;
  uint32_t sequence = 0;
  bool canStage = false;
  portENTER_CRITICAL(&gDictMux);
  if (dictationLocalCurrentLocked(run)) {
    gDict.transcriptStatus = snap.transcript;
    const DictationState state = !gDict.stopRequested &&
        (snap.captureActive || snap.state == STTState::Preparing)
            ? DictationState::RECORDING : DictationState::WAITING;
    if (gDict.state != state) { gDict.state = state; gDict.stateEnteredMs = millis(); }
    canStage = !gDict.textPending && !gDict.deliveryAckPending;
    offset = gDict.deliveryOffset;
    sequence = gDict.deliverySequence;
  }
  portEXIT_CRITICAL(&gDictMux);
  if (canStage) {
    STTTextChunk chunk;
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
      portENTER_CRITICAL(&gDictMux);
      if (dictationLocalCurrentLocked(run) && !gDict.textPending &&
          !gDict.deliveryAckPending && gDict.deliveryOffset == offset && stillLive)
        dictationStageTextLocked(run.exchange, chunk.sequence, chunk.text,
                                 length, offset, /*needsAck=*/true);
      portEXIT_CRITICAL(&gDictMux);
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
  if (snap.state == STTState::Failed || snap.state == STTState::Cancelled) {
    dictationFailLocal(run, snap.error[0] ? snap.error : "local transcription failed");
    return;
  }
  bool terminal = false;
  portENTER_CRITICAL(&gDictMux);
  if (dictationLocalCurrentLocked(run) && !snap.workerActive && snap.pendingTexts == 0 &&
      !gDict.deliveryExchange && !gDict.textPending) {
    gDict.transcriptStatus = snap.transcript;
    dictationFinishInputLocked();
    terminal = true;
    if (gLocalDict.exchange == run.exchange) gLocalDict = LocalDictation{};
  }
  portEXIT_CRITICAL(&gDictMux);
  if (terminal) (void)sttCancel(run.actor, run.token);
}
#endif // ENABLE_LOCAL_STT

static void dictationProcessSave() {
  DictationSaveJob job;
  portENTER_CRITICAL(&gDictMux);
  if (gDictSave.pending && !gDictSave.inFlight) {
    job = gDictSave;
    gDictSave.pending = false;
    gDictSave.inFlight = true;
  }
  portEXIT_CRITICAL(&gDictMux);
  if (!job.exchange) return;
  TranscriptSession transcript;
  transcript.begin(job.options, "pi", job.exchange);
  (void)transcript.append(1, job.text, strlen(job.text));
  transcript.finish("done");
  const TranscriptStatus status = transcript.snapshot();
  if (status.error[0]) INFO_SYSTEMF("[DICTATE] Transcript saving failed: %s", status.error);
  portENTER_CRITICAL(&gDictMux);
  if (gDict.transcriptExchange == job.exchange &&
      gDict.transcriptOptions.source == job.options.source &&
      gDict.transcriptOptions.epoch == job.options.epoch)
    gDict.transcriptStatus = status;
  if (gDictSave.exchange == job.exchange) gDictSave = DictationSaveJob{};
  portEXIT_CRITICAL(&gDictMux);
  volatile char* privateText = job.text;
  for (size_t i = 0; i < sizeof(job.text); ++i) privateText[i] = 0;
}

static bool dictationWorkerHasWork() {
  bool work = false;
  portENTER_CRITICAL(&gDictMux);
  work =
#if ENABLE_LOCAL_STT
         gLocalReadyPending || gLocalDict.exchange != 0 ||
#endif
         gDict.owner != 0 || gDict.deliveryExchange != 0 ||
         gDictSave.pending || gDictSave.inFlight ||
         gDict.published.pending || gDict.cancelEvent.pending ||
         gDict.cleanupOwner != 0 ||
         (gDict.micStopPending && !gDict.owner);
  portEXIT_CRITICAL(&gDictMux);
  return work;
}

static void dictationWorkerBody(void*) {
  for (;;) {
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
      if (dictationWorkerHasWork()) {
        (void)ulTaskNotifyTake(pdTRUE,
                               pdMS_TO_TICKS(kDictationWorkerRetryMs));
      }
    }
  }
}

static bool dictationEnsureWorker() {
  portENTER_CRITICAL(&gDictMux);
  if (gDictWorkerTask) {
    portEXIT_CRITICAL(&gDictMux);
    return true;
  }
  if (gDictWorkerStarting) {
    portEXIT_CRITICAL(&gDictMux);
    return false;
  }
  gDictWorkerStarting = true;
  portEXIT_CRITICAL(&gDictMux);

  TaskHandle_t created = nullptr;
  const BaseType_t rc = xTaskCreateLogged(
      dictationWorkerBody, "dictation_svc", kDictationWorkerStackBytes,
      nullptr, TASK_PRIORITY_LOW, &created, "dictation.service", PRO_CORE);

  portENTER_CRITICAL(&gDictMux);
  if (rc == pdPASS) gDictWorkerTask = created;
  gDictWorkerStarting = false;
  portEXIT_CRITICAL(&gDictMux);
  if (rc != pdPASS) {
    WARN_SYSTEMF("[DICTATE] service task unavailable");
    return false;
  }
  dictationWakeWorker();
  return true;
}

bool dictationAvailable(const char** whyNot) {
  if (!audioAnySourceAvailable()) {
    // Covers both boards with no PDM silicon and a board whose glasses are not
    // connected right now. Either way there is nothing to record with.
    if (whyNot) *whyNot = "no mic";
    return false;
  }
#if ENABLE_LOCAL_STT
  // An enabled local provider never silently exports microphone audio to a
  // host, even if its model is missing or its inference fails.
  return dictationLocalAvailable(whyNot);
#endif
  if (!uartLinkIsRunning()) {
    if (whyNot) *whyNot = "no host link";
    return false;
  }
  const uint32_t hostEpoch = uartLinkSessionEpoch();
  if (hostEpoch == 0) {
    if (whyNot) *whyNot = "host not logged in";
    return false;
  }
  return dictationHostReadyForEpoch(hostEpoch, whyNot);
}

static bool dictationBeginImpl(CommandSource displaySource,
                               TransportSessionEpoch displayEpoch,
                               DictationAppLease* app) {
  if ((displaySource != SOURCE_LOCAL_DISPLAY &&
       displaySource != SOURCE_G2_GLASSES &&
       !(app && displaySource == SOURCE_WEB)) ||
      displayEpoch == kNoTransportSessionEpoch) return false;
  if (!displaySessionStillLive(displaySource, displayEpoch)) return false;

#if ENABLE_LOCAL_STT
  return dictationBeginLocal(displaySource, displayEpoch, app);
#endif

  if (!dictationEnsureWorker()) return false;

  const char* why = nullptr;
  if (!dictationAvailable(&why)) return false;

  // Refuse a second arm rather than stacking captures; the recorder would
  // reject the start anyway. Cleanup/publication debt is also an admission
  // fence: its fixed slot must never be overwritten by a later path.
  const TranscriptOptions transcriptOptions = transcriptCaptureOptions(displaySource, displayEpoch);
  uint32_t nonceCandidate = esp_random();
  if (!nonceCandidate) nonceCandidate = 1;
  bool armed = false;
  uint64_t owner = 0;
  portENTER_CRITICAL(&gDictMux);
  if ((gDict.state == DictationState::IDLE ||
       gDict.state == DictationState::FAILED) &&
      !gDict.micStopPending && !gDict.cleanupOwner &&
      !gDict.published.pending && !gDict.cancelEvent.pending &&
      !gDictSave.pending && !gDictSave.inFlight) {
    if (!gDictBootNonce) gDictBootNonce = nonceCandidate;
    if (++gDictCounter == 0) ++gDictCounter;
    owner = ((uint64_t)gDictBootNonce << 32) | (uint64_t)gDictCounter;
    gDict.state = DictationState::RECORDING;
    gDict.owner = owner;
    dictationAdmitConsumerLocked(displaySource, displayEpoch, owner, app);
    gDict.transcriptOptions = transcriptOptions;
    gDict.transcriptStatus = TranscriptStatus{};
    gDict.transcriptStatus.enabled = transcriptOptions.enabled;
    gDict.transcriptExchange = owner;
    gDict.stateEnteredMs = millis();
    gDict.failure[0] = '\0';
    dictationClearDeliveryLocked();
    gDict.continuous = false;
    gDict.stopRequested = false;
    gDict.weStartedMic = false;
    gDict.displaySource = displaySource;
    gDict.displayEpoch = displayEpoch;
    gDict.requestHostEpoch = 0;
    gDict.requestPushInFlight = false;
    gDict.requestWasPushed = false;
    gDict.exchangePathOwner = 0;
    gDict.exchangePath[0] = '\0';
    armed = true;
  }
  portEXIT_CRITICAL(&gDictMux);
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
    if (!initMicrophone()) {
      (void)dictFailOwned(owner, "mic would not start");
      dictationResolveCleanup(owner);
      dictationWakeWorker();
      return false;
    }
    startedMic = true;
  }

  // Cancellation may run while initMicrophone() is opening the source. Publish
  // our source ownership only if the same admitted exchange still exists;
  // otherwise let the service worker hand it back.
  bool stillCurrent = false;
  portENTER_CRITICAL(&gDictMux);
  stillCurrent = gDict.state == DictationState::RECORDING &&
                 gDict.owner == owner &&
                 gDict.displaySource == displaySource &&
                 gDict.displayEpoch == displayEpoch;
  if (stillCurrent) {
    gDict.weStartedMic = startedMic;
  } else if (startedMic) {
    gDict.micStopPending = true;
  }
  portEXIT_CRITICAL(&gDictMux);
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
  portENTER_CRITICAL(&gDictMux);
  stillCurrent = gDict.state == DictationState::RECORDING &&
                 gDict.owner == owner;
  portEXIT_CRITICAL(&gDictMux);
  if (!stillCurrent) {
    dictationResolveCleanup(owner);
    dictationWakeWorker();
    return false;
  }

  // Source-agnostic by construction: startRecordingOwned re-resolves the
  // preference against what is connected now and owns every G2-only step.
  if (!startRecordingOwned(owner, kDictationVadSilenceMs, /*trim=*/true)) {
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

  portENTER_CRITICAL(&gDictMux);
  stillCurrent = gDict.state == DictationState::RECORDING &&
                 gDict.owner == owner;
  portEXIT_CRITICAL(&gDictMux);
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
  return dictationBeginImpl(source, epoch, nullptr);
}

bool dictationAppBegin(CommandSource source, TransportSessionEpoch epoch,
                       DictationAppLease* lease) {
  if (!lease) return false;
  *lease = DictationAppLease{};
  if (dictationBeginImpl(source, epoch, lease)) return true;
  *lease = DictationAppLease{};
  return false;
}

bool dictationBegin(TransportSessionEpoch displayEpoch) {
  return dictationBeginFor(SOURCE_LOCAL_DISPLAY, displayEpoch);
}

static bool dictationRequestStopImpl(CommandSource displaySource,
                                     const DictationAppLease* app = nullptr) {
#if ENABLE_LOCAL_STT
  portENTER_CRITICAL(&gDictMux);
  const bool mine = dictationConsumerMatchesLocked(displaySource, app);
  if (mine && gLocalDict.exchange && gDict.owner == gLocalDict.exchange)
    gDict.stopRequested = true;
  portEXIT_CRITICAL(&gDictMux);
  if (mine) dictationWakeWorker();
  return mine;
#else
  uint64_t owner = 0;
  portENTER_CRITICAL(&gDictMux);
  const bool mine = dictationConsumerMatchesLocked(displaySource, app);
  if (mine && gDict.state == DictationState::RECORDING) owner = gDict.owner;
  portEXIT_CRITICAL(&gDictMux);
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
  portENTER_CRITICAL(&gDictMux);
  mine = force || dictationConsumerMatchesLocked(displaySource, app);
  if (mine) {
    owner = gDict.owner;
    priorState = gDict.state;
    cancelHostWork = dictationCancelEventNeeded(
        gDict.requestPushInFlight, gDict.requestWasPushed);
    cancelHostEpoch = gDict.requestHostEpoch;
    gDict.state = failure ? DictationState::FAILED : DictationState::IDLE;
    gDict.stateEnteredMs = millis();
    gDict.owner = 0;
    if (!failure) {
      gDict.displaySource = SOURCE_INTERNAL;
      gDict.displayEpoch = kNoTransportSessionEpoch;
    }
    gDict.requestHostEpoch = 0;
    gDict.requestPushInFlight = false;
    gDict.requestWasPushed = false;
    gDict.micStopPending = gDict.weStartedMic;
    dictationClearDeliveryLocked();
    snprintf(gDict.failure, sizeof(gDict.failure), "%s", failure ? failure : "");
  }
  portEXIT_CRITICAL(&gDictMux);

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
  portENTER_CRITICAL(&gDictMux);
  out.state = gDict.state;
  out.ownerSource = gDict.displaySource;
  out.continuous = gDict.continuous;
  out.transcript = gDict.transcriptStatus;
  out.transcript.path[0] = '\0'; // No requesting owner in this global UI snapshot.
  out.elapsedMs = (uint32_t)(now - gDict.stateEnteredMs);
  snprintf(out.failure, sizeof(out.failure), "%s", gDict.failure);
  portEXIT_CRITICAL(&gDictMux);
  out.captureActive = out.state == DictationState::RECORDING;
  out.inferenceActive = out.state == DictationState::WAITING;
  out.sourceName = sourceLabel(audioGetSource());
  out.level = (out.state == DictationState::RECORDING) ? getAudioLevel() : 0;
#if ENABLE_LOCAL_STT
  LocalDictation local;
  portENTER_CRITICAL(&gDictMux);
  local = gLocalDict;
  portEXIT_CRITICAL(&gDictMux);
  STTSnapshot snap;
  if (local.token && sttSnapshot(local.actor, local.token, &snap)) {
    out.sourceName = sourceLabel(static_cast<AudioSource>(snap.audioSource));
    out.level = snap.level;
    out.transcript = snap.transcript;
    out.transcript.path[0] = '\0';
    out.preparing = snap.state == STTState::Preparing;
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
                         DictationAppLease* lease) {
  if (!lease) return false;
  *lease = DictationAppLease{};
  if (!displaySessionStillLive(source, epoch)) return false;
  portENTER_CRITICAL(&gDictMux);
  if (gDict.appConsumer && gDict.appLease.source == source &&
      gDict.appLease.epoch == epoch) *lease = gDict.appLease;
  portEXIT_CRITICAL(&gDictMux);
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
  portENTER_CRITICAL(&gDictMux);
  mine = dictationConsumerMatchesLocked(lease.source, &lease);
  portEXIT_CRITICAL(&gDictMux);
  DictationSnapshot status{};
  if (mine) status = dictationSnapshotNow();
  portENTER_CRITICAL(&gDictMux);
  out->busy = gDict.owner || gDict.textPending || gDict.deliveryExchange ||
      gDict.micStopPending || gDict.cleanupOwner || gDict.published.pending ||
      gDict.cancelEvent.pending || gDictSave.pending || gDictSave.inFlight;
#if ENABLE_LOCAL_STT
  out->busy = out->busy || gLocalDict.exchange != 0;
#endif
  // Recheck after the provider snapshot: a new admission may have replaced it.
  if (mine && dictationConsumerMatchesLocked(lease.source, &lease)) {
    out->valid = true;
    out->active = out->busy;
    out->done = !out->active;
    out->textPending = gDict.textPending;
    out->status = status;
    out->status.ownerSource = lease.source;
    if (gDict.transcriptExchange == lease.exchange)
      snprintf(out->status.transcript.path, sizeof(out->status.transcript.path),
               "%s", gDict.transcriptStatus.path);
  }
  portEXIT_CRITICAL(&gDictMux);
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
  portENTER_CRITICAL(&gDictMux);
  state = gDict.state;
  elapsed = (uint32_t)(now - gDict.stateEnteredMs);
  owner = gDict.owner;
  delivery = gDict.deliveryExchange;
  source = gDict.displaySource;
  epoch = gDict.displayEpoch;
  if (state == DictationState::WAITING) {
    cancelHostWork = dictationCancelEventNeeded(
        gDict.requestPushInFlight, gDict.requestWasPushed);
    requestHostEpoch = gDict.requestHostEpoch;
  }
  portEXIT_CRITICAL(&gDictMux);

  if ((owner || delivery) && !displaySessionStillLive(source, epoch)) {
    if (owner && dictFailOwned(owner, "session changed")) {
      if (cancelHostWork) dictationQueueCancelEvent(owner, requestHostEpoch);
      (void)dictationQueueCleanup(owner, state != DictationState::RECORDING);
      (void)requestStopRecordingOwned(owner, /*discard=*/true);
    } else if (!owner && delivery) {
      // Accepted Pi text can outlive capture. A vanished page/login must not
      // strand its mailbox or expose it to a later user of that transport.
      portENTER_CRITICAL(&gDictMux);
      if (!gDict.owner && gDict.deliveryExchange == delivery &&
          gDict.displaySource == source && gDict.displayEpoch == epoch)
        dictationFinishInputLocked();
      portEXIT_CRITICAL(&gDictMux);
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
  portENTER_CRITICAL(&gDictMux);
  if (dictationConsumerMatchesLocked(source, app) && gDict.textPending) epoch = gDict.displayEpoch;
  portEXIT_CRITICAL(&gDictMux);
  if (!epoch) return false;
  if (!displaySessionStillLive(source, epoch)) {
    portENTER_CRITICAL(&gDictMux);
    if (dictationConsumerMatchesLocked(source, app) && gDict.displayEpoch == epoch) {
      if (!gDict.owner) dictationFinishInputLocked();
      else dictationClearDeliveryLocked();
    }
    portEXIT_CRITICAL(&gDictMux);
    dictationWakeWorker();
    return false;
  }
  portENTER_CRITICAL(&gDictMux);
  if (dictationConsumerMatchesLocked(source, app) && gDict.displayEpoch == epoch &&
      gDict.deliveryExchange && gDict.textPending && outSize > 1) {
    const size_t count = std::min(strlen(gDict.text), outSize - 1);
    memcpy(out, gDict.text, count); out[count] = '\0';
    *receipt = {gDict.deliveryExchange, gDict.deliverySequence,
                gDict.deliveryOffset, static_cast<uint16_t>(count)};
  }
  portEXIT_CRITICAL(&gDictMux);
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
  portENTER_CRITICAL(&gDictMux);
  if (dictationConsumerMatchesLocked(source, app))
    epoch = app ? app->epoch : gDict.displayEpoch;
  portEXIT_CRITICAL(&gDictMux);
  if (!epoch || !displaySessionStillLive(source, epoch)) return false;
  bool committed = false;
  portENTER_CRITICAL(&gDictMux);
  // The last exact App ACK may be retried after a lost HTTP response. Once a
  // newer piece commits (or admission changes), an older receipt is stale.
  if (app && dictationConsumerMatchesLocked(source, app) &&
      gDict.appLastAccepted == accepted &&
      gDict.appLastReceipt.exchange == receipt.exchange &&
      gDict.appLastReceipt.sequence == receipt.sequence &&
      gDict.appLastReceipt.offset == receipt.offset &&
      gDict.appLastReceipt.length == receipt.length) {
    portEXIT_CRITICAL(&gDictMux);
    return true;
  }
  if (dictationConsumerMatchesLocked(source, app) && gDict.displayEpoch == epoch &&
      receipt.exchange && gDict.deliveryExchange == receipt.exchange &&
      gDict.deliverySequence == receipt.sequence &&
      gDict.deliveryOffset == receipt.offset && gDict.textPending &&
      accepted <= strlen(gDict.text)) {
    const size_t left = strlen(gDict.text) - accepted;
    memmove(gDict.text, gDict.text + accepted, left + 1);
    memset(gDict.text + left + 1, 0, sizeof(gDict.text) - left - 1);
    gDict.textPending = left != 0;
    gDict.deliveryOffset += accepted;
    if (gDict.deliveryOffset == gDict.deliveryTotal) {
      if (gDict.deliveryNeedsAck) gDict.deliveryAckPending = true;
      else dictationFinishInputLocked(); // Pi v1's sole final segment.
    }
    if (app) { gDict.appLastReceipt = receipt; gDict.appLastAccepted = accepted; }
    committed = true;
  }
  portEXIT_CRITICAL(&gDictMux);
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

bool dictationTakeTextFor(CommandSource source, char* out, size_t outSize) {
  // Compatibility API: copying into the caller's buffer accepts that bounded
  // piece. New field consumers use peek/commit after their actual insertion.
  TransportSessionEpoch epoch = 0;
  portENTER_CRITICAL(&gDictMux);
  if (gDict.displaySource == source) epoch = gDict.displayEpoch;
  portEXIT_CRITICAL(&gDictMux);
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
  // us its local result copy after IDLE; no pointer into currentRecordingPath is
  // retained past this function.
  DictationPublishedCapture incoming{};
  incoming.pending = true;
  incoming.owner = owner;
  incoming.saved = saved;
  snprintf(incoming.path, sizeof(incoming.path), "%s", path ? path : "");
  snprintf(incoming.failure, sizeof(incoming.failure), "%s",
           failure ? failure : "");

  bool wake = false;
  portENTER_CRITICAL(&gDictMux);
  if ((gDict.state == DictationState::RECORDING && gDict.owner == owner) ||
      gDict.cleanupOwner == owner) {
    gDict.exchangePathOwner = owner;
    memcpy(gDict.exchangePath, incoming.path, sizeof(gDict.exchangePath));
  }
  if (gDict.state == DictationState::RECORDING && gDict.owner == owner &&
      (!gDict.published.pending || gDict.published.owner == owner)) {
    gDict.published = incoming;
    wake = true;
  }
  if (gDict.cleanupOwner == owner) {
    // Publication proves begin can no longer create a future result for this
    // token; NOT_FOUND is now terminal rather than a transient start race.
    gDict.cleanupResolved = true;
    if (incoming.path[0]) {
      memcpy(gDict.cleanupPath, incoming.path, sizeof(gDict.cleanupPath));
    }
    wake = true;
  }
  portEXIT_CRITICAL(&gDictMux);
  if (wake) dictationWakeWorker();
}

// ============================================================================
// Direct UART control plane
// ============================================================================

static const char* dictDeliver(uint64_t id, const char* text, size_t textLen,
                               uint32_t commandHostEpoch) {
  EXT_RAM_BSS_ATTR static char reply[96];  // PSRAM: written after gDictMux exits; fixed format, no transcript

  bool matched = false;
  CommandSource displaySource = SOURCE_INTERNAL;
  TransportSessionEpoch displayEpoch = kNoTransportSessionEpoch;
  uint32_t requestHostEpoch = 0;
  portENTER_CRITICAL(&gDictMux);
  if (gDict.state == DictationState::WAITING && gDict.owner == id &&
      gDict.requestHostEpoch != 0) {
    matched = true;
    displaySource = gDict.displaySource;
    displayEpoch = gDict.displayEpoch;
    requestHostEpoch = gDict.requestHostEpoch;
  }
  portEXIT_CRITICAL(&gDictMux);
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
  portENTER_CRITICAL(&gDictMux);
  if (gDict.state == DictationState::WAITING && gDict.owner == id &&
      gDict.displaySource == displaySource &&
      gDict.displayEpoch == displayEpoch &&
      gDict.requestHostEpoch == commandHostEpoch &&
      !gDictSave.pending && !gDictSave.inFlight) {
    // Reserve an immutable accepted-result job before clearing host ownership.
    // No filesystem work occurs under this lock or on the UART callback.
    if (gDict.transcriptOptions.enabled) {
      gDictSave = DictationSaveJob{};
      gDictSave.pending = true;
      gDictSave.exchange = id;
      gDictSave.options = gDict.transcriptOptions;
      memcpy(gDictSave.text, text, textLen);
      gDictSave.text[textLen] = '\0';
    }
    // One final chunk through the same UI mailbox as local inference. Keep its
    // accepted ID after clearing host ownership so partial UI commits stay fenced.
    dictationStageTextLocked(id, 1, text ? text : "", textLen, 0, /*needsAck=*/false);
    gDict.owner = 0;
    gDict.requestHostEpoch = 0;
    gDict.requestPushInFlight = false;
    gDict.requestWasPushed = false;
    // State stays WAITING until the keyboard drains it; dictationTakeText()
    // publishes IDLE. That keeps the mode from flashing "ready" for one frame
    // before the text actually appears.
    gDict.stateEnteredMs = millis();
    committed = true;
  }
  portEXIT_CRITICAL(&gDictMux);

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
    portENTER_CRITICAL(&gDictMux);
    gDictHostReadyEpoch = commandHostEpoch;
    portEXIT_CRITICAL(&gDictMux);
    return "OK: dictate hostready v1";
  }
  if (dictWireEqualsNoCase(args, argsLen, "hostready off")) {
    portENTER_CRITICAL(&gDictMux);
    if (gDictHostReadyEpoch == commandHostEpoch) gDictHostReadyEpoch = 0;
    portEXIT_CRITICAL(&gDictMux);
    return "OK: dictate hostready off";
  }

  if (argsLen == 0 || dictWireEqualsNoCase(args, argsLen, "status")) {
    EXT_RAM_BSS_ATTR static char status[256];  // PSRAM: written from the snapshot copy, outside gDictMux
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
    portENTER_CRITICAL(&gDictMux);
    matched = (gDict.state == DictationState::WAITING && gDict.owner == id &&
               gDict.requestHostEpoch != 0);
    if (matched) requestHostEpoch = gDict.requestHostEpoch;
    portEXIT_CRITICAL(&gDictMux);
    if (!matched) return "Error: no dictation is waiting for that id";
    if (!dictationResponseSessionReady(requestHostEpoch, commandHostEpoch)) {
      if (dictFailOwned(id, "host session changed")) {
        (void)dictationQueueCleanup(id, /*resolved=*/true);
      }
      return "Error: the UART session that received this dictation is gone";
    }
    cursor = dictSkipWireSpace(cursor);
    const size_t reasonLen = dictTrimmedWireLength(cursor);
    if (reasonLen >= sizeof(gDict.failure))
      return "Error: dictate fail reason is too long";
    if (!dictationPrintableAscii(cursor, reasonLen, /*allowEmpty=*/true))
      return "Error: dictate fail reason must be printable ASCII";
    char reason[sizeof(gDict.failure)] = {};
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
