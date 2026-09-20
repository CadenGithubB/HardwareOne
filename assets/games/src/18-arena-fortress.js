// =============================================
// ARENA CHALLENGE SYSTEM
// =============================================

function startArenaChallenge(altar) {
  if (arenaChallenge) return; // already in a challenge
  var key = altar.regionX + ',' + altar.regionY;
  if (completedArenas[key]) return;
  var sc = altar.scale || 1.0;
  var ringR = CHUNK_SIZE * 1.1 * sc;
  arenaChallenge = {
    centerX: altar.x,
    centerY: altar.y,
    regionX: altar.regionX,
    regionY: altar.regionY,
    ringRadius: ringR,
    wave: 0,
    maxWaves: 3,
    state: 'spawning', // 'spawning', 'fighting', 'waveDelay', 'complete'
    waveDelayUntil: 0,
    startMs: Date.now()
  };
  spawnArenaWave(arenaChallenge);
  pushToast('Arena Challenge — Wave 1/3', '#cc9944', 3000);
  console.log('[ARENA] Challenge started at region (' + altar.regionX + ',' + altar.regionY + ')');
}

function spawnArenaWave(ac) {
  ac.wave++;
  ac.state = 'fighting';
  // Wave composition: escalating difficulty
  var comp;
  if (ac.wave === 1) comp = [{type:'fast',n:3},{type:'normal',n:1}];        // 4
  else if (ac.wave === 2) comp = [{type:'fast',n:2},{type:'normal',n:3},{type:'tank',n:1}]; // 6
  else comp = [{type:'fast',n:3},{type:'normal',n:3},{type:'tank',n:2}];     // 8

  for (var ci = 0; ci < comp.length; ci++) {
    var entry = comp[ci];
    var eType = enemyTypes[entry.type];
    for (var ei = 0; ei < entry.n; ei++) {
      var angle = Math.random() * Math.PI * 2;
      var dist = 30 + Math.random() * (ac.ringRadius * 0.6);
      var ex = ac.centerX + Math.cos(angle) * dist;
      var ey = ac.centerY + Math.sin(angle) * dist;
      var wps = generatePatrolWaypoints(ex, ey, eType.chaseRange * 0.5);
      enemies.push({
        x: ex, y: ey, z: 0,
        enemyType: eType, health: eType.health, maxHealth: eType.health,
        speed: eType.speed, chaseRange: eType.chaseRange,
        lastUpdate: 0, damageFlash: 0, damageFlashColor: '#ffffff',
        slowUntil: 0, burnUntil: 0, burnDmgLast: 0,
        iceHits: 0, fireHits: 0, lightningHits: 0,
        vx: 0, vy: 0, aggroAt: 0,
        attackState: 'idle', attackStateUntil: 0,
        patrolWaypoints: wps, patrolIdx: 0, facing: 0,
        arenaEnemy: true
      });
    }
  }
  console.log('[ARENA] Wave ' + ac.wave + '/' + ac.maxWaves + ' spawned');
}

function updateArenaChallenge() {
  if (!arenaChallenge) return;
  var ac = arenaChallenge;

  // Check if player left the arena (abandon)
  var pdx = pos.x - ac.centerX, pdy = pos.y - ac.centerY;
  if (Math.hypot(pdx, pdy) > ac.ringRadius * 2.5) {
    // Despawn arena enemies
    var kept = [];
    for (var i = 0; i < enemies.length; i++) {
      if (!enemies[i].arenaEnemy) kept.push(enemies[i]);
    }
    enemies = kept;
    arenaChallenge = null;
    pushToast('Arena abandoned', '#887766', 2000);
    console.log('[ARENA] Challenge abandoned — player left arena');
    return;
  }

  if (ac.state === 'fighting') {
    // Count surviving arena enemies
    var alive = 0;
    for (var i = 0; i < enemies.length; i++) {
      if (enemies[i].arenaEnemy && enemies[i].health > 0) alive++;
    }
    if (alive === 0) {
      if (ac.wave >= ac.maxWaves) {
        // Challenge complete!
        ac.state = 'complete';
        var key = ac.regionX + ',' + ac.regionY;
        completedArenas[key] = true;
        // Drop Arcane Tome at arena center
        arcaneTomes.push({
          x: ac.centerX, y: ac.centerY,
          spawnMs: Date.now(), bob: Math.random() * Math.PI * 2
        });
        pushToast('Arena Cleared! Arcane Tome dropped!', '#cc88ff', 4000);
        console.log('[ARENA] Challenge complete! Arcane Tome dropped.');
        arenaChallenge = null;
      } else {
        // Wave cleared — brief delay then next wave
        ac.state = 'waveDelay';
        ac.waveDelayUntil = Date.now() + 2000;
        pushToast('Wave ' + ac.wave + ' cleared!', '#cc9944', 2000);
      }
    }
  } else if (ac.state === 'waveDelay') {
    if (Date.now() >= ac.waveDelayUntil) {
      spawnArenaWave(ac);
      pushToast('Arena Challenge — Wave ' + ac.wave + '/' + ac.maxWaves, '#cc9944', 3000);
    }
  }
}

