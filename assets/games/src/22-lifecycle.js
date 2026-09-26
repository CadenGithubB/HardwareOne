// =============================================
// SECTION 18: GAME LIFECYCLE
// =============================================

var gamePauseReason = '';
var gameSessionStarted = false;

function clearGameplayInput() {
  kbState = {up:false, down:false, left:false, right:false};
  _arrowCam = {up:false, down:false, left:false, right:false};
  mouseAccum = {x:0, y:0}; mouseDelta = {x:0, y:0};
  _mouseHeld = false; _attackHeld = false; flameStreamActive = false;
  jumpPressed = false; dashPressed = false;
  // A paused player must not accelerate from stale input when play resumes.
  vel.x = 0; vel.y = 0;
}

function updateGameSessionUi() {
  var paused = running && menuOpen;
  var status = document.getElementById('gameSessionState');
  var hint = document.getElementById('gameSessionHint');
  var pauseButton = document.getElementById('btnStop');
  var startButton = document.getElementById('btnStart');
  if (pauseButton) {
    pauseButton.textContent = paused ? 'Resume' : 'Pause';
    pauseButton.disabled = !running || gameOverState;
    pauseButton.setAttribute('aria-pressed', paused ? 'true' : 'false');
  }
  if (startButton) startButton.textContent = gameSessionStarted ? 'New maze run' : 'Play maze';
  if (status) {
    status.textContent = gameOverState ? 'Run complete' : paused ? 'Paused' : running ? 'Exploring' : 'Ready to explore';
    status.setAttribute('data-state', paused ? 'paused' : running ? 'playing' : 'ready');
  }
  if (hint) {
    if (gameOverState) hint.textContent = 'Choose a new maze run or explore endless to begin again.';
    else if (paused) hint.textContent = 'Your run is paused. Click the view or press P to resume.';
    else if (!running) hint.textContent = 'Choose Play maze or Explore endless, then click the view to take control.';
    else if (USE_KEYBOARD || USE_MOUSE) hint.textContent = document.pointerLockElement === canvas ?
      'WASD move · mouse look · click cast · E interact · P or Esc pause' :
      'Click the view for mouse look · WASD move · arrows look · E interact · P pause';
    else hint.textContent = 'Hardware controls active · click the view to use keyboard and mouse.';
  }
}

function pauseGame(reason) {
  clearGameplayInput();
  if (!running || gameOverState) return;
  if (!menuOpen) gamePauseReason = reason || 'manual';
  menuOpen = true;
  _physicsAccum = 0; lastUpdate = 0;
  if (typeof cancelPendingMissileCasts === 'function') cancelPendingMissileCasts(true);
  if (document.pointerLockElement === canvas) document.exitPointerLock();
  updateGameSessionUi();
}

function resumeGame() {
  if (!running || gameOverState) return;
  clearGameplayInput();
  menuOpen = false; gamePauseReason = '';
  _physicsAccum = 0; lastUpdate = 0;
  updateGameSessionUi();
}

function toggleGamePause() {
  if (menuOpen) resumeGame();
  else pauseGame('manual');
}

function prepareGameRun() {
  clearGameplayInput();
  gameSessionStarted = true;
  menuOpen = false; gamePauseReason = '';
  settingsOpen = false; shopOpen = false; forgeOpen = false;
  toastLogOpen = false; overviewActive = false;
  _physicsAccum = 0; lastUpdate = 0;
}

