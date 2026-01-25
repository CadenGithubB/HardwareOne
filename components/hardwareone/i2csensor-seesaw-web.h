#ifndef SEESAW_GAMEPAD_SENSOR_WEB_H
#define SEESAW_GAMEPAD_SENSOR_WEB_H

#include <Arduino.h>
#include "WebServer_Utils.h"

inline void streamSeesawGamepadSensorCard(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"HTML(

    <div class='sensor-card' id='sensor-card-gamepad'>
      <div class='sensor-title'><span>Gamepad (Seesaw)</span><span class='status-indicator status-disabled' id='gamepad-status-indicator'></span></div>
      <div class='sensor-description'>Mini I2C Gamepad with joystick and buttons.</div>
      <div id='gamepad-queue-status' style='display:none;background:#fff3cd;border:1px solid #ffc107;border-radius:4px;padding:8px;margin-bottom:10px;color:#856404;font-size:.9em'></div>
      <div class='sensor-controls'><button class='btn' id='btn-gamepad-start'>Start Gamepad</button><button class='btn' id='btn-gamepad-stop'>Stop Gamepad</button></div>
      <div class='sensor-data' id='gamepad-data'>Gamepad data will appear here...</div>
      <div class='gamepad-row' style='margin-top:10px'>
        <div class='joy-wrap'><canvas id='gamepad-joystick' class='joy-canvas' width='100' height='100'></canvas></div>
        <div class='abxy-grid'>
          <div></div>
          <div id='btn-x' class='btn btn-small' style='width:36px;font-size:0.75rem;padding:4px'>X</div>
          <div></div>
          <div id='btn-y' class='btn btn-small' style='width:36px;font-size:0.75rem;padding:4px'>Y</div>
          <div></div>
          <div id='btn-a' class='btn btn-small' style='width:36px;font-size:0.75rem;padding:4px'>A</div>
          <div></div>
          <div id='btn-b' class='btn btn-small' style='width:36px;font-size:0.75rem;padding:4px'>B</div>
          <div></div>
        </div>
        <div style='display:flex;flex-direction:column;gap:4px;margin-left:8px'>
          <div id='btn-select' class='btn btn-small' style='width:50px;font-size:0.65rem;padding:4px'>Sel</div>
          <div id='btn-start' class='btn btn-small' style='width:50px;font-size:0.65rem;padding:4px'>Start</div>
        </div>
      </div>
    </div>

)HTML", HTTPD_RESP_USE_STRLEN);
}

inline void streamSeesawGamepadSensorBindButtons(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "bind('btn-gamepad-start','gamepadstart');bind('btn-gamepad-stop','gamepadstop');", HTTPD_RESP_USE_STRLEN);
}

