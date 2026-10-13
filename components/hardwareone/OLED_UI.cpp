// OLED_UI.cpp - Unified OLED UI Component System
// Provides the reusable Toast and Pairing Ribbon overlays and drawing helpers

#include "OLED_UI.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "HAL_Input.h"
#include "System_Icons.h"
#include <cstring>
#include <cmath>
#include <climits>
#include <esp_attr.h>  // EXT_RAM_BSS_ATTR

// Spinlock for thread-safe toast access (called from ESP-NOW, ESP-SR, BLE, main loop)
static portMUX_TYPE sToastMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================================
// Global State
// ============================================================================

OledToast gOledToast = {{0}, 0, OledUIIcon::NONE, false};

// ============================================================================
// Drawing Helpers
// ============================================================================

void oledDrawIcon(int x, int y, OledUIIcon icon) {
  if (!gDisplay || icon == OledUIIcon::NONE) return;
  
  // Simple 8x8 icons using basic shapes
  switch (icon) {
    case OledUIIcon::INFO:
      gDisplay->drawCircle(x + 4, y + 4, 4, DISPLAY_FG);
      gDisplay->fillRect(x + 3, y + 3, 2, 2, DISPLAY_FG);
      gDisplay->fillRect(x + 3, y + 5, 2, 3, DISPLAY_FG);
      break;
    case OledUIIcon::WARNING:
      gDisplay->drawTriangle(x + 4, y, x, y + 8, x + 8, y + 8, DISPLAY_FG);
      gDisplay->fillRect(x + 3, y + 3, 2, 3, DISPLAY_FG);
      gDisplay->fillRect(x + 3, y + 7, 2, 1, DISPLAY_FG);
      break;
    case OledUIIcon::ERROR:
      gDisplay->drawCircle(x + 4, y + 4, 4, DISPLAY_FG);
      gDisplay->drawLine(x + 2, y + 2, x + 6, y + 6, DISPLAY_FG);
      gDisplay->drawLine(x + 6, y + 2, x + 2, y + 6, DISPLAY_FG);
      break;
    case OledUIIcon::SUCCESS:
      gDisplay->drawCircle(x + 4, y + 4, 4, DISPLAY_FG);
      gDisplay->drawLine(x + 2, y + 4, x + 4, y + 6, DISPLAY_FG);
      gDisplay->drawLine(x + 4, y + 6, x + 7, y + 2, DISPLAY_FG);
      break;
    case OledUIIcon::QUESTION:
      gDisplay->drawCircle(x + 4, y + 4, 4, DISPLAY_FG);
      gDisplay->setCursor(x + 2, y + 1);
      gDisplay->print("?");
      break;
    default:
      break;
  }
}

// Helper to get a bit from 32x32 1bpp bitmap (row-major, LSB first per byte)
static inline bool getBitmapBit(const uint8_t* buffer, int x, int y) {
  if (x < 0 || x >= 32 || y < 0 || y >= 32) return false;
  int byteIndex = y * 4 + (x / 8);  // 32 pixels / 8 = 4 bytes per row
  int bitIndex = x % 8;
  return (buffer[byteIndex] >> bitIndex) & 1;
}

