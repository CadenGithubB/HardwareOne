// =============================================
// SECTION 10: ENEMY SYSTEM
// =============================================

// Build a short looping patrol route for one enemy.
// Every waypoint is validated with two checks at load time so enemies never
// walk into walls:
//   1. Grid check — the target cell must be open (not grid[gy*gridW+gx])
//   2. LOS check  — hasLineOfSight from the spawn point ensures the path is
//      clear; a second LOS check from the previous waypoint keeps the loop smooth.
// If a waypoint can't be placed in 25 attempts we just use fewer points —
// tight areas get a shorter patrol rather than a broken one.
function generatePatrolWaypoints(sx, sy, range) {
  var count = 2 + Math.floor(Math.random() * 3); // 2-4 waypoints
  var wps = [{x:sx, y:sy}];
  var prev = wps[0];
  var patrolRange = range * 0.45;
  for (var wi = 1; wi < count; wi++) {
    var placed = false;
    for (var att = 0; att < 25 && !placed; att++) {
      var ang = Math.random() * Math.PI * 2;
      var d = patrolRange * 0.4 + Math.random() * patrolRange * 0.6;
      var tx = sx + Math.cos(ang) * d;
      var ty = sy + Math.sin(ang) * d;
      // World bounds
      if (tx < cell * 2 || ty < cell * 2 || tx > worldW - cell * 2 || ty > worldH - cell * 2) continue;
      // Grid cell open?
      var gx = Math.floor(tx / cell), gy = Math.floor(ty / cell);
      if (gx < 0 || gy < 0 || gx >= gridW || gy >= gridH) continue;
      if (grid && grid[gy * gridW + gx]) continue;
      // LOS from spawn and from previous waypoint
      if (!hasLineOfSight(sx, sy, tx, ty)) continue;
      if (!hasLineOfSight(prev.x, prev.y, tx, ty)) continue;
      var wp = {x:tx, y:ty};
      wps.push(wp);
      prev = wp;
      placed = true;
    }
  }
  return wps;
}

function spawnEnemies(cfg) {
  enemies = [];
  var maxE = (cfg && cfg.maxEnemies) ? cfg.maxEnemies : 4;
  var spawns = (cfg && cfg.enemies) ? cfg.enemies : [];
  // For plains/cave the fixed spawn list still works — enemies start in open space
  // near borders and the level's start corner; if a position lands in a procedural
  // wall the separation physics will push them clear within the first few frames.
  // Cap to maxEnemies in case someone defines more entries than they want active.
  if (spawns.length > maxE) spawns = spawns.slice(0, maxE);
  // If fewer fixed spawns than maxEnemies (e.g. plains/cave with extra budget),
  // scatter random extras across the world, well away from the player start.
  var typeKeys = Object.keys(enemyTypes);
  while (spawns.length < maxE) {
    var rx = Math.floor(100 + Math.random() * (worldW - 200));
    var ry = Math.floor(100 + Math.random() * (worldH - 200));
    spawns.push({x:rx, y:ry, type:typeKeys[Math.floor(Math.random() * typeKeys.length)]});
  }
  for (var i = 0; i < spawns.length; i++) {
    var s = spawns[i];
    var eType = enemyTypes[s.type || 'normal'];
    var wps = generatePatrolWaypoints(s.x, s.y, eType.chaseRange);
    enemies.push({x:s.x, y:s.y, z:0, enemyType:eType, health:eType.health, maxHealth:eType.health, speed:eType.speed,
      chaseRange:eType.chaseRange, lastUpdate:0, damageFlash:0, damageFlashColor:'#ffffff',
      slowUntil:0, burnUntil:0, burnDmgLast:0, iceHits:0, fireHits:0, lightningHits:0,
      vx:0, vy:0, aggroAt:0, attackState:'idle', attackStateUntil:0,
      patrolWaypoints:wps, patrolIdx:0, facing:0});
  }
  console.log('[ENEMY] Spawned ' + enemies.length + ' / ' + maxE + ' enemies (world ' + worldW + 'x' + worldH + ')');
}

