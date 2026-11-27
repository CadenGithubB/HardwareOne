// ============================================================================
// OLED Map Mode
// ============================================================================
// Extracted from GPS_MapRenderer.cpp for modularity
// Contains all OLED-specific map rendering and menu code

#include "OLED_Display.h"
#include "System_CommandReply.h"  // the reply words and how to read them
#include "System_MemUtil.h"  // ps_alloc / AllocPref
#include "System_BuildConfig.h"

// Gate the ENTIRE Map mode on ENABLE_MAPS — when maps aren't compiled in
// there's nothing meaningful for the page to do (it can't load tiles, can't
// draw POIs, can't compute viewports), but the OLED mode entry would still
// register and surface "Map" in the hardware menu. gpsMapAvailable() used
// to be a no-op (always-true) which made the menu entry undismissable on
// builds where ENABLE_MAPS=0. Gating at the file level is cleaner than
// patching availability: when the feature is off, no symbol is emitted, no
// registration runs, the menu entry simply doesn't exist.
#if ENABLE_OLED_DISPLAY && ENABLE_MAPS

#include <Adafruit_SSD1306.h>
#include <cstdarg>    // va_list (context bar append)
#include <cstring>
#include <strings.h>  // strcasecmp (map picker filter)
#include "System_Maps.h"
#include "System_Mutex.h"   // MapCacheGuard — serialize tile-cache reads vs mapRenderTask
#include "OLED_Utils.h"
#include "OLED_HeaderCore.h"  // OledHeaderSpec: popup titles in the header bar
#include "OLED_UI.h"  // Shared command-failure toasts
#include "System_FileManager.h"
#include "i2csensor_seesaw.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_Command.h"
#include "HAL_Input.h"
#include "System_TaskUtils.h"
#include "System_VFS.h"
#include "System_CaptureCrypto.h"  // a sealed track's rows, revealed for the probe
#include "System_Filesystem.h"     // normalizeFsPath: the class a sealed row opens with
#include "System_AuthIdentity.h"  // CommandIdentityScope (OLEDMapCtxGuard)

#include "i2csensor_pa1010d.h"

#if ENABLE_ESPNOW
#include "System_ESPNow_Sensors.h"
#endif

// OLED identity for Map work. The Map owners (MapCore, WaypointManager,
// GpsTrackManager) check file access against currentAuthContext(), and the
// main-loop input/render path that runs this mode installs no identity (an
// empty user is refused). Every direct owner call below therefore runs inside
// an OledMapCtxGuard: the composed identity + NOTIF_SOURCE_OLED scope, as
// OledFileBrowserCtxGuard does for the file browser. Installed per action,
// not per frame.
static AuthContext oledMapAuthContext() {
  return oledAuthContext("/oled/map");
}

class OledMapCtxGuard {
 public:
  OledMapCtxGuard() : scope_(oledMapAuthContext()) {}
  ~OledMapCtxGuard() = default;
  OledMapCtxGuard(const OledMapCtxGuard&)            = delete;
  OledMapCtxGuard& operator=(const OledMapCtxGuard&) = delete;
  OledMapCtxGuard(OledMapCtxGuard&&)                 = delete;
  OledMapCtxGuard& operator=(OledMapCtxGuard&&)      = delete;
 private:
  CommandIdentityScope scope_;
};

// External OLED display pointer (provided via HAL_Display.h #define oledDisplay gDisplay)

// Singleton renderer for OLED (used only for overlays now)
static OledMapRenderer* sOledMapRenderer = nullptr;

// =============================================================================
// Async Double-Buffer Rendering
// =============================================================================

// Sized for the physical panel; a portrait raster (64 x 128 logical) has the
// same pixel count and packs into the same 1024 B.
static const size_t kSSD1306BufSize = DISPLAY_WIDTH * ((DISPLAY_HEIGHT + 7) / 8);
static const size_t kMapShadeBufSize = DISPLAY_WIDTH * DISPLAY_HEIGHT;  // one shade byte per pixel

// Popups (waypoint and track pickers, the map menu): a box under the header,
// inset 10 px on the 128 px row, full width and the whole content zone in
// portrait. MAP_DY moves the rows written for the landscape content start
// (y11) to the live one (y21 in portrait).
#define MAP_POP_X (oledPortrait() ? 0 : 10)
#define MAP_POP_W (SCREEN_WIDTH - 2 * MAP_POP_X)
#define MAP_POP_H (oledPortrait() ? oledContentBottom() + 1 - OLED_CONTENT_START_Y : 42)
#define MAP_DY (OLED_CONTENT_START_Y - 11)

// Double framebuffers in PSRAM (1-bit SSD1306 page layout, blitted per UI frame)
static uint8_t* sMapBufA = nullptr;   // Buffer A (PSRAM)
static uint8_t* sMapBufB = nullptr;   // Buffer B (PSRAM)
static uint8_t* sMapFrontBuf = nullptr;  // Points to completed render (A or B)
static uint8_t* sMapBackBuf  = nullptr;  // Points to in-progress render (A or B)
static volatile bool sFrontBufReady = false;  // True once first render completes

// Shade scratch (render-task-only): the renderer draws 0-15 shades here, then
// the task crushes shade>0 into the 1-bit back buffer before the swap. The
// OLED can't show shades — this keeps its output byte-identical while the
// same renderer feeds real shades to the G2 map page.
static uint8_t* sMapShadeBuf = nullptr;

// Render task synchronization
static SemaphoreHandle_t sRenderSemaphore = nullptr;  // Signals render task to start
static portMUX_TYPE sSwapSpinlock = portMUX_INITIALIZER_UNLOCKED;

// Snapshot of viewport state for the render task (written by display task, read by render task)
static struct {
  float centerLat, centerLon;
  float zoom, rotation;
  uint16_t visibleLayers;
  uint8_t subtypeVisibility[65];
  bool hasTrack;
  // Track scale params (precomputed by display task)
  int32_t trackScaleX, trackScaleY;
  // The logical canvas the raster is for (oledrotation: 128 x 64 or 64 x 128)
  // and its content rows.
  int16_t width, height, viewHeight, offsetY;
} sRenderSnapshot;

// The logical canvas the front raster was drawn for (under sSwapSpinlock).
static int16_t sFrontW = 0, sFrontH = 0;

// Last-rendered snapshot (for dirty detection)
static float sRenderedLat = 0, sRenderedLon = 0;
static float sRenderedZoom = 0, sRenderedRotation = 0;
static uint16_t sRenderedLayers = 0;
static int16_t sRenderedW = 0, sRenderedH = 0;  // the canvas the last request was for

// Force re-render flag (set when layers toggled, etc.)
static volatile bool sForceRender = false;

// Render task handle. Written under sSwapSpinlock: by ensureMapRenderTask()
// (display task) when it creates the task and by the task itself as it leaves.
static TaskHandle_t sRenderTaskHandle = nullptr;
// The task leaves on its own this long after the last render request once the
// OLED has left the map (a still frame on the map is no reason to go), giving
// its 8 KB internal stack and TCB back; the next map frame creates it again.
// A create the heap refuses (fragmented internal DRAM, e.g. Feather V2 with
// the glasses up) is retried every kMapRenderRetryMs and the frame says so.
static constexpr uint32_t kMapRenderIdleRetireMs = 10000;
static constexpr uint32_t kMapRenderRetryMs = 2000;
static bool sRenderTaskRefused = false;     // display task only
static uint32_t sRenderTaskRefusedMs = 0;   // display task only

// Forward declaration
static void mapRenderTask(void* param);

// Map menu state (accessed by footer system)
bool gMapMenuOpen = false;
static int sMapMenuSelection = 0;
static int sMapMenuScrollOffset = 0;

// Context bar scrolling state
static int sContextScrollOffset = 0;
static uint32_t sContextScrollLastUpdate = 0;
#define CONTEXT_SCROLL_SPEED_MS 100  // Update every 100ms for smooth scrolling

// Hierarchical menu system
static int sMapSubmenuLevel = 0;     // 0=main menu, 1=submenu
static int sMapSubmenuType = 0;      // Which submenu is open (index in main menu)

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
  MENU_CAT_LAYERS,
  MENU_CAT_CLOSE,
  MENU_CAT_COUNT
};

static const char* const kMapMainMenuItems[] = {
  "View >",
  "Maps >",
#if ENABLE_GPS_SENSOR
  "GPS >",
#endif
  "Waypoints >",
  "Tracks >",
  "Info >",
  "Layers >",
  "Close Menu"
};

#if ENABLE_GPS_SENSOR
static const int kMapMainMenuItemCount = 8;
#else
static const int kMapMainMenuItemCount = 7;
#endif

// Submenu items
static const char* const kViewSubmenu[] = {
  "Zoom In", "Zoom Out", "Reset Zoom",
  "Rotate Left", "Rotate Right", "Reset Rotation",
  "< Back"
};
static const int kViewSubmenuCount = 7;

static const char* const kMapsSubmenu[] = {
  "Select Map", "Next Map", "Previous Map", "Recenter",
  "< Back"
};
static const int kMapsSubmenuCount = 5;

#if ENABLE_GPS_SENSOR
static const char* const kGpsSubmenu[] = {
  "Center on GPS", "Toggle GPS",
  "< Back"
};
static const int kGpsSubmenuCount = 3;
#endif

static const char* const kWaypointsSubmenu[] = {
  "Mark Waypoint", "Goto Waypoint", "Clear Nav", "Delete Waypoint",
  "< Back"
};
static const int kWaypointsSubmenuCount = 5;

static const char* const kTracksSubmenu[] = {
  "Load Track", "Clear Track", "Track Status", "Delete Track",
  "Live Track", "Save Track", "< Back"
};
static const int kTracksSubmenuCount = 7;

static const char* const kInfoSubmenu[] = {
  "Map Info", "Features", "Search Names", "Transit Routes",
  "< Back"
};
static const int kInfoSubmenuCount = 5;

// Waypoint selection state for submenu
static int sWaypointSelectMode = 0;  // 0=none, 1=goto, 2=delete
static int sWaypointSelectIdx = 0;

// Track file selection state
static int sTrackSelectMode = 0;  // 0=none, 1=load, 2=delete
static int sTrackFileIdx = 0;
// 96: the card twin of a shaped session file ("/sd/logging_captures/sensors/
// <day>/<name>-<stamp>.csv") is longer than 64.
EXT_RAM_BSS_ATTR static char gTrackFiles[8][96];
static int sTrackFileCount = 0;

// Track status overlay
static bool sShowTrackStatus = false;

// Map info overlay state
static bool sShowMapInfo = false;

// Layer type info for the Layers submenu (used to build dynamic menu items)
struct MapLayerTypeInfo {
  const char* name;         // Short display name
  uint16_t    layerFlag;    // LAYER_* flag for type-level toggle
  uint16_t    hwmapMask;    // HWMAP_FTYPE_* bits to check in header.featureTypeMask
  uint8_t     featureType;  // MapFeatureType value
  int         subtypeCount; // 0 = no subtype submenu
  const char* subtypes[4];  // Subtype display names (nullptr-terminated)
};

static const MapLayerTypeInfo kMapLayerTypes[] = {
  { "Highways",  LAYER_HIGHWAYS,  HWMAP_FTYPE_HIGHWAY,                                      MAP_FEATURE_HIGHWAY,    2, {"Motorway", "Trunk",     nullptr,   nullptr    } },
  { "MajorRoads",LAYER_MAJOR,     HWMAP_FTYPE_MAJOR,                                        MAP_FEATURE_ROAD_MAJOR, 2, {"Primary",  "Secondary", nullptr,   nullptr    } },
  { "MinorRoads",LAYER_MINOR,     HWMAP_FTYPE_MINOR,                                        MAP_FEATURE_ROAD_MINOR, 3, {"Tertiary", "Resident",  "Service", nullptr    } },
  { "Paths",     LAYER_PATHS,     HWMAP_FTYPE_PATH,                                         MAP_FEATURE_PATH,       3, {"Footway",  "Cycleway",  "Track",   nullptr    } },
  { "Water",     LAYER_WATER,     HWMAP_FTYPE_WATER,                                        MAP_FEATURE_WATER,      3, {"Lake",     "River",     "Coast",   nullptr    } },
  { "Parks",     LAYER_PARKS,     HWMAP_FTYPE_PARK,                                         MAP_FEATURE_PARK,       3, {"Park",     "Forest",    "Grass",   nullptr    } },
  { "Railways",  LAYER_RAILWAYS,  HWMAP_FTYPE_RAILWAY,                                      MAP_FEATURE_RAILWAY,    2, {"Rail",     "Subway",    nullptr,   nullptr    } },
  { "Transit",   LAYER_TRANSIT,   HWMAP_FTYPE_BUS | HWMAP_FTYPE_FERRY | HWMAP_FTYPE_STATION, MAP_FEATURE_BUS,        0, {nullptr,    nullptr,     nullptr,   nullptr    } },
  { "Buildings", LAYER_BUILDINGS,  HWMAP_FTYPE_BUILDING,                                     MAP_FEATURE_BUILDING,   4, {"Building", "Industrial","Commerc", "Resident" } },
  { "Land Mask", LAYER_LAND_MASK, HWMAP_FTYPE_LAND,                                         MAP_FEATURE_LAND_MASK,  0, {nullptr,    nullptr,     nullptr,   nullptr    } },
};
static const int kMapLayerTypeCount = 10;

