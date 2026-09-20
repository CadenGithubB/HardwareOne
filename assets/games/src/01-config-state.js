

// =============================================
// CANVAS & RENDERING
// =============================================
var canvas = document.getElementById('maze');
var ctx = canvas.getContext('2d');
var USE_TEXTURES = true;
var BASE_SEED = 54321;
var depthBuffer = null;
var CANVAS_BASE_W = canvas.width;   // original authored resolution (360)

// =============================================
// GAME_CONFIG — Centralized balance, physics, combat & rendering constants
// =============================================
// All tunable game parameters live here. Subsystems read from this object
// at init or per-frame; the JIT inlines these lookups so there is zero
// perf cost vs. scattered vars.
var GAME_CONFIG = {
  // ── Player ──
  player: {
    healthMax:      100,
    healthRegenPerS: 3,
    manaMax:        100,
    manaRegenPerS:  15,
    baseSpeed:      90,
    baseFov:        1.3962634016,  // ~80° in radians
    dashSpeed:      400,           // impulse magnitude
    dashCooldownMs: 3000,
    dashDurationMs: 200,           // i-frame window
    dashFovPunch:   0.25,          // radians added to FOV during dash
    jumpImpulse:    300,           // vertical velocity on jump
    gravity:        600            // downward accel (units/s²)
  },
  // ── Terrain physics (indexed by terrain name) ──
  terrainPhysics: {
    ground:  { damping: 0.965, bounce: 0.45, deadzone: 1.5 },
    ice:     { damping: 0.992, bounce: 0.20, deadzone: 1.2 },
    plains:  { damping: 0.970, bounce: 0.40, deadzone: 1.5 },
    cave:    { damping: 0.975, bounce: 0.35, deadzone: 1.3 },
    expanse: { damping: 0.968, bounce: 0.38, deadzone: 1.4 }
  },
  // ── Combat ──
  combat: {
    shootCooldownMs: 300,
    projSpeed:       360,
    projLifeMs:      1200,
    projRadius:      3,
    manaCostMin:     12,
    manaCostMax:     35
  },
  // ── Tower ──
  tower: {
    fireIntervalMs: 1500,
    range:          180,
    height:         80,
    damage:         1.2,
    projSpeed:      320
  },
  // ── Spawners ──
  spawner: {
    globalMax: 8
  },
  // ── World generation ──
  world: {
    chunkSize:      480,     // world pixels per chunk
    chunkCells:     40,      // grid cells per chunk (chunkSize / cellSize)
    windowChunks:   7,       // NxN chunk window
    cellSize:       12,      // grid cell size in world pixels (matches meshGridSize)
    meshGridSize:   12,      // mesh subdivision size
    biomeBlendWidth: 0.06,   // noise-space blend zone between biomes
    biomeThresholds: { cave: 0.167, ground: 0.333, plains: 0.500, forest: 0.667, expanse: 0.833 }
  },
  // ── Rendering ──
  rendering: {
    projScale:       180,    // base projection scale factor
    lightCellSize:   15,     // world units per light grid cell
    fogFloor:        0.3,    // minimum fog brightness
    wallShadeN:      0.8,
    wallShadeS:      0.8,
    wallShadeE:      1.0,
    wallShadeW:      1.0,
    topShade:        1.15
  },
  // ── Camera ──
  camera: {
    stickYawSpeed:    1.8,
    stickPitchSpeed:  1.8,
    pitchMax:         1.4,      // ~80° max look range
    mouseSensitivity: 0.008,
    mouseCamSensitivity: 0.003,
    yawAlpha:         0.08,     // yaw smoothing
    pitchAlpha:       0.12      // pitch smoothing
  },
  // ── Physics frame ──
  physics: {
    fixedDt:         1 / 60,   // 60Hz tick
    fixedDtMs:       1000 / 60,
    maxSteps:        4          // spiral-of-death cap
  },
  // ── Input ──
  input: {
    selectToggleCooldown: 300,
    startMenuCooldown:    300
  },
  // ── Scoring ──
  scoring: {
    medalGold:   10,
    medalSilver: 15,
    medalBronze: 22
  },
  // ── Day/Night ──
  dayNight: {
    initialTime:  0.35,    // 0=midnight, 0.5=noon
    speed:        0.008,   // ~2 min full cycle
    baseSunIntensity: 0.6
  },
  // ── HUD ──
  hud: {
    minimapW: 90,
    minimapH: 65
  }
};
var CANVAS_BASE_H = canvas.height;  // original authored resolution (240)
var projScale = 180;                // projection scale factor — recomputed each frame for resolution independence
var resScale = 1;                   // resolution scale (1.0 at base res) — recomputed each frame

// =============================================
// BIOME_PALETTE — Centralized per-biome color definitions
// =============================================
// All biome-specific colors (floor, wall, sky, mountain, ceiling, pattern)
// live here. Consumed by: getFloorColor, makeWallColor, makeCeilPattern,
// makePattern, drawSkybox3D, drawSimpleWallSlice, getFloorColorBlended.
// Adding a new biome = add one entry here; all systems pick it up.
var BIOME_PALETTE = {
  cave: {
    wallColor:  '#6a6a70',
    ceilFill:   '#1a1a1e',
    patternBase:'#2a2a2e',
    wallBaseRGB: [95, 95, 105],
    // Floor height bands: 8 entries from deepest (-0.9) to highest (>0.5)
    floorBands: ['#18181c','#28282e','#3a3a42','#4e4e58','#7a7a80','#8a8a90','#9a9aa0','#aaaab0'],
    // Sky: [top, bottom] RGB arrays
    sky:     [[0x12,0x12,0x1a], [0x1a,0x1a,0x1e]],
    // Mountain layers: [far, mid, near] RGB arrays (hidden for cave)
    mountain:[[0x12,0x12,0x1a], [0x12,0x12,0x1a], [0x12,0x12,0x1a]],
    foothills: [0x12,0x12,0x1a],
    haze:      [15,15,25],
    mountainVisible: 0
  },
  ground: {
    wallColor:  '#a77a45',
    ceilFill:   '#3f2f1c',
    patternBase:'#3b2a18',
    wallBaseRGB: [180, 140, 100],
    floorBands: ['#0e1a12','#1e2e22','#2a4232','#3a5642','#5a8a69','#6a9a79','#7aaa89','#8aba99'],
    sky:     [[0x06,0x06,0x08], [0x0e,0x0c,0x08]],
    mountain:[[0x22,0x1a,0x0e], [0x1a,0x14,0x08], [0x12,0x0e,0x05]],
    foothills: [0x0c,0x0a,0x04],
    haze:      [30,22,12],
    mountainVisible: 1
  },
  plains: {
    wallColor:  '#a77a45',
    ceilFill:   '#3f2f1c',
    patternBase:'#3b2a18',
    wallBaseRGB: [180, 140, 100],
    floorBands: ['#12180a','#222e10','#344218','#4a5a28','#6a7a40','#808e50','#96a260','#a8b870'],
    sky:     [[0x06,0x06,0x08], [0x0e,0x0c,0x08]],
    mountain:[[0x22,0x1a,0x0e], [0x1a,0x14,0x08], [0x12,0x0e,0x05]],
    foothills: [0x0c,0x0a,0x04],
    haze:      [30,22,12],
    mountainVisible: 1
  },
  forest: {
    wallColor:  '#4b3723',
    ceilFill:   '#1a2a10',
    patternBase:'#2a3a18',
    wallBaseRGB: [75, 55, 35],
    floorBands: ['#0e1608','#1a2810','#283a18','#385020','#4a6830','#5a7a40','#6a8a50','#7a9a60'],
    sky:     [[0x08,0x0a,0x06], [0x12,0x18,0x0c]],
    mountain:[[0x1a,0x2a,0x12], [0x14,0x22,0x0c], [0x0e,0x1a,0x08]],
    foothills: [0x0c,0x14,0x06],
    haze:      [20,30,15],
    mountainVisible: 1
  },
  expanse: {
    wallColor:  '#c07838',
    ceilFill:   '#5a3010',
    patternBase:'#7a4e22',
    wallBaseRGB: [180, 140, 100],
    floorBands: ['#1a0e08','#2e1808','#4a2a10','#6b3e1a','#8b5a28','#a87040','#c48a52','#d8a86a'],
    sky:     [[0x06,0x04,0x08], [0x0e,0x0a,0x06]],
    mountain:[[0x2a,0x1c,0x10], [0x1e,0x14,0x08], [0x14,0x0e,0x05]],
    foothills: [0x0e,0x0a,0x04],
    haze:      [50,30,12],
    mountainVisible: 1
  },
  ice: {
    wallColor:  '#7aa7ff',
    ceilFill:   '#0d1a2e',
    patternBase:'#0a1322',
    wallBaseRGB: [140, 170, 240],
    floorBands: ['#0a0e1a','#141c30','#1e2c48','#2a3c5e','#4a6888','#6888a8','#88a8c8','#a0c0e0'],
    sky:     [[0x05,0x07,0x0f], [0x0b,0x0d,0x12]],
    mountain:[[0x1a,0x25,0x40], [0x14,0x1c,0x35], [0x0e,0x14,0x28]],
    foothills: [0x0c,0x12,0x20],
    haze:      [15,20,35],
    mountainVisible: 1
  }
};

// Floor height thresholds — shared by getFloorColor, maps heightPercent to band index
var _floorBandThresholds = [-0.9, -0.6, -0.3, -0.1, 0.1, 0.3, 0.5];

// =============================================
// SHARED 3D PROJECTION & OCCLUSION
// =============================================

// Returns camera state object used by all 3D renderers.
// Call once per renderer, destructure into local vars.
function getCam3D() {
  var w = canvas.width, h = canvas.height;
  var halfFov = cam.fov / 2;
  var cosAng = Math.cos(cam.ang), sinAng = Math.sin(cam.ang);
  var invTanHalf = 1 / Math.tan(halfFov);
  var pitchOff = Math.floor(-(cam.pitch || 0) * projScale);
  var horizonY = Math.floor(h * 0.5) + pitchOff;
  // cam.z retains the legacy player-height encoding (60 + 40 * mesh H).
  // Zero is a valid height; only a missing/nonfinite value uses the fallback.
  var cameraZ = 60 + ((Number.isFinite(cam.z) ? cam.z : 60) - 60) * (25 / 40);
  return { w: w, h: h, cosAng: cosAng, sinAng: sinAng, invTanHalf: invTanHalf,
           horizonY: horizonY, cameraZ: cameraZ };
}

// Projects a world point to screen coordinates.
// wx, wy = world position; wz = world Z height in world units (NOT grid cells).
// C = camera state from getCam3D().
// Returns {sx, sy, fwd} or null if behind camera.
function projToScreen(wx, wy, wz, C) {
  var dx = wx - cam.x, dy = wy - cam.y;
  var fwd = dx * C.cosAng + dy * C.sinAng;
  if (fwd < 1) return null;
  var rgt = dx * (-C.sinAng) + dy * C.cosAng;
  var sx = Math.floor((rgt / fwd * C.invTanHalf * 0.5 + 0.5) * C.w);
  var sy = Math.floor(C.horizonY + (C.cameraZ - wz) / fwd * projScale);
  return { sx: sx, sy: sy, fwd: fwd };
}

