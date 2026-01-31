#ifndef PA1010D_GPS_SENSOR_WEB_H
#define PA1010D_GPS_SENSOR_WEB_H

#include <Arduino.h>
#include "WebServer_Utils.h"

inline void streamPA1010DGpsSensorCard(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"HTML(

    <div class='sensor-card' id='sensor-card-gps'>
      <div class='sensor-title'><span>GPS Module (PA1010D)</span><span class='status-indicator status-disabled' id='gps-status-indicator'></span></div>
      <div class='sensor-description'>Mini GPS module for location, time, and satellite data.</div>
      <div id='gps-queue-status' style='display:none;background:#fff3cd;border:1px solid #ffc107;border-radius:4px;padding:8px;margin-bottom:10px;color:#856404;font-size:.9em'></div>
      <div class='sensor-controls'><button class='btn' id='btn-gps-start'>Open GPS</button><button class='btn' id='btn-gps-stop'>Close GPS</button></div>
      <div class='sensor-data' id='gps-data'><div style="padding:1rem;text-align:center;color:#6c757d"><p style="margin:0;font-size:1.1em">GPS Closed</p><p style="margin:0.5rem 0 0 0;font-size:0.9em">Click "Open GPS" to begin</p></div></div>
    </div>

)HTML", HTTPD_RESP_USE_STRLEN);
}

inline void streamPA1010DGpsSensorBindButtons(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "bind('btn-gps-start','opengps');bind('btn-gps-stop','closegps');", HTTPD_RESP_USE_STRLEN);
}

inline void streamPA1010DGpsDashboardDef(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "window.__dashSensorDefs.push({device:'PA1010D',key:'gps',name:'GPS (PA1010D)',desc:'Location & Time'});", HTTPD_RESP_USE_STRLEN);
}

inline void streamPA1010DGpsSensorJs(httpd_req_t* req) {
  httpd_resp_send_chunk(req,
    "window._sensorReaders = window._sensorReaders || {};\n"
    "window._sensorDataIds = window._sensorDataIds || {};\n"
    "window._sensorPollingIntervals = window._sensorPollingIntervals || {};\n"
    "window._sensorDataIds.gps = 'gps-data';\n"
    "window._sensorPollingIntervals.gps = 1000;\n"
    "window._sensorReaders.gps = function() {\n"
    "  return fetch('/api/sensors/status', {cache: 'no-store', credentials: 'include'})\n"
    "    .then(function(r) { return r.json(); })\n"
    "    .then(function(status) {\n"
    "      var el = document.getElementById('gps-data');\n"
    "      if (!el) return;\n"
    "      if (!status.gpsCompiled) {\n"
    "        el.textContent = 'GPS error: not_compiled';\n"
    "        return 'not_compiled';\n"
    "      }\n"
    "      if (!status.gpsEnabled) {\n"
    "        el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#6c757d\"><p style=\"margin:0;font-size:1.1em\">GPS Closed</p><p style=\"margin:0.5rem 0 0 0;font-size:0.9em\">Click \"Open\" to begin</p></div>';\n"
    "        return 'stopped';\n"
    "      }\n"
    "      return fetch('/api/cli', {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body: 'cmd=gps', cache: 'no-store', credentials: 'include'})\n"
    "        .then(function(r) { return r.text(); })\n"
    "        .then(function(txt) {\n"
    "          if (!txt || txt.trim() === '' || txt === 'OK') {\n"
    "            el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#ffc107\"><p style=\"margin:0;font-size:1.1em\">Searching for satellites...</p><p style=\"margin:0.5rem 0 0 0;font-size:0.9em\">GPS is running but has no fix yet</p><p style=\"margin:0.5rem 0 0 0;font-size:0.85em;color:#6c757d\">Ensure device has clear view of sky</p></div>';\n"
    "          } else if (txt.indexOf('not initialized') !== -1) {\n"
    "            el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#17a2b8\"><p style=\"margin:0;font-size:1.1em\">Initializing GPS...</p><p style=\"margin:0.5rem 0 0 0;font-size:0.9em\">GPS module is starting up</p><p style=\"margin:0.5rem 0 0 0;font-size:0.85em;color:#6c757d\">This may take a few seconds</p></div>';\n"
    "          } else if (txt.indexOf('No GPS fix') !== -1 || txt.indexOf('waiting for satellites') !== -1) {\n"
    "            el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#ffc107\"><p style=\"margin:0;font-size:1.1em\">Searching for satellites...</p><p style=\"margin:0.5rem 0 0 0;font-size:0.9em\">Satellites: ' + (txt.match(/Satellites: (\\d+)/) ? txt.match(/Satellites: (\\d+)/)[1] : '0') + '</p><p style=\"margin:0.5rem 0 0 0;font-size:0.85em;color:#6c757d\">Waiting for GPS fix...</p></div>';\n"
    "          } else {\n"
    "            el.innerHTML = '<pre style=\"margin:0;font-size:0.9em;line-height:1.4;color:#333;white-space:pre-wrap\">' + txt.replace(/</g, '&lt;').replace(/>/g, '&gt;') + '</pre>';\n"
    "          }\n"
    "          return txt;\n"
    "        });\n"
    "    })\n"
    "    .catch(function(e) {\n"
    "      console.error('[Sensors] GPS read error', e);\n"
    "      var el2 = document.getElementById('gps-data');\n"
    "      if (el2) el2.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#dc3545\">Error reading GPS data</div>';\n"
    "      throw e;\n"
    "    });\n"
    "};\n",
    HTTPD_RESP_USE_STRLEN);
}

#endif // PA1010D_GPS_SENSOR_WEB_H
