#ifndef MLX90640_THERMAL_SENSOR_WEB_H
#define MLX90640_THERMAL_SENSOR_WEB_H

#include <Arduino.h>
#include "WebServer_Utils.h"

inline void streamMLX90640ThermalSensorCard(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"HTML(

    <div class='sensor-card' id='sensor-card-thermal'>
      <div class='sensor-title'><span>Thermal Camera (MLX90640)</span><span class='status-indicator status-disabled' id='thermal-status-indicator'></span></div>
      <div class='sensor-description'>32x24 thermal infrared camera for temperature imaging.</div>
      <div id='thermal-queue-status' style='display:none;background:#fff3cd;border:1px solid #ffc107;border-radius:4px;padding:8px;margin-bottom:10px;color:#856404;font-size:.9em'></div>
      <div class='sensor-controls'><button class='btn' id='btn-thermal-start'>Start Thermal</button><button class='btn' id='btn-thermal-stop'>Stop Thermal</button></div>
      <div class='sensor-data' id='thermal-data'>
        <div id='thermal-stats' style='color:#333'>Min: <span id='thermalMin'>--</span>&deg;C, Max: <span id='thermalMax'>--</span>&deg;C, Avg: <span id='thermalAvg'>--</span>&deg;C, FPS: <span id='thermalFps'>--</span></div>
        <div id='thermal-performance' style='font-size:.9em;color:#333;margin-top:5px'>Capture: --ms</div>
        <canvas id='thermalCanvas' style='margin-top:10px;width:320px;height:240px;image-rendering:pixelated;border:1px solid #dee2e6;border-radius:4px;background:#000'></canvas>
      </div>
    </div>

)HTML", HTTPD_RESP_USE_STRLEN);
}

inline void streamMLX90640ThermalSensorBindButtons(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "bind('btn-thermal-start','thermalstart');bind('btn-thermal-stop','thermalstop');", HTTPD_RESP_USE_STRLEN);
}

