#pragma once
#include "System_BuildConfig.h"
#if ENABLE_OLED_DISPLAY && ENABLE_DICTATION
void prepareTranscriptionData();
void resetOledTranscription();
#else
inline void prepareTranscriptionData() {}
inline void resetOledTranscription() {}
#endif
