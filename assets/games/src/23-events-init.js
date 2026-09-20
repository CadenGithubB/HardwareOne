// =============================================
// SECTION 19: EVENT LISTENERS & INIT
// =============================================

document.getElementById('btnStart').addEventListener('click', startGame);
document.getElementById('btnStop').addEventListener('click', stopGame);
document.getElementById('chkImuDebug').addEventListener('change', function() { DEBUG_IMU = this.checked; });
document.getElementById('btnFwDbgOn').addEventListener('click', fwDebugOn);
document.getElementById('btnFwDbgOff').addEventListener('click', fwDebugOff);

(function() {
  var sel = document.getElementById('terrainSelect');
  if (sel) {
    sel.value = terrain;
    sel.addEventListener('change', function() {
      var ctOpts = document.getElementById('caveTestOptions');
      if (this.value === 'cavetest') {
        // Show cave test options panel
        if (ctOpts) ctOpts.style.display = '';
        var kindSelect = document.getElementById('ctEntranceKind');
        window._caveTestKindOverride = kindSelect && kindSelect.value === 'hillside' ? 'hillside' : 'descending';
        // Two reproducible natural sites, not fabricated test-only landforms.
        window._caveTestSeedOverride = window._caveTestKindOverride === 'hillside' ? 5668 : 12345;
        // Read flags from checkboxes
        caveTestFlags.surfaceEnemies = document.getElementById('ctSurfaceEnemies').checked;
        caveTestFlags.structures = document.getElementById('ctStructures').checked;
        caveTestFlags.markets = document.getElementById('ctMarkets').checked;
        caveTestFlags.shrines = document.getElementById('ctShrines').checked;
        caveTestFlags.spawners = document.getElementById('ctSpawners').checked;
        caveTestFlags.surfaceChests = document.getElementById('ctSurfaceChests').checked;
        caveTestFlags.caveEnemies = document.getElementById('ctCaveEnemies').checked;
        caveTestFlags.caveChests = document.getElementById('ctCaveChests').checked;
        // Cave test: launch endless mode with forced cave at center
        CAVE_TEST_MODE = true;
        ENDLESS_MODE = true;
        gameOverState = false;
        running = true;
        equipment = {armor: null, hat: null, robes: null};
        inventoryOpen = false; inventorySelIdx = -1;
        health = HEALTH_MAX; mana = MANA_MAX;
        terrain = 'plains';
        resetEndlessMode();
        MODE3D = true;
        hasBaseline = true;
        calibrating = false;
        lastUpdate = 0;
        CONTROL_MODE = MODE_STICK_AIM;
        USE_KEYBOARD = true; USE_MOUSE = true;
        USE_GAMEPAD = false; stopGamepadPolling();
        var gamepadInput = document.getElementById('chkGamepad');
        if (gamepadInput) gamepadInput.checked = false;
        var keyInput = document.getElementById('chkKeyboard');
        if (keyInput) keyInput.checked = true;
        previewCaveView('approach');
        _loopGen++;
        if (running) _scheduleLoop();
        updateHudInput();
        return;
      }
      if (ctOpts) ctOpts.style.display = 'none';
      var fixtures = document.getElementById('ctVisibilityFixtures');
      if (fixtures) fixtures.checked = false;
      setCavePreviewNoon(false);
      CAVE_TEST_MODE = false;
      terrain = this.value;
      applyPreset();
    });
  }
})();

