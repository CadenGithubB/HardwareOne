/**
 * System Power Management
 * 
 * Handles CPU frequency scaling and display brightness management
 * for battery optimization.
 */

#ifndef SYSTEM_POWER_H
#define SYSTEM_POWER_H

#include <Arduino.h>

// Power mode constants
#define POWER_MODE_PERFORMANCE  0
#define POWER_MODE_BALANCED     1
#define POWER_MODE_POWERSAVER   2
#define POWER_MODE_ULTRASAVER   3

// ============================================================================
// Power Mode Management Functions
// ============================================================================

const char* getPowerModeName(uint8_t mode);
uint32_t getPowerModeCpuFreq(uint8_t mode);
uint8_t getPowerModeDisplayBrightness(uint8_t mode);

void applyPowerMode(uint8_t mode);
void checkAutoPowerMode();

// ============================================================================
// Command Registry
// ============================================================================

struct CommandEntry;
extern const CommandEntry powerCommands[];
extern const size_t powerCommandsCount;

#endif
