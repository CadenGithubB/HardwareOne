#ifndef WEBPAGE_LOGGING_H
#define WEBPAGE_LOGGING_H

#include "WebServer_Utils.h"  // For getFileBrowserScript
#include "System_BuildConfig.h"

// Streamed inner content for logging page
inline void streamLoggingInner(httpd_req_t* req) {
  // Load file browser script for log viewer
  String fbScript = getFileBrowserScript();
  httpd_resp_send_chunk(req, fbScript.c_str(), fbScript.length());
  
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
    <button id='btn-start' class='btn' onclick='startLogging()' style='display:none'>Start Logging</button>
    <button id='btn-stop' class='btn' onclick='stopLogging()' style='display:none'>Stop Logging</button>
    <button id='btn-autostart' class='btn' onclick='toggleAutoStart()'>Auto-Start: <span id='autostart-status'>Loading...</span></button>
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
    
    <!-- Logging Parameters Section (Combined) -->
    <div class='settings-panel'>
      <div style='display:flex;align-items:center;justify-content:space-between'>
        <div style='font-weight:bold;color:var(--panel-fg)'>Logging Parameters</div>
        <button class='btn' id='btn-params-toggle' onclick="toggleConfigPane('params-pane','btn-params-toggle')" style='padding:0.25rem 0.75rem;font-size:0.85rem'>Expand</button>
      </div>
      <div id='params-pane' style='display:none;margin-top:0.75rem'>
        <label style='display:block;margin-bottom:1rem'>
          <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>File Path:</div>
          <input id='config-path' type='text' placeholder='Generating timestamp...' class='form-input' style='width:100%;font-family:monospace'>
          <small style='color:var(--panel-fg)'>Auto-generated with timestamp (NTP or uptime)</small>
        </label>
        <label style='display:block;margin-bottom:1rem'>
          <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>Interval (ms):</div>
          <input id='config-interval' type='number' value='5000' min='100' max='3600000' class='form-input' style='width:100%'>
          <small style='color:var(--panel-fg)'>Min: 100ms, Max: 1 hour (3600000ms)</small>
        </label>
        <label style='display:block;margin-bottom:1rem'>
          <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>Format:</div>
          <select id='config-format' class='form-input' style='width:100%'>
            <option value='text'>Text (Human-readable)</option>
            <option value='csv'>CSV (Structured data)</option>
            <option value='track'>Track (GPS-only compact with signal loss dedup)</option>
          </select>
        </label>
        <label style='display:block;margin-bottom:1rem'>
          <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>Max File Size (bytes):</div>
          <input id='config-maxsize' type='number' value='256000' min='10240' max='10485760' class='form-input' style='width:100%'>
          <small style='color:var(--panel-fg)'>Min: 10KB, Max: 10MB</small>
        </label>
        <label style='display:block'>
          <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>Rotations (old logs to keep):</div>
          <input id='config-rotations' type='number' value='3' min='0' max='9' class='form-input' style='width:100%'>
          <small style='color:var(--panel-fg)'>0 = delete old logs, 1-9 = keep N old files</small>
        </label>
      </div>
    </div>
    
    <!-- Sensors Section -->
    <div class='settings-panel'>
      <div style='display:flex;align-items:center;justify-content:space-between'>
        <div style='font-weight:bold;color:var(--panel-fg)'>Sensors to Log</div>
        <button class='btn' id='btn-sensors-toggle' onclick="toggleConfigPane('sensors-pane','btn-sensors-toggle')" style='padding:0.25rem 0.75rem;font-size:0.85rem'>Expand</button>
      </div>
      <div id='sensors-pane' style='display:none;margin-top:0.75rem'>
)HTML", HTTPD_RESP_USE_STRLEN);
#if ENABLE_THERMAL_SENSOR
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-thermal' value='thermal' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Thermal (temperature array)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_TOF_SENSOR
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-tof' value='tof' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>ToF (distance sensors)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_IMU_SENSOR
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-imu' value='imu' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>IMU (orientation, accel, gyro, temp)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_GAMEPAD_SENSOR
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-gamepad' value='gamepad' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Gamepad (buttons, joystick)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_APDS_SENSOR
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-apds' value='apds' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>APDS (color, proximity, gesture)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_GPS_SENSOR
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-gps' value='gps' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>GPS (position, speed, altitude)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_PRESENCE_SENSOR
  httpd_resp_send_chunk(req, R"HTML(
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-presence' value='presence' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Presence (IR presence/motion)</span>
          </label>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif
  httpd_resp_send_chunk(req, R"HTML(
        <div style='margin-top:0.5rem;display:flex;gap:0.5rem'>
          <button class='btn' onclick='selectAllSensors()' style='padding:0.25rem 0.75rem;font-size:0.85rem'>Select All</button>
          <button class='btn' onclick='selectNoSensors()' style='padding:0.25rem 0.75rem;font-size:0.85rem'>Select None</button>
        </div>
      </div>
    </div>
    
    <div style='margin-top:1rem;display:flex;gap:0.5rem'>
      <button class='btn' onclick='applyConfig()'>Apply Configuration</button>
    </div>
    <div id='config-status' style='margin-top:1rem;color:#dc3545'></div>
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
    <button id='sys-btn-start' class='btn' onclick='startSystemLogging()' style='display:none'>Start System Logging</button>
    <button id='sys-btn-stop' class='btn' onclick='stopSystemLogging()' style='display:none'>Stop System Logging</button>
    <button id='sys-btn-autostart' class='btn' onclick='toggleSystemAutoStart()'>Auto-Start: <span id='sys-autostart-status'>Loading...</span></button>
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
    
    <!-- File Path Section -->
    <div class='settings-panel'>
      <div style='font-weight:bold;color:var(--panel-fg);margin-bottom:0.75rem'>File Path & Options</div>
      <label style='display:block;margin-bottom:1rem'>
        <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>Log File Path:</div>
        <input id='sys-config-path' type='text' placeholder='Generating timestamp...' class='form-input' style='width:100%;font-family:monospace'>
        <small style='color:var(--panel-fg)'>Auto-generated with timestamp (NTP or uptime)</small>
      </label>
      <label style='display:flex;align-items:center;gap:0.5rem;cursor:pointer'>
        <input type='checkbox' id='sys-config-tags' checked style='margin:0;padding:0;width:16px;height:16px'>
        <span style='font-size:0.95em;color:var(--panel-fg)'>Include category tags in log output (e.g., [AUTH], [HTTP])</span>
      </label>
    </div>
    
    <!-- Debug Flags Section -->
    <div class='settings-panel'>
      <div style='font-weight:bold;color:var(--panel-fg);margin-bottom:0.75rem'>Debug Message Categories</div>
      <div style='padding:0.5rem;background:var(--panel-bg);border:1px solid var(--border);border-radius:4px;max-height:300px;overflow-y:auto'>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-auth' value='0x0001' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Authentication</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-http' value='0x0002' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>HTTP Requests</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-sse' value='0x0004' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Server-Sent Events</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-cli' value='0x0008' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>CLI Processing</span>
          </label>

          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-sensors' value='0x0040' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>General Sensor Operations</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-wifi' value='0x0200' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>WiFi Operations</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-storage' value='0x8000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Storage / Security</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-performance' value='0x0400' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Performance Metrics</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-system' value='0x4000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>System/Boot Operations</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-users' value='0x2000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>User Management</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-automations' value='0x10000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Automations</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-logger' value='0x20000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Sensor Logger Internals</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-camera' value='0x20000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Camera Operations</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-microphone' value='0x0800' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Microphone Operations</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-fmradio' value='0x0080' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>FM Radio</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-espnow' value='0x10000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>ESP-NOW Core</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-memory' value='0x40000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Memory Operations</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-i2c' value='0x0100' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>I2C Bus</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-cmdflow' value='0x1000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Command Flow</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-espnow-router' value='0x80000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>ESP-NOW Router</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-espnow-mesh' value='0x100000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>ESP-NOW Mesh</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-espnow-topo' value='0x200000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>ESP-NOW Topology</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-espnow-stream' value='0x400000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>ESP-NOW Stream</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-command-system' value='0x800000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Command System</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-settings-system' value='0x1000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Settings System</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-auto-exec' value='0x2000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Auto Execute</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-auto-condition' value='0x4000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Auto Condition</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-auto-timing' value='0x8000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Auto Timing</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-auto-scheduler' value='0x40000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Auto Scheduler</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-espnow-enc' value='0x80000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>ESP-NOW Encryption</span>
          </label>
          <div style='font-size:0.75rem;font-weight:600;color:var(--panel-fg);text-transform:uppercase;padding:0.3rem 0 0.1rem;border-bottom:1px solid var(--border);margin-top:0.5rem'>I2C Sensors</div>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-gps' value='0x100000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>GPS (PA1010D)</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-rtc' value='0x200000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>RTC (DS3231)</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-imu' value='0x400000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>IMU (BNO055)</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-thermal' value='0x800000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Thermal (MLX90640)</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-tof' value='0x1000000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>ToF (VL53L4CX)</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-gamepad' value='0x2000000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Gamepad (Seesaw)</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-apds' value='0x4000000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>APDS (APDS9960)</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-presence' value='0x8000000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Presence (STHS34PF80)</span>
          </label>
          <div style='font-size:0.75rem;font-weight:600;color:var(--panel-fg);text-transform:uppercase;padding:0.3rem 0 0.1rem;border-bottom:1px solid var(--border);margin-top:0.5rem'>Sensor Detail</div>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-thermal-frame' value='0x10000000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Thermal Frame</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-thermal-data' value='0x20000000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Thermal Data</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-tof-frame' value='0x40000000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>ToF Frame</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-gamepad-frame' value='0x80000000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Gamepad Frame</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-gamepad-data' value='0x100000000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Gamepad Data</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-imu-frame' value='0x200000000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>IMU Frame</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-imu-data' value='0x400000000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>IMU Data</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-apds-frame' value='0x800000000000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>APDS Frame</span>
          </label>
      </div>
      <div style='margin-top:0.5rem;display:flex;gap:0.5rem'>
        <button class='btn' onclick='selectAllFlags()' style='padding:0.25rem 0.75rem;font-size:0.85rem'>Select All</button>
        <button class='btn' onclick='selectNoFlags()' style='padding:0.25rem 0.75rem;font-size:0.85rem'>Select None</button>
      </div>
    </div>
    
    <div style='margin-top:1rem;display:flex;gap:0.5rem'>
      <button class='btn' onclick='applySystemConfig()'>Apply Configuration</button>
    </div>
    <div id='sys-config-status' style='margin-top:1rem;color:#dc3545'></div>
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

<div class='settings-panel' style='background:var(--panel-bg)'>
  <div id='viewer-pane' style='margin-top:0.75rem'>
    
    <!-- File Selection & Actions -->
    <div style='margin-bottom:1rem'>
      <div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:0.5rem'>
        <label style='color:var(--panel-fg);font-weight:500'>Select Log File:</label>
        <button class='btn' id='btn-switch-logs' style='display:none;padding:0.25rem 0.75rem;font-size:0.85rem' onclick='switchLogSource()'>View System Logs</button>
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
    <div id='viewer-display' style='display:none;background:#12121c;color:#d4d4d4;border-radius:8px;padding:1rem;margin:1rem 0;max-height:600px;overflow-y:auto;font-family:monospace;font-size:0.85rem;line-height:1.5;border:1px solid rgba(255,255,255,0.12)'>
      <div id='viewer-content'>No log loaded</div>
    </div>
    
  </div>
</div>

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
    // Sensor logging initialization
    generateDefaultFilename();
    refreshStatus();
    // System logging initialization
    generateSystemFilename();
    refreshSystemStatus();
    populateLogViewerFileList();
    
    // Show admin log toggle if user is admin (check via settings API which includes user.isAdmin)
    fetch('/api/settings')
      .then(function(r) { return r.json(); })
      .then(function(data) {
        if (data && data.user && data.user.isAdmin === true) {
          document.getElementById('btn-switch-logs').style.display = '';
        }
      })
      .catch(function(e) {
        console.error('[LOGGING] Failed to check user role:', e);
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
function generateDefaultFilename() {
  console.log('[LOGGING] Section 3a: generateDefaultFilename called');
  // Try to get system time from device
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent('time')
  })
  .then(r => r.text())
  .then(text => {
    console.log('[LOGGING] Section 3b: Time response:', text);
    let filename = '/logging_captures/sensors/sensors-';
    
    // Check if we have NTP time (ISO format in response)
    const isoMatch = text.match(/Time:\s*(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})/);
    if (isoMatch) {
      // NTP time available - use ISO format
      console.log('[LOGGING] Section 3c: Using NTP time:', isoMatch[1]);
      const timestamp = isoMatch[1].replace(/:/g, '-');
      filename += timestamp;
    } else {
      // Fallback to browser time
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
    
    const format = document.getElementById('config-format').value;
    filename += (format === 'csv' ? '.csv' : format === 'track' ? '.txt' : '.log');
    console.log('[LOGGING] Section 3e: Generated filename:', filename);
    document.getElementById('config-path').value = filename;
  })
  .catch(e => {
    console.warn('[LOGGING] Section 3f: Failed to get time, using browser time:', e);
    const now = new Date();
    const timestamp = now.getFullYear() + '-' +
      String(now.getMonth() + 1).padStart(2, '0') + '-' +
      String(now.getDate()).padStart(2, '0') + 'T' +
      String(now.getHours()).padStart(2, '0') + '-' +
      String(now.getMinutes()).padStart(2, '0') + '-' +
      String(now.getSeconds()).padStart(2, '0');
    const format = document.getElementById('config-format').value;
    const filename = '/logging_captures/sensors/sensors-' + timestamp + (format === 'csv' ? '.csv' : format === 'track' ? '.txt' : '.log');
    document.getElementById('config-path').value = filename;
  });
}
console.log('[LOGGING] Section 3g: generateDefaultFilename defined');
</script>

<script>
console.log('[LOGGING] Section 4: Status refresh function');
function refreshStatus() {
  console.log('[LOGGING] Section 4a: refreshStatus called');
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent('sensorlog status')
  })
  .then(r => {
    console.log('[LOGGING] Section 4b: Status fetch response:', r.status);
    return r.text();
  })
  .then(text => {
    console.log('[LOGGING] Section 4c: Status response text:', text);
    
    // Check if command is not available
    if (text.includes('Unknown command') || text.includes('not found')) {
      console.log('[LOGGING] Section 4d: sensorlog command not available');
      const statusDot = document.getElementById('status-dot');
      const statusText = document.getElementById('status-text');
      statusDot.style.background = '#ffc107';
      statusText.textContent = 'Not Available';
      statusText.style.color = '#ffc107';
      document.getElementById('btn-start').style.display = 'none';
      document.getElementById('btn-stop').style.display = 'none';
      return;
    }
    
    const isActive = text.includes('logging ACTIVE');
    console.log('[LOGGING] Section 4d: Logging active:', isActive);
    const statusDot = document.getElementById('status-dot');
    const statusText = document.getElementById('status-text');
    const btnStart = document.getElementById('btn-start');
    const btnStop = document.getElementById('btn-stop');
    
    if (isActive) {
      statusDot.style.background = '#28a745';
      statusText.textContent = 'ACTIVE';
      statusText.style.color = '#28a745';
      btnStart.style.display = 'none';
      btnStop.style.display = 'inline-block';
      
      // Parse details
      const fileMatch = text.match(/File:\s*(.+)/);
      const intervalMatch = text.match(/Interval:\s*(\d+)ms/);
      const formatMatch = text.match(/Format:\s*(\w+)/);
      const maxsizeMatch = text.match(/Max size:\s*(\d+)\s*bytes/);
      const rotationsMatch = text.match(/Rotations:\s*(\d+)/);
      const sensorsMatch = text.match(/Sensors:\s*(.+)/);
      const lastwriteMatch = text.match(/Last write:\s*(.+)/);
      
      console.log('[LOGGING] Section 4e: Parsed active status - File:', fileMatch?.[1], 'Interval:', intervalMatch?.[1], 'Format:', formatMatch?.[1], 'Sensors:', sensorsMatch?.[1]);
      
      document.getElementById('detail-file').textContent = fileMatch ? fileMatch[1] : '—';
      document.getElementById('detail-interval').textContent = intervalMatch ? intervalMatch[1] + 'ms' : '—';
      document.getElementById('detail-format').textContent = formatMatch ? formatMatch[1] : '—';
      document.getElementById('detail-maxsize').textContent = maxsizeMatch ? parseInt(maxsizeMatch[1]).toLocaleString() + ' bytes' : '—';
      document.getElementById('detail-rotations').textContent = rotationsMatch ? rotationsMatch[1] : '—';
      document.getElementById('detail-sensors').textContent = sensorsMatch ? sensorsMatch[1].trim() : '—';
      document.getElementById('detail-lastwrite').textContent = lastwriteMatch ? lastwriteMatch[1] : '—';
      
      // Update config fields with active values
      if (fileMatch) document.getElementById('config-path').value = fileMatch[1].trim();
      if (intervalMatch) document.getElementById('config-interval').value = intervalMatch[1];
      if (formatMatch) document.getElementById('config-format').value = formatMatch[1].toLowerCase();
      if (maxsizeMatch) document.getElementById('config-maxsize').value = maxsizeMatch[1];
      if (rotationsMatch) document.getElementById('config-rotations').value = rotationsMatch[1];
    } else {
      statusDot.style.background = '#6c757d';
      statusText.textContent = 'INACTIVE';
      statusText.style.color = 'var(--panel-fg)';
      btnStart.style.display = 'inline-block';
      btnStop.style.display = 'none';
      
      // Parse current settings even when inactive
      const formatMatch = text.match(/Format:\s*(\w+)/);
      const maxsizeMatch = text.match(/Max size:\s*(\d+)\s*bytes/);
      const rotationsMatch = text.match(/Rotations:\s*(\d+)/);
      const sensorsMatch = text.match(/Sensors:\s*(.+)/);
      
      console.log('[LOGGING] Section 4f: Parsed inactive settings - Format:', formatMatch?.[1], 'MaxSize:', maxsizeMatch?.[1], 'Sensors:', sensorsMatch?.[1]);
      
      if (formatMatch) document.getElementById('config-format').value = formatMatch[1].toLowerCase();
      if (maxsizeMatch) document.getElementById('config-maxsize').value = maxsizeMatch[1];
      if (rotationsMatch) document.getElementById('config-rotations').value = rotationsMatch[1];
      
      // Parse and set sensor checkboxes dynamically
      if (sensorsMatch) {
        const sensorStr = sensorsMatch[1].toLowerCase();
        document.querySelectorAll('#sensors-pane input[type=checkbox]').forEach(function(cb) {
          cb.checked = sensorStr.includes(cb.value);
        });
      }
    }
  })
  .catch(e => {
    console.error('[LOGGING] Section 4g: Status refresh error:', e);
    document.getElementById('status-text').textContent = 'Error: ' + e.message;
    document.getElementById('status-text').style.color = '#dc3545';
  });
  
  // Also update auto-start status
  updateAutoStartStatus();
}
console.log('[LOGGING] Section 4h: refreshStatus defined');

