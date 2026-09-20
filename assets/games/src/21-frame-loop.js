function renderFrame() {
  if (overviewActive) { drawDebugOverview(); return; }
  _pt('draw()', function(){ draw(); });
  if (MODE3D) {
    _pt('groundFX3D', function(){ drawGroundEffects3D(); });
    _pt('coneFX3D', function(){ drawConeEffects3D(); drawFlameStream3D(); });
    _pt('proj3D', function(){ drawProjectiles3D(); });
    _pt('chainFX3D', function(){ drawChainEffects3D(); });
    _pt('novaFX3D', function(){ drawNovaEffects3D(); });
    _pt('impacts3D', function(){ drawImpacts3D(); });
    _pt('enemies3D', function(){ drawEnemies3D(); });
    _pt('soulOrbs3D', function(){ drawSoulOrbs3D(); });
    _pt('arcaneTomes3D', function(){ drawArcaneTomes3D(); });
    _pt('statPickups3D', function(){ drawStatPickups3D(); });
    _pt('companions3D', function(){ drawCompanions3D(); });
    _pt('fortressAllies3D', function(){ drawFortressAllies3D(); });
    _pt('coins3D', function(){ drawCoins3D(); });
    _pt('marketStalls3D', function(){ drawMarketStalls3D(); });
    _pt('shrines3D', function(){ drawShrines3D(); });
    _pt('ruins3D', function(){ drawRuins3D(); });
    _pt('structures3D', function(){ drawStructures3D(); });
    _pt('torchGlow3D', function(){ drawTorchGlow3D(); });
    _pt('ambientFX3D', function(){ drawAmbientParticles3D(); });
    _pt('fpsArms', function(){ drawFPSArms(); });
    _pt('arenaAltarPrompt', function(){ drawArenaAltarPrompt3D(); });
    _pt('arenaHUD', function(){ drawArenaHUD(); });
    _pt('forgeOverlay', function(){ drawForgeOverlay(); });
    // ── Crosshair — pitch-aware aiming reticle ──
    _pt('crosshair3D', function(){
      var w = canvas.width, h = canvas.height;
      var S = resScale;
      var cx = Math.floor(w / 2);
      var pitchPx = Math.floor(-(cam.pitch || 0) * projScale);
      var cy = Math.floor(h / 2) + pitchPx;
      var sz = 10 * S, gap = 4 * S;
      ctx.save();
      ctx.strokeStyle = 'rgba(255,255,255,0.7)';
      ctx.lineWidth = 2 * S;
      ctx.beginPath();
      ctx.moveTo(cx - sz, cy); ctx.lineTo(cx - gap, cy);
      ctx.moveTo(cx + gap, cy); ctx.lineTo(cx + sz, cy);
      ctx.moveTo(cx, cy - sz); ctx.lineTo(cx, cy - gap);
      ctx.moveTo(cx, cy + gap); ctx.lineTo(cx, cy + sz);
      ctx.stroke();
      // dot center
      ctx.fillStyle = 'rgba(255,255,255,0.5)';
      ctx.beginPath(); ctx.arc(cx, cy, 1.5 * S, 0, Math.PI * 2); ctx.fill();

      // ── Dash cooldown arc (bottom-right of crosshair) ──
      var _now = Date.now();
      if (equipment.boots && equipment.boots.canDash && _now < dashCooldownUntil) {
        var cdFrac = (dashCooldownUntil - _now) / 3000;
        ctx.strokeStyle = 'rgba(255,180,60,0.7)';
        ctx.lineWidth = 2.5 * S;
        ctx.beginPath();
        ctx.arc(cx + 18 * S, cy + 14 * S, 7 * S, -Math.PI/2, -Math.PI/2 + cdFrac * Math.PI * 2);
        ctx.stroke();
        ctx.fillStyle = 'rgba(255,180,60,0.5)';
        ctx.font = Math.floor(8 * S) + 'px Arial'; ctx.textAlign = 'center';
        ctx.fillText(Math.ceil(cdFrac * 3) + 's', cx + 18 * S, cy + 17 * S);
      }

      // ── Jump indicator (bottom-left of crosshair) ──
      if (equipment.boots && equipment.boots.canJump && jumpAirborne) {
        ctx.fillStyle = 'rgba(120,200,255,0.6)';
        ctx.font = Math.floor(9 * S) + 'px Arial'; ctx.textAlign = 'center';
        ctx.fillText('AIR', cx - 18 * S, cy + 17 * S);
      }

      ctx.restore();
    });
    if (DEBUG_SKELETON) _pt('skelDebug', function(){ drawSkeletonDebug(); });
  } else {
    _pt('groundFX2D', function(){ drawGroundEffects2D(); });
    _pt('coneFX2D', function(){ drawConeEffects2D(); drawFlameStream2D(); });
    _pt('proj2D', function(){ drawProjectiles2D(); });
    _pt('chainFX2D', function(){ drawChainEffects2D(); });
    _pt('novaFX2D', function(){ drawNovaEffects2D(); });
    _pt('impacts2D', function(){ drawImpacts2D(); });
    _pt('coins2D', function(){ drawCoins2D(); });
    _pt('shopMarker2D', function(){ drawShopMarker2D(); });
    _pt('caveEntrance2D', function(){ drawCaveEntrance2D(); });
  }
  _pt('hudOverlay', function(){ drawHudOverlay(); });
  _pt('shopOverlay', function(){ drawShopOverlay(); });
  _pt('menuOverlay', function(){ drawMenuOverlay(); });
  _pt('settingsOverlay', function(){ drawSettingsOverlay(); });
  if (gameOverState) _pt('gameOver', function(){ drawGameOver(); });

  // Perf HUD overlay — drawn last so it sits on top of everything
  if (DEBUG_PERF_HUD) drawPerfHud();
  if (DEBUG_CAVE) drawCaveDebug();
  if (caveControlMapVisible()) _pt('caveControlMap', function(){ drawCaveControlMap(); });

  // Auto-dump every 5s
  var now = Date.now();
  if (now - _perfBreakdownLog > 5000 && Object.keys(_perfBreakdown).length > 0) {
    _perfBreakdownLog = now;
    dumpPerfBreakdown();
  }
}

