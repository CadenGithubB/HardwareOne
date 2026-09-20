// =============================================
// SECTION 9: COMBAT SYSTEM
// =============================================

function getUnlockedSpells() {
  var result = [];
  for (var i = 0; i < spellOrder.length; i++) {
    if (spells[spellOrder[i]].unlocked) result.push(spellOrder[i]);
  }
  return result;
}

function getCurrentSpell() {
  var unlocked = getUnlockedSpells();
  if (currentSpellIdx >= unlocked.length) currentSpellIdx = 0;
  return spells[unlocked[currentSpellIdx] || 'missile'];
}

function cycleSpell(dir) {
  var n = getUnlockedSpells().length;
  if (n < 2) return;
  currentSpellIdx = ((currentSpellIdx + (dir || 1)) % n + n) % n;
  lastSpellChangeMs = Date.now();
  console.log('[SPELL] Cycled to: ' + getCurrentSpell().name);
}

function castCurrentSpell() {
  if (!running || gameOverState || menuOpen) return;
  var now = Date.now();
  var spell = getCurrentSpell();
  if (DEBUG_COMBAT && now - __combatDbgLast > 200) {
    __combatDbgLast = now;
    var cd = now - lastShotMs;
    console.log('[COMBAT] mouseHeld=' + _mouseHeld + ' eHeld=' + _attackHeld + ' mana=' + mana.toFixed(1) + ' cd=' + cd + 'ms/' + SHOOT_COOLDOWN + 'ms spell=' + spell.id + ' lastShot=' + lastShotMs);
  }
  // Stream attacks handled by _tryCastSpell continuous loop
  if (spell.attackType === 'stream') {
    _tryCastSpell(now);
    return;
  }
  var effManaCost = spell.manaCost * ((equipment.robes && equipment.robes.manaCostReduction) ? (1 - equipment.robes.manaCostReduction) : 1);
  var _effCD = getEffectiveCooldown();
  if (mana >= effManaCost && (now - lastShotMs) >= _effCD) {
    if (spell.attackType === 'nova') castNovaAttack(spell);
    else if (spell.attackType === 'cone') castConeAttack(spell);
    else if (spell.attackType === 'lob') {
      if (spell.tier >= 3) {
        // Tier 3: 3 lobs in a spread
        spawnLobProjectile(spell, -0.18);
        spawnLobProjectile(spell, 0);
        spawnLobProjectile(spell,  0.18);
      } else if (spell.tier >= 2) {
        // Tier 2: 2 lobs
        spawnLobProjectile(spell, -0.12);
        spawnLobProjectile(spell,  0.12);
      } else {
        spawnLobProjectile(spell);
      }
    }
    else {
      if (spell.tier >= 2 && spell.id === 'missile') {
        // Split Shot: fire 3 missiles in a spread
        for (var si = -1; si <= 1; si++) {
          spawnProjectile(spell.speed, PROJ_RADIUS, spell, si * 0.2);
        }
      } else {
        spawnProjectile(spell.speed, PROJ_RADIUS);
      }
    }
    var manaCostMult = (equipment.robes && equipment.robes.manaCostReduction) ? (1 - equipment.robes.manaCostReduction) : 1;
    var finalManaCost = spell.manaCost * manaCostMult;
    mana -= finalManaCost; stats.totalManaConsumed += finalManaCost;
    // When holding fire, snap lastShotMs to the ideal cooldown boundary
    // instead of the current frame time. This prevents per-frame timing
    // drift that makes held fire slower than rapid tapping.
    // Clamp to (now - 1 frame) minimum to prevent double-shots after long pauses.
    if (_mouseHeld || _attackHeld) {
      lastShotMs = Math.max(lastShotMs + _effCD, now - 16);
    } else {
      lastShotMs = now;
    }
    castAnimUntil = now + 280;
  }
}

// Helper: compute aim angle from current input mode
function getAimAngle() {
  var ang = cam.ang || 0;
  if (!MODE3D && gpLast && gpLast.valid) {
    var jx = gpLast.x, jy = -gpLast.y;
    var jmag = Math.hypot(jx, jy);
    if (jmag > 0.2) ang = Math.atan2(jy, jx);
  } else if (MODE3D) {
    if (CONTROL_MODE === MODE_STICK_AIM && gpLast && gpLast.valid) {
      var jx2 = gpLast.x, jmag2 = Math.abs(jx2);
      if (jmag2 > 0.15) ang = cam.ang + jx2 * 0.3;
    } else if (CONTROL_MODE === MODE_GYRO_AIM && hasYaw) {
      ang = yawTarget + yawOffset;
    }
  }
  return ang;
}

// Stored player height is 60+40H; effects/projectiles use render-world 25H.
function getPlayerRenderFloorZ() {
  return Number.isFinite(pos.floorZ) ? (pos.floorZ - 60) * 0.625 : 0;
}