// ══════════════════════════════════════════════════════════════
// ── Fortress Interactables: Forge, Garrison, Lectern ──
// ══════════════════════════════════════════════════════════════

function activateFortressInteract(interact) {
  var key = interact.structure.regionX + ',' + interact.structure.regionY;
  if (!completedFortresses[key]) completedFortresses[key] = {};
  if (interact.type === 'forge') activateFortressForge(interact);
  else if (interact.type === 'garrison') activateFortressGarrison(interact);
  else if (interact.type === 'lectern') activateFortressLectern(interact);
}

// ── Enchantment Forge ──
function activateFortressForge(interact) {
  // Check player has at least 2 equipped items
  var equipped = 0;
  for (var i = 0; i < EQUIP_SLOTS.length; i++) {
    if (equipment[EQUIP_SLOTS[i].key]) equipped++;
  }
  if (equipped < 2) {
    pushToast('Need at least 2 equipped items to use the forge', '#cc6633', 3000);
    return;
  }
  forgeOpen = true;
  forgeStep = 0;
  forgeSacrificeIdx = -1;
  forgeEnhanceIdx = -1;
  forgeStructure = interact;
  kbState = {up:false, down:false, left:false, right:false};
}

function forgeConfirm() {
  if (!forgeOpen) return;
  if (forgeStep === 0) {
    // Confirm sacrifice selection
    if (forgeSacrificeIdx >= 0 && equipment[EQUIP_SLOTS[forgeSacrificeIdx].key]) {
      forgeStep = 1;
      forgeEnhanceIdx = -1;
    }
  } else if (forgeStep === 1) {
    // Confirm enhance selection — execute the forge
    if (forgeEnhanceIdx >= 0 && forgeEnhanceIdx !== forgeSacrificeIdx && equipment[EQUIP_SLOTS[forgeEnhanceIdx].key]) {
      var sacrificeSlot = EQUIP_SLOTS[forgeSacrificeIdx].key;
      var enhanceSlot = EQUIP_SLOTS[forgeEnhanceIdx].key;
      var enhanceItem = equipment[enhanceSlot];
      // Destroy sacrifice
      equipment[sacrificeSlot] = null;
      // Boost quality by 0.15–0.25
      var boost = 0.15 + Math.random() * 0.10;
      var newQuality = (enhanceItem.quality || 1.0) + boost;
      equipment[enhanceSlot] = createEquipInstance(enhanceItem.id, newQuality);
      // Mark completed
      var key = forgeStructure.structure.regionX + ',' + forgeStructure.structure.regionY;
      if (!completedFortresses[key]) completedFortresses[key] = {};
      completedFortresses[key].forge = true;
      // Close forge
      forgeOpen = false;
      forgeStep = 0;
      forgeSacrificeIdx = -1;
      forgeEnhanceIdx = -1;
      goalMessage = equipment[enhanceSlot].displayName + ' enhanced!';
      goalMessageUntil = Date.now() + 3000;
    }
  }
}

function forgeSelectSlot(slotIdx) {
  if (!forgeOpen || slotIdx < 0 || slotIdx >= EQUIP_SLOTS.length) return;
  if (!equipment[EQUIP_SLOTS[slotIdx].key]) return; // can't select empty slot
  if (forgeStep === 0) {
    forgeSacrificeIdx = slotIdx;
  } else if (forgeStep === 1) {
    if (slotIdx !== forgeSacrificeIdx) {
      forgeEnhanceIdx = slotIdx;
    }
  }
}

