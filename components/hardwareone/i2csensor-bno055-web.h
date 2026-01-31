#ifndef BNO055_IMU_SENSOR_WEB_H
#define BNO055_IMU_SENSOR_WEB_H

#include <Arduino.h>
#include "WebServer_Utils.h"

inline void streamBNO055ImuSensorCard(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"HTML(

    <div class='sensor-card' id='sensor-card-imu'>
      <div class='sensor-title'><span>Gyroscope & Accelerometer (BNO055)</span><span class='status-indicator status-disabled' id='gyro-status-indicator'></span></div>
      <div class='sensor-description'>9-axis IMU sensor for orientation, acceleration, and gyroscope data.</div>
      <div id='imu-queue-status' style='display:none;background:#fff3cd;border:1px solid #ffc107;border-radius:4px;padding:8px;margin-bottom:10px;color:#856404;font-size:.9em'></div>
      <div class='sensor-controls'><button class='btn' id='btn-imu-start'>Open IMU</button><button class='btn' id='btn-imu-stop'>Close IMU</button></div>
      <div class='sensor-data' id='gyro-data'>
        <div class='gyro-display-container'>
          <div style='display:flex;gap:1rem;align-items:center'>
            <div class='gyro-reading'>Initializing...</div>
            <canvas id='device-rotation-canvas' width='80' height='80' style='border:1px solid #ddd;border-radius:4px;background:#f9f9f9'></canvas>
          </div>
        </div>
      </div>
    </div>

)HTML", HTTPD_RESP_USE_STRLEN);
}

inline void streamBNO055ImuSensorBindButtons(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "bind('btn-imu-start','openimu');bind('btn-imu-stop','closeimu');", HTTPD_RESP_USE_STRLEN);
}

