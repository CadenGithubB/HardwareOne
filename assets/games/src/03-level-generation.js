// =============================================
// SECTION 3: LEVEL GENERATION
// =============================================

function generateCaveWalls() {
  if (terrain !== 'cave') return;
  Math.seedrandom = function(seed) {
    var m = 0x80000000, a = 1103515245, c = 12345;
    var s = seed ? seed : Math.floor(Math.random() * (m - 1));
    return function() { s = (a * s + c) % m; return s / (m - 1); };
  };
  var rng = Math.seedrandom(seedFor(1));
  var margin = 40, buffer = 35;
  function overlaps(r) {
    for (var j = 0; j < walls.length; j++) {
      var e = walls[j];
      if (!(r.x + r.w + buffer < e.x || r.x > e.x + e.w + buffer || r.y + r.h + buffer < e.y || r.y > e.y + e.h + buffer)) return true;
    }
    return false;
  }
  function blocks(r) {
    var bs = (r.x < pos.x + buffer && r.x + r.w > pos.x - buffer && r.y < pos.y + buffer && r.y + r.h > pos.y - buffer);
    var bg = goal ? (r.x < goal.x + buffer && r.x + r.w > goal.x - buffer && r.y < goal.y + buffer && r.y + r.h > goal.y - buffer) : false;
    return bs || bg;
  }
  function tryAdd(rects) {
    for (var i = 0; i < rects.length; i++) {
      var r = rects[i];
      if (r.x < margin || r.y < margin || r.x + r.w > worldW - margin || r.y + r.h > worldH - margin) return false;
      if (overlaps(r) || blocks(r)) return false;
    }
    for (var k = 0; k < rects.length; k++) walls.push(rects[k]);
    return true;
  }
  var shapes = ['rect', 'L', 'T', 'plus', 'corr_h', 'corr_v', 'zigzag', 'pillars', 'U', 'arc'];
  var targetShapes = Math.floor(6 + rng() * 6), addedRects = 0, placedShapes = 0, attempts = targetShapes * 30;
  while (placedShapes < targetShapes && attempts-- > 0) {
    var shape = shapes[Math.floor(rng() * shapes.length)];
    var cx = Math.floor(margin + rng() * (worldW - 2 * margin));
    var cy = Math.floor(margin + rng() * (worldH - 2 * margin));
    var scale = 20 + rng() * 60, rects = [];
    if (shape === 'rect') {
      var w = Math.floor(scale * 0.6 + rng() * scale), h = Math.floor(scale * 0.6 + rng() * scale);
      rects = [{x:cx, y:cy, w:w, h:h}];
    } else if (shape === 'L') {
      var arm = Math.floor(scale * 0.8), th = Math.floor(10 + rng() * 18);
      rects = [{x:cx, y:cy, w:arm, h:th}, {x:cx, y:cy, w:th, h:arm}];
    } else if (shape === 'T') {
      var arm2 = Math.floor(scale), th2 = Math.floor(10 + rng() * 18);
      rects = [{x:cx - Math.floor(arm2 / 2), y:cy, w:arm2, h:th2}, {x:cx - Math.floor(th2 / 2), y:cy, w:th2, h:arm2}];
    } else if (shape === 'plus') {
      var arm3 = Math.floor(scale * 0.8), th3 = Math.floor(10 + rng() * 16);
      rects = [{x:cx - Math.floor(arm3 / 2), y:cy - Math.floor(th3 / 2), w:arm3, h:th3}, {x:cx - Math.floor(th3 / 2), y:cy - Math.floor(arm3 / 2), w:th3, h:arm3}];
    } else if (shape === 'corr_h') {
      var len = Math.floor(scale * 1.4), gap = Math.floor(18 + rng() * 28), th4 = Math.floor(10 + rng() * 14);
      rects = [{x:cx, y:cy, w:len, h:th4}, {x:cx, y:cy + gap, w:len, h:th4}];
    } else if (shape === 'corr_v') {
      var len2 = Math.floor(scale * 1.4), gap2 = Math.floor(18 + rng() * 28), th5 = Math.floor(10 + rng() * 14);
      rects = [{x:cx, y:cy, w:th5, h:len2}, {x:cx + gap2, y:cy, w:th5, h:len2}];
    } else if (shape === 'zigzag') {
      var seg = Math.floor(14 + rng() * 18), th6 = Math.floor(10 + rng() * 14);
      rects = [
        {x:cx, y:cy, w:seg, h:th6},
        {x:cx + seg - Math.floor(th6 / 2), y:cy + seg, w:seg, h:th6},
        {x:cx + 2 * seg - Math.floor(th6), y:cy + 2 * seg, w:seg, h:th6}
      ];
    } else if (shape === 'pillars') {
      var n = 3 + Math.floor(rng() * 3), s = Math.floor(12 + rng() * 18);
      for (var i = 0; i < n; i++) {
        var ox = Math.floor((rng() - 0.5) * scale), oy = Math.floor((rng() - 0.5) * scale);
        rects.push({x:cx + ox, y:cy + oy, w:s, h:s});
      }
    } else if (shape === 'U') {
      var arm4 = Math.floor(scale), th7 = Math.floor(10 + rng() * 16), inn = Math.floor(scale * 0.5);
      rects = [{x:cx, y:cy, w:th7, h:arm4}, {x:cx + inn, y:cy, w:th7, h:arm4}, {x:cx, y:cy + arm4 - th7, w:inn + th7, h:th7}];
    } else {
      var th8 = Math.floor(10 + rng() * 16), r = Math.floor(scale * 0.8), seg2 = Math.floor(r / 2);
      rects = [
        {x:cx - r, y:cy, w:seg2, h:th8},
        {x:cx - Math.floor(seg2 / 2), y:cy + Math.floor(seg2 / 2), w:seg2, h:th8},
        {x:cx, y:cy + seg2, w:seg2, h:th8}
      ];
    }
    if (tryAdd(rects)) {
      addedRects += rects.length;
      placedShapes++;
    }
  }
  walls.caveCount = addedRects;
}

// Scatter varied obstacle structures across the open playable area of
// bordered terrains (expanse, plains). Creates visual interest, cover, and
// breaks up long sight lines in otherwise empty floors.
// Types: stone_pillar (tall 1-2 cell), ruined_wall (broken L/line),
//        rock_cluster (organic blob), ancient_column (round with rubble).
function generateOpenAreaObstacles() {
  if (!currentBorderPoly) return;
  var m2 = 0x80000000, a2 = 1103515245, c2 = 12345;
  var rs = (seedFor(13)) | 0;
  var rng = function() { rs = (a2 * rs + c2) % m2; return rs / (m2 - 1); };

  var area = worldW * worldH;
  // Scale count with map area: small ~5, large ~20
  var targetCount = Math.max(5, Math.min(22, Math.floor(area / 100000)));
  var obstacleTypes;
  if (terrain === 'ice')         obstacleTypes = ['ice_pillar', 'frozen_boulder', 'glacial_shard', 'ruined_wall'];
  else if (terrain === 'cave')   obstacleTypes = ['stone_pillar', 'rock_cluster', 'ancient_column'];
  else if (terrain === 'ground') obstacleTypes = ['barricade', 'ruined_wall', 'rock_cluster', 'stone_pillar'];
  else if (terrain === 'forest') obstacleTypes = ['rock_cluster', 'ancient_column', 'stone_pillar', 'ruined_wall'];
  else                           obstacleTypes = ['stone_pillar', 'ruined_wall', 'rock_cluster', 'ancient_column'];

  var margin = 60;
  var placed = 0, attempts = targetCount * 40;
  var obstacles = []; // track placed centers for spacing

  while (placed < targetCount && attempts-- > 0) {
    var cx = margin + rng() * (worldW - 2 * margin);
    var cy = margin + rng() * (worldH - 2 * margin);

    // Must be inside playable area (border polygon)
    if (!pointInPolygon(cx, cy, currentBorderPoly)) continue;

    // Keep away from player start position and other obstacles
    var tooClose = false;
    for (var oi = 0; oi < obstacles.length; oi++) {
      if (Math.hypot(cx - obstacles[oi].x, cy - obstacles[oi].y) < 120) { tooClose = true; break; }
    }
    if (tooClose) continue;
    // Keep away from map center (leave a clearing for player)
    if (Math.hypot(cx - worldW * 0.5, cy - worldH * 0.5) < Math.min(worldW, worldH) * 0.12) continue;

    var type = obstacleTypes[Math.floor(rng() * obstacleTypes.length)];
    var rects = [];

    if (type === 'stone_pillar') {
      // 1×1 or 2×2 tall pillar
      var sz = (rng() < 0.5) ? cell : cell * 2;
      rects.push({x: cx - sz * 0.5, y: cy - sz * 0.5, w: sz, h: sz, height: 0.7 + rng() * 0.5});
    } else if (type === 'ruined_wall') {
      // Broken wall segment — 2-4 cells in an L or line
      var th = cell;
      var len = cell * (2 + Math.floor(rng() * 3));
      var h1 = 0.25 + rng() * 0.35;
      if (rng() < 0.5) {
        // Horizontal line
        rects.push({x: cx, y: cy, w: len, h: th, height: h1});
        // Maybe an L stub
        if (rng() < 0.5) {
          var stubLen = cell * (1 + Math.floor(rng() * 2));
          rects.push({x: cx, y: cy, w: th, h: stubLen, height: h1 * (0.5 + rng() * 0.4)});
        }
      } else {
        // Vertical line
        rects.push({x: cx, y: cy, w: th, h: len, height: h1});
        if (rng() < 0.5) {
          var stubLen2 = cell * (1 + Math.floor(rng() * 2));
          rects.push({x: cx, y: cy + len - th, w: stubLen2, h: th, height: h1 * (0.5 + rng() * 0.4)});
        }
      }
    } else if (type === 'rock_cluster') {
      // 2-4 cell organic blob
      var numRocks = 2 + Math.floor(rng() * 3);
      for (var ri = 0; ri < numRocks; ri++) {
        var rox = cx + (rng() - 0.5) * cell * 2.5;
        var roy = cy + (rng() - 0.5) * cell * 2.5;
        var rsz = cell * (0.7 + rng() * 0.6);
        rects.push({x: rox - rsz * 0.5, y: roy - rsz * 0.5, w: rsz, h: rsz, height: 0.15 + rng() * 0.25});
      }
    } else if (type === 'ice_pillar') {
      // Tall ice column, slightly wider than stone_pillar
      var isz = cell * (1.2 + rng() * 0.5);
      rects.push({x: cx - isz * 0.5, y: cy - isz * 0.5, w: isz, h: isz, height: 0.8 + rng() * 0.5});
    } else if (type === 'frozen_boulder') {
      // Wide, low frozen rock — 2-3 cells
      var bw = cell * (1.5 + rng() * 1.5);
      var bh = cell * (1.2 + rng() * 1.0);
      rects.push({x: cx - bw * 0.5, y: cy - bh * 0.5, w: bw, h: bh, height: 0.2 + rng() * 0.2});
    } else if (type === 'glacial_shard') {
      // 2-3 thin tall shards clustered
      var nShards = 2 + Math.floor(rng() * 2);
      for (var gi = 0; gi < nShards; gi++) {
        var gox = cx + (rng() - 0.5) * cell * 1.5;
        var goy = cy + (rng() - 0.5) * cell * 1.5;
        var gsz = cell * (0.6 + rng() * 0.4);
        rects.push({x: gox - gsz * 0.5, y: goy - gsz * 0.5, w: gsz, h: gsz, height: 0.6 + rng() * 0.6});
      }
    } else if (type === 'barricade') {
      // Low L-shaped or straight barrier — defensive structure
      var bth = cell;
      var blen = cell * (2 + Math.floor(rng() * 2));
      var bht = 0.2 + rng() * 0.15;
      if (rng() < 0.5) {
        rects.push({x: cx, y: cy, w: blen, h: bth, height: bht});
        if (rng() < 0.6) {
          var bstub = cell * (1 + Math.floor(rng() * 2));
          rects.push({x: cx + blen - bth, y: cy, w: bth, h: bstub, height: bht * 0.8});
        }
      } else {
        rects.push({x: cx, y: cy, w: bth, h: blen, height: bht});
        if (rng() < 0.6) {
          var bstub2 = cell * (1 + Math.floor(rng() * 2));
          rects.push({x: cx, y: cy + blen - bth, w: bstub2, h: bth, height: bht * 0.8});
        }
      }
    } else {
      // ancient_column — single tall cell with very tall height
      var csz = cell * (1 + rng() * 0.3);
      rects.push({x: cx - csz * 0.5, y: cy - csz * 0.5, w: csz, h: csz, height: 0.9 + rng() * 0.4});
    }

    // Validate all rects are inside border polygon and don't overlap existing walls
    var valid = true;
    for (var vi = 0; vi < rects.length; vi++) {
      var vr = rects[vi];
      var vcx = vr.x + vr.w * 0.5, vcy = vr.y + vr.h * 0.5;
      if (!pointInPolygon(vcx, vcy, currentBorderPoly)) { valid = false; break; }
      // Check grid cells won't overlap existing walls
      var gx0 = Math.max(0, Math.floor(vr.x / cell));
      var gy0 = Math.max(0, Math.floor(vr.y / cell));
      var gx1 = Math.min(gridW - 1, Math.floor((vr.x + vr.w) / cell));
      var gy1 = Math.min(gridH - 1, Math.floor((vr.y + vr.h) / cell));
      for (var vy = gy0; vy <= gy1 && valid; vy++) {
        for (var vx = gx0; vx <= gx1 && valid; vx++) {
          if (grid[vy * gridW + vx]) valid = false;
        }
      }
    }
    if (!valid) continue;

    // Stamp into grid
    for (var si = 0; si < rects.length; si++) {
      var sr = rects[si];
      var sx0 = Math.max(0, Math.floor(sr.x / cell));
      var sy0 = Math.max(0, Math.floor(sr.y / cell));
      var sx1 = Math.min(gridW - 1, Math.floor((sr.x + sr.w) / cell));
      var sy1 = Math.min(gridH - 1, Math.floor((sr.y + sr.h) / cell));
      for (var sy = sy0; sy <= sy1; sy++) {
        for (var sx = sx0; sx <= sx1; sx++) {
          grid[sy * gridW + sx] = 1;
          wallHeights[sy * gridW + sx] = sr.height;
        }
      }
    }
    obstacles.push({x: cx, y: cy, type: type});
    placed++;
  }
  console.log('[OBSTACLES] Placed ' + placed + '/' + targetCount + ' open-area obstacles');
}

