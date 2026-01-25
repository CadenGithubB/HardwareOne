#include "OLED_Utils.h"
#include "OLED_Display.h"
#include "Optional_Bluetooth.h"
#include "System_Battery.h"
#include "System_BuildConfig.h"
#include "System_Command.h"

// Forward declaration for memory stats display
void displayMemoryStats();

#if !ENABLE_OLED_DISPLAY
bool oledBootModeActive = false;
#endif

#if ENABLE_OLED_DISPLAY

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_sleep.h>
#include <Wire.h>
#if ENABLE_GPS_SENSOR
#include <Adafruit_GPS.h>
#endif
#if ENABLE_WIFI
#include <WiFi.h>
#endif

#include "i2csensor-rda5807.h"
#include "OLED_ConsoleBuffer.h"
#include "OLED_Footer.h"
#include "OLED_SettingsEditor.h"
#include "System_Debug.h"
#include "System_FileManager.h"
#include "System_FirstTimeSetup.h"
#include "System_I2C.h"
#include "System_SensorLogging.h"
#include "System_SensorStubs.h"
#include "System_Auth.h"
#include "System_Settings.h"
#include "System_User.h"
#include "System_Utils.h"

#if ENABLE_ESPNOW
#include "OLED_ESPNow.h"
#include "System_ESPNow.h"
#endif
#if ENABLE_APDS_SENSOR
#include "i2csensor-apds9960.h"
#endif
#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor-seesaw.h"
#endif
#if ENABLE_IMU_SENSOR
#include "i2csensor-bno055.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor-mlx90640.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor-vl53l4cx.h"
#endif
#if ENABLE_RTC_SENSOR
#include "i2csensor-ds3231.h"
#endif

#if ENABLE_WIFI || ENABLE_ESPNOW
#include <esp_wifi.h>
#endif
#include <LittleFS.h>

// =============================================================================
// Modular Scrolling System Implementation
// =============================================================================

void oledScrollInit(OLEDScrollState* state, const char* title, int visibleLines) {
  if (!state) return;
  
  state->itemCount = 0;
  state->selectedIndex = 0;
  state->scrollOffset = 0;
  state->visibleLines = visibleLines > 0 ? visibleLines : 4;
  state->wrapAround = true;
  state->title = title;  // Store pointer directly
  state->footer = nullptr;
  state->refreshCounter = 0;
  
  // Clear all items
  for (int i = 0; i < OLED_SCROLL_MAX_ITEMS; i++) {
    state->items[i].line1 = nullptr;
    state->items[i].line2 = nullptr;
    state->items[i].isSelectable = true;
    state->items[i].isHighlighted = false;
    state->items[i].userData = nullptr;
    state->items[i].icon = 0;
    state->items[i].validationKey = 0;
  }
}

bool oledScrollAddItem(OLEDScrollState* state, const char* line1, const char* line2, 
                       bool selectable, void* userData) {
  if (!state || state->itemCount >= OLED_SCROLL_MAX_ITEMS) return false;
  
  int idx = state->itemCount;
  state->items[idx].line1 = line1;  // Store pointer directly (no copy)
  state->items[idx].line2 = line2;  // Store pointer directly (no copy)
  state->items[idx].isSelectable = selectable;
  state->items[idx].isHighlighted = false;
  state->items[idx].userData = userData;
  state->items[idx].icon = 0;
  state->items[idx].validationKey = state->refreshCounter;  // Mark with current refresh cycle
  
  state->itemCount++;
  return true;
}

void oledScrollClear(OLEDScrollState* state) {
  if (!state) return;
  state->itemCount = 0;
  state->selectedIndex = 0;
  state->scrollOffset = 0;
  state->refreshCounter++;  // Increment to invalidate old pointers
}

void oledScrollUp(OLEDScrollState* state) {
  if (!state || state->itemCount == 0) return;
  
  if (state->selectedIndex > 0) {
    state->selectedIndex--;
  } else if (state->wrapAround) {
    state->selectedIndex = state->itemCount - 1;
  }
  
  // Adjust scroll offset if selection moved above visible area
  if (state->selectedIndex < state->scrollOffset) {
    state->scrollOffset = state->selectedIndex;
  }
  
  // Adjust scroll offset if selection moved below visible area (wrap case)
  if (state->wrapAround && state->selectedIndex == state->itemCount - 1) {
    state->scrollOffset = max(0, state->itemCount - state->visibleLines);
  }
}

void oledScrollDown(OLEDScrollState* state) {
  if (!state || state->itemCount == 0) return;
  
  if (state->selectedIndex < state->itemCount - 1) {
    state->selectedIndex++;
  } else if (state->wrapAround) {
    state->selectedIndex = 0;
  }
  
  // Adjust scroll offset if selection moved below visible area
  if (state->selectedIndex >= state->scrollOffset + state->visibleLines) {
    state->scrollOffset = state->selectedIndex - state->visibleLines + 1;
  }
  
  // Adjust scroll offset if selection moved above visible area (wrap case)
  if (state->wrapAround && state->selectedIndex == 0) {
    state->scrollOffset = 0;
  }
}

void oledScrollPageUp(OLEDScrollState* state) {
  if (!state || state->itemCount == 0) return;
  
  state->selectedIndex = max(0, state->selectedIndex - state->visibleLines);
  state->scrollOffset = max(0, state->scrollOffset - state->visibleLines);
}

void oledScrollPageDown(OLEDScrollState* state) {
  if (!state || state->itemCount == 0) return;
  
  state->selectedIndex = min(state->itemCount - 1, state->selectedIndex + state->visibleLines);
  state->scrollOffset = min(max(0, state->itemCount - state->visibleLines), 
                            state->scrollOffset + state->visibleLines);
}

OLEDScrollItem* oledScrollGetSelected(OLEDScrollState* state) {
  if (!state || state->itemCount == 0) return nullptr;
  if (state->selectedIndex < 0 || state->selectedIndex >= state->itemCount) return nullptr;
  return &state->items[state->selectedIndex];
}

OLEDScrollItem* oledScrollGetItem(OLEDScrollState* state, int index) {
  if (!state || index < 0 || index >= state->itemCount) return nullptr;
  return &state->items[index];
}

int oledScrollCalculateVisibleLines(int displayHeight, int textSize, bool hasTitle, bool hasFooter) {
  int lineHeight = 8 * textSize;  // 8 pixels per line for size 1
  // Use content area height instead of full display height (reserves space for global footer)
  int availableHeight = OLED_CONTENT_HEIGHT;
  
  if (hasTitle) availableHeight -= lineHeight + 2;  // Title + spacing
  if (hasFooter) availableHeight -= lineHeight;  // Mode-specific footer (deprecated, use global footer)
  
  // Each list item takes 2 lines (line1 + line2)
  int itemHeight = lineHeight * 2;
  return max(1, availableHeight / itemHeight);
}

void oledScrollRender(Adafruit_SSD1306* display, OLEDScrollState* state, 
                      bool showScrollbar, bool showSelection) {
  if (!display || !state) return;
  
  int yPos = 0;
  int lineHeight = 8;  // For text size 1
  
  // Draw title if present
  if (state->title && state->title[0] != '\0') {
    display->setTextSize(1);
    display->setCursor(0, yPos);
    display->print(state->title);
    yPos += lineHeight + 2;  // Title + spacing
  }
  
  // Calculate visible range
  int visibleStart = state->scrollOffset;
  int visibleEnd = min(state->itemCount, state->scrollOffset + state->visibleLines);
  
  // Draw visible items
  for (int i = visibleStart; i < visibleEnd; i++) {
    OLEDScrollItem* item = &state->items[i];
    bool isSelected = (i == state->selectedIndex);
    
    // Draw selection indicator
    if (showSelection && isSelected) {
      display->fillRect(0, yPos, 3, lineHeight * 2, DISPLAY_COLOR_WHITE);
      display->setCursor(5, yPos);
    } else {
      display->setCursor(0, yPos);
    }
    
    // Draw line1 (primary text)
    display->setTextSize(1);
    if (showSelection && isSelected) {
      display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
    } else {
      display->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    // Draw line1 with null safety and truncation
    if (item->line1 && item->line1[0] != '\0') {
      int len = strlen(item->line1);
      if (len > 20) {
        // Truncate long strings
        char truncated[22];
        strncpy(truncated, item->line1, 19);
        truncated[19] = '~';
        truncated[20] = '\0';
        display->println(truncated);
      } else {
        display->println(item->line1);
      }
    } else {
      display->println("---");  // Fallback for null/empty
    }
    
    // Draw line2 (secondary text)
    yPos += lineHeight;
    if (showSelection && isSelected) {
      display->setCursor(5, yPos);
    } else {
      display->setCursor(0, yPos);
    }
    
    display->setTextColor(DISPLAY_COLOR_WHITE);  // Always white for line2
    if (item->line2 && item->line2[0] != '\0') {
      int len = strlen(item->line2);
      if (len > 20) {
        // Truncate long strings
        char truncated[22];
        strncpy(truncated, item->line2, 19);
        truncated[19] = '~';
        truncated[20] = '\0';
        display->println(truncated);
      } else {
        display->println(item->line2);
      }
    } else {
      display->println("");  // Empty line for null/empty
    }
    
    yPos += lineHeight;
  }
  
  // Draw scrollbar if needed (constrained to content area)
  if (showScrollbar && state->itemCount > state->visibleLines) {
    int scrollbarX = SCREEN_WIDTH - 1;
    bool hasTitle = (state->title && state->title[0] != '\0');
    int scrollbarHeight = OLED_CONTENT_HEIGHT - (hasTitle ? 10 : 0);
    int scrollbarY = hasTitle ? 10 : 0;
    
    // Draw scrollbar track
    display->drawFastVLine(scrollbarX, scrollbarY, scrollbarHeight, DISPLAY_COLOR_WHITE);
    
    // Calculate thumb position and size
    int thumbHeight = max(4, (scrollbarHeight * state->visibleLines) / state->itemCount);
    int thumbY = scrollbarY + (scrollbarHeight - thumbHeight) * state->scrollOffset / 
                 max(1, state->itemCount - state->visibleLines);
    
    // Draw scrollbar thumb
    display->fillRect(scrollbarX - 1, thumbY, 3, thumbHeight, DISPLAY_COLOR_WHITE);
  }
  
  // Note: Mode-specific footer removed - global footer now handles all button hints
}

// =============================================================================
// Virtual Keyboard Implementation
// =============================================================================

#include "System_Utils.h"

#if ENABLE_GAMEPAD_SENSOR
  #include "i2csensor-seesaw.h"  // For JOYSTICK_DEADZONE
#endif

// Character grid layouts - three modes (3 rows each)
// Uppercase letters ONLY (10 columns x 3 rows)
const char OLED_KEYBOARD_CHARS_UPPER[OLED_KEYBOARD_ROWS][OLED_KEYBOARD_COLS] = {
  {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J'},  // Row 0
  {'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T'},  // Row 1
  {'U', 'V', 'W', 'X', 'Y', 'Z', '.', ' ', '\b', '\t'}  // Row 2 (dot, space, DEL, MODE)
};

// Lowercase letters ONLY (10 columns x 3 rows)
const char OLED_KEYBOARD_CHARS_LOWER[OLED_KEYBOARD_ROWS][OLED_KEYBOARD_COLS] = {
  {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'},  // Row 0
  {'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't'},  // Row 1
  {'u', 'v', 'w', 'x', 'y', 'z', '.', ' ', '\b', '\t'}  // Row 2 (dot, space, DEL, MODE)
};

// Numbers and symbols ONLY (10 columns x 3 rows)
const char OLED_KEYBOARD_CHARS_NUMBERS[OLED_KEYBOARD_ROWS][OLED_KEYBOARD_COLS] = {
  {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'},  // Row 0
  {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'},  // Row 1
  {'-', '_', '=', '+', '[', ']', '{', '}', ' ', '\t'}  // Row 2 (space at 8, MODE at 9)
};

// Special character indicators
#define CHAR_SPACE ' '
#define CHAR_DONE '\n'   // Newline represents DONE
#define CHAR_MODE '\t'   // Tab represents MODE toggle
#define CHAR_BACK '\b'   // Not in grid, triggered by B button

// Helper to get character at position based on current mode
static char getCharAt(int row, int col) {
  switch (gOLEDKeyboardState.mode) {
    case KEYBOARD_MODE_UPPERCASE: return OLED_KEYBOARD_CHARS_UPPER[row][col];
    case KEYBOARD_MODE_LOWERCASE: return OLED_KEYBOARD_CHARS_LOWER[row][col];
    case KEYBOARD_MODE_NUMBERS: return OLED_KEYBOARD_CHARS_NUMBERS[row][col];
    default: return OLED_KEYBOARD_CHARS_UPPER[row][col];
  }
}

// Global keyboard state
OLEDKeyboardState gOLEDKeyboardState;

void oledKeyboardInit(const char* title, const char* initialText, int maxLength) {
  memset(gOLEDKeyboardState.text, 0, sizeof(gOLEDKeyboardState.text));
  gOLEDKeyboardState.textLength = 0;
  gOLEDKeyboardState.cursorX = 0;
  gOLEDKeyboardState.cursorY = 0;
  gOLEDKeyboardState.mode = KEYBOARD_MODE_LOWERCASE;  // Start with lowercase
  gOLEDKeyboardState.active = true;
  gOLEDKeyboardState.cancelled = false;
  gOLEDKeyboardState.completed = false;
  gOLEDKeyboardState.title = title ? String(title) : "Enter Text:";
  gOLEDKeyboardState.maxLength = min(maxLength, OLED_KEYBOARD_MAX_LENGTH);
  
  // Initialize autocomplete state
  gOLEDKeyboardState.autocompleteFunc = nullptr;
  gOLEDKeyboardState.autocompleteUserData = nullptr;
  gOLEDKeyboardState.showingSuggestions = false;
  gOLEDKeyboardState.suggestionCount = 0;
  gOLEDKeyboardState.selectedSuggestion = 0;
  memset(gOLEDKeyboardState.suggestions, 0, sizeof(gOLEDKeyboardState.suggestions));
  
  // Copy initial text if provided
  if (initialText && strlen(initialText) > 0) {
    strncpy(gOLEDKeyboardState.text, initialText, gOLEDKeyboardState.maxLength);
    gOLEDKeyboardState.textLength = strlen(gOLEDKeyboardState.text);
  }
}

void oledKeyboardReset() {
  gOLEDKeyboardState.active = false;
  gOLEDKeyboardState.cancelled = false;
  gOLEDKeyboardState.completed = false;
  memset(gOLEDKeyboardState.text, 0, sizeof(gOLEDKeyboardState.text));
  gOLEDKeyboardState.textLength = 0;
}

void oledKeyboardDisplay(Adafruit_SSD1306* display) {
  if (!display) return;
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  
  // If showing suggestions, render suggestion list instead of keyboard
  if (gOLEDKeyboardState.showingSuggestions && gOLEDKeyboardState.suggestionCount > 0) {
    // Title
    display->setCursor(0, 0);
    display->print("Suggestions:");
    
    // Show current input
    display->setCursor(75, 0);
    char inputPreview[10];
    strncpy(inputPreview, gOLEDKeyboardState.text, 8);
    inputPreview[8] = '\0';
    display->print(inputPreview);
    
    // List suggestions (up to 5 visible)
    int visibleCount = min(gOLEDKeyboardState.suggestionCount, 5);
    int startIdx = 0;
    if (gOLEDKeyboardState.selectedSuggestion >= 5) {
      startIdx = gOLEDKeyboardState.selectedSuggestion - 4;
    }
    
    for (int i = 0; i < visibleCount && (startIdx + i) < gOLEDKeyboardState.suggestionCount; i++) {
      int idx = startIdx + i;
      int y = 10 + i * 10;
      
      bool isSelected = (idx == gOLEDKeyboardState.selectedSuggestion);
      
      if (isSelected) {
        display->fillRect(0, y - 1, 128, 10, DISPLAY_COLOR_WHITE);
        display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
      } else {
        display->setTextColor(DISPLAY_COLOR_WHITE);
      }
      
      display->setCursor(2, y);
      const char* suggestion = gOLEDKeyboardState.suggestions[idx];
      if (suggestion) {
        // Truncate long names
        char truncated[22];
        strncpy(truncated, suggestion, 21);
        truncated[21] = '\0';
        display->print(truncated);
      }
      
      display->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    // Footer
    display->drawFastHLine(0, 54, 128, DISPLAY_COLOR_WHITE);
    display->setCursor(0, 56);
    display->print("A:Pick B:Back ^v:Nav");
    return;
  }
  
  // Normal keyboard display
  // Draw title with mode indicator
  display->setCursor(0, 0);
  display->print(gOLEDKeyboardState.title);
  
  // Show mode indicator at right edge (compact format)
  const char* modeStr = "";
  switch (gOLEDKeyboardState.mode) {
    case KEYBOARD_MODE_UPPERCASE: modeStr = "ABC"; break;
    case KEYBOARD_MODE_LOWERCASE: modeStr = "abc"; break;
    case KEYBOARD_MODE_NUMBERS: modeStr = "123"; break;
    case KEYBOARD_MODE_COUNT: break; // Should never happen
  }
  // Right-align mode indicator (3 chars + padding)
  display->setCursor(128 - (strlen(modeStr) * 6), 0);
  display->print(modeStr);
  
  // Draw text preview box
  display->drawRect(0, 9, 128, 11, DISPLAY_COLOR_WHITE);
  display->setCursor(2, 11);
  
  // Show current text with cursor
  String displayText = String(gOLEDKeyboardState.text);
  if (displayText.length() > 20) {
    // Scroll text if too long
    displayText = displayText.substring(displayText.length() - 20);
  }
  display->print(displayText);
  
  // Show blinking cursor
  if ((millis() / 500) % 2 == 0) {
    display->print("_");
  }
  
  // Draw character grid (starting at y=22, 3 rows only)
  int startY = 22;
  int charWidth = 12;   // Width per character cell
  int charHeight = 10;  // Height per character row
  
  for (int row = 0; row < OLED_KEYBOARD_ROWS; row++) {
    for (int col = 0; col < OLED_KEYBOARD_COLS; col++) {
      int x = col * charWidth + 2;
      int y = startY + row * charHeight;
      
      char c = getCharAt(row, col);
      
      // Highlight current cursor position
      bool isCursor = (col == gOLEDKeyboardState.cursorX && row == gOLEDKeyboardState.cursorY);
      
      if (isCursor) {
        // Draw filled rectangle for cursor
        display->fillRect(x - 1, y - 1, charWidth - 2, charHeight - 1, DISPLAY_COLOR_WHITE);
        display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
      } else {
        display->setTextColor(DISPLAY_COLOR_WHITE);
      }
      
      display->setCursor(x + 2, y);
      
      // Display special characters with labels (must fit in 12px cell)
      if (c == CHAR_SPACE) {
        display->print("_");  // Space (underscore visual)
      } else if (c == CHAR_BACK) {
        display->print("<");  // Backspace arrow
      } else if (c == CHAR_MODE) {
        display->print("*");  // Mode toggle (asterisk)
      } else {
        display->print(c);
      }
      
      // Reset text color
      display->setTextColor(DISPLAY_COLOR_WHITE);
    }
  }
  
  // Draw footer with button hints (show SEL if autocomplete available)
  display->drawFastHLine(0, 54, 128, DISPLAY_COLOR_WHITE);
  display->setCursor(0, 56);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  if (gOLEDKeyboardState.autocompleteFunc) {
    display->print("A:Sel SEL:? ST:Done");
  } else {
    display->print("A:Sel Y:Del ST:Done");
  }
}

bool oledKeyboardHandleInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (!gOLEDKeyboardState.active) {
    // Reset state when keyboard becomes inactive
    return false;
  }
  
  bool inputHandled = false;
  
  // Handle suggestion mode differently
  if (gOLEDKeyboardState.showingSuggestions) {
    // Y-axis navigates suggestions
    if (abs(deltaY) > JOYSTICK_DEADZONE) {
      static unsigned long lastSuggMove = 0;
      if (millis() - lastSuggMove > 150) {
        if (deltaY > 0 && gOLEDKeyboardState.selectedSuggestion < gOLEDKeyboardState.suggestionCount - 1) {
          gOLEDKeyboardState.selectedSuggestion++;
          lastSuggMove = millis();
          inputHandled = true;
        } else if (deltaY < 0 && gOLEDKeyboardState.selectedSuggestion > 0) {
          gOLEDKeyboardState.selectedSuggestion--;
          lastSuggMove = millis();
          inputHandled = true;
        }
      }
    }
    
    // A button selects suggestion
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      Serial.println("[KEYBOARD] A button - selecting suggestion");
      oledKeyboardSelectSuggestion();
      inputHandled = true;
    }
    
    // B button dismisses suggestions
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      Serial.println("[KEYBOARD] B button - dismissing suggestions");
      oledKeyboardDismissSuggestions();
      inputHandled = true;
    }
    
    return inputHandled;
  }
  
  // Normal keyboard mode
  // Auto-repeat timing for keyboard navigation (more responsive than menu latching)
  static unsigned long lastMoveTimeX = 0;
  static unsigned long lastMoveTimeY = 0;
  static bool wasDeflectedX = false;
  static bool wasDeflectedY = false;
  
  const unsigned long INITIAL_DELAY_MS = 250;  // Delay before auto-repeat starts
  const unsigned long REPEAT_DELAY_MS = 80;    // Delay between repeated movements
  
  unsigned long now = millis();
  
  // X-axis movement with auto-repeat
  bool deflectedX = abs(deltaX) > JOYSTICK_DEADZONE;
  if (!deflectedX) {
    // Joystick returned to center - reset state
    wasDeflectedX = false;
    lastMoveTimeX = 0;
  } else {
    // Joystick is deflected
    bool shouldMove = false;
    
    if (!wasDeflectedX) {
      // First deflection - move immediately
      shouldMove = true;
      wasDeflectedX = true;
      lastMoveTimeX = now;
    } else {
      // Held deflection - check for auto-repeat
      unsigned long elapsed = now - lastMoveTimeX;
      unsigned long threshold = (lastMoveTimeX == 0) ? INITIAL_DELAY_MS : 
                                (elapsed > INITIAL_DELAY_MS) ? REPEAT_DELAY_MS : INITIAL_DELAY_MS;
      if (elapsed >= threshold) {
        shouldMove = true;
        lastMoveTimeX = now;
      }
    }
    
    if (shouldMove) {
      if (deltaX > 0) {
        oledKeyboardMoveRight();
      } else {
        oledKeyboardMoveLeft();
      }
      inputHandled = true;
    }
  }
  
  // Y-axis movement with auto-repeat
  bool deflectedY = abs(deltaY) > JOYSTICK_DEADZONE;
  if (!deflectedY) {
    // Joystick returned to center - reset state
    wasDeflectedY = false;
    lastMoveTimeY = 0;
  } else {
    // Joystick is deflected
    bool shouldMove = false;
    
    if (!wasDeflectedY) {
      // First deflection - move immediately
      shouldMove = true;
      wasDeflectedY = true;
      lastMoveTimeY = now;
    } else {
      // Held deflection - check for auto-repeat
      unsigned long elapsed = now - lastMoveTimeY;
      unsigned long threshold = (lastMoveTimeY == 0) ? INITIAL_DELAY_MS : 
                                (elapsed > INITIAL_DELAY_MS) ? REPEAT_DELAY_MS : INITIAL_DELAY_MS;
      if (elapsed >= threshold) {
        shouldMove = true;
        lastMoveTimeY = now;
      }
    }
    
    if (shouldMove) {
      if (deltaY > 0) {
        oledKeyboardMoveDown();
      } else {
        oledKeyboardMoveUp();
      }
      inputHandled = true;
    }
  }
  
  // Button actions using input abstraction
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    Serial.println("[KEYBOARD] A button pressed - selecting char");
    oledKeyboardSelectChar();
    inputHandled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
    Serial.printf("[KEYBOARD] Y button pressed - backspace (textLen=%d)\n", gOLEDKeyboardState.textLength);
    oledKeyboardBackspace();
    Serial.printf("[KEYBOARD] After backspace: textLen=%d text='%s'\n", 
                  gOLEDKeyboardState.textLength, gOLEDKeyboardState.text);
    inputHandled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    Serial.println("[KEYBOARD] B button pressed - cancel");
    oledKeyboardCancel();
    inputHandled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_START)) {
    Serial.println("[KEYBOARD] X/START button pressed - complete");
    oledKeyboardComplete();
    inputHandled = true;
  }
  
  // SELECT button triggers autocomplete (if provider is set)
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_SELECT)) {
    if (gOLEDKeyboardState.autocompleteFunc) {
      Serial.println("[KEYBOARD] SELECT button pressed - triggering autocomplete");
      oledKeyboardTriggerAutocomplete();
      inputHandled = true;
    }
  }
  
  // Only log when something actually happened (button edge or joystick move
  // that resulted in an action). This avoids spamming logs every frame when
  // the keyboard is idle.
  if (inputHandled) {
    Serial.printf("[KEYBOARD] HANDLED: dX=%d dY=%d newly=0x%08lX textLen=%d\n", 
                  deltaX, deltaY, (unsigned long)newlyPressed, gOLEDKeyboardState.textLength);
    static bool sLoggedMasks = false;
    if (!sLoggedMasks) {
      Serial.printf("[KEYBOARD] Button masks: A=0x%08lX B=0x%08lX X=0x%08lX Y=0x%08lX START=0x%08lX SEL=0x%08lX\n",
                    (unsigned long)INPUT_MASK(INPUT_BUTTON_A), (unsigned long)INPUT_MASK(INPUT_BUTTON_B), 
                    (unsigned long)INPUT_MASK(INPUT_BUTTON_X), (unsigned long)INPUT_MASK(INPUT_BUTTON_Y), 
                    (unsigned long)INPUT_MASK(INPUT_BUTTON_START), (unsigned long)INPUT_MASK(INPUT_BUTTON_SELECT));
      sLoggedMasks = true;
    }
  }
  
  return inputHandled;
}

