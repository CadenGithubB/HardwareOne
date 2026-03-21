#include <LittleFS.h>
#include <ArduinoJson.h>
#include <cstring>

#include "System_Maps.h"
#include "System_BuildConfig.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_Utils.h"

#if ENABLE_OLED_DISPLAY
#include <Adafruit_SSD1306.h>
#include "OLED_Display.h"
#include "System_FileManager.h"
#endif

#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor-seesaw.h"
#endif

#if ENABLE_GPS_SENSOR
#include <Adafruit_GPS.h>
#include "i2csensor-pa1010d.h"
#include "System_I2C_Manager.h"
#endif

// =============================================================================
// MapCore Static Member (works without GPS)
// =============================================================================

LoadedMap MapCore::_currentMap = {};
LocationContext LocationContextManager::_context = {"", 0, MAP_FEATURE_HIGHWAY, "", 0, MAP_FEATURE_PARK, 0, 0, 0, false};

float gMapRotation = 0.0f;  // Rotation angle in degrees

// Center position for map viewing without GPS
float gMapCenterLat = 0.0f;
float gMapCenterLon = 0.0f;
bool gMapCenterSet = false;  // Non-static for external access from file browser
bool gMapManuallyPanned = false;  // Track if user has manually moved the map

// Momentum-based scrolling for smoother panning
float gMapVelocityLat = 0.0f;
float gMapVelocityLon = 0.0f;
float gMapRotationVelocity = 0.0f;  // For smooth rotation
unsigned long gMapLastMomentumUpdate = 0;

// Zoom level (1.0 = default, higher = zoomed in)
float gMapZoom = 1.0f;

// =============================================================================
// Map Feature Highlighting System
// =============================================================================

MapHighlight gMapHighlight = {HIGHLIGHT_NONE, "", 0, false, 300, 0, false};

void mapHighlightClear() {
  gMapHighlight.mode = HIGHLIGHT_NONE;
  gMapHighlight.name[0] = '\0';
  gMapHighlight.active = false;
}

void mapHighlightByName(const char* name, bool prefixMatch, uint32_t blinkMs) {
  gMapHighlight.mode = HIGHLIGHT_BY_NAME;
  strncpy(gMapHighlight.name, name, sizeof(gMapHighlight.name) - 1);
  gMapHighlight.name[sizeof(gMapHighlight.name) - 1] = '\0';
  gMapHighlight.prefixMatch = prefixMatch;
  gMapHighlight.blinkIntervalMs = blinkMs;
  gMapHighlight.startTime = millis();
  gMapHighlight.active = true;
}

void mapHighlightByType(uint8_t featureType, uint32_t blinkMs) {
  gMapHighlight.mode = HIGHLIGHT_BY_TYPE;
  gMapHighlight.featureType = featureType;
  gMapHighlight.blinkIntervalMs = blinkMs;
  gMapHighlight.startTime = millis();
  gMapHighlight.active = true;
}

void mapHighlightByNameAndType(const char* name, uint8_t featureType, uint32_t blinkMs) {
  gMapHighlight.mode = HIGHLIGHT_BY_NAME_AND_TYPE;
  strncpy(gMapHighlight.name, name, sizeof(gMapHighlight.name) - 1);
  gMapHighlight.name[sizeof(gMapHighlight.name) - 1] = '\0';
  gMapHighlight.featureType = featureType;
  gMapHighlight.prefixMatch = false;
  gMapHighlight.blinkIntervalMs = blinkMs;
  gMapHighlight.startTime = millis();
  gMapHighlight.active = true;
}

bool mapHighlightMatches(uint16_t nameIndex, uint8_t featureType) {
  if (!gMapHighlight.active || gMapHighlight.mode == HIGHLIGHT_NONE) return false;
  
  bool typeMatches = (gMapHighlight.featureType == featureType);
  bool nameMatches = false;
  
  if (gMapHighlight.mode == HIGHLIGHT_BY_TYPE) {
    return typeMatches;
  }
  
  // Check name match
  if (nameIndex != HWMAP_NO_NAME) {
    const char* featureName = MapCore::getName(nameIndex);
    if (featureName) {
      if (gMapHighlight.prefixMatch) {
        nameMatches = (strncmp(featureName, gMapHighlight.name, strlen(gMapHighlight.name)) == 0);
      } else {
        nameMatches = (strcmp(featureName, gMapHighlight.name) == 0);
      }
    }
  }
  
  if (gMapHighlight.mode == HIGHLIGHT_BY_NAME) {
    return nameMatches;
  }
  
  // HIGHLIGHT_BY_NAME_AND_TYPE
  return nameMatches && typeMatches;
}

bool mapHighlightIsVisible() {
  if (!gMapHighlight.active) return false;
  if (gMapHighlight.blinkIntervalMs == 0) return true;  // Solid highlight
  
  // Blink: alternate on/off based on time
  uint32_t elapsed = millis() - gMapHighlight.startTime;
  return ((elapsed / gMapHighlight.blinkIntervalMs) % 2) == 0;
}

// =============================================================================
// Layer Visibility System
// =============================================================================

static uint16_t gVisibleLayers = LAYER_ALL;  // All layers visible by default

// Per-subtype visibility bitmask: bit N = subtype N is visible.
// Indexed directly by MapFeatureType value. Max value is 0x40=64, so 65 bytes.
// All bits 1 = all subtypes visible (default).
static uint8_t gSubtypeVisibility[65];
static bool gSubtypeVisibilityInited = false;

static void ensureSubtypeVisibilityInited() {
  if (!gSubtypeVisibilityInited) {
    memset(gSubtypeVisibility, 0xFF, sizeof(gSubtypeVisibility));
    gSubtypeVisibilityInited = true;
  }
}

uint16_t mapLayersGetVisible() {
  return gVisibleLayers;
}

void mapLayersSetVisible(uint16_t layers) {
  gVisibleLayers = layers;
}

void mapLayerToggle(uint16_t layer) {
  gVisibleLayers ^= layer;
}

bool mapSubtypeIsVisible(uint8_t featureType, uint8_t subtype) {
  ensureSubtypeVisibilityInited();
  if (featureType > 64 || subtype > 7) return true;
  return (gSubtypeVisibility[featureType] >> subtype) & 1;
}

void mapSubtypeToggle(uint8_t featureType, uint8_t subtype) {
  ensureSubtypeVisibilityInited();
  if (featureType <= 64 && subtype <= 7) gSubtypeVisibility[featureType] ^= (1 << subtype);
}

uint8_t mapSubtypeGetMask(uint8_t featureType) {
  ensureSubtypeVisibilityInited();
  if (featureType > 64) return 0xFF;
  return gSubtypeVisibility[featureType];
}

void mapSubtypeSetMask(uint8_t featureType, uint8_t mask) {
  ensureSubtypeVisibilityInited();
  if (featureType <= 64) gSubtypeVisibility[featureType] = mask;
}

bool mapLayerIsVisible(uint8_t featureType) {
  switch (featureType) {
    case MAP_FEATURE_HIGHWAY:  return (gVisibleLayers & LAYER_HIGHWAYS) != 0;
    case MAP_FEATURE_ROAD_MAJOR: return (gVisibleLayers & LAYER_MAJOR) != 0;
    case MAP_FEATURE_ROAD_MINOR: return (gVisibleLayers & LAYER_MINOR) != 0;
    case MAP_FEATURE_PATH:     return (gVisibleLayers & LAYER_PATHS) != 0;
    case MAP_FEATURE_WATER:    return (gVisibleLayers & LAYER_WATER) != 0;
    case MAP_FEATURE_PARK:     return (gVisibleLayers & LAYER_PARKS) != 0;
    case MAP_FEATURE_LAND_MASK: return (gVisibleLayers & LAYER_LAND_MASK) != 0;
    case MAP_FEATURE_RAILWAY:  return (gVisibleLayers & LAYER_RAILWAYS) != 0;
    case MAP_FEATURE_BUS:      return (gVisibleLayers & LAYER_TRANSIT) != 0;
    case MAP_FEATURE_FERRY:    return (gVisibleLayers & LAYER_TRANSIT) != 0;
    case MAP_FEATURE_BUILDING: return (gVisibleLayers & LAYER_BUILDINGS) != 0;
    case MAP_FEATURE_STATION:  return (gVisibleLayers & LAYER_TRANSIT) != 0;
    default: return true;
  }
}

// Build a MapRenderParams snapshot from current global state
MapRenderParams mapRenderParamsFromGlobals() {
  ensureSubtypeVisibilityInited();
  MapRenderParams p;
  p.zoom = gMapZoom;
  p.rotation = gMapRotation;
  p.visibleLayers = gVisibleLayers;
  memcpy(p.subtypeVisibility, gSubtypeVisibility, sizeof(p.subtypeVisibility));
  return p;
}

// Static helpers: check visibility against a MapRenderParams (no global reads)
static bool paramLayerIsVisible(const MapRenderParams& p, uint8_t featureType) {
  switch (featureType) {
    case MAP_FEATURE_HIGHWAY:    return (p.visibleLayers & LAYER_HIGHWAYS) != 0;
    case MAP_FEATURE_ROAD_MAJOR: return (p.visibleLayers & LAYER_MAJOR) != 0;
    case MAP_FEATURE_ROAD_MINOR: return (p.visibleLayers & LAYER_MINOR) != 0;
    case MAP_FEATURE_PATH:       return (p.visibleLayers & LAYER_PATHS) != 0;
    case MAP_FEATURE_WATER:      return (p.visibleLayers & LAYER_WATER) != 0;
    case MAP_FEATURE_PARK:       return (p.visibleLayers & LAYER_PARKS) != 0;
    case MAP_FEATURE_LAND_MASK:  return (p.visibleLayers & LAYER_LAND_MASK) != 0;
    case MAP_FEATURE_RAILWAY:    return (p.visibleLayers & LAYER_RAILWAYS) != 0;
    case MAP_FEATURE_BUS:        return (p.visibleLayers & LAYER_TRANSIT) != 0;
    case MAP_FEATURE_FERRY:      return (p.visibleLayers & LAYER_TRANSIT) != 0;
    case MAP_FEATURE_BUILDING:   return (p.visibleLayers & LAYER_BUILDINGS) != 0;
    case MAP_FEATURE_STATION:    return (p.visibleLayers & LAYER_TRANSIT) != 0;
    default: return true;
  }
}

static bool paramSubtypeIsVisible(const MapRenderParams& p, uint8_t featureType, uint8_t subtype) {
  if (featureType > 64 || subtype > 7) return true;
  return (p.subtypeVisibility[featureType] >> subtype) & 1;
}

// =============================================================================
// MapRenderer Base Class - Default Feature Styles
// =============================================================================

MapFeatureStyle MapRenderer::getFeatureStyle(MapFeatureType type) {
  // Default styles (can be overridden by subclasses)
  switch (type) {
    case MAP_FEATURE_HIGHWAY:
      return {LINE_SOLID, 3, 10, true, 0xFFFF};  // White, thicker (was 2)
    case MAP_FEATURE_ROAD_MAJOR:
      return {LINE_SOLID, 2, 9, true, 0xFFFF};   // White, medium (was 1)
    case MAP_FEATURE_ROAD_MINOR:
      return {LINE_DASHED, 1, 5, true, 0xC618};  // Gray, thin, dashed
    case MAP_FEATURE_PATH:
      return {LINE_DOTTED, 1, 3, true, 0x8410};  // Dark gray, dotted
    case MAP_FEATURE_WATER:
      return {LINE_SOLID, 1, 8, true, 0x001F};   // Blue
    case MAP_FEATURE_PARK:
      return {LINE_DOTTED, 1, 2, false, 0x07E0}; // Green, skip on mono
    case MAP_FEATURE_LAND_MASK:
      return {LINE_DOTTED, 1, 1, true, 0x8410};  // Coastline, thin dotted, lowest priority
    case MAP_FEATURE_RAILWAY:
      return {LINE_DASHED, 1, 7, true, 0x7BEF};  // Gray, dashed
    case MAP_FEATURE_BUS:
      return {LINE_DASHED, 1, 4, true, 0xFD20};  // Orange, dashed
    case MAP_FEATURE_FERRY:
      return {LINE_DASHED, 2, 6, true, 0x07FF};  // Cyan, dashed, thicker
    case MAP_FEATURE_BUILDING:
      return {LINE_NONE, 1, 1, false, 0x4208};   // Skip
    case MAP_FEATURE_STATION:
      return {LINE_SOLID, 1, 7, true, 0xF81F};   // Magenta, point marker
    default:
      return {LINE_SOLID, 1, 5, true, 0xFFFF};
  }
}

// =============================================================================
// MapCore - Map File Loading (Display-Agnostic)
// =============================================================================

