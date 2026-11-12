#ifndef OLED_UTILS_H
#define OLED_UTILS_H

#include <Arduino.h>

#include "System_BuildConfig.h"
#include "System_Notifications.h"  // NotificationSource enum (defined there, not here)
#include "System_CommandTypes.h"
#include "System_User.h"           // AuthContext (returned by oledAuthContext)

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "HAL_Display.h"
#include "OLED_Layout.h"

inline constexpr int oledVisibleRows(int rowHeight, int reservedHeight = 0, int baselineRows = 0) {
  return oled_layout::visibleRows(OLED_CONTENT_HEIGHT, rowHeight, reservedHeight, baselineRows);
}

// =============================================================================
// OLED Utilities - Scrolling Lists & Virtual Keyboard
// =============================================================================

bool submitOledCommandForSession(const String& command, TransportSessionEpoch epoch,
                                 ExecAsyncCallback callback, void* context);

// ============= Standardized Header System =============

// Header display options
struct OledHeaderInfo {
  const char* title;          // Mode/menu title (nullptr = auto from current mode)
  bool showBattery;           // Show battery icon and percentage
  bool showNotifications;     // Show notification indicator if queue not empty
  bool showUsb;               // Show USB indicator when connected
  uint8_t notificationCount;  // Number of unread notifications (0 = none)
};

// Default header config (shows battery, notifications, auto title)
extern const OledHeaderInfo HEADER_DEFAULT;

// Render standardized header bar at top of display
// Returns the Y position where content should start (after header)
int oledRenderHeader(Adafruit_SSD1306* display, const OledHeaderInfo* info = nullptr);

// Get current mode name for header (from currentOledMode)
const char* oledGetCurrentModeName();

// ============= Notification Queue System =============

#define OLED_NOTIFICATION_MAX 8
#define OLED_NOTIFICATION_MSG_LEN 48
#define OLED_NOTIFICATION_SUBSOURCE_LEN 32

// NotificationSource enum is defined in System_Notifications.h (included above).

struct OledNotification {
  char message[OLED_NOTIFICATION_MSG_LEN];
  char subsource[OLED_NOTIFICATION_SUBSOURCE_LEN];  // IP address, device name, or MAC
  uint32_t timestampMs;
  uint8_t level;    // 0=info, 1=success, 2=warning, 3=error
  uint8_t source;   // NotificationSource enum
  bool read;        // Has user seen this notification?
};

// The notification center is a VIEW over the system event register since the
// Phase-1 cutover — there is no add call. Events whose rule has NSINK_QUEUE
// appear here automatically; read/clear are seq watermarks.

// Get notification count (total and unread)
int oledNotificationCount();
int oledNotificationUnreadCount();

// Mark all notifications as read
void oledNotificationMarkAllRead();

// Clear all notifications
void oledNotificationClear();

// Get notification by index (0 = newest)
const OledNotification* oledNotificationGet(int index);

// ============= Standardized Footer System =============

// Footer hint structure for navigation display
struct OledFooterHints {
  const char* buttonA;  // nullptr = hide button
  const char* buttonB;
  const char* buttonX;
  const char* buttonY;
};

// Common footer presets
extern const OledFooterHints FOOTER_BACK_ONLY;
extern const OledFooterHints FOOTER_SELECT_BACK;
extern const OledFooterHints FOOTER_CONFIRM_CANCEL;
extern const OledFooterHints FOOTER_KEYBOARD;
extern const OledFooterHints FOOTER_DONE_BACK;

// Render standardized footer bar at bottom of display
void oledRenderFooter(Adafruit_SSD1306* display, const OledFooterHints* hints);

// ============= Shared Drawing Utilities =============

// Draw a progress/measurement bar with optional right-aligned label
// value/maxValue determine fill percentage. label (if non-null) is drawn to the right of the bar.
void oledDrawBar(Adafruit_SSD1306* display, int x, int y, int width, int height,
                 int value, int maxValue, const char* label = nullptr);

