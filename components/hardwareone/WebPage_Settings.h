#ifndef WEBPAGE_SETTINGS_H
#define WEBPAGE_SETTINGS_H

// Stream settings page content using raw string literals
static void streamSettingsInner(httpd_req_t* req) {
  // Define togglePane early - before any HTML that uses it
  httpd_resp_send_chunk(req, R"EARLYJS(
<script>
window.togglePane = function(paneId, btnId) {
  var p = document.getElementById(paneId);
  var b = document.getElementById(btnId);
  if (!p || !b) { console.warn('[togglePane] Element not found:', paneId, btnId); return; }
  var isHidden = (p.style.display === 'none' || !p.style.display);
  p.style.display = isHidden ? 'block' : 'none';
  b.textContent = isHidden ? 'Collapse' : 'Expand';
};
window.sendSequential = function(cmds, onDone, onFail) {
  var all = ['beginwrite'].concat(cmds).concat(['savesettings']);
  fetch('/api/cli/batch', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    credentials: 'same-origin',
    body: JSON.stringify({commands: all})
  })
  .then(function(r) {
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  })
  .then(function(j) {
    if (j && j.ok) { if (onDone) onDone(); }
    else { if (onFail) onFail(new Error(j && j.error ? j.error : 'batch failed')); }
  })
  .catch(function(err) { if (onFail) onFail(err); });
};
</script>
)EARLYJS", HTTPD_RESP_USE_STRLEN);

  // Part 1: Header and WiFi section
  httpd_resp_send_chunk(req, R"SETPART1(
<h2>System Settings</h2>
<p>Configure your HardwareOne device settings</p>
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>WiFi Network</div><div style='color:var(--panel-fg);font-size:0.9rem'>Current WiFi network and connection settings.</div></div>
    <button class='btn' id='btn-wifi-toggle' onclick="togglePane('wifi-pane','btn-wifi-toggle')">Expand</button>
  </div>
  <div id='wifi-pane' style='display:none;margin-top:0.75rem'>
  <div style='margin-bottom:1rem'>
    <span style='color:var(--panel-fg)'>SSID: <span style='font-weight:bold;color:#667eea' id='wifi-ssid'>-</span></span>
  </div>
  <div style='display:flex;align-items:center;gap:1rem;margin-bottom:1rem;flex-wrap:wrap'>
    <span style='color:var(--panel-fg)' title='Automatically reconnect to saved WiFi networks after power loss or disconnection'>Auto-Reconnect: <span style='font-weight:bold;color:#667eea' id='wifi-value'>-</span></span>
    <button class='btn' onclick='toggleWifi()' id='wifi-btn' title='Enable/disable automatic WiFi reconnection on boot'>Toggle</button>
  </div>
  <div style='display:flex;align-items:center;gap:1rem;flex-wrap:wrap'>
    <button class='btn' onclick='disconnectWifi()' title='Disconnect from current WiFi network (may lose connection to device)'>Disconnect WiFi</button>
    <button class='btn' onclick='scanNetworks()' title='Scan for available WiFi networks in range'>Scan Networks</button>
  </div>
  <div id='wifi-scan-results' style='margin-top:1rem'></div>
  <div id='wifi-connect-panel' style='display:none;margin-top:0.75rem'>
    <div style='margin-bottom:0.5rem'>Selected SSID: <strong id='sel-ssid'>-</strong></div>
    <input type='password' id='sel-pass' placeholder='WiFi password (leave blank if open)' class='form-input input-medium'>
    <button class='btn' onclick="(function(){ var ssid=(document.getElementById('sel-ssid')||{}).textContent||''; var pass=(document.getElementById('sel-pass')||{}).value||''; if(!ssid){ alert('No SSID selected'); return; } var cmd1='wifiadd '+ssid+' '+pass+' 1 0'; fetch('/api/cli',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},credentials:'same-origin',body:'cmd='+encodeURIComponent(cmd1)}).then(function(r){return r.text();}).then(function(t1){ if(!confirm('Credentials saved for \"'+ssid+'\". Attempt to connect now? You may temporarily lose access while switching.')){ alert('Saved. You can connect later from this page.'); if(typeof refreshSettings==='function') refreshSettings(); return null; } return fetch('/api/cli',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},credentials:'same-origin',body:'cmd='+encodeURIComponent('wificonnect')}); }).then(function(r){ if(!r) return ''; return r.text(); }).then(function(t2){ if(t2){ alert(t2||'Connect attempted'); } if(typeof refreshSettings==='function') refreshSettings(); }).catch(function(e){ alert('Action failed: '+e.message); }); })();">Connect</button>
  </div>
  <div id='wifi-manual-panel' style='display:none;margin-top:0.75rem'>
    <div style='margin-bottom:0.5rem'>Enter hidden network credentials</div>
    <input type='text' id='manual-ssid' placeholder='Hidden SSID' class='form-input input-medium' style='margin-right:6px'>
    <input type='password' id='manual-pass' placeholder='Password (leave blank if open)' class='form-input input-medium' style='margin-right:6px'>
    <button class='btn' onclick="(function(){ var ssid=(document.getElementById('manual-ssid')||{}).value||''; var pass=(document.getElementById('manual-pass')||{}).value||''; if(!ssid){ alert('Enter SSID'); return; } var cmd1='wifiadd '+ssid+' '+pass+' 1 1'; fetch('/api/cli',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},credentials:'same-origin',body:'cmd='+encodeURIComponent(cmd1)}).then(function(r){return r.text();}).then(function(t1){ if(!confirm('Credentials saved for hidden network \"'+ssid+'\". Attempt to connect now? You may temporarily lose access while switching.')){ alert('Saved. You can connect later from this page.'); if(typeof refreshSettings==='function') refreshSettings(); return null; } return fetch('/api/cli',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},credentials:'same-origin',body:'cmd='+encodeURIComponent('wificonnect')}); }).then(function(r){ if(!r) return ''; return r.text(); }).then(function(t2){ if(t2){ alert(t2||'Connect attempted'); } if(typeof refreshSettings==='function') refreshSettings(); }).catch(function(e){ alert('Action failed: '+e.message); }); })();">Connect</button>
  </div>
  </div>
</div>
)SETPART1", HTTPD_RESP_USE_STRLEN);

  // Part 2: System Time, Output Channels, CLI History sections
  httpd_resp_send_chunk(req, R"SETPART2(
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>System Time</div><div style='color:var(--panel-fg);font-size:0.9rem'>Configure timezone offset and NTP server for accurate time synchronization.</div></div>
    <button class='btn' id='btn-time-toggle' onclick="togglePane('time-pane','btn-time-toggle')">Expand</button>
  </div>
  <div id='time-pane' style='display:none;margin-top:0.75rem'>
  <div style='display:flex;align-items:center;gap:1rem;margin-bottom:1rem;flex-wrap:wrap'>
    <span style='color:var(--panel-fg)' title='Current timezone'>Timezone: <span style='font-weight:bold;color:#667eea' id='tz-value'>-</span></span>
    <select id='tz-select' class='form-input input-medium' title='Select timezone'>
      <option value='-720'>UTC-12 (Baker Island)</option>
      <option value='-660'>UTC-11 (Hawaii-Aleutian)</option>
      <option value='-600'>UTC-10 (Hawaii)</option>
      <option value='-540'>UTC-9 (Alaska)</option>
      <option value='-480'>UTC-8 (Pacific)</option>
      <option value='-420'>UTC-7 (Mountain)</option>
      <option value='-360'>UTC-6 (Central)</option>
      <option value='-300'>UTC-5 (Eastern)</option>
      <option value='-240'>UTC-4 (Atlantic)</option>
      <option value='-180'>UTC-3 (Argentina)</option>
      <option value='-120'>UTC-2 (Mid-Atlantic)</option>
      <option value='-60'>UTC-1 (Azores)</option>
      <option value='0'>UTC+0 (London/Dublin)</option>
      <option value='60'>UTC+1 (Berlin/Paris)</option>
      <option value='120'>UTC+2 (Cairo/Athens)</option>
      <option value='180'>UTC+3 (Moscow/Baghdad)</option>
      <option value='240'>UTC+4 (Dubai/Baku)</option>
      <option value='300'>UTC+5 (Karachi/Tashkent)</option>
      <option value='330'>UTC+5:30 (Mumbai/Delhi)</option>
      <option value='360'>UTC+6 (Dhaka/Almaty)</option>
      <option value='420'>UTC+7 (Bangkok/Jakarta)</option>
      <option value='480'>UTC+8 (Beijing/Singapore)</option>
      <option value='540'>UTC+9 (Tokyo/Seoul)</option>
      <option value='570'>UTC+9:30 (Adelaide)</option>
      <option value='600'>UTC+10 (Sydney/Melbourne)</option>
      <option value='660'>UTC+11 (Solomon Islands)</option>
      <option value='720'>UTC+12 (Fiji/Auckland)</option>
    </select>
    <button class='btn' onclick='updateTimezone()' title='Save selected timezone'>Update</button>
  </div>
  <div style='display:flex;align-items:center;gap:1rem;flex-wrap:wrap'>
    <span style='color:var(--panel-fg)' title='NTP server for time synchronization'>NTP Server: <span style='font-weight:bold;color:#667eea' id='ntp-value'>-</span></span>
    <input type='text' id='ntp-input' placeholder='pool.ntp.org' class='form-input' style='width:200px' title='Set NTP server hostname'>
    <button class='btn' onclick='updateNtpServer()' title='Save new NTP server'>Update</button>
  </div>
  </div>
</div>
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Output Channels</div><div style='color:var(--panel-fg);font-size:0.9rem'>Configure persistent settings and see current runtime state. Use 'Temp On/Off' to affect only this session.</div></div>
    <button class='btn' id='btn-output-toggle' onclick="togglePane('output-pane','btn-output-toggle')">Expand</button>
  </div>
  <div id='output-pane' style='display:none;margin-top:0.75rem'>
  <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:1rem'>
    <div style='display:flex;flex-direction:column;gap:0.35rem'>
      <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable sensor data output to serial console (saved to device memory)'>Serial (persisted): <span style='font-weight:bold;color:#667eea' id='serial-value'>-</span></span><button class='btn' onclick="toggleOutput('outSerial','serial')" id='serial-btn' title='Toggle persistent serial output setting'>Toggle</button></div>
      <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Current session serial output status (temporary, resets on reboot)'>Serial (runtime): <span style='font-weight:bold' id='serial-runtime'>-</span></span><button class='btn' id='serial-temp-on' onclick="setOutputRuntime('serial',1)" title='Enable serial output for this session only'>Temp On</button><button class='btn' id='serial-temp-off' onclick="setOutputRuntime('serial',0)" title='Disable serial output for this session only'>Temp Off</button></div>
    </div>
    <div style='display:flex;flex-direction:column;gap:0.35rem'>
      <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable sensor data output to web interface (saved to device memory)'>Web (persisted): <span style='font-weight:bold;color:#667eea' id='web-value'>-</span></span><button class='btn' onclick="toggleOutput('outWeb','web')" id='web-btn' title='Toggle persistent web output setting'>Toggle</button></div>
      <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Current session web output status (temporary, resets on reboot)'>Web (runtime): <span style='font-weight:bold' id='web-runtime'>-</span></span><button class='btn' id='web-temp-on' onclick="setOutputRuntime('web',1)" title='Enable web output for this session only'>Temp On</button><button class='btn' id='web-temp-off' onclick="setOutputRuntime('web',0)" title='Disable web output for this session only'>Temp Off</button></div>
    </div>
    <div style='display:flex;flex-direction:column;gap:0.35rem'>
      <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable sensor data output to display (saved to device memory)'>Display (persisted): <span style='font-weight:bold;color:#667eea' id='display-value'>-</span></span><button class='btn' onclick="toggleOutput('outDisplay','display')" id='display-btn' title='Toggle persistent display output setting'>Toggle</button></div>
      <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Current session display status (temporary, resets on reboot)'>Display (runtime): <span style='font-weight:bold' id='display-runtime'>-</span></span><button class='btn' id='display-temp-on' onclick="setOutputRuntime('display',1)" title='Enable display output for this session only'>Temp On</button><button class='btn' id='display-temp-off' onclick="setOutputRuntime('display',0)" title='Disable display output for this session only'>Temp Off</button></div>
    </div>
)SETPART2", HTTPD_RESP_USE_STRLEN);
#if ENABLE_G2_GLASSES
  httpd_resp_send_chunk(req, R"SP2G(    <div style='display:flex;flex-direction:column;gap:0.35rem'>
      <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable output to G2 smart glasses (saved to device memory)'>G2 Glasses (persisted): <span style='font-weight:bold;color:#667eea' id='g2-value'>-</span></span><button class='btn' onclick="toggleOutput('outG2','g2')" id='g2-btn' title='Toggle persistent G2 glasses output setting'>Toggle</button></div>
      <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Current session G2 glasses status (temporary, resets on reboot)'>G2 (runtime): <span style='font-weight:bold' id='g2-runtime'>-</span></span><button class='btn' id='g2-temp-on' onclick="setOutputRuntime('g2',1)" title='Enable G2 output for this session only'>Temp On</button><button class='btn' id='g2-temp-off' onclick="setOutputRuntime('g2',0)" title='Disable G2 output for this session only'>Temp Off</button></div>
    </div>
)SP2G", HTTPD_RESP_USE_STRLEN);
#endif
  httpd_resp_send_chunk(req, R"SP2B(  </div>
</div>
</div>
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Web CLI History Size</div><div style='color:var(--panel-fg);font-size:0.9rem'>Number of commands to keep in history buffer.</div></div>
    <button class='btn' id='btn-cli-toggle' onclick="togglePane('cli-pane','btn-cli-toggle')">Expand</button>
  </div>
  <div id='cli-pane' style='display:none;margin-top:0.75rem'>
  <div style='display:flex;align-items:center;gap:1rem;flex-wrap:wrap'>
    <span style='color:var(--panel-fg)' title='Number of CLI commands stored in web history buffer'>Current: <span style='font-weight:bold;color:#667eea' id='cli-value'>-</span></span>
    <input type='number' id='cli-input' min='1' max='100' value='10' class='form-input' style='width:80px' title='Set web CLI history buffer size (1-100 commands)'>
    <button class='btn' onclick='updateWebCliHistory()' title='Save new web CLI history buffer size'>Update</button>
    <button class='btn' onclick='clearCliHistory()' title='Clear all stored CLI command history'>Clear History</button>
  </div>
</div>
</div>
)SP2B", HTTPD_RESP_USE_STRLEN);