function buildGrid() {
  gridW = Math.floor(worldW / cell);
  gridH = Math.floor(worldH / cell);
  grid = new Array(gridW * gridH);
  wallHeights = new Array(gridW * gridH);
  Math.seedrandom = function(seed) {
    var m = 0x80000000, a = 1103515245, c = 12345;
    var state = seed ? seed : Math.floor(Math.random() * (m - 1));
    return function() { state = (a * state + c) % m; return state / (m - 1); };
  };
  var rng = Math.seedrandom(seedFor(2));
  for (var i = 0; i < grid.length; i++) {
    grid[i] = 0;
    wallHeights[i] = 0.7 + rng() * 0.6;
  }
  var caveWallStartIdx = walls.length;
  if (terrain === 'cave') {
    caveWallStartIdx = walls.length - (walls.caveCount || 0);
  }
  for (var wi = 0; wi < walls.length; wi++) {
    var w = walls[wi];
    var isCaveWall = (terrain === 'cave' && wi >= caveWallStartIdx);
    var heightForWall = isCaveWall ? (0.3 + rng() * 0.4) : (0.7 + rng() * 0.6);
    var x0 = Math.max(0, Math.floor(w.x / cell));
    var y0 = Math.max(0, Math.floor(w.y / cell));
    var x1 = Math.min(gridW - 1, Math.floor((w.x + w.w - 1) / cell));
    var y1 = Math.min(gridH - 1, Math.floor((w.y + w.h - 1) / cell));
    for (var y = y0; y <= y1; y++) {
      for (var x = x0; x <= x1; x++) {
        grid[y * gridW + x] = 1;
        wallHeights[y * gridW + x] = heightForWall;
      }
    }
  }
}

// ── Global noise functions (used by both level-based and endless modes) ──
var noiseSeed = 0;
function hashNoise(ix, iy) {
  var n = ix * 374761393 + iy * 668265263 + noiseSeed;
  n = (n ^ (n >> 13)) * 1274126177;
  n = n ^ (n >> 16);
  return (n & 0x7fffffff) / 0x7fffffff;
}
function smoothNoise(wx, wy, scale) {
  var fx = wx / scale, fy = wy / scale;
  var ix = Math.floor(fx), iy = Math.floor(fy);
  var tx = fx - ix, ty = fy - iy;
  var sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
  var n00 = hashNoise(ix, iy), n10 = hashNoise(ix+1, iy);
  var n01 = hashNoise(ix, iy+1), n11 = hashNoise(ix+1, iy+1);
  var nx0 = n00 + (n10 - n00) * sx;
  var nx1 = n01 + (n11 - n01) * sx;
  return nx0 + (nx1 - nx0) * sy;
}
function biomeNoise(wx, wy, scale) {
  // Separate noise using _biomeSeed for biome determination
  var oldSeed = noiseSeed;
  noiseSeed = _biomeSeed;
  var v = smoothNoise(wx, wy, scale);
  noiseSeed = oldSeed;
  return v;
}
var _ampBoost = 1.6;
var _geoSeed = 0; // set in resetEndlessMode
function geographyNoise(wx, wy) {
  var old = noiseSeed;
  noiseSeed = _geoSeed;
  var v = smoothNoise(wx, wy, 2400) * 0.5 + smoothNoise(wx, wy, 900) * 0.3 + smoothNoise(wx, wy, 350) * 0.15 + smoothNoise(wx, wy, 150) * 0.05;
  // Push values away from center for more dramatic peaks/valleys
  v = (v - 0.5) * 1.6 + 0.5; // expand contrast
  v = Math.max(0, Math.min(1, v));
  noiseSeed = old;
  return v; // 0–1 (with sharper extremes)
}
function terrainNoise(wx, wy) {
  var n = 0;
  n += (smoothNoise(wx, wy, 400) - 0.5) * 2.0 * _ampBoost;
  n += (smoothNoise(wx, wy, 160) - 0.5) * 1.2 * _ampBoost;
  n += (smoothNoise(wx, wy, 80)  - 0.5) * 0.4;
  n += (smoothNoise(wx, wy, 35)  - 0.5) * 0.1;
  return n;
}

function generateFloorMesh() {
  // Use a coarser grid for very large maps to keep generation fast.
  // Influence radii scale with world size so terrain features span the map properly.
  var gridSize = (worldW * worldH > 1500000) ? 12 : 6;
  var radiusScale = Math.max(1.0, Math.sqrt(worldW * worldH / (720 * 480)));
  var meshW = Math.ceil(worldW / gridSize);
  var meshH = Math.ceil(worldH / gridSize);
  // Layer arrays built at the end from heights/ceilH scratch
  var _nfmN = meshW * meshH;
  floorMesh = {w:meshW, h:meshH, gridSize:gridSize, heights:[], colors:[],
               ceilH: new Float32Array(_nfmN),
               LAYER_MAX: 5,
               layerCount: new Uint8Array(_nfmN),
               l0TopZ: new Float32Array(_nfmN), l0Type: new Uint8Array(_nfmN),
               l1TopZ: new Float32Array(_nfmN), l1Type: new Uint8Array(_nfmN),
               l2TopZ: new Float32Array(_nfmN), l2Type: new Uint8Array(_nfmN),
               l3TopZ: new Float32Array(_nfmN), l3Type: new Uint8Array(_nfmN),
               l4TopZ: new Float32Array(_nfmN), l4Type: new Uint8Array(_nfmN)};

  // Set noise seed and amplitude for this level
  noiseSeed = seedFor(42) | 0;
  var worldScale = Math.max(1.0, Math.sqrt(worldW * worldH / (720 * 480)));
  _ampBoost = Math.min(2.0, 0.8 + worldScale * 0.3);

  for (var y = 0; y < meshH; y++) {
    for (var x = 0; x < meshW; x++) {
      var worldX = x * gridSize;
      var worldY = y * gridSize;
      var height = 0;
      var totalWeight = 0;
      for (var i = 0; i < platforms.length; i++) {
        var p = platforms[i];
        var dx = Math.max(0, Math.max(p.x - worldX, worldX - (p.x + p.w)));
        var dy = Math.max(0, Math.max(p.y - worldY, worldY - (p.y + p.h)));
        var dist = Math.sqrt(dx * dx + dy * dy);
        var radius = ((p.heightPercent < 0) ? 200 : 80) * radiusScale;
        var t = Math.min(1, dist / radius);
        var influence = 1 - t * t * (3 - 2 * t);
        if (influence > 0) {
          height += p.heightPercent * influence;
          totalWeight += influence;
        }
      }
      if (totalWeight > 0) {
        height /= totalWeight;
      }
      // Overlay multi-octave terrain noise for natural undulation everywhere
      height += terrainNoise(worldX, worldY);
      // Deep cave overlay: force floor deep, stamp ceiling height
      // (border variations get NO override — normal terrain height/color)
      var caveData = isInDeepCave(worldX, worldY);
      if (caveData) {
        height = Math.min(height, -1.8) - 0.3 * Math.random(); // deep cave floor
        floorMesh.ceilH[y * meshW + x] = caveData.ceilZ / 25;  // store as height units
      }
      floorMesh.heights[y * meshW + x] = height;
      floorMesh.colors[y * meshW + x] = caveData ? '#2a1a0e' : getFloorColor(height);
    }
  }
  console.log('[FLOOR] Generated ' + (meshW * meshH) + ' mesh points (' + meshW + 'x' + meshH + ') gridSize=' + gridSize + (deepCaveRegions.length ? ' cave=' + deepCaveRegions.length + 'segs' : ''));

  // ── Slope shading — improve depth perception ──
  var waterArr = floorMesh.water;
  for (var sy = 1; sy < meshH - 1; sy++) {
    for (var sx = 1; sx < meshW - 1; sx++) {
      var si = sy * meshW + sx;
      if (waterArr && waterArr[si]) continue;
      var hc = floorMesh.heights[si];
      var hN = floorMesh.heights[(sy-1)*meshW+sx], hS = floorMesh.heights[(sy+1)*meshW+sx];
      var hE = floorMesh.heights[sy*meshW+sx+1], hW = floorMesh.heights[sy*meshW+sx-1];
      var slope = ((hN + hS + hE + hW) * 0.25) - hc;
      var gradX = (hE - hW) * 0.5, gradY = (hS - hN) * 0.5;
      var sunDot = (-gradX - gradY) * 0.5;
      var shadeFactor = 0;
      if (slope > 0.15) shadeFactor -= Math.min(0.12, slope * 0.10);
      else if (slope < -0.15) shadeFactor += Math.min(0.10, -slope * 0.08);
      if (sunDot > 0.1) shadeFactor += Math.min(0.14, sunDot * 0.12);
      else if (sunDot < -0.1) shadeFactor -= Math.min(0.10, -sunDot * 0.08);
      if (shadeFactor < -0.02 || shadeFactor > 0.02) {
        var col = floorMesh.colors[si];
        var cr = parseInt(col.substr(1,2),16), cg = parseInt(col.substr(3,2),16), cb = parseInt(col.substr(5,2),16);
        var mult = 1 + shadeFactor;
        cr = Math.max(0, Math.min(255, Math.floor(cr*mult)));
        cg = Math.max(0, Math.min(255, Math.floor(cg*mult)));
        cb = Math.max(0, Math.min(255, Math.floor(cb*mult)));
        floorMesh.colors[si] = '#' + ((1<<24)|(cr<<16)|(cg<<8)|cb).toString(16).slice(1);
      }
    }
  }
  // Convert heights/ceilH to layer arrays (non-endless path — endless mode
  // builds layers per-chunk). No cap generation here; non-endless maps
  // don't support the cave-hill system.
  for (var _nfI = 0; _nfI < _nfmN; _nfI++) {
    var _nfFh = floorMesh.heights[_nfI];
    var _nfCh = floorMesh.ceilH[_nfI];
    floorMesh.l0TopZ[_nfI] = _nfFh; floorMesh.l0Type[_nfI] = 1;
    if (_nfCh > 0.1) {
      floorMesh.l1TopZ[_nfI] = _nfCh; floorMesh.l1Type[_nfI] = 2;
      floorMesh.layerCount[_nfI] = 2;
    } else {
      floorMesh.layerCount[_nfI] = 1;
    }
  }
  buildWalkCandZ(floorMesh);
}

