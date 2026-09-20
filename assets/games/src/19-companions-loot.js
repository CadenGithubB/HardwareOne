// =============================================
// COMPANION SYSTEM
// =============================================

function updateCompanions(dt) {
  if (!companions || !companions.length) return;
  var now = Date.now();
  for (var i = 0; i < companions.length; i++) {
    var c = companions[i];
    var def = COMPANION_DEFS[c.type];
    if (!def) continue;
    if (!Number.isFinite(c.renderFloorZ)) {
      c.renderFloorZ = sampleEntitySupportRenderZ(c.x, c.y, getPlayerRenderFloorZ());
    }

    // Initialize wander angle on first frame
    if (c.wanderAng === undefined) c.wanderAng = Math.random() * Math.PI * 2;

    // Distance to player
    var cdx = pos.x - c.x, cdy = pos.y - c.y;
    var cdist = Math.hypot(cdx, cdy);

    // Teleport if too far
    if (cdist > def.teleportDist) {
      var tAng = cam.ang + (Math.random() - 0.5) * 1.2;
      c.x = pos.x + Math.cos(tAng) * def.followIdeal;
      c.y = pos.y + Math.sin(tAng) * def.followIdeal;
      c.renderFloorZ = sampleEntitySupportRenderZ(c.x, c.y, getPlayerRenderFloorZ());
      cdx = pos.x - c.x; cdy = pos.y - c.y;
      cdist = Math.hypot(cdx, cdy);
    }

    // Wander orbit — smoothly orbit around the player instead of
    // standing at their feet. The wander angle drifts continuously,
    // biased toward the direction the player is facing so the slime
    // tends to stay in front / beside the player.
    var wanderR = def.wanderRadius || 55;
    c.wanderAng += (def.wanderSpeed || 0.7) * dt;
    // Bias toward camera forward direction (slime stays in view)
    var idealAng = cam.ang + Math.sin(c.wanderAng) * 1.3;
    var targetX = pos.x + Math.cos(idealAng) * wanderR;
    var targetY = pos.y + Math.sin(idealAng) * wanderR;

    // Movement toward wander target
    var prevZ = c.z;
    var tdx = targetX - c.x, tdy = targetY - c.y;
    var tdist = Math.hypot(tdx, tdy);

    // If too far from player, prioritize catching up
    if (cdist > def.followDist * 2.5) {
      tdx = cdx; tdy = cdy; tdist = cdist;
    }

    var moving = tdist > 5;
    if (moving) {
      var moveSpd = def.speed * dt;
      // Move faster when far from player to keep up
      if (cdist > def.followDist) moveSpd *= 1.0 + (cdist - def.followDist) * 0.01;
      var nx = c.x + (tdx / tdist) * moveSpd;
      var ny = c.y + (tdy / tdist) * moveSpd;
      // Wall collision
      if (!isInGridWall(nx, ny, 5)) {
        c.x = nx; c.y = ny;
      } else if (!isInGridWall(nx, c.y, 5)) {
        c.x = nx;
      } else if (!isInGridWall(c.x, ny, 5)) {
        c.y = ny;
      }
      c.phase += moveSpd * 0.10;
    } else {
      // Idle bounce — slow
      c.phase += dt * 2.5;
    }

    // Carry support with this actor as it walks; bounce remains a local offset.
    var nextSupportZ = sampleEntitySupportRenderZ(c.x, c.y, c.renderFloorZ);
    if (Number.isFinite(nextSupportZ)) c.renderFloorZ = nextSupportZ;
    // Bounce height
    c.z = Math.abs(Math.sin(c.phase)) * def.bounceHeight;

    // Landing detection — squash on land
    var landed = prevZ > 0.5 && c.z <= 0.5;
    if (landed) {
      c.squash = 0.5;
    }

    // Ranged attack — fire a projectile at nearest enemy on cooldown
    if (now - c.lastAttackMs > def.attackCooldown) {
      var bestE = null, bestD = def.attackRange;
      for (var ei = 0; ei < enemies.length; ei++) {
        var e = enemies[ei]; if (e.health <= 0) continue;
        var edx = e.x - c.x, edy = e.y - c.y;
        var ed = Math.hypot(edx, edy);
        if (ed < bestD) { bestD = ed; bestE = e; }
      }
      if (bestE) {
        c.lastAttackMs = now;
        // Fire projectile toward enemy
        var pdx = bestE.x - c.x, pdy = bestE.y - c.y;
        var pdist = Math.hypot(pdx, pdy);
        if (pdist > 1) {
          var pvx = (pdx / pdist) * def.projSpeed;
          var pvy = (pdy / pdist) * def.projSpeed;
          impacts.push({
            x: c.x, y: c.y, z: getEntityRenderFloorZ(c) + c.z * 25,
            vx: pvx, vy: pvy,
            spawnMs: now, lifeMs: def.projLifeMs,
            color: def.color, size: 4,
            isCompanionProj: true,
            damage: def.attackDamage
          });
        }
      }
    }

    // Squash recovery
    if (c.squash === undefined) c.squash = 1.0;
    c.squash += (1.0 - c.squash) * 0.15;
  }

  // Companion-to-companion separation
  for (var a = 0; a < companions.length; a++) {
    for (var b = a + 1; b < companions.length; b++) {
      var dx = companions[b].x - companions[a].x, dy = companions[b].y - companions[a].y;
      var d = Math.hypot(dx, dy);
      if (d < 30 && d > 0.1) {
        var push = (30 - d) * 0.3 / d;
        companions[a].x -= dx * push; companions[a].y -= dy * push;
        companions[b].x += dx * push; companions[b].y += dy * push;
      }
    }
  }

  // Update companion projectiles (they're stored in impacts with isCompanionProj flag)
  for (var pi = impacts.length - 1; pi >= 0; pi--) {
    var imp = impacts[pi];
    if (!imp.isCompanionProj) continue;
    // Move projectile
    imp.x += imp.vx * dt;
    imp.y += imp.vy * dt;
    // Check enemy hit
    for (var ei = 0; ei < enemies.length; ei++) {
      var e = enemies[ei]; if (e.health <= 0) continue;
      var hdx = e.x - imp.x, hdy = e.y - imp.y;
      if (Math.hypot(hdx, hdy) < 15) {
        e.health -= imp.damage;
        e.damageFlash = 3;
        e.damageFlashColor = imp.color;
        // Replace projectile with hit impact
        imp.isCompanionProj = false;
        imp.vx = 0; imp.vy = 0;
        imp.lifeMs = 250;
        imp.spawnMs = Date.now();
        break;
      }
    }
  }
}

