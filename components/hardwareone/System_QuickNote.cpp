// System_QuickNote.cpp — see System_QuickNote.h for the flow, authority and
// privacy rules.

#include "System_QuickNote.h"

#if ENABLE_QUICKNOTE

#include <ArduinoJson.h>
#include <esp_attr.h>  // EXT_RAM_BSS_ATTR
#include <esp_random.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "BLE_Peers.h"
#include "G2_Glasses.h"          // g2NativeVoiceActive
#include "QuickNote_Store.h"
#include "System_AuthIdentity.h"
#include "System_Clock.h"         // isSynced: a note's id; tzOffsetMinutes: its local hh:mm
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Dictation.h"
#include "System_Events.h"
#include "System_Microphone.h"   // MIC_STT_MAX_CAPTURE_MS
#include "System_Settings.h"
#if ENABLE_LOCAL_STT
#include "System_STT.h"          // sttKeepWarm
#endif
#include "System_TaskUtils.h"
#include "System_Transcript.h"   // transcriptCaptureIdentity
#include "System_Utils.h"
#include "System_VFS.h"          // isSDWritable: the default `note list` tier
#include "Transcription_UI_Policy.h"

using namespace QuickNotePolicy;

namespace {
// The worker runs the note file create (guarded VFS, account lookup), the same
// work as dictation_svc's transcript writer on its CM5 stack.
constexpr uint32_t kWorkerStackBytes = 5120;
constexpr uint32_t kPassMs = 200;              // worker cadence while a note runs
constexpr uint32_t kHoldSilenceStopMs = 6000;  // CM5 VAD: a held note pauses longer
constexpr size_t kUserMax = 65;
// Local STT keeps its model resident this long after the Notes page or the
// last note (sttKeepWarm), so the next hold skips the multi-second load.
constexpr uint32_t kWarmHoldMs = 120000;
// Restamping this boot's "boot" notes once the clock is set (restampPass).
constexpr size_t kUnstampedMax = 16, kRestampPerPass = 2;
constexpr uint8_t kRestampTries = 5;
constexpr uint32_t kRestampRetryMs = 2000;

struct Note {
  Phase phase = Phase::Idle;
  bool starting = false;      // quickNoteStart between the claim and the lease
  uint64_t id = 0;
  DictationAppLease lease;
  char owner[kUserMax] = {};
  Trigger trigger = Trigger::Cli;
  Mic mic = Mic::Pdm;
  bool hold = false;
  uint32_t startedMs = 0;
  bool captureSeen = false;
  uint32_t captureMs = 0;     // first pass that saw audio
  bool stopWanted = false;    // stop requested; the worker issues it
  bool stopIssued = false;
  uint32_t stopMs = 0;
  bool cancelWanted = false;
  const char* discardReason = nullptr;  // static text for a requested discard
  bool captureActive = false, preparing = false;
  int level = 0;
};
struct Last {
  uint64_t id = 0;
  QuickNoteOutcome outcome = QuickNoteOutcome::None;
  char owner[kUserMax] = {};
  char path[kPathMax] = {};
  uint32_t words = 0;
  uint32_t durationMs = 0;
  char reason[48] = {};
};

// gNote/gLast are shared by cmd_exec (start/stop/cancel/status), the worker
// and any task asking quickNoteActive(); every field is read and written
// under gQnMux. Only the worker ends a note.
portMUX_TYPE gQnMux = portMUX_INITIALIZER_UNLOCKED;
Note gNote;
Last gLast;
TaskHandle_t gQnTask = nullptr;
bool gQnTaskStarting = false;
// The note text. Worker only (and quickNoteStart before the worker can see
// the note); wiped when each note ends.
EXT_RAM_BSS_ATTR char gQnText[kMaxNoteBytes + 1];
size_t gQnTextLen = 0;
// Notes this worker saved as "boot" this boot, waiting for the clock to give
// them their real time (restampPass). Worker only. Past the cap a note keeps
// its boot name; the list still shows its age this boot, "earlier" after.
struct Unstamped {
  char path[kPathMax];
  char owner[kUserMax];
  uint64_t id;
  uint32_t uptimeS;
  CommandSource source;
  uint8_t tries;
};
EXT_RAM_BSS_ATTR Unstamped gUnstamped[kUnstampedMax];
size_t gUnstampedCount = 0;
bool gRestampDue = false;  // entries left that a timed pass should retry

bool refuse(const char** why, const char* reason) {
  if (why) *why = reason;
  return false;
}

void formatId(uint64_t id, char out[17]) {
  snprintf(out, 17, "%08lx%08lx", static_cast<unsigned long>(id >> 32),
           static_cast<unsigned long>(id & 0xffffffffu));
}

bool ownerMatches(const char* owner, const char* user, bool isAdmin) {
  return isAdmin || (user && *user && !strcmp(owner, user));
}

// notemaxsec, 10-120 s; the CM5 recorder caps every capture at 30 s.
uint32_t maxNoteMs() {
  const uint32_t sec = std::min<uint32_t>(120, std::max<uint32_t>(10, gSettings.noteMaxSec));
  uint32_t ms = sec * 1000;
#if !ENABLE_LOCAL_STT
  ms = std::min<uint32_t>(ms, MIC_STT_MAX_CAPTURE_MS);
#endif
  return ms;
}

void wipeText() {
  TranscriptionUI::clear(gQnText, sizeof(gQnText));
  gQnTextLen = 0;
}

void wake() {
  TaskHandle_t task = nullptr;
  portENTER_CRITICAL(&gQnMux);
  task = gQnTask;
  portEXIT_CRITICAL(&gQnMux);
  if (task) xTaskNotifyGive(task);
}

// Applies one lifecycle event to the live note `id` (ignored if it ended).
void noteEvent(uint64_t id, Event event) {
  portENTER_CRITICAL(&gQnMux);
  if (gNote.id == id) (void)noteTransition(gNote.phase, event);
  portEXIT_CRITICAL(&gQnMux);
}

// Ends the live note: records the outcome for status, posts the event and
// wipes the text. A discard (cancel, a slip, nothing heard) posts nothing.
void finish(const Note& n, QuickNoteOutcome outcome, const char* reason,
            const char* path, uint32_t words, uint32_t durationMs) {
  wipeText();
  portENTER_CRITICAL(&gQnMux);
  if (gNote.id == n.id) {
    gLast = Last{};
    gLast.id = n.id;
    gLast.outcome = outcome;
    memcpy(gLast.owner, n.owner, sizeof(gLast.owner));
    snprintf(gLast.path, sizeof(gLast.path), "%s", path ? path : "");
    gLast.words = words;
    gLast.durationMs = durationMs;
    snprintf(gLast.reason, sizeof(gLast.reason), "%s", reason ? reason : "");
    // Saved/Discarded/Failed, then the report clears it back to Idle.
    (void)noteTransition(gNote.phase, outcome == QuickNoteOutcome::Saved ? Event::Saved
                                      : outcome == QuickNoteOutcome::Failed ? Event::Fail
                                      : Event::Discard);
    (void)noteTransition(gNote.phase, Event::Clear);
    gNote = Note{};
  }
  portEXIT_CRITICAL(&gQnMux);
  char id[17];
  formatId(n.id, id);
  const char* trigger = noteTriggerName(n.trigger);
  const uint8_t via = transportToNotifSource(n.lease.source);
  if (outcome == QuickNoteOutcome::Saved) {
    char detail[32];
    snprintf(detail, sizeof(detail), "%luw %lums", static_cast<unsigned long>(words),
             static_cast<unsigned long>(durationMs));
    INFO_SYSTEMF("[QNOTE] note %s saved (%s, %lu words, %lu ms)", id, trigger,
                 static_cast<unsigned long>(words), static_cast<unsigned long>(durationMs));
    systemEventPost(SYSEVT_NOTE_SAVED, trigger, detail, via, n.owner);
  } else if (outcome == QuickNoteOutcome::Failed) {
    INFO_SYSTEMF("[QNOTE] note %s failed: %s", id, reason ? reason : "");
    systemEventPost(SYSEVT_NOTE_FAILED, reason, nullptr, via, n.owner);
  } else {
    INFO_SYSTEMF("[QNOTE] note %s discarded: %s", id, reason ? reason : "");
  }
}

// Appends every staged piece to the note, then acknowledges it. A new chunk
// (offset 0) after earlier text is a new phrase and gets a separating space.
// Text past kMaxNoteBytes is acknowledged and dropped; returns true once full.
bool drain(const DictationAppLease& lease) {
  char piece[DICTATION_MAX_TEXT + 1];
  DictationTextReceipt receipt;
  bool full = gQnTextLen >= kMaxNoteBytes;
  for (int i = 0; i < 8 && dictationAppPeekText(lease, piece, sizeof(piece), &receipt); ++i) {
    size_t room = kMaxNoteBytes - gQnTextLen;
    if (receipt.offset == 0 && gQnTextLen && room && piece[0] != ' ' &&
        gQnText[gQnTextLen - 1] != ' ') {
      gQnText[gQnTextLen++] = ' ';
      --room;
    }
    const size_t take = std::min<size_t>(receipt.length, room);
    memcpy(gQnText + gQnTextLen, piece, take);
    gQnTextLen += take;
    if (take < receipt.length) full = true;
    if (!dictationAppCommitText(lease, receipt, receipt.length)) break;
  }
  TranscriptionUI::clear(piece, sizeof(piece));
  gQnText[gQnTextLen] = '\0';
  return full || gQnTextLen >= kMaxNoteBytes;
}

// Remembers a note saved before the clock was set, for restampPass.
void trackUnstamped(const Note& n, const char* path, uint32_t uptimeS) {
  if (gUnstampedCount == kUnstampedMax) {
    WARN_SYSTEMF("[QNOTE] over %u notes before the clock was set: later ones keep their boot name",
                 static_cast<unsigned>(kUnstampedMax));
    return;
  }
  Unstamped& u = gUnstamped[gUnstampedCount++];
  snprintf(u.path, sizeof(u.path), "%s", path);
  memcpy(u.owner, n.owner, sizeof(u.owner));
  u.id = n.id;
  u.uptimeS = uptimeS;
  u.source = n.lease.source;
  u.tries = 0;
}

// Gives this boot's "boot" notes their real time once the clock is set
// (QuickNote_Store::restamp: the stamped copy is written and verified before
// the boot file goes). Never while a note runs, at most kRestampPerPass notes
// a pass; a note that still fails after kRestampTries keeps its boot name.
void restampPass() {
  // A note running or starting defers the pass to a timed retry: a start
  // that then fails does not wake this worker.
  gRestampDue = gUnstampedCount && quickNoteActive();
  if (!gUnstampedCount || gRestampDue) return;
  char stamp[kStampMax + 1];
  for (size_t i = 0, done = 0; i < gUnstampedCount && done < kRestampPerPass; ++done) {
    Unstamped& u = gUnstamped[i];
    // Not set yet: quickNoteClockSet wakes this worker when it is.
    if (!QuickNoteStore::stampSince(u.uptimeS, stamp, sizeof(stamp))) return;
    bool retry = false;
    const char* why = nullptr;
    const bool ok = QuickNoteStore::restamp(u.source, u.owner, u.path, stamp, nullptr, 0,
                                            &retry, &why);
    if (!ok && retry && ++u.tries < kRestampTries) {
      ++i;
      continue;
    }
    char id[17];
    formatId(u.id, id);
    INFO_SYSTEMF("[QNOTE] note %s %s %s", id, ok ? "given its time" : "keeps its boot name:",
                 ok ? stamp : why ? why : "");
    const size_t last = --gUnstampedCount;
    if (i != last) u = gUnstamped[last];
    gUnstamped[last] = Unstamped{};
  }
  gRestampDue = gUnstampedCount != 0;
}

// Dictation is over (finished, failed, timed out or taken): keep what was
// said, or discard a slip, a hold under kMinHoldMs or a take with nothing in
// it. `failure` (static or dictation's own reason) only matters when there is
// no text to keep, and then it wins over the slip rule: a hold note whose
// dictation failed before any audio is a failure, not "too short" (an early
// release never gets here; quickNoteStop discards it).
void complete(const Note& n, const char* failure, uint32_t now) {
  const uint32_t end = n.stopIssued ? n.stopMs : now;
  const uint32_t durationMs = n.captureSeen ? end - n.captureMs : 0;
  const size_t length = gQnTextLen;
  const bool spoken = length && noteWordCount(gQnText, length);
  if (!spoken && failure && *failure) {
    finish(n, QuickNoteOutcome::Failed, failure, nullptr, 0, durationMs);
    return;
  }
  if (n.hold && noteHoldTooShort(n.captureSeen, durationMs)) {
    finish(n, QuickNoteOutcome::Discarded, "too short", nullptr, 0, durationMs);
    return;
  }
  if (!spoken || noteIsJunk(gQnText, length, durationMs)) {
    finish(n, QuickNoteOutcome::Discarded, "nothing heard", nullptr, 0, durationMs);
    return;
  }
  // Text arrived, so capture ran even if no pass saw it: reach Saving.
  noteEvent(n.id, Event::CaptureActive);
  noteEvent(n.id, Event::Finished);
  NoteHeader header;
  header.id = n.id;
  header.trigger = n.trigger;
  header.mic = n.mic;
  header.durationMs = durationMs;
  char path[kPathMax];
  const char* why = nullptr;
  if (QuickNoteStore::create(n.lease.source, n.lease.epoch, n.owner, header, gQnText,
                             length, path, sizeof(path), &why)) {
    if (!strcmp(header.created, "boot")) trackUnstamped(n, path, header.uptimeS);
    finish(n, QuickNoteOutcome::Saved, nullptr, path, header.words, durationMs);
  } else {
    finish(n, QuickNoteOutcome::Failed, why ? why : "Cannot save note", nullptr, 0, durationMs);
  }
  TranscriptionUI::clear(header.title, sizeof(header.title));
}

// One worker pass over the live note. Every dictation call for a note runs
// here, so the lease has a single user.
void servicePass() {
  Note n;
  portENTER_CRITICAL(&gQnMux);
  n = gNote;
  portEXIT_CRITICAL(&gQnMux);
  if (n.starting || !noteActive(n.phase) || !n.lease.exchange) return;
  const uint32_t now = millis();
#if ENABLE_LOCAL_STT
  // Until kWarmHoldMs after this note, its STT worker leaves the verified
  // model resident for the next one (no load here: the worker has it).
  sttKeepWarm(kWarmHoldMs, false);
#endif

  if (n.cancelWanted) {
    (void)dictationAppCancel(n.lease);
    finish(n, QuickNoteOutcome::Discarded, n.discardReason ? n.discardReason : "cancelled",
           nullptr, 0, 0);
    return;
  }
  // A logout, unpairing or session replacement ends the note unsaved. The
  // dictation service cancels its own capture on the same revocation.
  if (!transportSessionEpochIsLive(n.lease.source, n.lease.epoch)) {
    (void)dictationAppCancel(n.lease);
    finish(n, QuickNoteOutcome::Failed, "session ended", nullptr, 0, 0);
    return;
  }
  DictationAppSnapshot snap;
  if (!dictationAppSnapshot(n.lease, &snap)) {
    // Revoked between the two checks, or the lease is gone: keep what arrived.
    if (!transportSessionEpochIsLive(n.lease.source, n.lease.epoch)) {
      finish(n, QuickNoteOutcome::Failed, "session ended", nullptr, 0, 0);
    } else {
      complete(n, "dictation ended", now);
    }
    return;
  }
  const bool full = drain(n.lease);
  const bool nativeVoice = g2NativeVoiceActive();
  const uint32_t maxMs = maxNoteMs();

  bool stop = false, abandon = false;
  portENTER_CRITICAL(&gQnMux);
  if (gNote.id == n.id) {
    gNote.captureActive = snap.status.captureActive;
    gNote.preparing = snap.status.preparing;
    gNote.level = snap.status.captureActive ? snap.status.level : 0;
    if ((snap.status.captureActive || gQnTextLen) && !gNote.captureSeen) {
      gNote.captureSeen = true;
      gNote.captureMs = now;
      (void)noteTransition(gNote.phase, Event::CaptureActive);
    }
    // The wearer's stop, the length limit, a full buffer, or a native voice
    // feature taking over the glasses (R16): finish and keep what was said.
    // Before any audio there is nothing to keep: abandon the note instead.
    stop = !gNote.stopIssued &&
        (gNote.stopWanted || full || now - gNote.startedMs >= maxMs || nativeVoice);
    if (stop && gNote.phase == Phase::Starting) {
      stop = false;
      abandon = true;
    } else if (stop) {
      gNote.stopIssued = true;
      gNote.stopMs = now;
      (void)noteTransition(gNote.phase, Event::Stop);
    }
    n = gNote;
  }
  portEXIT_CRITICAL(&gQnMux);
  if (abandon) {
    (void)dictationAppCancel(n.lease);
    if (nativeVoice) {
      finish(n, QuickNoteOutcome::Discarded, "Even AI or Conversate took the glasses",
             nullptr, 0, 0);
    } else {
      finish(n, QuickNoteOutcome::Failed, "recording did not start", nullptr, 0, 0);
    }
    return;
  }
  if (stop) (void)dictationAppRequestStop(n.lease);

  if (n.stopIssued && now - n.stopMs >= noteFinishTimeoutMs(ENABLE_LOCAL_STT)) {
    (void)dictationAppCancel(n.lease);
    complete(n, "transcription timed out", now);
    return;
  }
  if (snap.done && !snap.textPending) {
    complete(n, snap.status.state == DictationState::FAILED ? snap.status.failure : nullptr,
             now);
  }
}

void workerBody(void*) {
  for (;;) {
    (void)ulTaskNotifyTake(pdTRUE, quickNoteActive() ? pdMS_TO_TICKS(kPassMs)
                                   : gRestampDue ? pdMS_TO_TICKS(kRestampRetryMs)
                                   : portMAX_DELAY);
    servicePass();
    restampPass();
  }
}

bool ensureWorker() {
  portENTER_CRITICAL(&gQnMux);
  if (gQnTask || gQnTaskStarting) {
    const bool ready = gQnTask != nullptr;
    portEXIT_CRITICAL(&gQnMux);
    return ready;
  }
  gQnTaskStarting = true;
  portEXIT_CRITICAL(&gQnMux);
  TaskHandle_t created = nullptr;
  const BaseType_t rc = xTaskCreateLogged(workerBody, "qnote", kWorkerStackBytes, nullptr,
                                          TASK_PRIORITY_LOW, &created, "quicknote.service",
                                          PRO_CORE);
  portENTER_CRITICAL(&gQnMux);
  if (rc == pdPASS) gQnTask = created;
  gQnTaskStarting = false;
  portEXIT_CRITICAL(&gQnMux);
  if (rc != pdPASS) WARN_SYSTEMF("[QNOTE] service task unavailable");
  return rc == pdPASS;
}
}  // namespace

