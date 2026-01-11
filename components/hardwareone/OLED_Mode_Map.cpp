// ============================================================================
// OLED Map Mode
// ============================================================================
// Extracted from GPS_MapRenderer.cpp for modularity
// Contains all OLED-specific map rendering and menu code

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "System_GPSMapRenderer.h"
#include "OLED_Utils.h"
#include "System_FileManager.h"
#include "i2csensor-seesaw.h"
#include "System_Debug.h"
#include "System_I2C.h"

#if ENABLE_GPS_SENSOR
#include <Adafruit_GPS.h>
#include "i2csensor-pa1010d.h"
#endif

// External OLED display pointer
extern Adafruit_SSD1306* oledDisplay;
extern OLEDMode currentOLEDMode;

// External map state from GPS_MapRenderer.cpp
extern bool gMapRendererEnabled;
extern float gMapRotation;
extern float gMapZoom;

// Center position for map viewing (defined in GPS_MapRenderer.cpp, accessed via extern)
extern float gMapCenterLat;
extern float gMapCenterLon;
extern bool gMapCenterSet;
extern bool gMapManuallyPanned;

// Momentum-based scrolling
extern float gMapVelocityLat;
extern float gMapVelocityLon;
extern float gMapRotationVelocity;

// Singleton renderer for OLED
static OLEDMapRenderer* gOLEDMapRenderer = nullptr;

// Map menu state (accessed by footer system)
bool gMapMenuOpen = false;
static int gMapMenuSelection = 0;
static int gMapMenuScrollOffset = 0;

// Menu items for the map mode
static const char* gMapMenuItems[] = {
  "Zoom In",
  "Zoom Out",
  "Reset Zoom",
  "Rotate Left",
  "Rotate Right",
  "Reset Rotation",
  "Select Map",
  "Next Map",
  "Previous Map",
  "Recenter Map",
#if ENABLE_GPS_SENSOR
  "Center on GPS",
  "Toggle GPS",
#endif
  "Mark Waypoint",
  "Goto Waypoint",
  "Clear Nav",
  "Del Waypoint",
  "Map Info",
  "Features",
  "Close Menu"
};
#if ENABLE_GPS_SENSOR
static const int gMapMenuItemCount = 19;
#else
static const int gMapMenuItemCount = 17;
#endif

// Waypoint selection state for submenu
static int gWaypointSelectMode = 0;  // 0=none, 1=goto, 2=delete
static int gWaypointSelectIdx = 0;

// Map info overlay state
static bool gShowMapInfo = false;

// Features viewer state
static bool gShowFeatures = false;
static int gFeaturesCategory = 0;  // 0-5 for different categories
static int gFeaturesScrollOffset = 0;

// Zoom constants
static const float MAP_ZOOM_MIN = 0.25f;
static const float MAP_ZOOM_MAX = 4.0f;
static const float MAP_ZOOM_STEP = 1.5f;

// Draw map info overlay
static void drawMapInfo() {
  if (!oledDisplay) return;
  
  const LoadedMap& currentMap = MapCore::getCurrentMap();
  
  oledDisplay->fillRect(0, 0, 128, 64, DISPLAY_COLOR_BLACK);
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  oledDisplay->setCursor(0, 0);
  if (currentMap.valid) {
    oledDisplay->print(currentMap.filename);
  } else {
    oledDisplay->print("No map loaded");
  }
  
  if (currentMap.valid) {
    char buf[32];
    oledDisplay->setCursor(0, 10);
    snprintf(buf, sizeof(buf), "Features: %lu", currentMap.header.featureCount);
    oledDisplay->print(buf);
    
    oledDisplay->setCursor(0, 20);
    snprintf(buf, sizeof(buf), "Zoom: %.1fx", gMapZoom);
    oledDisplay->print(buf);
    
    oledDisplay->setCursor(0, 30);
    snprintf(buf, sizeof(buf), "Rot: %.0f", gMapRotation);
    oledDisplay->print(buf);
  }
  
  oledDisplay->setCursor(0, 40);
  char wpBuf[24];
  snprintf(wpBuf, sizeof(wpBuf), "WPs: %d", WaypointManager::getActiveCount());
  oledDisplay->print(wpBuf);
  
  oledDisplay->setCursor(0, 50);
  char coordBuf[24];
  snprintf(coordBuf, sizeof(coordBuf), "%.4f,%.4f", gMapCenterLat, gMapCenterLon);
  oledDisplay->print(coordBuf);
  
  oledDisplay->setCursor(90, 56);
  oledDisplay->print("B:OK");
}

