#include "System_BuildConfig.h"

#if ENABLE_WEB_POWER

#include <Arduino.h>
#include <ArduinoJson.h>

#include "System_User.h"          // AuthContext
#include "System_MemUtil.h"       // ps_alloc, AllocPref, PSRAM_JSON_DOC
#include "WebPage_Power.h"
#include "WebServer_Server.h"     // streamBeginHtml/EndHtml, streamPageWithContent, WEB_AUTH_OR_RETURN
#include "WebServer_Utils.h"

extern void streamBeginHtml(httpd_req_t* req, const char* title, bool isPublic,
                            const String& username, const String& activePage);

// ===========================================================================
// /api/power/status — live snapshot
// ===========================================================================
// The JSON is built by the single core builder in System_Power.cpp
// (buildPowerJson(JsonDocument&)) so this endpoint and the `power json`
// CLI/BLE command return the IDENTICAL schema — including the `modes` array
// the page uses to render the preset picker, so no MHz value is re-typed in
// JavaScript.
//
// Deliberately a plain GET rather than a wrapper around `power`: executeCommand
// stamps powerSaveNoteActivity() for every non-internal transport, so polling
// this through /api/cli would hold the device out of idle power-save forever —
// the page would break the very feature it reports on.
esp_err_t handlePowerStatus(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  httpd_resp_set_type(req, "application/json");
  // Worst case (widest numbers, longest mode names) serializes to 785 B, and
  // the `modes` table is fixed-size so it cannot grow at runtime. serializeJson
  // silently TRUNCATES to fit rather than failing, so a short buffer would emit
  // invalid JSON the page cannot parse — keep the headroom.
  static char* buf = nullptr;
  static const size_t kBufSize = 1024;
  if (!buf) buf = (char*)ps_alloc(kBufSize, AllocPref::PreferPSRAM, "power.status.json");
  if (!buf) { httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
  PSRAM_JSON_DOC(doc);
  extern void buildPowerJson(JsonDocument& doc);  // core: System_Power.cpp
  buildPowerJson(doc);
  serializeJson(doc, buf, kBufSize);
  httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// ===========================================================================
// /power page
// ===========================================================================
// Every mutation goes through /api/cli, so authorizeCommand applies the same
// admin gate the CLI, OLED and G2 surfaces get — the page never pokes
// gSettings or calls setCpuFrequencyMhz itself. The controls are additionally
// hidden for non-admins so a user who cannot run them isn't handed buttons
// that only ever return "Error:".
static void streamPowerContent(httpd_req_t* req, const String& username) {
  streamBeginHtml(req, "Power", false, username, "power");

  httpd_resp_send_chunk(req, R"HTML(
<div class='card'>
  <h2 style='margin-top:0'>Power</h2>
  <div style='display:flex;flex-wrap:wrap;gap:1.5rem;align-items:baseline'>
    <div><span id='pw-mode' style='font-size:2rem;font-weight:bold'>--</span></div>
    <div><span id='pw-cpu' style='font-size:2rem;font-weight:bold'>--</span><span style='font-size:1.1rem'> MHz</span></div>
    <div id='pw-downclocked' class='text-muted' style='display:none'>idle power-save has downclocked the core</div>
  </div>
  <div class='settings-grid' style='margin-top:1.25rem'>
    <div class='sys-card'>
      <div class='sys-card-row'><span>Active clock</span><strong id='pw-active'>--</strong></div>
      <div class='sys-card-row'><span>Idle clock</span><strong id='pw-idle'>--</strong></div>
      <div class='sys-card-row'><span>XTAL / APB</span><strong id='pw-xtal'>--</strong></div>
    </div>
    <div class='sys-card'>
      <div class='sys-card-row'><span>Auto mode</span><strong id='pw-auto'>--</strong></div>
      <div class='sys-card-row'><span>Battery threshold</span><strong id='pw-threshold'>--</strong></div>
      <div class='sys-card-row'><span>Display brightness</span><strong id='pw-bright'>--</strong></div>
    </div>
    <div class='sys-card'>
      <div class='sys-card-row'><span>Idle power-save</span><strong id='pw-ps'>--</strong></div>
      <div class='sys-card-row'><span>Idle for</span><strong id='pw-idlefor'>--</strong></div>
      <div class='sys-card-row'><span>Sleep cooldown</span><strong id='pw-cooldown'>--</strong></div>
    </div>
  </div>
  <div id='pw-sleepblock' class='alert alert-warning' style='display:none;margin-top:1rem'></div>
  <div id='pw-readonly' class='alert alert-info' style='display:none;margin-top:1rem'>
    Power controls require an admin account. This page is read-only for you.
  </div>
</div>

<div class='card' data-guest-hide id='pw-card-mode'>
  <h3 style='margin-top:0'>CPU &amp; power mode</h3>
  <p class='text-muted text-sm'>Applies immediately and persists. The second figure is the clock idle power-save may sink to once the display blanks.</p>
  <div class='btn-row' id='pw-modes'></div>
  <h4 style='margin:1.25rem 0 .5rem'>Set the clock directly</h4>
  <p class='text-muted text-sm'>One-off override; the next power-mode change or wake replaces it. 80 MHz is the WiFi/PSRAM floor.</p>
  <div class='btn-row'>
    <button class='btn' data-freq='80'>80 MHz</button>
    <button class='btn' data-freq='160'>160 MHz</button>
    <button class='btn' data-freq='240'>240 MHz</button>
  </div>
</div>

<div class='card' data-guest-hide id='pw-card-tuning'>
  <h3 style='margin-top:0'>Idle power-save &amp; timing</h3>
  <div class='settings-grid'>
    <div>
      <label for='pw-in-save'>Power saving (minutes, 0 disables)</label>
      <input type='number' id='pw-in-save' min='0' max='1440' class='input-fit input-m'>
      <div class='text-muted text-sm' id='pw-save-note'>Blanks the display and downclocks after this much idle time. The radio stays up.</div>
      <button class='btn btn-small space-top-sm' id='pw-save-apply'>Apply</button>
    </div>
    <div>
      <label for='pw-in-cooldown'>Sleep cooldown (ms, 0 disables)</label>
      <input type='number' id='pw-in-cooldown' min='0' max='60000' class='input-fit input-m'>
      <div class='text-muted text-sm'>Anti-flap: the minimum gap between light/deep sleep entries.</div>
      <button class='btn btn-small space-top-sm' id='pw-cooldown-apply'>Apply</button>
    </div>
    <div>
      <label for='pw-in-dim'>Display dim level (%)</label>
      <input type='number' id='pw-in-dim' min='0' max='100' class='input-fit input-m'>
      <div class='text-muted text-sm'>Brightness used when the display is dimmed rather than blanked.</div>
      <button class='btn btn-small space-top-sm' id='pw-dim-apply'>Apply</button>
    </div>
    <div>
      <label for='pw-in-threshold'>Battery threshold (%)</label>
      <input type='number' id='pw-in-threshold' min='0' max='100' class='input-fit input-m'>
      <div class='text-muted text-sm'>Auto mode drops to PowerSaver below this charge level.</div>
      <div class='btn-row space-top-sm'>
        <button class='btn btn-small' id='pw-threshold-apply'>Apply</button>
        <button class='btn btn-small' id='pw-auto-toggle'>Auto mode: --</button>
      </div>
    </div>
  </div>
</div>

<div class='card' data-guest-hide id='pw-card-actions'>
  <h3 style='margin-top:0'>Actions</h3>
  <p class='text-muted text-sm'>Each one interrupts the device. You will lose this session until it comes back.</p>
  <div class='btn-row'>
    <button class='btn' id='pw-act-reboot'>Restart</button>
    <button class='btn' id='pw-act-ramflush'>RAM Flush</button>
    <button class='btn' id='pw-act-lightsleep'>Light Sleep</button>
    <button class='btn' id='pw-act-deepsleep' style='color:var(--danger)'>Power Off</button>
  </div>
  <div class='btn-row space-top-sm'>
    <label for='pw-in-sleepsecs' class='text-sm'>Light sleep duration (s)</label>
    <input type='number' id='pw-in-sleepsecs' min='1' max='3600' value='20' class='input-fit'>
  </div>
  <div class='text-muted text-sm space-top-sm'>
    RAM Flush restarts and restores the features that are running right now.
    Power Off is a deep sleep with no wake source &mdash; only the reset button brings it back.
  </div>
  <div id='pw-act-result' class='text-sm space-top-sm'></div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, R"JS(
<script>(function(){
  var admin = hw.isAdmin();
  var last = null;          // most recent status snapshot
  var stopPoll = null;      // poller canceller, so actions can quiet the page
  var editing = {};         // field id -> true while the user is mid-edit

  // /api/cli answers a REFUSED command with 400/403 and the reason in the body.
  // hw.postFormText throws on any non-2xx, which would replace that reason with
  // a bare "HTTP 403" — so read the body off the Response directly and let
  // failed() classify it. hw.postForm still runs the 401 auth_required redirect.
  function cli(cmd){
    return hw.postForm('/api/cli', {cmd: cmd}).then(function(r){ return r.text(); });
  }
  function failed(r){ return !r || /^(?:Error|Failed|Cancelled|\[ERROR\])/i.test(r.trim()); }

  // Run a mutating command, surface a refusal instead of silently doing
  // nothing, then re-read status so the UI shows what actually took effect.
  function apply(cmd){
    return cli(cmd).then(function(r){
      if (failed(r)) { alert(r.trim()); }
      return refresh();
    }).catch(function(e){ alert('Command failed: ' + (e && e.message ? e.message : cmd)); });
  }

  function dur(ms){
    if (ms == null || isNaN(ms)) return '--';
    var s = Math.floor(ms/1000);
    if (s < 60) return s + 's';
    var m = Math.floor(s/60); s = s % 60;
    if (m < 60) return m + 'm ' + s + 's';
    var h = Math.floor(m/60); m = m % 60;
    return h + 'h ' + m + 'm';
  }

  // The preset buttons are built FROM the status payload's `modes` array, not
  // from a hardcoded list — the gPowerModes table in System_Power.cpp stays the
  // only place the clocks are written down.
  var modesSig = null;   // redraw guard — see below
  function renderModes(s){
    var wrap = hw.$('pw-modes');
    if (!wrap || !s.modes) return;
    // The 2 s poll calls this every tick. Rebuilding innerHTML each time tears
    // down and re-creates the buttons under the user's cursor (losing hover,
    // focus, and an in-flight click), so only redraw when the picker actually
    // changed — which is the selected mode, since the table itself is static.
    var sig = s.mode + '|' + s.modes.length;
    if (sig === modesSig) return;
    modesSig = sig;
    var h = '';
    for (var i = 0; i < s.modes.length; i++){
      var m = s.modes[i];
      var mhz = (m.idleMhz < m.activeMhz) ? (m.activeMhz + '/' + m.idleMhz) : ('' + m.activeMhz);
      var cur = (m.index === s.mode);
      h += "<button class='btn' data-mode='" + m.index + "'" +
           (cur ? " style='font-weight:700;border-color:var(--success)'" : "") + ">" +
           (cur ? '✓ ' : '') + m.name + ' ' + mhz + 'MHz</button>';
    }
    wrap.innerHTML = h;
    var btns = wrap.querySelectorAll('button[data-mode]');
    for (var b = 0; b < btns.length; b++){
      hw.on(btns[b], 'click', function(){ apply('power mode ' + this.getAttribute('data-mode')); });
    }
  }

  // Only write a field the user isn't currently typing into — otherwise the
  // 2 s poll would yank the caret out from under them mid-edit.
  function setField(id, val){
    if (editing[id]) return;
    var el = hw.$(id);
    if (el && el.value !== String(val)) el.value = val;
  }

  function applyStatus(s){
    if (!s || s.schema == null) return;
    last = s;
    hw.setText('pw-mode', s.modeName || '--');
    hw.setText('pw-cpu', s.cpuMhz != null ? s.cpuMhz : '--');
    hw.toggle('pw-downclocked', s.cpuMhz != null && s.activeMhz != null && s.cpuMhz < s.activeMhz);

    hw.setText('pw-active', s.activeMhz + ' MHz');
    hw.setText('pw-idle', s.idleMhz + ' MHz');
    hw.setText('pw-xtal', s.xtalMhz + ' / ' + s.apbMhz + ' MHz');

    hw.setText('pw-auto', s.autoMode ? 'On' : 'Off');
    hw.setText('pw-threshold', s.batteryThreshold + '%');
    hw.setText('pw-bright', s.displayBrightness + '/255 (dim ' + s.displayDimLevel + '%)');

    if (!s.powerSaveSupported) {
      hw.setText('pw-ps', 'not supported on this build');
      hw.setText('pw-idlefor', '--');
      hw.setText('pw-save-note', 'This build has no display, so nothing consumes the idle timeout.');
    } else if (s.powerSaveMinutes === 0) {
      hw.setText('pw-ps', 'disabled');
      hw.setText('pw-idlefor', dur(s.idleMsAgo));
    } else {
      hw.setText('pw-ps', s.powerSaveMinutes + ' min');
      hw.setText('pw-idlefor', dur(s.idleMsAgo));
    }
    hw.setText('pw-cooldown', s.cooldownMs === 0 ? 'disabled'
      : (s.cooldownMs + ' ms' + (s.cooldownRemainingMs > 0 ? ' (' + s.cooldownRemainingMs + ' ms to go)' : '')));

    // Say WHY a sleep would be refused right now, rather than letting the user
    // press the button and read a bare error.
    var block = hw.$('pw-sleepblock');
    if (block) {
      if (s.otaProbation) {
        block.textContent = 'Sleep is refused while the new firmware is in OTA health probation.';
        hw.show(block);
      } else if (s.cooldownRemainingMs > 0) {
        block.textContent = 'Sleep is on cooldown for another ' + s.cooldownRemainingMs + ' ms.';
        hw.show(block);
      } else {
        hw.hide(block);
      }
    }

    renderModes(s);
    setField('pw-in-save', s.powerSaveMinutes);
    setField('pw-in-cooldown', s.cooldownMs);
    setField('pw-in-dim', s.displayDimLevel);
    setField('pw-in-threshold', s.batteryThreshold);
    hw.setText('pw-auto-toggle', 'Auto mode: ' + (s.autoMode ? 'On' : 'Off'));
  }

  function refresh(){
    return hw.fetchJSON('/api/power/status').then(applyStatus)
      .catch(function(e){ console.error('[POWER] status', e); });
  }

  // Non-admins keep the live status but lose every control — the commands
  // behind them are admin-gated, so the buttons could only ever fail.
  if (!admin) {
    hw.hide('pw-card-mode');
    hw.hide('pw-card-tuning');
    hw.hide('pw-card-actions');
    hw.show('pw-readonly');
  }

  refresh();
  stopPoll = hw.pollJSON('/api/power/status', 2000, applyStatus);

  if (admin) {
    // --- direct clock override ---
    var freqBtns = document.querySelectorAll('button[data-freq]');
    for (var f = 0; f < freqBtns.length; f++){
      hw.on(freqBtns[f], 'click', function(){ apply('cpufreq ' + this.getAttribute('data-freq')); });
    }

    // --- tuning fields ---
    function bindField(id){
      var el = hw.$(id);
      if (!el) return;
      hw.on(el, 'focus', function(){ editing[id] = true; });
      hw.on(el, 'blur',  function(){ editing[id] = false; });
    }
    bindField('pw-in-save');
    bindField('pw-in-cooldown');
    bindField('pw-in-dim');
    bindField('pw-in-threshold');

    function applyField(id, verb){
      var el = hw.$(id);
      if (!el) return;
      var v = parseInt(el.value, 10);
      if (isNaN(v)) { alert('Enter a number.'); return; }
      editing[id] = false;
      apply(verb + ' ' + v);
    }
    hw.on(hw.$('pw-save-apply'),      'click', function(){ applyField('pw-in-save', 'powersave'); });
    hw.on(hw.$('pw-cooldown-apply'),  'click', function(){ applyField('pw-in-cooldown', 'powercooldown'); });
    hw.on(hw.$('pw-dim-apply'),       'click', function(){ applyField('pw-in-dim', 'powerdim'); });
    hw.on(hw.$('pw-threshold-apply'), 'click', function(){ applyField('pw-in-threshold', 'power threshold'); });
    hw.on(hw.$('pw-auto-toggle'), 'click', function(){
      apply('power auto ' + (last && last.autoMode ? 'off' : 'on'));
    });

    // --- actions ---
    function note(msg){ hw.setText('pw-act-result', msg); }

    // Restart-class actions: rebootDevice() defers the reset ~1 s, so the HTTP
    // response DOES come back. Check it before tearing down the page — a
    // refused command that had already wiped the UI would leave the user
    // waiting for a restart that never happens (same trap the Settings page
    // documents for its Reboot button).
    function restartAction(cmd, prompt, banner){
      return function(){
        hwConfirm(prompt).then(function(ok){
          if (!ok) return;
          note('Sending ' + cmd + '…');
          cli(cmd).then(function(r){
            if (failed(r)) { note(''); alert(r.trim()); return; }
            if (stopPoll) stopPoll();
            document.body.innerHTML = "<div style='text-align:center;padding:4rem;color:var(--panel-fg)'><h2>" +
              banner + "</h2><p>The device is restarting. Please wait and then reconnect.</p></div>";
          }).catch(function(e){ note(''); alert('Error: ' + (e && e.message ? e.message : cmd + ' failed')); });
        });
      };
    }
    hw.on(hw.$('pw-act-reboot'), 'click',
      restartAction('reboot', 'Restart the device now?', 'Rebooting…'));
    hw.on(hw.$('pw-act-ramflush'), 'click',
      restartAction('ramflush', 'RAM flush? The device restarts and comes back with the features that are running now.', 'Flushing RAM…'));

    // Light sleep BLOCKS the command executor for its whole duration, and the
    // web server is single-task — so this request, and every other one, hangs
    // until the device wakes. Say so up front, then poll our way back rather
    // than leaving a dead page.
    hw.on(hw.$('pw-act-lightsleep'), 'click', function(){
      var el = hw.$('pw-in-sleepsecs');
      var secs = parseInt(el ? el.value : '20', 10);
      if (isNaN(secs) || secs < 1 || secs > 3600) { alert('Duration must be 1-3600 seconds.'); return; }
      hwConfirm('Light sleep for ' + secs + 's? The whole web interface stops responding until the device wakes.')
        .then(function(ok){
          if (!ok) return;
          note('Sleeping for ' + secs + 's… the page will catch up on wake.');
          cli('lightsleep ' + secs).then(function(r){
            note(failed(r) ? r.trim() : 'Awake.');
            refresh();
          }).catch(function(){
            // Expected when the browser gives up before the device wakes.
            note('Request dropped while asleep — refreshing.');
            refresh();
          });
        });
    });

    // Deep sleep never returns: esp_deep_sleep_start() cuts the rails, so the
    // request dies mid-flight. A network error here is the SUCCESS path.
    hw.on(hw.$('pw-act-deepsleep'), 'click', function(){
      hwConfirm('Power off? Deep sleep has no wake source — only the physical reset button brings the device back.')
        .then(function(ok){
          if (!ok) return;
          function offScreen(){
            if (stopPoll) stopPoll();
            document.body.innerHTML = "<div style='text-align:center;padding:4rem;color:var(--panel-fg)'><h2>Powered off</h2>" +
              "<p>The device is in deep sleep. Press its reset button to bring it back.</p></div>";
          }
          note('Powering off…');
          cli('deepsleep').then(function(r){
            // A response only comes back if the command was REFUSED (cooldown,
            // OTA probation, permissions) — it never returns on success.
            if (failed(r)) { note(''); alert(r.trim()); return; }
            offScreen();
          }).catch(offScreen);
        });
    });
  }
})();</script>
)JS", HTTPD_RESP_USE_STRLEN);

  streamEndHtml(req);
}

esp_err_t handlePowerPage(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  streamPageWithContent(req, "power", ctx.user, streamPowerContent);
  return ESP_OK;
}

void registerPowerHandlers(httpd_handle_t server) {
  static const httpd_uri_t powerPage = { .uri = "/power", .method = HTTP_GET, .handler = handlePowerPage, .user_ctx = NULL };
  httpd_register_uri_handler(server, &powerPage);
  static const httpd_uri_t powerStatus = { .uri = "/api/power/status", .method = HTTP_GET, .handler = handlePowerStatus, .user_ctx = NULL };
  httpd_register_uri_handler(server, &powerStatus);
}

#endif // ENABLE_WEB_POWER
