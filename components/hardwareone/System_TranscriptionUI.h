#pragma once
#include "System_BuildConfig.h"
#include <cstddef>
#if ENABLE_DICTATION
struct CommandEntry;
extern const CommandEntry transcriptionUICommands[];
extern const size_t transcriptionUICommandsCount;
#endif
