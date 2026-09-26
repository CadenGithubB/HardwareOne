// =============================================
// SECTION 13: RENDERING - FLOOR
// =============================================

function drawPlatforms2D() {
  if (!floorMesh) return;
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  var mesh = floorMesh;
  var gridSize = mesh.gridSize;
  var now2d = Date.now();
  for (var y = 0; y < mesh.h - 1; y++) {
    for (var x = 0; x < mesh.w - 1; x++) {
      var height = mesh.heights[y * mesh.w + x];
      var worldX = x * gridSize;
      var worldY = y * gridSize;
      ctx.fillStyle = mesh.colors[y * mesh.w + x];
      ctx.globalAlpha = 0.4 + height * 0.5;
      ctx.fillRect(worldX, worldY, gridSize, gridSize);
      // Water overlay in 2D — streams shimmer, pools are near-static
      var wt2d = mesh.water ? mesh.water[y * mesh.w + x] : 0;
      if (wt2d === 2) {
        var ripple2d = Math.sin(now2d * 0.003 + x * 0.3 + y * 0.3);
        ctx.globalAlpha = 0.30 + 0.15 * ripple2d;
        ctx.fillStyle = '#55aaee';
        ctx.fillRect(worldX, worldY, gridSize, gridSize);
      } else if (wt2d === 1) {
        ctx.globalAlpha = 0.18;
        ctx.fillStyle = '#3388bb';
        ctx.fillRect(worldX, worldY, gridSize, gridSize);
      }
    }
  }
  ctx.globalAlpha = 1.0;
  ctx.restore();
}

// =============================================
// SECTION 13c: RENDERING - LAYERED FLOOR
// =============================================
// Reads the layered height field directly. Floor quads stitch by semantic
// stratum rather than layer index or nearest arbitrary walkable height, which
// keeps surface, cave floor, cap and ledge geometry distinct at cave mouths.
// The pass includes painter ordering, spatial/FOV culling, authored layer
// colors, local lighting/AO, cave fog, and steep-ground skirts.

// Renderer-facing semantic helpers are deliberately top-level so focused
// fixtures can exercise the exact stitch contract without running Canvas.
function getFloorRenderLayerRole(mesh, ci, li, type) {
  var meta = 0;
  if (li === 0) meta = mesh.l0Meta ? mesh.l0Meta[ci] : 0;
  else if (li === 1) meta = mesh.l1Meta ? mesh.l1Meta[ci] : 0;
  else if (li === 2) meta = mesh.l2Meta ? mesh.l2Meta[ci] : 0;
  else if (li === 3) meta = mesh.l3Meta ? mesh.l3Meta[ci] : 0;
  else meta = mesh.l4Meta ? mesh.l4Meta[ci] : 0;
  var role = meta & 0x7f;
  if (role) return role;
  // Fixed/legacy meshes may not carry metadata. Keep a conservative fallback
  // that still separates caps and ledges from ordinary ground.
  if (type === 4) return 3;
  if (type === 3) return 4;
  if (type === 1) {
    return mesh.ceilAboveMask && ((mesh.ceilAboveMask[ci] >> li) & 1) ? 2 : 1;
  }
  return 0;
}

function findFloorRenderRoleZ(mesh, ci, targetRole, targetZ, sourceCi) {
  var lc = mesh.layerCount[ci];
  if (!lc) return NaN;
  var bestZ = 0, bestDiff = 1e9, exteriorZ = 0, exteriorDiff = 1e9;
  var mouthZ = 0, mouthDiff = 1e9;
  var sourceSurface = mesh.surfaceH && sourceCi >= 0 ? mesh.surfaceH[sourceCi] : NaN;
  for (var li = 0; li < lc; li++) {
    var type, z;
    if (li === 0) { type = mesh.l0Type[ci]; z = mesh.l0TopZ[ci]; }
    else if (li === 1) { type = mesh.l1Type[ci]; z = mesh.l1TopZ[ci]; }
    else if (li === 2) { type = mesh.l2Type[ci]; z = mesh.l2TopZ[ci]; }
    else if (li === 3) { type = mesh.l3Type[ci]; z = mesh.l3TopZ[ci]; }
    else { type = mesh.l4Type[ci]; z = mesh.l4TopZ[ci]; }
    if (type !== 1 && type !== 3 && type !== 4) continue;
    var role = getFloorRenderLayerRole(mesh, ci, li, type);
    var diff = z > targetZ ? z - targetZ : targetZ - z;
    if (role === targetRole) {
      if (diff < bestDiff) { bestDiff = diff; bestZ = z; }
      continue;
    }
    // Surface and cap are two storage roles for one exterior terrain skin.
    // Join them only when both samples still sit on their natural surface.
    // The depressed approach fails this test, so it cannot pull the cap down
    // into a green spike at the mouth.
    if ((targetRole === 1 && role === 3) || (targetRole === 3 && role === 1)) {
      var neighborSurface = mesh.surfaceH ? mesh.surfaceH[ci] : NaN;
      if (sourceSurface === sourceSurface && neighborSurface === neighborSurface &&
          Math.abs(targetZ - sourceSurface) <= 0.6 &&
          Math.abs(z - neighborSurface) <= 0.6 && diff <= 1.5 &&
          diff < exteriorDiff) {
        exteriorDiff = diff;
        exteriorZ = z;
      }
    }
    // Open approach (surface role) and covered tunnel floor (cave-floor role)
    // are physically one support at the portal plane. Preserve that seam only
    // while it is within the movement step contract; deeper/lateral strata
    // remain separate instead of being stretched into a sloped quad.
    if (((targetRole === 1 && role === 2) || (targetRole === 2 && role === 1)) &&
        diff <= 1.0 && diff < mouthDiff) {
      mouthDiff = diff;
      mouthZ = z;
    }
  }
  return bestDiff < 1e9 ? bestZ :
    exteriorDiff < 1e9 ? exteriorZ : mouthDiff < 1e9 ? mouthZ : NaN;
}

