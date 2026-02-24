// OLED_UI.cpp - Unified OLED UI Component System
// Provides reusable Toast, Dialog, Progress, and List components

#include "OLED_UI.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "HAL_Input.h"
#include "System_Icons.h"
#include <cstring>

// Spinlock for thread-safe toast access (called from ESP-NOW, ESP-SR, BLE, main loop)
static portMUX_TYPE sToastMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================================
// Global State
// ============================================================================

OledToast gOledToast = {{0}, 0, OledUIIcon::NONE, false};
OledDialog gOledDialog = {{0}, {{0}}, 0, {OledUIButton::NONE}, 0, 0, {nullptr}, nullptr, OledUIIcon::NONE, false};
OledProgress gOledProgress = {{0}, 0, 0, false, false};
OledList gOledList = {{0}, {{0}}, 0, 0, 0, 4, nullptr, nullptr, nullptr, false};

// ============================================================================
// Drawing Helpers
// ============================================================================

void oledDrawBox(int x, int y, int w, int h, bool filled) {
  if (!gDisplay) return;
  if (filled) {
    gDisplay->fillRect(x, y, w, h, DISPLAY_FG);
    gDisplay->drawRect(x, y, w, h, DISPLAY_BG);
  } else {
    gDisplay->fillRect(x, y, w, h, DISPLAY_BG);
    gDisplay->drawRect(x, y, w, h, DISPLAY_FG);
  }
}

void oledDrawButton(int x, int y, int w, const char* label, bool selected) {
  if (!gDisplay) return;
  const int h = 11;
  
  if (selected) {
    gDisplay->fillRect(x, y, w, h, DISPLAY_FG);
    gDisplay->setTextColor(DISPLAY_BG, DISPLAY_FG);
  } else {
    gDisplay->drawRect(x, y, w, h, DISPLAY_FG);
    gDisplay->setTextColor(DISPLAY_FG, DISPLAY_BG);
  }
  
  // Center text in button
  int textW = strlen(label) * 6;
  int textX = x + (w - textW) / 2;
  gDisplay->setCursor(textX, y + 2);
  gDisplay->print(label);
  
  // Reset text color
  gDisplay->setTextColor(DISPLAY_FG);
}

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

