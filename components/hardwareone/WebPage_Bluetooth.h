#ifndef WEBPAGE_BLUETOOTH_H
#define WEBPAGE_BLUETOOTH_H

#include <Arduino.h>
#include "WebServer_Utils.h"
#include "System_BuildConfig.h"
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif

// Registration function - registers /bluetooth URI handler
#if ENABLE_HTTP_SERVER
void registerBluetoothHandlers(httpd_handle_t server);
#else
inline void registerBluetoothHandlers(httpd_handle_t) {}
#endif

// Streamed inner content for Bluetooth page
inline void streamBluetoothInner(httpd_req_t* req) {
#if !ENABLE_BLUETOOTH
  httpd_resp_send_chunk(req, R"HTML(
<div style='text-align:center;padding:2rem'>
  <h2 style='color:var(--warning);margin-bottom:1rem'>Bluetooth Disabled</h2>
  <p style='color:var(--panel-fg);margin-bottom:2rem'>
    Bluetooth has been disabled during firmware compilation to save memory and resources.
  </p>
  <div style='background:var(--crumb-bg);padding:1.5rem;border-radius:10px;border:1px solid var(--border);max-width:500px;margin:0 auto;text-align:left'>
    <h3 style='color:var(--panel-fg);margin:0 0 1rem 0;font-size:1rem'>To Enable Bluetooth:</h3>
    <p style='color:var(--panel-fg);font-size:0.9rem;margin:0'>
      Recompile the firmware with <code style='background:rgba(0,0,0,0.1);padding:2px 6px;border-radius:3px'>ENABLE_BLUETOOTH=1</code>
      in your build configuration.
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

  // CSS
  httpd_resp_send_chunk(req, R"CSS(
<style>
.bt-header{background:var(--panel-bg);border:1px solid var(--border);border-radius:10px;margin-bottom:16px;padding:10px 16px}
.bt-header-row{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:8px}
.bt-header-left{display:flex;align-items:center;gap:10px}
.bt-header-title{font-size:1.15em;font-weight:700;color:var(--panel-fg)}
.bt-header-right{display:flex;gap:6px;flex-wrap:wrap}
.bt-header-status{font-family:'Courier New',monospace;font-size:.85em;color:var(--muted);line-height:1.35;margin-top:8px;padding-top:8px;border-top:1px solid var(--border);white-space:pre-line;display:none}
.bt-not-init{display:flex;flex-direction:column;align-items:center;justify-content:center;padding:48px 20px;text-align:center;background:var(--panel-bg);border:1px solid var(--border);border-radius:10px;margin-bottom:16px}
.bt-not-init h3{color:var(--panel-fg);margin-bottom:6px;font-size:1.1em}
.bt-not-init p{color:var(--muted);margin-bottom:20px;max-width:380px;font-size:.9em;line-height:1.5}
.bt-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:16px;margin-bottom:16px}
.bt-card{background:var(--panel-bg);border:1px solid var(--border);border-radius:10px;overflow:hidden}
.bt-card-header{padding:12px 14px;border-bottom:1px solid var(--border);display:flex;align-items:center;justify-content:space-between;gap:8px}
.bt-card-title{font-weight:600;color:var(--panel-fg);font-size:.95em}
.bt-card-body{padding:14px}
.bt-conn-item{display:flex;justify-content:space-between;align-items:flex-start;padding:10px 0;border-bottom:1px solid var(--border)}
.bt-conn-item:last-child{border-bottom:none}
.bt-conn-name{font-weight:600;color:var(--panel-fg);font-size:.9em}
.bt-conn-mac{font-family:'Courier New',monospace;font-size:.8em;color:var(--link)}
.bt-conn-meta{font-size:.8em;color:var(--muted);margin-top:2px}
.bt-conn-actions{display:flex;gap:5px;flex-shrink:0}
.bt-empty{text-align:center;color:var(--muted);padding:24px;font-style:italic;font-size:.9em}
.bt-pill{display:inline-block;padding:2px 8px;border-radius:999px;font-size:.75em;font-weight:600}
.bt-pill.connected{background:rgba(0,200,83,0.15);color:#00c853;border:1px solid rgba(0,200,83,0.3)}
.bt-pill.advertising{background:rgba(100,180,255,0.15);color:#64b4ff;border:1px solid rgba(100,180,255,0.3)}
.bt-pill.idle{background:rgba(120,120,120,0.15);color:var(--muted);border:1px solid var(--border)}
.bt-toggle-row{display:flex;align-items:center;justify-content:space-between;padding:8px 0;border-bottom:1px solid var(--border)}
.bt-toggle-row:last-child{border-bottom:none}
.bt-toggle-label{font-size:.9em;color:var(--panel-fg)}
.bt-toggle-sub{font-size:.78em;color:var(--muted);margin-top:1px}
.bt-stream-grid{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-bottom:12px}
.bt-stream-btn{padding:7px 10px;font-size:.82em;text-align:center}
.bt-stream-btn.active{background:var(--accent);color:var(--panel-bg);border-color:var(--accent)}
.bt-field-row{display:flex;gap:8px;align-items:center;margin-bottom:10px}
.bt-field-row input{flex:1;min-width:0}
.bt-field-row .btn{flex-shrink:0}
.bt-section-label{font-size:.75em;font-weight:600;color:var(--muted);text-transform:uppercase;letter-spacing:.06em;margin-bottom:8px;margin-top:16px}
.bt-section-label:first-child{margin-top:0}
.bt-warn{background:var(--warning-bg,rgba(255,200,0,0.08));border:1px solid var(--warning-border,rgba(255,200,0,0.2));border-left:3px solid var(--warning-accent,#ffd600);color:var(--warning-fg,#c8a000);border-radius:6px;padding:8px 12px;font-size:.82em;margin-top:12px;line-height:1.4}
@media(max-width:700px){
  .bt-grid{grid-template-columns:1fr}
  .bt-stream-grid{grid-template-columns:1fr}
  .bt-header-right{flex-wrap:wrap}
}
</style>
)CSS", HTTPD_RESP_USE_STRLEN);

  // Header
  httpd_resp_send_chunk(req, R"HTML(
<div class='bt-header'>
  <div class='bt-header-row'>
    <div class='bt-header-left'>
      <span class='status-indicator status-disabled' id='bt-status-dot'></span>
      <span class='bt-header-title'>Bluetooth</span>
      <span class='bt-pill idle' id='bt-mode-pill'>Server</span>
      <span class='bt-pill idle' id='bt-state-pill'>--</span>
    </div>
    <div class='bt-header-right'>
)HTML", HTTPD_RESP_USE_STRLEN);

#if ENABLE_G2_GLASSES
  // The mode toggle only makes sense in builds that actually include the G2
  // client — otherwise there's no second role to switch to.
  httpd_resp_send_chunk(req, R"HTML(
      <button class='btn' id='btn-bt-mode' title='Switch BLE role (server phone-peripheral vs. G2 glasses client)'>Mode: Server</button>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif

  httpd_resp_send_chunk(req, R"HTML(
      <button class='btn' id='btn-bt-open'>Open</button>
      <button class='btn' id='btn-bt-close' style='display:none'>Close</button>
      <button class='btn' id='btn-bt-adv' style='display:none'>Advertise</button>
      <button class='btn' id='btn-bt-disconnect' style='display:none'>Disconnect</button>
      <button class='btn' id='btn-bt-refresh' style='background:none;border-color:var(--border);color:var(--muted)'>↻</button>
    </div>
  </div>
  <div class='bt-header-status' id='bt-status-text'></div>
</div>

<!-- Not initialized state (copy swapped by JS based on current mode) -->
<div class='bt-not-init' id='bt-not-init'>
  <h3 id='bt-not-init-title'>Bluetooth is not running</h3>
  <p id='bt-not-init-body'>Open the BLE server to begin advertising and accept connections from clients.</p>
  <button class='btn' id='btn-bt-open2'>Open Bluetooth</button>
</div>

<!-- Initialized panels -->
<div id='bt-panels' style='display:none'>
  <div class='bt-grid'>

    <!-- Connections -->
    <div class='bt-card'>
      <div class='bt-card-header'>
        <span class='bt-card-title'>Connected Clients</span>
        <span id='bt-conn-count' style='font-size:.8em;color:var(--muted)'>0 / 4</span>
      </div>
      <div class='bt-card-body' id='bt-conn-list'>
        <div class='bt-empty'>No active connections</div>
      </div>
    </div>

    <!-- Streaming -->
    <div class='bt-card'>
      <div class='bt-card-header'>
        <span class='bt-card-title'>Streaming</span>
      </div>
      <div class='bt-card-body'>
        <div class='bt-section-label'>Data Streams</div>
        <div class='bt-stream-grid'>
          <button class='btn bt-stream-btn' id='btn-stream-on'>All On</button>
          <button class='btn bt-stream-btn' id='btn-stream-off'>All Off</button>
          <button class='btn bt-stream-btn' id='btn-stream-sensors'>Sensors</button>
          <button class='btn bt-stream-btn' id='btn-stream-system'>System</button>
          <button class='btn bt-stream-btn' id='btn-stream-events'>Events</button>
        </div>
        <div class='bt-section-label'>Intervals (ms)</div>
        <div class='bt-field-row'>
          <input type='number' id='stream-sensor-ms' class='form-control' min='100' value='1000' placeholder='Sensor ms' />
          <input type='number' id='stream-system-ms' class='form-control' min='100' value='5000' placeholder='System ms' />
          <button class='btn' id='btn-stream-interval'>Apply</button>
        </div>
        <div id='bt-stream-status' style='font-size:.82em;color:var(--muted);min-height:1.2em'></div>
      </div>
    </div>

  </div>

  <!-- Configuration -->
  <div class='bt-card'>
    <div class='bt-card-header'>
      <span class='bt-card-title'>Configuration</span>
      <button class='btn' id='btn-bt-loadconfig' style='background:none;border-color:var(--border);color:var(--muted);font-size:.8em;padding:3px 10px'>Load</button>
    </div>
    <div class='bt-card-body'>
      <div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:0 24px'>

        <div>
          <div class='bt-section-label'>Device Name</div>
          <div class='bt-field-row'>
            <input type='text' id='bt-cfg-name' class='form-control' maxlength='30' placeholder='e.g. HardwareOne' />
            <button class='btn' id='btn-bt-savename'>Save</button>
          </div>

          <div class='bt-section-label'>TX Power</div>
          <div class='bt-field-row'>
            <input type='number' id='bt-cfg-txpower' class='form-control' min='0' max='7' placeholder='0–7' style='max-width:90px' />
            <span style='font-size:.82em;color:var(--muted);flex:1'>0 = −12 dBm &nbsp; 7 = +9 dBm</span>
            <button class='btn' id='btn-bt-savetx'>Save</button>
          </div>
        </div>

        <div>
          <div class='bt-section-label'>Options</div>
          <div class='bt-toggle-row'>
            <div>
              <div class='bt-toggle-label'>Auto-Start on Boot</div>
              <div class='bt-toggle-sub'>Open Bluetooth automatically after reboot</div>
            </div>
            <button class='btn bt-stream-btn' id='btn-bt-autostart' style='min-width:52px'>--</button>
          </div>
          <div class='bt-toggle-row'>
            <div>
              <div class='bt-toggle-label'>Require Authentication</div>
              <div class='bt-toggle-sub'>Clients must authenticate before sending commands</div>
            </div>
            <button class='btn bt-stream-btn' id='btn-bt-requireauth' style='min-width:52px'>--</button>
          </div>
        </div>

      </div>
      <div id='bt-cfg-status' style='font-size:.82em;color:var(--muted);margin-top:10px;min-height:1.2em'></div>
    </div>
  </div>

  <div class='bt-warn'>
    <strong>Note:</strong> Bluetooth and Wi-Fi share the 2.4 GHz radio. Throughput and latency may
    degrade when both are active simultaneously.
  </div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);

#if ENABLE_G2_GLASSES
  // -----------------------------------------------------------------------------
  // G2 Glasses (BLE Client) panels
  // -----------------------------------------------------------------------------
  // Mutually exclusive with the server panels above — JS flips visibility based
  // on current mode (gSettings.bleMode / `blemode` CLI). Only compiled in when
  // ENABLE_G2_GLASSES=1, so on slimmer builds this section simply doesn't exist.
  httpd_resp_send_chunk(req, R"HTML(
<div id='bt-g2-panels' style='display:none'>
  <div class='bt-grid'>

    <!-- Temples (L / R connection state) -->
    <div class='bt-card'>
      <div class='bt-card-header'>
        <span class='bt-card-title'>Temples</span>
        <span id='g2-state-summary' style='font-size:.8em;color:var(--muted)'>idle</span>
      </div>
      <div class='bt-card-body' id='g2-temples-body'>
        <!-- Status-only rows: G2 requires both arms connected for display to
             work, so per-arm connect buttons are intentionally omitted. Use
             "Connect Both" / "Disconnect" below. -->
        <div class='bt-conn-item'>
          <div>
            <div class='bt-conn-name'>Left</div>
            <div class='bt-conn-meta'>
              <span id='g2-l-state'>--</span> &nbsp;·&nbsp;
              battery <span id='g2-l-batt'>?</span>% &nbsp;·&nbsp;
              MTU <span id='g2-l-mtu'>--</span>
            </div>
            <div class='bt-conn-meta'>
              tx <span id='g2-l-tx'>0</span> / rx <span id='g2-l-rx'>0</span>
            </div>
          </div>
        </div>
        <div class='bt-conn-item'>
          <div>
            <div class='bt-conn-name'>Right</div>
            <div class='bt-conn-meta'>
              <span id='g2-r-state'>--</span> &nbsp;·&nbsp;
              battery <span id='g2-r-batt'>?</span>% &nbsp;·&nbsp;
              MTU <span id='g2-r-mtu'>--</span>
            </div>
            <div class='bt-conn-meta'>
              tx <span id='g2-r-tx'>0</span> / rx <span id='g2-r-rx'>0</span>
            </div>
          </div>
        </div>

        <div class='bt-section-label' style='margin-top:14px'>Scan &amp; Connect</div>
        <div class='bt-stream-grid'>
          <button class='btn bt-stream-btn' id='btn-g2-scan'>Scan</button>
          <button class='btn bt-stream-btn' id='btn-g2-connect-auto'>Connect Both</button>
          <button class='btn bt-stream-btn' id='btn-g2-disconnect'>Disconnect</button>
          <button class='btn bt-stream-btn' id='btn-g2-battery'>Refresh Battery</button>
        </div>
        <div id='g2-conn-status' style='font-size:.82em;color:var(--muted);min-height:1.2em;margin-top:8px'></div>
        <label style='display:flex;align-items:center;gap:8px;font-size:.85em;margin-top:10px;cursor:pointer;flex-wrap:wrap;line-height:1.3'
               title='When enabled, on boot the device reconnects to the saved temple MACs (no scan / no pairing). Pair once via Connect Both first.'>
          <input type='checkbox' id='cb-g2-autoconnect' style='flex-shrink:0'>
          <span>Auto-reconnect on boot</span>
        </label>
      </div>
    </div>

    <!-- R1 Ring (third BLE peripheral — info-only / Path 1) -->
    <!-- Note: ring card uses the same auto-reconnect label pattern as Temples,
         see id='cb-ring-autoconnect' below for the duplicate. -->
    <div class='bt-card'>
      <div class='bt-card-header'>
        <span class='bt-card-title'>R1 Ring</span>
        <span id='ring-state-summary' style='font-size:.8em;color:var(--muted)'>not scanned</span>
      </div>
      <div class='bt-card-body'>
        <div class='bt-conn-item'>
          <div>
            <div class='bt-conn-name'>Ring</div>
            <div class='bt-conn-meta'>
              <span id='ring-state'>--</span> &nbsp;·&nbsp;
              MTU <span id='ring-mtu'>--</span> &nbsp;·&nbsp;
              rx <span id='ring-rx'>0</span>
            </div>
            <div class='bt-conn-meta'>
              <span id='ring-name'>--</span>
            </div>
            <div class='bt-conn-meta' style='font-family:monospace'>
              <span id='ring-mac'>--</span>
            </div>
          </div>
        </div>
        <div class='bt-section-label' style='margin-top:14px'>Connect</div>
        <div class='bt-stream-grid'>
          <button class='btn bt-stream-btn' id='btn-ring-connect'>Connect</button>
          <button class='btn bt-stream-btn' id='btn-ring-disconnect'>Disconnect</button>
        </div>
        <label style='display:flex;align-items:center;gap:8px;font-size:.85em;margin-top:10px;cursor:pointer;flex-wrap:wrap;line-height:1.3'
               title='When enabled, on boot the device reconnects to the saved ring MAC (no scan). Pair once via Connect first.'>
          <input type='checkbox' id='cb-ring-autoconnect' style='flex-shrink:0'>
          <span>Auto-reconnect on boot</span>
        </label>
        <div style='font-size:.78em;color:var(--muted);margin-top:8px;line-height:1.4'>
          Info-only mode: subscribes to ring notify channel and logs health
          pushes (heart rate, activity). Ring gesture relay to glasses
          needs server-issued pkey auth — not yet implemented.
          Run the Temples <b>Scan</b> first with the ring in range; its
          advert is stashed during the G2 scan.
        </div>
        <div id='ring-conn-status' style='font-size:.82em;color:var(--muted);min-height:1.2em;margin-top:8px'></div>
      </div>
    </div>

    <!-- Display (text output to the glasses) -->
    <div class='bt-card'>
      <div class='bt-card-header'>
        <span class='bt-card-title'>Display</span>
      </div>
      <div class='bt-card-body'>
        <div class='bt-section-label'>Show Text (back pane)</div>
        <div class='bt-field-row'>
          <input type='text' id='g2-text' class='form-control' maxlength='120' placeholder='Text to show on the glasses' />
          <button class='btn' id='btn-g2-show'>Send</button>
        </div>
        <div class='bt-section-label' style='margin-top:14px'>AI Reply (front pane)</div>
        <div class='bt-field-row'>
          <input type='text' id='g2-ai-text' class='form-control' maxlength='250' placeholder='Text to render in the front-pane answer card' />
          <button class='btn' id='btn-g2-ai'>Send</button>
        </div>
        <div style='font-size:.78em;color:var(--muted);margin-top:4px;line-height:1.4'>
          Pushes a card to the front (closer) focal plane via the Even-AI
          subsystem (sid=0x07). Wakes the display from off; auto-dismisses
          after ~10 s. Briefly shows "(host)" as the question label.
        </div>
        <div class='bt-section-label' style='margin-top:14px'>Notify (transient)</div>
        <div class='bt-field-row'>
          <input type='text' id='g2-notify-text' class='form-control' maxlength='120' placeholder='Notification text (auto-clears)' />
          <input type='number' id='g2-notify-secs' class='form-control' style='max-width:70px' min='1' max='60' value='5' title='Seconds before auto-clear' />
          <button class='btn' id='btn-g2-notify'>Notify</button>
        </div>
        <div style='font-size:.78em;color:var(--muted);margin-top:4px;line-height:1.4'>
          Placeholder: uses full-screen text display with an auto-clear timer.
          Real overlay notifications (like the Even app's) require the
          JSON-over-EFS protocol which hasn't been reverse-engineered yet.
        </div>
        <div class='bt-section-label' style='margin-top:14px'>BMP from storage</div>
        <div class='bt-field-row'>
          <input type='text' id='g2-bmp-path' class='form-control' maxlength='180' placeholder='LittleFS path (e.g. /images/test.bmp)' value='/images/test.bmp' />
          <input type='number' id='g2-bmp-bright' class='form-control' style='max-width:74px' min='-100' max='100' value='0' title='Brightness -100..100' />
          <input type='number' id='g2-bmp-contrast' class='form-control' style='max-width:74px' min='-100' max='100' value='0' title='Contrast -100..100' />
          <input type='number' id='g2-bmp-hold' class='form-control' style='max-width:86px' min='0' max='120' value='3' title='Hold seconds before cleanup' />
          <button class='btn' id='btn-g2-bmp'>Show BMP</button>
        </div>
        <div style='font-size:.78em;color:var(--muted);margin-top:4px;line-height:1.4'>
          Loads a BMP from storage and pushes it via the image pipeline.
          Default is LittleFS; use <code>/sd/...</code> to target SD.
        </div>
        <div class='bt-stream-grid' style='grid-template-columns:1fr 1fr 1fr;margin-top:10px'>
          <button class='btn bt-stream-btn' id='btn-g2-clear'>Clear Display</button>
          <button class='btn bt-stream-btn' id='btn-g2-mic-toggle'>Mic: --</button>
          <button class='btn bt-stream-btn' id='btn-g2-reopen' title='Re-open the hijacked Blocks app after an abnormal exit (lens went dark, tapping Blocks in the menu does nothing). Sends Cmd=18 + CREATE so the firmware accepts a fresh container.'>Re-open Hijack</button>
        </div>
        <div id='g2-display-status' style='font-size:.82em;color:var(--muted);min-height:1.2em;margin-top:8px'></div>
      </div>
    </div>

  </div>

  <!-- Session options -->
  <div class='bt-card'>
    <div class='bt-card-header'>
      <span class='bt-card-title'>Session</span>
    </div>
    <div class='bt-card-body'>
      <div class='bt-toggle-row'>
        <div>
          <div class='bt-toggle-label'>Menu Navigation</div>
          <div class='bt-toggle-sub'>Route glasses gestures to the OLED menu (g2nav)</div>
        </div>
        <button class='btn bt-stream-btn' id='btn-g2-nav' style='min-width:52px'>--</button>
      </div>
      <div class='bt-toggle-row'>
        <div>
          <div class='bt-toggle-label'>Verbose Scan Logging</div>
          <div class='bt-toggle-sub'>Log every BLE advert seen while scanning (g2verbose)</div>
        </div>
        <button class='btn bt-stream-btn' id='btn-g2-verbose' style='min-width:52px'>--</button>
      </div>
    </div>
  </div>

  <div class='bt-warn'>
    <strong>Note:</strong> G2 client mode scans the 2.4 GHz band for Even Realities glasses temples
    (advertised names like <code>Even G1_12345_L_*</code>). Server mode is automatically torn down
    when switching to client.
  </div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);
#endif // ENABLE_G2_GLASSES

  // Inject compile-time feature flag so the JS below can branch without
  // duplicating the ENABLE_G2_GLASSES guard for every code path.
#if ENABLE_G2_GLASSES
  httpd_resp_send_chunk(req, "<script>window.__bluetoothG2Enabled=true;</script>", HTTPD_RESP_USE_STRLEN);
#else
  httpd_resp_send_chunk(req, "<script>window.__bluetoothG2Enabled=false;</script>", HTTPD_RESP_USE_STRLEN);
#endif

  // JavaScript
  httpd_resp_send_chunk(req, R"JS(
<script>
(function(){
  // ── helpers ──────────────────────────────────────────────────────────────
  // Client-side queue for /api/cli. The server rate-limits at 1 req
  // per 50 ms and returns 429 on burst; without queuing, page-load
  // alone fires loadMode + loadConfig + refresh (g2status+ringstatus)
  // + g2nav + g2verbose all in parallel, and several get 429ed before
  // the limiter window opens. The queue serialises every cli() call
  // with 80 ms minimum spacing — invisible to the user, eliminates
  // the 429 storm. Per-call .then/.catch semantics are preserved.
  var _cliQueue = [];
  var _cliBusy  = false;
  var _cliMinGapMs = 80;
  var _cliLastDispatchedAt = 0;

  function _cliDispatch(){
    if (_cliBusy) return;
    if (_cliQueue.length === 0) return;
    var now = Date.now();
    var elapsed = now - _cliLastDispatchedAt;
    if (elapsed < _cliMinGapMs) {
      setTimeout(_cliDispatch, _cliMinGapMs - elapsed);
      return;
    }
    _cliBusy = true;
    _cliLastDispatchedAt = now;
    var job = _cliQueue.shift();
    fetch('/api/cli',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      credentials:'same-origin',
      body:'cmd='+encodeURIComponent(job.cmd)
    }).then(function(r){
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.text();
    }).then(function(text){
      job.resolve(text);
    }).catch(function(err){
      job.reject(err);
    }).then(function(){
      _cliBusy = false;
      _cliDispatch();
    });
  }

  function cli(cmd){
    return new Promise(function(resolve, reject){
      _cliQueue.push({cmd:cmd, resolve:resolve, reject:reject});
      _cliDispatch();
    });
  }

  function el(id){ return document.getElementById(id); }
  function setText(id,v){ var e=el(id); if(e) e.textContent=v||''; }

  // ── state ─────────────────────────────────────────────────────────────────
  var bleMode = 'server';  // 'server' | 'client' — tracks gSettings.bleMode
  var btState = 'unknown'; // 'off' | 'advertising' | 'connected' | 'on' (server) or 'on' (client)
  var autoStartState = null;
  var requireAuthState = null;
  var navState = null;
  var verboseState = null;
  var micState = false;

  var G2_ENABLED = !!window.__bluetoothG2Enabled;

  // ── UI helpers ────────────────────────────────────────────────────────────
  function updateToggle(btnId, enabled){
    var btn = el(btnId);
    if(!btn) return;
    btn.textContent = enabled ? 'On' : 'Off';
    btn.className = 'btn bt-stream-btn' + (enabled ? ' active' : '');
  }

  function applyMode(mode){
    bleMode = (mode === 'client') ? 'client' : 'server';

    var modePill = el('bt-mode-pill');
    if(modePill){
      modePill.textContent = (bleMode === 'client') ? 'G2 Client' : 'Server';
    }
    var modeBtn = el('btn-bt-mode');
    if(modeBtn){
      modeBtn.textContent = 'Mode: ' + (bleMode === 'client' ? 'G2 Client' : 'Server');
    }

    // Swap the not-init copy to match the mode the user is about to act on.
    var nit = el('bt-not-init-title');
    var nib = el('bt-not-init-body');
    var ob2 = el('btn-bt-open2');
    if(bleMode === 'client'){
      if(nit) nit.textContent = 'G2 client is not running';
      if(nib) nib.textContent = 'Initialize the BLE central stack to scan for and connect to Even Realities G2 glasses.';
      if(ob2) ob2.textContent = 'Initialize G2';
    } else {
      if(nit) nit.textContent = 'Bluetooth is not running';
      if(nib) nib.textContent = 'Open the BLE server to begin advertising and accept connections from clients.';
      if(ob2) ob2.textContent = 'Open Bluetooth';
    }

    // The Advertise button is server-only; hide it up-front in client mode
    // (applyState() re-evaluates visibility after each refresh).
    var advBtn = el('btn-bt-adv');
    if(advBtn && bleMode === 'client') advBtn.style.display = 'none';
    var discBtn = el('btn-bt-disconnect');
    if(discBtn && bleMode === 'client') discBtn.style.display = 'none';

    // Force a fresh status read for the new mode.
    refresh();
  }

  function applyState(state){
    btState = state;
    var dot = el('bt-status-dot');
    var pill = el('bt-state-pill');
    var isOn = (state === 'advertising' || state === 'connected' || state === 'on');

    if(dot) dot.className = 'status-indicator ' + (isOn ? 'status-enabled' : 'status-disabled');

    if(pill){
      pill.className = 'bt-pill ' + (state === 'connected' ? 'connected' : state === 'advertising' ? 'advertising' : 'idle');
      pill.textContent = state === 'off' ? 'Off' :
                         state === 'advertising' ? 'Advertising' :
                         state === 'connected'   ? 'Connected' :
                         state === 'on'          ? 'Running' : '--';
    }

    // Panel visibility: mutually exclusive, and the "not running" card shows
    // only when the selected mode's stack is down.
    var srvPanels = el('bt-panels');
    var g2Panels  = el('bt-g2-panels');
    var notInit   = el('bt-not-init');
    if(bleMode === 'server'){
      if(srvPanels) srvPanels.style.display = isOn ? '' : 'none';
      if(g2Panels)  g2Panels.style.display  = 'none';
    } else {
      if(srvPanels) srvPanels.style.display = 'none';
      if(g2Panels)  g2Panels.style.display  = isOn ? '' : 'none';
    }
    if(notInit) notInit.style.display = isOn ? 'none' : 'flex';

    var openBtn  = el('btn-bt-open');
    var closeBtn = el('btn-bt-close');
    var advBtn   = el('btn-bt-adv');
    var discBtn  = el('btn-bt-disconnect');
    if(openBtn)  openBtn.style.display  = isOn ? 'none' : '';
    if(closeBtn) closeBtn.style.display = isOn ? ''     : 'none';
    // Advertise / Disconnect-client are server-only.
    if(advBtn)   advBtn.style.display   = (bleMode === 'server' && isOn && state !== 'connected') ? '' : 'none';
    if(discBtn)  discBtn.style.display  = (bleMode === 'server' && state === 'connected') ? '' : 'none';
  }

  function parseServerState(text){
    var lower = (text || '').toLowerCase();
    if(lower.indexOf('not initialized') >= 0) return 'off';
    if(lower.indexOf('connected') >= 0) return 'connected';
    if(lower.indexOf('advertising') >= 0) return 'advertising';
    if(lower.indexOf('disabled') >= 0 || lower.indexOf('stopped') >= 0) return 'off';
    if(lower.indexOf('ble status') >= 0) return 'on';
    return 'unknown';
  }

  // Parse the single-line format produced by getG2Status():
  //   state=<name> L=up|down R=up|down mtu=L244/R244 batt=L82/R75 tx=L12/R9 rx=L3/R1
  function parseG2Status(text){
    var lower = (text || '').toLowerCase();
    if(lower.indexOf('state=') < 0) {
      return { running: false, state: 'off' };
    }
    function grab(re, def){ var m = text.match(re); return m ? m[1] : def; }

    return {
      running: true,
      state:   (grab(/state=(\w+)/i, 'idle') || 'idle').toLowerCase(),
      lUp:     /L=up/i.test(text),
      rUp:     /R=up/i.test(text),
      mtuL:    grab(/mtu=L(\d+)\/R\d+/i, '--'),
      mtuR:    grab(/mtu=L\d+\/R(\d+)/i, '--'),
      battL:   grab(/batt=L(-?\d+)\/R-?\d+/i, '?'),
      battR:   grab(/batt=L-?\d+\/R(-?\d+)/i, '?'),
      txL:     grab(/tx=L(\d+)\/R\d+/i, '0'),
      txR:     grab(/tx=L\d+\/R(\d+)/i, '0'),
      rxL:     grab(/rx=L(\d+)\/R\d+/i, '0'),
      rxR:     grab(/rx=L\d+\/R(\d+)/i, '0')
    };
  }

  function renderG2(status){
    setText('g2-state-summary', 'state: ' + (status.state || '--'));
    function setSide(prefix, up, batt, mtu, tx, rx){
      var stEl = el('g2-' + prefix + '-state');
      if(stEl){
        stEl.textContent = up ? 'connected' : 'idle';
        stEl.style.color = up ? 'var(--success,#00c853)' : 'var(--muted)';
      }
      setText('g2-' + prefix + '-batt', (batt === '?' || batt === '-1') ? '?' : batt);
      setText('g2-' + prefix + '-mtu',  mtu);
      setText('g2-' + prefix + '-tx',   tx);
      setText('g2-' + prefix + '-rx',   rx);
    }
    setSide('l', status.lUp, status.battL, status.mtuL, status.txL, status.rxL);
    setSide('r', status.rUp, status.battR, status.mtuR, status.txR, status.rxR);
  }

  // R1 ring status parser — mirrors the `ringstatus` CLI output format
  // (Optional_EvenG2_Ring.cpp::g2RingGetStatus). Example line:
  //   ring=up name='EVEN R1_BAAC1C' addr=f8:29:ca:ba:ac:1c mtu=64 rx=12 up=5.230s (scan=found)
  function parseRingStatus(text){
    if(!text) return { up:false, name:'--', addr:'--', mtu:'--', rx:'0', scan:'not-found' };
    function grab(re, def){ var m = text.match(re); return m ? m[1] : def; }
    return {
      up:   /ring=up/i.test(text),
      name: grab(/name='([^']*)'/, '--'),
      addr: grab(/addr=([0-9a-f:]{17})/i, '--'),
      mtu:  grab(/mtu=(\d+)/i, '--'),
      rx:   grab(/rx=(\d+)/i, '0'),
      scan: grab(/scan=([\w-]+)/i, '--')
    };
  }

  function renderRing(status){
    var stEl = el('ring-state');
    if(stEl){
      stEl.textContent = status.up ? 'connected'
                                    : (status.scan === 'found' ? 'found, idle' : 'not connected');
      stEl.style.color = status.up ? 'var(--success,#00c853)' : 'var(--muted)';
    }
    setText('ring-state-summary',
            status.up ? 'connected'
                      : (status.scan === 'found' ? 'found — click Connect' : 'not scanned'));
    setText('ring-name', status.name);
    setText('ring-mac',  status.addr);
    setText('ring-mtu',  status.mtu);
    setText('ring-rx',   status.rx);

    // Clear the per-action conn-status line once the ring actually
    // reaches the steady state. The Connect button writes a transient
    // "RING: connect task started — use ringstatus to watch" line into
    // #ring-conn-status; once we see the ring as `connected up` (or
    // see it firmly `down`), that message is stale — clear it so the
    // card stops showing irrelevant past state. Disconnect path
    // writes "Disconnected" and we clear that on the next confirmed
    // down/idle tick.
    var cs = el('ring-conn-status');
    if (cs && cs.textContent) {
      var t = cs.textContent;
      var stale =
        // Clear when connected (any prior connect-task message is moot)
        status.up ||
        // Clear when firmly disconnected and the message was a connect-task line
        (!status.up && /connect\s*task\s*started/i.test(t));
      if (stale) cs.textContent = '';
    }
  }

  function renderConnections(text){
    var list = el('bt-conn-list');
    if(!list) return;

    var lines = (text || '').split('\n');
    // Parse "Active connections: X/Y"
    var countMatch = text.match(/Active connections:\s*(\d+)\/(\d+)/);
    if(countMatch) setText('bt-conn-count', countMatch[1] + ' / ' + countMatch[2]);

    var slots = [];
    for(var i = 0; i < lines.length; i++){
      var slotMatch = lines[i].match(/^\[(\d+)\]\s+(.+)/);
      if(slotMatch){
        var details = lines[i+1] || '';
        var macMatch  = details.match(/MAC:\s*([0-9A-Fa-f:]+)/);
        var durMatch  = details.match(/(\d+)\s*sec/);
        var cmdMatch  = details.match(/(\d+)\s*cmds/);
        slots.push({
          slot: slotMatch[1],
          name: slotMatch[2].trim(),
          mac:  macMatch  ? macMatch[1]  : '--',
          dur:  durMatch  ? durMatch[1]  + 's' : '--',
          cmds: cmdMatch  ? cmdMatch[1]  : '0'
        });
      }
    }

    if(slots.length === 0){
      list.innerHTML = '<div class="bt-empty">No active connections</div>';
      return;
    }

    var html = '';
    slots.forEach(function(s){
      html += '<div class="bt-conn-item">'
        + '<div>'
        + '<div class="bt-conn-name">' + s.name + '</div>'
        + '<div class="bt-conn-mac">'  + s.mac  + '</div>'
        + '<div class="bt-conn-meta">Slot ' + s.slot + ' &nbsp;·&nbsp; ' + s.dur + ' &nbsp;·&nbsp; ' + s.cmds + ' cmds</div>'
        + '</div>'
        + '<div class="bt-conn-actions">'
        + '<button class="btn" style="font-size:.78em;padding:3px 8px" onclick="btDisconnect()">Kick</button>'
        + '</div>'
        + '</div>';
    });
    list.innerHTML = html;
  }

  function showStatusText(text){
    var box = el('bt-status-text');
    if(!box) return;
    if(text && text.trim()){
      box.style.display = 'block';
      box.textContent = text;
    } else {
      box.style.display = 'none';
    }
  }

  // ── refresh ───────────────────────────────────────────────────────────────
  // Issues sequential CLI calls to avoid the server's 50 ms /api/cli
  // rate limiter — firing g2status + ringstatus in parallel had them
  // arrive within ~1 ms and the second got 429'd, which the page used
  // to render as "G2 client off" until the next tick succeeded.
  // Sequencing keeps the requests >50 ms apart in the worst case.
  //
  // On any HTTP error (including 429), we deliberately do NOT flip the
  // state to 'off' — we just leave whatever was last rendered alone.
  // True "off" comes through a successful response with parsed.running
  // === false; transient errors shouldn't make the UI lie.
  function refresh(){
    if(bleMode === 'client' && G2_ENABLED){
      cli('g2status').then(function(out){
        var parsed = parseG2Status(out);
        if(!parsed.running){
          applyState('off');
          showStatusText('');
          return;
        }
        // When a temple is up, treat the whole client as "connected" so the
        // header pill matches the UX from ESP-NOW / server mode. Otherwise
        // initialized-but-idle registers as "on" (green dot, "Running" pill).
        applyState((parsed.lUp || parsed.rUp) ? 'connected' : 'on');
        renderG2(parsed);
        showStatusText(out);
      }).catch(function(){
        // Transient failure (network blip, rate-limit, etc.) — don't
        // overwrite the last known state. The next tick will recover.
      }).then(function(){
        // Chain ringstatus AFTER g2status resolves. Sequential rather
        // than parallel keeps both requests clear of the 50 ms /api/cli
        // rate limiter the server enforces. .then() runs regardless of
        // whether the first call succeeded or failed (we caught above).
        return cli('ringstatus');
      }).then(function(out){
        renderRing(parseRingStatus(out));
      }).catch(function(){
        // Same policy: silent on transient failures. The card just
        // doesn't update this tick.
      });
    } else {
      cli('blestatus').then(function(out){
        var state = parseServerState(out);
        applyState(state);
        if(state !== 'off') {
          renderConnections(out);
          showStatusText(out);
        } else {
          showStatusText('');
        }
      }).catch(function(){
        // Don't flip to off on transient errors — leave previous state.
      });
    }
  }

  // ── mode load ─────────────────────────────────────────────────────────────
  function loadMode(){
    if(!G2_ENABLED){
      applyMode('server');   // No G2 compiled in — there's nothing to switch to
      return;
    }
    cli('blemode').then(function(out){
      var isClient = (out || '').toLowerCase().indexOf('client') >= 0;
      applyMode(isClient ? 'client' : 'server');
    }).catch(function(){ applyMode('server'); });
  }

  // ── config load (server mode) ─────────────────────────────────────────────
  function loadConfig(){
    setText('bt-cfg-status', 'Loading…');
    cli('bleinfo').then(function(out){
      var nameMatch = out.match(/Device Name:\s*(.+)/);
      if(nameMatch){ var inp = el('bt-cfg-name'); if(inp) inp.value = nameMatch[1].trim(); }

      var txMatch = out.match(/TX Power:\s*(\d+)/);
      if(txMatch){ var inp = el('bt-cfg-txpower'); if(inp) inp.value = txMatch[1]; }

      autoStartState   = /Auto-Start:\s*Yes/i.test(out);
      requireAuthState = /Require Auth:\s*Yes/i.test(out);
      updateToggle('btn-bt-autostart',   autoStartState);
      updateToggle('btn-bt-requireauth', requireAuthState);

      setText('bt-cfg-status', 'Loaded at ' + new Date().toLocaleTimeString());
    }).catch(function(){
      setText('bt-cfg-status', 'Failed to load config');
    });
  }

  // ── actions ───────────────────────────────────────────────────────────────
  window.btDisconnect = function(){
    cli('bledisconnect').then(function(){ setTimeout(refresh, 400); });
  };

  function runAndRefresh(cmd, statusEl, msg){
    if(statusEl) setText(statusEl, msg || 'Running…');
    cli(cmd).then(function(out){
      if(statusEl) setText(statusEl, out || 'Done');
      setTimeout(refresh, 400);
    }).catch(function(e){
      if(statusEl) setText(statusEl, 'Error: ' + e.message);
    });
  }

  // Mode-aware open / close dispatchers
  function openCmd(){  return (bleMode === 'client') ? 'g2init'   : 'openble';  }
  function closeCmd(){ return (bleMode === 'client') ? 'g2deinit' : 'closeble'; }

  function bind(id, fn){
    var e = el(id); if(e) e.addEventListener('click', fn);
  }

  // ── event bindings ────────────────────────────────────────────────────────
  document.addEventListener('DOMContentLoaded', function(){

    // Header buttons
    bind('btn-bt-open',       function(){ runAndRefresh(openCmd(),  null, null); });
    bind('btn-bt-open2',      function(){ runAndRefresh(openCmd(),  null, null); });
    bind('btn-bt-close',      function(){ runAndRefresh(closeCmd(), null, null); });
    bind('btn-bt-adv',        function(){ cli('bleadv').then(function(out){ showStatusText(out); }); });
    bind('btn-bt-disconnect', function(){ cli('bledisconnect').then(function(){ setTimeout(refresh,400); }); });
    bind('btn-bt-refresh',    function(){ refresh(); });

    // Mode toggle (only present when G2 is compiled in)
    bind('btn-bt-mode', function(){
      var next = (bleMode === 'server') ? 'client' : 'server';
      var btn = el('btn-bt-mode');
      if(btn){ btn.disabled = true; btn.textContent = 'Switching…'; }
      cli('blemode ' + next).then(function(out){
        showStatusText(out);
        applyMode(next);
      }).catch(function(e){
        showStatusText('Mode switch failed: ' + e.message);
      }).then(function(){
        if(btn) btn.disabled = false;
      });
    });

    // Streaming (server mode only, but binding is harmless when panels hidden)
    bind('btn-stream-on',      function(){ cli('blestream on').then(function(o){ setText('bt-stream-status', o); }); });
    bind('btn-stream-off',     function(){ cli('blestream off').then(function(o){ setText('bt-stream-status', o); }); });
    bind('btn-stream-sensors', function(){ cli('blestream sensors').then(function(o){ setText('bt-stream-status', o); }); });
    bind('btn-stream-system',  function(){ cli('blestream system').then(function(o){ setText('bt-stream-status', o); }); });
    bind('btn-stream-events',  function(){ cli('blestream events').then(function(o){ setText('bt-stream-status', o); }); });
    bind('btn-stream-interval', function(){
      var sm = el('stream-sensor-ms'); var sy = el('stream-system-ms');
      var s = sm ? (sm.value || '1000') : '1000';
      var y = sy ? (sy.value || '5000') : '5000';
      cli('blestream interval ' + s + ' ' + y).then(function(o){ setText('bt-stream-status', o); });
    });

    // Config
    bind('btn-bt-loadconfig', function(){ loadConfig(); });
    bind('btn-bt-savename', function(){
      var inp = el('bt-cfg-name'); if(!inp) return;
      cli('blename ' + inp.value.trim()).then(function(o){ setText('bt-cfg-status', o); });
    });
    bind('btn-bt-savetx', function(){
      var inp = el('bt-cfg-txpower'); if(!inp) return;
      cli('bletxpower ' + inp.value).then(function(o){ setText('bt-cfg-status', o); });
    });
    bind('btn-bt-autostart', function(){
      autoStartState = !autoStartState;
      updateToggle('btn-bt-autostart', autoStartState);
      cli('bleautostart ' + (autoStartState ? 'on' : 'off')).then(function(o){ setText('bt-cfg-status', o); });
    });
    bind('btn-bt-requireauth', function(){
      requireAuthState = !requireAuthState;
      updateToggle('btn-bt-requireauth', requireAuthState);
      cli('blerequireauth ' + (requireAuthState ? 'on' : 'off')).then(function(o){ setText('bt-cfg-status', o); });
    });

    // G2 actions — only bound when the G2 panel is in the DOM
    if(G2_ENABLED){
      bind('btn-g2-scan',         function(){ runAndRefresh('g2scan',      'g2-conn-status', 'Scanning…'); });
      bind('btn-g2-connect-auto', function(){ runAndRefresh('openg2 auto', 'g2-conn-status', 'Connecting both…'); });
      bind('btn-g2-disconnect',   function(){ runAndRefresh('closeg2',     'g2-conn-status', 'Disconnecting…'); });
      bind('btn-g2-battery',      function(){
        // g2battery only *kicks* an async request — the actual %-values arrive
        // in BLE notifications some hundreds of ms later. Poll g2status a few
        // times so the UI picks up the update whenever it lands.
        cli('g2battery').then(function(o){ setText('g2-conn-status', o); });
        setTimeout(refresh, 500);
        setTimeout(refresh, 1500);
        setTimeout(refresh, 3000);
      });

      bind('btn-g2-show', function(){
        var inp = el('g2-text'); if(!inp) return;
        var txt = (inp.value || '').trim();
        if(!txt){ setText('g2-display-status', 'Enter text to send'); return; }
        cli('g2show ' + txt).then(function(o){ setText('g2-display-status', o); });
      });
      bind('btn-g2-ai', function(){
        var inp = el('g2-ai-text'); if(!inp) return;
        var txt = (inp.value || '').trim();
        if(!txt){ setText('g2-display-status', 'Enter text to send'); return; }
        cli('g2ai ' + txt).then(function(o){ setText('g2-display-status', o); });
      });
      bind('btn-g2-notify', function(){
        var inp  = el('g2-notify-text');
        var secs = el('g2-notify-secs');
        if(!inp) return;
        var txt = (inp.value || '').trim();
        if(!txt){ setText('g2-display-status', 'Enter notification text'); return; }
        var s = (secs && secs.value) ? parseInt(secs.value, 10) : 5;
        if(!(s > 0 && s <= 60)) s = 5;
        // g2notify CLI parses a leading integer as duration seconds, then
        // the remaining tokens as the body text. No quoting issues because
        // the shell doesn't re-parse — cmd_g2notify takes the raw args.
        cli('g2notify ' + s + ' ' + txt).then(function(o){
          setText('g2-display-status', o);
        });
      });
      bind('btn-g2-bmp', function(){
        var inp = el('g2-bmp-path');
        var bri = el('g2-bmp-bright');
        var con = el('g2-bmp-contrast');
        var hld = el('g2-bmp-hold');
        if(!inp) return;
        var p = (inp.value || '').trim();
        if(!p){ setText('g2-display-status', 'Enter a BMP path'); return; }
        var b = bri ? parseInt(bri.value || '0', 10) : 0;
        var c = con ? parseInt(con.value || '0', 10) : 0;
        var h = hld ? parseInt(hld.value || '3', 10) : 3;
        if(!(b >= -100 && b <= 100)) b = 0;
        if(!(c >= -100 && c <= 100)) c = 0;
        if(!(h >= 0 && h <= 120)) h = 3;
        cli('g2bmp ' + p + ' ' + b + ' ' + c + ' ' + h).then(function(o){ setText('g2-display-status', o); });
      });
      bind('btn-g2-clear', function(){
        cli('g2clear').then(function(o){ setText('g2-display-status', o); });
      });
      bind('btn-g2-reopen', function(){
        cli('g2reopen').then(function(o){ setText('g2-display-status', o); });
      });

      // R1 ring controls. Connect spawns a background task server-side so
      // the response comes back fast — the actual connect takes 1-3 s and
      // the periodic refresh() picks up the state change. Disconnect is
      // immediate. Both paths route their status line into
      // #ring-conn-status so the card has a per-action feedback spot
      // independent of the Temples card's status.
      bind('btn-ring-connect', function(){
        setText('ring-conn-status', 'Connecting...');
        cli('ringconnect').then(function(o){
          setText('ring-conn-status', o);
          // Ring BLE connect + service discovery + notify subscribe is
          // observed at 5-9 seconds total on this stack. The 1.5 s poll
          // tick will eventually catch the result on its own, but we
          // nudge a few extra refreshes here so the UI surfaces the
          // outcome the moment it's available rather than waiting up
          // to a full poll period after the connect actually completes.
          setTimeout(refresh,  1000);
          setTimeout(refresh,  4000);
          setTimeout(refresh,  8000);
          setTimeout(refresh, 15000);
        });
      });
      bind('btn-ring-disconnect', function(){
        cli('ringdisconnect').then(function(o){
          setText('ring-conn-status', o);
          setTimeout(refresh, 500);
        });
      });

      // Auto-reconnect-at-boot toggles. Fire-and-forget — settings are
      // persisted to flash by the CLI handler. Initial state is pulled
      // by sniffing the no-arg form of each command (which returns
      // "enabled" / "disabled" in the output text).
      function bindAutoToggle(cbId, cmd) {
        var cb = el(cbId); if (!cb) return;
        cb.addEventListener('change', function() {
          cli(cmd + ' ' + (cb.checked ? 'on' : 'off'));
        });
        // Pull current state once at page load.
        cli(cmd).then(function(o) {
          cb.checked = /enabled/i.test(o || '');
        });
      }
      // Use the generic `bleautoconnect <peer-name>` form. The per-peer
      // shims (g2autoconnect, ringautoconnect) still work but the generic
      // form is the canonical name and doesn't need a per-peer alias.
      bindAutoToggle('cb-g2-autoconnect',   'bleautoconnect g2-glasses');
      bindAutoToggle('cb-ring-autoconnect', 'bleautoconnect r1-ring');
      bind('btn-g2-mic-toggle', function(){
        micState = !micState;
        var btn = el('btn-g2-mic-toggle');
        if(btn) btn.textContent = 'Mic: ' + (micState ? 'On' : 'Off');
        cli('g2mic ' + (micState ? 'on' : 'off')).then(function(o){ setText('g2-display-status', o); });
      });

      bind('btn-g2-nav', function(){
        // g2nav with no arg toggles — we mirror the state locally and let the
        // response text confirm.
        navState = !navState;
        updateToggle('btn-g2-nav', navState);
        cli('g2nav ' + (navState ? 'on' : 'off')).then(function(o){
          navState = /on\b/i.test(o);
          updateToggle('btn-g2-nav', navState);
        });
      });
      bind('btn-g2-verbose', function(){
        verboseState = !verboseState;
        updateToggle('btn-g2-verbose', verboseState);
        cli('g2verbose ' + (verboseState ? 'on' : 'off')).then(function(o){
          verboseState = /on\b/i.test(o);
          updateToggle('btn-g2-verbose', verboseState);
        });
      });
    }

    // Initial load — determine mode first, which triggers refresh(); then pull
    // the saved server config so toggles show their real state regardless of
    // which mode we end up in.
    loadMode();
    loadConfig();

    // G2 Session toggles — query their current state so the buttons show
    // On/Off on page load instead of "--". Bare command = report state
    // (no flip). Chained sequentially because /api/cli is rate-limited
    // at 1 req per 50 ms and returns 429 on burst — firing both in
    // parallel made the second one race-lose, leaving its button stuck
    // at "--".
    if(window.__bluetoothG2Enabled){
      cli('g2nav').then(function(o){
        navState = /on\b/i.test(o);
        updateToggle('btn-g2-nav', navState);
        return cli('g2verbose');
      }).then(function(o){
        verboseState = /on\b/i.test(o);
        updateToggle('btn-g2-verbose', verboseState);
      }).catch(function(e){
        console.warn('[bluetooth] G2 toggle state fetch failed:', e);
      });
    }

    // Adaptive polling. Mirrors WebPage_Sensors' "fetch every tick to stay
    // in sync" model, but with a two-speed cadence so the serial log
    // (and BLE/WiFi work queues) don't get hammered while nothing is
    // happening:
    //
    //   FAST  (1500 ms) — when something is actively in motion: a
    //                     temple is down, in 'dead' state, or a state
    //                     transition just happened. Surfaces tap /
    //                     connect / disconnect feedback within 1-2 s.
    //   SLOW (15000 ms) — when both temples are 'up' and we've seen
    //                     three consecutive stable ticks. Drops the
    //                     CLI load by 10× during the long boring
    //                     stretches (which is most of the time during
    //                     normal use). The next state change kicks
    //                     back into FAST instantly.
    //
    // Why not pure SSE: the EventSource handle wedged in readyState=0
    // (CONNECTING) across WiFi-reconnect cycles. Polling has no long-
    // lived connection to break, so a transient network blip costs at
    // most one missed update — exactly the same robustness model as
    // Sensors page.
    var FAST_MS = 1500;
    var SLOW_MS = 15000;
    var STABLE_TICKS_TO_SLOW = 3;

    var pollLastL = '?', pollLastR = '?';
    var pollStableCount = 0;

    function snapshotConnState(){
      // Reading from the DOM that refresh() just rendered. This is the
      // same source of truth the user sees — if either shows anything
      // other than a connected/up state, we treat as "in motion."
      var l = (el('g2-l-state')||{}).textContent || '';
      var r = (el('g2-r-state')||{}).textContent || '';
      return { l: l.trim(), r: r.trim() };
    }

    function pickNextDelay(){
      var s = snapshotConnState();
      var bothUp = (s.l === 'up' && s.r === 'up');
      if (s.l !== pollLastL || s.r !== pollLastR) {
        // State changed since last tick — kick back into fast mode.
        pollStableCount = 0;
      } else if (bothUp) {
        if (pollStableCount < 1000) pollStableCount++;
      } else {
        pollStableCount = 0;
      }
      pollLastL = s.l; pollLastR = s.r;
      return (bothUp && pollStableCount >= STABLE_TICKS_TO_SLOW)
               ? SLOW_MS : FAST_MS;
    }

    function pollTick(){
      try { refresh(); } catch(e){ /* never let the chain die */ }
      // Schedule the next tick whenever this one's HTTP fetches settle.
      // refresh() is fire-and-forget against multiple cli() promises;
      // we don't await them — the cadence is from-tick-start to next-
      // tick-start, which is what users intuit by "every N seconds."
      window.__btPollTimer = setTimeout(pollTick, pickNextDelay());
    }

    if (window.__btPollTimer) {
      clearTimeout(window.__btPollTimer);
      clearInterval(window.__btPollTimer);  // legacy if reloaded over old version
    }
    pollTick();
  });
})();
</script>
)JS", HTTPD_RESP_USE_STRLEN);
}

#endif
