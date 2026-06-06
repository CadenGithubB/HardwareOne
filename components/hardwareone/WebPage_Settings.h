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
// Baseline snapshot: track what was loaded so saves only send changed values
window._settingsBaseline = {};
window._debugBaseline = {};
window._snapInput = function(el) {
  if (!el || el.type === 'password') return undefined;
  if (el.type === 'checkbox') return el.checked ? 1 : 0;
  if (el.type === 'number') {
    var n = el.step && el.step.indexOf('.') !== -1 ? parseFloat(el.value) : parseInt(el.value, 10);
    return isNaN(n) ? el.value : n;
  }
  return el.value;
};
window._snapshotContainer = function(root) {
  var scope = (root && root.querySelectorAll) ? root : document;
  scope.querySelectorAll('input,select,textarea').forEach(function(el) {
    if (el.id && el.type !== 'password') {
      window._settingsBaseline[el.id] = window._snapInput(el);
    }
  });
};
window._isChanged = function(id, val) {
  return !(id in window._settingsBaseline) || window._settingsBaseline[id] !== val;
};
// Target awareness — set by showLocalSettings / showBondedSettings. Determines
// whether postSettingsCli and sendSequential route to the local endpoints
// (/api/cli, /api/cli/batch) or to the bond proxy endpoints (/api/bond/exec,
// /api/bond/cli/batch). Defaults to 'local' so pages that don't have a
// bonded view (everything except the Settings page) behave as before.
window._settingsTarget = 'local';

// Single CLI command. Target-aware: in local mode posts to /api/cli and
// returns the plain-text response body. In bonded mode posts to
// /api/bond/exec (which routes through the bond session to the worker) and
// unwraps the JSON envelope to return the same plain-text shape — callers
// don't need to know which path was taken.
window.postSettingsCli = function(cmd) {
  if (window._settingsTarget === 'bonded') {
    return hw.postForm('/api/bond/exec', { cmd: cmd })
    .then(function(r) { return r.json(); })
    .then(function(j) {
      // /api/bond/exec returns {success, result}; collapse to a string so
      // callers see the same plain-text contract as /api/cli.
      if (!j || !j.success) {
        var msg = (j && (j.result || j.error)) || 'bond command failed';
        throw new Error(msg);
      }
      return j.result || '';
    });
  }
  return hw.postFormText('/api/cli', { cmd: cmd });
};

// LED live control — fire-and-forget commands (no settings save flow)
window.ledLiveSetBrightness = function() {
  var val = parseInt(document.getElementById('led-live-brightness').value);
  if (isNaN(val)) return;
  if (val < 0) val = 0;
  if (val > 100) val = 100;
  postSettingsCli('ledbrightness ' + val)
    .catch(function(e) { alert('LED brightness failed: ' + e.message); });
};
window.ledLiveApplyColor = function() {
  var color = document.getElementById('led-live-color').value;
  postSettingsCli('ledcolor ' + color)
    .catch(function(e) { alert('LED color failed: ' + e.message); });
};
window.ledLiveEffectChanged = function() {
  var needsTwo = document.getElementById('led-live-effect').value === 'fade';
  var w = document.getElementById('led-live-color2-wrap');
  if (w) w.style.display = needsTwo ? '' : 'none';
};
window.ledLiveRunEffect = function() {
  var effect   = document.getElementById('led-live-effect').value;
  var color1   = document.getElementById('led-live-eff-color1').value;
  var color2   = document.getElementById('led-live-eff-color2').value;
  var duration = parseInt(document.getElementById('led-live-duration').value) || 3000;
  var cmd = effect === 'fade'
    ? ('ledeffect fade ' + color1 + ' ' + color2 + ' ' + duration)
    : ('ledeffect ' + effect + ' ' + color1 + ' ' + duration);
  postSettingsCli(cmd)
    .catch(function(e) { alert('LED effect failed: ' + e.message); });
};
window.ledLiveClear = function() {
  postSettingsCli('ledclear')
    .catch(function(e) { alert('LED clear failed: ' + e.message); });
};

window.sendSequential = function(cmds, onDone, onFail) {
  var all = ['beginwrite'].concat(cmds).concat(['savesettings']);
  // Target-aware dispatch — bonded view sends the batch through the bond
  // session via /api/bond/cli/batch (same request shape, same response shape).
  var url = (window._settingsTarget === 'bonded') ? '/api/bond/cli/batch' : '/api/cli/batch';
  console.log('[sendSequential] target=' + window._settingsTarget + ' url=' + url + ' cmds=' + all.length);
  hw.postJSON(url, {commands: all})
  .then(function(j) {
    if (!(j && j.ok)) {
      if (onFail) onFail(new Error(j && j.error ? j.error : 'batch failed'));
      return;
    }

    var results = (j && Array.isArray(j.results)) ? j.results : [];
    var firstError = null;
    for (var i = 0; i < results.length; i++) {
      var out = String(results[i] || '');
      var low = out.toLowerCase();
      if (low.indexOf('unknown command') >= 0 ||
          low.indexOf('error:') >= 0 ||
          low.indexOf('failed') >= 0 ||
          low.indexOf('not found') >= 0) {
        firstError = { index: i, output: out };
        break;
      }
    }

    if (firstError) {
      var cmd = (all[firstError.index] || '').trim();
      var msg = 'Command failed: ' + cmd;
      if (firstError.output) msg += ' -> ' + firstError.output;
      if (onFail) onFail(new Error(msg));
      return;
    }

    if (onDone) onDone();
  })
  .catch(function(err) { if (onFail) onFail(err); });
};

// ============================================================================
// window.SchemaPanel — reusable schema-driven panel renderer.
//
// Each settings panel that was previously hand-rolled with bespoke HTML +
// inline /api/cli handlers calls SchemaPanel.render({...}). The helper:
//   1. Fetches /api/settings/schema and /api/settings (cached per page load).
//   2. Looks up the named module and filters its entries by an optional keys
//      whitelist (otherwise renders all entries in the module).
//   3. Renders an input per entry, type-driven (bool→checkbox, int/float→
//      number, string→text, secret string→password, anything with `options`→
//      <select>).
//   4. Wires a single Save button to sendSequential, which produces a
//      [beginwrite, ...cmds, savesettings] batch through /api/cli/batch.
//
// Logs every step with the caller's logPrefix so DevTools shows exactly what
// happened on flash without needing to add per-panel prints.
//
// Opts: {
//   containerId:  string  - DOM id to render into (required)
//   moduleName:   string  - schema module name (required, e.g. 'debug')
//   sectionPath:  string  - dot-path into /api/settings for value lookup
//                           (optional; defaults to the module's jsonSection)
//   keys:         array   - optional whitelist of entry.key values
//   excludeKeys:  array   - optional blacklist (applied after keys filter)
//   saveLabel:    string  - text for the Save button (default 'Save')
//   logPrefix:    string  - label used in console.log lines
//   target:       string  - 'local' (default) or 'bonded'. Switches the
//                           schema + values fetch to /api/bond/settings/schema
//                           + /api/bond/settings so the panel renders the
//                           worker's actual compiled-in schema instead of the
//                           master's. Save still flows through sendSequential,
//                           which is target-aware via window._settingsTarget.
// }
window.SchemaPanel = (function(){
  // Cache the schema + settings fetch across panels on the same page so we
  // don't refetch for every panel. Keyed by target ('local' vs 'bonded') so
  // both panels can coexist without clobbering each other's cache.
  var _cache = { local: null, bonded: null };
  function loadOnce(target){
    var key = target || 'local';
    if (_cache[key]) return _cache[key];
    var schemaUrl   = (key === 'bonded') ? '/api/bond/settings/schema' : '/api/settings/schema';
    var settingsUrl = (key === 'bonded') ? '/api/bond/settings'        : '/api/settings';
    _cache[key] = Promise.all([
      hw.fetchJSON(schemaUrl),
      hw.fetchJSON(settingsUrl)
    ]);
    return _cache[key];
  }
  // Force a fresh load on the next render (used after a save so the form
  // reflects whatever the worker actually accepted). Optional target arg —
  // omit to drop both caches.
  window.SchemaPanelInvalidate = function(target){
    if (!target) { _cache = { local: null, bonded: null }; return; }
    _cache[target] = null;
  };

  function log(pfx, msg){ try { console.log('[SchemaPanel:' + pfx + '] ' + msg); } catch(_) {} }
  function warn(pfx, msg){ try { console.warn('[SchemaPanel:' + pfx + '] ' + msg); } catch(_) {} }
  function errp(pfx, msg){ try { console.error('[SchemaPanel:' + pfx + '] ' + msg); } catch(_) {} }

  function getValueByPath(obj, path){
    if (!obj) return undefined;
    if (!path) return obj;
    var parts = path.split('.');
    var v = obj;
    for (var i = 0; i < parts.length && v != null; i++) v = v[parts[i]];
    return v;
  }

  function renderInput(entry, val, idPrefix){
    var id = idPrefix + '-' + entry.key.replace(/\./g, '-');
    var cmdAttr = entry.cmdKey ? ' data-cmd="' + entry.cmdKey + '"' : '';
    // Read-only display — system-managed values the user reads but never edits
    // (crashCount, lastResetReason, etc.). Render as plain text with the same
    // label styling as other fields so layout stays consistent. No id is needed
    // since the save handler only reads inputs with data-cmd attributes; this
    // span has neither, so it's automatically skipped on Save.
    if (entry.readOnly) {
      var displayVal = (val === undefined || val === null) ? '—' : val;
      return '<label style="display:flex;flex-direction:column;gap:0.25rem;font-size:0.9em;color:var(--panel-fg)">' + entry.label +
             '<span style="padding:0.5rem;border:1px solid var(--border);border-radius:4px;background:rgba(255,255,255,0.04);color:var(--muted);min-width:140px;display:inline-block">' +
             displayVal + '</span></label>';
    }
    // Enum picker — `options` is a CSV of "value|label" pairs (label-only OK).
    if (entry.options) {
      var html = '<label style="display:flex;flex-direction:column;gap:0.25rem;font-size:0.9em;color:var(--panel-fg)">' + entry.label + '<select id="' + id + '"' + cmdAttr + ' style="padding:0.5rem;border:1px solid var(--border);border-radius:4px;min-width:200px">';
      var current = (val !== undefined && val !== null) ? String(val) : '';
      entry.options.split(',').forEach(function(tok){
        var bar = tok.indexOf('|');
        var ov = bar >= 0 ? tok.substring(0, bar) : tok;
        var ol = bar >= 0 ? tok.substring(bar + 1) : tok;
        html += '<option value="' + ov + '"' + (ov === current ? ' selected' : '') + '>' + ol + '</option>';
      });
      return html + '</select></label>';
    }
    if (entry.type === 'bool') {
      // Global CSS in WebServer_Utils.h declares `input { width:100%; padding:.5rem;
      // border:1px solid #ddd; margin-bottom:.5rem; ... }` which applies to checkboxes
      // and makes each one stretch to fill its grid cell — that's what was pushing the
      // label text aside and producing the overlapping/wrapped mess. The inline overrides
      // below restore the checkbox's natural ~13px size and remove the extraneous padding
      // and margin so it lays out cleanly inside the flex label. Text wrapped in a
      // nowrap span so labels like "Auto-start after boot" stay on one line.
      return '<label style="display:flex;align-items:center;gap:0.5rem;font-size:0.9em;color:var(--panel-fg)">' +
             '<input type="checkbox" id="' + id + '"' + (val ? ' checked' : '') + cmdAttr +
             ' style="width:auto;flex:0 0 auto;margin:0;padding:0;border:none;background:transparent">' +
             '<span style="white-space:nowrap">' + entry.label + '</span></label>';
    }
    if (entry.type === 'string' && entry.secret) {
      var placeholder = (val !== undefined && val !== '') ? '(set - leave blank to keep)' : '(not set)';
      return '<label style="display:flex;flex-direction:column;gap:0.25rem;font-size:0.9em;color:var(--panel-fg)">' + entry.label + '<input type="password" id="' + id + '" placeholder="' + placeholder + '"' + cmdAttr + ' style="padding:0.5rem;border:1px solid var(--border);border-radius:4px;width:240px"></label>';
    }
    if (entry.type === 'int' || entry.type === 'float' ||
        entry.type === 'u8'  || entry.type === 'u16'   || entry.type === 'u32') {
      var step = entry.type === 'float' ? '0.01' : '1';
      var minAttr = entry.min !== undefined && entry.min !== null ? ' min="' + entry.min + '"' : '';
      var maxAttr = entry.max !== undefined && entry.max !== null ? ' max="' + entry.max + '"' : '';
      var current = (val !== undefined && val !== null) ? val : (entry['default'] || 0);
      return '<label style="display:flex;flex-direction:column;gap:0.25rem;font-size:0.9em;color:var(--panel-fg)">' + entry.label + '<input type="number" id="' + id + '" value="' + current + '"' + minAttr + maxAttr + ' step="' + step + '"' + cmdAttr + ' style="padding:0.5rem;border:1px solid var(--border);border-radius:4px;width:140px"></label>';
    }
    // string fallback
    return '<label style="display:flex;flex-direction:column;gap:0.25rem;font-size:0.9em;color:var(--panel-fg)">' + entry.label + '<input type="text" id="' + id + '" value="' + (val == null ? '' : val) + '"' + cmdAttr + ' style="padding:0.5rem;border:1px solid var(--border);border-radius:4px;width:240px"></label>';
  }

  function build(cont, opts, schema, settings){
    var pfx = opts.logPrefix || opts.containerId;
    var mod = (schema.modules || []).find(function(m){ return m.name === opts.moduleName; });
    if (!mod) {
      errp(pfx, 'Module "' + opts.moduleName + '" not found in schema');
      cont.innerHTML = '<span style="color:var(--danger,#e74c3c)">Schema module &quot;' + opts.moduleName + '&quot; not found</span>';
      return;
    }
    var entries = mod.entries || [];
    if (opts.keys && opts.keys.length) {
      entries = entries.filter(function(e){ return opts.keys.indexOf(e.key) >= 0; });
    }
    if (opts.excludeKeys && opts.excludeKeys.length) {
      entries = entries.filter(function(e){ return opts.excludeKeys.indexOf(e.key) < 0; });
    }
    if (entries.length === 0) {
      warn(pfx, 'No entries to render after filter (module=' + opts.moduleName + ' keys=' + (opts.keys||['*']).join(',') + ')');
      cont.innerHTML = '<span style="opacity:0.7">No matching settings</span>';
      return;
    }
    log(pfx, 'Rendering ' + entries.length + ' entries: ' + entries.map(function(e){return e.key;}).join(','));

    var sectionPath = opts.sectionPath || mod.section || mod.name;
    var section = getValueByPath(settings, sectionPath) || {};
    log(pfx, 'Reading values from settings path "' + sectionPath + '" (got ' + Object.keys(section).length + ' keys)');

    var idPrefix = 'sp-' + opts.containerId;
    // Group bool toggles together, scalars together — keeps the layout tidy.
    var bools = entries.filter(function(e){ return e.type === 'bool' && !e.options; });
    var others = entries.filter(function(e){ return !(e.type === 'bool' && !e.options); });

    var html = '';
    if (others.length > 0) {
      html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:0.75rem;margin-bottom:0.75rem">';
      others.forEach(function(e){ html += renderInput(e, section[e.key], idPrefix); });
      html += '</div>';
    }
    if (bools.length > 0) {
      // Grid with auto-fill minmax so each toggle gets its own slot at consistent
      // width — labels of varying length no longer wrap raggedly into the next
      // toggle, and the layout stays readable even with the ~80 debug flags the
      // worker exposes. 200px min keeps the longest realistic label on one line.
      html += '<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:0.4rem 1rem;margin-bottom:0.75rem">';
      bools.forEach(function(e){ html += renderInput(e, section[e.key], idPrefix); });
      html += '</div>';
    }
    var btnId = idPrefix + '-save';
    var msgId = idPrefix + '-msg';
    html += '<button class="btn" id="' + btnId + '">' + (opts.saveLabel || 'Save') + '</button>';
    html += ' <span id="' + msgId + '" style="font-size:0.85em;color:var(--muted);margin-left:0.5rem"></span>';
    cont.innerHTML = html;
    window._snapshotContainer(cont);

    document.getElementById(btnId).addEventListener('click', function(){
      var msg = document.getElementById(msgId);
      var cmds = [];
      var skipped = [];
      cont.querySelectorAll('input,select').forEach(function(el){
        if (!el.id) return;
        var verb = el.getAttribute('data-cmd');
        if (!verb) return;  // no CLI verb mapped → skip
        var val;
        if (el.type === 'checkbox') val = el.checked ? 1 : 0;
        else if (el.tagName === 'SELECT') val = el.value;
        else if (el.type === 'number') val = (el.step && el.step.indexOf('.') !== -1) ? parseFloat(el.value) : parseInt(el.value, 10);
        else if (el.type === 'password') { if (!el.value || el.value.trim() === '') return; val = el.value; }
        else val = el.value;
        if (el.type !== 'password' && !window._isChanged(el.id, val)) { skipped.push(el.id); return; }
        cmds.push(verb + ' ' + val);
      });
      log(pfx, 'Save click — ' + cmds.length + ' change(s), ' + skipped.length + ' unchanged');
      if (cmds.length === 0) { if (msg) msg.textContent = 'No changes to save.'; return; }
      log(pfx, 'Dispatching batch: ' + cmds.join(' ; '));
      if (msg) msg.textContent = 'Saving ' + cmds.length + ' change(s)…';
      sendSequential(cmds,
        function(){
          log(pfx, 'Save OK');
          if (msg) msg.textContent = 'Saved ' + cmds.length + ' change(s).';
          window._snapshotContainer(cont);
          // Bonded save: worker just wrote its settings.json, so the master's
          // cache is stale. Force a fresh /api/bond/settings/sync + invalidate
          // and re-render so this panel reflects ground truth. Whoever wired
          // the bonded view (window._bondSyncHook) gets a chance to update its
          // page-level state (formLoadedHash, dirty banner) at the same time.
          if (opts.target === 'bonded') {
            log(pfx, 'Bonded save → /api/bond/settings/sync + cache invalidate');
            hw.postJSON('/api/bond/settings/sync', {})
              .then(function(d){
                window.SchemaPanelInvalidate('bonded');
                if (typeof window._bondSyncHook === 'function') window._bondSyncHook(d);
                window.SchemaPanel.render(opts);
              })
              .catch(function(e){ warn(pfx, 'Post-save bonded sync failed: ' + e.message); });
          }
          setTimeout(function(){ if (msg) msg.textContent = ''; }, 4000);
        },
        function(e){
          errp(pfx, 'Save failed: ' + (e ? e.message : 'unknown'));
          if (msg) msg.textContent = 'Save failed: ' + (e ? e.message : 'unknown');
        }
      );
    });
  }

  return {
    render: function(opts){
      var pfx = opts.logPrefix || opts.containerId;
      var cont = document.getElementById(opts.containerId);
      if (!cont) { errp(pfx, 'Container #' + opts.containerId + ' not found in DOM'); return Promise.reject(new Error('container not found')); }
      var target = opts.target || 'local';
      log(pfx, 'render() called for module=' + opts.moduleName + ' target=' + target);
      return loadOnce(target).then(function(results){
        // /api/bond/settings returns {settings:{...}}, /api/settings returns
        // {settings:{...},...} — same shape for our purposes.
        build(cont, opts, results[0] || {}, (results[1] && results[1].settings) || {});
      }).catch(function(e){
        errp(pfx, 'Render failed: ' + e.message);
        cont.innerHTML = '<span style="color:var(--danger,#e74c3c)">Failed to load schema: ' + e.message + '</span>';
      });
    }
  };
})();
</script>
)EARLYJS", HTTPD_RESP_USE_STRLEN);

  // Part 1: Header
  httpd_resp_send_chunk(req, R"SETPART1(
<h2>System Settings</h2>
<p>Configure your HardwareOne device settings</p>
)SETPART1", HTTPD_RESP_USE_STRLEN);

  // Target toggle — same pattern as the CLI and Files pages. Hidden by default;
  // revealed at the bottom of this function (after the bonded panel HTML is
  // emitted) when /api/bond/status reports this device is the bonded master.
  // Opens settings-local-container, which wraps ALL the existing settings
  // panels emitted below. Closing div + bonded container are emitted in the
  // tail chunk at the end of streamSettingsInner.
  httpd_resp_send_chunk(req, R"SETTOGGLE(
<div id='settings-source-toggle' style='display:none;margin:0.75rem 0 0.25rem 0;align-items:center;gap:8px'>
  <span style='font-size:0.85rem;color:var(--muted)'>Target:</span>
  <button id='settings-btn-local' class='btn' onclick='showLocalSettings()'>This Device</button>
  <button id='settings-btn-bonded' class='btn' onclick='showBondedSettings()'>Bonded Device</button>
</div>
<div id='settings-local-container'>
)SETTOGGLE", HTTPD_RESP_USE_STRLEN);

  // Part 2: System Time, Output Channels, CLI History sections
  httpd_resp_send_chunk(req, R"SETPART2(
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>System Time</div><div style='color:var(--panel-fg);font-size:0.9rem'>Configure timezone offset and NTP server for accurate time synchronization.</div></div>
    <button class='btn' id='btn-time-toggle' onclick="togglePane('time-pane','btn-time-toggle')">Expand</button>
  </div>
  <div id='time-pane' style='display:none;margin-top:0.75rem'>
    <!-- Schema-driven: the IIFE below fills this container at page load from
         the wifi module's tzOffsetMinutes + ntpServer schema entries. All
         save dispatch flows through sendSequential (beginwrite/savesettings
         batched) instead of the old per-field Update buttons. -->
    <div id='system-time-container' style='color:var(--panel-fg)'>Loading…</div>
  </div>
</div>
<script>
// System Time — schema-driven via the shared SchemaPanel helper.
window.SchemaPanel.render({
  containerId: 'system-time-container',
  moduleName: 'wifi',
  sectionPath: 'network.wifi',
  keys: ['tzOffsetMinutes', 'ntpServer'],
  saveLabel: 'Save System Time',
  logPrefix: 'System Time'
});
</script>
<!-- Output Channels settings now in schema-driven Sensors panel -->
)SETPART2", HTTPD_RESP_USE_STRLEN);

