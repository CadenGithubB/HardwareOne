// =============================================
// SECTION 3b: ENDLESS MODE — CHUNK-BASED GENERATION
// =============================================

function chunkSeedFor(cx, cy, component) {
  var h = WORLD_SEED;
  h = ((h << 5) - h + cx * 374761393) | 0;
  h = ((h << 5) - h + cy * 668265263) | 0;
  h = ((h << 5) - h + component * 1274126177) | 0;
  return h;
}

function pushToast(text, color, ms) {
  var now = Date.now();
  toasts.push({text: text, color: color || '#aabbcc', spawnMs: now, lifeMs: ms || 2500});
  if (toasts.length > 6) toasts.shift();
  // Persistent log entry with timestamp
  var d = new Date(now);
  var tLabel = ('0' + d.getHours()).slice(-2) + ':' + ('0' + d.getMinutes()).slice(-2) + ':' + ('0' + d.getSeconds()).slice(-2);
  toastLog.push({text: text, color: color || '#aabbcc', timeLabel: tLabel});
  if (toastLog.length > 200) toastLog.shift();
}

function chunkRng(cx, cy, component) {
  var m = 0x80000000, a = 1103515245, c = 12345;
  var state = chunkSeedFor(cx, cy, component);
  if (!state) state = 1;
  var next = function() { state = (a * state + c) % m; return (state & 0x7fffffff) / 0x80000000; };
  // Warm-up: break linearity from seed hashing
  next(); next(); next();
  return next;
}

function getBiomeAt(wx, wy) {
  if (CAVE_TEST_MODE) return 'plains';
  var n = biomeNoise(wx, wy, 3600);
  if (n < 0.167) return 'cave';
  if (n < 0.333) return 'ground';
  if (n < 0.500) return 'plains';
  if (n < 0.667) return 'forest';
  if (n < 0.833) return 'expanse';
  return 'ice';
}

function getFloorColorForBiome(biome, heightPercent) {
  var oldTerrain = terrain;
  terrain = biome;
  var c = getFloorColor(heightPercent);
  terrain = oldTerrain;
  return c;
}

// Parse hex color to RGB array (reusable scratch arrays to avoid GC)
var _frgb1 = [0,0,0], _frgb2 = [0,0,0];
function _hexToRGB(hex, out) {
  out[0] = parseInt(hex.substr(1, 2), 16);
  out[1] = parseInt(hex.substr(3, 2), 16);
  out[2] = parseInt(hex.substr(5, 2), 16);
}
function _rgbToHex(r, g, b) {
  return '#' + ((1 << 24) | (r << 16) | (g << 8) | b).toString(16).slice(1);
}

// Smooth floor color blending across biome boundaries using world position
function getFloorColorBlended(wx, wy, heightPercent) {
  var n = biomeNoise(wx, wy, 3600);
  var bt = GAME_CONFIG.world.biomeThresholds;
  var biome1, biome2, t;
  var bw = GAME_CONFIG.world.biomeBlendWidth;
  var baseColor;
  if (n < bt.cave - bw)     { baseColor = getFloorColorForBiome('cave', heightPercent); }
  else if (n < bt.cave + bw)    { biome1 = 'cave';    biome2 = 'ground';  t = (n - (bt.cave - bw)) / (2 * bw); }
  else if (n < bt.ground - bw)  { baseColor = getFloorColorForBiome('ground', heightPercent); }
  else if (n < bt.ground + bw)  { biome1 = 'ground';  biome2 = 'plains';  t = (n - (bt.ground - bw)) / (2 * bw); }
  else if (n < bt.plains - bw)  { baseColor = getFloorColorForBiome('plains', heightPercent); }
  else if (n < bt.plains + bw)  { biome1 = 'plains';  biome2 = 'forest';  t = (n - (bt.plains - bw)) / (2 * bw); }
  else if (n < bt.forest - bw) { baseColor = getFloorColorForBiome('forest', heightPercent); }
  else if (n < bt.forest + bw) { biome1 = 'forest';  biome2 = 'expanse'; t = (n - (bt.forest - bw)) / (2 * bw); }
  else if (n < bt.expanse - bw) { baseColor = getFloorColorForBiome('expanse', heightPercent); }
  else if (n < bt.expanse + bw) { biome1 = 'expanse'; biome2 = 'ice';     t = (n - (bt.expanse - bw)) / (2 * bw); }
  else { baseColor = getFloorColorForBiome('ice', heightPercent); }

  if (!baseColor) {
    t = t * t * (3 - 2 * t);
    var c1 = getFloorColorForBiome(biome1, heightPercent);
    var c2 = getFloorColorForBiome(biome2, heightPercent);
    _hexToRGB(c1, _frgb1); _hexToRGB(c2, _frgb2);
    baseColor = _rgbToHex(
      Math.round(_frgb1[0] + (_frgb2[0] - _frgb1[0]) * t),
      Math.round(_frgb1[1] + (_frgb2[1] - _frgb1[1]) * t),
      Math.round(_frgb1[2] + (_frgb2[2] - _frgb1[2]) * t)
    );
  }

  // Elevation tinting — mountains get lighter/grey, valleys get darker/blue-green
  if (!ENDLESS_MODE) return baseColor;
  var geo = geographyNoise(wx, wy);
  if (geo > 0.6 || geo < 0.35) {
    _hexToRGB(baseColor, _frgb1);
    var r = _frgb1[0], g = _frgb1[1], b = _frgb1[2];
    if (geo > 0.6) {
      // Mountain: lighten + grey shift (rocky)
      var mt = (geo - 0.6) / 0.4; // 0–1
      r = Math.min(255, Math.round(r + (180 - r) * mt * 0.35));
      g = Math.min(255, Math.round(g + (175 - g) * mt * 0.35));
      b = Math.min(255, Math.round(b + (170 - b) * mt * 0.35));
      // Ice biome mountains: snow white
      if (n >= 0.80) {
        var snowT = mt * 0.5;
        r = Math.min(255, Math.round(r + (240 - r) * snowT));
        g = Math.min(255, Math.round(g + (245 - g) * snowT));
        b = Math.min(255, Math.round(b + (250 - b) * snowT));
      }
    } else {
      // Valley: darken + blue-green tint (marshy)
      var vt = (0.35 - geo) / 0.35; // 0–1
      r = Math.round(r * (1 - vt * 0.2));
      g = Math.round(g * (1 - vt * 0.05));
      b = Math.min(255, Math.round(b + (40 * vt)));
    }
    return _rgbToHex(r, g, b);
  }
  return baseColor;
}

// Smooth wall color interpolation across biome boundaries
// Returns RGB based on biome noise at world position, blending between adjacent biome colors
function getWallBiomeRGB(wx, wy) {
  var n = biomeNoise(wx, wy, 3600);
  // Biome color anchors (midpoints of each biome's noise range)
  // cave:0.083  ground:0.250  plains:0.416  forest:0.583  expanse:0.750  ice:0.916
  var ar, ag, ab, br2, bg2, bb2, t;
  if (n < 0.083) { return 0x5F5F69; } // pure cave
  else if (n < 0.250) {
    t = (n - 0.083) / 0.167;
    ar = 95; ag = 95; ab = 105;       // cave
    br2 = 180; bg2 = 140; bb2 = 100;  // ground
  } else if (n < 0.416) {
    t = (n - 0.250) / 0.167;
    ar = 180; ag = 140; ab = 100;     // ground
    br2 = 160; bg2 = 130; bb2 = 90;   // plains
  } else if (n < 0.583) {
    t = (n - 0.416) / 0.167;
    ar = 160; ag = 130; ab = 90;      // plains
    br2 = 75; bg2 = 55; bb2 = 35;    // forest (bark brown — canopy is separate)
  } else if (n < 0.750) {
    t = (n - 0.583) / 0.167;
    ar = 75; ag = 55; ab = 35;       // forest (bark brown)
    br2 = 90; bg2 = 85; bb2 = 80;     // expanse
  } else if (n < 0.916) {
    t = (n - 0.750) / 0.167;
    ar = 90; ag = 85; ab = 80;        // expanse
    br2 = 140; bg2 = 170; bb2 = 240;  // ice
  } else { return 0x8CAAF0; } // pure ice
  var r = Math.round(ar + (br2 - ar) * t);
  var g = Math.round(ag + (bg2 - ag) * t);
  var b = Math.round(ab + (bb2 - ab) * t);
  return (r << 16) | (g << 8) | b;
}

function getDifficultyAt(wx, wy) {
  var dist = Math.hypot(wx, wy);
  return 1.0 + dist / 5000;
}

// ══════════════════════════════════════════════════════════════
// ── Endless-Mode Underground Cave Network Generator ──
// ══════════════════════════════════════════════════════════════

// Heights in this section are mesh-height units. XY is always world-space;
// only the copies in deepCaveEntrances are rebased into window coordinates.
function getEndlessNaturalSurfaceH(wx, wy) {
  var geoOffset = (geographyNoise(wx, wy) - 0.5) * 8.0;
  var spawnDist = Math.hypot(wx, wy);
  if (spawnDist < 800) geoOffset *= Math.max(0, (spawnDist - 200) / 600);
  return terrainNoise(wx, wy) + geoOffset;
}

function caveSmooth01(t) {
  t = Math.max(0, Math.min(1, t));
  return t * t * (3 - 2 * t);
}

// This footprint is shared with render/diagnostic consumers. along is
// positive OUTSIDE the mouth, matching the original entrance-zone convention.
function sampleCavePortal(e, x, y) {
  var dx = x - e.x, dy = y - e.y;
  var co = Math.cos(e.angle), si = Math.sin(e.angle);
  var along = -(dx * co + dy * si);
  var cross = -dx * si + dy * co;
  var perp = Math.abs(cross);
  var outer = e.approachLength || 480, inner = e.innerLength || 240;
  if (along > outer || along < -inner) return null;
  var halfW = e.halfWidth || 54;
  var coreW = halfW + (along > 0 ? 42 * along / outer : 0);
  var feather = 48;
  if (perp >= coreW + feather) return null;
  var lat = 1 - caveSmooth01((perp - coreW) / feather);
  var axial = along > 0 ? 1 - caveSmooth01(along / outer) : 1;
  return {along: along, cross: cross, perp: perp, coreW: coreW, lat: lat,
    tAxial: axial, t: axial * lat, inCore: perp <= coreW,
    isInside: along >= -inner && along <= 0, covered: along <= 0, entrance: e};
}

function generateCaveNetwork(regionX, regionY) {
  var netKey = regionX + ',' + regionY;
  if (netKey in endlessCaveNetworks) return endlessCaveNetworks[netKey];

  var REGION_CHUNKS = 3;
  var centerCX = regionX * REGION_CHUNKS + Math.floor(REGION_CHUNKS / 2);
  var centerCY = regionY * REGION_CHUNKS + Math.floor(REGION_CHUNKS / 2);
  var centerWX = centerCX * CHUNK_SIZE + CHUNK_SIZE / 2;
  var centerWY = centerCY * CHUNK_SIZE + CHUNK_SIZE / 2;

  // Cave test mode: force cave at region (0,0), skip all others
  if (CAVE_TEST_MODE) {
    if (regionX !== 0 || regionY !== 0) {
      endlessCaveNetworks[netKey] = null;
      return null;
    }
    // Fall through — always generate at (0,0)
  } else {
    // Only generate in cave biome
    var biome = getBiomeAt(centerWX, centerWY);
    if (biome !== 'cave') {
      endlessCaveNetworks[netKey] = null;
      return null;
    }

    // ~30% of cave-biome regions get underground caves
    var rng2 = chunkRng(regionX, regionY, 100);
    if (rng2() > 0.30) {
      endlessCaveNetworks[netKey] = null;
      return null;
    }
  }

  var rng = chunkRng(regionX, regionY, 100);
  var entryAngle = rng() * Math.PI * 2;
  var entryDist = CHUNK_SIZE * (0.5 + rng() * 0.4);
  var mouthX = centerWX + Math.cos(entryAngle) * entryDist;
  var mouthY = centerWY + Math.sin(entryAngle) * entryDist;
  var inward = entryAngle + Math.PI;
  var headroom = 3.8 + rng() * 0.4, minCover = 1.0;
  var style = rng(), width = 104 + rng() * 16;
  var turns = [], lengths = [], widths = [];
  var count = 5 + Math.floor(rng() * 3);
  for (var s = 0; s < count; s++) {
    turns.push(s === 0 ? 0 : (rng() - 0.5) * 0.65);
    lengths.push(s === 0 ? 180 : 55 + rng() * 25);
    widths.push(s === 0 ? width : width * (0.86 + rng() * 0.2));
  }
  var chamberRadius = 82 + rng() * 24;

  // A modest first recipe: one connected winding route into a chamber. The
  // old independently grown entrance networks could advertise disconnected
  // exits. Larger/dual-entrance recipes can use this same portal contract later.
  function makeLayout(ex, ey, ang, hillside) {
    var corridors = [], px = ex, py = ey, a = ang;
    var steps = hillside ? 4 : count;
    for (var i = 0; i < steps; i++) {
      a = ang + Math.max(-0.65, Math.min(0.65, a - ang + turns[i]));
      var segLen = hillside && i === 0 ? 120 : lengths[i];
      var nx = px + Math.cos(a) * segLen, ny = py + Math.sin(a) * segLen;
      corridors.push({x1: px, y1: py, x2: nx, y2: ny, width: widths[i], ceilH: headroom});
      px = nx; py = ny;
    }
    return {corridors: corridors, chambers: [{cx: px, cy: py,
      radius: chamberRadius, ceilH: headroom + 0.6, terminal: true}]};
  }
  // Check the full width, not merely its centerline. Sampling extends beyond
  // the footprint; the extra cover margin absorbs sub-cell terrain variation.
  function floorLimit(layout, ex, ey, ang, skipPortal) {
    var limit = Infinity, co = Math.cos(ang), si = Math.sin(ang);
    function take(x, y, roof) {
      var axial = (x - ex) * co + (y - ey) * si;
      if (skipPortal && axial < 240) return;
      limit = Math.min(limit, getEndlessNaturalSurfaceH(x, y) - roof - minCover - 0.35);
    }
    for (var j = 0; j < layout.corridors.length; j++) {
      var c = layout.corridors[j], dx = c.x2 - c.x1, dy = c.y2 - c.y1;
      var len = Math.hypot(dx, dy), n = Math.ceil(len / 24);
      for (var k = 0; k <= n; k++) {
        var x = c.x1 + dx * k / n, y = c.y1 + dy * k / n;
        for (var side = -1; side <= 1; side++) {
          take(x - dy / len * (c.width / 2 + 24) * side,
               y + dx / len * (c.width / 2 + 24) * side, c.ceilH);
        }
      }
    }
    var ch = layout.chambers[0];
    take(ch.cx, ch.cy, ch.ceilH);
    for (var r = 1; r <= 2; r++) {
      for (var a = 0; a < 16; a++) {
        var theta = a * Math.PI / 8;
        take(ch.cx + Math.cos(theta) * (ch.radius + 24) * r / 2,
             ch.cy + Math.sin(theta) * (ch.radius + 24) * r / 2, ch.ceilH);
      }
    }
    return limit;
  }

  var layout = makeLayout(mouthX, mouthY, inward);
  var floorH = Math.min(getEndlessNaturalSurfaceH(mouthX, mouthY) - headroom - minCover,
    floorLimit(layout, mouthX, mouthY, inward, false));
  var kind = 'descending';
  var hillsideBestMargin = -Infinity;
  // Hillside mouths require a real uphill landform. Search is deterministic,
  // bounded, and done once per cached network; a failed fit becomes a descent.
  // No synthetic test-only hillside or globally raised terrain is involved.
  var forcedKind = CAVE_TEST_MODE && typeof window !== 'undefined' ? window._caveTestKindOverride : null;
  for (var attempt = 0; attempt < 32 && forcedKind !== 'descending'; attempt++) {
    var ca = entryAngle + attempt * Math.PI * 2 / 32;
    var tx = centerWX + Math.cos(ca) * entryDist;
    var ty = centerWY + Math.sin(ca) * entryDist;
    var ta = ca + Math.PI;
    // A hillside mouth is cut into the hillside at its OUTSIDE approach
    // elevation, not at the uncut rock's height directly over the opening.
    var th = getEndlessNaturalSurfaceH(tx - Math.cos(ta) * 240, ty - Math.sin(ta) * 240) - 0.4;
    var uphill = getEndlessNaturalSurfaceH(tx + Math.cos(ta) * 300, ty + Math.sin(ta) * 300);
    if (uphill < th + headroom + minCover) continue;
    var trial = makeLayout(tx, ty, ta, true);
    var fitMargin = floorLimit(trial, tx, ty, ta, true) - th;
    hillsideBestMargin = Math.max(hillsideBestMargin, fitMargin);
    if (fitMargin < 0) continue;
    mouthX = tx; mouthY = ty; inward = ta; floorH = th;
    layout = trial; kind = 'hillside'; break;
  }
  for (var ci = 0; ci < layout.corridors.length; ci++) {
    layout.corridors[ci].floorH = floorH;
    layout.corridors[ci].depth = -floorH;
  }
  for (var chi = 0; chi < layout.chambers.length; chi++) {
    layout.chambers[chi].floorH = floorH;
    layout.chambers[chi].depth = -floorH;
  }
  var entrance = {id: netKey + ':0', x: mouthX, y: mouthY, angle: inward,
    kind: kind, floorH: floorH, depth: -floorH, ceilH: headroom,
    ceilingH: floorH + headroom, halfWidth: width / 2,
    approachLength: kind === 'hillside' ? 240 : 480, innerLength: 240,
    minCover: minCover, style: style};
  var net = {corridors: layout.corridors, chambers: layout.chambers, entrances: [entrance],
    regionX: regionX, regionY: regionY, recipe: 'winding-chamber-v1', hillsideFitMargin: hillsideBestMargin};
  net.route = [{x: mouthX, y: mouthY}];
  for (var ri = 0; ri < layout.corridors.length; ri++) {
    net.route.push({x: layout.corridors[ri].x2, y: layout.corridors[ri].y2});
  }
  endlessCaveNetworks[netKey] = net;
  return net;
}

// Point-in-cave-network test: capsule (corridors) + circle (chambers)
// Returns {depth, ceilH, type, halfW, dist, fade} or null
// fade: 1.0 deep inside, smoothly → 0.0 at boundary (prevents jagged mesh edges)
function queryEndlessCave(wx, wy, networks) {
  var best = null;
  var bestFade = 0;
  for (var ni = 0; ni < networks.length; ni++) {
    var net = networks[ni];
    // Test corridors (capsule distance)
    for (var ci = 0; ci < net.corridors.length; ci++) {
      var c = net.corridors[ci];
      var cdx = c.x2 - c.x1, cdy = c.y2 - c.y1;
      var lenSq = cdx * cdx + cdy * cdy;
      if (lenSq < 1) continue;
      var t = Math.max(0, Math.min(1, ((wx - c.x1) * cdx + (wy - c.y1) * cdy) / lenSq));
      var px = c.x1 + t * cdx, py = c.y1 + t * cdy;
      var perpDist = Math.hypot(wx - px, wy - py);
      var hw = c.width * 0.5;
      var outerHW = hw + 20; // 20-unit fade zone beyond hard edge
      if (perpDist < outerHW) {
        // Smooth fade: 1.0 inside 70% of hw, smoothstep to 0 at outerHW
        var fadeStart = hw * 0.7;
        var fade;
        if (perpDist <= fadeStart) fade = 1.0;
        else {
          var ft = (perpDist - fadeStart) / (outerHW - fadeStart);
          fade = 1.0 - ft * ft * (3 - 2 * ft); // inverse smoothstep
        }
        if (fade > bestFade) {
          bestFade = fade;
          best = {depth: c.depth, ceilH: c.ceilH, type: 'corridor', halfW: hw, dist: perpDist, fade: fade};
        }
      }
    }
    // Test chambers (circle distance)
    for (var chi = 0; chi < net.chambers.length; chi++) {
      var ch = net.chambers[chi];
      var chDist = Math.hypot(wx - ch.cx, wy - ch.cy);
      var outerR = ch.radius + 20; // 20-unit fade zone
      if (chDist < outerR) {
        var fadeStart = ch.radius * 0.7;
        var fade;
        if (chDist <= fadeStart) fade = 1.0;
        else {
          var ft = (chDist - fadeStart) / (outerR - fadeStart);
          fade = 1.0 - ft * ft * (3 - 2 * ft);
        }
        if (fade > bestFade) {
          bestFade = fade;
          best = {depth: ch.depth, ceilH: ch.ceilH, type: 'chamber', halfW: ch.radius, dist: chDist, fade: fade};
        }
      }
    }
  }
  return best;
}

// ═══════════════════════════════════════════════════════════════════════
//  UNIFIED CAVE GEOMETRY QUERY — single source of truth
// ═══════════════════════════════════════════════════════════════════════
// Replaces queryEndlessCave + queryEntranceZone + queryEntranceApproach +
// rim-lip + entrance-clearance pass + ad-hoc interior blends with one
// function. For any (wx, wy) and the smoothed surface at that point, it
// returns either null (no cave influence) or a single record describing
// how the cave should be stamped at this cell.
//
// Returned record:
//   {
//     fade: 0..1,         blend factor. 1 = pure cave, 0 = pure surface.
//     caveFloorZ,         connected passage floor in mesh-height units.
//     floorH,             actual floor after approach/edge shaping.
//     surfaceH,           ordinary surface, or the local hillside cover bank.
//     ceilZ,              cave ceiling Z, or null if no ceiling (approach ramp).
//     covered,            true only on the covered side of the portal plane.
//     portal,             complete authoritative entrance record.
//     isEntrance,         near the mouth; diagnostic, not another cap rule.
//     wallCarve,          true if grid[] should be carved open here.
//     source,             'corridor' | 'chamber' | 'approach' — diagnostic only.
//   }
//
// Per-chunk stamping becomes:
//   var c = queryCaveGeometry(wx, wy, nets, smoothSurf);
//   if (c) { cHeights[i] = c.floorH; cSurfaceH[i] = c.surfaceH;
//            if (c.ceilZ !== null) cCaveCeilH[i] = c.ceilZ; }
//
// Per-grid carving becomes:
//   var c = queryCaveGeometry(cwx, cwy, nets, smoothSurf);
//   if (c && c.wallCarve) cGrid[i] = 0;
//
// All depth clamping, boundary smoothing, and entrance-mouth logic lives
// here. No other system touches cave geometry.
function queryCaveGeometry(wx, wy, networks, smoothedSurface) {
  var surface = smoothedSurface;
  if (typeof surface !== 'number' || !isFinite(surface)) surface = getEndlessNaturalSurfaceH(wx, wy);
  var best = null, bestFade = 0, approach = null;
  var bankSurface = surface, bankPortal = null;
  for (var ni = 0; ni < networks.length; ni++) {
    var net = networks[ni];
    // A generated cave lives on the inward side of its portal plane. The
    // rounded end of a capsule must never stamp a roof into the open approach.
    var entry = net.entrances[0], portal = null, inward = true;
    if (entry) {
      portal = sampleCavePortal(entry, wx, wy);
      var along = -((wx - entry.x) * Math.cos(entry.angle) + (wy - entry.y) * Math.sin(entry.angle));
      inward = along <= 0;
      if (portal && !inward && (!approach || portal.t > approach.t)) approach = portal;
    }
    if (!inward) continue;
    // A narrow, explicit rock bevel closes headroom at the boundary. We do
    // not blend the cave floor toward a distant terrain maximum: that used
    // to raise the floor through its own ceiling and create invisible steps.
    for (var ci = 0; ci < net.corridors.length; ci++) {
      var c = net.corridors[ci];
      var cdx = c.x2 - c.x1, cdy = c.y2 - c.y1;
      var lenSq = cdx * cdx + cdy * cdy;
      if (lenSq < 1) continue;
      var tC = Math.max(0, Math.min(1, ((wx - c.x1) * cdx + (wy - c.y1) * cdy) / lenSq));
      var perpDist = Math.hypot(wx - c.x1 - tC * cdx, wy - c.y1 - tC * cdy);
      var hw = c.width * 0.5;
      if (entry && entry.kind === 'hillside' && along >= -entry.innerLength && perpDist < hw + 96) {
        var bankFade = 1 - caveSmooth01((perpDist - hw - 36) / 60);
        var bankTarget = c.floorH + c.ceilH + entry.minCover;
        var bankH = surface + Math.max(0, bankTarget - surface) * bankFade;
        if (bankH > bankSurface) { bankSurface = bankH; bankPortal = entry; }
      }
      if (perpDist < hw + 36) {
        var fade = 1 - caveSmooth01((perpDist - hw) / 36);
        if (fade > bestFade) {
          bestFade = fade;
          best = {floor: typeof c.floorH === 'number' ? c.floorH : -c.depth,
            headroom: c.ceilH, source: 'corridor', entry: entry, portal: portal};
        }
      }
    }
    for (var chi = 0; chi < net.chambers.length; chi++) {
      var ch = net.chambers[chi];
      var chDist = Math.hypot(wx - ch.cx, wy - ch.cy);
      if (entry && entry.kind === 'hillside' && along >= -entry.innerLength && chDist < ch.radius + 96) {
        var chBankFade = 1 - caveSmooth01((chDist - ch.radius - 36) / 60);
        var chBankH = surface + Math.max(0, ch.floorH + ch.ceilH + entry.minCover - surface) * chBankFade;
        if (chBankH > bankSurface) { bankSurface = chBankH; bankPortal = entry; }
      }
      if (chDist < ch.radius + 36) {
        var fadeCh = 1 - caveSmooth01((chDist - ch.radius) / 36);
        if (fadeCh > bestFade) {
          bestFade = fadeCh;
          best = {floor: typeof ch.floorH === 'number' ? ch.floorH : -ch.depth,
            headroom: ch.ceilH, source: 'chamber', entry: entry, portal: portal};
        }
      }
    }
  }
  surface = bankSurface;
  if (best) {
    var cover = best.entry && best.entry.minCover || 1;
    var ceiling = best.floor + best.headroom;
    var localPortal = best.portal;
    // Only a hillside's entrance bank may add cover. Everywhere else the
    // generation-time terrain fit chooses the common network floor first.
    ceiling = Math.min(ceiling, surface - cover);
    var floor = best.floor + Math.max(0, ceiling - best.floor) * (1 - bestFade);
    return {fade: bestFade, caveFloorZ: best.floor, floorH: floor, ceilZ: ceiling,
      surfaceH: surface, covered: true, portal: best.entry,
      isEntrance: !!localPortal && localPortal.along >= -24,
      wallCarve: ceiling - floor >= 3.0 && floor - best.floor <= 0.75, source: best.source};
  }
  if (!approach || approach.t <= 0) {
    if (!bankPortal) return null;
    return {fade: 0, caveFloorZ: surface, floorH: surface, surfaceH: surface,
      ceilZ: null, covered: false, portal: bankPortal, isEntrance: false,
      wallCarve: false, source: 'bank'};
  }
  var e = approach.entrance;
  var target = typeof e.floorH === 'number' ? e.floorH : -e.depth;
  return {fade: approach.t, caveFloorZ: target,
    floorH: surface * (1 - approach.t) + target * approach.t,
    surfaceH: surface, ceilZ: null, covered: false, portal: e,
    isEntrance: approach.along < 24, wallCarve: approach.inCore,
    source: 'approach'};
}

