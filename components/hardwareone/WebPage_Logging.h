#ifndef WEBPAGE_LOGGING_H
#define WEBPAGE_LOGGING_H

#include "WebServer_Utils.h"  // For getFileBrowserScript
#include "System_BuildConfig.h"

// Streamed inner content for logging page
inline void streamLoggingInner(httpd_req_t* req) {
  // Load file browser script for log viewer
  httpd_resp_send_chunk(req, getFileBrowserScript(), HTTPD_RESP_USE_STRLEN);
  
  // HTML structure
  httpd_resp_send_chunk(req, R"HTML(
<h2>Data Logging</h2>
<p>Configure and manage automated logging to files</p>

<!-- Sensor Logging Section -->
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div>
      <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Sensor Data Logging</div>
      <div style='color:var(--panel-fg);font-size:0.9rem'>Configure sensor data collection, file rotation, and output formats.</div>
    </div>
    <button class='btn' id='btn-sensor-section-toggle' onclick="togglePane('content-sensors','btn-sensor-section-toggle')">Expand</button>
  </div>
  <div id='content-sensors' style='display:none;margin-top:0.75rem'>

<!-- Status Section -->
<div class='settings-panel' style='background:var(--panel-bg)'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div>
      <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Status</div>
      <div id='status-indicator' style='display:flex;align-items:center;gap:0.5rem;margin-top:0.5rem'>
        <span id='status-dot' style='width:12px;height:12px;border-radius:50%;background:#6c757d'></span>
        <span id='status-text' style='font-weight:500;color:var(--panel-fg)'>Loading...</span>
      </div>
    </div>
    <button class='btn' id='btn-status-toggle' onclick="togglePane('status-pane','btn-status-toggle')">Expand</button>
  </div>
  <div id='status-pane' style='display:none;margin-top:0.75rem'>
    <div style='background:var(--panel-bg);padding:1rem;border-radius:6px;border:1px solid var(--border)'>
      <div style='display:grid;grid-template-columns:140px 1fr;gap:0.5rem;font-size:0.95rem'>
        <div style='font-weight:500'>File:</div>
        <div id='detail-file' style='font-family:monospace;color:var(--panel-fg)'>—</div>
        <div style='font-weight:500'>Format:</div>
        <div id='detail-format'>—</div>
        <div style='font-weight:500'>Interval:</div>
        <div id='detail-interval'>—</div>
        <div style='font-weight:500'>Max Size:</div>
        <div id='detail-maxsize'>—</div>
        <div style='font-weight:500'>Rotations:</div>
        <div id='detail-rotations'>—</div>
        <div style='font-weight:500'>Sensors:</div>
        <div id='detail-sensors' style='color:var(--panel-fg)'>—</div>
        <div style='font-weight:500'>Last Write:</div>
        <div id='detail-lastwrite'>—</div>
      </div>
    </div>
  </div>
</div>

<!-- Quick Actions Section -->
<div class='settings-panel' style='background:var(--panel-bg)'>
  <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg);margin-bottom:0.75rem'>Quick Actions</div>
  <div style='display:flex;gap:1rem;flex-wrap:wrap'>
    <button id='btn-start' class='btn' onclick='startLogging()' style='display:none' data-guest-hide>Start Logging</button>
    <button id='btn-stop' class='btn' onclick='stopLogging()' style='display:none' data-guest-hide>Stop Logging</button>
    <button id='btn-autostart' class='btn' onclick='toggleAutoStart()' data-guest-hide>Auto-Start: <span id='autostart-status'>Loading...</span></button>
    <button class='btn' onclick='refreshStatus()'>Refresh Status</button>
  </div>
</div>

<!-- Configuration Section -->
<div class='settings-panel' style='background:var(--panel-bg)'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div>
      <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Configuration</div>
      <div style='color:var(--panel-fg);font-size:0.9rem'>Configure logging parameters, file management, and sensor selection.</div>
    </div>
    <button class='btn' id='btn-config-toggle' onclick="togglePane('config-pane','btn-config-toggle')">Expand</button>
  </div>
  <div id='config-pane' style='display:none;margin-top:0.75rem'>
    
    <!-- Logging Parameters -->
    <div style='font-weight:bold;color:var(--panel-fg);margin-bottom:0.75rem'>Logging Parameters</div>
        <label style='display:block;margin-bottom:1rem'>
          <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>File Path:</div>
          <input id='config-path' type='text' placeholder='Generating timestamp...' class='form-input input-fit input-l' style='font-family:monospace' data-guest-hide>
          <small style='color:var(--panel-fg)'>Auto-generated with timestamp (NTP or uptime)</small>
        </label>
        <label style='display:block;margin-bottom:1rem'>
          <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>Interval (ms):</div>
          <input id='config-interval' type='number' value='5000' min='100' max='3600000' class='form-input input-fit' data-guest-hide>
          <small style='color:var(--panel-fg)'>Min: 100ms, Max: 1 hour (3600000ms)</small>
        </label>
        <label style='display:block;margin-bottom:1rem'>
          <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>Format:</div>
          <select id='config-format' class='form-input input-fit input-m' data-guest-hide>
            <option value='text'>Text (Human-readable)</option>
            <option value='csv'>CSV (Structured data)</option>
            <option value='track'>Track (GPS-only compact with signal loss dedup)</option>
          </select>
        </label>
        <label style='display:block;margin-bottom:1rem'>
          <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>Max File Size (bytes):</div>
          <input id='config-maxsize' type='number' value='256000' min='10240' max='10485760' class='form-input input-fit input-m' data-guest-hide>
          <small style='color:var(--panel-fg)'>Min: 10KB, Max: 10MB</small>
        </label>
        <label style='display:block'>
          <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>Rotations (old logs to keep):</div>
          <input id='config-rotations' type='number' value='3' min='0' max='9' class='form-input input-fit' data-guest-hide>
          <small style='color:var(--panel-fg)'>0 = delete old logs, 1-9 = keep N old files</small>
        </label>
    
    <!-- Sensors to Log -->
    <div style='font-weight:bold;color:var(--panel-fg);margin:1rem 0 0.75rem'>Sensors to Log</div>
    <div id='sensors-pane'>
)HTML", HTTPD_RESP_USE_STRLEN);
#if ENABLE_THERMAL_SENSOR
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-thermal' value='thermal' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px' data-guest-hide>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Thermal (temperature array)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_TOF_SENSOR
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-tof' value='tof' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px' data-guest-hide>
            <span style='font-size:0.9em;color:var(--panel-fg)'>ToF (distance sensors)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_IMU_SENSOR
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-imu' value='imu' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px' data-guest-hide>
            <span style='font-size:0.9em;color:var(--panel-fg)'>IMU (orientation, accel, gyro, temp)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_GAMEPAD_SENSOR
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-gamepad' value='gamepad' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px' data-guest-hide>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Gamepad (buttons, joystick)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_APDS_SENSOR
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-apds' value='apds' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px' data-guest-hide>
            <span style='font-size:0.9em;color:var(--panel-fg)'>APDS (color, proximity, gesture)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_GPS_SENSOR
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-gps' value='gps' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px' data-guest-hide>
            <span style='font-size:0.9em;color:var(--panel-fg)'>GPS (position, speed, altitude)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_PRESENCE_SENSOR
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-presence' value='presence' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px' data-guest-hide>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Presence (IR presence/motion)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_R1_HEALTH
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-r1' value='r1' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px' data-guest-hide>
            <span style='font-size:0.9em;color:var(--panel-fg)'>R1 Health (HR, HRV, SpO2, battery)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
