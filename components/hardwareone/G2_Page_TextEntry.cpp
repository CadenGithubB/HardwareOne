// =============================================================================
// G2 glasses — generic text-entry overlay implementation
// =============================================================================
// See G2_Page_TextEntry.h for layout + contract.
//
// Compound architecture (the mic-detail / Health pattern): ONE page holding
//   • a static, event-capturing ListObject on the LEFT half — controls +
//     the current group's characters, and
//   • a display-only TextObject on the RIGHT half — live "<prompt>: <buf>_"
//     panel (eventCapture on a TextObject is firmware-rejected, so all taps
//     land on the list).
//
// Per-keystroke path: mutate the buffer, then Cmd=5 UPDATE_TEXT the panel
// ONLY (g2UpdateMixedTextChild — fire-and-forget, single fragment, no ack).
// The list child is never touched, so the firmware keeps its rows, focus
// and scroll: no per-keystroke REBUILD, no highlight reset to row 0, no
// whole-screen refresh. This replaces the old live-list-worker design whose
// full REBUILD every keystroke reset the native highlight and forced a
// re-scroll from the top for every character.
//
// Group switch is the ONE op that must change list rows: an in-place
// multi-child REBUILD (g2RelistMixedListText, ~80 ms, full child set
// re-sent) with a SHUTDOWN+CREATE swap fallback. Rare by design —
// bidirectional Grp rows replace the old forward-only cycle.
#include "G2_Page_TextEntry.h"
#include "G2_HijackCmd.h"   // g2BumpMenuGen() — bumped on text-entry enter/exit

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_Glasses.h"
#include "G2_RenderTypes.h"     // G2TextChildSpec + geometry presets
#include "System_Debug.h"

#include <stdio.h>
#include <string.h>

namespace {

// All session state. An exact coordinator token makes it routable while
// active; terminal reset revokes that token immediately but leaves this state
// intact until its FIFO tap-worker cleanup can safely free pad buffers. The
// framework accepts up to G2_TEXT_ENTRY_MAX_LEN bytes; individual fields
// retain their own smaller semantic/protocol limits.
struct TextEntryState {
  bool              active;
  uint32_t          serial;
  uint32_t          lifecycleEpoch;
  uint32_t          connectionGeneration;
  char              side;
  char              buf[G2_TEXT_ENTRY_MAX_LEN + 1];
  size_t            len;
  size_t            maxLen;
  char              prompt[32];
  TextEntryCommitFn onCommit;
  TextEntryCancelFn onCancel;
  TextEntryAbandonFn onAbandon; // cleanup-only caller staging scrub
  uint8_t           group;       // index into kGroupChars
  bool              isSecret;    // mask display; suppress logs/dictation
  bool              padMode;     // arrow-pad surface active (vs legacy list)
};

static TextEntryState sTe = {};

static void secureClear(char* buf, size_t cap) {
  if (!buf || cap == 0) return;
  volatile char* p = reinterpret_cast<volatile char*>(buf);
  while (cap--) *p++ = '\0';
}

// Local execution lease for the mutable page/pad state above. Text-entry can
// be started from g2_taps or from an off-tap session worker; terminal
// cleanup must never free the pad's shades/BMP while that startup is still
// allocating, CREATEing, or pushing. The coordinator token remains the
// routing/callback authority; this lease only serializes destructive local
// execution. Same-task nesting is allowed because pad handlers call back into
// g2TextEntryPadEvent and commit callbacks may synchronously open another
// entry. Cleanup itself is never admitted reentrantly.
static portMUX_TYPE sTextEntryExecMux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t sTextEntryExecOwner = nullptr;
static uint32_t     sTextEntryExecRootSerial = 0;
static uint16_t     sTextEntryExecDepth = 0;
static bool         sTextEntryExecCleanup = false;

static bool textEntryExecAcquire(uint32_t serial, bool cleanup) {
  if (!serial) return false;
  const TaskHandle_t task = xTaskGetCurrentTaskHandle();
  bool acquired = false;
  portENTER_CRITICAL(&sTextEntryExecMux);
  if (sTextEntryExecDepth == 0) {
    sTextEntryExecOwner = task;
    sTextEntryExecRootSerial = serial;
    sTextEntryExecDepth = 1;
    sTextEntryExecCleanup = cleanup;
    acquired = true;
  } else if (!cleanup && !sTextEntryExecCleanup &&
             sTextEntryExecOwner == task && sTextEntryExecDepth != UINT16_MAX) {
    // Normal same-task recursion only. A cleanup token never nests with page
    // work, even on g2_taps, so its exact teardown remains one operation.
    sTextEntryExecDepth++;
    acquired = true;
  }
  portEXIT_CRITICAL(&sTextEntryExecMux);
  return acquired;
}

static void textEntryExecRelease(bool cleanup) {
  const TaskHandle_t task = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&sTextEntryExecMux);
  if (sTextEntryExecDepth != 0 && sTextEntryExecOwner == task &&
      sTextEntryExecCleanup == cleanup) {
    if (--sTextEntryExecDepth == 0) {
      sTextEntryExecOwner = nullptr;
      sTextEntryExecRootSerial = 0;
      sTextEntryExecCleanup = false;
    }
  }
  portEXIT_CRITICAL(&sTextEntryExecMux);
}

