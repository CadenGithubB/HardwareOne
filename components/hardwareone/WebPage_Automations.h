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
#auto_form .row-inline{display:flex;align-items:center;gap:0.5rem;flex-wrap:wrap;}
#auto_form .row-inline .input-tall{height:32px;line-height:32px;box-sizing:border-box;}
#auto_form .row-inline .btn,#auto_form .row-inline .btn-small{height:32px;line-height:32px;padding:0 10px;display:inline-flex;align-items:center;margin:0;box-sizing:border-box;font-size:14px;}
#auto_form input[type=time].input-tall{height:32px;line-height:32px;}
#auto_form .row-inline input,#auto_form .row-inline select{margin:0;}
</style>
<h3 style='margin-top:0;color:var(--panel-fg)'>Create Automation</h3>
<input id='a_name' class='input-tall' placeholder='Name' style='width:100%;box-sizing:border-box;margin-bottom:0.6rem'>
<div style='font-weight:600;color:var(--panel-fg);margin:0.35rem 0 0.45rem'>Triggers <span style='font-size:0.8em;font-weight:400;color:var(--muted)'>when this automation fires (up to 4)</span></div>
<div style='display:flex;flex-wrap:wrap;gap:0.5rem;align-items:center'>
<span style='font-size:0.82em;color:var(--muted)'>Trigger 1</span>
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
      <option value='daily' selected>Every day</option>
      <option value='weekly'>Certain days of the week</option>
      <option value='monthly'>Once a month</option>
      <option value='yearly'>Once a year</option>
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
  <div style='display:flex;align-items:center;flex-wrap:wrap;margin:0 0 0.25rem 0'>
    <span style='font-size:0.9em;color:var(--panel-fg);margin:0;margin-right:1rem'>Every</span>
    <input type='number' id='a_week_interval' min='1' max='12' value='1' class='input-tall' style='width:4em;margin-right:0.5rem'>
    <span style='font-size:0.9em;color:var(--panel-fg);margin:0'>week(s)</span>
  </div>
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
<div id='monthly_wrap' style='display:none;margin-top:0.5rem'>
  <label style='font-size:0.9em;color:var(--panel-fg);margin-right:0.5rem'>Day of month:</label>
  <input type='number' id='a_day_of_month' min='1' max='31' value='1' class='input-tall' style='width:5em'>
  <span style='font-size:0.85em;color:var(--muted);margin-left:0.5rem'>(Feb 30 etc. clamped to last day)</span>
</div>
<div id='yearly_wrap' style='display:none;flex-direction:column;gap:0.25rem;margin-top:0.5rem'>
  <div class='row-inline'>
    <label style='font-size:0.9em;color:var(--panel-fg);margin-right:0.5rem'>Month:</label>
    <select id='a_month_of_year' class='input-tall'>
      <option value='1'>January</option><option value='2'>February</option><option value='3'>March</option>
      <option value='4'>April</option><option value='5'>May</option><option value='6'>June</option>
      <option value='7'>July</option><option value='8'>August</option><option value='9'>September</option>
      <option value='10'>October</option><option value='11'>November</option><option value='12'>December</option>
    </select>
  </div>
  <div class='row-inline'>
    <label style='font-size:0.9em;color:var(--panel-fg);margin-right:0.5rem'>Day:</label>
    <input type='number' id='a_day_of_month_yearly' min='1' max='31' value='1' class='input-tall' style='width:5em'>
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
<div id='secondary_triggers_section' style='margin-top:0.5rem'>
  <div id='secondary_triggers_container'></div>
  <button type='button' class='btn btn-small' onclick='addSecondaryTrigger()' title='Fire this automation from another source (up to 4 total)'>+ Add trigger</button>
</div>
<template id='secondary_trigger_template'>
  <div class='secondary-trigger' style='display:flex;align-items:center;gap:0.4rem;padding:0.5rem;border:1px solid var(--border);border-radius:4px;margin-bottom:0.3rem;flex-wrap:wrap'>
    <span class='st-num' style='font-size:0.82em;color:var(--muted)'>Trigger</span>
    <select class='st-type input-tall' onchange='stTypeChanged(this)' style='min-width:120px'>
      <option value='time'>At Time</option>
      <option value='interval'>Interval</option>
      <option value='manual'>Manual (After Delay)</option>
      <option value='boot'>On Boot</option>
    </select>
    <span class='st-fields st-fields-time' style='display:inline-flex;gap:0.3rem;align-items:center;flex-wrap:wrap'>
      <input type='time' class='st-time input-tall' style='width:120px'>
      <select class='st-recur input-tall' onchange='stRecurChanged(this)'>
        <option value='daily' selected>Every day</option>
        <option value='weekly'>Certain days</option>
      </select>
      <span class='st-days-wrap' style='display:none;gap:0.25rem;align-items:center'>
        <label style='display:flex;align-items:center;gap:2px'><input type='checkbox' class='st-day' value='mon'>Mon</label>
        <label style='display:flex;align-items:center;gap:2px'><input type='checkbox' class='st-day' value='tue'>Tue</label>
        <label style='display:flex;align-items:center;gap:2px'><input type='checkbox' class='st-day' value='wed'>Wed</label>
        <label style='display:flex;align-items:center;gap:2px'><input type='checkbox' class='st-day' value='thu'>Thu</label>
        <label style='display:flex;align-items:center;gap:2px'><input type='checkbox' class='st-day' value='fri'>Fri</label>
        <label style='display:flex;align-items:center;gap:2px'><input type='checkbox' class='st-day' value='sat'>Sat</label>
        <label style='display:flex;align-items:center;gap:2px'><input type='checkbox' class='st-day' value='sun'>Sun</label>
      </span>
    </span>
    <span class='st-fields st-fields-interval' style='display:none;gap:0.3rem;align-items:center'>
      <label style='font-size:0.85em'>Every</label>
      <input type='number' class='st-interval-value input-tall' placeholder='Value' min='1' style='width:80px'>
      <select class='st-interval-unit input-tall'>
        <option value='s' selected>seconds</option><option value='min'>minutes</option><option value='hr'>hours</option><option value='ms'>ms</option>
      </select>
    </span>
    <span class='st-fields st-fields-manual' style='display:none;gap:0.3rem;align-items:center'>
      <label style='font-size:0.85em'>Delay:</label>
      <input type='number' class='st-delay-value input-tall' placeholder='Delay' min='0' style='width:80px'>
      <select class='st-delay-unit input-tall'>
        <option value='s' selected>seconds</option><option value='min'>minutes</option><option value='ms'>ms</option>
      </select>
    </span>
    <span class='st-fields st-fields-boot' style='display:none;gap:0.3rem;align-items:center'>
      <label style='font-size:0.85em'>Delay:</label>
      <input type='number' class='st-boot-delay input-tall' value='0' min='0' style='width:80px'>
      <span style='font-size:0.85em'>ms</span>
    </span>
    <button type='button' class='btn btn-small' onclick='removeSecondaryTrigger(this)' style='color:var(--danger);margin-left:auto'>Remove</button>
  </div>
