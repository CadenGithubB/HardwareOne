// OLED_Mode_Menu.cpp - Menu display functions
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "HAL_Input.h"
#include "System_Battery.h"
#include "System_Icons.h"
#include "System_Settings.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

extern DisplayDriver* oledDisplay;
extern bool oledConnected;
extern OLEDMode currentOLEDMode;
extern Settings gSettings;

// Dynamic menu system
extern OLEDMenuItemEx gDynamicMenuItems[];
extern int gDynamicMenuItemCount;
extern void buildDynamicMenu();

// Remote submenu system
extern bool isInRemoteSubmenu();
extern OLEDMenuItemEx* getRemoteSubmenuItems();
extern int getRemoteSubmenuItemCount();
extern int getRemoteSubmenuSelection();
extern const char* getRemoteSubmenuId();

// Sensor menu state variables from OLED_Display.cpp
extern const OLEDMenuItem oledSensorMenuItems[];
extern const int oledSensorMenuItemCount;
extern int oledSensorMenuSelectedIndex;

// Battery icon state
struct BatteryIconState {
  float percentage;
  char icon;
  unsigned long lastUpdateMs;
  bool valid;
};
extern BatteryIconState batteryIconState;
extern const unsigned long BATTERY_ICON_UPDATE_INTERVAL;

// External functions
extern float getBatteryPercentage();
extern char getBatteryIcon();
extern void drawIcon(DisplayDriver* display, const char* iconName, int x, int y, uint16_t color);
extern void drawIconScaled(DisplayDriver* display, const char* iconName, int x, int y, uint16_t color, float scale);
extern void enterUnavailablePage(const String& title, const String& reason);

// ============================================================================
// Sensor Menu Filtering & Sorting
// ============================================================================

// Filtered/sorted index array - only includes compiled-in sensors
static int sensorMenuSortedIndices[20];  // Max 20 sensor items
static int sensorMenuVisibleCount = 0;   // Number of visible (compiled-in) items
static bool sensorMenuSorted = false;

// Get sort priority for availability (lower = higher priority)
static int getAvailabilitySortPriority(MenuAvailability avail) {
  switch (avail) {
    case MenuAvailability::AVAILABLE:        return 0;  // Ready - show first
    case MenuAvailability::FEATURE_DISABLED: return 1;  // Off but available
    case MenuAvailability::NOT_DETECTED:     return 2;  // No hardware
    default:                                 return 3;
  }
}

// Filter and sort the sensor menu - excludes NOT_BUILT items
void sortSensorMenu() {
  sensorMenuVisibleCount = 0;
  
  // First pass: collect only compiled-in sensors
  for (int i = 0; i < oledSensorMenuItemCount && sensorMenuVisibleCount < 20; i++) {
    MenuAvailability avail = getMenuAvailability(oledSensorMenuItems[i].targetMode, nullptr);
    if (avail != MenuAvailability::NOT_BUILT) {
      sensorMenuSortedIndices[sensorMenuVisibleCount++] = i;
    }
  }
  
  // Second pass: bubble sort by availability priority
  for (int i = 0; i < sensorMenuVisibleCount - 1; i++) {
    for (int j = 0; j < sensorMenuVisibleCount - i - 1; j++) {
      int idxA = sensorMenuSortedIndices[j];
      int idxB = sensorMenuSortedIndices[j + 1];
      
      MenuAvailability availA = getMenuAvailability(oledSensorMenuItems[idxA].targetMode, nullptr);
      MenuAvailability availB = getMenuAvailability(oledSensorMenuItems[idxB].targetMode, nullptr);
      
      if (getAvailabilitySortPriority(availA) > getAvailabilitySortPriority(availB)) {
        int temp = sensorMenuSortedIndices[j];
        sensorMenuSortedIndices[j] = sensorMenuSortedIndices[j + 1];
        sensorMenuSortedIndices[j + 1] = temp;
      }
    }
  }
  
  sensorMenuSorted = true;
}

