/**
 * Microphone Sensor Module - ESP32-S3 PDM Microphone Implementation
 * 
 * Uses I2S peripheral to interface with PDM microphone on XIAO ESP32S3 Sense.
 */

#include "System_Microphone.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_Filesystem.h"  // requireQuotedPath (uniform quoted-path rule)
#include <esp_attr.h>

// Gate on the mic SUBSYSTEM (PDM silicon OR a G2-capable build), not on the PDM
// silicon flag — this module is source-agnostic (all capture flows through
// HAL_Audio) so it must compile and run on PDM-less boards where the G2 glasses
// mic is the only source. See System_SensorStubs (gated on !ENABLE_MICROPHONE in
// lockstep, or these definitions collide).
#if ENABLE_MICROPHONE

#include <Arduino.h>
#include "System_VFS.h"
#include "System_MemUtil.h"
#include "System_Debug.h"
#include "System_TaskUtils.h"
#include "System_Command.h"
#include "System_CommandTypes.h"
#include "System_Utils.h"   // argWantsJson
#include <ArduinoJson.h>
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_I2C.h"
#include "System_Microphone_OLED.h"
#include "System_AuthIdentity.h"  // currentAuthContext (recording path checks)
#include "System_UartLink.h"      // cmd_voicefetch (registered in micCommands below)
#include "System_LiveAudio.h"     // best-effort owned-recorder PCM shadow
#include "System_Dictation.h"     // keyboard voice input: terminal capture hook
#include "G2_Glasses.h"           // native EvenAI owner/login-epoch fence
#include "HAL_Audio.h"            // single PDM/I2S capture owner (audioCaptureStart/audioReadPcm)

// XIAO ESP32S3 Sense PDM Microphone Pins
#define MIC_PDM_CLK_PIN     42        // PDM CLK (GPIO42 on XIAO Sense)
#define MIC_PDM_DATA_PIN    41        // PDM DATA (GPIO41 on XIAO Sense)

// PDM I2S capture is owned by HAL_Audio — no local channel handle here.

// Default audio settings
#define DEFAULT_SAMPLE_RATE   16000
#define DEFAULT_BIT_DEPTH     16
#define DEFAULT_CHANNELS      1

// Buffer for audio capture
#define AUDIO_BUFFER_SIZE     1024
#define RECORDING_CHUNK_SIZE  4096
#define MAX_RECORDING_SEC     60

// Defaults: new recordings go to SD when writable; legacy/internal copies may remain on LittleFS.
static const char kMicRecLittleFS[] = "/recordings";
static const char kMicRecSD[]       = "/sd/recordings";

static String micPrimaryRecordingsFolder() {
  if (VFS::isSDWritable()) {
    return String(kMicRecSD);
  }
  return String(kMicRecLittleFS);
}

// Microphone state
bool gMicRunning = false;
bool micConnected = false;

// Serialize whole source open/close transactions. The I2S mutex only protects
// individual backend operations; releasing it while closemic joins the recorder
// used to let a new openmic start, after which the old close path could clear
// the new owner's flags. This mutex deliberately spans the recorder join but is
// never taken by the recorder task or a BLE source-loss callback.
static StaticSemaphore_t gMicSourceOpMutexStorage;
static SemaphoreHandle_t gMicSourceOpMutex = nullptr;
static portMUX_TYPE gMicSourceOpInitMux = portMUX_INITIALIZER_UNLOCKED;

static bool ensureMicSourceOpMutex() {
  portENTER_CRITICAL(&gMicSourceOpInitMux);
  if (!gMicSourceOpMutex) {
    gMicSourceOpMutex = xSemaphoreCreateMutexStatic(&gMicSourceOpMutexStorage);
  }
  const bool ready = gMicSourceOpMutex != nullptr;
  portEXIT_CRITICAL(&gMicSourceOpInitMux);
  if (!ready) WARN_SYSTEMF("[MIC] source-operation mutex initialization failed");
  return ready;
}

class MicSourceOpGuard {
 public:
  MicSourceOpGuard() : held_(false) {
    if (ensureMicSourceOpMutex()) {
      held_ = xSemaphoreTake(gMicSourceOpMutex, portMAX_DELAY) == pdTRUE;
    }
  }
  ~MicSourceOpGuard() {
    if (held_) xSemaphoreGive(gMicSourceOpMutex);
  }
  explicit operator bool() const { return held_; }

 private:
  bool held_;
};

// Microphone info
int micSampleRate = DEFAULT_SAMPLE_RATE;
int micBitDepth = DEFAULT_BIT_DEPTH;
int micChannels = DEFAULT_CHANNELS;
int micGain = 50;  // Software gain 0-100%

// Recording state
static TaskHandle_t recordingTaskHandle = nullptr;
static File recordingFile;
static uint32_t recordingStartTime = 0;
static uint32_t recordingSamples = 0;
static char currentRecordingPath[64] = {0};

// One synchronized authority for the WAV capture lifecycle. gMicRunning owns
// the HAL audio source; this state owns recordingFile/currentRecordingPath and
// remains busy until the task has closed the WAV, emitted its terminal events,
// cleared its handle, and published IDLE as the final shared write.
struct MicRecordingControl {
  MicRecordingState state;
  bool stopRequested;
  bool sourceLost;
  MicRecordingOwner owner;
  bool discardRequested;
  bool dispositionSealed;
  uint32_t admissionSessionEpoch;
  LiveAudioRecorderAuthorization shadowAuth;
  uint8_t resultNext;
  uint8_t resultCount;
  MicRecordingResult results[4];
};
static portMUX_TYPE gMicRecordingMux = portMUX_INITIALIZER_UNLOCKED;
static MicRecordingControl gMicRecordingControl = {
  MicRecordingState::IDLE,
  {},
  false,
  MIC_RECORDING_OWNER_MANUAL,
  false,
  false,
  0,
  {},
  0,
  0,
  {},
};

static constexpr size_t kMicRecordingResultHistory =
    sizeof(gMicRecordingControl.results) / sizeof(gMicRecordingControl.results[0]);

static void micRecordingOwnerHex(MicRecordingOwner owner, char out[17]) {
  snprintf(out, 17, "%08lx%08lx",
           (unsigned long)(owner >> 32),
           (unsigned long)(owner & 0xFFFFFFFFu));
}

static bool micRecordingOwnerValid(MicRecordingOwner owner) {
  // EvenAI IDs are boot-nonce:exchange-counter. Requiring both halves prevents
  // a malformed/stale token from collapsing into a manual-like or partially
  // initialized identity even though the combined uint64_t is nonzero.
  return (uint32_t)(owner >> 32) != 0 &&
         (uint32_t)(owner & 0xFFFFFFFFu) != 0;
}

MicRecordingState getMicRecordingState() {
  portENTER_CRITICAL(&gMicRecordingMux);
  const MicRecordingState state = gMicRecordingControl.state;
  portEXIT_CRITICAL(&gMicRecordingMux);
  return state;
}

const char* micRecordingStateName(MicRecordingState state) {
  switch (state) {
    case MicRecordingState::IDLE:       return "idle";
    case MicRecordingState::STARTING:   return "starting";
    case MicRecordingState::CAPTURING:  return "capturing";
    case MicRecordingState::STOPPING:   return "stopping";
    case MicRecordingState::FINALIZING: return "finalizing";
  }
  return "unknown";
}

bool micRecordingBusy() {
  return getMicRecordingState() != MicRecordingState::IDLE;
}

bool micRecordingCapturing() {
  return getMicRecordingState() == MicRecordingState::CAPTURING;
}

static bool micRecordingStopRequested() {
  portENTER_CRITICAL(&gMicRecordingMux);
  const bool requested = gMicRecordingControl.stopRequested;
  portEXIT_CRITICAL(&gMicRecordingMux);
  return requested;
}

static bool micRecordingSourceLost() {
  portENTER_CRITICAL(&gMicRecordingMux);
  const bool lost = gMicRecordingControl.sourceLost;
  portEXIT_CRITICAL(&gMicRecordingMux);
  return lost;
}

// The successful claim is the first per-capture mutation. In particular, a
// duplicate/delayed start cannot reset the incumbent capture's VAD trackers,
// path, file, or task state before discovering that it is busy.
static bool micRecordingHasCompletedOwnerLocked(MicRecordingOwner owner) {
  if (owner == MIC_RECORDING_OWNER_MANUAL) return false;
  for (size_t age = 0; age < gMicRecordingControl.resultCount; ++age) {
    const size_t slot =
        (gMicRecordingControl.resultNext + kMicRecordingResultHistory - 1 - age) %
        kMicRecordingResultHistory;
    const MicRecordingResult& result = gMicRecordingControl.results[slot];
    if (result.valid && result.owner == owner) return true;
  }
  return false;
}

static bool micRecordingClaimStart(MicRecordingOwner owner,
                                   uint32_t admissionSessionEpoch,
                                   const LiveAudioRecorderAuthorization& shadowAuth) {
  bool claimed = false;
  portENTER_CRITICAL(&gMicRecordingMux);
  if (gMicRecordingControl.state == MicRecordingState::IDLE &&
      !micRecordingHasCompletedOwnerLocked(owner)) {
    gMicRecordingControl.state = MicRecordingState::STARTING;
    gMicRecordingControl.stopRequested = false;
    gMicRecordingControl.sourceLost = false;
    gMicRecordingControl.owner = owner;
    gMicRecordingControl.discardRequested = false;
    gMicRecordingControl.dispositionSealed = false;
    gMicRecordingControl.admissionSessionEpoch = admissionSessionEpoch;
    gMicRecordingControl.shadowAuth = shadowAuth;
    claimed = true;
  }
  portEXIT_CRITICAL(&gMicRecordingMux);
  return claimed;
}

// Non-blocking and safe from BLE callbacks. STARTING may skip CAPTURING when a
// stop races setup; CAPTURING advances to STOPPING exactly once. Repeated stop
// requests while STOPPING/FINALIZING are idempotent.
static bool micRecordingRequestStop(bool sourceLost = false) {
  bool changed = false;
  portENTER_CRITICAL(&gMicRecordingMux);
  switch (gMicRecordingControl.state) {
    case MicRecordingState::STARTING:
    case MicRecordingState::CAPTURING:
      // Terminal cause is first-wins. A disconnect after an explicit/VAD stop
      // has already reached STOPPING/FINALIZING must not retroactively turn a
      // valid, closing WAV into a source-loss failure.
      if (sourceLost) gMicRecordingControl.sourceLost = true;
      gMicRecordingControl.state = MicRecordingState::STOPPING;
      changed = true;
      [[fallthrough]];
    case MicRecordingState::STOPPING:
      gMicRecordingControl.stopRequested = true;
      break;
    case MicRecordingState::FINALIZING:
      gMicRecordingControl.stopRequested = true;
      break;
    case MicRecordingState::IDLE:
      break;
  }
  portEXIT_CRITICAL(&gMicRecordingMux);
  return changed;
}

// Owner-scoped stop admission. Discard is monotonic but deliberately separate
// from sourceLost: the first terminal cause still wins, while a wearer
// dismissal can request removal even if VAD already moved the capture into
// STOPPING. A request that arrives after dispositionSealed is handled by the
// exact-result cleanup in stopRecordingOwned(); it can never target a newer
// capture or a global last-path string.
static MicRecordingOwnedOp micRecordingRequestStopOwned(
    MicRecordingOwner owner, bool discard) {
  if (!micRecordingOwnerValid(owner)) {
    return MicRecordingOwnedOp::INVALID_OWNER;
  }
  MicRecordingOwnedOp result = MicRecordingOwnedOp::OK;
  portENTER_CRITICAL(&gMicRecordingMux);
  if (gMicRecordingControl.state == MicRecordingState::IDLE) {
    result = micRecordingHasCompletedOwnerLocked(owner)
                 ? MicRecordingOwnedOp::OK
                 : MicRecordingOwnedOp::NOT_FOUND;
  } else if (gMicRecordingControl.owner != owner) {
    // A delayed stop/discard for an already-completed owner is still safe
    // while a newer owner records: the completion ring supplies the old exact
    // path and the operation below never mutates the active FSM. Reject only
    // when this ID has neither the active lease nor a retained terminal result.
    result = micRecordingHasCompletedOwnerLocked(owner)
                 ? MicRecordingOwnedOp::OK
                 : MicRecordingOwnedOp::OWNER_MISMATCH;
  } else {
    if (discard) gMicRecordingControl.discardRequested = true;
    switch (gMicRecordingControl.state) {
      case MicRecordingState::STARTING:
      case MicRecordingState::CAPTURING:
        gMicRecordingControl.state = MicRecordingState::STOPPING;
        [[fallthrough]];
      case MicRecordingState::STOPPING:
      case MicRecordingState::FINALIZING:
        gMicRecordingControl.stopRequested = true;
        break;
      case MicRecordingState::IDLE:
        break;
    }
  }
  portEXIT_CRITICAL(&gMicRecordingMux);
  return result;
}

// Called by the recorder task after its buffer exists. A stop that arrived
// during STARTING wins; the task then skips PCM and goes straight to cleanup.
static bool micRecordingEnterCapturing() {
  bool capture = false;
  portENTER_CRITICAL(&gMicRecordingMux);
  if (gMicRecordingControl.state == MicRecordingState::STARTING &&
      !gMicRecordingControl.stopRequested) {
    gMicRecordingControl.state = MicRecordingState::CAPTURING;
    capture = true;
  } else if (gMicRecordingControl.state == MicRecordingState::STARTING) {
    gMicRecordingControl.state = MicRecordingState::STOPPING;
  }
  portEXIT_CRITICAL(&gMicRecordingMux);
  return capture;
}

static void micRecordingEnterFinalizing() {
  portENTER_CRITICAL(&gMicRecordingMux);
  if (gMicRecordingControl.state != MicRecordingState::IDLE) {
    gMicRecordingControl.state = MicRecordingState::FINALIZING;
    gMicRecordingControl.stopRequested = true;
  }
  portEXIT_CRITICAL(&gMicRecordingMux);
}

static void micRecordingPublishIdle(bool publishResult = false,
                                    bool failed = false,
                                    bool discarded = false,
                                    MicRecordingOwner owner =
                                        MIC_RECORDING_OWNER_MANUAL,
                                    uint32_t samples = 0,
                                    uint32_t sampleRate = 0,
                                    const char* path = nullptr,
                                    const char* failure = nullptr,
                                    bool degraded = false) {
  // Format outside the critical section. The mux protects publication, not
  // libc formatting; keeping its hold time bounded matters because status and
  // source-loss callbacks read/write the same control object on both cores.
  MicRecordingResult completed{};
  if (publishResult) {
    completed.valid = true;
    completed.failed = failed;
    completed.discarded = discarded;
    completed.degraded = degraded;
    completed.owner = owner;
    completed.samples = samples;
    completed.sampleRate = sampleRate;
    snprintf(completed.path, sizeof(completed.path), "%s", path ? path : "");
    snprintf(completed.failure, sizeof(completed.failure), "%s",
             failure ? failure : "");
  }
  portENTER_CRITICAL(&gMicRecordingMux);
  if (publishResult) {
    gMicRecordingControl.results[gMicRecordingControl.resultNext] = completed;
    gMicRecordingControl.resultNext =
        (gMicRecordingControl.resultNext + 1) % kMicRecordingResultHistory;
    if (gMicRecordingControl.resultCount < kMicRecordingResultHistory) {
      gMicRecordingControl.resultCount++;
    }
  }
  gMicRecordingControl.stopRequested = false;
  gMicRecordingControl.sourceLost = false;
  gMicRecordingControl.owner = MIC_RECORDING_OWNER_MANUAL;
  gMicRecordingControl.discardRequested = false;
  gMicRecordingControl.dispositionSealed = false;
  gMicRecordingControl.admissionSessionEpoch = 0;
  gMicRecordingControl.shadowAuth = LiveAudioRecorderAuthorization{};
  gMicRecordingControl.state = MicRecordingState::IDLE;
  portEXIT_CRITICAL(&gMicRecordingMux);
}

static MicRecordingResult micRecordingLastResult() {
  MicRecordingResult out{};
  portENTER_CRITICAL(&gMicRecordingMux);
  if (gMicRecordingControl.resultCount > 0) {
    const size_t slot =
        (gMicRecordingControl.resultNext + kMicRecordingResultHistory - 1) %
        kMicRecordingResultHistory;
    out = gMicRecordingControl.results[slot];
  }
  portEXIT_CRITICAL(&gMicRecordingMux);
  out.path[sizeof(out.path) - 1] = '\0';
  out.failure[sizeof(out.failure) - 1] = '\0';
  return out;
}

MicRecordingOwnedOp getRecordingResultOwned(MicRecordingOwner owner,
                                            MicRecordingResult* out) {
  if (out) *out = MicRecordingResult{};
  if (!micRecordingOwnerValid(owner)) {
    return MicRecordingOwnedOp::INVALID_OWNER;
  }
  MicRecordingOwnedOp op = MicRecordingOwnedOp::NOT_FOUND;
  portENTER_CRITICAL(&gMicRecordingMux);
  if (gMicRecordingControl.state != MicRecordingState::IDLE &&
      gMicRecordingControl.owner == owner) {
    op = MicRecordingOwnedOp::NOT_READY;
  } else {
    for (size_t age = 0; age < gMicRecordingControl.resultCount; ++age) {
      const size_t slot =
          (gMicRecordingControl.resultNext + kMicRecordingResultHistory - 1 - age) %
          kMicRecordingResultHistory;
      const MicRecordingResult& result = gMicRecordingControl.results[slot];
      if (result.valid && result.owner == owner) {
        if (out) *out = result;
        op = MicRecordingOwnedOp::OK;
        break;
      }
    }
    if (op == MicRecordingOwnedOp::NOT_FOUND &&
        gMicRecordingControl.state != MicRecordingState::IDLE) {
      op = MicRecordingOwnedOp::OWNER_MISMATCH;
    }
  }
  portEXIT_CRITICAL(&gMicRecordingMux);
  if (out) {
    out->path[sizeof(out->path) - 1] = '\0';
    out->failure[sizeof(out->failure) - 1] = '\0';
  }
  return op;
}