bool oledDrawIcon(Adafruit_GFX* display, const char* name, int x, int y, uint16_t color, float scale) {
  if (!display || !name || !name[0] || !std::isfinite(scale) || scale <= 0 ||
      scale >= static_cast<float>(INT_MAX) / 32.0f) {
    return false;
  }

  const EmbeddedIcon* icon = findEmbeddedIcon(name);
  if (!icon) return false;
  const uint8_t width = pgm_read_byte(&icon->width);
  const uint8_t height = pgm_read_byte(&icon->height);
  const uint8_t* bitmap = (const uint8_t*)pgm_read_ptr(&icon->bitmapData);
  // The embedded monochrome format is fixed at 32x32, LSB first. A PNG-only
  // entry has no drawable bitmap; do not treat it as a successful draw.
  if (!bitmap || width != 32 || height != 32) return false;
  uint8_t buffer[128];
  memcpy_P(buffer, bitmap, sizeof(buffer));

  // Calculate output dimensions
  int outWidth = (int)(width * scale);
  int outHeight = (int)(height * scale);

  // Handle 0.5x scale (32→16) with 2×2 block sampling
  if (scale >= 0.49f && scale <= 0.51f && width == 32 && height == 32) {
    for (int dy = 0; dy < 16; dy++) {
      for (int dx = 0; dx < 16; dx++) {
        // Sample 2×2 block from source
        int srcX = dx * 2;
        int srcY = dy * 2;
        bool p00 = getBitmapBit(buffer, srcX, srcY);
        bool p01 = getBitmapBit(buffer, srcX + 1, srcY);
        bool p10 = getBitmapBit(buffer, srcX, srcY + 1);
        bool p11 = getBitmapBit(buffer, srcX + 1, srcY + 1);

        // OR logic: draw pixel if any source pixel is set (preserves thin lines)
        if (p00 || p01 || p10 || p11) {
          display->drawPixel(x + dx, y + dy, color);
        }
      }
    }
    return true;
  }

  // Generic scaling for other factors (nearest neighbor). This also handles
  // 1.0x deliberately: embedded icon rows are LSB-first, while Adafruit's
  // drawBitmap() consumes MSB-first bytes.
  float invScale = 1.0f / scale;
  for (int dy = 0; dy < outHeight; dy++) {
    for (int dx = 0; dx < outWidth; dx++) {
      int srcX = (int)(dx * invScale);
      int srcY = (int)(dy * invScale);
      if (getBitmapBit(buffer, srcX, srcY)) {
        display->drawPixel(x + dx, y + dy, color);
      }
    }
  }

  return true;
}

void oledDrawIcon(int x, int y, const char* iconName, int targetSize) {
  if (!gDisplay || !iconName || !iconName[0]) return;
  
  // Icons are 32x32; compute scale factor from target size
  float scale = (float)targetSize / 32.0f;
  oledDrawIcon(gDisplay, iconName, x, y, DISPLAY_FG, scale);
}

void oledDrawLevelBars(int x, int y, int level, int maxBars, int barHeight) {
  if (!gDisplay || maxBars <= 0 || level < 0) return;
  
  // 2px wide bars with 2px gaps - clean at all scales
  const int barWidth = 2;
  const int barSpacing = 2;
  
  // Calculate bar heights (increasing progression)
  int baseHeight = barHeight / maxBars;
  if (baseHeight < 1) baseHeight = 1;
  
  for (int i = 0; i < maxBars; i++) {
    if (i >= level) break;  // Only draw filled bars
    int barX = x + i * (barWidth + barSpacing);
    int currentBarHeight = baseHeight * (i + 1);
    int barY = y + barHeight - currentBarHeight;
    gDisplay->fillRect(barX, barY, barWidth, currentBarHeight, DISPLAY_FG);
  }
}

// ============================================================================
// Toast Component
// ============================================================================

void oledToastShow(const char* message, uint32_t durationMs, OledUIIcon icon) {
  portENTER_CRITICAL(&sToastMux);
  strncpy(gOledToast.message, message, sizeof(gOledToast.message) - 1);
  gOledToast.message[sizeof(gOledToast.message) - 1] = '\0';
  gOledToast.expireMs = millis() + durationMs;
  gOledToast.icon = icon;
  gOledToast.active = true;
  portEXIT_CRITICAL(&sToastMux);
  oledMarkDirty();
}

void oledToastClear() {
  portENTER_CRITICAL(&sToastMux);
  gOledToast.active = false;
  gOledToast.message[0] = '\0';
  gOledToast.expireMs = 0;
  portEXIT_CRITICAL(&sToastMux);
  oledMarkDirty();
}

bool oledToastActive() {
  portENTER_CRITICAL(&sToastMux);
  bool active = gOledToast.active;
  if (active && millis() > gOledToast.expireMs) {
    gOledToast.active = false;
    gOledToast.message[0] = '\0';
    active = false;
  }
  portEXIT_CRITICAL(&sToastMux);
  return active;
}