function spawnProjectile(speedOverride, radiusOverride, spellOverride, angOffset) {
  var spell = spellOverride || getCurrentSpell();
  var ang = getAimAngle() + (angOffset || 0);
  var usePitch = -(cam.pitch || 0);
  // Spawn from the casting hand (right arm orb position)
  // Small forward offset — too large and point-blank enemies can't be hit
  var handFwd = 3, handRight = 2;
  var rightAng = ang + Math.PI * 0.5;
  var sx = pos.x + Math.cos(ang) * handFwd + Math.cos(rightAng) * handRight;
  var sy = pos.y + Math.sin(ang) * handFwd + Math.sin(rightAng) * handRight;
  var sp = (speedOverride || spell.speed);
  if (equipment.relic && equipment.relic.effect === 'spellRange') sp *= (1 + equipment.relic.value);
  var rr = (radiusOverride || PROJ_RADIUS);
  var hz = sp * Math.cos(usePitch);
  var vz = sp * Math.sin(usePitch);
  var spawnZ = MODE3D ? getPlayerRenderFloorZ() + 55 : 0;
  console.log('[PROJ] pitch=' + (cam.pitch||0).toFixed(3) + ' usePitch=' + usePitch.toFixed(3) + ' hz=' + hz.toFixed(1) + ' vz=' + vz.toFixed(1) + ' spawnZ=' + spawnZ);
  var _pLife = PROJ_LIFE_MS;
  if (equipment.relic && equipment.relic.effect === 'spellRange') _pLife = Math.round(_pLife * (1 + equipment.relic.value));
  projectiles.push({x:sx, y:sy, z:spawnZ, ang:ang, speed:sp, hz:hz, vz:vz,
                    spawnMs:Date.now(), lifeMs:_pLife, r:rr, spell:spell,
                    renderFloorZ:getPlayerRenderFloorZ(), underground:!!playerUnderground});
}

// Lob projectile — arcing trajectory for Poison Cloud
// angOffset: optional horizontal angle offset in radians (for spread shots)
function spawnLobProjectile(spell, angOffset) {
  var ang = getAimAngle() + (angOffset || 0);
  var lobRange = spell.lobRange || 180;
  var handFwd = 3, handRight = 2;
  var rightAng = ang + Math.PI * 0.5;
  var sx = pos.x + Math.cos(ang) * handFwd + Math.cos(rightAng) * handRight;
  var sy = pos.y + Math.sin(ang) * handFwd + Math.sin(rightAng) * handRight;
  var sp = spell.speed || 200;
  var spawnZ = MODE3D ? getPlayerRenderFloorZ() + 55 : 0;
  var gravZ = -200;

  // Landing Z = floor height at target position (works at any elevation including caves)
  var targetDist = lobRange;
  if (MODE3D && (cam.pitch || 0) > 0.06) {
    var eyeZ = 25;  // eye height above hand (relative, not absolute)
    targetDist = Math.max(12, Math.min(lobRange, eyeZ / Math.tan(cam.pitch)));
  }

  // Compute landing floor height at target
  var tXpre = sx + Math.cos(ang) * targetDist;
  var tYpre = sy + Math.sin(ang) * targetDist;
  var landingZ = 0;
  if (MODE3D && floorMesh) {
    landingZ = sampleEntitySupportRenderZ(tXpre, tYpre, getPlayerRenderFloorZ(), playerUnderground);
    if (!Number.isFinite(landingZ)) landingZ = getPlayerRenderFloorZ();
  }

  var lobTime = targetDist / sp;
  // Correct arc: solve for initVZ such that z(lobTime) = landingZ
  // spawnZ + initVZ*lobTime + 0.5*gravZ*lobTime^2 = landingZ
  var initVZ = ((landingZ - spawnZ) / lobTime) - (0.5 * gravZ * lobTime);

  // Store expected landing position for the indicator
  var tX = sx + Math.cos(ang) * targetDist;
  var tY = sy + Math.sin(ang) * targetDist;

  projectiles.push({x:sx, y:sy, z:spawnZ, ang:ang, speed:sp, hz:sp, vz:initVZ,
                    spawnMs:Date.now(), lifeMs:3000, r:PROJ_RADIUS, spell:spell,
                    isLob:true, gravZ:gravZ, lobStartZ:spawnZ,
                    targetX:tX, targetY:tY, targetZ:landingZ,
                    renderFloorZ:getPlayerRenderFloorZ(), underground:!!playerUnderground});
}

// Nova attack — instant AoE damage around the player
function castNovaAttack(spell) {
  var now = Date.now();
  var radius = spell.novaRadius || 100;
  var hitCount = 0;
  novaEffects.push({x:pos.x, y:pos.y, renderFloorZ:getPlayerRenderFloorZ(), underground:!!playerUnderground,
    radius:radius, color:spell.color, spawnMs:now, lifeMs:400});
  if (enemies && enemies.length) {
    for (var ei = 0; ei < enemies.length; ei++) {
      var e = enemies[ei];
      var dx = e.x - pos.x, dy = e.y - pos.y;
      var dist = Math.hypot(dx, dy);
      if (dist > radius || dist < 1) continue;
      var dmg = (spell.damage || 1) * (now < dmgBoostUntil ? 1.30 : 1.0) * (1 + ((equipment.robes && equipment.robes.spellDmgBonus) || 0));
      // Damage falls off at edge of radius
      dmg *= (1.0 - dist / radius * 0.5);
      e.health -= dmg; stats.totalDamageDone += dmg;
      applyRelicOnHit(dmg, e, false);
      if (e.health <= 0) stats.totalEnemiesKilled++;
      e.damageFlash = now + 200; e.damageFlashColor = spell.color;
      // Knockback away from player
      var kb = Math.hypot(dx, dy) || 1;
      e.vx = (dx / kb) * 150; e.vy = (dy / kb) * 150;
      // Stun
      if (spell.stunDuration) e.stunUntil = now + spell.stunDuration;
      hitCount++;
    }
  }
  console.log('[NOVA] ' + spell.name + ' hit ' + hitCount + ' enemies');
}

