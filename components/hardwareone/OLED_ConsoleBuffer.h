/**
 * OLED Console Buffer - Lightweight ring buffer for CLI output display
 * 
 * Stores the last N lines of CLI/debug output for display on OLED screen.
 * Independent of web interface and gWebMirror.
 * 
 * Memory cost: only the struct itself (pointers + counters) sits in .bss. The
 * ring is allocated by init(), sized to the depth latched from
 * gSettings.oledCliHistorySize rather than to the 100-line ceiling — the
 * default 50 lines costs ~3.4KB against the ceiling's ~6.8KB. The allocation is
 * what fixes the depth, so changing the setting needs a reboot.
 *
 * The ring stays in INTERNAL DRAM because it can contain private command
 * results. Credential-bearing command lines are redacted before append, and
 * the whole ring is cleared on every local-display identity transition.
 *
 * init() runs only after OLED display initialization succeeds. Devices with
 * OLED disabled in settings, or without a detected display, keep this ring
 * unallocated.
 */

#ifndef OLED_CONSOLEBUFFER_H
#define OLED_CONSOLEBUFFER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

// Ring buffer configuration
#define OLED_CONSOLE_LINES 100     // Max selectable history depth (allocation clamp ceiling)
#define OLED_CONSOLE_LINE_LEN 64   // 64 chars per line (enough for OLED width)

// OLED Console Buffer - stores recent CLI output
// The ring is null until init() allocates it, and stays null if that allocation
// fails, so every accessor tolerates a null ring: the console then degrades to
// "no history" instead of faulting.
struct OLEDConsoleBuffer {
  char (*lines)[OLED_CONSOLE_LINE_LEN];  // [capacity][LINE_LEN], INTERNAL DRAM
  uint32_t* timestamps;                  // [capacity], INTERNAL DRAM (allocated alongside lines)
  uint8_t head;      // Write position (next slot to write)
  uint8_t count;     // Number of valid lines (0 to capacity)
  uint8_t capacity;  // Allocated depth = gSettings.oledCliHistorySize (latched at init); 0 if unallocated
  SemaphoreHandle_t mutex;
  
  OLEDConsoleBuffer();
  void init();
  void clear();  // blocking identity-boundary erase
  void append(const char* text, uint32_t timestamp);
  int getLineCount() const;
  const char* getLine(int index) const;  // 0 = oldest, count-1 = newest
  uint32_t getTimestamp(int index) const;
};

// Global OLED console buffer instance
extern OLEDConsoleBuffer gOledConsole;

#endif // ENABLE_OLED_DISPLAY

#endif // OLED_CONSOLEBUFFER_H