const char* oledKeyboardGetText() {
  return gOLEDKeyboardState.text;
}

bool oledKeyboardIsActive() {
  return gOLEDKeyboardState.active;
}

bool oledKeyboardIsCompleted() {
  return gOLEDKeyboardState.completed;
}

bool oledKeyboardIsCancelled() {
  return gOLEDKeyboardState.cancelled;
}

void oledKeyboardMoveUp() {
  if (gOLEDKeyboardState.cursorY > 0) {
    gOLEDKeyboardState.cursorY--;
  } else {
    gOLEDKeyboardState.cursorY = OLED_KEYBOARD_ROWS - 1;  // Wrap to bottom
  }
}

void oledKeyboardMoveDown() {
  if (gOLEDKeyboardState.cursorY < OLED_KEYBOARD_ROWS - 1) {
    gOLEDKeyboardState.cursorY++;
  } else {
    gOLEDKeyboardState.cursorY = 0;  // Wrap to top
  }
}

void oledKeyboardMoveLeft() {
  if (gOLEDKeyboardState.cursorX > 0) {
    gOLEDKeyboardState.cursorX--;
  } else {
    gOLEDKeyboardState.cursorX = OLED_KEYBOARD_COLS - 1;  // Wrap to right
  }
}

void oledKeyboardMoveRight() {
  if (gOLEDKeyboardState.cursorX < OLED_KEYBOARD_COLS - 1) {
    gOLEDKeyboardState.cursorX++;
  } else {
    gOLEDKeyboardState.cursorX = 0;  // Wrap to left
  }
}

void oledKeyboardSelectChar() {
  // Get character at current cursor position
  char selectedChar = getCharAt(gOLEDKeyboardState.cursorY, gOLEDKeyboardState.cursorX);
  
  Serial.printf("[KEYBOARD_SELECT] Cursor at [%d,%d] char='%c' (0x%02X)\n", 
                gOLEDKeyboardState.cursorX, gOLEDKeyboardState.cursorY, 
                selectedChar, (unsigned char)selectedChar);
  
  // Handle special characters
  if (selectedChar == CHAR_MODE) {
    // Toggle keyboard mode
    Serial.println("[KEYBOARD_SELECT] Mode toggle selected");
    oledKeyboardToggleMode();
    return;
  } else if (selectedChar == CHAR_BACK) {
    // Backspace
    Serial.println("[KEYBOARD_SELECT] DEL button selected");
    oledKeyboardBackspace();
    return;
  }
  
  // Add character if not at max length
  if (gOLEDKeyboardState.textLength < gOLEDKeyboardState.maxLength) {
    gOLEDKeyboardState.text[gOLEDKeyboardState.textLength] = selectedChar;
    gOLEDKeyboardState.textLength++;
    gOLEDKeyboardState.text[gOLEDKeyboardState.textLength] = '\0';
    Serial.printf("[KEYBOARD_SELECT] Added char: textLength=%d text='%s'\n", 
                  gOLEDKeyboardState.textLength, gOLEDKeyboardState.text);
  } else {
    Serial.printf("[KEYBOARD_SELECT] At max length (%d), cannot add char\n", 
                  gOLEDKeyboardState.maxLength);
  }
}

void oledKeyboardBackspace() {
  Serial.printf("[KEYBOARD_BACKSPACE] Called: textLength=%d text='%s'\n", 
                gOLEDKeyboardState.textLength, gOLEDKeyboardState.text);
  
  if (gOLEDKeyboardState.textLength > 0) {
    gOLEDKeyboardState.textLength--;
    gOLEDKeyboardState.text[gOLEDKeyboardState.textLength] = '\0';
    Serial.printf("[KEYBOARD_BACKSPACE] Deleted char: new textLength=%d text='%s'\n", 
                  gOLEDKeyboardState.textLength, gOLEDKeyboardState.text);
  } else {
    Serial.println("[KEYBOARD_BACKSPACE] No characters to delete (textLength=0)");
  }
}

void oledKeyboardComplete() {
  gOLEDKeyboardState.completed = true;
  gOLEDKeyboardState.active = false;
}

void oledKeyboardCancel() {
  gOLEDKeyboardState.cancelled = true;
  gOLEDKeyboardState.active = false;
  Serial.println("[KEYBOARD] Cancelled");
}

void oledKeyboardToggleMode() {
  // Cycle through modes: lowercase -> uppercase -> numbers -> lowercase
  gOLEDKeyboardState.mode = (OLEDKeyboardMode)((gOLEDKeyboardState.mode + 1) % KEYBOARD_MODE_COUNT);
  
  const char* modeName = "unknown";
  switch (gOLEDKeyboardState.mode) {
    case KEYBOARD_MODE_UPPERCASE: modeName = "UPPERCASE"; break;
    case KEYBOARD_MODE_LOWERCASE: modeName = "lowercase"; break;
    case KEYBOARD_MODE_NUMBERS: modeName = "123/symbols"; break;
    case KEYBOARD_MODE_COUNT: break; // Should never happen
  }
  
  Serial.printf("[KEYBOARD] Mode changed to: %s\n", modeName);
}

// ============================================================================
// Autocomplete Support (Select button triggers suggestions)
// ============================================================================

void oledKeyboardSetAutocomplete(OLEDKeyboardAutocompleteFunc func, void* userData) {
  gOLEDKeyboardState.autocompleteFunc = func;
  gOLEDKeyboardState.autocompleteUserData = userData;
  Serial.printf("[KEYBOARD] Autocomplete provider %s\n", func ? "set" : "cleared");
}

void oledKeyboardTriggerAutocomplete() {
  if (!gOLEDKeyboardState.autocompleteFunc) {
    Serial.println("[KEYBOARD] No autocomplete provider set");
    return;
  }
  
  // Call the autocomplete provider
  gOLEDKeyboardState.suggestionCount = gOLEDKeyboardState.autocompleteFunc(
    gOLEDKeyboardState.text,
    gOLEDKeyboardState.suggestions,
    OLED_KEYBOARD_MAX_SUGGESTIONS,
    gOLEDKeyboardState.autocompleteUserData
  );
  
  if (gOLEDKeyboardState.suggestionCount > 0) {
    gOLEDKeyboardState.showingSuggestions = true;
    gOLEDKeyboardState.selectedSuggestion = 0;
    Serial.printf("[KEYBOARD] Autocomplete found %d suggestions for '%s'\n", 
                  gOLEDKeyboardState.suggestionCount, gOLEDKeyboardState.text);
  } else {
    Serial.printf("[KEYBOARD] No suggestions found for '%s'\n", gOLEDKeyboardState.text);
  }
}

void oledKeyboardSelectSuggestion() {
  if (!gOLEDKeyboardState.showingSuggestions || gOLEDKeyboardState.suggestionCount == 0) {
    return;
  }
  
  const char* selected = gOLEDKeyboardState.suggestions[gOLEDKeyboardState.selectedSuggestion];
  if (selected) {
    // Copy the selected suggestion to the text field
    strncpy(gOLEDKeyboardState.text, selected, gOLEDKeyboardState.maxLength);
    gOLEDKeyboardState.text[gOLEDKeyboardState.maxLength] = '\0';
    gOLEDKeyboardState.textLength = strlen(gOLEDKeyboardState.text);
    Serial.printf("[KEYBOARD] Selected suggestion: '%s'\n", selected);
  }
  
  oledKeyboardDismissSuggestions();
}

void oledKeyboardDismissSuggestions() {
  gOLEDKeyboardState.showingSuggestions = false;
  gOLEDKeyboardState.suggestionCount = 0;
  gOLEDKeyboardState.selectedSuggestion = 0;
}

bool oledKeyboardShowingSuggestions() {
  return gOLEDKeyboardState.showingSuggestions;
}

struct OLEDConfirmState {
  bool active;
  const char* line1;
  const char* line2;
  bool selectYes;
  OLEDConfirmCallback onYes;
  void* userData;
};

static OLEDConfirmState gOLEDConfirmState = {false, nullptr, nullptr, true, nullptr, nullptr};

bool oledConfirmRequest(const char* line1, const char* line2, OLEDConfirmCallback onYes, void* userData, bool defaultYes) {
  if (gOLEDConfirmState.active) return false;
  gOLEDConfirmState.active = true;
  gOLEDConfirmState.line1 = line1;
  gOLEDConfirmState.line2 = line2;
  gOLEDConfirmState.selectYes = defaultYes;
  gOLEDConfirmState.onYes = onYes;
  gOLEDConfirmState.userData = userData;

  Serial.printf("[OLED_CONFIRM] %s%s%s\n",
                line1 ? line1 : "",
                (line1 && line2) ? " | " : "",
                line2 ? line2 : "");
  Serial.println("[OLED_CONFIRM] Use UP/DOWN to select, A to confirm, B to cancel");
  oledMarkDirty();
  return true;
}

bool oledConfirmIsActive() {
  return gOLEDConfirmState.active;
}

static void oledConfirmClose(bool confirmed) {
  if (!gOLEDConfirmState.active) return;
  Serial.printf("[OLED_CONFIRM] %s\n", confirmed ? "CONFIRMED" : "CANCELLED");
  gOLEDConfirmState.active = false;
  gOLEDConfirmState.line1 = nullptr;
  gOLEDConfirmState.line2 = nullptr;
  gOLEDConfirmState.selectYes = true;
  gOLEDConfirmState.onYes = nullptr;
  gOLEDConfirmState.userData = nullptr;
  oledMarkDirty();
}

static bool oledConfirmHandleInput(uint32_t newlyPressed) {
  if (!gOLEDConfirmState.active) return false;

  bool handled = false;

  if (gNavEvents.up) {
    gOLEDConfirmState.selectYes = true;
    oledMarkDirty();
    handled = true;
  } else if (gNavEvents.down) {
    gOLEDConfirmState.selectYes = false;
    oledMarkDirty();
    handled = true;
  } else if (gNavEvents.left || gNavEvents.right) {
    gOLEDConfirmState.selectYes = !gOLEDConfirmState.selectYes;
    oledMarkDirty();
    handled = true;
  }

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (gOLEDConfirmState.selectYes) {
      if (gOLEDConfirmState.onYes) {
        gOLEDConfirmState.onYes(gOLEDConfirmState.userData);
      }
      oledConfirmClose(true);
    } else {
      oledConfirmClose(false);
    }
    handled = true;
  } else if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    oledConfirmClose(false);
    handled = true;
  }

  return handled;
}