// Checks all occlusion conditions for a world entity and returns visibility info.
// Returns null if occluded, otherwise {sx, sy, fwd, dist, floorZ, fade}.
// opts: { maxDist, depthOffset, checkMidpoint, fadeFraction, skipDepth, stats }
//   stats: optional object — if provided, increments .tooFar / .behind / .depthOccl / .belowFloor / .midOccl
function entityVisible3D(wx, wy, wz, C, opts) {
  opts = opts || {};
  var sceneDepth = !!(opts.sceneDepth || opts.bounds);
  if (!Number.isFinite(wx) || !Number.isFinite(wy)) return null;
  var dx = wx - cam.x, dy = wy - cam.y;
  var dist = Math.hypot(dx, dy);
  if (dist < 1) dist = 1;
  var maxDist = opts.maxDist || viewDist;
  if (dist > maxDist) { if (opts.stats) opts.stats.tooFar++; return null; }

  var fwd = dx * C.cosAng + dy * C.sinAng;
  if (fwd < 1) { if (opts.stats) opts.stats.behind++; return null; }

  var rgt = dx * (-C.sinAng) + dy * C.cosAng;
  var sx = Math.floor((rgt / fwd * C.invTanHalf * 0.5 + 0.5) * C.w);

  // Depth buffer check — use min depth across a small column range to avoid
  // 1-pixel edge artifacts where a face boundary causes single-column occlusion flicker.
  if (!sceneDepth && !opts.skipDepth) {
    var depthOff = opts.depthOffset || 2;
    if (depthBuffer && sx >= 0 && sx < depthBuffer.length) {
      // Single-column read + larger tolerance (+4) replaces the 5-column scan
      // that was smoothing wall-seam 1-pixel flicker. Cheaper, same practical result.
      var _dMin = depthBuffer[sx];
      if (fwd > _dMin + depthOff + 4) {
        if (opts.stats) opts.stats.depthOccl++;
        return null;
      }
    }
  }

  // Floor height at entity
  var fh = floorMesh ? getFloorHeightAt(wx, wy) : 0;
  var floorZ = fh * 25;
  if (!sceneDepth && (wz || 0) < floorZ - 40) { if (opts.stats) opts.stats.belowFloor++; return null; }

  // Surface→underground entity culling: skip underground entities from surface
  if (!sceneDepth && floorMesh && !playerUnderground && fh < -0.1) return null;

  // Midpoint occlusion — hill between camera and entity.
  // Skip when underground (surface terrain would falsely occlude) or when the
  // entity is within ~80u (no hill physically fits between).
  if (!sceneDepth && opts.checkMidpoint && !playerUnderground && dist > 80) {
    var fhMid = floorMesh ? getFloorHeightAt((cam.x + wx) * 0.5, (cam.y + wy) * 0.5) : 0;
    if (fhMid * 25 > (C.cameraZ + floorZ) * 0.5 + 8) {
      if (opts.stats) opts.stats.midOccl++;
      return null;
    }
  }

  // Distance fade
  var fade = 1.0;
  var fadeFrac = opts.fadeFraction !== undefined ? opts.fadeFraction : 0.8;
  if (fadeFrac < 1 && dist > maxDist * fadeFrac) {
    fade = Math.max(0, 1.0 - (dist - maxDist * fadeFrac) / (maxDist * (1 - fadeFrac)));
  }

  // Migrated callers supply an absolute render height. Zero is valid; bounce
  // offsets belong in their animation, not in this world-height slot.
  var renderZ = sceneDepth ? (Number.isFinite(wz) ? wz : floorZ) : (wz || floorZ);
  var sy = Math.floor(C.horizonY + (C.cameraZ - renderZ) / fwd * projScale);
  return { sx: sx, sy: sy, fwd: fwd, dist: dist, floorZ: floorZ, renderZ:renderZ, fade: fade };
}

// =============================================
// GENERIC 3D ENTITY RENDERER
// =============================================
// Shared boilerplate for all entity types that follow the
// entityVisible3D → sort far-to-near → draw-callback pattern.
//
// Usage:
//   renderEntities3D(enemies, {maxDist: viewDist, depthOffset:5, sort:true}, function(entity, vis, C, ctx) {
//     // custom drawing code per entity type
//   });
//
// visOpts: passed to entityVisible3D, plus:
//   sort:       true = sort far-to-near (painter's order). default false.
//   minDist:    skip entities closer than this (default 0)
//   mode3dOnly: require MODE3D flag (default false)
//
// drawFn(entity, vis, C, ctx, now):
//   entity = array element
//   vis    = {sx, sy, fwd, dist, floorZ, fade} from entityVisible3D
//   C      = camera state from getCam3D
//   ctx    = canvas context
//   now    = Date.now() (computed once, shared)

// Pre-allocated sort buffers for renderEntities3D (shared across all callers)
var _reIdxBuf  = new Uint16Array(512);
var _reDistBuf = new Float32Array(512);
var _reVisBuf  = new Array(512);
for (var _ri = 0; _ri < 512; _ri++) _reVisBuf[_ri] = null;

function renderEntities3D(arr, visOpts, drawFn) {
  if (!arr || !arr.length) return;
  if (visOpts.mode3dOnly && !MODE3D) return;
  var C = getCam3D();
  var now = Date.now();
  var doSort = visOpts.sort || false;
  var minDist = visOpts.minDist || 0;

  // Visibility pass
  var count = 0;
  for (var i = 0; i < arr.length; i++) {
    var e = arr[i];
    var wx = e.x, wy = e.y;
    var wz = visOpts.groundAnchor ? getEntityRenderFloorZ(e) :
      (visOpts.sceneDepth || visOpts.bounds) ? (Number.isFinite(e.wz) ? e.wz : Number.isFinite(e.z) ? e.z : getEntityRenderFloorZ(e)) : (e.wz || e.z || 0);
    if ((visOpts.sceneDepth || visOpts.bounds) && !Number.isFinite(wz)) continue;
    var vis = entityVisible3D(wx, wy, wz, C, visOpts);
    if (!vis) continue;
    if (visOpts.groundAnchor) vis.floorZ = wz;
    if (vis.dist < minDist) continue;
    if (count >= 512) break; // safety cap
    _reIdxBuf[count] = i;
    _reDistBuf[count] = vis.dist;
    _reVisBuf[count] = vis;
    count++;
  }
  if (count === 0) return;

  // Sort far-to-near (painter's order) if requested
  if (doSort && count > 1) {
    // Insertion sort on flat buffers — zero allocation
    for (var si = 1; si < count; si++) {
      var sIdx = _reIdxBuf[si], sDist = _reDistBuf[si], sVis = _reVisBuf[si];
      var j = si - 1;
      while (j >= 0 && _reDistBuf[j] < sDist) {
        _reIdxBuf[j + 1] = _reIdxBuf[j];
        _reDistBuf[j + 1] = _reDistBuf[j];
        _reVisBuf[j + 1] = _reVisBuf[j];
        j--;
      }
      _reIdxBuf[j + 1] = sIdx;
      _reDistBuf[j + 1] = sDist;
      _reVisBuf[j + 1] = sVis;
    }
  }

  // Draw pass
  for (var di = 0; di < count; di++) {
    var entity = arr[_reIdxBuf[di]], visible = _reVisBuf[di];
    if (typeof visOpts.bounds === 'function') {
      var bounds = visOpts.bounds(entity, visible, C, now);
      withSceneDepthBillboard(bounds, visible.fwd, function() {
        drawFn(entity, visible, C, ctx, now);
      });
    } else {
      drawFn(entity, visible, C, ctx, now);
    }
  }
}

// =============================================
// GENERIC COLLECTIBLE UPDATER
// =============================================
// Shared boilerplate for vacuum-pull + proximity-collect pattern.
//
// Usage:
//   soulOrbs = updateCollectibles(soulOrbs, {collectRadius:22, vacuumRadius:80, vacuumPull:3.5}, function(item) {
//     mana = Math.min(MANA_MAX, mana + 15);
//   });
//
// opts:
//   collectRadius:  distance to collect (world px)
//   vacuumRadius:   distance for vacuum pull (world px), or function() returning radius
//   vacuumPull:     pull strength, or function() returning strength
//   dashVacRadius:  vacuum radius during dash (optional, overrides vacuumRadius)
//   dashVacPull:    pull strength during dash (optional, overrides vacuumPull)
//
// onCollect(item):
//   Called when item is collected. Return value ignored.
//   Item is removed from array automatically.
//
// Returns the filtered array (kept items only).

function updateCollectibles(arr, opts, onCollect) {
  if (!arr || !arr.length) return arr;
  var kept = [];
  var cr = opts.collectRadius;
  var isDashing = Date.now() < dashUntil;
  var vr = (isDashing && opts.dashVacRadius) ? opts.dashVacRadius : (typeof opts.vacuumRadius === 'function' ? opts.vacuumRadius() : opts.vacuumRadius);
  var vp = (isDashing && opts.dashVacPull)   ? opts.dashVacPull   : (typeof opts.vacuumPull === 'function'   ? opts.vacuumPull()   : opts.vacuumPull);
  // Interest management: items past the vacuum radius don't collect or pull,
  // so we only need the cheap squared-distance gate. Skip hypot for those.
  var _imVacR = vr || cr;
  var _imVacRSq = _imVacR * _imVacR;
  for (var i = 0; i < arr.length; i++) {
    var item = arr[i];
    var dx = pos.x - item.x, dy = pos.y - item.y;
    var dsq = dx * dx + dy * dy;
    if (dsq > _imVacRSq) { kept.push(item); continue; }
    var dist = Math.sqrt(dsq);
    if (dist < cr) {
      onCollect(item);
    } else {
      // Vacuum pull
      if (vr && dist < vr && dist > 1) {
        var pull = (vr - dist) / vr * vp;
        item.x += (dx / dist) * pull;
        item.y += (dy / dist) * pull;
      }
      kept.push(item);
    }
  }
  return kept;
}

// =============================================
// FULLSCREEN
// =============================================
function toggleFullscreen() {
  var fs = document.fullscreenElement || document.webkitFullscreenElement
         || document.mozFullScreenElement || document.msFullscreenElement;
  if (!fs) {
    var req = canvas.requestFullscreen || canvas.webkitRequestFullscreen
            || canvas.mozRequestFullScreen || canvas.msRequestFullscreen;
    if (req) req.call(canvas);
  } else {
    var ex = document.exitFullscreen || document.webkitExitFullscreen
           || document.mozCancelFullScreen || document.msExitFullscreen;
    if (ex) ex.call(document);
  }
}

function onFullscreenChange() {
  var fs = document.fullscreenElement || document.webkitFullscreenElement
         || document.mozFullScreenElement || document.msFullscreenElement;
  if (fs === canvas) {
    // Entering fullscreen — expand draw buffer to the actual screen size.
    // A brief rAF ensures the browser has committed the new viewport size.
    requestAnimationFrame(function() {
      canvas.width  = window.innerWidth  || screen.width;
      canvas.height = window.innerHeight || screen.height;
    });
  } else {
    // Exiting fullscreen — restore authored resolution.
    canvas.width  = CANVAS_BASE_W;
    canvas.height = CANVAS_BASE_H;
  }
}
document.addEventListener('fullscreenchange',       onFullscreenChange);
document.addEventListener('webkitfullscreenchange', onFullscreenChange);
document.addEventListener('mozfullscreenchange',    onFullscreenChange);
document.addEventListener('MSFullscreenChange',     onFullscreenChange);

// =============================================
// GAME STATE
// =============================================
var running = false;
var gameOverState = false;
var polling = null;
var lastUpdate = 0;
var level = 1;
var collisions = 0;
var startMs = 0;
var timeSec = 0;
var pos = {x:20, y:20};
var vel = {x:0, y:0};
var dashPressed = false, jumpPressed = false;
var dashCooldownUntil = 0, dashUntil = 0;  // timestamps: cooldown end, i-frame end
var dashFovPunch = 0;                      // FOV punch lerp (0..1)
var jumpVelZ = 0, jumpAirborne = false;    // jump physics
var walls = [];
var goal = null;
var goalSpawned = false;
var goalMessage = '';
var goalMessageUntil = 0;
var toasts = []; // [{text, color, spawnMs, lifeMs}]
var toastLog = []; // persistent ordered log [{text, color, timeLabel}]
var toastLogOpen = false;
var toastLogScroll = 0; // lines scrolled from bottom

// =============================================
// PHYSICS CONFIG
// =============================================
var dead = GAME_CONFIG.terrainPhysics.ground.deadzone;
var maxAng = 15;     // max tilt angle
var speed = GAME_CONFIG.player.baseSpeed;
var damping = GAME_CONFIG.terrainPhysics.ground.damping;
var bounce = GAME_CONFIG.terrainPhysics.ground.bounce;
var cell = GAME_CONFIG.world.cellSize;

// =============================================
// TERRAIN
// =============================================
var terrain = 'ground';
var bgPattern = null;
var ceilPattern = null;
var wallColor = '#8b6b3a';

// =============================================
// GRID & LEVEL GEOMETRY
// =============================================
var worldW = 720;          // active level world width  (set in resetLevel)
var worldH = 480;          // active level world height (set in resetLevel)
var viewDist = 1400;       // max render distance — overridden by quality preset
var borderVariations = []; // [{x1,y1,x2,y2,width}] subtle border wall roughening capsules
var deepCaveRegions = [];  // [{x1,y1,x2,y2,width,ceilZ,type}] deep cave capsule segments
var deepCaveEntrances = []; // [{x,y,angle,depth,ceilH}] world-space entrance positions

