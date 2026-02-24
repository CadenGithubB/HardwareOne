/**
 * Edge Impulse ML Web Module - Sensors page integration
 * 
 * TensorFlow Lite Micro inference with runtime model loading from LittleFS
 * Includes state change tracking for object detection
 */

#ifndef SYSTEM_EDGE_IMPULSE_WEB_H
#define SYSTEM_EDGE_IMPULSE_WEB_H

#include <Arduino.h>
#include "WebServer_Utils.h"
#include "System_BuildConfig.h"

#if ENABLE_EDGE_IMPULSE

// Stream the Edge Impulse sensor card HTML
inline void streamEdgeImpulseSensorCard(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"HTML(

    <div class='sensor-card' id='sensor-card-edgeimpulse'>
      <div class='sensor-title'><span>Edge Impulse ML</span><span class='status-indicator status-disabled' id='ei-status-indicator'></span></div>
      <div class='sensor-description'>TensorFlow Lite Micro object detection with state change tracking.</div>
      <div class='sensor-controls'>
        <button class='btn' id='btn-ei-enable'>Enable</button>
        <button class='btn' id='btn-ei-disable' style='display:none'>Disable</button>
        <button class='btn' id='btn-ei-detect'>Detect</button>
        <button class='btn' id='btn-ei-continuous-start'>Continuous</button>
        <button class='btn' id='btn-ei-continuous-stop' style='display:none'>Stop</button>
      </div>
      <div style='margin-top:10px'>
        <button class='btn' id='btn-ei-settings-toggle' style='width:100%;background:var(--panel-bg);border:1px solid #dee2e6'>ML Settings</button>
      </div>
      <div id='ei-settings' style='display:none;margin-top:10px;padding:10px;background:var(--panel-bg);border:1px solid #dee2e6;border-radius:4px'>
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
            <button class='btn' id='btn-ei-load-model'>Load Model</button>
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
      </div>
      <div class='sensor-data' id='ei-data'>
        <div id='ei-status-text' style='color:var(--panel-fg);margin-bottom:8px'>Model: <span id='eiModelPath'>Not loaded</span></div>
        <div id='ei-detections' style='color:var(--panel-fg)'></div>
        <div id='ei-tracked' style='margin-top:10px;padding-top:10px;border-top:1px solid var(--border)'>
          <div style='font-weight:bold;margin-bottom:6px;color:var(--panel-fg)'>Tracked Objects:</div>
          <div id='ei-tracked-list' style='font-size:0.9em;color:var(--panel-fg)'>None</div>
        </div>
        <div id='ei-state-changes' style='margin-top:10px;max-height:150px;overflow-y:auto'>
          <div style='font-weight:bold;margin-bottom:6px;color:var(--panel-fg)'>State Changes:</div>
          <div id='ei-state-log' style='font-size:0.85em;color:var(--muted)'></div>
        </div>
      </div>
    </div>

)HTML", HTTPD_RESP_USE_STRLEN);
}

// Stream button bindings for Edge Impulse
inline void streamEdgeImpulseSensorBindButtons(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "bind('btn-ei-enable','ei enable 1');bind('btn-ei-disable','ei enable 0');bind('btn-ei-detect','ei detect');bind('btn-ei-continuous-start','ei continuous 1');bind('btn-ei-continuous-stop','ei continuous 0');", HTTPD_RESP_USE_STRLEN);
}