// State for the layer subtype sub-submenu (level 2)
static int sLayerTypeSelectIdx = 0;

// Features viewer state
static bool sShowFeatures = false;
static int sFeaturesCategory = 0;  // 0-5 for different categories
static int sFeaturesScrollOffset = 0;

// Routes viewer state
static bool sShowRoutes = false;
static int sRoutesScrollOffset = 0;
static int sRoutesSelectedIdx = 0;

// Search mode state
static bool sMapSearchMode = false;
static char sSearchResult[64] = "";

// Mark Waypoint name entry: set while the shared keyboard collects the name.
// The keyboard owns input meanwhile; gpsMapInputHandler consumes its result.
static bool sMapWaypointNameMode = false;

// Search results navigation (for multiple matches like "Target")
// Store coordinates directly since tiled format doesn't have flat feature indices
struct SearchResultCoord {
  float lat;
  float lon;
};
EXT_RAM_BSS_ATTR static SearchResultCoord gSearchResultCoords[32];  // Coordinates of matches
static int sSearchResultCount = 0;
static int sSearchResultCurrent = 0;
static bool sSearchResultsActive = false;  // True when viewing search results

// Autocomplete provider for map name search
static int mapNameAutocomplete(const char* input, const char** results, int maxResults, void* userData) {
  (void)userData;  // Unused
  return MapCore::searchNamesByPrefix(input, results, maxResults);
}

// Zoom constants
static const float kMapZoomMin = 0.20f;
static const float kMapZoomMax = 30.0f;
static const float kMapZoomStep = 1.5f;

// Draw map info overlay
static void drawMapInfo() {
  if (!oledDisplay) return;
  
  const LoadedMap& currentMap = MapCore::getCurrentMap();
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  int y = OLED_CONTENT_START_Y;
  oledDisplay->setCursor(0, y);
  if (currentMap.valid) {
    oledDisplay->print(currentMap.filename);
  } else {
    oledDisplay->print("No map loaded");
  }
  
  if (currentMap.valid) {
    char buf[32];
    oledDisplay->setCursor(0, y + 10);
    snprintf(buf, sizeof(buf), "Features: %lu", currentMap.header.featureCount);
    oledDisplay->print(buf);
    
    oledDisplay->setCursor(0, y + 20);
    snprintf(buf, sizeof(buf), "Zoom: %.1fx", gMapZoom);
    oledDisplay->print(buf);
    
    oledDisplay->setCursor(0, y + 30);
    snprintf(buf, sizeof(buf), "Rot: %.0f", gMapRotation);
    oledDisplay->print(buf);
  }
  // B:OK is the footer's hint (oledMapFooterHint).
}

// Draw track status overlay. Its title ("LIVE TRACKING" / "Track Status") is
// in the header bar (mapHeaderTitle); the rows run on the 8 px text pitch from
// the top of the content area, five of them above the 64 px OLED's footer,
// and B:OK is the footer's hint.
static void drawTrackStatus() {
  if (!oledDisplay) return;
  const GpsTrackStatus status = GpsTrackManager::readStatus();
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  const int y = OLED_CONTENT_START_Y;
  const int step = 8;
  if (status.pointCount == 0 && !status.live) {
    oledDisplay->setCursor(0, y);
    oledDisplay->print("No track loaded");
    return;
  }
  
  char buf[32];
  const char* filename = strrchr(status.filename, '/');
  if (filename) filename++; else filename = status.filename;
  
  oledDisplay->setCursor(0, y);
  oledDisplay->print(filename);
  
  oledDisplay->setCursor(0, y + step);
  snprintf(buf, sizeof(buf), "Points: %d", status.pointCount);
  oledDisplay->print(buf);
  
  const GpsTrackStats& stats = status.stats;
  if (stats.valid) {
    oledDisplay->setCursor(0, y + 2 * step);
    if (stats.totalDistanceM >= 1000.0f) {
      snprintf(buf, sizeof(buf), "Dist: %.2fkm", stats.totalDistanceM / 1000.0f);
    } else {
      snprintf(buf, sizeof(buf), "Dist: %.0fm", stats.totalDistanceM);
    }
    oledDisplay->print(buf);
    
    oledDisplay->setCursor(0, y + 3 * step);
    int mins = (int)(stats.durationSec / 60);
    int secs = (int)stats.durationSec % 60;
    snprintf(buf, sizeof(buf), "Time: %d:%02d", mins, secs);
    oledDisplay->print(buf);
    
    // Beside the time on the 128 px row; under it in portrait.
    if (oledPortrait()) oledDisplay->setCursor(0, y + 5 * step);
    else oledDisplay->setCursor(72, y + 3 * step);
    snprintf(buf, sizeof(buf), "%.1fm/s", stats.avgSpeedMps);
    oledDisplay->print(buf);
  }
  
  // Validation status
  oledDisplay->setCursor(0, y + 4 * step);
  snprintf(buf, sizeof(buf), "Coverage: %.0f%%", status.coveragePercent);
  oledDisplay->print(buf);
}

// Draw features list overlay
static void drawFeatures() {
  if (!oledDisplay) return;

  // Reads map.names[] throughout, which unloadMap() frees; hold the map lock so
  // a concurrent unload/load can't pull the name table out mid-draw.
  MapCacheGuard mapGuard("OLED.drawFeatures");
  const LoadedMap& map = MapCore::getCurrentMap();
  
  oledDisplay->fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, DISPLAY_COLOR_BLACK);  // the logical canvas
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  const char* catNames[] = {"Highways", "Roads", "Water", "Parks", "Railways", "Subways"};
  
  // Under the global header (drawn over y 0-9 after this screen); the
  // controls are in the footer (oledMapFooterHint).
  const int titleY = OLED_CONTENT_START_Y;
  oledDisplay->setCursor(0, titleY);
  if (!map.valid || map.nameCount == 0) {
    oledDisplay->print("No names");
    oledDisplay->setCursor(0, titleY + 10);
    oledDisplay->print("(map has no names)");
    return;
  }
  
  int nameCount = map.nameCount;
  
  char header[32];
  snprintf(header, sizeof(header), "Names (%d)", nameCount);
  oledDisplay->print(header);
  
  // 10 px rows under the title line, as many as end above the footer (3 on
  // the 64 px OLED, 9 on the P4X-EYE canvas). The title used to sit at y=0
  // under the header and a fixed 5 rows ran into the hint line and footer.
  const int itemsPerPage = oledVisibleRows(10, 10);
  int displayIdx = 0;
  
  for (int i = sFeaturesScrollOffset; i < map.nameCount && displayIdx < itemsPerPage; i++) {
    oledDisplay->setCursor(0, titleY + 10 + displayIdx * 10);
    char nameBuf[22];
    strncpy(nameBuf, map.names[i].name, 21);
    nameBuf[21] = '\0';
    oledDisplay->print(nameBuf);
    displayIdx++;
  }
}

// Route info structure for transit viewer
struct RouteInfo {
  const char* name;
  uint8_t type;  // 0x20=rail, 0x21=bus, 0x22=ferry
};

