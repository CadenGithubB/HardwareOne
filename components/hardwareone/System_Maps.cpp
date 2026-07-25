#include "System_BuildConfig.h"
#include "System_Filesystem.h"  // requireQuotedPath (uniform quoted-path rule)
#include "System_Maps.h"

#if ENABLE_MAPS

#include <LittleFS.h>
#include <ArduinoJson.h>
#include <cstring>

#include "System_Command.h"
#include "System_Debug.h"
#include "System_VFS.h"
#include "System_AuthIdentity.h"  // currentAuthContext

#include "System_I2C.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_Settings.h"  // gSettings (previously only reached transitively via the
                               // ENABLE_OLED_DISPLAY-gated OLED_Display.h include below —
                               // broke on I2C-disabled builds since gSettings is used
                               // unconditionally throughout this file)
#include "System_Utils.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if ENABLE_OLED_DISPLAY
#include <Adafruit_SSD1306.h>
#include "OLED_Display.h"
#include "System_FileManager.h"
#endif

#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor_seesaw.h"
#endif

#if ENABLE_GPS_SENSOR
#include <Adafruit_GPS.h>
#include "i2csensor_pa1010d.h"
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
// Discrete map panning (shared)
// =============================================================================
// Nudges the map center one step in a screen-space direction. Used by surfaces
// that pan via discrete taps rather than an analog stick (the G2 lens map page's
// Pan N/S/E/W rows). The math mirrors the OLED joystick pan (OLED_Mode_Map.cpp):
// one isotropic degree step scaled by 1/zoom, run through the rotation matrix,
// then clamped to the map bounds with half a viewport of overscroll. OLED keeps
// its own momentum path; this is the reusable "move by one step" core.
void mapPanStep(float dx, float dy, float frac) {
  const LoadedMap& m = MapCore::getCurrentMap();
  if (!m.valid) return;

  const float minLat = m.header.minLat / 1000000.0f;
  const float maxLat = m.header.maxLat / 1000000.0f;
  const float minLon = m.header.minLon / 1000000.0f;
  const float maxLon = m.header.maxLon / 1000000.0f;
  const float spanLat = maxLat - minLat;
  const float spanLon = maxLon - minLon;

  // Isotropic degree step ~ `frac` of the viewport (viewport span ~ map span /
  // zoom), matching the OLED pan which uses a single accel scalar for both axes.
  const float step = frac * 0.5f * (spanLat + spanLon) / gMapZoom;

  // Rotation-aware screen->geo mapping. lat subtracts so the direction stays
  // correct at any gMapRotation (identical sign convention to OLED_Mode_Map).
  const float rad  = -gMapRotation * (float)M_PI / 180.0f;
  const float cosR = cosf(rad);
  const float sinR = sinf(rad);
  gMapCenterLon += (dx * cosR - dy * sinR) * step;
  gMapCenterLat -= (dx * sinR + dy * cosR) * step;

  // Clamp to map bounds, allowing panning up to half a viewport past the edge.
  const float marginLat = spanLat * 0.5f / gMapZoom;
  const float marginLon = spanLon * 0.5f / gMapZoom;
  gMapCenterLat = fmaxf(minLat - marginLat, fminf(maxLat + marginLat, gMapCenterLat));
  gMapCenterLon = fmaxf(minLon - marginLon, fminf(maxLon + marginLon, gMapCenterLon));

  gMapCenterSet      = true;
  gMapManuallyPanned = true;
}

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

// Shade bands (0-15): brightness encodes prominence AND z-order (max-write).
// The G2 lens shows roughly 8 visually distinct levels, so classes sit on a
// coarse ladder (13/11/9/7/5/3) with MAP_SHADE_MAX=15 reserved for overlays.
// Bands, not fine gradations: adjacent nibbles look alike on the lens.
// The OLED crushes shade>0 to white, so these values only affect the G2.
MapFeatureStyle MapRenderer::getFeatureStyle(MapFeatureType type) {
  // Default styles (can be overridden by subclasses)
  switch (type) {
    case MAP_FEATURE_HIGHWAY:
      return {LINE_SOLID, 3, 13, true};   // Brightest feature class, thicker
    case MAP_FEATURE_ROAD_MAJOR:
      return {LINE_SOLID, 2, 11, true};
    case MAP_FEATURE_ROAD_MINOR:
      return {LINE_DASHED, 1, 9, true};
    case MAP_FEATURE_PATH:
      return {LINE_DOTTED, 1, 5, true};
    case MAP_FEATURE_WATER:
      return {LINE_SOLID, 1, 7, true};
    case MAP_FEATURE_PARK:
      return {LINE_DOTTED, 1, 3, false};  // Skip on mono
    case MAP_FEATURE_LAND_MASK:
      return {LINE_DOTTED, 1, 3, true};   // Coastline, thin dotted, dimmest
    case MAP_FEATURE_RAILWAY:
      return {LINE_DASHED, 1, 7, true};
    case MAP_FEATURE_BUS:
      return {LINE_DASHED, 1, 5, true};
    case MAP_FEATURE_FERRY:
      return {LINE_DASHED, 2, 5, true};
    case MAP_FEATURE_BUILDING:
      return {LINE_NONE, 1, 3, false};    // Skip
    case MAP_FEATURE_STATION:
      return {LINE_SOLID, 1, 9, true};    // Point marker
    default:
      return {LINE_SOLID, 1, 9, true};
  }
}

// =============================================================================
// MapCore - Map File Loading (Display-Agnostic)
// =============================================================================

