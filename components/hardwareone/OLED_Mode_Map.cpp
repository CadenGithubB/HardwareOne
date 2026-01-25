// ============================================================================
// OLED Map Mode
// ============================================================================
// Extracted from GPS_MapRenderer.cpp for modularity
// Contains all OLED-specific map rendering and menu code

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "System_Maps.h"
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

// Context bar scrolling state
static int gContextScrollOffset = 0;
static uint32_t gContextScrollLastUpdate = 0;
#define CONTEXT_SCROLL_SPEED_MS 100  // Update every 100ms for smooth scrolling

// Hierarchical menu system
static int gMapSubmenuLevel = 0;     // 0=main menu, 1=submenu
static int gMapSubmenuType = 0;      // Which submenu is open (index in main menu)

// Main menu categories
enum MapMenuCategory {
  MENU_CAT_VIEW = 0,
  MENU_CAT_MAPS,
#if ENABLE_GPS_SENSOR
  MENU_CAT_GPS,
#endif
  MENU_CAT_WAYPOINTS,
  MENU_CAT_TRACKS,
  MENU_CAT_INFO,
  MENU_CAT_CLOSE,
  MENU_CAT_COUNT
};

static const char* gMapMainMenuItems[] = {
  "View >",
  "Maps >",
#if ENABLE_GPS_SENSOR
  "GPS >",
#endif
  "Waypoints >",
  "Tracks >",
  "Info >",
  "Close Menu"
};

#if ENABLE_GPS_SENSOR
static const int gMapMainMenuItemCount = 7;
#else
static const int gMapMainMenuItemCount = 6;
#endif

// Submenu items
static const char* gViewSubmenu[] = {
  "Zoom In", "Zoom Out", "Reset Zoom",
  "Rotate Left", "Rotate Right", "Reset Rotation",
  "< Back"
};
static const int gViewSubmenuCount = 7;

static const char* gMapsSubmenu[] = {
  "Select Map", "Next Map", "Previous Map", "Recenter",
  "< Back"
};
static const int gMapsSubmenuCount = 5;

#if ENABLE_GPS_SENSOR
static const char* gGPSSubmenu[] = {
  "Center on GPS", "Toggle GPS",
  "< Back"
};
static const int gGPSSubmenuCount = 3;
#endif

static const char* gWaypointsSubmenu[] = {
  "Mark Waypoint", "Goto Waypoint", "Clear Nav", "Delete Waypoint",
  "< Back"
};
static const int gWaypointsSubmenuCount = 5;

static const char* gTracksSubmenu[] = {
  "Load Track", "Clear Track", "Track Status", "Delete Track",
  "Live Track", "< Back"
};
static const int gTracksSubmenuCount = 6;

static const char* gInfoSubmenu[] = {
  "Map Info", "Features", "Search Names", "Transit Routes",
  "< Back"
};
static const int gInfoSubmenuCount = 5;

// Waypoint selection state for submenu
static int gWaypointSelectMode = 0;  // 0=none, 1=goto, 2=delete
static int gWaypointSelectIdx = 0;

// Track file selection state
static int gTrackSelectMode = 0;  // 0=none, 1=load, 2=delete
static int gTrackFileIdx = 0;
static char gTrackFiles[8][64];
static int gTrackFileCount = 0;

// Track status overlay
static bool gShowTrackStatus = false;

// Map info overlay state
static bool gShowMapInfo = false;

// Layer visibility toggles (default all on)
static bool gLayerHighways = true;
static bool gLayerMajorRoads = true;
static bool gLayerMinorRoads = true;
static bool gLayerPaths = true;
static bool gLayerWater = true;
static bool gLayerParks = true;
static bool gLayerRailways = true;
static bool gLayerTransit = true;
static bool gLayerBuildings = true;

// Features viewer state
static bool gShowFeatures = false;
static int gFeaturesCategory = 0;  // 0-5 for different categories
static int gFeaturesScrollOffset = 0;

// Routes viewer state
static bool gShowRoutes = false;
static int gRoutesScrollOffset = 0;
static int gRoutesSelectedIdx = 0;

// Search mode state
static bool gMapSearchMode = false;
static char gSearchResult[64] = "";

// Search results navigation (for multiple matches like "Target")
// Store coordinates directly since v5 format doesn't have flat feature indices
struct SearchResultCoord {
  float lat;
  float lon;
};
static SearchResultCoord gSearchResultCoords[32];  // Coordinates of matches
static int gSearchResultCount = 0;
static int gSearchResultCurrent = 0;
static bool gSearchResultsActive = false;  // True when viewing search results

// Autocomplete provider for map name search
static int mapNameAutocomplete(const char* input, const char** results, int maxResults, void* userData) {
  (void)userData;  // Unused
  return MapCore::searchNamesByPrefix(input, results, maxResults);
}

// Zoom constants
static const float MAP_ZOOM_MIN = 0.25f;
static const float MAP_ZOOM_MAX = 30.0f;
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

