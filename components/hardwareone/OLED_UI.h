#ifndef OLED_UI_H
#define OLED_UI_H

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "HAL_Display.h"

// ============================================================================
// OLED UI Component System
// ============================================================================
// Provides reusable, standardized UI components for OLED display:
// - Toast: Temporary notification overlay (auto-dismiss)
// - Pairing ribbon / notification banner: animated status indicator
//
// Both are overlays drawn on top of the current display content; neither
// captures input.
// ============================================================================

// ----------------------------------------------------------------------------
// Common Types
// ----------------------------------------------------------------------------

// Icon types for toasts
enum class OledUIIcon : uint8_t {
  NONE = 0,
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
  char message[64];
  uint32_t expireMs;
  OledUIIcon icon;
  bool active;
};

extern OledToast gOledToast;

// Show a toast message (auto-dismisses after durationMs)
void oledToastShow(const char* message, uint32_t durationMs = 3000, OledUIIcon icon = OledUIIcon::NONE);

// Clear any active toast immediately
void oledToastClear();

// Check if toast is currently showing
bool oledToastActive();

// Render toast overlay (call from display update, after main content)
void oledToastRender();

// ----------------------------------------------------------------------------
// (Removed) OledList modal list-selector overlay
// ----------------------------------------------------------------------------
// The OledList component was a dormant modal list-picker superseded by
// OLEDScrollState (OLED_Utils.h). It had no callers and was removed. For any
// list/menu, use OLEDScrollState + oledScrollRenderSimple/oledScrollHandleNav.

// ----------------------------------------------------------------------------
// Pairing Ribbon Component - Animated status indicator
// ----------------------------------------------------------------------------
// Shows pairing status with unfurl/shrink animation from top-right corner.
// Displays icon + status text, then minimizes to a small indicator.

enum class PairingRibbonState : uint8_t {
  HIDDEN = 0,      // Not visible
  UNFURLING,       // Animating down from top
  VISIBLE,         // Fully visible, showing status
  SHRINKING,       // Animating to minimized state
  MINIMIZED        // Small persistent indicator in corner
};

enum class PairingRibbonIcon : uint8_t {
  LINK = 0,        // Connected (chain link)
  LINK_OFF,        // Disconnected (broken link)
  SYNC,            // Handshake in progress (rotating arrows)
  SEARCHING,       // Looking for peer (magnifying glass)
  // General-purpose notification icons (text-only fallback, no embedded icon needed)
  SUCCESS,         // Checkmark / OK
  ERROR_ICON,      // X / fail
  WARNING_ICON,    // ! / caution
  INFO_ICON        // i / information
};

struct OledPairingRibbon {
  char message[128];             // Status text (e.g., "Connected", peer name, command result)
  PairingRibbonState state;      // Animation state
  PairingRibbonIcon icon;        // Current icon to display
  uint32_t stateStartMs;         // When current state started
  uint32_t visibleDurationMs;    // How long to stay visible before shrinking
  int animY;                     // Current Y position for animation
  bool iconBlink;                // Whether icon should blink
  uint8_t blinkCount;            // Number of blinks remaining
  int scrollOffset;              // Horizontal scroll position in pixels for long text
  uint32_t lastScrollMs;         // Last time scroll position updated
};

extern OledPairingRibbon gOledPairingRibbon;

// Show pairing ribbon with message and icon
void oledPairingRibbonShow(const char* message, PairingRibbonIcon icon, 
                           uint32_t visibleMs = 3000, bool blink = true);

// Update to minimized state (call when you want persistent indicator)
void oledPairingRibbonMinimize();

// Hide ribbon completely
void oledPairingRibbonHide();

// Check if ribbon is visible (any state except HIDDEN)
bool oledPairingRibbonActive();

// Update animation state (call from main loop)
void oledPairingRibbonUpdate();

// Render ribbon overlay
void oledPairingRibbonRender();

// General-purpose notification banner (wraps ribbon for non-pairing use)
void oledNotificationBannerShow(const char* message, PairingRibbonIcon icon,
                                uint32_t visibleMs = 2500, bool blink = false);

// Update an already-visible banner in place (icon + text) without re-animating.
// Extends display by extraMs so the user sees the change. Falls back to full
// show() if banner is not currently visible.
void oledNotificationBannerUpdate(const char* message, PairingRibbonIcon icon,
                                  uint32_t extraMs = 1000);

// ----------------------------------------------------------------------------
// UI System - Unified handling
// ----------------------------------------------------------------------------

// Initialize UI system (call once at startup)
void oledUIInit();

// Render all active UI overlays (call after main content, before display())
// Renders in order: pairing ribbon, then toast on top
void oledUIRender();

// ----------------------------------------------------------------------------
// Drawing Helpers - Reusable primitives
// ----------------------------------------------------------------------------

// Draw an icon at position (simple 8x8 built-in shapes)
void oledDrawIcon(int x, int y, OledUIIcon icon);

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