function updateAutoStartStatus() {
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent('sensorlog status')
  })
  .then(r => r.text())
  .then(text => {
    const autostartMatch = text.match(/Auto-start:\s*(ON|OFF)/i);
    const statusSpan = document.getElementById('autostart-status');
    if (autostartMatch) {
      const isOn = autostartMatch[1].toUpperCase() === 'ON';
      statusSpan.textContent = isOn ? 'ON' : 'OFF';
      statusSpan.style.color = isOn ? '#28a745' : 'var(--panel-fg)';
    } else {
      statusSpan.textContent = '?';
    }
  })
  .catch(e => console.error('[LOGGING] Auto-start status error:', e));
}

function toggleAutoStart() {
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent('sensorlog autostart')
  })
  .then(r => r.text())
  .then(text => {
    alert(text);
    updateAutoStartStatus();
  })
  .catch(e => alert('Error toggling auto-start: ' + e.message));
}
</script>

<script>
</script>

<script>
console.log('[LOGGING] Section 6: Start logging function');
function startLogging() {
  console.log('[LOGGING] Section 6a: startLogging called');
  const path = document.getElementById('config-path').value;
  const interval = document.getElementById('config-interval').value;
  console.log('[LOGGING] Section 6b: Start params - Path:', path, 'Interval:', interval);
  
  if (!path || !path.startsWith('/')) {
    alert('Error: File path must start with / (e.g., /logging_captures/sensors.csv)');
    return;
  }
  
  if (!confirm('Start logging to ' + path + ' every ' + interval + 'ms?')) {
    console.log('[LOGGING] Section 6c: Start cancelled by user');
    return;
  }
  
  const cmd = 'sensorlog start ' + path + ' ' + interval;
  console.log('[LOGGING] Section 6d: Executing command:', cmd);
  
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent(cmd)
  })
  .then(r => {
    console.log('[LOGGING] Section 6e: Start command response:', r.status);
    return r.text();
  })
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
function stopLogging() {
  console.log('[LOGGING] Section 7a: stopLogging called');
  if (!confirm('Stop sensor logging?')) {
    console.log('[LOGGING] Section 7b: Stop cancelled by user');
    return;
  }
  
  console.log('[LOGGING] Section 7c: Executing stop command');
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent('sensorlog stop')
  })
  .then(r => {
    console.log('[LOGGING] Section 7d: Stop command response:', r.status);
    return r.text();
  })
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
  const format = document.getElementById('config-format').value;
  const maxsize = document.getElementById('config-maxsize').value;
  const rotations = document.getElementById('config-rotations').value;
  console.log('[LOGGING] Section 8b: Config values - Format:', format, 'MaxSize:', maxsize, 'Rotations:', rotations);
  
  // Build sensor list dynamically from compiled-in checkboxes
  const sensors = [];
  document.querySelectorAll('#sensors-pane input[type=checkbox]').forEach(function(cb) {
    if (cb.checked) sensors.push(cb.value);
  });
  const sensorList = sensors.length > 0 ? sensors.join(',') : 'none';
  console.log('[LOGGING] Section 8c: Selected sensors:', sensorList);
  
  document.getElementById('config-status').textContent = 'Applying...';
  document.getElementById('config-status').style.color = '#007bff';
  
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
      document.getElementById('config-status').textContent = 'Configuration applied!';
      document.getElementById('config-status').style.color = '#28a745';
      setTimeout(() => {
        document.getElementById('config-status').textContent = '';
        refreshStatus();
      }, 2000);
      return;
    }
    
    console.log('[LOGGING] Section 8f: Executing command', index + 1, 'of', commands.length, ':', commands[index]);
    
    fetch('/api/cli', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'cmd=' + encodeURIComponent(commands[index])
    })
    .then(r => r.text())
    .then(text => {
      console.log('[LOGGING] Section 8g: Command', index + 1, 'result:', text);
      results.push(text);
      runCommand(index + 1);
    })
    .catch(e => {
      console.error('[LOGGING] Section 8h: Command', index + 1, 'error:', e);
      document.getElementById('config-status').textContent = 'Error: ' + e.message;
      document.getElementById('config-status').style.color = '#dc3545';
    });
  }
  
  runCommand(0);
}
console.log('[LOGGING] Section 8i: applyConfig defined');
</script>