// Debug helper: call `debugCaveAt(wx, wy)` from the console to inspect
// exactly what queryCaveGeometry returns at a world position. Prints the
// record plus the nearby networks' parameters for context.
if (typeof window !== 'undefined') {
  window.debugCaveAt = function(wx, wy) {
    var nets = (typeof endlessCaveNetworks !== 'undefined')
      ? Object.values(endlessCaveNetworks).filter(function(v){return v;}) : [];
    if (!nets.length) { console.log('[CAVE-DBG] no cave networks'); return null; }
    var r = queryCaveGeometry(wx, wy, nets, getEndlessNaturalSurfaceH(wx, wy));
    console.log('[CAVE-DBG] at (' + wx.toFixed(0) + ',' + wy.toFixed(0) + '):', r);
    return r;
  };
}

// Entrance ramp test: returns interpolation factor 0..1 (0=surface, 1=full depth)
// Works INSIDE cave corridors near the entrance point
function queryEntranceRamp(wx, wy, networks) {
  var rampLen = 180; // world units for the transition ramp (longer for gradual descent)
  for (var ni = 0; ni < networks.length; ni++) {
    var net = networks[ni];
    for (var ei = 0; ei < net.entrances.length; ei++) {
      var e = net.entrances[ei];
      var dist = Math.hypot(wx - e.x, wy - e.y);
      if (dist < rampLen) {
        return Math.min(1.0, dist / rampLen);
      }
    }
  }
  return -1; // not near any entrance
}

// Reach constant for cross-chunk cave network inclusion. Derived from zone
// geometry: outward approach OUTER_LEN=480 + margin for rare wider corridors.
// If queryEntranceZone's extents change, update this in tandem.
var ENTRANCE_ZONE_REACH = 520;

// Unified entrance zone — single field covering the approach cone outside
// the cave, the mouth, and a short inner stub. Eliminates the seam between
// approach (old queryEntranceApproach) and cave interior (queryEndlessCave)
// by producing one coherent footprint for floor, ceiling, and walls.
//
// Returns null if (wx,wy) is not near any entrance. Otherwise:
//   {
//     t: 0..1       — global blend: 0 at outer edge of zone, 1 deep inside
//     tAxial: 0..1  — blend ignoring lateral falloff (along-axis only)
//     lat: 0..1     — lateral smoothstep (1 in core, 0 at feather edge)
//     along: world-units along axis (positive outward, negative inward)
//     perp: perpendicular distance to axis
//     inCore: bool  — inside the core band (walls should be carved)
//     isInside: bool — on the cave side of the entrance
//     depth, ceilH  — cave geometry parameters near this entrance
//     entrance: the winning entrance record
//   }
//
// Axis convention: entrance.angle faces INWARD toward cave center, so outward
// direction is -angle. Along > 0 means OUTSIDE the cave; along < 0 is inside.
function queryEntranceZone(wx, wy, networks) {
  var best = null;
  var bestT = -1;
  for (var ni = 0; ni < networks.length; ni++) {
    var net = networks[ni];
    for (var ei = 0; ei < net.entrances.length; ei++) {
      var e = net.entrances[ei];
      var p = sampleCavePortal(e, wx, wy);
      if (!p || p.t <= bestT) continue;
      bestT = p.t;
      p.depth = e.depth;
      p.ceilH = e.ceilH;
      best = p;
    }
  }
  return best;
}

// Back-compat shim: old call sites expect {t, depth, ceilH} where t=0 at outer
// edge and t=1 at entrance. Route through the unified zone.
function queryEntranceApproach(wx, wy, networks) {
  var z = queryEntranceZone(wx, wy, networks);
  if (!z || z.isInside) return null; // old function was outside-only
  return {t: z.t, depth: z.depth, ceilH: z.ceilH};
}

// The chamber center owns its content even when it lands on a chunk edge.
// Insets keep initial actors in loaded owner geometry; unlike rejecting edge
// rows, this cannot silently remove a small cave's only reward/encounter.
function caveChamberContentPoint(chamber, offsetX, offsetY) {
  var cx = Math.floor(chamber.cx / CHUNK_SIZE), cy = Math.floor(chamber.cy / CHUNK_SIZE);
  var inset = 18;
  return {ownerCX: cx, ownerCY: cy,
    x: Math.max(cx * CHUNK_SIZE + inset, Math.min((cx + 1) * CHUNK_SIZE - inset, chamber.cx + offsetX)),
    y: Math.max(cy * CHUNK_SIZE + inset, Math.min((cy + 1) * CHUNK_SIZE - inset, chamber.cy + offsetY))};
}