class TextEntryExecGuard {
 public:
  TextEntryExecGuard(uint32_t serial, bool cleanup)
      : cleanup_(cleanup), held_(textEntryExecAcquire(serial, cleanup)) {}
  ~TextEntryExecGuard() {
    if (held_) textEntryExecRelease(cleanup_);
  }
  explicit operator bool() const { return held_; }
  TextEntryExecGuard(const TextEntryExecGuard&) = delete;
  TextEntryExecGuard& operator=(const TextEntryExecGuard&) = delete;

 private:
  bool cleanup_;
  bool held_;
};

static bool sessionCurrent(bool* secretOut = nullptr) {
  // Queries run on the control, tap, and session workers without taking the
  // execution lease. Snapshot every published identity field atomically: an
  // off-worker replacement may revoke/reseed the struct between fields, and
  // the coordinator's exact-token check turns any mixed snapshot into false.
  const bool active = __atomic_load_n(&sTe.active, __ATOMIC_ACQUIRE);
  if (!active) return false;
  const uint32_t serial = __atomic_load_n(&sTe.serial, __ATOMIC_ACQUIRE);
  const uint32_t lifecycleEpoch =
      __atomic_load_n(&sTe.lifecycleEpoch, __ATOMIC_ACQUIRE);
  const uint32_t connectionGeneration =
      __atomic_load_n(&sTe.connectionGeneration, __ATOMIC_ACQUIRE);
  const char side = __atomic_load_n(&sTe.side, __ATOMIC_ACQUIRE);
  const bool secret = secretOut
      ? __atomic_load_n(&sTe.isSecret, __ATOMIC_ACQUIRE) : false;
  if (!g2TextEntrySessionIsCurrent(
          serial, lifecycleEpoch, connectionGeneration, side)) {
    return false;
  }
  // `secret` was sampled before the exact coordinator check. If replacement
  // changed it after the identity loads, that replacement first revoked this
  // serial and the check above fails. If it starts afterward, this result is a
  // coherent snapshot of the old session at the validation point.
  if (secretOut) *secretOut = secret;
  return true;
}

static void clearLocalSession() {
  __atomic_store_n(&sTe.active, false, __ATOMIC_RELEASE);
  __atomic_store_n(&sTe.serial, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&sTe.lifecycleEpoch, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&sTe.connectionGeneration, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&sTe.side, '\0', __ATOMIC_RELEASE);
  sTe.len = 0;
  sTe.maxLen = 0;
  secureClear(sTe.buf, sizeof(sTe.buf));
  memset(sTe.prompt, 0, sizeof(sTe.prompt));
  sTe.onCommit = nullptr;
  sTe.onCancel = nullptr;
  sTe.onAbandon = nullptr;
  sTe.group = 0;
  __atomic_store_n(&sTe.isSecret, false, __ATOMIC_RELEASE);
  sTe.padMode = false;
}

// 13 chars per group keeps each row short (single-char) and the total
// row count under 20 — the firmware list caps near 20 items.
// The symbols group exists for WiFi passwords (real-world PSKs lean on
// !@#$ etc.); the double-quote is deliberately ABSENT from every group so
// text-entry output is always safe to wrap in a quoted CLI argument
// (CommandArgs has no escape support). Add more groups if a caller needs
// the remaining punctuation (:;'<>[]{}|~\/).
static const char* const kGroupChars[] = {
  "abcdefghijklm",
  "nopqrstuvwxyz",
  "ABCDEFGHIJKLM",
  "NOPQRSTUVWXYZ",
  "0123456789._-",
  "!@#$%^&*()+=?",
};
static const char* const kGroupName[] = {
  "a-m", "n-z", "A-M", "N-Z", "0-9 . _ -", "!@# sym",
};
static constexpr size_t kGroupCount =
    sizeof(kGroupChars) / sizeof(kGroupChars[0]);