// Draw transit routes viewer
static void drawRoutes() {
  if (!oledDisplay) return;

  // Parses tile payloads returned by loadTileData below; hold the map lock
  // across the whole scan so the async mapRenderTask can't evict a slot
  // mid-parse. Reentrant guard → the inner loadTileData calls no-op.
  MapCacheGuard mapGuard("OLED.drawRoutes");
  const LoadedMap& map = MapCore::getCurrentMap();
  
  oledDisplay->fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, DISPLAY_COLOR_BLACK);  // the logical canvas
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Under the global header; the controls are in the footer (oledMapFooterHint).
  const int titleY = OLED_CONTENT_START_Y;
  oledDisplay->setCursor(0, titleY);
  oledDisplay->print("Transit Routes");
  
  if (!map.valid) {
    oledDisplay->setCursor(0, titleY + 10);
    oledDisplay->print("No map loaded");
    return;
  }
  
  // Count transit features with names (rail, bus, ferry)
  int routeCount = 0;
  EXT_RAM_BSS_ATTR static RouteInfo routes[32];  // PSRAM: OLED task only, under MapCacheGuard
  
  // Iterate through all tiles to find transit features
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
    
    for (uint16_t f = 0; f < featureCount && routeCount < 32; f++) {
      if (ptr + HWMAP_FEATURE_HEADER_SIZE > end) break;
      
      uint8_t type = ptr[0];
      // type(1) + subtype(1) + nameIndex(2) + pointCount(2)
      uint16_t nameIdx = ptr[2] | (ptr[3] << 8);
      uint16_t pointCount = ptr[4] | (ptr[5] << 8);
      ptr += HWMAP_FEATURE_HEADER_SIZE + pointCount * 4;  // Skip header + quantized points
      
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
    oledDisplay->setCursor(0, titleY + 10);
    oledDisplay->print("No routes found");
    return;
  }
  
  // Display routes with type indicator
  // 10 px rows under the title line, as many as end above the footer (3 on
  // the 64 px OLED, 9 on the P4X-EYE canvas); the input handler scrolls by
  // the same count.
  const int itemsPerPage = oledVisibleRows(10, 10);
  for (int i = 0; i < itemsPerPage && (i + sRoutesScrollOffset) < routeCount; i++) {
    int idx = i + sRoutesScrollOffset;
    oledDisplay->setCursor(0, titleY + 10 + i * 10);
    
    if (idx == sRoutesSelectedIdx) {
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
}

// Draw waypoint selection submenu
static void drawWaypointSelect() {
  if (!oledDisplay) return;
  
  // Under the header bar, which shows the popup's title (mapHeaderTitle).
  oledDisplay->fillRect(MAP_POP_X, OLED_CONTENT_START_Y, MAP_POP_W, MAP_POP_H, DISPLAY_COLOR_BLACK);
  oledDisplay->drawRect(MAP_POP_X, OLED_CONTENT_START_Y, MAP_POP_W, MAP_POP_H, DISPLAY_COLOR_WHITE);
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  int activeIdx = 0;
  for (int i = 0; i < MAX_WAYPOINTS; i++) {
    const Waypoint* wp = WaypointManager::getWaypoint(i);
    if (wp) {
      if (activeIdx == sWaypointSelectIdx) {
        oledDisplay->fillRect(MAP_POP_X + 2, 20 + MAP_DY, MAP_POP_W - 4, 12, DISPLAY_COLOR_WHITE);
        oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
        oledDisplay->setCursor(MAP_POP_X + 4, 22 + MAP_DY);
        char buf[24];
        snprintf(buf, sizeof(buf), "%d: %s", i, wp->name);
        oledDisplay->print(buf);
        break;
      }
      activeIdx++;
    }
  }
  
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(MAP_POP_X + 4, 40 + MAP_DY);
  oledDisplay->print("A:OK  B:Cancel");
}

// Check if a line looks like GPS track data
static bool isGpsDataLine(const String& line) {
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

// Probe one candidate file's head for GPS data; record it in gTrackFiles if
// it qualifies. Shared by the top-level scan and the subfolder descent.
// `path` is the file's VFS path, built by the scan from the folder it listed:
// a card entry's own path() has no "/sd" in front and would name the flash
// file of the same name.
static void probeTrackFile(const String& path, const AuthContext& auth) {
  if (sTrackFileCount >= 8) return;
  // Staging files of an interrupted merge/export are never loadable tracks.
  if (VFS::isStagingName(path)) return;
  FsLockGuard fsGuard("oled.map.probe_track");
  File check = VFS::openGuarded(path, "r", auth);
  if (!check) return;
  bool hasGps = false;
  // Rows open with the class of the path they are read at; a plain line in a
  // sealed file is not a row and never counts as GPS data.
  String classPath;
  if (!normalizeFsPath(path, classPath)) classPath = "";
  sealedlog::LineState lineKinds = sealedlog::stateFor(classPath.c_str());
  for (int i = 0; i < 15 && check.available(); i++) {
    String line = check.readStringUntil('\n');
    // Sealed at rest (every track on the card, B11): the rows are revealed
    // for this user, who passed the guarded open above.
    if (captureCryptoRevealFileLine(classPath.c_str(), lineKinds, line) == sealedlog::Line::Unsealed) continue;
    if (isGpsDataLine(line)) {
      hasGps = true;
      break;
    }
  }
  check.close();
  // A path that does not fit would name another file: such a track is left out.
  if (hasGps && path.length() < sizeof(gTrackFiles[0])) {
    strlcpy(gTrackFiles[sTrackFileCount], path.c_str(), sizeof(gTrackFiles[0]));
    sTrackFileCount++;
  }
}

// "<folder>/<name>" for a listed entry, whatever form its name() has.
static String trackEntryPath(const String& folder, File& entry) {
  const char* name = entry.name();
  const char* slash = name ? strrchr(name, '/') : nullptr;
  return folder + "/" + (slash ? slash + 1 : (name ? name : ""));
}

// True when `path` is one of the folders the track scan visits by name.
static bool trackFolderScannedOnItsOwn(const String& path, const char* const* dirs, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (path == dirs[i]) return true;
  }
  return false;
}

// Scan for GPS track files in the capture dirs. Session shaping places logs
// one level down in YYYY-MM-DD/ and boot-N/ subfolders of /sensors (see
// shapeSessionPath in System_SensorLogging.cpp), so the scan descends exactly
// one directory level — a flat-only scan misses every shaped track.
static void scanTrackFiles() {
  FsLockGuard fsGuard("oled.map.scan_tracks");
  sTrackFileCount = 0;
  // List under the OLED identity, the one Load/Delete Track open the file
  // with, so the picker offers exactly the tracks this user may use.
  const AuthContext auth = oledMapAuthContext();

  // Scan directories (sensors included: autostart-resumed TRACK sessions
  // land there, shaped into session subfolders).
  // Both tiers: a capture goes to the card while one takes log lines (B11),
  // so a track can be on flash, on the card, or split across the two.
  const char* dirs[] = {"/logging_captures", "/logging_captures/tracks",
                        "/logging_captures/sensors",
                        "/sd/logging_captures", "/sd/logging_captures/tracks",
                        "/sd/logging_captures/sensors"};

  for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]) && sTrackFileCount < 8; d++) {
    File root = VFS::openGuarded(dirs[d], "r", auth);
    if (!root || !root.isDirectory()) continue;

    const String folder(dirs[d]);
    File file = root.openNextFile();
    while (file && sTrackFileCount < 8) {
      const String path = trackEntryPath(folder, file);
      if (!file.isDirectory()) {
        probeTrackFile(path, auth);
      } else if (!trackFolderScannedOnItsOwn(path, dirs, sizeof(dirs) / sizeof(dirs[0]))) {
        // One level down: session subfolders (dated / boot-N). A folder this
        // scan visits on its own (tracks, sensors) is not walked from its
        // parent as well: its tracks were listed twice, and the list holds 8.
        File sub = VFS::openGuarded(path, "r", auth);
        if (sub && sub.isDirectory()) {
          File child = sub.openNextFile();
          while (child && sTrackFileCount < 8) {
            if (!child.isDirectory()) probeTrackFile(trackEntryPath(path, child), auth);
            child = sub.openNextFile();
          }
        }
        if (sub) sub.close();
      }
      file = root.openNextFile();
    }
  }
}