#if !ENABLE_THERMAL_SENSOR && !ENABLE_TOF_SENSOR && !ENABLE_IMU_SENSOR && !ENABLE_GAMEPAD_SENSOR && !ENABLE_APDS_SENSOR && !ENABLE_GPS_SENSOR && !ENABLE_PRESENCE_SENSOR && !ENABLE_R1_HEALTH
  httpd_resp_send_chunk(req, R"HTML(
        <div style='color:var(--panel-fg);opacity:0.6;font-size:0.9em;font-style:italic;margin:0.5rem 0 0.75rem'>
          No loggable sensors are compiled into this firmware. To enable sensors, adjust
          <code>I2C_FEATURE_LEVEL</code> (or individual <code>ENABLE_*_SENSOR</code> flags)
          in <code>System_BuildConfig.h</code> and rebuild.
        </div>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
  httpd_resp_send_chunk(req, R"HTML(
        <div style='margin-top:0.5rem;display:flex;gap:0.5rem'>
          <button class='btn' onclick='selectAllSensors()' style='padding:0.25rem 0.75rem;font-size:0.85rem' data-guest-hide>Select All</button>
          <button class='btn' onclick='selectNoSensors()' style='padding:0.25rem 0.75rem;font-size:0.85rem' data-guest-hide>Select None</button>
        </div>
      </div>
    
    <div style='margin-top:1rem;display:flex;gap:0.5rem'>
      <button class='btn' onclick='applyConfig()' data-guest-hide>Apply Configuration</button>
    </div>
    <div id='config-status' style='margin-top:1rem;color:var(--danger)'></div>
  </div>
</div>


  </div>
</div>

<!-- System Logging Section -->
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div>
      <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>System Logging</div>
      <div style='color:var(--panel-fg);font-size:0.9rem'>Debug logging with category filtering and configurable output destinations.</div>
    </div>
    <button class='btn' id='btn-system-section-toggle' onclick="togglePane('content-system','btn-system-section-toggle')">Expand</button>
  </div>
  <div id='content-system' style='display:none;margin-top:0.75rem'>

<!-- System Status Section -->
<div class='settings-panel' style='background:var(--panel-bg)'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div>
      <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Status</div>
      <div id='sys-status-indicator' style='display:flex;align-items:center;gap:0.5rem;margin-top:0.5rem'>
        <span id='sys-status-dot' style='width:12px;height:12px;border-radius:50%;background:#6c757d'></span>
        <span id='sys-status-text' style='font-weight:500;color:var(--panel-fg)'>Loading...</span>
      </div>
    </div>
    <button class='btn' id='btn-sys-status-toggle' onclick="togglePane('sys-status-pane','btn-sys-status-toggle')">Expand</button>
  </div>
  <div id='sys-status-pane' style='display:none;margin-top:0.75rem'>
    <div style='background:var(--panel-bg);padding:1rem;border-radius:6px;border:1px solid var(--border)'>
      <div style='display:grid;grid-template-columns:140px 1fr;gap:0.5rem;font-size:0.95rem'>
        <div style='font-weight:500'>File:</div>
        <div id='sys-detail-file' style='font-family:monospace;color:var(--panel-fg)'>—</div>
        <div style='font-weight:500'>Last Write:</div>
        <div id='sys-detail-lastwrite'>—</div>
        <div style='font-weight:500'>Output Flags:</div>
        <div id='sys-detail-flags'>—</div>
      </div>
    </div>
  </div>
</div>

<!-- System Quick Actions Section -->
<div class='settings-panel' style='background:var(--panel-bg)'>
  <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg);margin-bottom:0.75rem'>Quick Actions</div>
  <div style='display:flex;gap:1rem;flex-wrap:wrap'>
    <button id='sys-btn-start' class='btn' onclick='startSystemLogging()' style='display:none' data-guest-hide>Start System Logging</button>
    <button id='sys-btn-stop' class='btn' onclick='stopSystemLogging()' style='display:none' data-guest-hide>Stop System Logging</button>
    <button id='sys-btn-autostart' class='btn' onclick='toggleSystemAutoStart()' data-guest-hide>Auto-Start: <span id='sys-autostart-status'>Loading...</span></button>
    <button class='btn' onclick='refreshSystemStatus()'>Refresh Status</button>
  </div>
</div>

<!-- System Configuration Section -->
<div class='settings-panel' style='background:var(--panel-bg)'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div>
      <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Configuration</div>
      <div style='color:var(--panel-fg);font-size:0.9rem'>Configure log file path and debug message categories.</div>
    </div>
    <button class='btn' id='btn-sys-config-toggle' onclick="togglePane('sys-config-pane','btn-sys-config-toggle')">Expand</button>
  </div>
  <div id='sys-config-pane' style='display:none;margin-top:0.75rem'>
    
    <!-- File Path & Options -->
    <div style='font-weight:bold;color:var(--panel-fg);margin-bottom:0.75rem'>File Path & Options</div>
      <label style='display:block;margin-bottom:1rem'>
        <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>Log File Path:</div>
        <input id='sys-config-path' type='text' placeholder='Generating timestamp...' class='form-input input-fit input-l' style='font-family:monospace' data-guest-hide>
        <small style='color:var(--panel-fg)'>Auto-generated with timestamp (NTP or uptime)</small>
      </label>
      <label style='display:flex;align-items:center;gap:0.5rem;cursor:pointer'>
        <input type='checkbox' id='sys-config-tags' checked style='margin:0;padding:0;width:16px;height:16px' data-guest-hide>
        <span style='font-size:0.95em;color:var(--panel-fg)'>Include category tags in log output (e.g., [AUTH], [HTTP])</span>
      </label>
    
    <!-- Debug Message Categories — wide multi-column grid (matches Settings bitmask layout) -->
    <div style='font-weight:bold;color:var(--panel-fg);margin:1rem 0 0.75rem'>Debug Message Categories</div>
      <div id='sys-flags-pane' style='padding:0.15rem 0;display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:0.25rem 0.75rem' data-guest-hide>
        <div style='grid-column:1/-1;color:var(--panel-fg);opacity:0.7;font-size:0.85rem'>Loading debug categories…</div>
      </div>
      <div style='margin-top:0.5rem;display:flex;gap:0.5rem'>
        <button class='btn' onclick='selectAllFlags()' style='padding:0.25rem 0.75rem;font-size:0.85rem' data-guest-hide>Select All</button>
        <button class='btn' onclick='selectNoFlags()' style='padding:0.25rem 0.75rem;font-size:0.85rem' data-guest-hide>Select None</button>
      </div>
    
    <div style='margin-top:1rem;display:flex;gap:0.5rem'>
      <button class='btn' onclick='applySystemConfig()' data-guest-hide>Apply Configuration</button>
    </div>
    <div id='sys-config-status' style='margin-top:1rem;color:var(--danger)'></div>
  </div>
</div>


  </div>
</div>

)HTML", HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, R"HTML(
<!-- Log Viewer & File Browser Section (Third Top-Level Segment) -->
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div>
      <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Log Viewer & File Browser</div>
      <div style='color:var(--panel-fg);font-size:0.9rem'>Browse, view, filter, and download log files</div>
    </div>
    <button class='btn' id='btn-viewer-section-toggle' onclick="togglePane('content-viewer','btn-viewer-section-toggle')">Expand</button>
  </div>
  <div id='content-viewer' style='display:none;margin-top:0.75rem'>

<div id='viewer-pane' style='margin-top:0.75rem'>
    
    <!-- File Selection & Actions -->
    <div style='margin-bottom:1rem'>
      <div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:0.5rem'>
        <label style='color:var(--panel-fg);font-weight:500'>Select Log File:</label>
        <button class='btn' id='btn-switch-logs' style='display:none;padding:0.25rem 0.75rem;font-size:0.85rem' onclick='switchLogSource()'>View Admin Logs</button>
      </div>
      <div id='log-viewer-file-explorer' style='margin-top:0.5rem'></div>
    </div>
    
    <!-- Filter Controls -->
    <div id='viewer-filters' style='display:none;background:var(--panel-bg);border-radius:8px;padding:1rem;margin:1rem 0;border:1px solid var(--border)'>
      <div style='font-weight:bold;color:var(--panel-fg);margin-bottom:0.75rem'>Filters</div>
      <div style='display:grid;grid-template-columns:1fr 1fr 1fr;gap:1rem'>
        <div>
          <label style='display:block;margin-bottom:0.5rem;color:var(--panel-fg)'>Category:</label>
          <select id='viewer-category-filter' onchange='applyLogFilters()' class='form-input' style='width:100%'>
            <option value=''>All Categories</option>
          </select>
        </div>
        <div>
          <label style='display:block;margin-bottom:0.5rem;color:var(--panel-fg)'>Level:</label>
          <select id='viewer-level-filter' onchange='applyLogFilters()' class='form-input' style='width:100%'>
            <option value=''>All Levels</option>
            <option value='ERROR'>ERROR</option>
            <option value='WARN'>WARN</option>
            <option value='INFO'>INFO</option>
            <option value='DEBUG'>DEBUG</option>
            <option value='EVENT'>EVENT</option>
          </select>
        </div>
        <div>
          <label style='display:block;margin-bottom:0.5rem;color:var(--panel-fg)'>Search Text:</label>
          <input id='viewer-search' type='text' oninput='applyLogFilters()' placeholder='Search log content...' class='form-input' style='width:100%'>
        </div>
      </div>
      <div style='margin-top:0.75rem;color:var(--panel-fg);font-size:0.9rem'>
        <span id='viewer-stats'>No file loaded</span>
      </div>
    </div>
    
    <!-- Log Display -->
    <div id='viewer-display' style='display:none;background:var(--terminal-bg);color:var(--terminal-fg);border-radius:8px;padding:1rem;margin:1rem 0;max-height:600px;overflow-y:auto;font-family:monospace;font-size:0.85rem;line-height:1.5;border:1px solid var(--border)'>
      <div id='viewer-content'>No log loaded</div>
    </div>
    
  </div>

  </div>
</div>

<!-- Bonded Device Logs Section (master only; hidden unless bonded + master) -->
<div class='settings-panel' id='bonded-logs-panel' style='display:none'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div>
      <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Bonded Device Logs</div>
      <div style='color:var(--panel-fg);font-size:0.9rem'>Control logging on the bonded device and retrieve its log files</div>
    </div>
    <button class='btn' id='btn-bondlog-section-toggle' onclick="toggleBondLogs()">Expand</button>
  </div>
  <div id='content-bondlog' style='display:none;margin-top:0.75rem'>
    <div style='background:var(--panel-bg);border:1px solid var(--border);border-radius:8px;padding:1rem;margin-bottom:1rem'>
      <div style='font-weight:bold;color:var(--panel-fg);margin-bottom:0.5rem'>Remote Logging Control</div>
      <div style='display:flex;gap:8px;flex-wrap:wrap;margin-bottom:0.5rem'>
        <button class='btn' onclick='bondLogCtl("sensorlog start")' data-guest-hide>Start Sensor Logging</button>
        <button class='btn' onclick='bondLogCtl("sensorlog stop")' data-guest-hide>Stop Sensor Logging</button>
        <button class='btn' onclick='bondLogCtl("log start")' data-guest-hide>Start System Logging</button>
        <button class='btn' onclick='bondLogCtl("log stop")' data-guest-hide>Stop System Logging</button>
        <button class='btn' onclick='bondLogStatus()'>Refresh Status</button>
      </div>
      <pre id='bondlog-status' style='background:var(--terminal-bg);color:var(--terminal-fg);border-radius:6px;padding:0.5rem;margin:0;max-height:180px;overflow:auto;font-size:0.8rem;white-space:pre-wrap'>Status not loaded</pre>
    </div>
    <label style='color:var(--panel-fg);font-weight:500'>Log Files on Bonded Device:</label>
    <div id='bonded-log-explorer' style='margin-top:0.5rem'></div>
  </div>
</div>

)HTML", HTTPD_RESP_USE_STRLEN);

  // JavaScript - Section 1: Initialization
  httpd_resp_send_chunk(req, R"JS(
<script>
console.log('[LOGGING] Section 1: Pre-script sentinel');
</script>
<script>
console.log('[LOGGING] Section 2: Window onload setup');
window.onload = function() {
  try {
    console.log('[LOGGING] Section 2a: Window loaded, starting initialization...');
    populateLogViewerFileList();
    initBondedLogs();
    populateFlagsPane();  // render the Debug Message Categories grid from /api/debug/flags

    // Show admin log toggle if user is admin (check via settings API which includes user.isAdmin)
    hw.fetchJSON('/api/settings')
      .then(function(data) {
        if (data && data.user && data.user.isAdmin === true) {
          hw.$('btn-switch-logs').style.display = '';
        }
      })
      .catch(function(e) {
        console.error('[LOGGING] Failed to check user role:', e);
      });

    // Dedicated status endpoint (replaces CLI batch so guests can load the page)
    hw.fetchJSON('/api/logging/status')
    .then(function(data) {
      if (!data || !Array.isArray(data.results) || data.results.length < 3) {
        console.warn('[LOGGING] Status response invalid, falling back to individual requests');
        generateDefaultFilename();
        refreshStatus();
        generateSystemFilename();
        refreshSystemStatus();
        return;
      }
      var timeText = data.results[0] || '';
      var sensorlogText = data.results[1] || '';
      var logText = data.results[2] || '';
      generateDefaultFilename(timeText);
      generateSystemFilename(timeText);
      refreshStatus(sensorlogText);
      refreshSystemStatus(logText);
    })
    .catch(function(e) {
      console.warn('[LOGGING] Status fetch failed, falling back to individual requests:', e);
      generateDefaultFilename();
      refreshStatus();
      generateSystemFilename();
      refreshSystemStatus();
    });

    console.log('[LOGGING] Section 2b: Initialization complete');
  } catch(e) {
    console.error('[LOGGING] Section 2: Window onload error:', e);
  }
};
console.log('[LOGGING] Section 2c: Window onload registered');
</script>

<script>
console.log('[LOGGING] Section 3: Filename generation function');
function generateDefaultFilename(preloadedTimeText) {
  console.log('[LOGGING] Section 3a: generateDefaultFilename called');
  function applyTime(text) {
    console.log('[LOGGING] Section 3b: Time response:', text);
    let filename = '/logging_captures/sensors/sensors-';
    const isoMatch = text.match(/Time:\s*(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})/);
    if (isoMatch) {
      console.log('[LOGGING] Section 3c: Using NTP time:', isoMatch[1]);
      const timestamp = isoMatch[1].replace(/:/g, '-');
      filename += timestamp;
    } else {
      const now = new Date();
      const timestamp = now.getFullYear() + '-' +
        String(now.getMonth() + 1).padStart(2, '0') + '-' +
        String(now.getDate()).padStart(2, '0') + 'T' +
        String(now.getHours()).padStart(2, '0') + '-' +
        String(now.getMinutes()).padStart(2, '0') + '-' +
        String(now.getSeconds()).padStart(2, '0');
      console.log('[LOGGING] Section 3d: Using browser time:', timestamp);
      filename += timestamp;
    }
    const format = hw.$('config-format').value;
    filename += (format === 'csv' ? '.csv' : format === 'track' ? '.txt' : '.log');
    console.log('[LOGGING] Section 3e: Generated filename:', filename);
    hw.$('config-path').value = filename;
  }
  if (preloadedTimeText !== undefined) {
    applyTime(preloadedTimeText);
    return;
  }
  hw.postFormText('/api/cli', { cmd: 'time' })
  .then(applyTime)
  .catch(e => {
    console.warn('[LOGGING] Section 3f: Failed to get time, using browser time:', e);
    const now = new Date();
    const timestamp = now.getFullYear() + '-' +
      String(now.getMonth() + 1).padStart(2, '0') + '-' +
      String(now.getDate()).padStart(2, '0') + 'T' +
      String(now.getHours()).padStart(2, '0') + '-' +
      String(now.getMinutes()).padStart(2, '0') + '-' +
      String(now.getSeconds()).padStart(2, '0');
    const format = hw.$('config-format').value;
    const filename = '/logging_captures/sensors/sensors-' + timestamp + (format === 'csv' ? '.csv' : format === 'track' ? '.txt' : '.log');
    hw.$('config-path').value = filename;
  });
}
console.log('[LOGGING] Section 3g: generateDefaultFilename defined');
</script>