function generateChunk(cx, cy) {
  var key = cx + ',' + cy;
  if (chunks[key]) return chunks[key];

  var biome = getBiomeAt(cx * CHUNK_SIZE + CHUNK_SIZE / 2, cy * CHUNK_SIZE + CHUNK_SIZE / 2);
  var difficulty = getDifficultyAt(cx * CHUNK_SIZE + CHUNK_SIZE / 2, cy * CHUNK_SIZE + CHUNK_SIZE / 2);
  var rng = chunkRng(cx, cy, 0);

  // ── Market check — decide early so we can clear walls ──
  var hasMarket = false;
  if ((!CAVE_TEST_MODE || caveTestFlags.markets) && !(cx === 0 && cy === 0)) {
    var marketRng = chunkRng(cx, cy, 50);
    hasMarket = marketRng() < 0.02; // ~2% per chunk
    // Enforce 3-chunk exclusion zone — deterministic neighbor check
    if (hasMarket) {
      for (var ndy = -3; ndy <= 3 && hasMarket; ndy++) {
        for (var ndx = -3; ndx <= 3 && hasMarket; ndx++) {
          if (ndx === 0 && ndy === 0) continue;
          if (cx + ndx === 0 && cy + ndy === 0) continue; // skip origin
          var nRng = chunkRng(cx + ndx, cy + ndy, 50);
          if (nRng() < 0.02) {
            // Neighbor also has a market — lower coord wins
            if ((cy + ndy) < cy || ((cy + ndy) === cy && (cx + ndx) < cx)) {
              hasMarket = false;
            }
          }
        }
      }
    }
  }

  // ── Grid & walls ──
  var cGrid = new Uint8Array(CHUNK_CELLS * CHUNK_CELLS);
  var cWallH = new Float32Array(CHUNK_CELLS * CHUNK_CELLS);
  var cWallCR = new Uint8Array(CHUNK_CELLS * CHUNK_CELLS);
  var cWallCG = new Uint8Array(CHUNK_CELLS * CHUNK_CELLS);
  var cWallCB = new Uint8Array(CHUNK_CELLS * CHUNK_CELLS);
  var cWalls = [];

  // Noise-threshold wall placement — seamless across chunks
  // Larger scales = broader wall formations; higher thresholds = more open space
  var wallScale, wallThresh;
  if (biome === 'cave')    { wallScale = 70; wallThresh = 0.74; }
  else if (biome === 'expanse') { wallScale = 120; wallThresh = 0.86; }
  else if (biome === 'ice')     { wallScale = 110; wallThresh = 0.84; }
  else if (biome === 'plains')  { wallScale = 130; wallThresh = 0.88; }
  else if (biome === 'forest')  { wallScale = 90;  wallThresh = 0.80; }
  else                          { wallScale = 100; wallThresh = 0.82; }

  // Geography modulates wall density: mountains denser, valleys sparser
  var chunkCenterWX = cx * CHUNK_SIZE + CHUNK_SIZE * 0.5;
  var chunkCenterWY = cy * CHUNK_SIZE + CHUNK_SIZE * 0.5;
  var chunkGeo = geographyNoise(chunkCenterWX, chunkCenterWY);
  if (chunkGeo > 0.65) wallThresh -= 0.06; // mountains: more walls
  else if (chunkGeo < 0.35) wallThresh += 0.04; // valleys: fewer walls

  // Use a second noise layer for wall placement (different seed from terrain)
  var oldSeed = noiseSeed;
  noiseSeed = (WORLD_SEED * 3 + 7777) | 0;
  for (var ly = 0; ly < CHUNK_CELLS; ly++) {
    for (var lx = 0; lx < CHUNK_CELLS; lx++) {
      var wx = cx * CHUNK_SIZE + lx * cell + cell * 0.5;
      var wy = cy * CHUNK_SIZE + ly * cell + cell * 0.5;
      var n = smoothNoise(wx, wy, wallScale);
      // Add medium-scale variation for organic shapes
      n += (smoothNoise(wx, wy, wallScale * 3) - 0.5) * 0.15;
      // Skip walls in water zones (very low terrain)
      var wallTerrain = terrainNoise(wx, wy) + (geographyNoise(wx, wy) - 0.5) * 8.0;
      if (n > wallThresh && wallTerrain > -2.5) {
        cGrid[ly * CHUNK_CELLS + lx] = 1;
        cWallH[ly * CHUNK_CELLS + lx] = 0.5 + rng() * 0.8;
      } else {
        cWallH[ly * CHUNK_CELLS + lx] = 0.7 + rng() * 0.6;
      }
    }
  }
  noiseSeed = oldSeed;

  // Cleanup: remove isolated wall cells (fewer than 2 wall neighbors) to eliminate thin columns
  for (var cy2 = 0; cy2 < CHUNK_CELLS; cy2++) {
    for (var cx2 = 0; cx2 < CHUNK_CELLS; cx2++) {
      if (!cGrid[cy2 * CHUNK_CELLS + cx2]) continue;
      var neighbors = 0;
      if (cx2 > 0 && cGrid[cy2 * CHUNK_CELLS + (cx2-1)]) neighbors++;
      if (cx2 < CHUNK_CELLS-1 && cGrid[cy2 * CHUNK_CELLS + (cx2+1)]) neighbors++;
      if (cy2 > 0 && cGrid[(cy2-1) * CHUNK_CELLS + cx2]) neighbors++;
      if (cy2 < CHUNK_CELLS-1 && cGrid[(cy2+1) * CHUNK_CELLS + cx2]) neighbors++;
      if (neighbors < 2) cGrid[cy2 * CHUNK_CELLS + cx2] = 0;
    }
  }

  // Ensure a clear spawn area near origin (first chunk)
  if (cx === 0 && cy === 0) {
    var centerL = Math.floor(CHUNK_CELLS / 2);
    for (var dy = -3; dy <= 3; dy++) {
      for (var dx = -3; dx <= 3; dx++) {
        var gx = centerL + dx, gy = centerL + dy;
        if (gx >= 0 && gx < CHUNK_CELLS && gy >= 0 && gy < CHUNK_CELLS) {
          cGrid[gy * CHUNK_CELLS + gx] = 0;
        }
      }
    }
  }

  // Clear market area — carve out a 10x10 open space in chunk center
  if (hasMarket) {
    var mcx = Math.floor(CHUNK_CELLS / 2);
    var mcy = Math.floor(CHUNK_CELLS / 2);
    for (var mdy = -5; mdy <= 5; mdy++) {
      for (var mdx = -5; mdx <= 5; mdx++) {
        var mgx = mcx + mdx, mgy = mcy + mdy;
        if (mgx >= 1 && mgx < CHUNK_CELLS - 1 && mgy >= 1 && mgy < CHUNK_CELLS - 1) {
          cGrid[mgy * CHUNK_CELLS + mgx] = 0;
        }
      }
    }
  }

  // ── Multi-chunk structures (fortress / arena / watchtower) ──
  // Region grid: every 3×3 chunk area is a "region". A deterministic RNG
  // per region center decides if a large structure spawns there (~4% chance).
  // Each chunk checks the 4 surrounding regions for overlap and stamps the
  // portion of any structure that falls within its own 32×32 grid.
  var cStructure = null; // {type, centerWX, centerWY, regionX, regionY}
  var REGION_CHUNKS = 3;
  var regionX = Math.floor(cx / REGION_CHUNKS);
  var regionY = Math.floor(cy / REGION_CHUNKS);
  // Check this region and 8 neighbors (structure footprints can span regions)
  // Skip structures entirely in cave test mode — clean plains only
  for (var rdy2 = -1; rdy2 <= 1 && !cStructure && (!CAVE_TEST_MODE || caveTestFlags.structures); rdy2++) {
    for (var rdx2 = -1; rdx2 <= 1 && !cStructure; rdx2++) {
      var rX = regionX + rdx2, rY = regionY + rdy2;
      // No structures near spawn
      if (rX === 0 && rY === 0) continue;
      var regRng = chunkRng(rX, rY, 80);
      if (regRng() > 0.012) continue; // ~1.2% per region — rare landmarks
      // Structure center in world coords (center of the 3×3 chunk region)
      var sCenterCX = rX * REGION_CHUNKS + Math.floor(REGION_CHUNKS / 2);
      var sCenterCY = rY * REGION_CHUNKS + Math.floor(REGION_CHUNKS / 2);
      var sCenterWX = sCenterCX * CHUNK_SIZE + CHUNK_SIZE / 2;
      var sCenterWY = sCenterCY * CHUNK_SIZE + CHUNK_SIZE / 2;
      // Don't place structures in caves or ice
      var sBiome = getBiomeAt(sCenterWX, sCenterWY);
      if (sBiome === 'cave' || sBiome === 'ice') continue;
      // Structure type — biome-influenced
      var sTypeRoll = regRng();
      var sType;
      if (sBiome === 'expanse') {
        // Arid wastes: arenas and watchtowers more common, no mossy colors
        sType = (sTypeRoll < 0.25) ? 'fortress' : (sTypeRoll < 0.65) ? 'arena' : 'watchtower';
      } else if (sBiome === 'ground') {
        // Dungeon-like: fortresses and watchtowers, fewer arenas
        sType = (sTypeRoll < 0.5) ? 'fortress' : (sTypeRoll < 0.65) ? 'arena' : 'watchtower';
      } else if (sBiome === 'forest') {
        // Forest: ancient temples (fortress), hidden arenas rare
        sType = (sTypeRoll < 0.55) ? 'fortress' : (sTypeRoll < 0.70) ? 'arena' : 'watchtower';
      } else {
        // Plains: even mix
        sType = (sTypeRoll < 0.4) ? 'fortress' : (sTypeRoll < 0.7) ? 'arena' : 'watchtower';
      }
      // Per-instance variation (deterministic from regRng)
      var sScale = 0.85 + regRng() * 0.3; // 0.85–1.15 size multiplier
      // Color palette — biome-driven with random secondary variation
      var sPaletteRoll = regRng();
      var sPalette;
      if (sType === 'fortress') {
        // Biome-appropriate palettes:
        // ground: grey stone or mossy green
        // plains: grey stone or warm sandstone
        // expanse: warm sandstone or dark basalt
        var fPal;
        if (sBiome === 'ground')       fPal = (sPaletteRoll < 0.5) ? 0 : 3;
        else if (sBiome === 'expanse') fPal = (sPaletteRoll < 0.6) ? 1 : 2;
        else                           fPal = (sPaletteRoll < 0.5) ? 0 : 1; // plains
        if (fPal === 0)      sPalette = {w:[140,135,125], c:[155,150,140], k:[150,145,135], f:[140,135,125]};
        else if (fPal === 1) sPalette = {w:[165,140,100], c:[180,155,115], k:[175,150,110], f:[160,140,105]};
        else if (fPal === 2) sPalette = {w:[75,70,80],    c:[90,85,95],    k:[85,80,90],    f:[80,75,85]};
        else                 sPalette = {w:[105,130,100], c:[120,145,115], k:[115,140,110], f:[110,135,105]};
      } else if (sType === 'arena') {
        // ground: red-brown clay or dark iron
        // plains: red-brown or golden sand
        // expanse: golden sand or white marble (sun-bleached)
        var aPal;
        if (sBiome === 'ground')       aPal = (sPaletteRoll < 0.6) ? 0 : 3;
        else if (sBiome === 'expanse') aPal = (sPaletteRoll < 0.5) ? 1 : 2;
        else                           aPal = (sPaletteRoll < 0.5) ? 0 : 1; // plains
        if (aPal === 0)      sPalette = {w:[130,75,55],  p:[100,60,45],  s:[115,65,50],  f:[160,120,80]};
        else if (aPal === 1) sPalette = {w:[170,150,90], p:[140,120,70], s:[155,135,80], f:[180,165,110]};
        else if (aPal === 2) sPalette = {w:[190,185,175],p:[160,155,145],s:[175,170,160],f:[200,195,190]};
        else                 sPalette = {w:[70,65,75],   p:[55,50,60],   s:[60,55,65],   f:[90,85,95]};
      } else {
        // ground: mossy/copper-brown (woodland outpost)
        // plains: blue-grey or pale ivory
        // expanse: copper-brown or dark slate (weathered)
        var tPal;
        if (sBiome === 'ground')       tPal = (sPaletteRoll < 0.5) ? 1 : 0;
        else if (sBiome === 'expanse') tPal = (sPaletteRoll < 0.5) ? 1 : 3;
        else                           tPal = (sPaletteRoll < 0.5) ? 0 : 2; // plains
        if (tPal === 0)      sPalette = {w:[110,120,140], t:[120,130,155], a:[110,120,140], f:[120,125,140]};
        else if (tPal === 1) sPalette = {w:[145,110,75],  t:[160,125,85],  a:[140,105,70],  f:[150,120,85]};
        else if (tPal === 2) sPalette = {w:[185,180,165], t:[195,190,175], a:[180,175,160], f:[190,185,175]};
        else                 sPalette = {w:[65,70,80],    t:[75,80,90],    a:[60,65,75],    f:[70,75,85]};
      }
      // Layout variations
      var sNumBuildings = Math.floor(regRng() * 3); // 0–2 internal buildings for fortress
      var sNumPillars = 8 + Math.floor(regRng() * 8); // 8–15 for arena
      var sArmCount = 2 + Math.floor(regRng() * 3); // 2–4 arms for watchtower
      var sRotation = regRng() * Math.PI * 0.5; // 0–90° rotation for layout variety
      // Check if this chunk overlaps the structure footprint
      var chunkWX0 = cx * CHUNK_SIZE, chunkWY0 = cy * CHUNK_SIZE;
      var chunkWX1 = chunkWX0 + CHUNK_SIZE, chunkWY1 = chunkWY0 + CHUNK_SIZE;
      var sRadius; // half-extent of structure bounding box
      if (sType === 'fortress') sRadius = CHUNK_SIZE * 1.8 * sScale;
      else if (sType === 'arena') sRadius = CHUNK_SIZE * 1.3 * sScale;
      else sRadius = CHUNK_SIZE * 1.5 * sScale;
      // AABB overlap check
      if (chunkWX1 < sCenterWX - sRadius || chunkWX0 > sCenterWX + sRadius) continue;
      if (chunkWY1 < sCenterWY - sRadius || chunkWY0 > sCenterWY + sRadius) continue;
      cStructure = {type: sType, centerWX: sCenterWX, centerWY: sCenterWY, regionX: rX, regionY: rY,
                    scale: sScale, palette: sPalette, numBuildings: sNumBuildings,
                    numPillars: sNumPillars, armCount: sArmCount, rotation: sRotation};
    }
  }
  if (cStructure) {
    var sWX = cStructure.centerWX, sWY = cStructure.centerWY;
    var sType2 = cStructure.type;
    var _sc = cStructure.scale;
    var _pal = cStructure.palette;
    var structWallH = ((sType2 === 'fortress') ? 0.9 : (sType2 === 'watchtower') ? 1.0 : 0.7) * (0.9 + _sc * 0.1);
    for (var sgy = 0; sgy < CHUNK_CELLS; sgy++) {
      for (var sgx = 0; sgx < CHUNK_CELLS; sgx++) {
        var swx = cx * CHUNK_SIZE + sgx * cell + cell * 0.5;
        var swy = cy * CHUNK_SIZE + sgy * cell + cell * 0.5;
        var sdx = swx - sWX, sdy = swy - sWY;
        var sIdx = sgy * CHUNK_CELLS + sgx;
        if (sType2 === 'fortress') {
          var extent = CHUNK_SIZE * 1.7 * _sc;
          var absX = Math.abs(sdx), absY = Math.abs(sdy);
          var insideOuter = (absX < extent && absY < extent);
          var insideInner = (absX < extent - cell * 3 && absY < extent - cell * 3);
          var isGate = false;
          if (absX < cell * 2.5) {
            if (absY > extent - cell * 3.5 && absY < extent + cell * 0.5) isGate = true;
          }
          if (absY < cell * 2.5) {
            if (absX > extent - cell * 3.5 && absX < extent + cell * 0.5) isGate = true;
          }
          var isCorner = (absX > extent - cell * 5 && absX < extent + cell * 0.5 &&
                          absY > extent - cell * 5 && absY < extent + cell * 0.5);
          if (isCorner) {
            cGrid[sIdx] = 1;
            cWallH[sIdx] = structWallH + 0.3;
            cWallCR[sIdx] = _pal.c[0]; cWallCG[sIdx] = _pal.c[1]; cWallCB[sIdx] = _pal.c[2];
          } else if (insideOuter && !insideInner && !isGate) {
            cGrid[sIdx] = 1;
            cWallH[sIdx] = structWallH;
            cWallCR[sIdx] = _pal.w[0]; cWallCG[sIdx] = _pal.w[1]; cWallCB[sIdx] = _pal.w[2];
          } else if (insideInner) {
            cGrid[sIdx] = 0;
            // Central gazebo / greek temple — 6 tall pillars in a wide circle
            var _gazR = cell * 10 * _sc;
            var _gazN = 6;
            for (var _gpi = 0; _gpi < _gazN; _gpi++) {
              var _gpAng = _gpi * Math.PI * 2 / _gazN + Math.PI / 6;
              var _gpx = sWX + Math.cos(_gpAng) * _gazR;
              var _gpy = sWY + Math.sin(_gpAng) * _gazR;
              if (Math.abs(swx - _gpx) < cell * 0.65 && Math.abs(swy - _gpy) < cell * 0.65) {
                cGrid[sIdx] = 1;
                cWallH[sIdx] = structWallH + 0.6;
                cWallCR[sIdx] = _pal.k[0]; cWallCG[sIdx] = _pal.k[1]; cWallCB[sIdx] = _pal.k[2];
                break;
              }
            }
            // Outer decorative marker pillars — shorter, 8-count ring midway to the walls
            var _decR = cell * 20 * _sc;
            var _decN = 8;
            for (var _dpi = 0; _dpi < _decN; _dpi++) {
              var _dpAng = _dpi * Math.PI * 2 / _decN + Math.PI / _decN;
              var _dpx = sWX + Math.cos(_dpAng) * _decR;
              var _dpy = sWY + Math.sin(_dpAng) * _decR;
              if (Math.abs(swx - _dpx) < cell * 0.5 && Math.abs(swy - _dpy) < cell * 0.5) {
                cGrid[sIdx] = 1;
                cWallH[sIdx] = structWallH * 0.55;
                cWallCR[sIdx] = _pal.w[0]; cWallCG[sIdx] = _pal.w[1]; cWallCB[sIdx] = _pal.w[2];
                break;
              }
            }
            // Internal buildings (0–2 based on numBuildings)
            if (cStructure.numBuildings >= 1) {
              var barrDX = sdx - extent * 0.45, barrDY = sdy + extent * 0.4;
              if (Math.abs(barrDX) < cell * 4 && Math.abs(barrDY) < cell * 3) {
                if (Math.abs(barrDX) < cell * 2 && Math.abs(barrDY) < cell * 1.5) {
                  cGrid[sIdx] = 0;
                } else {
                  cGrid[sIdx] = 1;
                  cWallH[sIdx] = structWallH * 0.7;
                  cWallCR[sIdx] = _pal.w[0]; cWallCG[sIdx] = _pal.w[1]; cWallCB[sIdx] = _pal.w[2];
                }
              }
            }
            if (cStructure.numBuildings >= 2) {
              var barr2DX = sdx + extent * 0.45, barr2DY = sdy - extent * 0.4;
              if (Math.abs(barr2DX) < cell * 4 && Math.abs(barr2DY) < cell * 3) {
                if (Math.abs(barr2DX) < cell * 2 && Math.abs(barr2DY) < cell * 1.5) {
                  cGrid[sIdx] = 0;
                } else {
                  cGrid[sIdx] = 1;
                  cWallH[sIdx] = structWallH * 0.7;
                  cWallCR[sIdx] = _pal.w[0]; cWallCG[sIdx] = _pal.w[1]; cWallCB[sIdx] = _pal.w[2];
                }
              }
            }
          }
        } else if (sType2 === 'arena') {
          var dist2 = Math.sqrt(sdx * sdx + sdy * sdy);
          var ringR = CHUNK_SIZE * 1.1 * _sc;
          var ringW2 = cell * 2.5;
          var angle = Math.atan2(sdy, sdx);
          // Gate positions rotated by per-instance rotation
          var gateAng = angle - cStructure.rotation;
          var isArenaGate = (Math.abs(((gateAng + Math.PI) % (Math.PI * 2)) - Math.PI - Math.PI / 2) < 0.2 ||
                             Math.abs(((gateAng + Math.PI) % (Math.PI * 2)) - Math.PI + Math.PI / 2) < 0.2);
          // Outer seating tier
          var outerR = ringR + ringW2 + cell * 2;
          var outerW = cell * 3;
          if (dist2 > outerR - outerW && dist2 < outerR && !isArenaGate) {
            cGrid[sIdx] = 1;
            cWallH[sIdx] = structWallH * 0.5;
            cWallCR[sIdx] = _pal.s[0]; cWallCG[sIdx] = _pal.s[1]; cWallCB[sIdx] = _pal.s[2];
          }
          // Main wall ring
          if (dist2 > ringR - ringW2 && dist2 < ringR + ringW2 && !isArenaGate) {
            cGrid[sIdx] = 1;
            cWallH[sIdx] = structWallH;
            cWallCR[sIdx] = _pal.w[0]; cWallCG[sIdx] = _pal.w[1]; cWallCB[sIdx] = _pal.w[2];
          } else if (dist2 < ringR - ringW2) {
            cGrid[sIdx] = 0;
            // Pillar columns (variable count from numPillars)
            var pillarR = ringR * 0.55;
            var _nPil = cStructure.numPillars;
            for (var pi = 0; pi < _nPil; pi++) {
              var pAng = pi * Math.PI * 2 / _nPil + cStructure.rotation;
              var px = sWX + Math.cos(pAng) * pillarR;
              var py = sWY + Math.sin(pAng) * pillarR;
              if (Math.abs(swx - px) < cell * 0.7 && Math.abs(swy - py) < cell * 0.7) {
                cGrid[sIdx] = 1;
                cWallH[sIdx] = structWallH + 0.2;
                cWallCR[sIdx] = _pal.p[0]; cWallCG[sIdx] = _pal.p[1]; cWallCB[sIdx] = _pal.p[2];
              }
            }
            // Inner pillar ring (half the count)
            var innerPillarR = ringR * 0.3;
            var _nInner = Math.max(4, Math.floor(_nPil / 2));
            for (var pi2 = 0; pi2 < _nInner; pi2++) {
              var pAng2 = pi2 * Math.PI * 2 / _nInner + cStructure.rotation + Math.PI / _nInner;
              var px2 = sWX + Math.cos(pAng2) * innerPillarR;
              var py2 = sWY + Math.sin(pAng2) * innerPillarR;
              if (Math.abs(swx - px2) < cell * 0.7 && Math.abs(swy - py2) < cell * 0.7) {
                cGrid[sIdx] = 1;
                cWallH[sIdx] = structWallH + 0.1;
                cWallCR[sIdx] = _pal.p[0]; cWallCG[sIdx] = _pal.p[1]; cWallCB[sIdx] = _pal.p[2];
              }
            }
            // Short central pedestal — single cell, open approach from all sides
            if (dist2 <= cell * 0.55) {
              cGrid[sIdx] = 1;
              cWallH[sIdx] = structWallH * 0.18; // short pedestal so items float visibly above
              cWallCR[sIdx] = _pal.p[0]; cWallCG[sIdx] = _pal.p[1]; cWallCB[sIdx] = _pal.p[2];
            }
          }
        } else { // watchtower
          var towerR = cell * 3 * _sc;
          if (Math.abs(sdx) < towerR && Math.abs(sdy) < towerR) {
            if (Math.abs(sdx) < cell * 1 && Math.abs(sdy) < cell * 1) {
              cGrid[sIdx] = 0;
            } else {
              cGrid[sIdx] = 1;
              cWallH[sIdx] = structWallH * 1.4;
              cWallCR[sIdx] = _pal.t[0]; cWallCG[sIdx] = _pal.t[1]; cWallCB[sIdx] = _pal.t[2];
            }
          }
          // Variable arm count (2–4) with rotation
          var armLen = cell * 14 * _sc, armW = cell * 1;
          var _nArms = cStructure.armCount;
          for (var ai = 0; ai < _nArms; ai++) {
            var aAng = cStructure.rotation + ai * Math.PI * 2 / _nArms;
            var aCos = Math.cos(aAng), aSin = Math.sin(aAng);
            // Rotate sdx/sdy into arm-local space
            var aLocal = sdx * aCos + sdy * aSin;  // along arm
            var aPerp = -sdx * aSin + sdy * aCos;  // perpendicular
            if (Math.abs(aPerp) < armW && aLocal > towerR && aLocal < towerR + armLen) {
              cGrid[sIdx] = 1;
              cWallH[sIdx] = structWallH * 0.7;
              cWallCR[sIdx] = _pal.a[0]; cWallCG[sIdx] = _pal.a[1]; cWallCB[sIdx] = _pal.a[2];
            }
            // End room at tip of each arm
            var roomR = cell * 2.5 * _sc;
            var roomCX = Math.cos(aAng) * (towerR + armLen);
            var roomCY = Math.sin(aAng) * (towerR + armLen);
            var rdx3 = sdx - roomCX, rdy3 = sdy - roomCY;
            if (Math.abs(rdx3) < roomR && Math.abs(rdy3) < roomR) {
              if (Math.abs(rdx3) < cell * 1 && Math.abs(rdy3) < cell * 1) {
                cGrid[sIdx] = 0;
              } else {
                var crenHash = (Math.floor(sdx / cell) + Math.floor(sdy / cell)) % 2;
                cGrid[sIdx] = 1;
                cWallH[sIdx] = (crenHash === 0) ? structWallH * 0.9 : structWallH * 0.6;
                cWallCR[sIdx] = _pal.a[0]; cWallCG[sIdx] = _pal.a[1]; cWallCB[sIdx] = _pal.a[2];
              }
            }
          }
        }
      }
    }
  }

  // ── Underground cave network carving ──
  var cCaveNets = [];   // cave networks that overlap this chunk
  var cCaveEntrances = [];
  var cCaveInterior = new Uint8Array(CHUNK_CELLS * CHUNK_CELLS); // 1 = cave-carved cell
  if (ENDLESS_MODE) {
    var REGION_CHUNKS_CV = 3;
    var cvRegionX = Math.floor(cx / REGION_CHUNKS_CV);
    var cvRegionY = Math.floor(cy / REGION_CHUNKS_CV);
    var chunkWX0 = cx * CHUNK_SIZE, chunkWY0 = cy * CHUNK_SIZE;
    var chunkWX1 = chunkWX0 + CHUNK_SIZE, chunkWY1 = chunkWY0 + CHUNK_SIZE;
    for (var cvdy = -1; cvdy <= 1; cvdy++) {
      for (var cvdx = -1; cvdx <= 1; cvdx++) {
        var cvRX = cvRegionX + cvdx, cvRY = cvRegionY + cvdy;
        var net = generateCaveNetwork(cvRX, cvRY);
        if (!net) continue;
        // Quick AABB check: does any corridor/chamber/entrance overlap this chunk?
        var overlaps = false;
        var margin = 100; // generous margin for wide corridors
        for (var _ci = 0; _ci < net.corridors.length && !overlaps; _ci++) {
          var _c = net.corridors[_ci];
          var _cMinX = Math.min(_c.x1, _c.x2) - _c.width;
          var _cMaxX = Math.max(_c.x1, _c.x2) + _c.width;
          var _cMinY = Math.min(_c.y1, _c.y2) - _c.width;
          var _cMaxY = Math.max(_c.y1, _c.y2) + _c.width;
          if (_cMaxX > chunkWX0 - margin && _cMinX < chunkWX1 + margin &&
              _cMaxY > chunkWY0 - margin && _cMinY < chunkWY1 + margin) overlaps = true;
        }
        for (var _chi = 0; _chi < net.chambers.length && !overlaps; _chi++) {
          var _ch = net.chambers[_chi];
          if (_ch.cx + _ch.radius > chunkWX0 - margin && _ch.cx - _ch.radius < chunkWX1 + margin &&
              _ch.cy + _ch.radius > chunkWY0 - margin && _ch.cy - _ch.radius < chunkWY1 + margin) overlaps = true;
        }
        // Entrance reach must cover the entire entrance zone footprint so
        // chunks near an entrance include its networks and entrance list.
        // Derive from zone constants (see queryEntranceZone): zone extends
        // OUTER_LEN=480u outward and INNER_LEN=220u inward from each
        // entrance, with a lateral feather. Use the outward reach (always
        // the larger) + a small margin so chunks catch everything.
        var entReach = ENTRANCE_ZONE_REACH;
        for (var _eni = 0; _eni < net.entrances.length && !overlaps; _eni++) {
          var _en = net.entrances[_eni];
          if (_en.x + entReach > chunkWX0 && _en.x - entReach < chunkWX1 &&
              _en.y + entReach > chunkWY0 && _en.y - entReach < chunkWY1) overlaps = true;
        }
        if (overlaps) {
          cCaveNets.push(net);
          // Use same reach for entrance-list collection so every chunk within
          // the zone footprint knows about the entrance. Previously this was
          // a smaller number (260) than the zone's outward extent (480),
          // leaving an annulus of chunks with no entrance data — seam bug.
          for (var _ei2 = 0; _ei2 < net.entrances.length; _ei2++) {
            var _e = net.entrances[_ei2];
            if (_e.x + entReach > chunkWX0 && _e.x - entReach < chunkWX1 &&
                _e.y + entReach > chunkWY0 && _e.y - entReach < chunkWY1) {
              cCaveEntrances.push(Object.assign({}, _e));
            }
          }
        }
      }
    }
    // Carve grid-wall cells using the unified cave geometry query. Single
    // rule: wherever queryCaveGeometry returns wallCarve=true, open the
    // cell. Wall boundaries and approach ramps are both handled by that
    // one flag — no separate zone/cave logic here.
    if (cCaveNets.length > 0) {
      for (var cgy = 0; cgy < CHUNK_CELLS; cgy++) {
        for (var cgx = 0; cgx < CHUNK_CELLS; cgx++) {
          var cwx = cx * CHUNK_SIZE + cgx * cell + cell * 0.5;
          var cwy = cy * CHUNK_SIZE + cgy * cell + cell * 0.5;
          var cidx = cgy * CHUNK_CELLS + cgx;
          var _cgCarve = queryCaveGeometry(cwx, cwy, cCaveNets, getEndlessNaturalSurfaceH(cwx, cwy));
          // A rock bevel is solid even where the original surface maze was
          // already open. Validate the cell's corner samples too: the floor
          // mesh is vertex-aligned, not sampled at this grid cell's center.
          var _cellCovered = !!(_cgCarve && _cgCarve.covered);
          var _cellWalkable = !_cellCovered || _cgCarve.wallCarve;
          var _cellFloorMin = _cgCarve ? _cgCarve.floorH : getEndlessNaturalSurfaceH(cwx, cwy);
          var _cellFloorMax = _cellFloorMin;
          // Check all corners even when the center is outside the cavity;
          // otherwise a thin roof/rock edge can live in an open grid cell.
          if (_cgCarve || cCaveNets.length) {
            for (var _vcy = -1; _vcy <= 1; _vcy += 2) {
              for (var _vcx = -1; _vcx <= 1; _vcx += 2) {
                var _vx = cwx + _vcx * cell * 0.5, _vy = cwy + _vcy * cell * 0.5;
                var _vs = getEndlessNaturalSurfaceH(_vx, _vy);
                var _vg = queryCaveGeometry(_vx, _vy, cCaveNets, _vs);
                var _vh = _vg ? _vg.floorH : _vs;
                _cellFloorMin = Math.min(_cellFloorMin, _vh);
                _cellFloorMax = Math.max(_cellFloorMax, _vh);
                if (_vg && _vg.covered) _cellCovered = true;
                if (_vg && _vg.covered && !_vg.wallCarve) _cellWalkable = false;
              }
            }
          }
          if (_cellCovered) {
            if (_cellFloorMax - _cellFloorMin > 0.75) _cellWalkable = false;
            cGrid[cidx] = _cellWalkable ? 0 : 1;
            cCaveInterior[cidx] = 1;
            continue;
          }
          if (_cgCarve && _cgCarve.wallCarve) {
            cGrid[cidx] = 0;
            cCaveInterior[cidx] = 1;
          }
        }
      }
    }
  }

  // Build wall rects from grid (merge horizontal runs)
  for (var wy2 = 0; wy2 < CHUNK_CELLS; wy2++) {
    var runStart = -1;
    for (var wx2 = 0; wx2 <= CHUNK_CELLS; wx2++) {
      var isWall = (wx2 < CHUNK_CELLS) && cGrid[wy2 * CHUNK_CELLS + wx2];
      if (isWall && runStart < 0) runStart = wx2;
      else if (!isWall && runStart >= 0) {
        cWalls.push({
          x: cx * CHUNK_SIZE + runStart * cell,
          y: cy * CHUNK_SIZE + wy2 * cell,
          w: (wx2 - runStart) * cell,
          h: cell
        });
        runStart = -1;
      }
    }
  }

  // ── Floor mesh ──
  var meshGridSize = GAME_CONFIG.world.meshGridSize;
  var meshW = Math.ceil(CHUNK_SIZE / meshGridSize);
  var meshH = Math.ceil(CHUNK_SIZE / meshGridSize);
  var cHeights = new Float32Array(meshW * meshH);
  var cColors = new Array(meshW * meshH);
  for (var my = 0; my < meshH; my++) {
    for (var mx = 0; mx < meshW; mx++) {
      var fwx = cx * CHUNK_SIZE + mx * meshGridSize;
      var fwy = cy * CHUNK_SIZE + my * meshGridSize;
      var h = getEndlessNaturalSurfaceH(fwx, fwy);
      cHeights[my * meshW + mx] = h;
      cColors[my * meshW + mx] = getFloorColorBlended(fwx, fwy, h);
    }
  }

  // ── Flatten floor inside structures + smooth transition zone ──
  if (cStructure) {
    var flatH = geographyNoise(cStructure.centerWX, cStructure.centerWY);
    flatH = (flatH - 0.5) * 1.0; // mild elevation, not full geography offset
    // Inner radius = fully flat, outer radius = blend to natural terrain
    var _fsc = cStructure.scale;
    var sInner, sOuter, sTransition;
    if (cStructure.type === 'fortress') {
      sInner = CHUNK_SIZE * 1.75 * _fsc; sOuter = CHUNK_SIZE * 2.2 * _fsc;
    } else if (cStructure.type === 'arena') {
      sInner = CHUNK_SIZE * 1.25 * _fsc; sOuter = CHUNK_SIZE * 1.7 * _fsc;
    } else {
      sInner = CHUNK_SIZE * 1.55 * _fsc; sOuter = CHUNK_SIZE * 2.0 * _fsc;
    }
    sTransition = sOuter - sInner;
    for (var fy = 0; fy < meshH; fy++) {
      for (var fx = 0; fx < meshW; fx++) {
        var ffwx = cx * CHUNK_SIZE + fx * meshGridSize;
        var ffwy = cy * CHUNK_SIZE + fy * meshGridSize;
        var fdx2 = ffwx - cStructure.centerWX, fdy2 = ffwy - cStructure.centerWY;
        // Distance metric: use Chebyshev (max of abs) for box structures, Euclidean for arena
        var sDist;
        if (cStructure.type === 'arena') {
          sDist = Math.sqrt(fdx2 * fdx2 + fdy2 * fdy2);
        } else {
          sDist = Math.max(Math.abs(fdx2), Math.abs(fdy2));
        }
        if (sDist < sOuter) {
          var fi2 = fy * meshW + fx;
          var naturalH = cHeights[fi2];
          if (sDist < sInner) {
            // Fully inside — flat
            cHeights[fi2] = flatH;
          } else {
            // Transition zone — smooth blend from flat to natural
            var t = (sDist - sInner) / sTransition; // 0 at inner edge, 1 at outer edge
            t = t * t * (3 - 2 * t); // smoothstep for nice curve
            cHeights[fi2] = flatH * (1 - t) + naturalH * t;
          }
          // Tint floor inside the inner zone
          if (sDist < sInner) {
            var baseFloor = getFloorColorBlended(ffwx, ffwy, flatH);
            var _fr = parseInt(baseFloor.substring(1,3), 16);
            var _fg = parseInt(baseFloor.substring(3,5), 16);
            var _fb = parseInt(baseFloor.substring(5,7), 16);
            var _ba = 0.45; // blend amount
            var _pf = cStructure.palette.f;
            _fr = Math.floor(_fr * (1 - _ba) + _pf[0] * _ba);
            _fg = Math.floor(_fg * (1 - _ba) + _pf[1] * _ba);
            _fb = Math.floor(_fb * (1 - _ba) + _pf[2] * _ba);
            cColors[fi2] = '#' + ((1<<24)|(_fr<<16)|(_fg<<8)|_fb).toString(16).slice(1);
          }
        }
      }
    }
  }

  // Entrance shaping is sampled in world space below. Do not blur chunks
  // independently: truncated kernels at chunk edges created floor seams.

  // Snapshot the pristine per-cell surface color AND (smoothed-near-entrance)
  // height BEFORE cave stamping — used by the cap layer during window assembly.
  var cSurfaceBiome = new Array(meshW * meshH);
  var cSurfaceH = new Float32Array(meshW * meshH);
  for (var _sci = 0; _sci < cSurfaceBiome.length; _sci++) {
    cSurfaceBiome[_sci] = cColors[_sci];
    cSurfaceH[_sci] = cHeights[_sci];
  }

  // ── Cave stamping via unified queryCaveGeometry ──
  // Single source of truth. For each mesh cell we call queryCaveGeometry,
  // which returns either null (no cave influence) or a record with the
  // target floor/ceiling Z and blend factor. All depth-gating, smoothed-
  // surface clamping, corridor/chamber/approach logic lives in that one
  // function. This loop just reads and stamps.
  var cCaveCeilH = new Float32Array(meshW * meshH);
  // Parallel flag: 1 when we've stamped a ceiling at this cell. Needed
  // because the actual ceiling Z can be zero or negative (deep caves,
  // low-surface regions) and Float32Array defaults to 0 — so a numeric
  // test for "has ceiling" is ambiguous.
  var cCaveHasCeil = new Uint8Array(meshW * meshH);
  var _cgDbg = { total: 0, corridor: 0, chamber: 0, approach: 0, mouth: 0, carved: 0 };
  if (cCaveNets.length > 0) {
    for (var cmy = 0; cmy < meshH; cmy++) {
      for (var cmx = 0; cmx < meshW; cmx++) {
        var cmwx = cx * CHUNK_SIZE + cmx * meshGridSize;
        var cmwy = cy * CHUNK_SIZE + cmy * meshGridSize;
        var cmIdx = cmy * meshW + cmx;

        var cg = queryCaveGeometry(cmwx, cmwy, cCaveNets, cSurfaceH[cmIdx]);
        if (!cg) continue;

        _cgDbg.total++;
        if (cg.source === 'corridor') _cgDbg.corridor++;
        else if (cg.source === 'chamber') _cgDbg.chamber++;
        else if (cg.source === 'approach') _cgDbg.approach++;
        if (cg.isEntrance) _cgDbg.mouth++;
        if (cg.wallCarve) _cgDbg.carved++;

        // Geometry owns the final floor, roof, and (entrance-only) cover bank.
        cHeights[cmIdx] = cg.floorH;
        cSurfaceH[cmIdx] = cg.surfaceH;

        // Ceiling (only when the geometry record has one — approach has no ceiling).
        if (cg.ceilZ !== null) { cCaveCeilH[cmIdx] = cg.ceilZ; cCaveHasCeil[cmIdx] = 1; }

        // Material is authored once with the finished layers below. A separate
        // hard threshold here used to recolor the mouth floor abruptly.
      }
    }
    if (DEBUG_CAVE) {
      console.log('[CAVE-GEOM chunk ' + cx + ',' + cy + '] total=' + _cgDbg.total +
        ' corridor=' + _cgDbg.corridor + ' chamber=' + _cgDbg.chamber +
        ' approach=' + _cgDbg.approach + ' mouth=' + _cgDbg.mouth + ' carved=' + _cgDbg.carved);
    }
  }

  // Entrance clearance pass and rim-lip hook DELETED — both were band-aids
  // for the earlier multi-system conflict. queryCaveGeometry now owns the
  // full contract (depth-gated floor, clamped ceiling, smooth boundary
  // fade, entrance mouth flag) so there's nothing left for them to fix.

  // ── Water in valleys ──
  // Reuses the same water types as level-mode generateWaterFeatures():
  //   0 = dry, 1 = still pool, 2 = flowing stream
  // Renderer already handles these in drawPlatforms3D.
  var WATER_THRESH = -3.0;
  var cWater = new Uint8Array(meshW * meshH);
  var isIce = (biome === 'ice');
  var isForest = (biome === 'forest');
  var poolDeep  = isIce ? '#1a2a3a' : isForest ? '#0e2a18' : '#0e1e30';
  var poolShall = isIce ? '#2a3a4a' : isForest ? '#1a3a22' : '#152a45';
  for (var wi = 0; wi < meshW * meshH; wi++) {
    if (cHeights[wi] < WATER_THRESH) {
      // Skip cave interiors and entrance-zone cells — those are dirt
      // corridors/ramps, not water. Otherwise the approach ramp (dipping
      // below -3) gets stamped as pool and renders blue.
      if (cCaveHasCeil[wi]) continue; // cave cell (has ceiling)
      var _wwx = cx * CHUNK_SIZE + (wi % meshW) * meshGridSize + meshGridSize * 0.5;
      var _wwy = cy * CHUNK_SIZE + ((wi / meshW) | 0) * meshGridSize + meshGridSize * 0.5;
      // Skip cells that queryCaveGeometry claims — approach ramps etc.
      if (cCaveNets.length > 0 && queryCaveGeometry(_wwx, _wwy, cCaveNets, 2.0)) continue;
      cWater[wi] = 1;
      cColors[wi] = (cHeights[wi] < WATER_THRESH - 0.3) ? poolDeep : poolShall;
    }
  }

  // ── Slope shading — improve depth perception on terrain ──
  // Two components combined at generation time (zero render cost):
  // 1. Ambient occlusion: valleys darker, ridges lighter (symmetric)
  // 2. Directional light: slopes facing sun (NW) brighter, facing away darker
  for (var sy = 1; sy < meshH - 1; sy++) {
    for (var sx = 1; sx < meshW - 1; sx++) {
      var si = sy * meshW + sx;
      if (cWater[si]) continue;
      var hc = cHeights[si];
      var hN = cHeights[(sy - 1) * meshW + sx];
      var hS = cHeights[(sy + 1) * meshW + sx];
      var hE = cHeights[sy * meshW + sx + 1];
      var hW = cHeights[sy * meshW + sx - 1];
      // 1. Ambient: average neighbor height vs this cell
      var slope = ((hN + hS + hE + hW) * 0.25) - hc;
      // 2. Directional: gradient in X and Y (sun from NW = negative X, negative Y)
      var gradX = (hE - hW) * 0.5;  // positive = slopes up to east
      var gradY = (hS - hN) * 0.5;  // positive = slopes up to south
      // Sun from NW: dot product of gradient with sun direction (-0.7, -0.7)
      var sunDot = (-gradX - gradY) * 0.5; // -1..+1 range, positive = facing sun
      // Combined shading factor
      var shadeFactor = 0;
      // Ambient component
      if (slope > 0.15) shadeFactor -= Math.min(0.12, slope * 0.10);       // valley darken
      else if (slope < -0.15) shadeFactor += Math.min(0.10, -slope * 0.08); // ridge lighten
      // Directional component (stronger effect)
      if (sunDot > 0.1) shadeFactor += Math.min(0.14, sunDot * 0.12);      // sun-facing brighten
      else if (sunDot < -0.1) shadeFactor -= Math.min(0.10, -sunDot * 0.08); // shadow-facing darken
      if (shadeFactor < -0.02 || shadeFactor > 0.02) {
        var col = cColors[si];
        var cr = parseInt(col.substr(1, 2), 16);
        var cg = parseInt(col.substr(3, 2), 16);
        var cb = parseInt(col.substr(5, 2), 16);
        var mult = 1 + shadeFactor;
        cr = Math.max(0, Math.min(255, Math.floor(cr * mult)));
        cg = Math.max(0, Math.min(255, Math.floor(cg * mult)));
        cb = Math.max(0, Math.min(255, Math.floor(cb * mult)));
        cColors[si] = '#' + ((1 << 24) | (cr << 16) | (cg << 8) | cb).toString(16).slice(1);
      }
    }
  }

  // ── Floor scatter ──
  var cScatter = [];
  var scatterRng = chunkRng(cx, cy, 9);
  var scatterPool;
  if (biome === 'cave') scatterPool = ['crystal','stalagmite','rock_pile','puddle','boulder','cave_rubble_pile','rock_spire','bookshelf_debris','iron_chain','barrel'];
  else if (biome === 'ice') scatterPool = ['ice_shard','frozen_pool','icicle_cluster','frost_patch','frozen_skull','cracked_stone'];
  else if (biome === 'plains') scatterPool = ['tall_grass','wildflower','tall_grass','mesa_boulder','stone_marker','flat_rock','dead_shrub','wildflower','tree_stump'];
  else if (biome === 'forest') scatterPool = ['tree_stump','fallen_log','tall_grass','wildflower','mushroom','moss_patch','fern','leaf_pile','tall_grass','fern'];
  else if (biome === 'expanse') scatterPool = ['desert_rock','dead_shrub','dry_bones','stone_column','sand_pillar','cracked_stone','flat_rock'];
  else scatterPool = ['bones','crate','skull','rubble','rib_cage','flat_rock','cracked_stone'];
  var scatterDensity = biome === 'cave' ? 0.025 : biome === 'forest' ? 0.022 : biome === 'ground' ? 0.020 : biome === 'expanse' ? 0.018 : 0.015;
  for (var sy = 0; sy < CHUNK_CELLS; sy++) {
    for (var sx = 0; sx < CHUNK_CELLS; sx++) {
      if (cGrid[sy * CHUNK_CELLS + sx]) continue;
      if (scatterRng() < scatterDensity) {
        cScatter.push({
          x: cx * CHUNK_SIZE + sx * cell + scatterRng() * cell,
          y: cy * CHUNK_SIZE + sy * cell + scatterRng() * cell,
          type: scatterPool[Math.floor(scatterRng() * scatterPool.length)],
          variant: Math.floor(scatterRng() * 4),
          seed: Math.floor(scatterRng() * 10000)
        });
      }
    }
  }

  // ── Wall decorations ──
  var cDecors = [];
  var decorRng = chunkRng(cx, cy, 5);
  for (var dy3 = 0; dy3 < CHUNK_CELLS; dy3++) {
    for (var dx3 = 0; dx3 < CHUNK_CELLS; dx3++) {
      if (!cGrid[dy3 * CHUNK_CELLS + dx3]) continue;
      // Higher torch density near cave interiors (35% vs 15% base)
      var _adjCave = false;
      if (cCaveInterior) {
        for (var _ady = -1; _ady <= 1 && !_adjCave; _ady++) {
          for (var _adx = -1; _adx <= 1 && !_adjCave; _adx++) {
            var _anx = dx3 + _adx, _any = dy3 + _ady;
            if (_anx >= 0 && _anx < CHUNK_CELLS && _any >= 0 && _any < CHUNK_CELLS) {
              if (cCaveInterior[_any * CHUNK_CELLS + _anx]) _adjCave = true;
            }
          }
        }
      }
      if (decorRng() > (_adjCave ? 0.35 : 0.15)) continue;
      // Check for exposed faces
      var sides = [];
      if (dy3 > 0 && !cGrid[(dy3-1)*CHUNK_CELLS+dx3]) sides.push('north');
      if (dy3 < CHUNK_CELLS-1 && !cGrid[(dy3+1)*CHUNK_CELLS+dx3]) sides.push('south');
      if (dx3 > 0 && !cGrid[dy3*CHUNK_CELLS+(dx3-1)]) sides.push('west');
      if (dx3 < CHUNK_CELLS-1 && !cGrid[dy3*CHUNK_CELLS+(dx3+1)]) sides.push('east');
      if (!sides.length) continue;
      var side = sides[Math.floor(decorRng() * sides.length)];
      var dwx = cx * CHUNK_SIZE + dx3 * cell + cell/2;
      var dwy = cy * CHUNK_SIZE + dy3 * cell + cell/2;
      if (side === 'north') dwy -= cell/2;
      else if (side === 'south') dwy += cell/2;
      else if (side === 'west') dwx -= cell/2;
      else if (side === 'east') dwx += cell/2;
      // Cave-adjacent walls use cave decoration set with extra torches
      var dTypes = (_adjCave) ? ['torch','torch','torch','stalactite','crack','fungi','moss'] :
                   (biome === 'cave') ? ['fungi','stalactite','crack','moss','torch','torch'] :
                   (biome === 'ice')  ? ['icicle','frost_crack','torch'] :
                   (biome === 'forest') ? ['vine_growth','moss_drip','vine_growth','carved_rune','moss_drip','torch'] :
                   ['torch','shield','crack','vine','banner'];
      cDecors.push({
        worldX: dwx, worldY: dwy, side: side,
        type: dTypes[Math.floor(decorRng() * dTypes.length)],
        gridX: dx3, gridY: dy3
      });
    }
  }

  // ── Structure-specific decorations (torch-heavy) ──
  if (cStructure) {
    var sDecorRng = chunkRng(cx, cy, 77);
    for (var sdy3 = 0; sdy3 < CHUNK_CELLS; sdy3++) {
      for (var sdx3 = 0; sdx3 < CHUNK_CELLS; sdx3++) {
        var _si3 = sdy3 * CHUNK_CELLS + sdx3;
        if (!cGrid[_si3]) continue;
        if (!cWallCR[_si3]) continue; // only structure walls
        if (sDecorRng() > 0.30) continue; // 30% chance
        var sides3 = [];
        if (sdy3 > 0 && !cGrid[(sdy3-1)*CHUNK_CELLS+sdx3]) sides3.push('north');
        if (sdy3 < CHUNK_CELLS-1 && !cGrid[(sdy3+1)*CHUNK_CELLS+sdx3]) sides3.push('south');
        if (sdx3 > 0 && !cGrid[sdy3*CHUNK_CELLS+(sdx3-1)]) sides3.push('west');
        if (sdx3 < CHUNK_CELLS-1 && !cGrid[sdy3*CHUNK_CELLS+(sdx3+1)]) sides3.push('east');
        if (!sides3.length) continue;
        var side3 = sides3[Math.floor(sDecorRng() * sides3.length)];
        var dwx3 = cx * CHUNK_SIZE + sdx3 * cell + cell/2;
        var dwy3 = cy * CHUNK_SIZE + sdy3 * cell + cell/2;
        if (side3 === 'north') dwy3 -= cell/2;
        else if (side3 === 'south') dwy3 += cell/2;
        else if (side3 === 'west') dwx3 -= cell/2;
        else if (side3 === 'east') dwx3 += cell/2;
        // Check for duplicates
        var _dup3 = false;
        for (var _di3 = 0; _di3 < cDecors.length; _di3++) {
          if (Math.abs(cDecors[_di3].worldX - dwx3) < 2 && Math.abs(cDecors[_di3].worldY - dwy3) < 2) { _dup3 = true; break; }
        }
        if (_dup3) continue;
        var sPool = (cStructure.type === 'fortress') ? ['torch','torch','torch','banner','shield','crack'] :
                    (cStructure.type === 'arena') ? ['torch','torch','sconce','crack','banner'] :
                    ['torch','torch','torch','sconce','crack'];
        cDecors.push({
          worldX: dwx3, worldY: dwy3, side: side3,
          type: sPool[Math.floor(sDecorRng() * sPool.length)],
          gridX: sdx3, gridY: sdy3
        });
      }
    }
  }

  // ── Treasure chests ──
  var cChests = [];
  var chestRng = chunkRng(cx, cy, 14);
  var chestChance = cStructure ? 0.70 : 0.35; // structures have double chest rate
  if ((!CAVE_TEST_MODE || caveTestFlags.surfaceChests) && chestRng() < chestChance) {
    // Find a dead-end or open spot for chest
    for (var attempt = 0; attempt < 20; attempt++) {
      var tcx2 = Math.floor(chestRng() * (CHUNK_CELLS - 4)) + 2;
      var tcy2 = Math.floor(chestRng() * (CHUNK_CELLS - 4)) + 2;
      if (cGrid[tcy2 * CHUNK_CELLS + tcx2]) continue;
      // Check 3x3 is clear
      var clear = true;
      for (var tdy = -1; tdy <= 1 && clear; tdy++)
        for (var tdx = -1; tdx <= 1 && clear; tdx++)
          if (cGrid[(tcy2+tdy)*CHUNK_CELLS+(tcx2+tdx)]) clear = false;
      if (!clear) continue;
      var chTier = rollChestTier(chestRng);
      var chTierDef = CHEST_TIER_DEFS[chTier];
      var chQual = rollQuality(chTier, chestRng);
      var chRelicId = null, chEquipId = null, chGold = Math.floor((15 + chestRng() * 30 * difficulty) * chTierDef.goldMult);
      if (chTier === 'epic') {
        chRelicId = pickRandomRelic(chestRng);
        chGold = 0;
      } else if (chestRng() < chTierDef.equipChance) {
        var eqKeys = Object.keys(EQUIPMENT_DEFS);
        chEquipId = eqKeys[Math.floor(chestRng() * eqKeys.length)];
        chGold = 0;
      }
      cChests.push({
        x: cx * CHUNK_SIZE + tcx2 * cell + cell/2,
        y: cy * CHUNK_SIZE + tcy2 * cell + cell/2,
        gold: chGold,
        collected: false, opened: false, lidAngle: 0,
        facing: Math.floor(chestRng() * 4) * Math.PI * 0.5,
        equipId: chEquipId, relicId: chRelicId,
        tier: chTier, quality: chQual,
        seed: Math.floor(chestRng() * 10000)
      });
      break;
    }
  }

  // ── Enemies ──
  var cEnemies = [];
  var enemyRng = chunkRng(cx, cy, 10);
  // Sparse random enemies — most chunks empty, spawners provide the real challenge
  var numEnemies = 0;
  if ((!CAVE_TEST_MODE || caveTestFlags.surfaceEnemies) && enemyRng() < 0.6) numEnemies = Math.min(4, Math.floor(1 + difficulty * 1.0));
  // Don't spawn enemies in the starting chunk or market chunks (safe haven)
  if (cx === 0 && cy === 0) numEnemies = 0;
  if (hasMarket) numEnemies = 0;
  var typeKeys = Object.keys(enemyTypes);
  for (var ei = 0; ei < numEnemies; ei++) {
    var tries = 0;
    while (tries < 15) {
      var ex = Math.floor(enemyRng() * (CHUNK_CELLS - 4)) + 2;
      var ey = Math.floor(enemyRng() * (CHUNK_CELLS - 4)) + 2;
      if (!cGrid[ey * CHUNK_CELLS + ex]) {
        var ewx = cx * CHUNK_SIZE + ex * cell + cell/2;
        var ewy = cy * CHUNK_SIZE + ey * cell + cell/2;
        // Don't spawn within aggro+patrol range of player spawn (center of chunk 0,0)
        if (Math.hypot(ewx - CHUNK_SIZE/2, ewy - CHUNK_SIZE/2) < 900) { tries++; continue; }
        var eTypeKey = typeKeys[Math.floor(enemyRng() * typeKeys.length)];
        var eType = enemyTypes[eTypeKey];
        cEnemies.push({
          x: ewx, y: ewy, z: 0,
          enemyType: eType,
          health: Math.floor(eType.health * difficulty),
          maxHealth: Math.floor(eType.health * difficulty),
          speed: eType.speed * Math.min(1.5, 0.8 + difficulty * 0.15),
          chaseRange: eType.chaseRange,
          lastUpdate: 0, damageFlash: 0, damageFlashColor: '#ffffff',
          slowUntil: 0, burnUntil: 0, burnDmgLast: 0,
          iceHits: 0, fireHits: 0, lightningHits: 0,
          vx: 0, vy: 0, aggroAt: 0,
          attackState: 'idle', attackStateUntil: 0,
          patrolWaypoints: [{x: ewx + (enemyRng()-0.5)*100, y: ewy + (enemyRng()-0.5)*100},
                            {x: ewx + (enemyRng()-0.5)*100, y: ewy + (enemyRng()-0.5)*100}],
          patrolIdx: 0, facing: 0
        });
        break;
      }
      tries++;
    }
  }

  // ── Cave-specific enemies (underground chambers) ──
  if (cCaveNets.length > 0 && (!CAVE_TEST_MODE || caveTestFlags.caveEnemies)) {
    var caveEnemyRng = chunkRng(cx, cy, 102);
    var caveTypeKeys = ['normal', 'tank']; // tougher enemies underground
    for (var _cni = 0; _cni < cCaveNets.length; _cni++) {
      var _cn = cCaveNets[_cni];
      for (var _cci = 0; _cci < _cn.chambers.length; _cci++) {
        var _cch = _cn.chambers[_cci];
        var _chOwner = caveChamberContentPoint(_cch, 0, 0);
        if (_chOwner.ownerCX !== cx || _chOwner.ownerCY !== cy) continue;
        var numCaveEn = 2 + Math.floor(caveEnemyRng() * 2); // 2-3 per chamber
        for (var _cei = 0; _cei < numCaveEn; _cei++) {
          var ceOff = _cch.radius * 0.5;
          var cePoint = caveChamberContentPoint(_cch, (caveEnemyRng() - 0.5) * ceOff,
            (caveEnemyRng() - 0.5) * ceOff);
          var ceX = cePoint.x, ceY = cePoint.y;
          var ceTypeKey = caveTypeKeys[Math.floor(caveEnemyRng() * caveTypeKeys.length)];
          var ceType = enemyTypes[ceTypeKey];
          // Spawn at correct cave floor Z immediately (not z:0 which is surface level)
          var ceFloorZ = -_cch.depth * 25;
          cEnemies.push({
            x: ceX, y: ceY, z: ceFloorZ, underground: true,
            caveSpawnId: _cn.regionX + ',' + _cn.regionY + ':chamber:' + _cci + ':enemy:' + _cei,
            enemyType: ceType,
            health: Math.floor(ceType.health * difficulty * 1.2),
            maxHealth: Math.floor(ceType.health * difficulty * 1.2),
            speed: ceType.speed * Math.min(1.5, 0.8 + difficulty * 0.15),
            chaseRange: ceType.chaseRange * 0.7, // shorter range underground
            lastUpdate: 0, damageFlash: 0, damageFlashColor: '#ffffff',
            slowUntil: 0, burnUntil: 0, burnDmgLast: 0,
            iceHits: 0, fireHits: 0, lightningHits: 0,
            vx: 0, vy: 0, aggroAt: 0,
            attackState: 'idle', attackStateUntil: 0,
            patrolWaypoints: [{x: ceX + (caveEnemyRng()-0.5)*60, y: ceY + (caveEnemyRng()-0.5)*60},
                              {x: ceX + (caveEnemyRng()-0.5)*60, y: ceY + (caveEnemyRng()-0.5)*60}],
            patrolIdx: 0, facing: 0
          });
        }
      }
    }
  }

  // ── Cave treasure chests (terminal chambers) ──
  if (cCaveNets.length > 0 && (!CAVE_TEST_MODE || caveTestFlags.caveChests)) {
    var caveChestRng = chunkRng(cx, cy, 103);
    for (var _cni2 = 0; _cni2 < cCaveNets.length; _cni2++) {
      var _cn2 = cCaveNets[_cni2];
      for (var _cci2 = 0; _cci2 < _cn2.chambers.length; _cci2++) {
        var _cch2 = _cn2.chambers[_cci2];
        if (!_cch2.terminal) continue; // only terminal chambers get chests
        var _tcPoint = caveChamberContentPoint(_cch2, 0, 0);
        if (_tcPoint.ownerCX !== cx || _tcPoint.ownerCY !== cy) continue;
        var tierRoll = caveChestRng() + 0.15; // shift toward higher tiers
        var cTier = tierRoll < 0.3 ? 'common' : tierRoll < 0.65 ? 'uncommon' : tierRoll < 0.85 ? 'rare' : 'epic';
        cChests.push({
          x: _tcPoint.x, y: _tcPoint.y,
          caveSpawnId: _cn2.regionX + ',' + _cn2.regionY + ':chamber:' + _cci2 + ':chest',
          gold: Math.floor(10 + caveChestRng() * 30 * difficulty),
          collected: false, opened: false, lidAngle: 0,
          facing: Math.floor(caveChestRng() * 4) * Math.PI / 2,
          equipId: null, relicId: null,
          tier: cTier, quality: 0.7 + caveChestRng() * 0.3,
          seed: Math.floor(caveChestRng() * 10000)
        });
      }
    }
  }

  // ── Enemy spawners ──
  var cSpawners = [];
  var spawnRng = chunkRng(cx, cy, 15);
  // Structures still always spawn one; wild spawners use 2-chunk exclusion
  // like shrines/markets so spawners don't cluster.
  var spawnerChance = cStructure ? 0.80 : 0.25;
  var wantsSpawner = spawnRng() < spawnerChance;
  if (wantsSpawner && !cStructure) {
    for (var snsy = -2; snsy <= 2 && wantsSpawner; snsy++) {
      for (var snsx = -2; snsx <= 2 && wantsSpawner; snsx++) {
        if (snsx === 0 && snsy === 0) continue;
        var snsRng = chunkRng(cx + snsx, cy + snsy, 15);
        if (snsRng() < 0.25) {
          // Neighbor also wants a wild spawner — lower coord wins.
          if ((cy + snsy) < cy || ((cy + snsy) === cy && (cx + snsx) < cx)) {
            wantsSpawner = false;
          }
        }
      }
    }
  }
  if ((!CAVE_TEST_MODE || caveTestFlags.spawners) && difficulty > (cStructure ? 1.0 : 2.0) && !hasMarket && wantsSpawner) {
    for (var sa = 0; sa < 15; sa++) {
      var ssx = Math.floor(spawnRng() * (CHUNK_CELLS - 6)) + 3;
      var ssy = Math.floor(spawnRng() * (CHUNK_CELLS - 6)) + 3;
      if (!cGrid[ssy * CHUNK_CELLS + ssx]) {
        var swx = cx * CHUNK_SIZE + ssx * cell + cell/2;
        var swy = cy * CHUNK_SIZE + ssy * cell + cell/2;
        // Don't place spawners near player spawn
        if (Math.hypot(swx - CHUNK_SIZE/2, swy - CHUNK_SIZE/2) < 900) break;
        cSpawners.push({
          x: swx, y: swy,
          hp: 6, maxHp: 6,
          cooldownMs: 8000, lastSpawn: 0,
          spawnCount: 0, maxSpawns: 4,
          active: true, type: typeKeys[Math.floor(spawnRng() * typeKeys.length)],
          pulsePhase: spawnRng() * Math.PI * 2
        });
        break;
      }
    }
  }

  // ── Market stalls (replaces old shop marker) ──
  var cMarket = null;
  if (hasMarket) {
    var mCenterCell = Math.floor(CHUNK_CELLS / 2);
    var mCenterX = cx * CHUNK_SIZE + mCenterCell * cell + cell / 2;
    var mCenterY = cy * CHUNK_SIZE + mCenterCell * cell + cell / 2;
    var stallTypes = ['weapons', 'potions', 'scrolls', 'trinkets'];
    // 4 stalls arranged around the center in a square
    var stallOffsets = [
      {dx: -4, dy: -3, facing: Math.PI * 0.5},   // left-top, facing right
      {dx:  4, dy: -3, facing: -Math.PI * 0.5},   // right-top, facing left
      {dx: -4, dy:  3, facing: Math.PI * 0.5},    // left-bottom, facing right
      {dx:  4, dy:  3, facing: -Math.PI * 0.5}    // right-bottom, facing left
    ];
    var mStalls = [];
    for (var si = 0; si < 4; si++) {
      var so = stallOffsets[si];
      mStalls.push({
        x: mCenterX + so.dx * cell,
        y: mCenterY + so.dy * cell,
        facing: so.facing,
        stallType: stallTypes[si]
      });
    }
    cMarket = { centerX: mCenterX, centerY: mCenterY, stalls: mStalls };
  }

  // ── Shrine generation ──
  var cShrine = null;
  if ((!CAVE_TEST_MODE || caveTestFlags.shrines) && !hasMarket && !(cx === 0 && cy === 0)) {
    var shrineRng = chunkRng(cx, cy, 61);
    var hasShrine = shrineRng() < 0.05; // 5% chance
    if (hasShrine) {
      // 2-chunk exclusion zone for shrines
      for (var sny = -2; sny <= 2 && hasShrine; sny++) {
        for (var snx = -2; snx <= 2 && hasShrine; snx++) {
          if (snx === 0 && sny === 0) continue;
          if (cx + snx === 0 && cy + sny === 0) continue;
          var snRng = chunkRng(cx + snx, cy + sny, 61);
          if (snRng() < 0.05) {
            if ((cy + sny) < cy || ((cy + sny) === cy && (cx + snx) < cx)) {
              hasShrine = false;
            }
          }
        }
      }
    }
    if (hasShrine) {
      var buffTypes = ['damage', 'speed', 'regen', 'armor'];
      var buffType = buffTypes[Math.floor(shrineRng() * buffTypes.length)];
      // Place shrine in a 5x5 cleared area offset from center
      var shX = Math.floor(CHUNK_CELLS / 2) + Math.floor(shrineRng() * 5) - 2;
      var shY = Math.floor(CHUNK_CELLS / 2) + Math.floor(shrineRng() * 5) - 2;
      shX = Math.max(3, Math.min(CHUNK_CELLS - 4, shX));
      shY = Math.max(3, Math.min(CHUNK_CELLS - 4, shY));
      // Clear 5x5 area around shrine
      for (var sdy = -2; sdy <= 2; sdy++) {
        for (var sdx = -2; sdx <= 2; sdx++) {
          var sgx = shX + sdx, sgy = shY + sdy;
          if (sgx >= 0 && sgx < CHUNK_CELLS && sgy >= 0 && sgy < CHUNK_CELLS) {
            cGrid[sgy * CHUNK_CELLS + sgx] = 0;
          }
        }
      }
      cShrine = {
        x: cx * CHUNK_SIZE + shX * cell + cell / 2,
        y: cy * CHUNK_SIZE + shY * cell + cell / 2,
        buffType: buffType,
        used: false
      };
    }
  }

  // ── Ruin generation ──
  var cRuin = null;
  if (!hasMarket && !cShrine && !cStructure && !(cx === 0 && cy === 0) && (biome === 'ground' || biome === 'plains' || biome === 'forest' || biome === 'expanse')) {
    var ruinRng = chunkRng(cx, cy, 70);
    var hasRuin = ruinRng() < 0.04; // 4% chance
    if (hasRuin) {
      // 2-chunk exclusion zone
      for (var rny = -2; rny <= 2 && hasRuin; rny++) {
        for (var rnx = -2; rnx <= 2 && hasRuin; rnx++) {
          if (rnx === 0 && rny === 0) continue;
          var rnBiome = getBiomeAt((cx + rnx) * CHUNK_SIZE + CHUNK_SIZE/2, (cy + rny) * CHUNK_SIZE + CHUNK_SIZE/2);
          if (rnBiome !== 'ground' && rnBiome !== 'plains' && rnBiome !== 'forest' && rnBiome !== 'expanse') continue;
          var rnRng = chunkRng(cx + rnx, cy + rny, 70);
          if (rnRng() < 0.04) {
            if ((cy + rny) < cy || ((cy + rny) === cy && (cx + rnx) < cx)) {
              hasRuin = false;
            }
          }
        }
      }
    }
    if (hasRuin) {
      var ruinTypes = ['hut', 'tower_base', 'hall'];
      var ruinType = ruinTypes[Math.floor(ruinRng() * ruinTypes.length)];
      var facing = Math.floor(ruinRng() * 4); // 0=N, 1=E, 2=S, 3=W (open side)
      // Place near chunk center
      var ruX = Math.floor(CHUNK_CELLS / 2) + Math.floor(ruinRng() * 5) - 2;
      var ruY = Math.floor(CHUNK_CELLS / 2) + Math.floor(ruinRng() * 5) - 2;
      var ruinCells = [];
      var ruinWallH = 0.35 + ruinRng() * 0.15; // short ruined walls

      if (ruinType === 'hut') {
        // 3x3 with one open side
        for (var rdy = -1; rdy <= 1; rdy++) {
          for (var rdx = -1; rdx <= 1; rdx++) {
            if (rdx === 0 && rdy === 0) continue; // hollow inside
            // Open side based on facing
            if (facing === 0 && rdy === -1 && rdx === 0) continue;
            if (facing === 1 && rdx === 1 && rdy === 0) continue;
            if (facing === 2 && rdy === 1 && rdx === 0) continue;
            if (facing === 3 && rdx === -1 && rdy === 0) continue;
            ruinCells.push({dx: rdx, dy: rdy});
          }
        }
      } else if (ruinType === 'tower_base') {
        // 2x2 solid short walls
        for (var rdy2 = 0; rdy2 <= 1; rdy2++) {
          for (var rdx2 = 0; rdx2 <= 1; rdx2++) {
            ruinCells.push({dx: rdx2, dy: rdy2});
          }
        }
      } else { // hall
        // 5x5 footprint: perimeter walls with an open side + 3-cell interior.
        // Open side has 3 cells removed so the player can walk in. Interior
        // gets two short pillars NOT on the central path so passage is clear.
        for (var rdy3 = -2; rdy3 <= 2; rdy3++) {
          for (var rdx3 = -2; rdx3 <= 2; rdx3++) {
            var isPerim = (rdx3 === -2 || rdx3 === 2 || rdy3 === -2 || rdy3 === 2);
            if (!isPerim) continue; // interior stays clear
            // Open side: skip the middle 3 perimeter cells on that face
            if (facing === 0 && rdy3 === -2 && Math.abs(rdx3) <= 1) continue; // north
            if (facing === 1 && rdx3 === 2  && Math.abs(rdy3) <= 1) continue; // east
            if (facing === 2 && rdy3 === 2  && Math.abs(rdx3) <= 1) continue; // south
            if (facing === 3 && rdx3 === -2 && Math.abs(rdy3) <= 1) continue; // west
            ruinCells.push({dx: rdx3, dy: rdy3});
          }
        }
        // Two decorative interior pillars along the side walls — they sit
        // at (±1, ±1) corners so they hug the walls and don't obstruct the
        // central 3-wide path through the hall.
        ruinCells.push({dx: -1, dy: -1});
        ruinCells.push({dx:  1, dy:  1});
      }

      // Stamp ruin walls into grid
      var ruinValid = true;
      for (var rci = 0; rci < ruinCells.length; rci++) {
        var rgx = ruX + ruinCells[rci].dx, rgy = ruY + ruinCells[rci].dy;
        if (rgx < 1 || rgx >= CHUNK_CELLS - 1 || rgy < 1 || rgy >= CHUNK_CELLS - 1) { ruinValid = false; break; }
      }
      if (ruinValid) {
        for (var rci2 = 0; rci2 < ruinCells.length; rci2++) {
          var rgx2 = ruX + ruinCells[rci2].dx, rgy2 = ruY + ruinCells[rci2].dy;
          cGrid[rgy2 * CHUNK_CELLS + rgx2] = 1;
          cWallH[rgy2 * CHUNK_CELLS + rgx2] = ruinWallH;
        }
        // Clear floor inside for hut. Hall geometry already builds its
        // open side into the perimeter-wall loop above (and doesn't mark
        // interior cells as walls to begin with), so no post-stamp clearing.
        if (ruinType === 'hut') {
          cGrid[ruY * CHUNK_CELLS + ruX] = 0;
        }
        cRuin = {
          x: cx * CHUNK_SIZE + ruX * cell + cell / 2,
          y: cy * CHUNK_SIZE + ruY * cell + cell / 2,
          ruinType: ruinType, facing: facing,
          cells: ruinCells
        };
        // 15% chance of a stat pickup inside this ruin
        var spRng = chunkRng(cx, cy, 90);
        if (spRng() < 0.15) {
          var spTypes = ['heartCrystal', 'manaStar', 'movementTome'];
          cRuin.statPickup = {type: spTypes[Math.floor(spRng() * spTypes.length)], wx: cRuin.x, wy: cRuin.y};
        }
        // Rebuild wall rects since we stamped new walls
        cWalls = [];
        for (var rwy = 0; rwy < CHUNK_CELLS; rwy++) {
          var rRunStart = -1;
          for (var rwx = 0; rwx <= CHUNK_CELLS; rwx++) {
            var rIsWall = (rwx < CHUNK_CELLS) && cGrid[rwy * CHUNK_CELLS + rwx];
            if (rIsWall && rRunStart < 0) rRunStart = rwx;
            else if (!rIsWall && rRunStart >= 0) {
              cWalls.push({x: cx * CHUNK_SIZE + rRunStart * cell, y: cy * CHUNK_SIZE + rwy * cell, w: (rwx - rRunStart) * cell, h: cell});
              rRunStart = -1;
            }
          }
        }
      }
    }
  }

  // Per-chunk terrain debug
  var _chGeo = geographyNoise(chunkCenterWX, chunkCenterWY);
  var _chWater = 0;
  for (var _wi2 = 0; _wi2 < cWater.length; _wi2++) if (cWater[_wi2]) _chWater++;
  console.log('[CHUNK ' + cx + ',' + cy + '] biome=' + biome + ' geo=' + _chGeo.toFixed(3) +
    ' wallThresh=' + wallThresh.toFixed(3) + ' walls=' + cWalls.length +
    ' water=' + _chWater +
    (cStructure ? ' STRUCTURE=' + cStructure.type : '') +
    (cRuin ? ' ruin=' + cRuin.ruinType : '') +
    (hasMarket ? ' MARKET' : ''));

  // ── Convert heights/ceilH → per-chunk layered field ──
  // Generation writes to cHeights/cCaveCeilH through many code paths (noise,
  // caves, ramps, structures, water). Rather than refactor each write-site
  // to the layered form, we convert the finished arrays into layers here.
  // heights/ceilH become purely internal to this function.
  var _chN = meshW * meshH;
  var cLayerCount = new Uint8Array(_chN);
  var cL0TopZ = new Float32Array(_chN), cL0Type = new Uint8Array(_chN), cL0Color = new Array(_chN);
  var cL1TopZ = new Float32Array(_chN), cL1Type = new Uint8Array(_chN), cL1Color = new Array(_chN);
  var cL2TopZ = new Float32Array(_chN), cL2Type = new Uint8Array(_chN), cL2Color = new Array(_chN);
  var cL3TopZ = new Float32Array(_chN), cL3Type = new Uint8Array(_chN), cL3Color = new Array(_chN);
  var cL4TopZ = new Float32Array(_chN), cL4Type = new Uint8Array(_chN), cL4Color = new Array(_chN);
  // One shared rock material for every cavity-facing surface. Author against
  // world-space portals here; rendering only reads the cached packed color.
  // The field includes neighboring solid cells so the wall/portal cut faces
  // sample the same palette even when their midpoint sits outside the cavity.
  var cCaveStone = new Uint32Array(_chN);
  for (var _chI = 0; _chI < _chN; _chI++) {
    var _chFh = cHeights[_chI];
    var _chCh = cCaveCeilH[_chI];
    var _matWX = cx * CHUNK_SIZE + (_chI % meshW) * meshGridSize;
    var _matWY = cy * CHUNK_SIZE + Math.floor(_chI / meshW) * meshGridSize;
    cCaveStone[_chI] = sampleCaveMaterialColor(cSurfaceBiome[_chI], _matWX, _matWY, cCaveEntrances);
    cL0TopZ[_chI] = _chFh; cL0Type[_chI] = 1;
    if (cCaveHasCeil[_chI]) {
      cL0Color[_chI] = caveMaterialColorHex(cCaveStone[_chI]);
      cL1TopZ[_chI] = _chCh; cL1Type[_chI] = 2;
      cL1Color[_chI] = cL0Color[_chI];
      cLayerCount[_chI] = 2;
    } else {
      cL0Color[_chI] = cColors[_chI];
      cLayerCount[_chI] = 1;
    }
  }

  var chunk = {
    cx: cx, cy: cy, biome: biome, difficulty: difficulty,
    grid: cGrid, wallHeights: cWallH, wallColorR: cWallCR, wallColorG: cWallCG, wallColorB: cWallCB, walls: cWalls,
    floorMesh: {w: meshW, h: meshH, gridSize: meshGridSize, water: cWater,
                surfaceBiome: cSurfaceBiome, surfaceH: cSurfaceH, caveStone: cCaveStone,
                layerCount: cLayerCount,
                l0TopZ: cL0TopZ, l0Type: cL0Type, l0Color: cL0Color,
                l1TopZ: cL1TopZ, l1Type: cL1Type, l1Color: cL1Color,
                l2TopZ: cL2TopZ, l2Type: cL2Type, l2Color: cL2Color,
                l3TopZ: cL3TopZ, l3Type: cL3Type, l3Color: cL3Color,
                l4TopZ: cL4TopZ, l4Type: cL4Type, l4Color: cL4Color},
    floorScatter: cScatter, wallDecorations: cDecors,
    treasureChests: cChests, enemySpawners: cSpawners,
    enemies: cEnemies, market: cMarket, shrine: cShrine, ruin: cRuin, structure: cStructure,
    caveEntrances: cCaveEntrances, caveInterior: cCaveInterior,
    exploredCells: new Uint8Array(CHUNK_CELLS * CHUNK_CELLS),
    lastAccess: Date.now()
  };

  // Apply entity deltas if any
  if (entityDeltas[key]) {
    var deltas = entityDeltas[key];
    if (deltas.chestsCollected) {
      for (var di = 0; di < deltas.chestsCollected.length; di++) {
        var ci2 = deltas.chestsCollected[di];
        if (ci2 < chunk.treasureChests.length) chunk.treasureChests[ci2].collected = true;
      }
    }
    if (deltas.spawnersDestroyed) {
      for (var di2 = 0; di2 < deltas.spawnersDestroyed.length; di2++) {
        var si2 = deltas.spawnersDestroyed[di2];
        if (si2 < chunk.enemySpawners.length) chunk.enemySpawners[si2].active = false;
      }
    }
    if (deltas.explored) chunk.exploredCells = deltas.explored;
    if (deltas.shrineUsed && chunk.shrine) chunk.shrine.used = true;
  }

  chunks[key] = chunk;
  return chunk;
}

