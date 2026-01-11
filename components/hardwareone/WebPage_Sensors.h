#ifndef WEBPAGE_SENSORS_H
#define WEBPAGE_SENSORS_H

#include <esp_http_server.h>
#include <Arduino.h>
#include "System_BuildConfig.h"

// Forward declaration for streamBeginHtml (5-param version from web_server.cpp)
extern void streamBeginHtml(httpd_req_t* req, const char* title, bool isPublic, const String& username, const String& activePage);

// Check if any I2C sensors are enabled (and the I2C system itself is compiled in)
#if (ENABLE_I2C_SYSTEM && ( \
     ENABLE_THERMAL_SENSOR || \
     ENABLE_TOF_SENSOR || \
     ENABLE_IMU_SENSOR || \
     ENABLE_GAMEPAD_SENSOR || \
     ENABLE_GPS_SENSOR || \
     ENABLE_APDS_SENSOR || \
     ENABLE_FM_RADIO))
#define I2C_SENSORS_ENABLED 1
#else
#define I2C_SENSORS_ENABLED 0
#endif

#if I2C_SENSORS_ENABLED
// Forward declaration
inline void streamSensorsInner(httpd_req_t* req);

// Individual sensor web modules (conditionally included)
#if ENABLE_THERMAL_SENSOR
#include "i2csensor-mlx90640-web.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor-vl53l4cx-web.h"
#endif
#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor-seesaw-web.h"
#endif
#if ENABLE_GPS_SENSOR
#include "i2csensor-pa1010d-web.h"
#endif
#if ENABLE_FM_RADIO
#include "i2csensor-rda5807-web.h"
#endif
#if ENABLE_IMU_SENSOR
#include "i2csensor-bno055-web.h"
#endif
#include "i2csensor-pca9685-web.h"
#endif

// Top-level Sensors page content streamer (wrapper that conditionally shows sensors or disabled message)
inline void streamSensorsContent(httpd_req_t* req) {
#if I2C_SENSORS_ENABLED
  // Show full sensors page with all sensor modules
  streamSensorsInner(req);
#else
  // Show informational page explaining I2C sensors are disabled
  httpd_resp_set_type(req, "text/html");
  
  // Basic HTML structure
  httpd_resp_send_chunk(req, R"HTML(<!DOCTYPE html>
<html>
<head>
  <meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1.0'>
  <title>Sensors - I2C Disabled</title>
  <link rel='stylesheet' href='/hw.css'>
</head>
<body>)HTML", HTTPD_RESP_USE_STRLEN);

  // Navigation
  httpd_resp_send_chunk(req, R"HTML(<nav><a href='/'>Dashboard</a><a href='/settings'>Settings</a><a href='/sensors' class='active'>Sensors</a><a href='/logs'>Logs</a></nav>)HTML", HTTPD_RESP_USE_STRLEN);

  // Main content
  httpd_resp_send_chunk(req, R"HTML(
<main>
  <div class='page-header'>
    <h1>Sensors</h1>
    <p>I2C sensor monitoring and control</p>
  </div>
  
  <div class='info-card' style='max-width: 600px; margin: 2rem auto; text-align: center;'>
    <h2 style='color: #e67e22; margin-bottom: 1rem;'>I2C Sensor System Disabled</h2>
    <p style='font-size: 1.1rem; margin-bottom: 1.5rem; color: #555;'>
      The I2C sensor subsystem has been disabled during firmware compilation to save memory and reduce complexity.
    </p>
    
    <div style='background: #f8f9fa; padding: 1.5rem; border-radius: 8px; margin: 1.5rem 0; text-align: left;'>
      <h3 style='margin: 0 0 1rem 0; color: #333;'>Disabled Components:</h3>
      <ul style='margin: 0; padding-left: 1.5rem; color: #666;'>
        <li>Thermal Camera (MLX90640)</li>
        <li>Time-of-Flight Sensor (VL53L4CX)</li>  
        <li>IMU Sensor (BNO055)</li>
        <li>Gamepad Controller (Seesaw)</li>
        <li>GPS Module (PA1010D)</li>
        <li>I2C device discovery and management</li>
      </ul>
    </div>
    
    <div style='background: #e8f5e8; padding: 1.5rem; border-radius: 8px; margin: 1.5rem 0; text-align: left;'>
      <h3 style='margin: 0 0 1rem 0; color: #27ae60;'>Available Features:</h3>
      <ul style='margin: 0; padding-left: 1.5rem; color: #27ae60;'>
        <li>WiFi connectivity and web interface</li>
        <li>Settings management</li>
        <li>System logging and diagnostics</li>
        <li>ESP-NOW mesh networking (if enabled)</li>
        <li>OLED display (if connected via different pins)</li>
      </ul>
    </div>
    
    <div style='background: #fff3cd; padding: 1.5rem; border-radius: 8px; margin: 1.5rem 0; text-align: left; border-left: 4px solid #ffc107;'>
      <h3 style='margin: 0 0 1rem 0; color: #856404;'>To Enable Sensors:</h3>
      <p style='margin: 0; color: #856404; font-size: 0.95rem;'>
        Recompile the firmware with <code style='background: #f8f9fa; padding: 2px 6px; border-radius: 3px;'>ENABLE_I2C_SENSORS=1</code> 
        and the desired individual sensor flags (<code style='background: #f8f9fa; padding: 2px 6px; border-radius: 3px;'>ENABLE_THERMAL_SENSOR=1</code>, etc.) 
        in your build configuration.
      </p>
    </div>
    
    <div style='margin-top: 2rem;'>
      <a href='/' class='btn btn-primary' style='margin-right: 1rem;'>← Back to Dashboard</a>
      <a href='/settings' class='btn btn-secondary'>Settings</a>
    </div>
  </div>
</main>
</body></html>)HTML", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, NULL, 0); // End response
#endif
}

