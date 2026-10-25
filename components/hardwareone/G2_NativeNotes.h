#pragma once
#include "System_BuildConfig.h"
#include <cstddef>
#include <cstdint>

struct G2NoteContext {
  uint32_t epoch=0, left=0, right=0;
  char user[65]={};
};
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_QUICKNOTE && ENABLE_LOCAL_STT
// The G2 control owner calls tick/receive; neither does storage or transcription.
void g2NativeNotesTick();
void g2NativeNotesReceive(const G2NoteContext&, const uint8_t*, size_t);
// Cooperative worker exit for an explicit Bluetooth off or closeg2. Call after
// the control owner (the only producer) is gone and before the temple objects
// are reset; never from the worker. True: no worker remains and both queues
// are empty. False: it is still running (timeout, or called from the worker).
bool g2NativeNotesRelease(uint32_t timeoutMs);
bool g2NativeNotesActive();
// G2 transport boundary. Every physical ATT write checks the original context.
bool g2NativeNotesContext(G2NoteContext*);
bool g2NativeNotesContextLive(const G2NoteContext&);
bool g2NativeNotesWireSend(const G2NoteContext&, const uint8_t*, size_t);
#else
inline void g2NativeNotesTick() {}
inline void g2NativeNotesReceive(const G2NoteContext&, const uint8_t*, size_t) {}
inline bool g2NativeNotesRelease(uint32_t) { return true; }
inline bool g2NativeNotesActive() { return false; }
inline bool g2NativeNotesContextLive(const G2NoteContext&) { return false; }
#endif