void oledToastRender() {
  if (!oledToastActive() || !gDisplay) return;
  
  // Draw toast box at bottom of screen
  int boxW = SCREEN_WIDTH - 16;
  int boxH = 18;
  int boxX = 8;
  int boxY = SCREEN_HEIGHT - boxH - 12;  // Above footer
  
  // Background with border
  gDisplay->fillRect(boxX, boxY, boxW, boxH, DISPLAY_FG);
  gDisplay->drawRect(boxX, boxY, boxW, boxH, DISPLAY_BG);
  
  // Icon if present
  int textX = boxX + 4;
  if (gOledToast.icon != OledUIIcon::NONE) {
    oledDrawIcon(boxX + 4, boxY + 5, gOledToast.icon);
    textX = boxX + 14;
  }
  
  // Message text
  gDisplay->setTextSize(1);
  gDisplay->setTextColor(DISPLAY_BG);
  gDisplay->setCursor(textX, boxY + 5);
  
  // Truncate if needed
  int maxChars = (boxW - (textX - boxX) - 4) / 6;
  if ((int)strlen(gOledToast.message) > maxChars) {
    char truncated[64];
    strncpy(truncated, gOledToast.message, maxChars - 2);
    truncated[maxChars - 2] = '\0';
    strcat(truncated, "..");
    gDisplay->print(truncated);
  } else {
    gDisplay->print(gOledToast.message);
  }
  
  gDisplay->setTextColor(DISPLAY_FG);
}

// ============================================================================
// Pairing Ribbon Component
// ============================================================================

// Zero image == hidden ribbon (HIDDEN==0, LINK==0). oledPairingRibbonShow()
// rewrites every field before any state leaves HIDDEN, so the old {3000,-20}
// initializer values were dead; .ext_ram.bss is zeroed before C++ ctors run.
EXT_RAM_BSS_ATTR OledPairingRibbon gOledPairingRibbon;

// Ribbon dimensions
static const int RIBBON_WIDTH = 80;    // Width of the ribbon
static const int RIBBON_HEIGHT = 18;   // Height when fully visible
static const int RIBBON_ICON_SIZE = 16; // Icon size in the ribbon (0.5x of 32x32 -> clean 2x2 downsample)
static const int RIBBON_MIN_SIZE = 18; // Minimized indicator size (fits the 16px icon)
static const int RIBBON_ANIM_DURATION_MS = 500; // Total time for slide in/out animation (~5 frames at 10 FPS)

// Marquee scroll speed for long banner text. The panel redraws ~10 FPS, so we
// advance this many pixels per 100ms frame; 2 px ≈ 20 px/s (2x the old 1 px).
// Faster scroll also clears each banner sooner, shrinking the queue backlog.
static const int RIBBON_SCROLL_PX_PER_FRAME = 2;

// ── Notification banner queue ──────────────────────────────────────────────
// The ribbon is a single animated slot, so back-to-back notifications used to
// clobber each other. oledNotificationBannerShow() enqueues when the ribbon is
// busy; oledBannerQueuePump() shows the next one the moment it returns to idle.
// Retention isn't the point (the notification center holds full history) — this
// just paces transient pop-ups so each shows for its full duration. Overflow
// drops the OLDEST pending (favour recency; the record is still in history).
static const uint8_t BANNER_QUEUE_MAX = 12;
struct BannerQueueEntry {
  char message[128];
  PairingRibbonIcon icon;
  uint32_t visibleMs;
  bool blink;
};
EXT_RAM_BSS_ATTR static BannerQueueEntry sBannerQueue[BANNER_QUEUE_MAX];
static uint8_t  sBannerQHead = 0;
static uint8_t  sBannerQCount = 0;
static uint32_t sBannerQDropped = 0;   // overflow drops (visible via bannerqueue diag if wanted)

static void oledBannerQueuePush(const char* message, PairingRibbonIcon icon,
                                uint32_t visibleMs, bool blink) {
  if (sBannerQCount >= BANNER_QUEUE_MAX) {          // full — drop oldest pending
    sBannerQHead = (uint8_t)((sBannerQHead + 1) % BANNER_QUEUE_MAX);
    sBannerQCount--;
    sBannerQDropped++;
  }
  uint8_t tail = (uint8_t)((sBannerQHead + sBannerQCount) % BANNER_QUEUE_MAX);
  BannerQueueEntry& e = sBannerQueue[tail];
  strncpy(e.message, message, sizeof(e.message) - 1);
  e.message[sizeof(e.message) - 1] = '\0';
  e.icon = icon;
  e.visibleMs = visibleMs;
  e.blink = blink;
  sBannerQCount++;
}

