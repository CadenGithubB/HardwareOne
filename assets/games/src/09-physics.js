// =============================================
// SECTION 8: PHYSICS & COLLISION
// =============================================

// Returns wall-top Z in player-Z units if (cx,cy) is inside a grid wall cell, or 0 if not.
// Wall top = groundFloorZ + 25 * wallHeight multiplier.
function getWallTopZ(cx, cy) {
  if (!grid) return 0;
  var gx = Math.floor(cx / cell), gy = Math.floor(cy / cell);
  if (gx < 0 || gy < 0 || gx >= gridW || gy >= gridH) return 0;
  if (!grid[gy * gridW + gx]) return 0;
  var wh = wallHeights ? wallHeights[gy * gridW + gx] : 1.0;
  var fh = floorMesh ? getFloorHeightAt(cx, cy) : 0;
  var groundZ = fh * 40 + 60;
  return groundZ + 25 * wh;
}

// Returns true if any corner of the player circle (radius rad) sits inside a grid wall cell.
// Optional playerZ: if provided, skip walls whose top is below the player's Z height.
// This prevents underground cave walls from blocking surface movement.
function isInGridWall(cx, cy, rad, playerZ) {
  if (!grid) return false;
  var pts = [
    cx - rad, cy - rad,
    cx + rad, cy - rad,
    cx - rad, cy + rad,
    cx + rad, cy + rad,
    cx,       cy        // centre point too
  ];
  for (var i = 0; i < pts.length; i += 2) {
    var gx = Math.floor(pts[i]     / cell);
    var gy = Math.floor(pts[i + 1] / cell);
    if (gx >= 0 && gx < gridW && gy >= 0 && gy < gridH && grid[gy * gridW + gx]) {
      // Z-height check: skip walls the player is above
      if (playerZ !== undefined) {
        var wallTopZ = getWallTopZ(pts[i], pts[i + 1]);
        if (playerZ > wallTopZ + 5) continue; // 5 units grace margin
      }
      // Cave wall check: surface players walk over cave walls (pre-computed flag)
      if (playerZ !== undefined && !playerUnderground && gridCave && gridCave[gy * gridW + gx]) {
        continue;
      }
      return true;
    }
  }
  return false;
}

function repelFromWalls(dt) {
  var cx = pos.x, cy = pos.y;
  var rad = 10, range = 18, k = 60;
  for (var i = 0; i < walls.length; i++) {
    var w = walls[i];
    // Surface players skip cave wall repulsion (pre-computed flag)
    if (!playerUnderground && w.cave) continue;
    var nx = Math.max(w.x, Math.min(cx, w.x + w.w));
    var ny = Math.max(w.y, Math.min(cy, w.y + w.h));
    var dx = cx - nx, dy = cy - ny;
    var dist = Math.hypot(dx, dy);
    var thresh = rad + range;
    if (dist < thresh && dist > 0.001) {
      var s = (thresh - dist) / thresh;
      var inv = 1.0 / dist;
      vel.x += (dx * inv) * k * s * dt;
      vel.y += (dy * inv) * k * s * dt;
      if (DEBUG_CAVE && dist < rad + 2 && Date.now() - (_lastRepelDbg || 0) > 1000) {
        _lastRepelDbg = Date.now();
        console.log('[REPEL-WALL] walls[' + i + '] rect=(' + w.x.toFixed(0) + ',' + w.y.toFixed(0) + ' ' + w.w.toFixed(0) + 'x' + w.h.toFixed(0) + ') dist=' + dist.toFixed(1) + ' player=(' + cx.toFixed(0) + ',' + cy.toFixed(0) + ')');
      }
    }
  }
}

