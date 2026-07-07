#include "WebServer_Handle.h"
#include "OLED_Utils.h"
#include "OLED_Display.h"
#include <esp_app_desc.h>
#include "OLED_UI.h"
#include "Bluetooth.h"
#include "System_Battery.h"
#include "System_BuildConfig.h"
#include "System_Command.h"
#include "System_Settings.h"
#include "System_ESPNow.h"
#include "System_Utils.h"  // For AuthContext
#include "System_CommandTypes.h"  // For Command, CmdOutputMask
#include "System_FirstTimeSetup.h"

// Forward declaration for memory stats display
void displayMemoryStats();

#if !ENABLE_OLED_DISPLAY
bool oledBootModeActive = false;

// Linked whenever OLED UI is compiled out; System_User::tgRequireAuth still references this.
bool shouldBlockForDisplayAuth() {
  extern bool gLocalDisplayAuthed;
  return gSettings.localDisplayRequireAuth && !gLocalDisplayAuthed && !oledBootModeActive;
}
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

#include <ArduinoJson.h>
#include <LittleFS.h>
#include "System_VFS.h"
#include "i2csensor_rda5807.h"

#include "OLED_ConsoleBuffer.h"
#include "OLED_Footer.h"
#include "OLED_SettingsEditor.h"
#include "OLED_RemoteSettings.h"
#include "System_Debug.h"
#include "System_FileManager.h"
#include "System_MemUtil.h"
#include "System_FirstTimeSetup.h"
#include "System_I2C.h"
#include "System_SensorLogging.h"
#include "System_SensorStubs.h"
#include "System_User.h"
#include "System_Settings.h"
#include "System_User.h"
#include "System_Utils.h"

#if ENABLE_ESPNOW
#include "OLED_ESPNow.h"
#include "System_ESPNow.h"
#endif
#if ENABLE_APDS_SENSOR
#include "i2csensor_apds9960.h"
#endif
#if ENABLE_OLED_INPUT
// Includes the InputCache struct + gGamepad* extern declarations and
// JOYSTICK_CENTER/DEADZONE constants — used by both the seesaw gamepad and
// the ANO encoder driver (the ANO driver populates the gamepad-shaped cache
// so OLED_Utils stays driver-agnostic).
#include "i2csensor_seesaw.h"
#if ENABLE_ANO_ENCODER
#include "i2csensor_ano_encoder.h"
#endif
#endif
#if ENABLE_IMU_SENSOR
#include "i2csensor_bno055.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor_mlx90640.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor_vl53l4cx.h"
#endif
#if ENABLE_RTC_SENSOR
#include "i2csensor_ds3231.h"
#endif
#if ENABLE_GPS_SENSOR
#include "i2csensor_pa1010d.h"  // gGpsEnabled, gGpsConnected
#endif

#if ENABLE_WIFI || ENABLE_ESPNOW
#include <esp_wifi.h>
#endif
#include <LittleFS.h>

// =============================================================================
// Standardized Footer System Implementation
// =============================================================================

// Common footer presets
const OLEDFooterHints FOOTER_BACK_ONLY = { nullptr, "Back", nullptr, nullptr };
const OLEDFooterHints FOOTER_SELECT_BACK = { "Select", "Back", nullptr, nullptr };
const OLEDFooterHints FOOTER_CONFIRM_CANCEL = { "Confirm", "Cancel", nullptr, nullptr };
const OLEDFooterHints FOOTER_KEYBOARD = { "Done", "Back", nullptr, "Undo" };
const OLEDFooterHints FOOTER_DONE_BACK = { "Done", "Back", nullptr, nullptr };

void oledRenderFooter(Adafruit_SSD1306* display, const OLEDFooterHints* hints) {
  if (!display || !hints) return;
  
  // Draw separator line at top of footer area
  display->drawFastHLine(0, DISPLAY_CONTENT_HEIGHT, DISPLAY_WIDTH, DISPLAY_COLOR_WHITE);
  
  // Clear footer area (below separator line)
  display->fillRect(0, DISPLAY_CONTENT_HEIGHT + 1, DISPLAY_WIDTH, 
                    DISPLAY_FOOTER_HEIGHT - 1, DISPLAY_COLOR_BLACK);
  
  // Render button hints in compact format: "A:Sel B:Back Y:Set"
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  display->setCursor(0, DISPLAY_CONTENT_HEIGHT + 2);
  
  bool needSpace = false;
  
  if (hints->buttonA) {
    if (needSpace) display->print(" ");
    display->print("A:");
    display->print(hints->buttonA);
    needSpace = true;
  }
  
  if (hints->buttonB) {
    if (needSpace) display->print(" ");
    display->print("B:");
    display->print(hints->buttonB);
    needSpace = true;
  }
  
  if (hints->buttonX) {
    if (needSpace) display->print(" ");
    display->print("X:");
    display->print(hints->buttonX);
    needSpace = true;
  }
  
  if (hints->buttonY) {
    if (needSpace) display->print(" ");
    display->print("Y:");
    display->print(hints->buttonY);
  }
}

// =============================================================================
// Shared Drawing Utilities
// =============================================================================

void oledDrawBar(Adafruit_SSD1306* display, int x, int y, int width, int height,
                 int value, int maxValue, const char* label) {
  if (!display || maxValue <= 0) return;
  if (value < 0) value = 0;
  if (value > maxValue) value = maxValue;
  
  // Draw outline
  display->drawRect(x, y, width, height, DISPLAY_COLOR_WHITE);
  
  // Fill proportionally
  int fillWidth = (width - 2) * value / maxValue;
  if (fillWidth > 0) {
    display->fillRect(x + 1, y + 1, fillWidth, height - 2, DISPLAY_COLOR_WHITE);
  }
  
  // Optional label to the right of the bar
  if (label) {
    display->setCursor(x + width + 2, y + (height > 8 ? (height - 8) / 2 : 0));
    display->setTextSize(1);
    display->setTextColor(DISPLAY_COLOR_WHITE);
    display->print(label);
  }
}

// =============================================================================
// Standardized Header System Implementation
// =============================================================================

// Forward declaration for mode name lookup
static const char* getOLEDModeName(OLEDMode mode);

// Default header config
const OLEDHeaderInfo HEADER_DEFAULT = { nullptr, true, true, true, 0 };

// Notification queue storage
static OLEDNotification sNotificationQueue[OLED_NOTIFICATION_MAX];
static int sNotificationCount = 0;
static int sNotificationHead = 0;  // Newest notification index

void oledNotificationAdd(const char* message, uint8_t level, uint8_t source, const char* subsource) {
  if (!message) return;
  
  // Find slot for new notification (circular buffer, newest at head)
  int slot = sNotificationHead;
  
  // Copy message
  strncpy(sNotificationQueue[slot].message, message, OLED_NOTIFICATION_MSG_LEN - 1);
  sNotificationQueue[slot].message[OLED_NOTIFICATION_MSG_LEN - 1] = '\0';
  
  // Copy subsource (IP, device name, or MAC)
  if (subsource && subsource[0]) {
    strncpy(sNotificationQueue[slot].subsource, subsource, OLED_NOTIFICATION_SUBSOURCE_LEN - 1);
    sNotificationQueue[slot].subsource[OLED_NOTIFICATION_SUBSOURCE_LEN - 1] = '\0';
  } else {
    sNotificationQueue[slot].subsource[0] = '\0';
  }
  
  sNotificationQueue[slot].timestampMs = millis();
  sNotificationQueue[slot].level = level;
  sNotificationQueue[slot].source = source;
  sNotificationQueue[slot].read = false;
  
  // Advance head
  sNotificationHead = (sNotificationHead + 1) % OLED_NOTIFICATION_MAX;
  if (sNotificationCount < OLED_NOTIFICATION_MAX) {
    sNotificationCount++;
  }
}

int oledNotificationCount() {
  return sNotificationCount;
}

int oledNotificationUnreadCount() {
  int unread = 0;
  for (int i = 0; i < sNotificationCount; i++) {
    int idx = (sNotificationHead - 1 - i + OLED_NOTIFICATION_MAX) % OLED_NOTIFICATION_MAX;
    if (!sNotificationQueue[idx].read) unread++;
  }
  return unread;
}

void oledNotificationMarkAllRead() {
  for (int i = 0; i < OLED_NOTIFICATION_MAX; i++) {
    sNotificationQueue[i].read = true;
  }
}

void oledNotificationClear() {
  sNotificationCount = 0;
  sNotificationHead = 0;
}

const OLEDNotification* oledNotificationGet(int index) {
  if (index < 0 || index >= sNotificationCount) return nullptr;
  // Index 0 = newest (head - 1)
  int idx = (sNotificationHead - 1 - index + OLED_NOTIFICATION_MAX) % OLED_NOTIFICATION_MAX;
  return &sNotificationQueue[idx];
}

// Defined later in this file — header text when mode is OLED_UNAVAILABLE
extern String unavailableOLEDTitle;

// Get mode name from current OLED mode
const char* oledGetCurrentModeName() {
  return getOLEDModeName(currentOLEDMode);
}

int oledRenderHeader(Adafruit_SSD1306* display, const OLEDHeaderInfo* info) {
  if (!display) return OLED_HEADER_HEIGHT;
  
  // Use default if no info provided
  OLEDHeaderInfo headerInfo = info ? *info : HEADER_DEFAULT;
  
  // Clear header area
  display->fillRect(0, 0, DISPLAY_WIDTH, OLED_HEADER_HEIGHT, DISPLAY_COLOR_BLACK);
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Left side: Mode title with breadcrumb for menu submenus
  const char* title = headerInfo.title;
  if (!title) {
    // Build breadcrumb for menu category submenus
    extern int oledMenuCategorySelected;
    extern const OLEDMenuItem oledMenuCategories[];
    static char breadcrumbBuf[22];  // "Menu > " (7) + name (up to 14) + null
    
    if (currentOLEDMode == OLED_MENU && oledMenuCategorySelected >= 0) {
      // In a category submenu - show "Menu>CategoryName"
      snprintf(breadcrumbBuf, sizeof(breadcrumbBuf), "Menu>%s", 
               oledMenuCategories[oledMenuCategorySelected].name);
      title = breadcrumbBuf;
    } else if (currentOLEDMode == OLED_SETTINGS) {
      // Settings breadcrumb - show "Set>ModuleName" when in a submenu
      extern SettingsEditorContext gSettingsEditor;
      if (gSettingsEditor.currentModule && gSettingsEditor.state != SETTINGS_CATEGORY_SELECT) {
        snprintf(breadcrumbBuf, sizeof(breadcrumbBuf), "Set>%s", 
                 gSettingsEditor.currentModule->name);
        title = breadcrumbBuf;
      } else {
        title = oledGetCurrentModeName();
      }
    } else if (currentOLEDMode == OLED_FILE_BROWSER) {
      // Picker-mode header takes priority: the picker title tells the user
      // WHAT they're picking (e.g. "Pick model") and is more useful at a
      // glance than the current path. The path is visible in the file
      // listing itself when navigation matters.
      extern const char* oledFilePickerTitle();
      const char* pickerTitle = oledFilePickerTitle();
      if (pickerTitle && pickerTitle[0]) {
        title = pickerTitle;
      } else {
        // Viewer mode: show "Files>folder/sub" breadcrumb.
        extern FileManager* gOledFileManager;
        if (gOledFileManager) {
          const char* path = gOledFileManager->getCurrentPath();
          if (path && strcmp(path, "/") != 0) {
            snprintf(breadcrumbBuf, sizeof(breadcrumbBuf), "Files>%s", path);
            title = breadcrumbBuf;
          } else {
            title = "Files";
          }
        } else {
          title = "Files";
        }
      }
    } else if (currentOLEDMode == OLED_UNAVAILABLE && unavailableOLEDTitle.length() > 0) {
      // Unavailable overlay uses mode OLED_UNAVAILABLE but body shows unavailableOLEDTitle;
      // use the same semantic title in the header (e.g. "Automations") instead of "Unavail".
      static char unavailHeaderBuf[16];
      strncpy(unavailHeaderBuf, unavailableOLEDTitle.c_str(), 15);
      unavailHeaderBuf[15] = '\0';
      title = unavailHeaderBuf;
#if ENABLE_ESPNOW
    } else if (currentOLEDMode == OLED_ESPNOW) {
      // The conversation (device-detail) view surfaces the interaction mode in
      // the header ("ESPNOW: Text/Remote/File") instead of a separate in-body
      // "Mode:" line; other ESP-NOW views return nullptr and keep "ESP-NOW".
      extern const char* oledEspNowHeaderTitle();
      const char* espnowTitle = oledEspNowHeaderTitle();
      title = (espnowTitle && espnowTitle[0]) ? espnowTitle : oledGetCurrentModeName();
#endif
    } else {
      title = oledGetCurrentModeName();
    }
  }
  
  display->setCursor(0, 1);
  if (title) {
    // Truncate to fit available space (leave room for status icons)
    char titleBuf[16];
    strncpy(titleBuf, title, 15);
    titleBuf[15] = '\0';
    display->print(titleBuf);
  }
  
  // Right side: Status icons (right-aligned). Each char is 6 px wide at
  // textSize 1; the layout below allocates a multiple of 6 per element so
  // strings don't wrap mid-glyph to the next OLED row.
  int iconX = DISPLAY_WIDTH;
  bool drewRightSide = false;  // tracks whether to draw a "|" separator before notifications

  // Battery / USB indicator
  if (headerInfo.showBattery || headerInfo.showUSB) {
    extern BatteryState gBatteryState;
    extern char getBatteryIcon();
    extern bool isUsbPresent();

    // USB presence (not just "actively charging") — covers the float-charge
    // case where CRATE ≈ 0 but VBUS is still holding the cell topped off.
    // Falling back to isBatteryCharging() here would silently drop the USB
    // indicator the moment the cell hits 100%.
    bool usbConnected = isUsbPresent();

    if (usbConnected && headerInfo.showUSB) {
      // "USB" is 3 chars × 6 px = 18 px. Anything less wraps "B" onto the
      // next OLED row — see commit history for the original off-by-six bug.
      iconX -= 18;
      display->setCursor(iconX, 1);
      display->print("USB");
      drewRightSide = true;
    } else if (headerInfo.showBattery && gBatteryState.status != BATTERY_NOT_PRESENT) {
      // Show "NN%" — the tier-letter icon (F/H/M/L/E from getBatteryIcon)
      // would be redundant with the number AND reads like a unit suffix
      // ("87F" looked like 87° Fahrenheit). The richer state info (charging
      // vs float vs discharging) is communicated by the parallel "USB" /
      // "USB+" branch above and on the system status page.
      int pct = (int)gBatteryState.percentage;

      // Width: percentage digits (1-3 chars) + "%" (1 char). 6 px per char.
      int pctWidth = (pct >= 100) ? 18 : (pct >= 10) ? 12 : 6;
      iconX -= (pctWidth + 6);

      display->setCursor(iconX, 1);
      display->print(pct);
      display->print('%');
      drewRightSide = true;
    }
  }

  // Notification indicator (bell icon with count)
  int unreadCount = headerInfo.showNotifications ? oledNotificationUnreadCount() : 0;
  if (unreadCount > 0) {
    // Visual separator between notifications and battery/USB so the two
    // status groups don't run together visually. Only drawn when both are
    // present — otherwise a stray "|" hangs in space at the right edge.
    if (drewRightSide) {
      iconX -= 8;  // 6 px for the pipe + 2 px of breathing room on its right
      display->setCursor(iconX + 2, 1);
      display->print('|');
    }
    iconX -= 12;  // Space for bell + count
    display->setCursor(iconX, 1);
    display->print((char)0x07);  // Bell character
    if (unreadCount < 10) {
      display->print(unreadCount);
    } else {
      display->print('+');
    }
  }
  
  // Draw separator line at bottom of header
  display->drawFastHLine(0, OLED_HEADER_HEIGHT - 1, DISPLAY_WIDTH, DISPLAY_COLOR_WHITE);
  
  return OLED_HEADER_HEIGHT;
}

// =============================================================================
// Notifications Mode Display
// =============================================================================

static int sNotificationsScrollOffset = 0;
static int sNotificationsSelectedIndex = 0;
static bool sNotificationsShowingDetail = false;

// Helper to get source name string
static const char* getNotificationSourceName(uint8_t source) {
  switch (source) {
    case NOTIF_SOURCE_CLI:    return "CLI";
    case NOTIF_SOURCE_OLED:   return "OLED";
    case NOTIF_SOURCE_WEB:    return "Web";
    case NOTIF_SOURCE_VOICE:  return "Voice";
    case NOTIF_SOURCE_REMOTE: return "Remote";
    case NOTIF_SOURCE_SYSTEM: return "System";
    case NOTIF_SOURCE_G2:     return "G2";
    default: return "Unknown";
  }
}

void displayNotifications() {
  if (!oledDisplay) return;
  
  int count = oledNotificationCount();
  
  // Content starts after header
  int startY = OLED_CONTENT_START_Y;
  int lineHeight = 10;
  int maxVisible = (OLED_CONTENT_HEIGHT) / lineHeight;  // ~4 items
  
  if (count == 0) {
    oledDisplay->setCursor(0, startY + 10);
    oledDisplay->print("No notifications");
    sNotificationsShowingDetail = false;
    return;
  }
  
  // Clamp selected index
  if (sNotificationsSelectedIndex >= count) {
    sNotificationsSelectedIndex = count - 1;
  }
  if (sNotificationsSelectedIndex < 0) {
    sNotificationsSelectedIndex = 0;
  }
  
  // Show detail view if A was pressed
  if (sNotificationsShowingDetail) {
    const OLEDNotification* notif = oledNotificationGet(sNotificationsSelectedIndex);
    if (!notif) {
      sNotificationsShowingDetail = false;
      return;
    }
    
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

    int y = startY;

    // Line 1: Source + timestamp (left) and n/N counter (right)
    char counterBuf[8];
    snprintf(counterBuf, sizeof(counterBuf), "%d/%d",
             sNotificationsSelectedIndex + 1, count);
    int counterX = 128 - (int)strlen(counterBuf) * 6;
    oledDisplay->setCursor(counterX, y);
    oledDisplay->print(counterBuf);

    oledDisplay->setCursor(0, y);
    oledDisplay->print(getNotificationSourceName(notif->source));

    // Show elapsed time
    uint32_t elapsed = (millis() - notif->timestampMs) / 1000;
    if (elapsed < 60) {
      oledDisplay->print(" ");
      oledDisplay->print((int)elapsed);
      oledDisplay->print("s ago");
    } else if (elapsed < 3600) {
      oledDisplay->print(" ");
      oledDisplay->print((int)(elapsed / 60));
      oledDisplay->print("m ago");
    }
    y += 9;
    
    // Line 2: Subsource (IP/device/MAC) if present
    if (notif->subsource[0]) {
      oledDisplay->setCursor(0, y);
      oledDisplay->print("From: ");
      char subsourceBuf[20];
      strncpy(subsourceBuf, notif->subsource, 19);
      subsourceBuf[19] = '\0';
      oledDisplay->print(subsourceBuf);
      y += 9;
    }
    
    // Separator
    oledDisplay->drawFastHLine(0, y, 128, DISPLAY_COLOR_WHITE);
    y += 2;
    
    // Message (word-wrapped, up to 3 lines)
    oledDisplay->setCursor(0, y);
    int charsPerLine = 21;
    int linesShown = 0;
    int msgLen = strlen(notif->message);
    for (int i = 0; i < msgLen && linesShown < 3; ) {
      int lineEnd = i + charsPerLine;
      if (lineEnd >= msgLen) {
        // Last line
        oledDisplay->print(&notif->message[i]);
        break;
      }
      // Find last space before lineEnd
      int breakPos = lineEnd;
      for (int j = lineEnd; j > i; j--) {
        if (notif->message[j] == ' ') {
          breakPos = j;
          break;
        }
      }
      // Print line
      char lineBuf[32];
      int lineLen = breakPos - i;
      if (lineLen > 31) lineLen = 31;
      strncpy(lineBuf, &notif->message[i], lineLen);
      lineBuf[lineLen] = '\0';
      oledDisplay->println(lineBuf);
      i = breakPos + 1;  // Skip space
      linesShown++;
    }
    
    return;
  }
  
  // List view - clamp scroll offset to keep selection visible
  if (sNotificationsSelectedIndex < sNotificationsScrollOffset) {
    sNotificationsScrollOffset = sNotificationsSelectedIndex;
  }
  if (sNotificationsSelectedIndex >= sNotificationsScrollOffset + maxVisible) {
    sNotificationsScrollOffset = sNotificationsSelectedIndex - maxVisible + 1;
  }
  
  // Draw notifications (newest first)
  for (int i = 0; i < maxVisible && (sNotificationsScrollOffset + i) < count; i++) {
    int notifIdx = sNotificationsScrollOffset + i;
    const OLEDNotification* notif = oledNotificationGet(notifIdx);
    if (!notif) continue;
    
    int y = startY + i * lineHeight;
    bool isSelected = (notifIdx == sNotificationsSelectedIndex);
    
    // Highlight selected item
    if (isSelected) {
      oledDisplay->fillRect(0, y, 118, lineHeight - 1, DISPLAY_COLOR_WHITE);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    // Level indicator
    char levelChar = ' ';
    if (notif->level == 1) levelChar = '+';  // success
    else if (notif->level == 2) levelChar = '!';  // warning
    else if (notif->level == 3) levelChar = 'X';  // error
    
    // Unread indicator
    if (!notif->read && !isSelected) {
      oledDisplay->fillCircle(2, y + 3, 2, isSelected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE);
    }
    
    oledDisplay->setCursor(6, y + 1);
    oledDisplay->print(levelChar);
    oledDisplay->print(' ');
    
    // Truncate message to fit
    char msgBuf[20];
    strncpy(msgBuf, notif->message, 18);
    msgBuf[18] = '\0';
    oledDisplay->print(msgBuf);
    
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  }
  
  // Scroll indicators
  if (sNotificationsScrollOffset > 0) {
    oledDisplay->setCursor(120, startY);
    oledDisplay->print("\x18");  // Up arrow
  }
  if (sNotificationsScrollOffset + maxVisible < count) {
    oledDisplay->setCursor(120, startY + (maxVisible - 1) * lineHeight);
    oledDisplay->print("\x19");  // Down arrow
  }
  
  // Mark all as read when viewing
  oledNotificationMarkAllRead();
}

bool handleNotificationsInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  int count = oledNotificationCount();
  
  // B button: Back from detail view or exit notifications
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (sNotificationsShowingDetail) {
      sNotificationsShowingDetail = false;
      return true;
    }
    return false;  // Let caller handle exit
  }
  
  // A button: Show detail view
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (!sNotificationsShowingDetail && count > 0) {
      sNotificationsShowingDetail = true;
      return true;
    }
    return false;
  }
  
  // X button: Clear all notifications (only in list view)
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X) && !sNotificationsShowingDetail) {
    oledNotificationClear();
    sNotificationsScrollOffset = 0;
    sNotificationsSelectedIndex = 0;
    return true;
  }
  
  // In detail view: up/down scrolls between notifications. LEFT/RIGHT are
  // reserved for back/forward (B on ANO encoder) — must NOT be consumed
  // here or the centralized B-back fall-through never fires and the user
  // gets stuck on the page.
  if (sNotificationsShowingDetail) {
    if (gNavEvents.up && sNotificationsSelectedIndex > 0) {
      sNotificationsSelectedIndex--;
      return true;
    }
    if (gNavEvents.down && sNotificationsSelectedIndex < count - 1) {
      sNotificationsSelectedIndex++;
      return true;
    }
    return false;
  }

  // List view navigation (same rationale: up/down only, no axis conflation)
  if (gNavEvents.up && sNotificationsSelectedIndex > 0) {
    sNotificationsSelectedIndex--;
    return true;
  }
  if (gNavEvents.down && sNotificationsSelectedIndex < count - 1) {
    sNotificationsSelectedIndex++;
    return true;
  }
  
  // B button: Let global handler call oledMenuBack()
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    return false;
  }
  
  return false;
}

// Notifications mode registration
extern void displayNotifications();

static bool notificationsRegisteredInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  return handleNotificationsInput(deltaX, deltaY, newlyPressed);
}

// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints
static const OLEDModeEntry sNotificationsModes[] = {
  { OLED_NOTIFICATIONS, "Notifications", "notify_sensor", displayNotifications, nullptr, notificationsRegisteredInputHandler, false, -1, nullptr },
};

REGISTER_OLED_MODE_MODULE(sNotificationsModes, sizeof(sNotificationsModes) / sizeof(sNotificationsModes[0]), "Notifications");

// =============================================================================
// Content Area System Implementation
// =============================================================================

void oledContentInit(OLEDContentArea* ctx, Adafruit_SSD1306* display) {
  if (!ctx) return;
  
  ctx->display = display;
  ctx->scrollOffset = 0;
  ctx->contentHeight = 0;
  ctx->cursorY = 0;
  ctx->needsScroll = false;
  ctx->scrollAtTop = true;
  ctx->scrollAtBottom = true;
}

void oledContentBegin(OLEDContentArea* ctx) {
  if (!ctx || !ctx->display) return;

  // Clear content area only (between header and footer)
  ctx->display->fillRect(0, OLED_CONTENT_START_Y, DISPLAY_WIDTH, DISPLAY_CONTENT_HEIGHT, DISPLAY_COLOR_BLACK);

  // cursorY is content-relative (0 = top of content area).
  // scrollOffset is how many pixels of content are hidden above the top edge.
  // Screen Y = OLED_CONTENT_START_Y + cursorY - scrollOffset.
  ctx->cursorY      = 0;
  ctx->contentHeight = 0;
  // scrollOffset is intentionally NOT reset here so scroll position persists between frames.
}