// Resolves an enemy attack landing — checks dash i-frames, ward, then applies damage
function _applyEnemyAttack(e, eType, now) {
  if (now < dashUntil) return; // dash i-frames
  if (wardActive) {
    wardActive = false;
    console.log('[WARD] Hit absorbed by Warding Stone!');
    return;
  }
  var rawDmg = eType.attackDamage;
  if (equipment.armor) rawDmg *= (1 - equipment.armor.damageReduction);
  if (Date.now() < armorBoostUntil) rawDmg *= 0.7;
  health = Math.max(0, health - rawDmg);
  stats.totalDamageTaken += rawDmg;
  // Frost Heart: slow the attacker
  if (equipment.relic && equipment.relic.effect === 'frostAura' && e) {
    e.slowUntil = Math.max(e.slowUntil || 0, Date.now() + equipment.relic.value);
  }
}

// Moves an enemy along its patrol waypoints. Returns 1 if moved, 0 if not.
function _patrolEnemy(e, eRad) {
  var wp = e.patrolWaypoints[e.patrolIdx || 0];
  var wpDx = wp.x - e.x, wpDy = wp.y - e.y;
  var wpDist = Math.hypot(wpDx, wpDy);
  if (wpDist < 14) {
    e.patrolIdx = ((e.patrolIdx || 0) + 1) % e.patrolWaypoints.length;
    return 0;
  }
  var pSpeed = e.speed * 0.38 * 0.08;
  e.facing = Math.atan2(wpDy, wpDx);
  var pnx = e.x + (wpDx / wpDist) * pSpeed;
  var pny = e.y + (wpDy / wpDist) * pSpeed;
  if (!isInGridWall(pnx, pny, eRad * 0.6)) {
    e.x = pnx; e.y = pny;
    return 1;
  }
  e.patrolIdx = ((e.patrolIdx || 0) + 1) % e.patrolWaypoints.length;
  return 0;
}

