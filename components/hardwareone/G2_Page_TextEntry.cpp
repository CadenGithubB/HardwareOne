// =============================================================================
// G2 glasses — generic text-entry overlay implementation
// =============================================================================
// See G2_Page_TextEntry.h for layout + contract. The session piggy-backs
// on the live-page worker so each tap fires a fast REBUILD-list (~80 ms)
// instead of a SHUTDOWN+CREATE swap (~600 ms). The worker's interval is
// set to 60 s as a passive watchdog; every tap explicitly kicks the
// refresh semaphore so the visible buffer updates immediately.
#include "G2_Page_TextEntry.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_Glasses.h"
#include "System_Debug.h"

#include <stdarg.h>
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
};

static TextEntryState gTE = {};

// 13 chars per group keeps each row short (single-char) and the total
// row count under 20 — comfortable for the firmware's list pagination.
// Symbol set is intentionally small: just enough for hostnames and BLE
// peer names. Add more groups (punctuation, brackets) if a future caller
// needs them.
static const char* const kGroupChars[] = {
  "abcdefghijklm",
  "nopqrstuvwxyz",
  "ABCDEFGHIJKLM",
  "NOPQRSTUVWXYZ",
  "0123456789._-",
};
static const char* const kGroupName[] = {
  "a-m", "n-z", "A-M", "N-Z", "0-9 . _ -",
};
static constexpr size_t kGroupCount =
    sizeof(kGroupChars) / sizeof(kGroupChars[0]);

// Row indices — fixed positions for the control rows. Char rows start at
// kRowFirstChar. The dispatcher trusts these positions.
//
// NOTE: row 0 is the back row auto-prepended by the live-page worker. We
// pass backLabel="X Cancel" so the prepended row IS the cancel affordance
// — buildFn must NOT emit a separate cancel row or every other index
// shifts by 1 and the dispatcher tapping wrong things.
constexpr size_t kRowCancel    = 0;
constexpr size_t kRowBuffer    = 1;
constexpr size_t kRowSpace     = 2;
constexpr size_t kRowBackspace = 3;
constexpr size_t kRowDone      = 4;
constexpr size_t kRowGroup     = 5;
constexpr size_t kRowFirstChar = 6;

// Append printf-style to a bounded buffer, advancing offset. Silently
// truncates on overflow — the buildFn budget (2048 B from the live-page
// worker) is far larger than any sane text-entry render.
static void appendf(char* out, size_t cap, size_t& off,
                    const char* fmt, ...) {
  if (off + 1 >= cap) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(out + off, cap - off, fmt, ap);
  va_end(ap);
  if (n > 0) off += (size_t)n;
}

// G2LivePageBuildFn — called by the live-page worker on each REBUILD tick.
// Output is split on \n into list rows by the worker.
static void buildTextEntryText(char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  size_t off = 0;

  // Row 0 is the auto-prepended back row carrying our "X Cancel" label —
  // do NOT emit it from buildFn or the indices shift.
  // Buffer line — the cursor "_" trails so an empty buffer still shows
  // something for the user to register that the field is editable.
  appendf(out, cap, off, "%s: %s_\n",
          gTE.prompt[0] ? gTE.prompt : "Input",
          gTE.buf);
  appendf(out, cap, off, "Space\n");
  appendf(out, cap, off, "Backspace\n");
  appendf(out, cap, off, "Done\n");
  appendf(out, cap, off, "Group: %s\n", kGroupName[gTE.group]);

  const char* chars = kGroupChars[gTE.group];
  for (size_t i = 0; chars[i] != '\0'; i++) {
    appendf(out, cap, off, "%c\n", chars[i]);
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
  g2StopLiveListPage();
  if (cb) cb(snap);
}

static void finishCancel() {
  TextEntryCancelFn cb = gTE.onCancel;
  gTE.active = false;
  g2StopLiveListPage();
  if (cb) cb();
}

}  // namespace

bool g2TextEntryIsActive() { return gTE.active; }

bool g2BeginTextEntry(const TextEntryConfig& cfg) {
  if (!cfg.onCommit) return false;
  if (cfg.maxLen == 0 || cfg.maxLen > 32) return false;

  // Seed state. Replace any prior session — drops its buffer without
  // firing its callback. Caller guarantees they're not stomping their
  // own session by checking g2TextEntryIsActive().
  gTE.active   = true;
  gTE.maxLen   = cfg.maxLen;
  gTE.onCommit = cfg.onCommit;
  gTE.onCancel = cfg.onCancel;
  gTE.group    = 0;

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

  // 60 s interval is just a passive heartbeat — every tap kicks the sem
  // for an immediate REBUILD. Lower would burn BLE bandwidth on idle
  // pages; we don't want a self-driven refresh stomping on the user's
  // mid-tap state.
  // backLabel="X Cancel" repurposes the auto-prepended back row as the
  // session's cancel affordance — see kRow* note above.
  if (!g2StartLiveListPage(buildTextEntryText, 60000, "X Cancel")) {
    gTE.active = false;
    DEBUG_G2F("[G2] text-entry: live-page start failed");
    return false;
  }
  DEBUG_G2F("[G2] text-entry: '%s' (max=%u, initial='%s')",
            gTE.prompt, (unsigned)gTE.maxLen, gTE.buf);
  return true;
}

void g2TextEntryHandleTap(uint32_t idx) {
  if (!gTE.active) return;

  if (idx == kRowCancel) {
    DEBUG_G2F("[G2] text-entry: cancelled (buf='%s')", gTE.buf);
    finishCancel();
    return;
  }
  if (idx == kRowBuffer) {
    // Tapping the buffer line is a no-op — caret is just a display.
    return;
  }
  if (idx == kRowDone) {
    DEBUG_G2F("[G2] text-entry: done (buf='%s')", gTE.buf);
    finishCommit();
    return;
  }
  if (idx == kRowSpace) {
    appendChar(' ');
  } else if (idx == kRowBackspace) {
    if (gTE.len > 0) gTE.buf[--gTE.len] = '\0';
  } else if (idx == kRowGroup) {
    gTE.group = (uint8_t)((gTE.group + 1) % kGroupCount);
  } else if (idx >= kRowFirstChar) {
    size_t charIdx = idx - kRowFirstChar;
    const char* chars = kGroupChars[gTE.group];
    size_t groupLen = strlen(chars);
    if (charIdx < groupLen) appendChar(chars[charIdx]);
    else {
      DEBUG_G2F("[G2] text-entry: tap idx=%u out of group range (%u chars)",
                (unsigned)idx, (unsigned)groupLen);
    }
  }

  // Mutated state — kick the live-page so the buffer line updates now.
  g2KickLivePageRefresh();
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
