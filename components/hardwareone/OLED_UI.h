#ifndef OLED_UI_H
#define OLED_UI_H

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Arduino.h>
#include "HAL_Display.h"

// ============================================================================
// OLED UI Component System
// ============================================================================
// Provides reusable, standardized UI components for OLED display:
// - Toast: Temporary notification overlay (auto-dismiss)
// - Dialog: Modal dialog with title, message, and buttons
// - Progress: Progress bar overlay
// - List selector: Scrollable list with selection
//
// All components use consistent styling and can be shown as overlays
// on top of the current display content.
// ============================================================================

// ----------------------------------------------------------------------------
// Common Types
// ----------------------------------------------------------------------------

// Callback for dialog button press (userData passed from show call)
typedef void (*OledUICallback)(void* userData);

// Button configuration for dialogs
enum class OledUIButton : uint8_t {
  NONE = 0,
  OK = 1,
  CANCEL = 2,
  YES = 3,
  NO = 4,
  RETRY = 5,
  CUSTOM = 6
};

// Icon types for dialogs/toasts
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
// Dialog Component - Modal dialog with buttons
// ----------------------------------------------------------------------------
// Shows a modal dialog that captures input until dismissed.
// Supports title, message (up to 3 lines), and configurable buttons.

struct OledDialog {
  char title[24];
  char lines[3][32];
  uint8_t lineCount;
  OledUIButton buttons[2];
  uint8_t buttonCount;
  uint8_t selectedButton;
  OledUICallback onButton[2];
  void* userData;
  OledUIIcon icon;
  bool active;
};

extern OledDialog gOledDialog;

// Show a simple OK dialog
void oledDialogOK(const char* title, const char* message, OledUICallback onOK = nullptr, void* userData = nullptr);

// Show a Yes/No confirmation dialog
void oledDialogYesNo(const char* title, const char* message, OledUICallback onYes = nullptr, OledUICallback onNo = nullptr, void* userData = nullptr);

// Show a custom dialog with configurable buttons
void oledDialogCustom(const char* title, const char* message, 
                      OledUIButton btn1, OledUICallback cb1,
                      OledUIButton btn2 = OledUIButton::NONE, OledUICallback cb2 = nullptr,
                      void* userData = nullptr, OledUIIcon icon = OledUIIcon::NONE);

// Close any active dialog
void oledDialogClose();

// Check if dialog is currently showing
bool oledDialogActive();

// Handle input for dialog (returns true if input was consumed)
bool oledDialogHandleInput(uint32_t newlyPressed);

// Render dialog overlay (call from display update)
void oledDialogRender();

// ----------------------------------------------------------------------------
// Progress Component - Progress bar overlay
// ----------------------------------------------------------------------------
// Shows a progress bar with optional label. Can be determinate (percentage)
// or indeterminate (spinner/animation).

struct OledProgress {
  char label[32];
  int percent;        // 0-100, or -1 for indeterminate
  uint32_t startMs;   // For animation timing
  bool active;
  bool cancellable;   // Show cancel hint
};

extern OledProgress gOledProgress;

// Show progress bar (percent 0-100, or -1 for indeterminate spinner)
void oledProgressShow(const char* label, int percent = -1, bool cancellable = false);

// Update progress value
void oledProgressUpdate(int percent);

// Update progress label
void oledProgressLabel(const char* label);

// Close progress overlay
void oledProgressClose();

// Check if progress is showing
bool oledProgressActive();

// Render progress overlay
void oledProgressRender();

// ----------------------------------------------------------------------------
// List Selector Component - Scrollable list with selection
// ----------------------------------------------------------------------------
// Shows a scrollable list overlay for item selection.
// Supports callbacks for selection and cancel.

#define OLED_LIST_MAX_ITEMS 16
#define OLED_LIST_ITEM_LEN 24

struct OledListItem {
  char label[OLED_LIST_ITEM_LEN];
  int value;  // Custom value for callback
};

typedef void (*OledListCallback)(int selectedIndex, int value, void* userData);

struct OledList {
  char title[24];
  OledListItem items[OLED_LIST_MAX_ITEMS];
  uint8_t itemCount;
  uint8_t selectedIndex;
  uint8_t scrollOffset;
  uint8_t visibleCount;  // How many items fit on screen
  OledListCallback onSelect;
  OledListCallback onCancel;
  void* userData;
  bool active;
};

extern OledList gOledList;

// Show list selector
void oledListShow(const char* title, const OledListItem* items, uint8_t count,
                  OledListCallback onSelect, OledListCallback onCancel = nullptr, void* userData = nullptr);

// Add item to list (alternative to passing array)
void oledListAddItem(const char* label, int value = 0);

// Clear list and start fresh
void oledListClear(const char* title);

// Finalize and show list after adding items
void oledListFinalize(OledListCallback onSelect, OledListCallback onCancel = nullptr, void* userData = nullptr);

// Close list
void oledListClose();

// Check if list is showing
bool oledListActive();

// Handle input for list
bool oledListHandleInput(uint32_t newlyPressed);

// Render list overlay
void oledListRender();

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

// Handle input for any active UI component (returns true if consumed)
// Call this before mode-specific input handling
bool oledUIHandleInput(uint32_t newlyPressed);

// Render all active UI overlays (call after main content, before display())
// Renders in order: progress (bottom), dialog (middle), toast (top)
void oledUIRender();

// Check if any modal UI is active (blocks normal input)
bool oledUIModalActive();

// ----------------------------------------------------------------------------
// Drawing Helpers - Reusable primitives
// ----------------------------------------------------------------------------

// Draw a bordered box with optional fill
void oledDrawBox(int x, int y, int w, int h, bool filled = false);

// Draw a button (highlighted if selected)
void oledDrawButton(int x, int y, int w, const char* label, bool selected);

// Draw an icon at position (simple 8x8 built-in shapes)
void oledDrawIcon(int x, int y, OledUIIcon icon);

// Draw an embedded icon by name, scaled to targetSize pixels (e.g. 12, 16, 32)
void oledDrawIcon(int x, int y, const char* iconName, int targetSize);

// Draw vertical level bars (for volume, signal strength, etc.)
void oledDrawLevelBars(int x, int y, int level, int maxBars, int barHeight);

// Draw centered text within bounds
void oledDrawTextCentered(int x, int y, int w, const char* text);

// Get button label text
const char* oledUIButtonLabel(OledUIButton btn);

#endif // ENABLE_OLED_DISPLAY
#endif // OLED_UI_H