inline void streamSeesawGamepadSensorJs(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req,
    "window.hwRenderGamepadState = function(j, ids) {\n"
    "  try {\n"
    "    ids = ids || {};\n"
    "    if (!j || typeof j !== 'object') return;\n"
    "    var hasXYZ = (j.x !== undefined && j.y !== undefined && j.buttons !== undefined);\n"
    "    var isOk = false;\n"
    "    if (j.v !== undefined) isOk = !!j.v;\n"
    "    else if (j.valid !== undefined) isOk = !!j.valid;\n"
    "    else if (j.ok !== undefined) isOk = !!j.ok;\n"
    "    else isOk = hasXYZ;\n"
    "    if (!isOk && !hasXYZ) return;\n"
    "    var x = j.x, y = j.y, b = j.buttons;\n"
    "    var el = document.getElementById(ids.data || 'gamepad-data');\n"
    "    if (el) {\n"
    "      var bHex = '0x' + ((b >>> 0) & 0xFFFF).toString(16).toUpperCase().padStart(4, '0');\n"
    "      el.textContent = 'X: ' + x + '  Y: ' + y + '  Buttons: ' + bHex;\n"
    "    }\n"
    "    var pins = {x: 6, y: 2, a: 5, b: 1, select: 0, start: 16};\n"
    "    function setBtn(id, p) {\n"
    "      var e = document.getElementById(id);\n"
    "      if (!e) return;\n"
    "      e.style.background = p ? '#28a745' : '#f8f9fa';\n"
    "      e.style.color = p ? '#fff' : '#333';\n"
    "    }\n"
    "    setBtn(ids.btnX || 'btn-x', ((b & (1 << pins.x)) === 0));\n"
    "    setBtn(ids.btnY || 'btn-y', ((b & (1 << pins.y)) === 0));\n"
    "    setBtn(ids.btnA || 'btn-a', ((b & (1 << pins.a)) === 0));\n"
    "    setBtn(ids.btnB || 'btn-b', ((b & (1 << pins.b)) === 0));\n"
    "    setBtn(ids.btnSelect || 'btn-select', ((b & (1 << pins.select)) === 0));\n"
    "    setBtn(ids.btnStart || 'btn-start', ((b & (1 << pins.start)) === 0));\n"
    "    try {\n"
    "      var cv = document.getElementById(ids.joystick || 'gamepad-joystick');\n"
    "      if (cv) {\n"
    "        var ctx = cv.getContext('2d');\n"
    "        var w = cv.width, h = cv.height;\n"
    "        var cx = w / 2, cy = h / 2;\n"
    "        ctx.clearRect(0, 0, w, h);\n"
    "        ctx.strokeStyle = '#ddd';\n"
    "        ctx.lineWidth = 2;\n"
    "        ctx.beginPath();\n"
    "        ctx.arc(cx, cy, cx - 10, 0, 2 * Math.PI);\n"
    "        ctx.stroke();\n"
    "        ctx.strokeStyle = '#ccc';\n"
    "        ctx.beginPath();\n"
    "        ctx.moveTo(cx, 10);\n"
    "        ctx.lineTo(cx, h - 10);\n"
    "        ctx.moveTo(10, cy);\n"
    "        ctx.lineTo(w - 10, cy);\n"
    "        ctx.stroke();\n"
    "        var dx = x - 512, dy = y - 512;\n"
    "        var deadzone = 30;\n"
    "        if (Math.abs(dx) < deadzone) dx = 0;\n"
    "        if (Math.abs(dy) < deadzone) dy = 0;\n"
    "        var jx = cx + (dx / 512.0) * (cx - 10);\n"
    "        var jy = cy - (dy / 512.0) * (cy - 10);\n"
    "        ctx.fillStyle = '#007bff';\n"
    "        ctx.beginPath();\n"
    "        ctx.arc(jx, jy, 8, 0, 2 * Math.PI);\n"
    "        ctx.fill();\n"
    "      }\n"
    "    } catch (_) {}\n"
    "  } catch (_) {}\n"
    "};\n",
    HTTPD_RESP_USE_STRLEN);
  
  // Gamepad sensor reader - register in window._sensorReaders
  httpd_resp_send_chunk(req,
    "window._sensorReaders = window._sensorReaders || {};\n"
    "window._sensorReaders.gamepad = function() {\n"
    "    var url = '/api/sensors?sensor=gamepad&ts=' + Date.now();\n"
    "    return fetch(url, {cache: 'no-store', credentials: 'include'})\n"
    "      .then(function(r) {\n"
    "        return r.json();\n"
    "      })\n"
    "      .then(function(j) {\n"
    "        try {\n"
    "          if (typeof window.hwRenderGamepadState === 'function') {\n"
    "            window.hwRenderGamepadState(j, {data:'gamepad-data', joystick:'gamepad-joystick', btnX:'btn-x', btnY:'btn-y', btnA:'btn-a', btnB:'btn-b', btnSelect:'btn-select', btnStart:'btn-start'});\n"
    "          }\n"
    "        } catch (_) {}\n"
    "        return j;\n"
    "      })\n"
    "      .catch(function(e) {\n"
    "        console.error('[Sensors] Gamepad read error', e);\n"
    "        throw e;\n"
    "      });\n"
    "};\n", HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
}

inline void streamSeesawGamepadDashboardDef(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "window.__dashSensorDefs.push({device:'Seesaw',key:'gamepad',name:'Gamepad (Seesaw)',desc:'Joystick + buttons'});", HTTPD_RESP_USE_STRLEN);
}

#endif // SEESAW_GAMEPAD_SENSOR_WEB_H