function chunkWallDecorationInWindow(d, chunkWindowX, chunkWindowY) {
  return {worldX: d.worldX - windowOriginX, worldY: d.worldY - windowOriginY,
    side: d.side, type: d.type,
    gridX: d.gridX + chunkWindowX * CHUNK_CELLS,
    gridY: d.gridY + chunkWindowY * CHUNK_CELLS};
}

function assembleWindow(centerCX, centerCY) {
  var half = Math.floor(WINDOW_CHUNKS / 2);
  var newWCX = centerCX - half;
  var newWCY = centerCY - half;

  // Compute position shift before changing window origin
  var shiftX = (newWCX - windowCX) * CHUNK_SIZE;
  var shiftY = (newWCY - windowCY) * CHUNK_SIZE;

  // Save entity deltas from old chunks before reassembly
  if (grid && ENDLESS_MODE) saveAllEntityDeltas();

  windowCX = newWCX;
  windowCY = newWCY;
  windowOriginX = windowCX * CHUNK_SIZE;
  windowOriginY = windowCY * CHUNK_SIZE;

  // Set world dimensions to window size
  var totalCells = WINDOW_CHUNKS * CHUNK_CELLS;
  gridW = totalCells;
  gridH = totalCells;
  worldW = WINDOW_CHUNKS * CHUNK_SIZE;
  worldH = WINDOW_CHUNKS * CHUNK_SIZE;
  viewDist = settings.viewDist;

  // Generate all chunks in window
  for (var dy = 0; dy < WINDOW_CHUNKS; dy++) {
    for (var dx = 0; dx < WINDOW_CHUNKS; dx++) {
      generateChunk(windowCX + dx, windowCY + dy);
    }
  }

  // ── Guarantee at least one market and one shrine in the spawn window ──
  // Only on initial load (no existing grid), pick a random eligible chunk
  if (!grid) {
    var _hasMarket = false, _hasShrine = false;
    var _eligible = []; // chunks that aren't origin
    for (var _gy = 0; _gy < WINDOW_CHUNKS; _gy++) {
      for (var _gx = 0; _gx < WINDOW_CHUNKS; _gx++) {
        var _gcx = windowCX + _gx, _gcy = windowCY + _gy;
        if (_gcx === 0 && _gcy === 0) continue;
        var _gk = _gcx + ',' + _gcy;
        var _gc = chunks[_gk];
        if (_gc.market) _hasMarket = true;
        if (_gc.shrine) _hasShrine = true;
        _eligible.push(_gk);
      }
    }
    // Use WORLD_SEED for deterministic but random-feeling selection
    var _pickRng = chunkRng(0, 0, 99);
    if (!_hasMarket && _eligible.length > 0) {
      // Pick random chunk, regenerate with forced market
      var _mIdx = Math.floor(_pickRng() * _eligible.length);
      var _mk = _eligible[_mIdx];
      var _mc = chunks[_mk];
      // Remove from cache and regenerate — but we can't easily force market in generateChunk.
      // Instead, directly inject market data into the chunk.
      if (!_mc.market) {
        var _cell = CHUNK_SIZE / CHUNK_CELLS; // same as cell in generateChunk
        var _mcx = _mc.cx * CHUNK_SIZE + Math.floor(CHUNK_CELLS / 2) * _cell + _cell / 2;
        var _mcy = _mc.cy * CHUNK_SIZE + Math.floor(CHUNK_CELLS / 2) * _cell + _cell / 2;
        // Clear center area in grid
        var _half = Math.floor(CHUNK_CELLS / 2);
        for (var _mdy = -5; _mdy <= 5; _mdy++) {
          for (var _mdx = -5; _mdx <= 5; _mdx++) {
            var _mgx2 = _half + _mdx, _mgy2 = _half + _mdy;
            if (_mgx2 >= 1 && _mgx2 < CHUNK_CELLS - 1 && _mgy2 >= 1 && _mgy2 < CHUNK_CELLS - 1) {
              _mc.grid[_mgy2 * CHUNK_CELLS + _mgx2] = 0;
            }
          }
        }
        // Create simple market with 4 stalls
        var _stallOffsets = [{dx:-2,dy:-2,facing:'se'},{dx:2,dy:-2,facing:'sw'},{dx:-2,dy:2,facing:'ne'},{dx:2,dy:2,facing:'nw'}];
        var _stallTypes = ['weapons','potions','armor','scrolls'];
        var _mStalls = [];
        for (var _msi = 0; _msi < 4; _msi++) {
          _mStalls.push({x: _mcx + _stallOffsets[_msi].dx * _cell, y: _mcy + _stallOffsets[_msi].dy * _cell,
            facing: _stallOffsets[_msi].facing, stallType: _stallTypes[_msi]});
        }
        _mc.market = { centerX: _mcx, centerY: _mcy, stalls: _mStalls };
        _rebuildChunkWalls(_mc);
        console.log('[SPAWN] Guaranteed market injected at chunk ' + _mk);
      }
    }
    if (!_hasShrine && _eligible.length > 0) {
      // Pick a different chunk from the market one
      var _sIdx = Math.floor(_pickRng() * _eligible.length);
      var _sk = _eligible[_sIdx];
      var _sc = chunks[_sk];
      if (!_sc.shrine && !_sc.market) {
        var _cell2 = CHUNK_SIZE / CHUNK_CELLS;
        var _shX = Math.floor(CHUNK_CELLS / 2), _shY = Math.floor(CHUNK_CELLS / 2);
        // Clear 5x5 area
        for (var _sdy2 = -2; _sdy2 <= 2; _sdy2++) {
          for (var _sdx2 = -2; _sdx2 <= 2; _sdx2++) {
            var _sgx2 = _shX + _sdx2, _sgy2 = _shY + _sdy2;
            if (_sgx2 >= 0 && _sgx2 < CHUNK_CELLS && _sgy2 >= 0 && _sgy2 < CHUNK_CELLS) {
              _sc.grid[_sgy2 * CHUNK_CELLS + _sgx2] = 0;
            }
          }
        }
        var _buffTypes = ['damage', 'speed', 'regen', 'armor'];
        var _buffType = _buffTypes[Math.floor(_pickRng() * _buffTypes.length)];
        _sc.shrine = {
          x: _sc.cx * CHUNK_SIZE + _shX * _cell2 + _cell2 / 2,
          y: _sc.cy * CHUNK_SIZE + _shY * _cell2 + _cell2 / 2,
          buffType: _buffType, used: false
        };
        _rebuildChunkWalls(_sc);
        console.log('[SPAWN] Guaranteed shrine (' + _buffType + ') injected at chunk ' + _sk);
      }
    }
  }

  // Rebuild wall rects from a chunk's grid (used after injecting market/shrine clearings)
  function _rebuildChunkWalls(ch) {
    var _cw = [];
    var _cell = CHUNK_SIZE / CHUNK_CELLS;
    for (var _wy = 0; _wy < CHUNK_CELLS; _wy++) {
      var _rs = -1;
      for (var _wx = 0; _wx <= CHUNK_CELLS; _wx++) {
        var _isW = (_wx < CHUNK_CELLS) && ch.grid[_wy * CHUNK_CELLS + _wx];
        if (_isW && _rs < 0) _rs = _wx;
        else if (!_isW && _rs >= 0) {
          _cw.push({x: ch.cx * CHUNK_SIZE + _rs * _cell, y: ch.cy * CHUNK_SIZE + _wy * _cell,
            w: (_wx - _rs) * _cell, h: _cell});
          _rs = -1;
        }
      }
    }
    ch.walls = _cw;
  }

  // Assemble grid + wallHeights
  grid = new Uint8Array(totalCells * totalCells);
  wallHeights = new Float32Array(totalCells * totalCells);
  wallColorR = new Uint8Array(totalCells * totalCells);
  wallColorG = new Uint8Array(totalCells * totalCells);
  wallColorB = new Uint8Array(totalCells * totalCells);
  for (var dy2 = 0; dy2 < WINDOW_CHUNKS; dy2++) {
    for (var dx2 = 0; dx2 < WINDOW_CHUNKS; dx2++) {
      var ch = chunks[(windowCX + dx2) + ',' + (windowCY + dy2)];
      var offX = dx2 * CHUNK_CELLS;
      var offY = dy2 * CHUNK_CELLS;
      for (var ly = 0; ly < CHUNK_CELLS; ly++) {
        for (var lx = 0; lx < CHUNK_CELLS; lx++) {
          var gi = (offY + ly) * totalCells + (offX + lx);
          var ci = ly * CHUNK_CELLS + lx;
          grid[gi] = ch.grid[ci];
          wallHeights[gi] = ch.wallHeights[ci];
          wallColorR[gi] = ch.wallColorR ? ch.wallColorR[ci] : 0;
          wallColorG[gi] = ch.wallColorG ? ch.wallColorG[ci] : 0;
          wallColorB[gi] = ch.wallColorB ? ch.wallColorB[ci] : 0;
        }
      }
    }
  }

  // Assemble walls array — offset to window-local coords
  walls = [];
  for (var dy3 = 0; dy3 < WINDOW_CHUNKS; dy3++) {
    for (var dx3 = 0; dx3 < WINDOW_CHUNKS; dx3++) {
      var ch3 = chunks[(windowCX + dx3) + ',' + (windowCY + dy3)];
      for (var wi = 0; wi < ch3.walls.length; wi++) {
        var w = ch3.walls[wi];
        walls.push({
          x: w.x - windowOriginX,
          y: w.y - windowOriginY,
          w: w.w, h: w.h
        });
      }
    }
  }

  // Cross-chunk wall-rect merge. Per-chunk gen already merges horizontal
  // runs within each chunk, but rects terminate at chunk boundaries — a
  // wall spanning chunks A and B becomes two rects. Merge aligned pairs
  // so downstream code (collision, rendering) sees unified rects.
  if (walls.length > 1) {
    // Index by (y, h) → list of {idx, x, x2}. Two rects are mergeable if
    // they share the same y and h AND their x-ranges are adjacent.
    var _mergeMap = {};
    for (var _wm = 0; _wm < walls.length; _wm++) {
      var _wmr = walls[_wm];
      var _wmk = _wmr.y + '|' + _wmr.h;
      if (!_mergeMap[_wmk]) _mergeMap[_wmk] = [];
      _mergeMap[_wmk].push(_wm);
    }
    var _wmKeep = new Uint8Array(walls.length);
    for (var _wmi = 0; _wmi < walls.length; _wmi++) _wmKeep[_wmi] = 1;
    for (var _wmk2 in _mergeMap) {
      var _wmGroup = _mergeMap[_wmk2];
      if (_wmGroup.length < 2) continue;
      // Sort by x so adjacents are consecutive
      _wmGroup.sort(function(a, b) { return walls[a].x - walls[b].x; });
      for (var _wmgi = 0; _wmgi < _wmGroup.length - 1; _wmgi++) {
        var _wmA = walls[_wmGroup[_wmgi]];
        if (!_wmKeep[_wmGroup[_wmgi]]) continue;
        var _wmB = walls[_wmGroup[_wmgi + 1]];
        // Adjacent if A's right edge meets B's left edge (within 0.5u)
        if (Math.abs((_wmA.x + _wmA.w) - _wmB.x) < 0.5) {
          _wmA.w += _wmB.w;
          _wmKeep[_wmGroup[_wmgi + 1]] = 0;
          // Extend current group head: next iteration keeps comparing A
          _wmGroup[_wmgi + 1] = _wmGroup[_wmgi];
        }
      }
    }
    var _wmMerged = [];
    for (var _wmf = 0; _wmf < walls.length; _wmf++) {
      if (_wmKeep[_wmf]) _wmMerged.push(walls[_wmf]);
    }
    walls = _wmMerged;
  }

  // Assemble floor mesh
  var chunkMeshW = Math.ceil(CHUNK_SIZE / 12);
  var chunkMeshH = Math.ceil(CHUNK_SIZE / 12);
  var totalMeshW = WINDOW_CHUNKS * chunkMeshW;
  var totalMeshH = WINDOW_CHUNKS * chunkMeshH;
  var totalMeshSize = totalMeshW * totalMeshH;
  floorMesh = {
    w: totalMeshW, h: totalMeshH, gridSize: 12,
    // colors: per-cell biome color (used by renderer).
    // water: per-cell water flag (0=none, 1=shallow, 2=deep).
    // colors[]: convenience "top walkable color" per cell for 2D/minimap,
    // floor-scatter queries, and other non-layered consumers. Authoritative
    // color lives in per-layer lXColor arrays. colors[] is a derived view —
    // updated whenever layer data changes.
    colors: new Array(totalMeshSize),
    surfaceBiome: new Array(totalMeshSize),
    surfaceH: new Float32Array(totalMeshSize),
    caveStone: new Uint32Array(totalMeshSize),
    water: new Uint8Array(totalMeshSize),
    waterDirX: new Float32Array(totalMeshSize),
    waterDirY: new Float32Array(totalMeshSize),
    // ── Layered height field (authoritative storage) ──
    // Up to LAYER_MAX layers per cell, bottom → top (index 0 is lowest).
    // topZ: height of the layer's top, in world height units (× 25 for camera Z).
    // type: 0=empty, 1=walkable (floor/cap/ledge), 2=ceiling, 4=surface cap.
    // color: packed 0xRRGGBB for that layer's top face.
    LAYER_MAX: 5,
    layerCount: new Uint8Array(totalMeshSize),
    l0TopZ: new Float32Array(totalMeshSize), l0Type: new Uint8Array(totalMeshSize), l0Color: new Array(totalMeshSize),
    l1TopZ: new Float32Array(totalMeshSize), l1Type: new Uint8Array(totalMeshSize), l1Color: new Array(totalMeshSize),
    l2TopZ: new Float32Array(totalMeshSize), l2Type: new Uint8Array(totalMeshSize), l2Color: new Array(totalMeshSize),
    l3TopZ: new Float32Array(totalMeshSize), l3Type: new Uint8Array(totalMeshSize), l3Color: new Array(totalMeshSize),
    l4TopZ: new Float32Array(totalMeshSize), l4Type: new Uint8Array(totalMeshSize), l4Color: new Array(totalMeshSize)
  };
  for (var dy4 = 0; dy4 < WINDOW_CHUNKS; dy4++) {
    for (var dx4 = 0; dx4 < WINDOW_CHUNKS; dx4++) {
      var ch4 = chunks[(windowCX + dx4) + ',' + (windowCY + dy4)];
      var cmesh = ch4.floorMesh;
      var mOffX = dx4 * chunkMeshW;
      var mOffY = dy4 * chunkMeshH;
      for (var my = 0; my < cmesh.h && (mOffY + my) < totalMeshH; my++) {
        for (var mx = 0; mx < cmesh.w && (mOffX + mx) < totalMeshW; mx++) {
          var ti = (mOffY + my) * totalMeshW + (mOffX + mx);
          var si = my * cmesh.w + mx;
          floorMesh.surfaceBiome[ti] = cmesh.surfaceBiome[si];
          floorMesh.surfaceH[ti] = cmesh.surfaceH ? cmesh.surfaceH[si] : cmesh.l0TopZ[si];
          floorMesh.caveStone[ti] = cmesh.caveStone[si];
          if (cmesh.water) floorMesh.water[ti] = cmesh.water[si];
          // Copy per-chunk layer data (heights, types, colors) into window mesh
          floorMesh.layerCount[ti] = cmesh.layerCount[si];
          floorMesh.l0TopZ[ti] = cmesh.l0TopZ[si]; floorMesh.l0Type[ti] = cmesh.l0Type[si]; floorMesh.l0Color[ti] = cmesh.l0Color[si];
          floorMesh.l1TopZ[ti] = cmesh.l1TopZ[si]; floorMesh.l1Type[ti] = cmesh.l1Type[si]; floorMesh.l1Color[ti] = cmesh.l1Color[si];
          floorMesh.l2TopZ[ti] = cmesh.l2TopZ[si]; floorMesh.l2Type[ti] = cmesh.l2Type[si]; floorMesh.l2Color[ti] = cmesh.l2Color[si];
          floorMesh.l3TopZ[ti] = cmesh.l3TopZ[si]; floorMesh.l3Type[ti] = cmesh.l3Type[si]; floorMesh.l3Color[ti] = cmesh.l3Color[si];
          floorMesh.l4TopZ[ti] = cmesh.l4TopZ[si]; floorMesh.l4Type[ti] = cmesh.l4Type[si]; floorMesh.l4Color[ti] = cmesh.l4Color[si];
        }
      }
    }
  }

  // ═══════════════════════════════════════════════════════════════════
  //  UNIFIED MESH POST-ASSEMBLY PASS (layer-native)
  //  Input: window mesh with per-chunk layers already copied in (floor + ceiling).
  //  Output: layers augmented with hill caps over deep cave cells, plus the
  //  meshCave boolean grid. Everything operates on layer data directly —
  //  no intermediate heights/ceilH arrays on floorMesh.
  // ═══════════════════════════════════════════════════════════════════
  var N = totalMeshW * totalMeshH;

  // meshCave: 1 if the cell has any ceiling-type layer. Layers are sorted
  // ascending so a ceiling at the end of the stack is where it'll be.
  meshCave = new Uint8Array(N);
  for (var iMC = 0; iMC < N; iMC++) {
    var lcMC = floorMesh.layerCount[iMC];
    // Inline check across l0..l4 Type for type=2
    if (lcMC >= 1 && floorMesh.l0Type[iMC] === 2) meshCave[iMC] = 1;
    else if (lcMC >= 2 && floorMesh.l1Type[iMC] === 2) meshCave[iMC] = 1;
    else if (lcMC >= 3 && floorMesh.l2Type[iMC] === 2) meshCave[iMC] = 1;
  }

  // Helper: read floor Z (l0) and ceiling Z (first type=2 layer, or 0).
  // Per-chunk gen guarantees l0 = floor (type=1). Ceiling follows if present.
  function _getFloorZ(i) { return floorMesh.l0TopZ[i]; }
  function _getCeilZ(i) {
    if (meshCave[i] !== 1) return 0;
    var lc = floorMesh.layerCount[i];
    if (lc >= 2 && floorMesh.l1Type[i] === 2) return floorMesh.l1TopZ[i];
    if (lc >= 3 && floorMesh.l2Type[i] === 2) return floorMesh.l2TopZ[i];
    return 0;
  }

  // A ceiling is the authoritative roof footprint, including its mouth edge.
  // The approach has no ceiling and therefore no cap. No second circular
  // entrance exclusion can disagree with the portal plane.
  var needsCap = new Uint8Array(N);
  var capH = new Float32Array(N);
  // Flat-surface cap model: cap sits at the NATURAL surface height for
  // that cell (snapshotted before cave stamping). No diffusion, no depth
  // bump, no hill bulge — the ground over a cave reads as the terrain
  // that would have been there without the cave. Z always ends up above
  // the ceiling because strategy-1 depth-gating guarantees ceiling <
  // surface - 0.5.
  for (var iN = 0; iN < N; iN++) {
    var fhN = _getFloorZ(iN);
    if (meshCave[iN]) {
      // Cap cells with a ceiling overhead (actual cave interior). Skip cells
      // without a ceiling — the approach ramp / open mouth should read as a
      // hole in the ground, not a capped tunnel.
      var _lc = floorMesh.layerCount[iN];
      var _hasCeilAbove = (_lc >= 2 && floorMesh.l1Type[iN] === 2) ||
                          (_lc >= 3 && floorMesh.l2Type[iN] === 2) ||
                          (_lc >= 4 && floorMesh.l3Type[iN] === 2) ||
                          (_lc >= 5 && floorMesh.l4Type[iN] === 2);
      if (_hasCeilAbove) needsCap[iN] = 1;
    }
    capH[iN] = needsCap[iN] ? floorMesh.surfaceH[iN] : fhN;
  }

  // Final clamp: ensure each cap ends up strictly above its ceiling. Diffusion
  // and the bulge pass pull cap Z toward neighboring surface, which can push
  // it below the ceiling. If that happens, the sort-by-Z insertion puts the
  // cap BELOW the ceiling layer, and _mesh_cellCapZ (which needs walkable-
  // after-ceiling in Z-sorted order) fails to recognize it as a cap.
  for (var iC = 0; iC < N; iC++) {
    if (!needsCap[iC]) continue;
    var cz = _getCeilZ(iC);
    if (capH[iC] < cz + 0.25) capH[iC] = cz + 0.25;
  }

  // Insert cap layer (type=4) into the existing layer stack for each cap
  // cell, along with its color. Cap color varies per-cell around the local
  // biome color so stacked terrain reads as distinct strata, not a flat
  // surface. Colors ride the insertion sort alongside Z and type.
  var layerTmpZ = new Float32Array(5), layerTmpT = new Uint8Array(5), layerTmpC = new Array(5);
  var _capRng = (function(){ var s = 1664525; return function(){ s = (s * 1103515245 + 12345) | 0; return ((s >>> 0) % 10000) / 10000; }; })();
  for (var iL = 0; iL < N; iL++) {
    if (!needsCap[iL]) continue;
    var lc = floorMesh.layerCount[iL];
    var n = 0;
    if (lc >= 1) { layerTmpZ[n] = floorMesh.l0TopZ[iL]; layerTmpT[n] = floorMesh.l0Type[iL]; layerTmpC[n] = floorMesh.l0Color[iL]; n++; }
    if (lc >= 2) { layerTmpZ[n] = floorMesh.l1TopZ[iL]; layerTmpT[n] = floorMesh.l1Type[iL]; layerTmpC[n] = floorMesh.l1Color[iL]; n++; }
    if (lc >= 3) { layerTmpZ[n] = floorMesh.l2TopZ[iL]; layerTmpT[n] = floorMesh.l2Type[iL]; layerTmpC[n] = floorMesh.l2Color[iL]; n++; }
    if (lc >= 4) { layerTmpZ[n] = floorMesh.l3TopZ[iL]; layerTmpT[n] = floorMesh.l3Type[iL]; layerTmpC[n] = floorMesh.l3Color[iL]; n++; }
    // Cap color: biome base + mild per-cell variation (±6%) so the surface
    // over a cave reads as natural patches, not a uniform tint.
    var _biomeStr = floorMesh.surfaceBiome[iL] || '#9bb06d';
    var _bp = parseInt(_biomeStr.slice(1), 16);
    var _br = (_bp >> 16) & 0xff, _bg = (_bp >> 8) & 0xff, _bb = _bp & 0xff;
    var _jr = 0.94 + _capRng() * 0.12;
    var _jg = 0.94 + _capRng() * 0.12;
    var _jb = 0.94 + _capRng() * 0.12;
    var _cr = Math.min(255, Math.max(0, (_br * _jr) | 0));
    var _cg = Math.min(255, Math.max(0, (_bg * _jg) | 0));
    var _cb = Math.min(255, Math.max(0, (_bb * _jb) | 0));
    var capCol = '#' + ('000000' + (((_cr << 16) | (_cg << 8) | _cb) >>> 0).toString(16)).slice(-6);
    layerTmpZ[n] = capH[iL]; layerTmpT[n] = 4; layerTmpC[n] = capCol; n++;
    // Insertion sort by topZ — colors swap alongside Z/type
    for (var si = 1; si < n; si++) {
      var vz = layerTmpZ[si], vt = layerTmpT[si], vc = layerTmpC[si], sj = si - 1;
      while (sj >= 0 && layerTmpZ[sj] > vz) {
        layerTmpZ[sj + 1] = layerTmpZ[sj]; layerTmpT[sj + 1] = layerTmpT[sj]; layerTmpC[sj + 1] = layerTmpC[sj]; sj--;
      }
      layerTmpZ[sj + 1] = vz; layerTmpT[sj + 1] = vt; layerTmpC[sj + 1] = vc;
    }
    floorMesh.layerCount[iL] = n;
    if (n > 0) { floorMesh.l0TopZ[iL] = layerTmpZ[0]; floorMesh.l0Type[iL] = layerTmpT[0]; floorMesh.l0Color[iL] = layerTmpC[0]; }
    if (n > 1) { floorMesh.l1TopZ[iL] = layerTmpZ[1]; floorMesh.l1Type[iL] = layerTmpT[1]; floorMesh.l1Color[iL] = layerTmpC[1]; }
    if (n > 2) { floorMesh.l2TopZ[iL] = layerTmpZ[2]; floorMesh.l2Type[iL] = layerTmpT[2]; floorMesh.l2Color[iL] = layerTmpC[2]; }
    if (n > 3) { floorMesh.l3TopZ[iL] = layerTmpZ[3]; floorMesh.l3Type[iL] = layerTmpT[3]; floorMesh.l3Color[iL] = layerTmpC[3]; }
    if (n > 4) { floorMesh.l4TopZ[iL] = layerTmpZ[4]; floorMesh.l4Type[iL] = layerTmpT[4]; floorMesh.l4Color[iL] = layerTmpC[4]; }
  }

  // Derive floorMesh.colors[] — the "top walkable color" per cell. Consumers
  // that aren't layer-aware (2D map, floor-scatter queries, minimap) use
  // this. Computed from per-layer lXColor after all layer mutations settle.
  for (var _fc = 0; _fc < N; _fc++) {
    var _fcLc = floorMesh.layerCount[_fc];
    var _fcCol = floorMesh.l0Color[_fc];
    for (var _fcLi = _fcLc - 1; _fcLi >= 0; _fcLi--) {
      var _fcT, _fcC;
      if (_fcLi === 0) { _fcT = floorMesh.l0Type[_fc]; _fcC = floorMesh.l0Color[_fc]; }
      else if (_fcLi === 1) { _fcT = floorMesh.l1Type[_fc]; _fcC = floorMesh.l1Color[_fc]; }
      else if (_fcLi === 2) { _fcT = floorMesh.l2Type[_fc]; _fcC = floorMesh.l2Color[_fc]; }
      else if (_fcLi === 3) { _fcT = floorMesh.l3Type[_fc]; _fcC = floorMesh.l3Color[_fc]; }
      else { _fcT = floorMesh.l4Type[_fc]; _fcC = floorMesh.l4Color[_fc]; }
      if (_fcT === 1 || _fcT === 3 || _fcT === 4) { _fcCol = _fcC; break; }
    }
    floorMesh.colors[_fc] = _fcCol;
  }

  // gridCave: 1 if wall cell is in a cave region (checks meshCave with ±2 margin)
  gridCave = new Uint8Array(totalCells * totalCells);
  var _mcMeshGS = 12; // floorMesh.gridSize
  for (var _gcy = 0; _gcy < totalCells; _gcy++) {
    for (var _gcx = 0; _gcx < totalCells; _gcx++) {
      if (!grid[_gcy * totalCells + _gcx]) continue;
      var _wcx2 = (_gcx + 0.5) * cell, _wcy2 = (_gcy + 0.5) * cell;
      var _fxC2 = Math.floor(_wcx2 / _mcMeshGS), _fyC2 = Math.floor(_wcy2 / _mcMeshGS);
      // Flag as cave wall ONLY if the wall's own mesh cell or a direct (1-
      // cell) neighbor has cave ceiling. Wider margins mis-tagged tower/
      // structure walls that happened to stand near a cave as "cave walls".
      var _found2 = false;
      for (var _fy2 = _fyC2 - 1; _fy2 <= _fyC2 + 1 && !_found2; _fy2++) {
        for (var _fx2 = _fxC2 - 1; _fx2 <= _fxC2 + 1; _fx2++) {
          if (_fx2 >= 0 && _fx2 < totalMeshW && _fy2 >= 0 && _fy2 < totalMeshH) {
            if (meshCave[_fy2 * totalMeshW + _fx2]) { _found2 = true; break; }
          }
        }
      }
      if (_found2) gridCave[_gcy * totalCells + _gcx] = 1;
    }
  }
  // Tag walls[] rects with .cave flag
  for (var _cwi = 0; _cwi < walls.length; _cwi++) {
    var _cwr = walls[_cwi];
    var _cwgx0 = Math.floor(_cwr.x / cell), _cwgy0 = Math.floor(_cwr.y / cell);
    var _cwgx1 = Math.floor((_cwr.x + _cwr.w - 1) / cell);
    _cwr.cave = false;
    for (var _cwgx = _cwgx0; _cwgx <= _cwgx1; _cwgx++) {
      if (_cwgx >= 0 && _cwgx < gridW && _cwgy0 >= 0 && _cwgy0 < gridH) {
        if (gridCave[_cwgy0 * gridW + _cwgx]) { _cwr.cave = true; break; }
      }
    }
  }
  var _caveCellCount = 0;
  for (var _cci = 0; _cci < gridCave.length; _cci++) _caveCellCount += gridCave[_cci];
  console.log('[CAVE-META] gridCave: ' + _caveCellCount + '/' + gridCave.length + ' cells, meshCave: ' + meshCave.reduce(function(a,b){return a+b;},0) + '/' + meshCave.length + ' cells, walls[].cave: ' + walls.filter(function(w){return w.cave;}).length + '/' + walls.length);

  // ── Per-layer semantic metadata (once per assembly, zero per-frame cost) ──
  // Encodes {hasCeilAbove, role} as a packed byte per layer. Queried by the
  // physics picker and walkable-layer resolver so the engine can reason
  // about stratum identity instead of just Z proximity.
  //
  // role values: 0=none, 1=surface (type 1, open sky), 2=caveFloor (type 1
  // with ceiling above), 3=cap (type 4), 4=ledge (type 3).
  // bit 7 = hasCeilAbove (reserved for type-1 layers; other roles encode it
  // implicitly but the bit is still valid).
  floorMesh.l0Meta = new Uint8Array(totalMeshSize);
  floorMesh.l1Meta = new Uint8Array(totalMeshSize);
  floorMesh.l2Meta = new Uint8Array(totalMeshSize);
  floorMesh.l3Meta = new Uint8Array(totalMeshSize);
  floorMesh.l4Meta = new Uint8Array(totalMeshSize);
  var _lmMetas = [floorMesh.l0Meta, floorMesh.l1Meta, floorMesh.l2Meta, floorMesh.l3Meta, floorMesh.l4Meta];
  var _lmTypes = [floorMesh.l0Type, floorMesh.l1Type, floorMesh.l2Type, floorMesh.l3Type, floorMesh.l4Type];
  for (var _lmi = 0; _lmi < totalMeshSize; _lmi++) {
    var _lmLc = floorMesh.layerCount[_lmi];
    for (var _lmLi = 0; _lmLi < _lmLc; _lmLi++) {
      var _lmT = _lmTypes[_lmLi][_lmi];
      var _lmHasCeil = 0;
      for (var _lmAbove = _lmLi + 1; _lmAbove < _lmLc; _lmAbove++) {
        if (_lmTypes[_lmAbove][_lmi] === 2) { _lmHasCeil = 1; break; }
      }
      var _lmRole = 0;
      if (_lmT === 1) _lmRole = _lmHasCeil ? 2 : 1; // caveFloor vs surface
      else if (_lmT === 4) _lmRole = 3;              // cap
      else if (_lmT === 3) _lmRole = 4;              // ledge
      // type 2 (ceiling) stays role=0
      _lmMetas[_lmLi][_lmi] = _lmRole | (_lmHasCeil << 7);
    }
  }

  // Precompute per-wall-face base Z. Fixes visible gaps between wall bottoms
  // and adjacent floor quads caused by wall cell size (15u) being misaligned
  // with floor mesh grid (12u). Center-of-neighbor sampling misses corner
  // variations up to 3u; sampling along the full face edge catches them.
  // Runs once per chunk shift — zero per-frame cost.
  var _wfbT0 = Date.now();
  wallFaceBase = new Float32Array(gridW * gridH * 4);
  var _wfbSamples = 0, _wfbCells = 0;
  // Face direction table: 0=W, 1=E, 2=N, 3=S
  //   [dx, dy, e1x, e1y, e2x, e2y]  — e*x/y are 0 or 1 for cell corners (x1/x2, y1/y2)
  var _wfbFaces = [[-1,0, 0,1, 0,0], [1,0, 1,0, 1,1], [0,-1, 0,0, 1,0], [0,1, 1,1, 0,1]];
  for (var _wfbGy = 0; _wfbGy < gridH; _wfbGy++) {
    for (var _wfbGx = 0; _wfbGx < gridW; _wfbGx++) {
      var _wfbIdx = _wfbGy * gridW + _wfbGx;
      if (!grid[_wfbIdx]) continue;
      _wfbCells++;
      var _wfbFh = floorMesh ? getFloorHeightAt((_wfbGx + 0.5) * cell, (_wfbGy + 0.5) * cell) : 0;
      var _wfbX1 = _wfbGx * cell, _wfbY1 = _wfbGy * cell;
      var _wfbX2 = _wfbX1 + cell, _wfbY2 = _wfbY1 + cell;
      for (var _wfbFi = 0; _wfbFi < 4; _wfbFi++) {
        var _wfbF = _wfbFaces[_wfbFi];
        var _wfbNgx = _wfbGx + _wfbF[0], _wfbNgy = _wfbGy + _wfbF[1];
        var _wfbBase = _wfbFh;
        if (floorMesh && _wfbNgx >= 0 && _wfbNgx < gridW && _wfbNgy >= 0 && _wfbNgy < gridH) {
          var _wfbEx1 = _wfbF[2] ? _wfbX2 : _wfbX1;
          var _wfbEy1 = _wfbF[3] ? _wfbY2 : _wfbY1;
          var _wfbEx2 = _wfbF[4] ? _wfbX2 : _wfbX1;
          var _wfbEy2 = _wfbF[5] ? _wfbY2 : _wfbY1;
          // Offset 1u into the neighbor cell along the face normal
          var _wfbOx = _wfbF[0] * 1.0, _wfbOy = _wfbF[1] * 1.0;
          var _wfbMx = (_wfbEx1 + _wfbEx2) * 0.5, _wfbMy = (_wfbEy1 + _wfbEy2) * 0.5;
          var _wfbS1 = getFloorHeightAt(_wfbEx1 + _wfbOx, _wfbEy1 + _wfbOy);
          var _wfbS2 = getFloorHeightAt(_wfbEx2 + _wfbOx, _wfbEy2 + _wfbOy);
          var _wfbSm = getFloorHeightAt(_wfbMx + _wfbOx, _wfbMy + _wfbOy);
          _wfbSamples += 3;
          if (_wfbS1 < _wfbBase) _wfbBase = _wfbS1;
          if (_wfbS2 < _wfbBase) _wfbBase = _wfbS2;
          if (_wfbSm < _wfbBase) _wfbBase = _wfbSm;
        }
        wallFaceBase[_wfbIdx * 4 + _wfbFi] = _wfbBase;
      }
    }
  }
  // Cave-wall deep-extend: cave floor swings several units across short spans,
  // so even perfect edge sampling can't fully close the seam. Extend cave wall
  // bases down to the minimum cave floor Z in this window so the wall quad
  // always overlaps whatever floor is adjacent. Safe margin of 0.5 below min.
  var _cwDeepMin = 0;
  if (floorMesh && floorMesh.l0TopZ) {
    for (var _cwi = 0; _cwi < floorMesh.l0TopZ.length; _cwi++) {
      var _cwz = floorMesh.l0TopZ[_cwi];
      if (_cwz < _cwDeepMin) _cwDeepMin = _cwz;
    }
  }
  var _cwDeepZ = _cwDeepMin - 0.5;
  var _cwExtended = 0;
  if (gridCave) {
    for (var _cwGi = 0; _cwGi < gridCave.length; _cwGi++) {
      if (!gridCave[_cwGi] || !grid[_cwGi]) continue;
      var _cwBi = _cwGi * 4;
      if (wallFaceBase[_cwBi]     > _cwDeepZ) wallFaceBase[_cwBi]     = _cwDeepZ;
      if (wallFaceBase[_cwBi + 1] > _cwDeepZ) wallFaceBase[_cwBi + 1] = _cwDeepZ;
      if (wallFaceBase[_cwBi + 2] > _cwDeepZ) wallFaceBase[_cwBi + 2] = _cwDeepZ;
      if (wallFaceBase[_cwBi + 3] > _cwDeepZ) wallFaceBase[_cwBi + 3] = _cwDeepZ;
      _cwExtended++;
    }
  }
  console.log('[WALL-BASE] precomputed ' + _wfbCells + ' walls × 4 faces (' + _wfbSamples + ' samples) in ' + (Date.now() - _wfbT0) + 'ms  caveExtend=' + _cwExtended + ' @ Z=' + _cwDeepZ.toFixed(2));

  // ── Per-wall layer tag: wallCapZ ──
  // For each wall cell, find the lowest walkable-layer Z that sits ABOVE the
  // wall's top. That's the "ceiling of ground" directly over this wall.
  // A cave wall (under dirt) has a finite wallCapZ and should be hidden from
  // a camera above the cap. A surface wall or an entrance-mouth wall has no
  // layer above it — stored as -Infinity — and renders in all cases.
  // Compared to cam.z at draw time (both in world Z units = meshZ * 25).
  // Per-cell cap lookup: cap Z if the mesh cell has a walkable layer above
  // any ceiling layer, else -Infinity. Used both for wallCapZ (skip test)
  // and to compute topZ clamps from neighbors.
  function _mesh_cellCapZ(mi) {
    if (mi < 0 || mi >= floorMesh.layerCount.length) return -Infinity;
    var lc = floorMesh.layerCount[mi];
    var sawCeil = false;
    for (var li = 0; li < lc; li++) {
      var t, z;
      if (li === 0) { t = floorMesh.l0Type[mi]; z = floorMesh.l0TopZ[mi]; }
      else if (li === 1) { t = floorMesh.l1Type[mi]; z = floorMesh.l1TopZ[mi]; }
      else if (li === 2) { t = floorMesh.l2Type[mi]; z = floorMesh.l2TopZ[mi]; }
      else if (li === 3) { t = floorMesh.l3Type[mi]; z = floorMesh.l3TopZ[mi]; }
      else { t = floorMesh.l4Type[mi]; z = floorMesh.l4TopZ[mi]; }
      if (t === 2) sawCeil = true;
      else if ((t === 1 || t === 3 || t === 4) && sawCeil) return z;
    }
    return -Infinity;
  }

  wallCapZ = new Float32Array(gridW * gridH);
  // wallMaxTopZ: highest allowed wall top in world-mesh Z. Starts at the
  // cell's own cap (if capped) or ceiling (if uncapped-with-ceiling), then
  // relaxed to the lowest neighbor-cap Z among 3x3 neighbors so entrance-
  // mouth walls are clamped to surrounding ground level instead of floating
  // up to wherever the cave ceiling wanders.
  wallMaxTopZ = new Float32Array(gridW * gridH);
  var _wclCapped = 0;
  for (var _wclGy = 0; _wclGy < gridH; _wclGy++) {
    for (var _wclGx = 0; _wclGx < gridW; _wclGx++) {
      var _wclIdx = _wclGy * gridW + _wclGx;
      wallCapZ[_wclIdx] = -Infinity;
      wallMaxTopZ[_wclIdx] = Infinity;
      if (!grid[_wclIdx]) continue;
      if (!floorMesh || !floorMesh.layerCount) continue;
      var _wclMx = Math.floor((_wclGx + 0.5) * cell / floorMesh.gridSize);
      var _wclMy = Math.floor((_wclGy + 0.5) * cell / floorMesh.gridSize);
      if (_wclMx < 0 || _wclMx >= floorMesh.w || _wclMy < 0 || _wclMy >= floorMesh.h) continue;
      var _wclMi = _wclMy * floorMesh.w + _wclMx;

      // Cap Z from own cell OR any of the 3×3 neighboring mesh cells. A
      // cave-boundary wall sits in a mesh cell with no ceiling (so no own
      // cap) — but if a neighbor mesh cell has dirt above it, this wall
      // is still visually covered from the surface and must be culled.
      var _wclOwnCap = -Infinity;
      for (var _wclDy = -1; _wclDy <= 1; _wclDy++) {
        for (var _wclDx = -1; _wclDx <= 1; _wclDx++) {
          var _wclNx = _wclMx + _wclDx, _wclNy = _wclMy + _wclDy;
          if (_wclNx < 0 || _wclNx >= floorMesh.w || _wclNy < 0 || _wclNy >= floorMesh.h) continue;
          var _wclNC = _mesh_cellCapZ(_wclNy * floorMesh.w + _wclNx);
          if (_wclNC > _wclOwnCap) _wclOwnCap = _wclNC;
        }
      }
      if (_wclOwnCap > -Infinity) { wallCapZ[_wclIdx] = _wclOwnCap; _wclCapped++; }

      // Compute top-Z clamp. For uncapped-with-ceiling cells (mouth walls),
      // clamp to the min of nearby caps — "the wall can't rise higher than
      // the surrounding dirt." Fall back to own ceiling if no neighbor has
      // a cap.
      var _wclCeilZ = -Infinity;
      var _wclLc2 = floorMesh.layerCount[_wclMi];
      for (var _wclLi2 = 0; _wclLi2 < _wclLc2; _wclLi2++) {
        var _wclT2, _wclZ2;
        if (_wclLi2 === 0) { _wclT2 = floorMesh.l0Type[_wclMi]; _wclZ2 = floorMesh.l0TopZ[_wclMi]; }
        else if (_wclLi2 === 1) { _wclT2 = floorMesh.l1Type[_wclMi]; _wclZ2 = floorMesh.l1TopZ[_wclMi]; }
        else if (_wclLi2 === 2) { _wclT2 = floorMesh.l2Type[_wclMi]; _wclZ2 = floorMesh.l2TopZ[_wclMi]; }
        else if (_wclLi2 === 3) { _wclT2 = floorMesh.l3Type[_wclMi]; _wclZ2 = floorMesh.l3TopZ[_wclMi]; }
        else { _wclT2 = floorMesh.l4Type[_wclMi]; _wclZ2 = floorMesh.l4TopZ[_wclMi]; }
        if (_wclT2 === 2 && _wclZ2 > _wclCeilZ) _wclCeilZ = _wclZ2;
      }
      var _wclTopLimit = Infinity;
      if (_wclOwnCap > -Infinity) {
        // Capped cell: wall top at cap (wall will be skipped from above
        // anyway; this bound is for the descent transition).
        _wclTopLimit = _wclOwnCap;
      } else if (_wclCeilZ > -Infinity) {
        // Uncapped cell with a ceiling — a "mouth" cell in a region where
        // everything is cave interior. Clamp to min(ceiling, SEA_LEVEL_Z).
        // SEA_LEVEL_Z is the world reference surface (mesh-Z ~ 0 → world 0);
        // walls can't rise above it by construction of this terrain.
        var _seaZ = 0.0;
        _wclTopLimit = Math.min(_wclCeilZ, _seaZ);
      }
      wallMaxTopZ[_wclIdx] = _wclTopLimit;
    }
  }
  console.log('[WALL-CAP] tagged ' + _wclCapped + ' capped walls; wallMaxTopZ computed for all');
  buildWalkCandZ(floorMesh);

  // Assemble entities — convert world coords to window-local
  floorScatter = [];
  wallDecorations = [];
  treasureChests = [];
  enemySpawners = [];
  enemies = [];
  oreVeins = [];
  shopMarker = null;
  marketStalls = [];
  shrines = [];
  ruins = [];
  statPickups = [];
  largeStructures = [];
  deepCaveEntrances = [];
  guardTowerLastFire = 0;
  var _assembleNow = Date.now();
  // Build next window key set; stamp chunks that are new to this window
  var _nextWindowChunks = {};
  for (var _pdy = 0; _pdy < WINDOW_CHUNKS; _pdy++) {
    for (var _pdx = 0; _pdx < WINDOW_CHUNKS; _pdx++) {
      var _pk = (windowCX + _pdx) + ',' + (windowCY + _pdy);
      _nextWindowChunks[_pk] = true;
      var _pc = chunks[_pk];
      if (_pc && !_prevWindowChunks[_pk]) _pc.windowAddedMs = _assembleNow;
    }
  }
  _prevWindowChunks = _nextWindowChunks;
  for (var dy5 = 0; dy5 < WINDOW_CHUNKS; dy5++) {
    for (var dx5 = 0; dx5 < WINDOW_CHUNKS; dx5++) {
      var ch5 = chunks[(windowCX + dx5) + ',' + (windowCY + dy5)];
      ch5.lastAccess = _assembleNow;
      for (var si2 = 0; si2 < ch5.floorScatter.length; si2++) {
        var s = ch5.floorScatter[si2];
        floorScatter.push({x: s.x - windowOriginX, y: s.y - windowOriginY,
          type: s.type, variant: s.variant, seed: s.seed,
          spawnMs: ch5.windowAddedMs || 0});
      }
      for (var di3 = 0; di3 < ch5.wallDecorations.length; di3++) {
        var d = ch5.wallDecorations[di3];
        wallDecorations.push(chunkWallDecorationInWindow(d, dx5, dy5));
      }
      for (var ti2 = 0; ti2 < ch5.treasureChests.length; ti2++) {
        var tc = ch5.treasureChests[ti2];
        treasureChests.push({x: tc.x - windowOriginX, y: tc.y - windowOriginY,
          caveSpawnId: tc.caveSpawnId || null,
          gold: tc.gold, collected: tc.collected, opened: tc.opened, lidAngle: tc.lidAngle,
          facing: tc.facing, equipId: tc.equipId, seed: tc.seed,
          tier: tc.tier || 'common', quality: tc.quality || 1.0, relicId: tc.relicId || null,
          _chunkKey: (windowCX + dx5) + ',' + (windowCY + dy5), _chunkIdx: ti2});
      }
      for (var sp2 = 0; sp2 < ch5.enemySpawners.length; sp2++) {
        var sp = ch5.enemySpawners[sp2];
        enemySpawners.push({x: sp.x - windowOriginX, y: sp.y - windowOriginY,
          hp: sp.hp, maxHp: sp.maxHp, cooldownMs: sp.cooldownMs, lastSpawn: sp.lastSpawn,
          spawnCount: sp.spawnCount, maxSpawns: sp.maxSpawns, active: sp.active, type: sp.type,
          _chunkKey: (windowCX + dx5) + ',' + (windowCY + dy5), _chunkIdx: sp2});
      }
      for (var ei2 = 0; ei2 < ch5.enemies.length; ei2++) {
        var en = ch5.enemies[ei2];
        enemies.push({x: en.x - windowOriginX, y: en.y - windowOriginY, z: en.z,
          caveSpawnId: en.caveSpawnId || null,
          enemyType: en.enemyType, health: en.health, maxHealth: en.maxHealth, speed: en.speed,
          chaseRange: en.chaseRange, lastUpdate: en.lastUpdate,
          damageFlash: en.damageFlash, damageFlashColor: en.damageFlashColor,
          slowUntil: en.slowUntil, burnUntil: en.burnUntil, burnDmgLast: en.burnDmgLast,
          iceHits: en.iceHits, fireHits: en.fireHits, lightningHits: en.lightningHits,
          vx: en.vx, vy: en.vy, aggroAt: en.aggroAt,
          attackState: en.attackState, attackStateUntil: en.attackStateUntil,
          patrolWaypoints: en.patrolWaypoints.map(function(wp) {
            return {x: wp.x - windowOriginX, y: wp.y - windowOriginY};
          }),
          patrolIdx: en.patrolIdx, facing: en.facing || 0, underground: en.underground || false});
      }
      if (ch5.market && !shopMarker) {
        shopMarker = {x: ch5.market.centerX - windowOriginX, y: ch5.market.centerY - windowOriginY};
        // Track for persistent minimap display
        var _alreadyDiscovered = false;
        for (var _dm = 0; _dm < discoveredMarkets.length; _dm++) {
          if (Math.abs(discoveredMarkets[_dm].wx - ch5.market.centerX) < 10 && Math.abs(discoveredMarkets[_dm].wy - ch5.market.centerY) < 10) { _alreadyDiscovered = true; break; }
        }
        if (!_alreadyDiscovered) discoveredMarkets.push({wx: ch5.market.centerX, wy: ch5.market.centerY});
        for (var ms = 0; ms < ch5.market.stalls.length; ms++) {
          var mst = ch5.market.stalls[ms];
          marketStalls.push({
            x: mst.x - windowOriginX,
            y: mst.y - windowOriginY,
            facing: mst.facing,
            stallType: mst.stallType
          });
        }
      }
      if (ch5.shrine) {
        var _sh = ch5.shrine;
        var _shKey = (windowCX + dx5) + ',' + (windowCY + dy5);
        shrines.push({
          x: _sh.x - windowOriginX,
          y: _sh.y - windowOriginY,
          buffType: _sh.buffType,
          used: _sh.used,
          chunkKey: _shKey
        });
        // Track for persistent minimap
        var _shDiscovered = false;
        for (var _dsi = 0; _dsi < discoveredShrines.length; _dsi++) {
          if (Math.abs(discoveredShrines[_dsi].wx - _sh.x) < 10 && Math.abs(discoveredShrines[_dsi].wy - _sh.y) < 10) { _shDiscovered = true; break; }
        }
        if (!_shDiscovered) discoveredShrines.push({wx: _sh.x, wy: _sh.y, buffType: _sh.buffType});
      }
      if (ch5.ruin) {
        var _ru = ch5.ruin;
        ruins.push({
          x: _ru.x - windowOriginX,
          y: _ru.y - windowOriginY,
          ruinType: _ru.ruinType, facing: _ru.facing,
          chunkKey: (windowCX + dx5) + ',' + (windowCY + dy5)
        });
        var _ruDiscovered = false;
        for (var _dri = 0; _dri < discoveredRuins.length; _dri++) {
          if (Math.abs(discoveredRuins[_dri].wx - _ru.x) < 10 && Math.abs(discoveredRuins[_dri].wy - _ru.y) < 10) { _ruDiscovered = true; break; }
        }
        if (!_ruDiscovered) discoveredRuins.push({wx: _ru.x, wy: _ru.y, ruinType: _ru.ruinType});
        // Stat pickup inside ruin
        if (_ru.statPickup) {
          var _spKey = Math.floor(_ru.statPickup.wx) + ',' + Math.floor(_ru.statPickup.wy);
          if (!collectedStatPickups[_spKey]) {
            statPickups.push({
              x: _ru.statPickup.wx - windowOriginX,
              y: _ru.statPickup.wy - windowOriginY,
              type: _ru.statPickup.type,
              wx: _ru.statPickup.wx, wy: _ru.statPickup.wy,
              bob: (_ru.statPickup.wx * 7 + _ru.statPickup.wy * 13) % (Math.PI * 2)
            });
          }
        }
      }
      if (ch5.structure) {
        var _st = ch5.structure;
        // Deduplicate — same structure can be referenced by multiple chunks
        var _stDup = false;
        for (var _dsti = 0; _dsti < largeStructures.length; _dsti++) {
          if (largeStructures[_dsti].regionX === _st.regionX && largeStructures[_dsti].regionY === _st.regionY) { _stDup = true; break; }
        }
        if (!_stDup) {
          largeStructures.push({
            x: _st.centerWX - windowOriginX,
            y: _st.centerWY - windowOriginY,
            type: _st.type,
            centerWX: _st.centerWX, centerWY: _st.centerWY,
            regionX: _st.regionX, regionY: _st.regionY,
            scale: _st.scale || 1.0
          });
        }
      }
      // Assemble cave entrances from chunks into deepCaveEntrances for beacon rendering.
      // Dedupe: the same entrance is registered by multiple neighboring chunks so
      // their clearance/approach passes can touch adjacent cells. Without this,
      // one entrance ends up in deepCaveEntrances 4+ times.
      if (ch5.caveEntrances && ch5.caveEntrances.length) {
        for (var _cei = 0; _cei < ch5.caveEntrances.length; _cei++) {
          var _ce = ch5.caveEntrances[_cei];
          var _ceDup = false;
          for (var _dei = 0; _dei < deepCaveEntrances.length; _dei++) {
            var _ee = deepCaveEntrances[_dei];
            if (Math.abs(_ee.x - (_ce.x - windowOriginX)) < 1 &&
                Math.abs(_ee.y - (_ce.y - windowOriginY)) < 1) { _ceDup = true; break; }
          }
          if (!_ceDup) {
            var _ceAng = _ce.angle || 0;
            deepCaveEntrances.push(Object.assign({}, _ce, {
              x: _ce.x - windowOriginX, y: _ce.y - windowOriginY,
              cosA: Math.cos(_ceAng), sinA: Math.sin(_ceAng)}));
          }
        }
      }
    }
  }

  // ── Clear the cave archway opening ─────────────────────────────────────────
  // Chunks were generated before entrances were known, so filter window-level
  // arrays after assembly. The archway is a visible structure at each entrance;
  // keep its footprint free of scatter/decorations/ore/chests/enemies so the
  // opening stays visually and mechanically unobstructed.
  if (deepCaveEntrances.length) {
    var _clrBefore = {s: floorScatter.length, d: wallDecorations.length,
                     o: oreVeins.length, c: treasureChests.length, e: enemies.length};
    floorScatter = floorScatter.filter(function(it){ return !inEntranceReserve(it.x, it.y); });
    wallDecorations = wallDecorations.filter(function(it){ return !inEntranceReserve(it.worldX, it.worldY); });
    oreVeins = oreVeins.filter(function(it){ return !inEntranceReserve(it.worldX, it.worldY); });
    treasureChests = treasureChests.filter(function(it){ return !inEntranceReserve(it.x, it.y); });
    enemies = enemies.filter(function(it){ return !inEntranceReserve(it.x, it.y); });
    console.log('[ARCH-CLEAR] scatter:' + _clrBefore.s + '→' + floorScatter.length +
      ' decor:' + _clrBefore.d + '→' + wallDecorations.length +
      ' ore:' + _clrBefore.o + '→' + oreVeins.length +
      ' chests:' + _clrBefore.c + '→' + treasureChests.length +
      ' enemies:' + _clrBefore.e + '→' + enemies.length);
  }

  // ── Enemy assembly debug ──
  var _enemyChunkCounts = {};
  for (var _ei = 0; _ei < enemies.length; _ei++) {
    var _e = enemies[_ei];
    var _ewx = _e.x + windowOriginX, _ewy = _e.y + windowOriginY;
    var _ecx = Math.floor(_ewx / CHUNK_SIZE), _ecy = Math.floor(_ewy / CHUNK_SIZE);
    var _eck = _ecx + ',' + _ecy;
    if (!_enemyChunkCounts[_eck]) _enemyChunkCounts[_eck] = {alive: 0, dead: 0, nearPlayer: 0};
    if (_e.health > 0) _enemyChunkCounts[_eck].alive++;
    else _enemyChunkCounts[_eck].dead++;
    var _edist = Math.hypot(_e.x - pos.x, _e.y - pos.y);
    if (_edist < 300) _enemyChunkCounts[_eck].nearPlayer++;
  }
  var _chunkSummary = [];
  for (var _ck in _enemyChunkCounts) {
    var _cc = _enemyChunkCounts[_ck];
    _chunkSummary.push(_ck + ':' + _cc.alive + 'a/' + _cc.dead + 'd/' + _cc.nearPlayer + 'near');
  }
  console.log('[ENEMY-ASSEMBLE] total=' + enemies.length + ' chunks={' + _chunkSummary.join(', ') + '}');

  // ── Terrain feature debug logging ──
  if (floorMesh && floorMesh.layerCount) {
    var _hMin = Infinity, _hMax = -Infinity, _waterCount = 0;
    var _nCells = floorMesh.layerCount.length;
    for (var _hi = 0; _hi < _nCells; _hi++) {
      var _hv = floorMesh.l0TopZ[_hi];
      if (_hv < _hMin) _hMin = _hv;
      if (_hv > _hMax) _hMax = _hv;
      if (floorMesh.water && floorMesh.water[_hi]) _waterCount++;
    }
    console.log('[TERRAIN] heightRange=[' + _hMin.toFixed(2) + ', ' + _hMax.toFixed(2) + ']' +
      ' waterCells=' + _waterCount + '/' + _nCells +
      ' (' + (_waterCount / _nCells * 100).toFixed(1) + '%)');

    // Cave floor color verification — sample colors of cells with negative heights
    if (floorMesh.colors) {
      var _caveColorSamples = [];
      var _caveCeilCount = 0;
      var _caveNegCount = 0;
      var _caveRSum = 0, _caveGSum = 0, _caveBSum = 0, _caveColorN = 0;
      for (var _ci = 0; _ci < _nCells; _ci++) {
        if (floorMesh.l0TopZ[_ci] < -0.5) {
          _caveNegCount++;
          if (meshCave && meshCave[_ci]) _caveCeilCount++;
          var _cc = floorMesh.colors[_ci];
          if (_cc && _cc.charAt(0) === '#') {
            var _cv = parseInt(_cc.slice(1), 16);
            _caveRSum += (_cv >> 16) & 0xff;
            _caveGSum += (_cv >> 8) & 0xff;
            _caveBSum += _cv & 0xff;
            _caveColorN++;
            if (_caveColorSamples.length < 5) _caveColorSamples.push(_cc);
          }
        }
      }
      if (_caveColorN > 0) {
        console.log('%c[CAVE-COLORS] ══════════════════════════════════════', 'color: #ee8844; font-weight: bold');
        console.log('[CAVE-COLORS] Cave cells: ' + _caveNegCount + ' negative height, ' + _caveCeilCount + ' with ceiling');
        console.log('[CAVE-COLORS] Average base floor color: R=' + Math.round(_caveRSum/_caveColorN) +
          ' G=' + Math.round(_caveGSum/_caveColorN) + ' B=' + Math.round(_caveBSum/_caveColorN) +
          '  (target was R=85 G=60 B=40 at full blend)');
        console.log('[CAVE-COLORS] Sample colors: ' + _caveColorSamples.join(', '));
        console.log('[CAVE-COLORS] Ambient cap: 0.35  →  rendered avg ≈ R=' +
          Math.round(_caveRSum/_caveColorN * 0.35) + ' G=' + Math.round(_caveGSum/_caveColorN * 0.35) +
          ' B=' + Math.round(_caveBSum/_caveColorN * 0.35));
        console.log('[CAVE-COLORS] OLD values would have been: base avg ≈ R=42 G=26 B=14, ambient=0.22, rendered ≈ R=9 G=6 B=3');
        console.log('%c[CAVE-COLORS] ══════════════════════════════════════', 'color: #ee8844');
      }
    }
  }
  if (largeStructures && largeStructures.length) {
    for (var _si = 0; _si < largeStructures.length; _si++) {
      var _st = largeStructures[_si];
      console.log('[STRUCTURE] type=' + _st.type + ' center=(' + _st.centerWX + ',' + _st.centerWY + ')' +
        ' region=(' + _st.regionX + ',' + _st.regionY + ')');
    }
  } else {
    console.log('[STRUCTURE] none in current window');
  }

  buildPointLights();

  // Assemble minimap explored cells
  exploredCells = new Uint8Array(totalCells * totalCells);
  for (var dy6 = 0; dy6 < WINDOW_CHUNKS; dy6++) {
    for (var dx6 = 0; dx6 < WINDOW_CHUNKS; dx6++) {
      var ch6 = chunks[(windowCX + dx6) + ',' + (windowCY + dy6)];
      var eOffX = dx6 * CHUNK_CELLS;
      var eOffY = dy6 * CHUNK_CELLS;
      for (var ey2 = 0; ey2 < CHUNK_CELLS; ey2++) {
        for (var ex2 = 0; ex2 < CHUNK_CELLS; ex2++) {
          if (ch6.exploredCells[ey2 * CHUNK_CELLS + ex2]) {
            exploredCells[(eOffY + ey2) * totalCells + (eOffX + ex2)] = 1;
          }
        }
      }
    }
  }

  // Rebase positions
  if (shiftX !== 0 || shiftY !== 0) {
    pos.x -= shiftX;
    pos.y -= shiftY;
    cam.x -= shiftX;
    cam.y -= shiftY;
    // Rebase projectiles, effects, etc
    for (var pi = 0; pi < projectiles.length; pi++) {
      projectiles[pi].x -= shiftX; projectiles[pi].y -= shiftY;
    }
    for (var ii = 0; ii < impacts.length; ii++) {
      impacts[ii].x -= shiftX; impacts[ii].y -= shiftY;
    }
    for (var gi2 = 0; gi2 < groundEffects.length; gi2++) {
      groundEffects[gi2].x -= shiftX; groundEffects[gi2].y -= shiftY;
    }
    for (var ci3 = 0; ci3 < coinDrops.length; ci3++) {
      coinDrops[ci3].x -= shiftX; coinDrops[ci3].y -= shiftY;
    }
    for (var so = 0; so < soulOrbs.length; so++) {
      soulOrbs[so].x -= shiftX; soulOrbs[so].y -= shiftY;
    }
    for (var de = 0; de < deathEffects.length; de++) {
      deathEffects[de].x -= shiftX; deathEffects[de].y -= shiftY;
    }
    for (var at = 0; at < arcaneTomes.length; at++) {
      arcaneTomes[at].x -= shiftX; arcaneTomes[at].y -= shiftY;
    }
    if (arenaChallenge) {
      arenaChallenge.centerX -= shiftX;
      arenaChallenge.centerY -= shiftY;
    }
    for (var sp = 0; sp < statPickups.length; sp++) {
      statPickups[sp].x -= shiftX; statPickups[sp].y -= shiftY;
    }
    for (var ci4 = 0; ci4 < companions.length; ci4++) {
      companions[ci4].x -= shiftX; companions[ci4].y -= shiftY;
    }
  }

  // Reinit minimap canvas
  minimapDirty = true;
  if (!minimapCanvas) {
    minimapCanvas = document.createElement('canvas');
    minimapCanvas.width = 90; minimapCanvas.height = 65;
  }
  renderMinimapBase();

  // Evict distant chunks
  evictDistantChunks(centerCX, centerCY);

  console.log('[ENDLESS] Window assembled at chunk (' + windowCX + ',' + windowCY + ') → (' +
    (windowCX + WINDOW_CHUNKS - 1) + ',' + (windowCY + WINDOW_CHUNKS - 1) + ')' +
    '  grid=' + gridW + 'x' + gridH + '  walls=' + walls.length +
    '  enemies=' + enemies.length + '  chests=' + treasureChests.length +
    '  chunks loaded=' + Object.keys(chunks).length);
}

