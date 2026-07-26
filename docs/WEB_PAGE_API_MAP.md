# Web Page → API Usage Map

> Which web pages call which backend endpoints (and which CLI commands they scrape), built by
> reading each page's rendered HTML/JS. Use the **API Usage Index** to see each endpoint's
> consumers and the **Consolidation Findings** to plan merges and cleanup. Companion to
> [WEB_API_INVENTORY.md](WEB_API_INVENTORY.md).

## Overview

- **Pages mapped:** 20 (+ 1 shared layer)
- **Registered endpoints:** 78
- **Classification:** shared-layer 17 · multi-page 8 · single-page 35 · unused-by-pages 18

**Class meanings** — `shared-layer` called by the common JS every page inherits · `multi-page` called directly by 2+ pages · `single-page` called by exactly one page · `unused-by-pages` no page calls it (may still have a non-page consumer — see the index notes).

## The Shared Layer (inherited by every authenticated page)

Every page rendered through `streamPageWithContent` inherits this common JS (navigation, theme, dialogs, the file-browser modal, the `exec()`/`hw.*` REST helpers, and the central SSE). These endpoints are therefore used by *all* pages implicitly:

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/user/settings` | GET/POST | fetch | Load and save user theme preference (light/dark/system) |
| `/api/events` | GET | EventSource | SSE stream for real-time notifications and events; listens for 'notification' event type |
| `/api/icon` | GET | img-src | Serve 48x48 and 20x20 themed icon images for file explorer (name query param) |
| `/api/files/list` | GET | fetch | List directory contents for local file explorer; returns {success, files[], dirPerms} |
| `/api/files/read` | GET | img-src | Download/read file by name query param |
| `/api/files/view` | GET | img-src | View file in browser (opens in new tab for AVI, PDFs, etc.) |
| `/api/files/upload` | POST | xhr | Upload file to target path; tracks progress, handles binary/text mode |
| `/api/files/stats` | GET | fetch | Get storage stats {total, used, free} for path before upload |
| `/api/cli` | POST | fetch | Execute single CLI command; receives form-encoded {cmd} param |
| `/api/cli/batch` | POST | fetch | Execute multiple CLI commands atomically; receives JSON {commands: []} array |
| `/api/bond/status` | GET | fetch | Check bonded device status {bonded, role, peerMac, peerName, localMac} |
| `/api/bond/fs/list` | GET | fetch | List directory on bonded peer device; structured {entries[], success, error} |
| `/api/bond/fs/stat` | GET | fetch | Get storage stats on bonded peer {total, used, free, usagePercent} |
| `/api/bond/fs/get` | POST | fetch | Trigger file transfer from bonded peer to local landing directory |
| `/api/bond/exec` | POST | fetch | Execute command on bonded peer via bond token; receives form {cmd} |
| `/api/espnow/messages` | GET | fetch | Polling endpoint for ESP-NOW command responses from bonded peer; query params: since=seq, mac=peerMac |
| `/api/bond/cli/batch` | POST | fetch | Execute batched CLI commands on bonded peer; alternative to /api/cli/batch when target=bond |

Shared-layer CLI commands (issued via `/api/cli` by the file-browser modal):

- `rmdir "/path/to/folder"` — Delete empty folder via file explorer delete action
- `filedelete "/path/to/file"` — Initiate two-step file delete via cliConfirm (requires 'yes' in batch); part of Phase 3+4+5 confirmation flow
- `yes` — Confirm filedelete action in batch with cliConfirm; second command in [filedelete, yes] atomic batch
- `filerename "/old/path" "/new/name"` — Rename file or folder via file explorer rename action
- `filecreate "/path/to/newfile.ext"` — Create new empty file via file manager
- `mkdir "/path/to/newfolder"` — Create new folder via file manager

## Per-Page API Usage

Each page also inherits the shared layer above. The tables below list only each page's *direct* calls.

### `/dashboard`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/devices` | GET | fetch | Fetch device registry - list of connected I2C and other hardware devices |
| `/api/sensors/status` | GET | fetch | Get current sensor status (enabled/disabled state for IMU, thermal, ToF, APDS, input, GPS, FM radio, camera, microphone, etc.) |
| `/api/system` | GET | fetch | Fetch system status including time, uptime, memory (heap/PSRAM), storage, WiFi SSID/IP, connectivity info (ESP-NOW, MQTT, Bluetooth, webserver, I2C, LLM) |
| `/api/sessions` | GET | fetch | Fetch list of signed-in users/active sessions for admin view |
| `/api/user/settings` | GET/POST | fetch | Load/save user settings including dashboard layout preferences (dashboardSystemLayout, dashboardSystemHidden, dashboardConnLayout, dashboardConnHidden, dashboardSensorLayout, dashboardSensorHidden) |
| `/api/cli` | POST | xhr | Execute CLI commands for dashboard layout operations |
| `/api/events` | GET | EventSource | Server-sent events stream for real-time updates |

CLI commands (scraped via `/api/cli`):

- `dashboard layout <description>` — Log/record dashboard layout changes (panels and visibility)
- `dashboard layout reset <gridname>` — Reset dashboard layout to defaults for specified grid (sensor-grid, system-grid, or conn-grid)

**Notes:** The dashboard page inherits the shared layer (streamPageWithContent wraps with streamBeginHtml/streamEndHtml which includes navigation, common CSS, and common dialogs). The page uses EventSource('/api/events') to listen for 'sensor-status' and 'system' events for real-time updates. Layout customization modal includes show/hide/reorder functionality for panels, posting layout changes to /api/user/settings. The page makes separate fetches for device registry, sensor status, system status, and signed-in users on DOM load, plus periodic polling of /api/sessions every 15 seconds if admin user."

### `/settings`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/settings` | GET | fetch | Fetch device settings (system state, user info, features) |
| `/api/settings/schema` | GET | fetch | Fetch settings schema (module definitions, entry types, constraints) |
| `/api/buildconfig` | GET | fetch | Fetch build configuration to filter UI options |
| `/api/cli` | POST | xhr | Execute single CLI command (target-aware via postSettingsCli wrapper) |
| `/api/cli/batch` | POST | xhr | Execute batch of CLI commands with [beginwrite, ...cmds, savesettings] wrapper |
| `/api/files/list` | GET | fetch | List files in /system/certs directory for certificate management |
| `/api/files/upload` | POST | xhr | Upload certificate content to device |
| `/api/bond/status` | GET | fetch | Check bonded device status, role, peer MAC, settings hash |
| `/api/bond/exec` | POST | xhr | Execute single CLI command on bonded worker device |
| `/api/bond/cli/batch` | POST | xhr | Execute batch of CLI commands on bonded worker |
| `/api/bond/settings` | GET | fetch | Fetch settings from bonded worker |
| `/api/bond/settings/schema` | GET | fetch | Fetch settings schema from bonded worker |
| `/api/bond/settings/sync` | POST | xhr | Synchronize settings values from bonded worker to master |
| `/api/bond/settings/schema/sync` | POST | xhr | Synchronize settings schema from bonded worker to master |

CLI commands (scraped via `/api/cli`):

- `ledbrightness <val>` — Set LED brightness (0-100)
- `ledcolor <color>` — Set LED solid color
- `ledeffect fade <color1> <color2> <duration>` — Run LED fade effect
- `ledeffect <effect> <color> <duration>` — Run LED effect (blink, pulse, strobe)
- `ledclear` — Clear LED display
- `beginwrite` — Begin settings write transaction (sent as first command in batch)
- `savesettings` — Save settings to persistent storage (sent as last command in batch)
- `wifiautoreconnect <0\|1>` — Toggle WiFi auto-reconnect on boot
- `wifidisconnect` — Disconnect from current WiFi network
- `wifiscan json` — Scan for available WiFi networks, return JSON
- `wifiadd <ssid> <password> <1> <0\|1>` — Add visible WiFi credentials (last param: 0=not hidden, 1=hidden)
- `wificonnect` — Connect to saved WiFi network
- `bondmodeenabled` — Toggle bond mode enabled state
- `userlist json` — List all users, return JSON
- `sessionlist json` — List active sessions, return JSON
- `pendinglist json` — List pending user approvals, return JSON
- `useradd <username> <password> <0\|1>` — Add new user (last param: 0=no must-change, 1=must change password on first login)
- `userresetpassword <username> <password> <0\|1>` — Reset user password
- `userpromote <username>` — Promote user to admin
- `userdemote <username>` — Demote user from admin to regular user
- `userapprove <username>` — Approve pending user registration
- `userdeny <username>` — Deny and reject pending user registration
- `userdelete <username>` — Delete user (two-step confirm: userdelete X, yes)
- `banuser <username>` — Ban user from all access
- `unbanuser <username>` — Remove ban and restore user access
- `sessionrevoke user <username>` — Revoke all sessions for a user
- `serialrequireauth <0\|1>` — Toggle authentication requirement for serial output
- `displayrequireauth <0\|1>` — Toggle authentication requirement for display output
- `bluetoothrequireauth <0\|1>` — Toggle authentication requirement for Bluetooth
- `espnowusersync on\|off` — Enable/disable user sync over ESP-NOW
- `espnowdevices` — List bonded ESP-NOW peer devices
- `usersync <username> <userPass> <device> <targetAdminUser> <targetAdminPass> <yourAdminPass>` — Sync user account to another device over ESP-NOW
- `certgen` — Generate self-signed certificate
- `reboot` — Reboot the device