bool MapCore::loadMapFile(const char* path) {
  // Unload any existing map
  unloadMap();
  
  // Pause sensor polling during file I/O to prevent I2C contention
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;
  vTaskDelay(pdMS_TO_TICKS(50));  // Let any in-flight I2C complete

  FsLockGuard fsGuard("MapCore.loadMapFile");
  
  if (!LittleFS.exists(path)) {
    WARN_SENSORSF("Map file not found: %s", path);
    gSensorPollingPaused = wasPaused;
    return false;
  }
  
  File f = LittleFS.open(path, "r");
  if (!f) {
    ERROR_SENSORSF("Failed to open map file: %s", path);
    gSensorPollingPaused = wasPaused;
    return false;
  }
  
  size_t fileSize = f.size();
  if (fileSize < sizeof(HWMapHeader)) {
    ERROR_SENSORSF("Map file too small: %zu bytes", fileSize);
    f.close();
    gSensorPollingPaused = wasPaused;
    return false;
  }
  
  // Read header first
  HWMapHeader header;
  if (f.read((uint8_t*)&header, sizeof(header)) != sizeof(header)) {
    ERROR_SENSORSF("Failed to read map header");
    f.close();
    gSensorPollingPaused = wasPaused;
    return false;
  }
  
  // Validate magic
  if (memcmp(header.magic, "HWMP", 4) != 0) {
    ERROR_SENSORSF("Invalid map magic: %.4s", header.magic);
    f.close();
    gSensorPollingPaused = wasPaused;
    return false;
  }
  
  // Validate version (v6 only)
  if (header.version != 6) {
    ERROR_SENSORSF("Unsupported map version: %u (need v6)", header.version);
    f.close();
    gSensorPollingPaused = wasPaused;
    return false;
  }
  
  // Store basic info (cache allocated after tile directory is parsed)
  _currentMap.valid = true;
  memcpy(&_currentMap.header, &header, sizeof(header));
  
  // Normalize bounds: map tool writes lat/lon * 10^7 (deci-microdegrees)
  // but all device code expects lat/lon * 10^6 (microdegrees)
  _currentMap.header.minLat /= 10;
  _currentMap.header.minLon /= 10;
  _currentMap.header.maxLat /= 10;
  _currentMap.header.maxLon /= 10;
  
  _currentMap.fileSize = fileSize;
  _currentMap.cachePool = nullptr;
  _currentMap.cachePoolSize = 0;
  _currentMap.slotSize = 0;
  _currentMap.numSlots = 0;
  _currentMap.accessSeq = 0;
  _currentMap.slots = nullptr;
  _currentMap.cacheHits = 0;
  _currentMap.cacheMisses = 0;
  _currentMap.names = nullptr;
  _currentMap.nameCount = 0;
  _currentMap.tileDir = nullptr;
  _currentMap.tileCount = 0;
  
  // Extract tiling parameters from flags
  _currentMap.tileGridSize = HWMAP_GET_TILE_GRID_SIZE(header.flags);
  _currentMap.haloPct = HWMAP_GET_HALO_PCT(header.flags);
  _currentMap.quantBits = HWMAP_GET_QUANT_BITS(header.flags);
  _currentMap.tileCount = _currentMap.tileGridSize * _currentMap.tileGridSize;
  
  // Precompute tile geometry for dequantization (use normalized bounds)
  int32_t mapWidth = _currentMap.header.maxLon - _currentMap.header.minLon;
  int32_t mapHeight = _currentMap.header.maxLat - _currentMap.header.minLat;
  _currentMap.tileW = mapWidth / _currentMap.tileGridSize;
  _currentMap.tileH = mapHeight / _currentMap.tileGridSize;
  _currentMap.haloW = (int32_t)(_currentMap.tileW * _currentMap.haloPct);
  _currentMap.haloH = (int32_t)(_currentMap.tileH * _currentMap.haloPct);
  
  // Store path for later reads
  strncpy(_currentMap.filepath, path, sizeof(_currentMap.filepath) - 1);
  _currentMap.filepath[sizeof(_currentMap.filepath) - 1] = '\0';
  
  // Extract filename from path
  const char* fname = strrchr(path, '/');
  if (fname) fname++; else fname = path;
  strncpy(_currentMap.filename, fname, sizeof(_currentMap.filename) - 1);
  _currentMap.filename[sizeof(_currentMap.filename) - 1] = '\0';
  
  INFO_SENSORSF("Loading map v%u: %s (%zu bytes, %u features, %ux%u tiles)", 
               header.version, _currentMap.filename, fileSize, header.featureCount,
               _currentMap.tileGridSize, _currentMap.tileGridSize);
  
  // === PARSE NAME TABLE (small, keep in RAM) ===
  size_t nameTableEnd = sizeof(HWMapHeader);
  bool nameTableScanOk = true;
  if (header.nameCount > 0) {
    f.seek(sizeof(HWMapHeader));
    for (uint16_t i = 0; i < header.nameCount; i++) {
      int c = f.read();
      if (c < 0) {
        nameTableScanOk = false;
        break;
      }
      uint8_t strLen = (uint8_t)c;
      size_t nextPos = (size_t)f.position() + (size_t)strLen;
      if (!f.seek(nextPos)) {
        nameTableScanOk = false;
        break;
      }
    }
    if (nameTableScanOk) {
      nameTableEnd = (size_t)f.position();
    } else {
      ERROR_SENSORSF("Failed to scan name table");
      f.close();
      gSensorPollingPaused = wasPaused;
      unloadMap();
      return false;
    }
  }
  if (header.nameCount > 0 && header.nameCount <= MAX_MAP_NAMES) {
    MapNameEntry* names = (MapNameEntry*)ps_malloc(sizeof(MapNameEntry) * header.nameCount);
    if (names) {
      f.seek(sizeof(HWMapHeader));
      uint16_t parsed = 0;
      for (uint16_t i = 0; i < header.nameCount; i++) {
        int c = f.read();
        if (c < 0) break;
        uint8_t strLen = (uint8_t)c;
        size_t copyLen = (strLen < sizeof(names[parsed].name) - 1) ? strLen : sizeof(names[parsed].name) - 1;
        size_t got = (copyLen > 0) ? f.read((uint8_t*)names[parsed].name, copyLen) : 0;
        if (got != copyLen) break;
        names[parsed].name[copyLen] = '\0';
        if (strLen > copyLen) {
          size_t skipPos = (size_t)f.position() + (size_t)(strLen - copyLen);
          if (!f.seek(skipPos)) break;
        }
        parsed++;
      }
      _currentMap.names = names;
      _currentMap.nameCount = parsed;
      INFO_SENSORSF("Parsed %u names", parsed);
    }
  }
  
  // === PARSE TILE DIRECTORY (small, keep in RAM) ===
  if (_currentMap.tileCount == 0) {
    ERROR_SENSORSF("Tile count is 0 (gridSize=%u, flags=0x%04X)", _currentMap.tileGridSize, header.flags);
  } else if (_currentMap.tileCount > HWMAP_MAX_TILES) {
    ERROR_SENSORSF("Tile count %u exceeds max %u (gridSize=%u)", _currentMap.tileCount, HWMAP_MAX_TILES, _currentMap.tileGridSize);
  }
  if (_currentMap.tileCount > 0 && _currentMap.tileCount <= HWMAP_MAX_TILES) {
    size_t tileDirSize = sizeof(HWMapTileDirEntry) * _currentMap.tileCount;
    HWMapTileDirEntry* tileDir = (HWMapTileDirEntry*)ps_malloc(tileDirSize);
    
    if (tileDir) {
      f.seek(nameTableEnd);
      size_t readBytes = f.read((uint8_t*)tileDir, tileDirSize);
      if (readBytes == tileDirSize) {
        _currentMap.tileDir = tileDir;
        INFO_SENSORSF("Tile directory: %u tiles, first at offset %u", 
                      _currentMap.tileCount, tileDir[0].offset);
        // Detailed tile stats for debugging
        int nonEmpty = 0;
        for (uint16_t i = 0; i < _currentMap.tileCount; i++) {
          if (tileDir[i].payloadSize > 0) nonEmpty++;
        }
        DEBUG_MAPS_LOADINGF("[MAPS] tileDir: %u total, %d non-empty, tileW=%ld tileH=%ld haloW=%ld haloH=%ld",
                            _currentMap.tileCount, nonEmpty,
                            (long)_currentMap.tileW, (long)_currentMap.tileH,
                            (long)_currentMap.haloW, (long)_currentMap.haloH);
      } else {
        free(tileDir);
        ERROR_SENSORSF("Failed to read tile directory");
      }
    }
  }
  
  // Keep file open as persistent handle (closed in unloadMap)
  _currentMap.mapFile = f;
  
  // === ALLOCATE MULTI-SLOT TILE CACHE ===
  // Scan tile directory to find max tile payload size → determines slot size
  uint32_t maxPayload = 0;
  uint32_t totalPayload = 0;
  uint16_t nonEmptyTiles = 0;
  if (_currentMap.tileDir) {
    for (uint16_t i = 0; i < _currentMap.tileCount; i++) {
      uint32_t ps = _currentMap.tileDir[i].payloadSize;
      if (ps > 0) {
        nonEmptyTiles++;
        totalPayload += ps;
        if (ps > maxPayload) maxPayload = ps;
      }
    }
  }
  
  // Slot size = max payload rounded up to 4KB boundary, with minimum
  uint32_t slotSize = (maxPayload + 4095) & ~4095u;  // Round up to 4KB
  if (slotSize < MAP_CACHE_MIN_SLOT) slotSize = MAP_CACHE_MIN_SLOT;
  
  // Try to allocate pool (4MB default, fall back to smaller if needed)
  size_t poolSize = MAP_CACHE_POOL_SIZE;
  uint8_t* pool = nullptr;
  while (poolSize >= slotSize && !pool) {
    pool = (uint8_t*)ps_malloc(poolSize);
    if (!pool) {
      poolSize /= 2;  // Try half size
    }
  }
  if (!pool) {
    ERROR_SENSORSF("Failed to allocate tile cache pool in PSRAM");
    gSensorPollingPaused = wasPaused;
    unloadMap();
    return false;
  }
  
  uint16_t numSlots = (uint16_t)(poolSize / slotSize);
  if (numSlots > MAP_CACHE_MAX_SLOTS) numSlots = MAP_CACHE_MAX_SLOTS;
  if (numSlots == 0) numSlots = 1;
  
  // Allocate slot metadata array
  TileCacheSlot* slots = (TileCacheSlot*)ps_malloc(sizeof(TileCacheSlot) * numSlots);
  if (!slots) {
    free(pool);
    ERROR_SENSORSF("Failed to allocate %u tile cache slot entries", numSlots);
    gSensorPollingPaused = wasPaused;
    unloadMap();
    return false;
  }
  // Initialize all slots as empty
  for (uint16_t i = 0; i < numSlots; i++) {
    slots[i].tileIdx = -1;
    slots[i].dataSize = 0;
    slots[i].lastAccessSeq = 0;
  }
  
  _currentMap.cachePool = pool;
  _currentMap.cachePoolSize = poolSize;
  _currentMap.slotSize = slotSize;
  _currentMap.numSlots = numSlots;
  _currentMap.accessSeq = 0;
  _currentMap.slots = slots;
  _currentMap.cacheHits = 0;
  _currentMap.cacheMisses = 0;
  
  size_t metadataSize = sizeof(MapNameEntry) * _currentMap.nameCount + 
                        sizeof(HWMapTileDirEntry) * _currentMap.tileCount +
                        sizeof(TileCacheSlot) * numSlots;
  uint32_t avgPayload = nonEmptyTiles ? (totalPayload / nonEmptyTiles) : 0;
  INFO_SENSORSF("Tile cache: %uKB pool, %u slots x %uKB | tiles: %u non-empty, avg %uB, max %uB | meta: %zuB",
                (unsigned)(poolSize / 1024), numSlots, (unsigned)(slotSize / 1024),
                nonEmptyTiles, avgPayload, maxPayload, metadataSize);
  
  // === PRE-WARM CACHE: load all tiles that fit into slots at load time ===
  // This eliminates ALL runtime cache misses for maps that fit in the pool
  if (_currentMap.mapFile && _currentMap.tileDir && nonEmptyTiles <= numSlots) {
    uint16_t slotIdx = 0;
    uint16_t preloaded = 0;
    uint32_t preloadStart = millis();
    for (uint16_t i = 0; i < _currentMap.tileCount && slotIdx < numSlots; i++) {
      uint32_t ps = _currentMap.tileDir[i].payloadSize;
      if (ps == 0) continue;
      if (ps > slotSize) {
        DEBUG_MAPS_LOADINGF("[MAPS] prewarm: tile %u payload %u > slotSize %u, skipping", i, ps, slotSize);
        continue;
      }
      uint8_t* slotData = pool + ((size_t)slotIdx * slotSize);
      _currentMap.mapFile.seek(_currentMap.tileDir[i].offset);
      size_t got = _currentMap.mapFile.read(slotData, ps);
      if (got == ps) {
        slots[slotIdx].tileIdx = (int16_t)i;
        slots[slotIdx].dataSize = ps;
        slots[slotIdx].lastAccessSeq = ++_currentMap.accessSeq;
        slotIdx++;
        preloaded++;
      }
    }
    INFO_SENSORSF("Cache pre-warmed: %u tiles loaded in %lums (zero runtime misses expected)",
                  preloaded, (unsigned long)(millis() - preloadStart));
  } else if (nonEmptyTiles > numSlots) {
    INFO_SENSORSF("Map too large for full pre-warm: %u tiles > %u slots (LRU will handle misses)",
                  nonEmptyTiles, numSlots);
  }
  
  // Invalidate location context since map changed
  LocationContextManager::invalidate();
  
  // Load waypoints for this map
  WaypointManager::loadWaypoints();
  
  // Resume sensor polling
  gSensorPollingPaused = wasPaused;
  
  return true;
}

void MapCore::unloadMap() {
  // Log cache stats before freeing
  if (_currentMap.valid && (_currentMap.cacheHits > 0 || _currentMap.cacheMisses > 0)) {
    uint32_t total = _currentMap.cacheHits + _currentMap.cacheMisses;
    INFO_SENSORSF("Tile cache stats: %u hits, %u misses (%.1f%% hit rate), %u slots",
                  _currentMap.cacheHits, _currentMap.cacheMisses,
                  total > 0 ? (100.0f * _currentMap.cacheHits / total) : 0.0f,
                  _currentMap.numSlots);
  }
  
  // Close persistent file handle
  if (_currentMap.mapFile) {
    _currentMap.mapFile.close();
  }
  
  // Free multi-slot cache
  if (_currentMap.slots) {
    free(_currentMap.slots);
    _currentMap.slots = nullptr;
  }
  if (_currentMap.cachePool) {
    free(_currentMap.cachePool);
    _currentMap.cachePool = nullptr;
  }
  _currentMap.cachePoolSize = 0;
  _currentMap.slotSize = 0;
  _currentMap.numSlots = 0;
  _currentMap.accessSeq = 0;
  _currentMap.cacheHits = 0;
  _currentMap.cacheMisses = 0;
  
  if (_currentMap.names) {
    free(_currentMap.names);
    _currentMap.names = nullptr;
  }
  // Free tile directory
  if (_currentMap.tileDir) {
    free(_currentMap.tileDir);
    _currentMap.tileDir = nullptr;
  }
  _currentMap.tileCount = 0;
  _currentMap.tileGridSize = 0;
  
  _currentMap.valid = false;
  _currentMap.fileSize = 0;
  _currentMap.filename[0] = '\0';
  _currentMap.filepath[0] = '\0';
  _currentMap.nameCount = 0;
  
  // Invalidate context when map unloaded
  LocationContextManager::invalidate();
}

const char* MapCore::getName(uint16_t index) {
  if (!_currentMap.valid || !_currentMap.names || index >= _currentMap.nameCount) {
    return nullptr;
  }
  return _currentMap.names[index].name;
}

// Helper: Load tile data via multi-slot cache
// Returns pointer to tile data in cache slot, or nullptr on error
const uint8_t* MapCore::loadTileData(uint16_t tileIdx, size_t* outSize) {
  if (!_currentMap.valid || !_currentMap.tileDir || tileIdx >= _currentMap.tileCount) {
    return nullptr;
  }
  
  HWMapTileDirEntry& tile = _currentMap.tileDir[tileIdx];
  if (tile.payloadSize == 0) {
    if (outSize) *outSize = 0;
    return nullptr;
  }
  
  if (!_currentMap.cachePool || !_currentMap.slots || _currentMap.numSlots == 0) {
    DEBUG_MAPS_RENDERINGF("[MAPS] loadTileData: cache not initialized!");
    return nullptr;
  }
  
  // === CACHE LOOKUP: search slots for this tile ===
  for (uint16_t i = 0; i < _currentMap.numSlots; i++) {
    if (_currentMap.slots[i].tileIdx == (int16_t)tileIdx) {
      // Cache hit - update LRU counter and return
      _currentMap.slots[i].lastAccessSeq = ++_currentMap.accessSeq;
      _currentMap.cacheHits++;
      if (outSize) *outSize = _currentMap.slots[i].dataSize;
      const uint8_t* hitPtr = _currentMap.cachePool + ((size_t)i * _currentMap.slotSize);
      return hitPtr;
    }
  }
  
  // === CACHE MISS ===
  _currentMap.cacheMisses++;
  
  uint32_t payloadSize = tile.payloadSize;
  
  // If tile is larger than slot size, log warning and read directly into slot 0
  // (evicting whatever was there - this should be rare/never with proper slot sizing)
  if (payloadSize > _currentMap.slotSize) {
    DEBUG_MAPS_RENDERINGF("[MAPS] WARNING: tile %u payload %u > slotSize %u, truncating read!",
                          tileIdx, payloadSize, _currentMap.slotSize);
    payloadSize = _currentMap.slotSize;
  }
  
  // Find a slot: prefer empty, otherwise evict LRU
  int16_t targetSlot = -1;
  uint32_t oldestSeq = UINT32_MAX;
  for (uint16_t i = 0; i < _currentMap.numSlots; i++) {
    if (_currentMap.slots[i].tileIdx == -1) {
      targetSlot = i;
      break;  // Empty slot found, use it
    }
    if (_currentMap.slots[i].lastAccessSeq < oldestSeq) {
      oldestSeq = _currentMap.slots[i].lastAccessSeq;
      targetSlot = i;
    }
  }
  
  if (targetSlot < 0) targetSlot = 0;  // Shouldn't happen, but safety
  
  DEBUG_MAPS_PERFF("[TILE_CACHE] miss tile=%u slot=%d payload=%uB %s (hits=%u misses=%u seq=%u)",
                   tileIdx, targetSlot, tile.payloadSize,
                   _currentMap.slots[targetSlot].tileIdx == -1 ? "empty" : "evict",
                   _currentMap.cacheHits, _currentMap.cacheMisses, _currentMap.accessSeq);
  
  // Read tile data from file into the target slot
  uint8_t* slotData = _currentMap.cachePool + ((size_t)targetSlot * _currentMap.slotSize);
  
  {
    FsLockGuard fsGuard("MapCore.loadTileData");
    bool usedPersistent = false;
    size_t bytesRead = 0;
    
    // Prefer persistent handle (no open/close overhead)
    if (_currentMap.mapFile) {
      _currentMap.mapFile.seek(tile.offset);
      bytesRead = _currentMap.mapFile.read(slotData, payloadSize);
      usedPersistent = true;
    } else {
      // Fallback: open/close per miss
      File f = LittleFS.open(_currentMap.filepath, "r");
      if (!f) {
        DEBUG_MAPS_RENDERINGF("[MAPS] loadTileData: failed to open '%s'", _currentMap.filepath);
        return nullptr;
      }
      f.seek(tile.offset);
      bytesRead = f.read(slotData, payloadSize);
      f.close();
    }
    
    if (bytesRead != payloadSize) {
      DEBUG_MAPS_RENDERINGF("[MAPS] loadTileData: short read tile %u: got %zu, expected %u (persistent=%d)",
                            tileIdx, bytesRead, payloadSize, usedPersistent);
      if (bytesRead == 0) { return nullptr; }
      payloadSize = bytesRead;
    }
  }
  
  // Update slot metadata
  _currentMap.slots[targetSlot].tileIdx = (int16_t)tileIdx;
  _currentMap.slots[targetSlot].dataSize = payloadSize;
  _currentMap.slots[targetSlot].lastAccessSeq = ++_currentMap.accessSeq;
  
  if (outSize) *outSize = payloadSize;
  return slotData;
}

int MapCore::searchNamesByPrefix(const char* prefix, const char** results, int maxResults) {
  if (!_currentMap.valid || !_currentMap.names || !results || maxResults <= 0) {
    return 0;
  }
  
  if (!prefix || prefix[0] == '\0') {
    // No prefix - return first N names
    int count = min((int)_currentMap.nameCount, maxResults);
    for (int i = 0; i < count; i++) {
      results[i] = _currentMap.names[i].name;
    }
    return count;
  }
  
  int prefixLen = strlen(prefix);
  int count = 0;
  
  // Case-insensitive prefix search
  for (int i = 0; i < _currentMap.nameCount && count < maxResults; i++) {
    const char* name = _currentMap.names[i].name;
    if (name && strncasecmp(name, prefix, prefixLen) == 0) {
      results[count++] = name;
    }
  }
  
  return count;
}