// Stream Edge Impulse-specific JavaScript
inline void streamEdgeImpulseSensorJs(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Loading Edge Impulse ML module JS...');}catch(_){ }", HTTPD_RESP_USE_STRLEN);

  // Settings toggle
  httpd_resp_send_chunk(req,
    "(function(){\n"
    "var settingsVisible = false;\n"
    "var toggleBtn = document.getElementById('btn-ei-settings-toggle');\n"
    "var settingsDiv = document.getElementById('ei-settings');\n"
    "if(toggleBtn && settingsDiv) {\n"
    "  toggleBtn.onclick = function() {\n"
    "    settingsVisible = !settingsVisible;\n"
    "    settingsDiv.style.display = settingsVisible ? 'block' : 'none';\n"
    "  };\n"
    "}\n"
    "})();\n", HTTPD_RESP_USE_STRLEN);

  // Confidence slider
  httpd_resp_send_chunk(req,
    "(function(){\n"
    "var slider = document.getElementById('ei-confidence');\n"
    "var valSpan = document.getElementById('ei-confidence-val');\n"
    "if(slider && valSpan) {\n"
    "  slider.oninput = function() { valSpan.textContent = parseFloat(this.value).toFixed(2); };\n"
    "  slider.onchange = function() {\n"
    "    fetch('/api/cli', {method:'POST', credentials:'include', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=ei confidence '+this.value});\n"
    "  };\n"
    "}\n"
    "})();\n", HTTPD_RESP_USE_STRLEN);

  // Interval slider
  httpd_resp_send_chunk(req,
    "(function(){\n"
    "var slider = document.getElementById('ei-interval');\n"
    "var valSpan = document.getElementById('ei-interval-val');\n"
    "if(slider && valSpan) {\n"
    "  slider.oninput = function() { valSpan.textContent = this.value; };\n"
    "  slider.onchange = function() {\n"
    "    fetch('/api/cli', {method:'POST', credentials:'include', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=set edgeimpulse intervalMs '+this.value});\n"
    "  };\n"
    "}\n"
    "})();\n", HTTPD_RESP_USE_STRLEN);

  // Model list loading
  httpd_resp_send_chunk(req,
    "window._eiLoadModels = function() {\n"
    "  var select = document.getElementById('ei-model-select');\n"
    "  if(!select) return;\n"
    "  fetch('/api/cli', {method:'POST', credentials:'include', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=ei model list'})\n"
    "    .then(function(r){ return r.text(); })\n"
    "    .then(function(txt) {\n"
    "      select.innerHTML = '<option value=\"\">-- Select Model --</option>';\n"
    "      var lines = txt.split('\\n');\n"
    "      lines.forEach(function(line) {\n"
    "        var match = line.match(/^\\s+([\\w.-]+\\.tflite)/);\n"
    "        if(match) {\n"
    "          var opt = document.createElement('option');\n"
    "          opt.value = match[1];\n"
    "          opt.textContent = match[1] + (line.indexOf('[LOADED]') !== -1 ? ' (loaded)' : '');\n"
    "          select.appendChild(opt);\n"
    "        }\n"
    "      });\n"
    "    });\n"
    "};\n"
    "document.getElementById('btn-ei-refresh-models').onclick = window._eiLoadModels;\n"
    "window._eiLoadModels();\n", HTTPD_RESP_USE_STRLEN);

  // Model load button
  httpd_resp_send_chunk(req,
    "document.getElementById('btn-ei-load-model').onclick = function() {\n"
    "  var select = document.getElementById('ei-model-select');\n"
    "  if(!select || !select.value) { alert('Select a model first'); return; }\n"
    "  fetch('/api/cli', {method:'POST', credentials:'include', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=ei model load '+select.value})\n"
    "    .then(function(r){ return r.text(); })\n"
    "    .then(function(txt) {\n"
    "      console.log('[EI] Load result:', txt);\n"
    "      window._eiLoadModels();\n"
    "      window._eiUpdateStatus();\n"
    "    });\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  // Model upload handler
  httpd_resp_send_chunk(req,
    "document.getElementById('btn-ei-upload-model').onclick = function() {\n"
    "  var fileInput = document.getElementById('ei-model-file');\n"
    "  var statusEl = document.getElementById('ei-upload-status');\n"
    "  if(!fileInput || !fileInput.files || fileInput.files.length === 0) {\n"
    "    if(statusEl) statusEl.textContent = 'Please select a .tflite file first';\n"
    "    return;\n"
    "  }\n"
    "  var file = fileInput.files[0];\n"
    "  if(!file.name.endsWith('.tflite')) {\n"
    "    if(statusEl) statusEl.textContent = 'File must be a .tflite model';\n"
    "    return;\n"
    "  }\n"
    "  var btn = document.getElementById('btn-ei-upload-model');\n"
    "  btn.disabled = true;\n"
    "  if(statusEl) statusEl.textContent = 'Uploading ' + file.name + '...';\n"
    "  var reader = new FileReader();\n"
    "  reader.onload = function(e) {\n"
    "    var b64 = btoa(String.fromCharCode.apply(null, new Uint8Array(e.target.result)));\n"
    "    var body = 'path=/EI Models/' + encodeURIComponent(file.name) + '&binary=1&content=' + encodeURIComponent(b64);\n"
    "    fetch('/api/files/upload', {method:'POST', credentials:'include', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:body})\n"
    "      .then(function(r){ return r.json(); })\n"
    "      .then(function(data) {\n"
    "        btn.disabled = false;\n"
    "        if(data.success) {\n"
    "          if(statusEl) statusEl.innerHTML = '<span style=\"color:#28a745\">Uploaded! Now select and load it.</span>';\n"
    "          window._eiLoadModels();\n"
    "        } else {\n"
    "          if(statusEl) statusEl.innerHTML = '<span style=\"color:#dc3545\">Error: ' + (data.error||'Unknown') + '</span>';\n"
    "        }\n"
    "      })\n"
    "      .catch(function(err) {\n"
    "        btn.disabled = false;\n"
    "        if(statusEl) statusEl.innerHTML = '<span style=\"color:#dc3545\">Upload failed: ' + err + '</span>';\n"
    "      });\n"
    "  };\n"
    "  reader.onerror = function() {\n"
    "    btn.disabled = false;\n"
    "    if(statusEl) statusEl.innerHTML = '<span style=\"color:#dc3545\">Failed to read file</span>';\n"
    "  };\n"
    "  reader.readAsArrayBuffer(file);\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  // Organize models button
  httpd_resp_send_chunk(req,
    "document.getElementById('btn-ei-organize-models').onclick = function() {\n"
    "  var statusEl = document.getElementById('ei-organize-status');\n"
    "  var btn = document.getElementById('btn-ei-organize-models');\n"
    "  btn.disabled = true;\n"
    "  if(statusEl) statusEl.textContent = 'Organizing...';\n"
    "  fetch('/api/ei/organize', {method:'POST', credentials:'include'})\n"
    "    .then(function(r){ return r.json(); })\n"
    "    .then(function(data) {\n"
    "      btn.disabled = false;\n"
    "      if(data.success) {\n"
    "        if(statusEl) statusEl.innerHTML = '<span style=\"color:#28a745\">Moved '+data.moved+' files</span>';\n"
    "        window._eiLoadModels();\n"
    "      } else {\n"
    "        if(statusEl) statusEl.innerHTML = '<span style=\"color:#dc3545\">Error: '+(data.error||'Unknown')+'</span>';\n"
    "      }\n"
    "    })\n"
    "    .catch(function(err) {\n"
    "      btn.disabled = false;\n"
    "      if(statusEl) statusEl.innerHTML = '<span style=\"color:#dc3545\">Failed: '+err+'</span>';\n"
    "    });\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  // Status update function
  httpd_resp_send_chunk(req,
    "window._eiUpdateStatus = function() {\n"
    "  fetch('/api/cli', {method:'POST', credentials:'include', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=ei status'})\n"
    "    .then(function(r){ return r.text(); })\n"
    "    .then(function(txt) {\n"
    "      var modelMatch = txt.match(/Model path:\\s*(.+)/i);\n"
    "      var modelEl = document.getElementById('eiModelPath');\n"
    "      if(modelEl) modelEl.textContent = modelMatch ? modelMatch[1].trim() : 'Not loaded';\n"
    "      var indicator = document.getElementById('ei-status-indicator');\n"
    "      var enabledMatch = txt.match(/Enabled:\\s*(yes|no)/i);\n"
    "      var isEnabled = enabledMatch && enabledMatch[1].toLowerCase() === 'yes';\n"
    "      if(indicator) indicator.className = 'status-indicator ' + (isEnabled ? 'status-enabled' : 'status-disabled');\n"
    "      var btnEnable = document.getElementById('btn-ei-enable');\n"
    "      var btnDisable = document.getElementById('btn-ei-disable');\n"
    "      if(btnEnable) btnEnable.style.display = isEnabled ? 'none' : 'inline-block';\n"
    "      if(btnDisable) btnDisable.style.display = isEnabled ? 'inline-block' : 'none';\n"
    "      var contMatch = txt.match(/Continuous:\\s*(running|stopped)/i);\n"
    "      var isRunning = contMatch && contMatch[1].toLowerCase() === 'running';\n"
    "      var btnStart = document.getElementById('btn-ei-continuous-start');\n"
    "      var btnStop = document.getElementById('btn-ei-continuous-stop');\n"
    "      if(btnStart) btnStart.style.display = isRunning ? 'none' : 'inline-block';\n"
    "      if(btnStop) btnStop.style.display = isRunning ? 'inline-block' : 'none';\n"
    "    });\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  // Bounding box overlay setup
  httpd_resp_send_chunk(req,
    "window._eiOverlayCanvas = null;\n"
    "window._eiModelInputSize = 160;\n"
    "window._eiLastDetections = [];\n"
    "window._eiBoxPersistFrames = 5;\n"
    "window._eiBoxFrameCount = 0;\n"
    "window._eiSetupOverlay = function() {\n"
    "  if(window._eiOverlayCanvas) return;\n"
    "  var camImg = document.getElementById('camera-stream-img');\n"
    "  if(!camImg) return;\n"
    "  var wrapper = camImg.parentElement;\n"
    "  if(!wrapper) return;\n"
    "  wrapper.style.position = 'relative';\n"
    "  var canvas = document.createElement('canvas');\n"
    "  canvas.id = 'ei-overlay-canvas';\n"
    "  canvas.style.cssText = 'position:absolute;top:0;left:0;width:100%;height:100%;pointer-events:none;z-index:10';\n"
    "  wrapper.appendChild(canvas);\n"
    "  window._eiOverlayCanvas = canvas;\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  // Draw bounding boxes on overlay - expand FOMO grid cells to be more visible
  httpd_resp_send_chunk(req,
    "window._eiDrawBoxes = function(detections) {\n"
    "  window._eiSetupOverlay();\n"
    "  var canvas = window._eiOverlayCanvas;\n"
    "  if(!canvas) return;\n"
    "  var camImg = document.getElementById('camera-stream-img');\n"
    "  if(!camImg) return;\n"
    "  var rect = camImg.getBoundingClientRect();\n"
    "  canvas.width = rect.width;\n"
    "  canvas.height = rect.height;\n"
    "  var ctx = canvas.getContext('2d');\n"
    "  ctx.clearRect(0, 0, canvas.width, canvas.height);\n"
    "  if(!detections || detections.length === 0) return;\n"
    "  var scaleX = canvas.width / window._eiModelInputSize;\n"
    "  var scaleY = canvas.height / window._eiModelInputSize;\n"
    "  var expandFactor = 4;\n"
    "  detections.forEach(function(d, i) {\n"
    "    var cx = (d.x + d.width/2) * scaleX;\n"
    "    var cy = (d.y + d.height/2) * scaleY;\n"
    "    var w = d.width * scaleX * expandFactor;\n"
    "    var h = d.height * scaleY * expandFactor;\n"
    "    var x = cx - w/2;\n"
    "    var y = cy - h/2;\n"
    "    var colors = ['#00ff00','#ff6600','#00ffff','#ff00ff','#ffff00'];\n"
    "    var color = colors[i % colors.length];\n"
    "    ctx.strokeStyle = color;\n"
    "    ctx.lineWidth = 3;\n"
    "    ctx.strokeRect(x, y, w, h);\n"
    "    ctx.fillStyle = color;\n"
    "    ctx.font = 'bold 14px sans-serif';\n"
    "    var label = d.label + ' ' + (d.confidence * 100).toFixed(0) + '%%';\n"
    "    var labelWidth = ctx.measureText(label).width + 8;\n"
    "    ctx.fillRect(x, y - 20, labelWidth, 20);\n"
    "    ctx.fillStyle = '#000';\n"
    "    ctx.fillText(label, x + 4, y - 5);\n"
    "  });\n"
    "  window._eiLastDetections = detections;\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  // Detection display
  httpd_resp_send_chunk(req,
    "window._eiShowDetections = function(data) {\n"
    "  var el = document.getElementById('ei-detections');\n"
    "  if(!el) return;\n"
    "  if(!data || !data.success) {\n"
    "    el.innerHTML = '<span style=\"color:#dc3545\">Error: ' + (data && data.error ? data.error : 'Unknown') + '</span>';\n"
    "    window._eiDrawBoxes([]);\n"
    "    return;\n"
    "  }\n"
    "  if(data.modelInputSize) {\n"
    "    window._eiModelInputSize = data.modelInputSize;\n"
    "  }\n"
    "  if(!data.detections || data.detections.length === 0) {\n"
    "    el.innerHTML = 'No detections (inference: ' + data.inferenceTimeMs + 'ms)';\n"
    "    window._eiDrawBoxes([]);\n"
    "    return;\n"
    "  }\n"
    "  window._eiDrawBoxes(data.detections);\n"
    "  var html = '<div style=\"margin-bottom:4px\">Detected ' + data.detections.length + ' objects (' + data.inferenceTimeMs + 'ms):</div>';\n"
    "  data.detections.forEach(function(d) {\n"
    "    html += '<div style=\"padding:4px 8px;background:rgba(40,167,69,0.2);border-radius:4px;margin:2px 0\">';\n"
    "    html += '<strong>' + d.label + '</strong> ' + (d.confidence * 100).toFixed(1) + '%% ';\n"
    "    html += '<span style=\"opacity:0.7\">at (' + d.x + ',' + d.y + ')</span>';\n"
    "    html += '</div>';\n"
    "  });\n"
    "  el.innerHTML = html;\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  // Tracked objects display
  httpd_resp_send_chunk(req,
    "window._eiShowTracked = function(data) {\n"
    "  var el = document.getElementById('ei-tracked-list');\n"
    "  if(!el) return;\n"
    "  if(!data || !data.trackedObjects || data.trackedObjects.length === 0) {\n"
    "    el.textContent = 'None';\n"
    "    return;\n"
    "  }\n"
    "  var html = '';\n"
    "  data.trackedObjects.forEach(function(obj, i) {\n"
    "    var stateClass = obj.stateChanged ? 'background:rgba(255,193,7,0.3);' : '';\n"
    "    html += '<div style=\"padding:4px 8px;border-radius:4px;margin:2px 0;' + stateClass + '\">';\n"
    "    html += '[' + i + '] <strong>' + obj.label + '</strong>';\n"
    "    if(obj.prevLabel) html += ' <span style=\"opacity:0.6\">(was: ' + obj.prevLabel + ')</span>';\n"
    "    html += ' at (' + obj.x + ',' + obj.y + ')';\n"
    "    if(obj.stateChanged) html += ' <span style=\"color:#ffc107\">CHANGED</span>';\n"
    "    html += '</div>';\n"
    "  });\n"
    "  el.innerHTML = html;\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  // State change log
  httpd_resp_send_chunk(req,
    "window._eiStateLog = [];\n"
    "window._eiLogStateChange = function(prev, curr, x, y) {\n"
    "  var now = new Date().toLocaleTimeString();\n"
    "  window._eiStateLog.unshift({time: now, prev: prev, curr: curr, x: x, y: y});\n"
    "  if(window._eiStateLog.length > 20) window._eiStateLog.pop();\n"
    "  var el = document.getElementById('ei-state-log');\n"
    "  if(!el) return;\n"
    "  var html = '';\n"
    "  window._eiStateLog.forEach(function(entry) {\n"
    "    html += '<div style=\"margin:2px 0\">';\n"
    "    html += '<span style=\"opacity:0.6\">[' + entry.time + ']</span> ';\n"
    "    html += '<span style=\"color:#dc3545\">' + entry.prev + '</span> → ';\n"
    "    html += '<span style=\"color:#28a745\">' + entry.curr + '</span>';\n"
    "    html += '</div>';\n"
    "  });\n"
    "  el.innerHTML = html || '<span style=\"opacity:0.5\">No state changes yet</span>';\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  // Polling for detections and tracked objects
  httpd_resp_send_chunk(req,
    "window._eiPollingInterval = null;\n"
    "window._eiStartPolling = function() {\n"
    "  if(window._eiPollingInterval) return;\n"
    "  window._eiPollingInterval = setInterval(function() {\n"
    "    fetch('/api/edgeimpulse/detect', {credentials:'include'})\n"
    "      .then(function(r){ return r.json(); })\n"
    "      .then(function(data) {\n"
    "        window._eiShowDetections(data);\n"
    "        if(data && data.trackedObjects) {\n"
    "          data.trackedObjects.forEach(function(obj) {\n"
    "            if(obj.stateChanged && obj.prevLabel) {\n"
    "              window._eiLogStateChange(obj.prevLabel, obj.label, obj.x, obj.y);\n"
    "            }\n"
    "          });\n"
    "          window._eiShowTracked(data);\n"
    "        }\n"
    "      })\n"
    "      .catch(function(e){ console.error('[EI] Poll error:', e); });\n"
    "  }, 1000);\n"
    "};\n"
    "window._eiStopPolling = function() {\n"
    "  if(window._eiPollingInterval) {\n"
    "    clearInterval(window._eiPollingInterval);\n"
    "    window._eiPollingInterval = null;\n"
    "  }\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  // Single detect button override - use capturing to intercept before generic handler
  httpd_resp_send_chunk(req,
    "document.addEventListener('DOMContentLoaded', function() {\n"
    "  var btn = document.getElementById('btn-ei-detect');\n"
    "  if(btn) {\n"
    "    btn.addEventListener('click', function(e) {\n"
    "      e.stopImmediatePropagation();\n"
    "      e.preventDefault();\n"
    "      console.log('[EI] Detect button clicked - fetching results');\n"
    "      fetch('/api/edgeimpulse/detect', {credentials:'include'})\n"
    "        .then(function(r){ return r.json(); })\n"
    "        .then(function(data) {\n"
    "          console.log('[EI] Detection result:', data);\n"
    "          window._eiShowDetections(data);\n"
    "        })\n"
    "        .catch(function(e){ console.error('[EI] Detect error:', e); });\n"
    "    }, true);\n"
    "  }\n"
    "});\n", HTTPD_RESP_USE_STRLEN);

  // Start/stop polling when continuous buttons clicked
  httpd_resp_send_chunk(req,
    "(function(){\n"
    "var btnStart = document.getElementById('btn-ei-continuous-start');\n"
    "var btnStop = document.getElementById('btn-ei-continuous-stop');\n"
    "if(btnStart) {\n"
    "  var origClick = btnStart.onclick;\n"
    "  btnStart.onclick = function(e) {\n"
    "    if(origClick) origClick.call(this, e);\n"
    "    setTimeout(function(){ window._eiStartPolling(); }, 500);\n"
    "  };\n"
    "}\n"
    "if(btnStop) {\n"
    "  var origClick2 = btnStop.onclick;\n"
    "  btnStop.onclick = function(e) {\n"
    "    if(origClick2) origClick2.call(this, e);\n"
    "    window._eiStopPolling();\n"
    "  };\n"
    "}\n"
    "})();\n", HTTPD_RESP_USE_STRLEN);

  // Initialize on page load
  httpd_resp_send_chunk(req,
    "document.addEventListener('DOMContentLoaded', function() {\n"
    "  window._eiUpdateStatus();\n"
    "});\n", HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Edge Impulse ML module JS loaded');}catch(_){ }", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
}

#endif // ENABLE_EDGE_IMPULSE
#endif // SYSTEM_EDGE_IMPULSE_WEB_H
