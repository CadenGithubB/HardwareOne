# SD Card Integration Plan for HardwareOne

## Overview

This document outlines the work required to integrate SD card storage as a unified, removable storage volume alongside the existing LittleFS internal storage. The goal is to present SD card contents seamlessly in the file browser (web and OLED) and allow subsystems (camera, microphone, models, maps, etc.) to save to either storage location.

---

## Current State Assessment

### Existing SD Card Support

1. **ImageManager** (`System_ImageManager.cpp`)
   - Already has `ImageStorageLocation` enum: `IMAGE_STORAGE_LITTLEFS`, `IMAGE_STORAGE_SD`, `IMAGE_STORAGE_BOTH`
   - SD card initialization via `SPI.begin()` and `SD.begin(SD_CS_PIN)`
   - Mount prefix `/sd` for SD paths
   - Camera photos can already save to SD via `cameraStorageLocation` setting

2. **Pin Configuration** (`System_BuildConfig.h`)
   - `SD_CS_PIN` defined for XIAO Sense hat

3. **Camera Storage** (`System_Camera_DVP.cpp`)
   - `cameraStorageLocation` setting (0=LittleFS, 1=SD, 2=Both)
   - `cameraCaptureFolder` for save path

### Systems Currently LittleFS-Only

| System | Hardcoded Path | File Types |
|--------|----------------|------------|
| **Microphone** | `/recordings` | `.wav` audio |
| **Edge Impulse** | `/models` | `.tflite`, `labels.txt` |
| **GPS Maps** | `/maps` | `.hwmap`, `.gpx` |
| **Sensor Logs** | `/logs` | `.csv`, `.json` |
| **Automations** | `/system/automations.json` | JSON config |
| **User Data** | `/system/users.json` | JSON config |
| **Settings** | `/system/settings.json` | JSON config |

### File Browser Limitations

1. **FileManager** (`System_FileManager.cpp`)
   - Only operates on LittleFS via `LittleFS.open()`, `LittleFS.exists()`, etc.
   - No awareness of SD card mount point

2. **Web File Browser** (`WebServer_Server.cpp`)
   - API endpoints (`/api/files/*`) only access LittleFS
   - No SD card path handling

3. **OLED File Browser** (`OLED_Mode_FileBrowser.cpp`)
   - Uses `FileManager` class, inherits LittleFS-only limitation

---

## Implementation Plan

### Phase 1: Unified File System Abstraction Layer

**Goal:** Create a thin abstraction that routes file operations to LittleFS or SD based on path prefix.

#### 1.1 Create `System_VFS.h` / `System_VFS.cpp`

```cpp
// Virtual File System - routes to LittleFS or SD based on path
namespace VFS {
  enum StorageType { INTERNAL, SDCARD, AUTO };
  
  // Determine storage type from path
  StorageType getStorageType(const String& path);
  
  // Unified file operations
  bool exists(const String& path);
  File open(const String& path, const char* mode);
  bool mkdir(const String& path);
  bool remove(const String& path);
  bool rename(const String& oldPath, const String& newPath);
  bool rmdir(const String& path);
  
  // Storage stats
  bool getStats(StorageType type, size_t& total, size_t& used, size_t& free);
  bool isSDAvailable();
  
  // Path helpers
  String normalize(const String& path);  // Handle /sd/ prefix
}
```

**Path Convention:**
- `/sd/...` → routes to SD card
- All other paths → routes to LittleFS
- `/` root lists both LittleFS root and `/sd` as a virtual folder

#### 1.2 SD Card Hot-Plug Detection

- Periodic check for SD card presence (every 2-5 seconds)
- State change callback for UI refresh
- Graceful handling of SD removal during operations

### Phase 2: FileManager Updates

#### 2.1 Modify `FileManager` Class

- Replace direct `LittleFS.*` calls with `VFS::*` calls
- When at root `/`, inject virtual `/sd` folder if SD available
- Track current storage type for proper routing

#### 2.2 Root Directory Handling

```cpp
bool FileManager::loadDirectory() {
  // If at root, include both LittleFS contents AND /sd virtual folder
  if (strcmp(state.currentPath, "/") == 0) {
    // Add LittleFS root contents
    // Add "/sd" as virtual folder if SD available
  }
}
```

### Phase 3: Web API Updates

#### 3.1 Modify File API Endpoints (`WebServer_Server.cpp`)

| Endpoint | Change Required |
|----------|-----------------|
| `/api/files/list` | Route via VFS, include SD detection |
| `/api/files/read` | Route via VFS based on path |
| `/api/files/write` | Route via VFS based on path |
| `/api/files/delete` | Route via VFS based on path |
| `/api/files/upload` | Support `/sd/...` destination paths |
| `/api/files/download` | Support `/sd/...` source paths |

#### 3.2 Add SD Card Status Endpoint

```
GET /api/storage/status
Response: {
  "littlefs": { "total": 3145728, "used": 1234567, "free": 1911161 },
  "sd": { "available": true, "total": 32000000000, "used": 1000000, "free": 31999000000 },
  "sdInserted": true
}
```

### Phase 4: Subsystem Storage Location Settings

#### 4.1 Microphone

Add settings:
```cpp
int microphoneStorageLocation;   // 0=LittleFS, 1=SD, 2=Both
String microphoneRecordingsFolder;  // Default: "/recordings"
```

