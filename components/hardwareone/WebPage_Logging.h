#ifndef WEBPAGE_LOGGING_H
#define WEBPAGE_LOGGING_H

#include "WebServer_Utils.h"  // For getFileBrowserScript

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
      <div style='color:var(--muted);font-size:0.9rem'>Configure sensor data collection, file rotation, and output formats.</div>
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
    <button class='btn' onclick='refreshStatus()'>Refresh Status</button>
  </div>
</div>

<!-- Configuration Section -->
<div class='settings-panel' style='background:var(--panel-bg)'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div>
      <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Configuration</div>
      <div style='color:var(--muted);font-size:0.9rem'>Configure logging parameters, file management, and sensor selection.</div>
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
          <small style='color:#6c757d'>Auto-generated with timestamp (NTP or uptime)</small>
        </label>
        <label style='display:block;margin-bottom:1rem'>
          <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>Interval (ms):</div>
          <input id='config-interval' type='number' value='5000' min='100' max='3600000' class='form-input' style='width:100%'>
          <small style='color:#6c757d'>Min: 100ms, Max: 1 hour (3600000ms)</small>
        </label>
        <label style='display:block;margin-bottom:1rem'>
          <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>Format:</div>
          <select id='config-format' class='form-input' style='width:100%'>
            <option value='text'>Text (Human-readable)</option>
            <option value='csv'>CSV (Structured data)</option>
          </select>
        </label>
        <label style='display:block;margin-bottom:1rem'>
          <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>Max File Size (bytes):</div>
          <input id='config-maxsize' type='number' value='256000' min='10240' max='10485760' class='form-input' style='width:100%'>
          <small style='color:#6c757d'>Min: 10KB, Max: 10MB</small>
        </label>
        <label style='display:block'>
          <div style='margin-bottom:0.25rem;color:var(--panel-fg)'>Rotations (old logs to keep):</div>
          <input id='config-rotations' type='number' value='3' min='0' max='9' class='form-input' style='width:100%'>
          <small style='color:#6c757d'>0 = delete old logs, 1-9 = keep N old files</small>
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
        <div class='form-input' style='background:var(--crumb-bg)'>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-thermal' value='thermal' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Thermal (temperature array)</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-tof' value='tof' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>ToF (distance sensors)</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-imu' value='imu' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>IMU (orientation, accel, gyro, temp)</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-gamepad' value='gamepad' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Gamepad (buttons, joystick)</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-apds' value='apds' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>APDS (color, proximity, gesture)</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='sensor-gps' value='gps' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>GPS (position, speed, altitude)</span>
          </label>
        </div>
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

<!-- Log Files Section (Sensor) -->
<div class='settings-panel' style='background:var(--panel-bg)'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div>
      <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Sensor Log Files</div>
      <div style='color:var(--muted);font-size:0.9rem'>View and download sensor log files from /logs/</div>
    </div>
    <button class='btn' id='btn-files-toggle' onclick="togglePane('files-pane','btn-files-toggle')">Expand</button>
  </div>
  <div id='files-pane' style='display:none;margin-top:0.75rem'>
    <div id='log-files' style='color:var(--panel-fg)'>Loading...</div>
  </div>
</div>

  </div>
</div>

<!-- System Logging Section -->
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div>
      <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>System Logging</div>
      <div style='color:var(--muted);font-size:0.9rem'>Debug logging with category filtering and configurable output destinations.</div>
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
    <button class='btn' onclick='refreshSystemStatus()'>Refresh Status</button>
  </div>
</div>