// Get number of visible (compiled-in) sensor menu items
int getSensorMenuVisibleCount() {
  if (!sensorMenuSorted) sortSensorMenu();
  return sensorMenuVisibleCount;
}

// Get the actual menu item index from display position
int getSensorMenuActualIndex(int displayIndex) {
  if (!sensorMenuSorted) sortSensorMenu();
  if (displayIndex < 0 || displayIndex >= sensorMenuVisibleCount) return 0;
  return sensorMenuSortedIndices[displayIndex];
}

// Force re-sort on next display (call when availability might have changed)
void invalidateSensorMenuSort() {
  sensorMenuSorted = false;
}

// ============================================================================
// Main Menu Display (List Style - Only Option)
// ============================================================================

// Forward declare category arrays from OLED_Utils.cpp
extern const OLEDMenuItem oledMenuCategory0[], oledMenuCategory1[], oledMenuCategory2[];
extern const OLEDMenuItem oledMenuCategory3[], oledMenuCategory4[], oledMenuCategory5[];
extern const int oledMenuCategory0Count, oledMenuCategory1Count, oledMenuCategory2Count;
extern const int oledMenuCategory3Count, oledMenuCategory4Count, oledMenuCategory5Count;

// Helper to get category items and count
void getCategoryItems(int categoryId, const OLEDMenuItem** outItems, int* outCount) {
  switch (categoryId) {
    case 0: *outItems = oledMenuCategory0; *outCount = oledMenuCategory0Count; break;
    case 1: *outItems = oledMenuCategory1; *outCount = oledMenuCategory1Count; break;
    case 2: *outItems = oledMenuCategory2; *outCount = oledMenuCategory2Count; break;
    case 3: *outItems = oledMenuCategory3; *outCount = oledMenuCategory3Count; break;
    case 4: *outItems = oledMenuCategory4; *outCount = oledMenuCategory4Count; break;
    case 5: *outItems = oledMenuCategory5; *outCount = oledMenuCategory5Count; break;
    default: *outItems = nullptr; *outCount = 0; break;
  }
}