// Physical push only — attack state is handled in updateEnemies (per-frame, pre-throttle)
function repelFromEnemies(dt) {
  if (!enemies || !enemies.length) return;
  var cx = pos.x, cy = pos.y, k = 150;
  for (var i = 0; i < enemies.length; i++) {
    var e = enemies[i];
    var enemyRad = 8 * (e.enemyType.size || 1);
    var dx = cx - e.x, dy = cy - e.y;
    var dist = Math.hypot(dx, dy);
    var thresh = 12 + enemyRad; // larger radius: repel earlier so close-range never looks broken
    if (dist < thresh && dist > 0.001) {
      var overlap = thresh - dist, inv = 1.0 / dist;
      var newX = pos.x + (dx * inv) * overlap * 0.5;
      var newY = pos.y + (dy * inv) * overlap * 0.5;
      // Use the same full-radius wall check as player movement so enemies
      // can never pin the player into wall geometry or out-of-bounds
      var blocked = (newX < 6 || newX > worldW - 6 || newY < 6 || newY > worldH - 6)
                    || isInGridWall(newX, newY, 6, meshHeightToPlayerZ(getPlayerFloorH()))
                    || isTerrainBlockedAt(newX, newY, getPlayerFloorH(), jumpAirborne);
      if (!blocked) { pos.x = newX; pos.y = newY; vel.x += (dx * inv) * k * overlap * dt; vel.y += (dy * inv) * k * overlap * dt; }
      else { vel.x *= 0.5; vel.y *= 0.5; }
    }
  }
}

function repelFromStalls() {
  if (!marketStalls || !marketStalls.length) return;
  var pr = 6; // player radius
  for (var i = 0; i < marketStalls.length; i++) {
    var st = marketStalls[i];
    var cf = Math.cos(st.facing), sf = Math.sin(st.facing);
    var hw = 17, hd = 9; // half-width and half-depth of stall
    // Transform player position into stall-local space
    var lx = (pos.x - st.x) * cf + (pos.y - st.y) * sf;
    var ly = -(pos.x - st.x) * sf + (pos.y - st.y) * cf;
    // Check overlap with padded AABB
    var ox = hw + pr - Math.abs(lx);
    var oy = hd + pr - Math.abs(ly);
    if (ox > 0 && oy > 0) {
      // Push out along the axis with smallest overlap
      if (ox < oy) {
        lx += (lx > 0 ? ox : -ox);
      } else {
        ly += (ly > 0 ? oy : -oy);
      }
      // Transform back to world space
      var stallPushX = st.x + lx * cf - ly * sf;
      var stallPushY = st.y + lx * sf + ly * cf;
      if (!isTerrainBlockedAt(stallPushX, stallPushY, getPlayerFloorH(), jumpAirborne)) {
        pos.x = stallPushX;
        pos.y = stallPushY;
      }
      vel.x *= 0.3; vel.y *= 0.3;
    }
  }
}

function tracksMeshFloor() {
  return !!floorMesh && (ENDLESS_MODE || terrain === 'plains' || terrain === 'cave' || terrain === 'expanse');
}

// Terrain is collision geometry too: a tall rise or low ceiling cannot be
// entered merely because the coarse wall grid happens to be empty. Sample
// the actor footprint, so the body cannot clip through the side of a passage.
function isTerrainBlockedAt(x, y, feetH, airborne) {
  if (!tracksMeshFloor()) return false;
  var offsets = [0, 0, -6, 0, 6, 0, 0, -6, 0, 6];
  for (var ti = 0; ti < offsets.length; ti += 2) {
    var support = getWalkableLayerTopAt(x + offsets[ti], y + offsets[ti + 1], feetH,
      airborne ? {stepUp: 0} : null);
    if (support.action === 'blocked' || support.action === 'missing') return true;
  }
  return false;
}

function updatePlayerCaveSpace() {
  playerUnderground = getCaveSpaceAt(pos.x, pos.y, getPlayerFloorH()).underground;
}