// Draw track status overlay
static void drawTrackStatus() {
  if (!oledDisplay) return;
  
  oledDisplay->fillRect(0, 0, 128, 64, DISPLAY_COLOR_BLACK);
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  oledDisplay->setCursor(0, 0);
  if (GPSTrackManager::isLiveTracking()) {
    oledDisplay->print("== LIVE TRACKING ==");
  } else {
    oledDisplay->print("== Track Status ==");
  }
  
  if (!GPSTrackManager::hasTrack() && !GPSTrackManager::isLiveTracking()) {
    oledDisplay->setCursor(0, 16);
    oledDisplay->print("No track loaded");
    oledDisplay->setCursor(90, 56);
    oledDisplay->print("B:OK");
    return;
  }
  
  char buf[32];
  const char* filename = strrchr(GPSTrackManager::getFilename(), '/');
  if (filename) filename++; else filename = GPSTrackManager::getFilename();
  
  oledDisplay->setCursor(0, 10);
  oledDisplay->print(filename);
  
  oledDisplay->setCursor(0, 20);
  snprintf(buf, sizeof(buf), "Points: %d", GPSTrackManager::getPointCount());
  oledDisplay->print(buf);
  
  const GPSTrackStats& stats = GPSTrackManager::getStats();
  if (stats.valid) {
    oledDisplay->setCursor(0, 30);
    if (stats.totalDistanceM >= 1000.0f) {
      snprintf(buf, sizeof(buf), "Dist: %.2fkm", stats.totalDistanceM / 1000.0f);
    } else {
      snprintf(buf, sizeof(buf), "Dist: %.0fm", stats.totalDistanceM);
    }
    oledDisplay->print(buf);
    
    oledDisplay->setCursor(0, 40);
    int mins = (int)(stats.durationSec / 60);
    int secs = (int)stats.durationSec % 60;
    snprintf(buf, sizeof(buf), "Time: %d:%02d", mins, secs);
    oledDisplay->print(buf);
    
    oledDisplay->setCursor(64, 40);
    snprintf(buf, sizeof(buf), "%.1fm/s", stats.avgSpeedMps);
    oledDisplay->print(buf);
  }
  
  // Validation status
  float coverage;
  TrackValidation validation = GPSTrackManager::validateTrack(coverage);
  oledDisplay->setCursor(0, 50);
  snprintf(buf, sizeof(buf), "Coverage: %.0f%%", coverage);
  oledDisplay->print(buf);
  
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
  if (!map.valid || map.nameCount == 0) {
    oledDisplay->print("No names");
    oledDisplay->setCursor(0, 10);
    oledDisplay->print("(map has no names)");
    oledDisplay->setCursor(90, 56);
    oledDisplay->print("B:OK");
    return;
  }
  
  int nameCount = map.nameCount;
  
  char header[32];
  snprintf(header, sizeof(header), "Names (%d)", nameCount);
  oledDisplay->print(header);
  
  int itemsPerPage = 5;
  int displayIdx = 0;
  int itemIdx = 0;
  
  for (int i = gFeaturesScrollOffset; i < map.nameCount && displayIdx < itemsPerPage; i++) {
    oledDisplay->setCursor(0, 10 + displayIdx * 10);
    char nameBuf[22];
    strncpy(nameBuf, map.names[i].name, 21);
    nameBuf[21] = '\0';
    oledDisplay->print(nameBuf);
    displayIdx++;
  }
  
  oledDisplay->setCursor(0, 56);
  oledDisplay->print("</>:Cat");
  oledDisplay->setCursor(50, 56);
  oledDisplay->print("^v:Scrl");
  oledDisplay->setCursor(100, 56);
  oledDisplay->print("B:OK");
}

// Route info structure for transit viewer
struct RouteInfo {
  const char* name;
  uint8_t type;  // 0x20=rail, 0x21=bus, 0x22=ferry
};