static void oledConfirmRender() {
  if (!gOLEDConfirmState.active || !oledDisplay) return;

  const int boxX = 2;
  const int boxY = 2;
  const int boxW = SCREEN_WIDTH - 4;
  const int boxH = OLED_CONTENT_HEIGHT - 4;

  oledDisplay->fillRect(boxX, boxY, boxW, boxH, DISPLAY_COLOR_BLACK);
  oledDisplay->drawRect(boxX, boxY, boxW, boxH, DISPLAY_COLOR_WHITE);

  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(boxX + 4, boxY + 4);
  oledDisplay->print("CONFIRM");

  int y = boxY + 14;
  if (gOLEDConfirmState.line1) {
    oledDisplay->setCursor(boxX + 4, y);
    oledDisplay->print(gOLEDConfirmState.line1);
    y += 10;
  }
  if (gOLEDConfirmState.line2) {
    oledDisplay->setCursor(boxX + 4, y);
    oledDisplay->print(gOLEDConfirmState.line2);
    y += 10;
  }

  int optY = boxY + boxH - 18;
  const int optX = boxX + 6;
  const int optW = boxW - 12;
  const int optH = 9;

  if (gOLEDConfirmState.selectYes) {
    oledDisplay->fillRect(optX, optY, optW, optH, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
  } else {
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  }
  oledDisplay->setCursor(optX + 2, optY + 1);
  oledDisplay->print("Yes");

  if (!gOLEDConfirmState.selectYes) {
    oledDisplay->fillRect(optX, optY + 10, optW, optH, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
  } else {
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  }
  oledDisplay->setCursor(optX + 2, optY + 11);
  oledDisplay->print("No");

  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
}

// ============================================================================
// OLED Console Buffer (merged from oled_console_buffer.cpp)
// ============================================================================

#include "OLED_ConsoleBuffer.h"
#include "System_Debug.h"  // For DEBUG_SYSTEMF, ERROR_SYSTEMF

// Global instance
OLEDConsoleBuffer gOLEDConsole;

// Constructor
OLEDConsoleBuffer::OLEDConsoleBuffer() 
  : head(0), count(0), mutex(nullptr) {
  memset(lines, 0, sizeof(lines));
  memset(timestamps, 0, sizeof(timestamps));
}

// Initialize buffer and mutex
void OLEDConsoleBuffer::init() {
  head = 0;
  count = 0;
  memset(lines, 0, sizeof(lines));
  memset(timestamps, 0, sizeof(timestamps));
  
  if (!mutex) {
    mutex = xSemaphoreCreateMutex();
    if (mutex) {
      DEBUG_SYSTEMF("OLED console buffer initialized (%d lines × %d chars = %d bytes)",
                    OLED_CONSOLE_LINES, OLED_CONSOLE_LINE_LEN,
                    OLED_CONSOLE_LINES * OLED_CONSOLE_LINE_LEN);
    } else {
      ERROR_SYSTEMF("Failed to create OLED console buffer mutex");
    }
  }
}

// Append a line to the ring buffer (filters non-ASCII for OLED display)
void OLEDConsoleBuffer::append(const char* text, uint32_t timestamp) {
  if (!text || !mutex) return;
  
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    // Copy text, filtering non-ASCII characters (OLED font only supports ASCII)
    char* dst = lines[head];
    const char* src = text;
    int dstIdx = 0;
    
    while (*src && dstIdx < OLED_CONSOLE_LINE_LEN - 1) {
      unsigned char c = (unsigned char)*src;
      
      if (c >= 32 && c < 127) {
        // Printable ASCII - keep as-is
        dst[dstIdx++] = c;
        src++;
      } else if (c == '\t') {
        // Tab -> space
        dst[dstIdx++] = ' ';
        src++;
      } else if (c >= 0xC0) {
        // UTF-8 multi-byte sequence start - skip entire sequence
        // Common box-drawing chars are 3-byte (0xE2...), warning symbol too
        if (c >= 0xF0) { src += 4; }       // 4-byte sequence
        else if (c >= 0xE0) { src += 3; }  // 3-byte sequence
        else { src += 2; }                  // 2-byte sequence
      } else {
        // Other non-printable or continuation byte - skip
        src++;
      }
    }
    dst[dstIdx] = '\0';
    
    // Store timestamp
    timestamps[head] = timestamp;
    
    // Advance head (circular)
    head = (head + 1) % OLED_CONSOLE_LINES;
    
    // Update count (saturate at buffer size)
    if (count < OLED_CONSOLE_LINES) {
      count++;
    }
    
    xSemaphoreGive(mutex);
  }
}

// Get number of valid lines in buffer
int OLEDConsoleBuffer::getLineCount() const {
  return count;
}

// Get line by index (0 = oldest, count-1 = newest)
const char* OLEDConsoleBuffer::getLine(int index) const {
  if (index < 0 || index >= count) {
    return nullptr;
  }
  
  // Calculate actual buffer position
  // If buffer not full: oldest is at 0
  // If buffer full: oldest is at head (just overwritten = oldest remaining)
  int bufferIndex;
  if (count < OLED_CONSOLE_LINES) {
    bufferIndex = index;
  } else {
    bufferIndex = (head + index) % OLED_CONSOLE_LINES;
  }
  
  return lines[bufferIndex];
}

// Get timestamp by index (0 = oldest, count-1 = newest)
uint32_t OLEDConsoleBuffer::getTimestamp(int index) const {
  if (index < 0 || index >= count) {
    return 0;
  }
  
  // Calculate actual buffer position (same logic as getLine)
  int bufferIndex;
  if (count < OLED_CONSOLE_LINES) {
    bufferIndex = index;
  } else {
    bufferIndex = (head + index) % OLED_CONSOLE_LINES;
  }
  
  return timestamps[bufferIndex];
}

// ============================================================================
// OLED Footer Drawing (merged from oled_footer.cpp)
// ============================================================================

#include "System_Settings.h"
#include "OLED_ESPNow.h"
#include "OLED_ConsoleBuffer.h"
#include "System_User.h"

#if ENABLE_BLUETOOTH
#include "Optional_Bluetooth.h"
#endif

#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif

// External state variables needed for context-aware footer hints
extern Adafruit_SSD1306* oledDisplay;
extern OLEDMode currentOLEDMode;
extern bool networkShowingStatus;
extern bool networkShowingWiFiSubmenu;
extern String unavailableOLEDTitle;
extern String unavailableOLEDReason;

// Forward declare FileBrowserRenderData struct to access selected item type
struct FileBrowserRenderData {
  char path[128];
  void* items;
  int itemCount;
  int selectedIdx;
  int pageStart;
  int pageEnd;
  bool valid;
  bool selectedIsFolder;
};
extern FileBrowserRenderData fileBrowserRenderData;

// Get specific action text for Bluetooth X button based on current state
static const char* getBluetoothActionText() {
#if ENABLE_BLUETOOTH
  if (!gBLEState || !gBLEState->initialized) {
    return "Start";  // Initialize Bluetooth
  } else if (gBLEState->connectionState == BLE_STATE_ADVERTISING) {
    return "Stop Adv";  // Stop advertising
  } else if (gBLEState->connectionState == BLE_STATE_IDLE) {
    return "Advertise";  // Start advertising
  } else if (gBLEState->connectionState == BLE_STATE_CONNECTED) {
    return "Disconnect";  // Disconnect current client
  }
#endif
  return "Toggle";  // Generic fallback
}

// Draw the persistent button hint footer for the current mode/state
void drawOLEDFooter() {
  if (!oledDisplay) return;
  
  // Skip footer for animations and screen off mode
  if (currentOLEDMode == OLED_ANIMATION || currentOLEDMode == OLED_OFF) {
    return;
  }
  
  const int footerY = OLED_CONTENT_HEIGHT + 2;
  
  // Draw separator line above footer (moved up 1px to avoid touching footer text)
  // For logo mode, draw shorter line with vertical box around "Back" text
  if (currentOLEDMode == OLED_LOGO) {
    // Horizontal line - only 1/3 width from left
    oledDisplay->drawFastHLine(0, OLED_CONTENT_HEIGHT - 1, SCREEN_WIDTH / 3, DISPLAY_COLOR_WHITE);
    // Vertical line down from end of horizontal line
    oledDisplay->drawFastVLine(SCREEN_WIDTH / 3, OLED_CONTENT_HEIGHT - 1, OLED_FOOTER_HEIGHT + 1, DISPLAY_COLOR_WHITE);
  } else {
    // Normal full-width separator for other modes
    oledDisplay->drawFastHLine(0, OLED_CONTENT_HEIGHT - 1, SCREEN_WIDTH, DISPLAY_COLOR_WHITE);
  }
  
  // Set text properties
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, footerY);

  if (oledConfirmIsActive()) {
    oledDisplay->print("A:Select B:Cancel");
    return;
  }
  
  // Check if keyboard is active - override mode hints with keyboard hints
  if (oledKeyboardIsActive()) {
    // Check if this is password entry (can go back) vs username (first screen, no back)
    extern OLEDKeyboardState gOLEDKeyboardState;
    bool isPasswordEntry = (gOLEDKeyboardState.title.indexOf("Password") >= 0);
    
    if (isPasswordEntry) {
      // Password screen - show back option (B:Bk to cancel and return to username)
      oledDisplay->print("A:Sel St:OK B:Back");
    } else {
      // Username/other screen - no back, B is backspace
      oledDisplay->print("A:Sel St:OK B:Del");
    }
    return;
  }
  
  // Determine button hints based on current mode and state
  const char* hints = nullptr;
  
  switch (currentOLEDMode) {
    case OLED_MENU:
      hints = "A:Select B:Back";
      break;
      
    case OLED_SENSOR_MENU:
      hints = "A:Select B:Back";
      break;
      
    case OLED_ESPNOW:
      #if ENABLE_ESPNOW
      {
        extern OLEDEspNowState gOLEDEspNowState;
        switch (gOLEDEspNowState.currentView) {
          case ESPNOW_VIEW_INIT_PROMPT:
            hints = "Y:Setup B:Back";
            break;
          case ESPNOW_VIEW_NAME_KEYBOARD:
            hints = "A:Type X:Done B:Cancel";
            break;
          case ESPNOW_VIEW_DEVICE_LIST:
            hints = "A:Open X:Broadcast B:Back";
            break;
          case ESPNOW_VIEW_DEVICE_DETAIL:
            hints = "A:Send X:Mode B:Back";
            break;
          case ESPNOW_VIEW_MODE_SELECT:
            hints = "A:Select B:Cancel";
            break;
          case ESPNOW_VIEW_BROADCAST:
            hints = "B:Back";
            break;
          case ESPNOW_VIEW_TEXT_KEYBOARD:
          case ESPNOW_VIEW_REMOTE_FORM:
            hints = "A:Type X:Done B:Cancel";
            break;
          default:
            hints = "B:Back";
            break;
        }
      }
      #else
      hints = "B:Back";
      #endif
      break;
      
    case OLED_NETWORK_INFO:
      if (networkShowingWiFiSubmenu) {
        hints = "A:Select B:Back";
      } else if (networkShowingStatus) {
        hints = "B:Back";
      } else {
        hints = "A:Select B:Back";
      }
      break;
      
    case OLED_FILE_BROWSER:
      // Show "A:Open" only for folders, just "B:Back" for files
      if (fileBrowserRenderData.valid && fileBrowserRenderData.selectedIsFolder) {
        hints = "A:Open B:Back";
      } else {
        hints = "B:Back";
      }
      break;
      
    case OLED_GAMEPAD_VISUAL:
      hints = "B:Back";
      break;
      
    case OLED_POWER:
      hints = "A:Select B:Back";
      break;
      
    case OLED_POWER_CPU:
    case OLED_POWER_SLEEP:
      hints = "A:Execute B:Back";
      break;
      
    case OLED_BLUETOOTH:
      {
        extern bool bluetoothShowingStatus;
        if (bluetoothShowingStatus) {
          hints = "A:Back B:Back";
        } else {
          hints = "A:Select B:Back";
        }
      }
      break;
      
    case OLED_SYSTEM_STATUS:
    case OLED_SENSOR_DATA:
    case OLED_SENSOR_LIST:
    case OLED_BOOT_SENSORS:
    case OLED_MEMORY_STATS:
      hints = "B:Back";
      break;
      
    case OLED_WEB_STATS:
#if ENABLE_HTTP_SERVER
      {
        extern httpd_handle_t server;
        hints = server ? "X:Stop B:Back" : "X:Start B:Back";
      }
#else
      hints = "B:Back";
#endif
      break;

    case OLED_RTC_DATA:
#if ENABLE_RTC_SENSOR
      {
        extern bool rtcEnabled;
        extern bool rtcConnected;
        hints = (rtcEnabled && rtcConnected) ? "X:Stop B:Back" : "X:Start B:Back";
      }
#else
      hints = "B:Back";
#endif
      break;

    case OLED_PRESENCE_DATA:
#if ENABLE_PRESENCE_SENSOR
      {
        extern bool presenceEnabled;
        extern bool presenceConnected;
        hints = (presenceEnabled && presenceConnected) ? "X:Stop B:Back" : "X:Start B:Back";
      }
#else
      hints = "B:Back";
#endif
      break;
      
    case OLED_CUSTOM_TEXT:
    case OLED_LOGO:
    case OLED_ANIMATION:
      hints = "B:Back";
      break;
      
    case OLED_AUTOMATIONS:
      hints = "B:Back";
      break;
      
    case OLED_CLI_VIEWER:
      {
        // Show selected/total in footer
        extern OLEDConsoleBuffer gOLEDConsole;
        extern int getCLIViewerSelectedIndex();
        int lineCount = gOLEDConsole.getLineCount();
        int selected = getCLIViewerSelectedIndex();
        static char cliHints[32];
        snprintf(cliHints, sizeof(cliHints), "A:Info B:Back [%d/%d]", selected, lineCount);
        hints = cliHints;
      }
      break;
      
    case OLED_LOGGING:
      hints = "A:Select B:Back";
      break;
    
    case OLED_LOGIN:
      {
        // Check if user is already authenticated
        bool isAuthed = isTransportAuthenticated(SOURCE_LOCAL_DISPLAY);
        
        if (gSettings.localDisplayRequireAuth && !isAuthed) {
          // Auth required and not logged in - can't go back, only login
          hints = "A:Select";
        } else {
          // Either auth not required, or user is already logged in (session switching)
          hints = "A:Select B:Back";
        }
      }
      break;
      
    case OLED_LOGOUT:
      hints = "A:Confirm B:Cancel";
      break;
      
    case OLED_QUICK_SETTINGS:
      hints = "A:Toggle B:Back";
      break;
      
    case OLED_GPS_MAP:
      {
        extern bool gMapMenuOpen;
        if (gMapMenuOpen) {
          hints = "A:Select B:Close";
        } else {
          hints = "St:Menu A+J:Rot B:Back";
        }
      }
      break;
      
    case OLED_OFF:
      hints = nullptr;  // No hints for OFF mode
      break;
      
    case OLED_UNAVAILABLE:
      // If feature is "Not built" (compile-time disabled), no X action is possible
      if (unavailableOLEDReason.indexOf("Not built") >= 0) {
        hints = "B:Back";  // Can only go back, no action available
      } else if (unavailableOLEDTitle == "ESP-NOW") {
        hints = "X:Setup B:Back";  // X opens ESP-NOW setup (then Y to name device)
      } else if (unavailableOLEDTitle == "Automations") {
        hints = "X:Enable B:Back";  // X enables automation system
      } else if (unavailableOLEDTitle == "Bluetooth") {
        hints = "X:Start B:Back";  // X initializes Bluetooth
      } else if (unavailableOLEDTitle == "Web") {
        hints = "X:Start B:Back";  // X starts HTTP server
      } else {
        hints = "X:Start B:Back";  // X attempts to initialize/start sensor, B returns to menu
      }
      break;
      
    default:
      // For registered modes (thermal, GPS, FM radio, etc.)
      hints = "B:Back";
      break;
  }
  
  // Draw the hint text
  if (hints) {
    oledDisplay->print(hints);
  }
}

// =============================================================================
// Shared Command Execution
// =============================================================================

#include "System_User.h"
#include "System_Command.h"

// External references for authentication
extern bool gLocalDisplayAuthed;
extern String gLocalDisplayUser;

void executeOLEDCommand(const String& cmd) {
  extern bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize);
  
  AuthContext ctx;
  ctx.transport = SOURCE_LOCAL_DISPLAY;
  ctx.user = gLocalDisplayAuthed ? gLocalDisplayUser : "";
  ctx.ip = "oled";
  ctx.path = "/oled/command";
  ctx.sid = "";
  
  char out[512];
  bool success = executeCommand(ctx, cmd.c_str(), out, sizeof(out));
  
  if (!success && strlen(out) > 0) {
    Serial.printf("[OLED_CMD] Command failed: %s\n", out);
  }
}


// =============================================================================
// MERGED FROM OLED_Display.cpp
// =============================================================================
// Forward declarations for OLED animation functions
static void showFirstTimeSetupPrompt();
static void showFirstTimeSetupProgress();
static void showSetupCompleteMessage();
static void showNormalBootProgress();
static void drawProgressBar(int percent);

// Forward declarations for menu system (defined later in file)
void displayMenuListStyle();

// ============================================================================
// Per-Mode Layout System
// ============================================================================
// Layout styles: 0 = default/grid, 1 = list/alternate, 2+ = mode-specific variants
// Each OLEDMode can have its own layout style setting

// Array to store layout style for each OLEDMode (indexed by enum value)
// OLED_GAMEPAD_VISUAL is currently the last enum value (index ~23)
int oledModeLayouts[32] = {0};  // Default all modes to layout 0
// Set OLED_MENU to default to grid style (0) for large icons
static int oledMenuInit = []() {
  oledModeLayouts[OLED_MENU] = 0;  // Default to grid (large icons)
  return 0;
}();

// Get layout style for a specific mode
int getOLEDModeLayout(OLEDMode mode) {
  if (mode >= 0 && mode < 32) return oledModeLayouts[mode];
  return 0;
}

// Set layout style for a specific mode
void setOLEDModeLayout(OLEDMode mode, int layout) {
  if (mode >= 0 && mode < 32) oledModeLayouts[mode] = layout;
}

// Get layout for current mode (convenience)
int getCurrentModeLayout() {
  return getOLEDModeLayout(currentOLEDMode);
}

// ============================================================================
// OLED Change Detection - Skip rendering when nothing has changed
// ============================================================================
// Uses existing sequence counters from sensor caches to detect changes
static uint32_t oledLastRenderedGamepadSeq = 0;
static unsigned long oledLastRenderedSensorSeq = 0;
static bool oledForceNextRender = true;  // Force first render

// Manual dirty flag for non-sensor changes (menu state, settings, etc.)
void oledMarkDirty() {
  oledForceNextRender = true;
}

void oledMarkDirtyMode(OLEDMode mode) {
  // For compatibility - any mode change triggers dirty
  oledForceNextRender = true;
}

bool oledIsDirty() {
  extern volatile unsigned long gSensorStatusSeq;
  extern ControlCache gControlCache;
  
  if (oledForceNextRender) return true;
  if (gControlCache.gamepadSeq != oledLastRenderedGamepadSeq) return true;
  if (gSensorStatusSeq != oledLastRenderedSensorSeq) return true;
  return false;
}

void oledClearDirty() {
  extern volatile unsigned long gSensorStatusSeq;
  extern ControlCache gControlCache;
  
  oledForceNextRender = false;
  oledLastRenderedGamepadSeq = gControlCache.gamepadSeq;
  oledLastRenderedSensorSeq = gSensorStatusSeq;
}

void oledSetAlwaysDirty(bool always) {
  // For animations - just keep forcing renders
  if (always) oledForceNextRender = true;
}

// Legacy compatibility - redirects to per-mode system for OLED_MENU
#define oledMenuLayoutStyle oledModeLayouts[OLED_MENU]
#if ENABLE_WIFI || ENABLE_ESPNOW
  #include <esp_wifi.h>
#endif
#include <LittleFS.h>

// ============================================================================
// OLED Display Functions
// ============================================================================

// OLED display object and state (owned by this module)
// Note: oledDisplay is now an alias for gDisplay (defined in Display_HAL.h)
// The actual display object is managed by Display_HAL.cpp
extern bool gLocalDisplayAuthed;
extern String gLocalDisplayUser;
bool oledConnected = false;
bool oledEnabled = false;

// ESP-NOW mesh functions from .ino
extern MeshPeerHealth gMeshPeers[];  // Array of mesh peer health entries
extern String macToHexString(const uint8_t* mac);
extern void macFromHexString(const String& s, uint8_t out[6]);
extern bool isSelfMac(const uint8_t* mac);
extern bool isMeshPeerAlive(const MeshPeerHealth* peer);

// Display helper functions
extern void displayAnimation();

// Device registry (from i2c_system.cpp)
extern ConnectedDevice connectedDevices[];
extern int connectedDeviceCount;

// Forward declarations for two-phase rendering functions
void prepareFileBrowserData();
void prepareNetworkData();
void prepareMemoryData();
void prepareWebStatsData();
void prepareSystemStatusData();
void prepareMeshStatusData();
void prepareConnectedSensorsData();
void displayFileBrowserRendered();
void displayNetworkInfoRendered();
void displayMemoryStatsRendered();
void displayWebStatsRendered();
void displaySystemStatusRendered();
void displayMeshStatusRendered();
void displayConnectedSensorsRendered();
void displaySensorMenu();

// OLED state variables (defined here, used by .ino and this file)
// Initial mode will be set based on oledRequireAuth setting during initialization
OLEDMode currentOLEDMode = OLED_SYSTEM_STATUS;
static OLEDMode lastRenderedMode = OLED_OFF;  // Track mode changes to force immediate refresh

// Mode navigation stack for back button (minimal fixed-size stack)
#define OLED_MODE_STACK_SIZE 8
static OLEDMode modeStack[OLED_MODE_STACK_SIZE];
static int modeStackDepth = 0;
String customOLEDText = "";
unsigned long oledLastUpdate = 0;
unsigned long animationFrame = 0;
unsigned long animationLastUpdate = 0;
int animationFPS = 30;
// Define current animation state (extern in header)
OLEDAnimationType currentAnimation = ANIM_BOOT_PROGRESS;

// ============================================================================
// OLED Mode Registration System
// ============================================================================

// Static storage for registered OLED modes
static const OLEDModeEntry* oledModeRegistry[MAX_OLED_MODES];
static size_t oledModeRegistrySize = 0;

// Module tracking for debug
#define MAX_OLED_MODULES 16
struct OLEDModuleInfo {
  const char* name;
  size_t count;
};
static OLEDModuleInfo registeredOLEDModules[MAX_OLED_MODULES];
static size_t registeredOLEDModuleCount = 0;

OLEDModeRegistrar::OLEDModeRegistrar(const OLEDModeEntry* modes, size_t count, const char* moduleName) {
  registerOLEDModes(modes, count);
  
  if (registeredOLEDModuleCount < MAX_OLED_MODULES) {
    registeredOLEDModules[registeredOLEDModuleCount].name = moduleName;
    registeredOLEDModules[registeredOLEDModuleCount].count = count;
    registeredOLEDModuleCount++;
  }
}

void registerOLEDMode(const OLEDModeEntry* mode) {
  DEBUG_SYSTEMF("[OLED] registerOLEDMode called: mode=%p", mode);
  if (!mode) {
    DEBUG_SYSTEMF("[OLED] registerOLEDMode: mode is NULL, returning");
    return;
  }
  
  DEBUG_SYSTEMF("[OLED] registerOLEDMode: registering mode=%d (%s), current size=%zu, max=%d", 
                mode->mode, mode->name ? mode->name : "unnamed", oledModeRegistrySize, MAX_OLED_MODES);
  
  if (oledModeRegistrySize >= MAX_OLED_MODES) {
    DEBUG_SYSTEMF("[OLED] registerOLEDMode: registry full, returning");
    return;
  }
  
  // Check for duplicate mode enum values
  for (size_t i = 0; i < oledModeRegistrySize; i++) {
    if (oledModeRegistry[i]->mode == mode->mode) {
      DEBUG_SYSTEMF("[OLED] registerOLEDMode: duplicate mode %d, returning", mode->mode);
      return;
    }
  }
  
  oledModeRegistry[oledModeRegistrySize] = mode;
  oledModeRegistrySize++;
  DEBUG_SYSTEMF("[OLED] registerOLEDMode: successfully registered mode %d, new size=%zu", 
                mode->mode, oledModeRegistrySize);
}

void registerOLEDModes(const OLEDModeEntry* modes, size_t count) {
  for (size_t i = 0; i < count; i++) {
    registerOLEDMode(&modes[i]);
  }
}

const OLEDModeEntry* findOLEDMode(OLEDMode mode) {
  for (size_t i = 0; i < oledModeRegistrySize; i++) {
    if (oledModeRegistry[i]->mode == mode) {
      return oledModeRegistry[i];
    }
  }
  return nullptr;
}

const OLEDModeEntry* getRegisteredOLEDModes() {
  // Return first entry (caller should use getRegisteredOLEDModeCount for iteration)
  return oledModeRegistrySize > 0 ? oledModeRegistry[0] : nullptr;
}

size_t getRegisteredOLEDModeCount() {
  return oledModeRegistrySize;
}

// Get mode entry by index (for menu building)
const OLEDModeEntry* getOLEDModeByIndex(size_t index) {
  if (index < oledModeRegistrySize) {
    return oledModeRegistry[index];
  }
  return nullptr;
}

// Forward declarations for quick settings (defined in oled_quick_settings.cpp)
extern void displayQuickSettings();
extern bool quickSettingsInputHandler(int deltaX, int deltaY, uint32_t newlyPressed);

static bool quickSettingsAvailability(String* outReason) {
  return true;
}

// Built-in quick settings mode registration (must be in oled_display.cpp to ensure linking)
static const OLEDModeEntry builtInQuickSettingsMode = {
  OLED_QUICK_SETTINGS,
  "Quick Settings",
  "settings",
  displayQuickSettings,
  quickSettingsAvailability,
  quickSettingsInputHandler,
  false,  // Don't show in main menu (accessed via SELECT button)
  -1
};

// Force linker to include OLED mode files that use static registration
// Without these calls, the linker may drop object files with no external references
extern void oledLoginModeInit();
extern void oledLogoutModeInit();
extern void oledLoggingModeInit();

// Print summary of all registered OLED modes (call from setup() after static init)
void printRegisteredOLEDModes() {
  // Force linker to include mode files (static registrars run during global init,
  // but we need external references to prevent linker from dropping these files)
  oledLoginModeInit();
  oledLogoutModeInit();
  oledLoggingModeInit();
  
  // Register built-in quick settings mode first
  static bool builtInRegistered = false;
  if (!builtInRegistered) {
    registerOLEDMode(&builtInQuickSettingsMode);
    builtInRegistered = true;
  }
  
  Serial.printf("[OLED_MODE] %d modes registered from %d modules:\n", 
                oledModeRegistrySize, registeredOLEDModuleCount);
  for (size_t i = 0; i < registeredOLEDModuleCount; i++) {
    Serial.printf("  - %s (%d modes)\n", 
                  registeredOLEDModules[i].name, 
                  registeredOLEDModules[i].count);
  }
}

String unavailableOLEDTitle = "Unavailable";
String unavailableOLEDReason = "";
unsigned long unavailableOLEDStartTime = 0;  // Non-static for extern access from OLED_Mode_System.cpp

// Flag to track if user manually changed mode during boot (prevents boot sequence from overriding)
static bool userOverrodeBootMode = false;

static void debugOLEDModeChange(const char* src, OLEDMode from, OLEDMode to, const String& extra) {
  if (from == to) return;
  if (extra.length() > 0) {
    Serial.printf("[OLED_MODE] %s: %d -> %d | %s\n", src, (int)from, (int)to, extra.c_str());
  } else {
    Serial.printf("[OLED_MODE] %s: %d -> %d\n", src, (int)from, (int)to);
  }
}

void enterUnavailablePage(const String& title, const String& reason) {
  unavailableOLEDTitle = title.length() > 0 ? title : String("Unavailable");
  unavailableOLEDReason = reason;
  unavailableOLEDStartTime = millis();
  // If we expect the user to take an action (e.g. "Press X"), keep the page up
  // rather than auto-returning after a timeout.
  if (unavailableOLEDReason.indexOf("Press X") >= 0) {
    unavailableOLEDStartTime = 0;
  }
  currentOLEDMode = OLED_UNAVAILABLE;
}

