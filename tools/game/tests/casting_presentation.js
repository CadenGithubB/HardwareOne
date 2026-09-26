// Gameplay snapshots recorded before the hands/missile art pass. The snapshots
// exercise production combat, not a second implementation of projectile rules.
// The user subsequently authorized a 120 ms missile wind-up. For the old
// snapshots only, replay with a zero-duration queue and no first-frame marker
// verifies all other combat behavior. Separate checks exercise the real delay.
// Only new impact spellId presentation metadata is omitted from snapshots.
(function () {
  var G = (0, eval)('this'), checks = 0, clock = 1000, randomCalls = 0, rng = 1234567;
  var oldDate = Date.now, oldRandom = Math.random;
  var baseArg = __argv.indexOf('--baseline-dir');
  var sourceRoot = baseArg >= 0 ? __argv[baseArg + 1] + '/assets/games/src/' : 'assets/games/src/';
  function noop() {}
  function check(name, condition) {
    if (!condition) throw Error(name);
    checks++; __out('PASS ' + name);
  }
  function source(name) { return slurp(sourceRoot + name); }
  function evaluate(text) { (0, eval)(text); }
  var config = source('01-config-state.js');
  var spellDefinitions = config.slice(config.indexOf('var spells = {'), config.indexOf('var currentSpellIdx'));
  var generation = source('03-level-generation.js');
  evaluate(generation.slice(generation.indexOf('function getEffectiveCooldown()'), generation.indexOf('// Place treasure chests')));
  evaluate(source('10-combat.js'));
  var input = source('17-input-lighting-update.js');
  evaluate(input.slice(input.indexOf('function _tryCastSpell('), input.indexOf('// Pairwise enemy separation')));
  if (__argv.indexOf('--delay-projectile-spawn') >= 0) G.spawnProjectile = noop;
  var realCast = G.castCurrentSpell, realGamepadCast = G._tryCastSpell;
  G.console = {log: noop};
  Date.now = function () { return clock; };
  Math.random = function () {
    randomCalls++; rng = (Math.imul(rng, 1664525) + 1013904223) >>> 0;
    return rng / 4294967296;
  };
  G.getEntityRenderFloorZ = function (entity) { return entity.renderFloorZ || 0; };
  G.sampleEntitySupportRenderZ = function (x, y, z) { return z; };
  var relicHits = [], presentationEvents = [];
  G.noteFirstPersonCast = function (spell, now) {
    presentationEvents.push({id: spell.id, now: now, projectileCount: G.projectiles.length, mana: G.mana});
  };
  G.applyRelicOnHit = function (damage, entity, companion) { relicHits.push([damage, entity.x, entity.y, companion]); };
  function reset() {
    evaluate(spellDefinitions);
    clock = 1000; randomCalls = 0; rng = 1234567; relicHits = []; presentationEvents = [];
    G.running = true; G.gameOverState = false; G.menuOpen = false; G.DEBUG_COMBAT = false; G.DEBUG_EFFECTS = false;
    G.shopOpen = G.inventoryOpen = G.forgeOpen = G.settingsOpen = false;
    G.currentSpellIdx = 0; G.mana = 100; G.lastShotMs = 0; G.castAnimUntil = 0;
    G.pendingMissileCasts = []; G.spellCastReservationSerial = 0; G.MANA_MAX = 100;
    G.SHOOT_COOLDOWN = 300; G.PROJ_RADIUS = 3; G.PROJ_LIFE_MS = 1200;
    G.equipment = {robes: null, relic: null}; G._mouseHeld = false; G._attackHeld = false;
    G.MODE3D = true; G.CONTROL_MODE = 'keyboard'; G.MODE_STICK_AIM = 'stick'; G.MODE_GYRO_AIM = 'gyro';
    G.gpLast = {valid: false}; G.hasYaw = false; G.cam = {ang: 0, pitch: 0};
    G.pos = {x: 100, y: 100, floorZ: 60}; G.playerUnderground = false; G.floorMesh = null;
    G.projectiles = []; G.impacts = []; G.enemies = []; G.enemySpawners = []; G.oreVeins = [];
    G.groundEffects = []; G.chainEffects = []; G.deathEffects = []; G.coinDrops = [];
    G.coneEffects = []; G.novaEffects = []; G.dmgBoostUntil = 0;
    G.stats = {totalDamageDone: 0, totalDamageTaken: 0, totalManaConsumed: 0, totalEnemiesKilled: 0};
    G.cell = 20; G.gridW = 100; G.gridH = 100; G.grid = new Uint8Array(10000);
    G.lastADown = false; G.manaBlinkUntil = 0; G.flameStreamLastTick = 0;
  }
  function normalize(value) {
    if (typeof value === 'number') return Math.round(value * 1e8) / 1e8;
    if (Array.isArray(value)) return value.map(normalize);
    if (value && typeof value === 'object') {
      var result = {};
      Object.keys(value).sort().forEach(function (key) { result[key] = normalize(value[key]); });
      return result;
    }
    return value;
  }
  function snapshot() {
    var impactGameplay = G.impacts.map(function (impact) {
      var result = {};
      Object.keys(impact).forEach(function (key) { if (key !== 'spellId') result[key] = impact[key]; });
      return result;
    });
    return normalize({mana: G.mana, lastShotMs: G.lastShotMs, castAnimUntil: G.castAnimUntil,
      projectiles: G.projectiles, impacts: impactGameplay, enemies: G.enemies,
      spawners: G.enemySpawners, ores: G.oreVeins, stats: G.stats, relicHits: relicHits,
      deathEffects: G.deathEffects, coinDrops: G.coinDrops, randomCalls: randomCalls,
      manaBlinkUntil: G.manaBlinkUntil, groundEffects: G.groundEffects,
      chainEffects: G.chainEffects, novaEffects: G.novaEffects, coneEffects: G.coneEffects});
  }
  function hash(value) {
    var text = JSON.stringify(value), result = 2166136261;
    for (var i = 0; i < text.length; i++) result = Math.imul(result ^ text.charCodeAt(i), 16777619);
    return ('00000000' + (result >>> 0).toString(16)).slice(-8);
  }
  var scenarios = {
    'press cooldown and immediate spawn': function () {
      var frames = [];
      [1000, 1001, 1299, 1300].forEach(function (time) { clock = time; G.castCurrentSpell(); frames.push(snapshot()); });
      return frames;
    },
    'held mouse cooldown boundary': function () {
      G._mouseHeld = true; var frames = [];
      [1000, 1270, 1290, 1580, 1590, 1900].forEach(function (time) { clock = time; G.castCurrentSpell(); frames.push(snapshot()); });
      return frames;
    },
    'held keyboard cooldown boundary': function () { G._attackHeld = true; G.lastShotMs = 689; G.castCurrentSpell(); },
    'paused dead and menu casts rejected': function () {
      G.running = false; G.castCurrentSpell(); G.running = true;
      G.gameOverState = true; G.castCurrentSpell(); G.gameOverState = false;
      G.menuOpen = true; G.castCurrentSpell();
    },
    'insufficient mana and exact mana boundary': function () {
      G.mana = 9.99; G.castCurrentSpell(); var first = snapshot();
      G.mana = 10; G.castCurrentSpell(); return [first, snapshot()];
    },
    'robe mana discount exact gate': function () {
      G.equipment.robes = {manaCostReduction: 0.25, spellDmgBonus: 0.4};
      G.mana = 7.49; G.castCurrentSpell(); var first = snapshot();
      G.mana = 7.5; G.castCurrentSpell(); return [first, snapshot()];
    },
    'tier two split shot': function () { G.spells.missile.tier = 2; G.castCurrentSpell(); },
    'tier three split shot': function () { G.spells.missile.tier = 3; G.castCurrentSpell(); },
    'range relic speed and lifetime': function () {
      G.equipment.relic = {effect: 'spellRange', value: 0.35}; G.castCurrentSpell();
    },
    'cooldown relic and held input': function () {
      G.equipment.relic = {effect: 'cooldownReduction', value: 0.27}; G._mouseHeld = true;
      var frames = [];
      [1000, 1202, 1203, 1425].forEach(function (time) { clock = time; G.castCurrentSpell(); frames.push(snapshot()); });
      return frames;
    },
    'pitched underground zero stored height spawn': function () {
      G.pos.floorZ = 0; G.cam.pitch = 0.23; G.cam.ang = -0.41; G.playerUnderground = true; G.castCurrentSpell();
    },
    'two dimensional stick aim spawn': function () {
      G.MODE3D = false; G.gpLast = {valid: true, x: 0.7, y: 0.4}; G.castCurrentSpell();
    },
    'homing trajectory and vertical steering': function () {
      G.enemies = [{x: 380, y: 160, z: 12, health: 10}, {x: 370, y: 30, z: 18, health: 10}];
      G.castCurrentSpell(); var frames = [];
      [0.016, 0.03, 0.02, 0.04].forEach(function (dt) { clock += dt * 1000; G.updateProjectiles(dt); frames.push(snapshot()); });
      return frames;
    },
    'enemy hit location damage and knockback': function () {
      G.enemies = [{x: 120, y: 102, z: 0, health: 10}];
      G.equipment.robes = {spellDmgBonus: 0.4}; G.dmgBoostUntil = 1200;
      G.castCurrentSpell(); clock += 16; G.updateProjectiles(0.016);
    },
    'enemy below projectile missed': function () {
      G.enemies = [{x: 120, y: 102, z: 0, health: 10, renderFloorZ: -120}];
      G.castCurrentSpell(); clock += 16; G.updateProjectiles(0.016);
    },
    'wall hit location and lifetime': function () {
      G.grid[5 * G.gridW + 5] = 1; G.castCurrentSpell(); clock += 16; G.updateProjectiles(0.016);
    },
    'ore hit drop and random consumption': function () {
      G.grid[5 * G.gridW + 5] = 1; G.oreVeins = [{gx: 5, gy: 5, hp: 1}];
      G.castCurrentSpell(); clock += 16; G.updateProjectiles(0.016);
    },
    'spawner hit drop and random consumption': function () {
      G.enemySpawners = [{x: 120, y: 102, hp: 1, active: true}];
      G.castCurrentSpell(); clock += 16; G.updateProjectiles(0.016);
    },
    'projectile lifetime exact expiry': function () {
      G.castCurrentSpell(); clock = 2199; G.updateProjectiles(0); var first = snapshot();
      clock = 2200; G.updateProjectiles(0); return [first, snapshot()];
    },
    'gamepad preserves original successful cast rules': function () { G._tryCastSpell(clock); },
    'gamepad preserves original robe mana gate': function () {
      G.equipment.robes = {manaCostReduction: 0.25}; G.mana = 7.5; G._tryCastSpell(clock);
    },
    'gamepad preserves original tier two single shot': function () {
      G.spells.missile.tier = 2; G._tryCastSpell(clock);
    }
  };
  // Filled from the frozen pre-art source, not from the redesigned renderer.
  var expected = {
    'press cooldown and immediate spawn': '5c46b2bc',
    'held mouse cooldown boundary': '4473aece',
    'held keyboard cooldown boundary': '17adfb5e',
    'paused dead and menu casts rejected': 'fadde900',
    'insufficient mana and exact mana boundary': 'a5e79fe1',
    'robe mana discount exact gate': '7a776e5b',
    'tier two split shot': '7eca0bc1',
    'tier three split shot': '364af8c8',
    'range relic speed and lifetime': '8bfaddff',
    'cooldown relic and held input': '66e9a3cc',
    'pitched underground zero stored height spawn': '9b54bc4e',
    'two dimensional stick aim spawn': 'd6951e01',
    'homing trajectory and vertical steering': 'eef294b5',
    'enemy hit location damage and knockback': '53642c9a',
    'enemy below projectile missed': 'c0e5fa69',
    'wall hit location and lifetime': '04c066ae',
    'ore hit drop and random consumption': 'd2a24cf4',
    'spawner hit drop and random consumption': '4d467b1f',
    'projectile lifetime exact expiry': 'df1a6e33',
    'gamepad preserves original successful cast rules': 'f8a38d0a',
    'gamepad preserves original robe mana gate': '749b3b95',
    'gamepad preserves original tier two single shot': '4543a7f3'
  };
  var actual = {};
  try {
    var productionWindup = G.MISSILE_CAST_WINDUP_MS;
    if (typeof G.servicePendingMissileCasts === 'function') {
      G.MISSILE_CAST_WINDUP_MS = 0;
      function releaseForLegacyReplay() {
        G.servicePendingMissileCasts(clock);
        G.projectiles.forEach(function (p) { delete p._castReleaseAt; });
      }
      G.castCurrentSpell = function () { realCast(); releaseForLegacyReplay(); };
      G._tryCastSpell = function (now) { realGamepadCast(now); releaseForLegacyReplay(); };
    }
    Object.keys(scenarios).forEach(function (name) {
      reset(); actual[name] = hash(scenarios[name]() || snapshot());
    });
    if (__argv.indexOf('--record-baseline') >= 0) { __out(JSON.stringify(actual)); return; }
    G.MISSILE_CAST_WINDUP_MS = productionWindup;
    G.castCurrentSpell = realCast; G._tryCastSpell = realGamepadCast;
    if (__argv.indexOf('--world-art-only') < 0) {
    Object.keys(scenarios).forEach(function (name) { check(name + ' matches pre-art gameplay', actual[name] === expected[name]); });
    reset(); G.castCurrentSpell();
    check('successful press publishes one presentation event after reserving gameplay',
      presentationEvents.length === 1 && presentationEvents[0].id === 'missile' &&
      presentationEvents[0].now === 1000 && presentationEvents[0].projectileCount === 0 && presentationEvents[0].mana === 90);
    reset(); G.mana = 0; G.castCurrentSpell(); G._tryCastSpell(clock);
    G.mana = 100; G.lastShotMs = 999; G.castCurrentSpell(); G._tryCastSpell(clock);
    check('mana and cooldown rejected casts publish no presentation event', presentationEvents.length === 0);
    reset(); G._tryCastSpell(clock);
    check('successful gamepad cast publishes one presentation event without changing legacy animation timing',
      presentationEvents.length === 1 && presentationEvents[0].now === clock && G.castAnimUntil === 0);
    reset(); G.spells.missile.tier = 2; G.castCurrentSpell();
    check('split shot publishes one gesture for three reserved projectiles',
      presentationEvents.length === 1 && presentationEvents[0].projectileCount === 0 && G.pendingMissileCasts[0].count === 3);

    if (__argv.indexOf('--early-missile-release') >= 0) G.MISSILE_CAST_WINDUP_MS = 0;
    reset(); G.castCurrentSpell();
    check('accepted missile reserves one cost and cooldown without an early projectile',
      G.MISSILE_CAST_WINDUP_MS === 120 && G.projectiles.length === 0 && G.pendingMissileCasts.length === 1 &&
      G.pendingMissileCasts[0].releaseAt === 1120 && G.mana === 90 && G.stats.totalManaConsumed === 10 && G.lastShotMs === 1000);
    clock = 1119; G.updateProjectiles(0.016);
    check('missile cannot release before the wind-up boundary', G.projectiles.length === 0 && G.pendingMissileCasts.length === 1);
    clock = 1120; G.updateProjectiles(0.016);
    check('missile releases exactly once at the boundary without whole-frame motion',
      G.projectiles.length === 1 && G.pendingMissileCasts.length === 0 && G.projectiles[0].x === 103 &&
      G.projectiles[0].y === 102 && G.projectiles[0].spawnMs === 1120 && !('_castReleaseAt' in G.projectiles[0]));
    G.updateProjectiles(0);
    check('servicing the same release time cannot duplicate a shot or mana charge', G.projectiles.length === 1 && G.mana === 90 && G.stats.totalManaConsumed === 10);
    reset(); G._tryCastSpell(clock); clock = 1119; G.updateProjectiles(0.016);
    check('gamepad missile uses the same wind-up boundary', G.projectiles.length === 0 && G.pendingMissileCasts.length === 1);
    clock = 1125; G.updateProjectiles(0.016);
    check('gamepad release integrates only the post-release part of its first frame',
      G.projectiles.length === 1 && Math.abs(G.projectiles[0].x - 104.8) < 1e-9 && G.projectiles[0].spawnMs === 1120);
    reset(); G.castCurrentSpell(); G.pos = {x:200,y:210,floorZ:0}; G.cam.ang = Math.PI/2; G.cam.pitch = 0.2; G.playerUnderground = true;
    clock = 1120; G.servicePendingMissileCasts(clock);
    check('release samples current player position aim pitch and cave floor',
      G.projectiles.length === 1 && G.projectiles[0].x === 198 && G.projectiles[0].y === 213 &&
      G.projectiles[0].ang === Math.PI/2 && G.projectiles[0].z === 17.5 &&
      Math.abs(G.projectiles[0].vz + 360*Math.sin(0.2)) < 1e-9 && G.projectiles[0].underground);
    reset(); G.spells.missile.tier = 2; G.equipment.relic = {effect:'spellRange',value:0.35};
    G.castCurrentSpell(); G.spells.missile.tier = 1; G.spells.missile.speed = 99; G.equipment.relic = null;
    clock = 1120; G.servicePendingMissileCasts(clock);
    check('accepted split count speed lifetime and spell data survive equipment changes during wind-up',
      G.projectiles.length === 3 && G.projectiles.every(function (p) { return Math.abs(p.speed-486) < 1e-9 && p.lifeMs === 1620 && p.spell.tier === 2 && p.spell.speed === 360; }) &&
      G.projectiles[0].ang === -0.2 && G.projectiles[1].ang === 0 && G.projectiles[2].ang === 0.2);
    reset(); G.mana = 20; G.equipment.relic = {effect:'cooldownReduction',value:0.8};
    G.castCurrentSpell(); G._tryCastSpell(clock); clock = 1060; G._tryCastSpell(clock); clock = 1120; G.castCurrentSpell();
    check('mixed inputs cannot bypass reserved mana or cooldown while casts overlap',
      G.pendingMissileCasts.length === 2 && G.mana === 0 && G.stats.totalManaConsumed === 20);
    G.servicePendingMissileCasts(clock); clock = 1180; G.servicePendingMissileCasts(clock);
    check('overlapping accepted casts release once each', G.projectiles.length === 2 && G.pendingMissileCasts.length === 0);
    reset(); G.castCurrentSpell(); var refunded = G.cancelPendingMissileCasts(true);
    check('cancelling wind-up refunds its reserved cost stats and unsuperseded cooldown',
      refunded === 1 && G.pendingMissileCasts.length === 0 && G.mana === 100 && G.stats.totalManaConsumed === 0 && G.lastShotMs === 0);
    clock = 2000; G.updateProjectiles(0.016);
    check('cancelled casts cannot reappear on resume', G.projectiles.length === 0);
    reset(); G.castCurrentSpell(); G.running = false; clock = 1120; G.updateProjectiles(0.016);
    check('pause observed by the queue cancels and refunds instead of releasing', G.projectiles.length === 0 && G.pendingMissileCasts.length === 0 && G.mana === 100);
    reset(); G.castCurrentSpell(); G.menuOpen = true; clock = 1120; G.updateProjectiles(0.016);
    check('menu observed by the queue cancels and refunds instead of releasing', G.projectiles.length === 0 && G.pendingMissileCasts.length === 0 && G.mana === 100);
    reset(); G.castCurrentSpell(); G.grid = new Uint8Array(10000); G.mana = 70; clock = 1120; G.updateProjectiles(0.016);
    check('replaced world identity discards stale casts without refunding new state', G.projectiles.length === 0 && G.pendingMissileCasts.length === 0 && G.mana === 70);
    reset(); G.castCurrentSpell(); G.cancelPendingMissileCasts(false); G.mana = 75; G.stats = {totalManaConsumed:0};
    clock = 2000; G.updateProjectiles(0.016);
    check('explicit restart cancellation cannot spawn or credit the new player', G.projectiles.length === 0 && G.mana === 75 && G.stats.totalManaConsumed === 0);
    reset(); G.equipment.relic = {effect:'cooldownReduction',value:0.8}; G.castCurrentSpell(); clock = 1060;
    G.spells.ice.unlocked = true; G.currentSpellIdx = 1; G.castCurrentSpell(); G.cancelPendingMissileCasts(true);
    check('cancelling a missile never rewinds a later different spell cooldown or cost',
      G.projectiles.length === 1 && G.projectiles[0].spell.id === 'ice' && G.lastShotMs === 1060 && G.mana === 86 && G.stats.totalManaConsumed === 14);
    reset(); G.spells.ice.unlocked = true; G.currentSpellIdx = 1; G.castCurrentSpell();
    check('other projectile spells retain immediate spawn behavior', G.projectiles.length === 1 && G.pendingMissileCasts.length === 0 && G.projectiles[0].spawnMs === 1000);
    reset(); G.spells.missile.homing = 0; G.castCurrentSpell(); clock = 1120; G.updateProjectiles(0.016);
    clock = 2320; G.updateProjectiles(0);
    check('projectile lifetime begins at release rather than accepted input', G.projectiles.length === 0);
    reset(); G.grid[5*G.gridW+5] = 1; G.castCurrentSpell(); clock = 1120; G.updateProjectiles(0.016);
    check('released missile impacts carry appearance metadata at the real hit position',
      G.impacts.length === 1 && G.impacts[0].spellId === 'missile' && G.impacts[0].x === 103 && G.impacts[0].y === 102 && G.impacts[0].spawnMs === 1120);
    ['shopOpen','inventoryOpen','forgeOpen','settingsOpen','gameOverState'].forEach(function (flag) {
      reset(); G.castCurrentSpell(); G[flag] = true; clock = 1120; G.updateProjectiles(0.016);
      G.castCurrentSpell(); G._tryCastSpell(clock);
      check(flag+' prevents direct queued release and re-reservation', G.projectiles.length === 0 && G.pendingMissileCasts.length === 0 && G.mana === 100);
    });
    reset(); var cancellationEvents = 0;
    G.cancelFirstPersonCast = function () { cancellationEvents++; };
    G.castCurrentSpell(); G.cancelPendingMissileCasts(true); G.cancelPendingMissileCasts(true);
    check('cancellation resets presentation once and tolerates an empty queue', cancellationEvents === 1);
    delete G.cancelFirstPersonCast;
    reset(); G.mana = 20; G.equipment.relic = {effect:'cooldownReduction',value:0.8};
    G.castCurrentSpell(); clock = 1060; G.castCurrentSpell(); G.cancelPendingMissileCasts(true);
    check('cancelling overlapping reservations unwinds both costs and cooldowns in order',
      G.pendingMissileCasts.length === 0 && G.mana === 20 && G.stats.totalManaConsumed === 0 && G.lastShotMs === 0 && G.spellCastReservationSerial === 0);
    reset(); var originalDocument = G.document;
    G.document = {hidden:false}; G.castCurrentSpell(); G.document.hidden = true; clock = 1120;
    G.updateProjectiles(0.016); G.castCurrentSpell(); G._tryCastSpell(clock);
    check('hidden-page service cancels pending casts and rejects new reservations',
      G.projectiles.length === 0 && G.pendingMissileCasts.length === 0 && G.mana === 100);
    if (typeof originalDocument === 'undefined') delete G.document; else G.document = originalDocument;
    }

    // Production world-space art and production depth rasterizer. This host
    // records commands/masks; native-browser rasterization is checked separately.
    clock = 1000;
    var drawCount = 0, clipCount = 0, stack = [], rects = [], clipRuns = [], commands = [];
    var canvasState = ['globalAlpha','shadowBlur','lineWidth','fillStyle','strokeStyle','lineCap','lineJoin'];
    G.ctx = {globalAlpha: 0.37, shadowBlur: 5, lineWidth: 2,
      save: function () { var saved = {}; canvasState.forEach(function (key) { saved[key] = G.ctx[key]; }); stack.push(saved); },
      restore: function () { var saved = stack.pop(); if (!saved) throw Error('unbalanced Canvas restore');
        canvasState.forEach(function (key) { G.ctx[key] = saved[key]; }); },
      beginPath: function () { rects = []; commands.push(['begin']); },
      moveTo: function (x,y) { commands.push(['move',x,y]); },
      lineTo: function (x,y) { commands.push(['line',x,y]); },
      closePath: function () { commands.push(['close']); },
      rect: function (x,y,w,h) { rects.push({x:x,y:y,w:w,h:h}); },
      clip: function () { clipCount++; clipRuns.push(rects.slice()); },
      fill: function () { drawCount++; commands.push(['fill',G.ctx.fillStyle,G.ctx.globalAlpha]); },
      stroke: function () { drawCount++; commands.push(['stroke',G.ctx.strokeStyle,G.ctx.lineWidth,G.ctx.globalAlpha]); },
      createLinearGradient: function (x1,y1,x2,y2) {
        var grad = {stops: [], addColorStop: function (offset,color) { this.stops.push([offset,color]); }};
        commands.push(['gradient',x1,y1,x2,y2,grad.stops]); return grad;
      }
    };
    evaluate(source('12-scene-depth.js'));
    evaluate(source('14-render-entities.js'));
    evaluate(source('14-missile-art.js'));
    G.cam = {x: 0, y: 100, z: 60, ang: 0, pitch: 0}; G.projScale = 80; G.viewDist = 700;
    G.resScale = 1; G.getScale3D = function () { return 1; };
    var camera = {w:120,h:90,cosAng:1,sinAng:0,invTanHalf:1,horizonY:35,cameraZ:30};
    G.getCam3D = function () { return camera; };
    function wall(depth, top) {
      G.writeSceneDepthPolygon([{x:0,y:top||0,depth:depth},{x:120,y:top||0,depth:depth},
        {x:120,y:90,depth:depth},{x:0,y:90,depth:depth}]);
    }
    function resetPaint(hidden) {
      G.beginSceneDepthFrame(120,90); drawCount = clipCount = 0; clipRuns = []; commands = [];
      G.ctx.globalAlpha = 0.37; G.ctx.shadowBlur = 5; G.ctx.lineWidth = 2;
      if (hidden) wall(1);
    }
    var projectile = {x:100,y:108,z:20,ang:0.23,hz:360,vz:15,speed:360,spawnMs:900,
      lifeMs:1200,r:3,spell:{id:'missile',color:'#4db6ff'}};
    var impact = {x:100,y:100,z:0,spawnMs:950,lifeMs:220,spellId:'missile'};
    if (__argv.indexOf('--bypass-missile-depth') >= 0) G.withSceneDepthClip = function (points, draw) { draw(); return 1; };
    var noRandom = function () { throw Error('art must not consume gameplay random numbers'); };
    Math.random = noRandom;
    resetPaint(false); var projectileBefore = JSON.stringify(projectile);
    check('new missile art handles visible missile', G.renderMissileProjectileArt(projectile,camera,clock) === true && drawCount > 0);
    var visibleHash = hash(normalize(commands));
    check('world missile art does not mutate projectile or consume random numbers', JSON.stringify(projectile) === projectileBefore);
    check('world missile art restores inherited alpha blur width and Canvas stack',
      stack.length === 0 && G.ctx.globalAlpha === 0.37 && G.ctx.shadowBlur === 5 && G.ctx.lineWidth === 2);
    check('world missile art does not write transparent bounds as opaque depth', G.sceneDepthAt(60,50) === Infinity);
    resetPaint(false); G.renderMissileProjectileArt(projectile,camera,clock);
    check('world missile art is deterministic for the same pose and timestamp', hash(normalize(commands)) === visibleHash);
    var turn = 0.83, turnedCamera = {w:120,h:90,cosAng:Math.cos(turn),sinAng:Math.sin(turn),invTanHalf:1,horizonY:35,cameraZ:30};
    var turnedProjectile = Object.assign({},projectile,{
      x:100*Math.cos(turn)-8*Math.sin(turn),y:100+100*Math.sin(turn)+8*Math.cos(turn),ang:projectile.ang+turn});
    resetPaint(false); G.renderMissileProjectileArt(turnedProjectile,turnedCamera,clock);
    check('world velocity orientation remains equivalent when camera and scene rotate together', hash(normalize(commands)) === visibleHash);
    resetPaint(true); G.renderMissileProjectileArt(projectile,camera,clock);
    check('new missile cannot paint through opaque terrain', drawCount === 0);
    resetPaint(false); var impactBefore = JSON.stringify(impact);
    check('new impact art handles visible missile impact', G.renderMissileImpactArt(impact,camera,clock) === true && drawCount > 0);
    check('world impact art does not mutate impact or consume random numbers', JSON.stringify(impact) === impactBefore);
    check('world impact art restores inherited Canvas state', stack.length === 0 && G.ctx.globalAlpha === 0.37 && G.ctx.shadowBlur === 5);
    check('world impact art does not write opaque depth', G.sceneDepthAt(60,50) === Infinity);
    resetPaint(true); G.renderMissileImpactArt(impact,camera,clock);
    check('new missile impact cannot paint through opaque terrain', drawCount === 0);
    resetPaint(false); wall(5,50); G.renderMissileProjectileArt(projectile,camera,clock);
    check('partly buried new missile retains visible fragments through a pixel clip', drawCount > 0 && clipCount > 0);
    check('new missile clip excludes buried pixels from every partial mask',
      clipRuns.length > 0 && clipRuns.every(function (runs) { return runs.every(function (r) { return r.y < 50; }); }));
    resetPaint(false); wall(80);
    var projectedSegments = [], segmentClip = G.withSceneDepthClip;
    G.withSceneDepthClip = function (points,draw,options) {
      projectedSegments.push(points); return segmentClip(points,draw,options);
    };
    G.renderMissileProjectileArt(projectile,camera,clock);
    G.withSceneDepthClip = segmentClip;
    check('near wake survives terrain that hides the missile head', drawCount > 0 && clipCount > 0);
    check('wake segment retains independent endpoint depth', projectedSegments.some(function (points) {
      return points.length >= 4 && points[0].depth !== points[1].depth;
    }));
    resetPaint(false);
    var behind = {x:-3,y:103,z:29,ang:Math.PI,hz:360,vz:0,speed:360,spawnMs:900,spell:{id:'missile'}};
    G.renderMissileProjectileArt(behind,camera,clock);
    check('visible wake remains when projectile head crosses behind camera', drawCount > 0 && stack.length === 0);
    resetPaint(false);
    var originalDiamond = G.missileArtDiamond, diamondCenters = [];
    G.missileArtDiamond = function () { diamondCenters.push([arguments[0],arguments[1]]); return originalDiamond.apply(null,arguments); };
    G.renderMissileImpactArt(impact,camera,clock); G.missileArtDiamond = originalDiamond;
    check('impact preserves absolute zero render height without adding floor again', diamondCenters.length > 0 && diamondCenters[0][1] === 59);
    resetPaint(false); G.renderMissileImpactArt(impact,camera,impact.spawnMs+impact.lifeMs);
    check('new impact ends at its existing gameplay lifetime', drawCount === 0);
    resetPaint(false);
    check('other spells and companion impacts keep legacy artwork paths',
      G.renderMissileProjectileArt({spell:{id:'ice'}},camera,clock) === false &&
      G.renderMissileImpactArt({spellId:'ice'},camera,clock) === false &&
      G.renderMissileImpactArt({spellId:'missile',isCompanionProj:true},camera,clock) === false && drawCount === 0);
    resetPaint(false);
    G.renderMissileProjectileArt({x:601,y:100,z:0,ang:0,spell:{id:'missile'}},camera,clock);
    G.renderMissileProjectileArt({x:NaN,y:100,z:0,ang:0,spell:{id:'missile'}},camera,clock);
    check('invalid and beyond-range missiles remain culled', drawCount === 0 && stack.length === 0);
    resetPaint(false); var realFill = G.ctx.fill, threw = false;
    G.ctx.fill = function () { throw Error('intentional Canvas failure'); };
    try { G.renderMissileProjectileArt(projectile,camera,clock); } catch (e) { threw = e.message === 'intentional Canvas failure'; }
    G.ctx.fill = realFill;
    check('missile drawing restores caller state after a paint exception',
      threw && stack.length === 0 && G.ctx.globalAlpha === 0.37 && G.ctx.shadowBlur === 5 && G.ctx.lineWidth === 2);
  } finally { Date.now = oldDate; Math.random = oldRandom; }
  __out('CASTING_PRESENTATION_RESULT PASS ' + checks);
}());
undefined;