// Static corner results, not camera transforms/materials/lighting. Tiles make
// first use incremental; a fixed byte cap bounds memory even on huge meshes.
var FLOOR_STITCH_TILE_CELLS = 256;
var FLOOR_STITCH_CACHE_BYTES = 4 * 1024 * 1024;

function getFloorStitchCache(mesh) {
  var cache = mesh._floorRenderStitches;
  // buildWalkCandZ is the existing geometry-finalization boundary. It replaces
  // walkCandZ on every rebuild, including rebuilding a reused mesh object.
  // Future in-place height/type/meta changes must rebuild that derived data
  // or delete _floorRenderStitches before another render.
  if (!cache || cache.stamp !== mesh.walkCandZ || cache.w !== mesh.w || cache.h !== mesh.h) {
    cache = mesh._floorRenderStitches = {stamp:mesh.walkCandZ,w:mesh.w,h:mesh.h,
      floors:[],ceilings:[],bytes:0,hits:0,misses:0,fallbacks:0,
      scratch:{values:new Float64Array(FLOOR_STITCH_TILE_CELLS*3)}};
  }
  return cache;
}

function findCeilingRenderMatchZ(mesh, ci, targetZ) {
  var lc = mesh.layerCount[ci], bestZ = 0, bestDiff = 1e9;
  for (var li = 0; li < lc; li++) {
    var type, z;
    if (li === 0) { type=mesh.l0Type[ci]; z=mesh.l0TopZ[ci]; }
    else if (li === 1) { type=mesh.l1Type[ci]; z=mesh.l1TopZ[ci]; }
    else if (li === 2) { type=mesh.l2Type[ci]; z=mesh.l2TopZ[ci]; }
    else if (li === 3) { type=mesh.l3Type[ci]; z=mesh.l3TopZ[ci]; }
    else { type=mesh.l4Type[ci]; z=mesh.l4TopZ[ci]; }
    if (type !== 2) continue;
    var diff = z > targetZ ? z-targetZ : targetZ-z;
    if (diff < bestDiff) {bestDiff=diff;bestZ=z;}
  }
  return bestDiff <= 0.8 ? bestZ : NaN;
}

function getFloorStitchTile(mesh, cache, ci, li, role, z, ceiling) {
  var layers = ceiling ? cache.ceilings : cache.floors;
  var tiles = layers[li] || (layers[li]=[]), tileIndex=ci>>>8;
  var tile = tiles[tileIndex], slot=ci&255, offset=slot*3;
  if (tile && tile.ready[slot]) {
    cache.hits++;
    if (!ceiling) _cacheStats.matchZ.hits+=3;
    return tile;
  }
  if (!tile) {
    var tileBytes=FLOOR_STITCH_TILE_CELLS*(3*8+1);
    if (cache.bytes+tileBytes <= FLOOR_STITCH_CACHE_BYTES) {
      tile=tiles[tileIndex]={values:new Float64Array(FLOOR_STITCH_TILE_CELLS*3),ready:new Uint8Array(FLOOR_STITCH_TILE_CELLS)};
      cache.bytes+=tileBytes;
    } else {
      tile=cache.scratch;cache.fallbacks++;
    }
  }
  var values=tile.values,mw=mesh.w;
  if (ceiling) {
    values[offset]=findCeilingRenderMatchZ(mesh,ci+1,z);
    values[offset+1]=findCeilingRenderMatchZ(mesh,ci+mw,z);
    values[offset+2]=findCeilingRenderMatchZ(mesh,ci+mw+1,z);
  } else {
    values[offset]=findFloorRenderRoleZ(mesh,ci+1,role,z,ci);
    values[offset+1]=findFloorRenderRoleZ(mesh,ci+mw,role,z,ci);
    values[offset+2]=findFloorRenderRoleZ(mesh,ci+mw+1,role,z,ci);
    _cacheStats.matchZ.misses+=3;
  }
  if(tile.ready)tile.ready[slot]=1;
  cache.misses++;
  return tile;
}

// Signed vertical distance from the eye to the triangle's actual plane.
// Comparing only corner heights is not back-face culling: an uphill bank can
// face an eye below every corner, while a downhill back face can face away
// from an eye above them. Test each triangle because a cell need not be planar.
// Heights here are already render-world units, matching cameraZ/projection.
function getTerrainTriangleEyeSide(a, b, c, eyeX, eyeY, eyeZ) {
  var ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
  var vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
  var nz = ux * vy - uy * vx;
  if (!nz) return NaN;
  return eyeZ - a.z + ((uy * vz - uz * vy) * (eyeX - a.x) +
    (uz * vx - ux * vz) * (eyeY - a.y)) / nz;
}