// Oriented reserved footprint around each cave entrance. World generators
// (mesh blend, grid walls, scatter, decorations, ore, lights) skip this zone
// so the stone archway opening stays unobstructed.
// along = axis through doorway, cross = pillar-to-pillar. Tuned to archway:
// ARCH_W=56 + margin → cross half=34; along half=44 (approach + threshold).
function inEntranceReserve(wx, wy) {
  if (!deepCaveEntrances || !deepCaveEntrances.length) return false;
  for (var _ier = 0; _ier < deepCaveEntrances.length; _ier++) {
    var _iee = deepCaveEntrances[_ier];
    var _iedx = wx - _iee.x, _iedy = wy - _iee.y;
    // cosA/sinA baked at entrance creation; fallback for legacy entries.
    var _iec = _iee.cosA, _ies = _iee.sinA;
    if (_iec === undefined) {
      var _iea = _iee.angle || 0;
      _iec = Math.cos(_iea); _ies = Math.sin(_iea);
      _iee.cosA = _iec; _iee.sinA = _ies;
    }
    var _iAlong = _iedx * _iec + _iedy * _ies;
    var _iCross = -_iedx * _ies + _iedy * _iec;
    if (Math.abs(_iAlong) < 44 && Math.abs(_iCross) < 34) return true;
  }
  return false;
}
// Pre-allocated buffer for distance-sorted floor cell render. Packs
// [distSq, x, y] triples. Max size = (2 * viewDist/gs + 2)^2 ~ 200^2 at
// high viewDist; 40000*3 = 120000 Float32 entries = ~480KB. One-time alloc.
var _drawFloorCellBuf = new Float32Array(120000);
// Wall projection scratch buffers — reused across frames by drawWalls3D/renderFace.
// 4 corners per quad: 0=bottom-left, 1=bottom-right, 2=top-right, 3=top-left.
var _wsx = new Float32Array(4), _wsy = new Float32Array(4), _wfwd = new Float32Array(4);
// Per-frame wall gradient cache (A1-3). Key packs color bucket + y-range bucket;
// cleared at drawWalls3D entry. Canvas gradients bake absolute coords so the
// y-range is part of the key — identical faces at same screen height share.
var _wallGradCache = new Map();
// findMatchZ candidates live on floorMesh.walkCandZ, built once at
// map-assembly time by buildWalkCandZ(). No per-frame cache is needed.

// Per-frame entrance cull mask — rasterized bitmap of cells inside any
// active cave-entrance "mouth hole" rectangle. Replaces the per-quad
// transform + rectangle check in drawLayersFloor3D. Allocated on demand
// at the window-mesh size.
var _entCullMask = null;
var _entCullMaskN = 0;
function buildWalkCandZ(m) {
  if (!m || !m.layerCount) return;
  var n = m.layerCount.length;
  m.walkCandZ = new Array(n);
  // Per-cell bitmasks (1 byte each, 5 bits used).
  //   ceilAboveMask: bit li set = a type=2 (ceiling) layer exists ABOVE layer li.
  //   capAboveMask:  bit li set = a type=4 (surface cap) layer exists ABOVE layer li.
  // Read at floor-render time to avoid per-quad layer-scan loops.
  m.ceilAboveMask = new Uint8Array(n);
  m.capAboveMask  = new Uint8Array(n);
  var cells = 0, entries = 0;
  for (var i = 0; i < n; i++) {
    var lc = m.layerCount[i];
    if (!lc) { m.walkCandZ[i] = null; continue; }
    // First pass: walkable-Z candidates.
    var z0=0,z1=0,z2=0,z3=0,z4=0,cnt=0;
    // Per-layer types (loaded once, reused for both passes).
    var t0 = lc > 0 ? m.l0Type[i] : 0;
    var t1 = lc > 1 ? m.l1Type[i] : 0;
    var t2 = lc > 2 ? m.l2Type[i] : 0;
    var t3 = lc > 3 ? m.l3Type[i] : 0;
    var t4 = lc > 4 ? m.l4Type[i] : 0;
    for (var li = 0; li < lc; li++) {
      var t, z;
      if (li === 0) { t = t0; z = m.l0TopZ[i]; }
      else if (li === 1) { t = t1; z = m.l1TopZ[i]; }
      else if (li === 2) { t = t2; z = m.l2TopZ[i]; }
      else if (li === 3) { t = t3; z = m.l3TopZ[i]; }
      else { t = t4; z = m.l4TopZ[i]; }
      if (t === 1 || t === 3 || t === 4) {
        if (cnt === 0) z0 = z;
        else if (cnt === 1) z1 = z;
        else if (cnt === 2) z2 = z;
        else if (cnt === 3) z3 = z;
        else z4 = z;
        cnt++;
      }
    }
    if (cnt === 0) m.walkCandZ[i] = null;
    else {
      var arr = new Float32Array(cnt);
      arr[0] = z0;
      if (cnt > 1) arr[1] = z1;
      if (cnt > 2) arr[2] = z2;
      if (cnt > 3) arr[3] = z3;
      if (cnt > 4) arr[4] = z4;
      m.walkCandZ[i] = arr;
      cells++; entries += cnt;
    }
    // Second pass: above-masks. For each li, look at layers li+1..lc-1.
    // "Above" means higher li index; types t1..t4 correspond to layers 1..4.
    var ceilMask = 0, capMask = 0;
    for (var li2 = 0; li2 < lc; li2++) {
      var sawCeil = 0, sawCap = 0;
      for (var lj = li2 + 1; lj < lc; lj++) {
        var tj = lj === 1 ? t1 : lj === 2 ? t2 : lj === 3 ? t3 : lj === 4 ? t4 : t0;
        if (tj === 2) sawCeil = 1;
        else if (tj === 4) sawCap = 1;
      }
      if (sawCeil) ceilMask |= (1 << li2);
      if (sawCap)  capMask  |= (1 << li2);
    }
    m.ceilAboveMask[i] = ceilMask;
    m.capAboveMask[i]  = capMask;
  }
  console.log('[WALK-CAND] ' + cells + ' cells, ' + entries + ' candidates');
  _cacheStats.matchZ.size = cells;
  _cacheStats.matchZ.peakSize = cells;
}

// Cache telemetry for perf HUD. Counters reset per frame; history ring feeds
// sparkline. Entries are added by the cache hit/miss sites themselves.
var CACHE_HIST_LEN = 120;
var _cacheStats = {
  matchZ:   {hits:0, misses:0, size:0, peakSize:0,
             hitsHist:new Int32Array(CACHE_HIST_LEN),
             missHist:new Int32Array(CACHE_HIST_LEN),
             sizeHist:new Int32Array(CACHE_HIST_LEN)},
  wallGrad: {hits:0, misses:0, size:0, peakSize:0,
             hitsHist:new Int32Array(CACHE_HIST_LEN),
             missHist:new Int32Array(CACHE_HIST_LEN),
             sizeHist:new Int32Array(CACHE_HIST_LEN)},
  rgbQ:     {hits:0, misses:0, size:0, peakSize:0,
             hitsHist:new Int32Array(CACHE_HIST_LEN),
             missHist:new Int32Array(CACHE_HIST_LEN),
             sizeHist:new Int32Array(CACHE_HIST_LEN)},
  floorH:   {hits:0, misses:0, size:0, peakSize:0,
             hitsHist:new Int32Array(CACHE_HIST_LEN),
             missHist:new Int32Array(CACHE_HIST_LEN),
             sizeHist:new Int32Array(CACHE_HIST_LEN)}
};
var _cacheHistIdx = 0;
function _cacheCommitFrame() {
  var i = _cacheHistIdx;
  var names = ['matchZ', 'wallGrad', 'rgbQ', 'floorH'];
  for (var ni = 0; ni < names.length; ni++) {
    var s = _cacheStats[names[ni]];
    s.hitsHist[i] = s.hits;
    s.missHist[i] = s.misses;
    s.sizeHist[i] = s.size;
    if (s.size > s.peakSize) s.peakSize = s.size;
    s.hits = 0; s.misses = 0;
  }
  _cacheHistIdx = (i + 1) % CACHE_HIST_LEN;
}
var deepCaveChambers = []; // [{cx,cy,radius,ceilZ}] circular cave chambers
var currentBorderPoly = null; // irregular border polygon for plains/cave terrain
var grid = null;
var gridW = 0;
var gridH = 0;
var wallHeights = null;
var wallColorR = null, wallColorG = null, wallColorB = null; // per-cell wall color overrides (structure walls)
// wallCapZ[i]: world-Z of the walkable layer directly above this wall's top
// (the "dirt ceiling" over a cave wall), or -Infinity if none. Hides cave
// walls from a surface camera without distance gates. Filled at window
// assembly; see [WALL-CAP] log.
var wallCapZ = null;
// wallMaxTopZ[i]: highest allowed wall top in mesh-Z. For entrance-mouth
// cells (no cap, has ceiling), relaxed to the lowest neighbor cap so walls
// don't rise above surrounding ground. Infinity = no clamp.
var wallMaxTopZ = null;
var wallDecorations = [];
var oreVeins = [];        // breakable mineral deposits on cave walls; cleared each level
var floorScatter = [];    // static decorative debris/props on open floor cells
var _prevWindowChunks = {}; // chunk keys present in last assembleWindow — used for spawn-fade
var treasureChests = [];  // [{x,y,gold,collected,equipId?,opened,lidAngle}] interactive chests
var nearestOpenChest = null;  // closest open chest for E-key interaction
var enemySpawners = [];   // [{x,y,hp,maxHp,cooldown,lastSpawn,type}] spawner obelisks in caves
var platforms = [];
var floorMesh = null;

// =============================================
// CAMERA
// =============================================
var cam = {x:20, y:20, ang:0, fov:GAME_CONFIG.player.baseFov};
var viewCam = {x:0, y:0};
var CAM_FOLLOW = false;
var NOCLIP = false;
var MODE3D = false;

// Minimap
var minimapMode = 0;  // 0=small, 1=large, 2=hidden
var exploredCells = null;     // Uint8Array tracking visited grid cells
var minimapCanvas = null;     // offscreen canvas for cached terrain render
var minimapDirty = true;      // true when new cells revealed, triggers redraw

// ── Endless Mode ──
var ENDLESS_MODE = false;
var CAVE_TEST_MODE = false;  // All-plains with guaranteed cave at center
var caveTestFlags = {
  surfaceEnemies: false,   // surface random enemies
  structures: false,       // fortresses/ruins
  markets: false,          // market stalls
  shrines: false,          // shrines
  spawners: false,         // enemy spawner pylons
  surfaceChests: false,    // surface treasure chests
  caveEnemies: true,       // enemies inside the cave
  caveChests: true         // chests inside the cave
};
var WORLD_SEED = 12345;
var CHUNK_SIZE = GAME_CONFIG.world.chunkSize;
var CHUNK_CELLS = GAME_CONFIG.world.chunkCells;
var WINDOW_CHUNKS = GAME_CONFIG.world.windowChunks;
var chunks = {};              // loaded chunks keyed by "cx,cy"
var windowCX = 0, windowCY = 0;       // chunk coords of window top-left
var windowOriginX = 0, windowOriginY = 0; // world pixel coords of window top-left
var entityDeltas = {};        // persistent entity state changes per chunk
var _biomeSeed = 0;           // separate noise seed for biome map

// =============================================
// INPUT MODE FLAGS
// =============================================
var USE_GAMEPAD = false;
var USE_KEYBOARD = false;
var USE_MOUSE = false;
var CONTROL_MODE = 2;
var MODE_GYRO_AIM = 1;
var MODE_STICK_AIM = 2;

// =============================================
// KEYBOARD & MOUSE STATE
// =============================================
var kbState = {up:false, down:false, left:false, right:false};
var _mouseHeld = false; // left mouse button held for continuous stream attacks
var _attackHeld = false; // E key held for continuous keyboard attacks
var _arrowCam = {up:false, down:false, left:false, right:false};
var mouseAccum = {x:0, y:0};
var MOUSE_SENSITIVITY = GAME_CONFIG.camera.mouseSensitivity;
var mouseDelta = {x:0, y:0};       // raw pixel deltas consumed each frame for camera
var MOUSE_CAM_SENSITIVITY = GAME_CONFIG.camera.mouseCamSensitivity;

// =============================================
// GAMEPAD STATE
// =============================================
var gpPoll = null;
var gpLast = {x:0, y:0, buttons:0, valid:false};
var gpBackoffUntil = 0;
var BTN_X = (1<<6), BTN_Y = (1<<2), BTN_A = (1<<5), BTN_B = (1<<1);
var BTN_SELECT = (1<<0);
var BTN_START = (1<<16);
var STICK_CAM_YAW_SPEED = GAME_CONFIG.camera.stickYawSpeed;
var STICK_CAM_PITCH_SPEED = GAME_CONFIG.camera.stickPitchSpeed;
var CAM_PITCH_MAX = GAME_CONFIG.camera.pitchMax;

// =============================================
// BUTTON TOGGLE STATE
// =============================================
var lastSelectDown = false;
var lastSelectToggleMs = 0;
var SELECT_TOGGLE_COOLDOWN = GAME_CONFIG.input.selectToggleCooldown;
var menuOpen = false;
var lastStartMenuDown = false;
var lastStartMenuToggleMs = 0;
var START_MENU_COOLDOWN = GAME_CONFIG.input.startMenuCooldown;

