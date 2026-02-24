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
      <div class='sensor-data' id='gps-data'><div style="padding:1rem;text-align:center;color:var(--panel-fg)"><p style="margin:0;font-size:1.1em">GPS Closed</p><p style="margin:0.5rem 0 0 0;font-size:0.9em">Click "Open GPS" to begin</p></div></div>
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
    "        el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:var(--panel-fg)\"><p style=\"margin:0;font-size:1.1em\">GPS Closed</p><p style=\"margin:0.5rem 0 0 0;font-size:0.9em\">Click \"Open\" to begin</p></div>';\n"
    "        return 'stopped';\n"
    "      }\n"
    "      return fetch('/api/sensors?sensor=gps&ts=' + Date.now(), {cache: 'no-store', credentials: 'include'})\n"
    "        .then(function(r) { return r.json(); })\n"
    "        .then(function(data) {\n"
    "          if (data.error) {\n"
    "            if (data.error === 'no_data') {\n"
    "              el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#17a2b8\"><p style=\"margin:0;font-size:1.1em\">Initializing GPS...</p><p style=\"margin:0.5rem 0 0 0;font-size:0.9em\">Waiting for first data</p></div>';\n"
    "            } else {\n"
    "              el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#dc3545\"><p style=\"margin:0;font-size:1.1em\">GPS Error</p><p style=\"margin:0.5rem 0 0 0;font-size:0.9em\">' + data.error + '</p></div>';\n"
    "            }\n"
    "            return data;\n"
    "          }\n"
    "          if (!data.fix) {\n"
    "            el.innerHTML = '<div style=\"padding:1rem;text-align:center;color:#ffc107\"><p style=\"margin:0;font-size:1.1em\">Searching for satellites...</p><p style=\"margin:0.5rem 0 0 0;font-size:0.9em\">Satellites: ' + (data.satellites || 0) + '</p><p style=\"margin:0.5rem 0 0 0;font-size:0.85em;color:var(--panel-fg)\">Waiting for GPS fix...</p></div>';\n"
    "          } else {\n"
    "            var html = '<div style=\"padding:0.5rem;font-size:0.9em;line-height:1.6\">';\n"
    "            html += '<p style=\"margin:0.25rem 0;color:#28a745;font-weight:bold\">✓ GPS FIX (Quality: ' + data.quality + ')</p>';\n"
    "            html += '<p style=\"margin:0.25rem 0\"><strong>Satellites:</strong> ' + data.satellites + '</p>';\n"
    "            html += '<p style=\"margin:0.25rem 0\"><strong>Latitude:</strong> ' + data.latitude.toFixed(6) + '</p>';\n"
    "            html += '<p style=\"margin:0.25rem 0\"><strong>Longitude:</strong> ' + data.longitude.toFixed(6) + '</p>';\n"
    "            html += '<p style=\"margin:0.25rem 0\"><strong>Altitude:</strong> ' + data.altitude.toFixed(1) + ' m</p>';\n"
    "            html += '<p style=\"margin:0.25rem 0\"><strong>Speed:</strong> ' + data.speed.toFixed(1) + ' knots</p>';\n"
    "            html += '<p style=\"margin:0.25rem 0\"><strong>Time:</strong> ' + data.time + ' UTC</p>';\n"
    "            html += '<p style=\"margin:0.25rem 0\"><strong>Date:</strong> ' + data.date + '</p>';\n"
    "            html += '</div>';\n"
    "            el.innerHTML = html;\n"
    "          }\n"
    "          return data;\n"
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
