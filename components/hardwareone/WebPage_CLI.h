#ifndef WEBPAGE_CLI_H
#define WEBPAGE_CLI_H

// Streamed inner content for the CLI page
inline void streamCLIInner(httpd_req_t* req, const String& username) {
  // CSS
  httpd_resp_send_chunk(req, R"CSS(
<style>
  html, body { height: 100vh; overflow: hidden; }
  .cli-container {
    background: rgba(0, 0, 0, 0.3);
    border-radius: 15px;
    padding: 12px;
    backdrop-filter: blur(10px);
    border: 1px solid rgba(255, 255, 255, 0.1);
    box-shadow: 0 20px 40px rgba(0, 0, 0, 0.2);
    font-family: 'Courier New', monospace;
    width: 95%;
    max-width: 1400px;
    margin: 0 auto;
    height: 62vh;
    max-height: 75vh;
    min-height: 45vh;
    overflow: hidden;
    display: flex;
    flex-direction: column;
  }
  .cli-header {
    text-align: center;
    font-size: 1.1em;
    margin-bottom: 6px;
    color: #4CAF50;
    font-weight: bold;
  }
  .cli-output {
    background: rgba(0, 0, 0, 0.5);
    border: 1px solid #333;
    border-radius: 5px;
    padding: 8px;
    flex: 1 1 auto;
    min-height: 60px;
    overflow-y: auto;
    margin-bottom: 6px;
    font-size: 14px;
    line-height: 1.4;
    white-space: pre-wrap;
    color: #fff;
    scroll-behavior: smooth;
  }
  .cli-input-container {
    display: flex;
    align-items: center;
    gap: 8px;
    flex: 0 0 auto;
    min-height: 30px;
    margin-top: 2px;
  }
  .cli-prompt {
    color: #4CAF50;
    font-weight: bold;
  }
  .cli-input {
    flex: 1 1 260px;
    min-width: 140px;
    width: auto;
    background: rgba(255,255,255,0.08);
    border: 1px solid rgba(255,255,255,0.25);
    color: #fff;
    font-family: 'Courier New', monospace;
    font-size: 14px;
    outline: none;
    padding: 6px 8px;
    height: 34px;
    margin-bottom: 0;
    display: block;
    position: relative;
    z-index: 2;
    pointer-events: auto;
    box-sizing: border-box;
  }
  .help-text { display:none; }
  @media (min-width: 1200px) { .content { max-width: 1600px; } }
  @media (min-width: 1600px) { .content { max-width: 90vw; } }
  @media (max-height: 820px) { .cli-container { height: 60vh; max-height: 68vh; padding: 8px; } .cli-header { font-size: 1.0em; margin-bottom: 4px; } .cli-output { padding: 6px; margin-bottom: 4px; min-height: 50px; } .cli-input-container { min-height: 28px; } }
  @media (max-height: 700px) { .cli-container { height: 55vh; max-height: 60vh; padding: 6px; } .cli-header { font-size: 0.95em; margin-bottom: 4px; } .cli-output { padding: 4px; margin-bottom: 4px; min-height: 40px; } .cli-input-container { gap: 6px; min-height: 26px; } }
</style>
)CSS", HTTPD_RESP_USE_STRLEN);

  // HTML structure
  String usernameEscaped = username;
  usernameEscaped.replace("<", "&lt;");
  usernameEscaped.replace(">", "&gt;");
  
  httpd_resp_send_chunk(req, R"HTML(
<div class='cli-container'>
  <div class='cli-header'>HardwareOne Command Line Interface</div>
  <script>try{console.log('[CLI] Section Header ready');}catch(_){}</script>
  <div id='cli-output' class='cli-output'></div>
  <script>try{console.log('[CLI] Section Output ready');}catch(_){}</script>
  <div class='cli-input-container'>
    <span class='cli-prompt'>$</span>
    <input type='text' id='cli-input' name='command' class='cli-input' placeholder='Enter command...' autocomplete='off'>
    <button id='cli-exec' class='btn'>Execute</button>
  </div>
  <script>try{console.log('[CLI] Section Input ready');}catch(_){}</script>
  <div class='help-text'>Press Enter to execute commands | Type 'help' for command list | Authenticated as: )HTML", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, usernameEscaped.c_str(), HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, R"HTML(</div>
  <script>try{console.log('[CLI] Section HelpText ready');}catch(_){}</script>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  // JS: Core init
  httpd_resp_send_chunk(req, R"JS(
<script>
try{console.log('[CLI] Core init start');}catch(_){}
var cliInput = document.getElementById('cli-input');
var cliOutput = document.getElementById('cli-output');
var cliExecBtn = document.getElementById('cli-exec');
window.addEventListener('error', function(e){ try { if(cliOutput){ cliOutput.textContent += ('[JS Error] ' + e.message + '\n'); } } catch(_){} });
var commandHistory = []; var historyIndex = -1; var currentCommand=''; var outputHistory=''; var inHelp=false; var outputBackup=''; var scrolledOnce=false;
if(cliExecBtn){ cliExecBtn.addEventListener('click', function(){ if(window.executeCommand) executeCommand(); }); }
if(cliInput){ cliInput.addEventListener('keydown', function(e){ if(e.key==='Enter' && window.executeCommand){ executeCommand(); } }); }
try{console.log('[CLI] Core init ready');}catch(_){}
</script>
)JS", HTTPD_RESP_USE_STRLEN);

  // JS: Session/init
  httpd_resp_send_chunk(req, R"JS(
<script>
try{console.log('[CLI] Session/init start');}catch(_){}
try{ commandHistory = JSON.parse(localStorage.getItem('cliHistory') || '[]'); }catch(_){ commandHistory = []; }
historyIndex = -1; currentCommand = '';
try{ inHelp = JSON.parse(localStorage.getItem('cliInHelp') || 'false'); }catch(_){ inHelp=false; }
try{ outputBackup = localStorage.getItem('cliOutputHistoryBackup') || ''; }catch(_){ outputBackup=''; }
function __stripAnsi(s){ try{ return (s||'').replace(/\x1B\[[0-9;]*[A-Za-z]/g, ''); }catch(_){ return s; } }
function __applyClear(s){ try{ var ESC=String.fromCharCode(27); var clearSeq=ESC+'[2J'+ESC+'[H'; var idx=(s||'').lastIndexOf(clearSeq); if(idx!==-1){ return s.substring(idx+clearSeq.length); } return s; }catch(_){ return s; } }
try{ fetch('/api/cli/logs', { credentials: 'same-origin', cache:'no-store' })
.then(function(r){ return r.text(); })
.then(function(text){ var t=__applyClear(text); t=__stripAnsi(t); if(cliOutput){ cliOutput.textContent = t || ''; try{ localStorage.setItem('cliOutputHistory', cliOutput.textContent); }catch(_){} try{ if(!scrolledOnce){ cliOutput.scrollTop = cliOutput.scrollHeight; scrolledOnce = true; } }catch(_){} } })
.catch(function(e){ try { console.debug('[CLI] logs fetch error: ' + e.message); } catch(_){} }); }catch(_){ }
try {
  if (window.__cliPoller) { try{ clearInterval(window.__cliPoller); }catch(_){} }
  window.__cliPoller = setInterval(function(){
    fetch('/api/cli/logs', { credentials: 'same-origin', cache: 'no-store' })
      .then(function(r){ if(r.status===401){ if(window.__cliPoller){ clearInterval(window.__cliPoller); window.__cliPoller=null; } return ''; } return r.text(); })
      .then(function(text){ if(text){ var t=__applyClear(text); t=__stripAnsi(t); if(cliOutput){ cliOutput.textContent = t; try{ localStorage.setItem('cliOutputHistory', cliOutput.textContent); }catch(_){} } } })
      .catch(function(_){ });
  }, 500);
} catch(e) { try{ console.debug('[CLI] polling init error: ' + e.message); }catch(_){} }
try{ window.addEventListener('beforeunload', function(){ try{ if(window.__cliPoller){ clearInterval(window.__cliPoller); window.__cliPoller=null; } }catch(_){ } }, {capture:true}); }catch(_){ }
if(cliInput){ cliInput.addEventListener('keydown', function(e){
  if (e.key === 'ArrowUp') { e.preventDefault(); if (historyIndex === -1) { currentCommand = cliInput.value; } if (historyIndex < commandHistory.length - 1) { historyIndex++; cliInput.value = commandHistory[commandHistory.length - 1 - historyIndex]; } }
  else if (e.key === 'ArrowDown') { e.preventDefault(); if (historyIndex > 0) { historyIndex--; cliInput.value = commandHistory[commandHistory.length - 1 - historyIndex]; } else if (historyIndex === 0) { historyIndex = -1; cliInput.value = currentCommand; } }
}); }
try{console.log('[CLI] Session/init ready');}catch(_){}
</script>
)JS", HTTPD_RESP_USE_STRLEN);

  // JS: Execute handler
  httpd_resp_send_chunk(req, R"JS(
<script>
try{console.log('[CLI] Execute handler ready');}catch(_){}
function executeCommand(){
  try { console.debug('[CLI] execute start'); } catch(_){}
  var command = (cliInput && cliInput.value ? cliInput.value : '').trim();
  if (!command) return;
  var lower = command.toLowerCase();
  var exitingHelp = false;
  if (!inHelp && (lower === 'help' || lower === 'menu' || lower === 'cli help')) {
    outputBackup = cliOutput ? cliOutput.textContent : '';
    try{ localStorage.setItem('cliOutputHistoryBackup', outputBackup); }catch(_){}
    inHelp = true; try{ localStorage.setItem('cliInHelp', 'true'); }catch(_){}
  } else if (inHelp && (lower === 'exit' || lower === 'back' || lower === 'q' || lower === 'quit')) {
    exitingHelp = true;
  }
  if (commandHistory[commandHistory.length - 1] !== command) {
    commandHistory.push(command); if (commandHistory.length > 50) commandHistory.shift();
    try{ localStorage.setItem('cliHistory', JSON.stringify(commandHistory)); }catch(_){}
  }
  historyIndex = -1; currentCommand = '';
  if (cliOutput) { cliOutput.textContent += ('$ ' + command + '\n'); }
  try { console.debug('[CLI] fetch start: ' + command); } catch(_){}
  fetch('/api/cli', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, credentials: 'same-origin', body: 'cmd=' + encodeURIComponent(command) })
  .then(function(r){ try{ console.debug('[CLI] fetch status: ' + r.status); }catch(_){} return r.text(); })
  .then(function(result){ try { console.debug('[CLI] fetch ok, len=' + (result ? result.length : 0)); } catch(_){} var ESC=String.fromCharCode(27); var clearSeq = ESC+'[2J'+ESC+'[H'; if (result && result.indexOf(clearSeq) !== -1) { var cleanResult = result.split(clearSeq).join(''); if (exitingHelp && inHelp) { if (cliOutput) { cliOutput.textContent = outputBackup || ''; } inHelp = false; try{ localStorage.setItem('cliInHelp','false'); localStorage.removeItem('cliOutputHistoryBackup'); }catch(_){} if (cleanResult && cliOutput) { cliOutput.textContent += cleanResult; } try{ localStorage.setItem('cliOutputHistory', cliOutput ? cliOutput.textContent : ''); }catch(_){} } else { if (cliOutput) { cliOutput.textContent = cleanResult; try{ localStorage.setItem('cliOutputHistory', cliOutput.textContent); }catch(_){} } } } else { if (cliOutput) { cliOutput.textContent += result + '\n'; try{ localStorage.setItem('cliOutputHistory', cliOutput.textContent); }catch(_){} } } if (cliInput) { cliInput.value=''; cliInput.focus(); } })
  .catch(function(e){ try { console.debug('[CLI] fetch error: ' + e.message); } catch(_){} var errorMsg='Error: ' + e.message + '\n'; if (cliOutput) { cliOutput.textContent += errorMsg; try{ localStorage.setItem('cliOutputHistory', cliOutput.textContent); }catch(_){} } if (cliInput) { cliInput.value=''; cliInput.focus(); } });
}
try { console.debug('[CLI] EOF'); } catch(_){}
function clearHistory() {
  localStorage.removeItem('cliHistory');
  localStorage.removeItem('cliOutputHistory');
  commandHistory = [];
  cliOutput.textContent = '';
  historyIndex = -1;
}
</script>
)JS", HTTPD_RESP_USE_STRLEN);
}

#endif