// Same endpoint closure as the wall pass, in mesh-height units. This is a
// static layer lookup, not a camera or player-stratum visibility decision.
function floorWallAOCeilingAt(wx, wy, topH) {
  if (deepCaveRegions.length > 0) {
    var region = isInDeepCave(wx, wy);
    if (region && region.ceilZ > 0) {
      var depth = region.depth || 1.0;
      return Math.max(topH, topH + (region.ceilZ / 25 - topH) * Math.min(1, depth * depth * 2.5));
    }
  }
  var mesh = floorMesh;
  if (!mesh || !mesh.layerCount) return topH;
  var mx = Math.floor(wx / mesh.gridSize), my = Math.floor(wy / mesh.gridSize);
  if (mx < 0 || my < 0 || mx >= mesh.w || my >= mesh.h) return topH;
  var mi = my * mesh.w + mx, ceiling = Infinity;
  for (var li = 0; li < mesh.layerCount[mi]; li++) {
    if (meshLayerType(mesh, mi, li) === 2) ceiling = Math.min(ceiling, meshLayerHeight(mesh, mi, li));
  }
  return ceiling < Infinity ? Math.max(topH, ceiling) : topH;
}

// Contact AO belongs to the floor edge beside the actual wall span. A 2D
// occupancy bit alone also describes buried cave walls, which must not draw
// a dark map of the cave through the grass above. Face indices match the wall
// renderer (W/E/N/S); edge heights run top-to-bottom or left-to-right.
function floorWallOccludesAO(gx, gy, face, edgeZ1, edgeZ2) {
  if (!grid || gx < 0 || gy < 0 || gx >= gridW || gy >= gridH) return false;
  var wi = gy * gridW + gx;
  if (!grid[wi]) return false;
  var edgeMin = Math.min(edgeZ1, edgeZ2), edgeMax = Math.max(edgeZ1, edgeZ2);
  var limit = wallMaxTopZ ? wallMaxTopZ[wi] : Infinity;
  var x1 = (gx + (face === 1 ? 1 : 0)) * cell;
  var y1 = (gy + (face === 3 ? 1 : 0)) * cell;
  var x2 = x1 + (face >= 2 ? cell : 0), y2 = y1 + (face < 2 ? cell : 0);
  // Buried walls usually reject using just the stored roof bound and its two
  // ceiling endpoints, without a floor-height query. A sloped closure can
  // still rise past that bound, so the bound alone is not an occluder test.
  if (limit <= edgeMin + 0.001 &&
      floorWallAOCeilingAt(x1, y1, limit) <= edgeZ1 + 0.001 &&
      floorWallAOCeilingAt(x2, y2, limit) <= edgeZ2 + 0.001) return false;
  var floorH = floorMesh ? getFloorHeightAt((gx + 0.5) * cell, (gy + 0.5) * cell) : 0;
  var topH = Math.min(floorH + (CANVAS_BASE_H / 25) * (wallHeights ? wallHeights[wi] : 1), limit);
  var baseH = (wallFaceBase ? wallFaceBase[wi * 4 + face] : floorH) - 0.15;
  if (baseH > edgeMax + 0.001) return false;
  if (topH > edgeMin + 0.001 && topH > baseH) return true;
  var top1 = floorWallAOCeilingAt(x1, y1, topH);
  var top2 = floorWallAOCeilingAt(x2, y2, topH);
  // Clip the floor-edge interval to the wall base, then test the two ends of
  // that interval. Linear roof/floor edges cannot cross elsewhere unnoticed.
  var t1 = 0, t2 = 1, dz = edgeZ2 - edgeZ1;
  if (edgeZ1 < baseH && dz > 0) t1 = (baseH - edgeZ1) / dz;
  if (edgeZ2 < baseH && dz < 0) t2 = (baseH - edgeZ1) / dz;
  var above1 = top1 - edgeZ1, aboveDelta = top2 - edgeZ2 - above1;
  return above1 + aboveDelta * t1 > 0.001 || above1 + aboveDelta * t2 > 0.001;
}