void oledDrawIcon(int x, int y, const char* iconName, int targetSize) {
  if (!gDisplay || !iconName || !iconName[0]) return;
  
  extern bool drawIconScaled(Adafruit_SSD1306* display, const char* name, int x, int y, uint16_t color, float scale);
  
  // Icons are 32x32; compute scale factor from target size
  float scale = (float)targetSize / 32.0f;
  drawIconScaled(gDisplay, iconName, x, y, DISPLAY_FG, scale);
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

void oledDrawTextCentered(int x, int y, int w, const char* text) {
  if (!gDisplay || !text) return;
  int textW = strlen(text) * 6;
  int textX = x + (w - textW) / 2;
  if (textX < x) textX = x;
  gDisplay->setCursor(textX, y);
  gDisplay->print(text);
}

const char* oledUIButtonLabel(OledUIButton btn) {
  switch (btn) {
    case OledUIButton::OK:     return "OK";
    case OledUIButton::CANCEL: return "Cancel";
    case OledUIButton::YES:    return "Yes";
    case OledUIButton::NO:     return "No";
    case OledUIButton::RETRY:  return "Retry";
    default:                   return "";
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
// Dialog Component
// ============================================================================

static void oledDialogSetup(const char* title, const char* message, OledUIIcon icon) {
  memset(&gOledDialog, 0, sizeof(gOledDialog));
  
  if (title) {
    strncpy(gOledDialog.title, title, sizeof(gOledDialog.title) - 1);
  }
  
  // Split message into lines (simple word wrap)
  if (message) {
    const int maxLineLen = sizeof(gOledDialog.lines[0]) - 1;
    const char* src = message;
    int lineIdx = 0;
    
    while (*src && lineIdx < 3) {
      int len = strlen(src);
      if (len <= maxLineLen) {
        strncpy(gOledDialog.lines[lineIdx], src, maxLineLen);
        lineIdx++;
        break;
      }
      
      // Find break point
      int breakAt = maxLineLen;
      while (breakAt > 0 && src[breakAt] != ' ' && src[breakAt] != '\n') breakAt--;
      if (breakAt == 0) breakAt = maxLineLen;
      
      strncpy(gOledDialog.lines[lineIdx], src, breakAt);
      gOledDialog.lines[lineIdx][breakAt] = '\0';
      lineIdx++;
      
      src += breakAt;
      while (*src == ' ' || *src == '\n') src++;
    }
    gOledDialog.lineCount = lineIdx;
  }
  
  gOledDialog.icon = icon;
  gOledDialog.selectedButton = 0;
  gOledDialog.active = true;
  oledMarkDirty();
}

void oledDialogOK(const char* title, const char* message, OledUICallback onOK, void* userData) {
  oledDialogSetup(title, message, OledUIIcon::INFO);
  gOledDialog.buttons[0] = OledUIButton::OK;
  gOledDialog.onButton[0] = onOK;
  gOledDialog.buttonCount = 1;
  gOledDialog.userData = userData;
}

void oledDialogYesNo(const char* title, const char* message, OledUICallback onYes, OledUICallback onNo, void* userData) {
  oledDialogSetup(title, message, OledUIIcon::QUESTION);
  gOledDialog.buttons[0] = OledUIButton::YES;
  gOledDialog.buttons[1] = OledUIButton::NO;
  gOledDialog.onButton[0] = onYes;
  gOledDialog.onButton[1] = onNo;
  gOledDialog.buttonCount = 2;
  gOledDialog.userData = userData;
}

void oledDialogCustom(const char* title, const char* message,
                      OledUIButton btn1, OledUICallback cb1,
                      OledUIButton btn2, OledUICallback cb2,
                      void* userData, OledUIIcon icon) {
  oledDialogSetup(title, message, icon);
  gOledDialog.buttons[0] = btn1;
  gOledDialog.onButton[0] = cb1;
  gOledDialog.buttonCount = 1;
  
  if (btn2 != OledUIButton::NONE) {
    gOledDialog.buttons[1] = btn2;
    gOledDialog.onButton[1] = cb2;
    gOledDialog.buttonCount = 2;
  }
  gOledDialog.userData = userData;
}

void oledDialogClose() {
  gOledDialog.active = false;
  oledMarkDirty();
}

bool oledDialogActive() {
  return gOledDialog.active;
}

bool oledDialogHandleInput(uint32_t newlyPressed) {
  if (!gOledDialog.active) return false;
  
  extern NavEvents gNavEvents;
  bool handled = false;
  
  // Left/right to switch buttons
  if (gOledDialog.buttonCount > 1) {
    if (gNavEvents.left || gNavEvents.up) {
      if (gOledDialog.selectedButton > 0) {
        gOledDialog.selectedButton--;
        oledMarkDirty();
      }
      handled = true;
    } else if (gNavEvents.right || gNavEvents.down) {
      if (gOledDialog.selectedButton < gOledDialog.buttonCount - 1) {
        gOledDialog.selectedButton++;
        oledMarkDirty();
      }
      handled = true;
    }
  }
  
  // A to confirm, B to cancel (INPUT_CHECK and InputButton from HAL_Input.h)
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    int idx = gOledDialog.selectedButton;
    if (gOledDialog.onButton[idx]) {
      gOledDialog.onButton[idx](gOledDialog.userData);
    }
    oledDialogClose();
    handled = true;
  } else if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    // B always cancels/closes
    oledDialogClose();
    handled = true;
  }
  
  return handled;
}

void oledDialogRender() {
  if (!gOledDialog.active || !gDisplay) return;
  
  // Center dialog box
  int boxW = SCREEN_WIDTH - 8;
  int boxH = 44;
  int boxX = 4;
  int boxY = (SCREEN_HEIGHT - boxH) / 2 - 4;
  
  // Background
  gDisplay->fillRect(boxX, boxY, boxW, boxH, DISPLAY_BG);
  gDisplay->drawRect(boxX, boxY, boxW, boxH, DISPLAY_FG);
  gDisplay->drawRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, DISPLAY_FG);
  
  gDisplay->setTextSize(1);
  gDisplay->setTextColor(DISPLAY_FG);
  
  // Title bar
  int titleY = boxY + 2;
  int contentX = boxX + 4;
  
  if (gOledDialog.icon != OledUIIcon::NONE) {
    oledDrawIcon(contentX, titleY, gOledDialog.icon);
    contentX += 12;
  }
  
  gDisplay->setCursor(contentX, titleY);
  gDisplay->print(gOledDialog.title);
  
  // Separator line
  gDisplay->drawLine(boxX + 2, boxY + 12, boxX + boxW - 3, boxY + 12, DISPLAY_FG);
  
  // Message lines
  int lineY = boxY + 16;
  for (int i = 0; i < gOledDialog.lineCount && i < 3; i++) {
    gDisplay->setCursor(boxX + 4, lineY);
    gDisplay->print(gOledDialog.lines[i]);
    lineY += 9;
  }
  
  // Buttons
  int btnY = boxY + boxH - 13;
  int btnW = (gOledDialog.buttonCount == 1) ? 40 : 36;
  int totalBtnW = btnW * gOledDialog.buttonCount + (gOledDialog.buttonCount - 1) * 4;
  int btnX = boxX + (boxW - totalBtnW) / 2;
  
  for (int i = 0; i < gOledDialog.buttonCount; i++) {
    oledDrawButton(btnX, btnY, btnW, oledUIButtonLabel(gOledDialog.buttons[i]), 
                   i == gOledDialog.selectedButton);
    btnX += btnW + 4;
  }
}