// =============================================
// WATER FEATURE GENERATION
// =============================================
// Water is now explicitly placed as two distinct feature types, fully
// independent from terrain depth so deep pits can exist without water:
//
//   mesh.water == 0  → dry terrain (including deep dark pits)
//   mesh.water == 1  → still pool  (slow/subtle glint, naturally enclosed)
//   mesh.water == 2  → flowing stream (animated shimmer, carved channel)
//
// Pools: BFS finds the single deepest connected cluster below POOL_THRESH
//   and marks it (up to MAX_POOL_CELLS).  Capped so a pool stays compact.
//
// Streams: 1–2 channels are carved with a biased random walk from one
//   quadrant of the map to the opposite one.  The walk is 70% biased
//   toward the target so channels meander naturally without looping.
//   Stream cells are carved slightly into the terrain so they sit in a
//   shallow depression visually.
function generateWaterFeatures() {
  if (!floorMesh) return;
  var mesh = floorMesh;
  mesh.water    = new Uint8Array(mesh.w * mesh.h);  // 0=none 1=pool 2=stream
  mesh.waterDirX = new Float32Array(mesh.w * mesh.h); // normalized flow direction X
  mesh.waterDirY = new Float32Array(mesh.w * mesh.h); // normalized flow direction Y

  // ---- POOLS -------------------------------------------------------
  // Large maps get more and larger pools to fill the landscape.
  var isLargeMesh = (mesh.w * mesh.h > 50000);
  var POOL_THRESH    = -1.25; // only absolute-deepest terrain qualifies
  var MAX_POOL_CELLS = isLargeMesh ? 180 : 55;
  var MAX_POOLS      = isLargeMesh ? 3   : 1;
  var visited = new Uint8Array(mesh.w * mesh.h);
  var bestCluster = null, bestAvgH = 0;

  for (var py = 2; py < mesh.h - 2; py++) {
    for (var px = 2; px < mesh.w - 2; px++) {
      var idx0 = py * mesh.w + px;
      if (visited[idx0] || mesh.heights[idx0] >= POOL_THRESH) continue;
      // BFS — collect connected deep cluster
      var cluster = [], queue = [idx0], totalH = 0;
      visited[idx0] = 1;
      while (queue.length > 0) {
        var qidx = queue.pop();
        cluster.push(qidx);
        totalH += mesh.heights[qidx];
        var qx = qidx % mesh.w, qy = Math.floor(qidx / mesh.w);
        var nbrs = [qidx - 1, qidx + 1, qidx - mesh.w, qidx + mesh.w];
        for (var ni = 0; ni < 4; ni++) {
          var nidx = nbrs[ni];
          var nx2 = nidx % mesh.w, ny2 = Math.floor(nidx / mesh.w);
          if (nx2 < 2 || ny2 < 2 || nx2 >= mesh.w - 2 || ny2 >= mesh.h - 2) continue;
          if (visited[nidx] || mesh.heights[nidx] >= POOL_THRESH) continue;
          visited[nidx] = 1;
          queue.push(nidx);
        }
      }
      if (cluster.length < 4) continue;
      var avgH = totalH / cluster.length;
      if (bestCluster === null || avgH < bestAvgH) { bestCluster = cluster; bestAvgH = avgH; }
    }
  }
  // For large maps, collect the top-N deepest distinct clusters instead of just 1
  var poolClusters = [];
  if (bestCluster) poolClusters.push(bestCluster);
  if (MAX_POOLS > 1) {
    // Re-scan visited; any unvisited qualifying cluster that doesn't overlap existing pools
    var visited2 = new Uint8Array(mesh.w * mesh.h);
    for (var py2 = 2; py2 < mesh.h - 2 && poolClusters.length < MAX_POOLS; py2++) {
      for (var px2 = 2; px2 < mesh.w - 2 && poolClusters.length < MAX_POOLS; px2++) {
        var idx2 = py2 * mesh.w + px2;
        if (visited2[idx2] || mesh.heights[idx2] >= POOL_THRESH + 0.10) continue;
        // Check spatial separation from existing pools (at least 15% of map width apart)
        var sepOK = true;
        var minSep = mesh.w * 0.15;
        for (var ep = 0; ep < poolClusters.length; ep++) {
          var repCell = poolClusters[ep][0];
          var repX = repCell % mesh.w, repY = Math.floor(repCell / mesh.w);
          if (Math.hypot(px2 - repX, py2 - repY) < minSep) { sepOK = false; break; }
        }
        if (!sepOK) { visited2[idx2] = 1; continue; }
        var cl2 = [], q2 = [idx2], th2 = 0;
        visited2[idx2] = 1;
        while (q2.length > 0) {
          var qi2 = q2.pop(); cl2.push(qi2); th2 += mesh.heights[qi2];
          var qx2 = qi2 % mesh.w, qy2 = Math.floor(qi2 / mesh.w);
          var nb2 = [qi2-1, qi2+1, qi2-mesh.w, qi2+mesh.w];
          for (var ni2 = 0; ni2 < 4; ni2++) {
            var ni2idx = nb2[ni2], nx3 = ni2idx % mesh.w, ny3 = Math.floor(ni2idx / mesh.w);
            if (nx3 < 2 || ny3 < 2 || nx3 >= mesh.w-2 || ny3 >= mesh.h-2) continue;
            if (visited2[ni2idx] || mesh.heights[ni2idx] >= POOL_THRESH + 0.10) continue;
            visited2[ni2idx] = 1; q2.push(ni2idx);
          }
        }
        if (cl2.length >= 4) poolClusters.push(cl2);
      }
    }
  }
  for (var pc = 0; pc < poolClusters.length; pc++) {
    var poolCells = poolClusters[pc].slice(0, MAX_POOL_CELLS);
    for (var ci = 0; ci < poolCells.length; ci++) mesh.water[poolCells[ci]] = 1;
  }

  // ---- STREAMS -----------------------------------------------------
  // Large maps get more streams crossing different parts of the world.
  var nStreams = isLargeMesh
    ? 3 + Math.floor(Math.random() * 3)   // 3–5 streams on big maps
    : 1 + Math.floor(Math.random() * 2);  // 1–2 streams normally
  for (var si = 0; si < nStreams; si++) {
    // Cycle through 4 diagonal patterns so streams spread across the whole map.
    // Pattern 0: TL→BR  1: TR→BL  2: BL→TR  3: BR→TL
    var sx, sy, ex, ey;
    var pat = si % 4;
    var qw = mesh.w * 0.30, qh = mesh.h * 0.30;
    if (pat === 0) {
      sx = 4 + Math.floor(Math.random() * qw);              sy = 4 + Math.floor(Math.random() * qh);
      ex = Math.floor(mesh.w * 0.65) + Math.floor(Math.random() * qw); ey = Math.floor(mesh.h * 0.65) + Math.floor(Math.random() * qh);
    } else if (pat === 1) {
      sx = Math.floor(mesh.w * 0.65) + Math.floor(Math.random() * qw); sy = 4 + Math.floor(Math.random() * qh);
      ex = 4 + Math.floor(Math.random() * qw);              ey = Math.floor(mesh.h * 0.65) + Math.floor(Math.random() * qh);
    } else if (pat === 2) {
      sx = 4 + Math.floor(Math.random() * qw);              sy = Math.floor(mesh.h * 0.65) + Math.floor(Math.random() * qh);
      ex = Math.floor(mesh.w * 0.65) + Math.floor(Math.random() * qw); ey = 4 + Math.floor(Math.random() * qh);
    } else {
      sx = Math.floor(mesh.w * 0.65) + Math.floor(Math.random() * qw); sy = Math.floor(mesh.h * 0.65) + Math.floor(Math.random() * qh);
      ex = 4 + Math.floor(Math.random() * qw);              ey = 4 + Math.floor(Math.random() * qh);
    }
    sx = Math.max(2, Math.min(mesh.w - 3, sx)); sy = Math.max(2, Math.min(mesh.h - 3, sy));
    ex = Math.max(2, Math.min(mesh.w - 3, ex)); ey = Math.max(2, Math.min(mesh.h - 3, ey));

    // Pre-compute normalized direction so every cell stores which way the stream flows
    var dLen = Math.hypot(ex - sx, ey - sy) || 1;
    var ndx = (ex - sx) / dLen, ndy = (ey - sy) / dLen;

    var scx = sx, scy = sy;
    var maxSteps = (mesh.w + mesh.h) * 3;
    for (var step = 0; step < maxSteps; step++) {
      if (scx < 1 || scy < 1 || scx >= mesh.w - 1 || scy >= mesh.h - 1) break;
      var sidx = scy * mesh.w + scx;
      mesh.water[sidx] = 2;
      mesh.waterDirX[sidx] = ndx; mesh.waterDirY[sidx] = ndy;
      // Carve stream bed slightly lower so it reads as a channel
      if (mesh.heights[sidx] > -0.55) mesh.heights[sidx] = -0.60 - Math.random() * 0.10;
      if (Math.abs(scx - ex) < 2 && Math.abs(scy - ey) < 2) break;
      // 70% biased toward target, 30% wander; advance one axis per step
      var ddx = ex - scx, ddy = ey - scy;
      var moveX = 0, moveY = 0;
      if (Math.random() < 0.5) {
        moveX = (Math.random() < 0.70) ? (ddx > 0 ? 1 : ddx < 0 ? -1 : 0)
                                        : (Math.random() < 0.5 ? 1 : -1);
      } else {
        moveY = (Math.random() < 0.70) ? (ddy > 0 ? 1 : ddy < 0 ? -1 : 0)
                                        : (Math.random() < 0.5 ? 1 : -1);
      }
      scx += moveX; scy += moveY;
    }
  }

  // ---- COLOURS -----------------------------------------------------
  var poolDeep  = (terrain === 'cave') ? '#0e1824' : '#0e1e30';
  var poolShall = (terrain === 'cave') ? '#142035' : '#152a45';
  var streamCol = (terrain === 'cave') ? '#1a2a44' : '#1a3a5a';
  var poolCount = 0, streamCount = 0;
  for (var wy3 = 0; wy3 < mesh.h; wy3++) {
    for (var wx3 = 0; wx3 < mesh.w; wx3++) {
      var wt = mesh.water[wy3 * mesh.w + wx3];
      if (wt === 1) {
        mesh.colors[wy3 * mesh.w + wx3] = (mesh.heights[wy3 * mesh.w + wx3] < -1.35) ? poolDeep : poolShall;
        poolCount++;
      } else if (wt === 2) {
        mesh.colors[wy3 * mesh.w + wx3] = streamCol;
        streamCount++;
      }
    }
  }
  console.log('[WATER] Pool: ' + poolCount + ' cells, Stream: ' + streamCount + ' cells');
}

function generateBlendedFloor() {
  platforms = [];
  var margin = Math.min(30, worldW * 0.08);
  var availableW = worldW - 2 * margin;
  var availableH = worldH - 2 * margin;
  Math.seedrandom = function(seed) {
    var m = 0x80000000, a = 1103515245, c = 12345;
    var state = seed ? seed : Math.floor(Math.random() * (m - 1));
    return function() { state = (a * state + c) % m; return state / (m - 1); };
  };
  var rng = Math.seedrandom(seedFor(0));
  // Large worlds get more platforms with bigger size ranges; the influence radius
  // scaling in generateFloorMesh means each platform covers proportionally more area.
  var worldScale = Math.max(1.0, Math.sqrt(worldW * worldH / (720 * 480)));
  var isLarge = (worldW * worldH > 1500000);
  var platformCap = isLarge ? 80 : 20;
  var platformDensity = isLarge ? 18000 : 5000; // sq-world-units per platform
  var targetPlatforms = Math.max(8, Math.min(platformCap, Math.floor(availableW * availableH / platformDensity)));
  console.log('[FLOOR] Starting blended floor generation: target=' + targetPlatforms + ', world=' + worldW + 'x' + worldH);
  var generated = 0;
  for (var i = 0; i < targetPlatforms; i++) {
    var attempts = 0;
    var placed = false;
    while (!placed && attempts < 30) {
      // Large maps: platforms can be much bigger to create sweeping regions
      var maxW = isLarge ? Math.min(600, availableW * 0.25) : Math.min(100, availableW * 0.35);
      var maxH = isLarge ? Math.min(400, availableH * 0.25) : Math.min(80,  availableH * 0.35);
      var minPW = isLarge ? 80 : 30, minPH = isLarge ? 60 : 20;
      var pw = Math.floor(minPW + rng() * maxW);
      var ph = Math.floor(minPH + rng() * maxH);
      var px = Math.floor(margin + rng() * (availableW - pw));
      var py = Math.floor(margin + rng() * (availableH - ph));
      // Bias: valleys deeper than hills (-2.5 to +1.0).
      // Combined with wider+smoothstep radius, this gives gradual descents into deep floors.
      var heightPercent = rng() * 3.5 - 2.5;
      var newPlat = {x:px, y:py, w:pw, h:ph, heightPercent:heightPercent};
      var overlaps = false;
      for (var j = 0; j < platforms.length; j++) {
        var existing = platforms[j];
        var buffer = Math.max(5, Math.min(12, (pw + ph) / 10));
        if (!(newPlat.x + newPlat.w + buffer < existing.x || newPlat.x > existing.x + existing.w + buffer ||
              newPlat.y + newPlat.h + buffer < existing.y || newPlat.y > existing.y + existing.h + buffer)) {
          overlaps = true; break;
        }
      }
      if (!overlaps) {
        platforms.push(newPlat);
        placed = true;
        generated++;
      }
      attempts++;
    }
  }
  // ── Hill generation: add prominent terrain mounds for visual interest ──
  // Hills are large positive-height platforms that create actual raised terrain.
  var hillCount = Math.max(2, Math.floor(worldScale * 2.5));
  var hillsPlaced = 0;
  for (var hi = 0; hi < hillCount; hi++) {
    var hillAttempts = 0;
    while (hillAttempts < 30) {
      var hillW = 80 + Math.floor(rng() * 200 * Math.min(worldScale, 2));
      var hillH = 60 + Math.floor(rng() * 150 * Math.min(worldScale, 2));
      var hillX = margin + Math.floor(rng() * (availableW - hillW));
      var hillY = margin + Math.floor(rng() * (availableH - hillH));
      // Hills are always positive (raised terrain), varying height
      var hillHt = 0.4 + rng() * 0.8;  // moderate to tall hills
      var hillPlat = {x:hillX, y:hillY, w:hillW, h:hillH, heightPercent:hillHt};
      // Check no overlap with existing platforms
      var hillOk = true;
      for (var hj = 0; hj < platforms.length; hj++) {
        var hp = platforms[hj];
        if (!(hillPlat.x + hillPlat.w + 20 < hp.x || hillPlat.x > hp.x + hp.w + 20 ||
              hillPlat.y + hillPlat.h + 20 < hp.y || hillPlat.y > hp.y + hp.h + 20)) {
          hillOk = false; break;
        }
      }
      if (hillOk) {
        platforms.push(hillPlat);
        hillsPlaced++;
        break;
      }
      hillAttempts++;
    }
  }
  console.log('[FLOOR] Hills: ' + hillsPlaced + '/' + hillCount + ' placed');

  console.log('[FLOOR] Pre-generating blended mesh...');
  generateFloorMesh();
  generateWaterFeatures();
  console.log('[FLOOR] Blended floor complete: ' + generated + ' platforms + ' + hillsPlaced + ' hills, mesh ready for runtime');
}

// =============================================
// IRREGULAR BORDER
// =============================================
// Generates a convex-ish polygon that approximates the world rectangle
// but with random notches bitten inward along each edge and at corners.
// Applied to the grid AFTER buildGrid() so cave/floor generation runs
// inside a simple rectangular grid, then the shape is carved out.

// Capsule test against border variation segments. Returns true/false.
function isInBorderVariation(wx, wy) {
  for (var i = 0; i < borderVariations.length; i++) {
    var r = borderVariations[i];
    var rdx = r.x2 - r.x1, rdy = r.y2 - r.y1;
    var lenSq = rdx * rdx + rdy * rdy;
    var t = lenSq > 0 ? Math.max(0, Math.min(1, ((wx - r.x1) * rdx + (wy - r.y1) * rdy) / lenSq)) : 0;
    var perpDist = Math.hypot(wx - (r.x1 + t * rdx), wy - (r.y1 + t * rdy));
    if (perpDist < r.width * 0.5) return true;
  }
  return false;
}

