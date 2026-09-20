// Shared opaque-scene visibility for the Canvas renderer. Unlike the legacy
// wall-only column buffer, this records floors, ceilings and walls at each
// screen pixel. Store reciprocal forward depth: it interpolates linearly in
// screen space even when a floor recedes steeply into the distance.
var _sceneDepthW = 0, _sceneDepthH = 0;
var _sceneDepthInv = new Float32Array(0);
var _sceneDepthClipMarks = new Uint32Array(0);
var _sceneDepthClipInv = new Float32Array(0);
var _sceneDepthClipRowMarks = new Uint32Array(0);
var _sceneDepthClipRowMin = new Int32Array(0), _sceneDepthClipRowMax = new Int32Array(0);
var _sceneDepthClipGeneration = 0;
var _sceneDepthClipCount = 0, _sceneDepthClipHidden = 0;
var _sceneDepthClipMinX = 0, _sceneDepthClipMaxX = -1;
var _sceneDepthClipMinY = 0, _sceneDepthClipMaxY = -1;

function beginSceneDepthFrame(w, h) {
  w = Math.floor(w); h = Math.floor(h);
  if (!Number.isFinite(w) || !Number.isFinite(h) || w <= 0 || h <= 0) {
    _sceneDepthW = _sceneDepthH = 0;
    return false;
  }
  var size = w * h;
  // Allocation depends only on canvas size, never on scene complexity. Reuse
  // the storage on every subsequent frame and when shrinking the canvas.
  if (_sceneDepthInv.length < size) {
    _sceneDepthInv = new Float32Array(size);
    _sceneDepthClipMarks = new Uint32Array(size);
    _sceneDepthClipInv = new Float32Array(size);
  } else {
    _sceneDepthInv.fill(0, 0, size);
  }
  if (_sceneDepthClipRowMarks.length < h) {
    _sceneDepthClipRowMarks = new Uint32Array(h);
    _sceneDepthClipRowMin = new Int32Array(h);
    _sceneDepthClipRowMax = new Int32Array(h);
  }
  _sceneDepthW = w; _sceneDepthH = h;
  return true;
}

function sceneDepthAt(x, y) {
  x = Math.floor(x); y = Math.floor(y);
  if (!Number.isFinite(x) || !Number.isFinite(y) || x < 0 || y < 0 ||
      x >= _sceneDepthW || y >= _sceneDepthH) return Infinity;
  var inverse = _sceneDepthInv[y * _sceneDepthW + x];
  return inverse > 0 ? 1 / inverse : Infinity;
}

// Sprites and their glow/labels occupy a screen rectangle at one camera-forward
// depth. They read opaque world depth but never write their transparent bounds.
function withSceneDepthBillboard(bounds, depth, drawCallback, options) {
  if (!bounds || !Number.isFinite(depth) || depth < 1 ||
      !Number.isFinite(bounds.x) || !Number.isFinite(bounds.y) ||
      !Number.isFinite(bounds.width) || !Number.isFinite(bounds.height) ||
      bounds.width <= 0 || bounds.height <= 0) return 0;
  var x = bounds.x, y = bounds.y, right = x + bounds.width, bottom = y + bounds.height;
  if (right <= 0 || bottom <= 0 || x >= _sceneDepthW || y >= _sceneDepthH) return 0;
  return withSceneDepthClip([
    {x:x,y:y,depth:depth}, {x:right,y:y,depth:depth},
    {x:right,y:bottom,depth:depth}, {x:x,y:bottom,depth:depth}
  ], drawCallback, {depthBias: options && Number.isFinite(options.depthBias) ? options.depthBias : 0.15});
}