struct MicRecordingDispositionSnapshot {
  MicRecordingOwner owner;
  bool discard;
  uint32_t admissionSessionEpoch;
};

static MicRecordingDispositionSnapshot micRecordingSealDisposition() {
  MicRecordingDispositionSnapshot out{};
  portENTER_CRITICAL(&gMicRecordingMux);
  gMicRecordingControl.dispositionSealed = true;
  out.owner = gMicRecordingControl.owner;
  out.discard = gMicRecordingControl.discardRequested;
  out.admissionSessionEpoch =
      gMicRecordingControl.admissionSessionEpoch;
  portEXIT_CRITICAL(&gMicRecordingMux);
  return out;
}

struct MicRecordingShadowSnapshot {
  MicRecordingOwner owner;
  LiveAudioRecorderAuthorization auth;
};

static MicRecordingShadowSnapshot micRecordingShadowSnapshot() {
  MicRecordingShadowSnapshot out{};
  portENTER_CRITICAL(&gMicRecordingMux);
  out.owner = gMicRecordingControl.owner;
  out.auth = gMicRecordingControl.shadowAuth;
  portEXIT_CRITICAL(&gMicRecordingMux);
  return out;
}

// Paths passed here were created and retained by the recorder itself, never
// supplied by a command. Treat an already-absent file as a successful discard;
// this makes an idempotent stopid/discard safe after a prior cleanup.
static bool micRecordingRemoveExactPath(const char* path, const char* authTag) {
  if (!path || !path[0]) return true;
  FsLockGuard fsGuard(authTag);
  const String ownedPath(path);
  const AuthContext auth = VFS::systemAuth(authTag);
  if (!VFS::existsGuarded(ownedPath, auth)) return true;
  return VFS::removeGuarded(ownedPath, auth);
}

