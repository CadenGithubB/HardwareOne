/**
 * Microphone Sensor Web UI Components
 * 
 * HTML and JavaScript for microphone sensor card on Sensors page.
 */

#ifndef SYSTEM_MICROPHONE_WEB_H
#define SYSTEM_MICROPHONE_WEB_H

#include "System_BuildConfig.h"

#if ENABLE_MICROPHONE_SENSOR

#include <esp_http_server.h>

// Stream microphone sensor card HTML
inline void streamMicrophoneSensorCard(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"rawliteral(
<div class="sensor-card" id="mic-card">
  <div class="sensor-header">
    <span class="sensor-title">Microphone</span>
    <span class="status-indicator status-disabled" id="mic-status-indicator"></span>
    <span class="status-indicator status-disabled" id="mic-recording-indicator" title="Recording status" style="margin-left:4px"></span>
  </div>
  <div class="sensor-body">
    <div class="sensor-info">
      <div class="info-row"><span>Sample Rate:</span><span id="mic-samplerate">--</span></div>
      <div class="info-row"><span>Bit Depth:</span><span id="mic-bitdepth">--</span></div>
      <div class="info-row"><span>Channels:</span><span id="mic-channels">--</span></div>
    </div>
    <div class="vu-meter-container">
      <div class="vu-meter">
        <div class="vu-meter-fill" id="mic-level-bar" style="width: 0%;"></div>
      </div>
      <span class="vu-meter-label" id="mic-level-text">0%</span>
    </div>
    <div class="sensor-controls">
      <button class="btn btn-primary" id="btn-mic-start">Start</button>
      <button class="btn btn-secondary" id="btn-mic-stop">Stop</button>
      <button class="btn btn-info" id="btn-mic-record">Record</button>
      <button class="btn btn-secondary" id="btn-mic-stop-record">Stop Rec</button>
    </div>
    <div style='margin-top:10px'>
      <button class='btn' id='btn-mic-settings-toggle' style='width:100%;background:var(--panel-bg);border:1px solid #dee2e6'>Microphone Settings</button>
    </div>
    <div id='mic-settings' style='display:none;margin-top:10px;padding:10px;background:var(--panel-bg);border:1px solid #dee2e6;border-radius:4px'>
      <div class="adjustments-grid">
        <div class="adjustment-row">
          <label for="mic-gain-slider">Gain: <span id="mic-gain-value">50</span>%</label>
          <input type="range" id="mic-gain-slider" min="0" max="100" value="50" step="5">
        </div>
        <div class="adjustment-row">
          <label for="mic-samplerate-select">Sample Rate:</label>
          <select id="mic-samplerate-select" class="form-select">
            <option value="8000">8 kHz</option>
            <option value="16000" selected>16 kHz</option>
            <option value="22050">22.05 kHz</option>
            <option value="44100">44.1 kHz</option>
            <option value="48000">48 kHz</option>
          </select>
        </div>
        <div class="adjustment-row">
          <label for="mic-bitdepth-select">Bit Depth:</label>
          <select id="mic-bitdepth-select" class="form-select">
            <option value="16" selected>16-bit</option>
            <option value="32">32-bit</option>
          </select>
        </div>
      </div>
    </div>
    <div style='margin-top:10px'>
      <button class='btn' id='btn-mic-recordings-toggle' style='width:100%;background:var(--panel-bg);border:1px solid #dee2e6'>Recordings <span id="mic-rec-count">(0)</span></button>
    </div>
    <div class="recordings-section" id="mic-recordings" style='display:none;margin-top:10px;padding:10px;background:var(--panel-bg);border:1px solid #dee2e6;border-radius:4px'>
      <div class="recordings-list" id="mic-recordings-list"></div>
    </div>
  </div>
</div>
)rawliteral", HTTPD_RESP_USE_STRLEN);
}

// Stream button bindings for microphone
inline void streamMicrophoneSensorBindButtons(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "bind('btn-mic-start','micstart');bind('btn-mic-stop','micstop');bind('btn-mic-record','micrecord start');bind('btn-mic-stop-record','micrecord stop');", HTTPD_RESP_USE_STRLEN);
}

