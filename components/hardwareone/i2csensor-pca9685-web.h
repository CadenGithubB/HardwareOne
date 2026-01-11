#ifndef PCA9685_SERVO_DRIVER_WEB_H
#define PCA9685_SERVO_DRIVER_WEB_H

#include <Arduino.h>
#include "WebServer_Utils.h"

inline void streamPCA9685ServoDriverCard(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"HTML(

    <div class='sensor-card' id='sensor-card-servo'>
      <div class='sensor-title'><span>Servo Driver (PCA9685)</span><span class='status-indicator status-disabled' id='servo-status-indicator'></span></div>
      <div class='sensor-description'>16-Channel PWM/Servo Driver for controlling servos and motors.</div>
      <div class='sensor-data' id='servo-data'>
        <div style='margin-bottom:10px;padding:8px;background:rgba(135,206,235,0.1);border-radius:4px;font-size:0.9em'>
          <strong>Board Detected:</strong> <span style='color:#28a745'>✓ PCA9685 found on I2C</span><br>
          <strong>Driver Status:</strong> <span id='servo-connection-status'>Checking...</span>
        </div>
        <div style='margin-top:10px;font-size:0.9em;color:#87ceeb'>
          Use CLI commands to control servos:<br>
          • <code>servo &lt;channel&gt; &lt;angle&gt;</code> - Move servo (auto-initializes driver)<br>
          • <code>servoprofile &lt;ch&gt; &lt;min&gt; &lt;max&gt; &lt;center&gt; &lt;name&gt;</code> - Configure<br>
          • <code>servolist</code> - Show configured servos<br>
          • <code>servocalibrate &lt;channel&gt;</code> - Calibration mode
        </div>
      </div>
    </div>

)HTML", HTTPD_RESP_USE_STRLEN);
}

inline void streamPCA9685ServoDriverDashboardDef(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "window.__dashSensorDefs.push({device:'PCA9685',key:'pwm',name:'Servo Driver (PCA9685)',desc:'16-Channel PWM/Servo'});", HTTPD_RESP_USE_STRLEN);
}

#endif // PCA9685_SERVO_DRIVER_WEB_H
