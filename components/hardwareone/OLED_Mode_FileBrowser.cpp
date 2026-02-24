// OLED_Mode_FileBrowser.cpp - File browser display mode
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "HAL_Input.h"
#include "System_FileManager.h"
#include "System_Icons.h"

#if ENABLE_GPS_SENSOR
#include "System_Maps.h"
#endif

// External references
extern bool oledConnected;
extern OLEDMode currentOLEDMode;
extern FileManager* gOLEDFileManager;
extern bool oledFileBrowserNeedsInit;
extern bool oledMenuBack();

// Icon functions (from System_Icons.cpp)
extern void drawIcon(Adafruit_SSD1306* display, const char* iconName, int x, int y, uint16_t color);
extern const char* getIconNameForExtension(const char* extension);
extern String formatFileSize(size_t bytes);

// ============================================================================
// File Browser State
// ============================================================================

// Pending action enum for deferred filesystem operations
enum class FileBrowserPendingAction {
  NONE,
  NAVIGATE_INTO,
  NAVIGATE_UP,
  NAVIGATE_BACK
};
static FileBrowserPendingAction fileBrowserPendingAction = FileBrowserPendingAction::NONE;

// Pre-rendered file browser data to avoid filesystem I/O inside I2C transaction
struct FileBrowserRenderData {
  char path[FILE_MANAGER_MAX_PATH];
  FileEntry items[FILE_MANAGER_PAGE_SIZE];
  int itemCount;
  int selectedIdx;
  int pageStart;
  int pageEnd;
  bool valid;
  bool selectedIsFolder;  // Track if selected item is a folder for footer hints
};
FileBrowserRenderData fileBrowserRenderData = {0};  // Non-static so footer can access it

// Button/navigation state
static unsigned long oledFileBrowserLastInput = 0;
static const unsigned long OLED_FILE_BROWSER_DEBOUNCE = 200; // ms

// ============================================================================
// File Browser Initialization
// ============================================================================

/**
 * Initialize file browser
 */
static bool initFileBrowser() {
  if (gOLEDFileManager == nullptr) {
    gOLEDFileManager = new FileManager();
    if (gOLEDFileManager == nullptr) {
      return false;
    }
  }
  
  // Start at root
  gOLEDFileManager->navigate("/");
  oledFileBrowserNeedsInit = false;
  return true;
}

// ============================================================================
// File Browser Rendered (two-phase rendering)
// ============================================================================

/**
 * Render file browser to OLED display
 * Compact layout for 128x64 screen:
 * - Line 0-9: Path (truncated)
 * - Line 10-53: File list (5 items, 9px each)
 * - Line 54-63: Navigation hints
 */
// Gather file browser data (called OUTSIDE I2C transaction to avoid blocking gamepad)
void prepareFileBrowserData() {
  // Initialize or reinitialize if needed
  if (!gOLEDFileManager || oledFileBrowserNeedsInit) {
    if (!initFileBrowser()) {
      fileBrowserRenderData.valid = false;
      return;
    }
  }
  
  // Process pending navigation actions (filesystem I/O happens here, OUTSIDE I2C transaction)
  if (fileBrowserPendingAction != FileBrowserPendingAction::NONE) {
    switch (fileBrowserPendingAction) {
      case FileBrowserPendingAction::NAVIGATE_INTO: {
        FileEntry entry;
        if (gOLEDFileManager->getCurrentItem(entry)) {
          if (entry.isFolder) {
            gOLEDFileManager->navigateInto();
          } else {
#if ENABLE_GPS_SENSOR
            // Check if it's a .hwmap file
            String filename = String(entry.name);
            if (filename.endsWith(".hwmap")) {
              // Load the map and return to map mode
              String fullPath = String(gOLEDFileManager->getCurrentPath());
              if (!fullPath.endsWith("/")) fullPath += "/";
              fullPath += entry.name;
              
              if (MapCore::loadMapFile(fullPath.c_str())) {
                extern bool gMapCenterSet;
                extern bool gMapManuallyPanned;
                setOLEDMode(OLED_GPS_MAP);
                gMapCenterSet = false;  // Reset to new map's center
                gMapManuallyPanned = false;  // Allow GPS to track on new map
              }
            }
#endif
          }
        }
        break;
      }
      case FileBrowserPendingAction::NAVIGATE_UP:
        gOLEDFileManager->navigateUp();
        break;
      case FileBrowserPendingAction::NAVIGATE_BACK:
        if (strcmp(gOLEDFileManager->getCurrentPath(), "/") == 0) {
          fileBrowserPendingAction = FileBrowserPendingAction::NONE;
          fileBrowserRenderData.valid = false;
          oledMenuBack();
          return;
        } else {
          gOLEDFileManager->navigateUp();
        }
        break;
      default:
        break;
    }
    fileBrowserPendingAction = FileBrowserPendingAction::NONE;
  }
  
  // Gather all data needed for rendering
  strncpy(fileBrowserRenderData.path, gOLEDFileManager->getCurrentPath(), FILE_MANAGER_MAX_PATH - 1);
  fileBrowserRenderData.path[FILE_MANAGER_MAX_PATH - 1] = '\0';
  fileBrowserRenderData.itemCount = gOLEDFileManager->getItemCount();
  fileBrowserRenderData.selectedIdx = gOLEDFileManager->getSelectedIndex();
  fileBrowserRenderData.pageStart = gOLEDFileManager->getPageStart();
  fileBrowserRenderData.pageEnd = gOLEDFileManager->getPageEnd();
  
  // Pre-fetch all visible items (filesystem I/O happens here)
  int itemsFetched = 0;
  for (int i = fileBrowserRenderData.pageStart; i < fileBrowserRenderData.pageEnd && i < fileBrowserRenderData.itemCount && itemsFetched < FILE_MANAGER_PAGE_SIZE; i++) {
    if (gOLEDFileManager->getItem(i, fileBrowserRenderData.items[itemsFetched])) {
      itemsFetched++;
    }
  }
  
  // Determine if selected item is a folder (for footer hints)
  fileBrowserRenderData.selectedIsFolder = false;
  if (fileBrowserRenderData.itemCount > 0) {
    int selectedItemIdx = fileBrowserRenderData.selectedIdx - fileBrowserRenderData.pageStart;
    if (selectedItemIdx >= 0 && selectedItemIdx < FILE_MANAGER_PAGE_SIZE && selectedItemIdx < itemsFetched) {
      fileBrowserRenderData.selectedIsFolder = fileBrowserRenderData.items[selectedItemIdx].isFolder;
    }
  }
  
  fileBrowserRenderData.valid = true;
}

