// =============================================
// SECTION 6: SENSOR & HARDWARE
// =============================================

function controlSensor(sensor, action) {
  return hw.postFormText('/api/cli', { cmd: sensor + action })
    .catch(function(_) { return ''; });
}

async function checkSensorAvailability() {
  try {
    var sysData = await hw.fetchJSON('/api/system');
    i2cEnabled = (sysData.i2c_enabled === true);
    if (!i2cEnabled) {
      console.warn('[GAMES] I2C is disabled - sensors unavailable');
      return;
    }
    var sensData = await hw.fetchJSON('/api/sensors/status');
    imuCompiled = (sensData.imuCompiled === true);
    imuRunning = (sensData.imuRunning === true);
    inputCompiled = (sensData.inputCompiled === true);
    inputRunning = (sensData.inputRunning === true);
    console.log('[GAMES] Sensor status: i2c=' + i2cEnabled + ' imuCompiled=' + imuCompiled + ' imuRunning=' + imuRunning + ' inputCompiled=' + inputCompiled + ' inputRunning=' + inputRunning);
  } catch (e) {
    console.error('[GAMES] Failed to check sensor availability:', e);
    i2cEnabled = false; imuRunning = false; imuCompiled = false;
    inputRunning = false; inputCompiled = false;
  }
}

function startCalibration() {
  if (!i2cEnabled || !imuCompiled) {
    var hud = document.getElementById('hudCal');
    hud.textContent = 'IMU unavailable: ' + (i2cEnabled ? 'not compiled' : 'I2C disabled');
    hud.style.color = '#ff6b6b';
    alert('IMU sensor is not available. I2C: ' + i2cEnabled + ', IMU compiled: ' + imuCompiled);
    return;
  }
  controlSensor('imu', 'start').then(function() {
    var hud = document.getElementById('hudCal');
    hud.textContent = 'Hold still... calibrating';
    hasBaseline = false; basePitch = 0; baseRoll = 0; calibSamples = [];
    var t0 = Date.now();
    if (calibTimer) { clearInterval(calibTimer); calibTimer = null; }
    calibTimer = setInterval(function() {
      hw.fetchJSON('/api/sensors?sensor=imu&ts=' + Date.now())
        .then(function(j) {
          if (j && j.valid && j.ori) calibSamples.push({p:j.ori.pitch || 0, r:j.ori.roll || 0});
        }).catch(function(_) {});
      if (Date.now() - t0 >= calibMs) {
        clearInterval(calibTimer); calibTimer = null;
        if (calibSamples.length) {
          var sp = 0, sr = 0;
          for (var i = 0; i < calibSamples.length; i++) {
            sp += calibSamples[i].p; sr += calibSamples[i].r;
          }
          basePitch = sp / calibSamples.length;
          baseRoll = sr / calibSamples.length;
          hasBaseline = true;
          hud.textContent = 'Calibrated: pitch ' + basePitch.toFixed(1) + '\u00b0, roll ' + baseRoll.toFixed(1) + '\u00b0';
        } else {
          hud.textContent = 'Calibration failed';
        }
      }
    }, 50);
  });
}

function startGamepadPolling() {
  if (gpPoll) return;
  if (!i2cEnabled || !inputCompiled) {
    console.warn('[GAMES] Gamepad polling disabled: i2c=' + i2cEnabled + ' inputCompiled=' + inputCompiled);
    return;
  }
  function tick() {
    var now = Date.now();
    if (now < gpBackoffUntil) return;
    hw.fetchJSON('/api/sensors?sensor=input&ts=' + now)
      .then(function(j) {
        if (!running) return;
        if (j && j.val) {
          var x = j.x | 0, y = j.y | 0;
          var nx = (x - 512) / 512, ny = (y - 512) / 512;
          if (nx < -1) nx = -1; if (nx > 1) nx = 1;
          if (ny < -1) ny = -1; if (ny > 1) ny = 1;
          // Deadzone: ignore tiny stick drift
          var GP_DEAD = 0.12;
          if (Math.abs(nx) < GP_DEAD) nx = 0;
          if (Math.abs(ny) < GP_DEAD) ny = 0;
          gpLast = {x:nx, y:ny, buttons:(j.buttons || 0), valid:true};
        } else {
          gpLast.valid = false; gpBackoffUntil = Date.now() + 150;
        }
      }).catch(function(_) { gpLast.valid = false; gpBackoffUntil = Date.now() + 200; });
  }
  gpPoll = setInterval(tick, 16);
}