// Sample the same semantic stratum and diagonal used by the terrain renderer.
// referenceZ, when supplied, is render-world height; otherwise underground
// selects the lowest support and surface selects the highest support. Neither
// choice depends on the camera or player. Missing support remains NaN.
function sampleEntitySupportRenderZ(wx, wy, referenceZ, underground, requireTriangle) {
  var mesh = floorMesh;
  if (!mesh || !mesh.layerCount) return 0;
  var gs = mesh.gridSize, gx = Math.floor(wx / gs), gy = Math.floor(wy / gs);
  if (gx < 0 || gy < 0 || gx >= mesh.w || gy >= mesh.h) return NaN;
  var ci = gy * mesh.w + gx, li = -1, z = 0, score = Infinity;
  for (var k = 0; k < mesh.layerCount[ci]; k++) {
    var type = mesh['l' + k + 'Type'][ci], h = mesh['l' + k + 'TopZ'][ci];
    if ((type !== 1 && type !== 3 && type !== 4) || !Number.isFinite(h)) continue;
    var next = Number.isFinite(referenceZ) ? Math.abs(h * 25 - referenceZ) : underground ? h : -h;
    if (next < score) { score = next; li = k; z = h; }
  }
  if (li < 0) return NaN;
  if (gx >= mesh.w - 1 || gy >= mesh.h - 1) return requireTriangle ? NaN : z * 25;
  var role = getFloorRenderLayerRole(mesh, ci, li, mesh['l' + li + 'Type'][ci]);
  var z1 = findFloorRenderRoleZ(mesh, ci + 1, role, z, ci);
  var z2 = findFloorRenderRoleZ(mesh, ci + mesh.w, role, z, ci);
  var z3 = findFloorRenderRoleZ(mesh, ci + mesh.w + 1, role, z, ci);
  // Keep a real cell's support at a stratum edge; do not bridge a missing roof
  // or manufacture zero height. Decal callers can reject missing triangles.
  if (!Number.isFinite(z1) || !Number.isFinite(z2) || !Number.isFinite(z3)) return requireTriangle ? NaN : z * 25;
  z1 = Math.max(z - 4.5, Math.min(z + 4.5, z1));
  z2 = Math.max(z - 4.5, Math.min(z + 4.5, z2));
  z3 = Math.max(z - 4.5, Math.min(z + 4.5, z3));
  var tx = wx / gs - gx, ty = wy / gs - gy;
  return 25 * (tx >= ty ? z + tx * (z1 - z) + ty * (z3 - z1) :
    z + ty * (z2 - z) + tx * (z3 - z2));
}

function getEntityGroundRenderZ(wx, wy, underground) {
  return sampleEntitySupportRenderZ(wx, wy, NaN, !!underground);
}

function getEntityRenderFloorZ(entity) {
  if (Number.isFinite(entity.renderFloorZ)) return entity.renderFloorZ;
  return getEntityGroundRenderZ(entity.x, entity.y, !!(entity.underground || entity.caveSpawnId));
}

// Input is a convex polygon already projected/clipped at the near plane.
function _sceneDepthValidPolygon(points) {
  if (!_sceneDepthW || !_sceneDepthH || !points || points.length < 3) return false;
  for (var i = 0; i < points.length; i++) {
    var p = points[i];
    if (!p || !Number.isFinite(p.x) || !Number.isFinite(p.y) ||
        !Number.isFinite(p.depth) || p.depth <= 0) return false;
  }
  return true;
}

