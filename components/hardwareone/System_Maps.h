#ifndef SYSTEM_MAPS_H
#define SYSTEM_MAPS_H

#include <Arduino.h>

#include "System_BuildConfig.h"

// =============================================================================
// HardwareOne Map (.hwmap) File Format - VERSION 5 (TILED + QUANTIZED)
// =============================================================================
// Binary format for compact offline maps on ESP32
//
// Header (40 bytes):
//   Magic: "HWMP" (4 bytes)
//   Version: uint16 = 5 (2 bytes)
//   Flags: uint16 (encodes tiling params):
//     bits 0-3:  reserved
//     bits 4-7:  quantBits (precision, typically 0 = full 16-bit)
//     bits 8-11: tileGridSize (e.g., 4 = 4x4 = 16 tiles)
//     bits 12-15: haloPct * 100 (e.g., 10 = 0.10 = 10% halo)
//   Bounds: minLat, minLon, maxLat, maxLon (4x int32, microdegrees)
//   FeatureCount: uint32 (total features across all tiles)
//   NameCount: uint16 (2 bytes)
//   RegionName: 8 bytes (null-padded)
//   Padding: 2 bytes
//
// Name Table (variable, immediately after header):
//   For each name (NameCount times):
//     Length: uint8 (max 63)
//     String: char[Length] (not null-terminated)
//
// Tile Directory (6 bytes per tile, tileGridSize * tileGridSize tiles):
//   Offset: uint32 (file offset to tile's feature data)
//   FeatureCount: uint16 (number of features in this tile)
//
// Tile Feature Data (per tile, at offset specified in directory):
//   For each feature:
//     Type: uint8
//     NameIndex: uint16 (0xFFFF = unnamed)
//     PointCount: uint16
//     Points: PointCount * (qLat: uint16, qLon: uint16)
//       qLat/qLon are quantized 0-65535 relative to tile halo bounds

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
  MAP_FEATURE_LAND_MASK  = 0x12,
  MAP_FEATURE_RAILWAY    = 0x20,
  MAP_FEATURE_BUS        = 0x21,
  MAP_FEATURE_FERRY      = 0x22,
  MAP_FEATURE_BUILDING   = 0x30,
  MAP_FEATURE_STATION    = 0x40
};

// =============================================================================
// Feature Subtypes (v6 format - must match web tool FEATURE_SUBTYPES)
// =============================================================================

// Highway subtypes (type 0x00)
#define SUBTYPE_HIGHWAY_MOTORWAY  0
#define SUBTYPE_HIGHWAY_TRUNK     1

// Major road subtypes (type 0x01)
#define SUBTYPE_MAJOR_PRIMARY     0
#define SUBTYPE_MAJOR_SECONDARY   1

// Minor road subtypes (type 0x02)
#define SUBTYPE_MINOR_TERTIARY    0
#define SUBTYPE_MINOR_RESIDENTIAL 1
#define SUBTYPE_MINOR_SERVICE     2

// Path subtypes (type 0x03)
#define SUBTYPE_PATH_FOOTWAY      0
#define SUBTYPE_PATH_CYCLEWAY     1
#define SUBTYPE_PATH_TRACK        2

// Water subtypes (type 0x10)
#define SUBTYPE_WATER_LAKE        0
#define SUBTYPE_WATER_RIVER       1
#define SUBTYPE_WATER_COASTLINE   2

// Park/nature subtypes (type 0x11)
#define SUBTYPE_PARK_PARK         0
#define SUBTYPE_PARK_FOREST       1
#define SUBTYPE_PARK_GRASSLAND    2

// Railway subtypes (type 0x20)
#define SUBTYPE_RAILWAY_RAIL      0
#define SUBTYPE_RAILWAY_SUBWAY    1

// Building subtypes (type 0x30)
#define SUBTYPE_BUILDING_BUILDING   0
#define SUBTYPE_BUILDING_INDUSTRIAL 1
#define SUBTYPE_BUILDING_COMMERCIAL 2
#define SUBTYPE_BUILDING_RESIDENTIAL 3

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
// Map Data Structures (v5/v6)
// =============================================================================

// Map header structure (40 bytes)
struct HWMapHeader {
  char magic[4];           // "HWMP"
  uint16_t version;        // 5 or 6
  uint16_t flags;          // bit 0: has spatial index
  int32_t minLat;          // Microdegrees (lat * 1000000)
  int32_t minLon;
  int32_t maxLat;
  int32_t maxLon;
  uint32_t featureCount;
  uint16_t nameCount;      // Number of entries in name table
  char regionName[8];
  uint16_t padding;        // Align to 40 bytes
} __attribute__((packed));