#if ENABLE_WIFI || ENABLE_MQTT || ENABLE_HTTP_SERVER || ENABLE_BLUETOOTH || ENABLE_ESPNOW
  // Part 3.5: Network Services section - WiFi (static) + dynamic schema-driven cards
  httpd_resp_send_chunk(req, R"SETPART3_5(
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Network Services</div><div style='color:var(--panel-fg);font-size:0.9rem'>Configure network service integrations.</div></div>
    <button class='btn' id='btn-network-toggle' onclick="togglePane('network-pane','btn-network-toggle')">Expand</button>
  </div>
  <div id='network-pane' style='display:none;margin-top:1rem;color:var(--panel-fg)'>
    <div class='settings-panel' style='margin:0 0 0.75rem 0'>
      <div style='display:flex;align-items:center;justify-content:space-between'>
        <div><div style='font-size:1.05rem;font-weight:bold;color:var(--panel-fg)'>WiFi <span id='wifi-status-badge'></span></div><div style='color:var(--panel-fg);font-size:0.85rem;margin-top:0.25rem'>Current network, scan, and saved-credential management.</div></div>
        <button class='btn' id='btn-wifi-toggle' onclick="togglePane('wifi-pane','btn-wifi-toggle')">Expand</button>
      </div>
      <div id='wifi-pane' style='display:none;margin-top:0.75rem'>
        <div style='margin-bottom:1rem'>
          <span style='color:var(--panel-fg)'>SSID: <span style='font-weight:bold;color:var(--accent)' id='wifi-ssid'>-</span></span>
        </div>
        <div style='display:flex;align-items:center;gap:1rem;margin-bottom:1rem;flex-wrap:wrap'>
          <span style='color:var(--panel-fg)' title='Automatically reconnect to saved WiFi networks after power loss or disconnection'>Auto-Reconnect: <span style='font-weight:bold;color:var(--accent)' id='wifi-value'>-</span></span>
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
          <button class='btn' onclick="(function(){ var ssid=(document.getElementById('sel-ssid')||{}).textContent||''; var pass=(document.getElementById('sel-pass')||{}).value||''; if(!ssid){ alert('No SSID selected'); return; } var cmd1='wifiadd '+ssid+' '+pass+' 1 0'; postSettingsCli(cmd1).then(function(t1){ return hwConfirm('Credentials saved for \"'+ssid+'\". Attempt to connect now? You may temporarily lose access while switching.').then(function(ok){ if(!ok){ alert('Saved. You can connect later from this page.'); if(typeof refreshSettings==='function') refreshSettings(); return null; } return postSettingsCli('wificonnect'); }); }).then(function(t){ return t || ''; }).then(function(t2){ if(t2){ alert(t2||'Connect attempted'); } if(typeof refreshSettings==='function') refreshSettings(); }).catch(function(e){ alert('Action failed: '+e.message); }); })();">Connect</button>
        </div>
        <div id='wifi-manual-panel' style='display:none;margin-top:0.75rem'>
          <div style='margin-bottom:0.5rem'>Enter hidden network credentials</div>
          <input type='text' id='manual-ssid' placeholder='Hidden SSID' class='form-input input-medium' style='margin-right:6px'>
          <input type='password' id='manual-pass' placeholder='Password (leave blank if open)' class='form-input input-medium' style='margin-right:6px'>
          <button class='btn' onclick="(function(){ var ssid=(document.getElementById('manual-ssid')||{}).value||''; var pass=(document.getElementById('manual-pass')||{}).value||''; if(!ssid){ alert('Enter SSID'); return; } var cmd1='wifiadd '+ssid+' '+pass+' 1 1'; postSettingsCli(cmd1).then(function(t1){ return hwConfirm('Credentials saved for hidden network \"'+ssid+'\". Attempt to connect now? You may temporarily lose access while switching.').then(function(ok){ if(!ok){ alert('Saved. You can connect later from this page.'); if(typeof refreshSettings==='function') refreshSettings(); return null; } return postSettingsCli('wificonnect'); }); }).then(function(t){ return t || ''; }).then(function(t2){ if(t2){ alert(t2||'Connect attempted'); } if(typeof refreshSettings==='function') refreshSettings(); }).catch(function(e){ alert('Action failed: '+e.message); }); })();">Connect</button>
        </div>
      </div>
    </div>
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
#if ENABLE_ESPNOW && (ENABLE_MQTT || ENABLE_HTTP_SERVER || ENABLE_BLUETOOTH)
  httpd_resp_send_chunk(req, ",", HTTPD_RESP_USE_STRLEN);
#endif
#if ENABLE_ESPNOW
  httpd_resp_send_chunk(req, "'espnow'", HTTPD_RESP_USE_STRLEN);
#endif
  httpd_resp_send_chunk(req, R"SETPART3_5B(];
  var networkSections = {'mqtt':'mqtt','http':'http','bluetooth':'bluetooth','espnow':'espnow'};
  var networkLabels = {mqtt:'MQTT Broker',http:'HTTP Server',bluetooth:'Bluetooth',espnow:'ESP-NOW'};
  
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
    var cmdAttr = e.cmdKey ? ' data-cmd="' + e.cmdKey + '"' : '';
    // Enum-style picker — schema entry's `options` field is a CSV of
    // `value|label` pairs (label-only tokens accepted; value defaults to
    // label). Renders a <select> regardless of underlying type (int / string).
    if (e.options) {
      var html = '<label style="' + grayStyle + '">' + e.label + '<br><select id="' + id + '"' + disAttr + cmdAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;min-width:200px">';
      var current = (val !== undefined && val !== null) ? String(val) : '';
      e.options.split(',').forEach(function(tok){
        var bar = tok.indexOf('|');
        var ov = bar >= 0 ? tok.substring(0, bar) : tok;
        var ol = bar >= 0 ? tok.substring(bar + 1) : tok;
        html += '<option value="' + ov + '"' + (ov === current ? ' selected' : '') + '>' + ol + '</option>';
      });
      html += '</select></label>';
      return html;
    }
    if (e.type === 'bool') {
      return '<label style="' + grayStyle + '"><input type="checkbox" id="' + id + '"' + (val ? ' checked' : '') + disAttr + cmdAttr + ' style="margin-right:0.5rem">' + e.label + '</label>';
    } else if (e.type === 'string' && e.secret) {
      var placeholder = val !== undefined && val !== '' ? '(set - leave blank to keep)' : '(not set)';
      return '<label style="' + grayStyle + '">' + e.label + '<br><input type="password" id="' + id + '" placeholder="' + placeholder + '"' + disAttr + cmdAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:200px"></label>';
    } else if (e.type === 'int' || e.type === 'float') {
      var step = e.type === 'float' ? '0.01' : '1';
      var minAttr = e.min !== undefined ? ' min="' + e.min + '"' : '';
      var maxAttr = e.max !== undefined ? ' max="' + e.max + '"' : '';
      return '<label style="' + grayStyle + '">' + e.label + '<br><input type="number" id="' + id + '" value="' + (val !== undefined ? val : e.default) + '"' + minAttr + maxAttr + ' step="' + step + '"' + disAttr + cmdAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:140px"></label>';
    } else {
      return '<label style="' + grayStyle + '">' + e.label + '<br><input type="text" id="' + id + '" value="' + (val || '') + '"' + disAttr + cmdAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:200px"></label>';
    }
  }
  
  function readNetSection(settings, mod) {
    if (!settings || !mod) return {};
    var path = mod.section || mod.name || '';
    if (!path) return settings;
    var v = settings;
    var parts = path.split('.');
    for (var i = 0; i < parts.length && v != null; i++) v = v[parts[i]];
    if (v == null) v = settings[mod.name];   // legacy fallback
    return v || {};
  }

  function renderNetworkModule(mod, settings) {
    var section = readNetSection(settings, mod);
    var entries = mod.entries || [];
    // Only show an Enabled/Disabled badge if the schema reports a real
    // runtime status (mod.connected is a boolean). Modules without an
    // isConnected callback omit the badge entirely.
    var hasConnStatus = (typeof mod.connected === 'boolean');
    var isDisconnected = mod.connected === false;
    var statusBadge = '';
    if (hasConnStatus && isDisconnected) {
      statusBadge = '<span style="background:rgba(255,152,0,0.15);color:#ff9800;border:1px solid rgba(255,152,0,0.3);padding:0.15rem 0.5rem;border-radius:3px;font-size:0.7rem;margin-left:0.5rem;font-weight:500">Disabled</span>';
    } else if (hasConnStatus) {
      statusBadge = '<span style="background:rgba(102,126,234,0.15);color:var(--accent);border:1px solid rgba(102,126,234,0.3);padding:0.15rem 0.5rem;border-radius:3px;font-size:0.7rem;margin-left:0.5rem;font-weight:500">Enabled</span>';
    }
    
    // Build the card title. The http module gets a special "HTTP(S) Server"
    // label when httpsEnabled is true in the saved settings — purely cosmetic
    // hint that the build also serves over TLS.
    var titleLabel = networkLabels[mod.name] || mod.description || mod.name;
    if (mod.name === 'http' && section && section.httpsEnabled) {
      titleLabel = 'HTTP(S) Server';
    }
    var html = '<div style="background:var(--panel-bg);border-radius:8px;padding:1rem 1.5rem;margin:0.5rem 0;box-shadow:0 1px 3px rgba(0,0,0,0.1);border:1px solid var(--border)">';
    html += '<div style="display:flex;align-items:center;justify-content:space-between">';
    html += '<div>';
    html += '<span style="font-size:1.1rem;font-weight:bold;color:var(--panel-fg)">' + titleLabel + '</span>' + statusBadge;
    // Always show the description as a subtitle when present (and distinct
    // from the title). The earlier !networkLabels[mod.name] guard hid it
    // for every labelled module, leaving cards without context.
    if (mod.description && mod.description !== (networkLabels[mod.name] || mod.name)) {
      html += '<div style="color:var(--panel-fg);font-size:0.85rem;margin-top:0.25rem">' + mod.description + '</div>';
    }
    html += '</div>';
    html += '<button class="btn" id="btn-' + mod.name + '-net-toggle" onclick="togglePane(\'' + mod.name + '-net-pane\',\'btn-' + mod.name + '-net-toggle\')">Expand</button>';
    html += '</div>';
    html += '<div id="' + mod.name + '-net-pane" style="display:none;margin-top:0.75rem">';
    
    if (isDisconnected) {
      html += '<div style="background:rgba(255,152,0,0.08);padding:0.75rem;margin-bottom:1rem;color:var(--panel-fg);opacity:0.8;font-size:0.85rem">';
      html += 'Service is currently disabled. Settings can still be changed; toggle Auto-start at boot or start it manually to enable.';
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
    hw.fetchJSON('/api/settings/schema'),
    hw.fetchJSON('/api/settings')
  ]).then(function(results) {
    var schema = results[0];
    var settingsResp = results[1];
    var settings = settingsResp.settings || {};
    var container = document.getElementById('network-dynamic-container');
    if (!container) return;

    // Update the static WiFi card's status badge from the schema's wifi module
    // connected flag (driven by isWifiConnected() server-side).
    var wifiBadge = document.getElementById('wifi-status-badge');
    if (wifiBadge) {
      var wifiMod = (schema.modules || []).find(function(m) { return m.name === 'wifi'; });
      if (wifiMod && typeof wifiMod.connected === 'boolean') {
        if (wifiMod.connected) {
          wifiBadge.outerHTML = '<span id="wifi-status-badge" style="background:rgba(102,126,234,0.15);color:var(--accent);border:1px solid rgba(102,126,234,0.3);padding:0.15rem 0.5rem;border-radius:3px;font-size:0.7rem;margin-left:0.5rem;font-weight:500">Enabled</span>';
        } else {
          wifiBadge.outerHTML = '<span id="wifi-status-badge" style="background:rgba(255,152,0,0.15);color:#ff9800;border:1px solid rgba(255,152,0,0.3);padding:0.15rem 0.5rem;border-radius:3px;font-size:0.7rem;margin-left:0.5rem;font-weight:500">Disabled</span>';
        }
      }
    }

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
    window._snapshotContainer(container);
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
      if (el.type !== 'password' && !window._isChanged(el.id, val)) return;
      updates[key] = val;
    });
    
    var cmds = [];
    for (var k in updates) {
      var el = container.querySelector('#net-' + k.replace(/\./g, '-'));
      var cmd = el && el.getAttribute('data-cmd');
      cmds.push((cmd || k) + ' ' + updates[k]);
    }

    if (cmds.length === 0) { alert('No changes to save.'); return; }

    sendSequential(cmds,
      function() { window._snapshotContainer(container); alert('Settings saved! Some changes may require a restart.'); },
      function(e) { alert('Save failed: ' + (e ? e.message : 'unknown')); }
    );
  };
})();
</script>
)SETPART3_5B", HTTPD_RESP_USE_STRLEN);
#endif // ENABLE_MQTT || ENABLE_HTTP_SERVER || ENABLE_BLUETOOTH

  // Hardware umbrella opens here. Sensors / I2C / LED render as nested cards
  // inside hardware-pane; the umbrella closes after the LED container below.
  httpd_resp_send_chunk(req, R"HWOPEN(
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Hardware</div><div style='color:var(--panel-fg);font-size:0.9rem'>Sensors, I2C bus, and onboard LED.</div></div>
    <button class='btn' id='btn-hardware-toggle' onclick="togglePane('hardware-pane','btn-hardware-toggle')">Expand</button>
  </div>
  <div id='hardware-pane' style='display:none;margin-top:0.75rem'>
)HWOPEN", HTTPD_RESP_USE_STRLEN);

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
  var sensorModules = ['camera','microphone','thermal','tof','imu','gps','fmradio','servo','apds','rtc','presence','oled','gamepad','input','anoencoder'];
  var hardwareTopModules = ['led'];   // sibling of Sensors/I2C inside Hardware
  var i2cModules = ['i2c'];
  var outputModules = ['output'];
  var appsModules = ['automation','llm','espsr','edgeimpulse','maps'];   // top-level Apps umbrella
  var loggingModules = ['sensorlog','systemlog'];
  // mlSubsections used to nest espsr/edgeimpulse INSIDE Microphone/Camera. Now
  // these render as their own cards under Apps; leave empty for backwards
  // compatibility (renderModule still consults this map).
  var mlSubsections = {};
  // Map from a top-level settings.json key to the module name. Used by the
  // orphan-detection loop below: a key in /api/settings whose schema module
  // isn't compiled becomes a read-only "Inactive" card. Post-v0.93 the JSON
  // top-level keys are now umbrellas (hardware, network, logging, system,
  // apps), so orphan detection effectively no-ops until that loop is taught
  // to recurse into the nested layout. Identity mappings kept here for
  // backwards compatibility with any older flat settings.json that lingers.
  var sensorSections = {'camera':'camera','microphone':'microphone','edgeimpulse':'edgeimpulse','espsr':'espsr','thermal':'thermal','tof':'tof','imu':'imu','gps':'gps','fmradio':'fmradio','apds':'apds','rtc':'rtc','presence':'presence','sensorlog':'sensorlog','power':'power','debug':'debug','output':'output','oled':'oled','gamepad':'gamepad','input':'input','anoencoder':'anoencoder','led':'led','llm':'llm','maps':'maps'};
  var moduleLabels = {camera:'Camera (OV2640/OV3660)',microphone:'Microphone (PDM)',edgeimpulse:'Machine Learning',espsr:'Voice Recognition (ESP-SR)',thermal:'Thermal Camera (MLX90640)',tof:'Time-of-Flight (VL53L4CX)',imu:'IMU (BNO055)',gps:'GPS (PA1010D)',fmradio:'FM Radio (RDA5807)',servo:'Servo Driver (PCA9685)',gamepad:'Gamepad (Seesaw)',input:'Input Device',anoencoder:'ANO Encoder (Seesaw)',apds:'APDS (APDS9960)',rtc:'RTC Clock (DS3231)',presence:'IR Presence (STHS34PF80)',sensorlog:'Sensor Logging',systemlog:'System Logging',i2c:'I2C Bus Configuration',power:'Power Management',debug:'Debug Flags',output:'Output Channels',oled:'OLED Display (SSD1306)',led:'LED Startup & Brightness',llm:'On-Device LLM',maps:'Maps'};
  
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
    var cmdAttr = e.cmdKey ? ' data-cmd="' + e.cmdKey + '"' : '';
    if (e.type === 'bool') {
      return '<label style="' + grayStyle + '"><input type="checkbox" id="' + id + '"' + (val ? ' checked' : '') + disAttr + cmdAttr + ' style="margin-right:0.5rem">' + e.label + '</label>';
    } else if (e.type === 'string' && e.secret) {
      // Secret field: use password input, show placeholder if set, blank = unchanged
      var placeholder = val !== undefined && val !== '' ? '(set - leave blank to keep)' : '(not set)';
      return '<label style="' + grayStyle + '">' + e.label + '<br><input type="password" id="' + id + '" placeholder="' + placeholder + '"' + disAttr + cmdAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:200px"></label>';
    } else if (e.type === 'string' && e.options) {
      var opts = e.options.split(',').map(function(o) {
        return '<option value="' + o + '"' + (val === o ? ' selected' : '') + '>' + o.charAt(0).toUpperCase() + o.slice(1) + '</option>';
      }).join('');
      return '<label style="' + grayStyle + '">' + e.label + '<br><select id="' + id + '"' + disAttr + cmdAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:160px">' + opts + '</select></label>';
    } else if ((e.type === 'int' || e.type === 'float') && e.options) {
      // Int/float with named options - render as dropdown
      var opts = e.options.split(',').map(function(o) {
        var parts = o.split(':');
        var optVal = parts[0];
        var optLabel = parts.length > 1 ? parts[1] : optVal;
        return '<option value="' + optVal + '"' + (parseInt(val) === parseInt(optVal) ? ' selected' : '') + '>' + optLabel + '</option>';
      }).join('');
      return '<label style="' + grayStyle + '">' + e.label + '<br><select id="' + id + '"' + disAttr + cmdAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:200px">' + opts + '</select></label>';
    } else if (e.type === 'int' || e.type === 'float') {
      var step = e.type === 'float' ? '0.01' : '1';
      var minAttr = e.min !== undefined ? ' min="' + e.min + '"' : '';
      var maxAttr = e.max !== undefined ? ' max="' + e.max + '"' : '';
      return '<label style="' + grayStyle + '">' + e.label + '<br><input type="number" id="' + id + '" value="' + (val !== undefined ? val : e.default) + '"' + minAttr + maxAttr + ' step="' + step + '"' + disAttr + cmdAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:140px"></label>';
    } else {
      return '<label style="' + grayStyle + '">' + e.label + '<br><input type="text" id="' + id + '" value="' + (val || '') + '"' + disAttr + cmdAttr + ' style="padding:0.5rem;border:1px solid #ddd;border-radius:4px;width:200px"></label>';
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
  
  function renderLedLiveControls() {
    var colors = [
      'red','green','blue','yellow','cyan','magenta','white','black',
      'orange','darkorange','orangered','coral','tomato','peach',
      'darkred','crimson','firebrick','indianred','lightcoral','salmon',
      'pink','lightpink','hotpink','deeppink','palevioletred','mediumvioletred',
      'purple','darkviolet','blueviolet','mediumpurple','plum','orchid',
      'darkblue','navy','mediumblue','royalblue','steelblue','lightblue',
      'skyblue','lightskyblue','deepskyblue','dodgerblue','cornflowerblue','cadetblue',
      'darkgreen','forestgreen','seagreen','mediumseagreen','springgreen','limegreen',
      'lime','lightgreen','palegreen','aquamarine','mediumaquamarine',
      'gold','lightyellow','lemonchiffon','lightgoldenrodyellow','khaki','darkkhaki',
      'brown','saddlebrown','sienna','chocolate','peru','tan','burlywood','wheat',
      'gray','darkgray','lightgray','silver','dimgray','gainsboro'
    ];
    var colorOpts = colors.map(function(c) { return '<option value="' + c + '">' + c + '</option>'; }).join('');
    var color2Opts = colors.map(function(c) { return '<option value="' + c + '"' + (c === 'blue' ? ' selected' : '') + '>' + c + '</option>'; }).join('');
    var sel = 'padding:0.4rem 0.5rem;border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg);font-size:0.85rem';
    var num = sel + ';width:100px';
    var lbl = 'font-size:0.78rem;color:var(--panel-fg);opacity:0.75;display:block;margin-bottom:0.3rem';
    var row = 'display:flex;align-items:flex-end;gap:0.75rem;flex-wrap:wrap;margin-bottom:0.75rem';

    var h = '<div style="margin-top:1rem;padding-top:1rem;border-top:1px solid var(--border)">';
    h += '<div style="font-weight:bold;margin-bottom:0.75rem;color:var(--panel-fg)">Live Control</div>';

    // Brightness row
    h += '<div style="' + row + '">';
    h += '<div><label style="' + lbl + '">Brightness (0–100)</label>';
    h += '<input type="number" id="led-live-brightness" value="100" min="0" max="100" step="1" style="' + num + '"></div>';
    h += '<button class="btn" onclick="ledLiveSetBrightness()">Set Brightness</button>';
    h += '</div>';

    // Solid color + clear row
    h += '<div style="' + row + '">';
    h += '<div><label style="' + lbl + '">Solid Color</label>';
    h += '<select id="led-live-color" style="' + sel + '">' + colorOpts + '</select></div>';
    h += '<button class="btn" onclick="ledLiveApplyColor()">Apply Color</button>';
    h += '<button class="btn" onclick="ledLiveClear()">Clear LED</button>';
    h += '</div>';

    // Effect row
    h += '<div style="' + row + '">';
    h += '<div><label style="' + lbl + '">Effect</label>';
    h += '<select id="led-live-effect" style="' + sel + '" onchange="ledLiveEffectChanged()">';
    h += '<option value="blink">Blink</option><option value="pulse">Pulse</option>';
    h += '<option value="strobe">Strobe</option><option value="fade">Fade</option>';
    h += '</select></div>';
    h += '<div><label style="' + lbl + '">Color</label>';
    h += '<select id="led-live-eff-color1" style="' + sel + '">' + colorOpts + '</select></div>';
    h += '<div id="led-live-color2-wrap" style="display:none"><label style="' + lbl + '">Color 2</label>';
    h += '<select id="led-live-eff-color2" style="' + sel + '">' + color2Opts + '</select></div>';
    h += '<div><label style="' + lbl + '">Duration (ms)</label>';
    h += '<input type="number" id="led-live-duration" value="3000" min="100" max="60000" step="500" style="' + num + '"></div>';
    h += '<button class="btn" onclick="ledLiveRunEffect()">Run Effect</button>';
    h += '</div>';

    h += '</div>';
    return h;
  }

  // Walk a dotted module section path (e.g. "hardware.sensors.camera") into
  // the settings tree, falling back to the module name for legacy/orphan
  // sections that haven't been migrated.
  function readModuleSection(settings, mod) {
    if (!settings || !mod) return {};
    var path = mod.section || mod.name || '';
    if (!path) return settings;
    var v = settings;
    var parts = path.split('.');
    for (var i = 0; i < parts.length && v != null; i++) v = v[parts[i]];
    if (v == null) v = settings[mod.name];   // legacy fallback
    return v || {};
  }

  // Read a setting value out of the settings JSON, honoring the entry's
  // optional `group` field (which nests the value one level deeper in JSON).
  function readEntryValue(section, e) {
    var v = (section && e.group) ? section[e.group] : section;
    if (v == null) return undefined;
    var parts = (e.key || '').split('.');
    for (var i = 0; i < parts.length && v != null; i++) v = v[parts[i]];
    return v;
  }

  function renderModule(mod, settings, isOrphan, allModules, allSettings) {
    var section = readModuleSection(settings, mod);
    var entries = mod.entries || [];
    var uiEntries = entries.filter(function(e) { return e.key.indexOf('ui.') === 0; });
    var devEntries = entries.filter(function(e) { return e.key.indexOf('device.') === 0; });
    var otherEntries = entries.filter(function(e) { return e.key.indexOf('ui.') !== 0 && e.key.indexOf('device.') !== 0; });
    
    // Status badge — only render when the schema reports a real connection
    // status (mod.connected is a boolean). Modules without an isConnected
    // callback omit it from the schema, so they show no badge instead of a
    // misleading "Connected".
    var hasConnStatus = (typeof mod.connected === 'boolean');
    var isDisconnected = mod.connected === false;
    var statusBadge = '';
    if (isOrphan) {
      statusBadge = '<span style="background:#6b7280;color:#fff;padding:0.15rem 0.5rem;border-radius:3px;font-size:0.7rem;margin-left:0.5rem;font-weight:500">Inactive</span>';
    } else if (hasConnStatus && isDisconnected) {
      statusBadge = '<span style="background:rgba(255,152,0,0.15);color:#ff9800;border:1px solid rgba(255,152,0,0.3);padding:0.15rem 0.5rem;border-radius:3px;font-size:0.7rem;margin-left:0.5rem;font-weight:500">Disconnected</span>';
    } else if (hasConnStatus) {
      statusBadge = '<span style="background:rgba(102,126,234,0.15);color:var(--accent);border:1px solid rgba(102,126,234,0.3);padding:0.15rem 0.5rem;border-radius:3px;font-size:0.7rem;margin-left:0.5rem;font-weight:500">Connected</span>';
    }
    
    var html = '<div style="background:var(--panel-bg);border-radius:8px;padding:1rem 1.5rem;margin:0.5rem 0;box-shadow:0 1px 3px rgba(0,0,0,0.1);border:1px solid var(--border)">';
    html += '<div style="display:flex;align-items:center;justify-content:space-between">';
    html += '<div>';
    html += '<span style="font-size:1.1rem;font-weight:bold;color:var(--panel-fg)">' + (moduleLabels[mod.name] || mod.description || mod.name) + '</span>' + statusBadge;
    // Always show the description as a subtitle when present (and distinct
    // from the title). The earlier !moduleLabels[mod.name] guard hid it
    // for every labelled module, leaving cards without context.
    if (mod.description && mod.description !== (moduleLabels[mod.name] || mod.name)) {
      html += '<div style="color:var(--panel-fg);font-size:0.85rem;margin-top:0.25rem">' + mod.description + '</div>';
    }
    html += '</div>';
    html += '<button class="btn" id="btn-' + mod.name + '-toggle" onclick="togglePane(\'' + mod.name + '-pane\',\'btn-' + mod.name + '-toggle\')">Expand</button>';
    html += '</div>';
    html += '<div id="' + mod.name + '-pane" style="display:none;margin-top:0.75rem">';
    
    if (isOrphan) {
      html += '<div style="background:var(--crumb-bg);padding:0.75rem;margin-bottom:1rem;color:var(--panel-fg);font-size:0.85rem">';
      html += 'Module not included in current build. Settings are preserved but read-only.';
      html += '</div>';
    } else if (isDisconnected) {
      html += '<div style="background:rgba(255,152,0,0.08);padding:0.75rem;margin-bottom:1rem;color:var(--panel-fg);opacity:0.8;font-size:0.85rem">';
      html += 'Module not connected, settings can still be changed.';
      html += '</div>';
    }
    
    function getValue(e) { return readEntryValue(section, e); }

    if (devEntries.length > 0) {
      html += '<div style="font-weight:bold;margin-bottom:0.5rem;color:var(--panel-fg);border-bottom:1px solid var(--border);padding-bottom:0.25rem">Device Settings</div>';
      html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
      devEntries.forEach(function(e) { html += renderInput(e, getValue(e), isOrphan); });
      html += '</div>';
    }
    if (uiEntries.length > 0) {
      html += '<div style="font-weight:bold;margin-bottom:0.5rem;color:var(--panel-fg);border-bottom:1px solid var(--border);padding-bottom:0.25rem">Client UI Settings</div>';
      html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
      uiEntries.forEach(function(e) { html += renderInput(e, getValue(e), isOrphan); });
      html += '</div>';
    }
    if (otherEntries.length > 0) {
      // For camera module, separate ESP-NOW related settings
      var espnowKeys = ['cameraSendAfterCapture', 'cameraTargetDevice'];
      var regularEntries = otherEntries.filter(function(e) { return espnowKeys.indexOf(e.key) === -1; });
      var espnowEntries = otherEntries.filter(function(e) { return espnowKeys.indexOf(e.key) !== -1; });

      if (regularEntries.length > 0) {
        html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
        regularEntries.forEach(function(e) { html += renderInput(e, getValue(e), isOrphan); });
        html += '</div>';
      }

      if (espnowEntries.length > 0 && mod.name === 'camera') {
        html += '<div style="font-weight:bold;margin:1rem 0 0.5rem 0;color:var(--panel-fg);border-bottom:1px solid var(--border);padding-bottom:0.25rem">ESP-NOW Integration</div>';
        html += '<div style="background:rgba(100,149,237,0.1);padding:0.5rem 0.75rem;margin-bottom:0.75rem;color:var(--panel-fg);font-size:0.85rem">';
        html += 'Send captured images to another device via ESP-NOW mesh network.';
        html += '</div>';
        html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
        espnowEntries.forEach(function(e) { html += renderInput(e, getValue(e), isOrphan); });
        html += '</div>';
      } else if (espnowEntries.length > 0) {
        html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
        espnowEntries.forEach(function(e) { html += renderInput(e, getValue(e), isOrphan); });
        html += '</div>';
      }
    }
    if (!isOrphan) {
      html += '<button class="btn" onclick="saveDynamicSettings(\'' + mod.name + '\',\'' + mod.section + '\')">Save ' + (moduleLabels[mod.name] || mod.name) + ' Settings</button>';
    }
    if (mod.name === 'led' && !isOrphan) {
      html += renderLedLiveControls();
    }

    // Render ML subsection if this module has one
    var mlModName = mlSubsections[mod.name];
    if (mlModName && allModules) {
      var mlMod = allModules.find(function(m) { return m.name === mlModName; });
      if (mlMod) {
        var mlSection = readModuleSection(allSettings, mlMod);
        var mlEntries = mlMod.entries || [];
        html += '<div style="margin-top:1rem;padding-top:1rem;border-top:1px solid var(--border)">';
        html += '<div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:0.5rem">';
        html += '<span style="font-weight:bold;color:var(--panel-fg)">' + (moduleLabels[mlModName] || 'Machine Learning') + '</span>';
        html += '<button class="btn" id="btn-' + mlModName + '-toggle" onclick="togglePane(\'' + mlModName + '-pane\',\'btn-' + mlModName + '-toggle\')">Expand</button>';
        html += '</div>';
        html += '<div id="' + mlModName + '-pane" style="display:none">';
        html += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
        mlEntries.forEach(function(e) {
          html += renderInput(e, readEntryValue(mlSection, e), isOrphan);
        });
        html += '</div>';
        if (!isOrphan) {
          html += '<button class="btn" onclick="saveDynamicSettings(\'' + mlModName + '\',\'' + mlMod.section + '\')">Save ' + (moduleLabels[mlModName] || mlModName) + ' Settings</button>';
        }
        html += '</div></div>';
      }
    }
    
    html += '</div></div>';
    return html;
  }
  
  Promise.all([
    hw.fetchJSON('/api/settings/schema'),
    hw.fetchJSON('/api/settings')
  ]).then(function(results) {
    var schema = results[0];
    var settingsResp = results[1];
    var settings = settingsResp.settings || {};
    var container = document.getElementById('sensors-dynamic-container');

    if (!container) return;

    var schemaModuleNames = (schema.modules || []).map(function(m) { return m.name; });
    var schemaSections = (schema.modules || []).map(function(m) { return m.section; });
    
    var allKnownModules = sensorModules.concat(hardwareTopModules).concat(i2cModules).concat(outputModules).concat(appsModules).concat(loggingModules);
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
    var outputHtml = '';
    var appsHtml = '';
    var loggingHtml = '';
    var hwLedHtml = '';

    // Render active (compiled) modules first
    var allMods = schema.modules || [];
    relevantModules.forEach(function(mod) {
      if (i2cModules.indexOf(mod.name) !== -1) {
        var sec = readModuleSection(settings, mod);
        var ents = mod.entries || [];
        i2cHtml += '<div id="i2c-pane"><div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
        ents.forEach(function(e) {
          i2cHtml += renderInput(e, readEntryValue(sec, e), false);
        });
        i2cHtml += '</div><button class="btn" onclick="saveDynamicSettings(\'' + mod.name + '\',\'' + mod.section + '\')">Save I2C Bus Configuration Settings</button></div>';
      } else if (outputModules.indexOf(mod.name) !== -1) {
        // Render output module flat (no inner card/badge/expand) — outer panel provides the expand
        // Auth toggles (serialRequireAuth, displayRequireAuth) are shown in Admin Controls > Authentication
        var authCmds = ['serialrequireauth', 'displayrequireauth'];
        var sec = readModuleSection(settings, mod);
        var ents = (mod.entries || []).filter(function(e) { return authCmds.indexOf(e.cmdKey) === -1; });
        outputHtml += '<div id="output-pane"><div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
        ents.forEach(function(e) {
          outputHtml += renderInput(e, readEntryValue(sec, e), false);
        });
        outputHtml += '</div><button class="btn" onclick="saveDynamicSettings(\'' + mod.name + '\',\'' + mod.section + '\')">Save Output Channels Settings</button></div>';
      } else if (hardwareTopModules.indexOf(mod.name) !== -1) {
        hwLedHtml += renderModule(mod, settings, false, allMods, settings);
      } else if (appsModules.indexOf(mod.name) !== -1) {
        appsHtml += renderModule(mod, settings, false, allMods, settings);
      } else if (loggingModules.indexOf(mod.name) !== -1) {
        loggingHtml += renderModule(mod, settings, false, allMods, settings);
      } else {
        html += renderModule(mod, settings, false, allMods, settings);
      }
    });

    // Render orphan modules (not compiled but settings exist)
    orphanModules.forEach(function(mod) {
      if (i2cModules.indexOf(mod.name) !== -1) {
        var ents = mod.entries || [];
        i2cHtml += '<div id="i2c-pane"><div style="background:var(--crumb-bg);padding:0.75rem;margin-bottom:1rem;color:var(--panel-fg);font-size:0.85rem">Module not included in current build. Settings are preserved but read-only.</div><div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem">';
        ents.forEach(function(e) { i2cHtml += renderInput(e, e.value, true); });
        i2cHtml += '</div></div>';
      } else if (outputModules.indexOf(mod.name) !== -1) {
        // Orphan: flat render with disabled inputs (auth entries excluded)
        var authCmds = ['serialrequireauth', 'displayrequireauth'];
        var ents = (mod.entries || []).filter(function(e) { return authCmds.indexOf(e.cmdKey) === -1; });
        outputHtml += '<div style="background:var(--crumb-bg);padding:0.75rem;margin-bottom:1rem;color:var(--panel-fg);font-size:0.85rem">Module not included in current build. Settings are preserved but read-only.</div>';
        outputHtml += '<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem">';
        ents.forEach(function(e) { outputHtml += renderInput(e, e.value, true); });
        outputHtml += '</div>';
      } else if (appsModules.indexOf(mod.name) !== -1) {
        appsHtml += renderModule(mod, settings, true, allMods, settings);
      } else if (loggingModules.indexOf(mod.name) !== -1) {
        loggingHtml += renderModule(mod, settings, true, allMods, settings);
      } else {
        html += renderModule(mod, settings, true, allMods, settings);
      }
    });

    // Populate logging container
    var loggingCont = document.getElementById('logging-dynamic-container');
    if (loggingCont) {
      loggingCont.innerHTML = loggingHtml || '<div style="text-align:center;padding:2rem;color:var(--panel-fg);font-style:italic">Logging settings not available</div>';
      if (loggingHtml) window._snapshotContainer(loggingCont);
    }

    // Populate i2c container
    var i2cCont = document.getElementById('i2c-bus-dynamic-container');
    if (i2cCont) {
      i2cCont.innerHTML = i2cHtml || '<div style="text-align:center;padding:2rem;color:var(--panel-fg);font-style:italic">I2C settings not available</div>';
    }

    // Populate Hardware LED card host (sibling of Sensors/I2C inside Hardware)
    var hwLedCont = document.getElementById('hw-led-container');
    if (hwLedCont) {
      hwLedCont.innerHTML = hwLedHtml || '';
      if (hwLedHtml) window._snapshotContainer(hwLedCont);
    }

    // Populate output container
    var outputCont = document.getElementById('output-dynamic-container');
    if (outputCont) {
      outputCont.innerHTML = outputHtml || '<div style="text-align:center;padding:2rem;color:var(--panel-fg);font-style:italic">Output channel settings not available</div>';
      if (outputHtml) window._snapshotContainer(outputCont);
    }

    // Populate Apps container (automation, llm, espsr, edgeimpulse)
    var appsCont = document.getElementById('apps-dynamic-container');
    if (appsCont) {
      appsCont.innerHTML = appsHtml || '<div style="text-align:center;padding:2rem;color:var(--panel-fg);font-style:italic">No apps available in this build</div>';
      if (appsHtml) window._snapshotContainer(appsCont);
    }
    
    if (html === '') {
      container.innerHTML = '<div style="text-align:center;padding:2rem;color:var(--panel-fg);font-style:italic">No sensor settings available</div>';
      return;
    }
    
    container.innerHTML = html;
    window._snapshotContainer(container);
    if (i2cCont) window._snapshotContainer(i2cCont);
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
        if (!el.value || el.value.trim() === '') return;
        val = el.value;
      }
      else val = el.value;
      if (el.type !== 'password' && !window._isChanged(el.id, val)) return;
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
      var el = pane.querySelector('#dyn-' + k.replace(/\./g, '-'));
      var cmd = el && el.getAttribute('data-cmd');
      cmds.push((cmd || k) + ' ' + updates[k]);
    }

    if (cmds.length === 0) { alert('No changes to save.'); return; }

    sendSequential(cmds,
      function() { window._snapshotContainer(pane); alert('Settings saved! Some changes may require a reboot.'); },
      function(e) { alert('Save failed: ' + (e ? e.message : 'unknown')); }
    );
  };
})();
</script>
)SETPART4", HTTPD_RESP_USE_STRLEN);

