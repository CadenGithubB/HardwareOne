/**
 * Camera Sensor Web Module - Sensors page integration
 * 
 * ESP32-S3 DVP (Digital Video Port) Camera Support
 * Supports OV2640, OV3660, and OV5640 cameras
 */

#ifndef SYSTEM_CAMERA_DVP_WEB_H
#define SYSTEM_CAMERA_DVP_WEB_H

#include <Arduino.h>
#include "WebServer_Utils.h"
#include "System_BuildConfig.h"

#if ENABLE_CAMERA_SENSOR

// Stream the camera sensor card HTML (matches other sensor card layout)
inline void streamCameraSensorCard(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"HTML(

    <div class='sensor-card' id='sensor-card-camera'>
      <div class='sensor-title'><span>Camera (DVP)</span><span class='status-indicator status-disabled' id='camera-status-indicator' title='Camera Enabled'></span><span class='status-indicator status-disabled' id='camera-streaming-indicator' title='Streaming/Capturing' style='margin-left:4px'></span><span class='status-indicator status-disabled' id='camera-ml-indicator' title='ML Inference' style='margin-left:4px'></span></div>
      <div class='sensor-description'>ESP32-S3 DVP camera sensor (OV2640/OV3660/OV5640).</div>
      <div id='camera-queue-status' style='display:none;background:#fff3cd;border:1px solid #ffc107;border-radius:4px;padding:8px;margin-bottom:10px;color:#856404;font-size:.9em'></div>
      <div class='sensor-controls'>
        <button class='btn' id='btn-camera-start'>Start Camera</button>
        <button class='btn' id='btn-camera-stop' style='display:none'>Stop Camera</button>
        <button class='btn' id='btn-camera-capture'>Capture</button>
        <button class='btn' id='btn-camera-stream'>Stream</button>
        <button class='btn' id='btn-camera-stream-stop' style='display:none'>Stop Stream</button>
        <button class='btn' id='btn-camera-save' style='display:none' title='Save current image to storage'>Save Image</button>
      </div>
      <div style='margin-top:10px'>
        <button class='btn' id='btn-camera-adjustments-toggle' style='width:100%;background:var(--panel-bg);border:1px solid #dee2e6' onclick='toggleCameraAdjustments()'>Image Adjustments</button>
      </div>
      <div id='camera-adjustments' style='display:none;margin-top:10px;padding:10px;background:var(--panel-bg);border:1px solid #dee2e6;border-radius:4px'>
        <div style='margin-bottom:8px'>
          <label style='display:block;margin-bottom:4px;font-size:0.9em;color:var(--panel-fg)'>Exposure (-2 to 2): <span id='exposure-val'>0</span></label>
          <input type='range' id='camera-exposure' min='-2' max='2' value='0' step='1' style='width:100%'>
        </div>
        <div style='margin-bottom:8px'>
          <label style='display:block;margin-bottom:4px;font-size:0.9em;color:var(--panel-fg)'>Resolution:</label>
          <select id='camera-framesize' style='width:100%'>
            <option value='0'>320x240 (QVGA)</option>
            <option value='1'>640x480 (VGA)</option>
            <option value='2'>800x600 (SVGA)</option>
            <option value='3'>1024x768 (XGA)</option>
            <option value='4'>1280x1024 (SXGA)</option>
            <option value='5'>1600x1200 (UXGA)</option>
          </select>
        </div>
        <div style='margin-bottom:8px'>
          <label style='display:block;margin-bottom:4px;font-size:0.9em;color:var(--panel-fg)'>Brightness (-2 to 2): <span id='brightness-val'>0</span></label>
          <input type='range' id='camera-brightness' min='-2' max='2' value='0' step='1' style='width:100%'>
        </div>
        <div style='margin-bottom:8px'>
          <label style='display:block;margin-bottom:4px;font-size:0.9em;color:var(--panel-fg)'>Contrast (-2 to 2): <span id='contrast-val'>0</span></label>
          <input type='range' id='camera-contrast' min='-2' max='2' value='0' step='1' style='width:100%'>
        </div>
        <div style='margin-bottom:8px'>
          <label style='display:block;margin-bottom:4px;font-size:0.9em;color:var(--panel-fg)'>Saturation (-2 to 2): <span id='saturation-val'>0</span></label>
          <input type='range' id='camera-saturation' min='-2' max='2' value='0' step='1' style='width:100%'>
        </div>
        <div style='margin-bottom:8px'>
          <label style='display:block;margin-bottom:4px;font-size:0.9em;color:var(--panel-fg)'>Quality (0-63, lower=better): <span id='quality-val'>12</span></label>
          <input type='range' id='camera-quality' min='0' max='63' value='12' step='1' style='width:100%'>
        </div>
        <div style='margin-bottom:8px'>
          <label style='display:block;margin-bottom:4px;font-size:0.9em;color:var(--panel-fg)'>Stream FPS: <span id='fps-val'>5</span> fps (<span id='fps-ms-val'>200</span>ms)</label>
          <input type='range' id='camera-fps' min='50' max='2000' value='200' step='50' style='width:100%'>
        </div>
        <div style='display:flex;gap:8px;margin-top:10px;flex-wrap:wrap'>
          <button class='btn' id='btn-hmirror' onclick="applyCameraAdjustment('camerahmirror', 'toggle')" style='flex:1;min-width:100px'>H-Mirror</button>
          <button class='btn' id='btn-vflip' onclick="applyCameraAdjustment('cameravflip', 'toggle')" style='flex:1;min-width:100px'>V-Flip</button>
          <button class='btn' id='btn-rotate' onclick="applyCameraAdjustment('camerarotate', 'toggle')" style='flex:1;min-width:100px'>Rotate 180°</button>
        </div>
      </div>
      <div style='margin-top:10px'>
        <button class='btn' id='btn-camera-ml-toggle' style='width:100%;background:var(--panel-bg);border:1px solid #dee2e6'>Machine Learning</button>
      </div>
      <div id='camera-ml-section' style='display:none;margin-top:10px;padding:10px;background:var(--panel-bg);border:1px solid #dee2e6;border-radius:4px'>
        <div class='sensor-controls' style='margin-bottom:10px'>
          <button class='btn' id='btn-ei-enable'>Enable ML</button>
          <button class='btn' id='btn-ei-disable' style='display:none'>Disable ML</button>
          <button class='btn' id='btn-ei-detect'>Detect</button>
          <button class='btn' id='btn-ei-continuous-start'>Continuous</button>
          <button class='btn' id='btn-ei-continuous-stop' style='display:none'>Stop</button>
        </div>
        <div style='margin-bottom:8px'>
          <label style='display:block;margin-bottom:4px;font-size:0.9em;color:var(--panel-fg)'>Min Confidence: <span id='ei-confidence-val'>0.60</span></label>
          <input type='range' id='ei-confidence' min='0.1' max='1.0' value='0.6' step='0.05' style='width:100%'>
        </div>
        <div style='margin-bottom:8px'>
          <label style='display:block;margin-bottom:4px;font-size:0.9em;color:var(--panel-fg)'>Interval (ms): <span id='ei-interval-val'>1000</span></label>
          <input type='range' id='ei-interval' min='100' max='5000' value='1000' step='100' style='width:100%'>
        </div>
        <div style='margin-bottom:10px'>
          <label style='display:block;margin-bottom:4px;font-size:0.9em;color:var(--panel-fg)'>Model:</label>
          <select id='ei-model-select' style='width:100%'>
            <option value=''>-- Select Model --</option>
          </select>
          <div style='display:flex;gap:6px;margin-top:6px;flex-wrap:wrap'>
            <button class='btn' id='btn-ei-load-model'>Load</button>
            <button class='btn' id='btn-ei-refresh-models'>Refresh</button>
            <button class='btn' id='btn-ei-organize-models'>Organize</button>
          </div>
          <div id='ei-organize-status' style='font-size:0.85em;margin-top:4px;color:var(--muted)'></div>
        </div>
        <div style='margin-top:10px;padding-top:10px;border-top:1px solid var(--border)'>
          <label style='display:block;margin-bottom:4px;font-size:0.9em;color:var(--panel-fg)'>Upload .tflite Model:</label>
          <input type='file' id='ei-model-file' accept='.tflite' style='width:100%;margin-bottom:6px'>
          <button class='btn' id='btn-ei-upload-model' style='width:100%'>Upload Model</button>
          <div id='ei-upload-status' style='font-size:0.85em;margin-top:4px;color:var(--muted)'></div>
        </div>
        <div id='ei-detections' style='color:var(--panel-fg);margin-top:8px'></div>
      </div>
      <div class='sensor-data' id='camera-data'>
        <div id='camera-stats' style='color:var(--panel-fg)'>Model: <span id='cameraModel'>--</span>, Resolution: <span id='cameraRes'>--</span>, PSRAM: <span id='cameraPsram'>--</span></div>
        <div id='camera-preview' style='margin-top:10px;text-align:center'>
          <img id='camera-image' style='max-width:100%;max-height:300px;border-radius:8px;border:1px solid #dee2e6;display:none' alt='Camera preview'>
        </div>
      </div>
    </div>

)HTML", HTTPD_RESP_USE_STRLEN);
}