inline void streamMLX90640ThermalSensorJs(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Loading thermal sensor module JS...');}catch(_){ }", HTTPD_RESP_USE_STRLEN);
  
  // Thermal color map functions (verbatim from web_sensors.h)
  httpd_resp_send_chunk(req, "function initThermalColorMap(){console.log('[Thermal] Initializing color map for palette: '+thermalPalette);if(thermalPalette==='iron'){thermalColorMap=getIronColorMap()}else if(thermalPalette==='rainbow'){thermalColorMap=getRainbowColorMap()}else if(thermalPalette==='hot'){thermalColorMap=getHotColorMap()}else if(thermalPalette==='coolwarm'){thermalColorMap=getCoolwarmColorMap()}else{thermalColorMap=getGrayscaleColorMap()}console.log('[Thermal] Color map initialized with '+Object.keys(thermalColorMap).length+' colors')}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function getIronColorMap(){var c={};for(var i=0;i<=255;i++){var r,g,b;if(i<85){r=i*3;g=0;b=0}else if(i<170){r=255;g=(i-85)*3;b=0}else{r=255;g=255;b=(i-170)*3}c[i]='rgb('+Math.min(255,r)+','+Math.min(255,g)+','+Math.min(255,b)+')'}return c}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function getRainbowColorMap(){var c={};for(var i=0;i<=255;i++){var hue=(i/255)*240;var rgb=hslToRgb(hue/360,1,0.5);c[i]='rgb('+rgb[0]+','+rgb[1]+','+rgb[2]+')'}return c}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function getHotColorMap(){var c={};for(var i=0;i<=255;i++){var r=Math.min(255,i*1.5);var g=Math.max(0,Math.min(255,(i-85)*1.5));var b=Math.max(0,Math.min(255,(i-170)*1.5));c[i]='rgb('+Math.round(r)+','+Math.round(g)+','+Math.round(b)+')'}return c}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function getCoolwarmColorMap(){var c={};for(var i=0;i<=255;i++){var t=i/255;var r,g,b;if(t<0.5){r=Math.round(255*(0.23+0.77*(1-2*t)));g=Math.round(255*(0.3+0.7*(1-2*t)));b=Math.round(255*(0.75+0.25*(1-2*t)))}else{r=Math.round(255*(0.7+0.3*(2*t-1)));g=Math.round(255*(0.15+0.35*(2*t-1)));b=Math.round(255*(0.1+0.1*(2*t-1)))}c[i]='rgb('+r+','+g+','+b+')'}return c}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function getGrayscaleColorMap(){var c={};for(var i=0;i<=255;i++){c[i]='rgb('+i+','+i+','+i+')'}return c}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function hslToRgb(h,s,l){var r,g,b;if(s===0){r=g=b=l}else{var hue2rgb=function(p,q,t){if(t<0)t+=1;if(t>1)t-=1;if(t<1/6)return p+(q-p)*6*t;if(t<1/2)return q;if(t<2/3)return p+(q-p)*(2/3-t)*6;return p};var q=l<0.5?l*(1+s):l+s-l*s;var p=2*l-q;r=hue2rgb(p,q,h+1/3);g=hue2rgb(p,q,h);b=hue2rgb(p,q,h-1/3)}return [Math.round(r*255),Math.round(g*255),Math.round(b*255)]}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function applyThermalPalette(p){switch(p){case'iron':thermalColorMap=getIronColorMap();break;case'rainbow':thermalColorMap=getRainbowColorMap();break;case'hot':thermalColorMap=getHotColorMap();break;case'coolwarm':thermalColorMap=getCoolwarmColorMap();break;case'grayscale':default:thermalColorMap=getGrayscaleColorMap();break}console.log('[Thermal] Applied palette:',p)}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "thermalColorMap=getGrayscaleColorMap();", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req,
    "function updateThermalVisualization() {\n"
    "  var url = '/api/sensors?sensor=thermal&ts=' + Date.now();\n"
    "  debugLog('http', 'GET ' + url);\n"
    "  fetch(url, {cache: 'no-store'})\n"
    "    .then(function(r) {\n"
    "      if (!r.ok) throw new Error('HTTP ' + r.status);\n"
    "      return r.json();\n"
    "    })\n"
    "    .then(function(d) {\n"
    "      console.log('[Thermal] Received data:', d);\n"
    "      if (d && d.v && d.data) {\n"
    "        console.log('[Thermal] Valid data, rendering...');\n"
    "        var isInterpolated = (d.w === 64 && d.h === 48);\n"
    "        var frame = isInterpolated ? d.data : d.data.map(function(val) { return val / 100.0; });\n"
    "        var min = d.mn || 0;\n"
    "        var max = d.mx || 100;\n"
    "        var avg = frame.reduce(function(a, b) { return a + b; }, 0) / frame.length;\n"
    "        var s = function(id, v) {\n"
    "          var el = document.getElementById(id);\n"
    "          if (el) el.textContent = v;\n"
    "        };\n"
    "        s('thermalMin', min.toFixed(1));\n"
    "        s('thermalMax', max.toFixed(1));\n"
    "        s('thermalAvg', avg.toFixed(1));\n"
    "        var cv = document.getElementById('thermalCanvas');\n"
    "        if (!cv) {\n"
    "          console.error('[Thermal] Canvas not found');\n"
    "          return;\n"
    "        }\n"
    "        var ctx = cv.getContext('2d');\n"
    "        var proc = frame.slice();\n"
    "        if (thermalPreviousFrame && thermalEWMAFactor > 0) {\n"
    "          for (var i = 0; i < frame.length; i++) {\n"
    "            proc[i] = thermalEWMAFactor * frame[i] + (1 - thermalEWMAFactor) * thermalPreviousFrame[i];\n"
    "          }\n"
    "        }\n"
    "        thermalPreviousFrame = frame.slice();\n"
    "        var w = d.w || 32, h = d.h || 24;\n"
    "        cv.width = w;\n"
    "        cv.height = h;\n"
    "        var img = ctx.createImageData(w, h);\n"
    "        for (var i = 0; i < proc.length; i++) {\n"
    "          var t = proc[i];\n"
    "          var norm = (t - min) / (max - min);\n"
    "          if (norm < 0) norm = 0;\n"
    "          if (norm > 1) norm = 1;\n"
    "          var idx = Math.round(255 * norm);\n"
    "          var col = thermalColorMap[idx] || 'rgb(128,128,128)';\n"
    "          var rgb = col.match(/\\d+/g);\n"
    "          if (!rgb || rgb.length < 3) rgb = ['128', '128', '128'];\n"
    "          var p = i * 4;\n"
    "          img.data[p] = parseInt(rgb[0]);\n"
    "          img.data[p + 1] = parseInt(rgb[1]);\n"
    "          img.data[p + 2] = parseInt(rgb[2]);\n"
    "          img.data[p + 3] = 255;\n"
    "        }\n"
    "        ctx.putImageData(img, 0, 0);\n"
    "        console.log('[Thermal] Rendered frame');\n"
    "      } else {\n"
    "        console.warn('[Thermal] Invalid data format:', d);\n"
    "      }\n"
    "    })\n"
    "    .catch(function(e) {\n"
    "      console.error('Thermal fetch error:', e);\n"
    "    });\n"
    "}\n", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function startThermalPolling(){console.log('[SENSORS] startThermalPolling called');if(thermalPollingInterval){console.log('[SENSORS] Thermal already polling');return}updateThermalVisualization();thermalPollingInterval=setInterval(function(){updateThermalVisualization()},thermalPollingMs);console.log('[SENSORS] Thermal polling started with interval:',thermalPollingMs+'ms')}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function stopThermalPolling(){console.log('[SENSORS] stopThermalPolling called');if(thermalPollingInterval){clearInterval(thermalPollingInterval);thermalPollingInterval=null;console.log('[SENSORS] Thermal polling stopped')}}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "try{console.log('[SENSORS] Chunk 4: Thermal functions ready');}catch(_){ }", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
}

inline void streamMLX90640ThermalDashboardDef(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "window.__dashSensorDefs.push({device:'MLX90640',key:'thermal',name:'Thermal (MLX90640)',desc:'32x24 IR Camera'});", HTTPD_RESP_USE_STRLEN);
}

#endif // MLX90640_THERMAL_SENSOR_WEB_H