// Show the next queued banner if the ribbon is idle. Called every update tick.
static void oledBannerQueuePump() {
  if (oledBootModeActive) return;
  if (sBannerQCount == 0) return;
  if (gOledPairingRibbon.state != PairingRibbonState::HIDDEN) return;
  const BannerQueueEntry& e = sBannerQueue[sBannerQHead];
  oledPairingRibbonShow(e.message, e.icon, e.visibleMs, e.blink);
  sBannerQHead = (uint8_t)((sBannerQHead + 1) % BANNER_QUEUE_MAX);
  sBannerQCount--;
}

void oledPairingRibbonShow(const char* message, PairingRibbonIcon icon, uint32_t visibleMs, bool blink) {
  strncpy(gOledPairingRibbon.message, message, sizeof(gOledPairingRibbon.message) - 1);
  gOledPairingRibbon.message[sizeof(gOledPairingRibbon.message) - 1] = '\0';
  gOledPairingRibbon.icon = icon;
  
  // Calculate dynamic duration for long messages to ensure full scroll completes
  int msgLen = strlen(gOledPairingRibbon.message);
  int textWidth = RIBBON_WIDTH - 22;  // Available pixel width for text (4px left + 18px right icon)
  int fullTextWidth = msgLen * 6;     // Total pixel width of message
  if (fullTextWidth > textWidth) {
    // Message needs scrolling - calculate time for one full scroll cycle
    // Scroll time budget: RIBBON_SCROLL_PX_PER_FRAME px per 100ms frame + end pauses.
    int pixelsToScroll = fullTextWidth - textWidth;
    uint32_t scrollTimeMs = (pixelsToScroll * 100 / RIBBON_SCROLL_PX_PER_FRAME) + 2000;
    // Use the longer of: provided duration or scroll duration, but cap at 15 seconds
    gOledPairingRibbon.visibleDurationMs = min(max(visibleMs, scrollTimeMs), (uint32_t)15000);
  } else {
    // Short message - use provided duration
    gOledPairingRibbon.visibleDurationMs = visibleMs;
  }
  
  gOledPairingRibbon.iconBlink = blink;
  gOledPairingRibbon.blinkCount = blink ? 6 : 0;  // 3 full blink cycles
  gOledPairingRibbon.state = PairingRibbonState::UNFURLING;
  gOledPairingRibbon.stateStartMs = millis();
  gOledPairingRibbon.animY = -RIBBON_HEIGHT;  // Start above screen
  gOledPairingRibbon.scrollOffset = 0;  // Reset scroll position
  gOledPairingRibbon.lastScrollMs = millis();
}

void oledPairingRibbonMinimize() {
  if (gOledPairingRibbon.state != PairingRibbonState::HIDDEN) {
    gOledPairingRibbon.state = PairingRibbonState::SHRINKING;
    gOledPairingRibbon.stateStartMs = millis();
  }
}

void oledPairingRibbonHide() {
  gOledPairingRibbon.state = PairingRibbonState::HIDDEN;
  gOledPairingRibbon.animY = -RIBBON_HEIGHT;
}

bool oledPairingRibbonActive() {
  return gOledPairingRibbon.state != PairingRibbonState::HIDDEN;
}

