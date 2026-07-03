#ifndef WEBPAGE_SENSORS_H
#define WEBPAGE_SENSORS_H

#include <Arduino.h>
#include "System_BuildConfig.h"
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif

// Registration function - registers all sensor-related URI handlers
#if ENABLE_HTTP_SERVER
void registerSensorHandlers(httpd_handle_t server);
#else
inline void registerSensorHandlers(httpd_handle_t) {}
#endif

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

// Forward declaration
inline void streamSensorsInner(httpd_req_t* req, const String& username);

// Individual sensor web modules (conditionally included)
#if ENABLE_THERMAL_SENSOR
#include "i2csensor_mlx90640_web.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor_vl53l4cx_web.h"
#endif
#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor_seesaw_web.h"
#endif
#if ENABLE_ANO_ENCODER
#include "i2csensor_ano_encoder_web.h"
#endif
#if ENABLE_GPS_SENSOR
#include "i2csensor_pa1010d_web.h"
#endif
#if ENABLE_RTC_SENSOR
#include "i2csensor_ds3231_web.h"
#endif
#if ENABLE_PRESENCE_SENSOR
#include "i2csensor_sths34pf80_web.h"
#endif
#if ENABLE_FM_RADIO
#include "i2csensor_rda5807_web.h"
#endif
#if ENABLE_IMU_SENSOR
#include "i2csensor_bno055_web.h"
#endif
#if ENABLE_SERVO
#include "i2csensor_pca9685_web.h"
#endif
#if ENABLE_CAMERA_SENSOR
#include "System_Camera_DVP_Web.h"
#endif
#if ENABLE_MICROPHONE_SENSOR
#include "System_Microphone_Web.h"
#endif
#if ENABLE_EDGE_IMPULSE
#include "System_EdgeImpulse_Web.h"
#endif

// External auth check to get username (for disabled page)
extern bool isAuthed(httpd_req_t* req, String& usernameOut);
extern void streamBeginHtml(httpd_req_t* req, const char* title, bool isPublic, const String& username, const String& activePage);
extern void streamEndHtml(httpd_req_t* req);

// Top-level Sensors page content streamer (wrapper that conditionally shows sensors or disabled message)
inline void streamSensorsContent(httpd_req_t* req, const String& username) {
  streamSensorsInner(req, username);
}

// Streamed inner content for the Sensors page (CSS + HTML skeleton + small JS)
inline void streamSensorsInner(httpd_req_t* req, const String& username) {
  // Stream HTML head with hw helpers (defines window.hw object)
  streamBeginHtml(req, "Sensors", false, username, "sensors");
  
  // CSS
  httpd_resp_send_chunk(req, R"CSS(
<style>
  .sensors-container{max-width:1200px;margin:0 auto;padding:20px}
  .sensor-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(350px,1fr));gap:20px;margin-bottom:30px}
  .sensor-card{background:var(--panel-bg);border-radius:15px;padding:20px;box-shadow:0 4px 6px rgba(0,0,0,.1);border:1px solid var(--border);overflow:hidden}
  .sensor-title{font-size:1.3em;font-weight:bold;margin-bottom:10px;color:var(--panel-fg);display:flex;align-items:center;gap:10px}
  .sensor-description{color:var(--panel-fg);margin-bottom:15px;font-size:.9em}
  .sensor-controls{display:flex;gap:10px;margin-bottom:15px;flex-wrap:wrap}
  .sensor-data{background:var(--crumb-bg);border-radius:8px;padding:15px;font-family:'Courier New',monospace;font-size:.9em;min-height:60px;color:var(--panel-fg)}
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
  .joy-canvas{width:100px;height:100px;border:1px solid var(--border);border-radius:50%;background:var(--crumb-bg)}
  .abxy-grid{display:grid;grid-template-columns:repeat(3,36px);grid-auto-rows:28px;gap:4px;align-content:start}
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
#if ENABLE_ANO_ENCODER
  streamAnoEncoderSensorCard(req);
#endif
#if ENABLE_GPS_SENSOR
  streamPA1010DGpsSensorCard(req);
#endif
#if ENABLE_RTC_SENSOR
  streamDS3231RtcSensorCard(req);
#endif
#if ENABLE_PRESENCE_SENSOR
  streamSTHS34PF80PresenceSensorCard(req);
#endif
#if ENABLE_FM_RADIO
  streamRDA5807FmRadioSensorCard(req);
#endif
#if ENABLE_CAMERA_SENSOR
  streamCameraSensorCard(req);
#endif
#if ENABLE_MICROPHONE_SENSOR
  streamMicrophoneSensorCard(req);
#endif
  // Edge Impulse ML is now integrated into camera card, not a separate sensor
#if ENABLE_SERVO
  streamPCA9685ServoDriverCard(req);