static const char* micRecordingBasename(const char* path) {
  if (!path) return "";
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static bool micRecordingExpectedFilenameMatches(
    MicRecordingOwner owner, const MicRecordingResult& result,
    const char* expectedFilename) {
  // A cleanup command accepts one basename, never a caller-selected path.
  if (!expectedFilename || !expectedFilename[0] ||
      strchr(expectedFilename, '/') || strchr(expectedFilename, '\\')) {
    return false;
  }
  if (result.path[0]) {
    return strcmp(micRecordingBasename(result.path), expectedFilename) == 0;
  }
  // Successful/idempotent cleanup redacts the path. Machine-owned filenames
  // are token-derived, so a retry can still authenticate the same basename
  // without retaining a stale fetchable path in the public result.
  if (result.discarded) {
    char ownerHex[17];
    char derived[32];
    micRecordingOwnerHex(owner, ownerHex);
    snprintf(derived, sizeof(derived), "rec_%s.wav", ownerHex);
    return strcmp(derived, expectedFilename) == 0;
  }
  return false;
}

// Optional per-recording silence auto-stop (opt-in). 0 = disabled (default):
// the recording runs until an explicit stop or the 60s cap — the behavior every
// existing caller keeps (G2 menu Record, g2micwav, manual micrecord, OLED). >0 =
// end the recording after this many ms of TRAILING silence, once speech has
// actually been heard. Set ONLY by the CM5/STT `micrecord start vad` path, so no
// other recorder is affected. Threshold is ADAPTIVE from BOTH ends: it tracks
// the loudest chunk AND the ambient floor (running min with a slow upward
// leak), calling a chunk silent below max(2*floor, peak/8, absolute floor).
// The earlier peak-only rule (silent = <20% of peak) assumed speech is much
// louder than ambient; on the G2 glasses mic speech-to-ambient is only ~7x,
// which parked the threshold inside the noise band — ambient wobbles kept
// resetting the silence window (bench replay 2026-08-08: good runs stopped
// ~1.5-2s late, quiet-speech runs never stopped and rode the caller's max
// window). Bit-exact offline mirror: cm5/ai-service/tools/vad_replay.py
// (--proposed flag) — keep the two in lockstep. Still biased toward
// recording-too-long: if speech is never clearly heard it simply never
// auto-stops and falls back to the caller's max window — it can't clip you.
static uint32_t gRecSilenceStopMs = 0;   // 0 = feature off for this recording
// Effective source rate latched for this capture. G2 is fixed at 16 kHz even
// when the persisted PDM rate differs, and later settings/source changes must
// not alter this WAV's VAD, duration, or header math.
static uint32_t gRecSampleRate = AUDIO_HAL_SAMPLE_RATE;
// True when the current/last recording captured from the G2 mic — gates the
// `degraded=1` token below so a stale G2 watchdog latch can never tag a
// later PDM recording.
static bool gRecWasG2Source = false;

// Delivered-rate token for stopped-recording replies. STRICT token format
// ("degraded=1", appended AFTER the path): the CM5 parsers substring-match
// the words "stopped"/"discarded" as terminal and regex the path as
// (\S+\.wav) — a key=value token after the path is provably inert to both.
// Takes the RESULT (sealed at finalize), never the live latch: statusid/stop
// replies can describe an earlier capture, and the live latch belongs to
// whatever stream is running NOW.
static const char* micDegradedSuffixFor(const MicRecordingResult& r) {
  return r.degraded ? " degraded=1" : "";
}
static int32_t  gRecPeakAvg       = 0;   // loudest chunk amplitude this recording
static int32_t  gRecFloorAvg      = -1;  // ambient-floor tracker; -1 = unseeded
static uint32_t gRecSilenceMs     = 0;   // accumulated trailing silence
static bool     gRecHeardSpeech   = false;
// Diagnostic-only trace state (never feeds a VAD decision). Exists to tell
// three failure modes apart on the wake path, all of which end in the same
// 1280 ms minimum-length WAV: (a) the floor tracker seeding on a speech-level
// chunk so real speech scores as silence, (b) a capture that genuinely opened
// after the utterance ended, (c) ambient above kRecSpeechFloorAvg latching
// speech on room tone. Read the [MIC_VAD] chunk trace under the MIC_VALUES
// debug flag; the summary line at stop prints regardless.
static uint32_t gRecChunkIdx      = 0;   // 0-based chunk counter this recording
static int32_t  gRecMinAvg        = -1;  // quietest chunk avg seen (-1 = none)
static int32_t  gRecLatchChunk    = -1;  // chunk that first latched speech
static int32_t  gRecLatchAvg      = 0;   // the avg that latched it
// Trailing window the ambient floor is the minimum of. 40 chunks ~= 5.1 s at
// 128 ms/chunk: long enough that normal inter-word gaps keep a true-ambient
// sample inside it, short enough that the estimate recovers within a few
// seconds of a room-level change.
static const size_t kRecFloorWinChunks = 40;
static int32_t  gRecFloorWin[kRecFloorWinChunks];
static size_t   gRecFloorWinIdx   = 0;
static size_t   gRecFloorWinCount = 0;
static const int32_t  kRecSpeechFloorAvg  = 120; // avg above this => speech present
static const int32_t  kRecSilenceFloorAvg = 45;  // avg below this => silence (floor)
static const uint32_t kRecVadMinMs        = 800;  // never auto-stop in the first 0.8s

// ── Opt-in leading/trailing silence trim (`micrecord ... vad <ms> trim`) ─────
// The VAD needs a long trailing window to be sure the speaker stopped — 1800 ms
// on the wake path, because a 768 ms mid-sentence pause has been observed on
// hardware and a shorter window truncates people mid-sentence. But none of that
// silence is worth anything to STT, and it costs ~60 UART frames plus STT time
// on every capture. So: keep the DETECTION window, drop the RECORDED tail.
//
// Implemented as write suppression, never write-then-truncate: the Arduino File
// class has no truncate(), and shortening only the WAV header leaves trailing
// PCM that the host parses as the next RIFF chunk — a content-dependent
// intermittent failure. Suppressing the write keeps the declared length and the
// on-disk length equal by construction.
//
// A silent chunk cannot be classified as "trailing" until we know speech never
// resumes, so silent chunks are HELD in a ring and only committed once a later
// chunk proves the utterance continued. Before speech latches, the same ring
// retains only a short pre-roll and discards older leading room tone; if speech
// never latches, the bounded safety valve disables withholding and preserves a
// diagnosable full capture.
//
// Opt-in is a separate token from `vad` on purpose: tools/vad_replay.py needs a
// byte-exact WAV to reproduce the device's per-chunk trace, so a bare
// `micrecord start vad 1800` must keep producing today's file.
static bool     gRecTrimEnabled    = false;
static uint32_t gRecWrittenSamples = 0;  // samples ON DISK — feeds the WAV header.
                                         // recordingSamples stays "captured", because
                                         // the VAD's elapsedMs and the max-duration cap
                                         // both derive from it and must keep advancing
                                         // while chunks are held back.
static uint32_t gRecTrimDropped    = 0;  // trailing chunks discarded at stop; diagnostics
static uint32_t gRecTrimLeadDropped = 0; // leading chunks discarded before latch; diagnostics
static bool     gRecPreLatchGaveUp = false;  // speech never latched — stop withholding
// True only when the VAD itself ended the capture (not the max-duration cap,
// not an external `micrecord stop`). Gates the mic_autostop EVT: a host-issued
// stop needs no push, since the host already knows.
static bool     gRecVadAutoStopped = false;
// Per-exchange timing stamps for the `evenai_timing` EVT (device-side half of
// the exec plan's Required timing record). Absolute device millis — the host
// anchors them against the evenai_wake arrival and computes deltas itself
// (absolute values cannot go negative on a stale wake stamp).
static uint32_t gRecFirstPcmMs = 0;   // first successful audioReadPcm
static uint32_t gRecVadEndMs   = 0;   // VAD auto-stop decision

// Since-wake preroll (2026-08-10). Native wake→capture-claim measured
// 680-996 ms; audio spoken in that gap was discarded by the trim-to-zero
// boundary, losing leading words in 2/4 field runs. For captures owned by
// the LIVE native EvenAI exchange, keep ring audio back to the wake stamp —
// bounded — instead of trimming to zero. Everything else (manual captures,
// stale/superseded wakes, fabricated startid) fails closed to trim-to-zero,
// preserving the stale-backlog fix this boundary exists for.
// The kept backlog MUST drain paced: the live-shadow queue is 4×2048
// samples and overflow is a hard ABORT_TX_BACKPRESSURE. 30 ms/chunk covers
// the ~21.4 ms wire time of one chunk at 2 Mbaud plus interleaved link
// frames; pacing also extends past the counter while reads stay saturated
// (full-chunk reads = ring still backlogged) so the handoff burst is paced
// too. Host-side queue budgets were raised in the same change
// (stt-queue-chunks 16, PCM inbox 48 KB).
static size_t gRecPrerollKeptSamples = 0;
static constexpr uint32_t kRecPrerollCapMs        = 1200;
static constexpr uint32_t kRecPrerollMaxWakeAgeMs = 5000;
static constexpr uint32_t kRecPrerollPaceMs       = 30;
// Set only for recorder-internal failures (allocation/header/data write). The
// task closes and removes the unusable placeholder/partial WAV, suppresses
// MIC_SAVED + mic_autostop, and leaves a precise stop-command error behind.
static bool     gRecCaptureFailed  = false;
static char     gRecFailureReason[48] = {0};
static size_t   gRecRingSlots      = 0;  // ring capacity this capture (0 = trim off)
static size_t   gRecHeldCount      = 0;  // silent chunks currently held
static size_t   gRecHeldHead       = 0;  // ring index of the OLDEST held chunk
static size_t   gRecHeldBytes[16];       // byte count per held slot (reads can be short)
// Tail kept after the last speech chunk. 3 chunks = 384 ms (RECORDING_CHUNK_SIZE
// is 4096 BYTES = 2048 samples = 128 ms at 16 kHz — not 4096 samples).
static const size_t kRecTailSlots    = 3;
// Pre-roll kept ahead of the latch, same 384 ms. Erring long protects onsets.
static const size_t kRecPreRollSlots = 3;
// If speech never latches by here, give up withholding and record straight
// through, so a failed capture is still diagnosable. See the use site.
static const uint32_t kRecPreLatchHoldMaxMs = 4000;
// Hard ceiling regardless of the armed window: 16 * 4096 = 65,536 B of PSRAM.
// `micrecord start vad 10000 trim` is legal (the CLI accepts 200..10000), and
// must not ask for 320 KB. Overflow FLUSHES the oldest held chunk rather than
// dropping it, so an under-sized ring degrades to "trims less", never to
// "loses audio" — which is what protects a long mid-sentence pause.
static const size_t kRecRingMaxSlots = 16;

// Command buffer
EXT_RAM_BSS_ATTR static char gMicCmdBuffer[512];

// ── Source helpers (unified PDM / G2 mic) ────────────────────────────────────
// HAL_Audio delivers int16 mono for every source. PDM runs at micSampleRate; the
// G2 ring is always 16 kHz. Use this for the WAV header + duration math so a G2
// recording is never mis-stamped.
static uint32_t micEffectiveSampleRate() {
  return (audioGetSource() == AUDIO_SRC_G2_LEFT) ? AUDIO_HAL_SAMPLE_RATE
                                                 : (uint32_t)micSampleRate;
}
static const char* micSourceName() {
  switch (audioGetSource()) {
    case AUDIO_SRC_LOCAL_PDM: return "pdm";
    case AUDIO_SRC_G2_LEFT:   return "g2";
    default:                  return "none";
  }
}
// The DSP chain (DC removal, HPF, pre-emphasis, gain) is tuned for the PDM mic;
// the G2 feed is already clean decoded LC3 PCM and the SR path consumes it raw,
// so skip processing for G2 to keep the level meter + recordings consistent with
// what SR hears.
static void micProcessForSource(int16_t* buf, size_t n) {
  if (audioGetSource() == AUDIO_SRC_LOCAL_PDM) applyMicAudioProcessing(buf, n);
}
// Reconcile this module's cached flags with the HAL. If the underlying capture
// vanished (e.g. the G2 dropped mid-session and onDisconnect released the
// lease), request STOPPING but never publish a false IDLE: only the recorder
// task may close/finalize the WAV and publish IDLE.
static void micReconcileState() {
  const bool active = audioCaptureActive() &&
                      audioSourceAvailable(audioGetSource());
  if (gMicRunning && !active) {
    microphoneNotifySourceLost();
  }
  micConnected = active;
}

void microphoneNotifySourceLost() {
  // Publish the first-wins terminal cause before dropping the loop gate. If
  // gMicRunning went false first, the recorder could exit and claim an
  // ordinary stop in the few instructions before SOURCE_LOST was recorded.
  const bool changed = micRecordingRequestStop(/*sourceLost=*/true);
  gMicRunning = false;
  micConnected = false;
  if (changed) {
    DEBUG_MIC_LIFECYCLEF("[MIC_REC_FSM] source lost — STOPPING requested");
  }
}

// Audio level tracking
static int lastAudioLevel = 0;
static uint32_t lastAudioLevelMs = 0;

// Audio preprocessing state (shared between mic and ESP-SR)
static int32_t gMicDcOffset = 0;
static bool gMicDcOffsetInitialized = false;
static float gMicBaseSoftwareGain = 24.0f;

// High-pass filter state (~50Hz cutoff at 16kHz sample rate)
// alpha = 1 / (1 + 2*pi*fc/fs) where fc=50Hz, fs=16000Hz
static const float kHighPassAlpha = 0.9806f;
static float gMicHighPassState = 0.0f;
static int16_t gMicHighPassPrevIn = 0;

// Pre-emphasis filter coefficient (boosts high frequencies for speech clarity)
static const float kPreEmphCoeff = 0.97f;
static int16_t gMicPreEmphPrevSample = 0;

float getMicSoftwareGainMultiplier() {
  if (micGain <= 0) return 0.0f;
  return gMicBaseSoftwareGain * ((float)micGain / 50.0f);
}

int32_t getMicDcOffset() {
  return gMicDcOffset;
}

void resetMicAudioProcessingState() {
  gMicDcOffset = 0;
  gMicDcOffsetInitialized = false;
  gMicHighPassState = 0.0f;
  gMicHighPassPrevIn = 0;
  gMicPreEmphPrevSample = 0;
}

void applyMicAudioProcessing(int16_t* buf, size_t sampleCount, float gainMultiplier, bool filtersEnabled) {
  if (!buf || sampleCount == 0) return;

  // Use provided gain or calculate from micGain setting
  if (gainMultiplier <= 0.0f) {
    gainMultiplier = getMicSoftwareGainMultiplier();
  }

  // Calculate DC offset from this chunk (running average)
  int64_t sum = 0;
  for (size_t i = 0; i < sampleCount; i++) {
    sum += buf[i];
  }
  int32_t chunkDc = (int32_t)(sum / (int64_t)sampleCount);

  // Slowly adapt DC offset estimate (EMA with alpha=0.1)
  if (!gMicDcOffsetInitialized) {
    gMicDcOffset = chunkDc;
    gMicDcOffsetInitialized = true;
  } else {
    gMicDcOffset = gMicDcOffset + (chunkDc - gMicDcOffset) / 10;
  }

  // Apply audio preprocessing pipeline:
  // 1. DC offset removal (always)
  // 2. High-pass filter (~50Hz cutoff) - optional
  // 3. Pre-emphasis filter (boost high frequencies) - optional
  // 4. Software gain (always)
  for (size_t i = 0; i < sampleCount; i++) {
    // Step 1: Remove DC offset
    float sample = (float)(buf[i] - gMicDcOffset);
    
    if (filtersEnabled) {
      // Step 2: High-pass filter (removes low-freq rumble/hum)
      // y[n] = alpha * (y[n-1] + x[n] - x[n-1])
      float hpOut = kHighPassAlpha * (gMicHighPassState + sample - (float)gMicHighPassPrevIn);
      gMicHighPassState = hpOut;
      gMicHighPassPrevIn = (int16_t)sample;
      sample = hpOut;
      
      // Step 3: Pre-emphasis filter (boosts high frequencies for speech clarity)
      // y[n] = x[n] - alpha * x[n-1]
      float preEmphOut = sample - kPreEmphCoeff * (float)gMicPreEmphPrevSample;
      gMicPreEmphPrevSample = (int16_t)sample;
      sample = preEmphOut;
    }
    
    // Step 4: Apply software gain
    sample *= gainMultiplier;
    
    // Clamp to 16-bit range
    int32_t sampleInt = (int32_t)sample;
    if (sampleInt > 32767) sampleInt = 32767;
    if (sampleInt < -32768) sampleInt = -32768;
    buf[i] = (int16_t)sampleInt;
  }
}

// WAV header structure
struct WavHeader {
  char riff[4] = {'R','I','F','F'};
  uint32_t fileSize;
  char wave[4] = {'W','A','V','E'};
  char fmt[4] = {'f','m','t',' '};
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1;  // PCM
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char data[4] = {'d','a','t','a'};
  uint32_t dataSize;
};

static bool writeWavHeader(File& f, uint32_t dataSize, uint32_t sampleRate) {
  WavHeader header;
  header.numChannels = micChannels;
  header.sampleRate = sampleRate ? sampleRate : AUDIO_HAL_SAMPLE_RATE;
  header.bitsPerSample = 16;   // HAL delivers int16 for every source — micBitDepth is cosmetic; a 32 here corrupts the WAV
  header.blockAlign = header.numChannels * (header.bitsPerSample / 8);
  header.byteRate = header.sampleRate * header.blockAlign;
  header.dataSize = dataSize;
  header.fileSize = dataSize + sizeof(WavHeader) - 8;
  
  if (!f.seek(0)) return false;
  return f.write((uint8_t*)&header, sizeof(header)) == sizeof(header);
}

static void micRecordingMarkFailed(const char* reason) {
  if (!gRecCaptureFailed) {
    gRecCaptureFailed = true;
    snprintf(gRecFailureReason, sizeof(gRecFailureReason), "%s",
             reason ? reason : "internal error");
  }
  micRecordingRequestStop();
}

static void recordingTask(void* param) {
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] ========== recordingTask() ENTRY ==========");
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] Task running on core %d", xPortGetCoreID());
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] Heap: %u, PSRAM: %u", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  INFO_MIC_LIFECYCLEF("Recording task started");
  
  // Slot 0 is the working buffer; slots 1..gRecRingSlots are the trim ring, so
  // this stays ONE allocation and one free per capture exactly as before.
  // Sized from the armed window (never a literal 1800 — the ask path arms 1200)
  // and clamped to kRecRingMaxSlots.
  gRecRingSlots = 0;
  if (gRecTrimEnabled) {
    const uint32_t rate = gRecSampleRate;
    const uint32_t nomChunkMs = rate
        ? (uint32_t)((uint64_t)(RECORDING_CHUNK_SIZE / sizeof(int16_t)) * 1000 / rate) : 128;
    size_t want = nomChunkMs ? (gRecSilenceStopMs / nomChunkMs) + 2 : kRecRingMaxSlots;
    if (want > kRecRingMaxSlots)      want = kRecRingMaxSlots;
    if (want < kRecTailSlots + 1)     want = kRecTailSlots + 1;
    gRecRingSlots = want;
  }
  size_t allocBytes = RECORDING_CHUNK_SIZE * (1 + gRecRingSlots);
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] Allocating %u byte recording buffer (%u ring slots)...",
                       (unsigned)allocBytes, (unsigned)gRecRingSlots);
  int16_t* buffer = nullptr;
  if (!micRecordingStopRequested()) {
    buffer = (int16_t*)ps_alloc(allocBytes, AllocPref::PreferPSRAM, "mic.rec.buf");
  }
  if (!buffer && gRecRingSlots && !micRecordingStopRequested()) {
    // Degrade, don't fail: drop the ring and record untrimmed.
    INFO_MIC_LIFECYCLEF("Trim ring alloc failed (%u B) — recording untrimmed", (unsigned)allocBytes);
    gRecRingSlots = 0; gRecTrimEnabled = false;
    allocBytes = RECORDING_CHUNK_SIZE;
    buffer = (int16_t*)ps_alloc(allocBytes, AllocPref::PreferPSRAM, "mic.rec.buf");
  }
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] ps_alloc returned: %p", buffer);

  if (!buffer && !micRecordingStopRequested()) {
    DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] *** BUFFER ALLOCATION FAILED! ***");
    INFO_MIC_LIFECYCLEF("Failed to allocate recording buffer");
    micRecordingMarkFailed("recording buffer allocation failed");
  }
  // Ring slot n lives at buffer + (1 + n) * (RECORDING_CHUNK_SIZE / 2) int16s.
  auto ringSlot = [&](size_t n) -> uint8_t* {
    return (uint8_t*)buffer + RECORDING_CHUNK_SIZE * (1 + n);
  };
  // Commit the oldest held chunk to the file and advance the ring.
  auto writePcm = [&](const uint8_t* data, size_t bytes) -> size_t {
    if (!recordingFile || !data || bytes == 0) return 0;
    const size_t written = recordingFile.write(data, bytes);
    gRecWrittenSamples += written / sizeof(int16_t);
    if (written != bytes) {
      DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] short PCM write: %u/%u B",
                           (unsigned)written, (unsigned)bytes);
      micRecordingMarkFailed("short PCM write");
    }
    return written;
  };
  auto flushOldestHeld = [&]() -> bool {
    if (!gRecHeldCount) return true;
    size_t bytes = gRecHeldBytes[gRecHeldHead];
    const bool ok = writePcm(ringSlot(gRecHeldHead), bytes) == bytes;
    gRecHeldHead = (gRecHeldHead + 1) % gRecRingSlots;
    gRecHeldCount--;
    return ok;
  };

  const bool captureStarted = buffer && micRecordingEnterCapturing();
  MicRecordingShadowSnapshot shadowCapture{};
  bool liveShadowStarted = false;
  if (captureStarted) {
    // openmic may have kept the G2 decoder running long before this recording
    // claim. Its 2.048 s jitter ring can therefore be full here; draining that
    // pre-claim backlog in a tight loop both prepends stale WAV audio and
    // overwhelms the bounded live-shadow queue. Immediately after CAPTURING
    // is published, discard that backlog for every G2 WAV, whether shadow is
    // armed or not. Effective sample zero linearizes when the G2 ring mutex is
    // acquired; a BLE packet concurrent with this O(1) trim can fall on either
    // side of that boundary.
    //
    // EXCEPTION (2026-08-10, since-wake preroll): when this capture is owned
    // by the LIVE native EvenAI exchange, keep ring audio back to the wake
    // stamp (capped) — the wake→claim arming gap is 0.7-1.0 s and speech in
    // it was being discarded. The exact-owner gate fails closed for manual
    // captures, fabricated startid, and stale/superseded/EXIT'd wakes.
    // Snapshot moved above the trim: owner/auth are claim-time-immutable.
    // NOTE: any future non-wake `startid` producer inherits preroll semantics
    // through this gate only if it reuses the live exchange id — by design.
    // Coverage split: fully effective when the mic stream was already flowing
    // pre-wake (bench gate + steady daemon); a cold wake (openmic submitted
    // post-wake) recovers only stream-on→claim audio — never stale pre-wake
    // content, since the feed-arm clears an idle ring.
    shadowCapture = micRecordingShadowSnapshot();
    size_t keepSamples = 0;
    if (gRecWasG2Source &&
        shadowCapture.owner != MIC_RECORDING_OWNER_MANUAL) {
      const uint64_t liveExchange = g2EvenAiActiveExchangeId();
      const uint32_t wakeMs = g2EvenAiLastWakeMs();
      const uint32_t nowMs  = millis();
      const uint32_t wakeAge = nowMs - wakeMs;   // uint32 math: wrap-safe
      if (liveExchange != 0 &&
          (uint64_t)shadowCapture.owner == liveExchange &&
          wakeMs != 0 && wakeAge <= kRecPrerollMaxWakeAgeMs) {
        const uint32_t keepMs =
            wakeAge < kRecPrerollCapMs ? wakeAge : kRecPrerollCapMs;
        keepSamples = (size_t)keepMs * gRecSampleRate / 1000u;
      }
    }
    gRecPrerollKeptSamples = 0;
    const size_t discardedBeforeCapture =
        audioTrimBufferedPcm("mic", keepSamples);
    if (keepSamples > 0) {
      // What the ring actually held may be less than requested; recompute
      // from the post-trim depth for honest accounting/pacing.
      const size_t depthNow = audioGetSource() == AUDIO_SRC_G2_LEFT
                                  ? g2MicAfeRingDepth() : 0;
      gRecPrerollKeptSamples = depthNow < keepSamples ? depthNow : keepSamples;
    }
    if (discardedBeforeCapture > 0 || gRecPrerollKeptSamples > 0) {
      INFO_MIC_LIFECYCLEF(
          "Recorder boundary: discarded %u pre-capture samples, kept %u "
          "since-wake preroll samples",
          (unsigned)discardedBeforeCapture,
          (unsigned)gRecPrerollKeptSamples);
    }
    // The WAV/header already exists and the recorder has committed CAPTURING.
    // Admit shadow before the first audioReadPcm(), but never let admission
    // failure alter the canonical recorder lifecycle.
    const LiveAudioRecorderSource shadowSource =
        audioGetSource() == AUDIO_SRC_G2_LEFT
            ? LiveAudioRecorderSource::G2
            : LiveAudioRecorderSource::PDM;
    liveShadowStarted = liveAudioRecorderBegin(
        shadowCapture.owner, shadowSource, gRecSampleRate,
        shadowCapture.auth);
    sensorStatusBumpWith("micrecstart");
    const char* slash = strrchr(currentRecordingPath, '/');
    systemEventPost(SYSEVT_MIC_RECORD_STARTED,
                    slash ? slash + 1 : currentRecordingPath);
    DEBUG_MIC_LIFECYCLEF("[MIC_REC_FSM] STARTING -> CAPTURING");
  } else {
    // A stop may have raced STARTING, or setup failed. Either way, the task
    // still owns the common close/remove/finalization path below.
    micRecordingRequestStop();
  }

  uint32_t maxSamples = gRecSampleRate * MAX_RECORDING_SEC;
  const uint32_t captureLoopStartedMs = millis();
  uint32_t lastPcmMs = captureLoopStartedMs;
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] Max samples: %lu (sampleRate=%u, maxSec=%d)", maxSamples, (unsigned)gRecSampleRate, MAX_RECORDING_SEC);
  
  uint32_t loopCount = 0;
  // Countdown of preroll samples still draining; pacing below also extends
  // past zero while reads stay saturated (see the pacing comment).
  size_t prerollRemaining = gRecPrerollKeptSamples;
  while (captureStarted && micRecordingCapturing() &&
         !micRecordingStopRequested() && gMicRunning &&
         audioCaptureActive() && audioSourceAvailable(audioGetSource()) &&
         recordingSamples < maxSamples &&
         (uint32_t)(millis() - captureLoopStartedMs) < MAX_RECORDING_SEC * 1000u) {
    size_t bytesRead = 0;
    esp_err_t err = ESP_OK;
    {
      // Read PCM via HAL_Audio (single I2S owner); errors fold into 0 samples.
      size_t got = audioReadPcm(buffer, RECORDING_CHUNK_SIZE / sizeof(int16_t), 100);
      bytesRead = got * sizeof(int16_t);
    }
    
    if (err == ESP_OK && bytesRead > 0 && recordingFile) {
      lastPcmMs = millis();
      if (gRecFirstPcmMs == 0) gRecFirstPcmMs = lastPcmMs;
      // Mirrors the VAD's per-chunk silence verdict out of the block below so
      // the write can act on it. Stays false when the VAD is not armed, so
      // untrimmed recorders take the plain write path unchanged.
      bool chunkScoredSilent = false;
      {
        int32_t sum = 0;
        size_t sampleCount = bytesRead / sizeof(int16_t);

        micProcessForSource(buffer, sampleCount);

        for (size_t i = 0; i < sampleCount; i++) {
          int16_t v = buffer[i];
          sum += (v < 0) ? -v : v;
        }
        int32_t avg = (sampleCount > 0) ? (sum / (int32_t)sampleCount) : 0;
        int level = map(avg, 0, 16384, 0, 100);
        level = constrain(level, 0, 100);
        lastAudioLevel = level;
        lastAudioLevelMs = millis();

        // Opt-in silence auto-stop (STT/AI flow only; gRecSilenceStopMs==0 skips
        // this entirely, so every other recorder is untouched). Adaptive: track
        // the loudest chunk, treat a chunk as silence at <20% of that (or below
        // the absolute floor), and end the recording after gRecSilenceStopMs of
        // trailing silence — but only once real speech has been heard and past a
        // min duration, so a slow start or a mid-sentence pause never clips it.
        if (gRecSilenceStopMs > 0) {
          if (avg > gRecPeakAvg) gRecPeakAvg = avg;
          if (gRecMinAvg < 0 || avg < gRecMinAvg) gRecMinAvg = avg;
          // The pre-update floor is what makes the seeding visible: when it
          // equals avg, this chunk set the floor and cut>=2*avg guarantees a
          // silence verdict no matter how loud the chunk actually was.
          const int32_t floorBefore = gRecFloorAvg;
          // Ambient-floor tracker: minimum over a bounded trailing window.
          //
          // This replaced a running-min-with-upward-leak. That leak ran on
          // every chunk, so it climbed straight through continuous speech —
          // a 12 s utterance walked the floor 168 -> 279 and dragged cut to
          // 558, at which point ordinary speech valleys scored as silence.
          // Gating the leak on "ambient-ish" chunks does not fix it either:
          // the gate has to be ~2x the floor to recover from an under-pinned
          // estimate, but 2x the floor IS the cut, so every chunk that scores
          // silence leaks the floor up and raises the cut — positive feedback.
          //
          // A windowed minimum has no such loop. It cannot drift above the
          // quietest chunk in the window, and it recovers from a freak-quiet
          // chunk automatically once that chunk ages out. Cost is one pass
          // over kRecFloorWinChunks int32s per chunk (~40 compares / 128 ms).
          gRecFloorWin[gRecFloorWinIdx] = avg;
          gRecFloorWinIdx = (gRecFloorWinIdx + 1) % kRecFloorWinChunks;
          if (gRecFloorWinCount < kRecFloorWinChunks) gRecFloorWinCount++;
          gRecFloorAvg = gRecFloorWin[0];
          for (size_t fi = 1; fi < gRecFloorWinCount; fi++) {
            if (gRecFloorWin[fi] < gRecFloorAvg) gRecFloorAvg = gRecFloorWin[fi];
          }
          // TWO gates, deliberately different.
          //
          // trimCut is floor-relative only: "is this chunk meaningfully above
          // the ambient noise?" It decides what is DISCARDED.
          //
          // cut adds the peak-relative term (1/8 of the loudest chunk, i.e.
          // -18 dB) and decides when the trailing-silence clock runs, i.e. when
          // to STOP.
          //
          // They were one value, and that truncated words. Word-final
          // consonants and vowel decays sit 20-30 dB below a stressed vowel, so
          // once peak grew they scored below peak/8, went into the hold ring,
          // and were dropped at stop. MEASURED 2026-08-13: "potato" captured as
          // "potat", "picture" as "pict", "That's the price" as "is the price".
          // Keying the discard decision to the MEASURED floor instead is
          // mic-neutral by construction: it self-scales to the G2 temple mic
          // (~-60 dBFS floor) and to the noisier on-carrier PDM mic (~-40 dBFS)
          // without favouring either, where any peak-relative or absolute
          // threshold necessarily suits one and hurts the other.
          //
          // trimCut <= cut always, so strictly fewer chunks are held than
          // before: auto-stop timing is unchanged, only less audio is thrown
          // away.
          int32_t trimCut = kRecSilenceFloorAvg;
          if (2 * gRecFloorAvg > trimCut) trimCut = 2 * gRecFloorAvg;
          int32_t cut = trimCut;
          if (gRecPeakAvg / 8 > cut)  cut = gRecPeakAvg / 8;
          // Speech has to stand above the MEASURED ambient floor, not merely
          // above a fixed absolute level. Room tone on the G2 temple mic runs
          // ~150 mean-abs — comfortably over kRecSpeechFloorAvg — so the
          // absolute gate alone latched on silence, started the trailing-
          // silence clock on chunk 0 and killed the capture at the 1280 ms
          // minimum ~1.6 s after the wake word, before the wearer could
          // speak. Requiring a floor that was already seeded coming in also
          // means chunk 0 can never latch: it sets the floor by construction,
          // so cut >= 2*avg and it can never clear its own bar.
          //
          // This restores the safety property the header claims: when nothing
          // clearly louder than ambient arrives, speech never latches, the
          // auto-stop stays unreachable, and the capture rides the caller's
          // max window instead of truncating. Pausing to think is then free.
          const bool latchedNow = (!gRecHeardSpeech && floorBefore >= 0
                                   && avg >= cut && avg >= kRecSpeechFloorAvg);
          if (latchedNow) {
            gRecHeardSpeech = true;
            gRecLatchChunk = (int32_t)gRecChunkIdx;
            gRecLatchAvg = avg;
          }
          const uint32_t chunkMs = (gRecSampleRate > 0)
              ? (uint32_t)((uint64_t)sampleCount * 1000 / gRecSampleRate) : 128;
          const bool scoredSilent = (gRecHeardSpeech && avg < cut);
          // Discard decision uses the floor-relative gate, so a quiet word tail
          // is committed rather than held; the auto-stop clock below keeps the
          // peak-relative gate so end-of-speech detection does not get slower.
          chunkScoredSilent = (gRecHeardSpeech && avg < trimCut);
          if (scoredSilent) gRecSilenceMs += chunkMs;
          else              gRecSilenceMs = 0;
          const uint32_t elapsedMs = (gRecSampleRate > 0)
              ? (uint32_t)((uint64_t)recordingSamples * 1000 / gRecSampleRate) : 0;
          // Never-latched safety valve. Speech may simply never latch (the VAD
          // deliberately allows that — see the header above; the capture then
          // rides the caller's max window instead of truncating). Without this,
          // leading-trim would withhold the ENTIRE capture and emit a ~384 ms
          // file. Past this point we stop withholding and write straight
          // through, so a failed capture yields room tone that
          // _archive_failed_utterance and tools/vad_replay.py can still work
          // with — an almost-empty WAV is strictly worse than a long one.
          if (gRecTrimEnabled && !gRecHeardSpeech && !gRecPreLatchGaveUp
              && elapsedMs >= kRecPreLatchHoldMaxMs) {
            gRecPreLatchGaveUp = true;
            DEBUG_MIC_LIFECYCLEF("[MIC_VAD] no latch by %lums — leading trim off for this capture",
                                 (unsigned long)elapsedMs);
          }
          // Per-chunk decision trace. Everything the offline replay needs to
          // reproduce the verdict, in the order the code evaluates it.
          DEBUG_MIC_VALUESF("[MIC_VAD] c%-3lu avg=%-5ld floor=%-5ld->%-5ld cut=%-5ld peak=%-5ld "
                            "%s %s sil=%lums el=%lums",
                            (unsigned long)gRecChunkIdx, (long)avg,
                            (long)floorBefore, (long)gRecFloorAvg, (long)cut, (long)gRecPeakAvg,
                            gRecHeardSpeech ? (latchedNow ? "LATCH" : "spch ") : "----- ",
                            scoredSilent ? "SIL" : "snd",
                            (unsigned long)gRecSilenceMs, (unsigned long)elapsedMs);
          gRecChunkIdx++;
          if (gRecHeardSpeech && gRecSilenceMs >= gRecSilenceStopMs && elapsedMs >= kRecVadMinMs) {
            DEBUG_MIC_LIFECYCLEF("[MIC_VAD] auto-stop: %lums trailing silence (peak avg=%ld)",
                                 (unsigned long)gRecSilenceMs, (long)gRecPeakAvg);
            gRecVadAutoStopped = true;
            gRecVadEndMs = millis();
            micRecordingRequestStop();  // STOPPING; task remains busy through close/event
          }
        }
      }
      // One nonblocking producer copy after all DSP/VAD decisions and before
      // the filesystem lock. Shadow failure/overflow only aborts live PCM;
      // it can never fail or delay the canonical WAV.
      if (liveShadowStarted) {
        (void)liveAudioRecorderOffer(shadowCapture.owner, buffer,
                                     bytesRead / sizeof(int16_t));
      }
      // Preroll drain pacing — MUST sit here, after the offer and OUTSIDE the
      // FsLockGuard scope below (never sleep holding the FS lock). The
      // 4-slot shadow queue hard-aborts on overflow (ABORT_TX_BACKPRESSURE);
      // one 2048-sample chunk costs ~21.4 ms of wire at 2 Mbaud, so 30 ms per
      // backlogged chunk keeps depth ≤2. Pacing continues past the counter
      // while reads stay saturated (a full-chunk read means ≥128 ms of ring
      // backlog remains — the preroll→live handoff burst); steady-state G2
      // reads return ≤800 samples so this self-terminates. Gated on a kept
      // preroll so PDM captures (whose reads are always full chunks) and
      // ordinary G2 captures never pace.
      if (gRecPrerollKeptSamples > 0 && liveShadowStarted) {
        const size_t gotSamples = bytesRead / sizeof(int16_t);
        const bool saturated =
            gotSamples >= RECORDING_CHUNK_SIZE / sizeof(int16_t);
        if (prerollRemaining > 0 || saturated) {
          prerollRemaining =
              gotSamples >= prerollRemaining ? 0 : prerollRemaining - gotSamples;
          vTaskDelay(pdMS_TO_TICKS(kRecPrerollPaceMs));
        }
      }
      FsLockGuard fsGuard("mic.record.write");
      size_t written = 0;
      // Leading silence: before the latch, hold a rolling pre-roll and discard
      // anything older. Deliberately does NOT defer opening the file — the WAV
      // is created and headered exactly as before, we only withhold writes. That
      // keeps every "does the recording exist / what is its path" assumption
      // downstream true, which is the whole risk class this avoids.
      // Preroll disables the pre-latch hold: the hold ring keeps only
      // kRecPreRollSlots (384 ms) and DISCARDS older chunks, so with ≥700 ms
      // of recovered leading audio a late latch would silently re-drop the
      // very words the preroll recovered. Leading silence reaching STT is
      // harmless; trailing-silence hold and auto-stop are unaffected.
      const bool preLatchHold = gRecTrimEnabled && !gRecHeardSpeech &&
                                !gRecPreLatchGaveUp &&
                                gRecPrerollKeptSamples == 0;
      if (gRecTrimEnabled && (chunkScoredSilent || preLatchHold)) {
        if (preLatchHold) {
          // Overflow DISCARDS the oldest: this is the audio we are here to
          // remove. Keeping kRecPreRollSlots means the ring always holds the
          // most recent 384 ms, which becomes the pre-roll the instant speech
          // latches. The pre-roll is not optional — the VAD latches on an
          // energy RISE, so the first phoneme normally lives in the chunk
          // BEFORE the latch chunk, and trimming to the latch clips onsets.
          if (gRecHeldCount >= kRecPreRollSlots) {
            gRecHeldHead = (gRecHeldHead + 1) % gRecRingSlots;
            gRecHeldCount--;
            gRecTrimLeadDropped++;
          }
        } else {
          // Trailing-silence candidate — hold it until we know whether speech
          // resumes. Ring full: commit the oldest instead of dropping it, so an
          // under-sized ring trims less rather than eating a mid-sentence pause.
          if (gRecHeldCount == gRecRingSlots && !flushOldestHeld()) {
            chunkScoredSilent = false;
          }
        }
        const size_t slot = (gRecHeldHead + gRecHeldCount) % gRecRingSlots;
        memcpy(ringSlot(slot), buffer, bytesRead);
        gRecHeldBytes[slot] = bytesRead;
        gRecHeldCount++;
      } else {
        // Speech (including the latching chunk itself, which arrives here with
        // gRecHeardSpeech already set), or post-give-up audio. Anything held is
        // either the pre-roll or a mid-utterance pause — commit it first so the
        // audio stays contiguous.
        while (gRecHeldCount && flushOldestHeld()) {}
        if (!gRecCaptureFailed) {
          written = writePcm((uint8_t*)buffer, bytesRead);
        }
      }
      // Always counts CAPTURED samples: the VAD's elapsedMs and the
      // max-duration loop bound both read it and must keep advancing while
      // chunks are held back.
      recordingSamples += bytesRead / sizeof(int16_t);

      // Log every 100 iterations
      if (loopCount % 100 == 0) {
        DEBUG_MIC_POLLINGF("[MIC_REC_TASK] Loop %lu: read=%u, written=%u, totalSamples=%lu",
                   loopCount, bytesRead, written, recordingSamples);
      }
    } else if (err != ESP_OK) {
      DEBUG_MIC_POLLINGF("[MIC_REC_TASK] i2s_channel_read error: 0x%x", err);
    } else if (bytesRead == 0) {
      // Log zero-byte reads periodically
      if (loopCount % 50 == 0) {
        DEBUG_MIC_POLLINGF("[MIC_REC_TASK] Loop %lu: i2s_channel_read returned 0 bytes (no data from mic)", loopCount);
      }
      if ((uint32_t)(millis() - lastPcmMs) >= 2000u &&
          !micRecordingStopRequested()) {
        micRecordingMarkFailed("audio source stalled");
      }
    } else if (!recordingFile) {
      micRecordingMarkFailed("recording file became unavailable");
    }
    
    loopCount++;
    // Don't add extra delay - i2s_channel_read already blocks for up to 100ms
    taskYIELD();
  }
  
  // Max duration, source loss, and an external HAL stop all converge on the
  // same STOPPING state. A G2 source can disappear without any UI/status poll,
  // so check the HAL here instead of relying on micReconcileState().
  const bool halGone = !audioCaptureActive() ||
                       !audioSourceAvailable(audioGetSource());
  // Claim STOPPING with the candidate cause, then read the synchronized
  // first-wins cause. This closes the gap where a disconnect callback could
  // win between a local cause snapshot and our separate stop request.
  const bool inferSourceLoss = !micRecordingStopRequested() && halGone;
  micRecordingRequestStop(/*sourceLost=*/inferSourceLoss);
  const bool sourceLost = micRecordingSourceLost();
  if (sourceLost) {
    gMicRunning = false;
    micConnected = false;
    if (!gRecCaptureFailed) {
      micRecordingMarkFailed("audio source lost");
    }
  }
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] Recording loop ended: state=%s gMicRunning=%d samples=%lu",
             micRecordingStateName(getMicRecordingState()), gMicRunning, recordingSamples);

  // No new capture may claim the shared path/file/trim globals from here until
  // the terminal event has been sent and IDLE is published at the very end.
  micRecordingEnterFinalizing();
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_FSM] STOPPING -> FINALIZING");

  // Commit the kept tail and discard the rest of the trailing silence. MUST run
  // before free(buffer) — the held chunks live inside that allocation — and
  // before the verdict line, which reports what was dropped.
  if (buffer && gRecTrimEnabled && recordingFile && !gRecCaptureFailed) {
    FsLockGuard fsGuard("mic.record.tail");
    const size_t keep = (gRecHeldCount < kRecTailSlots) ? gRecHeldCount : kRecTailSlots;
    gRecTrimDropped = gRecHeldCount - keep;
    for (size_t i = 0; i < keep; i++) {
      if (!flushOldestHeld()) break;
    }
    gRecHeldCount = 0;   // whatever is left IS the trimmed tail
  }

  // One-line VAD verdict, unconditional on the mic parent flag so a field
  // failure is diagnosable without pre-arming the per-chunk trace. Reads:
  //   latch=-1            -> speech never latched; the stop came from
  //                          elsewhere (max window / external stop / stream
  //                          death), NOT the VAD.
  //   latch=0             -> the very first chunk latched AND seeded the
  //                          floor; every later chunk within 6 dB of it scores
  //                          silence. This is the mis-seeded capture.
  //   min ~= peak         -> flat level for the whole capture (room tone that
  //                          cleared kRecSpeechFloorAvg), not an utterance.
  if (gRecSilenceStopMs > 0) {
    INFO_MICF("[MIC_VAD] verdict: chunks=%lu latch=%ld@avg%ld min=%ld peak=%ld "
              "floorEnd=%ld sil=%lums samples=%lu/%lu trimDrop=%lu+%lu%s rate=%u/%u",
              (unsigned long)gRecChunkIdx, (long)gRecLatchChunk, (long)gRecLatchAvg,
              (long)gRecMinAvg, (long)gRecPeakAvg, (long)gRecFloorAvg,
              (unsigned long)gRecSilenceMs, recordingSamples,
              (unsigned long)gRecWrittenSamples,
              (unsigned long)gRecTrimLeadDropped, (unsigned long)gRecTrimDropped,
              gRecPreLatchGaveUp ? " NOLATCH" : "",
              (unsigned)micSampleRate, (unsigned)gRecSampleRate);
  }
  
  free(buffer);
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] Buffer freed");

  // Finalize WAV file. The owner/discard decision is sealed only AFTER close:
  // an owned cancellation may arrive while the header is being patched, but
  // deletion must never race an open File or expose a half-finalized path.
  bool fileClosed = false;
  if (recordingFile) {
    if (!gRecCaptureFailed && gRecWrittenSamples == 0) {
      micRecordingMarkFailed("recording contained no PCM");
    }
    // gRecWrittenSamples, not recordingSamples: with trimming armed the two
    // diverge, and the header must describe what is actually on disk.
    uint32_t dataSize = gRecWrittenSamples * sizeof(int16_t);
    DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] Finalizing WAV: dataSize=%lu", dataSize);
    {
      FsLockGuard fsGuard("mic.record.finalize");
      if (!gRecCaptureFailed &&
          !writeWavHeader(recordingFile, dataSize, gRecSampleRate)) {
        micRecordingMarkFailed("WAV header finalization failed");
      }
      recordingFile.close();
    }
    fileClosed = true;
    DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] WAV file closed");
  } else {
    DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] WARNING: recordingFile is invalid!");
    micRecordingMarkFailed("recording file unavailable during finalization");
  }

  const MicRecordingDispositionSnapshot disposition =
      micRecordingSealDisposition();
  const bool removeRequested =
      gRecCaptureFailed || disposition.discard;
  bool removed = false;
  if (removeRequested) {
    removed = micRecordingRemoveExactPath(
        currentRecordingPath,
        disposition.discard ? "mic.record.owner_discard"
                            : "mic.record.failed_remove");
    if (!removed && disposition.discard && !gRecCaptureFailed) {
      micRecordingMarkFailed("discard cleanup failed");
    }
  }

  const bool discarded = disposition.discard && removed;
  const bool saved = fileClosed && !gRecCaptureFailed && !disposition.discard;
  if (liveShadowStarted) {
    // END is requested only after the retained WAV has closed successfully.
    // The TX worker drains every already-published queue slot before END.
    liveAudioRecorderFinish(
        shadowCapture.owner,
        saved ? LiveAudioRecorderOutcome::SAVED
              : (discarded ? LiveAudioRecorderOutcome::DISCARDED
                           : LiveAudioRecorderOutcome::FAILED));
  }
  if (discarded) {
    char ownerHex[17];
    micRecordingOwnerHex(disposition.owner, ownerHex);
    INFO_MIC_LIFECYCLEF("Recording discarded after close: owner=%s path=%s",
                        ownerHex, currentRecordingPath);
  } else if (saved) {
    INFO_MIC_LIFECYCLEF("Recording saved: %s (%lu samples)",
                        currentRecordingPath, recordingSamples);
    const char* slash = strrchr(currentRecordingPath, '/');
    systemEventPost(SYSEVT_MIC_SAVED,
                    slash ? slash + 1 : currentRecordingPath);
  } else if (gRecCaptureFailed && removed) {
    INFO_MIC_LIFECYCLEF("Recording failed and partial WAV removed: %s (%s)",
                        currentRecordingPath, gRecFailureReason);
  } else if (gRecCaptureFailed) {
    INFO_MIC_LIFECYCLEF("Recording failed; exact cleanup still required: %s (%s)",
                        currentRecordingPath, gRecFailureReason);
  }

  // Tell the host the VAD ended this capture, and hand it the path so it does
  // not need a `micrecord stop` round trip just to learn the filename —
  // MEASURED at 0.42s, 3.3% of an exchange, spent asking a question the device
  // could have volunteered.
  //
  // Emitted HERE, not at the auto-stop decision: the WAV is still open there
  // with unflushed data, and the host would race us to voicefetch it. From
  // this point the file is closed and immediately fetchable. Also outside
  // every FsLockGuard — uartLinkWriteFrame can wait up to 1000 ms on sTxMutex,
  // and no FS-lock -> TX-mutex ordering exists in this codebase (cmd_voicefetch
  // deliberately closes its FS scope before framing). Do not introduce one.
  //
  // Gated on the VAD having been the thing that stopped it: an external
  // `micrecord stop` needs no push. The event is fenced to the login epoch
  // latched at admission; a glasses/OLED capture with no CM5 authority is a
  // clean no-op rather than leaking its path to a later login.
  if (saved && gRecVadAutoStopped && currentRecordingPath[0]) {
    char ownerHex[17] = "-";
    if (disposition.owner != MIC_RECORDING_OWNER_MANUAL) {
      micRecordingOwnerHex(disposition.owner, ownerHex);
    }
    char evt[192];
    if (disposition.owner == MIC_RECORDING_OWNER_MANUAL) {
      snprintf(evt, sizeof(evt), "mic_autostop %s", currentRecordingPath);
    } else {
      snprintf(evt, sizeof(evt), "mic_autostop %s %s",
               ownerHex, currentRecordingPath);
    }
    const bool pushed = uartLinkPushEventForSession(
        disposition.admissionSessionEpoch, evt);
    if (!pushed) {
      DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] mic_autostop push skipped (link down / not authed)");
    }
    // Device-side timing record for this exchange, one id-correlated EVT of
    // key=value tokens (absolute device millis; the host anchors against the
    // evenai_wake arrival and derives deltas). Closes the exec plan's
    // NOT-MEASURABLE stations: capture claim, first PCM read, VAD end.
    // wall-vs-sample skew is readable directly: (vadend-firstpcm) wall vs
    // samples/rate — under half-rate delivery wall runs ~2x samples.
    // NOTE: samples= is the CAPTURED count (pre-trim) — correct for skew
    // math; the WAV on disk holds fewer after VAD trim, so do not derive
    // file duration from this token.
    char tev[224];
    snprintf(tev, sizeof(tev),
             "evenai_timing %s wake_ms=%lu claim_ms=%lu firstpcm_ms=%lu "
             "vadend_ms=%lu closed_ms=%lu samples=%lu rate=%lu degraded=%u "
             "preroll_ms=%lu",
             ownerHex,
             (unsigned long)g2EvenAiLastWakeMs(),
             (unsigned long)recordingStartTime,
             (unsigned long)gRecFirstPcmMs,
             (unsigned long)gRecVadEndMs,
             (unsigned long)millis(),
             (unsigned long)recordingSamples,
             (unsigned long)gRecSampleRate,
             (unsigned)((gRecWasG2Source && g2MicCaptureDegraded()) ? 1 : 0),
             (unsigned long)(gRecSampleRate
                                 ? gRecPrerollKeptSamples * 1000u / gRecSampleRate
                                 : 0));
    if (!uartLinkPushEventForSession(disposition.admissionSessionEpoch, tev)) {
      DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] evenai_timing push skipped");
    }
  }

  // Dictation captures (keyboard voice input) terminate here too, but on their
  // own terms: unlike the EvenAI flow above this fires whether the VAD ended
  // the capture or the wearer stopped it by hand, and it is NOT fenced to
  // disposition.admissionSessionEpoch — a wearer-armed capture has no admitting
  // UART session, so that epoch is zero. The hook re-checks that this exact
  // owner is the pending dictation before it does anything observable, and
  // no-ops for every other owner. See System_Dictation.h for why the fence
  // differs and how narrowly the difference is scoped.
  //
  // Placed alongside the pushes above for the same reason they are here: the
  // WAV is closed and immediately fetchable, and we are outside every
  // FsLockGuard.
  dictationOnCaptureClosed(disposition.owner, currentRecordingPath, saved);

  // Recording over: drop the FAST hold and tear down our container (if we
  // created one) BEFORE publishing IDLE. Recorder-task context; the ~200 ms
  // page shutdown is fine here. Latched no-ops for PDM captures and when the
  // disconnect teardown already released.
  if (gRecWasG2Source) {
    g2MicLinkFastRelease();
    g2MicReleaseCaptureContainer();
  }

  // IDLE is the last shared lifecycle publication. At this point the buffer is
  // gone, the WAV is closed (or removed), and all terminal events are done.
  recordingTaskHandle = nullptr;
  const bool fileRetained = !removed && currentRecordingPath[0];
  micRecordingPublishIdle(/*publishResult=*/true, gRecCaptureFailed,
                          discarded, disposition.owner,
                          recordingSamples, gRecSampleRate,
                          fileRetained ? currentRecordingPath : "",
                          gRecFailureReason,
                          /*degraded=*/gRecWasG2Source && g2MicCaptureDegraded());
  sensorStatusBumpWith("micrecstop");
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_FSM] FINALIZING -> IDLE");
  DEBUG_MIC_LIFECYCLEF("[MIC_REC_TASK] ========== recordingTask() EXIT ==========");
  vTaskDelete(NULL);
}

