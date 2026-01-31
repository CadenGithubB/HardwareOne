#ifndef VL53L4CX_TOF_SENSOR_WEB_H
#define VL53L4CX_TOF_SENSOR_WEB_H

#include <Arduino.h>
#include "WebServer_Utils.h"

inline void streamVL53L4CXTofSensorCard(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"HTML(

    <div class='sensor-card' id='sensor-card-tof'>
      <div class='sensor-title'><span>ToF Distance Sensor</span><span class='status-indicator status-disabled' id='tof-status-indicator'></span></div>
      <div class='sensor-description'>VL53L4CX Time-of-Flight sensor up to ~4m.</div>
      <div id='tof-queue-status' style='display:none;background:#fff3cd;border:1px solid #ffc107;border-radius:4px;padding:8px;margin-bottom:10px;color:#856404;font-size:.9em'></div>
      <div class='sensor-controls'><button class='btn' id='btn-tof-start'>Open ToF</button><button class='btn' id='btn-tof-stop'>Close ToF</button></div>
      <div class='sensor-data' id='tof-data'>ToF sensor data will appear here...</div>
      <div id='tof-objects-display' style='margin-top:15px;display:none'>
        <div style='font-weight:bold;margin-bottom:10px;color:#333'>Multi-Object Detection (0-<span id='tof-range-mm'>3400</span>mm)</div>
        <div class='tof-objects-container'>
          <div class='tof-object-row' id='tof-object-1'><div class='object-label'>Object 1:</div><div class='distance-bar-container'><div class='distance-bar' id='distance-bar-1'></div></div><div class='object-info' id='object-info-1'>---</div></div>
          <div class='tof-object-row' id='tof-object-2'><div class='object-label'>Object 2:</div><div class='distance-bar-container'><div class='distance-bar' id='distance-bar-2'></div></div><div class='object-info' id='object-info-2'>---</div></div>
          <div class='tof-object-row' id='tof-object-3'><div class='object-label'>Object 3:</div><div class='distance-bar-container'><div class='distance-bar' id='distance-bar-3'></div></div><div class='object-info' id='object-info-3'>---</div></div>
          <div class='tof-object-row' id='tof-object-4'><div class='object-label'>Object 4:</div><div class='distance-bar-container'><div class='distance-bar' id='distance-bar-4'></div></div><div class='object-info' id='object-info-4'>---</div></div>
        </div>
        <div id='tof-objects-summary' style='font-size:.9em;color:#212529;text-align:center;margin-top:10px;padding:8px;background:#e3f2fd;border-radius:4px;font-weight:500'>Multi-object detection ready...</div>
      </div>
    </div>

)HTML", HTTPD_RESP_USE_STRLEN);
}

inline void streamVL53L4CXTofSensorBindButtons(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "bind('btn-tof-start','opentof');bind('btn-tof-stop','closetof');", HTTPD_RESP_USE_STRLEN);
}

