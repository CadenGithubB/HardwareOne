
(function() {
  try {
    console.log('[ESP-NOW] Chunk 3A: listDevices function start');
    window.listDevices = function(preloadedBondStatus, preloadedList) {
      function applyBondStatus(bondStatus) {
        window.__bondedPeerMac = null;
        const bondMatch = bondStatus.match(/Peer MAC:\s*([A-Fa-f0-9:]{17})/);
        if (bondMatch) { window.__bondedPeerMac = bondMatch[1].toUpperCase(); }
      }
      function applyList(output) {
        try { console.log('[ESP-NOW][DEV] listDevices: output length', output ? output.length : -1); } catch(_){}
        let parsed = null;
        try { parsed = JSON.parse(output); } catch(_) { parsed = null; }
        const devices = (parsed && Array.isArray(parsed.devices)) ? parsed.devices : [];
        // Store parsed device list globally for unified rendering
        window.__pairedDevices = devices;
        window.renderUnifiedDeviceList();
      }
      if (preloadedBondStatus !== undefined) {
        applyBondStatus(preloadedBondStatus);
        applyList(preloadedList !== undefined ? preloadedList : '');
        return;
      }
      // First get bond status to identify bonded device
      hw.postFormText('/api/cli', { cmd: 'bondstatus' })
      .then(bondStatus => {
        applyBondStatus(bondStatus);
        // Now fetch device list
        return hw.postFormText('/api/cli', { cmd: 'espnowlist' });
      })
      .then(applyList)
      .catch(error => {
        hw.$('device-list').innerHTML = '<div style="color: #dc3545;">Error loading devices: ' + error + '</div>';
      });
    };
    console.log('[ESP-NOW] Chunk 3A: listDevices function ready');
    /* Unified device list renderer: merges paired devices + mesh health into one table */
    console.log('[ESP-NOW] Chunk 3B: renderUnifiedDeviceList start');
    window.__pairedDevices = window.__pairedDevices || [];
    window.__meshPeers = window.__meshPeers || [];
    window.__meshUnpaired = window.__meshUnpaired || [];
    window.renderUnifiedDeviceList = function() {
      var deviceList = hw.$('device-list');
      if (!deviceList) return;
      var paired = window.__pairedDevices || [];
      var meshPeers = window.__meshPeers || [];
      var meshUnpaired = window.__meshUnpaired || [];
      var isMesh = !!window.espnowIsMesh;

      // Build MAC-keyed mesh health map
      var healthMap = {};
      for (var mi = 0; mi < meshPeers.length; mi++) {
        var mp = meshPeers[mi];
        if (mp.mac) healthMap[mp.mac.toUpperCase()] = mp;
      }

      // Build unified device entries from paired list
      window.espnowDevices = [];
      var seenMacs = {};
      var html = '';
      for (var i = 0; i < paired.length; i++) {
        var dev = paired[i];
        var mac = (dev.mac || '').toUpperCase();
        if (!mac) continue;
        seenMacs[mac] = true;
        var deviceName = dev.name || '';
        var isEncrypted = !!dev.encrypted;
        var isBonded = window.__bondedPeerMac && mac === window.__bondedPeerMac;
        window.espnowDevices.push({ mac: mac, name: deviceName, encrypted: isEncrypted, bonded: isBonded });

        // Mesh health for this device
        var health = healthMap[mac] || null;
        var statusDot = '';
        var statusLabel = '';
        var statsLine = '';
        if (isMesh && health) {
          var hbSec = (typeof health.secondsSinceHeartbeat === 'number') ? health.secondsSinceHeartbeat : null;
          var actSec = (typeof health.secondsSinceActivity === 'number') ? health.secondsSinceActivity : null;
          
          // Consider device online if either heartbeat OR recent activity (ACKs) within timeout
          var isOnline = health.alive || health.activityAlive;
          var isFresh = (hbSec !== null && hbSec <= 15) || (actSec !== null && actSec <= 15);
          
          if (isOnline) {
            if (isFresh) {
              statusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--success);margin-right:6px" title="Online"></span>';
              statusLabel = '<span style="color:var(--success);font-size:.8em;margin-left:6px">Online</span>';
            } else {
              statusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--warning);margin-right:6px" title="Online (Stale)"></span>';
              statusLabel = '<span style="color:var(--warning);font-size:.8em;margin-left:6px">Online</span>';
            }
          } else {
            statusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--danger);margin-right:6px" title="Offline"></span>';
            statusLabel = '<span style="color:var(--danger);font-size:.8em;margin-left:6px">Offline</span>';
          }
          statsLine = '<span style="color:var(--muted);font-size:.78em;margin-left:4px">HB: ' + (health.heartbeatCount || 0) + ' | ACKs: ' + (health.ackCount || 0) + '</span>';
        }

        var encText = isEncrypted ? 'Encrypted' : 'Unencrypted';
        var encClass = isEncrypted ? 'device-encrypted' : 'device-unencrypted';
        var encInd = isEncrypted ? 'encryption-enabled' : 'encryption-disabled';
        var bondBadge = isBonded ? '<span style="color:var(--warning);margin-right:4px;font-weight:bold" title="Bonded Device">[BOND]</span>' : '';

        // Phase 2.8: render a mesh badge next to the device name when meshId
        // is available. Greys out + line-throughs the label if the mesh is
        // currently disabled — frames from this peer would be dropped.
        var meshBadge = '';
        if (typeof dev.meshId === 'number' && dev.meshId >= 0 && typeof window.meshLabelForSlot === 'function') {
          var meshLabel = window.meshLabelForSlot(dev.meshId);
          if (meshLabel) {
            var meshEn = (typeof window.meshEnabledForSlot === 'function') ? window.meshEnabledForSlot(dev.meshId) : true;
            var mbg  = meshEn ? 'rgba(74,144,226,.15)' : 'rgba(120,120,120,.15)';
            var mfg  = meshEn ? '#4a90e2' : 'var(--muted)';
            var mdeco = meshEn ? '' : ';text-decoration:line-through';
            var mhint = meshEn ? 'mesh: ' + meshLabel : 'mesh: ' + meshLabel + ' (disabled - frames dropped)';
            meshBadge = '<span style="font-size:.72em;padding:2px 8px;border-radius:6px;background:' + mbg + ';color:' + mfg + ';margin-left:6px' + mdeco + '" title="' + mhint + '">' + meshLabel + '</span>';
          }
        }

        html += '<div class="device-item">';
        html += '<div style="flex:1;min-width:0">';
        html += '<div class="device-mac">' + statusDot + bondBadge + '<strong>' + (deviceName || mac) + '</strong>';
        html += '<span class="encryption-indicator ' + encInd + '" title="' + encText + '"></span>';
        html += meshBadge;
        html += statusLabel + '</div>';
        html += '<div class="device-channel ' + encClass + '">' + mac + ' • ' + encText;
        if (isBonded) html += ' • <strong>Bonded</strong>';
        if (isMesh && health) html += ' • ' + statsLine;
        html += '</div>';
        html += '</div>';
        html += '<div class="device-actions">';
        html += '<button class="btn btn-small" onclick="toggleDevicePanel(\'' + mac + '\',\'message\')">Interact</button>';
        html += '<button class="btn btn-small" data-guest-hide onclick="unpairDevice(\'' + mac + '\')">Unpair</button>';
        html += '</div>';
        html += '</div>';
      }

      // Mesh-only peers (in meshPeers but not in paired list)
      if (isMesh) {
        var meshOnly = [];
        for (var mi2 = 0; mi2 < meshPeers.length; mi2++) {
          var pm = meshPeers[mi2];
          if (pm.mac && !seenMacs[pm.mac.toUpperCase()]) {
            meshOnly.push(pm);
            seenMacs[pm.mac.toUpperCase()] = true;
          }
        }
        if (meshOnly.length > 0) {
          for (var j = 0; j < meshOnly.length; j++) {
            var mp2 = meshOnly[j];
            var mmac = (mp2.mac || '').toUpperCase();
            var mname = mp2.name || 'Unknown';
            var mStatusDot = '', mStatusLabel = '';
            var mhbSec = (typeof mp2.secondsSinceHeartbeat === 'number') ? mp2.secondsSinceHeartbeat : null;
            if (mp2.alive) {
              if (mhbSec !== null && mhbSec <= 15) {
                mStatusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--success);margin-right:6px" title="Online"></span>';
                mStatusLabel = '<span style="color:var(--success);font-size:.8em;margin-left:6px">Online</span>';
              } else {
                mStatusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--warning);margin-right:6px" title="Stale"></span>';
                mStatusLabel = '<span style="color:var(--warning);font-size:.8em;margin-left:6px">Stale</span>';
              }
            } else if (mp2.activityAlive) {
              mStatusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--warning);margin-right:6px" title="Stale"></span>';
              mStatusLabel = '<span style="color:var(--warning);font-size:.8em;margin-left:6px">Stale</span>';
            } else {
              mStatusDot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--danger);margin-right:6px" title="Offline"></span>';
              mStatusLabel = '<span style="color:var(--danger);font-size:.8em;margin-left:6px">Offline</span>';
            }
            var mStats = '<span style="color:var(--muted);font-size:.78em;margin-left:4px">HB: ' + (mp2.heartbeatCount || 0) + ' | ACKs: ' + (mp2.ackCount || 0) + '</span>';

            html += '<div class="device-item">';
            html += '<div style="flex:1;min-width:0">';
            html += '<div class="device-mac">' + mStatusDot + '<strong>' + mname + '</strong>';
            html += '<span style="background:var(--crumb-bg);color:var(--panel-fg);font-size:.72em;padding:2px 6px;border-radius:4px;margin-left:6px">Mesh</span>';
            html += mStatusLabel + '</div>';
            html += '<div class="device-channel" style="color:var(--muted)">' + mmac + ' • ' + mStats + '</div>';
            html += '</div>';
            html += '<div class="device-actions">';
            html += '<button class="btn btn-small" onclick="toggleDevicePanel(\'' + mmac + '\',\'message\')">Interact</button>';
            html += '</div>';
            html += '</div>';
          }
        }

        // Unpaired/discovered devices
        if (meshUnpaired.length > 0) {
          html += '<div style="margin-top:12px;padding-top:12px;border-top:1px solid var(--border)">';
          html += '<div style="font-size:.85em;font-weight:600;color:var(--muted);margin-bottom:8px">Discovered Devices (' + meshUnpaired.length + ')</div>';
          for (var ui = 0; ui < meshUnpaired.length; ui++) {
            var ud = meshUnpaired[ui];
            var umac = (ud.mac || '').toUpperCase();
            var uname = ud.name || 'Unknown';
            var rssiColor = ud.rssi > -60 ? 'var(--success)' : (ud.rssi > -75 ? 'var(--warning)' : 'var(--danger)');
            html += '<div class="device-item">';
            html += '<div style="flex:1;min-width:0">';
            html += '<div class="device-mac"><strong>' + uname + '</strong>';
            html += '<span style="background:var(--crumb-bg);color:var(--muted);font-size:.72em;padding:2px 6px;border-radius:4px;margin-left:6px">Discovered</span></div>';
            html += '<div class="device-channel" style="color:var(--muted)">' + umac;
            html += ' • RSSI: <span style="color:' + rssiColor + '">' + ud.rssi + ' dBm</span>';
            html += ' • Last: ' + ud.secondsSinceLastSeen + 's ago</div>';
            html += '</div>';
            html += '<div class="device-actions">';
            html += '<button class="btn btn-small" data-guest-hide onclick="pairUnpairedDevice(\'' + umac + '\',\'' + uname.replace(/'/g, "\\'") + '\')">Pair</button>';
            html += '</div>';
            html += '</div>';
          }
          html += '</div>';
        }
      }

      if (!html) {
        deviceList.innerHTML = '<div style="color:var(--muted);text-align:center;padding:20px">No devices paired yet</div>';
      } else {
        deviceList.innerHTML = html;
      }
    };
    console.log('[ESP-NOW] Chunk 3B: renderUnifiedDeviceList ready');
    /* Per-device panel rendering and actions */
    console.log('[ESP-NOW] Chunk 3C: initializeFileBrowser start');
    window.initializeFileBrowser = function(mac) {
      if (typeof window.createFileExplorerWithInput === 'function') {
        window.createFileExplorerWithInput({
          explorerContainerId: 'fexplorer-' + mac,
          inputId: 'fp-' + mac,
          path: '/',
          height: '280px',
          mode: 'select',  // Select-only mode: no edit/delete/view actions
          selectFilesOnly: true,  // Only allow selecting files, not folders
          onSelect: function(filePath) {
            var statusDiv = hw.$('fstat-' + mac);
            if (statusDiv && filePath) {
              statusDiv.textContent = 'Ready to send: ' + filePath;
            }
          }
        });
      }
    };
    console.log('[ESP-NOW] Chunk 3C: initializeFileBrowser ready');
    console.log('[ESP-NOW] Chunk 3D: openBroadcastPanel start');
    window.openBroadcastPanel = function() {
      try {
        var card = hw.$('device-panel-card');
        var title = hw.$('device-panel-title');
        var subtitle = hw.$('device-panel-subtitle');
        var content = hw.$('device-panel-content');
        if (!card || !title || !content) return;
        
        var activeKey = (card.dataset.key || '');
        var nextKey = 'broadcast|broadcast';
        
        // Toggle off if already showing broadcast
        if (card.style.display !== 'none' && activeKey === nextKey) {
          card.style.display = 'none';
          return;
        }
        
        title.textContent = 'Broadcast Panel';
        subtitle.textContent = 'Send a message to all paired devices';
        content.innerHTML = renderDevicePanel('', 'broadcast');
        card.dataset.key = nextKey;
        card.style.display = 'block';
        try { card.scrollIntoView({behavior:'smooth', block:'nearest'}); } catch(_) {}
      } catch(e) { console.warn('[ESP-NOW] openBroadcastPanel error:', e); }
    };
    console.log('[ESP-NOW] Chunk 3D: openBroadcastPanel ready');
    console.log('[ESP-NOW] Chunk 3E: toggleDevicePanel start');
    window.toggleDevicePanel = function(mac, kind) {
      try {
        var card = hw.$('device-panel-card');
        var title = hw.$('device-panel-title');
        var subtitle = hw.$('device-panel-subtitle');
        var content = hw.$('device-panel-content');
        if (!card || !title || !content) return;
        var activeKey = (card.dataset.key || '');
        var nextKey = mac + '|' + kind;
        
        // Toggle off if clicking same panel
        if (card.style.display !== 'none' && activeKey === nextKey) {
          card.style.display = 'none';
          return;
        }
        
        var deviceInfo = mac;
        for (var i = 0; i < window.espnowDevices.length; i++) {
          if (window.espnowDevices[i].mac === mac) {
            deviceInfo = window.espnowDevices[i].name + ' • ' + mac;
            break;
          }
        }
        title.textContent = 'Interact — ' + deviceInfo;
        subtitle.textContent = 'Send messages, execute commands, or transfer files';
        content.innerHTML = renderDevicePanel(mac, kind);
        card.dataset.key = nextKey;
        card.style.display = 'block';
        try { card.scrollIntoView({behavior:'smooth', block:'nearest'}); } catch(_) {}
      } catch(e) { console.warn('[ESP-NOW] toggleDevicePanel error:', e); }
    };
    console.log('[ESP-NOW] Chunk 3E: toggleDevicePanel ready');
    console.log('[ESP-NOW] Chunk 3F: renderDevicePanel start');
    window.renderDevicePanel = function(mac, kind) {
      if (kind === 'message') {
        return (
          '<div style="display:grid;grid-template-columns:1fr 2fr;gap:12px;margin-bottom:12px">'
          + '<div class="interact-tabs">'
          + '<button class="btn interact-tab" id="btn-text-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'text\')">Text</button>'
          + '<button class="btn interact-tab" id="btn-remote-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'remote\')">Remote</button>'
          + '<button class="btn interact-tab" id="btn-file-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'file\')">File</button>'
          + '<button class="btn interact-tab" id="btn-metadata-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'metadata\')">Metadata</button>'
          + '<button class="btn interact-tab" id="btn-automations-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'automations\')">Automations</button>'
          + '<button class="btn interact-tab" id="btn-sensors-' + mac + '" onclick="toggleMessageType(\'' + mac + '\', \'sensors\')">Sensor Streaming</button>'
          + '</div>'
          + '<div>'
          + '<div class="message-log" id="log-' + mac + '" style="margin-bottom:12px;max-height:300px;overflow-y:auto"><div class="message-empty">No messages yet. Start a conversation!</div></div>'
          + '<div id="text-input-' + mac + '" style="display:block" data-guest-hide>'
          + '<div style="display:flex;gap:8px;align-items:flex-start;flex-wrap:wrap">'
          + '<textarea id="msg-' + mac + '" maxlength="1024" oninput="var c=hw.$(this.id.replace(\'msg-\',\'msg-count-\'));hw.setText(c,(1024-this.value.length)+\' characters left\');" placeholder="Message to send" style="flex:1;min-width:220px;min-height:60px;resize:vertical;font-family:inherit;padding:8px;border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg)"></textarea>'
          + '<button class="btn message-action-btn" onclick="doSendMessage(\'' + mac + '\')" style="align-self:flex-start">Send</button>'
          + '</div>'
          + '<div id="msg-count-' + mac + '" style="font-size:.78em;color:var(--muted);margin-top:5px">1024 characters left</div>'
          + '<div style="font-size:.72em;color:var(--muted);margin-top:2px">Longer messages are auto-split. For large content, use the File tab.</div>'
          + '</div>'
          + '<div id="remote-input-' + mac + '" style="display:none" data-guest-hide>'
          + '<div class="input-group" style="margin-bottom:8px">'
          + '<input type="text" id="ru-' + mac + '" placeholder="Username" style="flex:1">'
          + '<input type="password" id="rp-' + mac + '" placeholder="Password" style="flex:1">'
          + '</div>'
          + '<div style="display:flex;gap:8px;align-items:flex-start">'
          + '<input type="text" id="rc-' + mac + '" placeholder="Command (e.g., sensors, memory)" style="flex:1;padding:8px;border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg)">'
          + '<button class="btn message-action-btn" onclick="doRemoteExec(\'' + mac + '\')" style="align-self:flex-start">Execute</button>'
          + '</div>'
          + '</div>'
          + '<div id="file-input-' + mac + '" style="display:none" data-guest-hide>'
          + '<div style="display:flex;gap:8px;margin-bottom:12px">'
          + '<button class="btn message-action-btn" id="btn-file-send-' + mac + '" onclick="toggleFileMode(\'' + mac + '\', \'send\')" style="flex:1">Send File</button>'
          + '<button class="btn message-action-btn" id="btn-file-receive-' + mac + '" onclick="toggleFileMode(\'' + mac + '\', \'receive\')" style="flex:1">Receive File</button>'
          + '</div>'
          + '<div id="file-send-panel-' + mac + '" style="display:block">'
          + '<div style="margin-bottom:12px">'
          + '<label style="display:block;margin-bottom:6px;font-weight:500;color:var(--panel-fg)">Browse Local Files:</label>'
          + '<div id="fexplorer-' + mac + '"></div>'
          + '</div>'
          + '<div style="margin-bottom:10px">'
          + '<label style="display:block;margin-bottom:5px;font-weight:500;color:var(--panel-fg)">File Path:</label>'
          + '<input type="text" class="input-fit input-l" id="fp-' + mac + '" placeholder="/path/to/file.ext or select from explorer" style="padding:8px;border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg)">'
          + '<small style="color:var(--panel-fg);font-size:0.85em">Click a file in the explorer above or enter path manually</small>'
          + '</div>'
          + '<button class="btn message-action-btn" onclick="doSendFile(\'' + mac + '\')">Send File</button>'
          + '<div id="fstat-' + mac + '" style="margin-top:8px;padding:8px;border-radius:4px;font-size:0.9em;color:var(--panel-fg)">Select a file from the explorer or enter a file path manually</div>'
          + '</div>'
          + '<div id="file-receive-panel-' + mac + '" style="display:none">'
          + '<div class="input-group" style="margin-bottom:12px">'
          + '<input type="text" id="remote-user-' + mac + '" placeholder="Username" style="flex:1">'
          + '<input type="password" id="remote-pass-' + mac + '" placeholder="Password" style="flex:1">'
          + '<button class="btn message-action-btn" onclick="browseRemoteFiles(\'' + mac + '\', window.remoteCurrentPath && window.remoteCurrentPath[\'' + mac + '\'] ? window.remoteCurrentPath[\'' + mac + '\'] : \'/\')">Browse</button>'
          + '</div>'
          + '<div style="margin-bottom:12px">'
          + '<label style="display:block;margin-bottom:6px;font-weight:500;color:var(--panel-fg)">Remote Files:</label>'
          + '<div id="remote-fexplorer-' + mac + '" style="min-height:220px"></div>'
          + '</div>'
          + '<div style="margin-bottom:10px">'
          + '<label style="display:block;margin-bottom:5px;font-weight:500;color:var(--panel-fg)">Remote File Path:</label>'
          + '<input type="text" class="input-fit input-l" id="remote-fp-' + mac + '" placeholder="/path/to/remote/file.ext" style="padding:8px;border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg)">'
          + '</div>'
          + '<button class="btn message-action-btn" onclick="fetchRemoteFile(\'' + mac + '\')">Fetch File</button>'
          + '<div id="remote-fstat-' + mac + '" style="margin-top:8px;padding:8px;border-radius:4px;font-size:0.9em;color:var(--panel-fg)">Enter credentials and browse remote device</div>'
          + '</div>'
          + '</div>'
          + '<div id="metadata-' + mac + '" style="display:none;padding:12px;background:var(--crumb-bg);border-radius:8px;min-height:200px">'
          + '<div style="margin-bottom:10px;text-align:right">'
          + '<button class="btn" data-guest-hide onclick="syncMetadata(\'' + mac + '\')">Sync Metadata</button>'
          + '</div>'
          + '<div id="metadata-content-' + mac + '"><div style="text-align:center;color:var(--panel-fg);padding:20px">No metadata available. Click Sync Metadata to request from device.</div></div>'
          + '</div>'
          + '<div id="automations-input-' + mac + '" style="display:none" data-guest-hide>'
          + automationsInnerHtml(mac)
          + '</div>'
          + '<div id="sensors-input-' + mac + '" style="display:none;padding:12px;background:var(--crumb-bg);border-radius:8px;min-height:200px" data-guest-hide>'
          + '<p style="color:var(--panel-fg);font-size:0.9em;margin:0 0 10px 0">Select which sensors should stream, then apply the changes. Credentials are required.</p>'
          + '<div class="input-group" style="margin-bottom:12px">'
          + '<input type="text" id="su-' + mac + '" placeholder="Username" style="flex:1">'
          + '<input type="password" id="sp-' + mac + '" placeholder="Password" style="flex:1">'
          + '</div>'
          + '<div class="sensor-grid">'
          + ['thermal','tof','imu','gps','input','fmradio','rtc','presence'].map(function(s){
              return '<div class="sensor-pill" id="sensor-pill-' + s + '-' + mac + '" onclick="toggleSensorSelection(\'' + mac + '\',\'' + s + '\')">' + s + '</div>';
            }).join('')
          + '</div>'
          + '<div style="display:flex;flex-wrap:wrap;gap:8px;margin-bottom:10px">'
          + '<button class="btn message-action-btn" onclick="applySensorStreaming(\'' + mac + '\')">Apply Streaming</button>'
          + '<div style="flex:1"></div>'
          + '<button class="btn message-action-btn" onclick="doSensorBroadcast(\'' + mac + '\', true)">Broadcast On</button>'
          + '<button class="btn message-action-btn" onclick="doSensorBroadcast(\'' + mac + '\', false)">Broadcast Off</button>'
          + '</div>'
          + '<div id="sensor-status-' + mac + '" style="font-size:0.85em;color:var(--muted)"></div>'
          + '</div>'
          + '</div>'
        );
      }
      if (kind === 'broadcast') {
        return (
          '<div style="margin-bottom:12px">'
          + '<p style="color:var(--panel-fg);margin-bottom:12px;">Send a message to all paired devices simultaneously.</p>'
          + '</div>'
          + '<div style="display:flex;gap:8px;align-items:flex-start;flex-wrap:wrap">'
          + '<textarea id="broadcast-msg" placeholder="Broadcast message to all devices" style="flex:1;min-width:220px;min-height:60px;resize:vertical;font-family:inherit;padding:8px;border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg)"></textarea>'
          + '<button class="btn" onclick="doBroadcast()" style="align-self:flex-start">Send Broadcast</button>'
          + '</div>'
          + '<div id="broadcast-status" style="margin-top:12px;padding:8px;border-radius:4px;display:none;"></div>'
        );
      }
      return '<div>Unknown panel</div>';
    };
    console.log('[ESP-NOW] Chunk 3F: renderDevicePanel ready');
    console.log('[ESP-NOW] Chunk 3G: appendLogLine start');
    window.appendLogLine = function(containerId, type, message, status) {
      console.log('[appendLogLine] Called with:', {containerId, type, message, status});
      const log = hw.$(containerId);
      if (!log) {
        console.warn('[appendLogLine] Container not found:', containerId);
        return;
      }
      
      // Dedup: skip if same RECEIVED message was just added to this container within 2s
      if (type === 'RECEIVED' || type === 'ERROR') {
        if (!window.__logDedup) window.__logDedup = {};
        var dedupKey = containerId + '|' + type + '|' + message;
        var now = Date.now();
        if (window.__logDedup[dedupKey] && (now - window.__logDedup[dedupKey]) < 2000) {
          console.log('[appendLogLine] Dedup: skipping duplicate message');
          return null;
        }
        window.__logDedup[dedupKey] = now;
        // Prune old dedup entries every 50 messages
        if (!window.__logDedupCount) window.__logDedupCount = 0;
        if (++window.__logDedupCount % 50 === 0) {
          for (var k in window.__logDedup) {
            if (now - window.__logDedup[k] > 5000) delete window.__logDedup[k];
          }
        }
      }
      console.log('[appendLogLine] Container found, appending message');
      
      // Remove empty state message if present
      const emptyMsg = log.querySelector('.message-empty');
      if (emptyMsg) emptyMsg.remove();
      
      const ts = new Date().toLocaleTimeString();
      const bubble = document.createElement('div');
      bubble.className = 'message-bubble ' + (type==='ERROR'?'message-error': type==='RECEIVED'?'message-received':'message-sent');
      
      const textDiv = document.createElement('div');
      textDiv.className = 'message-text';
      textDiv.textContent = message;
      
      const statusDiv = document.createElement('div');
      statusDiv.className = 'message-status';
      
      // Determine status icon and text
      let statusIcon = '';
      let statusText = '';
      if (type === 'RECEIVED') {
        statusIcon = 'RX';
        statusText = ts;
      } else if (type === 'ERROR') {
        statusIcon = 'ERR';
        statusText = status || 'Failed';
      } else if (type === 'SENT') {
        if (status === 'sending') {
          statusIcon = '...';
          statusText = 'Sending...';
        } else if (status === 'sent') {
          statusIcon = 'OK';
          statusText = 'Sent';
        } else if (status === 'delivered') {
          statusIcon = 'OK';
          statusText = 'Delivered';
        } else {
          statusIcon = 'OK';
          statusText = ts;
        }
      }
      
      statusDiv.innerHTML = '<span class="status-icon">' + statusIcon + '</span>' + statusText;
      
      bubble.appendChild(textDiv);
      bubble.appendChild(statusDiv);
      log.appendChild(bubble);
      log.scrollTop = log.scrollHeight;
      console.log('[appendLogLine] Message appended successfully');
      
      return bubble;
    };
    console.log('[ESP-NOW] Chunk 3G: appendLogLine ready');
    console.log('[ESP-NOW] Chunk 3H: doSendMessage start');
    window.doSendMessage = function(mac) {
      const val = (hw.$('msg-' + mac) || {}).value || '';
      if (!val) { alert('Enter a message'); return; }
      
      // Clear input immediately
      const input = hw.$('msg-' + mac);
      if (input) input.value = '';
      
      // Show message as "sending" immediately
      const bubble = appendLogLine('log-' + mac, 'SENT', val, 'sending');
      
      // Use already-tracked mode — no extra round-trip needed
      var cmd = 'espnowsend ' + mac + ' ' + val;
      hw.postFormText('/api/cli', { cmd: cmd })
        .then(t=>{
          console.log('[ESP-NOW] Send result:', t);
          // Update bubble status based on result.
          // Phase 3.5 task #49 — "✓✓ Delivered" no longer fires immediately on
          // CLI accept. We tag the bubble with the msgId extracted from the
          // response and let the existing /api/espnow/messages poll loop flip
          // the bubble to Delivered (or Failed/Timeout) once the server
          // tracker reports the terminal state from the ACK.
          if (bubble) {
            const statusDiv = bubble.querySelector('.message-status');
            const lowerResult = (t || '').toLowerCase();
            if (lowerResult.indexOf('failed') >= 0 || lowerResult.indexOf('error') >= 0) {
              statusDiv.innerHTML = '<span class="status-icon">✗</span>Failed';
            } else {
              // Extract "ID: <num>" from the response and tag the bubble so
              // the poller can match the eventual delivery event.
              var idMatch = (t || '').match(/ID:\s*(\d+)/i);
              if (idMatch) {
                bubble.setAttribute('data-msg-id', idMatch[1]);
              }
              if (lowerResult.indexOf('queued') >= 0) {
                statusDiv.innerHTML = '<span class="status-icon">⌛</span>Queued';
              } else {
                statusDiv.innerHTML = '<span class="status-icon">✓</span>Sent';
              }
            }
          }
        })
        .catch(e=> {
          // Update bubble to show error
          if (bubble) {
            bubble.className = 'message-bubble message-error';
            const statusDiv = bubble.querySelector('.message-status');
            statusDiv.innerHTML = '<span class="status-icon">✗</span>Failed: ' + e.message;
          }
        });
    };
    console.log('[ESP-NOW] Chunk 3H: doSendMessage ready');
    console.log('[ESP-NOW] Chunk 3I: doBroadcast start');
    window.doBroadcast = function(){
      const input = hw.$('broadcast-msg');
      const statusDiv = hw.$('broadcast-status');
      const msg = (input || {}).value || '';
      
      if (!msg) { 
        alert('Enter a broadcast message'); 
        return; 
      }
      
      // Show sending status
      if (statusDiv) {
        statusDiv.style.display = 'block';
        statusDiv.style.background = '#fff3cd';
        statusDiv.style.color = '#856404';
        statusDiv.textContent = 'Broadcasting message...';
      }
      
      hw.postFormText('/api/cli', { cmd: 'espnowbroadcast ' + msg })
        .then(()=> {
          // Clear input
          if (input) input.value = '';
          
          // Show success feedback
          if (statusDiv) {
            statusDiv.style.background = '#d4edda';
            statusDiv.style.color = '#155724';
            statusDiv.innerHTML = '<strong>Broadcast sent successfully!</strong><br><small>Message: "' + msg + '"</small>';
            setTimeout(function() { 
              statusDiv.style.display = 'none'; 
            }, 5000);
          }
        })
        .catch(e=> {
          // Show error feedback
          if (statusDiv) {
            statusDiv.style.display = 'block';
            statusDiv.style.background = '#f8d7da';
            statusDiv.style.color = '#721c24';
            statusDiv.textContent = 'Broadcast failed: ' + e.message;
          } else {
            alert('Broadcast error: ' + e.message);
          }
        });
    };
    console.log('[ESP-NOW] Chunk 3I: doBroadcast ready');
    console.log('[ESP-NOW] Chunk 3J: doSendFile start');
    window.doSendFile = function(mac) {
      const path = (hw.$('fp-' + mac) || {}).value || '';
      if (!path) { 
        alert('Enter a file path or select a file from the explorer'); 
        return; 
      }
      
      const statDiv = hw.$('fstat-' + mac);
      const filename = path.split('/').pop();
      
      // Show sending status
      if (statDiv) {
        statDiv.style.background = '#fff3cd';
        statDiv.style.color = '#856404';
        statDiv.textContent = 'Sending file: ' + filename + '...';
      }
      
      // Also show in message log
      appendLogLine('log-' + mac, 'SENT', 'Sending file: ' + filename, 'sending');
      
      hw.postFormText('/api/cli', { cmd: 'espnowsendfile ' + mac + ' "' + path + '"' })
        .then(t=>{
          // An explicit "Error:" prefix always wins: several failure strings
          // legitimately contain the word "sent" (e.g. "...; not sent"), which
          // this substring sniff would otherwise paint as success.
          const success = t.indexOf('Error') !== 0 &&
                          (t.toLowerCase().indexOf('success') >= 0 || t.toLowerCase().indexOf('sent') >= 0);
          
          if (statDiv) {
            if (success) {
              statDiv.style.background = '#d4edda';
              statDiv.style.color = '#155724';
              statDiv.textContent = t;
            } else {
              statDiv.style.background = '#f8d7da';
              statDiv.style.color = '#721c24';
              statDiv.textContent = t;
            }
          }
          
          // Update message log with result
          if (success) {
            appendLogLine('log-' + mac, 'RECEIVED', 'File sent: ' + filename, null);
          } else {
            appendLogLine('log-' + mac, 'ERROR', 'File transfer failed: ' + t, null);
          }
          
          // Clear file path input on success
          if (success) {
            const fpInput = hw.$('fp-' + mac);
            if (fpInput) fpInput.value = '';
          }
        })
        .catch(e=>{
          if (statDiv) {
            statDiv.style.background = '#f8d7da';
            statDiv.style.color = '#721c24';
            statDiv.textContent = 'Error: ' + e.message;
          }
          appendLogLine('log-' + mac, 'ERROR', 'File transfer error: ' + e.message, null);
        });
    };
    console.log('[ESP-NOW] Chunk 3J: doSendFile ready');
    console.log('[ESP-NOW] Chunk 3K: toggleMessageType start');
    window.toggleMessageType = function(mac, type) {
      const textInput = hw.$('text-input-' + mac);
      const remoteInput = hw.$('remote-input-' + mac);
      const fileInput = hw.$('file-input-' + mac);
      const metadataDiv = hw.$('metadata-' + mac);
      const automationsInput = hw.$('automations-input-' + mac);
      const sensorsInput = hw.$('sensors-input-' + mac);
      const messageLog = hw.$('log-' + mac);
      const btnText = hw.$('btn-text-' + mac);
      const btnRemote = hw.$('btn-remote-' + mac);
      const btnFile = hw.$('btn-file-' + mac);
      const btnMetadata = hw.$('btn-metadata-' + mac);
      const btnAutomations = hw.$('btn-automations-' + mac);
      const btnSensors = hw.$('btn-sensors-' + mac);
      
      if (!textInput || !remoteInput || !fileInput) return;
      
      // Reset all button styles
      [btnText, btnRemote, btnFile, btnMetadata, btnAutomations, btnSensors].forEach(function(btn) {
        if (btn) btn.classList.remove('interact-tab-active');
      });
      if (btnRemote) {
        btnRemote.style.background = '';
        btnRemote.style.color = '';
        btnRemote.style.border = '';
      }
      
      // Hide all inputs and metadata
      textInput.style.display = 'none';
      remoteInput.style.display = 'none';
      fileInput.style.display = 'none';
      hw.hide(metadataDiv);
      hw.hide(automationsInput);
      hw.hide(sensorsInput);
      
      // messageLog already declared as const above, reuse it
      
      // Show selected input and highlight button
      if (type === 'text') {
        textInput.style.display = 'block';
        if (messageLog) messageLog.style.display = 'block';
        if (btnText) btnText.classList.add('interact-tab-active');
      } else if (type === 'remote') {
        remoteInput.style.display = 'block';
        if (messageLog) messageLog.style.display = 'block';
        if (btnRemote) btnRemote.classList.add('interact-tab-active');
      } else if (type === 'file') {
        fileInput.style.display = 'block';
        hw.hide(messageLog);
        if (btnFile) btnFile.classList.add('interact-tab-active');
        // Initialize file browser when file mode is selected
        if (typeof window.initializeFileBrowser === 'function') {
          setTimeout(function() { window.initializeFileBrowser(mac); }, 100);
        }
      } else if (type === 'metadata') {
        hw.show(metadataDiv);
        hw.hide(messageLog);
        if (btnMetadata) btnMetadata.classList.add('interact-tab-active');
        // Load cached metadata if available; don't auto-request from device
        window.loadPeerMetadata(mac);
      } else if (type === 'automations') {
        hw.show(automationsInput);
        hw.hide(messageLog);
        if (btnAutomations) btnAutomations.classList.add('interact-tab-active');
        // Auto-load already-received automations file if it exists
        window.tryLoadExistingAutomations(mac);
        // Pre-fill credentials from remote tab if available
        var ruEl = hw.$('ru-' + mac);
        var rpEl = hw.$('rp-' + mac);
        var auEl = hw.$('au-' + mac);
        var apEl = hw.$('ap-' + mac);
        if (ruEl && auEl && !auEl.value && ruEl.value) auEl.value = ruEl.value;
        if (rpEl && apEl && !apEl.value && rpEl.value) apEl.value = rpEl.value;
      } else if (type === 'sensors') {
        hw.show(sensorsInput);
        hw.hide(messageLog);
        if (btnSensors) btnSensors.classList.add('interact-tab-active');
        // Reflect what the peer is currently streaming (see loadSensorStreamingState).
        if (window.loadSensorStreamingState) window.loadSensorStreamingState(mac);
        // Pre-fill credentials from remote tab if available
        var ruEl2 = hw.$('ru-' + mac);
        var rpEl2 = hw.$('rp-' + mac);
        var suEl = hw.$('su-' + mac);
        var spEl = hw.$('sp-' + mac);
        if (ruEl2 && suEl && !suEl.value && ruEl2.value) suEl.value = ruEl2.value;
        if (rpEl2 && spEl && !spEl.value && rpEl2.value) spEl.value = rpEl2.value;
      }
    };
    console.log('[ESP-NOW] Chunk 3K: toggleMessageType ready');
    console.log('[ESP-NOW] Chunk 3L: loadRemoteAutomations start');
    window.loadRemoteAutomations = function(mac) {
      var listDiv = hw.$('automations-list-' + mac);
      if (!listDiv) return;
      if (window.__autoFetchState && window.__autoFetchState[mac]) {
        listDiv.innerHTML = '<div style="color:var(--muted);padding:12px;text-align:center">Transfer already in progress...</div>';
        window.setAutoButtonState(mac, { disabled: true, text: 'Transferring...' });
        return;
      }
      var u = (hw.$('au-' + mac) || {}).value || '';
      var p = (hw.$('ap-' + mac) || {}).value || '';
      var esc = (typeof hw !== 'undefined' && hw._esc)
        ? hw._esc
        : function(s){return String(s).replace(/[&<>"]/g,function(c){return({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'})[c]||c;});};
      if (!u || !p) {
        if (typeof hw !== 'undefined') hw.notify('warning', 'Enter username and password', 3000);
        else alert('Enter username and password');
        return;
      }
      if (window.__autoFetchState) window.__autoFetchState[mac] = true;
      window.setAutoButtonState(mac, { disabled: true, text: 'Requesting...' });
      listDiv.innerHTML = '<div style="color:var(--muted);padding:12px;text-align:center">Requesting automations via ESP-NOW...</div>';
      var macHex = mac.replace(/:/g, '').toUpperCase();
      var filePath = '/espnow/received/' + macHex + '/automations.json';
      hw.postFormText('/api/cli', { cmd: 'espnowfetch ' + mac + ' ' + u + ' ' + p + ' "/system/automations.json"' })
      .then(function(resp) {
        var lower = (resp || '').toLowerCase();
        if (lower.indexOf('error') >= 0 || lower.indexOf('not initialized') >= 0) {
          listDiv.innerHTML = '<div style="color:var(--danger);padding:12px">' + esc(resp) + '</div>';
          window.markAutomationsFetchIdle(mac, 'Retry Automations');
          return;
        }
        var attempts = 0;
        listDiv.innerHTML = '<div style="color:var(--muted);padding:12px;text-align:center">Transfer in progress (1/30)...</div>';
        function poll() {
          attempts++;
          window.setAutoButtonState(mac, { disabled: true, text: 'Receiving ' + attempts + '/30' });
          fetch('/api/files/read?name=' + encodeURIComponent(filePath))
            .then(function(r) {
              if (r.status === 404) {
                if (attempts < 30) {
                  listDiv.innerHTML = '<div style="color:var(--muted);padding:12px;text-align:center">Transfer in progress (' + attempts + '/30)...</div>';
                  setTimeout(poll, 1000);
                } else {
                  listDiv.innerHTML = '<div style="color:var(--danger);padding:12px">Timed out. Is encryption enabled and both devices securely paired?</div>';
                  window.markAutomationsFetchIdle(mac, 'Retry Automations');
                }
                return null;
              }
              if (!r.ok) throw new Error('HTTP ' + r.status);
              return r.text();
            })
            .then(function(text) {
              if (text === null || text === undefined) return;
              // File is now cached — use tryLoadExistingAutomations which renders
              // with proper click handlers, Run buttons, and event listeners
              window.tryLoadExistingAutomations(mac);
              if (typeof hw !== 'undefined') hw.notify('success', 'Automations loaded', 2000);
              // Transfer done — reset the button out of its "Receiving N/30" state
              // (the success path previously left it stuck) and clear __autoFetchState.
              window.markAutomationsFetchIdle(mac, 'Refresh Automations');
            })
            .catch(function(e) {
              listDiv.innerHTML = '<div style="color:var(--danger);padding:12px">Error: ' + esc(e.message) + '</div>';
              window.markAutomationsFetchIdle(mac, 'Retry Automations');
            });
        }
        setTimeout(poll, 1500);
      })
      .catch(function(e) {
        listDiv.innerHTML = '<div style="color:var(--danger);padding:12px">Error: ' + esc(e.message) + '</div>';
        window.markAutomationsFetchIdle(mac, 'Retry Automations');
      });
    };
    console.log('[ESP-NOW] Chunk 3L: loadRemoteAutomations ready');
    console.log('[ESP-NOW] Chunk 3M: doRemoteExec start');
    window.doRemoteExec = function(mac) {
      const u = (hw.$('ru-' + mac) || {}).value || '';
      const p = (hw.$('rp-' + mac) || {}).value || '';
      const c = (hw.$('rc-' + mac) || {}).value || '';
      if (!u || !p || !c) { alert('Enter username, password, and command'); return; }
      
      // Show command being executed in message log
      const sendingBubble = appendLogLine('log-' + mac, 'SENT', 'Remote: ' + c, 'sending');
      
      // Remote command output is picked up by the message polling loop
      // (/api/espnow/messages) which runs every 500ms — no SSE needed.
      const cmd = 'espnowremote ' + mac + ' ' + u + ' ' + p + ' ' + c;
      hw.postFormText('/api/cli', { cmd: cmd })
        .then(t=>{
          console.log('[ESP-NOW] Remote exec result:', t);
          // Update sending bubble to show completion
          if (sendingBubble) {
            const statusDiv = sendingBubble.querySelector('.message-status');
            const lowerResult = (t || '').toLowerCase();
            // Check for ACK confirmation (v2 protocol with ACK)
            if (lowerResult.indexOf('failed') >= 0 || lowerResult.indexOf('error') >= 0) {
              statusDiv.innerHTML = '<span class="status-icon">✗</span>Failed';
            } else if (lowerResult.indexOf('remote command sent') >= 0) {
              statusDiv.innerHTML = '<span class="status-icon">✓</span>Sent';
            } else {
              statusDiv.innerHTML = '<span class="status-icon">✓</span>' + t;
            }
          }
          
          // Show immediate result if it's not just the "sent" confirmation
          if (t && !t.includes('Remote command sent')) {
            appendLogLine('log-' + mac, 'RECEIVED', 'Result: ' + t, null);
          }
          
          // Clear command input
          const cmdInput = hw.$('rc-' + mac);
          if (cmdInput) cmdInput.value = '';
        })
        .catch(()=> {
          // Update sending bubble to show error
          if (sendingBubble) {
            sendingBubble.className = 'message-bubble message-error';
            const textDiv = sendingBubble.querySelector('.message-text');
            hw.setText(textDiv, 'Remote: ' + c + ' (FAILED)');
            const statusDiv = sendingBubble.querySelector('.message-status');
            hw.setHTML(statusDiv, '<span class="status-icon">✗</span>Failed');
          }
        });
    };
    console.log('[ESP-NOW] Chunk 3M: doRemoteExec ready');
    console.log('[ESP-NOW] Chunk 3N: toggleFileMode start');
    window.toggleFileMode = function(mac, mode) {
      const sendPanel = hw.$('file-send-panel-' + mac);
      const receivePanel = hw.$('file-receive-panel-' + mac);
      const btnSend = hw.$('btn-file-send-' + mac);
      const btnReceive = hw.$('btn-file-receive-' + mac);
      
      if (!sendPanel || !receivePanel || !btnSend || !btnReceive) return;
      
      if (mode === 'send') {
        sendPanel.style.display = 'block';
        receivePanel.style.display = 'none';
        btnSend.style.background = 'var(--link)';
        btnSend.style.color = 'white';
        btnReceive.style.background = '';
        btnReceive.style.color = '';
      } else if (mode === 'receive') {
        sendPanel.style.display = 'none';
        receivePanel.style.display = 'block';
        btnSend.style.background = '';
        btnSend.style.color = '';
        btnReceive.style.background = 'var(--link)';
        btnReceive.style.color = 'white';
      }
    };
    console.log('[ESP-NOW] Chunk 3N: toggleFileMode ready');
    console.log('[ESP-NOW] Chunk 3O: browseRemoteFiles start');
    window.browseRemoteFiles = function(mac, path) {
      var u = (hw.$('remote-user-' + mac) || {}).value || '';
      var p = (hw.$('remote-pass-' + mac) || {}).value || '';
      var container = hw.$('remote-fexplorer-' + mac);
      var statusDiv = hw.$('remote-fstat-' + mac);
      
      if (!u || !p) {
        hw.setText(statusDiv, 'Enter username and password first');
        return;
      }
      
      if (!container) return;
      container.innerHTML = '<div class="remote-explorer"><div class="remote-explorer-crumb">Loading...</div><div class="remote-entry remote-entry-empty" style="display:flex">Requesting directory...</div></div>';
      hw.setText(statusDiv, 'Requesting directory listing from ' + mac + '...');
      
      var targetMac = String(mac || '').toUpperCase();
      var browsePath = path || '/';
      var seqBefore = 0;
      
      // Get current max sequence number so we only look at NEW messages
      hw.fetchJSON('/api/espnow/messages?since=0')
        .then(function(data) {
          var msgs = Array.isArray(data) ? data : (data.messages || []);
          for (var i = 0; i < msgs.length; i++) {
            var s = msgs[i].seq || msgs[i].seqNum || 0;
            if (s > seqBefore) seqBefore = s;
          }
        })
        .catch(function() {})
        .finally(function() {
      
      // Send browse command (sends V3 CMD: user:pass:files "/path"). The remote
      // 'files' command requires the path in quotes (quoted-path standard), so
      // quote it here — an unquoted path is rejected with "path must be in quotes".
      var cmd = 'espnowremote ' + mac + ' ' + u + ' ' + p + ' files "' + browsePath + '"';
      hw.postFormText('/api/cli', { cmd: cmd })
      .then(function(text) {
        if (!text.includes('Remote command sent')) {
          container.innerHTML = '<pre style="margin:0;white-space:pre-wrap;font-size:0.85em">' + text + '</pre>';
          hw.setText(statusDiv, text);
          return;
        }
        hw.setText(statusDiv, 'Request sent, waiting for response...');

        // Poll peer messages for streamed file listing output.
        // Two notes about the polling design (mirror of the BondFs fix):
        //  1. seqBefore is local to this browse session — no shared mutable
        //     cursor across concurrent browses (would race the same way the
        //     bonded file viewer did before the BondFs.exec rewrite).
        //  2. doneMarker uses ' entries' (with leading space) rather than
        //     'Total:' — the file listing terminates with "Total: N entries"
        //     but other commands the peer may stream (fsusage emits
        //     "Total: NNNN bytes") would falsely trigger completion if we
        //     matched bare "Total:". This same collision broke the bonded
        //     viewer; fixing it preemptively here too.
        // We also advance seqBefore as we consume messages so each poll only
        // fetches new ones — earlier this loop re-processed every message
        // since the initial baseline on every tick.
        var pollCount = 0;
        var maxPolls = 15;
        var pollInterval = setInterval(function() {
          pollCount++;
          if (pollCount > maxPolls) {
            clearInterval(pollInterval);
            container.innerHTML = '<div class="remote-explorer"><div class="remote-explorer-crumb">' + browsePath + '</div><div class="remote-entry remote-entry-empty" style="display:flex">Timed out waiting for response</div></div>';
            hw.setText(statusDiv, 'Timed out');
            return;
          }

          hw.fetchJSON('/api/espnow/messages?since=' + seqBefore)
            .then(function(data) {
              var msgs = Array.isArray(data) ? data : (data.messages || []);
              var browseLines = [];
              var foundComplete = false;

              for (var i = 0; i < msgs.length; i++) {
                var m = msgs[i];
                var mMac = String(m.mac || m.from || '').toUpperCase();
                var mMsg = String(m.message || m.msg || m.text || '');
                var mSeq = m.seq || m.seqNum || 0;
                if (mSeq > seqBefore) seqBefore = mSeq;

                if (mMac !== targetMac) continue;

                // Match streamed file listing output from 'files' command.
                // Note: we INTENTIONALLY no longer include 'Total:' in the
                // line-keep whitelist — the file listing's terminator
                // "Total: N entries" is detected as the completion marker
                // (see the ' entries' check below), not parsed as content.
                if (mMsg.indexOf('Files (') >= 0 ||
                    mMsg.indexOf('items)') >= 0 ||
                    mMsg.indexOf('bytes)') >= 0 ||
                    mMsg.indexOf('[DIR]') >= 0 ||
                    mMsg.indexOf('[FILE]') >= 0 ||
                    mMsg.indexOf('File listing') >= 0 ||
                    mMsg.indexOf('File browse FAILED') >= 0 ||
                    mMsg.indexOf('empty directory') >= 0 ||
                    mMsg.indexOf('Directory not found') >= 0 ||
                    mMsg.indexOf('Error:') >= 0) {
                  browseLines.push(mMsg);
                }
                // doneMarker — see comment block above re: ' entries' vs 'Total:'
                if (mMsg.indexOf(' entries') >= 0) foundComplete = true;
              }

              if (foundComplete && browseLines.length > 0) {
                clearInterval(pollInterval);
                // Flatten multi-line messages into individual lines before parsing
                var flatLines = [];
                for (var li = 0; li < browseLines.length; li++) {
                  var subLines = browseLines[li].split('\n');
                  for (var si = 0; si < subLines.length; si++) {
                    flatLines.push(subLines[si]);
                  }
                }
                var entries = window.parseRemoteFileListing(flatLines);
                // Render with the SHARED file-explorer (window.BondFs.renderExplorer +
                // window.FileBrowser) so the remote browser matches the local / Files /
                // Logging views — same icons, breadcrumb, hover rows. Data source stays
                // the espnowbrowse text-scrape (arbitrary peer + credentials), so we
                // feed the parsed {name,isDir,size} entries straight in. peerLabel shows
                // the peer MAC (not the bonded peer). Track current path for the Browse
                // button (the old renderer used to set this).
                window.remoteCurrentPath = window.remoteCurrentPath || {};
                window.remoteCurrentPath[mac] = browsePath;
                window.BondFs.renderExplorer('remote-fexplorer-' + mac, browsePath, entries, {
                  peerLabel: mac,
                  onNavigate: function(p) { window.browseRemoteFiles(mac, p); },
                  fileActions: [{
                    label: 'Select',
                    title: 'Select this file — then click Fetch File to download it to this device',
                    fn: function(full) {
                      var input = hw.$('remote-fp-' + mac);
                      if (input) input.value = full;
                      var sd = hw.$('remote-fstat-' + mac);
                      hw.setText(sd, 'Selected: ' + full + ' — click Fetch File to download');
                    }
                  }]
                });
                hw.setText(statusDiv, 'Browse complete - ' + entries.length + ' items in ' + browsePath);
              } else if (pollCount > 2) {
                hw.setText(statusDiv, 'Waiting for response... (' + pollCount + '/' + maxPolls + ')');
              }
            })
            .catch(function() {});
        }, 1000);
      })
      .catch(function(e) {
        container.innerHTML = '<div style="color:var(--danger);padding:12px">Error: ' + e.message + '</div>';
        hw.setText(statusDiv, 'Browse error: ' + e.message);
      });
      
      }); // end of .finally() from seqBefore fetch
    };
    console.log('[ESP-NOW] Chunk 3O: browseRemoteFiles ready');
    console.log('[ESP-NOW] Chunk 3P: parseRemoteFileListing start');
    // Parse the human-readable text listing produced by the remote 'files'
    // command (System_Filesystem.cpp buildFilesListing, asJson=false). Lines:
    //   "Files (/path):"            header  -> skipped
    //   "  name (123 bytes)"        file    -> {isDir:false}
    //   "  name (3 items)"          folder  -> {isDir:true}
    //   "  name (3 items) [mount]"  mount   -> {isDir:true}
    //   "Total: N entries"          footer  -> skipped
    //   "  No files found"          empty   -> skipped
    window.parseRemoteFileListing = function(flatLines) {
      var entries = [];
      var seen = {};
      if (!flatLines) return entries;
      var re = /^\s+(.+?)\s+\((\d+)\s+(items|bytes)\)(\s+\[mount\])?\s*$/;
      for (var i = 0; i < flatLines.length; i++) {
        var line = String(flatLines[i] || '');
        if (!line) continue;
        if (line.indexOf('Files (') >= 0) continue;
        if (line.indexOf('Total:') >= 0) continue;
        if (line.indexOf('No files found') >= 0) continue;
        var m = line.match(re);
        if (!m) continue;
        var name = m[1].trim();
        if (!name || seen[name]) continue;
        seen[name] = true;
        var count = parseInt(m[2], 10);
        var isDir = (m[3] === 'items') || !!m[4];
        entries.push({
          name: name,
          isDir: isDir,
          isMount: !!m[4],
          size: count,
          meta: isDir ? (count + ' items') : (count + ' bytes')
        });
      }
      // Folders first, then files; alphabetical within each group.
      entries.sort(function(a, b) {
        if (a.isDir !== b.isDir) return a.isDir ? -1 : 1;
        return a.name < b.name ? -1 : (a.name > b.name ? 1 : 0);
      });
      return entries;
    };
    console.log('[ESP-NOW] Chunk 3P: parseRemoteFileListing ready');
    window.fetchRemoteFile = function(mac) {
      var u = (hw.$('remote-user-' + mac) || {}).value || '';
      var p = (hw.$('remote-pass-' + mac) || {}).value || '';
      var remotePath = (hw.$('remote-fp-' + mac) || {}).value || '';
      var statusDiv = hw.$('remote-fstat-' + mac);
      if (!u || !p || !remotePath) {
        hw.setText(statusDiv, 'Enter username, password, and remote file path');
        return;
      }
      var filename = remotePath.split('/').pop();
      if (statusDiv) {
        statusDiv.style.background = '#fff3cd';
        statusDiv.style.color = '#856404';
        statusDiv.textContent = 'Fetching ' + filename + ' from ' + mac + '...';
      }
      // Quote the path — espnowfetch requires a quoted path (quoted-path standard).
      var cmd = 'espnowfetch ' + mac + ' ' + u + ' ' + p + ' "' + remotePath + '"';
      hw.postFormText('/api/cli', { cmd: cmd }).then(function(text) {
        if (!text.includes('File fetch request sent') && !text.includes('Receiving file')) {
          if (statusDiv) {
            statusDiv.style.background = '#f8d7da';
            statusDiv.style.color = '#721c24';
            statusDiv.textContent = 'Failed: ' + text;
          }
          appendLogLine('log-' + mac, 'ERROR', 'Fetch failed: ' + text, null);
          return;
        }
        appendLogLine('log-' + mac, 'SENT', 'Fetch request sent for: ' + filename, null);
        hw.fetchJSON('/api/espnow/messages?mac=' + mac + '&since=0').then(function(data) {
          var existing = Array.isArray(data) ? data : (data.messages || []);
          var sinceSeq = existing.length > 0 ? (existing[existing.length - 1].seq || 0) : 0;
          var pollCount = 0;
          var pollMax = 15;
          var poll = setInterval(function() {
            pollCount++;
            hw.fetchJSON('/api/espnow/messages?mac=' + mac + '&since=' + sinceSeq).then(function(msgs_data) {
              var msgs = Array.isArray(msgs_data) ? msgs_data : (msgs_data.messages || []);
              for (var i = msgs.length - 1; i >= 0; i--) {
                var m = (msgs[i].msg || '');
                // Match what actually lands in peer history: logFileTransferEvent
                // writes "Received file: <name>" / "Failed to receive: <name>".
                // (This used to test 'File sent successfully', a string the
                // requester never sees — the sending side is the remote peer's
                // FS_GET handler, not a CLI command — so both branches were dead
                // and every fetch fell through to the 15-poll timeout.)
                if (m.indexOf('Received file') >= 0 && m.includes(filename)) {
                  clearInterval(poll);
                  if (statusDiv) {
                    statusDiv.style.background = '#d4edda';
                    statusDiv.style.color = '#155724';
                    statusDiv.textContent = 'Received: ' + filename;
                  }
                  appendLogLine('log-' + mac, 'RECEIVED', 'File received: ' + filename, null);
                  return;
                }
                var ml = m.toLowerCase();
                if ((ml.indexOf('failed') >= 0 || ml.indexOf('error') >= 0) && m.includes(filename)) {
                  clearInterval(poll);
                  if (statusDiv) {
                    statusDiv.style.background = '#f8d7da';
                    statusDiv.style.color = '#721c24';
                    statusDiv.textContent = 'Transfer failed: ' + m;
                  }
                  appendLogLine('log-' + mac, 'ERROR', 'Fetch failed: ' + m, null);
                  return;
                }
              }
              if (pollCount >= pollMax) {
                clearInterval(poll);
                if (statusDiv) {
                  statusDiv.style.background = '#f8d7da';
                  statusDiv.style.color = '#721c24';
                  statusDiv.textContent = 'Timed out waiting for ' + filename;
                }
                appendLogLine('log-' + mac, 'ERROR', 'Fetch timed out: ' + filename, null);
              }
            }).catch(function() { if (pollCount >= pollMax) clearInterval(poll); });
          }, 1000);
        }).catch(function() {});
      }).catch(function(e) {
        if (statusDiv) {
          statusDiv.style.background = '#f8d7da';
          statusDiv.style.color = '#721c24';
          statusDiv.textContent = 'Fetch error: ' + e.message;
        }
        appendLogLine('log-' + mac, 'ERROR', 'Fetch error: ' + e.message, null);
      });
    };
    window.unpairDevice = async function(mac) {
      if (await hwConfirm('Unpair device ' + mac + '?')) {
        hw.postFormText('/api/cli', { cmd: 'espnowunpair ' + mac })
        .then(() => {
          listDevices();
        })
        .catch(error => {
          console.error('[ESP-NOW] Unpair error:', error);
        });
      }
    };
    // Fetch and render a REMOTE/paired peer's cached metadata (by MAC) into that
    // peer's interact-panel Metadata tab. Distinct from loadLocalDeviceMetadata,
    // which loads THIS device's own metadata into the settings form.
    window.loadPeerMetadata = function(mac) {
      const container = hw.$('metadata-content-' + mac)
                     || hw.$('metadata-' + mac);
      if (!container) return;
      
      container.innerHTML = '<div style="text-align:center;color:var(--panel-fg);padding:20px">Loading metadata...</div>';

      hw.fetchJSON('/api/espnow/metadata?mac=' + encodeURIComponent(mac))
        .then(data => {
          if (!data.found) {
            container.innerHTML = '<div style="text-align:center;color:var(--panel-fg);padding:20px">No metadata available for this device</div>';
            return;
          }
          
          var html = '<div style="display:grid;gap:12px">';
          
          if (data.deviceName) {
            html += '<div><label style="font-weight:600;color:var(--panel-fg);display:block;margin-bottom:4px">Device Name</label>';
            html += '<div style="padding:8px;background:var(--panel-bg);border-radius:4px;border:1px solid var(--border)">' + data.deviceName + '</div></div>';
          }
          
          if (data.friendlyName) {
            html += '<div><label style="font-weight:600;color:var(--panel-fg);display:block;margin-bottom:4px">Friendly Name</label>';
            html += '<div style="padding:8px;background:var(--panel-bg);border-radius:4px;border:1px solid var(--border)">' + data.friendlyName + '</div></div>';
          }
          
          if (data.room) {
            html += '<div><label style="font-weight:600;color:var(--panel-fg);display:block;margin-bottom:4px">Room</label>';
            html += '<div style="padding:8px;background:var(--panel-bg);border-radius:4px;border:1px solid var(--border)">' + data.room + '</div></div>';
          }
          
          if (data.zone) {
            html += '<div><label style="font-weight:600;color:var(--panel-fg);display:block;margin-bottom:4px">Zone</label>';
            html += '<div style="padding:8px;background:var(--panel-bg);border-radius:4px;border:1px solid var(--border)">' + data.zone + '</div></div>';
          }
          
          if (data.tags) {
            html += '<div><label style="font-weight:600;color:var(--panel-fg);display:block;margin-bottom:4px">Tags</label>';
            html += '<div style="padding:8px;background:var(--panel-bg);border-radius:4px;border:1px solid var(--border)">' + data.tags + '</div></div>';
          }
          
          html += '<div><label style="font-weight:600;color:var(--panel-fg);display:block;margin-bottom:4px">Stationary</label>';
          html += '<div style="padding:8px;background:var(--panel-bg);border-radius:4px;border:1px solid var(--border)">' + (data.stationary ? 'Yes' : 'No') + '</div></div>';
          
          html += '<div style="margin-top:8px;padding:8px;background:var(--panel-bg);border-radius:4px;font-size:0.85em;color:var(--panel-fg)">';
          html += '<strong>Source:</strong> ' + (data.source === 'mesh' ? 'Mesh/Pairing Mode' : 'Bonded Mode (Cached)');
          html += '</div>';

          html += '</div>';
          
          container.innerHTML = html;
        })
        .catch(e => {
          container.innerHTML = '<div style="text-align:center;color:var(--danger);padding:20px">Error loading metadata: ' + e.message + '</div>';
        });
    };
    window.syncMetadata = function(mac) {
      const container = hw.$('metadata-content-' + mac)
                     || hw.$('metadata-' + mac);
      if (!container) return;
      
      container.innerHTML = '<div style="text-align:center;color:var(--panel-fg);padding:20px">Requesting metadata from device...</div>';
      
      // Send V3 METADATA_REQ to the peer - no credentials needed, it's a protocol-level request
      // The peer responds with METADATA_RESP which populates gMeshPeerMeta on our side
      hw.postFormText('/api/cli', { cmd: 'espnowrequestmeta ' + mac })
        .then(function() {
          appendLogLine('log-' + mac, 'RECEIVED', 'Metadata request sent', null);
          // Poll for metadata to appear (peer responds within ~1-2 seconds)
          var metaPollCount = 0;
          var metaPollInterval = setInterval(function() {
            metaPollCount++;
            if (metaPollCount > 10) {
              clearInterval(metaPollInterval);
              container.innerHTML = '<div style="text-align:center;color:var(--muted);padding:20px">Timed out waiting for metadata response</div>';
              return;
            }
            hw.fetchJSON('/api/espnow/metadata?mac=' + encodeURIComponent(mac))
              .then(function(data) {
                if (data.found) {
                  clearInterval(metaPollInterval);
                  window.loadPeerMetadata(mac);
                }
              })
              .catch(function() {});
          }, 1000);
        })
        .catch(function(e) {
          container.innerHTML = '<div style="text-align:center;color:var(--danger);padding:20px">Sync error: ' + e.message + '</div>';
        });
    };
    window.tryLoadExistingAutomations = function(mac) {
      var listDiv = hw.$('automations-list-' + mac);
      if (!listDiv) return;
      
      var macHex = mac.replace(/:/g, '').toUpperCase();
      var filePath = '/espnow/received/' + macHex + '/automations.json';
      var esc = (typeof hw !== 'undefined' && hw._esc)
        ? hw._esc
        : function(s){return String(s).replace(/[&<>"]/g,function(c){return({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'})[c]||c;});};
      
      listDiv.innerHTML = '<div style="color:var(--muted);padding:12px;text-align:center">Checking for cached automations...</div>';
      
      fetch('/api/files/read?name=' + encodeURIComponent(filePath))
        .then(function(r) {
          if (r.status === 404) {
            listDiv.innerHTML = '<div style="color:var(--muted);padding:20px;text-align:center">No automations file cached. Click "Load Automations" to request from device.</div>';
            return null;
          }
          if (!r.ok) throw new Error('HTTP ' + r.status);
          return r.text();
        })
        .then(function(text) {
          if (text === null || text === undefined) return;
          var trimmed = text.trim();
          if (!trimmed || (trimmed[0] !== '{' && trimmed[0] !== '[')) {
            listDiv.innerHTML = '<div style="color:var(--muted);padding:20px;text-align:center">No automations file cached. Click "Load Automations" to request from device.</div>';
            return;
          }
          try {
            var data = JSON.parse(trimmed);
            var autos = Array.isArray(data.automations) ? data.automations : [];
            if (autos.length === 0) {
              listDiv.innerHTML = '<div style="color:var(--muted);padding:20px;text-align:center">No automations on this device.</div>';
              return;
            }
            var autoUid = 'auto_' + mac.replace(/:/g, '') + '_';
            if (!window.__autoCache) window.__autoCache = {};
            var html = '<div style="display:flex;flex-direction:column;gap:8px">';
            autos.forEach(function(a, idx) {
              // Schedule lives in the `triggers` array (type/time/intervalMs/...),
              // NOT a `schedule` object — reading a.schedule gave '?'/unknown.
              var triggers = Array.isArray(a.triggers) ? a.triggers : [];
              var trig = triggers[0] || {};
              var ttype = trig.type || (triggers.length === 0 ? 'manual' : '?');
              // Cadence = the "Repeat" field (recurrence): daily / weekly / monthly /
              // yearly. A time trigger with no recurrence defaults to daily (matches
              // the local automations table). Without this, "14:02" alone is ambiguous.
              var rec = (trig.recurrence || '').toLowerCase();
              var cadence = (ttype === 'time') ? (rec || 'daily') : '';
              var schedStr = ttype;
              if (ttype === 'time' && trig.time) schedStr = trig.time + (cadence ? ' ' + cadence : '');
              else if (ttype === 'interval' && trig.intervalMs) schedStr = 'every ' + (trig.intervalMs / 1000) + 's';
              else if (ttype === 'boot') schedStr = 'boot';
              if (triggers.length > 1) schedStr += ' +' + (triggers.length - 1);
              var enabled = a.enabled !== false;
              var cmds = Array.isArray(a.commands) ? a.commands : [];
              var conditions = Array.isArray(a.conditions) ? a.conditions : [];
              var dot = '<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:' + (enabled ? '#28a745' : '#dc3545') + ';margin-right:6px;vertical-align:middle"></span>';
              var detailId = autoUid + idx;
              html += '<div class="auto-entry" data-detail-id="' + detailId + '" style="padding:10px 12px;background:var(--crumb-bg);border-radius:8px;border:1px solid var(--border);cursor:pointer">';
              html += '<div style="display:flex;align-items:center;justify-content:space-between;gap:8px">';
              html += '<div>' + dot + '<strong>' + esc(a.name || '(unnamed)') + '</strong></div>';
              html += '<div style="color:var(--muted);font-size:0.85em;white-space:nowrap">' + esc(schedStr) + ' &bull; ' + cmds.length + ' cmd' + (cmds.length !== 1 ? 's' : '') + '</div>';
              html += '</div>';
              html += '<div id="' + detailId + '" style="display:none;margin-top:8px;padding-top:8px;border-top:1px solid var(--border);font-size:0.85em">';
              html += '<div style="margin-bottom:6px"><span style="color:var(--muted)">Schedule:</span> <strong>';
              if (ttype === 'time' && rec === 'monthly') html += 'day ' + esc(String(trig.dayOfMonth || '?')) + ' of each month at ' + esc(trig.time || '?');
              else if (ttype === 'time' && rec === 'yearly') html += esc(String(trig.month || '?')) + '/' + esc(String(trig.dayOfMonth || '?')) + ' at ' + esc(trig.time || '?');
              else if (ttype === 'time') { html += esc(trig.time || '?') + ' (' + esc(cadence) + ')' + (trig.days ? ' on ' + esc(String(trig.days)) : ''); var wi = trig.weekInterval || 1; if (wi > 1) html += ', every ' + wi + ' weeks'; }
              else { html += esc(ttype); if (trig.intervalMs) html += ' every ' + (trig.intervalMs / 1000) + 's'; }
              html += '</strong>';
              html += '</div>';
              if (conditions.length > 0) {
                html += '<div style="margin-bottom:6px"><span style="color:var(--muted)">Conditions:</span>';
                conditions.forEach(function(cond) {
                  var cs = typeof cond === 'string' ? cond : JSON.stringify(cond);
                  html += '<div style="padding-left:12px;color:var(--panel-fg)">' + esc(cs) + '</div>';
                });
                html += '</div>';
              }
              var cmdStrings = cmds.map(function(cmd) { return typeof cmd === 'string' ? cmd : (cmd && cmd.command ? cmd.command : JSON.stringify(cmd)); });
              window.__autoCache[detailId] = { mac: mac, name: a.name || '(unnamed)', cmds: cmdStrings };
              if (cmdStrings.length > 0) {
                html += '<div><span style="color:var(--muted)">Commands:</span>';
                cmdStrings.forEach(function(s) {
                  html += '<div style="padding:2px 0 2px 12px;color:var(--panel-fg);font-family:monospace;font-size:0.9em">' + esc(s) + '</div>';
                });
                html += '</div>';
              }
              html += '<button class="btn auto-run-btn" data-detail-id="' + detailId + '" style="margin-top:8px;width:100%;font-size:0.85em">Run on Device</button>';
              html += '</div>';
              html += '</div>';
            });
            html += '</div>';
            listDiv.innerHTML = html;
            var entries = listDiv.querySelectorAll('.auto-entry');
            entries.forEach(function(entryEl) {
              entryEl.addEventListener('click', function() {
                var did = entryEl.getAttribute('data-detail-id');
                if (did) window.toggleAutoDetail(did);
              });
            });
            var runButtons = listDiv.querySelectorAll('.auto-run-btn');
            runButtons.forEach(function(btnEl) {
              btnEl.addEventListener('click', function(event) {
                event.stopPropagation();
                var did = btnEl.getAttribute('data-detail-id');
                if (did) window.runRemoteAutomation(did, btnEl);
              });
            });
          } catch(e) {
            listDiv.innerHTML = '<div style="color:var(--danger);padding:12px">Parse error: ' + esc(e.message) + '</div>';
          }
        })
        .catch(function() {
          listDiv.innerHTML = '<div style="color:var(--muted);padding:20px;text-align:center">No automations file cached. Click "Load Automations" to request from device.</div>';
        });
    };
    window.toggleAutoDetail = function(detailId) {
      var d = hw.$(detailId);
      if (d) d.style.display = d.style.display === 'none' ? 'block' : 'none';
    };
    window.runRemoteAutomation = function(detailId, btn) {
      var entry = window.__autoCache && window.__autoCache[detailId];
      if (!entry || !entry.cmds || entry.cmds.length === 0) { alert('No commands to run'); return; }
      var mac = entry.mac;
      var u = (hw.$('au-' + mac) || {}).value || '';
      var p = (hw.$('ap-' + mac) || {}).value || '';
      if (!u || !p) { alert('Enter username and password at the top of the Automations tab first.'); return; }
      var cmds = entry.cmds;
      var total = cmds.length;
      var origText = btn.textContent;
      btn.disabled = true;
      btn.textContent = 'Running 1/' + total + '...';
      var i = 0;
      function next() {
        if (i >= total) {
          btn.textContent = 'Done!';
          setTimeout(function() { btn.textContent = origText; btn.disabled = false; }, 2000);
          return;
        }
        var cmd = 'espnowremote ' + mac + ' ' + u + ' ' + p + ' ' + cmds[i];
        btn.textContent = 'Running ' + (i + 1) + '/' + total + '...';
        hw.postFormText('/api/cli', { cmd: cmd })
          .then(function() { i++; next(); })
          .catch(function() {
            btn.textContent = 'Error at cmd ' + (i + 1);
            btn.style.background = '#dc3545';
            setTimeout(function() { btn.textContent = origText; btn.style.background = ''; btn.disabled = false; }, 3000);
          });
      }
      next();
    };
    window.__sensorList = window.__sensorList || ['thermal','tof','imu','gps','input','fmradio','rtc','presence'];
    window.sensorActiveState = window.sensorActiveState || {};
    window.sensorPendingState = window.sensorPendingState || {};
    window.updateSensorStatus = function(mac) {
      var statusDiv = hw.$('sensor-status-' + mac);
      if (!statusDiv) return;
      var pendingCount = Object.keys(window.sensorPendingState[mac] || {}).length;
      statusDiv.textContent = pendingCount ? ('Pending changes: ' + pendingCount) : 'Select sensors and click Apply Streaming.';
    };
    window.updateSensorPill = function(mac, sensor) {
      var pill = hw.$('sensor-pill-' + sensor + '-' + mac);
      if (!pill) return;
      pill.classList.remove('sensor-active','sensor-pending');
      var activeState = window.sensorActiveState[mac] && window.sensorActiveState[mac][sensor] === 'on';
      var pendingMap = window.sensorPendingState[mac] || {};
      if (activeState) pill.classList.add('sensor-active');
      if (pendingMap.hasOwnProperty(sensor)) pill.classList.add('sensor-pending');
    };
    window.ensureSensorState = function(mac) {
      if (!window.sensorActiveState[mac]) window.sensorActiveState[mac] = {};
      if (!window.sensorPendingState[mac]) window.sensorPendingState[mac] = {};
      window.__sensorList.forEach(function(sensor){ window.updateSensorPill(mac, sensor); });
      window.updateSensorStatus(mac);
    };
    // Populate the pills' ACTIVE state from the peer's current streaming status on
    // load/open. Previously the pills were inert until clicked, so a reopened tab
    // showed no indication of what was already streaming. Source: the master's
    // remote-sensor cache — a sensor with FRESH data is one whose stream is
    // currently arriving. No credentials needed (already known from the mesh).
    window.loadSensorStreamingState = function(mac) {
      window.ensureSensorState(mac);
      var macHex = mac.replace(/:/g, '').toLowerCase();
      hw.fetchJSON('/api/sensors/remote').then(function(data) {
        if (!data || !data.devices || !data.devices.forEach) return;
        var dev = null;
        data.devices.forEach(function(d) {
          if (d && d.mac && String(d.mac).replace(/:/g, '').toLowerCase() === macHex) dev = d;
        });
        var active = window.sensorActiveState[mac] || (window.sensorActiveState[mac] = {});
        window.__sensorList.forEach(function(s) { active[s] = 'off'; });
        if (dev && dev.sensors && dev.sensors.forEach) {
          dev.sensors.forEach(function(s) {
            var t = (s && typeof s === 'object') ? s.type : s;
            var streaming = (s && typeof s === 'object') ? !!s.fresh : true;
            if (window.__sensorList.indexOf(t) >= 0) active[t] = streaming ? 'on' : 'off';
          });
        }
        window.__sensorList.forEach(function(s) { window.updateSensorPill(mac, s); });
        window.updateSensorStatus(mac);
      }).catch(function() {});
    };
    window.toggleSensorSelection = function(mac, sensor) {
      window.ensureSensorState(mac);
      var pendingMap = window.sensorPendingState[mac];
      var active = (window.sensorActiveState[mac][sensor] || 'off');
      if (pendingMap.hasOwnProperty(sensor)) {
        delete pendingMap[sensor];
      } else {
        pendingMap[sensor] = active === 'on' ? 'off' : 'on';
      }
      window.updateSensorPill(mac, sensor);
      window.updateSensorStatus(mac);
    };
    window.applySensorStreaming = function(mac) {
      window.ensureSensorState(mac);
      var pendingMap = window.sensorPendingState[mac];
      var entries = Object.entries(pendingMap);
      var statusDiv = hw.$('sensor-status-' + mac);
      var u = (hw.$('su-' + mac) || {}).value || '';
      var p = (hw.$('sp-' + mac) || {}).value || '';
      if (!u || !p) { alert('Enter username and password in Sensor Streaming tab'); return; }
      if (entries.length === 0) {
        hw.setText(statusDiv, 'No pending changes to apply');
        return;
      }
      var idx = 0;
      var results = [];
      function processNext() {
        if (idx >= entries.length) {
          window.updateSensorStatus(mac);
          hw.setText(statusDiv, results.join(' | '));
          return;
        }
        var sensor = entries[idx][0];
        var desired = entries[idx][1];
        idx++;
        hw.setText(statusDiv, 'Applying ' + sensor + ' ' + desired + '...');
        var cmd = 'espnowremote ' + mac + ' ' + u + ' ' + p + ' espnowsensorstream ' + sensor + ' ' + desired;
        hw.postFormText('/api/cli', { cmd: cmd })
        .then(function(){
          window.sensorActiveState[mac][sensor] = desired;
          delete pendingMap[sensor];
          window.updateSensorPill(mac, sensor);
          results.push(sensor + ' ' + desired + ' ✓');
          processNext();
        })
        .catch(function(e){
          results.push(sensor + ' error: ' + e.message);
          delete pendingMap[sensor];
          window.updateSensorPill(mac, sensor);
          processNext();
        });
      }
      processNext();
    };
    console.log('[ESP-NOW] Chunk 3: helpers ready');
  } catch(e) { console.error('[ESP-NOW] Chunk 3 error:', e); }
})();
