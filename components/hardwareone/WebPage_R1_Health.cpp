#include "System_BuildConfig.h"

#if ENABLE_WEB_R1_HEALTH

#include <Arduino.h>
#include <string.h>

#include "G2_Ring.h"
#include "System_MemUtil.h"           // ps_alloc, AllocPref
#include "System_SensorLogging.h"     // buildHealthStatusJson, health*
#include "System_Settings.h"
#include "System_User.h"              // AuthContext
#include "WebPage_R1_Health.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"

// ===========================================================================
// /api/health/status — identical schema to `healthstatus json`
// ===========================================================================
esp_err_t handleR1HealthStatus(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  httpd_resp_set_type(req, "application/json");
  static char* buf = nullptr;
  static const size_t kBufSize = 4096;
  if (!buf) buf = (char*)ps_alloc(kBufSize, AllocPref::PreferPSRAM, "health.status.json");
  if (!buf) { httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
  buildHealthStatusJson(buf, kBufSize);
  httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static bool readHealthActionBody(httpd_req_t* req, char* out, size_t cap) {
  if (!req || !out || cap < 2 || req->content_len == 0 || req->content_len >= cap)
    return false;
  size_t received = 0;
  while (received < req->content_len) {
    const int n = httpd_req_recv(req, out + received, req->content_len - received);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) return false;
    received += static_cast<size_t>(n);
  }
  out[received] = '\0';
  return true;
}

static String healthFormValue(const String& body, const char* key) {
  const String prefix = String(key) + "=";
  int start = body.indexOf(prefix);
  while (start >= 0 && start > 0 && body.charAt(start - 1) != '&')
    start = body.indexOf(prefix, start + 1);
  if (start < 0) return String();
  start += prefix.length();
  int end = body.indexOf('&', start);
  if (end < 0) end = body.length();
  String value = body.substring(start, end);
  value.replace("+", " ");
  value.toLowerCase();
  return value;
}

static esp_err_t healthActionReply(httpd_req_t* req, const char* status,
                                   const char* message) {
  httpd_resp_set_type(req, "text/plain");
  if (status) httpd_resp_set_status(req, status);
  httpd_resp_send(req, message ? message : "", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t handleR1HealthAction(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  if (isGuestUser(ctx.user))
    return healthActionReply(req, "403 Forbidden", "guest access is read-only");
  char bodyBuf[160];
  if (!readHealthActionBody(req, bodyBuf, sizeof(bodyBuf)))
    return healthActionReply(req, "400 Bad Request", "invalid action body");
  const String body(bodyBuf);
  const String action = healthFormValue(body, "action");
  const String value = healthFormValue(body, "value");
  const bool admin = isAdminUser(ctx.user);

  if (action == "poll") {
    if (healthStartPollBurst()) {
      return healthActionReply(req, "202 Accepted", "health refresh queued");
    }
    if (!g2RingIsConnected()) {
      return healthActionReply(req, "409 Conflict", "ring is not connected");
    }
    G2RingControlStatus status{};
    g2RingGetControlStatus(status);
    const bool refreshSupported = g2RingHealthPageRefreshSupported();
    return status.protocolProfileKnown && !refreshSupported
        ? healthActionReply(req, "409 Conflict",
              "health refresh is unsupported on the active ring profile; no command was sent")
        : healthActionReply(req, "409 Conflict",
              "ring setup/profile is not ready for health refresh; no command was sent");
  }
  if (action == "logging-toggle") {
    const char* result = healthLoggingSet(!gSettings.healthLoggingEnabled);
    const bool ok = result && strncmp(result, "SUCCESS", 7) == 0;
    return healthActionReply(req, ok ? nullptr : "409 Conflict", result);
  }
  if (action == "control-refresh") {
    if (g2RingRefreshControlStatus()) {
      return healthActionReply(req, "202 Accepted",
                               "ring control refresh queued");
    }
    G2RingControlStatus status{};
    g2RingGetControlStatus(status);
    return status.lowPowerLastError == G2_RING_ERR_FEATURE_UNSUPPORTED
        ? healthActionReply(req, "409 Conflict",
              "low-power status is unsupported on the active ring profile; no command was sent")
        : healthActionReply(req, "409 Conflict", "control refresh not queued");
  }
  if (action == "history-refresh") {
    return g2RingRequestHistoryRefresh(false)
        ? healthActionReply(req, "202 Accepted", "history refresh queued")
        : healthActionReply(req, "409 Conflict", "history refresh not queued");
  }
  if (action == "history-force") {
    if (!admin) return healthActionReply(req, "403 Forbidden", "admin required");
    return g2RingRequestHistoryRefresh(true)
        ? healthActionReply(req, "202 Accepted", "forced history refresh queued")
        : healthActionReply(req, "409 Conflict", "forced history refresh not queued");
  }
  if (action == "collection" || action == "low-power") {
    if (!admin) return healthActionReply(req, "403 Forbidden", "admin required");
    G2RingDesiredState desired;
    if (value == "preserve") desired = G2_RING_PRESERVE;
    else if (value == "off") desired = G2_RING_OFF;
    else if (value == "on") desired = G2_RING_ON;
    else return healthActionReply(req, "400 Bad Request", "value must be preserve, off, or on");
    const bool queued = action == "collection"
        ? g2RingSetHealthCollectionDesired(desired)
        : g2RingSetLowPowerDesired(desired);
    if (!queued)
      return healthActionReply(req, "409 Conflict", "desired policy not accepted");
    G2RingControlStatus status{};
    g2RingGetControlStatus(status);
    const bool unsupported = action == "collection"
        ? status.healthLastError == G2_RING_ERR_FEATURE_UNSUPPORTED
        : status.lowPowerLastError == G2_RING_ERR_FEATURE_UNSUPPORTED;
    if (unsupported) {
      return action == "collection"
          ? healthActionReply(req, "202 Accepted",
                "health collection preference saved; this operation is unsupported on the active ring profile, so no command was sent")
          : healthActionReply(req, "202 Accepted",
                "low-power preference saved; unsupported on the active ring profile, so no command was sent");
    }
    return action == "collection"
        ? healthActionReply(req, "202 Accepted",
              "health collection policy accepted; 0x0E has no proven readback, so ACK is not observed state")
        : healthActionReply(req, "202 Accepted",
              "low-power policy accepted; awaiting verified readback observation");
  }
  return healthActionReply(req, "400 Bad Request", "unknown health action");
}

// ===========================================================================
// /r1-health page
// ===========================================================================
static void streamR1HealthContent(httpd_req_t* req, const String& username) {
  streamBeginHtml(req, "R1 Health", false, username, "r1-health");

  httpd_resp_send_chunk(req, R"HTML(
<div class='card'>
  <h2 style='margin-top:0'>R1 Health</h2>
  <p style='margin:.25rem 0 .75rem;opacity:.85;font-size:.9em'>
    Live ring vitals, ring-owned privacy/power controls, and HardwareOne-local
    logging. Connect the ring on the
    <a href='/bluetooth'>Bluetooth</a> page.
  </p>
  <div id='rh-down' style='display:none;opacity:.85;margin-bottom:.75rem'>Ring not connected.</div>
  <div id='rh-live' style='display:flex;flex-wrap:wrap;gap:1.25rem;align-items:baseline'>
    <div><span style='opacity:.7'>HR</span> <b id='rh-hr' style='font-size:1.6rem'>--</b>
      <span id='rh-hr-age' style='font-size:.8em;opacity:.7'></span></div>
    <div><span style='opacity:.7'>HRV</span> <b id='rh-hrv' style='font-size:1.6rem'>--</b>
      <span id='rh-hrv-age' style='font-size:.8em;opacity:.7'></span></div>
    <div><span style='opacity:.7'>SpO2</span> <b id='rh-spo2' style='font-size:1.6rem'>--</b>
      <span id='rh-spo2-age' style='font-size:.8em;opacity:.7'></span></div>
    <div><span style='opacity:.7'>Temp</span> <b id='rh-temp' style='font-size:1.6rem'>--</b>
      <span id='rh-temp-age' style='font-size:.8em;opacity:.7'></span></div>
    <div><span style='opacity:.7'>Battery</span> <b id='rh-bat' style='font-size:1.6rem'>--</b>
      <span id='rh-bat-age' style='font-size:.8em;opacity:.7'></span></div>
    <div><span style='opacity:.7'>Wear</span> <b id='rh-wear' style='font-size:1.6rem'>--</b></div>
  </div>
  <div style='margin-top:1rem;display:flex;flex-wrap:wrap;gap:.5rem;align-items:center'>
    <button class='btn' id='rh-poll' data-guest-hide disabled>Poll unavailable</button>
    <button class='btn' id='rh-logging' data-guest-hide>Health logging: --</button>
    <span id='rh-logging-meta' style='font-size:.85em;opacity:.8'></span>
  </div>
  <div id='rh-status' style='font-size:.82em;opacity:.75;min-height:1.2em;margin-top:8px'></div>
</div>

<div class='card'>
  <h3 style='margin-top:0'>Ring controls</h3>
  <div style='font-size:.88em;margin-bottom:.75rem'>
    Setup: <b id='rh-setup'>--</b> · Profile: <b id='rh-profile'>--</b>
  </div>
  <div style='display:grid;grid-template-columns:minmax(150px,1fr) minmax(150px,1fr);gap:.8rem'>
    <div>
      <label for='rh-collection'>Ring health collection</label>
      <div style='display:flex;gap:.4rem;margin-top:.25rem'>
        <select id='rh-collection' class='form-input input-fit' data-guest-hide>
          <option value='preserve'>Preserve</option><option value='on'>On</option><option value='off'>Off</option>
        </select>
        <button class='btn' id='rh-collection-apply' data-guest-hide>Apply</button>
      </div>
      <div id='rh-collection-state' style='font-size:.8em;opacity:.75;margin-top:.25rem'>--</div>
    </div>
    <div>
      <label for='rh-low-power'>Ring low power</label>
      <div style='display:flex;gap:.4rem;margin-top:.25rem'>
        <select id='rh-low-power' class='form-input input-fit' data-guest-hide>
          <option value='preserve'>Preserve</option><option value='on'>On</option><option value='off'>Off</option>
        </select>
        <button class='btn' id='rh-low-power-apply' data-guest-hide>Apply</button>
      </div>
      <div id='rh-low-power-state' style='font-size:.8em;opacity:.75;margin-top:.25rem'>--</div>
    </div>
  </div>
  <div style='margin-top:.75rem'>
    <button class='btn' id='rh-control-refresh' data-guest-hide>Refresh observed state</button>
    <span style='font-size:.78em;opacity:.7'>Apply controls require admin. Desired and observed are never conflated.</span>
  </div>
</div>

<div class='card'>
  <h3 style='margin-top:0'>History &amp; activity</h3>
  <div id='rh-history-state' style='font-size:.88em'>No typed history yet.</div>
  <div id='rh-activity' style='margin-top:.5rem;font-size:.95em'>Steps -- · calories --</div>
  <div id='rh-history-store' style='margin-top:.35rem;font-size:.8em;opacity:.75'></div>
  <div style='margin-top:.75rem;display:flex;gap:.5rem;flex-wrap:wrap'>
    <button class='btn' id='rh-history-refresh' data-guest-hide>Refresh history</button>
    <button class='btn' id='rh-history-force' data-guest-hide>Force refresh (admin)</button>
  </div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, R"JS(
<script>(function(){
  function v(ok,n,suf){return ok?(n+(suf||'')):'--';}
  function age(sec){
    if(sec==null||sec<0)return '';
    if(sec<60)return sec+'s ago';
    if(sec<3600)return Math.floor(sec/60)+'m ago';
    if(sec<86400)return Math.floor(sec/3600)+'h ago';
    return Math.floor(sec/86400)+'d ago';
  }
  function wear(s){
    if(!s||!s.wearValid)return '--';
    if(s.wear===2)return 'on';
    if(s.wear===1)return 'off';
    return '?';
  }
  function temp(s){
    if(!s||!s.tempValid)return '--';
    var t=s.tempTenths|0;
    var whole=(t/10)|0;
    var frac=Math.abs(t%10);
    return whole+'.'+frac+'°C';
  }
  function ctl(desired,observed,pending,error,ageSec,tx){
    var parts=['desired '+(desired||'preserve'),'observed '+(observed||'unknown')];
    if(ageSec!=null&&ageSec>=0)parts.push(age(ageSec));
    if(pending)parts.push('pending');
    if(tx&&tx!=='invalid')parts.push(tx);
    if(error&&error!=='none')parts.push('error '+error);
    return parts.join(' · ');
  }
  function when(epoch){
    if(!epoch)return '--';
    try{return new Date(epoch*1000).toLocaleString();}catch(_){return String(epoch);}
  }
  function apply(s){
    if(!s||s.schema!==1)return;
    if(s.error){setMsg(s.error);return;}
    var up=!!s.ringConnected;
    hw.toggle('rh-down',!up);
    hw.setText('rh-hr',v(s.hrValid,s.hr));
    hw.setText('rh-hr-age',age(s.hrAgeSec));
    hw.setText('rh-hrv',v(s.hrvValid,s.hrv));
    hw.setText('rh-hrv-age',age(s.hrvAgeSec));
    hw.setText('rh-spo2',v(s.spo2Valid,s.spo2,'%'));
    hw.setText('rh-spo2-age',age(s.spo2AgeSec));
    hw.setText('rh-temp',temp(s));
    hw.setText('rh-temp-age',age(s.tempAgeSec));
    hw.setText('rh-bat',v(s.batteryValid,s.battery,'%'));
    hw.setText('rh-bat-age',age(s.batteryAgeSec));
    hw.setText('rh-wear',wear(s));
    var poll=hw.$('rh-poll');
    if(poll){
      var canPoll=up&&!!s.healthRefreshSupported;
      poll.disabled=!canPoll;
      poll.textContent=s.healthRefreshSupported?'Poll Now':
        (up&&s.protocolProfileKnown?'Poll unsupported':'Poll unavailable');
      poll.title=canPoll?'Refresh from 2.2.9 DAILY data and device status':
        'Poll Now requires R1 firmware 2.2.9.0003';
    }
    var btn=hw.$('rh-logging');
    if(btn)btn.textContent=s.healthLoggingActive?'Health logging: ON':
      (s.healthLoggingEnabled?'Health logging: armed':'Health logging: off');
    var meta=[];
    if(s.healthLoggingTimedPollSupported&&s.healthLoggingPollIntervalSec!=null)
      meta.push('poll '+s.healthLoggingPollIntervalSec+'s');
    else meta.push('passive/on-demand');
    if(s.healthLoggingWorkerRunning)meta.push('logger running');
    if(s.healthLoggingAtRest)meta.push('at-rest '+s.healthLoggingAtRest);
    hw.setText('rh-logging-meta',meta.join(' · '));
    hw.setText('rh-setup',(s.setupState||'--')+(s.setupError&&s.setupError!=='none'?' ('+s.setupError+')':''));
    hw.setText('rh-profile',s.protocolProfile||'unknown');
    var collection=hw.$('rh-collection');
    if(collection&&document.activeElement!==collection)collection.value=s.healthCollectionDesired||'preserve';
    var low=hw.$('rh-low-power');
    if(low&&document.activeElement!==low)low.value=s.lowPowerDesired||'preserve';
    hw.setText('rh-collection-state',ctl(s.healthCollectionDesired,s.healthCollectionObserved,
      s.healthCollectionPending,s.healthCollectionError,s.healthCollectionObservedAgeSec,
      s.healthCollectionTransaction));
    hw.setText('rh-low-power-state',ctl(s.lowPowerDesired,s.lowPowerObserved,
      s.lowPowerPending,s.lowPowerError,s.lowPowerObservedAgeSec,s.lowPowerTransaction));
    var hs='History '+(s.historyState||'idle');
    if(s.historyDayStart)hs+=' · day '+when(s.historyDayStart);
    if(s.historyLastSuccessEpoch)hs+=' · success '+when(s.historyLastSuccessEpoch);
    else if(s.historyLastPartialEpoch)hs+=' · partial '+when(s.historyLastPartialEpoch);
    if(s.sleepState)hs+=' · sleep '+s.sleepState;
    hw.setText('rh-history-state',hs);
    if(s.activityAvailable){
      hw.setText('rh-activity','Steps '+s.activitySteps+' · kcal active '+s.activityActiveKcal+
        ', resting '+s.activityRestingKcal+', total '+s.activityTotalKcal+' · '+
        s.activityBucketCount+'/144 slots · '+(s.activityFullDayVerified?'message verified':'unverified'));
    }else hw.setText('rh-activity','Steps -- · calories -- · no activity page');
    hw.setText('rh-history-store','Store '+(s.historyStoreAvailable?'available':'unavailable')+
      (s.historyStorePending?' · commit pending':'')+
      (s.historyStoreEncryptionRequired?' · sealing required':' · plaintext policy')+
      (s.historyStoreEncrypted?' · last commit encrypted':'')+
      (s.historyStoreError&&s.historyStoreError!=='none'?' · error '+s.historyStoreError:''));
  }
  function setMsg(t){hw.setText('rh-status',t||'');}
  function action(name,value){
    setMsg('working…');
    var form={action:name};if(value!=null)form.value=value;
    return hw.postFormText('/api/health/action',form).then(function(t){
      setMsg(t||'queued');return hw.fetchJSON('/api/health/status');
    }).then(apply).catch(function(e){setMsg(String(e));});
  }
  hw.fetchJSON('/api/health/status').then(apply).catch(function(e){console.error('[R1H]',e);});
  hw.pollJSON('/api/health/status',2000,apply);

  hw.on(hw.$('rh-poll'),'click',function(){action('poll');});
  hw.on(hw.$('rh-logging'),'click',function(){action('logging-toggle');});
  hw.on(hw.$('rh-control-refresh'),'click',function(){action('control-refresh');});
  hw.on(hw.$('rh-collection-apply'),'click',function(){action('collection',hw.$('rh-collection').value);});
  hw.on(hw.$('rh-low-power-apply'),'click',function(){action('low-power',hw.$('rh-low-power').value);});
  hw.on(hw.$('rh-history-refresh'),'click',function(){action('history-refresh');});
  hw.on(hw.$('rh-history-force'),'click',function(){action('history-force');});
})();</script>
)JS", HTTPD_RESP_USE_STRLEN);

  streamEndHtml(req);
}

esp_err_t handleR1HealthPage(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  streamPageWithContent(req, "r1-health", ctx.user, streamR1HealthContent);
  return ESP_OK;
}

void registerR1HealthHandlers(httpd_handle_t server) {
  static const httpd_uri_t healthPage = {
    .uri = "/r1-health", .method = HTTP_GET, .handler = handleR1HealthPage, .user_ctx = NULL
  };
  httpd_register_uri_handler(server, &healthPage);
  static const httpd_uri_t healthStatus = {
    .uri = "/api/health/status", .method = HTTP_GET, .handler = handleR1HealthStatus, .user_ctx = NULL
  };
  httpd_register_uri_handler(server, &healthStatus);
  static const httpd_uri_t healthAction = {
    .uri = "/api/health/action", .method = HTTP_POST, .handler = handleR1HealthAction, .user_ctx = NULL
  };
  httpd_register_uri_handler(server, &healthAction);
}

#endif  // ENABLE_WEB_R1_HEALTH