bool quickNoteActive() {
  portENTER_CRITICAL(&gQnMux);
  const bool active = gNote.starting || noteActive(gNote.phase);
  portEXIT_CRITICAL(&gQnMux);
  return active;
}

uint32_t quickNoteMaxMs() { return maxNoteMs(); }

void quickNoteClockSet() { wake(); }

void quickNoteWarm() {
#if ENABLE_LOCAL_STT
  sttKeepWarm(kWarmHoldMs, true);
#endif
}

bool quickNoteCanStart(const char** why) {
  if (why) *why = nullptr;
  if (quickNoteActive()) return refuse(why, "A note is already recording");
  if (g2NativeVoiceActive()) return refuse(why, "Even AI or Conversate is using the glasses");
  const DictationState state = dictationSnapshotNow().state;
  if (state == DictationState::RECORDING || state == DictationState::WAITING)
    return refuse(why, "Dictation is busy");
  if (!dictationAvailable(why)) return false;
  return QuickNoteStore::hasRoom(why);
}

bool quickNoteStart(const QuickNoteStartReq& req, uint64_t* id, const char** why) {
  if (id) *id = 0;
  if (why) *why = nullptr;
  // Exactly the sources dictation admits for an App, each with a named epoch.
  if ((req.source != SOURCE_G2_GLASSES && req.source != SOURCE_WEB &&
       req.source != SOURCE_LOCAL_DISPLAY) || !req.epoch || !req.user ||
      (req.mic != AUDIO_SRC_G2_LEFT && req.mic != AUDIO_SRC_LOCAL_PDM) ||
      !noteTriggerName(req.trigger))
    return refuse(why, "Invalid note request");
  // The note belongs to the named user of this exact live source+epoch.
  char owner[kUserMax] = {};
  const bool named = transcriptCaptureIdentity(req.source, req.epoch, owner, sizeof(owner)) &&
      !strcmp(owner, req.user);
  if (!named) return refuse(why, "Session expired");
  if (!quickNoteCanStart(why)) return false;
  if (!audioSourceAvailable(req.mic))
    return refuse(why, req.mic == AUDIO_SRC_G2_LEFT ? "Glasses mic unavailable"
                                                    : "Board mic unavailable");
  if (!ensureWorker()) return refuse(why, "Note service unavailable");

  // A note saved with the clock unset is boot-<id>.txt: its id keeps the
  // list newest first (noteNewId).
  uint32_t boot = 0, uptimeS = 0;
  QuickNoteStore::bootClock(&boot, &uptimeS);
  const bool clockSet = Clock::isSynced();
  uint64_t noteId = 0;
  while (!noteId) {
    const uint64_t bits = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
    noteId = noteNewId(clockSet, boot, uptimeS, bits);
  }
  bool claimed = false;
  portENTER_CRITICAL(&gQnMux);
  if (!gNote.starting && !noteActive(gNote.phase)) {
    gNote = Note{};
    gNote.starting = true;
    gNote.id = noteId;
    memcpy(gNote.owner, owner, sizeof(gNote.owner));
    gNote.trigger = req.trigger;
    gNote.mic = req.mic == AUDIO_SRC_G2_LEFT ? Mic::G2 : Mic::Pdm;
    gNote.hold = req.hold;
    gNote.startedMs = millis();
    claimed = noteTransition(gNote.phase, Event::Start);
  }
  portEXIT_CRITICAL(&gQnMux);
  if (!claimed) return refuse(why, "A note is already recording");
  wipeText();

  DictationAppOptions options;
  options.purpose = DictationAppPurpose::QuickNote;
  options.preferredSource = req.mic;
  options.silenceStopMs = req.hold ? kHoldSilenceStopMs : 0;
  options.saveTranscript = false;  // the note file is the only copy
  DictationAppLease lease;
  const bool begun = dictationAppBeginWith(req.source, req.epoch, options, &lease);
  portENTER_CRITICAL(&gQnMux);
  if (gNote.id == noteId) {
    gNote.starting = false;
    if (begun) gNote.lease = lease;
    else gNote = Note{};
  }
  portEXIT_CRITICAL(&gQnMux);
  if (!begun) {
    const char* unavailable = nullptr;
    return refuse(why, dictationAvailable(&unavailable) ? "Dictation is busy"
                       : unavailable ? unavailable : "Dictation unavailable");
  }
  wake();
  char idText[17];
  formatId(noteId, idText);
  INFO_SYSTEMF("[QNOTE] note %s started (%s, %s mic%s)", idText, noteTriggerName(req.trigger),
               noteMicName(req.mic == AUDIO_SRC_G2_LEFT ? Mic::G2 : Mic::Pdm),
               req.hold ? ", hold" : "");
  systemEventPost(SYSEVT_NOTE_STARTED, noteTriggerName(req.trigger),
                  ENABLE_LOCAL_STT ? "local" : "pi");
  if (id) *id = noteId;
  return true;
}