<!-- System Configuration Section -->
<div class='settings-panel' style='background:var(--panel-bg)'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div>
      <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Configuration</div>
      <div style='color:var(--muted);font-size:0.9rem'>Configure log file path and debug message categories.</div>
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
        <small style='color:#6c757d'>Auto-generated with timestamp (NTP or uptime)</small>
      </label>
      <label style='display:flex;align-items:center;gap:0.5rem;cursor:pointer'>
        <input type='checkbox' id='sys-config-tags' checked style='margin:0;padding:0;width:16px;height:16px'>
        <span style='font-size:0.95em;color:var(--panel-fg)'>Include category tags in log output (e.g., [AUTH], [HTTP])</span>
      </label>
    </div>
    
    <!-- Debug Flags Section -->
    <div class='settings-panel'>
      <div style='display:flex;align-items:center;justify-content:space-between'>
        <div style='font-weight:bold;color:var(--panel-fg)'>Debug Message Categories</div>
        <button class='btn' id='btn-sys-flags-toggle' onclick="toggleConfigPane('sys-flags-pane','btn-sys-flags-toggle')" style='padding:0.25rem 0.75rem;font-size:0.85rem'>Expand</button>
      </div>
      <div id='sys-flags-pane' style='display:none;margin-top:0.75rem'>
        <div style='padding:0.5rem;background:#f8f9fa;border:1px solid #ccc;border-radius:4px;max-height:300px;overflow-y:auto'>
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
            <input type='checkbox' id='flag-sensors-frame' value='0x0010' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Sensor Frame Processing</span>
          </label>
          <label style='display:flex;align-items:center;gap:0.25rem;margin:0.5rem 0;cursor:pointer'>
            <input type='checkbox' id='flag-sensors-data' value='0x0020' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Sensor Data</span>
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
            <input type='checkbox' id='flag-storage' value='0x0100' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Storage Operations</span>
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
            <input type='checkbox' id='flag-security' value='0x8000' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'>
            <span style='font-size:0.9em;color:var(--panel-fg)'>Security</span>
          </label>
        </div>
        <div style='margin-top:0.5rem;display:flex;gap:0.5rem'>
          <button class='btn' onclick='selectAllFlags()' style='padding:0.25rem 0.75rem;font-size:0.85rem'>Select All</button>
          <button class='btn' onclick='selectNoFlags()' style='padding:0.25rem 0.75rem;font-size:0.85rem'>Select None</button>
        </div>
      </div>
    </div>
    
    <div style='margin-top:1rem;display:flex;gap:0.5rem'>
      <button class='btn' onclick='applySystemConfig()'>Apply Configuration</button>
    </div>
    <div id='sys-config-status' style='margin-top:1rem;color:#dc3545'></div>
  </div>
</div>

<!-- System Log Files Section -->
<div class='settings-panel' style='background:var(--panel-bg)'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div>
      <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>System Log Files</div>
      <div style='color:var(--muted);font-size:0.9rem'>View and download system log files from /logs/</div>
    </div>
    <button class='btn' id='btn-sys-files-toggle' onclick="togglePane('sys-files-pane','btn-sys-files-toggle')">Expand</button>
  </div>
  <div id='sys-files-pane' style='display:none;margin-top:0.75rem'>
    <div id='sys-log-files' style='color:var(--panel-fg)'>Loading...</div>
  </div>
</div>

  </div>
</div>

<!-- Log Viewer & Parser Section (Third Top-Level Segment) -->
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div>
      <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Log Viewer & Parser</div>
      <div style='color:var(--muted);font-size:0.9rem'>Load and filter log files with category-based filtering</div>
    </div>
    <button class='btn' id='btn-viewer-section-toggle' onclick="togglePane('content-viewer','btn-viewer-section-toggle')">Expand</button>
  </div>
  <div id='content-viewer' style='display:none;margin-top:0.75rem'>