function collectChest(ch) {
  if (!ch || ch.collected) return;
  if (ch.relicId && RELIC_DEFS[ch.relicId]) {
    // Relic chest
    var rDef = RELIC_DEFS[ch.relicId];
    var newRelic = {id: rDef.id, name: rDef.name, desc: rDef.desc, effect: rDef.effect,
                    value: rDef.value, color: rDef.color, accent: rDef.accent, shape: rDef.shape};
    var oldRelic = equipment.relic;
    if (oldRelic) {
      equipment.relic = newRelic;
      ch.relicId = oldRelic.id;
      pushToast('Swapped ' + oldRelic.name + ' for ' + newRelic.name, '#88aacc', 3000);
      console.log('[CHEST] Relic swapped: ' + oldRelic.name + ' → ' + newRelic.name);
    } else {
      equipment.relic = newRelic;
      ch.collected = true;
      pushToast('Found ' + newRelic.name, '#aaaaff', 3000);
      console.log('[CHEST] Relic equipped: ' + newRelic.name);
    }
    if (newRelic.effect === 'phoenixRevive') phoenixFeatherUsed = false;
  } else if (ch.equipId) {
    var def = EQUIPMENT_DEFS[ch.equipId];
    if (!def) return;
    var inst = createEquipInstance(ch.equipId, ch.quality || 1.0);
    var oldItem = equipment[def.slot];
    if (oldItem) {
      // Swap: equip new, put old into chest
      equipment[def.slot] = inst;
      ch.equipId = oldItem.id;
      ch.quality = oldItem.quality || 1.0;
      pushToast('Swapped for ' + inst.displayName, '#88aacc', 3000);
      console.log('[CHEST] Swapped ' + (oldItem.displayName || oldItem.name) + ' → ' + inst.displayName);
    } else {
      equipment[def.slot] = inst;
      ch.collected = true;
      pushToast('Equipped ' + inst.displayName, '#88aacc', 3000);
      console.log('[CHEST] Equipped: ' + inst.displayName + ' (' + def.slot + ') Q=' + (ch.quality || 1.0).toFixed(2));
    }
  } else {
    // Gold chest
    coins += ch.gold;
    ch.collected = true;
    pushToast('+' + ch.gold + ' gold', '#d4a820', 2000);
    console.log('[CHEST] Collected! +' + ch.gold + ' gold (total: ' + coins + ')');
  }
}