void displayMenuListStyle() {
  if (!oledDisplay || !oledConnected) return;
  
  // Check if remote command keyboard input is active
  extern bool isRemoteCommandInputActive();
  if (isRemoteCommandInputActive()) {
    // Display keyboard for command input
    oledKeyboardDisplay(oledDisplay);
    return;
  }
  
  // Layout constants - adjusted for global header
  const int listWidth = 68;       // Width for text list area
  const int iconAreaX = 78;       // X position for icon area (after separator)
  const int iconSize = 32;        // Full size icon for selected item
  const int itemHeight = 10;      // Height per menu item (text only)
  const int maxVisibleItems = 4;  // Items visible at once (fits in content area)
  const int startY = OLED_CONTENT_START_Y + 1;  // Start 1px below header line for even spacing
  
  // Update battery icon state if needed (every 2 minutes)
  unsigned long now = millis();
  if (!batteryIconState.valid || (now - batteryIconState.lastUpdateMs >= BATTERY_ICON_UPDATE_INTERVAL)) {
    batteryIconState.percentage = getBatteryPercentage();
    batteryIconState.icon = getBatteryIcon();
    batteryIconState.lastUpdateMs = now;
    batteryIconState.valid = true;
  }
  
  // Category menu state
  extern int oledMenuCategorySelected;
  extern int oledMenuCategoryItemIndex;
  extern const OLEDMenuItem oledMenuCategories[];
  extern const int oledMenuCategoryCount;
  
  // Check if we're in a remote submenu (takes priority over category menu)
  bool inRemoteSubmenu = isInRemoteSubmenu();
  
  // Determine what to display
  const OLEDMenuItem* menuItems = nullptr;
  int menuCount = 0;
  int selectedIndex = 0;
  bool inCategorySubmenu = false;
  
  if (inRemoteSubmenu) {
    // Remote submenu mode
    menuItems = (const OLEDMenuItem*)getRemoteSubmenuItems();
    menuCount = getRemoteSubmenuItemCount();
    selectedIndex = getRemoteSubmenuSelection();
  } else if (oledMenuCategorySelected >= 0) {
    // Category submenu mode
    getCategoryItems(oledMenuCategorySelected, &menuItems, &menuCount);
    selectedIndex = oledMenuCategoryItemIndex;
    inCategorySubmenu = true;
  } else {
    // Top-level category menu
    menuItems = oledMenuCategories;
    menuCount = oledMenuCategoryCount;
    selectedIndex = oledMenuSelectedIndex;
  }
  
  // Header is now drawn globally
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Draw vertical separator between list and icon
  oledDisplay->drawFastVLine(74, OLED_CONTENT_START_Y, OLED_CONTENT_HEIGHT, DISPLAY_COLOR_WHITE);
  
  // Calculate scroll offset to keep selected item visible
  int scrollOffset = 0;
  if (selectedIndex >= maxVisibleItems) {
    scrollOffset = selectedIndex - maxVisibleItems + 1;
  }
  
  // Draw menu items (text list on left)
  for (int i = 0; i < maxVisibleItems && (scrollOffset + i) < menuCount; i++) {
    int idx = scrollOffset + i;
    int y = startY + i * itemHeight;
    
    const OLEDMenuItem& item = menuItems[idx];
    bool isSelected = (idx == selectedIndex);
    
    if (isSelected) {
      // Highlight selected item with inverse (1px shorter to create gap with arrows)
      // Start at y (not y-1) to maintain 1px gap from header line
      oledDisplay->fillRect(0, y, listWidth - 1, itemHeight - 1, DISPLAY_COLOR_WHITE);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    // Position text 1px down to align with highlight box
    oledDisplay->setCursor(2, y + 1);
    
    // Show ">" suffix for category items (top-level menu)
    if (!inRemoteSubmenu && !inCategorySubmenu) {
      oledDisplay->print(item.name);
      oledDisplay->print(" >");
    } else {
      // Show "R " prefix for remote items in remote submenu
      if (inRemoteSubmenu) {
        OLEDMenuItemEx* remoteItem = (OLEDMenuItemEx*)&item;
        if (remoteItem->isRemote) {
          oledDisplay->print("R ");
        }
      }
      oledDisplay->print(item.name);
    }
  }
  
  // Reset text color
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Get selected item (bounds check)
  if (menuCount == 0) return;
  const OLEDMenuItem& selectedItem = menuItems[selectedIndex];
  
  // Draw selected item's icon on the right (centered in icon area)
  const int availableIconHeight = OLED_CONTENT_HEIGHT;
  int iconX = iconAreaX + (128 - iconAreaX - iconSize) / 2;
  int iconY = OLED_CONTENT_START_Y + (availableIconHeight - iconSize) / 2;
  drawIcon(oledDisplay, selectedItem.iconName, iconX, iconY, DISPLAY_COLOR_WHITE);
  
  // Draw availability/remote indicator in top-left corner of icon area (only for actual items, not categories)
  if (inCategorySubmenu || inRemoteSubmenu) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(iconAreaX + 2, OLED_CONTENT_START_Y);
    
    if (inRemoteSubmenu) {
      OLEDMenuItemEx* remoteItem = (OLEDMenuItemEx*)&selectedItem;
      if (remoteItem->isRemote) {
        oledDisplay->print("R");  // Remote item
      } else {
        MenuAvailability availability = getMenuAvailability(selectedItem.targetMode, nullptr);
        if (availability != MenuAvailability::AVAILABLE) {
          if (availability == MenuAvailability::FEATURE_DISABLED) {
            oledDisplay->print("D");  // Disabled
          } else {
            oledDisplay->print("X");  // Not available
          }
        }
      }
    } else {
      // Category submenu - check availability
      MenuAvailability availability = getMenuAvailability(selectedItem.targetMode, nullptr);
      if (availability != MenuAvailability::AVAILABLE) {
        if (availability == MenuAvailability::FEATURE_DISABLED) {
          oledDisplay->print("D");  // Disabled
        } else {
          oledDisplay->print("X");  // Not available
        }
      }
    }
    
    // Show status text below icon
    int textY = iconY + iconSize + 2;
    if (textY + 8 <= OLED_CONTENT_HEIGHT) {
      oledDisplay->setTextSize(1);
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      oledDisplay->setCursor(iconAreaX + 2, textY);
      
      if (inRemoteSubmenu) {
        OLEDMenuItemEx* remoteItem = (OLEDMenuItemEx*)&selectedItem;
        if (remoteItem->isRemote) {
          oledDisplay->print("Remote");
        } else {
          MenuAvailability avail = getMenuAvailability(selectedItem.targetMode, nullptr);
          if (avail == MenuAvailability::AVAILABLE) {
            oledDisplay->print("Ready");
          } else if (avail == MenuAvailability::FEATURE_DISABLED) {
            oledDisplay->print("Off");
          } else if (avail == MenuAvailability::NOT_DETECTED) {
            oledDisplay->print("No HW");
          } else if (avail == MenuAvailability::NOT_BUILT) {
            oledDisplay->print("N/A");
          }
        }
      } else {
        // Category submenu
        MenuAvailability avail = getMenuAvailability(selectedItem.targetMode, nullptr);
        if (avail == MenuAvailability::AVAILABLE) {
          oledDisplay->print("Ready");
        } else if (avail == MenuAvailability::FEATURE_DISABLED) {
          oledDisplay->print("Off");
        } else if (avail == MenuAvailability::NOT_DETECTED) {
          oledDisplay->print("No HW");
        } else if (avail == MenuAvailability::NOT_BUILT) {
          oledDisplay->print("N/A");
        }
      }
    }
  }
  
  // Draw scroll indicators if needed (must stay within content area)
  if (scrollOffset > 0) {
    oledDisplay->setCursor(68, OLED_CONTENT_START_Y);
    oledDisplay->print("\x18");  // Up arrow
  }
  if (scrollOffset + maxVisibleItems < menuCount) {
    int scrollDownY = OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 9;
    oledDisplay->setCursor(68, scrollDownY);
    oledDisplay->print("\x19");  // Down arrow
  }
  
  // Note: Navigation hints now handled by global footer
}

