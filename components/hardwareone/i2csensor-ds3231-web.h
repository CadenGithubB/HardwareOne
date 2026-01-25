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
      <div class='sensor-controls'><button class='btn' id='btn-rtc-start'>Start RTC</button><button class='btn' id='btn-rtc-stop'>Stop RTC</button></div>
      <div class='sensor-data' id='rtc-data'><div style="padding:1rem;text-align:center;color:#6c757d"><p style="margin:0;font-size:1.1em">RTC Stopped</p><p style="margin:0.5rem 0 0 0;font-size:0.9em">Click "Start RTC" to begin</p></div></div>
    </div>

)HTML", HTTPD_RESP_USE_STRLEN);
}

inline void streamDS3231RtcSensorBindButtons(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "bind('btn-rtc-start','rtcstart');bind('btn-rtc-stop','rtcstop');", HTTPD_RESP_USE_STRLEN);
}

inline void streamDS3231RtcDashboardDef(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "window.__dashSensorDefs.push({device:'DS3231',key:'rtc',name:'RTC (DS3231)',desc:'Date, Time & Temp'});", HTTPD_RESP_USE_STRLEN);
}

inline void streamDS3231RtcSensorJs(httpd_req_t* req) {
  httpd_resp_send_chunk(req,
    "window._sensorReaders = window._sensorReaders || {};\n"
    "window._sensorDataIds = window._sensorDataIds || {};\n"
    "window._sensorPollingIntervals = window._sensorPollingIntervals || {};\n"
    "window._sensorDataIds.rtc = 'rtc-data';\n"
    "window._sensorPollingIntervals.rtc = 1000;\n"
    "window._sensorReaders.rtc = function() {\n"
    "  return fetch('/api/sensors/status', {cache: 'no-store', credentials: 'include'})\n"
    "    .then(function(r) { return r.json(); })\n"
    "    .then(function(status) {\n"
    "      var el = document.getElementById('rtc-data');\n"
    "      if (!el) return;\n"
    "      if (!status.rtcCompiled) {\n"
    "        el.textContent = 'RTC error: not_compiled';\n"
    "        return 'not_compiled';\n"
    "      }\n"
    "      if (!status.rtcEnabled) {\n"
    "        el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#6c757d\"><p style=\"margin:0;font-size:1.1em\">RTC Stopped</p><p style=\"margin:0.5rem 0 0 0;font-size:0.9em\">Click \\\"Start RTC\\\" to begin</p></div>';\n"
    "        return 'stopped';\n"
    "      }\n"
    "      return fetch('/api/cli', {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body: 'cmd=rtc', cache: 'no-store', credentials: 'include'})\n"
    "        .then(function(r) { return r.text(); })\n"
    "        .then(function(txt) {\n"
    "          if (!txt || txt.trim() === '' || txt === 'OK') {\n"
    "            el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#17a2b8\"><p style=\"margin:0;font-size:1.1em\">Reading RTC...</p></div>';\n"
    "          } else if (txt.indexOf('Not connected') !== -1) {\n"
    "            el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#ffc107\"><p style=\"margin:0;font-size:1.1em\">RTC not connected</p><p style=\"margin:0.5rem 0 0 0;font-size:0.9em\">Check wiring</p></div>';\n"
    "          } else {\n"
    "            el.innerHTML = '<pre style=\"margin:0;font-size:0.9em;line-height:1.4;color:#333;white-space:pre-wrap\">' + txt.replace(/</g, '&lt;').replace(/>/g, '&gt;') + '</pre>';\n"
    "          }\n"
    "          return txt;\n"
    "        });\n"
    "    })\n"
    "    .catch(function(e) {\n"
    "      console.error('[Sensors] RTC read error', e);\n"
    "      var el2 = document.getElementById('rtc-data');\n"
    "      if (el2) el2.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#dc3545\">Error reading RTC data</div>';\n"
    "      throw e;\n"
    "    });\n"
    "};\n",
    HTTPD_RESP_USE_STRLEN);
}

#endif // DS3231_RTC_SENSOR_WEB_H
