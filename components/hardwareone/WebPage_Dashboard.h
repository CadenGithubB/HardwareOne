#ifndef WEBPAGE_DASHBOARD_H
#define WEBPAGE_DASHBOARD_H

#include "System_BuildConfig.h"
#include "System_User.h"  // For isAdminUser declaration
#if ENABLE_WIFI
  #include <WiFi.h>
#endif
#if ENABLE_WEB_SENSORS
  #if ENABLE_IMU_SENSOR
    #include "i2csensor-bno055-web.h"
  #endif
  #if ENABLE_THERMAL_SENSOR
    #include "i2csensor-mlx90640-web.h"
  #endif
  #if ENABLE_TOF_SENSOR
    #include "i2csensor-vl53l4cx-web.h"
  #endif
  #if ENABLE_GAMEPAD_SENSOR
    #include "i2csensor-seesaw-web.h"
  #endif
  #if ENABLE_FM_RADIO
  #include "i2csensor-rda5807-web.h"
  #endif
  #include "i2csensor-pca9685-web.h"
  #if ENABLE_CAMERA_SENSOR
  #include "System_Camera_DVP_Web.h"
  #endif
  #if ENABLE_MICROPHONE_SENSOR
  #include "System_Microphone_Web.h"
  #endif
  #if ENABLE_GPS_SENSOR
    #include "i2csensor-pa1010d-web.h"
  #endif
  #if ENABLE_RTC_SENSOR
    #include "i2csensor-ds3231-web.h"
  #endif
  #if ENABLE_PRESENCE_SENSOR
    #include "i2csensor-sths34pf80-web.h"
  #endif
#endif

