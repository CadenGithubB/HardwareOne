#ifndef I2CSENSOR_ANO_ENCODER_WEB_H
#define I2CSENSOR_ANO_ENCODER_WEB_H

#include <Arduino.h>
#include "WebServer_Utils.h"

// ANO Rotary Encoder sensor card. Mirrors the structure of the other
// i2csensor_*_web.h files (own HTML card + JS reader + button binders +
// dashboard def) — included only when ENABLE_ANO_ENCODER is set.
//
// Card layout:
//   - Status indicator + open/close buttons
//   - Encoder position (signed integer) + axis indicator (V/H)
//   - 5-button D-pad in physical cross layout (UP/LEFT/IN/RIGHT/DOWN)
//   - START virtual button (synthesized from RIGHT+IN chord)
//
// API endpoint: /api/sensors?sensor=anoencoder → JSON from
// anoEncoderBuildDataJSON() — {"valid":..,"connected":..,"ts":..,"pos":N,"axis":0|1,"buttons":B}

inline void streamAnoEncoderSensorCard(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"HTML(

    <div class='sensor-card' id='sensor-card-ano-encoder'>
      <div class='sensor-title'><span>ANO Encoder (Seesaw)</span><span class='status-indicator status-disabled' id='ano-status-indicator'></span></div>
      <div class='sensor-description'>Adafruit ANO Rotary Encoder breakout — rotary wheel + 5-button D-pad with RIGHT+IN chord for START.</div>
      <div id='ano-queue-status' style='display:none;background:var(--warning-bg);border:1px solid var(--warning-border);border-left:4px solid var(--warning-accent);border-radius:4px;padding:8px;margin-bottom:10px;color:var(--warning-fg);font-size:.9em'></div>
      <div class='sensor-controls'><button class='btn' id='btn-ano-start'>Open Encoder</button><button class='btn' id='btn-ano-stop'>Close Encoder</button></div>
      <div class='sensor-data' id='ano-data'>Encoder data will appear here...</div>
      <div class='ano-row' style='margin-top:10px;display:flex;gap:16px;align-items:center;flex-wrap:wrap'>
        <div style='display:flex;flex-direction:column;align-items:center;gap:4px'>
          <canvas id='ano-wheel' width='100' height='100' style='border-radius:50%;background:var(--menu-item-bg)'></canvas>
          <div style='font-size:0.75rem;color:var(--text-muted)'>Position: <span id='ano-pos'>0</span></div>
          <div style='font-size:0.75rem;color:var(--text-muted)'>Axis: <span id='ano-axis'>vertical</span></div>
        </div>
        <div style='display:grid;grid-template-columns:repeat(3,36px);grid-template-rows:repeat(3,36px);gap:4px'>
          <div></div>
          <div id='ano-btn-up' class='btn btn-small' style='font-size:0.7rem;padding:4px'>UP</div>
          <div></div>
          <div id='ano-btn-left' class='btn btn-small' style='font-size:0.7rem;padding:4px'>LEFT</div>
          <div id='ano-btn-in' class='btn btn-small' style='font-size:0.7rem;padding:4px;background:var(--menu-item-bg)'>IN</div>
          <div id='ano-btn-right' class='btn btn-small' style='font-size:0.7rem;padding:4px'>RIGHT</div>
          <div></div>
          <div id='ano-btn-down' class='btn btn-small' style='font-size:0.7rem;padding:4px'>DOWN</div>
          <div></div>
        </div>
        <div style='display:flex;flex-direction:column;gap:4px'>
          <div id='ano-btn-start' class='btn btn-small' style='font-size:0.65rem;padding:4px'>START</div>
        </div>
      </div>
    </div>

)HTML", HTTPD_RESP_USE_STRLEN);
}

inline void streamAnoEncoderSensorBindButtons(httpd_req_t* req) {
  // Unified open/close commands work for the ANO under the input HAL.
  httpd_resp_send_chunk(req, "bind('btn-ano-start','openinput');bind('btn-ano-stop','closeinput');", HTTPD_RESP_USE_STRLEN);
}