// Draw features list overlay
static void drawFeatures() {
  if (!oledDisplay) return;
  
  const LoadedMap& map = MapCore::getCurrentMap();
  
  oledDisplay->fillRect(0, 0, 128, 64, DISPLAY_COLOR_BLACK);
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  const char* catNames[] = {"Highways", "Roads", "Water", "Parks", "Railways", "Subways"};
  
  oledDisplay->setCursor(0, 0);
  if (!map.valid || !map.hasMetadata) {
    oledDisplay->print("No features");
    oledDisplay->setCursor(0, 10);
    oledDisplay->print("(need metadata)");
    oledDisplay->setCursor(90, 56);
    oledDisplay->print("B:OK");
    return;
  }
  
  int catCount = 0;
  for (int i = 0; i < map.metadataCount; i++) {
    if (map.metadata[i].category == gFeaturesCategory) catCount++;
  }
  
  char header[32];
  snprintf(header, sizeof(header), "%s (%d)", catNames[gFeaturesCategory], catCount);
  oledDisplay->print(header);
  
  int itemsPerPage = 5;
  int displayIdx = 0;
  int itemIdx = 0;
  
  for (int i = 0; i < map.metadataCount && displayIdx < itemsPerPage; i++) {
    if (map.metadata[i].category == gFeaturesCategory) {
      if (itemIdx >= gFeaturesScrollOffset) {
        oledDisplay->setCursor(0, 10 + displayIdx * 10);
        char nameBuf[22];
        strncpy(nameBuf, map.metadata[i].name, 21);
        nameBuf[21] = '\0';
        oledDisplay->print(nameBuf);
        displayIdx++;
      }
      itemIdx++;
    }
  }
  
  oledDisplay->setCursor(0, 56);
  oledDisplay->print("</>:Cat");
  oledDisplay->setCursor(50, 56);
  oledDisplay->print("^v:Scrl");
  oledDisplay->setCursor(100, 56);
  oledDisplay->print("B:OK");
}

// Draw waypoint selection submenu
static void drawWaypointSelect() {
  if (!oledDisplay) return;
  
  oledDisplay->fillRect(10, 5, 108, 48, DISPLAY_COLOR_BLACK);
  oledDisplay->drawRect(10, 5, 108, 48, DISPLAY_COLOR_WHITE);
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(14, 8);
  oledDisplay->print(gWaypointSelectMode == 1 ? "= Goto WP =" : "= Del WP =");
  
  int activeIdx = 0;
  for (int i = 0; i < MAX_WAYPOINTS; i++) {
    const Waypoint* wp = WaypointManager::getWaypoint(i);
    if (wp) {
      if (activeIdx == gWaypointSelectIdx) {
        oledDisplay->fillRect(12, 20, 104, 12, DISPLAY_COLOR_WHITE);
        oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
        oledDisplay->setCursor(14, 22);
        char buf[24];
        snprintf(buf, sizeof(buf), "%d: %s", i, wp->name);
        oledDisplay->print(buf);
        break;
      }
      activeIdx++;
    }
  }
  
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(14, 40);
  oledDisplay->print("A:OK  B:Cancel");
}