// Explicit debug viewpoints make the two entrance profiles easy to compare.
// They only reposition the Cave Test player; normal Endless play is untouched.
var cavePreviewSavedDaySpeed = null;
var cavePreviewLastView = 'approach';
function caveVisibilityFixturesEnabled() {
  var checkbox = document.getElementById('ctVisibilityFixtures');
  return !!(CAVE_TEST_MODE && checkbox && checkbox.checked);
}
function drawCaveVisibilityFixtures() {
  if (!caveVisibilityFixturesEnabled()) { draw(); return; }
  // Freeze simulation and invalidate callbacks that were already queued.
  // The fixture is only a draw-time substitution, never chunk/gameplay data.
  running = false; _loopGen++;
  stopGamepadPolling();
  _mouseHeld = false; _attackHeld = false; flameStreamActive = false;
  if (document.pointerLockElement === canvas) document.exitPointerLock();
  var net = endlessCaveNetworks['0,0'];
  if (!net || !net.entrances.length) { draw(); return; }
  var entrance = net.entrances[0];
  function point(inward, cross) {
    var x = entrance.x + Math.cos(entrance.angle) * inward - Math.sin(entrance.angle) * cross - windowOriginX;
    var y = entrance.y + Math.sin(entrance.angle) * inward + Math.cos(entrance.angle) * cross - windowOriginY;
    return {x:x, y:y, z:getEntityGroundRenderZ(x,y,true)};
  }
  var ep = point(170,-18), cp = point(142,18), pp = point(150,0);
  var frozenType = Object.assign({}, enemyTypes.normal, {speed:0,chaseRange:0});
  var savedEnemies = enemies, savedChests = treasureChests, savedProjectiles = projectiles;
  try {
    enemies = [{x:ep.x,y:ep.y,z:ep.z,renderFloorZ:ep.z,underground:true,
      enemyType:frozenType,health:2,maxHealth:4,speed:0,chaseRange:0,
      facing:entrance.angle+Math.PI,patrolWaypoints:[],attackState:'idle'}];
    treasureChests = [{x:cp.x,y:cp.y,renderFloorZ:cp.z,underground:true,
      collected:false,opened:false,lidAngle:0,tier:'rare',gold:0,facing:entrance.angle+Math.PI,seed:1}];
    projectiles = [{x:pp.x,y:pp.y,z:pp.z+30,renderFloorZ:pp.z,underground:true,
      spell:spells.fire,ang:entrance.angle,vx:0,vy:0,vz:0}];
    renderFrame();
  } finally {
    enemies = savedEnemies; treasureChests = savedChests; projectiles = savedProjectiles;
  }
}
function setCavePreviewNoon(enabled) {
  if (enabled) {
    if (cavePreviewSavedDaySpeed === null) cavePreviewSavedDaySpeed = daySpeed;
    daySpeed = 0;
    dayTime = 0.5;
  } else if (cavePreviewSavedDaySpeed !== null) {
    daySpeed = cavePreviewSavedDaySpeed;
    cavePreviewSavedDaySpeed = null;
  }
}
function previewCaveView(view) {
  if (!CAVE_TEST_MODE) return;
  cavePreviewLastView = view;
  var net = endlessCaveNetworks['0,0'];
  if (!net || !net.entrances.length || !net.chambers.length) return;
  var e = net.entrances[0], chamber = net.chambers[net.chambers.length - 1];
  var along = view === 'approach' ? (e.approachLength || 480) * 0.8 : view === 'mouth' ? 24 : -90;
  var cross = 0;
  // These deliberately include terrain between the camera and lower jambs.
  // The old top-point-only entrance visibility check missed those buried parts.
  if (view === 'sideleft' || view === 'sideright') {
    along = 300; cross = view === 'sideleft' ? 160 : -160;
  } else if (view === 'oblique') {
    along = 190; cross = 120;
  } else if (view === 'roofedge') {
    along = -72; cross = (e.halfWidth || 60) * 1.35;
  }
  var wx = e.x - Math.cos(e.angle) * along - Math.sin(e.angle) * cross;
  var wy = e.y - Math.sin(e.angle) * along + Math.cos(e.angle) * cross;
  if (view === 'chamber' || view === 'above') { wx = chamber.cx; wy = chamber.cy; }
  pos.x = wx - windowOriginX; pos.y = wy - windowOriginY;
  updateChunks();
  var gx = Math.floor(pos.x / floorMesh.gridSize), gy = Math.floor(pos.y / floorMesh.gridSize);
  var h = e.floorH;
  if (view === 'approach' || view === 'above' || view === 'sideleft' || view === 'sideright' ||
      view === 'oblique' || view === 'roofedge') {
    var li = getTopWalkableLayerIdx(gx, gy);
    if (li >= 0) h = floorMesh['l' + li + 'TopZ'][gy * floorMesh.w + gx];
  } else {
    var support = getWalkableLayerTopAt(pos.x, pos.y, Number.isFinite(h) ? h : -5);
    if (Number.isFinite(support.topH)) h = support.topH;
  }
  if (!Number.isFinite(h)) h = getFloorHeightAt(pos.x, pos.y);
  pos.floorZ = meshHeightToPlayerZ(h);
  jumpAirborne = false; jumpVelZ = 0; onWallTop = false;
  vel.x = 0; vel.y = 0;
  kbState.up = kbState.down = kbState.left = kbState.right = false;
  cam.x = pos.x; cam.y = pos.y; cam.z = pos.floorZ; cam.pitch = 0;
  cam.ang = view === 'back' || view === 'chamber' || cross !== 0 ? Math.atan2(e.y - wy, e.x - wx) : e.angle;
  if (view === 'sideleft' || view === 'sideright' || view === 'oblique') cam.pitch = 0.18;
  if (view === 'roofedge') cam.pitch = 0.25;
  overviewActive = false; MODE3D = true; CAM_FOLLOW = true;
  var fixedNoon = document.getElementById('ctFixedNoon');
  setCavePreviewNoon(!!(fixedNoon && fixedNoon.checked));
  updateDayNight(0); updateLightGrid(); drawCaveVisibilityFixtures();
  var status = document.getElementById('ctPreviewStatus');
  if (status) status.textContent = (e.kind || 'cave') + ' · seed ' + WORLD_SEED + ' · ' + view +
    ' · floor ' + h.toFixed(2) + ' · ' + (playerUnderground ? 'underground' : 'outdoors') +
    (caveVisibilityFixturesEnabled() ? ' · PAUSED fixtures: skeleton, chest, fire orb' : '');
}
(function() {
  var selector = document.getElementById('ctEntranceKind');
  if (selector) selector.addEventListener('change', function() {
    if (CAVE_TEST_MODE) document.getElementById('terrainSelect').dispatchEvent(new Event('change'));
  });
  ['Approach', 'Mouth', 'Inside', 'Back', 'Chamber', 'Above', 'SideLeft', 'SideRight', 'Oblique', 'RoofEdge'].forEach(function(name) {
    var button = document.getElementById('ctView' + name);
    if (button) button.addEventListener('click', function() { previewCaveView(name.toLowerCase()); });
  });
  var fixedNoon = document.getElementById('ctFixedNoon');
  if (fixedNoon) fixedNoon.addEventListener('change', function() {
    if (!CAVE_TEST_MODE) return;
    setCavePreviewNoon(this.checked);
    updateDayNight(0); updateLightGrid(); drawCaveVisibilityFixtures();
  });
  var fixtures = document.getElementById('ctVisibilityFixtures');
  if (fixtures) fixtures.addEventListener('change', function() {
    if (!CAVE_TEST_MODE) return;
    if (this.checked) previewCaveView(cavePreviewLastView);
    else document.getElementById('terrainSelect').dispatchEvent(new Event('change'));
  });
})();