bool MapCore::isPositionInMap(float lat, float lon) {
  if (!_currentMap.valid) return false;
  
  int32_t latMicro = (int32_t)(lat * 1000000);
  int32_t lonMicro = (int32_t)(lon * 1000000);
  
  return (latMicro >= _currentMap.header.minLat &&
          latMicro <= _currentMap.header.maxLat &&
          lonMicro >= _currentMap.header.minLon &&
          lonMicro <= _currentMap.header.maxLon);
}

int MapCore::getAvailableMaps(char maps[][96], int maxMaps) {
  int count = 0;

  FsLockGuard fsGuard("MapCore.getAvailableMaps");
  
  if (!LittleFS.exists("/maps")) {
    return 0;
  }
  
  File dir = LittleFS.open("/maps");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }
  
  File entry = dir.openNextFile();
  while (entry && count < maxMaps) {
    if (entry.isDirectory()) {
      String dirName = String(entry.name());
      if (dirName.startsWith("/maps/")) dirName = dirName.substring(6);
      if (dirName.startsWith("/")) dirName = dirName.substring(1);
      if (dirName.length() > 0 && dirName.indexOf('/') == -1) {
        char subPathBuf[64];
        snprintf(subPathBuf, sizeof(subPathBuf), "/maps/%s", dirName.c_str());
        String subPath = subPathBuf;
        File sub = LittleFS.open(subPath);
        if (sub && sub.isDirectory()) {
          String preferred = dirName + ".hwmap";
          String found = "";
          File f = sub.openNextFile();
          while (f) {
            if (!f.isDirectory()) {
              String fn = String(f.name());
              String prefix = subPath + "/";
              if (fn.startsWith(prefix)) fn = fn.substring(prefix.length());
              if (fn.indexOf('/') == -1) {
                if (fn.length() > 6 && fn.substring(fn.length() - 6).equalsIgnoreCase(".hwmap")) {
                  if (fn == preferred) { found = fn; break; }
                  if (found.length() == 0) found = fn;
                }
              }
            }
            f = sub.openNextFile();
          }
          sub.close();
          if (found.length() > 0) {
            String rel = dirName + "/" + found;
            strncpy(maps[count], rel.c_str(), 95);
            maps[count][95] = '\0';
            count++;
          }
        } else {
          if (sub) sub.close();
        }
      }
    }
    entry = dir.openNextFile();
  }
  dir.close();
  
  return count;
}

// =============================================================================
// MapCore - Display-Agnostic Rendering
// =============================================================================

void MapCore::geoToScreen(int32_t lat, int32_t lon,
                          int32_t centerLat, int32_t centerLon,
                          int32_t scaleX, int32_t scaleY,
                          int viewWidth, int viewHeight,
                          int16_t& screenX, int16_t& screenY) {
  // Center of viewport
  const int16_t cx = viewWidth / 2;
  const int16_t cy = viewHeight / 2;
  
  // Delta from center in microdegrees
  int32_t dLon = lon - centerLon;
  int32_t dLat = lat - centerLat;
  
  // Convert to screen pixels (scaleX/Y = microdegrees per pixel)
  float x = (float)dLon / (float)scaleX;
  float y = -(float)dLat / (float)scaleY;  // Y is inverted (north = up)
  
  // Apply rotation around center if rotation is set
  if (gMapRotation != 0.0f) {
    float rad = gMapRotation * PI / 180.0f;
    float cosR = cosf(rad);
    float sinR = sinf(rad);
    float rx = x * cosR - y * sinR;
    float ry = x * sinR + y * cosR;
    x = rx;
    y = ry;
  }
  
  screenX = cx + (int16_t)x;
  screenY = cy + (int16_t)y;
}

// Thread-safe renderMap: all mutable state comes from params, no global reads
void MapCore::renderMap(MapRenderer* renderer, float centerLat, float centerLon,
                        const MapRenderParams& params) {
  if (!_currentMap.valid || !renderer || !_currentMap.tileDir) {
    DEBUG_MAPS_RENDERINGF("[MAPS] renderMap early exit: valid=%d renderer=%p tileDir=%p",
                          _currentMap.valid, renderer, _currentMap.tileDir);
    return;
  }
  
  const float zoom = params.zoom;
  const float rotation = params.rotation;
  
  const uint32_t perfStart = millis();
  uint32_t perfTileIOus = 0;
  
  int viewWidth = renderer->getWidth();
  int viewHeight = renderer->getHeight();
  
  // Convert center to microdegrees
  int32_t centerLatMicro = (int32_t)(centerLat * 1000000);
  int32_t centerLonMicro = (int32_t)(centerLon * 1000000);
  
  // Calculate scale: how many microdegrees per pixel
  int32_t baseScaleY = 188;   // Microdegrees per pixel (latitude) at 1x
  int32_t baseScaleX = 246;   // Microdegrees per pixel (longitude) at 1x
  int32_t scaleY = (int32_t)(baseScaleY / zoom);
  int32_t scaleX = (int32_t)(baseScaleX / zoom);
  if (scaleX < 10) scaleX = 10;
  if (scaleY < 10) scaleY = 10;
  
  // Pre-compute values for fast coordinate transform (avoids per-point division + trig)
  const float invScaleX = 1.0f / (float)scaleX;
  const float invScaleY = 1.0f / (float)scaleY;
  const int16_t cx = viewWidth / 2;
  const int16_t cy = viewHeight / 2;
  const bool hasRotation = (rotation != 0.0f);
  float cosR = 1.0f, sinR = 0.0f;
  if (hasRotation) {
    float rad = rotation * (float)PI / 180.0f;
    cosR = cosf(rad);
    sinR = sinf(rad);
  }
  
  // Calculate visible tile range based on viewport
  int32_t viewHalfWidth = (viewWidth / 2) * scaleX;
  int32_t viewHalfHeight = (viewHeight / 2) * scaleY;
  int32_t viewMinLon = centerLonMicro - viewHalfWidth;
  int32_t viewMaxLon = centerLonMicro + viewHalfWidth;
  int32_t viewMinLat = centerLatMicro - viewHalfHeight;
  int32_t viewMaxLat = centerLatMicro + viewHalfHeight;
  
  // Determine which tiles intersect the viewport
  int minTileX = (viewMinLon - _currentMap.header.minLon) / _currentMap.tileW;
  int maxTileX = (viewMaxLon - _currentMap.header.minLon) / _currentMap.tileW;
  int minTileY = (viewMinLat - _currentMap.header.minLat) / _currentMap.tileH;
  int maxTileY = (viewMaxLat - _currentMap.header.minLat) / _currentMap.tileH;
  
  // Clamp to valid tile range
  int rawMinTX = minTileX, rawMaxTX = maxTileX, rawMinTY = minTileY, rawMaxTY = maxTileY;
  if (minTileX < 0) minTileX = 0;
  if (maxTileX >= _currentMap.tileGridSize) maxTileX = _currentMap.tileGridSize - 1;
  if (minTileY < 0) minTileY = 0;
  if (maxTileY >= _currentMap.tileGridSize) maxTileY = _currentMap.tileGridSize - 1;
  
  // If viewport spans more tiles than cache can hold, use stride to prevent thrashing.
  // At low zoom on the OLED, each tile is only a few pixels — small gaps are invisible.
  int tileStep = 1;
  int numTilesX = maxTileX - minTileX + 1;
  int numTilesY = maxTileY - minTileY + 1;
  if (_currentMap.numSlots > 0) {
    while (((numTilesX + tileStep - 1) / tileStep) * ((numTilesY + tileStep - 1) / tileStep) > (int)_currentMap.numSlots && tileStep < 6) {
      tileStep++;
    }
  }
  
  DEBUG_MAPS_RENDERINGF("[MAPS] render: center=%.5f,%.5f zoom=%.2f scale=%ld,%ld grid=%d tiles=%d-%d/%d-%d (raw %d-%d/%d-%d) step=%d",
                        centerLat, centerLon, zoom, (long)scaleX, (long)scaleY,
                        _currentMap.tileGridSize,
                        minTileX, maxTileX, minTileY, maxTileY,
                        rawMinTX, rawMaxTX, rawMinTY, rawMaxTY, tileStep);

  int totalFeatures = 0, totalDrawn = 0, tilesLoaded = 0, tilesEmpty = 0;

  // Iterate through visible tiles (with stride to cap tile count within cache budget)
  for (int ty = minTileY; ty <= maxTileY; ty += tileStep) {
    for (int tx = minTileX; tx <= maxTileX; tx += tileStep) {
      uint16_t tileIdx = ty * _currentMap.tileGridSize + tx;
      if (tileIdx >= _currentMap.tileCount) continue;
      
      // Bail early if map was unloaded mid-render (async safety)
      if (!_currentMap.valid) return;
      
      HWMapTileDirEntry& tile = _currentMap.tileDir[tileIdx];
      if (tile.payloadSize == 0) { tilesEmpty++; continue; }
      
      // Calculate tile halo bounds for dequantization
      int32_t tileMinLon = _currentMap.header.minLon + tx * _currentMap.tileW - _currentMap.haloW;
      int32_t tileMaxLon = _currentMap.header.minLon + (tx + 1) * _currentMap.tileW + _currentMap.haloW;
      int32_t tileMinLat = _currentMap.header.minLat + ty * _currentMap.tileH - _currentMap.haloH;
      int32_t tileMaxLat = _currentMap.header.minLat + (ty + 1) * _currentMap.tileH + _currentMap.haloH;
      int32_t haloLonSpan = tileMaxLon - tileMinLon;
      int32_t haloLatSpan = tileMaxLat - tileMinLat;
      
      // Load tile data
      size_t tileDataSize;
      uint32_t tileIOStart = micros();
      const uint8_t* tileData = loadTileData(tileIdx, &tileDataSize);
      perfTileIOus += (uint32_t)(micros() - tileIOStart);
      if (!tileData || tileDataSize == 0) {
        DEBUG_MAPS_RENDERINGF("[MAPS] tile(%d,%d) idx=%u: loadTileData failed (ptr=%p size=%zu offset=%u payloadSize=%u)",
                              tx, ty, tileIdx, tileData, tileDataSize, tile.offset, tile.payloadSize);
        continue;
      }
      
      const uint8_t* ptr = tileData;
      const uint8_t* end = tileData + tileDataSize;
      
      // Feature count is at the START of each tile's payload (2 bytes)
      if (ptr + 2 > end) continue;
      uint16_t featureCount = ptr[0] | (ptr[1] << 8);
      ptr += 2;
      tilesLoaded++;
      totalFeatures += featureCount;
      
      // Parse and render features in this tile
      for (uint16_t f = 0; f < featureCount; f++) {
        if (ptr + HWMAP_FEATURE_HEADER_SIZE > end) break;
        
        uint8_t ftype = ptr[0];
        uint8_t fsubtype = ptr[1];
        // type(1) + subtype(1) + nameIndex(2) + pointCount(2)
        uint16_t nameIndex = ptr[2] | (ptr[3] << 8);
        uint16_t pointCount = ptr[4] | (ptr[5] << 8);
        ptr += HWMAP_FEATURE_HEADER_SIZE;
        
        size_t pointsBytes = pointCount * 4;
        if (ptr + pointsBytes > end) break;
        
        if (pointCount < 2) {
          ptr += pointsBytes;
          continue;
        }
        
        // Check layer visibility (uses snapshot, not globals)
        if (!paramLayerIsVisible(params, ftype)) {
          ptr += pointsBytes;
          continue;
        }
        
        // Check per-subtype visibility (uses snapshot)
        if (!paramSubtypeIsVisible(params, ftype, fsubtype)) {
          ptr += pointsBytes;
          continue;
        }
        
        // Subtype-based LOD: hide less important subtypes at low zoom
        if (ftype == MAP_FEATURE_ROAD_MINOR && fsubtype == SUBTYPE_MINOR_SERVICE && zoom < LOD_ZOOM_SERVICE_ROAD) {
          ptr += pointsBytes;
          continue;
        }
        if (ftype == MAP_FEATURE_PATH && fsubtype == SUBTYPE_PATH_TRACK && zoom < LOD_ZOOM_TRACK) {
          ptr += pointsBytes;
          continue;
        }
        
        // LOD culling - progressively hide features at lower zoom levels
        if (zoom < LOD_ZOOM_MAJOR_ROAD) {
          // Very far out: only highways
          if (ftype != MAP_FEATURE_HIGHWAY) {
            ptr += pointsBytes;
            continue;
          }
        } else if (zoom < LOD_ZOOM_WATER) {
          // Far out: hide everything except highways + major roads
          if (ftype != MAP_FEATURE_HIGHWAY && ftype != MAP_FEATURE_ROAD_MAJOR) {
            ptr += pointsBytes;
            continue;
          }
        } else if (zoom < LOD_ZOOM_MINOR_ROAD) {
          // Hide minor roads, paths, buildings, parks, transit
          if (ftype == MAP_FEATURE_ROAD_MINOR || ftype == MAP_FEATURE_PATH ||
              ftype == MAP_FEATURE_BUILDING || ftype == MAP_FEATURE_PARK ||
              ftype == MAP_FEATURE_BUS || ftype == MAP_FEATURE_STATION) {
            ptr += pointsBytes;
            continue;
          }
        } else if (zoom < LOD_ZOOM_PATH) {
          if (ftype == MAP_FEATURE_PATH) {
            ptr += pointsBytes;
            continue;
          }
        }
        // Buildings: only show when zoomed in enough to avoid blob effect
        if (ftype == MAP_FEATURE_BUILDING && zoom < LOD_ZOOM_BUILDING) {
          ptr += pointsBytes;
          continue;
        }
        
        // Subtype-level filtering (renderer can skip specific type+subtype combos)
        if (!renderer->shouldRenderFeature(ftype, fsubtype)) {
          ptr += pointsBytes;
          continue;
        }
        
        // Get style
        MapFeatureStyle style = renderer->getFeatureStyle((MapFeatureType)ftype);
        if (!style.render || style.lineStyle == LINE_NONE) {
          ptr += pointsBytes;
          continue;
        }
        
        // Check highlighting
        bool isHighlighted = mapHighlightMatches(nameIndex, ftype);
        if (isHighlighted && !mapHighlightIsVisible()) {
          ptr += pointsBytes;
          continue;
        }
        
        // Read and dequantize first point (inline transform using pre-computed values)
        uint16_t qLat = ptr[0] | (ptr[1] << 8);
        uint16_t qLon = ptr[2] | (ptr[3] << 8);
        ptr += 4;
        
        int32_t lat = tileMinLat + (int32_t)((int64_t)qLat * haloLatSpan >> 16);
        int32_t lon = tileMinLon + (int32_t)((int64_t)qLon * haloLonSpan >> 16);
        
        int16_t prevX, prevY;
        {
          float fx = (float)(lon - centerLonMicro) * invScaleX;
          float fy = -(float)(lat - centerLatMicro) * invScaleY;
          if (hasRotation) { float rx = fx*cosR - fy*sinR; fy = fx*sinR + fy*cosR; fx = rx; }
          prevX = cx + (int16_t)fx;
          prevY = cy + (int16_t)fy;
        }
        
        // Process remaining points
        for (uint16_t p = 1; p < pointCount; p++) {
          qLat = ptr[0] | (ptr[1] << 8);
          qLon = ptr[2] | (ptr[3] << 8);
          ptr += 4;
          
          lat = tileMinLat + (int32_t)((int64_t)qLat * haloLatSpan >> 16);
          lon = tileMinLon + (int32_t)((int64_t)qLon * haloLonSpan >> 16);
          
          int16_t curX, curY;
          {
            float fx = (float)(lon - centerLonMicro) * invScaleX;
            float fy = -(float)(lat - centerLatMicro) * invScaleY;
            if (hasRotation) { float rx = fx*cosR - fy*sinR; fy = fx*sinR + fy*cosR; fx = rx; }
            curX = cx + (int16_t)fx;
            curY = cy + (int16_t)fy;
          }
          
          // Simple visibility check
          bool visible = (prevX >= -50 && prevX < viewWidth + 50 &&
                          prevY >= -50 && prevY < viewHeight + 50) ||
                         (curX >= -50 && curX < viewWidth + 50 &&
                          curY >= -50 && curY < viewHeight + 50);
          
          if (visible) {
            renderer->drawLine(prevX, prevY, curX, curY, style);
            totalDrawn++;
          }
          
          prevX = curX;
          prevY = curY;
        }
      }
    }
  }
  
  DEBUG_MAPS_RENDERINGF("[MAPS] render done: %d tiles loaded, %d empty, %d features, %d lines drawn",
                        tilesLoaded, tilesEmpty, totalFeatures, totalDrawn);

  // Draw waypoints on map
  WaypointManager::renderWaypoints(renderer, centerLat, centerLon, scaleX, scaleY);
  
  // Draw GPS position marker at center
  renderer->drawPositionMarker(viewWidth / 2, viewHeight / 2);
  
  uint32_t perfTotal = millis() - perfStart;
  DEBUG_MAPS_PERFF("[MAP_PERF] render: %lums total | tileIO: %luus | tiles:%d feat:%d lines:%d | zoom:%.2f viewport:%dx%d",
                   (unsigned long)perfTotal, (unsigned long)perfTileIOus,
                   tilesLoaded, totalFeatures, totalDrawn, zoom, viewWidth, viewHeight);
}

