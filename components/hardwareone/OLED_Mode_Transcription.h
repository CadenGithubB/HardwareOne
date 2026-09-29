#pragma once
#include "System_BuildConfig.h"
#if ENABLE_OLED_DISPLAY && ENABLE_DICTATION
void prepareTranscriptionData();
void resetOLEDTranscription();
#else
inline void prepareTranscriptionData() {}
inline void resetOLEDTranscription() {}
#endif