<script>
console.log('[LOGGING] Section 4: Status refresh function');
function refreshStatus(preloadedStatusText) {
  console.log('[LOGGING] Section 4a: refreshStatus called');
  function applyStatus(text) {
    console.log('[LOGGING] Section 4c: Status response text:', text);
    if (text.includes('Unknown command') || text.includes('not found')) {
      console.log('[LOGGING] Section 4d: sensorlog command not available');
      const statusDot = hw.$('status-dot');
      const statusText = hw.$('status-text');
      statusDot.style.background = '#ffc107';
      statusText.textContent = 'Not Available';
      statusText.style.color = '#ffc107';
      hw.$('btn-start').style.display = 'none';
      hw.$('btn-stop').style.display = 'none';
      return;
    }
    const isActive = text.includes('logging ACTIVE');
    console.log('[LOGGING] Section 4d: Logging active:', isActive);
    const statusDot = hw.$('status-dot');
    const statusText = hw.$('status-text');
    const btnStart = hw.$('btn-start');
    const btnStop = hw.$('btn-stop');
    if (isActive) {
      statusDot.style.background = 'var(--success)';
      statusText.textContent = 'ACTIVE';
      statusText.style.color = 'var(--success)';
      btnStart.style.display = 'none';
      btnStop.style.display = 'inline-block';
      const fileMatch = text.match(/File:\s*(.+)/);
      const intervalMatch = text.match(/Interval:\s*(\d+)ms/);
      const formatMatch = text.match(/Format:\s*(\w+)/);
      const maxsizeMatch = text.match(/Max size:\s*(\d+)\s*bytes/);
      const rotationsMatch = text.match(/Rotations:\s*(\d+)/);
      const sensorsMatch = text.match(/Sensors:\s*(.+)/);
      const lastwriteMatch = text.match(/Last write:\s*(.+)/);
      console.log('[LOGGING] Section 4e: Parsed active status - File:', fileMatch?.[1], 'Interval:', intervalMatch?.[1], 'Format:', formatMatch?.[1], 'Sensors:', sensorsMatch?.[1]);
      hw.$('detail-file').textContent = fileMatch ? fileMatch[1] : '—';
      hw.$('detail-interval').textContent = intervalMatch ? intervalMatch[1] + 'ms' : '—';
      hw.$('detail-format').textContent = formatMatch ? formatMatch[1] : '—';
      hw.$('detail-maxsize').textContent = maxsizeMatch ? parseInt(maxsizeMatch[1]).toLocaleString() + ' bytes' : '—';
      hw.$('detail-rotations').textContent = rotationsMatch ? rotationsMatch[1] : '—';
      hw.$('detail-sensors').textContent = sensorsMatch ? sensorsMatch[1].trim() : '—';
      hw.$('detail-lastwrite').textContent = lastwriteMatch ? lastwriteMatch[1] : '—';
      if (fileMatch) hw.$('config-path').value = fileMatch[1].trim();
      if (intervalMatch) hw.$('config-interval').value = intervalMatch[1];
      if (formatMatch) hw.$('config-format').value = formatMatch[1].toLowerCase();
      if (maxsizeMatch) hw.$('config-maxsize').value = maxsizeMatch[1];
      if (rotationsMatch) hw.$('config-rotations').value = rotationsMatch[1];
    } else {
      statusDot.style.background = '#6c757d';
      statusText.textContent = 'INACTIVE';
      statusText.style.color = 'var(--panel-fg)';
      btnStart.style.display = 'inline-block';
      btnStop.style.display = 'none';
      const formatMatch = text.match(/Format:\s*(\w+)/);
      const maxsizeMatch = text.match(/Max size:\s*(\d+)\s*bytes/);
      const rotationsMatch = text.match(/Rotations:\s*(\d+)/);
      const sensorsMatch = text.match(/Sensors:\s*(.+)/);
      console.log('[LOGGING] Section 4f: Parsed inactive settings - Format:', formatMatch?.[1], 'MaxSize:', maxsizeMatch?.[1], 'Sensors:', sensorsMatch?.[1]);
      if (formatMatch) hw.$('config-format').value = formatMatch[1].toLowerCase();
      if (maxsizeMatch) hw.$('config-maxsize').value = maxsizeMatch[1];
      if (rotationsMatch) hw.$('config-rotations').value = rotationsMatch[1];
      if (sensorsMatch) {
        const sensorStr = sensorsMatch[1].toLowerCase();
        hw.qsa('#sensors-pane input[type=checkbox]').forEach(function(cb) {
          cb.checked = sensorStr.includes(cb.value);
        });
      }
    }
  }
  if (preloadedStatusText !== undefined) {
    applyStatus(preloadedStatusText);
    updateAutoStartStatus(preloadedStatusText);
    return;
  }
  hw.postFormText('/api/cli', { cmd: 'sensorlog status' })
  .then(applyStatus)
  .catch(e => {
    console.error('[LOGGING] Section 4g: Status refresh error:', e);
    hw.$('status-text').textContent = 'Error: ' + e.message;
    hw.$('status-text').style.color = 'var(--danger)';
  });
  // Also update auto-start status
  updateAutoStartStatus();
}
console.log('[LOGGING] Section 4h: refreshStatus defined');

