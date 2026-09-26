// =============================================
// SECTION 17: GAME LOOP
// =============================================

function handleInput(dt) {
  var btn = gpLast.buttons | 0;

  // Select button: toggle control mode
  var selectDown = ((btn & BTN_SELECT) === 0);
  if (selectDown && !lastSelectDown) {
    var now = Date.now();
    if (now - lastSelectToggleMs > SELECT_TOGGLE_COOLDOWN) {
      CONTROL_MODE = (CONTROL_MODE === MODE_GYRO_AIM) ? MODE_STICK_AIM : MODE_GYRO_AIM;
      lastSelectToggleMs = now;
      console.log('[CTRL] toggled CONTROL_MODE=', CONTROL_MODE);
      updateHudInput();
    }
  }
  lastSelectDown = selectDown;

  // Start button: toggle menu
  if (gpLast && gpLast.valid) {
    var startMenuDown = ((btn & BTN_START) === 0);
    if (startMenuDown && !lastStartMenuDown) {
      var now = Date.now();
      if (now - lastStartMenuToggleMs > START_MENU_COOLDOWN) {
        menuOpen = !menuOpen;
        lastStartMenuToggleMs = now;
        console.log('[MENU] toggled menuOpen=', menuOpen);
      }
    }
    lastStartMenuDown = startMenuDown;
  }

  // X-button yaw capture (only in Gyro Aim + 3D + Gamepad)
  if (MODE3D && CONTROL_MODE === MODE_GYRO_AIM && USE_GAMEPAD && gpLast && gpLast.valid) {
    var xPressed = ((gpLast.buttons & BTN_X) === 0);
    if (xPressed && !prevX) {
      capturingYaw = true; camHold = cam.ang; yawAtPress = yawTarget;
      if (DEBUG_CAM) {
        try { console.log('[CAM] press X', 'camHold=', camHold.toFixed(3), 'yawAtPress=', yawAtPress.toFixed ? yawAtPress.toFixed(3) : yawAtPress, 'yawOffset=', yawOffset.toFixed ? yawOffset.toFixed(3) : yawOffset); } catch (_) {}
      }
    } else if (!xPressed && prevX) {
      capturingYaw = false;
      var delta = angNorm(yawTarget - yawAtPress);
      yawOffset += delta;
      yawResumeUntil = Date.now() + 250;
      if (DEBUG_CAM) {
        try { console.log('[CAM] release X', 'yawTarget=', yawTarget.toFixed ? yawTarget.toFixed(3) : yawTarget, 'yawAtPress=', yawAtPress.toFixed ? yawAtPress.toFixed(3) : yawAtPress, 'delta=', delta.toFixed ? delta.toFixed(3) : delta, 'newOffset=', yawOffset.toFixed ? yawOffset.toFixed(3) : yawOffset); } catch (_) {}
      }
    }
    prevX = xPressed;
  }

  // Route inputs by control mode
  if (!calibrating) {
    // Keyboard input (also active in mouse mode — WASD drives movement)
    if (USE_KEYBOARD || USE_MOUSE) {
      var knx = (kbState.right ? 1 : 0) - (kbState.left ? 1 : 0);
      var kny = (kbState.up ? 1 : 0) - (kbState.down ? 1 : 0);
      if (knx !== 0 || kny !== 0) {
        var kmag = Math.hypot(knx, kny);
        applyGamepad(knx / kmag, kny / kmag, dt);
      }
    }

    // Mouse input — controls camera rotation; WASD (above) handles movement
    if (USE_MOUSE) {
      var mdx = mouseDelta.x, mdy = mouseDelta.y;
      mouseDelta.x = 0; mouseDelta.y = 0;
      if (mdx !== 0 || mdy !== 0) {
        cam.ang += mdx * MOUSE_CAM_SENSITIVITY;
        cam.pitch = clamp((cam.pitch || 0) + mdy * MOUSE_CAM_SENSITIVITY, -CAM_PITCH_MAX, CAM_PITCH_MAX);
      }
      // Continuous spell casting while mouse held (all spell types)
      if (_mouseHeld && !shopOpen && !menuOpen && !gameOverState) {
        castCurrentSpell();
      } else if (DEBUG_COMBAT && _mouseHeld) {
        console.log('[COMBAT] _mouseHeld but blocked: shop=' + shopOpen + ' menu=' + menuOpen + ' gameOver=' + gameOverState);
      }
    }
    // Continuous spell casting while E key held (keyboard attack)
    if (_attackHeld && !shopOpen && !menuOpen && !gameOverState) {
      castCurrentSpell();
    }

    // Arrow key camera — yaw left/right, pitch up/down
    if (_arrowCam.left || _arrowCam.right || _arrowCam.up || _arrowCam.down) {
      var arrowSpeed = 2.0 * (dt || 0.016);
      if (_arrowCam.left)  cam.ang -= arrowSpeed;
      if (_arrowCam.right) cam.ang += arrowSpeed;
      if (_arrowCam.up)    cam.pitch = Math.max(-CAM_PITCH_MAX, (cam.pitch || 0) - arrowSpeed * 0.6);
      if (_arrowCam.down)  cam.pitch = Math.min(CAM_PITCH_MAX, (cam.pitch || 0) + arrowSpeed * 0.6);
    }

    // Gamepad input — skip when KB+Mouse is active to prevent phantom button presses
    if (USE_GAMEPAD && !USE_MOUSE && gpLast && gpLast.valid) {

      // ── Shop overlay — handle buttons before movement/spell logic ───────
      // Joystick axes are NOT consumed here, so movement always works.
      // Walking out of range auto-closes the shop.
      var gpShopBtns = gpLast.buttons | 0;
      var gpShopA = ((gpShopBtns & BTN_A) === 0);
      var gpShopB = ((gpShopBtns & BTN_B) === 0);
      var gpShopY = ((gpShopBtns & BTN_Y) === 0);
      var gpShopX = ((gpShopBtns & BTN_X) === 0);
      if (!shopOpen && nearestOpenChest && gpShopA && !lastShopGpA) {
        collectChest(nearestOpenChest);                                    // A: collect chest
      } else if (!shopOpen && nearestShrine && gpShopA && !lastShopGpA) {
        activateShrine(nearestShrine);                                     // A: activate shrine
      } else if (!shopOpen && shopNearby && gpShopA && !lastShopGpA) {
        shopOpen = true; shopSelIdx = 0;                                   // A: open shop
      } else if (shopOpen) {
        var _vis = getVisibleShopItems();
        if (gpShopY && !lastShopGpY)
          shopSelIdx = (shopSelIdx + 1) % _vis.length;              // Y: next item
        if (gpShopX && !lastShopGpX)
          shopSelIdx = (shopSelIdx - 1 + _vis.length) % _vis.length; // X: prev
        if (gpShopA && !lastShopGpA)
          applyShopItem(_vis[shopSelIdx]);                           // A: buy
        if (gpShopB && !lastShopGpB)
          { shopOpen = false; shopSelIdx = 0; }                            // B: close
      }
      lastShopGpA = gpShopA; lastShopGpY = gpShopY;
      lastShopGpX = gpShopX; lastShopGpB = gpShopB;

      if (!MODE3D) {
        _handleGamepad2D(dt);
      } else {
        _handleGamepad3D(dt);
      }
    }
  }
}