void oledContentEnd(OLEDContentArea* ctx) {
  if (!ctx || !ctx->display) return;

  // Update scroll state after all content has been measured
  oledContentUpdateScroll(ctx);

  // Draw scroll indicators using SSD1306 font arrows
  if (ctx->needsScroll) {
    const int indicatorX = DISPLAY_WIDTH - 6;
    ctx->display->setTextColor(DISPLAY_COLOR_WHITE);

    // Up arrow at top of content area when not at the beginning
    if (!ctx->scrollAtTop) {
      ctx->display->setCursor(indicatorX, OLED_CONTENT_START_Y);
      ctx->display->print('\x18');  // ↑
    }

    // Down arrow at bottom of content area when more content remains
    if (!ctx->scrollAtBottom) {
      ctx->display->setCursor(indicatorX, OLED_CONTENT_START_Y + DISPLAY_CONTENT_HEIGHT - 8);
      ctx->display->print('\x19');  // ↓
    }
  }
}

void oledContentSetCursor(OLEDContentArea* ctx, int16_t x, int16_t y) {
  if (!ctx || !ctx->display) return;

  // y is content-relative; translate to screen coordinates
  int16_t screenY = OLED_CONTENT_START_Y + y - ctx->scrollOffset;
  const int16_t topClip    = OLED_CONTENT_START_Y - 8;
  const int16_t bottomClip = OLED_CONTENT_START_Y + DISPLAY_CONTENT_HEIGHT;

  if (screenY >= topClip && screenY < bottomClip) {
    ctx->display->setCursor(x, screenY);
  }

  ctx->cursorY = y;  // Track content-relative position
}

void oledContentPrint(OLEDContentArea* ctx, const char* text, bool newline) {
  if (!ctx || !ctx->display || !text) return;

  // Translate content-relative cursorY to screen Y
  int16_t screenY = OLED_CONTENT_START_Y + ctx->cursorY - ctx->scrollOffset;
  const int16_t topClip    = OLED_CONTENT_START_Y - 8;
  const int16_t bottomClip = OLED_CONTENT_START_Y + DISPLAY_CONTENT_HEIGHT;

  if (screenY >= topClip && screenY < bottomClip) {
    ctx->display->setCursor(0, screenY);
    if (newline) {
      ctx->display->println(text);
    } else {
      ctx->display->print(text);
    }
  }

  // Advance content-relative cursor by one text-size-1 line height
  ctx->cursorY += 8;

  // Track total content height (content-relative)
  if (ctx->cursorY > ctx->contentHeight) {
    ctx->contentHeight = ctx->cursorY;
  }
}

void oledContentPrintAt(OLEDContentArea* ctx, int16_t x, int16_t y, const char* text) {
  if (!ctx || !ctx->display || !text) return;

  // y is content-relative; translate to screen coordinates
  int16_t screenY = OLED_CONTENT_START_Y + y - ctx->scrollOffset;
  const int16_t topClip    = OLED_CONTENT_START_Y - 8;
  const int16_t bottomClip = OLED_CONTENT_START_Y + DISPLAY_CONTENT_HEIGHT;

  if (screenY >= topClip && screenY < bottomClip) {
    ctx->display->setCursor(x, screenY);
    ctx->display->print(text);
  }

  // Track content height (y is content-relative, add one line height)
  if (y + 8 > ctx->contentHeight) {
    ctx->contentHeight = y + 8;
  }
}

// scrollUp: user pressed UP — wants to see content above the current view.
// Decreases scrollOffset (fewer pixels hidden at top).
void oledContentScrollUp(OLEDContentArea* ctx, int lines) {
  if (!ctx) return;

  ctx->scrollOffset -= (lines * 8);
  if (ctx->scrollOffset < 0) ctx->scrollOffset = 0;

  oledContentUpdateScroll(ctx);
}

// scrollDown: user pressed DOWN — wants to see content below the current view.
// Increases scrollOffset (more pixels hidden at top, revealing content below).
void oledContentScrollDown(OLEDContentArea* ctx, int lines) {
  if (!ctx) return;

  ctx->scrollOffset += (lines * 8);

  int maxOffset = ctx->contentHeight - DISPLAY_CONTENT_HEIGHT;
  if (maxOffset < 0) maxOffset = 0;
  if (ctx->scrollOffset > maxOffset) ctx->scrollOffset = maxOffset;

  oledContentUpdateScroll(ctx);
}

void oledContentUpdateScroll(OLEDContentArea* ctx) {
  if (!ctx) return;

  ctx->needsScroll   = (ctx->contentHeight > DISPLAY_CONTENT_HEIGHT);
  ctx->scrollAtTop   = (ctx->scrollOffset == 0);

  int maxOffset = ctx->contentHeight - DISPLAY_CONTENT_HEIGHT;
  if (maxOffset < 0) maxOffset = 0;
  ctx->scrollAtBottom = (ctx->scrollOffset >= maxOffset);
}

// =============================================================================
// Modular Scrolling System Implementation
// =============================================================================

void oledScrollInit(OLEDScrollState* state, const char* title, int visibleLines) {
  if (!state) return;
  
  state->itemCount = 0;
  state->selectedIndex = 0;
  state->scrollOffset = 0;
  state->visibleLines = visibleLines > 0 ? visibleLines : 4;
  state->wrapAround = false;  // menus clamp at top/bottom (no wrap-around)
  state->title = title;  // Store pointer directly
  state->footer = nullptr;
  state->refreshCounter = 0;
  
  // Split-pane defaults (0 = full-width mode)
  state->listWidth = 0;
  state->separatorX = 0;
  state->iconSize = 32;
  state->singleLineItems = false;
  state->rightPaneDraw = nullptr;

  // Clear all items
  for (int i = 0; i < OLED_SCROLL_MAX_ITEMS; i++) {
    state->items[i].line1 = nullptr;
    state->items[i].line2 = nullptr;
    state->items[i].isSelectable = true;
    state->items[i].isHighlighted = false;
    state->items[i].userData = nullptr;
    state->items[i].icon = 0;
    state->items[i].iconName = nullptr;
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
  state->items[idx].iconName = nullptr;
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

void oledScrollClearKeepSelection(OLEDScrollState* state) {
  if (!state) return;
  state->itemCount = 0;
  state->refreshCounter++;  // Invalidate old pointers
  // selectedIndex / scrollOffset deliberately preserved across the rebuild; they
  // are clamped back into range by oledScrollClampSelection() (auto-called from
  // oledScrollHandleNav / oledScrollRenderSimple, or explicitly by manual renders).
}

void oledScrollClampSelection(OLEDScrollState* state) {
  if (!state) return;
  if (state->itemCount <= 0) {
    state->selectedIndex = 0;
    state->scrollOffset = 0;
    return;
  }
  if (state->selectedIndex >= state->itemCount) state->selectedIndex = state->itemCount - 1;
  if (state->selectedIndex < 0) state->selectedIndex = 0;
  // Keep the selection inside the visible window.
  if (state->visibleLines > 0) {
    if (state->scrollOffset > state->selectedIndex) state->scrollOffset = state->selectedIndex;
    if (state->selectedIndex >= state->scrollOffset + state->visibleLines) {
      state->scrollOffset = state->selectedIndex - state->visibleLines + 1;
    }
    int maxOff = state->itemCount - state->visibleLines;
    if (maxOff < 0) maxOff = 0;
    if (state->scrollOffset > maxOff) state->scrollOffset = maxOff;
    if (state->scrollOffset < 0) state->scrollOffset = 0;
  }
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

bool oledScrollHandleNav(OLEDScrollState* state, bool leftRightNav) {
  if (!state || state->itemCount == 0) return false;
  oledScrollClampSelection(state);  // fix a stale cursor after a keep-selection rebuild
  extern NavEvents gNavEvents;
  bool handled = false;
  if (gNavEvents.up || (leftRightNav && gNavEvents.left)) {
    oledScrollUp(state);
    handled = true;
  } else if (gNavEvents.down || (leftRightNav && gNavEvents.right)) {
    oledScrollDown(state);
    handled = true;
  }
  return handled;
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

void oledScrollSetSplitPane(OLEDScrollState* state, int listWidth, int separatorX, int iconSize) {
  if (!state) return;
  state->listWidth = listWidth;
  state->separatorX = separatorX;
  state->iconSize = iconSize;
  state->singleLineItems = true;  // Split-pane defaults to single-line items
}

void oledScrollRender(Adafruit_SSD1306* display, OLEDScrollState* state, 
                      bool showScrollbar, bool showSelection,
                      const OLEDFooterHints* footerHints) {
  if (!display || !state) return;
  
  bool splitPane = (state->listWidth > 0);
  int lineHeight = 8;  // For text size 1
  int itemHeight = splitPane ? 10 : (lineHeight * 2);  // Single-line or two-line
  
  // Starting Y position
  int yPos = splitPane ? (OLED_CONTENT_START_Y + 1) : 0;
  
  // Draw title if present (full-width mode only; split-pane uses global header)
  if (!splitPane && state->title && state->title[0] != '\0') {
    display->setTextSize(1);
    display->setCursor(0, yPos);
    display->print(state->title);
    yPos += lineHeight + 2;
  }
  
  // Draw vertical separator for split-pane
  if (splitPane && state->separatorX > 0) {
    display->drawFastVLine(state->separatorX, OLED_CONTENT_START_Y, OLED_CONTENT_HEIGHT, DISPLAY_COLOR_WHITE);
  }
  
  // Calculate visible range
  int visibleStart = state->scrollOffset;
  int visibleEnd = min(state->itemCount, state->scrollOffset + state->visibleLines);
  
  display->setTextSize(1);
  
  // Draw visible items
  for (int i = visibleStart; i < visibleEnd; i++) {
    OLEDScrollItem* item = &state->items[i];
    bool isSelected = (i == state->selectedIndex);
    
    if (splitPane) {
      // Split-pane mode: inverse highlight, single line
      if (showSelection && isSelected) {
        display->fillRect(0, yPos, state->listWidth - 1, itemHeight - 1, DISPLAY_COLOR_WHITE);
        display->setTextColor(DISPLAY_COLOR_BLACK);
      } else {
        display->setTextColor(DISPLAY_COLOR_WHITE);
      }
      
      display->setCursor(2, yPos + 1);
      if (item->line1) display->print(item->line1);
      
      yPos += itemHeight;
    } else {
      // Full-width mode: selection bar, two lines
      if (showSelection && isSelected) {
        display->fillRect(0, yPos, 3, lineHeight * 2, DISPLAY_COLOR_WHITE);
        display->setCursor(5, yPos);
      } else {
        display->setCursor(0, yPos);
      }
      
      if (showSelection && isSelected) {
        display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
      } else {
        display->setTextColor(DISPLAY_COLOR_WHITE);
      }
      
      // Draw line1
      if (item->line1 && item->line1[0] != '\0') {
        int len = strlen(item->line1);
        if (len > 20) {
          char truncated[22];
          strncpy(truncated, item->line1, 19);
          truncated[19] = '~';
          truncated[20] = '\0';
          display->println(truncated);
        } else {
          display->println(item->line1);
        }
      } else {
        display->println("---");
      }
      
      // Draw line2
      yPos += lineHeight;
      if (showSelection && isSelected) {
        display->setCursor(5, yPos);
      } else {
        display->setCursor(0, yPos);
      }
      
      display->setTextColor(DISPLAY_COLOR_WHITE);
      if (item->line2 && item->line2[0] != '\0') {
        int len = strlen(item->line2);
        if (len > 20) {
          char truncated[22];
          strncpy(truncated, item->line2, 19);
          truncated[19] = '~';
          truncated[20] = '\0';
          display->println(truncated);
        } else {
          display->println(item->line2);
        }
      } else {
        display->println("");
      }
      
      yPos += lineHeight;
    }
  }
  
  // Reset text color
  display->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Draw selected item's icon + optional right-pane decoration (split-pane only)
  if (splitPane && state->itemCount > 0) {
    OLEDScrollItem* selected = &state->items[state->selectedIndex];
    int iconAreaX = state->separatorX + 4;
    int iconY = OLED_CONTENT_START_Y + (OLED_CONTENT_HEIGHT - state->iconSize) / 2;
    if (selected->iconName && selected->iconName[0] != '\0') {
      extern bool drawIcon(Adafruit_SSD1306* display, const char* name, int x, int y, uint16_t color);
      int iconX = iconAreaX + (128 - iconAreaX - state->iconSize) / 2;
      drawIcon(display, selected->iconName, iconX, iconY, DISPLAY_COLOR_WHITE);
    }
    // Right-pane decorator (e.g. availability badge + status text). Drawn after
    // the icon so it can overlay a corner badge and place text below the icon.
    if (state->rightPaneDraw) {
      state->rightPaneDraw(display, selected, iconAreaX, iconY, state->iconSize);
    }
  }
  
  // Draw scroll indicators
  if (state->itemCount > state->visibleLines) {
    if (splitPane) {
      // Arrow indicators near separator
      int arrowX = state->listWidth;
      if (state->scrollOffset > 0) {
        display->setCursor(arrowX, OLED_CONTENT_START_Y);
        display->print("\x18");
      }
      if (visibleEnd < state->itemCount) {
        display->setCursor(arrowX, OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 9);
        display->print("\x19");
      }
    } else if (showScrollbar) {
      // Scrollbar for full-width mode
      int scrollbarX = SCREEN_WIDTH - 1;
      bool hasTitle = (state->title && state->title[0] != '\0');
      int scrollbarHeight = OLED_CONTENT_HEIGHT - (hasTitle ? 10 : 0);
      int scrollbarY = hasTitle ? 10 : 0;
      
      display->drawFastVLine(scrollbarX, scrollbarY, scrollbarHeight, DISPLAY_COLOR_WHITE);
      
      int thumbHeight = max(4, (scrollbarHeight * state->visibleLines) / state->itemCount);
      int thumbY = scrollbarY + (scrollbarHeight - thumbHeight) * state->scrollOffset / 
                   max(1, state->itemCount - state->visibleLines);
      
      display->fillRect(scrollbarX - 1, thumbY, 3, thumbHeight, DISPLAY_COLOR_WHITE);
    }
  }
  
  // Render footer if hints provided
  if (footerHints) {
    oledRenderFooter(display, footerHints);
  }
}

// Lightweight single-line list renderer — see header for rationale. Compact
// counterpart to oledScrollRender(): one 8px line per item, "> " cursor prefix,
// so a 5-item menu shows all 5 at once (unlike oledScrollRender's 16px two-line
// items which only fit ~1-2 in the content area). Matches the Power / Network
// main-menu look exactly.
void oledScrollRenderSimple(Adafruit_SSD1306* display, OLEDScrollState* state,
                            bool showSelection) {
  if (!display || !state) return;
  oledScrollClampSelection(state);  // fix a stale cursor after a keep-selection rebuild

  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);

  const int lineHeight = 8;
  int visibleStart = state->scrollOffset;
  int visibleEnd = min(state->itemCount, state->scrollOffset + state->visibleLines);

  int yPos = OLED_CONTENT_START_Y;
  for (int i = visibleStart; i < visibleEnd; i++) {
    display->setCursor(0, yPos);
    display->print((showSelection && i == state->selectedIndex) ? "> " : "  ");
    if (state->items[i].line1) display->print(state->items[i].line1);
    display->println();
    yPos += lineHeight;
  }

  // Thin scrollbar only when the list overflows the visible window.
  if (state->itemCount > state->visibleLines) {
    int scrollbarX = SCREEN_WIDTH - 1;
    int barH = state->visibleLines * lineHeight;
    display->drawFastVLine(scrollbarX, OLED_CONTENT_START_Y, barH, DISPLAY_COLOR_WHITE);
    int thumbH = max(4, (barH * state->visibleLines) / state->itemCount);
    int thumbY = OLED_CONTENT_START_Y +
                 (barH - thumbH) * state->scrollOffset / max(1, state->itemCount - state->visibleLines);
    display->fillRect(scrollbarX - 1, thumbY, 3, thumbH, DISPLAY_COLOR_WHITE);
  }
}

// =============================================================================
// Virtual Keyboard Implementation
// =============================================================================

#include "System_Utils.h"

#if ENABLE_GAMEPAD_SENSOR
  #include "i2csensor_seesaw.h"  // For JOYSTICK_DEADZONE
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

// Pattern mode direction characters (stored as password text, hashed normally)
#define PATTERN_UP    '^'
#define PATTERN_DOWN  'v'
#define PATTERN_LEFT  '<'
#define PATTERN_RIGHT '>'

// Helper to get character at position based on current mode
static char getCharAt(int row, int col) {
  switch (gOledKeyboardState.mode) {
    case KEYBOARD_MODE_UPPERCASE: return OLED_KEYBOARD_CHARS_UPPER[row][col];
    case KEYBOARD_MODE_LOWERCASE: return OLED_KEYBOARD_CHARS_LOWER[row][col];
    case KEYBOARD_MODE_NUMBERS: return OLED_KEYBOARD_CHARS_NUMBERS[row][col];
    case KEYBOARD_MODE_PATTERN: return '\0';  // No grid in pattern mode
    default: return OLED_KEYBOARD_CHARS_UPPER[row][col];
  }
}

// Global keyboard state
OLEDKeyboardState gOledKeyboardState;

void oledKeyboardInit(const char* title, const char* initialText, int maxLength) {
  memset(gOledKeyboardState.text, 0, sizeof(gOledKeyboardState.text));
  gOledKeyboardState.textLength = 0;
  gOledKeyboardState.cursorX = 0;
  gOledKeyboardState.cursorY = 0;
  gOledKeyboardState.mode = KEYBOARD_MODE_LOWERCASE;  // Start with lowercase
  gOledKeyboardState.active = true;
  gOledKeyboardState.cancelled = false;
  gOledKeyboardState.completed = false;
  gOledKeyboardState.title = title ? String(title) : "Enter Text:";
  gOledKeyboardState.maxLength = min(maxLength, OLED_KEYBOARD_MAX_LENGTH);
  
  // Initialize autocomplete state
  gOledKeyboardState.autocompleteFunc = nullptr;
  gOledKeyboardState.autocompleteUserData = nullptr;
  gOledKeyboardState.showingSuggestions = false;
  gOledKeyboardState.suggestionCount = 0;
  gOledKeyboardState.selectedSuggestion = 0;
  memset(gOledKeyboardState.suggestions, 0, sizeof(gOledKeyboardState.suggestions));
  
  // Copy initial text if provided
  if (initialText && strlen(initialText) > 0) {
    strncpy(gOledKeyboardState.text, initialText, gOledKeyboardState.maxLength);
    gOledKeyboardState.textLength = strlen(gOledKeyboardState.text);
  }
}

void oledKeyboardReset() {
  gOledKeyboardState.active = false;
  gOledKeyboardState.cancelled = false;
  gOledKeyboardState.completed = false;
  memset(gOledKeyboardState.text, 0, sizeof(gOledKeyboardState.text));
  gOledKeyboardState.textLength = 0;
}

void oledKeyboardDisplay(Adafruit_SSD1306* display) {
  if (!display) return;
  
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Keyboard uses full screen (no header when active)
  const int keyboardStartY = 0;
  
  // If showing suggestions, render suggestion list instead of keyboard
  if (gOledKeyboardState.showingSuggestions && gOledKeyboardState.suggestionCount > 0) {
    // Title at top
    display->setCursor(0, keyboardStartY);
    display->print("Suggestions:");
    
    // Show current input
    display->setCursor(75, keyboardStartY);
    char inputPreview[10];
    strncpy(inputPreview, gOledKeyboardState.text, 8);
    inputPreview[8] = '\0';
    display->print(inputPreview);
    
    // List suggestions (up to 5 visible with full screen)
    int visibleCount = min(gOledKeyboardState.suggestionCount, 5);
    int startIdx = 0;
    if (gOledKeyboardState.selectedSuggestion >= 5) {
      startIdx = gOledKeyboardState.selectedSuggestion - 4;
    }
    
    for (int i = 0; i < visibleCount && (startIdx + i) < gOledKeyboardState.suggestionCount; i++) {
      int idx = startIdx + i;
      int y = keyboardStartY + 10 + i * 11;
      
      bool isSelected = (idx == gOledKeyboardState.selectedSuggestion);
      
      if (isSelected) {
        display->fillRect(0, y - 1, 128, 10, DISPLAY_COLOR_WHITE);
        display->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
      } else {
        display->setTextColor(DISPLAY_COLOR_WHITE);
      }
      
      display->setCursor(2, y);
      const char* suggestion = gOledKeyboardState.suggestions[idx];
      if (suggestion) {
        // Truncate long names
        char truncated[22];
        strncpy(truncated, suggestion, 21);
        truncated[21] = '\0';
        display->print(truncated);
      }
      
      display->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    // Footer is drawn by drawOLEDFooter() in updateOLEDDisplay()
    return;
  }
  
  // Pattern mode display (replaces grid with compass/direction input)
  if (gOledKeyboardState.mode == KEYBOARD_MODE_PATTERN) {
    // Title at top
    display->setCursor(0, keyboardStartY);
    display->print(gOledKeyboardState.title);

    // Text preview box - show pattern as arrow characters
    display->drawRect(0, keyboardStartY + 9, 128, 11, DISPLAY_COLOR_WHITE);
    display->setCursor(2, keyboardStartY + 11);
    int startChar = 0;
    if (gOledKeyboardState.textLength > 20) {
      startChar = gOledKeyboardState.textLength - 20;
    }
    for (int i = startChar; i < gOledKeyboardState.textLength; i++) {
      display->print(gOledKeyboardState.text[i]);
    }
    if ((millis() / 500) % 2 == 0) {
      display->print("_");
    }

    // Compass area
    int cx = 28;
    int compassY = keyboardStartY + 22;
    display->setCursor(cx, compassY);
    display->print("^");
    display->setCursor(cx - 12, compassY + 10);
    display->print("<");
    display->setCursor(cx, compassY + 10);
    display->print("+");
    display->setCursor(cx + 12, compassY + 10);
    display->print(">");
    display->setCursor(cx, compassY + 20);
    display->print("v");

    // Move count (right side)
    char buf[16];
    snprintf(buf, sizeof(buf), "%d moves", gOledKeyboardState.textLength);
    display->setCursor(64, compassY + 5);
    display->print(buf);

    return;
  }

  // Normal keyboard display
  // Draw title at top
  display->setCursor(0, keyboardStartY);
  display->print(gOledKeyboardState.title);
  
  // Show mode indicator at right edge (compact format)
  const char* modeStr = "";
  switch (gOledKeyboardState.mode) {
    case KEYBOARD_MODE_UPPERCASE: modeStr = "ABC"; break;
    case KEYBOARD_MODE_LOWERCASE: modeStr = "abc"; break;
    case KEYBOARD_MODE_NUMBERS: modeStr = "123"; break;
    case KEYBOARD_MODE_PATTERN: modeStr = "PAT"; break;
    case KEYBOARD_MODE_COUNT: break; // Should never happen
  }
  // Right-align mode indicator (3 chars + padding)
  display->setCursor(128 - (strlen(modeStr) * 6), keyboardStartY);
  display->print(modeStr);
  
  // Draw text preview box
  display->drawRect(0, keyboardStartY + 9, 128, 11, DISPLAY_COLOR_WHITE);
  display->setCursor(2, keyboardStartY + 11);
  
  // Show current text with cursor
  int textLen = strlen(gOledKeyboardState.text);
  const char* displayStart = gOledKeyboardState.text;
  if (textLen > 20) {
    // Scroll text if too long
    displayStart = gOledKeyboardState.text + (textLen - 20);
  }
  display->print(displayStart);
  
  // Show blinking cursor
  if ((millis() / 500) % 2 == 0) {
    display->print("_");
  }
  
  // Draw character grid (3 rows, now with more space from full screen)
  int startY = keyboardStartY + 22;
  int charWidth = 12;   // Width per character cell
  int charHeight = 10;  // Height per character row
  
  for (int row = 0; row < OLED_KEYBOARD_ROWS; row++) {
    for (int col = 0; col < OLED_KEYBOARD_COLS; col++) {
      int x = col * charWidth + 2;
      int y = startY + row * charHeight;
      
      char c = getCharAt(row, col);
      
      // Highlight current cursor position
      bool isCursor = (col == gOledKeyboardState.cursorX && row == gOledKeyboardState.cursorY);
      
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
  
  // Footer is drawn by drawOLEDFooter() in updateOLEDDisplay() -
  // do NOT draw a second footer here or they will overlap and garble.
}

bool oledKeyboardHandleInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (!gOledKeyboardState.active) {
    // Reset state when keyboard becomes inactive
    return false;
  }

  bool inputHandled = false;

  // Wheel input — mode-agnostic single channel. gNavEvents.wheelDelta carries
  // signed detent counts from any rotary input device (currently the ANO
  // encoder). It's deliberately separate from deltaX/Y so wheel input doesn't
  // have to fake joystick deflection — both signals can be non-zero on the
  // same frame and both will be honoured (joystick MoveRight/Left/Up/Down +
  // wheel Advance). oledKeyboardAdvance handles char-grid (row-major scan)
  // and suggestion-list (linear scroll) modes internally, so this one call
  // covers every keyboard sub-mode where wheel scrolling makes sense.
  if (gNavEvents.wheelDelta != 0) {
    oledKeyboardAdvance(gNavEvents.wheelDelta);
    inputHandled = true;
  }

  // Handle suggestion mode differently
  if (gOledKeyboardState.showingSuggestions) {
    // Y-axis navigates suggestions
    if (abs(deltaY) > JOYSTICK_DEADZONE) {
      static unsigned long lastSuggMove = 0;
      if (millis() - lastSuggMove > 150) {
        if (deltaY > 0 && gOledKeyboardState.selectedSuggestion < gOledKeyboardState.suggestionCount - 1) {
          gOledKeyboardState.selectedSuggestion++;
          lastSuggMove = millis();
          inputHandled = true;
        } else if (deltaY < 0 && gOledKeyboardState.selectedSuggestion > 0) {
          gOledKeyboardState.selectedSuggestion--;
          lastSuggMove = millis();
          inputHandled = true;
        }
      }
    }
    
    // A button selects suggestion
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      DEBUG_DISPLAYF("[KEYBOARD] A button - selecting suggestion");
      oledKeyboardSelectSuggestion();
      inputHandled = true;
    }
    
    // B button dismisses suggestions
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      DEBUG_DISPLAYF("[KEYBOARD] B button - dismissing suggestions");
      oledKeyboardDismissSuggestions();
      inputHandled = true;
    }
    
    return inputHandled;
  }
  
  // Pattern mode: joystick directions add direction characters directly
  if (gOledKeyboardState.mode == KEYBOARD_MODE_PATTERN) {
    static bool patternWasDeflected = false;

    bool deflected = (abs(deltaX) > JOYSTICK_DEADZONE) || (abs(deltaY) > JOYSTICK_DEADZONE);

    if (!deflected) {
      patternWasDeflected = false;
    } else if (!patternWasDeflected) {
      // First deflection from center - register one direction
      patternWasDeflected = true;

      char dirChar = 0;
      if (abs(deltaX) > abs(deltaY)) {
        dirChar = (deltaX > 0) ? PATTERN_RIGHT : PATTERN_LEFT;
      } else {
        dirChar = (deltaY > 0) ? PATTERN_DOWN : PATTERN_UP;
      }

      if (dirChar && gOledKeyboardState.textLength < gOledKeyboardState.maxLength) {
        gOledKeyboardState.text[gOledKeyboardState.textLength] = dirChar;
        gOledKeyboardState.textLength++;
        gOledKeyboardState.text[gOledKeyboardState.textLength] = '\0';
        inputHandled = true;
      }
    }

    // Buttons in pattern mode
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_START)) {
      oledKeyboardComplete();
      inputHandled = true;
    }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
      oledKeyboardBackspace();
      inputHandled = true;
    }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      oledKeyboardCancel();
      inputHandled = true;
    }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_SELECT)) {
      oledKeyboardToggleMode();
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
    DEBUG_DISPLAYF("[KEYBOARD] A button pressed - selecting char");
    oledKeyboardSelectChar();
    inputHandled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
    DEBUG_DISPLAYF("[KEYBOARD] Y button pressed - backspace (textLen=%d)\n", gOledKeyboardState.textLength);
    oledKeyboardBackspace();
    DEBUG_DISPLAYF("[KEYBOARD] After backspace: textLen=%d text='%s'\n", gOledKeyboardState.textLength, gOledKeyboardState.text);
    inputHandled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    DEBUG_DISPLAYF("[KEYBOARD] B button pressed - cancel");
    oledKeyboardCancel();
    inputHandled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_START)) {
    DEBUG_DISPLAYF("[KEYBOARD] X/START button pressed - complete");
    oledKeyboardComplete();
    inputHandled = true;
  }
  
  // SELECT button: autocomplete if provider set, otherwise toggle keyboard mode
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_SELECT)) {
    if (gOledKeyboardState.autocompleteFunc) {
      DEBUG_DISPLAYF("[KEYBOARD] SELECT button pressed - triggering autocomplete");
      oledKeyboardTriggerAutocomplete();
    } else {
      DEBUG_DISPLAYF("[KEYBOARD] SELECT button pressed - toggling mode");
      oledKeyboardToggleMode();
    }
    inputHandled = true;
  }
  
  // Only log when something actually happened (button edge or joystick move
  // that resulted in an action). This avoids spamming logs every frame when
  // the keyboard is idle.
  if (inputHandled) {
    DEBUG_DISPLAYF("[KEYBOARD] HANDLED: dX=%d dY=%d newly=0x%08lX textLen=%d\n", deltaX, deltaY, (unsigned long)newlyPressed, gOledKeyboardState.textLength);
    static bool sLoggedMasks = false;
    if (!sLoggedMasks) {
      DEBUG_DISPLAYF("[KEYBOARD] Button masks: A=0x%08lX B=0x%08lX X=0x%08lX Y=0x%08lX START=0x%08lX SEL=0x%08lX\n", (unsigned long)INPUT_MASK(INPUT_BUTTON_A), (unsigned long)INPUT_MASK(INPUT_BUTTON_B), 
                    (unsigned long)INPUT_MASK(INPUT_BUTTON_X), (unsigned long)INPUT_MASK(INPUT_BUTTON_Y), 
                    (unsigned long)INPUT_MASK(INPUT_BUTTON_START), (unsigned long)INPUT_MASK(INPUT_BUTTON_SELECT));
      sLoggedMasks = true;
    }
  }
  
  return inputHandled;
}