function saveEnemyStateToChunks() {
  // Write live enemy state back to their source chunks so reassembly preserves it
  for (var dy = 0; dy < WINDOW_CHUNKS; dy++) {
    for (var dx = 0; dx < WINDOW_CHUNKS; dx++) {
      var key = (windowCX + dx) + ',' + (windowCY + dy);
      var ch = chunks[key];
      if (ch) ch.enemies = [];
    }
  }
  var saved = 0, discarded = 0;
  for (var i = 0; i < enemies.length; i++) {
    var e = enemies[i];
    if (e.health <= 0) { discarded++; continue; }
    var worldX = e.x + windowOriginX;
    var worldY = e.y + windowOriginY;
    var ecx = Math.floor(worldX / CHUNK_SIZE);
    var ecy = Math.floor(worldY / CHUNK_SIZE);
    // Clamp to current window bounds — prevents edge-wandered enemies being silently lost
    ecx = Math.max(windowCX, Math.min(windowCX + WINDOW_CHUNKS - 1, ecx));
    ecy = Math.max(windowCY, Math.min(windowCY + WINDOW_CHUNKS - 1, ecy));
    var key2 = ecx + ',' + ecy;
    var ch2 = chunks[key2];
    if (!ch2) { discarded++; continue; } // safety guard — shouldn't fire after clamp
    ch2.enemies.push({
      x: worldX, y: worldY, z: e.z || 0,
      caveSpawnId: e.caveSpawnId || null, underground: e.underground || false,
      enemyType: e.enemyType, health: e.health, maxHealth: e.maxHealth,
      speed: e.speed, chaseRange: e.chaseRange, lastUpdate: e.lastUpdate,
      damageFlash: e.damageFlash, damageFlashColor: e.damageFlashColor,
      slowUntil: e.slowUntil, burnUntil: e.burnUntil, burnDmgLast: e.burnDmgLast,
      iceHits: e.iceHits || 0, fireHits: e.fireHits || 0, lightningHits: e.lightningHits || 0,
      vx: e.vx || 0, vy: e.vy || 0, aggroAt: e.aggroAt || 0,
      attackState: e.attackState || 'idle', attackStateUntil: e.attackStateUntil || 0,
      patrolWaypoints: e.patrolWaypoints.map(function(wp) {
        return {x: wp.x + windowOriginX, y: wp.y + windowOriginY};
      }),
      patrolIdx: e.patrolIdx || 0, facing: e.facing || 0
    });
    saved++;
  }
  console.log('[ENEMY-SAVE] Saved ' + saved + ' enemies to chunks, discarded ' + discarded + ' dead');
}