// Gamepad input for 2D mode — joystick movement + XYAB spell casting
function _handleGamepad2D(dt) {
  var now2 = Date.now();
  var nx = gpLast.x, ny = -gpLast.y;
  var mag = Math.hypot(nx, ny);
  if (!shopOpen && mag > 0.1) {
    var dtScale = dt > 0 ? (dt / 0.060) : 1;
    vel.x += nx * speed * 0.12 * dtScale;
    vel.y += ny * speed * 0.12 * dtScale;
  }
  if (shopOpen) return;

  var btn2d = gpLast.buttons | 0;
  var xDown = ((btn2d & BTN_X) === 0);
  var aDown = ((btn2d & BTN_A) === 0);
  if (xDown && !lastXDown) cycleSpell();
  lastXDown = xDown;
  if (aDown) {
    _tryCastSpell(now2);
  } else { flameStreamActive = false; }
  lastADown = aDown;
}

// Gamepad input for 3D mode — gyro/stick aim + spell casting
function _handleGamepad3D(dt) {
  var now2 = Date.now();
  if (menuOpen) return;

  if (CONTROL_MODE === MODE_GYRO_AIM) {
    if (!shopOpen) applyGamepad(gpLast.x, gpLast.y, dt);
    if (shopOpen) return;

    var btn3d = gpLast.buttons | 0;
    var yDown = ((btn3d & BTN_Y) === 0);
    var aDown3d = ((btn3d & BTN_A) === 0);
    if (yDown && !lastXDown) cycleSpell();
    lastXDown = yDown;
    if (aDown3d) _tryCastSpell(now2);
    else flameStreamActive = false;
    lastADown = aDown3d;
    return;
  }

  // Stick Aim mode
  if (!shopOpen) {
    var nx = gpLast.x, ny = gpLast.y;
    cam.ang += nx * STICK_CAM_YAW_SPEED * dt;
    cam.pitch = clamp((cam.pitch || 0) + (-ny) * STICK_CAM_PITCH_SPEED * dt, -CAM_PITCH_MAX, CAM_PITCH_MAX);
  }
  if (shopOpen) return;

  var btn = gpLast.buttons | 0;
  var mvF = ((btn & BTN_X) === 0), mvB = ((btn & BTN_B) === 0);
  if (mvF || mvB) {
    var fwd = (mvF ? 1 : 0) + (mvB ? -1 : 0);
    var ca = Math.cos(cam.ang), sa = Math.sin(cam.ang);
    var dtScale = dt > 0 ? (dt / 0.060) : 1;
    vel.x += fwd * ca * speed * 0.10 * dtScale;
    vel.y += fwd * sa * speed * 0.10 * dtScale;
  }
  var yDown = ((btn & BTN_Y) === 0);
  var aDown = ((btn & BTN_A) === 0);
  if (yDown && !lastXDown) cycleSpell();
  lastXDown = yDown;
  if (aDown) _tryCastSpell(now2);
  else flameStreamActive = false;
  lastADown = aDown;
}

