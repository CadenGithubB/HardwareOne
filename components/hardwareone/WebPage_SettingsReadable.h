// READABLE VERSION OF WEB_SETTINGS.H JAVASCRIPT
// This file documents the expanded JavaScript for reference
// The actual web_settings.h uses minified versions to save flash memory

// ============================================================================
// PART 2: API HELPERS - EXPANDED FOR REFERENCE
// ============================================================================

window.toggleEspNow = function() {
  console.log('[SETTINGS] toggleEspNow called');
  
  var cur = ($('espnow-value').textContent === 'Enabled') ? 1 : 0;
  var v = cur ? 0 : 1;
  console.log('[SETTINGS] toggleEspNow - current:', cur, 'new:', v);
  
  // FIXED: Removed 'set' prefix - command is just 'espnowenabled'
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

// ============================================================================
// PART 3: SAVE FUNCTIONS - SENSORS UI SETTINGS
// ============================================================================

window.saveSensorsUISettings = function() {
  try {
    var cmds = [];
    
    // Helper to add command with 'set' prefix
    var pushCmd = function(k, v) {
      cmds.push('set ' + k + ' ' + v);  // ⚠️ USES 'set' PREFIX
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
    
    // Collect all settings
    var tp = getInt('thermalPollingMs');
    if (tp !== null) pushCmd('thermalPollingMs', tp);
    
    var tpf = getInt('tofPollingMs');
    if (tpf !== null) pushCmd('tofPollingMs', tpf);
    
    var tss = getInt('tofStabilityThreshold');
    if (tss !== null) pushCmd('tofStabilityThreshold', tss);
    
    var pal = getStr('thermalPaletteDefault');
    if (pal) pushCmd('thermalPaletteDefault', pal);
    
    var twf = getInt('thermalWebMaxFps');
    if (twf !== null) pushCmd('thermalWebMaxFps', twf);
    
    var ewma = getStr('thermalEWMAFactor');
    if (ewma !== null) pushCmd('thermalEWMAFactor', ewma);
    
    var ttm = getInt('thermalTransitionMs');
    if (ttm !== null) pushCmd('thermalTransitionMs', ttm);
    
    var ttm2 = getInt('tofTransitionMs');
    if (ttm2 !== null) pushCmd('tofTransitionMs', ttm2);
    
    var tmax = getInt('tofMaxDistanceMm');
    if (tmax !== null) pushCmd('tofMaxDistanceMm', tmax);
    
    if (cmds.length === 0) {
      alert('No Client UI settings to save.');
      return;
    }
    
    // Send all commands
    Promise.all(cmds.map(function(c) {
      return fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(c)
      }).then(function(r) { return r.text(); });
    }))
    .then(function() {
      try {
        if (typeof window.refreshSettings === 'function') {
          window.refreshSettings();
        }
      } catch(_) {}
      alert('Client UI settings saved.');
    })
    .catch(function() {
      alert('One or more Client UI commands failed.');
    });
  } catch(e) {
    alert('Error: ' + e.message);
  }
};

// ============================================================================
// PART 3: SAVE FUNCTIONS - HARDWARE SETTINGS
// ============================================================================

window.saveHardwareSettings = function() {
  try {
    var cmds = [];
    
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
    
    // LED Settings - ⚠️ ALL USE 'set' PREFIX
    var brightness = getInt('ledBrightness');
    if (brightness !== null) cmds.push('set hardware.led.brightness ' + brightness);
    
    var enabled = getBool('ledStartupEnabled');
    if (enabled !== null) cmds.push('set hardware.led.startupEnabled ' + enabled);
    
    var effect = getStr('ledStartupEffect');
    if (effect) cmds.push('set hardware.led.startupEffect ' + effect);
    
    var color = getStr('ledStartupColor');
    if (color) cmds.push('set hardware.led.startupColor ' + color);
    
    var color2 = getStr('ledStartupColor2');
    if (color2) cmds.push('set hardware.led.startupColor2 ' + color2);
    
    var duration = getInt('ledStartupDuration');
    if (duration !== null) cmds.push('set hardware.led.startupDuration ' + duration);
    
    // OLED Settings - ⚠️ ALL USE 'set' PREFIX
    var oledEnabled = getBool('oledEnabled');
    if (oledEnabled !== null) cmds.push('set oled.enabled ' + oledEnabled);
    
    var oledAutoInit = getBool('oledAutoInit');
    if (oledAutoInit !== null) cmds.push('set oled.autoInit ' + oledAutoInit);
    
    var oledBootMode = getStr('oledBootMode');
    if (oledBootMode) cmds.push('set oled.bootMode ' + oledBootMode);
    
    var oledDefaultMode = getStr('oledDefaultMode');
    if (oledDefaultMode) cmds.push('set oled.defaultMode ' + oledDefaultMode);
    
    var oledBootDuration = getInt('oledBootDuration');
    if (oledBootDuration !== null) cmds.push('set oled.bootDuration ' + oledBootDuration);
    
    var oledUpdateInterval = getInt('oledUpdateInterval');
    if (oledUpdateInterval !== null) cmds.push('set oled.updateInterval ' + oledUpdateInterval);
    
    var oledBrightness = getInt('oledBrightness');
    if (oledBrightness !== null) cmds.push('set oled.brightness ' + oledBrightness);
    
    var oledThermalScale = getStr('oledThermalScale');
    if (oledThermalScale) cmds.push('set oled.thermalScale ' + oledThermalScale);
    
    var oledThermalColorMode = getStr('oledThermalColorMode');
    if (oledThermalColorMode) cmds.push('set oled.thermalColorMode ' + oledThermalColorMode);
    
    if (cmds.length === 0) {
      alert('No hardware settings to save.');
      return;
    }
    
    // Send all commands
    Promise.all(cmds.map(function(c) {
      return fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        credentials: 'same-origin',
        body: 'cmd=' + encodeURIComponent(c)
      }).then(function(r) { return r.text(); });
    }))
    .then(function() {
      alert('Hardware settings saved.');
    })
    .catch(function() {
      alert('One or more hardware commands failed.');
    });
  } catch(e) {
    alert('Error saving hardware settings: ' + e.message);
  }
};

// ============================================================================
// COMMANDS AUDIT SUMMARY
// ============================================================================

/*
COMMANDS THAT USE 'set' PREFIX (NEED TO VERIFY THESE EXIST):
- set thermalPollingMs
- set tofPollingMs
- set tofStabilityThreshold
- set thermalPaletteDefault
- set thermalWebMaxFps
- set thermalEWMAFactor
- set thermalTransitionMs
- set tofTransitionMs
- set tofMaxDistanceMm
- set hardware.led.brightness
- set hardware.led.startupEnabled
- set hardware.led.startupEffect
- set hardware.led.startupColor
- set hardware.led.startupColor2
- set hardware.led.startupDuration
- set oled.enabled
- set oled.autoInit
- set oled.bootMode
- set oled.defaultMode
- set oled.bootDuration
- set oled.updateInterval
- set oled.brightness
- set oled.thermalScale
- set oled.thermalColorMode
- set tzOffsetMinutes
- set ntpServer

COMMANDS WITHOUT 'set' PREFIX (CORRECT):
- espnowenabled (FIXED!)
- wifiautoreconnect
- webclihistorysize, oledclihistorysize
- debugauthcookies, debughttp, debugsse, etc.
- thermaltargetfps, thermaldevicepollms, etc.
- outserial, outweb, outtft

QUESTION: Do the 'set' commands actually exist in the command registry?
Or should they all be converted to direct commands without 'set'?
*/