function saveAllEntityDeltas() {
  // Save live enemy state back to chunks
  saveEnemyStateToChunks();
  // Save modified chest/spawner state back to chunks (both in-memory chunk + deltas for eviction)
  for (var i = 0; i < treasureChests.length; i++) {
    var tc = treasureChests[i];
    if (tc._chunkKey) {
      var tcChunk = chunks[tc._chunkKey];
      if (tcChunk && tc._chunkIdx < tcChunk.treasureChests.length) {
        tcChunk.treasureChests[tc._chunkIdx].collected = tc.collected;
        tcChunk.treasureChests[tc._chunkIdx].opened = tc.opened || false;
        tcChunk.treasureChests[tc._chunkIdx].lidAngle = tc.lidAngle || 0;
        tcChunk.treasureChests[tc._chunkIdx].equipId = tc.equipId;
        tcChunk.treasureChests[tc._chunkIdx].relicId = tc.relicId;
      }
      if (tc.collected) {
        if (!entityDeltas[tc._chunkKey]) entityDeltas[tc._chunkKey] = {};
        if (!entityDeltas[tc._chunkKey].chestsCollected) entityDeltas[tc._chunkKey].chestsCollected = [];
        if (entityDeltas[tc._chunkKey].chestsCollected.indexOf(tc._chunkIdx) < 0) {
          entityDeltas[tc._chunkKey].chestsCollected.push(tc._chunkIdx);
        }
      }
    }
  }
  for (var i2 = 0; i2 < enemySpawners.length; i2++) {
    var sp = enemySpawners[i2];
    if (sp._chunkKey) {
      var spChunk = chunks[sp._chunkKey];
      if (spChunk && sp._chunkIdx < spChunk.enemySpawners.length) {
        spChunk.enemySpawners[sp._chunkIdx].active = sp.active;
        spChunk.enemySpawners[sp._chunkIdx].hp = sp.hp;
        spChunk.enemySpawners[sp._chunkIdx].spawnCount = sp.spawnCount;
        spChunk.enemySpawners[sp._chunkIdx].lastSpawn = sp.lastSpawn;
      }
      if (!sp.active) {
        if (!entityDeltas[sp._chunkKey]) entityDeltas[sp._chunkKey] = {};
        if (!entityDeltas[sp._chunkKey].spawnersDestroyed) entityDeltas[sp._chunkKey].spawnersDestroyed = [];
        if (entityDeltas[sp._chunkKey].spawnersDestroyed.indexOf(sp._chunkIdx) < 0) {
          entityDeltas[sp._chunkKey].spawnersDestroyed.push(sp._chunkIdx);
        }
      }
    }
  }
  // Save shrine used state back to chunks
  for (var si3 = 0; si3 < shrines.length; si3++) {
    var shr = shrines[si3];
    if (shr.used && shr.chunkKey) {
      var shrChunk = chunks[shr.chunkKey];
      if (shrChunk && shrChunk.shrine) shrChunk.shrine.used = true;
      if (!entityDeltas[shr.chunkKey]) entityDeltas[shr.chunkKey] = {};
      entityDeltas[shr.chunkKey].shrineUsed = true;
    }
  }
  // Save explored cells back to chunks
  for (var dy = 0; dy < WINDOW_CHUNKS; dy++) {
    for (var dx = 0; dx < WINDOW_CHUNKS; dx++) {
      var key = (windowCX + dx) + ',' + (windowCY + dy);
      var ch = chunks[key];
      if (!ch) continue;
      var offX = dx * CHUNK_CELLS;
      var offY = dy * CHUNK_CELLS;
      for (var ly = 0; ly < CHUNK_CELLS; ly++) {
        for (var lx = 0; lx < CHUNK_CELLS; lx++) {
          if (exploredCells[(offY + ly) * gridW + (offX + lx)]) {
            ch.exploredCells[ly * CHUNK_CELLS + lx] = 1;
          }
        }
      }
      // Persist explored to deltas for evicted chunks
      if (!entityDeltas[key]) entityDeltas[key] = {};
      entityDeltas[key].explored = new Uint8Array(ch.exploredCells);
    }
  }
}