#if ENABLE_ESPNOW
  // Part 3: ESP-NOW section
  httpd_resp_send_chunk(req, R"SETPART3(
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>ESP-NOW</div><div style='color:var(--panel-fg);font-size:0.9rem'>Interdevice Communication & Mesh Networking</div></div>
    <button class='btn' id='btn-espnow-toggle' onclick="togglePane('espnow-pane','btn-espnow-toggle')">Expand</button>
  </div>
  <div id='espnow-pane' style='display:none;margin-top:0.75rem'>
    <!-- Basic Settings -->
    <div style='display:flex;align-items:center;gap:1rem;margin-bottom:1rem;flex-wrap:wrap'>
      <span style='color:var(--panel-fg)' title='Enable ESP-NOW protocol on boot (requires reboot to take effect)'>Enable ESP-NOW on boot: <span style='font-weight:bold;color:var(--panel-fg)' id='espnow-value'>-</span></span>
      <button class='btn' onclick='toggleEspNow()' id='espnow-btn' title='Enable/disable ESP-NOW protocol'>Toggle</button>
    </div>
    <div style='display:flex;align-items:center;gap:1rem;margin-bottom:1rem;flex-wrap:wrap'>
      <span style='color:var(--panel-fg)'>Mesh Mode: <span style='font-weight:bold;color:var(--panel-fg)' id='espnow-mesh-value'>-</span></span>
      <button class='btn' onclick='toggleEspNowMesh()' id='espnow-mesh-btn'>Toggle</button>
    </div>
    
    <!-- Device Identity -->
    <div style='font-weight:bold;margin:1.5rem 0 0.75rem 0;padding-bottom:0.5rem;border-bottom:1px solid var(--border);color:var(--panel-fg)'>Device Identity</div>
    <div style='margin-bottom:1rem'>
      <label style='display:block;margin-bottom:0.25rem;font-size:0.9rem;color:var(--panel-fg)'>Device Name</label>
      <input type='text' id='espnow-devicename' placeholder='e.g., darkblue' maxlength='20' style='max-width:320px;width:100%'>
      <small style='color:var(--panel-fg);font-size:0.8rem'>Short identifier (1-20 chars, alphanumeric)</small>
    </div>
    
    <!-- Smart Home Metadata -->
    <div style='font-weight:bold;margin:1.5rem 0 0.75rem 0;padding-bottom:0.5rem;border-bottom:1px solid var(--border);color:var(--panel-fg)'>Smart Home Metadata</div>
    <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:0.75rem;margin-bottom:1rem'>
      <div>
        <label style='display:block;margin-bottom:0.25rem;font-size:0.9rem;color:var(--panel-fg)'>Friendly Name</label>
        <input type='text' id='espnow-friendlyname' placeholder='e.g., Living Room Light' maxlength='47' style='width:100%'>
        <small style='color:var(--panel-fg);font-size:0.8rem'>Human-readable name for displays</small>
      </div>
      <div>
        <label style='display:block;margin-bottom:0.25rem;font-size:0.9rem;color:var(--panel-fg)'>Room</label>
        <input type='text' id='espnow-room' placeholder='e.g., Living Room' maxlength='31' style='width:100%'>
      </div>
      <div>
        <label style='display:block;margin-bottom:0.25rem;font-size:0.9rem;color:var(--panel-fg)'>Zone</label>
        <input type='text' id='espnow-zone' placeholder='e.g., Upstairs' maxlength='31' style='width:100%'>
      </div>
      <div>
        <label style='display:block;margin-bottom:0.25rem;font-size:0.9rem;color:var(--panel-fg)'>Tags</label>
        <input type='text' id='espnow-tags' placeholder='e.g., light,dimmable' maxlength='63' style='width:100%'>
      </div>
    </div>
    <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:1rem'>
      <input type='checkbox' id='espnow-stationary' style='width:auto;margin:0'>
      <label for='espnow-stationary' style='color:var(--panel-fg);cursor:pointer'>Stationary Device (fixed location)</label>
    </div>
    
    <!-- Mesh Configuration -->
    <div style='font-weight:bold;margin:1.5rem 0 0.75rem 0;padding-bottom:0.5rem;border-bottom:1px solid var(--border);color:var(--panel-fg)'>Mesh Configuration</div>
    <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem'>
      <div>
        <label style='display:block;margin-bottom:0.25rem;font-size:0.9rem;color:var(--panel-fg)'>Mesh Role</label>
        <select id='espnow-meshrole' style='width:100%'>
          <option value='0'>Worker</option>
          <option value='1'>Master</option>
          <option value='2'>Backup Master</option>
        </select>
      </div>
      <div>
        <label style='display:block;margin-bottom:0.25rem;font-size:0.9rem;color:var(--panel-fg)'>Master MAC</label>
        <input type='text' id='espnow-mastermac' placeholder='XX:XX:XX:XX:XX:XX' maxlength='17' style='width:100%;font-family:monospace'>
      </div>
      <div>
        <label style='display:block;margin-bottom:0.25rem;font-size:0.9rem;color:var(--panel-fg)'>Backup MAC</label>
        <input type='text' id='espnow-backupmac' placeholder='XX:XX:XX:XX:XX:XX' maxlength='17' style='width:100%;font-family:monospace'>
      </div>
    </div>
    
    <!-- Bond Mode Configuration -->
    <div style='font-weight:bold;margin:1.5rem 0 0.75rem 0;padding-bottom:0.5rem;border-bottom:1px solid var(--border);color:var(--panel-fg)'>Bond Mode (Two-Device Pairing)</div>
    <div style='background:rgba(100,149,237,0.1);border-left:3px solid #6495ed;padding:0.5rem 0.75rem;margin-bottom:0.75rem;color:var(--panel-fg);font-size:0.85rem'>
      Bond mode creates a dedicated master/worker pair for specialized applications.
    </div>
    <div style='display:flex;align-items:center;gap:1rem;margin-bottom:1rem;flex-wrap:wrap'>
      <span style='color:var(--panel-fg)'>Bond Mode Enabled: <span style='font-weight:bold;color:var(--panel-fg)' id='bond-enabled-value'>-</span></span>
      <button class='btn' onclick='toggleBondMode()' id='bond-enabled-btn'>Toggle</button>
    </div>
    <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem'>
      <div>
        <label style='display:block;margin-bottom:0.25rem;font-size:0.9rem;color:var(--panel-fg)'>Bond Role</label>
        <select id='bond-role' style='width:100%'>
          <option value='0'>Worker</option>
          <option value='1'>Master</option>
        </select>
      </div>
      <div>
        <label style='display:block;margin-bottom:0.25rem;font-size:0.9rem;color:var(--panel-fg)'>Bonded Peer MAC</label>
        <input type='text' id='bond-peermac' placeholder='XX:XX:XX:XX:XX:XX' maxlength='17' style='width:100%;font-family:monospace'>
      </div>
    </div>
    <div style='font-weight:bold;margin:1rem 0 0.5rem 0;color:var(--panel-fg)'>Auto-Stream Sensors to Bonded Peer</div>
    <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:0.5rem;margin-bottom:1rem'>
      <label style='color:var(--panel-fg);cursor:pointer'><input type='checkbox' id='bond-stream-thermal' style='width:auto;margin-right:0.5rem'>Thermal Camera</label>
      <label style='color:var(--panel-fg);cursor:pointer'><input type='checkbox' id='bond-stream-tof' style='width:auto;margin-right:0.5rem'>Time-of-Flight</label>
      <label style='color:var(--panel-fg);cursor:pointer'><input type='checkbox' id='bond-stream-imu' style='width:auto;margin-right:0.5rem'>IMU</label>
      <label style='color:var(--panel-fg);cursor:pointer'><input type='checkbox' id='bond-stream-gps' style='width:auto;margin-right:0.5rem'>GPS</label>
      <label style='color:var(--panel-fg);cursor:pointer'><input type='checkbox' id='bond-stream-gamepad' style='width:auto;margin-right:0.5rem'>Gamepad</label>
      <label style='color:var(--panel-fg);cursor:pointer'><input type='checkbox' id='bond-stream-fmradio' style='width:auto;margin-right:0.5rem'>FM Radio</label>
      <label style='color:var(--panel-fg);cursor:pointer'><input type='checkbox' id='bond-stream-rtc' style='width:auto;margin-right:0.5rem'>RTC Clock</label>
      <label style='color:var(--panel-fg);cursor:pointer'><input type='checkbox' id='bond-stream-presence' style='width:auto;margin-right:0.5rem'>Presence Sensor</label>
    </div>
    
    <!-- Save Button -->
    <div style='margin-top:1.5rem;padding-top:1rem;border-top:1px solid var(--border)'>
      <button class='btn' onclick='saveEspNowSettings()'>Save ESP-NOW Settings</button>
      <span id='espnow-save-status' style='margin-left:1rem;color:var(--panel-fg)'></span>
    </div>
    
  </div>
</div>
)SETPART3", HTTPD_RESP_USE_STRLEN);
#endif // ENABLE_ESPNOW

