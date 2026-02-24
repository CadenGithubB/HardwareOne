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
  // Header / greeting
  httpd_resp_send_chunk(req, "<h2>Dashboard</h2>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, (String("<p>Welcome, <strong>") + username + "</strong>.</p>").c_str(), HTTPD_RESP_USE_STRLEN);

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
  httpd_resp_send_chunk(req, "<h3>Sensor Status</h3>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<div id='sensor-loading' style='text-align:center;padding:2rem;color:#87ceeb'>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<div style='font-size:1.1rem;margin-bottom:0.5rem'>Loading sensor status...</div>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<div style='font-size:0.9rem;opacity:0.7'>Checking connected sensors</div>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<div class='sensor-status-grid' id='sensor-grid' style='display:none;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:1rem;margin:1rem 0'>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

  // System Stats Section (within same panel)
  httpd_resp_send_chunk(req, "<h3 style='margin-top:2rem'>System Stats</h3>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<div class='system-grid' style='display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:1rem;margin:1rem 0;grid-auto-rows:minmax(60px,auto)'>", HTTPD_RESP_USE_STRLEN);
  if (isAdminUser(username)) {
    httpd_resp_send_chunk(req, "  <div class='sys-card' id='sys-signedin-card'>Signed in users: <strong id='sys-signedin'>--</strong></div>", HTTPD_RESP_USE_STRLEN);
  }
  httpd_resp_send_chunk(req, "  <div class='sys-card'>Uptime: <strong id='sys-uptime'>--</strong></div>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "  <div class='sys-card sys-card-tall'><div style='font-weight:bold;margin-bottom:0.25rem;color:rgba(255,255,255,0.9)'>WiFi Network</div><div class='sys-card-row'><span>SSID:</span><strong id='sys-ssid'>--</strong></div><div class='sys-card-row'><span>IP:</span><strong id='sys-ip'>--</strong></div></div>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "  <div class='sys-card sys-card-tall'><div style='font-weight:bold;margin-bottom:0.25rem;color:rgba(255,255,255,0.9)'>Memory</div><div class='sys-card-row'><span>Heap:</span><strong id='sys-heap'>--</strong></div><div class='sys-card-row'><span>PSRAM:</span><strong id='sys-psram'>--</strong></div></div>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "  <div class='sys-card'>Storage Used: <strong id='sys-storage-used'>--</strong></div>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN); // end system-grid
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
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 5: Adding system status functions');if(window.Dash){window.Dash.updateSystem=function(d){try{if(!d)return;if(d.uptime_hms)window.Dash.setText('sys-uptime',d.uptime_hms);if(d.net){if(d.net.ssid!=null)window.Dash.setText('sys-ssid',d.net.ssid);if(d.net.ip!=null)window.Dash.setText('sys-ip',d.net.ip)}if(d.mem){var heapTxt=null;if(d.mem.heap_free_kb!=null){if(d.mem.heap_total_kb!=null){heapTxt=d.mem.heap_free_kb+'/'+d.mem.heap_total_kb+' KB'}else{heapTxt=d.mem.heap_free_kb+' KB'}}if(heapTxt!=null)window.Dash.setText('sys-heap',heapTxt);var psTxt=null;var hasPs=(d.mem.psram_free_kb!=null)||(d.mem.psram_total_kb!=null);if(hasPs){var pf=(d.mem.psram_free_kb!=null)?d.mem.psram_free_kb:null;var pt=(d.mem.psram_total_kb!=null)?d.mem.psram_total_kb:null;if(pf!=null&&pt!=null)psTxt=pf+'/'+pt+' KB';else if(pf!=null)psTxt=pf+' KB'}if(psTxt!=null)window.Dash.setText('sys-psram',psTxt)}if(d.storage){if(d.storage.used_kb!=null){var usedTxt=d.storage.used_kb+' KB';if(d.storage.total_kb!=null)usedTxt+=' / '+d.storage.total_kb+' KB';window.Dash.setText('sys-storage-used',usedTxt)}if(d.storage.free_kb!=null)window.Dash.setText('sys-storage-free',d.storage.free_kb+' KB')}}catch(e){console.warn('[Dashboard] System update error',e)}};console.log('[Dashboard] Section 5a: updateSystem added')}else{console.error('[Dashboard] Section 5: Dash object not found!')}</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 6: Setting up global variables');window.__sensorStatusSeq=0;window.__probeCooldownMs=10000;window.__lastProbeAt=0;console.log('[Dashboard] Section 6a: Global variables set');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 7a: Adding applySensorStatus function');window.applySensorStatus=function(s){console.log('[Dashboard] applySensorStatus called with:',s);if(!s)return;window.__sensorStatusSeq=s.seq||0;window.__lastSensorStatus=s;};console.log('[Dashboard] Section 7a: applySensorStatus function added');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 7b: Adding sensor card creation');window.createSensorCards=function(sensorStatus,deviceRegistry){console.log('[Dashboard] createSensorCards called with status:',sensorStatus,'registry:',deviceRegistry);var loading=document.getElementById('sensor-loading');var grid=document.getElementById('sensor-grid');if(loading)loading.style.display='none';if(grid){grid.style.display='grid';grid.innerHTML=''}var availableSensors=window.getAvailableSensors(deviceRegistry);console.log('[Dashboard] Available sensors from getAvailableSensors:',availableSensors);var cardCount=0;for(var i=0;i<availableSensors.length;i++){var sensor=availableSensors[i];var enabled=window.getSensorEnabled(sensor.key,sensorStatus);var card=document.createElement('div');card.className='sensor-status-card';card.id='dash-'+sensor.key+'-card';card.style.cssText='background:rgba(255,255,255,0.1);border-radius:8px;padding:1rem;border:1px solid rgba(255,255,255,0.2)';var statusText=enabled?'Running':'Available';var statusColor=enabled?'#28a745':'#87ceeb';var dotsHtml='';if(sensor&&sensor.key==='mic'){var enabledClass=enabled?'status-enabled':'status-disabled';var recordingClass=(sensorStatus&&sensorStatus.micRecording)?'status-recording':'status-disabled';dotsHtml='<span class=\"status-indicator '+enabledClass+'\" id=\"dash-mic-status\"></span><span class=\"status-indicator '+recordingClass+'\" id=\"dash-mic-recording\" style=\"margin-left:4px\"></span>';if(sensorStatus&&sensorStatus.micRecording){statusText='Recording';statusColor='#e74c3c'}}else if(sensor&&sensor.key==='camera'){var enabledClass=enabled?'status-enabled':'status-disabled';var streamClass=(sensorStatus&&sensorStatus.cameraStreaming)?'status-recording':'status-disabled';var mlClass=(sensorStatus&&sensorStatus.eiEnabled)?'status-enabled':'status-disabled';dotsHtml='<span class=\"status-indicator '+enabledClass+'\" id=\"dash-camera-status\" title=\"Enabled\"></span><span class=\"status-indicator '+streamClass+'\" id=\"dash-camera-stream\" title=\"Streaming\" style=\"margin-left:4px\"></span><span class=\"status-indicator '+mlClass+'\" id=\"dash-camera-ml\" title=\"ML Inference\" style=\"margin-left:4px\"></span>'}else if(sensor&&sensor.key==='gamepad'){var enabledClass=enabled?'status-enabled':'status-disabled';dotsHtml='<span class=\"status-indicator '+enabledClass+'\" id=\"dash-gamepad-status\" style=\"margin-right:0.5rem\"></span>'}else{var statusClass=enabled?'status-enabled':'status-disabled';dotsHtml='<span class=\"status-indicator '+statusClass+'\" id=\"dash-'+sensor.key+'-status\"></span>'}card.innerHTML='<div style=\"display:flex;align-items:center;gap:0.5rem;margin-bottom:0.25rem\">'+dotsHtml+'<strong style=\"line-height:1.2\">'+sensor.name+'</strong></div>'+'<div style=\"font-size:0.85rem;opacity:0.8;margin-bottom:0.5rem\">'+sensor.desc+'</div>'+'<div style=\"font-size:0.9rem;color:'+statusColor+'\">'+statusText+'</div>';if(grid)grid.appendChild(card);cardCount++}if(cardCount===0&&grid){grid.innerHTML='<div style=\"grid-column:1/-1;text-align:center;padding:2rem;color:#87ceeb;font-style:italic\">No sensors are currently available.</div>'}console.log('[Dashboard] Created '+cardCount+' sensor cards')};console.log('[Dashboard] Section 7b: createSensorCards added');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 7c: Adding helper functions');window.getAvailableSensors=function(deviceRegistry){var sensors=[];var defs=window.__dashSensorDefs||[];var seen={};if(deviceRegistry&&deviceRegistry.devices){for(var di=0;di<deviceRegistry.devices.length;di++){var dev=deviceRegistry.devices[di];for(var i=0;i<defs.length;i++){var d=defs[i];if(!d||!d.device||!d.key)continue;if(d.device===dev.name){if(!seen[d.key]){seen[d.key]=1;sensors.push({key:d.key,name:d.name,desc:d.desc})}}}}}var status=window.__lastSensorStatus||{};for(var i=0;i<defs.length;i++){var d=defs[i];if(!d||!d.key)continue;if(d.key==='camera'&&status.cameraCompiled&&!seen['camera']){seen['camera']=1;sensors.push({key:'camera',name:d.name||'Camera (DVP)',desc:d.desc||'ESP32-S3 DVP Camera'})}}return sensors};window.getSensorEnabled=function(key,status){if(!status)return false;switch(key){case'imu':return !!status.imuEnabled;case'thermal':return !!status.thermalEnabled;case'tof':return !!status.tofEnabled;case'apds':return !!(status.apdsColorEnabled||status.apdsProximityEnabled||status.apdsGestureEnabled);case'gamepad':return !!status.gamepadEnabled;case'haptic':return !!status.hapticEnabled;case'pwm':return !!status.pwmDriverConnected;case'gps':return !!status.gpsEnabled;case'fmradio':return !!status.fmRadioEnabled;case'camera':return !!status.cameraEnabled;case'mic':return !!status.micEnabled;case'rtc':return !!status.rtcEnabled;case'presence':return !!status.presenceEnabled;default:return false}};window.Dash.showHideCard=function(cardId,show){var c=document.getElementById(cardId);if(c)c.style.display=show?'':'none'};console.log('[Dashboard] Section 7c: Helper functions added');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 7d: Updating applySensorStatus to use helpers');window.__deviceRegistry=null;window.applySensorStatus=function(s){console.log('[Dashboard] applySensorStatus called with:',s);if(!s)return;window.__sensorStatusSeq=s.seq||0;if(window.__deviceRegistry){console.log('[Dashboard] Using cached device registry:',window.__deviceRegistry);window.createSensorCards(s,window.__deviceRegistry)}else{console.log('[Dashboard] Device registry not loaded yet, fetching...');window.fetchDeviceRegistry().then(function(registry){console.log('[Dashboard] Fetch complete, calling createSensorCards with:',registry);window.createSensorCards(s,registry||window.__deviceRegistry)})}if(window.Dash)window.Dash.updateSensorStatus(s)};window.fetchDeviceRegistry=function(){console.log('[Dashboard] fetchDeviceRegistry called');return fetch('/api/devices',{credentials:'include',cache:'no-store'}).then(function(r){console.log('[Dashboard] Device registry fetch response:',r.status);return r.json()}).then(function(d){console.log('[Dashboard] Setting window.__deviceRegistry to:',d);window.__deviceRegistry=d;console.log('[Dashboard] Device registry loaded and stored:',window.__deviceRegistry);if(window.Dash)window.Dash.updateDeviceVisibility(d);return d}).catch(function(e){console.warn('[Dashboard] Device registry fetch failed:',e);return null})};console.log('[Dashboard] Section 7d: applySensorStatus updated');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 7e: Adding fetchSensorStatus');window.fetchSensorStatus=function(){console.log('[Dashboard] Fetching sensor status...');return fetch('/api/sensors/status',{credentials:'include',cache:'no-store'}).then(function(r){console.log('[Dashboard] Sensor status response:',r.status);if(r.status===404){console.log('[Dashboard] Sensor endpoints not available (sensors disabled)');window.applySensorStatus({sensorsDisabled:true});return}return r.json()}).then(function(j){if(!j)return;console.log('[Dashboard] Raw sensor status data:',JSON.stringify(j,null,2));console.log('[Dashboard] Individual sensor states:');console.log('  - imuEnabled:',j.imuEnabled);console.log('  - thermalEnabled:',j.thermalEnabled);console.log('  - tofEnabled:',j.tofEnabled);console.log('  - apdsColorEnabled:',j.apdsColorEnabled);window.applySensorStatus(j)}).catch(function(e){console.warn('[Dashboard] sensor status fetch failed (sensors may be disabled)',e);window.applySensorStatus({sensorsDisabled:true})})};console.log('[Dashboard] Section 7e: fetchSensorStatus added');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 8: Adding SSE functions');window.createSSEIfNeeded=function(){try{console.log('[Dashboard] Creating SSE connection...');if(!window.EventSource){console.warn('[Dashboard] EventSource not supported');return false}if(window.__es){var rs=-1;try{if(typeof window.__es.readyState!=='undefined')rs=window.__es.readyState}catch(_){}console.log('[Dashboard] Existing SSE readyState:',rs);if(rs===2){console.log('[Dashboard] Closing existing SSE connection');try{window.__es.close()}catch(_){}window.__es=null}}if(window.__es){console.log('[Dashboard] Using existing SSE connection');return true}console.log('[Dashboard] Opening new SSE to /api/events');var es=new EventSource('/api/events', { withCredentials: true });es.onopen=function(){console.log('[Dashboard] SSE connection opened')};es.onerror=function(e){console.warn('[Dashboard] SSE error:',e);try{es.close()}catch(_){}window.__es=null};window.__es=es;return true}catch(e){console.error('[Dashboard] SSE creation failed:',e);return false}};console.log('[Dashboard] Section 8a: createSSEIfNeeded added');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 9: Adding SSE attachment');window.attachSSE=function(){try{console.log('[Dashboard] Attaching SSE event listeners...');if(!window.__es){console.warn('[Dashboard] No SSE connection to attach to');return false}var handler=function(e){try{console.log('[Dashboard] Received sensor-status event:',e.data);var dj=JSON.parse(e.data||'{}');var seq=(dj&&dj.seq)?dj.seq:0;var cur=window.__sensorStatusSeq||0;if(seq<=cur)return;window.__sensorStatusSeq=seq;if(window.applySensorStatus)window.applySensorStatus(dj)}catch(err){console.warn('[Dashboard] SSE sensor-status parse error',err)}};window.__es.addEventListener('sensor-status',handler);console.log('[Dashboard] Added sensor-status listener');window.__es.addEventListener('system',function(e){try{console.log('[Dashboard] Received system event:',e.data);var dj=JSON.parse(e.data||'{}');if(window.Dash){console.log('[Dashboard] Calling updateSystem with:',dj);window.Dash.updateSystem(dj)}else{console.warn('[Dashboard] Dash object not available for system update')}}catch(err){console.warn('[Dashboard] SSE system parse error',err)}});console.log('[Dashboard] Added system listener');return true}catch(e){console.error('[Dashboard] SSE attachment failed:',e);return false}};console.log('[Dashboard] Section 9a: attachSSE added');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 10: Adding utility functions');window.fetchSystemStatus=function(){console.log('[Dashboard] Fetching system status via API...');return fetch('/api/system',{credentials:'include',cache:'no-store'}).then(function(r){console.log('[Dashboard] System status response:',r.status);if(!r.ok)throw new Error('HTTP '+r.status);return r.json()}).then(function(j){console.log('[Dashboard] System status data:',j);if(window.Dash)window.Dash.updateSystem(j)}).catch(function(e){console.warn('[Dashboard] System status fetch failed:',e)})};window.setupSensorSSE=function(){console.log('[Dashboard] Setting up sensor-only SSE...');if(window.createSSEIfNeeded)window.createSSEIfNeeded();if(window.attachSSE)window.attachSSE()};console.log('[Dashboard] Section 10a: Utility functions added');</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 10b: Adding signed-in users fetch');window.fetchSignedInUsers=function(){try{var card=document.getElementById('sys-signedin-card');if(!card)return;return fetch('/api/sessions',{credentials:'include',cache:'no-store'}).then(function(r){if(!r.ok)return r.text().then(function(t){throw new Error('HTTP '+r.status+' '+t)});return r.json()}).then(function(j){var users='--';try{if(j&&j.success===true&&Array.isArray(j.sessions)){var seen={};var list=[];for(var i=0;i<j.sessions.length;i++){var u=j.sessions[i]&&j.sessions[i].user?String(j.sessions[i].user):'';if(u&&!seen[u]){seen[u]=1;list.push(u)}}users=list.length?list.join(', '):'--'}}catch(_){users='--'}if(window.Dash)window.Dash.setText('sys-signedin',users)}).catch(function(e){console.log('[Dashboard] Sessions fetch failed:',e);if(window.Dash)window.Dash.setText('sys-signedin','--')})}catch(e){console.log('[Dashboard] fetchSignedInUsers error:',e)}};</script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "<script>console.log('[Dashboard] Section 11: DOM initialization');document.addEventListener('DOMContentLoaded',function(){try{console.log('[Dashboard] Section 11a: DOM loaded, initializing...');if(window.fetchDeviceRegistry)window.fetchDeviceRegistry();if(window.fetchSensorStatus)window.fetchSensorStatus();if(window.fetchSystemStatus)window.fetchSystemStatus();if(window.fetchSignedInUsers&&document.getElementById('sys-signedin-card'))window.fetchSignedInUsers();if(window.createSSEIfNeeded)window.createSSEIfNeeded();if(window.attachSSE)window.attachSSE();try{if(window.__sessionsTimer){clearInterval(window.__sessionsTimer)}window.__sessionsTimer=setInterval(function(){if(window.fetchSignedInUsers&&document.getElementById('sys-signedin-card'))window.fetchSignedInUsers()},15000)}catch(_){ }console.log('[Dashboard] Section 11b: All initialization complete')}catch(e){console.error('[Dashboard] DOM init error',e)}});console.log('[Dashboard] Section 11c: DOM listener registered');</script>", HTTPD_RESP_USE_STRLEN);
}

#endif