// ============= Content Area System =============

// Content area rendering context for scrollable content
struct OledContentArea {
  Adafruit_SSD1306* display;
  int16_t scrollOffset;     // Y offset for scrolling (negative = scrolled down)
  int16_t contentHeight;    // Total height of content in pixels
  int16_t cursorY;          // Current Y position for content rendering
  bool needsScroll;         // True if content exceeds display area
  bool scrollAtTop;         // True if scrolled to top
  bool scrollAtBottom;      // True if scrolled to bottom
};

// Initialize content area for rendering
void oledContentInit(OledContentArea* ctx, Adafruit_SSD1306* display);

// Begin content rendering (clears content area, sets up clipping)
void oledContentBegin(OledContentArea* ctx);

// End content rendering (draws scroll indicators if needed)
void oledContentEnd(OledContentArea* ctx);

// Print text in content area (respects scroll offset and boundaries)
void oledContentPrint(OledContentArea* ctx, const char* text, bool newline = true);
void oledContentPrintAt(OledContentArea* ctx, int16_t x, int16_t y, const char* text);

// Set cursor position in content area (absolute Y, will be adjusted by scroll offset)
void oledContentSetCursor(OledContentArea* ctx, int16_t x, int16_t y);

// Scroll content area up/down by lines (8 pixels per line)
void oledContentScrollUp(OledContentArea* ctx, int lines = 1);
void oledContentScrollDown(OledContentArea* ctx, int lines = 1);

// Update scroll state after content is measured
void oledContentUpdateScroll(OledContentArea* ctx);

// ============= Scrolling System =============

#define OLED_SCROLL_MAX_ITEMS 32

struct OledScrollItem {
  const char* line1;
  const char* line2;
  bool isSelectable;
  bool isHighlighted;
  void* userData;
  uint8_t icon;
  const char* iconName;   // Icon name for oledDrawIcon() in split-pane mode
  uint32_t validationKey;
};

struct OledScrollState {
  OledScrollItem items[OLED_SCROLL_MAX_ITEMS];
  int itemCount;
  int selectedIndex;
  int scrollOffset;
  int visibleLines;
  bool wrapAround;
  const char* title;
  const char* footer;
  uint32_t refreshCounter;
  
  // Split-pane layout (set listWidth > 0 to enable)
  int listWidth;          // Width of text list area in pixels (0 = full width)
  int separatorX;         // X position of vertical separator line
  int iconSize;           // Icon size in right pane (default 32)
  bool singleLineItems;   // true = 10px single-line items, false = 16px two-line

  // Optional right-pane decorator (split-pane only). Called once per frame for
  // the SELECTED item, right after its icon is drawn, so a menu can render an
  // availability badge + status text beside the icon without baking that logic
  // into the primitive. areaX = left edge of the icon pane (separatorX + 4);
  // iconY/iconSize describe the icon box so the callback can place text below
  // it. nullptr = draw the icon only (the default for every existing user).
  void (*rightPaneDraw)(Adafruit_SSD1306* display, OledScrollItem* selected,
                        int areaX, int iconY, int iconSize);
};

void oledScrollInit(OledScrollState* state, const char* title = nullptr, int visibleLines = oledVisibleRows(10, 1, 4));
bool oledScrollAddItem(OledScrollState* state, const char* line1, const char* line2 = nullptr, 
                       bool selectable = true, void* userData = nullptr);