function castConeAttack(spell) {
  var now = Date.now();
  var ang = getAimAngle();
  var coneAngle = spell.coneAngle || 0.25;
  // Tier 2 Inferno: wider cone, more damage
  if (spell.id === 'fire' && spell.tier >= 2) { coneAngle = 0.5; }
  var halfAngle = coneAngle / 2;
  var range = spell.coneRange || 100;
  var hitCount = 0;
  coneEffects.push({x:pos.x, y:pos.y, z:MODE3D ? getPlayerRenderFloorZ() + 35 : 0,
    renderFloorZ:getPlayerRenderFloorZ(), underground:!!playerUnderground,
    ang:ang, halfAngle:halfAngle, range:range, color:spell.color, spellId:spell.id, spawnMs:now, lifeMs:200});
  if (enemies && enemies.length) {
    for (var ei = 0; ei < enemies.length; ei++) {
      var e = enemies[ei];
      var dx = e.x - pos.x, dy = e.y - pos.y;
      var dist = Math.hypot(dx, dy);
      if (dist > range || dist < 1) continue;
      var angleToEnemy = Math.atan2(dy, dx);
      var angleDiff = angleToEnemy - ang;
      while (angleDiff > Math.PI) angleDiff -= Math.PI * 2;
      while (angleDiff < -Math.PI) angleDiff += Math.PI * 2;
      if (Math.abs(angleDiff) <= halfAngle) {
        var dmg = (spell.damage || 1) * (now < dmgBoostUntil ? 1.30 : 1.0) * (1 + ((equipment.robes && equipment.robes.spellDmgBonus) || 0));
        // Synergy: Steam Burst — fire on ice-slowed enemy
        if (spell.id === 'fire' && e.slowUntil && now < e.slowUntil) {
          applySteamBurst(e, now);
        }
        e.health -= dmg; stats.totalDamageDone += dmg;
        applyRelicOnHit(dmg, e, false);
        if (e.health <= 0) stats.totalEnemiesKilled++;
        e.damageFlash = now + 150; e.damageFlashColor = spell.color; hitCount++;
        if (spell.id === 'fire') {
          e.fireHits = (e.fireHits || 0) + 1;
          if (e.fireHits >= 3) { e.burnUntil = now + 4000; e.burnDmgLast = now; e.fireHits = 0; }
        }
      }
    }
  }
  // Ground fire patches — scatter 2-3 fire patches along the cone
  if (spell.groundFire) {
    var fireDur = (spell.tier >= 2) ? 5000 : 3000;
    var numPatches = 2 + Math.floor(Math.random() * 2);
    for (var fi = 0; fi < numPatches; fi++) {
      var fd = range * (0.3 + Math.random() * 0.6);
      var fa = ang + (Math.random() - 0.5) * coneAngle;
      var fx = pos.x + Math.cos(fa) * fd, fy = pos.y + Math.sin(fa) * fd;
      // Don't place in walls
      var fgx = Math.floor(fx / cell), fgy = Math.floor(fy / cell);
      if (fgx >= 0 && fgy >= 0 && fgx < gridW && fgy < gridH && !grid[fgy * gridW + fgx]) {
        groundEffects.push({x:fx, y:fy, radius:20, duration:fireDur,
                            renderFloorZ:sampleEntitySupportRenderZ(fx, fy, getPlayerRenderFloorZ(), playerUnderground), underground:!!playerUnderground,
                            damage:0.5, color:'#ff4400', spellId:'fire', spawnMs:now, tickMs:now});
      }
    }
  }
}

// Stream attack — continuous hitscan flamethrower
function castStreamAttack(spell) {
  var now = Date.now();
  var ang = getAimAngle();
  var range = spell.streamRange || 160;
  var halfWidth = spell.streamWidth || 0.20;
  if (spell.id === 'fire' && spell.tier >= 2) { range = 180; halfWidth = 0.55; }

  // Update persistent stream state (rendered continuously in draw functions)
  if (!flameStreamActive) flameStreamStartMs = now; // ramp-up start
  flameStreamAng = ang;

  // Hitscan: trace a line from player along aim, find wall hit
  var cosA = Math.cos(ang), sinA = Math.sin(ang);
  var hitRange = range;
  var stepSize = cell * 0.5;
  for (var sd = stepSize; sd < range; sd += stepSize) {
    var sx = pos.x + cosA * sd, sy = pos.y + sinA * sd;
    var sgx = Math.floor(sx / cell), sgy = Math.floor(sy / cell);
    if (sgx >= 0 && sgy >= 0 && sgx < gridW && sgy < gridH && grid[sgy * gridW + sgx]) {
      hitRange = sd;
      break;
    }
  }
  flameStreamRange = hitRange;

  // Damage enemies in the beam
  if (enemies && enemies.length) {
    var dmg = (spell.damage || 0.4) * (now < dmgBoostUntil ? 1.30 : 1.0) * (1 + ((equipment.robes && equipment.robes.spellDmgBonus) || 0));
    for (var ei = 0; ei < enemies.length; ei++) {
      var e = enemies[ei];
      var dx = e.x - pos.x, dy = e.y - pos.y;
      var along = dx * cosA + dy * sinA;
      if (along < 10 || along > hitRange) continue;
      var perp = Math.abs(-dx * sinA + dy * cosA);
      var widthAtDist = 8 + Math.tan(halfWidth) * along;
      if (perp > widthAtDist) continue;

      if (spell.id === 'fire' && e.slowUntil && now < e.slowUntil) {
        applySteamBurst(e, now);
      }
      e.health -= dmg; stats.totalDamageDone += dmg;
      applyRelicOnHit(dmg, e, false);
      if (e.health <= 0) stats.totalEnemiesKilled++;
      e.damageFlash = now + 80; e.damageFlashColor = spell.color;
      if (spell.id === 'fire') {
        e.fireHits = (e.fireHits || 0) + 1;
        if (e.fireHits >= 5) { e.burnUntil = now + 4000; e.burnDmgLast = now; e.fireHits = 0; }
      }
    }
  }

  // Occasional ground fire patches
  if (spell.groundFire && Math.random() < 0.12) {
    var fireDur = (spell.tier >= 2) ? 5000 : 3000;
    var fd = hitRange * (0.3 + Math.random() * 0.5);
    var fa = ang + (Math.random() - 0.5) * halfWidth * 2;
    var fx = pos.x + Math.cos(fa) * fd, fy = pos.y + Math.sin(fa) * fd;
    var fgx = Math.floor(fx / cell), fgy = Math.floor(fy / cell);
    if (fgx >= 0 && fgy >= 0 && fgx < gridW && fgy < gridH && !grid[fgy * gridW + fgx]) {
      groundEffects.push({x:fx, y:fy, radius:18, duration:fireDur,
                          renderFloorZ:sampleEntitySupportRenderZ(fx, fy, getPlayerRenderFloorZ(), playerUnderground), underground:!!playerUnderground,
                          damage:0.4, color:'#ff4400', spellId:'fire', spawnMs:now, tickMs:now});
    }
  }
}

