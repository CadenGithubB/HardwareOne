#include "System_BuildConfig.h"

#if ENABLE_WEB_BATTERY

#include <Arduino.h>
#include <ArduinoJson.h>

#include "System_User.h"          // AuthContext
#include "System_Battery.h"       // gBatteryState, getBatteryStatusString()
#include "System_MemUtil.h"       // ps_alloc, AllocPref, PSRAM_JSON_DOC
#include "WebPage_Battery.h"
#include "WebServer_Server.h"     // streamBeginHtml/EndHtml, streamPageWithContent, WEB_AUTH_OR_RETURN
#include "WebServer_Utils.h"

// ===========================================================================
// /api/battery/status — capability-flagged live snapshot
// ===========================================================================
// Battery JSON is built by the single core builder in System_Battery.cpp
// (buildBatteryJson(JsonDocument&)) so the web /api/battery/status and the
// `battery json` CLI/BLE command return the IDENTICAL schema. Fields: present,
// backend, voltage, percentage, status, charging, usbPresent, vbusSense,
// lastReadMsAgo, ratePctPerHr/etaMinutes (fuelgauge only), rawADC (adc only).
// Availability/validity flags distinguish unknown/null from a measured zero.
esp_err_t handleBatteryStatus(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  httpd_resp_set_type(req, "application/json");
  static char* buf = nullptr;
  static const size_t kBufSize = 1536;
  if (!buf) buf = (char*)ps_alloc(kBufSize, AllocPref::PreferPSRAM, "battery.status.json");
  if (!buf) { httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
  PSRAM_JSON_DOC(doc);
  extern void buildBatteryJson(JsonDocument& doc);  // core: System_Battery.cpp
  buildBatteryJson(doc);
  if (measureJson(doc) >= kBufSize) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Battery status exceeds response buffer");
    return ESP_FAIL;
  }
  const size_t written = serializeJson(doc, buf, kBufSize);
  httpd_resp_send(req, buf, written);
  return ESP_OK;
}