// Render file browser from pre-gathered data (called INSIDE I2C transaction)
void displayFileBrowserRendered() {
  if (!oledDisplay || !oledConnected) return;
  
  if (!fileBrowserRenderData.valid) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Init failed!");
    return;
  }
  
  // Layout constants (matching menu style) - adjusted for global header
  const int listWidth = 78;       // Width for text list area
  const int iconAreaX = 88;       // X position for icon area
  const int iconSize = 32;        // Full size icon for selected item
  const int itemHeight = 10;      // Height per item
  const int maxVisibleItems = 4;  // Items visible at once
  const int startY = OLED_CONTENT_START_Y + 1;  // Start 1px below header for even spacing
  
  // Header is now drawn globally
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Draw vertical separator between list and icon area
  oledDisplay->drawFastVLine(84, OLED_CONTENT_START_Y, OLED_CONTENT_HEIGHT, DISPLAY_COLOR_WHITE);
  
  // Calculate scroll offset to keep selected item visible
  int scrollOffset = 0;
  if (fileBrowserRenderData.selectedIdx >= maxVisibleItems) {
    scrollOffset = fileBrowserRenderData.selectedIdx - maxVisibleItems + 1;
  }
  
  // === File List: Show 4 items (text list on left) ===
  int itemIdx = 0;
  for (int i = 0; i < maxVisibleItems && (scrollOffset + i) < fileBrowserRenderData.itemCount; i++) {
    int idx = scrollOffset + i;
    int y = startY + i * itemHeight;
    
    // Get the item from pre-fetched data
    if (idx >= fileBrowserRenderData.pageStart && idx < fileBrowserRenderData.pageEnd && itemIdx < FILE_MANAGER_PAGE_SIZE) {
      FileEntry& entry = fileBrowserRenderData.items[itemIdx];
      itemIdx++;
      
      bool isSelected = (idx == fileBrowserRenderData.selectedIdx);
      
      // Highlight selected item (1px shorter to create gap, no -1 offset)
      if (isSelected) {
        oledDisplay->fillRect(0, y, listWidth, itemHeight - 1, DISPLAY_COLOR_WHITE);
        oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
      } else {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      }
      
      // Draw name (truncate to fit in list area, 1px down to align with highlight)
      oledDisplay->setCursor(2, y + 1);
      String name = String(entry.name);
      if (name.length() > 13) {
        name = name.substring(0, 10) + "...";
      }
      oledDisplay->print(name);
    }
  }
  
  // Reset text color
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // === Draw selected item's icon and info on the right ===
  if (fileBrowserRenderData.itemCount > 0) {
    // Get selected item
    int selectedItemIdx = fileBrowserRenderData.selectedIdx - fileBrowserRenderData.pageStart;
    if (selectedItemIdx >= 0 && selectedItemIdx < FILE_MANAGER_PAGE_SIZE) {
      FileEntry& selectedEntry = fileBrowserRenderData.items[selectedItemIdx];
      
      // Draw icon (centered in content area, not touching header)
      const int availableIconHeight = OLED_CONTENT_HEIGHT - 10;
      int iconX = iconAreaX + (128 - iconAreaX - iconSize) / 2;
      int iconY = OLED_CONTENT_START_Y + (availableIconHeight - iconSize - 18) / 2;  // Relative to content area
      
      if (selectedEntry.isFolder) {
        drawIcon(oledDisplay, "folder", iconX, iconY, DISPLAY_COLOR_WHITE);
      } else {
        const char* ext = strrchr(selectedEntry.name, '.');
        const char* iconName = getIconNameForExtension(ext ? ext + 1 : "");
        drawIcon(oledDisplay, iconName, iconX, iconY, DISPLAY_COLOR_WHITE);
      }
      
      // Draw file info below icon
      int textY = iconY + iconSize + 2;
      if (textY + 16 <= OLED_CONTENT_HEIGHT) {
        oledDisplay->setTextSize(1);
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        
        // Show file size or folder indicator
        if (selectedEntry.isFolder) {
          oledDisplay->setCursor(iconAreaX + 2, textY);
          oledDisplay->print("Folder");
        } else {
          String sizeStr = formatFileSize(selectedEntry.size);
          // Center the size text
          int sizeWidth = sizeStr.length() * 6;
          int sizeX = iconAreaX + (128 - iconAreaX - sizeWidth) / 2;
          oledDisplay->setCursor(sizeX, textY);
          oledDisplay->print(sizeStr);
        }
      }
    }
  }
  
  // Show empty message if no items
  if (fileBrowserRenderData.itemCount == 0) {
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(20, 30);
    oledDisplay->print("(empty)");
  }
  
  // Draw scroll indicators if needed (must stay within content area)
  if (scrollOffset > 0) {
    oledDisplay->setCursor(78, OLED_CONTENT_START_Y + 1);
    oledDisplay->print("^");
  }
  if (scrollOffset + maxVisibleItems < fileBrowserRenderData.itemCount) {
    int scrollDownY = OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 9;
    oledDisplay->setCursor(78, scrollDownY);
    oledDisplay->print("v");
  }
  
  // Note: Footer navigation hints now handled by global footer system
  // Don't call display() here - let updateOLEDDisplay() render footer and display in same frame
}