// Spatial grid cache for fast isInDeepCave lookups.
// Built once after cave generation; converts O(n) per-call to O(k) where k is
// the small number of segments overlapping a single grid bucket.
var _caveGrid = null;      // 2D array of lists of segment/chamber indices
var _caveGridCell = 60;    // bucket size in world pixels
var _caveGridW = 0;
var _caveGridH = 0;

function buildCaveSpatialGrid() {
  var gc = _caveGridCell;
  _caveGridW = Math.ceil(worldW / gc) + 1;
  _caveGridH = Math.ceil(worldH / gc) + 1;
  _caveGrid = new Array(_caveGridW * _caveGridH);
  for (var i = 0; i < _caveGrid.length; i++) _caveGrid[i] = null;

  // Insert corridor segments — expand bounding box by half-width
  for (var si = 0; si < deepCaveRegions.length; si++) {
    var r = deepCaveRegions[si];
    var hw = r.width * 0.5;
    var minX = Math.max(0, Math.floor((Math.min(r.x1, r.x2) - hw) / gc));
    var maxX = Math.min(_caveGridW - 1, Math.floor((Math.max(r.x1, r.x2) + hw) / gc));
    var minY = Math.max(0, Math.floor((Math.min(r.y1, r.y2) - hw) / gc));
    var maxY = Math.min(_caveGridH - 1, Math.floor((Math.max(r.y1, r.y2) + hw) / gc));
    for (var gy = minY; gy <= maxY; gy++) {
      for (var gx = minX; gx <= maxX; gx++) {
        var idx = gy * _caveGridW + gx;
        if (!_caveGrid[idx]) _caveGrid[idx] = [];
        _caveGrid[idx].push({src: 'seg', i: si});
      }
    }
  }
  // Insert chambers
  for (var ci = 0; ci < deepCaveChambers.length; ci++) {
    var ch = deepCaveChambers[ci];
    var cMinX = Math.max(0, Math.floor((ch.cx - ch.radius) / gc));
    var cMaxX = Math.min(_caveGridW - 1, Math.floor((ch.cx + ch.radius) / gc));
    var cMinY = Math.max(0, Math.floor((ch.cy - ch.radius) / gc));
    var cMaxY = Math.min(_caveGridH - 1, Math.floor((ch.cy + ch.radius) / gc));
    for (var cgy = cMinY; cgy <= cMaxY; cgy++) {
      for (var cgx = cMinX; cgx <= cMaxX; cgx++) {
        var cidx = cgy * _caveGridW + cgx;
        if (!_caveGrid[cidx]) _caveGrid[cidx] = [];
        _caveGrid[cidx].push({src: 'cham', i: ci});
      }
    }
  }
  // Count non-empty buckets for logging
  var filled = 0;
  for (var fi = 0; fi < _caveGrid.length; fi++) { if (_caveGrid[fi]) filled++; }
  console.log('[CAVE_GRID] Spatial grid: ' + _caveGridW + 'x' + _caveGridH + ' (' +
    filled + '/' + (_caveGridW * _caveGridH) + ' filled)  ' +
    deepCaveRegions.length + ' segs + ' + deepCaveChambers.length + ' chambers indexed');
}

// Capsule + circle test against deep cave regions. Returns {ceilZ, type} or null.
// Uses spatial grid for O(1) average lookup instead of scanning all segments.
function isInDeepCave(wx, wy) {
  // Fast path: spatial grid lookup
  if (_caveGrid) {
    var bgx = Math.floor(wx / _caveGridCell);
    var bgy = Math.floor(wy / _caveGridCell);
    if (bgx < 0 || bgy < 0 || bgx >= _caveGridW || bgy >= _caveGridH) return null;
    var bucket = _caveGrid[bgy * _caveGridW + bgx];
    if (!bucket) return null;
    for (var bi = 0; bi < bucket.length; bi++) {
      var entry = bucket[bi];
      if (entry.src === 'seg') {
        var r = deepCaveRegions[entry.i];
        var rdx = r.x2 - r.x1, rdy = r.y2 - r.y1;
        var lenSq = rdx * rdx + rdy * rdy;
        var t = lenSq > 0 ? Math.max(0, Math.min(1, ((wx - r.x1) * rdx + (wy - r.y1) * rdy) / lenSq)) : 0;
        var perpDist = Math.hypot(wx - (r.x1 + t * rdx), wy - (r.y1 + t * rdy));
        if (perpDist < r.width * 0.5) {
          var penetration = 1.0 - perpDist / (r.width * 0.5); // 0 at edge, 1 at center
          return {ceilZ: r.ceilZ, type: r.type, depth: penetration};
        }
      } else {
        var ch = deepCaveChambers[entry.i];
        var chDist = Math.hypot(wx - ch.cx, wy - ch.cy);
        if (chDist < ch.radius) {
          var penetration = 1.0 - chDist / ch.radius;
          return {ceilZ: ch.ceilZ, type: 'chamber', depth: penetration};
        }
      }
    }
    return null;
  }
  // Fallback: brute-force scan (used before grid is built)
  for (var i = 0; i < deepCaveRegions.length; i++) {
    var r2 = deepCaveRegions[i];
    var rdx2 = r2.x2 - r2.x1, rdy2 = r2.y2 - r2.y1;
    var lenSq2 = rdx2 * rdx2 + rdy2 * rdy2;
    var t2 = lenSq2 > 0 ? Math.max(0, Math.min(1, ((wx - r2.x1) * rdx2 + (wy - r2.y1) * rdy2) / lenSq2)) : 0;
    var perpDist2 = Math.hypot(wx - (r2.x1 + t2 * rdx2), wy - (r2.y1 + t2 * rdy2));
    if (perpDist2 < r2.width * 0.5) {
      var pen2 = 1.0 - perpDist2 / (r2.width * 0.5);
      return {ceilZ: r2.ceilZ, type: r2.type, depth: pen2};
    }
  }
  for (var j = 0; j < deepCaveChambers.length; j++) {
    var ch2 = deepCaveChambers[j];
    var chDist2 = Math.hypot(wx - ch2.cx, wy - ch2.cy);
    if (chDist2 < ch2.radius) {
      var pen3 = 1.0 - chDist2 / ch2.radius;
      return {ceilZ: ch2.ceilZ, type: 'chamber', depth: pen3};
    }
  }
  return null;
}

// Subtle border wall roughening — creates small indentations along the border
// to make walls feel organic rather than sterile. NO ceiling, NO cave semantics.
// Runs on ALL bordered terrains. Called AFTER applyIrregularBorder().
function generateBorderVariation() {
  if (!grid || !currentBorderPoly) return;
  var m2 = 0x80000000, a2 = 1103515245, c2 = 12345;
  var rs = (seedFor(7)) | 0;
  var rng = function() { rs = (a2 * rs + c2) % m2; return rs / (m2 - 1); };

  var numVeins = (worldW * worldH > 1500000) ? (4 + Math.floor(rng() * 3)) : (2 + Math.floor(rng() * 3));

  // Collect border-edge candidates: wall cells that have at least one open neighbour
  var edgeCells = [];
  var dirs4 = [[-1,0],[1,0],[0,-1],[0,1]];
  for (var gy = 1; gy < gridH - 1; gy++) {
    for (var gx = 1; gx < gridW - 1; gx++) {
      if (!grid[gy * gridW + gx]) continue;
      for (var d = 0; d < 4; d++) {
        var nx = gx + dirs4[d][0], ny = gy + dirs4[d][1];
        if (nx >= 0 && ny >= 0 && nx < gridW && ny < gridH && !grid[ny * gridW + nx]) {
          edgeCells.push({gx:gx, gy:gy}); break;
        }
      }
    }
  }
  if (!edgeCells.length) return;

  for (var v = 0; v < numVeins; v++) {
    var entrance = edgeCells[Math.floor(rng() * edgeCells.length)];
    var ex = entrance.gx * cell + cell * 0.5;
    var ey = entrance.gy * cell + cell * 0.5;

    // Direction biased toward map centre
    var toCX = worldW * 0.5 - ex, toCY = worldH * 0.5 - ey;
    var toCLen = Math.hypot(toCX, toCY) || 1;
    var dirX = toCX / toCLen, dirY = toCY / toCLen;

    var numSteps = 4 + Math.floor(rng() * 5);  // 4-8 short steps
    var stepLen  = 25 + rng() * 25;             // 25-50px per step
    var veinW    = 30 + rng() * 20;             // 30-50px wide

    // Punch small opening through border wall (radius 2 cells)
    var entrRad = 2;
    for (var edy = -entrRad; edy <= entrRad; edy++) {
      for (var edx = -entrRad; edx <= entrRad; edx++) {
        if (edx*edx + edy*edy > entrRad*entrRad) continue;
        var enx = entrance.gx + edx, eny = entrance.gy + edy;
        if (enx >= 0 && eny >= 0 && enx < gridW && eny < gridH) grid[eny * gridW + enx] = 0;
      }
    }

    var px = ex, py = ey;
    var margin = 50;

    for (var step = 0; step < numSteps; step++) {
      var jAng = (rng() - 0.5) * 1.4;
      var cosJ = Math.cos(jAng), sinJ = Math.sin(jAng);
      var jdX = dirX * cosJ - dirY * sinJ, jdY = dirX * sinJ + dirY * cosJ;
      var bdX = jdX * 0.55 + dirX * 0.45, bdY = jdY * 0.55 + dirY * 0.45;
      var bLen = Math.hypot(bdX, bdY) || 1;
      bdX /= bLen; bdY /= bLen;

      var nx2 = Math.max(margin, Math.min(worldW - margin, px + bdX * stepLen));
      var ny2 = Math.max(margin, Math.min(worldH - margin, py + bdY * stepLen));
      var segW = veinW * (0.65 + rng() * 0.7);

      // Store segment (no ceilZ — border variation, not a cave)
      borderVariations.push({x1:px, y1:py, x2:nx2, y2:ny2, width:segW});

      // Punch grid cells along the segment open
      var segLen = Math.hypot(nx2 - px, ny2 - py);
      var sdX = (nx2 - px) / (segLen || 1), sdY = (ny2 - py) / (segLen || 1);
      var punchR = Math.ceil(segW * 0.5 / cell) + 1;
      for (var t = 0; t < segLen; t += cell) {
        var swx = px + sdX * t, swy = py + sdY * t;
        var sgx2 = Math.floor(swx / cell), sgy2 = Math.floor(swy / cell);
        for (var pdy = -punchR; pdy <= punchR; pdy++) {
          for (var pdx = -punchR; pdx <= punchR; pdx++) {
            var pnx = sgx2 + pdx, pny = sgy2 + pdy;
            if (pnx < 0 || pny < 0 || pnx >= gridW || pny >= gridH) continue;
            if (isInBorderVariation(pnx * cell + cell * 0.5, pny * cell + cell * 0.5)) {
              grid[pny * gridW + pnx] = 0;
            }
          }
        }
      }
      // No branch logic — border variation is just subtle roughening
      px = nx2; py = ny2;
    }
  }
  console.log('[BORDER_VAR] Placed ' + numVeins + ' border variations, ' + borderVariations.length + ' segments');
}