// Draw the map menu overlay
static void drawMapMenu() {
  if (!oledDisplay) return;
  
  if (gShowMapInfo) {
    drawMapInfo();
    return;
  }
  
  if (gShowFeatures) {
    drawFeatures();
    return;
  }
  
  if (gWaypointSelectMode != 0) {
    drawWaypointSelect();
    return;
  }
  
  oledDisplay->fillRect(10, 5, 108, 48, DISPLAY_COLOR_BLACK);
  oledDisplay->drawRect(10, 5, 108, 48, DISPLAY_COLOR_WHITE);
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(14, 8);
  oledDisplay->print("== Map Menu ==");
  
  const int maxVisible = 4;
  const int itemHeight = 9;
  const int startY = 18;
  
  for (int i = 0; i < maxVisible && (i + gMapMenuScrollOffset) < gMapMenuItemCount; i++) {
    int itemIdx = i + gMapMenuScrollOffset;
    int y = startY + (i * itemHeight);
    
    if (itemIdx == gMapMenuSelection) {
      oledDisplay->fillRect(12, y - 1, 104, itemHeight, DISPLAY_COLOR_WHITE);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    oledDisplay->setCursor(14, y);
    oledDisplay->print(gMapMenuItems[itemIdx]);
  }
  
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  if (gMapMenuScrollOffset > 0) {
    oledDisplay->setCursor(112, 18);
    oledDisplay->print("^");
  }
  if (gMapMenuScrollOffset + maxVisible < gMapMenuItemCount) {
    oledDisplay->setCursor(112, 44);
    oledDisplay->print("v");
  }
}

// Display function for GPS Map mode
static void displayGPSMap() {
  if (!oledDisplay) return;
  
  // Handle OLED keyboard for waypoint naming
    
  if (gOLEDKeyboardState.active) {
    oledKeyboardDisplay(oledDisplay);
    
    if (oledKeyboardIsCompleted()) {
      const char* wpName = oledKeyboardGetText();
      int idx = WaypointManager::addWaypoint(gMapCenterLat, gMapCenterLon, wpName);
      if (idx >= 0) {
        INFO_SENSORSF("Marked waypoint %d: %s at %.5f, %.5f", idx, wpName, gMapCenterLat, gMapCenterLon);
      }
      oledKeyboardReset();
      gOLEDKeyboardState.active = false;
    } else if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      gOLEDKeyboardState.active = false;
    }
    return;
  }
  
  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, 0);
  
  float lat = 0.0f, lon = 0.0f;
  bool hasGPSFix = false;
  int satellites = 0;
  
#if ENABLE_GPS_SENSOR
  if (gpsConnected && gPA1010D != nullptr && gpsEnabled) {
    (void)gPA1010D->read();
    if (gPA1010D->newNMEAreceived()) {
      gPA1010D->parse(gPA1010D->lastNMEA());
    }
    
    if (gPA1010D->fix) {
      lat = gPA1010D->latitudeDegrees;
      lon = gPA1010D->longitudeDegrees;
      if (gPA1010D->lat == 'S') lat = -lat;
      if (gPA1010D->lon == 'W') lon = -lon;
      hasGPSFix = true;
      satellites = (int)gPA1010D->satellites;
    } else {
      satellites = (int)gPA1010D->satellites;
    }
  }
#endif
  
  const LoadedMap& currentMap = MapCore::getCurrentMap();
  
  if (!currentMap.valid && !gSensorPollingPaused) {
    char maps[8][32];
    int mapCount = MapCore::getAvailableMaps(maps, 8);
    if (mapCount > 0) {
      char path[48];
      snprintf(path, sizeof(path), "/maps/%s", maps[0]);
      MapCore::loadMapFile(path);
    }
  }
  
  const LoadedMap& map = MapCore::getCurrentMap();
  
  if (!map.valid) {
    oledDisplay->println("=== MAP VIEWER ===");
    oledDisplay->println();
    oledDisplay->println("No map loaded");
    oledDisplay->println();
    oledDisplay->println("Upload .hwmap files");
    oledDisplay->println("to /maps/ folder");
    
    if (gMapMenuOpen) {
      drawMapMenu();
    }
    return;
  }
  
  if (hasGPSFix && MapCore::isPositionInMap(lat, lon) && !gMapManuallyPanned) {
    gMapCenterLat = lat;
    gMapCenterLon = lon;
    gMapCenterSet = true;
  } else if (!gMapCenterSet) {
    gMapCenterLat = (map.header.minLat + map.header.maxLat) / 2000000.0f;
    gMapCenterLon = (map.header.minLon + map.header.maxLon) / 2000000.0f;
    gMapCenterSet = true;
  }
  
  if (!gOLEDMapRenderer) {
    gOLEDMapRenderer = new OLEDMapRenderer(oledDisplay);
  }
  
  MapCore::renderMap(gOLEDMapRenderer, gMapCenterLat, gMapCenterLon);
  
  char overlayBuf[24];
  snprintf(overlayBuf, sizeof(overlayBuf), " %.8s ", map.header.regionName);
  gOLEDMapRenderer->drawOverlayText(0, 0, overlayBuf, true);
  
  if (gMapZoom != 1.0f || gMapRotation != 0.0f) {
    int pos = 50;
    if (gMapZoom != 1.0f) {
      snprintf(overlayBuf, sizeof(overlayBuf), " %.1fx ", gMapZoom);
      gOLEDMapRenderer->drawOverlayText(pos, 0, overlayBuf, true);
      pos += 30;
    }
    if (gMapRotation != 0.0f) {
      snprintf(overlayBuf, sizeof(overlayBuf), " %d\xf8 ", (int)gMapRotation);
      gOLEDMapRenderer->drawOverlayText(pos, 0, overlayBuf, true);
    }
  }
  