// Shared spell casting logic — fires current spell if mana/cooldown allow
function _tryCastSpell(now) {
  var spell = getCurrentSpell();
  if (spell.id === 'missile' && missileCastingBlocked()) return;
  // Stream attacks (flamethrower) tick continuously while held
  if (spell.attackType === 'stream') {
    var tickInterval = spell.streamTickMs || 80;
    if (equipment.relic && equipment.relic.effect === 'cooldownReduction') tickInterval = Math.round(tickInterval * (1 - equipment.relic.value));
    if (mana >= spell.manaCost && (now - flameStreamLastTick) >= tickInterval) {
      castStreamAttack(spell);
      var manaCostMult = (equipment.robes && equipment.robes.manaCostReduction) ? (1 - equipment.robes.manaCostReduction) : 1;
      var cost = spell.manaCost * manaCostMult;
      mana -= cost; stats.totalManaConsumed += cost;
      flameStreamLastTick = Math.max(flameStreamLastTick + tickInterval, now - 16);
      flameStreamActive = true;
      castAnimUntil = now + 120;
    } else if (mana < spell.manaCost && !lastADown) {
      manaBlinkUntil = now + 1000;
      flameStreamActive = false;
    }
    return;
  }
  var _effCD2 = getEffectiveCooldown();
  if (mana >= spell.manaCost && (now - lastShotMs) >= _effCD2) {
    var previousShotMs = lastShotMs;
    var windupMissile = spell.id === 'missile' && spell.attackType === 'projectile';
    if (spell.attackType === 'cone') castConeAttack(spell);
    else if (!windupMissile) spawnProjectile(spell.speed, PROJ_RADIUS);
    mana -= spell.manaCost; stats.totalManaConsumed += spell.manaCost;
    // Snap to ideal cooldown boundary to prevent held-fire drift
    lastShotMs = Math.max(lastShotMs + _effCD2, now - 16);
    spellCastReservationSerial++;
    if (windupMissile) reserveMissileCast(spell, now, 1, spell.manaCost, previousShotMs);
    if (typeof noteFirstPersonCast === 'function') noteFirstPersonCast(spell, now);
  } else if (mana < spell.manaCost && !lastADown) {
    manaBlinkUntil = now + 1000;
  }
}