#endif

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
  // `gamepad` and `anoencoder` are both keyed by the Seesaw chip on the bus —
  // only one card exists in DOM at a time (whichever driver was compiled in),
  // so the setVis for the missing one is a no-op.
  httpd_resp_send_chunk(req, "var has={imu:false,thermal:false,tof:false,gamepad:false,anoencoder:false,gps:false,servo:false,fmradio:false,camera:false,rtc:false,presence:false};if(d&&d.devices&&d.devices.forEach){d.devices.forEach(function(dev){", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "if(dev&&dev.name==='BNO055')has.imu=true;else if(dev&&dev.name==='MLX90640')has.thermal=true;else if(dev&&dev.name==='VL53L4CX')has.tof=true;else if(dev&&dev.name==='Seesaw'){has.gamepad=true;has.anoencoder=true;}else if(dev&&dev.name==='PA1010D')has.gps=true;else if(dev&&dev.name==='PCA9685')has.servo=true;else if(dev&&dev.name==='RDA5807')has.fmradio=true;else if(dev&&dev.name==='DS3231')has.rtc=true;else if(dev&&dev.name==='STHS34PF80')has.presence=true;});}console.log('[SENSORS] Detected sensors:',has);", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "var compiled={imu:!!st.imuCompiled,thermal:!!st.thermalCompiled,tof:!!st.tofCompiled,gamepad:!!st.inputCompiled,anoencoder:!!st.inputCompiled,gps:!!st.gpsCompiled,fmradio:!!st.fmradioCompiled,servo:!!st.servoCompiled,camera:!!st.cameraCompiled,rtc:!!st.rtcCompiled,presence:!!st.presenceCompiled};has.camera=!!st.cameraCompiled;console.log('[SENSORS] Compiled sensors:',compiled);", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "setVis('sensor-card-imu',has.imu&&compiled.imu);setVis('sensor-card-thermal',has.thermal&&compiled.thermal);setVis('sensor-card-tof',has.tof&&compiled.tof);setVis('sensor-card-gamepad',has.gamepad&&compiled.gamepad);setVis('sensor-card-ano-encoder',has.anoencoder&&compiled.anoencoder);setVis('sensor-card-gps',has.gps&&compiled.gps);setVis('sensor-card-servo',has.servo&&compiled.servo);setVis('sensor-card-fmradio',has.fmradio&&compiled.fmradio);setVis('sensor-card-camera',has.camera&&compiled.camera);setVis('sensor-card-rtc',has.rtc&&compiled.rtc);setVis('sensor-card-presence',has.presence&&compiled.presence);", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "var any=(has.imu&&compiled.imu)||(has.thermal&&compiled.thermal)||(has.tof&&compiled.tof)||(has.gamepad&&compiled.gamepad)||(has.anoencoder&&compiled.anoencoder)||(has.gps&&compiled.gps)||(has.servo&&compiled.servo)||(has.fmradio&&compiled.fmradio)||(has.camera&&compiled.camera)||(has.rtc&&compiled.rtc)||(has.presence&&compiled.presence);if(!any&&grid){grid.innerHTML='<div style=\"grid-column:1/-1;text-align:center;padding:2rem;color:#87ceeb;font-style:italic\">No sensors available (none compiled + detected)</div>';}console.log('[SENSORS] Device detection complete');", HTTPD_RESP_USE_STRLEN);
  // Show banner for sensors detected on I2C bus but not compiled into firmware
  httpd_resp_send_chunk(req,
    "var nameMap={imu:'IMU (BNO055)',thermal:'Thermal Camera (MLX90640)',tof:'ToF Distance (VL53L4CX)',"
#if ENABLE_ANO_ENCODER
    "gamepad:'ANO Encoder (Seesaw)',gps:'GPS (PA1010D)',fmradio:'FM Radio (RDA5807)',"
#else
    "gamepad:'Gamepad (Seesaw)',gps:'GPS (PA1010D)',fmradio:'FM Radio (RDA5807)',"
#endif
    "rtc:'RTC (DS3231)',presence:'Presence (STHS34PF80)'};"
    "var uncompiled=[];"
    "for(var k in has){if(has[k]&&!compiled[k]&&nameMap[k])uncompiled.push(nameMap[k]);}"
    "if(uncompiled.length&&grid){"
    "var banner=document.createElement('div');"
    "banner.style.cssText='grid-column:1/-1;background:rgba(255,193,7,0.12);border:1px solid rgba(255,193,7,0.4);border-radius:8px;padding:1rem 1.25rem;margin-bottom:0.5rem;color:#ffc107';"
    "banner.innerHTML='<div style=\"font-weight:600;margin-bottom:0.35rem\">Detected but not compiled</div>'"
    "+'<div style=\"color:rgba(255,255,255,0.8);font-size:0.9rem\">The following sensors were found on the I2C bus but are not included in this firmware build: <strong style=\"color:#ffc107\">'+uncompiled.join(', ')+'</strong>.</div>'"
    "+'<div style=\"color:rgba(255,255,255,0.55);font-size:0.82rem;margin-top:0.35rem\">Enable the corresponding CUSTOM_ENABLE_* flags in System_BuildConfig.h and rebuild to use them.</div>';"
    "grid.insertBefore(banner,grid.firstChild);"
    "}",
    HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "}).catch(function(e){console.error('[SENSORS] Device/status fetch error:',e);}).finally(function(){if(loading)loading.style.display='none';if(grid)grid.style.display='grid';});", HTTPD_RESP_USE_STRLEN);

  // Control helpers
  httpd_resp_send_chunk(req, "console.log('[SENSORS] Setting up control helpers');var setClass=function(id,enabled){var el=hw._ge(id);if(!el)return;var c=enabled?'status-indicator status-enabled':'status-indicator status-disabled';if(el.className!==c)el.className=c};", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req,
    "var bind=function(id,cmd){var el=hw._ge(id);if(el){hw.on(el,'click',function(){console.log('[SENSORS] Button clicked:',id,'cmd:',cmd);hw.postForm('/api/cli',{cmd:cmd}).then(function(r){console.log('[SENSORS] Command result:',r);try{var action=(/start$/.test(cmd)||/^open/.test(cmd)?'start':(/stop$/.test(cmd)||/^close/.test(cmd)?'stop':''));var sensor='';var c=cmd.replace(/^(open|close)/,'');if(/^imu/i.test(c))sensor='imu';else if(/^thermal/i.test(c))sensor='thermal';else if(/^tof/i.test(c))sensor='tof';else if(/^gamepad/i.test(c))sensor='gamepad';else if(/^input/i.test(c))sensor='"
#if ENABLE_ANO_ENCODER
    "anoencoder"
#else
    "gamepad"
#endif
    "';else if(/^gps/i.test(c))sensor='gps';else if(/^fmradio/i.test(c))sensor='fmradio';else if(/^camera/i.test(c))sensor='camera';else if(/^mic/i.test(c))sensor='microphone';else if(/^edgeimpulse/i.test(c))sensor='edgeimpulse';else if(/^rtc/i.test(c))sensor='rtc';else if(/^presence/i.test(c))sensor='presence';if(action==='start'&&sensor){startSensorPolling(sensor)}else if(action==='stop'&&sensor){stopSensorPolling(sensor)}}catch(_){}}).catch(function(e){console.error('[SENSORS] Command error:',e);})})}};",
    HTTPD_RESP_USE_STRLEN);
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
#if ENABLE_ANO_ENCODER
  streamAnoEncoderSensorBindButtons(req);