// ============================================================================
// Progress Component
// ============================================================================

void oledProgressShow(const char* label, int percent, bool cancellable) {
  strncpy(gOledProgress.label, label ? label : "", sizeof(gOledProgress.label) - 1);
  gOledProgress.percent = percent;
  gOledProgress.startMs = millis();
  gOledProgress.cancellable = cancellable;
  gOledProgress.active = true;
  oledMarkDirty();
}

void oledProgressUpdate(int percent) {
  gOledProgress.percent = percent;
  oledMarkDirty();
}

void oledProgressLabel(const char* label) {
  strncpy(gOledProgress.label, label ? label : "", sizeof(gOledProgress.label) - 1);
  oledMarkDirty();
}

void oledProgressClose() {
  gOledProgress.active = false;
  oledMarkDirty();
}

bool oledProgressActive() {
  return gOledProgress.active;
}

void oledProgressRender() {
  if (!gOledProgress.active || !gDisplay) return;
  
  int boxW = SCREEN_WIDTH - 20;
  int boxH = 28;
  int boxX = 10;
  int boxY = (SCREEN_HEIGHT - boxH) / 2;
  
  // Background
  gDisplay->fillRect(boxX, boxY, boxW, boxH, DISPLAY_BG);
  gDisplay->drawRect(boxX, boxY, boxW, boxH, DISPLAY_FG);
  
  gDisplay->setTextSize(1);
  gDisplay->setTextColor(DISPLAY_FG);
  
  // Label
  oledDrawTextCentered(boxX, boxY + 3, boxW, gOledProgress.label);
  
  // Progress bar
  int barX = boxX + 4;
  int barY = boxY + 14;
  int barW = boxW - 8;
  int barH = 8;
  
  gDisplay->drawRect(barX, barY, barW, barH, DISPLAY_FG);
  
  if (gOledProgress.percent >= 0) {
    // Determinate progress
    int fillW = (barW - 2) * gOledProgress.percent / 100;
    if (fillW > 0) {
      gDisplay->fillRect(barX + 1, barY + 1, fillW, barH - 2, DISPLAY_FG);
    }
  } else {
    // Indeterminate - animated bar
    uint32_t elapsed = millis() - gOledProgress.startMs;
    int pos = (elapsed / 50) % (barW - 10);
    gDisplay->fillRect(barX + 1 + pos, barY + 1, 10, barH - 2, DISPLAY_FG);
  }
  
  // Cancel hint
  if (gOledProgress.cancellable) {
    gDisplay->setCursor(boxX + boxW - 20, boxY + boxH - 9);
    gDisplay->print("B:X");
  }
}