function drawForgeOverlay() {
  if (!forgeOpen) return;
  var w = canvas.width, h = canvas.height;
  var S = resScale;
  ctx.save();
  // Dark backdrop
  ctx.globalAlpha = 0.7;
  ctx.fillStyle = '#000000';
  ctx.fillRect(0, 0, w, h);
  ctx.globalAlpha = 1.0;
  // Panel
  var panW = Math.min(280 * S, w * 0.8);
  var panH = Math.min(220 * S, h * 0.7);
  var px = Math.floor((w - panW) / 2), py = Math.floor((h - panH) / 2);
  ctx.fillStyle = 'rgba(15,10,25,0.95)';
  ctx.fillRect(px, py, panW, panH);
  ctx.strokeStyle = '#ff8800'; ctx.lineWidth = 2 * S;
  ctx.strokeRect(px, py, panW, panH);
  // Title
  ctx.fillStyle = '#ff8800';
  ctx.font = 'bold ' + Math.floor(14 * S) + 'px monospace';
  ctx.textAlign = 'center';
  ctx.fillText('Enchantment Forge', w / 2, py + 20 * S);
  // Instruction
  ctx.fillStyle = '#cccccc';
  ctx.font = Math.floor(10 * S) + 'px monospace';
  var instrText = forgeStep === 0 ? 'Select item to SACRIFICE (1-5, then E)' : 'Select item to ENHANCE (1-5, then E)';
  ctx.fillText(instrText, w / 2, py + 36 * S);
  // Equipment slots
  var slotSize = 24 * S, gap = 6 * S;
  var totalW = EQUIP_SLOTS.length * (slotSize + gap) - gap;
  var slotX0 = Math.floor((w - totalW) / 2);
  var slotY = py + 52 * S;
  for (var si = 0; si < EQUIP_SLOTS.length; si++) {
    var sd = EQUIP_SLOTS[si];
    var sx = slotX0 + si * (slotSize + gap);
    var eqItem = equipment[sd.key];
    // Background
    ctx.globalAlpha = eqItem ? 0.9 : 0.4;
    ctx.fillStyle = 'rgba(20,20,40,0.8)';
    ctx.fillRect(sx, slotY, slotSize, slotSize);
    // Highlight
    if (si === forgeSacrificeIdx) {
      ctx.strokeStyle = '#ff3333'; ctx.lineWidth = 2.5 * S;
      ctx.strokeRect(sx - 1, slotY - 1, slotSize + 2, slotSize + 2);
    } else if (si === forgeEnhanceIdx && forgeStep === 1) {
      ctx.strokeStyle = '#ffdd00'; ctx.lineWidth = 2.5 * S;
      ctx.strokeRect(sx - 1, slotY - 1, slotSize + 2, slotSize + 2);
    } else {
      ctx.strokeStyle = eqItem ? (RARITY_COLORS[eqItem.rarity] || '#888') : 'rgba(255,255,255,0.2)';
      ctx.lineWidth = 1 * S;
      ctx.strokeRect(sx, slotY, slotSize, slotSize);
    }
    // Icon
    ctx.globalAlpha = 1.0;
    if (eqItem) {
      ctx.fillStyle = '#fff'; ctx.font = 'bold ' + Math.floor(12 * S) + 'px Arial'; ctx.textAlign = 'center';
      ctx.fillText(sd.icon, sx + slotSize / 2, slotY + slotSize - 5 * S);
    }
    // Number label
    ctx.fillStyle = '#888'; ctx.font = Math.floor(8 * S) + 'px monospace'; ctx.textAlign = 'center';
    ctx.fillText('' + (si + 1), sx + slotSize / 2, slotY + slotSize + 10 * S);
  }
  // Item info
  var infoY = slotY + slotSize + 20 * S;
  ctx.textAlign = 'center';
  if (forgeSacrificeIdx >= 0) {
    var sacItem = equipment[EQUIP_SLOTS[forgeSacrificeIdx].key];
    if (sacItem) {
      ctx.fillStyle = '#ff3333'; ctx.font = 'bold ' + Math.floor(10 * S) + 'px monospace';
      ctx.fillText('SACRIFICE: ' + (sacItem.displayName || sacItem.name), w / 2, infoY);
      infoY += 14 * S;
    }
  }
  if (forgeStep === 1 && forgeEnhanceIdx >= 0) {
    var enhItem = equipment[EQUIP_SLOTS[forgeEnhanceIdx].key];
    if (enhItem) {
      ctx.fillStyle = '#ffdd00'; ctx.font = 'bold ' + Math.floor(10 * S) + 'px monospace';
      ctx.fillText('ENHANCE: ' + (enhItem.displayName || enhItem.name), w / 2, infoY);
      infoY += 14 * S;
      var curQ = Math.round((enhItem.quality || 1.0) * 100);
      ctx.fillStyle = '#aaaaaa'; ctx.font = Math.floor(9 * S) + 'px monospace';
      ctx.fillText('Quality ' + curQ + '% → ~' + (curQ + 20) + '%', w / 2, infoY);
    }
  }
  // ESC hint
  ctx.fillStyle = '#666'; ctx.font = Math.floor(9 * S) + 'px monospace'; ctx.textAlign = 'center';
  ctx.fillText('[ESC] Cancel', w / 2, py + panH - 8 * S);
  ctx.restore();
}