// ============================================================================
// File Browser Navigation Functions
// ============================================================================

/**
 * File browser navigation functions
 * Call these from your button interrupt handlers or main loop
 */
void oledFileBrowserUp() {
  if (!gOLEDFileManager) return;
  
  unsigned long now = millis();
  if (now - oledFileBrowserLastInput < OLED_FILE_BROWSER_DEBOUNCE) return;
  oledFileBrowserLastInput = now;
  
  gOLEDFileManager->moveUp();
  // Display will update on next updateOLEDDisplay() call
}

void oledFileBrowserDown() {
  if (!gOLEDFileManager) return;
  
  unsigned long now = millis();
  if (now - oledFileBrowserLastInput < OLED_FILE_BROWSER_DEBOUNCE) return;
  oledFileBrowserLastInput = now;
  
  gOLEDFileManager->moveDown();
  // Display will update on next updateOLEDDisplay() call
}

void oledFileBrowserSelect() {
  if (!gOLEDFileManager) return;
  
  unsigned long now = millis();
  if (now - oledFileBrowserLastInput < OLED_FILE_BROWSER_DEBOUNCE) return;
  oledFileBrowserLastInput = now;
  
  // Defer navigation to prevent filesystem I/O outside I2C transaction
  fileBrowserPendingAction = FileBrowserPendingAction::NAVIGATE_INTO;
  // Actual navigation will happen in displayFileBrowser() inside I2C transaction
}

void oledFileBrowserBack() {
  if (!gOLEDFileManager) return;
  
  unsigned long now = millis();
  if (now - oledFileBrowserLastInput < OLED_FILE_BROWSER_DEBOUNCE) return;
  oledFileBrowserLastInput = now;
  
  // Defer navigation to prevent filesystem I/O outside I2C transaction
  fileBrowserPendingAction = FileBrowserPendingAction::NAVIGATE_BACK;
  // Actual navigation will happen in displayFileBrowser() inside I2C transaction
}

// ============================================================================
// File Browser Input Handler (registered via OLEDModeEntry)
// ============================================================================

static bool fileBrowserInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  extern NavEvents gNavEvents;
  if (gNavEvents.down) {
    oledFileBrowserDown();
    return true;
  } else if (gNavEvents.up) {
    oledFileBrowserUp();
    return true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    oledFileBrowserSelect();
    return true;
  }
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    oledFileBrowserBack();
    return true;
  }
  return false;
}

extern void displayFileBrowserRendered();

static const OLEDModeEntry sFileBrowserModes[] = {
  { OLED_FILE_BROWSER, "Files", "file_text", displayFileBrowserRendered, nullptr, fileBrowserInputHandler, false, -1 },
};

REGISTER_OLED_MODE_MODULE(sFileBrowserModes, sizeof(sFileBrowserModes) / sizeof(sFileBrowserModes[0]), "FileBrowser");

/**
 * Reset file browser (e.g., when switching to this mode)
 */
void resetOLEDFileBrowser() {
  // Clean up existing manager
  if (gOLEDFileManager) {
    delete gOLEDFileManager;
    gOLEDFileManager = nullptr;
  }
  
  // Initialize immediately (not on next display call)
  initFileBrowser();
}

#endif // ENABLE_OLED_DISPLAY