var FIXED_DT = GAME_CONFIG.physics.fixedDt;
var FIXED_DT_MS = GAME_CONFIG.physics.fixedDtMs;
var _physicsAccum = 0;
var MAX_PHYSICS_STEPS = GAME_CONFIG.physics.maxSteps;

// Generation counter — incremented each time a new loop is intentionally started.
// Every rAF call captures its generation at scheduling time; if the generation no
// longer matches when the callback fires, it means a newer loop has taken over and
// this stale callback simply discards itself. This prevents multiple concurrent
// game loops from accumulating across Start / Endless-Mode presses.
var _loopGen = 0;

function _scheduleLoop() {
  var gen = _loopGen;
  requestAnimationFrame(function(ts) { loop(ts, gen); });
}

function loop(ts, gen) {
  if (!running || gen !== _loopGen) return; // stale — a newer loop is in charge
  try {
  var loopStart = performance.now();
  recordPerfFrameCadence(ts, gen);
  // Advance perf ring + zero this slot's stages before _pt writes this frame
  _perfRingIdx = (_perfRingIdx + 1) % PERF_HISTORY_LEN;
  _perfFrameTotals[_perfRingIdx] = 0;
  for (var _pk in _perfStageHistory) _perfStageHistory[_pk][_perfRingIdx] = 0;
  var elapsed = lastUpdate ? (ts - lastUpdate) : 16;
  var dt = elapsed / 1000;
  lastUpdate = ts;

  // Input runs at render rate for responsiveness
  _pt('handleInput', function(){ handleInput(FIXED_DT); });

  // Physics runs at fixed timestep for determinism
  if (!menuOpen) {
    _physicsAccum += elapsed;
    var steps = 0;
    while (_physicsAccum >= FIXED_DT_MS && steps < MAX_PHYSICS_STEPS) {
      _pt('gameUpdate', function(){ gameUpdate(FIXED_DT); });
      _physicsAccum -= FIXED_DT_MS;
      steps++;
    }
    if (_physicsAccum > FIXED_DT_MS * MAX_PHYSICS_STEPS) _physicsAccum = 0; // clamp after long pause
  }
  renderFrame();

  // Performance logging
  if (DEBUG_PERF) {
    var loopEnd = performance.now();
    var frameTime = loopEnd - loopStart;
    __perfFrames.push({total:frameTime, dt:dt * 1000, proj:projectiles.length, cone:coneEffects.length, enemy:enemies.length, impacts:impacts.length, gpValid:gpLast.valid, gpX:gpLast.x, gpY:gpLast.y});
    var now = Date.now();
    if (now - __perfLastLog > __perfLogInterval && __perfFrames.length > 0) {
      var avgTotal = 0, avgDt = 0, maxTotal = 0;
      for (var i = 0; i < __perfFrames.length; i++) {
        var f = __perfFrames[i];
        avgTotal += f.total; avgDt += f.dt;
        if (f.total > maxTotal) maxTotal = f.total;
      }
      var n = __perfFrames.length;
      avgTotal /= n; avgDt /= n;
      var lastF = __perfFrames[__perfFrames.length - 1];
      try {
        //console.log('[PERF]', 'frames=' + n, // TEMP DISABLED
        //  'avgTotal=' + avgTotal.toFixed(1) + 'ms', 'maxTotal=' + maxTotal.toFixed(1) + 'ms',
        //  'avgDt=' + avgDt.toFixed(1) + 'ms',
        //  'proj=' + lastF.proj, 'cone=' + lastF.cone, 'enemy=' + lastF.enemy, 'impacts=' + lastF.impacts,
        //  'gpValid=' + lastF.gpValid, 'gpX=' + lastF.gpX.toFixed(2), 'gpY=' + lastF.gpY.toFixed(2));
      } catch (_) {}
      __perfFrames = []; __perfLastLog = now;
    }
  }

  // Periodic state dump for distance debugging
  if (ENDLESS_MODE && Math.random() < 0.005) {
    var _trX = pos.x + windowOriginX, _trY = pos.y + windowOriginY;
    var _dist = Math.floor(Math.hypot(_trX, _trY));
    var _errTotal = 0; for (var ek in _ptErrors) _errTotal += _ptErrors[ek];
    if (_errTotal > 0 || _dist > 1500) {
      console.warn('[STATE @' + _dist + 'm] pos=(' + pos.x.toFixed(0) + ',' + pos.y.toFixed(0) +
        ') origin=(' + windowOriginX + ',' + windowOriginY + ')' +
        ' grid=' + gridW + 'x' + gridH + ' walls=' + walls.length +
        ' enemies=' + enemies.length + ' health=' + Math.floor(health) +
        ' canvasW=' + canvas.width + ' canvasH=' + canvas.height +
        ' cam.z=' + (cam.z || 0).toFixed(1) + ' floorZ=' + (pos.floorZ || 0).toFixed(1) +
        ' errors=' + JSON.stringify(_ptErrors) +
        ' loopErrors=' + (window._loopErrCount || 0));
    }
  }
  // Commit this frame's total wall-time to the perf ring
  var _frameTotal = performance.now() - loopStart;
  _perfFrameTotals[_perfRingIdx] = _frameTotal;
  _perfTickSecondBucket();
  _perfSecFrameCount[_perfSecIdx]++;
  _perfSecTotalMs[_perfSecIdx] += _frameTotal;
  } catch (loopErr) {
    if (!window._loopErrCount) window._loopErrCount = 0;
    window._loopErrCount++;
    if (window._loopErrCount <= 5 || window._loopErrCount % 100 === 0) {
      console.error('[LOOP CRASH #' + window._loopErrCount + ']', loopErr.message, '\n', loopErr.stack);
    }
  }
  _scheduleLoop();
}