#if ENABLE_I2C_SYSTEM
  // I2C Bus Configuration — nested card inside Hardware umbrella
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
#endif // ENABLE_I2C_SYSTEM

  // LED card host inside Hardware. Populated by the Sensors IIFE script
  // alongside the other module renders. Then close the Hardware umbrella.
  httpd_resp_send_chunk(req, R"HWCLOSE(
<div id='hw-led-container'></div>
</div>
</div>
)HWCLOSE", HTTPD_RESP_USE_STRLEN);

  // Apps umbrella — high-level features that run on top of the hardware:
  // automation, on-device LLM, ESP-SR voice recognition, Edge Impulse ML.
  // Cards are populated by the Sensors IIFE based on which modules the
  // current build includes; the umbrella stays even if some are absent.
  httpd_resp_send_chunk(req, R"APPSPART(
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Apps</div><div style='color:var(--panel-fg);font-size:0.9rem'>Automation, on-device LLM, voice recognition, and ML.</div></div>
    <button class='btn' id='btn-apps-toggle' onclick="togglePane('apps-pane','btn-apps-toggle')">Expand</button>
  </div>
  <div id='apps-pane' style='display:none;margin-top:0.75rem'>
    <div id='apps-dynamic-container'>
      <div style='text-align:center;padding:2rem;color:var(--panel-fg);font-style:italic'>No apps available in this build</div>
    </div>
  </div>
</div>
)APPSPART", HTTPD_RESP_USE_STRLEN);

  // Logging section (standalone)
  httpd_resp_send_chunk(req, R"LOGGINGPART(
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Logging</div><div style='color:var(--panel-fg);font-size:0.9rem'>Configure automated logging to files.</div></div>
    <button class='btn' id='btn-logging-toggle' onclick="togglePane('logging-panel-pane','btn-logging-toggle')">Expand</button>
  </div>
  <div id='logging-panel-pane' style='display:none;margin-top:0.75rem'>
    <div id='logging-dynamic-container'>
      <div style='text-align:center;padding:2rem;color:var(--panel-fg)'>Loading logging settings...</div>
    </div>
  </div>
</div>
)LOGGINGPART", HTTPD_RESP_USE_STRLEN);

  // Output Channels section (standalone)
  httpd_resp_send_chunk(req, R"OUTPUTPART(
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Output Channels</div><div style='color:var(--panel-fg);font-size:0.9rem'>Configure serial, web, and display output routing.</div></div>
    <button class='btn' id='btn-output-toggle' onclick="togglePane('output-panel-pane','btn-output-toggle')">Expand</button>
  </div>
  <div id='output-panel-pane' style='display:none;margin-top:0.75rem'>
    <div id='output-dynamic-container'>
      <div style='text-align:center;padding:2rem;color:var(--panel-fg)'>Loading output settings...</div>
    </div>
  </div>
</div>
)OUTPUTPART", HTTPD_RESP_USE_STRLEN);

  // (On-Device LLM panel folded into the Apps umbrella above.)

  // Part 6: (LED settings now rendered dynamically via schema in Sensors panel)
  httpd_resp_send_chunk(req, R"SETPART6(
<!-- LED settings removed - now in schema-driven Sensors panel -->
)SETPART6", HTTPD_RESP_USE_STRLEN);

  // Part 7: Debug Controls section (dynamic from schema)
  httpd_resp_send_chunk(req, R"SETPART7(
<div class='settings-panel'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Debug Controls</div>
    <div style='color:var(--panel-fg);font-size:0.85rem'>Click a category name to toggle all flags in that group</div></div>
    <button class='btn' id='btn-debug-toggle' onclick="togglePane('debug-pane','btn-debug-toggle')">Expand</button>
  </div>
  <div id='debug-pane' style='display:none;margin-top:0.75rem'>
  <style>
  .dbg-sw{position:relative;display:inline-block;width:34px;height:18px;flex-shrink:0}
  .dbg-sw input{opacity:0;width:0;height:0;position:absolute}
  .dbg-sw .sl{position:absolute;cursor:pointer;inset:0;background:var(--border);border-radius:9px;transition:background .2s}
  .dbg-sw input:checked+.sl{background:var(--accent)}
  .dbg-sw .sl::before{content:'';position:absolute;height:12px;width:12px;left:3px;bottom:3px;background:#fff;border-radius:50%;transition:transform .15s}
  .dbg-sw input:checked+.sl::before{transform:translateX(16px)}
  .dbg-card{background:var(--panel-bg);border:1px solid var(--border);border-radius:8px;padding:0.6rem 0.75rem;transition:border-color .25s}
  .dbg-card.on{border-color:var(--accent)}
  .dbg-card-hdr{display:flex;align-items:center;gap:6px;cursor:pointer;-webkit-user-select:none;user-select:none;padding-bottom:6px;border-bottom:1px solid var(--border)}
  .dbg-card-hdr:hover span:last-child{color:var(--accent)}
  .dbg-dot{width:7px;height:7px;border-radius:50%;background:#555;transition:background .25s;flex-shrink:0}
  .dbg-card.on .dbg-dot{background:var(--accent)}
  .dbg-card-hdr span:last-child{font-weight:600;font-size:0.92rem;color:var(--panel-fg);transition:color .15s}
  .dbg-row{display:flex;align-items:center;justify-content:space-between;padding:3px 0}
  .dbg-row .lbl{font-size:0.84rem;color:var(--panel-fg);opacity:0.85}
  .dbg-pill{display:flex;align-items:center;justify-content:space-between;border:1px solid var(--border);border-radius:6px;padding:4px 10px;transition:border-color .2s}
  .dbg-pill.on{border-color:var(--accent)}
  .dbg-pill .lbl{font-size:0.84rem;color:var(--panel-fg);margin-right:10px;white-space:nowrap}
  </style>
  <div id='debug-dynamic-container'><div style='text-align:center;padding:2rem;color:var(--panel-fg);opacity:0.4'>Loading...</div></div>
  </div>
</div>
<script>
(function(){
  var GL={authentication:'Authentication',http:'HTTP',https:'HTTPS',sse:'SSE',wifi:'WiFi',storage:'Storage','esp-now':'ESP-NOW',bluetooth:'Bluetooth',g2:'G2 Glasses',system:'System',users:'Users',cli:'CLI',commands:'Commands',performance:'Performance',automations:'Automations',sensors:'Sensors',camera:'Camera',microphone:'Microphone',gps:'GPS',rtc:'RTC',presence:'Presence',fmradio:'FM Radio',thermal:'Thermal',imu:'IMU',gamepad:'Gamepad',input:'Input',anoencoder:'Ano Encoder',tof:'ToF',apds:'APDS',maps:'Maps',datetime:'Date / Time',llm:'LLM',espsr:'ESP-SR Speech',memory:'Memory',mqtt:'MQTT',i2c:'I2C',display:'Display',oled:'OLED',logger:'Sensor Data Logger',channels:'Channels',auth:'Auth',boot:'Boot',broker:'Broker',topics:'Topics',publish:'Publish',image:'Image',tuning:'Tuning',storage:'Storage',autoCapture:'Auto Capture',timing:'Timing',orientation:'Orientation',filtering:'Filtering',interpolation:'Interpolation',scale:'Scale',mesh:'Mesh',bond:'Bond',identity:'Identity',buffers:'Buffers',capture:'Capture',lifecycle:'Lifecycle',polling:'Polling',values:'Values',page:'Page Console'};
  // Hover help for each debug flag. Keyed by the CLI cmdKey so it survives
  // label changes. Missing keys fall back to a generic group-level hint.
  var HELP={
    // authentication
    debugauth:"Master toggle for all authentication-related debug logs — sessions, cookies, login, boot ID.",
    debugauthsessions:"Logs session creation, lookup, expiry, and invalidation across the web and CLI transports.",
    debugauthcookies:"Logs Set-Cookie responses and the parsed session cookies seen on each incoming request.",
    debugauthlogin:"Logs each login attempt: username seen, credential match result, session issued.",
    debugauthbootid:"Logs the per-boot auth ID that invalidates all sessions across a reboot. Useful for diagnosing unexpected logouts.",
    // http
    debughttp:"Master toggle for HTTP server debug logs (handlers, requests, responses, streaming).",
    debughttphandlers:"Logs handler registration and dispatch when a URI matches a registered handler.",
    debughttprequests:"Logs every incoming request line, method, URI, and parsed headers.",
    debughttpresponses:"Logs each response status code, Content-Length, and the handler that produced it.",
    debughttpstreaming:"Logs chunked-response streaming progress including chunk sizes and flush boundaries.",
    // sse
    debugsse:"Master toggle for Server-Sent Events debug output.",
    debugsseconnection:"Logs SSE client connect / disconnect and subscription lifecycle.",
    debugsseevents:"Logs each event dispatched (event type, channel, payload size).",
    debugssebroadcast:"Logs the SSE broadcast fanout — which subscribers each event is delivered to.",
    // wifi
    debugwifi:"Master toggle for WiFi subsystem logs.",
    debugwificonnection:"Logs state transitions: connecting, connected, disconnected, reconnect attempts.",
    debugwificonfig:"Logs saved-network config loads and credential writes.",
    debugwifiscanning:"Logs scan starts, AP list results, and signal strengths.",
    debugwifidriver:"Low-level WiFi driver events from the ESP-IDF wifi event loop. Verbose.",
    // storage
    debugstorage:"Master toggle for LittleFS storage-subsystem logs.",
    debugstoragefiles:"Logs file open / read / write / close / rename operations.",
    debugstoragejson:"Logs JSON parse and serialize operations against saved files.",
    debugstoragesettings:"Logs settings-file load and save cycles.",
    debugstoragemigration:"Logs settings-schema migrations applied during load.",
    // esp-now
    debugespnow:"Master toggle for ESP-NOW subsystem logs.",
    debugespnowstream:"Logs streaming-protocol frames (data, ack, resume).",
    debugespnowcore:"Core driver events: peer add/remove, send status, TX callbacks.",
    debugespnowrouter:"Logs the command router that decides whether an RPC runs locally or forwards to a bonded peer.",
    debugespnowmesh:"Logs mesh/discovery heartbeats and peer table updates.",
    debugespnowtopo:"Logs topology changes — new neighbors, lost peers, link-quality updates.",
    debugespnowencryption:"Logs LMK/PMK key installation and encrypted peer handshakes.",
    debugespnowmetadata:"Logs metadata-sync frames exchanged between bonded peers.",
    // bluetooth
    debugbluetooth:"Master toggle for BLE debug output (server mode).",
    debugbluetoothcore:"Logs BLE stack init, advertise start/stop, GAP events.",
    debugbluetoothgatt:"Logs GATT service/characteristic registration and read/write operations.",
    debugbluetoothdata:"Logs raw data flowing through GATT characteristics — verbose.",
    // system
    debugsystem:"Master toggle for core-system debug logs.",
    debugsystemboot:"Logs the boot sequence — component init, setup phases, timing.",
    debugsystemconfig:"Logs runtime config reads and build-flag feature status.",
    debugsystemtasks:"Logs FreeRTOS task creation, deletion, and stack-watermark snapshots.",
    debugsystemhardware:"Logs hardware probes (I2C ping, peripheral detection) during boot.",
    // users
    debugusers:"Master toggle for user-management debug logs.",
    debugusersmgmt:"Logs admin user-management actions: create, delete, promote, demote.",
    debugusersregister:"Logs registration requests and approval/denial flow.",
    debugusersquery:"Logs user-lookup queries (e.g. isValidUser, getRole).",
    // cli
    debugcli:"Master toggle for CLI-subsystem debug output.",
    debugcliexecution:"Logs each CLI command as it executes — user, source, result.",
    debugcliqueue:"Logs the CLI command queue: enqueue, dequeue, pending count.",
    debugclivalidation:"Logs the validate-only pass used to check commands before execution.",
    // commands
    debugcommandflow:"Master toggle for command-dispatch pipeline logs.",
    debugcommandsystem:"Logs the command-registry system: module registration, command lookup.",
    debugcmdflowrouting:"Logs which destination a command is routed to (local vs. bonded peer).",
    debugcmdflowqueue:"Logs the command-dispatch queue internals.",
    debugcmdflowcontext:"Logs the auth context (user, source, session) attached to each executing command.",
    // performance
    debugperformance:"Master toggle for performance-monitoring logs.",
    debugperfstack:"Periodic task stack-watermark dumps so you can spot tasks approaching their limit.",
    debugperfheap:"Periodic heap-free and largest-free-block snapshots to track fragmentation.",
    debugperftiming:"Logs timing of key operations (scheduler ticks, sensor polls, render passes).",
    // automations
    debugautomations:"Master toggle for automation-system debug logs.",
    debugautoscheduler:"Logs scheduler ticks and which automations are evaluated each cycle.",
    debugautoexec:"Logs each automation execution — commands fired, results broadcast.",
    debugautocondition:"Logs IF/THEN condition evaluation inside automation command lists.",
    debugautotiming:"Logs nextAt computation and post-fire reschedule details. Noisy when many automations exist.",
    // sensors
    debugcamera:"All camera (OV2640/OV3660) debug output: init, frame capture, streaming.",
    // microphone
    debugmicrophone:"Master toggle for microphone debug (I2S init, recording sessions, level calc).",
    debugmiclifecycle:"Logs microphone start/stop and recording-task lifecycle.",
    debugmicpolling:"Logs microphone capture cadence — verbose during recording.",
    debugmicvalues:"Logs microphone level-meter readings and audio sample stats.",
    // gps
    debuggps:"Master toggle for GPS (PA1010D) debug.",
    debuggpslifecycle:"Logs GPS init, connect, and recovery.",
    debuggpspolling:"Logs GPS poll cadence and NMEA-sentence parse activity.",
    debuggpsvalues:"Logs GPS fix coordinates, satellite count, and parsed NMEA payload data.",
    // rtc
    debugrtc:"Master toggle for RTC (DS3231) debug.",
    debugrtclifecycle:"Logs RTC init, connect, recovery, and time-source switches.",
    debugrtcpolling:"Logs RTC poll cadence.",
    debugrtcvalues:"Logs RTC time-read values and drift correction.",
    // presence
    debugpresence:"Master toggle for presence-sensor (STHS34PF80) debug.",
    debugpresencelifecycle:"Logs presence-sensor init, connect, and auto-disable.",
    debugpresencepolling:"Logs presence-sensor poll cadence.",
    debugpresencevalues:"Logs presence/motion detection events and value changes.",
    // fmradio
    debugfmradio:"Master toggle for FM Radio (RDA5807) debug.",
    debugfmradiolifecycle:"Logs FM radio init, tune, and recovery.",
    debugfmradiopolling:"Logs FM radio poll cadence and tuning-task activity.",
    debugfmradiovalues:"Logs FM radio RDS strings, RSSI/signal values, and station info.",
    // thermal
    debugthermal:"Master toggle for thermal camera (MLX90640) debug.",
    debugthermallifecycle:"Logs thermal sensor init, connect, and recovery.",
    debugthermalpolling:"Logs per-frame capture timing — very noisy at higher frame rates.",
    debugthermalvalues:"Logs the raw pixel values / min-max-avg stats of each captured frame. Extremely verbose.",
    // imu
    debugimu:"Master toggle for IMU (BNO055) debug.",
    debugimulifecycle:"Logs IMU init, connect, and recovery.",
    debugimupolling:"Logs each IMU poll cycle (orientation + accel + gyro).",
    debugimuvalues:"Logs the raw quaternion and Euler-angle values read from the sensor. Verbose.",
    // gamepad
    debuggamepad:"Master toggle for Seesaw gamepad debug.",
    debuggamepadlifecycle:"Logs gamepad init, connect, and auto-disable.",
    debuggamepadpolling:"Logs each gamepad read cycle (stick + button snapshot).",
    debuggamepadvalues:"Logs the raw button mask and stick XY values. Verbose.",
    // tof
    debugtof:"Master toggle for Time-of-Flight (VL53L4CX) debug.",
    debugtoflifecycle:"Logs ToF init, connect, and recovery.",
    debugtofpolling:"Logs each ToF measurement frame — detected objects, distances, statuses.",
    debugtofvalues:"Logs ToF distance and object-detection values.",
    // apds
    debugapds:"Master toggle for APDS9960 debug (color + proximity + gesture).",
    debugapdslifecycle:"Logs APDS init, connect, and recovery.",
    debugapdspolling:"Logs each APDS poll cycle — RGB color, clear, proximity, gesture events.",
    debugapdsvalues:"Logs APDS color, proximity, and gesture values.",
    // maps
    debugmaps:"Master toggle for offline-maps subsystem debug.",
    debugmapsloading:"Logs tile file opens and chunk reads from LittleFS.",
    debugmapsrendering:"Logs map-render passes and buffer writes to the OLED.",
    debugmapsperf:"Performance counters for map rendering: per-tile draw time, cache hit rate.",
    // llm
    debugllm:"Master toggle for on-device LLM debug (when ENABLE_ONDEVICE_LLM is built in).",
    debugllmload:"Logs model checkpoint loading and weight-tensor allocation.",
    debugllmtokenizer:"Logs tokenizer operations — encode and decode calls.",
    debugllmforward:"Logs forward-pass execution steps. Very verbose during generation.",
    debugllmgenerate:"Logs token generation — prompts, sampled tokens, termination conditions.",
    debugllmmemory:"Logs PSRAM allocations and model memory footprint.",
    // datetime
    debugdatetime:"Master toggle for NTP / DateTime subsystem debug.",
    debugdatetimesync:"Logs the NTP sync loop — requests, responses, drift calculations.",
    debugdatetimesetup:"Logs configTime() calls and timezone parsing.",
    debugdatetimeanchor:"Logs boot-anchor timestamps used to recover time across reboots.",
    debugdatetimeresolve:"Logs timestamp-resolution calls used when no RTC/NTP is available.",
    // standalone (no group)
    debuglogger:"Logs the logger subsystem itself — mostly for debugging the debug infrastructure.",
    debugmemory:"Enables periodic memory reports (DRAM, PSRAM, task stacks) in the log.",
    debugsettingssystem:"Logs settings-module registration and validation errors.",
    // g2
    debugg2:"Master toggle for Even Realities G2 glasses debug output.",
    debugg2lifecycle:"Logs scan, BLE connect/disconnect, MTU negotiation, and service enumeration.",
    debugg2protocol:"Logs envelope TX/RX, CRC, fragmentation, and parse errors.",
    debugg2events:"Logs DevEvents, ListEvents, SysEvents, gestures, and taps from the lens.",
    debugg2pages:"Logs the page-swap worker, hijack flow, CREATE-list/text, and lens state transitions.",
    debugg2heartbeat:"Logs heartbeat TX and HeartbeatAck (every ~5 s — loud).",
    debugg2dump:"Logs [G2-DUMP] diagnostic ring-buffer dumps fired on protocol errors.",
    // espsr (ESP-SR speech recognition)
    debugsr:"Master toggle for ESP-SR speech recognition (WakeNet + MultiNet + AFE).",
    debugsrwake:"Logs wake-word detections, scores, and post-trigger transitions.",
    debugsrcommand:"Logs MultiNet command recognition, candidate matches, and final decisions.",
    debugsrafe:"Logs AFE chain — VAD edges, noise-suppression gain, AGC adjustments. Verbose.",
    debugsrlifecycle:"Logs SR init / start / stop, model resolution, and partition mounting.",
    debugsrtuning:"Logs auto-tune sweeps, confidence threshold updates, and SNR snapshots.",
    debugi2c:"Logs low-level I2C bus transactions: start, address, ack/nack, clock changes.",
    debugmqtt:"Logs MQTT client connection, publish, subscribe, and message delivery.",
    webconsole:"Allow the served web pages to emit normal JavaScript console.log/warn/debug to your browser DevTools. Default OFF mutes those calls (page-side noise only). Does NOT route firmware output to the browser console.",
    loglevel:"Minimum severity that gets printed: Error, Warn, Info, or Debug. Debug is most verbose.",
    memorysampleintervalsec:"How often (in seconds) the memory monitor records a snapshot. Lower = more detail, more CPU."
  };
  // Generic fallback for group sub-flags not in the map above.
  function helpFor(cmd, grp, isAll){
    if(HELP[cmd]) return HELP[cmd];
    var gl = GL[grp] || grp || '';
    if(isAll) return 'Master toggle for all ' + gl + ' debug output.';
    return gl ? (gl + ' debug flag.') : '';
  }
  function esc(s){return (s||'').replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
  function sw(cmd,grp,on,isAll){
    var tip = esc(helpFor(cmd, grp, isAll));
    var tAttr = tip ? ' title="'+tip+'"' : '';
    return '<label class="dbg-sw"'+tAttr+'><input type="checkbox" class="dbg-cb" data-cmd="'+cmd+'"'+(grp?' data-group="'+grp+'"':'')+(isAll?' data-all="1"':'')+(on?' checked':'')+'><span class="sl"></span></label>';
  }
  Promise.all([
    hw.fetchJSON('/api/settings/schema'),
    hw.fetchJSON('/api/settings')
  ]).then(function(res){
    var schema=res[0],settings=(res[1].settings||{}),c=document.getElementById('debug-dynamic-container');
    if(!c)return;
    var dm=null;(schema.modules||[]).forEach(function(m){if(m.name==='debug')dm=m;});
    if(!dm){c.innerHTML='<div>Debug module not found</div>';return;}
    var entries=dm.entries||[],dbg=((settings.system&&settings.system.debug)||settings.debug||{}),groups={},gOrder=[],standalone=[];
    entries.forEach(function(e){
      if(e.group){if(!groups[e.group]){groups[e.group]=[];gOrder.push(e.group);}groups[e.group].push(e);}
      else standalone.push(e);
    });
    gOrder.sort(function(a,b){return (GL[a]||a).localeCompare(GL[b]||b);});
    var h='<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:0.6rem">';
    gOrder.forEach(function(gn){
      var ge=groups[gn],gl=GL[gn]||gn,gid='dbg-'+gn.replace(/[^a-z0-9]/g,'');
      var pe=null,ce=[];
      ge.forEach(function(e){if(e.key==='enabled')pe=e;else ce.push(e);});
      var gd=dbg[gn]||{},anyOn=false;
      ge.forEach(function(e){if(e.type!=='int'&&e.type!=='float'&&gd[e.key])anyOn=true;});
      // Group-level hint: prefer the 'enabled' entry's help text; fall back to generic.
      var groupTip = '';
      if (pe) groupTip = helpFor(pe.cmdKey || pe.key, gn, true);
      else groupTip = 'Master toggle for all ' + gl + ' debug output.';
      h+='<div class="dbg-card'+(anyOn?' on':'')+'" id="'+gid+'" title="'+esc(groupTip)+'">';
      h+='<div class="dbg-card-hdr" onclick="dbgToggleAll(\''+gn+'\')" title="'+esc(groupTip)+'"><span class="dbg-dot"></span><span>'+gl+'</span></div>';
      ge.forEach(function(e){
        var lbl=e.key==='enabled'?'All':'&ensp;'+e.label;
        var rowTip = esc(helpFor(e.cmdKey||e.key, gn, e.key==='enabled'));
        var cmd = e.cmdKey || e.key;
        if (e.type === 'int' || e.type === 'float') {
          // Numeric setting inside a folder — render as number input, not toggle.
          var nv = gd[e.key]; if (nv === undefined) nv = e['default'];
          var step = e.type === 'float' ? '0.01' : '1';
          var mi = e.min !== undefined ? ' min="'+e.min+'"' : '';
          var ma = e.max !== undefined ? ' max="'+e.max+'"' : '';
          h+='<div class="dbg-row" title="'+rowTip+'"><span class="lbl">'+lbl+'</span>';
          h+='<input type="number" class="dbg-input form-input" data-cmd="'+cmd+'" value="'+nv+'" step="'+step+'"'+mi+ma+' style="width:80px"></div>';
        } else {
          // Bool / default — render as toggle switch.
          var v=!!(e.key==='enabled'?gd.enabled:gd[e.key]);
          h+='<div class="dbg-row" title="'+rowTip+'"><span class="lbl">'+lbl+'</span>'+sw(cmd,gn,v,e.key==='enabled')+'</div>';
        }
      });
      h+='</div>';
    });
    h+='</div>';
    var boolSA=standalone.filter(function(e){return e.type==='bool';}),cfgSA=standalone.filter(function(e){return e.type!=='bool';});
    if(boolSA.length>0){
      h+='<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(145px,1fr));gap:0.45rem;margin-top:0.7rem">';
      boolSA.forEach(function(e){
        var v=!!dbg[e.key];
        var pillTip = esc(helpFor(e.cmdKey||e.key, '', false));
        h+='<div class="dbg-pill'+(v?' on':'')+'" title="'+pillTip+'"><span class="lbl">'+e.label+'</span>'+sw(e.cmdKey||e.key,'',v)+'</div>';
      });
      h+='</div>';
    }
    if(cfgSA.length>0){
      h+='<div style="margin-top:0.7rem;display:flex;flex-wrap:wrap;gap:0.6rem 1.5rem;align-items:center">';
      cfgSA.forEach(function(e){
        var v=dbg[e.key];if(v===undefined)v=e['default'];var cmd=e.cmdKey||e.key;
        var cfgTip = esc(helpFor(cmd, '', false));
        var tAttr = cfgTip ? ' title="'+cfgTip+'"' : '';
        if(e.key==='logLevel'){
          h+='<div style="display:flex;align-items:center;gap:0.5rem"'+tAttr+'><span style="font-size:0.88rem;color:var(--panel-fg)">'+e.label+'</span>';
          h+='<select class="dbg-input form-input" data-cmd="'+cmd+'" style="width:130px"'+tAttr+'>';
          h+='<option value="0"'+(v===0?' selected':'')+'>Error</option>';
          h+='<option value="1"'+(v===1?' selected':'')+'>Warn</option>';
          h+='<option value="2"'+(v===2?' selected':'')+'>Info</option>';
          h+='<option value="3"'+(v===3?' selected':'')+'>Debug</option></select></div>';
        } else {
          h+='<div style="display:flex;align-items:center;gap:0.5rem"'+tAttr+'><span style="font-size:0.88rem;color:var(--panel-fg)">'+e.label+'</span>';
          var mi=e.min!==undefined?' min="'+e.min+'"':'',ma=e.max!==undefined?' max="'+e.max+'"':'';
          h+='<input type="number" class="dbg-input form-input" data-cmd="'+cmd+'" value="'+v+'"'+mi+ma+' style="width:80px"'+tAttr+'></div>';
        }
      });
      h+='</div>';
    }
    h+='<div style="margin-top:0.8rem;display:flex;align-items:center;gap:0.75rem">';
    h+='<button class="btn" onclick="saveDebugSettings()">Save</button>';
    h+='<span id="dbg-save-msg" style="font-size:0.82rem;color:var(--accent);opacity:0;transition:opacity .3s"></span></div>';
    c.innerHTML=h;
    window._debugBaseline = {};
    c.querySelectorAll('.dbg-cb,.dbg-input').forEach(function(el) {
      var cmd = el.getAttribute('data-cmd');
      if (cmd) window._debugBaseline[cmd] = el.type === 'checkbox' ? (el.checked ? 1 : 0) : el.value;
    });
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
    var bl=window._debugBaseline||{};
    document.querySelectorAll('.dbg-cb').forEach(function(cb){
      var cmd=cb.getAttribute('data-cmd');if(!cmd)return;
      var val=cb.checked?1:0;
      if(cmd in bl && bl[cmd]===val)return;
      cmds.push(cmd+' '+val);
    });
    document.querySelectorAll('.dbg-input').forEach(function(el){
      var cmd=el.getAttribute('data-cmd');if(!cmd)return;
      var val=el.value;
      if(cmd in bl && bl[cmd]===val)return;
      cmds.push(cmd+' '+val);
    });
    if(!cmds.length){
      var msg=document.getElementById('dbg-save-msg');
      if(msg){msg.textContent='No changes';msg.style.opacity='1';setTimeout(function(){msg.style.opacity='0';},1500);}
      return;
    }
    var msg=document.getElementById('dbg-save-msg');
    if(msg){msg.textContent='Saving...';msg.style.opacity='1';}
    sendSequential(cmds,
      function(){
        document.querySelectorAll('.dbg-cb,.dbg-input').forEach(function(el){
          var cmd=el.getAttribute('data-cmd');
          if(cmd) window._debugBaseline[cmd]=el.type==='checkbox'?(el.checked?1:0):el.value;
        });
        if(msg){msg.textContent='Saved';setTimeout(function(){msg.style.opacity='0';},1500);}
        try{refreshSettings();}catch(_){}
      },
      function(){if(msg){msg.textContent='Error saving';msg.style.color='#f55';}}
    );
  };
})();
</script>
)SETPART7", HTTPD_RESP_USE_STRLEN);

  // Part 8: Admin section and page controls
  httpd_resp_send_chunk(req, R"SETPART8(
<div id='admin-section' style='display:none;background:var(--panel-bg);border-radius:8px;border:1px solid var(--border);padding:1.0rem 1.5rem;margin:1rem 0;color:var(--panel-fg)'>
  <div style='display:flex;align-items:center;justify-content:space-between'>
    <div><div style='font-size:1.2rem;font-weight:bold;color:var(--panel-fg)'>Admin Controls</div><div style='color:var(--panel-fg);font-size:0.9rem'>User management, authentication policy, and HTTPS controls.</div></div>
    <button class='btn' id='btn-admin-toggle' onclick="togglePane('admin-pane','btn-admin-toggle')">Expand</button>
  </div>
  <div id='admin-pane' style='display:none;margin-top:0.75rem'>
  <div style='display:grid;grid-template-columns:1fr;gap:1rem'>
    <div style='background:var(--crumb-bg);border:1px solid var(--border);border-radius:8px;padding:1rem'>
      <div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:0.5rem'>
        <div style='font-weight:bold;color:var(--panel-fg)'>User Management <span id='pending-badge' style='display:none;font-size:0.75rem;font-weight:600;background:#b8860b;color:#fff;padding:1px 7px;border-radius:10px;vertical-align:middle;margin-left:6px'></span></div>
        <button class='btn' id='btn-users-toggle' onclick="togglePane('users-pane','btn-users-toggle')">Expand</button>
      </div>
      <div style='color:var(--panel-fg);margin-bottom:0.75rem;font-size:0.9rem'>Manage existing users and their roles.</div>
      <div id='users-pane' style='display:none;margin-top:0.75rem'>
        <div id='users-list' style='min-height:24px;color:var(--panel-fg);margin-bottom:0.75rem'>Loading...</div>
        <div class='btn-row' style='margin-top:0'>
        <button class='btn' onclick='refreshUsers()' title='Reload list of users'>Refresh Users</button>
        <button class='btn' onclick='openAddUserModal()' title='Create a new user account'>Add User</button>
        </div>
        <div id='add-user-modal' class='modal-overlay' style='display:none;align-items:center;justify-content:center;z-index:10000' onclick='if(event.target===this)closeAddUserModal()'>
          <div class='modal-dialog' role='dialog' aria-modal='true' onclick='event.stopPropagation()'>
            <div style='font-weight:bold;margin-bottom:0.75rem;color:var(--panel-fg)'>Add user</div>
            <div style='display:grid;gap:0.75rem'>
              <div><label style='display:block;font-size:0.85rem;color:var(--muted);margin-bottom:0.25rem'>Username</label>
              <input type='text' id='add-user-name' class='form-input' autocomplete='off'></div>
              <div><label style='display:block;font-size:0.85rem;color:var(--muted);margin-bottom:0.25rem'>Password</label>
              <input type='password' id='add-user-pass' class='form-input' autocomplete='new-password'></div>
              <div><label style='display:block;font-size:0.85rem;color:var(--muted);margin-bottom:0.25rem'>Confirm password</label>
              <input type='password' id='add-user-pass2' class='form-input' autocomplete='new-password'></div>
              <label style='display:flex;align-items:flex-start;gap:0.5rem;cursor:pointer;color:var(--panel-fg);font-size:0.9rem;width:100%;min-width:0;box-sizing:border-box'>
                <input type='checkbox' id='add-user-mustch' style='margin-top:0.15rem;flex-shrink:0;width:1rem;height:1rem'>
                <span style='flex:1 1 0;min-width:0;line-height:1.35'>User must set a new password on next login</span>
              </label>
              <div class='btn-row' style='justify-content:flex-end;margin-top:0.25rem'>
                <button type='button' class='btn' onclick='closeAddUserModal()'>Cancel</button>
                <button type='button' class='btn' onclick='submitAddUser()'>Create</button>
              </div>
            </div>
          </div>
        </div>
        <div id='reset-pw-modal' class='modal-overlay' style='display:none;align-items:center;justify-content:center;z-index:10000' onclick='if(event.target===this)closeResetPasswordModal()'>
          <div class='modal-dialog' role='dialog' aria-modal='true' onclick='event.stopPropagation()'>
            <div style='font-weight:bold;margin-bottom:0.5rem;color:var(--panel-fg)'>Reset password</div>
            <div style='font-size:0.9rem;color:var(--muted);margin-bottom:0.75rem' id='reset-pw-for-user'></div>
            <div style='display:grid;gap:0.75rem'>
              <div><label style='display:block;font-size:0.85rem;color:var(--muted);margin-bottom:0.25rem'>New password</label>
              <input type='password' id='reset-pw-pass' class='form-input' autocomplete='new-password'></div>
              <div><label style='display:block;font-size:0.85rem;color:var(--muted);margin-bottom:0.25rem'>Confirm new password</label>
              <input type='password' id='reset-pw-pass2' class='form-input' autocomplete='new-password'></div>
              <label style='display:flex;align-items:flex-start;gap:0.5rem;cursor:pointer;color:var(--panel-fg);font-size:0.9rem;width:100%;min-width:0;box-sizing:border-box'>
                <input type='checkbox' id='reset-pw-mustch' style='margin-top:0.15rem;flex-shrink:0;width:1rem;height:1rem'>
                <span style='flex:1 1 0;min-width:0;line-height:1.35'>User must set a new password on next login</span>
              </label>
              <div class='btn-row' style='justify-content:flex-end;margin-top:0.25rem'>
                <button type='button' class='btn' onclick='closeResetPasswordModal()'>Cancel</button>
                <button type='button' class='btn' onclick='submitResetPassword()'>OK</button>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
    <div style='background:var(--crumb-bg);border:1px solid var(--border);border-radius:8px;padding:1rem'>
      <div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:0.5rem'>
        <div style='font-weight:bold;color:var(--panel-fg)'>Authentication</div>
        <button class='btn' id='btn-auth-toggle' onclick="togglePane('auth-pane','btn-auth-toggle')">Expand</button>
      </div>
      <div style='color:var(--panel-fg);font-size:0.9rem'>Require login before accepting commands on each interface.</div>
      <div id='auth-pane' style='display:none;margin-top:0.75rem'>
        <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0.75rem;margin-bottom:1rem'>
          <label><input type='checkbox' id='auth-serial' style='margin-right:0.5rem'>Serial Require Auth</label>
          <label><input type='checkbox' id='auth-display' style='margin-right:0.5rem'>Display Require Auth</label>
          <label id='auth-bluetooth-wrap'><input type='checkbox' id='auth-bluetooth' style='margin-right:0.5rem'>Bluetooth Require Auth</label>
        </div>
        <button class='btn' onclick='saveAuthSettings()'>Save Authentication Settings</button>
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
          <div id='https-nocert-row' style='display:none;margin-top:0.5rem;padding:0.75rem;background:rgba(255,255,255,0.05);border:1px solid var(--border);border-radius:6px'>
            <span style='color:var(--panel-fg);font-weight:bold'>Certificates required</span>
            <span style='color:var(--panel-fg);font-size:0.9rem;display:block;margin-top:0.25rem'>HTTPS cannot be enabled until a certificate and private key are present. Use <strong>Generate Certs</strong> below to create a self-signed certificate, or upload your own.</span>
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
  function updateCertPresenceFlag() {
    var certEl = document.getElementById('https-cert-status');
    var keyEl = document.getElementById('https-key-status');
    window._httpsCertsPresent = !!(certEl && certEl.textContent === 'Present' &&
                                   keyEl  && keyEl.textContent  === 'Present');
  }
  function checkCertFile(path, statusId) {
    hw.fetchJSON('/api/files/list?path=' + encodeURIComponent('/system/certs'))
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
        el.style.color = 'var(--accent)';
      })
      .catch(function(){
        var el = document.getElementById(statusId);
        if (el) {
          el.textContent = 'Unknown';
          el.style.color = '#a0aec0';
        }
      });
  }

  window._httpsCertsPresent = false;
  hw.fetchJSON('/api/files/list?path=' + encodeURIComponent('/system/certs'))
    .then(function(j){
      var checks = [
        {path:'/system/certs/https_server.crt', id:'https-cert-status'},
        {path:'/system/certs/https_server.key', id:'https-key-status'}
      ];
      var allPresent = true;
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
        if (!found) allPresent = false;
        el.textContent = found ? 'Present' : 'Missing';
        el.style.color = 'var(--accent)';
      });
      window._httpsCertsPresent = allPresent;
      if (allPresent) document.getElementById('https-nocert-row').style.display = 'none';
    })
    .catch(function(){
      ['https-cert-status','https-key-status'].forEach(function(id){
        var el = document.getElementById(id);
        if (el) { el.textContent='Unknown'; el.style.color='#a0aec0'; }
      });
    });

  window._httpsCurrentValue = false;
  function refreshHttpsStatus() {
    hw.fetchJSON('/api/settings')
      .then(function(j){
        var val = j && j.settings && j.settings.network && j.settings.network.http && j.settings.network.http.httpsEnabled;
        window._httpsCurrentValue = !!val;
        var el = document.getElementById('https-enabled-value');
        if(el){ el.textContent = val ? 'Enabled' : 'Disabled'; el.style.color = 'var(--accent)'; }
      });
  }
  refreshHttpsStatus();

  window.toggleHttps = function(){
    var newVal = !window._httpsCurrentValue;
    // Block enabling if certs aren't present
    if (newVal && !window._httpsCertsPresent) {
      document.getElementById('https-reboot-row').style.display = 'none';
      document.getElementById('https-nocert-row').style.display = 'block';
      return;
    }
    document.getElementById('https-nocert-row').style.display = 'none';
    var cmd = 'httpsEnabled ' + (newVal ? '1' : '0');
    sendSequential([cmd], function(){
      window._httpsCurrentValue = newVal;
      var el = document.getElementById('https-enabled-value');
      if(el){ el.textContent = newVal ? 'Enabled' : 'Disabled'; el.style.color = 'var(--accent)'; }
      document.getElementById('https-reboot-row').style.display = 'block';
    }, function(err){ alert('Failed to toggle HTTPS: ' + err.message); });
  };
  function uploadHttpsFile(file, destPath, statusId, inputEl) {
    var statusEl = document.getElementById(statusId);
    if (statusEl) { statusEl.textContent = 'Uploading...'; statusEl.style.color = '#a0aec0'; }
    var reader = new FileReader();
    reader.onload = function(evt) {
      var content = evt.target.result;
      hw.postForm('/api/files/upload', { path: destPath, binary: '0', content: content })
      .then(function(r){ return r.json(); })
      .then(function(j){
        if (j.success) {
          if (statusEl) { statusEl.textContent = 'Uploaded'; statusEl.style.color = 'var(--accent)'; }
          var fname = destPath.split('/').pop();
          var certId = destPath.indexOf('.crt') >= 0 ? 'https-cert-status' : 'https-key-status';
          var certEl = document.getElementById(certId);
          if (certEl) { certEl.textContent = 'Present'; certEl.style.color = 'var(--accent)'; }
          updateCertPresenceFlag();
        } else {
          if (statusEl) { statusEl.textContent = 'Failed: ' + (j.error || 'unknown'); statusEl.style.color = 'var(--accent)'; }
        }
        if (inputEl) inputEl.value = '';
      })
      .catch(function(e){
        if (statusEl) { statusEl.textContent = 'Error: ' + e.message; statusEl.style.color = 'var(--accent)'; }
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

  window.generateCerts = async function(){
    if(!await hwConfirm('Generate a self-signed ECDSA P-256 certificate? This will overwrite any existing cert/key files.')) return;
    var btn = document.getElementById('https-certgen-btn');
    var status = document.getElementById('https-certgen-status');
    if(btn) btn.disabled = true;
    if(status){ status.textContent = 'Generating...'; status.style.color = '#a0aec0'; }
    postSettingsCli('certgen')
      .then(function(output){
        if(btn) btn.disabled = false;
        if(output.indexOf('Error') >= 0 || output.indexOf('error') >= 0){
          if(status){ status.textContent = output; status.style.color = '#fc8181'; }
        } else {
          if(status){ status.textContent = 'Generated!'; status.style.color = '#68d391'; }
          checkCertFile('/system/certs/https_server.crt', 'https-cert-status');
          checkCertFile('/system/certs/https_server.key', 'https-key-status');
          window._httpsCertsPresent = true;
          document.getElementById('https-nocert-row').style.display = 'none';
        }
      })
      .catch(function(e){
        if(btn) btn.disabled = false;
        if(status){ status.textContent = 'Error: ' + e.message; status.style.color = '#fc8181'; }
      });
  };

  window.rebootDevice = async function(){
    if(!await hwConfirm('Reboot the device now?')) return;
    postSettingsCli('reboot')
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
        // WiFi settings are nested under network.wifi after the v0.93 refactor.
        // Older flat fallbacks (s.wifiSSID etc.) kept defensively in case an
        // upgrade path leaves stale shapes around.
        var wifiSect = (s.network && s.network.wifi) || s.wifi || {};
        __S.state.currentSSID = (s.wifiPrimarySSID || wifiSect.ssid || wifiSect.wifiSSID || '');
        $('wifi-ssid').textContent = __S.state.currentSSID;
        var primary = __S.state.currentSSID || '';
        var netList = wifiSect.networks || s.wifiNetworks || [];
        var list = Array.isArray(netList)
          ? netList.map(function(n) { return n && n.ssid; }).filter(function(x) { return !!x; })
          : [];
        __S.state.savedSSIDs = [];
        if (primary) __S.state.savedSSIDs.push(primary);
        if (list && list.length) __S.state.savedSSIDs = __S.state.savedSSIDs.concat(list);
        var wifiAutoReconnect = (wifiSect.autoReconnect !== undefined ? wifiSect.autoReconnect
                              : (wifiSect.wifiAutoReconnect !== undefined ? wifiSect.wifiAutoReconnect
                              : (s.wifiAutoReconnect || false)));
        $('wifi-value').textContent = wifiAutoReconnect ? 'Enabled' : 'Disabled';
        $('wifi-btn').textContent = wifiAutoReconnect ? 'Disable' : 'Enable';
        // Timezone + NTP are now schema-driven (System Time panel is rendered
        // from /api/settings/schema by the IIFE near the top of the page).
        // refreshSettings no longer pokes their DOM directly.
        // ESP-NOW toggle states handled by schema-driven Network Services panel
        
        // Output and LED settings now rendered dynamically via schema in Sensors panel.
        // (Removed dead reads of s.thermal_mlx90640 / s.tof_vl53l4cx / s.imu_bno055 / s.i2c —
        //  those flat-key sections no longer exist after the v0.93 refactor and the locals
        //  weren't consumed anywhere downstream.)
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
          // Populate Authentication panel
          try {
            // Output auth lives under system.output.auth; bluetooth under network.bluetooth.
            // Older flat shapes kept as fallback in case of mid-upgrade state.
            var outSectV093  = (s.system && s.system.output) || s.output || {};
            var outAuthSect  = outSectV093.auth || outSectV093;
            var btSect       = (s.network && s.network.bluetooth) || s.bluetooth || {};
            var el;
            el = document.getElementById('auth-serial');    if (el) el.checked = outAuthSect.serialRequireAuth  !== false;
            el = document.getElementById('auth-display');   if (el) el.checked = outAuthSect.displayRequireAuth !== false;
            el = document.getElementById('auth-bluetooth'); if (el) el.checked = btSect.bluetoothRequireAuth !== false;
            // Hide bluetooth row if module not present in settings
            var btPresent = (s.network && s.network.bluetooth !== undefined) || (s.bluetooth !== undefined);
            var btWrap = document.getElementById('auth-bluetooth-wrap');
            if (btWrap) btWrap.style.display = btPresent ? '' : 'none';
          } catch(e) {}
        }
      } catch(e) {
        alert('Render error: ' + e.message);
      }
      // Snapshot all hardcoded inputs after they've been populated
      setTimeout(function() { window._snapshotContainer(document.body); }, 0);
    };
    
    // renderOutputRuntime, refreshOutput, setOutputRuntime removed - Output settings now in schema-driven Sensors panel
    
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
      return hw.fetchJSON('/api/buildconfig')
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
      hw.fetchJSON('/api/settings')
        .then(function(d) {
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
      postSettingsCli(cmd)
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
    
    // Toggle bond mode
    window.toggleBondMode = function() {
      postSettingsCli('bondmodeenabled')
      .then(function(t) { refreshSettings(); })
      .catch(function(e) { alert('Error: ' + e.message); });
    };
    
    // saveEspNowSettings removed - ESP-NOW settings now saved via schema-driven saveNetworkSettings

    // Save authentication settings
    window.saveAuthSettings = function() {
      var serial  = document.getElementById('auth-serial');
      var display = document.getElementById('auth-display');
      var ble     = document.getElementById('auth-bluetooth');
      var cmds = [];
      if (serial)  cmds.push('serialrequireauth '  + (serial.checked  ? '1' : '0'));
      if (display) cmds.push('displayrequireauth ' + (display.checked ? '1' : '0'));
      if (ble && document.getElementById('auth-bluetooth-wrap').style.display !== 'none') {
        cmds.push('bluetoothrequireauth ' + (ble.checked ? '1' : '0'));
      }
      Promise.all(cmds.map(function(cmd) { return postSettingsCli(cmd); }))
        .then(function() { refreshSettings(); })
        .catch(function(e) { alert('Error saving auth settings: ' + e.message); });
    };

    // updateTimezone / updateNtpServer removed — System Time panel is now
    // schema-driven (see IIFE near SETPART2). The schema-rendered Save button
    // dispatches via sendSequential so saves batch through beginwrite/savesettings.

    // toggleOutput removed - Output settings now in schema-driven Sensors panel

    // Disconnect WiFi
    window.disconnectWifi = async function() {
      if (await hwConfirm('Are you sure you want to disconnect from WiFi? You may lose connection to this device.')) {
        postSettingsCli('wifidisconnect')
        .then(function(t) {
          alert(t || 'Disconnected');
        })
        .catch(function(e) {
          alert('Error: ' + e.message);
        });
      }
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
    // saveDeviceSensorSettings and saveSensorsUISettings removed - these referenced non-existent HTML inputs
    
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
    // saveLEDSettings removed - LED settings now saved via schema-driven saveDynamicSettings

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
      postSettingsCli('wifiscan json')
      .then(function(txt) {
        console.log('[SETTINGS] WiFi scan result length:', (txt||'').length);
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
            html += '<div style="background:var(--panel-bg);color:var(--panel-fg);border:1px solid ' + border + ';border-radius:6px;padding:0.5rem;display:flex;align-items:center;justify-content:space-between">' + '<div><div style="font-weight:bold">' + ssid + ' ' + badge + '</div><div style="color:var(--panel-fg);font-size:0.85rem">RSSI ' + rssi + ' | CH ' + ch + '</div></div>' + '<button class="btn' + btnCls + '" data-ssid="' + esc + '" data-locked="' + needsPass + '" onclick="(function(b){selectSsid(decodeURIComponent(b.dataset.ssid), b.dataset.locked===\\"true\\");})(this)">Select</button>' + '</div>';
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
    
    window.revokeUserSessions = async function(username) {
      if (!username || !await hwConfirm('Revoke all sessions for user: ' + username + '?')) return;
      var cmd = 'sessionrevoke user ' + username;
      postSettingsCli(cmd)
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

    window.banUserByName = async function(username) {
      if (!username || !await hwConfirm('Ban user "' + username + '"? They will lose all access immediately.')) return;
      var cmd = 'banuser ' + username;
      postSettingsCli(cmd)
      .then(function(t) {
        alert(t || 'User banned');
        try {
          refreshUsers();
        } catch(_) {}
      })
      .catch(function(e) {
        alert('Error: ' + e.message);
      });
    };

    window.unbanUserByName = async function(username) {
      if (!username || !await hwConfirm('Remove ban for user "' + username + '"?')) return;
      var cmd = 'unbanuser ' + username;
      postSettingsCli(cmd)
      .then(function(t) {
        alert(t || 'User unbanned');
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

    window.openAddUserModal = function() {
      var m = $('add-user-modal');
      if (!m) return;
      var n = $('add-user-name'), p = $('add-user-pass'), p2 = $('add-user-pass2'), c = $('add-user-mustch');
      if (n) n.value = '';
      if (p) p.value = '';
      if (p2) p2.value = '';
      if (c) c.checked = false;
      m.style.display = 'flex';
    };
    window.closeAddUserModal = function() {
      var m = $('add-user-modal');
      if (m) m.style.display = 'none';
    };
    window.submitAddUser = function() {
      var n = $('add-user-name'), p = $('add-user-pass'), p2 = $('add-user-pass2'), c = $('add-user-mustch');
      var username = n && n.value ? n.value.trim() : '';
      var pass = p && p.value ? p.value : '';
      var pass2 = p2 && p2.value ? p2.value : '';
      var mustCh = !!(c && c.checked);
      if (!username) { hwAlert('Enter a username'); return; }
      if (pass.length < 6) { hwAlert('Password must be at least 6 characters'); return; }
      if (pass !== pass2) { hwAlert('Passwords do not match'); return; }
      var cmd = 'useradd ' + username + ' ' + pass + ' ' + (mustCh ? '1' : '0');
      postSettingsCli(cmd)
        .then(function(t) {
          if (t && t.indexOf('Error') >= 0) {
            hwAlert(t);
            return;
          }
          closeAddUserModal();
          hwAlert(t || 'User created');
          try { refreshUsers(); } catch (_) {}
        })
        .catch(function(e) { hwAlert('Error: ' + (e && e.message ? e.message : String(e))); });
    };

    window.openResetPasswordModal = function(username) {
      var m = $('reset-pw-modal');
      var lab = $('reset-pw-for-user');
      var p = $('reset-pw-pass'), p2 = $('reset-pw-pass2'), c = $('reset-pw-mustch');
      window._resetPwTargetUser = username || '';
      if (lab) lab.textContent = username ? ('User: ' + username) : '';
      if (p) p.value = '';
      if (p2) p2.value = '';
      if (c) c.checked = false;
      if (m) m.style.display = 'flex';
    };
    window.closeResetPasswordModal = function() {
      var m = $('reset-pw-modal');
      if (m) m.style.display = 'none';
      window._resetPwTargetUser = '';
    };
    window.submitResetPassword = function() {
      var username = window._resetPwTargetUser || '';
      var p = $('reset-pw-pass'), p2 = $('reset-pw-pass2'), c = $('reset-pw-mustch');
      var pass = p && p.value ? p.value : '';
      var pass2 = p2 && p2.value ? p2.value : '';
      var mustCh = !!(c && c.checked);
      if (!username) { hwAlert('Username required'); return; }
      if (pass.length < 6) { hwAlert('Password must be at least 6 characters'); return; }
      if (pass !== pass2) { hwAlert('Passwords do not match'); return; }
      var cmd = 'userresetpassword ' + username + ' ' + pass + ' ' + (mustCh ? '1' : '0');
      postSettingsCli(cmd)
        .then(function(t) {
          if (t && t.indexOf('Error') >= 0) {
            hwAlert(t);
            return;
          }
          closeResetPasswordModal();
          hwAlert(t || 'Password reset');
          try { refreshUsers(); } catch (_) {}
        })
        .catch(function(e) { hwAlert('Error: ' + (e && e.message ? e.message : String(e))); });
    };
    
    window.refreshUsers = function() {
      var container = $('users-list');
      if (!container) return;
      container.innerHTML = 'Loading...';
      Promise.all([
        postSettingsCli('userlist json'),
        postSettingsCli('sessionlist json'),
        postSettingsCli('pendinglist json')
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
        // Update pending badge on the User Management header
        var badge = $('pending-badge');
        if (badge) {
          if (pending.length > 0) {
            badge.textContent = pending.length + ' pending';
            badge.style.display = '';
          } else {
            badge.style.display = 'none';
          }
        }

        // Merge pending users into the main list as synthetic user objects
        var pendingUsernames = {};
        pending.forEach(function(p) { pendingUsernames[p.username || ''] = true; });
        var allUsers = users.slice();
        pending.forEach(function(p) {
          if (!pendingUsernames[p.username]) return;
          allUsers.push({ username: p.username, isPending: true });
        });

        var html = '<div style="display:grid;gap:0.5rem">';
        allUsers.forEach(function(user) {
          var username = user.username || '';
          var isPending = user.isPending || false;
          var isAdmin = (user.role === 'admin') || (user.isAdmin === true);
          var isBanned = user.banned || false;
          var userSessions = sessionsByUser[username] || [];
          var sessionCount = userSessions.length;
          var uid = 'u' + Math.random().toString(36).substr(2, 9);
          var createdAt = user.createdAt || '';
          var rowBorder = isPending ? 'border-left:3px solid #b8860b;' : (isBanned ? 'border-left:3px solid #dc3545;' : '');
          html += '<div style="margin-bottom:0.25rem;background:var(--panel-bg);border:1px solid var(--border);border-radius:4px;' + rowBorder + '">';
          html += '<div onclick="toggleUserDropdown(\'' + uid + '\')" style="padding:0.5rem;cursor:pointer">';
          if (isPending) {
            html += '<div style="display:flex;align-items:center;justify-content:space-between"><div><strong>' + username + '</strong> <span style="color:#b8860b;font-size:0.85rem">(Pending Approval)</span></div><div style="font-size:0.8rem;color:var(--panel-fg)">&#9660;</div></div>';
            if (user.timestamp) { html += '<div style="font-size:0.8rem;color:var(--muted,#888);margin-top:0.25rem">Requested: ' + (typeof user.timestamp === 'string' ? user.timestamp : formatMillisTimestamp(user.timestamp)) + '</div>'; }
          } else {
            // Line 1: username + role badge + banned badge + session count
            var roleBadge = isAdmin ? '<span style="color:var(--accent);font-size:0.8rem;font-weight:600;background:rgba(102,126,234,0.15);padding:1px 6px;border-radius:3px;margin-left:6px">Admin</span>' : '<span style="color:#a0aec0;font-size:0.8rem;font-weight:600;background:rgba(160,174,192,0.1);padding:1px 6px;border-radius:3px;margin-left:6px">User</span>';
            var bannedBadge = isBanned ? ' <span style="color:#dc3545;font-size:0.8rem;font-weight:600;background:rgba(220,53,69,0.15);padding:1px 6px;border-radius:3px">Banned</span>' : '';
            var sessionBadge = '<span style="color:var(--panel-fg);font-size:0.8rem;margin-left:6px">' + sessionCount + ' session' + (sessionCount !== 1 ? 's' : '') + '</span>';
            html += '<div style="display:flex;align-items:center;justify-content:space-between"><div><strong>' + username + '</strong>' + roleBadge + bannedBadge + sessionBadge + '</div><div style="font-size:0.8rem;color:var(--panel-fg)">&#9660;</div></div>';
            // Line 2: metadata summary — rendered inside the dropdown, not here
          }
          html += '</div>';
          html += '<div id="dropdown-' + uid + '" style="display:none;padding:0.5rem">';
          if (!isPending) {
            var meta = [];
            if (createdAt) meta.push('<strong>Created:</strong> ' + createdAt.replace('T', ' ').replace('Z', ''));
            if (meta.length) html += '<div style="font-size:0.8rem;color:var(--muted,#888);margin-bottom:0.5rem">' + meta.join(' &middot; ') + '</div>';
          }
          if (!isPending && sessionCount > 0) {
            html += '<div style="margin-bottom:0.4rem;font-size:0.9rem;color:var(--panel-fg)"><strong>Active Sessions:</strong></div>';
            html += '<div style="font-size:0.85rem;margin-bottom:0.5rem">';
            userSessions.forEach(function(session) {
              var transport = session.transport || '';
              var label = transport === 'oled' ? 'OLED Display' : transport === 'serial' ? 'Serial' : transport === 'bluetooth' ? 'Bluetooth' : transport === 'g2' ? 'G2 Glasses' : transport ? transport : 'Web';
              var detail = [];
              if (!transport) {
                var ip = cleanIPAddress(session.ip || '');
                var created = session.createdAt ? formatMillisTimestamp(session.createdAt) : '';
                var current = session.current || false;
                if (ip) detail.push('IP: ' + ip + (current ? ' <span style="color:#28a745;font-weight:bold">(Current)</span>' : ''));
                if (created) detail.push('Since: ' + created);
              }
              html += '<div style="padding:0.2rem 0;border-bottom:1px solid var(--border)">';
              html += '<strong>' + label + '</strong>';
              if (detail.length) html += ' <span style="color:var(--muted,#888)">&middot; ' + detail.join(' &middot; ') + '</span>';
              html += '</div>';
            });
            html += '</div>';
          }
          html += '<div style="display:flex;gap:0.5rem;flex-wrap:wrap">';
          if (isPending) {
            // Pending users only get Approve and Deny
            html += '<button class="btn" data-user="' + username + '" onclick="approveUserByName(this.dataset.user)" title="Approve this registration request">Approve</button>';
            html += '<button class="btn" data-user="' + username + '" onclick="denyUserByName(this.dataset.user)" title="Deny and remove this registration request">Deny</button>';
          } else {
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
            if (isBanned) {
              html += '<button class="btn" data-user="' + username + '" onclick="unbanUserByName(this.dataset.user)" title="Remove ban and restore access">Unban</button>';
            } else {
              html += '<button class="btn" data-user="' + username + '" onclick="banUserByName(this.dataset.user)" title="Ban this user from all access">Ban</button>';
            }
          }
          var hasEspNow = !isPending && (__S && __S.features && __S.features.espnow === true);
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
    
    window.promoteUserByName = async function(username) {
      if (!username) {
        alert('Username required');
        return;
      }
      if (!await hwConfirm('Promote user "' + username + '" to admin?')) {
        return;
      }
      var cmd = 'userpromote ' + username;
      postSettingsCli(cmd)
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
    
    window.approveUserByName = async function(username) {
      if (!username) {
        alert('Username required');
        return;
      }
      if (!await hwConfirm('Approve user "' + username + '"?')) {
        return;
      }
      var cmd = 'userapprove ' + username;
      postSettingsCli(cmd)
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
    
    window.denyUserByName = async function(username) {
      if (!username) {
        alert('Username required');
        return;
      }
      if (!await hwConfirm('Deny user "' + username + '"? This will permanently reject their registration.')) {
        return;
      }
      var cmd = 'userdeny ' + username;
      postSettingsCli(cmd)
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
    
    window.demoteUserByName = async function(username) {
      if (!username) {
        alert('Username required');
        return;
      }
      if (!await hwConfirm('Demote admin user "' + username + '" to regular user?')) {
        return;
      }
      var cmd = 'userdemote ' + username;
      postSettingsCli(cmd)
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
    
    window.deleteUserByName = async function(username) {
      if (!username) {
        alert('Username required');
        return;
      }
      // userdelete uses the two-step CLIMode confirm framework (same as
      // filedelete / factoryreset — see System_User.cpp:1720). hw.cliConfirm
      // shows the themed dialog and, on accept, sends [userdelete X, yes] as
      // a batch so the worker's confirm-mode state resolves on the same web
      // session. Target follows the This/Bonded toggle.
      var prompt = 'Delete user "' + username + '"? This action cannot be undone.';
      var target = (window._settingsTarget === 'bonded') ? 'bond' : 'local';
      try {
        var r = await hw.cliConfirm('userdelete ' + username, prompt, { target: target });
        if (r.cancelled) return;
        if (!r.ok) { alert('Error: ' + (r.result || 'no response')); return; }
        alert(r.result);
        try { refreshUsers(); } catch(_) {}
      } catch (e) {
        alert('Error: ' + e.message);
      }
    };
    
    // toggleSerialAuth, toggleBleAuth, toggleDisplayAuth removed - auth settings now in respective module panels

    window.toggleUserSync = function() {
      var current = $('usersync-enabled-value') && $('usersync-enabled-value').textContent === 'Enabled';
      var cmd = current ? 'espnowusersync off' : 'espnowusersync on';
      postSettingsCli(cmd)
      .then(function() {
        var nowEnabled = !current;
        var el = $('usersync-enabled-value');
        if (el) { el.textContent = nowEnabled ? 'Enabled' : 'Disabled'; el.style.color = 'var(--accent)'; }
        var btn = $('usersync-enabled-btn');
        if (btn) btn.textContent = nowEnabled ? 'Disable' : 'Enable';
        var form = $('usersync-form');
        if (form) form.style.display = nowEnabled ? 'block' : 'none';
        if (nowEnabled) { refreshSyncUsers(); refreshSyncPeers(); }
      });
    };
    
    window.refreshSyncUsers = function() {
      postSettingsCli('userlist json')
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
      postSettingsCli('espnowdevices')
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
      var cmd = 'usersync ' + username + ' ' + device + ' ' + adminPass + ' ' + userPass;
      postSettingsCli(cmd)
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
      postSettingsCli('espnowdevices')
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
      var cmd = 'usersync ' + username + ' ' + device + ' ' + adminPass + ' ' + userPass;
      postSettingsCli(cmd)
      .then(function(t) {
        alert(t || 'Sync complete');
        if ($('sync-admin-pass-' + uid)) $('sync-admin-pass-' + uid).value = '';
        if ($('sync-user-pass-' + uid)) $('sync-user-pass-' + uid).value = '';
      })
      .catch(function(e) { alert('Error: ' + e.message); });
    };

    window.resetUserPassword = function(username) {
      if (!username) {
        hwAlert('Username required');
        return;
      }
      openResetPasswordModal(username);
    };
    
  } catch(err) {
    console.error('[SETTINGS] Part 12 ERROR:', err);
    alert('Settings page error (Part 12): ' + err.message);
  }
})();
console.log('[SETTINGS] Part 4: Complete');
console.log('[SETTINGS] All parts loaded successfully');
</script>)SETPART4", HTTPD_RESP_USE_STRLEN);

  // Tail: close settings-local-container, emit settings-bonded-container with
  // the bonded settings panel (curated tz/ntp/loglevel subset), and the toggle
  // JS that swaps containers. The toggle bar at the top of the page stays
  // hidden until /api/bond/status confirms this device is the bonded master.
  //
  // First </div> closes the unclosed text-align refresh wrapper that
  // SETPART8B opens at line 1984 (`<div style='text-align:center...'>`) and
  // never closes — the four trailing <script> blocks live inside it. Second
  // </div> closes settings-local-container itself. Without the first close,
  // settings-bonded-container would land INSIDE settings-local-container, so
  // showBondedSettings() hiding the local container would also hide the
  // bonded panel — invisible bonded view in the UI.
  httpd_resp_send_chunk(req, R"BONDSETTAIL(
</div><!-- /text-align refresh wrapper opened by SETPART8B -->
</div><!-- /settings-local-container -->
<div id='settings-bonded-container' style='display:none'>
  <!-- Slim toolbar row — Re-sync button + status text, no card border. The
       local view goes straight from the Target toggle to the panel list with
       no enclosing "Bonded Device Settings" wrapper; matching that here keeps
       both views structurally consistent and avoids the double-card look. -->
  <div style='display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:8px;margin:1rem 0 0.5rem 0'>
    <span id='bond-settings-status' style='font-size:0.85em;color:var(--muted)'>Click Re-sync to pull fresh settings from the worker</span>
    <button class='btn' id='btn-bond-settings-resync' title='Pull a fresh copy from the worker over ESP-NOW'>Re-sync</button>
  </div>
  <div id='bond-settings-dirty-banner' style='display:none;margin-bottom:0.75rem;padding:0.6rem 0.9rem;background:rgba(255,193,7,0.15);border-left:3px solid #ffc107;border-radius:4px;color:var(--panel-fg);font-size:0.88rem'>
    <strong>Worker settings changed</strong> since you loaded these. Click <em>Re-sync</em> to refresh — saving now would overwrite the worker's new values with the older ones in your form.
  </div>
  <!-- Dynamic host: filled after Re-sync with one collapsible SchemaPanel per
       worker module (renderBondedModules in the script below). The schema is
       fetched from /api/bond/settings/schema and values from /api/bond/settings,
       so panels render whatever the worker actually has compiled in — even
       modules absent from the master's build show up here, and modules missing
       from the worker silently disappear. Per-panel Save buttons route through
       the target-aware sendSequential -> /api/bond/cli/batch. -->
  <div id='bond-settings-content' style='display:none'>
    <div id='bond-modules-host'></div>
  </div>
</div><!-- /settings-bonded-container -->
<script>
(function(){
  // Toggle helpers — mirror the Files/CLI page pattern.
  function setSettingsButtons(local){
    var bl = document.getElementById('settings-btn-local');
    var bb = document.getElementById('settings-btn-bonded');
    if (bl) bl.style.opacity = local ? '1' : '0.55';
    if (bb) bb.style.opacity = local ? '0.55' : '1';
  }
  window.showLocalSettings = function(){
    var lc = document.getElementById('settings-local-container');
    var bc = document.getElementById('settings-bonded-container');
    if (lc) lc.style.display = '';
    if (bc) bc.style.display = 'none';
    setSettingsButtons(true);
    stopDirtyPoll();  // no need to poll while the bonded view isn't visible
    // postSettingsCli / sendSequential route based on this — local view
    // dispatches via /api/cli (writes go to this device).
    window._settingsTarget = 'local';
    console.log('[Settings] target → local');
  };
  window.showBondedSettings = function(){
    var lc = document.getElementById('settings-local-container');
    var bc = document.getElementById('settings-bonded-container');
    if (lc) lc.style.display = 'none';
    if (bc) bc.style.display = '';
    setSettingsButtons(false);
    // From now on every postSettingsCli / sendSequential dispatch (from any
    // panel inside settings-bonded-container, including SchemaPanel.render
    // ones if we ever add them) routes through the bond session to the worker.
    window._settingsTarget = 'bonded';
    console.log('[Settings] target → bonded');
    // Hide form fields every time the user enters the bonded view — the
    // cached values may be stale (e.g. worker edited locally since last
    // sync). User must click Re-sync to acknowledge they want fresh data.
    var content = document.getElementById('bond-settings-content');
    if (content) content.style.display = 'none';
    statusSet('Click Re-sync to pull fresh settings from the worker');
    saveStatusSet('');
    // Reset dirty state — banner and poller restart only after a successful
    // Re-sync repopulates the form (loadFromCache sets formLoadedHash).
    setDirty(false);
    formLoadedHash = null;
    stopDirtyPoll();
  };

  function statusSet(msg){ var s=document.getElementById('bond-settings-status'); if(s) s.textContent=msg; }
  // saveStatusSet kept as a no-op for legacy callers (the old global Save
  // button is gone; each SchemaPanel now shows its own status next to its
  // Save button).
  function saveStatusSet(msg, isErr){ /* per-panel status replaces this */ }
  var bondPeerMac = '';

  // Dirty-detection state. The peer publishes a CRC32 of its settings.json
  // payload in every bond heartbeat. We capture that value at loadFromCache
  // time as formLoadedHash; if subsequent polls show a different value, the
  // worker has changed settings since the form was populated and saving now
  // would overwrite their changes.
  var formLoadedHash = null;
  var dirtyPollTimer = null;
  var isDirty = false;
  function setDirty(d){
    isDirty = !!d;
    var b = document.getElementById('bond-settings-dirty-banner');
    if (b) b.style.display = isDirty ? '' : 'none';
  }
  function checkDirty(){
    if (formLoadedHash === null) return;
    hw.fetchJSON('/api/bond/status')
      .then(function(d){
        if (!d) return;
        if (d.peerSettingsHash !== undefined && d.peerSettingsHash !== formLoadedHash) setDirty(true);
        else setDirty(false);
      })
      .catch(function(){});
  }
  function startDirtyPoll(){ stopDirtyPoll(); dirtyPollTimer = setInterval(checkDirty, 5000); }
  function stopDirtyPoll(){ if (dirtyPollTimer) { clearInterval(dirtyPollTimer); dirtyPollTimer = null; } }

  // Probe /api/bond/status — only reveal the toggle bar for a bonded master.
  hw.fetchJSON('/api/bond/status').then(function(d){
    if(!d || d.role !== 1 || !d.bonded || !d.peerMac) return;
    if(d.peerMac === '00:00:00:00:00:00') return;
    bondPeerMac = d.peerMac;
    var t = document.getElementById('settings-source-toggle');
    if (t) t.style.display = 'flex';
    setSettingsButtons(true);  // Default to "This Device" highlighted
  }).catch(function(){});

  // Modules we don't render in the bonded view. Auth/users have dedicated
  // composite UI on the local page (multi-user lists, password resets) that
  // doesn't map cleanly to SchemaPanel's flat field grid. Bond mode itself
  // excluded because reconfiguring bond over its own session would
  // self-destruct. If a future refactor splits credential-bearing entries
  // out, we can re-add some of these.
  var BONDED_MODULE_DENYLIST = ['users', 'bond'];

  // Per-module key whitelist for the bonded view. Modules listed here render
  // ONLY the keys named — used for partial exposure when a module has both
  // safe-to-edit fields and risky ones. WiFi is the canonical case: tz/NTP
  // are fine to push to the worker, but SSID/password/enabled would cut the
  // network out from under the bond.
  var BONDED_KEY_WHITELIST = {
    wifi: ['ntpServer', 'tzOffsetMinutes']
  };

  // After a successful sync, fetch the worker's schema and render one
  // SchemaPanel per module into bond-modules-host. Each panel renders the
  // entries for its module and wires its own Save button — which routes
  // through sendSequential → /api/bond/cli/batch because
  // window._settingsTarget is 'bonded' while this view is active.
  function renderBondedModules(){
    var host = document.getElementById('bond-modules-host');
    if(!host) return;
    host.innerHTML = '<span style="opacity:0.7">Loading worker schema…</span>';
    hw.fetchJSON('/api/bond/settings/schema')
      .then(function(schema){
        if(!schema || !Array.isArray(schema.modules)){
          host.innerHTML = '<span style="color:var(--danger,#e74c3c)">Worker schema unavailable</span>';
          return;
        }
        var visible = schema.modules.filter(function(m){
          return m && m.name && BONDED_MODULE_DENYLIST.indexOf(m.name) < 0
                                   && Array.isArray(m.entries) && m.entries.length > 0;
        });
        console.log('[Bonded] Worker exposed ' + schema.modules.length + ' modules, rendering ' + visible.length + ' (denylist: ' + BONDED_MODULE_DENYLIST.join(',') + ')');
        host.innerHTML = '';
        visible.forEach(function(m){
          var panelId = 'bond-mod-' + m.name;
          var paneId  = 'bond-pane-' + m.name;
          var btnId   = 'btn-bond-' + m.name + '-toggle';
          var wrap = document.createElement('div');
          wrap.className = 'settings-panel';
          wrap.style.marginBottom = '0.75rem';
          var title = m.description && m.description.length > 0 ? m.description : m.name;
          var connected = (m.connected === undefined) ? '' :
            ' <span style="font-size:0.8em;opacity:0.7;margin-left:0.5rem">[' + (m.connected ? 'connected' : 'disconnected') + ']</span>';
          // Collapsible header + hidden content pane — same pattern as the local
          // settings page's panels (togglePane is defined in EARLYJS at the top
          // of streamSettingsInner). Modules start collapsed so the page is
          // compact by default; user expands the ones they want to edit.
          wrap.innerHTML =
            '<div style="display:flex;align-items:center;justify-content:space-between;gap:0.5rem">' +
              '<div style="font-size:1.05rem;font-weight:bold;color:var(--panel-fg)">' +
                title + connected +
              '</div>' +
              '<button class="btn" id="' + btnId + '" onclick="togglePane(\'' + paneId + '\',\'' + btnId + '\')">Expand</button>' +
            '</div>' +
            '<div id="' + paneId + '" style="display:none;margin-top:0.75rem">' +
              '<div id="' + panelId + '"></div>' +
            '</div>';
          host.appendChild(wrap);
          var panelOpts = {
            containerId: panelId,
            moduleName: m.name,
            target: 'bonded',
            saveLabel: 'Save to Worker',
            logPrefix: 'Bonded:' + m.name
          };
          if (BONDED_KEY_WHITELIST[m.name]) panelOpts.keys = BONDED_KEY_WHITELIST[m.name];
          window.SchemaPanel.render(panelOpts);
        });
        if (visible.length === 0) {
          host.innerHTML = '<span style="opacity:0.7">No editable modules — worker may have non-default build flags.</span>';
        }
      })
      .catch(function(e){ host.innerHTML = '<span style="color:var(--danger,#e74c3c)">Schema fetch failed: ' + e.message + '</span>'; });
  }

  // SchemaPanel calls this after a bonded save → /api/bond/settings/sync. It
  // hands us the sync response so we can update the dirty-detection baseline
  // alongside the cache invalidate. Without this, the form would re-render
  // against fresh values but the dirty banner would think it's still drifted.
  window._bondSyncHook = function(d){
    if (d && d.ok && d.peerSettingsHash !== undefined) formLoadedHash = d.peerSettingsHash;
    setDirty(false);
  };

  // Re-sync button: pull BOTH the worker's values (/api/bond/settings/sync)
  // AND its schema (/api/bond/settings/schema/sync) in parallel, then render.
  // Both endpoints trigger a SETTINGS_REQ / SCHEMA_REQ over bond and poll
  // their respective `received` flags until the file transfer completes.
  // Parallel because the two transfers are independent on the wire and the
  // master/worker can pipeline them — typical wall time is max(2s, 2s)
  // rather than sum.
  var resyncBtn = document.getElementById('btn-bond-settings-resync');
  if(resyncBtn){
    resyncBtn.addEventListener('click', function(){
      resyncBtn.disabled = true;
      statusSet('Syncing values + schema from worker…');
      Promise.all([
        hw.postJSON('/api/bond/settings/sync', {}),
        hw.postJSON('/api/bond/settings/schema/sync', {})
      ]).then(function(results){
        var sd = results[0]; // settings sync result
        var hd = results[1]; // schema sync result
        resyncBtn.disabled = false;
        if(!sd || !sd.ok){ statusSet('Settings sync failed: '+((sd&&sd.error)||'unknown')); return; }
        if(!hd || !hd.ok){ statusSet('Schema sync failed: '+((hd&&hd.error)||'unknown')); return; }
        if (sd.peerSettingsHash !== undefined) formLoadedHash = sd.peerSettingsHash;
        statusSet('Synced (values '+sd.elapsedMs+'ms, schema '+hd.elapsedMs+'ms)');
        // Both file caches are now fresh on disk; invalidate the browser
        // SchemaPanel cache so the next render fetches the new bytes.
        window.SchemaPanelInvalidate('bonded');
        document.getElementById('bond-settings-content').style.display = '';
        setDirty(false);
        startDirtyPoll();
        renderBondedModules();
      }).catch(function(e){ resyncBtn.disabled=false; statusSet('Sync error: '+e.message); });
    });
  }
})();
</script>
)BONDSETTAIL", HTTPD_RESP_USE_STRLEN);
}

// Legacy function removed - now using streamSettingsInner() for efficient streaming
#endif