// =============================================
// IMU & CALIBRATION
// =============================================
var basePitch = 0, baseRoll = 0, baseYaw = 0, hasBaseline = false;
var calibTimer = null, calibMs = 1000, calibSamples = [];
var calibrating = false, calStableMs = 0, calNeedStableMs = 1200;
var lastRoll = 0, lastPitch = 0, lastYaw = 0, hasYaw = false;
var USE_YAW = true, yawTarget = 0, yawAlpha = GAME_CONFIG.camera.yawAlpha;
var USE_PITCH = true, pitchTarget = 0, pitchAlpha = GAME_CONFIG.camera.pitchAlpha, pitchOffset = 0, autoPitchOff = 0;
var yawOffset = 0, capturingYaw = false, yawAtPress = 0, camHold = 0, yawResumeUntil = 0, prevX = false;

// =============================================
// SKELETON SPRITES
// =============================================
var skeletonFrames = null;  // skeletonFrames[typeId][dirIdx][frameIdx]
var SKEL_FRAMES = 8;
var SKEL_W = 48, SKEL_H = 80;
var wolfFrames = null;      // wolfFrames[dirIdx][frameIdx]
var WOLF_FRAMES = 8;
var WOLF_W = 80, WOLF_H = 52; // wider than tall — side-view quadruped
var SPRITE_DIRS = 8;
// Mirror lookup: canonical source direction for each of 8 dirs
// Dirs 5,6,7 are horizontal flips of dirs 3,2,1
var MIRROR_DIR  = [0, 1, 2, 3, 4, 3, 2, 1];
var MIRROR_FLIP = [false, false, false, false, false, true, true, true];
// Reusable scratch canvas for ambient-light tinting — lets source-atop
// composite darken only bone pixels, not the transparent bounding box.
var _skelScratch = document.createElement('canvas');
var _skelScratchCtx = _skelScratch.getContext('2d');

// Direction index from enemy facing angle vs camera angle
// 0=front (toward cam), 2=right profile, 4=back, 6=left profile
function getDirIndex(enemyFacing, camAngle) {
  var rel = ((enemyFacing - camAngle - Math.PI) % (Math.PI * 2) + Math.PI * 2) % (Math.PI * 2);
  return Math.round(rel / (Math.PI * 0.25)) % 8;
}


// =============================================
// SENSOR HARDWARE STATE
// =============================================
var i2cEnabled = false;
var imuRunning = false;
var imuCompiled = false;
var inputRunning = false;
var inputCompiled = false;

// =============================================
// COMBAT
// =============================================
var MANA_MAX = GAME_CONFIG.player.manaMax, mana = GAME_CONFIG.player.manaMax, MANA_REGEN_PER_S = GAME_CONFIG.player.manaRegenPerS;
var MANA_COST_MIN = GAME_CONFIG.combat.manaCostMin, MANA_COST_MAX = GAME_CONFIG.combat.manaCostMax;
var manaBlinkUntil = 0;
var HEALTH_MAX = GAME_CONFIG.player.healthMax, health = GAME_CONFIG.player.healthMax, HEALTH_REGEN_PER_S = GAME_CONFIG.player.healthRegenPerS;
var SHOOT_COOLDOWN = GAME_CONFIG.combat.shootCooldownMs, PROJ_SPEED = GAME_CONFIG.combat.projSpeed, PROJ_LIFE_MS = GAME_CONFIG.combat.projLifeMs, PROJ_RADIUS = GAME_CONFIG.combat.projRadius;
var projectiles = [];
var lastShotMs = 0;
var impacts = [];
var coneEffects = [];
var groundEffects = [];   // fire patches, ice patches, poison clouds on the floor
var chainEffects = [];    // lightning chain arcs between enemies
var novaEffects = [];     // expanding ring from arcane blast
var spells = {
  missile:  {id:'missile',  name:'Magic Missile',    color:'#4db6ff', damage:1.0, speed:360, manaCost:10,
             attackType:'projectile', homing:0.8, tier:1, unlocked:true},
  fire:     {id:'fire',     name:'Flame Jet',        color:'#ff6600', damage:0.5, speed:300, manaCost:2.0,
             attackType:'stream', streamRange:140, streamWidth:0.40, groundFire:true, streamTickMs:80, tier:1, unlocked:false},
  ice:      {id:'ice',      name:'Frost Bolt',       color:'#00ffff', damage:1.0, speed:380, manaCost:14,
             attackType:'projectile', instantSlow:true, groundPatch:true, tier:1, unlocked:false},
  lightning:{id:'lightning', name:'Chain Lightning',  color:'#ffff00', damage:1.8, speed:450, manaCost:20,
             attackType:'projectile', chainCount:2, chainRange:80, chainDmgFalloff:0.5, tier:1, unlocked:false},
  poison:   {id:'poison',   name:'Poison Cloud',     color:'#88ff44', damage:0.4, speed:200, manaCost:18,
             attackType:'lob', lobRange:180, cloudRadius:50, cloudDuration:4000, tier:1, unlocked:false},
  arcane:   {id:'arcane',   name:'Arcane Blast',     color:'#cc66ff', damage:2.0, speed:0,   manaCost:25,
             attackType:'nova', novaRadius:100, stunDuration:800, tier:1, unlocked:false}
};
var spellOrder = ['missile','fire','ice','lightning','poison','arcane'];
var currentSpellIdx = 0;
var lastSpellChangeMs = 0;
var lastADown = false;
var flameStreamActive = false; // true while holding fire button with flame selected
var flameStreamLastTick = 0;   // last time stream dealt damage/drained mana
var flameStreamAng = 0;        // current aim angle of the stream
var flameStreamRange = 160;    // current range (may be limited by wall hits)
var flameStreamStartMs = 0;    // when the stream started (for ramp-up)
var lastXDown = false;
var lastShopGpA = false, lastShopGpB = false, lastShopGpX = false, lastShopGpY = false;
var stats = {totalDamageDone:0, totalDamageTaken:0, totalManaConsumed:0, totalEnemiesKilled:0};

// =============================================
// ENEMIES
// =============================================
var enemies = [];
var deathEffects = [];
var soulOrbs = [];
var coinDrops = [];      // gold coins dropped by enemies — picked up for shop currency
var ambientParticles = [];  // atmospheric particles (dust, snow, drips, etc.)
var coins = 0;           // current coin count
var shopMarker = null;   // {x,y} — market center for interaction proximity
var discoveredMarkets = []; // persistent list of {wx,wy} for minimap
var marketStalls = [];   // [{x,y,facing,stallType}] — physical stall objects in window-local coords
var guardTowerLastFire = 0;
var TOWER_FIRE_INTERVAL = GAME_CONFIG.tower.fireIntervalMs;
var TOWER_RANGE = GAME_CONFIG.tower.range;
var TOWER_HEIGHT = GAME_CONFIG.tower.height;
var towerSpell = {id:'tower', name:'Tower Bolt', color:'#ffd54f', damage:GAME_CONFIG.tower.damage, speed:GAME_CONFIG.tower.projSpeed, attackType:'projectile', isTower:true};
var shopOpen = false;    // shop overlay visible
var shopNearby = false;  // player within interaction range
var shopSelIdx = 0;      // currently-highlighted item in the shop UI
var wardActive = false;  // "next hit absorbed" buff from Warding Stone
var dmgBoostUntil = 0;   // timestamp for Power Rune damage boost
var speedBoostUntil = 0; // timestamp for Wind Boots speed boost
var regenBoostUntil = 0; // timestamp for shrine regen buff
var armorBoostUntil = 0; // timestamp for shrine armor buff
var lastRegenTick = 0;   // last time regen healed player
var shrines = [];           // active shrines in current chunk window
var discoveredShrines = []; // persistent for minimap [{wx,wy,buffType}]
var nearestShrine = null;   // shrine in interaction range
var ruins = [];             // active ruins in current chunk window
var largeStructures = [];   // multi-chunk structures [{x,y,type,centerWX,centerWY}]
var discoveredRuins = [];   // persistent for minimap [{wx,wy,ruinType}]
var arenaChallenge = null;       // active arena challenge state
var completedArenas = {};        // {"regionX,regionY": true} — persists across chunk loads
var nearestArenaAltar = null;    // arena altar in E-key interaction range
var nearestFortressInteract = null; // {type:'forge'|'garrison'|'lectern', structure, x, y}
var fortressLockedNear = null;      // {structure, x, y} — player is near locked (uncleared) fortress relics
var completedFortresses = {};       // {"regionX,regionY": {forge:true, garrison:true, lectern:true}}
var fortressAllies = [];            // [{x, y, z, health, maxHealth, speed, ...}]
var mapRevealAnim = null;           // {startMs, centerGX, centerGY, revealRadius, duration, prevMode}
var forgeOpen = false;
var forgeStep = 0;                  // 0=select sacrifice, 1=select enhance
var forgeSacrificeIdx = -1;
var forgeEnhanceIdx = -1;
var forgeStructure = null;          // reference to fortress structure for current forge session
var arcaneTomes = [];            // special spell-upgrade pickups [{x,y,spawnMs,bob}]
var endlessCaveNetworks = {};    // keyed by "regionX,regionY" → cave network data
var statPickups = [];            // [{x, y, type, bob, wx, wy}] — findable permanent upgrades
var collectedStatPickups = {};   // {"wx,wy": true} — prevents re-spawning
var permanentSpeedBonus = 0;     // cumulative speed multiplier bonus
var permanentHealthBonus = 0;    // cumulative HP max bonus
var permanentManaBonus = 0;      // cumulative mana max bonus
var companions = [];             // [{type, x, y, z, phase, squash, lastAttackMs, ...}]
var castAnimUntil = 0;   // timestamp for FPS arm cast animation
var walkBobPhase = 0;    // running phase for arm bob

// ── Equipment System ──
var EQUIP_SLOTS = [
  {key: 'armor', label: 'Armor', icon: 'A'},
  {key: 'hat',   label: 'Hat',   icon: 'H'},
  {key: 'robes', label: 'Robes', icon: 'R'},
  {key: 'boots', label: 'Boots', icon: 'B'},
  {key: 'relic', label: 'Relic', icon: '\u2726'}
];
var equipment = {armor: null, hat: null, robes: null, boots: null, relic: null};
var phoenixFeatherUsed = false;
var inventoryOpen = false; // legacy compat
var inventorySelIdx = -1;  // -1=closed, 0-4=selected equip slot

// ── Settings / Quality ──
var settingsOpen = false;
var qualityPreset = 'medium';
var _pendingQualityPreset = 'medium'; // tracks preset selection in the staging copy
var QUALITY_PRESETS = {
  low:    {viewDist: 500,  chunkWindow: 5, particles: 10, resolution: 0.5},
  medium: {viewDist: 900,  chunkWindow: 5, particles: 25, resolution: 0.75},
  high:   {viewDist: 1300, chunkWindow: 7, particles: 40, resolution: 1.0},
  ultra:  {viewDist: 1800, chunkWindow: 9, particles: 60, resolution: 1.0}
};
var settings = {viewDist: 900, chunkWindow: 5, particles: 25, resolution: 0.75, showFPS: false, dayNight: true};
// Staging copy — holds uncommitted edits while settings overlay is open.
// Written to by all slider/toggle/preset interactions.
// Flushed → settings on Apply; discarded on close without Apply.
var pendingSettings = {viewDist: 900, chunkWindow: 5, particles: 25, resolution: 0.75, showFPS: false, dayNight: true};
var _settingsDirty = false; // true when pendingSettings differs from settings
var _fpsSmooth = 60;
var _lastFrameTimeMs = 0;
var _settingsClickAreas = []; // populated each frame by drawSettingsOverlay for mouse interaction

// ── Day/Night Cycle ──
var dayTime = GAME_CONFIG.dayNight.initialTime;
var daySpeed = GAME_CONFIG.dayNight.speed;
var ambientLight = 0.9;   // base brightness 0-1 (computed per frame from dayTime)
var playerUnderground = false; // true when player has cave ceiling overhead
var sunIntensity = GAME_CONFIG.dayNight.baseSunIntensity;
var sunDirX = 1, sunDirZ = 0; // sun direction (XZ plane)
var fogFloor = GAME_CONFIG.rendering.fogFloor;
// Pre-computed per-normal shade values (updated once per frame)
var _wallShadeN = GAME_CONFIG.rendering.wallShadeN, _wallShadeS = GAME_CONFIG.rendering.wallShadeS;
var _wallShadeE = GAME_CONFIG.rendering.wallShadeE, _wallShadeW = GAME_CONFIG.rendering.wallShadeW;
var _topShade = GAME_CONFIG.rendering.topShade;

