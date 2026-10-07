#pragma once
// G2_Page_Notes.h — Apps → Notes: QuickNote on the lens, and the consumer of a
// ring/temple hold (see G2_Page_Notes.cpp).
#include "System_BuildConfig.h"
#include "G2_HoldGesture.h"   // G2HoldReason
#include <cstddef>
#include <cstdint>
#if ENABLE_QUICKNOTE
void g2ShowNotesMenu();
void g2BuildNotesInfo(char* out, size_t cap);
void g2NotesHandleTap(uint32_t index);
void g2NotesTick();
// Hold consumers, called on g2_tap_disp only (G2_Glasses.h, G2PageHoldFn).
// g2NotesHandleHold is this page's own handler (g2RegisterPageHold). The
// HoldBegin/End/Abort trio is the global hold action (g2holdaction 1) for a
// press on any other HW1 page: Begin shows this page and starts a hold note,
// or refuses (*why) without navigating; End/Abort stop the note that press
// started, if it is still that press's.
bool g2NotesHandleHold(uint8_t phase, uint8_t source);
bool g2NotesHoldBegin(uint32_t pressId, const char** why);
void g2NotesHoldEnd(uint32_t pressId, uint32_t heldMs);
void g2NotesHoldAbort(uint32_t pressId, G2HoldReason reason);
#else
inline void g2NotesTick() {}
inline bool g2NotesHoldBegin(uint32_t, const char** why) {
  if (why) *why = "quicknote-not-built";
  return false;
}
inline void g2NotesHoldEnd(uint32_t, uint32_t) {}
inline void g2NotesHoldAbort(uint32_t, G2HoldReason) {}
#endif