function updateAutoStartStatus(preloadedText) {
  function applyAutoStart(text) {
    const autostartMatch = text.match(/Auto-start:\s*(ON|OFF)/i);
    const statusSpan = hw.$('autostart-status');
    if (autostartMatch) {
      const isOn = autostartMatch[1].toUpperCase() === 'ON';
      statusSpan.textContent = isOn ? 'ON' : 'OFF';
      statusSpan.style.color = isOn ? 'var(--success)' : 'var(--panel-fg)';
    } else {
      statusSpan.textContent = '?';
    }
  }
  if (preloadedText !== undefined) {
    applyAutoStart(preloadedText);
    return;
  }
  hw.postFormText('/api/cli', { cmd: 'sensorlog status' })
  .then(applyAutoStart)
  .catch(e => console.error('[LOGGING] Auto-start status error:', e));
}

function toggleAutoStart() {
  hw.postFormText('/api/cli', { cmd: 'sensorlog autostart' })
  .then(text => {
    alert(text);
    updateAutoStartStatus();
  })
  .catch(e => alert('Error toggling auto-start: ' + e.message));
}
</script>

<script>
console.log('[LOGGING] Section 6: Start logging function');
async function startLogging() {
  console.log('[LOGGING] Section 6a: startLogging called');
  const path = hw.$('config-path').value;
  const interval = hw.$('config-interval').value;
  console.log('[LOGGING] Section 6b: Start params - Path:', path, 'Interval:', interval);

  if (!path || !path.startsWith('/')) {
    alert('Error: File path must start with / (e.g., /logging_captures/sensors.csv)');
    return;
  }

  if (!await hwConfirm('Start logging to ' + path + ' every ' + interval + 'ms?')) {
    console.log('[LOGGING] Section 6c: Start cancelled by user');
    return;
  }
  
  const cmd = 'sensorlog start ' + path + ' ' + interval;
  console.log('[LOGGING] Section 6d: Executing command:', cmd);

  hw.postFormText('/api/cli', { cmd: cmd })
  .then(text => {
    console.log('[LOGGING] Section 6f: Start command result:', text);
    if (text.includes('SUCCESS') || text.includes('started')) {
      alert('Logging started successfully!');
      refreshStatus();
    } else {
      alert('Error: ' + text);
    }
  })
  .catch(e => {
    console.error('[LOGGING] Section 6g: Start logging error:', e);
    alert('Error: ' + e.message);
  });
}
console.log('[LOGGING] Section 6h: startLogging defined');
</script>

<script>
console.log('[LOGGING] Section 7: Stop logging function');
async function stopLogging() {
  console.log('[LOGGING] Section 7a: stopLogging called');
  if (!await hwConfirm('Stop sensor logging?')) {
    console.log('[LOGGING] Section 7b: Stop cancelled by user');
    return;
  }
  
  console.log('[LOGGING] Section 7c: Executing stop command');
  hw.postFormText('/api/cli', { cmd: 'sensorlog stop' })
  .then(text => {
    console.log('[LOGGING] Section 7e: Stop command result:', text);
    alert(text);
    refreshStatus();
  })
  .catch(e => {
    console.error('[LOGGING] Section 7f: Stop logging error:', e);
    alert('Error: ' + e.message);
  });
}
console.log('[LOGGING] Section 7g: stopLogging defined');
</script>

<script>
console.log('[LOGGING] Section 8: Apply configuration function');
function applyConfig() {
  console.log('[LOGGING] Section 8a: applyConfig called');
  const format = hw.$('config-format').value;
  const maxsize = hw.$('config-maxsize').value;
  const rotations = hw.$('config-rotations').value;
  console.log('[LOGGING] Section 8b: Config values - Format:', format, 'MaxSize:', maxsize, 'Rotations:', rotations);
  
  // Build sensor list dynamically from compiled-in checkboxes
  const sensors = [];
  hw.qsa('#sensors-pane input[type=checkbox]').forEach(function(cb) {
    if (cb.checked) sensors.push(cb.value);
  });
  const sensorList = sensors.length > 0 ? sensors.join(',') : 'none';
  console.log('[LOGGING] Section 8c: Selected sensors:', sensorList);
  
  hw.$('config-status').textContent = 'Applying...';
  hw.$('config-status').style.color = 'var(--accent)';
  
  const commands = [
    'sensorlog format ' + format,
    'sensorlog maxsize ' + maxsize,
    'sensorlog rotations ' + rotations,
    'sensorlog sensors ' + sensorList
  ];
  
  let results = [];
  
  console.log('[LOGGING] Section 8d: Commands to execute:', commands);
  
  function runCommand(index) {
    if (index >= commands.length) {
      console.log('[LOGGING] Section 8e: All commands completed successfully');
      hw.$('config-status').textContent = 'Configuration applied!';
      hw.$('config-status').style.color = 'var(--success)';
      setTimeout(() => {
        hw.$('config-status').textContent = '';
        refreshStatus();
      }, 2000);
      return;
    }
    
    console.log('[LOGGING] Section 8f: Executing command', index + 1, 'of', commands.length, ':', commands[index]);

    hw.postFormText('/api/cli', { cmd: commands[index] })
    .then(text => {
      console.log('[LOGGING] Section 8g: Command', index + 1, 'result:', text);
      results.push(text);
      runCommand(index + 1);
    })
    .catch(e => {
      console.error('[LOGGING] Section 8h: Command', index + 1, 'error:', e);
      hw.$('config-status').textContent = 'Error: ' + e.message;
      hw.$('config-status').style.color = 'var(--danger)';
    });
  }
  
  runCommand(0);
}
console.log('[LOGGING] Section 8i: applyConfig defined');
</script>

<script>
console.log('[LOGGING] Section 10: Sensor selection helpers');
function selectAllSensors() {
  console.log('[LOGGING] Section 10a: selectAllSensors called');
  hw.qsa('#sensors-pane input[type=checkbox]').forEach(function(cb) { cb.checked = true; });
  console.log('[LOGGING] Section 10b: All sensors selected');
}

function selectNoSensors() {
  console.log('[LOGGING] Section 10c: selectNoSensors called');
  hw.qsa('#sensors-pane input[type=checkbox]').forEach(function(cb) { cb.checked = false; });
  console.log('[LOGGING] Section 10d: All sensors deselected');
}
console.log('[LOGGING] Section 10e: Sensor selection helpers defined');
</script>

<script>
console.log('[LOGGING] Section 11: Toggle functions');

console.log('[LOGGING] Section 11g: Toggle functions defined');
</script>