inline void streamDashboardInner(httpd_req_t* req, const String& username) {
  // Header
  httpd_resp_send_chunk(req, "<h2>Dashboard</h2>", HTTPD_RESP_USE_STRLEN);

  // Ensure dashboard hides sensors whose modules are not compiled into firmware.
  // This runs early and patches functions once the main dashboard JS is loaded.
  httpd_resp_send_chunk(req,
    "<script>(function(){\n"
    "  function isCompiled(key,s){\n"
    "    if(!s) return true;\n"
    "    var k=String(key||'')+'Compiled';\n"
    "    if(typeof s[k]==='boolean') return !!s[k];\n"
    "    if(key==='fmradio'||key==='pwm') return true;\n"
    "    return true;\n"
    "  }\n"
    "  function tryPatch(){\n"
    "    if(window.__dashCompiledPatched) return true;\n"
    "    if(typeof window.createSensorCards!=='function' || typeof window.getAvailableSensors!=='function') return false;\n"
    "    window.__dashCompiledPatched=true;\n"
    "    var origGet=window.getAvailableSensors;\n"
    "    window.getAvailableSensors=function(deviceRegistry){\n"
    "      var list=origGet(deviceRegistry)||[];\n"
    "      var s=window.__lastSensorStatus||null;\n"
    "      try{\n"
    "        if(s && s.micCompiled){\n"
    "          var has=false;\n"
    "          for(var i=0;i<list.length;i++){if(list[i]&&list[i].key==='mic'){has=true;break;}}\n"
    "          if(!has){\n"
    "            var defs=window.__dashSensorDefs||[];\n"
    "            for(var di=0;di<defs.length;di++){var d=defs[di];if(d&&d.key==='mic'){list.push({key:'mic',name:d.name||'Microphone',desc:d.desc||''});break;}}\n"
    "          }\n"
    "        }\n"
    "      }catch(_){ }\n"
    "      return list.filter(function(it){return it && isCompiled(it.key,s);});\n"
    "    };\n"
    "    if(typeof window.getSensorEnabled==='function'){\n"
    "      var origEn=window.getSensorEnabled;\n"
    "      window.getSensorEnabled=function(key,status){\n"
    "        if(key==='mic') return !!(status && status.micEnabled);\n"
    "        return origEn(key,status);\n"
    "      };\n"
    "    }\n"
    "    var origCreate=window.createSensorCards;\n"
    "    window.createSensorCards=function(sensorStatus,deviceRegistry){\n"
    "      window.__lastSensorStatus=sensorStatus||null;\n"
    "      var r=origCreate(sensorStatus,deviceRegistry);\n"
    "      try{\n"
    "        var grid=document.getElementById('sensor-grid');\n"
    "        if(!grid)return r;\n"
    "        var s=sensorStatus||{};\n"
    "        var devNames={};\n"
    "        if(deviceRegistry&&Array.isArray(deviceRegistry.devices)){\n"
    "          deviceRegistry.devices.forEach(function(d){if(d&&d.name)devNames[d.name]=true;});\n"
    "        }\n"
    "        var nameMap={\n"
    "          'BNO055':{key:'imu',label:'IMU (BNO055)',compiled:'imuCompiled'},\n"
    "          'MLX90640':{key:'thermal',label:'Thermal Camera (MLX90640)',compiled:'thermalCompiled'},\n"
    "          'VL53L4CX':{key:'tof',label:'ToF Distance (VL53L4CX)',compiled:'tofCompiled'},\n"
    "          'Seesaw':{key:'gamepad',label:'Gamepad (Seesaw)',compiled:'gamepadCompiled'},\n"
    "          'PA1010D':{key:'gps',label:'GPS (PA1010D)',compiled:'gpsCompiled'},\n"
    "          'RDA5807':{key:'fmradio',label:'FM Radio (RDA5807)',compiled:'fmRadioCompiled'},\n"
    "          'DS3231':{key:'rtc',label:'RTC (DS3231)',compiled:'rtcCompiled'},\n"
    "          'STHS34PF80':{key:'presence',label:'Presence (STHS34PF80)',compiled:'presenceCompiled'}\n"
    "        };\n"
    "        var uncompiled=[];\n"
    "        for(var dn in nameMap){\n"
    "          if(devNames[dn]&&!s[nameMap[dn].compiled])uncompiled.push(nameMap[dn].label);\n"
    "        }\n"
    "        if(uncompiled.length){\n"
    "          var banner=document.createElement('div');\n"
    "          banner.style.cssText='grid-column:1/-1;background:rgba(255,193,7,0.12);border:1px solid rgba(255,193,7,0.4);border-radius:8px;padding:1rem 1.25rem;margin-bottom:0.5rem;color:#ffc107';\n"
    "          banner.innerHTML='<div style=\"font-weight:600;margin-bottom:0.35rem\">Detected but not compiled</div>'\n"
    "            +'<div style=\"color:rgba(255,255,255,0.8);font-size:0.9rem\">The following sensors were found on the I2C bus but are not included in this firmware build: <strong style=\"color:#ffc107\">'+uncompiled.join(', ')+'</strong>.</div>'\n"
    "            +'<div style=\"color:rgba(255,255,255,0.55);font-size:0.82rem;margin-top:0.35rem\">Enable the corresponding CUSTOM_ENABLE_* flags in System_BuildConfig.h and rebuild.</div>';\n"
    "          grid.insertBefore(banner,grid.firstChild);\n"
    "        }\n"
    "      }catch(_){ }\n"
    "      return r;\n"
    "    };\n"
    "    return true;\n"
    "  }\n"
    "  tryPatch();\n"
    "  window.addEventListener('load',tryPatch);\n"
    "  setInterval(tryPatch,500);\n"
    "})();</script>",
    HTTPD_RESP_USE_STRLEN);

 #if ENABLE_WEB_SENSORS
  // Register dashboard sensor card definitions from per-sensor web modules
  httpd_resp_send_chunk(req, "<script>window.__dashSensorDefs=window.__dashSensorDefs||[];</script>", HTTPD_RESP_USE_STRLEN);
 #if ENABLE_IMU_SENSOR
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  streamBNO055ImuDashboardDef(req);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
 #endif
 #if ENABLE_THERMAL_SENSOR
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  streamMLX90640ThermalDashboardDef(req);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
 #endif
 #if ENABLE_TOF_SENSOR
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  streamVL53L4CXTofDashboardDef(req);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
 #endif
 #if ENABLE_GAMEPAD_SENSOR
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  streamSeesawGamepadDashboardDef(req);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
 #endif
 #if ENABLE_GPS_SENSOR
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  streamPA1010DGpsDashboardDef(req);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
 #endif
 #if ENABLE_RTC_SENSOR
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  streamDS3231RtcDashboardDef(req);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
 #endif
 #if ENABLE_PRESENCE_SENSOR
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  streamSTHS34PF80PresenceDashboardDef(req);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
 #endif
 #if ENABLE_FM_RADIO
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  streamRDA5807FmRadioDashboardDef(req);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
 #endif
 #if ENABLE_CAMERA_SENSOR
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  streamCameraDashboardDef(req);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
 #endif
 #if ENABLE_MICROPHONE_SENSOR
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  streamMicrophoneDashboardDef(req);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
 #endif
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  streamPCA9685ServoDriverDashboardDef(req);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
 #endif

  // Combined Status Panel (Sensor Status + System Stats)
  httpd_resp_send_chunk(req, "<div style='margin:2rem 0'>", HTTPD_RESP_USE_STRLEN);

  // Sensor Status Overview
  httpd_resp_send_chunk(req, "<div style='display:flex;align-items:center;gap:0.75rem'><h3 style='margin:0'>Sensor Status</h3><button onclick='window.Dash.openLayoutEditor(\"sensor-grid\")' style='background:none;border:1px solid var(--border);color:var(--muted);border-radius:4px;padding:2px 8px;font-size:0.75rem;cursor:pointer'>Customize</button></div>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<div id='sensor-loading' style='text-align:center;padding:2rem;color:#87ceeb'>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<div style='font-size:1.1rem;margin-bottom:0.5rem'>Loading sensor status...</div>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<div style='font-size:0.9rem;opacity:0.7'>Checking connected sensors</div>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<div class='sensor-status-grid' id='sensor-grid' style='display:none;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:1rem;margin:1rem 0'>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

  // System Stats Section (within same panel)
  httpd_resp_send_chunk(req, "<div style='display:flex;align-items:center;gap:0.75rem;margin-top:2rem'><h3 style='margin:0'>System Status</h3><button onclick='window.Dash.openLayoutEditor(\"system-grid\")' style='background:none;border:1px solid var(--border);color:var(--muted);border-radius:4px;padding:2px 8px;font-size:0.75rem;cursor:pointer'>Customize</button></div>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<div class='system-grid' id='system-grid' style='display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:1rem;margin:1rem 0;visibility:hidden'>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req,
    "<div class='sys-card sys-card-tall' data-panel='time'>"
    "<div style='font-weight:bold;margin-bottom:0.5rem;color:rgba(255,255,255,0.9)'>Date and Time</div>"
    "<div class='sys-card-row'><span>System:</span><strong id='sys-time'>--</strong></div>"
    "<div class='sys-card-row'><span>Uptime:</span><strong id='sys-uptime'>--</strong></div>"
    "</div>",
    HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req,
    "<div class='sys-card sys-card-tall' data-panel='memory'>"
    "<div style='font-weight:bold;margin-bottom:0.5rem;color:rgba(255,255,255,0.9)'>Memory</div>"
    "<div class='sys-card-row'><span>Heap:</span><strong id='sys-heap'>--</strong></div>"
    "<div class='sys-card-row'><span>PSRAM:</span><strong id='sys-psram'>--</strong></div>"
    "</div>",
    HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req,
    "<div class='sys-card sys-card-tall' data-panel='storage'>"
    "<div style='font-weight:bold;margin-bottom:0.5rem;color:rgba(255,255,255,0.9)'>Storage</div>"
    "<div class='sys-card-row'><span>Onboard Flash:</span><strong id='sys-storage-used'>--</strong></div>"
    "<div class='sys-card-row' id='sys-sd-row' style='display:none'><span>SD Card:</span><strong id='sys-storage-sd'>--</strong></div>"
    "</div>",
    HTTPD_RESP_USE_STRLEN);
  if (isAdminUser(username)) {
    httpd_resp_send_chunk(req,
      "<div class='sys-card sys-card-tall' id='sys-signedin-card' data-panel='users'>"
      "<div style='font-weight:bold;margin-bottom:0.5rem;color:rgba(255,255,255,0.9)'>Users</div>"
      "<div class='sys-card-row'><span>Signed in:</span><strong id='sys-signedin'>--</strong></div>"
      "</div>",
      HTTPD_RESP_USE_STRLEN);
  }
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN); // end system-grid

#if ENABLE_WIFI || ENABLE_ESPNOW || (ENABLE_WIFI && ENABLE_MQTT) || ENABLE_BLUETOOTH
  httpd_resp_send_chunk(req, "<div style='display:flex;align-items:center;gap:0.75rem;margin-top:2rem'><h3 style='margin:0'>Connectivity Status</h3><button onclick='window.Dash.openLayoutEditor(\"conn-grid\")' style='background:none;border:1px solid var(--border);color:var(--muted);border-radius:4px;padding:2px 8px;font-size:0.75rem;cursor:pointer'>Customize</button></div>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<div id='conn-grid' style='display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:1rem;margin:1rem 0;visibility:hidden'>", HTTPD_RESP_USE_STRLEN);

#if ENABLE_WIFI
  httpd_resp_send_chunk(req,
    "<div class='sys-card sys-card-tall' id='conn-wifi-card' data-panel='wifi'>"
    "<div style='font-weight:bold;margin-bottom:0.5rem;color:rgba(255,255,255,0.9);display:flex;align-items:center;gap:0.5rem'>"
    "<span class='status-indicator status-disabled' id='conn-wifi-dot'></span>WiFi</div>"
    "<div class='sys-card-row'><span>SSID:</span><strong id='sys-ssid'>--</strong></div>"
    "<div class='sys-card-row'><span>IP:</span><strong id='sys-ip'>--</strong></div>"
    "<div class='sys-card-row'><span>Channel:</span><strong id='conn-wifi-channel'>--</strong></div>"
    "<div class='sys-card-row'><span>MAC:</span><strong id='conn-wifi-mac' style='font-size:0.8em'>--</strong></div>"
    "</div>",
    HTTPD_RESP_USE_STRLEN);
#endif

#if ENABLE_ESPNOW
  httpd_resp_send_chunk(req,
    "<div class='sys-card sys-card-tall' id='conn-espnow-card' data-panel='espnow'>"
    "<div style='font-weight:bold;margin-bottom:0.5rem;color:rgba(255,255,255,0.9);display:flex;align-items:center;gap:0.5rem'>"
    "<span class='status-indicator status-disabled' id='conn-espnow-dot'></span>ESP-NOW</div>"
    "<div class='sys-card-row'><span>Status:</span><strong id='conn-espnow-status'>--</strong></div>"
    "<div class='sys-card-row'><span>Device Name:</span><strong id='conn-espnow-name'>--</strong></div>"
    "<div class='sys-card-row'><span>Mode:</span><strong id='conn-espnow-mesh'>--</strong></div>"
    "<div class='sys-card-row'><span>Passphrase:</span><strong id='conn-espnow-encrypted'>--</strong></div>"
    "</div>",
    HTTPD_RESP_USE_STRLEN);
#endif

#if ENABLE_ESPNOW && ENABLE_BONDED_MODE
  httpd_resp_send_chunk(req,
    "<div class='sys-card sys-card-tall' id='conn-bond-card' data-panel='bond'>"
    "<div style='font-weight:bold;margin-bottom:0.5rem;color:rgba(255,255,255,0.9);display:flex;align-items:center;gap:0.5rem'>"
    "<span class='status-indicator status-disabled' id='conn-bond-dot'></span>Bond</div>"
    "<div class='sys-card-row'><span>Enabled:</span><strong id='conn-bond-enabled'>--</strong></div>"
    "<div class='sys-card-row'><span>Role:</span><strong id='conn-bond-role'>--</strong></div>"
    "<div class='sys-card-row'><span>Peer:</span><strong id='conn-bond-peer'>--</strong></div>"
    "<div class='sys-card-row'><span>Online:</span><strong id='conn-bond-online'>--</strong></div>"
    "<div class='sys-card-row'><span>Synced:</span><strong id='conn-bond-synced'>--</strong></div>"
    "</div>",
    HTTPD_RESP_USE_STRLEN);
#endif

#if ENABLE_WIFI && ENABLE_MQTT
  httpd_resp_send_chunk(req,
    "<div class='sys-card sys-card-tall' id='conn-mqtt-card' data-panel='mqtt'>"
    "<div style='font-weight:bold;margin-bottom:0.5rem;color:rgba(255,255,255,0.9);display:flex;align-items:center;gap:0.5rem'>"
    "<span class='status-indicator status-disabled' id='conn-mqtt-dot'></span>MQTT</div>"
    "<div class='sys-card-row'><span>Enabled:</span><strong id='conn-mqtt-enabled'>--</strong></div>"
    "<div class='sys-card-row'><span>Connected:</span><strong id='conn-mqtt-connected'>--</strong></div>"
    "<div class='sys-card-row'><span>Broker:</span><strong id='conn-mqtt-host'>--</strong></div>"
    "</div>",
    HTTPD_RESP_USE_STRLEN);
#endif

#if ENABLE_BLUETOOTH
  httpd_resp_send_chunk(req,
    "<div class='sys-card sys-card-tall' id='conn-bt-card' data-panel='bluetooth'>"
    "<div style='font-weight:bold;margin-bottom:0.5rem;color:rgba(255,255,255,0.9);display:flex;align-items:center;gap:0.5rem'>"
    "<span class='status-indicator status-disabled' id='conn-bt-dot'></span>Bluetooth</div>"
    "<div class='sys-card-row'><span>Running:</span><strong id='conn-bt-running'>--</strong></div>"
    "<div class='sys-card-row'><span>State:</span><strong id='conn-bt-state'>--</strong></div>"
    "</div>",
    HTTPD_RESP_USE_STRLEN);
#endif

#if ENABLE_HTTP_SERVER
  httpd_resp_send_chunk(req,
    "<div class='sys-card sys-card-tall' id='conn-webserver-card' data-panel='webserver'>"
    "<div style='font-weight:bold;margin-bottom:0.5rem;color:rgba(255,255,255,0.9);display:flex;align-items:center;gap:0.5rem'>"
    "<span class='status-indicator status-disabled' id='conn-ws-dot'></span>Web Server"
    "<span id='https-badge' style='display:none;margin-left:auto;font-size:0.75em;padding:2px 6px;background:rgba(255,255,255,0.08);border:1px solid var(--border);border-radius:4px;color:var(--panel-fg)'>HTTPS</span>"
    "</div>"
    "<div class='sys-card-row'><span>Status:</span><strong id='conn-ws-status'>--</strong></div>"
    "<div class='sys-card-row'><span>Protocol:</span><strong id='conn-ws-proto'>--</strong></div>"
    "<div class='sys-card-row'><span>Port:</span><strong id='conn-ws-port'>--</strong></div>"
    "<div class='sys-card-row'><span>Sessions:</span><strong id='conn-ws-sessions'>--</strong></div>"
    "</div>",
    HTTPD_RESP_USE_STRLEN);
#endif

  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN); // end conn-grid
