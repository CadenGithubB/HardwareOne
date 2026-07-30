// =============================================================================
// G2 glasses — generic text-entry overlay
// =============================================================================
// On-glasses keyboard for short identifiers (ESPNow name, WiFi credentials,
// usernames, filenames). The lens has no real keyboard input — only
// list-tap and double-tap-dismiss — so the "keyboard" is a character picker
// rendered as a tappable list. Each tap appends one char (or runs an edit
// op); Done/Cancel return control via callback.
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
//     <buffer>_              (wrapped at 20 chars)
//     (n/max)
//
// Keystrokes patch ONLY the text panel (Cmd=5 UPDATE_TEXT — fire-and-
// forget); the list child is never rebuilt, so the firmware keeps its
// highlight/scroll and nothing else repaints. Group switches are the one
// list-changing op: in-place multi-child REBUILD (~80 ms), full-swap
// fallback.
//
// Callback contract: onCommit / onCancel run on the BLE notify task.
// Keep them short — repaint your own menu via g2ShowListPage if needed.
// Both callbacks are responsible for re-rendering whatever menu the user
// should see next (that swap tears the keyboard compound down).
//
// Single-instance: only one text-entry session at a time. Calling
// g2BeginTextEntry while one is active replaces it (no callback fires
// for the old session — the buffer is dropped).
#pragma once

#include "System_BuildConfig.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <stddef.h>
#include <stdint.h>

typedef void (*TextEntryCommitFn)(const char* text);
typedef void (*TextEntryCancelFn)(void);

// Every field carries a default initializer: at least one caller
// stack-allocates this bare (no `= {}`), so a defaulted field is the only
// thing keeping a newly added member from being read uninitialized.
struct TextEntryConfig {
  const char*       prompt   = nullptr;  // banner text e.g. "ESPNow Name"
  const char*       initial  = nullptr;  // pre-fill (may be null/empty)
  size_t            maxLen   = 0;        // chars excluding NUL; capped at 32
  TextEntryCommitFn onCommit = nullptr;  // required
  TextEntryCancelFn onCancel = nullptr;  // optional, may be null
  bool              isSecret = false;    // true = never log buffer contents
                                         // (passwords/PSKs); length only
};

// Begin a text-entry session. Replaces any active one. Returns false on
// invalid args (null onCommit, maxLen 0 or > 32) or live-page-start failure.
bool g2BeginTextEntry(const TextEntryConfig& cfg);

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

#else  // !ENABLE_BLUETOOTH || !ENABLE_G2_GLASSES

#include <stddef.h>
#include <stdint.h>

typedef void (*TextEntryCommitFn)(const char* text);
typedef void (*TextEntryCancelFn)(void);

struct TextEntryConfig {
  const char*       prompt   = nullptr;
  const char*       initial  = nullptr;
  size_t            maxLen   = 0;
  TextEntryCommitFn onCommit = nullptr;
  TextEntryCancelFn onCancel = nullptr;
  bool              isSecret = false;
};

inline bool g2BeginTextEntry(const TextEntryConfig&)  { return false; }
inline bool g2TextEntryIsActive()                     { return false; }
inline bool g2TextEntryIsSecret()                     { return false; }
inline void g2TextEntryHandleTap(uint32_t)            {}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