// Pairwise enemy separation — spatial hash for O(n) average instead of O(n²)
var _sepGrid = {};
function separateEnemies() {
  if (!enemies || enemies.length < 2) return;
  var SEP_CELL = 30; // >= max minDist (28px for two Brutes)
  // Interest management: only hash enemies near the player — distant ones
  // aren't moving (gated in updateEnemies) so they don't need separation.
  var _sepR = viewDist * 1.5;
  var _sepRSq = _sepR * _sepR;
  // Build spatial hash
  var sg = _sepGrid; for (var k in sg) delete sg[k];
  for (var i = 0; i < enemies.length; i++) {
    var e = enemies[i];
    var _sdx = pos.x - e.x, _sdy = pos.y - e.y;
    if (_sdx * _sdx + _sdy * _sdy > _sepRSq) continue;
    var gk = (Math.floor(e.x / SEP_CELL)) + ',' + (Math.floor(e.y / SEP_CELL));
    if (!sg[gk]) sg[gk] = [];
    sg[gk].push(i);
  }
  // Check only within neighboring cells
  for (var key in sg) {
    var bucket = sg[key];
    var parts = key.split(',');
    var cx0 = parseInt(parts[0]), cy0 = parseInt(parts[1]);
    // Same-cell pairs
    for (var ii = 0; ii < bucket.length; ii++) {
      for (var jj = ii + 1; jj < bucket.length; jj++) {
        _separatePair(enemies[bucket[ii]], enemies[bucket[jj]]);
      }
    }
    // Neighbor cells (only 4 directions to avoid double-checking)
    var nbrs = [[1,0],[0,1],[1,1],[1,-1]];
    for (var ni = 0; ni < 4; ni++) {
      var nk = (cx0 + nbrs[ni][0]) + ',' + (cy0 + nbrs[ni][1]);
      var nb = sg[nk];
      if (!nb) continue;
      for (var ii2 = 0; ii2 < bucket.length; ii2++) {
        for (var jj2 = 0; jj2 < nb.length; jj2++) {
          _separatePair(enemies[bucket[ii2]], enemies[nb[jj2]]);
        }
      }
    }
  }
}
function _separatePair(a, b) {
  var dx = b.x - a.x, dy = b.y - a.y;
  var dist = Math.hypot(dx, dy);
  var minDist = (a.enemyType.size + b.enemyType.size) * 10;
  if (dist >= minDist || dist < 0.01) return;
  var push = (minDist - dist) * 0.35;
  var inv = 1 / dist;
  var px = dx * inv * push, py = dy * inv * push;
  var axNew = a.x - px, ayNew = a.y - py;
  var agx = Math.floor(axNew / cell), agy = Math.floor(ayNew / cell);
  if (!(agx < 0 || agy < 0 || agx >= gridW || agy >= gridH || (grid && grid[agy * gridW + agx]))) {
    a.x = axNew; a.y = ayNew;
  }
  var bxNew = b.x + px, byNew = b.y + py;
  var bgx = Math.floor(bxNew / cell), bgy = Math.floor(byNew / cell);
  if (!(bgx < 0 || bgy < 0 || bgx >= gridW || bgy >= gridH || (grid && grid[bgy * gridW + bgx]))) {
    b.x = bxNew; b.y = byNew;
  }
}

