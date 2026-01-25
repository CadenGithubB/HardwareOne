#ifndef WEBPAGE_AUTOMATIONS_H
#define WEBPAGE_AUTOMATIONS_H

// Stream automations page content using raw string literals
static void streamAutomationsInner(httpd_req_t* req) {
  // Part 1: Header and system status check
  httpd_resp_send_chunk(req, R"AUTOPART1(
<h2>Automations</h2>
<p>Create automations to execute commands depending on certain criteria.</p>

<div id='auto_system_status' class='settings-panel'>
<h3 style='margin-top:0;color:var(--panel-fg)'>System Status</h3>
<div style='display:flex;align-items:center;gap:1rem;flex-wrap:wrap'>
  <button class='btn' id='btn-auto-refresh-status'>Refresh Status</button>
  <button class='btn' id='btn-auto-enable-system' style='display:none'>Enable Automation System</button>
  <button class='btn' id='btn-auto-disable-system' style='display:none'>Disable Automation System</button>
  <span id='auto-status-indicator' style='display:inline-flex;align-items:center;gap:0.5rem'>
    <span class='status-indicator status-disabled' id='auto-status-dot'></span>
    <span id='auto-status-text'>Click 'Refresh Status' to check system status...</span>
  </span>
</div>
<div id='auto-system-warning' class='alert alert-warning' style='display:none;margin-top:10px'>
  <strong>Automation System Disabled</strong><br>
  The automation system is currently disabled to save memory.<br>
  Click "Enable Automation System" above to start it instantly (no reboot required).
</div>
</div>

<div id='auto_form' class='settings-panel' style='display:none;background:var(--panel-bg)'>
<style>
.status-indicator { display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }
.status-enabled { background: #28a745; animation: pulse 2s infinite; }
.status-disabled { background: #dc3545; }
@keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.5; } 100% { opacity: 1; } }
#auto_form .row-inline{display:flex;align-items:center;gap:0.5rem;flex-wrap:wrap;}
#auto_form .row-inline .input-tall{height:32px;line-height:32px;box-sizing:border-box;}
#auto_form .row-inline .btn,#auto_form .row-inline .btn-small{height:32px;line-height:32px;padding:0 10px;display:inline-flex;align-items:center;margin:0;box-sizing:border-box;font-size:14px;}
#auto_form input[type=time].input-tall{height:32px;line-height:32px;}
#auto_form .row-inline input,#auto_form .row-inline select{margin:0;}
</style>
<h3 style='margin-top:0;color:var(--panel-fg)'>Create Automation</h3>
<div style='display:flex;flex-wrap:wrap;gap:0.5rem;align-items:center'>
<input id='a_name' class='input-tall' placeholder='Name' style='flex:1;min-width:160px'>
<select id='a_type' class='input-tall' onchange='autoTypeChanged()'>
  <option value='atTime'>At Time</option>
  <option value='afterDelay'>After Delay</option>
  <option value='interval'>Interval</option>
  <option value='onBoot'>On Boot</option>
</select>
<div id='grp_atTime'>
<div style='display:flex;flex-direction:column;gap:0.5rem'>
  <div class='row-inline'>
    <label style='font-size:0.9em;color:var(--panel-fg)'>Repeat:</label>
    <select id='a_recur' class='input-tall' onchange='recurChanged()'>
      <option value='daily' selected>Daily</option>
      <option value='weekly'>Weekly</option>
      <option value='monthly'>Monthly</option>
      <option value='yearly'>Yearly</option>
    </select>
  </div>
  <div style='margin-top:0.5rem'>
    <label style='font-size:0.9em;color:var(--panel-fg);margin-bottom:0.25rem;display:block'>Times:</label>
    <div class='row-inline'>
      <input type='time' class='time-input input-tall' placeholder='HH:MM' style='width:120px;height:32px;line-height:32px'>
      <button id='btn_add_time' type='button' class='btn btn-small' onclick='addTimeField()' style='height:32px;line-height:32px;padding:0 10px;box-sizing:border-box;font-size:14px;display:inline-flex;align-items:center;margin:0'>+ Add Time</button>
      <button id='btn_remove_main_time' type='button' class='btn btn-small' onclick='removeMainTimeField()' style='height:32px;line-height:32px;padding:0 10px;box-sizing:border-box;font-size:14px;display:inline-flex;align-items:center;margin:0;visibility:hidden'>Remove</button>
    </div>
  </div>
  <div id='time_fields' style='margin-top:0.25rem'></div>
</div>
<div id='dow_wrap' style='display:none;flex-direction:column;gap:0.25rem;margin-top:0.5rem;color:var(--panel-fg);margin-left:0;padding-left:0'>
  <div style='display:flex;align-items:center;flex-wrap:wrap;margin:0'>
    <span style='font-size:0.9em;color:var(--panel-fg);margin:0;margin-right:1rem'>Days of week:</span>
    <label style='display:flex;align-items:center;gap:0;margin-right:2.5rem'><input type='checkbox' id='day_mon' value='mon' style='margin:0;padding:0;vertical-align:middle'><span style='display:inline-block;margin-left:-2px;font-kerning:none'>Mon</span></label>
    <label style='display:flex;align-items:center;gap:0;margin-right:2.5rem'><input type='checkbox' id='day_tue' value='tue' style='margin:0;padding:0;vertical-align:middle'><span style='display:inline-block;margin-left:-2px;font-kerning:none'>Tue</span></label>
    <label style='display:flex;align-items:center;gap:0;margin-right:2.5rem'><input type='checkbox' id='day_wed' value='wed' style='margin:0;padding:0;vertical-align:middle'><span style='display:inline-block;margin-left:-2px;font-kerning:none'>Wed</span></label>
    <label style='display:flex;align-items:center;gap:0;margin-right:2.5rem'><input type='checkbox' id='day_thu' value='thu' style='margin:0;padding:0;vertical-align:middle'><span style='display:inline-block;margin-left:-2px;font-kerning:none'>Thu</span></label>
    <label style='display:flex;align-items:center;gap:0;margin-right:2.5rem'><input type='checkbox' id='day_fri' value='fri' style='margin:0;padding:0;vertical-align:middle'><span style='display:inline-block;margin-left:-2px;font-kerning:none'>Fri</span></label>
    <label style='display:flex;align-items:center;gap:0;margin-right:2.5rem'><input type='checkbox' id='day_sat' value='sat' style='margin:0;padding:0;vertical-align:middle'><span style='display:inline-block;margin-left:-2px;font-kerning:none'>Sat</span></label>
    <label style='display:flex;align-items:center;gap:0;margin-right:0'><input type='checkbox' id='day_sun' value='sun' style='margin:0;padding:0;vertical-align:middle'><span style='display:inline-block;margin-left:-2px;font-kerning:none'>Sun</span></label>
  </div>
  </div>
</div>
</div>
<div id='grp_afterDelay' class='vis-gone'>
<div class='row-inline' style='gap:0.3rem'>
  <input id='a_delay' class='input-tall' placeholder='Delay' style='width:160px'>
  <select id='a_delay_unit' class='input-tall'>
    <option value='ms' selected>ms</option>
    <option value='s'>seconds</option>
    <option value='min'>minutes</option>
    <option value='hr'>hours</option>
    <option value='day'>days</option>
  </select>
</div>
</div>
<div id='grp_interval' class='vis-gone row-inline' style='gap:0.3rem'>
  <input id='a_interval' class='input-tall' placeholder='Interval' style='width:160px'>
  <select id='a_interval_unit' class='input-tall'>
    <option value='ms' selected>ms</option>
    <option value='s'>seconds</option>
    <option value='min'>minutes</option>
    <option value='hr'>hours</option>
    <option value='day'>days</option>
  </select>
</div>
<div style='display:flex;flex-direction:column;gap:0.5rem'>
  <div style='display:flex;flex-direction:column;gap:0.5rem'>
    <div style='margin-top:0.5rem'>
      <label style='font-size:0.9em;color:var(--panel-fg);margin-bottom:0.25rem;display:block'>Commands & Logic:</label>
      <div id='command_fields' style='margin-top:0.25rem'>
        <div id='command_buttons' class='row-inline' style='gap:0.5rem;margin-top:0.5rem'>
        <button id='btn_add_cmd' type='button' class='btn btn-small' onclick='addCommandField()' title='Add another command to execute (e.g., ledcolor red, status, broadcast message)'>+ Add Command</button>
        <button id='btn_add_logic' type='button' class='btn btn-small' onclick='addLogicField()' title='Add conditional logic (IF/THEN statements for sensor-based automation)'>+ Add Logic</button>
        <button id='btn_add_wait' type='button' class='btn btn-small' onclick='addWaitField()' title='Add a wait/pause command with dropdown timing'>+ Add Wait</button>
      </div>
    </div>
    </div>
    <div style='display:flex;align-items:center;gap:0.5rem;flex-wrap:wrap'>
      <label style='display:flex;align-items:center;gap:0;margin:0'><input id='a_enabled' type='checkbox' checked style='margin:0 -8px 0 6px;padding:0;vertical-align:middle;width:16px;height:16px'><span style='display:inline-block;margin-left:0;font-kerning:none;color:var(--panel-fg) !important;position:relative;left:16px'>Enabled</span></label>
      <label style='display:flex;align-items:center;gap:0.25rem;margin-left:1rem'><input id='a_runatboot' type='checkbox' style='margin:0;padding:0;vertical-align:middle;width:16px;height:16px'><span style='font-size:0.9em;color:var(--panel-fg)'>Run at boot</span></label>
    </div>
    <div style='margin-top:0.5rem'>
      <button class='btn' onclick='createAutomation()'>Add</button>
    </div>
  </div>
</div>
<div id='a_error' class='alert alert-danger' style='margin-top:0.5rem'></div>
</div>
  )AUTOPART1", HTTPD_RESP_USE_STRLEN);

  // Part 2: Download/Export section and automations table
  httpd_resp_send_chunk(req, R"AUTOPART2(
<div style='background:var(--panel-bg);border:1px solid var(--border);border-radius:8px 8px 0 0;padding:1rem;border-bottom:1px solid var(--border);margin:1rem 0 0 0'>
<div style='background:var(--panel-bg);border:1px solid var(--border);border-radius:8px 8px 0 0;padding:1rem;border-bottom:1px solid var(--border);margin:1rem 0 0 0'>
<div style='display:flex;gap:2rem;align-items:flex-start;flex-wrap:wrap'>
<div style='flex:1;min-width:300px'>
<h3 style='margin-top:0;color:var(--panel-fg)'>Download from GitHub</h3>
<p style='margin:0.5rem 0;color:var(--muted);font-size:0.9em'>Import automation scripts from GitHub repositories:</p>
<div style='margin-bottom:0.5rem'>
<input type='text' id='github_url' placeholder='https://github.com/user/repo/blob/main/automation.json' style='width:100%;padding:0.5rem;border:1px solid #ccc;border-radius:4px;font-size:0.9em;box-sizing:border-box;background:white;color:#333'>
</div>
<div style='display:flex;gap:0.5rem;align-items:stretch;flex-wrap:wrap;margin-bottom:0.5rem'>
<input type='text' id='github_name' placeholder='Custom name (optional)' style='flex:1;min-width:150px;padding:0.5rem;border:1px solid #ccc;border-radius:4px;font-size:0.9em;height:auto;background:white;color:#333'>
<button onclick='downloadFromGitHub()' class='btn' style='padding:0.5rem 1rem;height:auto'>Download</button>
</div>
<div id='download_status' style='font-size:0.8em'></div>
</div>
<div style='flex:1;min-width:250px'>
<h3 style='margin-top:0;color:var(--panel-fg)'>Export Automations</h3>
<p style='margin:0.5rem 0 1rem 0;color:var(--muted);font-size:0.9em'>Download your automations as JSON backup files:</p>
<div style='margin-bottom:0.75rem;padding:0.75rem;background:var(--panel-bg);border-radius:4px;border:1px solid var(--border)'>
<div style='font-weight:600;font-size:0.85em;color:var(--panel-fg);margin-bottom:0.5rem'>Export Options:</div>
<div style='display:flex;align-items:flex-start;justify-content:space-between;gap:1rem'>
<div style='flex:1'>
<label style='display:inline-flex;align-items:center;cursor:pointer'>
<input type='checkbox' id='export_separate' style='cursor:pointer;margin:0;padding:0;width:16px;height:16px'>
<span style='font-size:0.9em;color:var(--panel-fg);margin-left:0.5rem'>Separate files</span>
</label>
<div style='font-size:0.75em;color:var(--muted);margin-top:0.3rem;margin-left:1.5rem'>Download one automation per file</div>
</div>
<button onclick='exportAllAutomations()' class='btn' style='margin:0;flex-shrink:0'>Export All</button>
</div>
</div>
<div id='export_status' style='font-size:0.8em'></div>
</div>
</div>
</div>
<div id='autos_list' style='background:var(--panel-bg);border:1px solid var(--border);border-radius:0 0 8px 8px;padding:1rem;color:var(--panel-fg);border-top:none;display:none'>
<div id='autos'>Loading automations...</div>
</div>
)AUTOPART2", HTTPD_RESP_USE_STRLEN);

  // Part 2b: System status check JavaScript
  httpd_resp_send_chunk(req, R"AUTOPART2B(<script>
console.log('[AUTOMATIONS] System status check starting...');
window.refreshAutomationSystemStatus = function() {
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent('automation system status')
  })
  .then(response => response.text())
  .then(output => {
    console.log('[AUTOMATIONS] System status response:', output);
    const statusDot = document.getElementById('auto-status-dot');
    const statusText = document.getElementById('auto-status-text');
    const enableBtn = document.getElementById('btn-auto-enable-system');
    const warningDiv = document.getElementById('auto-system-warning');
    const autoForm = document.getElementById('auto_form');
    const autosList = document.getElementById('autos_list');
    
    const isEnabled = output.includes('Automation system: enabled');
    
    const disableBtn = document.getElementById('btn-auto-disable-system');
    
    if (isEnabled) {
      statusDot.className = 'status-indicator status-enabled';
      statusText.textContent = 'Automation system is enabled and running';
      enableBtn.style.display = 'none';
      disableBtn.style.display = 'inline-block';
      warningDiv.style.display = 'none';
      autoForm.style.display = 'block';
      if (autosList) autosList.style.display = 'block';
      // Load automations list
      if (typeof loadAutos === 'function') {
        loadAutos();
      }
    } else {
      statusDot.className = 'status-indicator status-disabled';
      statusText.textContent = 'Automation system is disabled';
      enableBtn.style.display = 'inline-block';
      disableBtn.style.display = 'none';
      warningDiv.style.display = 'block';
      autoForm.style.display = 'none';
      if (autosList) autosList.style.display = 'none';
    }
  })
  .catch(error => {
    console.error('[AUTOMATIONS] Status check error:', error);
    document.getElementById('auto-status-text').textContent = 'Error checking status: ' + error;
  });
};