#endif
#if ENABLE_GPS_SENSOR
  streamPA1010DGpsSensorBindButtons(req);
#endif
#if ENABLE_RTC_SENSOR
  streamDS3231RtcSensorBindButtons(req);
#endif
#if ENABLE_PRESENCE_SENSOR
  streamSTHS34PF80PresenceSensorBindButtons(req);
#endif
#if ENABLE_FM_RADIO
  streamRDA5807FmRadioSensorBindButtons(req);
#endif
#if ENABLE_CAMERA_SENSOR
  streamCameraSensorBindButtons(req);
#endif
#if ENABLE_MICROPHONE_SENSOR
  streamMicrophoneSensorBindButtons(req);
#endif
#if ENABLE_EDGE_IMPULSE
  streamEdgeImpulseSensorBindButtons(req);
#endif
  httpd_resp_send_chunk(req, "console.log('[SENSORS] Button bindings complete');", HTTPD_RESP_USE_STRLEN);

  // Status poller - also toggle button visibility based on enabled state
  httpd_resp_send_chunk(req, "console.log('[SENSORS] Setting up status poller');var apply=function(s){console.log('[SENSORS] Status update:',s);try{setClass('gyro-status-indicator',!!s.imuEnabled);setClass('thermal-status-indicator',!!s.thermalEnabled);setClass('tof-status-indicator',!!s.tofEnabled);setClass('gamepad-status-indicator',!!s.inputEnabled);setClass('ano-status-indicator',!!s.inputEnabled);setClass('gps-status-indicator',!!s.gpsEnabled);setClass('rtc-status-indicator',!!s.rtcEnabled);setClass('presence-status-indicator',!!s.presenceEnabled);setClass('fmradio-status-indicator',!!s.fmRadioEnabled);setClass('servo-status-indicator',!!s.pwmDriverConnected);setClass('camera-status-indicator',!!s.cameraEnabled);setClass('camera-streaming-indicator',!!s.cameraStreaming);setClass('camera-ml-indicator',!!s.eiEnabled);setClass('mic-status-indicator',!!s.micEnabled);setClass('ei-status-indicator',!!s.eiEnabled);var rec=hw._ge('mic-recording-indicator');if(rec){var cls=(s.micRecording?'status-indicator status-recording':'status-indicator status-disabled');if(rec.className!==cls)rec.className=cls}", HTTPD_RESP_USE_STRLEN);
  // Button visibility toggling
  httpd_resp_send_chunk(req, "var toggleBtns=function(startId,stopId,isOn){var startBtn=hw._ge(startId);var stopBtn=hw._ge(stopId);if(startBtn)startBtn.style.display=isOn?'none':'inline-block';if(stopBtn)stopBtn.style.display=isOn?'inline-block':'none';};toggleBtns('btn-gamepad-start','btn-gamepad-stop',!!s.inputEnabled);toggleBtns('btn-ano-start','btn-ano-stop',!!s.inputEnabled);toggleBtns('btn-gps-start','btn-gps-stop',!!s.gpsEnabled);toggleBtns('btn-rtc-start','btn-rtc-stop',!!s.rtcEnabled);toggleBtns('btn-presence-start','btn-presence-stop',!!s.presenceEnabled);toggleBtns('btn-imu-start','btn-imu-stop',!!s.imuEnabled);toggleBtns('btn-thermal-start','btn-thermal-stop',!!s.thermalEnabled);toggleBtns('btn-tof-start','btn-tof-stop',!!s.tofEnabled);toggleBtns('btn-camera-start','btn-camera-stop',!!s.cameraEnabled);toggleBtns('btn-mic-start','btn-mic-stop',!!s.micEnabled);toggleBtns('btn-ei-enable','btn-ei-disable',!!s.eiEnabled);var camRec=hw._ge('btn-camera-record');var camRecStop=hw._ge('btn-camera-record-stop');var camRecHint=hw._ge('camera-record-hint');if(camRec){camRec.style.display=(s.cameraEnabled&&!s.cameraRecording)?'inline-block':'none';camRec.disabled=!s.sdWritable;if(camRec.title){if(s.sdWritable)camRec.title='Record stream to SD card as MJPEG AVI';else if(s.sdAvailable)camRec.title='SD mounted but writes are failing (card may be flaky)';else camRec.title='Requires SD card';}}if(camRecStop){camRecStop.style.display=(s.cameraEnabled&&s.cameraRecording)?'inline-block':'none';}if(camRecHint){if(s.cameraEnabled&&!s.sdWritable){camRecHint.style.display='block';camRecHint.textContent=s.sdAvailable?'SD card is mounted but writes are failing — card may be flaky, full, or write-protected.':'SD card required to record video.';}else{camRecHint.style.display='none';}}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "if(s.fmRadioEnabled){if(typeof startFMRadioPolling==='function')startFMRadioPolling()}else{if(typeof stopFMRadioPolling==='function')stopFMRadioPolling()}var servoStatus=hw._ge('servo-connection-status');if(servoStatus){servoStatus.textContent=s.pwmDriverConnected?'Initialized & ready':'Not initialized (use servo command to start)';servoStatus.style.color=s.pwmDriverConnected?'#28a745':'#ffc107';}}catch(_){}};", HTTPD_RESP_USE_STRLEN);
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
    "  return hw.fetchJSON('/api/settings')\n"
    "    .then(function(s) {\n"
    "      console.log('[Settings] Loaded:', s);\n"
    "      // Sensor settings live under hardware.sensors.<name> after the\n"
    "      // v0.93 refactor. Thermal entries are further grouped by purpose\n"
    "      // (timing/display/filtering); ToF entries are flat.\n"
    "      var thSect = (s.settings && s.settings.hardware && s.settings.hardware.sensors && s.settings.hardware.sensors.thermal) || {};\n"
    "      var thTiming = thSect.timing || {}, thDisp = thSect.display || {}, thFilt = thSect.filtering || {};\n"
    "      var tofSect = (s.settings && s.settings.hardware && s.settings.hardware.sensors && s.settings.hardware.sensors.tof) || {};\n"
    "      if (thTiming.thermalPollingMs !== undefined) {\n"
    "        thermalPollingMs = thTiming.thermalPollingMs;\n"
    "        console.log('[Settings] Thermal polling: ' + thermalPollingMs + 'ms');\n"
    "      }\n"
    "      if (tofSect.tofPollingMs !== undefined) {\n"
    "        tofPollingMs = tofSect.tofPollingMs;\n"
    "        console.log('[Settings] ToF polling: ' + tofPollingMs + 'ms');\n"
    "      }\n"
    "      if (tofSect.tofStabilityThreshold !== undefined) {\n"
    "        tofStabilityThreshold = tofSect.tofStabilityThreshold;\n"
    "        console.log('[Settings] ToF stability threshold: ' + tofStabilityThreshold);\n"
    "      }\n"
    "      if (tofSect.tofMaxDistanceMm !== undefined) {\n"
    "        tofMaxDistance = tofSect.tofMaxDistanceMm;\n"
    "        console.log('[Settings] ToF max distance: ' + tofMaxDistance + 'mm');\n"
    "        var rng = document.getElementById('tof-range-mm');\n"
    "        if (rng) {\n"
    "          rng.textContent = String(tofMaxDistance);\n"
    "        }\n"
    "      }\n"
    "      if (tofSect.tofTransitionMs !== undefined) {\n"
    "        tofTransitionMs = tofSect.tofTransitionMs;\n"
    "        console.log('[Settings] ToF transition ms: ' + tofTransitionMs);\n"
    "      }\n"
    "      if (thDisp.thermalPaletteDefault !== undefined) {\n"
    "        thermalPalette = thDisp.thermalPaletteDefault;\n"
    "        console.log('[Settings] Thermal palette: ' + thermalPalette);\n"
    "        applyThermalPalette(thermalPalette);\n"
    "      }\n"
    "      if (thFilt.thermalEWMAFactor !== undefined) {\n"
    "        thermalEWMAFactor = thFilt.thermalEWMAFactor;\n"
    "        console.log('[Settings] Thermal EWMA factor: ' + thermalEWMAFactor);\n"
    "      }\n"
    "      // (Removed legacy debugSettings reads of s.settings.debug.sensorsFrame /\n"
    "      //  sensorsData / sensorsGeneral — those umbrella flags no longer exist\n"
    "      //  post per-sensor sub-flag retrofit, and debugSettings itself was\n"
    "      //  never consumed downstream.)\n"
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
#if ENABLE_RTC_SENSOR
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Loading RTC sensor module...');}catch(_){ }", HTTPD_RESP_USE_STRLEN);
  streamDS3231RtcSensorJs(req);