inline void streamAnoEncoderSensorJs(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);

  // Renderer: takes ANO JSON {valid,connected,ts,pos,axis,buttons} and updates the card UI.
  // Button bit layout matches i2csensor_ano_encoder.h ANO_BTN_* constants:
  //   IN=bit 0, UP=bit 1, DOWN=bit 2, LEFT=bit 3, RIGHT=bit 4
  //   Virtual START=bit 16 (synthesized from RIGHT+IN chord by the driver).
  httpd_resp_send_chunk(req,
    "window.hwRenderAnoState = function(j, ids) {\n"
    "  try {\n"
    "    if (!j || typeof j !== 'object') return;\n"
    "    ids = ids || {};\n"
    "    var idData=ids.data||'ano-data', idPos=ids.pos||'ano-pos', idAxis=ids.axis||'ano-axis', idWheel=ids.wheel||'ano-wheel';\n"
    "    var idIn=ids.btnIn||'ano-btn-in', idUp=ids.btnUp||'ano-btn-up', idDown=ids.btnDown||'ano-btn-down', idLeft=ids.btnLeft||'ano-btn-left', idRight=ids.btnRight||'ano-btn-right', idStart=ids.btnStart||'ano-btn-start';\n"
    "    var hasData = (j.pos !== undefined && j.buttons !== undefined);\n"
    "    if (!hasData) return;\n"
    "    var pos = j.pos|0, axis = j.axis|0, b = j.buttons>>>0;\n"
    "    var dataEl = hw.$(idData);\n"
    "    if (dataEl) {\n"
    "      var bHex = '0x' + (b & 0xFFFF).toString(16).toUpperCase().padStart(4,'0');\n"
    "      dataEl.textContent = 'Pos: ' + pos + '  Axis: ' + (axis ? 'horizontal' : 'vertical') + '  Buttons: ' + bHex;\n"
    "    }\n"
    "    var posEl = hw.$(idPos);\n"
    "    if (posEl) posEl.textContent = pos;\n"
    "    var axisEl = hw.$(idAxis);\n"
    "    if (axisEl) axisEl.textContent = axis ? 'horizontal ↔' : 'vertical ↕';\n"
    "    // Buttons stored active-high in gAnoEncoderCache (bit set = pressed)\n"
    "    var pins = {in:0, up:1, down:2, left:3, right:4, start:16};\n"
    "    function setBtn(id, p) {\n"
    "      var e = hw.$(id);\n"
    "      if (!e) return;\n"
    "      e.style.background = p ? 'var(--success)' : 'var(--menu-item-bg)';\n"
    "      e.style.color = p ? '#fff' : 'var(--menu-item-fg)';\n"
    "    }\n"
    "    setBtn(idIn,    !!(b & (1<<pins.in)));\n"
    "    setBtn(idUp,    !!(b & (1<<pins.up)));\n"
    "    setBtn(idDown,  !!(b & (1<<pins.down)));\n"
    "    setBtn(idLeft,  !!(b & (1<<pins.left)));\n"
    "    setBtn(idRight, !!(b & (1<<pins.right)));\n"
    "    setBtn(idStart, !!(b & (1<<pins.start)));\n"
    "    // Rotary wheel visual — draw a dot at the angle implied by pos\n"
    "    try {\n"
    "      var cv = hw.$(idWheel);\n"
    "      if (cv) {\n"
    "        var ctx = cv.getContext('2d');\n"
    "        var w = cv.width, h = cv.height;\n"
    "        var cx = w/2, cy = h/2;\n"
    "        var cs = getComputedStyle(document.documentElement);\n"
    "        var strokeCol = cs.getPropertyValue('--panel-fg').trim() || '#888';\n"
    "        var linkCol = cs.getPropertyValue('--link').trim() || '#007bff';\n"
    "        ctx.clearRect(0,0,w,h);\n"
    "        ctx.strokeStyle = strokeCol;\n"
    "        ctx.lineWidth = 2;\n"
    "        ctx.beginPath();\n"
    "        ctx.arc(cx, cy, cx-8, 0, 2*Math.PI);\n"
    "        ctx.stroke();\n"
    "        // 12 tick marks around the rim, every 30°\n"
    "        ctx.strokeStyle = strokeCol;\n"
    "        ctx.lineWidth = 1;\n"
    "        for (var i = 0; i < 12; i++) {\n"
    "          var a = (i/12)*2*Math.PI - Math.PI/2;\n"
    "          ctx.beginPath();\n"
    "          ctx.moveTo(cx + Math.cos(a)*(cx-12), cy + Math.sin(a)*(cy-12));\n"
    "          ctx.lineTo(cx + Math.cos(a)*(cx-4),  cy + Math.sin(a)*(cy-4));\n"
    "          ctx.stroke();\n"
    "        }\n"
    "        // Position indicator — 24 detents/revolution feels right\n"
    "        var ang = ((pos % 24) / 24) * 2*Math.PI - Math.PI/2;\n"
    "        ctx.fillStyle = linkCol;\n"
    "        ctx.beginPath();\n"
    "        ctx.arc(cx + Math.cos(ang)*(cx-14), cy + Math.sin(ang)*(cy-14), 6, 0, 2*Math.PI);\n"
    "        ctx.fill();\n"
    "      }\n"
    "    } catch (_) {}\n"
    "  } catch (_) {}\n"
    "};\n",
    HTTPD_RESP_USE_STRLEN);

  // Shared card-body builder: the rich D-pad layout with baseId-prefixed ids, so
  // a remote peer's ANO card reuses hwRenderAnoState (no duplicate renderer).
  httpd_resp_send_chunk(req,
    "window.hwBuildAnoInner=function(baseId){\n"
    "  return '<div style=\"margin-top:10px;display:flex;gap:16px;align-items:center;flex-wrap:wrap\">'+\n"
    "    '<div style=\"display:flex;flex-direction:column;align-items:center;gap:4px\">'+\n"
    "    '<canvas id=\"'+baseId+'-wheel\" width=\"100\" height=\"100\" style=\"border-radius:50%;background:var(--menu-item-bg)\"></canvas>'+\n"
    "    '<div style=\"font-size:0.75rem;color:var(--text-muted)\">Position: <span id=\"'+baseId+'-pos\">0</span></div>'+\n"
    "    '<div style=\"font-size:0.75rem;color:var(--text-muted)\">Axis: <span id=\"'+baseId+'-axis\">vertical</span></div>'+\n"
    "    '</div>'+\n"
    "    '<div style=\"display:grid;grid-template-columns:repeat(3,36px);grid-template-rows:repeat(3,36px);gap:4px\">'+\n"
    "    '<div></div><div id=\"'+baseId+'-btn-up\" class=\"btn btn-small\" style=\"font-size:0.7rem;padding:4px\">UP</div><div></div>'+\n"
    "    '<div id=\"'+baseId+'-btn-left\" class=\"btn btn-small\" style=\"font-size:0.7rem;padding:4px\">LEFT</div>'+\n"
    "    '<div id=\"'+baseId+'-btn-in\" class=\"btn btn-small\" style=\"font-size:0.7rem;padding:4px;background:var(--menu-item-bg)\">IN</div>'+\n"
    "    '<div id=\"'+baseId+'-btn-right\" class=\"btn btn-small\" style=\"font-size:0.7rem;padding:4px\">RIGHT</div>'+\n"
    "    '<div></div><div id=\"'+baseId+'-btn-down\" class=\"btn btn-small\" style=\"font-size:0.7rem;padding:4px\">DOWN</div><div></div>'+\n"
    "    '</div>'+\n"
    "    '<div style=\"display:flex;flex-direction:column;gap:4px\"><div id=\"'+baseId+'-btn-start\" class=\"btn btn-small\" style=\"font-size:0.65rem;padding:4px\">START</div></div>'+\n"
    "    '</div>';\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  // Sensor reader — registers in window._sensorReaders.anoencoder
  httpd_resp_send_chunk(req,
    "window._sensorReaders = window._sensorReaders || {};\n"
    "window._sensorReaders.anoencoder = function() {\n"
    "  var url = '/api/sensors?sensor=anoencoder&ts=' + Date.now();\n"
    "  return hw.fetchJSON(url)\n"
    "    .then(function(j) {\n"
    "      try {\n"
    "        if (typeof window.hwRenderAnoState === 'function') window.hwRenderAnoState(j);\n"
    "      } catch (_) {}\n"
    "      return j;\n"
    "    })\n"
    "    .catch(function(e) {\n"
    "      console.error('[Sensors] ANO encoder read error', e);\n"
    "      throw e;\n"
    "    });\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
}

inline void streamAnoEncoderDashboardDef(httpd_req_t* req) {
  // key:'input' so the dashboard's isCompiled() check reads `s.inputCompiled`
  // (which is true under either input driver). Sensors-page card and the
  // sensor-read API endpoint still use the 'anoencoder' moniker for
  // ANO-specific data shape.
  httpd_resp_send_chunk(req, "window.__dashSensorDefs.push({device:'Seesaw',key:'input',name:'ANO Encoder (Seesaw)',desc:'Rotary + 5-button D-pad'});", HTTPD_RESP_USE_STRLEN);
}

#endif // I2CSENSOR_ANO_ENCODER_WEB_H