function stopGamepadPolling() {
  if (gpPoll) { try { clearInterval(gpPoll); } catch (_) {} gpPoll = null; }
  gpLast.valid = false;
}


// =============================================
// SECTION 7: INPUT PROCESSING
// =============================================

function applyTilt(pitch, roll) {
  if (hasBaseline) { roll -= baseRoll; pitch -= basePitch; }
  if (Math.abs(roll) < dead) roll = 0;
  roll = clamp(roll, -maxAng, maxAng);
  if (Math.abs(pitch) < dead) pitch = 0;
  pitch = clamp(pitch, -maxAng, maxAng);
  vel.x += (roll / maxAng) * speed * 0.10;
  vel.y += (pitch / maxAng) * speed * 0.10;
}

function applyGamepad(nx, ny, dt) {
  var dz = 0.05;
  if (Math.abs(nx) < dz) nx = 0;
  if (Math.abs(ny) < dz) ny = 0;
  nx = clamp(nx, -1, 1);
  ny = clamp(ny, -1, 1);
  var dtScale = dt > 0 ? (dt / 0.060) : 1;
  var jy = -ny; // invert Y so up is up
  if (MODE3D) {
    var ca = Math.cos(cam.ang), sa = Math.sin(cam.ang);
    var fwd = -jy, str = nx; // flip forward only for 3D
    var vx = fwd * ca + str * (-sa);
    var vy = fwd * sa + str * (ca);
    vel.x += vx * speed * 0.15 * dtScale;
    vel.y += vy * speed * 0.15 * dtScale;
  } else {
    vel.x += (nx) * speed * 0.15 * dtScale;
    vel.y += (jy) * speed * 0.15 * dtScale;
  }
}

function onCalibUpdate() {
  var thr = 0.35;
  var ok = (Math.abs(lastRoll - baseRoll) < thr) && (Math.abs(lastPitch - basePitch) < thr);
  if (hasYaw) {
    ok = ok && (Math.abs(angNorm(deg2rad(lastYaw - baseYaw))) < deg2rad(2.0));
  }
  var now = Date.now();
  if (ok) { calStableMs += 60; } else { calStableMs = 0; }
  if (calStableMs >= calNeedStableMs) {
    hasBaseline = true;
    calibrating = false;
    resetLevel(1);
  }
}