// Enemy spawners periodically emit enemies when the player is within range
// AND has line-of-sight (no spawning through walls). Global cap prevents
// screen-flooding when multiple spawners are nearby.
var SPAWNER_GLOBAL_MAX = GAME_CONFIG.spawner.globalMax;

var _spawnerEnemyCount = 0;
var _spawnerCountLastUpdate = 0;
function updateEnemySpawners() {
  if (!enemySpawners || !enemySpawners.length) return;
  var now = Date.now();
  var typeKeys = Object.keys(enemyTypes);

  // Recount spawner-born enemies every 500ms instead of every frame
  if (now - _spawnerCountLastUpdate > 500) {
    _spawnerEnemyCount = 0;
    for (var ei = 0; ei < enemies.length; ei++) {
      if (enemies[ei].fromSpawner && enemies[ei].health > 0) _spawnerEnemyCount++;
    }
    _spawnerCountLastUpdate = now;
  }
  var spawnerEnemiesAlive = _spawnerEnemyCount;

  for (var i = 0; i < enemySpawners.length; i++) {
    var sp = enemySpawners[i];
    if (!sp.active || sp.hp <= 0) continue;
    var pdist = Math.hypot(pos.x - sp.x, pos.y - sp.y);

    // Only spawn when player is within 300px and cooldown elapsed
    if (pdist > 300) continue;
    if (sp.spawnCount >= sp.maxSpawns) continue;
    if (now - sp.lastSpawn < sp.cooldownMs) continue;

    // Global cap — don't flood the screen
    if (spawnerEnemiesAlive >= SPAWNER_GLOBAL_MAX) continue;

    // Line-of-sight check — spawner must "see" the player (no walls between)
    if (!hasLineOfSight(sp.x, sp.y, pos.x, pos.y)) continue;

    // Spawn an enemy near the spawner
    var spAng = Math.random() * Math.PI * 2;
    var spDist = 30 + Math.random() * 20;
    var ex = sp.x + Math.cos(spAng) * spDist;
    var ey = sp.y + Math.sin(spAng) * spDist;
    // Don't spawn in walls
    if (isInGridWall(ex, ey, 8)) continue;

    var eTypeKey = typeKeys[Math.floor(Math.random() * typeKeys.length)];
    var eType = enemyTypes[eTypeKey];
    var wps = generatePatrolWaypoints(ex, ey, eType.chaseRange);
    enemies.push({x:ex, y:ey, z:0, enemyType:eType, health:eType.health, maxHealth:eType.health, speed:eType.speed,
      chaseRange:eType.chaseRange, lastUpdate:0, damageFlash:0, damageFlashColor:'#ffffff',
      slowUntil:0, burnUntil:0, burnDmgLast:0, iceHits:0, fireHits:0, lightningHits:0,
      vx:0, vy:0, aggroAt:0, attackState:'idle', attackStateUntil:0,
      patrolWaypoints:wps, patrolIdx:0, facing:0, fromSpawner:true});
    sp.lastSpawn = now;
    sp.spawnCount++;
    spawnerEnemiesAlive++;
    console.log('[SPAWNER] Spawned enemy #' + sp.spawnCount + '/' + sp.maxSpawns +
                ' near (' + Math.round(sp.x) + ',' + Math.round(sp.y) + ')' +
                ' [alive from spawners: ' + spawnerEnemiesAlive + '/' + SPAWNER_GLOBAL_MAX + ']');
  }
}