// Row indices — fixed positions for the control rows. Char rows start at
// kRowFirstChar. The dispatcher trusts these positions.
//
// NOTE: unlike the old live-list design there is NO auto-prepended back
// row — the compound page owns every row, so row 0 is OUR Cancel row and
// the buffer no longer occupies a list row at all (it lives in the text
// panel). The freed row funds the bidirectional Grp pair that replaces
// the forward-only "Group:" cycle.
constexpr size_t kRowCancel    = 0;
constexpr size_t kRowSpace     = 1;
constexpr size_t kRowBackspace = 2;
constexpr size_t kRowDone      = 3;
constexpr size_t kRowGrpPrev   = 4;
constexpr size_t kRowGrpNext   = 5;
constexpr size_t kRowFirstChar = 6;
constexpr size_t kMaxRows      = kRowFirstChar + 13;  // 19 ≤ firmware ~20 cap

// Compound layout: chars left, live buffer panel right. Same split the
// Files page uses (list LEFT_HALF + text child on the right).
static constexpr G2ContainerGeom kKbListGeom = G2_GEOM_LEFT_HALF;
static constexpr G2ContainerGeom kKbBufGeom  = G2_GEOM_RIGHT_HALF;
static constexpr const char*     kKbBufName  = "kbBuf";
static constexpr uint32_t        kKbBufId    = 97;

// Row storage. Group-row labels name the group a tap switches TO; char
// rows are single-character labels from the active group. All pointers
// handed to the render calls are deep-copied by the senders, but these
// stay static anyway (live-label lifetime footguns are not worth relearning).
static const char* sRowPtrs[kMaxRows];
static char        sGrpPrevLabel[24];
static char        sGrpNextLabel[24];
static char        sCharRows[13][2];

static size_t buildRows() {
  sRowPtrs[kRowCancel]    = "X Cancel";
  sRowPtrs[kRowSpace]     = "Space";
  sRowPtrs[kRowBackspace] = "Backspace";
  sRowPtrs[kRowDone]      = "Done";
  snprintf(sGrpPrevLabel, sizeof(sGrpPrevLabel), "<- %s",
           kGroupName[(sTe.group + kGroupCount - 1) % kGroupCount]);
  snprintf(sGrpNextLabel, sizeof(sGrpNextLabel), "%s ->",
           kGroupName[(sTe.group + 1) % kGroupCount]);
  sRowPtrs[kRowGrpPrev] = sGrpPrevLabel;
  sRowPtrs[kRowGrpNext] = sGrpNextLabel;

  const char* chars = kGroupChars[sTe.group];
  size_t row = kRowFirstChar;
  for (size_t i = 0; chars[i] != '\0' && row < kMaxRows; i++, row++) {
    sCharRows[i][0] = chars[i];
    sCharRows[i][1] = '\0';
    sRowPtrs[row] = sCharRows[i];
  }
  return row;
}

// Buffer panel content. A two-line tail window of the authoritative buffer
// (up to G2_TEXT_ENTRY_MAX_LEN). The production pad's kbBuf pane is 288px —
// the same width as the key grid sitting under it — and the legacy
// RIGHT_HALF pane is 280px. G2_GEOM_LARGE (560px) holds G2_TEXT_DEFAULT_COLS
// (48), so both panes fit 24 columns. The previous 20-column wrap left a
// visible gap to the right of the typed text while the keyboard below
// continued to the pane edge. Cursor "_" trails so an empty buffer still
// shows the field is editable; "(n/max)" makes truncation/windowing
// explicit.
static void buildBufferText(char* out, size_t cap) {
  const char* prompt = sTe.prompt[0] ? sTe.prompt : "Input";
  constexpr size_t kWrap = 24;
  constexpr size_t kVisible = kWrap * 2;
  char l1[kWrap + 1];
  char l2[kWrap + 1];
  const size_t shown = sTe.len < kVisible ? sTe.len : kVisible;
  const size_t start = sTe.len - shown;
  const size_t n1 = shown < kWrap ? shown : kWrap;
  if (sTe.isSecret) memset(l1, '*', n1);
  else memcpy(l1, sTe.buf + start, n1);
  l1[n1] = '\0';
  const size_t n2 = shown > kWrap ? shown - kWrap : 0;
  if (sTe.isSecret) memset(l2, '*', n2);
  else memcpy(l2, sTe.buf + start + n1, n2);
  l2[n2] = '\0';
  if (n2 > 0) {
    snprintf(out, cap, "%s:\n%s\n%s_\n(%u/%u)", prompt, l1, l2,
             (unsigned)sTe.len, (unsigned)sTe.maxLen);
  } else {
    snprintf(out, cap, "%s:\n%s_\n(%u/%u)", prompt, l1,
             (unsigned)sTe.len, (unsigned)sTe.maxLen);
  }
}