extern OLEDAnimationType currentAnimation;
extern const OLEDAnimation gAnimationRegistry[];
extern const int gAnimationCount;

// Sensor state (managed by I2C system)
extern bool imuConnected;
extern bool imuEnabled;
extern bool tofConnected;
extern bool tofEnabled;
extern bool thermalConnected;
extern bool thermalEnabled;
extern bool gpsConnected;
extern bool gpsEnabled;
extern bool gamepadConnected;
extern bool gamepadEnabled;
extern bool apdsConnected;
extern bool rtcConnected;

// Modular sensor caches (each sensor defines its own cache)
// Includes are conditional based on sensor availability

// Settings and EspNowState now come from espnow_system.h -> settings.h
extern Settings gSettings;

// GPS module
extern Adafruit_GPS* gPA1010D;

// ESP-NOW mesh state (defined in espnow_system.h or stubs)
#if ENABLE_ESPNOW
// gEspNow already declared in espnow_system.h
extern size_t gMeshTopologySize;
#else
// ESP-NOW stubs provide gEspNow, gMeshTopology, gMeshPeers
static size_t gMeshTopologySize = 0;
#endif

// Cache locking functions
extern bool lockThermalCache(TickType_t timeout);
extern void unlockThermalCache();

// I2C transaction wrapper
extern void i2cTransactionVoid(uint32_t hz, uint32_t timeoutMs, std::function<void()> func);

// Helper functions
extern void broadcastOutput(const String& msg);
extern bool meshEnabled();
extern String getEspNowDeviceName(const uint8_t* mac);
// macToHexString, macFromHexString, and macEqual6 now in espnow_system.h
extern void updateIMUActions();

// Debug flags from debug_system.h (gDebugFlags, DEBUG_SENSORS_FRAME, etc.)

// Constants (MESH_PEER_MAX now defined in espnow_system.h)

// ============================================================================
// OLED Initialization and Control
// ============================================================================

bool initOLEDDisplay() {
  if (gDisplay != nullptr) {
    broadcastOutput("OLED display already initialized");
    return true;
  }

  DEBUG_SENSORSF("Starting display initialization (%s)...", DISPLAY_NAME);

  // Use Display HAL initialization
  bool success = displayInit();
  
  if (success) {
    oledConnected = true;
    oledEnabled = true;
    
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
    // Register OLED for I2C health tracking
    i2cRegisterDevice(OLED_I2C_ADDRESS, "OLED");
#endif
    
    broadcastOutput("Display initialized successfully");
    INFO_SYSTEMF("Display initialized: %s (%dx%d)", DISPLAY_NAME, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    
    // Show initial splash screen
    gDisplay->clearDisplay();
    gDisplay->setRotation(0);
    gDisplay->setTextSize(1);
    gDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    gDisplay->setCursor(0, 0);
    gDisplay->println("HardwareOne v2.1");
    gDisplay->print("Display: ");
    gDisplay->println(DISPLAY_NAME);
    displayUpdate();
    
    // Initialize input abstraction layer
    inputAbstractionInit();
    
    // Initialize modular OLED interfaces only if their systems are already running
#if ENABLE_ESPNOW
    if (gEspNow && gEspNow->initialized) {
      oledEspNowInit();
    }
#endif
  } else {
    ERROR_SYSTEMF("Display initialization failed");
  }
  
  return success;
}

void stopOLEDDisplay() {
  if (!oledConnected || gDisplay == nullptr) {
    return;
  }

#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  // Use i2cTransaction wrapper for safe mutex + clock management
  i2cTransactionVoid(100000, 500, [&]() {
    gDisplay->clearDisplay();
    displayUpdate();
    delete gDisplay;
    gDisplay = nullptr;
  });
#else
  // For SPI displays, no transaction needed
  displayClear();
  displayUpdate();
  delete gDisplay;
  gDisplay = nullptr;
#endif

  oledConnected = false;
  oledEnabled = false;

  DEBUG_SENSORSF("Display stopped");
}

// ============================================================================
// Display Mode Functions
// ============================================================================

// displaySystemStatus() moved to OLED_Mode_System.cpp
// displaySensorData() moved to OLED_Mode_Sensors.cpp
// displayThermalVisual() moved to Sensor_Thermal_MLX90640.cpp (modular OLED mode)

// displayGPSData() moved to Sensor_GPS_PA1010D.cpp (modular OLED mode)

// displayFmRadio() moved to fm_radio.cpp (modular OLED mode)

// Power functions moved to OLED_Mode_Power.cpp
#include "OLED_Utils.h"  // For executeOLEDCommand

// Network and system display functions moved to OLED_Mode_Network.cpp and OLED_Mode_System.cpp
// Variables defined in OLED_Mode_Network.cpp
extern int networkMenuSelection;
extern const int NETWORK_MENU_ITEMS;
extern bool networkShowingStatus;
extern bool networkShowingWiFiSubmenu;
extern OLEDScrollState wifiSubmenuScroll;
extern bool wifiSubmenuScrollInitialized;
extern bool wifiAddingNetwork;
extern bool wifiEnteringSSID;
extern bool wifiEnteringPassword;
extern String wifiNewSSID;
extern String wifiNewPassword;
extern void initWifiSubmenuScroll();

// 3D Cube rotation helper functions
void rotateCubePoint(float& x, float& y, float& z, float angleX, float angleY, float angleZ) {
  // Rotate around X axis
  float cosX = cos(angleX);
  float sinX = sin(angleX);
  float y1 = y * cosX - z * sinX;
  float z1 = y * sinX + z * cosX;
  y = y1;
  z = z1;

  // Rotate around Y axis
  float cosY = cos(angleY);
  float sinY = sin(angleY);
  float x1 = x * cosY + z * sinY;
  z1 = -x * sinY + z * cosY;
  x = x1;
  z = z1;

  // Rotate around Z axis
  float cosZ = cos(angleZ);
  float sinZ = sin(angleZ);
  x1 = x * cosZ - y * sinZ;
  y1 = x * sinZ + y * cosZ;
  x = x1;
  y = y1;
}

void projectCubePoint(float x, float y, float z, int& screenX, int& screenY, int centerX, int centerY) {
  // Simple perspective projection
  float perspective = 200.0 / (200.0 + z);
  screenX = centerX + (int)(x * perspective);
  screenY = centerY + (int)(y * perspective);
}

// displayLogo() moved to OLED_Mode_Menu.cpp

// displayIMUActions() moved to Sensor_IMU_BNO055.cpp (modular OLED mode)

// displayToFData() moved to Sensor_ToF_VL53L4CX.cpp (modular OLED mode)

// displayAPDSData() moved to Sensor_APDS_APDS9960.cpp (modular OLED mode)
// displayConnectedSensors() moved to OLED_Mode_Sensors.cpp

// Forward declaration for gamepad input processing
bool processGamepadMenuInput();
void tryAutoStartGamepadForMenu();

void updateOLEDDisplay() {
  // animationLastUpdate, animationFrame, animationFPS are now defined at top of file
  extern void displayAnimation();
  
  if (!oledEnabled || !oledConnected || oledDisplay == nullptr) {
    return;
  }

  // AUTHENTICATION ENFORCEMENT: Force login screen if auth is required and user is not authenticated
  extern Settings gSettings;
  extern bool gLocalDisplayAuthed;
  extern bool oledBootModeActive;
  
  if (gSettings.localDisplayRequireAuth && !gLocalDisplayAuthed && !oledBootModeActive) {
    // User must be on login screen - force mode change if they somehow got to another mode
    if (currentOLEDMode != OLED_LOGIN) {
      Serial.printf("[OLED_AUTH_GUARD] Forcing mode from %d to LOGIN - auth required\n", (int)currentOLEDMode);
      currentOLEDMode = OLED_LOGIN;
    }
  }

  // Process gamepad input for menu navigation (runs every frame, handles its own debouncing)
  processGamepadMenuInput();

  unsigned long now = millis();
  
  // Force immediate refresh on mode change (don't wait for timer)
  bool modeChanged = (currentOLEDMode != lastRenderedMode);

  // Animation mode has its own FPS control
  if (currentOLEDMode == OLED_ANIMATION) {
    unsigned long animInterval = 1000 / animationFPS;
    if (now - animationLastUpdate >= animInterval) {
      animationLastUpdate = now;
      animationFrame++;
    } else if (!modeChanged) {
      return;
    }
  } else {
    // Timer-based throttle: check at most every updateInterval ms
    unsigned long updateInterval = (gSettings.oledUpdateInterval > 0) ? (unsigned long)gSettings.oledUpdateInterval : 200;
    if (now - oledLastUpdate < updateInterval) {
      return;  // Not time to check yet
    }
    
    // Skip render if nothing changed (uses gamepadSeq + sensorStatusSeq)
    if (!modeChanged && !oledIsDirty()) {
      oledLastUpdate = now;  // Reset timer even if we skip
      return;  // Nothing changed, skip expensive render
    }
  }
  oledLastUpdate = now;
  lastRenderedMode = currentOLEDMode;

  // Skip if OLED is degraded (will auto-retry after recovery timeout)
  if (i2cDeviceIsDegraded(OLED_I2C_ADDRESS)) {
    static unsigned long lastDegradedLog = 0;
    unsigned long nowLog = millis();
    if ((isDebugFlagSet(DEBUG_MEMORY) || isDebugFlagSet(DEBUG_SYSTEM)) && (nowLog - lastDegradedLog > 2000)) {
      lastDegradedLog = nowLog;
      Serial.println("[OLED] Skipping render - I2C device marked DEGRADED");
    }
    return;
  }

  // Pre-gather data OUTSIDE I2C transaction to avoid blocking gamepad
  switch (currentOLEDMode) {
    case OLED_FILE_BROWSER:
      prepareFileBrowserData();
      break;
    case OLED_NETWORK_INFO:
      prepareNetworkData();
      break;
    case OLED_MEMORY_STATS:
      prepareMemoryData();
      break;
    case OLED_WEB_STATS:
      prepareWebStatsData();
      break;
    case OLED_SYSTEM_STATUS:
      prepareSystemStatusData();
      break;
    case OLED_MESH_STATUS:
      prepareMeshStatusData();
      break;
    case OLED_SENSOR_LIST:
    case OLED_BOOT_SENSORS:
      prepareConnectedSensorsData();  // Update scroll animation every frame
      break;
    default:
      break;
  }

#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  // Use device-aware i2cTransaction wrapper for safe mutex + clock management
  // Use 500ms timeout to tolerate I2C bus contention from other sensors
  i2cDeviceTransactionVoid(OLED_I2C_ADDRESS, 100000, 500, [&]() {
#endif
    // Clear content area and footer area separately
    // Animations handle their own full-screen clear since they don't use footer
    if (currentOLEDMode == OLED_ANIMATION) {
      gDisplay->clearDisplay();
    } else {
      // Clear content area (0-53)
      gDisplay->fillRect(0, 0, SCREEN_WIDTH, OLED_CONTENT_HEIGHT, DISPLAY_COLOR_BLACK);
      // Clear footer area (54-63) to remove any artifacts from previous modes
      gDisplay->fillRect(0, OLED_CONTENT_HEIGHT, SCREEN_WIDTH, OLED_FOOTER_HEIGHT, DISPLAY_COLOR_BLACK);
    }
    gDisplay->setTextSize(1);
    gDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    gDisplay->setCursor(0, 0);

    // DEBUG: Track render for black flash investigation
    static unsigned long renderCount = 0;
    renderCount++;
    bool contentDrawn = true;  // Assume true, set false if mode doesn't draw
    
    // Log every 50th render or if mode changes to help track black flash
    static OLEDMode lastLoggedMode = OLED_OFF;
    if (currentOLEDMode != lastLoggedMode || (renderCount % 50) == 0) {
      Serial.printf("[OLED_RENDER] mode=%d render#%lu\n", (int)currentOLEDMode, renderCount);
      lastLoggedMode = currentOLEDMode;
    }

    switch (currentOLEDMode) {
      case OLED_MENU:
        if (oledMenuLayoutStyle == 0) {
          displayMenuListStyle();  // 0 = large icon grid view
        } else {
          displayMenu();           // 1 = small icon grid view
        }
        break;

      case OLED_SENSOR_MENU:
        displaySensorMenu();
        break;

      case OLED_SYSTEM_STATUS:
        displaySystemStatusRendered();
        break;

      case OLED_SENSOR_DATA:
        displaySensorData();
        break;

      case OLED_SENSOR_LIST:
      case OLED_BOOT_SENSORS:
        displayConnectedSensorsRendered();
        break;

      // OLED_THERMAL_VISUAL handled by registered mode in Sensor_Thermal_MLX90640.cpp

      case OLED_NETWORK_INFO:
        displayNetworkInfoRendered();
        break;

      case OLED_MESH_STATUS:
        displayMeshStatusRendered();
        break;

      case OLED_CUSTOM_TEXT:
        displayCustomText();
        break;

      case OLED_UNAVAILABLE:
        displayUnavailable();
        break;

      case OLED_LOGO:
        displayLogo();
        break;

      case OLED_ANIMATION:
        displayAnimation();
        break;

      // OLED_IMU_ACTIONS handled by registered mode in Sensor_IMU_BNO055.cpp

      // OLED_GPS_DATA handled by registered mode in Sensor_GPS_PA1010D.cpp

      // OLED_FM_RADIO handled by registered mode in fm_radio.cpp

      case OLED_FILE_BROWSER:
        displayFileBrowserRendered();
        break;

#if ENABLE_AUTOMATION
      case OLED_AUTOMATIONS:
        displayAutomations();
        break;
#else
      case OLED_AUTOMATIONS:
        enterUnavailablePage("Automations", "Not compiled");
        break;
#endif

      case OLED_ESPNOW:
        displayEspNow();
        break;

      // OLED_TOF_DATA handled by registered mode in Sensor_ToF_VL53L4CX.cpp

      case OLED_APDS_DATA:
#if ENABLE_APDS_SENSOR
        displayAPDSData();
#endif
        break;

      case OLED_POWER:
        displayPower();
        break;
        
      case OLED_POWER_CPU:
        displayPowerCPU();
        break;
        
      case OLED_POWER_SLEEP:
        displayPowerSleep();
        break;

      case OLED_MEMORY_STATS:
        displayMemoryStatsRendered();
        break;

      case OLED_WEB_STATS:
        displayWebStatsRendered();
        break;

      case OLED_QUICK_SETTINGS:
        // Quick settings handled by registered mode
        {
          const OLEDModeEntry* registeredMode = findOLEDMode(OLED_QUICK_SETTINGS);
          if (registeredMode && registeredMode->displayFunc) {
            registeredMode->displayFunc();
          }
        }
        break;

      case OLED_OFF:
        contentDrawn = false;  // OLED_OFF intentionally draws nothing
        break;
        
      default:
        // Check registered modes for any mode not handled above
        {
          const OLEDModeEntry* registeredMode = findOLEDMode(currentOLEDMode);
          if (registeredMode && registeredMode->displayFunc) {
            registeredMode->displayFunc();
          } else {
            contentDrawn = false;
            // DEBUG: Log when mode not found - this would cause black screen!
            Serial.printf("[OLED_RENDER_FAIL] Mode %d not found! render#%lu registeredMode=%p\n", 
                         (int)currentOLEDMode, renderCount, (void*)registeredMode);
          }
        }
        break;
    }

    // Failsafe: if no content was drawn, draw an error message so screen isn't black
    if (!contentDrawn) {
      Serial.printf("[OLED_BLACK_FLASH] No content drawn! mode=%d render#%lu\n", 
                   (int)currentOLEDMode, renderCount);
      gDisplay->setCursor(0, 20);
      gDisplay->print("Mode ");
      gDisplay->print((int)currentOLEDMode);
      gDisplay->print(" no render");
    }

    oledConfirmRender();

    // Draw persistent footer with button hints (always last, in same frame)
    drawOLEDFooter();

    // Mark mode as clean after successful render
    oledClearDirty();

    displayUpdate();
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  });
#endif
}

// OLED Settings Commands (migrated from .ino)
// ============================================================================

// Validation macro used by CLI handlers
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { \
    extern bool gCLIValidateOnly; \
    if (gCLIValidateOnly) return "VALID"; \
  } while(0)

extern bool ensureDebugBuffer();
extern bool writeSettingsJson();

const char* cmd_oled_enabled(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: oledenabled <0|1>";
  while (*p == ' ') p++;  // Skip whitespace
  bool enabled = (*p == '1' || strncasecmp(p, "true", 4) == 0);
  gSettings.oledEnabled = enabled;
  writeSettingsJson();

  if (gSettings.oledEnabled) {
    if (!oledConnected) {
      if (initOLEDDisplay()) {
        broadcastOutput("OLED display started");
      } else {
        broadcastOutput("Failed to initialize OLED display. Check wiring.");
        return "ERROR";
      }
    } else {
      oledEnabled = true;
    }

    String defaultMode = gSettings.oledDefaultMode;
    defaultMode.toLowerCase();
    OLEDMode prevMode = currentOLEDMode;
    if (defaultMode == "status") currentOLEDMode = OLED_SYSTEM_STATUS;
    else if (defaultMode == "sensordata") currentOLEDMode = OLED_SENSOR_DATA;
    else if (defaultMode == "sensorlist") currentOLEDMode = OLED_SENSOR_LIST;
    else if (defaultMode == "thermal") currentOLEDMode = OLED_THERMAL_VISUAL;
    else if (defaultMode == "network") currentOLEDMode = OLED_NETWORK_INFO;
    else if (defaultMode == "mesh") currentOLEDMode = OLED_MESH_STATUS;
    else if (defaultMode == "logo") currentOLEDMode = OLED_LOGO;
    else currentOLEDMode = OLED_SYSTEM_STATUS;

    debugOLEDModeChange("cmd.oledenabled.forceDefault", prevMode, currentOLEDMode, String("defaultMode=") + defaultMode);

    updateOLEDDisplay();
    snprintf(getDebugBuffer(), 1024, "OLED display enabled (mode: %s)", gSettings.oledDefaultMode.c_str());
  } else {
    if (oledConnected) {
      oledEnabled = false;
      i2cTransactionVoid(100000, 500, [&]() {
        oledDisplay->clearDisplay();
        oledDisplay->display();
      });
    }
    snprintf(getDebugBuffer(), 1024, "OLED display disabled");
  }
  return getDebugBuffer();
}

const char* cmd_oled_autoinit(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: oledautoinit <0|1>";
  while (*p == ' ') p++;  // Skip whitespace
  bool enabled = (*p == '1' || strncasecmp(p, "true", 4) == 0);
  gSettings.oledAutoInit = enabled;
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "OLED auto-init %s", enabled ? "enabled" : "disabled");
  return getDebugBuffer();
}

const char* cmd_oled_requireauth(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: oledrequireauth <0|1>";
  while (*p == ' ') p++;  // Skip whitespace
  bool enabled = (*p == '1' || strncasecmp(p, "true", 4) == 0);
  gSettings.localDisplayRequireAuth = enabled;
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "Local display require auth %s", enabled ? "enabled" : "disabled");
  return getDebugBuffer();
}

const char* cmd_oled_bootmode(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: oledbootmode <logo|status|sensors|thermal|network|mesh|off>";
  while (*p == ' ') p++;  // Skip whitespace
  // Case-insensitive compare for mode names
  if (strncasecmp(p, "logo", 4) == 0) {
    gSettings.oledBootMode = "logo";
  } else if (strncasecmp(p, "status", 6) == 0) {
    gSettings.oledBootMode = "status";
  } else if (strncasecmp(p, "sensors", 7) == 0) {
    gSettings.oledBootMode = "sensors";
  } else if (strncasecmp(p, "thermal", 7) == 0) {
    gSettings.oledBootMode = "thermal";
  } else if (strncasecmp(p, "network", 7) == 0) {
    gSettings.oledBootMode = "network";
  } else if (strncasecmp(p, "mesh", 4) == 0) {
    gSettings.oledBootMode = "mesh";
  } else if (strncasecmp(p, "off", 3) == 0) {
    gSettings.oledBootMode = "off";
  } else {
    return "Error: OLED boot mode must be logo|status|sensors|thermal|network|mesh|off";
  }
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "OLED boot mode set to %s", gSettings.oledBootMode.c_str());
  return getDebugBuffer();
}

const char* cmd_oled_defaultmode(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: oleddefaultmode <logo|status|sensors|thermal|network|mesh|off>";
  while (*p == ' ') p++;  // Skip whitespace
  // Case-insensitive compare for mode names
  if (strncasecmp(p, "logo", 4) == 0) {
    gSettings.oledDefaultMode = "logo";
  } else if (strncasecmp(p, "status", 6) == 0) {
    gSettings.oledDefaultMode = "status";
  } else if (strncasecmp(p, "sensors", 7) == 0) {
    gSettings.oledDefaultMode = "sensors";
  } else if (strncasecmp(p, "thermal", 7) == 0) {
    gSettings.oledDefaultMode = "thermal";
  } else if (strncasecmp(p, "network", 7) == 0) {
    gSettings.oledDefaultMode = "network";
  } else if (strncasecmp(p, "mesh", 4) == 0) {
    gSettings.oledDefaultMode = "mesh";
  } else if (strncasecmp(p, "off", 3) == 0) {
    gSettings.oledDefaultMode = "off";
  } else {
    return "Error: OLED default mode must be logo|status|sensors|thermal|network|mesh|off";
  }
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "OLED default mode set to %s", gSettings.oledDefaultMode.c_str());
  return getDebugBuffer();
}

