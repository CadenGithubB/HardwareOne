// System_QuickNote.h — QuickNote: a private spoken note, recorded through
// dictation (System_Dictation, purpose QuickNote) and saved by QuickNote_Store
// under the speaker's own /stt/u<id>/notes/.
//
// This service is the single dictation lease holder for notes. The `note` CLI
// (and the lens/hold front ends built on it) only make requests to it; none of
// them ever holds a lease. One note at a time:
//
//   quickNoteStart (cmd_exec) ── dictationAppBeginWith(purpose QuickNote,
//        │                        no transcript copy) ── note_started
//        ▼ wakes
//   "qnote" worker ── drains the text into a PSRAM buffer, enforces notemaxsec,
//        │            stands down for Even AI / Conversate, finishes on stop,
//        │            silence or the length limit
//        ▼
//   QuickNote_Store::create ── note_saved   (or note_failed / a silent discard)
//
// Authority (R21): a start binds to a live named session that can hear the
// note's owner: the paired wearer on the lens, a web cookie session or the
// local display. Other consoles use `note start glasses`, which needs the
// caller to BE the paired wearer (no admin override) and records on the
// wearer's glasses under the wearer's G2 session. Automations, ESP-NOW, MQTT
// and voice never start a note.
//
// Privacy (R22, R23): note text exists only in the worker's buffer (cleared
// after every note), the saved file and `note read`'s private reply. Snapshots,
// events and log lines carry ids, phases, counts, timings and reasons only.
// The events are queue-only or event-only, never a banner or toast.
#pragma once
#include "System_BuildConfig.h"
#include "HAL_Audio.h"        // AudioSource
#include "QuickNote_Policy.h"
#include "System_User.h"      // CommandSource, TransportSessionEpoch
#include <cstddef>
#include <cstdint>

#if ENABLE_QUICKNOTE
// How the last note ended.
enum class QuickNoteOutcome : uint8_t { None = 0, Saved, Failed, Discarded };

struct QuickNoteStartReq {
  CommandSource source = SOURCE_INTERNAL;  // G2_GLASSES, WEB or LOCAL_DISPLAY
  TransportSessionEpoch epoch = kNoTransportSessionEpoch;
  const char* user = nullptr;              // must name the owner of source+epoch
  QuickNotePolicy::Trigger trigger = QuickNotePolicy::Trigger::Cli;
  bool hold = false;                       // ends on release (6 s CM5 silence)
  AudioSource mic = AUDIO_SRC_NONE;        // G2_LEFT or LOCAL_PDM, resolved
};

// Never carries note text. owner/lastOwner let a front end show a note only to
// the account that spoke it, or to an admin.
struct QuickNoteSnapshot {
  QuickNotePolicy::Phase phase = QuickNotePolicy::Phase::Idle;  // live note
  uint64_t id = 0;
  QuickNotePolicy::Trigger trigger = QuickNotePolicy::Trigger::Cli;
  QuickNotePolicy::Mic mic = QuickNotePolicy::Mic::Pdm;
  bool hold = false;
  bool captureActive = false;   // the mic is delivering audio ("speak now")
  bool preparing = false;
  uint32_t elapsedMs = 0;       // since the start request
  int level = 0;                // 0..100 while capturing
  char owner[65] = {};
  // The last note that ended (saved, failed or discarded).
  uint64_t lastId = 0;
  QuickNoteOutcome lastOutcome = QuickNoteOutcome::None;
  char lastOwner[65] = {};
  char lastPath[QuickNotePolicy::kPathMax] = {};  // saved file
  uint32_t lastWords = 0;
  uint32_t lastDurationMs = 0;
  char reason[48] = {};         // why it failed or was discarded
};

// Cheap pre-check, any task: no note live, dictation idle and available, no
// native Even AI / Conversate session, room for a note above the reserve
// (this one takes the filesystem lock, bounded at 1 s).
bool quickNoteCanStart(const char** why);
// cmd_exec only (dictation begin may bring up the microphone). On success *id
// is the new note's id. False with a short reason in *why.
bool quickNoteStart(const QuickNoteStartReq& req, uint64_t* id, const char** why);
// Finish and save what was said (a hold released before capture, or with less
// than kMinHoldMs of audio, is discarded instead), or discard. Only the note's
// owner (the same user) or an admin. False when no such live note is visible.
bool quickNoteStop(uint64_t id, const char* user, bool isAdmin);
bool quickNoteCancel(uint64_t id, const char* user, bool isAdmin);
// Fills *out (no text). True while a note is live.
bool quickNoteSnapshot(QuickNoteSnapshot* out);
bool quickNoteActive();
// The longest note: notemaxsec (10-120 s), capped at the CM5 recorder's 30 s
// on Pi builds. Any task.
uint32_t quickNoteMaxMs();
// The system clock was set or stepped (Clock's PEND_RESOLVE duty, main loop):
// the worker gives this boot's "boot" notes their real time. Any task.
void quickNoteClockSet();
// A note may start soon (the Notes lens page is open): on local-STT builds the
// verified model is loaded now, off this task, and kept resident until two
// minutes after the last call or note (sttKeepWarm). Nothing on CM5 builds.
// Any task; never blocks.
void quickNoteWarm();

struct CommandEntry;
extern const CommandEntry noteCommands[];
extern const size_t noteCommandsCount;
#else
inline bool quickNoteActive() { return false; }
inline uint32_t quickNoteMaxMs() { return 120000; }
inline void quickNoteClockSet() {}
#endif