const char* oledKeyboardGetText() {
  return gOledKeyboardState.text;
}

bool oledKeyboardIsActive() {
  return gOledKeyboardState.active;
}

bool oledKeyboardDrawIfActive(Adafruit_SSD1306* display) {
  if (!gOledKeyboardState.active) return false;
  oledKeyboardDisplay(display);
  return true;
}

bool oledKeyboardIsCompleted() {
  return gOledKeyboardState.completed;
}

bool oledKeyboardIsCancelled() {
  return gOledKeyboardState.cancelled;
}

void oledKeyboardMoveUp() {
  if (gOledKeyboardState.cursorY > 0) {
    gOledKeyboardState.cursorY--;
  } else {
    gOledKeyboardState.cursorY = OLED_KEYBOARD_ROWS - 1;  // Wrap to bottom
  }
}

void oledKeyboardMoveDown() {
  if (gOledKeyboardState.cursorY < OLED_KEYBOARD_ROWS - 1) {
    gOledKeyboardState.cursorY++;
  } else {
    gOledKeyboardState.cursorY = 0;  // Wrap to top
  }
}

void oledKeyboardMoveLeft() {
  if (gOledKeyboardState.cursorX > 0) {
    gOledKeyboardState.cursorX--;
  } else {
    gOledKeyboardState.cursorX = OLED_KEYBOARD_COLS - 1;  // Wrap to right
  }
}

void oledKeyboardMoveRight() {
  if (gOledKeyboardState.cursorX < OLED_KEYBOARD_COLS - 1) {
    gOledKeyboardState.cursorX++;
  } else {
    gOledKeyboardState.cursorX = 0;  // Wrap to left
  }
}

// Row-major / linear scan across the keyboard.
//   • Normal char grid: treat the grid as a single linear strip of
//     (ROWS * COLS) keys and move `steps` positions, wrapping at both ends.
//     This is the behaviour the rotary wheel wants — a single dimension that
//     the user can swipe through without ever having to think about rows.
//   • Suggestions visible: scroll the suggestion list by `steps`, clamped at
//     the ends (no wrap, because suggestions are an ordered ranked list and
//     wrapping past the last one would be disorienting).
//   • Pattern mode: ignored (pattern uses gesture deflection, not a cursor).
//
// `steps` can be any signed int; large magnitudes (e.g. accumulated detents
// from a single OLED frame during a fast spin) are handled via positive-
// remainder modulo so the result lands on a valid cell either direction.
void oledKeyboardAdvance(int steps) {
  if (!gOledKeyboardState.active) return;
  if (steps == 0) return;

  if (gOledKeyboardState.showingSuggestions) {
    int last = (int)gOledKeyboardState.suggestionCount - 1;
    if (last < 0) return;  // nothing to scroll
    int next = (int)gOledKeyboardState.selectedSuggestion + steps;
    if (next < 0) next = 0;
    if (next > last) next = last;
    gOledKeyboardState.selectedSuggestion = (uint8_t)next;
    return;
  }

  if (gOledKeyboardState.mode == KEYBOARD_MODE_PATTERN) return;

  const int total = OLED_KEYBOARD_ROWS * OLED_KEYBOARD_COLS;
  int linear = gOledKeyboardState.cursorY * OLED_KEYBOARD_COLS + gOledKeyboardState.cursorX;
  // Signed-safe modulo with positive remainder so `steps` can be negative.
  int next = ((linear + steps) % total + total) % total;
  gOledKeyboardState.cursorY = next / OLED_KEYBOARD_COLS;
  gOledKeyboardState.cursorX = next % OLED_KEYBOARD_COLS;
}

void oledKeyboardSelectChar() {
  // Get character at current cursor position
  char selectedChar = getCharAt(gOledKeyboardState.cursorY, gOledKeyboardState.cursorX);
  
  DEBUG_DISPLAYF("[KEYBOARD_SELECT] Cursor at [%d,%d] char='%c' (0x%02X)\n", gOledKeyboardState.cursorX, gOledKeyboardState.cursorY, 
                selectedChar, (unsigned char)selectedChar);
  // Handle special characters
  if (selectedChar == CHAR_MODE) {
    // Toggle keyboard mode
    DEBUG_DISPLAYF("[KEYBOARD_SELECT] Mode toggle selected");
    oledKeyboardToggleMode();
    return;
  } else if (selectedChar == CHAR_BACK) {
    // Backspace
    DEBUG_DISPLAYF("[KEYBOARD_SELECT] DEL button selected");
    oledKeyboardBackspace();
    return;
  }
  
  // Add character if not at max length
  if (gOledKeyboardState.textLength < gOledKeyboardState.maxLength) {
    gOledKeyboardState.text[gOledKeyboardState.textLength] = selectedChar;
    gOledKeyboardState.textLength++;
    gOledKeyboardState.text[gOledKeyboardState.textLength] = '\0';
    DEBUG_DISPLAYF("[KEYBOARD_SELECT] Added char: textLength=%d text='%s'\n", gOledKeyboardState.textLength, gOledKeyboardState.text);
  } else {
    DEBUG_DISPLAYF("[KEYBOARD_SELECT] At max length (%d), cannot add char\n", gOledKeyboardState.maxLength);
  }
}

void oledKeyboardBackspace() {
  DEBUG_DISPLAYF("[KEYBOARD_BACKSPACE] Called: textLength=%d text='%s'\n", gOledKeyboardState.textLength, gOledKeyboardState.text);
  if (gOledKeyboardState.textLength > 0) {
    gOledKeyboardState.textLength--;
    gOledKeyboardState.text[gOledKeyboardState.textLength] = '\0';
    DEBUG_DISPLAYF("[KEYBOARD_BACKSPACE] Deleted char: new textLength=%d text='%s'\n", gOledKeyboardState.textLength, gOledKeyboardState.text);
  } else {
    DEBUG_DISPLAYF("[KEYBOARD_BACKSPACE] No characters to delete (textLength=0)");
  }
}

void oledKeyboardComplete() {
  gOledKeyboardState.completed = true;
  gOledKeyboardState.active = false;
}

void oledKeyboardCancel() {
  gOledKeyboardState.cancelled = true;
  gOledKeyboardState.active = false;
  DEBUG_DISPLAYF("[KEYBOARD] Cancelled");
}

void oledKeyboardToggleMode() {
  // Cycle through modes: lowercase -> uppercase -> numbers -> pattern -> lowercase
  gOledKeyboardState.mode = (OLEDKeyboardMode)((gOledKeyboardState.mode + 1) % KEYBOARD_MODE_COUNT);
  
  const char* modeName = "unknown";
  switch (gOledKeyboardState.mode) {
    case KEYBOARD_MODE_UPPERCASE: modeName = "UPPERCASE"; break;
    case KEYBOARD_MODE_LOWERCASE: modeName = "lowercase"; break;
    case KEYBOARD_MODE_NUMBERS: modeName = "123/symbols"; break;
    case KEYBOARD_MODE_PATTERN: modeName = "PATTERN"; break;
    case KEYBOARD_MODE_COUNT: break; // Should never happen
  }
  
  DEBUG_DISPLAYF("[KEYBOARD] Mode changed to: %s\n", modeName);
}

// ============================================================================
// Autocomplete Support (Select button triggers suggestions)
// ============================================================================

void oledKeyboardSetAutocomplete(OLEDKeyboardAutocompleteFunc func, void* userData) {
  gOledKeyboardState.autocompleteFunc = func;
  gOledKeyboardState.autocompleteUserData = userData;
  DEBUG_DISPLAYF("[KEYBOARD] Autocomplete provider %s\n", func ? "set" : "cleared");
}

void oledKeyboardTriggerAutocomplete() {
  if (!gOledKeyboardState.autocompleteFunc) {
    DEBUG_DISPLAYF("[KEYBOARD] No autocomplete provider set");
    return;
  }
  
  // Call the autocomplete provider
  gOledKeyboardState.suggestionCount = gOledKeyboardState.autocompleteFunc(
    gOledKeyboardState.text,
    gOledKeyboardState.suggestions,
    OLED_KEYBOARD_MAX_SUGGESTIONS,
    gOledKeyboardState.autocompleteUserData
  );
  
  if (gOledKeyboardState.suggestionCount > 0) {
    gOledKeyboardState.showingSuggestions = true;
    gOledKeyboardState.selectedSuggestion = 0;
    DEBUG_DISPLAYF("[KEYBOARD] Autocomplete found %d suggestions for '%s'\n", gOledKeyboardState.suggestionCount, gOledKeyboardState.text);
  } else {
    DEBUG_DISPLAYF("[KEYBOARD] No suggestions found for '%s'\n", gOledKeyboardState.text);
  }
}

void oledKeyboardSelectSuggestion() {
  if (!gOledKeyboardState.showingSuggestions || gOledKeyboardState.suggestionCount == 0) {
    return;
  }
  
  const char* selected = gOledKeyboardState.suggestions[gOledKeyboardState.selectedSuggestion];
  if (selected) {
    // Copy the selected suggestion to the text field
    strncpy(gOledKeyboardState.text, selected, gOledKeyboardState.maxLength);
    gOledKeyboardState.text[gOledKeyboardState.maxLength] = '\0';
    gOledKeyboardState.textLength = strlen(gOledKeyboardState.text);
    DEBUG_DISPLAYF("[KEYBOARD] Selected suggestion: '%s'\n", selected);
  }
  
  oledKeyboardDismissSuggestions();
}

void oledKeyboardDismissSuggestions() {
  gOledKeyboardState.showingSuggestions = false;
  gOledKeyboardState.suggestionCount = 0;
  gOledKeyboardState.selectedSuggestion = 0;
}

bool oledKeyboardShowingSuggestions() {
  return gOledKeyboardState.showingSuggestions;
}

struct OLEDConfirmState {
  bool active;
  const char* line1;
  const char* line2;
  bool selectYes;
  OLEDConfirmCallback onYes;
  void* userData;
};

static OLEDConfirmState gOledConfirmState = {false, nullptr, nullptr, true, nullptr, nullptr};

bool oledConfirmRequest(const char* line1, const char* line2, OLEDConfirmCallback onYes, void* userData, bool defaultYes) {
  if (gOledConfirmState.active) return false;
  gOledConfirmState.active = true;
  gOledConfirmState.line1 = line1;
  gOledConfirmState.line2 = line2;
  gOledConfirmState.selectYes = defaultYes;
  gOledConfirmState.onYes = onYes;
  gOledConfirmState.userData = userData;

  DEBUG_DISPLAYF("[OLED_CONFIRM] %s%s%s\n", line1 ? line1 : "",
                (line1 && line2) ? " | " : "",
                line2 ? line2 : "");
  DEBUG_DISPLAYF("[OLED_CONFIRM] Use UP/DOWN to select, A to confirm, B to cancel");
  oledMarkDirty();
  return true;
}

bool oledConfirmIsActive() {
  return gOledConfirmState.active;
}

static void oledConfirmClose(bool confirmed) {
  if (!gOledConfirmState.active) return;
  DEBUG_DISPLAYF("[OLED_CONFIRM] %s\n", confirmed ? "CONFIRMED" : "CANCELLED");
  gOledConfirmState.active = false;
  gOledConfirmState.line1 = nullptr;
  gOledConfirmState.line2 = nullptr;
  gOledConfirmState.selectYes = true;
  gOledConfirmState.onYes = nullptr;
  gOledConfirmState.userData = nullptr;
  oledMarkDirty();
}