const char* cmd_oled_bootduration(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: oledbootduration <0..60000>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 0 || v > 60000) return "Error: OLED boot duration must be 0..60000 ms";
  gSettings.oledBootDuration = v;
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "OLED boot duration set to %dms", v);
  return getDebugBuffer();
}

const char* cmd_oled_updateinterval(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: oledupdateinterval <10..1000>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 10 || v > 1000) return "Error: OLED update interval must be 10..1000 ms";
  gSettings.oledUpdateInterval = v;
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "OLED update interval set to %dms (applies on next update)", v);
  return getDebugBuffer();
}

const char* cmd_oled_brightness(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: oledbrightness <0..255>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 0 || v > 255) return "Error: OLED brightness must be 0..255";
  gSettings.oledBrightness = v;
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "OLED brightness set to %d", v);
  return getDebugBuffer();
}

const char* cmd_oled_thermalscale(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: oledthermalscale <0.1..10.0>";
  while (*p == ' ') p++;  // Skip whitespace
  float f = strtof(p, nullptr);
  if (f < 0.1 || f > 10.0) return "Error: OLED thermal scale must be 0.1..10.0";
  gSettings.oledThermalScale = f;
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "OLED thermal scale set to %.2f", f);
  return getDebugBuffer();
}

const char* cmd_oled_thermalcolormode(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: oledthermalcolormode <3level|grayscale>";
  while (*p == ' ') p++;  // Skip whitespace
  if (strncasecmp(p, "3level", 6) == 0) {
    gSettings.oledThermalColorMode = "3level";
  } else if (strncasecmp(p, "grayscale", 9) == 0) {
    gSettings.oledThermalColorMode = "grayscale";
  } else {
    return "Error: OLED thermal color mode must be 3level|grayscale";
  }
  writeSettingsJson();
  snprintf(getDebugBuffer(), 1024, "OLED thermal color mode set to %s", gSettings.oledThermalColorMode.c_str());
  return getDebugBuffer();
}

// ============================================================================
// OLED Display Command Handlers  
// ============================================================================

const char* cmd_oledstart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (oledConnected) {
    broadcastOutput("OLED display already running");
    return "OK";
  }

  if (initOLEDDisplay()) {
    return "OK";
  } else {
    broadcastOutput("Failed to initialize OLED display. Check wiring.");
    return "ERROR";
  }
}

const char* cmd_oledstop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!oledConnected) {
    broadcastOutput("OLED display not running");
    return "OK";
  }

  stopOLEDDisplay();
  broadcastOutput("OLED display stopped");
  return "OK";
}

const char* cmd_oledmode(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!oledConnected) {
    broadcastOutput("OLED display not running. Use 'oledstart' first.");
    return "ERROR";
  }

  String mode = args;
  mode.trim();
  
  if (mode.length() == 0) {
    broadcastOutput("Usage: oledmode <menu|status|sensordata|sensorlist|thermal|network|mesh|gps|text|logo|anim|imuactions|fmradio|files|automations|espnow|memory|off>");
    return "ERROR";
  }

  mode.toLowerCase();

  // If boot sequence is still running, mark that user overrode it
  if (oledBootModeActive) {
    userOverrodeBootMode = true;
    Serial.println("[OLED_MODE] User overrode boot sequence - will not auto-transition");
  }

  if (mode == "menu") {
    currentOLEDMode = OLED_MENU;
    resetOLEDMenu();
    tryAutoStartGamepadForMenu();  // Auto-start gamepad if connected
    broadcastOutput("OLED mode: Menu");
  } else if (mode == "status") {
    currentOLEDMode = OLED_SYSTEM_STATUS;
    broadcastOutput("OLED mode: System Status");
  } else if (mode == "sensordata") {
    currentOLEDMode = OLED_SENSOR_DATA;
    broadcastOutput("OLED mode: Sensor Data");
  } else if (mode == "sensorlist") {
    currentOLEDMode = OLED_SENSOR_LIST;
    broadcastOutput("OLED mode: Sensor List (scrolling)");
  } else if (mode == "thermal") {
    currentOLEDMode = OLED_THERMAL_VISUAL;
    broadcastOutput("OLED mode: Thermal Visual");
  } else if (mode == "network") {
    currentOLEDMode = OLED_NETWORK_INFO;
    broadcastOutput("OLED mode: Network Info");
  } else if (mode == "mesh") {
    currentOLEDMode = OLED_MESH_STATUS;
    broadcastOutput("OLED mode: Mesh Status");
  } else if (mode == "text") {
    currentOLEDMode = OLED_CUSTOM_TEXT;
    broadcastOutput("OLED mode: Custom Text");
  } else if (mode == "logo") {
    currentOLEDMode = OLED_LOGO;
    broadcastOutput("OLED mode: Logo");
  } else if (mode == "anim" || mode == "animation") {
    currentOLEDMode = OLED_ANIMATION;
    animationFrame = 0;  // Use the module-level variable
    broadcastOutput("OLED mode: Animation");
  } else if (mode == "imuactions" || mode == "actions") {
    currentOLEDMode = OLED_IMU_ACTIONS;
    broadcastOutput("OLED mode: IMU Action Detection");
  } else if (mode == "gps") {
    currentOLEDMode = OLED_GPS_DATA;
    broadcastOutput("OLED mode: GPS Data");
  } else if (mode == "fmradio") {
    currentOLEDMode = OLED_FM_RADIO;
    broadcastOutput("OLED mode: FM Radio");
  } else if (mode == "files" || mode == "filebrowser" || mode == "fb") {
    currentOLEDMode = OLED_FILE_BROWSER;
    resetOLEDFileBrowser();
    broadcastOutput("OLED mode: File Browser");
  } else if (mode == "automations" || mode == "auto") {
    currentOLEDMode = OLED_AUTOMATIONS;
    broadcastOutput("OLED mode: Automations");
  } else if (mode == "memory" || mode == "mem") {
    currentOLEDMode = OLED_MEMORY_STATS;
    broadcastOutput("OLED mode: Memory Stats");
  } else if (mode == "espnow" || mode == "mesh") {
    currentOLEDMode = OLED_ESPNOW;
#if ENABLE_ESPNOW
    // Initialize ESP-NOW OLED state based on ESP-NOW initialization status
    if (!gEspNow || !gEspNow->initialized) {
      oledEspNowShowInitPrompt();
    } else {
      oledEspNowInit();  // Set up device list view
    }
#endif
    broadcastOutput("OLED mode: ESP-NOW");
  } else if (mode == "gamepad" || mode == "gpad") {
    currentOLEDMode = OLED_GAMEPAD_VISUAL;
    broadcastOutput("OLED mode: Gamepad Visual");
  } else if (mode == "off") {
    currentOLEDMode = OLED_OFF;
    // i2cTransactionVoid template is available via i2c_system.h
    i2cTransactionVoid(100000, 500, [&]() {
      oledDisplay->clearDisplay();
      oledDisplay->display();
    });
    broadcastOutput("OLED mode: Off (display cleared)");
  } else {
    broadcastOutput("Invalid mode. Options: menu, status, sensordata, sensorlist, gamepad, thermal, network, gps, text, logo, anim, imuactions, fmradio, files, automations, espnow, memory, off");
    return "ERROR";
  }

  updateOLEDDisplay();
  return "OK";
}

const char* cmd_oledtext(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!oledConnected) {
    broadcastOutput("OLED display not running. Use 'oledstart' first.");
    return "ERROR";
  }

  String text = args;
  text.trim();
  
  if (text.length() == 0) {
    broadcastOutput("Usage: oledtext \"Your text here\"");
    return "ERROR";
  }

  if (text.startsWith("\"") && text.endsWith("\"")) {
    text = text.substring(1, text.length() - 1);
  }

  extern String customOLEDText;
  customOLEDText = text;
  currentOLEDMode = OLED_CUSTOM_TEXT;

  if (ensureDebugBuffer()) {
    snprintf(getDebugBuffer(), 1024, "Custom text set: %s", text.c_str());
    broadcastOutput(getDebugBuffer());
  }
  updateOLEDDisplay();
  return "OK";
}

const char* cmd_oledclear(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!oledConnected) {
    broadcastOutput("OLED display not running. Use 'oledstart' first.");
    return "ERROR";
  }

  // i2cTransactionVoid template is available via i2c_system.h
  i2cTransactionVoid(100000, 500, [&]() {
    oledDisplay->clearDisplay();
    oledDisplay->display();
  });

  broadcastOutput("OLED display cleared");
  return "OK";
}

const char* cmd_oledstatus(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!oledConnected) {
    broadcastOutput("OLED display: Not connected");
    return "OK";
  }

  broadcastOutput("OLED display: Connected");
  if (ensureDebugBuffer()) {
    snprintf(getDebugBuffer(), 1024, "Address: 0x%02X", OLED_I2C_ADDRESS);
    broadcastOutput(getDebugBuffer());
    snprintf(getDebugBuffer(), 1024, "Resolution: %dx%d", SCREEN_WIDTH, SCREEN_HEIGHT);
    broadcastOutput(getDebugBuffer());
    snprintf(getDebugBuffer(), 1024, "Enabled: %s", oledEnabled ? "Yes" : "No");
    broadcastOutput(getDebugBuffer());

    String modeStr;
    switch (currentOLEDMode) {
      case OLED_SYSTEM_STATUS: modeStr = "System Status"; break;
      case OLED_SENSOR_DATA: modeStr = "Sensor Data"; break;
      case OLED_SENSOR_LIST: modeStr = "Sensor List"; break;
      case OLED_THERMAL_VISUAL: modeStr = "Thermal Visual"; break;
      case OLED_GAMEPAD_VISUAL: modeStr = "Gamepad Visual"; break;
      case OLED_NETWORK_INFO: modeStr = "Network Info"; break;
      case OLED_MESH_STATUS: modeStr = "Mesh Status"; break;
      case OLED_CUSTOM_TEXT: modeStr = "Custom Text"; break;
      case OLED_LOGO: modeStr = "Logo"; break;
      case OLED_ANIMATION: modeStr = "Animation"; break;
      case OLED_FILE_BROWSER: modeStr = "File Browser"; break;
      case OLED_OFF: modeStr = "Off"; break;
      default: modeStr = "Unknown"; break;
    }
    snprintf(getDebugBuffer(), 1024, "Mode: %s", modeStr.c_str());
    broadcastOutput(getDebugBuffer());

    if (currentOLEDMode == OLED_ANIMATION) {
      for (int i = 0; i < gAnimationCount; i++) {
        if (gAnimationRegistry[i].type == currentAnimation) {
          snprintf(getDebugBuffer(), 1024, "Current Animation: %s", gAnimationRegistry[i].name);
          broadcastOutput(getDebugBuffer());
          snprintf(getDebugBuffer(), 1024, "Animation FPS: %d", animationFPS);
          broadcastOutput(getDebugBuffer());
          break;
        }
      }
    }
  }

  return "OK";
}

const char* cmd_oledanim(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!oledConnected) {
    broadcastOutput("OLED display not running. Use 'oledstart' first.");
    return "ERROR";
  }

  String arg = args;
  arg.trim();
  
  if (arg.length() == 0) {
    broadcastOutput("Available animations:");
    for (int i = 0; i < gAnimationCount; i++) {
      if (ensureDebugBuffer()) {
        snprintf(getDebugBuffer(), 1024, "  %s - %s", gAnimationRegistry[i].name, gAnimationRegistry[i].description);
        broadcastOutput(getDebugBuffer());
      }
    }
    broadcastOutput("");
    broadcastOutput("Usage: oledanim <name>");
    broadcastOutput("       oledanim fps <1-60>");
    return "OK";
  }

  arg.toLowerCase();

  if (arg.startsWith("fps ")) {
    int fps = arg.substring(4).toInt();
    if (fps < 1 || fps > 60) {
      broadcastOutput("FPS must be between 1 and 60");
      return "ERROR";
    }
    animationFPS = fps;
    if (ensureDebugBuffer()) {
      snprintf(getDebugBuffer(), 1024, "Animation FPS set to %d", animationFPS);
      broadcastOutput(getDebugBuffer());
    }
    return "OK";
  }

  bool found = false;
  for (int i = 0; i < gAnimationCount; i++) {
    if (arg == gAnimationRegistry[i].name) {
      currentAnimation = gAnimationRegistry[i].type;
      currentOLEDMode = OLED_ANIMATION;
      animationFrame = 0;
      if (ensureDebugBuffer()) {
        snprintf(getDebugBuffer(), 1024, "Animation set to: %s", gAnimationRegistry[i].description);
        broadcastOutput(getDebugBuffer());
      }
      updateOLEDDisplay();
      found = true;
      break;
    }
  }

  if (!found) {
    broadcastOutput("Unknown animation. Use 'oledanim' to list available animations.");
    return "ERROR";
  }

  return "OK";
}

// Mode name lookup for CLI
static const char* getOLEDModeName(OLEDMode mode) {
  switch (mode) {
    case OLED_OFF: return "off";
    case OLED_MENU: return "menu";
    case OLED_SYSTEM_STATUS: return "status";
    case OLED_SENSOR_DATA: return "sensordata";
    case OLED_SENSOR_LIST: return "sensorlist";
    case OLED_THERMAL_VISUAL: return "thermal";
    case OLED_NETWORK_INFO: return "network";
    case OLED_MESH_STATUS: return "mesh";
    case OLED_CUSTOM_TEXT: return "text";
    case OLED_UNAVAILABLE: return "unavailable";
    case OLED_LOGO: return "logo";
    case OLED_ANIMATION: return "animation";
    case OLED_BOOT_SENSORS: return "boot";
    case OLED_IMU_ACTIONS: return "imu";
    case OLED_GPS_DATA: return "gps";
    case OLED_FM_RADIO: return "fmradio";
    case OLED_FILE_BROWSER: return "files";
    case OLED_AUTOMATIONS: return "automations";
    case OLED_ESPNOW: return "espnow";
    case OLED_TOF_DATA: return "tof";
    case OLED_APDS_DATA: return "apds";
    case OLED_POWER: return "power";
    case OLED_POWER_CPU: return "power_cpu";
    case OLED_POWER_SLEEP: return "power_sleep";
    case OLED_GAMEPAD_VISUAL: return "gamepad";
    case OLED_BLUETOOTH: return "bluetooth";
    case OLED_REMOTE_SENSORS: return "remote";
    case OLED_MEMORY_STATS: return "memory";
    case OLED_WEB_STATS: return "web";
    case OLED_RTC_DATA: return "rtc";
    case OLED_PRESENCE_DATA: return "presence";
    default: return "unknown";
  }
}

static OLEDMode getOLEDModeByName(const String& name) {
  if (name == "off") return OLED_OFF;
  if (name == "menu") return OLED_MENU;
  if (name == "status") return OLED_SYSTEM_STATUS;
  if (name == "sensordata") return OLED_SENSOR_DATA;
  if (name == "sensorlist") return OLED_SENSOR_LIST;
  if (name == "thermal") return OLED_THERMAL_VISUAL;
  if (name == "network") return OLED_NETWORK_INFO;
  if (name == "mesh") return OLED_MESH_STATUS;
  if (name == "text") return OLED_CUSTOM_TEXT;
  if (name == "logo") return OLED_LOGO;
  if (name == "animation") return OLED_ANIMATION;
  if (name == "imu") return OLED_IMU_ACTIONS;
  if (name == "gps") return OLED_GPS_DATA;
  if (name == "fmradio") return OLED_FM_RADIO;
  if (name == "files") return OLED_FILE_BROWSER;
  if (name == "automations") return OLED_AUTOMATIONS;
  if (name == "espnow") return OLED_ESPNOW;
  if (name == "tof") return OLED_TOF_DATA;
  if (name == "apds") return OLED_APDS_DATA;
  if (name == "power") return OLED_POWER;
  if (name == "gamepad" || name == "gpad") return OLED_GAMEPAD_VISUAL;
  if (name == "bluetooth") return OLED_BLUETOOTH;
  if (name == "remote") return OLED_REMOTE_SENSORS;
  if (name == "memory" || name == "mem") return OLED_MEMORY_STATS;
  if (name == "web") return OLED_WEB_STATS;
  if (name == "rtc") return OLED_RTC_DATA;
  if (name == "presence") return OLED_PRESENCE_DATA;
  return (OLEDMode)-1;  // Invalid
}

const char* cmd_oledlayout(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = argsIn;
  args.trim();
  
  if (args.length() == 0) {
    // No argument - show all mode layouts
    broadcastOutput("=== OLED Mode Layouts ===");
    broadcastOutput(String("Current mode: ") + getOLEDModeName(currentOLEDMode) + " (layout " + getCurrentModeLayout() + ")");
    broadcastOutput("");
    broadcastOutput("Usage: oledlayout [mode] <layout>");
    broadcastOutput("  oledlayout <0-9>        - Set current mode layout");
    broadcastOutput("  oledlayout menu 1       - Set menu to list layout");
    broadcastOutput("  oledlayout toggle       - Toggle current mode layout");
    broadcastOutput("  oledlayout show         - Show all mode layouts");
    return "OK";
  }
  
  // Check for special commands
  if (args == "toggle" || args == "t") {
    int current = getCurrentModeLayout();
    setOLEDModeLayout(currentOLEDMode, (current == 0) ? 1 : 0);
    broadcastOutput(String(getOLEDModeName(currentOLEDMode)) + " layout toggled to: " + getCurrentModeLayout());
    updateOLEDDisplay();
    return "OK";
  }
  
  if (args == "show") {
    broadcastOutput("=== Mode Layouts ===");
    for (int i = 0; i <= OLED_GAMEPAD_VISUAL; i++) {
      OLEDMode m = (OLEDMode)i;
      int layout = getOLEDModeLayout(m);
      if (layout != 0) {  // Only show non-default layouts
        broadcastOutput(String("  ") + getOLEDModeName(m) + ": " + layout);
      }
    }
    broadcastOutput(String("Current: ") + getOLEDModeName(currentOLEDMode) + " = " + getCurrentModeLayout());
    return "OK";
  }
  
  // Check if first arg is a number (layout for current mode)
  if (args.length() == 1 && isDigit(args[0])) {
    int layout = args.toInt();
    setOLEDModeLayout(currentOLEDMode, layout);
    broadcastOutput(String(getOLEDModeName(currentOLEDMode)) + " layout set to: " + layout);
    updateOLEDDisplay();
    return "OK";
  }
  
  // Check for "mode layout" format
  int secondSpace = args.indexOf(' ');
  if (secondSpace > 0) {
    String modeName = args.substring(0, secondSpace);
    String layoutStr = args.substring(secondSpace + 1);
    modeName.trim();
    modeName.toLowerCase();
    layoutStr.trim();
    
    OLEDMode mode = getOLEDModeByName(modeName);
    if ((int)mode < 0) {
      broadcastOutput(String("Unknown mode: ") + modeName);
      return "ERROR";
    }
    
    int layout = layoutStr.toInt();
    setOLEDModeLayout(mode, layout);
    broadcastOutput(String(getOLEDModeName(mode)) + " layout set to: " + layout);
    
    // Force redraw if we changed current mode's layout
    if (mode == currentOLEDMode) {
      updateOLEDDisplay();
    }
    return "OK";
  }
  
  // Legacy: grid/list for menu
  args.toLowerCase();
  if (args == "grid") {
    setOLEDModeLayout(OLED_MENU, 0);
    broadcastOutput("Menu layout set to: grid (0)");
  } else if (args == "list") {
    setOLEDModeLayout(OLED_MENU, 1);
    broadcastOutput("Menu layout set to: list (1)");
  } else {
    broadcastOutput("Unknown argument. Use: oledlayout [mode] <layout>");
    return "ERROR";
  }
  
  if (currentOLEDMode == OLED_MENU) {
    updateOLEDDisplay();
  }
  return "OK";
}

// ============================================================================
// Boot State Variables (moved from .ino)
// ============================================================================

bool oledBootModeActive = false;
enum OLEDBootPhase {
  BOOT_PHASE_ANIMATION,
  BOOT_PHASE_LOGO,
  BOOT_PHASE_SENSORS,
  BOOT_PHASE_COMPLETE
};
OLEDBootPhase currentBootPhase = BOOT_PHASE_ANIMATION;
unsigned long bootPhaseStartTime = 0;
int bootProgressPercent = 0;
String bootProgressLabel = "";

// Menu navigation state (declared early for boot sequence access)
int oledMenuSelectedIndex = 0;
int oledSensorMenuSelectedIndex = 0;
static OLEDMode previousOLEDMode = OLED_SYSTEM_STATUS;

// External dependencies for boot logic
extern const char* USERS_JSON_FILE;
extern int connectedDeviceCount;
extern ConnectedDevice connectedDevices[];

// ============================================================================
// OLED Animation System - moved to OLED_Mode_Animations.cpp
// ============================================================================
// gAnimationRegistry, gAnimationCount, and displayAnimation() are now
// defined in OLED_Mode_Animations.cpp


// ============================================================================
// Boot Sequence Helper Functions (for setup() and loop())
// ============================================================================