// Macros to extract v5 tiling params from flags (matches generator encoding)
// bits 0-1: tileGridCode (0=16, 1=32, 2=64), bits 2-6: haloPct (0-31), bits 7-10: quantBits-10
#define HWMAP_GET_TILE_GRID_CODE(flags) ((flags) & 0x03)
#define HWMAP_GET_TILE_GRID_SIZE(flags) (HWMAP_GET_TILE_GRID_CODE(flags) == 0 ? 16 : HWMAP_GET_TILE_GRID_CODE(flags) == 2 ? 64 : 32)
#define HWMAP_GET_HALO_PCT(flags)      ((((flags) >> 2) & 0x1F) / 100.0f)
#define HWMAP_GET_QUANT_BITS(flags)    ((((flags) >> 7) & 0x0F) + 10)

// Tile directory entry (8 bytes per tile)
// Note: featureCount is stored at the START of each tile's payload, not here
struct HWMapTileDirEntry {
  uint32_t offset;         // File offset to tile's payload data
  uint32_t payloadSize;    // Size of tile's payload in bytes (0 = empty tile)
} __attribute__((packed));

// Feature header for v5 format (5 bytes, no points - points follow as quantized pairs)
struct HWMapFeatureHeader {
  uint8_t type;
  uint16_t nameIndex;      // Index into name table, 0xFFFF = unnamed
  uint16_t pointCount;     // Number of quantized points following
} __attribute__((packed));

// Feature header for v6 format (6 bytes - adds subtype byte)
struct HWMapFeatureHeaderV6 {
  uint8_t type;
  uint8_t subtype;         // Subtype for granular classification
  uint16_t nameIndex;      // Index into name table, 0xFFFF = unnamed
  uint16_t pointCount;     // Number of quantized points following
} __attribute__((packed));

// Feature header size based on version
#define HWMAP_FEATURE_HEADER_SIZE(version) ((version) >= 6 ? 6 : 5)

// Marker for unnamed features
#define HWMAP_NO_NAME 0xFFFF

// Maximum names to load (memory limit)
#define MAX_MAP_NAMES 512

// Maximum tiles (16x16 = 256 max from 4-bit grid size)
#define HWMAP_MAX_TILES 256

// =============================================================================
// Map Feature Highlighting System (generic, can highlight any feature)
// =============================================================================

// Highlight mode types
enum MapHighlightMode : uint8_t {
  HIGHLIGHT_NONE = 0,        // No highlighting active
  HIGHLIGHT_BY_NAME,         // Match features by name (exact or prefix)
  HIGHLIGHT_BY_TYPE,         // Match all features of a type
  HIGHLIGHT_BY_NAME_AND_TYPE // Match name AND type
};

// Highlight configuration
struct MapHighlight {
  MapHighlightMode mode;
  char name[64];             // Name to match (if mode includes name)
  uint8_t featureType;       // Type to match (if mode includes type)
  bool prefixMatch;          // If true, match name prefix instead of exact
  uint32_t blinkIntervalMs;  // Blink interval in ms (0 = solid highlight)
  uint32_t startTime;        // When highlight started (for blink timing)
  bool active;               // Is highlight currently active
};

// Global highlight state (extern, defined in cpp)
extern MapHighlight gMapHighlight;

// Highlight API
void mapHighlightClear();
void mapHighlightByName(const char* name, bool prefixMatch = false, uint32_t blinkMs = 300);
void mapHighlightByType(uint8_t featureType, uint32_t blinkMs = 300);
void mapHighlightByNameAndType(const char* name, uint8_t featureType, uint32_t blinkMs = 300);
bool mapHighlightMatches(uint16_t nameIndex, uint8_t featureType);
bool mapHighlightIsVisible();  // Returns false during "off" phase of blink

// =============================================================================
// Layer Visibility API (for toggling feature types on OLED/Web)
// =============================================================================

// Layer flags for bulk operations
#define LAYER_HIGHWAYS   (1 << 0)
#define LAYER_MAJOR      (1 << 1)
#define LAYER_MINOR      (1 << 2)
#define LAYER_PATHS      (1 << 3)
#define LAYER_WATER      (1 << 4)
#define LAYER_PARKS      (1 << 5)
#define LAYER_RAILWAYS   (1 << 6)
#define LAYER_TRANSIT    (1 << 7)
#define LAYER_BUILDINGS  (1 << 8)
#define LAYER_LAND_MASK  (1 << 9)
#define LAYER_ALL        0x3FF

// Get/set layer visibility
uint16_t mapLayersGetVisible();
void mapLayersSetVisible(uint16_t layers);
void mapLayerToggle(uint16_t layer);
bool mapLayerIsVisible(uint8_t featureType);  // Check if a feature type should render

// Name entry (parsed from name table)
struct MapNameEntry {
  char name[64];  // Truncated if longer than 63
};