// =============================================================================
// OffscreenMapRenderer Implementation (1-bit framebuffer for async rendering)
// =============================================================================

OffscreenMapRenderer::OffscreenMapRenderer(uint8_t* buffer, int width, int height, int offsetY)
  : _buffer(buffer), _offsetY(offsetY) {
  _width = width;
  _height = height;
}

void OffscreenMapRenderer::setViewport(int width, int height) {
  _width = width;
  _height = height;
}

void OffscreenMapRenderer::clear() {
  if (_buffer) memset(_buffer, 0, OFFSCREEN_BUF_SIZE);
}

// SSD1306-compatible pixel layout: byte = 8 vertical pixels, LSB = top
void OffscreenMapRenderer::drawPixel(int16_t x, int16_t y) {
  int16_t ay = y + _offsetY;
  if (x < 0 || x >= 128 || ay < 0 || ay >= 64) return;
  _buffer[x + (ay / 8) * 128] |= (1 << (ay & 7));
}

bool OffscreenMapRenderer::clipLine(int16_t& x0, int16_t& y0, int16_t& x1, int16_t& y1) {
  // Cohen-Sutherland clipping to [0, _offsetY] .. [_width-1, _offsetY+_height-1]
  const int16_t xmin = 0, ymin = _offsetY, xmax = _width - 1, ymax = _offsetY + _height - 1;
  
  auto outcode = [&](int16_t x, int16_t y) -> int {
    int code = 0;
    if (x < xmin) code |= 1;
    else if (x > xmax) code |= 2;
    if (y < ymin) code |= 4;
    else if (y > ymax) code |= 8;
    return code;
  };
  
  int code0 = outcode(x0, y0);
  int code1 = outcode(x1, y1);
  
  for (int iter = 0; iter < 10; iter++) {
    if (!(code0 | code1)) return true;
    if (code0 & code1) return false;
    
    int codeOut = code0 ? code0 : code1;
    int16_t x, y;
    int32_t dx = x1 - x0, dy = y1 - y0;
    
    if (codeOut & 8) {
      x = (dy != 0) ? (int16_t)(x0 + dx * (int32_t)(ymax - y0) / dy) : x0;
      y = ymax;
    } else if (codeOut & 4) {
      x = (dy != 0) ? (int16_t)(x0 + dx * (int32_t)(ymin - y0) / dy) : x0;
      y = ymin;
    } else if (codeOut & 2) {
      y = (dx != 0) ? (int16_t)(y0 + dy * (int32_t)(xmax - x0) / dx) : y0;
      x = xmax;
    } else {
      y = (dx != 0) ? (int16_t)(y0 + dy * (int32_t)(xmin - x0) / dx) : y0;
      x = xmin;
    }
    
    if (codeOut == code0) {
      x0 = x; y0 = y;
      code0 = outcode(x0, y0);
    } else {
      x1 = x; y1 = y;
      code1 = outcode(x1, y1);
    }
  }
  return false;
}

void OffscreenMapRenderer::bresenhamLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  int16_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int16_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int16_t err = dx + dy;
  
  for (;;) {
    // Direct pixel set (already clipped)
    int16_t ay = y0;  // y0 already includes _offsetY from clipLine
    if (ay >= 0 && ay < 64 && x0 >= 0 && x0 < 128)
      _buffer[x0 + (ay / 8) * 128] |= (1 << (ay & 7));
    
    if (x0 == x1 && y0 == y1) break;
    int16_t e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void OffscreenMapRenderer::drawDashedLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int dashLen) {
  int adx = abs(x1 - x0), ady = abs(y1 - y0);
  int len = (adx > ady) ? (adx + ady / 2) : (ady + adx / 2);
  if (len < 1) return;
  
  float invLen = 1.0f / len;
  float dx = (x1 - x0) * invLen;
  float dy = (y1 - y0) * invLen;
  float x = x0, y = y0;
  bool draw = true;
  int segLen = 0;
  
  for (int t = 0; t < len; t++) {
    if (draw) {
      int16_t px = (int16_t)x, py = (int16_t)y;
      if (px >= 0 && px < 128 && py >= 0 && py < 64)
        _buffer[px + (py / 8) * 128] |= (1 << (py & 7));
    }
    x += dx; y += dy;
    if (++segLen >= dashLen) { segLen = 0; draw = !draw; }
  }
}

void OffscreenMapRenderer::drawDottedLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int spacing) {
  int adx = abs(x1 - x0), ady = abs(y1 - y0);
  int len = (adx > ady) ? (adx + ady / 2) : (ady + adx / 2);
  if (len < 1) return;
  
  float invLen = 1.0f / len;
  float dx = (x1 - x0) * invLen;
  float dy = (y1 - y0) * invLen;
  
  for (int t = 0; t <= len; t += spacing) {
    int16_t px = x0 + (int16_t)(dx * t);
    int16_t py = y0 + (int16_t)(dy * t);
    if (px >= 0 && px < 128 && py >= 0 && py < 64)
      _buffer[px + (py / 8) * 128] |= (1 << (py & 7));
  }
}

void OffscreenMapRenderer::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                     const MapFeatureStyle& style) {
  if (!_buffer) return;
  
  // Apply content area offset
  y0 += _offsetY;
  y1 += _offsetY;
  
  // Clip to content area
  if (!clipLine(x0, y0, x1, y1)) return;
  
  switch (style.lineStyle) {
    case LINE_SOLID:
      bresenhamLine(x0, y0, x1, y1);
      break;
    case LINE_DASHED:
      drawDashedLine(x0, y0, x1, y1, 4);
      break;
    case LINE_DOTTED:
      drawDottedLine(x0, y0, x1, y1, 3);
      break;
    case LINE_NONE:
    default:
      break;
  }
}

void OffscreenMapRenderer::drawPositionMarker(int16_t x, int16_t y) {
  if (!_buffer) return;
  y += _offsetY;
  
  const int16_t ymin = _offsetY, ymax = _offsetY + _height - 1;
  if (y < ymin || y > ymax) return;
  
  // Crosshair
  MapFeatureStyle solid = {LINE_SOLID, 1, 15, true, 0xFFFF};
  int16_t x0 = x - 4, x1 = x + 4;
  int16_t yt = (y - 4 < ymin) ? ymin : y - 4;
  int16_t yb = (y + 4 > ymax) ? ymax : y + 4;
  
  // Horizontal
  int16_t cy0 = y, cy1 = y;
  if (clipLine(x0, cy0, x1, cy1)) bresenhamLine(x0, cy0, x1, cy1);
  // Vertical
  int16_t cx0 = x, cx1 = x;
  if (clipLine(cx0, yt, cx1, yb)) bresenhamLine(cx0, yt, cx1, yb);
  
  // Simple circle (midpoint algorithm, r=3)
  if (y - 3 >= ymin && y + 3 <= ymax) {
    int16_t r = 3, px = r, py = 0, err = 1 - r;
    while (px >= py) {
      drawPixel(x + px, y + py - _offsetY);
      drawPixel(x - px, y + py - _offsetY);
      drawPixel(x + px, y - py - _offsetY);
      drawPixel(x - px, y - py - _offsetY);
      drawPixel(x + py, y + px - _offsetY);
      drawPixel(x - py, y + px - _offsetY);
      drawPixel(x + py, y - px - _offsetY);
      drawPixel(x - py, y - px - _offsetY);
      py++;
      if (err < 0) {
        err += 2 * py + 1;
      } else {
        px--;
        err += 2 * (py - px) + 1;
      }
    }
  }
}

// Reuse OLED styles for offscreen (same 1-bit rendering characteristics)
MapFeatureStyle OffscreenMapRenderer::getFeatureStyle(MapFeatureType type) {
  switch (type) {
    case MAP_FEATURE_HIGHWAY:    return {LINE_SOLID, 1, 10, true, 0xFFFF};
    case MAP_FEATURE_ROAD_MAJOR: return {LINE_SOLID, 1, 9, true, 0xFFFF};
    case MAP_FEATURE_ROAD_MINOR: return {LINE_SOLID, 1, 5, true, 0xFFFF};
    case MAP_FEATURE_PATH:       return {LINE_DOTTED, 1, 3, true, 0xFFFF};
    case MAP_FEATURE_WATER:      return {LINE_SOLID, 1, 8, true, 0xFFFF};
    case MAP_FEATURE_PARK:       return {LINE_NONE, 1, 0, false, 0xFFFF};
    case MAP_FEATURE_LAND_MASK:  return {LINE_NONE, 1, 0, false, 0xFFFF};
    case MAP_FEATURE_RAILWAY:    return {LINE_DASHED, 1, 7, true, 0xFFFF};
    case MAP_FEATURE_BUS:        return {LINE_DASHED, 1, 4, true, 0xFFFF};
    case MAP_FEATURE_FERRY:      return {LINE_DASHED, 1, 6, true, 0xFFFF};
    case MAP_FEATURE_BUILDING:   return {LINE_DOTTED, 1, 1, true, 0xFFFF};
    case MAP_FEATURE_STATION:    return {LINE_SOLID, 1, 7, true, 0xFFFF};
    default:                     return {LINE_SOLID, 1, 5, true, 0xFFFF};
  }
}

bool OffscreenMapRenderer::shouldRenderFeature(uint8_t type, uint8_t subtype) {
  switch (type) {
    case MAP_FEATURE_WATER:
      return (subtype == SUBTYPE_WATER_RIVER);
    case MAP_FEATURE_PARK:
    case MAP_FEATURE_LAND_MASK:
      return false;
    default:
      return true;
  }
}

// =============================================================================
// OLEDMapRenderer Implementation
// =============================================================================

#if ENABLE_OLED_DISPLAY

OLEDMapRenderer::OLEDMapRenderer(Adafruit_SSD1306* display) : _display(display) {
  _width = DISPLAY_WIDTH;
  _height = DISPLAY_CONTENT_HEIGHT;  // Content area only (between header and footer)
  _offsetY = DISPLAY_CONTENT_START_Y;  // Offset to content area start
}

void OLEDMapRenderer::setViewport(int width, int height) {
  _width = width;
  _height = height;
}

void OLEDMapRenderer::clear() {
  // Don't clear - OLED display is managed by the mode system
}

// Cohen-Sutherland line clipping to content area [0, _offsetY, _width-1, _offsetY+_height-1]
bool OLEDMapRenderer::clipToContent(int16_t& x0, int16_t& y0, int16_t& x1, int16_t& y1) {
  const int16_t xmin = 0, ymin = _offsetY, xmax = _width - 1, ymax = _offsetY + _height - 1;
  
  auto outcode = [&](int16_t x, int16_t y) -> int {
    int code = 0;
    if (x < xmin) code |= 1;
    else if (x > xmax) code |= 2;
    if (y < ymin) code |= 4;
    else if (y > ymax) code |= 8;
    return code;
  };
  
  int code0 = outcode(x0, y0);
  int code1 = outcode(x1, y1);
  
  for (int iter = 0; iter < 10; iter++) {
    if (!(code0 | code1)) return true;   // Both inside
    if (code0 & code1) return false;     // Both outside same side
    
    int codeOut = code0 ? code0 : code1;
    int16_t x, y;
    int32_t dx = x1 - x0, dy = y1 - y0;
    
    if (codeOut & 8) {        // Below ymax
      x = (dy != 0) ? (int16_t)(x0 + dx * (int32_t)(ymax - y0) / dy) : x0;
      y = ymax;
    } else if (codeOut & 4) { // Above ymin
      x = (dy != 0) ? (int16_t)(x0 + dx * (int32_t)(ymin - y0) / dy) : x0;
      y = ymin;
    } else if (codeOut & 2) { // Right of xmax
      y = (dx != 0) ? (int16_t)(y0 + dy * (int32_t)(xmax - x0) / dx) : y0;
      x = xmax;
    } else {                  // Left of xmin
      y = (dx != 0) ? (int16_t)(y0 + dy * (int32_t)(xmin - x0) / dx) : y0;
      x = xmin;
    }
    
    if (codeOut == code0) {
      x0 = x; y0 = y;
      code0 = outcode(x0, y0);
    } else {
      x1 = x; y1 = y;
      code1 = outcode(x1, y1);
    }
  }
  return false;
}

void OLEDMapRenderer::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                const MapFeatureStyle& style) {
  if (!_display) return;
  
  // Apply content area offset for actual display drawing
  y0 += _offsetY;
  y1 += _offsetY;
  
  // Clip line to content area - prevents drawing into header/footer
  if (!clipToContent(x0, y0, x1, y1)) return;
  
  switch (style.lineStyle) {
    case LINE_SOLID:
      _display->drawLine(x0, y0, x1, y1, SSD1306_WHITE);
      break;
    case LINE_DASHED:
      drawDashedLine(x0, y0, x1, y1, 4);
      break;
    case LINE_DOTTED:
      drawDottedLine(x0, y0, x1, y1, 3);
      break;
    case LINE_NONE:
    default:
      break;
  }
}

void OLEDMapRenderer::drawDashedLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int dashLen) {
  int adx = abs(x1 - x0);
  int ady = abs(y1 - y0);
  int len = (adx > ady) ? (adx + ady / 2) : (ady + adx / 2);  // Fast length approx
  if (len < 1) return;
  
  float invLen = 1.0f / len;
  float dx = (x1 - x0) * invLen;
  float dy = (y1 - y0) * invLen;
  
  float x = x0, y = y0;
  bool draw = true;
  int segLen = 0;
  
  const int16_t ymin = _offsetY, ymax = _offsetY + _height - 1;
  for (int t = 0; t < len; t++) {
    if (draw) {
      int16_t py = (int16_t)y;
      if (py >= ymin && py <= ymax)
        _display->drawPixel((int16_t)x, py, SSD1306_WHITE);
    }
    x += dx;
    y += dy;
    if (++segLen >= dashLen) {
      segLen = 0;
      draw = !draw;
    }
  }
}

void OLEDMapRenderer::drawDottedLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int spacing) {
  int adx = abs(x1 - x0);
  int ady = abs(y1 - y0);
  int len = (adx > ady) ? (adx + ady / 2) : (ady + adx / 2);  // Fast length approx
  if (len < 1) return;
  
  float invLen = 1.0f / len;
  float dx = (x1 - x0) * invLen;
  float dy = (y1 - y0) * invLen;
  
  const int16_t ymin2 = _offsetY, ymax2 = _offsetY + _height - 1;
  for (int t = 0; t <= len; t += spacing) {
    int16_t px = x0 + (int16_t)(dx * t);
    int16_t py = y0 + (int16_t)(dy * t);
    if (py >= ymin2 && py <= ymax2)
      _display->drawPixel(px, py, SSD1306_WHITE);
  }
}

void OLEDMapRenderer::drawPositionMarker(int16_t x, int16_t y) {
  if (!_display) return;
  
  // Apply content area offset
  y += _offsetY;
  
  // Clip marker to content area
  const int16_t ymin = _offsetY, ymax = _offsetY + _height - 1;
  if (y < ymin || y > ymax) return;
  
  // Draw crosshair (clamp vertical line to content area)
  _display->drawLine(x - 4, y, x + 4, y, SSD1306_WHITE);
  int16_t yt = (y - 4 < ymin) ? ymin : y - 4;
  int16_t yb = (y + 4 > ymax) ? ymax : y + 4;
  _display->drawLine(x, yt, x, yb, SSD1306_WHITE);
  
  // Draw circle around crosshair (only if fully within content area)
  if (y - 3 >= ymin && y + 3 <= ymax)
    _display->drawCircle(x, y, 3, SSD1306_WHITE);
}

void OLEDMapRenderer::drawOverlayText(int16_t x, int16_t y, const char* text, bool inverted) {
  if (!_display) return;
  
  _display->setCursor(x, y + _offsetY);
  if (inverted) {
    _display->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  } else {
    _display->setTextColor(SSD1306_WHITE);
  }
  _display->print(text);
  _display->setTextColor(SSD1306_WHITE);  // Reset
}

