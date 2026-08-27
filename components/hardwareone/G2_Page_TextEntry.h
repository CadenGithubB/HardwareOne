// =============================================================================
// G2 glasses — generic text-entry overlay
// =============================================================================
// On-glasses keyboard for short identifiers (ESPNow name, WiFi credentials,
// usernames, filenames). The preferred surface is an image-backed QWERTY grid:
// indexed ListEvent CLICK moves its cursor and a rowless ring SysEvent
// DOUBLE_CLICK selects the current key. A grouped tappable-character list is
// the compatibility fallback. Done/Cancel return control via callback.
//
// Compound layout (mic-detail/Health pattern — one page, two children):
//   LEFT half — tappable list (top-to-bottom):
//     0: X Cancel
//     1: Space
//     2: Backspace
//     3: Done
//     4: <- <group>          (previous group)
//     5: <group> ->          (next group)
//     6..18: 13 chars from current group
//   RIGHT half — display-only text panel, updated LIVE on every keystroke:
//     <prompt>:
//     <buffer>_              (wrapped at 24 chars)
//     (n/max)
//
// Keystrokes patch ONLY the text panel (Cmd=5 UPDATE_TEXT — fire-and-
// forget); the list child is never rebuilt, so the firmware keeps its
// highlight/scroll and nothing else repaints. Group switches are the one
// list-changing op: in-place multi-child REBUILD (~80 ms), full-swap
// fallback.
//
// Callback contract: onCommit / onCancel run on the G2 tap-dispatch worker,
// never on the BLE callback or G2 control/ACK owner. Keep them short — repaint
// your own menu via g2ShowListPage if needed.
// Both callbacks are responsible for re-rendering whatever menu the user
// should see next (that swap tears the keyboard compound down).
//
// Single-instance: only one text-entry session at a time. Calling
// g2BeginTextEntry while an ordinary session is active replaces it (no
// commit/cancel fires; the optional cleanup-only abandon hook does). A
// terminal-tombstoned session is not replaceable until its exact FIFO cleanup
// has run; begin returns false during that short interval.
#pragma once

#include "System_BuildConfig.h"
#include <stddef.h>

// Storage ceiling for the shared G2 text-entry surface. Individual callers
// still choose a field-specific maxLen at or below this value.
static constexpr size_t G2_TEXT_ENTRY_MAX_LEN = 256;

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <stdint.h>

typedef void (*TextEntryCommitFn)(const char* text);
typedef void (*TextEntryCancelFn)(void);
typedef void (*TextEntryAbandonFn)(void);

// Every field carries a default initializer: at least one caller
// stack-allocates this bare (no `= {}`), so a defaulted field is the only
// thing keeping a newly added member from being read uninitialized.
struct TextEntryConfig {
  const char*       prompt   = nullptr;  // banner text e.g. "ESPNow Name"
  const char*       initial  = nullptr;  // pre-fill (may be null/empty)
  size_t            maxLen   = 0;        // chars excluding NUL; capped at 256
  TextEntryCommitFn onCommit = nullptr;  // required
  TextEntryCancelFn onCancel = nullptr;  // optional, may be null
  // Optional cleanup-only hook for replacement or terminal abandonment. It
  // runs under the text-entry execution lease and must only clear caller-owned
  // staging: no rendering, commands, or new text-entry session.
  TextEntryAbandonFn onAbandon = nullptr;
  bool              isSecret = false;    // mask display; never log/dictate
                                         // buffer contents (passwords/PSKs)
};

// Exact identity of an abandoned entry session. Terminal/lifecycle reduction
// tombstones routing synchronously, then carries this value through the
// g2_tap_disp FIFO. Cleanup never invokes commit/cancel; after older tap jobs
// complete it may invoke the cleanup-only onAbandon hook. A stale token is
// always a no-op.
struct G2TextEntryCleanupToken {
  uint32_t serial;
  uint32_t lifecycleEpoch;
  uint32_t connectionGeneration;
  char     side;
  uint8_t  reserved[3];
};

// Begin a text-entry session. Replaces any active one. Returns false on
// invalid args (null onCommit, maxLen 0 or > G2_TEXT_ENTRY_MAX_LEN) or
// live-page-start failure.
bool g2BeginTextEntry(const TextEntryConfig& cfg);

