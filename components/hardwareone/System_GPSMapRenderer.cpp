#include <LittleFS.h>
#include <ArduinoJson.h>

#include "System_GPSMapRenderer.h"
#include "System_BuildConfig.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_MemUtil.h"
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

LoadedMap MapCore::_currentMap = {false, {}, nullptr, 0, ""};
bool gMapRendererEnabled = true;
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
    case MAP_FEATURE_RAILWAY:
      return {LINE_DASHED, 1, 7, true, 0x7BEF};  // Gray, dashed
    case MAP_FEATURE_BUILDING:
      return {LINE_NONE, 1, 1, false, 0x4208};   // Skip
    default:
      return {LINE_SOLID, 1, 5, true, 0xFFFF};
  }
}

// =============================================================================
// MapCore - Map File Loading (Display-Agnostic)
// =============================================================================

void initMapRenderer() {
  WaypointManager::loadWaypoints();
  INFO_SENSORSF("Map renderer initialized (%d waypoints)", WaypointManager::getActiveCount());
}

bool MapCore::loadMapFile(const char* path) {
  // Unload any existing map
  unloadMap();
  
  // Pause sensor polling during file I/O to prevent I2C contention
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;
  vTaskDelay(pdMS_TO_TICKS(50));  // Let any in-flight I2C complete
  
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
  
  // Validate version
  if (header.version != 1) {
    ERROR_SENSORSF("Unsupported map version: %u", header.version);
    f.close();
    gSensorPollingPaused = wasPaused;
    return false;
  }
  
  // Allocate memory for full file (prefer PSRAM)
  uint8_t* data = nullptr;
  if (psramFound()) {
    data = (uint8_t*)ps_malloc(fileSize);
  }
  if (!data) {
    data = (uint8_t*)malloc(fileSize);
  }
  
  if (!data) {
    ERROR_SENSORSF("Failed to allocate %zu bytes for map", fileSize);
    f.close();
    gSensorPollingPaused = wasPaused;
    return false;
  }
  
  // Read entire file
  f.seek(0);
  size_t bytesRead = f.read(data, fileSize);
  f.close();
  
  if (bytesRead != fileSize) {
    ERROR_SENSORSF("Failed to read full map: %zu/%zu bytes", bytesRead, fileSize);
    free(data);
    gSensorPollingPaused = wasPaused;
    return false;
  }
  
  // Store loaded map
  _currentMap.valid = true;
  memcpy(&_currentMap.header, &header, sizeof(header));
  _currentMap.data = data;
  _currentMap.dataSize = fileSize;
  _currentMap.hasMetadata = false;
  _currentMap.metadata = nullptr;
  _currentMap.metadataCount = 0;
  
  // Extract filename from path
  const char* fname = strrchr(path, '/');
  if (fname) fname++; else fname = path;
  strncpy(_currentMap.filename, fname, sizeof(_currentMap.filename) - 1);
  _currentMap.filename[sizeof(_currentMap.filename) - 1] = '\0';
  
  INFO_SENSORSF("Loaded map: %s (%zu bytes, %u features)", 
                _currentMap.filename, fileSize, header.featureCount);
  INFO_SENSORSF("Bounds: %.4f,%.4f to %.4f,%.4f",
                header.minLat / 1000000.0f, header.minLon / 1000000.0f,
                header.maxLat / 1000000.0f, header.maxLon / 1000000.0f);
  
  // Parse metadata if present (flags bit 0)
  if (header.flags & HWMAP_FLAG_HAS_METADATA) {
    // Calculate where features end to find metadata section
    size_t offset = sizeof(HWMapHeader);
    for (uint32_t i = 0; i < header.featureCount && offset < fileSize; i++) {
      if (offset + 2 > fileSize) break;
      uint8_t pointCount = data[offset + 1];
      // Skip: type(1) + count(1) + first point(8) + deltas((n-1)*4)
      offset += 2 + 8 + (pointCount > 0 ? (pointCount - 1) * 4 : 0);
    }
    
    // Check if metadata section exists at this offset
    if (offset + 6 <= fileSize) {
      uint32_t metaSize = *(uint32_t*)(data + offset);
      uint16_t entryCount = *(uint16_t*)(data + offset + 4);
      
      if (entryCount > 0 && entryCount <= MAX_METADATA_ENTRIES && offset + metaSize <= fileSize) {
        // Allocate metadata array (prefer PSRAM)
        MapMetadataEntry* meta = nullptr;
        if (psramFound()) {
          meta = (MapMetadataEntry*)ps_malloc(sizeof(MapMetadataEntry) * entryCount);
        }
        if (!meta) {
          meta = (MapMetadataEntry*)malloc(sizeof(MapMetadataEntry) * entryCount);
        }
        
        if (meta) {
          size_t metaOffset = offset + 6;  // Skip size(4) + count(2)
          uint16_t parsed = 0;
          
          for (uint16_t i = 0; i < entryCount && metaOffset + 2 <= fileSize; i++) {
            uint8_t category = data[metaOffset++];
            uint8_t strLen = data[metaOffset++];
            
            if (metaOffset + strLen > fileSize) break;
            
            meta[parsed].category = category;
            size_t copyLen = (strLen < sizeof(meta[parsed].name) - 1) ? strLen : sizeof(meta[parsed].name) - 1;
            memcpy(meta[parsed].name, data + metaOffset, copyLen);
            meta[parsed].name[copyLen] = '\0';
            
            metaOffset += strLen;
            parsed++;
          }
          
          _currentMap.metadata = meta;
          _currentMap.metadataCount = parsed;
          _currentMap.hasMetadata = true;
          
          INFO_SENSORSF("Parsed %u metadata entries", parsed);
        }
      }
    }
  }
  
  // Load waypoints for this map
  WaypointManager::loadWaypoints();
  
  // Resume sensor polling
  gSensorPollingPaused = wasPaused;
  
  return true;
}