function startGame() {
  prepareGameRun();
  if (typeof cancelPendingMissileCasts === 'function') cancelPendingMissileCasts(false);
  // If Cave Test is selected in the terrain dropdown, delegate to the cave test launcher
  var _tSel = document.getElementById('terrainSelect');
  if (_tSel && _tSel.value === 'cavetest') {
    // Trigger the terrain selector change handler which does the full cave test setup
    _tSel.dispatchEvent(new Event('change'));
    // Still need to start the game loop
    var useHardware = !USE_KEYBOARD && !USE_MOUSE && i2cEnabled && imuCompiled && imuRunning && inputRunning;
    hasBaseline = true;
    calibrating = false;
    lastUpdate = 0;
    CONTROL_MODE = MODE_STICK_AIM;
    lastSelectDown = false;
    console.log('[CTRL] start (cave test) CONTROL_MODE=', CONTROL_MODE, 'useHardware=', useHardware);
    if (useHardware) {
      startCalibration();
      hasBaseline = false; calStableMs = 0; calibrating = true;
      Promise.all([controlSensor('imu', 'start'), controlSensor('gamepad', 'start')]).then(function() {
        polling = setInterval(pollIMU, 60);
        _loopGen++; _scheduleLoop();
      });
    } else {
      _loopGen++; _scheduleLoop();
    }
    var chk = document.getElementById('chkCamFollow');
    if (chk && chk.checked) CAM_FOLLOW = true;
    if (USE_GAMEPAD) startGamepadPolling();
    updateHudInput();
    updateGameSessionUi();
    return;
  }

  var useHardware = !USE_KEYBOARD && !USE_MOUSE && i2cEnabled && imuCompiled && imuRunning && inputRunning;
  gameOverState = false;
  running = true;
  ENDLESS_MODE = false;
  equipment = {armor: null, hat: null, robes: null};
  inventoryOpen = false; inventorySelIdx = -1;
  resetLevel(1);
  hasBaseline = true;
  calibrating = false;
  lastUpdate = 0;
  CONTROL_MODE = MODE_STICK_AIM;
  lastSelectDown = false;
  console.log('[CTRL] start CONTROL_MODE=', CONTROL_MODE, 'useHardware=', useHardware);
  draw();
  // Pointer lock must be requested from a direct canvas click gesture — see canvas click handler
  if (useHardware) {
    startCalibration();
    hasBaseline = false; calStableMs = 0; calibrating = true;
    Promise.all([controlSensor('imu', 'start'), controlSensor('gamepad', 'start')]).then(function() {
      polling = setInterval(pollIMU, 60);
      _loopGen++; _scheduleLoop();
    });
  } else {
    _loopGen++; _scheduleLoop();
  }
  var chk = document.getElementById('chkCamFollow');
  if (chk && chk.checked) CAM_FOLLOW = true;
  if (USE_GAMEPAD) startGamepadPolling();
  updateHudInput();
  updateGameSessionUi();
}

function stopGame() {
  if (typeof cancelPendingMissileCasts === 'function') cancelPendingMissileCasts(true);
  running = false;
  clearGameplayInput();
  menuOpen = false; gamePauseReason = '';
  _loopGen++;
  if (document.pointerLockElement === canvas) document.exitPointerLock();
  if (polling) { try { clearInterval(polling); } catch (_) {} polling = null; }
  stopGamepadPolling();
  controlSensor('imu', 'stop');
  updateGameSessionUi();
}

function fwDebugOn() {
  FW_DEBUG = true;
  var cmds = ['debugsensorsgeneral 1', 'debugimudata 1'];
  Promise.all(cmds.map(function(c) {
    return hw.postFormText('/api/cli', { cmd: c }).catch(function(_) { return ''; });
  })).then(function() { startLogPoller(); });
}

function fwDebugOff() {
  FW_DEBUG = false;
  var cmds = ['debugsensorsgeneral 0', 'debugimudata 0'];
  Promise.all(cmds.map(function(c) {
    return hw.postFormText('/api/cli', { cmd: c }).catch(function(_) { return ''; });
  })).then(function() { stopLogPoller(); });
}

function startLogPoller() {
  if (__gamesLogPoll) return;
  __gamesLogLastLen = 0;
  __gamesLogPoll = setInterval(function() {
    if (!FW_DEBUG) return;
    hw.fetchText('/api/cli/logs').then(function(t) {
      if (typeof t !== 'string') return;
      var s = t;
      if (s.length > __gamesLogLastLen) {
        var delta = s.substring(__gamesLogLastLen);
        __gamesLogLastLen = s.length;
        var lines = delta.split(/\\n/);
        for (var i = 0; i < lines.length; i++) {
          var ln = lines[i];
          if (!ln) continue;
          if (ln.indexOf('[DEBUG_SENSORS') >= 0 || /IMU|BNO|ori|gyro|accel/i.test(ln)) {
            try { console.log('[FW]', ln); } catch (_) {}
          }
        }
      }
    }).catch(function(_) {});
  }, 800);
}

function stopLogPoller() {
  if (__gamesLogPoll) { try { clearInterval(__gamesLogPoll); } catch (_) {} __gamesLogPoll = null; }
}
