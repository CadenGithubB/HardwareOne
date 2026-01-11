#ifndef MEMORY_MONITOR_H
#define MEMORY_MONITOR_H

#include <Arduino.h>

// ============================================================================
// Memory Threshold Registry
// ============================================================================

// Memory requirements for each component (derived from task stack sizes)
struct MemoryRequirement {
  const char* component;
  size_t minHeapBytes;      // Minimum free heap needed to start
  size_t taskStackWords;    // Task stack size in words (0 if no task)
  size_t minPsramBytes;     // Minimum PSRAM needed (0 if not required)
};

// Get memory requirements for a component
// Returns nullptr if component not found
const MemoryRequirement* getMemoryRequirement(const char* component);

// Check if sufficient memory available for a component
// Returns true if memory check passes, false otherwise
// If outReason is provided, fills with failure reason
bool checkMemoryAvailable(const char* component, String* outReason = nullptr);

// Get all registered memory requirements (for diagnostics)
const MemoryRequirement* getAllMemoryRequirements(size_t& outCount);

// ============================================================================
// Memory Sampling Command
// ============================================================================

// Sample current memory state and output to CLI
// This is the core function called by both manual command and periodic sampling
void sampleMemoryState();

// CLI command handler for manual memory sampling
const char* cmd_memsample(const String& cmd);

// Periodic memory sampling (called from main loop when debug flag enabled)
void periodicMemorySample();

#endif // MEMORY_MONITOR_H
