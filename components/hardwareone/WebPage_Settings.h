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
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Web CLI History Size</div><div style='color:var(--panel-fg);font-size:0.9rem'>Number of commands to keep in web CLI history buffer.</div></div>
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
    var statusBadge = isDisconnected ? '<span style="background:#f59e0b;color:#fff;padding:0.15rem 0.5rem;border-radius:3px;font-size:0.7rem;margin-left:0.5rem;font-weight:500">Disconnected</span>' : '';
    
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
      html += '<div style="background:#fef3c7;border-left:3px solid #f59e0b;padding:0.75rem;margin-bottom:1rem;color:#92400e;font-size:0.85rem">';
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
      statusBadge = '<span style="background:#f59e0b;color:#fff;padding:0.15rem 0.5rem;border-radius:3px;font-size:0.7rem;margin-left:0.5rem;font-weight:500">Disconnected</span>';
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
      html += '<div style="background:#fef3c7;border-left:3px solid #f59e0b;padding:0.75rem;margin-bottom:1rem;color:#92400e;font-size:0.85rem">';
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
          <label><input type='checkbox' id='oledAutoInit' style='margin-right:0.5rem'>Auto-Initialize on Boot</label>
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

  // Part 7: Debug Controls section (large - split into columns)
  httpd_resp_send_chunk(req, R"SETPART7(
<div class='settings-panel'>
  <style>
    /* Table-like alignment for all debug control rows */
    #debug-pane div[style*='display:flex'][style*='align-items:center'] {
      display: grid;
      grid-template-columns: 1fr auto auto;
      column-gap: 0.5rem;
      align-items: center;
    }
    #debug-pane div[style*='display:flex'][style*='align-items:center'] > span {
      /* Allow text to wrap instead of truncating with ellipsis */
      word-break: break-word;
      line-height: 1.3;
    }
    #debug-pane span[id$='-value'] { 
      display: inline-block; 
      min-width: 72px; 
      text-align: right; 
    }
    #debug-pane .btn { 
      min-width: 92px; 
      white-space: nowrap; 
    }
    /* Child detail rows get single-column grid for single toggle button */
    #debug-pane div[id$='-details'] div[style*='display:flex'][style*='align-items:center'] {
      grid-template-columns: 1fr auto;
    }
  </style>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Debug Controls</div><div style='color:var(--panel-fg);font-size:0.9rem'>Toggle debugging output categories to serial console. Hover over each sub-option to read about each option.</div></div>
    <button class='btn' id='btn-debug-toggle' onclick="togglePane('debug-pane','btn-debug-toggle')">Expand</button>
  </div>
  <div id='debug-pane' style='display:none;margin-top:0.75rem'>
  <!-- Global Log Level Control -->
  <div style='background:var(--panel-bg);border:2px solid #667eea;border-radius:6px;padding:1rem;margin-bottom:1rem'>
    <div style='font-weight:bold;margin-bottom:0.5rem;color:var(--panel-fg)'>Global Log Level</div>
    <div style='color:var(--panel-fg);font-size:0.9rem;margin-bottom:0.75rem'>Controls minimum severity for all debug output below. Error (least verbose) to Debug (most verbose).</div>
    <div style='display:flex;align-items:center;gap:0.5rem'>
      <span style='color:var(--panel-fg)' title='Severity-based log level (errors always visible; this controls WARN/INFO/DEBUG visibility).'>Current Level: <span style='font-weight:bold;color:#667eea' id='logLevel-value'>-</span></span>
      <select id='logLevel-select' class='form-input' style='width:180px'>
        <option value='0'>Error (Least)</option>
        <option value='1'>Warn</option>
        <option value='2'>Info</option>
        <option value='3'>Debug (Most)</option>
      </select>
      <button class='btn' onclick="updateLogLevel()" id='logLevel-btn' title='Apply log level'>Apply</button>
    </div>
  </div>
  <!-- Debug Category Toggles -->
  <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:1rem'>
    <div style='background:var(--panel-bg);border:1px solid var(--border);border-radius:6px;padding:1rem'>
      <div style='font-weight:bold;margin-bottom:0.75rem;color:var(--panel-fg);border-bottom:1px solid #e5e7eb;padding-bottom:0.5rem'>System & Core</div>
      <div style='display:flex;flex-direction:column;gap:0.35rem'>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable all authentication debug output (derived state: ON if any sub-category is ON)'>Authentication: <span style='font-weight:bold;color:#667eea' id='debugAuthGroup-value'>-</span></span><button class='btn' onclick="toggleDebugGroup('auth')" id='debugAuthGroup-btn' title='Toggle all auth debugging'>Toggle All</button><button class='btn' onclick="togglePane('auth-details','auth-details-btn')" id='auth-details-btn' style='font-size:0.85rem'>Expand</button></div>
        <div id='auth-details' style='display:none;margin-left:1rem;margin-top:0.5rem;padding-left:0.75rem;border-left:2px solid #e5e7eb'>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Master switch for all authentication-related debug output'>Parent: <span style='font-weight:bold;color:#667eea' id='debugAuth-value'>-</span></span><button class='btn' onclick="toggleDebug('debugAuth')" id='debugAuth-btn' style='font-size:0.85rem' title='Toggle all authentication debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows unique boot identifier generation and validation for device tracking'>Boot ID: <span style='font-weight:bold;color:#667eea' id='debugAuthBootId-value'>-</span></span><button class='btn' onclick="toggleDebug('debugAuthBootId')" id='debugAuthBootId-btn' style='font-size:0.85rem' title='Toggle boot ID debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows HTTP cookie creation, parsing, and validation for session management'>Cookies: <span style='font-weight:bold;color:#667eea' id='debugAuthCookies-value'>-</span></span><button class='btn' onclick="toggleDebug('debugAuthCookies')" id='debugAuthCookies-btn' style='font-size:0.85rem' title='Toggle cookie debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows login attempts, password verification, and authentication flow'>Login: <span style='font-weight:bold;color:#667eea' id='debugAuthLogin-value'>-</span></span><button class='btn' onclick="toggleDebug('debugAuthLogin')" id='debugAuthLogin-btn' style='font-size:0.85rem' title='Toggle login debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows user session creation, validation, expiration, and cleanup'>Sessions: <span style='font-weight:bold;color:#667eea' id='debugAuthSessions-value'>-</span></span><button class='btn' onclick="toggleDebug('debugAuthSessions')" id='debugAuthSessions-btn' style='font-size:0.85rem' title='Toggle session debugging'>Toggle</button></div>
        </div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable all automations debug output (derived state: ON if any sub-category is ON)'>Automations: <span style='font-weight:bold;color:#667eea' id='debugAutomationsGroup-value'>-</span></span><button class='btn' onclick="toggleDebugGroup('automations')" id='debugAutomationsGroup-btn' title='Toggle all automations debugging'>Toggle All</button><button class='btn' onclick="togglePane('automations-details','automations-details-btn')" id='automations-details-btn' style='font-size:0.85rem'>Expand</button></div>
        <div id='automations-details' style='display:none;margin-left:1rem;margin-top:0.5rem;padding-left:0.75rem;border-left:2px solid #e5e7eb'>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable automations conditions debug output'>Conditions: <span style='font-weight:bold;color:#667eea' id='debugAutoCondition-value'>-</span></span><button class='btn' onclick="toggleDebug('debugAutoCondition')" id='debugAutoCondition-btn' style='font-size:0.85rem' title='Toggle conditions debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable automations execution debug output'>Execution: <span style='font-weight:bold;color:#667eea' id='debugAutoExec-value'>-</span></span><button class='btn' onclick="toggleDebug('debugAutoExec')" id='debugAutoExec-btn' style='font-size:0.85rem' title='Toggle execution debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable automations scheduler debug output'>Scheduler: <span style='font-weight:bold;color:#667eea' id='debugAutoScheduler-value'>-</span></span><button class='btn' onclick="toggleDebug('debugAutoScheduler')" id='debugAutoScheduler-btn' style='font-size:0.85rem' title='Toggle scheduler debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable automations timing debug output'>Timing: <span style='font-weight:bold;color:#667eea' id='debugAutoTiming-value'>-</span></span><button class='btn' onclick="toggleDebug('debugAutoTiming')" id='debugAutoTiming-btn' style='font-size:0.85rem' title='Toggle timing debugging'>Toggle</button></div>
        </div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable all CLI debug output (derived state: ON if any sub-category is ON)'>CLI: <span style='font-weight:bold;color:#667eea' id='debugCliGroup-value'>-</span></span><button class='btn' onclick="toggleDebugGroup('cli')" id='debugCliGroup-btn' title='Toggle all CLI debugging'>Toggle All</button><button class='btn' onclick="togglePane('cli-details','cli-details-btn')" id='cli-details-btn' style='font-size:0.85rem'>Expand</button></div>
        <div id='cli-details' style='display:none;margin-left:1rem;margin-top:0.5rem;padding-left:0.75rem;border-left:2px solid #e5e7eb'>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable CLI execution debug output'>Execution: <span style='font-weight:bold;color:#667eea' id='debugCliExecution-value'>-</span></span><button class='btn' onclick="toggleDebug('debugCliExecution')" id='debugCliExecution-btn' style='font-size:0.85rem' title='Toggle execution debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable CLI queue debug output'>Queue: <span style='font-weight:bold;color:#667eea' id='debugCliQueue-value'>-</span></span><button class='btn' onclick="toggleDebug('debugCliQueue')" id='debugCliQueue-btn' style='font-size:0.85rem' title='Toggle queue debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable CLI validation debug output'>Validation: <span style='font-weight:bold;color:#667eea' id='debugCliValidation-value'>-</span></span><button class='btn' onclick="toggleDebug('debugCliValidation')" id='debugCliValidation-btn' style='font-size:0.85rem' title='Toggle validation debugging'>Toggle</button></div>
        </div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable all command flow debug output (derived state: ON if any sub-category is ON)'>Command Flow: <span style='font-weight:bold;color:#667eea' id='debugCmdFlowGroup-value'>-</span></span><button class='btn' onclick="toggleDebugGroup('cmdflow')" id='debugCmdFlowGroup-btn' title='Toggle all command flow debugging'>Toggle All</button><button class='btn' onclick="togglePane('cmdflow-details','cmdflow-details-btn')" id='cmdflow-details-btn' style='font-size:0.85rem'>Expand</button></div>
        <div id='cmdflow-details' style='display:none;margin-left:1rem;margin-top:0.5rem;padding-left:0.75rem;border-left:2px solid #e5e7eb'>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable command context debug output'>Context: <span style='font-weight:bold;color:#667eea' id='debugCmdflowContext-value'>-</span></span><button class='btn' onclick="toggleDebug('debugCmdflowContext')" id='debugCmdflowContext-btn' style='font-size:0.85rem' title='Toggle context debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable command queue debug output'>Queue: <span style='font-weight:bold;color:#667eea' id='debugCmdflowQueue-value'>-</span></span><button class='btn' onclick="toggleDebug('debugCmdflowQueue')" id='debugCmdflowQueue-btn' style='font-size:0.85rem' title='Toggle queue debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable command routing debug output'>Routing: <span style='font-weight:bold;color:#667eea' id='debugCmdflowRouting-value'>-</span></span><button class='btn' onclick="toggleDebug('debugCmdflowRouting')" id='debugCmdflowRouting-btn' style='font-size:0.85rem' title='Toggle routing debugging'>Toggle</button></div>
        </div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable modular command registry operations debug output'>Command System: <span style='font-weight:bold;color:#667eea' id='debugCommandSystem-value'>-</span></span><button class='btn' onclick="toggleDebug('debugCommandSystem')" id='debugCommandSystem-btn' title='Toggle command system debugging'>Toggle</button></div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable date/time and NTP debug output'>Date/Time & NTP: <span style='font-weight:bold;color:#667eea' id='debugDateTime-value'>-</span></span><button class='btn' onclick="toggleDebug('debugDateTime')" id='debugDateTime-btn' title='Toggle date/time debugging'>Toggle</button></div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable sensor logger internal diagnostics'>Logger Internals: <span style='font-weight:bold;color:#667eea' id='debugLogger-value'>-</span></span><button class='btn' onclick="toggleDebug('debugLogger')" id='debugLogger-btn' title='Toggle sensor logger diagnostics'>Toggle</button></div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable all performance debug output (derived state: ON if any sub-category is ON)'>Performance: <span style='font-weight:bold;color:#667eea' id='debugPerfGroup-value'>-</span></span><button class='btn' onclick="toggleDebugGroup('performance')" id='debugPerfGroup-btn' title='Toggle all performance debugging'>Toggle All</button><button class='btn' onclick="togglePane('perf-details','perf-details-btn')" id='perf-details-btn' style='font-size:0.85rem'>Expand</button></div>
        <div id='perf-details' style='display:none;margin-left:1rem;margin-top:0.5rem;padding-left:0.75rem;border-left:2px solid #e5e7eb'>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable heap usage debug output'>Heap: <span style='font-weight:bold;color:#667eea' id='debugPerfHeap-value'>-</span></span><button class='btn' onclick="toggleDebug('debugPerfHeap')" id='debugPerfHeap-btn' style='font-size:0.85rem' title='Toggle heap debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable stack watermark debug output'>Stack: <span style='font-weight:bold;color:#667eea' id='debugPerfStack-value'>-</span></span><button class='btn' onclick="toggleDebug('debugPerfStack')" id='debugPerfStack-btn' style='font-size:0.85rem' title='Toggle stack debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable timing debug output'>Timing: <span style='font-weight:bold;color:#667eea' id='debugPerfTiming-value'>-</span></span><button class='btn' onclick="toggleDebug('debugPerfTiming')" id='debugPerfTiming-btn' style='font-size:0.85rem' title='Toggle timing debugging'>Toggle</button></div>
        </div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable settings module registration and validation debug output'>Settings System: <span style='font-weight:bold;color:#667eea' id='debugSettingsSystem-value'>-</span></span><button class='btn' onclick="toggleDebug('debugSettingsSystem')" id='debugSettingsSystem-btn' title='Toggle settings system debugging'>Toggle</button></div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable all storage debug output (derived state: ON if any sub-category is ON)'>Storage: <span style='font-weight:bold;color:#667eea' id='debugStorageGroup-value'>-</span></span><button class='btn' onclick="toggleDebugGroup('storage')" id='debugStorageGroup-btn' title='Toggle all storage debugging'>Toggle All</button><button class='btn' onclick="togglePane('storage-details','storage-details-btn')" id='storage-details-btn' style='font-size:0.85rem'>Expand</button></div>
        <div id='storage-details' style='display:none;margin-left:1rem;margin-top:0.5rem;padding-left:0.75rem;border-left:2px solid #e5e7eb'>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable file operations debug output'>Files: <span style='font-weight:bold;color:#667eea' id='debugStorageFiles-value'>-</span></span><button class='btn' onclick="toggleDebug('debugStorageFiles')" id='debugStorageFiles-btn' style='font-size:0.85rem' title='Toggle files debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable JSON operations debug output'>JSON: <span style='font-weight:bold;color:#667eea' id='debugStorageJson-value'>-</span></span><button class='btn' onclick="toggleDebug('debugStorageJson')" id='debugStorageJson-btn' style='font-size:0.85rem' title='Toggle JSON debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable migration debug output'>Migration: <span style='font-weight:bold;color:#667eea' id='debugStorageMigration-value'>-</span></span><button class='btn' onclick="toggleDebug('debugStorageMigration')" id='debugStorageMigration-btn' style='font-size:0.85rem' title='Toggle migration debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable settings operations debug output'>Settings: <span style='font-weight:bold;color:#667eea' id='debugStorageSettings-value'>-</span></span><button class='btn' onclick="toggleDebug('debugStorageSettings')" id='debugStorageSettings-btn' style='font-size:0.85rem' title='Toggle settings debugging'>Toggle</button></div>
        </div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable all system debug output (derived state: ON if any sub-category is ON)'>System: <span style='font-weight:bold;color:#667eea' id='debugSystemGroup-value'>-</span></span><button class='btn' onclick="toggleDebugGroup('system')" id='debugSystemGroup-btn' title='Toggle all system debugging'>Toggle All</button><button class='btn' onclick="togglePane('system-details','system-details-btn')" id='system-details-btn' style='font-size:0.85rem'>Expand</button></div>
        <div id='system-details' style='display:none;margin-left:1rem;margin-top:0.5rem;padding-left:0.75rem;border-left:2px solid #e5e7eb'>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable system boot debug output'>Boot: <span style='font-weight:bold;color:#667eea' id='debugSystemBoot-value'>-</span></span><button class='btn' onclick="toggleDebug('debugSystemBoot')" id='debugSystemBoot-btn' style='font-size:0.85rem' title='Toggle boot debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable system config debug output'>Config: <span style='font-weight:bold;color:#667eea' id='debugSystemConfig-value'>-</span></span><button class='btn' onclick="toggleDebug('debugSystemConfig')" id='debugSystemConfig-btn' style='font-size:0.85rem' title='Toggle config debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable system hardware debug output'>Hardware: <span style='font-weight:bold;color:#667eea' id='debugSystemHardware-value'>-</span></span><button class='btn' onclick="toggleDebug('debugSystemHardware')" id='debugSystemHardware-btn' style='font-size:0.85rem' title='Toggle hardware debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable system tasks debug output'>Tasks: <span style='font-weight:bold;color:#667eea' id='debugSystemTasks-value'>-</span></span><button class='btn' onclick="toggleDebug('debugSystemTasks')" id='debugSystemTasks-btn' style='font-size:0.85rem' title='Toggle tasks debugging'>Toggle</button></div>
        </div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable all users debug output (derived state: ON if any sub-category is ON)'>Users: <span style='font-weight:bold;color:#667eea' id='debugUsersGroup-value'>-</span></span><button class='btn' onclick="toggleDebugGroup('users')" id='debugUsersGroup-btn' title='Toggle all users debugging'>Toggle All</button><button class='btn' onclick="togglePane('users-details','users-details-btn')" id='users-details-btn' style='font-size:0.85rem'>Expand</button></div>
        <div id='users-details' style='display:none;margin-left:1rem;margin-top:0.5rem;padding-left:0.75rem;border-left:2px solid #e5e7eb'>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable user management debug output'>Management: <span style='font-weight:bold;color:#667eea' id='debugUsersMgmt-value'>-</span></span><button class='btn' onclick="toggleDebug('debugUsersMgmt')" id='debugUsersMgmt-btn' style='font-size:0.85rem' title='Toggle management debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable user query debug output'>Query: <span style='font-weight:bold;color:#667eea' id='debugUsersQuery-value'>-</span></span><button class='btn' onclick="toggleDebug('debugUsersQuery')" id='debugUsersQuery-btn' style='font-size:0.85rem' title='Toggle query debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Enable/disable user registration debug output'>Registration: <span style='font-weight:bold;color:#667eea' id='debugUsersRegister-value'>-</span></span><button class='btn' onclick="toggleDebug('debugUsersRegister')" id='debugUsersRegister-btn' style='font-size:0.85rem' title='Toggle registration debugging'>Toggle</button></div>
        </div>
      </div>
    </div>
    <div style='background:var(--panel-bg);border:1px solid var(--border);border-radius:6px;padding:1rem'>
      <div style='font-weight:bold;margin-bottom:0.75rem;color:var(--panel-fg);border-bottom:1px solid #e5e7eb;padding-bottom:0.5rem'>Network & Web</div>
      <div style='display:flex;flex-direction:column;gap:0.35rem'>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable all WiFi debug output (derived state: ON if any sub-category is ON)'>WiFi: <span style='font-weight:bold;color:#667eea' id='debugWifiGroup-value'>-</span></span><button class='btn' onclick="toggleDebugGroup('wifi')" id='debugWifiGroup-btn' title='Toggle all WiFi debugging'>Toggle All</button><button class='btn' onclick="togglePane('wifi-details','wifi-details-btn')" id='wifi-details-btn' style='font-size:0.85rem'>Expand</button></div>
        <div id='wifi-details' style='display:none;margin-left:1rem;margin-top:0.5rem;padding-left:0.75rem;border-left:2px solid #e5e7eb'>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows WiFi connection attempts, status changes, and reconnection logic'>Connection: <span style='font-weight:bold;color:#667eea' id='debugWifiConnection-value'>-</span></span><button class='btn' onclick="toggleDebug('debugWifiConnection')" id='debugWifiConnection-btn' style='font-size:0.85rem' title='Toggle connection debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows WiFi configuration loading, SSID/password changes, and settings validation'>Config: <span style='font-weight:bold;color:#667eea' id='debugWifiConfig-value'>-</span></span><button class='btn' onclick="toggleDebug('debugWifiConfig')" id='debugWifiConfig-btn' style='font-size:0.85rem' title='Toggle config debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows WiFi network scanning for available access points and signal strength'>Scanning: <span style='font-weight:bold;color:#667eea' id='debugWifiScanning-value'>-</span></span><button class='btn' onclick="toggleDebug('debugWifiScanning')" id='debugWifiScanning-btn' style='font-size:0.85rem' title='Toggle scanning debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows low-level WiFi hardware driver events, power management, and radio state'>Driver: <span style='font-weight:bold;color:#667eea' id='debugWifiDriver-value'>-</span></span><button class='btn' onclick="toggleDebug('debugWifiDriver')" id='debugWifiDriver-btn' style='font-size:0.85rem' title='Toggle driver debugging'>Toggle</button></div>
        </div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable all HTTP debug output (derived state: ON if any sub-category is ON)'>HTTP: <span style='font-weight:bold;color:#667eea' id='debugHttpGroup-value'>-</span></span><button class='btn' onclick="toggleDebugGroup('http')" id='debugHttpGroup-btn' title='Toggle all HTTP debugging'>Toggle All</button><button class='btn' onclick="togglePane('http-details','http-details-btn')" id='http-details-btn' style='font-size:0.85rem'>Expand</button></div>
        <div id='http-details' style='display:none;margin-left:1rem;margin-top:0.5rem;padding-left:0.75rem;border-left:2px solid #e5e7eb'>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows HTTP endpoint registration, handler routing, and URL processing'>Handlers: <span style='font-weight:bold;color:#667eea' id='debugHttpHandlers-value'>-</span></span><button class='btn' onclick="toggleDebug('debugHttpHandlers')" id='debugHttpHandlers-btn' style='font-size:0.85rem' title='Toggle handlers debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows incoming HTTP requests, headers, methods, and query parameters'>Requests: <span style='font-weight:bold;color:#667eea' id='debugHttpRequests-value'>-</span></span><button class='btn' onclick="toggleDebug('debugHttpRequests')" id='debugHttpRequests-btn' style='font-size:0.85rem' title='Toggle requests debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows HTTP response generation, status codes, and content delivery'>Responses: <span style='font-weight:bold;color:#667eea' id='debugHttpResponses-value'>-</span></span><button class='btn' onclick="toggleDebug('debugHttpResponses')" id='debugHttpResponses-btn' style='font-size:0.85rem' title='Toggle responses debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows chunked transfer encoding and large file streaming operations'>Streaming: <span style='font-weight:bold;color:#667eea' id='debugHttpStreaming-value'>-</span></span><button class='btn' onclick="toggleDebug('debugHttpStreaming')" id='debugHttpStreaming-btn' style='font-size:0.85rem' title='Toggle streaming debugging'>Toggle</button></div>
        </div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable all SSE debug output (derived state: ON if any sub-category is ON)'>SSE: <span style='font-weight:bold;color:#667eea' id='debugSseGroup-value'>-</span></span><button class='btn' onclick="toggleDebugGroup('sse')" id='debugSseGroup-btn' title='Toggle all SSE debugging'>Toggle All</button><button class='btn' onclick="togglePane('sse-details','sse-details-btn')" id='sse-details-btn' style='font-size:0.85rem'>Expand</button></div>
        <div id='sse-details' style='display:none;margin-left:1rem;margin-top:0.5rem;padding-left:0.75rem;border-left:2px solid #e5e7eb'>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows Server-Sent Events client connections, keepalives, and disconnections'>Connection: <span style='font-weight:bold;color:#667eea' id='debugSseConnection-value'>-</span></span><button class='btn' onclick="toggleDebug('debugSseConnection')" id='debugSseConnection-btn' style='font-size:0.85rem' title='Toggle connection debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows real-time event messages being sent to connected web clients'>Events: <span style='font-weight:bold;color:#667eea' id='debugSseEvents-value'>-</span></span><button class='btn' onclick="toggleDebug('debugSseEvents')" id='debugSseEvents-btn' style='font-size:0.85rem' title='Toggle events debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows events being broadcast to all connected clients simultaneously'>Broadcast: <span style='font-weight:bold;color:#667eea' id='debugSseBroadcast-value'>-</span></span><button class='btn' onclick="toggleDebug('debugSseBroadcast')" id='debugSseBroadcast-btn' style='font-size:0.85rem' title='Toggle broadcast debugging'>Toggle</button></div>
        </div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable all ESP-NOW debug output (derived state: ON if any sub-category is ON)'>ESP-NOW: <span style='font-weight:bold;color:#667eea' id='debugEspNowGroup-value'>-</span></span><button class='btn' onclick="toggleDebugGroup('espnow')" id='debugEspNowGroup-btn' title='Toggle all ESP-NOW debugging'>Toggle All</button><button class='btn' onclick="togglePane('espnow-details','espnow-details-btn')" id='espnow-details-btn' style='font-size:0.85rem'>Expand</button></div>
        <div id='espnow-details' style='display:none;margin-left:1rem;margin-top:0.5rem;padding-left:0.75rem;border-left:2px solid #e5e7eb'>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Master switch for all ESP-NOW wireless mesh networking debug output'>Parent: <span style='font-weight:bold;color:#667eea' id='debugEspNow-value'>-</span></span><button class='btn' onclick="toggleDebug('debugEspNow')" id='debugEspNow-btn' style='font-size:0.85rem' title='Toggle all ESP-NOW debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows data streaming over ESP-NOW for large transfers and file sharing'>Stream: <span style='font-weight:bold;color:#667eea' id='debugEspNowStream-value'>-</span></span><button class='btn' onclick="toggleDebug('debugEspNowStream')" id='debugEspNowStream-btn' style='font-size:0.85rem' title='Toggle stream debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows core ESP-NOW packet send/receive, peer management, and protocol handling'>Core: <span style='font-weight:bold;color:#667eea' id='debugEspNowCore-value'>-</span></span><button class='btn' onclick="toggleDebug('debugEspNowCore')" id='debugEspNowCore-btn' style='font-size:0.85rem' title='Toggle core debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows message routing and forwarding between devices in the mesh network'>Router: <span style='font-weight:bold;color:#667eea' id='debugEspNowRouter-value'>-</span></span><button class='btn' onclick="toggleDebug('debugEspNowRouter')" id='debugEspNowRouter-btn' style='font-size:0.85rem' title='Toggle router debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows mesh network formation, node discovery, and multi-hop communication'>Mesh: <span style='font-weight:bold;color:#667eea' id='debugEspNowMesh-value'>-</span></span><button class='btn' onclick="toggleDebug('debugEspNowMesh')" id='debugEspNowMesh-btn' style='font-size:0.85rem' title='Toggle mesh debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows network topology mapping, device relationships, and connection graph'>Topology: <span style='font-weight:bold;color:#667eea' id='debugEspNowTopo-value'>-</span></span><button class='btn' onclick="toggleDebug('debugEspNowTopo')" id='debugEspNowTopo-btn' style='font-size:0.85rem' title='Toggle topology debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem;margin-bottom:0.35rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows encryption key exchange, secure pairing, and encrypted message handling'>Encryption: <span style='font-weight:bold;color:#667eea' id='debugEspNowEncryption-value'>-</span></span><button class='btn' onclick="toggleDebug('debugEspNowEncryption')" id='debugEspNowEncryption-btn' style='font-size:0.85rem' title='Toggle encryption debugging'>Toggle</button></div>
          <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg);font-size:0.9rem' title='Shows metadata REQ/RESP/PUSH frames, payload content, and storage into gMeshPeerMeta'>Metadata: <span style='font-weight:bold;color:#667eea' id='debugEspNowMetadata-value'>-</span></span><button class='btn' onclick="toggleDebug('debugEspNowMetadata')" id='debugEspNowMetadata-btn' style='font-size:0.85rem' title='Toggle metadata exchange debugging'>Toggle</button></div>
        </div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Enable/disable memory instrumentation debug output (stack, buffers)'>Memory Instrumentation: <span style='font-weight:bold;color:#667eea' id='debugMemory-value'>-</span></span><button class='btn' onclick="toggleDebug('debugMemory')" id='debugMemory-btn' title='Toggle memory debugging'>Toggle</button></div>
        <div style='display:flex;align-items:center;gap:0.5rem;margin-top:0.5rem'><span style='color:var(--panel-fg)' title='Periodic memory sampling interval in seconds (0 to disable)'>Memory Sample Interval: <span style='font-weight:bold;color:#667eea' id='memorySampleInterval-value'>-</span>s</span><input type='number' id='memorySampleInterval-input' min='0' max='300' step='5' value='30' class='form-input' style='width:80px;margin-left:0.5rem' title='Sampling interval in seconds'><button class='btn' onclick="updateMemorySampleInterval()" id='memorySampleInterval-btn' title='Update memory sample interval'>Update</button></div>
      </div>
    </div>
    <div style='background:var(--panel-bg);border:1px solid var(--border);border-radius:6px;padding:1rem'>
      <div style='font-weight:bold;margin-bottom:0.75rem;color:var(--panel-fg);border-bottom:1px solid #e5e7eb;padding-bottom:0.5rem'>Sensors & Hardware</div>
      <div style='display:flex;flex-direction:column;gap:0.35rem'>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Shows APDS9960 gesture/proximity/color sensor I2C communication and readings'>APDS: <span style='font-weight:bold;color:#667eea' id='debugApds-value'>-</span></span><button class='btn' onclick="toggleDebug('debugApds')" id='debugApds-btn' title='Toggle APDS debugging'>Toggle</button></div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Shows camera initialization, image capture, streaming, and DVP interface operations'>Camera: <span style='font-weight:bold;color:#667eea' id='debugCamera-value'>-</span></span><button class='btn' onclick="toggleDebug('debugCamera')" id='debugCamera-btn' title='Toggle camera debugging'>Toggle</button></div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Shows RDA5807 FM radio tuning, station scanning, RDS decoding, and audio output'>FM Radio: <span style='font-weight:bold;color:#667eea' id='debugFmRadio-value'>-</span></span><button class='btn' onclick="toggleDebug('debugFmRadio')" id='debugFmRadio-btn' title='Toggle FM radio debugging'>Toggle</button></div>
)SETPART7", HTTPD_RESP_USE_STRLEN);
#if ENABLE_G2_GLASSES
  httpd_resp_send_chunk(req, R"SP7G(        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Shows Even Realities G2 smart glasses BLE connection, gesture detection, and display'>G2 Glasses: <span style='font-weight:bold;color:#667eea' id='debugG2-value'>-</span></span><button class='btn' onclick="toggleDebug('debugG2')" id='debugG2-btn' title='Toggle G2 glasses debugging'>Toggle</button></div>
)SP7G", HTTPD_RESP_USE_STRLEN);
#endif
  httpd_resp_send_chunk(req, R"SP7B(        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Shows Adafruit Seesaw gamepad button presses, joystick movements, and I2C communication'>Gamepad: <span style='font-weight:bold;color:#667eea' id='debugGamepad-value'>-</span></span><button class='btn' onclick="toggleDebug('debugGamepad')" id='debugGamepad-btn' title='Toggle gamepad debugging'>Toggle</button></div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Shows PA1010D GPS NMEA sentence parsing, satellite fix status, and location data'>GPS: <span style='font-weight:bold;color:#667eea' id='debugGps-value'>-</span></span><button class='btn' onclick="toggleDebug('debugGps')" id='debugGps-btn' title='Toggle GPS debugging'>Toggle</button></div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Shows BNO055 IMU calibration, orientation, acceleration, and gyroscope readings'>IMU: <span style='font-weight:bold;color:#667eea' id='debugImu-value'>-</span></span><button class='btn' onclick="toggleDebug('debugImu')" id='debugImu-btn' title='Toggle IMU debugging'>Toggle</button></div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Shows PDM microphone I2S audio capture, gain control, and recording operations'>Microphone: <span style='font-weight:bold;color:#667eea' id='debugMicrophone-value'>-</span></span><button class='btn' onclick="toggleDebug('debugMicrophone')" id='debugMicrophone-btn' title='Toggle microphone debugging'>Toggle</button></div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Shows STHS34PF80 IR presence/motion detection, temperature compensation, and thresholds'>Presence: <span style='font-weight:bold;color:#667eea' id='debugPresence-value'>-</span></span><button class='btn' onclick="toggleDebug('debugPresence')" id='debugPresence-btn' title='Toggle presence debugging'>Toggle</button></div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Shows DS3231 precision RTC time synchronization, alarms, and temperature readings'>RTC: <span style='font-weight:bold;color:#667eea' id='debugRtc-value'>-</span></span><button class='btn' onclick="toggleDebug('debugRtc')" id='debugRtc-btn' title='Toggle RTC debugging'>Toggle</button></div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Shows MLX90640 thermal camera 32x24 temperature array capture and interpolation'>Thermal: <span style='font-weight:bold;color:#667eea' id='debugThermal-value'>-</span></span><button class='btn' onclick="toggleDebug('debugThermal')" id='debugThermal-btn' title='Toggle thermal debugging'>Toggle</button></div>
        <div style='display:flex;align-items:center;gap:0.5rem'><span style='color:var(--panel-fg)' title='Shows VL53L4CX time-of-flight distance measurements, ranging modes, and calibration'>ToF: <span style='font-weight:bold;color:#667eea' id='debugTof-value'>-</span></span><button class='btn' onclick="toggleDebug('debugTof')" id='debugTof-btn' title='Toggle ToF debugging'>Toggle</button></div>
      </div>
    </div>
  </div>
  <div style='margin-top:1rem'><button class='btn' onclick="saveDebugSettings()">Save Debug Settings</button></div>
  </div>
</div>
)SP7B", HTTPD_RESP_USE_STRLEN);

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
  </div>
  </div>