// Keep the daylight solution separate from the camera's cave exposure.  The
// legacy renderer stored both in ambientLight, which meant that entering a
// cave darkened sunlit terrain visible through the mouth.  Cave-aware passes
// use getCaveRenderLightAt(); surface passes use renderSurfaceAmbient.
var renderSurfaceAmbient = 0.9;
var renderSurfaceSunIntensity = 0.6;
var renderSurfaceFogFloor = 0.3;
var renderSurfaceWallShadeN = 0.8;
var renderSurfaceWallShadeS = 0.8;
var renderSurfaceWallShadeE = 1.0;
var renderSurfaceWallShadeW = 1.0;
var renderSurfaceTopShade = 1.15;
var renderCaveAmbient = 0.45;
var renderCaveFogFloor = 0.15;
var renderCameraCaveBlend = 0;

function _renderSmooth01(t) {
  t = Math.max(0, Math.min(1, t));
  return t * t * (3 - 2 * t);
}

function getRenderCameraFeetH() {
  var z = (typeof cam !== 'undefined' && typeof cam.z === 'number' && isFinite(cam.z)) ?
    cam.z : ((typeof pos.floorZ === 'number' && isFinite(pos.floorZ)) ? pos.floorZ : 60);
  return (z - 60) / 40;
}

function isRenderCameraUnderground() {
  if (typeof getCaveSpaceAt !== 'function') return !!playerUnderground;
  return !!getCaveSpaceAt(cam.x, cam.y, getRenderCameraFeetH()).underground;
}

// Daylight fades over the first part of the covered passage.  Portal geometry
// owns the mouth frame and signed axial coordinate; lighting only consumes it.
// A covered point outside every portal footprint is a deep-interior point.
function getPortalRenderBlendAt(x, y, covered) {
  if (!covered) return 0;
  return getCavePortalDepthBlendAt(x, y,
    typeof deepCaveEntrances !== 'undefined' ? deepCaveEntrances : []);
}

// Explicit-coordinate form shared by generation and rendering. Entrances and
// XY must use the same frame: world-space while authoring a chunk, window-local
// for lighting. No player/camera state participates in a surface's material.
function getCavePortalDepthBlendAt(x, y, entrances) {
  var best = 1;
  var found = false;
  if (typeof sampleCavePortal === 'function' && entrances) {
    for (var i = 0; i < entrances.length; i++) {
      var e = entrances[i];
      var s = sampleCavePortal(e, x, y);
      if (!s) continue;
      var fadeLen = Math.max(60, Math.min(150, (e.innerLength || 240) * 0.6));
      // A stamped roof can reach one mesh cell outward from the mathematical
      // plane. Treat that fringe as the bright mouth (depth zero), not as a
      // deep interior point; otherwise ambient flickers dark→bright→dark while
      // crossing adjacent +along/-along cells.
      var b = _renderSmooth01(Math.max(0, -s.along) / fadeLen);
      if (b < best) best = b;
      found = true;
    }
  }
  return found ? best : 1;
}

// Author one stone albedo per mesh vertex. Mouth rock starts with the actual
// neighboring terrain palette and becomes warm-neutral stone farther inward.
// Floor, walls, ceiling and cut faces share this palette; orientation and
// illumination are shading, not alternate material definitions. Fine grain and
// broad damp/rust/worn fields use absolute world coordinates, never window
// indices or chunk RNG consumption order, so rebuilding cannot recolor a rock.
function sampleCaveMaterialColor(surfaceHex, worldX, worldY, worldEntrances) {
  var stone = GAME_MATERIALS.caveStone;
  var surface = typeof surfaceHex === 'number' ? surfaceHex :
    parseInt(typeof surfaceHex === 'string' ? surfaceHex.slice(1) : '', 16);
  if (!isFinite(surface)) surface = stone.packed.base;
  var blend = getCavePortalDepthBlendAt(worldX, worldY, worldEntrances);
  var authored = sampleInteriorStoneAlbedo(stone.packed.base, worldX, worldY);
  var r = Math.max(0, Math.min(255, Math.round(((surface >> 16) & 255) * (1 - blend) + ((authored >> 16) & 255) * blend)));
  var g = Math.max(0, Math.min(255, Math.round(((surface >> 8) & 255) * (1 - blend) + ((authored >> 8) & 255) * blend)));
  var b = Math.max(0, Math.min(255, Math.round((surface & 255) * (1 - blend) + (authored & 255) * blend)));
  return (r << 16) | (g << 8) | b;
}