// Draw track file selection submenu
static void drawTrackSelect() {
  if (!oledDisplay) return;
  
  // Under the header bar, which shows the popup's title (mapHeaderTitle).
  oledDisplay->fillRect(MAP_POP_X, OLED_CONTENT_START_Y, MAP_POP_W, MAP_POP_H, DISPLAY_COLOR_BLACK);
  oledDisplay->drawRect(MAP_POP_X, OLED_CONTENT_START_Y, MAP_POP_W, MAP_POP_H, DISPLAY_COLOR_WHITE);
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  if (sTrackFileCount == 0) {
    oledDisplay->setCursor(MAP_POP_X + 4, 22 + MAP_DY);
    oledDisplay->print("No GPS logs found");
    oledDisplay->setCursor(MAP_POP_X + 4, 40 + MAP_DY);
    oledDisplay->print("B:Cancel");
    return;
  }
  
  if (sTrackFileIdx < sTrackFileCount) {
    oledDisplay->fillRect(MAP_POP_X + 2, 20 + MAP_DY, MAP_POP_W - 4, 12, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    oledDisplay->setCursor(MAP_POP_X + 4, 22 + MAP_DY);
    // Show just filename, not full path
    const char* filename = strrchr(gTrackFiles[sTrackFileIdx], '/');
    if (filename) filename++; else filename = gTrackFiles[sTrackFileIdx];
    char buf[32];
    snprintf(buf, sizeof(buf), "%d/%d: %.10s", sTrackFileIdx + 1, sTrackFileCount, filename);
    oledDisplay->print(buf);
  }
  
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(MAP_POP_X + 4, 40 + MAP_DY);
  oledDisplay->print("A:OK  B:Cancel");
}

// Dynamic buffers for the Layers submenu (rebuilt each draw to show current on/off state)
EXT_RAM_BSS_ATTR static char       gLayersMenuBuf[11][20];
static const char* sLayersMenuPtrs[11];
static int        sLayersMenuCount = 0;

static void buildLayersMenuItems() {
  sLayersMenuCount = 0;
  uint16_t vis = mapLayersGetVisible();
  const LoadedMap& map = MapCore::getCurrentMap();
  uint16_t ftMask = map.valid ? map.header.featureTypeMask : 0xFFFF;
  for (int i = 0; i < kMapLayerTypeCount; i++) {
    const MapLayerTypeInfo& t = kMapLayerTypes[i];
    bool present = (ftMask & t.hwmapMask) != 0;
    bool on = (vis & t.layerFlag) != 0;
    if (!present) {
      snprintf(gLayersMenuBuf[i], 20, "~%s", t.name);
    } else if (t.subtypeCount > 0) {
      snprintf(gLayersMenuBuf[i], 20, "%s %s>", t.name, on ? "[+]" : "[ ]");
    } else {
      snprintf(gLayersMenuBuf[i], 20, "%s %s", t.name, on ? "[+]" : "[ ]");
    }
    sLayersMenuPtrs[sLayersMenuCount++] = gLayersMenuBuf[i];
  }
  strlcpy(gLayersMenuBuf[sLayersMenuCount], "< Back", 20);
  sLayersMenuPtrs[sLayersMenuCount++] = gLayersMenuBuf[kMapLayerTypeCount];
}

// Dynamic buffers for the subtype sub-submenu
EXT_RAM_BSS_ATTR static char       gSubtypeMenuBuf[7][20];
static const char* sSubtypeMenuPtrs[7];
static int        sSubtypeMenuCount = 0;

static void buildSubtypeMenuItems(int typeIdx) {
  sSubtypeMenuCount = 0;
  if (typeIdx < 0 || typeIdx >= kMapLayerTypeCount) return;
  const MapLayerTypeInfo& t = kMapLayerTypes[typeIdx];

  bool typeOn = (mapLayersGetVisible() & t.layerFlag) != 0;
  snprintf(gSubtypeMenuBuf[0], 20, "All %s", typeOn ? "[+]" : "[ ]");
  sSubtypeMenuPtrs[sSubtypeMenuCount++] = gSubtypeMenuBuf[0];

  uint8_t mask = mapSubtypeGetMask(t.featureType);
  for (int s = 0; s < t.subtypeCount; s++) {
    bool on = typeOn && ((mask >> s) & 1);
    snprintf(gSubtypeMenuBuf[1 + s], 20, "%s %s", t.subtypes[s], on ? "[+]" : "[ ]");
    sSubtypeMenuPtrs[sSubtypeMenuCount++] = gSubtypeMenuBuf[1 + s];
  }
  strlcpy(gSubtypeMenuBuf[sSubtypeMenuCount], "< Back", 20);
  sSubtypeMenuPtrs[sSubtypeMenuCount++] = gSubtypeMenuBuf[1 + t.subtypeCount];
}

// Execute a subtype sub-submenu action (level 2)
static void executeSubtypeMenuAction(int typeIdx, int action) {
  if (typeIdx < 0 || typeIdx >= kMapLayerTypeCount) {
    sMapSubmenuLevel = 1;
    sMapMenuSelection = 0;
    sMapMenuScrollOffset = 0;
    return;
  }
  const MapLayerTypeInfo& t = kMapLayerTypes[typeIdx];
  const int backItem = 1 + t.subtypeCount;  // "All" + subtypes
  if (action == 0) {
    mapLayerToggle(t.layerFlag);
    sForceRender = true;
  } else if (action > 0 && action < backItem) {
    mapSubtypeToggle(t.featureType, (uint8_t)(action - 1));
    sForceRender = true;
  } else {
    // Back → return to layers submenu, restore selection
    sMapSubmenuLevel = 1;
    sMapMenuSelection = sLayerTypeSelectIdx;
    sMapMenuScrollOffset = max(0, sLayerTypeSelectIdx - oledVisibleRows(9, 18 + MAP_DY - OLED_CONTENT_START_Y, 4) + 1);
  }
}

// The header bar's title while a map popup or the track status is open (the
// popups start under the bar); nullptr = the mode name ("Map"). The names,
// routes and info overlays keep their own first line.
static const char* mapHeaderTitle() {
  if (!gMapMenuOpen || sShowMapInfo || sShowFeatures || sShowRoutes) return nullptr;
  if (sShowTrackStatus) return GpsTrackManager::isLiveTracking() ? "LIVE TRACKING" : "Track Status";
  if (sWaypointSelectMode != 0) return sWaypointSelectMode == 1 ? "Goto WP" : "Del WP";
  if (sTrackSelectMode != 0) return sTrackSelectMode == 1 ? "Load Track" : "Delete Track";
  if (sMapSubmenuLevel == 0) return "Map Menu";
  if (sMapSubmenuLevel == 2) {
    return (sLayerTypeSelectIdx >= 0 && sLayerTypeSelectIdx < kMapLayerTypeCount)
               ? kMapLayerTypes[sLayerTypeSelectIdx].name : "Types";
  }
  switch (sMapSubmenuType) {
    case MENU_CAT_VIEW: return "View";
    case MENU_CAT_MAPS: return "Maps";
#if ENABLE_GPS_SENSOR
    case MENU_CAT_GPS: return "GPS";
#endif
    case MENU_CAT_WAYPOINTS: return "Waypoints";
    case MENU_CAT_TRACKS: return "Tracks";
    case MENU_CAT_INFO: return "Info";
    case MENU_CAT_LAYERS: return "Layers";
    default: return "Map Menu";
  }
}

static void gpsMapHeader(OledHeaderSpec& spec) {
  if (oledKeyboardIsActive()) return;
  const char* title = mapHeaderTitle();
  if (title) spec.text = title;
}

// Draw the map menu overlay
static void drawMapMenu() {
  if (!oledDisplay) return;
  
  if (sShowMapInfo) {
    drawMapInfo();
    return;
  }
  
  if (sShowTrackStatus) {
    drawTrackStatus();
    return;
  }
  
  if (sShowFeatures) {
    drawFeatures();
    return;
  }
  
  if (sShowRoutes) {
    drawRoutes();
    return;
  }
  
  if (sWaypointSelectMode != 0) {
    drawWaypointSelect();
    return;
  }
  
  if (sTrackSelectMode != 0) {
    drawTrackSelect();
    return;
  }
  
  // Under the header bar, which shows the popup's title (mapHeaderTitle).
  oledDisplay->fillRect(MAP_POP_X, OLED_CONTENT_START_Y, MAP_POP_W, MAP_POP_H, DISPLAY_COLOR_BLACK);
  oledDisplay->drawRect(MAP_POP_X, OLED_CONTENT_START_Y, MAP_POP_W, MAP_POP_H, DISPLAY_COLOR_WHITE);
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Current menu items by submenu level (the title is in the header bar).
  const char* const* menuItems;
  int menuItemCount;
  
  if (sMapSubmenuLevel == 0) {
    menuItems = kMapMainMenuItems;
    menuItemCount = kMapMainMenuItemCount;
  } else if (sMapSubmenuLevel == 2) {
    buildSubtypeMenuItems(sLayerTypeSelectIdx);
    menuItems = sSubtypeMenuPtrs;
    menuItemCount = sSubtypeMenuCount;
  } else {
    switch (sMapSubmenuType) {
      case MENU_CAT_VIEW:
        menuItems = kViewSubmenu;
        menuItemCount = kViewSubmenuCount;
        break;
      case MENU_CAT_MAPS:
        menuItems = kMapsSubmenu;
        menuItemCount = kMapsSubmenuCount;
        break;
#if ENABLE_GPS_SENSOR
      case MENU_CAT_GPS:
        menuItems = kGpsSubmenu;
        menuItemCount = kGpsSubmenuCount;
        break;
#endif
      case MENU_CAT_WAYPOINTS:
        menuItems = kWaypointsSubmenu;
        menuItemCount = kWaypointsSubmenuCount;
        break;
      case MENU_CAT_TRACKS:
        menuItems = kTracksSubmenu;
        menuItemCount = kTracksSubmenuCount;
        break;
      case MENU_CAT_INFO:
        menuItems = kInfoSubmenu;
        menuItemCount = kInfoSubmenuCount;
        break;
      case MENU_CAT_LAYERS:
        buildLayersMenuItems();
        menuItems = sLayersMenuPtrs;
        menuItemCount = sLayersMenuCount;
        break;
      default:
        menuItems = kMapMainMenuItems;
        menuItemCount = kMapMainMenuItemCount;
        break;
    }
  }
  
  const int maxVisible = oledVisibleRows(9, 18 + MAP_DY - OLED_CONTENT_START_Y, 4);
  const int itemHeight = 9;
  const int startY = 18 + MAP_DY;
  
  for (int i = 0; i < maxVisible && (i + sMapMenuScrollOffset) < menuItemCount; i++) {
    int itemIdx = i + sMapMenuScrollOffset;
    int y = startY + (i * itemHeight);
    
    if (itemIdx == sMapMenuSelection) {
      oledDisplay->fillRect(MAP_POP_X + 2, y - 1, MAP_POP_W - 4, itemHeight, DISPLAY_COLOR_WHITE);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }
    
    oledDisplay->setCursor(MAP_POP_X + 4, y);
    oledDisplay->printf("%.*s", max(1, (MAP_POP_W - 14) / 6), menuItems[itemIdx]);
  }
  
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  if (sMapMenuScrollOffset > 0) {
    oledDisplay->setCursor(MAP_POP_X + MAP_POP_W - 6, startY);
    oledDisplay->print("^");
  }
  if (sMapMenuScrollOffset + maxVisible < menuItemCount) {
    // Beside the last visible row (y=44 on the 64 px OLED), not at a fixed y.
    oledDisplay->setCursor(MAP_POP_X + MAP_POP_W - 6, startY + (maxVisible - 1) * itemHeight - 1);
    oledDisplay->print("v");
  }
}

// =============================================================================
// Async Map Render Task
// =============================================================================
// Architecture:
// - displayGpsMap() (UI path) writes sRenderSnapshot + xSemaphoreGive(sRenderSemaphore) when dirty.
// - mapRenderTask (TASK_PRIORITY_LOW, core 0) blocks on the binary semaphore, renders MapCore::renderMap
//   into sMapBackBuf, swaps sMapFrontBuf/sMapBackBuf, sets sFrontBufReady.
// - Each UI frame memcpy's sMapFrontBuf to the SSD1306 buffer, then draws overlays (context bar, zoom).
// Binary semaphore: if render is slower than the UI rate, extra Gives coalesce — the task still runs
// renderMap() to completion each wake, but sRenderSnapshot may advance (latest wins). Slower frames
// mean the user briefly sees an older raster until the next swap — acceptable vs. skipping tiles inside
// renderMap (checkerboard). MapCore may force full tile coverage when stride would look wrong, with
// LRU thrashing; renderMap also taskYIELDs under cache pressure so this path stays cooperative.

static void initAsyncMapRenderer() {
  if (sMapBufA) return;  // Already initialized
  
  // Allocate double buffers + shade scratch in PSRAM
  sMapBufA = (uint8_t*)ps_alloc(kSSD1306BufSize, AllocPref::RequirePsram, "oled.map.bufA");
  sMapBufB = (uint8_t*)ps_alloc(kSSD1306BufSize, AllocPref::RequirePsram, "oled.map.bufB");
  sMapShadeBuf = (uint8_t*)ps_alloc(kMapShadeBufSize, AllocPref::RequirePsram,
                                    "oled.map.shade");
  if (!sMapBufA || !sMapBufB || !sMapShadeBuf) {
    DEBUG_MAPSF("[MAP_ASYNC] PSRAM alloc failed for double buffers!");
    return;
  }
  memset(sMapBufA, 0, kSSD1306BufSize);
  memset(sMapBufB, 0, kSSD1306BufSize);
  sMapFrontBuf = sMapBufA;
  sMapBackBuf  = sMapBufB;
  sFrontBufReady = false;
  
  // The render semaphore outlives every render task: a give to a task that
  // is leaving waits for its successor.
  sRenderSemaphore = xSemaphoreCreateBinary();
  
  DEBUG_MAPSF("[MAP_ASYNC] Async renderer initialized (double buffers)");
}

// The render task, created on the first map frame that needs it and again
// after it left (mapRenderTask's idle exit). APP_CORE: the offscreen raster is
// pure compute; keep it off the Wi-Fi/BLE core. False while the heap refuses
// the task (retried every kMapRenderRetryMs): the frame keeps its dirty state
// and shows a notice instead of a raster that will not update.
static bool ensureMapRenderTask() {
  if (!sRenderSemaphore) return false;  // initAsyncMapRenderer's PSRAM buffers failed
  taskENTER_CRITICAL(&sSwapSpinlock);
  const bool present = sRenderTaskHandle != nullptr;
  taskEXIT_CRITICAL(&sSwapSpinlock);
  if (present) return true;
  const uint32_t now = millis();
  if (sRenderTaskRefused && now - sRenderTaskRefusedMs < kMapRenderRetryMs) return false;
  TaskHandle_t created = nullptr;
  taskStackRecord("map_render", MAP_RENDER_STACK_WORDS);
  if (xTaskCreatePinnedToCore(mapRenderTask, "map_render", MAP_RENDER_STACK_WORDS, nullptr,
                              TASK_PRIORITY_LOW, &created, APP_CORE) != pdPASS ||
      !created) {
    if (!sRenderTaskRefused) {
      DEBUG_MAPSF("[MAP_ASYNC] render task refused (internal heap); retrying every %lu ms",
                  (unsigned long)kMapRenderRetryMs);
    }
    sRenderTaskRefused = true;
    sRenderTaskRefusedMs = now;
    return false;
  }
  taskENTER_CRITICAL(&sSwapSpinlock);
  sRenderTaskHandle = created;
  taskEXIT_CRITICAL(&sSwapSpinlock);
  sRenderTaskRefused = false;
  DEBUG_MAPSF("[MAP_ASYNC] render task started");
  return true;
}

// Crush the shade scratch (one byte per logical pixel, values 0-15) into a
// 1-bit SSD1306 page buffer (byte = 8 vertical pixels, LSB = top). shade>0 => pixel on, so
// OLED output is byte-identical to the old direct 1-bit rendering no matter
// how the per-class shade values are tuned.
// width x height is the logical canvas (64 x 128 in portrait): the pages are
// that canvas's, not the panel's, and the blit turns them (blitMapFrame).
static void packShadeBufTo1Bit(const uint8_t* shades, uint8_t* pageBuf, int width, int height) {
  for (int page = 0; page < (height + 7) / 8; page++) {
    const uint8_t* rows = shades + (size_t)page * 8 * width;
    uint8_t* out = pageBuf + (size_t)page * width;
    for (int x = 0; x < width; x++) {
      uint8_t b = 0;
      for (int bit = 0; bit < 8; bit++) {
        if (page * 8 + bit < height && rows[(size_t)bit * width + x]) {
          b |= (uint8_t)(1 << bit);
        }
      }
      out[x] = b;
    }
  }
}

// The front raster onto the display. The raster is in logical pages of the
// canvas it was drawn for; the panel's buffer is in physical pages. Unturned
// (rotation 0) they are the same layout and one copy does it (~1 us). Any
// other rotation (180 flipped, 90/270 portrait) goes through the GFX pixel
// path, which applies the rotation, so the map reads upright as every other
// screen does. A raster drawn for the other orientation is not shown (the
// next render, already requested, replaces it).
static void blitMapFrame() {
  const int w = SCREEN_WIDTH, h = SCREEN_HEIGHT;
  if (oledDisplay->getRotation() == 0 && w == DISPLAY_WIDTH && h == DISPLAY_HEIGHT) {
    taskENTER_CRITICAL(&sSwapSpinlock);
    const bool same = sFrontW == w && sFrontH == h;
    if (same) memcpy(oledDisplay->getBuffer(), sMapFrontBuf, kSSD1306BufSize);
    taskEXIT_CRITICAL(&sSwapSpinlock);
    return;
  }
  // Copy out under the lock, draw outside it (drawPixel is not for a
  // critical section).
  static uint8_t* sBlitCopy = nullptr;
  if (!sBlitCopy) sBlitCopy = (uint8_t*)ps_alloc(kSSD1306BufSize, AllocPref::RequirePsram, "oled.map.blit");
  if (!sBlitCopy) return;
  taskENTER_CRITICAL(&sSwapSpinlock);
  const bool same = sFrontW == w && sFrontH == h;
  if (same) memcpy(sBlitCopy, sMapFrontBuf, kSSD1306BufSize);
  taskEXIT_CRITICAL(&sSwapSpinlock);
  if (!same) return;
  for (int page = 0; page < (h + 7) / 8; ++page) {
    const uint8_t* row = sBlitCopy + (size_t)page * w;
    for (int x = 0; x < w; ++x) {
      const uint8_t bits = row[x];
      if (!bits) continue;
      for (int bit = 0; bit < 8; ++bit) {
        if (bits & (1u << bit)) oledDisplay->drawPixel(x, page * 8 + bit, DISPLAY_COLOR_WHITE);
      }
    }
  }
}

static void mapRenderTask(void* param) {
  const TaskHandle_t self = xTaskGetCurrentTaskHandle();
  for (;;) {
    // Block until the display task signals us. Nothing in
    // kMapRenderIdleRetireMs and the OLED off the map: leave, the handle
    // nulled under the swap lock in the step that decides (the lock
    // ensureMapRenderTask checks), then the task's own delete. The stack and
    // TCB go back to internal heap; a give meanwhile waits in the semaphore.
    if (xSemaphoreTake(sRenderSemaphore, pdMS_TO_TICKS(kMapRenderIdleRetireMs)) != pdTRUE) {
      taskENTER_CRITICAL(&sSwapSpinlock);
      const bool leave = currentOledMode != OLED_GPS_MAP && sRenderTaskHandle == self;
      if (leave) sRenderTaskHandle = nullptr;
      taskEXIT_CRITICAL(&sSwapSpinlock);
      if (leave) vTaskDelete(nullptr);
      continue;
    }
    
    // Copy snapshot to local (tiny critical section)
    float lat, lon, zoom, rotation;
    bool hasTrack;
    int32_t trackScaleX, trackScaleY;
    int16_t width, height, viewHeight, offsetY;
    MapRenderParams renderParams;
    
    taskENTER_CRITICAL(&sSwapSpinlock);
    lat = sRenderSnapshot.centerLat;
    lon = sRenderSnapshot.centerLon;
    zoom = sRenderSnapshot.zoom;
    rotation = sRenderSnapshot.rotation;
    renderParams.zoom = zoom;
    renderParams.rotation = rotation;
    renderParams.visibleLayers = sRenderSnapshot.visibleLayers;
    memcpy(renderParams.subtypeVisibility, sRenderSnapshot.subtypeVisibility, sizeof(renderParams.subtypeVisibility));
    hasTrack = sRenderSnapshot.hasTrack;
    trackScaleX = sRenderSnapshot.trackScaleX;
    trackScaleY = sRenderSnapshot.trackScaleY;
    width = sRenderSnapshot.width;
    height = sRenderSnapshot.height;
    viewHeight = sRenderSnapshot.viewHeight;
    offsetY = sRenderSnapshot.offsetY;
    taskEXIT_CRITICAL(&sSwapSpinlock);
    if ((size_t)width * (size_t)height > kMapShadeBufSize || width <= 0 || height <= 0) continue;
    
    // Get current back buffer pointer (read under spinlock)
    taskENTER_CRITICAL(&sSwapSpinlock);
    uint8_t* backBuf = sMapBackBuf;
    taskEXIT_CRITICAL(&sSwapSpinlock);
    
    // Create a local offscreen renderer targeting the shade scratch buffer
    OffscreenMapRenderer localRenderer(sMapShadeBuf, width, height, width, viewHeight, offsetY);
    localRenderer.clear();

    MapCore::renderMap(&localRenderer, lat, lon, renderParams);

    // Render GPS track if loaded
    if (hasTrack && GpsTrackManager::hasTrack()) {
      GpsTrackManager::renderTrack(&localRenderer, lat, lon, trackScaleX, trackScaleY);
    }

    // Crush shades to 1-bit into the back buffer (fully overwrites it)
    packShadeBufTo1Bit(sMapShadeBuf, backBuf, width, height);

    // Swap front/back pointers (atomic via spinlock)
    taskENTER_CRITICAL(&sSwapSpinlock);
    uint8_t* temp = sMapFrontBuf;
    sMapFrontBuf = backBuf;
    sMapBackBuf  = temp;
    sFrontW = width;
    sFrontH = height;
    sFrontBufReady = true;
    taskEXIT_CRITICAL(&sSwapSpinlock);
  }
}

// Appends printf-formatted text to the context bar text in buf at pos and
// returns the new text length. Text that does not fit is cut at the end of the
// buffer and the length stays below size, so once the buffer is full further
// appends write nothing. A formatting error keeps the text before pos.
__attribute__((format(printf, 4, 5)))
static int appendMapContextText(char* buf, size_t size, int pos, const char* fmt, ...) {
  if (pos < 0 || (size_t)pos + 1 >= size) return pos;
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf + pos, size - (size_t)pos, fmt, args);
  va_end(args);
  if (n < 0) {
    buf[pos] = '\0';
    return pos;
  }
  const size_t end = (size_t)pos + (size_t)n;
  return end < size ? (int)end : (int)(size - 1);
}