// High-level key event from the arrow-pad surface (G2_Glasses.cpp):
// printable char = append; '\b' backspace; '\n' Done (commit); '\x1b'
// Cancel. Pad-mode sessions only; no-op otherwise.
void g2TextEntryPadEvent(char code);

// Append a bounded transcript to the active arrow-pad field and refresh only
// its TEXT child. Runs on the tap-dispatch worker; returns the number of bytes
// accepted. Secret fields refuse dictation entirely.
size_t g2TextEntryPadAppendText(const char* text);

// True while a text-entry session is in progress.
bool g2TextEntryIsActive();

// True while the active session is secret (passwords/PSKs). Callers that
// log taps must suppress the row label AND the row idx — on the keyboard
// page either one narrows which character was typed.
bool g2TextEntryIsSecret();

// Tap dispatcher — called from the hijack tap router when a session is
// active, BEFORE the page-specific handler. Routes the tap to the
// text-entry handler instead of the underlying page.
void g2TextEntryHandleTap(uint32_t idx);

// Internal execution lease for G2_Glasses.cpp paths that mutate the pad
// before calling back into this module (rowless double-select and dictation
// service). Begin is non-blocking and validates the exact current session;
// every successful Begin must be paired with End on the same task. Holding
// this lease keeps terminal cleanup from freeing pad buffers mid-operation.
bool g2TextEntryOperationBegin(bool* secretOut = nullptr);
void g2TextEntryOperationEnd();

// Tap-router classification lease. Unlike OperationBegin, this also succeeds
// when no current session exists, preventing an off-task replacement from
// starting between "no keyboard" classification and underlying page dispatch.
// `localOwnedOut` is true for either a current session or a terminal-tombstoned
// local session whose FIFO cleanup has not yet reclaimed its buffers.
bool g2TextEntryRouteBegin(bool* currentOut, bool* localOwnedOut,
                           bool* secretOut);

// Internal lifecycle hook consumed by G2_Glasses.cpp's tap dispatcher.
// Must run on g2_tap_disp. It never invokes onCommit/onCancel; onAbandon, when
// configured, is limited by contract to clearing caller-owned staging.
bool g2TextEntryCleanupAbandoned(const G2TextEntryCleanupToken& token);

#else  // !ENABLE_BLUETOOTH || !ENABLE_G2_GLASSES

#include <stddef.h>
#include <stdint.h>

typedef void (*TextEntryCommitFn)(const char* text);
typedef void (*TextEntryCancelFn)(void);
typedef void (*TextEntryAbandonFn)(void);

struct TextEntryConfig {
  const char*       prompt   = nullptr;
  const char*       initial  = nullptr;
  size_t            maxLen   = 0;
  TextEntryCommitFn onCommit = nullptr;
  TextEntryCancelFn onCancel = nullptr;
  TextEntryAbandonFn onAbandon = nullptr;
  bool              isSecret = false;
};

struct G2TextEntryCleanupToken {
  uint32_t serial;
  uint32_t lifecycleEpoch;
  uint32_t connectionGeneration;
  char     side;
  uint8_t  reserved[3];
};

inline bool g2BeginTextEntry(const TextEntryConfig&)  { return false; }
inline bool g2TextEntryIsActive()                     { return false; }
inline bool g2TextEntryIsSecret()                     { return false; }
inline void g2TextEntryHandleTap(uint32_t)            {}
inline void g2TextEntryPadEvent(char)                 {}
inline size_t g2TextEntryPadAppendText(const char*)   { return 0; }
inline bool g2TextEntryOperationBegin(bool* = nullptr) { return false; }
inline void g2TextEntryOperationEnd()                 {}
inline bool g2TextEntryRouteBegin(bool* currentOut, bool* localOwnedOut,
                                  bool* secretOut) {
  if (currentOut) *currentOut = false;
  if (localOwnedOut) *localOwnedOut = false;
  if (secretOut) *secretOut = false;
  return true;
}
inline bool g2TextEntryCleanupAbandoned(
    const G2TextEntryCleanupToken&)                   { return false; }

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