// Synergy: Steam Burst — AoE damage around an ice-slowed enemy hit by fire
function applySteamBurst(target, now) {
  var burstRadius = 60;
  var burstDmg = 2.0;
  for (var si = 0; si < enemies.length; si++) {
    var se = enemies[si];
    if (Math.hypot(se.x - target.x, se.y - target.y) < burstRadius) {
      se.health -= burstDmg; stats.totalDamageDone += burstDmg;
      applyRelicOnHit(burstDmg, se, false);
      if (se.health <= 0) stats.totalEnemiesKilled++;
      se.damageFlash = now + 200; se.damageFlashColor = '#cccccc';
    }
  }
  target.slowUntil = 0; target.burnUntil = 0; // clear both statuses
  impacts.push({x:target.x, y:target.y, z:getEntityRenderFloorZ(target) + 10, spawnMs:now, lifeMs:500, type:'steam'});
  console.log('[SYNERGY] Steam Burst!');
}

// Applies damage, knockback, spell effects, and chain lightning when a projectile hits an enemy.
function _applyProjectileHit(spell, e, ei, nx, ny, nz, dx, dy, now) {
  var dmg = (spell.damage || 1);
  if (!spell.isTower) {
    dmg *= (now < dmgBoostUntil ? 1.30 : 1.0) * (1 + ((equipment.robes && equipment.robes.spellDmgBonus) || 0));
  }

  // Synergy: Shatter (lightning on ice-slowed)
  var chainRange = spell.chainRange || 80;
  if (!spell.isTower && spell.id === 'lightning' && e.slowUntil && now < e.slowUntil) {
    dmg *= 1.5; chainRange *= 2;
    impacts.push({x:nx, y:ny, z:nz, spawnMs:now, lifeMs:400, type:'shatter'});
    console.log('[SYNERGY] Shatter! 1.5x dmg, 2x chain range');
  }

  e.health -= dmg;
  if (!spell.isTower) { stats.totalDamageDone += dmg; applyRelicOnHit(dmg, e, false); }
  if (e.health <= 0) stats.totalEnemiesKilled++;
  e.damageFlash = now + 150; e.damageFlashColor = spell.color || '#ffffff';
  var kb2d = Math.hypot(dx, dy) || 1;
  e.vx = (-dx / kb2d) * 110; e.vy = (-dy / kb2d) * 110;

  // Spell-specific effects
  if (spell.isTower) {
    // Tower bolts: damage + knockback only
  } else if (spell.id === 'ice') {
    e.slowUntil = now + 2500;
    if (spell.groundPatch) {
      groundEffects.push({x:e.x, y:e.y, renderFloorZ:getEntityRenderFloorZ(e), underground:!!e.underground, radius:35, duration:3000,
        damage:0, color:'#00ffff', spellId:'ice', spawnMs:now, tickMs:now,
        slowFactor:0.4});
    }
    if (spell.tier >= 2) {
      var frostNovaR = 80;
      for (var fni = 0; fni < enemies.length; fni++) {
        var fne = enemies[fni];
        if (fne === e || Math.hypot(fne.x - e.x, fne.y - e.y) >= frostNovaR) continue;
        fne.slowUntil = now + 2000;
        fne.damageFlash = now + 100; fne.damageFlashColor = '#00ffff';
      }
      impacts.push({x:e.x, y:e.y, z:getEntityRenderFloorZ(e) + 10, spawnMs:now, lifeMs:350, type:'frostnova'});
    }
  } else if (spell.id === 'fire') {
    e.fireHits = (e.fireHits || 0) + 1;
    if (e.fireHits >= 3) { e.burnUntil = now + 4000; e.burnDmgLast = now; e.fireHits = 0; }
    if (e.slowUntil && now < e.slowUntil) { applySteamBurst(e, now); }
  }

  // Chain Lightning
  if (!spell.chainCount) return;
  var chainTargets = [];
  var chainN = (spell.tier >= 2) ? (spell.chainCount + 1) : spell.chainCount;
  var hitSet = {}; hitSet[ei] = true;

  // Check Conductive Cloud synergy: lightning on enemy in poison zone
  var inPoisonCloud = false;
  for (var gei = 0; gei < groundEffects.length; gei++) {
    var ge = groundEffects[gei];
    if (ge.spellId === 'poison' && Math.hypot(e.x - ge.x, e.y - ge.y) < ge.radius) {
      inPoisonCloud = true; break;
    }
  }
  if (inPoisonCloud) {
    console.log('[SYNERGY] Conductive Cloud! Chains to all in poison zone');
    for (var cci = 0; cci < enemies.length; cci++) {
      if (hitSet[cci]) continue;
      var cce = enemies[cci]; if (cce.health <= 0) continue;
      for (var gcj = 0; gcj < groundEffects.length; gcj++) {
        var gc2 = groundEffects[gcj];
        if (gc2.spellId === 'poison' && Math.hypot(cce.x - gc2.x, cce.y - gc2.y) < gc2.radius) {
          chainTargets.push(cci); hitSet[cci] = true; break;
        }
      }
    }
  } else {
    var chainCandidates = [];
    for (var ci = 0; ci < enemies.length; ci++) {
      if (hitSet[ci]) continue;
      var ce = enemies[ci]; if (ce.health <= 0) continue;
      var cdist = Math.hypot(ce.x - e.x, ce.y - e.y);
      if (cdist < chainRange) chainCandidates.push({idx:ci, dist:cdist});
    }
    chainCandidates.sort(function(a,b) { return a.dist - b.dist; });
    for (var cj = 0; cj < Math.min(chainN, chainCandidates.length); cj++) {
      chainTargets.push(chainCandidates[cj].idx);
    }
  }

  var chainDmg = dmg * (spell.chainDmgFalloff || 0.5);
  for (var ck = 0; ck < chainTargets.length; ck++) {
    var ce2 = enemies[chainTargets[ck]];
    ce2.health -= chainDmg; stats.totalDamageDone += chainDmg;
    applyRelicOnHit(chainDmg, ce2, false);
    if (ce2.health <= 0) stats.totalEnemiesKilled++;
    ce2.damageFlash = now + 150; ce2.damageFlashColor = '#ffff00';
    ce2.vx = (ce2.x - e.x) * 0.5; ce2.vy = (ce2.y - e.y) * 0.5;
  }
  if (chainTargets.length > 0) {
    chainEffects.push({fromX:e.x, fromY:e.y, fromZ:getEntityRenderFloorZ(e) + 30, targets:chainTargets.map(function(ci2) {
      return {x:enemies[ci2].x, y:enemies[ci2].y, z:getEntityRenderFloorZ(enemies[ci2]) + 30};
    }), spawnMs:now, lifeMs:300});
  }
}

