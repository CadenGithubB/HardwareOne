#pragma once

#include "sdkconfig.h"
#include <freertos/FreeRTOS.h>

#include "System_MemTrackerCore.h"

#if defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && \
    CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
inline constexpr size_t kMemTrackerCapacity = 512;
#else
inline constexpr size_t kMemTrackerCapacity = 64;
#endif

// Initializes the statically-backed mutex and starts a fresh cumulative
// allocation-traffic window.  This must run before tracking is enabled.
bool memTrackerInit(bool enabled);

// Enable/disable is non-destructive.  Reset clears the cumulative window while
// preserving the current enabled state.
bool memTrackerSetEnabled(bool enabled);
bool memTrackerReset();

// Copies an immutable, internally-consistent snapshot.  topEntries may be null
// only when topCapacity is zero.  No logging or allocation occurs while the
// tracker mutex is held.
bool memTrackerSnapshot(MemTrackerSnapshot& out,
                        MemTrackerEntry* topEntries = nullptr,
                        size_t topCapacity = 0,
                        TickType_t timeoutTicks = portMAX_DELAY);

// Non-blocking diagnostic update used by the ps_alloc hook.  Contention,
// invalid tags, and a full registry are counted explicitly instead of risking
// latency or corrupting the registry.
void memTrackerRecord(const char* op, void* ptr, size_t size,
                      bool requestedPS, bool usedPS, bool fellBack,
                      const char* tag);