// Streaming cache size (1MB in PSRAM for feature data)
#define MAP_CACHE_SIZE (1024 * 1024)

// Loaded map state - v5 tiled architecture
struct LoadedMap {
  bool valid;
  HWMapHeader header;
  char filename[32];
  char filepath[128];       // Full path for reopening file
  size_t fileSize;         // Total file size
  
  // Name table (always in RAM - typically small)
  MapNameEntry* names;     // Array of names
  uint16_t nameCount;      // Number of names loaded
  
  // V5 tiling parameters (extracted from header.flags)
  uint8_t tileGridSize;    // e.g., 4 = 4x4 = 16 tiles
  float haloPct;           // Halo fraction (e.g., 0.10)
  uint8_t quantBits;       // Quantization bits (usually 0 = full 16-bit)
  
  // Tile directory (always in RAM - small: 6 bytes per tile)
  HWMapTileDirEntry* tileDir;  // Array of tile offsets/counts
  uint16_t tileCount;      // tileGridSize * tileGridSize
  
  // Precomputed tile geometry (for fast dequantization)
  int32_t tileW;           // Tile width in microdegrees
  int32_t tileH;           // Tile height in microdegrees
  int32_t haloW;           // Halo width in microdegrees
  int32_t haloH;           // Halo height in microdegrees
  
  // Streaming cache (1MB buffer in PSRAM)
  uint8_t* cache;          // 1MB cache buffer
  size_t cacheStart;       // File offset where cache starts
  size_t cacheLen;         // Bytes currently in cache
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
  
  // Draw scrolling context bar at top (for location context)
  virtual void drawContextBar(const char* text, int scrollOffset = 0) = 0;
  
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
  static int getAvailableMaps(char maps[][96], int maxMaps);
  
  // Render map to a renderer at given center position
  static void renderMap(MapRenderer* renderer, float centerLat, float centerLon);
  
  // Get current map info
  static const LoadedMap& getCurrentMap() { return _currentMap; }
  static bool hasValidMap() { return _currentMap.valid; }
  
  // Name table access (v2)
  static int getNameCount() { return _currentMap.valid ? _currentMap.nameCount : 0; }
  static const char* getName(uint16_t index);
  
  // Get all names of a specific feature type (for search/browsing)
  static int getNamesByFeatureType(MapFeatureType type, const char** names, int maxNames);
  
  // Search names by prefix (case-insensitive) - for autocomplete
  static int searchNamesByPrefix(const char* prefix, const char** results, int maxResults);
  
  // Convert geo coordinates to screen coordinates (public for waypoint rendering)
  static void geoToScreen(int32_t lat, int32_t lon,
                          int32_t centerLat, int32_t centerLon,
                          int32_t scaleX, int32_t scaleY,
                          int viewWidth, int viewHeight,
                          int16_t& screenX, int16_t& screenY);
  
  // Streaming helpers - load tile data from cache or file
  static const uint8_t* loadTileData(uint16_t tileIdx, size_t* outSize = nullptr);
  
private:
  static LoadedMap _currentMap;
};

// =============================================================================
// Location Context System
// =============================================================================

// How often to update context (not every GPS update)
#define CONTEXT_UPDATE_INTERVAL_MS  5000   // 5 seconds
#define CONTEXT_UPDATE_MIN_DISTANCE 10.0f  // meters moved

// Location context result
struct LocationContext {
  char nearestRoad[64];      // Name of nearest road/street
  float roadDistanceM;       // Distance to nearest road in meters
  MapFeatureType roadType;   // Type of nearest road
  
  char nearestArea[64];      // Name of nearest area (park, water)
  float areaDistanceM;       // Distance to nearest area
  MapFeatureType areaType;   // Type of nearest area
  
  uint32_t lastUpdateMs;     // When context was last computed
  float lastLat, lastLon;    // Position when context was computed
  bool valid;                // Whether context is valid
};

// Location context manager
class LocationContextManager {
public:
  // Check if context needs updating based on time/distance
  static bool shouldUpdate(float lat, float lon);
  
  // Update context for current position (call only if shouldUpdate returns true)
  static void updateContext(float lat, float lon);
  
  // Get current context (may be stale - check lastUpdateMs)
  static const LocationContext& getContext() { return _context; }
  
  // Force context update on next check
  static void invalidate() { _context.valid = false; }
  
private:
  static LocationContext _context;
  
  // Calculate distance from point to line segment (returns meters)
  static float pointToSegmentDistance(float lat, float lon,
                                      int32_t lat1, int32_t lon1,
                                      int32_t lat2, int32_t lon2);
  