function caveMaterialColorHex(color) {
  return '#' + ('000000' + (color >>> 0).toString(16)).slice(-6);
}

// Constant-time material lookup for a window-local rendered face midpoint.
// The bounded array belongs to the mesh and is replaced with it. In particular,
// this must not query biome/noise, portals or camera state for every wall face.
function getCaveMaterialColorAt(x, y) {
  if (typeof floorMesh !== 'undefined' && floorMesh && floorMesh.caveStone &&
      isFinite(x) && isFinite(y)) {
    var gx = Math.floor(x / floorMesh.gridSize), gy = Math.floor(y / floorMesh.gridSize);
    if (gx >= 0 && gy >= 0 && gx < floorMesh.w && gy < floorMesh.h) {
      return floorMesh.caveStone[gy * floorMesh.w + gx];
    }
  }
  // Legacy level meshes have no authored cave field. Keep a stable neutral
  // fallback rather than silently reintroducing a view-dependent palette.
  return GAME_MATERIALS.caveStone.packed.base;
}

function getCaveRenderLightAt(x, y, covered) {
  if (!covered) return renderSurfaceAmbient;
  var blend = getPortalRenderBlendAt(x, y, true);
  return renderSurfaceAmbient + (renderCaveAmbient - renderSurfaceAmbient) * blend;
}

// A point on the opposite side of the roof plane is visible only when its ray
// crosses a real portal aperture. This works in both directions: looking in
// from the approach and looking back out from the chamber.
function isPointVisibleThroughCavePortal(wx, wy) {
  if (!deepCaveEntrances || !deepCaveEntrances.length) return false;
  for (var i = 0; i < deepCaveEntrances.length; i++) {
    var e = deepCaveEntrances[i];
    var co = e.cosA, si = e.sinA;
    if (co === undefined || si === undefined) {
      co = Math.cos(e.angle || 0); si = Math.sin(e.angle || 0);
    }
    // e.angle points inward; sampleCavePortal's public along axis points out.
    var cdx = cam.x - e.x, cdy = cam.y - e.y;
    var tdx = wx - e.x, tdy = wy - e.y;
    var ca = -(cdx * co + cdy * si);
    var ta = -(tdx * co + tdy * si);
    if (!((ca <= 0 && ta >= 0) || (ca >= 0 && ta <= 0)) || ta === ca) continue;
    var cc = -cdx * si + cdy * co;
    var tc = -tdx * si + tdy * co;
    var u = -ca / (ta - ca);
    if (u < 0 || u > 1) continue;
    var crossAtMouth = cc + (tc - cc) * u;
    if (Math.abs(crossAtMouth) <= (e.halfWidth || 54) + 6) return true;
  }
  return false;
}

function isExteriorVisibleThroughCavePortal(wx, wy) {
  return isPointVisibleThroughCavePortal(wx, wy);
}

