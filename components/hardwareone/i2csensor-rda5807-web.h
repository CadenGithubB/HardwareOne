#ifndef RDA5807_FM_RADIO_SENSOR_WEB_H
#define RDA5807_FM_RADIO_SENSOR_WEB_H

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
          <button class='btn btn-small' onclick="sendCmd('fmradio seek down')">⏮ Seek</button>
          <button class='btn btn-small' onclick="sendCmd('fmradio seek up')">Seek ⏭</button>
          <button class='btn btn-small' id='fmradio-mute-btn' onclick="toggleFMRadioMute()">🔇 Mute</button>
          <button class='btn btn-small' onclick="sendCmd('fmradio volume ' + Math.max(0, parseInt(document.getElementById('fmradio-volume').innerText) - 1))">🔉 Vol-</button>
          <button class='btn btn-small' onclick="sendCmd('fmradio volume ' + Math.min(15, parseInt(document.getElementById('fmradio-volume').innerText) + 1))">🔊 Vol+</button>
        </div>
        <div style='margin-top:10px;font-size:0.9em;color:#6c757d'>
          Tune: <code>fmradio tune 103.9</code>
        </div>
      </div>
    </div>

)HTML", HTTPD_RESP_USE_STRLEN);
}

inline void streamRDA5807FmRadioSensorBindButtons(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "bind('btn-fmradio-start','fmradio start');bind('btn-fmradio-stop','fmradio stop');", HTTPD_RESP_USE_STRLEN);
}

inline void streamRDA5807FmRadioSensorJs(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function startFMRadioPolling(){if(fmradioPollingInterval){return}console.log('[SENSORS] startFMRadioPolling called');updateFMRadioDisplay();fmradioPollingInterval=setInterval(function(){updateFMRadioDisplay()},1000);console.log('[SENSORS] FM Radio polling started with interval: 1000ms')}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function stopFMRadioPolling(){if(!fmradioPollingInterval){return}console.log('[SENSORS] stopFMRadioPolling called');clearInterval(fmradioPollingInterval);fmradioPollingInterval=null;console.log('[SENSORS] FM Radio polling stopped')}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "var fmRadioMuted=false;function toggleFMRadioMute(){var cmd=fmRadioMuted?'fmradio unmute':'fmradio mute';hw.postForm('/api/cli',{cmd:cmd}).then(function(r){console.log('[FM Radio] Mute toggle result:',r);updateFMRadioDisplay()}).catch(function(e){console.error('[FM Radio] Mute toggle error:',e)})}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "function updateFMRadioDisplay(){var url='/api/sensors?sensor=fmradio&ts='+Date.now();hw.fetchJSON(url).then(function(d){var el=function(id){return document.getElementById(id)};if(!d){return}var controlsDiv=el('fmradio-controls');if(d.error==='not_enabled'){if(el('fmradio-freq'))el('fmradio-freq').textContent='--';if(el('fmradio-volume'))el('fmradio-volume').textContent='--';if(el('fmradio-rssi'))el('fmradio-rssi').textContent='--';if(el('fmradio-headphones'))el('fmradio-headphones').textContent='--';if(el('fmradio-station'))el('fmradio-station').textContent='--';if(el('fmradio-rds'))el('fmradio-rds').textContent='--';if(controlsDiv)controlsDiv.style.display='none';return}var f=d.fmradio?d.fmradio:d; if(!f){return}if(controlsDiv)controlsDiv.style.display='flex';if(el('fmradio-freq'))el('fmradio-freq').textContent=(f.frequency!=null&&f.frequency!==''?f.frequency:'--');if(el('fmradio-volume'))el('fmradio-volume').textContent=(f.volume!=null&&f.volume!==''?f.volume:'--');if(el('fmradio-rssi'))el('fmradio-rssi').textContent=(f.rssi!=null&&f.rssi!==''?f.rssi:'--');if(el('fmradio-headphones'))el('fmradio-headphones').textContent=(f.headphones===true?'Yes':(f.headphones===false?'No':'--'));if(el('fmradio-station'))el('fmradio-station').textContent=(f.station&&f.station!==''?f.station:'--');if(el('fmradio-rds'))el('fmradio-rds').textContent=(f.radioText&&f.radioText!==''?f.radioText:'--');fmRadioMuted=(f.muted===true);var muteBtn=el('fmradio-mute-btn');if(muteBtn){muteBtn.textContent=fmRadioMuted?'🔊 Unmute':'🔇 Mute'}}).catch(function(e){console.error('[FM Radio] Update error:',e)})}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
}

inline void streamRDA5807FmRadioDashboardDef(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "window.__dashSensorDefs.push({device:'RDA5807',key:'fmradio',name:'FM Radio (RDA5807)',desc:'FM Receiver + RDS'});", HTTPD_RESP_USE_STRLEN);
}

#endif // RDA5807_FM_RADIO_SENSOR_WEB_H