</div>
<div style='text-align:center;margin-top:2rem'>
  <button class='btn' onclick='refreshSettings()' title='Reload all settings from device memory'>Refresh Settings</button>
)SETPART8", HTTPD_RESP_USE_STRLEN);

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
        
        var out = (s.output || {}), th = (s.thermal_mlx90640 || {}), tof = (s.tof_vl53l4cx || {}), oled = (s.oled_ssd1306 || {}), led = (s.led || {}), imu = (s.imu_bno055 || {}), i2c = (s.i2c || {}), dbg = (s.debug || {});
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
        // Sensor settings are now handled by dynamic renderer
        var dAuth = (dbg.authCookies !== undefined ? dbg.authCookies : s.debugAuthCookies);
        var dAuthValEl = $('debugAuthCookies-value');
        if (dAuthValEl) {
          dAuthValEl.textContent = dAuth ? 'Enabled' : 'Disabled';
          var dAuthBtnEl = $('debugAuthCookies-btn');
          if (dAuthBtnEl) dAuthBtnEl.textContent = dAuth ? 'Disable' : 'Enable';
        }
        var dHttp = (dbg.http !== undefined ? dbg.http : s.debugHttp);
        var dHttpValEl = $('debugHttp-value');
        if (dHttpValEl) {
          dHttpValEl.textContent = dHttp ? 'Enabled' : 'Disabled';
          var dHttpBtnEl = $('debugHttp-btn');
          if (dHttpBtnEl) dHttpBtnEl.textContent = dHttp ? 'Disable' : 'Enable';
        }
        var dSse = (dbg.sse !== undefined ? dbg.sse : s.debugSse);
        var dSseValEl = $('debugSse-value');
        if (dSseValEl) {
          dSseValEl.textContent = dSse ? 'Enabled' : 'Disabled';
          var dSseBtnEl = $('debugSse-btn');
          if (dSseBtnEl) dSseBtnEl.textContent = dSse ? 'Disable' : 'Enable';
        }
        var dCli = (dbg.cli !== undefined ? dbg.cli : s.debugCli);
        var dCliValEl = $('debugCli-value');
        if (dCliValEl) {
          dCliValEl.textContent = dCli ? 'Enabled' : 'Disabled';
          var dCliBtnEl = $('debugCli-btn');
          if (dCliBtnEl) dCliBtnEl.textContent = dCli ? 'Disable' : 'Enable';
        }
        var dWifi = (dbg.wifi !== undefined ? dbg.wifi : s.debugWifi);
        var dWifiValEl = $('debugWifi-value');
        if (dWifiValEl) {
          dWifiValEl.textContent = dWifi ? 'Enabled' : 'Disabled';
          var dWifiBtnEl = $('debugWifi-btn');
          if (dWifiBtnEl) dWifiBtnEl.textContent = dWifi ? 'Disable' : 'Enable';
        }
        var dStorage = (dbg.storage !== undefined ? dbg.storage : s.debugStorage);
        var dStorageValEl = $('debugStorage-value');
        if (dStorageValEl) {
          dStorageValEl.textContent = dStorage ? 'Enabled' : 'Disabled';
          var dStorageBtnEl = $('debugStorage-btn');
          if (dStorageBtnEl) dStorageBtnEl.textContent = dStorage ? 'Disable' : 'Enable';
        }
        var dPerformance = (dbg.performance !== undefined ? dbg.performance : s.debugPerformance);
        var dPerfValEl = $('debugPerformance-value');
        if (dPerfValEl) {
          dPerfValEl.textContent = dPerformance ? 'Enabled' : 'Disabled';
          var dPerfBtnEl = $('debugPerformance-btn');
          if (dPerfBtnEl) dPerfBtnEl.textContent = dPerformance ? 'Disable' : 'Enable';
        }
        var dSystem = (dbg.system !== undefined ? dbg.system : s.debugSystem);
        var dSEl = $('debugSystem-value');
        if (dSEl) {
          dSEl.textContent = dSystem ? 'Enabled' : 'Disabled';
          var sbtn = $('debugSystem-btn');
          if (sbtn) {
            sbtn.textContent = dSystem ? 'Disable' : 'Enable';
          }
        }
        var dAutomations = (dbg.automations !== undefined ? dbg.automations : s.debugAutomations);
        var dAEl = $('debugAutomations-value');
        if (dAEl) {
          dAEl.textContent = dAutomations ? 'Enabled' : 'Disabled';
          var abtn = $('debugAutomations-btn');
          if (abtn) {
            abtn.textContent = dAutomations ? 'Disable' : 'Enable';
          }
        }
        var dLogger = (dbg.logger !== undefined ? dbg.logger : s.debugLogger);
        var dLEl = $('debugLogger-value');
        if (dLEl) {
          dLEl.textContent = dLogger ? 'Enabled' : 'Disabled';
          var lbtn = $('debugLogger-btn');
          if (lbtn) {
            lbtn.textContent = dLogger ? 'Disable' : 'Enable';
          }
        }
        var dCmdFlow = (dbg.cmdFlow !== undefined ? dbg.cmdFlow : s.debugCommandFlow);
        var dCFEl = $('debugCommandFlow-value');
        if (dCFEl) {
          dCFEl.textContent = dCmdFlow ? 'Enabled' : 'Disabled';
          var btn = $('debugCommandFlow-btn');
          if (btn) {
            btn.textContent = dCmdFlow ? 'Disable' : 'Enable';
          }
        }
        var dUsers = (dbg.users !== undefined ? dbg.users : s.debugUsers);
        var dUEl = $('debugUsers-value');
        if (dUEl) {
          dUEl.textContent = dUsers ? 'Enabled' : 'Disabled';
          var ubtn = $('debugUsers-btn');
          if (ubtn) {
            ubtn.textContent = dUsers ? 'Disable' : 'Enable';
          }
        }
        var dDateTime = (dbg.dateTime !== undefined ? dbg.dateTime : s.debugDateTime);
        $('debugDateTime-value').textContent = dDateTime ? 'Enabled' : 'Disabled';
        $('debugDateTime-btn').textContent = dDateTime ? 'Disable' : 'Enable';
        // Get individual sensor debug flags (defined later in the code)
        var dGps = (dbg.gps !== undefined ? dbg.gps : s.debugGps);
        var dRtc = (dbg.rtc !== undefined ? dbg.rtc : s.debugRtc);
        var dImu = (dbg.imu !== undefined ? dbg.imu : s.debugImu);
        var dThermal = (dbg.thermal !== undefined ? dbg.thermal : s.debugThermal);
        var dTof = (dbg.tof !== undefined ? dbg.tof : s.debugTof);
        var dGamepad = (dbg.gamepad !== undefined ? dbg.gamepad : s.debugGamepad);
        var dApds = (dbg.apds !== undefined ? dbg.apds : s.debugApds);
        var dPresence = (dbg.presence !== undefined ? dbg.presence : s.debugPresence);
        var dCamera = (dbg.camera !== undefined ? dbg.camera : s.debugCamera);
        var dMic = (dbg.microphone !== undefined ? dbg.microphone : s.debugMicrophone);
        var dFmRadio = (dbg.fmRadio !== undefined ? dbg.fmRadio : s.debugFmRadio);
        var dG2 = (dbg.g2 !== undefined ? dbg.g2 : s.debugG2);
        var dESN = (dbg.espNowStream !== undefined ? dbg.espNowStream : s.debugEspNowStream);
        $('debugEspNowStream-value').textContent = dESN ? 'Enabled' : 'Disabled';
        $('debugEspNowStream-btn').textContent = dESN ? 'Disable' : 'Enable';
        var dESNCore = (dbg.espNowCore !== undefined ? dbg.espNowCore : s.debugEspNowCore);
        $('debugEspNowCore-value').textContent = dESNCore ? 'Enabled' : 'Disabled';
        $('debugEspNowCore-btn').textContent = dESNCore ? 'Disable' : 'Enable';
        var dESNRouter = (dbg.espNowRouter !== undefined ? dbg.espNowRouter : s.debugEspNowRouter);
        $('debugEspNowRouter-value').textContent = dESNRouter ? 'Enabled' : 'Disabled';
        $('debugEspNowRouter-btn').textContent = dESNRouter ? 'Disable' : 'Enable';
        var dESNMesh = (dbg.espNowMesh !== undefined ? dbg.espNowMesh : s.debugEspNowMesh);
        $('debugEspNowMesh-value').textContent = dESNMesh ? 'Enabled' : 'Disabled';
        $('debugEspNowMesh-btn').textContent = dESNMesh ? 'Disable' : 'Enable';
        var dESNTopo = (dbg.espNowTopo !== undefined ? dbg.espNowTopo : s.debugEspNowTopo);
        $('debugEspNowTopo-value').textContent = dESNTopo ? 'Enabled' : 'Disabled';
        $('debugEspNowTopo-btn').textContent = dESNTopo ? 'Disable' : 'Enable';
        var dESNEncryption = (dbg.espNowEncryption !== undefined ? dbg.espNowEncryption : s.debugEspNowEncryption);
        $('debugEspNowEncryption-value').textContent = dESNEncryption ? 'Enabled' : 'Disabled';
        $('debugEspNowEncryption-btn').textContent = dESNEncryption ? 'Disable' : 'Enable';
        var dESNMetadata = (dbg.espNowMetadata !== undefined ? dbg.espNowMetadata : s.debugEspNowMetadata);
        $('debugEspNowMetadata-value').textContent = dESNMetadata ? 'Enabled' : 'Disabled';
        $('debugEspNowMetadata-btn').textContent = dESNMetadata ? 'Disable' : 'Enable';
        var dAutoSched = (dbg.autoScheduler !== undefined ? dbg.autoScheduler : s.debugAutoScheduler);
        $('debugAutoScheduler-value').textContent = dAutoSched ? 'Enabled' : 'Disabled';
        $('debugAutoScheduler-btn').textContent = dAutoSched ? 'Disable' : 'Enable';
        var dAutoExec = (dbg.autoExec !== undefined ? dbg.autoExec : s.debugAutoExec);
        $('debugAutoExec-value').textContent = dAutoExec ? 'Enabled' : 'Disabled';
        $('debugAutoExec-btn').textContent = dAutoExec ? 'Disable' : 'Enable';
        var dAutoCond = (dbg.autoCondition !== undefined ? dbg.autoCondition : s.debugAutoCondition);
        $('debugAutoCondition-value').textContent = dAutoCond ? 'Enabled' : 'Disabled';
        $('debugAutoCondition-btn').textContent = dAutoCond ? 'Disable' : 'Enable';
        var dAutoTiming = (dbg.autoTiming !== undefined ? dbg.autoTiming : s.debugAutoTiming);
        $('debugAutoTiming-value').textContent = dAutoTiming ? 'Enabled' : 'Disabled';
        $('debugAutoTiming-btn').textContent = dAutoTiming ? 'Disable' : 'Enable';
        // Auth sub-flags
        var dAuthSess = (dbg.authSessions !== undefined ? dbg.authSessions : s.debugAuthSessions);
        $('debugAuthSessions-value').textContent = dAuthSess ? 'Enabled' : 'Disabled';
        $('debugAuthSessions-btn').textContent = dAuthSess ? 'Disable' : 'Enable';
        var dAuthCook = (dbg.authCookies !== undefined ? dbg.authCookies : s.debugAuthCookies);
        $('debugAuthCookies-value').textContent = dAuthCook ? 'Enabled' : 'Disabled';
        $('debugAuthCookies-btn').textContent = dAuthCook ? 'Disable' : 'Enable';
        var dAuthLogin = (dbg.authLogin !== undefined ? dbg.authLogin : s.debugAuthLogin);
        $('debugAuthLogin-value').textContent = dAuthLogin ? 'Enabled' : 'Disabled';
        $('debugAuthLogin-btn').textContent = dAuthLogin ? 'Disable' : 'Enable';
        var dAuthBootId = (dbg.authBootId !== undefined ? dbg.authBootId : s.debugAuthBootId);
        $('debugAuthBootId-value').textContent = dAuthBootId ? 'Enabled' : 'Disabled';
        $('debugAuthBootId-btn').textContent = dAuthBootId ? 'Disable' : 'Enable';
        // HTTP sub-flags
        var dHttpHandlers = (dbg.httpHandlers !== undefined ? dbg.httpHandlers : s.debugHttpHandlers);
        $('debugHttpHandlers-value').textContent = dHttpHandlers ? 'Enabled' : 'Disabled';
        $('debugHttpHandlers-btn').textContent = dHttpHandlers ? 'Disable' : 'Enable';
        var dHttpRequests = (dbg.httpRequests !== undefined ? dbg.httpRequests : s.debugHttpRequests);
        $('debugHttpRequests-value').textContent = dHttpRequests ? 'Enabled' : 'Disabled';
        $('debugHttpRequests-btn').textContent = dHttpRequests ? 'Disable' : 'Enable';
        var dHttpResponses = (dbg.httpResponses !== undefined ? dbg.httpResponses : s.debugHttpResponses);
        $('debugHttpResponses-value').textContent = dHttpResponses ? 'Enabled' : 'Disabled';
        $('debugHttpResponses-btn').textContent = dHttpResponses ? 'Disable' : 'Enable';
        var dHttpStreaming = (dbg.httpStreaming !== undefined ? dbg.httpStreaming : s.debugHttpStreaming);
        $('debugHttpStreaming-value').textContent = dHttpStreaming ? 'Enabled' : 'Disabled';
        $('debugHttpStreaming-btn').textContent = dHttpStreaming ? 'Disable' : 'Enable';
        // WiFi sub-flags
        var dWifiConn = (dbg.wifiConnection !== undefined ? dbg.wifiConnection : s.debugWifiConnection);
        $('debugWifiConnection-value').textContent = dWifiConn ? 'Enabled' : 'Disabled';
        $('debugWifiConnection-btn').textContent = dWifiConn ? 'Disable' : 'Enable';
        var dWifiConf = (dbg.wifiConfig !== undefined ? dbg.wifiConfig : s.debugWifiConfig);
        $('debugWifiConfig-value').textContent = dWifiConf ? 'Enabled' : 'Disabled';
        $('debugWifiConfig-btn').textContent = dWifiConf ? 'Disable' : 'Enable';
        var dWifiScan = (dbg.wifiScanning !== undefined ? dbg.wifiScanning : s.debugWifiScanning);
        $('debugWifiScanning-value').textContent = dWifiScan ? 'Enabled' : 'Disabled';
        $('debugWifiScanning-btn').textContent = dWifiScan ? 'Disable' : 'Enable';
        var dWifiDrv = (dbg.wifiDriver !== undefined ? dbg.wifiDriver : s.debugWifiDriver);
        $('debugWifiDriver-value').textContent = dWifiDrv ? 'Enabled' : 'Disabled';
        $('debugWifiDriver-btn').textContent = dWifiDrv ? 'Disable' : 'Enable';
        // Storage sub-flags
        var dStorFiles = (dbg.storageFiles !== undefined ? dbg.storageFiles : s.debugStorageFiles);
        $('debugStorageFiles-value').textContent = dStorFiles ? 'Enabled' : 'Disabled';
        $('debugStorageFiles-btn').textContent = dStorFiles ? 'Disable' : 'Enable';
        var dStorJson = (dbg.storageJson !== undefined ? dbg.storageJson : s.debugStorageJson);
        $('debugStorageJson-value').textContent = dStorJson ? 'Enabled' : 'Disabled';
        $('debugStorageJson-btn').textContent = dStorJson ? 'Disable' : 'Enable';
        var dStorSettings = (dbg.storageSettings !== undefined ? dbg.storageSettings : s.debugStorageSettings);
        $('debugStorageSettings-value').textContent = dStorSettings ? 'Enabled' : 'Disabled';
        $('debugStorageSettings-btn').textContent = dStorSettings ? 'Disable' : 'Enable';
        var dStorMigr = (dbg.storageMigration !== undefined ? dbg.storageMigration : s.debugStorageMigration);
        $('debugStorageMigration-value').textContent = dStorMigr ? 'Enabled' : 'Disabled';
        $('debugStorageMigration-btn').textContent = dStorMigr ? 'Disable' : 'Enable';
        // System sub-flags
        var dSysBoot = (dbg.systemBoot !== undefined ? dbg.systemBoot : s.debugSystemBoot);
        $('debugSystemBoot-value').textContent = dSysBoot ? 'Enabled' : 'Disabled';
        $('debugSystemBoot-btn').textContent = dSysBoot ? 'Disable' : 'Enable';
        var dSysConf = (dbg.systemConfig !== undefined ? dbg.systemConfig : s.debugSystemConfig);
        $('debugSystemConfig-value').textContent = dSysConf ? 'Enabled' : 'Disabled';
        $('debugSystemConfig-btn').textContent = dSysConf ? 'Disable' : 'Enable';
        var dSysTasks = (dbg.systemTasks !== undefined ? dbg.systemTasks : s.debugSystemTasks);
        $('debugSystemTasks-value').textContent = dSysTasks ? 'Enabled' : 'Disabled';
        $('debugSystemTasks-btn').textContent = dSysTasks ? 'Disable' : 'Enable';
        var dSysHw = (dbg.systemHardware !== undefined ? dbg.systemHardware : s.debugSystemHardware);
        $('debugSystemHardware-value').textContent = dSysHw ? 'Enabled' : 'Disabled';
        $('debugSystemHardware-btn').textContent = dSysHw ? 'Disable' : 'Enable';
        // Users sub-flags
        var dUsersMgmt = (dbg.usersMgmt !== undefined ? dbg.usersMgmt : s.debugUsersMgmt);
        $('debugUsersMgmt-value').textContent = dUsersMgmt ? 'Enabled' : 'Disabled';
        $('debugUsersMgmt-btn').textContent = dUsersMgmt ? 'Disable' : 'Enable';
        var dUsersReg = (dbg.usersRegister !== undefined ? dbg.usersRegister : s.debugUsersRegister);
        $('debugUsersRegister-value').textContent = dUsersReg ? 'Enabled' : 'Disabled';
        $('debugUsersRegister-btn').textContent = dUsersReg ? 'Disable' : 'Enable';
        var dUsersQuery = (dbg.usersQuery !== undefined ? dbg.usersQuery : s.debugUsersQuery);
        $('debugUsersQuery-value').textContent = dUsersQuery ? 'Enabled' : 'Disabled';
        $('debugUsersQuery-btn').textContent = dUsersQuery ? 'Disable' : 'Enable';
        // CLI sub-flags
        var dCliExec = (dbg.cliExecution !== undefined ? dbg.cliExecution : s.debugCliExecution);
        $('debugCliExecution-value').textContent = dCliExec ? 'Enabled' : 'Disabled';
        $('debugCliExecution-btn').textContent = dCliExec ? 'Disable' : 'Enable';
        var dCliQueue = (dbg.cliQueue !== undefined ? dbg.cliQueue : s.debugCliQueue);
        $('debugCliQueue-value').textContent = dCliQueue ? 'Enabled' : 'Disabled';
        $('debugCliQueue-btn').textContent = dCliQueue ? 'Disable' : 'Enable';
        var dCliValid = (dbg.cliValidation !== undefined ? dbg.cliValidation : s.debugCliValidation);
        $('debugCliValidation-value').textContent = dCliValid ? 'Enabled' : 'Disabled';
        $('debugCliValidation-btn').textContent = dCliValid ? 'Disable' : 'Enable';
        // Performance sub-flags
        var dPerfStack = (dbg.perfStack !== undefined ? dbg.perfStack : s.debugPerfStack);
        $('debugPerfStack-value').textContent = dPerfStack ? 'Enabled' : 'Disabled';
        $('debugPerfStack-btn').textContent = dPerfStack ? 'Disable' : 'Enable';
        var dPerfHeap = (dbg.perfHeap !== undefined ? dbg.perfHeap : s.debugPerfHeap);
        $('debugPerfHeap-value').textContent = dPerfHeap ? 'Enabled' : 'Disabled';
        $('debugPerfHeap-btn').textContent = dPerfHeap ? 'Disable' : 'Enable';
        var dPerfTiming = (dbg.perfTiming !== undefined ? dbg.perfTiming : s.debugPerfTiming);
        $('debugPerfTiming-value').textContent = dPerfTiming ? 'Enabled' : 'Disabled';
        $('debugPerfTiming-btn').textContent = dPerfTiming ? 'Disable' : 'Enable';
        // SSE sub-flags
        var dSseConn = (dbg.sseConnection !== undefined ? dbg.sseConnection : s.debugSseConnection);
        $('debugSseConnection-value').textContent = dSseConn ? 'Enabled' : 'Disabled';
        $('debugSseConnection-btn').textContent = dSseConn ? 'Disable' : 'Enable';
        var dSseEvents = (dbg.sseEvents !== undefined ? dbg.sseEvents : s.debugSseEvents);
        $('debugSseEvents-value').textContent = dSseEvents ? 'Enabled' : 'Disabled';
        $('debugSseEvents-btn').textContent = dSseEvents ? 'Disable' : 'Enable';
        var dSseBcast = (dbg.sseBroadcast !== undefined ? dbg.sseBroadcast : s.debugSseBroadcast);
        $('debugSseBroadcast-value').textContent = dSseBcast ? 'Enabled' : 'Disabled';
        $('debugSseBroadcast-btn').textContent = dSseBcast ? 'Disable' : 'Enable';
        // Command Flow sub-flags
        var dCmdflowRoute = (dbg.cmdflowRouting !== undefined ? dbg.cmdflowRouting : s.debugCmdflowRouting);
        $('debugCmdflowRouting-value').textContent = dCmdflowRoute ? 'Enabled' : 'Disabled';
        $('debugCmdflowRouting-btn').textContent = dCmdflowRoute ? 'Disable' : 'Enable';
        var dCmdflowQueue = (dbg.cmdflowQueue !== undefined ? dbg.cmdflowQueue : s.debugCmdflowQueue);
        $('debugCmdflowQueue-value').textContent = dCmdflowQueue ? 'Enabled' : 'Disabled';
        $('debugCmdflowQueue-btn').textContent = dCmdflowQueue ? 'Disable' : 'Enable';
        var dCmdflowCtx = (dbg.cmdflowContext !== undefined ? dbg.cmdflowContext : s.debugCmdflowContext);
        $('debugCmdflowContext-value').textContent = dCmdflowCtx ? 'Enabled' : 'Disabled';
        $('debugCmdflowContext-btn').textContent = dCmdflowCtx ? 'Disable' : 'Enable';
        // Compute derived parent states (ON if parent OR any child ON)
        var dAuthParent = (dbg.auth !== undefined ? dbg.auth : s.debugAuth);
        var dAuthParentValEl = $('debugAuth-value');
        if (dAuthParentValEl) {
          dAuthParentValEl.textContent = dAuthParent ? 'Enabled' : 'Disabled';
          var dAuthParentBtnEl = $('debugAuth-btn');
          if (dAuthParentBtnEl) dAuthParentBtnEl.textContent = dAuthParent ? 'Disable' : 'Enable';
        }
        var authGroupOn = dAuthParent || dAuthSess || dAuthCook || dAuthLogin || dAuthBootId;
        $('debugAuthGroup-value').textContent = authGroupOn ? 'Enabled' : 'Disabled';
        $('debugAuthGroup-btn').textContent = authGroupOn ? 'Disable All' : 'Enable All';
        var httpGroupOn = dHttpHandlers || dHttpRequests || dHttpResponses || dHttpStreaming;
        $('debugHttpGroup-value').textContent = httpGroupOn ? 'Enabled' : 'Disabled';
        $('debugHttpGroup-btn').textContent = httpGroupOn ? 'Disable All' : 'Enable All';
        var wifiGroupOn = dWifiConn || dWifiConf || dWifiScan || dWifiDrv;
        $('debugWifiGroup-value').textContent = wifiGroupOn ? 'Enabled' : 'Disabled';
        $('debugWifiGroup-btn').textContent = wifiGroupOn ? 'Disable All' : 'Enable All';
        var storageGroupOn = dStorFiles || dStorJson || dStorSettings || dStorMigr;
        $('debugStorageGroup-value').textContent = storageGroupOn ? 'Enabled' : 'Disabled';
        $('debugStorageGroup-btn').textContent = storageGroupOn ? 'Disable All' : 'Enable All';
        var systemGroupOn = dSysBoot || dSysConf || dSysTasks || dSysHw;
        $('debugSystemGroup-value').textContent = systemGroupOn ? 'Enabled' : 'Disabled';
        $('debugSystemGroup-btn').textContent = systemGroupOn ? 'Disable All' : 'Enable All';
        var usersGroupOn = dUsersMgmt || dUsersReg || dUsersQuery;
        $('debugUsersGroup-value').textContent = usersGroupOn ? 'Enabled' : 'Disabled';
        $('debugUsersGroup-btn').textContent = usersGroupOn ? 'Disable All' : 'Enable All';
        var cliGroupOn = dCliExec || dCliQueue || dCliValid;
        $('debugCliGroup-value').textContent = cliGroupOn ? 'Enabled' : 'Disabled';
        $('debugCliGroup-btn').textContent = cliGroupOn ? 'Disable All' : 'Enable All';
        var perfGroupOn = dPerfStack || dPerfHeap || dPerfTiming;
        $('debugPerfGroup-value').textContent = perfGroupOn ? 'Enabled' : 'Disabled';
        $('debugPerfGroup-btn').textContent = perfGroupOn ? 'Disable All' : 'Enable All';
        var sseGroupOn = dSseConn || dSseEvents || dSseBcast;
        $('debugSseGroup-value').textContent = sseGroupOn ? 'Enabled' : 'Disabled';
        $('debugSseGroup-btn').textContent = sseGroupOn ? 'Disable All' : 'Enable All';
        var cmdflowGroupOn = dCmdflowRoute || dCmdflowQueue || dCmdflowCtx;
        $('debugCmdFlowGroup-value').textContent = cmdflowGroupOn ? 'Enabled' : 'Disabled';
        $('debugCmdFlowGroup-btn').textContent = cmdflowGroupOn ? 'Disable All' : 'Enable All';
        var dEspNowParent = (dbg.espNow !== undefined ? dbg.espNow : s.debugEspNow);
        var dEspNowParentValEl = $('debugEspNow-value');
        if (dEspNowParentValEl) {
          dEspNowParentValEl.textContent = dEspNowParent ? 'Enabled' : 'Disabled';
          var dEspNowParentBtnEl = $('debugEspNow-btn');
          if (dEspNowParentBtnEl) dEspNowParentBtnEl.textContent = dEspNowParent ? 'Disable' : 'Enable';
        }
        var espnowGroupOn = dEspNowParent || dESN || dESNCore || dESNRouter || dESNMesh || dESNTopo || dESNEncryption;
        $('debugEspNowGroup-value').textContent = espnowGroupOn ? 'Enabled' : 'Disabled';
        $('debugEspNowGroup-btn').textContent = espnowGroupOn ? 'Disable All' : 'Enable All';
        var automationsGroupOn = dAutoSched || dAutoExec || dAutoCond || dAutoTiming;
        $('debugAutomationsGroup-value').textContent = automationsGroupOn ? 'Enabled' : 'Disabled';
        $('debugAutomationsGroup-btn').textContent = automationsGroupOn ? 'Disable All' : 'Enable All';
        try { window.updateDebugGroupIndicators(); } catch(_) {}
        var dMemory = (dbg.memory !== undefined ? dbg.memory : s.debugMemory);
        $('debugMemory-value').textContent = dMemory ? 'Enabled' : 'Disabled';
        $('debugMemory-btn').textContent = dMemory ? 'Disable' : 'Enable';
        var memInterval = (dbg.memorySampleIntervalSec !== undefined ? dbg.memorySampleIntervalSec : s.memorySampleIntervalSec);
        if (memInterval !== undefined) {
          $('memorySampleInterval-value').textContent = memInterval;
          $('memorySampleInterval-input').value = memInterval;
        }
        var dCmdSys = (dbg.commandSystem !== undefined ? dbg.commandSystem : s.debugCommandSystem);
        $('debugCommandSystem-value').textContent = dCmdSys ? 'Enabled' : 'Disabled';
        $('debugCommandSystem-btn').textContent = dCmdSys ? 'Disable' : 'Enable';
        var dSetSys = (dbg.settingsSystem !== undefined ? dbg.settingsSystem : s.debugSettingsSystem);
        $('debugSettingsSystem-value').textContent = dSetSys ? 'Enabled' : 'Disabled';
        $('debugSettingsSystem-btn').textContent = dSetSys ? 'Disable' : 'Enable';
        // Note: dFmRadio, dCamera, dMic, dG2, dGps, dRtc, dImu, dThermal, dTof, dGamepad, dApds, dPresence already declared above
        $('debugFmRadio-value').textContent = dFmRadio ? 'Enabled' : 'Disabled';
        $('debugFmRadio-btn').textContent = dFmRadio ? 'Disable' : 'Enable';
        $('debugCamera-value').textContent = dCamera ? 'Enabled' : 'Disabled';
        $('debugCamera-btn').textContent = dCamera ? 'Disable' : 'Enable';
        $('debugMicrophone-value').textContent = dMic ? 'Enabled' : 'Disabled';
        $('debugMicrophone-btn').textContent = dMic ? 'Disable' : 'Enable';
        if ($('debugG2-value')) { $('debugG2-value').textContent = dG2 ? 'Enabled' : 'Disabled'; $('debugG2-btn').textContent = dG2 ? 'Disable' : 'Enable'; }
        
        // Individual I2C sensor debug flags (persistent settings)
        $('debugGps-value').textContent = dGps ? 'Enabled' : 'Disabled';
        $('debugGps-btn').textContent = dGps ? 'Disable' : 'Enable';
        $('debugRtc-value').textContent = dRtc ? 'Enabled' : 'Disabled';
        $('debugRtc-btn').textContent = dRtc ? 'Disable' : 'Enable';
        $('debugImu-value').textContent = dImu ? 'Enabled' : 'Disabled';
        $('debugImu-btn').textContent = dImu ? 'Disable' : 'Enable';
        $('debugThermal-value').textContent = dThermal ? 'Enabled' : 'Disabled';
        $('debugThermal-btn').textContent = dThermal ? 'Disable' : 'Enable';
        $('debugTof-value').textContent = dTof ? 'Enabled' : 'Disabled';
        $('debugTof-btn').textContent = dTof ? 'Disable' : 'Enable';
        $('debugGamepad-value').textContent = dGamepad ? 'Enabled' : 'Disabled';
        $('debugGamepad-btn').textContent = dGamepad ? 'Disable' : 'Enable';
        $('debugApds-value').textContent = dApds ? 'Enabled' : 'Disabled';
        $('debugApds-btn').textContent = dApds ? 'Disable' : 'Enable';
        $('debugPresence-value').textContent = dPresence ? 'Enabled' : 'Disabled';
        $('debugPresence-btn').textContent = dPresence ? 'Disable' : 'Enable';

        // Log level (severity-based)
        // Single source of truth: settings.debug.logLevel (modular registry)
        var ll = (dbg.logLevel !== undefined ? dbg.logLevel : 3);
        if (ll < 0) ll = 0;
        if (ll > 3) ll = 3;
        var llName = (ll === 0 ? 'Error' : (ll === 1 ? 'Warn' : (ll === 2 ? 'Info' : 'Debug')));
        var llVal = $('logLevel-value');
        if (llVal) llVal.textContent = llName;
        var llSel = $('logLevel-select');
        if (llSel) llSel.value = String(ll);
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
        var oledAuto = $('oledAutoInit');
        if (oledAuto) oledAuto.checked = (oled.oledAutoInit === 1 || oled.oledAutoInit === true);
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
          el.style.color = on ? '#28a745' : '#dc3545';
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
    
    // Fetch build configuration and hide debug options for features not compiled in
    window.fetchBuildConfig = function() {
      return fetch('/api/buildconfig', {credentials: 'same-origin'})
        .then(function(r) { return r.json(); })
        .then(function(config) {
          window.__buildConfig = config;
          console.log('[SETTINGS] Build config loaded:', config);
          
          // Hide debug options for features not compiled in
          var hideIfDisabled = [
            {feature: 'camera', ids: ['debugCamera-value', 'debugCamera-btn']},
            {feature: 'microphone', ids: ['debugMicrophone-value', 'debugMicrophone-btn']},
            {feature: 'g2glasses', ids: ['debugG2-value', 'debugG2-btn']},
            {feature: 'apds', ids: ['debugApds-value', 'debugApds-btn']},
            {feature: 'fmradio', ids: ['debugFmRadio-value', 'debugFmRadio-btn']},
            {feature: 'gamepad', ids: ['debugGamepad-value', 'debugGamepad-btn']},
            {feature: 'gps', ids: ['debugGps-value', 'debugGps-btn']},
            {feature: 'imu', ids: ['debugImu-value', 'debugImu-btn']},
            {feature: 'thermal', ids: ['debugThermal-value', 'debugThermal-btn']},
            {feature: 'tof', ids: ['debugTof-value', 'debugTof-btn']},
            {feature: 'rtc', ids: ['debugRtc-value', 'debugRtc-btn']},
            {feature: 'presence', ids: ['debugPresence-value', 'debugPresence-btn']}
          ];
          
          hideIfDisabled.forEach(function(item) {
            if (!config[item.feature]) {
              item.ids.forEach(function(id) {
                var el = document.getElementById(id);
                if (el && el.parentElement) {
                  el.parentElement.style.display = 'none';
                }
              });
            }
          });
          
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
        backupMAC:    $('espnow-backupmac').value.trim()
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
    
    // Update memory sample interval
    window.updateMemorySampleInterval = function() {
      console.log('[SETTINGS] updateMemorySampleInterval called');
      var v = parseInt($('memorySampleInterval-input').value);
      console.log('[SETTINGS] updateMemorySampleInterval value:', v);
      if (v < 0 || v > 300) {
        alert('Interval must be between 0 and 300 seconds');
        return;
      }
      var cmd = 'set debug.memorySampleIntervalSec ' + v;
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) { return r.text(); })
      .then(function(t) {
        console.log('[SETTINGS] updateMemorySampleInterval result:', t);
        alert('Memory sample interval updated to ' + v + ' seconds');
        refreshSettings();
      })
      .catch(function(e) {
        console.error('[SETTINGS] updateMemorySampleInterval error:', e);
        alert('Error: ' + e.message);
      });
    };
    console.log('[SETTINGS] updateMemorySampleInterval defined');

    // Update log level
    window.updateLogLevel = function() {
      try {
        var sel = $('logLevel-select');
        if (!sel) return;
        var v = parseInt(sel.value, 10);
        if (isNaN(v) || v < 0 || v > 3) {
          alert('Log level must be 0..3');
          return;
        }
        var cmd = 'loglevel ' + v;
        fetch('/api/cli', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          credentials: 'same-origin',
          body: 'cmd=' + encodeURIComponent(cmd)
        })
        .then(function(r) { return r.text(); })
        .then(function(t) {
          alert(t);
          refreshSettings();
        })
        .catch(function(e) {
          alert('Error: ' + e.message);
        });
      } catch(e) {
        alert('Error: ' + e.message);
      }
    };
    console.log('[SETTINGS] updateLogLevel defined');
    
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
    
    window.updateDebugGroupIndicators = function() {
      var anyEnabled = function(keys) {
        for (var i = 0; i < keys.length; i++) {
          var el = $(keys[i] + '-value');
          if (el && el.textContent === 'Enabled') return true;
        }
        return false;
      };
      var setGroup = function(valueId, btnId, keys) {
        var vEl = $(valueId);
        var bEl = $(btnId);
        if (!vEl || !bEl) return;
        var on = anyEnabled(keys);
        vEl.textContent = on ? 'Enabled' : 'Disabled';
        bEl.textContent = on ? 'Disable All' : 'Enable All';
      };
      setGroup('debugCliGroup-value', 'debugCliGroup-btn', ['debugCliExecution', 'debugCliQueue', 'debugCliValidation']);
      setGroup('debugCmdFlowGroup-value', 'debugCmdFlowGroup-btn', ['debugCmdflowRouting', 'debugCmdflowQueue', 'debugCmdflowContext']);
      setGroup('debugUsersGroup-value', 'debugUsersGroup-btn', ['debugUsersMgmt', 'debugUsersRegister', 'debugUsersQuery']);
      setGroup('debugSystemGroup-value', 'debugSystemGroup-btn', ['debugSystemBoot', 'debugSystemConfig', 'debugSystemTasks', 'debugSystemHardware']);
      setGroup('debugAuthGroup-value', 'debugAuthGroup-btn', ['debugAuthSessions', 'debugAuthCookies', 'debugAuthLogin', 'debugAuthBootId']);
      setGroup('debugStorageGroup-value', 'debugStorageGroup-btn', ['debugStorageFiles', 'debugStorageJson', 'debugStorageSettings', 'debugStorageMigration']);
      setGroup('debugPerfGroup-value', 'debugPerfGroup-btn', ['debugPerfStack', 'debugPerfHeap', 'debugPerfTiming']);
      setGroup('debugAutomationsGroup-value', 'debugAutomationsGroup-btn', ['debugAutoScheduler', 'debugAutoExec', 'debugAutoCondition', 'debugAutoTiming']);
      setGroup('debugWifiGroup-value', 'debugWifiGroup-btn', ['debugWifiConnection', 'debugWifiConfig', 'debugWifiScanning', 'debugWifiDriver']);
      setGroup('debugHttpGroup-value', 'debugHttpGroup-btn', ['debugHttpHandlers', 'debugHttpRequests', 'debugHttpResponses', 'debugHttpStreaming']);
      setGroup('debugSseGroup-value', 'debugSseGroup-btn', ['debugSseConnection', 'debugSseEvents', 'debugSseBroadcast']);
      setGroup('debugEspNowGroup-value', 'debugEspNowGroup-btn', ['debugEspNowStream', 'debugEspNowCore', 'debugEspNowRouter', 'debugEspNowMesh', 'debugEspNowTopo', 'debugEspNowEncryption', 'debugEspNowMetadata']);
    };

    // Toggle debug flag
    window.toggleDebug = function(setting) {
      var valueEl = $(setting + '-value');
      var btnEl = $(setting + '-btn');
      if (!valueEl || !btnEl) return;
      var newVal = (valueEl.textContent !== 'Enabled') ? 1 : 0;
      valueEl.textContent = newVal ? 'Enabled' : 'Disabled';
      btnEl.textContent = newVal ? 'Disable' : 'Enable';
      try { window.updateDebugGroupIndicators(); } catch(_) {}
    };
    
    // Toggle debug group (parent/child semantics)
    window.toggleDebugGroup = function(group) {
      var children = [];
      var parentFlag = null;
      var groupValueId = '';
      var groupBtnId = '';
      
      if (group === 'automations') {
        children = ['debugAutoScheduler', 'debugAutoExec', 'debugAutoCondition', 'debugAutoTiming'];
        parentFlag = 'debugAutomations';
        groupValueId = 'debugAutomationsGroup-value';
        groupBtnId = 'debugAutomationsGroup-btn';
      } else if (group === 'espnow') {
        children = ['debugEspNowStream', 'debugEspNowCore', 'debugEspNowRouter', 'debugEspNowMesh', 'debugEspNowTopo', 'debugEspNowEncryption', 'debugEspNowMetadata'];
        parentFlag = 'debugEspNow';
        groupValueId = 'debugEspNowGroup-value';
        groupBtnId = 'debugEspNowGroup-btn';
      } else if (group === 'sensors') {
        children = ['debugGPS', 'debugRTC', 'debugIMU', 'debugThermal', 'debugToF', 'debugGamepad', 'debugAPDS', 'debugPresence', 'debugCamera', 'debugMicrophone', 'debugFMRadio', 'debugG2'];
        parentFlag = null;
        groupValueId = null;
        groupBtnId = null;
      } else if (group === 'auth') {
        children = ['debugAuthSessions', 'debugAuthCookies', 'debugAuthLogin', 'debugAuthBootId'];
        parentFlag = 'debugAuth';
        groupValueId = 'debugAuthGroup-value';
        groupBtnId = 'debugAuthGroup-btn';
      } else if (group === 'http') {
        children = ['debugHttpHandlers', 'debugHttpRequests', 'debugHttpResponses', 'debugHttpStreaming'];
        parentFlag = 'debugHttp';
        groupValueId = 'debugHttpGroup-value';
        groupBtnId = 'debugHttpGroup-btn';
      } else if (group === 'wifi') {
        children = ['debugWifiConnection', 'debugWifiConfig', 'debugWifiScanning', 'debugWifiDriver'];
        parentFlag = 'debugWifi';
        groupValueId = 'debugWifiGroup-value';
        groupBtnId = 'debugWifiGroup-btn';
      } else if (group === 'storage') {
        children = ['debugStorageFiles', 'debugStorageJson', 'debugStorageSettings', 'debugStorageMigration'];
        parentFlag = 'debugStorage';
        groupValueId = 'debugStorageGroup-value';
        groupBtnId = 'debugStorageGroup-btn';
      } else if (group === 'system') {
        children = ['debugSystemBoot', 'debugSystemConfig', 'debugSystemTasks', 'debugSystemHardware'];
        parentFlag = 'debugSystem';
        groupValueId = 'debugSystemGroup-value';
        groupBtnId = 'debugSystemGroup-btn';
      } else if (group === 'users') {
        children = ['debugUsersMgmt', 'debugUsersRegister', 'debugUsersQuery'];
        parentFlag = 'debugUsers';
        groupValueId = 'debugUsersGroup-value';
        groupBtnId = 'debugUsersGroup-btn';
      } else if (group === 'cli') {
        children = ['debugCliExecution', 'debugCliQueue', 'debugCliValidation'];
        parentFlag = 'debugCli';
        groupValueId = 'debugCliGroup-value';
        groupBtnId = 'debugCliGroup-btn';
      } else if (group === 'performance') {
        children = ['debugPerfStack', 'debugPerfHeap', 'debugPerfTiming'];
        parentFlag = 'debugPerformance';
        groupValueId = 'debugPerfGroup-value';
        groupBtnId = 'debugPerfGroup-btn';
      } else if (group === 'sse') {
        children = ['debugSseConnection', 'debugSseEvents', 'debugSseBroadcast'];
        parentFlag = 'debugSse';
        groupValueId = 'debugSseGroup-value';
        groupBtnId = 'debugSseGroup-btn';
      } else if (group === 'cmdflow') {
        children = ['debugCmdflowRouting', 'debugCmdflowQueue', 'debugCmdflowContext'];
        parentFlag = 'debugCommandFlow';
        groupValueId = 'debugCmdFlowGroup-value';
        groupBtnId = 'debugCmdFlowGroup-btn';
      } else {
        return;
      }
      
      var groupValueEl = $(groupValueId);
      var groupBtnEl = $(groupBtnId);
      if (!groupValueEl || !groupBtnEl) return;
      
      // Determine current group state from the group label (which may reflect parent-only state)
      var anyOn = (groupValueEl.textContent === 'Enabled');
      
      // Toggle action: if group is currently ON, turn all OFF; if group is OFF, turn all ON
      var newVal = anyOn ? 0 : 1;
      
      // Update group UI state
      groupValueEl.textContent = newVal ? 'Enabled' : 'Disabled';
      groupBtnEl.textContent = newVal ? 'Disable All' : 'Enable All';

      // Update all child flags
      for (var i = 0; i < children.length; i++) {
        var child = children[i];
        var childValueEl = $(child + '-value');
        var childBtnEl = $(child + '-btn');
        if (childValueEl) childValueEl.textContent = newVal ? 'Enabled' : 'Disabled';
        if (childBtnEl) childBtnEl.textContent = newVal ? 'Disable' : 'Enable';
      }

      // Update parent flag UI if one exists for this group
      if (parentFlag) {
        var parentValueEl = $(parentFlag + '-value');
        var parentBtnEl = $(parentFlag + '-btn');
        if (parentValueEl) parentValueEl.textContent = newVal ? 'Enabled' : 'Disabled';
        if (parentBtnEl) parentBtnEl.textContent = newVal ? 'Disable' : 'Enable';
      }

      try { window.updateDebugGroupIndicators(); } catch(_) {}
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
    // Save debug settings
    window.saveDebugSettings = function() {
      console.log('[SETTINGS] saveDebugSettings called');
      var cmds = [];
      var getVal = function(k) {
        var el = $(k + '-value');
        return el && (el.textContent === 'Enabled');
      };
      var getValById = function(id) {
        var el = $(id);
        return el && (el.textContent === 'Enabled');
      };
      var push = function(cmd) { cmds.push(cmd); };

      push('debugcli ' + (getVal('debugCli') ? 1 : 0));
      push('debugcommandflow ' + (getVal('debugCommandFlow') ? 1 : 0));
      push('debugusers ' + (getVal('debugUsers') ? 1 : 0));
      push('debugsystem ' + (getVal('debugSystem') ? 1 : 0));
      push('debugwifi ' + (getVal('debugWifi') ? 1 : 0));
      push('debugstorage ' + (getVal('debugStorage') ? 1 : 0));
      push('debugperformance ' + (getVal('debugPerformance') ? 1 : 0));
      push('debugautomations ' + (getVal('debugAutomations') ? 1 : 0));
      push('debughttp ' + (getVal('debugHttp') ? 1 : 0));
      push('debugsse ' + (getVal('debugSse') ? 1 : 0));
      push('debuglogger ' + (getVal('debugLogger') ? 1 : 0));
      push('debugdatetime ' + (getVal('debugDateTime') ? 1 : 0));
      push('debugauth ' + (getValById('debugAuthGroup-value') ? 1 : 0));
      push('debugespnow ' + (getValById('debugEspNowGroup-value') ? 1 : 0));
      push('debugespnowstream ' + (getVal('debugEspNowStream') ? 1 : 0));
      push('debugespnowcore ' + (getVal('debugEspNowCore') ? 1 : 0));
      push('debugespnowrouter ' + (getVal('debugEspNowRouter') ? 1 : 0));
      push('debugespnowmesh ' + (getVal('debugEspNowMesh') ? 1 : 0));
      push('debugespnowtopo ' + (getVal('debugEspNowTopo') ? 1 : 0));
      push('debugespnowencryption ' + (getVal('debugEspNowEncryption') ? 1 : 0));
      push('debugespnowmetadata ' + (getVal('debugEspNowMetadata') ? 1 : 0));
      push('debugautoscheduler ' + (getVal('debugAutoScheduler') ? 1 : 0));
      push('debugautoexec ' + (getVal('debugAutoExec') ? 1 : 0));
      push('debugautocondition ' + (getVal('debugAutoCondition') ? 1 : 0));
      push('debugautotiming ' + (getVal('debugAutoTiming') ? 1 : 0));
      push('debugauthsessions ' + (getVal('debugAuthSessions') ? 1 : 0));
      push('debugauthcookies ' + (getVal('debugAuthCookies') ? 1 : 0));
      push('debugauthlogin ' + (getVal('debugAuthLogin') ? 1 : 0));
      push('debugauthbootid ' + (getVal('debugAuthBootId') ? 1 : 0));
      push('debughttphandlers ' + (getVal('debugHttpHandlers') ? 1 : 0));
      push('debughttprequests ' + (getVal('debugHttpRequests') ? 1 : 0));
      push('debughttpresponses ' + (getVal('debugHttpResponses') ? 1 : 0));
      push('debughttpstreaming ' + (getVal('debugHttpStreaming') ? 1 : 0));
      push('debugwificonnection ' + (getVal('debugWifiConnection') ? 1 : 0));
      push('debugwificonfig ' + (getVal('debugWifiConfig') ? 1 : 0));
      push('debugwifiscanning ' + (getVal('debugWifiScanning') ? 1 : 0));
      push('debugwifidriver ' + (getVal('debugWifiDriver') ? 1 : 0));
      push('debugstoragefiles ' + (getVal('debugStorageFiles') ? 1 : 0));
      push('debugstoragejson ' + (getVal('debugStorageJson') ? 1 : 0));
      push('debugstoragesettings ' + (getVal('debugStorageSettings') ? 1 : 0));
      push('debugstoragemigration ' + (getVal('debugStorageMigration') ? 1 : 0));
      push('debugsystemboot ' + (getVal('debugSystemBoot') ? 1 : 0));
      push('debugsystemconfig ' + (getVal('debugSystemConfig') ? 1 : 0));
      push('debugsystemtasks ' + (getVal('debugSystemTasks') ? 1 : 0));
      push('debugsystemhardware ' + (getVal('debugSystemHardware') ? 1 : 0));
      push('debugusersmgmt ' + (getVal('debugUsersMgmt') ? 1 : 0));
      push('debugusersregister ' + (getVal('debugUsersRegister') ? 1 : 0));
      push('debugusersquery ' + (getVal('debugUsersQuery') ? 1 : 0));
      push('debugcliexecution ' + (getVal('debugCliExecution') ? 1 : 0));
      push('debugcliqueue ' + (getVal('debugCliQueue') ? 1 : 0));
      push('debugclivalidation ' + (getVal('debugCliValidation') ? 1 : 0));
      push('debugperfstack ' + (getVal('debugPerfStack') ? 1 : 0));
      push('debugperfheap ' + (getVal('debugPerfHeap') ? 1 : 0));
      push('debugperftiming ' + (getVal('debugPerfTiming') ? 1 : 0));
      push('debugsseconnection ' + (getVal('debugSseConnection') ? 1 : 0));
      push('debugsseevents ' + (getVal('debugSseEvents') ? 1 : 0));
      push('debugssebroadcast ' + (getVal('debugSseBroadcast') ? 1 : 0));
      push('debugcmdflowrouting ' + (getVal('debugCmdflowRouting') ? 1 : 0));
      push('debugcmdflowqueue ' + (getVal('debugCmdflowQueue') ? 1 : 0));
      push('debugcmdflowcontext ' + (getVal('debugCmdflowContext') ? 1 : 0));
      push('debugmemory ' + (getVal('debugMemory') ? 1 : 0));
      try {
        var llSel = $('logLevel-select');
        if (llSel) {
          var v = parseInt(llSel.value, 10);
          if (!isNaN(v) && v >= 0 && v <= 3) push('loglevel ' + v);
        }
      } catch(_) {}

      sendSequential(cmds,
        function() {
          try { if (typeof window.refreshSettings === 'function') window.refreshSettings(); } catch(_) {}
          alert('Debug settings saved.');
        },
        function() { alert('One or more debug commands failed.'); }
      );
    };
    
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
        ['i2cClockThermalHz', 'i2cclockthermalHz'],
        ['i2cClockToFHz', 'i2cclocktofHz']
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
        var oledAutoInit = getBool('oledAutoInit');
        if (oledAutoInit !== null) cmds.push('oledautoinit ' + oledAutoInit);
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
    
    window.toggleUserDropdown = function(username) {
      var dropdown = $('dropdown-' + username);
      if (!dropdown) return;
      var isVisible = dropdown.style.display === 'block';
      dropdown.style.display = isVisible ? 'none' : 'block';
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
          html += '<div><strong>' + username + '</strong> ' + (isAdmin ? '<span style="color:#007bff;font-size:0.85rem">(Admin)</span>' : '<span style="color:#28a745;font-size:0.85rem">(User)</span>') + ' <span style="color:var(--panel-fg);font-size:0.85rem">' + sessionCount + ' session' + (sessionCount !== 1 ? 's' : '') + '</span></div>';
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
          html += '</div></div></div>';
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