// ── Point Lights ──
var pointLights = [];         // [{x, y, radius, intensity, r, g, b}]
var _lightGrid = null;        // Float32Array — additive brightness per grid cell
var _bakedLightGrid = null;   // Float32Array — static torch contributions, scaled per-frame
var _lightGridW = 0;
var _lightGridH = 0;
var _lightCellSize = GAME_CONFIG.rendering.lightCellSize;
var gridCave = null;  // Uint8Array, gridW*gridH — 1 if wall cell is in cave region
// wallFaceBase: Float32Array, gridW*gridH*4 — precomputed wall base Z per face.
// Index layout: (gy*gridW + gx)*4 + faceIdx (0=W, 1=E, 2=N, 3=S).
// Each entry is the min floor height across own-cell + 3 points along the
// neighbor edge (endpoints + midpoint, offset into the neighbor). Computed
// once per window assembly so drawWalls3D has zero per-frame sampling cost.
// Only meaningful for cells where grid[idx] !== 0.
var wallFaceBase = null;
var meshCave = null;  // Uint8Array, floorMesh.w*h — 1 if mesh cell has ceiling

var EQUIPMENT_DEFS = {
  // Armor — damage reduction
  leather_armor:  {id:'leather_armor',  slot:'armor', name:'Leather Armor',  desc:'-10% damage taken', rarity:'common',   cost:40,  damageReduction:0.10},
  chain_mail:     {id:'chain_mail',     slot:'armor', name:'Chain Mail',     desc:'-18% damage taken', rarity:'uncommon', cost:80,  damageReduction:0.18},
  plate_armor:    {id:'plate_armor',    slot:'armor', name:'Plate Armor',    desc:'-25% damage taken', rarity:'rare',     cost:140, damageReduction:0.25},
  // Hat — utility bonuses
  cloth_hood:     {id:'cloth_hood',     slot:'hat',   name:'Cloth Hood',     desc:'+3 mana/s regen',   rarity:'common',   cost:35,  manaRegen:3},
  iron_helm:      {id:'iron_helm',      slot:'hat',   name:'Iron Helm',      desc:'+2 HP/s regen',     rarity:'uncommon', cost:70,  hpRegen:2},
  wind_crown:     {id:'wind_crown',     slot:'hat',   name:'Wind Crown',     desc:'+15% move speed',   rarity:'rare',     cost:120, speedBonus:0.15},
  // Robes — spell power + arm colors
  apprentice_robes:{id:'apprentice_robes',slot:'robes',name:'Apprentice Robes',desc:'+10% spell damage',rarity:'common',  cost:45,  spellDmgBonus:0.10, manaCostReduction:0,   armColor:{deep:'#4a2868',mid:'#5c3a80',lit:'#7050a0',cuff:'#8868b8'}},
  mage_robes:     {id:'mage_robes',     slot:'robes', name:'Mage Robes',     desc:'+20% spell damage', rarity:'uncommon', cost:90,  spellDmgBonus:0.20, manaCostReduction:0,   armColor:{deep:'#1a3a68',mid:'#2a4a80',lit:'#3a60a0',cuff:'#5078b8'}},
  shadow_robes:   {id:'shadow_robes',   slot:'robes', name:'Shadow Robes',   desc:'-20% mana cost',    rarity:'rare',     cost:130, spellDmgBonus:0,    manaCostReduction:0.20,armColor:{deep:'#1a1a28',mid:'#2a2a38',lit:'#3a3a4a',cuff:'#4a4a5a'}},
  // Boots — movement abilities
  leather_boots:  {id:'leather_boots',  slot:'boots', name:'Leather Boots',  desc:'Dash (Shift)',       rarity:'common',   cost:50,  canDash:true,  canJump:false},
  winged_boots:   {id:'winged_boots',   slot:'boots', name:'Winged Boots',   desc:'Jump (Space)',       rarity:'uncommon', cost:100, canDash:false, canJump:true},
  arcane_striders:{id:'arcane_striders',slot:'boots', name:'Arcane Striders', desc:'Dash + Jump',       rarity:'rare',     cost:160, canDash:true,  canJump:true}
};
var RARITY_COLORS = {common:'#a0a0a0', uncommon:'#40a040', rare:'#6060e0', epic:'#c040ff'};

// ── Centralized 3D object scaling ──
// All 3D perspective sizes are scaled through getScale3D(tier).
// Objects are grouped by SIZE CLASS — not by type. Adjust a tier to
// uniformly resize all objects of that size class.
// Adjust SCALE_3D_GLOBAL to uniformly resize everything at once.
var SCALE_3D_GLOBAL = 1.0;  // master multiplier for all 3D object sizes
var SIZE_TIERS = {
  // ── Structures ──
  lgStructure: 1.5,   // spawner obelisks
  mdStructure: 1.25,  // shrines, chests, cave entrance pillars
  smStructure: 1.0,   // goal markers, altar runes
  // ── Floor decorations ──
  lgPlant:     1.3,   // tree stumps, fallen logs, ancient columns, stone pillars
  mdPlant:     1.0,   // ferns, mushrooms, crystals, crates, ice shards
  smPlant:     0.75,  // moss, frost, wildflowers, tall grass, rubble
  // ── Wall decorations ──
  lgWall:      1.1,   // torches, large icicles
  smWall:      0.8,   // carved runes, ore veins, moss drip
  // ── Creatures & companions ──
  creature:    1.2,   // pet companions
  // ── Pickups & collectibles ──
  lgPickup:    1.15,  // arcane tomes
  smPickup:    1.0,   // soul orbs, heart crystals, mana stars
  // ── Projectiles & effects ──
  projectile:  1.0,   // player spell projectiles, impact rings
  smProjectile:0.75,  // companion projectile orbs
  particle:    0.75,  // ambient particles (fireflies, embers, drips)
  // ── Glows & auras ──
  lgGlow:      1.2,   // shrine crystal glow, chest glow halo
  smGlow:      1.0,   // torch light radius
  // ── Text & labels ──
  lgText:      1.0,   // interactive prompts ("Press E", "[E] Challenge Arena")
  smText:      0.9,   // floating labels (shrine names, pickup names)
};
function getScale3D(tier) { return (SIZE_TIERS[tier] || 1.0) * SCALE_3D_GLOBAL; }

// Maps floor scatter item types to size tiers
var FLOOR_ITEM_TIER = {
  tree_stump:'lgPlant', fallen_log:'lgPlant', rock_cluster:'lgPlant',
  ancient_column:'lgPlant', stone_pillar:'lgPlant', ruined_wall:'lgPlant',
  crystal_cluster:'mdPlant', crate:'mdPlant', barrel:'mdPlant', skull:'mdPlant',
  mushroom:'mdPlant', fern:'mdPlant', leaf_pile:'mdPlant', ice_shard:'mdPlant',
  bookshelf_debris:'mdPlant', iron_chain:'mdPlant',
  moss_patch:'smPlant', frost_patch:'smPlant', wildflower:'smPlant', tall_grass:'smPlant',
  flat_rock:'smPlant', rubble:'smPlant',
};
// Maps wall decoration types to size tiers
var WALL_DECOR_TIER = {
  torch:'lgWall', icicle:'lgWall', vine_growth:'lgWall',
  moss_drip:'smWall', carved_rune:'smWall', ore_vein:'smWall',
};

// ── Chest Tier System ──
var CHEST_TIER_DEFS = {
  common:   {name:'Common',   weight:50, qualityMin:0.7,  qualityMax:1.0,  equipChance:0.25, goldMult:1.0,
             body:'#a06830', bodyDk:'#3a2010', bodyR:'#5a3010', bodyL:'#4a2808', edge:'#1a0c04',
             trim:'#b08040', lock:'#c8a848', lockStr:'#8a6828', lidTop:'#c89848', lidClosed:'#906828',
             glow:'#ffd740', glowA:0.25, spark:'#fff8d0'},
  uncommon: {name:'Uncommon', weight:30, qualityMin:0.85, qualityMax:1.15, equipChance:0.35, goldMult:1.5,
             body:'#8090a0', bodyDk:'#3a4550', bodyR:'#607080', bodyL:'#506070', edge:'#202830',
             trim:'#a0b0c0', lock:'#c0d0e0', lockStr:'#6080a0', lidTop:'#b0c0d0', lidClosed:'#708090',
             glow:'#40a040', glowA:0.35, spark:'#b0ffb0'},
  rare:     {name:'Rare',     weight:15, qualityMin:1.0,  qualityMax:1.3,  equipChance:0.50, goldMult:2.0,
             body:'#c0a020', bodyDk:'#6a5010', bodyR:'#a08818', bodyL:'#907810', edge:'#3a2a08',
             trim:'#e0c040', lock:'#ffe060', lockStr:'#a08020', lidTop:'#e8d050', lidClosed:'#b09030',
             glow:'#6060e0', glowA:0.45, spark:'#c0c0ff'},
  epic:     {name:'Epic',     weight:5,  qualityMin:1.0,  qualityMax:1.0,  equipChance:0, goldMult:3.0,
             body:'#6030a0', bodyDk:'#2a1050', bodyR:'#502888', bodyL:'#402078', edge:'#180840',
             trim:'#9060d0', lock:'#c080ff', lockStr:'#6030a0', lidTop:'#a070e0', lidClosed:'#7040b0',
             glow:'#c040ff', glowA:0.55, spark:'#e0a0ff'}
};
var CHEST_TIER_KEYS = ['common','uncommon','rare','epic'];
var CHEST_TIER_TOTAL_WEIGHT = 100;

// ── Relic Definitions (Epic chests only) ──
var RELIC_DEFS = {
  vampiric_orb:    {id:'vampiric_orb',    name:'Vampiric Orb',    desc:'8% lifesteal on spell damage',
                    effect:'lifesteal',        value:0.08, color:'#ff3030', accent:'#ff8080', shape:'orb'},
  phoenix_feather: {id:'phoenix_feather', name:'Phoenix Feather', desc:'Auto-revive once at full HP',
                    effect:'phoenixRevive',     value:1,    color:'#ff8020', accent:'#ffcc60', shape:'feather'},
  chrono_shard:    {id:'chrono_shard',    name:'Chrono Shard',    desc:'-25% spell cooldowns',
                    effect:'cooldownReduction', value:0.25, color:'#20e0e0', accent:'#80ffff', shape:'crystal'},
  thunder_core:    {id:'thunder_core',    name:'Thunder Core',    desc:'15% chance bonus chain lightning',
                    effect:'chainLightning',    value:0.15, color:'#ffdd00', accent:'#ffff80', shape:'sphere'},
  arcane_lens:     {id:'arcane_lens',     name:'Arcane Lens',     desc:'+30% projectile speed & range',
                    effect:'spellRange',        value:0.30, color:'#4080ff', accent:'#80b0ff', shape:'lens'},
  frost_heart:     {id:'frost_heart',     name:'Frost Heart',     desc:'Enemies that hit you get slowed 2s',
                    effect:'frostAura',         value:2000, color:'#60c0ff', accent:'#c0e8ff', shape:'heart'}
};
var RELIC_KEYS = Object.keys(RELIC_DEFS);

var enemyTypes = {
  fast:   {id:'fast',  name:'Scout',   color:'#ff9900',size:0.8,health:2,speed:55,chaseRange:450, attackWindup:280,  attackCooldown:900,  attackDamage:8 },
  normal: {id:'normal',name:'Soldier', color:'#ff4444',size:1.0,health:4,speed:35,chaseRange:400, attackWindup:480,  attackCooldown:1200, attackDamage:15},
  tank:   {id:'tank',  name:'Brute',   color:'#cc0000',size:1.4,health:8,speed:20,chaseRange:350, attackWindup:750,  attackCooldown:1800, attackDamage:25},
  wolf:   {id:'wolf',  name:'Wolf',    color:'#8888aa',size:0.9,health:3,speed:70,chaseRange:550, attackWindup:200,  attackCooldown:900,  attackDamage:12}
};

var COMPANION_DEFS = {
  slime: {
    id: 'slime', name: 'Slime Companion',
    color: '#44dd55', eyeColor: '#ffffff',
    speed: 140, followDist: 60, followIdeal: 40, teleportDist: 300,
    attackRange: 160, attackDamage: 0.8, attackCooldown: 600,
    projSpeed: 280, projLifeMs: 900,
    bounceHeight: 18, size: 1.8,
    wanderRadius: 55, wanderSpeed: 0.7,
    cost: 40, desc: 'A friendly slime that fights alongside you'
  }
};

// =============================================
// SCORING
// =============================================
var medalGold = GAME_CONFIG.scoring.medalGold, medalSilver = GAME_CONFIG.scoring.medalSilver, medalBronze = GAME_CONFIG.scoring.medalBronze;
var timeColor = '#d4af37';

