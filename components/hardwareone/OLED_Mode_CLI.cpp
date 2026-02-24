// ============================================================================
// OLED CLI Viewer Mode
// ============================================================================
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "OLED_ConsoleBuffer.h"
#include "System_Utils.h"
#include "System_Debug.h"

// External OLED console buffer (display pointer via HAL_Display.h #define oledDisplay gDisplay)
extern OLEDConsoleBuffer gOLEDConsole;

// CLI viewer state
static int cliScrollOffset = 0;
static uint32_t cliSelectedTimestamp = 0;  // Track by timestamp (survives buffer shifts)
static bool cliShowingDetail = false;
static uint32_t cliDetailLockedTimestamp = 0;  // Locked message timestamp when viewing detail
// Calculate max visible lines based on content area (43px / 10px per line = 4 lines)
static const int CLI_MAX_VISIBLE_LINES = OLED_CONTENT_HEIGHT / 10;

// Helper: find index by timestamp, returns -1 if not found
static int findIndexByTimestamp(uint32_t ts) {
  if (ts == 0) return -1;
  int count = gOLEDConsole.getLineCount();
  for (int i = 0; i < count; i++) {
    if (gOLEDConsole.getTimestamp(i) == ts) return i;
  }
  return -1;  // Message was evicted from buffer
}

// Get current CLI viewer selection (1-indexed for display, 0 if none)
int getCLIViewerSelectedIndex() {
  int idx = findIndexByTimestamp(cliSelectedTimestamp);
  return (idx >= 0) ? idx + 1 : 0;  // 1-indexed for user display
}