document.getElementById('btnView2D').addEventListener('click', function() { MODE3D = false; if (overviewActive) toggleOverview(); });
document.getElementById('btnView3D').addEventListener('click', function() { MODE3D = true; if (overviewActive) toggleOverview(); });
document.getElementById('btnToggleTex').addEventListener('click', function() {
  USE_TEXTURES = !USE_TEXTURES;
  this.textContent = USE_TEXTURES ? 'Disable Textures' : 'Enable Textures';
});
document.getElementById('btnOverview').addEventListener('click', function() { toggleOverview(); });
document.getElementById('btnEndless').addEventListener('click', function() {
  ENDLESS_MODE = true;
  var fixtures = document.getElementById('ctVisibilityFixtures');
  if (fixtures) fixtures.checked = false;
  setCavePreviewNoon(false);
  CAVE_TEST_MODE = false;  // Normal endless mode
  gameOverState = false;
  running = true;
  equipment = {armor: null, hat: null, robes: null};
  inventoryOpen = false; inventorySelIdx = -1;
  health = HEALTH_MAX; mana = MANA_MAX;
  resetEndlessMode();
  MODE3D = true;
  hasBaseline = true;
  calibrating = false;
  lastUpdate = 0;
  CONTROL_MODE = MODE_STICK_AIM;
  USE_KEYBOARD = true; USE_MOUSE = true;
  var chk = document.getElementById('chkKeyboard');
  if (chk) chk.checked = true;
  draw();
  _loopGen++; _scheduleLoop();
  var chk2 = document.getElementById('chkCamFollow');
  if (chk2 && chk2.checked) CAM_FOLLOW = true;
  updateHudInput();
  console.log('[ENDLESS] Endless mode launched');
});