// =============================================
// DEBUG FLAGS
// =============================================
var DEBUG_IMU = false;
var DEBUG_3D = false, __dbg3dLast = 0;
var DEBUG_TEXTURE = false, __dbgTexLast = 0;
var DEBUG_FLOOR = false, __dbgFloorLast = 0;
var DEBUG_FLOOR_LINE = false;  // bright red ground plane + green quad outlines to find floor line
var DEBUG_DECORATIONS = false;
var DEBUG_SKELETON = false;
var DEBUG_CAM = false, __camDbgLast = 0;
var DEBUG_COMBAT = false, __combatDbgLast = 0;
var DEBUG_PERF = true, __perfFrames = [], __perfLogInterval = 2000, __perfLastLog = 0;
var DEBUG_PERF_HUD = false;
var DEBUG_CAVE = false;
var DEBUG_CAVE_COLORS = false; // Set true to color-code floor quad categories at entrance
var DEBUG_LAYER_TYPES = false; // Set true to color floor quads by layer type (1=green, 3=yellow, 4=blue)
var DEBUG_POLY_TYPES = false;  // Set true to color every rendered polygon by what it is
// Polygon debug palette — consistent across floor/ceiling/wall render:
//   RED        cave boundary wall (grid wall flagged gridCave)
//   GRAY       surface wall (non-cave)
//   BLUE       surface cap (type 4)
//   PURPLE     cave ceiling (type 2)
//   GREEN      normal surface floor (flat)
//   YELLOW     tilted floor quad (corner Z delta >= 2u)
//   ORANGE     cave floor (type 1 under a ceiling)
var DEBUG_CEIL_WIRE = false; // Set true to draw magenta outlines on ceiling quads
var DEBUG_HIDE_CEIL = false;  // Set true to skip drawLayersCeiling3D pass (pass isolation)
var DEBUG_HIDE_WALLS = false; // Set true to skip drawWalls3D pass
var DEBUG_HIDE_PLATS = false; // Set true to skip drawLayersFloor3D (floor) pass
var __cvDbgLast = 0;
var __cvDeferLast = 0;
var __cvTransLast = 0;
// Cross-function cave debug stats. Floor loop, ceiling pass, and window.caveDiag()
// all read/write this. Reset each frame by drawFloor3D; drawCeiling3D fills its slice.
var __caveStats = {
  // Floor quad rejection histogram (counted in main quad loop)
  floorTotal: 0, floorDistCull: 0, floorFovCull: 0, floorBehind: 0,
  floorBlendRange: 0, floorOutsideBlend: 0,
  // Ceiling pipeline trace (filled by drawCeiling3D)
  ceilCollected: 0, ceilSkipLowCeil: 0, ceilSkipEntrRange: 0,
  ceilSkipViewDist: 0, ceilSkipBehind: 0, ceilSkipFov: 0, ceilRendered: 0
};
// Automated cave visibility test: cycles through distances from entrance
// Press 'T' to step through test positions. Logs rendering stats at each.
var _caveTestStep = -1;  // -1 = not testing, 0+ = current step index
var _lastWallDbg = 0;    // throttle wall collision debug logging
var _lastRectColDbg = 0; // throttle rect overlap collision debug logging
var _lastRepelDbg = 0;   // throttle repel-from-walls debug logging
var _caveTestDistances = [300, 200, 150, 100, 75, 50, 30, 15]; // world units from entrance
var _caveTestLogNext = false; // flag to log stats on next render frame
// On-demand cave diagnostics dump. Call window.caveDiag() from the console to
// get a JSON snapshot of all current cave-rendering state — entrance table,
// player position, floor/ceiling stats from last frame, config thresholds,
// and a 21×21 ASCII grid around the player. Safe to paste back to an LLM.
window.caveDiag = function() {
  var snap = window.__caveDebugLast ? JSON.parse(JSON.stringify(window.__caveDebugLast)) : null;
  var live = null;
  try {
    var _dPF = (typeof getFloorHeightAt === 'function' && typeof pos !== 'undefined') ?
      getFloorHeightAt(pos.x, pos.y) : null;
    var _dPC = 0;
    if (typeof floorMesh !== 'undefined' && floorMesh && floorMesh.layerCount) {
      var _dgx = Math.floor(pos.x / floorMesh.gridSize), _dgy = Math.floor(pos.y / floorMesh.gridSize);
      if (_dgx >= 0 && _dgx < floorMesh.w && _dgy >= 0 && _dgy < floorMesh.h) {
        var _didx = _dgy * floorMesh.w + _dgx;
        var _dlc = floorMesh.layerCount[_didx];
        if (_dlc >= 2 && floorMesh.l1Type[_didx] === 2) _dPC = floorMesh.l1TopZ[_didx];
        else if (_dlc >= 3 && floorMesh.l2Type[_didx] === 2) _dPC = floorMesh.l2TopZ[_didx];
      }
    }
    var _dEntr = [];
    if (typeof deepCaveEntrances !== 'undefined' && deepCaveEntrances) {
      for (var _di = 0; _di < deepCaveEntrances.length; _di++) {
        var _de = deepCaveEntrances[_di];
        var _ddx = pos.x - _de.x, _ddy = pos.y - _de.y;
        _dEntr.push({
          id: _di, x: +_de.x.toFixed(0), y: +_de.y.toFixed(0),
          dist: +Math.sqrt(_ddx*_ddx + _ddy*_ddy).toFixed(0),
          depth: _de.depth !== undefined ? +(+_de.depth).toFixed(2) : null,
          ceilH: _de.ceilH !== undefined ? +(+_de.ceilH).toFixed(2) : null
        });
      }
    }
    live = {
      t: Date.now(),
      player: _dPF !== null ? {
        x: +pos.x.toFixed(1), y: +pos.y.toFixed(1),
        floorH: +_dPF.toFixed(3), ceilH: +_dPC.toFixed(3),
        underground: (typeof playerUnderground !== 'undefined') ? !!playerUnderground : null
      } : null,
      entrances: _dEntr,
      lastFrameStats: (typeof __caveStats !== 'undefined') ? __caveStats : null
    };
  } catch (e) { live = { error: e.message }; }
  var out = { lastTransitionSnapshot: snap, live: live };
  console.log(JSON.stringify(out, null, 2));
  return out;
};

// Empirical wall/floor gap scanner. For each cave wall near the player, replicates
// the exact per-face baseFh that drawWalls3D uses (min of own-cell floor and
// neighbor-center floor), then samples the floor along the face's outside edge
// (at endpoints + midpoint, offset into the neighbor) to find the worst point
// where the floor renders BELOW the wall base. That's the real visible gap.
//
// Output: JSON with top N offenders sorted by worst gap. Fields per entry:
//   gx,gy,cx,cy         — wall cell grid + world center
//   wallBaseZ           — what the wall extends down to (getFloorHeightAt at center, then -0.15 extension)
//   cornerZ[4]          — floor mesh l0TopZ at wall's 4 corners (NE,SE,SW,NW)
//   neighborZ[4]        — floor mesh l0TopZ at 4 neighboring cell centers
//   worstGap            — max(wallBaseZ - adjacentZ), positive = floor below wall base → hole visible
//   ceilZ               — cave ceiling at wall center (for context)
window.caveGaps = function(maxRadius, gapThreshold, topN) {
  maxRadius = maxRadius || 200;
  gapThreshold = gapThreshold != null ? gapThreshold : 0.2;
  topN = topN || 30;
  if (typeof grid === 'undefined' || !grid || typeof floorMesh === 'undefined' || !floorMesh) {
    console.log('caveGaps: game not running'); return null;
  }
  var gs = floorMesh.gridSize;
  function floorZAt(wx, wy) {
    var gx = Math.floor(wx / gs), gy = Math.floor(wy / gs);
    if (gx < 0 || gx >= floorMesh.w || gy < 0 || gy >= floorMesh.h) return NaN;
    return floorMesh.l0TopZ[gy * floorMesh.w + gx];
  }
  function ceilZAt(wx, wy) {
    var gx = Math.floor(wx / gs), gy = Math.floor(wy / gs);
    if (gx < 0 || gx >= floorMesh.w || gy < 0 || gy >= floorMesh.h) return 0;
    var idx = gy * floorMesh.w + gx, lc = floorMesh.layerCount[idx];
    if (lc >= 2 && floorMesh.l1Type[idx] === 2) return floorMesh.l1TopZ[idx];
    if (lc >= 3 && floorMesh.l2Type[idx] === 2) return floorMesh.l2TopZ[idx];
    return 0;
  }
  var fghAt = (typeof getFloorHeightAt === 'function') ? getFloorHeightAt : floorZAt;
  var results = [];
  var rr = maxRadius * maxRadius;
  var gxMin = Math.max(0, Math.floor((pos.x - maxRadius) / cell));
  var gxMax = Math.min(gridW - 1, Math.ceil((pos.x + maxRadius) / cell));
  var gyMin = Math.max(0, Math.floor((pos.y - maxRadius) / cell));
  var gyMax = Math.min(gridH - 1, Math.ceil((pos.y + maxRadius) / cell));
  var scanned = 0, caveWallCount = 0, facesChecked = 0;
  // Face defs: dx,dy = neighbor direction; e1,e2 = endpoints relative to cell (0/1 for x1/x2, y1/y2)
  var faceDefs = [
    { dir: 'W', dx: -1, dy: 0, e1x: 0, e1y: 1, e2x: 0, e2y: 0 },
    { dir: 'E', dx: 1, dy: 0, e1x: 1, e1y: 0, e2x: 1, e2y: 1 },
    { dir: 'N', dx: 0, dy: -1, e1x: 0, e1y: 0, e2x: 1, e2y: 0 },
    { dir: 'S', dx: 0, dy: 1, e1x: 1, e1y: 1, e2x: 0, e2y: 1 }
  ];
  for (var gy = gyMin; gy <= gyMax; gy++) {
    for (var gx = gxMin; gx <= gxMax; gx++) {
      if (!grid[gy * gridW + gx]) continue;
      var cx = (gx + 0.5) * cell, cy = (gy + 0.5) * cell;
      var dxp = cx - pos.x, dyp = cy - pos.y;
      if (dxp * dxp + dyp * dyp > rr) continue;
      scanned++;
      var isCave = false;
      if (gridCave && gridCave[gy * gridW + gx]) isCave = true;
      else if (typeof isInDeepCave === 'function' && isInDeepCave(cx, cy)) isCave = true;
      else if (floorZAt(cx, cy) < -0.1) isCave = true;
      if (!isCave) continue;
      caveWallCount++;
      var fh = fghAt(cx, cy);
      var x1 = gx * cell, y1 = gy * cell;
      var x2 = (gx + 1) * cell, y2 = (gy + 1) * cell;
      // Walk each exposed face; replicate renderer's baseFh; find worst sample along face
      for (var fi = 0; fi < 4; fi++) {
        var fd = faceDefs[fi];
        var ngx = gx + fd.dx, ngy = gy + fd.dy;
        if (ngx < 0 || ngx >= gridW || ngy < 0 || ngy >= gridH) continue;
        if (grid[ngy * gridW + ngx]) continue; // face hidden by adjacent wall
        facesChecked++;
        // Renderer's actual baseFh comes from precomputed wallFaceBase (read the
        // same value drawWalls3D uses each frame). Fall back to the legacy
        // neighbor-center min if the precompute hasn't run yet.
        var nfh = fghAt((ngx + 0.5) * cell, (ngy + 0.5) * cell);
        var baseFh;
        if (typeof wallFaceBase !== 'undefined' && wallFaceBase) {
          baseFh = wallFaceBase[(gy * gridW + gx) * 4 + fi];
        } else {
          baseFh = Math.min(fh, nfh);
        }
        // Face endpoints in world coords
        var ex1 = fd.e1x ? x2 : x1, ey1 = fd.e1y ? y2 : y1;
        var ex2 = fd.e2x ? x2 : x1, ey2 = fd.e2y ? y2 : y1;
        // Sample floor along the face on the neighbor side (offset 1u into neighbor)
        var off = 1.0;
        var ox = fd.dx * off, oy = fd.dy * off;
        var mx = (ex1 + ex2) * 0.5, my = (ey1 + ey2) * 0.5;
        var s1 = fghAt(ex1 + ox, ey1 + oy);
        var s2 = fghAt(ex2 + ox, ey2 + oy);
        var sm = fghAt(mx + ox, my + oy);
        var minSample = Math.min(s1, s2, sm);
        var gap = baseFh - minSample;
        if (gap > gapThreshold) {
          results.push({
            gx: gx, gy: gy, face: fd.dir,
            cx: +cx.toFixed(1), cy: +cy.toFixed(1),
            dist: +Math.sqrt(dxp * dxp + dyp * dyp).toFixed(1),
            fh: +fh.toFixed(3),
            nfh: +nfh.toFixed(3),
            baseFh: +baseFh.toFixed(3),
            faceSamples: { e1: +s1.toFixed(3), mid: +sm.toFixed(3), e2: +s2.toFixed(3) },
            minSample: +minSample.toFixed(3),
            gap: +gap.toFixed(3),
            ceilZ: +ceilZAt(cx, cy).toFixed(3)
          });
        }
      }
    }
  }
  results.sort(function(a, b) { return b.gap - a.gap; });
  var out = {
    player: { x: +pos.x.toFixed(1), y: +pos.y.toFixed(1), underground: (typeof playerUnderground !== 'undefined') ? !!playerUnderground : null },
    params: { maxRadius: maxRadius, gapThreshold: gapThreshold, topN: topN },
    stats: { wallCellsScanned: scanned, caveWalls: caveWallCount, facesChecked: facesChecked, gapsFound: results.length },
    meshGridSize: gs,
    wallCellSize: cell,
    note: 'baseFh = renderer per-face wall base. gap = baseFh - minSample (positive = wall bottom above adjacent floor).',
    top: results.slice(0, topN)
  };
  console.log(JSON.stringify(out, null, 2));
  return out;
};