Update `System_Microphone.cpp`:
- Replace hardcoded `/recordings` with configurable path
- Use VFS for file operations

#### 4.2 Edge Impulse Models

Add settings:
```cpp
String edgeImpulseModelFolder;  // Default: "/models", can be "/sd/models"
```

Update `System_EdgeImpulse.cpp`:
- Use VFS for model loading
- Support models on SD card

#### 4.3 GPS Maps

Add settings:
```cpp
String gpsMapFolder;  // Default: "/maps", can be "/sd/maps"
```

#### 4.4 Sensor Logs

Add settings:
```cpp
int sensorLogStorageLocation;
String sensorLogFolder;
```

### Phase 5: UI/UX Updates

#### 5.1 Web File Browser

- Show SD card as removable folder icon at root
- Display "Eject" button when SD selected
- Show storage type indicator in file list
- Handle SD removal gracefully (refresh view)

#### 5.2 OLED File Browser

- Show SD card folder with distinct icon
- Display "SD" indicator in header when browsing SD

#### 5.3 Settings Pages

- Add storage location dropdowns for:
  - Camera captures
  - Microphone recordings
  - EI models folder
  - Map storage

---

## Technical Considerations

### 1. Thread Safety

- SD card operations may be slower; ensure mutex protection
- Handle concurrent access from web server, OLED UI, and sensor tasks
- Consider read/write locks for file operations

### 2. Path Normalization

```cpp
// Input: "//sd//photos//img.jpg" → Output: "/sd/photos/img.jpg"
String VFS::normalize(const String& path);
```

### 3. Error Handling

- SD card not present: Return clear error, fallback to LittleFS
- SD card full: Warn user, suggest cleanup
- SD card removed during write: Handle gracefully, mark operation failed

### 4. Protected Paths

Paths that should NEVER be on SD (security/reliability):
- `/system/settings.json`
- `/system/users.json`
- `/system/automations.json`

### 5. File Size Limits

- LittleFS: Individual file limit ~2MB practical
- SD card: FAT32 supports up to 4GB files
- Consider auto-routing large files to SD

### 6. Performance

- SD card SPI speed affects performance
- Consider buffered reads/writes for large transfers
- Camera streaming may need LittleFS for speed

---

## Implementation Order (Recommended)

1. **Week 1: VFS Abstraction**
   - Create `System_VFS.h/cpp`
   - Implement basic routing
   - Add SD hot-plug detection

2. **Week 2: FileManager Integration**
   - Update FileManager to use VFS
   - Implement root `/sd` virtual folder
   - Test OLED file browser

3. **Week 3: Web API**
   - Update all `/api/files/*` endpoints
   - Add storage status endpoint
   - Update web file browser UI

4. **Week 4: Subsystems**
   - Add storage location settings
   - Update microphone recording paths
   - Update EI model paths
   - Update map/log paths

5. **Week 5: Polish & Testing**
   - Error handling edge cases
   - Hot-plug reliability
   - Performance optimization
   - Documentation

---

## Files to Modify

### New Files
- `System_VFS.h` - Virtual file system header
- `System_VFS.cpp` - Virtual file system implementation

### Core Modifications
- `System_FileManager.h/cpp` - VFS integration
- `WebServer_Server.cpp` - API endpoint routing
- `System_Settings.h` - New storage location settings
- `System_Settings.cpp` - Settings handling

### Subsystem Modifications
- `System_Microphone.cpp` - Configurable recording path
- `System_EdgeImpulse.cpp` - Configurable model path
- `System_GPSMapRenderer.cpp` - Configurable map path
- `System_SensorLogging.cpp` - Configurable log path
- `System_ImageManager.cpp` - Already has SD support, minor updates

### UI Modifications
- `OLED_Mode_FileBrowser.cpp` - SD folder display
- `WebPage_Files.h` or similar - Web UI for SD indication

---

## Testing Checklist

- [ ] SD card detection at boot
- [ ] SD card hot-plug detection
- [ ] SD card removal handling
- [ ] File browser shows `/sd` at root
- [ ] Navigate into SD card
- [ ] Create folder on SD
- [ ] Upload file to SD via web
- [ ] Download file from SD via web
- [ ] Delete file on SD
- [ ] Rename file on SD
- [ ] Camera save to SD
- [ ] Microphone record to SD
- [ ] Load EI model from SD
- [ ] Load map from SD
- [ ] Mixed operations (copy from SD to internal)
- [ ] Large file handling (>1MB)
- [ ] Concurrent access (stream + file ops)

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| SD card corruption | Medium | High | Write buffering, safe eject |
| Performance degradation | Medium | Medium | Async operations, caching |
| Hot-plug race conditions | Medium | Medium | Mutex protection, state machine |
| Breaking existing paths | Low | High | Extensive testing, path normalization |
| Memory pressure | Low | Medium | Streaming file ops, no full-file buffers |

---

## Questions to Resolve

1. Should system files ever be allowed on SD? (Current recommendation: No)
2. Default storage location for new installs? (Current recommendation: LittleFS)
3. Auto-migrate files when SD inserted? (Current recommendation: No, manual only)
4. Support exFAT for >32GB cards? (Current: FAT32 only via SD library)

---

*Document created: January 2026*
*Last updated: January 21, 2026*