inline void streamBNO055ImuSensorJs(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  
  // IMU sensor reader - register in window._sensorReaders
  httpd_resp_send_chunk(req,
    "window._sensorReaders = window._sensorReaders || {};\n"
    "window._sensorReaders.imu = function() {\n"
    "    var url = '/api/sensors?sensor=imu&ts=' + Date.now();\n"
    "    return fetch(url, {cache: 'no-store', credentials: 'include'})\n"
    "      .then(function(r) {\n"
    "        return r.json();\n"
    "      })\n"
    "      .then(function(j) {\n"
    "        var el = document.getElementById('gyro-data');\n"
    "        if (el) {\n"
    "          if (j && j.error) {\n"
    "            el.textContent = 'IMU error: ' + j.error;\n"
    "          } else if (j && j.valid) {\n"
    "            var ax = j.accel.x.toFixed(2), ay = j.accel.y.toFixed(2), az = j.accel.z.toFixed(2);\n"
    "            var gx = j.gyro.x.toFixed(2), gy = j.gyro.y.toFixed(2), gz = j.gyro.z.toFixed(2);\n"
    "            var yw = j.ori.yaw.toFixed(1), pt = j.ori.pitch.toFixed(1), rl = j.ori.roll.toFixed(1);\n"
    "            var tc = Number(j.temp).toFixed(0);\n"
    "            var accel = 'Accel: X=' + (j.ax || 0).toFixed(2) + ' Y=' + (j.ay || 0).toFixed(2) + ' Z=' + (j.az || 0).toFixed(2) + ' m/s²';\n"
    "            var gyro = 'Gyro: X=' + (j.gx || 0).toFixed(2) + ' Y=' + (j.gy || 0).toFixed(2) + ' Z=' + (j.gz || 0).toFixed(2) + ' °/s';\n"
    "            var euler = 'Euler: X=' + (j.ex || 0).toFixed(1) + '° Y=' + (j.ey || 0).toFixed(1) + '° Z=' + (j.ez || 0).toFixed(1) + '°';\n"
    "            var temp = 'Temperature: ' + (j.temp || 0).toFixed(1) + '°C';\n"
    "            \n"
    "            // Update device rotation visualization\n"
    "            if (window.updateDeviceOrientation) {\n"
    "              window.updateDeviceOrientation(j.ex || 0, j.ey || 0, j.ez || 0);\n"
    "            }\n"
    "            \n"
    "            if (el) {\n"
    "              el.innerHTML = '<div style=\"display:grid;grid-template-columns:1fr;gap:0.5rem;font-family:monospace;font-size:0.9em;line-height:1.4\">' +\n"
    "                '<div style=\"color:#007bff\">' + accel + '</div>' +\n"
    "                '<div style=\"color:#28a745\">' + gyro + '</div>' +\n"
    "                '<div style=\"color:#dc3545\">' + euler + '</div>' +\n"
    "                '<div style=\"color:#6c757d\">' + temp + '</div>' +\n"
    "              '</div>';\n"
    "            }\n"
    "          } else {\n"
    "            var en = (j && typeof j.enabled !== 'undefined') ? j.enabled : '?';\n"
    "            var cn = (j && typeof j.connected !== 'undefined') ? j.connected : '?';\n"
    "            var ir = (j && typeof j.initRequested !== 'undefined') ? j.initRequested : '?';\n"
    "            var id = (j && typeof j.initDone !== 'undefined') ? j.initDone : '?';\n"
    "            var io = (j && typeof j.initResult !== 'undefined') ? j.initResult : '?';\n"
    "            var age = (j && typeof j.ageMs !== 'undefined') ? j.ageMs : '?';\n"
    "            var seq = (j && typeof j.seq !== 'undefined') ? j.seq : '?';\n"
    "            el.textContent = 'IMU not ready (enabled=' + en + ', connected=' + cn + ', initRequested=' + ir + ', initDone=' + id + ', initResult=' + io + ', ageMs=' + age + ', seq=' + seq + ')';\n"
    "          }\n"
    "        }\n"
    "        return j;\n"
    "      })\n"
    "      .catch(function(e) {\n"
    "        console.error('[Sensors] IMU read error', e);\n"
    "        throw e;\n"
    "      });\n"
    "};\n", HTTPD_RESP_USE_STRLEN);

  // DeviceRotationViz class (verbatim from web_sensors.h)
  httpd_resp_send_chunk(req,
    "class DeviceRotationViz {\n"
    "  constructor(canvasId) {\n"
    "    this.canvas = document.getElementById(canvasId);\n"
    "    if (!this.canvas) return;\n"
    "    this.ctx = this.canvas.getContext('2d');\n"
    "    this.size = 80;\n"
    "    this.pitch = 0; this.roll = 0; this.yaw = 0;\n"
    "    this.setupCanvas();\n"
    "  }\n"
    "  setupCanvas() {\n"
    "    this.canvas.width = this.size;\n"
    "    this.canvas.height = this.size;\n"
    "    this.ctx.imageSmoothingEnabled = true;\n"
    "  }\n"
    "  rotateCubePoint(x, y, z, angleX, angleY, angleZ) {\n"
    "    const cosX = Math.cos(angleX), sinX = Math.sin(angleX);\n"
    "    let y1 = y * cosX - z * sinX, z1 = y * sinX + z * cosX;\n"
    "    y = y1; z = z1;\n"
    "    const cosY = Math.cos(angleY), sinY = Math.sin(angleY);\n"
    "    let x1 = x * cosY + z * sinY; z1 = -x * sinY + z * cosY;\n"
    "    x = x1; z = z1;\n"
    "    const cosZ = Math.cos(angleZ), sinZ = Math.sin(angleZ);\n"
    "    x1 = x * cosZ - y * sinZ; y1 = x * sinZ + y * cosZ;\n"
    "    return [x1, y1, z1];\n"
    "  }\n"
    "  projectCubePoint(x, y, z, centerX, centerY) {\n"
    "    const perspective = 200.0 / (200.0 + z);\n"
    "    return [Math.round(centerX + x * perspective), Math.round(centerY + y * perspective)];\n"
    "  }\n"
    "  calculateFaceBrightness(normal) {\n"
    "    const lightDirection = [0, 0, 1];\n"
    "    const normalLength = Math.sqrt(normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2]);\n"
    "    if (normalLength === 0) return 200;\n"
    "    const normalNorm = [normal[0]/normalLength, normal[1]/normalLength, normal[2]/normalLength];\n"
    "    const dotProduct = normalNorm[0]*lightDirection[0] + normalNorm[1]*lightDirection[1] + normalNorm[2]*lightDirection[2];\n"
    "    return Math.max(180, Math.min(255, 180 + 75 * Math.max(0, dotProduct)));\n"
    "  }\n"
    "  updateOrientation(pitchDeg, rollDeg, yawDeg) {\n"
    "    this.pitch = (pitchDeg || 0) * Math.PI / 180;\n"
    "    this.roll = (rollDeg || 0) * Math.PI / 180;\n"
    "    this.yaw = (yawDeg || 0) * Math.PI / 180;\n"
    "    this.render();\n"
    "  }\n", HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req,
    "  render() {\n"
    "    if (!this.ctx) return;\n"
    "    this.ctx.clearRect(0, 0, this.size, this.size);\n"
    "    const scale = this.size / 100.0;\n"
    "    const width = 12.5 * scale, height = 25.0 * scale, depth = 5.0 * scale;\n"
    "    const centerX = this.size / 2, centerY = this.size / 2;\n"
    "    const vertices = [\n"
    "      [-width,-height,-depth],[width,-height,-depth],[width,height,-depth],[-width,height,-depth],\n"
    "      [-width,-height,depth],[width,-height,depth],[width,height,depth],[-width,height,depth]\n"
    "    ];\n"
    "    const rotated = [], projected = [];\n"
    "    for (const vertex of vertices) {\n"
    "      const [x,y,z] = this.rotateCubePoint(vertex[0],vertex[1],vertex[2],this.pitch,this.roll,this.yaw);\n"
    "      rotated.push([x,y,z]);\n"
    "      const [screenX,screenY] = this.projectCubePoint(x,y,z,centerX,centerY);\n"
    "      projected.push([screenX,screenY]);\n"
    "    }\n"
    "    const faces = [\n"
    "      [[0,1,5,4],'left'],[[3,7,6,2],'right'],[[4,5,6,7],'front'],\n"
    "      [[0,3,2,1],'back'],[[0,4,7,3],'bottom'],[[1,2,6,5],'top']\n"
    "    ];\n"
    "    const facesWithDepth = [];\n"
    "    for (const [faceVertices] of faces) {\n"
    "      const avgZ = faceVertices.reduce((sum,i) => sum + rotated[i][2], 0) / faceVertices.length;\n"
    "      facesWithDepth.push([avgZ, faceVertices]);\n"
    "    }\n"
    "    facesWithDepth.sort((a,b) => a[0] - b[0]);\n"
    "    let frontFaceVertices = null, minAvgZ = Infinity;\n"
    "    for (const [avgZ, faceVertices] of facesWithDepth) {\n"
    "      if (avgZ < minAvgZ) { minAvgZ = avgZ; frontFaceVertices = faceVertices; }\n"
    "    }\n", HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req,
    "    this.ctx.lineWidth = 1;\n"
    "    for (const [avgZ, faceVertices] of facesWithDepth) {\n"
    "      if (faceVertices === frontFaceVertices) continue;\n"
    "      const points = faceVertices.map(i => projected[i]);\n"
    "      const v0=faceVertices[0], v1=faceVertices[1], v2=faceVertices[2];\n"
    "      const edge1=[rotated[v1][0]-rotated[v0][0],rotated[v1][1]-rotated[v0][1],rotated[v1][2]-rotated[v0][2]];\n"
    "      const edge2=[rotated[v2][0]-rotated[v0][0],rotated[v2][1]-rotated[v0][1],rotated[v2][2]-rotated[v0][2]];\n"
    "      const normal=[edge1[1]*edge2[2]-edge1[2]*edge2[1],edge1[2]*edge2[0]-edge1[0]*edge2[2],edge1[0]*edge2[1]-edge1[1]*edge2[0]];\n"
    "      const brightness = this.calculateFaceBrightness(normal);\n"
    "      this.ctx.fillStyle = 'rgb('+brightness+','+brightness+','+brightness+')';\n"
    "      this.ctx.beginPath(); this.ctx.moveTo(points[0][0], points[0][1]);\n"
    "      for (let i=1; i<points.length; i++) this.ctx.lineTo(points[i][0], points[i][1]);\n"
    "      this.ctx.closePath(); this.ctx.fill();\n"
    "    }\n"
    "    if (frontFaceVertices) {\n"
    "      const points = frontFaceVertices.map(i => projected[i]);\n"
    "      this.ctx.fillStyle = 'white'; this.ctx.beginPath();\n"
    "      this.ctx.moveTo(points[0][0], points[0][1]);\n"
    "      for (let i=1; i<points.length; i++) this.ctx.lineTo(points[i][0], points[i][1]);\n"
    "      this.ctx.closePath(); this.ctx.fill();\n"
    "      this.ctx.strokeStyle = 'black'; this.ctx.lineWidth = 2; this.ctx.stroke();\n"
    "      const frontZ = depth * Math.cos(this.roll) * Math.cos(this.pitch);\n"
    "      if (frontZ > 0) {\n"
    "        const [screenX,screenY] = this.projectCubePoint(0,-height*0.7,depth,centerX,centerY);\n"
    "        this.ctx.fillStyle = '#333';\n"
    "        this.ctx.fillRect(screenX-width*0.6,screenY-height*0.15,width*1.2,height*0.3);\n"
    "        this.ctx.strokeStyle = 'white'; this.ctx.lineWidth = 1;\n"
    "        this.ctx.strokeRect(screenX-width*0.6,screenY-height*0.15,width*1.2,height*0.3);\n"
    "        const [tofX,tofY] = this.projectCubePoint(-width*0.4,height*0.125,depth,centerX,centerY);\n"
    "        this.ctx.fillRect(tofX-4,tofY-3,8,6); this.ctx.strokeRect(tofX-4,tofY-3,8,6);\n"
    "        const [irX,irY] = this.projectCubePoint(width*0.3,height*0.125,depth,centerX,centerY);\n"
    "        this.ctx.beginPath(); this.ctx.arc(irX,irY,4,0,2*Math.PI); this.ctx.fill(); this.ctx.stroke();\n"
    "      }\n"
    "    }\n"
    "    this.ctx.strokeStyle = 'white'; this.ctx.lineWidth = 1;\n"
    "    for (const [,faceVertices] of facesWithDepth) {\n"
    "      const points = faceVertices.map(i => projected[i]);\n"
    "      this.ctx.beginPath(); this.ctx.moveTo(points[0][0], points[0][1]);\n"
    "      for (let i=1; i<points.length; i++) this.ctx.lineTo(points[i][0], points[i][1]);\n"
    "      this.ctx.closePath(); this.ctx.stroke();\n"
    "    }\n"
    "  }\n"
    "}\n"
    "let deviceViz = null;\n"
    "window.initDeviceVisualization = function() {\n"
    "  deviceViz = new DeviceRotationViz('device-rotation-canvas');\n"
    "  if (deviceViz && deviceViz.canvas) deviceViz.updateOrientation(15, 0, 0);\n"
    "};\n"
    "window.updateDeviceOrientation = function(pitch, roll, yaw) {\n"
    "  if (deviceViz && deviceViz.canvas) deviceViz.updateOrientation(pitch, roll, yaw);\n"
    "};\n", HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);
}

inline void streamBNO055ImuDashboardDef(httpd_req_t* req) {
  httpd_resp_send_chunk(req, "window.__dashSensorDefs.push({device:'BNO055',key:'imu',name:'IMU (BNO055)',desc:'Gyroscope & Accelerometer'});", HTTPD_RESP_USE_STRLEN);
}

#endif // BNO055_IMU_SENSOR_WEB_H