<script>
console.log('[LOGGING] Section 12: Page initialization');
// No tab switching needed - using collapsible sections like other pages
</script>

<script>
console.log('[LOGGING] Section 13: System logging functions');

function generateSystemFilename(preloadedTimeText) {
  console.log('[LOGGING] generateSystemFilename called');
  function applyTime(text) {
    console.log('[LOGGING] System time response:', text);
    let filename = '/logging_captures/system/system-';
    const isoMatch = text.match(/Time:\s*(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})/);
    if (isoMatch) {
      const timestamp = isoMatch[1].replace(/:/g, '-');
      filename += timestamp;
    } else {
      const now = new Date();
      const timestamp = now.getFullYear() + '-' +
        String(now.getMonth() + 1).padStart(2, '0') + '-' +
        String(now.getDate()).padStart(2, '0') + 'T' +
        String(now.getHours()).padStart(2, '0') + '-' +
        String(now.getMinutes()).padStart(2, '0') + '-' +
        String(now.getSeconds()).padStart(2, '0');
      filename += timestamp;
    }
    filename += '.log';
    hw.$('sys-config-path').value = filename;
  }
  if (preloadedTimeText !== undefined) {
    applyTime(preloadedTimeText);
    return;
  }
  hw.postFormText('/api/cli', { cmd: 'time' })
  .then(applyTime)
  .catch(e => {
    console.warn('[LOGGING] Failed to get time, using browser time:', e);
    const now = new Date();
    const timestamp = now.getFullYear() + '-' +
      String(now.getMonth() + 1).padStart(2, '0') + '-' +
      String(now.getDate()).padStart(2, '0') + 'T' +
      String(now.getHours()).padStart(2, '0') + '-' +
      String(now.getMinutes()).padStart(2, '0') + '-' +
      String(now.getSeconds()).padStart(2, '0');
    const filename = '/logging_captures/system/system-' + timestamp + '.log';
    hw.$('sys-config-path').value = filename;
  });
}

function refreshSystemStatus(preloadedStatusText) {
  console.log('[LOGGING] refreshSystemStatus called');
  function applySystemStatus(text) {
    console.log('[LOGGING] System status response:', text);
    if (text.includes('Unknown command') || text.includes('not found')) {
      console.log('[LOGGING] log command not available');
      const statusDot = hw.$('sys-status-dot');
      const statusText = hw.$('sys-status-text');
      statusDot.style.background = '#ffc107';
      statusText.textContent = 'Not Available';
      statusText.style.color = '#ffc107';
      hw.$('sys-btn-start').style.display = 'none';
      hw.$('sys-btn-stop').style.display = 'none';
      return;
    }
    const isActive = text.includes('logging ACTIVE');
    const statusDot = hw.$('sys-status-dot');
    const statusText = hw.$('sys-status-text');
    const btnStart = hw.$('sys-btn-start');
    const btnStop = hw.$('sys-btn-stop');
    if (isActive) {
      statusDot.style.background = 'var(--success)';
      statusText.textContent = 'ACTIVE';
      statusText.style.color = 'var(--success)';
      btnStart.style.display = 'none';
      btnStop.style.display = 'inline-block';
      const fileMatch = text.match(/File:\s*(.+)/);
      const lastwriteMatch = text.match(/Last write:\s*(\d+)s ago/);
      const flagsMatch = text.match(/Output flags:\s*0x([0-9A-Fa-f]+)/);
      hw.$('sys-detail-file').textContent = fileMatch ? fileMatch[1].trim() : '—';
      hw.$('sys-detail-lastwrite').textContent = lastwriteMatch ? lastwriteMatch[1] + 's ago' : '—';
      hw.$('sys-detail-flags').textContent = flagsMatch ? '0x' + flagsMatch[1] : '—';
      if (fileMatch) hw.$('sys-config-path').value = fileMatch[1].trim();
    } else {
      statusDot.style.background = '#6c757d';
      statusText.textContent = 'INACTIVE';
      statusText.style.color = 'var(--panel-fg)';
      btnStart.style.display = 'inline-block';
      btnStop.style.display = 'none';
      hw.$('sys-detail-file').textContent = '—';
      hw.$('sys-detail-lastwrite').textContent = '—';
      hw.$('sys-detail-flags').textContent = '—';
    }
  }
  if (preloadedStatusText !== undefined) {
    applySystemStatus(preloadedStatusText);
    updateSystemAutoStartStatus(preloadedStatusText);
    return;
  }
  hw.postFormText('/api/cli', { cmd: 'log status' })
  .then(applySystemStatus)
  .catch(e => {
    console.error('[LOGGING] System status refresh error:', e);
    hw.$('sys-status-text').textContent = 'Error: ' + e.message;
    hw.$('sys-status-text').style.color = 'var(--danger)';
  });
  // Also update auto-start status
  updateSystemAutoStartStatus();
}

function startSystemLogging() {
  console.log('[LOGGING] startSystemLogging called');
  const filepath = hw.$('sys-config-path').value;
  if (!filepath) {
    alert('Please specify a log file path');
    return;
  }
  
  // Get selected debug flags
  const flags = [];
  hw.qsa('#sys-flags-pane input[type="checkbox"]:checked').forEach(cb => {
    flags.push(cb.value);
  });
  
  // Calculate combined flag value (BigInt required for flags > bit 31)
  let flagValue = BigInt(0);
  flags.forEach(f => {
    flagValue |= BigInt(f);
  });
  
  // Get category tags setting
  const categoryTags = hw.$('sys-config-tags').checked ? 1 : 0;
  
  // Build command with optional flags and tags
  let cmd = 'log start ' + filepath;
  if (flagValue > BigInt(0)) {
    cmd += ' flags=0x' + flagValue.toString(16);
  }
  cmd += ' tags=' + categoryTags;
  
  console.log('[LOGGING] Executing:', cmd);

  hw.postFormText('/api/cli', { cmd: cmd })
  .then(text => {
    console.log('[LOGGING] Start response:', text);
    if (text.includes('started')) {
      alert('System logging started successfully!');
      refreshSystemStatus();
    } else {
      alert('Error: ' + text);
    }
  })
  .catch(e => {
    console.error('[LOGGING] Start error:', e);
    alert('Error: ' + e.message);
  });
}

async function stopSystemLogging() {
  console.log('[LOGGING] stopSystemLogging called');
  if (!await hwConfirm('Stop system logging?')) return;

  hw.postFormText('/api/cli', { cmd: 'log stop' })
  .then(text => {
    console.log('[LOGGING] Stop response:', text);
    alert(text);
    refreshSystemStatus();
  })
  .catch(e => {
    console.error('[LOGGING] Stop error:', e);
    alert('Error: ' + e.message);
  });
}

function applySystemConfig() {
  console.log('[LOGGING] applySystemConfig called');
  
  // Get selected debug flags
  const flags = [];
  hw.qsa('#sys-flags-pane input[type="checkbox"]:checked').forEach(cb => {
    flags.push(cb.value);
  });
  
  if (flags.length === 0) {
    hw.$('sys-config-status').textContent = 'Warning: No debug categories selected. No messages will be logged.';
    hw.$('sys-config-status').style.color = '#ffc107';
    return;
  }
  
  // Calculate combined flag value (BigInt required for flags > bit 31)
  let flagValue = BigInt(0);
  flags.forEach(f => {
    flagValue |= BigInt(f);
  });
  
  console.log('[LOGGING] Setting debug flags to:', '0x' + flagValue.toString(16));
  
  // Store for use when starting logging
  hw.$('sys-config-status').textContent = 'Configuration saved. Click "Start System Logging" to apply with these flags.';
  hw.$('sys-config-status').style.color = 'var(--success)';
}

function selectAllFlags() {
  hw.qsa('#sys-flags-pane input[type="checkbox"]').forEach(cb => cb.checked = true);
}

function selectNoFlags() {
  hw.qsa('#sys-flags-pane input[type="checkbox"]').forEach(cb => cb.checked = false);
}

// ---------------------------------------------------------------------------
// Debug Message Categories pane — rendered from GET /api/debug/flags so the
// grid always matches the firmware's DBG_FLAG_LIST table (no hand-maintained
// checkbox list to drift). The endpoint returns one object per bit-bearing
// debug flag; expected/accepted per-row fields (all tolerated, with aliases):
//     mask  (alias value)  : the flag's bitmask as a hex string, e.g. "0x100000000"
//     tag                  : writer-side category, e.g. "AUTH", "ESP-NOW", "GPS_LIFE"
//     bank  (alias bankLabel): family bank LABEL, e.g. "Core", "ESP-NOW", "GPS"
//     label                : UI text for the checkbox
// Rows are already in table (bank) order, so we emit a bank header whenever the
// bank changes. Every checkbox keeps value='0x<mask>' inside #sys-flags-pane, so
// selectAllFlags / selectNoFlags and the BigInt mask builders in
// startSystemLogging() / applySystemConfig() keep working byte-for-byte
// unchanged. Rows without a mask (e.g. a control bit) are skipped as checkboxes
// but still feed the tag->bank colour map. gFlagBankByTag is consumed by
// getCategoryColor() to colour the log viewer by family bank.
var gFlagBankByTag = {};
// Brand colours for the named non-CORE families; any bank not listed here (the
// individual sensor banks) gets a stable hashed hue in getCategoryColor().
var gBankColor = {
  'Memory':'#b5cea8', 'ESP-NOW':'#d16969', 'MQTT':'#4ec9b0', 'Automations':'#d7ba7d',
  'Bluetooth':'#4fc1ff', 'G2':'#9cdcfe', 'Speech':'#ce9178', 'LLM':'#c586c0',
  'Maps':'#dcdcaa', 'Camera':'#d16fa8', 'I2C':'#56b6c2'
};