function evictDistantChunks(centerCX, centerCY) {
  var maxDist = WINDOW_CHUNKS + 2;
  var keys = Object.keys(chunks);
  for (var i = 0; i < keys.length; i++) {
    var ch = chunks[keys[i]];
    if (Math.abs(ch.cx - centerCX) > maxDist || Math.abs(ch.cy - centerCY) > maxDist) {
      delete chunks[keys[i]];
    }
  }
}

function updateChunks() {
  if (!ENDLESS_MODE) return;
  var trueX = pos.x + windowOriginX;
  var trueY = pos.y + windowOriginY;
  var pcx = Math.floor(trueX / CHUNK_SIZE);
  var pcy = Math.floor(trueY / CHUNK_SIZE);
  var half = Math.floor(WINDOW_CHUNKS / 2);
  var centerCX = windowCX + half;
  var centerCY = windowCY + half;
  if (pcx !== centerCX || pcy !== centerCY) {
    console.log('[ENDLESS] Chunk shift triggered: player chunk (' + pcx + ',' + pcy +
      ') vs window center (' + centerCX + ',' + centerCY + ')' +
      '  pos=(' + pos.x.toFixed(1) + ',' + pos.y.toFixed(1) + ')' +
      '  truePos=(' + trueX.toFixed(1) + ',' + trueY.toFixed(1) + ')' +
      '  windowOrigin=(' + windowOriginX + ',' + windowOriginY + ')');
    var t0 = performance.now();
    try {
      assembleWindow(pcx, pcy);
      var dt = performance.now() - t0;
      console.log('[ENDLESS] Window assembled OK in ' + dt.toFixed(1) + 'ms' +
        '  newPos=(' + pos.x.toFixed(1) + ',' + pos.y.toFixed(1) + ')' +
        '  newOrigin=(' + windowOriginX + ',' + windowOriginY + ')' +
        '  grid=' + gridW + 'x' + gridH + '  walls=' + walls.length +
        '  enemies=' + enemies.length + '  chunks=' + Object.keys(chunks).length);
    } catch (e) {
      console.error('[ENDLESS] assembleWindow CRASHED:', e.message, '\n', e.stack);
    }
  }

  // Update biome at player position
  var newBiome = getBiomeAt(trueX, trueY);
  if (newBiome !== terrain) {
    terrain = newBiome;
    applyPreset();
  }
}

