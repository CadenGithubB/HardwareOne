// System_Dictation.cpp — see System_Dictation.h for the round trip and for the
// security note on the event push.

#include "System_Dictation.h"

#if ENABLE_DICTATION

#include <esp_random.h>

#include "HAL_Audio.h"
#include "System_Cm5Presence.h"
#include "System_Debug.h"
#include "System_DictationPolicy.h"
#include "System_Microphone.h"
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
static constexpr uint32_t kDictationWorkerStackBytes = 3072;
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
  CommandSource displaySource;       // OLED or G2 consumer/authority
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
};

static portMUX_TYPE gDictMux = portMUX_INITIALIZER_UNLOCKED;
static DictationControl gDict = {
    DictationState::IDLE, 0, SOURCE_INTERNAL, kNoTransportSessionEpoch,
    0, false, false, 0, false, false, false, {}, {}, 0, {}, 0, false,
    {}, {false, 0, false, {}, {}}, {false, 0, 0}};

// Capability is deliberately an epoch, not a sticky bool: UART logout/login
// revokes it without depending on a delayed session-change callback.
static uint32_t gDictHostReadyEpoch = 0;
static TaskHandle_t gDictWorkerTask = nullptr;
static bool gDictWorkerStarting = false;

static void dictationWakeWorker();

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