// Cave test flag checkboxes — toggling any flag restarts cave test mode
(function() {
  var flagMap = {
    'ctSurfaceEnemies': 'surfaceEnemies',
    'ctStructures': 'structures',
    'ctMarkets': 'markets',
    'ctShrines': 'shrines',
    'ctSpawners': 'spawners',
    'ctSurfaceChests': 'surfaceChests',
    'ctCaveEnemies': 'caveEnemies',
    'ctCaveChests': 'caveChests'
  };
  Object.keys(flagMap).forEach(function(chkId) {
    var el = document.getElementById(chkId);
    if (el) {
      el.addEventListener('change', function() {
        caveTestFlags[flagMap[chkId]] = this.checked;
        // Auto-restart cave test if currently active
        if (CAVE_TEST_MODE) {
          resetEndlessMode();
          if (caveVisibilityFixturesEnabled()) previewCaveView(cavePreviewLastView);
          console.log('[CAVE TEST] Restarted with flag ' + flagMap[chkId] + '=' + this.checked);
        }
      });
    }
  });
})();

canvas.addEventListener('click', function(e) {
  if (overviewActive) { toggleOverview(); return; }
  // Settings overlay click handling — when open, route clicks to settings UI
  if (settingsOpen) {
    var rect = canvas.getBoundingClientRect();
    var mx = (e.clientX - rect.left) * (canvas.width / rect.width);
    var my = (e.clientY - rect.top) * (canvas.height / rect.height);
    handleSettingsClick(mx, my);
    return;
  }
  // Pointer lock: clicking the canvas while the game is running requests lock
  // and auto-enables keyboard+mouse mode (browsers require a direct user gesture).
  if (running && document.pointerLockElement !== canvas) {
    USE_KEYBOARD = true; USE_MOUSE = true;
    USE_GAMEPAD = false;
    var cg = document.getElementById('chkGamepad'); if (cg) cg.checked = false;
    var ck = document.getElementById('chkKeyboard'); if (ck) ck.checked = true;
    stopGamepadPolling();
    kbState = {up:false, down:false, left:false, right:false};
    mouseAccum = {x:0, y:0}; mouseDelta = {x:0, y:0};
    updateHudInput();
    canvas.requestPointerLock();
  }
});
document.getElementById('chk3dDebug').addEventListener('change', function() { DEBUG_3D = this.checked; });
document.getElementById('chkTexDebug').addEventListener('change', function() { DEBUG_TEXTURE = this.checked; });
document.getElementById('chkFloorDebug').addEventListener('change', function() { DEBUG_FLOOR = this.checked; });
document.getElementById('chkFloorLineDebug').addEventListener('change', function() { DEBUG_FLOOR_LINE = this.checked; });
document.getElementById('chkDecorDebug').addEventListener('change', function() { DEBUG_DECORATIONS = this.checked; });
document.getElementById('chkSkelDebug').addEventListener('change', function() { DEBUG_SKELETON = this.checked; });
document.getElementById('chkCombatDebug').addEventListener('change', function() { DEBUG_COMBAT = this.checked; });
document.getElementById('chkCamFollow').addEventListener('change', function() { CAM_FOLLOW = this.checked; });
document.getElementById('chkNoclip').addEventListener('change', function() { NOCLIP = this.checked; });
document.getElementById('chkPerfHud').addEventListener('change', function() {
  DEBUG_PERF_HUD = this.checked;
  var p = document.getElementById('perfHudPanel');
  if (p) p.style.display = this.checked ? 'block' : 'none';
});
document.getElementById('chkCaveDbg').addEventListener('change', function() {
  DEBUG_CAVE = this.checked;
  var panel = document.getElementById('caveDebugPanel');
  if (panel) panel.style.display = this.checked ? '' : 'none';
  if (caveVisibilityFixturesEnabled()) drawCaveVisibilityFixtures();
});
var _archVisDbg = document.getElementById('chkArchVisDbg');
if (_archVisDbg) _archVisDbg.addEventListener('change', function() { window.DEBUG_ARCH_VIS = this.checked; });
document.getElementById('chkCaveColors').addEventListener('change', function() { DEBUG_CAVE_COLORS = this.checked; });
document.getElementById('chkLayerTypes').addEventListener('change', function() { DEBUG_LAYER_TYPES = this.checked; });
document.getElementById('chkPolyTypes').addEventListener('change', function() { DEBUG_POLY_TYPES = this.checked; });
(function(){ var b = document.getElementById('chkCeilWire'); if (b) b.addEventListener('change', function() { DEBUG_CEIL_WIRE = this.checked; }); })();
(function(){ var b = document.getElementById('chkHideCeil'); if (b) b.addEventListener('change', function() { DEBUG_HIDE_CEIL = this.checked; }); })();
(function(){ var b = document.getElementById('chkHideWalls'); if (b) b.addEventListener('change', function() { DEBUG_HIDE_WALLS = this.checked; }); })();
(function(){ var b = document.getElementById('chkHidePlats'); if (b) b.addEventListener('change', function() { DEBUG_HIDE_PLATS = this.checked; }); })();