#if ENABLE_GPS_SENSOR
  if (gpsEnabled) {
    snprintf(overlayBuf, sizeof(overlayBuf), " %dS ", satellites);
    gOLEDMapRenderer->drawOverlayText(100, 0, overlayBuf, true);
  }
#endif
  
  float distM, bearingDeg;
  if (WaypointManager::getDistanceBearing(gMapCenterLat, gMapCenterLon, distM, bearingDeg)) {
    const Waypoint* target = WaypointManager::getWaypoint(WaypointManager::getSelectedTarget());
    if (target) {
      if (distM >= 1000.0f) {
        snprintf(overlayBuf, sizeof(overlayBuf), "%.1fkm %d\xf8", distM / 1000.0f, (int)bearingDeg);
      } else {
        snprintf(overlayBuf, sizeof(overlayBuf), "%dm %d\xf8", (int)distM, (int)bearingDeg);
      }
      gOLEDMapRenderer->drawOverlayText(0, 56, overlayBuf, true);
    }
  }
  
  if (gMapMenuOpen) {
    drawMapMenu();
  }
}

static bool gpsMapAvailable(String* outReason) {
  return true;
}

// Execute a menu action
static void executeMapMenuAction(int action) {
  char maps[8][32];
  int mapCount;
  int currentIdx;
  const LoadedMap& currentMap = MapCore::getCurrentMap();
  
  switch (action) {
    case 0:  // Zoom In
      gMapZoom = fminf(gMapZoom * MAP_ZOOM_STEP, MAP_ZOOM_MAX);
      break;
    case 1:  // Zoom Out
      gMapZoom = fmaxf(gMapZoom / MAP_ZOOM_STEP, MAP_ZOOM_MIN);
      break;
    case 2:  // Reset Zoom
      gMapZoom = 1.0f;
      break;
    case 3:  // Rotate Left
      gMapRotation = fmodf(gMapRotation - 15.0f + 360.0f, 360.0f);
      break;
    case 4:  // Rotate Right
      gMapRotation = fmodf(gMapRotation + 15.0f, 360.0f);
      break;
    case 5:  // Reset Rotation
      gMapRotation = 0.0f;
      break;
    case 6:  // Select Map
      {
        extern class FileManager* gOLEDFileManager;
        extern bool oledFileBrowserNeedsInit;
        
        currentOLEDMode = OLED_FILE_BROWSER;
        
        if (gOLEDFileManager) {
          gOLEDFileManager->navigate("/maps");
        } else {
          oledFileBrowserNeedsInit = true;
        }
        
        gMapMenuOpen = false;
      }
      break;
    case 7:  // Next Map
      mapCount = MapCore::getAvailableMaps(maps, 8);
      if (mapCount > 0) {
        currentIdx = -1;
        for (int i = 0; i < mapCount; i++) {
          if (strcmp(maps[i], currentMap.filename) == 0) {
            currentIdx = i;
            break;
          }
        }
        int nextIdx = (currentIdx + 1) % mapCount;
        char path[48];
        snprintf(path, sizeof(path), "/maps/%s", maps[nextIdx]);
        MapCore::loadMapFile(path);
        gMapCenterSet = false;
        gMapManuallyPanned = false;
      }
      break;
    case 8:  // Previous Map
      mapCount = MapCore::getAvailableMaps(maps, 8);
      if (mapCount > 0) {
        currentIdx = -1;
        for (int i = 0; i < mapCount; i++) {
          if (strcmp(maps[i], currentMap.filename) == 0) {
            currentIdx = i;
            break;
          }
        }
        int prevIdx = (currentIdx - 1 + mapCount) % mapCount;
        char path[48];
        snprintf(path, sizeof(path), "/maps/%s", maps[prevIdx]);
        MapCore::loadMapFile(path);
        gMapCenterSet = false;
        gMapManuallyPanned = false;
      }
      break;
    case 9:  // Recenter Map
      if (currentMap.valid) {
        gMapCenterLat = (currentMap.header.minLat + currentMap.header.maxLat) / 2000000.0f;
        gMapCenterLon = (currentMap.header.minLon + currentMap.header.maxLon) / 2000000.0f;
        gMapCenterSet = true;
        gMapManuallyPanned = false;
      }
      break;
#if ENABLE_GPS_SENSOR
    case 10:  // Center on GPS
      if (gpsConnected && gPA1010D != nullptr && gPA1010D->fix) {
        gMapCenterLat = gPA1010D->latitudeDegrees;
        gMapCenterLon = gPA1010D->longitudeDegrees;
        if (gPA1010D->lat == 'S') gMapCenterLat = -gMapCenterLat;
        if (gPA1010D->lon == 'W') gMapCenterLon = -gMapCenterLon;
        gMapCenterSet = true;
        gMapManuallyPanned = false;
      }
      break;
    case 11:  // Toggle GPS
      {
        extern bool enqueueSensorStart(SensorType sensor);
        extern bool isInQueue(SensorType sensor);
        if (gpsEnabled) {
          gpsEnabled = false;
        } else if (!isInQueue(SENSOR_GPS)) {
          enqueueSensorStart(SENSOR_GPS);
        }
      }
      break;
    case 12:  // Mark Waypoint (GPS build)
      {
                char defaultName[WAYPOINT_NAME_LEN];
        snprintf(defaultName, sizeof(defaultName), "WP%d", WaypointManager::getActiveCount());
        oledKeyboardInit("Name Waypoint", defaultName, WAYPOINT_NAME_LEN - 1);
        gOLEDKeyboardState.active = true;
        gMapMenuOpen = false;
      }
      break;
    case 13:  // Goto Waypoint (GPS build)
      if (WaypointManager::getActiveCount() > 0) {
        gWaypointSelectMode = 1;
        gWaypointSelectIdx = 0;
      }
      break;
    case 14:  // Clear Nav (GPS build)
      WaypointManager::selectTarget(-1);
      break;
    case 15:  // Del Waypoint (GPS build)
      if (WaypointManager::getActiveCount() > 0) {
        gWaypointSelectMode = 2;
        gWaypointSelectIdx = 0;
      }
      break;
    case 16:  // Map Info (GPS build)
      gShowMapInfo = true;
      break;
    case 17:  // Features (GPS build)
      gShowFeatures = true;
      gFeaturesCategory = 0;
      gFeaturesScrollOffset = 0;
      break;
    case 18:  // Close Menu (GPS build)
      gMapMenuOpen = false;
      break;
#else
    case 10:  // Mark Waypoint (no GPS build)
      {
                char defaultName[WAYPOINT_NAME_LEN];
        snprintf(defaultName, sizeof(defaultName), "WP%d", WaypointManager::getActiveCount());
        oledKeyboardInit("Name Waypoint", defaultName, WAYPOINT_NAME_LEN - 1);
        gOLEDKeyboardState.active = true;
        gMapMenuOpen = false;
      }
      break;
    case 11:  // Goto Waypoint (no GPS build)
      if (WaypointManager::getActiveCount() > 0) {
        gWaypointSelectMode = 1;
        gWaypointSelectIdx = 0;
      }
      break;
    case 12:  // Clear Nav (no GPS build)
      WaypointManager::selectTarget(-1);
      break;
    case 13:  // Del Waypoint (no GPS build)
      if (WaypointManager::getActiveCount() > 0) {
        gWaypointSelectMode = 2;
        gWaypointSelectIdx = 0;
      }
      break;
    case 14:  // Map Info (no GPS build)
      gShowMapInfo = true;
      break;
    case 15:  // Features (no GPS build)
      gShowFeatures = true;
      gFeaturesCategory = 0;
      gFeaturesScrollOffset = 0;
      break;
    case 16:  // Close Menu (no GPS build)
      gMapMenuOpen = false;
      break;
#endif
  }
}

