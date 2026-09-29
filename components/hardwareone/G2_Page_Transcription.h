#pragma once
#include "System_BuildConfig.h"
#include <cstddef>
#include <cstdint>
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_DICTATION
void g2ShowTranscriptionMenu();
void g2BuildTranscriptionInfo(char* out, size_t cap);
void g2TranscriptionHandleTap(uint32_t index);
void g2TranscriptionTick();
#else
inline void g2ShowTranscriptionMenu() {}
inline void g2BuildTranscriptionInfo(char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
inline void g2TranscriptionHandleTap(uint32_t) {}
inline void g2TranscriptionTick() {}
#endif
