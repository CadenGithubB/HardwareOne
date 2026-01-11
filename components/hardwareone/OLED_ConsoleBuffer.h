/**
 * OLED Console Buffer - Lightweight ring buffer for CLI output display
 * 
 * Stores the last N lines of CLI/debug output for display on OLED screen.
 * Independent of web interface and gWebMirror.
 * 
 * Memory cost: 50 lines × 64 chars = 3.2KB + overhead ≈ 3.5KB total
 */

#ifndef OLED_CONSOLE_BUFFER_H
#define OLED_CONSOLE_BUFFER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

// Ring buffer configuration
#define OLED_CONSOLE_LINES 50      // Keep last 50 lines
#define OLED_CONSOLE_LINE_LEN 64   // 64 chars per line (enough for OLED width)

// OLED Console Buffer - stores recent CLI output
struct OLEDConsoleBuffer {
  char lines[OLED_CONSOLE_LINES][OLED_CONSOLE_LINE_LEN];
  uint32_t timestamps[OLED_CONSOLE_LINES];
  uint8_t head;   // Write position (next slot to write)
  uint8_t count;  // Number of valid lines (0 to OLED_CONSOLE_LINES)
  SemaphoreHandle_t mutex;
  
  OLEDConsoleBuffer();
  void init();
  void append(const char* text, uint32_t timestamp);
  int getLineCount() const;
  const char* getLine(int index) const;  // 0 = oldest, count-1 = newest
  uint32_t getTimestamp(int index) const;
};

// Global OLED console buffer instance
extern OLEDConsoleBuffer gOLEDConsole;

#endif // ENABLE_OLED_DISPLAY

#endif // OLED_CONSOLE_BUFFER_H