inline void streamVL53L4CXTofSensorJs(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Chunk 5: ToF functions start');}catch(_){ }", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req,
    "function updateToFObjects() {\n"
    "  var url = '/api/sensors?sensor=tof&ts=' + Date.now();\n"
    "  debugLog('http', 'GET ' + url);\n"
    "  fetch(url, {cache: 'no-store'})\n"
    "    .then(function(r) {\n"
    "      if (!r.ok) throw new Error('HTTP ' + r.status);\n"
    "      return r.json();\n"
    "    })\n"
    "    .then(function(d) {\n"
    "      if (d && d.objects) {\n"
    "        var valid = 0;\n"
    "        for (var i = 0; i < 4; i++) {\n"
    "          var obj = d.objects[i];\n"
    "          var bar = document.getElementById('distance-bar-' + (i + 1));\n"
    "          var info = document.getElementById('object-info-' + (i + 1));\n"
    "          var st = tofObjectStates[i];\n"
    "          if (obj && obj.detected && obj.valid) {\n"
    "            var mm = obj.distance_mm || 0;\n"
    "            var cm = obj.distance_cm || 0;\n"
    "            if (!st.lastDistance || Math.abs(st.lastDistance - mm) < 200) {\n"
    "              st.stableCount = (st.stableCount || 0) + 1;\n"
    "              st.lastDistance = mm;\n"
    "              if (st.stableCount >= tofStabilityThreshold) {\n"
    "                valid++;\n"
    "                var pct = Math.min(100, (mm / tofMaxDistance) * 100);\n"
    "                if (tofTransitionMs > 0) {\n"
    "                  bar.style.transition = 'width ' + tofTransitionMs + 'ms ease-in-out, background-color ' + tofTransitionMs + 'ms ease-in-out';\n"
    "                }\n"
    "                bar.style.width = pct + '%';\n"
    "                bar.className = 'distance-bar';\n"
    "                info.textContent = cm.toFixed(1) + ' cm';\n"
    "                st.displayed = true;\n"
    "              }\n"
    "            } else {\n"
    "              st.stableCount = 1;\n"
    "              st.lastDistance = mm;\n"
    "            }\n"
    "          } else {\n"
    "            st.stableCount = 0;\n"
    "            if (st.displayed) {\n"
    "              st.missCount = (st.missCount || 0) + 1;\n"
    "              if (st.missCount >= tofStabilityThreshold) {\n"
    "                if (tofTransitionMs > 0) {\n"
    "                  bar.style.transition = 'width ' + tofTransitionMs + 'ms ease-in-out, background-color ' + tofTransitionMs + 'ms ease-in-out';\n"
    "                }\n"
    "                bar.style.width = '0%';\n"
    "                bar.className = 'distance-bar invalid';\n"
    "                info.textContent = '---';\n"
    "                st.displayed = false;\n"
    "                st.missCount = 0;\n"
    "              }\n"
    "            } else {\n"
    "              if (tofTransitionMs > 0) {\n"
    "                bar.style.transition = 'width ' + tofTransitionMs + 'ms ease-in-out, background-color ' + tofTransitionMs + 'ms ease-in-out';\n"
    "              }\n"
    "              bar.style.width = '0%';\n"
    "              bar.className = 'distance-bar invalid';\n"
    "              info.textContent = '---';\n"
    "            }\n"
    "          }\n"
    "        }\n"
    "        var sum = document.getElementById('tof-objects-summary');\n"
    "        if (sum) {\n"
    "          sum.textContent = valid + ' object(s) detected';\n"
    "        }\n"
    "      }\n"
    "    })\n"
    "    .catch(function(e) {\n"
    "      console.error('[ToF] Fetch error:', e);\n"
    "    });\n"
    "}\n", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function startToFPolling(){console.log('[SENSORS] startToFPolling called');if(tofPollingInterval){console.log('[SENSORS] ToF already polling');return}var d=document.getElementById('tof-objects-display');if(d)d.style.display='block';var ph=document.getElementById('tof-data');if(ph)ph.style.display='none';updateToFObjects();tofPollingInterval=setInterval(function(){updateToFObjects()},tofPollingMs);console.log('[SENSORS] ToF polling started with interval:',tofPollingMs+'ms')}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function stopToFPolling(){console.log('[SENSORS] stopToFPolling called');if(tofPollingInterval){clearInterval(tofPollingInterval);tofPollingInterval=null;console.log('[SENSORS] ToF polling stopped')}var d=document.getElementById('tof-objects-display');if(d)d.style.display='none';var ph=document.getElementById('tof-data');if(ph){ph.textContent='ToF sensor data will appear here...';ph.style.display=''}}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Chunk 5: ToF functions ready');}catch(_){ }", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
}

inline void streamVL53L4CXTofDashboardDef(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "window.__dashSensorDefs.push({device:'VL53L4CX',key:'tof',name:'ToF (VL53L4CX)',desc:'Distance Measurement'});", HTTPD_RESP_USE_STRLEN);
}

#endif // VL53L4CX_TOF_SENSOR_WEB_H