<div class='settings-panel' style='background:var(--panel-bg)'>
  <div id='viewer-pane' style='margin-top:0.75rem'>
    
    <!-- File Selection -->
    <div style='margin-bottom:1rem'>
      <label style='display:block;margin-bottom:0.5rem;color:var(--panel-fg);font-weight:500'>Select Log File:</label>
      <div id='log-viewer-file-explorer' style='margin-top:0.5rem'></div>
    </div>
    
    <!-- Filter Controls -->
    <div id='viewer-filters' style='display:none;background:var(--panel-bg);border-radius:8px;padding:1rem;margin:1rem 0;border:1px solid var(--border)'>
      <div style='font-weight:bold;color:var(--panel-fg);margin-bottom:0.75rem'>Filters</div>
      <div style='display:grid;grid-template-columns:1fr 1fr;gap:1rem'>
        <div>
          <label style='display:block;margin-bottom:0.5rem;color:var(--panel-fg)'>Category:</label>
          <select id='viewer-category-filter' onchange='applyLogFilters()' class='form-input' style='width:100%'>
            <option value=''>All Categories</option>
          </select>
        </div>
        <div>
          <label style='display:block;margin-bottom:0.5rem;color:var(--panel-fg)'>Search Text:</label>
          <input id='viewer-search' type='text' oninput='applyLogFilters()' placeholder='Search log content...' class='form-input' style='width:100%'>
        </div>
      </div>
      <div style='margin-top:0.75rem;color:var(--muted);font-size:0.9rem'>
        <span id='viewer-stats'>No file loaded</span>
      </div>
    </div>
    
    <!-- Log Display -->
    <div id='viewer-display' style='display:none;background:#1e1e1e;color:#d4d4d4;border-radius:8px;padding:1rem;margin:1rem 0;max-height:600px;overflow-y:auto;font-family:monospace;font-size:0.85rem;line-height:1.5'>
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
    refreshLogFiles();
    // System logging initialization
    generateSystemFilename();
    refreshSystemStatus();
    refreshSystemLogFiles();
    populateLogViewerFileList();
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
    let filename = '/logs/sensors-';
    
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
    filename += (format === 'csv' ? '.csv' : '.log');
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
    const filename = '/logs/sensors-' + timestamp + (format === 'csv' ? '.csv' : '.log');
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
      statusText.style.color = '#6c757d';
      btnStart.style.display = 'inline-block';
      btnStop.style.display = 'none';
      
      // Parse current settings even when inactive
      const formatMatch = text.match(/Current format:\s*(\w+)/);
      const maxsizeMatch = text.match(/Max size:\s*(\d+)\s*bytes/);
      const rotationsMatch = text.match(/Rotations:\s*(\d+)/);
      const sensorsMatch = text.match(/Sensors:\s*(.+)/);
      
      console.log('[LOGGING] Section 4f: Parsed inactive settings - Format:', formatMatch?.[1], 'MaxSize:', maxsizeMatch?.[1], 'Sensors:', sensorsMatch?.[1]);
      
      if (formatMatch) document.getElementById('config-format').value = formatMatch[1].toLowerCase();
      if (maxsizeMatch) document.getElementById('config-maxsize').value = maxsizeMatch[1];
      if (rotationsMatch) document.getElementById('config-rotations').value = rotationsMatch[1];
      
      // Parse and set sensor checkboxes
      if (sensorsMatch) {
        const sensors = sensorsMatch[1].toLowerCase();
        document.getElementById('sensor-thermal').checked = sensors.includes('thermal');
        document.getElementById('sensor-tof').checked = sensors.includes('tof');
        document.getElementById('sensor-imu').checked = sensors.includes('imu');
        document.getElementById('sensor-gamepad').checked = sensors.includes('gamepad');
        document.getElementById('sensor-apds').checked = sensors.includes('apds');
      }
    }
  })
  .catch(e => {
    console.error('[LOGGING] Section 4g: Status refresh error:', e);
    document.getElementById('status-text').textContent = 'Error: ' + e.message;
    document.getElementById('status-text').style.color = '#dc3545';
  });
}
console.log('[LOGGING] Section 4h: refreshStatus defined');
</script>

<script>
console.log('[LOGGING] Section 5: Log files refresh function');
function refreshLogFiles() {
  console.log('[LOGGING] Section 5a: refreshLogFiles called');
  fetch('/api/files/list?path=/logs')
  .then(r => {
    console.log('[LOGGING] Section 5b: Files list response:', r.status);
    return r.json();
  })
  .then(d => {
    console.log('[LOGGING] Section 5c: Files list data:', d);
    if (d.success && d.files) {
      const logFiles = d.files.filter(f => f.type === 'file' && (f.name.startsWith('sensors') || f.name.includes('.log') || f.name.endsWith('.csv')));
      console.log('[LOGGING] Section 5d: Found', logFiles.length, 'log files');
      
      if (logFiles.length === 0) {
        document.getElementById('log-files').innerHTML = '<p style="color:#6c757d">No log files found in /logs/</p>';
        return;
      }
      
      let html = '<table style="width:100%;border-collapse:collapse">';
      html += '<tr style="background:#e9ecef"><th style="padding:0.5rem;text-align:left">File</th><th style="padding:0.5rem;text-align:left">Size</th><th style="padding:0.5rem;text-align:left">Actions</th></tr>';
      
      logFiles.forEach(f => {
        const fullPath = '/logs/' + f.name;
        html += '<tr style="border-bottom:1px solid #ddd">';
        html += '<td style="padding:0.5rem;font-family:monospace;font-size:0.9rem">' + f.name + '</td>';
        html += '<td style="padding:0.5rem">' + (f.size || '0 bytes') + '</td>';
        html += '<td style="padding:0.5rem">';
        html += '<button onclick="downloadLog(\'' + fullPath + '\')" class="btn" style="margin-right:0.5rem">Download</button>';
        html += '<button onclick="viewLog(\'' + fullPath + '\')" class="btn">View</button>';
        html += '</td></tr>';
      });
      
      html += '</table>';
      document.getElementById('log-files').innerHTML = html;
    } else {
      document.getElementById('log-files').innerHTML = '<p style="color:#6c757d">Unable to load log files</p>';
    }
  })
  .catch(e => {
    console.error('[LOGGING] Section 5e: Files list error:', e);
    document.getElementById('log-files').innerHTML = '<p style="color:#dc3545">Error: ' + e.message + '</p>';
  });
}
console.log('[LOGGING] Section 5f: refreshLogFiles defined');
</script>