#if ENABLE_MQTT || ENABLE_HTTP_SERVER || ENABLE_BLUETOOTH
  // Part 3.5: Network Services section - renders from /api/settings/schema
  httpd_resp_send_chunk(req, R"SETPART3_5(
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Network Services</div><div style='color:var(--panel-fg);font-size:0.9rem'>Configure network service integrations.</div></div>
    <button class='btn' id='btn-network-toggle' onclick="togglePane('network-pane','btn-network-toggle')">Expand</button>
  </div>
  <div id='network-pane' style='display:none;margin-top:1rem;color:var(--panel-fg)'>
    <div id='network-dynamic-container'>
      <div style='text-align:center;padding:2rem;color:var(--panel-fg)'>Loading network settings...</div>
    </div>
  </div>
</div>
<script>
(function(){
  var networkModules = [)SETPART3_5", HTTPD_RESP_USE_STRLEN);
#if ENABLE_MQTT
  httpd_resp_send_chunk(req, "'mqtt'", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_MQTT && (ENABLE_HTTP_SERVER || ENABLE_BLUETOOTH)
  httpd_resp_send_chunk(req, ",", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_HTTP_SERVER
  httpd_resp_send_chunk(req, "'http'", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_HTTP_SERVER && ENABLE_BLUETOOTH
  httpd_resp_send_chunk(req, ",", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_BLUETOOTH
  httpd_resp_send_chunk(req, "'bluetooth'", HTTPD_RESP_USE_STRLEN);
#endif
  httpd_resp_send_chunk(req, R"SETPART3_5B(];
  var networkSections = {'mqtt':'mqtt','http':'http','bluetooth':'bluetooth'};
  var networkLabels = {mqtt:'MQTT Broker',http:'HTTP Server',bluetooth:'Bluetooth'};
  
  function inferType(val) {
    if (typeof val === 'boolean') return 'bool';
    if (typeof val === 'number') return Number.isInteger(val) ? 'int' : 'float';
    return 'string';
  }
  
  function keyToLabel(key) {
    return key.replace(/([A-Z])/g, ' $1').replace(/^./, function(s){return s.toUpperCase();}).replace(/\./g, ' > ');
  }
  
  function renderNetworkInput(e, val, disabled) {
    var id = 'net-' + e.key.replace(/\./g, '-');
    var disAttr = disabled ? ' disabled' : '';
    var grayStyle = disabled ? 'opacity:0.6;cursor:not-allowed;' : '';
    if (e.type === 'bool') {
      return '<label style="' + grayStyle + '"><input type="checkbox" id="' + id + '"' + (val ? ' checked' : '') + disAttr + ' style="margin-right:0.5rem">' + e.label + '</label>';
    } else if (e.type === 'string' && e.secret) {
      var placeholder = val !== undefined && val !== '' ? '(set - leave blank to keep)' : '(not set)';
      return '<label style="' + grayStyle + '">' + e.label + '<br><input type="password" id="' + id + '" placeholder="' + placeholder + '"' + disAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:200px"></label>';
    } else if (e.type === 'int' || e.type === 'float') {
      var step = e.type === 'float' ? '0.01' : '1';
      var minAttr = e.min !== undefined ? ' min="' + e.min + '"' : '';
      var maxAttr = e.max !== undefined ? ' max="' + e.max + '"' : '';
      return '<label style="' + grayStyle + '">' + e.label + '<br><input type="number" id="' + id + '" value="' + (val !== undefined ? val : e.default) + '"' + minAttr + maxAttr + ' step="' + step + '"' + disAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:140px"></label>';
    } else {
      return '<label style="' + grayStyle + '">' + e.label + '<br><input type="text" id="' + id + '" value="' + (val || '') + '"' + disAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:200px"></label>';
    }
  }
  
  function renderNetworkModule(mod, settings) {
    var section = settings[mod.section] || settings[mod.name] || {};
    var entries = mod.entries || [];
    var isDisconnected = mod.connected === false;
    var statusBadge = isDisconnected ? '<span style="background:rgba(255,152,0,0.15);color:#ff9800;border:1px solid rgba(255,152,0,0.3);padding:0.15rem 0.5rem;border-radius:3px;font-size:0.7rem;margin-left:0.5rem;font-weight:500">Disconnected</span>' : '<span style="background:rgba(102,126,234,0.15);color:#667eea;border:1px solid rgba(102,126,234,0.3);padding:0.15rem 0.5rem;border-radius:3px;font-size:0.7rem;margin-left:0.5rem;font-weight:500">Connected</span>';
    
    var html = '<div style="background:var(--panel-bg);border-radius:8px;padding:1rem 1.5rem;margin:0.5rem 0;box-shadow:0 1px 3px rgba(0,0,0,0.1);border:1px solid var(--border)">';
    html += '<div style="display:flex;align-items:center;justify-content:space-between">';
    html += '<div>';
    html += '<span style="font-size:1.1rem;font-weight:bold;color:var(--panel-fg)">' + (networkLabels[mod.name] || mod.description || mod.name) + '</span>' + statusBadge;
    if (mod.description && !networkLabels[mod.name]) {
      html += '<div style="color:var(--panel-fg);font-size:0.85rem;margin-top:0.25rem">' + mod.description + '</div>';
    }
    html += '</div>';
    html += '<button class="btn" id="btn-' + mod.name + '-net-toggle" onclick="togglePane(\'' + mod.name + '-net-pane\',\'btn-' + mod.name + '-net-toggle\')">Expand</button>';
    html += '</div>';
    html += '<div id="' + mod.name + '-net-pane" style="display:none;margin-top:0.75rem">';
    
    if (isDisconnected) {
      html += '<div style="background:rgba(255,152,0,0.08);border-left:3px solid rgba(255,152,0,0.4);padding:0.75rem;margin-bottom:1rem;color:var(--panel-fg);opacity:0.8;font-size:0.85rem">';
      html += 'Service not available. Check WiFi connection and configuration.';
      html += '</div>';
    }
    
    function getValue(key) {
      var parts = key.split('.');
      var v = section;
      for (var i = 0; i < parts.length && v; i++) v = v[parts[i]];
      return v;
    }
    
    html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
    entries.forEach(function(e) { html += renderNetworkInput(e, getValue(e.key), false); });
    html += '</div>';
    html += '<button class="btn" onclick="saveNetworkSettings(\'' + mod.name + '\',\'' + mod.section + '\')">Save ' + (networkLabels[mod.name] || mod.name) + ' Settings</button>';
    html += '</div></div>';
    return html;
  }
  
  Promise.all([
    fetch('/api/settings/schema', {credentials:'include'}).then(function(r){return r.json();}),
    fetch('/api/settings', {credentials:'include'}).then(function(r){return r.json();})
  ]).then(function(results) {
    var schema = results[0];
    var settingsResp = results[1];
    var settings = settingsResp.settings || {};
    var container = document.getElementById('network-dynamic-container');
    if (!container) return;
    
    var relevantModules = (schema.modules || []).filter(function(m) {
      return networkModules.indexOf(m.name) !== -1;
    });
    
    var html = '';
    relevantModules.forEach(function(mod) {
      html += renderNetworkModule(mod, settings);
    });
    
    if (html === '') {
      container.innerHTML = '<div style="text-align:center;padding:2rem;color:var(--panel-fg);font-style:italic">No network services available</div>';
      return;
    }
    
    container.innerHTML = html;
  }).catch(function(err) {
    console.error('[Network Settings] Schema load error:', err);
    var container = document.getElementById('network-dynamic-container');
    if (container) container.innerHTML = '<div style="text-align:center;padding:2rem;color:#dc3545">Failed to load network settings</div>';
  });
  
  window.saveNetworkSettings = function(modName, section) {
    var container = document.getElementById('network-dynamic-container');
    var inputs = container.querySelectorAll('[id^="net-"]:not([disabled])');
    var updates = {};
    inputs.forEach(function(el) {
      var key = el.id.replace('net-', '').replace(/-/g, '.');
      var parentMod = el.closest('[id$="-net-pane"]');
      if (!parentMod || !parentMod.id.startsWith(modName)) return;
      var val;
      if (el.type === 'checkbox') val = el.checked ? 1 : 0;
      else if (el.type === 'number') val = el.step && el.step.indexOf('.') !== -1 ? parseFloat(el.value) : parseInt(el.value);
      else if (el.type === 'password') {
        if (!el.value || el.value.trim() === '') return;
        val = el.value;
      }
      else val = el.value;
      updates[key] = val;
    });
    
    var cmds = [];
    for (var k in updates) {
      cmds.push(k + ' ' + updates[k]);
    }
    
    if (cmds.length === 0) { alert('No settings to save'); return; }

    sendSequential(cmds,
      function() { alert('Settings saved! Some changes may require a restart.'); },
      function(e) { alert('Save failed: ' + (e ? e.message : 'unknown')); }
    );
  };
})();
</script>
)SETPART3_5B", HTTPD_RESP_USE_STRLEN);
#endif // ENABLE_MQTT || ENABLE_HTTP_SERVER || ENABLE_BLUETOOTH

  // Part 4: Dynamic Sensors section - renders from /api/settings/schema
  httpd_resp_send_chunk(req, R"SETPART4(
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Sensors</div><div style='color:var(--panel-fg);font-size:0.9rem'>Configure sensor behavior and visualization settings.</div></div>
    <button class='btn' id='btn-sensors-toggle' onclick="togglePane('sensors-pane','btn-sensors-toggle')">Expand</button>
  </div>
  <div id='sensors-pane' style='display:none;margin-top:1rem;color:var(--panel-fg)'>
    <div id='sensors-dynamic-container'>
      <div style='text-align:center;padding:2rem;color:var(--panel-fg)'>Loading sensor settings...</div>
    </div>
  </div>
</div>
<script>
(function(){
  var sensorModules = ['camera','microphone','thermal','tof','imu','gps','fmradio','apds','rtc','presence','sensorlog','espsr'];
  var i2cModules = ['i2c'];
  var mlSubsections = {camera:'edgeimpulse',microphone:'espsr'};
  var sensorSections = {'camera':'camera','microphone':'microphone','edgeimpulse':'edgeimpulse','espsr':'espsr','thermal_mlx90640':'thermal','tof_vl53l4cx':'tof','imu_bno055':'imu','gps':'gps','fmradio':'fmradio','apds':'apds','rtc':'rtc','presence':'presence','sensorlog':'sensorlog','power':'power','debug':'debug','output':'output'};
  var moduleLabels = {camera:'Camera (OV2640/OV3660)',microphone:'Microphone (PDM)',edgeimpulse:'Machine Learning',espsr:'Voice Recognition (ESP-SR)',thermal:'Thermal Camera (MLX90640)',tof:'Time-of-Flight (VL53L4CX)',imu:'IMU (BNO055)',gps:'GPS (PA1010D)',fmradio:'FM Radio (RDA5807)',gamepad:'Gamepad (Seesaw)',apds:'APDS (APDS9960)',rtc:'RTC Clock (DS3231)',presence:'IR Presence (STHS34PF80)',sensorlog:'Sensor Logging',i2c:'I2C Bus Configuration',power:'Power Management',debug:'Debug Flags',output:'Output Channels'};
  
  function inferType(val) {
    if (typeof val === 'boolean') return 'bool';
    if (typeof val === 'number') return Number.isInteger(val) ? 'int' : 'float';
    return 'string';
  }
  
  function keyToLabel(key) {
    return key.replace(/([A-Z])/g, ' $1').replace(/^./, function(s){return s.toUpperCase();}).replace(/\./g, ' > ');
  }
  
  function renderInput(e, val, disabled) {
    var id = 'dyn-' + e.key.replace(/\./g, '-');
    var disAttr = disabled ? ' disabled' : '';
    var grayStyle = disabled ? 'opacity:0.6;cursor:not-allowed;' : '';
    if (e.type === 'bool') {
      return '<label style="' + grayStyle + '"><input type="checkbox" id="' + id + '"' + (val ? ' checked' : '') + disAttr + ' style="margin-right:0.5rem">' + e.label + '</label>';
    } else if (e.type === 'string' && e.secret) {
      // Secret field: use password input, show placeholder if set, blank = unchanged
      var placeholder = val !== undefined && val !== '' ? '(set - leave blank to keep)' : '(not set)';
      return '<label style="' + grayStyle + '">' + e.label + '<br><input type="password" id="' + id + '" placeholder="' + placeholder + '"' + disAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:200px"></label>';
    } else if (e.type === 'string' && e.options) {
      var opts = e.options.split(',').map(function(o) {
        return '<option value="' + o + '"' + (val === o ? ' selected' : '') + '>' + o.charAt(0).toUpperCase() + o.slice(1) + '</option>';
      }).join('');
      return '<label style="' + grayStyle + '">' + e.label + '<br><select id="' + id + '"' + disAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:160px">' + opts + '</select></label>';
    } else if ((e.type === 'int' || e.type === 'float') && e.options) {
      // Int/float with named options - render as dropdown
      var opts = e.options.split(',').map(function(o) {
        var parts = o.split(':');
        var optVal = parts[0];
        var optLabel = parts.length > 1 ? parts[1] : optVal;
        return '<option value="' + optVal + '"' + (parseInt(val) === parseInt(optVal) ? ' selected' : '') + '>' + optLabel + '</option>';
      }).join('');
      return '<label style="' + grayStyle + '">' + e.label + '<br><select id="' + id + '"' + disAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:200px">' + opts + '</select></label>';
    } else if (e.type === 'int' || e.type === 'float') {
      var step = e.type === 'float' ? '0.01' : '1';
      var minAttr = e.min !== undefined ? ' min="' + e.min + '"' : '';
      var maxAttr = e.max !== undefined ? ' max="' + e.max + '"' : '';
      return '<label style="' + grayStyle + '">' + e.label + '<br><input type="number" id="' + id + '" value="' + (val !== undefined ? val : e.default) + '"' + minAttr + maxAttr + ' step="' + step + '"' + disAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:140px"></label>';
    } else {
      return '<label style="' + grayStyle + '">' + e.label + '<br><input type="text" id="' + id + '" value="' + (val || '') + '"' + disAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:200px"></label>';
    }
  }
  
  function flattenObj(obj, prefix) {
    var result = [];
    for (var k in obj) {
      if (!obj.hasOwnProperty(k)) continue;
      var key = prefix ? prefix + '.' + k : k;
      var val = obj[k];
      if (val !== null && typeof val === 'object' && !Array.isArray(val)) {
        result = result.concat(flattenObj(val, key));
      } else {
        result.push({key: key, value: val, type: inferType(val), label: keyToLabel(key)});
      }
    }
    return result;
  }
  
  function renderModule(mod, settings, isOrphan, allModules, allSettings) {
    var section = settings[mod.section] || settings[mod.name] || {};
    var entries = mod.entries || [];
    var uiEntries = entries.filter(function(e) { return e.key.indexOf('ui.') === 0; });
    var devEntries = entries.filter(function(e) { return e.key.indexOf('device.') === 0; });
    var otherEntries = entries.filter(function(e) { return e.key.indexOf('ui.') !== 0 && e.key.indexOf('device.') !== 0; });
    
    // Check if module is disconnected (from schema API)
    var isDisconnected = mod.connected === false;
    var statusBadge = '';
    if (isOrphan) {
      statusBadge = '<span style="background:#6b7280;color:#fff;padding:0.15rem 0.5rem;border-radius:3px;font-size:0.7rem;margin-left:0.5rem;font-weight:500">Inactive</span>';
    } else if (isDisconnected) {
      statusBadge = '<span style="background:rgba(255,152,0,0.15);color:#ff9800;border:1px solid rgba(255,152,0,0.3);padding:0.15rem 0.5rem;border-radius:3px;font-size:0.7rem;margin-left:0.5rem;font-weight:500">Disconnected</span>';
    } else {
      statusBadge = '<span style="background:rgba(102,126,234,0.15);color:#667eea;border:1px solid rgba(102,126,234,0.3);padding:0.15rem 0.5rem;border-radius:3px;font-size:0.7rem;margin-left:0.5rem;font-weight:500">Connected</span>';
    }
    
    var html = '<div style="background:var(--panel-bg);border-radius:8px;padding:1rem 1.5rem;margin:0.5rem 0;box-shadow:0 1px 3px rgba(0,0,0,0.1);border:1px solid var(--border)">';
    html += '<div style="display:flex;align-items:center;justify-content:space-between">';
    html += '<div>';
    html += '<span style="font-size:1.1rem;font-weight:bold;color:var(--panel-fg)">' + (moduleLabels[mod.name] || mod.description || mod.name) + '</span>' + statusBadge;
    if (mod.description && !moduleLabels[mod.name]) {
      html += '<div style="color:var(--panel-fg);font-size:0.85rem;margin-top:0.25rem">' + mod.description + '</div>';
    }
    html += '</div>';
    html += '<button class="btn" id="btn-' + mod.name + '-toggle" onclick="togglePane(\'' + mod.name + '-pane\',\'btn-' + mod.name + '-toggle\')">Expand</button>';
    html += '</div>';
    html += '<div id="' + mod.name + '-pane" style="display:none;margin-top:0.75rem">';
    
    if (isOrphan) {
      html += '<div style="background:var(--crumb-bg);border-left:3px solid var(--border);padding:0.75rem;margin-bottom:1rem;color:var(--panel-fg);font-size:0.85rem">';
      html += 'Module not included in current build. Settings are preserved but read-only.';
      html += '</div>';
    } else if (isDisconnected) {
      html += '<div style="background:rgba(255,152,0,0.08);border-left:3px solid rgba(255,152,0,0.4);padding:0.75rem;margin-bottom:1rem;color:var(--panel-fg);opacity:0.8;font-size:0.85rem">';
      html += 'Module not connected, settings can still be changed.';
      html += '</div>';
    }
    
    function getValue(key) {
      var parts = key.split('.');
      var v = section;
      for (var i = 0; i < parts.length && v; i++) v = v[parts[i]];
      return v;
    }
    
    if (devEntries.length > 0) {
      html += '<div style="font-weight:bold;margin-bottom:0.5rem;color:var(--panel-fg);border-bottom:1px solid var(--border);padding-bottom:0.25rem">Device Settings</div>';
      html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
      devEntries.forEach(function(e) { html += renderInput(e, getValue(e.key), isOrphan); });
      html += '</div>';
    }
    if (uiEntries.length > 0) {
      html += '<div style="font-weight:bold;margin-bottom:0.5rem;color:var(--panel-fg);border-bottom:1px solid var(--border);padding-bottom:0.25rem">Client UI Settings</div>';
      html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
      uiEntries.forEach(function(e) { html += renderInput(e, getValue(e.key), isOrphan); });
      html += '</div>';
    }
    if (otherEntries.length > 0) {
      // For camera module, separate ESP-NOW related settings
      var espnowKeys = ['cameraSendAfterCapture', 'cameraTargetDevice'];
      var regularEntries = otherEntries.filter(function(e) { return espnowKeys.indexOf(e.key) === -1; });
      var espnowEntries = otherEntries.filter(function(e) { return espnowKeys.indexOf(e.key) !== -1; });
      
      if (regularEntries.length > 0) {
        html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
        regularEntries.forEach(function(e) { html += renderInput(e, getValue(e.key), isOrphan); });
        html += '</div>';
      }
      
      if (espnowEntries.length > 0 && mod.name === 'camera') {
        html += '<div style="font-weight:bold;margin:1rem 0 0.5rem 0;color:var(--panel-fg);border-bottom:1px solid var(--border);padding-bottom:0.25rem">ESP-NOW Integration</div>';
        html += '<div style="background:rgba(100,149,237,0.1);border-left:3px solid #6495ed;padding:0.5rem 0.75rem;margin-bottom:0.75rem;color:var(--panel-fg);font-size:0.85rem">';
        html += 'Send captured images to another device via ESP-NOW mesh network.';
        html += '</div>';
        html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
        espnowEntries.forEach(function(e) { html += renderInput(e, getValue(e.key), isOrphan); });
        html += '</div>';
      } else if (espnowEntries.length > 0) {
        html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
        espnowEntries.forEach(function(e) { html += renderInput(e, getValue(e.key), isOrphan); });
        html += '</div>';
      }
    }
    if (!isOrphan) {
      html += '<button class="btn" onclick="saveDynamicSettings(\'' + mod.name + '\',\'' + mod.section + '\')">Save ' + (moduleLabels[mod.name] || mod.name) + ' Settings</button>';
    }
    
    // Render ML subsection if this module has one
    var mlModName = mlSubsections[mod.name];
    if (mlModName && allModules) {
      var mlMod = allModules.find(function(m) { return m.name === mlModName; });
      if (mlMod) {
        var mlSection = allSettings[mlMod.section] || {};
        var mlEntries = mlMod.entries || [];
        html += '<div style="margin-top:1rem;padding-top:1rem;border-top:1px solid var(--border)">';
        html += '<div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:0.5rem">';
        html += '<span style="font-weight:bold;color:var(--panel-fg)">Machine Learning</span>';
        html += '<button class="btn" id="btn-' + mlModName + '-toggle" onclick="togglePane(\'' + mlModName + '-pane\',\'btn-' + mlModName + '-toggle\')">Expand</button>';
        html += '</div>';
        html += '<div id="' + mlModName + '-pane" style="display:none">';
        html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
        mlEntries.forEach(function(e) {
          var parts = e.key.split('.');
          var v = mlSection;
          for (var i = 0; i < parts.length && v; i++) v = v[parts[i]];
          html += renderInput(e, v, isOrphan);
        });
        html += '</div>';
        if (!isOrphan) {
          html += '<button class="btn" onclick="saveDynamicSettings(\'' + mlModName + '\',\'' + mlMod.section + '\')">Save ML Settings</button>';
        }
        html += '</div></div>';
      }
    }
    
    html += '</div></div>';
    return html;
  }
  
  Promise.all([
    fetch('/api/settings/schema', {credentials:'include'}).then(function(r){return r.json();}),
    fetch('/api/settings', {credentials:'include'}).then(function(r){return r.json();})
  ]).then(function(results) {
    var schema = results[0];
    var settingsResp = results[1];
    var settings = settingsResp.settings || {};
    var container = document.getElementById('sensors-dynamic-container');
    if (!container) return;
    
    var schemaModuleNames = (schema.modules || []).map(function(m) { return m.name; });
    var schemaSections = (schema.modules || []).map(function(m) { return m.section; });
    
    var allKnownModules = sensorModules.concat(i2cModules);
    var relevantModules = (schema.modules || []).filter(function(m) {
      return allKnownModules.indexOf(m.name) !== -1;
    });
    
    // Find orphaned sensor settings in JSON that aren't in schema
    var orphanModules = [];
    for (var sectionKey in settings) {
      if (!settings.hasOwnProperty(sectionKey)) continue;
      var modName = sensorSections[sectionKey];
      if (!modName) continue;
      // Check if this section has a registered module
      if (schemaSections.indexOf(sectionKey) !== -1) continue;
      // This is an orphan - settings exist but module not compiled
      var sectionData = settings[sectionKey];
      var entries = flattenObj(sectionData, '');
      orphanModules.push({
        name: modName,
        section: sectionKey,
        entries: entries
      });
    }
    
    var html = '';
    var i2cHtml = '';
    
    // Render active (compiled) modules first
    var allMods = schema.modules || [];
    relevantModules.forEach(function(mod) {
      if (i2cModules.indexOf(mod.name) !== -1) {
        var sec = settings[mod.section] || settings[mod.name] || {};
        var ents = mod.entries || [];
        i2cHtml += '<div id="i2c-pane"><div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
        ents.forEach(function(e) {
          var parts = e.key.split('.'), v = sec;
          for (var pi = 0; pi < parts.length && v; pi++) v = v[parts[pi]];
          i2cHtml += renderInput(e, v, false);
        });
        i2cHtml += '</div><button class="btn" onclick="saveDynamicSettings(\'' + mod.name + '\',\'' + mod.section + '\')">Save I2C Bus Configuration Settings</button></div>';
      } else {
        html += renderModule(mod, settings, false, allMods, settings);
      }
    });
    
    // Render orphan modules (not compiled but settings exist)
    orphanModules.forEach(function(mod) {
      if (i2cModules.indexOf(mod.name) !== -1) {
        var ents = mod.entries || [];
        i2cHtml += '<div id="i2c-pane"><div style="background:var(--crumb-bg);border-left:3px solid var(--border);padding:0.75rem;margin-bottom:1rem;color:var(--panel-fg);font-size:0.85rem">Module not included in current build. Settings are preserved but read-only.</div><div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
        ents.forEach(function(e) { i2cHtml += renderInput(e, e.value, true); });
        i2cHtml += '</div></div>';
      } else {
        html += renderModule(mod, settings, true, allMods, settings);
      }
    });
    
    // Populate i2c container
    var i2cCont = document.getElementById('i2c-bus-dynamic-container');
    if (i2cCont) {
      i2cCont.innerHTML = i2cHtml || '<div style="text-align:center;padding:2rem;color:var(--panel-fg);font-style:italic">I2C settings not available</div>';
    }
    
    if (html === '') {
      container.innerHTML = '<div style="text-align:center;padding:2rem;color:var(--panel-fg);font-style:italic">No sensor settings available</div>';
      return;
    }
    
    container.innerHTML = html;
  }).catch(function(err) {
    console.error('[Settings] Schema load error:', err);
    var container = document.getElementById('sensors-dynamic-container');
    if (container) container.innerHTML = '<div style="text-align:center;padding:2rem;color:#dc3545">Failed to load sensor settings</div>';
  });
  
  window.saveDynamicSettings = function(modName, section) {
    var pane = document.getElementById(modName + '-pane');
    if (!pane) { alert('Settings pane not found for: ' + modName); return; }
    var inputs = pane.querySelectorAll('[id^="dyn-"]:not([disabled])');
    var updates = {};
    inputs.forEach(function(el) {
      var key = el.id.replace('dyn-', '').replace(/-/g, '.');
      var val;
      if (el.type === 'checkbox') val = el.checked ? 1 : 0;
      else if (el.type === 'number') val = el.step && el.step.indexOf('.') !== -1 ? parseFloat(el.value) : parseInt(el.value);
      else if (el.type === 'password') {
        // Secret field: skip if blank (blank = unchanged)
        if (!el.value || el.value.trim() === '') return;
        val = el.value;
      }
      else val = el.value;
      updates[key] = val;
    });
    
    // For camera: if enabling auto-capture and folder is empty, set default and update UI
    if (modName === 'camera' && updates['cameraAutoCapture'] === 1) {
      var folderInput = document.getElementById('dyn-cameraCaptureFolder');
      if (folderInput && !folderInput.value.trim()) {
        folderInput.value = '/photos';
        updates['cameraCaptureFolder'] = '/photos';
      }
    }
    
    var cmds = [];
    for (var k in updates) {
      cmds.push(k + ' ' + updates[k]);
    }
    
    if (cmds.length === 0) { alert('No settings to save'); return; }

    sendSequential(cmds,
      function() { alert('Settings saved! Some changes may require a reboot.'); },
      function(e) { alert('Save failed: ' + (e ? e.message : 'unknown')); }
    );
  };
})();
</script>
)SETPART4", HTTPD_RESP_USE_STRLEN);

  // I2C Bus Configuration section (standalone, not under Sensors)
  httpd_resp_send_chunk(req, R"I2CPART(
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>I2C Bus Configuration</div><div style='color:var(--panel-fg);font-size:0.9rem'>Configure I2C bus pins, clock speeds, and enable/disable settings.</div></div>
    <button class='btn' id='btn-i2cbus-toggle' onclick="togglePane('i2cbus-pane','btn-i2cbus-toggle')">Expand</button>
  </div>
  <div id='i2cbus-pane' style='display:none;margin-top:0.75rem'>
    <div id='i2c-bus-dynamic-container'>
      <div style='text-align:center;padding:2rem;color:var(--panel-fg)'>Loading I2C settings...</div>
    </div>
  </div>
</div>
)I2CPART", HTTPD_RESP_USE_STRLEN);

  // Part 6: Hardware Settings section
  httpd_resp_send_chunk(req, R"SETPART6(
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Hardware Settings</div><div style='color:var(--panel-fg);font-size:0.9rem'>Configure onboard hardware behavior (LED, OLED, Gamepad)</div></div>
    <button class='btn' id='btn-hardware-toggle' onclick="togglePane('hardware-pane','btn-hardware-toggle')">Expand</button>
  </div>
  <div id='hardware-pane' style='display:none;margin-top:0.75rem'>
    <!-- LED Configuration Subsection -->
    <div style='background:var(--panel-bg);border:1px solid var(--border);border-radius:6px;padding:1rem;margin-bottom:1rem'>
      <div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:0.75rem'>
        <div style='font-weight:bold;color:var(--panel-fg)'>LED Configuration</div>
        <button class='btn' id='btn-led-toggle' onclick="togglePane('led-pane','btn-led-toggle')" style='font-size:0.85rem;padding:0.25rem 0.75rem'>Expand</button>
      </div>
      <div id='led-pane' style='display:none'>
        <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:1rem;margin-bottom:1rem'>
          <label title="Global LED brightness (0-100%)">LED Brightness (%)<br><input type='number' id='ledBrightness' min='0' max='100' step='5' value='100' style='padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:120px' title='LED brightness percentage'></label>
          <label><input type='checkbox' id='ledStartupEnabled' style='margin-right:0.5rem'>Enable Startup Effect</label>
        </div>
        <div style='padding:1rem;background:var(--crumb-bg);border:1px solid var(--border);border-radius:4px;margin-bottom:1rem'>
          <div style='font-weight:bold;color:var(--panel-fg);margin-bottom:0.5rem'>Startup Effect Configuration</div>
          <div style='color:var(--panel-fg);font-size:0.9rem;margin-bottom:0.75rem'>LED effect to run when device finishes booting.</div>
          <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:1rem;align-items:end'>
            <label title="Effect type">Effect Type<br><select id='ledStartupEffect' style='padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:140px'><option value='none'>None</option><option value='rainbow'>Rainbow</option><option value='pulse'>Pulse</option><option value='fade'>Fade</option><option value='blink'>Blink</option><option value='strobe'>Strobe</option></select></label>
            <label title="Primary color">Primary Color<br><select id='ledStartupColor' style='padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:140px'><option value='red'>Red</option><option value='green'>Green</option><option value='blue'>Blue</option><option value='cyan'>Cyan</option><option value='magenta'>Magenta</option><option value='yellow'>Yellow</option><option value='white'>White</option><option value='orange'>Orange</option><option value='purple'>Purple</option></select></label>
            <label title="Secondary color (for fade effect)">Secondary Color<br><select id='ledStartupColor2' style='padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:140px'><option value='red'>Red</option><option value='green'>Green</option><option value='blue'>Blue</option><option value='cyan'>Cyan</option><option value='magenta'>Magenta</option><option value='yellow'>Yellow</option><option value='white'>White</option><option value='orange'>Orange</option><option value='purple'>Purple</option></select></label>
            <label title="Effect duration in milliseconds">Duration (ms)<br><input type='number' id='ledStartupDuration' min='100' max='10000' step='100' value='1000' style='padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:140px' title='Effect duration'></label>
          </div>
        </div>
        <button class='btn' onclick="saveLEDSettings()">Save LED Settings</button>
      </div>
    </div>
    <!-- OLED Display Configuration Subsection -->
    <div style='background:var(--panel-bg);border:1px solid var(--border);border-radius:6px;padding:1rem;margin-bottom:1rem'>
      <div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:0.75rem'>
        <div style='font-weight:bold;color:var(--panel-fg)'>OLED Display Configuration</div>
        <button class='btn' id='btn-oled-toggle' onclick="togglePane('oled-pane','btn-oled-toggle')" style='font-size:0.85rem;padding:0.25rem 0.75rem'>Expand</button>
      </div>
      <div id='oled-pane' style='display:none'>
        <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:1rem;margin-bottom:1rem'>
          <label><input type='checkbox' id='oledEnabled' style='margin-right:0.5rem'>Enable OLED Display</label>
        </div>
        <div style='padding:1rem;background:var(--crumb-bg);border:1px solid var(--border);border-radius:4px;margin-bottom:1rem'>
          <div style='font-weight:bold;color:var(--panel-fg);margin-bottom:0.5rem'>Display Modes & Timing</div>
          <div style='color:var(--panel-fg);font-size:0.9rem;margin-bottom:0.75rem'>Configure what the OLED shows during boot and after.</div>
          <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:1rem;align-items:end'>
            <label title="Mode shown during boot">Boot Mode<br><select id='oledBootMode' style='padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:140px'><option value='logo'>Logo</option><option value='status'>Status</option><option value='sensors'>Sensors</option><option value='thermal'>Thermal</option><option value='network'>Network</option><option value='off'>Off</option></select></label>
            <label title="Mode after boot completes">Default Mode<br><select id='oledDefaultMode' style='padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:140px'><option value='logo'>Logo</option><option value='status'>Status</option><option value='sensors'>Sensors</option><option value='thermal'>Thermal</option><option value='network'>Network</option><option value='off'>Off</option></select></label>
            <label title="Duration to show boot mode (ms)">Boot Duration (ms)<br><input type='number' id='oledBootDuration' min='0' max='60000' step='100' value='2000' style='padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:140px' title='Boot mode duration'></label>
            <label title="Display refresh interval (ms)">Update Interval (ms)<br><input type='number' id='oledUpdateInterval' min='10' max='1000' step='10' value='200' style='padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:140px' title='Refresh rate'></label>
          </div>
        </div>
        <div style='padding:1rem;background:var(--crumb-bg);border:1px solid var(--border);border-radius:4px;margin-bottom:1rem'>
          <div style='font-weight:bold;color:var(--panel-fg);margin-bottom:0.5rem'>Display Settings</div>
          <div style='color:var(--panel-fg);font-size:0.9rem;margin-bottom:0.75rem'>Brightness and thermal visualization settings.</div>
          <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:1rem;align-items:end'>
            <label title="Display brightness (0-255)">Brightness<br><input type='number' id='oledBrightness' min='0' max='255' step='5' value='255' style='padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:120px' title='Brightness level'></label>
            <label title="Thermal image scale factor">Thermal Scale<br><input type='number' id='oledThermalScale' min='0.5' max='5.0' step='0.1' value='2.5' style='padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:140px' title='Scale factor'></label>
            <label title="Thermal color mode">Thermal Color<br><select id='oledThermalColorMode' style='padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:140px'><option value='3level'>3-Level</option><option value='grayscale'>Grayscale</option></select></label>
          </div>
        </div>
        <button class='btn' onclick="saveOLEDSettings()">Save OLED Settings</button>
      </div>
    </div>
    <!-- Gamepad Configuration Subsection -->
    <div style='background:var(--panel-bg);border:1px solid var(--border);border-radius:6px;padding:1rem'>
      <div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:0.75rem'>
        <div style='font-weight:bold;color:var(--panel-fg)'>Gamepad Configuration</div>
        <button class='btn' id='btn-gamepad-toggle' onclick="togglePane('gamepad-pane','btn-gamepad-toggle')" style='font-size:0.85rem;padding:0.25rem 0.75rem'>Expand</button>
      </div>
      <div id='gamepad-pane' style='display:none'>
        <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:1rem;margin-bottom:1rem'>
          <label><input type='checkbox' id='gamepadAutoStart' style='margin-right:0.5rem'>Auto-Start Gamepad After Boot</label>
        </div>
        <button class='btn' onclick="saveGamepadSettings()">Save Gamepad Settings</button>
      </div>
    </div>
  </div>
</div>
)SETPART6", HTTPD_RESP_USE_STRLEN);

  // Part 7: Debug Controls section (dynamic from schema)
  httpd_resp_send_chunk(req, R"SETPART7(
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Debug Controls</div>
    <div style='color:var(--panel-fg);font-size:0.85rem;opacity:0.55'>Click a category name to toggle all flags in that group</div></div>
    <button class='btn' id='btn-debug-toggle' onclick="togglePane('debug-pane','btn-debug-toggle')">Expand</button>
  </div>
  <div id='debug-pane' style='display:none;margin-top:0.75rem'>
  <style>
  .dbg-sw{position:relative;display:inline-block;width:34px;height:18px;flex-shrink:0}
  .dbg-sw input{opacity:0;width:0;height:0;position:absolute}
  .dbg-sw .sl{position:absolute;cursor:pointer;inset:0;background:var(--border);border-radius:9px;transition:background .2s}
  .dbg-sw input:checked+.sl{background:#667eea}
  .dbg-sw .sl::before{content:'';position:absolute;height:12px;width:12px;left:3px;bottom:3px;background:#fff;border-radius:50%;transition:transform .15s}
  .dbg-sw input:checked+.sl::before{transform:translateX(16px)}
  .dbg-card{background:var(--panel-bg);border:1px solid var(--border);border-radius:8px;padding:0.6rem 0.75rem;transition:border-color .25s}
  .dbg-card.on{border-color:#667eea}
  .dbg-card-hdr{display:flex;align-items:center;gap:6px;cursor:pointer;-webkit-user-select:none;user-select:none;padding-bottom:6px;border-bottom:1px solid var(--border)}
  .dbg-card-hdr:hover span:last-child{color:#667eea}
  .dbg-dot{width:7px;height:7px;border-radius:50%;background:#555;transition:background .25s;flex-shrink:0}
  .dbg-card.on .dbg-dot{background:#667eea}
  .dbg-card-hdr span:last-child{font-weight:600;font-size:0.92rem;color:var(--panel-fg);transition:color .15s}
  .dbg-row{display:flex;align-items:center;justify-content:space-between;padding:3px 0}
  .dbg-row .lbl{font-size:0.84rem;color:var(--panel-fg);opacity:0.85}
  .dbg-pill{display:flex;align-items:center;justify-content:space-between;border:1px solid var(--border);border-radius:6px;padding:4px 10px;transition:border-color .2s}
  .dbg-pill.on{border-color:#667eea}
  .dbg-pill .lbl{font-size:0.84rem;color:var(--panel-fg);margin-right:10px;white-space:nowrap}
  </style>
  <div id='debug-dynamic-container'><div style='text-align:center;padding:2rem;color:var(--panel-fg);opacity:0.4'>Loading...</div></div>
  </div>
</div>
<script>
(function(){
  var GL={authentication:'Authentication',http:'HTTP',sse:'SSE',wifi:'WiFi',storage:'Storage','esp-now':'ESP-NOW',system:'System',users:'Users',cli:'CLI',commands:'Commands',performance:'Performance',automations:'Automations',sensors:'Sensors',thermal:'Thermal',imu:'IMU',gamepad:'Gamepad',tof:'ToF',apds:'APDS'};
  function sw(cmd,grp,on,isAll){return '<label class="dbg-sw"><input type="checkbox" class="dbg-cb" data-cmd="'+cmd+'"'+(grp?' data-group="'+grp+'"':'')+(isAll?' data-all="1"':'')+(on?' checked':'')+'><span class="sl"></span></label>';}
  Promise.all([
    fetch('/api/settings/schema',{credentials:'include'}).then(function(r){return r.json();}),
    fetch('/api/settings',{credentials:'include'}).then(function(r){return r.json();})
  ]).then(function(res){
    var schema=res[0],settings=(res[1].settings||{}),c=document.getElementById('debug-dynamic-container');
    if(!c)return;
    var dm=null;(schema.modules||[]).forEach(function(m){if(m.name==='debug')dm=m;});
    if(!dm){c.innerHTML='<div>Debug module not found</div>';return;}
    var entries=dm.entries||[],dbg=settings.debug||{},groups={},gOrder=[],standalone=[];
    entries.forEach(function(e){
      if(e.group){if(!groups[e.group]){groups[e.group]=[];gOrder.push(e.group);}groups[e.group].push(e);}
      else standalone.push(e);
    });
    var h='<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:0.6rem">';
    gOrder.forEach(function(gn){
      var ge=groups[gn],gl=GL[gn]||gn,gid='dbg-'+gn.replace(/[^a-z0-9]/g,'');
      var pe=null,ce=[];
      ge.forEach(function(e){if(e.key==='enabled')pe=e;else ce.push(e);});
      var gd=dbg[gn]||{},anyOn=false;
      ge.forEach(function(e){if(gd[e.key])anyOn=true;});
      h+='<div class="dbg-card'+(anyOn?' on':'')+'" id="'+gid+'">';
      h+='<div class="dbg-card-hdr" onclick="dbgToggleAll(\''+gn+'\')"><span class="dbg-dot"></span><span>'+gl+'</span></div>';
      ge.forEach(function(e){
        var v=!!(e.key==='enabled'?gd.enabled:gd[e.key]);
        var lbl=e.key==='enabled'?'All':'&ensp;'+e.label;
        h+='<div class="dbg-row"><span class="lbl">'+lbl+'</span>'+sw(e.cmdKey||e.key,gn,v,e.key==='enabled')+'</div>';
      });
      h+='</div>';
    });
    h+='</div>';
    var boolSA=standalone.filter(function(e){return e.type==='bool';}),cfgSA=standalone.filter(function(e){return e.type!=='bool';});
    if(boolSA.length>0){
      h+='<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(145px,1fr));gap:0.45rem;margin-top:0.7rem">';
      boolSA.forEach(function(e){
        var v=!!dbg[e.key];
        h+='<div class="dbg-pill'+(v?' on':'')+'"><span class="lbl">'+e.label+'</span>'+sw(e.cmdKey||e.key,'',v)+'</div>';
      });
      h+='</div>';
    }
    if(cfgSA.length>0){
      h+='<div style="margin-top:0.7rem;display:flex;flex-wrap:wrap;gap:0.6rem 1.5rem;align-items:center">';
      cfgSA.forEach(function(e){
        var v=dbg[e.key];if(v===undefined)v=e['default'];var cmd=e.cmdKey||e.key;
        if(e.key==='logLevel'){
          h+='<div style="display:flex;align-items:center;gap:0.5rem"><span style="font-size:0.88rem;color:var(--panel-fg)">'+e.label+'</span>';
          h+='<select class="dbg-input form-input" data-cmd="'+cmd+'" style="width:130px">';
          h+='<option value="0"'+(v===0?' selected':'')+'>Error</option>';
          h+='<option value="1"'+(v===1?' selected':'')+'>Warn</option>';
          h+='<option value="2"'+(v===2?' selected':'')+'>Info</option>';
          h+='<option value="3"'+(v===3?' selected':'')+'>Debug</option></select></div>';
        } else {
          h+='<div style="display:flex;align-items:center;gap:0.5rem"><span style="font-size:0.88rem;color:var(--panel-fg)">'+e.label+'</span>';
          var mi=e.min!==undefined?' min="'+e.min+'"':'',ma=e.max!==undefined?' max="'+e.max+'"':'';
          h+='<input type="number" class="dbg-input form-input" data-cmd="'+cmd+'" value="'+v+'"'+mi+ma+' style="width:80px"></div>';
        }
      });
      h+='</div>';
    }
    h+='<div style="margin-top:0.8rem;display:flex;align-items:center;gap:0.75rem">';
    h+='<button class="btn" onclick="saveDebugSettings()">Save</button>';
    h+='<span id="dbg-save-msg" style="font-size:0.82rem;color:#667eea;opacity:0;transition:opacity .3s"></span></div>';
    c.innerHTML=h;
    c.addEventListener('change',function(ev){
      var t=ev.target;if(!t.classList.contains('dbg-cb'))return;
      var g=t.getAttribute('data-group');
      if(g){var card=document.getElementById('dbg-'+g.replace(/[^a-z0-9]/g,''));if(card){if(t.hasAttribute('data-all')){card.querySelectorAll('.dbg-cb').forEach(function(x){x.checked=t.checked;});}var on=false;card.querySelectorAll('.dbg-cb').forEach(function(x){if(x.checked)on=true;});card.classList.toggle('on',on);}}
      else{var p=t.closest('.dbg-pill');if(p)p.classList.toggle('on',t.checked);}
    });
  }).catch(function(err){
    var c=document.getElementById('debug-dynamic-container');
    if(c)c.innerHTML='<div style="color:#f55">Error loading debug settings: '+err.message+'</div>';
  });
  window.dbgToggleAll=function(gn){
    var card=document.getElementById('dbg-'+gn.replace(/[^a-z0-9]/g,''));if(!card)return;
    var cbs=card.querySelectorAll('.dbg-cb'),any=false;
    cbs.forEach(function(x){if(x.checked)any=true;});
    cbs.forEach(function(x){x.checked=!any;});
    card.classList.toggle('on',!any);
  };
  window.saveDebugSettings=function(){
    var cmds=[];
    document.querySelectorAll('.dbg-cb').forEach(function(cb){
      var cmd=cb.getAttribute('data-cmd');if(cmd)cmds.push(cmd+' '+(cb.checked?1:0));
    });
    document.querySelectorAll('.dbg-input').forEach(function(el){
      var cmd=el.getAttribute('data-cmd');if(cmd)cmds.push(cmd+' '+el.value);
    });
    if(!cmds.length)return;
    var msg=document.getElementById('dbg-save-msg');
    if(msg){msg.textContent='Saving...';msg.style.opacity='1';}
    sendSequential(cmds,
      function(){if(msg){msg.textContent='Saved';setTimeout(function(){msg.style.opacity='0';},1500);}try{refreshSettings();}catch(_){}},
      function(){if(msg){msg.textContent='Error saving';msg.style.color='#f55';}}
    );
  };
})();
</script>
)SETPART7", HTTPD_RESP_USE_STRLEN);

  // Part 8: Admin section and page controls
  httpd_resp_send_chunk(req, R"SETPART8(
<div id='admin-section' style='display:none;background:var(--panel-bg);border-radius:8px;padding:1.0rem 1.5rem;margin:1rem 0;color:var(--panel-fg)'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Admin Controls</div>
    <button class='btn' id='btn-admin-toggle' onclick="togglePane('admin-pane','btn-admin-toggle')">Expand</button>
  </div>
  <div id='admin-pane' style='display:none;margin-top:0.75rem'>
  <div style='display:grid;grid-template-columns:1fr;gap:1rem'>
    <div style='background:var(--crumb-bg);border:1px solid var(--border);border-radius:8px;padding:1rem'>
      <div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:0.5rem'>
        <div style='font-weight:bold;color:var(--panel-fg)'>User Management</div>
        <button class='btn' id='btn-users-toggle' onclick="togglePane('users-pane','btn-users-toggle')">Expand</button>
      </div>
      <div style='color:var(--panel-fg);margin-bottom:0.75rem;font-size:0.9rem'>Manage existing users and their roles.</div>
      <div id='users-pane' style='display:none;margin-top:0.75rem'>
        <div id='users-list' style='min-height:24px;color:var(--panel-fg);margin-bottom:0.75rem'>Loading...</div>
        <button class='btn' onclick='refreshUsers()' title='Reload list of users'>Refresh Users</button>
      </div>
    </div>
    <div style='background:var(--crumb-bg);border:1px solid var(--border);border-radius:8px;padding:1rem'>
      <div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:0.5rem'>
        <div style='font-weight:bold;color:var(--panel-fg)'>Security</div>
        <button class='btn' id='btn-security-toggle' onclick="togglePane('security-pane','btn-security-toggle')">Expand</button>
      </div>
      <div style='color:var(--panel-fg);margin-bottom:0.75rem;font-size:0.9rem'>Authentication and access control settings.</div>
      <div id='security-pane' style='display:none;margin-top:0.75rem'>
        <div style='display:grid;gap:0.75rem'>
          <div style='display:flex;align-items:center;gap:0.75rem;flex-wrap:wrap'>
            <span style='color:var(--panel-fg);min-width:160px'>Local Display Auth: <span style='font-weight:bold' id='display-auth-value'>-</span></span>
            <button class='btn' id='display-auth-btn' onclick='toggleDisplayAuth()' title='Require login before accessing OLED display menus'>Toggle</button>
          </div>
          <div id='ble-auth-row' style='display:none;align-items:center;gap:0.75rem;flex-wrap:wrap'>
            <span style='color:var(--panel-fg);min-width:160px'>Bluetooth Auth: <span style='font-weight:bold' id='ble-auth-value'>-</span></span>
            <button class='btn' id='ble-auth-btn' onclick='toggleBleAuth()' title='Require login before accepting BLE commands'>Toggle</button>
          </div>
          <div style='display:flex;align-items:center;gap:0.75rem;flex-wrap:wrap'>
            <span style='color:var(--panel-fg);min-width:160px'>Serial Auth: <span style='font-weight:bold' id='serial-auth-value'>-</span></span>
            <button class='btn' id='serial-auth-btn' onclick='toggleSerialAuth()' title='Require login before accepting serial CLI commands'>Toggle</button>
          </div>
        </div>
      </div>
    </div>
)SETPART8", HTTPD_RESP_USE_STRLEN);

#if ENABLE_HTTPS
  httpd_resp_send_chunk(req, R"SETHTTPS(
    <div style='background:var(--crumb-bg);border:1px solid var(--border);border-radius:8px;padding:1rem'>
      <div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:0.5rem'>
        <div style='font-weight:bold;color:var(--panel-fg)'>HTTPS / TLS</div>
        <button class='btn' id='btn-https-toggle' onclick="togglePane('https-pane','btn-https-toggle')">Expand</button>
      </div>
      <div style='color:var(--panel-fg);margin-bottom:0.75rem;font-size:0.9rem'>Encrypt all web traffic with TLS. Requires certificate files.</div>
      <div id='https-pane' style='display:none;margin-top:0.75rem'>
        <div style='display:grid;gap:0.75rem'>
          <div style='display:flex;align-items:center;gap:0.75rem;flex-wrap:wrap'>
            <span style='color:var(--panel-fg);min-width:160px'>Server Certificate: <span style='font-weight:bold' id='https-cert-status'>Checking...</span></span>
          </div>
          <div style='display:flex;align-items:center;gap:0.75rem;flex-wrap:wrap'>
            <span style='color:var(--panel-fg);min-width:160px'>Private Key: <span style='font-weight:bold' id='https-key-status'>Checking...</span></span>
          </div>
          <div style='display:flex;align-items:center;gap:0.75rem;flex-wrap:wrap'>
            <span style='color:var(--panel-fg);min-width:160px'>HTTPS Mode: <span style='font-weight:bold' id='https-enabled-value'>-</span></span>
            <button class='btn' id='https-toggle-btn' onclick='toggleHttps()' title='Enable or disable HTTPS (requires reboot)'>Toggle</button>
          </div>
          <div id='https-reboot-row' style='display:none;margin-top:0.5rem;padding:0.75rem;background:rgba(255,255,255,0.05);border:1px solid var(--border);border-radius:6px'>
            <span style='color:var(--panel-fg);font-weight:bold'>Reboot required for changes to take effect.</span>
            <button class='btn' style='margin-left:1rem' onclick='rebootDevice()'>Reboot Now</button>
          </div>
          <div style='display:grid;gap:0.5rem;margin-top:0.25rem'>
            <div style='display:flex;align-items:center;gap:0.75rem;flex-wrap:wrap'>
              <span style='color:var(--panel-fg);min-width:160px;font-size:0.9rem'>Server Certificate:</span>
              <label class='btn' style='cursor:pointer;margin:0'>
                Upload .crt
                <input type='file' id='https-cert-input' accept='.crt,.pem' style='display:none' onchange='uploadHttpsCert(this)'>
              </label>
              <span id='https-cert-upload-status' style='font-size:0.85rem;color:var(--panel-fg);opacity:0.7'></span>
            </div>
            <div style='display:flex;align-items:center;gap:0.75rem;flex-wrap:wrap'>
              <span style='color:var(--panel-fg);min-width:160px;font-size:0.9rem'>Private Key:</span>
              <label class='btn' style='cursor:pointer;margin:0'>
                Upload .key
                <input type='file' id='https-key-input' accept='.key,.pem' style='display:none' onchange='uploadHttpsKey(this)'>
              </label>
              <span id='https-key-upload-status' style='font-size:0.85rem;color:var(--panel-fg);opacity:0.7'></span>
            </div>
            <div style='display:flex;align-items:center;gap:0.75rem;flex-wrap:wrap;margin-top:0.25rem'>
              <span style='color:var(--panel-fg);min-width:160px;font-size:0.9rem'>Self-Signed:</span>
              <button class='btn' id='https-certgen-btn' onclick='generateCerts()'>Generate Certs</button>
              <span id='https-certgen-status' style='font-size:0.85rem;color:var(--panel-fg);opacity:0.7'></span>
            </div>
            <div style='color:var(--panel-fg);font-size:0.85rem;opacity:0.7;margin-top:0.25rem'>
              When HTTPS is active, the device serves on port 443 instead of port 80.
            </div>
          </div>
        </div>
      </div>
    </div>
<script>
(function(){
  function checkCertFile(path, statusId) {
    fetch('/api/files/list?path=' + encodeURIComponent('/system/certs'), {credentials:'same-origin'})
      .then(function(r){return r.json()})
      .then(function(j){
        var el = document.getElementById(statusId);
        if (!el) return;
        var found = false;
        var fname = path.split('/').pop();
        if (j && j.files) {
          for (var i = 0; i < j.files.length; i++) {
            if (j.files[i].name === fname || j.files[i].path === path) {
              found = true;
              break;
            }
          }
        }
        el.textContent = found ? 'Present' : 'Missing';
        el.style.color = '#667eea';
      })
      .catch(function(){
        var el = document.getElementById(statusId);
        if (el) {
          el.textContent = 'Unknown';
          el.style.color = '#a0aec0';
        }
      });
  }

  fetch('/api/files/list?path=' + encodeURIComponent('/system/certs'), {credentials:'same-origin'})
    .then(function(r){return r.json()})
    .then(function(j){
      var checks = [
        {path:'/system/certs/https_server.crt', id:'https-cert-status'},
        {path:'/system/certs/https_server.key', id:'https-key-status'}
      ];
      checks.forEach(function(c){
        var el = document.getElementById(c.id);
        if (!el) return;
        var found = false;
        var fname = c.path.split('/').pop();
        if (j && j.files) {
          for (var i=0;i<j.files.length;i++) {
            if (j.files[i].name===fname || j.files[i].path===c.path) { found=true; break; }
          }
        }
        el.textContent = found ? 'Present' : 'Missing';
        el.style.color = '#667eea';
      });
    })
    .catch(function(){
      ['https-cert-status','https-key-status'].forEach(function(id){
        var el = document.getElementById(id);
        if (el) { el.textContent='Unknown'; el.style.color='#a0aec0'; }
      });
    });

  window._httpsCurrentValue = false;
  function refreshHttpsStatus() {
    fetch('/api/settings',{credentials:'same-origin'})
      .then(function(r){return r.json()})
      .then(function(j){
        var val = j && j.settings && j.settings.http && j.settings.http.httpsEnabled;
        window._httpsCurrentValue = !!val;
        var el = document.getElementById('https-enabled-value');
        if(el){ el.textContent = val ? 'Enabled' : 'Disabled'; el.style.color = '#667eea'; }
      });
  }
  refreshHttpsStatus();

  window.toggleHttps = function(){
    var newVal = !window._httpsCurrentValue;
    var cmd = 'httpsEnabled ' + (newVal ? '1' : '0');
    sendSequential([cmd], function(){
      window._httpsCurrentValue = newVal;
      var el = document.getElementById('https-enabled-value');
      if(el){ el.textContent = newVal ? 'Enabled' : 'Disabled'; el.style.color = '#667eea'; }
      document.getElementById('https-reboot-row').style.display = 'block';
    }, function(err){ alert('Failed to toggle HTTPS: ' + err.message); });
  };
  function uploadHttpsFile(file, destPath, statusId, inputEl) {
    var statusEl = document.getElementById(statusId);
    if (statusEl) { statusEl.textContent = 'Uploading...'; statusEl.style.color = '#a0aec0'; }
    var reader = new FileReader();
    reader.onload = function(evt) {
      var content = evt.target.result;
      fetch('/api/files/upload', {
        method: 'POST',
        credentials: 'same-origin',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'path=' + encodeURIComponent(destPath) + '&binary=0&content=' + encodeURIComponent(content)
      })
      .then(function(r){ return r.json(); })
      .then(function(j){
        if (j.success) {
          if (statusEl) { statusEl.textContent = 'Uploaded'; statusEl.style.color = '#667eea'; }
          var fname = destPath.split('/').pop();
          var certId = destPath.indexOf('.crt') >= 0 ? 'https-cert-status' : 'https-key-status';
          var certEl = document.getElementById(certId);
          if (certEl) { certEl.textContent = 'Present'; certEl.style.color = '#667eea'; }
        } else {
          if (statusEl) { statusEl.textContent = 'Failed: ' + (j.error || 'unknown'); statusEl.style.color = '#667eea'; }
        }
        if (inputEl) inputEl.value = '';
      })
      .catch(function(e){
        if (statusEl) { statusEl.textContent = 'Error: ' + e.message; statusEl.style.color = '#667eea'; }
        if (inputEl) inputEl.value = '';
      });
    };
    reader.readAsText(file);
  }
  window.uploadHttpsCert = function(input) {
    if (!input.files || !input.files[0]) return;
    uploadHttpsFile(input.files[0], '/system/certs/https_server.crt', 'https-cert-upload-status', input);
  };
  window.uploadHttpsKey = function(input) {
    if (!input.files || !input.files[0]) return;
    uploadHttpsFile(input.files[0], '/system/certs/https_server.key', 'https-key-upload-status', input);
  };

  window.generateCerts = function(){
    if(!confirm('Generate a self-signed ECDSA P-256 certificate? This will overwrite any existing cert/key files.')) return;
    var btn = document.getElementById('https-certgen-btn');
    var status = document.getElementById('https-certgen-status');
    if(btn) btn.disabled = true;
    if(status){ status.textContent = 'Generating...'; status.style.color = '#a0aec0'; }
    fetch('/api/cli',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd=certgen'})
      .then(function(r){ return r.text(); })
      .then(function(output){
        if(btn) btn.disabled = false;
        if(output.indexOf('Error') >= 0 || output.indexOf('error') >= 0){
          if(status){ status.textContent = output; status.style.color = '#fc8181'; }
        } else {
          if(status){ status.textContent = 'Generated!'; status.style.color = '#68d391'; }
          checkCertFile('/system/certs/https_server.crt', 'https-cert-status');
          checkCertFile('/system/certs/https_server.key', 'https-key-status');
          document.getElementById('https-reboot-row').style.display = 'block';
        }
      })
      .catch(function(e){
        if(btn) btn.disabled = false;
        if(status){ status.textContent = 'Error: ' + e.message; status.style.color = '#fc8181'; }
      });
  };

  window.rebootDevice = function(){
    if(!confirm('Reboot the device now?')) return;
    fetch('/api/cli',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd=reboot'})
      .then(function(){
        document.body.innerHTML = '<div style="text-align:center;padding:4rem;color:var(--panel-fg)"><h2>Rebooting...</h2><p>The device is restarting. Please wait and then reconnect.</p></div>';
      });
  };
})();
</script>
)SETHTTPS", HTTPD_RESP_USE_STRLEN);
#endif // ENABLE_HTTPS

  httpd_resp_send_chunk(req, R"SETPART8B(
  </div>
  </div>
</div>
<div style='text-align:center;margin-top:2rem'>
  <button class='btn' onclick='refreshSettings()' title='Reload all settings from device memory'>Refresh Settings</button>
)SETPART8B", HTTPD_RESP_USE_STRLEN);

  // Part 1: JavaScript - Core init, state management, and rendering (EXPANDED FOR READABILITY)
  httpd_resp_send_chunk(req, R"SETPART1(<script>
console.log('[SETTINGS] Section 1: Pre-script sentinel');
</script>
<script>
console.log('[SETTINGS] Part 1: Core init starting...');
(function() {
  try {
    window.settingsBuildTag = 'settings-streaming-v1';
    window.__S = window.__S || {};
    window.$ = function(id) {
      return document.getElementById(id);
    };
    __S.state = {
      savedSSIDs: [],
      currentSSID: ''
    };
    console.log('[SETTINGS] Part 1: Base objects initialized');
    
    window.onload = function() {
      console.log('[SETTINGS] Window onload - fetching build config and settings');
      try {
        // Fetch build config first to hide unavailable debug options
        if (typeof window.fetchBuildConfig === 'function') {
          window.fetchBuildConfig().then(function() {
            refreshSettings();
          }).catch(function() {
            // If build config fails, still load settings
            refreshSettings();
          });
        } else {
          refreshSettings();
        }
      } catch(e) {
        console.error('[SETTINGS] onload fail', e);
        refreshSettings();
      }
    };
    console.log('[SETTINGS] onload registered');
    
    __S.renderSettings = function(s) {
      try {
        console.log('[SETTINGS] renderSettings called with:', s);
        __S.state.currentSSID = (s.wifiPrimarySSID || (s.wifi && s.wifi.wifiSSID) || '');
        $('wifi-ssid').textContent = __S.state.currentSSID;
        var primary = __S.state.currentSSID || '';
        var list = Array.isArray(s.wifiNetworks) 
          ? s.wifiNetworks.map(function(n) { return n && n.ssid; }).filter(function(x) { return !!x; })
          : [];
        __S.state.savedSSIDs = [];
        if (primary) __S.state.savedSSIDs.push(primary);
        if (list && list.length) __S.state.savedSSIDs = __S.state.savedSSIDs.concat(list);
        var wifiAutoReconnect = s.wifiAutoReconnect || (s.wifi && s.wifi.wifiAutoReconnect) || false;
        $('wifi-value').textContent = wifiAutoReconnect ? 'Enabled' : 'Disabled';
        var webHistorySize = (s.cli && s.cli.webHistorySize) || s.webCliHistorySize || 10;
        $('cli-value').textContent = webHistorySize;
        $('cli-input').value = webHistorySize;
        $('wifi-btn').textContent = wifiAutoReconnect ? 'Disable' : 'Enable';
        $('espnow-value').textContent = s.espnowenabled ? 'Enabled' : 'Disabled';
        $('espnow-btn').textContent = s.espnowenabled ? 'Disable' : 'Enable';
        
        // Load ESP-NOW settings
        var espnow = s.espnow || {};
        if ($('espnow-mesh-value')) {
          $('espnow-mesh-value').textContent = espnow.mesh ? 'Enabled' : 'Disabled';
          $('espnow-mesh-btn').textContent = espnow.mesh ? 'Disable' : 'Enable';
        }
        if ($('espnow-devicename')) $('espnow-devicename').value = espnow.deviceName || '';
        if ($('espnow-friendlyname')) $('espnow-friendlyname').value = espnow.friendlyName || '';
        if ($('espnow-room')) $('espnow-room').value = espnow.room || '';
        if ($('espnow-zone')) $('espnow-zone').value = espnow.zone || '';
        if ($('espnow-tags')) $('espnow-tags').value = espnow.tags || '';
        if ($('espnow-stationary')) $('espnow-stationary').checked = !!espnow.stationary;
        if ($('espnow-meshrole')) $('espnow-meshrole').value = String(espnow.meshRole || 0);
        if ($('espnow-mastermac')) $('espnow-mastermac').value = espnow.masterMAC || '';
        if ($('espnow-backupmac')) $('espnow-backupmac').value = espnow.backupMAC || '';
        
        // Load bond settings
        if ($('bond-enabled-value')) {
          $('bond-enabled-value').textContent = espnow.bondModeEnabled ? 'Enabled' : 'Disabled';
          $('bond-enabled-btn').textContent = espnow.bondModeEnabled ? 'Disable' : 'Enable';
        }
        if ($('bond-role')) $('bond-role').value = String(espnow.bondRole || 0);
        if ($('bond-peermac')) $('bond-peermac').value = espnow.bondPeerMac || '';
        if ($('bond-stream-thermal')) $('bond-stream-thermal').checked = !!espnow.bondStreamThermal;
        if ($('bond-stream-tof')) $('bond-stream-tof').checked = !!espnow.bondStreamTof;
        if ($('bond-stream-imu')) $('bond-stream-imu').checked = !!espnow.bondStreamImu;
        if ($('bond-stream-gps')) $('bond-stream-gps').checked = !!espnow.bondStreamGps;
        if ($('bond-stream-gamepad')) $('bond-stream-gamepad').checked = !!espnow.bondStreamGamepad;
        if ($('bond-stream-fmradio')) $('bond-stream-fmradio').checked = !!espnow.bondStreamFmradio;
        if ($('bond-stream-rtc')) $('bond-stream-rtc').checked = !!espnow.bondStreamRtc;
        if ($('bond-stream-presence')) $('bond-stream-presence').checked = !!espnow.bondStreamPresence;
        
        var out = (s.output || {}), th = (s.thermal_mlx90640 || {}), tof = (s.tof_vl53l4cx || {}), oled = (s.oled_ssd1306 || {}), led = (s.led || {}), imu = (s.imu_bno055 || {}), i2c = (s.i2c || {});
        var thUI = (th.ui || {}), thDev = (th.device || {}), tofUI = (tof.ui || {}), tofDev = (tof.device || {});
        var outSerial = (out.outSerial !== undefined ? out.outSerial : s.outSerial);
        var outWeb = (out.outWeb !== undefined ? out.outWeb : s.outWeb);
        var outDisplay = (out.outDisplay !== undefined ? out.outDisplay : s.outDisplay);
        $('serial-value').textContent = outSerial ? 'Enabled' : 'Disabled';
        $('serial-btn').textContent = outSerial ? 'Disable' : 'Enable';
        $('web-value').textContent = outWeb ? 'Enabled' : 'Disabled';
        $('web-btn').textContent = outWeb ? 'Disable' : 'Enable';
        $('display-value').textContent = outDisplay ? 'Enabled' : 'Disabled';
        $('display-btn').textContent = outDisplay ? 'Disable' : 'Enable';
        var hw = (s.hardware || {});
        var led = (hw.led || {});
        if (led.brightness !== undefined) {
          var b = $('ledBrightness');
          if (b) b.value = led.brightness;
        } else if (s.ledBrightness !== undefined) {
          var b = $('ledBrightness');
          if (b) b.value = s.ledBrightness;
        }
        var ledEnabled = (led.startupEnabled !== undefined ? led.startupEnabled : s.ledStartupEnabled);
        var chk = $('ledStartupEnabled');
        if (chk) chk.checked = (ledEnabled === 1 || ledEnabled === true);
        if (led.startupEffect) {
          var ef = $('ledStartupEffect');
          if (ef) ef.value = led.startupEffect;
        } else if (s.ledStartupEffect) {
          var ef = $('ledStartupEffect');
          if (ef) ef.value = s.ledStartupEffect;
        }
        if (led.startupColor) {
          var c1 = $('ledStartupColor');
          if (c1) c1.value = led.startupColor;
        } else if (s.ledStartupColor) {
          var c1 = $('ledStartupColor');
          if (c1) c1.value = s.ledStartupColor;
        }
        if (led.startupColor2) {
          var c2 = $('ledStartupColor2');
          if (c2) c2.value = led.startupColor2;
        } else if (s.ledStartupColor2) {
          var c2 = $('ledStartupColor2');
          if (c2) c2.value = s.ledStartupColor2;
        }
        if (led.startupDuration !== undefined) {
          var d = $('ledStartupDuration');
          if (d) d.value = led.startupDuration;
        } else if (s.ledStartupDuration !== undefined) {
          var d = $('ledStartupDuration');
          if (d) d.value = s.ledStartupDuration;
        }
        var oledEn = $('oledEnabled');
        if (oledEn) oledEn.checked = (oled.oledEnabled === 1 || oled.oledEnabled === true);
        var gamepadAuto = $('gamepadAutoStart');
        if (gamepadAuto && s.gamepad && s.gamepad.gamepadAutoStart !== undefined) {
          gamepadAuto.checked = (s.gamepad.gamepadAutoStart === 1 || s.gamepad.gamepadAutoStart === true);
        }
        if (oled.bootMode) {
          var bm = $('oledBootMode');
          if (bm) bm.value = oled.bootMode;
        }
        if (oled.defaultMode) {
          var dm = $('oledDefaultMode');
          if (dm) dm.value = oled.defaultMode;
        }
        if (oled.bootDuration !== undefined) {
          var bd = $('oledBootDuration');
          if (bd) bd.value = oled.bootDuration;
        }
        if (oled.updateInterval !== undefined) {
          var ui = $('oledUpdateInterval');
          if (ui) ui.value = oled.updateInterval;
        }
        if (oled.brightness !== undefined) {
          var br = $('oledBrightness');
          if (br) br.value = oled.brightness;
        }
        if (oled.thermalScale !== undefined) {
          var ts = $('oledThermalScale');
          if (ts) ts.value = oled.thermalScale;
        }
        if (oled.thermalColorMode) {
          var tcm = $('oledThermalColorMode');
          if (tcm) tcm.value = oled.thermalColorMode;
        }
        var isAdm = (s && s.user && (s.user.isAdmin === true)) || (__S && __S.user && (__S.user.isAdmin === true));
        var hasFeat = (__S && __S.features && __S.features.adminSessions === true);
        var admin = isAdm && hasFeat;
        var sec = document.getElementById('admin-section');
        if (sec) {
          sec.style.display = admin ? 'block' : 'none';
        }
        if (admin) {
          try {
            if (typeof window.refreshUsers === 'function') {
              refreshUsers();
            }
          } catch(e) {}
          try {
            var oledSect = s.oled_ssd1306 || {};
            var dispAuth = (oledSect.oledRequireAuth !== undefined ? oledSect.oledRequireAuth : true);
            var dispAuthEl = $('display-auth-value');
            if (dispAuthEl) {
              dispAuthEl.textContent = dispAuth ? 'Required' : 'Disabled';
              dispAuthEl.style.color = '#667eea';
            }
            var dispAuthBtn = $('display-auth-btn');
            if (dispAuthBtn) dispAuthBtn.textContent = dispAuth ? 'Disable' : 'Enable';
            var btSect = s.bluetooth || {};
            var bleAuth = (btSect.bluetoothRequireAuth !== undefined ? btSect.bluetoothRequireAuth : true);
            var bleAuthEl = $('ble-auth-value');
            if (bleAuthEl) {
              bleAuthEl.textContent = bleAuth ? 'Required' : 'Disabled';
              bleAuthEl.style.color = '#667eea';
            }
            var bleAuthBtn = $('ble-auth-btn');
            if (bleAuthBtn) bleAuthBtn.textContent = bleAuth ? 'Disable' : 'Enable';
            var outSect = s.output || {};
            var serialAuth = (outSect.serialRequireAuth !== undefined ? outSect.serialRequireAuth : true);
            var serialAuthEl = $('serial-auth-value');
            if (serialAuthEl) {
              serialAuthEl.textContent = serialAuth ? 'Required' : 'Disabled';
              serialAuthEl.style.color = '#667eea';
            }
            var serialAuthBtn = $('serial-auth-btn');
            if (serialAuthBtn) serialAuthBtn.textContent = serialAuth ? 'Disable' : 'Enable';
            var hasBle = (__S && __S.features && __S.features.bluetooth === true);
            var bleRow = $('ble-auth-row');
            if (bleRow) bleRow.style.display = hasBle ? 'flex' : 'none';
          } catch(e) {}
        }
      } catch(e) {
        alert('Render error: ' + e.message);
      }
    };
    
    window.renderOutputRuntime = function(obj) {
      try {
        obj = obj || {};
        var r = obj.runtime || {};
        var p = obj.persisted || {};
        var set = function(id, val) {
          var el = $(id);
          if (!el) return;
          var on = (String(val) == '1' || val === 1 || val === true || String(val).toLowerCase() == 'true');
          el.textContent = on ? 'On' : 'Off';
          el.style.color = '#667eea';
        };
        var setHidden = function(id, hide) {
          var el = $(id);
          if (!el) return;
          try {
            if (hide) {
              el.classList.add('vis-gone');
            } else {
              el.classList.remove('vis-gone');
            }
          } catch(_) {
            el.style.display = hide ? 'none' : '';
          }
        };
        set('serial-runtime', r.serial);
        set('web-runtime', r.web);
        set('display-runtime', r.display);
        set('g2-runtime', r.g2);
        try {
          if (p && typeof p === 'object') {
            if (p.serial !== undefined) {
              var pv = $('serial-value');
              if (pv) pv.textContent = p.serial ? 'Enabled' : 'Disabled';
              var pb = $('serial-btn');
              if (pb) {
                pb.textContent = p.serial ? 'Disable' : 'Enable';
              }
            }
            if (p.web !== undefined) {
              var pv2 = $('web-value');
              if (pv2) pv2.textContent = p.web ? 'Enabled' : 'Disabled';
              var pb2 = $('web-btn');
              if (pb2) {
                pb2.textContent = p.web ? 'Disable' : 'Enable';
              }
            }
            if (p.display !== undefined) {
              var pv3 = $('display-value');
              if (pv3) pv3.textContent = p.display ? 'Enabled' : 'Disabled';
              var pb3 = $('display-btn');
              if (pb3) {
                pb3.textContent = p.display ? 'Disable' : 'Enable';
              }
            }
            if (p.g2 !== undefined) {
              var pv4 = $('g2-value');
              if (pv4) pv4.textContent = p.g2 ? 'Enabled' : 'Disabled';
              var pb4 = $('g2-btn');
              if (pb4) {
                pb4.textContent = p.g2 ? 'Disable' : 'Enable';
              }
            }
          }
          var curSerial = (r.serial !== undefined) ? (String(r.serial) == '1' || r.serial === 1 || r.serial === true || String(r.serial).toLowerCase() == 'true') : !!(p && p.serial);
          var curWeb = (r.web !== undefined) ? (String(r.web) == '1' || r.web === 1 || r.web === true || String(r.web).toLowerCase() == 'true') : !!(p && p.web);
          var curDisplay = (r.display !== undefined) ? (String(r.display) == '1' || r.display === 1 || r.display === true || String(r.display).toLowerCase() == 'true') : !!(p && p.display);
          var curG2 = (r.g2 !== undefined) ? (String(r.g2) == '1' || r.g2 === 1 || r.g2 === true || String(r.g2).toLowerCase() == 'true') : !!(p && p.g2);
          setHidden('serial-temp-on', curSerial);
          setHidden('serial-temp-off', !curSerial);
          setHidden('web-temp-on', curWeb);
          setHidden('web-temp-off', !curWeb);
          setHidden('display-temp-on', curDisplay);
          setHidden('display-temp-off', !curDisplay);
          setHidden('g2-temp-on', curG2);
          setHidden('g2-temp-off', !curG2);
        } catch(e) {}
      } catch(e) {}
    };
    
    window.refreshOutput = function() {
      return fetch('/api/output', {credentials: 'same-origin'})
        .then(function(r) { return r.text(); })
        .then(function(t) {
          var d = null;
          try {
            d = JSON.parse(t || '{}');
          } catch(e) {
            return;
          }
          if (d && d.success) {
            window.renderOutputRuntime(d);
          }
        })
        .catch(function(e) {});
    };
    
    window.setOutputRuntime = function(channel, val) {
      try {
        if (!channel) return;
        var map = {serial: 'outserial', web: 'outweb', display: 'outdisplay', g2: 'outg2'};
        var key = map[channel];
        if (!key) return;
        var v = val ? 1 : 0;
        var cmd = key + ' temp ' + v;
        fetch('/api/cli', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          credentials: 'same-origin',
          body: 'cmd=' + encodeURIComponent(cmd)
        })
        .then(function(r) { return r.text(); })
        .then(function(_t) {
          try {
            if (typeof window.refreshOutput === 'function') {
              window.refreshOutput();
            }
          } catch(_) {}
        })
        .catch(function(e) {
          alert('Error: ' + e.message);
        });
      } catch(e) {
        alert('Error: ' + e.message);
      }
    };
    
    window.onload = function() {
      try {
        refreshSettings();
      } catch(e) {}
    };
    
  } catch(err) {
    console.error('[SETTINGS] Part 1 ERROR:', err);
    alert('Settings page error (Part 1): ' + err.message);
  }
})();
console.log('[SETTINGS] Part 1: Complete');
</script>)SETPART1", HTTPD_RESP_USE_STRLEN);

  // Part 2: JavaScript - API helpers and UI actions (EXPANDED FOR READABILITY)
  httpd_resp_send_chunk(req, R"SETPART2(<script>
console.log('[SETTINGS] Part 2: API helpers starting...');
(function() {
  try {
    // Global build configuration cache
    window.__buildConfig = null;
    
    // Fetch build configuration
    window.fetchBuildConfig = function() {
      return fetch('/api/buildconfig', {credentials: 'same-origin'})
        .then(function(r) { return r.json(); })
        .then(function(config) {
          window.__buildConfig = config;
          console.log('[SETTINGS] Build config loaded:', config);
          return config;
        })
        .catch(function(err) {
          console.error('[SETTINGS] Failed to load build config:', err);
        });
    };
    
    // Refresh settings from device
    window.refreshSettings = function() {
      console.log('[SETTINGS] refreshSettings called');
      fetch('/api/settings', {credentials: 'same-origin'})
        .then(function(r) {
          console.log('[SETTINGS] Settings fetch response:', r.status);
          return r.text();
        })
        .then(function(t) {
          console.log('[SETTINGS] Settings response text length:', t.length);
          var d = null;
          try {
            d = JSON.parse(t || '{}');
          } catch(e) {
            console.error('[SETTINGS] JSON parse error:', e);
            alert('Error fetching settings');
            return;
          }
          console.log('[SETTINGS] Parsed settings data:', d);
          if (d && d.success) {
            try {
              window.__S = window.__S || {};
              __S.user = d.user || null;
              __S.features = d.features || null;
            } catch(_) {}
            __S.renderSettings(d.settings || {});
          } else {
            alert('Error: ' + (d && d.error || 'Unknown'));
          }
        })
        .then(function() {
          try {
            if (typeof window.refreshOutput === 'function') {
              window.refreshOutput();
            }
          } catch(_) {}
        })
        .catch(function(e) {
          console.error('[SETTINGS] Fetch error:', e);
          alert('Error: ' + e.message);
        });
    };
    console.log('[SETTINGS] refreshSettings defined');
    
    // Toggle WiFi auto-reconnect
    window.toggleWifi = function() {
      console.log('[SETTINGS] toggleWifi called');
      var cur = ($('wifi-value').textContent === 'Enabled') ? 1 : 0;
      var v = cur ? 0 : 1;
      console.log('[SETTINGS] toggleWifi - current:', cur, 'new:', v);
      var cmd = 'wifiautoreconnect ' + v;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) { return r.text(); })
      .then(function(t) {
        console.log('[SETTINGS] toggleWifi result:', t);
        if (t.indexOf('Error') >= 0) {
          alert(t);
        }
        refreshSettings();
      })
      .catch(function(e) {
        console.error('[SETTINGS] toggleWifi error:', e);
        alert('Error: ' + e.message);
      });
    };
    console.log('[SETTINGS] toggleWifi defined');
    
    // Toggle ESP-NOW auto-init
    window.toggleEspNow = function() {
      console.log('[SETTINGS] toggleEspNow called');
      var cur = ($('espnow-value').textContent === 'Enabled') ? 1 : 0;
      var v = cur ? 0 : 1;
      console.log('[SETTINGS] toggleEspNow - current:', cur, 'new:', v);
      var cmd = 'espnowenabled ' + v;
      $('espnow-btn').textContent = '...';
      $('espnow-btn').disabled = true;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) { return r.text(); })
      .then(function(t) {
        console.log('[SETTINGS] toggleEspNow result:', t);
        if (t.indexOf('Error') >= 0) {
          alert(t);
        } else {
          if (v === 1) {
            alert('ESP-NOW enabled. Will initialize on next reboot.');
          } else {
            alert('ESP-NOW disabled. Will not initialize on next reboot.');
          }
        }
        refreshSettings();
      })
      .catch(function(e) {
        console.error('[SETTINGS] toggleEspNow error:', e);
        alert('Error: ' + e.message);
        refreshSettings();
      });
    };
    console.log('[SETTINGS] toggleEspNow defined');
    
    // Toggle ESP-NOW mesh mode
    window.toggleEspNowMesh = function() {
      var cur = ($('espnow-mesh-value').textContent === 'Enabled') ? 1 : 0;
      var v = cur ? 0 : 1;
      var cmd = 'espnow mode ' + (v ? 'mesh' : 'direct');
      $('espnow-mesh-btn').textContent = '...';
      $('espnow-mesh-btn').disabled = true;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) { return r.text(); })
      .then(function(t) {
        refreshSettings();
      })
      .catch(function(e) {
        alert('Error: ' + e.message);
        refreshSettings();
      });
    };
    
    // Toggle bond mode
    window.toggleBondMode = function() {
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent('bondmodeenabled')
      })
      .then(function(r) { return r.text(); })
      .then(function(t) { refreshSettings(); })
      .catch(function(e) { alert('Error: ' + e.message); });
    };
    
    // Save all ESP-NOW settings
    window.saveEspNowSettings = function() {
      var status = $('espnow-save-status');
      status.textContent = 'Saving...';
      status.style.color = '#667eea';

      var s = {
        deviceName:   $('espnow-devicename').value.trim(),
        friendlyName: $('espnow-friendlyname').value.trim(),
        room:         $('espnow-room').value.trim(),
        zone:         $('espnow-zone').value.trim(),
        tags:         $('espnow-tags').value.trim(),
        stationary:   $('espnow-stationary').checked,
        meshRole:     parseInt($('espnow-meshrole').value),
        masterMAC:    $('espnow-mastermac').value.trim(),
        backupMAC:    $('espnow-backupmac').value.trim(),
        bondRole:     parseInt($('bond-role').value),
        bondPeerMac:  $('bond-peermac').value.trim()
      };

      var cmds = [];
      if (s.deviceName)   cmds.push('espnow setname "' + s.deviceName + '"');
      if (s.friendlyName) cmds.push('espnow friendlyname "' + s.friendlyName + '"');
      if (s.room)         cmds.push('espnow room "' + s.room + '"');
      if (s.zone)         cmds.push('espnow zone "' + s.zone + '"');
      if (s.tags)         cmds.push('espnow tags "' + s.tags + '"');
      cmds.push('espnow stationary ' + (s.stationary ? 'on' : 'off'));
      var roleNames = ['worker', 'master', 'backup'];
      if (s.meshRole >= 0 && s.meshRole <= 2) cmds.push('espnow role ' + roleNames[s.meshRole]);
      if (s.masterMAC) cmds.push('espnow mastermac ' + s.masterMAC);
      if (s.backupMAC) cmds.push('espnow backupmac ' + s.backupMAC);
      
      // Bond settings
      var bondRoleNames = ['worker', 'master'];
      if (s.bondRole >= 0 && s.bondRole <= 1) cmds.push('bondrole ' + s.bondRole);
      if (s.bondPeerMac) cmds.push('bondpeermac ' + s.bondPeerMac);
      cmds.push('bondstreamthermal ' + ($('bond-stream-thermal').checked ? '1' : '0'));
      cmds.push('bondstreamtof ' + ($('bond-stream-tof').checked ? '1' : '0'));
      cmds.push('bondstreamimu ' + ($('bond-stream-imu').checked ? '1' : '0'));
      cmds.push('bondstreamgps ' + ($('bond-stream-gps').checked ? '1' : '0'));
      cmds.push('bondstreamgamepad ' + ($('bond-stream-gamepad').checked ? '1' : '0'));
      cmds.push('bondstreamfmradio ' + ($('bond-stream-fmradio').checked ? '1' : '0'));
      cmds.push('bondstreamrtc ' + ($('bond-stream-rtc').checked ? '1' : '0'));
      cmds.push('bondstreampresence ' + ($('bond-stream-presence').checked ? '1' : '0'));

      sendSequential(cmds,
        function() {
          status.textContent = 'Saved successfully!';
          status.style.color = '#28a745';
          setTimeout(function() { status.textContent = ''; refreshSettings(); }, 2000);
        },
        function(e) {
          status.textContent = 'Error: ' + (e ? e.message : 'unknown');
          status.style.color = '#dc3545';
        }
      );
    };
    
    // Update Web CLI history size
    window.updateWebCliHistory = function() {
      console.log('[SETTINGS] updateWebCliHistory called');
      var v = parseInt($('cli-input').value);
      console.log('[SETTINGS] updateWebCliHistory value:', v);
      if (v < 1) {
        alert('Must be at least 1');
        return;
      }
      var cmd = 'webclihistorysize ' + v;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) { return r.text(); })
      .then(function(t) {
        console.log('[SETTINGS] updateWebCliHistory result:', t);
        refreshSettings();
      })
      .catch(function(e) {
        console.error('[SETTINGS] updateWebCliHistory error:', e);
        alert('Error: ' + e.message);
      });
    };
    console.log('[SETTINGS] updateWebCliHistory defined');
    
    // Clear CLI history
    window.clearCliHistory = function() {
      var cmd = 'clear';
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) { return r.text(); })
      .then(function(t) {
        try {
          if (t && t.indexOf('Error') >= 0) {
            alert(t);
          }
        } catch(_) {}
        if (typeof refreshSettings === 'function') refreshSettings();
      })
      .catch(function(e) {
        alert('Error: ' + e.message);
      });
    };
    
    // Update timezone
    window.updateTimezone = function() {
      const select = document.getElementById('tz-select');
      if (!select) return;
      const value = select.value;
      if (!value) {
        alert('Please select a timezone');
        return;
      }
      const cmd = 'tzoffsetminutes ' + value;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) { return r.text(); })
      .then(function(response) {
        alert(response);
        if (typeof refreshSettings === 'function') refreshSettings();
      })
      .catch(function(e) {
        alert('Failed to update timezone: ' + e.message);
      });
    };
    
    // Update NTP server
    window.updateNtpServer = function() {
      var val = $('ntp-input').value.trim();
      if (!val) {
        alert('Enter NTP server');
        return;
      }
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent('ntpserver ' + val)
      })
      .then(function(r) { return r.text(); })
      .then(function(t) {
        if (t.indexOf('Error') >= 0) {
          alert(t);
        } else {
          refreshSettings();
        }
      })
      .catch(function(e) {
        alert('Error: ' + e.message);
      });
    };
    
    // Toggle output channel
    window.toggleOutput = function(setting, channel) {
      var valueId = (channel || setting.replace('out', '').toLowerCase()) + '-value';
      var btnId = (channel || setting.replace('out', '').toLowerCase()) + '-btn';
      var valueEl = $(valueId);
      var btnEl = $(btnId);
      if (!valueEl || !btnEl) return;
      var cur = (valueEl.textContent === 'Enabled') ? 1 : 0;
      var newVal = cur ? 0 : 1;
      btnEl.textContent = '...';
      btnEl.disabled = true;
      valueEl.textContent = newVal ? 'Enabled' : 'Disabled';
      var cmdMap = {
        outSerial: 'outserial',
        outWeb: 'outweb',
        outDisplay: 'outdisplay'
      };
      var key = cmdMap[setting] || setting.toLowerCase();
      var cmd = key + ' ' + newVal;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) { return r.text(); })
      .then(function(_t) {
        btnEl.textContent = newVal ? 'Disable' : 'Enable';
        btnEl.disabled = false;
        try {
          if (typeof window.refreshOutput === 'function') {
            window.refreshOutput();
          }
        } catch(_) {}
      })
      .catch(function(e) {
        valueEl.textContent = cur ? 'Enabled' : 'Disabled';
        btnEl.textContent = cur ? 'Disable' : 'Enable';
        btnEl.disabled = false;
        alert('Error: ' + e.message);
      });
    };
    
    // Disconnect WiFi
    window.disconnectWifi = function() {
      if (confirm('Are you sure you want to disconnect from WiFi? You may lose connection to this device.')) {
        fetch('/api/cli', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          credentials: 'same-origin',
          body: 'cmd=' + encodeURIComponent('wifidisconnect')
        })
        .then(function(r) { return r.text(); })
        .then(function(t) {
          alert(t || 'Disconnected');
        })
        .catch(function(e) {
          alert('Error: ' + e.message);
        });
      }
    };
    
    // Toggle pane visibility
    window.togglePane = function(paneId, btnId) {
      var p = document.getElementById(paneId);
      var b = document.getElementById(btnId);
      if (!p || !b) return;
      var isHidden = (p.style.display === 'none' || !p.style.display);
      p.style.display = isHidden ? 'block' : 'none';
      b.textContent = isHidden ? 'Collapse' : 'Expand';
    };
    
  } catch(err) {
    console.error('[SETTINGS] Part 2 ERROR:', err);
    alert('Settings page error (Part 2): ' + err.message);
  }
})();
console.log('[SETTINGS] Part 2: Complete');
</script>)SETPART2", HTTPD_RESP_USE_STRLEN);

  // Part 3: JavaScript - Save functions for sensors and hardware (EXPANDED FOR READABILITY)
  httpd_resp_send_chunk(req, R"SETPART3(<script>
console.log('[SETTINGS] Part 3: Save functions starting...');
(function() {
  try {
    // Save device sensor settings
    window.saveDeviceSensorSettings = function() {
      var cmds = [];
      var getInt = function(id, def) {
        var el = $(id);
        if (!el) return def;
        var n = parseInt(el.value, 10);
        return isNaN(n) ? def : n;
      };
      var getStr = function(id) {
        var el = $(id);
        if (!el) return null;
        return String(el.value || '');
      };
      var getBool = function(id) {
        var el = $(id);
        if (!el) return null;
        return el.checked ? 1 : 0;
      };
      
      var map = [
        ['thermalTargetFps', 'thermaltargetfps'],
        ['thermalDevicePollMs', 'thermaldevicepollms'],
        ['thermalUpscaleFactor', 'thermalupscalefactor'],
        ['tofDevicePollMs', 'tofdevicepollms'],
        ['imuDevicePollMs', 'imudevicepollms'],
        ['thermalI2cClockHz', 'thermali2cclockHz'],
        ['tofI2cClockHz', 'tofi2cclockHz']
      ];
      map.forEach(function(pair) {
        var id = pair[0], cmdKey = pair[1];
        var v = getInt(id, null);
        if (v !== null && v !== undefined) {
          cmds.push(cmdKey + ' ' + v);
        }
      });
      
      var rmmEn = getBool('thermalRollingMinMaxEnabled');
      if (rmmEn !== null) cmds.push('thermalrollingminmaxenabled ' + rmmEn);
      var rmmAlpha = getStr('thermalRollingMinMaxAlpha');
      if (rmmAlpha !== null) cmds.push('thermalrollingminmaxalpha ' + rmmAlpha);
      var rmmGuard = getStr('thermalRollingMinMaxGuardC');
      if (rmmGuard !== null) cmds.push('thermalrollingminmaxguardc ' + rmmGuard);
      var tmpAlpha = getStr('thermalTemporalAlpha');
      if (tmpAlpha !== null) cmds.push('thermaltemporalalpha ' + tmpAlpha);
      
      if (cmds.length === 0) {
        alert('No device settings to save.');
        return;
      }

      sendSequential(cmds,
        function() {
          alert('Device settings saved. Restart the thermal sensor (closethermal + openthermal) or reboot manually to apply upscale changes.');
          refreshSettings();
        },
        function() { alert('One or more device commands failed.'); }
      );
    };
    
    // Save sensors UI settings
    window.saveSensorsUISettings = function() {
      try {
        var cmds = [];
        var pushCmd = function(k, v) {
          cmds.push(k + ' ' + v);
        };
        var getInt = function(id) {
          var el = $(id);
          if (!el) return null;
          var n = parseInt(el.value, 10);
          return isNaN(n) ? null : n;
        };
        var getStr = function(id) {
          var el = $(id);
          if (!el) return null;
          return String(el.value || '');
        };
        
        var tp = getInt('thermalPollingMs');
        if (tp !== null) pushCmd('thermalpollingms', tp);
        var tpf = getInt('tofPollingMs');
        if (tpf !== null) pushCmd('tofpollingms', tpf);
        var tss = getInt('tofStabilityThreshold');
        if (tss !== null) pushCmd('tofstabilitythreshold', tss);
        var pal = getStr('thermalPaletteDefault');
        if (pal) pushCmd('thermalpalettedefault', pal);
        var twf = getInt('thermalWebMaxFps');
        if (twf !== null) pushCmd('thermalwebmaxfps', twf);
        var ewma = getStr('thermalEWMAFactor');
        if (ewma !== null) pushCmd('thermalewmafactor', ewma);
        var ttm = getInt('thermalTransitionMs');
        if (ttm !== null) pushCmd('thermaltransitionms', ttm);
        var ttm2 = getInt('tofTransitionMs');
        if (ttm2 !== null) pushCmd('toftransitionms', ttm2);
        var tmax = getInt('tofMaxDistanceMm');
        if (tmax !== null) pushCmd('tofmaxdistancemm', tmax);
        
        // Thermal UI settings
        pushCmd('thermalpollingms', $('thermalPollingMs'));
        pushCmd('thermalpalettedefault', $('thermalPaletteDefault'));
        pushCmd('thermalewmafactor', $('thermalEWMAFactor'));
        pushCmd('thermaltransitionms', $('thermalTransitionMs'));
        pushCmd('thermalwebmaxfps', $('thermalWebMaxFps'));
        
        // ToF UI settings
        pushCmd('tofpollingms', $('tofPollingMs'));
        pushCmd('tofstabilitythreshold', $('tofStabilityThreshold'));
        pushCmd('toftransitionms', $('tofTransitionMs'));
        pushCmd('tofmaxdistancemm', $('tofMaxDistanceMm'));
        
        // IMU UI settings
        pushCmd('imupollingms', $('imuPollingMs'));
        pushCmd('imuewmafactor', $('imuEWMAFactor'));
        pushCmd('imutransitionms', $('imuTransitionMs'));
        pushCmd('imuwebmaxfps', $('imuWebMaxFps'));
        
        if (cmds.length === 0) {
          alert('No Client UI settings to save.');
          return;
        }

        sendSequential(cmds,
          function() {
            try { if (typeof window.refreshSettings === 'function') window.refreshSettings(); } catch(_) {}
            alert('Client UI settings saved.');
          },
          function() { alert('One or more Client UI commands failed.'); }
        );
      } catch(e) {
        alert('Error: ' + e.message);
      }
    };
    
    // Save hardware settings
    // Helper functions for hardware settings
    var getInt = function(id) {
      var el = $(id);
      if (!el) return null;
      var n = parseInt(el.value, 10);
      return isNaN(n) ? null : n;
    };
    var getStr = function(id) {
      var el = $(id);
      if (!el) return null;
      return String(el.value || '');
    };
    var getBool = function(id) {
      var el = $(id);
      if (!el) return null;
      return el.checked ? 1 : 0;
    };
    
    // Save LED settings
    window.saveLEDSettings = function() {
      try {
        var cmds = [];
        var brightness = getInt('ledBrightness');
        if (brightness !== null) cmds.push('hardwareledbrightness ' + brightness);
        var enabled = getBool('ledStartupEnabled');
        if (enabled !== null) cmds.push('hardwareledstartupenabled ' + enabled);
        var effect = getStr('ledStartupEffect');
        if (effect) cmds.push('hardwareledstartupeffect ' + effect);
        var color = getStr('ledStartupColor');
        if (color) cmds.push('hardwareledstartupcolor ' + color);
        var color2 = getStr('ledStartupColor2');
        if (color2) cmds.push('hardwareledstartupcolor2 ' + color2);
        var duration = getInt('ledStartupDuration');
        if (duration !== null) cmds.push('hardwareledstartupduration ' + duration);
        
        if (cmds.length === 0) {
          alert('No LED settings to save.');
          return;
        }

        sendSequential(cmds,
          function() { alert('LED settings saved.'); },
          function() { alert('One or more LED commands failed.'); }
        );
      } catch(e) {
        alert('Error saving LED settings: ' + e.message);
      }
    };
    
    // Save OLED settings
    window.saveOLEDSettings = function() {
      try {
        var cmds = [];
        var oledEnabled = getBool('oledEnabled');
        if (oledEnabled !== null) cmds.push('oledenabled ' + oledEnabled);
        var oledBootMode = getStr('oledBootMode');
        if (oledBootMode) cmds.push('oledbootmode ' + oledBootMode);
        var oledDefaultMode = getStr('oledDefaultMode');
        if (oledDefaultMode) cmds.push('oleddefaultmode ' + oledDefaultMode);
        var oledBootDuration = getInt('oledBootDuration');
        if (oledBootDuration !== null) cmds.push('oledbootduration ' + oledBootDuration);
        var oledUpdateInterval = getInt('oledUpdateInterval');
        if (oledUpdateInterval !== null) cmds.push('oledupdateinterval ' + oledUpdateInterval);
        var oledBrightness = getInt('oledBrightness');
        if (oledBrightness !== null) cmds.push('oledbrightness ' + oledBrightness);
        var oledThermalScale = getStr('oledThermalScale');
        if (oledThermalScale) cmds.push('oledthermalscale ' + oledThermalScale);
        var oledThermalColorMode = getStr('oledThermalColorMode');
        if (oledThermalColorMode) cmds.push('oledthermalcolormode ' + oledThermalColorMode);
        
        if (cmds.length === 0) {
          alert('No OLED settings to save.');
          return;
        }

        sendSequential(cmds,
          function() { alert('OLED settings saved.'); },
          function() { alert('One or more OLED commands failed.'); }
        );
      } catch(e) {
        alert('Error saving OLED settings: ' + e.message);
      }
    };
    
    // Save Gamepad settings
    window.saveGamepadSettings = function() {
      try {
        var cmds = [];
        var gamepadAutoStart = getBool('gamepadAutoStart');
        if (gamepadAutoStart !== null) cmds.push('gamepadautostart ' + (gamepadAutoStart ? 'on' : 'off'));
        
        if (cmds.length === 0) {
          alert('No Gamepad settings to save.');
          return;
        }

        sendSequential(cmds,
          function() { alert('Gamepad settings saved.'); },
          function() { alert('One or more Gamepad commands failed.'); }
        );
      } catch(e) {
        alert('Error saving Gamepad settings: ' + e.message);
      }
    };
    
  } catch(err) {
    console.error('[SETTINGS] Part 3 ERROR:', err);
    alert('Settings page error (Part 3): ' + err.message);
  }
})();
console.log('[SETTINGS] Part 3: Complete');
</script>)SETPART3", HTTPD_RESP_USE_STRLEN);

  // Part 4: JavaScript - WiFi scanning and user management (EXPANDED FOR READABILITY)
  httpd_resp_send_chunk(req, R"SETPART4(<script>
console.log('[SETTINGS] Part 4: WiFi/User management starting...');
(function() {
  try {
    window.scanNetworks = function() {
      console.log('[SETTINGS] scanNetworks called');
      var container = $('wifi-scan-results');
      container.innerHTML = 'Scanning...';
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent('wifiscan json')
      })
      .then(function(r) {
        console.log('[SETTINGS] WiFi scan response:', r.status);
        return r.text();
      })
      .then(function(txt) {
        console.log('[SETTINGS] WiFi scan result length:', txt.length);
        var data = [];
        try {
          data = JSON.parse(txt || '[]');
        } catch(_) {
          try {
            data = JSON.parse((txt || '').substring((txt || '').indexOf('[')));
          } catch(__) {
            data = [];
          }
        }
        if (!Array.isArray(data)) {
          data = [];
        }
        console.log('[SETTINGS] Parsed', data.length, 'networks');
        var hiddenCount = 0, visible = [];
        data.forEach(function(ap) {
          var isHidden = (!ap.ssid || ap.ssid.length === 0 || ap.hidden === true || ap.hidden === 'true');
          if (isHidden) {
            hiddenCount++;
          } else {
            visible.push(ap);
          }
        });
        visible.sort(function(a, b) {
          return (b.rssi || -999) - (a.rssi || -999);
        });
        console.log('[SETTINGS] Visible networks:', visible.length, 'Hidden:', hiddenCount);
        var html = '<div style="margin-top:0.5rem"><strong>Nearby Networks</strong></div>';
        if (hiddenCount > 0) {
          html += '<div style="color:var(--panel-fg);font-size:0.85rem;margin-top:4px">' + hiddenCount + ' hidden network' + (hiddenCount > 1 ? 's' : '') + ' detected</div>';
        }
        if (visible.length === 0) {
          html += '<div style="color:var(--panel-fg)">No networks found.</div>';
        } else {
          html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:0.5rem;margin-top:0.5rem">';
          visible.forEach(function(ap) {
            var ssid = (ap.ssid || '(hidden)');
            var lock = (ap.auth && ap.auth !== '0') ? '[SEC]' : '[OPEN]';
            var rssi = ap.rssi || -999;
            var ch = ap.channel || 0;
            var saved = (__S.state.savedSSIDs || []).indexOf(ssid) !== -1;
            var isCur = (ssid && ssid === __S.state.currentSSID);
            var border = isCur ? '#007bff' : (saved ? '#28a745' : 'var(--border)');
            var badgeTxt = isCur ? '(Connected)' : (saved ? '(Saved)' : '');
            var badgeColor = isCur ? '#007bff' : (saved ? '#28a745' : 'var(--muted)');
            var badge = badgeTxt ? '<span style="color:' + badgeColor + ';font-weight:bold;margin-left:6px">' + badgeTxt + '</span>' : '';
            var esc = encodeURIComponent(ssid);
            var needsPass = (ap.auth && ap.auth !== '0') ? 'true' : 'false';
            var btnCls = isCur ? ' vis-hidden' : '';
            html += '<div style="background:var(--panel-bg);color:var(--panel-fg);border:1px solid ' + border + ';border-radius:6px;padding:0.5rem;display:flex;align-items:center;justify-content:space-between">' + '<div><div style="font-weight:bold">' + ssid + ' ' + badge + '</div><div style="color:var(--panel-fg);font-size:0.85rem">RSSI ' + rssi + ' | CH ' + ch + '</div></div>' + '<button class="btn' + btnCls + '" data-ssid="' + esc + '" data-locked="' + needsPass + '" onclick="(function(b){selectSsid(decodeURIComponent(b.dataset.ssid), b.dataset.locked===\\"true\\");})(this)">Select ' + lock + '</button>' + '</div>';
          });
          html += '</div>';
        }
        html += '<div style="margin-top:0.5rem"><button class="btn" onclick="toggleManualConnect()">Hidden network...</button></div>';
        container.innerHTML = html;
        console.log('[SETTINGS] WiFi scan results rendered');
      })
      .catch(function(e) {
        console.error('[SETTINGS] WiFi scan error:', e);
        container.textContent = 'Scan failed: ' + e.message;
      });
    };
    console.log('[SETTINGS] scanNetworks defined');
    
    window.selectSsid = function(ssid, needsPass) {
      try {
        var p = $('wifi-connect-panel');
        if (p) {
          p.style.display = 'block';
        }
        var s = $('sel-ssid');
        if (s) {
          s.textContent = ssid || '';
        }
        var inp = $('sel-pass');
        if (inp) {
          inp.value = '';
          if (needsPass) {
            inp.placeholder = 'WiFi password';
            inp.disabled = false;
          } else {
            inp.placeholder = '(open network)';
            inp.disabled = false;
          }
        }
      } catch(e) {}
    };
    
    window.toggleManualConnect = function() {
      var p = $('wifi-manual-panel');
      if (!p) return;
      p.style.display = (p.style.display === 'none' || !p.style.display) ? 'block' : 'none';
    };
    
    window.toggleUserDropdown = function(id) {
      var dropdown = $('dropdown-' + id);
      if (!dropdown) return;
      var isVisible = dropdown.style.display === 'block';
      dropdown.style.display = isVisible ? 'none' : 'block';
      if (!isVisible && id.indexOf('-sync') !== -1) {
        var uid = id.replace('-sync', '');
        if (typeof window.refreshSyncPeersFor === 'function') refreshSyncPeersFor(uid);
      }
    };
    
    window.revokeUserSessions = function(username) {
      if (!username || !confirm('Revoke all sessions for user: ' + username + '?')) return;
      var cmd = 'session revoke user ' + username;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) {
        return r.text();
      })
      .then(function(t) {
        alert(t || 'Sessions revoked');
        try {
          refreshUsers();
        } catch(_) {}
      })
      .catch(function(e) {
        alert('Error: ' + e.message);
      });
    };
    
    function formatMillisTimestamp(millis) {
      if (!millis || millis === 0) return 'Unknown';
      var d = new Date(millis);
      if (isNaN(d.getTime())) return 'Invalid';
      return d.toLocaleString();
    }
    
    function cleanIPAddress(ip) {
      if (!ip) return '';
      // Remove IPv6 prefix like ::FFFF: to show clean IPv4
      return ip.replace(/^::ffff:/i, '').replace(/^::FFFF:/i, '');
    }
    
    window.refreshUsers = function() {
      var container = $('users-list');
      if (!container) return;
      container.innerHTML = 'Loading...';
      Promise.all([
        fetch('/api/cli', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          credentials: 'same-origin',
          body: 'cmd=' + encodeURIComponent('user list json')
        }).then(function(r) {
          return r.text();
        }),
        fetch('/api/cli', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          credentials: 'same-origin',
          body: 'cmd=' + encodeURIComponent('session list json')
        }).then(function(r) {
          return r.text();
        }),
        fetch('/api/cli', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          credentials: 'same-origin',
          body: 'cmd=' + encodeURIComponent('pending list json')
        }).then(function(r) {
          return r.text();
        })
      ])
      .then(function(results) {
        var users = [], sessions = [], pending = [];
        try {
          users = JSON.parse(results[0] || '[]');
          sessions = JSON.parse(results[1] || '[]');
          pending = JSON.parse(results[2] || '[]');
        } catch(e) {
          container.innerHTML = '<div style="color:#dc3545">Error parsing data: ' + e.message + '</div>';
          return;
        }
        if (!Array.isArray(users) || !Array.isArray(sessions) || !Array.isArray(pending)) {
          container.innerHTML = '<div style="color:#dc3545">Invalid data format</div>';
          return;
        }
        var sessionsByUser = {};
        sessions.forEach(function(s) {
          var u = s.user || '';
          if (!sessionsByUser[u]) sessionsByUser[u] = [];
          sessionsByUser[u].push(s);
        });
        var html = '<div style="display:grid;gap:0.5rem">';
        if (pending.length > 0) {
          html += '<div style="background:#fff3cd;border:1px solid #ffeaa7;border-radius:4px;padding:0.75rem;margin-bottom:0.5rem">';
          html += '<div style="font-weight:bold;color:#856404;margin-bottom:0.5rem">Pending Approvals</div>';
          pending.forEach(function(pendingUser) {
            var username = pendingUser.username || '';
            var uid = 'u' + Math.random().toString(36).substr(2, 9);
            html += '<div style="margin-bottom:0.25rem">';
            html += '<div onclick="toggleUserDropdown(\'' + uid + '-pending\')" style="display:flex;align-items:center;justify-content:space-between;padding:0.5rem;background:var(--panel-bg);border:1px solid var(--border);border-radius:4px;cursor:pointer">';
            html += '<div><strong>' + username + '</strong> <span style="color:#856404;font-size:0.85rem">(Pending)</span></div>';
            html += '<div style="font-size:0.8rem;color:var(--panel-fg)">▼</div></div>';
            html += '<div id="dropdown-' + uid + '-pending" style="display:none;margin-top:0.25rem;padding:0.5rem;background:var(--crumb-bg);border:1px solid var(--border);border-radius:4px">';
            html += '<div style="display:flex;gap:0.5rem;flex-wrap:wrap">';
            html += '<button class="btn" data-user="' + username + '" onclick="approveUserByName(this.dataset.user); toggleUserDropdown(\'' + uid + '-pending\')" style="width:100%;margin-bottom:0.25rem;font-size:0.8rem;padding:0.25rem 0.5rem" title="Approve user">Approve</button>';
            html += '<button class="btn" data-user="' + username + '" onclick="denyUserByName(this.dataset.user); toggleUserDropdown(\'' + uid + '-pending\')" style="width:100%;font-size:0.8rem;padding:0.25rem 0.5rem" title="Deny user">Deny</button>';
            html += '</div></div></div>';
          });
          html += '</div>';
        }
        users.forEach(function(user) {
          var username = user.username || '';
          var isAdmin = user.isAdmin || false;
          var userSessions = sessionsByUser[username] || [];
          var sessionCount = userSessions.length;
          var uid = 'u' + Math.random().toString(36).substr(2, 9);
          html += '<div style="margin-bottom:0.25rem">';
          html += '<div onclick="toggleUserDropdown(\'' + uid + '\')" style="display:flex;align-items:center;justify-content:space-between;padding:0.5rem;background:var(--panel-bg);border:1px solid var(--border);border-radius:4px;cursor:pointer">';
          html += '<div><strong>' + username + '</strong> ' + (isAdmin ? '<span style="color:#667eea;font-size:0.85rem">(Admin)</span>' : '<span style="color:#667eea;font-size:0.85rem">(User)</span>') + ' <span style="color:var(--panel-fg);font-size:0.85rem">' + sessionCount + ' session' + (sessionCount !== 1 ? 's' : '') + '</span></div>';
          html += '<div style="font-size:0.8rem;color:var(--panel-fg)">▼</div></div>';
          html += '<div id="dropdown-' + uid + '" style="display:none;margin-top:0.25rem;padding:0.5rem;background:var(--crumb-bg);border:1px solid var(--border);border-radius:4px">';
          if (sessionCount > 0) {
            html += '<div style="margin-bottom:0.5rem;font-size:0.9rem;color:var(--panel-fg)"><strong>Active Sessions:</strong></div>';
            userSessions.forEach(function(session) {
              var ip = cleanIPAddress(session.ip || '');
              var created = session.createdAt ? formatMillisTimestamp(session.createdAt) : '';
              var lastSeen = session.lastSeen ? formatMillisTimestamp(session.lastSeen) : '';
              var current = session.current || false;
              html += '<div style="background:var(--panel-bg);border:1px solid var(--border);border-radius:4px;padding:0.5rem;margin-bottom:0.25rem;font-size:0.85rem">';
              html += '<div><strong>IP:</strong> ' + ip + ' ' + (current ? '<span style="color:#28a745;font-weight:bold">(Current)</span>' : '') + '</div>';
              if (created) html += '<div><strong>Created:</strong> ' + created + '</div>';
              if (lastSeen) html += '<div><strong>Last Seen:</strong> ' + lastSeen + '</div>';
              html += '</div>';
            });
          }
          html += '<div style="display:flex;gap:0.5rem;flex-wrap:wrap">';
          if (!isAdmin) {
            html += '<button class="btn" data-user="' + username + '" onclick="promoteUserByName(this.dataset.user)" title="Promote to admin">Promote</button>';
          } else {
            html += '<button class="btn" data-user="' + username + '" onclick="demoteUserByName(this.dataset.user)" title="Demote from admin">Demote</button>';
          }
          html += '<button class="btn" data-user="' + username + '" onclick="resetUserPassword(this.dataset.user)" title="Reset password for this user">Reset Password</button>';
          html += '<button class="btn" data-user="' + username + '" onclick="deleteUserByName(this.dataset.user)" title="Delete this user">Delete</button>';
          if (sessionCount > 0) {
            html += '<button class="btn" data-user="' + username + '" onclick="revokeUserSessions(this.dataset.user)" title="Revoke all sessions for this user">Revoke Sessions</button>';
          }
          var hasEspNow = (__S && __S.features && __S.features.espnow === true);
          if (hasEspNow) {
            html += '<button class="btn" data-user="' + username + '" onclick="toggleUserDropdown(\'' + uid + '-sync\')" title="Sync this user to another device over ESP-NOW">Sync via ESP-NOW</button>';
          }
          html += '</div>';
          if (hasEspNow) {
            html += '<div id="dropdown-' + uid + '-sync" style="display:none;margin-top:0.5rem;padding:0.75rem;background:var(--panel-bg);border:1px solid var(--border);border-radius:6px">';
            html += '<div style="font-weight:bold;font-size:0.9rem;color:var(--panel-fg);margin-bottom:0.5rem">Sync \'' + username + '\' to device</div>';
            html += '<div style="display:grid;grid-template-columns:1fr auto;gap:0.5rem;align-items:end;margin-bottom:0.5rem">';
            html += '<div><label style="display:block;font-size:0.85rem;color:var(--muted);margin-bottom:0.25rem">Target Device</label>';
            html += '<select id="sync-device-' + uid + '" style="width:100%"><option value="">Loading...</option></select></div>';
            html += '<button class="btn" onclick="refreshSyncPeersFor(\'' + uid + '\')" title="Refresh peer list">&#8635;</button>';
            html += '</div>';
            html += '<div style="display:grid;grid-template-columns:1fr 1fr;gap:0.5rem;margin-bottom:0.5rem">';
            html += '<div><label style="display:block;font-size:0.85rem;color:var(--muted);margin-bottom:0.25rem">Your Admin Password</label>';
            html += '<input type="password" id="sync-admin-pass-' + uid + '" placeholder="Admin password" style="width:100%;box-sizing:border-box"></div>';
            html += '<div><label style="display:block;font-size:0.85rem;color:var(--muted);margin-bottom:0.25rem">Password for ' + username + '</label>';
            html += '<input type="password" id="sync-user-pass-' + uid + '" placeholder="User\'s password" style="width:100%;box-sizing:border-box"></div>';
            html += '</div>';
            html += '<button class="btn" data-uid="' + uid + '" data-user="' + username + '" onclick="syncUserToDeviceFor(this.dataset.uid,this.dataset.user)" title="Send sync">Sync</button>';
            html += '</div>';
          }
          html += '</div></div>';
        });
        html += '</div>';
        container.innerHTML = html;
      })
      .catch(function(e) {
        container.innerHTML = '<div style="color:#dc3545">Error loading users: ' + e.message + '</div>';
      });
    };
    
    window.promoteUserByName = function(username) {
      if (!username) {
        alert('Username required');
        return;
      }
      if (!confirm('Promote user "' + username + '" to admin?')) {
        return;
      }
      var cmd = 'user promote ' + username;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) {
        return r.text();
      })
      .then(function(t) {
        if (t.indexOf('Error') >= 0) {
          alert('Error: ' + t);
        } else {
          alert(t);
          try {
            refreshUsers();
          } catch(_) {}
        }
      })
      .catch(function(e) {
        alert('Error: ' + e.message);
      });
    };
    
    window.approveUserByName = function(username) {
      if (!username) {
        alert('Username required');
        return;
      }
      if (!confirm('Approve user "' + username + '"?')) {
        return;
      }
      var cmd = 'user approve ' + username;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) {
        return r.text();
      })
      .then(function(t) {
        if (t.indexOf('Error') >= 0) {
          alert('Error: ' + t);
        } else {
          alert(t);
          try {
            refreshUsers();
          } catch(_) {}
        }
      })
      .catch(function(e) {
        alert('Error: ' + e.message);
      });
    };
    
    window.denyUserByName = function(username) {
      if (!username) {
        alert('Username required');
        return;
      }
      if (!confirm('Deny user "' + username + '"? This will permanently reject their registration.')) {
        return;
      }
      var cmd = 'user deny ' + username;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) {
        return r.text();
      })
      .then(function(t) {
        if (t.indexOf('Error') >= 0) {
          alert('Error: ' + t);
        } else {
          alert(t);
          try {
            refreshUsers();
          } catch(_) {}
        }
      })
      .catch(function(e) {
        alert('Error: ' + e.message);
      });
    };
    
    window.demoteUserByName = function(username) {
      if (!username) {
        alert('Username required');
        return;
      }
      if (!confirm('Demote admin user "' + username + '" to regular user?')) {
        return;
      }
      var cmd = 'user demote ' + username;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) {
        return r.text();
      })
      .then(function(t) {
        if (t.indexOf('Error') >= 0) {
          alert('Error: ' + t);
        } else {
          alert(t);
          try {
            refreshUsers();
          } catch(_) {}
        }
      })
      .catch(function(e) {
        alert('Error: ' + e.message);
      });
    };
    
    window.deleteUserByName = function(username) {
      if (!username) {
        alert('Username required');
        return;
      }
      if (!confirm('Delete user "' + username + '"? This action cannot be undone.')) {
        return;
      }
      var cmd = 'user delete ' + username;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) {
        return r.text();
      })
      .then(function(t) {
        if (t.indexOf('Error') >= 0) {
          alert('Error: ' + t);
        } else {
          alert(t);
          try {
            refreshUsers();
          } catch(_) {}
        }
      })
      .catch(function(e) {
        alert('Error: ' + e.message);
      });
    };
    
    window.toggleSerialAuth = function() {
      var current = $('serial-auth-value') && $('serial-auth-value').textContent === 'Required';
      var val = current ? 0 : 1;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent('serialrequireauth ' + val)
      }).then(function(r) { return r.text(); })
      .then(function() {
        var el = $('serial-auth-value');
        if (el) { el.textContent = val ? 'Required' : 'Disabled'; el.style.color = val ? '#28a745' : '#dc3545'; }
        var btn = $('serial-auth-btn');
        if (btn) btn.textContent = val ? 'Disable' : 'Enable';
      });
    };
    
    window.toggleBleAuth = function() {
      var current = $('ble-auth-value') && $('ble-auth-value').textContent === 'Required';
      var val = current ? 0 : 1;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent('blerequireauth ' + val)
      }).then(function(r) { return r.text(); })
      .then(function() {
        var el = $('ble-auth-value');
        if (el) { el.textContent = val ? 'Required' : 'Disabled'; el.style.color = val ? '#28a745' : '#dc3545'; }
        var btn = $('ble-auth-btn');
        if (btn) btn.textContent = val ? 'Disable' : 'Enable';
      });
    };
    
    window.toggleDisplayAuth = function() {
      var current = $('display-auth-value') && $('display-auth-value').textContent === 'Required';
      var val = current ? 0 : 1;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent('oledrequireauth ' + val)
      }).then(function(r) { return r.text(); })
      .then(function() {
        var el = $('display-auth-value');
        if (el) { el.textContent = val ? 'Required' : 'Disabled'; el.style.color = '#667eea'; }
        var btn = $('display-auth-btn');
        if (btn) btn.textContent = val ? 'Disable' : 'Enable';
      });
    };
    
    window.toggleUserSync = function() {
      var current = $('usersync-enabled-value') && $('usersync-enabled-value').textContent === 'Enabled';
      var cmd = current ? 'espnow usersync off' : 'espnow usersync on';
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      }).then(function(r) { return r.text(); })
      .then(function() {
        var nowEnabled = !current;
        var el = $('usersync-enabled-value');
        if (el) { el.textContent = nowEnabled ? 'Enabled' : 'Disabled'; el.style.color = '#667eea'; }
        var btn = $('usersync-enabled-btn');
        if (btn) btn.textContent = nowEnabled ? 'Disable' : 'Enable';
        var form = $('usersync-form');
        if (form) form.style.display = nowEnabled ? 'block' : 'none';
        if (nowEnabled) { refreshSyncUsers(); refreshSyncPeers(); }
      });
    };
    
    window.refreshSyncUsers = function() {
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent('user list json')
      }).then(function(r) { return r.text(); })
      .then(function(t) {
        var users = [];
        try { users = JSON.parse(t); } catch(e) {}
        var sel = $('usersync-user');
        if (!sel) return;
        sel.innerHTML = '';
        if (!Array.isArray(users) || !users.length) {
          sel.innerHTML = '<option value="">No users found</option>';
          return;
        }
        users.forEach(function(u) {
          var opt = document.createElement('option');
          opt.value = u.username || '';
          opt.textContent = (u.username || '') + (u.isAdmin ? ' (Admin)' : ' (User)');
          sel.appendChild(opt);
        });
      });
    };
    
    window.refreshSyncPeers = function() {
      var sel = $('usersync-device');
      if (!sel) return;
      sel.innerHTML = '<option value="">Loading...</option>';
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent('espnow devices')
      }).then(function(r) { return r.text(); })
      .then(function(t) {
        var peers = [];
        var lines = t.split('\n');
        for (var i = 0; i < lines.length; i++) {
          var line = lines[i];
          if (line.length > 2 && line[0] === ' ' && line[1] === ' ' && line[2] !== '.' && line[2] !== '(') {
            var name = line.trim().split(/[\s[\(]/)[0];
            if (name && name.length > 0) peers.push(name);
          }
        }
        sel.innerHTML = '';
        if (!peers.length) {
          sel.innerHTML = '<option value="">No peers found</option>';
          return;
        }
        peers.forEach(function(name) {
          var opt = document.createElement('option');
          opt.value = name;
          opt.textContent = name;
          sel.appendChild(opt);
        });
      });
    };
    
    window.syncUserToDevice = function() {
      var username = $('usersync-user') ? $('usersync-user').value : '';
      var device = $('usersync-device') ? $('usersync-device').value : '';
      var adminPass = $('usersync-admin-password') ? $('usersync-admin-password').value : '';
      var userPass = $('usersync-user-password') ? $('usersync-user-password').value : '';
      if (!username || !device || !adminPass || !userPass) {
        alert('Please select a user, select a device, and enter both passwords');
        return;
      }
      var cmd = 'user sync ' + username + ' ' + device + ' ' + adminPass + ' ' + userPass;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      }).then(function(r) { return r.text(); })
      .then(function(t) {
        alert(t || 'Sync complete');
        if ($('usersync-admin-password')) $('usersync-admin-password').value = '';
        if ($('usersync-user-password')) $('usersync-user-password').value = '';
      })
      .catch(function(e) { alert('Error: ' + e.message); });
    };

    window.refreshSyncPeersFor = function(uid) {
      var sel = $('sync-device-' + uid);
      if (!sel) return;
      sel.innerHTML = '<option value="">Loading...</option>';
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent('espnow devices')
      }).then(function(r) { return r.text(); })
      .then(function(t) {
        var peers = [];
        var lines = t.split('\n');
        for (var i = 0; i < lines.length; i++) {
          var line = lines[i];
          if (line.length > 2 && line[0] === ' ' && line[1] === ' ' && line[2] !== '.' && line[2] !== '(') {
            var name = line.trim().split(/[\s[(]/)[0];
            if (name && name.length > 0) peers.push(name);
          }
        }
        sel.innerHTML = '';
        if (!peers.length) {
          sel.innerHTML = '<option value="">No peers found</option>';
          return;
        }
        peers.forEach(function(name) {
          var opt = document.createElement('option');
          opt.value = name;
          opt.textContent = name;
          sel.appendChild(opt);
        });
      });
    };

    window.syncUserToDeviceFor = function(uid, username) {
      var device = $('sync-device-' + uid) ? $('sync-device-' + uid).value : '';
      var adminPass = $('sync-admin-pass-' + uid) ? $('sync-admin-pass-' + uid).value : '';
      var userPass = $('sync-user-pass-' + uid) ? $('sync-user-pass-' + uid).value : '';
      if (!device || !adminPass || !userPass) {
        alert('Please select a device and enter both passwords');
        return;
      }
      var cmd = 'user sync ' + username + ' ' + device + ' ' + adminPass + ' ' + userPass;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      }).then(function(r) { return r.text(); })
      .then(function(t) {
        alert(t || 'Sync complete');
        if ($('sync-admin-pass-' + uid)) $('sync-admin-pass-' + uid).value = '';
        if ($('sync-user-pass-' + uid)) $('sync-user-pass-' + uid).value = '';
      })
      .catch(function(e) { alert('Error: ' + e.message); });
    };

    window.resetUserPassword = async function(username) {
      if (!username) {
        await hwAlert('Username required');
        return;
      }
      var newPassword = await hwPrompt('Enter new password for user "' + username + '" (minimum 6 characters):');
      if (!newPassword) {
        return;
      }
      if (newPassword.length < 6) {
        await hwAlert('Password must be at least 6 characters');
        return;
      }
      var confirmPassword = await hwPrompt('Confirm new password for "' + username + '":');
      if (newPassword !== confirmPassword) {
        await hwAlert('Passwords do not match');
        return;
      }
      var cmd = 'user resetpassword ' + username + ' ' + newPassword;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) {
        return r.text();
      })
      .then(function(t) {
        if (t.indexOf('Error') >= 0) {
          alert('Error: ' + t);
        } else {
          alert(t);
        }
      })
      .catch(function(e) {
        alert('Error: ' + e.message);
      });
    };
    
  } catch(err) {
    console.error('[SETTINGS] Part 12 ERROR:', err);
    alert('Settings page error (Part 12): ' + err.message);
  }
})();
console.log('[SETTINGS] Part 4: Complete');
console.log('[SETTINGS] All parts loaded successfully');
</script>)SETPART4", HTTPD_RESP_USE_STRLEN);
}

// Legacy function removed - now using streamSettingsInner() for efficient streaming
#endif