static bool startRecordingInternal(MicRecordingOwner owner,
                                   uint32_t silenceStopMs, bool trim,
                                   uint32_t admissionSessionEpoch,
                                   const LiveAudioRecorderAuthorization& shadowAuth) {
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] ========== startRecording() ENTRY ==========");
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] gMicRunning=%d state=%s", gMicRunning,
                       micRecordingStateName(getMicRecordingState()));
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Heap: %u, PSRAM: %u", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  if (!gMicRunning) {
    DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] FAILED: mic not enabled");
    INFO_MIC_LIFECYCLEF("Cannot record - mic not enabled");
    return false;
  }

  // Re-resolve the source preference against what is connected NOW.
  //
  // initMicrophone() is the only place the preference is resolved, and nothing
  // on the wake path calls it — so a mic started before the glasses connected
  // stays latched on the fallback for every capture afterwards. MEASURED
  // 2026-08-13: `micsource` reported preference=g2, active=pdm for a whole
  // session with the glasses connected throughout, producing -25 dBFS audio and
  // badly wrong transcripts, and only a manual closemic/openmic fixed it.
  //
  // Checked here rather than on the BLE connect event for three reasons: this is
  // the only moment the source actually matters, it runs on the caller's task
  // instead of BTC_TASK (whose stack cannot afford a capture restart), and the
  // lifecycle claim below has not been taken yet, so a cycle cannot interrupt an
  // in-flight capture. If the glasses are NOT connected, audioSourceAvailable()
  // is false and the incumbent source is left alone — an out-of-range G2 must
  // never take the mic away from PDM.
  if (gSettings.micSource == "g2" &&
      audioGetSource() != AUDIO_SRC_G2_LEFT &&
      audioSourceAvailable(AUDIO_SRC_G2_LEFT) &&
      !micRecordingBusy()) {
    WARN_SYSTEMF("[MIC_START_REC] preference=g2 and the glasses are connected but "
                 "active=%s — re-resolving the source", micSourceName());
    if (stopMicrophone()) {
      if (!initMicrophone()) {
        // The mic is now DOWN: report the failed start rather than claiming a
        // recording that has no capture behind it.
        WARN_SYSTEMF("[MIC_START_REC] re-resolve could not restart the mic");
        INFO_MIC_LIFECYCLEF("Mic source re-resolve failed");
        return false;
      }
      WARN_SYSTEMF("[MIC_START_REC] source re-resolved to %s", micSourceName());
    } else {
      // Finalizing a previous capture — keep the incumbent source and record.
      WARN_SYSTEMF("[MIC_START_REC] re-resolve skipped (mic busy stopping)");
    }
  }

  // This claim MUST precede every write to the capture globals below. A start
  // rejected in STARTING/CAPTURING/STOPPING/FINALIZING therefore cannot corrupt
  // the incumbent recording's VAD, trim ring, path, file, or sample counters.
  if (!micRecordingClaimStart(owner, admissionSessionEpoch, shadowAuth)) {
    DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] FAILED: lifecycle busy (%s)",
                         micRecordingStateName(getMicRecordingState()));
    INFO_MIC_LIFECYCLEF("Already recording/finalizing");
    return false;
  }

  // Latch after ownership is claimed and before any file/header work. The HAL
  // source cannot be switched while this capture owns it.
  gRecSampleRate = micEffectiveSampleRate();
  gRecWasG2Source = (audioGetSource() == AUDIO_SRC_G2_LEFT);
  gRecFirstPcmMs = 0;
  gRecVadEndMs   = 0;
  gRecPrerollKeptSamples = 0;
  if (gRecWasG2Source) {
    // Recording-scoped G2 capture support (moved here from the HAL claim so
    // boot's idle-open mic no longer paints a page or pins the interval):
    // hold FAST conn params for the capture window, put up a container if
    // the lens is blank (the glasses silently drop mic audio without one),
    // and kick the stream so frames start now, not a keepalive lap later.
    // All released/torn down at finalize below; disconnect releases too.
    g2MicLinkFastAcquire();
    g2MicEnsureCaptureContainer();
    g2MicKickStream();
  }

  auto failStart = [&](const char* reason) -> bool {
    DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] setup failed: %s", reason ? reason : "unknown");
    if (gRecWasG2Source) {
      g2MicLinkFastRelease();           // latched no-ops if never acquired
      g2MicReleaseCaptureContainer();
    }
    micRecordingRequestStop();
    micRecordingEnterFinalizing();
    if (recordingFile) {
      FsLockGuard fsGuard("mic.record.start_cleanup");
      recordingFile.close();
    }
    const bool removed = micRecordingRemoveExactPath(
        currentRecordingPath, "mic.record.start_remove");
    recordingTaskHandle = nullptr;
    // Publish this setup failure as the newest terminal result. Otherwise a
    // later idempotent `micrecord stop` could return the previous capture's
    // path after this start already failed.
    const MicRecordingDispositionSnapshot disposition =
        micRecordingSealDisposition();
    micRecordingPublishIdle(/*publishResult=*/true, /*failed=*/true,
                            disposition.discard && removed,
                            disposition.owner,
                            /*samples=*/0, gRecSampleRate,
                            removed ? "" : currentRecordingPath,
                            reason ? reason : "recording setup failed");
    sensorStatusBumpWith("micrecstop");
    return false;
  };

  // Arm (or leave off, silenceStopMs==0) the opt-in silence auto-stop and reset
  // the trackers only after this capture owns STARTING.
  gRecSilenceStopMs = silenceStopMs;
  // Trimming is meaningless without the VAD — there is no "trailing silence"
  // to identify if nothing is scoring chunks. Both must be asked for.
  gRecTrimEnabled = (silenceStopMs > 0) && trim;
  gRecWrittenSamples = 0; gRecTrimDropped = 0; gRecTrimLeadDropped = 0;
  gRecPreLatchGaveUp = false; gRecVadAutoStopped = false;
  gRecCaptureFailed = false; gRecFailureReason[0] = '\0';
  gRecRingSlots = 0; gRecHeldCount = 0; gRecHeldHead = 0;
  gRecPeakAvg = 0; gRecFloorAvg = -1; gRecSilenceMs = 0; gRecHeardSpeech = false;
  gRecChunkIdx = 0; gRecMinAvg = -1; gRecLatchChunk = -1; gRecLatchAvg = 0;
  gRecFloorWinIdx = 0; gRecFloorWinCount = 0;
  currentRecordingPath[0] = '\0';
  recordingTaskHandle = nullptr;

  // closemic/source loss may race the claim. Preserve the ownership invariant
  // and unwind through FINALIZING rather than starting against a dead HAL.
  if (!gMicRunning || micRecordingStopRequested()) {
    return failStart("audio source stopped during setup");
  }
  
  String recDir = micPrimaryRecordingsFolder();
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Checking recordings folder: %s", recDir.c_str());
  {
    FsLockGuard fsGuard("mic.record.mkdir");
    if (!VFS::existsGuarded(recDir, currentAuthContext())) {
      DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Creating recordings folder...");
      bool created = VFS::mkdirGuarded(recDir, currentAuthContext());
      DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] mkdir returned: %d", created);
    } else {
      DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Recordings folder exists");
    }
  }
  
  // Machine-owned filenames carry the exact boot/session token. This prevents
  // delayed cleanup from crossing a reboot/millis collision and makes the
  // owner/path relationship independently inspectable. Manual callers retain
  // the legacy rec_<millis>.wav name.
  if (owner == MIC_RECORDING_OWNER_MANUAL) {
    snprintf(currentRecordingPath, sizeof(currentRecordingPath),
             "%s/rec_%lu.wav", recDir.c_str(), (unsigned long)millis());
  } else {
    char ownerHex[17];
    micRecordingOwnerHex(owner, ownerHex);
    snprintf(currentRecordingPath, sizeof(currentRecordingPath),
             "%s/rec_%s.wav", recDir.c_str(), ownerHex);
  }
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Recording path: %s", currentRecordingPath);
  
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Opening file for write...");
  {
    FsLockGuard fsGuard("mic.record.open");
    recordingFile = VFS::openGuarded(String(currentRecordingPath), "w", currentAuthContext(), true);
  }
  if (!recordingFile) {
    DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] *** FAILED to create file! ***");
    INFO_MIC_LIFECYCLEF("Failed to create recording file");
    return failStart("failed to create recording file");
  }
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] File opened successfully");
  
  // Write placeholder header (will be updated at end)
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Writing placeholder WAV header...");
  bool placeholderOk = false;
  {
    FsLockGuard fsGuard("mic.record.header");
    placeholderOk = writeWavHeader(recordingFile, 0, gRecSampleRate);
  }
  if (!placeholderOk) return failStart("failed to write placeholder WAV header");
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] Header written, file position: %lu", recordingFile.position());
  
  recordingStartTime = millis();
  recordingSamples = 0;
  
  // Start recording task
  taskStackRecord("mic_record", MIC_RECORD_STACK_WORDS);
  BaseType_t taskCreated = xTaskCreatePinnedToCore(
    recordingTask,
    "mic_record",
    MIC_RECORD_STACK_WORDS,
    nullptr,
    TASK_PRIORITY_HIGH,
    &recordingTaskHandle,
    1
  );
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] xTaskCreatePinnedToCore returned: %d, handle=%p", taskCreated, recordingTaskHandle);
  
  if (taskCreated != pdPASS) {
    DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] *** TASK CREATION FAILED! ***");
    return failStart("recording task creation failed");
  }
  
  DEBUG_MIC_LIFECYCLEF("[MIC_START_REC] ========== startRecording() SUCCESS ==========");
  INFO_MIC_LIFECYCLEF("Recording started: %s", currentRecordingPath);
  return true;
}