// Full compound render (SHUTDOWN+CREATE swap). Session entry + the
// fallback when the in-place relist fails.
static bool showCompound() {
  const size_t n = buildRows();
  char bufText[96];
  buildBufferText(bufText, sizeof(bufText));
  const G2TextChildSpec spec = { kKbBufName, bufText, kKbBufId, kKbBufGeom,
                                 /*eventCapture=*/false };
  return g2ShowMixedListText(sRowPtrs, n, kKbListGeom, spec);
}

// Per-keystroke buffer refresh: UPDATE_TEXT the panel ONLY. The list is
// untouched — its highlight/scroll persist and nothing else repaints.
static void pushBuffer() {
  char bufText[96];
  buildBufferText(bufText, sizeof(bufText));
  // Keep the production pad's recreate snapshot current. If a live four-band
  // push later fails, its automatic one-tile fallback must recreate the text
  // sibling with the exact buffer currently owned here.
  if (sTe.padMode) g2KbdPadNoteBufferText(bufText);
  if (!g2UpdateMixedTextChild(kKbBufName, kKbBufId, bufText)) {
    // Non-fatal: buffer state is authoritative on our side; the next
    // successful push (or relist/commit) catches the panel up.
    DEBUG_G2F("[G2] text-entry: buffer UPDATE_TEXT failed");
  }
}

// Group switch: the one op that must change list rows. In-place multi-child
// REBUILD first (~80 ms; resets highlight to row 0 — acceptable for a rare
// explicit mode change); full swap as the always-lands fallback.
static void relistGroup() {
  const size_t n = buildRows();
  char bufText[96];
  buildBufferText(bufText, sizeof(bufText));
  const G2TextChildSpec spec = { kKbBufName, bufText, kKbBufId, kKbBufGeom,
                                 /*eventCapture=*/false };
  if (!g2RelistMixedListText(sRowPtrs, n, kKbListGeom, spec)) {
    DEBUG_G2F("[G2] text-entry: relist failed — falling back to full swap");
    showCompound();
  }
}

static void appendChar(char c) {
  if (sTe.len >= sTe.maxLen) return;
  sTe.buf[sTe.len++] = c;
  sTe.buf[sTe.len]   = '\0';
}

// End the session, snapshot the buffer, then run the caller's commit.
// Snapshot first so the callback sees a stable string even if it
// triggers another text-entry session synchronously.
static void finishCommit(uint32_t serial) {
  if (!g2TextEntrySessionFinishClaim(
          serial, sTe.lifecycleEpoch, sTe.connectionGeneration, sTe.side)) {
    return;  // terminal tombstone won; FIFO cleanup owns buffers/staging scrub
  }
  if (sTe.padMode) { g2KbdPadEnd(); sTe.padMode = false; }
  char snap[sizeof(sTe.buf)];
  strlcpy(snap, sTe.buf, sizeof(snap));
  const bool secret = sTe.isSecret;
  TextEntryCommitFn cb = sTe.onCommit;
  __atomic_store_n(&sTe.active, false, __ATOMIC_RELEASE);
  sTe.onCommit = nullptr;
  sTe.onCancel = nullptr;
  sTe.onAbandon = nullptr;
  if (secret) secureClear(sTe.buf, sizeof(sTe.buf));
  g2BumpMenuGen();   // exiting text entry invalidates view snapshots
  if (cb) cb(snap);
  if (secret) secureClear(snap, sizeof(snap));
}

static void finishCancel(uint32_t serial) {
  if (!g2TextEntrySessionFinishClaim(
          serial, sTe.lifecycleEpoch, sTe.connectionGeneration, sTe.side)) {
    return;  // terminal tombstone won; FIFO cleanup owns buffers/staging scrub
  }
  if (sTe.padMode) { g2KbdPadEnd(); sTe.padMode = false; }
  const bool secret = sTe.isSecret;
  TextEntryCancelFn cb = sTe.onCancel;
  __atomic_store_n(&sTe.active, false, __ATOMIC_RELEASE);
  sTe.onCommit = nullptr;
  sTe.onCancel = nullptr;
  sTe.onAbandon = nullptr;
  if (secret) secureClear(sTe.buf, sizeof(sTe.buf));
  g2BumpMenuGen();   // exiting text entry invalidates view snapshots
  if (cb) cb();
}

}  // namespace