<script>
</script>

<script>
console.log('[LOGGING] Section 10: Sensor selection helpers');
function selectAllSensors() {
  console.log('[LOGGING] Section 10a: selectAllSensors called');
  document.querySelectorAll('#sensors-pane input[type=checkbox]').forEach(function(cb) { cb.checked = true; });
  console.log('[LOGGING] Section 10b: All sensors selected');
}

function selectNoSensors() {
  console.log('[LOGGING] Section 10c: selectNoSensors called');
  document.querySelectorAll('#sensors-pane input[type=checkbox]').forEach(function(cb) { cb.checked = false; });
  console.log('[LOGGING] Section 10d: All sensors deselected');
}
console.log('[LOGGING] Section 10e: Sensor selection helpers defined');
</script>

<script>
console.log('[LOGGING] Section 11: Toggle functions');
function togglePane(paneId, btnId) {
  console.log('[LOGGING] Section 11a: togglePane called for:', paneId);
  const pane = document.getElementById(paneId);
  const btn = document.getElementById(btnId);
  if (pane.style.display === 'none') {
    pane.style.display = 'block';
    btn.textContent = 'Collapse';
    console.log('[LOGGING] Section 11b: Pane', paneId, 'expanded');
  } else {
    pane.style.display = 'none';
    btn.textContent = 'Expand';
    console.log('[LOGGING] Section 11c: Pane', paneId, 'collapsed');
  }
}