// ── Garrison Allies ──
function activateFortressGarrison(interact) {
  var key = interact.structure.regionX + ',' + interact.structure.regionY;
  if (!completedFortresses[key]) completedFortresses[key] = {};
  completedFortresses[key].garrison = true;
  var _allyNames = ['Aldric', 'Sera', 'Borin', 'Mira', 'Thane', 'Lysa', 'Edric', 'Vorn'];
  // Spawn 2 soldier allies near the interact point
  for (var gi = 0; gi < 2; gi++) {
    var ang = gi * Math.PI + Math.random() * 0.5;
    var _aName = _allyNames[Math.floor(Math.random() * _allyNames.length)];
    fortressAllies.push({
      x: interact.x + Math.cos(ang) * 20,
      y: interact.y + Math.sin(ang) * 20,
      z: 0,
      health: 6, maxHealth: 6,
      speed: 42,
      attackRange: 28,
      attackDamage: 12,
      attackCooldown: 1000,
      lastAttackMs: 0,
      vx: 0, vy: 0,
      followDist: 90, followIdeal: 55, teleportDist: 400,
      wanderAng: Math.random() * Math.PI * 2,
      phase: Math.random() * Math.PI * 2,
      damageFlash: 0,
      targetEnemy: null,
      name: _aName
    });
    pushToast(_aName + ' has joined your party', '#aaaaff', 3000);
  }
}

function updateFortressAllies(dt) {
  for (var ai = fortressAllies.length - 1; ai >= 0; ai--) {
    var ally = fortressAllies[ai];
    // Death check
    if (ally.health <= 0) {
      // Death effect — small explosion of particles
      for (var pi = 0; pi < 5; pi++) {
        impacts.push({x: ally.x, y: ally.y, t: Date.now(), dur: 400, r: 8, color: '#c8a028', vx: (Math.random()-0.5)*60, vy: (Math.random()-0.5)*60});
      }
      var _dName = ally.name || 'Ally';
      pushToast(_dName + ' has fallen', '#cc4444', 4000);
      fortressAllies.splice(ai, 1);
      continue;
    }
    // Damage flash decay
    if (ally.damageFlash > 0) ally.damageFlash = Math.max(0, ally.damageFlash - dt * 4);
    // Teleport if too far from player
    var dx = pos.x - ally.x, dy = pos.y - ally.y;
    var dist = Math.hypot(dx, dy);
    if (dist > ally.teleportDist) {
      ally.x = pos.x + (Math.random() - 0.5) * 40;
      ally.y = pos.y + (Math.random() - 0.5) * 40;
      continue;
    }
    // Attack: find nearest enemy within attack range
    var now = Date.now();
    var bestE = null, bestD = ally.attackRange;
    if (now - ally.lastAttackMs >= ally.attackCooldown) {
      for (var ei = 0; ei < enemies.length; ei++) {
        var en = enemies[ei];
        if (en.health <= 0) continue;
        var ed = Math.hypot(en.x - ally.x, en.y - ally.y);
        if (ed < bestD) { bestD = ed; bestE = en; }
      }
      if (bestE) {
        // Melee hit
        bestE.health -= ally.attackDamage;
        bestE.damageFlash = 1.0;
        bestE.damageFlashColor = '#ffcc44';
        // Knockback
        var kbDist = Math.hypot(bestE.x - ally.x, bestE.y - ally.y) || 1;
        bestE.vx += (bestE.x - ally.x) / kbDist * 80;
        bestE.vy += (bestE.y - ally.y) / kbDist * 80;
        ally.lastAttackMs = now;
        ally.targetEnemy = bestE;
      }
    }
    // Move: chase nearby enemy or follow player
    var moveX = 0, moveY = 0;
    // Check for nearby enemy to chase
    var chaseE = null, chaseD = 120;
    for (var ei2 = 0; ei2 < enemies.length; ei2++) {
      var en2 = enemies[ei2];
      if (en2.health <= 0) continue;
      var ed2 = Math.hypot(en2.x - ally.x, en2.y - ally.y);
      if (ed2 < chaseD) { chaseD = ed2; chaseE = en2; }
    }
    if (chaseE && chaseD > ally.attackRange * 0.5) {
      // Chase enemy
      var cdx = chaseE.x - ally.x, cdy = chaseE.y - ally.y;
      var cd = Math.hypot(cdx, cdy) || 1;
      moveX = cdx / cd * ally.speed;
      moveY = cdy / cd * ally.speed;
    } else if (dist > ally.followIdeal) {
      // Follow player — orbit approach like companions
      var speed = ally.speed * (dist > ally.followDist * 1.5 ? 1.5 : 1.0);
      ally.wanderAng += (Math.random() - 0.5) * 2 * dt;
      var targetX = pos.x + Math.cos(ally.wanderAng) * ally.followIdeal * 0.7;
      var targetY = pos.y + Math.sin(ally.wanderAng) * ally.followIdeal * 0.7;
      var tdx = targetX - ally.x, tdy = targetY - ally.y;
      var td = Math.hypot(tdx, tdy) || 1;
      moveX = tdx / td * speed;
      moveY = tdy / td * speed;
    }
    // Apply movement with wall collision
    if (moveX !== 0 || moveY !== 0) {
      var newX = ally.x + moveX * dt;
      var newY = ally.y + moveY * dt;
      var gx1 = Math.floor(newX / cell), gy1 = Math.floor(ally.y / cell);
      var gx2 = Math.floor(ally.x / cell), gy2 = Math.floor(newY / cell);
      if (gx1 >= 0 && gx1 < gridW && gy1 >= 0 && gy1 < gridH && !grid[gy1 * gridW + gx1]) ally.x = newX;
      if (gx2 >= 0 && gx2 < gridW && gy2 >= 0 && gy2 < gridH && !grid[gy2 * gridW + gx2]) ally.y = newY;
    }
    // Walking phase
    ally.phase += dt * 6;
  }
}