// mode 0 writes opaque depth; mode 1 builds the visible pixel mask. Fan
// triangles share their edge pixels safely: depth uses max, masks use stamps.
function _sceneDepthTriangle(a, b, c, mode, bias) {
  var area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if (Math.abs(area) < 1e-9) return;
  if (area < 0) { var swap = b; b = c; c = swap; area = -area; }
  var x0 = Math.max(0, Math.ceil(Math.min(a.x, b.x, c.x) - 0.5));
  var x1 = Math.min(_sceneDepthW - 1, Math.floor(Math.max(a.x, b.x, c.x) - 0.5));
  var y0 = Math.max(0, Math.ceil(Math.min(a.y, b.y, c.y) - 0.5));
  var y1 = Math.min(_sceneDepthH - 1, Math.floor(Math.max(a.y, b.y, c.y) - 0.5));
  if (x0 > x1 || y0 > y1) return;
  var ax = b.y - c.y, ay = c.x - b.x;
  var bx = c.y - a.y, by = a.x - c.x;
  var cx = a.y - b.y, cy = b.x - a.x;
  var px = x0 + 0.5, py = y0 + 0.5;
  var rowA = ax * (px - b.x) + ay * (py - b.y);
  var rowB = bx * (px - c.x) + by * (py - c.y);
  var rowC = cx * (px - a.x) + cy * (py - a.y);
  var za = 1 / (a.depth * area), zb = 1 / (b.depth * area), zc = 1 / (c.depth * area);
  var dzx = ax * za + bx * zb + cx * zc;
  var dzy = ay * za + by * zb + cy * zc;
  var rowZ = rowA * za + rowB * zb + rowC * zc;
  var edgeTolerance = -area * 1e-10;
  for (var y = y0; y <= y1; y++) {
    // Intersect the three edge half-planes once per scanline. The inner
    // pixel loop visits only the triangle's covered span, not its bounding
    // rectangle, and needs no repeated edge/barycentric tests.
    var lo = 0, hi = x1 - x0;
    if (ax > 0) lo = Math.max(lo, Math.ceil((edgeTolerance - rowA) / ax));
    else if (ax < 0) hi = Math.min(hi, Math.floor((edgeTolerance - rowA) / ax));
    else if (rowA < edgeTolerance) hi = -1;
    if (bx > 0) lo = Math.max(lo, Math.ceil((edgeTolerance - rowB) / bx));
    else if (bx < 0) hi = Math.min(hi, Math.floor((edgeTolerance - rowB) / bx));
    else if (rowB < edgeTolerance) hi = -1;
    if (cx > 0) lo = Math.max(lo, Math.ceil((edgeTolerance - rowC) / cx));
    else if (cx < 0) hi = Math.min(hi, Math.floor((edgeTolerance - rowC) / cx));
    else if (rowC < edgeTolerance) hi = -1;
    var rowVisibleMin = x1 + 1, rowVisibleMax = -1;
    var startX = x0 + lo, endX = x0 + hi, iz = rowZ + lo * dzx;
    var index = y * _sceneDepthW + startX;
    for (var x = startX; x <= endX; x++, index++, iz += dzx) {
      if (iz <= 0) continue;
      var existing = _sceneDepthInv[index];
      if (!mode) {
        if (iz > existing) _sceneDepthInv[index] = iz;
      } else if (iz * (1 + existing * bias) + 1e-9 >= existing) {
        if (_sceneDepthClipMarks[index] !== _sceneDepthClipGeneration) {
          _sceneDepthClipMarks[index] = _sceneDepthClipGeneration;
          _sceneDepthClipInv[index] = iz;
          _sceneDepthClipCount++;
        } else if (iz > _sceneDepthClipInv[index]) {
          _sceneDepthClipInv[index] = iz;
        }
        if (x < rowVisibleMin) rowVisibleMin = x;
        rowVisibleMax = x;
      } else {
        _sceneDepthClipHidden++;
      }
    }
    if (rowVisibleMax >= rowVisibleMin) {
      if (_sceneDepthClipRowMarks[y] !== _sceneDepthClipGeneration) {
        _sceneDepthClipRowMarks[y] = _sceneDepthClipGeneration;
        _sceneDepthClipRowMin[y] = rowVisibleMin;
        _sceneDepthClipRowMax[y] = rowVisibleMax;
      } else {
        if (rowVisibleMin < _sceneDepthClipRowMin[y]) _sceneDepthClipRowMin[y] = rowVisibleMin;
        if (rowVisibleMax > _sceneDepthClipRowMax[y]) _sceneDepthClipRowMax[y] = rowVisibleMax;
      }
      if (rowVisibleMin < _sceneDepthClipMinX) _sceneDepthClipMinX = rowVisibleMin;
      if (rowVisibleMax > _sceneDepthClipMaxX) _sceneDepthClipMaxX = rowVisibleMax;
      if (y < _sceneDepthClipMinY) _sceneDepthClipMinY = y;
      if (y > _sceneDepthClipMaxY) _sceneDepthClipMaxY = y;
    }
    rowA += ay; rowB += by; rowC += cy; rowZ += dzy;
  }
}

function writeSceneDepthPolygon(points) {
  if (!_sceneDepthValidPolygon(points)) return false;
  for (var i = 1; i + 1 < points.length; i++) {
    _sceneDepthTriangle(points[0], points[i], points[i + 1], 0, 0);
  }
  return true;
}