bool startRecording(uint32_t silenceStopMs, bool trim) {
  uint32_t admissionSessionEpoch = 0;
  const AuthContext& ctx = currentAuthContext();
  if (ctx.transport == SOURCE_UART)
    admissionSessionEpoch = uartLinkSessionEpoch();
  return startRecordingInternal(MIC_RECORDING_OWNER_MANUAL,
                                silenceStopMs, trim,
                                admissionSessionEpoch,
                                LiveAudioRecorderAuthorization{});
}

bool startRecordingOwned(MicRecordingOwner owner,
                         uint32_t silenceStopMs, bool trim) {
  if (!micRecordingOwnerValid(owner)) return false;
  return startRecordingInternal(owner, silenceStopMs, trim,
                                /*admissionSessionEpoch=*/0,
                                LiveAudioRecorderAuthorization{});
}

bool stopRecording(uint32_t timeoutMs) {
  MicRecordingState state = getMicRecordingState();
  DEBUG_MIC_LIFECYCLEF("[MIC_STOP_REC] stopRecording() called, state=%s",
                       micRecordingStateName(state));
  if (state == MicRecordingState::IDLE) {
    DEBUG_MIC_LIFECYCLEF("[MIC_STOP_REC] Already IDLE");
    return true;
  }

  if (micRecordingRequestStop()) {
    DEBUG_MIC_LIFECYCLEF("[MIC_REC_FSM] %s -> STOPPING",
                         micRecordingStateName(state));
  }

  const uint32_t startedMs = millis();
  while (micRecordingBusy() && (uint32_t)(millis() - startedMs) < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (micRecordingBusy()) {
    DEBUG_MIC_LIFECYCLEF("[MIC_STOP_REC] finalization wait expired after %lums (state=%s)",
                         (unsigned long)timeoutMs,
                         micRecordingStateName(getMicRecordingState()));
    INFO_MIC_LIFECYCLEF("Recording stop timed out while %s",
                        micRecordingStateName(getMicRecordingState()));
    return false;
  }

  DEBUG_MIC_LIFECYCLEF("[MIC_STOP_REC] Recording reached IDLE");
  INFO_MIC_LIFECYCLEF("Recording stopped");
  return true;
}

// Delete only a path retrieved from the completion slot for this exact owner,
// then atomically redact that same slot. This is also the late-discard path for
// a cancellation that linearized just after the recorder sealed its normal
// save decision; no global `currentRecordingPath` or newest-result fallback is
// consulted.
static MicRecordingOwnedOp micRecordingDiscardCompleted(
    MicRecordingOwner owner, const char* expectedFilename = nullptr) {
  MicRecordingResult result{};
  MicRecordingOwnedOp op = getRecordingResultOwned(owner, &result);
  if (op != MicRecordingOwnedOp::OK) return op;
  if (expectedFilename &&
      !micRecordingExpectedFilenameMatches(owner, result, expectedFilename)) {
    return MicRecordingOwnedOp::PATH_MISMATCH;
  }
  if (result.discarded) return MicRecordingOwnedOp::OK;
  if (!result.path[0]) return MicRecordingOwnedOp::NOT_FOUND;
  if (!micRecordingRemoveExactPath(result.path,
                                   "mic.record.owner_late_discard")) {
    return MicRecordingOwnedOp::DELETE_FAILED;
  }

  bool updated = false;
  portENTER_CRITICAL(&gMicRecordingMux);
  for (size_t age = 0; age < gMicRecordingControl.resultCount; ++age) {
    const size_t slot =
        (gMicRecordingControl.resultNext + kMicRecordingResultHistory - 1 - age) %
        kMicRecordingResultHistory;
    MicRecordingResult& stored = gMicRecordingControl.results[slot];
    if (stored.valid && stored.owner == owner) {
      stored.discarded = true;
      stored.path[0] = '\0';
      updated = true;
      break;
    }
  }
  portEXIT_CRITICAL(&gMicRecordingMux);
  return updated ? MicRecordingOwnedOp::OK
                 : MicRecordingOwnedOp::NOT_FOUND;
}

MicRecordingOwnedOp deleteRecordingOwned(
    MicRecordingOwner owner, const char* expectedFilename) {
  if (!micRecordingOwnerValid(owner)) {
    return MicRecordingOwnedOp::INVALID_OWNER;
  }
  return micRecordingDiscardCompleted(owner, expectedFilename);
}

MicRecordingOwnedOp requestStopRecordingOwned(MicRecordingOwner owner,
                                              bool discard) {
  return micRecordingRequestStopOwned(owner, discard);
}

MicRecordingOwnedOp stopRecordingOwned(MicRecordingOwner owner,
                                       bool discard,
                                       uint32_t timeoutMs) {
  MicRecordingOwnedOp op =
      micRecordingRequestStopOwned(owner, discard);
  if (op != MicRecordingOwnedOp::OK) return op;

  const uint32_t startedMs = millis();
  MicRecordingResult result{};
  while (true) {
    op = getRecordingResultOwned(owner, &result);
    if (op == MicRecordingOwnedOp::OK) break;
    if (op != MicRecordingOwnedOp::NOT_READY) return op;
    if ((uint32_t)(millis() - startedMs) >= timeoutMs) {
      return MicRecordingOwnedOp::TIMED_OUT;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (discard && !result.discarded) {
    return micRecordingDiscardCompleted(owner);
  }
  return MicRecordingOwnedOp::OK;
}

int getRecordingCount() {
  FsLockGuard fsGuard("mic.record.count");
  int count = 0;
  String seen = ",";
  auto walk = [&](const String& folder) {
    if (!VFS::existsGuarded(folder, currentAuthContext())) return;
    File dir = VFS::openGuarded(folder, "r", currentAuthContext());
    if (!dir || !dir.isDirectory()) return;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (f.isDirectory()) continue;
      String name = f.name();
      if (!name.endsWith(".wav")) continue;
      String token = "," + name + ",";
      if (seen.indexOf(token) >= 0) continue;
      seen += name + ",";
      count++;
    }
  };
  walk(String(kMicRecSD));
  walk(String(kMicRecLittleFS));
  return count;
}

String getRecordingsList() {
  String list = "";
  FsLockGuard fsGuard("mic.record.list");
  String seen = ",";
  auto walk = [&](const String& folder) {
    if (!VFS::existsGuarded(folder, currentAuthContext())) return;
    File dir = VFS::openGuarded(folder, "r", currentAuthContext());
    if (!dir || !dir.isDirectory()) return;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (f.isDirectory()) continue;
      String name = f.name();
      if (!name.endsWith(".wav")) continue;
      String token = "," + name + ",";
      if (seen.indexOf(token) >= 0) continue;
      seen += name + ",";
      if (list.length() > 0) list += ",";
      char entryBuf[80];
      snprintf(entryBuf, sizeof(entryBuf), "%s:%d", name.c_str(), (int)f.size());
      list += entryBuf;
    }
  };
  walk(String(kMicRecSD));
  walk(String(kMicRecLittleFS));
  return list;
}

bool deleteRecording(const char* filename) {
  String sdPath = String(kMicRecSD) + "/" + filename;
  String lfPath = String(kMicRecLittleFS) + "/" + filename;
  FsLockGuard fsGuard("mic.record.delete");
  if (VFS::existsGuarded(sdPath, currentAuthContext())) return VFS::removeGuarded(sdPath, currentAuthContext());
  if (VFS::existsGuarded(lfPath, currentAuthContext())) return VFS::removeGuarded(lfPath, currentAuthContext());
  return false;
}

bool initMicrophone() {
  MicSourceOpGuard sourceOp;
  if (!sourceOp) return false;

  WARN_SYSTEMF("[MIC_INIT] ########## initMicrophone() BEGIN ##########");
  WARN_SYSTEMF("[MIC_INIT] Heap: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  WARN_SYSTEMF("[MIC_INIT] Current state: gMicRunning=%d, micConnected=%d", gMicRunning, micConnected);

  // Source-loss/closemic can make the HAL flag false while the recorder still
  // owns a file in STOPPING/FINALIZING. Do not start a replacement source until
  // that capture has published IDLE.
  if (!gMicRunning && micRecordingBusy()) {
    WARN_SYSTEMF("[MIC_INIT] refusing start while recording is %s",
                 micRecordingStateName(getMicRecordingState()));
    return false;
  }

  I2sMicLockGuard i2sGuard("mic.init");
  
  if (gMicRunning) {
    WARN_SYSTEMF("[MIC_INIT] Already initialized - returning true");
    INFO_MIC_LIFECYCLEF("Already initialized");
    return true;
  }

  // Recheck after acquiring the I2S lock. Source loss can flip gMicRunning and
  // move the recorder to STOPPING between the optimistic check above and this
  // point; starting a new backend in that window would violate the lifecycle.
  if (micRecordingBusy() || audioCaptureBusy()) {
    WARN_SYSTEMF("[MIC_INIT] refusing locked start while recording=%s HAL owner='%s'",
                 micRecordingStateName(getMicRecordingState()),
                 audioCaptureOwner());
    return false;
  }

  // Only the caller that is actually about to start a fresh source may reset
  // shared DSP history. A duplicate openmic during capture must be a true no-op.
  resetMicAudioProcessingState();

  // Load settings from saved values
  WARN_SYSTEMF("[MIC_INIT] Loading settings from gSettings...");
  if (gSettings.microphoneSampleRate >= 8000 && gSettings.microphoneSampleRate <= 48000) {
    micSampleRate = gSettings.microphoneSampleRate;
  }
  if (gSettings.microphoneGain >= 0 && gSettings.microphoneGain <= 100) {
    micGain = gSettings.microphoneGain;
  }
  if (gSettings.microphoneBitDepth == 16 || gSettings.microphoneBitDepth == 32) {
    micBitDepth = gSettings.microphoneBitDepth;
  }

  WARN_SYSTEMF("[MIC_INIT] Audio settings: sampleRate=%d, bitDepth=%d, channels=%d, gain=%d%%",
               micSampleRate, micBitDepth, micChannels, micGain);
  WARN_SYSTEMF("[MIC_INIT] Pin config: CLK=%d, DATA=%d", MIC_PDM_CLK_PIN, MIC_PDM_DATA_PIN);
  INFO_MIC_LIFECYCLEF("Initializing PDM microphone...");
  STACK_TRACEF("initMicrophone.enter rate=%d bitDepth=%d channels=%d",
               micSampleRate, micBitDepth, micChannels);

  // Resolve the persisted source PREFERENCE against what is actually connected —
  // never assume a source exists. 'auto' (or an unavailable preference) falls
  // through to audioCaptureStart's PDM-first resolution.
  if (!audioAnySourceAvailable()) {
    WARN_SYSTEMF("[MIC_INIT] no mic source available (no PDM, no G2 connected) — cannot start");
    INFO_MIC_LIFECYCLEF("No mic source available");
    return false;
  }
  if (gSettings.micSource == "pdm" && audioSourceAvailable(AUDIO_SRC_LOCAL_PDM)) {
    audioSetSource(AUDIO_SRC_LOCAL_PDM);
  } else if (gSettings.micSource == "g2" && audioSourceAvailable(AUDIO_SRC_G2_LEFT)) {
    audioSetSource(AUDIO_SRC_G2_LEFT);
  }

  // Capture is owned by HAL_Audio (the single owner). audioCaptureStart resolves
  // the source (PDM-first if the selection is unavailable), starts the PDM I2S
  // channel + warm-up flush OR arms the G2 ring and enables the glasses stream.
  if (!audioCaptureStart("mic", (uint32_t)micSampleRate)) {
    WARN_SYSTEMF("[MIC_INIT] *** audioCaptureStart(\"mic\") FAILED at rate=%d Hz ***", micSampleRate);
    INFO_MIC_LIFECYCLEF("Failed to start PDM capture");
    return false;
  }
  gMicRunning = true;
  micReconcileState();  // set micConnected from live HAL state (source + capture)
  if (!gMicRunning || !micConnected) {
    WARN_SYSTEMF("[MIC_INIT] source disappeared while capture startup completed");
    audioCaptureStop("mic");
    return false;
  }
  WARN_SYSTEMF("[MIC_INIT] source=%s", micSourceName());
  sensorStatusBumpWith("openmic");
  systemEventPost(SYSEVT_SENSOR_STARTED, "Microphone");

  WARN_SYSTEMF("[MIC_INIT] ########## initMicrophone() SUCCESS ##########");
  WARN_SYSTEMF("[MIC_INIT] gMicRunning=%d, micConnected=%d", gMicRunning, micConnected);
  WARN_SYSTEMF("[MIC_INIT] Final heap: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  INFO_MIC_LIFECYCLEF("Initialized: %dHz, %d-bit, %d channel(s)", 
                micSampleRate, micBitDepth, micChannels);
  return true;
}

bool stopMicrophone() {
  MicSourceOpGuard sourceOp;
  if (!sourceOp) return false;

  STACK_TRACEF("stopMicrophone.enter gMicRunning=%d recState=%s",
               gMicRunning, micRecordingStateName(getMicRecordingState()));
  WARN_SYSTEMF("[MIC_STOP] ########## stopMicrophone() BEGIN ##########");
  WARN_SYSTEMF("[MIC_STOP] Current state: gMicRunning=%d recording=%s",
               gMicRunning, micRecordingStateName(getMicRecordingState()));

  {
    // Serialize the close gate with initMicrophone's locked recheck. Without
    // this, stop could observe pre-claim IDLE and return while an init already
    // held the I2S lifecycle lock and was about to claim HAL STARTING.
    I2sMicLockGuard lifecycleGuard("mic.stop.gate");
    if (!gMicRunning && !micRecordingBusy() &&
        !audioCaptureOwnedBy("mic")) {
      WARN_SYSTEMF("[MIC_STOP] Already stopped - returning");
      INFO_MIC_LIFECYCLEF("Already stopped");
      STACK_TRACEF("stopMicrophone.exit_already_stopped");
      return true;
    }

    // Close the start gate and claim the recorder's first-wins explicit stop
    // before HAL teardown. The recorder may finish its in-flight chunk, but a
    // resulting inactive HAL must not be misclassified as source loss.
    gMicRunning = false;
    micConnected = false;
    micRecordingRequestStop();
    STACK_TRACEF("stopMicrophone.cleared_gMicRunning");

    WARN_SYSTEMF("[MIC_STOP] Heap before stop: free=%u, PSRAM_free=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // HAL_Audio owns the I2S channel. Its nested PDM guard is reentrant for this
    // task; G2 teardown remains source-specific inside HAL_Audio.
    audioCaptureStop("mic");
  }

  // A stop can cancel HAL STARTING without blocking the BLE callback. Wait for
  // that starter to unwind its half-started backend and publish HAL IDLE before
  // allowing a replacement owner to proceed.
  const uint32_t halWaitStartedMs = millis();
  while (audioCaptureOwnedBy("mic") &&
         (uint32_t)(millis() - halWaitStartedMs) < 1500u) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  // Join the recorder only after the HAL is stopped/unblocked. Never force
  // IDLE if the task still owns the file.
  bool finalized = !micRecordingBusy() || stopRecording(3000);
  if (!finalized) {
    WARN_SYSTEMF("[MIC_STOP] recorder still %s after HAL teardown",
                 micRecordingStateName(getMicRecordingState()));
  }

  gMicRunning = false;
  const bool stopped = !micRecordingBusy() &&
                       !audioCaptureOwnedBy("mic");
  if (stopped) {
    sensorStatusBumpWith("closemic");
    systemEventPost(SYSEVT_SENSOR_STOPPED, "Microphone");
  }
  WARN_SYSTEMF("[MIC_STOP] ########## stopMicrophone() %s ##########",
               stopped ? "COMPLETE" : "INCOMPLETE");
  WARN_SYSTEMF("[MIC_STOP] Heap after stop: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  if (stopped) INFO_MIC_LIFECYCLEF("Stopped");
  else INFO_MIC_LIFECYCLEF("Source stopped; recorder still %s",
                           micRecordingStateName(getMicRecordingState()));
  return stopped;
}

int16_t* captureAudioSamples(size_t sampleCount, size_t* outLen) {
  WARN_SYSTEMF("[MIC_CAPTURE] captureAudioSamples(count=%u) called", (unsigned)sampleCount);
  WARN_SYSTEMF("[MIC_CAPTURE] gMicRunning=%d", gMicRunning);
  
  if (!gMicRunning) {
    WARN_SYSTEMF("[MIC_CAPTURE] Mic not enabled - returning NULL");
    if (outLen) *outLen = 0;
    return nullptr;
  }

  size_t bufferSize = sampleCount * sizeof(int16_t);
  WARN_SYSTEMF("[MIC_CAPTURE] Allocating %u bytes for %u samples...", (unsigned)bufferSize, (unsigned)sampleCount);
  WARN_SYSTEMF("[MIC_CAPTURE] Heap before alloc: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  
  int16_t* buffer = (int16_t*)ps_alloc(bufferSize, AllocPref::PreferPSRAM, "mic.samples");
  WARN_SYSTEMF("[MIC_CAPTURE] ps_alloc returned: %p", buffer);
  
  if (!buffer) {
    WARN_SYSTEMF("[MIC_CAPTURE] *** ALLOCATION FAILED! ***");
    INFO_MIC_POLLINGF("Failed to allocate %u bytes", bufferSize);
    if (outLen) *outLen = 0;
    return nullptr;
  }

  WARN_SYSTEMF("[MIC_CAPTURE] Reading %u bytes via HAL_Audio...", (unsigned)bufferSize);
  unsigned long startMs = millis();
  size_t bytesRead = 0;
  esp_err_t err = ESP_OK;
  {
    // Read PCM via HAL_Audio (single I2S owner). A generous finite timeout
    // replaces the old portMAX_DELAY block; at 16 kHz a full read fills in ms.
    size_t got = audioReadPcm(buffer, bufferSize / sizeof(int16_t), 5000);
    bytesRead = got * sizeof(int16_t);
    if (got == 0) err = ESP_FAIL;
  }
  unsigned long elapsed = millis() - startMs;
  
  WARN_SYSTEMF("[MIC_CAPTURE] i2s_channel_read returned 0x%x (%s) in %lu ms, bytesRead=%u", 
               err, esp_err_to_name(err), elapsed, (unsigned)bytesRead);
  
  if (err != ESP_OK) {
    WARN_SYSTEMF("[MIC_CAPTURE] *** I2S READ FAILED! ***");
    INFO_MIC_POLLINGF("Failed to read samples: 0x%x", err);
    free(buffer);
    if (outLen) *outLen = 0;
    return nullptr;
  }

  micProcessForSource(buffer, bytesRead / sizeof(int16_t));

  // Log sample statistics
  if (bytesRead >= 4) {
    int16_t minVal = buffer[0], maxVal = buffer[0];
    int64_t sumAbs = 0;
    size_t numSamples = bytesRead / sizeof(int16_t);
    for (size_t i = 0; i < numSamples; i++) {
      if (buffer[i] < minVal) minVal = buffer[i];
      if (buffer[i] > maxVal) maxVal = buffer[i];
      sumAbs += (buffer[i] < 0) ? -buffer[i] : buffer[i];
    }
    float avgAbs = (float)sumAbs / (float)numSamples;
    WARN_SYSTEMF("[MIC_CAPTURE] Sample stats: min=%d, max=%d, range=%d, avg_abs=%.1f", 
                 minVal, maxVal, maxVal - minVal, avgAbs);
  }

  if (outLen) *outLen = bytesRead;
  WARN_SYSTEMF("[MIC_CAPTURE] Returning buffer=%p, len=%u", buffer, (unsigned)bytesRead);
  return buffer;
}

int getAudioLevel() {
  static uint32_t callCount = 0;
  callCount++;

  // Only log every 50th call to avoid spam
  bool shouldLog = (callCount % 50 == 1);

  if (shouldLog) {
    DEBUG_MIC_VALUESF("[MIC_LEVEL] getAudioLevel() call #%lu, gMicRunning=%d", callCount, gMicRunning);
  }

  // The VU meter is the most-frequent caller — piggyback the HAL reconcile here
  // so a G2 mid-session disconnect flips the mic off within one poll.
  micReconcileState();

  if (!gMicRunning) {
    if (shouldLog) DEBUG_MIC_VALUESF("[MIC_LEVEL] Mic not enabled - returning 0");
    return 0;
  }

  uint32_t now = millis();
  if (micRecordingBusy()) {
    return lastAudioLevel;
  }
  if (lastAudioLevelMs != 0 && (now - lastAudioLevelMs) < 150) {
    return lastAudioLevel;
  }

  // Read a small sample to calculate level
  int16_t samples[256];
  size_t bytesRead = 0;

  bool took = false;
  if (i2sMicMutex && xSemaphoreTake(i2sMicMutex, 0) == pdTRUE) {
    took = true;
  } else {
    if (shouldLog) {
      DEBUG_MIC_VALUESF("[MIC_LEVEL] i2sMicMutex busy; returning cached last=%d", lastAudioLevel);
    }
    return lastAudioLevel;
  }

  size_t gotSamples = audioReadPcm(samples, sizeof(samples) / sizeof(int16_t), 50);
  bytesRead = gotSamples * sizeof(int16_t);
  esp_err_t err = (gotSamples > 0) ? ESP_OK : ESP_FAIL;

  if (took && i2sMicMutex) {
    xSemaphoreGive(i2sMicMutex);
  }
  
  if (err != ESP_OK || bytesRead == 0) {
    if (shouldLog) {
      DEBUG_MIC_VALUESF("[MIC_LEVEL] i2s_channel_read failed or no data: err=0x%x bytesRead=%u, returning last=%d",
                 err, bytesRead, lastAudioLevel);
    }
    return lastAudioLevel;
  }

  size_t sampleCount = bytesRead / sizeof(int16_t);
  micProcessForSource(samples, sampleCount);

  // Calculate RMS level
  int32_t sum = 0;
  int16_t minVal = samples[0], maxVal = samples[0];
  
  for (size_t i = 0; i < sampleCount; i++) {
    sum += abs(samples[i]);
    if (samples[i] < minVal) minVal = samples[i];
    if (samples[i] > maxVal) maxVal = samples[i];
  }
  int32_t avg = sum / sampleCount;
  
  // Map to 0-100 range
  int level = map(avg, 0, 16384, 0, 100);
  level = constrain(level, 0, 100);
  
  if (shouldLog) {
    DEBUG_MIC_VALUESF("[MIC_LEVEL] samples=%u avg=%ld min=%d max=%d level=%d%%",
               sampleCount, avg, minVal, maxVal, level);
  }
  
  lastAudioLevel = level;
  lastAudioLevelMs = now;
  return level;
}

const char* buildMicrophoneStatusJson() {
  const MicRecordingState recState = getMicRecordingState();
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
    "{\"enabled\":%s,\"connected\":%s,\"recording\":%s,"
    "\"recordingState\":\"%s\","
    "\"source\":\"%s\",\"pdmAvailable\":%s,\"g2Available\":%s,"
    "\"sampleRate\":%u,\"bitDepth\":16,\"channels\":%d,\"level\":%d}",
    gMicRunning ? "true" : "false",
    micConnected ? "true" : "false",
    recState != MicRecordingState::IDLE ? "true" : "false",
    micRecordingStateName(recState),
    micSourceName(),
    audioSourceAvailable(AUDIO_SRC_LOCAL_PDM) ? "true" : "false",
    audioSourceAvailable(AUDIO_SRC_G2_LEFT) ? "true" : "false",
    (unsigned)micEffectiveSampleRate(), micChannels,
    gMicRunning ? getAudioLevel() : 0
  );
  return gMicCmdBuffer;
}

// ============================================================================
// CLI Commands
// ============================================================================

const char* cmd_mic(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const MicRecordingState recState = getMicRecordingState();
  if (argWantsJson(argsInput)) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
      "{\"schema\":1,\"enabled\":%s,\"connected\":%s,\"recording\":%s,"
      "\"recordingState\":\"%s\","
      "\"source\":\"%s\",\"pdmAvailable\":%s,\"g2Available\":%s,"
      "\"sampleRate\":%u,\"bitDepth\":16,\"channels\":%d,\"level\":%d}",
      gMicRunning ? "true" : "false", micConnected ? "true" : "false",
      recState != MicRecordingState::IDLE ? "true" : "false",
      micRecordingStateName(recState),
      micSourceName(),
      audioSourceAvailable(AUDIO_SRC_LOCAL_PDM) ? "true" : "false",
      audioSourceAvailable(AUDIO_SRC_G2_LEFT) ? "true" : "false",
      (unsigned)micEffectiveSampleRate(), micChannels, gMicRunning ? getAudioLevel() : 0);
    return gMicCmdBuffer;
  }
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
    "Microphone Status:\n"
    "  Enabled: %s\n"
    "  Connected: %s\n"
    "  Recording: %s (%s)\n"
    "  Source: %s (pdm:%s g2:%s)\n"
    "  Sample Rate: %u Hz\n"
    "  Bit Depth: 16\n"
    "  Channels: %d\n"
    "  Level: %d%%",
    gMicRunning ? "yes" : "no",
    micConnected ? "yes" : "no",
    recState != MicRecordingState::IDLE ? "yes" : "no",
    micRecordingStateName(recState),
    micSourceName(),
    audioSourceAvailable(AUDIO_SRC_LOCAL_PDM) ? "yes" : "no",
    audioSourceAvailable(AUDIO_SRC_G2_LEFT) ? "yes" : "no",
    (unsigned)micEffectiveSampleRate(), micChannels,
    gMicRunning ? getAudioLevel() : 0
  );
  return gMicCmdBuffer;
}

const char* cmd_micstart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const AuthContext& admissionCtx = currentAuthContext();
  const CommandContext* commandCtx =
      static_cast<const CommandContext*>(currentCommandContext());
  const bool syntheticEvenAi =
      admissionCtx.transport == SOURCE_INTERNAL &&
      admissionCtx.user == "uart-session" &&
      admissionCtx.path == "/g2evenai";
  if (syntheticEvenAi) {
    if (!commandCtx ||
        !(commandCtx->behaviorFlags &
          COMMAND_CONTEXT_REQUIRE_G2_EVENAI_AUTHORITY) ||
        !g2EvenAiExchangeBoundToUartSession(
            commandCtx->authorityId,
            commandCtx->authoritySessionEpoch)) {
      return "Error: G2 EvenAI UART authority changed before microphone start.";
    }
  }
  if (!gSettings.micEnabled) {
    return "ERROR: Microphone is disabled - run 'micenabled 1' first";
  }
  if (initMicrophone()) {
    return "Microphone started successfully";
  }
  return "Error: Failed to start microphone";
}