void MapCore::unloadMap() {
  if (_currentMap.data) {
    free(_currentMap.data);
    _currentMap.data = nullptr;
  }
  if (_currentMap.metadata) {
    free(_currentMap.metadata);
    _currentMap.metadata = nullptr;
  }
  _currentMap.valid = false;
  _currentMap.dataSize = 0;
  _currentMap.filename[0] = '\0';
  _currentMap.hasMetadata = false;
  _currentMap.metadataCount = 0;
}

const MapMetadataEntry* MapCore::getMetadata(int index) {
  if (!_currentMap.hasMetadata || index < 0 || index >= _currentMap.metadataCount) {
    return nullptr;
  }
  return &_currentMap.metadata[index];
}

int MapCore::getMetadataByCategory(uint8_t category, const MapMetadataEntry** entries, int maxEntries) {
  if (!_currentMap.hasMetadata || !entries || maxEntries <= 0) {
    return 0;
  }
  
  int count = 0;
  for (int i = 0; i < _currentMap.metadataCount && count < maxEntries; i++) {
    if (_currentMap.metadata[i].category == category) {
      entries[count++] = &_currentMap.metadata[i];
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

int MapCore::getAvailableMaps(char maps[][32], int maxMaps) {
  int count = 0;
  
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
    const char* name = entry.name();
    size_t len = strlen(name);
    
    // Check for .hwmap extension
    if (len > 6 && strcasecmp(name + len - 6, ".hwmap") == 0) {
      strncpy(maps[count], name, 31);
      maps[count][31] = '\0';
      count++;
    }
    entry = dir.openNextFile();
  }
  dir.close();
  
  return count;
}

bool MapCore::autoSelectMap(float lat, float lon) {
  // If current map contains position, keep it
  if (isPositionInMap(lat, lon)) {
    return true;
  }
  
  // Scan /maps/ directory for a map containing this position
  char maps[8][32];
  int mapCount = getAvailableMaps(maps, 8);
  
  for (int i = 0; i < mapCount; i++) {
    char path[48];
    snprintf(path, sizeof(path), "/maps/%s", maps[i]);
    
    // Try loading to check bounds
    if (loadMapFile(path)) {
      if (isPositionInMap(lat, lon)) {
        INFO_SENSORSF("Auto-selected map: %s", maps[i]);
        return true;
      }
      unloadMap();
    }
  }
  
  return false;
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
  float x = (float)(dLon / scaleX);
  float y = -(float)(dLat / scaleY);  // Y is inverted (north = up)
  
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

void MapCore::renderMap(MapRenderer* renderer, float centerLat, float centerLon) {
  if (!_currentMap.valid || !renderer) return;
  
  int viewWidth = renderer->getWidth();
  int viewHeight = renderer->getHeight();
  
  // Convert center to microdegrees
  int32_t centerLatMicro = (int32_t)(centerLat * 1000000);
  int32_t centerLonMicro = (int32_t)(centerLon * 1000000);
  
  // Calculate scale: how many microdegrees per pixel
  // At ~40° latitude, 1 degree ≈ 85km lat, 65km lon
  // For a ~2km view on 128px width at default zoom
  // Apply zoom: higher zoom = smaller scale = more detail
  int32_t baseScaleY = 188;   // Microdegrees per pixel (latitude) at 1x
  int32_t baseScaleX = 246;   // Microdegrees per pixel (longitude) at 1x
  int32_t scaleY = (int32_t)(baseScaleY / gMapZoom);
  int32_t scaleX = (int32_t)(baseScaleX / gMapZoom);
  if (scaleX < 10) scaleX = 10;  // Prevent divide by zero
  if (scaleY < 10) scaleY = 10;
  
  // Parse and render features by priority (low priority first)
  // For simplicity, we do a single pass - could be optimized with sorting
  const uint8_t* ptr = _currentMap.data + sizeof(HWMapHeader);
  const uint8_t* end = _currentMap.data + _currentMap.dataSize;
  
  for (uint32_t f = 0; f < _currentMap.header.featureCount && ptr < end; f++) {
    // Read feature header
    if (ptr + 2 > end) break;
    uint8_t type = *ptr++;
    uint8_t pointCount = *ptr++;
    
    if (pointCount < 2) {
      // Skip malformed feature
      ptr += 8;  // First point
      continue;
    }
    
    // Get style for this feature type
    MapFeatureStyle style = renderer->getFeatureStyle((MapFeatureType)type);
    
    // Skip features that shouldn't be rendered
    if (!style.render || style.lineStyle == LINE_NONE) {
      // Skip past this feature's data
      ptr += 8 + (pointCount - 1) * 4;
      continue;
    }
    
    // Read first point (absolute)
    if (ptr + 8 > end) break;
    int32_t lat, lon;
    memcpy(&lat, ptr, 4); ptr += 4;
    memcpy(&lon, ptr, 4); ptr += 4;
    
    int16_t prevX, prevY;
    geoToScreen(lat, lon, centerLatMicro, centerLonMicro, 
                scaleX, scaleY, viewWidth, viewHeight, prevX, prevY);
    
    // Process remaining points (delta encoded)
    for (uint8_t p = 1; p < pointCount; p++) {
      if (ptr + 4 > end) break;
      
      int16_t deltaLat, deltaLon;
      memcpy(&deltaLat, ptr, 2); ptr += 2;
      memcpy(&deltaLon, ptr, 2); ptr += 2;
      
      lat += deltaLat;
      lon += deltaLon;
      
      int16_t curX, curY;
      geoToScreen(lat, lon, centerLatMicro, centerLonMicro,
                  scaleX, scaleY, viewWidth, viewHeight, curX, curY);
      
      // Simple visibility check - at least one point should be near screen
      bool visible = (prevX >= -50 && prevX < viewWidth + 50 &&
                      prevY >= -50 && prevY < viewHeight + 50) ||
                     (curX >= -50 && curX < viewWidth + 50 &&
                      curY >= -50 && curY < viewHeight + 50);
      
      if (visible) {
        renderer->drawLine(prevX, prevY, curX, curY, style);
      }
      
      prevX = curX;
      prevY = curY;
    }
  }
  
  // Draw waypoints on map
  WaypointManager::renderWaypoints(renderer, centerLat, centerLon, scaleX, scaleY);
  
  // Draw GPS position marker at center
  renderer->drawPositionMarker(viewWidth / 2, viewHeight / 2);
}

// =============================================================================
// OLEDMapRenderer Implementation
// =============================================================================

#if ENABLE_OLED_DISPLAY

// External OLED display pointer
extern Adafruit_SSD1306* oledDisplay;

OLEDMapRenderer::OLEDMapRenderer(Adafruit_SSD1306* display) : _display(display) {
  _width = 128;
  _height = 54;  // Leave room for footer
}

void OLEDMapRenderer::setViewport(int width, int height) {
  _width = width;
  _height = height;
}

void OLEDMapRenderer::clear() {
  // Don't clear - OLED display is managed by the mode system
}

void OLEDMapRenderer::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                const MapFeatureStyle& style) {
  if (!_display) return;
  
  // Clip check - at least one endpoint near screen
  if ((x0 < -20 || x0 > _width + 20 || y0 < -20 || y0 > _height + 20) &&
      (x1 < -20 || x1 > _width + 20 || y1 < -20 || y1 > _height + 20)) {
    return;
  }
  
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
  float dx = x1 - x0;
  float dy = y1 - y0;
  float len = sqrtf(dx*dx + dy*dy);
  if (len < 1) return;
  
  dx /= len;
  dy /= len;
  
  float x = x0, y = y0;
  bool draw = true;
  int segLen = 0;
  
  for (float t = 0; t < len; t += 1.0f) {
    if (draw) {
      _display->drawPixel((int16_t)x, (int16_t)y, SSD1306_WHITE);
    }
    x += dx;
    y += dy;
    segLen++;
    if (segLen >= dashLen) {
      segLen = 0;
      draw = !draw;
    }
  }
}

void OLEDMapRenderer::drawDottedLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int spacing) {
  float dx = x1 - x0;
  float dy = y1 - y0;
  float len = sqrtf(dx*dx + dy*dy);
  if (len < 1) return;
  
  dx /= len;
  dy /= len;
  
  for (float t = 0; t < len; t += spacing) {
    int16_t px = x0 + (int16_t)(dx * t);
    int16_t py = y0 + (int16_t)(dy * t);
    _display->drawPixel(px, py, SSD1306_WHITE);
  }
}

void OLEDMapRenderer::drawPositionMarker(int16_t x, int16_t y) {
  if (!_display) return;
  
  // Draw crosshair
  _display->drawLine(x - 4, y, x + 4, y, SSD1306_WHITE);
  _display->drawLine(x, y - 4, x, y + 4, SSD1306_WHITE);
  
  // Draw circle around crosshair
  _display->drawCircle(x, y, 3, SSD1306_WHITE);
}

void OLEDMapRenderer::drawOverlayText(int16_t x, int16_t y, const char* text, bool inverted) {
  if (!_display) return;
  
  _display->setCursor(x, y);
  if (inverted) {
    _display->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  } else {
    _display->setTextColor(SSD1306_WHITE);
  }
  _display->print(text);
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
      return {LINE_SOLID, 1, 8, true, 0xFFFF};
    case MAP_FEATURE_PARK:
      return {LINE_DOTTED, 1, 2, true, 0xFFFF};  // Dotted for parks
    case MAP_FEATURE_RAILWAY:
      return {LINE_DASHED, 1, 7, true, 0xFFFF};
    case MAP_FEATURE_BUILDING:
      return {LINE_DOTTED, 1, 1, true, 0xFFFF};  // Dotted for buildings
    default:
      return {LINE_SOLID, 1, 5, true, 0xFFFF};
  }
}