**Notes:** The /settings page is the primary device configuration interface. It uses a schema-driven approach via /api/settings/schema to render dynamic UI panels. The page supports bonded device mode (ESP-NOW master/worker setup) with target-aware routing: postSettingsCli() and sendSequential() dispatch to either /api/cli (local device) or /api/bond/exec + /api/bond/cli/batch (worker device) based on window._settingsTarget flag. The page inherits shared layer components (common dialogs hwConfirm/hwAlert, navigation, theme). Settings changes are batched with beginwrite/savesettings envelope via /api/cli/batch. Dynamic CLI commands constructed from form values include WiFi credentials, user passwords, device names. Certificate management uses file upload to /api/files/upload. Bonded view includes dirty-detection polling of /api/bond/status peerSettingsHash to warn if worker settings changed while form was rendered.

### `/files`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/files/stats` | GET | fetch | Fetch storage statistics (used, total, free, usage percent) for a given path to display in the storage bar |
| `/api/files/list` | GET | fetch | List files and directories in a given path for the file browser UI |
| `/api/files/read` | GET | fetch | Download/read a file by name; used by Download button in file browser |
| `/api/files/write` | POST | fetch | Save edited file content; POSTed as form data with name and content parameters |
| `/api/files/view` | GET | fetch | Open a file in a new tab for inline viewing (text/JSON/images/AVI); supports mode=pretty or mode=raw query params |
| `/api/files/upload` | POST | xhr | Upload a file with binary/text content and track progress via XHR upload events |
| `/api/cli` | POST | fetch | Execute CLI commands for file operations (mkdir, filecreate, rmdir, filerename) |
| `/api/cli/batch` | POST | fetch | Execute multiple CLI commands atomically; used for file delete confirmation flow (filedelete + yes) |
| `/api/bond/status` | GET | fetch | Check if device is bonded (paired with another device) and get master/worker role |
| `/api/bond/exec` | POST | fetch | Execute CLI command on bonded peer device over bond session |
| `/api/espnow/messages` | GET | fetch | Poll for command responses from bonded peer device (streamed output lines with doneMarker) |
| `/api/bond/fs/list` | GET | fetch | List files on bonded peer device filesystem (structured JSON response with entries) |
| `/api/bond/fs/stat` | GET | fetch | Get storage statistics (total, used, free, usagePercent) for bonded peer device |
| `/api/bond/fs/get` | GET | fetch | Initiate file pull from bonded peer device; returns size and ack status |
| `/api/icon` | GET | img-src | Serve themed file/folder icons (48x48 for files, 20x20 for action buttons) |
| `/api/videos/file` | GET | fetch | Fetch AVI/MJPEG video file for playback in embedded AVI player modal |

CLI commands (scraped via `/api/cli`):

- `rmdir` — Delete an empty folder via /api/cli
- `filerename` — Rename a file or folder via /api/cli
- `mkdir` — Create a new directory via /api/cli
- `filecreate` — Create a new empty file via /api/cli
- `filedelete` — Delete a file (requires two-step confirm via /api/cli/batch with 'yes' response)