function pollIMU() {
  if (!i2cEnabled || !imuCompiled) {
    console.warn('[GAMES] IMU polling skipped: i2c=' + i2cEnabled + ' imuCompiled=' + imuCompiled);
    return;
  }
  var ts = Date.now();
  hw.fetchJSON('/api/sensors?sensor=imu&ts=' + ts).then(function(j) {
    if (!running) return;
    if (!j || !j.valid || !j.ori) return;

    var pitch = j.ori.pitch || 0;
    var roll = j.ori.roll || 0;
    var yaw = (j.ori.yaw !== undefined && j.ori.yaw !== null) ? j.ori.yaw : 0;
    lastPitch = pitch;
    lastRoll = roll;
    hasYaw = (j.ori.yaw !== undefined && j.ori.yaw !== null);
    if (hasYaw) lastYaw = yaw;

    if (DEBUG_IMU) {
      try {
        console.log('[IMU]', ts,
          'pitch=', pitch.toFixed ? pitch.toFixed(2) : pitch,
          'roll=', roll.toFixed ? roll.toFixed(2) : roll,
          'yaw=', hasYaw ? (yaw.toFixed ? yaw.toFixed(2) : yaw) : 'n/a',
          'baseP=', basePitch.toFixed ? basePitch.toFixed(2) : basePitch,
          'baseR=', baseRoll.toFixed ? baseRoll.toFixed(2) : baseRoll,
          'baseY=', baseYaw.toFixed ? baseYaw.toFixed(2) : baseYaw,
          'hasBase=', !!hasBaseline,
          'pos=(' + pos.x.toFixed(1) + ',' + pos.y.toFixed(1) + ')');
      } catch (_) {}
    }

    if (calibrating) {
      if (!hasBaseline) {
        basePitch = pitch;
        baseRoll = roll;
        if (hasYaw) baseYaw = yaw;
      }
      onCalibUpdate();
    } else {
      if (!USE_GAMEPAD && !USE_KEYBOARD && !USE_MOUSE) {
        applyTilt(pitch, roll);
      }
      if (USE_YAW && hasYaw) {
        var yawDiff = yaw - baseYaw;
        while (yawDiff > 180) yawDiff -= 360;
        while (yawDiff < -180) yawDiff += 360;
        var relYaw = deg2rad(yawDiff);
        var yawSmooth = 0.7;
        yawTarget = yawTarget * yawSmooth + relYaw * (1 - yawSmooth);
      }
      if (USE_PITCH) {
        var relPitch = deg2rad(pitch - basePitch);
        var newPitchTarget = Math.max(-0.65, Math.min(0.65, relPitch));
        var pitchSmooth = 0.6;
        pitchTarget = pitchTarget * pitchSmooth + newPitchTarget * (1 - pitchSmooth);
      }
    }
  }).catch(function(_) {});
}

function updateHudInput() {
  var el = document.getElementById('hudInput');
  if (!el) return;
  var label;
  if (USE_KEYBOARD && USE_MOUSE) label = 'KB + Mouse';
  else if (USE_KEYBOARD) label = 'Keyboard';
  else if (USE_MOUSE) label = 'Mouse';
  else if (USE_GAMEPAD) label = (CONTROL_MODE === MODE_GYRO_AIM) ? 'Gamepad (Gyro Aim)' : 'Gamepad (Stick)';
  else label = 'IMU';
  el.textContent = label;
}


// =============================================
// SECTION 7B: LINE OF SIGHT
// =============================================

// DDA ray march on the grid — returns true if no wall cell interrupts the segment
// LOS cache — quantize positions to half-cells, cache results for 200ms
var _losCache = {};
var _losCacheTime = 0;
function hasLineOfSight(x1, y1, x2, y2) {
  var now = Date.now();
  if (now - _losCacheTime > 200) { _losCache = {}; _losCacheTime = now; }
  // Quantize to half-cell precision for cache key
  var hc = cell * 0.5;
  var key = (Math.round(x1/hc)) + ',' + (Math.round(y1/hc)) + ',' + (Math.round(x2/hc)) + ',' + (Math.round(y2/hc));
  if (_losCache[key] !== undefined) return _losCache[key];
  var dx = x2 - x1, dy = y2 - y1;
  var steps = Math.ceil(Math.hypot(dx, dy) / hc);
  if (steps < 1) { _losCache[key] = true; return true; }
  for (var i = 1; i < steps; i++) {
    var t = i / steps;
    var gx = Math.floor((x1 + dx * t) / cell);
    var gy = Math.floor((y1 + dy * t) / cell);
    if (gx < 0 || gy < 0 || gx >= gridW || gy >= gridH) { _losCache[key] = false; return false; }
    if (grid && grid[gy * gridW + gx]) { _losCache[key] = false; return false; }
  }
  _losCache[key] = true; return true;
}