// Shared layout keeps the scene-depth bounds identical to the animated sprite,
// including its label and soft halo. Screen-edge clamping would detach loot
// from its real support and can pull buried objects back into the viewport.
function lootPickupRenderLayout(kind, item, vis, C, now) {
  var phase = Number.isFinite(item.bob) ? item.bob : 0;
  var size, centerY, label = '', blur;
  if (kind === 'soul') {
    size = Math.max(8, Math.min(32, Math.floor(C.h * 0.65 * getScale3D('smPickup') / (vis.fwd * 0.12 + 1))));
    centerY = vis.sy-size*1.6+Math.sin(now*0.003+phase)*4;
    blur = 28;
  } else if (kind === 'tome') {
    size = Math.max(12, Math.min(48, Math.floor(C.h * 0.80 * getScale3D('lgPickup') / (vis.fwd * 0.12 + 1))));
    centerY = vis.sy-size*2.5+Math.sin(now*0.002+phase)*6;
    label = 'Arcane Tome'; blur = 40;
  } else {
    size = Math.max(10, Math.min(36, Math.floor(C.h * 0.65 * getScale3D('smPickup') / (vis.fwd * 0.12 + 1))));
    centerY = vis.sy-size*2+Math.sin(now*0.003+phase)*5;
    label = item.type === 'heartCrystal' ? 'Heart Crystal' : item.type === 'manaStar' ? 'Mana Star' : 'Movement Tome';
    blur = 28;
  }
  var labelSize = Math.max(10, Math.floor(20 * getScale3D('smText') * projScale / vis.fwd));
  var halfW = size+blur, top = centerY-size-blur, bottom = centerY+size+blur;
  if (label) {
    ctx.save();
    ctx.font = 'bold ' + labelSize + 'px monospace';
    halfW = Math.max(halfW,ctx.measureText(label).width*0.5+2);
    ctx.restore();
    top = Math.min(top,centerY-size-8-labelSize-2);
  }
  return {size:size,centerY:centerY,label:label,labelSize:labelSize,phase:phase,
    bounds:{x:vis.sx-halfW,y:top,width:halfW*2,height:bottom-top}};
}