</template>
<div style='display:flex;flex-direction:column;gap:0.5rem'>
  <div style='display:flex;flex-direction:column;gap:0.5rem'>
    <div style='margin-top:0.5rem'>
      <label style='font-size:0.9em;color:var(--panel-fg);margin-bottom:0.25rem;display:block'>Fire when (optional sensor condition):</label>
      <div class='row-inline' style='gap:0.3rem;align-items:center;flex-wrap:wrap'>
        <select id='a_cond_var' class='input-tall'><option value=''>— none —</option><optgroup label="Sensors"><option value="temp">Temperature</option><option value="distance">Distance</option><option value="light">Light</option><option value="motion">Motion</option></optgroup><optgroup label="Time"><option value="time">Time of day</option><option value="hour">Hour (0-23)</option><option value="day">Day of week</option><option value="ntp">Clock synced</option></optgroup><optgroup label="System"><option value="battery">Battery %</option><option value="heap">Free heap KB</option><option value="psram">Free PSRAM KB</option><option value="fsfree">Free storage KB</option><option value="uptime">Uptime min</option><option value="chiptemp">Chip temp C</option></optgroup><optgroup label="Network"><option value="wifi">WiFi state</option><option value="rssi">WiFi RSSI dBm</option><option value="peers">ESP-NOW peers</option><option value="ble">BLE state</option></optgroup><optgroup label="Location"><option value="gps">GPS fix</option><option value="speed">GPS speed kn</option><option value="sats">GPS satellites</option></optgroup><optgroup label="AI"><option value="llm">LLM state</option></optgroup><optgroup label="ESP-NOW / Bond"><option value="espnow">ESP-NOW up</option><option value="bond_mode">Bond mode</option><option value="bond_role">Bond role</option><option value="bond_paired">Bond paired</option><option value="bond_online">Bond online</option><option value="bond_synced">Bond synced</option><option value="bond_rssi">Bond RSSI dBm</option><option value="bond_peer_heap">Bond peer heap KB</option><option value="bond_peer_uptime">Bond peer uptime min</option><option value="pairmode">Pairing mode</option><option value="pairmode_secs">Pairing secs left</option><option value="peersknown">Peers known</option><option value="stalestpeerage">Stalest peer age s</option></optgroup><optgroup label="ESP-NOW metadata"><option value="room">Room</option><option value="zone">Zone</option><option value="tags">Tags</option></optgroup></select>
        <select id='a_cond_op' class='input-tall'><option value=">">&gt;</option><option value="<">&lt;</option><option value="=">=</option><option value=">=">&gt;=</option><option value="<=">&lt;=</option><option value="!=">!=</option><option value="CONTAINS">CONTAINS</option></select>
        <input id='a_cond_val' class='input-tall' placeholder='value' style='width:90px'>
        <label style='font-size:0.85em;color:var(--panel-fg);margin-left:0.5rem'>Mode:</label>
        <select id='a_trigger_mode' class='input-tall'><option value='repeat'>Repeat while true</option><option value='once'>Once when it becomes true</option></select>
      </div>
      <div style='font-size:0.78em;color:var(--panel-fg);opacity:0.7;margin:0.2rem 0 0.4rem 0'>Pair with an Interval trigger to set how often the condition is checked. "Once" fires on the false-&gt;true crossing only (then re-arms when it goes false).</div>
    </div>
    <div style='margin-top:0.5rem'>
      <label style='font-size:0.9em;color:var(--panel-fg);margin-bottom:0.25rem;display:block'>Commands & Logic:</label>
      <div id='command_fields' style='margin-top:0.25rem'>
        <div id='command_buttons' class='row-inline' style='gap:0.5rem;margin-top:0.5rem'>
        <button id='btn_add_cmd' type='button' class='btn btn-small' onclick='addCommandField()' title='Add another command to execute (e.g., ledcolor red, status, broadcast message)'>+ Add Command</button>
        <button id='btn_add_print' type='button' class='btn btn-small' onclick='addPrintField()' title='Add a print/broadcast message statement'>+ Add Print</button>
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
<div id='a_error' class='alert alert-danger' style='margin-top:0.5rem;display:none'></div>
</div>
  )AUTOPART1", HTTPD_RESP_USE_STRLEN);

  // Part 2: Download/Export section and automations table
  httpd_resp_send_chunk(req, R"AUTOPART2(
<div style='background:var(--panel-bg);border:1px solid var(--border);border-radius:8px 8px 0 0;padding:1rem;border-bottom:1px solid var(--border);margin:1rem 0 0 0'>
<div style='display:flex;gap:2rem;align-items:flex-start;flex-wrap:wrap'>
<div style='flex:1;min-width:280px'>
<h3 style='margin-top:0;color:var(--panel-fg)'>Import Automation</h3>
<p style='margin:0.5rem 0;color:var(--panel-fg);font-size:0.9em'>Import from a local JSON file:</p>
<div style='display:flex;gap:0.5rem;align-items:center;margin-bottom:0.5rem;flex-wrap:wrap'>
<label class='btn' style='padding:0.4rem 0.8rem;cursor:pointer;font-size:0.9em'>
  Choose File
  <input type='file' id='import_file' accept='.json' onchange='importFromFile(this)' style='display:none'>
</label>
<span id='import_filename' style='font-size:0.85em;color:var(--panel-fg);opacity:0.7'>No file chosen</span>
</div>
<div id='import_file_status' style='font-size:0.8em;margin-bottom:0.75rem'></div>
<p style='margin:0.5rem 0;color:var(--panel-fg);font-size:0.9em'>Or import from a GitHub URL:</p>
<div style='margin-bottom:0.5rem'>
<input type='text' id='github_url' placeholder='https://github.com/user/repo/blob/main/automation.json' style='width:100%;padding:0.5rem;border:1px solid var(--border);border-radius:4px;font-size:0.9em;box-sizing:border-box;background:var(--input-bg,var(--panel-bg));color:var(--panel-fg)'>
</div>
<div style='display:flex;gap:0.5rem;align-items:stretch;flex-wrap:wrap;margin-bottom:0.5rem'>
<input type='text' id='github_name' placeholder='Custom name (optional)' style='flex:1;min-width:150px;padding:0.5rem;border:1px solid var(--border);border-radius:4px;font-size:0.9em;height:auto;background:var(--input-bg,var(--panel-bg));color:var(--panel-fg)'>
<button onclick='downloadFromGitHub()' class='btn' style='padding:0.5rem 1rem;height:auto'>Import</button>
</div>
<div id='download_status' style='font-size:0.8em'></div>
</div>
<div style='flex:1;min-width:250px'>
<h3 style='margin-top:0;color:var(--panel-fg)'>Export Automations</h3>
<p style='margin:0.5rem 0 1rem 0;color:var(--panel-fg);font-size:0.9em'>Download your automations as JSON backup files:</p>
<div style='margin-bottom:0.75rem;padding:0.75rem;background:var(--panel-bg);border-radius:4px;border:1px solid var(--border)'>
<div style='font-weight:600;font-size:0.85em;color:var(--panel-fg);margin-bottom:0.5rem'>Export Options:</div>
<div style='display:flex;align-items:flex-start;justify-content:space-between;gap:1rem'>
<div style='flex:1'>
<label style='display:inline-flex;align-items:center;cursor:pointer'>
<input type='checkbox' id='export_separate' style='cursor:pointer;margin:0;padding:0;width:16px;height:16px'>
<span style='font-size:0.9em;color:var(--panel-fg);margin-left:0.5rem'>Separate files</span>
</label>
<div style='font-size:0.75em;color:var(--panel-fg);margin-top:0.3rem;margin-left:1.5rem'>Download one automation per file</div>
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
  hw.postFormText('/api/cli', { cmd: 'automation system status' })
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

window.disableAutomationSystem = async function() {
  if (!await hwConfirm('Disable the automation system? This will:\n\n• Stop the automation scheduler immediately\n• Keep ~16KB memory allocated until next reboot\n• Disable all scheduled automations\n\nContinue?')) {
    return;
  }
  
  hw.postFormText('/api/cli', { cmd: 'automation system disable' })
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

window.enableAutomationSystem = async function() {
  if (!await hwConfirm('Enable automation system? This will:\n\n• Start the automation scheduler immediately\n• Enable all scheduled automations\n\nContinue?')) {
    return;
  }
  hw.postFormText('/api/cli', { cmd: 'automation system enable' })
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
  
  // Delay the initial status check slightly so the page-load TLS connection
  // has time to close before we open a second one (ESP32 HTTPS has a small
  // concurrent-session limit; firing immediately on DOMContentLoaded can
  // saturate it while the chunked page response is still in flight).
  setTimeout(refreshAutomationSystemStatus, 300);
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
    var mw=document.getElementById('monthly_wrap');
    var yw=document.getElementById('yearly_wrap');
    if(dw) dw.style.display=(r==='weekly')?'flex':'none';
    if(mw) mw.style.display=(r==='monthly')?'block':'none';
    if(yw) yw.style.display=(r==='yearly')?'flex':'none';
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
function addPrintField(){ 
  const container=document.getElementById('command_fields'); 
  const buttonsDiv=document.getElementById('command_buttons'); 
  const div=document.createElement('div'); 
  div.className='cmd-field row-inline'; 
  div.style.cssText='gap:0.5rem;margin-bottom:0.3rem'; 
  const input = document.createElement('input');
  input.type = 'text';
  input.className = 'cmd-input input-tall';
  input.value = 'PRINT ';
  input.placeholder = 'PRINT Your message here';
  input.style.cssText = 'flex:1;min-width:260px;height:32px;line-height:32px;padding:0 0.5rem;box-sizing:border-box';
  const removeBtn = document.createElement('button');
  removeBtn.type = 'button';
  removeBtn.className = 'btn btn-small';
  removeBtn.textContent = 'Remove';
  removeBtn.onclick = function() { this.parentElement.remove(); };
  removeBtn.style.cssText = 'height:32px;line-height:32px;padding:0 10px;box-sizing:border-box;font-size:14px;display:inline-flex;align-items:center;margin:0';
  div.appendChild(input);
  div.appendChild(removeBtn);
  container.insertBefore(div, buttonsDiv);
  input.focus();
  input.setSelectionRange(6, 6);
}
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
  varSelect.innerHTML = '<optgroup label="Sensors"><option value="temp">Temperature</option><option value="distance">Distance</option><option value="light">Light</option><option value="motion">Motion</option></optgroup><optgroup label="Time"><option value="time">Time of day</option><option value="hour">Hour (0-23)</option><option value="day">Day of week</option><option value="ntp">Clock synced</option></optgroup><optgroup label="System"><option value="battery">Battery %</option><option value="heap">Free heap KB</option><option value="psram">Free PSRAM KB</option><option value="fsfree">Free storage KB</option><option value="uptime">Uptime min</option><option value="chiptemp">Chip temp C</option></optgroup><optgroup label="Network"><option value="wifi">WiFi state</option><option value="rssi">WiFi RSSI dBm</option><option value="peers">ESP-NOW peers</option><option value="ble">BLE state</option></optgroup><optgroup label="Location"><option value="gps">GPS fix</option><option value="speed">GPS speed kn</option><option value="sats">GPS satellites</option></optgroup><optgroup label="AI"><option value="llm">LLM state</option></optgroup><optgroup label="ESP-NOW / Bond"><option value="espnow">ESP-NOW up</option><option value="bond_mode">Bond mode</option><option value="bond_role">Bond role</option><option value="bond_paired">Bond paired</option><option value="bond_online">Bond online</option><option value="bond_synced">Bond synced</option><option value="bond_rssi">Bond RSSI dBm</option><option value="bond_peer_heap">Bond peer heap KB</option><option value="bond_peer_uptime">Bond peer uptime min</option><option value="pairmode">Pairing mode</option><option value="pairmode_secs">Pairing secs left</option><option value="peersknown">Peers known</option><option value="stalestpeerage">Stalest peer age s</option></optgroup><optgroup label="ESP-NOW metadata"><option value="room">Room</option><option value="zone">Zone</option><option value="tags">Tags</option></optgroup>';
  varSelect.onchange = function() { updateValuePlaceholder(this); };
  const opSelect = document.createElement('select'); 
  opSelect.className = 'logic-operator input-tall'; 
  opSelect.style.cssText = 'height:32px;width:60px'; 
  opSelect.innerHTML = '<option value=">">></option><option value="<"><</option><option value="=">=</option><option value=">=">>=</option><option value="<="><=</option><option value="!=">!=</option><option value="CONTAINS">CONTAINS</option>'; 
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
function updateValuePlaceholder(varSelect){ 
  try { 
    const field=varSelect.parentElement; 
    const valueInput=field.querySelector('.logic-value'); 
    if(!valueInput) return;
    const varType=varSelect.value;
    if(varType==='room') valueInput.placeholder='Kitchen';
    else if(varType==='zone') valueInput.placeholder='Upstairs';
    else if(varType==='tags') valueInput.placeholder='dimmable';
    else if(varType==='time') valueInput.placeholder='EVENING';
    else if(varType==='motion') valueInput.placeholder='DETECTED';
    else if(varType==='wifi'||varType==='ble') valueInput.placeholder='CONNECTED';
    else if(varType==='ntp') valueInput.placeholder='SYNCED';
    else if(varType==='gps') valueInput.placeholder='FIX';
    else if(varType==='llm') valueInput.placeholder='READY';
    else if(varType==='day') valueInput.placeholder='SAT';
    else if(varType==='hour') valueInput.placeholder='22';
    else if(varType==='battery') valueInput.placeholder='20';
    else if(varType==='rssi') valueInput.placeholder='-75';
    else if(varType==='heap') valueInput.placeholder='40';
    else if(varType==='psram') valueInput.placeholder='512';
    else if(varType==='fsfree') valueInput.placeholder='100';
    else if(varType==='uptime') valueInput.placeholder='60';
    else if(varType==='chiptemp') valueInput.placeholder='70';
    else if(varType==='peers') valueInput.placeholder='1';
    else if(varType==='speed') valueInput.placeholder='10';
    else if(varType==='sats') valueInput.placeholder='6';
    else if(varType==='bond_online') valueInput.placeholder='OFFLINE';
    else if(varType==='bond_synced') valueInput.placeholder='SYNCED';
    else if(varType==='bond_paired') valueInput.placeholder='PAIRED';
    else if(varType==='bond_role') valueInput.placeholder='MASTER';
    else if(varType==='bond_mode'||varType==='espnow'||varType==='pairmode') valueInput.placeholder='ACTIVE';
    else if(varType==='bond_rssi') valueInput.placeholder='-85';
    else if(varType==='bond_peer_heap') valueInput.placeholder='40';
    else if(varType==='bond_peer_uptime') valueInput.placeholder='5';
    else if(varType==='pairmode_secs') valueInput.placeholder='30';
    else if(varType==='peersknown') valueInput.placeholder='1';
    else if(varType==='stalestpeerage') valueInput.placeholder='25';
    else valueInput.placeholder='75';
  } catch(e) { 
    console.error('updateValuePlaceholder error:', e); 
  } 
}
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
      const m = Math.floor(diffSec/60);
      const s = diffSec % 60;
      relativeStr = 'in ' + m + 'm' + (s ? ' ' + s + 's' : '');
    } else if(diffSec < 86400){
      const h = Math.floor(diffSec/3600);
      const m = Math.floor((diffSec%3600)/60);
      relativeStr = 'in ' + h + 'h' + (m ? ' ' + m + 'm' : '');
    } else {
      const d = Math.floor(diffSec/86400);
      const h = Math.floor((diffSec%86400)/3600);
      relativeStr = 'in ' + d + 'd' + (h ? ' ' + h + 'h' : '');
    }
    return timeStr + '<br><small style="color:var(--panel-fg)">' + relativeStr + '</small>'; 
  } catch(e){ 
    return '\u2014'; 
  } 
}
// Cached last-fetched automations list so the per-second tick can update the
// Next Run column without re-fetching from the server.
let gLastAutos = [];