void OLEDMapRenderer::drawContextBar(const char* text, int scrollOffset) {
  if (!_display || !text) return;
  
  // Context bar at top of content area (8 pixels high)
  // Draw inverted bar background
  _display->fillRect(0, _offsetY, _width, 8, SSD1306_WHITE);
  
  // Set text color to black on white
  _display->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  _display->setTextSize(1);
  
  // Calculate text width and apply scroll offset
  int textLen = strlen(text);
  int textWidth = textLen * 6; // 6 pixels per char at size 1
  
  // If text is wider than screen, scroll it
  int x = -scrollOffset;
  if (textWidth > _width) {
    // Wrap around for continuous scrolling
    x = x % (textWidth + 20); // +20 for gap before repeat
    if (x > 0) x -= (textWidth + 20);
  } else {
    // Center if fits on screen
    x = (_width - textWidth) / 2;
  }
  
  _display->setCursor(x, _offsetY);
  _display->print(text);
  
  // If scrolling and text wrapped, draw it again for seamless loop
  if (textWidth > _width && x < -20) {
    _display->setCursor(x + textWidth + 20, _offsetY);
    _display->print(text);
  }
  
  _display->setTextColor(SSD1306_WHITE);  // Reset
}

void OLEDMapRenderer::flush() {
  // Display update is handled by OLED mode system
}

MapFeatureStyle OLEDMapRenderer::getFeatureStyle(MapFeatureType type) {
  // OLED-optimized styles - ALL features rendered as white lines
  switch (type) {
    case MAP_FEATURE_HIGHWAY:
      return {LINE_SOLID, 1, 10, true, 0xFFFF};
    case MAP_FEATURE_ROAD_MAJOR:
      return {LINE_SOLID, 1, 9, true, 0xFFFF};
    case MAP_FEATURE_ROAD_MINOR:
      return {LINE_SOLID, 1, 5, true, 0xFFFF};  // Solid thin line
    case MAP_FEATURE_PATH:
      return {LINE_DOTTED, 1, 3, true, 0xFFFF};
    case MAP_FEATURE_WATER:
      return {LINE_SOLID, 1, 8, true, 0xFFFF};   // Rivers only (lakes/coastlines filtered by shouldRenderFeature)
    case MAP_FEATURE_PARK:
      return {LINE_NONE, 1, 0, false, 0xFFFF};   // Skip: polygon fill only (meaningless as wireframe)
    case MAP_FEATURE_LAND_MASK:
      return {LINE_NONE, 1, 0, false, 0xFFFF};   // Skip: land mask is only meaningful as filled region
    case MAP_FEATURE_RAILWAY:
      return {LINE_DASHED, 1, 7, true, 0xFFFF};
    case MAP_FEATURE_BUS:
      return {LINE_DASHED, 1, 4, true, 0xFFFF};  // Dashed for bus routes
    case MAP_FEATURE_FERRY:
      return {LINE_DASHED, 1, 6, true, 0xFFFF};  // Dashed for ferries
    case MAP_FEATURE_BUILDING:
      return {LINE_DOTTED, 1, 1, true, 0xFFFF};  // Dotted for buildings
    case MAP_FEATURE_STATION:
      return {LINE_SOLID, 1, 7, true, 0xFFFF};   // Solid for stations (drawn as point)
    default:
      return {LINE_SOLID, 1, 5, true, 0xFFFF};
  }
}

bool OLEDMapRenderer::shouldRenderFeature(uint8_t type, uint8_t subtype) {
  switch (type) {
    case MAP_FEATURE_WATER:
      // Only render rivers (linear feature) — skip lakes/coastlines (polygon outlines)
      return (subtype == SUBTYPE_WATER_RIVER);
    case MAP_FEATURE_PARK:
    case MAP_FEATURE_LAND_MASK:
      return false;  // Never render polygon fills as wireframes
    default:
      return true;
  }
}

#endif // ENABLE_OLED_DISPLAY

// =============================================================================
// Command Handlers
// =============================================================================

const char* cmd_map(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  const LoadedMap& currentMap = MapCore::getCurrentMap();
  
  if (!currentMap.valid) {
    return "No map loaded. Use 'mapload <path>' or upload to /maps/";
  }
  
  snprintf(getDebugBuffer(), 1024,
           "Map: %s\n"
           "Region: %.8s\n"
           "Features: %lu\n"
           "Size: %zu bytes\n"
           "Bounds: %.4f,%.4f to %.4f,%.4f",
           currentMap.filename,
           currentMap.header.regionName,
           currentMap.header.featureCount,
           currentMap.fileSize,
           currentMap.header.minLat / 1000000.0f,
           currentMap.header.minLon / 1000000.0f,
           currentMap.header.maxLat / 1000000.0f,
           currentMap.header.maxLon / 1000000.0f);
  
  return getDebugBuffer();
}

const char* cmd_mapload(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Usage: mapload <path>";
  const char* p = _arg.c_str();
  
  if (MapCore::loadMapFile(p)) {
    const LoadedMap& currentMap = MapCore::getCurrentMap();
    if (!ensureDebugBuffer()) return "Map loaded";
    snprintf(getDebugBuffer(), 1024, "Loaded: %s (%lu features)", 
             currentMap.filename, currentMap.header.featureCount);
    return getDebugBuffer();
  }
  
  return "Failed to load map";
}

const char* cmd_whereami(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  const LocationContext& ctx = LocationContextManager::getContext();
  
  if (!ctx.valid) {
    return "Location context not available. Need GPS fix and loaded map.";
  }
  
  char* buf = getDebugBuffer();
  int pos = 0;
  
  if (ctx.nearestRoad[0] != '\0') {
    pos += snprintf(buf + pos, 1024 - pos, "Road: %s (%.0fm)\n", 
                    ctx.nearestRoad, ctx.roadDistanceM);
  }
  
  if (ctx.nearestArea[0] != '\0') {
    pos += snprintf(buf + pos, 1024 - pos, "Near: %s (%.0fm)\n", 
                    ctx.nearestArea, ctx.areaDistanceM);
  }
  
  if (pos == 0) {
    return "No nearby features found";
  }
  
  return buf;
}

const char* cmd_search(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Usage: search <name>";
  const char* p = _arg.c_str();
  
  if (!MapCore::hasValidMap()) {
    return "No map loaded";
  }
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  // Search through all feature names
  const LoadedMap& map = MapCore::getCurrentMap();
  char* buf = getDebugBuffer();
  int pos = 0;
  int found = 0;
  
  // Convert search term to lowercase for case-insensitive search
  String searchTerm = String(p);
  searchTerm.toLowerCase();
  
  for (uint16_t i = 0; i < map.nameCount && found < 10; i++) {
    const char* name = MapCore::getName(i);
    if (!name) continue;
    
    String nameLower = String(name);
    nameLower.toLowerCase();
    
    if (nameLower.indexOf(searchTerm) >= 0) {
      pos += snprintf(buf + pos, 1024 - pos, "%s\n", name);
      found++;
    }
  }
  
  if (found == 0) {
    return "No matches found";
  }
  
  if (found >= 10) {
    pos += snprintf(buf + pos, 1024 - pos, "...and more");
  }
  
  return buf;
}

const char* cmd_maplist(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  char maps[8][96];
  int count = MapCore::getAvailableMaps(maps, 8);
  
  if (count == 0) {
    return "No maps found in /maps/";
  }
  
  char* buf = getDebugBuffer();
  int offset = snprintf(buf, 1024, "Available maps:\n");
  
  for (int i = 0; i < count && offset < 900; i++) {
    offset += snprintf(buf + offset, 1024 - offset, "  /maps/%s\n", maps[i]);
  }
  
  return buf;
}

// =============================================================================
// WaypointManager Implementation
// =============================================================================

static void sanitizeWaypointTextCopy(char* dst, size_t dstSize, const char* src, const char* fallback, bool allowNewlines) {
  if (!dst || dstSize == 0) return;
  const char* in = src ? src : "";
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < dstSize; i++) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    if (allowNewlines && c == '\n') {
      dst[j++] = '\n';
      continue;
    }
    if (c < 0x20 || c == 0x7F) continue;
    dst[j++] = static_cast<char>(c);
  }
  dst[j] = '\0';

  if (fallback && fallback[0] && dst[0] == '\0') {
    strlcpy(dst, fallback, dstSize);
  }
}

Waypoint WaypointManager::_waypoints[MAX_WAYPOINTS] = {};
int WaypointManager::_selectedTarget = -1;

bool WaypointManager::loadWaypoints() {
  const LoadedMap& map = MapCore::getCurrentMap();
  if (!map.valid) return false;

  FsLockGuard fsGuard("WaypointManager.loadWaypoints");
  
  String mapPath = String(map.filepath);
  int slash = mapPath.lastIndexOf('/');
  String mapDir = (slash > 0) ? mapPath.substring(0, slash) : String("/maps");
  
  // Extract map base name from filepath (e.g., "/maps/staten/staten.hwmap" -> "staten")
  String mapFileName = mapPath.substring(slash + 1);
  String mapBase = mapFileName;
  if (mapBase.endsWith(".hwmap")) {
    mapBase = mapBase.substring(0, mapBase.length() - 6);
  }
  
  // Try to find waypoints file with pattern: waypoints_<mapbase>.json or waypoints_<mapbase>.hwmap.json
  char wp1[128], wp2[128], wp3[128];
  snprintf(wp1, sizeof(wp1), "%s/waypoints_%s.hwmap.json", mapDir.c_str(), mapBase.c_str());
  snprintf(wp2, sizeof(wp2), "%s/waypoints_%s.json", mapDir.c_str(), mapBase.c_str());
  snprintf(wp3, sizeof(wp3), "%s/waypoints.json", mapDir.c_str());
  String wpPathStr1 = wp1;
  String wpPathStr2 = wp2;
  String wpPathStr3 = wp3;  // Fallback to old format
  
  String wpPathStr;
  if (LittleFS.exists(wpPathStr1.c_str())) {
    wpPathStr = wpPathStr1;
  } else if (LittleFS.exists(wpPathStr2.c_str())) {
    wpPathStr = wpPathStr2;
  } else if (LittleFS.exists(wpPathStr3.c_str())) {
    wpPathStr = wpPathStr3;
  } else {
    // No waypoints file for this map — clear any stale data from a previous map
    memset(_waypoints, 0, sizeof(_waypoints));
    _selectedTarget = -1;
    return false;
  }
  
  char wpPath[128];
  strlcpy(wpPath, wpPathStr.c_str(), sizeof(wpPath));
  
  File f = LittleFS.open(wpPath, "r");
  if (!f) return false;
  
  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  
  if (err) {
    WARN_SENSORSF("Waypoint JSON parse error: %s", err.c_str());
    return false;
  }
  
  // Clear existing
  memset(_waypoints, 0, sizeof(_waypoints));
  _selectedTarget = -1;
  
  JsonArray arr = doc["waypoints"].as<JsonArray>();
  int i = 0;
  for (JsonObject wp : arr) {
    if (i >= MAX_WAYPOINTS) break;
    _waypoints[i].lat = wp["lat"] | 0.0f;
    _waypoints[i].lon = wp["lon"] | 0.0f;
    sanitizeWaypointTextCopy(_waypoints[i].name, WAYPOINT_NAME_LEN, wp["name"] | "WP", "WP", false);
    sanitizeWaypointTextCopy(_waypoints[i].notes, WAYPOINT_NOTES_LEN, wp["notes"] | "", "", true);
    _waypoints[i].active = true;
    
    // Load files array
    _waypoints[i].fileCount = 0;
    memset(_waypoints[i].files, 0, sizeof(_waypoints[i].files));
    if (wp["files"].is<JsonArray>()) {
      JsonArray files = wp["files"].as<JsonArray>();
      for (JsonVariant file : files) {
        if (_waypoints[i].fileCount >= MAX_WAYPOINT_FILES) break;
        const char* path = file.as<const char*>();
        if (path && path[0]) {
          sanitizeWaypointTextCopy(_waypoints[i].files[_waypoints[i].fileCount], WAYPOINT_FILE_PATH_LEN, path, "", false);
          _waypoints[i].fileCount++;
        }
      }
    }
    i++;
  }
  
  _selectedTarget = doc["target"] | -1;
  if (_selectedTarget >= MAX_WAYPOINTS || (_selectedTarget >= 0 && !_waypoints[_selectedTarget].active)) {
    _selectedTarget = -1;
  }
  
  INFO_SENSORSF("Loaded %d waypoints", i);
  return true;
}

bool WaypointManager::saveWaypoints() {
  const LoadedMap& map = MapCore::getCurrentMap();
  if (!map.valid) return false;

  FsLockGuard fsGuard("WaypointManager.saveWaypoints");
  
  PSRAM_JSON_DOC(doc);
  JsonArray arr = doc["waypoints"].to<JsonArray>();
  
  for (int i = 0; i < MAX_WAYPOINTS; i++) {
    if (_waypoints[i].active) {
      JsonObject wp = arr.add<JsonObject>();
      wp["lat"] = _waypoints[i].lat;
      wp["lon"] = _waypoints[i].lon;
      wp["name"] = _waypoints[i].name;
      wp["notes"] = _waypoints[i].notes;
      
      // Save files array if any
      if (_waypoints[i].fileCount > 0) {
        JsonArray files = wp["files"].to<JsonArray>();
        for (int j = 0; j < _waypoints[i].fileCount && j < MAX_WAYPOINT_FILES; j++) {
          if (_waypoints[i].files[j][0]) {
            files.add(_waypoints[i].files[j]);
          }
        }
      }
    }
  }
  
  doc["target"] = _selectedTarget;
  
  String mapPath = String(map.filepath);
  int slash = mapPath.lastIndexOf('/');
  String mapDir = (slash > 0) ? mapPath.substring(0, slash) : String("/maps");
  if (!LittleFS.exists(mapDir)) {
    LittleFS.mkdir(mapDir);
  }
  
  // Extract map base name and save with pattern: waypoints_<mapbase>.json
  String mapFileName = mapPath.substring(slash + 1);
  String mapBase = mapFileName;
  if (mapBase.endsWith(".hwmap")) {
    mapBase = mapBase.substring(0, mapBase.length() - 6);
  }
  
  char wpPath[128];
  snprintf(wpPath, sizeof(wpPath), "%s/waypoints_%s.json", mapDir.c_str(), mapBase.c_str());
  
  File f = LittleFS.open(wpPath, "w");
  if (!f) {
    ERROR_SENSORSF("Failed to write waypoints file: %s", wpPath);
    return false;
  }
  
  serializeJson(doc, f);
  f.close();
  return true;
}

int WaypointManager::addWaypoint(float lat, float lon, const char* name) {
  for (int i = 0; i < MAX_WAYPOINTS; i++) {
    if (!_waypoints[i].active) {
      _waypoints[i].lat = lat;
      _waypoints[i].lon = lon;
      sanitizeWaypointTextCopy(_waypoints[i].name, WAYPOINT_NAME_LEN, name, "WP", false);
      _waypoints[i].notes[0] = '\0';
      _waypoints[i].fileCount = 0;
      memset(_waypoints[i].files, 0, sizeof(_waypoints[i].files));
      _waypoints[i].active = true;
      saveWaypoints();
      return i;
    }
  }
  return -1;  // No free slots
}

int WaypointManager::addWaypoint(float lat, float lon, const char* name, const char* notes) {
  for (int i = 0; i < MAX_WAYPOINTS; i++) {
    if (!_waypoints[i].active) {
      _waypoints[i].lat = lat;
      _waypoints[i].lon = lon;
      sanitizeWaypointTextCopy(_waypoints[i].name, WAYPOINT_NAME_LEN, name, "WP", false);
      sanitizeWaypointTextCopy(_waypoints[i].notes, WAYPOINT_NOTES_LEN, notes ? notes : "", "", true);
      _waypoints[i].fileCount = 0;
      memset(_waypoints[i].files, 0, sizeof(_waypoints[i].files));
      _waypoints[i].active = true;
      saveWaypoints();
      return i;
    }
  }
  return -1;
}

bool WaypointManager::setNotes(int index, const char* notes) {
  if (index < 0 || index >= MAX_WAYPOINTS) return false;
  if (!_waypoints[index].active) return false;
  sanitizeWaypointTextCopy(_waypoints[index].notes, WAYPOINT_NOTES_LEN, notes ? notes : "", "", true);
  saveWaypoints();
  return true;
}

bool WaypointManager::setName(int index, const char* name) {
  if (index < 0 || index >= MAX_WAYPOINTS) return false;
  if (!_waypoints[index].active) return false;
  sanitizeWaypointTextCopy(_waypoints[index].name, WAYPOINT_NAME_LEN, name ? name : "WP", "WP", false);
  saveWaypoints();
  return true;
}