bool MapCore::loadMapFile(const char* path) {
  // Hold the map lock across the WHOLE load: the unloadMap + fresh pool/slots/
  // tileDir allocation below must be atomic w.r.t. any concurrent renderMap, or
  // a render could see valid=true with a half-built cache. Taken before the FS
  // lock (global order: map -> FS). unloadMap's own guard no-ops (reentrant).
  MapCacheGuard mapGuard("MapCore.loadMapFile");

  // Unload any existing map
  unloadMap();

  // Pause sensor polling during file I/O to prevent I2C contention
  pollPause();
  vTaskDelay(pdMS_TO_TICKS(50));  // Let any in-flight I2C complete

  FsLockGuard fsGuard("MapCore.loadMapFile");
  
  if (!VFS::existsGuarded(path, currentAuthContext())) {
    WARN_MAPSF("Map file not found: %s", path);
    pollResume();
    return false;
  }

  File f = VFS::openGuarded(path, "r", currentAuthContext());
  if (!f) {
    ERROR_MAPSF("Failed to open map file: %s", path);
    pollResume();
    return false;
  }
  
  size_t fileSize = f.size();
  if (fileSize < sizeof(HWMapHeader)) {
    ERROR_MAPSF("Map file too small: %zu bytes", fileSize);
    f.close();
    pollResume();
    return false;
  }
  
  // Read header first
  HWMapHeader header;
  if (f.read((uint8_t*)&header, sizeof(header)) != sizeof(header)) {
    ERROR_MAPSF("Failed to read map header");
    f.close();
    pollResume();
    return false;
  }
  
  // Validate magic
  if (memcmp(header.magic, "HWMP", 4) != 0) {
    ERROR_MAPSF("Invalid map magic: %.4s", header.magic);
    f.close();
    pollResume();
    return false;
  }
  
  // Validate version (v6 only)
  if (header.version != 6) {
    ERROR_MAPSF("Unsupported map version: %u (need v6)", header.version);
    f.close();
    pollResume();
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
  memset(_currentMap.tiers, 0, sizeof(_currentMap.tiers));
  _currentMap.numTiers = 0;
  _currentMap.numSlots = 0;
  _currentMap.accessSeq = 0;
  _currentMap.slots = nullptr;
  _currentMap.cacheHits = 0;
  _currentMap.cacheMisses = 0;
  _currentMap.tilesDropped = 0;
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
  
  INFO_MAPSF("Loading map v%u: %s (%zu bytes, %u features, %ux%u tiles)", 
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
      ERROR_MAPSF("Failed to scan name table");
      f.close();
      pollResume();
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
      INFO_MAPSF("Parsed %u names", parsed);
    }
  }
  
  // === PARSE TILE DIRECTORY (small, keep in RAM) ===
  if (_currentMap.tileCount == 0) {
    ERROR_MAPSF("Tile count is 0 (gridSize=%u, flags=0x%04X)", _currentMap.tileGridSize, header.flags);
  } else if (_currentMap.tileCount > HWMAP_MAX_TILES) {
    ERROR_MAPSF("Tile count %u exceeds max %u (gridSize=%u)", _currentMap.tileCount, HWMAP_MAX_TILES, _currentMap.tileGridSize);
  }
  if (_currentMap.tileCount > 0 && _currentMap.tileCount <= HWMAP_MAX_TILES) {
    size_t tileDirSize = sizeof(HWMapTileDirEntry) * _currentMap.tileCount;
    HWMapTileDirEntry* tileDir = (HWMapTileDirEntry*)ps_malloc(tileDirSize);
    
    if (tileDir) {
      f.seek(nameTableEnd);
      size_t readBytes = f.read((uint8_t*)tileDir, tileDirSize);
      if (readBytes == tileDirSize) {
        _currentMap.tileDir = tileDir;
        INFO_MAPSF("Tile directory: %u tiles, first at offset %u", 
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
        ERROR_MAPSF("Failed to read tile directory");
      }
    }
  }
  
  // Keep file open as persistent handle (closed in unloadMap)
  _currentMap.mapFile = f;
  
  // === ALLOCATE MULTI-TIER SLAB CACHE ===
  // Scan tile directory: find max payload and build size histogram for tier config
  uint32_t maxPayload = 0;
  uint32_t totalPayload = 0;
  uint16_t nonEmptyTiles = 0;
  // Histogram: count tiles per 4KB-aligned bucket (0=0-4KB, 1=4-8KB, ...)
  const int kHistBuckets = 8;
  uint16_t histogram[kHistBuckets] = {0};
  if (_currentMap.tileDir) {
    for (uint16_t i = 0; i < _currentMap.tileCount; i++) {
      uint32_t ps = _currentMap.tileDir[i].payloadSize;
      if (ps > 0) {
        nonEmptyTiles++;
        totalPayload += ps;
        if (ps > maxPayload) maxPayload = ps;
        int bucket = (ps - 1) / 4096;
        if (bucket >= kHistBuckets) bucket = kHistBuckets - 1;
        histogram[bucket]++;
      }
    }
  }

  // Determine tier configuration from histogram
  // Each populated bucket range becomes a tier; sparse buckets merge upward
  // Tier slot sizes are 4KB-aligned: 4KB, 8KB, 12KB, 16KB, ...
  struct TierSpec { uint32_t slotSize; uint16_t tileCount; };
  TierSpec tierSpecs[MAP_CACHE_MAX_TIERS];
  uint8_t numTiers = 0;

  uint32_t maxSlotSize = (maxPayload + 4095) & ~4095u;
  if (maxSlotSize < MAP_CACHE_MIN_SLOT) maxSlotSize = MAP_CACHE_MIN_SLOT;

  // Merge threshold: buckets with fewer than 5% of tiles merge into next tier
  uint16_t mergeThreshold = nonEmptyTiles / 20;
  if (mergeThreshold < 2) mergeThreshold = 2;

  uint16_t pendingCount = 0;
  for (int b = 0; b < kHistBuckets; b++) {
    pendingCount += histogram[b];
    if (pendingCount == 0) continue;

    uint32_t tierSlotSize = ((uint32_t)(b + 1)) * 4096;
    if (tierSlotSize < MAP_CACHE_MIN_SLOT) tierSlotSize = MAP_CACHE_MIN_SLOT;

    // Check if this is the last populated bucket or we have enough tiles for a tier
    bool isLastPopulated = true;
    for (int nb = b + 1; nb < kHistBuckets; nb++) {
      if (histogram[nb] > 0) { isLastPopulated = false; break; }
    }

    bool shouldFlush = isLastPopulated || pendingCount >= mergeThreshold ||
                       numTiers == MAP_CACHE_MAX_TIERS - 1;

    if (shouldFlush && numTiers < MAP_CACHE_MAX_TIERS) {
      // This is the last tier we'll create - either the final populated
      // bucket, or we've hit the tier-count cap (only one tier slot left). It
      // must hold the largest tile in the map (nothing bigger gets its own
      // tier), and be credited with every still-unassigned tile so the pool
      // split gives it enough slots. The old code only bumped on
      // isLastPopulated, so when a map spanned more than MAP_CACHE_MAX_TIERS
      // size buckets the biggest tiles had no tier and were dropped every frame.
      bool isFinalTier = isLastPopulated || numTiers == MAP_CACHE_MAX_TIERS - 1;
      if (isFinalTier) {
        if (tierSlotSize < maxSlotSize) tierSlotSize = maxSlotSize;
        // Fold in tiles from any larger buckets - they get no tier of their own
        // and will land in this (now largest) tier at load time.
        for (int nb = b + 1; nb < kHistBuckets; nb++) pendingCount += histogram[nb];
      }
      tierSpecs[numTiers].slotSize = tierSlotSize;
      tierSpecs[numTiers].tileCount = pendingCount;
      numTiers++;
      pendingCount = 0;
    }
  }

  // Fallback: if no tiers created (no tiles), make one default tier
  if (numTiers == 0) {
    tierSpecs[0].slotSize = MAP_CACHE_MIN_SLOT;
    tierSpecs[0].tileCount = 1;
    numTiers = 1;
  }

  // Allocate pool — size driven by gSettings.mapCacheSizeKB (default 1280 KB).
  // Halves on alloc failure so the cache still comes up on tight PSRAM.
  size_t poolSize = (size_t)gSettings.mapCacheSizeKB * 1024;
  if (poolSize < 64 * 1024) poolSize = 64 * 1024;          // sanity floor
  if (poolSize > 8 * 1024 * 1024) poolSize = 8 * 1024 * 1024; // sanity cap
  uint8_t* pool = nullptr;
  while (poolSize >= tierSpecs[0].slotSize && !pool) {
    pool = (uint8_t*)ps_malloc(poolSize);
    if (!pool) {
      poolSize /= 2;
    }
  }
  if (!pool) {
    ERROR_MAPSF("Failed to allocate tile cache pool in PSRAM");
    pollResume();
    unloadMap();
    return false;
  }

  // Distribute pool budget proportionally to each tier's weighted need
  uint64_t totalWeighted = 0;
  for (uint8_t t = 0; t < numTiers; t++) {
    totalWeighted += (uint64_t)tierSpecs[t].tileCount * tierSpecs[t].slotSize;
  }
  if (totalWeighted == 0) totalWeighted = 1;

  // Assign slot counts per tier, ensuring at least 1 slot each
  size_t poolRemaining = poolSize;
  uint16_t totalSlots = 0;
  CacheTier tiers[MAP_CACHE_MAX_TIERS];

  for (uint8_t t = 0; t < numTiers; t++) {
    uint64_t tierWeight = (uint64_t)tierSpecs[t].tileCount * tierSpecs[t].slotSize;
    size_t tierBytes = (size_t)((tierWeight * poolSize) / totalWeighted);
    // Don't exceed remaining pool
    if (tierBytes > poolRemaining) tierBytes = poolRemaining;
    uint16_t slotCount = (uint16_t)(tierBytes / tierSpecs[t].slotSize);
    if (slotCount == 0) slotCount = 1;
    // Cap total slots
    if (totalSlots + slotCount > MAP_CACHE_MAX_SLOTS) {
      slotCount = MAP_CACHE_MAX_SLOTS - totalSlots;
    }
    tiers[t].slotSize = tierSpecs[t].slotSize;
    tiers[t].slotCount = slotCount;
    tiers[t].firstSlotIndex = totalSlots;
    tiers[t].poolOffset = poolSize - poolRemaining;

    size_t tierUsed = (size_t)slotCount * tierSpecs[t].slotSize;
    poolRemaining -= tierUsed;
    totalSlots += slotCount;
  }

  // If there's leftover pool space, give extra slots to the smallest tier
  if (poolRemaining >= tiers[0].slotSize && totalSlots < MAP_CACHE_MAX_SLOTS) {
    uint16_t extraSlots = (uint16_t)(poolRemaining / tiers[0].slotSize);
    if (totalSlots + extraSlots > MAP_CACHE_MAX_SLOTS) {
      extraSlots = MAP_CACHE_MAX_SLOTS - totalSlots;
    }
    if (extraSlots > 0) {
      // Shift all tiers after tier 0 to make room
      size_t extraBytes = (size_t)extraSlots * tiers[0].slotSize;
      for (uint8_t t = 1; t < numTiers; t++) {
        tiers[t].firstSlotIndex += extraSlots;
        tiers[t].poolOffset += extraBytes;
      }
      tiers[0].slotCount += extraSlots;
      totalSlots += extraSlots;
    }
  }

  // Allocate slot metadata array
  TileCacheSlot* slots = (TileCacheSlot*)ps_malloc(sizeof(TileCacheSlot) * totalSlots);
  if (!slots) {
    free(pool);
    ERROR_MAPSF("Failed to allocate %u tile cache slot entries", totalSlots);
    pollResume();
    unloadMap();
    return false;
  }
  // Initialize all slots as empty, with correct tier assignment
  for (uint8_t t = 0; t < numTiers; t++) {
    for (uint16_t i = tiers[t].firstSlotIndex;
         i < tiers[t].firstSlotIndex + tiers[t].slotCount; i++) {
      slots[i].tileIdx = -1;
      slots[i].dataSize = 0;
      slots[i].lastAccessSeq = 0;
      slots[i].tierIdx = t;
    }
  }

  _currentMap.cachePool = pool;
  _currentMap.cachePoolSize = poolSize;
  memcpy(_currentMap.tiers, tiers, sizeof(tiers));
  _currentMap.numTiers = numTiers;
  _currentMap.numSlots = totalSlots;
  _currentMap.accessSeq = 0;
  _currentMap.slots = slots;
  _currentMap.cacheHits = 0;
  _currentMap.cacheMisses = 0;
  _currentMap.tilesDropped = 0;

  uint32_t avgPayload = nonEmptyTiles ? (totalPayload / nonEmptyTiles) : 0;
  INFO_MAPSF("Tile cache: %uKB pool, %u tiers, %u total slots | tiles: %u non-empty, avg %uB, max %uB",
                (unsigned)(poolSize / 1024), numTiers, totalSlots,
                nonEmptyTiles, avgPayload, maxPayload);
  for (uint8_t t = 0; t < numTiers; t++) {
    INFO_MAPSF("  Tier %u: %uKB slots x %u = %uKB (offset %u, slots %u-%u)",
                  t, (unsigned)(tiers[t].slotSize / 1024), tiers[t].slotCount,
                  (unsigned)(tiers[t].slotSize * tiers[t].slotCount / 1024),
                  (unsigned)tiers[t].poolOffset,
                  tiers[t].firstSlotIndex,
                  tiers[t].firstSlotIndex + tiers[t].slotCount - 1);
  }

  // === PRE-WARM CACHE: load all tiles that fit into slots at load time ===
  if (_currentMap.mapFile && _currentMap.tileDir && nonEmptyTiles <= totalSlots) {
    // Track next-available slot per tier for sequential pre-warm filling
    uint16_t nextSlot[MAP_CACHE_MAX_TIERS];
    for (uint8_t t = 0; t < numTiers; t++) {
      nextSlot[t] = tiers[t].firstSlotIndex;
    }
    uint16_t preloaded = 0;
    uint32_t preloadStart = millis();
    for (uint16_t i = 0; i < _currentMap.tileCount; i++) {
      uint32_t ps = _currentMap.tileDir[i].payloadSize;
      if (ps == 0) continue;
      // Find smallest tier that fits this tile
      int t = 0;
      while (t < numTiers && tiers[t].slotSize < ps) t++;
      // Overflow upward if ideal tier is full
      while (t < numTiers && nextSlot[t] >= tiers[t].firstSlotIndex + tiers[t].slotCount) t++;
      if (t >= numTiers) {
        DEBUG_MAPS_LOADINGF("[MAPS] prewarm: tile %u payload %u, no tier/slot available, skipping", i, ps);
        continue;
      }
      uint16_t si = nextSlot[t]++;
      uint8_t* slotData = pool + tiers[t].poolOffset +
                           (size_t)(si - tiers[t].firstSlotIndex) * tiers[t].slotSize;
      _currentMap.mapFile.seek(_currentMap.tileDir[i].offset);
      size_t got = _currentMap.mapFile.read(slotData, ps);
      if (got == ps) {
        slots[si].tileIdx = (int16_t)i;
        slots[si].dataSize = ps;
        slots[si].lastAccessSeq = ++_currentMap.accessSeq;
        preloaded++;
      }
    }
    INFO_MAPSF("Cache pre-warmed: %u tiles loaded in %lums (zero runtime misses expected)",
                  preloaded, (unsigned long)(millis() - preloadStart));
  } else if (nonEmptyTiles > totalSlots) {
    INFO_MAPSF("Map too large for full pre-warm: %u tiles > %u slots (LRU will handle misses)",
                  nonEmptyTiles, totalSlots);
  }
  
  // Invalidate location context since map changed
  LocationContextManager::invalidate();
  
  // Load waypoints for this map
  WaypointManager::loadWaypoints();
  
  // Resume sensor polling
  pollResume();
  
  return true;
}

void MapCore::unloadMap() {
  // Take the map lock BEFORE the FS lock (global order: map -> FS) so we can't
  // free cachePool/slots/tileDir while a render is mid-parse holding pointers
  // into them. Reentrant: no-op when called from loadMapFile (already holds it).
  MapCacheGuard mapGuard("MapCore.unloadMap");
  FsLockGuard fsGuard("MapCore.unloadMap");

  // Log cache stats before freeing
  if (_currentMap.valid && (_currentMap.cacheHits > 0 || _currentMap.cacheMisses > 0)) {
    uint32_t total = _currentMap.cacheHits + _currentMap.cacheMisses;
    INFO_MAPSF("Tile cache stats: %u hits, %u misses (%.1f%% hit rate), %u slots, %u dropped",
                  _currentMap.cacheHits, _currentMap.cacheMisses,
                  total > 0 ? (100.0f * _currentMap.cacheHits / total) : 0.0f,
                  _currentMap.numSlots, _currentMap.tilesDropped);
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
  memset(_currentMap.tiers, 0, sizeof(_currentMap.tiers));
  _currentMap.numTiers = 0;
  _currentMap.numSlots = 0;
  _currentMap.accessSeq = 0;
  _currentMap.cacheHits = 0;
  _currentMap.cacheMisses = 0;
  _currentMap.tilesDropped = 0;
  
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

// Helper: compute data pointer for a slot using tier-aware offsets
static inline uint8_t* slotDataPtr(const LoadedMap& map, uint16_t slotIdx) {
  const CacheTier& tier = map.tiers[map.slots[slotIdx].tierIdx];
  return map.cachePool + tier.poolOffset +
         (size_t)(slotIdx - tier.firstSlotIndex) * tier.slotSize;
}

// Helper: Load tile data via multi-tier slab cache
// Returns pointer to tile data in cache slot, or nullptr on error
// Caller MUST hold gMapCacheMutex across BOTH this call and its use of the
// returned pointer (it points into the shared LRU pool a concurrent miss could
// evict). The guard here is reentrant, so it's a no-op when called from
// renderMap/updateContext (which already hold it) and a real lock only if some
// future caller forgets — but such a caller would still be unsafe once it
// parses the returned bytes outside the lock, so hold it at the call site.
const uint8_t* MapCore::loadTileData(uint16_t tileIdx, size_t* outSize) {
  MapCacheGuard mapGuard("MapCore.loadTileData");
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
      return slotDataPtr(_currentMap, i);
    }
  }

  // === CACHE MISS ===
  _currentMap.cacheMisses++;

  uint32_t payloadSize = tile.payloadSize;

  // Find the ideal tier: smallest tier whose slotSize >= payloadSize
  int idealTier = -1;
  for (uint8_t t = 0; t < _currentMap.numTiers; t++) {
    if (_currentMap.tiers[t].slotSize >= payloadSize) {
      idealTier = t;
      break;
    }
  }
  if (idealTier < 0) {
    // Tile too large for any tier — drop it
    _currentMap.tilesDropped++;
    DEBUG_MAPS_RENDERINGF("[TILE_CACHE] DROP tile=%u payload=%uB > max tier %uB",
                          tileIdx, payloadSize, _currentMap.tiers[_currentMap.numTiers - 1].slotSize);
    return nullptr;
  }

  // Search ideal tier for an empty slot
  int16_t targetSlot = -1;
  uint32_t oldestSeq = UINT32_MAX;
  int16_t lruSlot = -1;
  {
    const CacheTier& tier = _currentMap.tiers[idealTier];
    uint16_t end = tier.firstSlotIndex + tier.slotCount;
    for (uint16_t i = tier.firstSlotIndex; i < end; i++) {
      if (_currentMap.slots[i].tileIdx == -1) {
        targetSlot = i;
        break;
      }
      if (_currentMap.slots[i].lastAccessSeq < oldestSeq) {
        oldestSeq = _currentMap.slots[i].lastAccessSeq;
        lruSlot = i;
      }
    }
  }

  // Overflow upward: try larger tiers for an empty slot
  if (targetSlot < 0) {
    for (int t = idealTier + 1; t < _currentMap.numTiers && targetSlot < 0; t++) {
      const CacheTier& tier = _currentMap.tiers[t];
      uint16_t end = tier.firstSlotIndex + tier.slotCount;
      for (uint16_t i = tier.firstSlotIndex; i < end; i++) {
        if (_currentMap.slots[i].tileIdx == -1) {
          targetSlot = i;
          break;
        }
      }
    }
  }

  // If no empty slot found anywhere, evict LRU from the ideal tier
  if (targetSlot < 0) {
    targetSlot = (lruSlot >= 0) ? lruSlot : _currentMap.tiers[idealTier].firstSlotIndex;
  }

  DEBUG_MAPS_PERFF("[TILE_CACHE] miss tile=%u slot=%d payload=%uB %s (hits=%u misses=%u seq=%u)",
                   tileIdx, targetSlot, tile.payloadSize,
                   _currentMap.slots[targetSlot].tileIdx == -1 ? "empty" : "evict",
                   _currentMap.cacheHits, _currentMap.cacheMisses, _currentMap.accessSeq);

  // Read tile data from file into the target slot
  uint8_t* slotData = slotDataPtr(_currentMap, (uint16_t)targetSlot);

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
      File f = VFS::openGuarded(_currentMap.filepath, "r", currentAuthContext());
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

  // Update slot metadata (keep the tierIdx of the slot we landed in, not idealTier)
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
  
  if (!VFS::existsGuarded("/maps", currentAuthContext())) {
    return 0;
  }

  File dir = VFS::openGuarded("/maps", "r", currentAuthContext());
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
        File sub = VFS::openGuarded(subPath, "r", currentAuthContext());
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
  
  // Clamp before casting to int16_t to prevent overflow-wrap producing false on-screen coords
  screenX = cx + (int16_t)fmaxf(-30000.0f, fminf(30000.0f, x));
  screenY = cy + (int16_t)fmaxf(-30000.0f, fminf(30000.0f, y));
}

// Thread-safe renderMap: all mutable state comes from params, no global reads.
// Held for the WHOLE render, not just cache bookkeeping: loadTileData hands
// back pointers into the shared LRU pool that the loop below parses, so a
// concurrent render (OLED task vs G2 map page) or unloadMap must not evict/free
// those tiles mid-parse. See MapCacheGuard.
void MapCore::renderMap(MapRenderer* renderer, float centerLat, float centerLon,
                        const MapRenderParams& params) {
  MapCacheGuard mapGuard("MapCore.renderMap");
  if (!_currentMap.valid || !renderer || !_currentMap.tileDir) {
    DEBUG_MAPS_RENDERINGF("[MAPS] renderMap early exit: valid=%d renderer=%p tileDir=%p",
                          _currentMap.valid, renderer, _currentMap.tileDir);
    return;
  }
  
  const float zoom = params.zoom;
  const float rotation = params.rotation;

  // Reset per-frame drop counter (overlay reads this after render)
  _currentMap.tilesDropped = 0;

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
  // Max-zoom-in clamp, proportional to viewport density: 10 udeg/px on the
  // 128-wide OLED, 4 on the 288-wide G2. The G2 renders at zoom*2.25 for its
  // native resolution; a fixed clamp of 10 would saturate its zoom (and skew
  // its aspect) 2.25x earlier than the OLED at the same gMapZoom.
  int32_t minScale = (10 * 128) / viewWidth;
  if (minScale < 1) minScale = 1;
  if (scaleX < minScale) scaleX = minScale;
  if (scaleY < minScale) scaleY = minScale;
  
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
  
  // If visible tiles > numSlots, increase tileStep to cap work (LRU can't hold all tiles at once).
  // When each tile is still several pixels wide, stride looks like a checkerboard — overridden below.
  int tileStep = 1;
  int numTilesX = maxTileX - minTileX + 1;
  int numTilesY = maxTileY - minTileY + 1;
  // Smaller axis of map tile in screen pixels (stride leaves gaps only when this is tiny)
  float pxPerTileX = (scaleX > 0) ? (float)_currentMap.tileW / (float)scaleX : 0.f;
  float pxPerTileY = (scaleY > 0) ? (float)_currentMap.tileH / (float)scaleY : 0.f;
  float minTilePx = (pxPerTileX < pxPerTileY) ? pxPerTileX : pxPerTileY;

  if (_currentMap.numSlots > 0) {
    while (((numTilesX + tileStep - 1) / tileStep) * ((numTilesY + tileStep - 1) / tileStep) > (int)_currentMap.numSlots && tileStep < 6) {
      tileStep++;
    }
  }
  // At 0.2x–0.3x zoom the viewport can exceed numSlots; stride=2 looks like a checkerboard because
  // each tile is still a few pixels wide. Prefer LRU thrashing over skipping tiles in that case.
  if (tileStep > 1 && minTilePx >= MAP_TILE_STRIDE_OK_BELOW_PX) {
    DEBUG_MAPS_RENDERINGF("[MAPS] render: stride=%d -> 1 (minTilePx=%.2f >= %.2f, avoid checkerboard)",
                          tileStep, minTilePx, MAP_TILE_STRIDE_OK_BELOW_PX);
    tileStep = 1;
  }

  DEBUG_MAPS_RENDERINGF("[MAPS] render: center=%.5f,%.5f zoom=%.2f scale=%ld,%ld grid=%d tiles=%d-%d/%d-%d (raw %d-%d/%d-%d) step=%d minTilePx=%.2f slots=%u",
                        centerLat, centerLon, zoom, (long)scaleX, (long)scaleY,
                        _currentMap.tileGridSize,
                        minTileX, maxTileX, minTileY, maxTileY,
                        rawMinTX, rawMaxTX, rawMinTY, rawMaxTY, tileStep, minTilePx,
                        (unsigned)_currentMap.numSlots);

  int totalFeatures = 0, totalDrawn = 0, tilesLoaded = 0, tilesEmpty = 0;

  // Actual tile iterations for this frame (stride reduces visits; numTilesX*Y does not).
  const int visTileX = (maxTileX >= minTileX) ? (maxTileX - minTileX) / tileStep + 1 : 0;
  const int visTileY = (maxTileY >= minTileY) ? (maxTileY - minTileY) / tileStep + 1 : 0;
  const int visibleTileVisits = visTileX * visTileY;

  // When we draw every visible tile but have fewer slots, LRU + flash reads can run long (e.g. OLED
  // mapRenderTask). Yield occasionally so WiFi/idle/watchdog peers aren't starved on that core.
  const bool cachePressure =
      _currentMap.numSlots > 0 && (numTilesX * numTilesY) > (int)_currentMap.numSlots;
  const bool heavyTileFrame =
      cachePressure || (visibleTileVisits >= MAP_RENDER_YIELD_MANY_VISITS);
  // More frequent yields when thrashing; moderate when many tiles even if slots "fit"; rare on tiny views.
  const unsigned tileYieldMask =
      cachePressure ? 7u : (visibleTileVisits >= MAP_RENDER_YIELD_MANY_VISITS ? 15u : 63u);
  uint32_t tileVisitCounter = 0;

  // Iterate through visible tiles (with stride to cap tile count within cache budget)
  for (int ty = minTileY; ty <= maxTileY; ty += tileStep) {
    for (int tx = minTileX; tx <= maxTileX; tx += tileStep) {
      if (heavyTileFrame && (++tileVisitCounter & tileYieldMask) == 0u) {
        taskYIELD();
      }
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
        if (heavyTileFrame && f != 0 &&
            (f % (uint16_t)MAP_RENDER_FEATURE_YIELD_STRIDE) == 0) {
          taskYIELD();
        }
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
        
        // Zoom LOD (single implementation: mapLodFeatureVisibleAtZoom in System_Maps.h)
        if (!mapLodFeatureVisibleAtZoom(ftype, fsubtype, zoom)) {
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
        
        // Area polygons (water bodies, land/coast mask) are clipped to the
        // tile+halo box by the generator, which turns each cut into an edge that
        // runs along the box boundary (quantized coord 0 or 65535). Drawn as a
        // wireframe outline those trace the tile/halo grid. For such features,
        // suppress any segment whose BOTH endpoints lie on the same box edge -
        // that only happens for a clip edge; real shoreline points are interior.
        const bool suppressTileEdges =
            (ftype == MAP_FEATURE_WATER && fsubtype != SUBTYPE_WATER_RIVER) ||
            ftype == MAP_FEATURE_LAND_MASK;

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
          // Clamp before the int16 cast (matches geoToScreen): a far-offscreen
          // halo point must not wrap modulo 2^16 into the +/-50px visibility
          // window below and draw a phantom line across the view.
          prevX = cx + (int16_t)fmaxf(-30000.0f, fminf(30000.0f, fx));
          prevY = cy + (int16_t)fmaxf(-30000.0f, fminf(30000.0f, fy));
        }
        uint16_t prevQLat = qLat, prevQLon = qLon;

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
            curX = cx + (int16_t)fmaxf(-30000.0f, fminf(30000.0f, fx));
            curY = cy + (int16_t)fmaxf(-30000.0f, fminf(30000.0f, fy));
          }
          
          // Simple visibility check
          bool visible = (prevX >= -50 && prevX < viewWidth + 50 &&
                          prevY >= -50 && prevY < viewHeight + 50) ||
                         (curX >= -50 && curX < viewWidth + 50 &&
                          curY >= -50 && curY < viewHeight + 50);
          
          // Drop tile/halo-boundary clip edges of area polygons (see above).
          bool tileEdge = suppressTileEdges &&
              ((prevQLat == 0 && qLat == 0) || (prevQLat == 65535 && qLat == 65535) ||
               (prevQLon == 0 && qLon == 0) || (prevQLon == 65535 && qLon == 65535));

          if (visible && !tileEdge) {
            renderer->drawLine(prevX, prevY, curX, curY, style);
            totalDrawn++;
          }

          prevX = curX;
          prevY = curY;
          prevQLat = qLat;
          prevQLon = qLon;
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
// OffscreenMapRenderer Implementation (shade framebuffer for async rendering)
// =============================================================================

OffscreenMapRenderer::OffscreenMapRenderer(uint8_t* buffer, int bufWidth, int bufHeight,
                                           int viewW, int viewH, int offsetY)
  : _buffer(buffer), _bufW(bufWidth), _bufH(bufHeight), _offsetY(offsetY) {
  _width = viewW;
  _height = viewH;
  _scale = (int16_t)(bufWidth / 128);
  if (_scale < 1) _scale = 1;
}

void OffscreenMapRenderer::setViewport(int width, int height) {
  _width = width;
  _height = height;
}

void OffscreenMapRenderer::clear() {
  if (_buffer) memset(_buffer, 0, (size_t)_bufW * (size_t)_bufH);
}

// Byte-per-pixel shade write: max(existing, shade) so brighter classes win
// overlaps regardless of feature draw order (see MapFeatureStyle).
void OffscreenMapRenderer::drawPixel(int16_t x, int16_t y, uint8_t shade) {
  int16_t ay = y + _offsetY;
  if (x < 0 || x >= _bufW || ay < 0 || ay >= _bufH) return;
  uint8_t& px = _buffer[(size_t)ay * _bufW + x];
  if (shade > px) px = shade;
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

void OffscreenMapRenderer::bresenhamLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t shade) {
  int16_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int16_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int16_t err = dx + dy;

  for (;;) {
    // Direct max-write (already clipped; y0 already includes _offsetY from clipLine)
    if (y0 >= 0 && y0 < _bufH && x0 >= 0 && x0 < _bufW) {
      uint8_t& px = _buffer[(size_t)y0 * _bufW + x0];
      if (shade > px) px = shade;
    }

    if (x0 == x1 && y0 == y1) break;
    int16_t e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void OffscreenMapRenderer::drawDashedLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int dashLen, uint8_t shade) {
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
      if (px >= 0 && px < _bufW && py >= 0 && py < _bufH) {
        uint8_t& p = _buffer[(size_t)py * _bufW + px];
        if (shade > p) p = shade;
      }
    }
    x += dx; y += dy;
    if (++segLen >= dashLen) { segLen = 0; draw = !draw; }
  }
}

void OffscreenMapRenderer::drawDottedLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int spacing, uint8_t shade) {
  int adx = abs(x1 - x0), ady = abs(y1 - y0);
  int len = (adx > ady) ? (adx + ady / 2) : (ady + adx / 2);
  if (len < 1) return;

  float invLen = 1.0f / len;
  float dx = (x1 - x0) * invLen;
  float dy = (y1 - y0) * invLen;

  for (int t = 0; t <= len; t += spacing) {
    int16_t px = x0 + (int16_t)(dx * t);
    int16_t py = y0 + (int16_t)(dy * t);
    if (px >= 0 && px < _bufW && py >= 0 && py < _bufH) {
      uint8_t& p = _buffer[(size_t)py * _bufW + px];
      if (shade > p) p = shade;
    }
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

  // One styled pass. Dash/dot pitch scales with buffer resolution so patterns
  // keep the same visual proportions on the 288-wide G2 as on the 128-wide OLED.
  auto pass = [&](int16_t ax0, int16_t ay0, int16_t ax1, int16_t ay1) {
    switch (style.lineStyle) {
      case LINE_SOLID:  bresenhamLine(ax0, ay0, ax1, ay1, style.shade); break;
      case LINE_DASHED: drawDashedLine(ax0, ay0, ax1, ay1, 4 * _scale, style.shade); break;
      case LINE_DOTTED: drawDottedLine(ax0, ay0, ax1, ay1, 3 * _scale, style.shade); break;
      case LINE_NONE:
      default: break;
    }
  };

  // Line weight = thickness: lay down `weight` parallel passes offset across the
  // line's minor axis, so road class reads by thickness (highway thick -> minor
  // thin). Thickness works on BOTH sinks - it is the G2's strongest cue and the
  // ONLY per-class cue the 1-bit OLED can show (shade is inert there). The base
  // line is already content-clipped; each offset pass is re-clipped so a thick
  // line at the very top/bottom of the OLED map can't bleed into the header/footer.
  int weight = style.lineWeight < 1 ? 1 : style.lineWeight;
  bool horizontal = abs(x1 - x0) >= abs(y1 - y0);
  for (int i = 0; i < weight; i++) {
    int16_t off = (int16_t)(i - weight / 2);   // w1->{0}, w2->{-1,0}, w3->{-1,0,1}
    if (off == 0) { pass(x0, y0, x1, y1); continue; }   // base pass, already clipped
    int16_t ax0, ay0, ax1, ay1;
    if (horizontal) { ax0 = x0; ay0 = (int16_t)(y0 + off); ax1 = x1; ay1 = (int16_t)(y1 + off); }
    else            { ax0 = (int16_t)(x0 + off); ay0 = y0; ax1 = (int16_t)(x1 + off); ay1 = y1; }
    if (clipLine(ax0, ay0, ax1, ay1)) pass(ax0, ay0, ax1, ay1);
  }
}

void OffscreenMapRenderer::drawPositionMarker(int16_t x, int16_t y) {
  if (!_buffer) return;
  y += _offsetY;

  const int16_t ymin = _offsetY, ymax = _offsetY + _height - 1;
  if (y < ymin || y > ymax) return;

  // Crosshair (arm length scales with buffer resolution)
  const int16_t arm = 4 * _scale;
  int16_t x0 = x - arm, x1 = x + arm;
  int16_t yt = (y - arm < ymin) ? ymin : y - arm;
  int16_t yb = (y + arm > ymax) ? ymax : y + arm;

  // Horizontal
  int16_t cy0 = y, cy1 = y;
  if (clipLine(x0, cy0, x1, cy1)) bresenhamLine(x0, cy0, x1, cy1, MAP_SHADE_MAX);
  // Vertical
  int16_t cx0 = x, cx1 = x;
  if (clipLine(cx0, yt, cx1, yb)) bresenhamLine(cx0, yt, cx1, yb, MAP_SHADE_MAX);

  // Simple circle (midpoint algorithm)
  const int16_t r0 = 3 * _scale;
  if (y - r0 >= ymin && y + r0 <= ymax) {
    int16_t r = r0, px = r, py = 0, err = 1 - r;
    while (px >= py) {
      drawPixel(x + px, y + py - _offsetY, MAP_SHADE_MAX);
      drawPixel(x - px, y + py - _offsetY, MAP_SHADE_MAX);
      drawPixel(x + px, y - py - _offsetY, MAP_SHADE_MAX);
      drawPixel(x - px, y - py - _offsetY, MAP_SHADE_MAX);
      drawPixel(x + py, y + px - _offsetY, MAP_SHADE_MAX);
      drawPixel(x - py, y + px - _offsetY, MAP_SHADE_MAX);
      drawPixel(x + py, y - px - _offsetY, MAP_SHADE_MAX);
      drawPixel(x - py, y - px - _offsetY, MAP_SHADE_MAX);
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

// Feature styles for the offscreen (shade-buffer) renderer, shared by the G2
// lens and the OLED. Roads carry a thickness hierarchy and each class a distinct
// line style (both visible on 1-bit); shade is the G2's brightness band and is
// inert on the OLED, which crushes any shade>0 to a set bit.
MapFeatureStyle OffscreenMapRenderer::getFeatureStyle(MapFeatureType type) {
  // Shared style ladder for BOTH sinks. Weight (thickness) and line style
  // (solid/dash/dot) differentiate classes on the G2 AND the 1-bit OLED; shade
  // adds the G2's green brightness bands and doubles as z-order (max-write:
  // brighter draws on top) but is inert on the OLED (packed to a set bit). What
  // differs per sink is only shouldRenderFeature - water/coast area outlines are
  // surfaced on the G2 and kept off the low-pixel OLED. Bands:
  //   roads 10-14 (bright, + thick->thin weight)   transit/rail 6-9 (mid, dash/dot)
  //   water/coast/buildings 3-5 (dim).   Overlays sit above all at shade 15.
  switch (type) {
    case MAP_FEATURE_HIGHWAY:    return {LINE_SOLID,  3, 14, true};  // thickest + brightest
    case MAP_FEATURE_ROAD_MAJOR: return {LINE_SOLID,  2, 12, true};  // medium
    case MAP_FEATURE_STATION:    return {LINE_SOLID,  1, 11, true};  // point marker
    case MAP_FEATURE_ROAD_MINOR: return {LINE_SOLID,  1, 10, true};  // thin
    case MAP_FEATURE_RAILWAY:    return {LINE_DASHED, 1,  9, true};
    case MAP_FEATURE_PATH:       return {LINE_DOTTED, 1,  7, true};
    case MAP_FEATURE_BUS:        return {LINE_DASHED, 1,  7, true};
    case MAP_FEATURE_FERRY:      return {LINE_DASHED, 1,  6, true};  // over water
    case MAP_FEATURE_WATER:      return {LINE_SOLID,  1,  5, true};  // lakes/rivers/coast, dim solid
    case MAP_FEATURE_LAND_MASK:  return {LINE_DOTTED, 1,  4, true};  // coastline outline
    case MAP_FEATURE_BUILDING:   return {LINE_DOTTED, 1,  3, true};  // dimmest
    case MAP_FEATURE_PARK:       return {LINE_NONE,   1,  0, false}; // area fill, meaningless as wireframe
    default:                     return {LINE_SOLID,  1, 10, true};
  }
}

bool OffscreenMapRenderer::shouldRenderFeature(uint8_t type, uint8_t subtype) {
  if (!_surfaceAreas) {
    // Low-pixel 1-bit OLED: rivers only, skip parks + land_mask. Area outlines
    // (lakes, coastlines, land mask) are just more ON lines at 1-bit with no
    // brightness to set them apart from roads, so they clutter more than clarify.
    switch (type) {
      case MAP_FEATURE_WATER:     return (subtype == SUBTYPE_WATER_RIVER);
      case MAP_FEATURE_PARK:
      case MAP_FEATURE_LAND_MASK: return false;
      default:                    return true;
    }
  }
  // G2 lens: surface water bodies (lakes + rivers + coastline) and the
  // land/coastline mask as wireframe outlines, so shoreline and water read
  // instead of only rivers. Parks stay off: an unfilled park outline is noise.
  switch (type) {
    case MAP_FEATURE_PARK:
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
  // OLED-optimized styles - ALL features rendered as white lines (the OLED
  // draws 1-bit, so shade values here are inert; kept on the shared band
  // ladder for consistency)
  switch (type) {
    case MAP_FEATURE_HIGHWAY:
      return {LINE_SOLID, 1, 13, true};
    case MAP_FEATURE_ROAD_MAJOR:
      return {LINE_SOLID, 1, 11, true};
    case MAP_FEATURE_ROAD_MINOR:
      return {LINE_SOLID, 1, 9, true};   // Solid thin line
    case MAP_FEATURE_PATH:
      return {LINE_DOTTED, 1, 5, true};
    case MAP_FEATURE_WATER:
      return {LINE_SOLID, 1, 7, true};   // Rivers only (lakes/coastlines filtered by shouldRenderFeature)
    case MAP_FEATURE_PARK:
      return {LINE_NONE, 1, 0, false};   // Skip: polygon fill only (meaningless as wireframe)
    case MAP_FEATURE_LAND_MASK:
      return {LINE_NONE, 1, 0, false};   // Skip: land mask is only meaningful as filled region
    case MAP_FEATURE_RAILWAY:
      return {LINE_DASHED, 1, 7, true};
    case MAP_FEATURE_BUS:
      return {LINE_DASHED, 1, 5, true};  // Dashed for bus routes
    case MAP_FEATURE_FERRY:
      return {LINE_DASHED, 1, 5, true};  // Dashed for ferries
    case MAP_FEATURE_BUILDING:
      return {LINE_DOTTED, 1, 3, true};  // Dotted for buildings
    case MAP_FEATURE_STATION:
      return {LINE_SOLID, 1, 9, true};   // Solid for stations (drawn as point)
    default:
      return {LINE_SOLID, 1, 9, true};
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

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["valid"] = currentMap.valid;
    if (currentMap.valid) {
      char rgn[9]; memcpy(rgn, currentMap.header.regionName, 8); rgn[8] = '\0';
      doc["filename"]     = String(currentMap.filename);
      doc["region"]       = String(rgn);
      doc["featureCount"] = (unsigned long)currentMap.header.featureCount;
      doc["fileSize"]     = (unsigned long)currentMap.fileSize;
      JsonObject b = doc["bounds"].to<JsonObject>();
      b["minLat"] = currentMap.header.minLat / 1000000.0;
      b["minLon"] = currentMap.header.minLon / 1000000.0;
      b["maxLat"] = currentMap.header.maxLat / 1000000.0;
      b["maxLon"] = currentMap.header.maxLon / 1000000.0;
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

  if (!currentMap.valid) {
    return "Error: No map loaded. Use 'mapload <path>' or upload to /maps/";
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

  CommandArgs a(argsInput);
  String path;
  const char* qerr = requireQuotedToken(a, 0, path);
  if (qerr) return qerr;
  if (a.has(1)) return "Error: unexpected argument — usage: mapload \"<path>\"";

  if (MapCore::loadMapFile(path.c_str())) {
    const LoadedMap& currentMap = MapCore::getCurrentMap();
    if (!ensureDebugBuffer()) return "Map loaded";
    snprintf(getDebugBuffer(), 1024, "Loaded: %s (%lu features)", 
             currentMap.filename, currentMap.header.featureCount);
    return getDebugBuffer();
  }
  
  return "Error: Failed to load map";
}

const char* cmd_mapunload(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  if (!MapCore::hasValidMap()) {
    return "Error: No map loaded on device.";
  }
  MapCore::unloadMap();
  return "Map unloaded (PSRAM/cache freed on device).";
}

const char* cmd_whereami(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  const LocationContext& ctx = LocationContextManager::getContext();

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["valid"] = ctx.valid;
    if (ctx.valid) {
      if (ctx.nearestRoad[0] != '\0') {
        doc["road"]          = String(ctx.nearestRoad);
        doc["roadDistanceM"] = ctx.roadDistanceM;
      }
      if (ctx.nearestArea[0] != '\0') {
        doc["area"]          = String(ctx.nearestArea);
        doc["areaDistanceM"] = ctx.areaDistanceM;
      }
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

  if (!ctx.valid) {
    return "Error: Location context not available. Need GPS fix and loaded map.";
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

  CommandArgs a(argsInput);
  if (!a.hasMinArgs(1)) return "Error: invalid arguments — Usage: search <name>";

  if (!MapCore::hasValidMap()) {
    cliHint("search needs a loaded map - list them with 'maplist', then load one with 'mapload \"<path>\"'");
    return "Error: No map loaded";
  }

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  // Search through all feature names
  const LoadedMap& map = MapCore::getCurrentMap();
  char* buf = getDebugBuffer();
  int pos = 0;
  int found = 0;

  // Convert search term to lowercase for case-insensitive search (preserve spaces)
  String searchTerm = a.raw();
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

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    JsonArray arr = doc["maps"].to<JsonArray>();
    for (int i = 0; i < count; i++) arr.add(String("/maps/") + maps[i]);
    doc["count"] = count;
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

  if (count == 0) {
    return "No maps found in /maps/";
  }
  
  char* buf = getDebugBuffer();
  int offset = snprintf(buf, 1024, "Available maps:\n");

  for (int i = 0; i < count && offset < 900; i++) {
    offset += snprintf(buf + offset, 1024 - offset, "  /maps/%s\n", maps[i]);
  }

  cliHint("to load one of these, use 'mapload \"<path>\"'");
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

EXT_RAM_BSS_ATTR Waypoint WaypointManager::_waypoints[MAX_WAYPOINTS];
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
  if (VFS::existsGuarded(wpPathStr1, currentAuthContext())) {
    wpPathStr = wpPathStr1;
  } else if (VFS::existsGuarded(wpPathStr2, currentAuthContext())) {
    wpPathStr = wpPathStr2;
  } else if (VFS::existsGuarded(wpPathStr3, currentAuthContext())) {
    wpPathStr = wpPathStr3;
  } else {
    // No waypoints file for this map — clear any stale data from a previous map
    memset(_waypoints, 0, sizeof(_waypoints));
    _selectedTarget = -1;
    return false;
  }
  
  char wpPath[128];
  strlcpy(wpPath, wpPathStr.c_str(), sizeof(wpPath));
  
  File f = VFS::openGuarded(wpPath, "r", currentAuthContext());
  if (!f) return false;

  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  
  if (err) {
    WARN_MAPSF("Waypoint JSON parse error: %s", err.c_str());
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
  
  INFO_MAPSF("Loaded %d waypoints", i);
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
  if (!VFS::existsGuarded(mapDir, currentAuthContext())) {
    VFS::mkdirGuarded(mapDir, currentAuthContext());
  }
  
  // Extract map base name and save with pattern: waypoints_<mapbase>.json
  String mapFileName = mapPath.substring(slash + 1);
  String mapBase = mapFileName;
  if (mapBase.endsWith(".hwmap")) {
    mapBase = mapBase.substring(0, mapBase.length() - 6);
  }
  
  char wpPath[128];
  snprintf(wpPath, sizeof(wpPath), "%s/waypoints_%s.json", mapDir.c_str(), mapBase.c_str());
  
  File f = VFS::openGuarded(wpPath, "w", currentAuthContext(), true);
  if (!f) {
    ERROR_MAPSF("Failed to write waypoints file: %s", wpPath);
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

bool WaypointManager::getDistanceBearingToIndex(int index, float fromLat, float fromLon,
                                                 float& distanceM, float& bearingDeg) {
  if (index < 0 || index >= MAX_WAYPOINTS || !_waypoints[index].active) {
    return false;
  }

  const Waypoint& wp = _waypoints[index];

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

bool WaypointManager::getDistanceBearing(float fromLat, float fromLon,
                                          float& distanceM, float& bearingDeg) {
  return getDistanceBearingToIndex(_selectedTarget, fromLat, fromLon, distanceM, bearingDeg);
}

bool WaypointManager::getDistanceBearingToName(const char* name, float fromLat, float fromLon,
                                               float& distanceM, float& bearingDeg) {
  int index = findWaypointByName(name);
  if (index < 0) return false;
  return getDistanceBearingToIndex(index, fromLat, fromLon, distanceM, bearingDeg);
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
      // Draw waypoint marker: X shape, or filled for selected target.
      // Marker size scales with viewport resolution (1 on the 128-wide OLED,
      // 2 on the 288-wide G2) so markers stay legible on denser targets.
      const int16_t mf = (viewWidth >= 256) ? 2 : 1;
      bool isTarget = (i == _selectedTarget);
      if (isTarget) {
        // Filled diamond for target
        MapFeatureStyle style = {LINE_SOLID, 1, MAP_SHADE_MAX, true};
        renderer->drawLine(screenX - 3 * mf, screenY, screenX, screenY - 3 * mf, style);
        renderer->drawLine(screenX, screenY - 3 * mf, screenX + 3 * mf, screenY, style);
        renderer->drawLine(screenX + 3 * mf, screenY, screenX, screenY + 3 * mf, style);
        renderer->drawLine(screenX, screenY + 3 * mf, screenX - 3 * mf, screenY, style);
      } else {
        // Small X for regular waypoints
        MapFeatureStyle style = {LINE_SOLID, 1, MAP_SHADE_MAX, true};
        renderer->drawLine(screenX - 2 * mf, screenY - 2 * mf, screenX + 2 * mf, screenY + 2 * mf, style);
        renderer->drawLine(screenX - 2 * mf, screenY + 2 * mf, screenX + 2 * mf, screenY - 2 * mf, style);
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
EXT_RAM_BSS_ATTR char GPSTrackManager::_filename[256];
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
    // A general sensor-log line carries a "gps:" tag; reaching here means it had
    // no lat=/lon= (a no_fix reading), so reject it outright. Without this, the
    // CSV fallback below would blindly parse the first "<num>,<num>" on the line
    // — which in a multi-sensor log could be a comma-bearing co-logged sensor
    // (e.g. thermal "20.5,21.0,22.3"), injecting a bogus position. The dedicated
    // GPS-track CSV format has no "gps:" marker, so it is unaffected.
    if (strstr(line, "gps:")) return false;
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
  
  if (!VFS::existsGuarded(filepath, currentAuthContext())) {
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

  File f = VFS::openGuarded(filepath, "r", currentAuthContext());
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
      // Skip exact duplicates — GPS logs often repeat the same fix at sub-1Hz intervals
      if (_pointCount > 0 &&
          _points[_pointCount - 1].lat == lat &&
          _points[_pointCount - 1].lon == lon) {
        continue;
      }
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
  
  INFO_MAPSF("Loaded GPS track: %d points from %s", _pointCount, filepath);
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
  bool success = VFS::removeGuarded(filepath, currentAuthContext());
  fsUnlock();
  
  return success;
}

bool GPSTrackManager::saveTrack(char* outPath, size_t outPathSize) {
  if (!_points || _pointCount == 0) return false;

  // Generate timestamped filename
  const char* dir = "/logging_captures/tracks";
  // /logging_captures/ grants CREATE/WRITE to SYSTEM only (admins get just
  // READ|DELETE), so create the track file as system — same as gpstrackmerge
  // and the sensor logger. currentAuthContext() here fails from web/OLED (admin)
  // with a create denial.
  const AuthContext sys = VFS::systemAuth("gpstrack.save");
  fsLock("gpstrack.save");
  if (!VFS::existsGuarded(dir, sys)) {
    VFS::mkdirGuarded(dir, sys);
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

  File f = VFS::openGuarded(outPath, "w", sys, true);
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

  INFO_MAPSF("Saved GPS track: %d points to %s", _pointCount, outPath);
  return true;
}

void GPSTrackManager::setLiveTracking(bool enabled) {
  if (enabled && !_liveTracking) {
    // Starting live tracking - allocate buffer if not already allocated
    if (!_points) {
      _points = (GPSTrackPoint*)ps_alloc(MAX_TRACK_POINTS * sizeof(GPSTrackPoint), 
                                          AllocPref::PreferPSRAM, "gps.live");
      if (!_points) {
        ERROR_MAPSF("Failed to allocate live track buffer");
        return;
      }
    }
    _pointCount = 0;
    _bounds.valid = false;
    _stats.valid = false;
    strlcpy(_filename, "[LIVE]", sizeof(_filename));
    INFO_MAPSF("Live tracking started");
  } else if (!enabled && _liveTracking) {
    INFO_MAPSF("Live tracking stopped (%d points)", _pointCount);
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
  EXT_RAM_BSS_ATTR static char msg[128];
  
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
  
  // Track style: dotted line to distinguish from roads; overlay shade so the
  // track stays visible over every map feature (max-write)
  MapFeatureStyle trackStyle = {LINE_DOTTED, 2, MAP_SHADE_MAX, true};
  
  // Suppress physically-impossible jumps between consecutive fixes: a stitched
  // day can place two valid but far-apart fixes next to each other across a GPS
  // dropout (parked, signal lost, or a reboot between sessions), and drawing
  // that segment throws a line clear across the map. Anything over 5 miles at
  // this sample rate can't be real, so that one segment is skipped — the rest
  // of the track stays connected. Equirectangular metres with cosLat fixed at
  // the view centre, compared squared, so there is no per-point sqrt/trig.
  const float kMetersPerDeg   = 111320.0f;
  const float cosLat          = cosf(centerLat * 0.017453292519943295f);
  const float kMaxSegMeters   = 5.0f * 1609.344f;  // 5 miles
  const float kMaxSegMeters2  = kMaxSegMeters * kMaxSegMeters;

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

    bool tooLong = false;
    if (i > 0) {
      float dLatM = (_points[i].lat - _points[i - 1].lat) * kMetersPerDeg;
      float dLonM = (_points[i].lon - _points[i - 1].lon) * kMetersPerDeg * cosLat;
      tooLong = (dLatM * dLatM + dLonM * dLonM) > kMaxSegMeters2;
    }

    if (onScreen && prevValid && !tooLong) {
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
      MapFeatureStyle markerStyle = {LINE_SOLID, 1, MAP_SHADE_MAX, true};
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
      MapFeatureStyle markerStyle = {LINE_SOLID, 1, MAP_SHADE_MAX, true};
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

  CommandArgs a(argsInput);
  String sub = a.arg(0);
  sub.toLowerCase();

  if (sub.length() == 0 || sub == "status") {
    if (!GPSTrackManager::hasTrack()) {
      cliHint("to load a track, use 'gpstrack load <filepath>'");
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

  if (sub == "load") {
    if (!a.hasMinArgs(2)) return "Error: invalid arguments — Usage: gpstrack load <filepath>";
    String errorMsg;

    if (GPSTrackManager::loadTrack(a.arg(1).c_str(), errorMsg)) {
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
      snprintf(buf, 1024, "Error: Failed to load track: %s", errorMsg.c_str());
      return buf;
    }
  }

  if (sub == "clear") {
    GPSTrackManager::clearTrack();
    return "GPS track cleared";
  }

  return "Error: invalid arguments — Usage: gpstrack [status|load <filepath>|clear]";
}

// Stitch several GPS/sensor capture logs into one file, in the given order, so
// a day split across multiple files (e.g. by power loss) can be loaded as a
// single track. Args: "<output>" "<in1>" "<in2>" ... — output first, then the
// inputs front-to-back. A newline boundary is guaranteed after each file so a
// torn last line from a power cut can't fuse into the next file's first line.
const char* cmd_gpstrackmerge(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();

  CommandArgs a(argsInput);
  if (a.count() < 2) {
    return "Error: invalid arguments — Usage: gpstrackmerge \"<output>\" \"<in1>\" \"<in2>\" [...] "
           "(output first, then inputs in stitch order)";
  }

  // Output (arg 0): a bare name lands in /logging_captures/tracks/ so the map
  // page's track picker finds it; ensure a log-ish extension.
  String outName;
  const char* qerr = requireQuotedToken(a, 0, outName);
  if (qerr) return qerr;
  String outPath = outName;
  if (!outPath.startsWith("/")) outPath = "/logging_captures/tracks/" + outPath;
  if (!(outPath.endsWith(".log") || outPath.endsWith(".csv") || outPath.endsWith(".txt")))
    outPath += ".log";

  // Reads use the caller's identity so a non-admin can't stitch logs they
  // can't read. The output write uses system identity because the
  // /logging_captures/ tree grants CREATE/WRITE to SYSTEM only (admins get
  // just READ|DELETE, per the System_Filesystem.cpp path rules) — this is the
  // same reason the sensor logger writes here as systemAuth.
  const AuthContext& ctx    = currentAuthContext();
  const AuthContext  sysCtx = VFS::systemAuth("gps.stitch");
  FsLockGuard fsGuard("gpstrackmerge");

  // Validate every input up front (quoted, exists, not the output) so a bad
  // argument never leaves a half-written output behind.
  for (int i = 1; i < a.count(); i++) {
    String in;
    const char* e = requireQuotedPath(a, i, in);
    if (e) return e;
    if (in == outPath) return "Error: an input file is the same as the output";
    if (!VFS::existsGuarded(in, ctx)) {
      snprintf(buf, 1024, "Error: input not found: %s", in.c_str());
      return buf;
    }
  }

  VFS::mkdirGuarded("/logging_captures", sysCtx);
  VFS::mkdirGuarded("/logging_captures/tracks", sysCtx);

  File out = VFS::openGuarded(outPath, "w", sysCtx, true);
  if (!out) {
    snprintf(buf, 1024, "Error: cannot create output: %s", outPath.c_str());
    return buf;
  }

  uint8_t chunk[512];
  unsigned long totalBytes = 0;
  int filesDone = 0;
  for (int i = 1; i < a.count(); i++) {
    String in;
    requireQuotedPath(a, i, in);  // re-derive the path (already validated above)
    File f = VFS::openGuarded(in, "r", ctx);
    if (!f) {
      out.close();
      snprintf(buf, 1024, "Error: cannot open input: %s", in.c_str());
      return buf;
    }
    uint8_t lastByte = (uint8_t)'\n';
    while (f.available()) {
      int n = f.read(chunk, sizeof(chunk));
      if (n <= 0) break;
      if (out.write(chunk, (size_t)n) != (size_t)n) {  // short write == full/failing FS
        f.close();
        out.close();
        snprintf(buf, 1024, "Error: write failed (filesystem full?) after %lu bytes to %s",
                 totalBytes, outPath.c_str());
        return buf;
      }
      totalBytes += (unsigned long)n;
      lastByte = chunk[n - 1];
    }
    f.close();
    if (lastByte != (uint8_t)'\n') { out.write((uint8_t)'\n'); totalBytes++; }  // clean line boundary
    filesDone++;
  }
  out.close();

  snprintf(buf, 1024,
           "Stitched %d files into %s (%lu bytes). Load it: gpstrack load \"%s\"",
           filesDone, outPath.c_str(), totalBytes, outPath.c_str());
  return buf;
}

const char* cmd_waypoint(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();

  CommandArgs a(argsInput);
  String sub = a.arg(0);
  sub.toLowerCase();

  if (sub.length() == 0 || sub == "list") {
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

  if (sub == "add") {
    if (!a.hasMinArgs(3)) return "Error: invalid arguments — Usage: waypoint add <lat> <lon> [name]";
    float lat = a.argFloat(1);
    float lon = a.argFloat(2);
    String name = a.has(3) ? a.arg(3) : String("WP");
    if (name.length() == 0) name = "WP";
    int idx = WaypointManager::addWaypoint(lat, lon, name.c_str());
    if (idx >= 0) {
      snprintf(buf, 1024, "Added waypoint %d: %s", idx, name.c_str());
    } else {
      return "Error: No free waypoint slots";
    }
    return buf;
  }

  if (sub == "del") {
    if (!a.hasMinArgs(2)) return "Error: invalid arguments — Usage: waypoint del <index>";
    if (WaypointManager::deleteWaypoint(a.argInt(1))) {
      snprintf(buf, 1024, "Deleted waypoint %d", a.argInt(1));
    } else {
      return "Error: Invalid waypoint index";
    }
    return buf;
  }

  if (sub == "goto") {
    if (!a.hasMinArgs(2)) return "Error: invalid arguments — Usage: waypoint goto <index>";
    int idx = a.argInt(1);
    const Waypoint* wp = WaypointManager::getWaypoint(idx);
    if (wp) {
      WaypointManager::selectTarget(idx);
      snprintf(buf, 1024, "Navigation target: %s", wp->name);
    } else {
      return "Error: Invalid waypoint index";
    }
    return buf;
  }

  if (sub == "clearall") {
    WaypointManager::clearAll();
    return "All waypoints cleared";
  }

  if (sub == "clear") {
    WaypointManager::selectTarget(-1);
    return "Navigation target cleared";
  }

  if (sub == "rename") {
    if (!a.hasMinArgs(3)) return "Error: invalid arguments — Usage: waypoint rename <index> <name>";
    int idx = a.argInt(1);
    String name = a.remaining(1);  // everything after the index arg
    if (WaypointManager::setName(idx, name.c_str())) {
      snprintf(buf, 1024, "Renamed waypoint %d: %s", idx, name.c_str());
    } else {
      return "Error: Invalid waypoint index";
    }
    return buf;
  }

  if (sub == "notes") {
    if (!a.hasMinArgs(3)) return "Error: invalid arguments — Usage: waypoint notes <index> <notes>";
    int idx = a.argInt(1);
    String notes = a.remaining(1);  // everything after the index arg
    if (WaypointManager::setNotes(idx, notes.c_str())) {
      snprintf(buf, 1024, "Set notes for waypoint %d", idx);
    } else {
      return "Error: Invalid waypoint index";
    }
    return buf;
  }

  return "Error: invalid arguments — Usage: waypoint [list|add|del|goto|clear|clearall|rename|notes]";
}

// Link a file to a waypoint by GPS coordinates (creates waypoint if needed, or finds nearest)
const char* cmd_waypointfile(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();

  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) {
    return "Error: invalid arguments — Usage: waypointfile \"<file>\" <wpName>\n   or: waypointfile \"<file>\" <lat> <lon> [wpName]";
  }

  String filePathStr;
  const char* qerr = requireQuotedToken(a, 0, filePathStr);
  if (qerr) return qerr;
  char filepath[WAYPOINT_FILE_PATH_LEN];
  strlcpy(filepath, filePathStr.c_str(), sizeof(filepath));

  // Determine format: is arg(1) a numeric latitude?
  // strtof returns endptr == start if no conversion was performed.
  const String& arg1 = a.arg(1);
  char* endp;
  float lat = strtof(arg1.c_str(), &endp);
  bool hasLatLon = (endp != arg1.c_str() && *endp == '\0') && a.hasMinArgs(3);

  if (hasLatLon) {
    // Format: <filepath> <lat> <lon> [wpName]
    float lon = a.argFloat(2);
    char wpName[WAYPOINT_NAME_LEN] = "";
    strlcpy(wpName, a.arg(3).c_str(), sizeof(wpName));

    if (!VFS::existsGuarded(filepath, currentAuthContext())) {
      snprintf(buf, 1024, "Error: File not found: %s", filepath);
      return buf;
    }

    int wpIndex = -1;
    if (wpName[0]) {
      wpIndex = WaypointManager::findWaypointByName(wpName);
      if (wpIndex < 0) {
        wpIndex = WaypointManager::addWaypoint(lat, lon, wpName);
        if (wpIndex < 0) return "Error: No free waypoint slots";
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
      if (wpIndex < 0) return "Error: No nearby waypoint. Provide a name to create one.";
    }

    if (WaypointManager::addFile(wpIndex, filepath)) {
      const Waypoint* wp = WaypointManager::getWaypoint(wpIndex);
      snprintf(buf, 1024, "Linked %s to '%s' (%d files)",
               filepath, wp ? wp->name : "?", WaypointManager::getFileCount(wpIndex));
    } else {
      const Waypoint* wp = WaypointManager::getWaypoint(wpIndex);
      if (wp && wp->fileCount >= MAX_WAYPOINT_FILES) {
        snprintf(buf, 1024, "Error: '%s' has max files (%d)", wp->name, MAX_WAYPOINT_FILES);
      } else {
        return "Error: Failed to link (already linked?)";
      }
    }
    return buf;
  }

  // Format: <filepath> <wpName>
  char wpNameOnly[WAYPOINT_NAME_LEN];
  strlcpy(wpNameOnly, arg1.c_str(), sizeof(wpNameOnly));

  if (!VFS::existsGuarded(filepath, currentAuthContext())) {
    snprintf(buf, 1024, "Error: File not found: %s", filepath);
    return buf;
  }

  int wpIndex = WaypointManager::findWaypointByName(wpNameOnly);
  if (wpIndex < 0) {
    snprintf(buf, 1024, "Error: Waypoint not found: %s", wpNameOnly);
    return buf;
  }

  if (WaypointManager::addFile(wpIndex, filepath)) {
    snprintf(buf, 1024, "Linked %s to '%s' (%d files)",
             filepath, wpNameOnly, WaypointManager::getFileCount(wpIndex));
  } else {
    const Waypoint* wp = WaypointManager::getWaypoint(wpIndex);
    if (wp && wp->fileCount >= MAX_WAYPOINT_FILES) {
      snprintf(buf, 1024, "Error: '%s' has max files (%d)", wpNameOnly, MAX_WAYPOINT_FILES);
    } else {
      return "Error: Failed to link (already linked?)";
    }
  }
  return buf;
}

// List or remove files from a waypoint
const char* cmd_waypointfiles(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();

  CommandArgs a(argsInput);
  if (!a.hasMinArgs(1)) return "Error: invalid arguments — Usage: waypointfiles <wpName> [del <index>]";

  const String& wpName = a.arg(0);
  int wpIndex = WaypointManager::findWaypointByName(wpName.c_str());
  if (wpIndex < 0) {
    snprintf(buf, 1024, "Error: Waypoint not found: %s", wpName.c_str());
    return buf;
  }

  // Delete action: waypointfiles <wpName> del <index>
  if (a.arg(1) == "del") {
    if (!a.hasMinArgs(3)) return "Error: invalid arguments — Usage: waypointfiles <wpName> del <index>";
    int fileIdx = a.argInt(2);
    if (WaypointManager::removeFile(wpIndex, fileIdx)) {
      snprintf(buf, 1024, "Removed file %d from '%s'", fileIdx, wpName.c_str());
    } else {
      snprintf(buf, 1024, "Error: Invalid file index %d for '%s'", fileIdx, wpName.c_str());
    }
    return buf;
  }

  // List files
  int count = WaypointManager::getFileCount(wpIndex);
  if (count == 0) {
    snprintf(buf, 1024, "Waypoint '%s' has no files", wpName.c_str());
    return buf;
  }

  int offset = snprintf(buf, 1024, "Files for '%s' (%d):\n", wpName.c_str(), count);
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
  File f = VFS::openGuarded(fullPath, "r", currentAuthContext());
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
  if (!VFS::existsGuarded(srcPath, currentAuthContext())) { outErr = "src_missing"; return false; }
  if (!isMapFileByMagic(srcPath)) { outErr = "not_map_file"; return false; }
  if (!VFS::existsGuarded("/maps", currentAuthContext())) {
    if (!VFS::mkdirGuarded("/maps", currentAuthContext())) { outErr = "maps_mkdir_failed"; return false; }
  }
  char dstDir[64], dstMap[96];
  snprintf(dstDir, sizeof(dstDir), "/maps/%s", base.c_str());
  snprintf(dstMap, sizeof(dstMap), "%s/%s.hwmap", dstDir, base.c_str());
  if (srcPath == dstMap) { outErr = "already_organized"; return false; }
  if (!VFS::existsGuarded(dstDir, currentAuthContext())) {
    if (!VFS::mkdirGuarded(dstDir, currentAuthContext())) { outErr = "mkdir_failed"; return false; }
  }
  if (VFS::existsGuarded(dstMap, currentAuthContext())) { outErr = "dst_exists"; return false; }
  if (!VFS::renameGuarded(srcPath, dstMap, currentAuthContext())) { outErr = "rename_failed"; return false; }
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
  if (!VFS::existsGuarded(srcWp, currentAuthContext())) { outErr = "src_missing"; return false; }
  char dstDir[64], dstWp[128];
  snprintf(dstDir, sizeof(dstDir), "/maps/%s", base.c_str());
  snprintf(dstWp, sizeof(dstWp), "%s/%s", dstDir, wpFileName.c_str());
  if (!VFS::existsGuarded(dstDir, currentAuthContext())) { outErr = "dst_dir_missing"; return false; }
  if (VFS::existsGuarded(dstWp, currentAuthContext())) { outErr = "dst_exists"; return false; }
  if (!VFS::renameGuarded(srcWp, dstWp, currentAuthContext())) { outErr = "rename_failed"; return false; }
  return true;
}

// Append "name(reason)" to a bounded diagnostics string so maporganize can
// surface WHY a file failed to organize instead of only reporting failed=N.
static void appendOrganizeFailure(String& details, const String& name, const String& reason) {
  if (details.length() >= 400) return;  // keep the summary within the 1024-byte debug buffer
  if (details.length()) details += ", ";
  details += name;
  details += '(';
  details += reason;
  details += ')';
}

const char* cmd_maporganize(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();

  FsLockGuard guard("cmd_maporganize");
  File dir = VFS::openGuarded("/maps", "r", currentAuthContext());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return "Error: /maps directory not found";
  }

  int moved = 0, skipped = 0, failed = 0;
  String failDetails;  // "name(reason), ..." surfaced when files fail to organize
  File entry = dir.openNextFile();
  while (entry) {
    // On arduino-esp32 core 3.x, File::name() returns the BASENAME only
    // (e.g. "foo.hwmap"), NOT the full path — so downstream FS ops must be
    // given an absolute path. We rebuild "/maps/<name>", matching cmd_maplist.
    String leaf = String(entry.name());
    bool isDir = entry.isDirectory();
    entry.close();
    if (!isDir) {
      String fn = leaf;
      if (fn.startsWith("/maps/")) fn = fn.substring(6);
      if (fn.startsWith("/")) fn = fn.substring(1);
      if (fn.indexOf('/') == -1) {
        String srcFull = String("/maps/") + fn;  // absolute path for FS operations
        bool isMapByExt = fn.endsWith(".hwmap");
        bool isMapByMagic = (!isMapByExt && !fn.endsWith(".json")) ? isMapFileByMagic(srcFull) : false;
        if (isMapByExt || isMapByMagic) {
          String err;
          if (organizeMapFromAnyPath(srcFull, err)) moved++;
          else { failed++; appendOrganizeFailure(failDetails, fn, err); }
        } else if (fn.startsWith("waypoints_") && fn.endsWith(".json")) {
          String err;
          if (tryOrganizeLegacyWaypointsAtRoot(fn, err)) moved++;
          else { failed++; appendOrganizeFailure(failDetails, fn, err); }
        } else { skipped++; }
      } else { skipped++; }
    } else { skipped++; }
    entry = dir.openNextFile();
  }
  dir.close();
  if (failed > 0 && failDetails.length() > 0) {
    snprintf(buf, 1024, "Map organize: moved=%d skipped=%d failed=%d [%s]",
             moved, skipped, failed, failDetails.c_str());
  } else {
    snprintf(buf, 1024, "Map organize: moved=%d skipped=%d failed=%d", moved, skipped, failed);
  }
  return buf;
}

// Command registry
// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry mapCommands[] = {
  {"map", "Show current map info (add 'json' for JSON output)", false, cmd_map, nullptr},
  {"mapload", "Load map file: \"<path>\"", false, cmd_mapload, "Usage: mapload \"<path>\""},
  {"mapunload", "Unload current map (free PSRAM on device)", false, cmd_mapunload, nullptr},
  {"maplist", "List available maps (add 'json' for JSON output)", false, cmd_maplist, nullptr},
  {"whereami", "Show current location context (add 'json' for JSON output)", false, cmd_whereami, nullptr},
  {"search", "Search map features: <name>", false, cmd_search, "Usage: search <name>"},
  {"waypoint", "Manage waypoints: <list|add|del|goto|clear|clearall|rename|notes>", false, cmd_waypoint, "Usage: waypoint [list|add <lat> <lon> [name]|del <index>|goto <index>|clear|clearall|rename <index> <name>|notes <index> <notes>]"},
  {"gpstrack", "Manage GPS tracks: <status|load|clear>", false, cmd_gpstrack, "Usage: gpstrack [status|load <filepath>|clear]"},
  {"gpstrackmerge", "Stitch GPS logs in order: \"<out>\" \"<in1>\" \"<in2>\" ...", true, cmd_gpstrackmerge, "Usage: gpstrackmerge \"<output>\" \"<in1>\" \"<in2>\" [...]  (output first, then inputs in stitch order; max 9 inputs)"},
  {"waypointfile", "Link file to waypoint: \"<file>\" <wpName>", false, cmd_waypointfile, "Usage: waypointfile \"<file>\" <wpName> | waypointfile \"<file>\" <lat> <lon> [wpName]"},
  {"waypointfiles", "Waypoint files: <name> [del <idx>]", false, cmd_waypointfiles, "Usage: waypointfiles <wpName> [del <index>]"},
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
  // Runs on the OLED display task and reads tile data via loadTileData, so it
  // races the render task(s) on the shared LRU pool exactly like renderMap.
  // Hold the map lock across the whole scan (load + parse of every tile).
  MapCacheGuard mapGuard("LocationContext.updateContext");
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

// =============================================================================
// Maps Settings Module — persisted defaults under apps.maps
// =============================================================================
// Wired into the schema-driven settings registry. Field semantics:
//   mapZoom            — initial zoom applied to gMapZoom at boot
//   mapVisibleLayers   — initial visibility mask applied to gVisibleLayers
//   mapCacheSizeKB     — pool size for the tile LRU cache; takes effect at next
//                        map load (re-init of the pool happens per-map)
// CLI setters mirror the gSettings field into the matching live runtime
// variable so changes apply without a reboot (cache size aside).

static const char* cmd_mapzoom(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String v = argsInput; v.trim();
  if (v.length() == 0) return "Error: invalid arguments — Usage: mapzoom <0.5..20.0>";
  float f = v.toFloat();
  if (f < 0.5f) f = 0.5f;
  if (f > 20.0f) f = 20.0f;
  setSetting(gSettings.mapZoom, f);
  gMapZoom = f;
  snprintf(getDebugBuffer(), 1024, "mapZoom set to %.2f", f);
  return getDebugBuffer();
}

static const char* cmd_maplayers(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String v = argsInput; v.trim();
  if (v.length() == 0) return "Error: invalid arguments — Usage: maplayers <bitmask 0..1023>";
  int n = v.toInt();
  if (n < 0) n = 0;
  if (n > 0x3FF) n = 0x3FF;
  setSetting(gSettings.mapVisibleLayers, n);
  gVisibleLayers = (uint16_t)n;
  snprintf(getDebugBuffer(), 1024, "mapVisibleLayers set to 0x%03X", n);
  return getDebugBuffer();
}

static const char* cmd_mapcachekb(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String v = argsInput; v.trim();
  if (v.length() == 0) return "Error: invalid arguments — Usage: mapcachekb <256..4096>";
  int n = v.toInt();
  if (n < 256) n = 256;
  if (n > 4096) n = 4096;
  setSetting(gSettings.mapCacheSizeKB, n);
  snprintf(getDebugBuffer(), 1024, "mapCacheSizeKB set to %d (effective on next map load)", n);
  return getDebugBuffer();
}

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry mapsSettingEntries[] = {
  { "zoom",        SETTING_FLOAT, &gSettings.mapZoom,           0,    1.0f, nullptr, 0, 0,    "Default zoom (0.5-20.0)",            nullptr, false, nullptr, "mapzoom" },
  { "layers",      SETTING_INT,   &gSettings.mapVisibleLayers,  0x3FF, 0,   nullptr, 0, 0x3FF, "Visible layers (bitmask, 0-0x3FF)", nullptr, false, nullptr, "maplayers" },
  { "cacheSizeKB", SETTING_INT,   &gSettings.mapCacheSizeKB,    1280, 0,    nullptr, 256, 4096, "Tile cache size (KB, effective on next map load)", nullptr, false, nullptr, "mapcachekb" },
};

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule mapsSettingsModule = {
  "maps",
  "apps.maps",
  mapsSettingEntries,
  sizeof(mapsSettingEntries) / sizeof(mapsSettingEntries[0]),
  nullptr,
  "Map rendering defaults and tile cache"
};

// Apply persisted defaults to live runtime variables. Call after settings
// have loaded but before the first map render.
void mapsApplyPersistedSettings() {
  gMapZoom = gSettings.mapZoom;
  gVisibleLayers = (uint16_t)gSettings.mapVisibleLayers;
}

// CLI commands exposed to the global registry — picked up alongside
// mapCommands[] via the include in System_Command.cpp.
const CommandEntry mapsSettingCommands[] = {
  { "mapzoom",      "Set default map zoom: <0.5..20.0>",                 true, cmd_mapzoom,      "Usage: mapzoom <0.5..20.0>" },
  { "maplayers",    "Set visible layer bitmask: <0..1023>",              true, cmd_maplayers,    "Usage: maplayers <bitmask 0..1023>" },
  { "mapcachekb",   "Set tile cache size in KB (effective on next map load)",        true, cmd_mapcachekb,   "Usage: mapcachekb <256..4096>" },
};
const size_t mapsSettingCommandsCount = sizeof(mapsSettingCommands) / sizeof(mapsSettingCommands[0]);

#else // !ENABLE_MAPS

// Stubs so the linker is happy when maps are disabled
#include "System_Command.h"
const CommandEntry mapCommands[] = {};
const size_t mapCommandsCount = 0;
const CommandEntry mapsSettingCommands[] = {};
const size_t mapsSettingCommandsCount = 0;
float gMapRotation = 0;

#endif // ENABLE_MAPS