void oledPairingRibbonUpdate() {
  if (gOledPairingRibbon.state == PairingRibbonState::HIDDEN) {
    oledBannerQueuePump();  // idle — show the next queued notification banner, if any
    return;
  }
  
  uint32_t now = millis();
  uint32_t elapsed = now - gOledPairingRibbon.stateStartMs;
  
  switch (gOledPairingRibbon.state) {
    case PairingRibbonState::UNFURLING:
      // Time-based slide down: lerp from -RIBBON_HEIGHT to 0 over RIBBON_ANIM_DURATION_MS
      if (elapsed >= (uint32_t)RIBBON_ANIM_DURATION_MS) {
        gOledPairingRibbon.animY = 0;
        gOledPairingRibbon.state = PairingRibbonState::VISIBLE;
        gOledPairingRibbon.stateStartMs = now;
      } else {
        // Linear interpolation: -RIBBON_HEIGHT .. 0
        gOledPairingRibbon.animY = -RIBBON_HEIGHT + (int)((long)RIBBON_HEIGHT * elapsed / RIBBON_ANIM_DURATION_MS);
      }
      break;
      
    case PairingRibbonState::VISIBLE:
      // Stay visible for duration, then shrink
      if (elapsed >= gOledPairingRibbon.visibleDurationMs) {
        gOledPairingRibbon.state = PairingRibbonState::SHRINKING;
        gOledPairingRibbon.stateStartMs = now;
      }
      // Update blink state
      if (gOledPairingRibbon.blinkCount > 0 && (elapsed / 150) % 2 == 0) {
        // Decrement blink count every 300ms (full cycle)
        if (elapsed > 0 && (elapsed % 300) < 50) {
          gOledPairingRibbon.blinkCount--;
        }
      }
      // Update horizontal pixel scroll for long text (RIBBON_SCROLL_PX_PER_FRAME px / 100ms frame)
      if (now - gOledPairingRibbon.lastScrollMs >= 100) {
        gOledPairingRibbon.lastScrollMs = now;
        int msgLen = strlen(gOledPairingRibbon.message);
        int textWidth = RIBBON_WIDTH - 22;
        int fullTextWidth = msgLen * 6;
        if (fullTextWidth > textWidth) {
          gOledPairingRibbon.scrollOffset += RIBBON_SCROLL_PX_PER_FRAME;
          int maxScroll = fullTextWidth - textWidth;
          if (gOledPairingRibbon.scrollOffset > maxScroll + 12) {
            gOledPairingRibbon.scrollOffset = -12;  // Reset with brief pause (12px ~ 2 chars)
          }
        }
      }
      break;
      
    case PairingRibbonState::SHRINKING:
      // Time-based slide up: lerp from 0 to -RIBBON_HEIGHT over RIBBON_ANIM_DURATION_MS
      if (elapsed >= (uint32_t)RIBBON_ANIM_DURATION_MS) {
        gOledPairingRibbon.state = PairingRibbonState::HIDDEN;
        gOledPairingRibbon.stateStartMs = now;
        gOledPairingRibbon.animY = -RIBBON_HEIGHT;
      } else {
        gOledPairingRibbon.animY = -(int)((long)RIBBON_HEIGHT * elapsed / RIBBON_ANIM_DURATION_MS);
      }
      break;
      
    case PairingRibbonState::MINIMIZED:
      // No longer used - ribbon hides completely after shrinking
      gOledPairingRibbon.state = PairingRibbonState::HIDDEN;
      break;
      
    default:
      break;
  }
}

// Draw a small pairing icon from embedded icon sheet
static void drawPairingIcon(int x, int y, PairingRibbonIcon icon, bool visible) {
  if (!gDisplay || !visible) return;
  
  // Ribbon states reuse general-purpose icons, scaled down by oledDrawIcon.
  // These read well at RIBBON_ICON_SIZE (bold, few features); the bespoke
  // pair_* glyphs are no longer needed here.
  const char* iconName = nullptr;
  switch (icon) {
    case PairingRibbonIcon::LINK:      iconName = "check"; break;    // linked / connected
    case PairingRibbonIcon::LINK_OFF:  iconName = "close"; break;    // link down
    case PairingRibbonIcon::SYNC:      iconName = "refresh"; break;  // syncing
    case PairingRibbonIcon::SEARCHING: iconName = "search"; break;   // discovering peers
    default: break;  // SUCCESS/ERROR draw programmatically; others use text fallback below
  }
  
  // Try embedded icon first
  if (iconName) {
    const EmbeddedIcon* embIcon = findEmbeddedIcon(iconName);
    if (embIcon) {
      oledDrawIcon(x, y, iconName, RIBBON_ICON_SIZE);
      return;
    }
  }
  
  // Programmatic icons for SUCCESS (checkmark) and ERROR (X mark)
  if (icon == PairingRibbonIcon::SUCCESS) {
    // Draw checkmark: short leg down-right, long leg up-right
    gDisplay->drawLine(x + 1, y + 5, x + 4, y + 8, DISPLAY_FG);
    gDisplay->drawLine(x + 4, y + 8, x + 10, y + 2, DISPLAY_FG);
    // Thicken by drawing offset lines
    gDisplay->drawLine(x + 1, y + 6, x + 4, y + 9, DISPLAY_FG);
    gDisplay->drawLine(x + 4, y + 9, x + 10, y + 3, DISPLAY_FG);
    return;
  }
  if (icon == PairingRibbonIcon::ERROR_ICON) {
    // Draw X mark: two diagonal lines
    gDisplay->drawLine(x + 1, y + 1, x + 9, y + 9, DISPLAY_FG);
    gDisplay->drawLine(x + 9, y + 1, x + 1, y + 9, DISPLAY_FG);
    // Thicken
    gDisplay->drawLine(x + 2, y + 1, x + 10, y + 9, DISPLAY_FG);
    gDisplay->drawLine(x + 10, y + 1, x + 2, y + 9, DISPLAY_FG);
    return;
  }

  // Text fallback for other icon types
  gDisplay->setTextSize(1);
  gDisplay->setTextColor(DISPLAY_FG);
  gDisplay->setCursor(x + 2, y + 2);
  switch (icon) {
    case PairingRibbonIcon::LINK:         gDisplay->print("OK"); break;
    case PairingRibbonIcon::LINK_OFF:     gDisplay->print("X"); break;
    case PairingRibbonIcon::SYNC:         gDisplay->print("~"); break;
    case PairingRibbonIcon::SEARCHING:    gDisplay->print("?"); break;
    case PairingRibbonIcon::WARNING_ICON: gDisplay->print("!"); break;
    case PairingRibbonIcon::INFO_ICON:    gDisplay->print("i"); break;
    default: break;
  }
}