function updateEnemies(dt) {
  if (!enemies || !enemies.length) return;
  var now = Date.now();
  var alive = [], throttled = 0, moved = 0, burning = 0, slowed = 0;
  for (var i = 0; i < enemies.length; i++) {
    var e = enemies[i];
    if (e.health <= 0) {
      console.log('[ENEMY] Enemy ' + i + ' defeated!');
      // Bone scatter — 7 tumbling segments fly outward
      for (var b = 0; b < 7; b++) {
        var bang = Math.random() * Math.PI * 2;
        var bspd = 50 + Math.random() * 110;
        deathEffects.push({x:e.x, y:e.y, vx:Math.cos(bang)*bspd, vy:Math.sin(bang)*bspd,
          rot:Math.random()*Math.PI*2, rotVel:(Math.random()-0.5)*10,
          len:7+Math.random()*13, spawnMs:now, lifeMs:800, color:'#cfc8b8'});
      }
      // Soul orb — glowing mana pickup
      soulOrbs.push({x:e.x, y:e.y, renderFloorZ:getEntityRenderFloorZ(e), underground:!!e.underground,
        spawnMs:now, bob:Math.random()*Math.PI*2});
      // Coin drop — 3-5 scout, 4-6 wolf, 5-8 soldier, 8-12 brute
      var _eid = e.enemyType.id;
      var dropBase  = _eid === 'fast' ? 3 : _eid === 'wolf' ? 4 : _eid === 'normal' ? 5 : 8;
      var dropRange = _eid === 'fast' ? 3 : _eid === 'wolf' ? 3 : _eid === 'normal' ? 4 : 5;
      var dropCount = dropBase + Math.floor(Math.random() * dropRange);
      for (var ci = 0; ci < dropCount; ci++) {
        var cang = Math.random() * Math.PI * 2, cspd = 15 + Math.random() * 25;
        coinDrops.push({x:e.x + Math.cos(cang)*8, y:e.y + Math.sin(cang)*8,
          underground:!!(e.underground || e.caveSpawnId),
          bob:Math.random()*Math.PI*2, spawnMs:now});
      }
      continue;
    }
    // Interest management: enemies far from the player AND all fortress allies
    // skip every per-frame update for this frame. They "thaw" the moment
    // anyone comes within 1.5× viewDist. Burn/slow/stun timers effectively
    // pause, but those timers are measured in wall-clock time so the enemy
    // catches up correctly when it's next evaluated.
    var _imDx = pos.x - e.x, _imDy = pos.y - e.y;
    var _IM_R = viewDist * 1.5;
    var _IM_R_SQ = _IM_R * _IM_R;
    if (_imDx * _imDx + _imDy * _imDy > _IM_R_SQ) {
      var _imNear = false;
      for (var _imfi = 0; _imfi < fortressAllies.length; _imfi++) {
        var _imfa = fortressAllies[_imfi];
        if (_imfa.health <= 0) continue;
        var _imfdx = _imfa.x - e.x, _imfdy = _imfa.y - e.y;
        if (_imfdx * _imfdx + _imfdy * _imfdy < _IM_R_SQ) { _imNear = true; break; }
      }
      if (!_imNear) { alive.push(e); continue; }
    }
    // PER-FRAME: knockback velocity
    var eRad = 8 * (e.enemyType.size || 1);
    if (e.vx || e.vy) {
      var kbx = e.x + e.vx * dt, kby = e.y + e.vy * dt;
      if (!isInGridWall(kbx, kby, eRad * 0.6)) {
        e.x = kbx; e.y = kby;
      }
      var kbDecay = Math.pow(0.005, dt);
      e.vx *= kbDecay; e.vy *= kbDecay;
      if (Math.abs(e.vx) < 1 && Math.abs(e.vy) < 1) { e.vx = 0; e.vy = 0; }
    }
    // PER-FRAME: attack state machine — runs before throttle so response is immediate
    var eType = e.enemyType;
    var aThr = 6 + 8 * (eType.size || 1);
    var aPdx = pos.x - e.x, aPdy = pos.y - e.y;
    var aPdist = Math.hypot(aPdx, aPdy);
    // Check if enemy can attack player OR a fortress ally
    var _attackTarget = null; // null=player, ally ref=ally
    var _attackDist = aPdist;
    // Check fortress allies — enemy attacks whichever target is closer
    for (var _fai = 0; _fai < fortressAllies.length; _fai++) {
      var _fa = fortressAllies[_fai];
      if (_fa.health <= 0) continue;
      var _faDist = Math.hypot(_fa.x - e.x, _fa.y - e.y);
      if (_faDist < _attackDist) { _attackDist = _faDist; _attackTarget = _fa; }
    }
    if (!NOCLIP && _attackDist < aThr && _attackDist > 0.001) {
      if (!e.attackState || e.attackState === 'idle') {
        e.attackState = 'windup'; e.attackStateUntil = now + eType.attackWindup;
        e._attackTarget = _attackTarget; // remember target for when windup completes
      } else if (e.attackState === 'windup' && now >= e.attackStateUntil) {
        if (e._attackTarget && e._attackTarget.health > 0) {
          // Attack fortress ally
          e._attackTarget.health -= eType.attackDamage * 0.15; // scaled damage
          e._attackTarget.damageFlash = 1.0;
        } else {
          _applyEnemyAttack(e, eType, now);
        }
        e.attackState = 'cooldown'; e.attackStateUntil = now + eType.attackCooldown;
        e._attackTarget = null;
      }
    } else {
      if (e.attackState === 'windup') { e.attackState = 'idle'; e.attackStateUntil = 0; }
      else if (e.attackState === 'cooldown' && now >= e.attackStateUntil) { e.attackState = 'idle'; }
    }
    if (!e.lastUpdate) e.lastUpdate = now;
    if (now - e.lastUpdate < 80) { alive.push(e); throttled++; continue; }
    e.lastUpdate = now;
    // Stun effect — skip all actions while stunned
    if (e.stunUntil && now < e.stunUntil) {
      alive.push(e); continue;
    }
    if (e.burnUntil && now < e.burnUntil) {
      burning++;
      if (!e.burnDmgLast || now - e.burnDmgLast >= 500) {
        e.health -= 0.3; e.burnDmgLast = now;
      }
    }
    var dx = pos.x - e.x, dy = pos.y - e.y;
    var dist = Math.hypot(dx, dy);
    // Market safe zone — enemies won't chase if player is near market
    var inMarketSafe = shopMarker && Math.hypot(pos.x - shopMarker.x, pos.y - shopMarker.y) < 100;
    // Chase player or nearest fortress ally
    var chaseTargetX = pos.x, chaseTargetY = pos.y;
    var chasing = !inMarketSafe && dist < e.chaseRange && dist > 10 && hasLineOfSight(e.x, e.y, pos.x, pos.y);
    if (!chasing) {
      // Try chasing a fortress ally instead
      for (var _fci = 0; _fci < fortressAllies.length; _fci++) {
        var _fca = fortressAllies[_fci];
        if (_fca.health <= 0) continue;
        var _fcDist = Math.hypot(_fca.x - e.x, _fca.y - e.y);
        if (_fcDist < e.chaseRange * 0.6 && _fcDist > 10) {
          chasing = true; chaseTargetX = _fca.x; chaseTargetY = _fca.y;
          dx = chaseTargetX - e.x; dy = chaseTargetY - e.y; dist = _fcDist;
          break;
        }
      }
    }
    if (chasing) {
      if (!e.aggroAt) e.aggroAt = now;
      var dirX = dx / dist, dirY = dy / dist;
      e.facing = Math.atan2(dy, dx);
      var moveSpeed = e.speed * 0.08;
      if (e.slowUntil && now < e.slowUntil) { moveSpeed *= 0.4; slowed++; }
      var eWallRad = eRad * 0.6;
      var nx = e.x + dirX * moveSpeed, ny = e.y + dirY * moveSpeed;
      if (!isInGridWall(nx, ny, eWallRad)) {
        e.x = nx; e.y = ny; moved++;
      } else {
        if (!isInGridWall(e.x + dirX * moveSpeed, e.y, eWallRad)) { e.x += dirX * moveSpeed; moved++; }
        if (!isInGridWall(e.x, e.y + dirY * moveSpeed, eWallRad)) { e.y += dirY * moveSpeed; moved++; }
      }
    } else {
      e.aggroAt = 0;
      if (dist < 350 && e.patrolWaypoints && e.patrolWaypoints.length > 1) {
        moved += _patrolEnemy(e, eRad);
      }
    }
    // Underground enemies track floor height
    if (e.underground && floorMesh) {
      e.z = getFloorHeightAt(e.x, e.y) * 25;
    }
    alive.push(e);
  }
  if (false && DEBUG_EFFECTS && now - __effectsLastLog > __effectsLogInterval) { // TEMP DISABLED
    try {
      console.log('[EFFECTS]', 'enemies=' + alive.length, 'throttled=' + throttled, 'moved=' + moved,
        'burning=' + burning, 'slowed=' + slowed, 'deathFX=' + deathEffects.length,
        'proj=' + projectiles.length, 'cone=' + coneEffects.length, 'impacts=' + impacts.length);
      // Detailed enemy position snapshot (every 10s)
      if (!window._enemySnapLast || now - window._enemySnapLast > 10000) {
        window._enemySnapLast = now;
        var nearby = [];
        for (var _si = 0; _si < alive.length; _si++) {
          var _se = alive[_si];
          var _sd = Math.hypot(_se.x - pos.x, _se.y - pos.y);
          if (_sd < 400) {
            nearby.push({i: _si, x: Math.round(_se.x), y: Math.round(_se.y),
              hp: _se.health.toFixed(1) + '/' + _se.maxHealth,
              dist: Math.round(_sd), state: _se.attackState,
              type: (_se.enemyType && _se.enemyType.name) || '?'});
          }
        }
        if (nearby.length > 0) {
          console.log('[ENEMY-SNAP] ' + nearby.length + ' near player: ' +
            JSON.stringify(nearby.slice(0, 8)));
        }
      }
      __effectsLastLog = now;
    } catch (_) {}
  }
  enemies = alive;
  // Spawn goal when all enemies are defeated
  // Goal spawns when all enemies AND all spawners are destroyed
  var spawnersAlive = 0;
  if (enemySpawners) { for (var sai = 0; sai < enemySpawners.length; sai++) { if (enemySpawners[sai].active) spawnersAlive++; } }
  if (!goalSpawned && enemies.length === 0 && spawnersAlive === 0) {
    spawnGoal();
  }
}

