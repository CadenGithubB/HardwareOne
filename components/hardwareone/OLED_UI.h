#ifndef OLED_UI_H
#define OLED_UI_H

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "HAL_Display.h"
#include "OLED_MessageText.h"

// ============================================================================
// OLED UI Component System
// ============================================================================
// Provides reusable, standardized UI components for OLED display:
// - Toast: Temporary notification overlay (auto-dismiss)
// - Event notification adapter: same toast presentation
//
// Feedback overlays are drawn on top of the current display content and never
// capture input.
// ============================================================================

// ----------------------------------------------------------------------------
// Common Types
// ----------------------------------------------------------------------------

// Icon types for toasts
enum class OledUiIcon : uint8_t {
  NONE = 0,  // Automatic classification: recognized error prefix, otherwise info.
  INFO,
  WARNING,
  ERROR,
  SUCCESS,
  QUESTION
};

// ----------------------------------------------------------------------------
// Toast Component - Temporary notification overlay
// ----------------------------------------------------------------------------
// Shows a brief message that auto-dismisses after a timeout.
// Does not block input - just overlays on current content.

struct OledToast {
  char message[oled_message::kMessageBytes];
  uint32_t expireMs;
  OledUiIcon icon;
  bool active;
  uint32_t eventSeq;  // the event it shows (oledEventBannerShow); 0 = not from an event
};

extern OledToast gOledToast;

// Queue feedback. Errors have an eight-second minimum; duplicates coalesce.
// Full queue drops the oldest pending non-error first.
void oledToastShow(const char* message, uint32_t durationMs = 3000, OledUiIcon icon = OledUiIcon::NONE);

// A toast that changes as something progresses (a countdown): a call with
// the same nonzero tag replaces that toast, visible or pending, in place;
// otherwise it queues like oledToastShow. Any task may call it.
void oledToastUpdate(uint32_t tag, const char* message, uint32_t durationMs = 3000,
                     OledUiIcon icon = OledUiIcon::NONE);

// Clear active and pending feedback (including at session replacement).
void oledToastClear();

// Check if toast is currently showing
bool oledToastActive();

// Pause visible lifetime while a confirmation owns the content region.
void oledFeedbackSetPaused(bool paused);

// Render toast overlay (call from display update, after main content)
void oledToastRender();

// Event-notification entry point. Policy is applied by System_Notifications;
// accepted events use the shared feedback queue. Suppressed during boot.
void oledNotificationBannerShow(const char* message, OledUiIcon icon,
                                uint32_t visibleMs = 2500);

// The feedback for a notification event (systemEventsNotifyTick). Remembers the
// event's seq so clearing the notification center can take it back.
void oledEventBannerShow(uint32_t eventSeq, const char* message, OledUiIcon icon,
                         uint32_t visibleMs = 2500);

// Clearing the notification center: drop every pending event feedback with a
// seq at or below `seq`, and end the visible one if it is such a message.
// Feedback that is not from an event (seq 0) stays.
void oledEventBannersDismissThrough(uint32_t seq);

// ----------------------------------------------------------------------------
// UI System - Unified handling
// ----------------------------------------------------------------------------

// Initialize UI system (call once at startup)
void oledUiInit();

// Render all active UI overlays (call after main content, before display())
// Defers feedback while a confirmation is active.
void oledUiRender();

// ----------------------------------------------------------------------------
// Drawing Helpers - Reusable primitives
// ----------------------------------------------------------------------------

// Draw an icon at position (simple 8x8 built-in shapes)
void oledDrawIcon(int x, int y, OledUiIcon icon, uint16_t color = DISPLAY_FG);

// Draw an embedded icon by name, scaled to targetSize pixels (e.g. 12, 16, 32)
void oledDrawIcon(int x, int y, const char* iconName, int targetSize);

// Draw a named 32x32 monochrome asset on a caller's canvas. Half-size scaling
// preserves thin lines; other scales use nearest-neighbor sampling. Returns
// false for an invalid target/scale or unavailable bitmap. Does not flush.
bool oledDrawIcon(Adafruit_GFX* display, const char* name, int x, int y,
                  uint16_t color, float scale = 1.0f);

// Draw vertical level bars (for volume, signal strength, etc.)
void oledDrawLevelBars(int x, int y, int level, int maxBars, int barHeight);

#endif // ENABLE_OLED_DISPLAY
#endif // OLED_UI_H