void oledNotificationBannerShow(const char* message, PairingRibbonIcon icon,
                                uint32_t visibleMs, bool blink) {
  // Suppress notifications during boot animation - they overlay the progress screen
  if (oledBootModeActive) return;
  // If the ribbon is already busy (a prior banner still animating, or a pairing
  // ribbon), queue this one instead of clobbering it — the ribbon pumps the
  // queue when it returns to idle. Show immediately only when idle.
  if (gOledPairingRibbon.state != PairingRibbonState::HIDDEN) {
    oledBannerQueuePush(message, icon, visibleMs, blink);
    return;
  }
  oledPairingRibbonShow(message, icon, visibleMs, blink);
}

void oledNotificationBannerUpdate(const char* message, PairingRibbonIcon icon,
                                  uint32_t extraMs) {
  // Suppress notifications during boot animation
  if (oledBootModeActive) return;
  // If banner is not currently showing, fall back to full show
  if (gOledPairingRibbon.state == PairingRibbonState::HIDDEN ||
      gOledPairingRibbon.state == PairingRibbonState::SHRINKING) {
    oledPairingRibbonShow(message, icon, extraMs + 1000, false);
    return;
  }
  
  // Update icon and text in place - no animation reset
  gOledPairingRibbon.icon = icon;
  strncpy(gOledPairingRibbon.message, message, sizeof(gOledPairingRibbon.message) - 1);
  gOledPairingRibbon.message[sizeof(gOledPairingRibbon.message) - 1] = '\0';
  gOledPairingRibbon.iconBlink = false;
  gOledPairingRibbon.blinkCount = 0;
  
  // Reset scroll for the new (likely shorter) text
  gOledPairingRibbon.scrollOffset = 0;
  gOledPairingRibbon.lastScrollMs = millis();
  
  // Extend visible duration from now by extraMs so user sees the new icon
  gOledPairingRibbon.state = PairingRibbonState::VISIBLE;
  gOledPairingRibbon.stateStartMs = millis();
  
  // Recalculate duration: at least extraMs, longer if new text needs scrolling
  int msgLen = strlen(gOledPairingRibbon.message);
  int textWidthBanner = RIBBON_WIDTH - 22;
  int fullTextWidth = msgLen * 6;
  if (fullTextWidth > textWidthBanner) {
    int pixelsToScroll = fullTextWidth - textWidthBanner;
    uint32_t scrollTimeMs = (pixelsToScroll * 100 / RIBBON_SCROLL_PX_PER_FRAME) + 2000;
    gOledPairingRibbon.visibleDurationMs = min(max(extraMs, scrollTimeMs), (uint32_t)15000);
  } else {
    gOledPairingRibbon.visibleDurationMs = extraMs;
  }
}