function populateFlagsPane() {
  var pane = hw.$('sys-flags-pane');
  if (!pane) return;
  hw.fetchJSON('/api/debug/flags')
    .then(function(data) {
      var rows = Array.isArray(data) ? data
               : (data && (data.flags || data.rows)) ? (data.flags || data.rows) : [];
      if (!rows.length) {
        pane.innerHTML = '<div style="grid-column:1/-1;color:var(--panel-fg);opacity:0.7;font-size:0.85rem">No debug categories returned.</div>';
        return;
      }
      var html = '';
      var curBank = null;
      rows.forEach(function(r) {
        // The endpoint sends the integer bit; the checkbox value must be the hex
        // bitmask (1<<bit) that startSystemLogging()'s BigInt(cb.value) OR-folds.
        var mask = r.mask || r.value ||
                   (typeof r.bit === 'number' ? '0x' + (1n << BigInt(r.bit)).toString(16) : null);
        var tag  = r.tag || '';
        var bank = r.bank || r.bankLabel || '';
        var label = r.label || tag || mask;
        // Feed the colour map for every row (normalise ESP-NOW -> ESP_NOW to
        // match the log parser, which underscores category hyphens).
        if (tag) gFlagBankByTag[String(tag).replace(/-/g, '_')] = bank;
        if (!mask) return;  // no bit -> cannot ride in a flags= mask; skip checkbox
        if (bank !== curBank) {
          curBank = bank;
          html += '<div style="grid-column:1/-1;font-size:0.75rem;font-weight:600;color:var(--panel-fg);text-transform:uppercase;padding:0.35rem 0 0.1rem;border-bottom:1px solid var(--border);margin-top:0.25rem">' + escapeHtml(bank || 'Flags') + '</div>';
        }
        var slug = String(tag || mask).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
        html += '<label style="display:flex;align-items:center;gap:0.4rem;cursor:pointer">'
             +  '<input type="checkbox" id="flag-' + slug + '" value="' + mask + '" style="margin:0;padding:0;width:auto;flex:0 0 auto;border:none;background:transparent">'
             +  '<span style="font-size:0.9em;color:var(--panel-fg)">' + escapeHtml(label) + '</span>'
             +  '</label>';
      });
      pane.innerHTML = html;
    })
    .catch(function(e) {
      console.error('[LOGGING] Failed to load debug flags:', e);
      pane.innerHTML = '<div style="grid-column:1/-1;color:var(--danger);font-size:0.85rem">Failed to load debug categories: ' + escapeHtml(e && e.message ? e.message : String(e)) + '</div>';
    });
}

function updateSystemAutoStartStatus(preloadedText) {
  function applySysAutoStart(text) {
    const autostartMatch = text.match(/Auto-start:\s*(ON|OFF)/i);
    const statusSpan = hw.$('sys-autostart-status');
    if (autostartMatch) {
      const isOn = autostartMatch[1].toUpperCase() === 'ON';
      statusSpan.textContent = isOn ? 'ON' : 'OFF';
      statusSpan.style.color = isOn ? 'var(--success)' : 'var(--panel-fg)';
    } else {
      statusSpan.textContent = '?';
    }
  }
  if (preloadedText !== undefined) {
    applySysAutoStart(preloadedText);
    return;
  }
  hw.postFormText('/api/cli', { cmd: 'log status' })
  .then(applySysAutoStart)
  .catch(e => console.error('[LOGGING] System auto-start status error:', e));
}

function toggleSystemAutoStart() {
  hw.postFormText('/api/cli', { cmd: 'log autostart' })
  .then(text => {
    alert(text);
    updateSystemAutoStartStatus();
  })
  .catch(e => alert('Error toggling system log auto-start: ' + e.message));
}

console.log('[LOGGING] Section 13a: System logging functions defined');
</script>

<script>
console.log('[LOGGING] Section 14: Log viewer functions');

// Global log viewer state
let gLogLines = [];
let gFilteredLines = [];

function loadLogFile(filepath) {
  if (!filepath) {
    alert('Please select a log file');
    return;
  }
  
  console.log('[LOGGING] Loading log file:', filepath);
  
  // Use streaming fetch with mode=raw to get plain text. dec=1 asks the
  // server to reveal sealed (at-rest encrypted) captures for this parse —
  // a no-op for plaintext files.
  hw.fetchText('/api/files/view?name=' + encodeURIComponent(filepath) + '&mode=raw&dec=1')
    .then(text => {
      parseLogFile(text);
      hw.$('viewer-filters').style.display = 'block';
      hw.$('viewer-display').style.display = 'block';
    })
    .catch(e => {
      console.error('[LOGGING] Failed to load log file:', e);
      alert('Error loading log file: ' + e.message);
    });
}