void oledScrollClear(OledScrollState* state);
// Like oledScrollClear() but PRESERVES selectedIndex/scrollOffset, so a menu can
// be rebuilt every frame (oledScrollClear* + re-add items) without losing the
// cursor. The selection is clamped back into range automatically by
// oledScrollHandleNav()/oledScrollRenderSimple(); for hand-rolled render loops
// (e.g. Power) call oledScrollClampSelection() after re-adding items.
void oledScrollClearKeepSelection(OledScrollState* state);
// Clamp selectedIndex into [0, itemCount-1] and keep it within the visible window.
// Safe no-op when already valid.
void oledScrollClampSelection(OledScrollState* state);
void oledScrollUp(OledScrollState* state);
void oledScrollDown(OledScrollState* state);
void oledScrollPageUp(OledScrollState* state);
void oledScrollPageDown(OledScrollState* state);
OledScrollItem* oledScrollGetSelected(OledScrollState* state);
OledScrollItem* oledScrollGetItem(OledScrollState* state, int index);
void oledScrollRender(Adafruit_SSD1306* display, OledScrollState* state,
                      bool showScrollbar = true, bool showSelection = true,
                      const OledFooterHints* footerHints = nullptr);

// Lightweight single-line list renderer — the compact counterpart to
// oledScrollRender(). Renders each item on ONE 8px line with a "> " cursor
// prefix (the Power / Network main-menu look), instead of oledScrollRender()'s
// 16px two-line / split-pane items. Use this for plain single-line text menus
// so more than one option is visible at a time. Honors scrollOffset/visibleLines
// and draws a thin scrollbar only when the list overflows.
//
// startY: top Y of the list window (default = content top). Pass a lower value
// to reserve a header/status line above the list (e.g. the file-browser action
// menu draws the filename on the top line, then renders the list one row down) —
// the same "status line + scrollable list" shape the Power main menu hand-rolls.
// Size visibleLines to the reduced window ((OLED_CONTENT_HEIGHT - reserved)/8).
void oledScrollRenderSimple(Adafruit_SSD1306* display, OledScrollState* state,
                            bool showSelection = true, int startY = OLED_CONTENT_START_Y);
int oledScrollCalculateVisibleLines(int displayHeight, int textSize, bool hasTitle = false, bool hasFooter = false);

// Generic list-menu navigation helper using centralized gNavEvents.
// Handles up/down (and optionally left/right) scroll with wrap-around.
// Returns true if any navigation event was consumed.
bool oledScrollHandleNav(OledScrollState* state, bool leftRightNav = false);

// Configure split-pane layout (list on left, icon on right)
void oledScrollSetSplitPane(OledScrollState* state, int listWidth, int separatorX, int iconSize = 32);

// ============= Virtual Keyboard =============

// Raised from 32 to hold a dictated phrase (KEYBOARD_MODE_MIC). This is one
// global struct, so the cost is ~96 bytes of DRAM. Note that callers already
// asked for more than 32 — OLED_ESPNow's message field requests 128 and its
// remote form 64 — and were silently clamped by the min() in oledKeyboardInit;
// those fields get the length they always asked for as a side effect.
#define OLED_KEYBOARD_MAX_LENGTH 128
#define OLED_KEYBOARD_COLS 10
#define OLED_KEYBOARD_ROWS 3

enum OledKeyboardMode {
  KEYBOARD_MODE_LOWERCASE = 0,
  KEYBOARD_MODE_UPPERCASE = 1,
  KEYBOARD_MODE_NUMBERS = 2,
  KEYBOARD_MODE_SYMBOLS = 3,
  KEYBOARD_MODE_PATTERN = 4,
  KEYBOARD_MODE_MIC = 5,
  KEYBOARD_MODE_COUNT = 6
};

// Dictation is an input method, so every field must opt in explicitly. Keep
// DENY as zero: the global keyboard state and any partially initialized copy
// fail closed. Secret fields and opaque command payloads must never allow a
// microphone transcript to cross into their buffer.
enum class OledKeyboardDictationPolicy : uint8_t {
  DENY = 0,
  ALLOW_PLAINTEXT = 1,
};

// The MODE key (SELECT / in-grid '*') cycles through every mode in enum order:
// lowercase -> uppercase -> numbers -> symbols -> pattern -> mic -> lowercase.
// PATTERN stays in the rotation on purpose: the OLED login screen accepts a
// gamepad pattern in place of a text password (isValidUser() checks both
// hashes), so the login keyboard MUST be able to reach it via SELECT.
//
// MIC is the one CONDITIONAL member. Every field must explicitly allow
// dictation, and an allowed field still needs a mic source AND an authenticated
// CM5 host link (nothing on this device turns speech into arbitrary text — see
// System_Dictation.h). oledKeyboardToggleMode() therefore skips it when either
// the field policy denies it or dictationAvailable() is false. Do not
// "simplify" either gate away.

