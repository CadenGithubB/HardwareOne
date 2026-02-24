#ifndef DS3231_RTC_SENSOR_WEB_H
#define DS3231_RTC_SENSOR_WEB_H

#include <Arduino.h>
#include "WebServer_Utils.h"

inline void streamDS3231RtcSensorCard(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"HTML(

    <div class='sensor-card' id='sensor-card-rtc'>
      <div class='sensor-title'><span>RTC Clock (DS3231)</span><span class='status-indicator status-disabled' id='rtc-status-indicator'></span></div>
      <div class='sensor-description'>High-precision real-time clock with temperature sensor.</div>
      <div id='rtc-queue-status' style='display:none;background:#fff3cd;border:1px solid #ffc107;border-radius:4px;padding:8px;margin-bottom:10px;color:#856404;font-size:.9em'></div>
      <div class='sensor-controls'><button class='btn' id='btn-rtc-start'>Open RTC</button><button class='btn' id='btn-rtc-stop'>Close RTC</button></div>
      <div class='sensor-data' id='rtc-data'><div style="padding:1rem;text-align:center;color:var(--panel-fg)"><p style="margin:0;font-size:1.1em">RTC Closed</p><p style="margin:0.5rem 0 0 0;font-size:0.9em">Click "Open RTC" to begin</p></div></div>
    </div>

)HTML", HTTPD_RESP_USE_STRLEN);
}

inline void streamDS3231RtcSensorBindButtons(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "bind('btn-rtc-start','openrtc');bind('btn-rtc-stop','closertc');", HTTPD_RESP_USE_STRLEN);
}

inline void streamDS3231RtcDashboardDef(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "window.__dashSensorDefs.push({device:'DS3231',key:'rtc',name:'RTC (DS3231)',desc:'Date, Time & Temp'});", HTTPD_RESP_USE_STRLEN);
}