function toggleConfigPane(paneId, btnId) {
  console.log('[LOGGING] Section 11d: toggleConfigPane called for:', paneId);
  const pane = document.getElementById(paneId);
  const btn = document.getElementById(btnId);
  if (pane.style.display === 'none') {
    pane.style.display = 'block';
    btn.textContent = 'Collapse';
    console.log('[LOGGING] Section 11e: Config pane', paneId, 'expanded');
  } else {
    pane.style.display = 'none';
    btn.textContent = 'Expand';
    console.log('[LOGGING] Section 11f: Config pane', paneId, 'collapsed');
  }
}
console.log('[LOGGING] Section 11g: Toggle functions defined');
</script>

<script>
console.log('[LOGGING] Section 12: Page initialization');
// No tab switching needed - using collapsible sections like other pages
</script>

<script>
console.log('[LOGGING] Section 13: System logging functions');

function generateSystemFilename() {
  console.log('[LOGGING] generateSystemFilename called');
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent('time')
  })
  .then(r => r.text())
  .then(text => {
    console.log('[LOGGING] System time response:', text);
    let filename = '/logging_captures/system-';
    
    // Match new format: "Time: 2024-01-22T06:18:00 (NTP synced)"
    const isoMatch = text.match(/Time:\s*(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})/);
    if (isoMatch) {
      const timestamp = isoMatch[1].replace(/:/g, '-');
      filename += timestamp;
    } else {
      // Fallback to browser time
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
    document.getElementById('sys-config-path').value = filename;
  })
  .catch(e => {
    console.warn('[LOGGING] Failed to get time, using browser time:', e);
    const now = new Date();
    const timestamp = now.getFullYear() + '-' +
      String(now.getMonth() + 1).padStart(2, '0') + '-' +
      String(now.getDate()).padStart(2, '0') + 'T' +
      String(now.getHours()).padStart(2, '0') + '-' +
      String(now.getMinutes()).padStart(2, '0') + '-' +
      String(now.getSeconds()).padStart(2, '0');
    const filename = '/logging_captures/system-' + timestamp + '.log';
    document.getElementById('sys-config-path').value = filename;
  });
}

