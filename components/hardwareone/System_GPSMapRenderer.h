#ifndef SYSTEM_GPS_MAP_RENDERER_H
#define SYSTEM_GPS_MAP_RENDERER_H

#include <Arduino.h>

#include "System_BuildConfig.h"

// =============================================================================
// HardwareOne Map (.hwmap) File Format
// =============================================================================
// Binary format for compact offline maps on ESP32
//
// Header (36 bytes):
//   Magic: "HWMP" (4 bytes)
//   Version: uint16 (2 bytes)
//   Flags: uint16 (2 bytes, bit 0 = has metadata)
//   Bounds: minLat, minLon, maxLat, maxLon (4x int32, microdegrees)
//   FeatureCount: uint32 (4 bytes)
//   RegionName: 8 bytes (null-padded)
//
// Features (variable):
//   Type: uint8 (0x00=highway, 0x01=major road, 0x02=minor road, 0x03=path,
//                0x10=water, 0x11=park, 0x20=railway, 0x30=building)
//   PointCount: uint8
//   FirstPoint: lat + lon (2x int32, microdegrees)
//   DeltaPoints: (PointCount-1) x (deltaLat + deltaLon as int16)
//
// Metadata Section (if flags & 0x0001):
//   SectionSize: uint32 (4 bytes)
//   EntryCount: uint16 (2 bytes)
//   Entries:
//     Category: uint8 (0=highway, 1=road, 2=water, 3=park, 4=railway, 5=subway)
//     StringLen: uint8
//     String: N bytes (not null-terminated)

// =============================================================================
// Feature Types (must match web tool)
// =============================================================================

enum MapFeatureType : uint8_t {
  MAP_FEATURE_HIGHWAY    = 0x00,
  MAP_FEATURE_ROAD_MAJOR = 0x01,
  MAP_FEATURE_ROAD_MINOR = 0x02,
  MAP_FEATURE_PATH       = 0x03,
  MAP_FEATURE_WATER      = 0x10,
  MAP_FEATURE_PARK       = 0x11,
  MAP_FEATURE_RAILWAY    = 0x20,
  MAP_FEATURE_BUILDING   = 0x30
};

// =============================================================================
// Line Styles for Rendering
// =============================================================================

enum MapLineStyle : uint8_t {
  LINE_SOLID,
  LINE_DASHED,
  LINE_DOTTED,
  LINE_NONE       // Don't render
};

// Feature rendering style (display-agnostic)
struct MapFeatureStyle {
  MapLineStyle lineStyle;
  uint8_t lineWeight;     // 1 = thin, 2 = medium, 3 = thick
  uint8_t priority;       // Higher = render later (on top)
  bool render;            // Whether to render at all
  // Color info for color displays (RGB565 or similar)
  uint16_t color;
};

// =============================================================================
// Map Data Structures
// =============================================================================

// Map header structure (36 bytes)
struct HWMapHeader {
  char magic[4];           // "HWMP"
  uint16_t version;
  uint16_t flags;
  int32_t minLat;          // Microdegrees (lat * 1000000)
  int32_t minLon;
  int32_t maxLat;
  int32_t maxLon;
  uint32_t featureCount;
  char regionName[8];
} __attribute__((packed));

// Feature header (read from file, points follow)
struct HWMapFeatureHeader {
  uint8_t type;
  uint8_t pointCount;
} __attribute__((packed));

// Metadata category codes (for feature names)
enum MapMetadataCategory : uint8_t {
  META_CAT_HIGHWAY = 0,
  META_CAT_ROAD    = 1,
  META_CAT_WATER   = 2,
  META_CAT_PARK    = 3,
  META_CAT_RAILWAY = 4,
  META_CAT_SUBWAY  = 5
};

// Flag bits
#define HWMAP_FLAG_HAS_METADATA 0x0001

// Maximum metadata entries to parse (memory limit)
#define MAX_METADATA_ENTRIES 256

// Single metadata entry (parsed from file)
struct MapMetadataEntry {
  uint8_t category;
  char name[64];  // Truncated if longer
};

// Loaded map state
struct LoadedMap {
  bool valid;
  HWMapHeader header;
  uint8_t* data;           // Full file data in PSRAM
  size_t dataSize;
  char filename[32];
  
  // Metadata (if present)
  bool hasMetadata;
  MapMetadataEntry* metadata;
  uint16_t metadataCount;
};

// =============================================================================
// Abstract Map Renderer Interface
// =============================================================================

class MapRenderer {
public:
  virtual ~MapRenderer() {}
  
  // Set the viewport dimensions
  virtual void setViewport(int width, int height) = 0;
  
  // Clear the display/canvas
  virtual void clear() = 0;
  