// File attachment management methods
bool WaypointManager::addFile(int waypointIndex, const char* filePath) {
  if (waypointIndex < 0 || waypointIndex >= MAX_WAYPOINTS) return false;
  if (!_waypoints[waypointIndex].active) return false;
  if (!filePath || !filePath[0]) return false;
  if (_waypoints[waypointIndex].fileCount >= MAX_WAYPOINT_FILES) return false;

  char sanitized[WAYPOINT_FILE_PATH_LEN];
  sanitizeWaypointTextCopy(sanitized, sizeof(sanitized), filePath, "", false);
  if (!sanitized[0]) return false;
  
  // Check if file already exists
  for (int i = 0; i < _waypoints[waypointIndex].fileCount; i++) {
    if (strcmp(_waypoints[waypointIndex].files[i], sanitized) == 0) {
      return false;  // Already linked
    }
  }
  
  strlcpy(_waypoints[waypointIndex].files[_waypoints[waypointIndex].fileCount], 
          sanitized, WAYPOINT_FILE_PATH_LEN);
  _waypoints[waypointIndex].fileCount++;
  saveWaypoints();
  return true;
}

bool WaypointManager::removeFile(int waypointIndex, int fileIndex) {
  if (waypointIndex < 0 || waypointIndex >= MAX_WAYPOINTS) return false;
  if (!_waypoints[waypointIndex].active) return false;
  if (fileIndex < 0 || fileIndex >= _waypoints[waypointIndex].fileCount) return false;
  
  // Shift remaining files down
  for (int i = fileIndex; i < _waypoints[waypointIndex].fileCount - 1; i++) {
    strlcpy(_waypoints[waypointIndex].files[i], 
            _waypoints[waypointIndex].files[i + 1], WAYPOINT_FILE_PATH_LEN);
  }
  _waypoints[waypointIndex].fileCount--;
  _waypoints[waypointIndex].files[_waypoints[waypointIndex].fileCount][0] = '\0';
  saveWaypoints();
  return true;
}

int WaypointManager::getFileCount(int waypointIndex) {
  if (waypointIndex < 0 || waypointIndex >= MAX_WAYPOINTS) return 0;
  if (!_waypoints[waypointIndex].active) return 0;
  return _waypoints[waypointIndex].fileCount;
}

const char* WaypointManager::getFile(int waypointIndex, int fileIndex) {
  if (waypointIndex < 0 || waypointIndex >= MAX_WAYPOINTS) return nullptr;
  if (!_waypoints[waypointIndex].active) return nullptr;
  if (fileIndex < 0 || fileIndex >= _waypoints[waypointIndex].fileCount) return nullptr;
  return _waypoints[waypointIndex].files[fileIndex];
}

