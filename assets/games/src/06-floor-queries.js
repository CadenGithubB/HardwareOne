// =============================================
// SECTION 4: FLOOR HELPERS
// =============================================

// 1024-slot cache of recent (gridX, gridY) → lowest walkable floor lookups. Knuth multiplicative
// hash mixes both coords so row/column sweeps don't degenerate to a single slot.
// Bumped each frame via _floorCacheTick so stale entries miss on a new frame.
var _floorCacheKey = new Int32Array(1024);
var _floorCacheVal = new Float32Array(1024);
var _floorCacheTickArr = new Int32Array(1024);
var _floorCacheTick = 0;
var _floorCacheMesh = null;
var _lightGridFrameCount = 0;
function getFloorHeightAt(x, y) {
  if (!floorMesh) return 0;
  var mesh = floorMesh;
  // A window rebuild can query floors before the next draw tick. Coordinates
  // now refer to a different mesh, so the previous window's cache is invalid.
  if (_floorCacheMesh !== mesh) {
    _floorCacheMesh = mesh;
    _floorCacheTick = (_floorCacheTick + 1) | 0;
  }
  var gridX = Math.floor(x / mesh.gridSize);
  var gridY = Math.floor(y / mesh.gridSize);
  if (gridX < 0 || gridX >= mesh.w || gridY < 0 || gridY >= mesh.h) return 0;
  var key = (gridX << 16) | (gridY & 0xffff);
  var slot = ((gridX * 2654435761) ^ gridY) & 1023;
  if (_floorCacheKey[slot] === key && _floorCacheTickArr[slot] === _floorCacheTick) {
    _cacheStats.floorH.hits++;
    return _floorCacheVal[slot];
  }
  var v = mesh.l0TopZ[gridY * mesh.w + gridX];
  if (mesh.layerCount) {
    var cellIndex = gridY * mesh.w + gridX, lowest = Infinity;
    for (var li = 0; li < mesh.layerCount[cellIndex]; li++) {
      var type = meshLayerType(mesh, cellIndex, li);
      if (type === 1 || type === 3 || type === 4) lowest = Math.min(lowest, meshLayerHeight(mesh, cellIndex, li));
    }
    v = lowest < Infinity ? lowest : 0;
  }
  _floorCacheKey[slot] = key;
  _floorCacheVal[slot] = v;
  _floorCacheTickArr[slot] = _floorCacheTick;
  _cacheStats.floorH.misses++;
  _cacheStats.floorH.size = 1024;
  return v;
}

// Mesh heights are the spatial contract. Player physics retains its legacy
// offset/scale; rendering converts feet H separately and adds its eye height.
var PLAYER_BODY_H = 2.8;
function meshHeightToPlayerZ(h) { return h * 40 + 60; }
function getPlayerFloorH() {
  return ((typeof pos.floorZ === 'number' && isFinite(pos.floorZ) ? pos.floorZ : 60) - 60) / 40;
}
function meshLayerType(mesh, i, li) { return mesh['l' + li + 'Type'][i]; }
function meshLayerHeight(mesh, i, li) { return mesh['l' + li + 'TopZ'][i]; }
function meshCellAt(x, y) {
  if (!floorMesh || !floorMesh.layerCount) return -1;
  var gx = Math.floor(x / floorMesh.gridSize), gy = Math.floor(y / floorMesh.gridSize);
  return gx < 0 || gy < 0 || gx >= floorMesh.w || gy >= floorMesh.h ? -1 : gy * floorMesh.w + gx;
}

// Actual overhead geometry, independent of time of day, nearest entrance,
// or whether the player is grounded. A roof below the feet is not overhead.
function getCaveSpaceAt(x, y, feetH) {
  var i = meshCellAt(x, y), ceilingH = Infinity;
  if (i >= 0) {
    for (var li = 0; li < floorMesh.layerCount[i]; li++) {
      var type = meshLayerType(floorMesh, i, li), h = meshLayerHeight(floorMesh, i, li);
      // A cap within step height is our surface support while the grounded
      // camera eases upward, not a second roof above our head.
      if ((type === 2 || (type === 4 && h > feetH + 1.0)) && h > feetH + 0.001 && h < ceilingH) ceilingH = h;
    }
  }
  return {ceilingH: ceilingH, underground: ceilingH < Infinity,
    clearanceH: ceilingH - feetH, cellIdx: i};
}