**Notes:** The /files page is a comprehensive file manager for both local and bonded (peer) device filesystems. It relies heavily on the shared file browser utility (window.createFileManager, window.FileBrowser, window.BondFs) from WebServer_Utils.h. The page embeds an AVI player modal for playback of video recordings. Local file operations (edit, delete, rename, create) use either direct API endpoints (/api/files/*) or CLI commands via /api/cli(/batch). Bonded device operations use a separate set of endpoints (/api/bond/fs/* and /api/bond/exec) that tunnel commands to the peer. File upload uses XMLHttpRequest for progress tracking. Icons are fetched from /api/icon with fallback text rendering. The page does NOT use EventSource or WebSockets; it polls /api/espnow/messages for bonded command responses."

### `/cli`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/cli/logs` | GET | fetch | Fetch CLI output logs on page load and periodically poll (every 500ms) for new output while on local device |
| `/api/cli` | POST | fetch | Execute a CLI command with form body {cmd: string, capture: '1'} via hw.postFormText helper |
| `/api/user/settings` | GET/POST | fetch | Load and save user theme preference (via shared layer theme system) |
| `/api/bond/status` | GET | fetch | Check bonded device status to reveal target toggle when bonded as master (via shared BondFs helper) |
| `/api/bond/exec` | POST | fetch | Execute command on bonded peer with form body {cmd: string} (via shared BondFs helper) |
| `/api/espnow/messages` | GET | fetch | Poll for command output from bonded peer via ESPNow message stream (via shared BondFs helper) |
| `/api/files/list` | GET | fetch | List directory contents for file browser (via shared file-browser utilities embedded in page) |
| `/api/files/read` | GET | fetch | Download file via link href attribute (via shared file-browser utilities) |
| `/api/files/view` | GET | fetch | View file in browser (via shared file-browser utilities) |
| `/api/files/stats` | GET | fetch | Get storage stats before uploading file (via shared file-browser utilities) |
| `/api/files/upload` | POST | xhr | Upload file with progress tracking (via shared hwUploadFile utility) |
| `/api/icon` | GET | img-src | Fetch file/folder icons for file browser display (via shared file-browser icon renderer) |
| `/api/bond/fs/list` | GET | fetch | List bonded peer filesystem via structured API (via shared BondFs helper) |
| `/api/bond/fs/stat` | GET | fetch | Get bonded peer storage stats (via shared BondFs helper) |

CLI commands (scraped via `/api/cli`):

- `Any user-entered CLI command string` — Executed via hw.postFormText to /api/cli endpoint when user types and presses Enter
- `Any bonded-device command string` — Executed via window.BondFs.exec when bonded device mode is enabled
- `mkdir "<path>"` — Create folder via file-browser utilities (shared layer)
- `filecreate "<path>"` — Create file via file-browser utilities (shared layer)
- `filerename "<path>" "<newname>"` — Rename file/folder via file-browser utilities (shared layer)
- `rmdir "<path>"` — Delete empty folder via file-browser utilities (shared layer)
- `filedelete "<path>"` — Delete file with two-step confirm via hw.cliConfirm (shared layer) - followed by 'yes' in batch

**Notes:** Page embeds entire file-browser utility (window.createFileExplorer) and bonded-device helper (window.BondFs) from shared layer. /api/cli/logs is polled every 500ms but automatically stops when user switches to bonded device mode to avoid socket exhaustion. Bonded device commands are routed through ESPNow message polling rather than direct API calls. File operations in the embedded file browser use CLI commands (mkdir, filecreate, etc) routed through /api/cli, not dedicated file APIs. Icon requests use /api/icon with ?name= query param for file-type icons.

### `/logging`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/settings` | GET | fetch | Fetch user settings including isAdmin flag to determine if admin log toggle should be shown |
| `/api/cli` | POST | fetch | Execute single CLI commands for sensor/system logging control |
| `/api/cli/batch` | POST | fetch | Execute multiple CLI commands in a single batch request: ['time', 'sensorlog status', 'log status'] |
| `/api/files/list` | GET | fetch | List files in a directory path for file browser navigation |
| `/api/files/view` | GET | fetch | View/fetch log file content with mode=raw parameter for plain text streaming |
| `/api/files/stats` | GET | fetch | Fetch file statistics for the current directory |
| `/api/bond/status` | GET | fetch | Check bonded device status to determine if bonded logs panel should be shown |
| `/api/icon` | GET | img-src | Serve icon images for file types (folder, file, file_text, file_code, etc.) |
| `/api/events` | GET | EventSource | Server-Sent Events stream for real-time log/status updates (used by bonded device logging via BondFs) |

CLI commands (scraped via `/api/cli`):

- `time` — Get current system time for generating timestamped log file names
- `sensorlog status` — Query current sensor logging status (active/inactive, file, interval, format, sensors)
- `log status` — Query current system logging status (active/inactive, file, output flags)
- `sensorlog start <path> <interval_ms>` — Start sensor data logging to specified file path at given interval
- `sensorlog stop` — Stop active sensor logging
- `sensorlog autostart` — Toggle auto-start flag for sensor logging on device boot
- `sensorlog format <text\|csv\|track>` — Set sensor logging output format
- `sensorlog maxsize <bytes>` — Set maximum log file size before rotation (10KB-10MB)
- `sensorlog rotations <count>` — Set number of old log files to keep (0-9)
- `sensorlog sensors <sensor_list\|none>` — Configure which sensors to include in logging (thermal,tof,imu,gamepad,apds,gps,presence)
- `log start <path> [flags=0x...] [tags=0\|1]` — Start system debug logging with optional flag mask and category tag inclusion
- `log stop` — Stop active system logging
- `log autostart` — Toggle auto-start flag for system logging on device boot

**Notes:** The page uses the shared hw object (hw.fetchJSON, hw.postJSON, hw.postFormText) which is defined in the shared layout layer. Batch optimization: initial page load uses /api/cli/batch to fetch 'time', 'sensorlog status', and 'log status' in a single request, reducing from 3+ requests to 1. File browser uses window.FileBrowser shared rendering helpers for consistent icon mapping across local and bonded device explorers. The page includes support for viewing bonded device logs via window.BondFs helpers when device is bonded and is master. Log viewer parses three formats: debug logs with timestamps/categories, command audit logs (user@source), and simple timestamp messages. Debug flag system supports 40+ logging categories via 64-bit bitmask (flags parameter in 'log start' command)."

### `/automations`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/cli` | POST | fetch | Execute CLI commands for automation management: automation system status, enable/disable, add, delete, run, trigger, enable/disable individual automations |
| `/api/automations` | GET | fetch | Fetch list of all automations in JSON format for rendering the automations table |
| `/api/automations/export` | GET | fetch | Download all automations as a single JSON export file |

CLI commands (scraped via `/api/cli`):

- `automation system status` — Check if automation system is enabled or disabled
- `automation system enable` — Enable the automation system and start the scheduler
- `automation system disable` — Disable the automation system and stop the scheduler
- `automation add name=<name> type=<type> [time=<time>] [recurrence=<recurrence>] [days=<days>] [delayms=<ms>] [intervalms=<ms>] [commands=<cmds>] [enabled=<0\|1>] [runatboot=<0\|1>] [condition=<cond>] [triggermode=<mode>] [secondarytriggers=<json>] [id=<id>]` — Create or update an automation with specified parameters
- `automation delete id=<id>` — Delete an automation by ID
- `automation enable id=<id>` — Enable an automation
- `automation disable id=<id>` — Disable an automation
- `automation run id=<id>` — Execute an automation immediately
- `automation trigger id=<id>` — Trigger an after-delay automation (arm it for execution)

**Notes:** The page uses helper methods from the shared hw object (hw.postFormText, hw.fetchText, hw.fetchJSON) which abstract over fetch() calls. Both validate and execute flows POST to /api/cli with validate='1' parameter for dry-run validation before actual execution. The import feature fetches JSON from GitHub URLs via plain fetch() (line 1532). Multi-trigger automations are serialized as JSON in the secondarytriggers parameter. The page auto-loads automations on status-check completion and updates UI based on responses. EventSource not used; polling via interval timers for automation state updates.

### `/login`

_No direct backend calls (shared layer only)._

**Notes:** The /login page is a public authentication page (isPublic=true in streamBeginHtml) that does not use the shared layer features. The login form is a simple HTML form with method='POST' action='/login' - traditional form submission, not a dynamic backend data call. The page includes a basic JavaScript window load event that checks sessionStorage for a 'revokeMsg' key and displays it via alert() if present, but this is client-side only. No fetch(), EventSource, XMLHttpRequest, or CLI commands are used. The form submission to /login is handled server-side via the handleLogin POST handler which validates credentials and performs a 303 redirect on success (to /dashboard or /account/password-change) or re-renders the login page with an error message on failure."

### `/account/password-change`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/user/settings` | GET/POST | fetch | Load and save user theme preference (shared layer theme system, hw.loadThemePref() and hw.saveThemePref()) |
| `/api/events` | GET | EventSource | Real-time SSE stream for notifications; page listens for 'notification' event type |

**Notes:** Password-change page is a simple server-rendered HTML form with no page-specific JavaScript. The form POSTs directly to /account/password-change (traditional form submission, not fetch). All additional API calls come from the shared layer: (1) theme loading on page init via hw.loadThemePref(), (2) theme saving on user click via hw.saveThemePref(), (3) EventSource to /api/events for toast notifications. The page has no CLI commands, no image streams, no form-action overrides, and no page-specific fetch calls."

### `/icons/test`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/icon` | GET | img-src | Fetch embedded icon PNG by name parameter (name=<iconname>) |

**Notes:** Page is authentication-required (WEB_AUTH_OR_RETURN) but fully static/client-side after initial render. The /api/icon endpoint is called once per icon in the gallery via <img src=> tags. All interactivity (filtering, resizing) is pure client-side DOM manipulation with no backend calls. Page does not inherit shared layer features (nav, dialogs, theme, etc.)."

### `/sensors`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/devices` | GET | fetch | Fetch list of I2C devices connected to the system for sensor auto-detection (device name, address) |
| `/api/sensors/status` | GET | fetch | Fetch current sensor enabled/compiled flags, queue states, and streaming status; used for status polling (1000ms interval) and SSE updates; fields include imuEnabled, thermalEnabled, tofEnabled, cameraEnabled, micEnabled, eiEnabled, etc. |
| `/api/sensors` | GET | fetch | Fetch sensor data by query parameter (?sensor=<type>); supports: thermal, tof, imu, input (gamepad), anoencoder, fmradio, camera, microphone, presence, gps, rtc; returns JSON with sensor-specific fields |
| `/api/sensors/remote` | GET | fetch | Fetch list of remote devices with sensors via ESP-NOW; optional ?device=<MAC>&sensor=<type> to get specific remote sensor data |
| `/api/settings` | GET | fetch | Fetch device settings including thermal sensor settings (thermalPollingMs, palette, EWMA factor, interpolation), ToF settings (tofPollingMs, stability threshold, max distance, transition ms) |
| `/api/cli` | POST | fetch | Execute CLI commands via form POST with {cmd: <command>}; used for sensor control (open/close), camera adjustments, EI ML operations, and general device commands |
| `/api/events` | GET | EventSource | Server-Sent Events stream for real-time sensor-status updates; page listens for 'sensor-status' event |
| `/api/sensors/camera/frame` | GET | fetch | Fetch single JPEG frame from camera sensor; used by capture button and streaming tick animation (with ?t=timestamp for cache-busting) |
| `/api/sensors/camera/stream` | GET | fetch | MJPEG multipart stream endpoint for continuous camera video; returns continuous JPEG frames with multipart/x-mixed-replace boundary |
| `/api/videos` | GET | fetch | Fetch list of video recordings from SD card (camera MJPEG AVI files); returns {count, sdAvailable, files: [{name, size}]} |
| `/api/videos/file` | GET | fetch | Download a recorded camera video file by ?name=<filename> |
| `/api/recordings` | GET | fetch | Fetch list of microphone audio recordings; returns {count, files: [{name, size}]} |
| `/api/recordings/file` | GET | fetch | Stream microphone recording WAV file by ?name=<filename>; used by <audio> playback elements |
| `/api/files/upload` | POST | fetch | Upload .tflite ML model file for Edge Impulse; form POST with {path: '/EI Models/<filename>', binary: '1', content: <base64>} |
| `/api/ei/organize` | POST | fetch | Reorganize ML model files into proper directory structure; POST with empty JSON body |
| `/api/edgeimpulse/detect` | GET | fetch | Run Edge Impulse ML inference on current camera frame; returns {success, detections: [{label, confidence, x, y, width, height}], trackedObjects, inferenceTimeMs, modelInputSize} |

CLI commands (scraped via `/api/cli`):

- `openimu` — Open/enable IMU (BNO055) sensor and start data collection
- `closeimu` — Close/disable IMU sensor
- `openthermal` — Open/enable thermal camera (MLX90640) and start capture
- `closethermal` — Close/disable thermal camera
- `opentof` — Open/enable ToF distance sensor (VL53L4CX)
- `closetof` — Close/disable ToF sensor
- `openinput` — Open/enable gamepad or ANO encoder (unified HAL_Input command)
- `closeinput` — Close/disable gamepad or ANO encoder
- `opengps` — Open/enable GPS module (PA1010D)
- `closegps` — Close/disable GPS module
- `opencamera` — Open/enable DVP camera sensor
- `closecamera` — Close/disable DVP camera sensor
- `openmic` — Open/enable microphone sensor
- `closemic` — Close/disable microphone sensor
- `camerarecord start` — Start recording camera stream to SD card as MJPEG AVI
- `camerarecord stop` — Stop camera recording and finalize file
- `cameravideodelete <filename>` — Delete a camera video recording file
- `camerahmirror [on\|off]` — Toggle or set horizontal mirror on camera
- `cameravflip [on\|off]` — Toggle or set vertical flip on camera
- `camerarotate [on\|off]` — Toggle 180-degree rotation (equivalent to hmirror + vflip)
- `cameraframesize <size>` — Set camera resolution (0-10 framesize enum values)
- `cameraexposure <-2 to 2>` — Adjust camera exposure compensation
- `camerabrightness <-2 to 2>` — Adjust camera brightness
- `cameracontrast <-2 to 2>` — Adjust camera contrast
- `camerasaturation <-2 to 2>` — Adjust camera saturation
- `cameraquality <0-63>` — Set JPEG quality (0=best, 63=worst)
- `camerafps <1-20>` — Set camera stream FPS
- `camerasave` — Save current camera frame to storage
- `micrecord start` — Start recording audio from microphone
- `micrecord stop` — Stop microphone recording
- `micdelete <filename>` — Delete a microphone recording file
- `micgain <0-100>` — Set microphone input gain percentage
- `micsamplerate <8000\|16000\|22050\|44100\|48000>` — Set microphone sample rate in Hz
- `micbitdepth <16\|32>` — Set microphone bit depth
- `ei enable <0\|1>` — Enable or disable Edge Impulse ML
- `ei detect` — Run single Edge Impulse detection on current camera frame
- `ei continuous <0\|1>` — Enable or disable continuous Edge Impulse inference polling
- `ei confidence <0.1-1.0>` — Set minimum confidence threshold for ML detections
- `set edgeimpulse intervalMs <100-5000>` — Set ML inference polling interval in milliseconds
- `ei model list` — List available .tflite models in /EI Models directory
- `ei model load <filename.tflite>` — Load a .tflite model for inference
- `ei status` — Query current Edge Impulse status (enabled, model path, continuous state)
- `openfmradio` — Open/enable FM radio (RDA5807) receiver
- `closefmradio` — Close/disable FM radio
- `fmradioseek up` — Seek to next FM station
- `fmradioseek down` — Seek to previous FM station
- `fmradiotune <freq>` — Tune FM radio to specific frequency (e.g., 103.9)
- `fmradiovolume <0-15>` — Set FM radio volume level
- `fmradiomute` — Mute FM radio audio
- `fmradiounmute` — Unmute FM radio audio

**Notes:** The /sensors page relies heavily on shared layer helpers (hw.fetchJSON, hw.postForm, hw._ge, hw.on, etc.) defined in WebServer_Utils. The page uses hw.pollJSON() for periodic status polling (1000ms interval) and listens for 'sensor-status' events via EventSource (/api/events). All sensor data fetches use query parameter ?sensor=<type> except for status which has dedicated /api/sensors/status endpoint. The page implements per-sensor polling intervals (e.g., IMU 200ms, gamepad/anoencoder 56ms, thermal/ToF configurable via settings). Remote sensors are polled separately via /api/sensors/remote with optional ?device=<MAC>&sensor=<type> parameters. Camera streaming uses MJPEG multipart format with automatic takeover semantics (single stream lock per session). Microphone recordings auto-refresh when recording stops (500ms delay). EI ML models are uploaded as base64-encoded .tflite files. All sensor commands follow open<sensor>/close<sensor> pattern except unified input HAL (openinput/closeinput for both gamepad and ANO encoder)."

### `/llm`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/llm/status` | GET | fetch | Poll LLM engine state (READY/GENERATING/UNLOADED/ERROR), model info, performance metrics |
| `/api/llm/models` | GET | fetch | Fetch list of available .bin model files with size and storage location |
| `/api/llm/generate` | POST | fetch | Start async text generation with prompt and parameters (temperature, top_p, rep_penalty, sentence_limit, hard_cap) |
| `/api/llm/result` | GET | fetch | Poll for streamed generation output by session ID and byte offset; returns text chunks and done flag |
| `/api/llm/load` | POST | fetch | Load a model by path with optional max_ctx parameter |
| `/api/llm/unload` | POST | fetch | Unload the currently loaded model and free memory |
| `/api/llm/stop` | POST | fetch | Abort in-progress text generation |
| `/api/cli` | POST | fetch | Execute CLI command (used by Do: suggestion feature to run extracted commands) |

**Notes:** The page polls LLM state and generation output via repeated fetch calls (hw.fetchJSON). The /api/llm/result endpoint is polled every ~150ms during generation. Do: mode uses /api/cli to run extracted commands. The page relies entirely on polling architecture rather than EventSource/WebSocket. All fetch calls include auth (credentials via shared hw helpers)."

### `/maps`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/files/view` | GET | fetch | Load map file (.hwmap) binary data for parsing in browser |
| `/api/cli` | POST | xhr | Execute CLI commands for map device operations |
| `/api/waypoints` | GET/POST | xhr | Get/manage waypoints list; POST actions: add, delete, rename, set_notes, goto, clear, clear_all |
| `/api/gps/tracks` | GET | xhr | List GPS log files; load track from file; live tracking status and polling |
| `/api/maps/features` | GET | xhr | Get map metadata and feature information for display |
| `/api/system` | GET | xhr | Check system status (I2C enabled state) |
| `/api/sensors/status` | GET | xhr | Poll GPS sensor status and position data |

CLI commands (scraped via `/api/cli`):

- `mapunload` — Free device-side map cache in PSRAM and close file handle
- `mapload <path>` — Load a map file on device for waypoint API and tile access

**Notes:** The page renders a complex interactive map viewer with canvas-based rendering. It includes: (1) Map file loading via /api/files/view as binary ArrayBuffer parsed client-side; (2) Device map management via /api/cli with mapload/mapunload; (3) Waypoint CRUD via /api/waypoints with POST form data; (4) GPS track file listing/loading/live tracking via /api/gps/tracks with query params (?file=, ?live=start/stop/1); (5) System status polling via /api/system and /api/sensors/status for GPS availability; (6) Map feature metadata via /api/maps/features. All fetch calls use hw.fetchJSON() or hw.postForm() helpers (shared layer). The page auto-polls waypoints every 5s and optionally polls live GPS tracks every 1s. File upload via file browser uses hwUploadFile() helper (shared layer). No EventSource or WebSocket calls."

### `/espnow`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/cli` | POST | xhr | Execute CLI commands via hw.postFormText(). All ESP-NOW operations route through this endpoint. |
| `/api/cli/batch` | POST | xhr | Execute multiple CLI commands in a single request via hw.postJSON(). Used on page load to fetch status, mode, bond status, device list, meshes, device info, mesh role, and mesh status. |
| `/api/espnow/messages` | GET | xhr | Fetch received and sent ESP-NOW text messages with optional filtering. Accepts query params: ?since=<seqNum> for pagination, ?mac=<MAC> for per-device filtering. Returns JSON with messages array and deliveries snapshot. |
| `/api/espnow/metadata` | GET | xhr | Fetch cached device metadata (friendly name, room, zone, tags, stationary flag) for a specific device. Query param: ?mac=<MAC>. Returns JSON with device info and source (mesh/bonded/cached). |
| `/api/espnow/remotecap` | GET | xhr | Get remote device capability summary (cached from bond requestcap). Returns JSON with feature masks, service masks, sensor masks, and human-readable lists. |
| `/api/espnow/remotemanifest` | GET | xhr | List cached remote manifests or get specific manifest content. Query param: ?fwHash=<hash> to fetch a specific manifest. Returns JSON array of available manifests or single manifest content. |
| `/api/sensors/remote` | GET | xhr | Fetch remote sensor streaming state for all known peers. Returns JSON with per-device sensor types and streaming status (fresh/old). |
| `/api/files/read` | GET | fetch | Read file content from local filesystem. Query param: ?name=<filepath>. Used to fetch cached automations.json and other files. |

CLI commands (scraped via `/api/cli`):

- `espnowstatus` — Get ESP-NOW initialization status, MAC address, channel, and initialization state
- `espnowmode` — Get current ESP-NOW mode (Direct or Mesh)
- `bondstatus` — Get bonded peer MAC address (if any)
- `espnowlist` — Get list of paired ESP-NOW devices with encryption status
- `espnowmeshes listjson` — Get JSON list of configured meshes with labels, passphrases, enabled/default status
- `espnowdeviceinfo` — Get device metadata (friendly name, room, zone, tags, stationary flag)
- `espnowmeshrole` — Get current mesh role (worker/master/backup) and configuration
- `espnowmeshstatus` — Get JSON status of mesh peers with health, heartbeat, and activity data
- `espnowmeshstatus` — Get JSON mesh peer health information including alive status, heartbeat count, activity
- `espnowsend <mac> <message>` — Send a text message to a paired ESP-NOW device
- `espnowsendfile <mac> "<filepath>"` — Send a file from local filesystem to a paired device
- `espnowfetch <mac> <username> <password> "<remote_path>"` — Fetch a file from remote device filesystem (requires credentials)
- `espnowremote <mac> <username> <password> <command>` — Execute a command on remote device (e.g., sensors, memory, files)
- `espnowbroadcast <message>` — Broadcast a message to all paired devices
- `espnowunpair <mac>` — Unpair a device
- `espnowpair <mac> <name> [<mesh_label>]` — Pair a new device (unencrypted)
- `espnowpairsecure <mac> <name> [<mesh_label>]` — Pair a new device with encryption
- `espnowrequestmeta <mac>` — Request device metadata from peer via protocol-level METADATA_REQ
- `espnowmeshtopo` — Initiate mesh topology discovery
- `espnowtoporesults` — Get topology discovery results (device connections and paths)
- `espnowmeshrole <role>` — Set mesh role (worker, master, or backup)
- `espnowmeshmaster <mac>` — Set the master device MAC address
- `espnowmeshbackup <mac>` — Set the backup master MAC address
- `espnowbackupenable on\|off` — Enable or disable backup master functionality
- `openespnow` — Initialize and enable ESP-NOW
- `closeespnow` — Disable ESP-NOW (stops all communication)
- `espnowsetname [<device_name>]` — Set or get the device name for ESP-NOW
- `espnowfriendlyname "<name>"` — Set friendly name for home automation discovery
- `espnowroom "<room>"` — Set room name for device metadata
- `espnowzone "<zone>"` — Set zone name for device metadata
- `espnowtags "<tags>"` — Set comma-separated tags for device metadata
- `espnowstationary on\|off` — Mark device as stationary for mesh topology optimization
- `espnowsetpassphrase <mesh_label> "<passphrase>"\|clear` — Set or clear mesh encryption passphrase (takes ~1 minute to derive key)
- `espnowmeshes add <label>` — Add a new mesh slot with given label
- `espnowmeshes enable <label>` — Re-enable a disabled mesh
- `espnowmeshes disable <label>` — Disable a mesh (soft delete)
- `espnowmeshes rename <old_label> <new_label>` — Rename a mesh
- `espnowmeshes setdefault <label>` — Set a mesh as the default
- `espnowsensorstream <sensor> on\|off` — Enable or disable sensor streaming on remote device
- `ls` — List files in local filesystem

**Notes:** The /espnow page is a comprehensive ESP-NOW device management interface. Key observations: (1) Uses shared layer components (streamBeginHtml/streamEndHtml, getFileBrowserScript, hw.notify, hwConfirm dialogs). (2) Heavy reliance on /api/cli for all operations through structured CLI commands. (3) Batch endpoint used on initial load to reduce RTT for 8 status queries. (4) Message polling loop at 500ms cadence with chunked message reassembly (reqId-based grouping for multi-piece messages). (5) Persistent polling with auth failure detection redirects to /login. (6) Remote file browser uses text-scraping of 'files' command output with polling until completion marker. (7) Mesh management through event delegation pattern on single meshes-card listener. (8) Sensor streaming state loaded from /api/sensors/remote. (9) Device metadata cached client-side and synced via METADATA_REQ protocol. (10) Automations fetched via espnowfetch, then polled at /api/files/read until cached file appears.

### `/bond`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/bond/status` | GET | fetch | Fetch bonded device status including connection state, capabilities, sensor connectivity, heartbeat metrics, and link quality |
| `/api/bond/stream` | POST | fetch | Toggle streaming of sensor data from bonded device (thermal, ToF, IMU, GPS, input, FM radio, presence, RTC) |
| `/api/bond/exec` | POST | fetch | Execute CLI commands on the bonded peer device with remote: prefix routing |
| `/api/bond/role` | POST | fetch | Swap master/worker roles between bonded devices |
| `/api/bond/paired-devices` | GET | fetch | List ESP-NOW paired devices available for bonding |
| `/api/espnow/messages` | GET | fetch | Poll for messages from bonded peer (used for remote command output streaming with since and mac query parameters) |
| `/api/cli` | POST | fetch | Execute CLI commands: bondresync, bonddisconnect, bondconnect |

CLI commands (scraped via `/api/cli`):

- `bondresync --all` — Force re-fetch of bonded peer's capabilities, manifest, settings, and schema via /api/cli
- `bonddisconnect` — Unbond from the paired device via /api/cli
- `bondconnect <mac>` — Initiate bond connection to a paired device by MAC address via /api/cli
- `remote:<any_command>` — Execute arbitrary commands on bonded peer via /api/bond/exec (prefixed with 'remote:' for routing)
- `bondstream <sensor> <on\|off>` — Enable/disable streaming of specific sensor (thermal, tof, imu, input, gps, rtc, presence, fmradio) via /api/bond/stream toggle resolution
- `bondrole <master\|worker>` — Change device role in bond relationship via /api/bond/role

**Notes:** The /bond page relies on the shared layer (hw.postForm, hw.fetchJSON utilities) for API calls. The page auto-refreshes every 5 seconds via refreshBond(). Remote command execution uses a polling pattern on /api/espnow/messages to stream output from the bonded peer. The page inherits shared HTML/JS for navigation, theming, and common dialogs from the shared layer."

### `/battery`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/battery/status` | GET | fetch | Fetch live battery status snapshot (JSON). Called once on page load via hw.fetchJSON() and then polled every 2000ms via hw.pollJSON(). |
| `/api/files/view` | GET | fetch | Fetch raw battery.csv log file content (text). Called via hw.fetchText() when user clicks 'Refresh log' button or page initially loads. Query params: name=battery.csv&mode=raw |
| `/api/icon` | GET | img-src | Asset endpoint for loading themed file/folder icons used by shared file-browser components if present on page |

**Notes:** The battery page is simple and focused: it displays live battery status (percentage, voltage, charging state, rate of discharge for fuelgauge backend) polled at 2-second intervals, and a historical CSV log with a chart and table rendered from /api/files/view?name=battery.csv&mode=raw. The page does not make any CLI calls or use EventSource streams. The inheritsSharedLayer flag is set to true because the page uses streamBeginHtml() which includes the shared themed dialog system (hwAlert/hwConfirm/hwPrompt) and the common CSS/navigation, but those are mapped separately at the shared-layer level."

### `/bluetooth`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/cli` | POST | xhr | Execute CLI commands via a queued dispatch mechanism to handle rate limiting (80ms minimum gap between requests). The page's cli() helper serializes all commands through this endpoint, avoiding server 429 (too many requests) errors. |
| `/api/events` | GET | EventSource | Server-Sent Events stream for real-time updates to BLE status; the page uses adaptive polling instead, falling back if EventSource wedges. |

CLI commands (scraped via `/api/cli`):

- `blemode` — Query current Bluetooth mode (server vs G2 client); returns 'server' or 'client'
- `bleinfo json` — Get structured BLE server info (device name, TX power, connections, clients list, config state) as JSON
- `g2status` — Query G2 glasses (client mode) connection state; returns formatted text: state, left/right temple up/down, MTU, battery, tx/rx counts
- `ringstatus` — Query R1 ring BLE connection state; returns formatted text: ring state, name, MAC, MTU, rx count
- `openble` — Open/initialize the Bluetooth server (server mode)
- `closeble` — Close/deinitialize the Bluetooth server
- `g2init` — Initialize G2 client (BLE central) stack
- `g2deinit` — Deinitialize G2 client stack
- `bleadv` — Enable BLE server advertising
- `bledisconnect` — Disconnect all active BLE clients from server
- `blemode client\|server` — Switch BLE mode between server and G2 client
- `blestream on\|off\|sensors\|system\|events` — Configure which data streams to enable to connected BLE clients (sensors, system, events)
- `blestream interval <sensor-ms> <system-ms>` — Set streaming interval (in milliseconds) for sensor and system data
- `blename <name>` — Set Bluetooth device name (max 30 chars)
- `bletxpower <0-7>` — Set TX power level (0=-12dBm, 7=+9dBm)
- `bleautostart on\|off` — Enable/disable auto-start Bluetooth server on device boot
- `blerequireauth on\|off` — Require client authentication before accepting commands
- `g2scan` — Scan for Even Realities G2 glasses temples (BLE client mode)
- `openg2 auto` — Connect to previously-seen G2 temples (both left/right)
- `closeg2` — Disconnect from G2 temples (BLE client mode)
- `g2battery` — Query G2 temple battery levels; kicks async BLE notification, results arrive in g2status
- `g2show <text>` — Display text on back pane of G2 glasses
- `g2ai <text>` — Push text card to front (closer) focal plane on G2 display (Even-AI subsystem)
- `g2notify <seconds> <text>` — Show transient notification on G2 glasses that auto-clears after duration
- `g2bmp <path> <brightness> <contrast> <hold-secs>` — Load and display a BMP image from storage to G2 glasses
- `g2clear` — Clear G2 display
- `g2reopen` — Re-open hijacked Blocks app on G2 after abnormal exit
- `ringconnect` — Connect to R1 ring (spawns background task, returns immediately)
- `ringdisconnect` — Disconnect from R1 ring
- `bleautoreconnect g2-glasses on\|off` — Enable/disable auto-reconnect to G2 temples on device boot
- `bleautoreconnect r1-ring on\|off` — Enable/disable auto-reconnect to R1 ring on device boot
- `g2mic on\|off` — Enable/disable G2 microphone
- `g2nav on\|off` — Enable/disable menu navigation via G2 gestures (g2nav toggle)
- `g2verbose on\|off` — Enable/disable verbose BLE scan logging (logs every advert seen)

**Notes:** The page uses a sophisticated client-side CLI queue to serialize /api/cli requests with 80ms minimum spacing, avoiding the server's 50ms rate limiter. This is crucial because page-load alone fires loadMode() + loadConfig() + refresh() + g2status + ringstatus + g2nav + g2verbose in quick succession, which would cause 429 errors without queuing. The page also implements adaptive polling with a two-speed cadence (1500ms FAST when temples disconnected, 15000ms SLOW when both up for 3 stable ticks) instead of relying on EventSource due to EventSource wedging across WiFi-reconnect cycles. All status rendering is driven by CLI responses — no other data endpoints are called."

### `/speech`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/cli` | POST | exec-wrapper | Execute CLI commands for speech recognition control and model management |
| `/api/upload` | POST | fetch | Upload model files (wake word models, command definitions, binaries) |

CLI commands (scraped via `/api/cli`):

- `srstatus` — Query speech recognition engine status (running state, wake/command counts, audio levels, model info, raw output mode, auto-tune state)
- `srstart` — Start the speech recognition engine
- `srstop` — Stop the speech recognition engine
- `ls /sd/ESP-SR Models` — List model files in the ESP-SR Models directory
- `sr loadwake /sd/ESP-SR Models/<filename>` — Load a wake word model (.wn file)
- `sr loadcmds /sd/ESP-SR Models/<filename>` — Load command definitions (commands.txt)
- `srtuning` — Display current audio tuning status and gain settings
- `srraw on\|off` — Enable/disable raw output mode (shows all MultiNet hypotheses)
- `srautotune start\|stop` — Start/stop automatic tuning to find optimal gain presets
- `srtuninggain <gain_multiplier>` — Set AFE (audio front-end) gain multiplier (1.0x, 2.0x, 3.0x, 4.0x, 5.0x)
- `srdyngain off\|on <max_multiplier>` — Control dynamic gain (off, or on with max multiplier: 1.5x, 2.0x, 2.5x, 3.0x)

**Notes:** The page uses the shared hw.postFormText() helper which POSTs to /api/cli with form body {cmd: '...'} (Content-Type: application/x-www-form-urlencoded). Model file upload uses native fetch() with FormData. The page inherits shared layer components (dialogs, nav, theme) but makes no calls to those shared endpoints — only direct /api/cli and /api/upload calls. Page loads initial sr status and tuning info on mount and polls srstatus every 2 seconds when SR is running."

### `/mqtt`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/cli` | POST | fetch | Execute CLI commands for MQTT client control |
| `/api/mqtt/status` | GET | fetch | Get current MQTT connection status (connected: true/false) |

CLI commands (scraped via `/api/cli`):

- `mqttclientenabled` — Check if MQTT client service is enabled
- `mqttclientenabled 1` — Enable MQTT client service
- `mqttclientenabled 0` — Disable MQTT client service
- `openmqtt` — Connect to MQTT broker
- `closemqtt` — Disconnect from MQTT broker

**Notes:** The MQTT page relies on the shared layer for theme management (hw.loadThemePref, hw.saveThemePref), dialogs (hwAlert, hwConfirm), and notifications (hw.notify via EventSource). The page auto-refreshes the connection status every 5 seconds via setInterval(mqttRefresh, 5000). All CLI commands are POSTed to /api/cli using hw.postForm(). The page displays server-side rendered MQTT configuration and external sensor data; only the dynamic status indicators and enable/disable buttons trigger backend calls."

### `/games`

| Endpoint | Method | Via | Purpose |
|----------|--------|-----|---------|
| `/api/system` | GET | fetch | Retrieve system configuration including i2c_enabled status |
| `/api/sensors/status` | GET | fetch | Check sensor compilation and enable status (imuCompiled, imuEnabled, inputCompiled, inputEnabled) |
| `/api/sensors` | GET | fetch | Poll IMU and gamepad sensor data via query params (sensor=imu or sensor=gamepad, with timestamp) |
| `/api/cli` | POST | xhr | Execute CLI commands (sensor control, firmware debug toggles) |
| `/api/cli/logs` | GET | fetch | Retrieve firmware debug logs when FW_DEBUG mode is enabled |

CLI commands (scraped via `/api/cli`):

- `imustart` — Enable IMU sensor and begin reading orientation data for game control calibration
- `imustop` — Disable IMU sensor polling
- `debugsensorsgeneral 1` — Enable general sensor debug output in firmware logs
- `debugsensorsgeneral 0` — Disable general sensor debug output
- `debugimudata 1` — Enable IMU-specific debug logging in firmware
- `debugimudata 0` — Disable IMU-specific debug logging

**Notes:** The /games page is a client-side 3D maze game that uses IMU (inertial measurement unit) sensor data for tilt-based control. It polls sensors only after the user clicks Start. The page fetches /api/system and /api/sensors/status to check i2c availability and sensor compilation status on page load. During gameplay, it polls /api/sensors with query parameters (sensor=imu or sensor=gamepad) at intervals (50ms for calibration, 16ms for gamepad). Debug mode can be toggled via firmware debug CLI commands. The page inherits the shared layer (common navigation, dialogs, theme) which provides the hw object with fetch/post helpers that handle auth and serialization."

### `/darkroom`

_No direct backend calls (shared layer only)._

**Notes:** The /darkroom page is a completely self-contained, full-screen game implementation with no backend API calls whatsoever. It uses only localStorage for persistence and contains embedded jQuery for DOM manipulation. The game code is minified and includes multi-language support. All remote features (audio, Dropbox sync, analytics) have been explicitly removed.

## API Usage Index

Every registered endpoint, its class, and which pages call it.

| Endpoint | Class | #Pages | Used by |
|----------|-------|--------|---------|
| `/api/bond/cli/batch` | shared-layer | 2 | (shared layer), /settings |
| `/api/bond/exec` | shared-layer | 5 | (shared layer), /settings, /files, /cli, /bond |
| `/api/bond/fs/get` | shared-layer | 2 | (shared layer), /files |
| `/api/bond/fs/list` | shared-layer | 3 | (shared layer), /files, /cli |
| `/api/bond/fs/stat` | shared-layer | 3 | (shared layer), /files, /cli |
| `/api/bond/status` | shared-layer | 6 | (shared layer), /settings, /files, /cli, /logging, /bond |
| `/api/cli` | shared-layer | 16 | (shared layer), /dashboard, /settings, /files, /cli, /logging, /automations, /sensors, /llm, /maps, /espnow, /bond, /bluetooth, /speech, /mqtt, /games |
| `/api/cli/batch` | shared-layer | 5 | (shared layer), /settings, /files, /logging, /espnow |
| `/api/espnow/messages` | shared-layer | 5 | (shared layer), /files, /cli, /espnow, /bond |
| `/api/events` | shared-layer | 6 | (shared layer), /dashboard, /logging, /account/password-change, /sensors, /bluetooth |
| `/api/files/list` | shared-layer | 5 | (shared layer), /settings, /files, /cli, /logging |
| `/api/files/read` | shared-layer | 4 | (shared layer), /files, /cli, /espnow |
| `/api/files/stats` | shared-layer | 4 | (shared layer), /files, /cli, /logging |
| `/api/files/upload` | shared-layer | 5 | (shared layer), /settings, /files, /cli, /sensors |
| `/api/files/view` | shared-layer | 6 | (shared layer), /files, /cli, /logging, /maps, /battery |
| `/api/icon` | shared-layer | 6 | (shared layer), /files, /cli, /logging, /icons/test, /battery |
| `/api/user/settings` | shared-layer | 4 | (shared layer), /dashboard, /cli, /account/password-change |
| `/api/cli/logs` | multi-page | 2 | /cli, /games |
| `/api/devices` | multi-page | 2 | /dashboard, /sensors |
| `/api/sensors` | multi-page | 2 | /sensors, /games |
| `/api/sensors/remote` | multi-page | 2 | /sensors, /espnow |
| `/api/sensors/status` | multi-page | 4 | /dashboard, /sensors, /maps, /games |
| `/api/settings` | multi-page | 3 | /settings, /logging, /sensors |
| `/api/system` | multi-page | 3 | /dashboard, /maps, /games |
| `/api/videos/file` | multi-page | 2 | /files, /sensors |
| `/api/automations` | single-page | 1 | /automations |
| `/api/automations/export` | single-page | 1 | /automations |
| `/api/battery/status` | single-page | 1 | /battery |
| `/api/bond/paired-devices` | single-page | 1 | /bond |
| `/api/bond/role` | single-page | 1 | /bond |
| `/api/bond/settings` | single-page | 1 | /settings |
| `/api/bond/settings/schema` | single-page | 1 | /settings |
| `/api/bond/settings/schema/sync` | single-page | 1 | /settings |
| `/api/bond/settings/sync` | single-page | 1 | /settings |
| `/api/bond/stream` | single-page | 1 | /bond |
| `/api/buildconfig` | single-page | 1 | /settings |
| `/api/edgeimpulse/detect` | single-page | 1 | /sensors |
| `/api/ei/organize` | single-page | 1 | /sensors |
| `/api/espnow/metadata` | single-page | 1 | /espnow |
| `/api/espnow/remotecap` | single-page | 1 | /espnow |
| `/api/espnow/remotemanifest` | single-page | 1 | /espnow |
| `/api/files/write` | single-page | 1 | /files |
| `/api/gps/tracks` | single-page | 1 | /maps |
| `/api/llm/generate` | single-page | 1 | /llm |
| `/api/llm/load` | single-page | 1 | /llm |
| `/api/llm/models` | single-page | 1 | /llm |
| `/api/llm/result` | single-page | 1 | /llm |
| `/api/llm/status` | single-page | 1 | /llm |
| `/api/llm/stop` | single-page | 1 | /llm |
| `/api/llm/unload` | single-page | 1 | /llm |
| `/api/maps/features` | single-page | 1 | /maps |
| `/api/mqtt/status` | single-page | 1 | /mqtt |
| `/api/recordings` | single-page | 1 | /sensors |
| `/api/recordings/file` | single-page | 1 | /sensors |
| `/api/sensors/camera/frame` | single-page | 1 | /sensors |
| `/api/sensors/camera/stream` | single-page | 1 | /sensors |
| `/api/sessions` | single-page | 1 | /dashboard |
| `/api/settings/schema` | single-page | 1 | /settings |
| `/api/videos` | single-page | 1 | /sensors |
| `/api/waypoints` | single-page | 1 | /maps |
| `/api/admin/approve` | unused | 0 | — |
| `/api/admin/pending` | unused | 0 | — |
| `/api/admin/reject` | unused | 0 | — |
| `/api/admin/sessions` | unused | 0 | — |
| `/api/backup` | unused | 0 | — |
| `/api/files/create` | unused | 0 | — |
| `/api/llm/chat/clear` | unused | 0 | — |
| `/api/llm/chat/retry` | unused | 0 | — |
| `/api/llm/chat/turns` | unused | 0 | — |
| `/api/notice` | unused | 0 | — |
| `/api/output` | unused | 0 | — |
| `/api/output/temp` | unused | 0 | — |
| `/api/ping` | unused | 0 | — |
| `/api/restore` | unused | 0 | — |
| `/api/sensors/camera/status` | unused | 0 | — |
| `/apple-touch-icon-precomposed.png` | unused | 0 | — |
| `/apple-touch-icon.png` | unused | 0 | — |
| `/favicon.ico` | unused | 0 | — |

**Called but not registered (broken/mismatched paths):**

- `/api/upload` ← /speech

---

## Consolidation Findings

The web UI is heavily CLI-centric: /api/cli is the single most-shared endpoint (15+ pages) and most "data" pages scrape it rather than calling typed REST endpoints, which is why several REST endpoints (admin/*, notice, output) have zero page consumers — the same actions are done via CLI commands like pendinglist json, userapprove, sessionrevoke. The biggest structural duplication is the bond settings cluster: /api/bond/settings{,/sync,/schema,/schema/sync} is a 4-endpoint mirror of /api/settings + /api/settings/schema tunneled over bond, all consumed only by /settings. A second duplication axis is dual file-content endpoints (/api/files/view vs /api/files/read) and dual session endpoints (/api/sessions vs /api/admin/sessions). The dashboard and settings pages are chatty on boot (4-6 separate fetches each) and are the best batching targets. A handful of registered endpoints are genuinely non-page consumers (migration tool: ping/backup/restore; BLE/external clients: llm/chat/*; img-src: camera/frame) and should NOT be treated as dead, but /api/notice, /api/output, /api/output/temp, /api/sensors/camera/status, and /api/files/create appear to be truly orphaned, plus /speech POSTs to an unregistered /api/upload (a broken path).

### Orphaned / dead-endpoint candidates

Endpoints no page calls. The verdict column distinguishes genuinely removable from "has a non-page consumer, keep."

| Endpoint | Verdict | Note |
|----------|---------|------|
| `/api/ping` | keep (non-page consumer) | NOT dead. Registered by the external migration tool (WebServer_MigrationTool.cpp:612/760) as the OPTIONS/POST CORS-preflight + connection-test endpoint for /api/backup and /api/restore. External-only consumer; keep with the migration tool family. |
| `/api/backup` | keep (non-page consumer) | NOT dead. External backup/restore migration tool surface (WebServer_MigrationTool), not called by any page. Keep as the external migration consumer pair with /api/restore and /api/ping. |
| `/api/restore` | keep (non-page consumer) | NOT dead. External restore endpoint, registered on a minimal restore-only server alongside /api/ping (WebServer_MigrationTool.cpp). External-only consumer. |
| `/api/llm/chat/turns` | keep (non-page consumer) | NOT a page consumer but intentional: GET conversation-as-JSON for BLE/external clients. WebPage_LLM.cpp:306/421 only defines the handler; comments state clients (BLE app) call it. The web /llm page does not fetch it. Non-page consumer — keep, but it is web-untested. |
| `/api/llm/chat/retry` | keep (non-page consumer) | NOT a page consumer; same as chat/turns — POST regenerate-last-turn mirror for BLE/external clients (WebPage_LLM.cpp:348/422, comment at :232 says clients should call it). Web page owns retry differently. |
| `/api/llm/chat/clear` | keep (non-page consumer) | NOT a page consumer; POST wipe-history mirror for BLE/external clients (WebPage_LLM.cpp:381/423; System_LLM.cpp:2212 mirrors it). Non-page consumer. |
| `/api/admin/sessions` | remove candidate | Likely redundant rather than dead. No page fetches it; /settings does session admin via CLI (sessionlist json, sessionrevoke user). handleAdminSessionsList (WebServer_Server.cpp:2766) is an admin-scoped REST alternative to that CLI path. Candidate to remove or to back the CLI with. |
| `/api/admin/pending` | remove candidate | Redundant with CLI. No page fetch; /settings uses 'pendinglist json' via /api/cli for the pending-user list. handleAdminPending duplicates that. Remove or consolidate. |
| `/api/admin/approve` | remove candidate | Redundant with CLI. No page fetch; /settings approves via 'userapprove <username>' over /api/cli. POST handler duplicates the CLI action. |
| `/api/admin/reject` | remove candidate | Redundant with CLI. No page fetch; /settings denies via 'userdeny <username>' over /api/cli. POST handler duplicates the CLI action. |
| `/api/notice` | remove candidate | Appears orphaned. handleNotice (WebServer_Server.cpp:2871) dequeues one per-session SSE notice (e.g. [revoke]) — but no page polls it; the dashboard uses EventSource('/api/events') instead, which likely superseded it. Truly dead unless an external client polls it. |
| `/api/output` | remove candidate | Appears truly dead. GET serial/web/display/g2 output-flags (WebServer_Server.cpp:2787). No fetch/img-src/href anywhere in pages or sensor web fragments. |
| `/api/output/temp` | remove candidate | Appears truly dead. POST temporary output-flag override (WebServer_Server.cpp:2812). No consumer found in any page; equivalent control is available via CLI/settings. |
| `/api/sensors/camera/status` | remove candidate | Appears dead. Registered in WebPage_Sensors.cpp:1013 but only referenced there; no fetch from page JS (unlike camera/frame which is used as img.src). Camera state comes via /api/sensors/status. Candidate to remove. |
| `/api/files/create` | remove candidate | Appears dead as a REST path. No page fetch; /files creates files via the 'filecreate'/'mkdir' CLI commands (shared layer) over /api/cli. handleFilesCreate duplicates that CLI action. Remove or fold into the CLI path. |
| `/favicon.ico` | keep (non-page consumer) | NOT dead. Browser-implicit request served by handleBrowserIcon; never an explicit page fetch. Keep. |
| `/apple-touch-icon.png` | keep (non-page consumer) | NOT dead. Browser/OS-implicit icon request (handleBrowserIcon). Keep. |
| `/apple-touch-icon-precomposed.png` | keep (non-page consumer) | NOT dead. Browser/OS-implicit icon request (handleBrowserIcon). Keep. |
| `/api/upload` | broken path | Not in the registered list but the /speech page POSTs to it (WebPage_Speech.h:592). No handler is registered for /api/upload anywhere — this is a broken/dead client path. Should be /api/files/upload (the registered multipart upload handler). |

### Overlapping endpoints (merge candidates)

**[high effort]** `/api/settings`, `/api/settings/schema`, `/api/bond/settings`, `/api/bond/settings/sync`, `/api/bond/settings/schema`, `/api/bond/settings/schema/sync`

- *Why:* The four /api/bond/settings* endpoints are a transport-specific mirror of the two local /api/settings* endpoints (get/set values + get schema), only consumed by /settings. They differ from the local pair solely by routing the same request over the bond link to a paired device. This is 6 endpoints implementing 2 logical operations (settings CRUD, schema fetch) across 2 transports.
- *Proposal:* Collapse to /api/settings and /api/settings/schema with an optional ?target=local\|bond (or mac) parameter; route to bond internally. Drop the four bond-specific routes. /settings keeps one code path for local vs remote settings.

**[high effort]** `/api/cli`, `/api/cli/batch`, `/api/bond/cli/batch`, `/api/bond/exec`

- *Why:* Four ways to execute commands: single CLI, batched CLI, batched CLI over bond, and bond exec. /api/cli/batch is just N-of /api/cli; the two bond variants are the same operations over the bond transport. Settings page alone touches /api/cli, /api/cli/batch, /api/bond/cli/batch and /api/bond/exec.
- *Proposal:* Make /api/cli accept an optional array body (single or batch) to retire /api/cli/batch, and add ?target=bond&mac=.. to /api/cli to retire /api/bond/cli/batch and /api/bond/exec. One command-exec surface, transport as a parameter.

**[high effort]** `/api/files/list`, `/api/bond/fs/list`, `/api/files/view`, `/api/bond/fs/get`, `/api/bond/fs/stat`, `/api/files/stats`

- *Why:* /api/bond/fs/* is a remote-device mirror of the local /api/files/* filesystem operations (list/stat/get vs list/stats/view), used by /files and /cli for browsing a paired device. Same operations, different transport. Less of a pure duplicate than bond settings because remote FS semantics differ, but still parallel surfaces.
- *Proposal:* Lower priority than bond settings: unify under /api/files/* with a target/mac param if/when the bond transport is cleaned up; until then document them as the explicit remote-FS family so they aren't mistaken for dead local endpoints.

**[medium effort]** `/api/files/view`, `/api/files/read`

- *Why:* Both serve file content. /api/files/view is the broad shared-layer content endpoint (files/cli/logging/maps/battery); /api/files/read is a narrower variant used by /files, /cli, /espnow. Two endpoints for 'return file bytes/text' is redundant.
- *Proposal:* Merge into a single /api/files/view with a mode/format query param (raw vs inline-render vs download) covering both call sites; retire /api/files/read.

**[medium effort]** `/api/sensors`, `/api/sensors/status`, `/api/sensors/remote`

- *Why:* Three sensor endpoints with overlapping scope: /api/sensors (data/config), /api/sensors/status (lightweight state — polled by dashboard/maps/games), /api/sensors/remote (remote-device sensors). /dashboard and /maps and /games only need /status; /sensors uses all three. The split between /api/sensors and /api/sensors/status is the questionable one.
- *Proposal:* Keep /api/sensors/status as the cheap poll endpoint (many consumers) but consider folding /api/sensors detail responses to include the status fields so the heavy /sensors page can drop one of the two; treat /sensors/remote as the explicit remote family.

**[low effort]** `/api/sessions`, `/api/admin/sessions`

- *Why:* /api/sessions returns the caller's own sessions (used by /dashboard); /api/admin/sessions returns all sessions (no page consumer — /settings uses the 'sessionlist json' CLI instead). Same data shape, differing only by scope.
- *Proposal:* Keep one /api/sessions that returns own-vs-all based on the caller's admin role; delete /api/admin/sessions. Optionally back the /settings 'sessionlist json' CLI with the same handler.

**[low effort]** `/api/admin/pending`, `/api/admin/approve`, `/api/admin/reject`, `/api/admin/sessions`

- *Why:* All four admin REST endpoints are unused by pages because /settings performs the identical user/session admin via CLI (pendinglist json, userapprove, userdeny, sessionrevoke). Two parallel implementations of the same admin actions.
- *Proposal:* Pick one surface. Since the web UI already standardized on CLI for these, delete the four /api/admin/* REST handlers (or make them thin wrappers over the same shared core the CLI calls). Removes ~4 handlers.

### Chatty pages (batching candidates)

| Page | Direct calls | Idea |
|------|--------------|------|
| `/sensors` | 9 | Touches /api/sensors, /api/sensors/status, /api/sensors/remote, /api/recordings(+file), /api/videos(+file), camera frame/stream/status, EI endpoints. Merge recordings+videos into one media endpoint, drop camera/status (use /api/sensors/status), and consider a single /api/sensors bootstrap that includes status so the page doesn't fetch /api/sensors and /api/sensors/status separately. |
| `/settings` | 8 | On boot/refresh it pulls /api/buildconfig, /api/settings, /api/settings/schema, and the bond mirror set (/api/bond/status, /api/bond/settings, /api/bond/settings/schema, /api/bond/settings/sync, /api/bond/settings/schema/sync). Provide one /api/settings/bootstrap that returns buildconfig + values + schema in a single response, and gate the bond fetches behind a single /api/bond/status check so they only fire when a device is actually bonded. |
| `/llm` | 7 | Distinct lifecycle endpoints (status/models/load/unload/generate/stop/result) are mostly fine as separate actions, but the load flow polls /api/llm/status and the generate flow polls /api/llm/result on intervals. Consider delivering generation progress over the /api/events SSE stream to cut the result-polling loop. |
| `/dashboard` | 5 | DOMContentLoaded fires fetchDeviceRegistry (/api/devices), fetchSensorStatus (/api/sensors/status), fetchSystemStatus (/api/system), fetchSignedInUsers (/api/sessions), plus /api/user/settings, then opens SSE and re-polls /api/sessions every 15s. Combine the four boot reads into a single /api/dashboard or /api/system superset response, and push sessions + sensor status updates over the already-open /api/events SSE instead of the 15s interval poll. |
| `/maps` | 4 | Loads /api/maps/features, /api/waypoints, /api/gps/tracks (plus /api/system, /api/files/view). The three map-data fetches are all single-page and load together — batch them into one /api/maps/bootstrap response. |

### Prioritized consolidation candidates

#### [high] Collapse the bond settings 4-endpoint mirror into /api/settings

- *Endpoints:* `/api/bond/settings`, `/api/bond/settings/sync`, `/api/bond/settings/schema`, `/api/bond/settings/schema/sync`, `/api/settings`, `/api/settings/schema`
- *Rationale:* Six registered endpoints implement two logical operations (settings get/set, schema fetch) across local and bond transports, and all the bond variants are consumed only by /settings. This is the single largest structural duplication in the surface and the chattiest page's biggest contributor.
- *Proposal:* Add a target=local\|bond (and mac) parameter to /api/settings and /api/settings/schema; route to the bond link internally. Delete the four /api/bond/settings* routes. /settings then has one settings code path for local vs remote.

#### [high] Delete redundant /api/admin/* REST endpoints in favor of the CLI path

- *Endpoints:* `/api/admin/sessions`, `/api/admin/pending`, `/api/admin/approve`, `/api/admin/reject`
- *Rationale:* None are called by any page; /settings already does the identical user/session admin via /api/cli (pendinglist json, userapprove, userdeny, sessionlist json, sessionrevoke). Two parallel implementations of the same admin actions, with the REST half dead.
- *Proposal:* Remove the four handlers (or reduce them to thin wrappers over the same shared admin core the CLI uses). Confirm no external/non-page client depends on them first; they are admin-scoped so an external admin tool is the only risk.

#### [high] Fix the /speech upload path (broken) and unify upload

- *Endpoints:* `/api/upload`, `/api/files/upload`
- *Rationale:* /speech POSTs to /api/upload (WebPage_Speech.h:592) but no handler is registered for that path — the speech upload is silently broken. /api/files/upload is the real multipart upload handler used by settings/files/cli/sensors.
- *Proposal:* Point the /speech upload at /api/files/upload (with an appropriate target dir param) and remove the dangling /api/upload reference. One upload endpoint for the whole UI.

#### [medium] Retire genuinely orphaned endpoints

- *Endpoints:* `/api/notice`, `/api/output`, `/api/output/temp`, `/api/sensors/camera/status`, `/api/files/create`
- *Rationale:* No page consumer and no obvious external consumer: /api/notice is superseded by the /api/events SSE; /api/output[/temp] output-flag control has no caller; /api/sensors/camera/status is shadowed by /api/sensors/status; /api/files/create duplicates the 'filecreate'/'mkdir' CLI actions /files actually uses.
- *Proposal:* Remove these handlers to shrink the surface and free flash/RAM (every httpd_uri_t + handler costs code space on the ESP32). Grep for external clients first, but pages don't reference them.

#### [medium] Merge dual file-content and media endpoints

- *Endpoints:* `/api/files/view`, `/api/files/read`, `/api/recordings`, `/api/videos`, `/api/recordings/file`, `/api/videos/file`
- *Rationale:* /api/files/view and /api/files/read both return file content (just different default modes), and the audio (/api/recordings*) vs video (/api/videos*) endpoints are structurally identical media-list/media-file pairs that only /sensors (and /files for videos/file) use.
- *Proposal:* Collapse files/view+files/read into one /api/files/view with a mode/format param; collapse recordings+videos into one /api/media (and /api/media/file) with a type=audio\|video param. Net: 6 endpoints -> 3.

#### [low] Unify command execution and batch surfaces

- *Endpoints:* `/api/cli`, `/api/cli/batch`, `/api/bond/cli/batch`, `/api/bond/exec`
- *Rationale:* /api/cli/batch is just N-of /api/cli, and the two bond variants are the same execution over the bond transport. /api/cli is already the dominant shared endpoint; the extra three add surface without new semantics.
- *Proposal:* Let /api/cli accept either a single command or an array (retires /api/cli/batch), and add target=bond&mac=.. (retires /api/bond/cli/batch and /api/bond/exec). One command-exec endpoint, transport and batching as parameters. Higher effort due to many call sites.

#### [low] Fold single-page status endpoints into /api/system or SSE

- *Endpoints:* `/api/battery/status`, `/api/mqtt/status`, `/api/sessions`
- *Rationale:* Each is a small single-page status read that overlaps the multi-page /api/system status or could ride the already-open /api/events SSE; battery/mqtt are natural members of system status, and /api/sessions is dashboard-only and re-polled on a timer.
- *Proposal:* Add battery + mqtt fields to /api/system and push session changes over /api/events; remove the dedicated /api/battery/status and /api/mqtt/status polls and the dashboard's 15s /api/sessions interval. Lower priority, do alongside the dashboard/settings bootstrap batching.