window.disableAutomationSystem = function() {
  if (!confirm('Disable the automation system? This will:\n\n• Stop the automation scheduler immediately\n• Keep ~16KB memory allocated until next reboot\n• Disable all scheduled automations\n\nContinue?')) {
    return;
  }
  
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent('automation system disable')
  })
  .then(response => response.text())
  .then(output => {
    console.log('[AUTOMATIONS] Disable response:', output);
    // Immediately reflect disabled state in UI
    try {
      const statusDot = document.getElementById('auto-status-dot');
      const statusText = document.getElementById('auto-status-text');
      const enableBtn = document.getElementById('btn-auto-enable-system');
      const disableBtn = document.getElementById('btn-auto-disable-system');
      const warningDiv = document.getElementById('auto-system-warning');
      const autoForm = document.getElementById('auto_form');
      const autosList = document.getElementById('autos_list');
      if (statusDot) statusDot.className = 'status-indicator status-disabled';
      if (statusText) statusText.textContent = 'Automation system is disabled';
      if (enableBtn) enableBtn.style.display = 'inline-block';
      if (disableBtn) disableBtn.style.display = 'none';
      if (warningDiv) warningDiv.style.display = 'block';
      if (autoForm) autoForm.style.display = 'none';
      if (autosList) autosList.style.display = 'none';
    } catch(e) {}
    alert('Automation system disabled successfully. Scheduler suspended.');
    // Re-check status after updating UI
    try { refreshAutomationSystemStatus(); } catch(_) {}
  })
  .catch(error => {
    console.error('[AUTOMATIONS] Disable error:', error);
    alert('Error disabling automation system: ' + error);
  });
};