// Early OLED initialization during setup() - probes and initializes for boot animation
// Returns true if OLED was detected and initialized
bool earlyOLEDInit() {
  // Early exit if I2C bus is disabled
  if (!gI2CBusEnabled) {
    DEBUG_SENSORSF("OLED init skipped - I2C bus disabled");
    oledConnected = false;
    oledEnabled = false;
    return false;
  }
  
  extern TwoWire Wire1;
  
  // Try both common OLED addresses: 0x3D (default) and 0x3C (alternate)
  uint8_t oledAddresses[] = {0x3D, 0x3C};
  uint8_t detectedAddr = 0;
  
  for (uint8_t addr : oledAddresses) {
    DEBUG_SENSORSF("Probing for OLED at 0x%02X on Wire1 (SDA=%d, SCL=%d)", addr, gSettings.i2cSdaPin, gSettings.i2cSclPin);
    uint8_t probeResult = i2cProbeAddress(addr, 100000, 200);
    DEBUG_SENSORSF("OLED probe at 0x%02X result: %d (0=found, 2=NACK)", addr, probeResult);
    if (probeResult == 0) {
      detectedAddr = addr;
      break;
    }
  }

  if (detectedAddr != 0) {
    DEBUG_SENSORSF("OLED detected at 0x%02X - initializing for boot animation", detectedAddr);

    // Use Display HAL's gDisplay (oledDisplay is a macro alias for gDisplay in Display_HAL.h)
    extern DisplayDriver* gDisplay;
    if (!gDisplay) {
      gDisplay = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, OLED_RESET);
    }
    
    if (gDisplay && gDisplay->begin(SSD1306_SWITCHCAPVCC, detectedAddr)) {
      oledConnected = true;
      oledEnabled = true;

      // Set rotation (0 = normal, 2 = 180 degrees)
      oledDisplay->setRotation(2);

      // Start boot animation immediately
      currentBootPhase = BOOT_PHASE_ANIMATION;
      bootPhaseStartTime = millis();
      oledBootModeActive = true;

      currentOLEDMode = OLED_ANIMATION;
      currentAnimation = ANIM_BOOT_PROGRESS;
      animationFrame = 0;
      animationLastUpdate = millis();

      // Initialize boot progress
      bootProgressPercent = 0;
      bootProgressLabel = "Initializing...";

      // Clear display and render first animation frame (I2C-safe)
      i2cTransactionVoid(100000, 500, [&]() {
        oledDisplay->clearDisplay();
        displayAnimation();
        oledDisplay->display();
      });

      DEBUG_SENSORSF("OLED boot animation started at 0x%02X", detectedAddr);
      
      // Register OLED for I2C health tracking
      i2cRegisterDevice(detectedAddr, "OLED");
      return true;
    }
  }
  
  DEBUG_SENSORSF("OLED not detected or initialization failed");
  return false;
}

// Process boot sequence phase transitions in loop()
// Call this from loop() when oledBootModeActive is true
void processOLEDBootSequence() {
  if (!oledBootModeActive || !oledConnected || !oledEnabled) {
    return;
  }

  unsigned long now = millis();
  unsigned long elapsed = now - bootPhaseStartTime;

  // Phase durations (in milliseconds)
  const unsigned long LOGO_DURATION = 5000;       // 5 seconds
  const unsigned long SENSORS_DURATION = 3000;    // 3 seconds

  switch (currentBootPhase) {
    case BOOT_PHASE_ANIMATION:
      // Show boot progress animation until 100% complete, then wait 1 second
      if (bootProgressPercent >= 100 && elapsed >= 1000) {
        OLEDMode prevMode = currentOLEDMode;
        currentBootPhase = BOOT_PHASE_LOGO;
        bootPhaseStartTime = now;
        currentOLEDMode = OLED_LOGO;
        debugOLEDModeChange("boot.phase.animation->logo", prevMode, currentOLEDMode, "");
        DEBUG_SENSORSF("OLED boot sequence: Animation -> Logo");
      }
      break;

    case BOOT_PHASE_LOGO:
      // Show logo for 5 seconds
      if (elapsed >= LOGO_DURATION) {
        OLEDMode prevMode = currentOLEDMode;
        currentBootPhase = BOOT_PHASE_SENSORS;
        bootPhaseStartTime = now;
        currentOLEDMode = OLED_BOOT_SENSORS;
        debugOLEDModeChange("boot.phase.logo->sensors", prevMode, currentOLEDMode, "");
        DEBUG_SENSORSF("OLED boot sequence: Logo -> Sensors");
      }
      break;

    case BOOT_PHASE_SENSORS:
      // Show connected sensors - duration scales with sensor count
      {
        int connectedCount = 0;
        for (int i = 0; i < connectedDeviceCount; i++) {
          if (connectedDevices[i].isConnected) connectedCount++;
        }
        unsigned long dynamicDuration = SENSORS_DURATION + (connectedCount * 1000);  // +1s per sensor
        
        if (elapsed >= dynamicDuration) {
          currentBootPhase = BOOT_PHASE_COMPLETE;
          oledBootModeActive = false;

          // Only transition if user hasn't manually changed mode during boot
          if (userOverrodeBootMode) {
            Serial.printf("[OLED_MODE] boot.complete: User overrode boot, keeping mode %d\n", (int)currentOLEDMode);
            DEBUG_SENSORSF("OLED boot sequence complete (user overrode, keeping current mode)");
          } else {
            OLEDMode prevMode = currentOLEDMode;
            
            // After boot completes, go to login screen if auth is required
            if (gSettings.localDisplayRequireAuth && !gLocalDisplayAuthed) {
              currentOLEDMode = OLED_LOGIN;
              previousOLEDMode = OLED_MENU;  // After login, B button goes to menu
              debugOLEDModeChange("boot.complete.login", prevMode, currentOLEDMode, "Auth required");
              DEBUG_SENSORSF("OLED boot sequence: Sensors -> Login (auth required)");
            } else {
              // No auth required or already authed - go to default mode
              String defaultMode = gSettings.oledDefaultMode;
              defaultMode.toLowerCase();
              
              // Set previousOLEDMode to MENU so B button returns to menu
              previousOLEDMode = OLED_MENU;
              
              if (defaultMode == "status") currentOLEDMode = OLED_SYSTEM_STATUS;
              else if (defaultMode == "sensordata") currentOLEDMode = OLED_SENSOR_DATA;
              else if (defaultMode == "sensorlist") currentOLEDMode = OLED_SENSOR_LIST;
              else if (defaultMode == "thermal") currentOLEDMode = OLED_THERMAL_VISUAL;
              else if (defaultMode == "network") currentOLEDMode = OLED_NETWORK_INFO;
              else if (defaultMode == "mesh") currentOLEDMode = OLED_MESH_STATUS;
              else if (defaultMode == "logo") currentOLEDMode = OLED_LOGO;
              else currentOLEDMode = OLED_SYSTEM_STATUS;

              debugOLEDModeChange("boot.complete.defaultMode", prevMode, currentOLEDMode, String("defaultMode=") + defaultMode);
              DEBUG_SENSORSF("OLED boot sequence: Sensors -> %s (complete, B returns to menu)", defaultMode.c_str());
            }
            
            // Auto-start gamepad if setting is enabled and I2C bus is enabled
            if (gSettings.gamepadAutoStart && gSettings.i2cBusEnabled) {
              tryAutoStartGamepadForMenu();
            }
          }
        }
      }
      break;

    case BOOT_PHASE_COMPLETE:
      // Should not reach here, but just in case
      oledBootModeActive = false;
      break;
  }
}

// ============================================================================
// OLED Menu System (App Launcher with Icons)
// ============================================================================

// Menu items definition - maps apps to icons and modes
const OLEDMenuItem oledMenuItems[] = {
  { "System",     "notify_system",     OLED_SYSTEM_STATUS },
  { "Sensors",    "notify_sensor",     OLED_SENSOR_MENU },
  { "Memory",     "memory",            OLED_MEMORY_STATS },
  { "Settings",   "settings",          OLED_SETTINGS },
  { "Network",    "notify_server",     OLED_NETWORK_INFO },
  { "ESP-NOW",    "notify_espnow",     OLED_ESPNOW },
  { "Bluetooth",  "bt_idle",           OLED_BLUETOOTH },
#if ENABLE_AUTOMATION
  { "Automations","notify_automation", OLED_AUTOMATIONS },
#endif
  { "Files",      "notify_files",      OLED_FILE_BROWSER },
  { "Map",        "compass",           OLED_GPS_MAP },
  { "Web",        "notify_server",     OLED_WEB_STATS },
  { "Login",      "user",              OLED_LOGIN },
  { "CLI Output", "terminal",          OLED_CLI_VIEWER },
  { "Logging",    "file_text",         OLED_LOGGING },
  { "Logo",       "device",            OLED_LOGO },
  { "Power",      "power",             OLED_POWER },
};
const int oledMenuItemCount = sizeof(oledMenuItems) / sizeof(oledMenuItems[0]);

// Sensor submenu items (extern const for external linkage)
extern const OLEDMenuItem oledSensorMenuItems[] = {
  { "Data",       "notify_sensor",     OLED_SENSOR_DATA },
  { "List",       "notify_sensor",     OLED_SENSOR_LIST },
  { "Thermal",    "thermal",           OLED_THERMAL_VISUAL },
  { "ToF",        "tof_radar",         OLED_TOF_DATA },
  { "IMU",        "imu_axes",          OLED_IMU_ACTIONS },
  { "APDS",       "gesture",           OLED_APDS_DATA },
  { "GPS",        "compass",           OLED_GPS_DATA },
  { "Gamepad",    "gamepad",           OLED_GAMEPAD_VISUAL },
  { "FM Radio",   "radio",             OLED_FM_RADIO },
  { "RTC",        "clock",             OLED_RTC_DATA },
  { "Presence",   "notify_sensor",     OLED_PRESENCE_DATA },
};
extern const int oledSensorMenuItemCount = sizeof(oledSensorMenuItems) / sizeof(oledSensorMenuItems[0]);

// Menu layout style now uses per-mode system: oledModeLayouts[OLED_MENU]
// 0 = grid with large icons (default), 1 = list with icon on right

// MenuAvailability enum moved to OLED_Display.h

MenuAvailability getMenuAvailability(OLEDMode mode, String* outReason) {
  if (outReason) *outReason = "";

  switch (mode) {
#if ENABLE_AUTOMATION
    case OLED_AUTOMATIONS:
      if (!gSettings.automationsEnabled) {
        if (outReason) *outReason = "Disabled\nRun: automation system enable";
        return MenuAvailability::FEATURE_DISABLED;
      }
      return MenuAvailability::AVAILABLE;
#else
    case OLED_AUTOMATIONS:
      if (outReason) *outReason = "Not built";
      return MenuAvailability::NOT_BUILT;
#endif

    case OLED_ESPNOW:
#if ENABLE_ESPNOW
      // Check if ESP-NOW is actually initialized, not just enabled in settings
      if (gEspNow && gEspNow->initialized) {
        return MenuAvailability::AVAILABLE;
      }
      // If enabled but not initialized, show as unavailable with setup instructions
      if (gSettings.espnowenabled) {
        if (outReason) *outReason = "Not initialized\nPress X to setup";
        return MenuAvailability::FEATURE_DISABLED;
      }
#endif
      if (outReason) *outReason = "Disabled\nRun: espnowenabled 1\nReboot required";
      return MenuAvailability::FEATURE_DISABLED;

    
      // Sensor modes - always allow navigation, display functions handle "not active" state
      // Block if the sensor is not built at compile time or not currently detected/connected
      case OLED_THERMAL_VISUAL:
#ifndef ENABLE_THERMAL_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      if (thermalConnected) {
        return MenuAvailability::AVAILABLE;
      }
      // Check if hardware was detected during I2C scan (address 0x33)
      for (int i = 0; i < connectedDeviceCount; i++) {
        if (connectedDevices[i].address == I2C_ADDR_THERMAL && connectedDevices[i].isConnected) {
          if (outReason) *outReason = "Disabled\nPress X to start";
          return MenuAvailability::FEATURE_DISABLED;
        }
      }
      if (outReason) *outReason = "Not detected";
      return MenuAvailability::NOT_DETECTED;
#endif

    case OLED_FM_RADIO:
#ifndef ENABLE_FM_RADIO
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      if (fmRadioConnected && radioInitialized) {
        return MenuAvailability::AVAILABLE;
      }
      // Check if hardware was detected during I2C scan (address 0x11)
      for (int i = 0; i < connectedDeviceCount; i++) {
        if (connectedDevices[i].address == I2C_ADDR_FM_RADIO && connectedDevices[i].isConnected) {
          if (outReason) *outReason = "Disabled\nPress X to start";
          return MenuAvailability::FEATURE_DISABLED;
        }
      }
      if (outReason) *outReason = "Not detected";
      return MenuAvailability::NOT_DETECTED;
#endif

      case OLED_GPS_DATA:
#ifndef ENABLE_GPS_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      // Check if GPS is running
      if (gpsConnected && gpsEnabled) {
        return MenuAvailability::AVAILABLE;
      }
      // Check if hardware was detected during I2C scan (address 0x10)
      for (int i = 0; i < connectedDeviceCount; i++) {
        if (connectedDevices[i].address == I2C_ADDR_GPS && connectedDevices[i].isConnected) {
          if (outReason) *outReason = "Disabled\nPress X to start";
          return MenuAvailability::FEATURE_DISABLED;
        }
      }
      if (outReason) *outReason = "Not detected";
      return MenuAvailability::NOT_DETECTED;
#endif

      case OLED_IMU_ACTIONS:
#ifndef ENABLE_IMU_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      if (imuConnected) {
        return MenuAvailability::AVAILABLE;
      }
      // Check if hardware was detected during I2C scan (address 0x28)
      for (int i = 0; i < connectedDeviceCount; i++) {
        if (connectedDevices[i].address == I2C_ADDR_IMU && connectedDevices[i].isConnected) {
          if (outReason) *outReason = "Disabled\nPress X to start";
          return MenuAvailability::FEATURE_DISABLED;
        }
      }
      if (outReason) *outReason = "Not detected";
      return MenuAvailability::NOT_DETECTED;
#endif

      case OLED_TOF_DATA:
#ifndef ENABLE_TOF_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      if (tofConnected) {
        return MenuAvailability::AVAILABLE;
      }
      // Check if hardware was detected during I2C scan (address 0x29)
      for (int i = 0; i < connectedDeviceCount; i++) {
        if (connectedDevices[i].address == I2C_ADDR_TOF && connectedDevices[i].isConnected) {
          if (outReason) *outReason = "Disabled\nPress X to start";
          return MenuAvailability::FEATURE_DISABLED;
        }
      }
      if (outReason) *outReason = "Not detected";
      return MenuAvailability::NOT_DETECTED;
#endif

      case OLED_APDS_DATA:
#ifndef ENABLE_APDS_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      if (apdsConnected) {
        return MenuAvailability::AVAILABLE;
      }
      // Check if hardware was detected during I2C scan (address 0x39)
      for (int i = 0; i < connectedDeviceCount; i++) {
        if (connectedDevices[i].address == I2C_ADDR_APDS && connectedDevices[i].isConnected) {
          if (outReason) *outReason = "Disabled\nPress X to start";
          return MenuAvailability::FEATURE_DISABLED;
        }
      }
      if (outReason) *outReason = "Not detected";
      return MenuAvailability::NOT_DETECTED;
#endif

      case OLED_GAMEPAD_VISUAL:
#ifndef ENABLE_GAMEPAD_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      if (gamepadConnected) {
        return MenuAvailability::AVAILABLE;
      }
      // Check if hardware was detected during I2C scan (address 0x50)
      for (int i = 0; i < connectedDeviceCount; i++) {
        if (connectedDevices[i].address == I2C_ADDR_GAMEPAD && connectedDevices[i].isConnected) {
          if (outReason) *outReason = "Disabled\nPress X to start";
          return MenuAvailability::FEATURE_DISABLED;
        }
      }
      if (outReason) *outReason = "Not detected";
      return MenuAvailability::NOT_DETECTED;
#endif

      case OLED_RTC_DATA:
#ifndef ENABLE_RTC_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      if (rtcConnected) {
        return MenuAvailability::AVAILABLE;
      }
      // Check if hardware was detected during I2C scan (address 0x68)
      for (int i = 0; i < connectedDeviceCount; i++) {
        if (connectedDevices[i].address == I2C_ADDR_DS3231 && connectedDevices[i].isConnected) {
          if (outReason) *outReason = "Disabled\nPress X to start";
          return MenuAvailability::FEATURE_DISABLED;
        }
      }
      if (outReason) *outReason = "Not detected";
      return MenuAvailability::NOT_DETECTED;
#endif

      case OLED_PRESENCE_DATA:
#if !ENABLE_PRESENCE_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      {
        extern bool presenceConnected;
        if (presenceConnected) {
          return MenuAvailability::AVAILABLE;
        }
        // Check if hardware was detected during I2C scan (address 0x5A)
        for (int i = 0; i < connectedDeviceCount; i++) {
          if (connectedDevices[i].address == I2C_ADDR_PRESENCE && connectedDevices[i].isConnected) {
            if (outReason) *outReason = "Disabled\nPress X to start";
            return MenuAvailability::FEATURE_DISABLED;
          }
        }
        if (outReason) *outReason = "Not detected";
        return MenuAvailability::NOT_DETECTED;
      }
#endif

    case OLED_BLUETOOTH:
#if !ENABLE_BLUETOOTH
      if (outReason) *outReason = "Not built";
      return MenuAvailability::NOT_BUILT;
#else
      // Check if Bluetooth is initialized at runtime
      if (!gBLEState || !gBLEState->initialized) {
        if (outReason) *outReason = "Disabled\nRun: blestart";
        return MenuAvailability::FEATURE_DISABLED;
      }
      return MenuAvailability::AVAILABLE;
#endif

    case OLED_WEB_STATS:
#ifndef ENABLE_WIFI
      if (outReason) *outReason = "Not built";
      return MenuAvailability::NOT_BUILT;
#endif
      // Check if HTTP server is running
      {
        extern httpd_handle_t server;
        if (!server) {
          if (outReason) *outReason = "Disabled\nRun: httpstart";
          return MenuAvailability::FEATURE_DISABLED;
        }
      }
      return MenuAvailability::AVAILABLE;

    default:
      return MenuAvailability::AVAILABLE;
  }
}

// Battery icon state for main menu (updated every 2 minutes)
struct BatteryIconState {
  float percentage;
  char icon;
  unsigned long lastUpdateMs;
  bool valid;
};
BatteryIconState batteryIconState = {0};
extern const unsigned long BATTERY_ICON_UPDATE_INTERVAL = 120000; // 2 minutes

// displayMenu() moved to OLED_Mode_Menu.cpp

// displayMenuListStyle() moved to OLED_Mode_Menu.cpp
// displaySensorMenu() moved to OLED_Mode_Menu.cpp

// displayAutomations() moved to OLED_Mode_Menu.cpp
// displayEspNow() moved to OLED_Mode_Network.cpp

/**
 * Menu navigation functions
 */
void oledMenuUp() {
  if (oledMenuSelectedIndex > 0) {
    oledMenuSelectedIndex--;
  } else {
    oledMenuSelectedIndex = oledMenuItemCount - 1; // Wrap to end
  }
}

void oledMenuDown() {
  if (oledMenuSelectedIndex < oledMenuItemCount - 1) {
    oledMenuSelectedIndex++;
  } else {
    oledMenuSelectedIndex = 0; // Wrap to start
  }
}

// pushOLEDMode and popOLEDMode are declared in OLED_Display.h

// Logging mode state enum and variables (used by oledMenuSelect)
enum LoggingMenuState : uint8_t {
  LOG_MENU_MAIN,
  LOG_MENU_SENSOR,
  LOG_MENU_SYSTEM,
  LOG_MENU_SENSOR_CONFIG,
  LOG_MENU_VIEWER
};
LoggingMenuState loggingCurrentState = LOG_MENU_MAIN;
int loggingMenuSelection = 0;

void oledMenuSelect() {
  if (oledMenuSelectedIndex >= 0 && oledMenuSelectedIndex < oledMenuItemCount) {
    
    OLEDMode target = oledMenuItems[oledMenuSelectedIndex].targetMode;
    Serial.printf("[MENU_SELECT] sel=%d target=%d name='%s'\n", oledMenuSelectedIndex, (int)target, oledMenuItems[oledMenuSelectedIndex].name);
    Serial.printf("[MENU_SELECT] ENUM CHECK - OLED_LOGIN=%d OLED_CLI_VIEWER=%d OLED_LOGGING=%d\n", (int)OLED_LOGIN, (int)OLED_CLI_VIEWER, (int)OLED_LOGGING);
    Serial.printf("[MENU_SELECT] sizeof(OLEDMode)=%d sizeof(target)=%d\n", sizeof(OLEDMode), sizeof(target));
    String reason;
    MenuAvailability availability = getMenuAvailability(target, &reason);
    if (availability != MenuAvailability::AVAILABLE) {
      if (reason.length() == 0) {
        switch (availability) {
          case MenuAvailability::FEATURE_DISABLED: reason = "Disabled"; break;
          case MenuAvailability::NOT_DETECTED: reason = "Not detected"; break;
          case MenuAvailability::NOT_BUILT: reason = "Not built"; break;
          default: reason = "Unavailable"; break;
        }
      }
      broadcastOutput(String("[OLED] ") + oledMenuItems[oledMenuSelectedIndex].name + ": " + reason);

      enterUnavailablePage(oledMenuItems[oledMenuSelectedIndex].name, reason);
      return;
    }

    pushOLEDMode(currentOLEDMode);
    Serial.printf("[MENU_SELECT] Setting currentOLEDMode from %d to %d\n", (int)currentOLEDMode, (int)target);
    currentOLEDMode = target;
    Serial.printf("[MENU_SELECT] currentOLEDMode now = %d\n", (int)currentOLEDMode);

#if ENABLE_ESPNOW
    if (currentOLEDMode == OLED_ESPNOW) {
      if (!gEspNow || !gEspNow->initialized) {
        oledEspNowShowInitPrompt();
      } else {
        oledEspNowInit();
      }
    }
#endif
    
    // Reset file browser if entering that mode
    if (currentOLEDMode == OLED_FILE_BROWSER) {
      oledFileBrowserNeedsInit = true;
    }
    
    // Reset logging mode state when entering from main menu
    if (currentOLEDMode == OLED_LOGGING) {
      loggingCurrentState = LOG_MENU_MAIN;
      loggingMenuSelection = 0;
    }
  }
}