// Display function for GPS Map mode
static void displayGpsMap() {
  if (!oledDisplay) return;

  // The keyboard (waypoint name or name search) owns the screen while it is
  // up. Completion/cancel is consumed by gpsMapInputHandler: completing clears
  // the keyboard's active flag, so this check never sees a finished keyboard.
  if (oledKeyboardIsActive()) {
    oledKeyboardDisplay(oledDisplay);
    return;
  }

  oledDisplay->setTextSize(1);
  
  // Performance tracking
  const uint32_t frameStart = millis();
  static uint32_t sPerfLastReport = 0;
  static uint32_t sPerfFrameCount = 0;
  static uint32_t sPerfTotalMs = 0;
  static uint32_t sPerfCacheHits = 0;
  static uint32_t sPerfCacheMisses = 0;
  
  float lat = 0.0f, lon = 0.0f;
  bool hasGpsFix = false;
  int satellites = 0;
  bool usingRemoteGps = false;
  
#if ENABLE_GPS_SENSOR
  // Use cached GPS data (sensor_gps continuously polls and updates the cache).
  // Read the cache struct, never sPA1010D directly — the cache lat/lon are
  // already signed (S/W negative).
  if (gGpsConnected && gGpsRunning) {
    GpsCache g;
    if (gpsCacheSnapshot(g)) {
      satellites = (int)g.satellites;
      if (g.hasFix) {
        lat = g.latitude;
        lon = g.longitude;
        hasGpsFix = true;
      }
    }
  }
#endif

#if ENABLE_ESPNOW
  // Fall back to remote GPS data if local GPS is not available or has no fix
  if (!hasGpsFix) {
    RemoteGpsData remoteGps;
    if (getRemoteGpsData(&remoteGps)) {
      lat = remoteGps.latitude;
      lon = remoteGps.longitude;
      hasGpsFix = true;
      satellites = remoteGps.satellites;
      usingRemoteGps = true;
    }
  }
#endif
  
  const LoadedMap& currentMap = MapCore::getCurrentMap();
  
  if (!currentMap.valid && !gSensorPollingPaused) {
    // Load the first available map, at most once per 5 s while none is
    // valid: loadMapFile pauses sensor polling and reads the file, so a map
    // that fails to load must not be retried on every frame.
    static bool sAutoLoadTried = false;
    static uint32_t sAutoLoadLastMs = 0;
    const uint32_t now = millis();
    if (!sAutoLoadTried || now - sAutoLoadLastMs >= 5000) {
      sAutoLoadTried = true;
      sAutoLoadLastMs = now;
      OledMapCtxGuard ctxGuard;
      char maps[8][96];
      int mapCount = MapCore::getAvailableMaps(maps, 8);
      if (mapCount > 0) {
        char path[128];
        snprintf(path, sizeof(path), "/maps/%s", maps[0]);
        MapCore::loadMapFile(path);
      }
    }
  }
  
  const LoadedMap& map = MapCore::getCurrentMap();
  
  if (!map.valid) {
    // Header is rendered by the system - content starts at OLED_CONTENT_START_Y
    int y = OLED_CONTENT_START_Y;
    oledDisplay->setCursor(0, y);
    oledDisplay->println("No map loaded");
    y += 10;
    oledDisplay->setCursor(0, y);
    oledDisplay->println("Upload .hwmap files");
    y += 10;
    oledDisplay->setCursor(0, y);
    oledDisplay->println("to /maps/ folder");
    
    if (gMapMenuOpen) {
      drawMapMenu();
    }
    return;
  }
  
  if (hasGpsFix && MapCore::isPositionInMap(lat, lon) && !gMapManuallyPanned) {
    gMapCenterLat = lat;
    gMapCenterLon = lon;
    gMapCenterSet = true;
  } else if (!gMapCenterSet) {
    gMapCenterLat = (map.header.minLat + map.header.maxLat) / 2000000.0f;
    gMapCenterLon = (map.header.minLon + map.header.maxLon) / 2000000.0f;
    gMapCenterSet = true;
    DEBUG_MAPS_RENDERINGF("[MAPS] OLED center from header: lat=%.5f lon=%.5f (minLat=%ld maxLat=%ld minLon=%ld maxLon=%ld)",
                          gMapCenterLat, gMapCenterLon,
                          (long)map.header.minLat, (long)map.header.maxLat,
                          (long)map.header.minLon, (long)map.header.maxLon);
  }
  
  // Initialize async renderer on first call; the render task comes and goes
  // with the map (ensureMapRenderTask / mapRenderTask's idle exit).
  initAsyncMapRenderer();
  const bool renderer = ensureMapRenderTask();
  
  // Create OLED renderer for overlays (context bar, text, etc.)
  if (!sOledMapRenderer) {
    sOledMapRenderer = new OledMapRenderer(oledDisplay);
  }
  if (sOledMapRenderer) sOledMapRenderer->setViewport(SCREEN_WIDTH, OLED_CONTENT_HEIGHT);
  
  // Check if viewport changed since last render request
  uint16_t currentLayers = mapLayersGetVisible();
  bool mapDirty = sForceRender ||
                  gMapCenterLat != sRenderedLat ||
                  gMapCenterLon != sRenderedLon ||
                  gMapZoom != sRenderedZoom ||
                  gMapRotation != sRenderedRotation ||
                  currentLayers != sRenderedLayers ||
                  SCREEN_WIDTH != sRenderedW || SCREEN_HEIGHT != sRenderedH;
  
  // With the render task refused the frame stays dirty for the retry.
  if (mapDirty && sRenderSemaphore && renderer) {
    // Precompute outside critical section
    MapRenderParams tmpParams = mapRenderParamsFromGlobals();
    bool hasTrack = GpsTrackManager::hasTrack();
    int32_t mapWidth = map.header.maxLon - map.header.minLon;
    int32_t mapHeight = map.header.maxLat - map.header.minLat;
    int32_t trkScaleX = (mapWidth > 0) ? (int32_t)((SCREEN_WIDTH * 1000L) / mapWidth * gMapZoom) : 1;
    int32_t trkScaleY = (mapHeight > 0) ? (int32_t)((OLED_CONTENT_HEIGHT * 1000L) / mapHeight * gMapZoom) : 1;
    
    // Write snapshot for render task (minimal critical section)
    taskENTER_CRITICAL(&sSwapSpinlock);
    sRenderSnapshot.centerLat = gMapCenterLat;
    sRenderSnapshot.centerLon = gMapCenterLon;
    sRenderSnapshot.zoom = gMapZoom;
    sRenderSnapshot.rotation = gMapRotation;
    sRenderSnapshot.visibleLayers = currentLayers;
    memcpy(sRenderSnapshot.subtypeVisibility, tmpParams.subtypeVisibility, sizeof(sRenderSnapshot.subtypeVisibility));
    sRenderSnapshot.hasTrack = hasTrack;
    sRenderSnapshot.trackScaleX = trkScaleX;
    sRenderSnapshot.trackScaleY = trkScaleY;
    sRenderSnapshot.width = (int16_t)SCREEN_WIDTH;
    sRenderSnapshot.height = (int16_t)SCREEN_HEIGHT;
    sRenderSnapshot.viewHeight = (int16_t)OLED_CONTENT_HEIGHT;
    sRenderSnapshot.offsetY = (int16_t)OLED_CONTENT_START_Y;
    taskEXIT_CRITICAL(&sSwapSpinlock);
    
    // Update dirty-tracking state
    sRenderedLat = gMapCenterLat;
    sRenderedLon = gMapCenterLon;
    sRenderedZoom = gMapZoom;
    sRenderedRotation = gMapRotation;
    sRenderedLayers = currentLayers;
    sRenderedW = (int16_t)SCREEN_WIDTH;
    sRenderedH = (int16_t)SCREEN_HEIGHT;
    sForceRender = false;
    
    // Signal the render task
    xSemaphoreGive(sRenderSemaphore);
    sPerfCacheMisses++;
  } else {
    sPerfCacheHits++;
  }
  
  // Always blit the front raster to the display
  if (sFrontBufReady) blitMapFrame();
  if (!renderer) {
    // No task to draw the raster (or refresh the one shown): say so.
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Map: low memory");
  }
  
  // Update location context if GPS has fix and enough time has passed
  if (hasGpsFix && LocationContextManager::shouldUpdate(lat, lon)) {
    LocationContextManager::updateContext(lat, lon);
  }
  
  // Draw context bar at top with location context
  const LocationContext& ctx = LocationContextManager::getContext();
  if (ctx.valid && hasGpsFix) {
    char contextBuf[128];
    int pos = 0;
    
    // Build context string: "On [road] • Near [area] ([distance])", cut at
    // the end of contextBuf (names are up to 63 characters each)
    if (ctx.nearestRoad[0] != '\0') {
      pos = appendMapContextText(contextBuf, sizeof(contextBuf), pos, "On %s", ctx.nearestRoad);
    }
    
    if (ctx.nearestArea[0] != '\0') {
      if (pos > 0) {
        pos = appendMapContextText(contextBuf, sizeof(contextBuf), pos, " • Near %s", ctx.nearestArea);
      } else {
        pos = appendMapContextText(contextBuf, sizeof(contextBuf), pos, "Near %s", ctx.nearestArea);
      }
      
      // Add distance if close enough to be relevant
      if (ctx.areaDistanceM < 500.0f) {
        pos = appendMapContextText(contextBuf, sizeof(contextBuf), pos, " (%.0fm)", ctx.areaDistanceM);
      }
    }
    
    // Update scroll offset for animation
    uint32_t now = millis();
    if (now - sContextScrollLastUpdate >= CONTEXT_SCROLL_SPEED_MS) {
      sContextScrollOffset += 2;  // Scroll 2 pixels at a time
      sContextScrollLastUpdate = now;
    }
    
    if (pos > 0) {
      sOledMapRenderer->drawContextBar(contextBuf, sContextScrollOffset);
    }
  } else {
    // No context - just show region name
    char regionBuf[24];
    snprintf(regionBuf, sizeof(regionBuf), " %.8s ", map.header.regionName);
    sOledMapRenderer->drawOverlayText(0, 0, regionBuf, true);
  }
  
  char overlayBuf[32];
  // Overlay columns: the middle and the right of the top row (on a 64 px
  // portrait row the zoom and the angle take the second row instead).
  const bool tall = oledPortrait();
  const int midX = tall ? 0 : 50;
  const int midY = tall ? 8 : 0;
  const int rightX = SCREEN_WIDTH - 28;
  if (gMapZoom != 1.0f || gMapRotation != 0.0f) {
    int pos = midX;
    if (gMapZoom != 1.0f) {
      snprintf(overlayBuf, sizeof(overlayBuf), " %.1fx ", gMapZoom);
      sOledMapRenderer->drawOverlayText(pos, midY, overlayBuf, true);
      pos += 30;
    }
    if (gMapRotation != 0.0f) {
      snprintf(overlayBuf, sizeof(overlayBuf), " %d\xf8 ", (int)gMapRotation);
      sOledMapRenderer->drawOverlayText(pos, midY, overlayBuf, true);
    }
  }
  
  // Show cache pressure indicator if tiles were dropped this frame
  if (map.valid && map.tilesDropped > 0) {
    snprintf(overlayBuf, sizeof(overlayBuf), " -%lu ", map.tilesDropped);
    sOledMapRenderer->drawOverlayText(midX, midY, overlayBuf, true);
  }

  // Show satellite count with source indicator
  if (hasGpsFix) {
    if (usingRemoteGps) {
      snprintf(overlayBuf, sizeof(overlayBuf), " R%dS ", satellites);  // "R" = Remote GPS
    } else {
      snprintf(overlayBuf, sizeof(overlayBuf), " %dS ", satellites);
    }
    sOledMapRenderer->drawOverlayText(rightX, 0, overlayBuf, true);
  }
#if ENABLE_GPS_SENSOR
  else if (gGpsRunning) {
    // Local GPS enabled but no fix - show searching indicator
    snprintf(overlayBuf, sizeof(overlayBuf), " %dS ", satellites);
    sOledMapRenderer->drawOverlayText(rightX, 0, overlayBuf, true);
  }
  
  // Show LIVE indicator when live tracking
  if (GpsTrackManager::isLiveTracking()) {
    sOledMapRenderer->drawOverlayText(0, sOledMapRenderer->getHeight() - 8, " LIVE ", true);
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
      sOledMapRenderer->drawOverlayText(0, sOledMapRenderer->getHeight() - 8, overlayBuf, true);
    }
  }
  
  // Show search results navigation indicator
  if (sSearchResultsActive && sSearchResultCount > 1) {
    snprintf(overlayBuf, sizeof(overlayBuf), " %d/%d <> ", sSearchResultCurrent + 1, sSearchResultCount);
    sOledMapRenderer->drawOverlayText(tall ? 0 : 40, sOledMapRenderer->getHeight() - (tall ? 16 : 8), overlayBuf, true);
  }
  
  if (gMapMenuOpen) {
    drawMapMenu();
  }
  
  // Performance reporting (every 5 seconds)
  uint32_t frameMs = millis() - frameStart;
  sPerfFrameCount++;
  sPerfTotalMs += frameMs;
  if (millis() - sPerfLastReport >= 5000) {
    float avgMs = (sPerfFrameCount > 0) ? (float)sPerfTotalMs / sPerfFrameCount : 0;
    float fps = (sPerfTotalMs > 0) ? (sPerfFrameCount * 1000.0f / (millis() - sPerfLastReport)) : 0;
    DEBUG_MAPS_PERFF("[MAP_PERF] frame: %.1fms avg | %.1f FPS | %lu frames | cache hit:%lu miss:%lu (%.0f%%)",
                     avgMs, fps, (unsigned long)sPerfFrameCount,
                     (unsigned long)sPerfCacheHits, (unsigned long)sPerfCacheMisses,
                     (sPerfCacheHits + sPerfCacheMisses > 0) ? 
                       (100.0f * sPerfCacheHits / (sPerfCacheHits + sPerfCacheMisses)) : 0.0f);
    sPerfLastReport = millis();
    sPerfFrameCount = 0;
    sPerfTotalMs = 0;
    sPerfCacheHits = 0;
    sPerfCacheMisses = 0;
  }
}