void oledPairingRibbonRender() {
  if (!gDisplay || gOledPairingRibbon.state == PairingRibbonState::HIDDEN) return;
  
  uint32_t now = millis();
  int x = SCREEN_WIDTH - RIBBON_WIDTH;  // Right-aligned
  int y = gOledPairingRibbon.animY;
  
  // Determine if icon should be visible (for blinking)
  bool iconVisible = true;
  if (gOledPairingRibbon.iconBlink && gOledPairingRibbon.blinkCount > 0) {
    iconVisible = ((now / 150) % 2) == 0;
  }
  
  if (gOledPairingRibbon.state == PairingRibbonState::MINIMIZED) {
    // Draw minimized indicator - small box in top-right corner
    int minX = SCREEN_WIDTH - RIBBON_MIN_SIZE - 2;
    int minY = 1;
    
    // Background
    gDisplay->fillRect(minX, minY, RIBBON_MIN_SIZE, RIBBON_MIN_SIZE, DISPLAY_BG);
    gDisplay->drawRect(minX, minY, RIBBON_MIN_SIZE, RIBBON_MIN_SIZE, DISPLAY_FG);
    
    // Small icon centered
    drawPairingIcon(minX + 1, minY + 1, gOledPairingRibbon.icon, true);
  } else {
    // Draw full ribbon
    // Background with border
    gDisplay->fillRect(x, y, RIBBON_WIDTH, RIBBON_HEIGHT, DISPLAY_BG);
    gDisplay->drawRect(x, y, RIBBON_WIDTH, RIBBON_HEIGHT, DISPLAY_FG);
    
    // Left edge accent (ribbon fold effect)
    gDisplay->fillTriangle(x, y, x, y + RIBBON_HEIGHT - 1, x - 4, y + RIBBON_HEIGHT / 2, DISPLAY_FG);
    
    // Text with horizontal scrolling for long messages (LEFT side)
    gDisplay->setTextSize(1);
    gDisplay->setTextColor(DISPLAY_FG);
    
    int msgLen = strlen(gOledPairingRibbon.message);
    int textX = x + 4;
    int textY = y + 5;
    int textWidth = RIBBON_WIDTH - 22;  // Available width for text (4px left pad + 18px right icon area)
    
    int charW = 6;  // pixels per character at textSize(1)
    int maxVisible = textWidth / charW;
    
    if (msgLen * charW <= textWidth) {
      // Short message - just display it
      gDisplay->setCursor(textX, textY);
      gDisplay->print(gOledPairingRibbon.message);
    } else {
      // Long message - pixel-based smooth scroll with clipping
      int pixelOffset = max(0, gOledPairingRibbon.scrollOffset);
      // Draw characters that overlap the visible text area
      int firstChar = pixelOffset / charW;
      int subPixel = pixelOffset % charW;
      int charsVisible = (textWidth + charW - 1) / charW + 1;  // +1 for partial char
      for (int i = 0; i < charsVisible && (firstChar + i) < msgLen; i++) {
        int cx = textX + i * charW - subPixel;
        if (cx + charW > textX && cx < textX + textWidth) {
          gDisplay->setCursor(cx, textY);
          gDisplay->write(gOledPairingRibbon.message[firstChar + i]);
        }
      }
    }
    
    // Icon on the RIGHT side (16px, right-aligned with a 1px margin, vertically centered in the 18px ribbon)
    drawPairingIcon(x + RIBBON_WIDTH - 17, y + 1, gOledPairingRibbon.icon, iconVisible);
  }
}

// ============================================================================
// Unified UI System
// ============================================================================

void oledUIInit() {
  memset(&gOledToast, 0, sizeof(gOledToast));
  memset(&gOledPairingRibbon, 0, sizeof(gOledPairingRibbon));
  gOledPairingRibbon.state = PairingRibbonState::HIDDEN;
  gOledPairingRibbon.animY = -RIBBON_HEIGHT;
}

void oledUIRender() {
  // Update pairing ribbon animation state
  oledPairingRibbonUpdate();
  
  // Render in layer order (bottom to top)
  oledPairingRibbonRender();  // Pairing ribbon (below the toast)
  oledToastRender();  // Toast always on top
}

#endif // ENABLE_OLED_DISPLAY