bool g2TextEntryIsActive() { return sessionCurrent(); }

bool g2TextEntryIsSecret() {
  bool secret = false;
  return sessionCurrent(&secret) && secret;
}

bool g2BeginTextEntry(const TextEntryConfig& cfg) {
  if (!cfg.onCommit) return false;
  if (cfg.maxLen == 0 || cfg.maxLen > G2_TEXT_ENTRY_MAX_LEN) return false;
  const uint32_t serial = g2TextEntrySessionAllocateSerial();
  if (!serial) return false;
  TextEntryExecGuard execution(serial, /*cleanup*/ false);
  if (!execution) {
    DEBUG_G2F("[G2] text-entry: startup deferred; local state is busy");
    return false;
  }

  uint32_t lifecycleEpoch = 0;
  uint32_t connectionGeneration = 0;
  char side = 0;
  if (!g2TextEntryCaptureAdmissionFence(
          &lifecycleEpoch, &connectionGeneration, &side) ||
      (cfg.expectedLifecycleEpoch &&
       cfg.expectedLifecycleEpoch != lifecycleEpoch)) {
    return false;
  }

  // Replace an ordinary live session only if its exact callback claim is still
  // ours. A terminal-tombstoned session is intentionally not replaceable until
  // its FIFO cleanup has freed pad buffers on g2_taps.
  if (__atomic_load_n(&sTe.active, __ATOMIC_ACQUIRE)) {
    const uint32_t oldSerial = __atomic_load_n(&sTe.serial, __ATOMIC_ACQUIRE);
    if (!g2TextEntrySessionFinishClaim(
            oldSerial, sTe.lifecycleEpoch, sTe.connectionGeneration,
            sTe.side)) {
      DEBUG_G2F("[G2] text-entry: replacement deferred for terminal cleanup");
      return false;
    }
    TextEntryAbandonFn abandon = sTe.onAbandon;
    if (sTe.padMode) g2KbdPadEnd();
    clearLocalSession();
    if (abandon) abandon();
  }

  // Seed state before publishing the exact identity. Routing consults the
  // coordinator, so active=true is not externally visible until publication.
  sTe.padMode  = false;
  __atomic_store_n(&sTe.serial, serial, __ATOMIC_RELAXED);
  __atomic_store_n(&sTe.lifecycleEpoch, lifecycleEpoch, __ATOMIC_RELAXED);
  __atomic_store_n(&sTe.connectionGeneration, connectionGeneration,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&sTe.side, side, __ATOMIC_RELAXED);
  __atomic_store_n(&sTe.isSecret, cfg.isSecret, __ATOMIC_RELAXED);
  __atomic_store_n(&sTe.active, true, __ATOMIC_RELEASE);
  g2BumpMenuGen();   // entering text entry invalidates view snapshots
  sTe.maxLen   = cfg.maxLen;
  sTe.onCommit = cfg.onCommit;
  sTe.onCancel = cfg.onCancel;
  sTe.onAbandon = cfg.onAbandon;
  sTe.group    = 0;

  if (cfg.prompt) {
    strncpy(sTe.prompt, cfg.prompt, sizeof(sTe.prompt) - 1);
    sTe.prompt[sizeof(sTe.prompt) - 1] = '\0';
  } else {
    sTe.prompt[0] = '\0';
  }

  sTe.len = 0;
  sTe.buf[0] = '\0';
  if (cfg.initial) {
    size_t in = strlen(cfg.initial);
    if (in > sTe.maxLen) in = sTe.maxLen;
    memcpy(sTe.buf, cfg.initial, in);
    sTe.buf[in] = '\0';
    sTe.len = in;
  }

  if (!g2TextEntrySessionPublish(serial, lifecycleEpoch,
                                 connectionGeneration, side)) {
    clearLocalSession();
    g2BumpMenuGen();
    DEBUG_G2F("[G2] text-entry: lifecycle publication rejected");
    return false;
  }

  // Preferred surface: the arrow-pad (nav list + four-band QWERTY image;
  // see g2KbdPadBegin). Falls back to the legacy char-list compound when
  // neither the four-band nor one-tile CREATE can land (or on admission /
  // allocation failure).
  {
    char bufText[96];
    buildBufferText(bufText, sizeof(bufText));
    // padMode is committed BEFORE the (synchronous, multi-hundred-ms) mixed
    // CREATE + initial image push, not after. During that window sTe.active is
    // already true, so a tap arriving from a context that is NOT the tap
    // dispatcher (the map worker's Search flow calls us directly) would
    // otherwise be interpreted with the LEGACY row map — 'Left' would commit,
    // 'Right' would REBUILD the list out from under the pad. With padMode set
    // up front those taps route to g2KbdPadHandleTap, which no-ops until the
    // pad is actually live. Cleared again if the pad declines.
    sTe.padMode = true;
    if (!g2KbdPadBegin(bufText)) sTe.padMode = false;
  }
  // A terminal/cmd17 boundary may have tombstoned this session while the
  // synchronous pad CREATE unwound. Do not let that stale caller fall through
  // and enqueue the legacy compound against the replacement lifecycle; the
  // exact FIFO cleanup now owns its local buffers.
  if (!sessionCurrent()) return false;
  if (sTe.padMode) {
    if (sTe.isSecret) {
      DEBUG_G2F("[G2] text-entry(pad): '%s' (max=%u, initial len=%u, secret)",
                sTe.prompt, (unsigned)sTe.maxLen, (unsigned)sTe.len);
    } else {
      DEBUG_G2F("[G2] text-entry(pad): '%s' (max=%u, initial='%s')",
                sTe.prompt, (unsigned)sTe.maxLen, sTe.buf);
    }
    // The pad's synchronous CREATE/push and the exact-session check above
    // must both succeed before handing off to its ordinary mic countdown.
    // Reuse that path's auth, readiness and cancellation rules. A declined
    // shortcut leaves the ready keyboard usable, just like manual mic entry.
    if (cfg.startWithMic && !sTe.isSecret && !g2KbdPadStartMic()) {
      DEBUG_G2F("[G2] text-entry(pad): microphone shortcut unavailable");
    }
    return true;
  }

  // Compound page-swap: list (chars) left + live buffer panel right. No
  // live-list worker any more — the page is static like Files; keystrokes
  // patch the text child directly from the tap dispatcher.
  if (!showCompound()) {
    if (g2TextEntrySessionFinishClaim(
            serial, lifecycleEpoch, connectionGeneration, side)) {
      clearLocalSession();
      g2BumpMenuGen();   // rollback also counts as a transition
    }
    DEBUG_G2F("[G2] text-entry: compound show failed");
    return false;
  }
  if (!sessionCurrent()) return false;
  if (sTe.isSecret) {
    DEBUG_G2F("[G2] text-entry: '%s' (max=%u, initial len=%u, secret)",
              sTe.prompt, (unsigned)sTe.maxLen, (unsigned)sTe.len);
  } else {
    DEBUG_G2F("[G2] text-entry: '%s' (max=%u, initial='%s')",
              sTe.prompt, (unsigned)sTe.maxLen, sTe.buf);
  }
  return true;
}