// Push current mode onto stack before navigating to new mode
void pushOLEDMode(OLEDMode mode) {
  if (modeStackDepth < OLED_MODE_STACK_SIZE) {
    modeStack[modeStackDepth++] = mode;
  }
}

// Pop previous mode from stack for back navigation
OLEDMode popOLEDMode() {
  if (modeStackDepth > 0) {
    return modeStack[--modeStackDepth];
  }
  return OLED_MENU;  // Default fallback
}

// Get previous OLED mode for back navigation (for compatibility)
OLEDMode getPreviousOLEDMode() {
  if (modeStackDepth > 0) {
    return modeStack[modeStackDepth - 1];
  }
  return OLED_MENU;
}

// Pop mode from stack (exported for use by other files like quick settings)
OLEDMode popOLEDModeStack() {
  return popOLEDMode();
}

void oledMenuBack() {
  // Block navigation if authentication is required and user is not logged in
  extern Settings gSettings;
  extern bool gLocalDisplayAuthed;
  extern String gLocalDisplayUser;
  
  if (gSettings.localDisplayRequireAuth && !gLocalDisplayAuthed) {
    // User must be on login screen - don't allow navigation away
    if (currentOLEDMode != OLED_LOGIN) {
      currentOLEDMode = OLED_LOGIN;
    }
    Serial.println("[OLED_MENU_BACK] Navigation blocked - authentication required");
    return;
  }
  
  // Pop from mode stack to go back
  if (currentOLEDMode != OLED_MENU) {
    // Reset file browser initialization when exiting file browser
    if (currentOLEDMode == OLED_FILE_BROWSER) {
      oledFileBrowserNeedsInit = true;
    }
    currentOLEDMode = popOLEDMode();
  }
}

void resetOLEDMenu() {
  oledMenuSelectedIndex = 0;
}

// ============================================================================
// Gamepad Input for OLED Menu Navigation
// ============================================================================

#if ENABLE_GAMEPAD_SENSOR

// Gamepad navigation state
static unsigned long lastGamepadNavTime = 0;
static const unsigned long GAMEPAD_NAV_DEBOUNCE = 100; // ms between nav actions (reduced for responsiveness)
static uint32_t lastButtonState = 0xFFFFFFFF;  // Start with all buttons unpressed (active-low)
static bool lastButtonStateInitialized = false;

// Auto-repeat timing for menu navigation (faster scrolling when held)
static unsigned long lastMoveTimeX = 0;
static unsigned long lastMoveTimeY = 0;
static bool wasDeflectedX = false;
static bool wasDeflectedY = false;
static const unsigned long MENU_INITIAL_DELAY_MS = 200;  // Delay before auto-repeat starts
static const unsigned long MENU_REPEAT_DELAY_MS = 100;   // Delay between repeated movements

// Centralized navigation events - computed once per frame, used by all handlers
NavEvents gNavEvents = {false, false, false, false, 0, 0};

// Debug throttle for gamepad menu input
static unsigned long lastGamepadDebugTime = 0;
static const unsigned long GAMEPAD_DEBUG_INTERVAL = 10000; // Log every 10 seconds max (reduce spam)

// Current input state (for input helper functions)
static int gCurrentJoyX = 0;
static int gCurrentJoyY = 0;
static uint32_t gCurrentButtons = 0xFFFFFFFF;
static bool gInputStateValid = false;

/**
 * Update input state from gamepad cache (thread-safe)
 * Call this at the start of each input polling cycle
 */
void updateInputState() {
#if ENABLE_GAMEPAD_SENSOR
  if (!gControlCache.mutex) {
    gInputStateValid = false;
    return;
  }
  
  if (xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (gControlCache.gamepadDataValid) {
      gCurrentJoyX = gControlCache.gamepadX;
      gCurrentJoyY = gControlCache.gamepadY;
      gCurrentButtons = gControlCache.gamepadButtons;
      gInputStateValid = true;
    } else {
      gInputStateValid = false;
    }
    xSemaphoreGive(gControlCache.mutex);
  } else {
    gInputStateValid = false;
  }
#else
  gInputStateValid = false;
#endif
}

/**
 * Get newly pressed buttons (edge detection)
 * Returns button mask with newly pressed buttons
 */
uint32_t getNewlyPressedButtons() {
#if ENABLE_GAMEPAD_SENSOR
  if (!gInputStateValid) {
    return 0;
  }
  
  if (!lastButtonStateInitialized) {
    lastButtonState = gCurrentButtons;
    lastButtonStateInitialized = true;
    return 0;
  }
  
  // Buttons are active-low, so invert for edge detection
  uint32_t currentPressed = ~gCurrentButtons;
  uint32_t lastPressed = ~lastButtonState;
  uint32_t newlyPressed = currentPressed & ~lastPressed;
  
  lastButtonState = gCurrentButtons;
  return newlyPressed;
#else
  return 0;
#endif
}

/**
 * Get joystick delta from center position
 * deltaX: -512 to +512 (left to right)
 * deltaY: -512 to +512 (physical UP = negative, physical DOWN = positive)
 * Note: Y is inverted to match menu convention where pushing down increases values
 */
void getJoystickDelta(int& deltaX, int& deltaY) {
#if ENABLE_GAMEPAD_SENSOR
  if (!gInputStateValid) {
    deltaX = 0;
    deltaY = 0;
    return;
  }
  
  deltaX = gCurrentJoyX - JOYSTICK_CENTER;
  // Invert Y so physical DOWN produces positive values (matches menu convention)
  deltaY = JOYSTICK_CENTER - gCurrentJoyY;
#else
  deltaX = 0;
  deltaY = 0;
#endif
}

/**
 * Handle X button context-sensitive action based on current OLED mode
 * 
 * NOTE: Most modes now have their own inputFunc handler in their sensor file.
 * This function only handles special cases like OLED_UNAVAILABLE.
 */
void handleOLEDActionButton() {
  Serial.printf("[GAMEPAD_ACTION] X button pressed in mode %d\n", (int)currentOLEDMode);
  
  // Check if this mode has a registered custom input handler
  const OLEDModeEntry* registeredMode = findOLEDMode(currentOLEDMode);
  if (registeredMode && registeredMode->inputFunc) {
    // Custom handler exists - it should have already handled the input
    // This function is called as fallback, so just log and return
    Serial.println("[GAMEPAD_ACTION] Mode has custom inputFunc, skipping centralized handler");
    return;
  }
  
  // Use sensor queue for starting sensors
  extern bool enqueueSensorStart(SensorType sensor);
  extern bool isInQueue(SensorType sensor);
  
  switch (currentOLEDMode) {
    case OLED_UNAVAILABLE:
      // If feature is "Not built" (compile-time disabled), redirect to menu - no action possible
      if (unavailableOLEDReason.indexOf("Not built") >= 0) {
        currentOLEDMode = OLED_SENSOR_MENU;
        break;
      }
      
      // Try to start whatever sensor was unavailable based on the title
      if (unavailableOLEDTitle == "Thermal") {
#if ENABLE_THERMAL_SENSOR
        if (!isInQueue(SENSOR_THERMAL)) enqueueSensorStart(SENSOR_THERMAL);
        currentOLEDMode = OLED_THERMAL_VISUAL;  // Switch to thermal view
#endif
      } else if (unavailableOLEDTitle == "ToF") {
#if ENABLE_TOF_SENSOR
        if (!isInQueue(SENSOR_TOF)) enqueueSensorStart(SENSOR_TOF);
        currentOLEDMode = OLED_TOF_DATA;  // Switch to ToF view
#endif
      } else if (unavailableOLEDTitle == "IMU") {
#if ENABLE_IMU_SENSOR
        if (!isInQueue(SENSOR_IMU)) enqueueSensorStart(SENSOR_IMU);
        currentOLEDMode = OLED_IMU_ACTIONS;  // Switch to IMU view
#endif
      } else if (unavailableOLEDTitle == "APDS") {
#if ENABLE_APDS_SENSOR
        if (!isInQueue(SENSOR_APDS)) enqueueSensorStart(SENSOR_APDS);
        currentOLEDMode = OLED_APDS_DATA;  // Switch to APDS view
#endif
      } else if (unavailableOLEDTitle == "GPS") {
#if ENABLE_GPS_SENSOR
        if (!isInQueue(SENSOR_GPS)) enqueueSensorStart(SENSOR_GPS);
        currentOLEDMode = OLED_GPS_DATA;  // Switch to GPS view
#endif
      } else if (unavailableOLEDTitle == "RTC") {
#if ENABLE_RTC_SENSOR
        // Start RTC with confirmation
        static auto rtcStartConfirmedUnavail = [](void* userData) {
          (void)userData;
          executeOLEDCommand("rtcstart");
          currentOLEDMode = OLED_RTC_DATA;
        };
        oledConfirmRequest("Start RTC?", nullptr, rtcStartConfirmedUnavail, nullptr);
#endif
      } else if (unavailableOLEDTitle == "Presence") {
#if ENABLE_PRESENCE_SENSOR
        // Start Presence with confirmation
        static auto presenceStartConfirmedUnavail = [](void* userData) {
          (void)userData;
          extern bool startPresenceSensorInternal();
          startPresenceSensorInternal();
          currentOLEDMode = OLED_PRESENCE_DATA;
        };
        oledConfirmRequest("Start Presence?", nullptr, presenceStartConfirmedUnavail, nullptr);
#endif
      } else if (unavailableOLEDTitle == "FM Radio") {
        if (!isInQueue(SENSOR_FMRADIO)) enqueueSensorStart(SENSOR_FMRADIO);
        currentOLEDMode = OLED_FM_RADIO;  // Switch to FM Radio view
      } else if (unavailableOLEDTitle == "ESP-NOW") {
#if ENABLE_ESPNOW
        currentOLEDMode = OLED_ESPNOW;
        if (gSettings.espnowDeviceName.length() == 0) {
          oledEspNowShowNameKeyboard();
        } else {
          const char* initResult = cmd_espnow_init("");
          if (initResult && strstr(initResult, "initialized")) {
            oledEspNowInit();
          } else {
            oledEspNowShowInitPrompt();
          }
        }
#endif
      } else if (unavailableOLEDTitle == "Automations") {
        // Enable automation system
        executeOLEDCommand("automation system enable");
        broadcastOutput("[OLED] Automation system enabled - restart required");
        currentOLEDMode = OLED_MENU;
      } else if (unavailableOLEDTitle == "Bluetooth") {
#if ENABLE_BLUETOOTH
        // Initialize Bluetooth
        executeOLEDCommand("blestart");
        currentOLEDMode = OLED_BLUETOOTH;
#endif
      } else if (unavailableOLEDTitle == "Web") {
#if ENABLE_HTTP_SERVER
        // Start HTTP server with confirmation
        static auto httpStartConfirmedUnavail = [](void* userData) {
          (void)userData;
          executeOLEDCommand("httpstart");
          broadcastOutput("[OLED] HTTP server started");
          currentOLEDMode = OLED_WEB_STATS;
        };
        oledConfirmRequest("Start HTTP?", nullptr, httpStartConfirmedUnavail, nullptr);
#endif
      } else {
        Serial.printf("[GAMEPAD_ACTION] No action for unavailable: %s\n", unavailableOLEDTitle.c_str());
      }
      break;
      
    case OLED_WEB_STATS:
#if ENABLE_HTTP_SERVER
      {
        // Toggle HTTP server with confirmation
        extern httpd_handle_t server;
        static auto httpStopConfirmedWebStats = [](void* userData) {
          (void)userData;
          executeOLEDCommand("httpstop");
          broadcastOutput("[OLED] HTTP server stopped");
        };
        static auto httpStartConfirmedWebStats = [](void* userData) {
          (void)userData;
          executeOLEDCommand("httpstart");
          broadcastOutput("[OLED] HTTP server started");
        };
        if (server) {
          oledConfirmRequest("Stop HTTP?", nullptr, httpStopConfirmedWebStats, nullptr, false);
        } else {
          oledConfirmRequest("Start HTTP?", nullptr, httpStartConfirmedWebStats, nullptr);
        }
      }
#endif
      break;
      
    default:
      Serial.printf("[GAMEPAD_ACTION] No action defined for mode %d\n", (int)currentOLEDMode);
      break;
  }
}

/**
 * Process gamepad input for menu/app navigation
 * Call this from updateOLEDDisplay() when in menu mode
 * Returns true if input was processed
 */