// ============================================================================
// Sensor Submenu Display
// ============================================================================

void displaySensorMenu() {
  if (!oledDisplay || !oledConnected) return;
  
  // Ensure menu is sorted by availability
  if (!sensorMenuSorted) sortSensorMenu();
  
  // Layout constants (matching main menu style) - adjusted for global header
  const int listWidth = 78;
  const int iconAreaX = 88;
  const int iconSize = 32;
  const int itemHeight = 10;
  const int maxVisibleItems = 4;
  const int startY = OLED_CONTENT_START_Y + 1;  // Start 1px below header for even spacing
  
  // Header is now drawn globally
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Draw vertical separator between list and icon
  oledDisplay->drawFastVLine(84, OLED_CONTENT_START_Y, OLED_CONTENT_HEIGHT, DISPLAY_COLOR_WHITE);
  
  // Clamp selected index to visible count
  if (oledSensorMenuSelectedIndex >= sensorMenuVisibleCount) {
    oledSensorMenuSelectedIndex = sensorMenuVisibleCount > 0 ? sensorMenuVisibleCount - 1 : 0;
  }
  
  // Calculate scroll offset to keep selected item visible
  int scrollOffset = 0;
  if (oledSensorMenuSelectedIndex >= maxVisibleItems) {
    scrollOffset = oledSensorMenuSelectedIndex - maxVisibleItems + 1;
  }
  
  // Draw menu items (text list on left) - using filtered/sorted indices
  for (int i = 0; i < maxVisibleItems && (scrollOffset + i) < sensorMenuVisibleCount; i++) {
    int displayIdx = scrollOffset + i;
    int actualIdx = sensorMenuSortedIndices[displayIdx];  // Map to actual item
    int y = startY + i * itemHeight;
    
    bool isSelected = (displayIdx == oledSensorMenuSelectedIndex);
    
    if (isSelected) {
      oledDisplay->fillRect(0, y, listWidth, itemHeight - 1, DISPLAY_COLOR_WHITE);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    oledDisplay->setCursor(2, y + 1);
    oledDisplay->print(oledSensorMenuItems[actualIdx].name);
  }
  
  // Reset text color
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Get actual item index for selected display position
  int selectedActualIdx = sensorMenuSortedIndices[oledSensorMenuSelectedIndex];
  
  // Draw selected item's icon on the right
  const int availableIconHeight = OLED_CONTENT_HEIGHT;
  int iconX = iconAreaX + (128 - iconAreaX - iconSize) / 2;
  int iconY = OLED_CONTENT_START_Y + (availableIconHeight - iconSize) / 2;
  drawIcon(oledDisplay, oledSensorMenuItems[selectedActualIdx].iconName, iconX, iconY, DISPLAY_COLOR_WHITE);
  
  // Draw availability indicator
  MenuAvailability availability = getMenuAvailability(oledSensorMenuItems[selectedActualIdx].targetMode, nullptr);
  if (availability != MenuAvailability::AVAILABLE) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(iconAreaX + 2, OLED_CONTENT_START_Y);
    if (availability == MenuAvailability::FEATURE_DISABLED) {
      oledDisplay->print("D");
    } else {
      oledDisplay->print("X");
    }
  }
  
  // Show availability status text below icon
  int textY = iconY + iconSize + 2;
  if (textY + 8 <= OLED_CONTENT_HEIGHT) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(iconAreaX + 2, textY);
    
    if (availability == MenuAvailability::AVAILABLE) {
      oledDisplay->print("Ready");
    } else if (availability == MenuAvailability::FEATURE_DISABLED) {
      oledDisplay->print("Off");
    } else if (availability == MenuAvailability::NOT_DETECTED) {
      oledDisplay->print("No HW");
    } else if (availability == MenuAvailability::NOT_BUILT) {
      oledDisplay->print("N/A");
    }
  }
  
  // Draw scroll indicators if needed
  if (scrollOffset > 0) {
    oledDisplay->setCursor(78, OLED_CONTENT_START_Y);
    oledDisplay->print("\x18");  // Up arrow
  }
  if (scrollOffset + maxVisibleItems < sensorMenuVisibleCount) {
    int scrollDownY = OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 9;
    oledDisplay->setCursor(78, scrollDownY);
    oledDisplay->print("\x19");  // Down arrow
  }
}