// Capture a full snapshot of the cave rendering state at the CURRENT player
// position. No teleports — user walks to the spot they want sampled, then
// clicks the button. Output is a copy-pasteable block showing player state,
// entrances, floor histogram, ceiling trace, and the ASCII grid.
window.caveCapture = function() {
  var out = document.getElementById('caveTestResults');
  var write = function(s) { if (out) out.textContent = s; console.log(s); };
  if (typeof pos === 'undefined' || !floorMesh) {
    write('caveCapture: game not running (no pos / floorMesh).'); return null;
  }
  var cs = __caveStats;
  // Player state
  var pFloorH = (typeof getFloorHeightAt === 'function') ? getFloorHeightAt(pos.x, pos.y) : 0;
  var pCeilH = 0, pGx = Math.floor(pos.x / floorMesh.gridSize), pGy = Math.floor(pos.y / floorMesh.gridSize);
  if (floorMesh.layerCount && pGx >= 0 && pGx < floorMesh.w && pGy >= 0 && pGy < floorMesh.h) {
    var _pidx = pGy * floorMesh.w + pGx;
    var _plc = floorMesh.layerCount[_pidx];
    if (_plc >= 2 && floorMesh.l1Type[_pidx] === 2) pCeilH = floorMesh.l1TopZ[_pidx];
    else if (_plc >= 3 && floorMesh.l2Type[_pidx] === 2) pCeilH = floorMesh.l2TopZ[_pidx];
  }
  // Entrance table
  var entrList = [], nearestDist = Infinity, nearestId = -1;
  if (deepCaveEntrances) {
    for (var i = 0; i < deepCaveEntrances.length; i++) {
      var e = deepCaveEntrances[i];
      var d = Math.hypot(pos.x - e.x, pos.y - e.y);
      entrList.push({ id: i, x: +e.x.toFixed(0), y: +e.y.toFixed(0), dist: +d.toFixed(0),
        depth: e.depth !== undefined ? +(+e.depth).toFixed(2) : null,
        ceilH: e.ceilH !== undefined ? +(+e.ceilH).toFixed(2) : null,
        angle: e.angle !== undefined ? +(+e.angle).toFixed(2) : null });
      if (d < nearestDist) { nearestDist = d; nearestId = i; }
    }
  }
  var ft = cs.floorTotal || 1;
  var ct = (cs.ceilCollected + cs.ceilSkipLowCeil + cs.ceilSkipEntrRange +
            cs.ceilSkipViewDist + cs.ceilSkipBehind + cs.ceilSkipFov) || 1;
  var pctS = function(n, total) { return total ? (n*100/total).toFixed(1) + '%' : '—'; };
  // ASCII grid 15x15
  var asciiRows = [];
  var aR = 7, entrSet = {};
  if (deepCaveEntrances) {
    for (var ek = 0; ek < deepCaveEntrances.length; ek++) {
      entrSet[Math.floor(deepCaveEntrances[ek].x / floorMesh.gridSize) + ',' + Math.floor(deepCaveEntrances[ek].y / floorMesh.gridSize)] = true;
    }
  }
  for (var gy = pGy - aR; gy <= pGy + aR; gy++) {
    var row = '';
    for (var gx = pGx - aR; gx <= pGx + aR; gx++) {
      if (gx === pGx && gy === pGy) { row += '@ '; continue; }
      if (entrSet[gx + ',' + gy]) { row += '* '; continue; }
      if (gx < 0 || gx >= floorMesh.w || gy < 0 || gy >= floorMesh.h) { row += '  '; continue; }
      var h = floorMesh.l0TopZ[gy * floorMesh.w + gx];
      if (h < -1.0) row += '# ';
      else if (h < -0.1) row += '~ ';
      else row += '. ';
    }
    asciiRows.push(row);
  }
  // Build snapshot object
  var snap = {
    t: new Date().toISOString(),
    seed: (typeof WORLD_SEED !== 'undefined') ? WORLD_SEED : null,
    player: {
      x: +pos.x.toFixed(1), y: +pos.y.toFixed(1),
      floorH: +pFloorH.toFixed(3), ceilH: +pCeilH.toFixed(3),
      onSurface: pFloorH > -0.5,
      underground: (typeof playerUnderground !== 'undefined') ? !!playerUnderground : null,
      nearestEntranceId: nearestId,
      nearestEntranceDist: nearestId >= 0 ? +nearestDist.toFixed(0) : null
    },
    ambient: (typeof ambientLight !== 'undefined') ? +ambientLight.toFixed(3) : null,
    entrances: entrList,
    floorHistogram: {
      total: cs.floorTotal, distCull: cs.floorDistCull, fovCull: cs.floorFovCull,
      behind: cs.floorBehind, blendRange: cs.floorBlendRange, outsideBlend: cs.floorOutsideBlend
    },
    ceilPipeline: {
      visited: ct, skipLowCeil: cs.ceilSkipLowCeil, skipEntrRange: cs.ceilSkipEntrRange,
      skipViewDist: cs.ceilSkipViewDist, skipBehind: cs.ceilSkipBehind, skipFov: cs.ceilSkipFov,
      collected: cs.ceilCollected, rendered: cs.ceilRendered
    },
    config: {
      entranceVisRadius: 14, deferMaxDist: 350, ceilEntranceDist: 200,
      undergroundEntrDist: 120, ceilBlendStart: 60, gridSize: floorMesh.gridSize
    }
  };
  // Pretty-printed text block (easy to paste)
  var lines = [];
  lines.push('═══ CAVE CAPTURE @ ' + snap.t + ' ═══');
  lines.push('seed=' + snap.seed + '  pos=(' + snap.player.x + ', ' + snap.player.y + ')');
  lines.push('floorH=' + snap.player.floorH + '  ceilH=' + snap.player.ceilH +
             '  onSurface=' + snap.player.onSurface + '  underground=' + snap.player.underground);
  lines.push('nearest entrance #' + snap.player.nearestEntranceId +
             ' at dist=' + snap.player.nearestEntranceDist +
             '  ambient=' + snap.ambient);
  lines.push('');
  lines.push('── ENTRANCES ──');
  if (entrList.length) {
    lines.push('id    x      y    dist  depth  ceilH  angle');
    for (var ei2 = 0; ei2 < entrList.length; ei2++) {
      var et = entrList[ei2];
      lines.push(String(et.id).padStart(2) + '  ' + String(et.x).padStart(5) + '  ' +
                 String(et.y).padStart(5) + '  ' + String(et.dist).padStart(4) + '  ' +
                 String(et.depth).padStart(5) + '  ' + String(et.ceilH).padStart(5) + '  ' +
                 String(et.angle).padStart(5));
    }
  } else { lines.push('(none)'); }
  lines.push('');
  lines.push('── FLOOR HISTOGRAM ──');
  lines.push('total=' + cs.floorTotal + '  distCull=' + pctS(cs.floorDistCull, ft) +
             '  fovCull=' + pctS(cs.floorFovCull, ft) + '  behind=' + pctS(cs.floorBehind, ft));
  lines.push('blendRange=' + cs.floorBlendRange + '  outsideBlend=' + cs.floorOutsideBlend);
  lines.push('');
  lines.push('── CEILING PIPELINE ──');
  lines.push('visited=' + ct + '  collected=' + cs.ceilCollected + '  rendered=' + cs.ceilRendered);
  lines.push('skipLowCeil=' + pctS(cs.ceilSkipLowCeil, ct) +
             '  skipEntrRange=' + pctS(cs.ceilSkipEntrRange, ct) +
             '  skipViewDist=' + pctS(cs.ceilSkipViewDist, ct));
  lines.push('');
  lines.push('── ASCII GRID 15x15 (@=you *=entr #=deep ~=shallow .=surface) ──');
  for (var ari = 0; ari < asciiRows.length; ari++) lines.push(asciiRows[ari]);
  lines.push('');
  lines.push('── JSON (copy to share) ──');
  lines.push(JSON.stringify(snap));
  write(lines.join('\n'));
  window.__caveCaptureLast = snap;
  return snap;
};