bool quickNoteStop(uint64_t id, const char* user, bool isAdmin) {
  bool found = false;
  portENTER_CRITICAL(&gQnMux);
  if (id && gNote.id == id && noteActive(gNote.phase) &&
      ownerMatches(gNote.owner, user, isAdmin)) {
    found = true;
    const uint32_t captured = gNote.captureSeen ? millis() - gNote.captureMs : 0;
    if (gNote.phase == Phase::Starting) {
      gNote.cancelWanted = true;          // stopped before any audio: a slip
      gNote.discardReason = "stopped before recording";
    } else if (gNote.hold && noteHoldTooShort(gNote.captureSeen, captured)) {
      gNote.cancelWanted = true;
      gNote.discardReason = "too short";
    } else {
      gNote.stopWanted = true;
    }
  }
  portEXIT_CRITICAL(&gQnMux);
  if (found) wake();
  return found;
}

bool quickNoteCancel(uint64_t id, const char* user, bool isAdmin) {
  bool found = false;
  portENTER_CRITICAL(&gQnMux);
  if (id && gNote.id == id && noteActive(gNote.phase) &&
      ownerMatches(gNote.owner, user, isAdmin)) {
    found = true;
    gNote.cancelWanted = true;
    gNote.discardReason = "cancelled";
  }
  portEXIT_CRITICAL(&gQnMux);
  if (found) wake();
  return found;
}

