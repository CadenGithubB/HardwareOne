# Multi-Camera Security System Implementation Plan (Option B: Dashboard Proxy)

## Overview
This plan outlines the implementation of a WiFi-based multi-camera security system where:
- Multiple camera-equipped devices serve camera endpoints over WiFi
- One "dashboard" device acts as a reverse proxy and presents a unified multi-view web interface
- All browser traffic goes through the dashboard (single-origin, centralized auth)
- No ESP-NOW involvement (separate from the ESP-NOW sensor system)

## Architecture Summary

```
Browser → Dashboard Device → Camera Device 1
                           → Camera Device 2
                           → Camera Device N
```

- **Browser** loads only from dashboard (e.g., `http://dashboard.local/cameras`)
- **Dashboard** proxies requests to individual cameras
- **Cameras** expose HTTP endpoints for snapshots/streams

---

## Phase 1: Camera Node Endpoints

### 1.1 Snapshot Endpoint
**Endpoint:** `GET /camera/snapshot.jpg`

**Purpose:** Return a single JPEG frame (baseline for multi-view grid)

**Implementation:**
- Handler in `WebServer_Server.cpp` or new `WebPage_Camera.cpp`
- Call existing camera capture function
- Set headers:
  - `Content-Type: image/jpeg`
  - `Content-Length: <size>`
  - Optional: `Cache-Control: no-store` (for live feeds)
- Write JPEG bytes directly to response
- Error handling: return 503 if camera not ready/enabled

**Code location:** `components/hardwareone/WebServer_Server.cpp` (add handler registration)

**Dependencies:**
- `System_Camera_DVP.cpp`: existing `captureFrame()` or similar
- Ensure camera is initialized and enabled

---

### 1.2 MJPEG Stream Endpoint (Optional, Phase 2)
**Endpoint:** `GET /camera/stream.mjpeg`

**Purpose:** Continuous multipart MJPEG stream for live view

**Implementation:**
- Set headers:
  - `Content-Type: multipart/x-mixed-replace; boundary=frame`
  - `Cache-Control: no-cache`
- Loop:
  - Capture frame
  - Write boundary: `--frame\r\n`
  - Write headers: `Content-Type: image/jpeg\r\nContent-Length: <size>\r\n\r\n`
  - Write JPEG bytes
  - Delay (FPS control, e.g., 100ms for ~10fps)
  - Check if client disconnected
- Cleanup on disconnect

**Considerations:**
- Blocks one HTTP server thread/connection
- Limit concurrent streams (e.g., max 2-3 per camera device)
- Add FPS throttling to avoid WiFi saturation

---

### 1.3 Camera Status API
**Endpoint:** `GET /api/camera/status`

**Purpose:** Return camera metadata/health for dashboard

**Response JSON:**
```json
{
  "enabled": true,
  "connected": true,
  "streaming": false,
  "model": "OV2640",
  "width": 800,
  "height": 600,
  "fps": 10,
  "quality": 12,
  "uptime": 123456
}
```

**Implementation:**
- Read from existing camera state variables
- Serialize to JSON
- Return with `Content-Type: application/json`

---

## Phase 2: Dashboard Node - Camera Registry

### 2.1 Camera Registry Data Structure
**Purpose:** Track known camera devices

**Storage options:**
- In-memory array/vector (lost on reboot, simplest)
- JSON file in LittleFS (persistent, recommended)

**Structure:**
```cpp
struct CameraDevice {
  char id[32];          // Unique ID (MAC or user-assigned)
  char name[64];        // Display name (e.g., "Kitchen Cam")
  char ip[16];          // IPv4 address
  uint16_t port;        // HTTP port (default 80)
  char snapshotPath[64]; // e.g., "/camera/snapshot.jpg"
  char streamPath[64];   // e.g., "/camera/stream.mjpeg"
  unsigned long lastSeen; // millis() of last successful contact
  bool online;          // Health check result
};
```

**File location:** `components/hardwareone/System_CameraRegistry.h/cpp` (new files)

**Functions:**
- `void initCameraRegistry()`
- `bool addCamera(const CameraDevice& cam)`
- `bool removeCamera(const char* id)`
- `CameraDevice* getCameraById(const char* id)`
- `String getCameraListJSON()`
- `void saveCameraRegistry()` / `void loadCameraRegistry()`

---

### 2.2 Camera Registration API
**Endpoint:** `POST /api/cameras/register`

**Purpose:** Allow cameras to announce themselves to dashboard on boot

**Request JSON:**
```json
{
  "id": "aabbccddeeff",
  "name": "Living Room Cam",
  "ip": "192.168.1.50",
  "port": 80,
  "snapshotPath": "/camera/snapshot.jpg",
  "streamPath": "/camera/stream.mjpeg"
}
```

**Response JSON:**
```json
{
  "success": true,
  "message": "Camera registered"
}
```

**Implementation:**
- Parse JSON body
- Validate fields (non-empty id, valid IP)
- Add/update camera in registry
- Save registry to file
- Return success/error

**Security consideration:**
- Add authentication (require login or shared secret)
- Rate limit to prevent spam