static bool gpsMapAvailable(String* outReason) {
  return true;
}

// Go back to main menu from submenu
static void goBackToMainMenu() {
  sMapSubmenuLevel = 0;
  sMapMenuSelection = sMapSubmenuType;
  sMapMenuScrollOffset = 0;
}

// Select Map: the file browser as a picker rooted at /maps. The browser lists
// and navigates under its own OLED identity; picking a .hwmap pops back here
// and loads it.
static bool mapPickerShowsFile(const FileEntry& entry) {
  const size_t len = strlen(entry.name);
  return len > 6 && strcasecmp(entry.name + len - 6, ".hwmap") == 0;
}

static void mapPickerOnPicked(const char* fullPath, bool cancelled) {
  if (cancelled || !fullPath) return;
  OledMapCtxGuard ctxGuard;
  if (MapCore::loadMapFile(fullPath)) {
    gMapCenterSet = false;
    gMapManuallyPanned = false;
  } else {
    oledToastShow("Map load failed", 3000, OledUiIcon::ERROR);
  }
}

static void openMapPicker() {
  FilePickerRequest req = {};
  strlcpy(req.title, "Select Map", sizeof(req.title));
  strlcpy(req.startPath, "/maps", sizeof(req.startPath));
  req.filter = mapPickerShowsFile;
  req.onPicked = mapPickerOnPicked;
  req.requesterMode = OLED_GPS_MAP;
  if (!oledFilePickerPush(req)) {
    oledToastShow("File picker busy", 3000, OledUiIcon::ERROR);
    return;
  }
  requestOledMode(OLED_FILE_BROWSER, "map.browse.maps");  // pushes currentOledMode so the pick returns here
}

// Mark Waypoint completion: add at the map center as the OLED user and say
// why when nothing was added.
static void markWaypointAtCenter(const char* name) {
  OledMapCtxGuard ctxGuard;
  const int idx = WaypointManager::addWaypoint(gMapCenterLat, gMapCenterLon, name);
  if (idx >= 0) {
    INFO_MAPSF("Marked waypoint %d: %s at %.5f, %.5f", idx, name, gMapCenterLat, gMapCenterLon);
  } else {
    oledToastShow(idx == WaypointManager::kNotSaved ? "Waypoint not saved" : "No free waypoint slots",
                  3000, OledUiIcon::ERROR);
  }
}

// Execute submenu action based on current submenu type and selection
static void executeSubmenuAction(int submenuType, int action) {
  // Guests may pan/zoom/view; GPS toggle, waypoints, and track logging mutate.
  bool guestMutate = (submenuType == MENU_CAT_WAYPOINTS || submenuType == MENU_CAT_TRACKS);
#if ENABLE_GPS_SENSOR
  guestMutate = guestMutate || (submenuType == MENU_CAT_GPS);
#endif
  if (guestMutate && oledGuestBlocksMutate()) {
    return;
  }
  char maps[8][96];
  int mapCount;
  int currentIdx;
  const LoadedMap& currentMap = MapCore::getCurrentMap();
  // Map list/load, navigation target and track scan run as the OLED user.
  OledMapCtxGuard ctxGuard;

  switch (submenuType) {
    case MENU_CAT_VIEW:
      switch (action) {
        case 0: gMapZoom = fminf(gMapZoom * kMapZoomStep, kMapZoomMax); break;  // Zoom In
        case 1: gMapZoom = fmaxf(gMapZoom / kMapZoomStep, kMapZoomMin); break;  // Zoom Out
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
            openMapPicker();
            gMapMenuOpen = false;
            sMapSubmenuLevel = 0;
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
            snprintf(path, sizeof(path), "/maps/%.95s", maps[(currentIdx + 1) % mapCount]);
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
            snprintf(path, sizeof(path), "/maps/%.95s", maps[(currentIdx - 1 + mapCount) % mapCount]);
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
          {
            GpsCache g;
            if (gGpsConnected && gpsCacheSnapshot(g) && g.hasFix) {
              gMapCenterLat = g.latitude;   // cache lat/lon already signed
              gMapCenterLon = g.longitude;
              gMapCenterSet = true;
              gMapManuallyPanned = false;
            }
          }
          break;
        case 1:  // Toggle GPS
          {
            // The row says "Toggle GPS" without a state: toast the result.
            if (gGpsRunning) executeOledCommandShowResult("closegps");
            else executeOledCommandShowResult("opengps");
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
            oledKeyboardInit(
                "Name Waypoint", defaultName, WAYPOINT_NAME_LEN - 1,
                OledKeyboardDictationPolicy::ALLOW_PLAINTEXT);
            sMapWaypointNameMode = true;
            gMapMenuOpen = false;
            sMapSubmenuLevel = 0;
          }
          break;
        case 1:  // Goto Waypoint
          if (WaypointManager::getActiveCount() > 0) {
            sWaypointSelectMode = 1;
            sWaypointSelectIdx = 0;
          }
          break;
        case 2:  // Clear Nav
          WaypointManager::selectTarget(-1);
          break;
        case 3:  // Delete Waypoint
          if (WaypointManager::getActiveCount() > 0) {
            sWaypointSelectMode = 2;
            sWaypointSelectIdx = 0;
          }
          break;
        case 4: goBackToMainMenu(); break;  // Back
      }
      break;

    case MENU_CAT_TRACKS:
      switch (action) {
        case 0:  // Load Track
          scanTrackFiles();
          if (sTrackFileCount > 0) {
            sTrackSelectMode = 1;
            sTrackFileIdx = 0;
          }
          break;
        case 1:  // Clear Track
          {
            char result[160] = {};
            const bool executed = executeOledCommandWithResult(
                "gpstrack clear", result, sizeof(result));
            const char* message = commandReplyBody(result);
            if (!executed || strcmp(message, kGpsTrackReplyCleared) != 0) {
              char text[sizeof(result)];
              oledCommandReplyText(false, result, nullptr, "Clear track failed", text, sizeof(text));
              oledToastShow(text, 3000, OledUiIcon::ERROR);
            } else {
              sForceRender = true;
            }
          }
          break;
        case 2:  // Track Status
          sShowTrackStatus = true;
          break;
        case 3:  // Delete Track
          scanTrackFiles();
          if (sTrackFileCount > 0) {
            sTrackSelectMode = 2;  // Delete mode
            sTrackFileIdx = 0;
          }
          break;
        case 4:  // Live Track - toggle
          {
            // One owner decides whether to start or stop, under this display's
            // authenticated command session, and publishes the resulting state.
            char result[160] = {};
            const bool executed = executeOledCommandWithResult(
                "gpstrack live toggle", result, sizeof(result));
            if (!executed || !commandReplyConfirms(result)) {
              // The shared short refusal ("Needs an admin": logging is an
              // administrator's job).
              char text[sizeof(result)];
              oledCommandReplyText(false, result, nullptr, "Live track failed", text, sizeof(text));
              oledToastShow(text, 3000, OledUiIcon::ERROR);
            } else {
              sForceRender = true;
            }
          }
          break;
        case 5:  // Save Track
          {
            char savedPath[96];
            if (GpsTrackManager::saveTrack(savedPath, sizeof(savedPath))) {
              // Show brief confirmation via track status overlay
              sShowTrackStatus = true;
            } else {
              oledToastShow("Save track failed", 3000, OledUiIcon::ERROR);
            }
          }
          break;
        case 6: goBackToMainMenu(); break;  // Back
      }
      break;

    case MENU_CAT_INFO:
      switch (action) {
        case 0:  // Map Info
          sShowMapInfo = true;
          break;
        case 1:  // Features
          sShowFeatures = true;
          sFeaturesCategory = 0;
          sFeaturesScrollOffset = 0;
          break;
        case 2:  // Search Names
          sMapSearchMode = true;
          gMapMenuOpen = false;
          oledKeyboardInit("Search:", "", 20,
                           OledKeyboardDictationPolicy::ALLOW_PLAINTEXT);
          oledKeyboardSetAutocomplete(mapNameAutocomplete, nullptr);
          break;
        case 3:  // Transit Routes
          sShowRoutes = true;
          sRoutesScrollOffset = 0;
          break;
        case 4: goBackToMainMenu(); break;  // Back
      }
      break;

    case MENU_CAT_LAYERS:
      if (action == kMapLayerTypeCount) {
        // Back item
        goBackToMainMenu();
      } else if (action >= 0 && action < kMapLayerTypeCount) {
        const MapLayerTypeInfo& t = kMapLayerTypes[action];
        // Skip if this layer type is not present in the loaded map
        const LoadedMap& layerMap = MapCore::getCurrentMap();
        uint16_t ftm = layerMap.valid ? layerMap.header.featureTypeMask : 0xFFFF;
        if ((ftm & t.hwmapMask) == 0) break;  // Not present, ignore
        if (t.subtypeCount > 0) {
          // Enter subtype sub-submenu
          sLayerTypeSelectIdx = action;
          sMapSubmenuLevel = 2;
          sMapMenuSelection = 0;
          sMapMenuScrollOffset = 0;
        } else {
          // No subtypes: toggle type directly and stay in layers list
          mapLayerToggle(t.layerFlag);
          sForceRender = true;
        }
      }
      break;
  }
}