// ============================================================================
// List Component
// ============================================================================

void oledListClear(const char* title) {
  memset(&gOledList, 0, sizeof(gOledList));
  if (title) {
    strncpy(gOledList.title, title, sizeof(gOledList.title) - 1);
  }
  gOledList.visibleCount = 4;  // Default visible items
}

void oledListAddItem(const char* label, int value) {
  if (gOledList.itemCount >= OLED_LIST_MAX_ITEMS) return;
  
  strncpy(gOledList.items[gOledList.itemCount].label, label, OLED_LIST_ITEM_LEN - 1);
  gOledList.items[gOledList.itemCount].value = value;
  gOledList.itemCount++;
}

void oledListFinalize(OledListCallback onSelect, OledListCallback onCancel, void* userData) {
  gOledList.onSelect = onSelect;
  gOledList.onCancel = onCancel;
  gOledList.userData = userData;
  gOledList.selectedIndex = 0;
  gOledList.scrollOffset = 0;
  gOledList.active = true;
  oledMarkDirty();
}

void oledListShow(const char* title, const OledListItem* items, uint8_t count,
                  OledListCallback onSelect, OledListCallback onCancel, void* userData) {
  oledListClear(title);
  
  int copyCount = (count > OLED_LIST_MAX_ITEMS) ? OLED_LIST_MAX_ITEMS : count;
  memcpy(gOledList.items, items, copyCount * sizeof(OledListItem));
  gOledList.itemCount = copyCount;
  
  oledListFinalize(onSelect, onCancel, userData);
}

void oledListClose() {
  gOledList.active = false;
  oledMarkDirty();
}

bool oledListActive() {
  return gOledList.active;
}

bool oledListHandleInput(uint32_t newlyPressed) {
  if (!gOledList.active) return false;
  
  extern NavEvents gNavEvents;
  bool handled = false;
  
  if (gNavEvents.up) {
    if (gOledList.selectedIndex > 0) {
      gOledList.selectedIndex--;
      if (gOledList.selectedIndex < gOledList.scrollOffset) {
        gOledList.scrollOffset = gOledList.selectedIndex;
      }
      oledMarkDirty();
    }
    handled = true;
  } else if (gNavEvents.down) {
    if (gOledList.selectedIndex < gOledList.itemCount - 1) {
      gOledList.selectedIndex++;
      if (gOledList.selectedIndex >= gOledList.scrollOffset + gOledList.visibleCount) {
        gOledList.scrollOffset = gOledList.selectedIndex - gOledList.visibleCount + 1;
      }
      oledMarkDirty();
    }
    handled = true;
  }
  
  // A to select, B to cancel (INPUT_CHECK and InputButton from HAL_Input.h)
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (gOledList.onSelect && gOledList.itemCount > 0) {
      gOledList.onSelect(gOledList.selectedIndex, 
                         gOledList.items[gOledList.selectedIndex].value,
                         gOledList.userData);
    }
    oledListClose();
    handled = true;
  } else if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (gOledList.onCancel) {
      gOledList.onCancel(gOledList.selectedIndex, -1, gOledList.userData);
    }
    oledListClose();
    handled = true;
  }
  
  return handled;
}

