#ifndef I2CSENSOR_RDA5807_WEB_H
#define I2CSENSOR_RDA5807_WEB_H

#include <Arduino.h>
#include "WebServer_Utils.h"

inline void streamRDA5807FmRadioSensorCard(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"HTML(

    <div class='sensor-card' id='sensor-card-fmradio'>
      <div class='sensor-title'><span>FM Radio (RDA5807)</span><span class='status-indicator status-disabled' id='fmradio-status-indicator'></span></div>
      <div class='sensor-description'>FM Radio receiver with RDS support. Audio output via headphone jack.</div>
      <div class='sensor-controls'><button class='btn' id='btn-fmradio-start'>Start Radio</button><button class='btn' id='btn-fmradio-stop'>Stop Radio</button></div>
      <div class='sensor-data' id='fmradio-data'>
        <div id='fmradio-info' style='color:#333'>
          <div style='margin-bottom:8px'><strong>Frequency:</strong> <span id='fmradio-freq'>--</span> MHz</div>
          <div style='margin-bottom:8px'><strong>Volume:</strong> <span id='fmradio-volume'>--</span>/15</div>
          <div style='margin-bottom:8px'><strong>Signal:</strong> <span id='fmradio-rssi'>--</span> dBm</div>
          <div style='margin-bottom:8px'><strong>Headphones:</strong> <span id='fmradio-headphones'>--</span></div>
          <div style='margin-bottom:8px'><strong>Station:</strong> <span id='fmradio-station'>--</span></div>
          <div><strong>Radio Text:</strong> <span id='fmradio-rds'>--</span></div>
        </div>
        <div id='fmradio-controls' style='margin-top:12px;display:flex;gap:8px;flex-wrap:wrap'>
          <button class='btn btn-small' onclick="sendCmd('fmradioseek down')">⏮ Seek</button>
          <button class='btn btn-small' onclick="sendCmd('fmradioseek up')">Seek ⏭</button>
          <button class='btn btn-small' id='fmradio-mute-btn' onclick="toggleFMRadioMute()">🔇 Mute</button>
          <button class='btn btn-small' onclick="sendCmd('fmradiovolume ' + Math.max(0, parseInt(hw.$('fmradio-volume').innerText) - 1))">🔉 Vol-</button>
          <button class='btn btn-small' onclick="sendCmd('fmradiovolume ' + Math.min(15, parseInt(hw.$('fmradio-volume').innerText) + 1))">🔊 Vol+</button>
        </div>
        <div style='margin-top:10px;font-size:0.9em;color:var(--panel-fg)'>
          Tune: <code>fmradiotune 103.9</code>
        </div>
      </div>
    </div>

)HTML", HTTPD_RESP_USE_STRLEN);
}

inline void streamRDA5807FmRadioSensorBindButtons(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "bind('btn-fmradio-start','openfmradio');bind('btn-fmradio-stop','closefmradio');", HTTPD_RESP_USE_STRLEN);
}

inline void streamRDA5807FmRadioSensorJs(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function startFMRadioPolling(){if(fmradioPollingInterval){return}console.log('[SENSORS] startFMRadioPolling called');updateFMRadioDisplay();fmradioPollingInterval=setInterval(function(){updateFMRadioDisplay()},1000);console.log('[SENSORS] FM Radio polling started with interval: 1000ms')}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function stopFMRadioPolling(){if(!fmradioPollingInterval){return}console.log('[SENSORS] stopFMRadioPolling called');clearInterval(fmradioPollingInterval);fmradioPollingInterval=null;console.log('[SENSORS] FM Radio polling stopped')}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "var fmRadioMuted=false;function toggleFMRadioMute(){var cmd=fmRadioMuted?'fmradiounmute':'fmradiomute';hw.postForm('/api/cli',{cmd:cmd}).then(function(r){console.log('[FM Radio] Mute toggle result:',r);updateFMRadioDisplay()}).catch(function(e){console.error('[FM Radio] Mute toggle error:',e)})}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function updateFMRadioDisplay(){var url='/api/sensors?sensor=fmradio&ts='+Date.now();hw.fetchJSON(url).then(function(d){var el=function(id){return hw.$(id)};if(!d){return}var controlsDiv=el('fmradio-controls');if(d.error==='not_enabled'){hw.setText('fmradio-freq','--');hw.setText('fmradio-volume','--');hw.setText('fmradio-rssi','--');hw.setText('fmradio-headphones','--');hw.setText('fmradio-station','--');hw.setText('fmradio-rds','--');hw.hide(controlsDiv);return}var f=d.fmradio?d.fmradio:d; if(!f){return}if(controlsDiv)controlsDiv.style.display='flex';hw.setText('fmradio-freq',(f.frequency!=null&&f.frequency!==''?f.frequency:'--'));hw.setText('fmradio-volume',(f.volume!=null&&f.volume!==''?f.volume:'--'));hw.setText('fmradio-rssi',(f.rssi!=null&&f.rssi!==''?f.rssi:'--'));hw.setText('fmradio-headphones',(f.headphones===true?'Yes':(f.headphones===false?'No':'--')));hw.setText('fmradio-station',(f.station&&f.station!==''?f.station:'--'));hw.setText('fmradio-rds',(f.radioText&&f.radioText!==''?f.radioText:'--'));fmRadioMuted=(f.muted===true);var muteBtn=el('fmradio-mute-btn');hw.setText(muteBtn,fmRadioMuted?'🔊 Unmute':'🔇 Mute');}).catch(function(e){console.error('[FM Radio] Update error:',e)})}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
}

inline void streamRDA5807FmRadioDashboardDef(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "window.__dashSensorDefs.push({device:'RDA5807',key:'fmradio',name:'FM Radio (RDA5807)',desc:'FM Receiver & RDS'});", HTTPD_RESP_USE_STRLEN);
}

#endif // I2CSENSOR_RDA5807_WEB_H