window.enableAutomationSystem = function() {
  if (!confirm('Enable automation system? This will:\n\n• Start the automation scheduler immediately\n• Enable all scheduled automations\n\nContinue?')) {
    return;
  }
  fetch('/api/cli', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent('automation system enable')
  })
  .then(response => response.text())
  .then(output => {
    console.log('[AUTOMATIONS] Enable response:', output);
    // Immediately reflect enabled state in UI (expand panels like ESP-NOW)
    try {
      const statusDot = document.getElementById('auto-status-dot');
      const statusText = document.getElementById('auto-status-text');
      const enableBtn = document.getElementById('btn-auto-enable-system');
      const disableBtn = document.getElementById('btn-auto-disable-system');
      const warningDiv = document.getElementById('auto-system-warning');
      const autoForm = document.getElementById('auto_form');
      const autosList = document.getElementById('autos_list');
      if (statusDot) statusDot.className = 'status-indicator status-enabled';
      if (statusText) statusText.textContent = 'Automation system is enabled and running';
      if (enableBtn) enableBtn.style.display = 'none';
      if (disableBtn) disableBtn.style.display = 'inline-block';
      if (warningDiv) warningDiv.style.display = 'none';
      if (autoForm) autoForm.style.display = 'block';
      if (autosList) autosList.style.display = 'block';
      if (typeof loadAutos === 'function') loadAutos();
    } catch(e) {}
    alert('Automation system enabled and started successfully!');
    // Re-check status shortly to sync with backend
    try { setTimeout(function(){ refreshAutomationSystemStatus(); }, 300); } catch(_) {}
  })
  .catch(error => {
    console.error('[AUTOMATIONS] Enable error:', error);
    alert('Error enabling system: ' + error);
  });
};

// Set up button handlers
document.addEventListener('DOMContentLoaded', function() {
  const refreshBtn = document.getElementById('btn-auto-refresh-status');
  const enableBtn = document.getElementById('btn-auto-enable-system');
  const disableBtn = document.getElementById('btn-auto-disable-system');
  
  if (refreshBtn) {
    refreshBtn.addEventListener('click', refreshAutomationSystemStatus);
  }
  if (enableBtn) {
    enableBtn.addEventListener('click', enableAutomationSystem);
  }
  if (disableBtn) {
    disableBtn.addEventListener('click', disableAutomationSystem);
  }
  
  // Auto-refresh status on page load
  refreshAutomationSystemStatus();
});

