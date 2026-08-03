#ifndef WEBPAGE_AVIPLAYER_H
#define WEBPAGE_AVIPLAYER_H

#include <Arduino.h>
#include "esp_http_server.h"

// ── Shared AVI/MJPEG player (client-side) ─────────────────────────────────────
// Injected into any page that needs to play .avi recordings from the device.
// Parses the RIFF/AVI container in the browser and draws each embedded JPEG
// frame to a <canvas>. The device just serves the raw file via
// /api/videos/file?name=<basename>.
//
// Two streamers — call both on the target page (modal first, then JS):
//   streamAviPlayerModal(req);
//   streamAviPlayerJs(req);

inline void streamAviPlayerModal(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"HTML(
    <div id='avi-modal' style='display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.8);z-index:9999;align-items:center;justify-content:center'>
      <div style='background:var(--panel-bg,#222);color:var(--panel-fg,#fff);border:1px solid #444;border-radius:8px;padding:16px;max-width:95vw;max-height:95vh;display:flex;flex-direction:column;gap:10px'>
        <div style='display:flex;justify-content:space-between;align-items:center'>
          <strong id='avi-title'>Playback</strong>
          <button class='btn' id='avi-close' style='padding:2px 10px'>Close</button>
        </div>
        <div id='avi-status' style='font-size:0.85em;color:var(--muted,#aaa)'>Loading…</div>
        <canvas id='avi-canvas' width='640' height='480' style='max-width:85vw;max-height:70vh;background:#000;border-radius:4px'></canvas>
        <div style='display:flex;gap:8px;align-items:center'>
          <button class='btn' id='avi-play' style='min-width:70px'>Play</button>
          <input type='range' id='avi-scrub' min='0' max='0' value='0' step='1' style='flex:1'>
          <span id='avi-time' style='font-variant-numeric:tabular-nums;min-width:110px;text-align:right'>0.0s / 0.0s</span>
        </div>
      </div>
    </div>
)HTML", HTTPD_RESP_USE_STRLEN);
}