// v2 schema has an automation.triggers array. The UI was written against a
// single automation.trigger object + top-level runAtBoot flag. This function
// synthesizes the legacy fields from the array so the rest of the UI keeps
// working without per-site changes. Phase 2 of the multi-trigger work will
// replace this shim with a proper trigger-array UI.
function normalizeAutomation(a) {
  if (!a || !Array.isArray(a.triggers) || a.triggers.length === 0) return a;
  let primary = null, bootTrig = null;
  for (const t of a.triggers) {
    if (!t || !t.type) continue;
    const tt = t.type.toLowerCase();
    if (tt === 'boot') { if (!bootTrig) bootTrig = t; }
    else { if (!primary) primary = t; }
  }
  if (!primary) primary = a.triggers[0];  // all-boot edge case
  a.trigger = primary;
  if (bootTrig) {
    a.runAtBoot = true;
    if (typeof bootTrig.bootDelayMs !== 'undefined') a.bootDelayMs = bootTrig.bootDelayMs;
  }
  return a;
}

// Update only the Next Run cells in place. Runs every second. Doesn't touch
// the rest of the DOM so active buttons, edit forms, scroll position, etc.
// are preserved. If an automation's nextAt has been hit (overdue flag), we
// also trigger a fresh fetch so the scheduler's post-fire update shows up.
function tickNextRunCells() {
  if (!gLastAutos.length) return;
  const nowSec = Math.floor(Date.now()/1000);
  let anyOverdue = false;
  gLastAutos.forEach(a => {
    const id = (typeof a.id !== 'undefined') ? a.id : '';
    const cell = document.querySelector('[data-next-run-id="' + id + '"]');
    if (!cell) return;
    const sched = a.trigger || {};
    let rawType = (sched.type || a.type || '').toLowerCase();
    let runAtBoot = (a.runAtBoot === true);
    if (rawType === 'time') rawType = 'attime';
    else if (rawType === 'manual') rawType = 'afterdelay';
    else if (rawType === 'boot') { runAtBoot = true; rawType = 'afterdelay'; }
    const nextAt = parseInt(sched.nextAt || a.nextAt || 0);
    const isArmed = (rawType === 'afterdelay' && !runAtBoot && nextAt > nowSec);
    let html = formatNextRun(nextAt);
    if (isArmed) {
      html = '<span style="color:var(--accent,#ffa500);font-weight:600">\u23F1 Armed</span><br>' + html;
    }
    cell.innerHTML = html;
    if (nextAt > 0 && nextAt <= nowSec) anyOverdue = true;
  });
  // If anything is overdue, the scheduler fires within ~1s and writes a new
  // nextAt. Pull a fresh list so the UI reflects it.
  if (anyOverdue && !gAutosFetchInFlight) {
    scheduleAutosRefresh();
  }
}
let gAutosFetchInFlight = false;
let gAutosRefreshPending = null;
function scheduleAutosRefresh(){
  if (gAutosRefreshPending) return;
  gAutosRefreshPending = setTimeout(function(){
    gAutosRefreshPending = null;
    loadAutos();
  }, 1500);  // brief delay so the scheduler has time to reschedule
}
setInterval(tickNextRunCells, 1000);

