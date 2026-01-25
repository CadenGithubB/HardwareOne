// OLED_Mode_Menu.cpp - Menu display functions
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "System_Battery.h"
#include "System_Icons.h"
#include "System_Settings.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

extern DisplayDriver* oledDisplay;
extern bool oledConnected;
extern OLEDMode currentOLEDMode;
extern Settings gSettings;

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
// Main Menu Display (Grid Style)
// ============================================================================

void displayMenu() {
  if (!oledDisplay || !oledConnected) return;
  
  // Update battery icon state if needed (every 2 minutes)
  unsigned long now = millis();
  if (!batteryIconState.valid || (now - batteryIconState.lastUpdateMs >= BATTERY_ICON_UPDATE_INTERVAL)) {
    batteryIconState.percentage = getBatteryPercentage();
    batteryIconState.icon = getBatteryIcon();
    batteryIconState.lastUpdateMs = now;
    batteryIconState.valid = true;
  }
  
  // Menu layout constants - constrained to OLED_CONTENT_HEIGHT (54px)
  const int iconSize = 16;      // Display size for icons (scaled down from 32)
  const int itemWidth = 42;     // Width per menu item
  const int itemHeight = 23;    // Height per menu item (icon + label) - fits 2 rows in 54px
  const int cols = 3;           // Items per row
  const int startX = 2;
  const int startY = 8;
  const int labelOffsetY = 16;  // Y offset for label below icon
  
  // Draw header
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(35, 0);
  oledDisplay->print("MENU");
  
  // Draw battery icon in top-right corner (icon anchored at right edge, % to left)
  if (gBatteryState.status == BATTERY_NOT_PRESENT) {
    oledDisplay->setCursor(104, 0);  // "USB" + icon = 24px
    oledDisplay->print("USB");
  } else {
    int pct = (int)batteryIconState.percentage;
    int pctWidth = (pct >= 100) ? 24 : (pct >= 10) ? 18 : 12;  // 4/3/2 chars * 6px
    oledDisplay->setCursor(122 - pctWidth, 0);
    oledDisplay->print(pct);
    oledDisplay->print("%");
  }
  oledDisplay->setCursor(122, 0);
  oledDisplay->print(batteryIconState.icon);
  
  // Calculate visible page (show 6 items at a time - 2 rows x 3 cols)
  int itemsPerPage = 6;
  int pageStart = (oledMenuSelectedIndex / itemsPerPage) * itemsPerPage;
  
  // Draw menu items
  for (int i = 0; i < itemsPerPage && (pageStart + i) < oledMenuItemCount; i++) {
    int idx = pageStart + i;
    int col = i % cols;
    int row = i / cols;
    
    int x = startX + col * itemWidth;
    int y = startY + row * itemHeight;
    
    // Highlight selected item
    bool isSelected = (idx == oledMenuSelectedIndex);
    if (isSelected) {
      oledDisplay->drawRect(x - 1, y - 1, itemWidth - 2, itemHeight, DISPLAY_COLOR_WHITE);
    }
    
    // Draw icon (centered in item area)
    int iconX = x + (itemWidth - iconSize) / 2 - 4;
    int iconY = y + 1;
    
    // Draw scaled-down icon using bitmap (0.5x scale: 32→16)
    drawIconScaled(oledDisplay, oledMenuItems[idx].iconName, iconX, iconY, DISPLAY_COLOR_WHITE, 0.5f);

    // Show availability indicator overlay (simplified for small icons)
    MenuAvailability availability = getMenuAvailability(oledMenuItems[idx].targetMode, nullptr);
    if (availability != MenuAvailability::AVAILABLE) {
      // Just show X for anything not available (simplified for small icons)
      oledDisplay->drawLine(iconX, iconY, iconX + iconSize - 1, iconY + iconSize - 1, DISPLAY_COLOR_WHITE);
      oledDisplay->drawLine(iconX + iconSize - 1, iconY, iconX, iconY + iconSize - 1, DISPLAY_COLOR_WHITE);
    }
    
    // Draw label (truncated to fit)
    oledDisplay->setCursor(x + 2, y + labelOffsetY);
    String label = oledMenuItems[idx].name;
    if (label.length() > 6) {
      label = label.substring(0, 5) + ".";
    }
    oledDisplay->print(label);
  }
  
  // Draw scroll bar if multiple pages
  int totalPages = (oledMenuItemCount + itemsPerPage - 1) / itemsPerPage;
  if (totalPages > 1) {
    // Scroll bar on right side
    const int scrollBarX = 126;
    const int scrollBarHeight = OLED_CONTENT_HEIGHT - 8;  // Height from startY to footer
    const int scrollBarY = 8;
    
    // Calculate thumb position and size
    int currentPage = oledMenuSelectedIndex / itemsPerPage;
    int thumbHeight = max(2, scrollBarHeight / totalPages);
    int thumbY = scrollBarY + (currentPage * (scrollBarHeight - thumbHeight) / (totalPages - 1));
    
    // Draw scroll bar background
    oledDisplay->drawFastVLine(scrollBarX, scrollBarY, scrollBarHeight, DISPLAY_COLOR_WHITE);
    // Clear thumb area (draw black)
    oledDisplay->drawFastVLine(scrollBarX, thumbY, thumbHeight, DISPLAY_COLOR_BLACK);
    // Draw thumb
    oledDisplay->drawFastVLine(scrollBarX, thumbY, thumbHeight, DISPLAY_COLOR_WHITE);
  }
  
  // Note: Navigation hints now handled by global footer
}