// Legacy teleport-based test suite (currently unused — superseded by caveCapture).
// Kept around in case the teleport approach is revisited after solving the
// chunk-reassembly drift problem. Call from console if desired.
window.caveTest = async function() {
  var out = document.getElementById('caveTestResults');
  var write = function(s) { if (out) out.textContent = s; console.log(s); };
  if (typeof deepCaveEntrances === 'undefined' || !deepCaveEntrances || deepCaveEntrances.length === 0) {
    write('caveTest: no deepCaveEntrances available — load a cave level first.'); return null;
  }
  if (typeof pos === 'undefined') { write('caveTest: pos not available.'); return null; }
  var entr = deepCaveEntrances[0];
  // Cache entrance positions — teleports cause chunk reassembly which may
  // temporarily clear deepCaveEntrances. Use cached coords for distance checks.
  var cachedEntrances = deepCaveEntrances.map(function(e){ return {x: e.x, y: e.y, angle: e.angle, depth: e.depth, ceilH: e.ceilH}; });
  if (!floorMesh) { write('caveTest: floorMesh unavailable.'); return null; }
  // Pick real points instead of blindly offsetting — scan the mesh for cells
  // at each target distance from entrance. 'minH'/'maxH' filter floor height
  // so we land on actual surface or actual cave floor, not in the slope.
  //   surface band: floorH >= 0      (hard ground)
  //   cave    band: floorH <= -1.5   (clearly underground)
  //   rim     band: -0.8..0          (transition; neither fully on nor in)
  var gs = floorMesh.gridSize;
  // pos and deepCaveEntrances coords are window-local (same space as mesh cells).
  // Scan mesh for a cell near targetDist from entrance meeting height + cave filters.
  //   requireCave: true  → must have ceilH > 0.1 (true underground cell)
  //   requireCave: false → must have ceilH < 0.05 (true surface cell)
  //   requireCave: null  → don't care (rim)
  var findCell = function(targetDist, minH, maxH, requireCave, tol) {
    var best = null, bestScore = Infinity;
    for (var my = 0; my < floorMesh.h; my++) {
      for (var mx = 0; mx < floorMesh.w; mx++) {
        var idx = my * floorMesh.w + mx;
        var h = floorMesh.l0TopZ[idx];
        if (h < minH || h > maxH) continue;
        var lcL = floorMesh.layerCount[idx];
        var ch = 0;
        if (lcL >= 2 && floorMesh.l1Type[idx] === 2) ch = floorMesh.l1TopZ[idx];
        else if (lcL >= 3 && floorMesh.l2Type[idx] === 2) ch = floorMesh.l2TopZ[idx];
        if (requireCave === true && ch <= 0.1) continue;
        if (requireCave === false && ch >= 0.05) continue;
        var wx = (mx + 0.5) * gs, wy = (my + 0.5) * gs;
        var dd = Math.hypot(wx - entr.x, wy - entr.y);
        var score = Math.abs(dd - targetDist);
        if (score < bestScore && score <= tol) { bestScore = score; best = {x: wx, y: wy, h: h, ch: ch, dist: dd}; }
      }
    }
    return best;
  };
  // Checkpoint definitions. Tolerance is loose enough to find SOMETHING on
  // typical seeds; descriptions name what we're trying to verify.
  var cpDefs = [
    { name: 'far-surface',  dist: 400, minH: 0,     maxH: 99,  cave: false, tol: 80,
      expect: { underground: false, onSurface: true } },
    { name: 'approach-200', dist: 200, minH: 0,     maxH: 99,  cave: false, tol: 80,
      expect: { underground: false, onSurface: true } },
    { name: 'rim-50',       dist: 50,  minH: -2.0,  maxH: 0.3, cave: null,  tol: 40,
      // At the rim, assert we're not underground and blendRange picks up entrance zone
      expect: { underground: false, blendRange: '>0' } },
    // playerUnderground requires entrDist > 120, so "inside" probes pick
    // cells past that threshold rather than hugging the entrance.
    { name: 'inside-150',   dist: 150, minH: -99,   maxH: -0.5, cave: true, tol: 60,
      expect: { underground: true, ceilRendered: '>0' } },
    { name: 'deep-250',     dist: 250, minH: -99,   maxH: -0.5, cave: true, tol: 100,
      expect: { underground: true, ceilRendered: '>0' } },
  ];
  var checkpoints = [];
  for (var cdi = 0; cdi < cpDefs.length; cdi++) {
    var cd = cpDefs[cdi];
    var cell = findCell(cd.dist, cd.minH, cd.maxH, cd.cave, cd.tol);
    checkpoints.push({
      name: cd.name, expect: cd.expect, target: cell,
      missReason: cell ? null : ('no ' + (cd.cave===true?'cave':cd.cave===false?'surface':'rim') +
                                 ' cell at ~' + cd.dist + 'u (tol ' + cd.tol + ')')
    });
  }
  var wait = function(ms) { return new Promise(function(r){ setTimeout(r, ms); }); };
  var savedX = pos.x, savedY = pos.y;
  var results = [];
  var passCount = 0, failCount = 0;
  var checkPred = function(val, pred) {
    if (pred === 'any' || pred === undefined) return true;
    if (typeof pred === 'number') return val === pred;
    if (pred === '=0') return val === 0 || val === false;
    if (pred === '>0') return typeof val === 'number' ? val > 0 : !!val;
    if (pred === '>=0') return typeof val === 'number' ? val >= 0 : true;
    if (pred === true) return val === true;
    if (pred === false) return val === false;
    return false;
  };
  for (var ci = 0; ci < checkpoints.length; ci++) {
    var cp = checkpoints[ci];
    if (!cp.target) {
      // Couldn't find a valid cell for this checkpoint on this seed — skip
      results.push({ name: cp.name, pass: false, checks: [{key:'setup', expect:'cell found', got:cp.missReason, pass:false}], observed: {} });
      failCount++;
      continue;
    }
    // Pin pos in a tight loop — the player-update tick will drift it otherwise.
    // Pause-via-`running=false` isn't an option; it also halts the render loop.
    pos.x = cp.target.x; pos.y = cp.target.y;
    var _pinStart = Date.now();
    while (Date.now() - _pinStart < 400) {
      pos.x = cp.target.x; pos.y = cp.target.y;
      await wait(20);
    }
    // Final pin + short settle so __caveStats / playerUnderground reflect the
    // pinned position's last render.
    pos.x = cp.target.x; pos.y = cp.target.y;
    await wait(100);
    pos.x = cp.target.x; pos.y = cp.target.y;
    // Read LIVE state directly — no reliance on the throttled transition log
    // (which may have fired on a stale frame, or not at all if far from entrance).
    var _liveFloorH = (typeof getFloorHeightAt === 'function') ? getFloorHeightAt(pos.x, pos.y) : 0;
    var _liveCeilH = 0;
    if (floorMesh && floorMesh.layerCount) {
      var _liveGx = Math.floor(pos.x / floorMesh.gridSize);
      var _liveGy = Math.floor(pos.y / floorMesh.gridSize);
      if (_liveGx >= 0 && _liveGx < floorMesh.w && _liveGy >= 0 && _liveGy < floorMesh.h) {
        var _lidx = _liveGy * floorMesh.w + _liveGx;
        var _llc = floorMesh.layerCount[_lidx];
        if (_llc >= 2 && floorMesh.l1Type[_lidx] === 2) _liveCeilH = floorMesh.l1TopZ[_lidx];
        else if (_llc >= 3 && floorMesh.l2Type[_lidx] === 2) _liveCeilH = floorMesh.l2TopZ[_lidx];
      }
    }
    // Use cached entrances — live array may be empty mid-test due to chunk rebuild
    var _liveEntrDist = Infinity;
    for (var _ldi = 0; _ldi < cachedEntrances.length; _ldi++) {
      var _ldx2 = pos.x - cachedEntrances[_ldi].x;
      var _ldy2 = pos.y - cachedEntrances[_ldi].y;
      var _ld2 = Math.sqrt(_ldx2*_ldx2 + _ldy2*_ldy2);
      if (_ld2 < _liveEntrDist) _liveEntrDist = _ld2;
    }
    var observed = {
      underground: playerUnderground,
      onSurface: _liveFloorH > -0.5,
      entrDist: Math.round(_liveEntrDist),
      floorH: +_liveFloorH.toFixed(3),
      ceilH: +_liveCeilH.toFixed(3),
      // __caveStats updates every render — these reflect the most recent frame
      blendRange: __caveStats.floorBlendRange,
      outsideBlend: __caveStats.floorOutsideBlend,
      ceilCollected: __caveStats.ceilCollected,
      ceilRendered: __caveStats.ceilRendered
    };
    var checks = [], allPass = true;
    for (var k in cp.expect) {
      var ok = checkPred(observed[k], cp.expect[k]);
      if (!ok) allPass = false;
      checks.push({ key: k, expect: cp.expect[k], got: observed[k], pass: ok });
    }
    if (allPass) passCount++; else failCount++;
    results.push({ name: cp.name, pass: allPass, checks: checks, observed: observed });
  }
  // Restore position
  pos.x = savedX; pos.y = savedY;
  __cvTransLast = 0;
  // Render summary
  var sum = 'CAVE TEST — ' + passCount + '/' + checkpoints.length + ' passed' +
            (failCount ? '  (' + failCount + ' FAIL)' : '  ALL PASS') + '\n';
  sum += '─'.repeat(60) + '\n';
  for (var ri = 0; ri < results.length; ri++) {
    var r = results[ri];
    sum += (r.pass ? '✓ ' : '✗ ') + r.name.padEnd(16);
    sum += 'floorH=' + (r.observed.floorH !== undefined && r.observed.floorH !== null ? r.observed.floorH.toFixed(2) : '?').padStart(6);
    sum += '  entrDist=' + String(r.observed.entrDist).padStart(4);
    sum += '  under=' + (r.observed.underground ? 'Y' : '.') + '\n';
    for (var ki = 0; ki < r.checks.length; ki++) {
      var c = r.checks[ki];
      sum += '    ' + (c.pass ? '  ' : '✗ ') + c.key + ' expect=' + c.expect + '  got=' + c.got + '\n';
    }
  }
  sum += '─'.repeat(60) + '\n';
  sum += 'Full snapshots available in returned object / console.';
  write(sum);
  return { passed: passCount, failed: failCount, results: results };
};

// Wire up button (after DOM is ready — script runs after the button markup)
(function() {
  var _bindBtn = function() {
    var b = document.getElementById('caveTestRunBtn');
    if (b && !b._wired) {
      b._wired = true;
      b.addEventListener('click', function() {
        window.caveCapture();
      });
    }
  };
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', _bindBtn);
  } else { _bindBtn(); }
})();
// Rolling frame-time history for sparkline (last 120 frames)
var _hudFrameTimes = new Float32Array(120);
var _hudFrameIdx = 0;
var _hudFrameCount = 0;
// Per-frame snapshot of _perfBreakdown averages for HUD display
var _hudLastBreakdown = [];
var _hudBreakdownFlush = 0; // timestamp of last breakdown capture
// All-time per-subsystem max — never cleared by dumpPerfBreakdown()
var _hudAllTimeMax = {};
// All-time frame-time max — never shrinks, survives sparkline ring-buffer rollover
var _hudAllTimeMaxFt = 0;
var DEBUG_EFFECTS = true, __effectsLastLog = 0, __effectsLogInterval = 3000;
var FW_DEBUG = false;
var __gamesLogPoll = null, __gamesLogLastLen = 0;
var __decorDebugLast = 0;

// RGB string cache — caches exact 'rgb(r,g,b)' strings by packed integer key.
// After warm-up, every call is a hash lookup with zero string allocation.
// Typical cache size: ~1000-3000 entries (walls, decorations, effects).
var _rgbCache = {};
function rgbQ(r, g, b) {
  r = Math.max(0, Math.min(255, r)) | 0;
  g = Math.max(0, Math.min(255, g)) | 0;
  b = Math.max(0, Math.min(255, b)) | 0;
  var key = (r << 16) | (g << 8) | b;
  var s = _rgbCache[key];
  if (s) { _cacheStats.rgbQ.hits++; return s; }
  s = 'rgb(' + r + ',' + g + ',' + b + ')';
  _rgbCache[key] = s;
  _cacheStats.rgbQ.misses++;
  _cacheStats.rgbQ.size++;
  return s;
}

// =============================================
// LEVEL DATA
// =============================================
var levels = [
  // Level 1 — Ground dungeon, 1440×960
  { w:1440, h:960,
    terrain:'ground',
    start:{x:120,y:120}, goal:{x:1350,y:870,w:32,h:32},
    medals:{gold:25,silver:35,bronze:50},
    maxEnemies:6,
    enemies:[{x:400,y:200,type:'fast'},{x:800,y:420,type:'normal'},{x:400,y:700,type:'fast'},
             {x:1000,y:200,type:'normal'},{x:600,y:620,type:'tank'},{x:1100,y:520,type:'fast'}],
    walls:[]
  },
  // Level 2 — Ice realm, 2160×1440
  { w:2160, h:1440,
    terrain:'ice',
    start:{x:200,y:200}, goal:{x:2000,y:1300,w:40,h:40},
    medals:{gold:40,silver:55,bronze:75},
    maxEnemies:10,
    enemies:[{x:500,y:300,type:'fast'},{x:900,y:600,type:'normal'},{x:400,y:900,type:'fast'},
             {x:1400,y:350,type:'normal'},{x:1000,y:800,type:'tank'},{x:1600,y:700,type:'fast'},
             {x:700,y:1100,type:'normal'},{x:1300,y:1000,type:'tank'},
             {x:1800,y:500,type:'fast'},{x:1500,y:1200,type:'normal'}],
    walls:[]
  },
  // Level 3 — Plains, 2160×1440
  { w:2160, h:1440,
    terrain:'plains',
    start:{x:200,y:200}, goal:{x:2000,y:1300,w:40,h:40},
    medals:{gold:45,silver:60,bronze:85},
    maxEnemies:12,
    enemies:[{x:500,y:300,type:'fast'},{x:900,y:600,type:'normal'},{x:400,y:900,type:'fast'},
             {x:1400,y:350,type:'normal'},{x:1000,y:800,type:'tank'},{x:1600,y:700,type:'fast'},
             {x:700,y:1100,type:'normal'},{x:1300,y:1000,type:'tank'},
             {x:1800,y:500,type:'fast'},{x:1500,y:1200,type:'normal'},
             {x:600,y:500,type:'fast'},{x:1700,y:900,type:'tank'}],
    walls:[]
  },
  // Level 4 — 4320×2880, The Expanse (9× area of Level 3)
  // Terrain is pinned to 'expanse' so the selector updates automatically.
  // No interior walls — the procedural border polygon and floor mesh fill the space.
  // Enemies are scattered in loose clusters across the four quadrants.
  { w:4320, h:2880,
    terrain:'expanse',
    start:{x:400,y:400}, goal:{x:4160,y:2720,w:56,h:56},
    medals:{gold:120,silver:180,bronze:260},
    maxEnemies:18,
    enemies:[
      // NW cluster
      {x:600,  y:400,  type:'fast'},   {x:900,  y:550,  type:'normal'},
      {x:750,  y:800,  type:'tank'},
      // NE cluster
      {x:3200, y:350,  type:'fast'},   {x:3500, y:600,  type:'normal'},
      {x:3800, y:500,  type:'fast'},
      // Centre swarm
      {x:2000, y:1300, type:'normal'}, {x:2200, y:1500, type:'fast'},
      {x:1800, y:1600, type:'tank'},   {x:2400, y:1200, type:'normal'},
      // SW cluster
      {x:500,  y:2200, type:'tank'},   {x:800,  y:2500, type:'fast'},
      {x:650,  y:2700, type:'normal'},
      // SE cluster
      {x:3400, y:2200, type:'fast'},   {x:3700, y:2500, type:'normal'},
      {x:3600, y:2700, type:'tank'},
      // Roaming outriders
      {x:1500, y:700,  type:'fast'},   {x:2800, y:2100, type:'normal'}
    ],
    walls:[]
  }
];