(function() {
  // Gamepad checkbox — mutually exclusive with keyboard/mouse
  var cg = document.getElementById('chkGamepad');
  if (cg) {
    cg.addEventListener('change', function() {
      USE_GAMEPAD = this.checked;
      if (USE_GAMEPAD) {
        USE_KEYBOARD = false; USE_MOUSE = false;
        var ck = document.getElementById('chkKeyboard'); if (ck) ck.checked = false;
        if (document.pointerLockElement === canvas) document.exitPointerLock();
      }
      if (running) {
        if (USE_GAMEPAD) startGamepadPolling(); else stopGamepadPolling();
      }
      updateHudInput();
    });
  }

  // Keyboard+Mouse checkbox — enables both WASD movement and mouse look; mutually exclusive with gamepad
  var ckb = document.getElementById('chkKeyboard');
  if (ckb) {
    ckb.addEventListener('change', function() {
      USE_KEYBOARD = this.checked;
      USE_MOUSE = this.checked;
      if (this.checked) {
        USE_GAMEPAD = false;
        var cg2 = document.getElementById('chkGamepad'); if (cg2) cg2.checked = false;
        stopGamepadPolling();
        kbState = {up:false, down:false, left:false, right:false};
        mouseAccum = {x:0, y:0}; mouseDelta = {x:0, y:0};
        if (running) canvas.requestPointerLock();
      } else {
        kbState = {up:false, down:false, left:false, right:false};
        if (document.pointerLockElement === canvas) document.exitPointerLock();
      }
      updateHudInput();
    });
  }

  // Keyboard events — only active when USE_KEYBOARD
  // F key — fullscreen toggle, always active regardless of keyboard/mouse mode
  document.addEventListener('keydown', function(e) {
    if (e.key === 'f' || e.key === 'F') { toggleFullscreen(); e.preventDefault(); }
  });

  document.addEventListener('keydown', function(e) {
    if (!USE_KEYBOARD && !USE_MOUSE) return;
    if (caveVisibilityFixturesEnabled()) return;
    var k = e.key;
    // Movement blocked while shop is open; also clear any held keys so the
    // player doesn't slide after closing.
    // Forge number key selection (1-5)
    if (forgeOpen && k >= '1' && k <= '5') { forgeSelectSlot(parseInt(k) - 1); e.preventDefault(); return; }
    if (!shopOpen && !forgeOpen) {
      // WASD = movement, Arrow keys = camera rotation/pitch
      if (k === 'w' || k === 'W') { kbState.up    = true; e.preventDefault(); return; }
      if (k === 's' || k === 'S') { kbState.down  = true; e.preventDefault(); return; }
      if (k === 'a' || k === 'A') { kbState.left  = true; e.preventDefault(); return; }
      if (k === 'd' || k === 'D') { kbState.right = true; e.preventDefault(); return; }
      if (k === 'ArrowLeft')  { _arrowCam.left  = true; e.preventDefault(); return; }
      if (k === 'ArrowRight') { _arrowCam.right = true; e.preventDefault(); return; }
      if (k === 'ArrowUp')    { _arrowCam.up    = true; e.preventDefault(); return; }
      if (k === 'ArrowDown')  { _arrowCam.down  = true; e.preventDefault(); return; }
    }
    // Tab cycles shop items (forward) / Shift+Tab cycles backward — only when shop is open
    if (k === 'Tab' && shopOpen) {
      var _vk = getVisibleShopItems();
      shopSelIdx = e.shiftKey
        ? (shopSelIdx - 1 + _vk.length) % _vk.length
        : (shopSelIdx + 1) % _vk.length;
      e.preventDefault();
    }
    else if (k === 'e' || k === 'E') {
      if (shopOpen)                { var _ve = getVisibleShopItems(); applyShopItem(_ve[shopSelIdx]); }
      else if (nearestOpenChest)   { collectChest(nearestOpenChest); }
      else if (shopNearby)         { shopOpen = true; shopSelIdx = 0; kbState = {up:false,down:false,left:false,right:false}; }
      else if (nearestShrine)      { activateShrine(nearestShrine); }
      else if (nearestArenaAltar)  { startArenaChallenge(nearestArenaAltar); }
      else if (forgeOpen)          { forgeConfirm(); }
      else if (fortressLockedNear) { pushToast('Defeat all enemies to unlock the relics', '#cc6644', 2500); }
      else if (nearestFortressInteract) { activateFortressInteract(nearestFortressInteract); }
      else                         { _attackHeld = true; castCurrentSpell(); }
      e.preventDefault();
    }
    else if (k === 't' || k === 'T') {
      // Cave visibility test: teleport to next test distance from entrance
      if (deepCaveEntrances && deepCaveEntrances.length > 0) {
        _caveTestStep = (_caveTestStep + 1) % _caveTestDistances.length;
        var _testDist = _caveTestDistances[_caveTestStep];
        var _testEntr = deepCaveEntrances[0];
        // Position player at the test distance, facing the entrance
        // deepCaveEntrances are already in local (window-relative) coords
        var _testLocalX = _testEntr.x;
        var _testLocalY = _testEntr.y;
        var _testAng = Math.atan2(_testLocalY - pos.y, _testLocalX - pos.x);
        pos.x = _testLocalX - Math.cos(_testAng) * _testDist;
        pos.y = _testLocalY - Math.sin(_testAng) * _testDist;
        cam.x = pos.x; cam.y = pos.y;
        cam.ang = _testAng;
        cam.pitch = 0;
        _caveTestLogNext = true;
        console.log('%c[CAVE-TEST] Step ' + _caveTestStep + ': dist=' + _testDist + ' pos=(' + pos.x.toFixed(0) + ',' + pos.y.toFixed(0) + ') facing entrance at (' + _testLocalX.toFixed(0) + ',' + _testLocalY.toFixed(0) + ')', 'color: #ff00ff; font-weight: bold; font-size: 14px');
      } else {
        console.log('[CAVE-TEST] No cave entrances found');
      }
      e.preventDefault();
    }
    else if (k === 'm' || k === 'M') { minimapMode = (minimapMode + 1) % 3; e.preventDefault(); }
    else if (k === 'c' || k === 'C') {
      toastLogOpen = !toastLogOpen;
      if (toastLogOpen) toastLogScroll = 0;
      e.preventDefault();
    }
    else if (k === 'i' || k === 'I') { if (!settingsOpen) { inventorySelIdx = (inventorySelIdx + 1 > EQUIP_SLOTS.length - 1) ? -1 : inventorySelIdx + 1; inventoryOpen = inventorySelIdx >= 0; } e.preventDefault(); }
    else if (k === 'o' || k === 'O') {
      if (!shopOpen && !inventoryOpen) {
        if (!settingsOpen) {
          // Opening — sync pending copy from current committed settings
          pendingSettings.viewDist    = settings.viewDist;
          pendingSettings.chunkWindow = settings.chunkWindow;
          pendingSettings.particles   = settings.particles;
          pendingSettings.resolution  = settings.resolution;
          pendingSettings.showFPS     = settings.showFPS;
          pendingSettings.dayNight    = settings.dayNight;
          _pendingQualityPreset       = qualityPreset;
          _settingsDirty = false;
          if (document.pointerLockElement === canvas) document.exitPointerLock();
        }
        // Closing without Apply — pending changes are simply discarded (no sync needed)
        settingsOpen = !settingsOpen;
      }
      e.preventDefault();
    }
    else if (k === 'Escape') { if (forgeOpen) { forgeOpen = false; forgeStep = 0; forgeSacrificeIdx = -1; forgeEnhanceIdx = -1; } else if (settingsOpen) { settingsOpen = false; /* discard pending — no sync needed */ } else if (inventorySelIdx >= 0) { inventorySelIdx = -1; inventoryOpen = false; } else if (shopOpen) { shopOpen = false; shopSelIdx = 0; } e.preventDefault(); }
    else if (k === ' ') { if (!shopOpen) jumpPressed = true; e.preventDefault(); }
    else if (k === 'Shift') { if (!shopOpen) dashPressed = true; e.preventDefault(); }
    // Number keys: select shop item when open, or switch spell when closed (among unlocked)
    else if (k >= '1' && k <= '9') {
      var ni = parseInt(k) - 1;
      if (shopOpen) {
        var _vn = getVisibleShopItems();
        if (ni < _vn.length) shopSelIdx = ni;
      } else {
        var _un = getUnlockedSpells();
        if (ni < _un.length) { currentSpellIdx = ni; lastSpellChangeMs = Date.now(); }
      }
      e.preventDefault();
    }
    // Log scroll — arrow keys when log is open
    if (toastLogOpen) {
      if (k === 'ArrowUp')   { toastLogScroll++; e.preventDefault(); }
      if (k === 'ArrowDown') { toastLogScroll = Math.max(0, toastLogScroll - 1); e.preventDefault(); }
    }
  });

  // Mouse wheel — scroll toast log when open
  canvas.addEventListener('wheel', function(e) {
    if (toastLogOpen) {
      toastLogScroll = Math.max(0, toastLogScroll + (e.deltaY < 0 ? 1 : -1));
      e.preventDefault();
    }
  }, {passive: false});

  document.addEventListener('keyup', function(e) {
    if (!USE_KEYBOARD && !USE_MOUSE) return;
    var k = e.key;
    if (k === 'w' || k === 'W') kbState.up = false;
    else if (k === 's' || k === 'S') kbState.down = false;
    else if (k === 'a' || k === 'A') kbState.left = false;
    else if (k === 'd' || k === 'D') kbState.right = false;
    else if (k === 'ArrowLeft')  _arrowCam.left  = false;
    else if (k === 'ArrowRight') _arrowCam.right = false;
    else if (k === 'ArrowUp')    _arrowCam.up    = false;
    else if (k === 'ArrowDown')  _arrowCam.down  = false;
    else if (k === 'e' || k === 'E') { _attackHeld = false; flameStreamActive = false; }
    else if (k === ' ') jumpPressed = false;
    else if (k === 'Shift') dashPressed = false;
  });

  // Mouse movement — accumulate delta when pointer locked to canvas
  document.addEventListener('mousemove', function(e) {
    if (!USE_MOUSE || document.pointerLockElement !== canvas) return;
    mouseDelta.x += e.movementX;
    mouseDelta.y += e.movementY;
  });

  // Mouse click — left button casts current spell while pointer is locked
  document.addEventListener('mousedown', function(e) {
    if (caveVisibilityFixturesEnabled()) return;
    if (!USE_MOUSE || document.pointerLockElement !== canvas) return;
    if (settingsOpen) return;
    if (e.button === 0) { _mouseHeld = true; castCurrentSpell(); e.preventDefault(); }
  });
  document.addEventListener('mouseup', function(e) {
    if (e.button === 0) {
      if (DEBUG_COMBAT && _mouseHeld) console.log('[COMBAT] mouseup: clearing _mouseHeld, pointerLock=' + (document.pointerLockElement === canvas));
      _mouseHeld = false; flameStreamActive = false;
    }
  });
  // Also clear _mouseHeld when pointer lock is lost (e.g. pressing Escape)
  // This prevents stuck _mouseHeld=true state
  document.addEventListener('pointerlockchange', function() {
    if (document.pointerLockElement !== canvas && _mouseHeld) {
      if (DEBUG_COMBAT) console.log('[COMBAT] pointer lock lost while _mouseHeld=true, clearing');
      _mouseHeld = false; flameStreamActive = false;
    }
  });

  // Scroll wheel — cycle spells (down = next, up = prev)
  canvas.addEventListener('wheel', function(e) {
    if (!USE_MOUSE || document.pointerLockElement !== canvas) return;
    e.preventDefault();
    cycleSpell(e.deltaY > 0 ? 1 : -1);
  }, { passive: false });

  // Pointer lock change — ESC releases lock; uncheck keyboard+mouse box and clear mode
  document.addEventListener('pointerlockchange', function() {
    if (document.pointerLockElement !== canvas && USE_MOUSE) {
      USE_MOUSE = false; USE_KEYBOARD = false;
      var ck = document.getElementById('chkKeyboard'); if (ck) ck.checked = false;
      mouseAccum = {x:0, y:0}; mouseDelta = {x:0, y:0};
      kbState = {up:false, down:false, left:false, right:false};
      updateHudInput();
    }
  });
})();