// ============================================================================
// Main Menu Display (List Style)
// ============================================================================

void displayMenuListStyle() {
  if (!oledDisplay || !oledConnected) return;
  
  // Layout constants
  const int listWidth = 68;       // Width for text list area
  const int iconAreaX = 78;       // X position for icon area (after separator)
  const int iconSize = 32;        // Full size icon for selected item
  const int itemHeight = 10;      // Height per menu item (text only)
  const int maxVisibleItems = 4;  // Items visible at once (reduced to fit in content area)
  const int startY = 10;          // Start Y after header
  
  // Update battery icon state if needed (every 2 minutes)
  unsigned long now = millis();
  if (!batteryIconState.valid || (now - batteryIconState.lastUpdateMs >= BATTERY_ICON_UPDATE_INTERVAL)) {
    batteryIconState.percentage = getBatteryPercentage();
    batteryIconState.icon = getBatteryIcon();
    batteryIconState.lastUpdateMs = now;
    batteryIconState.valid = true;
  }
  
  // Draw header
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, 0);
  oledDisplay->print("MENU");
  
  // Draw battery icon in top-right corner (icon anchored at right edge, % to left)
  if (gBatteryState.status == BATTERY_NOT_PRESENT) {
    oledDisplay->setCursor(104, 0);  // "USB" + icon = 24px
    oledDisplay->print("USB");
  } else {
    int pct2 = (int)batteryIconState.percentage;
    int pctWidth2 = (pct2 >= 100) ? 24 : (pct2 >= 10) ? 18 : 12;  // 4/3/2 chars * 6px
    oledDisplay->setCursor(122 - pctWidth2, 0);
    oledDisplay->print(pct2);
    oledDisplay->print("%");
  }
  oledDisplay->setCursor(122, 0);
  oledDisplay->print(batteryIconState.icon);
  
  // Draw vertical separator between list and icon
  oledDisplay->drawFastVLine(74, 8, OLED_CONTENT_HEIGHT - 8, DISPLAY_COLOR_WHITE);  // Respect content area boundary
  
  // Calculate scroll offset to keep selected item visible
  int scrollOffset = 0;
  if (oledMenuSelectedIndex >= maxVisibleItems) {
    scrollOffset = oledMenuSelectedIndex - maxVisibleItems + 1;
  }
  
  // Draw menu items (text list on left)
  for (int i = 0; i < maxVisibleItems && (scrollOffset + i) < oledMenuItemCount; i++) {
    int idx = scrollOffset + i;
    int y = startY + i * itemHeight;
    
    bool isSelected = (idx == oledMenuSelectedIndex);
    
    if (isSelected) {
      // Highlight selected item with inverse
      oledDisplay->fillRect(0, y - 1, listWidth, itemHeight, DISPLAY_COLOR_WHITE);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    oledDisplay->setCursor(2, y);
    oledDisplay->print(oledMenuItems[idx].name);
  }
  
  // Reset text color
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Draw selected item's icon on the right (centered in icon area)
  // Icon area: x=78 to x=128 (50px wide), y=10 to OLED_CONTENT_HEIGHT
  // Center 32x32 icon in this area, leaving room for status text
  const int availableIconHeight = OLED_CONTENT_HEIGHT - 10;  // Height from startY to footer
  int iconX = iconAreaX + (128 - iconAreaX - iconSize) / 2;  // 78 + (50-32)/2 = 78 + 9 = 87
  int iconY = 10 + (availableIconHeight - iconSize - 10) / 2;  // Center icon with room for text below
  drawIcon(oledDisplay, oledMenuItems[oledMenuSelectedIndex].iconName, iconX, iconY, DISPLAY_COLOR_WHITE);
  
  // Draw availability indicator in top-left corner of icon area
  MenuAvailability availability = getMenuAvailability(oledMenuItems[oledMenuSelectedIndex].targetMode, nullptr);
  if (availability != MenuAvailability::AVAILABLE) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    // Position in top-left corner of icon area (just after vertical separator)
    oledDisplay->setCursor(iconAreaX + 2, 10);
    if (availability == MenuAvailability::FEATURE_DISABLED) {
      oledDisplay->print("D");  // Disabled
    } else {
      oledDisplay->print("X");  // Not available (not detected or not built)
    }
  }
  
  // Show availability status text below icon (ensure it stays within content area)
  int textY = iconY + iconSize + 2;
  if (textY + 8 <= OLED_CONTENT_HEIGHT) {  // Only draw if it fits in content area
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(iconAreaX + 2, textY);  // Align with icon area
    
    // Don't show "Ready" for always-available systems
    bool alwaysAvailable = (oledMenuItems[oledMenuSelectedIndex].targetMode == OLED_MEMORY_STATS ||
                           oledMenuItems[oledMenuSelectedIndex].targetMode == OLED_SENSOR_DATA ||
                           oledMenuItems[oledMenuSelectedIndex].targetMode == OLED_SYSTEM_STATUS ||
                           oledMenuItems[oledMenuSelectedIndex].targetMode == OLED_POWER ||
                           oledMenuItems[oledMenuSelectedIndex].targetMode == OLED_LOGO);
    
    if (availability == MenuAvailability::AVAILABLE && !alwaysAvailable) {
      oledDisplay->print("Ready");
    } else if (availability == MenuAvailability::FEATURE_DISABLED) {
      oledDisplay->print("Off");
    } else if (availability == MenuAvailability::NOT_DETECTED) {
      oledDisplay->print("No HW");
    } else if (availability == MenuAvailability::NOT_BUILT) {
      oledDisplay->print("N/A");
    } else if (!alwaysAvailable) {
      oledDisplay->print("N/A");
    }
    // Always-available systems show no status text
  }
  
  // Draw scroll indicators if needed (must stay within content area)
  if (scrollOffset > 0) {
    oledDisplay->setCursor(68, 10);
    oledDisplay->print("^");
  }
  if (scrollOffset + maxVisibleItems < oledMenuItemCount) {
    int scrollDownY = OLED_CONTENT_HEIGHT - 9;  // 9px from bottom of content area
    oledDisplay->setCursor(68, scrollDownY);
    oledDisplay->print("v");
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
  
  // Layout constants (matching main menu style)
  const int listWidth = 78;
  const int iconAreaX = 88;
  const int iconSize = 32;
  const int itemHeight = 10;
  const int maxVisibleItems = 4;
  const int startY = 10;
  
  // Draw header
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, 0);
  oledDisplay->print("SENSORS");
  
  // Draw vertical separator between list and icon
  oledDisplay->drawFastVLine(84, 8, OLED_CONTENT_HEIGHT - 8, DISPLAY_COLOR_WHITE);
  
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
      oledDisplay->fillRect(0, y - 1, listWidth, itemHeight, DISPLAY_COLOR_WHITE);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    oledDisplay->setCursor(2, y);
    oledDisplay->print(oledSensorMenuItems[actualIdx].name);
  }
  
  // Reset text color
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Get actual item index for selected display position
  int selectedActualIdx = sensorMenuSortedIndices[oledSensorMenuSelectedIndex];
  
  // Draw selected item's icon on the right
  const int availableIconHeight = OLED_CONTENT_HEIGHT - 10;
  int iconX = iconAreaX + (128 - iconAreaX - iconSize) / 2;
  int iconY = 10 + (availableIconHeight - iconSize - 10) / 2;
  drawIcon(oledDisplay, oledSensorMenuItems[selectedActualIdx].iconName, iconX, iconY, DISPLAY_COLOR_WHITE);
  
  // Draw availability indicator
  MenuAvailability availability = getMenuAvailability(oledSensorMenuItems[selectedActualIdx].targetMode, nullptr);
  if (availability != MenuAvailability::AVAILABLE) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(iconAreaX + 2, 10);
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
    oledDisplay->setCursor(78, 10);
    oledDisplay->print("^");
  }
  if (scrollOffset + maxVisibleItems < sensorMenuVisibleCount) {
    int scrollDownY = OLED_CONTENT_HEIGHT - 9;
    oledDisplay->setCursor(78, scrollDownY);
    oledDisplay->print("v");
  }
  
  // Draw page indicator
  String pageStr = String(oledSensorMenuSelectedIndex + 1) + "/" + String(sensorMenuVisibleCount);
  int charWidth = 6;
  int pageStrWidth = pageStr.length() * charWidth;
  oledDisplay->setCursor(128 - pageStrWidth, 0);
  oledDisplay->print(pageStr);
}

// ============================================================================
// Automations Display
// ============================================================================

#if ENABLE_AUTOMATION
void displayAutomations() {
  if (!gSettings.automationsEnabled) {
    enterUnavailablePage("Automations", "Disabled\nRun: automation system enable");
    return;
  }

  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, 0);
  oledDisplay->println("== AUTOMATIONS ==");
  oledDisplay->println();
  
  // TODO: Show automation count and recent activity
  oledDisplay->println("Automations active");
  // Note: Button hints now handled by global footer
}
#endif

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
  oledDisplay->println("v2.1");

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