// Trace an opaque fill's exact integer pixel mask, coalescing identical spans
// on adjacent rows. A tall wall needs one rectangle instead of one per scanline;
// holes and changing edges retain their original coverage. Five scalar pending
// span fields avoid per-row allocations or another canvas-sized scratch buffer.
function _traceSceneDepthPixelMask(generation, minY, maxY) {
  var marks = _sceneDepthClipMarks, rowMarks = _sceneDepthClipRowMarks;
  var rowMin = _sceneDepthClipRowMin, rowMax = _sceneDepthClipRowMax;
  var pendingX = 0, pendingY = 0, pendingW = 0, pendingH = 0;
  ctx.beginPath();
  for (var y = minY; y <= maxY; y++) {
    if (rowMarks[y] !== generation) continue;
    var row = y * _sceneDepthW, x = rowMin[y], end = rowMax[y];
    while (x <= end) {
      while (x <= end && marks[row + x] !== generation) x++;
      var start = x;
      while (x <= end && marks[row + x] === generation) x++;
      if (x > start) {
        var width = x - start;
        if (pendingH && pendingX === start && pendingW === width && pendingY + pendingH === y) {
          pendingH++;
        } else {
          if (pendingH) ctx.rect(pendingX, pendingY, pendingW, pendingH);
          pendingX = start; pendingY = y; pendingW = width; pendingH = 1;
        }
      }
    }
  }
  if (pendingH) ctx.rect(pendingX, pendingY, pendingW, pendingH);
}

// Draw once through a pixel-run clip only when partially obscured. Fully
// visible faces keep the cheap ordinary Canvas path; fully hidden ones do
// not invoke the callback. Opaque callers opt into writing their visible
// depth. Transparent effects should leave writeDepth false.
function withSceneDepthClip(points, drawCallback, options) {
  if (!_sceneDepthValidPolygon(points) || typeof drawCallback !== 'function') return 0;
  options = options || {};
  var bias = Number.isFinite(options.depthBias) ? Math.max(0, options.depthBias) : 0.15;
  _sceneDepthClipGeneration = (_sceneDepthClipGeneration + 1) >>> 0;
  if (!_sceneDepthClipGeneration) {
    _sceneDepthClipMarks.fill(0);
    _sceneDepthClipRowMarks.fill(0);
    _sceneDepthClipGeneration = 1;
  }
  _sceneDepthClipCount = _sceneDepthClipHidden = 0;
  _sceneDepthClipMinX = _sceneDepthW; _sceneDepthClipMaxX = -1;
  _sceneDepthClipMinY = _sceneDepthH; _sceneDepthClipMaxY = -1;
  for (var i = 1; i + 1 < points.length; i++) {
    _sceneDepthTriangle(points[0], points[i], points[i + 1], 1, bias);
  }
  // Extended effects can union endpoint glow caps with a depth-varying core
  // in one mask/callback, without stretching its depth or nesting clip calls.
  if (options.extraPolygons) {
    for (var pi = 0; pi < options.extraPolygons.length; pi++) {
      var extra = options.extraPolygons[pi];
      if (!_sceneDepthValidPolygon(extra)) continue;
      for (var ti = 1; ti + 1 < extra.length; ti++) {
        _sceneDepthTriangle(extra[0], extra[ti], extra[ti + 1], 1, bias);
      }
    }
  }
  var visibleCount = _sceneDepthClipCount;
  if (!visibleCount) return 0;
  // Capture mask metadata before calling drawing code. The callback should
  // only draw this face; nested withSceneDepthClip calls are not supported.
  var generation = _sceneDepthClipGeneration;
  var minY = _sceneDepthClipMinY, maxY = _sceneDepthClipMaxY;
  // The opaque helper already paints the exact visible-pixel mask. A second
  // Canvas clip would add work without changing its coverage. Ordinary
  // callbacks retain the partial-visibility clip below.
  if (!_sceneDepthClipHidden || options._paintsDepthPixels) {
    drawCallback();
  } else {
    ctx.save();
    try {
      // Preserve the original clipping path, including its one-pixel rows.
      // Native Canvas can round antialiased sprite/glow edges differently when
      // an equivalent clip path is coalesced, even under an identity transform.
      // Opaque mask fills above are bit-identical with coalescing; clips are not.
      ctx.beginPath();
      for (var y = minY; y <= maxY; y++) {
        if (_sceneDepthClipRowMarks[y] !== generation) continue;
        var row = y * _sceneDepthW, x = _sceneDepthClipRowMin[y], rowEnd = _sceneDepthClipRowMax[y];
        while (x <= rowEnd) {
          while (x <= rowEnd && _sceneDepthClipMarks[row + x] !== generation) x++;
          var start = x;
          while (x <= rowEnd && _sceneDepthClipMarks[row + x] === generation) x++;
          if (x > start) ctx.rect(start, y, x - start, 1);
        }
      }
      ctx.clip();
      drawCallback();
    } finally {
      ctx.restore();
    }
  }
  if (options.writeDepth) {
    for (var wy = minY; wy <= maxY; wy++) {
      if (_sceneDepthClipRowMarks[wy] !== generation) continue;
      var rowMin = _sceneDepthClipRowMin[wy], rowMax = _sceneDepthClipRowMax[wy];
      var index = wy * _sceneDepthW + rowMin;
      for (var wx = rowMin; wx <= rowMax; wx++, index++) {
        if (_sceneDepthClipMarks[index] === generation && _sceneDepthClipInv[index] > _sceneDepthInv[index]) {
          _sceneDepthInv[index] = _sceneDepthClipInv[index];
        }
      }
    }
  }
  return visibleCount;
}