// ===========================================================================
// /battery page
// ===========================================================================
static void streamBatteryContent(httpd_req_t* req, const String& username) {
  streamBeginHtml(req, "Battery", false, username, "battery");

  httpd_resp_send_chunk(req, R"HTML(
<div class='card'>
  <h2 style='margin-top:0'>Battery</h2>
  <div id='bat-absent' style='display:none;opacity:.8'></div>
  <div id='bat-live' style='display:flex;flex-wrap:wrap;gap:1.5rem;align-items:baseline'>
    <div><span id='bat-pct' style='font-size:2.6rem;font-weight:bold'>--</span><span style='font-size:1.2rem'>%</span></div>
    <div>Voltage: <b id='bat-volt'>--</b> V</div>
    <div>Status: <b id='bat-status'>--</b></div>
    <div id='bat-rate-wrap' style='display:none'>Rate: <b id='bat-rate'>--</b> %/hr</div>
    <div id='bat-eta-wrap' style='display:none'>Est. remaining: <b id='bat-eta'>--</b></div>
    <div>Source: <b id='bat-src'>--</b></div>
    <div>Charging: <b id='bat-charging'>--</b></div>
  </div>
  <div id='bat-estimate' style='display:none;margin-top:.7rem;opacity:.8'>Charge is estimated from voltage and may rise while plugged in, even without a cell. It does not confirm battery presence or charging.</div>
</div>

<div class='card'>
  <h3 style='margin-top:0'>History</h3>
  <canvas id='bat-chart' width='960' height='280' style='width:100%;max-width:960px;background:var(--panel-bg);border:1px solid #444'></canvas>
  <div style='margin:.5rem 0'>
    <button class='btn' id='bat-refresh'>Refresh log</button>
    <span id='bat-log-info' style='margin-left:.5rem;opacity:.8'></span>
  </div>
  <div id='bat-table-wrap' style='overflow-x:hidden;overflow-y:auto;max-height:340px'>
    <table id='bat-table' style='border-collapse:separate;border-spacing:0;font-family:monospace;font-size:.8rem;width:100%;table-layout:fixed'></table>
  </div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, R"JS(
<script>(function(){
  function fmt(n,d){return (n===null||n===undefined||isNaN(n))?'--':Number(n).toFixed(d);}
  function etaStr(min){if(min==null)return '--';var h=Math.floor(min/60),m=min%60;return h>0?(h+'h '+m+'m'):(m+'m');}

  // --- live status poll (capability-driven) ---
  function applyStatus(s){
    if(!s){return;}
    var absent=hw.$('bat-absent'),live=hw.$('bat-live');
    var notice=s.voltageAvailable===false?'Battery monitoring is unavailable on this board.':
      (s.present===false?'No battery detected.':
      (s.voltageValid===false?'Battery voltage is unavailable.':''));
    hw.setText(absent,notice);hw.toggle(absent,!!notice);
    if(live)live.style.display='flex';
    var hasPct=s.percentageValid===true&&s.percentage!=null;
    hw.setText('bat-pct',hasPct?((s.percentageEstimated?'~':'')+fmt(s.percentage,1)):'--');
    hw.setText('bat-volt',s.voltageValid?fmt(s.voltage,3):'--');
    hw.setText('bat-status',s.status||'Unknown');
    hw.setText('bat-src',s.usbKnown?(s.usbPresent?'USB connected':(s.present===true?'Battery':'USB absent')):'Unknown');
    hw.setText('bat-charging',s.chargingKnown?((s.charging?'Yes':'No')+(s.chargingEstimated?' (estimated)':'')):'Unknown');
    hw.toggle('bat-estimate',hasPct&&s.percentageSource==='voltage');
    var hasRate=s.rateValid===true;
    hw.toggle('bat-rate-wrap',hasRate);
    if(hasRate)hw.setText('bat-rate',fmt(s.ratePctPerHr,2));
    var showEta=hasRate&&(s.etaMinutes!=null&&s.etaMinutes!==undefined);
    hw.toggle('bat-eta-wrap',showEta);
    if(showEta)hw.setText('bat-eta',etaStr(s.etaMinutes));
  }
  hw.fetchJSON('/api/battery/status').then(applyStatus).catch(function(e){console.error('[BATTERY] status',e);});
  hw.pollJSON('/api/battery/status',2000,applyStatus);

  // --- log fetch + parse + render (clean comma-CSV) ---
  function parseCSV(text){
    var lines=text.split('\n').filter(function(l){return l.trim().length;});
    if(!lines.length)return {hdr:[],rows:[]};
    return {hdr:lines[0].split(','),rows:lines.slice(1).map(function(l){return l.split(',');})};
  }
  function col(hdr,name){for(var i=0;i<hdr.length;i++){if(hdr[i].replace(/\[.*\]/,'').trim()===name)return i;}return -1;}
  function renderTable(d){
    var t=hw.$('bat-table');if(!t)return;
    var cell='padding:2px 4px;overflow:hidden;text-overflow:ellipsis;word-break:break-word;vertical-align:top';
    // Sticky header: needs border-collapse:separate on the table (set in HTML)
    // and an opaque background so scrolling rows don't bleed through.
    var th='text-align:left;position:sticky;top:0;z-index:2;background:var(--panel-bg,#1e1e1e);box-shadow:inset 0 -1px 0 #555;'+cell;
    var h='<thead><tr>';for(var i=0;i<d.hdr.length;i++){h+='<th style="'+th+'">'+d.hdr[i]+'</th>';}h+='</tr></thead><tbody>';
    var rows=d.rows.slice(-60);
    for(var r=0;r<rows.length;r++){h+='<tr>';for(var c=0;c<rows[r].length;c++){h+='<td style="'+cell+'">'+(rows[r][c]||'')+'</td>';}h+='</tr>';}
    h+='</tbody>';
    t.innerHTML=h;
  }
  function drawChart(d){
    var cv=hw.$('bat-chart');if(!cv||!cv.getContext)return;var ctx=cv.getContext('2d');
    var W=cv.width,H=cv.height,padL=34,padR=10,padT=10,padB=6;
    ctx.clearRect(0,0,W,H);
    var ci=col(d.hdr,'pct'),ei=col(d.hdr,'event');
    if(ci<0||!d.rows.length)return;
    var n=d.rows.length;
    function X(i){return padL+(W-padL-padR)*(n<=1?0:i/(n-1));}
    function Yp(p){return padT+(H-padT-padB)*(1-(p/100));}
    ctx.strokeStyle='#444';ctx.fillStyle='#888';ctx.font='10px monospace';ctx.lineWidth=1;
    [0,50,100].forEach(function(p){var y=Yp(p);ctx.beginPath();ctx.moveTo(padL,y);ctx.lineTo(W-padR,y);ctx.stroke();ctx.fillText(p+'%',2,y+3);});
    // event markers (vertical ticks) behind the curve
    if(ei>=0){ctx.strokeStyle='#e0a030';ctx.lineWidth=1;
      for(var j=0;j<n;j++){if(!(d.rows[j][ei]||'').trim())continue;var x=X(j);ctx.beginPath();ctx.moveTo(x,padT);ctx.lineTo(x,H-padB);ctx.stroke();}}
    // percent curve
    ctx.strokeStyle='#4caf50';ctx.lineWidth=2;ctx.beginPath();var started=false;
    for(var i=0;i<n;i++){var p=parseFloat(d.rows[i][ci]);if(isNaN(p)){started=false;continue;}var x=X(i),y=Yp(p);started?ctx.lineTo(x,y):(ctx.moveTo(x,y),started=true);}
    ctx.stroke();
  }
  function loadLog(){
    var info=hw.$('bat-log-info');hw.setText(info,'loading…');
    hw.fetchText('/api/files/view?name=battery.csv&mode=raw').then(function(text){
      var d=parseCSV(text);renderTable(d);drawChart(d);
      hw.setText(info,d.rows.length+' samples');
    }).catch(function(e){hw.setText(info,'no log data yet');console.error('[BATTERY] log',e);});
  }
  hw.on(hw.$('bat-refresh'),'click',loadLog);
  loadLog();
})();</script>
)JS", HTTPD_RESP_USE_STRLEN);

  streamEndHtml(req);
}

esp_err_t handleBatteryPage(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  streamPageWithContent(req, "battery", ctx.user, streamBatteryContent);
  return ESP_OK;
}

void registerBatteryHandlers(httpd_handle_t server) {
  static const httpd_uri_t batteryPage = { .uri = "/battery", .method = HTTP_GET, .handler = handleBatteryPage, .user_ctx = NULL };
  httpd_register_uri_handler(server, &batteryPage);
  static const httpd_uri_t batteryStatus = { .uri = "/api/battery/status", .method = HTTP_GET, .handler = handleBatteryStatus, .user_ctx = NULL };
  httpd_register_uri_handler(server, &batteryStatus);
}

#endif // ENABLE_WEB_BATTERY