function updateDayNight(dt) {
  if (!settings.dayNight) {
    // Cycle disabled — fixed noon
    ambientLight = 0.9; sunIntensity = 0.6; fogFloor = 0.3;
    sunDirX = 0; sunDirZ = 1;
  } else {
    dayTime += dt * daySpeed;
    if (dayTime >= 1) dayTime -= 1;

    // Sun orbit — sunrise at 0.25, overhead at 0.5, sunset at 0.75.
    // This phase is shared by the visible sky so the disk and world lighting
    // no longer disagree about where noon is.
    var sunAngle = (dayTime - 0.25) * Math.PI * 2;
    sunDirX = Math.cos(sunAngle);
    sunDirZ = Math.sin(sunAngle);  // positive = above horizon

    // Ambient + sun intensity from time of day
    // Night: 0.0-0.15 and 0.85-1.0, Dawn: 0.15-0.30, Day: 0.30-0.70, Dusk: 0.70-0.85
    var t = dayTime;
    if (t < 0.15 || t > 0.85) {
      // Night
      ambientLight = 0.32;
      sunIntensity = 0.0;
      fogFloor = 0.2;
    } else if (t < 0.30) {
      // Dawn — lerp from night to day
      var p = (t - 0.15) / 0.15;
      ambientLight = 0.32 + p * 0.58;
      sunIntensity = p * 0.6;
      fogFloor = 0.2 + p * 0.1;
    } else if (t < 0.70) {
      // Day
      ambientLight = 0.9;
      sunIntensity = 0.6;
      fogFloor = 0.3;
    } else {
      // Dusk — lerp from day to night
      var p = (t - 0.70) / 0.15;
      ambientLight = 0.9 - p * 0.58;
      sunIntensity = 0.6 - p * 0.6;
      fogFloor = 0.3 - p * 0.1;
    }
  }

  // Save the exterior solution before applying camera exposure.  Even with
  // day/night disabled this section runs, so underground topology never goes
  // stale.  The cave floor is a true readability floor, not merely an upper
  // cap on an already-dark night value.
  renderSurfaceAmbient = ambientLight;
  renderSurfaceSunIntensity = sunIntensity;
  renderSurfaceFogFloor = fogFloor;
  renderCaveAmbient = Math.max(0.40, Math.min(0.45, renderSurfaceAmbient));
  renderCaveFogFloor = 0.15;

  if (typeof getCaveSpaceAt === 'function') {
    playerUnderground = !!getCaveSpaceAt(pos.x, pos.y, getPlayerFloorH()).underground;
  } else {
    playerUnderground = false;
  }
  renderCameraCaveBlend = getPortalRenderBlendAt(pos.x, pos.y, playerUnderground);
  ambientLight = renderSurfaceAmbient +
    (renderCaveAmbient - renderSurfaceAmbient) * renderCameraCaveBlend;
  sunIntensity = renderSurfaceSunIntensity * (1 - renderCameraCaveBlend);
  fogFloor = renderSurfaceFogFloor +
    (renderCaveFogFloor - renderSurfaceFogFloor) * renderCameraCaveBlend;

  // Pre-compute shade per wall normal direction (4 values, reused for all faces)
  // Normals: N=(0,-1), S=(0,1), E=(1,0), W=(-1,0)
  // sunDirY component represents N/S illumination — sun orbits in XZ so we use sunDirX for E/W
  var dotN = Math.max(0, -sunDirZ * 0.3);  // north faces get glancing light
  var dotS = Math.max(0, sunDirZ * 0.3);   // south faces get direct southern sun
  var dotE = Math.max(0, sunDirX);
  var dotW = Math.max(0, -sunDirX);
  _wallShadeN = ambientLight + sunIntensity * dotN;
  _wallShadeS = ambientLight + sunIntensity * dotS;
  _wallShadeE = ambientLight + sunIntensity * dotE;
  _wallShadeW = ambientLight + sunIntensity * dotW;
  // Top faces — lit by sun elevation (sunDirZ = how high sun is)
  _topShade = ambientLight + sunIntensity * Math.max(0, sunDirZ) * 0.8;

  // Exterior shade remains stable while the player looks out from a cave.
  renderSurfaceWallShadeN = renderSurfaceAmbient + renderSurfaceSunIntensity * dotN;
  renderSurfaceWallShadeS = renderSurfaceAmbient + renderSurfaceSunIntensity * dotS;
  renderSurfaceWallShadeE = renderSurfaceAmbient + renderSurfaceSunIntensity * dotE;
  renderSurfaceWallShadeW = renderSurfaceAmbient + renderSurfaceSunIntensity * dotW;
  renderSurfaceTopShade = renderSurfaceAmbient +
    renderSurfaceSunIntensity * Math.max(0, sunDirZ) * 0.8;
}

