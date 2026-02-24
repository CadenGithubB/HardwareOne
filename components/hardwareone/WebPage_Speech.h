#ifndef WEBPAGE_SPEECH_H
#define WEBPAGE_SPEECH_H

#include <Arduino.h>
#include "WebServer_Utils.h"
#include "System_BuildConfig.h"
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif

// Registration function - registers /speech URI handler
#if ENABLE_HTTP_SERVER
void registerSpeechPageHandlers(httpd_handle_t server);
#else
inline void registerSpeechPageHandlers(httpd_handle_t) {}
#endif

// Streamed inner content for Speech Recognition page
inline void streamSpeechInner(httpd_req_t* req) {
#if !ENABLE_ESP_SR
  // ESP-SR not compiled - show disabled message
  httpd_resp_send_chunk(req, R"HTML(
<div style='text-align:center;padding:2rem'>
  <h2 style='color:var(--warning);margin-bottom:1rem'>Speech Recognition Disabled</h2>
  <p style='color:var(--panel-fg);margin-bottom:2rem'>
    ESP-SR speech recognition has been disabled during firmware compilation.
    This feature requires ESP32-S3 with PSRAM.
  </p>
  
  <div style='background:var(--crumb-bg);padding:1.5rem;border-radius:10px;border:1px solid var(--border);max-width:500px;margin:0 auto;text-align:left'>
    <h3 style='color:var(--panel-fg);margin:0 0 1rem 0;font-size:1rem'>To Enable Speech Recognition:</h3>
    <p style='color:var(--panel-fg);font-size:0.9rem;margin:0'>
      Recompile the firmware with <code style='background:rgba(0,0,0,0.1);padding:2px 6px;border-radius:3px'>ENABLE_ESP_SR=1</code> 
      in your build configuration. Requires ESP32-S3 target with PSRAM enabled.
    </p>
  </div>
  
  <div style='margin-top:2rem'>
    <a href='/' class='btn'>← Back to Dashboard</a>
    <a href='/settings' class='btn' style='margin-left:1rem'>Settings</a>
  </div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);
  return;
#endif

  // CSS styling
  httpd_resp_send_chunk(req, R"CSS(
<style>
.sr-container { max-width: 960px; margin: 0 auto; padding: 20px; }
.sr-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 20px; }
.sr-card { background: var(--panel-bg); border-radius: 15px; padding: 20px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); border: 1px solid var(--border); overflow: hidden; }
.sr-title { font-size: 1.3em; font-weight: bold; margin-bottom: 10px; color: var(--panel-fg); display: flex; align-items: center; gap: 10px; }
.sr-description { color: var(--muted); margin-bottom: 15px; font-size: 0.9em; }
.sr-controls { display: flex; flex-wrap: wrap; gap: 10px; margin-bottom: 15px; }
.sr-status { background: var(--crumb-bg); border-radius: 8px; padding: 15px; font-family: 'Courier New', monospace; font-size: 0.9em; border-left: 4px solid var(--link); min-height: 80px; color: var(--panel-fg); white-space: pre-wrap; }
.status-indicator { display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }
.status-running { background: #28a745; animation: pulse 2s infinite; }
.status-wake { background: #ffc107; animation: pulse-fast 0.5s infinite; }
.status-stopped { background: #dc3545; }
@keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.5; } 100% { opacity: 1; } }
@keyframes pulse-fast { 0% { opacity: 1; } 50% { opacity: 0.3; } 100% { opacity: 1; } }
.sr-stats { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; margin-top: 15px; }
.sr-stat { background: var(--crumb-bg); border-radius: 8px; padding: 12px; text-align: center; }
.sr-stat-value { font-size: 1.5em; font-weight: bold; color: var(--link); }
.sr-stat-label { font-size: 0.8em; color: var(--muted); margin-top: 4px; }
.sr-log { background: #1a1a2e; color: #0f0; border-radius: 8px; padding: 12px; font-family: 'Courier New', monospace; font-size: 0.85em; max-height: 200px; overflow-y: auto; margin-top: 15px; }
.sr-log-entry { margin: 4px 0; }
.sr-log-wake { color: #ffc107; }
.sr-log-cmd { color: #28a745; }
.sr-info { background:rgba(100,149,237,0.1); border:1px solid rgba(100,149,237,0.3); color:var(--panel-fg); border-radius:6px; padding:10px 12px; font-size:0.85em; margin-top:10px; }
.audio-meter { height:24px; background:#1a1a2e; border-radius:4px; overflow:hidden; position:relative; margin:10px 0; }
.audio-meter-bar { height:100%; background:linear-gradient(90deg,#28a745 0%,#28a745 60%,#ffc107 60%,#ffc107 80%,#dc3545 80%,#dc3545 100%); width:0%; transition:width 0.1s ease-out; }
.audio-meter-label { position:absolute; right:8px; top:50%; transform:translateY(-50%); font-size:0.8em; color:#fff; text-shadow:0 0 4px #000; }
.vad-indicator { display:inline-block; width:10px; height:10px; border-radius:50%; margin-right:6px; background:#444; }
.vad-active { background:#28a745; box-shadow:0 0 8px #28a745; }
</style>
)CSS", HTTPD_RESP_USE_STRLEN);

  // HTML structure
  httpd_resp_send_chunk(req, R"HTML(
<div class='sr-container'>
  <div class='sr-grid'>
    <!-- Status Card -->
    <div class='sr-card'>
      <div class='sr-title'>
        <span>🎤 Speech Recognition</span>
        <span class='status-indicator status-stopped' id='sr-status-indicator'></span>
      </div>
      <div class='sr-description'>
        ESP-SR wake word detection and command recognition. Say the wake word to activate, then speak a command.
      </div>
      <div class='sr-controls'>
        <button class='btn' id='btn-sr-start'>Start SR</button>
        <button class='btn' id='btn-sr-stop' style='display:none'>Stop SR</button>
        <button class='btn' id='btn-sr-refresh'>Refresh Status</button>
      </div>
      <div class='sr-status' id='sr-status-box'>Click "Refresh Status" to query speech recognition state...</div>
      
      <!-- Audio Level Meter -->
      <div style='margin-top:15px'>
        <div style='display:flex;align-items:center;margin-bottom:6px'>
          <span class='vad-indicator' id='sr-vad-indicator'></span>
          <span style='font-size:0.85em;color:var(--panel-fg)'>Audio Level</span>
          <span style='margin-left:auto;font-size:0.85em;color:var(--panel-fg)'>Gain: <span id='sr-micgain-display'>--</span>%</span>
        </div>
        <div class='audio-meter'>
          <div class='audio-meter-bar' id='sr-audio-bar'></div>
          <span class='audio-meter-label' id='sr-audio-db'>-- dB</span>
        </div>
      </div>
      
      <div class='sr-stats'>
        <div class='sr-stat'>
          <div class='sr-stat-value' id='sr-wake-count'>0</div>
          <div class='sr-stat-label'>Wake Words</div>
        </div>
        <div class='sr-stat'>
          <div class='sr-stat-value' id='sr-cmd-count'>0</div>
          <div class='sr-stat-label'>Commands</div>
        </div>
        <div class='sr-stat'>
          <div class='sr-stat-value' id='sr-confidence'>-</div>
          <div class='sr-stat-label'>Last Confidence</div>
        </div>
        <div class='sr-stat'>
          <div class='sr-stat-value' id='sr-rejects'>0</div>
          <div class='sr-stat-label'>Rejects</div>
        </div>
      </div>
      
      <!-- Voice State Display -->
      <div id='sr-voice-state' style='margin-top:15px;padding:12px;background:var(--crumb-bg);border-radius:8px;display:none'>
        <div style='font-size:0.85em;color:var(--panel-fg);margin-bottom:6px'>Voice Navigation:</div>
        <div id='sr-voice-path' style='font-family:monospace;font-size:1.1em;color:var(--link)'></div>
      </div>
    </div>
    
    <!-- Detection Log Card -->
    <div class='sr-card'>
      <div class='sr-title'>Detection Log</div>
      <div class='sr-description'>
        Recent wake word and command detections. Log updates automatically when SR is running.
      </div>
      <div class='sr-log' id='sr-log'>
        <div class='sr-log-entry' style='color:#666'>No detections yet...</div>
      </div>
      <div class='sr-info'>
        <strong>Tip:</strong> Place models in <code>/sd/ESP-SR Models/</code> for custom wake words and commands.
        Use <code>commands.txt</code> to define custom voice commands.
      </div>
    </div>
  </div>
  
  <!-- Debug & Tuning Card -->
  <div class='sr-card' style='margin-top:20px'>
    <div class='sr-title'>Debug & Tuning</div>
    <div class='sr-description'>
      Audio gain settings and debug options to improve recognition accuracy.
    </div>
    
    <!-- Current Tuning Display -->
    <div id='sr-tuning-status' style='background:var(--crumb-bg);border-radius:8px;padding:12px;margin-bottom:15px;font-family:monospace;font-size:0.85em;color:var(--panel-fg)'>
      Loading tuning status...
    </div>
    
    <!-- Quick Controls -->
    <div style='display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:15px'>
      <div>
        <label style='display:block;margin-bottom:6px;font-size:0.85em;color:var(--panel-fg)'>Raw Output Mode</label>
        <button class='btn' id='btn-sr-raw-toggle' style='width:100%'>Enable Raw</button>
      </div>
      <div>
        <label style='display:block;margin-bottom:6px;font-size:0.85em;color:var(--panel-fg)'>Auto-Tune</label>
        <button class='btn' id='btn-sr-autotune-toggle' style='width:100%'>Start Auto-Tune</button>
      </div>
    </div>
    
    <!-- Gain Controls -->
    <div style='border-top:1px solid var(--border);padding-top:15px'>
      <label style='display:block;margin-bottom:8px;font-size:0.9em;color:var(--panel-fg)'>Manual Gain Adjustment</label>
      <div style='display:grid;grid-template-columns:1fr 1fr;gap:10px'>
        <div>
          <label style='font-size:0.8em;color:var(--panel-fg)'>AFE Gain</label>
          <select id='sr-afe-gain' style='width:100%;padding:6px;border-radius:4px;border:1px solid var(--border);background:var(--panel-bg);color:var(--panel-fg)'>
            <option value='1.0'>1.0x (Default)</option>
            <option value='2.0'>2.0x</option>
            <option value='3.0'>3.0x</option>
            <option value='4.0'>4.0x</option>
            <option value='5.0'>5.0x</option>
          </select>
        </div>
        <div>
          <label style='font-size:0.8em;color:var(--panel-fg)'>Dynamic Gain Max</label>
          <select id='sr-dyngain-max' style='width:100%;padding:6px;border-radius:4px;border:1px solid var(--border);background:var(--panel-bg);color:var(--panel-fg)'>
            <option value='0'>Disabled</option>
            <option value='1.5'>1.5x</option>
            <option value='2.0'>2.0x</option>
            <option value='2.5'>2.5x (Default)</option>
            <option value='3.0'>3.0x</option>
          </select>
        </div>
      </div>
      <button class='btn' id='btn-sr-apply-gain' style='width:100%;margin-top:10px'>Apply Gain Settings</button>
    </div>
    
    <div class='sr-info' style='margin-top:12px'>
      <strong>Tips:</strong> Raw mode shows all MultiNet hypotheses. Auto-tune cycles through gain presets to find optimal settings.
    </div>
  </div>
  
  <!-- Models Card -->
  <div class='sr-card' style='margin-top:20px'>
    <div class='sr-title'>🧠 Models</div>
    <div class='sr-description'>
      Loaded speech recognition models. Wake word model detects activation phrase, MultiNet recognizes commands.
    </div>
    <div id='sr-models-info' style='color:var(--panel-fg)'>Loading model information...</div>
    
    <!-- File Explorer Toggle -->
    <div style='margin-top:15px'>
      <button class='btn' id='btn-sr-files-toggle' style='width:100%;background:var(--panel-bg);border:1px solid var(--border)'>Model Files</button>
    </div>
    
    <!-- File Explorer Section -->
    <div id='sr-files-section' style='display:none;margin-top:10px;padding:15px;background:var(--crumb-bg);border:1px solid var(--border);border-radius:8px'>
      <div style='margin-bottom:12px'>
        <label style='display:block;margin-bottom:6px;font-size:0.9em;color:var(--panel-fg)'>Model Directory:</label>
        <select id='sr-model-select' style='width:100%;padding:8px;border-radius:4px;border:1px solid var(--border);background:var(--panel-bg);color:var(--panel-fg)'>
          <option value=''>-- Select Model File --</option>
        </select>
        <div style='display:flex;gap:8px;margin-top:8px;flex-wrap:wrap'>
          <button class='btn' id='btn-sr-load-model'>Load</button>
          <button class='btn' id='btn-sr-refresh-models'>Refresh</button>
          <button class='btn' id='btn-sr-open-folder'>Open Folder</button>
        </div>
        <div id='sr-file-status' style='font-size:0.85em;margin-top:6px;color:var(--panel-fg)'></div>
      </div>
      
      <!-- File List -->
      <div style='margin-top:15px;padding-top:12px;border-top:1px solid var(--border)'>
        <label style='display:block;margin-bottom:6px;font-size:0.9em;color:var(--panel-fg)'>Files in /sd/ESP-SR Models/:</label>
        <div id='sr-file-list' style='background:var(--panel-bg);border:1px solid var(--border);border-radius:4px;max-height:150px;overflow-y:auto;font-family:monospace;font-size:0.85em'>
          <div style='padding:8px;color:var(--panel-fg)'>Click Refresh to load file list...</div>
        </div>
      </div>
      
      <!-- Upload Section -->
      <div style='margin-top:15px;padding-top:12px;border-top:1px solid var(--border)'>
        <label style='display:block;margin-bottom:6px;font-size:0.9em;color:var(--panel-fg)'>Upload Model File:</label>
        <input type='file' id='sr-model-file' accept='.wn,.bin,.txt' style='width:100%;margin-bottom:8px;color:var(--panel-fg)'>
        <button class='btn' id='btn-sr-upload-model' style='width:100%'>Upload Model</button>
        <div id='sr-upload-status' style='font-size:0.85em;margin-top:6px;color:var(--panel-fg)'></div>
      </div>
      
      <div class='sr-info' style='margin-top:12px'>
        <strong>Model Files:</strong><br>
        • <code>.wn</code> - Wake word model files<br>
        • <code>commands.txt</code> - Custom command definitions<br>
        • Place files in <code>/sd/ESP-SR Models/</code>
      </div>
    </div>
  </div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  // JavaScript
  httpd_resp_send_chunk(req, R"JS(
<script>
(function(){
  var logEntries = [];
  var maxLogEntries = 20;
  var pollInterval = null;
  
  function postCli(cmd){
    return fetch('/api/cli',{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      credentials:'same-origin',
      body:'cmd='+encodeURIComponent(cmd)
    }).then(function(r){return r.text();});
  }
  
  function setStatus(text){
    var box=document.getElementById('sr-status-box');
    if(box) box.textContent=text||'';
  }
  
  function setIndicator(state){
    var el=document.getElementById('sr-status-indicator');
    if(!el) return;
    el.className='status-indicator';
    if(state==='wake') el.classList.add('status-wake');
    else if(state==='running') el.classList.add('status-running');
    else el.classList.add('status-stopped');
  }
  
  function updateButtons(running){
    var startBtn=document.getElementById('btn-sr-start');
    var stopBtn=document.getElementById('btn-sr-stop');
    if(startBtn) startBtn.style.display=running?'none':'inline-block';
    if(stopBtn) stopBtn.style.display=running?'inline-block':'none';
  }
  
  function addLogEntry(type, message){
    var timestamp = new Date().toLocaleTimeString();
    logEntries.unshift({type:type, message:message, time:timestamp});
    if(logEntries.length > maxLogEntries) logEntries.pop();
    renderLog();
  }
  
  function renderLog(){
    var logEl = document.getElementById('sr-log');
    if(!logEl) return;
    if(logEntries.length === 0){
      logEl.innerHTML = '<div class="sr-log-entry" style="color:#666">No detections yet...</div>';
      return;
    }
    var html = '';
    for(var i=0; i<logEntries.length; i++){
      var e = logEntries[i];
      var cls = e.type==='wake'?'sr-log-wake':(e.type==='cmd'?'sr-log-cmd':'');
      html += '<div class="sr-log-entry ' + cls + '">[' + e.time + '] ' + e.message + '</div>';
    }
    logEl.innerHTML = html;
  }
  
  function parseStatus(json){
    try {
      var data = JSON.parse(json);
      var running = data.running;
      var wakeActive = data.wakeActive;
      
      // Update indicator
      if(wakeActive) setIndicator('wake');
      else if(running) setIndicator('running');
      else setIndicator('stopped');
      
      // Update buttons
      updateButtons(running);
      
      // Update stats
      var wakeEl = document.getElementById('sr-wake-count');
      var cmdEl = document.getElementById('sr-cmd-count');
      var confEl = document.getElementById('sr-confidence');
      var rejectEl = document.getElementById('sr-rejects');
      if(wakeEl) wakeEl.textContent = data.wakeCount || 0;
      if(cmdEl) cmdEl.textContent = data.commandCount || 0;
      if(confEl) confEl.textContent = data.lastConfidence ? Math.round(data.lastConfidence * 100) + '%' : '-';
      if(rejectEl) rejectEl.textContent = data.lowConfidenceRejects || 0;
      
      // Update audio level meter
      var audioBar = document.getElementById('sr-audio-bar');
      var audioDb = document.getElementById('sr-audio-db');
      var vadInd = document.getElementById('sr-vad-indicator');
      var micgainDisp = document.getElementById('sr-micgain-display');
      if(audioBar && audioDb){
        var volDb = data.volumeDb || -60;
        // Map dB to percentage: -60dB=0%, 0dB=100%
        var pct = Math.max(0, Math.min(100, ((volDb + 60) / 60) * 100));
        audioBar.style.width = pct + '%';
        audioDb.textContent = volDb.toFixed(1) + ' dB';
      }
      if(vadInd){
        if(data.vadState && data.vadState > 0) vadInd.classList.add('vad-active');
        else vadInd.classList.remove('vad-active');
      }
      if(micgainDisp) micgainDisp.textContent = data.micgain || '--';
      
      // Update voice state display
      var stateEl = document.getElementById('sr-voice-state');
      var pathEl = document.getElementById('sr-voice-path');
      if(stateEl && pathEl){
        var state = data.state || 'idle';
        if(state !== 'idle' && running){
          stateEl.style.display = 'block';
          var path = '🎤 ';
          if(data.category) path += data.category;
          if(data.subcategory) path += ' → ' + data.subcategory;
          path += ' → [awaiting ' + state + ']';
          pathEl.textContent = path;
        } else {
          stateEl.style.display = 'none';
        }
      }
      
      // Update status text
      var statusLines = [];
      statusLines.push('Status: ' + (running ? 'RUNNING' : 'STOPPED'));
      if(running){
        var stateStr = data.state || 'idle';
        if(stateStr === 'idle') statusLines.push('State: Waiting for wake word');
        else if(stateStr === 'category') statusLines.push('State: Listening for CATEGORY...');
        else if(stateStr === 'subcategory') statusLines.push('State: Listening for SUBCATEGORY...');
        else if(stateStr === 'target') statusLines.push('State: Listening for TARGET...');
      }
      if(data.lastCommand) statusLines.push('Last: ' + data.lastCommand + ' (' + Math.round((data.lastConfidence||0)*100) + '%)');
      setStatus(statusLines.join('\n'));
      
      // Update models info
      var modelsEl = document.getElementById('sr-models-info');
      if(modelsEl){
        var modelsHtml = '<div style="display:grid;grid-template-columns:1fr 1fr;gap:10px">';
        modelsHtml += '<div><strong>Wake Word (AFE):</strong> ' + (data.hasAFE ? '✓ Loaded' : '✗ Not loaded') + '</div>';
        modelsHtml += '<div><strong>Commands (MultiNet):</strong> ' + (data.hasMultiNet ? '✓ Loaded' : '✗ Disabled') + '</div>';
        modelsHtml += '</div>';
        modelsEl.innerHTML = modelsHtml;
      }
      
      // Sync debug button states
      if(typeof rawEnabled !== 'undefined'){
        rawEnabled = data.rawOutput || false;
        updateRawButton();
      }
      if(typeof autotuneActive !== 'undefined'){
        autotuneActive = data.autotuneActive || false;
        updateAutotuneButton();
      }
      
      return data;
    } catch(e) {
      setStatus('Error parsing status: ' + e.message);
      return null;
    }
  }
  
  var lastWakeCount = 0;
  var lastCmdCount = 0;
  
  function refreshStatus(){
    postCli('sr status').then(function(out){
      var data = parseStatus(out);
      if(data){
        // Check for new detections
        if(data.wakeCount > lastWakeCount){
          addLogEntry('wake', 'Wake word detected!');
          lastWakeCount = data.wakeCount;
        }
        if(data.commandCount > lastCmdCount){
          addLogEntry('cmd', 'Command: ' + (data.lastCommand || 'unknown'));
          lastCmdCount = data.commandCount;
        }
      }
    }).catch(function(e){
      setStatus('Error: ' + e.message);
    });
  }
  
  function startSR(){
    setStatus('Starting speech recognition...');
    postCli('sr start').then(function(out){
      setStatus(out);
      setTimeout(refreshStatus, 500);
      startPolling();
    });
  }
  
  function stopSR(){
    setStatus('Stopping speech recognition...');
    postCli('sr stop').then(function(out){
      setStatus(out);
      setTimeout(refreshStatus, 500);
      stopPolling();
    });
  }
  
  function startPolling(){
    if(pollInterval) return;
    pollInterval = setInterval(refreshStatus, 2000);
  }
  
  function stopPolling(){
    if(pollInterval){
      clearInterval(pollInterval);
      pollInterval = null;
    }
  }
  
  // Event handlers
  document.getElementById('btn-sr-start').onclick = startSR;
  document.getElementById('btn-sr-stop').onclick = stopSR;
  document.getElementById('btn-sr-refresh').onclick = refreshStatus;
  
  // File explorer toggle
  document.getElementById('btn-sr-files-toggle').onclick = function(){
    var panel = document.getElementById('sr-files-section');
    if(panel) panel.style.display = (panel.style.display === 'none') ? 'block' : 'none';
  };
  
  // Model file functions
  function setFileStatus(msg){ 
    var el = document.getElementById('sr-file-status'); 
    if(el) el.textContent = msg; 
  }
  function setUploadStatus(msg){ 
    var el = document.getElementById('sr-upload-status'); 
    if(el) el.textContent = msg; 
  }
  
  function refreshModelList(){
    setFileStatus('Loading files...');
    postCli('ls /sd/ESP-SR Models').then(function(out){
      var lines = out.split('\n').filter(function(l){ return l.trim().length > 0; });
      var select = document.getElementById('sr-model-select');
      var fileList = document.getElementById('sr-file-list');
      
      // Parse file entries
      var files = [];
      for(var i=0; i<lines.length; i++){
        var line = lines[i].trim();
        if(line.indexOf('ERROR') >= 0 || line.indexOf('not found') >= 0) continue;
        // Extract filename from ls output (format varies)
        var parts = line.split(/\s+/);
        var fname = parts[parts.length - 1];
        if(fname && fname !== '.' && fname !== '..') files.push(fname);
      }
      
      // Update dropdown
      if(select){
        select.innerHTML = '<option value="">-- Select Model File --</option>';
        for(var j=0; j<files.length; j++){
          var opt = document.createElement('option');
          opt.value = files[j];
          opt.textContent = files[j];
          select.appendChild(opt);
        }
      }
      
      // Update file list display
      if(fileList){
        if(files.length === 0){
          fileList.innerHTML = '<div style="padding:8px;color:var(--panel-fg)">No files found. Upload models to get started.</div>';
        } else {
          var html = '';
          for(var k=0; k<files.length; k++){
            var f = files[k];
            var icon = '';
            if(f.endsWith('.wn')) icon = '';
            else if(f.endsWith('.txt')) icon = '';
            else if(f.endsWith('.bin')) icon = '';
            html += '<div style="padding:6px 8px;border-bottom:1px solid var(--border)">' + icon + ' ' + f + '</div>';
          }
          fileList.innerHTML = html;
        }
      }
      
      setFileStatus('Found ' + files.length + ' file(s)');
    }).catch(function(e){
      setFileStatus('Error: ' + e.message);
    });
  }
  
  function loadSelectedModel(){
    var select = document.getElementById('sr-model-select');
    if(!select || !select.value){
      setFileStatus('Please select a model file first');
      return;
    }
    var filename = select.value;
    setFileStatus('Loading ' + filename + '...');
    // For now, just show info - actual loading depends on model type
    if(filename.endsWith('.wn')){
      postCli('sr loadwake /sd/ESP-SR Models/' + filename).then(function(out){
        setFileStatus(out);
        setTimeout(refreshStatus, 500);
      }).catch(function(e){ setFileStatus('Error: ' + e.message); });
    } else if(filename === 'commands.txt'){
      postCli('sr loadcmds /sd/ESP-SR Models/' + filename).then(function(out){
        setFileStatus(out);
        setTimeout(refreshStatus, 500);
      }).catch(function(e){ setFileStatus('Error: ' + e.message); });
    } else {
      setFileStatus('Selected: ' + filename + ' (use sr commands to load)');
    }
  }
  
  function openModelFolder(){
    window.location.href = '/files?path=/sd/ESP-SR%20Models';
  }
  
  function uploadModelFile(){
    var fileInput = document.getElementById('sr-model-file');
    if(!fileInput || !fileInput.files || !fileInput.files.length){
      setUploadStatus('Please select a file first');
      return;
    }
    var file = fileInput.files[0];
    setUploadStatus('Uploading ' + file.name + '...');
    
    var formData = new FormData();
    formData.append('file', file);
    formData.append('path', '/sd/ESP-SR Models/' + file.name);
    
    fetch('/api/upload', {
      method: 'POST',
      credentials: 'same-origin',
      body: formData
    }).then(function(r){
      if(!r.ok) throw new Error('Upload failed: ' + r.status);
      return r.text();
    }).then(function(out){
      setUploadStatus('Upload complete: ' + file.name);
      fileInput.value = '';
      setTimeout(refreshModelList, 500);
    }).catch(function(e){
      setUploadStatus('Error: ' + e.message);
    });
  }
  
  document.getElementById('btn-sr-refresh-models').onclick = refreshModelList;
  document.getElementById('btn-sr-load-model').onclick = loadSelectedModel;
  document.getElementById('btn-sr-open-folder').onclick = openModelFolder;
  document.getElementById('btn-sr-upload-model').onclick = uploadModelFile;
  
  // Debug & Tuning controls
  var rawEnabled = false;
  var autotuneActive = false;
  
  function updateTuningStatus(){
    postCli('sr tuning').then(function(out){
      var el = document.getElementById('sr-tuning-status');
      if(el) el.textContent = out;
    });
  }
  
  function updateRawButton(){
    var btn = document.getElementById('btn-sr-raw-toggle');
    if(btn){
      btn.textContent = rawEnabled ? 'Disable Raw' : 'Enable Raw';
      btn.style.background = rawEnabled ? 'var(--warning)' : '';
    }
  }
  
  function updateAutotuneButton(){
    var btn = document.getElementById('btn-sr-autotune-toggle');
    if(btn){
      btn.textContent = autotuneActive ? 'Stop Auto-Tune' : 'Start Auto-Tune';
      btn.style.background = autotuneActive ? 'var(--warning)' : '';
    }
  }
  
  document.getElementById('btn-sr-raw-toggle').onclick = function(){
    var cmd = rawEnabled ? 'sr raw off' : 'sr raw on';
    postCli(cmd).then(function(out){
      rawEnabled = !rawEnabled;
      updateRawButton();
      updateTuningStatus();
    });
  };
  
  document.getElementById('btn-sr-autotune-toggle').onclick = function(){
    var cmd = autotuneActive ? 'sr autotune stop' : 'sr autotune start';
    postCli(cmd).then(function(out){
      autotuneActive = !autotuneActive;
      updateAutotuneButton();
      updateTuningStatus();
    });
  };
  
  document.getElementById('btn-sr-apply-gain').onclick = function(){
    var afeGain = document.getElementById('sr-afe-gain').value;
    var dynMax = document.getElementById('sr-dyngain-max').value;
    
    // Apply AFE gain via tuning command
    postCli('sr tuning gain ' + afeGain).then(function(){
      // Apply dynamic gain
      if(dynMax === '0'){
        return postCli('sr dyngain off');
      } else {
        return postCli('sr dyngain on ' + dynMax);
      }
    }).then(function(){
      updateTuningStatus();
    });
  };
  
  // Initial status check
  refreshStatus();
  updateTuningStatus();
  
  // Start polling if already running
  postCli('sr status').then(function(out){
    try {
      var data = JSON.parse(out);
      if(data.running) startPolling();
    } catch(e){}
  });
})();
</script>
)JS", HTTPD_RESP_USE_STRLEN);
}

#endif // WEBPAGE_SPEECH_H