// Autocomplete provider callback types
// Returns number of suggestions found, fills results array (up to maxResults)
// Each result is a null-terminated string pointer (must remain valid until next call)
#define OLED_KEYBOARD_MAX_SUGGESTIONS 8
typedef int (*OledKeyboardAutocompleteFunc)(const char* input, const char** results, int maxResults, void* userData);

struct OledKeyboardState {
  char text[OLED_KEYBOARD_MAX_LENGTH + 1];
  int textLength;
  int cursorX;
  int cursorY;
  OledKeyboardMode mode;
  OledKeyboardDictationPolicy dictationPolicy;
  bool masked;  // secret field: every preview draws '*' per character
  bool active;
  bool cancelled;
  bool completed;
  String title;
  int maxLength;
  
  // Autocomplete system (triggered by Select button)
  OledKeyboardAutocompleteFunc autocompleteFunc;
  void* autocompleteUserData;
  bool showingSuggestions;
  const char* suggestions[OLED_KEYBOARD_MAX_SUGGESTIONS];
  int suggestionCount;
  int selectedSuggestion;
};

extern const char OLED_KEYBOARD_CHARS_UPPER[OLED_KEYBOARD_ROWS][OLED_KEYBOARD_COLS];
extern const char OLED_KEYBOARD_CHARS_LOWER[OLED_KEYBOARD_ROWS][OLED_KEYBOARD_COLS];
extern const char OLED_KEYBOARD_CHARS_NUMBERS[OLED_KEYBOARD_ROWS][OLED_KEYBOARD_COLS];
extern const char OLED_KEYBOARD_CHARS_SYMBOLS[OLED_KEYBOARD_ROWS][OLED_KEYBOARD_COLS];
extern OledKeyboardState gOledKeyboardState;

// No default for the dictation policy is intentional: adding a text field
// requires an explicit dictation security decision at its call site.
// `masked` marks a secret field (password, passphrase, gamepad pattern): every
// keyboard page then draws one '*' per character in its preview, while the
// buffer keeps the real text for oledKeyboardGetText(). It is separate from the
// dictation policy because non-secret command fields also deny dictation.
// Plain fields omit it.
void oledKeyboardInit(const char* title, const char* initialText, int maxLength,
                      OledKeyboardDictationPolicy dictationPolicy,
                      bool masked = false);
void oledKeyboardReset();
void oledKeyboardDisplay(Adafruit_SSD1306* display);
// Convenience "curtain": if the keyboard overlay is active, draw it and return
// true so a mode's display function can early-`return`. Replaces the repeated
// `if (oledKeyboardIsActive()) { oledKeyboardDisplay(d); return; }` idiom.
bool oledKeyboardDrawIfActive(Adafruit_SSD1306* display);
bool oledKeyboardHandleInput(int deltaX, int deltaY, uint32_t newlyPressed);
const char* oledKeyboardGetText();
bool oledKeyboardIsActive();
bool oledKeyboardIsCompleted();
bool oledKeyboardIsCancelled();
void oledKeyboardMoveUp();
void oledKeyboardMoveDown();
void oledKeyboardMoveLeft();
void oledKeyboardMoveRight();
// Row-major scan across the entire char grid (or linear scan through the
// suggestions list when those are showing). +1 = next, -1 = previous; any
// integer is accepted and the grid wraps with full-wheel modulo, so passing
// the total accumulated detents from a fast wheel spin in a single call is
// fine. Used by the ANO encoder path so a rotation scans a→b→c→…→z→a
// instead of being trapped in one column.
void oledKeyboardAdvance(int steps);
void oledKeyboardSelectChar();
void oledKeyboardBackspace();
void oledKeyboardComplete();
void oledKeyboardCancel();
void oledKeyboardToggleMode();