function gameUpdate(dt) {
  if (overviewActive) return;  // pause game while viewing overview map
  updateDayNight(dt);
  updateViewCamera();          // camera follow (was incorrectly in draw2D)
  updateEnemies(dt);   // movement + per-frame attack state (pre-throttle)
  separateEnemies();   // pairwise push — must run after positions are updated
  updateProjectiles(dt);
  updateGuardTower();
  updateGroundEffects(dt);
  tickEffects(dt);     // impacts + cones + bone physics + soul orb collection
  step(dt);            // player movement → repelFromWalls → repelFromEnemies (push only)
  updateTreasureChests();
  updateEnemySpawners();
  updateArenaChallenge();
  updateArcaneTomes();
  updateStatPickups();
  updateCompanions(dt);
  updateFortressAllies(dt);
  updateMapRevealAnim();
  updateAmbientParticles(dt);
  updateExplored();
}

// Check if player is close enough to collect a treasure chest.
function updateTreasureChests() {
  if (!treasureChests || !treasureChests.length) return;
  nearestOpenChest = null;
  var nearestDist = Infinity;
  for (var i = 0; i < treasureChests.length; i++) {
    var ch = treasureChests[i];
    if (ch.collected) continue;
    var dist = Math.hypot(pos.x - ch.x, pos.y - ch.y);
    var inRange = dist < 90;
    // Set opened state based on proximity
    ch.opened = inRange;
    // Smooth lid animation via lerp
    var targetAngle = ch.opened ? 1.0 : 0.0;
    if (ch.lidAngle === undefined) ch.lidAngle = 0;
    ch.lidAngle += (targetAngle - ch.lidAngle) * 0.12;
    if (ch.lidAngle < 0.01) ch.lidAngle = 0;
    if (ch.lidAngle > 0.99) ch.lidAngle = 1;
    // Track nearest open chest for E-key targeting
    if (inRange && dist < nearestDist) {
      nearestDist = dist;
      nearestOpenChest = ch;
    }
  }
}

function activateShrine(shr) {
  if (!shr || shr.used) return;
  shr.used = true;
  var now = Date.now();
  var duration = 120000; // 2 minutes
  var buffName = '';
  if (shr.buffType === 'damage') { dmgBoostUntil = now + duration; buffName = 'Power'; }
  else if (shr.buffType === 'speed') { speedBoostUntil = now + duration; buffName = 'Swiftness'; }
  else if (shr.buffType === 'regen') { regenBoostUntil = now + duration; lastRegenTick = now; buffName = 'Regeneration'; }
  else if (shr.buffType === 'armor') { armorBoostUntil = now + duration; buffName = 'Protection'; }
  // Visual feedback — spawn particles at shrine location
  for (var pi = 0; pi < 12; pi++) {
    var ang = (pi / 12) * Math.PI * 2;
    impacts.push({
      x: shr.x, y: shr.y, z: 10,
      vx: Math.cos(ang) * 40, vy: Math.sin(ang) * 40, vz: 30 + Math.random() * 20,
      life: 0.8, maxLife: 0.8,
      color: shr.buffType === 'damage' ? '#ff4444' : shr.buffType === 'speed' ? '#44ffff' : shr.buffType === 'regen' ? '#44ff44' : '#ffaa44',
      size: 4
    });
  }
  pushToast('Shrine of ' + buffName + ' — 2 min buff', '#ffcc66', 3000);
  console.log('[SHRINE] Activated ' + buffName + ' shrine for ' + (duration / 1000) + 's');
}