function refreshSystemStatus() {
  console.log('[LOGGING] refreshSystemStatus called');
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent('log status')
  })
  .then(r => r.text())
  .then(text => {
    console.log('[LOGGING] System status response:', text);
    
    // Check if command is not available
    if (text.includes('Unknown command') || text.includes('not found')) {
      console.log('[LOGGING] log command not available');
      const statusDot = document.getElementById('sys-status-dot');
      const statusText = document.getElementById('sys-status-text');
      statusDot.style.background = '#ffc107';
      statusText.textContent = 'Not Available';
      statusText.style.color = '#ffc107';
      document.getElementById('sys-btn-start').style.display = 'none';
      document.getElementById('sys-btn-stop').style.display = 'none';
      return;
    }
    
    const isActive = text.includes('logging ACTIVE');
    const statusDot = document.getElementById('sys-status-dot');
    const statusText = document.getElementById('sys-status-text');
    const btnStart = document.getElementById('sys-btn-start');
    const btnStop = document.getElementById('sys-btn-stop');
    
    if (isActive) {
      statusDot.style.background = '#28a745';
      statusText.textContent = 'ACTIVE';
      statusText.style.color = '#28a745';
      btnStart.style.display = 'none';
      btnStop.style.display = 'inline-block';
      
      const fileMatch = text.match(/File:\s*(.+)/);
      const lastwriteMatch = text.match(/Last write:\s*(\d+)s ago/);
      const flagsMatch = text.match(/Output flags:\s*0x([0-9A-Fa-f]+)/);
      
      document.getElementById('sys-detail-file').textContent = fileMatch ? fileMatch[1].trim() : '—';
      document.getElementById('sys-detail-lastwrite').textContent = lastwriteMatch ? lastwriteMatch[1] + 's ago' : '—';
      document.getElementById('sys-detail-flags').textContent = flagsMatch ? '0x' + flagsMatch[1] : '—';
      
      if (fileMatch) document.getElementById('sys-config-path').value = fileMatch[1].trim();
    } else {
      statusDot.style.background = '#6c757d';
      statusText.textContent = 'INACTIVE';
      statusText.style.color = 'var(--panel-fg)';
      btnStart.style.display = 'inline-block';
      btnStop.style.display = 'none';
      
      document.getElementById('sys-detail-file').textContent = '—';
      document.getElementById('sys-detail-lastwrite').textContent = '—';
      document.getElementById('sys-detail-flags').textContent = '—';
    }
  })
  .catch(e => {
    console.error('[LOGGING] System status refresh error:', e);
    document.getElementById('sys-status-text').textContent = 'Error: ' + e.message;
    document.getElementById('sys-status-text').style.color = '#dc3545';
  });
  
  // Also update auto-start status
  updateSystemAutoStartStatus();
}