#if I2C_SENSORS_ENABLED
// External auth check to get username
extern bool isAuthed(httpd_req_t* req, String& usernameOut);

// Streamed inner content for the Sensors page (CSS + HTML skeleton + small JS)
inline void streamSensorsInner(httpd_req_t* req) {
  // Get username for theme lookup
  String username;
  isAuthed(req, username);
  
  // Stream HTML head with hw helpers (defines window.hw object)
  streamBeginHtml(req, "Sensors", false, username, "sensors");
  
  // CSS
  httpd_resp_send_chunk(req, R"CSS(
<style>
  .sensors-container{max-width:1200px;margin:0 auto;padding:20px}
  .sensor-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(350px,1fr));gap:20px;margin-bottom:30px}
  .sensor-card{background:var(--panel-bg);border-radius:15px;padding:20px;box-shadow:0 4px 6px rgba(0,0,0,.1);border:1px solid var(--border)}
  .sensor-title{font-size:1.3em;font-weight:bold;margin-bottom:10px;color:var(--panel-fg);display:flex;align-items:center;gap:10px}
  .sensor-description{color:var(--muted);margin-bottom:15px;font-size:.9em}
  .sensor-controls{display:flex;gap:10px;margin-bottom:15px;flex-wrap:wrap}
  .sensor-data{background:var(--crumb-bg);border-radius:8px;padding:15px;font-family:'Courier New',monospace;font-size:.9em;min-height:60px;color:var(--panel-fg)}
  .status-indicator{display:inline-block;width:12px;height:12px;min-width:12px;min-height:12px;flex:0 0 12px;border-radius:50%;margin-right:8px;box-sizing:content-box;vertical-align:middle}
  .status-enabled{background:#28a745;animation:pulse 2s infinite}
  .status-disabled{background:#dc3545}
  @keyframes pulse{0%{opacity:1}50%{opacity:.5}100%{opacity:1}}
  /* IMU */
  #gyro-data{color:var(--panel-fg)}
  .imu-grid{display:grid;grid-template-columns:160px 1fr;column-gap:8px;row-gap:6px;align-items:baseline}
  .imu-label{color:var(--panel-fg);font-weight:600;font-family:system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif}
  .imu-val{color:var(--panel-fg);font-family:'Courier New',monospace}
  /* ToF */
  .tof-objects-container{display:flex;flex-direction:column;gap:8px}
  .tof-object-row{display:flex;align-items:center;gap:10px;padding:8px;background:var(--crumb-bg);border:1px solid var(--border);border-radius:4px;box-shadow:0 1px 2px rgba(0,0,0,0.06)}
  .object-label{min-width:70px;font-size:.9em;font-weight:bold;color:var(--panel-fg)}
  .distance-bar-container{flex:1;height:18px;background:var(--crumb-bg);border-radius:0;position:relative;overflow:hidden;border:1px solid var(--border)}
  .distance-bar{height:100%;background:#4caf50;border-radius:0;transition:width .2s ease;width:0%}
  .distance-bar.invalid{background:#9e9e9e;opacity:.4}
  .object-info{min-width:80px;font-size:.9em;text-align:right;color:var(--panel-fg);font-weight:600}
  /* Gamepad */
  .gamepad-row{display:flex;align-items:flex-start;gap:16px}
  .joy-wrap{display:flex;align-items:center;justify-content:center}
  .joy-canvas{width:120px;height:120px;border:1px solid var(--border);border-radius:8px;background:var(--crumb-bg)}
  .abxy-grid{display:grid;grid-template-columns:repeat(3,52px);grid-auto-rows:32px;gap:6px;align-content:start}
</style>
)CSS", HTTPD_RESP_USE_STRLEN);

  // Containers + Cards
  httpd_resp_send_chunk(req, R"HTML(
<div class='sensors-container'>
  <div id='sensors-loading' style='text-align:center;padding:2rem;color:#87ceeb'>
    <div style='font-size:1.1rem;margin-bottom:0.5rem'>Loading sensors...</div>
    <div style='font-size:0.9rem;opacity:0.7'>Checking connected sensors</div>
  </div>

  <!-- Local Sensors Section -->
  <h2 style='color:var(--panel-fg);margin-bottom:20px;margin-top:20px'>Local Sensors</h2>
  <div class='sensor-grid' id='sensors-grid' style='display:none'>

  )HTML", HTTPD_RESP_USE_STRLEN);

#if ENABLE_IMU_SENSOR
  streamBNO055ImuSensorCard(req);
#endif
#if ENABLE_THERMAL_SENSOR
  streamMLX90640ThermalSensorCard(req);
#endif
#if ENABLE_TOF_SENSOR
  streamVL53L4CXTofSensorCard(req);
#endif
#if ENABLE_GAMEPAD_SENSOR
  streamSeesawGamepadSensorCard(req);
#endif
#if ENABLE_GPS_SENSOR
  streamPA1010DGpsSensorCard(req);
#endif
#if ENABLE_FM_RADIO
  streamRDA5807FmRadioSensorCard(req);
#endif
  streamPCA9685ServoDriverCard(req);

  httpd_resp_send_chunk(req, R"HTML(

  </div>

  <!-- Remote Sensors Section (ESP-NOW) -->
  <div style='margin-top:40px;padding-top:30px;border-top:2px solid rgba(255,255,255,0.1)'>
    <h2 style='color:var(--panel-fg);margin-bottom:20px'>Remote Sensors (ESP-NOW)</h2>
    <div id='remote-sensors-status' style='background:rgba(255,255,255,0.05);border-radius:10px;padding:20px;margin-bottom:20px;color:var(--panel-fg)'>
      <div style='text-align:center;padding:1rem'>Loading remote sensors...</div>
    </div>
    <div class='sensor-grid' id='remote-sensors-grid' style='display:none'></div>
  </div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  // Small script: detect devices and toggle card visibility (using shared hw helpers)
  httpd_resp_send_chunk(req, "<script>console.log('[SENSORS] Section 1: Pre-script sentinel');</script><script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "console.log('[SENSORS] Device detection starting...');(function(){try{var loading=hw._ge('sensors-loading');var grid=hw._ge('sensors-grid');", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "var setVis=function(id,show){var el=hw._ge(id);if(el){el.style.display=show?'':'none';}};", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "Promise.all([hw.fetchJSON('/api/devices'),hw.fetchJSON('/api/sensors/status')]).then(function(rs){var d=rs[0]||{};var st=rs[1]||{};console.log('[SENSORS] Devices response:',d);console.log('[SENSORS] Status response:',st);", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "var has={imu:false,thermal:false,tof:false,gamepad:false,gps:false,servo:false,fmradio:false};if(d&&d.devices&&d.devices.forEach){d.devices.forEach(function(dev){", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "if(dev&&dev.name==='BNO055')has.imu=true;else if(dev&&dev.name==='MLX90640')has.thermal=true;else if(dev&&dev.name==='VL53L4CX')has.tof=true;else if(dev&&dev.name==='Seesaw')has.gamepad=true;else if(dev&&dev.name==='PA1010D')has.gps=true;else if(dev&&dev.name==='PCA9685')has.servo=true;else if(dev&&dev.name==='RDA5807')has.fmradio=true;});}console.log('[SENSORS] Detected sensors:',has);", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "var compiled={imu:!!st.imuCompiled,thermal:!!st.thermalCompiled,tof:!!st.tofCompiled,gamepad:!!st.gamepadCompiled,gps:!!st.gpsCompiled,fmradio:true,servo:true};console.log('[SENSORS] Compiled sensors:',compiled);", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "setVis('sensor-card-imu',has.imu&&compiled.imu);setVis('sensor-card-thermal',has.thermal&&compiled.thermal);setVis('sensor-card-tof',has.tof&&compiled.tof);setVis('sensor-card-gamepad',has.gamepad&&compiled.gamepad);setVis('sensor-card-gps',has.gps&&compiled.gps);setVis('sensor-card-servo',has.servo&&compiled.servo);setVis('sensor-card-fmradio',has.fmradio&&compiled.fmradio);", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "var any=(has.imu&&compiled.imu)||(has.thermal&&compiled.thermal)||(has.tof&&compiled.tof)||(has.gamepad&&compiled.gamepad)||(has.gps&&compiled.gps)||(has.servo&&compiled.servo)||(has.fmradio&&compiled.fmradio);if(!any&&grid){grid.innerHTML='<div style=\\\"grid-column:1/-1;text-align:center;padding:2rem;color:#87ceeb;font-style:italic\\\">No sensors available (none compiled + detected)</div>';}console.log('[SENSORS] Device detection complete');", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "}).catch(function(e){console.error('[SENSORS] Device/status fetch error:',e);}).finally(function(){if(loading)loading.style.display='none';if(grid)grid.style.display='grid';});", HTTPD_RESP_USE_STRLEN);

  // Control helpers
  httpd_resp_send_chunk(req, "console.log('[SENSORS] Setting up control helpers');var setClass=function(id,enabled){var el=hw._ge(id);if(!el)return;var c=enabled?'status-indicator status-enabled':'status-indicator status-disabled';if(el.className!==c)el.className=c};", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "var bind=function(id,cmd){var el=hw._ge(id);if(el){hw.on(el,'click',function(){console.log('[SENSORS] Button clicked:',id,'cmd:',cmd);hw.postForm('/api/cli',{cmd:cmd}).then(function(r){console.log('[SENSORS] Command result:',r);try{var action=(/start$/.test(cmd)?'start':(/stop$/.test(cmd)?'stop':''));var sensor=(/^imu/.test(cmd)?'imu':(/^thermal/.test(cmd)?'thermal':(/^tof/.test(cmd)?'tof':(/^gamepad/.test(cmd)?'gamepad':(/^gps/.test(cmd)?'gps':(/^fmradio/.test(cmd)?'fmradio':''))))));if(action==='start'&&sensor){startSensorPolling(sensor)}else if(action==='stop'&&sensor){stopSensorPolling(sensor)}}catch(_){}}).catch(function(e){console.error('[SENSORS] Command error:',e);})})}};", HTTPD_RESP_USE_STRLEN);
#if ENABLE_IMU_SENSOR
  streamBNO055ImuSensorBindButtons(req);
#endif
#if ENABLE_THERMAL_SENSOR
  streamMLX90640ThermalSensorBindButtons(req);
#endif
#if ENABLE_TOF_SENSOR
  streamVL53L4CXTofSensorBindButtons(req);
#endif
#if ENABLE_GAMEPAD_SENSOR
  streamSeesawGamepadSensorBindButtons(req);
#endif
#if ENABLE_GPS_SENSOR
  streamPA1010DGpsSensorBindButtons(req);
#endif
#if ENABLE_FM_RADIO
  streamRDA5807FmRadioSensorBindButtons(req);
#endif
  httpd_resp_send_chunk(req, "console.log('[SENSORS] Button bindings complete');", HTTPD_RESP_USE_STRLEN);

  // Status poller
  httpd_resp_send_chunk(req, "console.log('[SENSORS] Setting up status poller');var apply=function(s){console.log('[SENSORS] Status update:',s);try{setClass('gyro-status-indicator',!!s.imuEnabled);setClass('thermal-status-indicator',!!s.thermalEnabled);setClass('tof-status-indicator',!!s.tofEnabled);setClass('gamepad-status-indicator',!!s.gamepadEnabled);setClass('gps-status-indicator',!!s.gpsEnabled);setClass('fmradio-status-indicator',!!s.fmRadioEnabled);setClass('servo-status-indicator',!!s.pwmDriverConnected);if(s.fmRadioEnabled){if(typeof startFMRadioPolling==='function')startFMRadioPolling()}else{if(typeof stopFMRadioPolling==='function')stopFMRadioPolling()}var servoStatus=hw._ge('servo-connection-status');if(servoStatus){servoStatus.textContent=s.pwmDriverConnected?'Initialized & ready':'Not initialized (use servo command to start)';servoStatus.style.color=s.pwmDriverConnected?'#28a745':'#ffc107';}}catch(_){}};", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "hw.fetchJSON('/api/sensors/status').then(apply).catch(function(e){console.error('[SENSORS] Status fetch error:',e);})", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, ";hw.pollJSON('/api/sensors/status',1000,apply);console.log('[SENSORS] Status poller started');", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "}catch(e){console.error('[SENSORS] Init error:',e);}})();", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "window.sendCmd=function(cmd){hw.postForm('/api/cli',{cmd:cmd}).then(function(r){console.log('[SENSORS] sendCmd result:',r);}).catch(function(e){console.error('[SENSORS] sendCmd error:',e);});};", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);

  // Step A: Port core JS behavior from legacy page (variables + settings + color maps + core fns + SSE + init)
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Loading core variables & settings...');}catch(_){ }", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "var sensorIntervals={};var thermalPollingInterval=null;var thermalPollingMs=200;var tofPollingInterval=null;var fmradioPollingInterval=null;var tofObjectStates=[{},{},{},{}];var tofStabilityThreshold=2;var tofMaxDistance=3400;var tofPollingMs=300;var tofTransitionMs=200;var settingsLoaded=false;var thermalPalette='grayscale';var thermalColorMap={};var thermalEWMAFactor=0.2;var thermalInterpolationEnabled=false;var thermalInterpolationSteps=3;var thermalInterpolationBufferSize=3;var thermalUpscaleFactor=1;var thermalTransitionMs=120;var thermalPreviousFrame=null;var debugSettings={sensorsFrame:0,http:0,sse:0};", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function debugLog(category,message){try{if(debugSettings[category]){console.log('[DEBUG-'+category.toUpperCase()+']',message);}}catch(_){}}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Core variables ready');}catch(_){ }", HTTPD_RESP_USE_STRLEN);

  // Settings loader
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Loading settings loader...');}catch(_){ }", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, 
    "function loadSensorSettings() {\n"
    "  console.log('[Settings] Loading sensor settings...');\n"
    "  return fetch('/api/settings', {cache: 'no-store'})\n"
    "    .then(function(r) {\n"
    "      if (!r.ok) throw new Error('Settings fetch failed');\n"
    "      return r.json();\n"
    "    })\n"
    "    .then(function(s) {\n"
    "      console.log('[Settings] Loaded:', s);\n"
    "      if (s.settings && s.settings.thermal && s.settings.thermal.ui && s.settings.thermal.ui.thermalPollingMs !== undefined) {\n"
    "        thermalPollingMs = s.settings.thermal.ui.thermalPollingMs;\n"
    "        console.log('[Settings] Thermal polling: ' + thermalPollingMs + 'ms');\n"
    "      }\n"
    "      if (s.settings && s.settings.tof && s.settings.tof.ui && s.settings.tof.ui.tofPollingMs !== undefined) {\n"
    "        tofPollingMs = s.settings.tof.ui.tofPollingMs;\n"
    "        console.log('[Settings] ToF polling: ' + tofPollingMs + 'ms');\n"
    "      }\n"
    "      if (s.settings && s.settings.tof && s.settings.tof.ui && s.settings.tof.ui.tofStabilityThreshold !== undefined) {\n"
    "        tofStabilityThreshold = s.settings.tof.ui.tofStabilityThreshold;\n"
    "        console.log('[Settings] ToF stability threshold: ' + tofStabilityThreshold);\n"
    "      }\n"
    "      if (s.settings && s.settings.tof && s.settings.tof.ui && s.settings.tof.ui.tofUiMaxDistanceMm !== undefined) {\n"
    "        tofMaxDistance = s.settings.tof.ui.tofUiMaxDistanceMm;\n"
    "        console.log('[Settings] ToF max distance: ' + tofMaxDistance + 'mm');\n"
    "        var rng = document.getElementById('tof-range-mm');\n"
    "        if (rng) {\n"
    "          rng.textContent = String(tofMaxDistance);\n"
    "        }\n"
    "      }\n"
    "      if (s.settings && s.settings.tof && s.settings.tof.ui && s.settings.tof.ui.tofTransitionMs !== undefined) {\n"
    "        tofTransitionMs = s.settings.tof.ui.tofTransitionMs;\n"
    "        console.log('[Settings] ToF transition ms: ' + tofTransitionMs);\n"
    "      }\n"
    "      if (s.settings && s.settings.thermal && s.settings.thermal.ui && s.settings.thermal.ui.thermalPaletteDefault !== undefined) {\n"
    "        thermalPalette = s.settings.thermal.ui.thermalPaletteDefault;\n"
    "        console.log('[Settings] Thermal palette: ' + thermalPalette);\n"
    "        applyThermalPalette(thermalPalette);\n"
    "      }\n"
    "      if (s.settings && s.settings.thermal && s.settings.thermal.ui && s.settings.thermal.ui.thermalEWMAFactor !== undefined) {\n"
    "        thermalEWMAFactor = s.settings.thermal.ui.thermalEWMAFactor;\n"
    "        console.log('[Settings] Thermal EWMA factor: ' + thermalEWMAFactor);\n"
    "      }\n"
    "      if (s.settings && s.settings.debug) {\n"
    "        debugSettings.thermal = s.settings.debug.sensorsFrame || false;\n"
    "        debugSettings.tof = s.settings.debug.sensorsFrame || false;\n"
    "        debugSettings.imu = s.settings.debug.sensorsFrame || false;\n"
    "        debugSettings.data = s.settings.debug.sensorsData || false;\n"
    "        debugSettings.general = s.settings.debug.sensorsGeneral || false;\n"
    "        console.log('[Settings] Debug flags:', debugSettings);\n"
    "      }\n"
    "      return s;\n"
    "    })\n"
    "    .catch(function(e) {\n"
    "      console.error('[Settings] Error loading sensor settings:', e);\n"
    "      return null;\n"
    "    });\n"
    "}\n", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Settings loader ready');}catch(_){ }", HTTPD_RESP_USE_STRLEN);

  // Core functions: control, readSensor (IMU, gamepad), generic polling
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Loading core control functions...');}catch(_){ }", HTTPD_RESP_USE_STRLEN);

#if ENABLE_GPS_SENSOR
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Loading GPS sensor module...');}catch(_){ }", HTTPD_RESP_USE_STRLEN);
  streamPA1010DGpsSensorJs(req);
#endif

  httpd_resp_send_chunk(req, "window._sensorReaders=window._sensorReaders||{};window._sensorDataIds=window._sensorDataIds||{};window._sensorPollingIntervals=window._sensorPollingIntervals||{};", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req,
    "function controlSensor(sensor, action) {\n"
    "  var command = sensor + action;\n"
    "  return fetch('/api/cli', {\n"
    "    method: 'POST',\n"
    "    headers: {'Content-Type': 'application/x-www-form-urlencoded'},\n"
    "    body: 'cmd=' + encodeURIComponent(command)\n"
    "  })\n"
    "  .then(function(r) {\n"
    "    return r.text();\n"
    "  })\n"
    "  .then(function(result) {\n"
    "    console.log('[Sensors] control result', result);\n"
    "    return fetch('/api/sensors/status', {cache: 'no-store'})\n"
    "      .then(function(r) {\n"
    "        return r.json();\n"
    "      })\n"
    "      .then(function(status) {\n"
    "        if (typeof window.applySensorStatus === 'function') {\n"
    "          window.applySensorStatus(status);\n"
    "        }\n"
    "      });\n"
    "  })\n"
    "  .catch(function(e) {\n"
    "    console.error('Sensor control error:', e);\n"
    "    throw e;\n"
    "  });\n"
    "}\n", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function readSensor(sensor) {\n", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "  var k=String(sensor||'');\n", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "  try{if(window._sensorReaders&&typeof window._sensorReaders[k]==='function'){return window._sensorReaders[k]();}}catch(_){}\n", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "  return Promise.resolve('Sensor read placeholder');\n", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "}\n", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function getSensorDataId(sensor){var k=String(sensor||'');if(window._sensorDataIds&&window._sensorDataIds[k])return window._sensorDataIds[k];if(k.indexOf('imu')!==-1)return 'gyro-data';if(k.indexOf('tof')!==-1)return 'tof-data';if(k.indexOf('thermal')!==-1)return 'thermal-data';if(k.indexOf('gamepad')!==-1)return 'gamepad-data';return k+'-data'}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function startSensorPolling(sensor){console.log('[SENSORS] startSensorPolling called for:',sensor);if(sensorIntervals[sensor]){console.log('[SENSORS] Already polling',sensor);return}if(sensor==='thermal'){if(typeof startThermalPolling==='function'){startThermalPolling()}return}else if(sensor==='tof'){if(typeof startToFPolling==='function'){startToFPolling()}return}else{readSensor(sensor);var interval=(window._sensorPollingIntervals&&window._sensorPollingIntervals[sensor])?window._sensorPollingIntervals[sensor]:(sensor==='imu'?200:(sensor==='gamepad'?56:500));console.log('[SENSORS] Starting',sensor,'polling with interval:',interval+'ms');sensorIntervals[sensor]=setInterval(function(){readSensor(sensor)},interval)}}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function stopSensorPolling(sensor){console.log('[SENSORS] stopSensorPolling called for:',sensor);if(sensorIntervals[sensor]){clearInterval(sensorIntervals[sensor]);delete sensorIntervals[sensor];console.log('[SENSORS] Stopped polling',sensor)}if(sensor==='thermal'){if(typeof stopThermalPolling==='function'){stopThermalPolling()}}else if(sensor==='tof'){if(typeof stopToFPolling==='function'){stopToFPolling()}}}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Core control functions ready');}catch(_){ }", HTTPD_RESP_USE_STRLEN);

  // SSE + indicators + already-active check
  httpd_resp_send_chunk(req, "window.attachPageSSEListeners=function(es){if(!es){console.warn('[Sensors] attachPageSSEListeners called with null EventSource');return}console.log('[Sensors] Attaching sensor-status listener to EventSource');var handler=function(e){try{var status=JSON.parse(e.data||'{}');console.log('[Sensors] Received SSE sensor-status update:',status);if(window.applySensorStatus){window.applySensorStatus(status)}updateStatusIndicators(status)}catch(err){console.warn('[Sensors] SSE sensor-status parse error:',err)}};es.addEventListener('sensor-status',handler);console.log('[Sensors] SSE sensor-status listener attached successfully')};", HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req,
    "window.applySensorStatus = function(status) {\n"
    "  if (!status) return;\n"
    "  console.log('[Queue] Applying sensor status:', status);\n"
    "  ['thermal', 'tof', 'imu', 'gps', 'gamepad', 'fmradio'].forEach(function(sensor) {\n"
    "    var queueEl = document.getElementById(sensor + '-queue-status');\n"
    "    if (!queueEl) return;\n"
    "    var isQueued = status[sensor + 'Queued'];\n"
    "    var queuePos = status[sensor + 'QueuePos'];\n"
    "    if (isQueued && queuePos > 0) {\n"
    "      var qd = status.queueDepth || 0;\n"
    "      queueEl.textContent = '⏱️ Queued for start (position ' + queuePos + ' of ' + qd + ')';\n"
    "      queueEl.style.display = 'block';\n"
    "    } else {\n"
    "      queueEl.style.display = 'none';\n"
    "    }\n"
    "  });\n"
    "  try {\n"
    "    if (status.imuCompiled && status.imuEnabled) {\n"
    "      startSensorPolling('imu');\n"
    "    } else {\n"
    "      stopSensorPolling('imu');\n"
    "    }\n"
    "    if (status.gamepadCompiled && status.gamepadEnabled) {\n"
    "      startSensorPolling('gamepad');\n"
    "    } else {\n"
    "      stopSensorPolling('gamepad');\n"
    "    }\n"
    "    if (status.gpsCompiled && status.gpsEnabled) {\n"
    "      startSensorPolling('gps');\n"
    "    } else {\n"
    "      stopSensorPolling('gps');\n"
    "    }\n"
    "    if (status.thermalCompiled && status.thermalEnabled) {\n"
    "      if (typeof startThermalPolling === 'function') startThermalPolling();\n"
    "    } else {\n"
    "      if (typeof stopThermalPolling === 'function') stopThermalPolling();\n"
    "    }\n"
    "    if (status.tofCompiled && status.tofEnabled) {\n"
    "      if (typeof startToFPolling === 'function') startToFPolling();\n"
    "    } else {\n"
    "      if (typeof stopToFPolling === 'function') stopToFPolling();\n"
    "    }\n"
    "    if (window._lastFmRadioEnabled !== status.fmRadioEnabled) {\n"
    "      if (status.fmRadioEnabled) {\n"
    "        if (typeof startFMRadioPolling === 'function') startFMRadioPolling();\n"
    "      } else {\n"
    "        if (typeof stopFMRadioPolling === 'function') stopFMRadioPolling();\n"
    "      }\n"
    "      window._lastFmRadioEnabled = status.fmRadioEnabled;\n"
    "    }\n"
    "  } catch (_) {}\n"
    "};\n", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function updateStatusIndicators(status){if(!status){console.warn('[Sensors] updateStatusIndicators called with null/undefined status');return}var t=document.getElementById('thermal-status-indicator');var f=document.getElementById('tof-status-indicator');var i=document.getElementById('gyro-status-indicator');var g=document.getElementById('gamepad-status-indicator');var r=document.getElementById('fmradio-status-indicator');if(t){t.className=status.thermalEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}if(f){f.className=status.tofEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}if(i){i.className=status.imuEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}if(g){g.className=status.gamepadEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}if(r){r.className=status.fmRadioEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req,
    "function checkAlreadyActiveSensors() {\n"
    "  console.log('[Sensors] Checking for already-active sensors...');\n"
    "  Promise.all([\n"
    "    fetch('/api/devices').then(function(r) { return r.json(); }),\n"
    "    fetch('/api/sensors/status').then(function(r) { return r.json(); })\n"
    "  ])\n"
    "  .then(function(results) {\n"
    "    var devicesData = results[0];\n"
    "    var status = results[1];\n"
    "    console.log('[Sensors] Devices:', devicesData);\n"
    "    console.log('[Sensors] Status:', status);\n"
    "    var devices = devicesData.devices || [];\n"
    "    devices.forEach(function(device) {\n"
    "      var map = {\n"
    "        'BNO055': {type: 'imu', enabledKey: 'imuEnabled', compiledKey: 'imuCompiled', indicatorId: 'gyro-status-indicator'},\n"
    "        'MLX90640': {type: 'thermal', enabledKey: 'thermalEnabled', compiledKey: 'thermalCompiled', indicatorId: 'thermal-status-indicator'},\n"
    "        'VL53L4CX': {type: 'tof', enabledKey: 'tofEnabled', compiledKey: 'tofCompiled', indicatorId: 'tof-status-indicator'},\n"
    "        'PA1010D': {type: 'gps', enabledKey: 'gpsEnabled', compiledKey: 'gpsCompiled', indicatorId: 'gps-status-indicator'},\n"
    "        'RDA5807': {type: 'fmradio', enabledKey: 'fmRadioEnabled', compiledKey: 'fmradioCompiled', indicatorId: 'fmradio-status-indicator'},\n"
    "        'Seesaw': {type: 'gamepad', enabledKey: 'gamepadEnabled', compiledKey: 'gamepadCompiled', indicatorId: 'gamepad-status-indicator', address: 0x50}\n"
    "      }[device.name];\n"
    "      if (!map) {\n"
    "        if (device && (device.name === 'SSD1306')) return;\n"
    "        console.log('[Sensors] No map for device:', device.name);\n"
    "        return;\n"
    "      }\n"
    "      if (device.name === 'Seesaw' && device.address !== 0x50) return;\n"
    "      if (map.compiledKey && status && status[map.compiledKey] === false) {\n"
    "        console.log('[Sensors] Skipping', device.name, '(not compiled)');\n"
    "        return;\n"
    "      }\n"
    "      console.log('[Sensors] Checking', device.name, 'enabled=', status[map.enabledKey]);\n"
    "      if (status[map.enabledKey]) {\n"
    "        console.log('[Sensors] ' + device.name + ' connected and enabled - starting client polling');\n"
    "        var ind = document.getElementById(map.indicatorId);\n"
    "        if (ind) ind.className = 'status-indicator status-enabled';\n"
    "        startSensorPolling(map.type);\n"
    "      }\n"
    "    });\n"
    "  })\n"
    "  .catch(function(err) {\n"
    "    console.warn('[Sensors] Status check failed:', err);\n"
    "  });\n"
    "}\n", HTTPD_RESP_USE_STRLEN);

  // Remote sensors loader
  httpd_resp_send_chunk(req, 
    "function stopRemoteSensorsPolling(){\n"
    "  try{if(window._remoteSensorsTimer){clearInterval(window._remoteSensorsTimer);window._remoteSensorsTimer=null;}}catch(_){}\n"
    "}\n"
    "function updateRemoteSensor(deviceMac,sensorType){\n"
    "  try{\n"
    "    var id='remote-'+String(deviceMac).replace(/:/g,'')+'-'+sensorType;\n"
    "    var el=hw._ge(id);\n"
    "    if(!el)return;\n"
    "    var url='/api/sensors/remote?device='+encodeURIComponent(deviceMac)+'&sensor='+encodeURIComponent(sensorType);\n"
    "    hw.fetchJSON(url).then(function(d){\n"
    "      if(!el)return;\n"
    "      if(d&&d.error){el.textContent=d.error;return;}\n"
    "      var payload=d;\n"
    "      if(payload&&typeof payload==='object'&&payload.data!==undefined){payload=payload.data;}\n"
    "      if(typeof payload==='string'){try{payload=JSON.parse(payload);}catch(_){}}\n"
    "      if(sensorType==='gamepad'&&payload&&typeof payload==='object'&&typeof window.hwRenderGamepadState==='function'){\n"
    "        try{\n"
    "          window.hwRenderGamepadState(payload,{data:id,joystick:id+'-joystick',btnX:id+'-btn-x',btnY:id+'-btn-y',btnA:id+'-btn-a',btnB:id+'-btn-b',btnSelect:id+'-btn-select',btnStart:id+'-btn-start'});\n"
    "        }catch(_){ }\n"
    "        return;\n"
    "      }\n"
    "      if(sensorType==='gamepad'&&payload&&typeof payload==='object'){\n"
    "        var btn=payload.buttons;\n"
    "        var btnHex=(typeof btn==='number')?('0x'+(btn>>>0).toString(16)):String(btn);\n"
    "        if(payload.x===undefined&&payload.y===undefined&&payload.buttons===undefined){\n"
    "          try{el.textContent=JSON.stringify(payload);}catch(_){el.textContent=String(payload);}\n"
    "          return;\n"
    "        }\n"
    "        el.textContent='x: '+payload.x+'  y: '+payload.y+'  buttons: '+btnHex;\n"
    "        return;\n"
    "      }\n"
    "      try{el.textContent=JSON.stringify(payload);}catch(_){el.textContent=String(payload);}\n"
    "    }).catch(function(_e){if(el)el.textContent='Error';});\n"
    "  }catch(_){}\n"
    "}\n"
    "function startRemoteSensorsPolling(devices){\n"
    "  stopRemoteSensorsPolling();\n"
    "  if(!devices||!devices.forEach)return;\n"
    "  var tick=function(){\n"
    "    devices.forEach(function(device){\n"
    "      if(!device||!device.sensors||!device.sensors.forEach)return;\n"
    "      device.sensors.forEach(function(sensorType){updateRemoteSensor(device.mac,sensorType);});\n"
    "    });\n"
    "  };\n"
    "  tick();\n"
    "  window._remoteSensorsTimer=setInterval(tick,1000);\n"
    "}\n"
    "function loadRemoteSensors() {\n"
    "  var statusDiv = hw._ge('remote-sensors-status');\n"
    "  var gridDiv = hw._ge('remote-sensors-grid');\n"
    "  console.log('[REMOTE_SENSORS] Loading remote sensors...');\n"
    "  hw.fetchJSON('/api/sensors/remote').then(function(data) {\n"
    "    console.log('[REMOTE_SENSORS] Response:', data);\n"
    "    if (!data || !data.devices || data.devices.length === 0) {\n"
    "      if (statusDiv) {\n"
    "        statusDiv.innerHTML = '<div style=\"text-align:center;padding:1rem;color:var(--muted)\">ESP-NOW not enabled or no remote devices found</div>';\n"
    "        statusDiv.style.display = 'block';\n"
    "      }\n"
    "      if (gridDiv) gridDiv.style.display = 'none';\n"
    "      stopRemoteSensorsPolling();\n"
    "      return;\n"
    "    }\n"
    "    if (statusDiv) statusDiv.style.display = 'none';\n"
    "    if (gridDiv) {\n"
    "      gridDiv.innerHTML = '';\n"
    "      data.devices.forEach(function(device) {\n"
    "        device.sensors.forEach(function(sensorType) {\n"
    "          var card = document.createElement('div');\n"
    "          card.className = 'sensor-card';\n"
    "          var macKey = device.mac.replace(/:/g, '');\n"
    "          if(sensorType==='gamepad'&&typeof window.hwRenderGamepadState==='function'){\n"
    "            var base='remote-'+macKey+'-gamepad';\n"
    "            card.innerHTML = '<div class=\"sensor-title\"><span class=\"status-indicator status-enabled\"></span>' + device.name + ' - ' + sensorType + '</div>' +\n"
    "              '<div class=\"sensor-description\">Remote sensor via ESP-NOW (MAC: ' + device.mac + ')</div>' +\n"
    "              '<div class=\"sensor-data\" id=\"'+base+'\">Loading...</div>' +\n"
    "              '<div class=\"gamepad-row\" style=\"margin-top:10px\">' +\n"
    "                '<div class=\"joy-wrap\"><canvas id=\"'+base+'-joystick\" class=\"joy-canvas\" width=\"120\" height=\"120\"></canvas></div>' +\n"
    "                '<div class=\"abxy-grid\">' +\n"
    "                  '<div></div><div id=\"'+base+'-btn-x\" class=\"btn btn-small\" style=\"width:52px\">X</div><div></div>' +\n"
    "                  '<div id=\"'+base+'-btn-y\" class=\"btn btn-small\" style=\"width:52px\">Y</div><div></div><div id=\"'+base+'-btn-a\" class=\"btn btn-small\" style=\"width:52px\">A</div><div></div><div id=\"'+base+'-btn-b\" class=\"btn btn-small\" style=\"width:52px\">B</div><div></div>' +\n"
    "                '</div>' +\n"
    "                '<div style=\"display:flex;flex-direction:column;gap:6px;margin-left:12px\">' +\n"
    "                  '<div id=\"'+base+'-btn-select\" class=\"btn btn-small\" style=\"width:80px\">Select</div>' +\n"
    "                  '<div id=\"'+base+'-btn-start\" class=\"btn btn-small\" style=\"width:80px\">Start</div>' +\n"
    "                '</div>' +\n"
    "              '</div>';\n"
    "          } else {\n"
    "            card.innerHTML = '<div class=\"sensor-title\"><span class=\"status-indicator status-enabled\"></span>' + device.name + ' - ' + sensorType + '</div>' +\n"
    "              '<div class=\"sensor-description\">Remote sensor via ESP-NOW (MAC: ' + device.mac + ')</div>' +\n"
    "              '<div class=\"sensor-data\" id=\"remote-' + macKey + '-' + sensorType + '\">Loading...</div>';\n"
    "          }\n"
    "          gridDiv.appendChild(card);\n"
    "        });\n"
    "      });\n"
    "      gridDiv.style.display = 'grid';\n"
    "    }\n"
    "    startRemoteSensorsPolling(data.devices);\n"
    "  }).catch(function(err) {\n"
    "    console.error('[REMOTE_SENSORS] Error:', err);\n"
    "    if (statusDiv) {\n"
    "      statusDiv.innerHTML = '<div style=\"text-align:center;padding:1rem;color:#dc3545\">Error loading remote sensors</div>';\n"
    "      statusDiv.style.display = 'block';\n"
    "    }\n"
    "    stopRemoteSensorsPolling();\n"
    "  });\n"
    "}\n", HTTPD_RESP_USE_STRLEN);

  // Main init: load settings, visibility, handlers, SSE, remote sensors
  httpd_resp_send_chunk(req, "document.addEventListener('DOMContentLoaded',function(){console.log('[SENSORS] DOMContentLoaded');loadSensorSettings().then(function(){/* card visibility handled earlier */checkAlreadyActiveSensors()});loadRemoteSensors();/* button handlers wired above (bind) */if(window.__es){window.attachPageSSEListeners(window.__es)}});", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);

#if ENABLE_THERMAL_SENSOR
  streamMLX90640ThermalSensorJs(req);
#endif
#if ENABLE_FM_RADIO
  streamRDA5807FmRadioSensorJs(req);
#endif
#if ENABLE_TOF_SENSOR
  streamVL53L4CXTofSensorJs(req);
#endif
#if ENABLE_IMU_SENSOR
  streamBNO055ImuSensorJs(req);
#endif
#if ENABLE_GAMEPAD_SENSOR
  streamSeesawGamepadSensorJs(req);
#endif
}
#endif // I2C_SENSORS_ENABLED

#endif // WEB_SENSORS_H