#endif
#if ENABLE_PRESENCE_SENSOR
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Loading Presence sensor module...');}catch(_){ }", HTTPD_RESP_USE_STRLEN);
  streamSTHS34PF80PresenceSensorJs(req);
#endif

  httpd_resp_send_chunk(req, "window._sensorReaders=window._sensorReaders||{};window._sensorDataIds=window._sensorDataIds||{};window._sensorPollingIntervals=window._sensorPollingIntervals||{};", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req,
    "function controlSensor(sensor, action) {\n"
    "  var command = sensor + action;\n"
    "  return hw.postFormText('/api/cli', {cmd: command})\n"
    "  .then(function(result) {\n"
    "    console.log('[Sensors] control result', result);\n"
    "    return hw.fetchJSON('/api/sensors/status')\n"
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
  httpd_resp_send_chunk(req, "function startSensorPolling(sensor){console.log('[SENSORS] startSensorPolling called for:',sensor);if(sensorIntervals[sensor]){console.log('[SENSORS] Already polling',sensor);return}if(sensor==='thermal'){if(typeof startThermalPolling==='function'){startThermalPolling()}return}else if(sensor==='tof'){if(typeof startToFPolling==='function'){startToFPolling()}return}else{readSensor(sensor);var interval=(window._sensorPollingIntervals&&window._sensorPollingIntervals[sensor])?window._sensorPollingIntervals[sensor]:(sensor==='imu'?200:((sensor==='gamepad'||sensor==='anoencoder')?56:500));console.log('[SENSORS] Starting',sensor,'polling with interval:',interval+'ms');sensorIntervals[sensor]=setInterval(function(){readSensor(sensor)},interval)}}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function stopSensorPolling(sensor){console.log('[SENSORS] stopSensorPolling called for:',sensor);if(sensorIntervals[sensor]){clearInterval(sensorIntervals[sensor]);delete sensorIntervals[sensor];console.log('[SENSORS] Stopped polling',sensor)}if(sensor==='thermal'){if(typeof stopThermalPolling==='function'){stopThermalPolling()}}else if(sensor==='tof'){if(typeof stopToFPolling==='function'){stopToFPolling()}}}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Core control functions ready');}catch(_){ }", HTTPD_RESP_USE_STRLEN);

  // SSE + indicators + already-active check
  httpd_resp_send_chunk(req, "window.attachPageSSEListeners=function(es){if(!es){console.warn('[Sensors] attachPageSSEListeners called with null EventSource');return}console.log('[Sensors] Attaching sensor-status listener to EventSource');var handler=function(e){try{var status=JSON.parse(e.data||'{}');console.log('[Sensors] Received SSE sensor-status update:',status);if(window.applySensorStatus){window.applySensorStatus(status)}updateStatusIndicators(status)}catch(err){console.warn('[Sensors] SSE sensor-status parse error:',err)}};es.addEventListener('sensor-status',handler);console.log('[Sensors] SSE sensor-status listener attached successfully')};", HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req,
    "window.applySensorStatus = function(status) {\n"
    "  if (!status) return;\n"
    "  console.log('[Queue] Applying sensor status:', status);\n"
    "  ['thermal', 'tof', 'imu', 'gps', 'gamepad', 'ano', 'fmradio', 'presence'].forEach(function(sensor) {\n"
    "    var queueEl = document.getElementById(sensor + '-queue-status');\n"
    "    if (!queueEl) return;\n"
    "    var isQueued = status[sensor + 'Queued'];\n"
    "    var queuePos = status[sensor + 'QueuePos'];\n"
    "    if (isQueued && queuePos > 0) {\n"
    "      var qd = status.queueDepth || 0;\n"
    "      queueEl.textContent = 'Queued for start (position ' + queuePos + ' of ' + qd + ')';\n"
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
    "    if (status.inputCompiled && status.inputEnabled) {\n"
    "      startSensorPolling('gamepad');\n"
    "      startSensorPolling('anoencoder');\n"
    "    } else {\n"
    "      stopSensorPolling('gamepad');\n"
    "      stopSensorPolling('anoencoder');\n"
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
    "    if (status.micCompiled && status.micEnabled) {\n"
    "      startSensorPolling('microphone');\n"
    "    } else if (status.micCompiled) {\n"
    "      stopSensorPolling('microphone');\n"
    "    }\n"
    "    // Auto-refresh recordings when recording stops\n"
    "    if (status.micCompiled) {\n"
    "      var wasRecording = window._lastMicRecording === true;\n"
    "      var isRecording = status.micRecording === true;\n"
    "      if (wasRecording && !isRecording) {\n"
    "        console.log('[Sensors] Recording stopped - refreshing recordings list');\n"
    "        window.__lastRecCount = -1;\n"
    "        if (typeof window.loadMicRecordings === 'function') {\n"
    "          setTimeout(function() { window.loadMicRecordings(); }, 500);\n"
    "        }\n"
    "      }\n"
    "      window._lastMicRecording = isRecording;\n"
    "    }\n"
    "    if (status.presenceCompiled && status.presenceEnabled) {\n"
    "      startSensorPolling('presence');\n"
    "    } else if (status.presenceCompiled) {\n"
    "      stopSensorPolling('presence');\n"
    "    }\n"
    "    if (status.cameraCompiled && status.cameraEnabled) {\n"
    "      startSensorPolling('camera');\n"
    "    } else if (status.cameraCompiled) {\n"
    "      stopSensorPolling('camera');\n"
    "    }\n"
    "    // Update Edge Impulse UI when status changes\n"
    "    if (typeof window._eiUpdateStatus === 'function') {\n"
    "      var btnEnable = document.getElementById('btn-ei-enable');\n"
    "      var btnDisable = document.getElementById('btn-ei-disable');\n"
    "      if(btnEnable) btnEnable.style.display = status.eiEnabled ? 'none' : 'inline-block';\n"
    "      if(btnDisable) btnDisable.style.display = status.eiEnabled ? 'inline-block' : 'none';\n"
    "    }\n"
    "  } catch (_) {}\n"
    "};\n", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function updateStatusIndicators(status){if(!status){console.warn('[Sensors] updateStatusIndicators called with null/undefined status');return}var t=document.getElementById('thermal-status-indicator');var f=document.getElementById('tof-status-indicator');var i=document.getElementById('gyro-status-indicator');var g=document.getElementById('gamepad-status-indicator');var r=document.getElementById('fmradio-status-indicator');var c=document.getElementById('camera-status-indicator');if(t){t.className=status.thermalEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}if(f){f.className=status.tofEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}if(i){i.className=status.imuEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}if(g){g.className=status.inputEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}var g2=document.getElementById('ano-status-indicator');if(g2){g2.className=status.inputEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}if(r){r.className=status.fmRadioEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}if(c){c.className=status.cameraEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}var cs=document.getElementById('camera-streaming-indicator');if(cs){cs.className=status.cameraStreaming?'status-indicator status-recording':'status-indicator status-disabled'}var cml=document.getElementById('camera-ml-indicator');if(cml){cml.className=status.eiEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}var m=document.getElementById('mic-status-indicator');if(m){m.className=status.micEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}var mr=document.getElementById('mic-recording-indicator');if(mr){mr.className=status.micRecording?'status-indicator status-recording':'status-indicator status-disabled'}var ei=document.getElementById('ei-status-indicator');if(ei){ei.className=status.eiEnabled?'status-indicator status-enabled':'status-indicator status-disabled'}}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req,
    "function checkAlreadyActiveSensors() {\n"
    "  console.log('[Sensors] Checking for already-active sensors...');\n"
    "  Promise.all([\n"
    "    hw.fetchJSON('/api/devices'),\n"
    "    hw.fetchJSON('/api/sensors/status')\n"
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
#if ENABLE_ANO_ENCODER
    "        'Seesaw': {type: 'anoencoder', enabledKey: 'inputEnabled', compiledKey: 'inputCompiled', indicatorId: 'ano-status-indicator', address: 0x49},\n"
#else
    "        'Seesaw': {type: 'gamepad', enabledKey: 'inputEnabled', compiledKey: 'inputCompiled', indicatorId: 'gamepad-status-indicator', address: 0x50},\n"
#endif
    "        'DS3231': {type: 'rtc', enabledKey: 'rtcEnabled', compiledKey: 'rtcCompiled', indicatorId: 'rtc-status-indicator'},\n"
    "        'STHS34PF80': {type: 'presence', enabledKey: 'presenceEnabled', compiledKey: 'presenceCompiled', indicatorId: 'presence-status-indicator'}\n"
    "      }[device.name];\n"
    "      if (!map) {\n"
    "        if (device && (device.name === 'SSD1306')) return;\n"
    "        console.log('[Sensors] No map for device:', device.name);\n"
    "        return;\n"
    "      }\n"
    "      if (device.name === 'Seesaw' && map.address !== undefined && device.address !== map.address) return;\n"
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

  // Remote input renderer — used for the REMOTE_SENSOR_INPUT ("input") type,
  // which bond/mesh streaming reports for BOTH worker input devices:
  //   ANO encoder : {"pos":N,"axis":0|1,"buttons":B}  (buttons active-HIGH)
  //   gamepad     : {"x":N,"y":N,"buttons":B}         (buttons active-LOW)
  // Remote INPUT (gamepad/ANO) reuses the SHARED, parameterized renderers —
  // hwRenderGamepadState / hwRenderAnoState (+ hwBuildAnoInner for the card body).
  // The old dedicated hwRenderRemoteInput duplicate was removed; see updateRemoteSensor.

  // Remote sensors loader
  httpd_resp_send_chunk(req,
    "function stopRemoteSensorsPolling(){\n"
    "  try{if(window._remoteSensorsTimer){clearInterval(window._remoteSensorsTimer);window._remoteSensorsTimer=null;}}catch(_){}\n"
    "}\n"
    "window.hwRenderGenericSensor=function(el,data){\n"
    "  if(!el)return;\n"
    "  if(data===null||data===undefined){el.textContent='No data';return;}\n"
    "  if(typeof data!=='object'){el.textContent=String(data);return;}\n"
    "  var skip={valid:1,connected:1,ts:1,age:1,seq:1,val:1};\n"
    "  var esc=function(s){return String(s).replace(/[<>&]/g,function(c){return c==='<'?'&lt;':(c==='>'?'&gt;':'&amp;');});};\n"
    "  var fmt=function(v){if(typeof v==='boolean')return v?'yes':'no';if(typeof v==='number')return (v%1===0)?String(v):v.toFixed(2);return esc(v);};\n"
    "  var rows=[];\n"
    "  var addRow=function(label,val){rows.push('<div style=\"display:flex;justify-content:space-between;gap:12px\"><span style=\"opacity:0.7\">'+esc(label)+'</span><span style=\"font-family:monospace\">'+val+'</span></div>');};\n"
    "  Object.keys(data).forEach(function(k){\n"
    "    if(skip[k])return;\n"
    "    var v=data[k];\n"
    "    if(v===null||v===undefined){addRow(k,'\\u2014');return;}\n"
    "    if(Array.isArray(v)){\n"
    "      if(v.length===0){addRow(k,'none');return;}\n"
    "      v.forEach(function(item,idx){\n"
    "        if(item&&typeof item==='object'){var parts=Object.keys(item).filter(function(ik){return !skip[ik];}).map(function(ik){return ik+':'+fmt(item[ik]);});addRow(k+' '+(idx+1),parts.join('  '));}\n"
    "        else{addRow(k+' '+(idx+1),fmt(item));}\n"
    "      });return;\n"
    "    }\n"
    "    if(typeof v==='object'){var sub=Object.keys(v).map(function(sk){return sk+':'+fmt(v[sk]);});addRow(k,sub.join('  '));return;}\n"
    "    addRow(k,fmt(v));\n"
    "  });\n"
    "  el.innerHTML=rows.length?rows.join(''):'(no readings)';\n"
    "};\n"
    "function updateRemoteSensor(deviceMac,sensorType){\n"
    "  try{\n"
    "    var id='remote-'+String(deviceMac).replace(/:/g,'')+'-'+sensorType;\n"
    "    var el=hw._ge(id);\n"
    "    if(!el)return;\n"
    "    var setDot=function(ok){var dot=hw._ge(id+'-dot');if(!dot)return;var c=ok?'status-indicator status-enabled':'status-indicator status-disabled';if(dot.className!==c)dot.className=c;};\n"
    "    var url='/api/sensors/remote?device='+encodeURIComponent(deviceMac)+'&sensor='+encodeURIComponent(sensorType);\n"
    "    hw.fetchJSON(url).then(function(d){\n"
    "      if(!el)return;\n"
    "      if(d&&d.error){setDot(false);el.textContent=d.error;return;}\n"
    "      var en=!!(d&&d.enabled);setDot(en);\n"
    "      var payload=d;\n"
    "      if(payload&&typeof payload==='object'&&payload.data!==undefined){payload=payload.data;}\n"
    "      if(payload===null||payload===undefined){el.textContent=en?'No data yet':'Disabled';return;}\n"
    "      if(typeof payload==='string'){try{payload=JSON.parse(payload);}catch(_){}}\n"
    "      if(sensorType==='input'&&payload&&typeof payload==='object'){\n"
    "        var host=document.getElementById(id+'-widget');\n"
    "        var isAno=(payload.pos!==undefined);\n"
    "        var kind=isAno?'ano':'gp';\n"
    "        if(host&&host.getAttribute('data-kind')!==kind){\n"
    "          host.setAttribute('data-kind',kind);\n"
    "          if(isAno){\n"
    "            host.innerHTML=window.hwBuildAnoInner(id);\n"
    "          }else{\n"
    "            host.innerHTML='<div class=\"gamepad-row\" style=\"margin-top:10px\"><div class=\"joy-wrap\"><canvas id=\"'+id+'-joystick\" class=\"joy-canvas\" width=\"100\" height=\"100\"></canvas></div><div class=\"abxy-grid\"><div></div><div id=\"'+id+'-btn-x\" class=\"btn btn-small\" style=\"width:52px\">X</div><div></div><div id=\"'+id+'-btn-y\" class=\"btn btn-small\" style=\"width:52px\">Y</div><div></div><div id=\"'+id+'-btn-a\" class=\"btn btn-small\" style=\"width:52px\">A</div><div></div><div id=\"'+id+'-btn-b\" class=\"btn btn-small\" style=\"width:52px\">B</div><div></div></div><div style=\"display:flex;flex-direction:column;gap:6px;margin-left:12px\"><div id=\"'+id+'-btn-select\" class=\"btn btn-small\" style=\"width:80px\">Select</div><div id=\"'+id+'-btn-start\" class=\"btn btn-small\" style=\"width:80px\">Start</div></div></div>';\n"
    "          }\n"
    "        }\n"
    "        if(isAno){ window.hwRenderAnoState(payload,{data:id,pos:id+'-pos',axis:id+'-axis',wheel:id+'-wheel',btnIn:id+'-btn-in',btnUp:id+'-btn-up',btnDown:id+'-btn-down',btnLeft:id+'-btn-left',btnRight:id+'-btn-right',btnStart:id+'-btn-start'}); }\n"
    "        else if(typeof window.hwRenderGamepadState==='function'){ window.hwRenderGamepadState(payload,{data:id,joystick:id+'-joystick',btnX:id+'-btn-x',btnY:id+'-btn-y',btnA:id+'-btn-a',btnB:id+'-btn-b',btnSelect:id+'-btn-select',btnStart:id+'-btn-start'}); }\n"
    "        return;\n"
    "      }\n"
    "      try{window.hwRenderGenericSensor(el,payload);}catch(_){try{el.textContent=JSON.stringify(payload);}catch(_2){el.textContent=String(payload);}}\n"
    "    }).catch(function(_e){if(el)el.textContent='Error';setDot(false);});\n"
    "  }catch(_){}\n"
    "}\n"
    "function startRemoteSensorsPolling(devices){\n"
    "  stopRemoteSensorsPolling();\n"
    "  if(!devices||!devices.forEach)return;\n"
    "  var tick=function(){\n"
    "    devices.forEach(function(device){\n"
    "      if(!device||!device.sensors||!device.sensors.forEach)return;\n"
    "      device.sensors.forEach(function(s){var t=(s&&typeof s==='object')?s.type:s;updateRemoteSensor(device.mac,t);});\n"
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
    "        var msg = (data && data.enabled === false)\n"
    "          ? 'ESP-NOW is not initialized. Click Initialize on the ESP-NOW page to enable it.'\n"
    "          : 'ESP-NOW is active but no remote devices are sending sensor data.';\n"
    "        statusDiv.innerHTML = '<div style=\"text-align:center;padding:1rem;color:var(--panel-fg)\">' + msg + '</div>';\n"
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
    "        device.sensors.forEach(function(s) {\n"
    "          var sensorType=(s&&typeof s==='object')?s.type:s;\n"
    "          var initEnabled=(s&&typeof s==='object')?!!s.enabled:true;\n"
    "          var dotCls=initEnabled?'status-indicator status-enabled':'status-indicator status-disabled';\n"
    "          var card = document.createElement('div');\n"
    "          card.className = 'sensor-card';\n"
    "          var macKey = device.mac.replace(/:/g, '');\n"
    "          if(sensorType==='input'){\n"
    "            var ib='remote-'+macKey+'-input';\n"
    "            card.innerHTML = '<div class=\"sensor-title\"><span class=\"'+dotCls+'\" id=\"' + ib + '-dot\"></span>' + device.name + ' - input</div>' +\n"
    "              '<div class=\"sensor-description\">Remote input via ESP-NOW (MAC: ' + device.mac + ')</div>' +\n"
    "              '<div class=\"sensor-data\" id=\"'+ib+'\">Loading...</div>' +\n"
    "              '<div id=\"'+ib+'-widget\" data-kind=\"\"></div>';\n"
    "          } else {\n"
    "            card.innerHTML = '<div class=\"sensor-title\"><span class=\"'+dotCls+'\" id=\"remote-' + macKey + '-' + sensorType + '-dot\"></span>' + device.name + ' - ' + sensorType + '</div>' +\n"
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
#if ENABLE_ANO_ENCODER
  streamAnoEncoderSensorJs(req);
#endif
#if ENABLE_CAMERA_SENSOR
  streamCameraSensorJs(req);
#endif
#if ENABLE_MICROPHONE_SENSOR
  streamMicrophoneSensorJS(req);
#endif
#if ENABLE_EDGE_IMPULSE
  streamEdgeImpulseSensorJs(req);
#endif
}

#endif // WEB_SENSORS_H