// Draw transit routes viewer
static void drawRoutes() {
  if (!oledDisplay) return;
  
  const LoadedMap& map = MapCore::getCurrentMap();
  
  oledDisplay->fillRect(0, 0, 128, 64, DISPLAY_COLOR_BLACK);
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  oledDisplay->setCursor(0, 0);
  oledDisplay->print("Transit Routes");
  
  if (!map.valid) {
    oledDisplay->setCursor(0, 16);
    oledDisplay->print("No map loaded");
    oledDisplay->setCursor(100, 56);
    oledDisplay->print("B:OK");
    return;
  }
  
  // Count transit features with names (rail, bus, ferry)
  int routeCount = 0;
  static RouteInfo routes[32];
  
  // Iterate through all tiles to find transit features (v5 tiled format)
  for (uint16_t tileIdx = 0; tileIdx < map.tileCount && routeCount < 32; tileIdx++) {
    size_t tileDataSize;
    const uint8_t* tileData = MapCore::loadTileData(tileIdx, &tileDataSize);
    if (!tileData || tileDataSize == 0) continue;
    
    const uint8_t* ptr = tileData;
    const uint8_t* end = tileData + tileDataSize;
    
    // Feature count is at the START of each tile's payload (2 bytes)
    if (ptr + 2 > end) continue;
    uint16_t featureCount = ptr[0] | (ptr[1] << 8);
    ptr += 2;
    
    const int hdrSize = HWMAP_FEATURE_HEADER_SIZE(map.header.version);
    for (uint16_t f = 0; f < featureCount && routeCount < 32; f++) {
      if (ptr + hdrSize > end) break;
      
      uint8_t type = ptr[0];
      // v6: type(1) + subtype(1) + nameIndex(2) + pointCount(2)
      // v5: type(1) + nameIndex(2) + pointCount(2)
      uint16_t nameIdx = (hdrSize == 6) ? (ptr[2] | (ptr[3] << 8)) : (ptr[1] | (ptr[2] << 8));
      uint16_t pointCount = (hdrSize == 6) ? (ptr[4] | (ptr[5] << 8)) : (ptr[3] | (ptr[4] << 8));
      ptr += hdrSize + pointCount * 4;  // Skip header + quantized points
      
      // Check if transit type (rail=0x20, bus=0x21, ferry=0x22) with name
      if ((type == 0x20 || type == 0x21 || type == 0x22) && nameIdx != 0xFFFF) {
        const char* name = MapCore::getName(nameIdx);
        if (name && name[0] != '\0') {
          // Check for duplicate
          bool duplicate = false;
          for (int i = 0; i < routeCount; i++) {
            if (strcmp(routes[i].name, name) == 0 && routes[i].type == type) {
              duplicate = true;
              break;
            }
          }
          if (!duplicate) {
            routes[routeCount].name = name;
            routes[routeCount].type = type;
            routeCount++;
          }
        }
      }
    }
  }
  
  if (routeCount == 0) {
    oledDisplay->setCursor(0, 16);
    oledDisplay->print("No routes found");
    oledDisplay->setCursor(100, 56);
    oledDisplay->print("B:OK");
    return;
  }
  
  // Display routes with type indicator
  int itemsPerPage = 5;
  for (int i = 0; i < itemsPerPage && (i + gRoutesScrollOffset) < routeCount; i++) {
    int idx = i + gRoutesScrollOffset;
    oledDisplay->setCursor(0, 10 + i * 10);
    
    if (idx == gRoutesSelectedIdx) {
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    // Type indicator: R=Rail, B=Bus, F=Ferry
    char typeChar = 'R';
    if (routes[idx].type == 0x21) typeChar = 'B';
    else if (routes[idx].type == 0x22) typeChar = 'F';
    
    char nameBuf[22];
    snprintf(nameBuf, sizeof(nameBuf), "%c %s", typeChar, routes[idx].name);
    nameBuf[21] = '\0';
    oledDisplay->print(nameBuf);
  }
  
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, 56);
  oledDisplay->print("^v:Sel");
  oledDisplay->setCursor(42, 56);
  oledDisplay->print("A:Go X:Hl");
  oledDisplay->setCursor(105, 56);
  oledDisplay->print("B:X");
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

// Check if a line looks like GPS track data
static bool isGPSDataLine(const String& line) {
  // Format 1: General sensor log with "gps:" marker
  if (line.indexOf("gps:") >= 0 && line.indexOf("lat=") >= 0) {
    return true;
  }
  // Format 2: Dedicated GPS track CSV (timestamp,lat,lon,...)
  // Look for numeric pattern: digits,float,float
  if (line.length() > 10 && line.charAt(0) != '#') {
    int firstComma = line.indexOf(',');
    int secondComma = line.indexOf(',', firstComma + 1);
    if (firstComma > 0 && secondComma > firstComma) {
      // Check if second field looks like a latitude (-90 to 90)
      String latStr = line.substring(firstComma + 1, secondComma);
      float lat = latStr.toFloat();
      if (lat >= -90.0f && lat <= 90.0f && lat != 0.0f) {
        return true;
      }
    }
  }
  return false;
}

// Scan for GPS track files in /logs and /logs/tracks
static void scanTrackFiles() {
  gTrackFileCount = 0;
  
  // Scan directories
  const char* dirs[] = {"/logs", "/logs/tracks"};
  
  for (int d = 0; d < 2 && gTrackFileCount < 8; d++) {
    File root = LittleFS.open(dirs[d]);
    if (!root || !root.isDirectory()) continue;
    
    File file = root.openNextFile();
    while (file && gTrackFileCount < 8) {
      if (!file.isDirectory()) {
        // Check if file has GPS data
        File check = LittleFS.open(file.path(), "r");
        if (check) {
          bool hasGPS = false;
          for (int i = 0; i < 15 && check.available(); i++) {
            String line = check.readStringUntil('\n');
            if (isGPSDataLine(line)) {
              hasGPS = true;
              break;
            }
          }
          check.close();
          
          if (hasGPS) {
            strlcpy(gTrackFiles[gTrackFileCount], file.path(), 64);
            gTrackFileCount++;
          }
        }
      }
      file = root.openNextFile();
    }
  }
}

// Draw track file selection submenu
static void drawTrackSelect() {
  if (!oledDisplay) return;
  
  oledDisplay->fillRect(10, 5, 108, 48, DISPLAY_COLOR_BLACK);
  oledDisplay->drawRect(10, 5, 108, 48, DISPLAY_COLOR_WHITE);
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(14, 8);
  oledDisplay->print(gTrackSelectMode == 1 ? "= Load Track =" : "= Delete Track =");
  
  if (gTrackFileCount == 0) {
    oledDisplay->setCursor(14, 22);
    oledDisplay->print("No GPS logs found");
    oledDisplay->setCursor(14, 40);
    oledDisplay->print("B:Cancel");
    return;
  }
  
  if (gTrackFileIdx < gTrackFileCount) {
    oledDisplay->fillRect(12, 20, 104, 12, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    oledDisplay->setCursor(14, 22);
    // Show just filename, not full path
    const char* filename = strrchr(gTrackFiles[gTrackFileIdx], '/');
    if (filename) filename++; else filename = gTrackFiles[gTrackFileIdx];
    char buf[32];
    snprintf(buf, sizeof(buf), "%d/%d: %.10s", gTrackFileIdx + 1, gTrackFileCount, filename);
    oledDisplay->print(buf);
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
  
  if (gShowTrackStatus) {
    drawTrackStatus();
    return;
  }
  
  if (gShowFeatures) {
    drawFeatures();
    return;
  }
  
  if (gShowRoutes) {
    drawRoutes();
    return;
  }
  
  if (gWaypointSelectMode != 0) {
    drawWaypointSelect();
    return;
  }
  
  if (gTrackSelectMode != 0) {
    drawTrackSelect();
    return;
  }
  
  oledDisplay->fillRect(10, 5, 108, 48, DISPLAY_COLOR_BLACK);
  oledDisplay->drawRect(10, 5, 108, 48, DISPLAY_COLOR_WHITE);
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(14, 8);
  
  // Get current menu items based on submenu level
  const char** menuItems;
  int menuItemCount;
  const char* menuTitle;
  
  if (gMapSubmenuLevel == 0) {
    menuItems = gMapMainMenuItems;
    menuItemCount = gMapMainMenuItemCount;
    menuTitle = "== Map Menu ==";
  } else {
    menuTitle = "== Submenu ==";
    switch (gMapSubmenuType) {
      case MENU_CAT_VIEW:
        menuItems = gViewSubmenu;
        menuItemCount = gViewSubmenuCount;
        menuTitle = "== View ==";
        break;
      case MENU_CAT_MAPS:
        menuItems = gMapsSubmenu;
        menuItemCount = gMapsSubmenuCount;
        menuTitle = "== Maps ==";
        break;
#if ENABLE_GPS_SENSOR
      case MENU_CAT_GPS:
        menuItems = gGPSSubmenu;
        menuItemCount = gGPSSubmenuCount;
        menuTitle = "== GPS ==";
        break;
#endif
      case MENU_CAT_WAYPOINTS:
        menuItems = gWaypointsSubmenu;
        menuItemCount = gWaypointsSubmenuCount;
        menuTitle = "== Waypoints ==";
        break;
      case MENU_CAT_TRACKS:
        menuItems = gTracksSubmenu;
        menuItemCount = gTracksSubmenuCount;
        menuTitle = "== Tracks ==";
        break;
      case MENU_CAT_INFO:
        menuItems = gInfoSubmenu;
        menuItemCount = gInfoSubmenuCount;
        menuTitle = "== Info ==";
        break;
      default:
        menuItems = gMapMainMenuItems;
        menuItemCount = gMapMainMenuItemCount;
        menuTitle = "== Map Menu ==";
        break;
    }
  }
  
  oledDisplay->print(menuTitle);
  
  const int maxVisible = 4;
  const int itemHeight = 9;
  const int startY = 18;
  
  for (int i = 0; i < maxVisible && (i + gMapMenuScrollOffset) < menuItemCount; i++) {
    int itemIdx = i + gMapMenuScrollOffset;
    int y = startY + (i * itemHeight);
    
    if (itemIdx == gMapMenuSelection) {
      oledDisplay->fillRect(12, y - 1, 104, itemHeight, DISPLAY_COLOR_WHITE);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    oledDisplay->setCursor(14, y);
    oledDisplay->print(menuItems[itemIdx]);
  }
  
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  if (gMapMenuScrollOffset > 0) {
    oledDisplay->setCursor(112, 18);
    oledDisplay->print("^");
  }
  if (gMapMenuScrollOffset + maxVisible < menuItemCount) {
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
  // Use cached GPS data (gpsTask continuously polls and updates gPA1010D)
  // Do NOT call gPA1010D->read() here - causes I2C bus contention with OLED
  if (gpsConnected && gPA1010D != nullptr && gpsEnabled) {
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
    char maps[8][96];
    int mapCount = MapCore::getAvailableMaps(maps, 8);
    if (mapCount > 0) {
      char path[128];
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
  
  // Render GPS track if loaded
  if (GPSTrackManager::hasTrack()) {
    // Calculate scale factors from map bounds (same logic as MapCore::renderMap)
    int32_t mapWidth = map.header.maxLon - map.header.minLon;
    int32_t mapHeight = map.header.maxLat - map.header.minLat;
    int32_t scaleX = (mapWidth > 0) ? (gOLEDMapRenderer->getWidth() * 1000) / mapWidth : 1;
    int32_t scaleY = (mapHeight > 0) ? (gOLEDMapRenderer->getHeight() * 1000) / mapHeight : 1;
    scaleX = (int32_t)(scaleX * gMapZoom);
    scaleY = (int32_t)(scaleY * gMapZoom);
    GPSTrackManager::renderTrack(gOLEDMapRenderer, gMapCenterLat, gMapCenterLon, scaleX, scaleY);
  }
  
  // Update location context if GPS has fix and enough time has passed
  if (hasGPSFix && LocationContextManager::shouldUpdate(lat, lon)) {
    LocationContextManager::updateContext(lat, lon);
  }
  
  // Draw context bar at top with location context
  const LocationContext& ctx = LocationContextManager::getContext();
  if (ctx.valid && hasGPSFix) {
    char contextBuf[128];
    int pos = 0;
    
    // Build context string: "On [road] • Near [area] ([distance])"
    if (ctx.nearestRoad[0] != '\0') {
      pos += snprintf(contextBuf + pos, sizeof(contextBuf) - pos, "On %s", ctx.nearestRoad);
    }
    
    if (ctx.nearestArea[0] != '\0') {
      if (pos > 0) {
        pos += snprintf(contextBuf + pos, sizeof(contextBuf) - pos, " • Near %s", ctx.nearestArea);
      } else {
        pos += snprintf(contextBuf + pos, sizeof(contextBuf) - pos, "Near %s", ctx.nearestArea);
      }
      
      // Add distance if close enough to be relevant
      if (ctx.areaDistanceM < 500.0f) {
        pos += snprintf(contextBuf + pos, sizeof(contextBuf) - pos, " (%.0fm)", ctx.areaDistanceM);
      }
    }
    
    // Update scroll offset for animation
    uint32_t now = millis();
    if (now - gContextScrollLastUpdate >= CONTEXT_SCROLL_SPEED_MS) {
      gContextScrollOffset += 2;  // Scroll 2 pixels at a time
      gContextScrollLastUpdate = now;
    }
    
    if (pos > 0) {
      gOLEDMapRenderer->drawContextBar(contextBuf, gContextScrollOffset);
    }
  } else {
    // No context - just show region name
    char regionBuf[24];
    snprintf(regionBuf, sizeof(regionBuf), " %.8s ", map.header.regionName);
    gOLEDMapRenderer->drawOverlayText(0, 0, regionBuf, true);
  }
  
  char overlayBuf[32];
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
  
  // Show LIVE indicator when live tracking
  if (GPSTrackManager::isLiveTracking()) {
    gOLEDMapRenderer->drawOverlayText(0, 56, " LIVE ", true);
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
  
  // Show search results navigation indicator
  if (gSearchResultsActive && gSearchResultCount > 1) {
    snprintf(overlayBuf, sizeof(overlayBuf), " %d/%d <> ", gSearchResultCurrent + 1, gSearchResultCount);
    gOLEDMapRenderer->drawOverlayText(40, 56, overlayBuf, true);
  }
  
  if (gMapMenuOpen) {
    drawMapMenu();
  }
}

static bool gpsMapAvailable(String* outReason) {
  return true;
}

// Go back to main menu from submenu
static void goBackToMainMenu() {
  gMapSubmenuLevel = 0;
  gMapMenuSelection = gMapSubmenuType;
  gMapMenuScrollOffset = 0;
}

// Execute submenu action based on current submenu type and selection
static void executeSubmenuAction(int submenuType, int action) {
  char maps[8][96];
  int mapCount;
  int currentIdx;
  const LoadedMap& currentMap = MapCore::getCurrentMap();
  
  switch (submenuType) {
    case MENU_CAT_VIEW:
      switch (action) {
        case 0: gMapZoom = fminf(gMapZoom * MAP_ZOOM_STEP, MAP_ZOOM_MAX); break;  // Zoom In
        case 1: gMapZoom = fmaxf(gMapZoom / MAP_ZOOM_STEP, MAP_ZOOM_MIN); break;  // Zoom Out
        case 2: gMapZoom = 1.0f; break;  // Reset Zoom
        case 3: gMapRotation = fmodf(gMapRotation - 15.0f + 360.0f, 360.0f); break;  // Rotate Left
        case 4: gMapRotation = fmodf(gMapRotation + 15.0f, 360.0f); break;  // Rotate Right
        case 5: gMapRotation = 0.0f; break;  // Reset Rotation
        case 6: goBackToMainMenu(); break;  // Back
      }
      break;
      
    case MENU_CAT_MAPS:
      switch (action) {
        case 0:  // Select Map
          {
            extern class FileManager* gOLEDFileManager;
            extern bool oledFileBrowserNeedsInit;
            extern void pushOLEDMode(OLEDMode mode);
            pushOLEDMode(currentOLEDMode);  // Push so B returns here
            currentOLEDMode = OLED_FILE_BROWSER;
            if (gOLEDFileManager) gOLEDFileManager->navigate("/maps");
            else oledFileBrowserNeedsInit = true;
            gMapMenuOpen = false;
            gMapSubmenuLevel = 0;
          }
          break;
        case 1:  // Next Map
          mapCount = MapCore::getAvailableMaps(maps, 8);
          if (mapCount > 0) {
            currentIdx = -1;
            char currentRel[96] = {0};
            if (strncmp(currentMap.filepath, "/maps/", 6) == 0) {
              strlcpy(currentRel, currentMap.filepath + 6, sizeof(currentRel));
            } else {
              const char* fn = strrchr(currentMap.filepath, '/');
              if (fn) strlcpy(currentRel, fn + 1, sizeof(currentRel));
            }
            for (int i = 0; i < mapCount; i++) {
              if (strcmp(maps[i], currentRel) == 0) { currentIdx = i; break; }
            }
            char path[128];
            snprintf(path, sizeof(path), "/maps/%s", maps[(currentIdx + 1) % mapCount]);
            MapCore::loadMapFile(path);
            gMapCenterSet = false;
            gMapManuallyPanned = false;
          }
          break;
        case 2:  // Previous Map
          mapCount = MapCore::getAvailableMaps(maps, 8);
          if (mapCount > 0) {
            currentIdx = -1;
            char currentRel[96] = {0};
            if (strncmp(currentMap.filepath, "/maps/", 6) == 0) {
              strlcpy(currentRel, currentMap.filepath + 6, sizeof(currentRel));
            } else {
              const char* fn = strrchr(currentMap.filepath, '/');
              if (fn) strlcpy(currentRel, fn + 1, sizeof(currentRel));
            }
            for (int i = 0; i < mapCount; i++) {
              if (strcmp(maps[i], currentRel) == 0) { currentIdx = i; break; }
            }
            char path[128];
            snprintf(path, sizeof(path), "/maps/%s", maps[(currentIdx - 1 + mapCount) % mapCount]);
            MapCore::loadMapFile(path);
            gMapCenterSet = false;
            gMapManuallyPanned = false;
          }
          break;
        case 3:  // Recenter
          if (currentMap.valid) {
            gMapCenterLat = (currentMap.header.minLat + currentMap.header.maxLat) / 2000000.0f;
            gMapCenterLon = (currentMap.header.minLon + currentMap.header.maxLon) / 2000000.0f;
            gMapCenterSet = true;
            gMapManuallyPanned = false;
          }
          break;
        case 4: goBackToMainMenu(); break;  // Back
      }
      break;

#if ENABLE_GPS_SENSOR
    case MENU_CAT_GPS:
      switch (action) {
        case 0:  // Center on GPS
          if (gpsConnected && gPA1010D != nullptr && gPA1010D->fix) {
            gMapCenterLat = gPA1010D->latitudeDegrees;
            gMapCenterLon = gPA1010D->longitudeDegrees;
            if (gPA1010D->lat == 'S') gMapCenterLat = -gMapCenterLat;
            if (gPA1010D->lon == 'W') gMapCenterLon = -gMapCenterLon;
            gMapCenterSet = true;
            gMapManuallyPanned = false;
          }
          break;
        case 1:  // Toggle GPS
          {
            extern bool enqueueSensorStart(SensorType sensor);
            extern bool isInQueue(SensorType sensor);
            if (gpsEnabled) gpsEnabled = false;
            else if (!isInQueue(SENSOR_GPS)) enqueueSensorStart(SENSOR_GPS);
          }
          break;
        case 2: goBackToMainMenu(); break;  // Back
      }
      break;
#endif

    case MENU_CAT_WAYPOINTS:
      switch (action) {
        case 0:  // Mark Waypoint
          {
            char defaultName[WAYPOINT_NAME_LEN];
            snprintf(defaultName, sizeof(defaultName), "WP%d", WaypointManager::getActiveCount());
            oledKeyboardInit("Name Waypoint", defaultName, WAYPOINT_NAME_LEN - 1);
            gOLEDKeyboardState.active = true;
            gMapMenuOpen = false;
            gMapSubmenuLevel = 0;
          }
          break;
        case 1:  // Goto Waypoint
          if (WaypointManager::getActiveCount() > 0) {
            gWaypointSelectMode = 1;
            gWaypointSelectIdx = 0;
          }
          break;
        case 2:  // Clear Nav
          WaypointManager::selectTarget(-1);
          break;
        case 3:  // Delete Waypoint
          if (WaypointManager::getActiveCount() > 0) {
            gWaypointSelectMode = 2;
            gWaypointSelectIdx = 0;
          }
          break;
        case 4: goBackToMainMenu(); break;  // Back
      }
      break;

    case MENU_CAT_TRACKS:
      switch (action) {
        case 0:  // Load Track
          scanTrackFiles();
          if (gTrackFileCount > 0) {
            gTrackSelectMode = 1;
            gTrackFileIdx = 0;
          }
          break;
        case 1:  // Clear Track
          GPSTrackManager::clearTrack();
          break;
        case 2:  // Track Status
          gShowTrackStatus = true;
          break;
        case 3:  // Delete Track
          scanTrackFiles();
          if (gTrackFileCount > 0) {
            gTrackSelectMode = 2;  // Delete mode
            gTrackFileIdx = 0;
          }
          break;
        case 4:  // Live Track - toggle
          if (GPSTrackManager::isLiveTracking()) {
            GPSTrackManager::setLiveTracking(false);
          } else {
            GPSTrackManager::clearTrack();  // Start fresh
            GPSTrackManager::setLiveTracking(true);
          }
          break;
        case 5: goBackToMainMenu(); break;  // Back
      }
      break;

    case MENU_CAT_INFO:
      switch (action) {
        case 0:  // Map Info
          gShowMapInfo = true;
          break;
        case 1:  // Features
          gShowFeatures = true;
          gFeaturesCategory = 0;
          gFeaturesScrollOffset = 0;
          break;
        case 2:  // Search Names
          gMapSearchMode = true;
          gMapMenuOpen = false;
          oledKeyboardInit("Search:", "", 20);
          oledKeyboardSetAutocomplete(mapNameAutocomplete, nullptr);
          break;
        case 3:  // Transit Routes
          gShowRoutes = true;
          gRoutesScrollOffset = 0;
          break;
        case 4: goBackToMainMenu(); break;  // Back
      }
      break;
  }
}

// Execute main menu action (opens submenus or closes)
static void executeMainMenuAction(int action) {
#if ENABLE_GPS_SENSOR
  // With GPS: View, Maps, GPS, Waypoints, Tracks, Info, Close
  switch (action) {
    case MENU_CAT_VIEW:
    case MENU_CAT_MAPS:
    case MENU_CAT_GPS:
    case MENU_CAT_WAYPOINTS:
    case MENU_CAT_TRACKS:
    case MENU_CAT_INFO:
      gMapSubmenuLevel = 1;
      gMapSubmenuType = action;
      gMapMenuSelection = 0;
      gMapMenuScrollOffset = 0;
      break;
    case MENU_CAT_CLOSE:
      gMapMenuOpen = false;
      gMapSubmenuLevel = 0;
      break;
  }
#else
  // Without GPS: View, Maps, Waypoints, Tracks, Info, Close
  int mappedAction = action;
  if (action >= 2) mappedAction = action + 1;  // Skip GPS category
  
  switch (mappedAction) {
    case MENU_CAT_VIEW:
    case MENU_CAT_MAPS:
    case MENU_CAT_WAYPOINTS:
    case MENU_CAT_TRACKS:
    case MENU_CAT_INFO:
      gMapSubmenuLevel = 1;
      gMapSubmenuType = mappedAction;
      gMapMenuSelection = 0;
      gMapMenuScrollOffset = 0;
      break;
    default:
      gMapMenuOpen = false;
      gMapSubmenuLevel = 0;
      break;
  }
#endif
}

// Execute a menu action based on current menu level
static void executeMapMenuAction(int action) {
  if (gMapSubmenuLevel == 0) {
    executeMainMenuAction(action);
  } else {
    executeSubmenuAction(gMapSubmenuType, action);
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
      gMapSubmenuLevel = 0;  // Reset to main menu
    }
    return true;
  }
  
  // Search mode - keyboard handles input
  if (gMapSearchMode) {
    if (oledKeyboardIsActive()) {
      oledKeyboardHandleInput(deltaX, deltaY, newlyPressed);
      return true;
    }
    
    // Keyboard completed or cancelled
    if (oledKeyboardIsCompleted()) {
      strncpy(gSearchResult, oledKeyboardGetText(), sizeof(gSearchResult) - 1);
      gSearchResult[sizeof(gSearchResult) - 1] = '\0';
      Serial.printf("[MAP_SEARCH] Selected: '%s'\n", gSearchResult);
      
      // Find ALL matching features (v5 tiled format - iterate through tiles)
      gSearchResultCount = 0;
      gSearchResultCurrent = 0;
      const LoadedMap& map = MapCore::getCurrentMap();
      if (map.valid && map.tileDir && gSearchResult[0] != '\0') {
        // Iterate through all tiles
        for (uint16_t tileIdx = 0; tileIdx < map.tileCount && gSearchResultCount < 32; tileIdx++) {
          size_t tileDataSize;
          const uint8_t* tileData = MapCore::loadTileData(tileIdx, &tileDataSize);
          if (!tileData || tileDataSize == 0) continue;
          
          // Calculate tile halo bounds for dequantization
          int tx = tileIdx % map.tileGridSize;
          int ty = tileIdx / map.tileGridSize;
          int32_t tileMinLon = map.header.minLon + tx * map.tileW - map.haloW;
          int32_t tileMinLat = map.header.minLat + ty * map.tileH - map.haloH;
          int32_t haloLonSpan = map.tileW + 2 * map.haloW;
          int32_t haloLatSpan = map.tileH + 2 * map.haloH;
          
          const uint8_t* ptr = tileData;
          const uint8_t* end = tileData + tileDataSize;
          
          // Feature count is at the START of each tile's payload (2 bytes)
          if (ptr + 2 > end) continue;
          uint16_t featureCount = ptr[0] | (ptr[1] << 8);
          ptr += 2;
          
          const int hdrSize = HWMAP_FEATURE_HEADER_SIZE(map.header.version);
          for (uint16_t f = 0; f < featureCount && gSearchResultCount < 32; f++) {
            if (ptr + hdrSize > end) break;
            
            // v6: type(1) + subtype(1) + nameIndex(2) + pointCount(2)
            // v5: type(1) + nameIndex(2) + pointCount(2)
            uint16_t nameIndex = (hdrSize == 6) ? (ptr[2] | (ptr[3] << 8)) : (ptr[1] | (ptr[2] << 8));
            uint16_t pointCount = (hdrSize == 6) ? (ptr[4] | (ptr[5] << 8)) : (ptr[3] | (ptr[4] << 8));
            
            const char* featureName = MapCore::getName(nameIndex);
            if (featureName && strcmp(featureName, gSearchResult) == 0 && pointCount > 0) {
              // Dequantize first point and store coordinates (points start after header)
              const uint8_t* pointsPtr = ptr + hdrSize;
              uint16_t qLat = pointsPtr[0] | (pointsPtr[1] << 8);
              uint16_t qLon = pointsPtr[2] | (pointsPtr[3] << 8);
              int32_t lat = tileMinLat + (int32_t)((int64_t)qLat * haloLatSpan / 65535);
              int32_t lon = tileMinLon + (int32_t)((int64_t)qLon * haloLonSpan / 65535);
              gSearchResultCoords[gSearchResultCount].lat = lat / 1000000.0f;
              gSearchResultCoords[gSearchResultCount].lon = lon / 1000000.0f;
              gSearchResultCount++;
            }
            
            ptr += hdrSize + pointCount * 4;  // Skip to next feature
          }
        }
        
        // Center on first result
        if (gSearchResultCount > 0) {
          gMapCenterLat = gSearchResultCoords[0].lat;
          gMapCenterLon = gSearchResultCoords[0].lon;
          gMapCenterSet = true;
          gMapManuallyPanned = true;
          gSearchResultsActive = (gSearchResultCount > 1);
          
          Serial.printf("[MAP_SEARCH] Found %d matches for '%s', showing 1/%d\n", 
                       gSearchResultCount, gSearchResult, gSearchResultCount);
        }
      }
    }
    gMapSearchMode = false;
    oledKeyboardReset();
    return true;
  }
  
  // Search results navigation (when multiple matches exist)
  if (gSearchResultsActive && gSearchResultCount > 1) {
    // Left/Right to navigate between results
    if (newlyPressed & 0x08) {  // Left - previous result
      gSearchResultCurrent = (gSearchResultCurrent - 1 + gSearchResultCount) % gSearchResultCount;
      goto centerOnCurrentResult;
    }
    if (newlyPressed & 0x04) {  // Right - next result
      gSearchResultCurrent = (gSearchResultCurrent + 1) % gSearchResultCount;
      goto centerOnCurrentResult;
    }
    if (newlyPressed & 0x02) {  // B button - exit search results mode
      gSearchResultsActive = false;
      return true;
    }
    
    centerOnCurrentResult: {
      // Use stored coordinates directly (v5 format stores coords, not indices)
      gMapCenterLat = gSearchResultCoords[gSearchResultCurrent].lat;
      gMapCenterLon = gSearchResultCoords[gSearchResultCurrent].lon;
      Serial.printf("[MAP_SEARCH] Showing result %d/%d\n", 
                   gSearchResultCurrent + 1, gSearchResultCount);
      return true;
    }
  }
  
  // Map info overlay - any button dismisses
  if (gShowMapInfo) {
    if (newlyPressed & (0x20 | 0x02)) {
      gShowMapInfo = false;
      gMapMenuOpen = false;
      gMapSubmenuLevel = 0;
      return true;
    }
    return true;
  }
  
  // Track status overlay - any button dismisses
  if (gShowTrackStatus) {
    if (newlyPressed & (0x20 | 0x02)) {
      gShowTrackStatus = false;
      gMapMenuOpen = false;
      gMapSubmenuLevel = 0;
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
      gMapSubmenuLevel = 0;
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
    
    if (map.valid && map.nameCount > 0) {
      if (deltaY < -menuDeadzone && gFeaturesScrollOffset > 0) {
        gFeaturesScrollOffset--;
        return true;
      }
      if (deltaY > menuDeadzone && gFeaturesScrollOffset < map.nameCount - 5) {
        gFeaturesScrollOffset++;
        return true;
      }
    }
    
    return true;
  }
  
  // Routes viewer input handling
  if (gShowRoutes) {
    const int menuDeadzone = 5;
    
    if (newlyPressed & 0x02) {  // B button - close
      gShowRoutes = false;
      gMapMenuOpen = false;
      gMapSubmenuLevel = 0;
      return true;
    }
    
    // Build route list (same logic as drawRoutes) using v5 tiled architecture
    const LoadedMap& map = MapCore::getCurrentMap();
    static RouteInfo routeList[32];
    static float routeFirstLat[32];  // Store first point coords for goto
    static float routeFirstLon[32];
    int routeCount = 0;
    if (map.valid && map.tileDir) {
      for (uint16_t tileIdx = 0; tileIdx < map.tileCount && routeCount < 32; tileIdx++) {
        size_t tileDataSize;
        const uint8_t* tileData = MapCore::loadTileData(tileIdx, &tileDataSize);
        if (!tileData || tileDataSize == 0) continue;
        
        // Calculate tile halo bounds for dequantization
        int tx = tileIdx % map.tileGridSize;
        int ty = tileIdx / map.tileGridSize;
        int32_t tileMinLon = map.header.minLon + tx * map.tileW - map.haloW;
        int32_t tileMinLat = map.header.minLat + ty * map.tileH - map.haloH;
        int32_t haloLonSpan = map.tileW + 2 * map.haloW;
        int32_t haloLatSpan = map.tileH + 2 * map.haloH;
        
        const uint8_t* ptr = tileData;
        const uint8_t* end = tileData + tileDataSize;
        
        // Feature count is at the START of each tile's payload (2 bytes)
        if (ptr + 2 > end) continue;
        uint16_t featureCount = ptr[0] | (ptr[1] << 8);
        ptr += 2;
        
        const int hdrSize = HWMAP_FEATURE_HEADER_SIZE(map.header.version);
        for (uint16_t f = 0; f < featureCount && routeCount < 32; f++) {
          if (ptr + hdrSize > end) break;
          
          uint8_t type = ptr[0];
          // v6: type(1) + subtype(1) + nameIndex(2) + pointCount(2)
          // v5: type(1) + nameIndex(2) + pointCount(2)
          uint16_t nameIdx = (hdrSize == 6) ? (ptr[2] | (ptr[3] << 8)) : (ptr[1] | (ptr[2] << 8));
          uint16_t pointCount = (hdrSize == 6) ? (ptr[4] | (ptr[5] << 8)) : (ptr[3] | (ptr[4] << 8));
          
          // Check for transit types (rail, bus, ferry)
          if ((type == 0x20 || type == 0x21 || type == 0x22) && nameIdx != 0xFFFF && pointCount > 0) {
            const char* name = MapCore::getName(nameIdx);
            if (name && name[0]) {
              bool dup = false;
              for (int i = 0; i < routeCount; i++) {
                if (strcmp(routeList[i].name, name) == 0 && routeList[i].type == type) { dup = true; break; }
              }
              if (!dup) {
                // Dequantize first point for goto functionality (points start after header)
                const uint8_t* pointsPtr = ptr + hdrSize;
                uint16_t qLat = pointsPtr[0] | (pointsPtr[1] << 8);
                uint16_t qLon = pointsPtr[2] | (pointsPtr[3] << 8);
                int32_t lat = tileMinLat + (int32_t)((int64_t)qLat * haloLatSpan / 65535);
                int32_t lon = tileMinLon + (int32_t)((int64_t)qLon * haloLonSpan / 65535);
                
                routeList[routeCount].name = name;
                routeList[routeCount].type = type;
                routeFirstLat[routeCount] = lat / 1000000.0f;
                routeFirstLon[routeCount] = lon / 1000000.0f;
                routeCount++;
              }
            }
          }
          
          ptr += hdrSize + pointCount * 4;  // Skip to next feature
        }
      }
    }
    
    // Navigate selection
    if (deltaY < -menuDeadzone && gRoutesSelectedIdx > 0) {
      gRoutesSelectedIdx--;
      if (gRoutesSelectedIdx < gRoutesScrollOffset) gRoutesScrollOffset = gRoutesSelectedIdx;
      return true;
    }
    if (deltaY > menuDeadzone && gRoutesSelectedIdx < routeCount - 1) {
      gRoutesSelectedIdx++;
      if (gRoutesSelectedIdx >= gRoutesScrollOffset + 5) gRoutesScrollOffset = gRoutesSelectedIdx - 4;
      return true;
    }
    
    // X button - highlight route on map (toggle blink)
    if (newlyPressed & 0x04) {
      if (map.valid && routeCount > 0 && gRoutesSelectedIdx < routeCount) {
        if (gMapHighlight.active && strcmp(gMapHighlight.name, routeList[gRoutesSelectedIdx].name) == 0) {
          // Toggle off if same route
          mapHighlightClear();
        } else {
          // Enable highlighting for selected route using generic system
          mapHighlightByNameAndType(routeList[gRoutesSelectedIdx].name, 
                                     routeList[gRoutesSelectedIdx].type, 300);
        }
        gShowRoutes = false;
        gMapMenuOpen = false;
        gMapSubmenuLevel = 0;
        return true;
      }
    }
    
    // A button - goto selected route (center on first point)
    if (newlyPressed & 0x20) {
      if (map.valid && routeCount > 0 && gRoutesSelectedIdx < routeCount) {
        // Use stored coordinates directly (v5 format)
        gMapCenterLat = routeFirstLat[gRoutesSelectedIdx];
        gMapCenterLon = routeFirstLon[gRoutesSelectedIdx];
        gMapCenterSet = true;
        gMapManuallyPanned = true;
        gShowRoutes = false;
        gMapMenuOpen = false;
        gMapSubmenuLevel = 0;
        return true;
      }
    }
    
    return true;
  }
  
  // Track file selection mode input handling
  if (gTrackSelectMode != 0) {
    const int menuDeadzone = 5;
    
    if (newlyPressed & 0x20) {  // A button: load or delete track
      if (gTrackFileCount > 0 && gTrackFileIdx < gTrackFileCount) {
        if (gTrackSelectMode == 1) {
          // Load track
          String errorMsg;
          if (GPSTrackManager::loadTrack(gTrackFiles[gTrackFileIdx], errorMsg)) {
            float coverage;
            TrackValidation validation = GPSTrackManager::validateTrack(coverage);
            if (validation == TRACK_OUT_OF_BOUNDS) {
              GPSTrackManager::clearTrack();
            }
          }
        } else if (gTrackSelectMode == 2) {
          // Delete track
          GPSTrackManager::deleteTrackFile(gTrackFiles[gTrackFileIdx]);
        }
      }
      gTrackSelectMode = 0;
      gMapMenuOpen = false;
      gMapSubmenuLevel = 0;
      return true;
    }
    
    if (newlyPressed & 0x02) {  // B button: cancel
      gTrackSelectMode = 0;
      return true;
    }
    
    // Navigate track files with joystick
    if (deltaY < -menuDeadzone && gTrackFileIdx > 0) {
      gTrackFileIdx--;
      return true;
    }
    if (deltaY > menuDeadzone && gTrackFileIdx < gTrackFileCount - 1) {
      gTrackFileIdx++;
      return true;
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
      gMapSubmenuLevel = 0;
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
    
    // Get item count for current menu level
    int menuItemCount;
    if (gMapSubmenuLevel == 0) {
      menuItemCount = gMapMainMenuItemCount;
    } else {
      switch (gMapSubmenuType) {
        case MENU_CAT_VIEW: menuItemCount = gViewSubmenuCount; break;
        case MENU_CAT_MAPS: menuItemCount = gMapsSubmenuCount; break;
#if ENABLE_GPS_SENSOR
        case MENU_CAT_GPS: menuItemCount = gGPSSubmenuCount; break;
#endif
        case MENU_CAT_WAYPOINTS: menuItemCount = gWaypointsSubmenuCount; break;
        case MENU_CAT_TRACKS: menuItemCount = gTracksSubmenuCount; break;
        case MENU_CAT_INFO: menuItemCount = gInfoSubmenuCount; break;
        default: menuItemCount = gMapMainMenuItemCount; break;
      }
    }
    
    if (newlyPressed & 0x20) {
      executeMapMenuAction(gMapMenuSelection);
      return true;
    }
    
    if (newlyPressed & 0x02) {
      // B button: go back to main menu or close
      if (gMapSubmenuLevel > 0) {
        goBackToMainMenu();
      } else {
        gMapMenuOpen = false;
      }
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
    if (deltaY > menuDeadzone && gMapMenuSelection < menuItemCount - 1) {
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