// Generate deep cave tunnel networks with branching fingers and chambers.
// Only runs on expanse terrain. Uses iterative work-stack for corridor growth.
function generateDeepCaves() {
  if (!grid || !currentBorderPoly) return;
  var m2 = 0x80000000, a2 = 1103515245, c2 = 12345;
  var rs = (seedFor(11)) | 0;
  var rng = function() { rs = (a2 * rs + c2) % m2; return rs / (m2 - 1); };

  // More caves on larger maps — sprinkled around the perimeter for variety
  var worldScale = Math.max(1.0, Math.sqrt(worldW * worldH / (720 * 480)));
  // Scale cave count with map size: small maps 2-3, expanse 5-8
  var numCaves = Math.min(8, 2 + Math.floor(rng() * Math.min(6, worldScale * 1.5)));

  // Cave archetypes: each cave gets a random type for variety
  // 'narrow'  — thin single corridor, long reach, no branches
  // 'fingers' — medium width, short reach, lots of branches (spider-like)
  // 'cavern'  — wide corridors, medium reach, chambers, some branches
  var caveTypes = ['narrow', 'fingers', 'cavern'];

  // Collect border-edge candidates
  var edgeCells = [];
  var dirs4 = [[-1,0],[1,0],[0,-1],[0,1]];
  for (var gy = 1; gy < gridH - 1; gy++) {
    for (var gx = 1; gx < gridW - 1; gx++) {
      if (!grid[gy * gridW + gx]) continue;
      for (var d = 0; d < 4; d++) {
        var nx = gx + dirs4[d][0], ny = gy + dirs4[d][1];
        if (nx >= 0 && ny >= 0 && nx < gridW && ny < gridH && !grid[ny * gridW + nx]) {
          edgeCells.push({gx:gx, gy:gy}); break;
        }
      }
    }
  }
  if (!edgeCells.length) return;

  // Compute angle from map center for each edge cell, then divide the
  // perimeter into numCaves equal angular sectors so entrances spread out.
  var centerX = worldW * 0.5, centerY = worldH * 0.5;
  for (var eci = 0; eci < edgeCells.length; eci++) {
    var ecwx = edgeCells[eci].gx * cell + cell * 0.5;
    var ecwy = edgeCells[eci].gy * cell + cell * 0.5;
    edgeCells[eci].ang = Math.atan2(ecwy - centerY, ecwx - centerX);
  }
  var sectorSize = (Math.PI * 2) / numCaves;
  var sectorOffset = rng() * Math.PI * 2;

  for (var cv = 0; cv < numCaves; cv++) {
    // Pick entrance from this cave's angular sector
    var sectorCenter = sectorOffset + cv * sectorSize;
    var sectorCands = edgeCells.filter(function(ec) {
      var da = ec.ang - sectorCenter;
      // Normalize to [-PI, PI]
      while (da > Math.PI) da -= Math.PI * 2;
      while (da < -Math.PI) da += Math.PI * 2;
      return Math.abs(da) < sectorSize * 0.5;
    });
    if (!sectorCands.length) sectorCands = edgeCells;  // fallback
    var entrance = sectorCands[Math.floor(rng() * sectorCands.length)];
    var ex = entrance.gx * cell + cell * 0.5;
    var ey = entrance.gy * cell + cell * 0.5;

    // Outward direction AWAY from map centre — cave tunnels into the border walls
    var toCX = worldW * 0.5 - ex, toCY = worldH * 0.5 - ey;
    var toCLen = Math.hypot(toCX, toCY) || 1;
    var inDirX = -toCX / toCLen, inDirY = -toCY / toCLen;  // negated = outward

    // Wide entrance (radius 5 cells)
    var entrRad = 5;
    for (var edy = -entrRad; edy <= entrRad; edy++) {
      for (var edx = -entrRad; edx <= entrRad; edx++) {
        if (edx*edx + edy*edy > entrRad*entrRad) continue;
        var enx = entrance.gx + edx, eny = entrance.gy + edy;
        if (enx >= 0 && eny >= 0 && enx < gridW && eny < gridH) grid[eny * gridW + enx] = 0;
      }
    }
    deepCaveEntrances.push({x: ex, y: ey, angle: 0, cosA: 1, sinA: 0});
    console.log('[DEEP_CAVE] Entrance ' + (cv+1) + ' at world (' + Math.round(ex) + ',' + Math.round(ey) + ')');

    // ── ANTECHAMBER: a defined vestibule room between the entrance and the
    // main cave network. The main network grows from the antechamber's far
    // edge instead of the entrance mouth, so the player always steps into a
    // dedicated room before reaching the cave proper — no more walking
    // straight from the archway into a random cluster of passages.
    var ANTE_OFFSET = entrRad * cell + 70;   // center distance from entrance
    var ANTE_RADIUS = 55;                    // fixed small-room size
    var ANTE_CEIL = 85;                      // ceiling Z for this room
    var anteCX = ex + inDirX * ANTE_OFFSET;
    var anteCY = ey + inDirY * ANTE_OFFSET;
    var antePunchR = Math.ceil(ANTE_RADIUS / cell) + 1;
    var anteGX = Math.floor(anteCX / cell), anteGY = Math.floor(anteCY / cell);
    for (var _ady = -antePunchR; _ady <= antePunchR; _ady++) {
      for (var _adx = -antePunchR; _adx <= antePunchR; _adx++) {
        var _apx = anteGX + _adx, _apy = anteGY + _ady;
        if (_apx < 0 || _apy < 0 || _apx >= gridW || _apy >= gridH) continue;
        var _awx = _apx * cell + cell * 0.5, _awy = _apy * cell + cell * 0.5;
        if (Math.hypot(_awx - anteCX, _awy - anteCY) < ANTE_RADIUS) {
          grid[_apy * gridW + _apx] = 0;
        }
      }
    }
    deepCaveChambers.push({cx: anteCX, cy: anteCY, radius: ANTE_RADIUS, ceilZ: ANTE_CEIL, terminal: false, ante: true});
    // Short neck connecting entrance blob → antechamber, logged as a corridor
    // so floor mesh gets negative height + ceiling along the walk path.
    deepCaveRegions.push({x1: ex, y1: ey, x2: anteCX, y2: anteCY, width: 60, ceilZ: ANTE_CEIL, type: 'corridor'});
    var _neckLen = Math.hypot(anteCX - ex, anteCY - ey);
    var _neckDX = (anteCX - ex) / (_neckLen || 1), _neckDY = (anteCY - ey) / (_neckLen || 1);
    var _neckHalfW = 30;
    var _neckPunchR = Math.ceil(_neckHalfW / cell) + 1;
    for (var _nt = 0; _nt <= _neckLen; _nt += cell * 0.5) {
      var _nwx = ex + _neckDX * _nt, _nwy = ey + _neckDY * _nt;
      var _ngx = Math.floor(_nwx / cell), _ngy = Math.floor(_nwy / cell);
      for (var _npdy = -_neckPunchR; _npdy <= _neckPunchR; _npdy++) {
        for (var _npdx = -_neckPunchR; _npdx <= _neckPunchR; _npdx++) {
          var _npnx = _ngx + _npdx, _npny = _ngy + _npdy;
          if (_npnx < 0 || _npny < 0 || _npnx >= gridW || _npny >= gridH) continue;
          var _npwx = _npnx * cell + cell * 0.5, _npwy = _npny * cell + cell * 0.5;
          if (Math.hypot(_npwx - _nwx, _npwy - _nwy) < _neckHalfW) {
            grid[_npny * gridW + _npnx] = 0;
          }
        }
      }
    }
    // Main cave network now grows from the antechamber's FAR edge, not the entrance.
    var mainStartX = anteCX + inDirX * ANTE_RADIUS * 0.8;
    var mainStartY = anteCY + inDirY * ANTE_RADIUS * 0.8;

    // Pick a cave type for this entrance
    var caveType = caveTypes[Math.floor(rng() * caveTypes.length)];
    var mainWidth, mainCeilZ, mainSteps, branchChance, branchAngleMin, branchAngleRange, chamberChance;
    var margin = 60;

    if (caveType === 'narrow') {
      // Thin, long, no branching — a single deep tunnel
      mainWidth = (25 + rng() * 20) * Math.max(1, worldScale * 0.35);   // narrow
      mainCeilZ = 70 + rng() * 30;
      mainSteps = 18 + Math.floor(rng() * 14);  // long reach
      branchChance = 0;         // no branches
      chamberChance = 0.05;     // rare small chamber at dead end
      branchAngleMin = 0; branchAngleRange = 0;
    } else if (caveType === 'fingers') {
      // Short stubby corridors that branch aggressively — spider-like
      mainWidth = (40 + rng() * 30) * Math.max(1, worldScale * 0.4);
      mainCeilZ = 90 + rng() * 30;
      mainSteps = 5 + Math.floor(rng() * 5);    // short main
      branchChance = 0.7;       // branch aggressively
      chamberChance = 0.15;     // some chambers
      branchAngleMin = 0.6; branchAngleRange = 1.0;  // wide splay
    } else {
      // Cavern — wide corridors with chambers (the original big cave)
      mainWidth = (60 + rng() * 40) * Math.max(1, worldScale * 0.5);
      mainCeilZ = 100 + rng() * 40;
      mainSteps = 14 + Math.floor(rng() * 10);
      branchChance = 0.45;
      chamberChance = 0.25;
      branchAngleMin = 0.7; branchAngleRange = 0.8;
    }
    console.log('[DEEP_CAVE] Cave ' + (cv+1) + ' type=' + caveType + ': worldScale=' + worldScale.toFixed(1) +
                ' mainWidth=' + mainWidth.toFixed(0) + ' steps=' + mainSteps);

    var stack = [{
      cx: mainStartX, cy: mainStartY, dirX: inDirX, dirY: inDirY,
      origDirX: inDirX, origDirY: inDirY,
      width: mainWidth, ceilZ: mainCeilZ, depth: 0,
      stepsLeft: mainSteps
    }];

    while (stack.length > 0) {
      var item = stack.pop();
      var icx = item.cx, icy = item.cy;
      var idX = item.dirX, idY = item.dirY;

      for (var step = 0; step < item.stepsLeft; step++) {
        var stepLen = (40 + rng() * 30) * Math.max(1, worldScale * 0.4);  // 56-98px on expanse

        // Jitter direction, blend back toward original
        var jAng = (rng() - 0.5) * 0.8;
        var cosJ = Math.cos(jAng), sinJ = Math.sin(jAng);
        var jdX = idX * cosJ - idY * sinJ, jdY = idX * sinJ + idY * cosJ;
        var bdX = jdX * 0.7 + item.origDirX * 0.3, bdY = jdY * 0.7 + item.origDirY * 0.3;
        var bLen = Math.hypot(bdX, bdY) || 1;
        bdX /= bLen; bdY /= bLen;

        // Allow cave corridors to extend to world edges (into wall mass)
        var caveMargin = 5;
        var nx2 = Math.max(caveMargin, Math.min(worldW - caveMargin, icx + bdX * stepLen));
        var ny2 = Math.max(caveMargin, Math.min(worldH - caveMargin, icy + bdY * stepLen));

        // Taper width slightly per step
        var segW = item.width * (0.75 + rng() * 0.5);
        var segCeil = item.ceilZ * (0.85 + 0.15 * (segW / item.width));

        // Skip segments whose endpoint is inside the playable area (border polygon).
        // Caves should only exist in the wall mass, not floating over open floor.
        if (currentBorderPoly && pointInPolygon(nx2, ny2, currentBorderPoly)) {
          break;  // stop this corridor — it's wandered back into the open
        }

        // Store corridor segment
        deepCaveRegions.push({x1:icx, y1:icy, x2:nx2, y2:ny2, width:segW, ceilZ:segCeil, type:'corridor'});

        // Punch grid cells open along segment — O(1) capsule check, not O(regions)
        var segLen = Math.hypot(nx2 - icx, ny2 - icy);
        var sdX = (nx2 - icx) / (segLen || 1), sdY = (ny2 - icy) / (segLen || 1);
        var halfW = segW * 0.5;
        var punchR = Math.ceil(halfW / cell) + 1;
        var segLenSq = segLen * segLen;
        for (var t = 0; t <= segLen; t += cell * 0.5) {
          var swx = icx + sdX * t, swy = icy + sdY * t;
          var sgx = Math.floor(swx / cell), sgy = Math.floor(swy / cell);
          for (var pdy = -punchR; pdy <= punchR; pdy++) {
            for (var pdx = -punchR; pdx <= punchR; pdx++) {
              var pnx = sgx + pdx, pny = sgy + pdy;
              if (pnx < 0 || pny < 0 || pnx >= gridW || pny >= gridH) continue;
              // Capsule check: distance from cell centre to segment line
              var pwx = pnx * cell + cell * 0.5, pwy = pny * cell + cell * 0.5;
              var tx2 = segLenSq > 0 ? Math.max(0, Math.min(1, ((pwx - icx) * sdX + (pwy - icy) * sdY) / segLen)) : 0;
              var cx2 = icx + tx2 * sdX * segLen, cy2 = icy + tx2 * sdY * segLen;
              if ((pwx - cx2) * (pwx - cx2) + (pwy - cy2) * (pwy - cy2) < halfW * halfW) {
                grid[pny * gridW + pnx] = 0;
              }
            }
          }
        }

        // CHAMBER: type-dependent chance after step 2, or at corridor end
        var isTerminal = (step === item.stepsLeft - 1);
        if ((rng() < chamberChance && step > 2) || (chamberChance > 0 && isTerminal)) {
          var chamberBase = (caveType === 'narrow') ? (20 + rng() * 15) : (40 + rng() * 40);
          var chamberR = chamberBase * Math.max(1, worldScale * 0.4);
          var chamberCeil = item.ceilZ * (1.0 + rng() * 0.2);
          deepCaveChambers.push({cx:nx2, cy:ny2, radius:chamberR, ceilZ:chamberCeil, terminal:isTerminal});

          // Chambers are stored in deepCaveChambers[] — no need to also create
          // 6 capsule segments in deepCaveRegions[]. isInDeepCave() checks both
          // arrays, and ceiling heights get stamped into floorMesh.ceilH at gen time.

          // Punch chamber circle
          var chPunchR = Math.ceil(chamberR / cell) + 1;
          var chgx = Math.floor(nx2 / cell), chgy = Math.floor(ny2 / cell);
          for (var cdy = -chPunchR; cdy <= chPunchR; cdy++) {
            for (var cdx = -chPunchR; cdx <= chPunchR; cdx++) {
              var cpx = chgx + cdx, cpy = chgy + cdy;
              if (cpx < 0 || cpy < 0 || cpx >= gridW || cpy >= gridH) continue;
              var cwx = cpx * cell + cell * 0.5, cwy = cpy * cell + cell * 0.5;
              if (Math.hypot(cwx - nx2, cwy - ny2) < chamberR) {
                grid[cpy * gridW + cpx] = 0;
              }
            }
          }
        }

        // BRANCH: type-dependent chance and angle
        if (item.depth < 3 && rng() < branchChance && step > 0 && step < item.stepsLeft - 1) {
          var brAngle = (rng() < 0.5 ? 1 : -1) * (branchAngleMin + rng() * branchAngleRange);
          var brCos = Math.cos(brAngle), brSin = Math.sin(brAngle);
          var brDirX = bdX * brCos - bdY * brSin, brDirY = bdX * brSin + bdY * brCos;
          var brLen = Math.hypot(brDirX, brDirY) || 1;
          brDirX /= brLen; brDirY /= brLen;

          stack.push({
            cx: nx2, cy: ny2, dirX: brDirX, dirY: brDirY,
            origDirX: brDirX, origDirY: brDirY,
            width: segW * (0.6 + rng() * 0.2),
            ceilZ: segCeil * 0.85,
            depth: item.depth + 1,
            stepsLeft: 4 + Math.floor(rng() * 6)
          });
        }

        icx = nx2; icy = ny2;
        idX = bdX; idY = bdY;
      }
    }
  }
  // ── Debug summary ─────────────────────────────────────────────────
  console.log('[DEEP_CAVE] Generated ' + deepCaveRegions.length + ' segments, ' +
              deepCaveChambers.length + ' chambers, ' + deepCaveEntrances.length + ' entrances');
  // Log bounding box and extent of the cave system
  if (deepCaveRegions.length > 0) {
    var cavMinX = Infinity, cavMinY = Infinity, cavMaxX = -Infinity, cavMaxY = -Infinity;
    var corridorSegs = 0, chamberSegs = 0;
    for (var dci = 0; dci < deepCaveRegions.length; dci++) {
      var dcr = deepCaveRegions[dci];
      cavMinX = Math.min(cavMinX, dcr.x1, dcr.x2);
      cavMinY = Math.min(cavMinY, dcr.y1, dcr.y2);
      cavMaxX = Math.max(cavMaxX, dcr.x1, dcr.x2);
      cavMaxY = Math.max(cavMaxY, dcr.y1, dcr.y2);
      if (dcr.type === 'corridor') corridorSegs++;
      else chamberSegs++;
    }
    console.log('[DEEP_CAVE] Bounds: (' + Math.round(cavMinX) + ',' + Math.round(cavMinY) +
                ') to (' + Math.round(cavMaxX) + ',' + Math.round(cavMaxY) + ')');
    console.log('[DEEP_CAVE] Extent: ' + Math.round(cavMaxX - cavMinX) + 'px × ' +
                Math.round(cavMaxY - cavMinY) + 'px  (' + corridorSegs + ' corridor, ' + chamberSegs + ' chamber segs)');
    // Log widths
    var widths = deepCaveRegions.filter(function(r){return r.type==='corridor';}).map(function(r){return r.width;});
    if (widths.length) {
      var avgW = widths.reduce(function(a,b){return a+b;},0) / widths.length;
      console.log('[DEEP_CAVE] Corridor widths: avg=' + avgW.toFixed(1) + 'px  min=' +
                  Math.min.apply(null, widths).toFixed(1) + '  max=' + Math.max.apply(null, widths).toFixed(1));
    }
    // Count grid cells punched open for cave
    var punchedCells = 0;
    for (var pci = 0; pci < deepCaveRegions.length; pci++) {
      var pcr = deepCaveRegions[pci];
      var pcLen = Math.hypot(pcr.x2 - pcr.x1, pcr.y2 - pcr.y1);
      punchedCells += Math.ceil(pcLen / cell) * Math.ceil(pcr.width / cell);
    }
    console.log('[DEEP_CAVE] ~' + punchedCells + ' grid cells carved (approx)');
    // Log each chamber
    for (var chdi = 0; chdi < deepCaveChambers.length; chdi++) {
      var chd = deepCaveChambers[chdi];
      console.log('[DEEP_CAVE] Chamber ' + (chdi+1) + ': center=(' + Math.round(chd.cx) + ',' +
                  Math.round(chd.cy) + ') r=' + Math.round(chd.radius) + ' ceil=' + Math.round(chd.ceilZ));
    }
  }
}