<script>
console.log('[LOGGING] Section 6: Start logging function');
function startLogging() {
  console.log('[LOGGING] Section 6a: startLogging called');
  const path = document.getElementById('config-path').value;
  const interval = document.getElementById('config-interval').value;
  console.log('[LOGGING] Section 6b: Start params - Path:', path, 'Interval:', interval);
  
  if (!path || !path.startsWith('/')) {
    alert('Error: File path must start with / (e.g., /logs/sensors.csv)');
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
      refreshLogFiles();
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
  
  // Build sensor list
  const sensors = [];
  if (document.getElementById('sensor-thermal').checked) sensors.push('thermal');
  if (document.getElementById('sensor-tof').checked) sensors.push('tof');
  if (document.getElementById('sensor-imu').checked) sensors.push('imu');
  if (document.getElementById('sensor-gamepad').checked) sensors.push('gamepad');
  if (document.getElementById('sensor-apds').checked) sensors.push('apds');
  
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
console.log('[LOGGING] Section 9: File operations');
function downloadLog(path) {
  console.log('[LOGGING] Section 9a: downloadLog called for:', path);
  window.open('/api/files/view?name=' + encodeURIComponent(path), '_blank');
}

function viewLog(path) {
  console.log('[LOGGING] Section 9b: viewLog called for:', path);
  window.open('/api/files/view?name=' + encodeURIComponent(path), '_blank');
}
console.log('[LOGGING] Section 9c: File operations defined');
</script>

<script>
console.log('[LOGGING] Section 10: Sensor selection helpers');
function selectAllSensors() {
  console.log('[LOGGING] Section 10a: selectAllSensors called');
  document.getElementById('sensor-thermal').checked = true;
  document.getElementById('sensor-tof').checked = true;
  document.getElementById('sensor-imu').checked = true;
  document.getElementById('sensor-gamepad').checked = true;
  document.getElementById('sensor-apds').checked = true;
  console.log('[LOGGING] Section 10b: All sensors selected');
}

function selectNoSensors() {
  console.log('[LOGGING] Section 10c: selectNoSensors called');
  document.getElementById('sensor-thermal').checked = false;
  document.getElementById('sensor-tof').checked = false;
  document.getElementById('sensor-imu').checked = false;
  document.getElementById('sensor-gamepad').checked = false;
  document.getElementById('sensor-apds').checked = false;
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
    let filename = '/logs/system-';
    
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
    const filename = '/logs/system-' + timestamp + '.log';
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
      statusText.style.color = '#6c757d';
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
  
  // Calculate combined flag value
  let flagValue = 0;
  flags.forEach(f => {
    flagValue |= parseInt(f);
  });
  
  // Get category tags setting
  const categoryTags = document.getElementById('sys-config-tags').checked ? 1 : 0;
  
  // Build command with optional flags and tags
  let cmd = 'log start ' + filepath;
  if (flagValue > 0) {
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
      refreshSystemLogFiles();
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
  
  // Calculate combined flag value
  let flagValue = 0;
  flags.forEach(f => {
    flagValue |= parseInt(f);
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

function refreshSystemLogFiles() {
  console.log('[LOGGING] refreshSystemLogFiles called');
  fetch('/api/files/list?path=/logs')
  .then(r => r.json())
  .then(data => {
    const container = document.getElementById('sys-log-files');
    if (!data.files || data.files.length === 0) {
      container.innerHTML = '<div style="color:#6c757d">No system log files found</div>';
      return;
    }
    
    // Filter for system log files
    const systemLogs = data.files.filter(f => f.name.startsWith('system-') && f.name.endsWith('.log'));
    
    if (systemLogs.length === 0) {
      container.innerHTML = '<div style="color:#6c757d">No system log files found</div>';
      return;
    }
    
    let html = '<div style="display:grid;gap:0.5rem">';
    systemLogs.forEach(file => {
      const sizeKB = (file.size !== undefined && !isNaN(file.size)) ? (file.size / 1024).toFixed(2) : '?';
      html += '<div style="display:flex;justify-content:space-between;align-items:center;padding:0.5rem;background:#fff;border:1px solid #dee2e6;border-radius:4px">';
      html += '<div style="font-family:monospace;color:var(--panel-fg)">' + file.name + ' (' + sizeKB + ' KB)</div>';
      html += '<div style="display:flex;gap:0.5rem">';
      html += '<a href="/api/files/view?name=' + encodeURIComponent('/logs/' + file.name) + '" class="btn" style="padding:0.25rem 0.75rem;font-size:0.85rem;text-decoration:none" target="_blank">View</a>';
      html += '<a href="/api/files/view?name=' + encodeURIComponent('/logs/' + file.name) + '&mode=raw" class="btn" style="padding:0.25rem 0.75rem;font-size:0.85rem;text-decoration:none" download>Download</a>';
      html += '</div></div>';
    });
    html += '</div>';
    container.innerHTML = html;
  })
  .catch(e => {
    console.error('[LOGGING] System log files refresh error:', e);
    document.getElementById('sys-log-files').innerHTML = '<div style="color:#dc3545">Error loading files: ' + e.message + '</div>';
  });
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
  const lines = text.split('\n').filter(l => l.trim().length > 0);
  gLogLines = [];
  const categories = new Set();
  
  lines.forEach(line => {
    let logLine = null;
    
    // Format 1: Debug logs - [timestamp] [CATEGORY] message
    let match = line.match(/^\[(\d+)\]\s*\[([A-Z_]+)\]\s*(.*)$/);
    if (match) {
      const [, timestamp, category, message] = match;
      logLine = {
        timestamp: parseInt(timestamp),
        category: category,
        message: message.trim(),
        raw: line
      };
      categories.add(category);
    }
    
    // Format 2: Command audit logs - [timestamp] user@source command -> result
    if (!logLine) {
      match = line.match(/^\[(\d+)\]\s*(\w+)@(\w+)\s+(.+?)\s*->\s*(.*)$/);
      if (match) {
        const [, timestamp, user, source, command, result] = match;
        const category = source.toUpperCase();
        logLine = {
          timestamp: parseInt(timestamp),
          category: category,
          user: user,
          command: command,
          result: result,
          message: user + '@' + source + ' ' + command + ' -> ' + result,
          raw: line
        };
        categories.add(category);
      }
    }
    
    // Format 3: Simple timestamp - [timestamp] message
    if (!logLine) {
      match = line.match(/^\[(\d+)\]\s*(.*)$/);
      if (match) {
        const [, timestamp, message] = match;
        logLine = {
          timestamp: parseInt(timestamp),
          category: 'GENERAL',
          message: message.trim(),
          raw: line
        };
        categories.add('GENERAL');
      }
    }
    
    // Fallback: Malformed line, add as-is
    if (!logLine) {
      logLine = {
        timestamp: 0,
        category: 'UNKNOWN',
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
  const searchText = document.getElementById('viewer-search').value.toLowerCase();
  
  gFilteredLines = gLogLines.filter(line => {
    // Category filter
    if (categoryFilter && line.category !== categoryFilter) return false;
    
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
  
  let html = '';
  gFilteredLines.forEach(line => {
    const categoryColor = getCategoryColor(line.category);
    const timestampStr = line.timestamp.toString().padStart(10, ' ');
    
    if (line.category !== 'UNKNOWN') {
      html += '<div style="margin:2px 0">';
      html += '<span style="color:#569cd6">[' + timestampStr + ']</span> ';
      html += '<span style="color:' + categoryColor + ';font-weight:bold">[' + line.category + ']</span> ';
      html += '<span style="color:#d4d4d4">' + escapeHtml(line.message) + '</span>';
      html += '</div>';
    } else {
      html += '<div style="margin:2px 0;color:#888">' + escapeHtml(line.raw) + '</div>';
    }
  });
  
  content.innerHTML = html;
}

function getCategoryColor(category) {
  const colors = {
    'AUTH': '#f48771',
    'HTTP': '#4ec9b0',
    'SSE': '#4fc1ff',
    'CLI': '#dcdcaa',
    'SENSORS': '#c586c0',
    'SENSORS_FRAME': '#c586c0',
    'SENSORS_DATA': '#c586c0',
    'WIFI': '#ce9178',
    'STORAGE': '#9cdcfe',
    'PERF': '#b5cea8',
    'SYSTEM': '#569cd6',
    'USERS': '#f48771',
    'AUTO': '#d7ba7d',
    'LOGGER': '#608b4e',
    'ESPNOW': '#d16969',
    'MEMORY': '#b5cea8',
    'SECURITY': '#f48771'
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

function populateLogViewerFileList() {
  // Initialize file explorer for log selection
  if (typeof window.createFileExplorer === 'function') {
    window.createFileExplorer({
      containerId: 'log-viewer-file-explorer',
      path: '/logs',
      height: '250px',
      mode: 'select',
      selectFilesOnly: true,
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

console.log('[LOGGING] Section 14a: Log viewer functions defined');
</script>

<script>
console.log('[LOGGING] Section 15: All JavaScript loaded successfully');
</script>
</body>
</html>)JS", HTTPD_RESP_USE_STRLEN);

}

#endif