  // Haversine distance between two points (meters)
  static float haversineDistance(float lat1, float lon1, float lat2, float lon2);
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
  void drawContextBar(const char* text, int scrollOffset) override;
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
// GPS Track System
// =============================================================================

#define MAX_TRACK_POINTS 500  // Memory limit for track points

struct GPSTrackPoint {
  float lat;
  float lon;
  unsigned long timestamp;  // milliseconds since boot (if available)
};

struct GPSTrackBounds {
  float minLat;
  float minLon;
  float maxLat;
  float maxLon;
  bool valid;
};

struct GPSTrackStats {
  float totalDistanceM;    // Total distance in meters (point-to-point)
  float durationSec;       // Duration from first to last point
  float avgSpeedMps;       // Average speed in m/s
  bool valid;
};

// GPS Track validation result
enum TrackValidation {
  TRACK_VALID,           // >90% points in map bounds
  TRACK_PARTIAL,         // 50-90% points in map bounds
  TRACK_OUT_OF_BOUNDS,   // <50% points in map bounds
  TRACK_NO_MAP_LOADED,   // No map to validate against
  TRACK_EMPTY            // No points in track
};

// GPS Track management
class GPSTrackManager {
public:
  // Load track from sensor log file
  static bool loadTrack(const char* filepath, String& errorMsg);
  
  // Clear current track
  static void clearTrack();
  
  // Check if track is loaded
  static bool hasTrack() { return _pointCount > 0; }
  
  // Get track info
  static int getPointCount() { return _pointCount; }
  static const GPSTrackPoint* getPoints() { return _points; }
  static const GPSTrackBounds& getBounds() { return _bounds; }
  static const GPSTrackStats& getStats() { return _stats; }
  static const char* getFilename() { return _filename; }
  
  // Validate track against current map bounds
  static TrackValidation validateTrack(float& coveragePercent);
  
  // Delete a track file
  static bool deleteTrackFile(const char* filepath);
  
  // Live tracking mode - append points in real-time
  static bool isLiveTracking() { return _liveTracking; }
  static void setLiveTracking(bool enabled);
  static bool appendPoint(float lat, float lon);  // Returns true if point was added
  static uint32_t getLastUpdateTime() { return _lastUpdateMs; }
  
  // Render track on map
  static void renderTrack(MapRenderer* renderer, 
                          float centerLat, float centerLon,
                          int32_t scaleX, int32_t scaleY);
  
  // Get validation message
  static const char* getValidationMessage(TrackValidation result, float coverage);
  
private:
  static GPSTrackPoint* _points;
  static int _pointCount;
  static GPSTrackBounds _bounds;
  static GPSTrackStats _stats;
  static char _filename[64];
  static bool _liveTracking;
  static uint32_t _lastUpdateMs;
  
  // Calculate bounds from loaded points
  static void calculateBounds();
  
  // Calculate statistics (distance, duration, speed)
  static void calculateStats();
  
  // Haversine distance between two points (meters)
  static float haversineDistance(float lat1, float lon1, float lat2, float lon2);
  
  // Parse GPS coordinates from log line
  static bool parseGPSLine(const char* line, float& lat, float& lon);
};

// =============================================================================
// Waypoint System
// =============================================================================

#define MAX_WAYPOINTS 16
#define WAYPOINT_NAME_LEN 12
#define WAYPOINT_NOTES_LEN 256
#define MAX_WAYPOINT_FILES 4
#define WAYPOINT_FILE_PATH_LEN 64

struct Waypoint {
  float lat;
  float lon;
  char name[WAYPOINT_NAME_LEN];
  char notes[WAYPOINT_NOTES_LEN];
  bool active;  // false = empty slot
  char files[MAX_WAYPOINT_FILES][WAYPOINT_FILE_PATH_LEN];  // Attached file paths
  uint8_t fileCount;
};

// Waypoint management
class WaypointManager {
public:
  // Load/save waypoints from/to JSON file
  static bool loadWaypoints();
  static bool saveWaypoints();
  
  // Add waypoint at position (uses first empty slot)
  static int addWaypoint(float lat, float lon, const char* name);
  static int addWaypoint(float lat, float lon, const char* name, const char* notes);

  // Set notes for an existing waypoint
  static bool setNotes(int index, const char* notes);

  // Set name for an existing waypoint
  static bool setName(int index, const char* name);

  // File attachment management
  static bool addFile(int waypointIndex, const char* filePath);
  static bool removeFile(int waypointIndex, int fileIndex);
  static int getFileCount(int waypointIndex);
  static const char* getFile(int waypointIndex, int fileIndex);
  static int findWaypointByName(const char* name);

  // Clear all waypoints
  static bool clearAll();
  
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
const char* cmd_whereami(const String& cmd);
const char* cmd_search(const String& cmd);

// Command registry
struct CommandEntry;
extern const CommandEntry mapCommands[];
extern const size_t mapCommandsCount;

#endif // SYSTEM_MAPS_H