// ============================================================================
// Sensor Menu Input Handler (registered via OLEDModeEntry)
// ============================================================================

static bool sensorMenuInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  int visibleCount = getSensorMenuVisibleCount();
  
  extern NavEvents gNavEvents;
  if (gNavEvents.right || gNavEvents.down) {
    oledSensorMenuSelectedIndex = (oledSensorMenuSelectedIndex + 1) % visibleCount;
    return true;
  } else if (gNavEvents.left || gNavEvents.up) {
    oledSensorMenuSelectedIndex = (oledSensorMenuSelectedIndex - 1 + visibleCount) % visibleCount;
    return true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (oledSensorMenuSelectedIndex >= 0 && oledSensorMenuSelectedIndex < visibleCount) {
      int actualIdx = getSensorMenuActualIndex(oledSensorMenuSelectedIndex);
      OLEDMode target = oledSensorMenuItems[actualIdx].targetMode;
      String reason;
      MenuAvailability availability = getMenuAvailability(target, &reason);
      if (availability != MenuAvailability::AVAILABLE) {
        pushOLEDMode(OLED_SENSOR_MENU);
        enterUnavailablePage(oledSensorMenuItems[actualIdx].name, reason);
      } else {
        pushOLEDMode(OLED_SENSOR_MENU);
        setOLEDMode(target);
      }
    }
    return true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    // Return false to let global handler call oledMenuBack() which properly
    // pops mode stack and preserves category submenu state
    return false;
  }
  
  return false;
}

static const OLEDModeEntry sSensorMenuModes[] = {
  { OLED_SENSOR_MENU, "Sensors", "sensor", displaySensorMenu, nullptr, sensorMenuInputHandler, false, -1 },
};