void oledListRender() {
  if (!gOledList.active || !gDisplay) return;
  
  int boxW = SCREEN_WIDTH - 8;
  int boxH = SCREEN_HEIGHT - 16;
  int boxX = 4;
  int boxY = 4;
  
  // Background
  gDisplay->fillRect(boxX, boxY, boxW, boxH, DISPLAY_BG);
  gDisplay->drawRect(boxX, boxY, boxW, boxH, DISPLAY_FG);
  
  gDisplay->setTextSize(1);
  gDisplay->setTextColor(DISPLAY_FG);
  
  // Title
  oledDrawTextCentered(boxX, boxY + 2, boxW, gOledList.title);
  gDisplay->drawLine(boxX + 2, boxY + 11, boxX + boxW - 3, boxY + 11, DISPLAY_FG);
  
  // List items
  int itemH = 10;
  int listY = boxY + 14;
  int listH = boxH - 18;
  gOledList.visibleCount = listH / itemH;
  
  for (int i = 0; i < gOledList.visibleCount && (gOledList.scrollOffset + i) < gOledList.itemCount; i++) {
    int idx = gOledList.scrollOffset + i;
    int itemY = listY + i * itemH;
    
    if (idx == gOledList.selectedIndex) {
      gDisplay->fillRect(boxX + 2, itemY, boxW - 4, itemH - 1, DISPLAY_FG);
      gDisplay->setTextColor(DISPLAY_BG, DISPLAY_FG);
    } else {
      gDisplay->setTextColor(DISPLAY_FG);
    }
    
    gDisplay->setCursor(boxX + 4, itemY + 1);
    gDisplay->print(gOledList.items[idx].label);
  }
  
  // Scroll indicators
  gDisplay->setTextColor(DISPLAY_FG);
  if (gOledList.scrollOffset > 0) {
    gDisplay->setCursor(boxX + boxW - 8, listY);
    gDisplay->print("^");
  }
  if (gOledList.scrollOffset + gOledList.visibleCount < gOledList.itemCount) {
    gDisplay->setCursor(boxX + boxW - 8, boxY + boxH - 10);
    gDisplay->print("v");
  }
}

// ============================================================================
// Pairing Ribbon Component
// ============================================================================

OledPairingRibbon gOledPairingRibbon = {{0}, PairingRibbonState::HIDDEN, PairingRibbonIcon::LINK, 0, 3000, -20, false, 0};

// Ribbon dimensions
static const int RIBBON_WIDTH = 80;    // Width of the ribbon
static const int RIBBON_HEIGHT = 18;   // Height when fully visible
static const int RIBBON_ICON_SIZE = 12; // Small icon size (0.375x scale of 32x32)
static const int RIBBON_MIN_SIZE = 14; // Minimized indicator size
static const int RIBBON_ANIM_DURATION_MS = 500; // Total time for slide in/out animation (~5 frames at 10 FPS)