// Execute main menu action (opens submenus or closes)
static void executeMainMenuAction(int action) {
#if ENABLE_GPS_SENSOR
  // With GPS: View, Maps, GPS, Waypoints, Tracks, Info, Layers, Close
  switch (action) {
    case MENU_CAT_VIEW:
    case MENU_CAT_MAPS:
    case MENU_CAT_GPS:
    case MENU_CAT_WAYPOINTS:
    case MENU_CAT_TRACKS:
    case MENU_CAT_INFO:
    case MENU_CAT_LAYERS:
      sMapSubmenuLevel = 1;
      sMapSubmenuType = action;
      sMapMenuSelection = 0;
      sMapMenuScrollOffset = 0;
      break;
    case MENU_CAT_CLOSE:
      gMapMenuOpen = false;
      sMapSubmenuLevel = 0;
      break;
  }
#else
  // Without GPS: View, Maps, Waypoints, Tracks, Info, Layers, Close
  int mappedAction = action;
  if (action >= 2) mappedAction = action + 1;  // Skip GPS category
  
  switch (mappedAction) {
    case MENU_CAT_VIEW:
    case MENU_CAT_MAPS:
    case MENU_CAT_WAYPOINTS:
    case MENU_CAT_TRACKS:
    case MENU_CAT_INFO:
    case MENU_CAT_LAYERS:
      sMapSubmenuLevel = 1;
      sMapSubmenuType = mappedAction;
      sMapMenuSelection = 0;
      sMapMenuScrollOffset = 0;
      break;
    default:
      gMapMenuOpen = false;
      sMapSubmenuLevel = 0;
      break;
  }
#endif
}

// Execute a menu action based on current menu level
static void executeMapMenuAction(int action) {
  if (sMapSubmenuLevel == 0) {
    executeMainMenuAction(action);
  } else if (sMapSubmenuLevel == 2) {
    executeSubtypeMenuAction(sLayerTypeSelectIdx, action);
  } else {
    executeSubmenuAction(sMapSubmenuType, action);
  }
}