---

### 2.3 Camera List API
**Endpoint:** `GET /api/cameras`

**Purpose:** Return list of registered cameras for web UI

**Response JSON:**
```json
{
  "cameras": [
    {
      "id": "aabbccddeeff",
      "name": "Living Room Cam",
      "online": true,
      "lastSeen": 123456
    },
    {
      "id": "112233445566",
      "name": "Kitchen Cam",
      "online": false,
      "lastSeen": 100000
    }
  ]
}
```

**Implementation:**
- Iterate camera registry
- Build JSON array
- Return with `Content-Type: application/json`

---

## Phase 3: Dashboard Node - HTTP Proxy

### 3.1 Snapshot Proxy
**Endpoint:** `GET /cam/<id>/snapshot.jpg`

**Purpose:** Fetch snapshot from camera device and relay to browser

**Implementation:**
- Parse `<id>` from URL
- Look up camera in registry
- If offline/not found: return 404 or placeholder image
- Use HTTP client (e.g., `HTTPClient` library) to fetch:
  - `GET http://<camera_ip>:<port><snapshotPath>`
- Read response body (JPEG bytes)
- Relay to browser:
  - Set `Content-Type: image/jpeg`
  - Write bytes
- Error handling:
  - Timeout: mark camera offline, return error
  - Non-200 response: log and return error

**Code location:** `components/hardwareone/WebServer_Server.cpp` (add handler)

**Dependencies:**
- `HTTPClient` library (ESP32 Arduino)
- Camera registry

**Performance notes:**
- This is synchronous and blocks one server thread
- For multi-camera grid, browser will make parallel requests
- Consider timeout (e.g., 5 seconds max)

---

### 3.2 MJPEG Stream Proxy (Advanced)
**Endpoint:** `GET /cam/<id>/stream.mjpeg`

**Purpose:** Relay MJPEG stream from camera to browser

**Implementation:**
- Parse `<id>` from URL
- Look up camera in registry
- Open HTTP client connection to camera stream endpoint
- Set response headers:
  - `Content-Type: multipart/x-mixed-replace; boundary=frame`
- Loop:
  - Read chunk from camera stream
  - Write chunk to browser response
  - Check if browser disconnected
- Cleanup on disconnect

**Challenges:**
- Must handle multipart boundaries correctly
- Memory buffering (use small chunks, e.g., 4KB)
- Concurrent stream limit (dashboard CPU/RAM)

**Recommendation:** Start with snapshot-only grid, add stream proxy later

---

## Phase 4: Dashboard Node - Web UI

### 4.1 Multi-Camera Grid Page
**Endpoint:** `GET /cameras`

**Purpose:** Display all cameras in a grid layout

**HTML/JS structure:**
```html
<div id="camera-grid">
  <!-- Populated by JS -->
</div>

<script>
async function loadCameras() {
  const resp = await fetch('/api/cameras');
  const data = await resp.json();
  
  const grid = document.getElementById('camera-grid');
  grid.innerHTML = '';
  
  data.cameras.forEach(cam => {
    const tile = document.createElement('div');
    tile.className = 'camera-tile';
    tile.innerHTML = `
      <h3>${cam.name}</h3>
      <img src="/cam/${cam.id}/snapshot.jpg?t=${Date.now()}" 
           onerror="this.src='/images/offline.jpg'">
      <div class="status ${cam.online ? 'online' : 'offline'}">
        ${cam.online ? 'Online' : 'Offline'}
      </div>
    `;
    tile.onclick = () => openFullView(cam.id);
    grid.appendChild(tile);
  });
}

// Refresh snapshots every 1-5 seconds
setInterval(() => {
  document.querySelectorAll('.camera-tile img').forEach(img => {
    const src = img.src.split('?')[0];
    img.src = src + '?t=' + Date.now();
  });
}, 2000);

loadCameras();
</script>
```

**CSS:**
- Grid layout (e.g., 2x2 or 3x3)
- Responsive tiles
- Offline indicator styling

**File location:** `components/hardwareone/WebPage_Cameras.h/cpp` (new files)

---

### 4.2 Full-Screen Camera View
**Purpose:** Click a tile to view single camera in detail (MJPEG stream or high-res snapshot)

**Implementation:**
- Modal or new page
- Show `<img src="/cam/<id>/stream.mjpeg">` or refreshing snapshot
- Close button to return to grid

**Optional features:**
- PTZ controls (if camera supports)
- Recording trigger
- Snapshot download

---

## Phase 5: Health Monitoring

### 5.1 Periodic Health Checks
**Purpose:** Detect offline cameras and update registry

**Implementation:**
- FreeRTOS task or timer callback
- Every 30-60 seconds:
  - Iterate camera registry
  - For each camera:
    - Attempt `GET http://<ip>:<port>/api/camera/status` (or snapshot)
    - If success: mark online, update lastSeen
    - If timeout/error: mark offline
- Update registry in memory (optionally persist)

**Code location:** `components/hardwareone/System_CameraRegistry.cpp`