inline void streamDS3231RtcSensorJs(httpd_req_t* req) {
  // RTC reader: fetches once from server, then ticks locally every second.
  // Re-syncs from server every 30s to correct drift and update temperature.
  httpd_resp_send_chunk(req,
    "window._sensorReaders = window._sensorReaders || {};\n"
    "window._sensorDataIds = window._sensorDataIds || {};\n"
    "window._sensorPollingIntervals = window._sensorPollingIntervals || {};\n"
    "window._sensorDataIds.rtc = 'rtc-data';\n"
    "window._sensorPollingIntervals.rtc = 30000;\n"
    "(function() {\n"
    "  var rtcState = {year:0,month:0,day:0,hour:0,minute:0,second:0,dayOfWeek:'',temperature:0,valid:false};\n"
    "  var tickTimer = null;\n"
    "  var days = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];\n"
    "  function daysInMonth(y,m){return new Date(y,m,0).getDate();}\n"
    "  function pad(n){return n<10?'0'+n:''+n;}\n"
    "  function renderRTC() {\n"
    "    var el = document.getElementById('rtc-data');\n"
    "    if (!el || !rtcState.valid) return;\n"
    "    var html = '<div style=\"padding:0.75rem;font-size:0.95em;line-height:1.8\">';\n"
    "    html += '<p style=\"margin:0.5rem 0;font-size:1.1em\"><strong>' + rtcState.year + '-' + pad(rtcState.month) + '-' + pad(rtcState.day) + '</strong></p>';\n"
    "    html += '<p style=\"margin:0.5rem 0;font-size:1.4em;color:#007bff\"><strong>' + pad(rtcState.hour) + ':' + pad(rtcState.minute) + ':' + pad(rtcState.second) + '</strong></p>';\n"
    "    html += '<p style=\"margin:0.5rem 0;color:var(--panel-fg)\">' + rtcState.dayOfWeek + '</p>';\n"
    "    html += '<p style=\"margin:0.5rem 0\"><strong>Temperature:</strong> ' + rtcState.temperature.toFixed(1) + ' \\u00B0C</p>';\n"
    "    html += '</div>';\n"
    "    el.innerHTML = html;\n"
    "  }\n"
    "  function tickSecond() {\n"
    "    if (!rtcState.valid) return;\n"
    "    rtcState.second++;\n"
    "    if (rtcState.second >= 60) {\n"
    "      rtcState.second = 0; rtcState.minute++;\n"
    "      if (rtcState.minute >= 60) {\n"
    "        rtcState.minute = 0; rtcState.hour++;\n"
    "        if (rtcState.hour >= 24) {\n"
    "          rtcState.hour = 0; rtcState.day++;\n"
    "          var dim = daysInMonth(rtcState.year, rtcState.month);\n"
    "          if (rtcState.day > dim) {\n"
    "            rtcState.day = 1; rtcState.month++;\n"
    "            if (rtcState.month > 12) { rtcState.month = 1; rtcState.year++; }\n"
    "          }\n"
    "          var d = new Date(rtcState.year, rtcState.month-1, rtcState.day);\n"
    "          rtcState.dayOfWeek = days[d.getDay()];\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    renderRTC();\n"
    "  }\n"
    "  function startTick() {\n"
    "    if (!tickTimer) tickTimer = setInterval(tickSecond, 1000);\n"
    "  }\n"
    "  function stopTick() {\n"
    "    if (tickTimer) { clearInterval(tickTimer); tickTimer = null; }\n"
    "  }\n"
    "  window._sensorReaders.rtc = function() {\n"
    "    return fetch('/api/sensors/status', {cache: 'no-store', credentials: 'include'})\n"
    "      .then(function(r) { return r.json(); })\n"
    "      .then(function(status) {\n"
    "        var el = document.getElementById('rtc-data');\n"
    "        if (!el) return;\n"
    "        if (!status.rtcCompiled) {\n"
    "          stopTick(); rtcState.valid = false;\n"
    "          el.textContent = 'RTC error: not_compiled';\n"
    "          return 'not_compiled';\n"
    "        }\n"
    "        if (!status.rtcEnabled) {\n"
    "          stopTick(); rtcState.valid = false;\n"
    "          el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:var(--panel-fg)\"><p style=\"margin:0;font-size:1.1em\">RTC Closed</p><p style=\"margin:0.5rem 0 0 0;font-size:0.9em\">Click \"Open\" to begin</p></div>';\n"
    "          return 'stopped';\n"
    "        }\n"
    "        return fetch('/api/sensors?sensor=rtc&ts=' + Date.now(), {cache: 'no-store', credentials: 'include'})\n"
    "          .then(function(r) { return r.json(); })\n"
    "          .then(function(data) {\n"
    "            if (data.error) {\n"
    "              if (data.error === 'no_data') {\n"
    "                el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#17a2b8\"><p style=\"margin:0;font-size:1.1em\">Reading RTC...</p><p style=\"margin:0.5rem 0 0 0;font-size:0.9em\">Waiting for first data</p></div>';\n"
    "              } else if (data.error === 'not_enabled') {\n"
    "                el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#ffc107\"><p style=\"margin:0;font-size:1.1em\">RTC not connected</p><p style=\"margin:0.5rem 0 0 0;font-size:0.9em\">Check wiring</p></div>';\n"
    "              } else {\n"
    "                el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#dc3545\"><p style=\"margin:0;font-size:1.1em\">RTC Error</p><p style=\"margin:0.5rem 0 0 0;font-size:0.9em\">' + data.error + '</p></div>';\n"
    "              }\n"
    "              return data;\n"
    "            }\n"
    "            rtcState.year = data.year;\n"
    "            rtcState.month = data.month;\n"
    "            rtcState.day = data.day;\n"
    "            rtcState.hour = data.hour;\n"
    "            rtcState.minute = data.minute;\n"
    "            rtcState.second = data.second;\n"
    "            rtcState.dayOfWeek = data.dayOfWeek;\n"
    "            rtcState.temperature = data.temperature;\n"
    "            rtcState.valid = true;\n"
    "            renderRTC();\n"
    "            startTick();\n"
    "            return data;\n"
    "          });\n"
    "      })\n"
    "      .catch(function(e) {\n"
    "        console.error('[Sensors] RTC read error', e);\n"
    "        var el2 = document.getElementById('rtc-data');\n"
    "        if (el2) el2.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#dc3545\">Error reading RTC data</div>';\n"
    "        throw e;\n"
    "      });\n"
    "  };\n"
    "})();\n",
    HTTPD_RESP_USE_STRLEN);
}

#endif // DS3231_RTC_SENSOR_WEB_H
