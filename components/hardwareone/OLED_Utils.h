#ifndef OLED_UTILS_H
#define OLED_UTILS_H

#include <Arduino.h>

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>

// =============================================================================
// OLED Utilities - Scrolling Lists & Virtual Keyboard
// =============================================================================

// ============= Scrolling System =============

#define OLED_SCROLL_MAX_ITEMS 32

struct OLEDScrollItem {
  const char* line1;
  const char* line2;
  bool isSelectable;
  bool isHighlighted;
  void* userData;
  uint8_t icon;
  uint32_t validationKey;
};

struct OLEDScrollState {
  OLEDScrollItem items[OLED_SCROLL_MAX_ITEMS];
  int itemCount;
  int selectedIndex;
  int scrollOffset;
  int visibleLines;
  bool wrapAround;
  const char* title;
  const char* footer;
  uint32_t refreshCounter;
};

void oledScrollInit(OLEDScrollState* state, const char* title = nullptr, int visibleLines = 4);
bool oledScrollAddItem(OLEDScrollState* state, const char* line1, const char* line2 = nullptr, 
                       bool selectable = true, void* userData = nullptr);
void oledScrollClear(OLEDScrollState* state);
void oledScrollUp(OLEDScrollState* state);
void oledScrollDown(OLEDScrollState* state);
void oledScrollPageUp(OLEDScrollState* state);
void oledScrollPageDown(OLEDScrollState* state);
OLEDScrollItem* oledScrollGetSelected(OLEDScrollState* state);
OLEDScrollItem* oledScrollGetItem(OLEDScrollState* state, int index);
void oledScrollRender(Adafruit_SSD1306* display, OLEDScrollState* state, 
                      bool showScrollbar = true, bool showSelection = true);
int oledScrollCalculateVisibleLines(int displayHeight, int textSize, bool hasTitle = false, bool hasFooter = false);

// ============= Virtual Keyboard =============

#define OLED_KEYBOARD_MAX_LENGTH 32
#define OLED_KEYBOARD_COLS 10
#define OLED_KEYBOARD_ROWS 3

enum OLEDKeyboardMode {
  KEYBOARD_MODE_UPPERCASE = 0,
  KEYBOARD_MODE_LOWERCASE = 1,
  KEYBOARD_MODE_NUMBERS = 2,
  KEYBOARD_MODE_COUNT = 3
};

// Autocomplete provider callback types
// Returns number of suggestions found, fills results array (up to maxResults)
// Each result is a null-terminated string pointer (must remain valid until next call)
#define OLED_KEYBOARD_MAX_SUGGESTIONS 8
typedef int (*OLEDKeyboardAutocompleteFunc)(const char* input, const char** results, int maxResults, void* userData);

struct OLEDKeyboardState {
  char text[OLED_KEYBOARD_MAX_LENGTH + 1];
  int textLength;
  int cursorX;
  int cursorY;
  OLEDKeyboardMode mode;
  bool active;
  bool cancelled;
  bool completed;
  String title;
  int maxLength;
  
  // Autocomplete system (triggered by Select button)
  OLEDKeyboardAutocompleteFunc autocompleteFunc;
  void* autocompleteUserData;
  bool showingSuggestions;
  const char* suggestions[OLED_KEYBOARD_MAX_SUGGESTIONS];
  int suggestionCount;
  int selectedSuggestion;
};

extern const char OLED_KEYBOARD_CHARS_UPPER[OLED_KEYBOARD_ROWS][OLED_KEYBOARD_COLS];
extern const char OLED_KEYBOARD_CHARS_LOWER[OLED_KEYBOARD_ROWS][OLED_KEYBOARD_COLS];
extern const char OLED_KEYBOARD_CHARS_NUMBERS[OLED_KEYBOARD_ROWS][OLED_KEYBOARD_COLS];
extern OLEDKeyboardState gOLEDKeyboardState;

void oledKeyboardInit(const char* title = nullptr, const char* initialText = nullptr, int maxLength = OLED_KEYBOARD_MAX_LENGTH);
void oledKeyboardReset();
void oledKeyboardDisplay(Adafruit_SSD1306* display);
bool oledKeyboardHandleInput(int deltaX, int deltaY, uint32_t newlyPressed);
const char* oledKeyboardGetText();
bool oledKeyboardIsActive();
bool oledKeyboardIsCompleted();
bool oledKeyboardIsCancelled();
void oledKeyboardMoveUp();
void oledKeyboardMoveDown();
void oledKeyboardMoveLeft();
void oledKeyboardMoveRight();
void oledKeyboardSelectChar();
void oledKeyboardBackspace();
void oledKeyboardComplete();
void oledKeyboardCancel();
void oledKeyboardToggleMode();

// Autocomplete support (Select button triggers suggestions)
void oledKeyboardSetAutocomplete(OLEDKeyboardAutocompleteFunc func, void* userData = nullptr);
void oledKeyboardTriggerAutocomplete();
void oledKeyboardSelectSuggestion();
void oledKeyboardDismissSuggestions();
bool oledKeyboardShowingSuggestions();

typedef void (*OLEDConfirmCallback)(void* userData);
bool oledConfirmRequest(const char* line1, const char* line2, OLEDConfirmCallback onYes, void* userData, bool defaultYes = true);
bool oledConfirmIsActive();

// ============= Shared Command Execution =============
// Execute a CLI command with OLED display authentication context
void executeOLEDCommand(const String& cmd);

#endif // ENABLE_OLED_DISPLAY

#endif // OLED_UTILS_H