void g2TextEntryHandleTap(uint32_t idx) {
  const uint32_t serial = __atomic_load_n(&sTe.serial, __ATOMIC_ACQUIRE);
  TextEntryExecGuard execution(serial, /*cleanup*/ false);
  if (!execution || !sessionCurrent()) return;

  if (sTe.padMode) { g2KbdPadHandleTap(idx); return; }

  if (idx == kRowCancel) {
    if (sTe.isSecret) {
      DEBUG_G2F("[G2] text-entry: cancelled (len=%u, secret)", (unsigned)sTe.len);
    } else {
      DEBUG_G2F("[G2] text-entry: cancelled (buf='%s')", sTe.buf);
    }
    finishCancel(serial);
    return;
  }
  if (idx == kRowDone) {
    if (sTe.isSecret) {
      DEBUG_G2F("[G2] text-entry: done (len=%u, secret)", (unsigned)sTe.len);
    } else {
      DEBUG_G2F("[G2] text-entry: done (buf='%s')", sTe.buf);
    }
    finishCommit(serial);
    return;
  }
  if (idx == kRowGrpPrev) {
    sTe.group = (uint8_t)((sTe.group + kGroupCount - 1) % kGroupCount);
    relistGroup();
    return;
  }
  if (idx == kRowGrpNext) {
    sTe.group = (uint8_t)((sTe.group + 1) % kGroupCount);
    relistGroup();
    return;
  }

  if (idx == kRowSpace) {
    appendChar(' ');
  } else if (idx == kRowBackspace) {
    if (sTe.len > 0) sTe.buf[--sTe.len] = '\0';
  } else if (idx >= kRowFirstChar) {
    size_t charIdx = idx - kRowFirstChar;
    const char* chars = kGroupChars[sTe.group];
    size_t groupLen = strlen(chars);
    if (charIdx < groupLen) {
      appendChar(chars[charIdx]);
    } else {
      DEBUG_G2F("[G2] text-entry: tap idx=%u out of group range (%u chars)",
                (unsigned)idx, (unsigned)groupLen);
      return;
    }
  } else {
    return;  // unmapped row — no-op
  }

  // Buffer mutated — patch the panel only. List untouched by design.
  pushBuffer();
}