function drawLayersFloor3D() {
  if (!floorMesh || !floorMesh.layerCount) return;
  var C = getCam3D();
  var w = C.w, h = C.h, cosAng = C.cosAng, sinAng = C.sinAng;
  var invTanHalf = C.invTanHalf, horizonY = C.horizonY, cameraZ = C.cameraZ;
  var mesh = floorMesh;
  var gs = mesh.gridSize;
  var tanHalf = 1 / invTanHalf;
  var stitchCache = getFloorStitchCache(mesh);
  // Reset debug stats for this frame
  __caveStats.floorTotal = 0;
  __caveStats.floorDistCull = 0;
  __caveStats.floorFovCull = 0;
  __caveStats.floorBehind = 0;
  __caveStats.floorBlendRange = 0;  // repurposed: layer-stitch successes
  __caveStats.floorOutsideBlend = 0; // repurposed: layer-stitch misses
  ctx.save();

  // Ground plane fill (solid dark below horizon — matches legacy behavior)
  if (horizonY < h) {
    var _gr = Math.floor(12 * ambientLight);
    var _gg = Math.floor(11 * ambientLight);
    var _gb = Math.floor(10 * ambientLight);
    ctx.fillStyle = rgbQ(_gr, _gg, _gb);
    var groundY = Math.max(0, Math.floor(horizonY) - 2);
    ctx.fillRect(0, groundY, w, h - groundY);
  }

  var camGX = cam.x / gs, camGY = cam.y / gs;
  var viewRad = viewDist / gs;
  var gxMin = Math.max(0, Math.floor(camGX - viewRad));
  var gxMax = Math.min(mesh.w - 2, Math.ceil(camGX + viewRad));
  var gyMin = Math.max(0, Math.floor(camGY - viewRad));
  var gyMax = Math.min(mesh.h - 2, Math.ceil(camGY + viewRad));

  var mw = mesh.w;
  var viewDistSq = viewDist * viewDist;
  var fovCutTan = tanHalf * 2.2;

  // Lookup: (cellIdx, layerIdx) → [topZ, type] or null if absent.
  function layerAt(ci, li) {
    if (li >= mesh.layerCount[ci]) return null;
    var t, z;
    if (li === 0) { t = mesh.l0Type[ci]; z = mesh.l0TopZ[ci]; }
    else if (li === 1) { t = mesh.l1Type[ci]; z = mesh.l1TopZ[ci]; }
    else if (li === 2) { t = mesh.l2Type[ci]; z = mesh.l2TopZ[ci]; }
    else if (li === 3) { t = mesh.l3Type[ci]; z = mesh.l3TopZ[ci]; }
    else { t = mesh.l4Type[ci]; z = mesh.l4TopZ[ci]; }
    return [z, t];
  }
  // Layer identity is semantic, not just "some walkable height nearby".
  // The mouth can have cave floor, open surface and roof cap in adjacent
  // cells. Pairing those roles by nearest Z makes the cap dive toward the
  // tunnel floor and produces the large terrain triangles seen through the
  // opening. lXMeta is authored after the final layer sort and therefore is
  // the authoritative role for stitching.
  // Return the closest height for the SAME semantic role. A role may move to
  // another layer index after height sorting, so matching the index is also
  // incorrect. Height clamping happens only after this identity check.

  // Diagnostic: how many cave-underground quads passed the cull this frame?
  window.__caveFloorRendered = 0;

  // Distance-sorted iteration: collect surviving cells, sort far→near, then
  // render. Eliminates the painter's-order flip that happened when sinAng
  // or cosAng changed sign (camera rotating past axis boundaries) —
  // previously caused overlapping quads to swap paint order, producing
  // the pop-on-turn artifact at cave boundaries.
  var _cellBuf = _drawFloorCellBuf;
  var _cellCnt = 0;
  for (var y = gyMin; y <= gyMax; y++) {
    for (var x = gxMin; x <= gxMax; x++) {
      __caveStats.floorTotal++;
      var centerX = (x + 0.5) * gs, centerY = (y + 0.5) * gs;
      var ddx = centerX - cam.x, ddy = centerY - cam.y;
      var distSq = ddx * ddx + ddy * ddy;
      if (distSq > viewDistSq) { __caveStats.floorDistCull++; continue; }
      // "Under-foot skirt": cells within a generous radius always render,
      // bypassing behind/FOV culls that ignore pitch. The radius grows when
      // pitched down/up so the visible ground in steep views is kept.
      var _pa = Math.abs(cam.pitch || 0);
      var _nearR = 80 + _pa * 300;
      var NEAR_SQ = _nearR * _nearR;
      if (distSq > NEAR_SQ) {
        var fwdDot = ddx * cosAng + ddy * sinAng;
        if (fwdDot < 0) { __caveStats.floorBehind++; continue; }
        var rgtDot = ddx * (-sinAng) + ddy * cosAng;
        if (Math.abs(rgtDot) / (fwdDot + 0.001) > fovCutTan) { __caveStats.floorFovCull++; continue; }
      }
      // Pack [distSq, x, y] into a flat buffer; sort as triples below.
      _cellBuf[_cellCnt * 3]     = distSq;
      _cellBuf[_cellCnt * 3 + 1] = x;
      _cellBuf[_cellCnt * 3 + 2] = y;
      _cellCnt++;
    }
  }
  // Sort triples by distSq descending (far first). In-place comb sort.
  if (_cellCnt > 1) {
    var _cgap = _cellCnt, _cswp = true;
    while (_cgap > 1 || _cswp) {
      _cgap = (_cgap * 10 / 13) | 0;
      if (_cgap < 1) _cgap = 1;
      _cswp = false;
      for (var _ci = 0; _ci + _cgap < _cellCnt; _ci++) {
        var _a = _ci * 3, _b = (_ci + _cgap) * 3;
        if (_cellBuf[_a] < _cellBuf[_b]) {
          var _td = _cellBuf[_a], _tx = _cellBuf[_a + 1], _ty = _cellBuf[_a + 2];
          _cellBuf[_a] = _cellBuf[_b]; _cellBuf[_a + 1] = _cellBuf[_b + 1]; _cellBuf[_a + 2] = _cellBuf[_b + 2];
          _cellBuf[_b] = _td;          _cellBuf[_b + 1] = _tx;               _cellBuf[_b + 2] = _ty;
          _cswp = true;
        }
      }
    }
  }

  for (var _cellI = 0; _cellI < _cellCnt; _cellI++) {
    var x = _cellBuf[_cellI * 3 + 1];
    var y = _cellBuf[_cellI * 3 + 2];
    var centerX = (x + 0.5) * gs, centerY = (y + 0.5) * gs;
    var ddx = centerX - cam.x, ddy = centerY - cam.y;
    var distSq = ddx * ddx + ddy * ddy;
    var fwdDot = ddx * cosAng + ddy * sinAng;
    var rgtDot = ddx * (-sinAng) + ddy * cosAng;
    {
      var idx0 = y * mw + x, idx1 = idx0 + 1, idx2 = idx0 + mw, idx3 = idx2 + 1;
      var lc0 = mesh.layerCount[idx0];

      for (var li = 0; li < lc0; li++) {
        var L0 = layerAt(idx0, li); if (!L0 || (L0[1] !== 1 && L0[1] !== 3 && L0[1] !== 4)) continue;

        // Per-cell underground + cap-above: O(1) bitmask reads (precomputed at
        // map load by buildWalkCandZ). Bit li set = ceiling/cap layer exists
        // above layer li in this cell.
        var _cellUnderground = (mesh.ceilAboveMask[idx0] >> li) & 1;
        // Visibility is resolved against the actual projected terrain and roof,
        // not a camera/portal mode switch.
        if (_cellUnderground) window.__caveFloorRendered++;

        // A quad may only join four samples of the same semantic stratum.
        // Surface, cave floor, cap and ledge can coexist at the mouth, but a
        // cap must end at the roof edge instead of sloping down to whichever
        // other walkable layer happens to be nearest in the next cell.
        var _tol = 4.5;
        var _z0 = L0[0];
        var _role0 = getFloorRenderLayerRole(mesh, idx0, li, L0[1]);
        var _stitch = getFloorStitchTile(mesh, stitchCache, idx0, li, _role0, _z0, false);
        var _stitchOffset = (idx0 & 255) * 3;
        var _z1 = _stitch.values[_stitchOffset];
        var _z2 = _stitch.values[_stitchOffset+1];
        var _z3 = _stitch.values[_stitchOffset+2];
        if (_z1 !== _z1 || _z2 !== _z2 || _z3 !== _z3) {
          __caveStats.floorOutsideBlend++;
          continue;
        }
        // Preserve solid steep terrain, but only within the already matched
        // role. This can no longer pull a cap toward a cave floor or surface.
        if (_z1 > _z0 + _tol) _z1 = _z0 + _tol;
        else if (_z1 < _z0 - _tol) _z1 = _z0 - _tol;
        if (_z2 > _z0 + _tol) _z2 = _z0 + _tol;
        else if (_z2 < _z0 - _tol) _z2 = _z0 - _tol;
        if (_z3 > _z0 + _tol) _z3 = _z0 + _tol;
        else if (_z3 < _z0 - _tol) _z3 = _z0 - _tol;
        __caveStats.floorBlendRange++;

        var wx1 = x * gs, wy1 = y * gs, wx2 = wx1 + gs, wy2 = wy1 + gs;
        var floorVertices = [
          {x:wx1,y:wy1,z:_z0*25}, {x:wx2,y:wy1,z:_z1*25},
          {x:wx2,y:wy2,z:_z3*25}, {x:wx1,y:wy2,z:_z2*25}
        ];
        // A sloped four-corner cell is not necessarily planar. Triangulate
        // before both facing and projection so visible uphill banks still
        // supply opaque depth while the exiting camera is below the surface.
        // Flat caps retain their actual top/underside distinction; this does
        // not draw a grass lid over the interior or switch by player stratum.
        var floorPoly = getTerrainTriangleEyeSide(floorVertices[0], floorVertices[1], floorVertices[2],
          cam.x, cam.y, cameraZ) >= -0.000001 ?
          projectSceneWorldPolygon([floorVertices[0], floorVertices[1], floorVertices[2]], C) : [];
        var floorPolyB = getTerrainTriangleEyeSide(floorVertices[0], floorVertices[2], floorVertices[3],
          cam.x, cam.y, cameraZ) >= -0.000001 ?
          projectSceneWorldPolygon([floorVertices[0], floorVertices[2], floorVertices[3]], C) : [];
        if (floorPoly.length < 3 && floorPolyB.length < 3) continue;

        // Biome color from mesh.colors[], dimmed by ambient + light grid + AO.
        // Bottom layer uses stored color directly (surface or cave). Higher
        // layers (cap, ledge) reuse the stored color too — they represent
        // the same ground, just lifted up; future: separate color per layer.
        // Per-layer color string, authored at build time. Direct read, no
        // per-frame conversion.
        var baseCol;
        if (li === 0) baseCol = mesh.l0Color[idx0];
        else if (li === 1) baseCol = mesh.l1Color[idx0];
        else if (li === 2) baseCol = mesh.l2Color[idx0];
        else if (li === 3) baseCol = mesh.l3Color[idx0];
        else baseCol = mesh.l4Color[idx0];
        // DEBUG_LAYER_TYPES: richer semantic palette using precomputed lXMeta
        // (role bits). Distinguishes role, not just type number.
        if (DEBUG_LAYER_TYPES) {
          var _lmMeta;
          if (li === 0) _lmMeta = mesh.l0Meta ? mesh.l0Meta[idx0] : 0;
          else if (li === 1) _lmMeta = mesh.l1Meta ? mesh.l1Meta[idx0] : 0;
          else if (li === 2) _lmMeta = mesh.l2Meta ? mesh.l2Meta[idx0] : 0;
          else if (li === 3) _lmMeta = mesh.l3Meta ? mesh.l3Meta[idx0] : 0;
          else _lmMeta = mesh.l4Meta ? mesh.l4Meta[idx0] : 0;
          var _lmRole = _lmMeta & 0x7f; // 1=surface, 2=caveFloor, 3=cap, 4=ledge
          baseCol = _lmRole === 1 ? '#2ecc40'    // bright green  — surface
                  : _lmRole === 2 ? '#ff851b'    // orange         — cave floor
                  : _lmRole === 3 ? '#0074d9'    // blue           — surface cap
                  : _lmRole === 4 ? '#ffdc00'    // yellow         — ledge
                  : '#ff00ff';                    // magenta        — unknown
        }
        // DEBUG_POLY_TYPES: one palette across floor/ceiling/wall render so
        // every polygon in the view is categorized by what it physically is.
        // Floor-quad classifications below:
        //   cap (type 4) → BLUE
        //   cave floor under ceiling → ORANGE
        //   normal surface, flat → GREEN
        //   surface with large corner Z delta (>= 2u) → YELLOW (tilted quad)
        //   ledge → YELLOW (same bucket as tilted)
        if (DEBUG_POLY_TYPES) {
          var _ptMaxDz = Math.max(Math.abs(_z1 - _z0), Math.abs(_z2 - _z0), Math.abs(_z3 - _z0));
          var _ptT = L0[1];
          if (_ptT === 4) baseCol = '#0074d9';        // BLUE cap
          else if (_cellUnderground) baseCol = '#ff851b'; // ORANGE cave floor
          else if (_ptT === 3) baseCol = '#ffdc00';   // YELLOW ledge
          else if (_ptMaxDz >= 2.0) baseCol = '#ffdc00'; // YELLOW tilted floor
          else baseCol = '#2ecc40';                     // GREEN flat surface
        }
        // Light belongs to the rendered cell, not to the player's global
        // state. Covered floors fade smoothly from exterior daylight at the
        // shared portal plane to the readable cave ambient deeper inside.
        var floorAmbient = _cellUnderground && typeof getCaveRenderLightAt === 'function' ?
          getCaveRenderLightAt(centerX, centerY, true) :
          (typeof renderSurfaceAmbient !== 'undefined' ? renderSurfaceAmbient : ambientLight);
        var floorPointLight = 0;
        var _surfacePointLight = !_cellUnderground && typeof getSurfaceFloorLightAt === 'function' ?
          getSurfaceFloorLightAt(mesh, idx0, li) : NaN;
        if (Number.isFinite(_surfacePointLight)) {
          floorPointLight = _surfacePointLight;
        } else if (_lightGrid) {
          var flgx = Math.floor(centerX / _lightCellSize);
          var flgy = Math.floor(centerY / _lightCellSize);
          if (flgx >= 0 && flgx < _lightGridW && flgy >= 0 && flgy < _lightGridH)
            floorPointLight = _lightGrid[flgy * _lightGridW + flgx];
        }
        // AO: only walls reaching this physical floor edge can darken it.
        var floorAO = 1;
        if (grid) {
          var _aoGx = Math.floor(centerX / cell), _aoGy = Math.floor(centerY / cell);
          if (_aoGx >= 0 && _aoGx < gridW && _aoGy >= 0 && _aoGy < gridH) {
            var _aoN = 0;
            if (floorWallOccludesAO(_aoGx - 1, _aoGy, 1, _z0, _z2)) _aoN++;
            if (floorWallOccludesAO(_aoGx + 1, _aoGy, 0, _z1, _z3)) _aoN++;
            if (floorWallOccludesAO(_aoGx, _aoGy - 1, 3, _z0, _z1)) _aoN++;
            if (floorWallOccludesAO(_aoGx, _aoGy + 1, 2, _z2, _z3)) _aoN++;
            if (_aoN > 0) floorAO = 1.0 - 0.08 * _aoN;
          }
        }
        var _fc = parseInt(baseCol.slice(1), 16);
        // Distance fog: underground cells fade toward black (cave depth),
        // surface cells fade via alpha toward whatever is behind (sky/ground).
        var fadeStart = viewDist * 0.72, fadeRange = viewDist - fadeStart;
        var d = Math.sqrt(distSq);
        var fadeF = d > fadeStart ? Math.max(0, 1.0 - (d - fadeStart) / fadeRange) : 1.0;
        fadeF *= fadeF;
        var _fr, _fg, _fb;
        if (_cellUnderground && !DEBUG_LAYER_TYPES && !DEBUG_POLY_TYPES) {
          var floorLit = shadeCaveSurfaceColor(_fc, CAVE_SURFACE_FLOOR,
            floorAmbient, floorPointLight, fadeF, floorAO);
          _fr = (floorLit >>> 16) & 255; _fg = (floorLit >>> 8) & 255; _fb = floorLit & 255;
          ctx.globalAlpha = 1.0;
        } else {
          var floorLight = Math.min(1, floorAmbient + floorPointLight) * floorAO;
          _fr = ((_fc >> 16) & 0xff) * floorLight;
          _fg = ((_fc >> 8) & 0xff) * floorLight;
          _fb = (_fc & 0xff) * floorLight;
          if (_cellUnderground) {
            _fr *= fadeF; _fg *= fadeF; _fb *= fadeF;
            ctx.globalAlpha = 1;
          } else {
            ctx.globalAlpha = fadeF;
          }
        }
        ctx.fillStyle = rgbQ(_fr, _fg, _fb);
        fillSceneDepthPolygon(floorPoly);
        fillSceneDepthPolygon(floorPolyB);

        ctx.globalAlpha = 1.0;
      }
    }
  }
  ctx.restore();
}


