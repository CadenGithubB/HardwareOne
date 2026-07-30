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
#include "System_G2_Protocol.h"  // G2TextChildSpec + geometry presets
#include "System_Debug.h"

#include <stdio.h>
#include <string.h>

namespace {

// All session state. Lives only while gActive=true. Buffer sized 33 so
// maxLen=32 fits with NUL terminator.
struct TextEntryState {
  bool              active;
  char              buf[33];
  size_t            len;
  size_t            maxLen;
  char              prompt[32];
  TextEntryCommitFn onCommit;
  TextEntryCancelFn onCancel;
  uint8_t           group;       // index into kGroupChars
  bool              isSecret;    // suppress buf contents in debug logs
};

static TextEntryState gTE = {};

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
static const char* gRowPtrs[kMaxRows];
static char        gGrpPrevLabel[24];
static char        gGrpNextLabel[24];
static char        gCharRows[13][2];

static size_t buildRows() {
  gRowPtrs[kRowCancel]    = "X Cancel";
  gRowPtrs[kRowSpace]     = "Space";
  gRowPtrs[kRowBackspace] = "Backspace";
  gRowPtrs[kRowDone]      = "Done";
  snprintf(gGrpPrevLabel, sizeof(gGrpPrevLabel), "<- %s",
           kGroupName[(gTE.group + kGroupCount - 1) % kGroupCount]);
  snprintf(gGrpNextLabel, sizeof(gGrpNextLabel), "%s ->",
           kGroupName[(gTE.group + 1) % kGroupCount]);
  gRowPtrs[kRowGrpPrev] = gGrpPrevLabel;
  gRowPtrs[kRowGrpNext] = gGrpNextLabel;

  const char* chars = kGroupChars[gTE.group];
  size_t row = kRowFirstChar;
  for (size_t i = 0; chars[i] != '\0' && row < kMaxRows; i++, row++) {
    gCharRows[i][0] = chars[i];
    gCharRows[i][1] = '\0';
    gRowPtrs[row] = gCharRows[i];
  }
  return row;
}

// Buffer panel content. Manual wrap at 20 chars/line — the proportional
// lens font would clip a full 32-char line in the half-width box; two
// lines cover maxLen=32. Cursor "_" trails so an empty buffer still shows
// the field is editable; "(n/max)" shows remaining room.
static void buildBufferText(char* out, size_t cap) {
  const char* prompt = gTE.prompt[0] ? gTE.prompt : "Input";
  constexpr size_t kWrap = 20;
  char l1[kWrap + 1];
  char l2[kWrap + 1];
  const size_t n1 = (gTE.len < kWrap) ? gTE.len : kWrap;
  memcpy(l1, gTE.buf, n1);
  l1[n1] = '\0';
  const size_t n2 = (gTE.len > kWrap) ? gTE.len - kWrap : 0;
  memcpy(l2, gTE.buf + kWrap, n2);
  l2[n2] = '\0';
  if (n2 > 0) {
    snprintf(out, cap, "%s:\n%s\n%s_\n(%u/%u)", prompt, l1, l2,
             (unsigned)gTE.len, (unsigned)gTE.maxLen);
  } else {
    snprintf(out, cap, "%s:\n%s_\n(%u/%u)", prompt, l1,
             (unsigned)gTE.len, (unsigned)gTE.maxLen);
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
  return g2ShowMixedListText(gRowPtrs, n, kKbListGeom, spec);
}

// Per-keystroke buffer refresh: UPDATE_TEXT the panel ONLY. The list is
// untouched — its highlight/scroll persist and nothing else repaints.
static void pushBuffer() {
  char bufText[96];
  buildBufferText(bufText, sizeof(bufText));
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
  if (!g2RelistMixedListText(gRowPtrs, n, kKbListGeom, spec)) {
    DEBUG_G2F("[G2] text-entry: relist failed — falling back to full swap");
    showCompound();
  }
}

static void appendChar(char c) {
  if (gTE.len >= gTE.maxLen) return;
  gTE.buf[gTE.len++] = c;
  gTE.buf[gTE.len]   = '\0';
}

// End the session, snapshot the buffer, then run the caller's commit.
// Snapshot first so the callback sees a stable string even if it
// triggers another text-entry session synchronously.
static void finishCommit() {
  char snap[sizeof(gTE.buf)];
  strncpy(snap, gTE.buf, sizeof(snap) - 1);
  snap[sizeof(snap) - 1] = '\0';
  TextEntryCommitFn cb = gTE.onCommit;
  gTE.active = false;
  g2BumpMenuGen();   // exiting text entry invalidates view snapshots
  if (cb) cb(snap);
}

static void finishCancel() {
  TextEntryCancelFn cb = gTE.onCancel;
  gTE.active = false;
  g2BumpMenuGen();   // exiting text entry invalidates view snapshots
  if (cb) cb();
}

}  // namespace

bool g2TextEntryIsActive() { return gTE.active; }

bool g2TextEntryIsSecret() { return gTE.active && gTE.isSecret; }

bool g2BeginTextEntry(const TextEntryConfig& cfg) {
  if (!cfg.onCommit) return false;
  if (cfg.maxLen == 0 || cfg.maxLen > 32) return false;

  // Seed state. Replace any prior session — drops its buffer without
  // firing its callback. Caller guarantees they're not stomping their
  // own session by checking g2TextEntryIsActive().
  gTE.active   = true;
  g2BumpMenuGen();   // entering text entry invalidates view snapshots
  gTE.maxLen   = cfg.maxLen;
  gTE.onCommit = cfg.onCommit;
  gTE.onCancel = cfg.onCancel;
  gTE.group    = 0;
  gTE.isSecret = cfg.isSecret;

  if (cfg.prompt) {
    strncpy(gTE.prompt, cfg.prompt, sizeof(gTE.prompt) - 1);
    gTE.prompt[sizeof(gTE.prompt) - 1] = '\0';
  } else {
    gTE.prompt[0] = '\0';
  }

  gTE.len = 0;
  gTE.buf[0] = '\0';
  if (cfg.initial) {
    size_t in = strlen(cfg.initial);
    if (in > gTE.maxLen) in = gTE.maxLen;
    memcpy(gTE.buf, cfg.initial, in);
    gTE.buf[in] = '\0';
    gTE.len = in;
  }

  // Compound page-swap: list (chars) left + live buffer panel right. No
  // live-list worker any more — the page is static like Files; keystrokes
  // patch the text child directly from the tap dispatcher.
  if (!showCompound()) {
    gTE.active = false;
    g2BumpMenuGen();   // rollback also counts as a transition
    DEBUG_G2F("[G2] text-entry: compound show failed");
    return false;
  }
  if (gTE.isSecret) {
    DEBUG_G2F("[G2] text-entry: '%s' (max=%u, initial len=%u, secret)",
              gTE.prompt, (unsigned)gTE.maxLen, (unsigned)gTE.len);
  } else {
    DEBUG_G2F("[G2] text-entry: '%s' (max=%u, initial='%s')",
              gTE.prompt, (unsigned)gTE.maxLen, gTE.buf);
  }
  return true;
}

void g2TextEntryHandleTap(uint32_t idx) {
  if (!gTE.active) return;

  if (idx == kRowCancel) {
    if (gTE.isSecret) {
      DEBUG_G2F("[G2] text-entry: cancelled (len=%u, secret)", (unsigned)gTE.len);
    } else {
      DEBUG_G2F("[G2] text-entry: cancelled (buf='%s')", gTE.buf);
    }
    finishCancel();
    return;
  }
  if (idx == kRowDone) {
    if (gTE.isSecret) {
      DEBUG_G2F("[G2] text-entry: done (len=%u, secret)", (unsigned)gTE.len);
    } else {
      DEBUG_G2F("[G2] text-entry: done (buf='%s')", gTE.buf);
    }
    finishCommit();
    return;
  }
  if (idx == kRowGrpPrev) {
    gTE.group = (uint8_t)((gTE.group + kGroupCount - 1) % kGroupCount);
    relistGroup();
    return;
  }
  if (idx == kRowGrpNext) {
    gTE.group = (uint8_t)((gTE.group + 1) % kGroupCount);
    relistGroup();
    return;
  }

  if (idx == kRowSpace) {
    appendChar(' ');
  } else if (idx == kRowBackspace) {
    if (gTE.len > 0) gTE.buf[--gTE.len] = '\0';
  } else if (idx >= kRowFirstChar) {
    size_t charIdx = idx - kRowFirstChar;
    const char* chars = kGroupChars[gTE.group];
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

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