function parseLogFile(text) {
  const lines = text.split(/\r?\n/).filter(l => l.trim().length > 0);
  gLogLines = [];
  const categories = new Set();
  
  lines.forEach(line => {
    let logLine = null;
    let match;

    // Format 0: wall-clock or boot-millis prefixed lines. The always-on admin
    // logs (errors.log, system-events.log, events.log, i2c_errors.log) use
    // buildTimestampPrefix(): "[YYYY-MM-DD HH:MM:SS.mmm] | msg", or "[ms=N] msg"
    // before a valid wall clock exists. Strip the prefix, then classify the
    // remainder with the same tag salvage the digit formats use.
    let body = null;
    let tsDisplay = null;
    let tsValue = 0;
    match = line.match(/^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\]\s*\|\s*(.*)$/);
    if (match) {
      tsDisplay = match[1];
      tsValue = Date.parse(match[1].replace(' ', 'T')) || 0;
      body = match[2];
    } else {
      match = line.match(/^\[ms=(\d+)\]\s*(.*)$/);
      if (match) {
        tsDisplay = 'ms=' + match[1];
        tsValue = parseInt(match[1]);
        body = match[2];
      }
    }
    if (body !== null) {
      // Typed event stream: "[EVLOG] #seq KIND | source[:who] | subject | detail".
      // KIND comes from the SYSEVT name table, so use it as the category —
      // the dropdown then filters on real event kinds. ("[EVLOG] !gap ..."
      // ring-overwrite markers fail this match and file under EVLOG below.)
      match = body.match(/^\[EVLOG\]\s*#(\d+)\s+([A-Z][A-Z0-9_-]*)\s*\|\s*(.*)$/);
      if (match) {
        logLine = {
          timestamp: tsValue,
          tsDisplay: tsDisplay,
          category: match[2].replace(/-/g, '_'),
          level: 'EVENT',
          message: '#' + match[1] + ' ' + match[3],
          raw: line
        };
      } else if ((match = body.match(/^\[(ERROR|WARN|INFO|DEBUG|EVENT)\]\[([A-Z][A-Z0-9_-]*)\]\s*(.*)$/))) {
        logLine = {
          timestamp: tsValue,
          tsDisplay: tsDisplay,
          category: match[2].replace(/-/g, '_'),
          level: match[1],
          message: match[3],
          raw: line
        };
      } else if ((match = body.match(/^\[([A-Z][A-Z0-9_-]*)\]\s*(.*)$/))) {
        logLine = {
          timestamp: tsValue,
          tsDisplay: tsDisplay,
          category: match[1].replace(/-/g, '_'),
          level: 'DEBUG',
          message: match[2],
          raw: line
        };
      } else if (/^I2C (ERROR|RECOVER)/.test(body)) {
        // i2c_errors.log lines carry no bracket tag ("I2C ERROR | addr=...")
        logLine = {
          timestamp: tsValue,
          tsDisplay: tsDisplay,
          category: 'I2C',
          level: body.indexOf('I2C ERROR') === 0 ? 'ERROR' : 'INFO',
          message: body,
          raw: line
        };
      } else {
        logLine = {
          timestamp: tsValue,
          tsDisplay: tsDisplay,
          category: 'GENERAL',
          level: 'DEBUG',
          message: body,
          raw: line
        };
      }
      categories.add(logLine.category);
    }

    // Format 1: Debug logs - [timestamp] [CATEGORY] message
    // Allow letters, digits, underscores, hyphens so [ESP-NOW], [CMD_SYS] etc. all match
    match = logLine ? null : line.match(/^\[(\d+)\]\s*\[([A-Z][A-Z0-9_-]*)\]\s*(.*)$/);
    if (match) {
      const [, timestamp, category, message] = match;
      const cat = category.replace(/-/g, '_');  // normalise ESP-NOW → ESP_NOW for color lookup
      // Extract level from leading [LEVEL] prefix in message, e.g. "[ERROR][WEB] ..."
      const levelMatch = message.match(/^\[(ERROR|WARN|INFO|DEBUG|EVENT)\]\[([A-Z][A-Z0-9_-]*)\]\s*(.*)$/);
      let level = 'DEBUG';
      let finalCat = cat;
      let finalMsg = message.trim();
      if (levelMatch) {
        level = levelMatch[1];
        finalCat = levelMatch[2].replace(/-/g, '_');
        finalMsg = levelMatch[3].trim();
      }
      logLine = {
        timestamp: parseInt(timestamp),
        category: finalCat,
        level: level,
        message: finalMsg,
        raw: line
      };
      categories.add(logLine.category);
    }
    
    // Format 2: Command audit logs - [timestamp] user@source command -> result
    if (!logLine) {
      match = line.match(/^\[(\d+)\]\s+(\w+)@(\w+)\s+(.+?)\s*->\s*(.*)$/);
      if (match) {
        const [, timestamp, user, source, command, result] = match;
        const category = source.toUpperCase();
        logLine = {
          timestamp: parseInt(timestamp),
          category: category,
          level: 'INFO',
          user: user,
          command: command,
          result: result,
          message: user + '@' + source + ' ' + command + ' -> ' + result,
          raw: line
        };
        categories.add(category);
        console.log('[LOG_PARSE] Matched command audit:', category, user, command);
      } else {
        // Debug: Check if line looks like command audit format
        if (line.includes('@') && line.includes('->')) {
          console.log('[LOG_PARSE] Failed to match command audit line:', line);
        }
      }
    }
    
    // Format 3: Simple timestamp - [timestamp] message
    // Try to salvage a category from an inline [TAG] prefix in the message body,
    // since broadcastOutput() calls don't carry a debug flag to the file writer.
    if (!logLine) {
      match = line.match(/^\[(\d+)\]\s*(.*)$/);
      if (match) {
        const [, timestamp, message] = match;
        const msg = message.trim();
        // Check for [LEVEL][CATEGORY] prefix first
        const levelCatTag = msg.match(/^\[(ERROR|WARN|INFO|DEBUG|EVENT)\]\[([A-Z][A-Z0-9_-]*)\]\s*(.*)/);
        if (levelCatTag) {
          const cat = levelCatTag[2].replace(/-/g, '_');
          logLine = {
            timestamp: parseInt(timestamp),
            category: cat,
            level: levelCatTag[1],
            message: levelCatTag[3].trim(),
            raw: line
          };
          categories.add(cat);
        } else {
          // Check if message starts with an inline [TAG] and use that as category
          const inlineTag = msg.match(/^\[([A-Z][A-Z0-9_-]*)\]\s*(.*)/);
          if (inlineTag) {
            const cat = inlineTag[1].replace(/-/g, '_');
            logLine = {
              timestamp: parseInt(timestamp),
              category: cat,
              level: 'DEBUG',
              message: inlineTag[2],
              raw: line
            };
            categories.add(cat);
          } else {
            logLine = {
              timestamp: parseInt(timestamp),
              category: 'GENERAL',
              level: 'DEBUG',
              message: msg,
              raw: line
            };
            categories.add('GENERAL');
          }
        }
      }
    }
    
    // Fallback: Malformed line, add as-is
    if (!logLine) {
      logLine = {
        timestamp: 0,
        category: 'UNKNOWN',
        level: 'DEBUG',
        message: line,
        raw: line
      };
    }
    
    gLogLines.push(logLine);
  });
  
  // Populate category filter
  const categoryFilter = hw.$('viewer-category-filter');
  categoryFilter.innerHTML = '<option value="">All Categories</option>';
  Array.from(categories).sort().forEach(cat => {
    const opt = document.createElement('option');
    opt.value = cat;
    opt.textContent = cat;
    categoryFilter.appendChild(opt);
  });
  
  // Apply initial filter (show all)
  applyLogFilters();
}

function applyLogFilters() {
  const categoryFilter = hw.$('viewer-category-filter').value;
  const levelFilter = hw.$('viewer-level-filter').value;
  const searchText = hw.$('viewer-search').value.toLowerCase();
  
  gFilteredLines = gLogLines.filter(line => {
    // Category filter
    if (categoryFilter && line.category !== categoryFilter) return false;
    
    // Level filter
    if (levelFilter && line.level !== levelFilter) return false;
    
    // Search filter
    if (searchText && !line.raw.toLowerCase().includes(searchText)) return false;
    
    return true;
  });
  
  displayLogLines();
  updateViewerStats();
}

function displayLogLines() {
  const content = hw.$('viewer-content');
  
  if (gFilteredLines.length === 0) {
    content.innerHTML = '<div style="color:#888">No matching log entries</div>';
    return;
  }
  
  const levelColors = { 'ERROR': '#f44747', 'WARN': '#ffc107', 'INFO': '#6a9955', 'DEBUG': '#569cd6', 'EVENT': '#c586c0' };
  let html = '';
  gFilteredLines.forEach(line => {
    const categoryColor = getCategoryColor(line.category);
    const levelColor = levelColors[line.level] || '#888';
    const timestampStr = line.tsDisplay || line.timestamp.toString();
    
    if (line.category !== 'UNKNOWN') {
      html += '<div style="margin:2px 0">';
      html += '<span style="color:#569cd6">[' + timestampStr + ']</span> ';
      html += '<span style="color:' + levelColor + ';font-weight:bold;font-size:0.8em;margin-right:3px">' + line.level + '</span>';
      
      // Special formatting for command audit logs (user@source format)
      if (line.user && line.command) {
        html += '<span style="color:' + categoryColor + ';font-weight:bold">[' + line.category + ']</span> ';
        html += '<span style="color:var(--panel-fg);white-space:pre">' + escapeHtml(line.user) + '@' + escapeHtml(line.category.toLowerCase()) + ' ' + escapeHtml(line.command) + ' -> ' + escapeHtml(line.result) + '</span>';
      } else {
        // Standard debug log format
        html += '<span style="color:' + categoryColor + ';font-weight:bold">[' + line.category + ']</span> ';
        html += '<span style="color:var(--panel-fg);white-space:pre">' + escapeHtml(line.message) + '</span>';
      }
      
      html += '</div>';
    } else {
      html += '<div style="margin:2px 0;color:var(--muted);white-space:pre">' + escapeHtml(line.raw) + '</div>';
    }
  });
  
  content.innerHTML = html;
}