bool processGamepadMenuInput() {
  unsigned long now = millis();
  bool shouldDebug = (now - lastGamepadDebugTime >= GAMEPAD_DEBUG_INTERVAL);
  
  // Check gamepad enabled/connected - silent exit when disabled (no spam)
  if (!gamepadEnabled) {
    return false;
  }
  
  // Read from gamepad cache (thread-safe)
  if (!gControlCache.mutex) {
    if (shouldDebug) {
      Serial.printf("[GAMEPAD_MENU] Exit: gControlCache.mutex is NULL addrs &en=%p &conn=%p &cache=%p\n",
                    (void*)&gamepadEnabled, (void*)&gamepadConnected, (void*)&gControlCache);
      lastGamepadDebugTime = now;
    }
    return false;
  }
  
  int joyX = 0, joyY = 0;
  uint32_t buttons = 0;
  bool dataValid = false;
  bool mutexTaken = false;
  
  if (xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    mutexTaken = true;
    if (gControlCache.gamepadDataValid) {
      joyX = gControlCache.gamepadX;
      joyY = gControlCache.gamepadY;
      buttons = gControlCache.gamepadButtons;
      dataValid = true;
    }
    xSemaphoreGive(gControlCache.mutex);
  }
  
  if (!dataValid) {
    if (shouldDebug) {
      Serial.printf("[GAMEPAD_MENU] Exit: mutexTaken=%d dataValid=%d\n", mutexTaken, dataValid);
      lastGamepadDebugTime = now;
    }
    return false;
  }
  
  // Log current state periodically
  if (shouldDebug) {
    int deltaX = joyX - JOYSTICK_CENTER;
    int deltaY = JOYSTICK_CENTER - joyY;
    Serial.printf("[GAMEPAD_MENU] joyX=%d joyY=%d dX=%d dY=%d buttons=0x%08lX mode=%d sel=%d\n",
                  joyX, joyY, deltaX, deltaY,
                  (unsigned long)buttons, currentOLEDMode, oledMenuSelectedIndex);
    lastGamepadDebugTime = now;
  }
  
  bool inputProcessed = false;
  
  // Joystick navigation (horizontal for menu grid)
  int deltaX = joyX - JOYSTICK_CENTER;
  // Compute deltaY so physical DOWN produces positive values and UP negative
  int deltaY = JOYSTICK_CENTER - joyY;

  // Check if there's any meaningful input
  bool deflectedX = abs(deltaX) > JOYSTICK_DEADZONE;
  bool deflectedY = abs(deltaY) > JOYSTICK_DEADZONE;
  bool hasJoystickInput = deflectedX || deflectedY;
  bool hasButtonChange = (buttons != lastButtonState);
  
  // Reset latch when joystick returns to center
  if (!deflectedX && wasDeflectedX) {
    wasDeflectedX = false;
    lastMoveTimeX = 0;
  }
  if (!deflectedY && wasDeflectedY) {
    wasDeflectedY = false;
    lastMoveTimeY = 0;
  }
  
  // EARLY EXIT: No input at all - skip all computation
  if (!hasJoystickInput && !hasButtonChange && !wasDeflectedX && !wasDeflectedY) {
    return false;
  }
  
  // Debounce navigation - don't update lastButtonState here or edge detection breaks!
  if (now - lastGamepadNavTime < GAMEPAD_NAV_DEBOUNCE) {
    return false;
  }
  
  // Initialize lastButtonState on first valid read AFTER debounce
  if (!lastButtonStateInitialized) {
    lastButtonState = buttons;
    lastButtonStateInitialized = true;
    Serial.printf("[GAMEPAD_INIT] Initialized lastButtonState=0x%08lX\n", (unsigned long)buttons);
    return false;  // Skip this frame to allow button state to change
  }
  
  // Reset auto-repeat state when mode changes to prevent stuck joystick
  static OLEDMode lastProcessedMode = OLED_OFF;
  if (currentOLEDMode != lastProcessedMode) {
    wasDeflectedX = false;
    wasDeflectedY = false;
    lastMoveTimeX = 0;
    lastMoveTimeY = 0;
    lastProcessedMode = currentOLEDMode;
  }
  
  // Debug button state changes
  if (shouldDebug && buttons != lastButtonState) {
    Serial.printf("[GAMEPAD_BUTTONS] buttons=0x%08lX last=0x%08lX changed=0x%08lX\n",
                  (unsigned long)buttons, (unsigned long)lastButtonState,
                  (unsigned long)(buttons ^ lastButtonState));
  }
  
  // =========================================================================
  // CENTRALIZED NAVIGATION EVENTS - computed once, used by all handlers
  // =========================================================================
  // Reset navigation events
  gNavEvents = {false, false, false, false, deltaX, deltaY};
  
  // Compute X-axis navigation with auto-repeat
  if (deflectedX) {
    bool shouldMoveX = false;
    if (!wasDeflectedX) {
      shouldMoveX = true;
      wasDeflectedX = true;
      lastMoveTimeX = now;
    } else {
      unsigned long elapsed = now - lastMoveTimeX;
      unsigned long threshold = (elapsed > MENU_INITIAL_DELAY_MS) ? MENU_REPEAT_DELAY_MS : MENU_INITIAL_DELAY_MS;
      if (elapsed >= threshold) {
        shouldMoveX = true;
        lastMoveTimeX = now;
      }
    }
    if (shouldMoveX) {
      if (deltaX > 0) {
        gNavEvents.right = true;
      } else {
        gNavEvents.left = true;
      }
    }
  }
  
  // Compute Y-axis navigation with auto-repeat
  if (deflectedY) {
    bool shouldMoveY = false;
    if (!wasDeflectedY) {
      shouldMoveY = true;
      wasDeflectedY = true;
      lastMoveTimeY = now;
    } else {
      unsigned long elapsed = now - lastMoveTimeY;
      unsigned long threshold = (elapsed > MENU_INITIAL_DELAY_MS) ? MENU_REPEAT_DELAY_MS : MENU_INITIAL_DELAY_MS;
      if (elapsed >= threshold) {
        shouldMoveY = true;
        lastMoveTimeY = now;
      }
    }
    if (shouldMoveY) {
      if (deltaY > 0) {
        gNavEvents.down = true;
      } else {
        gNavEvents.up = true;
      }
    }
  }
  // =========================================================================

  {
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtonState;
    uint32_t newlyPressed = pressedNow & ~pressedLast;
    if (oledConfirmIsActive()) {
      if (oledConfirmHandleInput(newlyPressed)) {
        inputProcessed = true;
      }
      if (inputProcessed) {
        lastGamepadNavTime = now;
      }
      lastButtonState = buttons;
      return inputProcessed;
    }
  }
  
  if (currentOLEDMode == OLED_MENU) {
    // Menu navigation depends on layout style
    // Large icon list (style 0): All directions = 1 item
    // Small icon grid (style 1): Left/Right = 1 item, Up/Down = 3 items (row)
    int verticalStep = (oledMenuLayoutStyle == 0) ? 1 : 3;
    
    // Use centralized navigation events (already computed with debounce/auto-repeat)
    if (gNavEvents.right) {
      oledMenuDown();  // Right = next item
      inputProcessed = true;
    } else if (gNavEvents.left) {
      oledMenuUp();    // Left = prev item
      inputProcessed = true;
    }
    
    if (gNavEvents.down) {
      if (oledMenuSelectedIndex + verticalStep < oledMenuItemCount) {
        oledMenuSelectedIndex += verticalStep;
      } else {
        oledMenuSelectedIndex = oledMenuItemCount - 1;
      }
      inputProcessed = true;
    } else if (gNavEvents.up) {
      if (oledMenuSelectedIndex - verticalStep >= 0) {
        oledMenuSelectedIndex -= verticalStep;
      } else {
        oledMenuSelectedIndex = 0;
      }
      inputProcessed = true;
    }
    
    // Button A (typically bit 5 or 6) - Select
    // Button B - Back
    // Seesaw gamepad buttons are active-low (0 = pressed)
    uint32_t pressedNow = ~buttons;  // Invert for active-high logic
    uint32_t pressedLast = ~lastButtonState;
    uint32_t newlyPressed = pressedNow & ~pressedLast;  // Rising edge

    if (shouldDebug && newlyPressed) {
      Serial.printf("[GAMEPAD_LOGICAL] MODE=MENU newly=0x%08lX A=%d B=%d X=%d Y=%d START=%d SEL=%d\n",
                    (unsigned long)newlyPressed,
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_A),
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_B),
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_X),
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y),
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_START),
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_SELECT));
    }
    
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      oledMenuSelect();
      inputProcessed = true;
    } else if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_SELECT)) {
      // SELECT button opens quick settings - only if authenticated
      extern Settings gSettings;
      if (!gSettings.localDisplayRequireAuth || isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)) {
        pushOLEDMode(currentOLEDMode);
        currentOLEDMode = OLED_QUICK_SETTINGS;
        inputProcessed = true;
      }
    } else if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_START)) {
      // START button toggles menu view style
      oledMenuLayoutStyle = (oledMenuLayoutStyle == 0) ? 1 : 0;
      broadcastOutput(String("[OLED] Menu style: ") + ((oledMenuLayoutStyle == 0) ? "Large Icon" : "Small Icon"));
      inputProcessed = true;
    }
    // B button does nothing in main menu (removed back option)
  } else if (currentOLEDMode == OLED_SENSOR_MENU) {
    // Sensor submenu navigation using centralized gNavEvents
    // Use visible count (excludes NOT_BUILT sensors)
    extern int getSensorMenuVisibleCount();
    extern int getSensorMenuActualIndex(int displayIndex);
    int visibleCount = getSensorMenuVisibleCount();
    
    if (gNavEvents.right) {
      oledSensorMenuSelectedIndex = (oledSensorMenuSelectedIndex + 1) % visibleCount;
      inputProcessed = true;
    } else if (gNavEvents.left) {
      oledSensorMenuSelectedIndex = (oledSensorMenuSelectedIndex - 1 + visibleCount) % visibleCount;
      inputProcessed = true;
    }
    
    if (gNavEvents.down) {
      oledSensorMenuSelectedIndex = (oledSensorMenuSelectedIndex + 1) % visibleCount;
      inputProcessed = true;
    } else if (gNavEvents.up) {
      oledSensorMenuSelectedIndex = (oledSensorMenuSelectedIndex - 1 + visibleCount) % visibleCount;
      inputProcessed = true;
    }
    
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtonState;
    uint32_t newlyPressed = pressedNow & ~pressedLast;
    
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      // Select sensor mode - use sorted index mapping
      if (oledSensorMenuSelectedIndex >= 0 && oledSensorMenuSelectedIndex < visibleCount) {
        int actualIdx = getSensorMenuActualIndex(oledSensorMenuSelectedIndex);
        OLEDMode target = oledSensorMenuItems[actualIdx].targetMode;
        String reason;
        MenuAvailability availability = getMenuAvailability(target, &reason);
        if (availability != MenuAvailability::AVAILABLE) {
          pushOLEDMode(OLED_SENSOR_MENU);  // Push so B returns here even from unavailable page
          enterUnavailablePage(oledSensorMenuItems[actualIdx].name, reason);
        } else {
          pushOLEDMode(OLED_SENSOR_MENU);  // Push so B returns here
          currentOLEDMode = target;
        }
      }
      inputProcessed = true;
    } else if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      // Back to main menu
      currentOLEDMode = OLED_MENU;
      inputProcessed = true;
    }
  } else if (currentOLEDMode == OLED_FILE_BROWSER) {
    // File browser navigation using centralized gNavEvents
    if (gNavEvents.down) {
      oledFileBrowserDown();
      inputProcessed = true;
    } else if (gNavEvents.up) {
      oledFileBrowserUp();
      inputProcessed = true;
    }
    
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtonState;
    uint32_t newlyPressed = pressedNow & ~pressedLast;

    if (shouldDebug && newlyPressed) {
      Serial.printf("[GAMEPAD_LOGICAL] MODE=FILES newly=0x%08lX A=%d B=%d X=%d Y=%d START=%d\n",
                    (unsigned long)newlyPressed,
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_A),
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_B),
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_X),
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y),
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_START));
    }
    
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      oledFileBrowserSelect();
      inputProcessed = true;
    } else if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      oledFileBrowserBack();
      inputProcessed = true;
    }
  } else if (currentOLEDMode == OLED_POWER) {
    // Power menu navigation using centralized gNavEvents
    if (gNavEvents.down) {
      powerMenuDown();
      inputProcessed = true;
    } else if (gNavEvents.up) {
      powerMenuUp();
      inputProcessed = true;
    }
    
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtonState;
    uint32_t newlyPressed = pressedNow & ~pressedLast;

    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
      executePowerAction();
      inputProcessed = true;
    } else if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      oledMenuBack();
      inputProcessed = true;
    }
  } else if (currentOLEDMode == OLED_POWER_CPU) {
    // CPU Power submenu navigation using centralized gNavEvents
    if (gNavEvents.down) {
      powerCpuDown();
      inputProcessed = true;
    } else if (gNavEvents.up) {
      powerCpuUp();
      inputProcessed = true;
    }
    
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtonState;
    uint32_t newlyPressed = pressedNow & ~pressedLast;
    
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
      executePowerCpuAction();
      inputProcessed = true;
    } else if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      oledMenuBack();
      inputProcessed = true;
    }
  } else if (currentOLEDMode == OLED_POWER_SLEEP) {
    // Sleep submenu navigation using centralized gNavEvents
    if (gNavEvents.down) {
      powerSleepDown();
      inputProcessed = true;
    } else if (gNavEvents.up) {
      powerSleepUp();
      inputProcessed = true;
    }
    
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtonState;
    uint32_t newlyPressed = pressedNow & ~pressedLast;
    
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
      executePowerSleepAction();
      inputProcessed = true;
    } else if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      oledMenuBack();
      inputProcessed = true;
    }
  } else if (currentOLEDMode == OLED_ESPNOW) {
#if ENABLE_ESPNOW
    // ESP-NOW interface navigation
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtonState;
    uint32_t newlyPressed = pressedNow & ~pressedLast;
    
    if (shouldDebug) {
      Serial.printf("[ESPNOW_BUTTONS] buttons=0x%08lX pressedNow=0x%08lX pressedLast=0x%08lX newlyPressed=0x%08lX\n",
                    (unsigned long)buttons, (unsigned long)pressedNow, 
                    (unsigned long)pressedLast, (unsigned long)newlyPressed);
      Serial.printf("[GAMEPAD_LOGICAL] MODE=ESPNOW newly=0x%08lX A=%d B=%d X=%d Y=%d START=%d\n",
                    (unsigned long)newlyPressed,
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_A),
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_B),
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_X),
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y),
                    INPUT_CHECK(newlyPressed, INPUT_BUTTON_START));
    }
    
    // Check if ESP-NOW needs initialization
    if (!gEspNow || !gEspNow->initialized) {
      // Check if keyboard is active (user is entering device name)
      if (oledKeyboardIsActive()) {
        // Let keyboard handle input
        if (oledKeyboardHandleInput(deltaX, deltaY, newlyPressed)) {
          inputProcessed = true;
        }
        
        // Check if user completed keyboard input
        if (oledKeyboardIsCompleted()) {
          const char* deviceName = oledKeyboardGetText();
          if (deviceName && strlen(deviceName) > 0) {
            broadcastOutput(String("[OLED] Setting ESP-NOW name: ") + deviceName);
            // First set the name
            const char* setnameResult = cmd_espnow_setname(String(deviceName));
            if (setnameResult && strstr(setnameResult, "Device name set")) {
              // Then initialize ESP-NOW
              broadcastOutput("[OLED] Initializing ESP-NOW...");
              const char* initResult = cmd_espnow_init("");
              if (initResult && strstr(initResult, "initialized")) {
                broadcastOutput("[OLED] ESP-NOW initialized successfully");
                // Enable ESP-NOW in settings so it persists
                gSettings.espnowenabled = 1;
                writeSettingsJson();
                // Initialize the OLED ESP-NOW interface now that ESP-NOW is ready
                oledEspNowInit();
                oledKeyboardReset();
              } else {
                broadcastOutput("[OLED] ESP-NOW initialization failed");
                oledKeyboardReset();
              }
            } else {
              broadcastOutput("[OLED] Failed to set device name");
              oledKeyboardReset();
            }
          } else {
            broadcastOutput("[OLED] Device name cannot be empty");
            oledKeyboardReset();
          }
        } else if (oledKeyboardIsCancelled()) {
          oledKeyboardReset();
        }
      } else {
        // Show init prompt, Y button opens keyboard
        Serial.printf("[ESPNOW_INIT] Checking buttons: newlyPressed=0x%08lX Y_mask=0x%08lX B_mask=0x%08lX\n",
                      (unsigned long)newlyPressed, 
                      (unsigned long)INPUT_MASK(INPUT_BUTTON_Y),
                      (unsigned long)INPUT_MASK(INPUT_BUTTON_B));
        
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
          if (gSettings.espnowDeviceName.length() == 0) {
            Serial.println("[ESPNOW_INIT] Y button pressed - opening keyboard");
            oledEspNowShowNameKeyboard();
          } else {
            Serial.println("[ESPNOW_INIT] Y button pressed - initializing ESP-NOW (name already set)");
            const char* initResult = cmd_espnow_init("");
            if (initResult && strstr(initResult, "initialized")) {
              oledEspNowInit();
            }
          }
          inputProcessed = true;
        }
        // B button: Back to menu
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
          Serial.println("[ESPNOW_INIT] B button pressed - going back");
          oledMenuBack();
          inputProcessed = true;
        }
      }
    } else {
      // ESP-NOW is initialized, let the handler process input
      if (oledEspNowHandleInput(deltaX, deltaY, newlyPressed)) {
        inputProcessed = true;
      }
      // If handler didn't process input and B was pressed, go back to menu
      if (!inputProcessed && INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        oledMenuBack();
        inputProcessed = true;
      }
    }
#endif
  } else if (currentOLEDMode == OLED_NETWORK_INFO) {
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtonState;
    uint32_t newlyPressed = pressedNow & ~pressedLast;
    
    // Check if keyboard is active for WiFi network addition
    if (wifiAddingNetwork && (wifiEnteringSSID || wifiEnteringPassword)) {
      if (oledKeyboardIsActive()) {
        // Let keyboard handle input
        if (oledKeyboardHandleInput(deltaX, deltaY, newlyPressed)) {
          inputProcessed = true;
        }
        
        // Check if user completed keyboard input
        if (oledKeyboardIsCompleted()) {
          const char* input = oledKeyboardGetText();
          if (wifiEnteringSSID) {
            // Got SSID, now ask for password
            wifiNewSSID = String(input);
            wifiEnteringSSID = false;
            wifiEnteringPassword = true;
            oledKeyboardReset();
            oledKeyboardInit("Enter Password:", "");
          } else if (wifiEnteringPassword) {
            // Got password, add the network
            wifiNewPassword = String(input);
            String addCmd = "wifiadd \"" + wifiNewSSID + "\" \"" + wifiNewPassword + "\"";
            executeOLEDCommand(addCmd);
            // Reset state
            wifiAddingNetwork = false;
            wifiEnteringPassword = false;
            wifiNewSSID = "";
            wifiNewPassword = "";
            oledKeyboardReset();
          }
        } else if (oledKeyboardIsCancelled()) {
          // User cancelled
          wifiAddingNetwork = false;
          wifiEnteringSSID = false;
          wifiEnteringPassword = false;
          wifiNewSSID = "";
          wifiNewPassword = "";
          oledKeyboardReset();
        }
      }
      lastButtonState = buttons;
      return inputProcessed;
    }
    
    // Network menu navigation using centralized gNavEvents
    if (gNavEvents.down) {
      networkMenuDown();
      inputProcessed = true;
    } else if (gNavEvents.up) {
      networkMenuUp();
      inputProcessed = true;
    }
    
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
      executeNetworkAction();
      inputProcessed = true;
    } else if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      if (networkShowingStatus) {
        networkMenuBack();
      } else if (networkShowingWiFiSubmenu) {
        networkMenuBack();
      } else {
        oledMenuBack();
      }
      inputProcessed = true;
    }
  } else {
    // Any other mode - check for registered custom input handler first
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtonState;
    uint32_t newlyPressed = pressedNow & ~pressedLast;
    
    // Global SELECT button handler - access quick settings from anywhere (only if authenticated)
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_SELECT)) {
      extern Settings gSettings;
      if (!gSettings.localDisplayRequireAuth || isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)) {
        pushOLEDMode(currentOLEDMode);
        currentOLEDMode = OLED_QUICK_SETTINGS;
        inputProcessed = true;
      }
    }
    
    // Check if this mode has a registered custom input handler
    const OLEDModeEntry* registeredMode = findOLEDMode(currentOLEDMode);
    if (registeredMode && registeredMode->inputFunc) {
      // Use custom input handler - it returns false if it wants to exit the mode
      bool handlerProcessed = registeredMode->inputFunc(deltaX, deltaY, newlyPressed);
      if (handlerProcessed) {
        inputProcessed = true;
      } else if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        // Handler returned false and B was pressed - exit to previous mode
        oledMenuBack();
        inputProcessed = true;
      }
    } else {
      // Default behavior: B = back, X = context action
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        oledMenuBack();
        inputProcessed = true;
      } else if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        // X button = context-sensitive action
        handleOLEDActionButton();
        inputProcessed = true;
      }
    }
  }
  
  if (inputProcessed) {
    lastGamepadNavTime = now;
    Serial.printf("[GAMEPAD_MENU] ACTION! sel=%d mode=%d\n", oledMenuSelectedIndex, currentOLEDMode);
  }
  
  lastButtonState = buttons;
  return inputProcessed;
}

/**
 * Try to auto-start gamepad when entering menu mode
 */
void tryAutoStartGamepadForMenu() {
  Serial.printf("[GAMEPAD_AUTO] tryAutoStartGamepadForMenu: enabled=%d connected=%d\n", gamepadEnabled, gamepadConnected);
  
  if (gamepadEnabled && gamepadConnected) {
    Serial.println("[GAMEPAD_AUTO] Already running, skipping");
    return;  // Already running
  }
  
  // Check if gamepad hardware is present via I2C ping
  bool pingResult = i2cPingAddress(I2C_ADDR_GAMEPAD, 100000, 50);
  Serial.printf("[GAMEPAD_AUTO] I2C ping 0x50 result: %d\n", pingResult);
  
  if (pingResult) {
    // Gamepad detected - try to start it
    extern bool enqueueSensorStart(SensorType sensor);
    extern bool isInQueue(SensorType sensor);
    
    bool inQueue = isInQueue(SENSOR_GAMEPAD);
    Serial.printf("[GAMEPAD_AUTO] inQueue=%d\n", inQueue);
    
    if (!inQueue) {
      bool enqueued = enqueueSensorStart(SENSOR_GAMEPAD);
      Serial.printf("[GAMEPAD_AUTO] enqueueSensorStart result: %d\n", enqueued);
      DEBUG_SENSORSF("[OLED] Auto-starting gamepad for menu navigation");
    }
  }
}

#else // !ENABLE_GAMEPAD_SENSOR

// Stubs when gamepad is disabled
bool processGamepadMenuInput() { return false; }
void tryAutoStartGamepadForMenu() {}

#endif // ENABLE_GAMEPAD_SENSOR

// ============================================================================
// OLED File Browser (128x64 optimized)
// ============================================================================
FileManager* gOLEDFileManager = nullptr;
bool oledFileBrowserNeedsInit = true;

// FileBrowserPendingAction, FileBrowserRenderData moved to OLED_Mode_FileBrowser.cpp
// initFileBrowser(), prepareFileBrowserData() moved to OLED_Mode_FileBrowser.cpp

// NetworkRenderData moved to OLED_Mode_Network.cpp
// prepareNetworkData(), displayNetworkInfoRendered() moved to OLED_Mode_Network.cpp

// MemoryRenderData moved to OLED_Mode_System.cpp
// displayMemoryStatsRendered() moved to OLED_Mode_System.cpp

// prepareWebStatsData() moved to OLED_Mode_Network.cpp
// displayWebStatsRendered() moved to OLED_Mode_Network.cpp

// prepareSystemStatusData() moved to OLED_Mode_System.cpp
// displaySystemStatusRendered() moved to OLED_Mode_System.cpp

// prepareMeshStatusData() moved to OLED_Mode_Network.cpp
// displayMeshStatusRendered() moved to OLED_Mode_Network.cpp

// displayFileBrowserRendered() moved to OLED_Mode_FileBrowser.cpp
// oledFileBrowserUp/Down/Select/Back() moved to OLED_Mode_FileBrowser.cpp
// resetOLEDFileBrowser() moved to OLED_Mode_FileBrowser.cpp

// ============================================================================
// OLED Command Registry
// ============================================================================

const CommandEntry oledCommands[] = {
  { "oledstart", "Start OLED display.", false, cmd_oledstart },
  { "oledstop", "Stop OLED display.", false, cmd_oledstop },
  { "oledmode", "Set display mode: menu|status|sensordata|thermal|network|gps|files|automations|espnow|memory|off.", false, cmd_oledmode,
    "Usage: oledmode <menu|status|sensordata|sensorlist|thermal|network|mesh|gps|text|logo|anim|imuactions|fmradio|files|automations|espnow|memory|off>\n"
    "Example: oledmode memory\n"
    "Example: oledmode off" },
  { "oledtext", "Set custom text: oledtext \"message\".", false, cmd_oledtext },
  { "oledanim", "Select animation: oledanim <name> or oledanim fps <1-60>.", false, cmd_oledanim },
  { "oledclear", "Clear OLED display.", false, cmd_oledclear },
  { "oledstatus", "Show OLED status.", false, cmd_oledstatus },
  { "oledlayout", "Set mode layout: oledlayout [mode] <0-9|toggle|show>.", false, cmd_oledlayout },
  { "oledrequireauth", "Enable/disable OLED authentication requirement <0|1>.", false, cmd_oled_requireauth },
};

const size_t oledCommandsCount = sizeof(oledCommands) / sizeof(oledCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _oled_cmd_registrar(oledCommands, oledCommandsCount, "oled");

// OLED Settings Module moved to OLED_Settings.cpp

#endif // ENABLE_OLED_DISPLAY

// =============================================================================
// OLED WRAPPER FUNCTIONS - Always compiled, safe to call without guards
// =============================================================================

void oledSetBootProgress(int percent, const char* label) {
#if ENABLE_OLED_DISPLAY
  bootProgressPercent = percent;
  bootProgressLabel = label;
  if (oledEnabled && oledConnected) {
    updateOLEDDisplay();
  }
#endif
}

void oledUpdate() {
#if ENABLE_OLED_DISPLAY
  if (oledEnabled && oledConnected) {
    updateOLEDDisplay();
  }
#endif
}

void oledEarlyInit() {
#if ENABLE_OLED_DISPLAY
  earlyOLEDInit();
  printRegisteredOLEDModes();
#endif
}

void applyOLEDBrightness() {
#if ENABLE_OLED_DISPLAY
  if (oledConnected && oledEnabled) {
    if (gSettings.oledBrightness >= 0 && gSettings.oledBrightness <= 255) {
      i2cDeviceTransactionVoid(I2C_ADDR_OLED, 100000, 200, [&]() {
        oledDisplay->ssd1306_command(SSD1306_SETCONTRAST);
        oledDisplay->ssd1306_command(gSettings.oledBrightness);
      });
    }
  }
#endif
}

void oledApplySettings() {
#if ENABLE_OLED_DISPLAY
  if (oledConnected && oledEnabled) {
    applyOLEDBrightness();
    DEBUG_SYSTEMF("OLED settings applied - boot animation running");
  }
#endif
}

void oledNotifyLocalDisplayAuthChanged() {
#if ENABLE_OLED_DISPLAY
  extern Settings gSettings;
  extern bool gLocalDisplayAuthed;
  extern bool oledBootModeActive;

  if (!oledEnabled || !oledConnected) {
    return;
  }

  // If auth is required and the display is not authenticated, force the login screen.
  if (gSettings.localDisplayRequireAuth && !gLocalDisplayAuthed && !oledBootModeActive) {
    if (currentOLEDMode != OLED_LOGIN) {
      currentOLEDMode = OLED_LOGIN;
      updateOLEDDisplay();
    }
    return;
  }

  // If we just became authenticated while on the login screen, return to the menu.
  if (gLocalDisplayAuthed && currentOLEDMode == OLED_LOGIN) {
    currentOLEDMode = OLED_MENU;
    resetOLEDMenu();
    tryAutoStartGamepadForMenu();
#if ENABLE_GAMEPAD_SENSOR
    // Prevent the login-confirm A press from being interpreted as a menu-select
    // on the first menu frame (avoids a brief flash into the first menu item).
    lastButtonStateInitialized = false;
    lastButtonState = 0xFFFFFFFF;
#endif
    updateOLEDDisplay();
  }
#endif
}

// ============================================================================
// Display Power Control (abstracted from SSD1306-specific commands)
// ============================================================================

void oledDisplayOff() {
#if ENABLE_OLED_DISPLAY
  if (oledDisplay && oledConnected) {
    i2cDeviceTransactionVoid(I2C_ADDR_OLED, 100000, 500, [&]() {
      oledDisplay->ssd1306_command(SSD1306_DISPLAYOFF);
    });
  }
#endif
}

void oledDisplayOn() {
#if ENABLE_OLED_DISPLAY
  if (oledDisplay && oledConnected) {
    i2cDeviceTransactionVoid(I2C_ADDR_OLED, 100000, 500, [&]() {
      oledDisplay->ssd1306_command(SSD1306_DISPLAYON);
    });
  }
#endif
}

void oledShowSleepScreen(int seconds) {
#if ENABLE_OLED_DISPLAY
  if (oledDisplay && oledConnected) {
    i2cDeviceTransactionVoid(I2C_ADDR_OLED, 100000, 500, [&]() {
      oledDisplay->clearDisplay();
      oledDisplay->setTextSize(1);
      oledDisplay->setCursor(0, 16);
      oledDisplay->println("  Sleeping...");
      oledDisplay->println();
      oledDisplay->printf("  Waking in %ds", seconds);
      oledDisplay->display();
    });
  }
#endif
}