// ── Map Reveal Lectern ──
function activateFortressLectern(interact) {
  var key = interact.structure.regionX + ',' + interact.structure.regionY;
  if (!completedFortresses[key]) completedFortresses[key] = {};
  completedFortresses[key].lectern = true;
  // Convert fortress world center to grid coords
  var fwx = interact.structure.centerWX || interact.x;
  var fwy = interact.structure.centerWY || interact.y;
  // For endless mode, convert world coords to window-local then to grid
  var localX = ENDLESS_MODE ? (fwx - windowOriginX) : fwx;
  var localY = ENDLESS_MODE ? (fwy - windowOriginY) : fwy;
  var centerGX = Math.floor(localX / cell);
  var centerGY = Math.floor(localY / cell);
  mapRevealAnim = {
    startMs: Date.now(),
    centerGX: centerGX,
    centerGY: centerGY,
    revealRadius: 90,
    duration: 3000,
    prevMode: minimapMode
  };
  minimapMode = 1; // force large map
  pushToast('Ancient knowledge reveals the land...', '#88bbff', 3000);
}

function updateMapRevealAnim() {
  if (!mapRevealAnim || !exploredCells) return;
  var elapsed = Date.now() - mapRevealAnim.startMs;
  if (elapsed >= mapRevealAnim.duration) {
    // Animation complete — restore minimap mode
    minimapMode = mapRevealAnim.prevMode;
    mapRevealAnim = null;
    return;
  }
  // Phase 2 (500–2500ms): progressive fog reveal
  if (elapsed > 400 && elapsed < mapRevealAnim.duration - 400) {
    var revealT = (elapsed - 400) / (mapRevealAnim.duration - 800);
    var currentR = Math.floor(revealT * mapRevealAnim.revealRadius);
    var cgx = mapRevealAnim.centerGX, cgy = mapRevealAnim.centerGY;
    var changed = false;
    for (var dy = -currentR; dy <= currentR; dy++) {
      for (var dx = -currentR; dx <= currentR; dx++) {
        if (dx * dx + dy * dy > currentR * currentR) continue;
        var gx = cgx + dx, gy = cgy + dy;
        if (gx < 0 || gy < 0 || gx >= gridW || gy >= gridH) continue;
        var idx = gy * gridW + gx;
        if (!exploredCells[idx]) { exploredCells[idx] = 1; changed = true; }
      }
    }
    if (changed) minimapDirty = true;
  }
}