function renderAutos(json) {
  try {
    let data = (typeof json === 'string') ? JSON.parse(json) : json;
    let autos = [];
    if (data && data.automations && Array.isArray(data.automations)) autos = data.automations;
    // Normalize triggers[] → trigger + runAtBoot so the rest of the UI works.
    autos.forEach(normalizeAutomation);
    gLastAutos = autos;
    let html = '<table style="width:100%;border-collapse:collapse">';
    html += '<tr style="background:var(--crumb-bg);color:var(--panel-fg)"><th style="padding:0.5rem;text-align:left">ID</th><th style="padding:0.5rem;text-align:left">Name</th><th style="padding:0.5rem;text-align:left">Enabled</th><th style="padding:0.5rem;text-align:left">Type</th><th style="padding:0.5rem;text-align:left">Summary</th><th style="padding:0.5rem;text-align:left">Next Run</th><th style="padding:0.5rem">Actions</th></tr>';
    if (autos.length === 0) {
      html += '<tr><td colspan="7" style="padding:2rem;text-align:center;color:var(--panel-fg);font-style:italic">No automations yet. Create your first automation above!</td></tr>';
    } else {
      autos.forEach(a => {
        let name = a.name || '(unnamed)';
        let enabled = (a.enabled === true ? 'Yes' : 'No');
        let sched = a.trigger || {};
        let rawType = (sched.type || a.type || '');
        let t = rawType.toLowerCase();
        let runAtBoot = (a.runAtBoot === true);
        // Normalize v1 trigger type names to legacy rendering values.
        if (t === 'time') t = 'attime';
        else if (t === 'manual') t = 'afterdelay';
        else if (t === 'boot') { runAtBoot = true; t = 'afterdelay'; }
        let type = runAtBoot ? 'On Boot' : rawType;
        let summary = '';
        if (t === 'attime') {
          const rec = (sched.recurrence || a.recurrence || '').toLowerCase();
          if (rec === 'monthly') {
            const dom = sched.dayOfMonth || a.dayOfMonth || '?';
            summary = 'On the ' + dom + ' of each month at ' + (sched.time || a.time || '?');
          } else if (rec === 'yearly') {
            const months = ['?','Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
            const moy = sched.month || a.month || 0;
            const dom = sched.dayOfMonth || a.dayOfMonth || '?';
            summary = 'Every ' + (months[moy] || '?') + ' ' + dom + ' at ' + (sched.time || a.time || '?');
          } else {
            summary = 'At ' + (sched.time || a.time || '?') + (sched.days || a.days ? ' on ' + (sched.days || a.days) : '');
            const wi = sched.weekInterval || a.weekInterval || 1;
            if (wi > 1) summary += ' (every ' + wi + ' weeks)';
            else if (sched.recurrence) summary += ' (' + sched.recurrence + ')';
          }
        } else if (t === 'afterdelay') {
          summary = 'After ' + (sched.delayMs || a.delayMs || '?') + ' ms';
        } else if (t === 'interval') {
          summary = 'Every ' + (sched.intervalMs || a.intervalMs || '?') + ' ms';
        } else {
          summary = '\u2014';
        }
        // Mention other triggers beyond the primary (the synthesized boot is
        // handled by the runAtBoot branch below).
        if (Array.isArray(a.triggers) && a.triggers.length > 1) {
          const extras = [];
          let skippedPrimary = false, skippedBoot = !runAtBoot;
          for (const tr of a.triggers) {
            if (!tr || !tr.type) continue;
            const tt = tr.type.toLowerCase();
            if (!skippedPrimary && tt !== 'boot') { skippedPrimary = true; continue; }
            if (!skippedBoot && tt === 'boot') { skippedBoot = true; continue; }
            if (tt === 'time') extras.push('Time ' + (tr.time || '?'));
            else if (tt === 'interval') extras.push('Every ' + (tr.intervalMs || '?') + 'ms');
            else if (tt === 'manual') extras.push('Manual ' + (tr.delayMs || '?') + 'ms');
            else if (tt === 'boot') extras.push('Boot' + (tr.bootDelayMs ? ' +' + tr.bootDelayMs + 'ms' : ''));
          }
          if (extras.length > 0) {
            summary += ' <em style="color:var(--muted)">+ ' + extras.join(', ') + '</em>';
          }
        }
        if (Array.isArray(a.commands) && a.commands.length) {
          summary += ' | cmds: ' + a.commands.join('; ');
        } else if (a.command) {
          summary += ' | cmd: ' + a.command;
        }
        if (runAtBoot) {
          summary += ' | Boot';
          let bootDelay = sched.bootDelayMs || a.bootDelayMs;
          if (typeof bootDelay !== 'undefined' && bootDelay !== null) {
            summary += ' (' + bootDelay + ' ms)';
          }
        }
        let nextAt = (sched.nextAt || a.nextAt);
        const nowSec = Math.floor(Date.now()/1000);
        const isArmed = (t === 'afterdelay' && !runAtBoot && nextAt && parseInt(nextAt) > nowSec);
        let nextRun = formatNextRun(nextAt);
        if (isArmed) {
          nextRun = '<span style="color:var(--accent,#ffa500);font-weight:600">\u23F1 Armed</span><br>' + nextRun;
        }
        let id = (typeof a.id !== 'undefined') ? a.id : '';
        let btns = '';
        if (id !== '') {
          if (a.enabled === true) {
            btns += '<button class="btn" onclick="autoToggle(' + id + ',0)" style="margin-right:0.3rem">Disable</button>';
          } else {
            btns += '<button class="btn" onclick="autoToggle(' + id + ',1)" style="margin-right:0.3rem">Enable</button>';
          }
          btns += '<button class="btn" onclick="autoRun(' + id + ')" style="margin-right:0.3rem">Run Now</button>';
          if (t === 'afterdelay') {
            btns += '<button class="btn" onclick="autoTrigger(' + id + ')" style="margin-right:0.3rem">Trigger</button>';
          }
          btns += '<button class="btn" onclick="autoEdit(' + id + ')" style="margin-right:0.3rem">Edit</button>';
          btns += '<button class="btn" onclick="autoDelete(' + id + ')" style="margin-right:0.3rem;color:var(--danger)">Delete</button>';
          btns += '<button class="btn" onclick="exportSingleAutomation(' + id + ')">Export</button>';
        }
        html += '<tr style="border-bottom:1px solid var(--border)">';
        html += '<td style="padding:0.5rem">' + id + '</td>';
        html += '<td style="padding:0.5rem">' + name + '</td>';
        html += '<td style="padding:0.5rem">' + enabled + '</td>';
        html += '<td style="padding:0.5rem">' + type + '</td>';
        html += '<td style="padding:0.5rem">' + summary + '</td>';
        html += '<td data-next-run-id="' + id + '" style="padding:0.5rem">' + nextRun + '</td>';
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
  gAutosFetchInFlight = true;
  hw.fetchText('/api/automations').then(txt => {
    console.log('[AUTOMATIONS] Automations data length:',txt.length);
    renderAutos(txt);
  }).catch(e => {
    console.error('[AUTOMATIONS] Load error:',e);
    document.getElementById('autos').innerHTML = 'Error loading automations: ' + e.message;
  }).finally(() => {
    gAutosFetchInFlight = false;
  });
}
console.log('[AUTOMATIONS] Part 1: Complete');
</script>)AUTOPART6", HTTPD_RESP_USE_STRLEN);

  // Part 7: CLI helper and createAutomation function (large, split into sub-parts)
  httpd_resp_send_chunk(req, R"AUTOPART7(<script>
function postCLI(cmd){
  return hw.postFormText('/api/cli', { cmd: cmd });
}
function postCLIValidate(cmd){
  return hw.postFormText('/api/cli', { cmd: cmd, validate: '1' });
}
// ============================================================================
// Secondary trigger row management
// ============================================================================
// Builds on top of the primary trigger form. Each secondary row is a compact
// element with its own type dropdown + type-specific fields. Rows can be added
// up to (4 - 1) = 3 (since the primary counts as one of the 4 max triggers).
// Manual triggers are capped at 1 across the entire automation — enforced at
// submit time by scanning primary + all secondaries.

function addSecondaryTrigger(initial){
  const primaryType=document.getElementById('a_type').value;
  const secondaryCount=document.querySelectorAll('#secondary_triggers_container .secondary-trigger').length;
  // Primary + runAtBoot-synthesized boot + secondaries. runAtBoot counts as 1
  // boot trigger if checked, so the cap is 4 - 1 - (runAtBoot?1:0) secondaries.
  const runBootChecked=(document.getElementById('a_runatboot')||{}).checked===true;
  const maxSecondaries=4-1-(runBootChecked?1:0);
  if(secondaryCount>=maxSecondaries){
    alert('Maximum of 4 triggers per automation reached.');
    return;
  }
  const tpl=document.getElementById('secondary_trigger_template');
  if(!tpl) return;
  const node=tpl.content.cloneNode(true).firstElementChild;
  document.getElementById('secondary_triggers_container').appendChild(node);
  if(initial){ populateSecondaryTrigger(node,initial); }
  stTypeChanged(node.querySelector('.st-type'));
  renumberTriggers();
}

function removeSecondaryTrigger(btn){
  const row=btn.closest('.secondary-trigger');
  if(row) row.remove();
  renumberTriggers();
}
function renumberTriggers(){
  const rows=document.querySelectorAll('#secondary_triggers_container .secondary-trigger');
  rows.forEach(function(r,i){ const lbl=r.querySelector('.st-num'); if(lbl) lbl.textContent='Trigger '+(i+2); });
}

function stTypeChanged(sel){
  const row=sel.closest('.secondary-trigger');
  if(!row) return;
  const type=sel.value;
  row.querySelectorAll('.st-fields').forEach(f=>{f.style.display='none';});
  const show=row.querySelector('.st-fields-'+type);
  if(show) show.style.display='inline-flex';
  if(type==='time'){ stRecurChanged(row.querySelector('.st-recur')); }
}

function stRecurChanged(sel){
  const row=sel.closest('.secondary-trigger');
  if(!row) return;
  const dw=row.querySelector('.st-days-wrap');
  if(dw) dw.style.display=(sel.value==='weekly')?'inline-flex':'none';
}

// Serialize a secondary-trigger row into a plain object matching the v1
// backend `trigger` JSON schema. Returns null if the row is incomplete.
function getSecondaryTriggerData(row){
  const type=row.querySelector('.st-type').value;
  const obj={type};
  if(type==='time'){
    const t=row.querySelector('.st-time').value;
    if(!t) return null;
    obj.time=t;
    const recur=row.querySelector('.st-recur').value;
    obj.recurrence=recur;
    if(recur==='weekly'){
      const days=[];
      row.querySelectorAll('.st-day:checked').forEach(c=>days.push(c.value));
      if(days.length===0) return null;
      obj.days=days.join(',');
    }
  } else if(type==='interval'){
    const v=parseInt(row.querySelector('.st-interval-value').value,10);
    if(!(v>0)) return null;
    const u=row.querySelector('.st-interval-unit').value;
    const mult=(u==='ms')?1:(u==='min')?60000:(u==='hr')?3600000:1000;
    obj.intervalMs=v*mult;
  } else if(type==='manual'){
    const v=parseInt(row.querySelector('.st-delay-value').value,10);
    if(!(v>=0)) return null;
    const u=row.querySelector('.st-delay-unit').value;
    const mult=(u==='ms')?1:(u==='min')?60000:1000;
    obj.delayMs=v*mult;
  } else if(type==='boot'){
    const d=parseInt(row.querySelector('.st-boot-delay').value,10)||0;
    if(d<0) return null;
    obj.bootDelayMs=d;
  }
  return obj;
}

function populateSecondaryTrigger(row,t){
  if(!t||!t.type) return;
  const sel=row.querySelector('.st-type');
  sel.value=t.type;
  if(t.type==='time'){
    if(t.time) row.querySelector('.st-time').value=t.time;
    if(t.recurrence==='weekly'){ row.querySelector('.st-recur').value='weekly'; }
    else{ row.querySelector('.st-recur').value='daily'; }
    if(t.days){
      const set=new Set((t.days||'').toLowerCase().split(',').map(s=>s.trim()));
      row.querySelectorAll('.st-day').forEach(c=>{c.checked=set.has(c.value);});
    }
  } else if(t.type==='interval'){
    const ms=parseInt(t.intervalMs)||0;
    let v=ms, u='ms';
    if(ms%3600000===0&&ms>=3600000){v=ms/3600000;u='hr';}
    else if(ms%60000===0&&ms>=60000){v=ms/60000;u='min';}
    else if(ms%1000===0){v=ms/1000;u='s';}
    row.querySelector('.st-interval-value').value=v;
    row.querySelector('.st-interval-unit').value=u;
  } else if(t.type==='manual'){
    const ms=parseInt(t.delayMs)||0;
    let v=ms, u='ms';
    if(ms%60000===0&&ms>=60000){v=ms/60000;u='min';}
    else if(ms%1000===0){v=ms/1000;u='s';}
    row.querySelector('.st-delay-value').value=v;
    row.querySelector('.st-delay-unit').value=u;
  } else if(t.type==='boot'){
    row.querySelector('.st-boot-delay').value=(typeof t.bootDelayMs!=='undefined')?t.bootDelayMs:0;
  }
}

// Collect secondary trigger objects for submit. Returns null on validation fail.
function collectSecondaryTriggers(){
  const rows=document.querySelectorAll('#secondary_triggers_container .secondary-trigger');
  const result=[];
  for(const row of rows){
    const data=getSecondaryTriggerData(row);
    if(!data){
      return null;  // incomplete row
    }
    result.push(data);
  }
  return result;
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
  const editIdEl=document.getElementById('a_edit_id');
  const editId=editIdEl?editIdEl.value.trim():'';
  document.getElementById('a_error').textContent=''; document.getElementById('a_error').style.display='none'; 
  const recur=(document.getElementById('a_recur')?document.getElementById('a_recur').value:'daily');
  let dayOfMonth=0, monthOfYear=0;
  if(type==='atTime'&&recur==='monthly'){
    const dEl=document.getElementById('a_day_of_month');
    dayOfMonth=dEl?parseInt(dEl.value,10):0;
    if(!(dayOfMonth>=1&&dayOfMonth<=31)){
      document.getElementById('a_error').textContent='Monthly requires a day of month (1-31).';
      document.getElementById('a_error').style.display='block';
      return;
    }
  }
  if(type==='atTime'&&recur==='yearly'){
    const dEl=document.getElementById('a_day_of_month_yearly');
    const mEl=document.getElementById('a_month_of_year');
    dayOfMonth=dEl?parseInt(dEl.value,10):0;
    monthOfYear=mEl?parseInt(mEl.value,10):0;
    if(!(dayOfMonth>=1&&dayOfMonth<=31)||!(monthOfYear>=1&&monthOfYear<=12)){
      document.getElementById('a_error').textContent='Yearly requires a month and day (1-12, 1-31).';
      document.getElementById('a_error').style.display='block';
      return;
    }
  }
  const selectedDays=[]; 
  if(type==='atTime'&&recur==='weekly'){ 
    ['mon','tue','wed','thu','fri','sat','sun'].forEach(day=>{ 
      if(document.getElementById('day_'+day).checked) selectedDays.push(day); 
    }); 
    if(selectedDays.length===0){ 
      document.getElementById('a_error').textContent='Please select at least one day for a weekly schedule.'; document.getElementById('a_error').style.display='block';
      return; 
    } 
  } 
  const days=selectedDays.join(',');
  let weekInterval=1;
  if(type==='atTime'&&recur==='weekly'){
    const wiEl=document.getElementById('a_week_interval');
    if(wiEl){
      const v=parseInt(wiEl.value,10);
      if(!isNaN(v)&&v>=1&&v<=12) weekInterval=v;
    }
  }
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
  let logicError=false;
  logicFields.forEach(field=>{ 
    const typeSelect=field.querySelector('.logic-type'); 
    const varSelect=field.querySelector('.logic-var'); 
    const operatorSelect=field.querySelector('.logic-operator'); 
    const value=field.querySelector('.logic-value'); 
    const action=field.querySelector('.logic-action'); 
    if(typeSelect && action){ 
      const typeVal=typeSelect.value; 
      const actVal=action.value.trim(); 
      if(typeVal==='ELSE'){ 
        if(actVal) conditionalChain.push(typeVal+' '+actVal); else logicError=true;
      } else if(typeVal){ 
        if(varSelect && operatorSelect && value){ 
          const varVal=varSelect.value; 
          const opVal=operatorSelect.value; 
          const valVal=value.value.trim(); 
          if(varVal && opVal && valVal && actVal){ 
            conditionalChain.push(typeVal+' '+varVal+opVal+valVal+' THEN '+actVal); 
          } else { logicError=true; }
        } 
      } 
    } 
  }); 
  if(logicError){ document.getElementById('a_error').textContent='Logic field incomplete: all fields (variable, operator, value, action) are required.'; document.getElementById('a_error').style.display='block'; return; }
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
      if(recur==='weekly'&&weekInterval>1) parts.push('weekinterval='+weekInterval);
      if(recur==='monthly'&&dayOfMonth>0) parts.push('dayofmonth='+dayOfMonth);
      if(recur==='yearly'&&dayOfMonth>0) parts.push('dayofmonth='+dayOfMonth);
      if(recur==='yearly'&&monthOfYear>0) parts.push('month='+monthOfYear);
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
    parts.push('commands="'+cmdsParam.replace(/"/g,'\\"')+'"');
    parts.push('enabled='+(en?1:0));
    // Option 2: top-level "Fire when" condition + repeat/once trigger mode.
    { const cv=(document.getElementById('a_cond_var')||{}).value||''; const co=(document.getElementById('a_cond_op')||{}).value||'>'; const cval=((document.getElementById('a_cond_val')||{}).value||'').trim(); const tm=(document.getElementById('a_trigger_mode')||{}).value||'repeat';
      if(cv && cval){ parts.push('condition="'+(cv+co+cval).replace(/"/g,'\\"')+'"'); if(tm==='once') parts.push('triggermode=once'); } }
    if(runAtBoot) parts.push('runatboot=1');
    // Secondary triggers: append as a JSON array via the `secondarytriggers`
    // arg. The backend merges these with the primary trigger + runAtBoot boot
    // into the triggers[] array, with a cap of 4 total.
    const secondaries=collectSecondaryTriggers();
    if(secondaries===null){
      document.getElementById('a_error').textContent='One or more triggers is incomplete. Fill in all fields or remove the row.';
      document.getElementById('a_error').style.display='block';
      throw new Error('Invalid secondary');
    }
    const primaryIsManual=(type==='afterDelay');
    const manualSecCount=secondaries.filter(s=>s.type==='manual').length;
    if((primaryIsManual?1:0)+manualSecCount>1){
      document.getElementById('a_error').textContent='Only one manual (After Delay) trigger is allowed per automation.';
      document.getElementById('a_error').style.display='block';
      throw new Error('Manual cap');
    }
    const totalTriggers=1+(runAtBoot?1:0)+secondaries.length;
    if(totalTriggers>4){
      document.getElementById('a_error').textContent='Maximum of 4 triggers per automation.';
      document.getElementById('a_error').style.display='block';
      throw new Error('Total cap');
    }
    if(secondaries.length>0){
      const json=JSON.stringify(secondaries);
      parts.push('secondarytriggers="'+json.replace(/"/g,'\\"')+'"');
    }
    if(editId && idx===0) parts.push('id='+editId);
    return parts.join(' ');
  };
  const fullCmds=(times.length?times:[null]).map((t,idx)=>buildParts(t,idx)); 
  
  Promise.all(fullCmds.map(c=>postCLIValidate(c))).then(vals=>{ 
    for(let i=0;i<vals.length;i++){ 
      const v=(vals[i]||'').trim(); 
      if(v!=='VALID'){ 
        document.getElementById('a_error').textContent=v; document.getElementById('a_error').style.display='block';
        throw new Error('Invalid'); 
      } 
    } 
    return Promise.all(fullCmds.map(c=>postCLI(c))); 
  }).then(results=>{ 
    const err=results.find(t=>t.toLowerCase().indexOf('error:')>=0); 
    if(err){ 
      document.getElementById('a_error').textContent=err; document.getElementById('a_error').style.display='block';
      return; 
    } 
    const resetForm=()=>{
      document.getElementById('a_name').value=''; 
      document.querySelectorAll('.time-input').forEach(input=>input.value=''); 
      ['mon','tue','wed','thu','fri','sat','sun'].forEach(day=>{ 
        let el=document.getElementById('day_'+day); 
        if(el) el.checked=false; 
      }); 
      document.getElementById('a_delay').value=''; 
      document.getElementById('a_interval').value=''; 
      var elRunBoot=document.getElementById('a_runatboot'); if(elRunBoot) elRunBoot.checked=false;
      { var cvr=document.getElementById('a_cond_var'); if(cvr) cvr.value=''; var cvv=document.getElementById('a_cond_val'); if(cvv) cvv.value=''; var tmr=document.getElementById('a_trigger_mode'); if(tmr) tmr.value='repeat'; } 
      const cwrap=document.getElementById('command_fields'); 
      if(cwrap){ 
        cwrap.innerHTML='<div id="command_buttons" class="row-inline" style="gap:0.5rem;margin-top:0.5rem"><button id="btn_add_cmd" type="button" class="btn btn-small" onclick="addCommandField()" title="Add another command to execute (e.g., ledcolor red, status, broadcast message)">+ Add Command</button><button id="btn_add_print" type="button" class="btn btn-small" onclick="addPrintField()" title="Add a print/broadcast message statement">+ Add Print</button><button id="btn_add_logic" type="button" class="btn btn-small" onclick="addLogicField()" title="Add conditional logic (IF/THEN statements for sensor-based automation)">+ Add Logic</button><button id="btn_add_wait" type="button" class="btn btn-small" onclick="addWaitField()" title="Add a wait/pause command with dropdown timing">+ Add Wait</button></div>'; 
      } 
      const eidEl=document.getElementById('a_edit_id'); if(eidEl) eidEl.value='';
      const addBtn=document.querySelector('button[onclick="createAutomation()"]'); if(addBtn) addBtn.textContent='Add';
      loadAutos();
    };
    resetForm();
  }).catch(e=>{ 
    if(!document.getElementById('a_error').textContent){ 
      document.getElementById('a_error').textContent='Validation error: '+e.message; document.getElementById('a_error').style.display='block';
    } 
  }); 
}
function autoToggle(id,en){ 
  const cmd='automation ' + (en? 'enable':'disable') + ' id='+id; 
  postCLI(cmd).then(()=>loadAutos()); 
}
async function autoDelete(id){
  if(!await hwConfirm('Delete automation '+id+'?')) return;
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
function autoTrigger(id){
  postCLI('automation trigger id='+id).then(r=>{
    if(r.toLowerCase().indexOf('error:')>=0){
      alert(r);
    } else {
      // No alert — the "Armed" badge + countdown in the Next Run column
      // gives immediate visual feedback once loadAutos() returns.
      loadAutos();
    }
  });
}
function autoEdit(id){
  hw.fetchJSON('/api/automations').then(data=>{
    if(!data||!data.automations) return;
    const a=normalizeAutomation(data.automations.find(x=>String(x.id)===String(id)));
    if(!a){alert('Automation not found');return;}
    const sched=a.trigger||{};
    // Clear any previous secondary trigger rows, then re-populate from the
    // saved triggers array (excluding the primary and the synthesized boot).
    const secCont=document.getElementById('secondary_triggers_container');
    if(secCont) secCont.innerHTML='';
    if(Array.isArray(a.triggers) && a.triggers.length > 0){
      // The primary trigger (first non-boot) and a runAtBoot-derived boot
      // trigger are already handled by the main form. Everything else goes
      // into the secondary rows.
      let primarySeen=false, bootSeen=false;
      for(const t of a.triggers){
        if(!t||!t.type) continue;
        const tt=t.type.toLowerCase();
        if(!primarySeen && tt!=='boot'){ primarySeen=true; continue; }
        if(!bootSeen && tt==='boot' && a.runAtBoot===true){ bootSeen=true; continue; }
        addSecondaryTrigger(t);
      }
    }
    document.getElementById('a_name').value=a.name||'';
    const typeRaw=((sched.type||a.type||'time')).toLowerCase();
    let typeVal='atTime';
    // Accept both legacy (atTime/afterDelay/onBoot) and v1 (time/manual/boot) type names.
    if(typeRaw==='afterdelay'||typeRaw==='manual') typeVal='afterDelay';
    else if(typeRaw==='interval') typeVal='interval';
    else if(typeRaw==='onboot'||typeRaw==='boot') typeVal='onBoot';
    else if(a.runAtBoot===true) typeVal='onBoot';
    document.getElementById('a_type').value=typeVal;
    autoTypeChanged();
    const recurEl=document.getElementById('a_recur');
    if(recurEl){recurEl.value=(sched.recurrence||a.recurrence||'daily');recurChanged();}
    const allTimes=sched.times||a.times||(sched.time?[sched.time]:a.time?[a.time]:[]);
    if(typeVal==='atTime'&&allTimes.length>0){
      const mainInput=document.querySelector('#grp_atTime .time-input');
      if(mainInput) mainInput.value=allTimes[0];
      const addlCont=document.getElementById('time_fields');
      if(addlCont) addlCont.innerHTML='';
      for(let i=1;i<allTimes.length;i++){addTimeField();const tf=document.querySelectorAll('.time-field .time-input');if(tf[i-1])tf[i-1].value=allTimes[i];}
    }
    if((sched.recurrence||a.recurrence||'')==='weekly'){
      ['mon','tue','wed','thu','fri','sat','sun'].forEach(d=>{const el=document.getElementById('day_'+d);if(el)el.checked=false;});
      (sched.days||a.days||[]).forEach(d=>{const el=document.getElementById('day_'+d.toLowerCase().substring(0,3));if(el)el.checked=true;});
      const wiEl=document.getElementById('a_week_interval');
      if(wiEl){const wi=sched.weekInterval||a.weekInterval||1;wiEl.value=(wi>=1&&wi<=12)?wi:1;}
    }
    if((sched.recurrence||a.recurrence||'')==='monthly'){
      const dEl=document.getElementById('a_day_of_month');
      if(dEl){const dom=sched.dayOfMonth||a.dayOfMonth||1;dEl.value=(dom>=1&&dom<=31)?dom:1;}
    }
    if((sched.recurrence||a.recurrence||'')==='yearly'){
      const dEl=document.getElementById('a_day_of_month_yearly');
      const mEl=document.getElementById('a_month_of_year');
      if(dEl){const dom=sched.dayOfMonth||a.dayOfMonth||1;dEl.value=(dom>=1&&dom<=31)?dom:1;}
      if(mEl){const moy=sched.month||a.month||1;mEl.value=(moy>=1&&moy<=12)?moy:1;}
    }
    if(typeVal==='afterDelay') document.getElementById('a_delay').value=sched.delayMs||a.delayMs||0;
    if(typeVal==='interval') document.getElementById('a_interval').value=sched.intervalMs||a.intervalMs||0;
    // Option 2: populate the "Fire when" condition + trigger mode from the record.
    { var cvr=document.getElementById('a_cond_var'); var cor=document.getElementById('a_cond_op'); var cvv=document.getElementById('a_cond_val'); var tmr=document.getElementById('a_trigger_mode');
      if(cvr&&cor&&cvv){ cvr.value='';cor.value='>';cvv.value='';
        var cond=(a.condition||'').trim();
        if(cond){ var ops=['CONTAINS','>=','<=','!=','>','<','=']; for(var oi=0;oi<ops.length;oi++){ var ix=cond.indexOf(ops[oi]); if(ix>0){ cvr.value=cond.substring(0,ix).trim().toLowerCase(); cor.value=ops[oi]; cvv.value=cond.substring(ix+ops[oi].length).trim(); break; } } } }
      if(tmr) tmr.value=(a.triggerMode==='once')?'once':'repeat'; }
    const commands=Array.isArray(a.commands)?a.commands:(a.command?[a.command]:[]);
    const cwrap=document.getElementById('command_fields');
    if(cwrap){
      cwrap.innerHTML='<div id="command_buttons" class="row-inline" style="gap:0.5rem;margin-top:0.5rem"><button id="btn_add_cmd" type="button" class="btn btn-small" onclick="addCommandField()" title="Add another command to execute (e.g., ledcolor red, status, broadcast message)">+ Add Command</button><button id="btn_add_print" type="button" class="btn btn-small" onclick="addPrintField()" title="Add a print/broadcast message statement">+ Add Print</button><button id="btn_add_logic" type="button" class="btn btn-small" onclick="addLogicField()" title="Add conditional logic (IF/THEN statements for sensor-based automation)">+ Add Logic</button><button id="btn_add_wait" type="button" class="btn btn-small" onclick="addWaitField()" title="Add a wait/pause command with dropdown timing">+ Add Wait</button></div>';
      commands.forEach(cmd=>{
        const cmdStr=typeof cmd==='object'?(cmd.conditional||JSON.stringify(cmd)):String(cmd);
        addCommandField();
        const inp=cwrap.querySelectorAll('.cmd-input');
        if(inp.length>0) inp[inp.length-1].value=cmdStr;
      });
    }
    document.getElementById('a_enabled').checked=a.enabled!==false;
    const rbEl=document.getElementById('a_runatboot');
    if(rbEl) rbEl.checked=(sched.runAtBoot===true||a.runAtBoot===true);
    let eidEl=document.getElementById('a_edit_id');
    if(!eidEl){eidEl=document.createElement('input');eidEl.type='hidden';eidEl.id='a_edit_id';document.getElementById('a_name').parentElement.appendChild(eidEl);}
    eidEl.value=id;
    const addBtn=document.querySelector('button[onclick="createAutomation()"]');
    if(addBtn) addBtn.textContent='Save Changes';
    document.getElementById('a_name').scrollIntoView({behavior:'smooth',block:'center'});
  }).catch(e=>console.error('autoEdit:',e));
}
</script>)AUTOPART7", HTTPD_RESP_USE_STRLEN);

  // Part 8: Import engine, GitHub download, file import, and export functions
  httpd_resp_send_chunk(req, R"AUTOPART8(<script>
// Shared import engine: takes a parsed automation JSON object (new schema) and calls 'automation add'
function importAutomationFromJson(autoJson, statusEl){
  const name=(autoJson.name||'').trim();
  if(!name){
    if(statusEl) statusEl.innerHTML='<span style="color:var(--danger)">Error: missing name</span>';
    return Promise.reject(new Error('missing name'));
  }
  const sched=autoJson.trigger||{};
  const rawType=(sched.type||autoJson.type||'').toLowerCase();
  if(!rawType){
    if(statusEl) statusEl.innerHTML='<span style="color:var(--danger)">Error: missing schedule.type</span>';
    return Promise.reject(new Error('missing type'));
  }
  const parts=['automation add'];
  parts.push('name="'+name.replace(/\\/g,'\\\\').replace(/"/g,'\\"')+'"');
  parts.push('type='+rawType);
  const time=sched.time||autoJson.time||'';
  if(time) parts.push('time='+time);
  const recurrence=sched.recurrence||autoJson.recurrence||'';
  if(recurrence) parts.push('recurrence='+recurrence);
  const days=sched.days||autoJson.days||'';
  if(days) parts.push('days='+days);
  const wi=sched.weekInterval||autoJson.weekInterval;
  if(wi&&wi>1) parts.push('weekinterval='+wi);
  const dom=sched.dayOfMonth||autoJson.dayOfMonth;
  if(dom&&dom>=1&&dom<=31) parts.push('dayofmonth='+dom);
  const moy=sched.month||autoJson.month;
  if(moy&&moy>=1&&moy<=12) parts.push('month='+moy);
  const delayMs=typeof sched.delayMs!=='undefined'?sched.delayMs:autoJson.delayMs;
  if(typeof delayMs!=='undefined'&&delayMs!==null) parts.push('delayms='+delayMs);
  const intervalMs=typeof sched.intervalMs!=='undefined'?sched.intervalMs:autoJson.intervalMs;
  if(typeof intervalMs!=='undefined'&&intervalMs!==null) parts.push('intervalms='+intervalMs);
  if(sched.runAtBoot===true||autoJson.runAtBoot===true) parts.push('runatboot=1');
  const bootDelay=typeof sched.bootDelayMs!=='undefined'?sched.bootDelayMs:autoJson.bootDelayMs;
  if(typeof bootDelay!=='undefined'&&bootDelay!==null) parts.push('bootdelayms='+bootDelay);
  // Propagate secondary triggers if present in the import JSON. This ensures
  // multi-trigger automations round-trip through export → import.
  if(Array.isArray(autoJson.secondaryTriggers) && autoJson.secondaryTriggers.length > 0){
    parts.push('secondarytriggers="'+JSON.stringify(autoJson.secondaryTriggers).replace(/"/g,'\\"')+'"');
  }
  const condition=autoJson.condition||'';
  if(condition) parts.push('condition="'+condition.replace(/\\/g,'\\\\').replace(/"/g,'\\"')+'"');
  const commands=Array.isArray(autoJson.commands)?autoJson.commands:(autoJson.command?[autoJson.command]:[]);
  if(commands.length===0){
    if(statusEl) statusEl.innerHTML='<span style="color:var(--danger)">Error: missing commands</span>';
    return Promise.reject(new Error('missing commands'));
  }
  const cmdsParam=commands.map(c=>typeof c==='object'?(c.conditional||JSON.stringify(c)):c).join(';');
  parts.push('commands="'+cmdsParam.replace(/\\/g,'\\\\').replace(/"/g,'\\"')+'"');
  parts.push('enabled='+(autoJson.enabled!==false?1:0));
  const cmd=parts.join(' ');
  function showImportError(msg){
    const errEl=document.getElementById('a_error');
    if(errEl){ errEl.textContent=msg; errEl.style.display='block'; }
    if(statusEl) statusEl.innerHTML='<span style="color:var(--danger)">'+msg+'</span>';
  }
  console.log('[importAutomationFromJson] Validating import for "'+name+'":', cmd);
  return postCLIValidate(cmd).then(v=>{
    const vv=(v||'').trim();
    console.log('[importAutomationFromJson] Validation result for "'+name+'":', vv);
    if(vv!=='VALID'){
      console.error('[importAutomationFromJson] VALIDATION FAILED for "'+name+'":', vv);
      showImportError('Import "'+name+'" failed: '+vv);
      throw new Error(vv);
    }
    console.log('[importAutomationFromJson] Validation passed, executing import for "'+name+'"');
    return postCLI(cmd);
  }).then(r=>{
    console.log('[importAutomationFromJson] Import result for "'+name+'":', r);
    if(r.toLowerCase().indexOf('error:')>=0){
      console.error('[importAutomationFromJson] IMPORT FAILED for "'+name+'":', r);
      showImportError('Import "'+name+'" failed: '+r);
      throw new Error(r);
    }
    return r;
  });
}
function importFromFile(input){
  const status=document.getElementById('import_file_status');
  const label=document.getElementById('import_filename');
  if(!input.files||input.files.length===0) return;
  const file=input.files[0];
  if(label) label.textContent=file.name;
  status.innerHTML='<span style="color:var(--accent)">Reading...</span>';
  const reader=new FileReader();
  reader.onload=function(e){
    let parsed;
    try{ parsed=JSON.parse(e.target.result); }
    catch(ex){ status.innerHTML='<span style="color:var(--danger)">Invalid JSON: '+ex.message+'</span>'; return; }
    // Support single automation object or { automations: [...] } wrapper
    const items=Array.isArray(parsed.automations)?parsed.automations:[parsed];
    let done=0,errs=0;
    const next=(idx)=>{
      if(idx>=items.length){
        if(done>0&&!errs) status.innerHTML='<span style="color:var(--success)">Imported '+done+' automation'+(done!==1?'s':'')+' successfully</span>';
        else if(done>0&&errs) status.innerHTML='<span style="color:var(--warning-accent,#ffc107)">Imported '+done+', '+errs+' failed</span>';
        else status.innerHTML='<span style="color:var(--danger)">Import failed ('+errs+' error'+(errs!==1?'s':'')+')</span>';
        if(done>0) loadAutos();
        input.value='';
        if(label) label.textContent='No file chosen';
        return;
      }
      importAutomationFromJson(items[idx],null).then(()=>{done++;next(idx+1);}).catch(()=>{errs++;next(idx+1);});
    };
    next(0);
  };
  reader.onerror=function(){ status.innerHTML='<span style="color:var(--danger)">Failed to read file</span>'; };
  reader.readAsText(file);
}
function downloadFromGitHub(){
  const urlEl=document.getElementById('github_url');
  const nameEl=document.getElementById('github_name');
  const status=document.getElementById('download_status');
  let url=(urlEl?urlEl.value.trim():'');
  const customName=(nameEl?nameEl.value.trim():'');
  if(!url){ status.innerHTML='<span style="color:var(--danger)">Please enter a GitHub URL</span>'; return; }
  // Convert github.com blob URL to raw.githubusercontent.com
  url=url.replace('https://github.com/','https://raw.githubusercontent.com/').replace('/blob/','/')
         .replace('http://github.com/','https://raw.githubusercontent.com/');
  status.innerHTML='<span style="color:var(--accent)">Fetching from GitHub...</span>';
  fetch(url)
    .then(r=>{ if(!r.ok) throw new Error('HTTP '+r.status); return r.json(); })
    .then(autoJson=>{
      if(customName) autoJson.name=customName;
      return importAutomationFromJson(autoJson,status);
    })
    .then(r=>{
      status.innerHTML='<span style="color:var(--success)">Imported successfully!</span>';
      if(urlEl) urlEl.value='';
      if(nameEl) nameEl.value='';
      loadAutos();
    })
    .catch(e=>{
      if(!status.innerHTML.includes('dc3545'))
        status.innerHTML='<span style="color:var(--danger)">Error: '+e.message+'</span>';
    });
}
function exportAllAutomations(){ 
  const status=document.getElementById('export_status'); 
  const separateFiles=document.getElementById('export_separate').checked; 
  status.innerHTML='<span style="color:var(--accent)">Preparing export...</span>'; 
  if(separateFiles){
    hw.fetchJSON('/api/automations').then(data=>{
      if(data && data.automations && data.automations.length>0){ 
        let downloadCount = 0; 
        const downloadNext = (index) => { 
          if(index >= data.automations.length) { 
            status.innerHTML='<span style="color:var(--success)">' + downloadCount + ' files downloaded separately (import-ready)</span>'; 
            return; 
          } 
          const auto = normalizeAutomation(data.automations[index]);
          const sched = auto.trigger || {};
          const exportAuto={}; 
          exportAuto.name=auto.name; 
          if(auto.condition) exportAuto.condition=auto.condition;
          const rawType = (sched.type || auto.type || '').toLowerCase();
          // Emit new v1 type names in the export; accept legacy names on input.
          let normType = rawType;
          if(normType==='attime') normType='time';
          else if(normType==='afterdelay') normType='manual';
          else if(normType==='onboot') normType='boot';
          exportAuto.trigger={type:normType};
          if(sched.time||auto.time) exportAuto.trigger.time=sched.time||auto.time;
          if(sched.recurrence) exportAuto.trigger.recurrence=sched.recurrence;
          if(sched.days||auto.days) exportAuto.trigger.days=sched.days||auto.days;
          if((sched.weekInterval||auto.weekInterval)&&(sched.weekInterval||auto.weekInterval)>1) exportAuto.trigger.weekInterval=sched.weekInterval||auto.weekInterval;
          if(sched.dayOfMonth||auto.dayOfMonth) exportAuto.trigger.dayOfMonth=sched.dayOfMonth||auto.dayOfMonth;
          if(sched.month||auto.month) exportAuto.trigger.month=sched.month||auto.month;
          if(sched.delayMs||auto.delayMs) exportAuto.trigger.delayMs=sched.delayMs||auto.delayMs;
          if(sched.intervalMs||auto.intervalMs) exportAuto.trigger.intervalMs=sched.intervalMs||auto.intervalMs;
          if(normType==='boot'&&(sched.bootDelayMs||auto.bootDelayMs)) exportAuto.trigger.bootDelayMs=sched.bootDelayMs||auto.bootDelayMs;
          // runAtBoot flag lives at automation top-level in v1 schema.
          if(auto.runAtBoot===true){ exportAuto.runAtBoot=true; if(auto.bootDelayMs) exportAuto.bootDelayMs=auto.bootDelayMs; }
          // Additional (non-primary, non-synthesized-boot) triggers get
          // exported so multi-trigger automations round-trip through export.
          if(Array.isArray(auto.triggers) && auto.triggers.length > 1){
            const extras=[];
            let skippedPrimary=false, skippedBoot=!(auto.runAtBoot===true);
            for(const tr of auto.triggers){
              if(!tr||!tr.type) continue;
              const tt=tr.type.toLowerCase();
              if(!skippedPrimary && tt!=='boot'){ skippedPrimary=true; continue; }
              if(!skippedBoot && tt==='boot'){ skippedBoot=true; continue; }
              const copy=Object.assign({},tr);
              delete copy.nextAt; delete copy.anchor;
              extras.push(copy);
            }
            if(extras.length>0) exportAuto.secondaryTriggers=extras;
          }
          if(auto.commands) exportAuto.commands=auto.commands;
          else if(auto.command) exportAuto.commands=[auto.command]; 
          exportAuto.enabled=auto.enabled===true; 
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
          status.innerHTML='<span style="color:var(--accent)">Downloading ' + (index + 1) + ' of ' + data.automations.length + '...</span>'; 
          setTimeout(() => downloadNext(index + 1), 500); 
        }; 
        downloadNext(0); 
      } else { 
        status.innerHTML='<span style="color:var(--danger)">No automations to export</span>'; 
      } 
    }).catch(e=>{ 
      status.innerHTML='<span style="color:var(--danger)">Export failed: '+e.message+'</span>'; 
    }); 
  } else { 
    const link=document.createElement('a'); 
    link.href='/api/automations/export'; 
    link.download=''; 
    link.style.display='none'; 
    document.body.appendChild(link); 
    link.click(); 
    document.body.removeChild(link); 
    status.innerHTML='<span style="color:var(--success)">Export started - check your downloads folder</span>'; 
  } 
  setTimeout(()=>{ 
    status.innerHTML=''; 
  }, 3000); 
}
function exportSingleAutomation(id){
  hw.fetchJSON('/api/automations').then(data=>{
    if(!data||!data.automations) throw new Error('No automations data');
    const auto=normalizeAutomation(data.automations.find(a=>String(a.id)===String(id)));
    if(!auto) throw new Error('Automation '+id+' not found');
    const sched=auto.trigger||{};
    const exportAuto={};
    exportAuto.name=auto.name;
    if(auto.condition) exportAuto.condition=auto.condition;
    const rawType=(sched.type||auto.type||'').toLowerCase();
    let normType=rawType;
    if(normType==='attime') normType='time';
    else if(normType==='afterdelay') normType='manual';
    else if(normType==='onboot') normType='boot';
    else if(!normType) normType='interval';
    exportAuto.trigger={type:normType};
    if(sched.time||auto.time) exportAuto.trigger.time=sched.time||auto.time;
    if(sched.recurrence) exportAuto.trigger.recurrence=sched.recurrence;
    if(sched.days||auto.days) exportAuto.trigger.days=sched.days||auto.days;
    const exWi=sched.weekInterval||auto.weekInterval;
    if(exWi&&exWi>1) exportAuto.trigger.weekInterval=exWi;
    if(sched.dayOfMonth||auto.dayOfMonth) exportAuto.trigger.dayOfMonth=sched.dayOfMonth||auto.dayOfMonth;
    if(sched.month||auto.month) exportAuto.trigger.month=sched.month||auto.month;
    const delayMs=typeof sched.delayMs!=='undefined'?sched.delayMs:auto.delayMs;
    if(typeof delayMs!=='undefined'&&delayMs!==null) exportAuto.trigger.delayMs=delayMs;
    const intervalMs=typeof sched.intervalMs!=='undefined'?sched.intervalMs:auto.intervalMs;
    if(typeof intervalMs!=='undefined'&&intervalMs!==null) exportAuto.trigger.intervalMs=intervalMs;
    // v1 runAtBoot/bootDelayMs at automation top-level, not inside trigger.
    if(auto.runAtBoot===true){ exportAuto.runAtBoot=true; if(auto.bootDelayMs) exportAuto.bootDelayMs=auto.bootDelayMs; }
    // Additional (non-primary, non-synthesized-boot) triggers so the export
    // preserves the full multi-trigger configuration.
    if(Array.isArray(auto.triggers) && auto.triggers.length > 1){
      const extras=[];
      let skippedPrimary=false, skippedBoot=!(auto.runAtBoot===true);
      for(const tr of auto.triggers){
        if(!tr||!tr.type) continue;
        const tt=tr.type.toLowerCase();
        if(!skippedPrimary && tt!=='boot'){ skippedPrimary=true; continue; }
        if(!skippedBoot && tt==='boot'){ skippedBoot=true; continue; }
        const copy=Object.assign({},tr);
        delete copy.nextAt; delete copy.anchor;
        extras.push(copy);
      }
      if(extras.length>0) exportAuto.secondaryTriggers=extras;
    }
    // bootDelayMs is only meaningful inside a "boot"-type trigger; for other
    // types, it's the top-level runAtBoot companion written above.
    if(normType==='boot'){
      const bootDelay=typeof sched.bootDelayMs!=='undefined'?sched.bootDelayMs:auto.bootDelayMs;
      if(typeof bootDelay!=='undefined'&&bootDelay!==null) exportAuto.trigger.bootDelayMs=bootDelay;
    }
    // Note: nextAt is NOT exported (recomputed on import)
    if(auto.commands) exportAuto.commands=auto.commands;
    else if(auto.command) exportAuto.commands=[auto.command];
    exportAuto.enabled=auto.enabled===true;
    const safeName=(auto.name||'automation_'+id).replace(/[^a-zA-Z0-9_\-]/g,'_');
    const blob=new Blob([JSON.stringify(exportAuto,null,2)],{type:'application/json'});
    const url=URL.createObjectURL(blob);
    const link=document.createElement('a');
    link.href=url; link.download=safeName+'.json'; link.style.display='none';
    document.body.appendChild(link); link.click(); document.body.removeChild(link);
    URL.revokeObjectURL(url);
  }).catch(e=>alert('Export failed: '+e.message));
}
</script>)AUTOPART8", HTTPD_RESP_USE_STRLEN);
}

// Legacy function removed - now using streamAutomationsInner() for efficient streaming
#endif