// Input handler for GPS Map mode. newlyPressed holds the input device's own
// press bits, so buttons are read with INPUT_CHECK (HAL_Input maps them per
// device) and list movement with gNavEvents, which the router sets for the
// joystick, the wheel and the ANO dpad alike. deltaX/deltaY are analog stick
// deflection (0 on wheel devices) and only drive pan and rotate.
static bool gpsMapInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Start button: toggle menu
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_START)) {
    gMapMenuOpen = !gMapMenuOpen;
    if (gMapMenuOpen) {
      sMapMenuSelection = 0;
      sMapMenuScrollOffset = 0;
      sMapSubmenuLevel = 0;  // Reset to main menu
    }
    return true;
  }

  // Waypoint name entry (Mark Waypoint). The central router feeds the keyboard
  // while it is active; the first input after it closes lands here.
  if (sMapWaypointNameMode) {
    if (oledKeyboardIsActive()) {
      oledKeyboardHandleInput(deltaX, deltaY, newlyPressed);
      return true;
    }
    if (oledKeyboardIsCompleted()) {
      markWaypointAtCenter(oledKeyboardGetText());
    }
    sMapWaypointNameMode = false;
    oledKeyboardReset();
    return true;
  }

  // Search mode - keyboard handles input
  if (sMapSearchMode) {
    if (oledKeyboardIsActive()) {
      oledKeyboardHandleInput(deltaX, deltaY, newlyPressed);
      return true;
    }
    
    // Keyboard completed or cancelled
    if (oledKeyboardIsCompleted()) {
      strlcpy(sSearchResult, oledKeyboardGetText(), sizeof(sSearchResult));
      DEBUG_MAPSF("[MAP_SEARCH] Selected: '%s'", sSearchResult);
      
      // Find ALL matching features (iterate through tiles)
      sSearchResultCount = 0;
      sSearchResultCurrent = 0;
      // Hold the map lock across the whole tile scan (load + parse) so the
      // async mapRenderTask can't evict a slot under the parser.
      MapCacheGuard mapGuard("OLED.mapSearch");
      const LoadedMap& map = MapCore::getCurrentMap();
      if (map.valid && map.tileDir && sSearchResult[0] != '\0') {
        // Iterate through all tiles
        for (uint16_t tileIdx = 0; tileIdx < map.tileCount && sSearchResultCount < 32; tileIdx++) {
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
          
          for (uint16_t f = 0; f < featureCount && sSearchResultCount < 32; f++) {
            if (ptr + HWMAP_FEATURE_HEADER_SIZE > end) break;
            
            // type(1) + subtype(1) + nameIndex(2) + pointCount(2)
            uint16_t nameIndex = ptr[2] | (ptr[3] << 8);
            uint16_t pointCount = ptr[4] | (ptr[5] << 8);
            
            const char* featureName = MapCore::getName(nameIndex);
            if (featureName && strcmp(featureName, sSearchResult) == 0 && pointCount > 0) {
              // Dequantize first point and store coordinates (points start after header)
              const uint8_t* pointsPtr = ptr + HWMAP_FEATURE_HEADER_SIZE;
              uint16_t qLat = pointsPtr[0] | (pointsPtr[1] << 8);
              uint16_t qLon = pointsPtr[2] | (pointsPtr[3] << 8);
              int32_t lat = tileMinLat + (int32_t)((int64_t)qLat * haloLatSpan >> 16);
              int32_t lon = tileMinLon + (int32_t)((int64_t)qLon * haloLonSpan >> 16);
              gSearchResultCoords[sSearchResultCount].lat = lat / 1000000.0f;
              gSearchResultCoords[sSearchResultCount].lon = lon / 1000000.0f;
              sSearchResultCount++;
            }
            
            ptr += HWMAP_FEATURE_HEADER_SIZE + pointCount * 4;  // Skip to next feature
          }
        }
        
        // Center on first result
        if (sSearchResultCount > 0) {
          gMapCenterLat = gSearchResultCoords[0].lat;
          gMapCenterLon = gSearchResultCoords[0].lon;
          gMapCenterSet = true;
          gMapManuallyPanned = true;
          sSearchResultsActive = (sSearchResultCount > 1);
          
          DEBUG_MAPSF("[MAP_SEARCH] Found %d matches for '%s', showing 1/%d", 
                       sSearchResultCount, sSearchResult, sSearchResultCount);
        }
      }
    }
    sMapSearchMode = false;
    oledKeyboardReset();
    return true;
  }
  
  // Search results navigation (when multiple matches exist)
  if (sSearchResultsActive && sSearchResultCount > 1) {
    // B exits first: on the ANO encoder LEFT is B and also a left nav event.
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      sSearchResultsActive = false;
      return true;
    }
    // Left/Right or the wheel steps between results
    if (gNavEvents.left || gNavEvents.wheelDelta < 0) {  // previous result
      sSearchResultCurrent = (sSearchResultCurrent - 1 + sSearchResultCount) % sSearchResultCount;
      goto centerOnCurrentResult;
    }
    if (gNavEvents.right || gNavEvents.wheelDelta > 0) {  // next result
      sSearchResultCurrent = (sSearchResultCurrent + 1) % sSearchResultCount;
      goto centerOnCurrentResult;
    }

    centerOnCurrentResult: {
      // Use stored coordinates directly (tiled format stores coords, not indices)
      gMapCenterLat = gSearchResultCoords[sSearchResultCurrent].lat;
      gMapCenterLon = gSearchResultCoords[sSearchResultCurrent].lon;
      DEBUG_MAPSF("[MAP_SEARCH] Showing result %d/%d", 
                   sSearchResultCurrent + 1, sSearchResultCount);
      return true;
    }
  }
  
  // Map info overlay - A or B dismisses
  if (sShowMapInfo) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      sShowMapInfo = false;
      gMapMenuOpen = false;
      sMapSubmenuLevel = 0;
      return true;
    }
    return true;
  }
  
  // Track status overlay - A or B dismisses
  if (sShowTrackStatus) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      sShowTrackStatus = false;
      gMapMenuOpen = false;
      sMapSubmenuLevel = 0;
      return true;
    }
    return true;
  }
  
  // Features viewer input handling
  if (sShowFeatures) {
    const LoadedMap& map = MapCore::getCurrentMap();

    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      sShowFeatures = false;
      gMapMenuOpen = false;
      sMapSubmenuLevel = 0;
      return true;
    }

    if (gNavEvents.left) {
      sFeaturesCategory = (sFeaturesCategory + 5) % 6;
      sFeaturesScrollOffset = 0;
      return true;
    }
    if (gNavEvents.right) {
      sFeaturesCategory = (sFeaturesCategory + 1) % 6;
      sFeaturesScrollOffset = 0;
      return true;
    }

    if (map.valid && map.nameCount > 0) {
      if (gNavEvents.up && sFeaturesScrollOffset > 0) {
        sFeaturesScrollOffset--;
        return true;
      }
      if (gNavEvents.down && sFeaturesScrollOffset < map.nameCount - oledVisibleRows(10, 10)) {
        sFeaturesScrollOffset++;
        return true;
      }
    }
    
    return true;
  }
  
  // Routes viewer input handling
  if (sShowRoutes) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {  // B button - close
      sShowRoutes = false;
      gMapMenuOpen = false;
      sMapSubmenuLevel = 0;
      return true;
    }
    
    // Build route list (same logic as drawRoutes) using tiled architecture.
    // Hold the map lock across the tile scan AND the subsequent map.* /
    // map.names[] use below, so a concurrent evict (mapRenderTask) or unload
    // (file delete) can't invalidate the pool or name table mid-branch.
    MapCacheGuard mapGuard("OLED.routesList");
    const LoadedMap& map = MapCore::getCurrentMap();
    EXT_RAM_BSS_ATTR static RouteInfo routeList[32];  // PSRAM: OLED task only, under MapCacheGuard
    EXT_RAM_BSS_ATTR static float routeFirstLat[32];  // Store first point coords for goto
    EXT_RAM_BSS_ATTR static float routeFirstLon[32];
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
        
        for (uint16_t f = 0; f < featureCount && routeCount < 32; f++) {
          if (ptr + HWMAP_FEATURE_HEADER_SIZE > end) break;
          
          uint8_t type = ptr[0];
          // type(1) + subtype(1) + nameIndex(2) + pointCount(2)
          uint16_t nameIdx = ptr[2] | (ptr[3] << 8);
          uint16_t pointCount = ptr[4] | (ptr[5] << 8);
          
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
                const uint8_t* pointsPtr = ptr + HWMAP_FEATURE_HEADER_SIZE;
                uint16_t qLat = pointsPtr[0] | (pointsPtr[1] << 8);
                uint16_t qLon = pointsPtr[2] | (pointsPtr[3] << 8);
                int32_t lat = tileMinLat + (int32_t)((int64_t)qLat * haloLatSpan >> 16);
                int32_t lon = tileMinLon + (int32_t)((int64_t)qLon * haloLonSpan >> 16);
                
                routeList[routeCount].name = name;
                routeList[routeCount].type = type;
                routeFirstLat[routeCount] = lat / 1000000.0f;
                routeFirstLon[routeCount] = lon / 1000000.0f;
                routeCount++;
              }
            }
          }
          
          ptr += HWMAP_FEATURE_HEADER_SIZE + pointCount * 4;  // Skip to next feature
        }
      }
    }
    
    // Buttons before navigation: ANO DOWN is X and also a down nav event, and
    // the wheel is what moves the selection there.
    // X button - highlight route on map (toggle blink)
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
      if (map.valid && routeCount > 0 && sRoutesSelectedIdx < routeCount) {
        if (gMapHighlight.active && strcmp(gMapHighlight.name, routeList[sRoutesSelectedIdx].name) == 0) {
          // Toggle off if same route
          mapHighlightClear();
        } else {
          // Enable highlighting for selected route using generic system
          mapHighlightByNameAndType(routeList[sRoutesSelectedIdx].name, 
                                     routeList[sRoutesSelectedIdx].type, 300);
        }
        sShowRoutes = false;
        gMapMenuOpen = false;
        sMapSubmenuLevel = 0;
        return true;
      }
    }
    
    // A button - goto selected route (center on first point)
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      if (map.valid && routeCount > 0 && sRoutesSelectedIdx < routeCount) {
        // Use stored coordinates directly
        gMapCenterLat = routeFirstLat[sRoutesSelectedIdx];
        gMapCenterLon = routeFirstLon[sRoutesSelectedIdx];
        gMapCenterSet = true;
        gMapManuallyPanned = true;
        sShowRoutes = false;
        gMapMenuOpen = false;
        sMapSubmenuLevel = 0;
        return true;
      }
    }

    // Navigate selection
    if (gNavEvents.up && sRoutesSelectedIdx > 0) {
      sRoutesSelectedIdx--;
      if (sRoutesSelectedIdx < sRoutesScrollOffset) sRoutesScrollOffset = sRoutesSelectedIdx;
      return true;
    }
    if (gNavEvents.down && sRoutesSelectedIdx < routeCount - 1) {
      sRoutesSelectedIdx++;
      if (sRoutesSelectedIdx >= sRoutesScrollOffset + oledVisibleRows(10, 10)) sRoutesScrollOffset = sRoutesSelectedIdx - oledVisibleRows(10, 10) + 1;
      return true;
    }

    return true;
  }

  // Track file selection mode input handling
  if (sTrackSelectMode != 0) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {  // A button: load or delete track
      if (sTrackFileCount > 0 && sTrackFileIdx < sTrackFileCount) {
        OledMapCtxGuard ctxGuard;  // the track owner opens/removes the file as the OLED user
        if (sTrackSelectMode == 1) {
          // Load track
          String errorMsg;
          GpsTrackStatus track;
          if (GpsTrackManager::loadTrack(gTrackFiles[sTrackFileIdx], errorMsg, track, true)) {
            sForceRender = true;
          } else {
            oledToastShow(errorMsg.length() ? errorMsg.c_str() : "Load track failed",
                          3000, OledUiIcon::ERROR);
          }
        } else if (sTrackSelectMode == 2) {
          // Delete track
          if (GpsTrackManager::deleteTrackFile(gTrackFiles[sTrackFileIdx])) {
            sForceRender = true;
          } else {
            oledToastShow("Delete track failed", 3000, OledUiIcon::ERROR);
          }
        }
      }
      sTrackSelectMode = 0;
      gMapMenuOpen = false;
      sMapSubmenuLevel = 0;
      return true;
    }
    
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {  // B button: cancel
      sTrackSelectMode = 0;
      return true;
    }

    // Navigate track files
    if (gNavEvents.up && sTrackFileIdx > 0) {
      sTrackFileIdx--;
      return true;
    }
    if (gNavEvents.down && sTrackFileIdx < sTrackFileCount - 1) {
      sTrackFileIdx++;
      return true;
    }
    
    return true;
  }
  
  // Waypoint selection mode input handling
  if (sWaypointSelectMode != 0) {
    int wpCount = WaypointManager::getActiveCount();

    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      OledMapCtxGuard ctxGuard;  // target and deletion are saved as the OLED user
      int activeIdx = 0;
      for (int i = 0; i < MAX_WAYPOINTS; i++) {
        const Waypoint* wp = WaypointManager::getWaypoint(i);
        if (wp) {
          if (activeIdx == sWaypointSelectIdx) {
            if (sWaypointSelectMode == 1) {
              WaypointManager::selectTarget(i);
              gMapCenterLat = wp->lat;
              gMapCenterLon = wp->lon;
              gMapCenterSet = true;
              gMapManuallyPanned = true;
            } else if (sWaypointSelectMode == 2) {
              // i is an active slot, so false means the file write failed.
              if (!WaypointManager::deleteWaypoint(i)) {
                oledToastShow("Delete not saved", 3000, OledUiIcon::ERROR);
              }
            }
            break;
          }
          activeIdx++;
        }
      }
      sWaypointSelectMode = 0;
      gMapMenuOpen = false;
      sMapSubmenuLevel = 0;
      return true;
    }
    
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      sWaypointSelectMode = 0;
      return true;
    }

    if (gNavEvents.up && sWaypointSelectIdx > 0) {
      sWaypointSelectIdx--;
      return true;
    }
    if (gNavEvents.down && sWaypointSelectIdx < wpCount - 1) {
      sWaypointSelectIdx++;
      return true;
    }
    
    return true;
  }
  
  // Menu navigation when open
  if (gMapMenuOpen) {
    const int maxVisible = oledVisibleRows(9, 18 + MAP_DY - OLED_CONTENT_START_Y, 4);
    
    // Get item count for current menu level
    int menuItemCount;
    if (sMapSubmenuLevel == 0) {
      menuItemCount = kMapMainMenuItemCount;
    } else if (sMapSubmenuLevel == 2) {
      int sc = (sLayerTypeSelectIdx >= 0 && sLayerTypeSelectIdx < kMapLayerTypeCount)
                 ? kMapLayerTypes[sLayerTypeSelectIdx].subtypeCount : 0;
      menuItemCount = 1 + sc + 1;  // "All" + subtypes + "< Back"
    } else {
      switch (sMapSubmenuType) {
        case MENU_CAT_VIEW: menuItemCount = kViewSubmenuCount; break;
        case MENU_CAT_MAPS: menuItemCount = kMapsSubmenuCount; break;
#if ENABLE_GPS_SENSOR
        case MENU_CAT_GPS: menuItemCount = kGpsSubmenuCount; break;
#endif
        case MENU_CAT_WAYPOINTS: menuItemCount = kWaypointsSubmenuCount; break;
        case MENU_CAT_TRACKS: menuItemCount = kTracksSubmenuCount; break;
        case MENU_CAT_INFO: menuItemCount = kInfoSubmenuCount; break;
        case MENU_CAT_LAYERS: menuItemCount = kMapLayerTypeCount + 1; break;
        default: menuItemCount = kMapMainMenuItemCount; break;
      }
    }
    
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
      executeMapMenuAction(sMapMenuSelection);
      return true;
    }

    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      // B button: go back one level
      if (sMapSubmenuLevel == 2) {
        // Subtype submenu → layers submenu, restore selection
        sMapSubmenuLevel = 1;
        sMapMenuSelection = sLayerTypeSelectIdx;
        sMapMenuScrollOffset = max(0, sLayerTypeSelectIdx - oledVisibleRows(9, 18 + MAP_DY - OLED_CONTENT_START_Y, 4) + 1);
      } else if (sMapSubmenuLevel > 0) {
        goBackToMainMenu();
      } else {
        gMapMenuOpen = false;
      }
      return true;
    }
    
    if (gNavEvents.up && sMapMenuSelection > 0) {
      sMapMenuSelection--;
      if (sMapMenuSelection < sMapMenuScrollOffset) {
        sMapMenuScrollOffset = sMapMenuSelection;
      }
      return true;
    }
    if (gNavEvents.down && sMapMenuSelection < menuItemCount - 1) {
      sMapMenuSelection++;
      if (sMapMenuSelection >= sMapMenuScrollOffset + maxVisible) {
        sMapMenuScrollOffset = sMapMenuSelection - maxVisible + 1;
      }
      return true;
    }
    
    return true;
  }
  
  // Normal map controls when menu is closed
  
  bool aHeld = false;
#if ENABLE_OLED_INPUT
  {
    InputCache input{};
    if (inputCacheSnapshot(input, 5)) {
      // buttons stores active-low (1 = unpressed). Inverting + mask
      // gives "currently pressed" for whichever logical A bit the active
      // input driver uses.
      aHeld = (~inputNavButtons(input) & INPUT_MASK(INPUT_BUTTON_A)) != 0;
    }
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
    
    // Pan follows the joystick direction (push right → view moves east,
    // push up → view moves north). accelLon/accelLat already include the
    // rotation matrix, so negating both keeps the mapping correct at any
    // gMapRotation.
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
    
    // Clamp center to map bounds (with margin so user can pan slightly past edge)
    const LoadedMap& clampMap = MapCore::getCurrentMap();
    if (clampMap.valid) {
      float mapMinLat = clampMap.header.minLat / 1000000.0f;
      float mapMaxLat = clampMap.header.maxLat / 1000000.0f;
      float mapMinLon = clampMap.header.minLon / 1000000.0f;
      float mapMaxLon = clampMap.header.maxLon / 1000000.0f;
      // Allow panning up to half the viewport span past the edge
      float marginLat = (mapMaxLat - mapMinLat) * 0.5f / gMapZoom;
      float marginLon = (mapMaxLon - mapMinLon) * 0.5f / gMapZoom;
      gMapCenterLat = fmaxf(mapMinLat - marginLat, fminf(mapMaxLat + marginLat, gMapCenterLat));
      gMapCenterLon = fmaxf(mapMinLon - marginLon, fminf(mapMaxLon + marginLon, gMapCenterLon));
    }
  }
  
  // X button: zoom in
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    gMapZoom = fminf(gMapZoom * kMapZoomStep, kMapZoomMax);
    return true;
  }
  // Y button: zoom out
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
    gMapZoom = fmaxf(gMapZoom / kMapZoomStep, kMapZoomMin);
    return true;
  }

  // B button: leave the map (the router goes back)
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    return false;
  }
  
  return panned;
}

// The footer's hints for the map and its menu screens (drawOledFooter). The
// names and routes viewers carry their own controls here instead of a hint
// line inside their list.
const char* oledMapFooterHint() {
  if (!gMapMenuOpen) return "X/Y:Zoom A+J:Rot B:Back";
  if (sShowMapInfo || sShowTrackStatus) return "B:OK";
  if (sShowFeatures) return "</>:Cat ^v:Scrl B:OK";
  if (sShowRoutes) return "^v:Sel A:Go X:Hl B:X";
  return "A:Select B:Close";
}

// Map OLED mode entry
// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints
static const OledModeEntry kGpsMapOledModes[] = {
  {
    OLED_GPS_MAP,
    "Map",
    "map",
    displayGpsMap,
    gpsMapAvailable,
    gpsMapInputHandler,
    true,
    50,
    nullptr,  // dynamic hints (menu vs map view)
    nullptr,  // onEnterFunc
    gpsMapHeader  // popup titles in the header bar
  }
};

REGISTER_OLED_MODE_MODULE(kGpsMapOledModes, 1, "Map");

#endif // ENABLE_OLED_DISPLAY