// High-level key events from the arrow-pad surface. Runs on the tap
// dispatcher (same context as g2TextEntryHandleTap). The pad owns cursor
// and pixels; this owns the buffer, commit/cancel and the secret policy.
void g2TextEntryPadEvent(char code) {
  const uint32_t serial = __atomic_load_n(&sTe.serial, __ATOMIC_ACQUIRE);
  TextEntryExecGuard execution(serial, /*cleanup*/ false);
  if (!execution || !sessionCurrent() || !sTe.padMode) return;
  switch (code) {
    case '\x1b':
      if (sTe.isSecret) {
        DEBUG_G2F("[G2] text-entry(pad): cancelled (len=%u, secret)",
                  (unsigned)sTe.len);
      } else {
        DEBUG_G2F("[G2] text-entry(pad): cancelled (buf='%s')", sTe.buf);
      }
      finishCancel(serial);
      return;
    case '\n':
      if (sTe.isSecret) {
        DEBUG_G2F("[G2] text-entry(pad): done (len=%u, secret)",
                  (unsigned)sTe.len);
      } else {
        DEBUG_G2F("[G2] text-entry(pad): done (buf='%s')", sTe.buf);
      }
      finishCommit(serial);
      return;
    case '\b':
      if (sTe.len > 0) sTe.buf[--sTe.len] = '\0';
      break;
    default:
      if ((unsigned char)code < 0x20) return;  // unknown control code
      appendChar(code);
      break;
  }
  pushBuffer();   // text child only — grid image untouched; selection is fast
}

size_t g2TextEntryPadRemaining(bool* needsSeparator) {
  if (needsSeparator) *needsSeparator = false;
  const uint32_t serial = __atomic_load_n(&sTe.serial, __ATOMIC_ACQUIRE);
  TextEntryExecGuard execution(serial, /*cleanup*/ false);
  if (!execution || !sessionCurrent() || !sTe.padMode || sTe.isSecret) return 0;
  if (needsSeparator && sTe.len) *needsSeparator = sTe.buf[sTe.len - 1] != ' ';
  return sTe.len < sTe.maxLen ? sTe.maxLen - sTe.len : 0;
}

size_t g2TextEntryPadAppendText(const char* text, size_t* consumed) {
  if (consumed) *consumed = 0;
  if (!text || !text[0]) return 0;
  const uint32_t serial = __atomic_load_n(&sTe.serial, __ATOMIC_ACQUIRE);
  TextEntryExecGuard execution(serial, /*cleanup*/ false);
  if (!execution || !sessionCurrent() || !sTe.padMode || sTe.isSecret) {
    return 0;
  }
  if (sTe.len >= sTe.maxLen) return 0;
  size_t add = 0;
  size_t filtered = 0;
  for (const unsigned char* p = (const unsigned char*)text;
       *p && sTe.len < sTe.maxLen; ++p) {
    if (consumed) ++*consumed;
    // Match the physical G2 grid's input contract. Control bytes, non-ASCII
    // UTF-8 fragments and double quotes cannot be entered manually either;
    // in particular, a quote would break callers that submit a quoted CLI
    // argument (rename/name editors have no escape syntax).
    if (*p < 0x20 || *p > 0x7e || *p == '"') {
      filtered++;
      continue;
    }
    sTe.buf[sTe.len++] = (char)*p;
    add++;
  }
  if (add == 0) {
    if (filtered) {
      DEBUG_G2F("[G2] text-entry(pad): dictation filtered %u byte(s)",
                (unsigned)filtered);
    }
    return 0;
  }
  sTe.buf[sTe.len] = '\0';
  pushBuffer();
  DEBUG_G2F("[G2] text-entry(pad): dictation appended %u byte(s), "
            "filtered=%u len=%u", (unsigned)add, (unsigned)filtered,
            (unsigned)sTe.len);
  return add;
}