// Stream button bindings for the camera sensor
inline void streamCameraSensorBindButtons(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "bind('btn-camera-start','camerastart');bind('btn-camera-stop','camerastop');", HTTPD_RESP_USE_STRLEN);
}

// Stream camera-specific JavaScript
inline void streamCameraSensorJs(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Loading camera sensor module JS...');}catch(_){ }", HTTPD_RESP_USE_STRLEN);

  // Camera sensor reader - register in window._sensorReaders
  httpd_resp_send_chunk(req,
    "window._sensorReaders = window._sensorReaders || {};\n"
    "window._sensorReaders.camera = function() {\n"
    "    var url = '/api/sensors?sensor=camera&ts=' + Date.now();\n"
    "    return fetch(url, {cache: 'no-store', credentials: 'include'})\n"
    "      .then(function(r) {\n"
    "        return r.json();\n"
    "      })\n"
    "      .then(function(j) {\n"
    "        var el = document.getElementById('camera-data');\n"
    "        if (el) {\n"
    "          if (j && j.error) {\n"
    "            el.textContent = 'Camera error: ' + j.error;\n"
    "          } else if (j && j.enabled) {\n"
    "            var s = function(id, v) { var e = document.getElementById(id); if (e) e.textContent = v; };\n"
    "            s('cameraModel', j.model || 'Unknown');\n"
    "            s('cameraRes', (j.width || 0) + 'x' + (j.height || 0));\n"
    "            s('cameraPsram', j.psram ? 'Yes' : 'No');\n"
    "          } else {\n"
    "            var stats = document.getElementById('camera-stats');\n"
    "            if (stats) stats.textContent = 'Camera not enabled (use Start Camera button)';\n"
    "          }\n"
    "        }\n"
    "        return j;\n"
    "      })\n"
    "      .catch(function(e) {\n"
    "        console.error('[Sensors] Camera read error', e);\n"
    "        throw e;\n"
    "      });\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  // Capture and stream button handlers
  httpd_resp_send_chunk(req,
    "var cameraAdjustmentStates = {hmirror: false, vflip: false};\n"
    "function __cameraUpdateFlipButtons() {\n"
    "  var rotated = (cameraAdjustmentStates.hmirror && cameraAdjustmentStates.vflip);\n"
    "  updateToggleButtonStyle('btn-rotate', rotated);\n"
    "  updateToggleButtonStyle('btn-hmirror', (cameraAdjustmentStates.hmirror && !rotated));\n"
    "  updateToggleButtonStyle('btn-vflip', (cameraAdjustmentStates.vflip && !rotated));\n"
    "}\n"
    "function __cameraCli(cmd) {\n"
    "  return fetch('/api/cli', {method:'POST', credentials:'include', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent(cmd)})\n"
    "    .then(function(r){ return r.text(); });\n"
    "}\n"
    "function __cameraIsOnText(t) {\n"
    "  try { return (/\\bon\\b/i).test(String(t || '')); } catch(_) { return false; }\n"
    "}\n"
    "function __cameraSyncFlipStates() {\n"
    "  return Promise.all([__cameraCli('camerahmirror'), __cameraCli('cameravflip')])\n"
    "    .then(function(res) {\n"
    "      cameraAdjustmentStates.hmirror = __cameraIsOnText(res[0]);\n"
    "      cameraAdjustmentStates.vflip = __cameraIsOnText(res[1]);\n"
    "      __cameraUpdateFlipButtons();\n"
    "    })\n"
    "    .catch(function(e) { try{ console.warn('[Camera] Sync flip state failed', e); }catch(_){ } });\n"
    "}\n"
    "var __cameraDebounceTimers = {};\n"
    "function __cameraDebouncedApply(cmd, value, waitMs) {\n"
    "  try {\n"
    "    var k = String(cmd || '');\n"
    "    if (__cameraDebounceTimers[k]) { clearTimeout(__cameraDebounceTimers[k]); }\n"
    "    __cameraDebounceTimers[k] = setTimeout(function(){ applyCameraAdjustment(cmd, value); }, waitMs || 200);\n"
    "  } catch(e) { console.error('[Camera] debounce error', e); }\n"
    "}\n"
    "function __cameraCancelDebounce(cmd) {\n"
    "  try {\n"
    "    var k = String(cmd || '');\n"
    "    if (__cameraDebounceTimers[k]) { clearTimeout(__cameraDebounceTimers[k]); }\n"
    "    delete __cameraDebounceTimers[k];\n"
    "  } catch(e) { }\n"
    "}\n"
    "function toggleCameraAdjustments() {\n"
    "  var panel = document.getElementById('camera-adjustments');\n"
    "  if (panel) panel.style.display = (panel.style.display === 'none') ? 'block' : 'none';\n"
    "}\n"
    "function toggleCameraML() {\n"
    "  var panel = document.getElementById('camera-ml-section');\n"
    "  if (panel) panel.style.display = (panel.style.display === 'none') ? 'block' : 'none';\n"
    "}\n"
    "document.getElementById('btn-camera-ml-toggle').onclick = toggleCameraML;\n"
    "var __cameraStreamTimer = null;\n"
    "var __cameraStreamRunning = false;\n"
    "var __cameraStreamPollMs = 200;\n"
    "function __cameraStreamScheduleNext(ms) {\n"
    "  try {\n"
    "    if (!__cameraStreamRunning) return;\n"
    "    if (__cameraStreamTimer) { clearTimeout(__cameraStreamTimer); }\n"
    "    __cameraStreamTimer = setTimeout(__cameraStreamTick, (ms === undefined ? __cameraStreamPollMs : ms));\n"
    "  } catch(e) { }\n"
    "}\n"
    "function __cameraStreamTick() {\n"
    "  try {\n"
    "    if (!__cameraStreamRunning) return;\n"
    "    var img = document.getElementById('camera-image');\n"
    "    if (!img) return;\n"
    "    img.onload = function(){ __cameraStreamScheduleNext(); };\n"
    "    img.onerror = function(){ __cameraStreamScheduleNext(500); };\n"
    "    img.src = '/api/sensors/camera/frame?t=' + Date.now();\n"
    "    img.style.display = 'block';\n"
    "  } catch(e) { console.error('[Camera] stream tick error', e); __cameraStreamScheduleNext(500); }\n"
    "}\n"
    "function __cameraStopStreamUi() {\n"
    "  try {\n"
    "    var img = document.getElementById('camera-image');\n"
    "    var streamBtn = document.getElementById('btn-camera-stream');\n"
    "    var streamStopBtn = document.getElementById('btn-camera-stream-stop');\n"
    "    if (__cameraStreamTimer) { clearTimeout(__cameraStreamTimer); __cameraStreamTimer = null; }\n"
    "    __cameraStreamRunning = false;\n"
    "    if (img) { img.src = 'about:blank'; }\n"
    "    if (streamBtn) streamBtn.style.display = 'inline-block';\n"
    "    if (streamStopBtn) streamStopBtn.style.display = 'none';\n"
    "  } catch(e) { console.error('[Camera] stop stream UI error', e); }\n"
    "}\n"
    "function __cameraStartStreamUi() {\n"
    "  try {\n"
    "    var img = document.getElementById('camera-image');\n"
    "    var streamBtn = document.getElementById('btn-camera-stream');\n"
    "    var streamStopBtn = document.getElementById('btn-camera-stream-stop');\n"
    "    var saveBtn = document.getElementById('btn-camera-save');\n"
    "    if (!img) return;\n"
    "    if (__cameraStreamTimer) { clearTimeout(__cameraStreamTimer); __cameraStreamTimer = null; }\n"
    "    __cameraStreamRunning = true;\n"
    "    __cameraStreamTick();\n"
    "    if (streamBtn) streamBtn.style.display = 'none';\n"
    "    if (streamStopBtn) streamStopBtn.style.display = 'inline-block';\n"
    "    if (saveBtn) saveBtn.style.display = 'none';\n"
    "  } catch(e) { console.error('[Camera] start stream UI error', e); }\n"
    "}\n"
    "function __cameraRestartStreamIfNeeded() {\n"
    "  try {\n"
    "    if (!__cameraStreamRunning) return;\n"
    "    __cameraStreamTick();\n"
    "  } catch(e) { console.error('[Camera] restart stream error', e); }\n"
    "}\n"
    "function updateToggleButtonStyle(btnId, isActive) {\n"
    "  var btn = document.getElementById(btnId);\n"
    "  if (btn) {\n"
    "    if (isActive) {\n"
    "      btn.style.outline = '2px solid var(--link)';\n"
    "      btn.style.outlineOffset = '1px';\n"
    "    } else {\n"
    "      btn.style.outline = '';\n"
    "      btn.style.outlineOffset = '';\n"
    "    }\n"
    "    btn.style.background = '';\n"
    "    btn.style.color = '';\n"
    "    btn.style.fontWeight = '';\n"
    "  }\n"
    "}\n"
    "function applyCameraAdjustment(cmd, value) {\n"
    "  var img = document.getElementById('camera-image');\n"
    "  var wasStreaming = (__cameraStreamRunning === true);\n"
    "  var needsStreamRestart = (cmd === 'camerahmirror' || cmd === 'cameravflip' || cmd === 'camerarotate' || cmd === 'cameraframesize');\n"
    "  var fullCmd = cmd;\n"
    "  if (value === 'toggle') {\n"
    "    if (cmd === 'camerahmirror') {\n"
    "      cameraAdjustmentStates.hmirror = !cameraAdjustmentStates.hmirror;\n"
    "      fullCmd = cmd + ' ' + (cameraAdjustmentStates.hmirror ? 'on' : 'off');\n"
    "      __cameraUpdateFlipButtons();\n"
    "    } else if (cmd === 'cameravflip') {\n"
    "      cameraAdjustmentStates.vflip = !cameraAdjustmentStates.vflip;\n"
    "      fullCmd = cmd + ' ' + (cameraAdjustmentStates.vflip ? 'on' : 'off');\n"
    "      __cameraUpdateFlipButtons();\n"
    "    } else if (cmd === 'camerarotate') {\n"
    "      var rotatedNow = (cameraAdjustmentStates.hmirror && cameraAdjustmentStates.vflip);\n"
    "      var enable = !rotatedNow;\n"
    "      fullCmd = cmd + ' ' + (enable ? 'on' : 'off');\n"
    "      cameraAdjustmentStates.hmirror = enable;\n"
    "      cameraAdjustmentStates.vflip = enable;\n"
    "      __cameraUpdateFlipButtons();\n"
    "    }\n"
    "  } else {\n"
    "    fullCmd = cmd + ' ' + value;\n"
    "  }\n"
    "  console.log('[Camera] Applying adjustment:', fullCmd);\n"
    "  if (wasStreaming && needsStreamRestart) {\n"
    "    __cameraStopStreamUi();\n"
    "  }\n"
    "  fetch('/api/cli', {method:'POST', credentials:'include', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent(fullCmd)})\n"
    "    .then(function(r) { return r.text(); })\n"
    "    .then(function(d) {\n"
    "      console.log('[Camera] Adjustment result:', d);\n"
    "      if (wasStreaming) {\n"
    "        if (needsStreamRestart) { setTimeout(__cameraStartStreamUi, (cmd === 'cameraframesize' ? 800 : 350)); }\n"
    "        else { __cameraRestartStreamIfNeeded(); }\n"
    "      }\n"
    "    })\n"
    "    .catch(function(e) { console.error('[Camera] Adjustment error:', e); });\n"
    "}\n"
    "document.addEventListener('DOMContentLoaded', function() {\n"
    "  var captureBtn = document.getElementById('btn-camera-capture');\n"
    "  var startBtn = document.getElementById('btn-camera-start');\n"
    "  var streamBtn = document.getElementById('btn-camera-stream');\n"
    "  var streamStopBtn = document.getElementById('btn-camera-stream-stop');\n"
    "  var saveBtn = document.getElementById('btn-camera-save');\n"
    "  var stopBtn = document.getElementById('btn-camera-stop');\n"
    "  var img = document.getElementById('camera-image');\n"
    "  var isStreaming = false;\n"
    "  \n"
    "  var exposureSlider = document.getElementById('camera-exposure');\n"
    "  var brightnessSlider = document.getElementById('camera-brightness');\n"
    "  var contrastSlider = document.getElementById('camera-contrast');\n"
    "  var saturationSlider = document.getElementById('camera-saturation');\n"
    "  var qualitySlider = document.getElementById('camera-quality');\n"
    "  var fpsSlider = document.getElementById('camera-fps');\n"
    "  var framesizeSel = document.getElementById('camera-framesize');\n"
    "  \n"
    "  __cameraCli('camerastreaminterval').then(function(t){\n"
    "    try {\n"
    "      var m = /Stream interval:\\s*(\\d+)\\s*ms/i.exec(String(t || ''));\n"
    "      if (m && m[1] !== undefined) {\n"
    "        var v = parseInt(m[1], 10);\n"
    "        if (!isNaN(v) && v >= 50 && v <= 2000) {\n"
    "          __cameraStreamPollMs = v;\n"
    "          if (fpsSlider) {\n"
    "            fpsSlider.value = v;\n"
    "            var fps = Math.round(1000 / v);\n"
    "            var fpsVal = document.getElementById('fps-val');\n"
    "            var fpsMs = document.getElementById('fps-ms-val');\n"
    "            if (fpsVal) fpsVal.textContent = fps;\n"
    "            if (fpsMs) fpsMs.textContent = v;\n"
    "          }\n"
    "        }\n"
    "      }\n"
    "    } catch(e) { }\n"
    "  });\n"
    "  \n"
    "  if (framesizeSel) {\n"
    "    framesizeSel.addEventListener('change', function() {\n"
    "      applyCameraAdjustment('cameraframesize', this.value);\n"
    "    });\n"
    "    __cameraCli('cameraframesize').then(function(t) {\n"
    "      try {\n"
    "        var m = /cameraFramesize\\s*=\\s*(\\d+)/i.exec(String(t || ''));\n"
    "        if (m && m[1] !== undefined) { framesizeSel.value = String(m[1]); }\n"
    "      } catch(e) { }\n"
    "    });\n"
    "  }\n"
    "  \n"
    "  if (exposureSlider) {\n"
    "    exposureSlider.addEventListener('input', function() {\n"
    "      document.getElementById('exposure-val').textContent = this.value;\n"
    "      __cameraDebouncedApply('cameraexposure', this.value, 150);\n"
    "    });\n"
    "    exposureSlider.addEventListener('change', function() {\n"
    "      __cameraCancelDebounce('cameraexposure');\n"
    "      applyCameraAdjustment('cameraexposure', this.value);\n"
    "    });\n"
    "  }\n"
    "  if (brightnessSlider) {\n"
    "    brightnessSlider.addEventListener('input', function() {\n"
    "      document.getElementById('brightness-val').textContent = this.value;\n"
    "      __cameraDebouncedApply('camerabrightness', this.value, 150);\n"
    "    });\n"
    "    brightnessSlider.addEventListener('change', function() {\n"
    "      __cameraCancelDebounce('camerabrightness');\n"
    "      applyCameraAdjustment('camerabrightness', this.value);\n"
    "    });\n"
    "  }\n"
    "  if (contrastSlider) {\n"
    "    contrastSlider.addEventListener('input', function() {\n"
    "      document.getElementById('contrast-val').textContent = this.value;\n"
    "      __cameraDebouncedApply('cameracontrast', this.value, 150);\n"
    "    });\n"
    "    contrastSlider.addEventListener('change', function() {\n"
    "      __cameraCancelDebounce('cameracontrast');\n"
    "      applyCameraAdjustment('cameracontrast', this.value);\n"
    "    });\n"
    "  }\n"
    "  if (saturationSlider) {\n"
    "    saturationSlider.addEventListener('input', function() {\n"
    "      document.getElementById('saturation-val').textContent = this.value;\n"
    "      __cameraDebouncedApply('camerasaturation', this.value, 150);\n"
    "    });\n"
    "    saturationSlider.addEventListener('change', function() {\n"
    "      __cameraCancelDebounce('camerasaturation');\n"
    "      applyCameraAdjustment('camerasaturation', this.value);\n"
    "    });\n"
    "  }\n"
    "  if (qualitySlider) {\n"
    "    qualitySlider.addEventListener('input', function() {\n"
    "      document.getElementById('quality-val').textContent = this.value;\n"
    "      __cameraDebouncedApply('cameraquality', this.value, 300);\n"
    "    });\n"
    "    qualitySlider.addEventListener('change', function() {\n"
    "      __cameraCancelDebounce('cameraquality');\n"
    "      applyCameraAdjustment('cameraquality', this.value);\n"
    "    });\n"
    "  }\n"
    "  if (fpsSlider) {\n"
    "    fpsSlider.addEventListener('input', function() {\n"
    "      var v = parseInt(this.value, 10);\n"
    "      var fps = Math.round(1000 / v);\n"
    "      var fpsVal = document.getElementById('fps-val');\n"
    "      var fpsMs = document.getElementById('fps-ms-val');\n"
    "      if (fpsVal) fpsVal.textContent = fps;\n"
    "      if (fpsMs) fpsMs.textContent = v;\n"
    "      __cameraStreamPollMs = v;\n"
    "    });\n"
    "    fpsSlider.addEventListener('change', function() {\n"
    "      var v = parseInt(this.value, 10);\n"
    "      applyCameraAdjustment('camerastreaminterval', v);\n"
    "    });\n"
    "  }\n"
    "  \n"
    "  if (captureBtn) {\n"
    "    captureBtn.addEventListener('click', function() {\n"
    "      console.log('[Camera] Capture requested');\n"
    "      if (!img) return;\n"
    "      captureBtn.disabled = true;\n"
    "      img.onload = function(){\n"
    "        captureBtn.disabled = false;\n"
    "        if (saveBtn) saveBtn.style.display = 'inline-block';\n"
    "      };\n"
    "      img.onerror = function(){\n"
    "        captureBtn.disabled = false;\n"
    "      };\n"
    "      img.src = '/api/sensors/camera/frame?t=' + Date.now();\n"
    "      img.style.display = 'block';\n"
    "    });\n"
    "  }\n"
    "  \n"
    "  if (saveBtn) {\n"
    "    saveBtn.addEventListener('click', function() {\n"
    "      console.log('[Camera] Save requested');\n"
    "      saveBtn.disabled = true;\n"
    "      saveBtn.textContent = 'Saving...';\n"
    "      fetch('/api/cli', {method:'POST', credentials:'include', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=camerasave'})\n"
    "        .then(function(r) { return r.text(); })\n"
    "        .then(function(d) {\n"
    "          console.log('[Camera] Save result:', d);\n"
    "          saveBtn.textContent = 'Saved!';\n"
    "          setTimeout(function() { saveBtn.textContent = 'Save Image'; saveBtn.disabled = false; }, 2000);\n"
    "        })\n"
    "        .catch(function(e) {\n"
    "          console.error('[Camera] Save error:', e);\n"
    "          saveBtn.textContent = 'Save Failed';\n"
    "          setTimeout(function() { saveBtn.textContent = 'Save Image'; saveBtn.disabled = false; }, 2000);\n"
    "        });\n"
    "    });\n"
    "  }\n"
    "  \n"
    "  if (streamBtn && streamStopBtn && img) {\n"
    "    streamBtn.addEventListener('click', function() {\n"
    "      console.log('[Camera] Starting stream');\n"
    "      __cameraStartStreamUi();\n"
    "      isStreaming = true;\n"
    "    });\n"
    "    streamStopBtn.addEventListener('click', function() {\n"
    "      console.log('[Camera] Stopping stream');\n"
    "      __cameraStopStreamUi();\n"
    "      streamBtn.style.display = 'inline-block';\n"
    "      streamStopBtn.style.display = 'none';\n"
    "      isStreaming = false;\n"
    "    });\n"
    "  }\n"

    "  // Ensure 'Stop Camera' also stops any active stream UI immediately.\n"
    "  if (stopBtn) {\n"
    "    stopBtn.addEventListener('click', function(){\n"
    "      __cameraStopStreamUi();\n"
    "      isStreaming = false;\n"
    "    });\n"
    "  }\n"

    "  // Sync button states against the device so on/off commands are correct.\n"
    "  __cameraSyncFlipStates();\n"
    "  if (startBtn) {\n"
    "    startBtn.addEventListener('click', function(){ setTimeout(__cameraSyncFlipStates, 750); });\n"
    "  }\n"
    "});\n", HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, "window._sensorDataIds = window._sensorDataIds || {};\nwindow._sensorDataIds['camera'] = 'camera-data';\n", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Camera sensor module ready');}catch(_){ }", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
}

// Dashboard definition for camera sensor
inline void streamCameraDashboardDef(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "window.__dashSensorDefs.push({device:'OV2640',key:'camera',name:'Camera (DVP)',desc:'ESP32-S3 DVP Camera'});", HTTPD_RESP_USE_STRLEN);
}

#endif // ENABLE_CAMERA_SENSOR

#endif // SYSTEM_CAMERA_DVP_WEB_H