#endif // any connectivity feature

  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN); // End combined status panel

  // CSS for dashboard-specific indicators
  httpd_resp_send_chunk(req, "<style>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, ".status-indicator{display:inline-block;width:12px;height:12px;min-width:12px;min-height:12px;flex:0 0 12px;border-radius:50%;margin-right:8px;box-sizing:content-box;vertical-align:middle}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, ".status-enabled{background:#28a745;animation:pulse 2s infinite}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, ".status-disabled{background:#dc3545}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, ".status-recording{background:#e74c3c;animation:blink 1s infinite}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "@keyframes pulse{0%{opacity:1}50%{opacity:0.5}100%{opacity:1}}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "@keyframes blink{0%{opacity:1}50%{opacity:0.3}100%{opacity:1}}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</style>", HTTPD_RESP_USE_STRLEN);

  // JS sections (identical to legacy)
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 1: Pre-script sentinel');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 2: Starting core object definition');(function(){console.log('[Dashboard] Section 2a: Inside IIFE wrapper');const Dash={log:function(){try{console.log.apply(console,arguments)}catch(_){ }},setText:function(id,v){var el=document.getElementById(id);if(el)el.textContent=v}};console.log('[Dashboard] Section 2b: Basic Dash object created');window.Dash=Dash;})();</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 3: Adding indicator functions');if(window.Dash){window.Dash.setIndicator=function(id,on){var el=document.getElementById(id);if(el){el.className=on?'status-indicator status-enabled':'status-indicator status-disabled'}};console.log('[Dashboard] Section 3a: setIndicator added')}else{console.error('[Dashboard] Section 3: Dash object not found!')}</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 4: Adding sensor status functions');if(window.Dash){window.Dash.updateSensorStatus=function(d){if(!d)return;try{var imuOn=!!(d.imuEnabled||d.imu);var thermOn=!!(d.thermalEnabled||d.thermal);var tofOn=!!(d.tofEnabled||d.tof);var apdsOn=!!(d.apdsColorEnabled||d.apdsProximityEnabled||d.apdsGestureEnabled);var gameOn=!!(d.gamepadEnabled||d.gamepad);var pwmOn=!!(d.pwmDriverConnected);var gpsOn=!!(d.gpsEnabled);var fmOn=!!(d.fmRadioEnabled);window.Dash.setIndicator('dash-imu-status',imuOn);window.Dash.setIndicator('dash-thermal-status',thermOn);window.Dash.setIndicator('dash-tof-status',tofOn);window.Dash.setIndicator('dash-apds-status',apdsOn);window.Dash.setIndicator('dash-gamepad-status',gameOn);window.Dash.setIndicator('dash-pwm-status',pwmOn);window.Dash.setIndicator('dash-gps-status',gpsOn);window.Dash.setIndicator('dash-fmradio-status',fmOn);window.Dash.setIndicator('dash-mic-status',!!(d.micEnabled));var micRec=document.getElementById('dash-mic-recording');if(micRec){micRec.className=(d.micRecording)?'status-indicator status-recording':'status-indicator status-disabled'}}catch(e){console.warn('[Dashboard] Sensor status update error',e)}};window.Dash.updateDeviceVisibility=function(registry){if(!registry||!registry.devices)return;try{var devices=registry.devices;var hasIMU=devices.some(function(d){return d.name==='BNO055'});var hasThermal=devices.some(function(d){return d.name==='MLX90640'});var hasToF=devices.some(function(d){return d.name==='VL53L4CX'});var hasAPDS=devices.some(function(d){return d.name==='APDS9960'});var hasGamepad=devices.some(function(d){return d.name==='Seesaw'});var hasDRV=devices.some(function(d){return d.name==='DRV2605'});var hasPCA9685=devices.some(function(d){return d.name==='PCA9685'});var hasGPS=devices.some(function(d){return d.name==='PA1010D'});var hasFMRadio=devices.some(function(d){return d.name==='RDA5807'});window.Dash.showHideCard('dash-imu-card',hasIMU);window.Dash.showHideCard('dash-thermal-card',hasThermal);window.Dash.showHideCard('dash-tof-card',hasToF);window.Dash.showHideCard('dash-apds-card',hasAPDS);window.Dash.showHideCard('dash-gamepad-card',hasGamepad);window.Dash.showHideCard('dash-drv-card',hasDRV);window.Dash.showHideCard('dash-pwm-card',hasPCA9685);window.Dash.showHideCard('dash-gps-card',hasGPS);window.Dash.showHideCard('dash-fmradio-card',hasFMRadio)}catch(e){console.warn('[Dashboard] Device visibility update error',e)}};console.log('[Dashboard] Section 4a: updateSensorStatus and updateDeviceVisibility added')}else{console.error('[Dashboard] Section 4: Dash object not found!')}</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 5: Adding system status functions');if(window.Dash){window.Dash.updateSystem=function(d){try{if(!d)return;if(d.system_time)window.Dash.setText('sys-time',d.system_time);else window.Dash.setText('sys-time','Not synced');if(d.uptime_hms)window.Dash.setText('sys-uptime',d.uptime_hms);if(d.net){if(d.net.ssid!=null)window.Dash.setText('sys-ssid',d.net.ssid);if(d.net.ip!=null)window.Dash.setText('sys-ip',d.net.ip)}if(d.mem){var heapTxt=null;if(d.mem.heap_free_kb!=null){if(d.mem.heap_total_kb!=null){heapTxt=d.mem.heap_free_kb+'/'+d.mem.heap_total_kb+' KB'}else{heapTxt=d.mem.heap_free_kb+' KB'}}if(heapTxt!=null)window.Dash.setText('sys-heap',heapTxt);var psTxt=null;var hasPs=(d.mem.psram_free_kb!=null)||(d.mem.psram_total_kb!=null);if(hasPs){var pf=(d.mem.psram_free_kb!=null)?d.mem.psram_free_kb:null;var pt=(d.mem.psram_total_kb!=null)?d.mem.psram_total_kb:null;if(pf!=null&&pt!=null)psTxt=pf+'/'+pt+' KB';else if(pf!=null)psTxt=pf+' KB'}if(psTxt!=null)window.Dash.setText('sys-psram',psTxt)}if(d.storage){if(d.storage.used_kb!=null){var usedTxt=d.storage.used_kb;if(d.storage.total_kb!=null)usedTxt+=' / '+d.storage.total_kb+' KB';window.Dash.setText('sys-storage-used',usedTxt)}if(d.storage.sd){var sd=d.storage.sd;var sdTxt=sd.used_mb+' / '+sd.total_mb+' MB';window.Dash.setText('sys-storage-sd',sdTxt);var sdRow=document.getElementById('sys-sd-row');if(sdRow)sdRow.style.display='';}else{var sdRow=document.getElementById('sys-sd-row');if(sdRow)sdRow.style.display='none';}}}catch(e){console.warn('[Dashboard] System update error',e)}};console.log('[Dashboard] Section 5a: updateSystem added')}else{console.error('[Dashboard] Section 5: Dash object not found!')}</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>"
    "if(window.Dash){"
      "var _origUpdateSystem=window.Dash.updateSystem||function(){};"
      "window.Dash.updateSystem=function(d){"
        "_origUpdateSystem(d);"
        "try{"
          "if(!d||!d.connectivity)return;"
          "var c=d.connectivity;"
          "if(d.net){var wifiUp=!!(d.net.ip&&d.net.ip!='0.0.0.0');window.Dash.setIndicator('conn-wifi-dot',wifiUp);if(d.net.channel)window.Dash.setText('conn-wifi-channel',d.net.channel);if(d.net.mac)window.Dash.setText('conn-wifi-mac',d.net.mac);var badge=document.getElementById('https-badge');if(badge){badge.style.display=(location.protocol==='https:')?'inline-block':'none';}}"
          "if(c.espnow){"
            "var en=c.espnow;"
            "window.Dash.setIndicator('conn-espnow-dot',!!en.running);"
            "window.Dash.setText('conn-espnow-status',en.running?'Running':(en.enabled?'Stopped':'Disabled'));"
            "window.Dash.setText('conn-espnow-name',en.deviceName||'--');"
            "window.Dash.setText('conn-espnow-mesh',en.mesh?'Mesh':'Direct');"
          "window.Dash.setText('conn-espnow-encrypted',en.passphraseSet?'Set':'Not Set');"
          "}"
          "if(c.bond){"
            "var b=c.bond;"
            "window.Dash.setIndicator('conn-bond-dot',!!(b.enabled&&b.synced));"
            "window.Dash.setText('conn-bond-enabled',b.enabled?'Yes':'No');"
            "window.Dash.setText('conn-bond-role',['Worker','Master','Backup'][b.role]||('Role '+b.role));"
            "window.Dash.setText('conn-bond-peer',b.peer||'None');"
            "window.Dash.setText('conn-bond-online',b.online?'Yes':'No');"
            "window.Dash.setText('conn-bond-synced',b.synced?'Yes':'No');"
          "}"
          "if(c.mqtt){"
            "var m=c.mqtt;"
            "window.Dash.setIndicator('conn-mqtt-dot',!!m.connected);"
            "window.Dash.setText('conn-mqtt-enabled',m.enabled?'Yes':'No');"
            "window.Dash.setText('conn-mqtt-connected',m.connected?'Connected':'Disconnected');"
            "window.Dash.setText('conn-mqtt-host',m.host||'--');"
          "}"
          "if(c.bluetooth){"
            "var bt=c.bluetooth;"
            "window.Dash.setIndicator('conn-bt-dot',!!bt.running);"
            "window.Dash.setText('conn-bt-running',bt.running?'Yes':'No');"
            "window.Dash.setText('conn-bt-state',bt.state||'--');"
          "}"
          "if(c.webserver){"
            "var ws=c.webserver;"
            "window.Dash.setIndicator('conn-ws-dot',!!ws.running);"
            "window.Dash.setText('conn-ws-status',ws.running?'Running':'Stopped');"
            "window.Dash.setText('conn-ws-proto',ws.running?(ws.https?'HTTPS':'HTTP'):'--');"
            "window.Dash.setText('conn-ws-port',ws.running?String(ws.port):'--');"
            "window.Dash.setText('conn-ws-sessions',(ws.sessions!=null)?(ws.sessions+'/'+ws.maxSessions):'--');"
          "}"
        "}catch(e){console.warn('[Dashboard] Connectivity update error',e);}"
      "};"
    "}"
    "</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 6: Setting up global variables');window.__sensorStatusSeq=0;window.__probeCooldownMs=10000;window.__lastProbeAt=0;console.log('[Dashboard] Section 6a: Global variables set');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 7a: Adding applySensorStatus function');window.applySensorStatus=function(s){console.log('[Dashboard] applySensorStatus called with:',s);if(!s)return;window.__sensorStatusSeq=s.seq||0;window.__lastSensorStatus=s;};console.log('[Dashboard] Section 7a: applySensorStatus function added');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 7b: Adding sensor card creation');window.createSensorCards=function(sensorStatus,deviceRegistry){console.log('[Dashboard] createSensorCards called with status:',sensorStatus,'registry:',deviceRegistry);var loading=document.getElementById('sensor-loading');var grid=document.getElementById('sensor-grid');if(loading)loading.style.display='none';if(grid){grid.style.display='grid';grid.innerHTML=''}var availableSensors=window.getAvailableSensors(deviceRegistry);console.log('[Dashboard] Available sensors from getAvailableSensors:',availableSensors);var cardCount=0;for(var i=0;i<availableSensors.length;i++){var sensor=availableSensors[i];var enabled=window.getSensorEnabled(sensor.key,sensorStatus);var card=document.createElement('div');card.className='sensor-status-card';card.id='dash-'+sensor.key+'-card';card.dataset.panel=sensor.key;card.style.cssText='background:rgba(255,255,255,0.1);border-radius:8px;padding:1rem;border:1px solid rgba(255,255,255,0.2)';var statusText=enabled?'Running':'Available';var statusColor=enabled?'#28a745':'#87ceeb';var dotsHtml='';if(sensor&&sensor.key==='mic'){var enabledClass=enabled?'status-enabled':'status-disabled';var recordingClass=(sensorStatus&&sensorStatus.micRecording)?'status-recording':'status-disabled';dotsHtml='<span class=\"status-indicator '+enabledClass+'\" id=\"dash-mic-status\"></span><span class=\"status-indicator '+recordingClass+'\" id=\"dash-mic-recording\" style=\"margin-left:4px\"></span>';if(sensorStatus&&sensorStatus.micRecording){statusText='Recording';statusColor='#e74c3c'}}else if(sensor&&sensor.key==='camera'){var enabledClass=enabled?'status-enabled':'status-disabled';var streamClass=(sensorStatus&&sensorStatus.cameraStreaming)?'status-recording':'status-disabled';var mlClass=(sensorStatus&&sensorStatus.eiEnabled)?'status-enabled':'status-disabled';dotsHtml='<span class=\"status-indicator '+enabledClass+'\" id=\"dash-camera-status\" title=\"Enabled\"></span><span class=\"status-indicator '+streamClass+'\" id=\"dash-camera-stream\" title=\"Streaming\" style=\"margin-left:4px\"></span><span class=\"status-indicator '+mlClass+'\" id=\"dash-camera-ml\" title=\"ML Inference\" style=\"margin-left:4px\"></span>'}else if(sensor&&sensor.key==='gamepad'){var enabledClass=enabled?'status-enabled':'status-disabled';dotsHtml='<span class=\"status-indicator '+enabledClass+'\" id=\"dash-gamepad-status\" style=\"margin-right:0.5rem\"></span>'}else{var statusClass=enabled?'status-enabled':'status-disabled';dotsHtml='<span class=\"status-indicator '+statusClass+'\" id=\"dash-'+sensor.key+'-status\"></span>'}card.innerHTML='<div style=\"display:flex;align-items:center;gap:0.5rem;margin-bottom:0.25rem\">'+dotsHtml+'<strong style=\"line-height:1.2\">'+sensor.name+'</strong></div>'+'<div style=\"font-size:0.85rem;opacity:0.8;margin-bottom:0.5rem\">'+sensor.desc+'</div>'+'<div style=\"font-size:0.9rem;color:'+statusColor+'\">'+statusText+'</div>';if(grid)grid.appendChild(card);cardCount++}if(cardCount===0&&grid){grid.innerHTML='<div style=\"grid-column:1/-1;text-align:center;padding:2rem;color:#87ceeb;font-style:italic\">No sensors are currently available.</div>'}console.log('[Dashboard] Created '+cardCount+' sensor cards')};console.log('[Dashboard] Section 7b: createSensorCards added');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 7c: Adding helper functions');window.getAvailableSensors=function(deviceRegistry){var sensors=[];var defs=window.__dashSensorDefs||[];var seen={};if(deviceRegistry&&deviceRegistry.devices){for(var di=0;di<deviceRegistry.devices.length;di++){var dev=deviceRegistry.devices[di];for(var i=0;i<defs.length;i++){var d=defs[i];if(!d||!d.device||!d.key)continue;if(d.device===dev.name){if(!seen[d.key]){seen[d.key]=1;sensors.push({key:d.key,name:d.name,desc:d.desc})}}}}}var status=window.__lastSensorStatus||{};for(var i=0;i<defs.length;i++){var d=defs[i];if(!d||!d.key)continue;if(d.key==='camera'&&status.cameraCompiled&&!seen['camera']){seen['camera']=1;sensors.push({key:'camera',name:d.name||'Camera (DVP)',desc:d.desc||'ESP32-S3 DVP Camera'})}}return sensors};window.getSensorEnabled=function(key,status){if(!status)return false;switch(key){case'imu':return !!status.imuEnabled;case'thermal':return !!status.thermalEnabled;case'tof':return !!status.tofEnabled;case'apds':return !!(status.apdsColorEnabled||status.apdsProximityEnabled||status.apdsGestureEnabled);case'gamepad':return !!status.gamepadEnabled;case'haptic':return !!status.hapticEnabled;case'pwm':return !!status.pwmDriverConnected;case'gps':return !!status.gpsEnabled;case'fmradio':return !!status.fmRadioEnabled;case'camera':return !!status.cameraEnabled;case'mic':return !!status.micEnabled;case'rtc':return !!status.rtcEnabled;case'presence':return !!status.presenceEnabled;default:return false}};window.Dash.showHideCard=function(cardId,show){var c=document.getElementById(cardId);if(c)c.style.display=show?'':'none'};console.log('[Dashboard] Section 7c: Helper functions added');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 7d: Updating applySensorStatus to use helpers');window.__deviceRegistry=null;window.applySensorStatus=function(s){console.log('[Dashboard] applySensorStatus called with:',s);if(!s)return;window.__sensorStatusSeq=s.seq||0;if(window.__deviceRegistry){console.log('[Dashboard] Using cached device registry:',window.__deviceRegistry);window.createSensorCards(s,window.__deviceRegistry)}else{console.log('[Dashboard] Device registry not loaded yet, fetching...');window.fetchDeviceRegistry().then(function(registry){console.log('[Dashboard] Fetch complete, calling createSensorCards with:',registry);window.createSensorCards(s,registry||window.__deviceRegistry)})}if(window.Dash)window.Dash.updateSensorStatus(s)};window.fetchDeviceRegistry=function(){console.log('[Dashboard] fetchDeviceRegistry called');return fetch('/api/devices',{credentials:'include',cache:'no-store'}).then(function(r){console.log('[Dashboard] Device registry fetch response:',r.status);return r.json()}).then(function(d){console.log('[Dashboard] Setting window.__deviceRegistry to:',d);window.__deviceRegistry=d;console.log('[Dashboard] Device registry loaded and stored:',window.__deviceRegistry);if(window.Dash)window.Dash.updateDeviceVisibility(d);return d}).catch(function(e){console.warn('[Dashboard] Device registry fetch failed:',e);return null})};console.log('[Dashboard] Section 7d: applySensorStatus updated');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 7e: Adding fetchSensorStatus');window.fetchSensorStatus=function(){console.log('[Dashboard] Fetching sensor status...');return fetch('/api/sensors/status',{credentials:'include',cache:'no-store'}).then(function(r){console.log('[Dashboard] Sensor status response:',r.status);if(r.status===404){console.log('[Dashboard] Sensor endpoints not available (sensors disabled)');window.applySensorStatus({sensorsDisabled:true});return}return r.json()}).then(function(j){if(!j)return;console.log('[Dashboard] Raw sensor status data:',JSON.stringify(j,null,2));console.log('[Dashboard] Individual sensor states:');console.log('  - imuEnabled:',j.imuEnabled);console.log('  - thermalEnabled:',j.thermalEnabled);console.log('  - tofEnabled:',j.tofEnabled);console.log('  - apdsColorEnabled:',j.apdsColorEnabled);window.applySensorStatus(j)}).catch(function(e){console.warn('[Dashboard] sensor status fetch failed (sensors may be disabled)',e);window.applySensorStatus({sensorsDisabled:true})})};console.log('[Dashboard] Section 7e: fetchSensorStatus added');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 8: Adding SSE functions');window.createSSEIfNeeded=function(){try{console.log('[Dashboard] Creating SSE connection...');if(!window.EventSource){console.warn('[Dashboard] EventSource not supported');return false}if(window.__es){var rs=-1;try{if(typeof window.__es.readyState!=='undefined')rs=window.__es.readyState}catch(_){}console.log('[Dashboard] Existing SSE readyState:',rs);if(rs===2){console.log('[Dashboard] Closing existing SSE connection');try{window.__es.close()}catch(_){}window.__es=null}}if(window.__es){console.log('[Dashboard] Using existing SSE connection');return true}console.log('[Dashboard] Opening new SSE to /api/events');var es=new EventSource('/api/events', { withCredentials: true });es.onopen=function(){console.log('[Dashboard] SSE connection opened')};es.onerror=function(e){console.warn('[Dashboard] SSE error:',e);try{es.close()}catch(_){}window.__es=null};window.__es=es;return true}catch(e){console.error('[Dashboard] SSE creation failed:',e);return false}};console.log('[Dashboard] Section 8a: createSSEIfNeeded added');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 9: Adding SSE attachment');window.attachSSE=function(){try{console.log('[Dashboard] Attaching SSE event listeners...');if(!window.__es){console.warn('[Dashboard] No SSE connection to attach to');return false}var handler=function(e){try{console.log('[Dashboard] Received sensor-status event:',e.data);var dj=JSON.parse(e.data||'{}');var seq=(dj&&dj.seq)?dj.seq:0;var cur=window.__sensorStatusSeq||0;if(seq<=cur)return;window.__sensorStatusSeq=seq;if(window.applySensorStatus)window.applySensorStatus(dj)}catch(err){console.warn('[Dashboard] SSE sensor-status parse error',err)}};window.__es.addEventListener('sensor-status',handler);console.log('[Dashboard] Added sensor-status listener');window.__es.addEventListener('system',function(e){try{console.log('[Dashboard] Received system event:',e.data);var dj=JSON.parse(e.data||'{}');if(window.Dash){console.log('[Dashboard] Calling updateSystem with:',dj);window.Dash.updateSystem(dj)}else{console.warn('[Dashboard] Dash object not available for system update')}}catch(err){console.warn('[Dashboard] SSE system parse error',err)}});console.log('[Dashboard] Added system listener');return true}catch(e){console.error('[Dashboard] SSE attachment failed:',e);return false}};console.log('[Dashboard] Section 9a: attachSSE added');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 10: Adding utility functions');window.fetchSystemStatus=function(){console.log('[Dashboard] Fetching system status via API...');return fetch('/api/system',{credentials:'include',cache:'no-store'}).then(function(r){console.log('[Dashboard] System status response:',r.status);if(!r.ok)throw new Error('HTTP '+r.status);return r.json()}).then(function(j){console.log('[Dashboard] System status data:',j);if(window.Dash)window.Dash.updateSystem(j)}).catch(function(e){console.warn('[Dashboard] System status fetch failed:',e)})};window.setupSensorSSE=function(){console.log('[Dashboard] Setting up sensor-only SSE...');if(window.createSSEIfNeeded)window.createSSEIfNeeded();if(window.attachSSE)window.attachSSE()};console.log('[Dashboard] Section 10a: Utility functions added');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 10b: Adding signed-in users fetch');window.fetchSignedInUsers=function(){try{var card=document.getElementById('sys-signedin-card');if(!card)return;return fetch('/api/sessions',{credentials:'include',cache:'no-store'}).then(function(r){if(!r.ok)return r.text().then(function(t){throw new Error('HTTP '+r.status+' '+t)});return r.json()}).then(function(j){var users='--';try{if(j&&j.success===true&&Array.isArray(j.sessions)){var seen={};var list=[];for(var i=0;i<j.sessions.length;i++){var u=j.sessions[i]&&j.sessions[i].user?String(j.sessions[i].user):'';if(u&&!seen[u]){seen[u]=1;list.push(u)}}users=list.length?list.join(', '):'--'}}catch(_){users='--'}if(window.Dash)window.Dash.setText('sys-signedin',users)}).catch(function(e){console.log('[Dashboard] Sessions fetch failed:',e);if(window.Dash)window.Dash.setText('sys-signedin','--')})}catch(e){console.log('[Dashboard] fetchSignedInUsers error:',e)}};</script>", HTTPD_RESP_USE_STRLEN);
  // Dashboard layout customization (load/apply/save panel order per user)
  httpd_resp_send_chunk(req,
    "<style>"
    "#dash-layout-modal{display:none;position:fixed;inset:0;background:rgba(0,0,0,0.55);z-index:99999;align-items:center;justify-content:center}"
    "#dash-layout-modal.open{display:flex}"
    "#dash-layout-inner{background:var(--panel-bg);color:var(--panel-fg);border:1px solid var(--border);border-radius:8px;padding:1.5rem;min-width:320px;max-width:480px;width:90vw;box-shadow:0 8px 32px rgba(0,0,0,0.4)}"
    "#dash-layout-inner h4{margin:0 0 1rem 0;font-size:1.1rem;color:var(--panel-fg)}"
    ".layout-item{display:flex;align-items:center;gap:0.75rem;padding:0.6rem 1rem;margin:0.35rem 0;background:var(--crumb-bg);border:1px solid var(--border);border-radius:6px;font-size:0.95rem;color:var(--panel-fg)}"
    ".layout-item span{flex:1}"
    ".layout-item .layout-hidden{opacity:0.4;text-decoration:line-through}"
    ".layout-actions{display:flex;gap:0.5rem;justify-content:flex-end;margin-top:1rem}"
    "</style>"
    "<div id='dash-layout-modal'><div id='dash-layout-inner'>"
    "<h4 id='dash-layout-title'>Customize Layout</h4>"
    "<div id='dash-layout-list'></div>"
    "<div class='layout-actions'>"
    "<button class='btn' onclick='window.Dash.resetLayout()'>Reset</button>"
    "<button class='btn' onclick='window.Dash.closeLayoutEditor()'>Cancel</button>"
    "<button class='btn' onclick='window.Dash.saveLayout()'>Save</button>"
    "</div></div></div>",
    HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req,
    "console.log('[Dashboard] Section 10c: Layout customization');"
    "(function(){"
    "var _editingGridId=null;"
    "var _editOrder=[];"
    "var _editHidden={};"

    "function getPanelLabel(el){"
    "var title=el.querySelector('div[style*=\"font-weight\"]');"
    "if(title){"
    "var txt='';"
    "for(var i=0;i<title.childNodes.length;i++){"
    "if(title.childNodes[i].nodeType===3)txt+=title.childNodes[i].textContent;"
    "else if(title.childNodes[i].nodeType===1&&title.childNodes[i].classList&&title.childNodes[i].classList.contains('status-indicator'))continue;"
    "else if(title.childNodes[i].nodeType===1&&title.childNodes[i].id&&title.childNodes[i].id.indexOf('badge')>=0)continue;"
    "else if(title.childNodes[i].nodeType===1)txt+=title.childNodes[i].textContent;"
    "}"
    "return txt.trim();"
    "}"
    "return el.dataset.panel||'?';"
    "}"

    "function applyOrder(gridId,order){"
    "if(!order||!order.length)return;"
    "var grid=document.getElementById(gridId);"
    "if(!grid)return;"
    "var children=Array.prototype.slice.call(grid.children);"
    "var map={};"
    "for(var i=0;i<children.length;i++){"
    "var p=children[i].dataset.panel;"
    "if(p)map[p]=children[i];"
    "}"
    "for(var i=0;i<order.length;i++){"
    "if(map[order[i]])grid.appendChild(map[order[i]]);"
    "}"
    "for(var i=0;i<children.length;i++){"
    "if(children[i].dataset.panel&&order.indexOf(children[i].dataset.panel)===-1){"
    "grid.appendChild(children[i]);"
    "}}"
    "}"

    "function applyHidden(gridId,hidden){"
    "if(!hidden)return;"
    "var grid=document.getElementById(gridId);"
    "if(!grid)return;"
    "var ch=grid.children;"
    "for(var i=0;i<ch.length;i++){"
    "var p=ch[i].dataset.panel;"
    "if(p&&hidden[p])ch[i].style.display='none';"
    "}"
    "}"

    "function getCurrentOrder(gridId){"
    "var grid=document.getElementById(gridId);"
    "if(!grid)return[];"
    "var order=[];"
    "var children=grid.children;"
    "for(var i=0;i<children.length;i++){"
    "var p=children[i].dataset.panel;"
    "if(p)order.push(p);"
    "}"
    "return order;"
    "}"

    "function getGridSettingKeys(gridId){"
    "if(gridId==='system-grid')return{order:'dashboardSystemLayout',hidden:'dashboardSystemHidden'};"
    "if(gridId==='conn-grid')return{order:'dashboardConnLayout',hidden:'dashboardConnHidden'};"
    "return{order:'dashboardSensorLayout',hidden:'dashboardSensorHidden'};"
    "}"

    "function getGridTitle(gridId){"
    "if(gridId==='system-grid')return 'System Status';"
    "if(gridId==='conn-grid')return 'Connectivity';"
    "return 'Sensor Status';"
    "}"

    "function renderEditor(){"
    "var list=document.getElementById('dash-layout-list');"
    "if(!list)return;"
    "list.innerHTML='';"
    "var grid=document.getElementById(_editingGridId);"
    "if(!grid)return;"
    "var labelMap={};"
    "var ch=grid.children;"
    "for(var i=0;i<ch.length;i++){"
    "var p=ch[i].dataset.panel;"
    "if(p)labelMap[p]=getPanelLabel(ch[i]);"
    "}"
    "for(var i=0;i<_editOrder.length;i++){"
    "var key=_editOrder[i];"
    "var isHidden=!!_editHidden[key];"
    "var div=document.createElement('div');"
    "div.className='layout-item';"
    "var sp=document.createElement('span');"
    "sp.textContent=labelMap[key]||key;"
    "if(isHidden)sp.className='layout-hidden';"
    "div.appendChild(sp);"

    "var vis=document.createElement('button');"
    "vis.className='btn';"
    "vis.textContent=isHidden?'Show':'Hide';"
    "vis.style.width='auto';vis.style.padding='2px 10px';vis.style.fontSize='0.8rem';"
    "vis.onclick=(function(k){return function(){"
    "_editHidden[k]=!_editHidden[k];renderEditor();"
    "}})(key);"
    "div.appendChild(vis);"

    "var up=document.createElement('button');"
    "up.className='btn';up.textContent='Up';"
    "up.style.fontSize='0.8rem';up.style.width='auto';up.style.padding='2px 10px';"
    "up.disabled=(i===0);"
    "up.onclick=(function(idx){return function(){"
    "var tmp=_editOrder[idx-1];_editOrder[idx-1]=_editOrder[idx];_editOrder[idx]=tmp;renderEditor();"
    "}})(i);"
    "div.appendChild(up);"
    "var dn=document.createElement('button');"
    "dn.className='btn';dn.textContent='Down';"
    "dn.style.fontSize='0.8rem';dn.style.width='auto';dn.style.padding='2px 10px';"
    "dn.disabled=(i===_editOrder.length-1);"
    "dn.onclick=(function(idx){return function(){"
    "var tmp=_editOrder[idx+1];_editOrder[idx+1]=_editOrder[idx];_editOrder[idx]=tmp;renderEditor();"
    "}})(i);"
    "div.appendChild(dn);"
    "list.appendChild(div);"
    "}}"

    "window.Dash.openLayoutEditor=function(gridId){"
    "_editingGridId=gridId;"
    "_editOrder=getCurrentOrder(gridId);"
    "_editHidden={};"
    "var grid=document.getElementById(gridId);"
    "if(grid){var ch=grid.children;"
    "for(var i=0;i<ch.length;i++){var p=ch[i].dataset.panel;"
    "if(p&&ch[i].style.display==='none')_editHidden[p]=true;"
    "}}"
    "var title=document.getElementById('dash-layout-title');"
    "if(title)title.textContent=getGridTitle(gridId)+' Layout';"
    "renderEditor();"
    "var modal=document.getElementById('dash-layout-modal');"
    "if(modal)modal.classList.add('open');"
    "};"

    "window.Dash.closeLayoutEditor=function(){"
    "var modal=document.getElementById('dash-layout-modal');"
    "if(modal)modal.classList.remove('open');"
    "_editingGridId=null;_editOrder=[];_editHidden={};"
    "};"

    "window.Dash.saveLayout=function(){"
    "if(!_editingGridId)return;"
    "applyOrder(_editingGridId,_editOrder);"
    "applyHidden(_editingGridId,_editHidden);"
    "var grid=document.getElementById(_editingGridId);"
    "if(grid){var ch=grid.children;"
    "for(var i=0;i<ch.length;i++){var p=ch[i].dataset.panel;"
    "if(p&&!_editHidden[p])ch[i].style.display='';"
    "}}"
    "var keys=getGridSettingKeys(_editingGridId);"
    "var hiddenArr=[];"
    "for(var k in _editHidden){if(_editHidden[k])hiddenArr.push(k);}"
    "var patch={};"
    "patch[keys.order]=_editOrder;"
    "patch[keys.hidden]=hiddenArr.length?hiddenArr:null;"
    "fetch('/api/user/settings',{method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify(patch)})"
    ".then(function(r){return r.json()})"
    ".then(function(j){console.log('[Dashboard] Layout saved:',j);"
    "if(window.hw&&window.hw.notify)window.hw.notify('success','Dashboard layout saved')})"
    ".catch(function(e){console.warn('[Dashboard] Layout save failed:',e)});"
    "var desc=getGridTitle(_editingGridId)+': '+_editOrder.join(',');"
    "if(hiddenArr.length)desc+=' hidden:'+hiddenArr.join(',');"
    "fetch('/api/cli',{method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({command:'dashboard layout '+desc})})"
    ".catch(function(){});"
    "window.Dash.closeLayoutEditor();"
    "};"

    "window.Dash.resetLayout=function(){"
    "if(!_editingGridId)return;"
    "var keys=getGridSettingKeys(_editingGridId);"
    "var patch={};"
    "patch[keys.order]=null;"
    "patch[keys.hidden]=null;"
    "fetch('/api/user/settings',{method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify(patch)})"
    ".catch(function(e){console.warn('[Dashboard] Layout reset failed:',e)});"
    "fetch('/api/cli',{method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({command:'dashboard layout reset '+getGridTitle(_editingGridId)})})"
    ".catch(function(){});"
    "window.Dash.closeLayoutEditor();"
    "location.reload();"
    "};"

    "window.Dash.loadSavedLayouts=function(){"
    "return fetch('/api/user/settings',{credentials:'include',cache:'no-store'})"
    ".then(function(r){return r.json()})"
    ".then(function(j){"
    "if(!j||!j.success||!j.settings)return;"
    "var s=j.settings;"
    "if(s.dashboardSystemLayout&&Array.isArray(s.dashboardSystemLayout))"
    "applyOrder('system-grid',s.dashboardSystemLayout);"
    "if(s.dashboardSystemHidden&&Array.isArray(s.dashboardSystemHidden)){"
    "var h={};s.dashboardSystemHidden.forEach(function(k){h[k]=true;});"
    "applyHidden('system-grid',h);"
    "}"
    "if(s.dashboardConnLayout&&Array.isArray(s.dashboardConnLayout))"
    "applyOrder('conn-grid',s.dashboardConnLayout);"
    "if(s.dashboardConnHidden&&Array.isArray(s.dashboardConnHidden)){"
    "var h2={};s.dashboardConnHidden.forEach(function(k){h2[k]=true;});"
    "applyHidden('conn-grid',h2);"
    "}"
    "if(s.dashboardSensorLayout&&Array.isArray(s.dashboardSensorLayout))"
    "applyOrder('sensor-grid',s.dashboardSensorLayout);"
    "if(s.dashboardSensorHidden&&Array.isArray(s.dashboardSensorHidden)){"
    "var h3={};s.dashboardSensorHidden.forEach(function(k){h3[k]=true;});"
    "applyHidden('sensor-grid',h3);"
    "}"
    "})"
    ".catch(function(e){console.warn('[Dashboard] Layout load failed:',e)});"
    "};"

    "})();",
    HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 11: DOM initialization');document.addEventListener('DOMContentLoaded',function(){try{console.log('[Dashboard] Section 11a: DOM loaded, initializing...');var grids=['system-grid','conn-grid'];for(var gi=0;gi<grids.length;gi++){var g=document.getElementById(grids[gi]);if(g)g.style.visibility='hidden';}function revealGrids(){for(var gi=0;gi<grids.length;gi++){var g=document.getElementById(grids[gi]);if(g)g.style.visibility='';}}var layoutP=(window.Dash&&window.Dash.loadSavedLayouts)?window.Dash.loadSavedLayouts():Promise.resolve();if(layoutP&&typeof layoutP.then==='function'){layoutP.then(revealGrids).catch(revealGrids)}else{revealGrids()}if(window.fetchDeviceRegistry)window.fetchDeviceRegistry();if(window.fetchSensorStatus)window.fetchSensorStatus();if(window.fetchSystemStatus)window.fetchSystemStatus();if(window.fetchSignedInUsers&&document.getElementById('sys-signedin-card'))window.fetchSignedInUsers();if(window.createSSEIfNeeded)window.createSSEIfNeeded();if(window.attachSSE)window.attachSSE();try{if(window.__sessionsTimer){clearInterval(window.__sessionsTimer)}window.__sessionsTimer=setInterval(function(){if(window.fetchSignedInUsers&&document.getElementById('sys-signedin-card'))window.fetchSignedInUsers()},15000)}catch(_){ }console.log('[Dashboard] Section 11b: All initialization complete')}catch(e){console.error('[Dashboard] DOM init error',e)}});console.log('[Dashboard] Section 11c: DOM listener registered');</script>", HTTPD_RESP_USE_STRLEN);
}

#endif