function updateProjectiles(dt) {
  if (!projectiles || !projectiles.length) return;
  var now = Date.now();
  var alive = [], hitWalls = 0, hitEnemies = 0, expired = 0;
  for (var i = 0; i < projectiles.length; i++) {
    var p = projectiles[i];
    var spell = p.spell || spells.missile;

    // ── Homing (Magic Missile) ──────────────────────────────────────
    if (spell.homing && !p.isLob) {
      var bestDist = 999999, bestAng = p.ang, bestEnemy = null;
      for (var hi = 0; hi < enemies.length; hi++) {
        var he = enemies[hi]; if (he.health <= 0) continue;
        var hdx = he.x - p.x, hdy = he.y - p.y;
        var hd = Math.hypot(hdx, hdy); if (hd > 400 || hd < 1) continue;
        var ha = Math.atan2(hdy, hdx);
        var hda = ha - p.ang;
        while (hda > Math.PI) hda -= Math.PI * 2;
        while (hda < -Math.PI) hda += Math.PI * 2;
        if (Math.abs(hda) < 0.78 && hd < bestDist) { bestDist = hd; bestAng = ha; bestEnemy = he; }
      }
      if (bestDist < 999999) {
        var turnDa = bestAng - p.ang;
        while (turnDa > Math.PI) turnDa -= Math.PI * 2;
        while (turnDa < -Math.PI) turnDa += Math.PI * 2;
        var maxTurn = spell.homing * dt;
        if (turnDa > maxTurn) turnDa = maxTurn;
        else if (turnDa < -maxTurn) turnDa = -maxTurn;
        p.ang += turnDa;
        // Z-homing: steer vz toward target's Z
        if (bestEnemy) {
          var dz = (bestEnemy.z || 0) - (p.z || 0);
          var zSteer = spell.homing * 200 * dt;
          if (dz > 0) p.vz = Math.min((p.vz || 0) + zSteer, p.speed * 0.5);
          else if (dz < 0) p.vz = Math.max((p.vz || 0) - zSteer, -p.speed * 0.5);
        }
      }
    }

    // ── Lob physics (Poison Cloud) ──────────────────────────────────
    if (p.isLob) {
      p.vz = (p.vz || 0) + (p.gravZ || -200) * dt;
    }

    var nx = p.x + Math.cos(p.ang) * (p.hz || p.speed) * dt;
    var ny = p.y + Math.sin(p.ang) * (p.hz || p.speed) * dt;
    var nz = (p.z || 0) + (p.vz || 0) * dt;

    // ── Lob landing — use floor height at current position (works underground) ──
    var lobFloorZ = p.isLob && floorMesh ? sampleEntitySupportRenderZ(nx, ny, p.renderFloorZ, p.underground) : 0;
    if (p.isLob && nz <= lobFloorZ && (now - p.spawnMs) > 100) {
      groundEffects.push({x:nx, y:ny, renderFloorZ:lobFloorZ, underground:!!p.underground, radius:spell.cloudRadius || 50,
        duration:spell.cloudDuration || 4000, damage:spell.damage || 0.4,
        color:spell.color, spellId:spell.id, spawnMs:now, tickMs:now});
      impacts.push({x:nx, y:ny, z:lobFloorZ, spawnMs:now, lifeMs:400});
      expired++; continue;
    }

    // Check enemies FIRST — so projectiles register hits on enemies that are
    // touching or partially inside a wall (single-cell wall check would kill
    // the projectile before it reaches the enemy collision test otherwise).
    var hitEnemy = false;
    if (enemies && enemies.length && !p.isLob) {
      for (var ei = 0; ei < enemies.length; ei++) {
        var e = enemies[ei];
        var dx = nx - e.x, dy = ny - e.y;
        var dist2d = Math.sqrt(dx * dx + dy * dy);
        // Cylinder hitbox: check XY distance, then Z within enemy body span.
        // Spherical 3D distance caused guaranteed misses in 3D because enemies
        // spawn at z=0 while projectiles fly at z≈55 (eye height) — the vertical
        // gap alone exceeded the sphere radius even on a direct visual hit.
        var hitRadius = (MODE3D ? 28 : 20) + (p.r || 8);
        if (dist2d >= hitRadius) continue;
        if (MODE3D) {
          // Use actual terrain height at enemy position as Z base — e.z is always 0
          // but terrain can be elevated (spawnZ seen as high as 110+ in logs).
          var eFloorH = getEntityRenderFloorZ(e);
          // Enemy body spans floor level to ~70 units above (head height)
          if (nz < eFloorH - 12 || nz > eFloorH + 72) continue;
        }

        hitEnemy = true;
        _applyProjectileHit(spell, e, ei, nx, ny, nz, dx, dy, now);
        impacts.push({x:nx, y:ny, z:nz, spawnMs:now, lifeMs:220});
        hitEnemies++; break;
      }
    }
    if (hitEnemy) continue;

    // Spawner damage check — projectiles can destroy obelisks
    if (!p.isLob && enemySpawners && enemySpawners.length) {
      var hitSpawner = false;
      for (var spi = 0; spi < enemySpawners.length; spi++) {
        var sp2 = enemySpawners[spi];
        if (!sp2.active || sp2.hp <= 0) continue;
        if (Math.hypot(nx - sp2.x, ny - sp2.y) < 25) {
          sp2.hp -= (spell.damage || 1);
          impacts.push({x:nx, y:ny, z:nz, spawnMs:now, lifeMs:300});
          if (sp2.hp <= 0) {
            sp2.active = false;
            // Death burst
            for (var sb = 0; sb < 5; sb++) {
              var sba = Math.random() * Math.PI * 2, sbs = 40 + Math.random() * 80;
              deathEffects.push({x:sp2.x, y:sp2.y, vx:Math.cos(sba)*sbs, vy:Math.sin(sba)*sbs,
                rot:Math.random()*Math.PI*2, rotVel:(Math.random()-0.5)*8,
                len:8+Math.random()*10, spawnMs:now, lifeMs:600, color:'#3a1520'});
            }
            // Drop gold
            for (var sd = 0; sd < 5; sd++) {
              var sda = Math.random() * Math.PI * 2;
              coinDrops.push({x:sp2.x + Math.cos(sda)*6, y:sp2.y + Math.sin(sda)*6,
                underground:!!(sp2.underground || sp2.caveSpawnId), spawnMs:now, lifeMs:25000});
            }
            console.log('[SPAWNER] Destroyed at (' + Math.round(sp2.x) + ',' + Math.round(sp2.y) + ')');
          }
          hitSpawner = true; break;
        }
      }
      if (hitSpawner) continue;
    }

    // Wall check after enemy check — projectile only stops on a wall if it
    // didn't already hit an enemy (handles enemies touching/inside walls)
    var gx = Math.floor(nx / cell), gy = Math.floor(ny / cell);
    var hitWall = (gx < 0 || gy < 0 || gx >= gridW || gy >= gridH)
                  || !!(grid && grid[gy * gridW + gx]);
    if (hitWall && !p.isLob) {
      // Ore vein check — projectile is consumed here if it hits one
      var hitOre = false;
      if (oreVeins && oreVeins.length) {
        for (var oi = 0; oi < oreVeins.length; oi++) {
          if (oreVeins[oi].gx !== gx || oreVeins[oi].gy !== gy) continue;
          oreVeins[oi].hp--;
          impacts.push({x:nx, y:ny, z:nz, spawnMs:now, lifeMs:400});
          if (oreVeins[oi].hp <= 0) {
            oreVeins.splice(oi, 1);
            var dropN = 3 + Math.floor(Math.random() * 4);
            for (var di = 0; di < dropN; di++) {
              var da = Math.random() * Math.PI * 2, ds = 15 + Math.random() * 30;
              coinDrops.push({x:nx + Math.cos(da)*6, y:ny + Math.sin(da)*6,
                              underground:!!p.underground,
                              vx:Math.cos(da)*ds, vy:Math.sin(da)*ds,
                              spawnMs:now, lifeMs:25000});
            }
          }
          hitOre = true; hitWalls++; break;
        }
      }
      if (!hitOre) { impacts.push({x:nx, y:ny, z:nz, spawnMs:now, lifeMs:220}); hitWalls++; }
      continue;
    }
    p.x = nx; p.y = ny; p.z = nz;
    if (now - p.spawnMs < p.lifeMs) { alive.push(p); } else { expired++; }
  }
  if (DEBUG_EFFECTS && hitWalls + hitEnemies + expired > 0) {
    try { console.log('[PROJECTILES]', 'alive=' + alive.length, 'hitWalls=' + hitWalls, 'hitEnemies=' + hitEnemies, 'expired=' + expired); } catch (_) {}
  }
  projectiles = alive;
}