function upgradeSpellToTier2(spellId) {
  var sp = spells[spellId];
  if (!sp || sp.tier >= 2) return false;
  sp.tier = 2;
  if (spellId === 'fire') {
    sp.streamRange = 180; sp.streamWidth = 0.55; sp.damage = sp.damage * 1.3;
  } else if (spellId === 'arcane') {
    sp.novaRadius = 160; sp.stunDuration = 1600;
  } else if (spellId === 'poison') {
    sp.cloudRadius = 75; sp.cloudDuration = 5000;
  } else if (spellId === 'lightning') {
    sp.speed = 300;
  }
  return true;
}

function applyArcaneTome() {
  // Try to upgrade active spell first
  var unlocked = getUnlockedSpells();
  var activeId = unlocked[currentSpellIdx] || 'missile';
  if (spells[activeId].tier < 2) {
    upgradeSpellToTier2(activeId);
    goalMessage = spells[activeId].name + ' upgraded to Tier 2!';
    goalMessageUntil = Date.now() + 4000;
    console.log('[TOME] Upgraded active spell: ' + activeId + ' → Tier 2');
    return;
  }
  // Else find first unlocked spell that's still tier 1
  for (var i = 0; i < spellOrder.length; i++) {
    var sid = spellOrder[i];
    if (spells[sid].unlocked && spells[sid].tier < 2) {
      upgradeSpellToTier2(sid);
      goalMessage = spells[sid].name + ' upgraded to Tier 2!';
      goalMessageUntil = Date.now() + 4000;
      console.log('[TOME] Upgraded spell: ' + sid + ' → Tier 2');
      return;
    }
  }
  // All spells maxed — 50 coins fallback
  coins += 50;
  pushToast('All spells maxed! +50 coins', '#d4a820', 3000);
  console.log('[TOME] All spells tier 2, awarded 50 coins');
}

function updateArcaneTomes() {
  arcaneTomes = updateCollectibles(arcaneTomes,
    {collectRadius: 30, vacuumRadius: 100, vacuumPull: 4.0},
    function(tome) {
      applyArcaneTome();
      for (var pi = 0; pi < 15; pi++) {
        var ang = (pi / 15) * Math.PI * 2;
        impacts.push({x: tome.x, y: tome.y, z: getEntityRenderFloorZ(tome) + 10,
          vx: Math.cos(ang) * 50, vy: Math.sin(ang) * 50, vz: 20 + Math.random() * 30,
          spawnMs: Date.now(), lifeMs: 1000, color: '#bb66ff', size: 5});
      }
    });
}

function updateStatPickups() {
  statPickups = updateCollectibles(statPickups,
    {collectRadius: 25, vacuumRadius: 100, vacuumPull: 3.5},
    function(sp) {
      var spKey = Math.floor(sp.wx) + ',' + Math.floor(sp.wy);
      collectedStatPickups[spKey] = true;
      var pColor, msg;
      if (sp.type === 'heartCrystal') {
        HEALTH_MAX += 10; health = Math.min(health + 10, HEALTH_MAX);
        permanentHealthBonus += 10;
        pColor = '#ff4444'; msg = 'Heart Crystal! Max HP +10 (now ' + HEALTH_MAX + ')';
      } else if (sp.type === 'manaStar') {
        MANA_MAX += 10; mana = Math.min(mana + 10, MANA_MAX);
        permanentManaBonus += 10;
        pColor = '#4488ff'; msg = 'Mana Star! Max Mana +10 (now ' + MANA_MAX + ')';
      } else {
        permanentSpeedBonus += 0.05;
        pColor = '#44dd55'; msg = 'Movement Tome! Speed +5%';
      }
      goalMessage = msg;
      goalMessageUntil = Date.now() + 4000;
      for (var pi = 0; pi < 12; pi++) {
        var ang = (pi / 12) * Math.PI * 2;
        impacts.push({x: sp.x, y: sp.y, z: getEntityRenderFloorZ(sp) + 8,
          vx: Math.cos(ang) * 45, vy: Math.sin(ang) * 45, vz: 15 + Math.random() * 25,
          spawnMs: Date.now(), lifeMs: 800, color: pColor, size: 4});
      }
      console.log('[STAT] Collected ' + sp.type + ' at (' + sp.wx.toFixed(0) + ',' + sp.wy.toFixed(0) + ')');
    });
}