bool quickNoteSnapshot(QuickNoteSnapshot* out) {
  if (!out) return false;
  *out = QuickNoteSnapshot{};
  const uint32_t now = millis();
  portENTER_CRITICAL(&gQnMux);
  const bool live = gNote.starting || noteActive(gNote.phase);
  if (live) {
    out->phase = gNote.phase;
    out->id = gNote.id;
    out->trigger = gNote.trigger;
    out->mic = gNote.mic;
    out->hold = gNote.hold;
    out->captureActive = gNote.captureActive;
    out->preparing = gNote.preparing;
    out->elapsedMs = now - gNote.startedMs;
    out->level = gNote.level;
    memcpy(out->owner, gNote.owner, sizeof(out->owner));
  }
  out->lastId = gLast.id;
  out->lastOutcome = gLast.outcome;
  memcpy(out->lastOwner, gLast.owner, sizeof(out->lastOwner));
  memcpy(out->lastPath, gLast.path, sizeof(out->lastPath));
  out->lastWords = gLast.words;
  out->lastDurationMs = gLast.durationMs;
  memcpy(out->reason, gLast.reason, sizeof(out->reason));
  portEXIT_CRITICAL(&gQnMux);
  return live;
}

// =============================================================================
// `note` CLI
// =============================================================================
namespace {
// Only cmd_exec uses this response. Every reply carries the private STT tag
// ("transcriptPath"), so logs, shared mirrors and other sessions see only
// "[private STT result]"; the caller's own direct reply keeps it.
EXT_RAM_BSS_ATTR char response[4096];

// Every field goes through one of these out-of-line setters, so each one costs
// a call rather than an inlined ArduinoJson member insert (S3 flash).
__attribute__((noinline)) void put(JsonObject at, const char* key, const char* value) {
  at[key] = value;
}
__attribute__((noinline)) void put(JsonObject at, const char* key, uint32_t value) {
  at[key] = value;
}
__attribute__((noinline)) void putFlag(JsonObject at, const char* key, bool value) {
  at[key] = value;
}
void putId(JsonObject at, const char* key, uint64_t id) {
  char text[17] = {};
  if (id) formatId(id, text);
  put(at, key, text);
}

// `doc` holds an object (commandNote and error() make it one).
const char* reply(JsonDocument& doc) {
  JsonObject root = doc.as<JsonObject>();
  if (root["success"].isNull()) putFlag(root, "success", true);
  if (root["error"].isNull()) put(root, "error", "");
  if (root["transcriptPath"].isNull()) putFlag(root, "transcriptPath", true);
  if (doc.overflowed() || measureJson(doc) >= sizeof(response))
    return "{\"success\":false,\"error\":\"Response too large\",\"transcriptPath\":true}";
  serializeJson(doc, response, sizeof(response));
  return response;
}
__attribute__((noinline)) const char* error(JsonDocument& doc, const char* why) {
  JsonObject root = doc.to<JsonObject>();
  putFlag(root, "success", false);
  put(root, "error", why);
  return reply(doc);
}
// The caller's own live named session (never a stateless or internal one).
bool actor(DictationAppLease& caller) {
  const auto& ctx = currentAuthContext();
  caller.source = ctx.transport;
  caller.epoch = captureTransportSessionEpoch(ctx);
  return ctx.user.length() && caller.epoch &&
      transportSessionEpochIsLive(caller.source, caller.epoch);
}
bool live(const DictationAppLease& caller) {
  return transportSessionEpochIsLive(caller.source, caller.epoch);
}
const char* outcomeName(QuickNoteOutcome outcome) {
  return outcome == QuickNoteOutcome::Saved ? "saved"
       : outcome == QuickNoteOutcome::Failed ? "failed"
       : outcome == QuickNoteOutcome::Discarded ? "discarded" : "";
}
// The live note (last=false) or the last one that ended. Never any text.
void putNote(JsonObject root, const QuickNoteSnapshot& s, bool last) {
  putId(root, "id", last ? s.lastId : s.id);
  put(root, "phase", notePhaseName(last ? Phase::Idle : s.phase));
  putFlag(root, "active", !last);
  if (!last) {
    put(root, "trigger", noteTriggerName(s.trigger));
    put(root, "mic", noteMicName(s.mic));
    putFlag(root, "hold", s.hold);
    putFlag(root, "captureActive", s.captureActive);
    putFlag(root, "preparing", s.preparing);
    put(root, "elapsedMs", s.elapsedMs);
    put(root, "level", static_cast<uint32_t>(s.level));
    put(root, "maxMs", maxNoteMs());
    return;
  }
  put(root, "outcome", outcomeName(s.lastOutcome));
  put(root, "reason", s.reason);
  put(root, "path", s.lastPath);
  put(root, "words", s.lastWords);
  put(root, "durationMs", s.lastDurationMs);
}

// True if `user` is the paired G2 wearer right now; *epoch gets the wearer's
// own G2 session (the one a glasses note runs under).
bool pairedWearer(const String& user, TransportSessionEpoch* epoch) {
  BlePeerOwnerSession wearer;
  const bool paired = blePeerOwnerSessionSnapshot(BLE_PEER_G2_GLASSES, wearer) &&
      wearer.live() && wearer.user == user;
  if (epoch) *epoch = paired ? wearer.transportEpoch : kNoTransportSessionEpoch;
  secureClearString(wearer.user);
  return paired;
}

const char* commandStart(JsonDocument& doc, const CommandArgs& args,
                         const DictationAppLease& caller) {
  bool glasses = false, hold = false, toggle = false;
  int micChoice = -1;  // the notemic values: 0 auto, 1 glasses, 2 board
  for (int i = 1; i < args.count(); ++i) {
    const String& token = args.arg(i);
    if (token == "glasses" && !glasses) glasses = true;
    else if (token == "hold" && !hold && !toggle) hold = true;
    else if (token == "toggle" && !hold && !toggle) toggle = true;
    else if (micChoice < 0 && token == "mic=auto") micChoice = 0;
    else if (micChoice < 0 && token == "mic=g2") micChoice = 1;
    else if (micChoice < 0 && token == "mic=pdm") micChoice = 2;
    else return error(doc, "Usage: note start [hold|toggle] [mic=auto|g2|pdm] | note start glasses [hold|toggle]");
  }
  const auto& ctx = currentAuthContext();
  String role;
  if (!getUserAuthorizationRole(ctx.user, role) || role == "guest")
    return error(doc, "Notes require a non-guest account");
  QuickNoteStartReq req;
  req.hold = hold;
  req.user = ctx.user.c_str();
  if (glasses) {
    // R21: any signed-in console, but never automations, ESP-NOW, MQTT or
    // voice, and only the paired wearer: an admin cannot open someone else's
    // glasses mic. The note runs under the wearer's own G2 session.
    if (caller.source == SOURCE_INTERNAL || caller.source == SOURCE_ESPNOW ||
        caller.source == SOURCE_MQTT || caller.source == SOURCE_VOICE)
      return error(doc, "Notes cannot start from automations, remote or voice");
    if (micChoice == 2) return error(doc, "Glasses notes use the glasses mic");
    if (!pairedWearer(ctx.user, &req.epoch))
      return error(doc, "Only the paired wearer can record on the glasses");
    req.source = SOURCE_G2_GLASSES;
    req.trigger = hold ? Trigger::Hold : Trigger::Cli;
    req.mic = AUDIO_SRC_G2_LEFT;
  } else {
    if (caller.source != SOURCE_G2_GLASSES && caller.source != SOURCE_WEB &&
        caller.source != SOURCE_LOCAL_DISPLAY)
      return error(doc, "From this console use: note start glasses");
    req.source = caller.source;
    req.epoch = caller.epoch;
    if (caller.source == SOURCE_G2_GLASSES) {
      // The lens records with the glasses that are showing it.
      if (micChoice == 2) return error(doc, "Glasses notes use the glasses mic");
      req.trigger = hold ? Trigger::Hold : Trigger::Lens;
      req.mic = AUDIO_SRC_G2_LEFT;
    } else {
      req.trigger = hold ? Trigger::Hold
                  : caller.source == SOURCE_WEB ? Trigger::Web : Trigger::Cli;
      // The glasses mic is the paired wearer's alone (R21), whichever
      // console asks: Auto falls back to the board mic for anyone else.
      const bool wearer = pairedWearer(ctx.user, nullptr);
      const int choice = micChoice >= 0 ? micChoice : gSettings.noteMic;
      if (choice == 1 && !wearer)
        return error(doc, "Only the paired wearer can record on the glasses");
      req.mic = choice == 1 ? AUDIO_SRC_G2_LEFT
              : choice == 2 ? AUDIO_SRC_LOCAL_PDM
              : wearer && audioSourceAvailable(AUDIO_SRC_G2_LEFT) ? AUDIO_SRC_G2_LEFT
              : AUDIO_SRC_LOCAL_PDM;
    }
  }
  uint64_t id = 0;
  const char* why = nullptr;
  if (!quickNoteStart(req, &id, &why)) return error(doc, why ? why : "Note unavailable");
  QuickNoteSnapshot snapshot;
  (void)quickNoteSnapshot(&snapshot);
  if (snapshot.id == id) putNote(doc.as<JsonObject>(), snapshot, false);
  else putId(doc.as<JsonObject>(), "id", id);   // already over (a very short failure)
  return nullptr;
}

const char* commandStatus(JsonDocument& doc, const CommandArgs& args) {
  JsonObject root = doc.as<JsonObject>();
  const auto& ctx = currentAuthContext();
  const bool admin = currentExecIsAdmin();
  QuickNoteSnapshot s;
  const bool active = quickNoteSnapshot(&s);
  if (args.count() <= 1) {  // `note` or `note status`
    if (active && ownerMatches(s.owner, ctx.user.c_str(), admin)) {
      putNote(root, s, false);
      return nullptr;
    }
    // Nothing of the caller's is live: only the generic start gate.
    const char* why = nullptr;
    const bool available = !active && quickNoteCanStart(&why);
    putId(root, "id", 0);
    put(root, "phase", notePhaseName(Phase::Idle));
    putFlag(root, "active", false);
    putFlag(root, "busy", active);
    putFlag(root, "available", available);
    put(root, "reason", why ? why : active ? "A note is already recording" : "");
    put(root, "maxMs", maxNoteMs());
    return nullptr;
  }
  uint64_t id = 0;
  if (args.count() != 2 || !noteParseId(args.arg(1).c_str(), args.arg(1).length(), &id))
    return error(doc, "A valid note ID is required");
  if (active && s.id == id && ownerMatches(s.owner, ctx.user.c_str(), admin)) {
    putNote(root, s, false);
  } else if (s.lastId == id && ownerMatches(s.lastOwner, ctx.user.c_str(), admin)) {
    putNote(root, s, true);
  } else {
    return error(doc, "Note unavailable");
  }
  return nullptr;
}

// A list row's time: the wearer's local hh:mm for a real stamp (the offset
// System_Clock applies), else how long ago this boot (noteAgeLabel), else
// "earlier"; never "--:--". `boot`/`uptimeS` are now (bootClock).
void noteWhen(const NoteHeader& header, uint32_t boot, uint32_t uptimeS, char* out, size_t cap) {
  if (!noteLocalTime(header.created, Clock::tzOffsetMinutes(), out, cap))
    noteAgeLabel(header.boot, header.uptimeS, boot, uptimeS, out, cap);
}

// One listing page (cmd_exec only). Raw PSRAM storage: Page's member
// defaults are not all zero, so it cannot be a BSS object itself.
EXT_RAM_BSS_ATTR alignas(QuickNoteStore::Page) uint8_t gPageStorage[sizeof(QuickNoteStore::Page)];

// list [internal|sd] [offset] [u<id>]; another account's notes need an
// unrestricted role (the transcript path policy decides, as for transcripts).
// With no tier it lists the one a save would use now (QuickNoteStore::create).
const char* commandList(JsonDocument& doc, const CommandArgs& args) {
  bool sd = false;
  uint32_t offset = 0, owner = 0;
  int i = 1;
  if (i < args.count() && (args.arg(i) == "internal" || args.arg(i) == "sd"))
    sd = args.arg(i++) == "sd";
  else
    sd = VFS::isSDWritable();
  if (i < args.count() && args.arg(i)[0] != 'u' &&
      !TranscriptionUI::parseUnsigned(args.arg(i++).c_str(), offset))
    return error(doc, "Invalid offset");
  if (i < args.count()) {
    const String& who = args.arg(i++);
    if (who.length() < 2 || who[0] != 'u' ||
        !TranscriptionUI::parseUnsigned(who.c_str() + 1, owner) || !owner)
      return error(doc, "Invalid account");
  }
  if (i != args.count()) return error(doc, "Usage: note list [internal|sd] [offset] [u<id>]");
  QuickNoteStore::Page& page = *new (gPageStorage) QuickNoteStore::Page{};
  const char* why = nullptr;
  const bool listed = QuickNoteStore::list(currentAuthContext(), owner, sd, offset, &page, &why);
  if (listed) {
    JsonObject root = doc.as<JsonObject>();
    JsonArray rows = root["notes"].to<JsonArray>();
    uint32_t boot = 0, uptimeS = 0;
    QuickNoteStore::bootClock(&boot, &uptimeS);
    for (size_t r = 0; r < page.count; ++r) {
      const QuickNoteStore::Row& row = page.rows[r];
      const bool valid = row.headerValid;
      JsonObject out = rows.add<JsonObject>();
      putId(out, "id", row.header.id);
      put(out, "path", row.path);
      put(out, "created", row.header.created);
      char when[kWhenMax];
      noteWhen(row.header, boot, uptimeS, when, sizeof(when));
      put(out, "when", when);
      put(out, "title", valid ? row.header.title : "");
      put(out, "words", valid ? row.header.words : 0u);
      put(out, "trigger", valid ? noteTriggerName(row.header.trigger) : "");
      put(out, "mic", valid ? noteMicName(row.header.mic) : "");
      put(out, "durationMs", valid ? row.header.durationMs : 0u);
    }
    put(root, "offset", offset);
    put(root, "nextOffset", page.nextOffset);
    put(root, "total", page.total);
    putFlag(root, "more", page.more);
    putFlag(root, "capped", page.capped);
  }
  // Titles are the start of the text: clear them once serialized.
  TranscriptionUI::clear(reinterpret_cast<char*>(gPageStorage), sizeof(gPageStorage));
  return listed ? nullptr : error(doc, why ? why : "Notes unavailable");
}

const char* commandRead(JsonDocument& doc, const CommandArgs& args) {
  uint32_t offset = 0;
  if (args.count() < 2 || args.count() > 3 ||
      (args.count() == 3 && !TranscriptionUI::parseUnsigned(args.arg(2).c_str(), offset)))
    return error(doc, "Usage: note read \"<path>\" [offset]");
  char text[QuickNoteStore::kReadWindow + 1];
  size_t got = 0;
  uint32_t total = 0;
  const char* why = nullptr;
  if (!QuickNoteStore::read(currentAuthContext(), args.arg(1).c_str(), offset, text,
                            sizeof(text), &got, &total, &why))
    return error(doc, why ? why : "Note unavailable");
  JsonObject root = doc.as<JsonObject>();
  put(root, "transcriptPath", args.arg(1).c_str());
  put(root, "sttText", text);
  put(root, "offset", offset);
  put(root, "nextOffset", static_cast<uint32_t>(offset + got));
  put(root, "total", total);
  putFlag(root, "eof", offset + got >= total);
  TranscriptionUI::clear(text, sizeof(text));
  return nullptr;
}

const char* commandNote(const String& input) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  JsonDocument doc;
  doc.to<JsonObject>();
  CommandArgs args(input);
  DictationAppLease caller;
  if (!actor(caller)) return error(doc, "Sign in to use notes");
  if (args.unterminatedQuote()) return error(doc, "Invalid arguments");
  const String op = args.count() ? args.arg(0) : "status";
  const char* failed = nullptr;
  if (op == "start") {
    failed = commandStart(doc, args, caller);
  } else if (op == "status" && args.count() <= 2) {
    failed = commandStatus(doc, args);
  } else if ((op == "stop" || op == "cancel") && args.count() == 2) {
    uint64_t id = 0;
    if (!noteParseId(args.arg(1).c_str(), args.arg(1).length(), &id))
      return error(doc, "A valid note ID is required");
    const char* user = currentAuthContext().user.c_str();
    const bool admin = currentExecIsAdmin();
    const bool ok = op == "stop" ? quickNoteStop(id, user, admin)
                                 : quickNoteCancel(id, user, admin);
    if (!ok) return error(doc, "Note unavailable");
    failed = commandStatus(doc, args);
  } else if (op == "list") {
    failed = commandList(doc, args);
  } else if (op == "read") {
    failed = commandRead(doc, args);
  } else if (op == "delete" && args.count() == 2) {
    const char* why = nullptr;
    if (!QuickNoteStore::remove(currentAuthContext(), args.arg(1).c_str(), &why))
      return error(doc, why ? why : "Note unavailable");
    put(doc.as<JsonObject>(), "deleted", args.arg(1).c_str());
  } else {
    return error(doc, "Invalid note action");
  }
  if (failed) return failed;
  if (!live(caller)) return error(doc, "Session expired");
  return reply(doc);
}
}  // namespace

const CommandEntry noteCommands[] = {
  { "note", "Private spoken notes (QuickNote)", false, commandNote,
    "note start [hold|toggle] [mic=auto|g2|pdm] | note start glasses [hold|toggle] | "
    "note stop|cancel <id> | note status [id] | note list [internal|sd] [offset] [u<id>] | "
    "note read \"<path>\" [offset] | note delete \"<path>\"" },
};
const size_t noteCommandsCount = sizeof(noteCommands) / sizeof(noteCommands[0]);

#endif  // ENABLE_QUICKNOTE