  // Draw a line segment with feature styling
  virtual void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, 
                        const MapFeatureStyle& style) = 0;
  
  // Draw GPS position marker at center
  virtual void drawPositionMarker(int16_t x, int16_t y) = 0;
  
  // Draw text overlay (region name, satellite count, etc.)
  virtual void drawOverlayText(int16_t x, int16_t y, const char* text, bool inverted = false) = 0;
  
  // Commit rendering (for double-buffered displays)
  virtual void flush() = 0;
  
  // Get viewport dimensions
  int getWidth() const { return _width; }
  int getHeight() const { return _height; }
  
  // Get feature style for a given type (can be overridden per-renderer)
  virtual MapFeatureStyle getFeatureStyle(MapFeatureType type);
  
protected:
  int _width = 128;
  int _height = 64;
};

// =============================================================================
// Map Core - Display-Agnostic Map Processing
// =============================================================================

class MapCore {
public:
  // Load a map file from filesystem
  static bool loadMapFile(const char* path);
  
  // Unload current map
  static void unloadMap();
  
  // Check if position is within loaded map bounds
  static bool isPositionInMap(float lat, float lon);
  
  // Auto-select map based on GPS position
  static bool autoSelectMap(float lat, float lon);
  
  // Get list of available maps
  static int getAvailableMaps(char maps[][32], int maxMaps);
  
  // Render map to a renderer at given center position
  static void renderMap(MapRenderer* renderer, float centerLat, float centerLon);
  
  // Get current map info
  static const LoadedMap& getCurrentMap() { return _currentMap; }
  static bool hasValidMap() { return _currentMap.valid; }
  
  // Get metadata (returns count, 0 if none)
  static int getMetadataCount() { return _currentMap.hasMetadata ? _currentMap.metadataCount : 0; }
  static const MapMetadataEntry* getMetadata(int index);
  static int getMetadataByCategory(uint8_t category, const MapMetadataEntry** entries, int maxEntries);
  
  // Convert geo coordinates to screen coordinates (public for waypoint rendering)
  static void geoToScreen(int32_t lat, int32_t lon,
                          int32_t centerLat, int32_t centerLon,
                          int32_t scaleX, int32_t scaleY,
                          int viewWidth, int viewHeight,
                          int16_t& screenX, int16_t& screenY);
  
private:
  static LoadedMap _currentMap;
};

// =============================================================================
// Concrete Renderer Implementations
// =============================================================================

#if ENABLE_OLED_DISPLAY
class Adafruit_SSD1306;

class OLEDMapRenderer : public MapRenderer {
public:
  OLEDMapRenderer(Adafruit_SSD1306* display);
  
  void setViewport(int width, int height) override;
  void clear() override;
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                const MapFeatureStyle& style) override;
  void drawPositionMarker(int16_t x, int16_t y) override;
  void drawOverlayText(int16_t x, int16_t y, const char* text, bool inverted) override;
  void flush() override;
  
  // OLED-specific: wireframe-optimized styles
  MapFeatureStyle getFeatureStyle(MapFeatureType type) override;
  
private:
  Adafruit_SSD1306* _display;
  
  // Draw dashed line for OLED
  void drawDashedLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int dashLen);
  void drawDottedLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int spacing);
};
#endif

// =============================================================================
// Waypoint System
// =============================================================================

#define MAX_WAYPOINTS 16
#define WAYPOINT_NAME_LEN 12

struct Waypoint {
  float lat;
  float lon;
  char name[WAYPOINT_NAME_LEN];
  bool active;  // false = empty slot
};

// Waypoint management
class WaypointManager {
public:
  // Load/save waypoints from/to JSON file
  static bool loadWaypoints();
  static bool saveWaypoints();
  
  // Add waypoint at position (uses first empty slot)
  static int addWaypoint(float lat, float lon, const char* name);
  
  // Delete waypoint by index
  static bool deleteWaypoint(int index);
  
  // Get waypoint by index
  static const Waypoint* getWaypoint(int index);
  
  // Get count of active waypoints
  static int getActiveCount();
  
  // Select waypoint as navigation target (-1 = none)
  static void selectTarget(int index);
  static int getSelectedTarget() { return _selectedTarget; }
  
  // Calculate distance (meters) and bearing (degrees) from position to target
  static bool getDistanceBearing(float fromLat, float fromLon, 
                                  float& distanceM, float& bearingDeg);
  
  // Render waypoints on map
  static void renderWaypoints(MapRenderer* renderer, 
                               float centerLat, float centerLon,
                               int32_t scaleX, int32_t scaleY);
  
private:
  static Waypoint _waypoints[MAX_WAYPOINTS];
  static int _selectedTarget;  // -1 = none
};

// =============================================================================
// Global State & Functions
// =============================================================================

extern bool gMapRendererEnabled;
extern float gMapRotation;  // Rotation angle in degrees (0-360)

// Initialize map renderer system
void initMapRenderer();

// Command handlers
const char* cmd_map(const String& cmd);
const char* cmd_mapload(const String& cmd);
const char* cmd_maplist(const String& cmd);
const char* cmd_waypoint(const String& cmd);

// Command registry
struct CommandEntry;
extern const CommandEntry mapCommands[];
extern const size_t mapCommandsCount;

#endif // SYSTEM_GPS_MAP_RENDERER_H