// Place ore vein markers on wall cells that border a deep cave corridor.
// Requires deepCaveRegions to be populated (call after generateDeepCaves).
function generateOreVeins() {
  oreVeins = [];
  if (!deepCaveRegions.length || !grid) return;
  var m2 = 0x80000000, a2 = 1103515245, c2 = 12345;
  var rs = (seedFor(7)) | 0;
  var rng = function() { rs = (a2 * rs + c2) % m2; return rs / (m2 - 1); };
  var placed = 0;
  var usedCells = {};  // track placed cells by "gx,gy" key for O(1) dedup

  // Walk along each corridor segment. At sample points, cast outward
  // perpendicular to the corridor direction to find the cave wall edge,
  // then place ore on the first wall cell encountered.
  var corridors = deepCaveRegions.filter(function(r) { return r.type === 'corridor'; });
  for (var si = 0; si < corridors.length; si++) {
    var seg = corridors[si];
    var segLen = Math.hypot(seg.x2 - seg.x1, seg.y2 - seg.y1);
    if (segLen < 1) continue;
    var sdx = (seg.x2 - seg.x1) / segLen, sdy = (seg.y2 - seg.y1) / segLen;
    // Perpendicular directions (left and right of corridor)
    var perpLX = -sdy, perpLY = sdx;   // left normal
    var perpRX = sdy,  perpRY = -sdx;  // right normal
    var halfW = seg.width * 0.5;
    var steps = Math.ceil(segLen / (cell * 2));  // sample every ~2 cells along corridor

    for (var st = 0; st <= steps; st++) {
      var t = st / (steps || 1);
      var cx = seg.x1 + sdx * segLen * t;
      var cy = seg.y1 + sdy * segLen * t;

      // Cast outward from corridor edge in both perpendicular directions
      var perps = [[perpLX, perpLY, 'east'], [perpRX, perpRY, 'west']];
      for (var pi = 0; pi < perps.length; pi++) {
        var px = perps[pi][0], py = perps[pi][1], side = perps[pi][2];
        // Start at corridor edge (halfW out from center), scan outward
        for (var dist = halfW; dist < halfW + cell * 4; dist += cell) {
          var wx = cx + px * dist, wy = cy + py * dist;
          var gx = Math.floor(wx / cell), gy = Math.floor(wy / cell);
          if (gx < 1 || gy < 1 || gx >= gridW - 1 || gy >= gridH - 1) break;
          if (!grid[gy * gridW + gx]) continue;  // still floor, keep scanning
          // Found a wall cell at the cave edge
          if (rng() > 0.25) break;  // 25% spawn chance
          var key = gx + ',' + gy;
          if (usedCells[key]) break;
          // Keep the archway opening clear of ore veins.
          if (inEntranceReserve(wx, wy)) break;
          usedCells[key] = true;
          var veinType = rng() < 0.5 ? 'gold' : 'teal';
          // Determine which side the cave interior is on
          var faceSide = (Math.abs(px) > Math.abs(py))
            ? (px > 0 ? 'west' : 'east')
            : (py > 0 ? 'north' : 'south');
          oreVeins.push({gx:gx, gy:gy, hp:3,
                         worldX: gx * cell + cell * 0.5,
                         worldY: gy * cell + cell * 0.5,
                         side: faceSide, veinType: veinType});
          placed++;
          break;  // only one vein per ray
        }
      }
    }
  }
  console.log('[ORE] Placed ' + placed + ' ore veins in cave walls');
}

// Scatter decorative floor props (bones, crates, crystals, etc.) across open grid cells.
// Items are purely visual — no collision or interaction. Terrain-aware type selection.
function generateFloorScatter() {
  floorScatter = [];
  if (!grid || gridW < 2 || gridH < 2) return;
  var m2 = 0x80000000, a2 = 1103515245, c2 = 12345;
  var rs = (seedFor(9)) | 0;
  var rng = function() { rs = (a2 * rs + c2) % m2; return rs / (m2 - 1); };

  // Terrain-specific type pool
  var typePool;
  if (terrain === 'ice')                   typePool = ['ice_shard','frozen_pool','rubble','ice_shard','flat_rock','cracked_stone','icicle_cluster','frost_patch','frozen_skull'];
  else if (terrain === 'cave')             typePool = ['crystal','stalagmite','rock_pile','puddle','crystal','flat_rock','cracked_stone','boulder','stone_column','rock_spire','cave_rubble_pile','rock_arch','bookshelf_debris','iron_chain','barrel'];
  else if (terrain === 'plains')           typePool = ['desert_rock','dry_bones','dead_shrub','rubble','flat_rock','stick_bundle','cracked_stone','tall_grass','wildflower','stone_marker'];
  else if (terrain === 'forest')          typePool = ['tree_stump','fallen_log','tall_grass','wildflower','mushroom','moss_patch','fern','leaf_pile','tall_grass','fern'];
  else if (terrain === 'expanse')          typePool = ['desert_rock','dry_bones','dead_shrub','rubble','desert_rock','flat_rock','stick_bundle','femur','cracked_stone'];
  else                                     typePool = ['tall_grass','wildflower','moss_patch','tall_grass','wildflower','moss_patch','fern','mushroom','flat_rock','cracked_stone','stone_marker','stick_bundle','rubble','bones','skull'];

  // Collect eligible open cells
  var candidates = [];
  var goalCX = goal ? goal.x + (goal.w || 0) * 0.5 : -9999;
  var goalCY = goal ? goal.y + (goal.h || 0) * 0.5 : -9999;
  var shopX = shopMarker ? shopMarker.x : -9999, shopY = shopMarker ? shopMarker.y : -9999;
  for (var gy = 1; gy < gridH - 1; gy++) {
    for (var gx = 1; gx < gridW - 1; gx++) {
      if (grid[gy * gridW + gx] !== 0) continue;   // must be open
      var wx = gx * cell + cell * 0.5;
      var wy = gy * cell + cell * 0.5;
      // Keep away from player start, goal, and shop
      if (Math.hypot(wx - pos.x, wy - pos.y) < 70) continue;
      if (Math.hypot(wx - goalCX, wy - goalCY) < 55) continue;
      if (Math.hypot(wx - shopX,  wy - shopY)  < 50) continue;
      // Keep the cave archway opening clear of scatter (bones, stones, grass, etc.).
      if (inEntranceReserve(wx, wy)) continue;
      // For irregular-border terrains, skip cells outside the polygon
      if (currentBorderPoly && !pointInPolygon(wx, wy, currentBorderPoly)) continue;
      candidates.push({gx:gx, gy:gy, wx:wx, wy:wy});
    }
  }

  // Fisher-Yates shuffle
  for (var si = candidates.length - 1; si > 0; si--) {
    var sj = Math.floor(rng() * (si + 1));
    var tmp = candidates[si]; candidates[si] = candidates[sj]; candidates[sj] = tmp;
  }

  // ── Singles — sparse, spread across the world ─────────────────────────────
  // max(6, …) ensures even tiny maps get a handful; cap 15 keeps it sparse.
  var cavePool = ['crystal','crystal','stalagmite','rock_pile','puddle','flat_rock','cracked_stone','boulder','stone_column','rock_spire','cave_rubble_pile'];
  var chamberPool = ['crystal','crystal','crystal','stalagmite','rock_pile','cracked_stone','boulder','rock_arch','stone_column','rock_spire','cave_rubble_pile'];
  var scatterDensity = (terrain === 'ground') ? 0.045 : 0.025;
  var singleCount = Math.max(20, Math.min(80, Math.floor(candidates.length * scatterDensity)));
  for (var ci = 0; ci < singleCount && ci < candidates.length; ci++) {
    var c2s = candidates[ci];
    // Pick type: override for deep cave interiors
    var inCave = isInDeepCave(c2s.wx, c2s.wy);
    var itemType;
    if (inCave && inCave.type === 'chamber') itemType = chamberPool[Math.floor(rng() * chamberPool.length)];
    else if (inCave) itemType = cavePool[Math.floor(rng() * cavePool.length)];
    else itemType = typePool[Math.floor(rng() * typePool.length)];
    floorScatter.push({
      x: c2s.wx + (rng() - 0.5) * cell * 0.6,
      y: c2s.wy + (rng() - 0.5) * cell * 0.6,
      type: itemType,
      // Preserve the stratum that selected this clutter, independent of camera.
      underground: !!inCave,
      variant: Math.floor(rng() * 3),
      seed: rng()
    });
  }

  // ── Groups — rare clusters of 2–4 same-type items around one anchor ────────
  // Drawn from candidates not already used by singles so they stay well separated.
  var groupCount = Math.max(4, Math.min(14, Math.floor(candidates.length * (terrain === 'ground' ? 0.006 : 0.003))));
  for (var gi = 0; gi < groupCount; gi++) {
    var gcIdx = singleCount + gi;
    if (gcIdx >= candidates.length) break;
    var gc = candidates[gcIdx];
    var inCaveG = isInDeepCave(gc.wx, gc.wy);
    var groupType;
    if (inCaveG && inCaveG.type === 'chamber') groupType = chamberPool[Math.floor(rng() * chamberPool.length)];
    else if (inCaveG) groupType = cavePool[Math.floor(rng() * cavePool.length)];
    else groupType = typePool[Math.floor(rng() * typePool.length)];
    var groupSize = 2 + Math.floor(rng() * 3);   // 2, 3, or 4 items per cluster
    for (var gj = 0; gj < groupSize; gj++) {
      var gAng = rng() * Math.PI * 2;
      var gRad = 5 + rng() * 20;                 // spread within ~20px of anchor
      floorScatter.push({
        x: gc.wx + Math.cos(gAng) * gRad + (rng() - 0.5) * cell * 0.25,
        y: gc.wy + Math.sin(gAng) * gRad + (rng() - 0.5) * cell * 0.25,
        type: groupType,
        underground: !!inCaveG,
        variant: Math.floor(rng() * 3),
        seed: rng(),
        grouped: true   // cosmetic flag — renderers treat these identically to singles
      });
    }
  }
  // ── Edge scatter — items hugging walls, columns, and border transitions ────
  // Collect floor cells adjacent to at least one wall cell, then sprinkle
  // terrain-appropriate items along these edges for a natural lived-in look.
  var edgePool;
  if (terrain === 'cave')                            edgePool = ['rubble','rock_pile','boulder','rubble','cracked_stone','flat_rock','cave_rubble_pile','rock_spire'];
  else if (terrain === 'forest')                      edgePool = ['fallen_log','moss_patch','fern','tree_stump','rubble','flat_rock','leaf_pile'];
  else if (terrain === 'expanse' || terrain === 'plains') edgePool = ['rubble','desert_rock','dry_bones','rubble','dead_shrub','stick_bundle','flat_rock','cracked_stone'];
  else if (terrain === 'ice')                        edgePool = ['ice_shard','rubble','ice_shard','cracked_stone','flat_rock'];
  else                                               edgePool = ['moss_patch','flat_rock','fallen_log','moss_patch','fern','rubble','cracked_stone','stick_bundle'];

  var edgeCands = [];
  var dirs4e = [[-1,0],[1,0],[0,-1],[0,1]];
  for (var egy = 2; egy < gridH - 2; egy++) {
    for (var egx = 2; egx < gridW - 2; egx++) {
      if (grid[egy * gridW + egx] !== 0) continue;
      var adjWall = false;
      for (var ed = 0; ed < 4; ed++) {
        var enx = egx + dirs4e[ed][0], eny = egy + dirs4e[ed][1];
        if (enx >= 0 && eny >= 0 && enx < gridW && eny < gridH && grid[eny * gridW + enx]) {
          adjWall = true; break;
        }
      }
      if (!adjWall) continue;
      var ewx = egx * cell + cell * 0.5, ewy = egy * cell + cell * 0.5;
      if (Math.hypot(ewx - pos.x, ewy - pos.y) < 50) continue;
      if (inEntranceReserve(ewx, ewy)) continue;
      if (currentBorderPoly && !pointInPolygon(ewx, ewy, currentBorderPoly)) continue;
      edgeCands.push({gx:egx, gy:egy, wx:ewx, wy:ewy});
    }
  }
  // Shuffle edge candidates
  for (var esi = edgeCands.length - 1; esi > 0; esi--) {
    var esj = Math.floor(rng() * (esi + 1));
    var etmp = edgeCands[esi]; edgeCands[esi] = edgeCands[esj]; edgeCands[esj] = etmp;
  }
  // Place ~4% of edge cells
  var edgeCount = Math.max(10, Math.min(80, Math.floor(edgeCands.length * (terrain === 'ground' ? 0.07 : 0.04))));
  for (var eci2 = 0; eci2 < edgeCount && eci2 < edgeCands.length; eci2++) {
    var ec = edgeCands[eci2];
    var inCaveE = isInDeepCave(ec.wx, ec.wy);
    var eType2;
    if (inCaveE) eType2 = cavePool[Math.floor(rng() * cavePool.length)];
    else eType2 = edgePool[Math.floor(rng() * edgePool.length)];
    // Offset toward the wall for a natural hugging effect
    floorScatter.push({
      x: ec.wx + (rng() - 0.5) * cell * 0.4,
      y: ec.wy + (rng() - 0.5) * cell * 0.4,
      type: eType2,
      underground: !!inCaveE,
      variant: Math.floor(rng() * 3),
      seed: rng(),
      edge: true
    });
  }

  console.log('[SCATTER] Placed ' + floorScatter.length + ' floor items (' + singleCount + ' singles + ' + groupCount + ' groups + ' + edgeCount + ' edge, ' + terrain + ')');
}