// ── Guard Tower (market center defense) ──────────────────────────────
function updateGuardTower() {
  if (!shopMarker) return;
  var now = Date.now();
  if (now - guardTowerLastFire < TOWER_FIRE_INTERVAL) return;
  var bestDist = TOWER_RANGE + 1, bestE = null;
  for (var i = 0; i < enemies.length; i++) {
    var e = enemies[i];
    if (e.health <= 0) continue;
    var d = Math.hypot(e.x - shopMarker.x, e.y - shopMarker.y);
    if (d < bestDist && hasLineOfSight(shopMarker.x, shopMarker.y, e.x, e.y)) { bestDist = d; bestE = e; }
  }
  if (!bestE) return;
  var ang = Math.atan2(bestE.y - shopMarker.y, bestE.x - shopMarker.x);
  var fh = floorMesh ? getFloorHeightAt(shopMarker.x, shopMarker.y) : 0;
  var spawnZ = fh * 25 + TOWER_HEIGHT - 5;
  var targetZ = bestE.z || 0;
  var flightTime = bestDist / towerSpell.speed;
  var vz = (flightTime > 0) ? (targetZ - spawnZ) / flightTime : 0;
  projectiles.push({
    x: shopMarker.x, y: shopMarker.y, z: spawnZ,
    ang: ang, speed: towerSpell.speed,
    hz: towerSpell.speed, vz: vz,
    spawnMs: now, lifeMs: 1800, r: 8,
    spell: towerSpell
  });
  guardTowerLastFire = now;
}