int WaypointManager::findWaypointByName(const char* name) {
  if (!name || !name[0]) return -1;
  for (int i = 0; i < MAX_WAYPOINTS; i++) {
    if (_waypoints[i].active && strcasecmp(_waypoints[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

bool WaypointManager::clearAll() {
  bool hadAny = false;
  for (int i = 0; i < MAX_WAYPOINTS; i++) {
    if (_waypoints[i].active) {
      _waypoints[i].active = false;
      hadAny = true;
    }
  }
  _selectedTarget = -1;
  if (hadAny) saveWaypoints();
  return true;
}

bool WaypointManager::deleteWaypoint(int index) {
  if (index < 0 || index >= MAX_WAYPOINTS) return false;
  if (!_waypoints[index].active) return false;
  
  _waypoints[index].active = false;
  if (_selectedTarget == index) _selectedTarget = -1;
  saveWaypoints();
  return true;
}

const Waypoint* WaypointManager::getWaypoint(int index) {
  if (index < 0 || index >= MAX_WAYPOINTS) return nullptr;
  if (!_waypoints[index].active) return nullptr;
  return &_waypoints[index];
}

int WaypointManager::getActiveCount() {
  int count = 0;
  for (int i = 0; i < MAX_WAYPOINTS; i++) {
    if (_waypoints[i].active) count++;
  }
  return count;
}

void WaypointManager::selectTarget(int index) {
  if (index < 0 || index >= MAX_WAYPOINTS) {
    _selectedTarget = -1;
  } else if (_waypoints[index].active) {
    _selectedTarget = index;
  } else {
    _selectedTarget = -1;
  }
  saveWaypoints();
}

bool WaypointManager::getDistanceBearing(float fromLat, float fromLon,
                                          float& distanceM, float& bearingDeg) {
  if (_selectedTarget < 0 || !_waypoints[_selectedTarget].active) {
    return false;
  }
  
  const Waypoint& wp = _waypoints[_selectedTarget];
  
  // Haversine distance
  const float R = 6371000.0f;  // Earth radius in meters
  float lat1 = fromLat * PI / 180.0f;
  float lat2 = wp.lat * PI / 180.0f;
  float dLat = (wp.lat - fromLat) * PI / 180.0f;
  float dLon = (wp.lon - fromLon) * PI / 180.0f;
  
  float a = sinf(dLat/2) * sinf(dLat/2) +
            cosf(lat1) * cosf(lat2) * sinf(dLon/2) * sinf(dLon/2);
  float c = 2 * atan2f(sqrtf(a), sqrtf(1-a));
  distanceM = R * c;
  
  // Bearing
  float y = sinf(dLon) * cosf(lat2);
  float x = cosf(lat1) * sinf(lat2) - sinf(lat1) * cosf(lat2) * cosf(dLon);
  bearingDeg = atan2f(y, x) * 180.0f / PI;
  if (bearingDeg < 0) bearingDeg += 360.0f;
  
  return true;
}

void WaypointManager::renderWaypoints(MapRenderer* renderer,
                                       float centerLat, float centerLon,
                                       int32_t scaleX, int32_t scaleY) {
  int viewWidth = renderer->getWidth();
  int viewHeight = renderer->getHeight();
  int32_t centerLatMicro = (int32_t)(centerLat * 1000000);
  int32_t centerLonMicro = (int32_t)(centerLon * 1000000);
  
  for (int i = 0; i < MAX_WAYPOINTS; i++) {
    if (!_waypoints[i].active) continue;
    
    int32_t wpLatMicro = (int32_t)(_waypoints[i].lat * 1000000);
    int32_t wpLonMicro = (int32_t)(_waypoints[i].lon * 1000000);
    
    int16_t screenX, screenY;
    MapCore::geoToScreen(wpLatMicro, wpLonMicro, centerLatMicro, centerLonMicro,
                         scaleX, scaleY, viewWidth, viewHeight, screenX, screenY);
    
    // Only render if on screen
    if (screenX >= 0 && screenX < viewWidth && screenY >= 0 && screenY < viewHeight) {
      // Draw waypoint marker: X shape, or filled for selected target
      bool isTarget = (i == _selectedTarget);
      if (isTarget) {
        // Filled diamond for target
        MapFeatureStyle style = {LINE_SOLID, 1, 15, true, 0xFFFF};
        renderer->drawLine(screenX - 3, screenY, screenX, screenY - 3, style);
        renderer->drawLine(screenX, screenY - 3, screenX + 3, screenY, style);
        renderer->drawLine(screenX + 3, screenY, screenX, screenY + 3, style);
        renderer->drawLine(screenX, screenY + 3, screenX - 3, screenY, style);
      } else {
        // Small X for regular waypoints
        MapFeatureStyle style = {LINE_SOLID, 1, 15, true, 0xFFFF};
        renderer->drawLine(screenX - 2, screenY - 2, screenX + 2, screenY + 2, style);
        renderer->drawLine(screenX - 2, screenY + 2, screenX + 2, screenY - 2, style);
      }
    }
  }
}

// =============================================================================
// GPS Track Manager Implementation
// =============================================================================

GPSTrackPoint* GPSTrackManager::_points = nullptr;
int GPSTrackManager::_pointCount = 0;
GPSTrackBounds GPSTrackManager::_bounds = {0, 0, 0, 0, false};
GPSTrackStats GPSTrackManager::_stats = {0, 0, 0, false};
char GPSTrackManager::_filename[64] = "";
bool GPSTrackManager::_liveTracking = false;
uint32_t GPSTrackManager::_lastUpdateMs = 0;

// Haversine formula for distance between two GPS points (returns meters)
float GPSTrackManager::haversineDistance(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371000.0f;  // Earth radius in meters
  float dLat = (lat2 - lat1) * M_PI / 180.0f;
  float dLon = (lon2 - lon1) * M_PI / 180.0f;
  float lat1Rad = lat1 * M_PI / 180.0f;
  float lat2Rad = lat2 * M_PI / 180.0f;
  
  float a = sinf(dLat / 2) * sinf(dLat / 2) +
            cosf(lat1Rad) * cosf(lat2Rad) * sinf(dLon / 2) * sinf(dLon / 2);
  float c = 2 * atan2f(sqrtf(a), sqrtf(1 - a));
  
  return R * c;
}

// Calculate track statistics (total distance point-to-point, duration, avg speed)
void GPSTrackManager::calculateStats() {
  _stats.valid = false;
  _stats.totalDistanceM = 0;
  _stats.durationSec = 0;
  _stats.avgSpeedMps = 0;
  
  if (_pointCount < 2) return;
  
  // Sum distances between consecutive points
  for (int i = 1; i < _pointCount; i++) {
    float dist = haversineDistance(
      _points[i-1].lat, _points[i-1].lon,
      _points[i].lat, _points[i].lon
    );
    _stats.totalDistanceM += dist;
  }
  
  // Duration from first to last point (using timestamps if available)
  if (_points[_pointCount - 1].timestamp > _points[0].timestamp) {
    _stats.durationSec = (_points[_pointCount - 1].timestamp - _points[0].timestamp) / 1000.0f;
  } else {
    // Estimate based on point count and typical logging interval (1 second)
    _stats.durationSec = (float)(_pointCount - 1);
  }
  
  // Average speed
  if (_stats.durationSec > 0) {
    _stats.avgSpeedMps = _stats.totalDistanceM / _stats.durationSec;
  }
  
  _stats.valid = true;
}

bool GPSTrackManager::parseGPSLine(const char* line, float& lat, float& lon) {
  // Skip comment lines
  if (line[0] == '#') return false;
  
  // Skip signal loss/regain markers (contain "---" or "~~~")
  if (strstr(line, "SIGNAL_LOST") || strstr(line, "SIGNAL_REGAINED")) {
    return false;
  }
  
  // Try Format 1: General sensor log
  // "gps: lat=37.123456 lon=-122.123456 alt=10.5m speed=0.0kn sats=8 q=1"
  const char* latPtr = strstr(line, "lat=");
  const char* lonPtr = strstr(line, "lon=");
  
  if (latPtr && lonPtr) {
    lat = atof(latPtr + 4);
    lon = atof(lonPtr + 4);
  } else {
    // Try Format 2: Dedicated GPS track CSV
    // "HH:MM:SS,lat,lon,alt_m,speed_kn,satellites" (new format with time)
    // "timestamp_ms,lat,lon,alt_m,speed_kn,satellites" (old format with millis)
    // e.g., "14:30:45,37.123456,-122.123456,10.5,0.0,8"
    char* endptr;
    
    // Skip timestamp (first field - either HH:MM:SS or milliseconds)
    const char* p = strchr(line, ',');
    if (!p) return false;
    p++;  // Skip comma
    
    // Check for signal markers (second field is "---" or "~~~")
    if (*p == '-' || *p == '~') return false;
    
    // Parse lat
    lat = strtof(p, &endptr);
    if (endptr == p || *endptr != ',') return false;
    p = endptr + 1;
    
    // Parse lon
    lon = strtof(p, &endptr);
    if (endptr == p) return false;
  }
  
  // Basic sanity check
  if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
    return false;
  }
  
  return true;
}

void GPSTrackManager::calculateBounds() {
  if (_pointCount == 0) {
    _bounds.valid = false;
    return;
  }
  
  _bounds.minLat = _points[0].lat;
  _bounds.maxLat = _points[0].lat;
  _bounds.minLon = _points[0].lon;
  _bounds.maxLon = _points[0].lon;
  
  for (int i = 1; i < _pointCount; i++) {
    if (_points[i].lat < _bounds.minLat) _bounds.minLat = _points[i].lat;
    if (_points[i].lat > _bounds.maxLat) _bounds.maxLat = _points[i].lat;
    if (_points[i].lon < _bounds.minLon) _bounds.minLon = _points[i].lon;
    if (_points[i].lon > _bounds.maxLon) _bounds.maxLon = _points[i].lon;
  }
  
  _bounds.valid = true;
}

bool GPSTrackManager::loadTrack(const char* filepath, String& errorMsg) {
  clearTrack();

  FsLockGuard fsGuard("GPSTrackManager.loadTrack");
  
  if (!LittleFS.exists(filepath)) {
    errorMsg = "File not found";
    return false;
  }
  
  // Allocate track points array in PSRAM if available
  _points = (GPSTrackPoint*)ps_alloc(MAX_TRACK_POINTS * sizeof(GPSTrackPoint), 
                                      AllocPref::PreferPSRAM, "gps.track");
  if (!_points) {
    errorMsg = "Memory allocation failed";
    return false;
  }
  
  File f = LittleFS.open(filepath, "r");
  if (!f) {
    free(_points);
    _points = nullptr;
    errorMsg = "Failed to open file";
    return false;
  }
  
  _pointCount = 0;
  while (f.available() && _pointCount < MAX_TRACK_POINTS) {
    String line = f.readStringUntil('\n');
    line.trim();
    
    if (line.length() == 0) continue;
    
    float lat, lon;
    if (parseGPSLine(line.c_str(), lat, lon)) {
      _points[_pointCount].lat = lat;
      _points[_pointCount].lon = lon;
      _points[_pointCount].timestamp = 0;  // Could parse from log if needed
      _pointCount++;
    }
  }
  
  f.close();
  
  if (_pointCount == 0) {
    free(_points);
    _points = nullptr;
    errorMsg = "No GPS data found in file";
    return false;
  }
  
  strlcpy(_filename, filepath, sizeof(_filename));
  calculateBounds();
  calculateStats();
  
  INFO_SENSORSF("Loaded GPS track: %d points from %s", _pointCount, filepath);
  return true;
}

void GPSTrackManager::clearTrack() {
  if (_points) {
    free(_points);
    _points = nullptr;
  }
  _pointCount = 0;
  _bounds.valid = false;
  _stats.valid = false;
  _filename[0] = '\0';
}

bool GPSTrackManager::deleteTrackFile(const char* filepath) {
  if (!filepath || filepath[0] != '/') return false;
  
  // Clear if this is the currently loaded track
  if (strcmp(_filename, filepath) == 0) {
    clearTrack();
  }
  
  fsLock("gpstrack.delete");
  bool success = LittleFS.remove(filepath);
  fsUnlock();
  
  return success;
}

bool GPSTrackManager::saveTrack(char* outPath, size_t outPathSize) {
  if (!_points || _pointCount == 0) return false;

  // Generate timestamped filename
  const char* dir = "/logging_captures/tracks";
  fsLock("gpstrack.save");
  if (!LittleFS.exists(dir)) {
    LittleFS.mkdir(dir);
  }

  time_t now = time(nullptr);
  char timestamp[24];
  if (now > 1609459200) {
    struct tm* ti = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H-%M-%S", ti);
  } else {
    snprintf(timestamp, sizeof(timestamp), "%lu", millis());
  }

  snprintf(outPath, outPathSize, "%s/track-%s.csv", dir, timestamp);

  File f = LittleFS.open(outPath, "w");
  if (!f) {
    fsUnlock();
    return false;
  }

  f.print("# GPS Track\n# time,lat,lon\n");
  char line[64];
  for (int i = 0; i < _pointCount; i++) {
    int len = snprintf(line, sizeof(line), "%d,%.6f,%.6f\n", i, _points[i].lat, _points[i].lon);
    f.write((const uint8_t*)line, len);
  }
  f.close();
  fsUnlock();

  INFO_SENSORSF("Saved GPS track: %d points to %s", _pointCount, outPath);
  return true;
}

void GPSTrackManager::setLiveTracking(bool enabled) {
  if (enabled && !_liveTracking) {
    // Starting live tracking - allocate buffer if not already allocated
    if (!_points) {
      _points = (GPSTrackPoint*)ps_alloc(MAX_TRACK_POINTS * sizeof(GPSTrackPoint), 
                                          AllocPref::PreferPSRAM, "gps.live");
      if (!_points) {
        ERROR_SENSORSF("Failed to allocate live track buffer");
        return;
      }
    }
    _pointCount = 0;
    _bounds.valid = false;
    _stats.valid = false;
    strlcpy(_filename, "[LIVE]", sizeof(_filename));
    INFO_SENSORSF("Live tracking started");
  } else if (!enabled && _liveTracking) {
    INFO_SENSORSF("Live tracking stopped (%d points)", _pointCount);
    // Calculate final stats
    calculateBounds();
    calculateStats();
  }
  _liveTracking = enabled;
}

bool GPSTrackManager::appendPoint(float lat, float lon) {
  if (!_liveTracking || !_points) return false;
  if (_pointCount >= MAX_TRACK_POINTS) return false;
  
  // Skip if too close to last point (avoid clutter)
  if (_pointCount > 0) {
    float dist = haversineDistance(_points[_pointCount-1].lat, _points[_pointCount-1].lon, lat, lon);
    if (dist < 2.0f) return false;  // Less than 2 meters, skip
  }
  
  _points[_pointCount].lat = lat;
  _points[_pointCount].lon = lon;
  _points[_pointCount].timestamp = millis();
  _pointCount++;
  _lastUpdateMs = millis();
  
  // Update bounds incrementally
  if (_pointCount == 1) {
    _bounds.minLat = _bounds.maxLat = lat;
    _bounds.minLon = _bounds.maxLon = lon;
    _bounds.valid = true;
  } else {
    if (lat < _bounds.minLat) _bounds.minLat = lat;
    if (lat > _bounds.maxLat) _bounds.maxLat = lat;
    if (lon < _bounds.minLon) _bounds.minLon = lon;
    if (lon > _bounds.maxLon) _bounds.maxLon = lon;
  }
  
  // Update stats incrementally
  if (_pointCount >= 2) {
    float dist = haversineDistance(_points[_pointCount-2].lat, _points[_pointCount-2].lon, lat, lon);
    _stats.totalDistanceM += dist;
    _stats.durationSec = (_points[_pointCount-1].timestamp - _points[0].timestamp) / 1000.0f;
    if (_stats.durationSec > 0) {
      _stats.avgSpeedMps = _stats.totalDistanceM / _stats.durationSec;
    }
    _stats.valid = true;
  }
  
  return true;
}

TrackValidation GPSTrackManager::validateTrack(float& coveragePercent) {
  if (_pointCount == 0) {
    coveragePercent = 0.0f;
    return TRACK_EMPTY;
  }
  
  if (!MapCore::hasValidMap()) {
    coveragePercent = 0.0f;
    return TRACK_NO_MAP_LOADED;
  }
  
  const LoadedMap& map = MapCore::getCurrentMap();
  float mapMinLat = map.header.minLat / 1000000.0f;
  float mapMaxLat = map.header.maxLat / 1000000.0f;
  float mapMinLon = map.header.minLon / 1000000.0f;
  float mapMaxLon = map.header.maxLon / 1000000.0f;
  
  int pointsInBounds = 0;
  for (int i = 0; i < _pointCount; i++) {
    if (_points[i].lat >= mapMinLat && _points[i].lat <= mapMaxLat &&
        _points[i].lon >= mapMinLon && _points[i].lon <= mapMaxLon) {
      pointsInBounds++;
    }
  }
  
  coveragePercent = (pointsInBounds * 100.0f) / _pointCount;
  
  if (coveragePercent > 90.0f) return TRACK_VALID;
  if (coveragePercent >= 50.0f) return TRACK_PARTIAL;
  return TRACK_OUT_OF_BOUNDS;
}

const char* GPSTrackManager::getValidationMessage(TrackValidation result, float coverage) {
  static char msg[128];
  
  switch (result) {
    case TRACK_VALID:
      snprintf(msg, sizeof(msg), "Track valid (%.0f%% visible)", coverage);
      break;
    case TRACK_PARTIAL:
      snprintf(msg, sizeof(msg), "Warning: Only %.0f%% of track visible on map", coverage);
      break;
    case TRACK_OUT_OF_BOUNDS:
      snprintf(msg, sizeof(msg), "Error: Track outside map bounds (%.0f%% visible)", coverage);
      break;
    case TRACK_NO_MAP_LOADED:
      strcpy(msg, "Error: No map loaded for validation");
      break;
    case TRACK_EMPTY:
      strcpy(msg, "Error: No track loaded");
      break;
    default:
      strcpy(msg, "Unknown validation status");
  }
  
  return msg;
}

void GPSTrackManager::renderTrack(MapRenderer* renderer,
                                   float centerLat, float centerLon,
                                   int32_t scaleX, int32_t scaleY) {
  if (_pointCount < 2) return;
  
  int viewWidth = renderer->getWidth();
  int viewHeight = renderer->getHeight();
  int32_t centerLatMicro = (int32_t)(centerLat * 1000000);
  int32_t centerLonMicro = (int32_t)(centerLon * 1000000);
  
  // Track style: dotted line to distinguish from roads
  MapFeatureStyle trackStyle = {LINE_DOTTED, 2, 12, true, 0xFFFF};
  
  // Draw track as connected line segments
  int16_t prevX = -1, prevY = -1;
  bool prevValid = false;
  
  for (int i = 0; i < _pointCount; i++) {
    int32_t latMicro = (int32_t)(_points[i].lat * 1000000);
    int32_t lonMicro = (int32_t)(_points[i].lon * 1000000);
    
    int16_t screenX, screenY;
    MapCore::geoToScreen(latMicro, lonMicro, centerLatMicro, centerLonMicro,
                         scaleX, scaleY, viewWidth, viewHeight, screenX, screenY);
    
    // Check if point is on screen (with margin)
    bool onScreen = (screenX >= -10 && screenX < viewWidth + 10 &&
                     screenY >= -10 && screenY < viewHeight + 10);
    
    if (onScreen && prevValid) {
      renderer->drawLine(prevX, prevY, screenX, screenY, trackStyle);
    }
    
    prevX = screenX;
    prevY = screenY;
    prevValid = onScreen;
  }
  
  // Draw start marker (small circle)
  if (_pointCount > 0) {
    int32_t startLatMicro = (int32_t)(_points[0].lat * 1000000);
    int32_t startLonMicro = (int32_t)(_points[0].lon * 1000000);
    int16_t startX, startY;
    MapCore::geoToScreen(startLatMicro, startLonMicro, centerLatMicro, centerLonMicro,
                         scaleX, scaleY, viewWidth, viewHeight, startX, startY);
    
    if (startX >= 0 && startX < viewWidth && startY >= 0 && startY < viewHeight) {
      MapFeatureStyle markerStyle = {LINE_SOLID, 1, 14, true, 0xFFFF};
      // Draw small circle for start
      renderer->drawLine(startX - 2, startY, startX + 2, startY, markerStyle);
      renderer->drawLine(startX, startY - 2, startX, startY + 2, markerStyle);
    }
  }
  
  // Draw end marker (small square)
  if (_pointCount > 1) {
    int32_t endLatMicro = (int32_t)(_points[_pointCount - 1].lat * 1000000);
    int32_t endLonMicro = (int32_t)(_points[_pointCount - 1].lon * 1000000);
    int16_t endX, endY;
    MapCore::geoToScreen(endLatMicro, endLonMicro, centerLatMicro, centerLonMicro,
                         scaleX, scaleY, viewWidth, viewHeight, endX, endY);
    
    if (endX >= 0 && endX < viewWidth && endY >= 0 && endY < viewHeight) {
      MapFeatureStyle markerStyle = {LINE_SOLID, 1, 14, true, 0xFFFF};
      // Draw small square for end
      renderer->drawLine(endX - 2, endY - 2, endX + 2, endY - 2, markerStyle);
      renderer->drawLine(endX + 2, endY - 2, endX + 2, endY + 2, markerStyle);
      renderer->drawLine(endX + 2, endY + 2, endX - 2, endY + 2, markerStyle);
      renderer->drawLine(endX - 2, endY + 2, endX - 2, endY - 2, markerStyle);
    }
  }
}

const char* cmd_gpstrack(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  
  // Parse subcommand
  const char* p = argsInput.c_str();
  while (*p && *p != ' ') p++;
  while (*p == ' ') p++;
  
  if (*p == '\0' || strncmp(p, "status", 6) == 0) {
    if (!GPSTrackManager::hasTrack()) {
      return "No GPS track loaded";
    }
    
    int count = GPSTrackManager::getPointCount();
    const GPSTrackBounds& bounds = GPSTrackManager::getBounds();
    const char* filename = GPSTrackManager::getFilename();
    
    float coverage;
    TrackValidation validation = GPSTrackManager::validateTrack(coverage);
    const char* validMsg = GPSTrackManager::getValidationMessage(validation, coverage);
    
    snprintf(buf, 1024, 
             "GPS Track: %s\nPoints: %d\nBounds: %.5f,%.5f to %.5f,%.5f\n%s",
             filename, count, bounds.minLat, bounds.minLon, 
             bounds.maxLat, bounds.maxLon, validMsg);
    return buf;
  }
  
  if (strncmp(p, "load ", 5) == 0) {
    const char* filepath = p + 5;
    String errorMsg;
    
    if (GPSTrackManager::loadTrack(filepath, errorMsg)) {
      float coverage;
      TrackValidation validation = GPSTrackManager::validateTrack(coverage);
      
      if (validation == TRACK_OUT_OF_BOUNDS) {
        GPSTrackManager::clearTrack();
        snprintf(buf, 1024, "Error: Track outside map bounds (%.0f%% visible)", coverage);
        return buf;
      }
      
      const char* validMsg = GPSTrackManager::getValidationMessage(validation, coverage);
      snprintf(buf, 1024, "Loaded %d GPS points\n%s", 
               GPSTrackManager::getPointCount(), validMsg);
      return buf;
    } else {
      snprintf(buf, 1024, "Failed to load track: %s", errorMsg.c_str());
      return buf;
    }
  }
  
  if (strncmp(p, "clear", 5) == 0) {
    GPSTrackManager::clearTrack();
    return "GPS track cleared";
  }
  
  return "Usage: gpstrack [status|load <filepath>|clear]";
}

const char* cmd_waypoint(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  
  // Parse subcommand
  const char* p = argsInput.c_str();
  while (*p && *p != ' ') p++;
  while (*p == ' ') p++;
  
  if (*p == '\0' || strncmp(p, "list", 4) == 0) {
    // List waypoints
    int offset = snprintf(buf, 1024, "Waypoints (%d/%d):\n", 
                          WaypointManager::getActiveCount(), MAX_WAYPOINTS);
    int target = WaypointManager::getSelectedTarget();
    for (int i = 0; i < MAX_WAYPOINTS && offset < 900; i++) {
      const Waypoint* wp = WaypointManager::getWaypoint(i);
      if (wp) {
        offset += snprintf(buf + offset, 1024 - offset, "  %d%s: %s (%.5f, %.5f)\n",
                           i, (i == target) ? "*" : "", wp->name, wp->lat, wp->lon);
      }
    }
    return buf;
  }
  
  if (strncmp(p, "add ", 4) == 0) {
    // waypoint add <lat> <lon> <name>
    float lat, lon;
    char name[WAYPOINT_NAME_LEN];
    if (sscanf(p + 4, "%f %f %11s", &lat, &lon, name) >= 2) {
      if (strlen(name) == 0) strcpy(name, "WP");
      int idx = WaypointManager::addWaypoint(lat, lon, name);
      if (idx >= 0) {
        snprintf(buf, 1024, "Added waypoint %d: %s", idx, name);
      } else {
        return "No free waypoint slots";
      }
    } else {
      return "Usage: waypoint add <lat> <lon> [name]";
    }
    return buf;
  }
  
  if (strncmp(p, "del ", 4) == 0) {
    int idx = atoi(p + 4);
    if (WaypointManager::deleteWaypoint(idx)) {
      snprintf(buf, 1024, "Deleted waypoint %d", idx);
    } else {
      return "Invalid waypoint index";
    }
    return buf;
  }
  
  if (strncmp(p, "goto ", 5) == 0) {
    int idx = atoi(p + 5);
    const Waypoint* wp = WaypointManager::getWaypoint(idx);
    if (wp) {
      WaypointManager::selectTarget(idx);
      snprintf(buf, 1024, "Navigation target: %s", wp->name);
    } else {
      return "Invalid waypoint index";
    }
    return buf;
  }
  
  if (strncmp(p, "clearall", 8) == 0) {
    WaypointManager::clearAll();
    return "All waypoints cleared";
  }

  if (strncmp(p, "clear", 5) == 0) {
    WaypointManager::selectTarget(-1);
    return "Navigation target cleared";
  }

  if (strncmp(p, "rename ", 7) == 0) {
    const char* rest = p + 7;
    char* endptr;
    int idx = (int)strtol(rest, &endptr, 10);
    while (*endptr == ' ') endptr++;
    if (endptr == rest || *endptr == '\0') return "Usage: waypoint rename <index> <name>";
    char nameStr[WAYPOINT_NAME_LEN];
    snprintf(nameStr, sizeof(nameStr), "%s", endptr);
    if (WaypointManager::setName(idx, nameStr)) {
      snprintf(buf, 1024, "Renamed waypoint %d: %s", idx, nameStr);
    } else {
      return "Invalid waypoint index";
    }
    return buf;
  }

  if (strncmp(p, "notes ", 6) == 0) {
    const char* rest = p + 6;
    char* endptr;
    int idx = (int)strtol(rest, &endptr, 10);
    while (*endptr == ' ') endptr++;
    if (endptr == rest || *endptr == '\0') return "Usage: waypoint notes <index> <notes>";
    if (WaypointManager::setNotes(idx, endptr)) {
      snprintf(buf, 1024, "Set notes for waypoint %d", idx);
    } else {
      return "Invalid waypoint index";
    }
    return buf;
  }

  return "Usage: waypoint [list|add|del|goto|clear|clearall|rename|notes]";
}

// Link a file to a waypoint by GPS coordinates (creates waypoint if needed, or finds nearest)
const char* cmd_waypointfile(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  
  // Parse: waypointfile <filepath> <lat> <lon> [waypointName]
  // Or:    waypointfile <filepath> <waypointName>
  const char* p = argsInput.c_str();
  while (*p && *p != ' ') p++;
  while (*p == ' ') p++;
  
  if (*p == '\0') {
    return "Usage: waypointfile <file> <wpName>\n   or: waypointfile <file> <lat> <lon> [wpName]";
  }
  
  char filepath[WAYPOINT_FILE_PATH_LEN];
  float lat, lon;
  char wpName[WAYPOINT_NAME_LEN] = "";
  
  // Try parsing as: filepath lat lon [wpName]
  int argsRead = sscanf(p, "%63s %f %f %11s", filepath, &lat, &lon, wpName);
  
  if (argsRead >= 3) {
    // GPS coords provided - find or create waypoint
    // Verify file exists
    if (!LittleFS.exists(filepath)) {
      snprintf(buf, 1024, "File not found: %s", filepath);
      return buf;
    }
    
    int wpIndex = -1;
    if (wpName[0]) {
      wpIndex = WaypointManager::findWaypointByName(wpName);
      if (wpIndex < 0) {
        wpIndex = WaypointManager::addWaypoint(lat, lon, wpName);
        if (wpIndex < 0) return "No free waypoint slots";
        snprintf(buf, 1024, "Created waypoint '%s' at %.5f, %.5f", wpName, lat, lon);
      }
    } else {
      // Find nearest waypoint within ~100m
      float minDist = 100.0f;
      for (int i = 0; i < MAX_WAYPOINTS; i++) {
        const Waypoint* wp = WaypointManager::getWaypoint(i);
        if (wp) {
          float dlat = (wp->lat - lat) * 111320.0f;
          float dlon = (wp->lon - lon) * 111320.0f * cosf(lat * M_PI / 180.0f);
          float dist = sqrtf(dlat * dlat + dlon * dlon);
          if (dist < minDist) {
            minDist = dist;
            wpIndex = i;
          }
        }
      }
      if (wpIndex < 0) return "No nearby waypoint. Provide a name to create one.";
    }
    
    if (WaypointManager::addFile(wpIndex, filepath)) {
      const Waypoint* wp = WaypointManager::getWaypoint(wpIndex);
      snprintf(buf, 1024, "Linked %s to '%s' (%d files)", 
               filepath, wp ? wp->name : "?", WaypointManager::getFileCount(wpIndex));
    } else {
      const Waypoint* wp = WaypointManager::getWaypoint(wpIndex);
      if (wp && wp->fileCount >= MAX_WAYPOINT_FILES) {
        snprintf(buf, 1024, "'%s' has max files (%d)", wp->name, MAX_WAYPOINT_FILES);
      } else {
        return "Failed to link (already linked?)";
      }
    }
    return buf;
  }
  
  // Try parsing as: filepath wpName
  char wpNameOnly[WAYPOINT_NAME_LEN];
  if (sscanf(p, "%63s %11s", filepath, wpNameOnly) >= 2) {
    if (!LittleFS.exists(filepath)) {
      snprintf(buf, 1024, "File not found: %s", filepath);
      return buf;
    }
    
    int wpIndex = WaypointManager::findWaypointByName(wpNameOnly);
    if (wpIndex < 0) {
      snprintf(buf, 1024, "Waypoint not found: %s", wpNameOnly);
      return buf;
    }
    
    if (WaypointManager::addFile(wpIndex, filepath)) {
      snprintf(buf, 1024, "Linked %s to '%s' (%d files)", 
               filepath, wpNameOnly, WaypointManager::getFileCount(wpIndex));
    } else {
      const Waypoint* wp = WaypointManager::getWaypoint(wpIndex);
      if (wp && wp->fileCount >= MAX_WAYPOINT_FILES) {
        snprintf(buf, 1024, "'%s' has max files (%d)", wpNameOnly, MAX_WAYPOINT_FILES);
      } else {
        return "Failed to link (already linked?)";
      }
    }
    return buf;
  }
  
  return "Usage: waypointfile <file> <wpName>\n   or: waypointfile <file> <lat> <lon> [wpName]";
}

// List or remove files from a waypoint
const char* cmd_waypointfiles(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  
  // Parse: waypointfiles <waypointName> [del <index>]
  const char* p = argsInput.c_str();
  while (*p && *p != ' ') p++;
  while (*p == ' ') p++;
  
  if (*p == '\0') {
    return "Usage: waypointfiles <wpName> [del <index>]";
  }
  
  char wpName[WAYPOINT_NAME_LEN];
  char action[8] = "";
  int fileIdx = -1;
  
  int argsRead = sscanf(p, "%11s %7s %d", wpName, action, &fileIdx);
  if (argsRead < 1) {
    return "Usage: waypointfiles <wpName> [del <index>]";
  }
  
  int wpIndex = WaypointManager::findWaypointByName(wpName);
  if (wpIndex < 0) {
    snprintf(buf, 1024, "Waypoint not found: %s", wpName);
    return buf;
  }
  
  // Delete action
  if (strcmp(action, "del") == 0 && fileIdx >= 0) {
    if (WaypointManager::removeFile(wpIndex, fileIdx)) {
      snprintf(buf, 1024, "Removed file %d from '%s'", fileIdx, wpName);
    } else {
      snprintf(buf, 1024, "Invalid file index %d for '%s'", fileIdx, wpName);
    }
    return buf;
  }
  
  // List files
  int count = WaypointManager::getFileCount(wpIndex);
  if (count == 0) {
    snprintf(buf, 1024, "Waypoint '%s' has no files", wpName);
    return buf;
  }
  
  int offset = snprintf(buf, 1024, "Files for '%s' (%d):\n", wpName, count);
  for (int i = 0; i < count && offset < 900; i++) {
    const char* file = WaypointManager::getFile(wpIndex, i);
    if (file) {
      offset += snprintf(buf + offset, 1024 - offset, "  %d: %s\n", i, file);
    }
  }
  return buf;
}

bool isMapFileByMagic(const String& fullPath) {
  FsLockGuard guard("maps.magic");
  File f = LittleFS.open(fullPath, "r");
  if (!f) return false;
  char magic[4] = {0};
  size_t rd = f.read((uint8_t*)magic, 4);
  f.close();
  return (rd == 4 && memcmp(magic, "HWMP", 4) == 0);
}

static String mapBaseNameNoExt(const String& filename) {
  String base = filename;
  if (base.startsWith("/")) {
    int lastSlash = base.lastIndexOf('/');
    if (lastSlash >= 0) base = base.substring(lastSlash + 1);
  }
  if (base.endsWith(".hwmap")) base = base.substring(0, base.length() - 6);
  return base;
}

bool organizeMapFromAnyPath(const String& srcPath, String& outErr) {
  int lastSlash = srcPath.lastIndexOf('/');
  String fileName = (lastSlash >= 0) ? srcPath.substring(lastSlash + 1) : srcPath;
  String base = mapBaseNameNoExt(fileName);
  if (base.length() == 0) { outErr = "empty_base"; return false; }
  if (!LittleFS.exists(srcPath)) { outErr = "src_missing"; return false; }
  if (!isMapFileByMagic(srcPath)) { outErr = "not_map_file"; return false; }
  if (!LittleFS.exists("/maps")) {
    if (!LittleFS.mkdir("/maps")) { outErr = "maps_mkdir_failed"; return false; }
  }
  char dstDir[64], dstMap[96];
  snprintf(dstDir, sizeof(dstDir), "/maps/%s", base.c_str());
  snprintf(dstMap, sizeof(dstMap), "%s/%s.hwmap", dstDir, base.c_str());
  if (srcPath == dstMap) { outErr = "already_organized"; return false; }
  if (!LittleFS.exists(dstDir)) {
    if (!LittleFS.mkdir(dstDir)) { outErr = "mkdir_failed"; return false; }
  }
  if (LittleFS.exists(dstMap)) { outErr = "dst_exists"; return false; }
  if (!LittleFS.rename(srcPath.c_str(), dstMap)) { outErr = "rename_failed"; return false; }
  return true;
}

bool tryOrganizeLegacyWaypointsAtRoot(const String& wpFileName, String& outErr) {
  if (!wpFileName.startsWith("waypoints_") || !wpFileName.endsWith(".json")) { outErr = "not_waypoints"; return false; }
  if (wpFileName.indexOf('/') >= 0) { outErr = "invalid_name"; return false; }
  String mapFileName = wpFileName.substring(10, wpFileName.length() - 5);
  String base = mapFileName.endsWith(".hwmap") ? mapFileName.substring(0, mapFileName.length() - 6) : mapFileName;
  if (base.length() == 0) { outErr = "empty_base"; return false; }
  char srcWp[96];
  snprintf(srcWp, sizeof(srcWp), "/maps/%s", wpFileName.c_str());
  if (!LittleFS.exists(srcWp)) { outErr = "src_missing"; return false; }
  char dstDir[64], dstWp[128];
  snprintf(dstDir, sizeof(dstDir), "/maps/%s", base.c_str());
  snprintf(dstWp, sizeof(dstWp), "%s/%s", dstDir, wpFileName.c_str());
  if (!LittleFS.exists(dstDir)) { outErr = "dst_dir_missing"; return false; }
  if (LittleFS.exists(dstWp)) { outErr = "dst_exists"; return false; }
  if (!LittleFS.rename(srcWp, dstWp)) { outErr = "rename_failed"; return false; }
  return true;
}

const char* cmd_maporganize(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();

  FsLockGuard guard("cmd_maporganize");
  File dir = LittleFS.open("/maps");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return "Error: /maps directory not found";
  }

  int moved = 0, skipped = 0, failed = 0;
  File entry = dir.openNextFile();
  while (entry) {
    String full = String(entry.name());
    bool isDir = entry.isDirectory();
    entry.close();
    if (!isDir) {
      String fn = full;
      if (fn.startsWith("/maps/")) fn = fn.substring(6);
      if (fn.startsWith("/")) fn = fn.substring(1);
      if (fn.indexOf('/') == -1) {
        bool isMapByExt = fn.endsWith(".hwmap");
        bool isMapByMagic = (!isMapByExt && !fn.endsWith(".json")) ? isMapFileByMagic(full) : false;
        if (isMapByExt || isMapByMagic) {
          String err;
          if (organizeMapFromAnyPath(full, err)) moved++; else failed++;
        } else if (fn.startsWith("waypoints_") && fn.endsWith(".json")) {
          String err;
          if (tryOrganizeLegacyWaypointsAtRoot(fn, err)) moved++; else failed++;
        } else { skipped++; }
      } else { skipped++; }
    } else { skipped++; }
    entry = dir.openNextFile();
  }
  dir.close();
  snprintf(buf, 1024, "Map organize: moved=%d skipped=%d failed=%d", moved, skipped, failed);
  return buf;
}

// Command registry
// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry mapCommands[] = {
  {"map", "Show current map info", false, cmd_map, nullptr},
  {"mapload", "Load map file: <path>", false, cmd_mapload, nullptr},
  {"maplist", "List available maps", false, cmd_maplist, nullptr},
  {"whereami", "Show current location context", false, cmd_whereami, nullptr},
  {"search", "Search map features: <name>", false, cmd_search, nullptr},
  {"waypoint", "Manage waypoints: <list|add|del|goto|clear>", false, cmd_waypoint, nullptr},
  {"gpstrack", "Manage GPS tracks: <status|load|clear>", false, cmd_gpstrack, nullptr},
  {"waypointfile", "Link file to waypoint: <file> <wpName>", false, cmd_waypointfile, nullptr},
  {"waypointfiles", "Waypoint files: <name> [del <idx>]", false, cmd_waypointfiles, nullptr},
  {"maporganize", "Organize map files in /maps into subdirectories", false, cmd_maporganize, nullptr}
};
const size_t mapCommandsCount = sizeof(mapCommands) / sizeof(mapCommands[0]);

// =============================================================================
// LocationContextManager Implementation
// =============================================================================

bool LocationContextManager::shouldUpdate(float lat, float lon) {
  if (!MapCore::hasValidMap()) {
    return false;
  }
  
  uint32_t now = millis();
  
  // Check if enough time has passed
  if (_context.valid && (now - _context.lastUpdateMs) < CONTEXT_UPDATE_INTERVAL_MS) {
    // Also check if we've moved enough
    float dist = haversineDistance(_context.lastLat, _context.lastLon, lat, lon);
    if (dist < CONTEXT_UPDATE_MIN_DISTANCE) {
      return false;
    }
  }
  
  return true;
}

void LocationContextManager::updateContext(float lat, float lon) {
  const LoadedMap& map = MapCore::getCurrentMap();
  if (!map.valid || !map.tileDir) {
    _context.valid = false;
    return;
  }
  
  // Reset context
  _context.nearestRoad[0] = '\0';
  _context.roadDistanceM = 999999.0f;
  _context.nearestArea[0] = '\0';
  _context.areaDistanceM = 999999.0f;
  
  // Convert position to microdegrees for tile lookup
  int32_t latMicro = (int32_t)(lat * 1000000);
  int32_t lonMicro = (int32_t)(lon * 1000000);
  
  // Find which tile contains this position
  int tileX = (lonMicro - map.header.minLon) / map.tileW;
  int tileY = (latMicro - map.header.minLat) / map.tileH;
  
  // Clamp to valid range
  if (tileX < 0) tileX = 0;
  if (tileX >= map.tileGridSize) tileX = map.tileGridSize - 1;
  if (tileY < 0) tileY = 0;
  if (tileY >= map.tileGridSize) tileY = map.tileGridSize - 1;
  
  // Check this tile and adjacent tiles (3x3 neighborhood)
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      int tx = tileX + dx;
      int ty = tileY + dy;
      if (tx < 0 || tx >= map.tileGridSize || ty < 0 || ty >= map.tileGridSize) continue;
      
      uint16_t tileIdx = ty * map.tileGridSize + tx;
      if (tileIdx >= map.tileCount) continue;
      
      HWMapTileDirEntry& tile = map.tileDir[tileIdx];
      if (tile.payloadSize == 0) continue;
      
      // Calculate tile halo bounds for dequantization
      int32_t tileMinLon = map.header.minLon + tx * map.tileW - map.haloW;
      int32_t tileMaxLon = map.header.minLon + (tx + 1) * map.tileW + map.haloW;
      int32_t tileMinLat = map.header.minLat + ty * map.tileH - map.haloH;
      int32_t tileMaxLat = map.header.minLat + (ty + 1) * map.tileH + map.haloH;
      int32_t haloLonSpan = tileMaxLon - tileMinLon;
      int32_t haloLatSpan = tileMaxLat - tileMinLat;
      
      // Load tile data
      size_t tileDataSize;
      const uint8_t* tileData = MapCore::loadTileData(tileIdx, &tileDataSize);
      if (!tileData || tileDataSize == 0) continue;
      
      const uint8_t* ptr = tileData;
      const uint8_t* end = tileData + tileDataSize;
      
      // Feature count is at the START of each tile's payload (2 bytes)
      if (ptr + 2 > end) continue;
      uint16_t featureCount = ptr[0] | (ptr[1] << 8);
      ptr += 2;
      
      for (uint16_t f = 0; f < featureCount; f++) {
        if (ptr + HWMAP_FEATURE_HEADER_SIZE > end) break;
        
        uint8_t ftype = ptr[0];
        // type(1) + subtype(1) + nameIndex(2) + pointCount(2)
        uint16_t nameIndex = ptr[2] | (ptr[3] << 8);
        uint16_t pointCount = ptr[4] | (ptr[5] << 8);
        ptr += HWMAP_FEATURE_HEADER_SIZE;
        
        size_t pointsBytes = pointCount * 4;
        if (ptr + pointsBytes > end) break;
        
        if (pointCount < 2) {
          ptr += pointsBytes;
          continue;
        }
        
        // Only check roads and areas
        bool isRoad = (ftype == MAP_FEATURE_HIGHWAY || ftype == MAP_FEATURE_ROAD_MAJOR || 
                       ftype == MAP_FEATURE_ROAD_MINOR || ftype == MAP_FEATURE_PATH);
        bool isArea = (ftype == MAP_FEATURE_PARK || ftype == MAP_FEATURE_WATER);
        
        if (!isRoad && !isArea) {
          ptr += pointsBytes;
          continue;
        }
        
        // Dequantize first point
        uint16_t qLat = ptr[0] | (ptr[1] << 8);
        uint16_t qLon = ptr[2] | (ptr[3] << 8);
        ptr += 4;
        
        int32_t prevLat = tileMinLat + (int32_t)((int64_t)qLat * haloLatSpan >> 16);
        int32_t prevLon = tileMinLon + (int32_t)((int64_t)qLon * haloLonSpan >> 16);
        
        float minDist = 999999.0f;
        
        for (uint16_t p = 1; p < pointCount; p++) {
          qLat = ptr[0] | (ptr[1] << 8);
          qLon = ptr[2] | (ptr[3] << 8);
          ptr += 4;
          
          int32_t curLat = tileMinLat + (int32_t)((int64_t)qLat * haloLatSpan >> 16);
          int32_t curLon = tileMinLon + (int32_t)((int64_t)qLon * haloLonSpan >> 16);
          
          float dist = pointToSegmentDistance(lat, lon, prevLat, prevLon, curLat, curLon);
          if (dist < minDist) minDist = dist;
          
          prevLat = curLat;
          prevLon = curLon;
        }
        
        if (isRoad && minDist < _context.roadDistanceM) {
          _context.roadDistanceM = minDist;
          _context.roadType = (MapFeatureType)ftype;
          
          if (nameIndex != HWMAP_NO_NAME && nameIndex < map.nameCount && map.names) {
            strncpy(_context.nearestRoad, map.names[nameIndex].name, sizeof(_context.nearestRoad) - 1);
            _context.nearestRoad[sizeof(_context.nearestRoad) - 1] = '\0';
          } else {
            _context.nearestRoad[0] = '\0';
          }
        }
        
        if (isArea && minDist < _context.areaDistanceM) {
          _context.areaDistanceM = minDist;
          _context.areaType = (MapFeatureType)ftype;
          
          if (nameIndex != HWMAP_NO_NAME && nameIndex < map.nameCount && map.names) {
            strncpy(_context.nearestArea, map.names[nameIndex].name, sizeof(_context.nearestArea) - 1);
            _context.nearestArea[sizeof(_context.nearestArea) - 1] = '\0';
          } else {
            _context.nearestArea[0] = '\0';
          }
        }
      }
    }
  }
  
  _context.lastUpdateMs = millis();
  _context.lastLat = lat;
  _context.lastLon = lon;
  _context.valid = true;
}