// Pick support only within the same open vertical interval. A ceiling between
// the actor and a lower floor prevents falling through the roof, even when
// there is no cap in the cell. Low headroom is a blocked passage, not a floor.
// topH is null when no reachable support exists; callers must read action.
function getWalkableLayerTopAt(x, y, playerH, options) {
  var mesh = floorMesh, i = meshCellAt(x, y);
  if (i < 0 || !mesh.layerCount[i]) return {topH: null, idx: -1, type: 0, action: 'missing', ceilingH: Infinity};
  var stepUp = options && options.stepUp !== undefined ? options.stepUp : 1.0;
  var bodyH = options && options.bodyH !== undefined ? options.bodyH : PLAYER_BODY_H;
  var best = null, bestDiff = Infinity, fall = null, blocked = null;
  for (var li = 0; li < mesh.layerCount[i]; li++) {
    var t = meshLayerType(mesh, i, li), z = meshLayerHeight(mesh, i, li);
    if (t !== 1 && t !== 3 && t !== 4) continue;
    var ceilingH = Infinity, separated = false;
    for (var ci = 0; ci < mesh.layerCount[i]; ci++) {
      var ct = meshLayerType(mesh, i, ci), cz = meshLayerHeight(mesh, i, ci);
      if (ct !== 2 && ct !== 4) continue;
      if (cz > z + 0.001 && cz < ceilingH) ceilingH = cz;
      if (ct === 2 && cz > Math.min(z, playerH) + 0.001 && cz <= Math.max(z, playerH) + 0.001) separated = true;
    }
    var candidate = {topH: z, idx: li, type: t, action: 'walk', ceilingH: ceilingH};
    if (separated || ceilingH - Math.max(z, playerH) < bodyH - 0.001 || z - playerH > stepUp) {
      if (!blocked || Math.abs(z - playerH) < Math.abs(blocked.topH - playerH)) blocked = candidate;
      continue;
    }
    var dz = z - playerH;
    if (dz < -1.0) {
      if (!fall || z > fall.topH) fall = candidate;
    } else if (Math.abs(dz) < bestDiff) {
      bestDiff = Math.abs(dz); best = candidate;
    }
  }
  if (best) return best;
  if (fall) { fall.supportIdx = fall.idx; fall.idx = -1; fall.action = 'fall'; return fall; }
  if (blocked) { blocked.supportIdx = blocked.idx; blocked.idx = -1; blocked.action = 'blocked'; return blocked; }
  return {topH: null, idx: -1, type: 0, action: 'missing', ceilingH: Infinity};
}

// Index of the topmost walkable layer in this cell (or -1 if none).
function getTopWalkableLayerIdx(gx, gy) {
  if (!floorMesh || !floorMesh.layerCount) return -1;
  var mesh = floorMesh;
  if (gx < 0 || gx >= mesh.w || gy < 0 || gy >= mesh.h) return -1;
  var i = gy * mesh.w + gx;
  var lc = mesh.layerCount[i];
  for (var li = lc - 1; li >= 0; li--) {
    var t;
    if (li === 0) t = mesh.l0Type[i];
    else if (li === 1) t = mesh.l1Type[i];
    else if (li === 2) t = mesh.l2Type[i];
    else if (li === 3) t = mesh.l3Type[i];
    else t = mesh.l4Type[i];
    if (t === 1 || t === 3 || t === 4) return li;
  }
  return -1;
}

function getFloorColorAt(x, y) {
  if (!floorMesh) return getFloorColor(0);
  var mesh = floorMesh;
  var gridX = Math.floor(x / mesh.gridSize);
  var gridY = Math.floor(y / mesh.gridSize);
  if (gridX < 0 || gridX >= mesh.w || gridY < 0 || gridY >= mesh.h) return getFloorColor(0);
  return mesh.colors[gridY * mesh.w + gridX];
}

function getFloorColor(heightPercent) {
  var bands = (BIOME_PALETTE[terrain] || BIOME_PALETTE.ground).floorBands;
  for (var bi = 0; bi < _floorBandThresholds.length; bi++) {
    if (heightPercent < _floorBandThresholds[bi]) return bands[bi];
  }
  return bands[7];
}

function avgColor(c1, c2, c3, c4) {
  var r1 = parseInt(c1.substr(1, 2), 16), g1 = parseInt(c1.substr(3, 2), 16), b1 = parseInt(c1.substr(5, 2), 16);
  var r2 = parseInt(c2.substr(1, 2), 16), g2 = parseInt(c2.substr(3, 2), 16), b2 = parseInt(c2.substr(5, 2), 16);
  var r3 = parseInt(c3.substr(1, 2), 16), g3 = parseInt(c3.substr(3, 2), 16), b3 = parseInt(c3.substr(5, 2), 16);
  var r4 = parseInt(c4.substr(1, 2), 16), g4 = parseInt(c4.substr(3, 2), 16), b4 = parseInt(c4.substr(5, 2), 16);
  var r = Math.floor((r1 + r2 + r3 + r4) / 4);
  var g = Math.floor((g1 + g2 + g3 + g4) / 4);
  var b = Math.floor((b1 + b2 + b3 + b4) / 4);
  return '#' + ('0' + r.toString(16)).slice(-2) + ('0' + g.toString(16)).slice(-2) + ('0' + b.toString(16)).slice(-2);
}
