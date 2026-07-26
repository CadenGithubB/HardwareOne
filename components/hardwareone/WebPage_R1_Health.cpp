#include "System_BuildConfig.h"

#if ENABLE_WEB_R1_HEALTH

#include <Arduino.h>

#include "System_MemUtil.h"           // ps_alloc, AllocPref
#include "System_SensorLogging.h"     // buildHealthStatusJson, health*
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
  static const size_t kBufSize = 768;
  if (!buf) buf = (char*)ps_alloc(kBufSize, AllocPref::PreferPSRAM, "health.status.json");
  if (!buf) { httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
  buildHealthStatusJson(buf, kBufSize);
  httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
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
    Live ring vitals and Health Track. Connect the ring on the
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
    <button class='btn' id='rh-poll' data-guest-hide>Poll Now</button>
    <button class='btn' id='rh-track' data-guest-hide>Track: --</button>
    <span id='rh-track-meta' style='font-size:.85em;opacity:.8'></span>
  </div>
  <div id='rh-status' style='font-size:.82em;opacity:.75;min-height:1.2em;margin-top:8px'></div>
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
  function apply(s){
    if(!s)return;
    var up=!!s.connected;
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
    var btn=hw._ge('rh-track');
    if(btn)btn.textContent=s.trackActive?'Track: ON':(s.trackEnabled?'Track: armed':'Track: off');
    var meta=[];
    if(s.pollIntervalSec!=null)meta.push('poll '+s.pollIntervalSec+'s');
    if(s.logging)meta.push('logging');
    hw.setText('rh-track-meta',meta.join(' · '));
  }
  function setMsg(t){hw.setText('rh-status',t||'');}
  hw.fetchJSON('/api/health/status').then(apply).catch(function(e){console.error('[R1H]',e);});
  hw.pollJSON('/api/health/status',2000,apply);

  hw.on(hw._ge('rh-poll'),'click',function(){
    setMsg('polling…');
    hw.postFormText('/api/cli',{cmd:'healthstatus poll'}).then(function(t){
      setMsg(t||'ok');
    }).catch(function(e){setMsg(String(e));});
  });
  hw.on(hw._ge('rh-track'),'click',function(){
    setMsg('toggling Track…');
    hw.postFormText('/api/cli',{cmd:'healthtrack toggle'}).then(function(t){
      setMsg(t||'ok');
      return hw.fetchJSON('/api/health/status');
    }).then(apply).catch(function(e){setMsg(String(e));});
  });
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
}

#endif  // ENABLE_WEB_R1_HEALTH