void oledPairingRibbonShow(const char* message, PairingRibbonIcon icon, uint32_t visibleMs, bool blink) {
  strncpy(gOledPairingRibbon.message, message, sizeof(gOledPairingRibbon.message) - 1);
  gOledPairingRibbon.message[sizeof(gOledPairingRibbon.message) - 1] = '\0';
  gOledPairingRibbon.icon = icon;
  
  // Calculate dynamic duration for long messages to ensure full scroll completes
  int msgLen = strlen(gOledPairingRibbon.message);
  int textWidth = RIBBON_WIDTH - 18;  // Available pixel width for text (4px left + 14px right icon)
  int fullTextWidth = msgLen * 6;     // Total pixel width of message
  if (fullTextWidth > textWidth) {
    // Message needs scrolling - calculate time for one full scroll cycle
    // Scroll speed: 1 pixel per frame (100ms at 10 FPS), plus pauses at each end
    int pixelsToScroll = fullTextWidth - textWidth;
    uint32_t scrollTimeMs = (pixelsToScroll * 100) + 2000;  // scroll time + pauses
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
  if (gOledPairingRibbon.state == PairingRibbonState::HIDDEN) return;
  
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
      // Update horizontal pixel scroll for long text (1 pixel per frame at 10 FPS)
      if (now - gOledPairingRibbon.lastScrollMs >= 100) {
        gOledPairingRibbon.lastScrollMs = now;
        int msgLen = strlen(gOledPairingRibbon.message);
        int textWidth = RIBBON_WIDTH - 18;
        int fullTextWidth = msgLen * 6;
        if (fullTextWidth > textWidth) {
          gOledPairingRibbon.scrollOffset++;
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
  
  // Map ribbon icons to embedded icon names (pairing-specific icons)
  const char* iconName = nullptr;
  switch (icon) {
    case PairingRibbonIcon::LINK:      iconName = "pair_link"; break;
    case PairingRibbonIcon::LINK_OFF:  iconName = "pair_link_off"; break;
    case PairingRibbonIcon::SYNC:      iconName = "pair_sync"; break;
    case PairingRibbonIcon::SEARCHING: iconName = "pair_search"; break;
    default: break;  // General icons use text fallback below
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
  extern bool oledBootModeActive;
  if (oledBootModeActive) return;
  oledPairingRibbonShow(message, icon, visibleMs, blink);
}

void oledNotificationBannerUpdate(const char* message, PairingRibbonIcon icon,
                                  uint32_t extraMs) {
  // Suppress notifications during boot animation
  extern bool oledBootModeActive;
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
  int textWidthBanner = RIBBON_WIDTH - 18;
  int fullTextWidth = msgLen * 6;
  if (fullTextWidth > textWidthBanner) {
    int pixelsToScroll = fullTextWidth - textWidthBanner;
    uint32_t scrollTimeMs = (pixelsToScroll * 100) + 2000;  // 1 pixel per frame + pauses
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
    int textWidth = RIBBON_WIDTH - 18;  // Available width for text (4px left pad + 14px right icon area)
    
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
    
    // Icon on the RIGHT side
    drawPairingIcon(x + RIBBON_WIDTH - 13, y + 3, gOledPairingRibbon.icon, iconVisible);
  }
}

// ============================================================================
// Unified UI System
// ============================================================================

void oledUIInit() {
  memset(&gOledToast, 0, sizeof(gOledToast));
  memset(&gOledDialog, 0, sizeof(gOledDialog));
  memset(&gOledProgress, 0, sizeof(gOledProgress));
  memset(&gOledList, 0, sizeof(gOledList));
  memset(&gOledPairingRibbon, 0, sizeof(gOledPairingRibbon));
  gOledPairingRibbon.state = PairingRibbonState::HIDDEN;
  gOledPairingRibbon.animY = -RIBBON_HEIGHT;
}

bool oledUIHandleInput(uint32_t newlyPressed) {
  // Handle in priority order (topmost first)
  if (oledDialogActive()) {
    return oledDialogHandleInput(newlyPressed);
  }
  if (oledListActive()) {
    return oledListHandleInput(newlyPressed);
  }
  // Toast and progress don't capture input (except progress cancel)
  if (oledProgressActive() && gOledProgress.cancellable) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      oledProgressClose();
      return true;
    }
  }
  return false;
}

void oledUIRender() {
  // Update pairing ribbon animation state
  oledPairingRibbonUpdate();
  
  // Render in layer order (bottom to top)
  oledProgressRender();
  oledListRender();
  oledPairingRibbonRender();  // Pairing ribbon (below dialog/toast)
  oledDialogRender();
  oledToastRender();  // Toast always on top
  
  // Render data source indicator when paired and not LOCAL
  if (gDataSourceIndicatorVisible && gDataSource != DataSource::LOCAL && gDisplay) {
    const char* label = nullptr;
    switch (gDataSource) {
      case DataSource::REMOTE: label = "R"; break;
      case DataSource::BOTH:   label = "B"; break;
      default: break;
    }
    if (label) {
      // Draw indicator in top-right corner
      int x = SCREEN_WIDTH - 10;
      int y = 1;
      gDisplay->fillRect(x - 1, y - 1, 10, 9, DISPLAY_BG);
      gDisplay->drawRect(x - 1, y - 1, 10, 9, DISPLAY_FG);
      gDisplay->setTextSize(1);
      gDisplay->setTextColor(DISPLAY_FG);
      gDisplay->setCursor(x + 1, y);
      gDisplay->print(label);
    }
  }
}

bool oledUIModalActive() {
  return oledDialogActive() || oledListActive();
}

#endif // ENABLE_OLED_DISPLAY