bool g2TextEntryOperationBegin(bool* secretOut) {
  if (secretOut) *secretOut = false;
  // Snapshot before acquisition, then revalidate after it. If an off-tap
  // replacement wins between those steps, a queued operation for the old
  // surface must not mutate the replacement merely because it is current.
  const uint32_t serial = __atomic_load_n(&sTe.serial, __ATOMIC_ACQUIRE);
  if (!textEntryExecAcquire(serial, /*cleanup*/ false)) return false;
  bool secret = false;
  if (__atomic_load_n(&sTe.serial, __ATOMIC_ACQUIRE) != serial ||
      !sessionCurrent(secretOut ? &secret : nullptr)) {
    textEntryExecRelease(/*cleanup*/ false);
    return false;
  }
  // Cleanup and off-task replacement cannot mutate local session state while
  // this lease is held. Return the exact session's redaction policy so callers
  // can keep it stable even if a terminal tombstone revokes routing afterward.
  if (secretOut) *secretOut = secret;
  return true;
}

bool g2TextEntryRouteBegin(bool* currentOut, bool* localOwnedOut,
                           bool* secretOut) {
  if (currentOut) *currentOut = false;
  if (localOwnedOut) *localOwnedOut = false;
  if (secretOut) *secretOut = false;

  // Claim even the no-session state. This makes tap routing linearizable with
  // off-task begin/replacement: whichever task wins the lease goes first, and
  // the loser cannot observe a transient coordinator gap and route underneath
  // a keyboard that is being replaced.
  const uint32_t serial = __atomic_load_n(&sTe.serial, __ATOMIC_ACQUIRE);
  const uint32_t leaseSerial = serial ? serial : UINT32_MAX;
  if (!textEntryExecAcquire(leaseSerial, /*cleanup*/ false)) return false;
  if (__atomic_load_n(&sTe.serial, __ATOMIC_ACQUIRE) != serial) {
    textEntryExecRelease(/*cleanup*/ false);
    return false;
  }

  const bool localOwned = __atomic_load_n(&sTe.active, __ATOMIC_ACQUIRE);
  bool secret = false;
  const bool current = sessionCurrent(&secret);
  if (currentOut) *currentOut = current;
  if (localOwnedOut) *localOwnedOut = localOwned || current;
  if (secretOut && current) *secretOut = secret;
  return true;
}

void g2TextEntryOperationEnd() {
  textEntryExecRelease(/*cleanup*/ false);
}

bool g2TextEntryCleanupAbandoned(const G2TextEntryCleanupToken& token) {
  // Claim local execution before claiming the coordinator cleanup token. If an
  // off-tap begin is still inside pad allocation/CREATE/push, leave the exact
  // tombstone pending; g2_taps clears its queued claim and retries later.
  TextEntryExecGuard execution(token.serial, /*cleanup*/ true);
  if (!execution) return false;
  if (!g2TextEntrySessionCleanupClaim(
          token.serial, token.lifecycleEpoch, token.connectionGeneration,
          token.side)) {
    return false;
  }

  const bool exactLocal =
      __atomic_load_n(&sTe.serial, __ATOMIC_ACQUIRE) == token.serial &&
      sTe.lifecycleEpoch == token.lifecycleEpoch &&
      sTe.connectionGeneration == token.connectionGeneration &&
      sTe.side == token.side;
  if (exactLocal) {
    // Tombstone already made routing false. Clear normal callbacks before any
    // teardown helper can re-enter shared services; this path never
    // commits/cancels. The narrowly-scoped abandon hook may only scrub
    // caller-owned staging and cannot render or start another entry.
    TextEntryAbandonFn abandon = sTe.onAbandon;
    __atomic_store_n(&sTe.active, false, __ATOMIC_RELEASE);
    sTe.onCommit = nullptr;
    sTe.onCancel = nullptr;
    sTe.onAbandon = nullptr;
    if (sTe.padMode) g2KbdPadEnd();
    clearLocalSession();
    if (abandon) abandon();
    g2BumpMenuGen();
    DEBUG_G2F("[G2] text-entry: terminal cleanup complete "
              "(serial=%u life=%u gen=%u side=%c)",
              (unsigned)token.serial, (unsigned)token.lifecycleEpoch,
              (unsigned)token.connectionGeneration, token.side);
  } else {
    // Publication is blocked while a cleanup token exists, so a mismatch can
    // only be already-cleared local state. Retire the coordinator token to
    // avoid wedging future entry; never touch a nonmatching replacement.
    DEBUG_G2F("[G2] text-entry: terminal cleanup local token mismatch; no-op "
              "(serial=%u)", (unsigned)token.serial);
  }
  g2TextEntrySessionCleanupComplete(
      token.serial, token.lifecycleEpoch, token.connectionGeneration,
      token.side);
  return true;  // exact coordinator token was consumed, even if local was gone
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