// Pick a random equipment ID that the player doesn't already have.
// Returns null if all equipment is owned.
function pickRandomEquipment(rngFn) {
  var ids = Object.keys(EQUIPMENT_DEFS);
  // Filter out already-equipped items
  var available = [];
  for (var i = 0; i < ids.length; i++) {
    var def = EQUIPMENT_DEFS[ids[i]];
    if (equipment[def.slot] && equipment[def.slot].id === def.id) continue;
    available.push(ids[i]);
  }
  if (available.length === 0) return null;
  return available[Math.floor(rngFn() * available.length)];
}

// ── Chest Tier & Quality Utilities ──
function rollChestTier(rngFn) {
  var r = rngFn() * CHEST_TIER_TOTAL_WEIGHT;
  var cum = 0;
  for (var i = 0; i < CHEST_TIER_KEYS.length; i++) {
    cum += CHEST_TIER_DEFS[CHEST_TIER_KEYS[i]].weight;
    if (r < cum) return CHEST_TIER_KEYS[i];
  }
  return 'common';
}

function rollQuality(tierKey, rngFn) {
  var td = CHEST_TIER_DEFS[tierKey];
  return td.qualityMin + rngFn() * (td.qualityMax - td.qualityMin);
}

function getQualityPrefix(q) {
  if (q < 0.85) return 'Poor';
  if (q < 1.05) return '';
  if (q < 1.15) return 'Fine';
  return 'Superior';
}

function createEquipInstance(equipId, quality) {
  var base = EQUIPMENT_DEFS[equipId];
  if (!base) return null;
  var inst = {};
  for (var k in base) inst[k] = base[k];
  inst.quality = quality;
  // Scale numeric stats by quality
  var scaleProps = ['damageReduction','manaRegen','hpRegen','speedBonus','spellDmgBonus','manaCostReduction'];
  for (var si = 0; si < scaleProps.length; si++) {
    var p = scaleProps[si];
    if (inst[p] && typeof inst[p] === 'number' && inst[p] > 0) {
      inst[p] = Math.round(inst[p] * quality * 1000) / 1000;
    }
  }
  // Build display name with quality prefix
  var prefix = getQualityPrefix(quality);
  inst.displayName = prefix ? (prefix + ' ' + inst.name) : inst.name;
  // Regenerate desc from actual values
  inst.desc = buildEquipDesc(inst);
  return inst;
}

function buildEquipDesc(inst) {
  if (inst.damageReduction > 0) return '-' + Math.round(inst.damageReduction * 100) + '% damage taken';
  if (inst.manaRegen > 0) return '+' + (Math.round(inst.manaRegen * 10) / 10) + ' mana/s regen';
  if (inst.hpRegen > 0) return '+' + (Math.round(inst.hpRegen * 10) / 10) + ' HP/s regen';
  if (inst.speedBonus > 0) return '+' + Math.round(inst.speedBonus * 100) + '% move speed';
  if (inst.spellDmgBonus > 0) return '+' + Math.round(inst.spellDmgBonus * 100) + '% spell damage';
  if (inst.manaCostReduction > 0) return '-' + Math.round(inst.manaCostReduction * 100) + '% mana cost';
  if (inst.canDash && inst.canJump) return 'Dash + Jump';
  if (inst.canDash) return 'Dash (Shift)';
  if (inst.canJump) return 'Jump (Space)';
  return inst.name;
}

function pickRandomRelic(rngFn) {
  var available = [];
  for (var i = 0; i < RELIC_KEYS.length; i++) {
    if (equipment.relic && equipment.relic.id === RELIC_KEYS[i]) continue;
    available.push(RELIC_KEYS[i]);
  }
  if (available.length === 0) return RELIC_KEYS[Math.floor(rngFn() * RELIC_KEYS.length)];
  return available[Math.floor(rngFn() * available.length)];
}

// ── Relic On-Hit Effects ──
function applyRelicOnHit(dmg, enemy, isBonusProc) {
  if (!equipment.relic) return;
  var r = equipment.relic;
  // Vampiric Orb: lifesteal
  if (r.effect === 'lifesteal') {
    var heal = dmg * r.value;
    health = Math.min(HEALTH_MAX, health + heal);
  }
  // Thunder Core: chance for bonus chain lightning (no recursive procs)
  if (r.effect === 'chainLightning' && !isBonusProc && enemy) {
    if (Math.random() < r.value) {
      // Find nearest OTHER enemy within 120px
      var bestD = 120, bestE = null;
      for (var ri = 0; ri < enemies.length; ri++) {
        var re = enemies[ri];
        if (re === enemy || re.health <= 0) continue;
        var rd = Math.hypot(re.x - enemy.x, re.y - enemy.y);
        if (rd < bestD) { bestD = rd; bestE = re; }
      }
      if (bestE) {
        var bDmg = dmg * 0.3;
        bestE.health -= bDmg; stats.totalDamageDone += bDmg;
        bestE.flashUntil = Date.now() + 150;
        if (bestE.health <= 0) stats.totalEnemiesKilled++;
        // Visual spark
        impacts.push({x: bestE.x, y: bestE.y, z: 10, life: 0.3, maxLife: 0.3,
          color: '#ffff00', size: 6, vx: 0, vy: 0, vz: 20});
      }
    }
  }
}

function getEffectiveCooldown() {
  if (equipment.relic && equipment.relic.effect === 'cooldownReduction')
    return Math.round(SHOOT_COOLDOWN * (1 - equipment.relic.value));
  return SHOOT_COOLDOWN;
}

// Place treasure chests at terminal cave chambers (dead-ends) and enemy
// spawners in larger non-terminal chambers. Chests contain 30 gold and glow
// so they're visible from a distance. Spawners periodically emit enemies
// until destroyed.
function generateCaveInteractables() {
  treasureChests = [];
  enemySpawners = [];
  if (!deepCaveChambers.length) return;

  var m2 = 0x80000000, a2 = 1103515245, c2 = 12345;
  var rs = (seedFor(14)) | 0;
  var rng = function() { rs = (a2 * rs + c2) % m2; return rs / (m2 - 1); };

  // Minimum distance between chests so they don't cluster
  var MIN_CHEST_SPACING = 200;
  // Reject chambers too close to world edges — those are just boundary hits
  var edgeMargin = 30;

  var skippedEdge = 0, skippedSpacing = 0;

  for (var ci = 0; ci < deepCaveChambers.length; ci++) {
    var ch = deepCaveChambers[ci];

    // Skip chambers that are at the world edge (boundary terminations, not real dead-ends)
    if (ch.cx < edgeMargin || ch.cy < edgeMargin ||
        ch.cx > worldW - edgeMargin || ch.cy > worldH - edgeMargin) {
      skippedEdge++;
      continue;
    }

    if (ch.terminal) {
      // Check minimum spacing from existing chests
      var tooClose = false;
      for (var ti = 0; ti < treasureChests.length; ti++) {
        if (Math.hypot(ch.cx - treasureChests[ti].x, ch.cy - treasureChests[ti].y) < MIN_CHEST_SPACING) {
          tooClose = true; break;
        }
      }
      if (tooClose) { skippedSpacing++; continue; }

      // Small offset from center, clamped to stay well within the chamber
      var chestX = ch.cx + (rng() - 0.5) * Math.min(ch.radius * 0.3, cell);
      var chestY = ch.cy + (rng() - 0.5) * Math.min(ch.radius * 0.3, cell);
      // Verify not inside a wall
      var ccgx = Math.floor(chestX / cell), ccgy = Math.floor(chestY / cell);
      if (ccgx < 0 || ccgy < 0 || ccgx >= gridW || ccgy >= gridH || grid[ccgy * gridW + ccgx] !== 0) continue;
      var cTier = rollChestTier(rng);
      var cTierDef = CHEST_TIER_DEFS[cTier];
      var cQual = rollQuality(cTier, rng);
      var chestData = {x: chestX, y: chestY, gold: Math.round(30 * cTierDef.goldMult), collected: false,
        seed: rng(), bobPhase: rng() * Math.PI * 2,
        facing: Math.floor(rng() * 4) * Math.PI * 0.5,
        tier: cTier, quality: cQual, relicId: null, equipId: null};
      if (cTier === 'epic') {
        chestData.relicId = pickRandomRelic(rng);
        chestData.gold = 0;
      } else if (rng() < cTierDef.equipChance) {
        var eqId = pickRandomEquipment(rng);
        if (eqId) { chestData.equipId = eqId; chestData.gold = 0; }
      }
      treasureChests.push(chestData);
    } else if (ch.radius > 50 && rng() < 0.45) {
      // Larger non-terminal chambers may get an enemy spawner
      // Also check spacing from other spawners
      var spTooClose = false;
      for (var si = 0; si < enemySpawners.length; si++) {
        if (Math.hypot(ch.cx - enemySpawners[si].x, ch.cy - enemySpawners[si].y) < 150) {
          spTooClose = true; break;
        }
      }
      if (spTooClose) continue;

      enemySpawners.push({
        x: ch.cx, y: ch.cy,
        hp: 5, maxHp: 5,
        cooldownMs: 12000,    // spawn an enemy every 12s
        lastSpawn: 0,
        spawnCount: 0,
        maxSpawns: 3,         // max 3 enemies per spawner
        active: true,
        seed: rng(),
        pulsePhase: rng() * Math.PI * 2
      });
    }
  }
  console.log('[INTERACTABLES] ' + treasureChests.length + ' treasure chests, ' +
              enemySpawners.length + ' enemy spawners placed in caves' +
              ' (skipped: ' + skippedEdge + ' edge, ' + skippedSpacing + ' too close)');
}