// Default state
var chkGamepad = document.getElementById('chkGamepad');
if (chkGamepad) { chkGamepad.checked = true; USE_GAMEPAD = true; }
var chkTextures = document.getElementById('btnToggleTex');
if (chkTextures) { USE_TEXTURES = true; chkTextures.textContent = 'Disable Textures'; }
var chk3dDebug = document.getElementById('chk3dDebug');
if (chk3dDebug) { chk3dDebug.checked = false; DEBUG_3D = false; }
var chkTexDebug = document.getElementById('chkTexDebug');
if (chkTexDebug) { chkTexDebug.checked = true; DEBUG_TEXTURE = true; }
var chkFloorDebug = document.getElementById('chkFloorDebug');
if (chkFloorDebug) { chkFloorDebug.checked = false; DEBUG_FLOOR = false; }
var chkDecorDebug = document.getElementById('chkDecorDebug');
if (chkDecorDebug) { chkDecorDebug.checked = true; DEBUG_DECORATIONS = true; }
var chkSkelDebug = document.getElementById('chkSkelDebug');
if (chkSkelDebug) { chkSkelDebug.checked = false; DEBUG_SKELETON = false; }
var chkCamFollow = document.getElementById('chkCamFollow');
if (chkCamFollow) { chkCamFollow.checked = true; CAM_FOLLOW = true; }
var chkPerfHud = document.getElementById('chkPerfHud');
if (chkPerfHud) { chkPerfHud.checked = false; DEBUG_PERF_HUD = false; }
var chkCaveDbg = document.getElementById('chkCaveDbg');
if (chkCaveDbg) { chkCaveDbg.checked = false; DEBUG_CAVE = false; }
buildSkeletonSprites();
buildWolfSprites();
buildPixelArmSprites();
checkSensorAvailability();