function step(dt) {
  updatePlayerCaveSpace();
  // Mana and health regen (with hat bonuses)
  var manaRegenBonus = (equipment.hat && equipment.hat.manaRegen) ? equipment.hat.manaRegen : 0;
  var hpRegenBonus = (equipment.hat && equipment.hat.hpRegen) ? equipment.hat.hpRegen : 0;
  mana = Math.min(MANA_MAX, mana + (MANA_REGEN_PER_S + manaRegenBonus) * dt);
  health = Math.min(HEALTH_MAX, health + (HEALTH_REGEN_PER_S + hpRegenBonus) * dt);

  // ── Dash mechanic (requires boots with canDash) ──
  var now = Date.now();
  var boots = equipment.boots;
  if (dashPressed && boots && boots.canDash && now >= dashCooldownUntil) {
    // Dash in movement direction (WASD), or facing direction if no keys held
    var dashAng = cam.ang;
    var _dkx = 0, _dky = 0;
    if (kbState.up)    { _dkx += Math.cos(cam.ang);  _dky += Math.sin(cam.ang); }
    if (kbState.down)  { _dkx -= Math.cos(cam.ang);  _dky -= Math.sin(cam.ang); }
    if (kbState.left)  { _dkx += Math.cos(cam.ang - Math.PI/2); _dky += Math.sin(cam.ang - Math.PI/2); }
    if (kbState.right) { _dkx += Math.cos(cam.ang + Math.PI/2); _dky += Math.sin(cam.ang + Math.PI/2); }
    if (_dkx !== 0 || _dky !== 0) dashAng = Math.atan2(_dky, _dkx);
    vel.x += Math.cos(dashAng) * GAME_CONFIG.player.dashSpeed;
    vel.y += Math.sin(dashAng) * GAME_CONFIG.player.dashSpeed;
    dashCooldownUntil = now + GAME_CONFIG.player.dashCooldownMs;
    dashUntil = now + GAME_CONFIG.player.dashDurationMs;
    dashFovPunch = 1.0;              // start FOV punch
    dashPressed = false;
  }

  // ── Jump mechanic (requires boots with canJump) ──
  if (jumpPressed && boots && boots.canJump && !jumpAirborne) {
    jumpVelZ = GAME_CONFIG.player.jumpImpulse;
    jumpAirborne = true;
    jumpPressed = false;
  }
  // Track whether player is standing on a wall top
  var onWallTop = false;
  var wallTopZ = getWallTopZ(pos.x, pos.y);
  // Wall-top clamping only applies on the surface. Inside a cave the player
  // walks on the cave floor; cave-boundary "wall" cells (which also carry a
  // wallTopZ) would otherwise trap the player at the wall's top height and
  // prevent descending to the cave floor.
  if (playerUnderground) wallTopZ = 0;
  // Surface players walk OVER cave walls (collision already skips them in
  // isInGridWall). But getWallTopZ reads grid[] directly and would otherwise
  // snap the player's Z to the cave wall's top as they descend the ramp,
  // yanking them to random heights. Match the collision behavior here.
  if (!playerUnderground && gridCave) {
    var _wtGx = Math.floor(pos.x / cell), _wtGy = Math.floor(pos.y / cell);
    if (_wtGx >= 0 && _wtGx < gridW && _wtGy >= 0 && _wtGy < gridH &&
        gridCave[_wtGy * gridW + _wtGx]) {
      wallTopZ = 0;
    }
  }
  if (jumpAirborne) {
    var previousH = getPlayerFloorH();
    jumpVelZ -= GAME_CONFIG.player.gravity * dt;
    pos.floorZ = meshHeightToPlayerZ(previousH) + jumpVelZ * dt;
    var groundZ = tracksMeshFloor() ? -Infinity : 60;
    if (tracksMeshFloor()) {
      // Sweep from the previous feet height so a fast fall cannot skip a
      // floor. Only floors reachable without crossing rock may catch us.
      var walk_j = getWalkableLayerTopAt(pos.x, pos.y, previousH, {stepUp: 0});
      if (walk_j.action === 'walk' || walk_j.action === 'fall') groundZ = meshHeightToPlayerZ(walk_j.topH);
      var ceiling_j = getCaveSpaceAt(pos.x, pos.y, previousH).ceilingH;
      if (jumpVelZ > 0 && getPlayerFloorH() + PLAYER_BODY_H > ceiling_j) {
        pos.floorZ = meshHeightToPlayerZ(Math.max(previousH, ceiling_j - PLAYER_BODY_H));
        jumpVelZ = 0;
      }
    }
    // Can land on wall top if falling down onto it
    if (wallTopZ > 0 && jumpVelZ <= 0 && pos.floorZ <= wallTopZ) {
      pos.floorZ = wallTopZ;
      jumpVelZ = 0;
      jumpAirborne = false;
      onWallTop = true;
    } else if (jumpVelZ <= 0 && pos.floorZ <= groundZ) {
      pos.floorZ = groundZ;
      jumpVelZ = 0;
      jumpAirborne = false;
    }
  } else if (wallTopZ > 0 && pos.floorZ >= wallTopZ - 2) {
    // Standing on wall top — stay on it
    onWallTop = true;
    pos.floorZ = wallTopZ;
  } else if (wallTopZ > 0 && pos.floorZ < wallTopZ - 2) {
    // Inside wall but below top — shouldn't happen, push to safety
    var safe = findSafeSpawn(pos.x, pos.y);
    pos.x = safe.x; pos.y = safe.y;
    vel.x = 0; vel.y = 0;
  }
  // Walking off a wall edge — start falling
  if (!jumpAirborne && !onWallTop && wallTopZ === 0) {
    var fallingOffEdge = pos.floorZ > 63;
    if (tracksMeshFloor()) {
      var walk_e = getWalkableLayerTopAt(pos.x, pos.y, getPlayerFloorH());
      fallingOffEdge = walk_e.action === 'fall';
    }
    if (fallingOffEdge) {
      // Walked off edge — start falling
      jumpAirborne = true;
      jumpVelZ = 0;
    }
  }

  // ── FOV punch decay ──
  if (dashFovPunch > 0) {
    dashFovPunch = Math.max(0, dashFovPunch - dt * 5);  // snap back over ~200ms
    cam.fov = GAME_CONFIG.player.baseFov + dashFovPunch * GAME_CONFIG.player.dashFovPunch;
  } else {
    cam.fov = GAME_CONFIG.player.baseFov;
  }

  // Apply damping — frame-rate independent (reference: 60fps)
  var damp = Math.pow(damping, dt * 60);
  vel.x *= damp;
  vel.y *= damp;

  // Move per-axis so the player slides along grid walls instead of stopping dead.
  // Each axis is tried independently: if moving X would enter a wall cell, revert
  // only X (and bounce it) so Y motion still continues — the player glides along
  // the wall face rather than getting stuck in a corner.
  var oldX = pos.x;
  pos.x += vel.x * dt;
  // Skip wall collision when: airborne above walls, or standing on a wall top
  var aboveWalls = onWallTop;
  var _collideZ = meshHeightToPlayerZ(getPlayerFloorH()); // legitimate zero is not the default floor
  var _hitWallX = false, _hitWallY = false;
  if (!NOCLIP && ((!aboveWalls && isInGridWall(pos.x, pos.y, 6, _collideZ)) ||
      isTerrainBlockedAt(pos.x, pos.y, getPlayerFloorH(), jumpAirborne))) {
    pos.x = oldX; vel.x *= -bounce; _hitWallX = true;
  }

  var oldY = pos.y;
  pos.y += vel.y * dt;
  if (!NOCLIP && ((!aboveWalls && isInGridWall(pos.x, pos.y, 6, _collideZ)) ||
      isTerrainBlockedAt(pos.x, pos.y, getPlayerFloorH(), jumpAirborne))) {
    pos.y = oldY; vel.y *= -bounce; _hitWallY = true;
  }

  // Active unstuck: if player is still inside a wall after axis collision
  // (common when cave gen leaves them stranded, or when they overlap a wall
  // they didn't come from), push outward toward the nearest open cell.
  // Spirals outward ring by ring so the push is minimal and deterministic.
  if (!NOCLIP && !aboveWalls && isInGridWall(pos.x, pos.y, 6, _collideZ)) {
    var _usGx = Math.floor(pos.x / cell), _usGy = Math.floor(pos.y / cell);
    var _usBestDX = 0, _usBestDY = 0, _usBestDistSq = 1e9;
    for (var _usR = 1; _usR <= 6; _usR++) {
      for (var _usDy = -_usR; _usDy <= _usR; _usDy++) {
        for (var _usDx = -_usR; _usDx <= _usR; _usDx++) {
          if (Math.max(Math.abs(_usDx), Math.abs(_usDy)) !== _usR) continue;
          var _usNx = _usGx + _usDx, _usNy = _usGy + _usDy;
          if (_usNx < 0 || _usNx >= gridW || _usNy < 0 || _usNy >= gridH) continue;
          var _usCx = (_usNx + 0.5) * cell, _usCy = (_usNy + 0.5) * cell;
          if (isInGridWall(_usCx, _usCy, 6, _collideZ)) continue;
          if (isTerrainBlockedAt(_usCx, _usCy, getPlayerFloorH(), jumpAirborne)) continue;
          var _usDistSq = _usDx * _usDx + _usDy * _usDy;
          if (_usDistSq < _usBestDistSq) { _usBestDistSq = _usDistSq; _usBestDX = _usDx; _usBestDY = _usDy; }
        }
      }
      if (_usBestDistSq < 1e9) break; // found a ring with open cells — stop
    }
    if (_usBestDistSq < 1e9) {
      pos.x = (_usGx + _usBestDX + 0.5) * cell;
      pos.y = (_usGy + _usBestDY + 0.5) * cell;
      vel.x = 0; vel.y = 0;
    }
  }

  // DEBUG: log wall collision details periodically
  if (DEBUG_CAVE && (_hitWallX || _hitWallY) && Date.now() - (_lastWallDbg || 0) > 1000) {
    _lastWallDbg = Date.now();
    var _wgx = Math.floor(pos.x / cell), _wgy = Math.floor(pos.y / cell);
    var _wallTopAtPlayer = getWallTopZ(pos.x, pos.y);
    var _nearbyWalls = [];
    for (var _wdy = -1; _wdy <= 1; _wdy++) {
      for (var _wdx = -1; _wdx <= 1; _wdx++) {
        var _wnx = _wgx + _wdx, _wny = _wgy + _wdy;
        if (_wnx >= 0 && _wnx < gridW && _wny >= 0 && _wny < gridH && grid[_wny * gridW + _wnx]) {
          var _wtz = getWallTopZ(_wnx * cell + cell * 0.5, _wny * cell + cell * 0.5);
          var _wfh = floorMesh ? getFloorHeightAt(_wnx * cell + cell * 0.5, _wny * cell + cell * 0.5) : 0;
          _nearbyWalls.push('(' + _wnx + ',' + _wny + ' fh=' + _wfh.toFixed(1) + ' topZ=' + _wtz.toFixed(0) + ')');
        }
      }
    }
    console.log('[WALL-COLLIDE] hitX=' + _hitWallX + ' hitY=' + _hitWallY +
      ' playerZ=' + _collideZ.toFixed(0) + ' pos=(' + pos.x.toFixed(0) + ',' + pos.y.toFixed(0) + ')' +
      ' nearby=' + _nearbyWalls.join(' '));
  }

  // Floor height tracking for procedural-floor terrains (always active in endless mode)
  // Skip floor lerp when airborne — jump physics handles Z directly
  if (!jumpAirborne && !onWallTop && tracksMeshFloor()) {
    // Layer-aware floor pick: given the player's current Z (converted to
    // height-units), find the walkable layer they should snap to. Auto step-up
    // handles walking onto a hill cap from normal surface without falling
    // through. Replaces the old clamp+surfaceH logic.
    var playerH_t = getPlayerFloorH();
    var walk_t = getWalkableLayerTopAt(pos.x, pos.y, playerH_t);
    var zDiff = 0;
    if (walk_t.action === 'walk') {
      var targetZ = meshHeightToPlayerZ(walk_t.topH);
      zDiff = targetZ - meshHeightToPlayerZ(playerH_t);
      pos.floorZ = meshHeightToPlayerZ(playerH_t) + zDiff * (1 - Math.pow(0.7, dt * 60));
    } else if (walk_t.action === 'fall') {
      jumpAirborne = true;
      jumpVelZ = 0;
    }

    // Auto-pitch: transient horizon nudge when traversing slopes.
    // Uses a SEPARATE offset that decays on its own — never touches pitchTarget
    // so manual aim ("hold angle" behaviour) is respected.
    if (MODE3D) {
      var autoPitch = zDiff * 0.003;
      autoPitch = Math.max(-0.1, Math.min(0.1, autoPitch));
      autoPitchOff = (autoPitchOff || 0) + autoPitch * 0.25;
      autoPitchOff *= 0.88;  // fast self-decay — only visible while actively on a slope
      autoPitchOff = Math.max(-0.12, Math.min(0.12, autoPitchOff));
    }
    //if (Math.random() < 0.01) { // TEMP DISABLED
    //  console.log('[FLOOR] Player at (' + pos.x.toFixed(1) + ',' + pos.y.toFixed(1) + ')' +
    //    ' floorHeight=' + floorHeight.toFixed(3) +
    //    ' targetZ=' + targetZ.toFixed(1) +
    //    ' currentZ=' + pos.floorZ.toFixed(1));
    //}
  }

  // World bounds (skip in endless mode — world is infinite)
  if (!ENDLESS_MODE) {
    if (pos.x < 6) { pos.x = 6; vel.x *= -bounce; }
    if (pos.x > worldW - 6) { pos.x = worldW - 6; vel.x *= -bounce; }
    if (pos.y < 6) { pos.y = 6; vel.y *= -bounce; }
    if (pos.y > worldH - 6) { pos.y = worldH - 6; vel.y *= -bounce; }
  }

  // Endless mode: check if we need to shift the chunk window
  if (ENDLESS_MODE) updateChunks();

  // Wall collision (rect-based — walls[] array)
  var player = {x:pos.x - 6, y:pos.y - 6, w:12, h:12};
  if (!NOCLIP) {
    for (var i = 0; i < walls.length; i++) {
      var w = walls[i];
      // Surface players skip cave wall rects
      if (!playerUnderground && w.cave) continue;
      if (rectsOverlap(player, w)) {
        collisions++;
        document.getElementById('hudCol').textContent = collisions;
        if (DEBUG_CAVE && Date.now() - (_lastRectColDbg || 0) > 1000) {
          _lastRectColDbg = Date.now();
          console.log('[RECT-COLLIDE] walls[' + i + '] rect=(' + w.x.toFixed(0) + ',' + w.y.toFixed(0) + ' ' + w.w.toFixed(0) + 'x' + w.h.toFixed(0) + ') player=(' + pos.x.toFixed(0) + ',' + pos.y.toFixed(0) + ')');
        }
        pos.x -= vel.x * dt * 2;
        pos.y -= vel.y * dt * 2;
        vel.x *= -0.5; vel.y *= -0.5;
        player = {x:pos.x - 6, y:pos.y - 6, w:12, h:12};
      }
    }
    repelFromWalls(dt);
    repelFromEnemies(dt);
    repelFromStalls();
  }
  updatePlayerCaveSpace();

  // Death check
  if (health <= 0 && running) {
    // Phoenix Feather: auto-revive once
    if (equipment.relic && equipment.relic.effect === 'phoenixRevive' && !phoenixFeatherUsed) {
      phoenixFeatherUsed = true;
      health = HEALTH_MAX;
      pushToast('Phoenix Feather revived you!', '#ff8844', 3000);
      // Burst of orange particles
      for (var phi = 0; phi < 16; phi++) {
        var pAng = (phi / 16) * Math.PI * 2;
        impacts.push({x: pos.x, y: pos.y, z: 15, life: 0.8, maxLife: 0.8,
          color: '#ff8020', size: 5, vx: Math.cos(pAng) * 60, vy: Math.sin(pAng) * 60, vz: 30 + Math.random() * 20});
      }
      console.log('[RELIC] Phoenix Feather triggered! Full HP restored.');
    } else {
      gameOverState = true;
      stopGame();
      draw();
      return;
    }
  }

  // Goal check (skip in endless mode)
  if (!ENDLESS_MODE && goal && goalSpawned && rectsOverlap(player, goal)) {
    if (level < levels.length) {
      if (terrain === 'ice') terrain = 'cave';
      else if (terrain === 'cave') terrain = 'plains';
      else if (terrain === 'plains') terrain = 'ice';
      else terrain = 'ice';
      var sel = document.getElementById('terrainSelect');
      if (sel) sel.value = terrain;
      resetLevel(level + 1);
    } else {
      stopGame();
      alert('You win!');
    }
  }

  // Timer and medal color
  timeSec = (Date.now() - startMs) / 1000;
  if (timeSec <= medalGold) timeColor = '#d4af37';
  else if (timeSec <= medalSilver) timeColor = '#c0c0c0';
  else if (timeSec <= medalBronze) timeColor = '#cd7f32';
  else timeColor = '#ffffff';
}