// Input handler for GPS Map mode
static bool gpsMapInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Start button: toggle menu
  if (newlyPressed & 0x10000) {
    gMapMenuOpen = !gMapMenuOpen;
    if (gMapMenuOpen) {
      gMapMenuSelection = 0;
      gMapMenuScrollOffset = 0;
    }
    return true;
  }
  
  // Map info overlay - any button dismisses
  if (gShowMapInfo) {
    if (newlyPressed & (0x20 | 0x02)) {
      gShowMapInfo = false;
      gMapMenuOpen = false;
      return true;
    }
    return true;
  }
  
  // Features viewer input handling
  if (gShowFeatures) {
    const LoadedMap& map = MapCore::getCurrentMap();
    const int menuDeadzone = 5;
    
    if (newlyPressed & 0x02) {
      gShowFeatures = false;
      gMapMenuOpen = false;
      return true;
    }
    
    if (deltaX < -menuDeadzone) {
      gFeaturesCategory = (gFeaturesCategory + 5) % 6;
      gFeaturesScrollOffset = 0;
      return true;
    }
    if (deltaX > menuDeadzone) {
      gFeaturesCategory = (gFeaturesCategory + 1) % 6;
      gFeaturesScrollOffset = 0;
      return true;
    }
    
    if (map.valid && map.hasMetadata) {
      int catCount = 0;
      for (int i = 0; i < map.metadataCount; i++) {
        if (map.metadata[i].category == gFeaturesCategory) catCount++;
      }
      
      if (deltaY < -menuDeadzone && gFeaturesScrollOffset > 0) {
        gFeaturesScrollOffset--;
        return true;
      }
      if (deltaY > menuDeadzone && gFeaturesScrollOffset < catCount - 5) {
        gFeaturesScrollOffset++;
        return true;
      }
    }
    
    return true;
  }
  
  // Waypoint selection mode input handling
  if (gWaypointSelectMode != 0) {
    int wpCount = WaypointManager::getActiveCount();
    
    if (newlyPressed & 0x20) {
      int activeIdx = 0;
      for (int i = 0; i < MAX_WAYPOINTS; i++) {
        const Waypoint* wp = WaypointManager::getWaypoint(i);
        if (wp) {
          if (activeIdx == gWaypointSelectIdx) {
            if (gWaypointSelectMode == 1) {
              WaypointManager::selectTarget(i);
              gMapCenterLat = wp->lat;
              gMapCenterLon = wp->lon;
              gMapCenterSet = true;
              gMapManuallyPanned = true;
            } else if (gWaypointSelectMode == 2) {
              WaypointManager::deleteWaypoint(i);
            }
            break;
          }
          activeIdx++;
        }
      }
      gWaypointSelectMode = 0;
      gMapMenuOpen = false;
      return true;
    }
    
    if (newlyPressed & 0x02) {
      gWaypointSelectMode = 0;
      return true;
    }
    
    const int menuDeadzone = 5;
    if (deltaY < -menuDeadzone && gWaypointSelectIdx > 0) {
      gWaypointSelectIdx--;
      return true;
    }
    if (deltaY > menuDeadzone && gWaypointSelectIdx < wpCount - 1) {
      gWaypointSelectIdx++;
      return true;
    }
    
    return true;
  }
  
  // Menu navigation when open
  if (gMapMenuOpen) {
    const int maxVisible = 4;
    
    if (newlyPressed & 0x20) {
      executeMapMenuAction(gMapMenuSelection);
      return true;
    }
    
    if (newlyPressed & 0x02) {
      gMapMenuOpen = false;
      return true;
    }
    
    const int menuDeadzone = 5;
    if (deltaY < -menuDeadzone && gMapMenuSelection > 0) {
      gMapMenuSelection--;
      if (gMapMenuSelection < gMapMenuScrollOffset) {
        gMapMenuScrollOffset = gMapMenuSelection;
      }
      return true;
    }
    if (deltaY > menuDeadzone && gMapMenuSelection < gMapMenuItemCount - 1) {
      gMapMenuSelection++;
      if (gMapMenuSelection >= gMapMenuScrollOffset + maxVisible) {
        gMapMenuScrollOffset = gMapMenuSelection - maxVisible + 1;
      }
      return true;
    }
    
    return true;
  }
  
  // Normal map controls when menu is closed
  
  bool aHeld = false;