// Emits the player JS *without* surrounding <script> tags, so it can be
// composed inside an existing script block or wrapped in a fresh one by the
// caller.
inline void streamAviPlayerJs(httpd_req_t* req) {
  httpd_resp_send_chunk(req,
    "(function(){\n"
    "  if (window.openAviPlayer) return;\n"
    "  function readFourCC(dv, pos){\n"
    "    return String.fromCharCode(dv.getUint8(pos),dv.getUint8(pos+1),dv.getUint8(pos+2),dv.getUint8(pos+3));\n"
    "  }\n"
    "  function parseHdrl(dv, start, end){\n"
    "    var fps=10, w=0, h=0, pos=start;\n"
    "    while (pos < end-8) {\n"
    "      var id = readFourCC(dv,pos); var size = dv.getUint32(pos+4,true);\n"
    "      if (id === 'avih') {\n"
    "        w = dv.getUint32(pos+8+32,true); h = dv.getUint32(pos+8+36,true);\n"
    "      } else if (id === 'LIST' && readFourCC(dv,pos+8) === 'strl') {\n"
    "        var sp = pos+12, strlEnd = pos+8+size;\n"
    "        while (sp < strlEnd-8) {\n"
    "          var sid = readFourCC(dv,sp); var ssize = dv.getUint32(sp+4,true);\n"
    "          if (sid === 'strh') {\n"
    "            var scale = dv.getUint32(sp+8+20,true); var rate = dv.getUint32(sp+8+24,true);\n"
    "            if (scale > 0) fps = rate/scale;\n"
    "          }\n"
    "          var adv = 8+ssize; if (ssize & 1) adv += 1; sp += adv;\n"
    "        }\n"
    "      }\n"
    "      var adv = 8+size; if (size & 1) adv += 1; pos += adv;\n"
    "    }\n"
    "    return {fps:fps, width:w, height:h};\n"
    "  }\n"
    "  function parseAvi(buf){\n"
    "    var dv = new DataView(buf);\n"
    "    if (readFourCC(dv,0) !== 'RIFF' || readFourCC(dv,8) !== 'AVI ')\n"
    "      throw new Error('Not an AVI file');\n"
    "    var meta = {fps:10, width:0, height:0};\n"
    "    var frames = [];\n"
    "    var pos = 12;\n"
    "    while (pos < buf.byteLength - 8) {\n"
    "      var id = readFourCC(dv,pos); var size = dv.getUint32(pos+4,true);\n"
    "      if (id === 'LIST') {\n"
    "        var listType = readFourCC(dv,pos+8);\n"
    "        if (listType === 'hdrl') {\n"
    "          var h = parseHdrl(dv, pos+12, pos+8+size);\n"
    "          meta.fps = h.fps; meta.width = h.width; meta.height = h.height;\n"
    "        } else if (listType === 'movi') {\n"
    // size==0 means an un-finalized/truncated AVI (header never patched). Scan
    // to EOF (or idx1) so we can still play whatever complete 00dc chunks exist.
    "          var moviEnd = (size > 0) ? (pos+8+size) : buf.byteLength;\n"
    "          if (moviEnd > buf.byteLength) moviEnd = buf.byteLength;\n"
    "          var fp = pos+12;\n"
    "          while (fp < moviEnd - 8) {\n"
    "            var fid = readFourCC(dv,fp); var fsize = dv.getUint32(fp+4,true);\n"
    "            if (fid === 'idx1') { moviEnd = fp; break; }\n"
    "            if (fid !== '00dc' && fid !== '00db') break;\n"
    "            if (fsize === 0 || fp+8+fsize > buf.byteLength) break;\n"
    "            frames.push({offset: fp+8, size: fsize});\n"
    "            var adv = 8+fsize; if (fsize & 1) adv += 1; fp += adv;\n"
    "          }\n"
    "          if (size === 0) size = Math.max(4, fp - (pos+8));\n"
    "        }\n"
    "      }\n"
    "      var adv = 8+size; if (size & 1) adv += 1; pos += adv;\n"
    "    }\n"
    "    return {meta:meta, frames:frames};\n"
    "  }\n"
    "  var _aviState = {buf:null, frames:null, fps:10, cur:0, timer:null, playing:false};\n"
    "  function _aviDraw(idx){\n"
    "    if (!_aviState.buf || !_aviState.frames) return;\n"
    "    var f = _aviState.frames[idx]; if (!f) return;\n"
    "    var canvas = document.getElementById('avi-canvas');\n"
    "    var ctx = canvas.getContext('2d');\n"
    "    var view = new Uint8Array(_aviState.buf, f.offset, f.size);\n"
    "    var blob = new Blob([view], {type:'image/jpeg'});\n"
    "    createImageBitmap(blob).then(function(bmp){\n"
    "      ctx.drawImage(bmp, 0, 0, canvas.width, canvas.height);\n"
    "      if (bmp.close) bmp.close();\n"
    "    }).catch(function(){});\n"
    "    var sc = document.getElementById('avi-scrub'); if (sc) sc.value = idx;\n"
    "    var tm = document.getElementById('avi-time');\n"
    "    if (tm) {\n"
    "      var cur = (idx/_aviState.fps).toFixed(1);\n"
    "      var tot = (_aviState.frames.length/_aviState.fps).toFixed(1);\n"
    "      tm.textContent = cur+'s / '+tot+'s';\n"
    "    }\n"
    "  }\n"
    "  function _aviStop(){\n"
    "    if (_aviState.timer) { clearInterval(_aviState.timer); _aviState.timer = null; }\n"
    "    _aviState.playing = false;\n"
    "    var pb = document.getElementById('avi-play'); if (pb) pb.textContent = 'Play';\n"
    "  }\n"
    "  function _aviPlayPause(){\n"
    "    if (_aviState.playing) { _aviStop(); return; }\n"
    "    if (!_aviState.frames || _aviState.frames.length === 0) return;\n"
    "    _aviState.playing = true;\n"
    "    var pb = document.getElementById('avi-play'); if (pb) pb.textContent = 'Pause';\n"
    "    var interval = Math.max(20, Math.round(1000 / (_aviState.fps || 10)));\n"
    "    _aviState.timer = setInterval(function(){\n"
    "      _aviState.cur = (_aviState.cur + 1) % _aviState.frames.length;\n"
    "      _aviDraw(_aviState.cur);\n"
    "    }, interval);\n"
    "  }\n"
    "  window.openAviPlayer = function(name) {\n"
    "    var modal = document.getElementById('avi-modal');\n"
    "    var status = document.getElementById('avi-status');\n"
    "    var title = document.getElementById('avi-title');\n"
    "    var scrub = document.getElementById('avi-scrub');\n"
    "    if (!modal) return;\n"
    "    _aviStop();\n"
    "    _aviState.buf = null; _aviState.frames = null; _aviState.cur = 0;\n"
    // Accept either a bare basename or a full path — the backend only
    // serves files under /VIDEOS and rejects anything with a slash.
    "    var base = name; var slash = name.lastIndexOf('/');\n"
    "    if (slash >= 0) base = name.substring(slash + 1);\n"
    "    if (title) title.textContent = base;\n"
    "    if (status) status.textContent = 'Loading…';\n"
    "    modal.style.display = 'flex';\n"
    "    fetch('/api/videos/file?name=' + encodeURIComponent(base), {credentials:'include'})\n"
    "      .then(function(r){ if (!r.ok) throw new Error('HTTP '+r.status); return r.arrayBuffer(); })\n"
    "      .then(function(buf){\n"
    "        var parsed;\n"
    "        try { parsed = parseAvi(buf); } catch(e) { throw new Error('Parse failed: '+e.message); }\n"
    "        if (!parsed.frames.length) throw new Error('No frames found in AVI');\n"
    "        _aviState.buf = buf;\n"
    "        _aviState.frames = parsed.frames;\n"
    "        _aviState.fps = parsed.meta.fps || 10;\n"
    "        var canvas = document.getElementById('avi-canvas');\n"
    "        if (canvas && parsed.meta.width > 0 && parsed.meta.height > 0) {\n"
    "          canvas.width = parsed.meta.width; canvas.height = parsed.meta.height;\n"
    "        }\n"
    "        if (scrub) { scrub.max = parsed.frames.length - 1; scrub.value = 0; }\n"
    "        if (status) status.textContent = parsed.frames.length + ' frames @ ' + _aviState.fps.toFixed(1) + ' fps · ' + (parsed.meta.width||'?') + 'x' + (parsed.meta.height||'?');\n"
    "        _aviDraw(0);\n"
    "      }).catch(function(e){ if (status) status.textContent = 'Error: ' + e.message; });\n"
    "  };\n"
    "  document.addEventListener('DOMContentLoaded', function(){\n"
    "    var modal = document.getElementById('avi-modal');\n"
    "    var closeBtn = document.getElementById('avi-close');\n"
    "    var playBtn = document.getElementById('avi-play');\n"
    "    var scrub = document.getElementById('avi-scrub');\n"
    "    if (closeBtn) closeBtn.onclick = function(){ _aviStop(); if (modal) modal.style.display='none'; };\n"
    "    if (playBtn) playBtn.onclick = _aviPlayPause;\n"
    "    if (scrub) scrub.oninput = function(){ _aviStop(); _aviState.cur = +scrub.value; _aviDraw(_aviState.cur); };\n"
    "    if (modal) modal.onclick = function(e){ if (e.target === modal) { _aviStop(); modal.style.display='none'; } };\n"
    "  });\n"
    "})();\n",
    HTTPD_RESP_USE_STRLEN);
}

#endif // WEBPAGE_AVIPLAYER_H