const char* cmd_micstop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!stopMicrophone()) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
             "Error: Microphone source stopped but recording is still %s",
             micRecordingStateName(getMicRecordingState()));
    return gMicCmdBuffer;
  }
  return "Microphone stopped";
}

const char* cmd_miclevel(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argWantsJson(argsInput)) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "{\"schema\":1,\"enabled\":%s,\"level\":%d}",
      gMicRunning ? "true" : "false", gMicRunning ? getAudioLevel() : 0);
    return gMicCmdBuffer;
  }
  if (!gMicRunning) {
    return "Error: Microphone not enabled";
  }
  int level = getAudioLevel();
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Audio level: %d%%", level);
  return gMicCmdBuffer;
}

static bool micParseOwnerId(const String& input, MicRecordingOwner* out) {
  if (!out || input.length() != 16) return false;
  MicRecordingOwner value = 0;
  for (size_t i = 0; i < 16; ++i) {
    const char c = input.charAt(i);
    uint8_t nibble = 0;
    if (c >= '0' && c <= '9') nibble = (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f') nibble = (uint8_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') nibble = (uint8_t)(c - 'A' + 10);
    else return false;
    value = (value << 4) | nibble;
  }
  if (!micRecordingOwnerValid(value)) return false;
  *out = value;
  return true;
}