// Is the display session that armed a dictation still the live one? Used on
// the delivery paths only. Deliberately NOT called from the recorder task: it
// takes the display lifecycle lock, and that task's terminal section has a
// documented FS-lock/TX-mutex ordering that no new lock should join.
static bool displaySessionStillLive(CommandSource source,
                                    TransportSessionEpoch expected) {
  if (expected == kNoTransportSessionEpoch) return false;
  if (source == SOURCE_G2_GLASSES) {
    return transportSessionEpochIsLive(SOURCE_G2_GLASSES, expected);
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

static bool dictationWorkerHasWork() {
  bool work = false;
  portENTER_CRITICAL(&gDictMux);
  work = gDict.published.pending || gDict.cancelEvent.pending ||
         gDict.cleanupOwner != 0 ||
         (gDict.micStopPending && !gDict.owner);
  portEXIT_CRITICAL(&gDictMux);
  return work;
}

static void dictationWorkerBody(void*) {
  for (;;) {
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (dictationWorkerHasWork()) {
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

bool dictationBeginFor(CommandSource displaySource,
                       TransportSessionEpoch displayEpoch) {
  if ((displaySource != SOURCE_LOCAL_DISPLAY &&
       displaySource != SOURCE_G2_GLASSES) ||
      displayEpoch == kNoTransportSessionEpoch) return false;
  if (!displaySessionStillLive(displaySource, displayEpoch)) return false;

  if (!dictationEnsureWorker()) return false;

  const char* why = nullptr;
  if (!dictationAvailable(&why)) return false;

  // Refuse a second arm rather than stacking captures; the recorder would
  // reject the start anyway. Cleanup/publication debt is also an admission
  // fence: its fixed slot must never be overwritten by a later path.
  uint32_t nonceCandidate = esp_random();
  if (!nonceCandidate) nonceCandidate = 1;
  bool armed = false;
  uint64_t owner = 0;
  portENTER_CRITICAL(&gDictMux);
  if ((gDict.state == DictationState::IDLE ||
       gDict.state == DictationState::FAILED) &&
      !gDict.micStopPending && !gDict.cleanupOwner &&
      !gDict.published.pending && !gDict.cancelEvent.pending) {
    if (!gDictBootNonce) gDictBootNonce = nonceCandidate;
    if (++gDictCounter == 0) ++gDictCounter;
    owner = ((uint64_t)gDictBootNonce << 32) | (uint64_t)gDictCounter;
    gDict.state = DictationState::RECORDING;
    gDict.owner = owner;
    gDict.stateEnteredMs = millis();
    gDict.failure[0] = '\0';
    gDict.textPending = false;
    gDict.text[0] = '\0';
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
  return true;
}

bool dictationBegin(TransportSessionEpoch displayEpoch) {
  return dictationBeginFor(SOURCE_LOCAL_DISPLAY, displayEpoch);
}

void dictationRequestStopFor(CommandSource displaySource) {
  uint64_t owner = 0;
  portENTER_CRITICAL(&gDictMux);
  if (gDict.displaySource == displaySource &&
      gDict.state == DictationState::RECORDING) owner = gDict.owner;
  portEXIT_CRITICAL(&gDictMux);
  if (!owner) return;
  // Non-blocking: this runs on the display task, which must not stall a frame
  // waiting for FINALIZING. The terminal hook picks it up from the recorder.
  requestStopRecordingOwned(owner, /*discard=*/false);
}

void dictationRequestStop() {
  dictationRequestStopFor(SOURCE_LOCAL_DISPLAY);
}

static void dictationCancelImpl(CommandSource displaySource, bool force) {
  uint64_t owner = 0;
  bool mine = false;
  DictationState priorState = DictationState::IDLE;
  bool cancelHostWork = false;
  uint32_t cancelHostEpoch = 0;
  portENTER_CRITICAL(&gDictMux);
  mine = force || gDict.displaySource == displaySource;
  if (mine) {
    owner = gDict.owner;
    priorState = gDict.state;
    cancelHostWork = dictationCancelEventNeeded(
        gDict.requestPushInFlight, gDict.requestWasPushed);
    cancelHostEpoch = gDict.requestHostEpoch;
    gDict.state = DictationState::IDLE;
    gDict.stateEnteredMs = millis();
    gDict.owner = 0;
    gDict.displaySource = SOURCE_INTERNAL;
    gDict.displayEpoch = kNoTransportSessionEpoch;
    gDict.requestHostEpoch = 0;
    gDict.requestPushInFlight = false;
    gDict.requestWasPushed = false;
    gDict.micStopPending = gDict.weStartedMic;
    gDict.textPending = false;
    gDict.text[0] = '\0';
    gDict.failure[0] = '\0';
  }
  portEXIT_CRITICAL(&gDictMux);

  if (!mine) return;

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

DictationSnapshot dictationSnapshotNow() {
  DictationSnapshot out{};
  const uint32_t now = millis();
  portENTER_CRITICAL(&gDictMux);
  out.state = gDict.state;
  out.ownerSource = gDict.displaySource;
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
  bool cancelHostWork = false;
  uint32_t requestHostEpoch = 0;
  const uint32_t now = millis();
  portENTER_CRITICAL(&gDictMux);
  state = gDict.state;
  elapsed = (uint32_t)(now - gDict.stateEnteredMs);
  owner = gDict.owner;
  if (state == DictationState::WAITING) {
    cancelHostWork = dictationCancelEventNeeded(
        gDict.requestPushInFlight, gDict.requestWasPushed);
    requestHostEpoch = gDict.requestHostEpoch;
  }
  portEXIT_CRITICAL(&gDictMux);

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

  if (dictationWorkerHasWork()) dictationWakeWorker();
}

bool dictationTakeTextFor(CommandSource displaySource,
                          char* out, size_t outSize) {
  if (!out || outSize == 0) return false;
  bool took = false;
  portENTER_CRITICAL(&gDictMux);
  if (gDict.displaySource == displaySource && gDict.textPending) {
    snprintf(out, outSize, "%s", gDict.text);
    gDict.textPending = false;
    gDict.text[0] = '\0';
    gDict.state = DictationState::IDLE;
    gDict.stateEnteredMs = millis();
    gDict.micStopPending = gDict.weStartedMic;
    gDict.displaySource = SOURCE_INTERNAL;
    gDict.displayEpoch = kNoTransportSessionEpoch;
    gDict.requestHostEpoch = 0;
    gDict.requestPushInFlight = false;
    gDict.requestWasPushed = false;
    took = true;
  }
  portEXIT_CRITICAL(&gDictMux);
  if (took) dictationWakeWorker();
  return took;
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
  if (gDict.state == DictationState::WAITING && gDict.owner == id) {
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
      gDict.requestHostEpoch == commandHostEpoch) {
    snprintf(gDict.text, sizeof(gDict.text), "%.*s",
             static_cast<int>(textLen), text ? text : "");
    gDict.textPending = true;
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
    EXT_RAM_BSS_ATTR static char status[160];  // PSRAM: written from the snapshot copy, outside gDictMux
    DictationSnapshot snap = dictationSnapshotNow();
    const char* stateName = "idle";
    switch (snap.state) {
      case DictationState::RECORDING: stateName = "recording"; break;
      case DictationState::WAITING:   stateName = "waiting";   break;
      case DictationState::FAILED:    stateName = "failed";    break;
      case DictationState::IDLE:      stateName = "idle";      break;
    }
    snprintf(status, sizeof(status),
             "OK: Dictation: state=%s source=%s elapsed=%lums%s%s", stateName,
             snap.sourceName, (unsigned long)snap.elapsedMs,
             snap.failure[0] ? " failure=" : "", snap.failure);
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
    matched = (gDict.state == DictationState::WAITING && gDict.owner == id);
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