function getCategoryColor(category) {
  // CORE-bank subsystems stay per-tag (bank-keying the whole CORE bank to one
  // colour would be a regression). Also covers non-flag categories the log
  // parser emits — command-audit sources (WEB/CMD), the sensor DATA logger
  // (SENSORS*/GAMEPAD), Memory aliases (MEM/HEAP/STACK), and severity words —
  // none of which are debug-flag tags and so are absent from gFlagBankByTag.
  const colors = {
    // Core system (per-tag)
    'AUTH': '#f48771',       'SESSION': '#f48771',
    'HTTP': '#4ec9b0',       'WEB': '#4ec9b0',        'HTTPS': '#4ec9b0',
    'SSE': '#4fc1ff',
    'CLI': '#dcdcaa',        'CMD': '#dcdcaa',        'CMD_FLOW': '#dcdcaa',   'CMD_SYS': '#dcdcaa',
    'SYSTEM': '#569cd6',     'SYS': '#569cd6',        'BOOT': '#569cd6',
    'NTP': '#569cd6',        'DISPLAY': '#569cd6',    'OLED': '#569cd6',
    'STORAGE': '#9cdcfe',    'SETTINGS_SYS': '#9cdcfe',
    'WIFI': '#ce9178',
    'SECURITY': '#f48771',
    'USERS': '#f48771',      'USER': '#f48771',
    'LOGGER': '#608b4e',     'LOG': '#608b4e',
    'NOTIFICATIONS': '#d19a66', 'NOTIF': '#d19a66',
    // Performance / memory (Memory bank kept green to match Performance)
    'PERF': '#b5cea8',       'PERFORMANCE': '#b5cea8', 'MEMORY': '#b5cea8',    'MEM': '#b5cea8',
    'HEAP': '#b5cea8',       'STACK': '#b5cea8',
    // Sensor DATA logger categories (not debug-flag banks)
    'SENSORS': '#c586c0',    'SENSORS_FRAME': '#c586c0', 'SENSORS_DATA': '#c586c0', 'GAMEPAD': '#c586c0',
    // Named non-CORE family parents, mirroring gBankColor so log-viewer colouring
    // is fetch-independent (a non-admin viewer never loads /api/debug/flags, so
    // gFlagBankByTag is empty for them; without these the families would hash).
    'ESP-NOW': '#d16969',    'ESP_NOW': '#d16969',
    'MQTT': '#4ec9b0',
    'AUTO': '#d7ba7d',       'AUTO_EXEC': '#d7ba7d',   'AUTO_COND': '#d7ba7d',
    'AUTO_TIME': '#d7ba7d',  'AUTO_SCHED': '#d7ba7d',
    'BT': '#4fc1ff',         'G2': '#9cdcfe',          'SR': '#ce9178',
    'LLM': '#c586c0',        'MAPS': '#dcdcaa',        'CAMERA': '#d16fa8',   'I2C': '#56b6c2',
    // Severity prefixes (from ERROR/WARN/INFO macros)
    'ERROR': '#f44747',      'WARN': '#ffc107',        'INFO': '#6a9955',
  };
  if (colors[category]) return colors[category];

  // Bank-keyed: colour a non-CORE tag by its family bank. tag->bank comes from
  // /api/debug/flags (gFlagBankByTag); named families keep a brand hue from
  // gBankColor, and any other bank (each individual sensor) gets a stable
  // hashed hue so the whole family reads as one colour. CORE-bank tags are
  // skipped here so they keep the per-tag colours above.
  function bankColor(tag) {
    var bank = gFlagBankByTag[tag];
    if (!bank || bank === 'Core') return null;
    if (gBankColor[bank]) return gBankColor[bank];
    var hb = 0;
    for (var i = 0; i < bank.length; i++) hb = (hb * 31 + bank.charCodeAt(i)) >>> 0;
    return 'hsl(' + (hb % 360) + ', 62%, 62%)';
  }
  var bc = bankColor(category);
  if (bc) return bc;

  // Sub-flag tags (THERMAL_POLL, I2C_BUS, SR_WAKE, LLM_LOAD…) inherit their
  // parent's colour so a family reads as one colour across its sub-categories —
  // first the per-tag CORE map, then the parent's family bank.
  const parent = category.replace(/_(LIFE|POLL|VAL|LOAD|BUS|CORE|GATT|DATA|CONN|PUBSUB|DISCOVERY|AUTOSTART|HEAP|STACK|BUFFERS|CMD|WAKE|AFE|TUNE|TOK|FWD|GEN|MEM|META|RENDER|PERF|EXEC|COND|TIME|SCHED|PROTO|EVT|PAGE|HB|DUMP|LIFECYCLE|CAPTURE|SETTINGS|VIDEO)$/, '');
  if (parent !== category) {
    if (colors[parent]) return colors[parent];
    var pbc = bankColor(parent);
    if (pbc) return pbc;
  }
  // Deterministic fallback: hash the FAMILY name (stripped parent) so sub-tags
  // of one unmapped family still share a hue, and never render gray.
  let h = 0;
  for (let i = 0; i < parent.length; i++) h = (h * 31 + parent.charCodeAt(i)) >>> 0;
  return 'hsl(' + (h % 360) + ', 55%, 60%)';
}

function escapeHtml(text) {
  const div = document.createElement('div');
  div.textContent = text;
  return div.innerHTML;
}

function updateViewerStats() {
  const stats = hw.$('viewer-stats');
  const total = gLogLines.length;
  const filtered = gFilteredLines.length;
  
  if (total === filtered) {
    stats.textContent = 'Showing ' + total + ' log entries';
  } else {
    stats.textContent = 'Showing ' + filtered + ' of ' + total + ' log entries';
  }
}

var currentLogSource = '/logging_captures';  // Track current log source

function populateLogViewerFileList() {
  // Initialize file explorer for log selection
  if (typeof window.createFileExplorer === 'function') {
    window.createFileExplorer({
      containerId: 'log-viewer-file-explorer',
      path: currentLogSource,
      height: '250px',
      mode: 'full',
      selectFilesOnly: true,
      lockToPath: currentLogSource,  // Lock navigation to current log source
      onSelect: function(filePath) {
        console.log('[LOGGING] Selected log file:', filePath);
        loadLogFile(filePath);
      },
      filter: function(file) {
        // Show all files in logs directory
        return true;
      }
    });
  } else {
    console.error('[LOGGING] createFileExplorer not available');
    hw.$('log-viewer-file-explorer').innerHTML = 
      '<div style="padding:1rem;color:#c00;border:1px solid #c00;border-radius:4px">File explorer component not loaded. Please refresh the page.</div>';
  }
}

function switchLogSource() {
  // Toggle between /logging_captures and /system/sys_logs
  if (currentLogSource === '/logging_captures') {
    currentLogSource = '/system/sys_logs';
    hw.$('btn-switch-logs').textContent = 'View User Logs';
  } else {
    currentLogSource = '/logging_captures';
    hw.$('btn-switch-logs').textContent = 'View Admin Logs';
  }
  
  // Reload file explorer with new source
  populateLogViewerFileList();
  
  // Clear current log viewer
  hw.$('viewer-content').innerHTML = '<div style="padding:1rem;color:var(--panel-fg)">Select a log file to view</div>';
}

console.log('[LOGGING] Section 14a: Log viewer functions defined');

// ===========================================================================
// Bonded Device Logs (master only). Retrieve + control the peer's logging over
// the bond session token, via the shared window.BondFs helper. The whole panel
// stays hidden unless this device is bonded AND is the master.
// ===========================================================================
var bondLogLoaded = false;
var bondLogPath = '/logging_captures';

function initBondedLogs(){
  if (!window.BondFs) return;
  window.BondFs.checkAvailable(function(ok){
    if (!ok) return;
    var p = hw.$('bonded-logs-panel');
    hw.show(p);
  });
}

function toggleBondLogs(){
  togglePane('content-bondlog','btn-bondlog-section-toggle');
  if (!bondLogLoaded){
    bondLogLoaded = true;
    bondLogStatus();
    bondLogBrowse(bondLogPath);
  }
}

function bondLogStatus(){
  var el = hw.$('bondlog-status');
  if (!window.BondFs || !el) return;
  el.textContent = 'Loading status…';
  window.BondFs.exec('sensorlog status', { onResult: function(lines, err){
    var sensor = lines ? lines.join('\n') : ('error: ' + (err || ''));
    window.BondFs.exec('log status', { onResult: function(l2, e2){
      var sys = l2 ? l2.join('\n') : ('error: ' + (e2 || ''));
      el.textContent = '$ sensorlog status\n' + sensor + '\n\n$ log status\n' + sys;
    }});
  }});
}

function bondLogCtl(cmd){
  var el = hw.$('bondlog-status');
  if (!window.BondFs || !el) return;
  el.textContent = 'Running: ' + cmd + ' …';
  window.BondFs.exec(cmd, { onResult: function(lines, err){
    el.textContent = '$ ' + cmd + '\n' + (lines ? lines.join('\n') : ('error: ' + (err || '')));
    setTimeout(bondLogStatus, 900);  // refresh status after the change settles
  }});
}

function bondLogBrowse(path){
  path = path || '/logging_captures';
  bondLogPath = path;
  if (!window.BondFs) return;
  window.BondFs.renderExplorer('bonded-log-explorer', path, [], { onNavigate: bondLogBrowse, fileActions: [], status: 'Loading…' });
  window.BondFs.list(path, function(entries, err){
    if (entries === null){
      window.BondFs.renderExplorer('bonded-log-explorer', path, [], { onNavigate: bondLogBrowse, fileActions: [], status: 'Error: ' + (err || 'failed') });
      return;
    }
    window.BondFs.renderExplorer('bonded-log-explorer', path, entries, {
      onNavigate: bondLogBrowse,
      fileActions: [
        { label: 'View', fn: bondLogView },
        { label: 'Download', fn: bondLogDownload }
      ]
    });
  });
}

// View: pull the peer's log to THIS device, then load it into the existing Log
// Viewer (reusing its Category/Level/Search filters and display).
function bondLogView(remotePath){
  if (!window.BondFs) return;
  window.BondFs.pull(remotePath, function(res, err){
    if (err || !res){ alert('Pull failed: ' + (err || 'unknown')); return; }
    var content = hw.$('content-viewer');
    if (content && content.style.display === 'none'){ togglePane('content-viewer','btn-viewer-section-toggle'); }
    if (typeof loadLogFile === 'function') loadLogFile(res.localPath);
    var disp = hw.$('viewer-display');
    if (disp) disp.scrollIntoView({ behavior: 'smooth' });
  });
}

function bondLogDownload(remotePath){
  if (!window.BondFs) return;
  window.BondFs.pull(remotePath, function(res, err){
    if (err || !res){ alert('Pull failed: ' + (err || 'unknown')); return; }
    window.location = res.localUrl;
  });
}
</script>
)JS", HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, R"JS(
<script>
console.log('[LOGGING] Section 15: All JavaScript loaded successfully');
</script>
</body>
</html>)JS", HTTPD_RESP_USE_STRLEN);

}

#endif