// Place treasure chests on the surface (outside caves) for non-expanse terrains.
// Looks for dead-end niches or near obstacle clusters in the open world.
function generateSurfaceChests() {
  if (terrain === 'expanse') return;  // expanse has enough cave chests already
  if (!grid || gridW < 2 || gridH < 2) return;

  var m2 = 0x80000000, a2 = 1103515245, c2 = 12345;
  var rs = (seedFor(16)) | 0;
  var rng = function() { rs = (a2 * rs + c2) % m2; return rs / (m2 - 1); };

  // Find dead-end cells: open cells with exactly 1 open neighbor (3 wall neighbors)
  var candidates = [];
  for (var gy = 1; gy < gridH - 1; gy++) {
    for (var gx = 1; gx < gridW - 1; gx++) {
      if (grid[gy * gridW + gx] !== 0) continue;
      var wx = gx * cell + cell * 0.5;
      var wy = gy * cell + cell * 0.5;
      if (currentBorderPoly && !pointInPolygon(wx, wy, currentBorderPoly)) continue;
      // Skip if inside a cave
      if (isInDeepCave(wx, wy)) continue;
      // Count wall neighbors
      var wallN = 0;
      if (grid[(gy-1)*gridW+gx]) wallN++;
      if (grid[(gy+1)*gridW+gx]) wallN++;
      if (grid[gy*gridW+(gx-1)]) wallN++;
      if (grid[gy*gridW+(gx+1)]) wallN++;
      // Dead-end niche (3 walls) or tight corner (2 walls) get priority
      if (wallN >= 2) candidates.push({x: wx, y: wy, walls: wallN});
    }
  }

  // Sort by wall count descending (dead-ends first)
  candidates.sort(function(a, b) { return b.walls - a.walls; });

  var targetChests = Math.max(3, Math.min(5, Math.floor(candidates.length * 0.02)));
  var MIN_SPACING = 250;
  var placed = 0;

  for (var ci = 0; ci < candidates.length && placed < targetChests; ci++) {
    var c = candidates[ci];
    // Keep away from player start and goal
    if (Math.hypot(c.x - pos.x, c.y - pos.y) < 100) continue;
    if (goal && Math.hypot(c.x - (goal.x + (goal.w||0)*0.5), c.y - (goal.y + (goal.h||0)*0.5)) < 80) continue;
    // Keep away from existing chests
    var tooClose = false;
    for (var ti = 0; ti < treasureChests.length; ti++) {
      if (Math.hypot(c.x - treasureChests[ti].x, c.y - treasureChests[ti].y) < MIN_SPACING) {
        tooClose = true; break;
      }
    }
    if (tooClose) continue;

    // Place chest at cell center with small random jitter, then verify clearance
    var cx = c.x + (rng() - 0.5) * cell * 0.2;
    var cy = c.y + (rng() - 0.5) * cell * 0.2;
    // Verify the chest footprint (+/- 8px) doesn't overlap wall cells
    var chestOk = true;
    for (var dy2 = -1; dy2 <= 1; dy2++) {
      for (var dx2 = -1; dx2 <= 1; dx2++) {
        var checkX = cx + dx2 * 8, checkY = cy + dy2 * 8;
        var cgx = Math.floor(checkX / cell), cgy = Math.floor(checkY / cell);
        if (cgx < 0 || cgy < 0 || cgx >= gridW || cgy >= gridH) { chestOk = false; break; }
        if (grid[cgy * gridW + cgx] !== 0) { chestOk = false; break; }
      }
      if (!chestOk) break;
    }
    if (!chestOk) continue;

    var sTier = rollChestTier(rng);
    var sTierDef = CHEST_TIER_DEFS[sTier];
    var sQual = rollQuality(sTier, rng);
    var sChest = {
      x: cx, y: cy,
      gold: Math.round((20 + Math.floor(rng() * 15)) * sTierDef.goldMult),
      collected: false,
      seed: rng(), bobPhase: rng() * Math.PI * 2,
      facing: Math.floor(rng() * 4) * Math.PI * 0.5,
      tier: sTier, quality: sQual, relicId: null, equipId: null
    };
    if (sTier === 'epic') {
      sChest.relicId = pickRandomRelic(rng);
      sChest.gold = 0;
    } else if (rng() < sTierDef.equipChance) {
      var seqId = pickRandomEquipment(rng);
      if (seqId) { sChest.equipId = seqId; sChest.gold = 0; }
    }
    treasureChests.push(sChest);
    placed++;
  }
  if (placed > 0) console.log('[SURFACE] Placed ' + placed + ' surface treasure chests');
}

function generateBorderPolygon() {
  // Inline seedable RNG so this is deterministic per level/seed
  var m2 = 0x80000000, a2 = 1103515245, c2 = 12345;
  var rs = (seedFor(5)) | 0;
  var rng = function() { rs = (a2 * rs + c2) % m2; return rs / (m2 - 1); };

  // Scale minimum border with world size so large maps have thick wall mass for caves to carve into.
  // Tiered: large maps (min dim > 1000) get 18% border, small maps get 12%.
  // Level 1 (720x480): m≈57  Expanse (4320x2880): m≈518
  var scaleFactor = (Math.min(worldW, worldH) > 1000) ? 0.18 : 0.12;
  var m = Math.max(22, Math.floor(Math.min(worldW, worldH) * scaleFactor));
  // Max bite inward for organic edges — proportional to border thickness
  var maxBite = Math.min(Math.min(worldW, worldH) * 0.15, m * 0.5);
  var segs = 5 + Math.floor(rng() * 4);   // 5–8 segments per edge (more organic)
  var poly = [];

  // Randomise the four corners slightly so even the corners feel organic
  var tlx = m + rng()*maxBite*0.4,  tly = m + rng()*maxBite*0.4;
  var trx = worldW-m-rng()*maxBite*0.4, tryY = m + rng()*maxBite*0.4;
  var brx = worldW-m-rng()*maxBite*0.4, bry = worldH-m-rng()*maxBite*0.4;
  var blx = m + rng()*maxBite*0.4,  bly = worldH-m-rng()*maxBite*0.4;

  // Helper: walk one edge, adding 'segs-1' intermediate vertices each bitten
  // inward by a random amount in the direction (indx, indy).
  var maxBiteCorner = 1.1 * maxBite / segs;
  function addEdge(x1,y1,x2,y2,indx,indy,skipFirst) {
    if (!skipFirst) poly.push({x:x1, y:y1});
    for (var i = 1; i < segs; i++) {
      var t = i / segs;
      var bite = rng() * maxBite;
      // Secondary depth noise for rougher, more natural edges
      bite += (rng() - 0.5) * 12;
      bite = Math.max(0, bite);
      // Near-corner taper to avoid sliver crevasses
      if (i === 1 || i === segs - 1) bite = Math.min(bite, maxBiteCorner);
      poly.push({x: x1+(x2-x1)*t + indx*bite,
                 y: y1+(y2-y1)*t + indy*bite});
    }
    poly.push({x:x2, y:y2});
  }

  addEdge(tlx,tly, trx,tryY,  0, 1, false);  // top   — bites go down (+y)
  addEdge(trx,tryY,brx,bry,  -1, 0, true);   // right — bites go left (-x)
  addEdge(brx,bry, blx,bly,   0,-1, true);   // bottom— bites go up  (-y)
  addEdge(blx,bly, tlx,tly,   1, 0, true);   // left  — bites go right(+x)

  // Micro-bump post-pass: insert one vertex between each pair with small
  // perpendicular jitter to break up straight-line segments
  var refined = [];
  for (var ri = 0; ri < poly.length; ri++) {
    refined.push(poly[ri]);
    var rj = (ri + 1) % poly.length;
    var mx = (poly[ri].x + poly[rj].x) * 0.5;
    var my = (poly[ri].y + poly[rj].y) * 0.5;
    var edx = poly[rj].x - poly[ri].x, edy = poly[rj].y - poly[ri].y;
    var eLen = Math.hypot(edx, edy) || 1;
    // Perpendicular direction (normalised)
    var px = -edy / eLen, py = edx / eLen;
    var jitter = (rng() - 0.5) * 10;
    refined.push({x: mx + px * jitter, y: my + py * jitter});
  }

  return refined;
}

// Standard ray-casting point-in-polygon test
function pointInPolygon(px, py, poly) {
  var inside = false;
  for (var i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    var xi = poly[i].x, yi = poly[i].y, xj = poly[j].x, yj = poly[j].y;
    if (((yi > py) !== (yj > py)) && (px < (xj-xi)*(py-yi)/(yj-yi)+xi))
      inside = !inside;
  }
  return inside;
}

// Draw the irregular border polygon outline in the 2D minimap view.
function drawIrregularBorder2D() {
  if (!currentBorderPoly || currentBorderPoly.length < 3) return;
  ctx.save();
  ctx.strokeStyle = '#aaaaaa';
  ctx.lineWidth = 2;
  ctx.setLineDash([6, 4]);
  ctx.beginPath();
  ctx.moveTo(currentBorderPoly[0].x, currentBorderPoly[0].y);
  for (var pi = 1; pi < currentBorderPoly.length; pi++) {
    ctx.lineTo(currentBorderPoly[pi].x, currentBorderPoly[pi].y);
  }
  ctx.closePath();
  ctx.stroke();
  ctx.setLineDash([]);
  ctx.restore();
}

// BFS from (targetX,targetY) to nearest position where isInGridWall returns false.
// Uses the same 6-unit player radius so the spawn is guaranteed collision-free.
function findSafeSpawn(targetX, targetY) {
  if (!grid) return {x: targetX, y: targetY};
  var tgx = Math.floor(targetX / cell), tgy = Math.floor(targetY / cell);
  tgx = Math.max(0, Math.min(gridW - 1, tgx));
  tgy = Math.max(0, Math.min(gridH - 1, tgy));
  var tx = tgx * cell + cell * 0.5, ty = tgy * cell + cell * 0.5;
  if (!isInGridWall(tx, ty, 6)) return {x: tx, y: ty};
  var visited = new Uint8Array(gridW * gridH);
  var queue = [{gx: tgx, gy: tgy}];
  visited[tgy * gridW + tgx] = 1;
  var dirs = [[-1,0],[1,0],[0,-1],[0,1]];
  while (queue.length > 0) {
    var cur = queue.shift();
    var nx, ny, nidx, d;
    for (d = 0; d < 4; d++) {
      nx = cur.gx + dirs[d][0]; ny = cur.gy + dirs[d][1];
      if (nx < 0 || ny < 0 || nx >= gridW || ny >= gridH) continue;
      nidx = ny * gridW + nx;
      if (visited[nidx]) continue;
      visited[nidx] = 1;
      var cx2 = nx * cell + cell * 0.5, cy2 = ny * cell + cell * 0.5;
      if (!isInGridWall(cx2, cy2, 6)) return {x: cx2, y: cy2};
      queue.push({gx: nx, gy: ny});
    }
  }
  return {x: targetX, y: targetY};
}

// Find a random walkable spawn position inside the border polygon.
// Tries random sampling first (fast), falls back to full grid scan.
function findRandomOpenSpawn(poly) {
  if (!grid || !poly || poly.length < 3) return findSafeSpawn(worldW * 0.5, worldH * 0.5);
  // Random sampling: 200 attempts
  var m2 = 0x80000000, a2 = 1103515245, c2 = 12345;
  var rs = (Date.now() ^ 0xDEAD) | 0;
  var rng = function() { rs = (a2 * rs + c2) % m2; return rs / (m2 - 1); };
  var candidates = [];
  for (var attempt = 0; attempt < 200; attempt++) {
    var gx = Math.floor(rng() * gridW), gy = Math.floor(rng() * gridH);
    if (grid[gy * gridW + gx]) continue; // wall
    var wx = gx * cell + cell * 0.5, wy = gy * cell + cell * 0.5;
    if (isInGridWall(wx, wy, 6)) continue;
    if (!pointInPolygon(wx, wy, poly)) continue;
    candidates.push({x: wx, y: wy});
    if (candidates.length >= 10) break; // enough candidates
  }
  if (candidates.length > 0) {
    // Pick a candidate near the center of the map for a fair start
    var bestDist = Infinity, best = candidates[0];
    var cx = worldW * 0.5, cy = worldH * 0.5;
    for (var ci = 0; ci < candidates.length; ci++) {
      var d = Math.hypot(candidates[ci].x - cx, candidates[ci].y - cy);
      if (d < bestDist) { bestDist = d; best = candidates[ci]; }
    }
    return best;
  }
  // Fallback: full grid scan for any open cell inside polygon
  for (var gy2 = 1; gy2 < gridH - 1; gy2++) {
    for (var gx2 = 1; gx2 < gridW - 1; gx2++) {
      if (grid[gy2 * gridW + gx2]) continue;
      var wx2 = gx2 * cell + cell * 0.5, wy2 = gy2 * cell + cell * 0.5;
      if (!isInGridWall(wx2, wy2, 6) && pointInPolygon(wx2, wy2, poly)) return {x: wx2, y: wy2};
    }
  }
  return findSafeSpawn(worldW * 0.5, worldH * 0.5);
}

// Spawn the goal at a random open position far from the player.
// Called when all enemies are defeated.
function spawnGoal() {
  var goalSize = (worldW > 2000) ? 56 : 32;
  var candidates = [];
  var m2 = 0x80000000, a2 = 1103515245, c2 = 12345;
  var rs = (Date.now() ^ 0xBEEF) | 0;
  var rng = function() { rs = (a2 * rs + c2) % m2; return rs / (m2 - 1); };
  for (var attempt = 0; attempt < 300; attempt++) {
    var gx = Math.floor(rng() * gridW), gy = Math.floor(rng() * gridH);
    if (grid[gy * gridW + gx]) continue;
    var wx = gx * cell + cell * 0.5, wy = gy * cell + cell * 0.5;
    if (isInGridWall(wx, wy, goalSize * 0.5)) continue;
    if (currentBorderPoly && !pointInPolygon(wx, wy, currentBorderPoly)) continue;
    var dist = Math.hypot(wx - pos.x, wy - pos.y);
    candidates.push({x: wx, y: wy, dist: dist});
  }
  if (!candidates.length) {
    // Fallback: place near world center
    var safe = findSafeSpawn(worldW * 0.5, worldH * 0.5);
    candidates.push({x: safe.x, y: safe.y, dist: 0});
  }
  // Pick from the farthest 25% to make the player search for it
  candidates.sort(function(a, b) { return b.dist - a.dist; });
  var pick = candidates[Math.floor(rng() * Math.min(candidates.length, Math.max(1, Math.floor(candidates.length * 0.25))))];
  goal = {x: pick.x - goalSize * 0.5, y: pick.y - goalSize * 0.5, w: goalSize, h: goalSize};
  goalSpawned = true;
  pushToast('All enemies defeated! Find the exit!', '#88cc88', 4000);
  console.log('[GOAL] Spawned at (' + pick.x.toFixed(0) + ',' + pick.y.toFixed(0) + ') dist=' + pick.dist.toFixed(0) + ' from player');
}

// Mark every grid cell whose centre falls outside the border polygon as a wall.
// Called once after buildGrid() so it overrides (or adds to) existing wall data.
function applyIrregularBorder(poly) {
  if (!grid || !poly || !poly.length) return;
  var marked = 0;
  for (var gy = 0; gy < gridH; gy++) {
    for (var gx = 0; gx < gridW; gx++) {
      var cx = gx * cell + cell * 0.5;
      var cy = gy * cell + cell * 0.5;
      if (!pointInPolygon(cx, cy, poly)) {
        grid[gy * gridW + gx] = 1;
        if (wallHeights) wallHeights[gy * gridW + gx] = 1.0;  // uniform → flat top silhouette
        marked++;
      }
    }
  }
  console.log('[BORDER] Irregular border applied: ' + marked + ' cells marked outside polygon');
}