REGISTER_OLED_MODE_MODULE(sSensorMenuModes, sizeof(sSensorMenuModes) / sizeof(sSensorMenuModes[0]), "SensorMenu");

// ============================================================================
// Automations Display - Full implementation in OLED_Mode_Automations.cpp
// ============================================================================

// ============================================================================
// Logo Display (with 3D animated device)
// ============================================================================

extern void rotateCubePoint(float& x, float& y, float& z, float angleX, float angleY, float angleZ);
extern void projectCubePoint(float x, float y, float z, int& screenX, int& screenY, int centerX, int centerY);

void displayLogo() {
  // Text on the left
  oledDisplay->setTextSize(2);
  oledDisplay->setCursor(0, 10);
  oledDisplay->println("Hardware");
  oledDisplay->println("  One");
  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, 44);
  oledDisplay->println("v0.9");

  // Animated 3D device model on the right
  static unsigned long animStartTime = 0;
  if (animStartTime == 0) animStartTime = millis();

  unsigned long elapsed = millis() - animStartTime;
  float animProgress = (elapsed % 4000) / 4000.0;

  float angleY = sin(animProgress * 2 * PI) * 0.25;
  float angleX = 0.15;
  float angleZ = 0;

  const int deviceX = 112;
  const int deviceY = 32;
  const float width = 12.5;
  const float height = 25.0;
  const float depth = 5.0;

  float vertices[8][3] = {
    { -width, -height, -depth },
    { width, -height, -depth },
    { width, height, -depth },
    { -width, height, -depth },
    { -width, -height, depth },
    { width, -height, depth },
    { width, height, depth },
    { -width, height, depth }
  };

  int projected[8][2];
  float rotated[8][3];
  for (int i = 0; i < 8; i++) {
    float x = vertices[i][0];
    float y = vertices[i][1];
    float z = vertices[i][2];

    rotateCubePoint(x, y, z, angleX, angleY, angleZ);
    rotated[i][0] = x;
    rotated[i][1] = y;
    rotated[i][2] = z;
    projectCubePoint(x, y, z, projected[i][0], projected[i][1], deviceX, deviceY);
  }

  // Face visibility helper
  auto isFaceVisible = [&](int v0, int v1, int v2) -> bool {
    float edge1[3] = {
      rotated[v1][0] - rotated[v0][0],
      rotated[v1][1] - rotated[v0][1],
      rotated[v1][2] - rotated[v0][2]
    };
    float edge2[3] = {
      rotated[v2][0] - rotated[v0][0],
      rotated[v2][1] - rotated[v0][1],
      rotated[v2][2] - rotated[v0][2]
    };

    float normal[3] = {
      edge1[1] * edge2[2] - edge1[2] * edge2[1],
      edge1[2] * edge2[0] - edge1[0] * edge2[2],
      edge1[0] * edge2[1] - edge1[1] * edge2[0]
    };

    return normal[2] > 0;
  };

  // Draw visible faces
  if (isFaceVisible(0, 1, 5)) {
    oledDisplay->drawLine(projected[0][0], projected[0][1], projected[1][0], projected[1][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[1][0], projected[1][1], projected[5][0], projected[5][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[5][0], projected[5][1], projected[4][0], projected[4][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[4][0], projected[4][1], projected[0][0], projected[0][1], DISPLAY_COLOR_WHITE);
  }

  if (isFaceVisible(3, 7, 6)) {
    oledDisplay->drawLine(projected[3][0], projected[3][1], projected[2][0], projected[2][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[2][0], projected[2][1], projected[6][0], projected[6][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[6][0], projected[6][1], projected[7][0], projected[7][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[7][0], projected[7][1], projected[3][0], projected[3][1], DISPLAY_COLOR_WHITE);
  }

  if (isFaceVisible(4, 5, 6)) {
    oledDisplay->drawLine(projected[4][0], projected[4][1], projected[5][0], projected[5][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[5][0], projected[5][1], projected[6][0], projected[6][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[6][0], projected[6][1], projected[7][0], projected[7][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[7][0], projected[7][1], projected[4][0], projected[4][1], DISPLAY_COLOR_WHITE);
  }

  if (isFaceVisible(0, 3, 2)) {
    oledDisplay->drawLine(projected[0][0], projected[0][1], projected[1][0], projected[1][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[1][0], projected[1][1], projected[2][0], projected[2][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[2][0], projected[2][1], projected[3][0], projected[3][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[3][0], projected[3][1], projected[0][0], projected[0][1], DISPLAY_COLOR_WHITE);
  }

  if (isFaceVisible(0, 4, 7)) {
    oledDisplay->drawLine(projected[0][0], projected[0][1], projected[4][0], projected[4][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[4][0], projected[4][1], projected[7][0], projected[7][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[7][0], projected[7][1], projected[3][0], projected[3][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[3][0], projected[3][1], projected[0][0], projected[0][1], DISPLAY_COLOR_WHITE);
  }

  if (isFaceVisible(1, 2, 6)) {
    oledDisplay->drawLine(projected[1][0], projected[1][1], projected[5][0], projected[5][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[5][0], projected[5][1], projected[6][0], projected[6][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[6][0], projected[6][1], projected[2][0], projected[2][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[2][0], projected[2][1], projected[1][0], projected[1][1], DISPLAY_COLOR_WHITE);
  }

  // Add front panel details when visible
  float frontZ = depth * cos(angleY) * cos(angleX);
  float frontVisibility = cos(angleY);

  if (frontZ > 0 && frontVisibility > 0.7) {
    float screenVerts[4][3] = {
      { -width * 0.7f, -height * 0.9f, depth },
      { width * 0.7f, -height * 0.9f, depth },
      { width * 0.7f, -height * 0.5f, depth },
      { -width * 0.7f, -height * 0.5f, depth }
    };

    int screenProj[4][2];
    for (int i = 0; i < 4; i++) {
      float x = screenVerts[i][0];
      float y = screenVerts[i][1];
      float z = screenVerts[i][2];
      rotateCubePoint(x, y, z, angleX, angleY, angleZ);
      projectCubePoint(x, y, z, screenProj[i][0], screenProj[i][1], deviceX, deviceY);
    }

    oledDisplay->drawLine(screenProj[0][0], screenProj[0][1], screenProj[1][0], screenProj[1][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(screenProj[1][0], screenProj[1][1], screenProj[2][0], screenProj[2][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(screenProj[2][0], screenProj[2][1], screenProj[3][0], screenProj[3][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(screenProj[3][0], screenProj[3][1], screenProj[0][0], screenProj[0][1], DISPLAY_COLOR_WHITE);

    // ToF sensor
    float tofX = -width * 0.4f;
    float tofY = height * 0.125f;
    float tofZ = depth;
    rotateCubePoint(tofX, tofY, tofZ, angleX, angleY, angleZ);
    int tofScreenX, tofScreenY;
    projectCubePoint(tofX, tofY, tofZ, tofScreenX, tofScreenY, deviceX, deviceY);
    oledDisplay->fillRect(tofScreenX - 2, tofScreenY - 1, 5, 3, DISPLAY_COLOR_BLACK);
    oledDisplay->drawRect(tofScreenX - 2, tofScreenY - 1, 5, 3, DISPLAY_COLOR_WHITE);

    // Thermal IR sensor
    float irX = width * 0.3f;
    float irY = height * 0.125f;
    float irZ = depth;
    rotateCubePoint(irX, irY, irZ, angleX, angleY, angleZ);
    int irScreenX, irScreenY;
    projectCubePoint(irX, irY, irZ, irScreenX, irScreenY, deviceX, deviceY);
    oledDisplay->fillCircle(irScreenX, irScreenY, 3, DISPLAY_COLOR_BLACK);
    oledDisplay->drawCircle(irScreenX, irScreenY, 3, DISPLAY_COLOR_WHITE);
  }
}

#endif // ENABLE_OLED_DISPLAY
