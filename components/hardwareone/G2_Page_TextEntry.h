// =============================================================================
// G2 glasses — generic text-entry overlay
// =============================================================================
// On-glasses keyboard for short identifiers (ESPNow name, BLE peer name,
// future WiFi credentials). The lens has no real keyboard input — only
// list-tap and double-tap-dismiss — so the "keyboard" is a paginated
// character picker rendered as a tappable list. Each tap appends one char
// (or runs an edit op); Done/Cancel return control via callback.
//
// Layout (rendered as a list, top-to-bottom):
//   0: X Cancel
//   1: <prompt>: <buffer>_   (read-only — tap is a no-op)
//   2: Space
//   3: Backspace
//   4: Done
//   5: Group: <name>         (cycles a-m → n-z → A-M → N-Z → 0-9._-)
//   6..18: 13 chars from current group
//
// Callback contract: onCommit / onCancel run on the BLE notify task.
// Keep them short — repaint your own menu via g2ShowListPage if needed.
// Both callbacks are responsible for re-rendering whatever menu the user
// should see next; the text-entry module only tears down its own live
// page (g2StopLiveListPage) before invoking the callback.
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

struct TextEntryConfig {
  const char*       prompt;     // banner text e.g. "ESPNow Name"
  const char*       initial;    // pre-fill (may be null/empty)
  size_t            maxLen;     // chars excluding NUL; capped at 32
  TextEntryCommitFn onCommit;   // required
  TextEntryCancelFn onCancel;   // optional, may be null
};

// Begin a text-entry session. Replaces any active one. Returns false on
// invalid args (null onCommit, maxLen 0 or > 32) or live-page-start failure.
bool g2BeginTextEntry(const TextEntryConfig& cfg);

// True while a text-entry session is in progress.
bool g2TextEntryIsActive();

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
  const char*       prompt;
  const char*       initial;
  size_t            maxLen;
  TextEntryCommitFn onCommit;
  TextEntryCancelFn onCancel;
};

inline bool g2BeginTextEntry(const TextEntryConfig&)  { return false; }
inline bool g2TextEntryIsActive()                     { return false; }
inline void g2TextEntryHandleTap(uint32_t)            {}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