function drawSoulOrbs3D() {
  renderEntities3D(soulOrbs, {maxDist: 600, groundAnchor: true, fadeFraction: 1,
    bounds:function(orb,vis,C,now){return lootPickupRenderLayout('soul',orb,vis,C,now).bounds;}},
    function(orb, vis, C, ctx, now) {
      var h = C.h, fwd = vis.fwd;
      var layout = lootPickupRenderLayout('soul',orb,vis,C,now);
      var orbSize = layout.size, orbY = layout.centerY;
      var pulse = 0.75 + 0.25 * Math.sin(now * 0.005 + layout.phase);
      ctx.save();
      ctx.globalAlpha = 0.92;
      ctx.shadowBlur = 14 * pulse; ctx.shadowColor = '#9933ff';
      ctx.fillStyle = '#cc88ff';
      ctx.beginPath(); ctx.arc(vis.sx, orbY, orbSize * pulse, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = 'rgba(255,255,255,0.75)';
      ctx.beginPath(); ctx.arc(vis.sx - orbSize * 0.28, orbY - orbSize * 0.28, orbSize * 0.28, 0, Math.PI * 2); ctx.fill();
      ctx.shadowBlur = 0;
      ctx.restore();
    });
}

function drawArcaneTomes3D() {
  renderEntities3D(arcaneTomes, {maxDist: 800, groundAnchor: true, fadeFraction: 1,
    bounds:function(tome,vis,C,now){return lootPickupRenderLayout('tome',tome,vis,C,now).bounds;}},
    function(tome, vis, C, ctx, now) {
      var h = C.h, sx = vis.sx, fwd = vis.fwd;
      var layout = lootPickupRenderLayout('tome',tome,vis,C,now);
      var sz = layout.size, cy = layout.centerY;
      var spin = (now * 0.002 + layout.phase) % (Math.PI * 2);
      var squish = Math.abs(Math.cos(spin));
      var pulse = 0.8 + 0.2 * Math.sin(now * 0.004);
      ctx.save();
      ctx.globalAlpha = 0.95 * vis.fade;
      ctx.shadowBlur = 20 * pulse; ctx.shadowColor = '#aa44ff';
      ctx.fillStyle = '#bb66ff';
      ctx.beginPath();
      ctx.moveTo(sx, cy - sz); ctx.lineTo(sx + sz * 0.6 * squish, cy);
      ctx.lineTo(sx, cy + sz * 0.8); ctx.lineTo(sx - sz * 0.6 * squish, cy);
      ctx.closePath(); ctx.fill();
      ctx.fillStyle = 'rgba(255,220,255,0.6)';
      ctx.beginPath();
      ctx.moveTo(sx, cy - sz * 0.5); ctx.lineTo(sx + sz * 0.25 * squish, cy);
      ctx.lineTo(sx, cy + sz * 0.3); ctx.lineTo(sx - sz * 0.25 * squish, cy);
      ctx.closePath(); ctx.fill();
      ctx.shadowBlur = 0;
      var labelSize = layout.labelSize;
      ctx.fillStyle = '#eeccff';
      ctx.font = 'bold ' + labelSize + 'px monospace';
      ctx.textAlign = 'center';
      ctx.fillText('Arcane Tome', sx, cy - sz - 8);
      ctx.restore();
    });
}

function drawStatPickups3D() {
  renderEntities3D(statPickups, {maxDist: 600, groundAnchor: true, fadeFraction: 1,
    bounds:function(sp,vis,C,now){return lootPickupRenderLayout('stat',sp,vis,C,now).bounds;}},
    function(sp, vis, C, ctx, now) {
      var h = C.h, sx = vis.sx, fwd = vis.fwd;
      var layout = lootPickupRenderLayout('stat',sp,vis,C,now);
      var sz = layout.size, cy = layout.centerY;
      var pulse = 0.75 + 0.25 * Math.sin(now * 0.005 + layout.phase);
      ctx.save();
      ctx.globalAlpha = 0.95 * vis.fade;
      if (sp.type === 'heartCrystal') {
        ctx.shadowBlur = 14 * pulse; ctx.shadowColor = '#ff2222';
        ctx.fillStyle = '#ff4444';
        var hs = sz * pulse;
        ctx.beginPath();
        ctx.moveTo(sx, cy + hs * 0.6);
        ctx.bezierCurveTo(sx - hs, cy - hs * 0.3, sx - hs * 0.5, cy - hs, sx, cy - hs * 0.4);
        ctx.bezierCurveTo(sx + hs * 0.5, cy - hs, sx + hs, cy - hs * 0.3, sx, cy + hs * 0.6);
        ctx.fill();
      } else if (sp.type === 'manaStar') {
        ctx.shadowBlur = 14 * pulse; ctx.shadowColor = '#2266ff';
        ctx.fillStyle = '#4488ff';
        var ss = sz * pulse;
        ctx.beginPath();
        for (var si = 0; si < 8; si++) {
          var sAng = si * Math.PI / 4 - Math.PI / 2;
          var sr = (si % 2 === 0) ? ss : ss * 0.45;
          var px = sx + Math.cos(sAng) * sr, py = cy + Math.sin(sAng) * sr;
          if (si === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py);
        }
        ctx.closePath(); ctx.fill();
      } else {
        ctx.shadowBlur = 14 * pulse; ctx.shadowColor = '#22cc44';
        ctx.fillStyle = '#44dd55';
        var ds = sz * pulse;
        ctx.beginPath();
        ctx.moveTo(sx, cy - ds); ctx.lineTo(sx + ds * 0.6, cy);
        ctx.lineTo(sx, cy + ds * 0.7); ctx.lineTo(sx - ds * 0.6, cy);
        ctx.closePath(); ctx.fill();
      }
      ctx.fillStyle = 'rgba(255,255,255,0.4)';
      ctx.beginPath(); ctx.arc(sx - sz * 0.15, cy - sz * 0.15, sz * 0.25 * pulse, 0, Math.PI * 2); ctx.fill();
      ctx.shadowBlur = 0;
      var labelSize = layout.labelSize;
      var labelText = layout.label;
      ctx.fillStyle = sp.type === 'heartCrystal' ? '#ffaaaa' : sp.type === 'manaStar' ? '#aaccff' : '#aaffaa';
      ctx.font = 'bold ' + labelSize + 'px monospace';
      ctx.textAlign = 'center';
      ctx.fillText(labelText, sx, cy - sz - 5);
      ctx.restore();
    });
}

function drawCompanions3D() {
  renderEntities3D(companions, {maxDist: 600, groundAnchor: true, fadeFraction: 0.9,
    bounds:function(c,vis,C) {
      var def = COMPANION_DEFS[c.type];
      if (!def) return null;
      var size = Math.max(14,Math.min(65,Math.floor(C.h*0.72*getScale3D('creature')/(vis.fwd*0.10+1))))*def.size;
      // Bounds include the maximum recoil/squash stretch and attack halo.
      var jump = (Number.isFinite(c.z) ? c.z : 0)*size/8;
      var halfW = size*2+36;
      return {x:vis.sx-halfW,y:vis.sy-size*2.5-jump-36,width:halfW*2,height:size*3+jump+72};
    }},
    function(c, vis, C, ctx, now) {
      var def = COMPANION_DEFS[c.type];
      if (!def) return;
      var h = C.h, sx = vis.sx, fwd = vis.fwd;
      var baseSz = Math.max(14, Math.min(65, Math.floor(h * 0.72 * getScale3D('creature') / (fwd * 0.10 + 1)))) * def.size;
      var floorY = vis.sy;
      var zPx = (Number.isFinite(c.z) ? c.z : 0) * (baseSz / 8);
      var squash = c.squash !== undefined ? c.squash : 1.0;
      var stretchY = c.z > def.bounceHeight * 0.7 ? 1.2 : 1.0;

      // Fire animation — brief recoil squash + glow when attacking
      var fireAge = now - (c.lastAttackMs || 0);
      var firing = fireAge < 200;
      if (firing) {
        var fireT = fireAge / 200; // 0→1 over 200ms
        // Quick squash then recover
        squash *= (1.0 - 0.35 * (1.0 - fireT));
      }

      var bodyW = baseSz * (2 - squash);
      var bodyH = baseSz * squash * stretchY;
      ctx.save();
      // Shadow on ground
      ctx.globalAlpha = 0.25 * vis.fade;
      ctx.fillStyle = '#000000';
      var shadowScale = 1 - c.z / (def.bounceHeight * 2);
      ctx.beginPath();
      ctx.ellipse(sx, floorY, bodyW * 0.7 * shadowScale, baseSz * 0.2 * shadowScale, 0, 0, Math.PI * 2);
      ctx.fill();
      // Body — apply night darkness
      var _compLight = (ambientLight < 0.85) ? (0.3 + ambientLight * 0.7) : 1.0;
      ctx.globalAlpha = 0.9 * vis.fade * _compLight;
      var bodyY = floorY - bodyH - zPx;
      // Brighter glow when firing
      var glowBlur = firing ? 18 : 8;
      var glowColor = firing ? '#88ff88' : def.color;
      ctx.shadowBlur = glowBlur * _compLight; ctx.shadowColor = glowColor;
      ctx.fillStyle = firing ? '#66ff77' : def.color;
      ctx.beginPath();
      ctx.ellipse(sx, bodyY + bodyH * 0.5, bodyW, bodyH, 0, 0, Math.PI * 2);
      ctx.fill();
      // Highlight
      ctx.fillStyle = firing ? 'rgba(255,255,255,0.4)' : 'rgba(255,255,255,0.2)';
      ctx.beginPath();
      ctx.ellipse(sx, bodyY + bodyH * 0.65, bodyW * 0.6, bodyH * 0.5, 0, 0, Math.PI * 2);
      ctx.fill();
      ctx.shadowBlur = 0;
      // Eyes — widen when firing
      var eyeY = bodyY + bodyH * 0.25;
      var eyeSpread = bodyW * 0.35;
      var eyeR = baseSz * (firing ? 0.22 : 0.18);
      ctx.fillStyle = def.eyeColor;
      ctx.beginPath(); ctx.arc(sx - eyeSpread, eyeY, eyeR, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath(); ctx.arc(sx + eyeSpread, eyeY, eyeR, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = '#111111';
      ctx.beginPath(); ctx.arc(sx - eyeSpread + eyeR * 0.2, eyeY + eyeR * 0.1, eyeR * 0.55, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath(); ctx.arc(sx + eyeSpread + eyeR * 0.2, eyeY + eyeR * 0.1, eyeR * 0.55, 0, Math.PI * 2); ctx.fill();
      // Mouth — opens when firing (small dark oval)
      if (firing) {
        var mouthY = bodyY + bodyH * 0.55;
        var mouthW = bodyW * 0.25 * (1.0 - fireAge / 200);
        var mouthH = mouthW * 0.7;
        ctx.fillStyle = '#115522';
        ctx.beginPath();
        ctx.ellipse(sx, mouthY, mouthW, mouthH, 0, 0, Math.PI * 2);
        ctx.fill();
      }
      ctx.restore();
    });
}

function drawFortressAllies3D() {
  if (!fortressAllies.length || !MODE3D) return;
  renderEntities3D(fortressAllies, {maxDist: 600, groundAnchor: true, fadeFraction: 0.9,
    bounds:function(ally,vis,C) {
      var size = Math.max(16,Math.min(60,Math.floor(C.h*0.7*getScale3D('creature')/(vis.fwd*0.10+1))));
      return {x:vis.sx-size,y:vis.sy-size*1.75-8,width:size*2,height:size*2+12};
    }},
    function(ally, vis, C, ctx, now) {
      var h = C.h, sx = vis.sx, fwd = vis.fwd;
      var baseSz = Math.max(16, Math.min(60, Math.floor(h * 0.7 * getScale3D('creature') / (fwd * 0.10 + 1))));
      var floorY = vis.sy;
      // Walking bob
      var walkBob = Math.sin(ally.phase) * baseSz * 0.05;
      // Night darkness
      var _allyLight = (ambientLight < 0.85) ? (0.3 + ambientLight * 0.7) : 1.0;
      ctx.save();
      ctx.globalAlpha = vis.fade * _allyLight;
      // Shadow
      ctx.globalAlpha = 0.25 * vis.fade;
      ctx.fillStyle = '#000000';
      ctx.beginPath(); ctx.ellipse(sx, floorY, baseSz * 0.6, baseSz * 0.15, 0, 0, Math.PI * 2); ctx.fill();
      ctx.globalAlpha = vis.fade;
      // Damage flash
      var flashMix = ally.damageFlash || 0;
      // Body (armored rectangle)
      var bodyW = baseSz * 0.7, bodyH = baseSz * 1.2;
      var bodyY = floorY - bodyH + walkBob;
      var bodyCol = flashMix > 0.1 ? '#ff6644' : '#8a7a50';
      ctx.fillStyle = bodyCol;
      ctx.fillRect(sx - bodyW / 2, bodyY, bodyW, bodyH);
      // Armor plate highlight
      ctx.fillStyle = flashMix > 0.1 ? '#ffaa88' : '#b8a870';
      ctx.fillRect(sx - bodyW * 0.35, bodyY + bodyH * 0.1, bodyW * 0.7, bodyH * 0.5);
      // Head (circle)
      var headR = baseSz * 0.25;
      var headY = bodyY - headR * 0.5;
      ctx.fillStyle = flashMix > 0.1 ? '#ff8866' : '#c8b080';
      ctx.beginPath(); ctx.arc(sx, headY, headR, 0, Math.PI * 2); ctx.fill();
      // Helmet
      ctx.fillStyle = '#666';
      ctx.beginPath(); ctx.arc(sx, headY - headR * 0.2, headR * 0.9, Math.PI, 0); ctx.fill();
      // Sword
      var swordLen = baseSz * 0.8;
      ctx.strokeStyle = '#aaaacc'; ctx.lineWidth = Math.max(1.5, baseSz * 0.06);
      ctx.beginPath();
      ctx.moveTo(sx + bodyW / 2, bodyY + bodyH * 0.3);
      ctx.lineTo(sx + bodyW / 2 + swordLen * 0.4, bodyY + bodyH * 0.3 - swordLen * 0.7);
      ctx.stroke();
      // Health bar
      var hbW = bodyW * 1.2, hbH = Math.max(2, baseSz * 0.08);
      var hbX = sx - hbW / 2, hbY = headY - headR - hbH - 3;
      var hpPct = ally.health / ally.maxHealth;
      ctx.fillStyle = 'rgba(0,0,0,0.5)'; ctx.fillRect(hbX, hbY, hbW, hbH);
      ctx.fillStyle = hpPct > 0.5 ? '#44cc44' : hpPct > 0.25 ? '#cccc44' : '#cc4444';
      ctx.fillRect(hbX, hbY, hbW * hpPct, hbH);
      ctx.restore();
    });
}

function drawArenaHUD() {
  // Replaced by toast notifications — no purple overlay banner
}

function drawArenaAltarPrompt3D() {
  if (!nearestArenaAltar || arenaChallenge) return;
  var C = getCam3D();
  function proj(wx, wy, wz) { return projToScreen(wx, wy, wz, C); }
  var altar = nearestArenaAltar;
  var floorZ = floorMesh ? getFloorHeightAt(altar.x, altar.y) * 25 : 0;
  // Glowing rune circle on ground
  var p = proj(altar.x, altar.y, floorZ + 2);
  if (p) {
    var now = Date.now();
    var pulse = 0.6 + 0.4 * Math.sin(now * 0.004);
    var sz = Math.max(14, Math.floor(60 * getScale3D('mdStructure') * projScale / (p.fwd || 10)));
    ctx.save();
    ctx.globalAlpha = 0.6 * pulse;
    ctx.strokeStyle = '#bb66ff';
    ctx.lineWidth = 3;
    ctx.beginPath(); ctx.arc(p.sx, p.sy, sz, 0, Math.PI * 2); ctx.stroke();
    ctx.globalAlpha = 0.3 * pulse;
    ctx.fillStyle = '#7722cc';
    ctx.fill();
    ctx.restore();
  }
  // "[E] Challenge Arena" text prompt
  var tp = proj(altar.x, altar.y, floorZ + 35);
  if (tp) {
    var fwd = tp.fwd || 10;
    var fontSize = Math.max(14, Math.floor(28 * getScale3D('lgText') * projScale / fwd));
    ctx.save();
    ctx.globalAlpha = 1;
    ctx.fillStyle = '#ffffff';
    ctx.font = 'bold ' + fontSize + 'px monospace';
    ctx.textAlign = 'center';
    ctx.fillText('[E] Challenge Arena', tp.sx, tp.sy);
    ctx.restore();
  }
}

// Detailed subsystem timing for performance profiling
var _perfBreakdown = {};
var _perfBreakdownLog = 0;
var _ptErrors = {};
// Ring buffer for the perf overlay. Captures last HISTORY_LEN frames of per-
// stage timings plus the total frame time. Written on every _pt call; frame
// boundary bumps _perfRingIdx.
var PERF_HISTORY_LEN = 120;
var _perfRingIdx = 0;
var _perfStageHistory = {};    // name → Float32Array(HISTORY_LEN)
var _perfFrameTotals = new Float32Array(PERF_HISTORY_LEN);