function startSystemLogging() {
  console.log('[LOGGING] startSystemLogging called');
  const filepath = document.getElementById('sys-config-path').value;
  if (!filepath) {
    alert('Please specify a log file path');
    return;
  }
  
  // Get selected debug flags
  const flags = [];
  document.querySelectorAll('#sys-flags-pane input[type="checkbox"]:checked').forEach(cb => {
    flags.push(cb.value);
  });
  
  // Calculate combined flag value (BigInt required for flags > bit 31)
  let flagValue = BigInt(0);
  flags.forEach(f => {
    flagValue |= BigInt(f);
  });
  
  // Get category tags setting
  const categoryTags = document.getElementById('sys-config-tags').checked ? 1 : 0;
  
  // Build command with optional flags and tags
  let cmd = 'log start ' + filepath;
  if (flagValue > BigInt(0)) {
    cmd += ' flags=0x' + flagValue.toString(16);
  }
  cmd += ' tags=' + categoryTags;
  
  console.log('[LOGGING] Executing:', cmd);
  
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent(cmd)
  })
  .then(r => r.text())
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

function stopSystemLogging() {
  console.log('[LOGGING] stopSystemLogging called');
  if (!confirm('Stop system logging?')) return;
  
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent('log stop')
  })
  .then(r => r.text())
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
  document.querySelectorAll('#sys-flags-pane input[type="checkbox"]:checked').forEach(cb => {
    flags.push(cb.value);
  });
  
  if (flags.length === 0) {
    document.getElementById('sys-config-status').textContent = 'Warning: No debug categories selected. No messages will be logged.';
    document.getElementById('sys-config-status').style.color = '#ffc107';
    return;
  }
  
  // Calculate combined flag value (BigInt required for flags > bit 31)
  let flagValue = BigInt(0);
  flags.forEach(f => {
    flagValue |= BigInt(f);
  });
  
  console.log('[LOGGING] Setting debug flags to:', '0x' + flagValue.toString(16));
  
  // Store for use when starting logging
  document.getElementById('sys-config-status').textContent = 'Configuration saved. Click "Start System Logging" to apply with these flags.';
  document.getElementById('sys-config-status').style.color = '#28a745';
}