console.log('[AUTOMATIONS] System status check ready');
</script>)AUTOPART2B", HTTPD_RESP_USE_STRLEN);

  // Part 3: JavaScript - Type change and time field functions
  httpd_resp_send_chunk(req, R"AUTOPART3(<script>console.log('[AUTOMATIONS] Section 1: Pre-script sentinel');</script><script>
console.log('[AUTOMATIONS] Part 1: Init starting...');
window.onload = function() { 
  console.log('[AUTOMATIONS] Window onload');
  try{ 
    autoTypeChanged(); 
  }catch(e){ 
    console.error('[AUTOMATIONS] Error in autoTypeChanged on load:', e); 
  } 
  // Don't auto-load automations here - let status check handle it
};
console.log('[AUTOMATIONS] onload registered');
function autoTypeChanged(){ 
  try { 
    var t=document.getElementById('a_type').value; 
    var g1=document.getElementById('grp_atTime'); 
    var g2=document.getElementById('grp_afterDelay'); 
    var g3=document.getElementById('grp_interval'); 
    if(t==='atTime'){ 
      g1.classList.remove('vis-gone'); 
      g2.classList.add('vis-gone'); 
      g3.classList.add('vis-gone'); 
      recurChanged(); 
    } else if(t==='afterDelay'){ 
      g1.classList.add('vis-gone'); 
      g2.classList.remove('vis-gone'); 
      g3.classList.add('vis-gone'); 
    } else if(t==='interval'){ 
      g1.classList.add('vis-gone'); 
      g2.classList.add('vis-gone'); 
      g3.classList.remove('vis-gone'); 
    } else if(t==='onBoot'){ 
      g1.classList.add('vis-gone'); 
      g2.classList.add('vis-gone'); 
      g3.classList.add('vis-gone'); 
      var rb=document.getElementById('a_runatboot'); if(rb){ rb.checked=true; } 
    } 
  }catch(e){ 
    console.error('autoTypeChanged error:', e); 
  } 
}
function recurChanged(){ 
  try { 
    var r=document.getElementById('a_recur').value; 
    var dw=document.getElementById('dow_wrap'); 
    if(!dw) return; 
    if(r==='weekly'){ 
      dw.style.display='flex'; 
    } else { 
      dw.style.display='none'; 
    } 
  }catch(e){ 
    console.error('recurChanged error:', e); 
  } 
}
function addTimeField(){ 
  const container=document.getElementById('time_fields'); 
  const newField=document.createElement('div'); 
  newField.className='time-field row-inline'; 
  newField.style.cssText='gap:0.5rem;margin-bottom:0.3rem'; 
  newField.innerHTML='<input type="time" class="time-input input-tall" placeholder="HH:MM" style="width:120px;height:32px;line-height:32px"><button type="button" class="btn btn-small" onclick="removeTimeField(this)" style="height:32px;line-height:32px;padding:0 10px;box-sizing:border-box;font-size:14px;display:inline-flex;align-items:center;margin:0">Remove</button>'; 
  container.appendChild(newField); 
  updateTimeRemoveButtons(); 
  updateMainTimeRemove(); 
}
function removeTimeField(btn){ 
  btn.parentElement.remove(); 
  updateTimeRemoveButtons(); 
  updateMainTimeRemove(); 
}
function removeMainTimeField(){ 
  const mainInput=document.querySelector('#grp_atTime .time-input'); 
  const additionalFields=document.querySelectorAll('.time-field'); 
  if(additionalFields.length>0){ 
    const firstAdditional=additionalFields[0]; 
    const firstAdditionalInput=firstAdditional.querySelector('.time-input'); 
    if(firstAdditionalInput){ 
      mainInput.value=firstAdditionalInput.value; 
      firstAdditional.remove(); 
    } 
  } else { 
    mainInput.value=''; 
  } 
  updateTimeRemoveButtons(); 
  updateMainTimeRemove(); 
}
function updateTimeRemoveButtons(){ 
  const fields=document.querySelectorAll('.time-field'); 
  const allTimeInputs=document.querySelectorAll('.time-input'); 
  const totalTimeFields=allTimeInputs.length; 
  fields.forEach((field,idx)=>{ 
    const btn=field.querySelector('button'); 
    if(totalTimeFields<=1){ 
      btn.style.visibility='hidden'; 
    } else { 
      btn.style.visibility='visible'; 
    } 
  }); 
}
function updateMainTimeRemove(){ 
  const allTimeInputs=document.querySelectorAll('.time-input'); 
  const mainRemoveBtn=document.querySelector('#btn_remove_main_time'); 
  if(mainRemoveBtn){ 
    if(allTimeInputs.length<=1){ 
      mainRemoveBtn.style.visibility='hidden'; 
    } else { 
      mainRemoveBtn.style.visibility='visible'; 
    } 
  } 
}
</script>)AUTOPART3", HTTPD_RESP_USE_STRLEN);

  // Part 4: Command and wait field functions
  httpd_resp_send_chunk(req, R"AUTOPART4(<script>
function addWaitField(){ 
  const container=document.getElementById('command_fields'); 
  const buttonsDiv=document.getElementById('command_buttons'); 
  const div=document.createElement('div'); 
  div.className='wait-field row-inline'; 
  div.style.cssText='gap:0.5rem;margin-bottom:0.3rem;align-items:center'; 
  const waitSpan=document.createElement('span'); 
  waitSpan.style.cssText='font-size:0.9em;color:var(--panel-fg);margin-right:0.3rem;font-weight:500'; 
  waitSpan.textContent='wait'; 
  const msSelect=document.createElement('select'); 
  msSelect.className='wait-ms-select input-tall'; 
  msSelect.style.cssText='height:32px;width:120px'; 
  msSelect.innerHTML='<option value="100">100 ms</option><option value="200" selected>200 ms</option><option value="300">300 ms</option><option value="400">400 ms</option><option value="500">500 ms</option><option value="600">600 ms</option><option value="700">700 ms</option><option value="800">800 ms</option><option value="900">900 ms</option><option value="1000">1000 ms</option><option value="1500">1500 ms</option><option value="2000">2000 ms</option><option value="3000">3000 ms</option><option value="5000">5000 ms</option>'; 
  const removeBtn=document.createElement('button'); 
  removeBtn.type='button'; 
  removeBtn.className='btn btn-small'; 
  removeBtn.textContent='Remove'; 
  removeBtn.style.cssText='height:32px;padding:0 10px;margin-left:0.3rem'; 
  removeBtn.onclick=function(){ removeWaitField(this); }; 
  div.appendChild(waitSpan); 
  div.appendChild(msSelect); 
  div.appendChild(removeBtn); 
  container.insertBefore(div, buttonsDiv); 
}
function removeWaitField(btn){ btn.parentElement.remove(); }
function addCommandField(){ 
  const container=document.getElementById('command_fields'); 
  const buttonsDiv=document.getElementById('command_buttons'); 
  const div=document.createElement('div'); 
  div.className='cmd-field row-inline'; 
  div.style.cssText='gap:0.5rem;margin-bottom:0.3rem'; 
  div.innerHTML='<input type="text" class="cmd-input input-tall" placeholder="Command to run" style="flex:1;min-width:260px;height:32px;line-height:32px;padding:0 0.5rem;box-sizing:border-box"><button type="button" class="btn btn-small" onclick="removeCommandField(this)" style="height:32px;line-height:32px;padding:0 10px;box-sizing:border-box;font-size:14px;display:inline-flex;align-items:center;margin:0">Remove</button>'; 
  container.insertBefore(div, buttonsDiv); 
}
function removeCommandField(btn){ btn.parentElement.remove(); }
</script>)AUTOPART4", HTTPD_RESP_USE_STRLEN);

  // Part 5: Logic field functions
  httpd_resp_send_chunk(req, R"AUTOPART5(<script>
function addLogicField(){ 
  const container=document.getElementById('command_fields'); 
  const buttonsDiv=document.getElementById('command_buttons'); 
  const newField=document.createElement('div'); 
  newField.className='logic-field row-inline'; 
  newField.style.cssText='gap:0.5rem;margin-bottom:0.3rem;align-items:center;flex-wrap:wrap'; 
  const typeSelect = document.createElement('select'); 
  typeSelect.className = 'logic-type input-tall'; 
  typeSelect.style.cssText = 'height:32px;margin-right:0.3rem'; 
  typeSelect.onchange = function() { updateLogicField(this); }; 
  typeSelect.innerHTML = '<option value="IF">IF</option><option value="ELSE IF">ELSE IF</option><option value="ELSE">ELSE</option>'; 
  const varSelect = document.createElement('select'); 
  varSelect.className = 'logic-var input-tall'; 
  varSelect.style.cssText = 'height:32px'; 
  varSelect.innerHTML = '<option value="temp">Temperature</option><option value="distance">Distance</option><option value="light">Light</option><option value="motion">Motion</option><option value="time">Time</option>'; 
  const opSelect = document.createElement('select'); 
  opSelect.className = 'logic-operator input-tall'; 
  opSelect.style.cssText = 'height:32px;width:60px'; 
  opSelect.innerHTML = '<option value=">">></option><option value="<"><</option><option value="=">=</option><option value=">=">=</option><option value="<="<=</option><option value="!=">!=</option>'; 
  const valueInput = document.createElement('input'); 
  valueInput.type = 'text'; 
  valueInput.className = 'logic-value input-tall'; 
  valueInput.placeholder = '75'; 
  valueInput.style.cssText = 'width:80px;height:32px'; 
  const thenSpan = document.createElement('span'); 
  thenSpan.className = 'then-text'; 
  thenSpan.style.cssText = 'font-size:0.9em;color:var(--panel-fg);margin:0 0.3rem'; 
  thenSpan.textContent = 'THEN'; 
  const actionInput = document.createElement('input'); 
  actionInput.type = 'text'; 
  actionInput.className = 'logic-action input-tall'; 
  actionInput.placeholder = 'ledcolor red'; 
  actionInput.style.cssText = 'flex:1;min-width:120px;height:32px'; 
  const removeBtn = document.createElement('button'); 
  removeBtn.type = 'button'; 
  removeBtn.className = 'btn btn-small'; 
  removeBtn.textContent = 'Remove'; 
  removeBtn.style.cssText = 'height:32px;padding:0 10px;margin-left:0.3rem'; 
  removeBtn.onclick = function() { removeLogicField(this); }; 
  newField.appendChild(typeSelect); 
  newField.appendChild(varSelect); 
  newField.appendChild(opSelect); 
  newField.appendChild(valueInput); 
  newField.appendChild(thenSpan); 
  newField.appendChild(actionInput); 
  newField.appendChild(removeBtn); 
  container.insertBefore(newField, buttonsDiv); 
}
function removeLogicField(btn){ btn.parentElement.remove(); }
function updateLogicField(selectElement){ 
  try { 
    const field=selectElement.parentElement; 
    const logicType=selectElement.value; 
    const varSelect=field.querySelector('.logic-var'); 
    const operatorSelect=field.querySelector('.logic-operator'); 
    const valueInput=field.querySelector('.logic-value'); 
    const thenText=field.querySelector('.then-text'); 
    if(logicType==='ELSE'){ 
      varSelect.style.display='none'; 
      operatorSelect.style.display='none'; 
      valueInput.style.display='none'; 
      thenText.style.display='none'; 
    } else { 
      varSelect.style.display='inline-block'; 
      operatorSelect.style.display='inline-block'; 
      valueInput.style.display='inline-block'; 
      thenText.style.display='inline-block'; 
    } 
  } catch(e) { 
    console.error('updateLogicField error:', e); 
  } 
}
</script>)AUTOPART5", HTTPD_RESP_USE_STRLEN);

  // Part 6: Utility and render functions
  httpd_resp_send_chunk(req, R"AUTOPART6(<script>
function human(v){ 
  if(v===null||v===undefined) return '\u2014'; 
  if(typeof v==='boolean') return v?'Yes':'No'; 
  return ''+v; 
}
function formatNextRun(nextAt){ 
  if(!nextAt || nextAt === null) return '\u2014'; 
  try { 
    const now = Math.floor(Date.now()/1000); 
    const next = parseInt(nextAt); 
    if(isNaN(next) || next <= 0) return '\u2014'; 
    const date = new Date(next * 1000); 
    const timeStr = date.toLocaleString(); 
    const diffSec = next - now; 
    let relativeStr = ''; 
    if(diffSec <= 0){ 
      relativeStr = 'overdue'; 
    } else if(diffSec < 60){ 
      relativeStr = 'in ' + diffSec + 's'; 
    } else if(diffSec < 3600){ 
      relativeStr = 'in ' + Math.floor(diffSec/60) + 'm'; 
    } else if(diffSec < 86400){ 
      relativeStr = 'in ' + Math.floor(diffSec/3600) + 'h'; 
    } else { 
      relativeStr = 'in ' + Math.floor(diffSec/86400) + 'd'; 
    } 
    return timeStr + '<br><small style="color:var(--muted)">' + relativeStr + '</small>'; 
  } catch(e){ 
    return '\u2014'; 
  } 
}
function renderAutos(json) {
  try {
    let data = (typeof json === 'string') ? JSON.parse(json) : json;
    let autos = [];
    if (data && data.automations && Array.isArray(data.automations)) autos = data.automations;
    let html = '<table style="width:100%;border-collapse:collapse">';
    html += '<tr style="background:#e9ecef"><th style="padding:0.5rem;text-align:left">ID</th><th style="padding:0.5rem;text-align:left">Name</th><th style="padding:0.5rem;text-align:left">Enabled</th><th style="padding:0.5rem;text-align:left">Type</th><th style="padding:0.5rem;text-align:left">Summary</th><th style="padding:0.5rem;text-align:left">Next Run</th><th style="padding:0.5rem">Actions</th></tr>';
    if (autos.length === 0) {
      html += '<tr><td colspan="7" style="padding:2rem;text-align:center;color:var(--muted);font-style:italic">No automations yet. Create your first automation above!</td></tr>';
    } else {
      autos.forEach(a => {
        let name = a.name || '(unnamed)';
        let enabled = (a.enabled === true ? 'Yes' : 'No');
        let t = (a.type || '').toLowerCase();
        let type = (a.runAtBoot === true) ? 'On Boot' : (a.type || human(a.type));
        let summary = '';
        if (t === 'attime') {
          summary = 'At ' + (a.time || '?') + (a.days ? ' on ' + a.days : '');
        } else if (t === 'afterdelay') {
          summary = 'After ' + (a.delayMs || '?') + ' ms';
        } else if (t === 'interval') {
          summary = 'Every ' + (a.intervalMs || '?') + ' ms';
        } else {
          summary = '\u2014';
        }
        if (Array.isArray(a.commands) && a.commands.length) {
          summary += ' | cmds: ' + a.commands.join('; ');
        } else if (a.command) {
          summary += ' | cmd: ' + a.command;
        }
        if (a.conditions && a.conditions.trim()) {
          summary += ' | conditions: ' + a.conditions;
        }
        if (a.runAtBoot === true) {
          summary += ' | Boot';
          if (typeof a.bootDelayMs !== 'undefined' && a.bootDelayMs !== null) {
            summary += ' (' + a.bootDelayMs + ' ms)';
          }
        }
        let nextRun = formatNextRun(a.nextAt);
        let id = (typeof a.id !== 'undefined') ? a.id : '';
        let btns = '';
        if (id !== '') {
          if (a.enabled === true) {
            btns += '<button class="btn" onclick="autoToggle(' + id + ',0)" style="margin-right:0.3rem">Disable</button>';
          } else {
            btns += '<button class="btn" onclick="autoToggle(' + id + ',1)" style="margin-right:0.3rem">Enable</button>';
          }
          btns += '<button class="btn" onclick="autoRun(' + id + ')" style="margin-right:0.3rem">Run</button>';
          btns += '<button class="btn" onclick="autoDelete(' + id + ')" style="margin-right:0.3rem;color:#b00">Delete</button>';
          btns += '<button class="btn" onclick="exportSingleAutomation(' + id + ')">Export</button>';
        }
        html += '<tr style="border-bottom:1px solid #ddd">';
        html += '<td style="padding:0.5rem">' + id + '</td>';
        html += '<td style="padding:0.5rem">' + name + '</td>';
        html += '<td style="padding:0.5rem">' + enabled + '</td>';
        html += '<td style="padding:0.5rem">' + type + '</td>';
        html += '<td style="padding:0.5rem">' + summary + '</td>';
        html += '<td style="padding:0.5rem">' + nextRun + '</td>';
        html += '<td style="padding:0.5rem">' + btns + '</td>';
        html += '</tr>';
      });
    }
    html += '</table>';
    document.getElementById('autos').innerHTML = html;
  } catch (e) {
    document.getElementById('autos').innerHTML = 'Error parsing automations: ' + e.message;
  }
}
function loadAutos(){ 
  console.log('[AUTOMATIONS] loadAutos called');
  fetch('/api/automations').then(r => { 
    console.log('[AUTOMATIONS] Automations fetch response:',r.status);
    if(r.ok) return r.text(); 
    else throw new Error('HTTP '+r.status); 
  }).then(txt => { 
    console.log('[AUTOMATIONS] Automations data length:',txt.length);
    renderAutos(txt); 
  }).catch(e => { 
    console.error('[AUTOMATIONS] Load error:',e);
    document.getElementById('autos').innerHTML = 'Error loading automations: ' + e.message; 
  }); 
}
console.log('[AUTOMATIONS] Part 1: Complete');
</script>)AUTOPART6", HTTPD_RESP_USE_STRLEN);

  // Part 7: CLI helper and createAutomation function (large, split into sub-parts)
  httpd_resp_send_chunk(req, R"AUTOPART7(<script>
function postCLI(cmd){ 
  return fetch('/api/cli',{
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'cmd='+encodeURIComponent(cmd)
  }).then(r=>r.text()); 
}
function postCLIValidate(cmd){ 
  return fetch('/api/cli',{
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'cmd='+encodeURIComponent(cmd)+'&validate=1'
  }).then(r=>r.text()); 
}
async function createAutomation(){ 
  const name=document.getElementById('a_name').value.trim(); 
  const type=document.getElementById('a_type').value; 
  const delayRaw=document.getElementById('a_delay').value.trim(); 
  const delayUnit=(document.getElementById('a_delay_unit')?document.getElementById('a_delay_unit').value:'ms'); 
  const intervalRaw=document.getElementById('a_interval').value.trim(); 
  const intervalUnit=(document.getElementById('a_interval_unit')?document.getElementById('a_interval_unit').value:'ms'); 
  const en=document.getElementById('a_enabled').checked; 
  const runAtBoot=document.getElementById('a_runatboot').checked; 
  document.getElementById('a_error').textContent=''; 
  const recur=(document.getElementById('a_recur')?document.getElementById('a_recur').value:'daily'); 
  if(type==='atTime'&&(recur==='monthly'||recur==='yearly')){ 
    document.getElementById('a_error').textContent='Monthly/Yearly repeats are not supported yet.'; 
    return; 
  } 
  const selectedDays=[]; 
  if(type==='atTime'&&recur==='weekly'){ 
    ['mon','tue','wed','thu','fri','sat','sun'].forEach(day=>{ 
      if(document.getElementById('day_'+day).checked) selectedDays.push(day); 
    }); 
    if(selectedDays.length===0){ 
      document.getElementById('a_error').textContent='Please select at least one day for a weekly schedule.'; 
      return; 
    } 
  } 
  const days=selectedDays.join(','); 
  const timeInputs=document.querySelectorAll('.time-input'); 
  const times=[]; 
  timeInputs.forEach(input=>{ 
    const val=input.value.trim(); 
    if(val) times.push(val); 
  }); 
  const cmdInputs=document.querySelectorAll('.cmd-input'); 
  const cmds=[]; 
  cmdInputs.forEach(inp=>{ 
    const v=inp.value.trim(); 
    if(v) cmds.push(v); 
  }); 
  const waitFields=document.querySelectorAll('.wait-field'); 
  waitFields.forEach(field=>{ 
    const select=field.querySelector('.wait-ms-select'); 
    if(select){ 
      const ms=select.value; 
      if(ms) cmds.push('wait '+ms); 
    } 
  }); 
  const logicFields=document.querySelectorAll('.logic-field'); 
  const conditionalChain=[]; 
  logicFields.forEach(field=>{ 
    const typeSelect=field.querySelector('.logic-type'); 
    const varSelect=field.querySelector('.logic-var'); 
    const operatorSelect=field.querySelector('.logic-operator'); 
    const value=field.querySelector('.logic-value'); 
    const action=field.querySelector('.logic-action'); 
    if(typeSelect && action){ 
      const typeVal=typeSelect.value; 
      const actVal=action.value.trim(); 
      if(typeVal && actVal){ 
        if(typeVal==='ELSE'){ 
          conditionalChain.push(typeVal+' '+actVal); 
        } else if(varSelect && operatorSelect && value){ 
          const varVal=varSelect.value; 
          const opVal=operatorSelect.value; 
          const valVal=value.value.trim(); 
          if(varVal && opVal && valVal){ 
            conditionalChain.push(typeVal+' '+varVal+opVal+valVal+' THEN '+actVal); 
          } 
        } 
      } 
    } 
  }); 
  if(conditionalChain.length>0){ 
    cmds.push(conditionalChain.join(' ')); 
  } 
  const cmdsParam=cmds.join(';'); 
  const buildParts=(time,idx)=>{ 
    let parts=['automation add']; 
    parts.push('name='+name+(time!==null && times.length>1?' #'+(idx+1):'')); 
    if(type==='onBoot'){ 
      parts.push('type=afterDelay'); 
      parts.push('delayms=0'); 
    } else { 
      parts.push('type='+type); 
    } 
    if(time) parts.push('time='+time); 
    if(type==='atTime'){ 
      parts.push('recurrence='+recur); 
      if(days) parts.push('days='+days); 
    } 
    if(delayRaw){ 
      let n=parseFloat(delayRaw); 
      if(!isNaN(n)&&n>=0){ 
        let mult=1; 
        if(delayUnit==='s') mult=1000; 
        else if(delayUnit==='min') mult=60000; 
        else if(delayUnit==='hr') mult=3600000; 
        else if(delayUnit==='day') mult=86400000; 
        const delayMs=Math.floor(n*mult); 
        parts.push('delayms='+delayMs); 
      } 
    } 
    if(intervalRaw){ 
      let n=parseFloat(intervalRaw); 
      if(!isNaN(n)&&n>=0){ 
        let mult=1; 
        if(intervalUnit==='s') mult=1000; 
        else if(intervalUnit==='min') mult=60000; 
        else if(intervalUnit==='hr') mult=3600000; 
        else if(intervalUnit==='day') mult=86400000; 
        const intervalMs=Math.floor(n*mult); 
        parts.push('intervalms='+intervalMs); 
      } 
    } 
    parts.push('commands='+cmdsParam); 
    parts.push('enabled='+(en?1:0)); 
    if(runAtBoot) parts.push('runatboot=1'); 
    return parts.join(' '); 
  }; 
  const fullCmds=(times.length?times:[null]).map((t,idx)=>buildParts(t,idx)); 
  if(conditionalChain.length>0){ 
    const chainStr=conditionalChain.join(' '); 
    const validationResult=await postCLIValidate('validate-conditions '+chainStr); 
    if(validationResult!=='VALID'){ 
      document.getElementById('a_error').textContent=validationResult; 
      return; 
    } 
  } 
  Promise.all(fullCmds.map(c=>postCLIValidate(c))).then(vals=>{ 
    for(let i=0;i<vals.length;i++){ 
      const v=(vals[i]||'').trim(); 
      if(v!=='VALID'){ 
        document.getElementById('a_error').textContent=v; 
        throw new Error('Invalid'); 
      } 
    } 
    return Promise.all(fullCmds.map(c=>postCLI(c))); 
  }).then(results=>{ 
    const err=results.find(t=>t.toLowerCase().indexOf('error:')>=0); 
    if(err){ 
      document.getElementById('a_error').textContent=err; 
      return; 
    } 
    document.getElementById('a_name').value=''; 
    document.querySelectorAll('.time-input').forEach(input=>input.value=''); 
    ['mon','tue','wed','thu','fri','sat','sun'].forEach(day=>{ 
      let el=document.getElementById('day_'+day); 
      if(el) el.checked=false; 
    }); 
    document.getElementById('a_delay').value=''; 
    document.getElementById('a_interval').value=''; 
    var elRunBoot=document.getElementById('a_runatboot'); if(elRunBoot) elRunBoot.checked=false; 
    const cwrap=document.getElementById('command_fields'); 
    if(cwrap){ 
      cwrap.innerHTML='<div id="command_buttons" class="row-inline" style="gap:0.5rem;margin-top:0.5rem"><button id="btn_add_cmd" type="button" class="btn btn-small" onclick="addCommandField()" title="Add another command to execute (e.g., ledcolor red, status, broadcast message)">+ Add Command</button><button id="btn_add_logic" type="button" class="btn btn-small" onclick="addLogicField()" title="Add conditional logic (IF/THEN statements for sensor-based automation)">+ Add Logic</button><button id="btn_add_wait" type="button" class="btn btn-small" onclick="addWaitField()" title="Add a wait/pause command with dropdown timing">+ Add Wait</button></div>'; 
    } 
    loadAutos(); 
  }).catch(e=>{ 
    if(!document.getElementById('a_error').textContent){ 
      document.getElementById('a_error').textContent='Validation error: '+e.message; 
    } 
  }); 
}
function autoToggle(id,en){ 
  const cmd='automation ' + (en? 'enable':'disable') + ' id='+id; 
  postCLI(cmd).then(()=>loadAutos()); 
}
function autoDelete(id){ 
  if(!confirm('Delete automation '+id+'?')) return; 
  postCLI('automation delete id='+id).then(()=>loadAutos()); 
}
function autoRun(id){ 
  postCLI('automation run id='+id).then(r=>{ 
    if(r.toLowerCase().indexOf('error:')>=0){ 
      alert(r); 
    } else { 
      alert('Automation executed: '+r); 
      loadAutos(); 
    } 
  }); 
}
</script>)AUTOPART7", HTTPD_RESP_USE_STRLEN);

  // Part 8: GitHub download and export functions
  httpd_resp_send_chunk(req, R"AUTOPART8(<script>
function downloadFromGitHub(){ 
  const url=document.getElementById('github_url').value.trim(); 
  const name=document.getElementById('github_name').value.trim(); 
  const status=document.getElementById('download_status'); 
  if(!url){ 
    status.innerHTML='<span style="color:#dc3545">Please enter a GitHub URL</span>'; 
    return; 
  } 
  status.innerHTML='<span style="color:#007bff">Downloading...</span>'; 
  let cmd='downloadautomation url='+encodeURIComponent(url); 
  if(name) cmd+=' name='+encodeURIComponent(name); 
  postCLI(cmd).then(r=>{ 
    if(r.toLowerCase().indexOf('error:')>=0){ 
      status.innerHTML='<span style="color:#dc3545">'+r+'</span>'; 
    } else { 
      status.innerHTML='<span style="color:#28a745">'+r+'</span>'; 
      document.getElementById('github_url').value=''; 
      document.getElementById('github_name').value=''; 
      loadAutos(); 
    } 
  }).catch(e=>{ 
    status.innerHTML='<span style="color:#dc3545">Network error: '+e.message+'</span>'; 
  }); 
}
function exportAllAutomations(){ 
  const status=document.getElementById('export_status'); 
  const separateFiles=document.getElementById('export_separate').checked; 
  status.innerHTML='<span style="color:#007bff">Preparing export...</span>'; 
  if(separateFiles){ 
    fetch('/api/automations').then(r=>r.json()).then(data=>{ 
      if(data && data.automations && data.automations.length>0){ 
        let downloadCount = 0; 
        const downloadNext = (index) => { 
          if(index >= data.automations.length) { 
            status.innerHTML='<span style="color:#28a745">' + downloadCount + ' files downloaded separately (import-ready)</span>'; 
            return; 
          } 
          const auto = data.automations[index]; 
          const exportAuto={}; 
          exportAuto.name=auto.name; 
          if(auto.type==='attime') exportAuto.type='atTime'; 
          else if(auto.type==='afterdelay') exportAuto.type='afterDelay'; 
          else if(auto.type==='interval') exportAuto.type='interval'; 
          else exportAuto.type=auto.type; 
          if(auto.time) exportAuto.time=auto.time; 
          if(auto.days) exportAuto.days=auto.days; 
          if(auto.delayMs) exportAuto.delay=auto.delayMs.toString(); 
          if(auto.intervalMs) exportAuto.interval=auto.intervalMs.toString(); 
          if(auto.commands) exportAuto.commands=auto.commands; 
          else if(auto.command) exportAuto.commands=[auto.command]; 
          if(auto.conditions) exportAuto.conditions=auto.conditions; 
          exportAuto.enabled=auto.enabled===true; 
          if(auto.runAtBoot===true) exportAuto.runAtBoot=true; 
          if(typeof auto.bootDelayMs!== 'undefined' && auto.bootDelayMs!==null) exportAuto.bootDelayMs=auto.bootDelayMs; 
          const blob=new Blob([JSON.stringify(exportAuto,null,2)],{type:'application/json'}); 
          const url=URL.createObjectURL(blob); 
          const link=document.createElement('a'); 
          link.href=url; 
          link.download=(auto.name || 'automation_'+auto.id)+'.json'; 
          link.style.display='none'; 
          document.body.appendChild(link); 
          link.click(); 
          document.body.removeChild(link); 
          URL.revokeObjectURL(url); 
          downloadCount++; 
          status.innerHTML='<span style="color:#007bff">Downloading ' + (index + 1) + ' of ' + data.automations.length + '...</span>'; 
          setTimeout(() => downloadNext(index + 1), 500); 
        }; 
        downloadNext(0); 
      } else { 
        status.innerHTML='<span style="color:#dc3545">No automations to export</span>'; 
      } 
    }).catch(e=>{ 
      status.innerHTML='<span style="color:#dc3545">Export failed: '+e.message+'</span>'; 
    }); 
  } else { 
    const link=document.createElement('a'); 
    link.href='/api/automations/export'; 
    link.download=''; 
    link.style.display='none'; 
    document.body.appendChild(link); 
    link.click(); 
    document.body.removeChild(link); 
    status.innerHTML='<span style="color:#28a745">Export started - check your downloads folder</span>'; 
  } 
  setTimeout(()=>{ 
    status.innerHTML=''; 
  }, 3000); 
}
function exportSingleAutomation(id){ 
  const link=document.createElement('a'); 
  link.href='/api/automations/export?id='+id; 
  link.download=''; 
  link.style.display='none'; 
  document.body.appendChild(link); 
  link.click(); 
  document.body.removeChild(link); 
}
</script>)AUTOPART8", HTTPD_RESP_USE_STRLEN);
}

// Legacy function removed - now using streamAutomationsInner() for efficient streaming
#endif