#if ENABLE_GAMEPAD_SENSOR
  if (gControlCache.mutex && xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    aHeld = !(gControlCache.gamepadButtons & GAMEPAD_BUTTON_A);
    xSemaphoreGive(gControlCache.mutex);
  }
#endif
  
  const int rotateDeadzone = 20;
  const float rotateAccel = 0.015f;
  const float rotateFriction = 0.85f;
  const float maxRotateVelocity = 8.0f;
  const float minRotateVelocity = 0.1f;
  
  bool rotated = false;
  
  if (!aHeld) {
    gMapRotationVelocity = 0.0f;
  }
  
  if (aHeld && (deltaX > rotateDeadzone || deltaX < -rotateDeadzone)) {
    int effectiveX = (deltaX > 0) ? (deltaX - rotateDeadzone) : (deltaX + rotateDeadzone);
    gMapRotationVelocity += effectiveX * rotateAccel;
    gMapRotationVelocity = fmaxf(-maxRotateVelocity, fminf(maxRotateVelocity, gMapRotationVelocity));
  }
  
  if (aHeld && fabsf(gMapRotationVelocity) > minRotateVelocity) {
    gMapRotation = fmodf(gMapRotation + gMapRotationVelocity + 360.0f, 360.0f);
    rotated = true;
    gMapRotationVelocity *= rotateFriction;
    if (fabsf(gMapRotationVelocity) < minRotateVelocity) gMapRotationVelocity = 0;
  }
  
  if (rotated) {
    return true;
  }
  
  const float baseAccel = 0.0000008f;
  const float accel = baseAccel / gMapZoom;
  const float friction = 0.88f;
  const float maxVelocity = 0.0004f / gMapZoom;
  const float minVelocity = 0.0000001f;
  
  bool panned = false;
  
  const int panDeadzone = 10;
  bool significantX = (deltaX > panDeadzone || deltaX < -panDeadzone);
  bool significantY = (deltaY > panDeadzone || deltaY < -panDeadzone);
  
  if (!aHeld && (significantX || significantY)) {
    int effectiveDeltaX = significantX ? ((deltaX > 0) ? (deltaX - panDeadzone) : (deltaX + panDeadzone)) : 0;
    int effectiveDeltaY = significantY ? ((deltaY > 0) ? (deltaY - panDeadzone) : (deltaY + panDeadzone)) : 0;
    
    float radians = -gMapRotation * M_PI / 180.0f;
    float cosR = cosf(radians);
    float sinR = sinf(radians);
    
    float accelLon = (effectiveDeltaX * cosR - effectiveDeltaY * sinR) * accel;
    float accelLat = (effectiveDeltaX * sinR + effectiveDeltaY * cosR) * accel;
    
    gMapVelocityLon += accelLon;
    gMapVelocityLat -= accelLat;
    
    gMapVelocityLon = fmaxf(-maxVelocity, fminf(maxVelocity, gMapVelocityLon));
    gMapVelocityLat = fmaxf(-maxVelocity, fminf(maxVelocity, gMapVelocityLat));
  }
  
  if (fabsf(gMapVelocityLon) > minVelocity || fabsf(gMapVelocityLat) > minVelocity) {
    gMapCenterLon += gMapVelocityLon;
    gMapCenterLat += gMapVelocityLat;
    gMapCenterSet = true;
    gMapManuallyPanned = true;
    panned = true;
    
    gMapVelocityLon *= friction;
    gMapVelocityLat *= friction;
    
    if (fabsf(gMapVelocityLon) < minVelocity) gMapVelocityLon = 0;
    if (fabsf(gMapVelocityLat) < minVelocity) gMapVelocityLat = 0;
  }
  
  if (newlyPressed & 0x02) {
    return false;
  }
  
  return panned;
}

// Map OLED mode entry
static const OLEDModeEntry gpsMapOLEDModes[] = {
  {
    OLED_GPS_MAP,
    "Map",
    "map",
    displayGPSMap,
    gpsMapAvailable,
    gpsMapInputHandler,
    true,
    50
  }
};

// Auto-register on startup
static OLEDModeRegistrar _gps_map_registrar(gpsMapOLEDModes, 1, "Map");

#endif // ENABLE_OLED_DISPLAY