#endif // ENABLE_OLED_DISPLAY

// =============================================================================
// Command Handlers
// =============================================================================

const char* cmd_map(const String& cmd) {
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
           currentMap.dataSize,
           currentMap.header.minLat / 1000000.0f,
           currentMap.header.minLon / 1000000.0f,
           currentMap.header.maxLat / 1000000.0f,
           currentMap.header.maxLon / 1000000.0f);
  
  return getDebugBuffer();
}

const char* cmd_mapload(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: mapload <path>";
  while (*p == ' ') p++;
  
  if (MapCore::loadMapFile(p)) {
    const LoadedMap& currentMap = MapCore::getCurrentMap();
    if (!ensureDebugBuffer()) return "Map loaded";
    snprintf(getDebugBuffer(), 1024, "Loaded: %s (%lu features)", 
             currentMap.filename, currentMap.header.featureCount);
    return getDebugBuffer();
  }
  
  return "Failed to load map";
}

const char* cmd_maplist(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  char maps[8][32];
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

Waypoint WaypointManager::_waypoints[MAX_WAYPOINTS] = {};
int WaypointManager::_selectedTarget = -1;

bool WaypointManager::loadWaypoints() {
  const LoadedMap& map = MapCore::getCurrentMap();
  if (!map.valid) return false;
  
  // Use map-specific waypoint file: waypoints_<mapname>.json
  char wpPath[80];
  snprintf(wpPath, sizeof(wpPath), "/maps/waypoints_%s.json", map.filename);
  
  if (!LittleFS.exists(wpPath)) {
    return false;
  }
  
  File f = LittleFS.open(wpPath, "r");
  if (!f) return false;
  
  StaticJsonDocument<1024> doc;
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
    strlcpy(_waypoints[i].name, wp["name"] | "WP", WAYPOINT_NAME_LEN);
    _waypoints[i].active = true;
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
  
  StaticJsonDocument<1024> doc;
  JsonArray arr = doc.createNestedArray("waypoints");
  
  for (int i = 0; i < MAX_WAYPOINTS; i++) {
    if (_waypoints[i].active) {
      JsonObject wp = arr.createNestedObject();
      wp["lat"] = _waypoints[i].lat;
      wp["lon"] = _waypoints[i].lon;
      wp["name"] = _waypoints[i].name;
    }
  }
  
  doc["target"] = _selectedTarget;
  
  // Ensure /maps/ exists
  if (!LittleFS.exists("/maps")) {
    LittleFS.mkdir("/maps");
  }
  
  // Use map-specific waypoint file: waypoints_<mapname>.json
  char wpPath[80];
  snprintf(wpPath, sizeof(wpPath), "/maps/waypoints_%s.json", map.filename);
  
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
      strlcpy(_waypoints[i].name, name, WAYPOINT_NAME_LEN);
      _waypoints[i].active = true;
      saveWaypoints();
      return i;
    }
  }
  return -1;  // No free slots
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

const char* cmd_waypoint(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  
  // Parse subcommand
  const char* p = cmd.c_str();
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
  
  if (strncmp(p, "clear", 5) == 0) {
    WaypointManager::selectTarget(-1);
    return "Navigation target cleared";
  }
  
  return "Usage: waypoint [list|add|del|goto|clear]";
}

// Command registry
const CommandEntry mapCommands[] = {
  {"map", "Show current map info", false, cmd_map, nullptr},
  {"mapload", "Load a map file: mapload <path>", false, cmd_mapload, nullptr},
  {"maplist", "List available maps in /maps/", false, cmd_maplist, nullptr},
  {"waypoint", "Manage waypoints: list|add|del|goto|clear", false, cmd_waypoint, nullptr}
};
const size_t mapCommandsCount = sizeof(mapCommands) / sizeof(mapCommands[0]);

// Command module registration
static CommandModuleRegistrar _map_cmd_registrar(mapCommands, mapCommandsCount, "Map");