// CLI display function
static void displayCLIViewer() {
  if (!oledDisplay) {
    ERROR_SYSTEMF("[CLI_VIEWER] oledDisplay is NULL!");
    return;
  }
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Lock buffer for reading
  if (!gOLEDConsole.mutex || xSemaphoreTake(gOLEDConsole.mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->print("Buffer locked...");
    return;
  }
  
  int totalLines = gOLEDConsole.getLineCount();
  
  if (totalLines == 0) {
    xSemaphoreGive(gOLEDConsole.mutex);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->print("No CLI output yet");
    return;
  }
  
  // Find current selection by timestamp (survives buffer shifts)
  int cliSelectedIndex = findIndexByTimestamp(cliSelectedTimestamp);
  if (cliSelectedIndex < 0) {
    // Message was evicted or not set - select newest
    cliSelectedIndex = totalLines - 1;
    cliSelectedTimestamp = gOLEDConsole.getTimestamp(cliSelectedIndex);
  }
  
  // Auto-scroll to keep selection visible
  int selectionFromEnd = totalLines - 1 - cliSelectedIndex;
  if (selectionFromEnd < cliScrollOffset) {
    cliScrollOffset = selectionFromEnd;  // Scroll down to show selection
  } else if (selectionFromEnd >= cliScrollOffset + CLI_MAX_VISIBLE_LINES) {
    cliScrollOffset = selectionFromEnd - CLI_MAX_VISIBLE_LINES + 1;  // Scroll up
  }
  
  // Clamp scroll offset
  int maxScroll = max(0, totalLines - CLI_MAX_VISIBLE_LINES);
  if (cliScrollOffset > maxScroll) cliScrollOffset = maxScroll;
  if (cliScrollOffset < 0) cliScrollOffset = 0;
  
  // Calculate visible window
  int endIdx = totalLines - cliScrollOffset;
  int startIdx = max(0, endIdx - CLI_MAX_VISIBLE_LINES);
  int visibleCount = endIdx - startIdx;
  
  // Detail popup - improved layout (in content area)
  if (cliShowingDetail && visibleCount > 0) {
    // Find locked message by timestamp (survives buffer shifts)
    int selectedIdx = findIndexByTimestamp(cliDetailLockedTimestamp);
    if (selectedIdx < 0) {
      // Message was evicted - fall back to current selection
      selectedIdx = cliSelectedIndex;
      cliDetailLockedTimestamp = gOLEDConsole.getTimestamp(selectedIdx);
    }
    
    const char* line = gOLEDConsole.getLine(selectedIdx);
    uint32_t timestamp = gOLEDConsole.getTimestamp(selectedIdx);
    
    if (line) {
      // Full screen detail view in content area
      oledDisplay->fillRect(0, OLED_CONTENT_START_Y, 128, OLED_CONTENT_HEIGHT, DISPLAY_COLOR_WHITE);
      oledDisplay->drawRect(0, OLED_CONTENT_START_Y, 128, OLED_CONTENT_HEIGHT, DISPLAY_COLOR_BLACK);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
      
      // Header: Line number
      oledDisplay->setCursor(2, OLED_CONTENT_START_Y + 2);
      oledDisplay->setTextSize(1);
      oledDisplay->printf("Line %d/%d", selectedIdx + 1, totalLines);
      
      // Timestamp (seconds since boot)
      oledDisplay->setCursor(2, OLED_CONTENT_START_Y + 12);
      oledDisplay->printf("T: %lu.%03lu s", timestamp / 1000, timestamp % 1000);
      
      // Separator line
      oledDisplay->drawFastHLine(2, OLED_CONTENT_START_Y + 22, 124, DISPLAY_COLOR_BLACK);
      
      // Content - word wrap across 2 lines (44px content area)
      int contentY = OLED_CONTENT_START_Y + 24;
      int lineHeight = 10;
      int maxLines = 2;  // 2 lines of content fit in 44px
      int charsPerLine = 20;
      
      size_t lineLen = strlen(line);
      for (int l = 0; l < maxLines && l * charsPerLine < (int)lineLen; l++) {
        oledDisplay->setCursor(2, contentY + (l * lineHeight));
        char segment[21];
        size_t start = l * charsPerLine;
        size_t end = min(start + charsPerLine, lineLen);
        strncpy(segment, line + start, end - start);
        segment[end - start] = '\0';
        oledDisplay->print(segment);
      }
      
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    xSemaphoreGive(gOLEDConsole.mutex);
    return;
  }
  
  // Normal view (in content area) - ensure lines don't overflow into footer
  int y = OLED_CONTENT_START_Y;
  const int lineHeight = 10;
  const int maxY = OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - lineHeight;
  
  for (int i = startIdx; i < endIdx && y <= maxY; i++) {
    const char* line = gOLEDConsole.getLine(i);
    if (line) {
      bool isSelected = (i == cliSelectedIndex);
      
      if (isSelected) {
        oledDisplay->setCursor(0, y);
        oledDisplay->print(">");
      }
      
      oledDisplay->setCursor(6, y);
      char truncated[21];
      strncpy(truncated, line, 20);
      truncated[20] = '\0';
      oledDisplay->print(truncated);
      y += lineHeight;
    }
  }
  
  xSemaphoreGive(gOLEDConsole.mutex);
  
  // Scroll indicators (in content area)
  if (cliScrollOffset > 0) {
    oledDisplay->setCursor(120, OLED_CONTENT_START_Y);
    oledDisplay->print("\x19");  // Down arrow (more below)
  }
  if (startIdx > 0) {
    oledDisplay->setCursor(120, OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 10);
    oledDisplay->print("\x18");  // Up arrow (more above)
  }
}

// CLI Viewer input handler
static bool handleCLIViewerInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  bool handled = false;
  
  // Acquire mutex for thread-safe buffer access
  if (!gOLEDConsole.mutex || xSemaphoreTake(gOLEDConsole.mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return false;
  }
  
  int totalLines = gOLEDConsole.getLineCount();
  if (totalLines == 0) {
    xSemaphoreGive(gOLEDConsole.mutex);
    return false;
  }
  
  // Find current selection index from timestamp
  int currentIdx = findIndexByTimestamp(cliSelectedTimestamp);
  if (currentIdx < 0) {
    currentIdx = totalLines - 1;
    cliSelectedTimestamp = gOLEDConsole.getTimestamp(currentIdx);
  }
  
  // A button = toggle detail
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (!cliShowingDetail) {
      // Entering detail view - lock the current message by its timestamp
      cliDetailLockedTimestamp = cliSelectedTimestamp;
    }
    cliShowingDetail = !cliShowingDetail;
    if (!cliShowingDetail) {
      // Exiting detail view - unlock
      cliDetailLockedTimestamp = 0;
    }
    handled = true;
  }
  
  // B button closes detail
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (cliShowingDetail) {
      cliShowingDetail = false;
      cliDetailLockedTimestamp = 0;
      handled = true;
    }
  }
  
  // In detail view, allow joystick navigation between messages using gNavEvents
  if (cliShowingDetail) {
    int lockedIdx = findIndexByTimestamp(cliDetailLockedTimestamp);
    if (lockedIdx < 0) {
      lockedIdx = currentIdx;
      cliDetailLockedTimestamp = gOLEDConsole.getTimestamp(lockedIdx);
    }
    
    if (gNavEvents.up && lockedIdx > 0) {
      cliDetailLockedTimestamp = gOLEDConsole.getTimestamp(lockedIdx - 1);
      handled = true;
    } else if (gNavEvents.down && lockedIdx < totalLines - 1) {
      cliDetailLockedTimestamp = gOLEDConsole.getTimestamp(lockedIdx + 1);
      handled = true;
    }
    xSemaphoreGive(gOLEDConsole.mutex);
    return handled;
  }
  
  // Navigation using centralized gNavEvents (already has debounce/auto-repeat)
  if (gNavEvents.up) {
    // Move to older message (scroll up in history)
    if (currentIdx > 0) {
      cliSelectedTimestamp = gOLEDConsole.getTimestamp(currentIdx - 1);
    }
    handled = true;
  } else if (gNavEvents.down) {
    // Move to newer message (scroll down in history)
    if (currentIdx < totalLines - 1) {
      cliSelectedTimestamp = gOLEDConsole.getTimestamp(currentIdx + 1);
    }
    handled = true;
  }
  
  // X button = jump to newest
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    cliSelectedTimestamp = gOLEDConsole.getTimestamp(totalLines - 1);
    cliScrollOffset = 0;
    handled = true;
  }
  
  // Y button = jump to oldest
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
    cliSelectedTimestamp = gOLEDConsole.getTimestamp(0);  // Oldest message
    cliScrollOffset = max(0, totalLines - CLI_MAX_VISIBLE_LINES);
    handled = true;
  }
  
  xSemaphoreGive(gOLEDConsole.mutex);
  return handled;
}

// CLI availability check
static bool isCLIViewerAvailable(String* outReason) {
  if (!gOLEDConsole.mutex) {
    if (outReason) *outReason = "Console buffer not initialized";
    return false;
  }
  return true;
}

// CLI mode registration
static const OLEDModeEntry cliViewerEntry = {
  OLED_CLI_VIEWER,
  "CLI Output",
  "notify_system",
  displayCLIViewer,
  isCLIViewerAvailable,
  handleCLIViewerInput,
  true,
  92
};

static const OLEDModeEntry cliViewerModes[] = { cliViewerEntry };

// Register with unique variable name
static OLEDModeRegistrar _oled_mode_registrar_cli(cliViewerModes, sizeof(cliViewerModes) / sizeof(cliViewerModes[0]), "CLIViewer");

#endif // ENABLE_OLED_DISPLAY