float LocationContextManager::pointToSegmentDistance(float lat, float lon,
                                                      int32_t lat1, int32_t lon1,
                                                      int32_t lat2, int32_t lon2) {
  // Convert microdegrees to degrees for calculation
  float pLat = lat;
  float pLon = lon;
  float aLat = lat1 / 1000000.0f;
  float aLon = lon1 / 1000000.0f;
  float bLat = lat2 / 1000000.0f;
  float bLon = lon2 / 1000000.0f;
  
  // Vector from A to B
  float abLat = bLat - aLat;
  float abLon = bLon - aLon;
  
  // Vector from A to P
  float apLat = pLat - aLat;
  float apLon = pLon - aLon;
  
  // Project P onto line AB, clamped to segment
  float ab2 = abLat * abLat + abLon * abLon;
  if (ab2 < 0.0000001f) {
    // Segment is a point
    return haversineDistance(pLat, pLon, aLat, aLon);
  }
  
  float t = (apLat * abLat + apLon * abLon) / ab2;
  t = fmaxf(0.0f, fminf(1.0f, t));
  
  // Closest point on segment
  float closestLat = aLat + t * abLat;
  float closestLon = aLon + t * abLon;
  
  return haversineDistance(pLat, pLon, closestLat, closestLon);
}

float LocationContextManager::haversineDistance(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371000.0f;  // Earth radius in meters
  
  float dLat = (lat2 - lat1) * PI / 180.0f;
  float dLon = (lon2 - lon1) * PI / 180.0f;
  
  float a = sinf(dLat / 2) * sinf(dLat / 2) +
            cosf(lat1 * PI / 180.0f) * cosf(lat2 * PI / 180.0f) *
            sinf(dLon / 2) * sinf(dLon / 2);
  
  float c = 2 * atan2f(sqrtf(a), sqrtf(1 - a));
  
  return R * c;
}