static bool micParseDecimalMs(const String& input, uint32_t* out) {
  if (!out || input.length() == 0) return false;
  uint32_t value = 0;
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input.charAt(i);
    if (c < '0' || c > '9') return false;
    value = value * 10u + (uint32_t)(c - '0');
    if (value > 10000u) return false;
  }
  if (value < 200u) return false;
  *out = value;
  return true;
}

// Strict machine grammar: empty, `vad`, `vad trim`, `vad <200..10000>`, or
// `vad <200..10000> trim`. Unlike the legacy human parser this rejects every
// unknown/partial token rather than silently falling back to 1200 ms.
static bool micParseOwnedRecordOptions(String options,
                                       uint32_t* silenceMs, bool* trim) {
  if (!silenceMs || !trim) return false;
  *silenceMs = 0;
  *trim = false;
  options.trim();
  options.toLowerCase();
  if (options.length() == 0) return true;
  if (options == "vad") {
    *silenceMs = 1200;
    return true;
  }
  if (!options.startsWith("vad ")) return false;
  options.remove(0, 4);
  options.trim();
  if (options == "trim") {
    *silenceMs = 1200;
    *trim = true;
    return true;
  }
  const int sp = options.indexOf(' ');
  const String msToken = (sp < 0) ? options : options.substring(0, sp);
  if (!micParseDecimalMs(msToken, silenceMs)) return false;
  if (sp < 0) return true;
  String tail = options.substring(sp + 1);
  tail.trim();
  if (tail != "trim") return false;
  *trim = true;
  return true;
}

static const char* micOwnedOpError(MicRecordingOwnedOp op) {
  switch (op) {
    case MicRecordingOwnedOp::INVALID_OWNER:
      return "Error: recording ID must be 16 hex digits with two nonzero 8-digit halves";
    case MicRecordingOwnedOp::OWNER_MISMATCH:
      return "Error: another owner currently controls the recording";
    case MicRecordingOwnedOp::NOT_READY:
      return "Error: recording is not finalized yet";
    case MicRecordingOwnedOp::NOT_FOUND:
      return "Error: recording ID not found";
    case MicRecordingOwnedOp::TIMED_OUT:
      return "Error: recording finalization timed out";
    case MicRecordingOwnedOp::DELETE_FAILED:
      return "Error: recording finalized but exact-path discard failed";
    case MicRecordingOwnedOp::PATH_MISMATCH:
      return "Error: filename does not belong to recording ID";
    case MicRecordingOwnedOp::OK:
      break;
  }
  return "Error: recording owner operation failed";
}

const char* cmd_micrecord(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    const MicRecordingState state = getMicRecordingState();
    if (state == MicRecordingState::CAPTURING) {
      uint32_t elapsed = (millis() - recordingStartTime) / 1000;
      snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Recording: active (%lus, %lu samples)", 
        elapsed, recordingSamples);
    } else if (state != MicRecordingState::IDLE) {
      snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Recording: %s",
               micRecordingStateName(state));
    } else {
      const MicRecordingResult last = micRecordingLastResult();
      if (last.valid && last.failed) {
      snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Recording: stopped (failed: %s)",
                 last.failure);
      } else {
        snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Recording: stopped");
      }
    }
    return gMicCmdBuffer;
  }

  String a = arg; a.toLowerCase();
  if (a.startsWith("startid ")) {
    String rest = arg.substring(8); rest.trim();
    const int sp = rest.indexOf(' ');
    const String idToken = (sp < 0) ? rest : rest.substring(0, sp);
    String options = (sp < 0) ? String("") : rest.substring(sp + 1);
    MicRecordingOwner owner = MIC_RECORDING_OWNER_MANUAL;
    uint32_t silenceMs = 0;
    bool trim = false;
    if (!micParseOwnerId(idToken, &owner)) {
      return "Error: usage: micrecord startid <16hex> [vad <200..10000>] [trim]";
    }
    if (!micParseOwnedRecordOptions(options, &silenceMs, &trim)) {
      return "Error: usage: micrecord startid <16hex> [vad <200..10000>] [trim]";
    }
    if (!gMicRunning) {
      return "Error: Microphone not enabled. Use 'openmic' first.";
    }
    const AuthContext& admissionCtx = currentAuthContext();
    const CommandContext* commandCtx =
        static_cast<const CommandContext*>(currentCommandContext());
    const bool syntheticEvenAi =
        admissionCtx.transport == SOURCE_INTERNAL &&
        admissionCtx.user == "uart-session" &&
        admissionCtx.path == "/g2evenai";
    if (syntheticEvenAi) {
      if (!commandCtx ||
          !(commandCtx->behaviorFlags &
            COMMAND_CONTEXT_REQUIRE_G2_EVENAI_AUTHORITY)) {
        return "Error: G2 EvenAI recording start lacks bound UART authority.";
      }
      if (commandCtx->authorityId != owner ||
          !g2EvenAiExchangeBoundToUartSession(
              commandCtx->authorityId,
              commandCtx->authoritySessionEpoch)) {
        return "Error: G2 EvenAI UART authority changed before recording start.";
      }
    }
    const uint32_t currentSessionEpoch = uartLinkSessionEpoch();
    uint32_t admissionSessionEpoch = 0;
    if (currentSessionEpoch != 0) {
      if (admissionCtx.transport == SOURCE_UART) {
        admissionSessionEpoch = currentSessionEpoch;
      } else if (admissionCtx.transport == SOURCE_INTERNAL &&
                 admissionCtx.path == "/g2evenai" &&
                 g2EvenAiExchangeBoundToUartSession(
                     owner, currentSessionEpoch)) {
        admissionSessionEpoch = currentSessionEpoch;
      }
    }
    // Consume a one-shot, controller+exchange+login-epoch provenance arm in
    // command context, then latch only the resulting bool into the recorder
    // FSM. Fabricated startid commands remain valid batch recordings but can
    // never become live shadow producers.
    LiveAudioRecorderAuthorization shadowAuth{};
    (void)liveAudioRecorderCaptureEligible(owner, &shadowAuth);
    if (!startRecordingInternal(owner, silenceMs, trim,
                                admissionSessionEpoch, shadowAuth)) {
      return "Error: Failed to start owned recording (busy or ID already consumed)";
    }
    char ownerHex[17];
    micRecordingOwnerHex(owner, ownerHex);
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
             "Recording %s started%s%s", ownerHex,
             silenceMs ? " (auto-stop on silence" : "",
             silenceMs ? (trim ? ", trimmed)" : ")") : "");
    return gMicCmdBuffer;
  }
  if (a.startsWith("statusid ")) {
    String idToken = arg.substring(9); idToken.trim();
    MicRecordingOwner owner = MIC_RECORDING_OWNER_MANUAL;
    if (!micParseOwnerId(idToken, &owner)) {
      return "Error: usage: micrecord statusid <16hex>";
    }
    MicRecordingResult result{};
    const MicRecordingOwnedOp op = getRecordingResultOwned(owner, &result);
    char ownerHex[17];
    micRecordingOwnerHex(owner, ownerHex);
    if (op == MicRecordingOwnedOp::NOT_READY) {
      snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
               "Recording %s: %s", ownerHex,
               micRecordingStateName(getMicRecordingState()));
      return gMicCmdBuffer;
    }
    if (op != MicRecordingOwnedOp::OK) return micOwnedOpError(op);
    if (result.discarded) {
      snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
               "Recording %s: discarded", ownerHex);
    } else if (result.failed) {
      snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
               "Error: Recording %s failed: %s", ownerHex, result.failure);
    } else {
      snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
               "Recording %s: stopped — %s%s", ownerHex, result.path,
               micDegradedSuffixFor(result));
    }
    return gMicCmdBuffer;
  }
  if (a.startsWith("stopid ")) {
    String rest = arg.substring(7); rest.trim();
    const int sp = rest.indexOf(' ');
    const String idToken = (sp < 0) ? rest : rest.substring(0, sp);
    String option = (sp < 0) ? String("") : rest.substring(sp + 1);
    option.trim(); option.toLowerCase();
    const bool discard = option == "discard";
    if (option.length() && !discard) {
      return "Error: usage: micrecord stopid <16hex> [discard]";
    }
    MicRecordingOwner owner = MIC_RECORDING_OWNER_MANUAL;
    if (!micParseOwnerId(idToken, &owner)) {
      return "Error: usage: micrecord stopid <16hex> [discard]";
    }
    const MicRecordingOwnedOp op = stopRecordingOwned(owner, discard);
    if (op != MicRecordingOwnedOp::OK) return micOwnedOpError(op);
    MicRecordingResult result{};
    const MicRecordingOwnedOp resultOp =
        getRecordingResultOwned(owner, &result);
    if (resultOp != MicRecordingOwnedOp::OK) return micOwnedOpError(resultOp);
    char ownerHex[17];
    micRecordingOwnerHex(owner, ownerHex);
    if (result.discarded) {
      snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
               "Recording %s discarded", ownerHex);
    } else if (result.failed) {
      snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
               "Error: Recording %s failed: %s", ownerHex, result.failure);
    } else {
      snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
               "Recording %s stopped — %s%s", ownerHex, result.path,
               micDegradedSuffixFor(result));
    }
    return gMicCmdBuffer;
  }
  if (arg == "1" || a == "start" || a.startsWith("start ")) {
    if (!gMicRunning) {
      return "Error: Microphone not enabled. Use 'openmic' first.";
    }
    // `micrecord start`                 → fixed window (unchanged; every other caller)
    // `micrecord start vad [ms]`        → opt-in: auto-stop after `ms` of trailing
    //                                     silence (default 1200), for the CM5/STT flow.
    // `micrecord start vad [ms] trim`   → additionally drop the recorded trailing
    //                                     silence from the file. Detection window is
    //                                     unchanged; a bare `vad` still yields a
    //                                     byte-exact WAV for tools/vad_replay.py.
    uint32_t silenceMs = 0;
    bool trim = false;
    int vadIdx = a.indexOf("vad");
    if (vadIdx >= 0) {
      silenceMs = 1200;
      String rest = a.substring(vadIdx + 3); rest.trim();
      int trimIdx = rest.indexOf("trim");
      if (trimIdx >= 0) { trim = true; rest = rest.substring(0, trimIdx); rest.trim(); }
      if (rest.length()) { long v = rest.toInt(); if (v >= 200 && v <= 10000) silenceMs = (uint32_t)v; }
    }
    if (startRecording(silenceMs, trim)) {
      return silenceMs ? (trim ? "Recording started (auto-stop on silence, trimmed)"
                              : "Recording started (auto-stop on silence)")
                       : "Recording started";
    } else {
      return "Error: Failed to start recording";
    }
  } else if (arg == "0" || arg.equalsIgnoreCase("stop")) {
    bool wasRecording = micRecordingBusy();
    if (!stopRecording()) {
      snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
               "Error: Recording still %s; WAV is not fetchable",
               micRecordingStateName(getMicRecordingState()));
      return gMicCmdBuffer;
    }
    const MicRecordingResult last = micRecordingLastResult();
    if (last.valid && last.failed) {
      snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Error: Recording failed: %s",
               last.failure);
      return gMicCmdBuffer;
    }
    if (!wasRecording) {
      // It may have already auto-stopped on silence — hand back the path so the
      // caller (CM5) can voicefetch it, exactly like an explicit stop would.
      if (last.valid && last.path[0]) {
        snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Recording stopped — %s%s",
                 last.path, micDegradedSuffixFor(last));
        return gMicCmdBuffer;
      }
      return "Recording stopped";
    }
    // Integer math for "N.Ns" (avoid float printf on newlib-nano).
    const uint32_t rate = last.sampleRate;
    uint32_t ms = (rate > 0)
                    ? (uint32_t)((uint64_t)last.samples * 1000 / rate)
                    : 0;
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Recording stopped — %s (%lu.%lus)%s",
             last.path, (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 100),
             micDegradedSuffixFor(last));
    return gMicCmdBuffer;
  }
  
  return "Error: invalid arguments — Usage: micrecord <start|stop|1|0|startid|statusid|stopid> ...";
}

