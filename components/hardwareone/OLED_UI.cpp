// OLED_UI.cpp - Unified OLED UI Component System
// Shared feedback queue, notification adapter and drawing helpers

#include "OLED_UI.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "System_Icons.h"
#include <cstring>
#include <cmath>
#include <climits>

// Spinlock for thread-safe toast access (called from ESP-NOW, ESP-SR, BLE, main loop)
static portMUX_TYPE sToastMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================================
// Global State
// ============================================================================

OledToast gOledToast = {{0}, 0, OledUiIcon::NONE, false, 0};

// ============================================================================
// Drawing Helpers
// ============================================================================

void oledDrawIcon(int x, int y, OledUiIcon icon, uint16_t color) {
  if (!gDisplay || icon == OledUiIcon::NONE) return;
  
  // Simple 8x8 icons using basic shapes
  switch (icon) {
    case OledUiIcon::INFO:
      gDisplay->drawCircle(x + 4, y + 4, 4, color);
      gDisplay->fillRect(x + 3, y + 3, 2, 2, color);
      gDisplay->fillRect(x + 3, y + 5, 2, 3, color);
      break;
    case OledUiIcon::WARNING:
      gDisplay->drawTriangle(x + 4, y, x, y + 8, x + 8, y + 8, color);
      gDisplay->fillRect(x + 3, y + 3, 2, 3, color);
      gDisplay->fillRect(x + 3, y + 7, 2, 1, color);
      break;
    case OledUiIcon::ERROR:
      gDisplay->drawCircle(x + 4, y + 4, 4, color);
      gDisplay->drawLine(x + 2, y + 2, x + 6, y + 6, color);
      gDisplay->drawLine(x + 6, y + 2, x + 2, y + 6, color);
      break;
    case OledUiIcon::SUCCESS:
      gDisplay->drawCircle(x + 4, y + 4, 4, color);
      gDisplay->drawLine(x + 2, y + 4, x + 4, y + 6, color);
      gDisplay->drawLine(x + 4, y + 6, x + 7, y + 2, color);
      break;
    case OledUiIcon::QUESTION:
      gDisplay->drawCircle(x + 4, y + 4, 4, color);
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

// One bounded queue serves local action replies and event notifications. The
// current item always gets its full visible lifetime. Overflow favours errors:
// routine feedback cannot evict an error, and duplicate bursts are coalesced.
static constexpr unsigned kToastQueueCapacity = 12;
struct PendingToast {
  char message[oled_message::kMessageBytes];
  uint32_t durationMs;
  OledUiIcon icon;
  uint32_t eventSeq;  // the event it shows; 0 = not from an event
  uint32_t tag;       // oledToastUpdate's tag; 0 = an ordinary toast
};
static uint32_t sToastActiveTag = 0;  // the visible toast's tag
static PendingToast sToastQueue[kToastQueueCapacity];
static unsigned sToastQueueCount = 0;
static bool sToastPaused = false;
static uint32_t sToastPausedAt = 0;

// Caller holds sToastMux. The head of the queue becomes the visible message.
static void toastPromoteNextLocked(uint32_t now) {
  gOledToast = {};
  if (!sToastQueueCount) return;
  const PendingToast item = sToastQueue[0];
  for (unsigned i = 1; i < sToastQueueCount; ++i) sToastQueue[i-1] = sToastQueue[i];
  sToastQueue[--sToastQueueCount] = {};
  memcpy(gOledToast.message, item.message, sizeof(item.message));
  gOledToast.icon = item.icon;
  gOledToast.expireMs = (sToastPaused ? sToastPausedAt : now) + item.durationMs;
  gOledToast.eventSeq = item.eventSeq;
  gOledToast.active = true;
  sToastActiveTag = item.tag;
}

static void toastEnqueue(const char* message, uint32_t durationMs, OledUiIcon icon, uint32_t eventSeq,
                         uint32_t tag = 0) {
  if (icon == OledUiIcon::NONE)
    icon = oled_message::isError(message) ? OledUiIcon::ERROR : OledUiIcon::INFO;
  if (icon == OledUiIcon::ERROR) {
    if (durationMs < oled_message::kErrorDurationMs) durationMs = oled_message::kErrorDurationMs;
    message = oled_message::reason(message);
    if (!message[0]) message = "Command failed. Check logs.";
  }
  PendingToast item{};
  snprintf(item.message, sizeof(item.message), "%s", message ? message : "");
  item.durationMs = durationMs;
  item.icon = icon;
  item.eventSeq = eventSeq;
  item.tag = tag;
  portENTER_CRITICAL(&sToastMux);
  // A tagged update replaces its own toast in place: the visible one, or one
  // still waiting in the queue.
  if (tag != 0) {
    if (gOledToast.active && sToastActiveTag == tag) {
      memcpy(gOledToast.message, item.message, sizeof(item.message));
      gOledToast.icon = icon;
      gOledToast.expireMs = (sToastPaused ? sToastPausedAt : millis()) + durationMs;
      portEXIT_CRITICAL(&sToastMux);
      oledMarkDirty();
      return;
    }
    for (unsigned i = 0; i < sToastQueueCount; ++i) {
      if (sToastQueue[i].tag != tag) continue;
      sToastQueue[i] = item;
      portEXIT_CRITICAL(&sToastMux);
      return;
    }
  }
  // A repeat is coalesced into the message already shown or pending. That
  // message then stands for the newer event too, so a clear of the notification
  // center that predates the newer event leaves it.
  bool duplicate = false;
  if (gOledToast.active && gOledToast.icon == icon &&
      strcmp(gOledToast.message, item.message) == 0 &&
      (sToastPaused || static_cast<int32_t>(millis() - gOledToast.expireMs) < 0)) {
    duplicate = true;
    if (gOledToast.eventSeq != 0 && eventSeq > gOledToast.eventSeq) gOledToast.eventSeq = eventSeq;
  }
  for (unsigned i = 0; i < sToastQueueCount; ++i) {
    if (sToastQueue[i].icon != icon || strcmp(sToastQueue[i].message, item.message) != 0) continue;
    duplicate = true;
    if (sToastQueue[i].eventSeq != 0 && eventSeq > sToastQueue[i].eventSeq) sToastQueue[i].eventSeq = eventSeq;
  }
  if (!duplicate) {
    if (!gOledToast.active) {
      memcpy(gOledToast.message, item.message, sizeof(item.message));
      gOledToast.icon = icon;
      gOledToast.expireMs = (sToastPaused ? sToastPausedAt : millis()) + durationMs;
      gOledToast.eventSeq = eventSeq;
      gOledToast.active = true;
      sToastActiveTag = tag;
    } else {
      if (sToastQueueCount == kToastQueueCapacity) {
        unsigned discard = 0;
        while (discard < sToastQueueCount && sToastQueue[discard].icon == OledUiIcon::ERROR) ++discard;
        // With only errors pending, discard the oldest only for another error.
        if (discard == sToastQueueCount && icon == OledUiIcon::ERROR) discard = 0;
        if (discard < sToastQueueCount) {
          for (unsigned i = discard + 1; i < sToastQueueCount; ++i) sToastQueue[i-1] = sToastQueue[i];
          --sToastQueueCount;
        }
      }
      if (sToastQueueCount < kToastQueueCapacity) sToastQueue[sToastQueueCount++] = item;
    }
  }
  portEXIT_CRITICAL(&sToastMux);
  oledMarkDirty();
}

void oledToastShow(const char* message, uint32_t durationMs, OledUiIcon icon) {
  toastEnqueue(message, durationMs, icon, 0);
}

void oledToastUpdate(uint32_t tag, const char* message, uint32_t durationMs, OledUiIcon icon) {
  toastEnqueue(message, durationMs, icon, 0, tag);
}

void oledEventBannersDismissThrough(uint32_t seq) {
  portENTER_CRITICAL(&sToastMux);
  // Keep the pending messages that are not cleared, in their order.
  unsigned kept = 0;
  for (unsigned i = 0; i < sToastQueueCount; ++i) {
    if (sToastQueue[i].eventSeq != 0 && sToastQueue[i].eventSeq <= seq) continue;
    if (kept != i) sToastQueue[kept] = sToastQueue[i];
    ++kept;
  }
  for (unsigned i = kept; i < sToastQueueCount; ++i) sToastQueue[i] = {};
  sToastQueueCount = kept;
  // The visible one ends now; the next kept message takes its place.
  if (gOledToast.active && gOledToast.eventSeq != 0 && gOledToast.eventSeq <= seq)
    toastPromoteNextLocked(millis());
  portEXIT_CRITICAL(&sToastMux);
  oledMarkDirty();
}

void oledToastClear() {
  portENTER_CRITICAL(&sToastMux);
  gOledToast = {};
  sToastActiveTag = 0;
  // Erase messages as well as counters at a display identity boundary.
  memset(sToastQueue, 0, sizeof(sToastQueue));
  sToastQueueCount = 0;
  sToastPaused = false;
  sToastPausedAt = 0;
  portEXIT_CRITICAL(&sToastMux);
  oledMarkDirty();
}

void oledFeedbackSetPaused(bool paused) {
  portENTER_CRITICAL(&sToastMux);
  if (paused != sToastPaused) {
    const uint32_t now = millis();
    if (paused) sToastPausedAt = now;
    else if (gOledToast.active) gOledToast.expireMs += now - sToastPausedAt;
    sToastPaused = paused;
  }
  portEXIT_CRITICAL(&sToastMux);
}

bool oledToastActive() {
  bool changed = false;
  portENTER_CRITICAL(&sToastMux);
  if (!sToastPaused && gOledToast.active && static_cast<int32_t>(millis() - gOledToast.expireMs) >= 0) {
    toastPromoteNextLocked(millis());
    changed = true;
  }
  const bool active = gOledToast.active;
  portEXIT_CRITICAL(&sToastMux);
  if (changed) oledMarkDirty();
  return active;
}

void oledToastRender() {
  if (!oledToastActive() || !gDisplay) return;
  OledToast toast;
  portENTER_CRITICAL(&sToastMux);
  toast = gOledToast;
  portEXIT_CRITICAL(&sToastMux);
  if (!toast.active) return;

  const int boxW = SCREEN_WIDTH - 16, boxX = 8;
  const int iconSpace = toast.icon == OledUiIcon::NONE ? 0 : 10;
  const int textX = boxX + 4 + iconSpace;
  const int columns = (boxW - 8 - iconSpace) / 6;
  const int maxLines = (OLED_CONTENT_HEIGHT - 10) / 8;
  const auto lines = oled_message::wrap(toast.message, columns, maxLines);
  const int boxH = (lines.count ? lines.count : 1) * 8 + 8;
  const int boxY = SCREEN_HEIGHT - boxH - 12;
  gDisplay->fillRect(boxX, boxY, boxW, boxH, DISPLAY_FG);
  gDisplay->drawRect(boxX, boxY, boxW, boxH, DISPLAY_BG);
  if (toast.icon != OledUiIcon::NONE)
    oledDrawIcon(boxX + 4, boxY + 5, toast.icon, DISPLAY_BG);
  gDisplay->setTextSize(1);
  gDisplay->setTextColor(DISPLAY_BG);
  for (int row = 0; row < lines.count; ++row) {
    gDisplay->setCursor(textX, boxY + 5 + row*8);
    gDisplay->print(lines.text[row]);
  }
  gDisplay->setTextColor(DISPLAY_FG);
}

// Event policy remains in System_Notifications; accepted events share the
// local-feedback queue, severity and renderer.
void oledEventBannerShow(uint32_t eventSeq, const char* message, OledUiIcon icon,
                         uint32_t visibleMs) {
  if (oledBootModeActive) return;
  toastEnqueue(message, visibleMs, icon, eventSeq);
}

void oledNotificationBannerShow(const char* message, OledUiIcon icon,
                                uint32_t visibleMs) {
  oledEventBannerShow(0, message, icon, visibleMs);
}

void oledUiInit() { oledToastClear(); }

void oledUiRender() {
  oledFeedbackSetPaused(oledConfirmIsActive());
  if (!oledConfirmIsActive()) oledToastRender();
}

#endif // ENABLE_OLED_DISPLAY