// Per-tick service for KEYBOARD_MODE_MIC: supervises the dictation timeouts and
// appends a delivered transcript into the field. Called from oledUpdate() on
// the display task, so the text lands on the same task that owns the keyboard
// state — the `dictate` command itself only stages it. Cheap no-op when the
// keyboard is inactive or in any other mode.
void oledKeyboardDictationTick();

// Autocomplete support (Select button triggers suggestions)
void oledKeyboardSetAutocomplete(OledKeyboardAutocompleteFunc func, void* userData = nullptr);
void oledKeyboardTriggerAutocomplete();
void oledKeyboardSelectSuggestion();
void oledKeyboardDismissSuggestions();
bool oledKeyboardShowingSuggestions();

// Draw a small curved back arrow icon (↩) inline at current cursor position
void oledDrawBackArrowIcon(Adafruit_SSD1306* display, int footerY);

typedef void (*OledConfirmCallback)(void* userData);
// onNo (optional) fires when the user picks No or cancels (B). Existing callers
// that omit it keep today's behavior (cancel just dismisses).
bool oledConfirmRequest(const char* line1, const char* line2, OledConfirmCallback onYes, void* userData, bool defaultYes = true, OledConfirmCallback onNo = nullptr);
bool oledConfirmIsActive();
// Poll for an incoming ESP-NOW pair request and pop the shared accept/reject
// dialog (defined in OLED_ESPNow.cpp; called from oledUpdate each tick).
void oledEspNowPollPairRequest();
// Mark the display dirty when the BLE role or a glasses, ring or phone link
// changes, so the Bluetooth screens redraw without input (OLED_Mode_Bluetooth.cpp;
// called from oledUpdate each tick in Bluetooth builds).
void oledBluetoothLinkWatch();

// ============= Shared Command Execution =============
// Execute a CLI command with OLED display authentication context
void executeOledCommand(const String& argsInput);
// Execute a CLI command and return success status + output (for callers that need the result)
bool executeOledCommandWithResult(const String& argsInput, char* out, size_t outSize);
// Session-bound variant for stateful OLED UI flows. `expectedEpoch` is the
// exact display incarnation that collected the input; submission fails closed
// if another identity replaced it before admission or result delivery.
bool executeOledCommandWithResultForSession(
    const String& argsInput, TransportSessionEpoch expectedEpoch,
    char* out, size_t outSize);
// Toast text for a command's outcome (the toast box fits ~17 columns). Success
// gives okText (empty when nullptr). A refusal gives "Admin required" for an
// admin or super-admin denial, otherwise the reply's first line without its
// leading [ERROR]/Error/ERROR token and ": ", or failText when nothing is left.
// out is always NUL-terminated (outSize > 0).
void oledCommandReplyText(bool ok, const char* reply, const char* okText,
                          const char* failText, char* out, size_t outSize);
// Run a user-initiated OLED action. A refused or failed command shows its
// reply on the global toast (oledCommandReplyText, "Failed" when empty) and
// returns false; true only when the command succeeded, so the caller shows its
// own success text only then.
bool oledRunCommand(const String& cmd);
// oledRunCommand for an action that can take a while (radio, Wi-Fi, Bluetooth,
// HTTP): returns at once and the frame loop never waits on cmd_exec. When
// the command finishes, the display task reports it as oledRunCommand would (a
// refusal toast) and then calls done(ok, context), ok true only on success. A
// reply that outlives its display session is dropped (done gets false). One
// request at a time: false means nothing was queued, and a toast already says
// why ("Please wait" while one is running, "Busy - try again" if the queue
// refused it); done is not called then.
typedef void (*OledCommandDoneFn)(bool ok, const void* context);
bool oledRunCommandAsync(const char* cmd, OledCommandDoneFn done, const void* context);
// The command that starts or stops the BLE role this device runs, or is set to
// run: the server, the G2 client (with any phone pipe attached to it) or the
// phone-only pipe. The Quick Settings Bluetooth row uses it so it never
// switches roles; the OLED Bluetooth menu has one row per role instead.
// nullptr in builds without Bluetooth.
const char* oledBluetoothToggleCommand();