// ── Ground Effects (fire/ice/poison patches on floor) ─────────────────
function updateGroundEffects(dt) {
  if (!groundEffects || !groundEffects.length) return;
  var now = Date.now();
  var alive = [];
  for (var i = 0; i < groundEffects.length; i++) {
    var ge = groundEffects[i];
    var age = now - ge.spawnMs;
    if (age > ge.duration) continue; // expired
    // Tick damage every 400ms to enemies inside
    if (ge.damage > 0 && now - ge.tickMs >= 400) {
      ge.tickMs = now;
      for (var ei = 0; ei < enemies.length; ei++) {
        var e = enemies[ei]; if (e.health <= 0) continue;
        if (Math.hypot(e.x - ge.x, e.y - ge.y) >= ge.radius) continue;
        var geDmg = ge.damage * 0.5;
        e.health -= geDmg; stats.totalDamageDone += geDmg;
        applyRelicOnHit(geDmg, e, false);
        if (e.health <= 0) {
          stats.totalEnemiesKilled++;
          // Plague upgrade: enemies killed in poison cloud drop bonus coins
          if (ge.spellId === 'poison' && spells.poison && spells.poison.tier >= 2) {
            var pn = 2 + Math.floor(Math.random() * 3);
            for (var pi = 0; pi < pn; pi++) {
              var pa = Math.random() * Math.PI * 2, ps = 10 + Math.random() * 20;
              coinDrops.push({x:e.x + Math.cos(pa)*4, y:e.y + Math.sin(pa)*4,
                underground:!!(e.underground || e.caveSpawnId),
                vx:Math.cos(pa)*ps, vy:Math.sin(pa)*ps, spawnMs:now, lifeMs:25000});
            }
          }
        }
        e.damageFlash = now + 100; e.damageFlashColor = ge.color;
        if (ge.spellId === 'fire') { e.burnUntil = now + 2000; e.burnDmgLast = e.burnDmgLast || now; }
      }
    }
    // Ice patches apply slow to enemies walking through
    if (ge.spellId === 'ice' && ge.slowFactor) {
      for (var si = 0; si < enemies.length; si++) {
        var se = enemies[si]; if (se.health <= 0) continue;
        if (Math.hypot(se.x - ge.x, se.y - ge.y) >= ge.radius) continue;
        se.slowUntil = Math.max(se.slowUntil || 0, now + 500);
      }
    }
    alive.push(ge);
  }
  groundEffects = alive;
}