static bool oledConfirmHandleInput(uint32_t newlyPressed) {
  if (!gOledConfirmState.active) return false;

  bool handled = false;

  // UP = Yes, DOWN = No. LEFT/RIGHT used to also toggle but that conflicted
  // with ANO LEFT-as-cancel — pressing LEFT would flip the selection then
  // immediately fire B-cancel in the same tick. UP/DOWN is sufficient and
  // unambiguous.
  if (gNavEvents.up) {
    gOledConfirmState.selectYes = true;
    oledMarkDirty();
    handled = true;
  } else if (gNavEvents.down) {
    gOledConfirmState.selectYes = false;
    oledMarkDirty();
    handled = true;
  }

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (gOledConfirmState.selectYes) {
      if (gOledConfirmState.onYes) {
        gOledConfirmState.onYes(gOledConfirmState.userData);
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
  if (!gOledConfirmState.active || !oledDisplay) return;

  const int boxX = 2;
  const int boxY = 2;
  const int boxW = SCREEN_WIDTH - 4;
  const int boxH = OLED_CONTENT_HEIGHT - 4;

  oledDisplay->fillRect(boxX, boxY, boxW, boxH, DISPLAY_COLOR_BLACK);
  oledDisplay->drawRect(boxX, boxY, boxW, boxH, DISPLAY_COLOR_WHITE);

  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

  // No title row: the box top overlaps the header bar, so anything drawn there
  // gets clipped. Start the question at +14 so it clears the header.
  int y = boxY + 14;
  if (gOledConfirmState.line1) {
    oledDisplay->setCursor(boxX + 4, y);
    oledDisplay->print(gOledConfirmState.line1);
    y += 10;
  }
  if (gOledConfirmState.line2) {
    oledDisplay->setCursor(boxX + 4, y);
    oledDisplay->print(gOledConfirmState.line2);
    y += 10;
  }

  int optY = boxY + boxH - 18;
  const int optX = boxX + 6;
  const int optW = boxW - 12;
  const int optH = 9;

  if (gOledConfirmState.selectYes) {
    oledDisplay->fillRect(optX, optY, optW, optH, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
  } else {
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  }
  oledDisplay->setCursor(optX + 2, optY + 1);
  oledDisplay->print("Yes");

  if (!gOledConfirmState.selectYes) {
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
OLEDConsoleBuffer gOledConsole;

// Constructor
OLEDConsoleBuffer::OLEDConsoleBuffer()
  : head(0), count(0), capacity(OLED_CONSOLE_LINES), mutex(nullptr) {
  memset(lines, 0, sizeof(lines));
  memset(timestamps, 0, sizeof(timestamps));
}

// Initialize buffer and mutex
void OLEDConsoleBuffer::init() {
  head = 0;
  count = 0;
  // Latch effective history depth from settings (clamped to physical capacity).
  int eff = gSettings.oledCliHistorySize;
  if (eff < 10) eff = 10; else if (eff > OLED_CONSOLE_LINES) eff = OLED_CONSOLE_LINES;
  capacity = (uint8_t)eff;
  memset(lines, 0, sizeof(lines));
  memset(timestamps, 0, sizeof(timestamps));

  if (!mutex) {
    mutex = xSemaphoreCreateMutex();
    if (mutex) {
      DEBUG_SYSTEMF("OLED console buffer initialized (%d/%d lines × %d chars)",
                    (int)capacity, OLED_CONSOLE_LINES, OLED_CONSOLE_LINE_LEN);
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
    
    // Advance head (circular, within the effective capacity)
    head = (head + 1) % capacity;
    
    // Update count (saturate at effective capacity)
    if (count < capacity) {
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
  if (count < capacity) {
    bufferIndex = index;
  } else {
    bufferIndex = (head + index) % capacity;
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
  if (count < capacity) {
    bufferIndex = index;
  } else {
    bufferIndex = (head + index) % capacity;
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
#include "Bluetooth.h"
#endif

#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif

// External state variables needed for context-aware footer hints
extern String unavailableOLEDTitle;
extern String unavailableOLEDReason;

// FileBrowserRenderData defined in System_FileManager.h (included above)
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

// Draw a small curved back arrow icon (↩) inline at current cursor, 7px wide x 5px tall
void oledDrawBackArrowIcon(Adafruit_SSD1306* d, int footerY) {
  //     XX.       0x0C
  //       X       0x02
  //  X  XX.       0x46
  // XXXXXX.       0xFC
  //  X.....       0x40
  static const uint8_t icon[] PROGMEM = {0x0C, 0x02, 0x46, 0xFC, 0x40};
  int x = d->getCursorX();
  d->drawBitmap(x, footerY + 1, icon, 7, 5, DISPLAY_COLOR_WHITE);
  d->setCursor(x + 8, footerY);  // advance past icon + 1px gap
}
static void drawBackArrowIcon(Adafruit_SSD1306* d, int footerY) {
  oledDrawBackArrowIcon(d, footerY);
}

// Draw the persistent button hint footer for the current mode/state
void drawOLEDFooter() {
  if (!oledDisplay) return;
  
  // Skip footer for animations and screen off mode
  if (currentOLEDMode == OLED_ANIMATION || currentOLEDMode == OLED_OFF) {
    return;
  }
  
  // Footer starts after header + content area
  const int footerStartY = OLED_HEADER_HEIGHT + OLED_CONTENT_HEIGHT;
  const int footerY = footerStartY + 2;  // Text position (2px below separator)
  
  // Clear footer area (prevents content from leaking through)
  oledDisplay->fillRect(0, footerStartY, SCREEN_WIDTH, OLED_FOOTER_HEIGHT, DISPLAY_COLOR_BLACK);
  
  // Draw separator line above footer
  // For logo mode, draw shorter line with vertical box around "Back" text
  if (currentOLEDMode == OLED_LOGO) {
    // Horizontal line - only 1/3 width from left
    oledDisplay->drawFastHLine(0, footerStartY, SCREEN_WIDTH / 3, DISPLAY_COLOR_WHITE);
    // Vertical line down from end of horizontal line
    oledDisplay->drawFastVLine(SCREEN_WIDTH / 3, footerStartY, OLED_FOOTER_HEIGHT, DISPLAY_COLOR_WHITE);
  } else {
    // Normal full-width separator for other modes
    oledDisplay->drawFastHLine(0, footerStartY, SCREEN_WIDTH, DISPLAY_COLOR_WHITE);
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
    extern OLEDKeyboardState gOledKeyboardState;
    if (gOledKeyboardState.mode == KEYBOARD_MODE_PATTERN) {
      oledDisplay->print("A:Done Y:Undo B:");
      drawBackArrowIcon(oledDisplay, footerY);
    } else if (gOledKeyboardState.showingSuggestions) {
      oledDisplay->print("A:Pick B:");
      drawBackArrowIcon(oledDisplay, footerY);
      oledDisplay->print("\x1e\x1f:Nav");
    } else if (gOledKeyboardState.autocompleteFunc) {
      oledDisplay->print("A:Sel Y:Del S:OK");
    } else {
      oledDisplay->print("A:Sel Y:Del B:");
      drawBackArrowIcon(oledDisplay, footerY);
      oledDisplay->print(" S:OK");
    }
    return;
  }
  
  // Check registered mode hints first (avoids central switch for self-describing modes)
  const char* hints = nullptr;
  {
    const OLEDModeEntry* regMode = findOLEDMode(currentOLEDMode);
    if (regMode && regMode->hints) {
      hints = regMode->hints;
    }
  }
  
  // Fall back to central switch for modes without registered hints
  // OLED_MENU hints provided via OLEDModeEntry registration (static "A:Select B:Back")
  if (!hints) switch (currentOLEDMode) {
    case OLED_NOTIFICATIONS:
      hints = sNotificationsShowingDetail
        ? "\x18\x19:Nav B:Back"       // slim up/down arrows (detail nav is up/down), matching the ←→ arrow style
        : "A:Detail X:Clear B:Back";  // list view
      break;
    case OLED_ESPNOW:
      #if ENABLE_ESPNOW
      {
        extern OLEDEspNowState gOledEspNowState;
        switch (gOledEspNowState.currentView) {
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
            // File mode shows a Send/Receive selector (A=select); Text/Remote send (A=send).
            hints = (gOledEspNowState.interactionMode == ESPNOW_MODE_FILE)
                      ? "A:Select X:Mode B:Back"
                      : "A:Send X:Mode B:Back";
            break;
          case ESPNOW_VIEW_MODE_SELECT:
            hints = "A:Select B:Cancel";
            break;
          case ESPNOW_VIEW_PAIRING:
            hints = "A:Toggle B:Back";
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
      
    // OLED_NETWORK_INFO / OLED_NETWORK_STATUS / OLED_NETWORK_WIFI_MENU hints
    // provided via OLEDModeEntry::hints in OLED_Mode_Network.cpp

    case OLED_FILE_BROWSER:
      // Show "A:Open" only for folders, just "B:Back" for files
      if (fileBrowserRenderData.valid && fileBrowserRenderData.selectedIsFolder) {
        hints = "A:Open B:Back";
      } else {
        hints = "B:Back";
      }
      break;
      
    // OLED_BLUETOOTH / _STATUS / _G2 / _G2_STATUS hints come from OLEDModeEntry::hints

    // OLED_SYSTEM_STATUS, OLED_SENSOR_DATA, OLED_SENSOR_LIST, OLED_BOOT_SENSORS, OLED_MEMORY_STATS
    // hints provided via OLEDModeEntry registration (static "B:Back")

    case OLED_WEB_STATS:
#if ENABLE_HTTP_SERVER
      {
        hints = server ? "X:Stop B:Back" : "X:Start B:Back";
      }
#else
      hints = "B:Back";
#endif
      break;

    case OLED_RTC_DATA:
#if ENABLE_RTC_SENSOR
      {
        extern bool gRtcEnabled;
        extern bool gRtcConnected;
        hints = (gRtcEnabled && gRtcConnected) ? "X:Stop B:Back" : "X:Start B:Back";
      }
#else
      hints = "B:Back";
#endif
      break;

    case OLED_PRESENCE_DATA:
#if ENABLE_PRESENCE_SENSOR
      {
        extern bool gPresenceEnabled;
        extern bool gPresenceConnected;
        hints = (gPresenceEnabled && gPresenceConnected) ? "X:Stop B:Back" : "X:Start B:Back";
      }
#else
      hints = "B:Back";
#endif
      break;
    
    // OLED_REMOTE hints provided via OLEDModeEntry registration (static "A:Select  B:Back")
    // OLED_CUSTOM_TEXT, OLED_LOGO, OLED_ANIMATION hints provided via OLEDModeEntry registration (static "B:Back")

    case OLED_CLI_VIEWER:
      {
        // Show selected/total in footer
        extern OLEDConsoleBuffer gOledConsole;
        extern int getCLIViewerSelectedIndex();
        int lineCount = gOledConsole.getLineCount();
        int selected = getCLIViewerSelectedIndex();
        static char cliHints[32];
        snprintf(cliHints, sizeof(cliHints), "A:Info B:Back [%d/%d]", selected, lineCount);
        hints = cliHints;
      }
      break;

    case OLED_CLI_INPUT:
      hints = "A:Send B:Back";
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
      
    case OLED_GPS_MAP:
      {
#if ENABLE_MAPS
        extern bool gMapMenuOpen;
        if (gMapMenuOpen) {
          hints = "A:Select B:Close";
        } else {
          hints = "X/Y:Zoom A+J:Rot B:Back";
        }
#else
        hints = "B:Back";
#endif
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
  
  // Draw the hint text, replacing "B:Back" with B: + curved arrow icon
  if (hints) {
    const char* backStr = strstr(hints, "B:Back");
    if (backStr) {
      // Print everything before "B:Back"
      int prefixLen = backStr - hints;
      if (prefixLen > 0) {
        char prefix[32];
        strncpy(prefix, hints, prefixLen);
        prefix[prefixLen] = '\0';
        oledDisplay->print(prefix);
      }
      // Draw "B:" + curved arrow icon
      oledDisplay->print("B:");
      drawBackArrowIcon(oledDisplay, footerY);
      // Print anything after "B:Back"
      const char* suffix = backStr + 6;  // strlen("B:Back") = 6
      if (*suffix) {
        oledDisplay->print(suffix);
      }
    } else {
      oledDisplay->print(hints);
    }
  }
}

// =============================================================================
// Shared Command Execution
// =============================================================================

#include "System_User.h"
#include "System_Command.h"

// Returns true if the local display should be blocked pending authentication.
// Use this instead of repeating the three-part condition everywhere.
bool shouldBlockForDisplayAuth() {
  return gSettings.localDisplayRequireAuth && !gLocalDisplayAuthed && !oledBootModeActive;
}

// Public AuthContext builder — single source of truth for "what OLED
// identity looks like." Used by buildOLEDCommand (async-submit path) and
// oledFileBrowserAuthContext (sync direct-FS path). Mirrors
// g2HijackAuthContext on the G2 side; centralizing prevents the drift-bug
// class that Pass 1 caught on G2 (two hand-built AuthContexts in different
// files going out of sync as fields are added/changed). Also explicitly
// initializes `opaque = nullptr` — buildOLEDCommand previously relied on
// `Command uc;` default-init, leaving opaque uninitialized (benign because
// executeCommand only reads opaque under SOURCE_WEB, but a latent bug).
//
// When displayRequireAuth is off, the "AuthBypass" reserved name is
// stamped so audit logs read `[CMD] AuthBypass@display: ...` (clear
// physical-user origin) instead of `[CMD] @display: ...` (ambiguous —
// could be a username-propagation bug). Reserved name; see adminCreateUser.
AuthContext oledAuthContext(const char* path) {
  AuthContext ctx;
  ctx.transport = SOURCE_LOCAL_DISPLAY;
  ctx.user      = gLocalDisplayAuthed ? gLocalDisplayUser : String("AuthBypass");
  ctx.ip        = "oled";
  ctx.path      = path ? path : "/oled";
  ctx.sid       = "";
  ctx.opaque    = nullptr;
  return ctx;
}

// Build a Command struct for OLED-originated commands.
// Routed through cmd_exec_task via submitAndExecuteSync for unified
// output routing, audit logging, and stack safety.
static Command buildOLEDCommand(const String& cmdLine) {
  Command uc;
  uc.line = cmdLine;
  uc.ctx.origin = ORIGIN_LOCAL_DISPLAY;
  uc.ctx.auth = oledAuthContext("/oled/command");
  uc.ctx.id = (uint32_t)millis();
  uc.ctx.timestampMs = (uint32_t)millis();
  uc.ctx.outputMask = CMD_OUT_LOG;  // file log + OLED console (via MSG_ROUTE_OLED), no serial echo
  uc.ctx.validateOnly = false;
  uc.ctx.replyHandle = nullptr;
  uc.ctx.httpReq = nullptr;
  return uc;
}

void executeOLEDCommand(const String& argsInput) {
  extern bool submitAndExecuteSync(const Command& cmd, String& out);

  Command uc = buildOLEDCommand(argsInput);
  String out;
  bool success = submitAndExecuteSync(uc, out);

  if (!success && out.length() > 0) {
    DEBUG_DISPLAYF("[OLED_CMD] Command failed: %s", out.c_str());
  }
}

bool executeOLEDCommandWithResult(const String& argsInput, char* out, size_t outSize) {
  extern bool submitAndExecuteSync(const Command& cmd, String& outStr);

  Command uc = buildOLEDCommand(argsInput);
  String outStr;
  bool success = submitAndExecuteSync(uc, outStr);

  // Copy result to caller's buffer
  strncpy(out, outStr.c_str(), outSize - 1);
  out[outSize - 1] = '\0';

  if (!success && outStr.length() > 0) {
    DEBUG_DISPLAYF("[OLED_CMD] Command failed: %s", out);
  }
  return success;
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

// ============================================================================
// OLED Change Detection - Skip rendering when nothing has changed
// ============================================================================
// Uses existing sequence counters from sensor caches to detect changes
static uint32_t oledLastRenderedGamepadSeq = 0;
static unsigned long oledLastRenderedSensorSeq = 0;
static bool oledForceNextRender = true;  // Force first render
static unsigned long oledDirtyUntilMs = 0;  // Keep rendering dirty until this time (for timed popups)

// Manual dirty flag for non-sensor changes (menu state, settings, etc.)
void oledMarkDirty() {
  oledForceNextRender = true;
}

void oledMarkDirtyMode(OLEDMode mode) {
  // For compatibility - any mode change triggers dirty
  oledForceNextRender = true;
}

void oledMarkDirtyUntil(unsigned long untilMs) {
  if (untilMs > oledDirtyUntilMs) oledDirtyUntilMs = untilMs;
}

bool oledIsDirty() {
  extern volatile unsigned long gSensorStatusSeq;
  extern InputCache gInputCache;
  
  if (oledForceNextRender) return true;
  if (gInputCache.seq != oledLastRenderedGamepadSeq) return true;
  if (gSensorStatusSeq != oledLastRenderedSensorSeq) return true;
  if (oledPairingRibbonActive()) return true;  // Continuous render during notification animations
  if (millis() < oledDirtyUntilMs) return true;  // Timed dirty (popup auto-dismiss, etc.)
  return false;
}

void oledClearDirty() {
  extern volatile unsigned long gSensorStatusSeq;
  extern InputCache gInputCache;
  
  oledForceNextRender = false;
  oledLastRenderedGamepadSeq = gInputCache.seq;
  oledLastRenderedSensorSeq = gSensorStatusSeq;
}

void oledSetAlwaysDirty(bool always) {
  // For animations - just keep forcing renders
  if (always) oledForceNextRender = true;
}

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
bool oledConnected = false;
bool gOledEnabled = false;

// ESP-NOW mesh functions from .ino
// gMeshPeers, gMeshPeerSlots declared in System_ESPNow.h (pointer, not array)
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
void prepareAutomationData();
// OLED state variables (defined here, used by .ino and this file)
// Initial mode will be set based on oledRequireAuth setting during initialization
OLEDMode currentOLEDMode = OLED_SYSTEM_STATUS;
static OLEDMode lastRenderedMode = OLED_OFF;  // Track mode changes to force immediate refresh

void setOLEDMode(OLEDMode newMode) {
  currentOLEDMode = newMode;
}

// Single authoritative mode transition entry point.
// Auth gating, back-nav stack push, and standardised "[OLED_MODE]" logging.
// pushStack=false for boot/system/replace-in-place transitions that must not pollute history.
void requestOLEDMode(OLEDMode newMode, const char* reason, bool pushStack, bool isBackNav) {
  // Remember where we came from so the centralized onEnter hook below fires only
  // on a real mode change (and the auth-gate rewrite of newMode is accounted for).
  OLEDMode prevMode = currentOLEDMode;

  // Auth gate: redirect to LOGIN if display auth is required and not yet satisfied.
  // Boot sequence bypasses this (oledBootModeActive guards the check).
  if (shouldBlockForDisplayAuth()) {
    if (newMode != OLED_LOGIN) {
      DEBUG_DISPLAYF("[OLED_MODE] AUTH_GATE %s -> LOGIN (wanted:%s) | %s\n", getOLEDModeName(currentOLEDMode), getOLEDModeName(newMode),
                    reason ? reason : "");
      newMode = OLED_LOGIN;
      pushStack = false;  // auth redirects don't pollute back-nav
    }
  }

  // Standardised transition log — always emitted so serial trace shows all
  // mode changes. (A duplicate DEBUG_DISPLAYF call used to live here from
  // the era when the author wasn't sure which flag was right; collapsed to
  // one line after migrating all OLED-internal logs to DEBUG_DISPLAYF.)
  DEBUG_DISPLAYF("[OLED_MODE] %s -> %s | %s\n", getOLEDModeName(currentOLEDMode), getOLEDModeName(newMode),
                reason ? reason : "");

  // Push current mode to back-nav stack before switching (if requested and mode actually changes).
  if (pushStack && newMode != currentOLEDMode) {
    pushOLEDMode(currentOLEDMode);
  }

  // Per-mode entry side-effects (state resets, hardware inits) are no longer
  // hand-coded here, in cmd_oledmode, and in the menu-select path. Each mode now
  // owns its own reset via OLEDModeEntry::onEnterFunc, invoked centrally just
  // below — see the onEnter dispatch after `currentOLEDMode = newMode`.

#if ENABLE_ANO_ENCODER
  // Reset the rotary axis on every mode change so a horizontal-axis flip from
  // one mode doesn't bleed into the next. MVP defaults to vertical for every
  // mode — a per-mode hint can be added later if a mode prefers to start
  // horizontal (radio tuner, map zoom, etc.).
  if (newMode != currentOLEDMode) {
    anoEncoderResetAxisForMode(ANO_AXIS_VERTICAL);
  }
#endif

  currentOLEDMode = newMode;

  // Centralized entry hook — the single owner of "what happens when this mode
  // becomes current". Fires only on a real change; the mode decides (via the
  // isForward flag) whether to reset its view or preserve it on back-nav.
  if (newMode != prevMode) {
    const OLEDModeEntry* entered = findOLEDMode(newMode);
    if (entered && entered->onEnterFunc) {
      entered->onEnterFunc(!isBackNav);
    }
  }
}

// Mode navigation stack for back button (minimal fixed-size stack)
#define OLED_MODE_STACK_SIZE 8
static OLEDMode modeStack[OLED_MODE_STACK_SIZE];
static int modeStackDepth = 0;
String customOLEDText = "";
unsigned long oledLastUpdate = 0;
unsigned long animationFrame = 0;
unsigned long animationLastUpdate = 0;
int animationFPS = 10;
// Define current animation state (extern in header)
OLEDAnimationType currentAnimation = ANIM_BOOT_PROGRESS;

// ============================================================================
// OLED Mode Registration System
// ============================================================================

// Static storage for registered OLED modes
static const OLEDModeEntry* oledModeRegistry[MAX_OLED_MODES];
static size_t oledModeRegistrySize = 0;

// Module tracking for debug
#define MAX_OLED_MODULES 32  // module-NAME tracking for the boot summary only (not a mode cap);
                             // was 16, which truncated the list and hid GC'd modules during debugging
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
  -1,
  "A:Toggle B:Back"
};

// Force linker to include OLED mode files that use static registration
// Without these calls, the linker may drop object files with no external references
extern void oledAuthModeInit();
extern void oledLoggingModeInit();
extern void oledSetPatternModeInit();
extern void oledChangePasswordModeInit();
extern void oledPowerModeInit();
extern void oledCLIInputModeInit();
extern void oledMenuModeInit();   // Menu / Logo / Sensor-menu registrars (OLED_Mode_Menu.cpp)
#if ENABLE_BLUETOOTH
extern void oledBluetoothModeInit();   // OLED_Mode_Bluetooth.cpp
#endif
#if ENABLE_ESPNOW && ENABLE_BONDED_MODE
extern void oledRemoteSettingsModeInit();   // OLED_Mode_RemoteSettings.cpp
#endif
#if ENABLE_ONDEVICE_LLM
extern void oledLLMModeInit();
#endif

// Print summary of all registered OLED modes (call from setup() after static init)
void printRegisteredOLEDModes() {
  // Force linker to include mode files (static registrars run during global init,
  // but we need external references to prevent linker from dropping these files)
  oledAuthModeInit();
  oledLoggingModeInit();
  oledSetPatternModeInit();
  oledChangePasswordModeInit();
  oledPowerModeInit();
  oledCLIInputModeInit();
  oledMenuModeInit();   // keep OLED_Mode_Menu.cpp (Menu/Logo/Sensor-menu) from being GC'd
#if ENABLE_BLUETOOTH
  oledBluetoothModeInit();   // keep OLED_Mode_Bluetooth.cpp from being GC'd
#endif
#if ENABLE_ESPNOW && ENABLE_BONDED_MODE
  oledRemoteSettingsModeInit();   // keep OLED_Mode_RemoteSettings.cpp from being GC'd
#endif
#if ENABLE_ONDEVICE_LLM
  oledLLMModeInit();
#endif
  
  // Register built-in quick settings mode first
  static bool builtInRegistered = false;
  if (!builtInRegistered) {
    registerOLEDMode(&builtInQuickSettingsMode);
    builtInRegistered = true;
  }
  
  DEBUG_DISPLAYF("[OLED_MODE] %d modes registered from %d modules:\n", oledModeRegistrySize, registeredOLEDModuleCount);
  for (size_t i = 0; i < registeredOLEDModuleCount; i++) {
    DEBUG_DISPLAYF("  - %s (%d modes)\n", registeredOLEDModules[i].name, 
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
    DEBUG_DISPLAYF("[OLED_MODE] %s: %d -> %d | %s\n", src, (int)from, (int)to, extra.c_str());
  } else {
    DEBUG_DISPLAYF("[OLED_MODE] %s: %d -> %d\n", src, (int)from, (int)to);
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
  requestOLEDMode(OLED_UNAVAILABLE, "unavail.enter", false);
}

extern OLEDAnimationType currentAnimation;
extern const OLEDAnimation gAnimationRegistry[];
extern const int gAnimationCount;

// Sensor state (managed by I2C system)
// (Sensor enabled/connected flags are provided by the per-sensor i2csensor_*.h
// headers included above; no extern re-declarations needed here.)

// Modular sensor caches (each sensor defines its own cache)
// Includes are conditional based on sensor availability

// Settings and EspNowState now come from espnow_system.h -> settings.h

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

// Helper functions
extern bool meshEnabled();
extern String getEspNowDeviceName(const uint8_t* mac);
// macToHexString, macFromHexString, and macEqual6 now in espnow_system.h
extern void imuUpdateActions();

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

  DEBUG_DISPLAYF("Starting display initialization (%s)...", DISPLAY_NAME);

  // Use Display HAL initialization
  bool success = displayInit();
  
  if (success) {
    oledConnected = true;
    gOledEnabled = true;
    
    broadcastOutput("Display initialized successfully");
    INFO_SYSTEMF("Display initialized: %s (%dx%d)", DISPLAY_NAME, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    
    // Show initial splash screen
    gDisplay->clearDisplay();
    gDisplay->setRotation(gSettings.oledFlipped ? 2 : 0);
    gDisplay->setTextSize(1);
    gDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    gDisplay->setCursor(0, 0);
    gDisplay->print("HardwareOne v");
    gDisplay->println(esp_app_get_description()->version);
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
  // Use i2cTransaction wrapper for safe mutex + clock management.
  // delete/null must happen after the transaction completes, not inside it.
  i2cOledTransactionVoid(400000, 500, [&]() {
    gDisplay->clearDisplay();
    displayUpdate();
  });
  delete gDisplay;
  gDisplay = nullptr;
#else
  // For SPI displays, no transaction needed
  displayClear();
  displayUpdate();
  delete gDisplay;
  gDisplay = nullptr;
#endif

  oledConnected = false;
  gOledEnabled = false;

  DEBUG_DISPLAYF("Display stopped");
}

// ============================================================================
// Display Mode Functions
// ============================================================================

// displaySystemStatus() moved to OLED_Mode_System.cpp
// displaySensorData() moved to OLED_Mode_Sensors.cpp
// displayThermalVisual() moved to i2csensor_mlx90640.cpp (modular OLED mode)

// displayGPSData() moved to i2csensor_pa1010d.cpp (modular OLED mode)

// displayFmRadio() moved to fm_radio.cpp (modular OLED mode)

// Power functions moved to OLED_Mode_Power.cpp
#include "OLED_Utils.h"  // For executeOLEDCommand

// Network and system display functions moved to OLED_Mode_Network.cpp and OLED_Mode_System.cpp
// (Network state is now fully encapsulated in OLED_Mode_Network.cpp via OLEDScrollState
//  + pushed sub-modes; no externs needed.)

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

// displayIMUActions() moved to i2csensor_bno055.cpp (modular OLED mode)

// displayToFData() moved to i2csensor_vl53l4cx.cpp (modular OLED mode)

// displayAPDSData() moved to i2csensor_apds9960.cpp (modular OLED mode)
// displayConnectedSensors() moved to OLED_Mode_Sensors.cpp

// Forward declaration for gamepad input processing
bool processOLEDInput();
void tryAutoStartInputForMenu();

void updateOLEDDisplay() {
  // animationLastUpdate, animationFrame, animationFPS are now defined at top of file
  extern void displayAnimation();
  
  if (!gOledEnabled || !oledConnected || oledDisplay == nullptr) {
    return;
  }

  // Skip normal rendering during First-Time Setup - FTS screens render directly
  extern volatile FirstTimeSetupState gFirstTimeSetupState;
  if (gFirstTimeSetupState == SETUP_IN_PROGRESS) {
    return;
  }

  // AUTHENTICATION ENFORCEMENT: Force login screen if auth is required and user is not authenticated
  if (shouldBlockForDisplayAuth()) {
    // User must be on login screen - force mode change if they somehow got to another mode
    if (currentOLEDMode != OLED_LOGIN) {
      requestOLEDMode(OLED_LOGIN, "auth.guard.update", false);
    }
  }

  // Process gamepad input for menu navigation (runs every frame, handles its own debouncing)
  bool inputProcessed = processOLEDInput();
  if (inputProcessed) {
    oledMarkDirty();
  }

  unsigned long now = millis();
  
  // Force immediate refresh on mode change (don't wait for timer)
  bool modeChanged = (currentOLEDMode != lastRenderedMode);

  // Modes that have internal animations and need continuous rendering
  bool isAnimatedMode = (currentOLEDMode == OLED_ANIMATION || 
                         currentOLEDMode == OLED_LOGO || 
                         currentOLEDMode == OLED_BOOT_SENSORS);
  
  if (isAnimatedMode) {
    // Animated modes use animationFPS (default 10 FPS, configurable via 'animation fps' command)
    unsigned long animInterval = 1000 / animationFPS;
    if (now - animationLastUpdate >= animInterval) {
      animationLastUpdate = now;
      if (currentOLEDMode == OLED_ANIMATION) animationFrame++;
    } else if (!modeChanged) {
      return;
    }
  } else {
    // Timer-based throttle: check at most every updateInterval ms
    unsigned long updateInterval = (gSettings.oledUpdateInterval > 0) ? (unsigned long)gSettings.oledUpdateInterval : 125;
    if (now - oledLastUpdate < updateInterval) {
      return;  // Not time to check yet
    }
    
    // Skip render if nothing changed (uses seq + sensorStatusSeq)
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
      DEBUG_DISPLAYF("[OLED] Skipping render - I2C device marked DEGRADED");
    }
    return;
  }

  // Pre-gather data OUTSIDE I2C transaction to avoid blocking gamepad
  switch (currentOLEDMode) {
    case OLED_FILE_BROWSER:
      prepareFileBrowserData();
      break;
    case OLED_NETWORK_INFO:
    case OLED_NETWORK_STATUS:
      // Both main menu and Status sub-mode read networkRenderData (RSSI/SSID/IP).
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
#if ENABLE_AUTOMATION
    case OLED_AUTOMATIONS:
      prepareAutomationData();
      break;
#endif
    default:
      break;
  }

    // All framebuffer drawing below is CPU-side RAM operations (no I2C needed).
    // The I2C bus is only acquired by displayUpdate() at the end to push the framebuffer.
    // This prevents the OLED from monopolizing the I2C bus during the entire render cycle.
    {
    // Clear display areas separately
    // Animations handle their own full-screen clear since they don't use header/footer
    if (currentOLEDMode == OLED_ANIMATION) {
      gDisplay->clearDisplay();
    } else {
      // Clear all areas: header (0-9), content (10-53), footer (54-63)
      gDisplay->fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, DISPLAY_COLOR_BLACK);
    }
    gDisplay->setTextSize(1);
    gDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    
    // Determine if header should be shown (drawn after content, like footer)
    bool showHeader = (currentOLEDMode != OLED_ANIMATION && 
                       currentOLEDMode != OLED_LOGO &&
                       currentOLEDMode != OLED_OFF &&
                       !oledKeyboardIsActive());
    
    // Set cursor to content area start
    gDisplay->setCursor(0, showHeader ? OLED_CONTENT_START_Y : 0);

    // DEBUG: Track render for black flash investigation
    static unsigned long renderCount = 0;
    renderCount++;
    bool contentDrawn = true;  // Assume true, set false if mode doesn't draw
    
    // Log every 50th render or if mode changes to help track black flash
    static OLEDMode lastLoggedMode = OLED_OFF;
    if (currentOLEDMode != lastLoggedMode || (renderCount % 50) == 0) {
      DEBUG_DISPLAYF("[OLED_RENDER] mode=%d render#%lu\n", (int)currentOLEDMode, renderCount);
      lastLoggedMode = currentOLEDMode;
    }

    switch (currentOLEDMode) {
      // OLED_MENU handled by registered mode in OLED_Mode_Menu.cpp
      // OLED_SENSOR_MENU handled by registered mode in OLED_Mode_Menu.cpp
      // OLED_SYSTEM_STATUS handled by registered mode in OLED_Mode_System.cpp
      // OLED_SENSOR_DATA handled by registered mode in OLED_Mode_Sensors.cpp
      // OLED_SENSOR_LIST / OLED_BOOT_SENSORS handled by registered mode in OLED_Mode_Sensors.cpp
      // OLED_THERMAL_VISUAL handled by registered mode in i2csensor_mlx90640.cpp
      // OLED_NETWORK_INFO handled by registered mode in OLED_Mode_Network.cpp
      // OLED_MESH_STATUS handled by registered mode in OLED_Mode_Network.cpp
      // OLED_CUSTOM_TEXT handled by registered mode in OLED_Mode_System.cpp
      // OLED_UNAVAILABLE handled by registered mode in OLED_Mode_System.cpp

      // OLED_LOGO handled by registered mode in OLED_Mode_Menu.cpp
      // OLED_ANIMATION handled by registered mode in OLED_Mode_Animations.cpp
      // OLED_IMU_ACTIONS handled by registered mode in i2csensor_bno055.cpp
      // OLED_GPS_DATA handled by registered mode in i2csensor_pa1010d.cpp
      // OLED_FM_RADIO handled by registered mode in fm_radio.cpp
      // OLED_FILE_BROWSER handled by registered mode in OLED_Mode_FileBrowser.cpp

#if !ENABLE_AUTOMATION
      case OLED_AUTOMATIONS:
        enterUnavailablePage("Automations", "Not compiled");
        break;
#endif
      // When ENABLE_AUTOMATION is set, OLED_AUTOMATIONS is handled by registered mode in OLED_Mode_Automations.cpp

      // OLED_ESPNOW handled by registered mode in OLED_Mode_Network.cpp
      // OLED_TOF_DATA handled by registered mode in i2csensor_vl53l4cx.cpp
      // OLED_APDS_DATA handled by registered mode in i2csensor_apds9960_oled.h
      // OLED_POWER / OLED_POWER_CPU / OLED_POWER_SLEEP handled by registered mode in OLED_Mode_Power.cpp
      // OLED_MEMORY_STATS handled by registered mode in OLED_Mode_System.cpp
      // OLED_WEB_STATS handled by registered mode in OLED_Mode_Network.cpp
      // OLED_REMOTE handled by registered mode in OLED_Mode_Remote.cpp
      // OLED_NOTIFICATIONS handled by registered mode in OLED_Utils.cpp

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
            DEBUG_DISPLAYF("[OLED_RENDER_FAIL] Mode %d not found! render#%lu registeredMode=%p\n", (int)currentOLEDMode, renderCount, (void*)registeredMode);
          }
        }
        break;
    }

    // Failsafe: if no content was drawn, draw an error message so screen isn't black
    if (!contentDrawn) {
      DEBUG_DISPLAYF("[OLED_BLACK_FLASH] No content drawn! mode=%d render#%lu\n", (int)currentOLEDMode, renderCount);
      gDisplay->setCursor(0, 20);
      gDisplay->print("Mode ");
      gDisplay->print((int)currentOLEDMode);
      gDisplay->print(" no render");
    }

    oledConfirmRender();

    // Draw persistent header bar (after content, so it's always on top - same pattern as footer)
    if (showHeader) {
      oledRenderHeader(gDisplay, nullptr);
    }

    // Draw persistent footer with button hints (always last, in same frame)
    drawOLEDFooter();
    
    // Draw UI overlays (toast, dialog, progress, list) on top of everything
    oledUIRender();

    // Mark mode as clean after successful render
    oledClearDirty();

    displayUpdate();
    }
}

// OLED Settings Commands (migrated from .ino)
// ============================================================================

const char* cmd_oled_enabled(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: oledenabled <0|1>";
  const char* p = _arg.c_str();
  bool enabled = (*p == '1' || strncasecmp(p, "true", 4) == 0);
  setSetting(gSettings.oledEnabled, enabled);

  if (gSettings.oledEnabled) {
    if (!oledConnected) {
      if (initOLEDDisplay()) {
        broadcastOutput("OLED display started");
      } else {
        broadcastOutput("Failed to initialize OLED display. Check wiring.");
        return "ERROR";
      }
    } else {
      gOledEnabled = true;
    }

    String defaultMode = gSettings.oledDefaultMode;
    defaultMode.toLowerCase();
    OLEDMode prevMode = currentOLEDMode;
    OLEDMode defMode = modeFromSlug(defaultMode);
    if ((int)defMode == -1) defMode = OLED_SYSTEM_STATUS;
    requestOLEDMode(defMode, "cmd.oledenabled.forceDefault", false);

    { char dbgBuf[48]; snprintf(dbgBuf, sizeof(dbgBuf), "defaultMode=%s", defaultMode.c_str()); debugOLEDModeChange("cmd.oledenabled.forceDefault", prevMode, currentOLEDMode, dbgBuf); }

    updateOLEDDisplay();
    snprintf(getDebugBuffer(), 1024, "OLED display enabled (mode: %s)", gSettings.oledDefaultMode.c_str());
  } else {
    if (oledConnected) {
      gOledEnabled = false;
      i2cOledTransactionVoid(400000, 500, [&]() {
        oledDisplay->clearDisplay();
        oledDisplay->display();
      });
    }
    snprintf(getDebugBuffer(), 1024, "OLED display disabled");
  }
  return getDebugBuffer();
}

const char* cmd_oled_requireauth(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: oledrequireauth <0|1>";
  const char* p = _arg.c_str();
  bool enabled = (*p == '1' || strncasecmp(p, "true", 4) == 0);
  setSetting(gSettings.localDisplayRequireAuth, enabled);
  snprintf(getDebugBuffer(), 1024, "Local display require auth %s", enabled ? "enabled" : "disabled");
  return getDebugBuffer();
}

const char* cmd_oled_bootmode(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: oledbootmode <logo|status|sensors|thermal|network|mesh|off>";
  const char* p = _arg.c_str();
  // Case-insensitive compare for mode names
  if (strncasecmp(p, "logo", 4) == 0) {
    setSetting(gSettings.oledBootMode, "logo");
  } else if (strncasecmp(p, "status", 6) == 0) {
    setSetting(gSettings.oledBootMode, "status");
  } else if (strncasecmp(p, "sensors", 7) == 0) {
    setSetting(gSettings.oledBootMode, "sensors");
  } else if (strncasecmp(p, "thermal", 7) == 0) {
    setSetting(gSettings.oledBootMode, "thermal");
  } else if (strncasecmp(p, "network", 7) == 0) {
    setSetting(gSettings.oledBootMode, "network");
  } else if (strncasecmp(p, "mesh", 4) == 0) {
    setSetting(gSettings.oledBootMode, "mesh");
  } else if (strncasecmp(p, "off", 3) == 0) {
    setSetting(gSettings.oledBootMode, "off");
  } else {
    return "Error: OLED boot mode must be logo|status|sensors|thermal|network|mesh|off";
  }
  snprintf(getDebugBuffer(), 1024, "OLED boot mode set to %s", gSettings.oledBootMode.c_str());
  return getDebugBuffer();
}

const char* cmd_oled_defaultmode(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: oleddefaultmode <logo|status|sensors|thermal|network|mesh|off>";
  const char* p = _arg.c_str();
  // Case-insensitive compare for mode names
  if (strncasecmp(p, "logo", 4) == 0) {
    setSetting(gSettings.oledDefaultMode, "logo");
  } else if (strncasecmp(p, "status", 6) == 0) {
    setSetting(gSettings.oledDefaultMode, "status");
  } else if (strncasecmp(p, "sensors", 7) == 0) {
    setSetting(gSettings.oledDefaultMode, "sensors");
  } else if (strncasecmp(p, "thermal", 7) == 0) {
    setSetting(gSettings.oledDefaultMode, "thermal");
  } else if (strncasecmp(p, "network", 7) == 0) {
    setSetting(gSettings.oledDefaultMode, "network");
  } else if (strncasecmp(p, "mesh", 4) == 0) {
    setSetting(gSettings.oledDefaultMode, "mesh");
  } else if (strncasecmp(p, "off", 3) == 0) {
    setSetting(gSettings.oledDefaultMode, "off");
  } else {
    return "Error: OLED default mode must be logo|status|sensors|thermal|network|mesh|off";
  }
  snprintf(getDebugBuffer(), 1024, "OLED default mode set to %s", gSettings.oledDefaultMode.c_str());
  return getDebugBuffer();
}

const char* cmd_oled_bootduration(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: oledbootduration <500..10000>";
  const char* p = _arg.c_str();
  int v = atoi(p);
  if (v < 500 || v > 10000) return "Error: OLED boot duration must be 500..10000 ms (0.5s..10s)";
  setSetting(gSettings.oledBootDuration, v);
  snprintf(getDebugBuffer(), 1024, "OLED boot duration set to %dms", v);
  return getDebugBuffer();
}

const char* cmd_oled_updateinterval(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: oledupdateinterval <10..1000>";
  const char* p = _arg.c_str();
  int v = atoi(p);
  if (v < 10 || v > 1000) return "Error: OLED update interval must be 10..1000 ms";
  setSetting(gSettings.oledUpdateInterval, v);
  snprintf(getDebugBuffer(), 1024, "OLED update interval set to %dms (applies on next update)", v);
  return getDebugBuffer();
}

const char* cmd_oled_brightness(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: oledbrightness <0..255>";
  const char* p = _arg.c_str();
  int v = atoi(p);
  if (v < 0 || v > 255) return "Error: OLED brightness must be 0..255";
  setSetting(gSettings.oledBrightness, (int)v);
  snprintf(getDebugBuffer(), 1024, "OLED brightness set to %d", v);
  return getDebugBuffer();
}

// Flip the OLED 180° (and live-apply by re-rotating the active display).
// Persisted via oledFlipped so the same orientation comes back on the next boot.
const char* cmd_oled_flip(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim(); arg.toLowerCase();
  if (arg.length() == 0) {
    snprintf(getDebugBuffer(), 1024, "OLED flip: %s", gSettings.oledFlipped ? "on (rotated 180°)" : "off (normal)");
    return getDebugBuffer();
  }
  bool target;
  if (arg == "on" || arg == "true" || arg == "1")       target = true;
  else if (arg == "off" || arg == "false" || arg == "0") target = false;
  else if (arg == "toggle")                              target = !gSettings.oledFlipped;
  else return "Error: invalid arguments — Usage: oledflip [on|off|toggle]";
  setSetting(gSettings.oledFlipped, target);
  applyOLEDRotation();
  snprintf(getDebugBuffer(), 1024, "OLED flip: %s", target ? "on (rotated 180°)" : "off (normal)");
  return getDebugBuffer();
}

const char* cmd_oled_thermalscale(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: oledthermalscale <0.1..10.0>";
  const char* p = _arg.c_str();
  float f = strtof(p, nullptr);
  if (f < 0.1 || f > 10.0) return "Error: OLED thermal scale must be 0.1..10.0";
  setSetting(gSettings.oledThermalScale, f);
  snprintf(getDebugBuffer(), 1024, "OLED thermal scale set to %.2f", f);
  return getDebugBuffer();
}

const char* cmd_oled_thermalcolormode(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: oledthermalcolormode <3level|grayscale>";
  const char* p = _arg.c_str();
  if (strncasecmp(p, "3level", 6) == 0) {
    setSetting(gSettings.oledThermalColorMode, "3level");
  } else if (strncasecmp(p, "grayscale", 9) == 0) {
    setSetting(gSettings.oledThermalColorMode, "grayscale");
  } else {
    return "Error: OLED thermal color mode must be 3level|grayscale";
  }
  snprintf(getDebugBuffer(), 1024, "OLED thermal color mode set to %s", gSettings.oledThermalColorMode.c_str());
  return getDebugBuffer();
}

// ============================================================================
// OLED Display Command Handlers  
// ============================================================================

const char* cmd_oledstart(const String& argsInput) {
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

const char* cmd_oledstop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!oledConnected) {
    broadcastOutput("OLED display not running");
    return "OK";
  }

  stopOLEDDisplay();
  broadcastOutput("OLED display stopped");
  return "OK";
}

const char* cmd_oledmode(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!oledConnected) {
    broadcastOutput("OLED display not running. Use 'oledstart' first.");
    return "ERROR";
  }

  String mode = argsInput;
  mode.trim();
  
  if (mode.length() == 0) {
    broadcastOutput("Usage: oledmode <menu|status|sensordata|sensorlist|thermal|network|mesh|gps|text|logo|anim|imuactions|fmradio|files|automations|espnow|memory|off>");
    return "ERROR";
  }

  mode.toLowerCase();

  // If boot sequence is still running, mark that user overrode it
  if (oledBootModeActive) {
    userOverrodeBootMode = true;
    DEBUG_DISPLAYF("[OLED_MODE] User overrode boot sequence - will not auto-transition");
  }

  // Slug -> enum lookup (validates the slug in one place).
  OLEDMode target = modeFromSlug(mode);
  if ((int)target == -1) {
    broadcastOutput("Invalid mode. Options: menu, status, sensordata, sensorlist, gamepad, "
                    "thermal, network, gps, text, logo, anim, imuactions, fmradio, files, "
                    "automations, espnow, memory, off");
    return "ERROR";
  }

  // Single authoritative transition — auth gating + debug log handled inside.
  // CLI transitions do not push to the back-nav stack (explicit user destination).
  requestOLEDMode(target, "cli.oledmode", false);

  // Per-mode initialisation side-effects (state resets, hardware inits).
  switch (target) {
    case OLED_MENU:
      // State reset (resetOLEDMenu) now runs via OLED_MENU's onEnterFunc. This
      // path keeps only the gamepad auto-start, which must run on the cmd_exec
      // task — not inside requestOLEDMode(), which the input pump also calls.
      tryAutoStartInputForMenu();
      break;
    case OLED_OFF:
      i2cOledTransactionVoid(400000, 500, [&]() {
        oledDisplay->clearDisplay();
        oledDisplay->display();
      });
      break;
    default:
      // OLED_ANIMATION / OLED_FILE_BROWSER / OLED_ESPNOW entry resets now run via
      // their per-mode onEnterFunc (see each mode's registration) — no longer
      // duplicated here or in the menu-select path.
      break;
  }

  if (ensureDebugBuffer()) {
    snprintf(getDebugBuffer(), 1024, "OLED mode: %s", getOLEDModeName(target));
    broadcastOutput(getDebugBuffer());
  }
  updateOLEDDisplay();
  return "OK";
}

const char* cmd_oledtext(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!oledConnected) {
    broadcastOutput("OLED display not running. Use 'oledstart' first.");
    return "ERROR";
  }

  String text = argsInput;
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
  requestOLEDMode(OLED_CUSTOM_TEXT, "cli.customtext", false);

  if (ensureDebugBuffer()) {
    snprintf(getDebugBuffer(), 1024, "Custom text set: %s", text.c_str());
    broadcastOutput(getDebugBuffer());
  }
  updateOLEDDisplay();
  return "OK";
}

const char* cmd_oledclear(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!oledConnected) {
    broadcastOutput("OLED display not running. Use 'oledstart' first.");
    return "ERROR";
  }

  i2cOledTransactionVoid(400000, 500, [&]() {
    oledDisplay->clearDisplay();
    oledDisplay->display();
  });

  broadcastOutput("OLED display cleared");
  return "OK";
}

const char* cmd_oledstatus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["connected"] = oledConnected;
    if (oledConnected) {
      doc["address"] = OLED_I2C_ADDRESS;
      doc["width"]   = SCREEN_WIDTH;
      doc["height"]  = SCREEN_HEIGHT;
      doc["enabled"] = gOledEnabled;
      const char* modeStr;
      switch (currentOLEDMode) {
        case OLED_SYSTEM_STATUS:  modeStr = "System Status"; break;
        case OLED_SENSOR_DATA:    modeStr = "Sensor Data"; break;
        case OLED_SENSOR_LIST:    modeStr = "Sensor List"; break;
        case OLED_THERMAL_VISUAL: modeStr = "Thermal Visual"; break;
        case OLED_GAMEPAD_VISUAL: modeStr = "Gamepad Visual"; break;
        case OLED_NETWORK_INFO:   modeStr = "Network Info"; break;
        case OLED_MESH_STATUS:    modeStr = "Mesh Status"; break;
        case OLED_CUSTOM_TEXT:    modeStr = "Custom Text"; break;
        case OLED_LOGO:           modeStr = "Logo"; break;
        case OLED_ANIMATION:      modeStr = "Animation"; break;
        case OLED_FILE_BROWSER:   modeStr = "File Browser"; break;
        case OLED_OFF:            modeStr = "Off"; break;
        default:                  modeStr = "Unknown"; break;
      }
      doc["mode"] = modeStr;
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

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
    snprintf(getDebugBuffer(), 1024, "Enabled: %s", gOledEnabled ? "Yes" : "No");
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

const char* cmd_oledanim(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!oledConnected) {
    broadcastOutput("OLED display not running. Use 'oledstart' first.");
    return "ERROR";
  }

  String arg = argsInput;
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
      requestOLEDMode(OLED_ANIMATION, "cli.animation", false);
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

// Mode name lookup - used for header display (human-readable names)
static const char* getOLEDModeName(OLEDMode mode) {
  switch (mode) {
    case OLED_OFF: return "Off";
    case OLED_MENU: return "Menu";
    case OLED_SENSOR_MENU: return "Sensors";
    case OLED_SYSTEM_STATUS: return "Status";
    case OLED_SENSOR_DATA: return "Sensors";
    case OLED_SENSOR_LIST: return "Devices";
    case OLED_THERMAL_VISUAL: return "Thermal";
    case OLED_NETWORK_INFO: return "Network";
    case OLED_NETWORK_STATUS: return "Status";
    case OLED_NETWORK_WIFI_MENU: return "WiFi";
    case OLED_NETWORK_WIFI_LIST: return "Saved";
    case OLED_NETWORK_WIFI_REMOVE: return "Remove";
    case OLED_NETWORK_WIFI_SCAN: return "Scan";
    case OLED_MESH_STATUS: return "Mesh";
    case OLED_CUSTOM_TEXT: return "Text";
    case OLED_UNAVAILABLE: return "Unavail";
    case OLED_LOGO: return "Logo";
    case OLED_ANIMATION: return "Anim";
    case OLED_BOOT_SENSORS: return "Boot";
    case OLED_IMU_ACTIONS: return "IMU";
    case OLED_GPS_DATA: return "GPS";
    case OLED_FM_RADIO: return "FM Radio";
    case OLED_FILE_BROWSER: return "Files";
    case OLED_AUTOMATIONS: return "Automations";
    case OLED_ESPNOW: return "ESP-NOW";
    case OLED_TOF_DATA: return "ToF";
    case OLED_APDS_DATA: return "APDS";
    case OLED_POWER: return "Power";
    case OLED_POWER_CPU: return "CPU Power";
    case OLED_POWER_SLEEP: return "Sleep";
    case OLED_GAMEPAD_VISUAL: return "Gamepad";
    case OLED_BLUETOOTH: return "Bluetooth";
    case OLED_BLUETOOTH_STATUS: return "BT Status";
    case OLED_BLUETOOTH_G2: return "G2";
    case OLED_BLUETOOTH_G2_STATUS: return "G2 Status";
    case OLED_REMOTE_SENSORS: return "Remote";
    case OLED_MEMORY_STATS: return "Memory";
    case OLED_WEB_STATS: return "Web Stats";
    case OLED_RTC_DATA: return "RTC";
    case OLED_PRESENCE_DATA: return "Presence";
    case OLED_REMOTE: return "Bond";
    case OLED_UNIFIED_MENU: return "Actions";
    case OLED_NOTIFICATIONS: return "Notifs";
    case OLED_SET_PATTERN: return "Pattern";
    case OLED_LOGIN: return "Login";
    case OLED_LOGOUT: return "Logout";
    case OLED_QUICK_SETTINGS: return "Quick Settings";
    case OLED_SPEECH: return "Speech";
    case OLED_SPEECH_STATUS: return "SR Status";
    case OLED_MICROPHONE: return "Mic";
    case OLED_GPS_MAP: return "Map";
    case OLED_SETTINGS: return "Settings";
    case OLED_CLI_VIEWER: return "CLI";
    case OLED_CLI_INPUT:  return "CLI Input";
    case OLED_LOGGING: return "Logging";
    case OLED_REMOTE_SETTINGS: return "Remote Set";
    default: return "Unknown";
  }
}

// modeFromSlug: canonical CLI slug -> OLEDMode (all recognised aliases included).
// Returns (OLEDMode)-1 for unknown/invalid slugs.
OLEDMode modeFromSlug(const String& slug) {
  if (slug == "off") return OLED_OFF;
  if (slug == "menu") return OLED_MENU;
  if (slug == "status") return OLED_SYSTEM_STATUS;
  if (slug == "sensordata" || slug == "sensors") return OLED_SENSOR_DATA;
  if (slug == "sensorlist") return OLED_SENSOR_LIST;
  if (slug == "thermal") return OLED_THERMAL_VISUAL;
  if (slug == "network") return OLED_NETWORK_INFO;
  if (slug == "mesh") return OLED_MESH_STATUS;
  if (slug == "text") return OLED_CUSTOM_TEXT;
  if (slug == "logo") return OLED_LOGO;
  if (slug == "anim" || slug == "animation") return OLED_ANIMATION;
  if (slug == "imuactions" || slug == "imu" || slug == "actions") return OLED_IMU_ACTIONS;
  if (slug == "gps") return OLED_GPS_DATA;
  if (slug == "fmradio") return OLED_FM_RADIO;
  if (slug == "files" || slug == "filebrowser" || slug == "fb") return OLED_FILE_BROWSER;
  if (slug == "automations" || slug == "auto") return OLED_AUTOMATIONS;
  if (slug == "espnow") return OLED_ESPNOW;
  if (slug == "tof") return OLED_TOF_DATA;
  if (slug == "apds") return OLED_APDS_DATA;
  if (slug == "power") return OLED_POWER;
  if (slug == "gamepad" || slug == "gpad") return OLED_GAMEPAD_VISUAL;
  if (slug == "bluetooth") return OLED_BLUETOOTH;
  if (slug == "remote") return OLED_REMOTE_SENSORS;
  if (slug == "memory" || slug == "mem") return OLED_MEMORY_STATS;
  if (slug == "web") return OLED_WEB_STATS;
  if (slug == "rtc") return OLED_RTC_DATA;
  if (slug == "presence") return OLED_PRESENCE_DATA;
  if (slug == "unified") return OLED_UNIFIED_MENU;
  if (slug == "notifs" || slug == "notifications") return OLED_NOTIFICATIONS;
  if (slug == "map" || slug == "gpsmap") return OLED_GPS_MAP;
  if (slug == "login") return OLED_LOGIN;
  if (slug == "settings") return OLED_SETTINGS;
  return (OLEDMode)-1;
}

// slugFromMode: OLEDMode -> primary CLI slug (round-trips with modeFromSlug).
const char* slugFromMode(OLEDMode mode) {
  switch (mode) {
    case OLED_OFF:             return "off";
    case OLED_MENU:            return "menu";
    case OLED_SYSTEM_STATUS:   return "status";
    case OLED_SENSOR_DATA:     return "sensordata";
    case OLED_SENSOR_LIST:     return "sensorlist";
    case OLED_THERMAL_VISUAL:  return "thermal";
    case OLED_NETWORK_INFO:    return "network";
    case OLED_NETWORK_STATUS:  return "networkstatus";
    case OLED_NETWORK_WIFI_MENU: return "wifimenu";
    case OLED_NETWORK_WIFI_LIST: return "wifilist_oled";
    case OLED_NETWORK_WIFI_REMOVE: return "wifiremove_oled";
    case OLED_NETWORK_WIFI_SCAN: return "wifiscan_oled";
    case OLED_MESH_STATUS:     return "mesh";
    case OLED_CUSTOM_TEXT:     return "text";
    case OLED_LOGO:            return "logo";
    case OLED_ANIMATION:       return "anim";
    case OLED_BOOT_SENSORS:    return "boot";
    case OLED_IMU_ACTIONS:     return "imuactions";
    case OLED_GPS_DATA:        return "gps";
    case OLED_FM_RADIO:        return "fmradio";
    case OLED_FILE_BROWSER:    return "files";
    case OLED_AUTOMATIONS:     return "automations";
    case OLED_ESPNOW:          return "espnow";
    case OLED_TOF_DATA:        return "tof";
    case OLED_APDS_DATA:       return "apds";
    case OLED_POWER:           return "power";
    case OLED_POWER_CPU:       return "powercpu";
    case OLED_POWER_SLEEP:     return "powersleep";
    case OLED_GAMEPAD_VISUAL:  return "gamepad";
    case OLED_BLUETOOTH:       return "bluetooth";
    case OLED_BLUETOOTH_STATUS: return "btstatus";
    case OLED_BLUETOOTH_G2:     return "g2";
    case OLED_BLUETOOTH_G2_STATUS: return "g2status";
    case OLED_REMOTE_SENSORS:  return "remote";
    case OLED_MEMORY_STATS:    return "memory";
    case OLED_WEB_STATS:       return "web";
    case OLED_RTC_DATA:        return "rtc";
    case OLED_PRESENCE_DATA:   return "presence";
    case OLED_UNIFIED_MENU:    return "actions";
    case OLED_NOTIFICATIONS:   return "notifs";
    case OLED_GPS_MAP:         return "map";
    case OLED_LOGIN:           return "login";
    case OLED_LOGOUT:          return "logout";
    case OLED_QUICK_SETTINGS:  return "quicksettings";
    case OLED_SETTINGS:        return "settings";
    case OLED_REMOTE_SETTINGS: return "remotesettings";
    case OLED_SET_PATTERN:     return "setpattern";
    case OLED_CHANGE_PASSWORD: return "changepass";
    case OLED_REMOTE:          return "remoteui";
    case OLED_SPEECH:          return "speech";
    case OLED_SPEECH_STATUS:   return "speechstatus";
    case OLED_MICROPHONE:      return "mic";
    case OLED_CLI_VIEWER:      return "cli";
    case OLED_CLI_INPUT:       return "cliinput";
    case OLED_LOGGING:         return "logging";
    case OLED_SENSOR_MENU:     return "sensormenu";
    case OLED_UNAVAILABLE:     return "unavail";
    default:                   return "unknown";
  }
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

// Category menu state (non-static for access from OLED_Mode_Menu.cpp)
int oledMenuCategorySelected = -1;  // -1 = showing categories, 0-5 = in category submenu
int oledMenuCategoryItemIndex = 0;  // Selected item within category

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
    DEBUG_DISPLAYF("OLED init skipped - I2C bus disabled");
    oledConnected = false;
    gOledEnabled = false;
    return false;
  }

  bool inFirstTimeSetup = (gFirstTimeSetupState != SETUP_NOT_NEEDED);
  DEBUG_DISPLAYF("[OLED_INIT] fts=%d settings.oledEnabled=%d\n", inFirstTimeSetup ? 1 : 0,
                gSettings.oledEnabled ? 1 : 0);
  if (!inFirstTimeSetup) {
    if (!gSettings.oledEnabled) {
      oledConnected = false;
      gOledEnabled = false;
      return false;
    }
  }
  
  // Resolve OLED's bus from settings (default 0 = I2C1 = Wire1). Same
  // hard-fail-on-unavailable contract as HAL_Display.cpp's displayInit.
  const uint8_t oledBus = (uint8_t)gSettings.oledBus;
  TwoWire* oledWire = i2c() ? i2c()->getWire(oledBus) : nullptr;
  if (!oledWire) {
    DEBUG_DISPLAYF("OLED bus %u not initialized — skip boot-anim OLED init", oledBus);
    return false;
  }
  const int oledSda = (oledBus == 0) ? gSettings.i2cSdaPin  : gSettings.i2c2SdaPin;
  const int oledScl = (oledBus == 0) ? gSettings.i2cSclPin  : gSettings.i2c2SclPin;

  // Try both common OLED addresses: 0x3D (default) and 0x3C (alternate).
  // Retry a few times — cold boot / shared I2C bus can NACK the first probe intermittently.
  uint8_t oledAddresses[] = {0x3D, 0x3C};
  uint8_t detectedAddr = 0;
  constexpr int kOledProbeAttempts = 3;
  for (int attempt = 0; attempt < kOledProbeAttempts && detectedAddr == 0; attempt++) {
    if (attempt > 0) {
      delay(50);
      DEBUG_DISPLAYF("OLED probe retry %d/%d", attempt + 1, kOledProbeAttempts);
    }
    for (uint8_t addr : oledAddresses) {
      DEBUG_DISPLAYF("Probing for OLED at 0x%02X on bus %u (SDA=%d, SCL=%d)",
                     addr, oledBus, oledSda, oledScl);
      uint8_t probeResult = i2cProbeAddress(addr, 100000, 200, oledBus);
      DEBUG_DISPLAYF("OLED probe at 0x%02X result: %d (0=found, 2=NACK)", addr, probeResult);
      if (probeResult == 0) {
        detectedAddr = addr;
        break;
      }
    }
  }

  if (detectedAddr != 0) {
    DEBUG_DISPLAYF("OLED detected at 0x%02X on bus %u - initializing for boot animation",
                   detectedAddr, oledBus);

    // Use Display HAL's gDisplay (oledDisplay is a macro alias for gDisplay in Display_HAL.h).
    // Construct with the OLED's resolved Wire pointer; all later library
    // calls (clearDisplay, display, etc.) route through it automatically.
    extern DisplayDriver* gDisplay;
    if (!gDisplay) {
      gDisplay = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, oledWire, OLED_RESET);
    }

    bool beginOk = gDisplay && i2cDeviceTransaction(oledBus, detectedAddr, 100000, 500, [&]() -> bool {
      return gDisplay->begin(SSD1306_SWITCHCAPVCC, detectedAddr);
    });
    if (beginOk) {
      oledConnected = true;
      gOledEnabled = true;

      // Set rotation (0 = normal, 2 = 180 degrees) — persisted via oledFlipped
      oledDisplay->setRotation(gSettings.oledFlipped ? 2 : 0);

      // Set up the input abstraction layer NOW (not lazily from
      // initOLEDDisplay later) so button mappings are live for any pre-WiFi
      // OLED mode that reads INPUT_CHECK. The static initializer in
      // HAL_Input.cpp already derives from INPUT_TYPE; this is a no-op
      // resync in case the controller type was changed at runtime earlier.
      inputAbstractionInit();

      // Start boot animation immediately
      currentBootPhase = BOOT_PHASE_ANIMATION;
      bootPhaseStartTime = millis();
      oledBootModeActive = true;

      requestOLEDMode(OLED_ANIMATION, "boot.init", false);
      currentAnimation = ANIM_BOOT_PROGRESS;
      animationFrame = 0;
      animationLastUpdate = millis();

      // Initialize boot progress
      bootProgressPercent = 0;
      bootProgressLabel = "Initializing...";

      // Clear display and render first animation frame (I2C-safe)
      i2cOledTransactionVoid(400000, 500, [&]() {
        oledDisplay->clearDisplay();
        displayAnimation();
        oledDisplay->display();
      });

      DEBUG_DISPLAYF("OLED boot animation started at 0x%02X", detectedAddr);
      logSystemEvent("DISPLAY", "OLED online at 0x%02X (bus %u)", detectedAddr, oledBus);
      return true;
    }
  }
  
  DEBUG_DISPLAYF("OLED not detected or initialization failed");
  // Genuine "configured but didn't come up" divergence only when OLED is actually
  // enabled in settings. During first-time setup the enabled gate above is bypassed,
  // so a board with no OLED attached reaches here with oledEnabled=false — don't
  // write a false hardware-failure record into a fresh device's very first log.
  if (gSettings.oledEnabled) {
    logSystemEvent("DISPLAY", "OLED enabled but init FAILED at boot (not detected / begin() failed on bus %u)", oledBus);
  }
  return false;
}

// Process boot sequence phase transitions in loop()
// Call this from loop() when oledBootModeActive is true
void processOLEDBootSequence() {
  if (!oledBootModeActive || !oledConnected || !gOledEnabled) {
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
        requestOLEDMode(OLED_LOGO, "boot.animation->logo", false);
        debugOLEDModeChange("boot.phase.animation->logo", prevMode, currentOLEDMode, "");
        DEBUG_DISPLAYF("OLED boot sequence: Animation -> Logo");
      }
      break;

    case BOOT_PHASE_LOGO:
      // Show logo, then go directly to complete (sensor list is a normal menu mode now)
      if (elapsed >= LOGO_DURATION) {
        currentBootPhase = BOOT_PHASE_COMPLETE;
        oledBootModeActive = false;

        // Only transition if user hasn't manually changed mode during boot
        if (userOverrodeBootMode) {
          DEBUG_DISPLAYF("[OLED_MODE] boot.complete: User overrode boot, keeping mode %d\n", (int)currentOLEDMode);
          DEBUG_DISPLAYF("OLED boot sequence complete (user overrode, keeping current mode)");
        } else {
          OLEDMode prevMode = currentOLEDMode;
          
          // After boot completes, go to login screen if auth is required
          if (gSettings.localDisplayRequireAuth && !gLocalDisplayAuthed) {
            requestOLEDMode(OLED_LOGIN, "boot.login", false);
            // B-button from login falls back to OLED_MENU via empty-stack default in popOLEDMode().
            debugOLEDModeChange("boot.complete.login", prevMode, currentOLEDMode, "Auth required");
            DEBUG_DISPLAYF("OLED boot sequence: Logo -> Login (auth required)");
          } else {
            // No auth required or already authed - go to default mode
            String defaultMode = gSettings.oledDefaultMode;
            defaultMode.toLowerCase();
            
            // modeFromSlug resolves the saved slug; fallback to status if unrecognised.
            // B-button from any mode falls back to OLED_MENU via empty-stack default.
            OLEDMode defMode = modeFromSlug(defaultMode);
            if ((int)defMode == -1) defMode = OLED_SYSTEM_STATUS;
            requestOLEDMode(defMode, "boot.default", false);

            { char dbgBuf[48]; snprintf(dbgBuf, sizeof(dbgBuf), "defaultMode=%s", defaultMode.c_str()); debugOLEDModeChange("boot.complete.defaultMode", prevMode, currentOLEDMode, dbgBuf); }
            DEBUG_DISPLAYF("OLED boot sequence: Logo -> %s (complete, B returns to menu)", defaultMode.c_str());
          }
          
          // Auto-start gamepad if setting is enabled and I2C bus is enabled
          if (gSettings.inputAutoStart && gSettings.i2cBusEnabled) {
            tryAutoStartInputForMenu();
          }
        }
      }
      break;

    case BOOT_PHASE_SENSORS:
      // Legacy phase - no longer used, skip to complete
      currentBootPhase = BOOT_PHASE_COMPLETE;
      oledBootModeActive = false;
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

// ============================================================================
// Categorized Menu System
// ============================================================================

// Category menu items (top level)
// Columns: name, iconName, targetMode (used as category ID)
const OLEDMenuItem oledMenuCategories[] = {
  { "System",       "notify_system",     (OLEDMode)0 },  // Category ID 0
  { "Config",       "settings",          (OLEDMode)1 },  // Category ID 1
  { "Connect",      "notify_server",     (OLEDMode)2 },  // Category ID 2
  { "Hardware",     "notify_sensor",     (OLEDMode)3 },  // Category ID 3
  { "Tools",        "notify_automation", (OLEDMode)4 },  // Category ID 4
  { "Power",        "power",             (OLEDMode)5 },  // Category ID 5
};
const int oledMenuCategoryCount = sizeof(oledMenuCategories) / sizeof(oledMenuCategories[0]);

// System & Diagnostics category items
// Columns: name, iconName, targetMode
const OLEDMenuItem oledMenuCategory0[] = {
  { "Status",     "notify_system",     OLED_SYSTEM_STATUS },
  { "Memory",     "memory",            OLED_MEMORY_STATS },
  { "Notifs",     "notify_bell",       OLED_NOTIFICATIONS },
  { "CLI Output", "terminal",          OLED_CLI_VIEWER },
  { "CLI Input",  "terminal",          OLED_CLI_INPUT },
  { "Logging",    "file_text",         OLED_LOGGING },
};
const int oledMenuCategory0Count = sizeof(oledMenuCategory0) / sizeof(oledMenuCategory0[0]);

// Configuration category items
// Columns: name, iconName, targetMode
const OLEDMenuItem oledMenuCategory1[] = {
  { "Settings",   "settings",          OLED_SETTINGS },
  { "Login",      "user",              OLED_LOGIN },
  { "Logout",     "user",              OLED_LOGOUT },
  { "Change PW",  "password",          OLED_CHANGE_PASSWORD },
#if ENABLE_GAMEPAD_SENSOR
  { "Gamepad PW", "gamepad",           OLED_SET_PATTERN },
#endif
};
const int oledMenuCategory1Count = sizeof(oledMenuCategory1) / sizeof(oledMenuCategory1[0]);

// Connectivity category items
// Columns: name, iconName, targetMode
const OLEDMenuItem oledMenuCategory2[] = {
#if ENABLE_WIFI
  { "Network",    "notify_server",     OLED_NETWORK_INFO },
#endif
#if ENABLE_ESPNOW
  { "ESP-NOW",    "notify_espnow",     OLED_ESPNOW },
#endif
#if ENABLE_BLUETOOTH
  { "Bluetooth",  "bt_idle",           OLED_BLUETOOTH },
#endif
#if ENABLE_BONDED_MODE
  { "Bond",       "notify_espnow",     OLED_REMOTE },
#endif
#if ENABLE_HTTP_SERVER
  { "Web",        "web",               OLED_WEB_STATS },
#endif
};
const int oledMenuCategory2Count = sizeof(oledMenuCategory2) / sizeof(oledMenuCategory2[0]);

// Hardware & Sensors category items
// Columns: name, iconName, targetMode
const OLEDMenuItem oledMenuCategory3[] = {
#if ENABLE_I2C_SYSTEM || ENABLE_CAMERA_SENSOR || ENABLE_MICROPHONE_SENSOR
  { "Sensors",    "notify_sensor",     OLED_SENSOR_MENU },
#endif
#if ENABLE_MICROPHONE_SENSOR
  { "Microphone", "notify_sensor",     OLED_MICROPHONE },
#endif
#if ENABLE_ESP_SR
  { "Speech",     "notify_sensor",     OLED_SPEECH },
#endif
#if ENABLE_GPS_SENSOR && ENABLE_MAPS
  // Map needs BOTH: GPS hardware to know your position AND map software to
  // render tiles. Was an OR previously, so on builds with GPS on but maps
  // off the entry would appear in the hardware menu pointing at a mode
  // that no longer registers (gated at the file level in OLED_Mode_Map.cpp).
  { "Map",        "compass",           OLED_GPS_MAP },
#endif
};
const int oledMenuCategory3Count = sizeof(oledMenuCategory3) / sizeof(oledMenuCategory3[0]);

// Automation & Tools category items
// Columns: name, iconName, targetMode
const OLEDMenuItem oledMenuCategory4[] = {
#if ENABLE_AUTOMATION
  { "Automations","notify_automation", OLED_AUTOMATIONS },
#endif
  { "Files",      "notify_files",      OLED_FILE_BROWSER },
#if ENABLE_ONDEVICE_LLM
  // LLM Chat — visible unconditionally when compiled in. The mode itself
  // shows a model-picker when no model is loaded, so the entry is useful
  // even on a fresh boot before any model has been selected.
  { "LLM Chat",   "terminal",          OLED_LLM },
#endif
};
const int oledMenuCategory4Count = sizeof(oledMenuCategory4) / sizeof(oledMenuCategory4[0]);

// Power & Display category items
// Columns: name, iconName, targetMode
const OLEDMenuItem oledMenuCategory5[] = {
  { "Power",      "power",             OLED_POWER },
};
const int oledMenuCategory5Count = sizeof(oledMenuCategory5) / sizeof(oledMenuCategory5[0]);

// Sensor submenu items (extern const for external linkage)
// Columns: name, iconName, targetMode
extern const OLEDMenuItem oledSensorMenuItems[] = {
  { "Data",       "notify_sensor",     OLED_SENSOR_DATA },
  { "List",       "notify_sensor",     OLED_SENSOR_LIST },
#if ENABLE_THERMAL_SENSOR
  { "Thermal",    "thermal",           OLED_THERMAL_VISUAL },
#endif
#if ENABLE_TOF_SENSOR
  { "ToF",        "tof_radar",         OLED_TOF_DATA },
#endif
#if ENABLE_IMU_SENSOR
  { "IMU",        "imu_axes",          OLED_IMU_ACTIONS },
#endif
#if ENABLE_APDS_SENSOR
  { "APDS",       "gesture",           OLED_APDS_DATA },
#endif
#if ENABLE_GPS_SENSOR
  { "GPS",        "compass",           OLED_GPS_DATA },
#endif
#if ENABLE_GAMEPAD_SENSOR
  { "Gamepad",    "gamepad",           OLED_GAMEPAD_VISUAL },
#endif
#if ENABLE_FM_RADIO
  { "FM Radio",   "radio",             OLED_FM_RADIO },
#endif
#if ENABLE_RTC_SENSOR
  { "RTC",        "rtc",               OLED_RTC_DATA },
#endif
#if ENABLE_PRESENCE_SENSOR
  { "Presence",   "presence",          OLED_PRESENCE_DATA },
#endif
#if ENABLE_CAMERA_SENSOR
  { "Camera",     "notify_sensor",     OLED_SENSOR_DATA },
#endif
#if ENABLE_MICROPHONE_SENSOR
  { "Microphone", "notify_sensor",     OLED_MICROPHONE },
#endif
#if ENABLE_ESP_SR
  { "Speech",     "notify_sensor",     OLED_SPEECH },
#endif
};
extern const int oledSensorMenuItemCount = sizeof(oledSensorMenuItems) / sizeof(oledSensorMenuItems[0]);

// =============================================================================
// Dynamic Menu System (combines local + remote items based on DataSource)
// =============================================================================

EXT_RAM_BSS_ATTR OLEDMenuItemEx gDynamicMenuItems[MAX_DYNAMIC_MENU_ITEMS];
int gDynamicMenuItemCount = 0;
static bool gDynamicMenuBuilt = false;
static DataSource gLastBuildSource = DataSource::LOCAL;

// Submenu state for grouped remote items
static bool gInRemoteSubmenu = false;
static char gRemoteSubmenuId[16] = "";
EXT_RAM_BSS_ATTR static OLEDMenuItemEx gRemoteSubmenuItems[MAX_DYNAMIC_MENU_ITEMS];
static int gRemoteSubmenuItemCount = 0;
static int gRemoteSubmenuSelection = 0;

// Remote command input state (for commands that need parameters)
static bool gRemoteCommandInputActive = false;
static char gPendingRemoteCommand[64] = "";

// Start remote command input mode with keyboard.
// [[maybe_unused]]: its only live caller (the old oledMenuSelect remote-submenu
// path) is gone with the menu unification, but it's kept as part of the
// not-yet-wired remote-command subsystem (buildRemoteSubmenu et al.).
[[maybe_unused]] static void startRemoteCommandInput(const char* baseCommand) {
  strncpy(gPendingRemoteCommand, baseCommand, sizeof(gPendingRemoteCommand) - 1);
  gPendingRemoteCommand[sizeof(gPendingRemoteCommand) - 1] = '\0';
  
  // Initialize keyboard with command pre-filled, add space for parameters
  char initialText[OLED_KEYBOARD_MAX_LENGTH];
  snprintf(initialText, sizeof(initialText), "%s ", baseCommand);
  oledKeyboardInit("Remote Command", initialText, OLED_KEYBOARD_MAX_LENGTH);
  
  gRemoteCommandInputActive = true;
  DEBUG_DISPLAYF("[RMENU] Started command input for: %s\n", baseCommand);
}

// Check if remote command input is active
bool isRemoteCommandInputActive() {
  return gRemoteCommandInputActive;
}

// Cancel remote command input
void cancelRemoteCommandInput() {
  gRemoteCommandInputActive = false;
  gPendingRemoteCommand[0] = '\0';
  oledKeyboardReset();
}

// Complete remote command input and execute
void completeRemoteCommandInput() {
  if (!gRemoteCommandInputActive) return;
  
  const char* fullCommand = oledKeyboardGetText();
  if (fullCommand && strlen(fullCommand) > 0) {
    // Execute remote command via unified OLED command helper
    char remoteCmd[128];
    snprintf(remoteCmd, sizeof(remoteCmd), "remote:%s", fullCommand);

    char out[256];
    executeOLEDCommandWithResult(remoteCmd, out, sizeof(out));

    BROADCAST_PRINTF("[OLED] Remote: %s", fullCommand);
    if (strlen(out) > 0) {
      broadcastOutput(out);
    }
  }
  
  gRemoteCommandInputActive = false;
  gPendingRemoteCommand[0] = '\0';
  oledKeyboardReset();
}

// Helper to add a submenu header item
static void addSubmenuHeader(OLEDMenuItemEx* items, int& count, int maxItems, int startIdx,
                              const char* name, const char* icon, const char* submenuId) {
  if (startIdx + count >= maxItems) return;
  OLEDMenuItemEx& item = items[startIdx + count];
  snprintf(item.name, sizeof(item.name), "%s >", name);
  strncpy(item.iconName, icon, sizeof(item.iconName) - 1);
  item.iconName[sizeof(item.iconName) - 1] = '\0';
  item.command[0] = '\0';
  item.targetMode = OLED_OFF;
  item.isRemote = true;
  item.isSubmenu = true;
  item.needsInput = false;
  strncpy(item.submenuId, submenuId, sizeof(item.submenuId) - 1);
  item.submenuId[sizeof(item.submenuId) - 1] = '\0';
  count++;
}

#if ENABLE_BONDED_MODE
// Helper: Load cached manifest for bonded peer
// Returns empty string if not found
static String loadCachedManifest() {
  extern bool filesystemReady;
  
  if (!filesystemReady || !gEspNow || !gEspNow->lastRemoteCapValid) {
    return "";
  }
  
  // Build filename from fwHash in capability summary
  char hashHex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(hashHex + (i * 2), 3, "%02x", gEspNow->lastRemoteCap.fwHash[i]);
  }
  hashHex[32] = '\0';
  
  char pathBuf[64];
  snprintf(pathBuf, sizeof(pathBuf), "/system/manifests/%s.json", hashHex);
  
  FsLockGuard guard("manifest.load");
  // trusted: cached manifest read for OLED capability rendering.
  AuthContext sys = VFS::systemAuth("oled.utils.manifest_read");
  if (!VFS::existsGuarded(pathBuf, sys)) {
    DEBUG_DISPLAYF("[RMENU] Manifest not cached: %s\n", pathBuf);
    return "";
  }

  File f = VFS::openGuarded(pathBuf, "r", sys);
  if (!f) return "";
  
  String content = f.readString();
  f.close();
  DEBUG_DISPLAYF("[RMENU] Loaded manifest: %d bytes\n", content.length());
  return content;
}

// Build submenu items for a given module from cached manifest
void buildRemoteSubmenu(const char* submenuId) {
  gRemoteSubmenuItemCount = 0;
  gRemoteSubmenuSelection = 0;
  strncpy(gRemoteSubmenuId, submenuId, sizeof(gRemoteSubmenuId) - 1);
  gRemoteSubmenuId[sizeof(gRemoteSubmenuId) - 1] = '\0';
  
  if (!gEspNow || !gEspNow->lastRemoteCapValid) return;
  
  auto add = [&](const char* name, const char* icon, const char* cmd, const char* helpText) {
    if (gRemoteSubmenuItemCount >= MAX_DYNAMIC_MENU_ITEMS) return;
    OLEDMenuItemEx& item = gRemoteSubmenuItems[gRemoteSubmenuItemCount];
    strncpy(item.name, name, sizeof(item.name) - 1);
    item.name[sizeof(item.name) - 1] = '\0';
    strncpy(item.iconName, icon, sizeof(item.iconName) - 1);
    item.iconName[sizeof(item.iconName) - 1] = '\0';
    strncpy(item.command, cmd, sizeof(item.command) - 1);
    item.command[sizeof(item.command) - 1] = '\0';
    // Parse help text once to determine if command needs input
    item.needsInput = helpText && (strchr(helpText, '<') || strchr(helpText, '['));
    item.targetMode = OLED_OFF;
    item.isRemote = true;
    item.isSubmenu = false;
    item.submenuId[0] = '\0';
    gRemoteSubmenuItemCount++;
  };
  
  // Load cached manifest
  String manifestStr = loadCachedManifest();
  if (manifestStr.length() == 0) {
    DEBUG_DISPLAYF("[RMENU] No cached manifest, using fallback");
    // Fallback to basic commands
    add("Status", "notify_system", "status", "Show system status");
    add("Help", "help", "help", "Show available commands");
    gInRemoteSubmenu = true;
    return;
  }
  
  // Parse manifest
  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, manifestStr);
  if (err) {
    DEBUG_DISPLAYF("[RMENU] Manifest parse error: %s\n", err.c_str());
    gInRemoteSubmenu = true;
    return;
  }
  
  // Find the module matching submenuId
  JsonArray modules = doc["cliModules"].as<JsonArray>();
  for (JsonObject module : modules) {
    const char* moduleName = module["name"] | "";
    if (strcmp(moduleName, submenuId) != 0) continue;
    
    // Found the module - add all its commands
    JsonArray commands = module["commands"].as<JsonArray>();
    for (JsonObject cmd : commands) {
      const char* cmdName = cmd["name"] | "";
      const char* cmdHelp = cmd["help"] | "";
      bool isAdmin = cmd["admin"] | false;
      
      // Skip empty or admin-only commands for now
      if (strlen(cmdName) == 0) continue;
      
      // Choose icon based on command type
      const char* icon = "terminal";
      if (strstr(cmdName, "status")) icon = "notify_system";
      else if (strstr(cmdName, "wifi")) icon = "notify_server";
      else if (strstr(cmdName, "ble") || strstr(cmdName, "bt")) icon = "bt_idle";
      else if (strstr(cmdName, "gps")) icon = "compass";
      else if (strstr(cmdName, "imu")) icon = "imu_axes";
      else if (strstr(cmdName, "thermal")) icon = "thermal";
      else if (strstr(cmdName, "file")) icon = "notify_files";
      else if (strstr(cmdName, "mute")) icon = "vol_mute";
      else if (strstr(cmdName, "volume") || strstr(cmdName, "gain")) icon = "speaker";
      else if (strstr(cmdName, "record")) icon = "mic";
      else if (strstr(cmdName, "set")) icon = "settings";
      else if (strstr(cmdName, "help")) icon = "help";
      
      // Use first word of command as display name (truncate if needed)
      char displayName[24];
      strncpy(displayName, cmdName, sizeof(displayName) - 1);
      displayName[sizeof(displayName) - 1] = '\0';
      // Add admin indicator
      if (isAdmin && strlen(displayName) < sizeof(displayName) - 2) {
        strcat(displayName, " *");
      }
      
      add(displayName, icon, cmdName, cmdHelp);
    }
    break;
  }
  
  gInRemoteSubmenu = true;
  DEBUG_DISPLAYF("[RMENU] Built submenu '%s' with %d items from manifest\n", submenuId, gRemoteSubmenuItemCount);
}

// Exit remote submenu
void exitRemoteSubmenu() {
  gInRemoteSubmenu = false;
  gRemoteSubmenuId[0] = '\0';
  gRemoteSubmenuItemCount = 0;
  gRemoteSubmenuSelection = 0;
}

// Check if in remote submenu
bool isInRemoteSubmenu() {
  return gInRemoteSubmenu;
}

// Get remote submenu items
OLEDMenuItemEx* getRemoteSubmenuItems() {
  return gRemoteSubmenuItems;
}

// Get remote submenu item count
int getRemoteSubmenuItemCount() {
  return gRemoteSubmenuItemCount;
}

// Get/set remote submenu selection
int getRemoteSubmenuSelection() {
  return gRemoteSubmenuSelection;
}

void setRemoteSubmenuSelection(int sel) {
  if (sel >= 0 && sel < gRemoteSubmenuItemCount) {
    gRemoteSubmenuSelection = sel;
  }
}

// Get current submenu ID
const char* getRemoteSubmenuId() {
  return gRemoteSubmenuId;
}

// Load remote menu items from cached manifest - creates submenu headers for each CLI module
static int loadRemoteMenuItems(OLEDMenuItemEx* items, int maxItems, int startIdx) {

  DEBUG_DISPLAYF("[RMENU] loadRemoteMenuItems called: startIdx=%d maxItems=%d\n", startIdx, maxItems);
  if (!gSettings.bondModeEnabled) {
    DEBUG_DISPLAYF("[RMENU] EXIT: bondModeEnabled=false");
    return 0;
  }
  if (gSettings.bondPeerMac.length() == 0) {
    DEBUG_DISPLAYF("[RMENU] EXIT: bondPeerMac is empty");
    return 0;
  }
  DEBUG_DISPLAYF("[RMENU] bondPeerMac=%s\n", gSettings.bondPeerMac.c_str());
  if (!gEspNow) {
    DEBUG_DISPLAYF("[RMENU] EXIT: gEspNow is NULL");
    return 0;
  }
  if (!gEspNow->lastRemoteCapValid) {
    DEBUG_DISPLAYF("[RMENU] EXIT: lastRemoteCapValid=false (no capability received yet)");
    return 0;
  }

  int count = 0;

  // Load cached manifest to get actual CLI modules
  String manifestStr = loadCachedManifest();
  if (manifestStr.length() == 0) {
    DEBUG_DISPLAYF("[RMENU] No cached manifest, using fallback headers");
    // Fallback to basic headers
    addSubmenuHeader(items, count, maxItems, startIdx, "Commands", "terminal", "core");
    return count;
  }
  
  // Parse manifest
  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, manifestStr);
  if (err) {
    DEBUG_DISPLAYF("[RMENU] Manifest parse error: %s\n", err.c_str());
    addSubmenuHeader(items, count, maxItems, startIdx, "Commands", "terminal", "core");
    return count;
  }
  
  // Create submenu header for each CLI module
  JsonArray modules = doc["cliModules"].as<JsonArray>();
  for (JsonObject module : modules) {
    if (startIdx + count >= maxItems) break;
    
    const char* moduleName = module["name"] | "";
    const char* moduleDesc = module["description"] | moduleName;
    int cmdCount = module["commands"].as<JsonArray>().size();
    
    if (strlen(moduleName) == 0 || cmdCount == 0) continue;
    
    // Choose icon based on module name
    const char* icon = "terminal";
    if (strcmp(moduleName, "wifi") == 0) icon = "wifi_3";
    else if (strcmp(moduleName, "bluetooth") == 0) icon = "bt_idle";
    else if (strcmp(moduleName, "espnow") == 0) icon = "notify_espnow";
    else if (strcmp(moduleName, "mqtt") == 0) icon = "mqtt";
    else if (strcmp(moduleName, "filesystem") == 0) icon = "notify_files";
    else if (strcmp(moduleName, "oled") == 0) icon = "notify_display";
    else if (strcmp(moduleName, "neopixel") == 0) icon = "neopixel";
    else if (strcmp(moduleName, "servo") == 0) icon = "servo";
    else if (strcmp(moduleName, "gamepad") == 0) icon = "gamepad";
    else if (strcmp(moduleName, "i2c") == 0) icon = "notify_sensor";
    else if (strcmp(moduleName, "camera") == 0) icon = "camera";
    else if (strcmp(moduleName, "microphone") == 0) icon = "mic";
    else if (strcmp(moduleName, "presence") == 0) icon = "presence";
    else if (strcmp(moduleName, "rtc") == 0) icon = "rtc";
    else if (strcmp(moduleName, "edgeimpulse") == 0) icon = "edgeimpulse";
    else if (strcmp(moduleName, "espsr") == 0) icon = "espsr";
    else if (strcmp(moduleName, "battery") == 0) icon = "battery_full";
    else if (strcmp(moduleName, "debug") == 0) icon = "debug";
    else if (strcmp(moduleName, "settings") == 0) icon = "settings";
    else if (strcmp(moduleName, "users") == 0) icon = "user";
    else if (strcmp(moduleName, "core") == 0) icon = "notify_system";
    else if (strcmp(moduleName, "cli") == 0) icon = "terminal";
    
    // Create display name with command count
    char displayName[24];
    snprintf(displayName, sizeof(displayName), "%s (%d)", moduleName, cmdCount);
    
    addSubmenuHeader(items, count, maxItems, startIdx, displayName, icon, moduleName);
  }

  DEBUG_DISPLAYF("[RMENU] Created %d module submenu headers from manifest\n", count);
  return count;
}
#endif // ENABLE_BONDED_MODE

// Build dynamic menu based on current DataSource
void buildDynamicMenu() {
  extern DataSource gDataSource;
  
  // Skip if already built for this source
  if (gDynamicMenuBuilt && gLastBuildSource == gDataSource) {
    return;
  }
  
  gDynamicMenuItemCount = 0;
  
  // Add local items if LOCAL or BOTH (built from category arrays)
  if (gDataSource == DataSource::LOCAL || gDataSource == DataSource::BOTH) {
    const struct { const OLEDMenuItem* items; int count; } cats[] = {
      { oledMenuCategory1, oledMenuCategory1Count },
      { oledMenuCategory2, oledMenuCategory2Count },
      { oledMenuCategory3, oledMenuCategory3Count },
      { oledMenuCategory4, oledMenuCategory4Count },
      { oledMenuCategory5, oledMenuCategory5Count },
    };
    for (int c = 0; c < (int)(sizeof(cats)/sizeof(cats[0])); c++) {
      for (int i = 0; i < cats[c].count && gDynamicMenuItemCount < MAX_DYNAMIC_MENU_ITEMS; i++) {
        OLEDMenuItemEx& item = gDynamicMenuItems[gDynamicMenuItemCount];
        
        strncpy(item.name, cats[c].items[i].name, sizeof(item.name) - 1);
        item.name[sizeof(item.name) - 1] = '\0';
        
        strncpy(item.iconName, cats[c].items[i].iconName, sizeof(item.iconName) - 1);
        item.iconName[sizeof(item.iconName) - 1] = '\0';
        
        item.command[0] = '\0';  // No command for local mode items
        item.targetMode = cats[c].items[i].targetMode;
        item.isRemote = false;
        item.isSubmenu = false;
        item.needsInput = false;
        item.submenuId[0] = '\0';
        
        gDynamicMenuItemCount++;
      }
    }
  }
  
#if ENABLE_BONDED_MODE
  // Add remote items if REMOTE or BOTH
  if (gDataSource == DataSource::REMOTE || gDataSource == DataSource::BOTH) {
    DEBUG_DISPLAYF("[MENU] Building REMOTE menu (source=%d)\n", (int)gDataSource);
    int added = loadRemoteMenuItems(gDynamicMenuItems, MAX_DYNAMIC_MENU_ITEMS, gDynamicMenuItemCount);
    DEBUG_DISPLAYF("[MENU] loadRemoteMenuItems returned %d items\n", added);
    gDynamicMenuItemCount += added;
    
    // Add "Remote Settings" menu item if remote settings are available
    extern bool hasRemoteSettings();
    bool hasSettings = hasRemoteSettings();
    DEBUG_DISPLAYF("[MENU] hasRemoteSettings()=%d\n", hasSettings ? 1 : 0);
    if (hasSettings && gDynamicMenuItemCount < MAX_DYNAMIC_MENU_ITEMS) {
      OLEDMenuItemEx& item = gDynamicMenuItems[gDynamicMenuItemCount];
      strncpy(item.name, "Remote Settings", sizeof(item.name) - 1);
      item.name[sizeof(item.name) - 1] = '\0';
      strncpy(item.iconName, "settings", sizeof(item.iconName) - 1);
      item.iconName[sizeof(item.iconName) - 1] = '\0';
      item.command[0] = '\0';
      item.targetMode = OLED_REMOTE_SETTINGS;
      item.isRemote = true;
      item.isSubmenu = false;
      item.needsInput = false;
      item.submenuId[0] = '\0';
      gDynamicMenuItemCount++;
    }
  }
#endif // ENABLE_BONDED_MODE
  
  gDynamicMenuBuilt = true;
  gLastBuildSource = gDataSource;
  
  DEBUG_DISPLAYF("[MENU] Built dynamic menu: %d items (source=%s)\n", gDynamicMenuItemCount, oledGetDataSourceLabel());
}

// Invalidate dynamic menu (call when source changes or manifest updates)
void invalidateDynamicMenu() {
  gDynamicMenuBuilt = false;
}

// Get filtered menu item count
int getFilteredMenuItemCount() {
  buildDynamicMenu();
  return gDynamicMenuItemCount;
}

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
    
    case OLED_REMOTE:
#if ENABLE_ESPNOW && ENABLE_BONDED_MODE
      // Show Bond mode whenever ESP-NOW is available — picker handles the not-yet-bonded state
      return MenuAvailability::AVAILABLE;
#endif
      if (outReason) *outReason = "Not compiled";
      return MenuAvailability::NOT_BUILT;

    
      // Sensor modes - always allow navigation, display functions handle "not active" state
      // Block if the sensor is not built at compile time or not currently detected/connected
      case OLED_THERMAL_VISUAL:
#if !ENABLE_THERMAL_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      if (gThermalConnected) {
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
#if !ENABLE_FM_RADIO
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      if (gFmRadioConnected && gRadioInitialized) {
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
#if !ENABLE_GPS_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      // Check if GPS is running
      if (gGpsConnected && gGpsEnabled) {
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
#if !ENABLE_IMU_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      if (gImuConnected) {
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
#if !ENABLE_TOF_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      if (gTofConnected) {
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
#if !ENABLE_APDS_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      if (gApdsConnected) {
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
#if !ENABLE_GAMEPAD_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      if (gInputConnected) {
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
#if !ENABLE_RTC_SENSOR
        if (outReason) *outReason = "Not built";
        return MenuAvailability::NOT_BUILT;
#else
      if (gRtcConnected) {
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
        extern bool gPresenceConnected;
        if (gPresenceConnected) {
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
        if (outReason) *outReason = "Disabled\nRun: openble";
        return MenuAvailability::FEATURE_DISABLED;
      }
      return MenuAvailability::AVAILABLE;
#endif

    case OLED_WEB_STATS:
#if !ENABLE_WIFI
      if (outReason) *outReason = "Not built";
      return MenuAvailability::NOT_BUILT;
#endif
      // Check if HTTP server is running
      {
        if (!server) {
          if (outReason) *outReason = "Disabled\nRun: openhttp";
          return MenuAvailability::FEATURE_DISABLED;
        }
      }
      return MenuAvailability::AVAILABLE;

    default:
      return MenuAvailability::AVAILABLE;
  }
}

// BatteryIconState struct declared in OLED_Utils.h
BatteryIconState batteryIconState = {0};
extern const unsigned long BATTERY_ICON_UPDATE_INTERVAL = 120000; // 2 minutes

// displayMenu() moved to OLED_Mode_Menu.cpp

// displayMenuListStyle() moved to OLED_Mode_Menu.cpp
// displaySensorMenu() moved to OLED_Mode_Menu.cpp

// displayAutomations() moved to OLED_Mode_Menu.cpp
// displayEspNow() moved to OLED_Mode_Network.cpp

// Handle B button press - exit keyboard input, submenu, or pop mode stack
// Returns true if back was consumed
bool oledMenuBack() {
  // Handle remote command keyboard cancellation
  if (gRemoteCommandInputActive) {
    cancelRemoteCommandInput();
    return true;
  }
  
  // Handle remote submenu back
  if (gInRemoteSubmenu) {
    gInRemoteSubmenu = false;
    gRemoteSubmenuSelection = 0;
    return true;
  }
  
  // If not in menu mode, pop mode stack to go back to previous mode.
  // Category state is preserved so we return to the category submenu, not root.
  if (currentOLEDMode != OLED_MENU) {
    requestOLEDMode(popOLEDMode(), "menu.back", false, /*isBackNav=*/true);
    return true;
  }

  // We're in OLED_MENU - handle category submenu back
  extern int oledMenuCategorySelected;
  extern int oledMenuCategoryItemIndex;
  if (oledMenuCategorySelected >= 0) {
    oledMenuCategorySelected = -1;
    oledMenuCategoryItemIndex = 0;
    return true;
  }
  
  return false;  // At top-level menu, nothing to go back to
}

// pushOLEDMode and popOLEDMode are declared in OLED_Display.h

// LoggingMenuState enum, loggingCurrentState, loggingMenuSelection
// declared in OLED_Utils.h, defined in OLED_Mode_Logging.cpp

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


void resetOLEDMenu() {
  // Fresh entry to the launcher returns to the top-level category list with the
  // cursor at the top. Back-navigation into OLED_MENU skips this (onEnter passes
  // isForward=false), so returning from a launched mode keeps your place.
  oledMenuSelectedIndex = 0;
  oledMenuCategorySelected = -1;
  oledMenuCategoryItemIndex = 0;
}

// ============================================================================
// Gamepad Input for OLED Menu Navigation
// ============================================================================

#if ENABLE_OLED_INPUT

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
NavEvents gNavEvents = {false, false, false, false, 0, 0, 0};

// =============================================================================
// Data Source Selection (for bond mode)
// =============================================================================
DataSource gDataSource = DataSource::LOCAL;
bool gDataSourceIndicatorVisible = false;

// Forward declaration
void invalidateDynamicMenu();

void oledCycleDataSource() {
  if (!oledRemoteSourceAvailable()) {
    // Not bonded or peer offline - stay on LOCAL
    gDataSource = DataSource::LOCAL;
    gDataSourceIndicatorVisible = false;
    invalidateDynamicMenu();
    return;
  }
  
  // Cycle: LOCAL -> REMOTE -> BOTH -> LOCAL
  switch (gDataSource) {
    case DataSource::LOCAL:
      gDataSource = DataSource::REMOTE;
      break;
    case DataSource::REMOTE:
      gDataSource = DataSource::BOTH;
      break;
    case DataSource::BOTH:
      gDataSource = DataSource::LOCAL;
      break;
  }
  gDataSourceIndicatorVisible = true;
  invalidateDynamicMenu();  // Rebuild menu with new source
  oledMarkDirty();
  BROADCAST_PRINTF("[OLED] Data source: %s", oledGetDataSourceLabel());
}

const char* oledGetDataSourceLabel() {
  switch (gDataSource) {
    case DataSource::LOCAL:  return "Local";
    case DataSource::REMOTE: return "Remote";
    case DataSource::BOTH:   return "Both";
    default:                 return "Local";
  }
}

bool oledRemoteSourceAvailable() {
#if ENABLE_BONDED_MODE
  return gSettings.bondModeEnabled && 
         gEspNow && gEspNow->initialized && 
         gEspNow->bondPeerOnline;
#else
  return false;
#endif
}

// Debug throttle for gamepad menu input
static unsigned long lastGamepadDebugTime = 0;
static const unsigned long GAMEPAD_DEBUG_INTERVAL = 30000; // Log every 30 seconds max (reduce spam)

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
  if (!gInputCache.mutex) {
    gInputStateValid = false;
    return;
  }
  
  SensorCacheGuard g(gInputCache.mutex, pdMS_TO_TICKS(10), "oled.inputStateRead");
  if (g.held) {
    if (gInputCache.dataValid) {
      gCurrentJoyX = gInputCache.joyX;
      gCurrentJoyY = gInputCache.joyY;
      gCurrentButtons = gInputCache.buttons;
      gInputStateValid = true;
    } else {
      gInputStateValid = false;
    }
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
#if ENABLE_OLED_INPUT
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
#if ENABLE_OLED_INPUT
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
  DEBUG_DISPLAYF("[GAMEPAD_ACTION] X button pressed in mode %d\n", (int)currentOLEDMode);
  // Check if this mode has a registered custom input handler
  const OLEDModeEntry* registeredMode = findOLEDMode(currentOLEDMode);
  if (registeredMode && registeredMode->inputFunc) {
    // Custom handler exists - it should have already handled the input
    // This function is called as fallback, so just log and return
    DEBUG_DISPLAYF("[GAMEPAD_ACTION] Mode has custom inputFunc, skipping centralized handler");
    return;
  }
  
  // enqueueDeviceStart, isInQueue provided by System_I2C.h
  switch (currentOLEDMode) {
    case OLED_UNAVAILABLE:
      // If feature is "Not built" (compile-time disabled), redirect to menu - no action possible
      if (unavailableOLEDReason.indexOf("Not built") >= 0) {
        requestOLEDMode(OLED_SENSOR_MENU, "unavail.notbuilt", false);
        break;
      }

      // Try to start whatever sensor was unavailable based on the title.
      // pushStack=false: we are replacing OLED_UNAVAILABLE, not stacking over it.
      if (unavailableOLEDTitle == "Thermal") {
#if ENABLE_THERMAL_SENSOR
        executeOLEDCommand("openthermal");
        requestOLEDMode(OLED_THERMAL_VISUAL, "unavail.start.thermal", false);
#endif
      } else if (unavailableOLEDTitle == "ToF") {
#if ENABLE_TOF_SENSOR
        executeOLEDCommand("opentof");
        requestOLEDMode(OLED_TOF_DATA, "unavail.start.tof", false);
#endif
      } else if (unavailableOLEDTitle == "IMU") {
#if ENABLE_IMU_SENSOR
        executeOLEDCommand("openimu");
        requestOLEDMode(OLED_IMU_ACTIONS, "unavail.start.imu", false);
#endif
      } else if (unavailableOLEDTitle == "APDS") {
#if ENABLE_APDS_SENSOR
        executeOLEDCommand("openapds");
        requestOLEDMode(OLED_APDS_DATA, "unavail.start.apds", false);
#endif
      } else if (unavailableOLEDTitle == "GPS") {
#if ENABLE_GPS_SENSOR
        executeOLEDCommand("opengps");
        requestOLEDMode(OLED_GPS_DATA, "unavail.start.gps", false);
#endif
      } else if (unavailableOLEDTitle == "RTC") {
#if ENABLE_RTC_SENSOR
        // Start RTC with confirmation
        static auto rtcOpenConfirmedUnavail = [](void* userData) {
          (void)userData;
          executeOLEDCommand("openrtc");
          requestOLEDMode(OLED_RTC_DATA, "unavail.confirm.rtc", false);
        };
        oledConfirmRequest("Open RTC?", nullptr, rtcOpenConfirmedUnavail, nullptr);
#endif
      } else if (unavailableOLEDTitle == "Presence") {
#if ENABLE_PRESENCE_SENSOR
        // Start Presence with confirmation
        static auto presenceOpenConfirmedUnavail = [](void* userData) {
          (void)userData;
          executeOLEDCommand("openpresence");
          requestOLEDMode(OLED_PRESENCE_DATA, "unavail.confirm.presence", false);
        };
        oledConfirmRequest("Open Presence?", nullptr, presenceOpenConfirmedUnavail, nullptr);
#endif
      } else if (unavailableOLEDTitle == "FM Radio") {
        executeOLEDCommand("openfmradio");
        requestOLEDMode(OLED_FM_RADIO, "unavail.start.fmradio", false);
      } else if (unavailableOLEDTitle == "ESP-NOW") {
#if ENABLE_ESPNOW
        requestOLEDMode(OLED_ESPNOW, "unavail.start.espnow", false);
        if (gSettings.espnowDeviceName.length() == 0) {
          oledEspNowShowNameKeyboard();
        } else {
          executeOLEDCommand("openespnow");
          if (gEspNow && gEspNow->initialized) {
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
        oledMenuBack();
      } else if (unavailableOLEDTitle == "Bluetooth") {
#if ENABLE_BLUETOOTH
        // Initialize Bluetooth
        executeOLEDCommand("openble");
        requestOLEDMode(OLED_BLUETOOTH, "unavail.start.bluetooth", false);
#endif
      } else if (unavailableOLEDTitle == "Web") {
#if ENABLE_HTTP_SERVER
        // Start HTTP server with confirmation
        static auto httpStartConfirmedUnavail = [](void* userData) {
          (void)userData;
          executeOLEDCommand("openhttp");
          broadcastOutput("[OLED] HTTP server started");
          requestOLEDMode(OLED_WEB_STATS, "unavail.confirm.web", false);
        };
        oledConfirmRequest("Start HTTP?", nullptr, httpStartConfirmedUnavail, nullptr);
#endif
      } else {
        DEBUG_DISPLAYF("[GAMEPAD_ACTION] No action for unavailable: %s\n", unavailableOLEDTitle.c_str());
      }
      break;
      
    case OLED_WEB_STATS:
#if ENABLE_HTTP_SERVER
      {
        // Toggle HTTP server with confirmation
        static auto httpStopConfirmedWebStats = [](void* userData) {
          (void)userData;
          executeOLEDCommand("closehttp");
          broadcastOutput("[OLED] HTTP server stopped");
        };
        static auto httpStartConfirmedWebStats = [](void* userData) {
          (void)userData;
          executeOLEDCommand("openhttp");
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
      DEBUG_DISPLAYF("[GAMEPAD_ACTION] No action defined for mode %d\n", (int)currentOLEDMode);
      break;
  }
}

/**
 * Process gamepad input for menu/app navigation
 * Call this from updateOLEDDisplay() when in menu mode
 * Returns true if input was processed
 */
bool processOLEDInput() {
  unsigned long now = millis();
  bool shouldDebug = (now - lastGamepadDebugTime >= GAMEPAD_DEBUG_INTERVAL);
  
  // Check gamepad enabled/connected - silent exit when disabled (no spam)
  if (!gInputEnabled) {
    return false;
  }
  
  // Read from gamepad cache (thread-safe)
  if (!gInputCache.mutex) {
    if (shouldDebug) {
      DEBUG_DISPLAYF("[GAMEPAD_MENU] Exit: gInputCache.mutex is NULL addrs &en=%p &conn=%p &cache=%p\n", (void*)&gInputEnabled, (void*)&gInputConnected, (void*)&gInputCache);
      lastGamepadDebugTime = now;
    }
    return false;
  }
  
  int joyX = 0, joyY = 0;
  uint32_t buttons = 0;
  bool dataValid = false;
  bool mutexTaken = false;
  
  uint32_t latchedPresses = 0;
  {
    SensorCacheGuard g(gInputCache.mutex, pdMS_TO_TICKS(10), "oled.gamepadMenuRead");
    if (g.held) {
      mutexTaken = true;
      if (gInputCache.dataValid) {
        joyX = gInputCache.joyX;
        joyY = gInputCache.joyY;
        buttons = gInputCache.buttons;
        latchedPresses = gInputCache.buttonPressedAccum;
        gInputCache.buttonPressedAccum = 0;  // Consume accumulated presses
        dataValid = true;
      }
    }
  }
  
  if (!dataValid) {
    if (shouldDebug) {
      DEBUG_DISPLAYF("[GAMEPAD_MENU] Exit: mutexTaken=%d dataValid=%d\n", mutexTaken, dataValid);
      lastGamepadDebugTime = now;
    }
    return false;
  }
  
  // Log current state periodically
  if (shouldDebug) {
    int deltaX = joyX - JOYSTICK_CENTER;
    int deltaY = JOYSTICK_CENTER - joyY;
    DEBUG_DISPLAYF("[GAMEPAD_MENU] joyX=%d joyY=%d dX=%d dY=%d buttons=0x%08lX mode=%d sel=%d\n", joyX, joyY, deltaX, deltaY,
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
  // But NOT when keyboard is active: pattern mode needs center-return events to reset deflection state
  //
  // ANO encoder caveat: rotation alone doesn't deflect the joystick (joyX/Y
  // stay at CENTER) and doesn't change `buttons` either. Without peeking at
  // the encoder's pending-detents cache here, a wheel-only spin would early-
  // exit and never reach the consumer below — detents would pile up forever
  // and only get drained the next time the user pressed a button.
  bool hasEncoderInput = false;
#if ENABLE_ANO_ENCODER
  // No mutex: encoderDelta is a single int32_t, racing with the driver's
  // accumulate is benign — we read a slightly stale value, the next frame
  // catches up. Cheaper than a 5ms guard on the hot path.
  hasEncoderInput = (gAnoEncoderCache.encoderDelta != 0);
#endif
  if (!hasJoystickInput && !hasButtonChange && !wasDeflectedX && !wasDeflectedY && !hasEncoderInput) {
    if (!oledKeyboardIsActive()) return false;
  }
  
  // Debounce navigation - don't update lastButtonState here or edge detection breaks!
  // Skip debounce when keyboard is active: keyboard has its own timing (pattern mode needs continuous polling)
  if (now - lastGamepadNavTime < GAMEPAD_NAV_DEBOUNCE) {
    if (!oledKeyboardIsActive()) return false;
  }
  
  // Initialize lastButtonState on first valid read AFTER debounce
  if (!lastButtonStateInitialized) {
    lastButtonState = buttons;
    lastButtonStateInitialized = true;
    DEBUG_DISPLAYF("[GAMEPAD_INIT] Initialized lastButtonState=0x%08lX\n", (unsigned long)buttons);
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
    DEBUG_DISPLAYF("[GAMEPAD_BUTTONS] buttons=0x%08lX last=0x%08lX changed=0x%08lX\n", (unsigned long)buttons, (unsigned long)lastButtonState,
                  (unsigned long)(buttons ^ lastButtonState));
  }
  
  // =========================================================================
  // CENTRALIZED NAVIGATION EVENTS - computed once, used by all handlers
  // =========================================================================
  // Reset navigation events
  gNavEvents = {false, false, false, false, deltaX, deltaY, 0};

#if ENABLE_ANO_ENCODER
  // Canonical-signal model: every input device emits ONLY the signals that
  // describe what it physically is. The ANO encoder has two physical input
  // primitives — a quadrature wheel and 5 discrete buttons — and nothing
  // about a joystick. So we emit:
  //
  //   • gNavEvents.wheelDelta — sum of detents consumed this frame (signed).
  //     A separate channel from deltaX/Y so the wheel never has to fake
  //     analog deflection. Mode handlers that want fine-grained wheel
  //     response read this directly; modes that only want "one nav per
  //     event" read the booleans below.
  //
  //   • gNavEvents.up/down/left/right — edge-style "nav happened" bools,
  //     emitted from BOTH wheel sign (axis-aware) AND dpad press edges, so
  //     existing menu code that only checks booleans keeps working without
  //     change.
  //
  //   • newlyPressed (downstream) — button rising edges from IN/UP/DOWN/
  //     LEFT/RIGHT plus the synthesized START from RIGHT+IN chord, mapped
  //     to logical INPUT_BUTTON_* via gAnoEncoderMapping. Handlers read
  //     these via INPUT_CHECK exactly as on the gamepad.
  //
  //   • deltaX/deltaY — left untouched (stays at 0 for the ANO because
  //     joyX/joyY are always JOYSTICK_CENTER). Joystick-deflection-aware
  //     modes (pattern keyboard, FM radio tuner) get nothing from the ANO,
  //     which is correct: the wheel isn't a joystick.
  //
  // No mode-awareness here. The keyboard, menus, and any future mode read
  // whichever signals make sense for them; this layer just reports what
  // the hardware did.

  // ---- Wheel ----
  // Drain ALL pending detents in one frame. consumeOneDetent returns one
  // at a time and is cheap; a fast spin produces a single proportional
  // wheelDelta that the consumer can apply in one step instead of dribbling
  // events across OLED ticks (where the OLED's 8 Hz update would cap the
  // effective scroll rate).
  {
    int totalDetents = 0;
    int d;
    while ((d = anoEncoderConsumeOneDetent()) != 0) {
      totalDetents += d;
      if (totalDetents > 64 || totalDetents < -64) break;  // safety clamp
    }
    gNavEvents.wheelDelta = totalDetents;

    // Boolean nav events from wheel sign — axis chooses up/down vs left/
    // right so modes that pre-date wheelDelta still respond. One bool max
    // per frame regardless of detent count; modes that want proportional
    // response read wheelDelta.
    if (totalDetents != 0) {
      uint8_t axis = ANO_AXIS_VERTICAL;
      if (gAnoEncoderCache.mutex) {
        SensorCacheGuard g(gAnoEncoderCache.mutex, pdMS_TO_TICKS(5), "ano.readAxis");
        if (g.held) axis = gAnoEncoderCache.currentAxis;
      }
      if (totalDetents > 0) {
        if (axis == ANO_AXIS_VERTICAL) gNavEvents.down  = true;
        else                            gNavEvents.right = true;
      } else {
        if (axis == ANO_AXIS_VERTICAL) gNavEvents.up   = true;
        else                            gNavEvents.left = true;
      }
      DEBUG_ANO_ENCODER_VALUESF("[ANO_VAL] wheel    delta=%+d axis=%s",
                                totalDetents, axis == ANO_AXIS_VERTICAL ? "V" : "H");
    }
  }

  // ---- Dpad ----
  // Edge-detected → boolean nav events. NO deltaX/Y emission: the dpad is a
  // digital input device with no analog magnitude. Modes that want sustained
  // / auto-repeat directional input use the wheel; modes that want a single
  // nav step per press read the booleans here. The press also lands in
  // newlyPressed downstream, where INPUT_CHECK maps it to its logical
  // INPUT_BUTTON_* role (LEFT=B, UP=Y, DOWN=X, RIGHT=SELECT).
  {
    uint32_t dpadEdge = latchedPresses & (ANO_BTN_UP | ANO_BTN_DOWN | ANO_BTN_LEFT | ANO_BTN_RIGHT);
    if (dpadEdge) {
      if (dpadEdge & ANO_BTN_UP)    gNavEvents.up    = true;
      if (dpadEdge & ANO_BTN_DOWN)  gNavEvents.down  = true;
      if (dpadEdge & ANO_BTN_LEFT)  gNavEvents.left  = true;
      if (dpadEdge & ANO_BTN_RIGHT) gNavEvents.right = true;
      DEBUG_ANO_ENCODER_VALUESF("[ANO_VAL] dpad     edge=0x%02lX", (unsigned long)dpadEdge);
    }
  }
#else
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
#endif  // !ENABLE_ANO_ENCODER (joystick → nav events)
  // =========================================================================

  {
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtonState;
    uint32_t newlyPressed = (pressedNow & ~pressedLast) | latchedPresses;
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

  if (currentOLEDMode == OLED_ESPNOW) {
#if ENABLE_ESPNOW
    // ESP-NOW interface navigation
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtonState;
    uint32_t newlyPressed = (pressedNow & ~pressedLast) | latchedPresses;
    
    if (shouldDebug) {
      DEBUG_DISPLAYF("[ESPNOW_BUTTONS] buttons=0x%08lX pressedNow=0x%08lX pressedLast=0x%08lX newlyPressed=0x%08lX\n", (unsigned long)buttons, (unsigned long)pressedNow, 
                    (unsigned long)pressedLast, (unsigned long)newlyPressed);
      DEBUG_DISPLAYF("[GAMEPAD_LOGICAL] MODE=ESP-NOW newly=0x%08lX A=%d B=%d X=%d Y=%d START=%d\n", (unsigned long)newlyPressed,
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
            BROADCAST_PRINTF("[OLED] Setting ESP-NOW name: %s", deviceName);
            // First set the name via command system
            char setnameCmd[48];
            snprintf(setnameCmd, sizeof(setnameCmd), "espnowsetname %s", deviceName);
            executeOLEDCommand(setnameCmd);
            if (gSettings.espnowDeviceName.length() > 0) {
              // Then initialize ESP-NOW via command system
              broadcastOutput("[OLED] Initializing ESP-NOW...");
              executeOLEDCommand("openespnow");
              if (gEspNow && gEspNow->initialized) {
                broadcastOutput("[OLED] ESP-NOW initialized successfully");
                // Enable ESP-NOW in settings so it persists
                executeOLEDCommand("espnowenabled 1");
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
        DEBUG_DISPLAYF("[ESPNOW_INIT] Checking buttons: newlyPressed=0x%08lX Y_mask=0x%08lX B_mask=0x%08lX\n", (unsigned long)newlyPressed, 
                      (unsigned long)INPUT_MASK(INPUT_BUTTON_Y),
                      (unsigned long)INPUT_MASK(INPUT_BUTTON_B));
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
          if (gSettings.espnowDeviceName.length() == 0) {
            DEBUG_DISPLAYF("[ESPNOW_INIT] Y button pressed - opening keyboard");
            oledEspNowShowNameKeyboard();
          } else {
            DEBUG_DISPLAYF("[ESPNOW_INIT] Y button pressed - initializing ESP-NOW (name already set)");
            executeOLEDCommand("openespnow");
            if (gEspNow && gEspNow->initialized) {
              oledEspNowInit();
            }
          }
          inputProcessed = true;
        }
        // B button: Back to menu
        if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
          DEBUG_DISPLAYF("[ESPNOW_INIT] B button pressed - going back");
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
  // OLED_NETWORK_INFO now handled by registered inputFunc (see OLED_Mode_Network.cpp)
  } else if (currentOLEDMode == OLED_REMOTE) {
#if ENABLE_ESPNOW && ENABLE_BONDED_MODE
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtonState;
    uint32_t newlyPressed = (pressedNow & ~pressedLast) | latchedPresses;
    
    extern bool bondModeInputHandler(int deltaX, int deltaY, uint32_t newlyPressed);
    if (bondModeInputHandler(deltaX, deltaY, newlyPressed)) {
      inputProcessed = true;
    }
    if (!inputProcessed && INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      oledMenuBack();
      inputProcessed = true;
    }
#endif
  } else {
    // Any other mode - check for registered custom input handler first
    uint32_t pressedNow = ~buttons;
    uint32_t pressedLast = ~lastButtonState;
    uint32_t newlyPressed = (pressedNow & ~pressedLast) | latchedPresses;
    
    // Global SELECT button handler - access quick settings from anywhere (only if authenticated)
    // NOTE: Skip if keyboard is active since SELECT toggles keyboard mode
    if (!oledKeyboardIsActive() && INPUT_CHECK(newlyPressed, INPUT_BUTTON_SELECT)) {
      if (!gSettings.localDisplayRequireAuth || isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)) {
        requestOLEDMode(OLED_QUICK_SETTINGS, "gamepad.any.quicksettings");
        inputProcessed = true;
      }
    }
    
    // CENTRALIZED KEYBOARD HANDLING
    // When keyboard is active, ALL input goes to keyboard first (including zero-deflection
    // joystick events needed by pattern mode to reset between directions).
    // This prevents every mode from needing to handle keyboard input specially.
    if (oledKeyboardIsActive()) {
      bool kbHandled = oledKeyboardHandleInput(deltaX, deltaY, newlyPressed);
      if (kbHandled) {
        inputProcessed = true;
      }
      // Don't pass input to mode handler - keyboard owns input while active
      // Mode will check oledKeyboardIsCompleted()/oledKeyboardIsCancelled() on next render
      lastButtonState = buttons;
      return inputProcessed;
    }
    
    // Check if this mode has a registered custom input handler
    const OLEDModeEntry* registeredMode = findOLEDMode(currentOLEDMode);
    if (registeredMode && registeredMode->inputFunc) {
#if ENABLE_ANO_ENCODER
      // Mode-dispatch log under ANO (VALUES). Shows exactly what arguments the
      // mode's inputFunc receives — the place where wheel/button events finally
      // become menu navigation. Suppressed when nothing meaningful is happening.
      if (deltaX != 0 || deltaY != 0 || newlyPressed != 0) {
        DEBUG_ANO_ENCODER_VALUESF("[ANO_VAL] dispatch mode=%d dX=%d dY=%d newly=0x%08lX",
                                  (int)currentOLEDMode, deltaX, deltaY,
                                  (unsigned long)newlyPressed);
      }
#endif
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
    DEBUG_DISPLAYF("[GAMEPAD_MENU] ACTION! sel=%d mode=%d\n", oledMenuSelectedIndex, currentOLEDMode);
  }
  
  lastButtonState = buttons;
  return inputProcessed;
}

/**
 * Try to auto-start gamepad when entering menu mode
 */
void tryAutoStartInputForMenu() {
  DEBUG_DISPLAYF("[GAMEPAD_AUTO] tryAutoStartInputForMenu: enabled=%d connected=%d\n", gInputEnabled, gInputConnected);
  if (gInputEnabled && gInputConnected) {
    DEBUG_DISPLAYF("[GAMEPAD_AUTO] Already running, skipping");
    return;  // Already running
  }

  bool inFirstTimeSetup = (gFirstTimeSetupState != SETUP_NOT_NEEDED);
  if (!inFirstTimeSetup) {
    // Pick the right auto-start setting for the active input driver.
#if ENABLE_ANO_ENCODER
    bool autoStart = gSettings.inputAutoStart;
#else
    bool autoStart = gSettings.inputAutoStart;
#endif
    if (!autoStart || !gSettings.i2cBusEnabled) {
      return;
    }
  }

  // Resolve the active input device's I2C address — gamepad at 0x50, or
  // the ANO encoder at whatever the user configured (default 0x49).
#if ENABLE_ANO_ENCODER
  uint8_t inputAddr = (gSettings.anoEncoderI2cAddr > 0 && gSettings.anoEncoderI2cAddr < 0x80)
                        ? (uint8_t)gSettings.anoEncoderI2cAddr
                        : I2C_ADDR_ANO_ENCODER;
#else
  uint8_t inputAddr = I2C_ADDR_GAMEPAD;
#endif
  // Ping on the input device's CONFIGURED bus (was implicit bus 0 — would miss
  // an input device on bus 1). The device-start queue then routes to the same bus.
  bool pingResult = i2cPingAddress(inputAddr, 100000, 50, (uint8_t)gSettings.inputBus);
  DEBUG_DISPLAYF("[INPUT_AUTO] I2C ping 0x%02X on bus %d result: %d\n", inputAddr, gSettings.inputBus, pingResult);
  if (pingResult) {
    // Input device detected — try to start it via the shared queue slot.
    bool inQueue = isInQueue(I2C_DEVICE_INPUT);
    DEBUG_DISPLAYF("[INPUT_AUTO] inQueue=%d\n", inQueue);
    if (!inQueue) {
      bool enqueued = enqueueDeviceStart(I2C_DEVICE_INPUT);
      DEBUG_DISPLAYF("[INPUT_AUTO] enqueueDeviceStart result: %d\n", enqueued);
      DEBUG_DISPLAYF("[OLED] Auto-starting input device for menu navigation");
    }
  }
}

#else // !ENABLE_OLED_INPUT

// Stubs when no input device is built in
bool processOLEDInput() { return false; }
void tryAutoStartInputForMenu() {}

#endif // ENABLE_OLED_INPUT

// ============================================================================
// OLED File Browser (128x64 optimized)
// ============================================================================
FileManager* gOledFileManager = nullptr;
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

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry oledCommands[] = {
  { "openoled", "Start OLED display.", false, cmd_oledstart },
  { "closeoled", "Stop OLED display.", false, cmd_oledstop },
  { "oledread", "Read OLED display status. (add 'json' for JSON output)", false, cmd_oledstatus },
  { "oledstart", "Start OLED display.", false, cmd_oledstart },
  { "oledstop", "Stop OLED display.", false, cmd_oledstop },
  { "oledmode", "Set display mode: <mode>", false, cmd_oledmode,
    "Usage: oledmode <menu|status|sensordata|sensorlist|thermal|network|mesh|gps|text|logo|anim|imuactions|fmradio|files|automations|espnow|memory|off>\n"
    "Example: oledmode memory\n"
    "Example: oledmode off" },
  { "oledtext", "Set custom text: <message>", false, cmd_oledtext, "Usage: oledtext <message>" },
  { "oledanim", "Select animation: <name> or fps <1-60>", false, cmd_oledanim,
    "Usage: oledanim <name>\n"
    "       oledanim fps <1-60>" },
  { "oledclear", "Clear OLED display.", false, cmd_oledclear },
  { "oledstatus", "Show OLED status. (add 'json' for JSON output)", false, cmd_oledstatus },
  { "oledrequireauth", "OLED auth requirement: <0|1>", true, cmd_oled_requireauth, "Usage: oledrequireauth <0|1>" },
  { "oledenabled", "Enable/disable OLED: <0|1>", false, cmd_oled_enabled, "Usage: oledenabled <0|1>" },
  { "oledbootmode", "OLED boot mode: <logo|status|sensors|thermal|network|mesh|off>", false, cmd_oled_bootmode, "Usage: oledbootmode <logo|status|sensors|thermal|network|mesh|off>" },
  { "oleddefaultmode", "OLED default mode: <logo|status|sensors|thermal|network|mesh|off>", false, cmd_oled_defaultmode, "Usage: oleddefaultmode <logo|status|sensors|thermal|network|mesh|off>" },
  { "oledbootduration", "Boot animation duration (ms): <500-10000>", false, cmd_oled_bootduration, "Usage: oledbootduration <500..10000>" },
  { "oledupdateinterval", "Display update interval (ms): <10-1000>", false, cmd_oled_updateinterval, "Usage: oledupdateinterval <10..1000>" },
  { "oledbrightness", "Display brightness: <0-255>", false, cmd_oled_brightness, "Usage: oledbrightness <0..255>" },
  { "oledflip",       "Flip display 180°: [on|off|toggle]", false, cmd_oled_flip, "Usage: oledflip [on|off|toggle]" },
  { "oledthermalscale", "Thermal image scale: <0.1-10.0>", false, cmd_oled_thermalscale, "Usage: oledthermalscale <0.1..10.0>" },
  { "oledthermalcolormode", "Thermal color mode: <3level|grayscale>", false, cmd_oled_thermalcolormode, "Usage: oledthermalcolormode <3level|grayscale>" },
};

const size_t oledCommandsCount = sizeof(oledCommands) / sizeof(oledCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// OLED Settings Module moved to OLED_Settings.cpp

#endif // ENABLE_OLED_DISPLAY

// =============================================================================
// OLED WRAPPER FUNCTIONS - Always compiled, safe to call without guards
// =============================================================================

void oledSetBootProgress(int percent, const char* label) {
#if ENABLE_OLED_DISPLAY
  bootProgressPercent = percent;
  bootProgressLabel = label;
  if (gOledEnabled && oledConnected) {
    updateOLEDDisplay();
  }
#endif
}

void oledUpdate() {
#if ENABLE_OLED_DISPLAY
  if (gOledEnabled && oledConnected) {
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
  if (oledConnected && gOledEnabled) {
    if (gSettings.oledBrightness >= 0 && gSettings.oledBrightness <= 255) {
      i2cDeviceTransactionVoid((uint8_t)gSettings.oledBus, I2C_ADDR_OLED, 400000, 200, [&]() {
        oledDisplay->ssd1306_command(SSD1306_SETCONTRAST);
        oledDisplay->ssd1306_command(gSettings.oledBrightness);
      });
    }
  }
#endif
}

// Apply oledFlipped live by re-rotating the active display. setRotation only
// changes the GFX coordinate transform — anything already in the frame buffer
// stays in the old orientation until the next render tick draws over a cleared
// buffer, so we clear + flush once to avoid showing a mirrored intermediate
// frame. The next normal mode tick repaints in the new orientation.
void applyOLEDRotation() {
#if ENABLE_OLED_DISPLAY
  if (!oledConnected || !gOledEnabled || !gDisplay) return;
  uint8_t rot = gSettings.oledFlipped ? 2 : 0;
  gDisplay->setRotation(rot);
  gDisplay->clearDisplay();
  displayUpdate();
#endif
}

void oledApplySettings() {
#if ENABLE_OLED_DISPLAY
  if (oledConnected && gOledEnabled) {
    applyOLEDBrightness();
    applyOLEDRotation();
    DEBUG_SYSTEMF("OLED settings applied - boot animation running");
  }
#endif
}

// Local-display (OLED) session idle-logout. Same per-transport policy as
// web/serial/BLE, keyed on PHYSICAL input only: gInputCache.seq is a monotonic
// counter advanced solely by the gamepad/ANO input devices (the same signal
// power-save uses), so CLI/web/ESP-NOW commands never refresh the OLED session
// — a network-busy box still locks its own screen. Deliberately independent of
// display-sleep: this revokes the login, it does not blank pixels, and it keeps
// running while the panel is asleep so an idle session still expires in the dark.
void localDisplaySessionTick() {
#if ENABLE_OLED_DISPLAY
  extern InputCache gInputCache;

  static uint32_t lastSeenSeq = 0;
  static bool inited = false;
  const uint32_t seq = gInputCache.seq;
  if (!inited) { inited = true; lastSeenSeq = seq; }

  if (!gLocalDisplayAuthed) {        // no local session → nothing to age
    lastSeenSeq = seq;
    return;
  }

  if (seq != lastSeenSeq) {          // real physical input → refresh idle clock
    lastSeenSeq = seq;
    gLocalDisplayLastInteractionMs = sessionStampNow();
    return;
  }

  if (sessionIdleExpired(SOURCE_LOCAL_DISPLAY, gLocalDisplayLastInteractionMs)) {
    // Clears gLocalDisplayAuthed/User and forces the OLED_LOGIN screen via
    // oledNotifyLocalDisplayAuthChanged() (or the auth guard on next render if
    // the panel is currently asleep).
    logoutTransport(SOURCE_LOCAL_DISPLAY);
    broadcastOutput("[display] Signed out due to inactivity. Please log in again.");
  }
#endif
}

void oledNotifyLocalDisplayAuthChanged() {
#if ENABLE_OLED_DISPLAY
  if (!gOledEnabled || !oledConnected) {
    return;
  }

  // If auth is required and the display is not authenticated, force the login screen.
  if (shouldBlockForDisplayAuth()) {
    if (currentOLEDMode != OLED_LOGIN) {
      requestOLEDMode(OLED_LOGIN, "auth.guard.notify", false);
      updateOLEDDisplay();
    }
    return;
  }

  // If we just became authenticated while on the login screen, return to the menu.
  if (gLocalDisplayAuthed && currentOLEDMode == OLED_LOGIN) {
    requestOLEDMode(OLED_MENU, "auth.notify.loggedin", false);
    resetOLEDMenu();
    tryAutoStartInputForMenu();
#if ENABLE_OLED_INPUT
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
    i2cDeviceTransactionVoid((uint8_t)gSettings.oledBus, I2C_ADDR_OLED, 400000, 500, [&]() {
      oledDisplay->ssd1306_command(SSD1306_DISPLAYOFF);
    });
  }
#endif
}

// Pre-sleep + post-wake helpers. These supersede oledDisplayOff/On for
// sleep flows because on power-gated boards (FeatherS3[D]) we also have to
// kill / restore LDO2 — which means the SSD1306 chip loses Vcc and needs a
// full re-init on wake. On other boards they degrade gracefully to a plain
// SSD1306-DISPLAYOFF / DISPLAYON, so cmd_lightsleep + deep-sleep entry can
// always call these instead of the lower-level helpers.
void oledPrepareForSleep() {
#if ENABLE_OLED_DISPLAY
  // Step 1: send DISPLAYOFF while the chip still has power, so it shuts down
  // its charge pump cleanly rather than just losing Vcc mid-frame.
  oledDisplayOff();
  delay(10);  // let the SSD1306 internal sequencer settle

  // Step 2: if our bus has a software-controllable power rail, drop it. The
  // pin's LOW state is preserved through light sleep automatically (the I/O
  // MUX retains digital state). For deep sleep, GPIO39 isn't an RTC IO on
  // ESP32-S3 so it gets reset on wake — but that's a fresh boot path anyway,
  // and the LDO defaults disabled when its enable line floats, so this is
  // also the desired sleep state.
#if defined(I2C2_POWER_PIN) && (I2C2_POWER_PIN >= 0)
  if (gSettings.oledBus == 1) {
    digitalWrite(I2C2_POWER_PIN, LOW);
    // Hold LDO low long enough for SSD1306 Vcc to fully decay past its
    // POR-rearm threshold before we hand off to esp_light_sleep_start.
    // Without this wait, the CPU gates a few ms after the enable pin
    // drops — Vcc is still ramping down across the chip's internal caps
    // and bus capacitance when sleep latches. On wake, the chip's POR
    // circuit may have never seen a clean falling edge, leaving it in
    // a half-reset state where the I2C peripheral is alive but the
    // analog/display subsystem is wedged. 200ms is overkill on paper
    // (SSD1306 internal caps discharge through quiescent current in
    // ~tens of ms) but cheap insurance against slow-decay variants.
    delay(200);
    DEBUG_SYSTEMF("[OLED] sleep: dropped I2C2 power pin GPIO%d + 200ms decay wait",
                  (int)I2C2_POWER_PIN);
  }
#endif
#endif
}

void oledResumeFromSleep() {
#if ENABLE_OLED_DISPLAY
#if defined(I2C2_POWER_PIN) && (I2C2_POWER_PIN >= 0)
  if (gSettings.oledBus == 1) {
    // Step 1: raise the LDO enable and wait for the rail + chip to come up.
    // The AP2127 LDO itself stabilises in ~5ms, BUT the SSD1306's internal
    // power-on reset + charge-pump startup wants ~100ms after VCC is good
    // before it's reliably ready to take I2C commands. The old 10ms wait was
    // enough for the LDO but not for the SSD1306 — begin() would land before
    // the chip was awake and fail silently, leaving the panel blank on wake.
    pinMode(I2C2_POWER_PIN, OUTPUT);
    digitalWrite(I2C2_POWER_PIN, HIGH);
    delay(100);

    // Step 1.5: BUS RECOVERY. When the SSD1306 lost Vcc mid-transaction (or
    // even just had a slow power-down), it may have left SDA stuck LOW. The
    // ESP32 I2C peripheral then sees a "busy" bus and silently NACKs every
    // transaction we issue — Wire1.endTransmission() returns an error but
    // the Adafruit library doesn't check it, so begin() reports success
    // (the test-ACK probe succeeds against the I2C controller's own ACK,
    // not the device) and frames pretend to push but never actually clock
    // out on the wire. Symptom: render counter ticks, no transactions
    // visible on scope, panel stays dark.
    //
    // performBusRecovery does the textbook fix: Wire1.end(), manually
    // toggle SCL 9 times with SDA released to clock out any device stuck
    // mid-byte, generate a clean STOP, then Wire1.begin() to fully reset
    // the I2C peripheral state on the MCU side. After this, the bus is
    // guaranteed in IDLE and the SSD1306 is in the same state begin()
    // expects at first-boot.
    {
      I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
      if (mgr) {
        mgr->performBusRecovery(1);
      }
    }

    // Step 2: the SSD1306 just power-cycled, so EVERY register is back at
    // ROM defaults — contrast, segment remap, COM scan dir, display on/off,
    // GDDRAM contents, the lot. Re-running begin() walks the standard init
    // sequence, then we reapply rotation + brightness + force a redraw.
    //
    // One automatic retry with another 100ms wait: cheap insurance against
    // edge cases (cold panel, marginal LDO, bus needing a recovery clock).
    if (oledDisplay) {
      bool ok = false;
      for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
        if (attempt > 0) {
          WARN_SYSTEMF("[OLED] wake: begin() attempt %d failed — waiting + retrying", attempt);
          delay(100);
        }
        ok = i2cDeviceTransaction((uint8_t)1, I2C_ADDR_OLED, 100000, 300, [&]() -> bool {
          return oledDisplay->begin(SSD1306_SWITCHCAPVCC, I2C_ADDR_OLED);
        });
      }
      if (ok) {
        // CRITICAL: After a real power-cycle of the SSD1306, the Adafruit
        // library's begin() reports success (chip ACKs I2C) but in practice
        // the panel often stays dark — the high-voltage charge pump that
        // drives the OLED pixels doesn't latch on. The chip is fully
        // responsive over I2C and our framebuffer pushes succeed silently,
        // but nothing is lit. Symptom: render counter ticks up, OLED back
        // power LED is on, but panel is black.
        //
        // Workaround: re-send the three commands that matter directly,
        // bypassing the library's init path entirely. DISPLAYOFF first
        // (clean state), CHARGEPUMP=0x14 (enable internal DC-DC for
        // SWITCHCAPVCC), DISPLAYON last. Wrapped in our own transaction
        // so we own the bus lock and timing.
        i2cDeviceTransactionVoid((uint8_t)1, I2C_ADDR_OLED, 400000, 200, [&]() {
          oledDisplay->ssd1306_command(SSD1306_DISPLAYOFF);
          oledDisplay->ssd1306_command(SSD1306_CHARGEPUMP);
          oledDisplay->ssd1306_command(0x14);  // enable charge pump (SWITCHCAPVCC)
          oledDisplay->ssd1306_command(SSD1306_DISPLAYON);
        });

        applyOLEDRotation();
        applyOLEDBrightness();
        // Push an explicit blank frame — proves the chip is responsive AND
        // overwrites whatever GDDRAM ended up at after the power cycle, so
        // there's no glitchy first-frame flash before the next mode render.
        i2cDeviceTransactionVoid((uint8_t)1, I2C_ADDR_OLED, 400000, 200, [&]() {
          oledDisplay->clearDisplay();
          oledDisplay->display();
        });
        oledMarkDirty();
        // And immediately render the current mode so the user sees something
        // the instant they wake — don't wait for the next main-loop tick (the
        // cmd_exec task that called us may keep the floor for a few more ms).
        updateOLEDDisplay();
        WARN_SYSTEMF("[OLED] wake: SSD1306 re-init + charge-pump kick on bus 1");
      } else {
        ERROR_SYSTEMF("[OLED] wake: SSD1306 begin() failed after LDO2 restore (both attempts)");
      }
    }
    return;
  }
#endif
  // Bus 0 / no power-gating: nothing was actually power-cycled, just flip
  // the panel back on with the existing software command.
  oledDisplayOn();
#endif
}

void oledDisplayOn() {
#if ENABLE_OLED_DISPLAY
  if (oledDisplay && oledConnected) {
    i2cDeviceTransactionVoid((uint8_t)gSettings.oledBus, I2C_ADDR_OLED, 400000, 500, [&]() {
      oledDisplay->ssd1306_command(SSD1306_DISPLAYON);
    });
  }
#endif
}

void oledShowSleepScreen(int seconds) {
#if ENABLE_OLED_DISPLAY
  if (oledDisplay && oledConnected) {
    i2cDeviceTransactionVoid((uint8_t)gSettings.oledBus, I2C_ADDR_OLED, 400000, 500, [&]() {
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