function settlePlayerAtSpawn(preferSurface) {
  var h = getFloorHeightAt(pos.x, pos.y);
  if (preferSurface && floorMesh && floorMesh.layerCount) {
    var gx = Math.floor(pos.x / floorMesh.gridSize), gy = Math.floor(pos.y / floorMesh.gridSize);
    var li = getTopWalkableLayerIdx(gx, gy);
    if (li >= 0) h = meshLayerHeight(floorMesh, gy * floorMesh.w + gx, li);
  }
  jumpVelZ = 0;
  jumpAirborne = false;
  pos.floorZ = 60 + h * 40;
  cam.x = pos.x; cam.y = pos.y; cam.z = pos.floorZ;
}

function resetEndlessMode() {
  ENDLESS_MODE = true;
  level = 1;
  collisions = 0;
  startMs = Date.now();
  lastSpellChangeMs = startMs;
  terrain = CAVE_TEST_MODE ? 'plains' : 'ground';
  chunks = {};
  entityDeltas = {};

  // Set seeds — Cave Test uses a fixed seed for reproducible results
  WORLD_SEED = CAVE_TEST_MODE ? (window._caveTestSeedOverride || 12345) : Math.floor(Math.random() * 999999) + 1;
  noiseSeed = (WORLD_SEED * 7 + 42) | 0;
  _biomeSeed = (WORLD_SEED * 13 + 999) | 0;
  _geoSeed = (WORLD_SEED * 19 + 5555) | 0;
  _ampBoost = 1.2;

  // Clear all state
  walls = [];
  currentBorderPoly = null;
  borderVariations = [];
  deepCaveRegions = [];
  deepCaveEntrances = [];
  deepCaveChambers = [];
  _caveGrid = null;
  endlessCaveNetworks = {};
  platforms = [];
  oreVeins = [];
  floorScatter = [];
  wallDecorations = [];
  treasureChests = [];
  enemySpawners = [];
  enemies = [];
  coinDrops = [];
  coins = 0;
  projectiles = [];
  impacts = [];
  coneEffects = [];
  flameStreamActive = false; flameStreamLastTick = 0;
  groundEffects = [];
  chainEffects = [];
  novaEffects = [];
  deathEffects = [];
  soulOrbs = [];
  ambientParticles = [];
  wardActive = false;
  dmgBoostUntil = 0;
  speedBoostUntil = 0;
  regenBoostUntil = 0;
  armorBoostUntil = 0;
  lastRegenTick = 0;
  shopMarker = null;
  marketStalls = [];
  shrines = [];
  ruins = [];
  largeStructures = [];
  discoveredMarkets = [];
  discoveredShrines = [];
  discoveredRuins = [];
  guardTowerLastFire = 0;
  shopOpen = false;
  nearestOpenChest = null;
  arenaChallenge = null;
  completedArenas = {};
  nearestArenaAltar = null;
  arcaneTomes = [];
  statPickups = [];
  collectedStatPickups = {};
  permanentSpeedBonus = 0;
  permanentHealthBonus = 0;
  permanentManaBonus = 0;
  HEALTH_MAX = 100; health = 100;
  MANA_MAX = 100; mana = 100;
  companions = [];
  equipment = {armor: null, hat: null, robes: null, boots: null, relic: null};
  phoenixFeatherUsed = false;
  goal = null;
  goalSpawned = false;

  // Set window at origin
  windowCX = -Math.floor(WINDOW_CHUNKS / 2);
  windowCY = -Math.floor(WINDOW_CHUNKS / 2);
  windowOriginX = windowCX * CHUNK_SIZE;
  windowOriginY = windowCY * CHUNK_SIZE;

  // Player starts at world origin
  pos = {x: CHUNK_SIZE / 2 - windowOriginX, y: CHUNK_SIZE / 2 - windowOriginY};
  vel = {x: 0, y: 0};
  cam.x = pos.x; cam.y = pos.y; cam.ang = 0; cam.z = 60; cam.pitch = 0;
  pos.floorZ = 60;
  health = HEALTH_MAX;
  mana = MANA_MAX;

  // Build initial window
  assembleWindow(0, 0);

  // Cave test mode: teleport player near the cave entrance
  if (CAVE_TEST_MODE) {
    var _ctNet = endlessCaveNetworks['0,0'];
    if (_ctNet && _ctNet.entrances && _ctNet.entrances.length > 0) {
      var _ctE = _ctNet.entrances[0];
      // Stand 60 units outside the entrance, facing in
      var outDX = -Math.cos(_ctE.angle);
      var outDY = -Math.sin(_ctE.angle);
      pos.x = _ctE.x + outDX * 60 - windowOriginX;
      pos.y = _ctE.y + outDY * 60 - windowOriginY;
      cam.ang = _ctE.angle; // face into the cave
      console.log('[CAVE TEST] Spawned near entrance at world (' + _ctE.x.toFixed(0) + ',' + _ctE.y.toFixed(0) + ')');
    }
  }

  // Spawn at the selected support immediately, not at the legacy zero-height
  // default followed by a fall. Reset vertical state left over from the last
  // world as well; opening Cave Test during a jump must not launch the player.
  settlePlayerAtSpawn(!CAVE_TEST_MODE);

  // Activate settings
  CAM_FOLLOW = true;
  initAmbientParticles();
  applyPreset();

  // Generation stats
  var wallCount = 0, floorCount = 0;
  for (var si3 = 0; si3 < grid.length; si3++) { if (grid[si3]) wallCount++; }
  floorCount = grid.length - wallCount;
  var biomeStats = {};
  var chunkKeys = Object.keys(chunks);
  for (var ck = 0; ck < chunkKeys.length; ck++) {
    var b = chunks[chunkKeys[ck]].biome;
    biomeStats[b] = (biomeStats[b] || 0) + 1;
  }
  var biomeStr = '';
  for (var bk in biomeStats) biomeStr += bk + '=' + biomeStats[bk] + ' ';
  console.log('[ENDLESS] ═══════════════════════════════════════════');
  console.log('[ENDLESS] Seed: ' + WORLD_SEED + '  Window: ' + WINDOW_CHUNKS + 'x' + WINDOW_CHUNKS + ' chunks');
  console.log('[ENDLESS] Grid: ' + gridW + 'x' + gridH + ' = ' + grid.length + ' cells (' + wallCount + ' walls, ' + floorCount + ' floor, ' + (wallCount / grid.length * 100).toFixed(1) + '% wall)');
  console.log('[ENDLESS] Entities: enemies=' + enemies.length + '  chests=' + treasureChests.length + '  spawners=' + enemySpawners.length + '  scatter=' + floorScatter.length + '  decors=' + wallDecorations.length);
  console.log('[ENDLESS] Biomes: ' + biomeStr);
  console.log('[ENDLESS] Mesh: ' + floorMesh.w + 'x' + floorMesh.h + '  walls[]=' + walls.length + ' rects');
  console.log('[ENDLESS] ═══════════════════════════════════════════');

  // Auto-show overview so the player can see the world layout
  var _autoOverview = document.getElementById('chkAutoOverview');
  if (!CAVE_TEST_MODE && (!_autoOverview || _autoOverview.checked)) {
    setTimeout(function() { if (!overviewActive) toggleOverview(); }, 100);
  }
}

function resetLevel(lv) {
  level = lv || 1;
  collisions = 0;
  startMs = Date.now();
  lastSpellChangeMs = startMs;
  document.getElementById('hudLevel').textContent = level;
  document.getElementById('hudCol').textContent = collisions;
  vel = {x:0, y:0};
  var idx = (level - 1);
  if (idx < 0) idx = 0;
  if (idx >= levels.length) idx = levels.length - 1;
  var cfg = levels[idx];
  // If terrain is 'expanse' but the current level config is a small map,
  // use the Expanse level config (last level) so dimensions match.
  if (terrain === 'expanse' && !cfg.terrain && levels.length > 0) {
    var expanseCfg = null;
    for (var li = 0; li < levels.length; li++) {
      if (levels[li].terrain === 'expanse') { expanseCfg = levels[li]; break; }
    }
    if (expanseCfg) cfg = expanseCfg;
  }
  // World dimensions come from the level definition — set globals first so all
  // downstream functions (buildGrid, generateFloorMesh, etc.) pick them up.
  worldW = cfg.w || 720;
  worldH = cfg.h || 480;
  // View distance scales with map size — ensures large maps feel appropriately vast
  viewDist = Math.max(settings.viewDist, Math.min(worldW, worldH) * 0.8);

  // ── MAP SIZING DIAGNOSTICS ────────────────────────────────────────────────
  var _area       = worldW * worldH;
  var _baseArea   = 720 * 480;
  var _gridSzPrev = (_area > 1500000) ? 12 : 6;          // matches generateFloorMesh
  var _meshW      = Math.ceil(worldW / _gridSzPrev);
  var _meshH      = Math.ceil(worldH / _gridSzPrev);
  var _gridWp     = Math.floor(worldW / cell);            // collision grid dims
  var _gridHp     = Math.floor(worldH / cell);
  var _radScale   = Math.max(1.0, Math.sqrt(_area / _baseArea));
  var _maxBite    = Math.min(Math.min(worldW, worldH) * 0.18, 180);
  var _viewDistCalc = Math.max(800, Math.min(worldW, worldH) * 0.7);
  console.log('[MAP] ═══════════════════════════════════════════');
  console.log('[MAP] Level ' + lv + '  terrain=' + (cfg.terrain || terrain));
  console.log('[MAP] World:      ' + worldW + ' × ' + worldH + ' px  (' + (worldW/720).toFixed(1) + 'x  ' + (_area/1e6).toFixed(2) + ' Mpx)');
  console.log('[MAP] Floor mesh: ' + _meshW + ' × ' + _meshH + ' cells  gridSize=' + _gridSzPrev + 'px  (' + (_meshW*_meshH) + ' total)');
  console.log('[MAP] Coll grid:  ' + _gridWp + ' × ' + _gridHp + ' cells  cell=' + cell + 'px');
  console.log('[MAP] viewDist:   ' + _viewDistCalc.toFixed(0) + ' px  (' + (_viewDistCalc/worldW*100).toFixed(0) + '% of width)');
  console.log('[MAP] radiusScale:' + _radScale.toFixed(2) + '×  maxBite=' + _maxBite.toFixed(0) + 'px');
  console.log('[MAP] Start:      (' + cfg.start.x + ',' + cfg.start.y + ')  Goal: (' + cfg.goal.x + ',' + cfg.goal.y + ')');
  console.log('[MAP] ═══════════════════════════════════════════');
  // ─────────────────────────────────────────────────────────────────────────

  // Level config can pin a terrain type (e.g. the Expanse level auto-selects 'expanse')
  if (cfg.terrain) {
    terrain = cfg.terrain;
    var sel = document.getElementById('terrainSelect');
    if (sel) sel.value = terrain;
  }
  pos = {x:cfg.start.x, y:cfg.start.y};
  goal = null; goalSpawned = false; goalMessage = ''; goalMessageUntil = 0; toasts = []; toastLog = []; toastLogScroll = 0; toastLogOpen = false;
  walls = cfg.walls.slice();
  var usesBorderPoly = (terrain === 'plains' || terrain === 'cave' || terrain === 'expanse' || terrain === 'ground' || terrain === 'ice');
  if (usesBorderPoly) {
    currentBorderPoly = generateBorderPolygon();
    walls = [];
  } else {
    currentBorderPoly = null;
  }
  // Pipeline: walls → border → border variation → deep caves → floor mesh
  if (terrain === 'cave') { generateCaveWalls(); }  // adds interior obstacles to walls[]
  buildGrid();
  borderVariations = [];
  deepCaveRegions = [];
  deepCaveEntrances = [];
  deepCaveChambers = [];
  _caveGrid = null;
  endlessCaveNetworks = {};
  if (currentBorderPoly) {
    applyIrregularBorder(currentBorderPoly);
    if (usesBorderPoly) { generateBorderVariation(); }  // subtle wall roughening on all bordered terrains
    if (terrain === 'expanse') { generateDeepCaves(); } // real deep cave networks (expanse only)
    generateOpenAreaObstacles();  // scatter pillars, ruins, rocks across open floor
  }
  if (deepCaveRegions.length > 0) { buildCaveSpatialGrid(); }  // spatial index for O(1) isInDeepCave()
  if (usesBorderPoly) { generateBlendedFloor(); }   // cave-aware: stamps ceilH into mesh
  else { platforms = []; floorMesh = null; }
  if (currentBorderPoly) {
    var safe = findRandomOpenSpawn(currentBorderPoly);
    pos.x = safe.x; pos.y = safe.y;
    console.log('[MAP] Safe spawn: (' + pos.x.toFixed(1) + ',' + pos.y.toFixed(1) + ')  grid=(' + Math.floor(pos.x/cell) + ',' + Math.floor(pos.y/cell) + ')  wall=' + (isInGridWall(pos.x,pos.y,6) ? 'YES ⚠' : 'no'));
  }
  cam.ang = 0; cam.pitch = 0;
  settlePlayerAtSpawn(true);
  CAM_FOLLOW = true;
  oreVeins = [];
  floorScatter = [];
  treasureChests = [];
  enemySpawners = [];
  createWallDecorations();
  buildPointLights();
  generateOreVeins();
  generateFloorScatter();
  generateCaveInteractables();
  generateSurfaceChests();
  spawnEnemies(cfg);
  coinDrops = []; coins = 0; wardActive = false; dmgBoostUntil = 0; speedBoostUntil = 0;
  groundEffects = []; chainEffects = []; novaEffects = [];
  projectiles = []; impacts = []; coneEffects = [];
  ambientParticles = [];
  initAmbientParticles();
  initMinimap();
  spawnShop();
  medalGold = (cfg.medals && cfg.medals.gold) ? cfg.medals.gold : 10;
  medalSilver = (cfg.medals && cfg.medals.silver) ? cfg.medals.silver : 15;
  medalBronze = (cfg.medals && cfg.medals.bronze) ? cfg.medals.bronze : 22;
  timeSec = 0; timeColor = '#d4af37';
  applyPreset();

  // ── Generation summary — geometry counts for profiling ──────────────
  var _smv = floorMesh ? (floorMesh.w * floorMesh.h) : 0;
  var _smq = floorMesh ? ((floorMesh.w - 1) * (floorMesh.h - 1)) : 0;
  var _swc = 0; if (grid) { for (var _si = 0; _si < grid.length; _si++) { if (grid[_si]) _swc++; } }
  console.log('[GEN] ── Geometry Summary ──────────────────────────');
  console.log('[GEN] Grid: ' + gridW + 'x' + gridH + ' = ' + (gridW*gridH) + ' cells (' + _swc + ' walls, ' + (gridW*gridH-_swc) + ' floor)');
  console.log('[GEN] Mesh: ' + (floorMesh ? floorMesh.w+'x'+floorMesh.h : '—') + ' = ' + _smv + ' verts, ' + _smq + ' quads, ' + (_smq*4) + ' edges');
  console.log('[GEN] Border: ' + (currentBorderPoly ? currentBorderPoly.length + ' verts' : 'none') + '  variations=' + (borderVariations ? borderVariations.length : 0));
  console.log('[GEN] Caves: ' + (deepCaveRegions ? deepCaveRegions.length : 0) + ' segs (' +
    (deepCaveRegions ? deepCaveRegions.filter(function(r){return r.type==='corridor'}).length : 0) + ' corr + ' +
    (deepCaveRegions ? deepCaveRegions.filter(function(r){return r.type==='chamber'}).length : 0) + ' cham)  chambers=' +
    (deepCaveChambers ? deepCaveChambers.length : 0) + '  entrances=' + (deepCaveEntrances ? deepCaveEntrances.length : 0));
  console.log('[GEN] Objects: ore=' + (oreVeins ? oreVeins.length : 0) + '  scatter=' + (floorScatter ? floorScatter.length : 0) +
    '  wallDecors=' + (wallDecorations ? wallDecorations.length : 0) + '  chests=' + (treasureChests ? treasureChests.length : 0) +
    '  spawners=' + (enemySpawners ? enemySpawners.length : 0) + '  enemies=' + (enemies ? enemies.length : 0));
  console.log('[GEN] ─────────────────────────────────────────────');

  // Auto-overview: show full map on load if the checkbox is checked
  var chkAuto = document.getElementById('chkAutoOverview');
  if (chkAuto && chkAuto.checked) {
    // Small delay so the game loop has initialised canvas dimensions first
    setTimeout(function() { if (!overviewActive) toggleOverview(); }, 80);
  }
}