function selectAllFlags() {
  document.querySelectorAll('#sys-flags-pane input[type="checkbox"]').forEach(cb => cb.checked = true);
}

function selectNoFlags() {
  document.querySelectorAll('#sys-flags-pane input[type="checkbox"]').forEach(cb => cb.checked = false);
}

function updateSystemAutoStartStatus() {
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent('log status')
  })
  .then(r => r.text())
  .then(text => {
    const autostartMatch = text.match(/Auto-start:\s*(ON|OFF)/i);
    const statusSpan = document.getElementById('sys-autostart-status');
    if (autostartMatch) {
      const isOn = autostartMatch[1].toUpperCase() === 'ON';
      statusSpan.textContent = isOn ? 'ON' : 'OFF';
      statusSpan.style.color = isOn ? '#28a745' : 'var(--panel-fg)';
    } else {
      statusSpan.textContent = '?';
    }
  })
  .catch(e => console.error('[LOGGING] System auto-start status error:', e));
}

function toggleSystemAutoStart() {
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent('log autostart')
  })
  .then(r => r.text())
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
  
  // Use streaming fetch with mode=raw to get plain text
  fetch('/api/files/view?name=' + encodeURIComponent(filepath) + '&mode=raw')
    .then(r => {
      if (!r.ok) throw new Error('HTTP ' + r.status + ': ' + r.statusText);
      return r.text();
    })
    .then(text => {
      parseLogFile(text);
      document.getElementById('viewer-filters').style.display = 'block';
      document.getElementById('viewer-display').style.display = 'block';
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
    
    // Format 1: Debug logs - [timestamp] [CATEGORY] message
    // Allow letters, digits, underscores, hyphens so [ESP-NOW], [CMD_SYS] etc. all match
    let match = line.match(/^\[(\d+)\]\s*\[([A-Z][A-Z0-9_-]*)\]\s*(.*)$/);
    if (match) {
      const [, timestamp, category, message] = match;
      const cat = category.replace(/-/g, '_');  // normalise ESP-NOW → ESP_NOW for color lookup
      // Extract level from leading [LEVEL] prefix in message, e.g. "[ERROR][WEB] ..."
      const levelMatch = message.match(/^\[(ERROR|WARN|INFO|DEBUG)\]\[([A-Z][A-Z0-9_-]*)\]\s*(.*)$/);
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
        const levelCatTag = msg.match(/^\[(ERROR|WARN|INFO|DEBUG)\]\[([A-Z][A-Z0-9_-]*)\]\s*(.*)/);
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
  const categoryFilter = document.getElementById('viewer-category-filter');
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
  const categoryFilter = document.getElementById('viewer-category-filter').value;
  const levelFilter = document.getElementById('viewer-level-filter').value;
  const searchText = document.getElementById('viewer-search').value.toLowerCase();
  
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
  const content = document.getElementById('viewer-content');
  
  if (gFilteredLines.length === 0) {
    content.innerHTML = '<div style="color:#888">No matching log entries</div>';
    return;
  }
  
  const levelColors = { 'ERROR': '#f44747', 'WARN': '#ffc107', 'INFO': '#6a9955', 'DEBUG': '#569cd6' };
  let html = '';
  gFilteredLines.forEach(line => {
    const categoryColor = getCategoryColor(line.category);
    const levelColor = levelColors[line.level] || '#888';
    const timestampStr = line.timestamp.toString();
    
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
  const colors = {
    // Core system
    'AUTH': '#f48771',       'SESSION': '#f48771',
    'HTTP': '#4ec9b0',       'WEB': '#4ec9b0',
    'SSE': '#4fc1ff',
    'CLI': '#dcdcaa',        'CMD': '#dcdcaa',        'CMD_FLOW': '#dcdcaa',
    'SYSTEM': '#569cd6',     'SYS': '#569cd6',        'BOOT': '#569cd6',
    'STORAGE': '#9cdcfe',
    'WIFI': '#ce9178',
    'SECURITY': '#f48771',
    'USERS': '#f48771',      'USER': '#f48771',
    'LOGGER': '#608b4e',     'LOG': '#608b4e',
    // Sensors
    'SENSORS': '#c586c0',    'SENSORS_FRAME': '#c586c0', 'SENSORS_DATA': '#c586c0',
    'GPS': '#c586c0',        'IMU': '#c586c0',         'THERMAL': '#c586c0',
    'TOF': '#c586c0',        'GAMEPAD': '#c586c0',     'APDS': '#c586c0',
    'PRESENCE': '#c586c0',   'FMRADIO': '#c586c0',     'RTC': '#c586c0',
    'CAMERA': '#c586c0',     'MIC': '#c586c0',
    // Performance / memory
    'PERF': '#b5cea8',       'MEMORY': '#b5cea8',      'MEM': '#b5cea8',
    'HEAP': '#b5cea8',       'STACK': '#b5cea8',
    // Automations
    'AUTO': '#d7ba7d',       'AUTO_EXEC': '#d7ba7d',   'AUTO_COND': '#d7ba7d',
    'AUTO_SCHED': '#d7ba7d', 'AUTO_TIME': '#d7ba7d',
    // ESP-NOW (hyphens normalised to underscores by parser)
    'ESPNOW': '#d16969',     'ESP_NOW': '#d16969',
    'ESPNOW_CORE': '#d16969','ESPNOW_MESH': '#d16969',
    'ESPNOW_ROUTER': '#d16969','ESPNOW_TOPO': '#d16969',
    'ESPNOW_STREAM': '#d16969','ESPNOW_ENCRYPTION': '#d16969',
    // Severity prefixes (from ERROR/WARN/INFO macros)
    'ERROR': '#f44747',      'WARN': '#ffc107',        'INFO': '#6a9955',
    // Settings / command system
    'SETTINGS_SYS': '#9cdcfe', 'CMD_SYS': '#dcdcaa',
    // G2 glasses, Bluetooth
    'G2': '#9cdcfe',         'BT': '#4fc1ff',
  };
  return colors[category] || '#888';
}

function escapeHtml(text) {
  const div = document.createElement('div');
  div.textContent = text;
  return div.innerHTML;
}

function updateViewerStats() {
  const stats = document.getElementById('viewer-stats');
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
    document.getElementById('log-viewer-file-explorer').innerHTML = 
      '<div style="padding:1rem;color:#c00;border:1px solid #c00;border-radius:4px">File explorer component not loaded. Please refresh the page.</div>';
  }
}

function switchLogSource() {
  // Toggle between /logging_captures and /system/sys_logs
  if (currentLogSource === '/logging_captures') {
    currentLogSource = '/system/sys_logs';
    document.getElementById('btn-switch-logs').textContent = 'View User Logs';
  } else {
    currentLogSource = '/logging_captures';
    document.getElementById('btn-switch-logs').textContent = 'View System Logs';
  }
  
  // Reload file explorer with new source
  populateLogViewerFileList();
  
  // Clear current log viewer
  document.getElementById('viewer-content').innerHTML = '<div style="padding:1rem;color:var(--panel-fg)">Select a log file to view</div>';
}

console.log('[LOGGING] Section 14a: Log viewer functions defined');
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