function traceSceneDepthPolygon(points) {
  if (!points || points.length < 3) return false;
  ctx.beginPath();
  ctx.moveTo(points[0].x, points[0].y);
  for (var i = 1; i < points.length; i++) ctx.lineTo(points[i].x, points[i].y);
  ctx.closePath();
  return true;
}

// Keep the caller's material/alpha; this helper only supplies shared opaque
// visibility. Transparent geometry must use withSceneDepthClip directly.
function fillSceneDepthPolygon(points) {
  return withSceneDepthClip(points, function () {
    // Canvas antialiases separate triangle edges, but depth is pixel-center
    // sampled. Filling the original path could leave half-painted cracks
    // whose depth was already opaque. Paint precisely the rasterized mask
    // with integer-aligned spans, using the caller's unchanged material.
    // Adjacent terrain triangles now agree on both color and depth coverage.
    _traceSceneDepthPixelMask(_sceneDepthClipGeneration, _sceneDepthClipMinY, _sceneDepthClipMaxY);
    ctx.fill();
  }, {writeDepth: true, _paintsDepthPixels: true});
}

// Clip in camera space before projection, so a face crossing the near plane
// stays a visible polygon instead of disappearing because one corner is
// behind the camera. z is the renderer's world height, not player floorZ.
function projectSceneWorldPolygon(vertices, C) {
  if (!vertices || vertices.length < 3 || !C) return [];
  var cameraPoints = [];
  for (var i = 0; i < vertices.length; i++) {
    var p = vertices[i];
    if (!p || !Number.isFinite(p.x) || !Number.isFinite(p.y) || !Number.isFinite(p.z)) return [];
    var dx = p.x - cam.x, dy = p.y - cam.y;
    cameraPoints.push({depth: dx * C.cosAng + dy * C.sinAng,
      right: -dx * C.sinAng + dy * C.cosAng, z: p.z});
  }
  var clipped = [], previous = cameraPoints[cameraPoints.length - 1];
  for (var j = 0; j < cameraPoints.length; j++) {
    var current = cameraPoints[j];
    if ((previous.depth >= 1) !== (current.depth >= 1)) {
      var t = (1 - previous.depth) / (current.depth - previous.depth);
      clipped.push({depth: 1, right: previous.right + t * (current.right - previous.right),
        z: previous.z + t * (current.z - previous.z)});
    }
    if (current.depth >= 1) clipped.push(current);
    previous = current;
  }
  var projected = [];
  for (var k = 0; k < clipped.length; k++) {
    var v = clipped[k];
    projected.push({x: (v.right / v.depth * C.invTanHalf * 0.5 + 0.5) * C.w,
      y: C.horizonY + (C.cameraZ - v.z) / v.depth * projScale, depth: v.depth});
  }
  return projected;
}