// ============= OLED AuthContext Builder =============
// Single source of truth for "what AuthContext represents OLED-originated
// work." Mirrors g2HijackAuthContext() on the G2 side — eliminates the
// drift-bug class (two hand-built AuthContexts in different files going
// out of sync as fields are added/changed) that Pass 1 caught on the G2
// path. The `path` argument is the audit-log/permission-check path string:
// "/oled/command" for CLI submissions via buildOledCommand, "/oled/files"
// for direct-FS work via oledFileBrowserAuthContext, etc.
//
// Identity reflects the OLED login state: gLocalDisplayUser when logged in,
// "AuthBypass" reserved name when displayRequireAuth is off. Audit log
// lines and per-user permissions both pick this up automatically.
AuthContext oledAuthContext(const char* path);

// ============= Battery Icon Shared State =============

struct BatteryIconState {
  float percentage;
  char icon;
  unsigned long lastUpdateMs;
  bool valid;
};

extern BatteryIconState batteryIconState;
extern const unsigned long BATTERY_ICON_UPDATE_INTERVAL;

// ============= Logging Mode Shared State =============

enum LoggingMenuState {
  LOG_MENU_MAIN,
  LOG_MENU_SENSOR,
  LOG_MENU_SYSTEM,
  LOG_MENU_SENSOR_CONFIG,
  LOG_MENU_SYSTEM_CONFIG,   // Tags / Categories / Path
  LOG_MENU_SYSTEM_BANKS,    // debug-flag families
  LOG_MENU_SYSTEM_FLAGS,    // flags in the chosen family
  LOG_MENU_SYSTEM_PATH      // full log path, read-only
};

extern LoggingMenuState loggingCurrentState;
extern int loggingMenuSelection;

// Mode entry points and shared state defined across the OLED files.
struct FileBrowserRenderData;  // System_FileManager.h
extern FileBrowserRenderData fileBrowserRenderData;
int getCliViewerSelectedIndex();
void oledAuthModeInit();
void oledAuthModeResetSessionState();
void oledCliInputModeInit();
void oledCliInputResetSessionState();
void oledChangePasswordModeInit();
void oledChangePasswordModeResetSessionState();
int oledFileBrowserActiveLevel();
void oledFileBrowserResetSessionState();
const char* oledFilePickerTitle();
void oledLoggingModeInit();
void oledMenuModeInit();
void oledNetworkModeResetSessionState();
void oledPowerModeInit();
void oledSetPatternModeInit();
void oledSetPatternModeResetSessionState();
void oledUserManagerModeInit();
void oledUserManagerModeResetSessionState();

#if ENABLE_OLED_DISPLAY && ENABLE_AUTOMATION
void oledAutomationsModeResetSessionState();
#endif

#if ENABLE_OLED_DISPLAY && ENABLE_BLUETOOTH
void oledBluetoothModeInit();
#endif

#if ENABLE_OLED_DISPLAY && ENABLE_LLM_BACKEND
const char* oledLlmFooterHint();
void oledLlmModeInit();
#endif

#if ENABLE_OLED_DISPLAY && ENABLE_MAPS
extern bool gMapMenuOpen;
#endif

#if ENABLE_OLED_DISPLAY && ENABLE_NEOPIXEL
void oledLedModeInit();
#endif

#if ENABLE_OLED_DISPLAY && ENABLE_R1_HEALTH
void oledR1HealthModeInit();
#endif

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE
bool bondModeInputHandler(int deltaX, int deltaY, uint32_t newlyPressed);
void oledRemoteSettingsModeInit();
#endif

#endif // ENABLE_OLED_DISPLAY

#endif // OLED_UTILS_H