// Layer-aware ceiling renderer. Iterates ceiling-type layers (type=2) from
// the layered field and stitches neighboring ceiling samples by height. Where
// ceilings do not match, the edge naturally opens at the entrance mouth.
//
// A roof cap and its underside are two faces of the same rock. The ceiling is
// rendered when the camera is below it; an exterior camera above it sees the
// cap instead. This remains correct while looking back out through the mouth.
function drawLayersCeiling3D() {
  if (!floorMesh || !floorMesh.layerCount) return;
  var C = getCam3D();
  var w = C.w, h = C.h, cosAng = C.cosAng, sinAng = C.sinAng;
  var invTanHalf = C.invTanHalf, horizonY = C.horizonY, cameraZ = C.cameraZ;
  var mesh = floorMesh;
  var gs = mesh.gridSize;
  var tanHalf = 1 / invTanHalf;
  var stitchCache = getFloorStitchCache(mesh);
  // Reset ceiling pipeline stats
  __caveStats.ceilCollected = 0;
  __caveStats.ceilSkipLowCeil = 0;
  __caveStats.ceilSkipEntrRange = 0;
  __caveStats.ceilSkipViewDist = 0;
  __caveStats.ceilSkipBehind = 0;
  __caveStats.ceilSkipFov = 0;
  __caveStats.ceilRendered = 0;
  ctx.save();

  var camGX = cam.x / gs, camGY = cam.y / gs;
  var viewRad = viewDist / gs;
  var gxMin = Math.max(0, Math.floor(camGX - viewRad));
  var gxMax = Math.min(mesh.w - 2, Math.ceil(camGX + viewRad));
  var gyMin = Math.max(0, Math.floor(camGY - viewRad));
  var gyMax = Math.min(mesh.h - 2, Math.ceil(camGY + viewRad));
  var iterYStart, iterYEnd, iterYStep, iterXStart, iterXEnd, iterXStep;
  if (sinAng >= 0) { iterYStart = gyMax; iterYEnd = gyMin - 1; iterYStep = -1; }
  else             { iterYStart = gyMin; iterYEnd = gyMax + 1; iterYStep = 1; }
  if (cosAng >= 0) { iterXStart = gxMax; iterXEnd = gxMin - 1; iterXStep = -1; }
  else             { iterXStart = gxMin; iterXEnd = gxMax + 1; iterXStep = 1; }

  var mw = mesh.w;
  var viewDistSq = viewDist * viewDist;
  var fovCutTan = tanHalf * 2.2;

  function layerAtC(ci, li) {
    if (li >= mesh.layerCount[ci]) return null;
    var t, z;
    if (li === 0) { t = mesh.l0Type[ci]; z = mesh.l0TopZ[ci]; }
    else if (li === 1) { t = mesh.l1Type[ci]; z = mesh.l1TopZ[ci]; }
    else if (li === 2) { t = mesh.l2Type[ci]; z = mesh.l2TopZ[ci]; }
    else if (li === 3) { t = mesh.l3Type[ci]; z = mesh.l3TopZ[ci]; }
    else { t = mesh.l4Type[ci]; z = mesh.l4TopZ[ci]; }
    return [z, t];
  }

  for (var y = iterYStart; y !== iterYEnd; y += iterYStep) {
    for (var x = iterXStart; x !== iterXEnd; x += iterXStep) {
      var centerX = (x + 0.5) * gs, centerY = (y + 0.5) * gs;
      var ddx = centerX - cam.x, ddy = centerY - cam.y;
      var distSq = ddx * ddx + ddy * ddy;
      if (distSq > viewDistSq) { __caveStats.ceilSkipViewDist++; continue; }
      var fwdDot = ddx * cosAng + ddy * sinAng;
      if (fwdDot < 0) { __caveStats.ceilSkipBehind++; continue; }
      var rgtDot = ddx * (-sinAng) + ddy * cosAng;
      if (Math.abs(rgtDot) / (fwdDot + 0.001) > fovCutTan) { __caveStats.ceilSkipFov++; continue; }

      var idx0 = y * mw + x, idx1 = idx0 + 1, idx2 = idx0 + mw, idx3 = idx2 + 1;
      var lc0 = mesh.layerCount[idx0];

      for (var li = 0; li < lc0; li++) {
        var L0 = layerAtC(idx0, li); if (!L0 || L0[1] !== 2) { __caveStats.ceilSkipLowCeil++; continue; }
        // Role-aware stitch (ceilings only): find the best ceiling layer in
        // each neighbour by height proximity. Tightened from 2.0 to 0.8 —
        // with smoothed-surface ceiling clamping, valid ceilings vary by
        // <=1u/cell; a 2u tol was accepting outlier corners that produced
        // steep tilted quads ("fangs") hanging into the cave.
        var _cz0 = L0[0];
        var _ceilStitch = getFloorStitchTile(mesh, stitchCache, idx0, li, 0, _cz0, true);
        var _ceilStitchOffset = (idx0 & 255) * 3;
        var _cz1 = _ceilStitch.values[_ceilStitchOffset];
        if (_cz1 !== _cz1) { __caveStats.ceilSkipEntrRange++; continue; }
        var _cz2 = _ceilStitch.values[_ceilStitchOffset+1];
        if (_cz2 !== _cz2) { __caveStats.ceilSkipEntrRange++; continue; }
        var _cz3 = _ceilStitch.values[_ceilStitchOffset+2];
        if (_cz3 !== _cz3) { __caveStats.ceilSkipEntrRange++; continue; }
        __caveStats.ceilCollected++;

        var wx1 = x * gs, wy1 = y * gs, wx2 = wx1 + gs, wy2 = wy1 + gs;
        var ceilingVertices = [
          {x:wx1,y:wy1,z:_cz0*25}, {x:wx2,y:wy1,z:_cz1*25},
          {x:wx2,y:wy2,z:_cz3*25}, {x:wx1,y:wy2,z:_cz2*25}
        ];
        // The underside uses the opposite side of the same triangle-plane
        // test, including sloped ceilings viewed obliquely through the mouth.
        var ceilingPoly = getTerrainTriangleEyeSide(ceilingVertices[0], ceilingVertices[1], ceilingVertices[2],
          cam.x, cam.y, cameraZ) <= 0.000001 ?
          projectSceneWorldPolygon([ceilingVertices[0], ceilingVertices[1], ceilingVertices[2]], C) : [];
        var ceilingPolyB = getTerrainTriangleEyeSide(ceilingVertices[0], ceilingVertices[2], ceilingVertices[3],
          cam.x, cam.y, cameraZ) <= 0.000001 ?
          projectSceneWorldPolygon([ceilingVertices[0], ceilingVertices[2], ceilingVertices[3]], C) : [];
        if (ceilingPoly.length < 3 && ceilingPolyB.length < 3) continue;

        // Same world-authored stone as walls and cave floors.
        var material = getCaveMaterialColorAt(centerX, centerY);
        var baseCol = '#' + ('000000' + material.toString(16)).slice(-6);
        // DEBUG_POLY_TYPES: ceilings = PURPLE. Plus YELLOW tint if the
        // ceiling quad is visibly tilted (corner Z delta >= 1u).
        if (DEBUG_POLY_TYPES) {
          var _cDz = Math.max(Math.abs(_cz1 - _cz0), Math.abs(_cz2 - _cz0), Math.abs(_cz3 - _cz0));
          baseCol = _cDz >= 1.0 ? '#ffdc00' : '#b10dc9';
        }
        var ceilAmbient = typeof getCaveRenderLightAt === 'function' ?
          getCaveRenderLightAt(centerX, centerY, true) : ambientLight;
        var ceilPointLight = 0;
        if (_lightGrid) {
          var clgx = Math.floor(centerX / _lightCellSize);
          var clgy = Math.floor(centerY / _lightCellSize);
          if (clgx >= 0 && clgx < _lightGridW && clgy >= 0 && clgy < _lightGridH)
            ceilPointLight = _lightGrid[clgy * _lightGridW + clgx];
        }
        var _cr, _cg, _cb;
        if (DEBUG_POLY_TYPES) {
          var _cp = parseInt(baseCol.slice(1), 16);
          var ceilLight = Math.min(1, ceilAmbient + ceilPointLight);
          _cr = Math.min(255, Math.floor(((_cp >> 16) & 0xff) * ceilLight));
          _cg = Math.min(255, Math.floor(((_cp >> 8) & 0xff) * ceilLight));
          _cb = Math.min(255, Math.floor((_cp & 0xff) * ceilLight));
        } else {
          var _ceilFogFloor = typeof renderCaveFogFloor !== 'undefined' ? renderCaveFogFloor :
            (typeof fogFloor !== 'undefined' ? fogFloor : 0.15);
          var ceilFog = Math.max(_ceilFogFloor, 1 - fwdDot * 0.0008);
          var ceilingLit = shadeCaveSurfaceColor(material, CAVE_SURFACE_CEILING,
            ceilAmbient, ceilPointLight, ceilFog);
          _cr = (ceilingLit >>> 16) & 255; _cg = (ceilingLit >>> 8) & 255; _cb = ceilingLit & 255;
        }
        ctx.fillStyle = rgbQ(_cr, _cg, _cb);

        // Ceiling is solid rock — full opacity. Exterior occlusion comes from
        // the eye-plane test and cap top face, not a global mode flag.
        ctx.globalAlpha = 1.0;
        fillSceneDepthPolygon(ceilingPoly);
        fillSceneDepthPolygon(ceilingPolyB);
        if (DEBUG_CEIL_WIRE) {
          withSceneDepthClip(ceilingPoly, function() {
            ctx.strokeStyle = '#ff00ff';
            ctx.lineWidth = 1;
            traceSceneDepthPolygon(ceilingPoly);
            ctx.stroke();
          });
        }
        __caveStats.ceilRendered++;
      }
    }
  }
  ctx.globalAlpha = 1.0;
  ctx.restore();
}