const char* cmd_miclist(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["count"] = getRecordingCount();
    JsonArray arr = doc["recordings"].to<JsonArray>();
    String list = getRecordingsList();  // "name:size,name:size,..."
    int start = 0;
    while (start < (int)list.length()) {
      int comma = list.indexOf(',', start);
      String entry = (comma < 0) ? list.substring(start) : list.substring(start, comma);
      entry.trim();
      if (entry.length()) {
        int colon = entry.lastIndexOf(':');
        JsonObject o = arr.add<JsonObject>();
        if (colon > 0) { o["filename"] = entry.substring(0, colon); o["size"] = entry.substring(colon + 1).toInt(); }
        else           { o["filename"] = entry; }
      }
      if (comma < 0) break;
      start = comma + 1;
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

  int count = getRecordingCount();
  if (count == 0) {
    return "No recordings found";
  }

  String list = getRecordingsList();
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Recordings (%d):\n%s", count, list.c_str());
  return gMicCmdBuffer;
}

const char* cmd_micdelete(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (micRecordingBusy()) {
    return "Error: Recording is still active/finalizing";
  }
  
  // `micdelete all` (bare keyword) wipes recordings; otherwise the filename is a
  // quoted token (uniform quoted-path rule).
  CommandArgs a(argsInput);
  bool isAll = (a.has(0) && !a.argWasQuoted(0) && a.arg(0).equalsIgnoreCase("all"));
  String arg;
  if (!isAll) {
    const char* qerr = requireQuotedToken(a, 0, arg);
    if (qerr) return qerr;
    if (a.has(1)) return "Error: unexpected argument — micdelete \"<filename>\" or micdelete all";
  }

  if (isAll) {
    auto wipeWavs = [](const char* folder) -> int {
      int deleted = 0;
      if (!VFS::existsGuarded(folder, currentAuthContext())) return 0;
      File dir = VFS::openGuarded(String(folder), "r", currentAuthContext());
      if (!dir || !dir.isDirectory()) return 0;
      File f = dir.openNextFile();
      while (f) {
        String name = f.name();
        bool isWav = name.endsWith(".wav");
        f.close();
        if (isWav) {
          String path = String(folder) + "/" + name;
          if (VFS::removeGuarded(path, currentAuthContext())) deleted++;
        }
        f = dir.openNextFile();
      }
      return deleted;
    };
    int deleted = wipeWavs(kMicRecSD) + wipeWavs(kMicRecLittleFS);
    if (deleted == 0) {
      return "No recordings found";
    }
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Deleted %d recording(s)", deleted);
    return gMicCmdBuffer;
  }
  
  if (deleteRecording(arg.c_str())) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Deleted: %s", arg.c_str());
    return gMicCmdBuffer;
  }
  return "Error: File not found";
}

const char* cmd_micdeleteid(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs args(argsInput);
  if (args.count() != 2 || args.argWasQuoted(0)) {
    return "Error: usage: micdeleteid <16hex> \"<filename>\"";
  }
  MicRecordingOwner owner = MIC_RECORDING_OWNER_MANUAL;
  if (!micParseOwnerId(args.arg(0), &owner)) {
    return "Error: usage: micdeleteid <16hex> \"<filename>\"";
  }
  String filename;
  const char* quoteError = requireQuotedToken(args, 1, filename);
  if (quoteError) return quoteError;

  const MicRecordingOwnedOp op =
      deleteRecordingOwned(owner, filename.c_str());
  if (op != MicRecordingOwnedOp::OK) return micOwnedOpError(op);

  char ownerHex[17];
  micRecordingOwnerHex(owner, ownerHex);
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
           "Deleted recording %s: %s", ownerHex, filename.c_str());
  return gMicCmdBuffer;
}

const char* cmd_micsamplerate(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Sample rate: %d Hz", micSampleRate);
    return gMicCmdBuffer;
  }

  int rate = arg.toInt();
  if (rate < 8000 || rate > 48000) {
    return "Error: Sample rate must be 8000-48000 Hz";
  }

  STACK_TRACEF("cmd_micsamplerate.enter requested=%d current=%d gMicRunning=%d",
               rate, micSampleRate, gMicRunning);

  // Need to reinitialize if already running
  bool wasEnabled = gMicRunning;
  if (wasEnabled || micRecordingBusy()) {
    STACK_TRACEF("cmd_micsamplerate.before_stop");
    if (!stopMicrophone()) {
      return "Error: Cannot change sample rate while recording is still finalizing";
    }
    STACK_TRACEF("cmd_micsamplerate.after_stop");
  }

  micSampleRate = rate;
  setSetting(gSettings.microphoneSampleRate, rate);
  STACK_TRACEF("cmd_micsamplerate.rate_saved_to_settings");

  if (wasEnabled) {
    STACK_TRACEF("cmd_micsamplerate.before_reinit");
    bool ok = initMicrophone();
    STACK_TRACEF("cmd_micsamplerate.after_reinit ok=%d gMicRunning=%d",
                 ok ? 1 : 0, gMicRunning);
    if (!ok) return "Error: Sample rate saved, but the microphone could not restart";
  }

  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Sample rate set to %d Hz (saved)%s", micSampleRate,
           audioGetSource() == AUDIO_SRC_G2_LEFT ? " (ignored while source=G2 — the glasses mic is fixed 16 kHz)" : "");
  return gMicCmdBuffer;
}

const char* cmd_micgain(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Gain: %d%%", micGain);
    return gMicCmdBuffer;
  }

  int gain = arg.toInt();
  if (gain < 0 || gain > 100) {
    return "Error: Gain must be 0-100%";
  }
  
  micGain = gain;
  setSetting(gSettings.microphoneGain, gain);
  
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Gain set to %d%% (saved)", micGain);
  return gMicCmdBuffer;
}

const char* cmd_micbitdepth(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer), "Bit depth: %d-bit", micBitDepth);
    return gMicCmdBuffer;
  }

  int depth = arg.toInt();
  if (depth != 16 && depth != 32) {
    return "Error: Bit depth must be 16 or 32";
  }

  STACK_TRACEF("cmd_micbitdepth.enter requested=%d current=%d gMicRunning=%d",
               depth, micBitDepth, gMicRunning);

  // Need to reinitialize if already running
  bool wasEnabled = gMicRunning;
  if (wasEnabled || micRecordingBusy()) {
    STACK_TRACEF("cmd_micbitdepth.before_stop");
    if (!stopMicrophone()) {
      return "Error: Cannot change bit depth while recording is still finalizing";
    }
    STACK_TRACEF("cmd_micbitdepth.after_stop");
  }

  micBitDepth = depth;
  setSetting(gSettings.microphoneBitDepth, depth);
  STACK_TRACEF("cmd_micbitdepth.depth_saved_to_settings");

  if (wasEnabled) {
    STACK_TRACEF("cmd_micbitdepth.before_reinit");
    bool ok = initMicrophone();
    STACK_TRACEF("cmd_micbitdepth.after_reinit ok=%d gMicRunning=%d",
                 ok ? 1 : 0, gMicRunning);
    if (!ok) return "Error: Bit depth saved, but the microphone could not restart";
  }

  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
           "Bit depth set to %d-bit (saved) — note: WAV recordings are always 16-bit (HAL canonical format)", micBitDepth);
  return gMicCmdBuffer;
}

// Real-time audio visualizer state
static volatile bool gMicVisualizerRunning = false;
static TaskHandle_t gMicVisualizerTask = nullptr;

static void micVisualizerTaskFunc(void* param) {
  const size_t bufSize = 512;
  int16_t* samples = (int16_t*)heap_caps_malloc(bufSize * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!samples) {
    gMicVisualizerRunning = false;
    gMicVisualizerTask = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  
  Serial.println("\n=== AUDIO VISUALIZER (press any key to stop) ===");
  Serial.println("Level: [--------------------] Peak | Min/Max samples");
  
  while (gMicVisualizerRunning && gMicRunning) {
    size_t bytesRead = 0;
    size_t got = audioReadPcm(samples, bufSize, 100);
    bytesRead = got * sizeof(int16_t);
    esp_err_t err = (got > 0) ? ESP_OK : ESP_FAIL;
    
    if (err == ESP_OK && bytesRead > 0) {
      size_t sampleCount = bytesRead / sizeof(int16_t);
      micProcessForSource(samples, sampleCount);
      
      // Calculate stats
      int16_t minVal = 32767, maxVal = -32768;
      int64_t sumAbs = 0;
      for (size_t i = 0; i < sampleCount; i++) {
        if (samples[i] < minVal) minVal = samples[i];
        if (samples[i] > maxVal) maxVal = samples[i];
        sumAbs += abs(samples[i]);
      }
      int avgAbs = (int)(sumAbs / sampleCount);
      
      // Map to 0-100 scale (32767 = max amplitude)
      int level = (avgAbs * 100) / 32767;
      if (level > 100) level = 100;
      
      // Create ASCII bar (40 chars wide)
      char bar[45];
      int barLen = (level * 40) / 100;
      for (int i = 0; i < 40; i++) {
        if (i < barLen) {
          if (i < 20) bar[i] = '=';
          else if (i < 32) bar[i] = '#';
          else bar[i] = '!';  // Clipping warning
        } else {
          bar[i] = '-';
        }
      }
      bar[40] = '\0';
      
      // Print with carriage return to overwrite line
      Serial.printf("\r[%s] %3d%% | %6d / %6d", bar, level, minVal, maxVal);
    }
    
    // Check for key press to stop
    if (Serial.available()) {
      while (Serial.available()) Serial.read();
      break;
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));  // ~20 updates/sec
  }
  
  Serial.println("\n=== VISUALIZER STOPPED ===");
  heap_caps_free(samples);
  gMicVisualizerRunning = false;
  gMicVisualizerTask = nullptr;
  vTaskDelete(nullptr);
}

const char* cmd_micviz(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gMicRunning) {
    return "Error: Microphone not enabled. Use 'openmic' first.";
  }
  
  if (gMicVisualizerRunning) {
    gMicVisualizerRunning = false;
    return "Stopping visualizer...";
  }
  
  gMicVisualizerRunning = true;
  // No core affinity: NORMAL prio on Core 0 was getting preempted by WiFi/BT.
  // mic_record (Core 1, HIGH) owns the audio buffer; viz is a consumer that
  // can run wherever the scheduler has cycles.
  // APP_CORE: waveform-render compute; keep it off the Wi-Fi/BLE core.
  taskStackRecord("mic_viz", MIC_VIZ_STACK_WORDS);
  xTaskCreatePinnedToCore(micVisualizerTaskFunc, "mic_viz", MIC_VIZ_STACK_WORDS, nullptr, TASK_PRIORITY_NORMAL, &gMicVisualizerTask, APP_CORE);
  return "Visualizer started (press any key to stop)";
}

const char* cmd_micautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    return gSettings.micAutoStart ? "[Mic] Auto-start: enabled" : "[Mic] Auto-start: disabled";
  }
  arg.toLowerCase();
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.micAutoStart, true);
    return "[Mic] Auto-start enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.micAutoStart, false);
    return "[Mic] Auto-start disabled";
  }
  return "Error: invalid arguments — Usage: micautostart [on|off]";
}

// Get/set the persisted mic-source PREFERENCE {auto,pdm,g2}. The preference is
// resolved lazily against runtime availability at capture start — 'g2' is a
// valid preference even with no glasses connected (it applies when they do). If
// the mic is running, apply the switch live (stop → set → start) since
// audioSetSource refuses a source change mid-capture.
const char* cmd_micsource(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim(); arg.toLowerCase();
  if (arg.length() == 0) {
    snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
      "Mic source: preference=%s, active=%s (available: pdm=%s g2=%s). Set with: micsource <auto|pdm|g2>",
      gSettings.micSource.c_str(), micSourceName(),
      audioSourceAvailable(AUDIO_SRC_LOCAL_PDM) ? "yes" : "no",
      audioSourceAvailable(AUDIO_SRC_G2_LEFT) ? "yes" : "no");
    return gMicCmdBuffer;
  }
  if (arg != "auto" && arg != "pdm" && arg != "g2") {
    return "Error: invalid arguments — Usage: micsource <auto|pdm|g2>";
  }
  const bool wasEnabled = gMicRunning;
  if (wasEnabled || micRecordingBusy()) {
    if (!stopMicrophone()) {
      return "Error: Cannot change mic source while recording is still finalizing";
    }
  }
  setSetting(gSettings.micSource, arg);
  if (wasEnabled && !initMicrophone()) {
    return "Error: Mic source preference saved, but the new source could not start";
  }
  const char* note = "";
  if (arg == "g2" && !audioSourceAvailable(AUDIO_SRC_G2_LEFT)) {
    note = " (glasses not connected — applies when they connect)";
  } else if (arg == "pdm" && !audioSourceAvailable(AUDIO_SRC_LOCAL_PDM)) {
    note = " (no onboard PDM mic on this board)";
  }
  snprintf(gMicCmdBuffer, sizeof(gMicCmdBuffer),
           "Mic source preference set to '%s'%s (active=%s)", arg.c_str(), note, micSourceName());
  return gMicCmdBuffer;
}

// Command registry
// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry micCommands[] = {
  { "micread", "Read microphone sensor status.", false, cmd_mic, "Usage: micread [json]" },
  { "openmic", "Start microphone sensor.", false, cmd_micstart },
  { "closemic", "Stop microphone sensor.", false, cmd_micstop },
  { "miclevel", "Get current audio level.", false, cmd_miclevel, "Usage: miclevel [json]" },
  { "micviz", "Real-time audio level visualizer.", false, cmd_micviz, "Usage: micviz (press any key to stop)" },
  { "micrecord", "Start/stop recording to WAV file (bare = show recording status).", false, cmd_micrecord,
    "Usage: micrecord [start [vad <200..10000>] [trim] | stop | 1 | 0]\n"
    "       micrecord startid <16hex> [vad <200..10000>] [trim]\n"
    "       micrecord statusid <16hex>\n"
    "       micrecord stopid <16hex> [discard]" },
  { "miclist", "List saved recordings.", false, cmd_miclist, "Usage: miclist [json]" },
  { "micdelete", "Delete recording(s).", true, cmd_micdelete, "Usage: micdelete \"<filename>\" | micdelete all" },
  { "micdeleteid", "Delete one owner-correlated recording.", true, cmd_micdeleteid,
    "Usage: micdeleteid <16hex> \"<filename>\"" },
  // Lives in System_UartLink.cpp (it owns the port + frame writer); scoped to
  // the recordings dirs, UART-transport-only, hence non-admin (plan D2).
  { "voicefetch", "Stream a recording to the UART host as binary frames (CM5 bulk pull).", false, cmd_voicefetch,
    "Usage: voicefetch \"<path>\" - path must be under /recordings or /sd/recordings.\n"
    "Sends META+AUDIO frames on the UART link, then replies with byte/frame totals and crc16." },
  { "micsamplerate", "Get/set sample rate.", false, cmd_micsamplerate, "Usage: micsamplerate [8000-48000]" },
  { "micgain", "Get/set microphone gain.", false, cmd_micgain, "Usage: micgain [0-100]" },
  { "micbitdepth", "Get/set bit depth.", false, cmd_micbitdepth, "Usage: micbitdepth [16|32]" },
  { "micsource", "Get/set mic source: onboard PDM or G2 glasses.", false, cmd_micsource, "Usage: micsource [auto|pdm|g2]" },

  // Auto-start
  { "micautostart", "Enable/disable microphone auto-start after boot [on|off]", false, cmd_micautostart, "Usage: micautostart [on|off]" },
};

const size_t micCommandsCount = sizeof(micCommands) / sizeof(micCommands[0]);

// Settings module registration
// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry micSettingEntries[] = {
  { "micEnabled", SETTING_BOOL, &gSettings.micEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "micenabled" },
  { "microphoneAutoStart", SETTING_BOOL, &gSettings.micAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, "micautostart" },
  // Source preference {auto,pdm,g2}. Resolved lazily against availability.
  { "micSource", SETTING_STRING, &gSettings.micSource, 0, 0, "auto", 0, 0, "Mic source", "auto|Auto,pdm|Onboard PDM,g2|G2 glasses", false, nullptr, "micsource" },
  // These three were previously reported as "(saved)" but never registered, so
  // they silently reset to defaults on reboot. Registering them fixes that.
  { "microphoneSampleRate", SETTING_INT, &gSettings.microphoneSampleRate, 16000, 0, nullptr, 8000, 48000, "Sample rate (Hz, PDM only)", nullptr, false, nullptr, "micsamplerate" },
  { "microphoneGain", SETTING_INT, &gSettings.microphoneGain, 70, 0, nullptr, 0, 100, "Software gain (%)", nullptr, false, nullptr, "micgain" },
  // Only 16 and 32 are accepted (cmd_micbitdepth); the 16..32 min/max would
  // otherwise render as a number box offering values the command rejects.
  { "microphoneBitDepth", SETTING_INT, &gSettings.microphoneBitDepth, 16, 0, nullptr, 16, 32, "Bit depth (cosmetic; WAV is always 16-bit)", "16|16-bit,32|32-bit", false, nullptr, "micbitdepth" },
};

// Module "connected" = a mic source is actually available (onboard PDM present
// OR G2 glasses connected). Drives whether the settings module is shown/active.
static bool isMicConnected() {
  return audioAnySourceAvailable();
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule micSettingsModule = {
  "microphone",
  "hardware.sensors.microphone",
  micSettingEntries,
  sizeof(micSettingEntries) / sizeof(micSettingEntries[0]),
  isMicConnected,
  "Microphone (onboard PDM or G2 glasses)"
};

// Registration handled by gCommandModules[] in System_Utils.cpp

#endif // ENABLE_MICROPHONE
