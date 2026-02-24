#ifndef OLED_UTILS_H
#define OLED_UTILS_H

#include <Arduino.h>

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "HAL_Display.h"

// =============================================================================
// OLED Utilities - Scrolling Lists & Virtual Keyboard
// =============================================================================

// ============= Standardized Header System =============

// Header display options
struct OLEDHeaderInfo {
  const char* title;          // Mode/menu title (nullptr = auto from current mode)
  bool showBattery;           // Show battery icon and percentage
  bool showNotifications;     // Show notification indicator if queue not empty
  bool showUSB;               // Show USB indicator when connected
  uint8_t notificationCount;  // Number of unread notifications (0 = none)
};

// Default header config (shows battery, notifications, auto title)
extern const OLEDHeaderInfo HEADER_DEFAULT;

// Render standardized header bar at top of display
// Returns the Y position where content should start (after header)
int oledRenderHeader(Adafruit_SSD1306* display, const OLEDHeaderInfo* info = nullptr);

// Get current mode name for header (from currentOLEDMode)
const char* oledGetCurrentModeName();

// ============= Notification Queue System =============

#define OLED_NOTIFICATION_MAX 8
#define OLED_NOTIFICATION_MSG_LEN 48
#define OLED_NOTIFICATION_SUBSOURCE_LEN 32

// Notification source types
enum NotificationSource : uint8_t {
  NOTIF_SOURCE_UNKNOWN = 0,
  NOTIF_SOURCE_CLI = 1,
  NOTIF_SOURCE_OLED = 2,
  NOTIF_SOURCE_WEB = 3,
  NOTIF_SOURCE_VOICE = 4,
  NOTIF_SOURCE_REMOTE = 5
};

struct OLEDNotification {
  char message[OLED_NOTIFICATION_MSG_LEN];
  char subsource[OLED_NOTIFICATION_SUBSOURCE_LEN];  // IP address, device name, or MAC
  uint32_t timestampMs;
  uint8_t level;    // 0=info, 1=success, 2=warning, 3=error
  uint8_t source;   // NotificationSource enum
  bool read;        // Has user seen this notification?
};

// Add notification to persistent queue
void oledNotificationAdd(const char* message, uint8_t level = 0, uint8_t source = NOTIF_SOURCE_UNKNOWN, const char* subsource = nullptr);

// Get notification count (total and unread)
int oledNotificationCount();
int oledNotificationUnreadCount();

// Mark all notifications as read
void oledNotificationMarkAllRead();

// Clear all notifications
void oledNotificationClear();

// Get notification by index (0 = newest)
const OLEDNotification* oledNotificationGet(int index);

// ============= Standardized Footer System =============

// Footer hint structure for navigation display
struct OLEDFooterHints {
  const char* buttonA;  // nullptr = hide button
  const char* buttonB;
  const char* buttonX;
  const char* buttonY;
};

// Common footer presets
extern const OLEDFooterHints FOOTER_BACK_ONLY;
extern const OLEDFooterHints FOOTER_SELECT_BACK;
extern const OLEDFooterHints FOOTER_CONFIRM_CANCEL;
extern const OLEDFooterHints FOOTER_KEYBOARD;
extern const OLEDFooterHints FOOTER_DONE_BACK;

// Render standardized footer bar at bottom of display
void oledRenderFooter(Adafruit_SSD1306* display, const OLEDFooterHints* hints);

// ============= Shared Drawing Utilities =============

// Draw a progress/measurement bar with optional right-aligned label
// value/maxValue determine fill percentage. label (if non-null) is drawn to the right of the bar.
void oledDrawBar(Adafruit_SSD1306* display, int x, int y, int width, int height,
                 int value, int maxValue, const char* label = nullptr);

// ============= Content Area System =============

// Content area rendering context for scrollable content
struct OLEDContentArea {
  Adafruit_SSD1306* display;
  int16_t scrollOffset;     // Y offset for scrolling (negative = scrolled down)
  int16_t contentHeight;    // Total height of content in pixels
  int16_t cursorY;          // Current Y position for content rendering
  bool needsScroll;         // True if content exceeds display area
  bool scrollAtTop;         // True if scrolled to top
  bool scrollAtBottom;      // True if scrolled to bottom
};

// Initialize content area for rendering
void oledContentInit(OLEDContentArea* ctx, Adafruit_SSD1306* display);

// Begin content rendering (clears content area, sets up clipping)
void oledContentBegin(OLEDContentArea* ctx);

// End content rendering (draws scroll indicators if needed)
void oledContentEnd(OLEDContentArea* ctx);

// Print text in content area (respects scroll offset and boundaries)
void oledContentPrint(OLEDContentArea* ctx, const char* text, bool newline = true);
void oledContentPrintAt(OLEDContentArea* ctx, int16_t x, int16_t y, const char* text);

// Set cursor position in content area (absolute Y, will be adjusted by scroll offset)
void oledContentSetCursor(OLEDContentArea* ctx, int16_t x, int16_t y);

// Scroll content area up/down by lines (8 pixels per line)
void oledContentScrollUp(OLEDContentArea* ctx, int lines = 1);
void oledContentScrollDown(OLEDContentArea* ctx, int lines = 1);

// Update scroll state after content is measured
void oledContentUpdateScroll(OLEDContentArea* ctx);

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
                      bool showScrollbar = true, bool showSelection = true,
                      const OLEDFooterHints* footerHints = nullptr);
int oledScrollCalculateVisibleLines(int displayHeight, int textSize, bool hasTitle = false, bool hasFooter = false);

// Generic list-menu navigation helper using centralized gNavEvents.
// Handles up/down (and optionally left/right) scroll with wrap-around.
// Returns true if any navigation event was consumed.
bool oledScrollHandleNav(OLEDScrollState* state, bool leftRightNav = false);

// ============= Virtual Keyboard =============

#define OLED_KEYBOARD_MAX_LENGTH 32
#define OLED_KEYBOARD_COLS 10
#define OLED_KEYBOARD_ROWS 3

enum OLEDKeyboardMode {
  KEYBOARD_MODE_LOWERCASE = 0,
  KEYBOARD_MODE_UPPERCASE = 1,
  KEYBOARD_MODE_NUMBERS = 2,
  KEYBOARD_MODE_PATTERN = 3,
  KEYBOARD_MODE_COUNT = 4
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

// Draw a small curved back arrow icon (↩) inline at current cursor position
void oledDrawBackArrowIcon(Adafruit_SSD1306* display, int footerY);

typedef void (*OLEDConfirmCallback)(void* userData);
bool oledConfirmRequest(const char* line1, const char* line2, OLEDConfirmCallback onYes, void* userData, bool defaultYes = true);
bool oledConfirmIsActive();

// ============= Shared Command Execution =============
// Execute a CLI command with OLED display authentication context
void executeOLEDCommand(const String& cmd);

#endif // ENABLE_OLED_DISPLAY

#endif // OLED_UTILS_H