// Stream microphone-specific JavaScript
inline void streamMicrophoneSensorJS(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Loading microphone sensor module JS...');}catch(_){ }", HTTPD_RESP_USE_STRLEN);

  // Microphone sensor reader - register in window._sensorReaders
  httpd_resp_send_chunk(req,
    "window._sensorReaders = window._sensorReaders || {};\n"
    "window._sensorReaders.microphone = function() {\n"
    "  var url = '/api/sensors?sensor=microphone&ts=' + Date.now();\n"
    "  return fetch(url, {cache: 'no-store', credentials: 'include'})\n"
    "    .then(function(r) { return r.json(); })\n"
    "    .then(function(data) {\n"
    "      if (!data) return data;\n"
    "      var statusInd = document.getElementById('mic-status-indicator');\n"
    "      var recInd = document.getElementById('mic-recording-indicator');\n"
    "      var srEl = document.getElementById('mic-samplerate');\n"
    "      var bdEl = document.getElementById('mic-bitdepth');\n"
    "      var chEl = document.getElementById('mic-channels');\n"
    "      var levelBar = document.getElementById('mic-level-bar');\n"
    "      var levelText = document.getElementById('mic-level-text');\n"
    "      if (statusInd) {\n"
    "        if (data.enabled) {\n"
    "          statusInd.className = 'status-indicator status-enabled';\n"
    "          statusInd.title = 'Active';\n"
    "        } else {\n"
    "          statusInd.className = 'status-indicator status-disabled';\n"
    "          statusInd.title = 'Stopped';\n"
    "        }\n"
    "      }\n"
    "      if (recInd) {\n"
    "        if (data.recording) {\n"
    "          recInd.className = 'status-indicator status-recording';\n"
    "          recInd.title = 'Recording';\n"
    "        } else {\n"
    "          recInd.className = 'status-indicator status-disabled';\n"
    "          recInd.title = 'Not recording';\n"
    "        }\n"
    "      }\n"
    "      if (srEl) srEl.textContent = data.sampleRate ? data.sampleRate + ' Hz' : '--';\n"
    "      if (bdEl) bdEl.textContent = data.bitDepth ? data.bitDepth + '-bit' : '--';\n"
    "      if (chEl) chEl.textContent = data.channels ? (data.channels == 1 ? 'Mono' : 'Stereo') : '--';\n"
    "      if (levelBar && data.level !== undefined) {\n"
    "        levelBar.style.width = data.level + '%';\n"
    "        if (data.level > 80) {\n"
    "          levelBar.style.backgroundColor = '#e74c3c';\n"
    "        } else if (data.level > 50) {\n"
    "          levelBar.style.backgroundColor = '#f39c12';\n"
    "        } else {\n"
    "          levelBar.style.backgroundColor = '#2ecc71';\n"
    "        }\n"
    "      }\n"
    "      if (levelText && data.level !== undefined) {\n"
    "        levelText.textContent = data.level + '%';\n"
    "      }\n"
    "      return data;\n"
    "    })\n"
    "    .catch(function(e) {\n"
    "      console.error('[Sensors] Microphone read error', e);\n"
    "      throw e;\n"
    "    });\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, "window._sensorDataIds = window._sensorDataIds || {};\nwindow._sensorDataIds['microphone'] = 'mic-status';\n", HTTPD_RESP_USE_STRLEN);
  
  // Add recordings loading function
  httpd_resp_send_chunk(req,
    "window.__lastRecCount = -1;\n"
    "window.loadMicRecordings = function() {\n"
    "  fetch('/api/recordings', {credentials:'include'})\n"
    "    .then(function(r){return r.json();})\n"
    "    .then(function(data){\n"
    "      var list = document.getElementById('mic-recordings-list');\n"
    "      var countEl = document.getElementById('mic-rec-count');\n"
    "      if(!list) return;\n"
    "      var count = data.count || 0;\n"
    "      if(countEl) countEl.textContent = '(' + count + ')';\n"
    "      if(window.__lastRecCount === count) return;\n"
    "      window.__lastRecCount = count;\n"
    "      list.innerHTML = '';\n"
    "      if(!data.files || data.files.length===0) {\n"
    "        list.innerHTML = '<div class=\"no-recordings\">No recordings</div>';\n"
    "        return;\n"
    "      }\n"
    "      data.files.forEach(function(f){\n"
    "        var item = document.createElement('div');\n"
    "        item.className = 'recording-item';\n"
    "        var sizeKB = Math.round(f.size/1024);\n"
    "        item.innerHTML = '<div class=\"rec-info\"><span class=\"rec-name\">' + f.name + '</span><span class=\"rec-size\">' + sizeKB + 'KB</span></div>' +\n"
    "          '<audio controls class=\"rec-audio\" preload=\"none\"><source src=\"/api/recordings/file?name=' + f.name + '\" type=\"audio/wav\"></audio>' +\n"
    "          '<button class=\"btn btn-sm btn-danger rec-delete\" data-name=\"' + f.name + '\">X</button>';\n"
    "        list.appendChild(item);\n"
    "      });\n"
    "      list.querySelectorAll('.rec-delete').forEach(function(btn){\n"
    "        btn.onclick = function(){\n"
    "          var name = this.getAttribute('data-name');\n"
    "          if(confirm('Delete ' + name + '?')){\n"
    "            fetch('/api/recordings/delete?name=' + name, {credentials:'include'})\n"
    "              .then(function(){window.__lastRecCount=-1;window.loadMicRecordings();});\n"
    "          }\n"
    "        };\n"
    "      });\n"
    "    })\n"
    "    .catch(function(e){console.error('Failed to load recordings',e);});\n"
    "};"
    "setTimeout(window.loadMicRecordings, 1000);\n"
    "setInterval(window.loadMicRecordings, 5000);\n", HTTPD_RESP_USE_STRLEN);
  
  // Add microphone settings event handlers
  httpd_resp_send_chunk(req,
    "function applyMicAdjustment(cmd) {\n"
    "  console.log('[Microphone] Applying adjustment:', cmd);\n"
    "  return fetch('/api/cli', {\n"
    "    method: 'POST',\n"
    "    headers: {'Content-Type': 'application/json'},\n"
    "    credentials: 'include',\n"
    "    body: JSON.stringify({command: cmd})\n"
    "  })\n"
    "  .then(function(r) { return r.text(); })\n"
    "  .then(function(result) {\n"
    "    console.log('[Microphone] Adjustment result:', result);\n"
    "    return result;\n"
    "  })\n"
    "  .catch(function(e) {\n"
    "    console.error('[Microphone] Adjustment failed:', e);\n"
    "  });\n"
    "}\n"
    "(function initMicSettings() {\n"
    "  var micSettingsToggle = document.getElementById('btn-mic-settings-toggle');\n"
    "  var micSettingsDiv = document.getElementById('mic-settings');\n"
    "  if (micSettingsToggle && micSettingsDiv) {\n"
    "    micSettingsToggle.onclick = function() {\n"
    "      micSettingsDiv.style.display = (micSettingsDiv.style.display === 'none') ? 'block' : 'none';\n"
    "    };\n"
    "  }\n"
    "  var micRecordingsToggle = document.getElementById('btn-mic-recordings-toggle');\n"
    "  var micRecordingsDiv = document.getElementById('mic-recordings');\n"
    "  if (micRecordingsToggle && micRecordingsDiv) {\n"
    "    micRecordingsToggle.onclick = function() {\n"
    "      micRecordingsDiv.style.display = (micRecordingsDiv.style.display === 'none') ? 'block' : 'none';\n"
    "    };\n"
    "  }\n"
    "  var gainSlider = document.getElementById('mic-gain-slider');\n"
    "  var gainValue = document.getElementById('mic-gain-value');\n"
    "  var sampleRateSelect = document.getElementById('mic-samplerate-select');\n"
    "  var bitDepthSelect = document.getElementById('mic-bitdepth-select');\n"
    "  if (gainSlider) {\n"
    "    gainSlider.addEventListener('input', function() {\n"
    "      if (gainValue) gainValue.textContent = this.value;\n"
    "    });\n"
    "    gainSlider.addEventListener('change', function() {\n"
    "      applyMicAdjustment('micgain ' + this.value);\n"
    "    });\n"
    "  }\n"
    "  if (sampleRateSelect) {\n"
    "    sampleRateSelect.addEventListener('change', function() {\n"
    "      applyMicAdjustment('micsamplerate ' + this.value);\n"
    "    });\n"
    "  }\n"
    "  if (bitDepthSelect) {\n"
    "    bitDepthSelect.addEventListener('change', function() {\n"
    "      applyMicAdjustment('micbitdepth ' + this.value);\n"
    "    });\n"
    "  }\n"
    "})();\n", HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Microphone sensor module ready');}catch(_){ }", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
}

// Dashboard definition for microphone sensor
inline void streamMicrophoneDashboardDef(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "window.__dashSensorDefs.push({device:'PDM',key:'mic',name:'Microphone (PDM)',desc:'ESP32-S3 PDM Microphone'});", HTTPD_RESP_USE_STRLEN);
}

// Stream microphone CSS
inline void streamMicrophoneSensorCSS(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"rawliteral(
<style>
.vu-meter-container {
  display: flex;
  align-items: center;
  gap: 10px;
  margin: 10px 0;
}
.vu-meter {
  flex: 1;
  height: 20px;
  background: #333;
  border-radius: 10px;
  overflow: hidden;
}
.vu-meter-fill {
  height: 100%;
  background: #2ecc71;
  transition: width 0.1s ease, background-color 0.2s ease;
}
.vu-meter-label {
  min-width: 40px;
  text-align: right;
  font-weight: bold;
}
.recordings-list {
  max-height: 300px;
  overflow-y: auto;
}
.recording-item {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px;
  background: rgba(0,0,0,0.2);
  border-radius: 4px;
  margin-bottom: 6px;
  border: 1px solid rgba(255,255,255,0.1);
}
.rec-info {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
  flex-shrink: 1;
}
.rec-name {
  font-size: 13px;
  font-weight: 500;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.rec-size {
  font-size: 11px;
  color: #888;
}
.rec-audio {
  height: 32px;
  flex-shrink: 0;
}
.rec-delete {
  padding: 4px 8px;
  font-size: 12px;
  flex-shrink: 0;
  min-width: 32px;
}
.no-recordings {
  color: #666;
  font-style: italic;
  padding: 10px;
}
</style>
)rawliteral", HTTPD_RESP_USE_STRLEN);
}

#endif // ENABLE_MICROPHONE_SENSOR

#endif // SYSTEM_MICROPHONE_WEB_H