// Single consolidated effect tick — one Date.now(), four small arrays
function tickEffects(dt) {
  var now = Date.now();

  // Impacts — timer only
  if (impacts && impacts.length) {
    var ki = [];
    for (var i = 0; i < impacts.length; i++) { if (now - impacts[i].spawnMs < impacts[i].lifeMs) ki.push(impacts[i]); }
    impacts = ki;
  }

  // Cone effects — timer only
  if (coneEffects && coneEffects.length) {
    var kc = [];
    for (var i = 0; i < coneEffects.length; i++) { if (now - coneEffects[i].spawnMs < coneEffects[i].lifeMs) kc.push(coneEffects[i]); }
    coneEffects = kc;
  }

  // Death effects — bone physics (gravity + friction)
  if (deathEffects && deathEffects.length) {
    var kd = [];
    for (var i = 0; i < deathEffects.length; i++) {
      var de = deathEffects[i];
      if (now - de.spawnMs < de.lifeMs) {
        de.x += de.vx * dt; de.y += de.vy * dt;
        de.vy += 60 * dt; de.vx *= 0.96; de.vy *= 0.96;
        kd.push(de);
      }
    }
    deathEffects = kd;
  }

  // Vacuum pull — boosted during dash (sweep effect)
  var isDashing = now < dashUntil;
  var vacRange = isDashing ? 140 : 80;
  var vacPull  = isDashing ? 6.0 : 3.5;

  // Soul orbs — vacuum pull + proximity collection
  soulOrbs = updateCollectibles(soulOrbs,
    {collectRadius: 22, vacuumRadius: vacRange, vacuumPull: vacPull},
    function(orb) {
      mana = Math.min(MANA_MAX, mana + 15);
      console.log('[SOUL] Orb collected! Mana +15');
    });

  // Coin drops — vacuum pull + proximity collection
  coinDrops = updateCollectibles(coinDrops,
    {collectRadius: 18, vacuumRadius: vacRange, vacuumPull: vacPull},
    function(coin) { coins++; });

  // Shop proximity check
  shopNearby = false;
  if (shopMarker) {
    var sdx = pos.x - shopMarker.x, sdy = pos.y - shopMarker.y;
    if (Math.hypot(sdx, sdy) < 90) shopNearby = true;
    else if (shopOpen) { shopOpen = false; shopSelIdx = 0; } // auto-close if player walks away
  }

  // Shrine proximity check
  nearestShrine = null;
  for (var _si = 0; _si < shrines.length; _si++) {
    var _shr = shrines[_si];
    if (_shr.used) continue;
    var _sdx = pos.x - _shr.x, _sdy = pos.y - _shr.y;
    if (Math.hypot(_sdx, _sdy) < 50) {
      nearestShrine = _shr;
      break;
    }
  }

  // Arena altar proximity check — press E to start challenge
  nearestArenaAltar = null;
  if (!arenaChallenge) {
    for (var _ai = 0; _ai < largeStructures.length; _ai++) {
      var _ast = largeStructures[_ai];
      if (_ast.type !== 'arena') continue;
      var _aKey = _ast.regionX + ',' + _ast.regionY;
      if (completedArenas[_aKey]) continue;
      var _adx = pos.x - _ast.x, _ady = pos.y - _ast.y;
      if (Math.hypot(_adx, _ady) < 60) {
        nearestArenaAltar = _ast;
        break;
      }
    }
  }

  // Fortress interactable proximity check — forge, garrison, lectern
  nearestFortressInteract = null;
  fortressLockedNear = null;
  if (!forgeOpen) {
    for (var _fi = 0; _fi < largeStructures.length; _fi++) {
      var _fst = largeStructures[_fi];
      if (_fst.type !== 'fortress') continue;
      var _fKey = _fst.regionX + ',' + _fst.regionY;
      var _fComp = completedFortresses[_fKey] || {};
      var _fdx = pos.x - _fst.x, _fdy = pos.y - _fst.y;
      // Enemy-clear check — are any alive enemies still inside the fortress?
      var _fortInnerR = CHUNK_SIZE * 1.5 * (_fst.scale || 1);
      var _fortCleared = true;
      for (var _fei = 0; _fei < enemies.length; _fei++) {
        var _fen = enemies[_fei];
        if (_fen.health <= 0) continue;
        if (Math.hypot(_fen.x - _fst.x, _fen.y - _fst.y) < _fortInnerR) { _fortCleared = false; break; }
      }
      // Check 3 interactable positions within the keep
      var _interacts = [
        {type: 'forge',    ox: 0,         oy: -cell * 4, used: !!_fComp.forge},
        {type: 'lectern',  ox: cell * 4,  oy: cell * 2,  used: !!_fComp.lectern},
        {type: 'garrison', ox: -cell * 4, oy: cell * 2,  used: !!_fComp.garrison}
      ];
      var _playerNearRelics = false;
      for (var _ii = 0; _ii < _interacts.length; _ii++) {
        var _int = _interacts[_ii];
        if (_int.used) continue;
        var _ix = _fst.x + _int.ox, _iy = _fst.y + _int.oy;
        if (Math.hypot(pos.x - _ix, pos.y - _iy) < 50) {
          _playerNearRelics = true;
          if (_fortCleared) {
            nearestFortressInteract = {type: _int.type, structure: _fst, x: _ix, y: _iy};
          } else {
            fortressLockedNear = {structure: _fst, x: _fst.x, y: _fst.y};
          }
          break;
        }
      }
      if (nearestFortressInteract || fortressLockedNear) break;
    }
  }

  // Active buffs — update global speed from base so all input handlers pick it up
  var BASE_SPEED = GAME_CONFIG.player.baseSpeed;
  var hatSpeedMult = (equipment.hat && equipment.hat.speedBonus) ? (1 + equipment.hat.speedBonus) : 1;
  speed = ((now < speedBoostUntil) ? BASE_SPEED * 1.25 : BASE_SPEED) * hatSpeedMult * (1 + permanentSpeedBonus);

  // Regen buff — heal 1 HP every 3 seconds
  if (now < regenBoostUntil && now - lastRegenTick > 3000) {
    health = Math.min(HEALTH_MAX, health + 1);
    lastRegenTick = now;
  }
}