**Function:** `void cameraHealthCheckTask(void* param)`

---

## Phase 6: Authentication & Security

### 6.1 Dashboard Authentication
**Purpose:** Require login to access camera views

**Implementation:**
- Reuse existing user/session system
- Protect endpoints:
  - `/cameras` → require login
  - `/cam/<id>/*` → require login
  - `/api/cameras` → require login
- Redirect to `/login` if not authenticated

**Code location:** Existing auth middleware in `WebServer_Server.cpp`

---

### 6.2 Camera-to-Dashboard Authentication (Optional)
**Purpose:** Prevent rogue devices from registering

**Options:**
- Shared secret in registration request
- Pre-configured allow-list of camera IDs/MACs
- Certificate-based (advanced, probably overkill)

**Recommendation:** Start with shared secret in POST body

---

## Phase 7: Optional Enhancements

### 7.1 Motion Detection Integration
- Camera nodes send motion events to dashboard
- Dashboard highlights cameras with recent motion
- Optional: trigger recording/alerts

### 7.2 Recording/Snapshots
- Dashboard saves snapshots to SD card on motion/schedule
- Playback interface for saved clips

### 7.3 mDNS Discovery
- Cameras announce `_hardwareonecam._tcp` service
- Dashboard auto-discovers and registers cameras
- Reduces manual configuration

### 7.4 Quality/FPS Controls
- Web UI sliders to adjust per-camera quality/FPS
- Dashboard sends control commands to cameras

### 7.5 Bandwidth Optimization
- Thumbnail generation on camera or dashboard
- Adaptive quality based on network conditions
- Limit concurrent streams

---

## Implementation Order (Recommended)

1. **Camera Node: Snapshot endpoint** (simplest, proves camera→HTTP works)
2. **Dashboard Node: Camera registry + list API** (data layer)
3. **Dashboard Node: Snapshot proxy** (proves proxy works)
4. **Dashboard Node: Grid web page** (snapshot-only multi-view)
5. **Camera Node: Registration on boot** (auto-discovery)
6. **Dashboard Node: Health monitoring** (offline detection)
7. **Dashboard Node: Authentication** (security)
8. **Camera Node: MJPEG stream endpoint** (live view)
9. **Dashboard Node: Stream proxy** (full live multi-view)
10. **Enhancements** (motion, recording, etc.)

---

## Key Files to Create/Modify

### New Files
- `components/hardwareone/System_CameraRegistry.h`
- `components/hardwareone/System_CameraRegistry.cpp`
- `components/hardwareone/WebPage_Cameras.h`
- `components/hardwareone/WebPage_Cameras.cpp`

### Modified Files
- `components/hardwareone/WebServer_Server.cpp` (add route handlers)
- `components/hardwareone/System_Camera_DVP.cpp` (ensure snapshot function exists)
- `components/hardwareone/CMakeLists.txt` (add new source files)

---

## Testing Plan

1. **Single camera snapshot**: Verify `GET /camera/snapshot.jpg` returns valid JPEG
2. **Dashboard proxy**: Verify `GET /cam/<id>/snapshot.jpg` fetches and relays
3. **Multi-camera grid**: Load `/cameras` with 2+ cameras, verify all tiles update
4. **Offline handling**: Disconnect a camera, verify it's marked offline within 60s
5. **Authentication**: Verify `/cameras` redirects to login when not authenticated
6. **MJPEG stream**: Verify live stream works in detail view
7. **Bandwidth test**: Load grid with 4+ cameras, measure WiFi saturation

---

## Performance Considerations

- **WiFi bandwidth**: 4 cameras @ 800x600 JPEG @ 5fps ≈ 2-4 Mbps (manageable)
- **Dashboard CPU**: Proxy is lightweight for snapshots, heavier for streams
- **Camera CPU**: MJPEG encoding is camera-limited (already handled by existing code)
- **Concurrent connections**: Limit MJPEG streams to 2-3 per camera device
- **HTTP server threads**: Ensure enough threads for multi-camera proxy (check ESP32 config)

---

## Security Checklist

- [ ] Dashboard requires login for all camera endpoints
- [ ] Camera registration requires shared secret or allow-list
- [ ] No hardcoded credentials in code
- [ ] HTTPS (optional, hard on ESP32 but consider for production)
- [ ] Rate limiting on registration endpoint
- [ ] Input validation on all APIs (prevent injection)
- [ ] Camera endpoints only accessible on trusted LAN (firewall/VLAN)

---

## Future Considerations

- **Cloud relay**: For remote viewing outside LAN (requires external server or VPN)
- **Mobile app**: Native app instead of web UI
- **AI/ML**: On-device object detection, person recognition
- **Storage**: NAS integration for long-term recording
- **Alerts**: Push notifications, email on motion

---

## Notes

- This plan assumes existing camera capture code is functional
- Start with snapshot-only grid (simplest, most stable)
- MJPEG streaming is optional and can be added later
- Focus on reliability over features initially
- Test with 2-3 cameras before scaling to more

---

**Plan created:** 2026-01-23  
**Target implementation:** After codebase stabilization
